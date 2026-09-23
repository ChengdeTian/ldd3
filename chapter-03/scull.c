#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>
#include <linux/kernel.h>	/* printk() */
#include <linux/slab.h>		/* kmalloc() */
#include <linux/fs.h>		/* everything... */
#include <linux/errno.h>	/* error codes */
#include <linux/types.h>	/* size_t */
#include <linux/fcntl.h>	/* O_ACCMODE */
#include <asm/uaccess.h>
#include "scull.h"		/* local definitions */

int scull_major =   SCULL_MAJOR;
int scull_minor =   0;
int scull_devs =    SCULL_DEVS;	/* number of bare scull devices */
int scull_qset =    SCULL_QSET;
int scull_quantum = SCULL_QUANTUM;

module_param(scull_major, int, 0);
module_param(scull_minor, int, 0);
module_param(scull_devs, int, 0);
module_param(scull_qset, int, 0);
module_param(scull_quantum, int, 0);
MODULE_AUTHOR("Tcd");
MODULE_LICENSE("Dual BSD/GPL");

struct scull_dev *scull_devices = NULL;

/**
 * @brief 获取第n个qset
 *
 * @param dev 设备
 * @param n 第n个qset
 * @return struct scull_qset* 第n个qset
 *
 * 如果第n个qset不存在，则创建第n个qset
 * 如果第n个qset存在，则返回第n个qset
 */
static struct scull_qset *scull_follow(struct scull_dev *dev, int n)
{
    struct scull_qset *qs = dev->data;
    if (!qs) {
        qs = kmalloc(sizeof(struct scull_qset), GFP_KERNEL);
        memset(qs, 0, sizeof(struct scull_qset));
        dev->data = qs;
    }
    while (n--) {
        if (!qs->next) {
            qs->next = kmalloc(sizeof(struct scull_qset), GFP_KERNEL);
            if (!qs->next) {
                printk(KERN_ALERT "scull_follow: kmalloc failed\n");
                return NULL;
            }
            memset(qs->next, 0, sizeof(struct scull_qset));
        }
        qs = qs->next;
    }
    return qs;
}
static int scull_trim(struct scull_dev *dev)
{
    struct scull_qset *dptr, *next;
    int qset_set = dev->qset;
    int i;
    
    for (dptr = dev->data; dptr; dptr = next) { // 链表+数组的结构
        if (dptr->data) {                       // data 可能为 NULL，必须先判空
            for (i = 0; i < qset_set; i++)
                if (dptr->data[i])
                    kfree(dptr->data[i]);
            kfree(dptr->data);
            dptr->data = NULL;
        }
        next = dptr->next; // 先保存后继，再释放当前节点
        kfree(dptr);
    }
    dev->size = 0;
    dev->quantum = scull_quantum;
    dev->qset = scull_qset;
    dev->data = NULL;     // 注意：清的是 data 链表头，不是 next
    
    return 0;
}

static ssize_t scull_read(struct file *filp, char __user *buf, size_t count, loff_t *f_pos)
{
    struct scull_dev *dev = filp->private_data;
    struct scull_qset *qs;
    int quantum = dev->quantum, qset = dev->qset;
    int itemsize = quantum * qset;
    int item, s_pos, q_pos, rest;
    ssize_t retval = 0;

    if (down_interruptible(&dev->sem))
        return -ERESTARTSYS;
    if (*f_pos >= dev->size)
        goto out;
    if (*f_pos + count > dev->size)
        count = dev->size - *f_pos;
    
    item = *f_pos / itemsize; // 第几个qset
    rest = *f_pos % itemsize;
    
    s_pos = rest / quantum;
    q_pos = rest % quantum;
    
    qs = scull_follow(dev, item);
    if (qs == NULL || !qs->data || !qs->data[s_pos])
        goto out;
    
    if (count > quantum - q_pos)
        count = quantum - q_pos;
    
    if (copy_to_user(buf, qs->data[s_pos] + q_pos, count)) {
        retval = -EFAULT;
        goto out;
    }
    *f_pos += count;
    retval = count;

out:
    up(&dev->sem);
    return retval;
}

