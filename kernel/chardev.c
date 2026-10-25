// SPDX-License-Identifier: GPL-2.0
/*
 * chardev.c - a message-queue character device.
 *
 * Each write() stores one message (1..CHARDEV_MAX_MSG bytes); each read()
 * returns the oldest message whole. Messages are packed into a single
 * kmalloc'd buffer as [u16 length][payload] records. One mutex protects the
 * buffer and the statistics.
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>

#include "chardev_ioctl.h"

#define HDR_SIZE sizeof(u16)

static dev_t chardev_devt;
static struct cdev chardev_cdev;
static struct class *chardev_class;
static struct device *chardev_device;
static struct proc_dir_entry *chardev_proc;

/* Everything below is protected by chardev_lock. */
static DEFINE_MUTEX(chardev_lock);
static char *msg_buf;		/* CHARDEV_BUF_SIZE bytes of records */
static size_t buf_used;		/* bytes of msg_buf currently in use */
static u32 msg_count;		/* records currently queued */
static u64 open_count;		/* successful open() calls since load */
static u64 bytes_written;	/* payload bytes accepted since load */

static int chardev_open(struct inode *inode, struct file *file)
{
	if (mutex_lock_interruptible(&chardev_lock))
		return -ERESTARTSYS;
	open_count++;
	mutex_unlock(&chardev_lock);

	pr_debug("open by pid %d\n", current->pid);
	return 0;
}

static int chardev_release(struct inode *inode, struct file *file)
{
	pr_debug("release by pid %d\n", current->pid);
	return 0;
}

static ssize_t chardev_read(struct file *file, char __user *ubuf,
			    size_t count, loff_t *ppos)
{
	u16 len;
	ssize_t ret;

	if (mutex_lock_interruptible(&chardev_lock))
		return -ERESTARTSYS;

	if (msg_count == 0) {
		ret = 0;	/* empty queue reads as end-of-file */
		goto out;
	}

	memcpy(&len, msg_buf, HDR_SIZE);
	if (count < len) {
		/* Leave the message queued rather than truncate it. */
		ret = -EMSGSIZE;
		goto out;
	}

	if (copy_to_user(ubuf, msg_buf + HDR_SIZE, len)) {
		ret = -EFAULT;
		goto out;
	}

	/* Only consume the message once the copy has fully succeeded. */
	buf_used -= HDR_SIZE + len;
	memmove(msg_buf, msg_buf + HDR_SIZE + len, buf_used);
	msg_count--;
	ret = len;
	pr_debug("read %u bytes, %u messages left\n", len, msg_count);
out:
	mutex_unlock(&chardev_lock);
	return ret;
}

static ssize_t chardev_write(struct file *file, const char __user *ubuf,
			     size_t count, loff_t *ppos)
{
	u16 len;
	ssize_t ret;

	if (count == 0)
		return -EINVAL;
	if (count > CHARDEV_MAX_MSG)
		return -EMSGSIZE;
	len = count;

	if (mutex_lock_interruptible(&chardev_lock))
		return -ERESTARTSYS;

	if (CHARDEV_BUF_SIZE - buf_used < HDR_SIZE + len) {
		ret = -ENOSPC;
		goto out;
	}

	/*
	 * Copy into the free space past buf_used. Nothing is committed until
	 * buf_used advances, so a faulting copy leaves the queue unchanged.
	 */
	if (copy_from_user(msg_buf + buf_used + HDR_SIZE, ubuf, len)) {
		ret = -EFAULT;
		goto out;
	}
	memcpy(msg_buf + buf_used, &len, HDR_SIZE);
	buf_used += HDR_SIZE + len;
	msg_count++;
	bytes_written += len;
	ret = len;
	pr_debug("wrote %u bytes, %u messages queued\n", len, msg_count);
out:
	mutex_unlock(&chardev_lock);
	return ret;
}

