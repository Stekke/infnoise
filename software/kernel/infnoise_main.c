// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Infinite Noise TRNG kernel driver
 *
 * Copyright (C) 2024 Manuel Domke
 *
 * Based on the userspace driver by Bill Cox
 * Hardware design by Bill Cox
 *
 * This driver interfaces with the Infinite Noise TRNG USB device,
 * implements health monitoring, Keccak-1600 whitening, and registers
 * as an hwrng device to feed entropy to the kernel's random subsystem.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/usb.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/hw_random.h>
#include <linux/workqueue.h>
#include <linux/uaccess.h>
#include <linux/compat.h>
#include <linux/capability.h>
#include <linux/jiffies.h>
#include <linux/wait.h>
#include <linux/sched/signal.h>
#include <linux/delay.h>
#include "infnoise.h"

#define DRIVER_NAME	"infnoise"
#define DRIVER_VERSION	"1.0"
#define DRIVER_DESC	"Infinite Noise TRNG driver"

/* Module parameters */
static bool infnoise_debug;
module_param_named(debug, infnoise_debug, bool, 0644);
MODULE_PARM_DESC(debug, "Enable debug output");

static bool infnoise_raw_mode;
module_param_named(raw_mode, infnoise_raw_mode, bool, 0644);
MODULE_PARM_DESC(raw_mode, "Output raw (unwhitened) data on /dev/infnoiseN");

static unsigned int infnoise_multiplier = 1;
module_param_named(multiplier, infnoise_multiplier, uint, 0644);
MODULE_PARM_DESC(multiplier, "/dev/infnoiseN output multiplier (0=entropy only, 1-10=multiply output)");

/* USB device table - only matches reprogrammed devices */
static const struct usb_device_id infnoise_table[] = {
	{ USB_DEVICE(INFNOISE_VENDOR_ID, INFNOISE_PRODUCT_ID) },
	{ }
};
MODULE_DEVICE_TABLE(usb, infnoise_table);

/* Forward declarations */
static struct usb_driver infnoise_driver;

/*
 * FTDI USB control transfer helper
 */
static int ftdi_control(struct infnoise_device *dev, u8 request,
			u16 value, u16 index)
{
	return usb_control_msg(dev->udev,
			       usb_sndctrlpipe(dev->udev, 0),
			       request,
			       USB_TYPE_VENDOR | USB_RECIP_DEVICE | USB_DIR_OUT,
			       value, index, NULL, 0,
			       INFNOISE_USB_TIMEOUT);
}

/*
 * Discard samples the FTDI chip has not delivered yet
 */
static int infnoise_purge_rx(struct infnoise_device *dev)
{
	return ftdi_control(dev, FTDI_SIO_RESET, FTDI_SIO_RESET_PURGE_RX,
			    FTDI_INDEX_INTERFACE_A);
}

/*
 * Configure FTDI device for synchronous bit-bang mode
 */
static int infnoise_configure_ftdi(struct infnoise_device *dev)
{
	int ret;

	/* Reset device */
	ret = ftdi_control(dev, FTDI_SIO_RESET, FTDI_SIO_RESET_SIO,
			   FTDI_INDEX_INTERFACE_A);
	if (ret < 0) {
		dev_err(&dev->intf->dev, "Failed to reset device: %d\n", ret);
		return ret;
	}

	/* Set latency timer to 1ms for faster response */
	ret = ftdi_control(dev, FTDI_SIO_SET_LATENCY_TIMER, 1,
			   FTDI_INDEX_INTERFACE_A);
	if (ret < 0) {
		dev_err(&dev->intf->dev, "Failed to set latency timer: %d\n", ret);
		return ret;
	}

	/*
	 * Set baud rate to 30000
	 * FTDI baud rate encoding: value = 3000000 / baud
	 * For 30000 baud: value = 100 = 0x0064
	 */
	ret = ftdi_control(dev, FTDI_SIO_SET_BAUDRATE, 0x0064,
			   FTDI_INDEX_INTERFACE_A);
	if (ret < 0) {
		dev_err(&dev->intf->dev, "Failed to set baud rate: %d\n", ret);
		return ret;
	}

	/*
	 * Enable synchronous bit-bang mode
	 * Mode byte in high byte of value, mask in low byte
	 * Mode 0x04 = SYNCBB, Mask 0xEB = all outputs except COMP1/COMP2
	 */
	ret = ftdi_control(dev, FTDI_SIO_SET_BITMODE,
			   (FTDI_BITMODE_SYNCBB << 8) | INFNOISE_MASK,
			   FTDI_INDEX_INTERFACE_A);
	if (ret < 0) {
		dev_err(&dev->intf->dev, "Failed to set bit-bang mode: %d\n", ret);
		return ret;
	}

	/* Purge RX buffer */
	ret = infnoise_purge_rx(dev);
	if (ret < 0) {
		dev_err(&dev->intf->dev, "Failed to purge RX: %d\n", ret);
		return ret;
	}

	/* Purge TX buffer */
	ret = ftdi_control(dev, FTDI_SIO_RESET, FTDI_SIO_RESET_PURGE_TX,
			   FTDI_INDEX_INTERFACE_A);
	if (ret < 0) {
		dev_err(&dev->intf->dev, "Failed to purge TX: %d\n", ret);
		return ret;
	}

	/* Give the device time to settle after configuration */
	msleep(50);

	return 0;
}