static ssize_t scull_write(struct file *filp, const char __user *buf, size_t count, loff_t *f_pos)
{
    struct scull_dev *dev = filp->private_data;
    struct scull_qset *dptr;
    int quantum = dev->quantum, qset = dev->qset;
    int itemsize = quantum * qset;
    int item, s_pos, q_pos, rest;
    ssize_t retval = -ENOMEM;

    if (down_interruptible(&dev->sem))
        return -ERESTARTSYS;
    
    item = *f_pos / itemsize; // 第几个device
    rest = *f_pos % itemsize;
    
    s_pos = rest / quantum;
    q_pos = rest % quantum;
    
    dptr = scull_follow(dev, item);
    if (dptr == NULL)
        goto out;
    
    if (!dptr->data) {
        dptr->data = kmalloc(qset * sizeof(char *), GFP_KERNEL); // qset存放的地址
        if (!dptr->data)
            goto out;
        memset(dptr->data, 0, qset * sizeof(char *));
    }
    if (!dptr->data[s_pos]) {
        dptr->data[s_pos] = kmalloc(quantum, GFP_KERNEL);
        if (!dptr->data[s_pos])
            goto out;
    }

    if (count > quantum - q_pos)
        count = quantum - q_pos;
    
    if (copy_from_user(dptr->data[s_pos] + q_pos, buf, count)) {
        retval = -EFAULT;
        goto out;
    }
    *f_pos += count;
    retval = count;

    if (dev->size < *f_pos)
    	dev->size = *f_pos;

out:
    up(&dev->sem);
    return retval;
}


static int scull_open(struct inode *inode, struct file *filp)
{
    struct scull_dev *dev;
    dev = container_of(inode->i_cdev, struct scull_dev, cdev);
    filp->private_data = dev;

    if ((filp->f_flags & O_ACCMODE) == O_WRONLY) {
        if (down_interruptible(&dev->sem))   // trim 会改共享数据，必须持锁
            return -ERESTARTSYS;
        scull_trim(dev); // 清空缓存
        up(&dev->sem);
    }
    printk(KERN_ALERT "scull_open\n");
    return 0;
}

static int scull_release(struct inode *inode, struct file *filp)
{
    printk(KERN_ALERT "scull_release\n");
    return 0;
}

/**
 * @brief 定位读写指针
 *
 * 注意：file_operations.llseek 为 NULL 时，内核会清掉 FMODE_LSEEK，
 *       用户态 lseek() 直接返回 -ESPIPE，定位读写就完全不可用。
 */
static loff_t scull_llseek(struct file *filp, loff_t off, int whence)
{
    struct scull_dev *dev = filp->private_data;
    loff_t newpos;

    switch (whence) {
    case SEEK_SET: /* 0 */
        newpos = off;
        break;
    case SEEK_CUR: /* 1 */
        newpos = filp->f_pos + off;
        break;
    case SEEK_END: /* 2 */
        newpos = dev->size + off;
        break;
    default:
        return -EINVAL;
    }
    if (newpos < 0)
        return -EINVAL;
    filp->f_pos = newpos;
    return newpos;
}

static struct file_operations scull_ops = {
	.owner   = THIS_MODULE,
	.llseek  = scull_llseek,
	.open    = scull_open,
	.release = scull_release,
    .read    = scull_read,
    .write   = scull_write,
    
};

static int scull_setup_cdev(struct scull_dev *dev, int index)
{
    int err, devno = MKDEV(scull_major, scull_minor + index);

    cdev_init(&dev->cdev, &scull_ops);
    dev->cdev.owner = THIS_MODULE;
    dev->cdev.ops = &scull_ops;
    err = cdev_add(&dev->cdev, devno, 1);
    if (err) {
        printk(KERN_ALERT "Error %d adding scull%d", err, index);
    }
    return err;
}

static int __init scull_init(void)
{
    int result, i;
    dev_t dev = 0;

    if (scull_major) {
        dev= MKDEV(scull_major, scull_minor);
        result = register_chrdev_region(dev, scull_devs, "scull");
    } else {
        result = alloc_chrdev_region(&dev, scull_minor, scull_devs, "scull");
        scull_major = MAJOR(dev);
    }
    if (result < 0) {
        printk(KERN_ALERT "scull: can't get major %d\n", scull_major);
        return result;
    }
    scull_devices = kmalloc(scull_devs * sizeof(struct scull_dev), GFP_KERNEL);
    if (!scull_devices) {
        result = -ENOMEM;
        goto fail_malloc;
    }
    memset(scull_devices, 0, scull_devs*sizeof (struct scull_dev));

    for (i = 0; i < scull_devs; i++) {
        scull_devices[i].quantum = scull_quantum;
        scull_devices[i].qset = scull_qset;
        sema_init(&scull_devices[i].sem, 1);
        scull_setup_cdev(scull_devices + i, i);
    }

    printk(KERN_ALERT "Hello, world!\n");
    return 0;
fail_malloc:
    unregister_chrdev_region(dev, scull_devs);
    return result;
}

static void __exit scull_exit(void)
{
    int i;
    dev_t devno = MKDEV(scull_major, scull_minor);
    
    if (scull_devices) {
        for (i = 0; i < scull_devs; i++) {
            cdev_del(&scull_devices[i].cdev);
            scull_trim(scull_devices + i);
        }
        kfree(scull_devices);
    }
    
    unregister_chrdev_region(devno, scull_devs);
    
    printk(KERN_ALERT "Goodbye, world!\n");
}

module_init(scull_init);
module_exit(scull_exit);