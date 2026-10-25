// SPDX-License-Identifier: GPL-2.0
/*
 * chardev_test.c - functional and concurrency tests for /dev/chardev.
 *
 * Exit status is 0 only if every test passes.
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "chardev_ioctl.h"

#define DEV_PATH	"/dev/" CHARDEV_NAME
#define PROC_PATH	"/proc/" CHARDEV_PROC_NAME

#define NUM_WRITERS	4
#define NUM_READERS	4
#define MSGS_PER_WRITER	2000
#define TOTAL_MSGS	(NUM_WRITERS * MSGS_PER_WRITER)

static int tests_failed;

/* Opens and payload bytes this process caused, to cross-check /proc. */
static atomic_ulong my_opens;
static atomic_ulong my_bytes;

#define FAIL(...)						\
	do {							\
		printf("    FAIL: " __VA_ARGS__);		\
		printf("\n");					\
		return -1;					\
	} while (0)

static void report(const char *name, int rc)
{
	printf("[%s] %s\n", rc == 0 ? "PASS" : "FAIL", name);
	if (rc != 0)
		tests_failed++;
}

static int dev_open(void)
{
	int fd = open(DEV_PATH, O_RDWR);

	if (fd >= 0)
		atomic_fetch_add(&my_opens, 1);
	return fd;
}

static int dev_close(int fd)
{
	if (close(fd) != 0) {
		perror("close");
		return -1;
	}
	return 0;
}

/* Write one message; returns 0 on full success, -1 otherwise (errno set). */
static int write_msg(int fd, const char *msg, size_t len)
{
	ssize_t n = write(fd, msg, len);

	if (n < 0)
		return -1;
	if ((size_t)n != len) {
		errno = EIO;
		return -1;
	}
	atomic_fetch_add(&my_bytes, len);
	return 0;
}

static int get_count(int fd, __u32 *count)
{
	return ioctl(fd, CHARDEV_IOC_GET_COUNT, count);
}

static int clear(int fd)
{
	return ioctl(fd, CHARDEV_IOC_CLEAR);
}

static int read_stats(unsigned long *opens, unsigned long *bytes)
{
	FILE *f = fopen(PROC_PATH, "r");
	int ok;

	if (!f) {
		perror("fopen " PROC_PATH);
		return -1;
	}
	ok = fscanf(f, "open_count: %lu\nbytes_written: %lu", opens, bytes);
	if (fclose(f) != 0) {
		perror("fclose");
		return -1;
	}
	return ok == 2 ? 0 : -1;
}

/* ---------------------------------------------------------------- tests */

static int test_write_read(int fd)
{
	const char *msgs[] = { "hello", "kernel world", "x" };
	char buf[CHARDEV_MAX_MSG];
	size_t i;
	ssize_t n;

	for (i = 0; i < 3; i++)
		if (write_msg(fd, msgs[i], strlen(msgs[i])) != 0)
			FAIL("write \"%s\": %s", msgs[i], strerror(errno));

	for (i = 0; i < 3; i++) {
		n = read(fd, buf, sizeof(buf));
		if (n < 0)
			FAIL("read: %s", strerror(errno));
		if ((size_t)n != strlen(msgs[i]) || memcmp(buf, msgs[i], n))
			FAIL("message %zu mismatch: got %zd bytes", i, n);
	}
	return 0;
}

static int test_empty_read(int fd)
{
	char buf[CHARDEV_MAX_MSG];
	ssize_t n = read(fd, buf, sizeof(buf));

	if (n != 0)
		FAIL("expected 0 (EOF) on empty queue, got %zd (%s)", n,
		     n < 0 ? strerror(errno) : "data");
	return 0;
}

static int test_get_count(int fd)
{
	__u32 count;
	int i;

	for (i = 0; i < 5; i++)
		if (write_msg(fd, "msg", 3) != 0)
			FAIL("write: %s", strerror(errno));
	if (get_count(fd, &count) != 0)
		FAIL("GET_COUNT: %s", strerror(errno));
	if (count != 5)
		FAIL("GET_COUNT returned %u, expected 5", count);
	return clear(fd) == 0 ? 0 : -1;
}

static int test_clear(int fd)
{
	char buf[CHARDEV_MAX_MSG];
	__u32 count;

	if (write_msg(fd, "a", 1) != 0 || write_msg(fd, "b", 1) != 0)
		FAIL("write: %s", strerror(errno));
	if (clear(fd) != 0)
		FAIL("CLEAR: %s", strerror(errno));
	if (get_count(fd, &count) != 0)
		FAIL("GET_COUNT: %s", strerror(errno));
	if (count != 0)
		FAIL("count after CLEAR is %u, expected 0", count);
	if (read(fd, buf, sizeof(buf)) != 0)
		FAIL("read after CLEAR returned data");
	return 0;
}