/*
 * Test USB transfer to verify device is working
 * Similar to what libftdi does during initialization
 */
static int infnoise_test_transfer(struct infnoise_device *dev)
{
	u8 *test_buf;
	int ret, actual;

	/* Allocate DMA-capable buffer */
	test_buf = kmalloc(64, GFP_KERNEL);
	if (!test_buf)
		return -ENOMEM;

	memset(test_buf, 0, 64);

	/* Clear any halt condition on endpoints */
	usb_clear_halt(dev->udev, usb_sndbulkpipe(dev->udev, dev->bulk_out_ep));
	usb_clear_halt(dev->udev, usb_rcvbulkpipe(dev->udev, dev->bulk_in_ep));

	/* Test write */
	ret = usb_bulk_msg(dev->udev,
			   usb_sndbulkpipe(dev->udev, dev->bulk_out_ep),
			   test_buf, 64,
			   &actual, INFNOISE_USB_TIMEOUT);
	if (ret < 0) {
		dev_err(&dev->intf->dev, "Test write failed: %d\n", ret);
		goto out;
	}
	if (actual != 64) {
		dev_err(&dev->intf->dev, "Short test write: %d/64\n", actual);
		ret = -EIO;
		goto out;
	}

	/* Test read */
	ret = usb_bulk_msg(dev->udev,
			   usb_rcvbulkpipe(dev->udev, dev->bulk_in_ep),
			   test_buf, 64,
			   &actual, INFNOISE_USB_TIMEOUT);
	if (ret < 0) {
		dev_err(&dev->intf->dev, "Test read failed: %d\n", ret);
		goto out;
	}
	if (actual < FTDI_STATUS_SIZE) {
		dev_err(&dev->intf->dev, "Short test read: %d bytes\n", actual);
		ret = -EIO;
		goto out;
	}

	/*
	 * The rest of the test samples must not enter the first real transfer,
	 * because they would shift the even/odd sample phase.
	 */
	msleep(INFNOISE_RETRY_DELAY_MS);
	ret = infnoise_purge_rx(dev);
	if (ret < 0) {
		dev_err(&dev->intf->dev, "Failed to purge RX: %d\n", ret);
		goto out;
	}

	dev_info(&dev->intf->dev, "Test transfer OK (%d bytes)\n", actual);
	ret = 0;
out:
	kfree(test_buf);
	return ret;
}

/*
 * Stop using the device and reset it from outside the read path
 *
 * A reset unbinds this driver, so it must not run on a thread that uses the
 * device. The USB core calls infnoise_disconnect() and then probes again. The
 * core drops the reset if it can't get the device lock, so the flag stays set
 * and infnoise_read_data() queues the reset again. The caller must hold
 * dev->lock.
 */
static void infnoise_queue_reset(struct infnoise_device *dev)
{
	set_bit(INFNOISE_RESET_PENDING, &dev->flags);
	wake_up_all(&dev->warmup_wq);
	usb_queue_reset_device(dev->intf);
}

/*
 * Attempt to recover from USB errors
 *
 * This function tries to restore communication with the device after
 * consecutive USB errors. It clears endpoint halts and reconfigures
 * the FTDI chip. If that fails, it queues a full USB device reset.
 *
 * Returns 0 on successful recovery, negative error code on failure.
 */
static int infnoise_try_recovery(struct infnoise_device *dev)
{
	int ret;

	if (!test_bit(INFNOISE_PRESENT, &dev->flags))
		return -ENODEV;

	dev_warn(&dev->intf->dev, "Attempting error recovery...\n");

	/* Clear any halt conditions on endpoints */
	usb_clear_halt(dev->udev, usb_sndbulkpipe(dev->udev, dev->bulk_out_ep));
	usb_clear_halt(dev->udev, usb_rcvbulkpipe(dev->udev, dev->bulk_in_ep));

	/* Small delay to let things settle */
	msleep(INFNOISE_RETRY_DELAY_MS);

	/* Try to reconfigure the FTDI device */
	ret = infnoise_configure_ftdi(dev);
	if (ret < 0) {
		dev_warn(&dev->intf->dev,
			 "FTDI reconfiguration failed (%d), queueing USB reset\n", ret);

		/* Last resort: reset the USB device */
		infnoise_queue_reset(dev);
		return -ENODEV;
	}

	dev->recoveries++;
	dev->consecutive_errors = 0;
	dev_info(&dev->intf->dev, "Recovery successful (total recoveries: %u)\n",
		 dev->recoveries);

	return 0;
}

/*
 * Prepare the clock buffer with alternating SWEN1/SWEN2 patterns
 */
static void infnoise_prepare_clock_buffer(struct infnoise_device *dev)
{
	unsigned int i;

	for (i = 0; i < INFNOISE_BUFLEN; i += 2) {
		dev->clock_buf[i] = (1 << INFNOISE_SWEN1);
		dev->clock_buf[i + 1] = (1 << INFNOISE_SWEN2);
	}
}

/*
 * Extract entropy bits from raw USB data
 *
 * In synchronous bit-bang mode, each byte read back contains the state
 * of all pins. We extract one bit per byte from COMP1/COMP2 alternating.
 */
