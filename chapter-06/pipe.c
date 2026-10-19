#include <linux/module.h>
#include <linux/moduleparam.h>

#include <linux/kernel.h>	/* printk(), min() */
#include <linux/slab.h>		/* kmalloc() */
#include <linux/fs.h>		/* everything... */
#include <linux/proc_fs.h>
#include <linux/errno.h>	/* error codes */
#include <linux/types.h>	/* size_t */
#include <linux/fcntl.h>
#include <linux/poll.h>
#include <linux/cdev.h>
#include <linux/uaccess.h>
#include <linux/signal.h>

#include "scull.h"		/* local definitions */

struct scull_pipe {
        wait_queue_head_t inq, outq;       /* read and write queues */
        char *buffer, *end;                /* begin of buf, end of buf */
        int buffersize;                    /* used in pointer arithmetic */
        char *rp, *wp;                     /* where to read, where to write */
        int nreaders, nwriters;            /* number of openings for r/w */
        struct fasync_struct *async_queue; /* asynchronous readers */
        struct semaphore sem;              /* mutual exclusion semaphore */
        struct cdev cdev;                  /* Char device structure */
};

static int scull_p_nr_devs = SCULL_P_NR_DEVS;	/* number of pipe devices */
int scull_p_buffer =  SCULL_P_BUFFER;	/* buffer size */
dev_t scull_p_devno;			/* Our first device number */

/* perm 必须非 0，否则 /sys/module/scull/parameters/ 下不会出现这两个节点 */
module_param(scull_p_nr_devs, int, 0444);
module_param(scull_p_buffer, int, 0444);

static struct scull_pipe *scull_p_devices;

static int scull_p_fasync(int fd, struct file *filp, int mode);
static int spacefree(struct scull_pipe *dev);

static int scull_p_open(struct inode *inode, struct file *filp)
{
    struct scull_pipe *dev;
	dev = container_of(inode->i_cdev, struct scull_pipe, cdev);
	filp->private_data = dev;

    if (down_interruptible(&dev->sem))
		return -ERESTARTSYS;
	if (!dev->buffer) {
		/* allocate the buffer */
		dev->buffer = kmalloc(scull_p_buffer, GFP_KERNEL);
		if (!dev->buffer) {
			up(&dev->sem);
			return -ENOMEM;
		}
		/*
		 * 只在首次分配时初始化指针：buffer 是所有 fd 共享的，
		 * 后续 open 若把 rp/wp 拨回起点，会把未读数据"弄丢"，
		 * 并让另一个 fd 的位置错乱。
		 */
		dev->end = dev->buffer + scull_p_buffer;
		dev->buffersize = scull_p_buffer;
		dev->rp = dev->wp = dev->buffer;
	}

	if (filp->f_mode & FMODE_READ)
		dev->nreaders++;	/* 读打开数加 1 */
	if (filp->f_mode & FMODE_WRITE)
		dev->nwriters++;	/* 写打开数加 1 */

	up(&dev->sem);
	return nonseekable_open(inode, filp);	/* 禁止 lseek/pread/pwrite */
}

static int scull_p_release(struct inode *inode, struct file *filp)
{
    struct scull_pipe *dev = filp->private_data;

    scull_p_fasync(-1, filp, 0);
	
    down(&dev->sem); // 加锁
	if (filp->f_mode & FMODE_READ)
		dev->nreaders--;
	if (filp->f_mode & FMODE_WRITE)
		dev->nwriters--;
    if (dev->nreaders == 0 && dev->nwriters == 0) // 读写打开数为0，释放buffer
    {
		kfree(dev->buffer);
		dev->buffer = NULL;
	}

	/*
	 * 关闭一端时必须唤醒另一端，否则：
	 *   - 写端全关时，睡在 read 里的进程永远等不到 EOF（本该返回 0）；
	 *   - 读端全关时，睡在 write 里的进程永远等不到 EPIPE。
	 */
	if (dev->nwriters == 0)
		wake_up_interruptible(&dev->inq);
	if (dev->nreaders == 0)
		wake_up_interruptible(&dev->outq);

	up(&dev->sem);
	return 0;
}

