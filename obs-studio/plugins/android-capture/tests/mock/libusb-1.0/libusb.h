/* SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once
#include <stdint.h>
#include <sys/time.h>
#define LIBUSB_CALL
#define LIBUSB_ERROR_IO -1
#define LIBUSB_ERROR_INVALID_PARAM -2
#define LIBUSB_ERROR_NO_DEVICE -4
#define LIBUSB_ERROR_NO_MEM -11
#define LIBUSB_ERROR_TIMEOUT -7
#define LIBUSB_ERROR_PIPE -9
#define LIBUSB_ERROR_OVERFLOW -8
#define LIBUSB_ERROR_INTERRUPTED -10
#define LIBUSB_ERROR_OTHER -99
typedef struct libusb_context {
	int unused;
} libusb_context;
typedef struct libusb_device_handle {
	int unused;
} libusb_device_handle;
enum libusb_transfer_status {
	LIBUSB_TRANSFER_COMPLETED,
	LIBUSB_TRANSFER_ERROR,
	LIBUSB_TRANSFER_TIMED_OUT,
	LIBUSB_TRANSFER_CANCELLED,
	LIBUSB_TRANSFER_STALL,
	LIBUSB_TRANSFER_NO_DEVICE,
	LIBUSB_TRANSFER_OVERFLOW
};
struct libusb_transfer {
	unsigned char *buffer;
	int actual_length;
	int length;
	enum libusb_transfer_status status;
	void *user_data;
	void (*callback)(struct libusb_transfer *);
	int active;
	int pending;
};
struct libusb_transfer *libusb_alloc_transfer(int n);
void libusb_free_transfer(struct libusb_transfer *x);
int libusb_submit_transfer(struct libusb_transfer *x);
int libusb_cancel_transfer(struct libusb_transfer *x);
int libusb_handle_events_timeout(libusb_context *ctx, struct timeval *tv);
int libusb_clear_halt(libusb_device_handle *h, unsigned char ep);
void libusb_lock_events(libusb_context *ctx);
void libusb_unlock_events(libusb_context *ctx);
static inline void libusb_fill_bulk_transfer(struct libusb_transfer *x, libusb_device_handle *h, unsigned char ep,
					     unsigned char *buf, int len, void (*cb)(struct libusb_transfer *),
					     void *user, unsigned timeout)
{
	(void)h;
	(void)ep;
	x->length = len;
	(void)timeout;
	x->buffer = buf;
	x->callback = cb;
	x->user_data = user;
}
