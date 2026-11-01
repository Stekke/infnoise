// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Userspace test harness for Infinite Noise TRNG kernel module
 *
 * Copyright (C) 2026 Avinash H. Duduskar
 *
 * Verifies driver mechanics: read interfaces, ioctl dispatch, non-blocking
 * I/O, and device lifecycle.  Entropy quality testing is handled by
 * make test (rngtest, ent) and the project's tests/runtests.sh suite.
 *
 * Requires: module loaded, device connected, root privileges.
 *
 * Build:  make test_infnoise
 * Run:    sudo ./test_infnoise   (or: make test)
 *
 * --destructive unbinds and resets the device while it is open, and --suspend
 * suspends the system with the device open.  Run these on a kernel with
 * CONFIG_KASAN, because a lifetime bug can pass here and only appear in dmesg.
 *
 * VID/PID note: the module matches 0x1209:0x3701 (pid.codes registered).
 * Stock Infinite Noise hardware ships as 0x0403:0x6015 (FTDI generic).
 * To test on unmodified hardware, temporarily change INFNOISE_VENDOR_ID
 * and INFNOISE_PRODUCT_ID in infnoise.h, rebuild, rmmod ftdi_sio, and
 * insmod the patched module.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <stdint.h>
#include <time.h>
#include <glob.h>
#include <dirent.h>
#include <linux/usbdevice_fs.h>

/* Must match kernel header exactly */
struct infnoise_stats {
	uint64_t total_bits;
	uint32_t total_ones;
	uint32_t total_zeros;
	uint32_t even_misfires;
	uint32_t odd_misfires;
	uint32_t entropy_estimate;
	uint32_t k_estimate;
	uint8_t warmup_complete;
	uint8_t health_ok;
	uint8_t __pad[6];
} __attribute__((aligned(8)));

#define INFNOISE_IOC_MAGIC	'N'
#define INFNOISE_GET_STATS	_IOR(INFNOISE_IOC_MAGIC, 1, struct infnoise_stats)
#define INFNOISE_SET_RAW	_IOW(INFNOISE_IOC_MAGIC, 2, int)
#define INFNOISE_GET_ENTROPY	_IOR(INFNOISE_IOC_MAGIC, 3, uint32_t)

static int passed = 0, failed = 0, skipped = 0;
static char dev_path[64];

#define DRIVER_SYSFS "/sys/bus/usb/drivers/infnoise"

#define TEST(name) printf("  %-50s ", name)
#define PASS() do { printf("\033[32mPASS\033[0m\n"); passed++; } while(0)
#define FAIL(msg) do { printf("\033[31mFAIL\033[0m (%s)\n", msg); failed++; } while(0)
#define SKIP(msg) do { printf("\033[33mSKIP\033[0m (%s)\n", msg); skipped++; } while(0)

/* ── Read interfaces ────────────────────────────────────── */

static void test_chardev_read(int fd)
{
	uint8_t buf[256];
	ssize_t n;

	TEST("chardev read returns data");
	n = read(fd, buf, sizeof(buf));
	if (n < 0) {
		FAIL(strerror(errno));
		return;
	}
	if (n == 0) {
		FAIL("got 0 bytes");
		return;
	}
	printf("got %zd bytes ", n);
	PASS();
}

static void test_hwrng_read(void)
{
	int fd;
	uint8_t buf[64];
	ssize_t n;

	TEST("/dev/hwrng read returns data");
	fd = open("/dev/hwrng", O_RDONLY);
	if (fd < 0) {
		SKIP(strerror(errno));
		return;
	}
	n = read(fd, buf, sizeof(buf));
	close(fd);
	if (n < 0) {
		FAIL(strerror(errno));
		return;
	}
	if (n == 0) {
		FAIL("got 0 bytes");
		return;
	}
	printf("got %zd bytes ", n);
	PASS();
}

/* ── Non-blocking I/O ───────────────────────────────────── */

