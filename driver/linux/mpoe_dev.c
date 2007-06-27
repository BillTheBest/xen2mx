#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/miscdevice.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/vmalloc.h>
#include <linux/mm.h>
#include <asm/uaccess.h>

#include "mpoe_hal.h"
#include "mpoe_io.h"
#include "mpoe_common.h"

/**********
 * ioctl commands
 */

static int
mpoe_open_endpoint(void __user * p,
		   struct mpoe_endpoint ** endpointp)
{
	struct mpoe_cmd_open_endpoint param;
	struct mpoe_endpoint * endpoint;
	union mpoe_evt * evt;
	char * buffer;
	int ret;

	ret = copy_from_user(&param, p, sizeof(param));
	if (ret < 0) {
		printk(KERN_ERR "MPoE: Failed to read open endpoint command argument, error %d\n", ret);
		ret = -EFAULT;
		goto out;
	}

	endpoint = kmalloc(sizeof(*endpoint), GFP_KERNEL);
	if (!endpoint) {
		printk(KERN_ERR "MPoE: Failed to allocate memory for endpoint\n");
		goto out;
	}

	ret = mpoe_net_attach_endpoint(endpoint, param.board_index, param.endpoint_index);
	if (ret < 0)
		goto out_with_endpoint;

	buffer = mpoe_vmalloc_user(MPOE_SENDQ_SIZE + MPOE_RECVQ_SIZE + MPOE_EVENTQ_SIZE);
	if (!buffer) {
		printk(KERN_ERR "MPoE: failed to allocate queues\n");
		ret = -ENOMEM;
		goto out_with_attach;
	}
	endpoint->sendq = buffer;
	endpoint->recvq = buffer + MPOE_SENDQ_SIZE;
	endpoint->eventq = buffer + MPOE_SENDQ_SIZE + MPOE_RECVQ_SIZE;

	/* initialize eventq */
	for(evt = endpoint->eventq;
	    (void *) evt < endpoint->eventq + MPOE_EVENTQ_SIZE;
	    evt++)
		evt->generic.type = MPOE_EVT_NONE;
	endpoint->next_eventq_slot = endpoint->eventq;
	endpoint->next_recvq_slot = endpoint->recvq;

	/* initialize user regions */
	mpoe_init_endpoint_user_regions(endpoint);

	*endpointp = endpoint;
	printk(KERN_INFO "MPoE: Successfully open board %d endpoint %d\n",
	       endpoint->board_index, endpoint->endpoint_index);

	return 0;

 out_with_attach:
	mpoe_net_detach_endpoint(endpoint);
 out_with_endpoint:
	kfree(endpoint);
 out:
	return ret;
}

int
mpoe_close_endpoint(struct mpoe_endpoint * endpoint, void __user * dummy)
{
	vfree(endpoint->sendq); /* recvq and eventq are in the same buffer */

	mpoe_deregister_endpoint_user_regions(endpoint);
	mpoe_net_detach_endpoint(endpoint);

	printk(KERN_INFO "MPoE: Successfully closed board %d endpoint %d\n",
	       endpoint->board_index, endpoint->endpoint_index);

	endpoint->file->private_data = NULL;
	kfree(endpoint);

	return 0;
}

/***************
 * File operations
 */

static int
mpoe_miscdev_open(struct inode * inode, struct file * file)
{
	file->private_data = NULL;
	return 0;
}

static int
mpoe_miscdev_release(struct inode * inode, struct file * file)
{
	struct mpoe_endpoint * endpoint = file->private_data;

	if (endpoint != NULL) {
		printk(KERN_INFO "MPoE: Forcing close of board %d endpoint %d\n",
		       endpoint->board_index, endpoint->endpoint_index);
		mpoe_close_endpoint(endpoint, NULL);
		file->private_data = NULL;
	}
	return 0;
}