static ssize_t scull_p_read(struct file *filp, char __user *buf, size_t count, loff_t *f_pos)
{
    struct scull_pipe *dev = filp->private_data;
    
    if (down_interruptible(&dev->sem))
		return -ERESTARTSYS;
	
    while (dev->rp == dev->wp) { // nothing to read
        if (!dev->nwriters) {    // 写端已全部关闭 → EOF
            up(&dev->sem);
            return 0;
        }
        up(&dev->sem);
        if (filp->f_flags & O_NONBLOCK)
			return -EAGAIN;

		printk(KERN_ALERT "scull_p_read: going to sleep\n");
        if (wait_event_interruptible(dev->inq, dev->rp != dev->wp))
            return -ERESTARTSYS; // 等待读事件唤醒进行检查，检查读写指针
        if (down_interruptible(&dev->sem)) // 重新持有锁
            return -ERESTARTSYS;
    }

    if (dev->wp > dev->rp)
        count = min(count, (size_t)(dev->wp - dev->rp));
    else
        count = min(count, (size_t)(dev->end - dev->rp));
    if (copy_to_user(buf, dev->rp, count)) {
		up(&dev->sem);
		return -EFAULT;
	}
	dev->rp += count;
	if (dev->rp == dev->end)
		dev->rp = dev->buffer;
	up(&dev->sem);

    wake_up_interruptible(&dev->outq); // 唤醒写事务
    printk(KERN_ALERT "scull_p_read: read %li bytes, wake up\n", (long)count);
	return count;
}

static int scull_getwritespace(struct scull_pipe *dev, struct file *filp)
{
    while (spacefree(dev) == 0) {
        if (!dev->nreaders) {     // 读端已全部关闭 → 断管
            up(&dev->sem);
            return -EPIPE;
        }
		up(&dev->sem); // 释放锁

        if (filp->f_flags & O_NONBLOCK)
            return -EAGAIN;
		printk(KERN_ALERT "scull_getwritespace: going to sleep\n");
        /*
		 * wait_event_interruptible() 内部完成：
		 *   DEFINE_WAIT + prepare_to_wait() + schedule() + finish_wait()
		 *   - 条件成立时立即返回 0，不睡眠；
		 *   - 被信号打断时返回 -ERESTARTSYS。
		 * 它在挂队之后会重新检查条件，因此不会丢唤醒。
		 */
		if (wait_event_interruptible(dev->outq, spacefree(dev) > 0))
			return -ERESTARTSYS;
		if (down_interruptible(&dev->sem))
			return -ERESTARTSYS;
	}
	return 0;
}

/* How much space is free? */
static int spacefree(struct scull_pipe *dev)
{
	if (dev->rp == dev->wp) // 空的状态
		return dev->buffersize - 1;
	/* 先做指针减法再加 buffersize，避免 rp + size 越过数组末尾 */
	return (int)((dev->rp - dev->wp + dev->buffersize) % dev->buffersize) - 1;
}

static ssize_t scull_p_write(struct file *filp, const char __user *buf, size_t count, loff_t *f_pos)
{
	struct scull_pipe *dev = filp->private_data;
	int result;

	if (down_interruptible(&dev->sem))
		return -ERESTARTSYS;

	/* 没有读端时立即断管（即使缓冲区还有空闲空间） */
	if (!dev->nreaders) {
		up(&dev->sem);
		return -EPIPE;
	}

	result = scull_getwritespace(dev, filp);
	if (result)
		return result;	/* getwritespace 返回前已放锁 */
	
	count = min(count, (size_t)spacefree(dev));
	if (dev->wp >= dev->rp)
		count = min(count, (size_t)(dev->end - dev->wp));
	else // 新一轮数据，未读数据再rp后面
		count = min(count, (size_t)(dev->rp - dev->wp - 1));
	printk(KERN_ALERT "scull_p_write: going to accept %li bytes to %p from %p\n", (long)count, dev->wp, buf);
	if (copy_from_user(dev->wp, buf, count)) {
		up(&dev->sem);
		return -EFAULT;
	}
	dev->wp += count;
	if (dev->wp == dev->end)
		dev->wp = dev->buffer;
	up(&dev->sem);
	
	wake_up_interruptible(&dev->inq); // 唤醒读事务
	if (dev->async_queue)
		kill_fasync(&dev->async_queue, SIGIO, POLL_IN); // 遍历链表，发送SIGIO信号
	printk(KERN_ALERT "scull_p_write: write %li bytes, wake up\n", (long)count);
	return count;
}