static int test_invalid_sizes(int fd)
{
	char big[CHARDEV_MAX_MSG + 1];
	char small[4];

	memset(big, 'z', sizeof(big));

	if (write(fd, big, 0) != -1 || errno != EINVAL)
		FAIL("zero-length write should fail with EINVAL");
	if (write(fd, big, sizeof(big)) != -1 || errno != EMSGSIZE)
		FAIL("oversized write should fail with EMSGSIZE");

	if (write_msg(fd, "too long for buf", 16) != 0)
		FAIL("write: %s", strerror(errno));
	if (read(fd, small, sizeof(small)) != -1 || errno != EMSGSIZE)
		FAIL("short read should fail with EMSGSIZE");
	/* The message must still be there after the failed read. */
	if (read(fd, big, sizeof(big)) != 16)
		FAIL("message lost after EMSGSIZE read");

	/* volatile: the bad pointer is deliberate, so hide it from -Wnonnull */
	const void *volatile bad_ptr = NULL;

	if (write(fd, bad_ptr, 8) != -1 || errno != EFAULT)
		FAIL("write from NULL should fail with EFAULT");
	return 0;
}

static int test_bad_ioctl(int fd)
{
	if (ioctl(fd, _IO(CHARDEV_IOC_MAGIC, 99)) != -1 || errno != ENOTTY)
		FAIL("unknown ioctl should fail with ENOTTY");
	if (ioctl(fd, CHARDEV_IOC_GET_COUNT, NULL) != -1 || errno != EFAULT)
		FAIL("GET_COUNT to NULL should fail with EFAULT");
	return 0;
}

static int test_full(int fd)
{
	char msg[CHARDEV_MAX_MSG];
	int stored = 0;

	memset(msg, 'f', sizeof(msg));
	while (write_msg(fd, msg, sizeof(msg)) == 0)
		stored++;
	if (errno != ENOSPC)
		FAIL("expected ENOSPC when full, got %s", strerror(errno));
	printf("    buffer full after %d x %d-byte messages\n", stored,
	       CHARDEV_MAX_MSG);
	return clear(fd) == 0 ? 0 : -1;
}

/* ---------------------------------------------------- concurrency test */

static atomic_int total_read;
static atomic_int writers_done;
static atomic_int thread_error;
static atomic_char seen[NUM_WRITERS][MSGS_PER_WRITER];

/* Message for (writer, seq): "WwwSsssss|" followed by a known pattern. */
static size_t make_msg(char *buf, int w, int s)
{
	size_t len = 12 + (size_t)(w * 7 + s * 13) % 200;
	size_t i;

	snprintf(buf, CHARDEV_MAX_MSG, "W%02dS%05d|", w, s);
	for (i = 10; i < len; i++)
		buf[i] = 'a' + (w * 31 + s * 17 + i) % 26;
	return len;
}

static void *writer_thread(void *arg)
{
	int w = (int)(long)arg;
	char msg[CHARDEV_MAX_MSG];
	int fd, s;

	fd = dev_open();
	if (fd < 0) {
		perror("writer open");
		atomic_store(&thread_error, 1);
		return NULL;
	}

	for (s = 0; s < MSGS_PER_WRITER && !atomic_load(&thread_error); s++) {
		size_t len = make_msg(msg, w, s);

		while (write_msg(fd, msg, len) != 0) {
			if (errno != ENOSPC) {
				perror("writer write");
				atomic_store(&thread_error, 1);
				goto out;
			}
			sched_yield();	/* queue full: let readers drain it */
		}
	}
out:
	if (dev_close(fd) != 0)
		atomic_store(&thread_error, 1);
	atomic_fetch_add(&writers_done, 1);
	return NULL;
}

static int verify_msg(const char *buf, ssize_t n)
{
	char expect[CHARDEV_MAX_MSG];
	int w, s;
	size_t len;

	if (n < 10 || sscanf(buf, "W%2dS%5d|", &w, &s) != 2 ||
	    w < 0 || w >= NUM_WRITERS || s < 0 || s >= MSGS_PER_WRITER) {
		fprintf(stderr, "corrupt header: %.*s\n", (int)n, buf);
		return -1;
	}
	len = make_msg(expect, w, s);
	if ((size_t)n != len || memcmp(buf, expect, len) != 0) {
		fprintf(stderr, "corrupt payload for W%02dS%05d\n", w, s);
		return -1;
	}
	if (atomic_exchange(&seen[w][s], 1)) {
		fprintf(stderr, "duplicate W%02dS%05d\n", w, s);
		return -1;
	}
	return 0;
}