static int infnoise_extract_bytes(struct infnoise_device *dev, u8 *bytes,
				  size_t length, const u8 *in_buf)
{
	unsigned int i, j;

	infnoise_health_clear_entropy(&dev->health);

	for (i = 0; i < length; i++) {
		u8 byte = 0;

		for (j = 0; j < 8; j++) {
			u8 val = in_buf[i * 8 + j];
			bool even_bit = (val >> INFNOISE_COMP2) & 1;
			bool odd_bit = (val >> INFNOISE_COMP1) & 1;
			bool even = j & 1; /* Use even bit if j is odd */
			u8 bit = even ? even_bit : odd_bit;

			byte = (byte << 1) | bit;

			/* Feed bit to health checker */
			if (!infnoise_health_add_bit(&dev->health, even_bit,
						     odd_bit, even)) {
				dev_err_ratelimited(&dev->intf->dev,
						    "Health check failed!\n");
				return -EBADMSG;
			}
		}
		bytes[i] = byte;
	}

	return infnoise_health_get_entropy(&dev->health);
}

/*
 * Strip FTDI modem status bytes from one bulk transfer
 *
 * FTDI sends data in 64-byte packets: 2 status bytes + 62 data bytes.
 * Each transfer starts on a packet boundary, and only its last packet can be
 * short. This function appends the data bytes to read_buf and returns the new
 * number of data bytes held there.
 */
static int infnoise_strip_ftdi_status(struct infnoise_device *dev,
				      int usb_bytes, int data_bytes)
{
	int offset;

	for (offset = 0; offset < usb_bytes; offset += FTDI_PACKET_SIZE) {
		int packet = min(usb_bytes - offset, FTDI_PACKET_SIZE);
		int copy_len = min(packet - FTDI_STATUS_SIZE,
				   INFNOISE_BUFLEN - data_bytes);

		if (copy_len > 0) {
			memcpy(dev->read_buf + data_bytes,
			       dev->usb_buf + offset + FTDI_STATUS_SIZE,
			       copy_len);
			data_bytes += copy_len;
		}
	}

	return data_bytes;
}

/*
 * Perform a single USB transfer cycle (internal, no retry)
 *
 * Writes clock pattern, reads back samples, extracts bits.
 * Returns number of entropy bits, or negative error.
 */
static int infnoise_usb_transfer_once(struct infnoise_device *dev)
{
	int ret;
	int actual;
	int data_bytes = 0;
	unsigned long deadline;

	/* Write clock pattern */
	ret = usb_bulk_msg(dev->udev,
			   usb_sndbulkpipe(dev->udev, dev->bulk_out_ep),
			   dev->clock_buf, INFNOISE_BUFLEN,
			   &actual, INFNOISE_USB_TIMEOUT);
	if (ret < 0) {
		if (infnoise_debug)
			dev_dbg(&dev->intf->dev, "USB write failed: %d\n", ret);
		return ret;
	}

	if (actual != INFNOISE_BUFLEN) {
		dev_err(&dev->intf->dev, "Short write: %d/%d\n",
			actual, INFNOISE_BUFLEN);
		return -EIO;
	}

	/*
	 * Read samples - FTDI returns data in 64-byte packets with 2 status
	 * bytes per packet, and also sends status-only packets while it has no
	 * data. Packets are parsed per transfer, because a short transfer ends
	 * in the middle of the buffer. The deadline stops a device that only
	 * ever sends status packets.
	 */
	deadline = jiffies + msecs_to_jiffies(INFNOISE_USB_TIMEOUT);
	while (data_bytes < INFNOISE_BUFLEN) {
		ret = usb_bulk_msg(dev->udev,
				   usb_rcvbulkpipe(dev->udev, dev->bulk_in_ep),
				   dev->usb_buf, INFNOISE_USB_READ_SIZE,
				   &actual, INFNOISE_USB_TIMEOUT);
		if (ret < 0) {
			if (infnoise_debug)
				dev_dbg(&dev->intf->dev, "USB read failed: %d\n", ret);
			return ret;
		}

		data_bytes = infnoise_strip_ftdi_status(dev, actual, data_bytes);

		if (data_bytes < INFNOISE_BUFLEN &&
		    (actual == 0 || time_after(jiffies, deadline))) {
			if (infnoise_debug)
				dev_dbg(&dev->intf->dev, "USB read timeout, got %d/%d\n",
					data_bytes, INFNOISE_BUFLEN);
			return -ETIMEDOUT;
		}
	}

	/* Extract entropy bits */
	return infnoise_extract_bytes(dev, dev->out_buf, INFNOISE_BYTES_OUT,
				      dev->read_buf);
}

/*
 * Check if an error is potentially recoverable
 */
static bool infnoise_is_recoverable_error(int err)
{
	switch (err) {
	case -EPIPE:		/* Endpoint stalled */
	case -ETIMEDOUT:	/* Timeout */
	case -EPROTO:		/* Protocol error */
	case -EILSEQ:		/* CRC/bit stuff error */
	case -EOVERFLOW:	/* Babble/overflow */
	case -EIO:		/* I/O error */
		return true;
	case -ENODEV:		/* Device disconnected */
	case -ESHUTDOWN:	/* Device shutting down */
	case -ENOENT:		/* URB cancelled */
		return false;
	default:
		return false;
	}
}

