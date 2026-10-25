# chardev: a message-queue character device

A Linux kernel module that creates `/dev/chardev`. Each `write()` stores one
message in a kernel buffer, and each `read()` returns the oldest message
whole (FIFO). Two ioctls report the message count and clear the queue, and
`/proc/chardev_stats` shows usage counters.

## Architecture

```
 user space                         kernel space
 ───────────                        ─────────────────────────────────────────
 chardev_test ──open/read/write──▶  VFS ──▶ chardev_fops (kernel/chardev.c)
              ──ioctl───────────▶            │
                                             ▼
                                   chardev_lock (mutex) guards:
                                     msg_buf  [len|data][len|data]...  4 KiB
                                     msg_count, open_count, bytes_written
 cat /proc/chardev_stats ────────▶  seq_file show() (reads counters under lock)

 include/chardev_ioctl.h is compiled into both sides
```

| Path | Role |
|---|---|
| `include/chardev_ioctl.h` | ioctl numbers and limits shared by kernel and user space |
| `kernel/chardev.c` | the driver |
| `kernel/Makefile` | Kbuild makefile for an out-of-tree module |
| `user/chardev_test.c` | functional and multi-threaded tests |
| `user/Makefile` | `make` (optimised) or `make debug` (`-O0 -g3`) |
| `scripts/run_tests.sh` | build, load, test, valgrind, dmesg, unload |
| `scripts/valgrind.supp` | hides valgrind's reports for the two deliberate bad-pointer tests |

When the module loads it:

1. allocates the 4 KiB buffer with `kmalloc`
2. gets a dynamic major number (`alloc_chrdev_region`)
3. registers the file operations (`cdev_add`)
4. creates a device class and device; udev then creates `/dev/chardev`
5. creates `/proc/chardev_stats`

Unloading undoes these steps in reverse order.

## Behaviour

| Call | Result |
|---|---|
| `write(fd, buf, n)`, 1 ≤ n ≤ 256 | queues one message, returns `n` |
| `write` with n = 0 | `-EINVAL` |
| `write` with n > 256 | `-EMSGSIZE` |
| `write` when the buffer is full | `-ENOSPC` (nothing is stored) |
| `write`/`read` with a bad pointer | `-EFAULT` (queue unchanged) |
| `read(fd, buf, n)` | returns the oldest message and removes it from the queue |
| `read` on an empty queue | `0` (end of file), so `cat /dev/chardev` ends |
| `read` with n smaller than the message | `-EMSGSIZE`, and the message stays queued |

Each message uses 2 bytes of header plus its payload, so the 4096-byte buffer
holds 15 maximum-size messages, or many more small ones. Calls never block.

## ioctl interface

Defined in `include/chardev_ioctl.h`, magic `'k'`:

| Command | Encoding | Argument | Effect |
|---|---|---|---|
| `CHARDEV_IOC_GET_COUNT` | `_IOR('k', 1, __u32)` | `__u32 *` | stores the number of queued messages |
| `CHARDEV_IOC_CLEAR` | `_IO('k', 2)` | none | discards all messages |

Any other command returns `-ENOTTY`, and a bad pointer passed to `GET_COUNT`
returns `-EFAULT`.

```c
__u32 n;
ioctl(fd, CHARDEV_IOC_GET_COUNT, &n);
ioctl(fd, CHARDEV_IOC_CLEAR);
```

## /proc/chardev_stats

```
open_count: 13
bytes_written: 1063527
```

`open_count` is the number of successful `open()` calls since the module
loaded. `bytes_written` is the total payload accepted by `write()`.
`CLEAR` does not reset either counter.

## Requirements

You need a Linux system. This was built against kernel 7.0 (Ubuntu 24.04
userland), and it should build on any kernel from 6.4 on, where
`class_create()` takes one argument.

```sh
sudo apt install build-essential linux-headers-$(uname -r) valgrind
```

**On Windows or macOS** you need a Linux VM, because WSL2 and Docker share a
kernel you can't easily load modules into. Install VirtualBox, VMware or UTM
(on Apple Silicon, use an ARM64 Ubuntu image), create an Ubuntu Server VM
with 2 CPUs (so the threaded test really runs in parallel), 2 GB of RAM and a
20 GB disk, then run the `apt` line above inside it. A VM is a good idea even
on Linux: a bug in a kernel module can crash the whole machine.

**Secure Boot**: if it is enabled, `insmod` fails with "Key was rejected by
service". Check with `mokutil --sb-state`. Either disable Secure Boot in the
VM's firmware settings, or sign the module with a MOK key.

## Build, load, test

Everything in one step (prompts for your sudo password):

```sh
sudo scripts/run_tests.sh
```

Or by hand:

```sh
make -C kernel                   # produces kernel/chardev.ko
make -C user                     # or: make -C user debug
sudo insmod kernel/chardev.ko
ls -l /dev/chardev               # crw------- root root <major>, 0
sudo user/chardev_test
sudo valgrind --leak-check=full --suppressions=scripts/valgrind.supp user/chardev_test
cat /proc/chardev_stats
sudo dmesg | grep chardev
sudo rmmod chardev
```

Quick manual check:

```sh
echo hello | sudo tee /dev/chardev   # queues "hello\n"
sudo cat /dev/chardev                # prints it and removes it from the queue
```

The device node is root-only (mode 0600, the udev default), so the tests run
under `sudo`.

### Per-call debug logging

Reads, writes and opens are logged with `pr_debug`, which is off by default.
To turn it on:

```sh
echo 'module chardev +p' | sudo tee /sys/kernel/debug/dynamic_debug/control
```

The concurrency test then produces about 16,000 log lines.
