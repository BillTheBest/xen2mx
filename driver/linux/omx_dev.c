/*
 * Open-MX
 * Copyright © INRIA 2007 (see AUTHORS file)
 *
 * The development of this software has been funded by Myricom, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * See the GNU General Public License in COPYING.GPL for more details.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/miscdevice.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/vmalloc.h>
#include <linux/mm.h>
#include <asm/uaccess.h>

#include "omx_hal.h"
#include "omx_io.h"
#include "omx_common.h"

/******************************
 * Alloc/Release internal endpoint fields once everything is setup/locked
 */

static int
omx_endpoint_alloc_resources(struct omx_endpoint * endpoint)
{
	union omx_evt * evt;
	char * buffer;
	int ret;

	/* alloc and init user queues */
	ret = -ENOMEM;
	buffer = omx_vmalloc_user(OMX_SENDQ_SIZE + OMX_RECVQ_SIZE + OMX_EVENTQ_SIZE);
	if (!buffer) {
		printk(KERN_ERR "Open-MX: failed to allocate queues\n");
		goto out;
	}
	endpoint->sendq = buffer;
	endpoint->recvq = buffer + OMX_SENDQ_SIZE;
	endpoint->eventq = buffer + OMX_SENDQ_SIZE + OMX_RECVQ_SIZE;

	for(evt = endpoint->eventq;
	    (void *) evt < endpoint->eventq + OMX_EVENTQ_SIZE;
	    evt++)
		evt->generic.type = OMX_EVT_NONE;
	endpoint->next_eventq_slot = endpoint->eventq;
	endpoint->next_recvq_slot = endpoint->recvq;

	/* initialize user regions */
	omx_endpoint_user_regions_init(endpoint);

	/* initialize pull handles */
	omx_endpoint_pull_handles_init(endpoint);

	return 0;

 out:
	return ret;
}

static void
omx_endpoint_free_resources(struct omx_endpoint * endpoint)
{
	omx_endpoint_pull_handles_exit(endpoint);
	omx_endpoint_user_regions_exit(endpoint);
	vfree(endpoint->sendq); /* recvq and eventq are in the same buffer */
}

/******************************
 * Opening/Closing endpoint main routines
 */

static int
omx_endpoint_open(struct omx_endpoint * endpoint, void __user * uparam)
{
	struct omx_cmd_open_endpoint param;
	int ret;

	ret = copy_from_user(&param, uparam, sizeof(param));
	if (ret < 0) {
		printk(KERN_ERR "Open-MX: Failed to read open endpoint command argument, error %d\n", ret);
		goto out;
	}
	endpoint->board_index = param.board_index;
	endpoint->endpoint_index = param.endpoint_index;

	/* test whether the endpoint is ok to be open
	 * and mark it as initializing */
	spin_lock(&endpoint->lock);
	ret = -EINVAL;
	if (endpoint->status != OMX_ENDPOINT_STATUS_FREE) {
		spin_unlock(&endpoint->lock);
		goto out;
	}
	endpoint->status = OMX_ENDPOINT_STATUS_INITIALIZING;
	atomic_inc(&endpoint->refcount);
	spin_unlock(&endpoint->lock);

	/* alloc internal fields */
	ret = omx_endpoint_alloc_resources(endpoint);
	if (ret < 0)
		goto out_with_init;

	/* attach the endpoint to the iface */
	ret = omx_iface_attach_endpoint(endpoint);
	if (ret < 0)
		goto out_with_resources;

	printk(KERN_INFO "Open-MX: Successfully open board %d endpoint %d\n",
	       endpoint->board_index, endpoint->endpoint_index);

	return 0;

 out_with_resources:
	omx_endpoint_free_resources(endpoint);
 out_with_init:
	atomic_dec(&endpoint->refcount);
	endpoint->status = OMX_ENDPOINT_STATUS_FREE;
 out:
	return ret;
}

/* Wait for all users to release an endpoint and then close it.
 * If already closing, return -EBUSY.
 */