static int (*mpoe_cmd_with_endpoint_handlers[])(struct mpoe_endpoint * endpoint, void __user * uparam) = {
	[MPOE_CMD_CLOSE_ENDPOINT]	= mpoe_close_endpoint,
	[MPOE_CMD_SEND_TINY]		= mpoe_net_send_tiny,
	[MPOE_CMD_SEND_MEDIUM]		= mpoe_net_send_medium,
	[MPOE_CMD_SEND_RENDEZ_VOUS]	= mpoe_net_send_rendez_vous,
	[MPOE_CMD_SEND_PULL]		= mpoe_net_send_pull,
	[MPOE_CMD_REGISTER_REGION]	= mpoe_register_user_region,
	[MPOE_CMD_DEREGISTER_REGION]	= mpoe_deregister_user_region,
};

static int
mpoe_miscdev_ioctl(struct inode *inode, struct file *file,
		   unsigned int cmd, unsigned long arg)
{
	int ret = 0;

	switch (cmd) {

	case MPOE_CMD_GET_BOARD_COUNT: {
		uint32_t count = mpoe_net_get_iface_count();

		ret = copy_to_user((void __user *) arg, &count,
				   sizeof(count));
		if (ret < 0) {
			printk(KERN_ERR "MPoE: Failed to write get_board_count command result, error %d\n", ret);
			goto out;
		}

		break;
	}

	case MPOE_CMD_GET_BOARD_ID: {
		struct mpoe_cmd_get_board_id get_board_id;

		ret = copy_from_user(&get_board_id, (void __user *) arg,
				     sizeof(get_board_id));
		if (ret < 0) {
			printk(KERN_ERR "MPoE: Failed to read get_board_id command argument, error %d\n", ret);
			goto out;
		}

		ret = mpoe_net_get_iface_id(get_board_id.board_index,
					    &get_board_id.board_addr,
					    get_board_id.board_name);
		if (ret < 0)
			goto out;

		ret = copy_to_user((void __user *) arg, &get_board_id,
				   sizeof(get_board_id));
		if (ret < 0) {
			printk(KERN_ERR "MPoE: Failed to write get_board_id command result, error %d\n", ret);
			goto out;
		}

		break;
	}

	case MPOE_CMD_OPEN_ENDPOINT: {
		struct mpoe_endpoint * endpoint = NULL; /* gcc's boring */

		if (file->private_data != NULL) {
			ret = -EBUSY;
			goto out;
		}

		ret = mpoe_open_endpoint((void __user *) arg, &endpoint);
		if (ret)
			goto out;

		file->private_data = endpoint;
		endpoint->file = file;
		break;
	}

	case MPOE_CMD_CLOSE_ENDPOINT:
	case MPOE_CMD_SEND_TINY:
	case MPOE_CMD_SEND_MEDIUM:
	case MPOE_CMD_SEND_RENDEZ_VOUS:
	case MPOE_CMD_SEND_PULL:
	case MPOE_CMD_REGISTER_REGION:
	case MPOE_CMD_DEREGISTER_REGION:
	{
		struct mpoe_endpoint * endpoint = file->private_data;

		if (unlikely(endpoint == NULL)) {
			printk(KERN_ERR "MPoE: Cannot process command '%s' without any endpoint open\n",
			       mpoe_strcmd(cmd));
			ret = -EINVAL;
			goto out;
		}

		BUG_ON(cmd >= ARRAY_SIZE(mpoe_cmd_with_endpoint_handlers));
		BUG_ON(mpoe_cmd_with_endpoint_handlers[cmd] == NULL);

		ret = mpoe_cmd_with_endpoint_handlers[cmd](endpoint, (void __user *) arg);
		break;
	}

	default:
		ret = -ENOSYS;
		break;
	}
 out:
	return ret;
}