/*
 * Perform USB transfer with retry and recovery
 *
 * Attempts the transfer up to INFNOISE_MAX_RETRIES times. If consecutive
 * errors exceed INFNOISE_RECOVERY_THRESHOLD, attempts device recovery.
 * Returns number of entropy bits, or negative error.
 */
static int infnoise_usb_transfer(struct infnoise_device *dev)
{
	int ret;
	int retry;

	for (retry = 0; retry < INFNOISE_MAX_RETRIES; retry++) {
		/*
		 * Let the clocks of a failed attempt finish, also one of an
		 * earlier call. Its samples would shift the phase.
		 */
		if (dev->consecutive_errors) {
			msleep(INFNOISE_RETRY_DELAY_MS);
			infnoise_purge_rx(dev);
		}

		ret = infnoise_usb_transfer_once(dev);

		/* A health fault is about the samples, not the USB transfer */
		if (ret >= 0 || ret == -EBADMSG) {
			/* Success - reset consecutive error counter */
			dev->consecutive_errors = 0;
			return ret;
		}

		/* Track errors */
		dev->usb_errors++;
		dev->consecutive_errors++;

		/* Check if error is recoverable */
		if (!infnoise_is_recoverable_error(ret)) {
			if (infnoise_debug)
				dev_dbg(&dev->intf->dev,
					"Non-recoverable error: %d\n", ret);
			/*
			 * The USB core disables the endpoints before it calls
			 * infnoise_disconnect(), so these mean the device is going.
			 */
			if (ret == -ESHUTDOWN || ret == -ENOENT || ret == -EINVAL)
				return -ENODEV;
			return ret;
		}

		/* Attempt recovery if we've hit the threshold */
		if (dev->consecutive_errors >= INFNOISE_RECOVERY_THRESHOLD) {
			int recovery_ret = infnoise_try_recovery(dev);

			if (recovery_ret < 0) {
				dev_err(&dev->intf->dev,
					"Recovery failed: %d\n", recovery_ret);
				return recovery_ret;
			}
			/* Recovery succeeded, retry counter continues */
		}
	}

	dev_err(&dev->intf->dev, "Transfer failed after %d retries, last error: %d\n",
		INFNOISE_MAX_RETRIES, ret);
	return ret;
}

/*
 * Squeeze whitened data from Keccak sponge
 *
 * Extracts up to 128 bytes per call. If more data is needed,
 * performs a permutation and can be called again.
 * Returns number of bytes extracted.
 */
static int infnoise_keccak_squeeze(struct infnoise_device *dev, u8 *result,
				   size_t max_len)
{
	size_t out_bytes;

	/* Extract at most 128 bytes (16 lanes) for security margin */
	out_bytes = min_t(size_t, max_len, 128);
	infnoise_keccak_extract(&dev->keccak, result, out_bytes);

	/* Permute state for next extraction */
	infnoise_keccak_permutation(&dev->keccak);

	return out_bytes;
}

/*
 * Read random data from the device
 *
 * This is the main data path used by both hwrng and character device.
 * Supports multiplier for increased output rate from Keccak sponge.
 * The caller must hold dev->lock. Returns the number of bytes read, 0 if data
 * was discarded, -EBADMSG on a health fault, or another negative error.
 */
static int infnoise_read_data(struct infnoise_device *dev, u8 *result,
			      size_t max_len, bool raw, unsigned int mult)
{
	int entropy;
	size_t out_bytes;

	/* Check device is still present */
	if (!test_bit(INFNOISE_PRESENT, &dev->flags))
		return -ENODEV;

	if (test_bit(INFNOISE_SUSPENDED, &dev->flags))
		return 0;

	/* This does nothing if the reset is still queued */
	if (test_bit(INFNOISE_RESET_PENDING, &dev->flags)) {
		usb_queue_reset_device(dev->intf);
		return -ENODEV;
	}

	/*
	 * If we have bytes left from a previous absorption (multiplier > 1),
	 * squeeze more data without doing a new USB transfer.
	 */
	if (dev->keccak_bytes_left > 0 && !raw) {
		out_bytes = min_t(size_t, max_len, dev->keccak_bytes_left);
		out_bytes = infnoise_keccak_squeeze(dev, result, out_bytes);
		dev->keccak_bytes_left -= out_bytes;
		dev->bytes_generated += out_bytes;
		return out_bytes;
	}

	/* Perform USB transfer to get new entropy */
	entropy = infnoise_usb_transfer(dev);
	if (entropy < 0)
		return entropy;

	/* Check health status */
	if (!infnoise_health_ok(&dev->health)) {
		if (infnoise_debug)
			dev_dbg(&dev->intf->dev, "Health check not OK yet\n");
		return 0; /* Discard data but don't error */
	}

	/* Check entropy meets target */
	if (!infnoise_health_entropy_on_target(&dev->health, entropy,
					       INFNOISE_BUFLEN)) {
		if (infnoise_debug)
			dev_dbg(&dev->intf->dev, "Entropy below target\n");
		return 0; /* Discard data */
	}

	/* Return raw data if requested */
	if (raw) {
		out_bytes = min_t(size_t, max_len, INFNOISE_BYTES_OUT);
		memcpy(result, dev->out_buf, out_bytes);
		dev->bytes_generated += out_bytes;
		return out_bytes;
	}

	/* Absorb raw data into Keccak sponge (64 bytes = 8 lanes) */
	infnoise_keccak_absorb(&dev->keccak, dev->out_buf, INFNOISE_BYTES_OUT / 8);

	/* Calculate total bytes to output based on multiplier */
	if (mult == 0) {
		/* Output only entropy bits worth of data */
		out_bytes = entropy / 8;
	} else {
		/* Output multiplier * 32 bytes (256 bits per multiplier unit) */
		out_bytes = mult * 32;
	}

	/* Set up remaining bytes for subsequent reads */
	dev->keccak_bytes_left = out_bytes;

	/* Squeeze first chunk */
	out_bytes = min_t(size_t, max_len, dev->keccak_bytes_left);
	out_bytes = infnoise_keccak_squeeze(dev, result, out_bytes);
	dev->keccak_bytes_left -= out_bytes;
	dev->bytes_generated += out_bytes;

	return out_bytes;
}

