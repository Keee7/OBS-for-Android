/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "uvc-bulk-pump.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER, events = PTHREAD_MUTEX_INITIALIZER;
static struct libusb_transfer *all[32];
static unsigned count, submissions, fail_alloc_at, fail_submit_at, callbacks, cancellations;
static void nap(void)
{
	struct timespec ts = {0, 1000000};
	nanosleep(&ts, 0);
}
struct libusb_transfer *libusb_alloc_transfer(int n)
{
	(void)n;
	if (count == fail_alloc_at)
		return 0;
	struct libusb_transfer *x = calloc(1, sizeof(*x));
	all[count++] = x;
	return x;
}
void libusb_free_transfer(struct libusb_transfer *x)
{
	assert(!x->active && !x->pending);
	free(x);
}
int libusb_submit_transfer(struct libusb_transfer *x)
{
	pthread_mutex_lock(&lock);
	if (++submissions == fail_submit_at) {
		pthread_mutex_unlock(&lock);
		return LIBUSB_ERROR_IO;
	}
	assert(!x->active);
	x->active = 1;
	pthread_mutex_unlock(&lock);
	return 0;
}
int libusb_cancel_transfer(struct libusb_transfer *x)
{
	pthread_mutex_lock(&lock);
	if (x->active && !x->pending) {
		x->status = LIBUSB_TRANSFER_CANCELLED;
		x->pending = 1;
		cancellations++;
	}
	pthread_mutex_unlock(&lock);
	return 0;
}
void libusb_lock_events(libusb_context *ctx)
{
	(void)ctx;
	pthread_mutex_lock(&events);
}
void libusb_unlock_events(libusb_context *ctx)
{
	(void)ctx;
	pthread_mutex_unlock(&events);
}
int libusb_handle_events_timeout(libusb_context *ctx, struct timeval *tv)
{
	(void)ctx;
	(void)tv;
	pthread_mutex_lock(&events);
	pthread_mutex_lock(&lock);
	struct libusb_transfer *x = 0;
	for (unsigned i = 0; i < count; i++)
		if (all[i]->pending) {
			x = all[i];
			x->pending = 0;
			x->active = 0;
			break;
		}
	pthread_mutex_unlock(&lock);
	if (x) {
		x->callback(x);
		callbacks++;
	}
	pthread_mutex_unlock(&events);
	if (!x)
		nap();
	return 0;
}
int libusb_clear_halt(libusb_device_handle *h, unsigned char ep)
{
	(void)h;
	(void)ep;
	return 0;
}
static unsigned char *inject(unsigned i, enum libusb_transfer_status status, unsigned char byte)
{
	pthread_mutex_lock(&lock);
	struct libusb_transfer *x = all[i];
	assert(x->active && !x->pending);
	unsigned char *addr = x->buffer;
	addr[0] = byte;
	x->actual_length = 1;
	x->status = status;
	x->pending = 1;
	pthread_mutex_unlock(&lock);
	return addr;
}
static void wait_count(unsigned target)
{
	for (unsigned i = 0; i < 3000; i++) {
		pthread_mutex_lock(&lock);
		unsigned n = submissions;
		pthread_mutex_unlock(&lock);
		if (n >= target)
			return;
		nap();
	}
	assert(!"callback timeout");
}
static void *blocked_acquire(void *arg)
{
	struct uvc_bulk_block b;
	int *result = malloc(sizeof(*result));
	*result = uvc_bulk_pump_acquire(arg, &b, 30000);
	return result;
}
static void reset(void)
{
	count = submissions = callbacks = cancellations = 0;
	fail_alloc_at = fail_submit_at = 9999;
}
int main(void)
{
	reset();
	int err = 0;
	libusb_context ctx;
	libusb_device_handle dev;
	struct uvc_bulk_pump *p = uvc_bulk_pump_start(&ctx, &dev, 0x81, &err);
	assert(p && !err);
	unsigned char *first = inject(0, LIBUSB_TRANSFER_COMPLETED, 42);
	wait_count(33);
	struct uvc_bulk_block b;
	assert(uvc_bulk_pump_acquire(p, &b, 1000) == 1);
	assert(b.data == first && b.data[0] == 42 && b.sequence == 0);
	uvc_bulk_pump_release(p, &b);
	inject(0, LIBUSB_TRANSFER_ERROR, 0);
	wait_count(34);
	inject(0, LIBUSB_TRANSFER_COMPLETED, 43);
	wait_count(35);
	assert(uvc_bulk_pump_acquire(p, &b, 1000) == 1 && b.sequence == 2 && b.data[0] == 43);
	uvc_bulk_pump_release(p, &b);
	for (unsigned i = 0; i < 80; i++) {
		inject(0, LIBUSB_TRANSFER_COMPLETED, (unsigned char)i);
		wait_count(36 + i);
	}
	struct uvc_bulk_stats s;
	uvc_bulk_pump_snapshot(p, &s);
	assert(s.dropped_blocks == 16 && s.dropped_bytes == 16 && s.ready_capacity == 64 && s.ready_high_water == 64 && s.errors == 1);
	assert(uvc_bulk_pump_acquire(p, &b, 1000) == 1 && b.sequence == 19 && b.data[0] == 16);
	uvc_bulk_pump_release(p, &b);
	uvc_bulk_pump_stop(p);
	assert(cancellations == 32);
	reset();
	p = uvc_bulk_pump_start(&ctx, &dev, 0x81, &err);
	assert(p);
	enum libusb_transfer_status faults[] = {LIBUSB_TRANSFER_TIMED_OUT, LIBUSB_TRANSFER_STALL,
		LIBUSB_TRANSFER_OVERFLOW, LIBUSB_TRANSFER_ERROR};
	for (unsigned i = 0; i < 4; i++) {
		inject(0, faults[i], 0);
		wait_count(33 + i);
	}
	uvc_bulk_pump_snapshot(p, &s);
	assert(s.errors == 4 && s.timeouts == 1 && s.stalls == 1 && s.overflows == 1 && s.io_errors == 1);
	uvc_bulk_pump_stop(p);

	reset();
	p = uvc_bulk_pump_start(&ctx, &dev, 0x81, &err);
	assert(p);
	first = inject(0, LIBUSB_TRANSFER_COMPLETED, 77);
	wait_count(33);
	assert(uvc_bulk_pump_acquire(p, &b, 1000) == 1 && b.data == first);
	for (unsigned i = 0; i < 80; i++) {
		unsigned char *inflight = inject(0, LIBUSB_TRANSFER_COMPLETED, (unsigned char)i);
		assert(inflight != first);
		wait_count(34 + i);
		assert(b.data[0] == 77);
	}
	uvc_bulk_pump_snapshot(p, &s);
	assert(s.dropped_blocks == 17 && s.ready_high_water == 63);
	uvc_bulk_pump_release(p, &b);
	uvc_bulk_pump_stop(p);
	reset();
	p = uvc_bulk_pump_start_sized(&ctx, &dev, 0x81, 16384, &err);
	assert(p);
	first = inject(0, LIBUSB_TRANSFER_COMPLETED, 77);
	wait_count(33);
	assert(uvc_bulk_pump_acquire(p, &b, 1000) == 1 && b.data == first);
	for (unsigned i = 0; i < 280; i++) {
		unsigned char *inflight = inject(0, LIBUSB_TRANSFER_COMPLETED, (unsigned char)i);
		assert(inflight != first);
		wait_count(34 + i);
		assert(b.data[0] == 77);
	}
	uvc_bulk_pump_snapshot(p, &s);
	assert(s.ready_capacity == 256 && s.ready_high_water == 255 && s.dropped_blocks == 25);
	uvc_bulk_pump_release(p, &b);
	assert(uvc_bulk_pump_acquire(p, &b, 1000) == 1 && b.sequence == 26 && b.data[0] == 25);
	uvc_bulk_pump_release(p, &b);
	uvc_bulk_pump_stop(p);
	reset();
	p = uvc_bulk_pump_start_sized(&ctx, &dev, 0x81, 16384, &err);
	assert(p);
	for (unsigned i = 0; i < 270; i++) {
		inject(0, LIBUSB_TRANSFER_COMPLETED, (unsigned char)i);
		wait_count(33 + i);
	}
	uvc_bulk_pump_snapshot(p, &s);
	assert(s.ready_capacity == 256 && s.ready_high_water == 256 && s.dropped_blocks == 14);
	assert(uvc_bulk_pump_acquire(p, &b, 1000) == 1 && b.sequence == 14 && b.data[0] == 14);
	uvc_bulk_pump_release(p, &b);
	uvc_bulk_pump_stop(p);
	reset();
	p = uvc_bulk_pump_start(&ctx, &dev, 0x81, &err);
	assert(p);
	assert(uvc_bulk_pump_acquire(p, &b, 2) == 0);
	inject(0, LIBUSB_TRANSFER_COMPLETED, 10);
	wait_count(33);
	inject(0, LIBUSB_TRANSFER_NO_DEVICE, 0);
	for (unsigned i = 0; i < 3000 && !uvc_bulk_pump_error(p); i++)
		nap();
	assert(uvc_bulk_pump_error(p) == LIBUSB_ERROR_NO_DEVICE);
	assert(uvc_bulk_pump_acquire(p, &b, 100) == -1);
	uvc_bulk_pump_stop(p);
	reset();
	p = uvc_bulk_pump_start(&ctx, &dev, 0x81, &err);
	assert(p);
	pthread_t waiter;
	assert(!pthread_create(&waiter, 0, blocked_acquire, p));
	for (unsigned i = 0; i < 20; i++)
		nap();
	uvc_bulk_pump_stop(p);
	void *result;
	pthread_join(waiter, &result);
	assert(*(int *)result == -1);
	free(result);
	reset();
	assert(!uvc_bulk_pump_start_sized(&ctx, &dev, 0x81, 0, &err) && err == LIBUSB_ERROR_INVALID_PARAM);
	assert(!uvc_bulk_pump_start_sized(&ctx, &dev, 0x81, 65537, &err) && err == LIBUSB_ERROR_INVALID_PARAM);
	p = uvc_bulk_pump_start_sized(&ctx, &dev, 0x81, 4096, &err);
	assert(p);
	for (unsigned i = 0; i < 32; i++)
		assert(all[i]->length == 4096);
	uvc_bulk_pump_stop(p);
	/* Fixed-size payloads share one URB up to the 128 KiB batch target. */
	reset();
	assert(uvc_bulk_payload_transfer_size(0) == 0);
	assert(uvc_bulk_payload_transfer_size(65537) == 0);
	assert(uvc_bulk_payload_transfer_size(16384) == 131072);
	assert(uvc_bulk_payload_transfer_size(3072) == 129024);
	p = uvc_bulk_pump_start_payload(&ctx, &dev, 0x81, 16384, &err);
	assert(p);
	for (unsigned i = 0; i < 32; i++)
		assert(all[i]->length == 131072);
	struct uvc_bulk_stats payload_stats;
	uvc_bulk_pump_snapshot(p, &payload_stats);
	assert(payload_stats.ready_capacity == 32);
	uvc_bulk_pump_stop(p);
	reset();
	assert(!uvc_bulk_pump_start_payload(&ctx, &dev, 0x81, 65537, &err) && err == LIBUSB_ERROR_INVALID_PARAM);
	reset();
	fail_alloc_at = 5;
	assert(!uvc_bulk_pump_start(&ctx, &dev, 0x81, &err) && err == LIBUSB_ERROR_NO_MEM);
	reset();
	fail_submit_at = 5;
	assert(!uvc_bulk_pump_start(&ctx, &dev, 0x81, &err) && err == LIBUSB_ERROR_IO);
	assert(cancellations == 4);
	puts("uvc bulk pump tests passed");
	return 0;
}