static int
mpoe_miscdev_mmap(struct file * file, struct vm_area_struct * vma)
{
	struct mpoe_endpoint * endpoint = file->private_data;
	unsigned long offset = vma->vm_pgoff << PAGE_SHIFT;
	unsigned long size = vma->vm_end - vma->vm_start;

	if (endpoint == NULL)
		return -EINVAL;

	if (offset == MPOE_SENDQ_OFFSET && size == MPOE_SENDQ_SIZE)
		return mpoe_remap_vmalloc_range(vma, endpoint->sendq, 0);
	else if (offset == MPOE_RECVQ_OFFSET && size == MPOE_RECVQ_SIZE)
		return mpoe_remap_vmalloc_range(vma, endpoint->sendq, MPOE_SENDQ_SIZE >> PAGE_SHIFT);
	else if (offset == MPOE_EVENTQ_OFFSET && size == MPOE_EVENTQ_SIZE)
		return mpoe_remap_vmalloc_range(vma, endpoint->sendq, (MPOE_SENDQ_SIZE + MPOE_RECVQ_SIZE) >> PAGE_SHIFT);
	else {
		printk(KERN_ERR "MPoE: Cannot mmap %lx at %lx\n", size, offset);
		return -EINVAL;
	}
}

static struct file_operations
mpoe_miscdev_fops = {
	.owner = THIS_MODULE,
	.open = mpoe_miscdev_open,
	.release = mpoe_miscdev_release,
	.mmap = mpoe_miscdev_mmap,
	.ioctl = mpoe_miscdev_ioctl,
};

static struct miscdevice
mpoe_miscdev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "mpoe",
	.fops = &mpoe_miscdev_fops,
};

/*************
 * Device attributes
 */

#ifdef MPOE_MISCDEV_HAVE_CLASS_DEVICE

static ssize_t
mpoe_ifaces_attr_show(struct class_device *dev, char *buf)
{
	return mpoe_net_ifaces_show(buf);
}

static ssize_t
mpoe_ifaces_attr_store(struct class_device *dev, const char *buf, size_t size)
{
	return mpoe_net_ifaces_store(buf, size);
}

static CLASS_DEVICE_ATTR(ifaces, S_IRUGO|S_IWUSR, mpoe_ifaces_attr_show, mpoe_ifaces_attr_store);

static int
mpoe_init_attributes(void)
{
	return class_device_create_file(mpoe_miscdev.class, &class_device_attr_ifaces);
}

static void
mpoe_exit_attributes(void)
{
	class_device_remove_file(mpoe_miscdev.class, &class_device_attr_ifaces);
}

#else /* !MPOE_MISCDEV_HAVE_CLASS_DEVICE */

static ssize_t
mpoe_ifaces_attr_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return mpoe_net_ifaces_show(buf);
}

static ssize_t
mpoe_ifaces_attr_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t size)
{
	return mpoe_net_ifaces_store(buf, size);
}

static DEVICE_ATTR(ifaces, S_IRUGO|S_IWUSR, mpoe_ifaces_attr_show, mpoe_ifaces_attr_store);

static int
mpoe_init_attributes(void)
{
	return device_create_file(mpoe_miscdev.this_device, &dev_attr_ifaces);
}

static void
mpoe_exit_attributes(void)
{
	device_remove_file(mpoe_miscdev.this_device, &dev_attr_ifaces);
}

#endif /* !MPOE_MISCDEV_HAVE_CLASS_DEVICE */

/*************
 * Device registration
 */

int
mpoe_dev_init(void)
{
	int ret;

	ret = misc_register(&mpoe_miscdev);
	if (ret < 0) {
		printk(KERN_ERR "MPoE: Failed to register misc device, error %d\n", ret);
		goto out;
	}

	ret = mpoe_init_attributes();
	if (ret < 0) {
		printk(KERN_ERR "MPoE: failed to create misc device attributes, error %d\n", ret);
		goto out_with_device;
	}

	return 0;

 out_with_device:
	misc_deregister(&mpoe_miscdev);
 out:
	return ret;
}

void
mpoe_dev_exit(void)
{
	mpoe_exit_attributes();
	misc_deregister(&mpoe_miscdev);
}

/*
 * Local variables:
 *  tab-width: 8
 *  c-basic-offset: 8
 *  c-indent-level: 8
 * End:
 */