/*
 * Warmup work function - runs in workqueue context
 *
 * Takes the lock per round, so that ioctls and disconnect wait for at most
 * one transfer.
 */
static void infnoise_warmup_work(struct work_struct *work)
{
	struct infnoise_device *dev = container_of(work, struct infnoise_device,
						   warmup_work);
	unsigned int rounds;
	unsigned int errors = 0;
	unsigned int faults = 0;
	bool ready = false;
	int ret = 0;
	u8 discard[64];

	dev_info(&dev->intf->dev, "Starting warmup...\n");

	for (rounds = 0; rounds < INFNOISE_WARMUP_ROUNDS; rounds++) {
		mutex_lock(&dev->lock);

		if (!test_bit(INFNOISE_PRESENT, &dev->flags) ||
		    test_bit(INFNOISE_SUSPENDED, &dev->flags)) {
			mutex_unlock(&dev->lock);
			goto out;
		}

		ready = infnoise_health_ok(&dev->health);
		if (!ready) {
			ret = infnoise_read_data(dev, discard, sizeof(discard),
						 true, 0);
			/* A few faults are normal, so start counting again */
			if (ret == -EBADMSG) {
				if (++faults >= INFNOISE_WARMUP_MAX_FAULTS) {
					mutex_unlock(&dev->lock);
					break;
				}
				infnoise_health_reset(&dev->health);
				ret = 0;
			}
		}

		mutex_unlock(&dev->lock);

		if (ready)
			break;

		if (ret >= 0) {
			errors = 0;
			continue;
		}

		/* Keep going on a temporary error, so that recovery can run */
		if (!infnoise_is_recoverable_error(ret) ||
		    ++errors >= INFNOISE_WARMUP_MAX_ERRORS)
			break;
	}

	if (ret == -ENODEV) {
		dev_info(&dev->intf->dev, "Device disconnected during warmup\n");
	} else if (ready) {
		dev_info(&dev->intf->dev,
			 "Warmup complete after %u rounds\n", rounds);
		set_bit(INFNOISE_WARMUP_DONE, &dev->flags);
	} else {
		dev_err(&dev->intf->dev,
			"Warmup failed after %u rounds: %d\n", rounds, ret);
		set_bit(INFNOISE_HEALTH_FAIL, &dev->flags);
	}

out:
	memzero_explicit(discard, sizeof(discard));
	wake_up_all(&dev->warmup_wq);
}

/*
 * Reset health state and warm up again. The caller must hold dev->lock.
 */
static void infnoise_restart_warmup(struct infnoise_device *dev)
{
	clear_bit(INFNOISE_WARMUP_DONE, &dev->flags);
	clear_bit(INFNOISE_HEALTH_FAIL, &dev->flags);
	infnoise_health_reset(&dev->health);
	dev->keccak_bytes_left = 0;
	queue_work(system_long_wq, &dev->warmup_work);
}

/*
 * Read for a consumer, and handle a health fault by warming up again, unless
 * the device keeps failing. The kernel credits what passes the checks, so a
 * failing device must stop. The caller must hold dev->lock.
 */
static int infnoise_read_checked(struct infnoise_device *dev, u8 *result,
				 size_t max_len, bool raw, unsigned int mult)
{
	int ret = infnoise_read_data(dev, result, max_len, raw, mult);

	if (ret == -EBADMSG) {
		if (time_after(jiffies, dev->last_health_fault +
			       INFNOISE_HEALTH_FORGET_SECS * HZ))
			dev->health_recoveries = 0;
		dev->last_health_fault = jiffies;

		if (++dev->health_recoveries > INFNOISE_MAX_HEALTH_RECOVERIES) {
			dev_err(&dev->intf->dev,
				"Too many health check failures, stopping\n");
			clear_bit(INFNOISE_WARMUP_DONE, &dev->flags);
			set_bit(INFNOISE_HEALTH_FAIL, &dev->flags);
			wake_up_all(&dev->warmup_wq);
			return -EIO;
		}
		infnoise_restart_warmup(dev);
		return -EIO;
	}

	return ret;
}