static unsigned int scull_p_poll(struct file *filp, poll_table *wait)
{
	struct scull_pipe *dev = filp->private_data;
	unsigned int mask = 0;
	
	down(&dev->sem);
	poll_wait(filp, &dev->inq, wait); // 挂载到读队列
	poll_wait(filp, &dev->outq, wait); // 挂载到写队列
	
	
	if (dev->rp != dev->wp)
		mask |= POLLIN | POLLRDNORM;
	else if (!dev->nwriters)
		mask |= POLLHUP;	/* 写端全关，读端会看到 EOF */

	if (!dev->nreaders)
		mask |= POLLERR;	/* 读端全关，写端会得到 EPIPE */
	else if (spacefree(dev) > 0)
		mask |= POLLOUT | POLLWRNORM;

	up(&dev->sem);
	return mask;
}

static int scull_p_fasync(int fd, struct file *filp, int mode)
{
	struct scull_pipe *dev = filp->private_data;

	// kill_fasync 负责通知
	return fasync_helper(fd, filp, mode, &dev->async_queue); // 将filp注册、注销到dev->async_queue链表中
}

struct file_operations scull_p_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = scull_ioctl,
	/* 不提供 llseek：nonseekable_open() 已清掉 FMODE_LSEEK，
	 * lseek() 会直接返回 -ESPIPE。Linux 6 已删除 no_llseek。 */
	.read = scull_p_read,
	.write = scull_p_write,
	.poll = scull_p_poll,
	.open = scull_p_open,
	.release = scull_p_release,
	.fasync = scull_p_fasync,
};

static int scull_p_setup_cdev(struct scull_pipe *dev, int index)
{
	int err;
	dev_t devno = scull_p_devno + index;

	cdev_init(&dev->cdev, &scull_p_fops);
	dev->cdev.owner = THIS_MODULE;
	dev->cdev.ops = &scull_p_fops;
	err = cdev_add(&dev->cdev, devno, 1);
	if (err)
		printk(KERN_NOTICE "Error %d adding scull_p cdev %d\n", err, index);
	return err;
}

int scull_p_init(dev_t firstdev)
{
	int i, result;

	if (scull_p_nr_devs <= 0 || scull_p_buffer <= 1) {
		printk(KERN_ERR "scull_p: invalid parameters (devs=%d, buffer=%d)\n",
		       scull_p_nr_devs, scull_p_buffer);
		return -EINVAL;
	}

	result = register_chrdev_region(firstdev, scull_p_nr_devs, "scull_p");
	if (result < 0) {
		printk(KERN_NOTICE "scull_p: can't get device numbers at %u\n",
		       (unsigned)firstdev);
		return result;
	}
	scull_p_devno = firstdev;

	scull_p_devices = kcalloc(scull_p_nr_devs, sizeof(struct scull_pipe),
				  GFP_KERNEL);
	if (!scull_p_devices) {
		result = -ENOMEM;
		goto fail_region;
	}

	for (i = 0; i < scull_p_nr_devs; i++) {
		init_waitqueue_head(&scull_p_devices[i].inq);
		init_waitqueue_head(&scull_p_devices[i].outq);
		sema_init(&scull_p_devices[i].sem, 1);
		result = scull_p_setup_cdev(scull_p_devices + i, i);
		if (result)
			goto fail_cdev;
	}

	return scull_p_nr_devs;

fail_cdev:
	while (--i >= 0)
		cdev_del(&scull_p_devices[i].cdev);
	kfree(scull_p_devices);
	scull_p_devices = NULL;
fail_region:
	unregister_chrdev_region(firstdev, scull_p_nr_devs);
	return result;
}

void scull_p_cleanup(void)
{
	int i;

	if (!scull_p_devices)
		return;

	for (i = 0; i < scull_p_nr_devs; i++) {
		cdev_del(&scull_p_devices[i].cdev);
		kfree(scull_p_devices[i].buffer);
		scull_p_devices[i].buffer = NULL;
	}
	kfree(scull_p_devices);
	scull_p_devices = NULL;

	unregister_chrdev_region(scull_p_devno, scull_p_nr_devs);
}