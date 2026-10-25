/* SPDX-License-Identifier: GPL-2.0 */
/*
 * chardev_ioctl.h - interface shared by the chardev kernel module and
 * user-space programs. Only <linux/...> uapi headers are used so this file
 * compiles in both worlds.
 */
#ifndef CHARDEV_IOCTL_H
#define CHARDEV_IOCTL_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define CHARDEV_NAME		"chardev"	/* /dev/chardev */
#define CHARDEV_PROC_NAME	"chardev_stats"	/* /proc/chardev_stats */

/* Largest single message accepted by write(). */
#define CHARDEV_MAX_MSG		256
/* Total bytes of message storage in the kernel (includes 2-byte headers). */
#define CHARDEV_BUF_SIZE	4096

/* Arbitrary "magic" byte that namespaces our ioctl numbers. */
#define CHARDEV_IOC_MAGIC	'k'

/* Copy the number of queued messages into a __u32 supplied by the caller. */
#define CHARDEV_IOC_GET_COUNT	_IOR(CHARDEV_IOC_MAGIC, 1, __u32)
/* Discard every queued message. Takes no argument. */
#define CHARDEV_IOC_CLEAR	_IO(CHARDEV_IOC_MAGIC, 2)

#endif /* CHARDEV_IOCTL_H */