/* True when a reader no longer needs to wait for warmup */
static bool infnoise_warmup_settled(struct infnoise_device *dev)
{
	return test_bit(INFNOISE_WARMUP_DONE, &dev->flags) ||
	       test_bit(INFNOISE_HEALTH_FAIL, &dev->flags) ||
	       test_bit(INFNOISE_RESET_PENDING, &dev->flags) ||
	       !test_bit(INFNOISE_PRESENT, &dev->flags);
}

/* Error for a reader once warmup has settled */
static int infnoise_settled_status(struct infnoise_device *dev)
{
	if (!test_bit(INFNOISE_PRESENT, &dev->flags))
		return -ENODEV;

	if (test_bit(INFNOISE_HEALTH_FAIL, &dev->flags))
		return -EIO;

	return 0;
}

/* ============== hwrng interface ============== */

/*
 * The kernel credits this data at a fixed quality, so this interface ignores
 * the raw mode and multiplier settings of the character device. It also drops
 * bytes left over from a multiplied absorption, which would not earn that
 * credit. With wait=false, it makes one attempt if warmup is done and nobody
 * else holds the device, so it blocks for one transfer at most.
 */
static int infnoise_hwrng_read(struct hwrng *rng, void *data, size_t max,
			       bool wait)
{
	struct infnoise_device *dev = (struct infnoise_device *)rng->priv;
	unsigned int tries;
	long left;
	int ret;

	if (wait) {
		/* The core retries on 0, and the fill thread must not block forever */
		left = wait_event_interruptible_timeout(dev->warmup_wq,
							infnoise_warmup_settled(dev),
							msecs_to_jiffies(INFNOISE_HWRNG_WAIT_MS));
		if (left < 0)
			return left;
		if (left == 0)
			return 0;
	} else if (!infnoise_warmup_settled(dev)) {
		return 0;
	}

	ret = infnoise_settled_status(dev);
	if (ret)
		return ret;

	/* The core sleeps for 10 s after a return of 0, so retry discarded data */
	for (tries = 0; tries < (wait ? INFNOISE_HWRNG_TRIES : 1); tries++) {
		if (wait) {
			ret = mutex_lock_interruptible(&dev->lock);
			if (ret)
				return ret;
		} else if (!mutex_trylock(&dev->lock)) {
			return 0;
		}

		dev->keccak_bytes_left = 0;
		ret = infnoise_read_checked(dev, data, max, false, 1);
		mutex_unlock(&dev->lock);

		if (ret || signal_pending(current))
			return ret;
	}

	return 0;
}

static int infnoise_hwrng_register(struct infnoise_device *dev)
{
	int ret;

	snprintf(dev->hwrng_name, sizeof(dev->hwrng_name),
		 "infnoise-%s", dev_name(&dev->intf->dev));

	dev->hwrng.name = dev->hwrng_name;
	dev->hwrng.read = infnoise_hwrng_read;
	dev->hwrng.priv = (unsigned long)dev;
	dev->hwrng.quality = INFNOISE_QUALITY_WHITENED;

	ret = hwrng_register(&dev->hwrng);
	if (ret) {
		dev_err(&dev->intf->dev, "Failed to register hwrng: %d\n", ret);
		return ret;
	}

	set_bit(INFNOISE_HWRNG_REG, &dev->flags);
	dev_info(&dev->intf->dev, "Registered hwrng: %s (quality=%u)\n",
		 dev->hwrng_name, dev->hwrng.quality);

	return 0;
}

static void infnoise_hwrng_unregister(struct infnoise_device *dev)
{
	if (test_and_clear_bit(INFNOISE_HWRNG_REG, &dev->flags)) {
		hwrng_unregister(&dev->hwrng);
		dev_info(&dev->intf->dev, "Unregistered hwrng\n");
	}
}

/* ============== Character device interface ============== */

/*
 * Free the device when the last open file and the driver binding are gone
 */
static void infnoise_delete(struct kref *kref)
{
	struct infnoise_device *dev = container_of(kref, struct infnoise_device,
						   kref);

	infnoise_health_free(&dev->health);
	kfree(dev->clock_buf);
	kfree_sensitive(dev->usb_buf);
	kfree_sensitive(dev->read_buf);
	kfree_sensitive(dev->out_buf);
	usb_put_intf(dev->intf);
	usb_put_dev(dev->udev);
	kfree_sensitive(dev);
}

/*
 * The USB core holds minor_rwsem around this function, and
 * usb_deregister_dev() takes it for writing. So disconnect cannot drop its
 * reference until this function has returned.
 */
static int infnoise_open(struct inode *inode, struct file *file)
{
	struct infnoise_device *dev;
	struct usb_interface *intf;

	intf = usb_find_interface(&infnoise_driver, iminor(inode));
	if (!intf)
		return -ENODEV;

	dev = usb_get_intfdata(intf);
	if (!dev)
		return -ENODEV;

	if (!test_bit(INFNOISE_PRESENT, &dev->flags))
		return -ENODEV;

	kref_get(&dev->kref);
	file->private_data = dev;

	return 0;
}

static int infnoise_release(struct inode *inode, struct file *file)
{
	struct infnoise_device *dev = file->private_data;

	kref_put(&dev->kref, infnoise_delete);

	return 0;
}