static void test_nonblock(void)
{
	TEST("O_NONBLOCK returns EAGAIN or data");
	int fd = open(dev_path, O_RDONLY | O_NONBLOCK);
	if (fd < 0) {
		FAIL(strerror(errno));
		return;
	}
	uint8_t buf[64];
	ssize_t n = read(fd, buf, sizeof(buf));
	close(fd);

	if (n > 0) {
		printf("got %zd bytes ", n);
		PASS();
	} else if (n < 0 && errno == EAGAIN) {
		printf("EAGAIN (device warming up) ");
		PASS();
	} else if (n < 0) {
		FAIL(strerror(errno));
	} else {
		FAIL("unexpected 0 return");
	}
}

/*
 * Regression test: blocking read must be interruptible by signal.
 *
 * Forks a child that reads in a blocking loop, parent sends SIGUSR1
 * after 500ms.  The child should exit cleanly via EINTR.  If the
 * driver's retry loop doesn't check signal_pending(), the child
 * hangs forever.
 */
static volatile sig_atomic_t child_got_signal = 0;
static void child_handler(int sig) { (void)sig; child_got_signal = 1; }

static void test_interruptible(void)
{
	TEST("blocking read is interruptible by signal");

	pid_t pid = fork();
	if (pid < 0) {
		FAIL(strerror(errno));
		return;
	}

	if (pid == 0) {
		signal(SIGUSR1, child_handler);
		int fd = open(dev_path, O_RDONLY);
		if (fd < 0) _exit(2);

		uint8_t buf[64];
		while (!child_got_signal) {
			ssize_t n = read(fd, buf, sizeof(buf));
			if (n < 0 && errno == EINTR) {
				close(fd);
				_exit(0);
			}
			if (n < 0) {
				close(fd);
				_exit(3);
			}
		}
		close(fd);
		_exit(0);
	}

	usleep(500000);
	kill(pid, SIGUSR1);

	int status;
	struct timespec ts = { .tv_sec = 3, .tv_nsec = 0 };
	nanosleep(&ts, NULL);

	pid_t ret = waitpid(pid, &status, WNOHANG);
	if (ret == 0) {
		kill(pid, SIGKILL);
		waitpid(pid, &status, 0);
		FAIL("child hung (busy-wait or uninterruptible)");
	} else if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
		PASS();
	} else {
		char msg[64];
		snprintf(msg, sizeof(msg), "child exit=%d", WEXITSTATUS(status));
		FAIL(msg);
	}
}

/* ── Ioctl interface ────────────────────────────────────── */

/*
 * Regression test: struct must be 40 bytes on all architectures.
 *
 * With __aligned(8) and explicit __pad[6], the struct is 40 bytes on
 * both 32-bit and 64-bit.  Without it, 32-bit compilers produce 36
 * bytes, causing _IOR to encode a different ioctl number.
 */
static void test_struct_size(void)
{
	TEST("sizeof(infnoise_stats) == 40");
	if (sizeof(struct infnoise_stats) == 40) {
		PASS();
	} else {
		char msg[64];
		snprintf(msg, sizeof(msg), "got %zu", sizeof(struct infnoise_stats));
		FAIL(msg);
	}
}

static void test_ioctl_get_stats(int fd)
{
	struct infnoise_stats stats;

	TEST("GET_STATS returns sane values");
	memset(&stats, 0xAA, sizeof(stats));

	if (ioctl(fd, INFNOISE_GET_STATS, &stats) < 0) {
		FAIL(strerror(errno));
		return;
	}

	if (!stats.warmup_complete) {
		FAIL("warmup_complete is 0");
		return;
	}
	if (!stats.health_ok) {
		FAIL("health_ok is 0");
		return;
	}

	printf("bits=%lu ones=%u zeros=%u ",
	       (unsigned long)stats.total_bits, stats.total_ones,
	       stats.total_zeros);
	PASS();
}

/*
 * Regression test: padding bytes in infnoise_stats must be zeroed.
 *
 * Pre-fills the struct with 0xAA, then calls GET_STATS.  If the kernel
 * doesn't memset the struct before copy_to_user, the 6 padding bytes
 * (offsets 34-39) contain uninitialized kernel stack data — an info leak.
 */
