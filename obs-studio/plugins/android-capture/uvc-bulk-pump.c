/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "uvc-bulk-pump.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

#define XFER_COUNT UVC_BULK_IN_FLIGHT
#define BUFFER_COUNT (XFER_COUNT * 2)
#define CHUNK_SIZE UVC_BULK_CHUNK_SIZE
#define ERROR_LIMIT 20U
#define BAD_SECONDS_LIMIT 5U

struct slot {
	struct libusb_transfer *x;
	struct uvc_bulk_pump *pump;
	bool active, needs_retry;
};
struct ready_block {
	unsigned buffer;
	size_t size;
	uint64_t seq, ns;
};
struct uvc_bulk_pump {
	libusb_context *ctx;
	libusb_device_handle *handle;
	uint8_t endpoint;
	pthread_t thread;
	pthread_mutex_t mutex;
	pthread_cond_t cond;
	bool thread_started, init_done, stopping, monotonic_cond;
	int fatal;
	unsigned active, waiters;
	struct slot slots[XFER_COUNT];
	uint8_t *buffers[BUFFER_COUNT];
	unsigned free_ids[BUFFER_COUNT], free_count;
	struct ready_block ready[XFER_COUNT];
	unsigned ready_head, ready_count;
	int borrowed;
	uint64_t sequence, win_start;
	unsigned win_errors, bad_seconds;
	struct uvc_bulk_stats stats;
};

static uint64_t now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}
/* Protected by mutex. The buffer identities never change during a session. */
static unsigned buffer_id(struct uvc_bulk_pump *p, const uint8_t *data)
{
	for (unsigned i = 0; i < BUFFER_COUNT; i++)
		if (p->buffers[i] == data)
			return i;
	abort();
}
static void set_fatal_locked(struct uvc_bulk_pump *p, int err)
{
	if (!p->fatal)
		p->fatal = err ? err : LIBUSB_ERROR_OTHER;
	pthread_cond_broadcast(&p->cond);
}
/* The mutex serializes submit with callbacks, including callbacks driven by a
 * concurrent libusb synchronous control transfer on another thread. */
