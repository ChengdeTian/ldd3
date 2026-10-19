/*
 * main.c -- the bare scull char module
 *
 * Copyright (C) 2001 Alessandro Rubini and Jonathan Corbet
 * Copyright (C) 2001 O'Reilly & Associates
 *
 * The source code in this file can be freely used, adapted,
 * and redistributed in source or binary form, so long as an
 * acknowledgment appears in derived source files.  The citation
 * should list that the code comes from the book "Linux Device
 * Drivers" by Alessandro Rubini and Jonathan Corbet, published
 * by O'Reilly & Associates.   No warranty is attached;
 * we cannot take responsibility for errors or fitness for use.
 *
 */

#include <linux/configfs.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>

#include <linux/kernel.h>	/* printk() */
#include <linux/slab.h>		/* kmalloc() */
#include <linux/fs.h>		/* everything... */
#include <linux/errno.h>	/* error codes */
#include <linux/types.h>	/* size_t */
#include <linux/proc_fs.h>
#include <linux/fcntl.h>	/* O_ACCMODE */

#include <linux/cdev.h>

#include <linux/tty.h> /* 包含current.h、struct tty_struct */
#include <linux/sched.h> /* current->signal->tty、current->comm */
#include <linux/atomic.h>
#include <linux/cred.h> /* current_uid()、kuid_t、uid_eq() */
#include <linux/kdev_t.h> /* MKDEV() */
#include <linux/mutex.h>
#include <linux/wait.h>

#include "scull.h"		/* local definitions */

/*
 * Our parameters which can be set at load time.
 */

static dev_t scull_a_firstdev;

static struct scull_dev scull_s_device;
static atomic_t scull_s_available = ATOMIC_INIT(1);

static int scull_s_open(struct inode *inode, struct file *filp)
{
    struct scull_dev *dev = &scull_s_device;
    if (! atomic_dec_and_test(&scull_s_available)) {  // -1为0，则返回true
        atomic_inc(&scull_s_available); // 回滚为0
        return -EBUSY;
    }

    if ((filp->f_flags & O_ACCMODE) == O_WRONLY)
        scull_trim(dev);

    filp->private_data = dev;
    return 0;
}

static int scull_s_release(struct inode *inode, struct file *filp)
{
    atomic_inc(&scull_s_available); // 恢复为1 
    return 0;
}
// 单open设备
struct file_operations scull_sngl_fops = { 
	.owner =    THIS_MODULE,
	.llseek =   scull_llseek,
	.read =     scull_read,
	.write =    scull_write,
	.unlocked_ioctl = scull_ioctl,
	.open =     scull_s_open,
	.release =  scull_s_release,
};

static struct scull_dev scull_u_device;
static int scull_u_count;	/* initialized to 0 by default */
static kuid_t scull_u_owner;	/* initialized to 0 by default 写法更新 */
static DEFINE_SPINLOCK(scull_u_lock);

static int scull_u_open(struct inode *inode, struct file *filp)
{
	struct scull_dev *dev = &scull_u_device;
	
	spin_lock(&scull_u_lock); // 加锁
	
	if (scull_u_count && !uid_eq(scull_u_owner, current_uid()) &&
			!uid_eq(scull_u_owner, current_euid()) && 
			!capable(CAP_DAC_OVERRIDE)) {
		spin_unlock(&scull_u_lock);
		return -EBUSY;
	}
	if (scull_u_count == 0)
		scull_u_owner = current_uid(); /* 首次打开，记录用户 */
	scull_u_count++;
	
	spin_unlock(&scull_u_lock);

	if ((filp->f_flags & O_ACCMODE) == O_WRONLY) {
		if (down_interruptible(&dev->sem)) {
			spin_lock(&scull_u_lock);
			scull_u_count--;
			spin_unlock(&scull_u_lock);
			return -ERESTARTSYS;
		}
		scull_trim(dev);
		up(&dev->sem);
	}
	
	filp->private_data = dev;
	return 0;
}

static int scull_u_release(struct inode *inode, struct file *filp)
{
	spin_lock(&scull_u_lock);
	scull_u_count--;
	spin_unlock(&scull_u_lock);
	return 0;
}