static ssize_t infnoise_char_read(struct file *file, char __user *buf,
				  size_t count, loff_t *ppos)
{
	struct infnoise_device *dev = file->private_data;
	bool nonblock = file->f_flags & O_NONBLOCK;
	u8 data[128];
	size_t chunk;
	ssize_t total = 0;
	int ret = 0;

	if (!dev)
		return -ENODEV;

	while (total < count) {
		if (signal_pending(current)) {
			ret = -ERESTARTSYS;
			break;
		}

		/* Wait for warmup, which a health fault can start again */
		if (!infnoise_warmup_settled(dev)) {
			if (total > 0)
				break;
			if (nonblock) {
				ret = -EAGAIN;
				break;
			}

			ret = wait_event_interruptible(dev->warmup_wq,
						       infnoise_warmup_settled(dev));
			if (ret)
				break;
		}

		ret = infnoise_settled_status(dev);
		if (ret)
			break;

		chunk = min(count - total, sizeof(data));

		ret = mutex_lock_interruptible(&dev->lock);
		if (ret)
			break;
		ret = infnoise_read_checked(dev, data, chunk,
					    READ_ONCE(infnoise_raw_mode) || dev->raw_mode,
					    min(READ_ONCE(infnoise_multiplier), 10U));
		mutex_unlock(&dev->lock);

		if (ret < 0)
			break;

		if (ret == 0) {
			/* No data available yet (warmup / entropy below target) */
			if (total > 0)
				break;
			if (nonblock) {
				ret = -EAGAIN;
				break;
			}
			/* Yield CPU and back off to avoid busy-wait / soft lockup */
			msleep(INFNOISE_RETRY_DELAY_MS);
			continue;
		}

		if (copy_to_user(buf + total, data, ret)) {
			ret = -EFAULT;
			break;
		}

		total += ret;
		cond_resched();
	}

	memzero_explicit(data, sizeof(data));

	return total ? total : ret;
}

static long infnoise_ioctl(struct file *file, unsigned int cmd,
			   unsigned long arg)
{
	struct infnoise_device *dev = file->private_data;
	struct infnoise_stats stats = {};
	int raw;
	int ret;
	u32 entropy;

	if (!dev)
		return -ENODEV;

	switch (cmd) {
	case INFNOISE_GET_STATS:
		ret = mutex_lock_interruptible(&dev->lock);
		if (ret)
			return ret;
		infnoise_health_get_stats(&dev->health, &stats);
		mutex_unlock(&dev->lock);
		if (copy_to_user((void __user *)arg, &stats, sizeof(stats)))
			return -EFAULT;
		return 0;

	case INFNOISE_SET_RAW:
		/* The setting is shared by every reader of this device */
		if (!capable(CAP_SYS_ADMIN))
			return -EPERM;
		if (copy_from_user(&raw, (void __user *)arg, sizeof(raw)))
			return -EFAULT;
		dev->raw_mode = !!raw;
		return 0;

	case INFNOISE_GET_ENTROPY:
		ret = mutex_lock_interruptible(&dev->lock);
		if (ret)
			return ret;
		entropy = infnoise_health_get_entropy(&dev->health);
		mutex_unlock(&dev->lock);
		if (copy_to_user((void __user *)arg, &entropy, sizeof(entropy)))
			return -EFAULT;
		return 0;

	default:
		return -ENOTTY;
	}
}

static const struct file_operations infnoise_fops = {
	.owner		= THIS_MODULE,
	.read		= infnoise_char_read,
	.open		= infnoise_open,
	.release	= infnoise_release,
	.llseek		= noop_llseek,
	.unlocked_ioctl	= infnoise_ioctl,
	.compat_ioctl	= compat_ptr_ioctl,
};

static struct usb_class_driver infnoise_class = {
	.name		= "infnoise%d",
	.fops		= &infnoise_fops,
	.minor_base	= 192,
};

/* ============== USB probe/disconnect ============== */