int
__omx_endpoint_close(struct omx_endpoint * endpoint,
		     int ifacelocked)
{
	DECLARE_WAITQUEUE(wq, current);
	int ret;

	/* test whether the endpoint is ok to be closed */
	spin_lock(&endpoint->lock);
	ret = -EBUSY;
	if (endpoint->status != OMX_ENDPOINT_STATUS_OK) {
		/* only CLOSING and OK endpoints may be attached to the iface */
		BUG_ON(endpoint->status != OMX_ENDPOINT_STATUS_CLOSING);
		spin_unlock(&endpoint->lock);
		goto out;
	}
	/* mark it as closing so that nobody may use it again */
	endpoint->status = OMX_ENDPOINT_STATUS_CLOSING;
	/* release our refcount now that other users cannot use again */
	atomic_dec(&endpoint->refcount);
	spin_unlock(&endpoint->lock);

	/* wait until refcount is 0 so that other users are gone */
	add_wait_queue(&endpoint->noref_queue, &wq);
	for(;;) {
		set_current_state(TASK_INTERRUPTIBLE);
		if (!atomic_read(&endpoint->refcount))
			break;
		schedule();
	}
	set_current_state(TASK_RUNNING);
	remove_wait_queue(&endpoint->noref_queue, &wq);

	/* release resources */
	omx_endpoint_free_resources(endpoint);

	/* detach */
	omx_iface_detach_endpoint(endpoint, ifacelocked);

	/* mark as free now */
	endpoint->status = OMX_ENDPOINT_STATUS_FREE;

	return 0;

 out:
	return ret;
}

static inline int
omx_endpoint_close(struct omx_endpoint * endpoint)
{
	return __omx_endpoint_close(endpoint, 0); /* we don't hold the iface lock */
}

/******************************
 * Acquiring/Releasing endpoints
 */

int
omx_endpoint_acquire(struct omx_endpoint * endpoint)
{
	int ret = -EINVAL;

	spin_lock(&endpoint->lock);
	if (unlikely(endpoint->status != OMX_ENDPOINT_STATUS_OK))
		goto out_with_lock;

	atomic_inc(&endpoint->refcount);

	spin_unlock(&endpoint->lock);
	return 0;

 out_with_lock:
	spin_unlock(&endpoint->lock);
	return ret;
}

struct omx_endpoint *
omx_endpoint_acquire_by_iface_index(struct omx_iface * iface, uint8_t index)
{
	struct omx_endpoint * endpoint;

	spin_lock(&iface->endpoint_lock);
	if (unlikely(index >= omx_endpoint_max))
		goto out_with_iface_lock;

	endpoint = iface->endpoints[index];
	if (unlikely(!endpoint))
		goto out_with_iface_lock;

	spin_lock(&endpoint->lock);
	if (unlikely(endpoint->status != OMX_ENDPOINT_STATUS_OK))
		goto out_with_endpoint_lock;

	atomic_inc(&endpoint->refcount);

	spin_unlock(&endpoint->lock);
	spin_unlock(&iface->endpoint_lock);
	return endpoint;

 out_with_endpoint_lock:
	spin_unlock(&endpoint->lock);
 out_with_iface_lock:
	spin_unlock(&iface->endpoint_lock);
	return NULL;
}

void
omx_endpoint_release(struct omx_endpoint * endpoint)
{
	/* decrement refcount and wake up the closer */
	if (unlikely(atomic_dec_and_test(&endpoint->refcount)))
		wake_up(&endpoint->noref_queue);
}

/******************************
 * File operations
 */

static int
omx_miscdev_open(struct inode * inode, struct file * file)
{
	struct omx_endpoint * endpoint;

	endpoint = kmalloc(sizeof(struct omx_endpoint), GFP_KERNEL);
	if (!endpoint)
		return -ENOMEM;

	spin_lock_init(&endpoint->lock);
	endpoint->status = OMX_ENDPOINT_STATUS_FREE;
	atomic_set(&endpoint->refcount, 0);
	init_waitqueue_head(&endpoint->noref_queue);

	file->private_data = endpoint;
	return 0;
}

static int
omx_miscdev_release(struct inode * inode, struct file * file)
{
	struct omx_endpoint * endpoint = file->private_data;

	BUG_ON(!endpoint);

	if (endpoint->status != OMX_ENDPOINT_STATUS_FREE)
		omx_endpoint_close(endpoint);

	return 0;
}

/*
 * Common command handlers
 * returns 0 on success, <0 on error,
 * 1 when success and does not want to release the reference on the endpoint
 */
static int (*omx_cmd_with_endpoint_handlers[])(struct omx_endpoint * endpoint, void __user * uparam) = {
	[OMX_CMD_SEND_TINY]		= omx_send_tiny,
	[OMX_CMD_SEND_SMALL]		= omx_send_small,
	[OMX_CMD_SEND_MEDIUM]		= omx_send_medium,
	[OMX_CMD_SEND_RENDEZ_VOUS]	= omx_send_rendez_vous,
	[OMX_CMD_SEND_PULL]		= omx_send_pull,
	[OMX_CMD_REGISTER_REGION]	= omx_register_user_region,
	[OMX_CMD_DEREGISTER_REGION]	= omx_deregister_user_region,
};