// 单用户可以重复打开设备
struct file_operations scull_user_fops = {
	.owner =      THIS_MODULE,
	.llseek =     scull_llseek,
	.read =       scull_read,
	.write =      scull_write,
	.unlocked_ioctl = scull_ioctl,
	.open =       scull_u_open,
	.release =    scull_u_release,
};

static struct scull_dev scull_w_device;
static int scull_w_count;	/* initialized to 0 by default */
static kuid_t scull_w_owner;	/* initialized to 0 by default */
static DECLARE_WAIT_QUEUE_HEAD(scull_w_wait);
static DEFINE_SPINLOCK(scull_w_lock);

static inline int scull_w_available(void)
{
	return scull_w_count == 0 ||
		uid_eq(scull_w_owner, current_uid()) ||
		uid_eq(scull_w_owner, current_euid()) ||
		capable(CAP_DAC_OVERRIDE);
}

static int scull_w_open(struct inode *inode, struct file *filp)
{
	struct scull_dev *dev = &scull_w_device;

	spin_lock(&scull_w_lock);
	while (!scull_w_available()) {
		spin_unlock(&scull_w_lock);
		if (filp->f_flags & O_NONBLOCK) // 非阻塞模式，直接返回
			return -EAGAIN;
		// 睡眠等待唤醒并且可用
		if (wait_event_interruptible(scull_w_wait, scull_w_available()))
			return -ERESTARTSYS;
		spin_lock(&scull_w_lock); // 重新加锁
	}
	
	if (scull_w_count == 0)
		scull_w_owner = current_uid();
	scull_w_count++;
	
	spin_unlock(&scull_w_lock);
	
	if ((filp->f_flags & O_ACCMODE) == O_WRONLY) {
		int temp;

		if (down_interruptible(&dev->sem)) {
			spin_lock(&scull_w_lock);
			scull_w_count--;
			temp = scull_w_count;
			spin_unlock(&scull_w_lock);
			if (temp == 0)
				wake_up_interruptible(&scull_w_wait);
			return -ERESTARTSYS;
		}
		scull_trim(dev);
		up(&dev->sem);
	}
	
	filp->private_data = dev;
	return 0;
}

static int scull_w_release(struct inode *inode, struct file *filp)
{
	int temp;

	spin_lock(&scull_w_lock);
	scull_w_count--;
	temp = scull_w_count;
	spin_unlock(&scull_w_lock);

	if (temp == 0)
		wake_up_interruptible_sync(&scull_w_wait); // 设备空了才唤醒等待者
	return 0;
}

// 阻塞open
struct file_operations scull_wusr_fops = {
	.owner =      THIS_MODULE,
	.llseek =     scull_llseek,
	.read =       scull_read,
	.write =      scull_write,
	.unlocked_ioctl = scull_ioctl,
	.open =       scull_w_open,
	.release =    scull_w_release,
};

struct scull_listitem {
	struct scull_dev device;
	dev_t key;
	struct list_head list;
    
};

static LIST_HEAD(scull_c_list);
/*
 * 这里用 mutex 而不是 spinlock：scull_c_lookfor_device() 里会用
 * GFP_KERNEL 分配内存，持自旋锁睡眠是非法操作。
 */
static DEFINE_MUTEX(scull_c_lock);

static struct scull_dev *scull_c_lookfor_device(dev_t key)
{
	struct scull_listitem *lptr;

	mutex_lock(&scull_c_lock);

	list_for_each_entry(lptr, &scull_c_list, list) {
		if (lptr->key == key) {
			mutex_unlock(&scull_c_lock);
			return &lptr->device;
		}
	}

	lptr = kmalloc(sizeof(struct scull_listitem), GFP_KERNEL);
	if (lptr == NULL) {
		mutex_unlock(&scull_c_lock);
		return NULL;
	}
	memset(lptr, 0, sizeof(struct scull_listitem));
	lptr->key = key;
	sema_init(&lptr->device.sem, 1);
	lptr->device.quantum = scull_quantum;
	lptr->device.qset = scull_qset;

	list_add(&lptr->list, &scull_c_list);
	mutex_unlock(&scull_c_lock);
	return &(lptr->device);
}