static void submit_slot_locked(struct slot *slot, uint64_t started)
{
	struct uvc_bulk_pump *p = slot->pump;
	int r = libusb_submit_transfer(slot->x);
	uint64_t elapsed = now_ns() - started;
	if (r)
		set_fatal_locked(p, r);
	else {
		slot->active = true;
		p->active++;
	}
	if (elapsed > p->stats.max_resubmit_ns)
		p->stats.max_resubmit_ns = elapsed;
}
static void LIBUSB_CALL on_transfer(struct libusb_transfer *x)
{
	struct slot *slot = x->user_data;
	struct uvc_bulk_pump *p = slot->pump;
	uint64_t started = now_ns();
	bool submit = false;
	pthread_mutex_lock(&p->mutex);
	slot->active = false;
	p->active--;
	if (p->stopping || p->fatal) {
		pthread_mutex_unlock(&p->mutex);
		return;
	}
	if (x->status == LIBUSB_TRANSFER_COMPLETED) {
		p->stats.transfers++;
		if (x->actual_length > 0) {
			unsigned replacement;
			p->stats.received_bytes += (uint64_t)x->actual_length;
			if (p->free_count)
				replacement = p->free_ids[--p->free_count];
			else {
				/* All spares are ready/borrowed: discard the oldest unborrowed block. */
				struct ready_block *old = &p->ready[p->ready_head];
				replacement = old->buffer;
				p->stats.dropped_blocks++;
				p->stats.dropped_bytes += old->size;
				p->ready_head = (p->ready_head + 1) % XFER_COUNT;
				p->ready_count--;
			}
			unsigned tail = (p->ready_head + p->ready_count) % XFER_COUNT;
			p->ready[tail] = (struct ready_block){buffer_id(p, x->buffer), (size_t)x->actual_length,
							      p->sequence++, started};
			p->ready_count++;
			if (p->ready_count > p->stats.ready_high_water)
				p->stats.ready_high_water = p->ready_count;
			x->buffer = p->buffers[replacement];
			pthread_cond_signal(&p->cond);
		}
		submit = true;
	} else if (x->status != LIBUSB_TRANSFER_CANCELLED) {
		p->sequence++; /* Any error may have lost bytes, including a partial transfer. */
		p->stats.errors++;
		p->win_errors++;
		if (x->status == LIBUSB_TRANSFER_NO_DEVICE)
			set_fatal_locked(p, LIBUSB_ERROR_NO_DEVICE);
		else
			slot->needs_retry = true;
	}
	if (submit)
		submit_slot_locked(slot, started);
	pthread_mutex_unlock(&p->mutex);
}
static void *pump_thread(void *arg)
{
	struct uvc_bulk_pump *p = arg;
	int r = 0;
	pthread_mutex_lock(&p->mutex);
	for (unsigned i = 0; i < XFER_COUNT; i++) {
		r = libusb_submit_transfer(p->slots[i].x);
		if (r)
			break;
		p->slots[i].active = true;
		p->active++;
	}
	if (r)
		set_fatal_locked(p, r);
	p->init_done = true;
	pthread_cond_broadcast(&p->cond);
	pthread_mutex_unlock(&p->mutex);
	for (;;) {
		pthread_mutex_lock(&p->mutex);
		bool stop = p->stopping || p->fatal;
		pthread_mutex_unlock(&p->mutex);
		if (stop)
			break;
		struct timeval tv = {0, 10000};
		r = libusb_handle_events_timeout(p->ctx, &tv);
		if (r && r != LIBUSB_ERROR_INTERRUPTED) {
			pthread_mutex_lock(&p->mutex);
			set_fatal_locked(p, r);
			pthread_mutex_unlock(&p->mutex);
			continue;
		}
		for (unsigned i = 0; i < XFER_COUNT; i++) {
			struct slot *slot = &p->slots[i];
			pthread_mutex_lock(&p->mutex);
			bool retry = slot->needs_retry && !p->stopping && !p->fatal;
			bool stall = retry && slot->x->status == LIBUSB_TRANSFER_STALL;
			if (retry)
				slot->needs_retry = false;
			pthread_mutex_unlock(&p->mutex);
			if (!retry)
				continue;
			r = stall ? libusb_clear_halt(p->handle, p->endpoint) : 0;
			pthread_mutex_lock(&p->mutex);
			if (r)
				set_fatal_locked(p, r);
			else if (!p->stopping && !p->fatal)
				submit_slot_locked(slot, now_ns());
			pthread_mutex_unlock(&p->mutex);
		}
		uint64_t now = now_ns();
		if (now - p->win_start >= 1000000000ULL) {
			pthread_mutex_lock(&p->mutex);
			p->bad_seconds = p->win_errors > ERROR_LIMIT ? p->bad_seconds + 1 : 0;
			p->win_errors = 0;
			p->win_start = now;
			if (p->bad_seconds >= BAD_SECONDS_LIMIT)
				set_fatal_locked(p, LIBUSB_ERROR_IO);
			pthread_mutex_unlock(&p->mutex);
		}
	}
	/* Only the event thread cancels and drains; callbacks own active accounting.
  * Never free a submitted transfer until its completion/cancellation callback runs. */
	pthread_mutex_lock(&p->mutex);
	for (unsigned i = 0; i < XFER_COUNT; i++)
		if (p->slots[i].active)
			libusb_cancel_transfer(p->slots[i].x);
	pthread_mutex_unlock(&p->mutex);
	for (;;) {
		pthread_mutex_lock(&p->mutex);
		unsigned active = p->active;
		pthread_mutex_unlock(&p->mutex);
		if (!active)
			break;
		struct timeval tv = {0, 10000};
		libusb_handle_events_timeout(p->ctx, &tv);
	}
	/* Synchronize with callbacks that may have been dispatched by another
  * libusb event-handling thread but have not yet returned. */
	libusb_lock_events(p->ctx);
	libusb_unlock_events(p->ctx);
	pthread_mutex_lock(&p->mutex);
	pthread_cond_broadcast(&p->cond);
	pthread_mutex_unlock(&p->mutex);
	return NULL;
}
static void destroy_pump(struct uvc_bulk_pump *p)
{
	for (unsigned i = 0; i < XFER_COUNT; i++)
		if (p->slots[i].x)
			libusb_free_transfer(p->slots[i].x);
	for (unsigned i = 0; i < BUFFER_COUNT; i++)
		free(p->buffers[i]);
	pthread_cond_destroy(&p->cond);
	pthread_mutex_destroy(&p->mutex);
	free(p);
}
struct uvc_bulk_pump *uvc_bulk_pump_start_sized(libusb_context *ctx, libusb_device_handle *handle, uint8_t endpoint,
						unsigned transfer_size, int *error_out)
{
	int err = LIBUSB_ERROR_NO_MEM;
	if (!transfer_size || transfer_size > UVC_BULK_CHUNK_SIZE) {
		if (error_out)
			*error_out = LIBUSB_ERROR_INVALID_PARAM;
		return NULL;
	}
	struct uvc_bulk_pump *p = calloc(1, sizeof(*p));
	if (!p)
		goto fail_null;
	if (pthread_mutex_init(&p->mutex, NULL)) {
		free(p);
		goto fail_null;
	}
	pthread_condattr_t attr;
	bool have_attr = pthread_condattr_init(&attr) == 0;
	if (have_attr && pthread_condattr_setclock(&attr, CLOCK_MONOTONIC) == 0)
		p->monotonic_cond = true;
	int cond_err = pthread_cond_init(&p->cond, have_attr ? &attr : NULL);
	if (have_attr)
		pthread_condattr_destroy(&attr);
	if (cond_err) {
		pthread_mutex_destroy(&p->mutex);
		free(p);
		goto fail_null;
	}
	p->ctx = ctx;
	p->handle = handle;
	p->endpoint = endpoint;
	p->borrowed = -1;
	for (unsigned i = 0; i < BUFFER_COUNT; i++) {
		p->buffers[i] = malloc(CHUNK_SIZE);
		if (!p->buffers[i])
			goto fail;
	}
	for (unsigned i = XFER_COUNT; i < BUFFER_COUNT; i++)
		p->free_ids[p->free_count++] = i;
	for (unsigned i = 0; i < XFER_COUNT; i++) {
		struct slot *slot = &p->slots[i];
		slot->pump = p;
		slot->x = libusb_alloc_transfer(0);
		if (!slot->x)
			goto fail;
		libusb_fill_bulk_transfer(slot->x, handle, endpoint, p->buffers[i], (int)transfer_size, on_transfer,
					  slot, 0);
	}
	p->win_start = now_ns();
	if (pthread_create(&p->thread, NULL, pump_thread, p)) {
		err = LIBUSB_ERROR_OTHER;
		goto fail;
	}
	p->thread_started = true;
	pthread_mutex_lock(&p->mutex);
	while (!p->init_done)
		pthread_cond_wait(&p->cond, &p->mutex);
	err = p->fatal;
	pthread_mutex_unlock(&p->mutex);
	if (err) {
		pthread_join(p->thread, NULL);
		goto fail;
	}
	if (error_out)
		*error_out = 0;
	return p;
fail:
	destroy_pump(p);
fail_null:
	if (error_out)
		*error_out = err;
	return NULL;
}
struct uvc_bulk_pump *uvc_bulk_pump_start(libusb_context *ctx, libusb_device_handle *handle, uint8_t endpoint,
					  int *error_out)
{
	return uvc_bulk_pump_start_sized(ctx, handle, endpoint, UVC_BULK_CHUNK_SIZE, error_out);
}
int uvc_bulk_pump_acquire(struct uvc_bulk_pump *p, struct uvc_bulk_block *block, unsigned timeout_ms)
{
	struct timespec until;
	clock_gettime(p->monotonic_cond ? CLOCK_MONOTONIC : CLOCK_REALTIME, &until);
	until.tv_sec += timeout_ms / 1000;
	until.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
	if (until.tv_nsec >= 1000000000L) {
		until.tv_nsec -= 1000000000L;
		until.tv_sec++;
	}
	pthread_mutex_lock(&p->mutex);
	p->waiters++;
	while (!p->stopping && !p->fatal && !p->ready_count) {
		if (pthread_cond_timedwait(&p->cond, &p->mutex, &until))
			break;
	}
	p->waiters--;
	if (p->stopping)
		pthread_cond_broadcast(&p->cond);
	if (p->stopping || p->fatal) {
		pthread_mutex_unlock(&p->mutex);
		return -1;
	}
	if (!p->ready_count || p->borrowed >= 0) {
		pthread_mutex_unlock(&p->mutex);
		return 0;
	}
	struct ready_block b = p->ready[p->ready_head];
	p->ready_head = (p->ready_head + 1) % XFER_COUNT;
	p->ready_count--;
	p->borrowed = (int)b.buffer;
	*block = (struct uvc_bulk_block){p->buffers[b.buffer], b.size, b.seq, b.ns};
	pthread_mutex_unlock(&p->mutex);
	return 1;
}
void uvc_bulk_pump_release(struct uvc_bulk_pump *p, struct uvc_bulk_block *block)
{
	pthread_mutex_lock(&p->mutex);
	if (p->borrowed >= 0 && block->data == p->buffers[p->borrowed]) {
		p->free_ids[p->free_count++] = (unsigned)p->borrowed;
		p->borrowed = -1;
		memset(block, 0, sizeof(*block));
	}
	pthread_mutex_unlock(&p->mutex);
}
int uvc_bulk_pump_error(struct uvc_bulk_pump *p)
{
	pthread_mutex_lock(&p->mutex);
	int e = p->fatal;
	pthread_mutex_unlock(&p->mutex);
	return e;
}
void uvc_bulk_pump_snapshot(struct uvc_bulk_pump *p, struct uvc_bulk_stats *stats)
{
	pthread_mutex_lock(&p->mutex);
	*stats = p->stats;
	pthread_mutex_unlock(&p->mutex);
}
void uvc_bulk_pump_stop(struct uvc_bulk_pump *p)
{
	if (!p)
		return;
	pthread_mutex_lock(&p->mutex);
	p->stopping = true;
	pthread_cond_broadcast(&p->cond);
	while (p->waiters)
		pthread_cond_wait(&p->cond, &p->mutex);
	pthread_mutex_unlock(&p->mutex);
	if (p->thread_started)
		pthread_join(p->thread, NULL);
	destroy_pump(p);
}