/*
 * Main ioctl switch where all application ioctls arrive
 */
static int
omx_miscdev_ioctl(struct inode *inode, struct file *file,
		  unsigned cmd, unsigned long arg)
{
	int ret = 0;

	switch (cmd) {

	case OMX_CMD_GET_BOARD_MAX: {
		uint32_t max = omx_iface_max;

		ret = copy_to_user((void __user *) arg, &max,
				   sizeof(max));
		if (ret < 0)
			printk(KERN_ERR "Open-MX: Failed to write get_board_max command result, error %d\n", ret);

		break;
	}

	case OMX_CMD_GET_ENDPOINT_MAX: {
		uint32_t max = omx_endpoint_max;

		ret = copy_to_user((void __user *) arg, &max,
				   sizeof(max));
		if (ret < 0)
			printk(KERN_ERR "Open-MX: Failed to write get_endpoint_max command result, error %d\n", ret);

		break;
	}

	case OMX_CMD_GET_PEER_MAX: {
		uint32_t max = omx_peer_max;

		ret = copy_to_user((void __user *) arg, &max,
				   sizeof(max));
		if (ret < 0)
			printk(KERN_ERR "Open-MX: Failed to write get_peer_max command result, error %d\n", ret);

		break;
	}

	case OMX_CMD_GET_BOARD_COUNT: {
		uint32_t count = omx_ifaces_get_count();

		ret = copy_to_user((void __user *) arg, &count,
				   sizeof(count));
		if (ret < 0)
			printk(KERN_ERR "Open-MX: Failed to write get_board_count command result, error %d\n", ret);

		break;
	}

	case OMX_CMD_GET_BOARD_ID: {
		struct omx_endpoint * endpoint = file->private_data;
		struct omx_cmd_get_board_id get_board_id;
		int use_endpoint = 0;

		/* try to acquire the endpoint */
		ret = omx_endpoint_acquire(endpoint);
		if (ret < 0) {
			/* the endpoint is not open, get the command parameter and use its board_index */
			ret = copy_from_user(&get_board_id, (void __user *) arg,
					     sizeof(get_board_id));
			if (ret < 0) {
				printk(KERN_ERR "Open-MX: Failed to read get_board_id command argument, error %d\n", ret);
				goto out;
			}
		} else {
			/* endpoint acquired, use its board index */
			get_board_id.board_index = endpoint->board_index;
			use_endpoint = 1;
		}

		ret = omx_iface_get_id(get_board_id.board_index,
				       &get_board_id.board_addr,
				       get_board_id.board_name);

		/* release the endpoint if we used it */
		if (use_endpoint)
			omx_endpoint_release(endpoint);

		if (ret < 0)
			goto out;

		ret = copy_to_user((void __user *) arg, &get_board_id,
				   sizeof(get_board_id));
		if (ret < 0)
			printk(KERN_ERR "Open-MX: Failed to write get_board_id command result, error %d\n", ret);

		break;
	}

	case OMX_CMD_OPEN_ENDPOINT: {
		struct omx_endpoint * endpoint = file->private_data;
		BUG_ON(!endpoint);

		ret = omx_endpoint_open(endpoint, (void __user *) arg);

		break;
	}

	case OMX_CMD_CLOSE_ENDPOINT: {
		struct omx_endpoint * endpoint = file->private_data;
		BUG_ON(!endpoint);

		ret = omx_endpoint_close(endpoint);

		break;
	}

	case OMX_CMD_SEND_TINY:
	case OMX_CMD_SEND_SMALL:
	case OMX_CMD_SEND_MEDIUM:
	case OMX_CMD_SEND_RENDEZ_VOUS:
	case OMX_CMD_SEND_PULL:
	case OMX_CMD_REGISTER_REGION:
	case OMX_CMD_DEREGISTER_REGION:
	{
		struct omx_endpoint * endpoint = file->private_data;

		BUG_ON(cmd >= ARRAY_SIZE(omx_cmd_with_endpoint_handlers));
		BUG_ON(omx_cmd_with_endpoint_handlers[cmd] == NULL);

		ret = omx_endpoint_acquire(endpoint);
		if (unlikely(ret < 0))
			goto out;

		ret = omx_cmd_with_endpoint_handlers[cmd](endpoint, (void __user *) arg);

		/* if ret > 0, the caller wants to keep a reference on the endpoint */
		if (likely(ret <= 0))
			omx_endpoint_release(endpoint);

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
omx_miscdev_mmap(struct file * file, struct vm_area_struct * vma)
{
	struct omx_endpoint * endpoint = file->private_data;
	unsigned long offset = vma->vm_pgoff << PAGE_SHIFT;
	unsigned long size = vma->vm_end - vma->vm_start;

	if (endpoint == NULL)
		return -EINVAL;

	if (offset == OMX_SENDQ_FILE_OFFSET && size == OMX_SENDQ_SIZE)
		return omx_remap_vmalloc_range(vma, endpoint->sendq, 0);
	else if (offset == OMX_RECVQ_FILE_OFFSET && size == OMX_RECVQ_SIZE)
		return omx_remap_vmalloc_range(vma, endpoint->sendq, OMX_SENDQ_SIZE >> PAGE_SHIFT);
	else if (offset == OMX_EVENTQ_FILE_OFFSET && size == OMX_EVENTQ_SIZE)
		return omx_remap_vmalloc_range(vma, endpoint->sendq, (OMX_SENDQ_SIZE + OMX_RECVQ_SIZE) >> PAGE_SHIFT);
	else {
		printk(KERN_ERR "Open-MX: Cannot mmap %lx at %lx\n", size, offset);
		return -EINVAL;
	}
}

static struct file_operations
omx_miscdev_fops = {
	.owner = THIS_MODULE,
	.open = omx_miscdev_open,
	.release = omx_miscdev_release,
	.mmap = omx_miscdev_mmap,
	.ioctl = omx_miscdev_ioctl,
};

static struct miscdevice
omx_miscdev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "open-mx",
	.fops = &omx_miscdev_fops,
};

/******************************
 * Device attributes
 */

#ifdef OMX_MISCDEV_HAVE_CLASS_DEVICE

static ssize_t
omx_ifaces_attr_show(struct class_device *dev, char *buf)
{
	return omx_ifaces_show(buf);
}

static ssize_t
omx_ifaces_attr_store(struct class_device *dev, const char *buf, size_t size)
{
	return omx_ifaces_store(buf, size);
}

static CLASS_DEVICE_ATTR(ifaces, S_IRUGO|S_IWUSR, omx_ifaces_attr_show, omx_ifaces_attr_store);

static int
omx_init_attributes(void)
{
	return class_device_create_file(omx_miscdev.class, &class_device_attr_ifaces);
}

static void
omx_exit_attributes(void)
{
	class_device_remove_file(omx_miscdev.class, &class_device_attr_ifaces);
}

#else /* !OMX_MISCDEV_HAVE_CLASS_DEVICE */

static ssize_t
omx_ifaces_attr_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return omx_ifaces_show(buf);
}

static ssize_t
omx_ifaces_attr_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t size)
{
	return omx_ifaces_store(buf, size);
}