static int scull_c_open(struct inode *inode, struct file *filp)
{
	struct scull_dev *dev;
	struct tty_struct *tty = current->signal->tty;
	dev_t key;

	if (!tty || !tty->driver) {
		printk(KERN_ERR "Process %s has no ctl tty\n", current->comm);
		return -EINVAL;
	}
	key = MKDEV(tty->driver->major, tty->driver->minor_start + tty->index);
	dev = scull_c_lookfor_device(key);
	if (dev == NULL)
		return -ENOMEM;

	filp->private_data = dev;
	return 0;
}

static int scull_c_release(struct inode *inode, struct file *filp)
{
	return 0;
}

struct file_operations scull_priv_fops = {
	.owner =    THIS_MODULE,
	.llseek =   scull_llseek,
	.read =     scull_read,
	.write =    scull_write,
	.unlocked_ioctl = scull_ioctl,
	.open =     scull_c_open,
	.release =  scull_c_release,
};

/* 克隆设备：所有 tty 共用一个 cdev，具体 dev 在 open 时按 tty 创建 */
static struct cdev scull_c_cdev;

static struct scull_adev_info {
	const char *name;
	struct scull_dev *sculldev;
	const struct file_operations *fops;
} scull_access_devs[] = {
	{ "scullsingle", &scull_s_device, &scull_sngl_fops },
	{ "sculluid", &scull_u_device, &scull_user_fops },
	{ "scullwuid", &scull_w_device, &scull_wusr_fops },
	{ "sullpriv", NULL, &scull_priv_fops }
};
#define SCULL_N_ADEVS ARRAY_SIZE(scull_access_devs)

static unsigned long scull_access_added;	/* 已成功添加的设备位图 */
static bool scull_a_registered;

static int scull_access_setup(dev_t devno, struct scull_adev_info *devinfo)
{
	struct scull_dev *dev = devinfo->sculldev;
	struct cdev *cdev;
	int err;

	if (dev) {
		dev->quantum = scull_quantum;
		dev->qset = scull_qset;
		sema_init(&dev->sem, 1);	/* Linux 6 没有 SEM_INITIALIZER 赋值写法 */
		cdev = &dev->cdev;
	} else {
		cdev = &scull_c_cdev;
	}

	cdev_init(cdev, devinfo->fops);
	cdev->owner = THIS_MODULE;
	kobject_set_name(&cdev->kobj, "%s", devinfo->name);

	err = cdev_add(cdev, devno, 1);
	if (err) {
		printk(KERN_ERR "Error %d adding %s cdev\n", err, devinfo->name);
		return err;
	}
	printk(KERN_NOTICE "%s cdev added\n", devinfo->name);
	return 0;
}

int scull_access_init(dev_t firstdev)
{
	int i, err;

	if (scull_a_registered)
		return -EBUSY;

	err = register_chrdev_region(firstdev, SCULL_N_ADEVS, "sculla");
	if (err < 0) {
		printk(KERN_WARNING "sculla: device number registration failed\n");
		return err;
	}

	scull_a_firstdev = firstdev;
	scull_a_registered = true;

	for (i = 0; i < SCULL_N_ADEVS; i++) {
		err = scull_access_setup(firstdev + i, scull_access_devs + i);
		if (err) {
			scull_access_cleanup();
			return err;
		}
		scull_access_added |= 1UL << i;
	}

	return 0;
}

/*
 * 卸载时统一回收：
 *   1) 删除四个访问控制设备的 cdev，并释放静态设备的缓存；
 *   2) 释放按控制终端克隆出来的 device（这是唯一会持续增长的分配）；
 *   3) 归还设备号区间。
 */
void scull_access_cleanup(void)
{
	struct scull_listitem *lptr, *next;
	unsigned int i;

	for (i = 0; i < SCULL_N_ADEVS; i++) {
		struct scull_dev *dev = scull_access_devs[i].sculldev;

		if (!(scull_access_added & (1UL << i)))
			continue;

		if (dev) {
			cdev_del(&dev->cdev);
			scull_trim(dev);
		} else {
			cdev_del(&scull_c_cdev);
		}
	}
	scull_access_added = 0;

	list_for_each_entry_safe(lptr, next, &scull_c_list, list) {
		list_del(&lptr->list);
		scull_trim(&lptr->device);	/* 先释放 dev->data */
		kfree(lptr);			/* 再释放 listitem */
	}

	if (scull_a_registered) {
		unregister_chrdev_region(scull_a_firstdev, SCULL_N_ADEVS);
		scull_a_registered = false;
	}
}