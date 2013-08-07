#include <linux/init.h>
#include <linux/module.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/interrupt.h>
#include <linux/err.h>
#include <linux/slab.h>
#include <linux/kernel.h>

#include <asm/gpio.h>
#include <plat/gpio-cfg.h>

#define ALLOW_OPEN_TIMES	1

static int major = 0;
static int minor = 0;
static struct class *cls;

struct button_resource {
	char *name;
	unsigned long gpio;
	unsigned long irq;
};

struct button_dev {
	struct device *dev;
	struct cdev cdev;
	atomic_t av;
	spinlock_t lock;
	struct mutex mlock;
	int open_times;
	struct semaphore sema;
	
	wait_queue_head_t wq;

};

static struct button_resource btn_info[] = {
	[0] = {
		.name = "KEY_UP",
		.gpio = S5PV210_GPH0(0),
		.irq  = IRQ_EINT(0),
	},
	[1] = {
		.name = "KEY_DOWN",
		.gpio = S5PV210_GPH0(1),
		.irq  = IRQ_EINT(1),
	},
};

static struct button_dev *pbtn = NULL;
unsigned char key_val;
int is_press;

static irqreturn_t button_isr(int irq, void *dev_id)
{
	struct button_resource *pindesc = (struct button_resource *)dev_id;
	unsigned int pinval;

	pinval = gpio_get_value(pindesc->gpio);
	if (pinval == 0) {
		key_val = 0x80;
	} else if(pinval == 1) {
		key_val = 0x81;
	}

	wake_up_interruptible(&(pbtn->wq));
	is_press = 1;
	return IRQ_HANDLED;
}

static int button_open(struct inode *inode, struct file *file)
{
	struct button_dev *pbtnp = NULL;

	pbtnp = container_of(inode->i_cdev, struct button_dev, cdev);
	file->private_data = pbtnp;

	if (file->f_flags & O_NONBLOCK) {
		if(down_trylock(&(pbtnp->sema))) {
				printk("%s : button device can be open two times\n", 
														__FUNCTION__);
				return -EBUSY;
		}
	} else {
		down(&(pbtnp->sema));
	}
	printk("%s: open button device successfule!\n", __FUNCTION__);

	return 0;
}

static int button_release(struct inode *inode, struct file *file)
{
	struct button_dev *pbtnp = file->private_data;

	up(&(pbtnp->sema));

	return 0;
}

static ssize_t button_read(struct file *file, char __user *buffer, 
								size_t count, loff_t *ppos)
{
	struct button_dev *pbtnp = file->private_data;

	if (file->f_flags & O_NONBLOCK) {
		if(!is_press) {
			return -EAGAIN;
		}	
	}
	
	wait_event_interruptible(pbtnp->wq, is_press != 0);
	is_press = 0;
	if (copy_to_user(buffer, &key_val, sizeof(key_val))) {
		return -EFAULT;
	}
	return count;
}

static struct file_operations button_fops = {
	.owner = THIS_MODULE,
	.open  = button_open,
	.release = button_release,
	.read = button_read,
};
static int button_init(void)
{
	int retval;
	dev_t dev_id;
	int i, j;
	
	if(major) {
		dev_id = MKDEV(major, minor);
		retval = register_chrdev_region(dev_id, 1, "button");
	} else {
		retval = alloc_chrdev_region(&dev_id, 0, 1, "button");
		major = MAJOR(dev_id);
	}
	if (retval < 0) {
		printk("register button device failed.\n");
		goto failure_register_device;
	}

	pbtn = kmalloc(sizeof(struct button_dev), GFP_KERNEL);
	if (IS_ERR(pbtn)) {
		printk("no enough memory for private struct\n");
		retval = PTR_ERR(pbtn);
		goto failure_alloc_dev;
	}
	memset(pbtn, 0, sizeof(struct button_dev));
	
	atomic_inc(&(pbtn->av));
	spin_lock_init(&(pbtn->lock));
	pbtn->open_times = 0;

	sema_init(&(pbtn->sema), 2);
	mutex_init(&(pbtn->mlock));

	init_waitqueue_head(&(pbtn->wq));
	
	cls = class_create(THIS_MODULE, "button");
	if (IS_ERR(cls)) {
		printk("create class failed.\n");
		retval = PTR_ERR(cls);
		goto failure_create_class;
	}

	cdev_init(&(pbtn->cdev), &button_fops);
	retval = cdev_add(&(pbtn->cdev), dev_id, 1);
	if (retval < 0) {
		printk("add button cdev failed.\n");
		goto failure_cdev_add;
	}

	pbtn->dev = device_create(cls, NULL, dev_id, NULL, "button");
	if(IS_ERR(pbtn->dev)) {
		printk("button device info create failed.\n");
		retval = PTR_ERR(pbtn->dev);
		goto failure_create_device;
	}

	for(i = 0; i < ARRAY_SIZE(btn_info); i++) {
		retval = gpio_request(btn_info[i].gpio, btn_info[i].name);
		if(retval) {
			printk("request gpio failed.\n");
			goto failure_request_gpio;
		}
		retval = request_irq(btn_info[i].irq, button_isr, 
							IRQF_SAMPLE_RANDOM | IRQF_TRIGGER_FALLING |
							IRQF_TRIGGER_RISING, btn_info[i].name, &btn_info[i]);
		if (retval) {
			printk("request irq failed.\n");
			goto failure_request_irq;
		}
	}

	return 0;
failure_request_irq:
failure_request_gpio:
	for (j = i; j >= 0; j--) {
		free_irq(btn_info[j].irq, NULL);
		gpio_free(btn_info[j].gpio);
	}
	device_destroy(cls, dev_id);
failure_create_device:
	cdev_del(&(pbtn->cdev));
failure_cdev_add:
	class_destroy(cls);
failure_create_class:
	atomic_dec(&(pbtn->av));
failure_alloc_dev:
	unregister_chrdev_region(dev_id, 1);
failure_register_device:
	return retval;
}

static void button_exit(void)
{
	int i;
	dev_t dev_id;

	dev_id = MKDEV(major, minor);
	for(i = 0; i < ARRAY_SIZE(btn_info); i++) {
		free_irq(btn_info[i].irq, &btn_info[i]);
		gpio_free(btn_info[i].gpio);
	}

	device_destroy(cls, dev_id);
	cdev_del(&(pbtn->cdev));
	class_destroy(cls);
	atomic_dec(&(pbtn->av));
	unregister_chrdev_region(dev_id, 1);
}

module_init(button_init);
module_exit(button_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("tarena");
MODULE_DESCRIPTION("BUTTON DRIVERS");
MODULE_VERSION("3.1");