static DEVICE_ATTR(ifaces, S_IRUGO|S_IWUSR, omx_ifaces_attr_show, omx_ifaces_attr_store);

static int
omx_init_attributes(void)
{
	return device_create_file(omx_miscdev.this_device, &dev_attr_ifaces);
}

static void
omx_exit_attributes(void)
{
	device_remove_file(omx_miscdev.this_device, &dev_attr_ifaces);
}

#endif /* !OMX_MISCDEV_HAVE_CLASS_DEVICE */


/******************************
 * Device registration
 */

int
omx_dev_init(void)
{
	int ret;

	ret = misc_register(&omx_miscdev);
	if (ret < 0) {
		printk(KERN_ERR "Open-MX: Failed to register misc device, error %d\n", ret);
		goto out;
	}

	ret = omx_init_attributes();
	if (ret < 0) {
		printk(KERN_ERR "Open-MX: failed to create misc device attributes, error %d\n", ret);
		goto out_with_device;
	}

	return 0;

 out_with_device:
	misc_deregister(&omx_miscdev);
 out:
	return ret;
}

void
omx_dev_exit(void)
{
	omx_exit_attributes();
	misc_deregister(&omx_miscdev);
}

/*
 * Local variables:
 *  tab-width: 8
 *  c-basic-offset: 8
 *  c-indent-level: 8
 * End:
 */
