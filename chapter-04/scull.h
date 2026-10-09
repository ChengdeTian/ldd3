
#include <linux/cdev.h>
#define SCULL_MAJOR 0   /* dynamic major by default */

#define SCULL_DEVS 4    /* scull0 through scull3 */

#define SCULL_QUANTUM  4000 /* use a quantum size like scull */
#define SCULL_QSET     500



struct scull_qset {
	void **data;
	struct scull_qset *next;
};

struct scull_dev {
	struct scull_qset *data; /* 指向scull_qset的指针数组*/
	struct scull_dev *next;  /* next listitem */
	int vmas;                 /* active mappings */
	int quantum;              /* the current allocation size */
	int qset;                 /* the current array size */
	size_t size;              /* 32-bit will suffice */
	struct semaphore sem;     /* Mutual exclusion */
	struct cdev cdev;
};

extern struct scull_dev *scull_devices;

extern struct file_operations scull_fops;

/*
 * The different configurable parameters
 */
extern int scull_major;     /* main.c */
extern int scull_devs;
extern int scull_order;
extern int scull_qset;