static void test_padding_zeroed(int fd)
{
	struct infnoise_stats stats;

	TEST("GET_STATS padding bytes zeroed (info leak check)");
	memset(&stats, 0xAA, sizeof(stats));

	if (ioctl(fd, INFNOISE_GET_STATS, &stats) < 0) {
		FAIL(strerror(errno));
		return;
	}

	int pad_ok = 1;
	for (int i = 0; i < 6; i++) {
		if (stats.__pad[i] != 0) pad_ok = 0;
	}
	if (!pad_ok) {
		FAIL("padding bytes not zeroed (kernel stack info leak)");
		return;
	}
	PASS();
}

static void test_ioctl_get_entropy(int fd)
{
	uint32_t entropy = 0;

	TEST("GET_ENTROPY returns non-zero");
	if (ioctl(fd, INFNOISE_GET_ENTROPY, &entropy) < 0) {
		FAIL(strerror(errno));
		return;
	}
	if (entropy == 0) {
		FAIL("entropy is 0");
		return;
	}
	printf("entropy=%u ", entropy);
	PASS();
}

static void test_ioctl_set_raw(int fd)
{
	int raw = 0;

	TEST("SET_RAW accepts value");
	if (ioctl(fd, INFNOISE_SET_RAW, &raw) < 0) {
		FAIL(strerror(errno));
		return;
	}
	PASS();
}

static void test_ioctl_bad_cmd(int fd)
{
	TEST("invalid ioctl returns ENOTTY");
	int ret = ioctl(fd, _IO('N', 99), NULL);
	if (ret == -1 && errno == ENOTTY) {
		PASS();
	} else {
		char buf[64];
		snprintf(buf, sizeof(buf), "ret=%d errno=%d (%s)",
			 ret, errno, strerror(errno));
		FAIL(buf);
	}
}

/* ── Device lifecycle ───────────────────────────────────── */

static void test_open_close_reopen(void)
{
	TEST("open/close/reopen 5 times");

	for (int i = 0; i < 5; i++) {
		int fd = open(dev_path, O_RDONLY);
		if (fd < 0) {
			char msg[64];
			snprintf(msg, sizeof(msg), "open %d: %s", i, strerror(errno));
			FAIL(msg);
			return;
		}
		uint8_t buf[32];
		ssize_t n = read(fd, buf, sizeof(buf));
		close(fd);
		if (n <= 0) {
			char msg[64];
			snprintf(msg, sizeof(msg), "read %d: %zd bytes", i, n);
			FAIL(msg);
			return;
		}
	}
	printf("5 cycles OK ");
	PASS();
}

static int read_full(int fd, size_t len)
{
	uint8_t buf[256];

	while (len > 0) {
		ssize_t n = read(fd, buf, len < sizeof(buf) ? len : sizeof(buf));

		if (n <= 0)
			return -1;
		len -= n;
	}
	return 0;
}

static void test_concurrent_readers(void)
{
	pid_t pids[4];
	int bad = 0;

	TEST("4 concurrent readers each get 4096 bytes");

	for (int i = 0; i < 4; i++) {
		pids[i] = fork();
		if (pids[i] < 0) {
			FAIL(strerror(errno));
			return;
		}
		if (pids[i] == 0) {
			int fd = open(dev_path, O_RDONLY);

			alarm(60);
			_exit(fd < 0 || read_full(fd, 4096) < 0);
		}
	}

	for (int i = 0; i < 4; i++) {
		int status;

		waitpid(pids[i], &status, 0);
		if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
			bad++;
	}

	if (bad)
		FAIL("a reader failed");
	else
		PASS();
}

/* SET_RAW is shared by all readers, so it must need privilege. */
static void test_set_raw_needs_privilege(int fd)
{
	int status;
	pid_t pid;

	TEST("SET_RAW returns EPERM without privilege");

	pid = fork();
	if (pid < 0) {
		FAIL(strerror(errno));
		return;
	}
	if (pid == 0) {
		int raw = 0;

		if (setuid(65534) < 0)
			_exit(2);
		_exit(ioctl(fd, INFNOISE_SET_RAW, &raw) == -1 && errno == EPERM ? 0 : 1);
	}

	waitpid(pid, &status, 0);
	if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
		PASS();
	else
		FAIL("unprivileged SET_RAW was not refused");
}