static void *reader_thread(void *arg)
{
	char buf[CHARDEV_MAX_MSG];
	ssize_t n;
	int fd;

	(void)arg;
	fd = dev_open();
	if (fd < 0) {
		perror("reader open");
		atomic_store(&thread_error, 1);
		return NULL;
	}

	while (atomic_load(&total_read) < TOTAL_MSGS &&
	       !atomic_load(&thread_error)) {
		n = read(fd, buf, sizeof(buf));
		if (n < 0) {
			perror("reader read");
			atomic_store(&thread_error, 1);
			break;
		}
		if (n == 0) {
			/* Empty. If every writer has finished, nothing more comes. */
			if (atomic_load(&writers_done) == NUM_WRITERS) {
				n = read(fd, buf, sizeof(buf));
				if (n == 0)
					break;
				if (n < 0) {
					perror("reader read");
					atomic_store(&thread_error, 1);
					break;
				}
			} else {
				sched_yield();
				continue;
			}
		}
		if (verify_msg(buf, n) != 0) {
			atomic_store(&thread_error, 1);
			break;
		}
		atomic_fetch_add(&total_read, 1);
	}

	if (dev_close(fd) != 0)
		atomic_store(&thread_error, 1);
	return NULL;
}

static int test_concurrent(void)
{
	pthread_t writers[NUM_WRITERS], readers[NUM_READERS];
	int i, w, s, rc, missing = 0;
	int started_w = 0, started_r = 0;

	for (i = 0; i < NUM_READERS; i++) {
		rc = pthread_create(&readers[i], NULL, reader_thread, NULL);
		if (rc != 0) {
			fprintf(stderr, "pthread_create: %s\n", strerror(rc));
			atomic_store(&thread_error, 1);
			break;
		}
		started_r++;
	}
	for (i = 0; i < NUM_WRITERS && !atomic_load(&thread_error); i++) {
		rc = pthread_create(&writers[i], NULL, writer_thread,
				    (void *)(long)i);
		if (rc != 0) {
			fprintf(stderr, "pthread_create: %s\n", strerror(rc));
			atomic_store(&thread_error, 1);
			break;
		}
		started_w++;
	}
	/* Writers that never started still count as done for the readers. */
	atomic_fetch_add(&writers_done, NUM_WRITERS - started_w);

	for (i = 0; i < started_w; i++) {
		rc = pthread_join(writers[i], NULL);
		if (rc != 0)
			FAIL("pthread_join: %s", strerror(rc));
	}
	for (i = 0; i < started_r; i++) {
		rc = pthread_join(readers[i], NULL);
		if (rc != 0)
			FAIL("pthread_join: %s", strerror(rc));
	}

	if (atomic_load(&thread_error))
		FAIL("a thread reported an error (see stderr)");

	for (w = 0; w < NUM_WRITERS; w++)
		for (s = 0; s < MSGS_PER_WRITER; s++)
			if (!atomic_load(&seen[w][s]))
				missing++;
	if (missing)
		FAIL("%d messages lost", missing);

	printf("    %d writers + %d readers moved %d messages intact\n",
	       NUM_WRITERS, NUM_READERS, TOTAL_MSGS);
	return 0;
}

static int test_proc_stats(unsigned long opens0, unsigned long bytes0)
{
	unsigned long opens1, bytes1;

	if (read_stats(&opens1, &bytes1) != 0)
		FAIL("could not parse " PROC_PATH);
	printf("    open_count +%lu, bytes_written +%lu\n",
	       opens1 - opens0, bytes1 - bytes0);
	if (opens1 - opens0 != atomic_load(&my_opens))
		FAIL("open_count grew by %lu, expected %lu",
		     opens1 - opens0, atomic_load(&my_opens));
	if (bytes1 - bytes0 != atomic_load(&my_bytes))
		FAIL("bytes_written grew by %lu, expected %lu",
		     bytes1 - bytes0, atomic_load(&my_bytes));
	return 0;
}

int main(void)
{
	unsigned long opens0, bytes0;
	int fd;

	setvbuf(stdout, NULL, _IOLBF, 0);

	if (read_stats(&opens0, &bytes0) != 0) {
		fprintf(stderr, "cannot read %s - is the module loaded?\n",
			PROC_PATH);
		return 1;
	}

	fd = dev_open();
	report("open " DEV_PATH, fd < 0 ? -1 : 0);
	if (fd < 0) {
		perror("open");
		return 1;
	}
	if (clear(fd) != 0) {
		perror("initial CLEAR");
		dev_close(fd);
		return 1;
	}

	report("write then read back in order", test_write_read(fd));
	report("read on empty queue returns 0", test_empty_read(fd));
	report("ioctl GET_COUNT", test_get_count(fd));
	report("ioctl CLEAR", test_clear(fd));
	report("invalid sizes and pointers rejected", test_invalid_sizes(fd));
	report("bad ioctl arguments rejected", test_bad_ioctl(fd));
	report("ENOSPC when buffer is full", test_full(fd));
	report("concurrent pthread readers/writers", test_concurrent());
	report("/proc stats match activity", test_proc_stats(opens0, bytes0));

	report("close", dev_close(fd));

	printf("\n%s: %d test(s) failed\n", tests_failed ? "FAILED" : "OK",
	       tests_failed);
	return tests_failed ? 1 : 0;
}