static long chardev_ioctl(struct file *file, unsigned int cmd,
			  unsigned long arg)
{
	u32 count;

	if (_IOC_TYPE(cmd) != CHARDEV_IOC_MAGIC)
		return -ENOTTY;

	switch (cmd) {
	case CHARDEV_IOC_GET_COUNT:
		if (mutex_lock_interruptible(&chardev_lock))
			return -ERESTARTSYS;
		count = msg_count;
		mutex_unlock(&chardev_lock);

		if (copy_to_user((void __user *)arg, &count, sizeof(count)))
			return -EFAULT;
		return 0;

	case CHARDEV_IOC_CLEAR:
		if (mutex_lock_interruptible(&chardev_lock))
			return -ERESTARTSYS;
		count = msg_count;
		buf_used = 0;
		msg_count = 0;
		mutex_unlock(&chardev_lock);

		pr_info("buffer cleared (%u messages discarded)\n", count);
		return 0;

	default:
		return -ENOTTY;
	}
}

static const struct file_operations chardev_fops = {
	.owner		= THIS_MODULE,
	.open		= chardev_open,
	.release	= chardev_release,
	.read		= chardev_read,
	.write		= chardev_write,
	.unlocked_ioctl	= chardev_ioctl,
	.compat_ioctl	= compat_ptr_ioctl,
	.llseek		= noop_llseek,
};

static int chardev_stats_show(struct seq_file *m, void *v)
{
	u64 opens, written;

	if (mutex_lock_interruptible(&chardev_lock))
		return -ERESTARTSYS;
	opens = open_count;
	written = bytes_written;
	mutex_unlock(&chardev_lock);

	seq_printf(m, "open_count: %llu\n", opens);
	seq_printf(m, "bytes_written: %llu\n", written);
	return 0;
}

static int chardev_stats_open(struct inode *inode, struct file *file)
{
	return single_open(file, chardev_stats_show, NULL);
}

static const struct proc_ops chardev_proc_ops = {
	.proc_open	= chardev_stats_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};

static int __init chardev_init(void)
{
	int ret;

	msg_buf = kmalloc(CHARDEV_BUF_SIZE, GFP_KERNEL);
	if (!msg_buf)
		return -ENOMEM;

	ret = alloc_chrdev_region(&chardev_devt, 0, 1, CHARDEV_NAME);
	if (ret) {
		pr_err("alloc_chrdev_region failed: %d\n", ret);
		goto err_free_buf;
	}

	cdev_init(&chardev_cdev, &chardev_fops);
	ret = cdev_add(&chardev_cdev, chardev_devt, 1);
	if (ret) {
		pr_err("cdev_add failed: %d\n", ret);
		goto err_unregister;
	}

	chardev_class = class_create(CHARDEV_NAME);
	if (IS_ERR(chardev_class)) {
		ret = PTR_ERR(chardev_class);
		pr_err("class_create failed: %d\n", ret);
		goto err_cdev_del;
	}

	/* udev sees this uevent and creates /dev/chardev. */
	chardev_device = device_create(chardev_class, NULL, chardev_devt, NULL,
				       CHARDEV_NAME);
	if (IS_ERR(chardev_device)) {
		ret = PTR_ERR(chardev_device);
		pr_err("device_create failed: %d\n", ret);
		goto err_class_destroy;
	}

	chardev_proc = proc_create(CHARDEV_PROC_NAME, 0444, NULL,
				   &chardev_proc_ops);
	if (!chardev_proc) {
		ret = -ENOMEM;
		pr_err("proc_create failed\n");
		goto err_device_destroy;
	}

	pr_info("loaded: major %d minor %d, buffer %d bytes\n",
		MAJOR(chardev_devt), MINOR(chardev_devt), CHARDEV_BUF_SIZE);
	return 0;

err_device_destroy:
	device_destroy(chardev_class, chardev_devt);
err_class_destroy:
	class_destroy(chardev_class);
err_cdev_del:
	cdev_del(&chardev_cdev);
err_unregister:
	unregister_chrdev_region(chardev_devt, 1);
err_free_buf:
	kfree(msg_buf);
	return ret;
}

static void __exit chardev_exit(void)
{
	proc_remove(chardev_proc);
	device_destroy(chardev_class, chardev_devt);
	class_destroy(chardev_class);
	cdev_del(&chardev_cdev);
	unregister_chrdev_region(chardev_devt, 1);
	kfree(msg_buf);
	pr_info("unloaded: %llu opens, %llu bytes written\n",
		open_count, bytes_written);
}

module_init(chardev_init);
module_exit(chardev_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Aman Kumar Singh");
MODULE_DESCRIPTION("Message-queue character device with ioctl and procfs stats");