static int infnoise_probe(struct usb_interface *intf,
			  const struct usb_device_id *id)
{
	struct usb_device *udev = interface_to_usbdev(intf);
	struct usb_endpoint_descriptor *bulk_in, *bulk_out;
	struct infnoise_device *dev;
	int ret;

	dev_info(&intf->dev, "Infinite Noise TRNG detected\n");

	/* Reset USB device to clear any stale state */
	ret = usb_reset_device(udev);
	if (ret) {
		dev_warn(&intf->dev, "USB reset failed: %d (continuing)\n", ret);
		/* Non-fatal, continue anyway */
	}

	/* Find bulk endpoints */
	ret = usb_find_common_endpoints(intf->cur_altsetting,
					&bulk_in, &bulk_out, NULL, NULL);
	if (ret) {
		dev_err(&intf->dev, "Could not find bulk endpoints\n");
		return ret;
	}

	/* Status bytes are parsed per 64-byte packet */
	if (usb_endpoint_maxp(bulk_in) != FTDI_PACKET_SIZE) {
		dev_err(&intf->dev, "Unexpected bulk IN packet size\n");
		return -ENODEV;
	}

	/* Allocate device structure */
	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	kref_init(&dev->kref);
	dev->udev = usb_get_dev(udev);
	dev->intf = usb_get_intf(intf);
	dev->bulk_in_ep = bulk_in->bEndpointAddress;
	dev->bulk_out_ep = bulk_out->bEndpointAddress;

	mutex_init(&dev->lock);
	init_waitqueue_head(&dev->warmup_wq);
	INIT_WORK(&dev->warmup_work, infnoise_warmup_work);

	/* Allocate buffers */
	dev->clock_buf = kmalloc(INFNOISE_BUFLEN, GFP_KERNEL);
	dev->usb_buf = kmalloc(INFNOISE_USB_READ_SIZE, GFP_KERNEL);
	dev->read_buf = kmalloc(INFNOISE_BUFLEN, GFP_KERNEL);
	dev->out_buf = kmalloc(INFNOISE_BYTES_OUT, GFP_KERNEL);

	if (!dev->clock_buf || !dev->usb_buf || !dev->read_buf || !dev->out_buf) {
		ret = -ENOMEM;
		goto err_put;
	}

	/* Initialize health check */
	ret = infnoise_health_init(&dev->health);
	if (ret)
		goto err_put;

	/* Initialize Keccak */
	infnoise_keccak_init(&dev->keccak);

	/* Prepare clock buffer */
	infnoise_prepare_clock_buffer(dev);

	/* Configure FTDI */
	ret = infnoise_configure_ftdi(dev);
	if (ret)
		goto err_put;

	/* Test transfer to prime the device */
	ret = infnoise_test_transfer(dev);
	if (ret)
		goto err_put;

	/* Mark device as present */
	set_bit(INFNOISE_PRESENT, &dev->flags);

	usb_set_intfdata(intf, dev);

	/* Register character device */
	ret = usb_register_dev(intf, &infnoise_class);
	if (ret) {
		dev_err(&intf->dev, "Could not register char device: %d\n", ret);
		usb_set_intfdata(intf, NULL);
		goto err_put;
	}
	dev_info(&intf->dev, "Registered /dev/%s\n", dev_name(intf->usb_dev));

	/* Register hwrng */
	ret = infnoise_hwrng_register(dev);
	if (ret)
		dev_warn(&intf->dev, "hwrng registration failed, continuing\n");

	/* Start warmup in workqueue */
	queue_work(system_long_wq, &dev->warmup_work);

	dev_info(&intf->dev, "Infinite Noise TRNG initialized\n");

	return 0;

err_put:
	kref_put(&dev->kref, infnoise_delete);
	return ret;
}

static void infnoise_disconnect(struct usb_interface *intf)
{
	struct infnoise_device *dev = usb_get_intfdata(intf);

	if (!dev)
		return;

	dev_info(&intf->dev, "Infinite Noise TRNG disconnected\n");

	/*
	 * Mark device as absent. Taking the lock waits for a transfer in
	 * progress, and later ones see the flag, so no USB I/O follows.
	 */
	mutex_lock(&dev->lock);
	clear_bit(INFNOISE_PRESENT, &dev->flags);
	clear_bit(INFNOISE_RESET_PENDING, &dev->flags);
	mutex_unlock(&dev->lock);

	/* Wake readers waiting for warmup */
	wake_up_all(&dev->warmup_wq);

	/* Cancel warmup work */
	cancel_work_sync(&dev->warmup_work);

	/* Unregister hwrng */
	infnoise_hwrng_unregister(dev);

	/* Unregister character device */
	usb_deregister_dev(intf, &infnoise_class);

	usb_set_intfdata(intf, NULL);

	/* Open files can still hold the device */
	kref_put(&dev->kref, infnoise_delete);
}

/*
 * The hwrng stays registered across system sleep. Readers wait for the warmup
 * that resume starts, and no USB I/O runs until then.
 */
static int infnoise_suspend(struct usb_interface *intf, pm_message_t message)
{
	struct infnoise_device *dev = usb_get_intfdata(intf);

	mutex_lock(&dev->lock);
	set_bit(INFNOISE_SUSPENDED, &dev->flags);
	mutex_unlock(&dev->lock);

	cancel_work_sync(&dev->warmup_work);
	clear_bit(INFNOISE_WARMUP_DONE, &dev->flags);

	return 0;
}

/*
 * The device can lose power during sleep, so configure it again in all cases.
 */
static int infnoise_resume(struct usb_interface *intf)
{
	struct infnoise_device *dev = usb_get_intfdata(intf);
	int ret;

	mutex_lock(&dev->lock);
	clear_bit(INFNOISE_SUSPENDED, &dev->flags);

	ret = infnoise_configure_ftdi(dev);
	if (ret < 0) {
		infnoise_queue_reset(dev);
	} else {
		clear_bit(INFNOISE_RESET_PENDING, &dev->flags);
		dev->health_recoveries = 0;
		infnoise_restart_warmup(dev);
	}
	mutex_unlock(&dev->lock);

	return 0;
}

static struct usb_driver infnoise_driver = {
	.name		= DRIVER_NAME,
	.probe		= infnoise_probe,
	.disconnect	= infnoise_disconnect,
	.suspend	= infnoise_suspend,
	.resume		= infnoise_resume,
	.reset_resume	= infnoise_resume,
	.id_table	= infnoise_table,
	.supports_autosuspend = 0,  /* No autosuspend - device feeds entropy continuously */
};

module_usb_driver(infnoise_driver);

MODULE_AUTHOR("Manuel Domke");
MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_VERSION(DRIVER_VERSION);
MODULE_LICENSE("GPL");