/* ── Destructive tests ──────────────────────────────────── */

/* Find the bound interface, for example "1-2:1.0" */
static int find_interface(char *name, size_t size)
{
	DIR *dir = opendir(DRIVER_SYSFS);
	struct dirent *ent;
	int ret = -1;

	if (!dir)
		return -1;
	while ((ent = readdir(dir))) {
		if (strchr(ent->d_name, ':')) {
			snprintf(name, size, "%.*s", (int)size - 1, ent->d_name);
			ret = 0;
			break;
		}
	}
	closedir(dir);
	return ret;
}

static int write_file(const char *path, const char *value)
{
	int fd = open(path, O_WRONLY);
	ssize_t n;

	if (fd < 0)
		return -1;
	n = write(fd, value, strlen(value));
	close(fd);
	return n < 0 ? -1 : 0;
}

/* Wait for the node to come back, and for warmup to finish */
static int wait_for_device(void)
{
	for (int i = 0; i < 100; i++) {
		int fd = open(dev_path, O_RDONLY);

		if (fd >= 0) {
			int ret = read_full(fd, 64);

			close(fd);
			if (ret == 0)
				return 0;
		}
		usleep(200000);
	}
	return -1;
}

/* A child blocks in read() on fd until the device goes away. */
static pid_t start_reader(int fd)
{
	pid_t pid = fork();

	if (pid == 0) {
		uint8_t buf[256];

		alarm(30);
		while (read(fd, buf, sizeof(buf)) > 0)
			;
		_exit(errno == ENODEV ? 0 : 1);
	}
	return pid;
}

/* Checks the open descriptor after the device is gone, then releases it. */
static void finish_gone_device(int fd, pid_t reader)
{
	struct infnoise_stats stats;
	uint8_t buf[64];
	int status;
	ssize_t n;

	waitpid(reader, &status, 0);
	n = read(fd, buf, sizeof(buf));
	if (n != -1 || errno != ENODEV) {
		FAIL("read on the old descriptor did not return ENODEV");
	} else if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		FAIL("blocked reader did not get ENODEV");
	} else if (ioctl(fd, INFNOISE_GET_STATS, &stats) < 0) {
		FAIL("GET_STATS on the old descriptor failed");
	} else {
		PASS();
	}
	close(fd);
}

static void test_unbind_while_open(void)
{
	char intf[64];
	pid_t reader;
	int fd;

	TEST("unbind with an open descriptor and a blocked reader");

	if (find_interface(intf, sizeof(intf)) < 0) {
		FAIL("cannot find the interface in sysfs");
		return;
	}
	fd = open(dev_path, O_RDONLY);
	if (fd < 0) {
		FAIL(strerror(errno));
		return;
	}
	reader = start_reader(fd);
	usleep(500000);

	if (write_file(DRIVER_SYSFS "/unbind", intf) < 0) {
		FAIL(strerror(errno));
		kill(reader, SIGKILL);
		waitpid(reader, NULL, 0);
		close(fd);
		return;
	}
	finish_gone_device(fd, reader);

	TEST("device comes back after rebind");
	if (write_file(DRIVER_SYSFS "/bind", intf) < 0 || wait_for_device() < 0)
		FAIL("no data after rebind");
	else
		PASS();
}

static int read_sysfs_int(const char *intf, const char *attr)
{
	char path[128];
	int value = 0;
	FILE *f;

	snprintf(path, sizeof(path), "/sys/bus/usb/devices/%s/%s", intf, attr);
	f = fopen(path, "r");
	if (!f)
		return 0;
	if (fscanf(f, "%d", &value) != 1)
		value = 0;
	fclose(f);
	return value;
}

static void test_reset_while_open(void)
{
	char intf[64], node[64];
	pid_t reader;
	int fd, usb_fd;

	TEST("USB reset with an open descriptor and a blocked reader");

	if (find_interface(intf, sizeof(intf)) < 0) {
		FAIL("cannot find the interface in sysfs");
		return;
	}
	*strchr(intf, ':') = '\0';

	snprintf(node, sizeof(node), "/dev/bus/usb/%03d/%03d",
		 read_sysfs_int(intf, "busnum"), read_sysfs_int(intf, "devnum"));
	usb_fd = open(node, O_WRONLY);
	if (usb_fd < 0) {
		SKIP(strerror(errno));
		return;
	}

	fd = open(dev_path, O_RDONLY);
	if (fd < 0) {
		FAIL(strerror(errno));
		close(usb_fd);
		return;
	}
	reader = start_reader(fd);
	usleep(500000);

	if (ioctl(usb_fd, USBDEVFS_RESET) < 0) {
		FAIL(strerror(errno));
		kill(reader, SIGKILL);
		waitpid(reader, NULL, 0);
		close(fd);
		close(usb_fd);
		return;
	}
	close(usb_fd);
	finish_gone_device(fd, reader);

	TEST("device comes back after reset");
	if (wait_for_device() < 0)
		FAIL("no data after reset");
	else
		PASS();
}

static void test_suspend_while_open(void)
{
	int fd;

	TEST("suspend for 10 s with an open descriptor");

	fd = open(dev_path, O_RDONLY);
	if (fd < 0) {
		FAIL(strerror(errno));
		return;
	}
	if (system("rtcwake -m mem -s 10 >/dev/null") != 0) {
		SKIP("rtcwake failed");
	} else if (read_full(fd, 64) < 0) {
		FAIL(strerror(errno));
	} else {
		PASS();
	}
	close(fd);
}

/* ── Main ───────────────────────────────────────────────── */

int main(int argc, char **argv)
{
	int destructive = 0, suspend = 0;
	glob_t devices;
	int fd;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--destructive")) {
			destructive = 1;
		} else if (!strcmp(argv[i], "--suspend")) {
			suspend = 1;
		} else {
			fprintf(stderr, "Usage: %s [--destructive] [--suspend]\n", argv[0]);
			return 2;
		}
	}

	if (glob("/dev/infnoise*", 0, NULL, &devices) != 0) {
		fprintf(stderr, "No /dev/infnoise* device found\n");
		fprintf(stderr, "Is the module loaded? Try: sudo insmod infnoise.ko\n");
		return 1;
	}
	snprintf(dev_path, sizeof(dev_path), "%s", devices.gl_pathv[0]);
	globfree(&devices);

	printf("\nInfinite Noise TRNG Kernel Module — Test Harness\n");
	printf("=================================================\n\n");

	fd = open(dev_path, O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "Cannot open %s: %s\n", dev_path, strerror(errno));
		fprintf(stderr, "Is the module loaded? Try: sudo insmod infnoise.ko\n");
		return 1;
	}

	printf("--- Read interfaces ---\n");
	test_chardev_read(fd);
	test_hwrng_read();

	printf("\n--- Non-blocking I/O ---\n");
	test_nonblock();
	test_interruptible();

	printf("\n--- Ioctl interface ---\n");
	test_struct_size();
	test_ioctl_get_stats(fd);
	test_padding_zeroed(fd);
	test_ioctl_get_entropy(fd);
	test_ioctl_set_raw(fd);
	test_set_raw_needs_privilege(fd);
	test_ioctl_bad_cmd(fd);

	printf("\n--- Device lifecycle ---\n");
	test_open_close_reopen();
	test_concurrent_readers();

	close(fd);

	if (destructive) {
		printf("\n--- Destructive ---\n");
		test_unbind_while_open();
		test_reset_while_open();
	}
	if (suspend) {
		printf("\n--- Suspend ---\n");
		test_suspend_while_open();
	}

	printf("\n=================================================\n");
	printf("Results: %d passed, %d failed, %d skipped\n\n",
	       passed, failed, skipped);

	return failed > 0 ? 1 : 0;
}
