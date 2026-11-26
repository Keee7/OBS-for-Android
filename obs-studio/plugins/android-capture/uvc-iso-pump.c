/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "uvc-iso-pump.h"

#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#define XFER_COUNT UVC_ISO_IN_FLIGHT
#define READY_BUDGET (4U * 1024U * 1024U)
#define READY_MAX 128U
#define BUFFER_MAX (XFER_COUNT + READY_MAX)
#define MAX_ISO_PACKET_BYTES 98304U
#define ERROR_LIMIT 20U
#define BAD_SECONDS_LIMIT 2U
#define CONSEC_ERROR_LIMIT 16U
#define RETRY_BACKOFF_AFTER 4U
#define RETRY_BACKOFF_NS 5000000L
#define DRAIN_EVENT_ERROR_LIMIT 5U
#define DRAIN_TIMEOUT_NS 2000000000ULL

_Static_assert(XFER_COUNT <= 32U, "retry bitmap requires at most 32 slots");

struct slot {
	struct libusb_transfer *x;
	struct uvc_iso_pump *pump;
	unsigned buffer_id, index;
	bool active;
};

struct ready_block {
	unsigned buffer;
	size_t bytes;
	unsigned num_packets;
	struct uvc_iso_packet_desc packets[UVC_ISO_MAX_PACKETS];
	uint64_t seq, ns;
};

struct uvc_iso_pump {
	libusb_context *ctx;
	libusb_device_handle *handle;
	uint8_t endpoint;
	unsigned packet_size;
	unsigned packets_per_xfer;
	unsigned transfer_size;
	pthread_t thread;
	pthread_mutex_t mutex;
	pthread_cond_t cond;
	bool thread_started, init_done, stopping, monotonic_cond, abandoned, owner_held;
	int fatal;
	unsigned active, waiters;
	uint32_t retry_pending;
	struct slot slots[XFER_COUNT];
	uint8_t *buffers[BUFFER_MAX];
	unsigned free_ids[BUFFER_MAX], free_count;
	struct ready_block ready[READY_MAX];
	unsigned ready_head, ready_count, ready_capacity, buffer_count;
	int borrowed;
	uint64_t sequence, win_start;
	unsigned win_errors, bad_seconds, consec_errors;
	struct uvc_iso_stats stats;
};

static uint64_t now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void set_fatal_locked(struct uvc_iso_pump *p, int err)
{
	if (!p->fatal)
		p->fatal = err ? err : LIBUSB_ERROR_OTHER;
	pthread_cond_broadcast(&p->cond);
}

static void record_transfer_status_stat(struct uvc_iso_pump *p, int status)
{
	switch (status) {
	case LIBUSB_TRANSFER_TIMED_OUT:
		p->stats.timeouts++;
		break;
	case LIBUSB_TRANSFER_STALL:
		p->stats.stalls++;
		break;
	case LIBUSB_TRANSFER_OVERFLOW:
		p->stats.overflows++;
		break;
	case LIBUSB_TRANSFER_ERROR:
		p->stats.io_errors++;
		break;
	default:
		break;
	}
}

static void reset_iso_transfer(struct libusb_transfer *x, const struct uvc_iso_pump *p)
{
	x->length = (int)p->transfer_size;
	x->num_iso_packets = (int)p->packets_per_xfer;
	for (unsigned k = 0; k < p->packets_per_xfer; k++) {
		x->iso_packet_desc[k].length = p->packet_size;
		x->iso_packet_desc[k].actual_length = 0;
		x->iso_packet_desc[k].status = 0;
	}
}

static void submit_slot_locked(struct slot *slot, uint64_t started)
{
	struct uvc_iso_pump *p = slot->pump;
	const int r = libusb_submit_transfer(slot->x);
	const uint64_t elapsed = now_ns() - started;
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
	struct uvc_iso_pump *p = slot->pump;
	const uint64_t started = now_ns();
	bool submit = false;

	pthread_mutex_lock(&p->mutex);
	slot->active = false;
	p->active--;
	if (p->abandoned) {
		struct libusb_transfer *tx = slot->x;
		slot->x = NULL;
		uint8_t *buf = NULL;
		if (slot->buffer_id < p->buffer_count) {
			buf = p->buffers[slot->buffer_id];
			p->buffers[slot->buffer_id] = NULL;
		}
		const bool destroy_self = (p->active == 0 && !p->owner_held);
		if (p->active == 0)
			pthread_cond_broadcast(&p->cond);
		pthread_mutex_unlock(&p->mutex);
		if (tx)
			libusb_free_transfer(tx);
		free(buf);
		if (destroy_self) {
			pthread_cond_destroy(&p->cond);
			pthread_mutex_destroy(&p->mutex);
			free(p);
		}
		return;
	}
	if (p->stopping || p->fatal) {
		pthread_mutex_unlock(&p->mutex);
		return;
	}

	if (x->status == LIBUSB_TRANSFER_COMPLETED) {
		p->stats.transfers++;
		p->consec_errors = 0;

		struct uvc_iso_packet_desc pkts[UVC_ISO_MAX_PACKETS];
		unsigned n_pkts = 0;
		size_t urb_bytes = 0;
		unsigned np = x->num_iso_packets > 0 ? (unsigned)x->num_iso_packets : 0U;
		if (np > p->packets_per_xfer)
			np = p->packets_per_xfer;
		if (np > UVC_ISO_MAX_PACKETS)
			np = UVC_ISO_MAX_PACKETS;

		for (unsigned k = 0; k < np; k++) {
			const struct libusb_iso_packet_descriptor *d = &x->iso_packet_desc[k];
			if (d->status == LIBUSB_TRANSFER_NO_DEVICE) {
				set_fatal_locked(p, LIBUSB_ERROR_NO_DEVICE);
				pthread_mutex_unlock(&p->mutex);
				return;
			}
			const uint32_t offset = k * p->packet_size;
			if (d->status != LIBUSB_TRANSFER_COMPLETED) {
				p->stats.packet_errors++;
				record_transfer_status_stat(p, d->status);
				pkts[n_pkts++] = (struct uvc_iso_packet_desc){offset, 0, d->status};
				continue;
			}
			uint32_t act = d->actual_length <= d->length ? d->actual_length : d->length;
			if (offset >= p->transfer_size)
				act = 0;
			else if (act > p->transfer_size - offset)
				act = p->transfer_size - offset;
			if (act > 0) {
				p->stats.received_bytes += act;
				urb_bytes += act;
				pkts[n_pkts++] =
					(struct uvc_iso_packet_desc){offset, act, LIBUSB_TRANSFER_COMPLETED};
			}
		}

		if (n_pkts > 0) {
			unsigned replacement;
			if (p->free_count) {
				replacement = p->free_ids[--p->free_count];
			} else {
				struct ready_block *old = &p->ready[p->ready_head];
				replacement = old->buffer;
				p->stats.dropped_blocks++;
				p->stats.dropped_bytes += old->bytes;
				p->ready_head = (p->ready_head + 1) % p->ready_capacity;
				p->ready_count--;
			}
			const unsigned tail = (p->ready_head + p->ready_count) % p->ready_capacity;
			struct ready_block *dst = &p->ready[tail];
			dst->buffer = slot->buffer_id;
			dst->bytes = urb_bytes;
			dst->num_packets = n_pkts;
			memcpy(dst->packets, pkts, n_pkts * sizeof(pkts[0]));
			dst->seq = p->sequence++;
			dst->ns = started;
			p->ready_count++;
			if (p->ready_count > p->stats.ready_high_water)
				p->stats.ready_high_water = p->ready_count;

			x->buffer = p->buffers[replacement];
			slot->buffer_id = replacement;
			if (p->waiters)
				pthread_cond_signal(&p->cond);
		}
		reset_iso_transfer(x, p);
		submit = true;
	} else if (x->status != LIBUSB_TRANSFER_CANCELLED) {
		p->sequence++; /* Whole-transfer failure may have dropped ISO frames. */
		p->stats.errors++;
		record_transfer_status_stat(p, x->status);
		p->win_errors++;
		if (x->status == LIBUSB_TRANSFER_NO_DEVICE)
			set_fatal_locked(p, LIBUSB_ERROR_NO_DEVICE);
		else if (x->status != LIBUSB_TRANSFER_TIMED_OUT && ++p->consec_errors >= CONSEC_ERROR_LIMIT)
			set_fatal_locked(p, LIBUSB_ERROR_IO);
		else
			p->retry_pending |= UINT32_C(1) << slot->index;
	}

	if (submit)
		submit_slot_locked(slot, started);
	pthread_mutex_unlock(&p->mutex);
}

static void *pump_thread(void *arg)
{
	struct uvc_iso_pump *p = arg;
	int r = 0;

	pthread_mutex_lock(&p->mutex);
	for (unsigned i = 0; i < XFER_COUNT; i++) {
		r = libusb_submit_transfer(p->slots[i].x);
		if (r) {
			if (r == LIBUSB_ERROR_NO_MEM && p->active >= UVC_ISO_MIN_IN_FLIGHT) {
				r = 0;
				break;
			}
			break;
		}
		p->slots[i].active = true;
		p->active++;
	}
	p->stats.active_xfers = p->active;
	if (r)
		set_fatal_locked(p, r);
	p->init_done = true;
	pthread_cond_broadcast(&p->cond);
	pthread_mutex_unlock(&p->mutex);

	for (;;) {
		const uint64_t now = now_ns();
		pthread_mutex_lock(&p->mutex);
		if (r && r != LIBUSB_ERROR_INTERRUPTED)
			set_fatal_locked(p, r);
		if (now - p->win_start >= 1000000000ULL) {
			p->bad_seconds = p->win_errors > ERROR_LIMIT ? p->bad_seconds + 1 : 0;
			p->win_errors = 0;
			p->win_start = now;
			if (p->bad_seconds >= BAD_SECONDS_LIMIT)
				set_fatal_locked(p, LIBUSB_ERROR_IO);
		}
		const bool stop = p->stopping || p->fatal;
		uint32_t pending = p->retry_pending;
		const bool had_retries = pending != 0;
		const bool retry_backoff = had_retries && p->consec_errors >= RETRY_BACKOFF_AFTER;
		p->retry_pending = 0;
		if (!stop && retry_backoff) {
			pthread_mutex_unlock(&p->mutex);
			struct timespec ts = {0, RETRY_BACKOFF_NS};
			nanosleep(&ts, NULL);
			pthread_mutex_lock(&p->mutex);
		}
		if (!p->stopping && !p->fatal) {
			for (unsigned i = 0; pending && i < XFER_COUNT; i++) {
				const uint32_t bit = UINT32_C(1) << i;
				if (!(pending & bit))
					continue;
				pending &= ~bit;
				struct slot *slot = &p->slots[i];
				/* Isochronous endpoints do not support halt/clear_halt; just reset & resubmit. */
				reset_iso_transfer(slot->x, p);
				submit_slot_locked(slot, now_ns());
				if (p->stopping || p->fatal)
					break;
			}
		}
		const bool stop_after = p->stopping || p->fatal;
		pthread_mutex_unlock(&p->mutex);
		if (stop || stop_after)
			break;
		if (had_retries && !retry_backoff)
			continue;
		struct timeval tv = {0, 10000};
		r = libusb_handle_events_timeout(p->ctx, &tv);
	}

	struct libusb_transfer *to_cancel[XFER_COUNT];
	unsigned n_cancel = 0;
	pthread_mutex_lock(&p->mutex);
	p->stopping = true;
	for (unsigned i = 0; i < XFER_COUNT; i++)
		if (p->slots[i].active)
			to_cancel[n_cancel++] = p->slots[i].x;
	pthread_mutex_unlock(&p->mutex);
	for (unsigned i = 0; i < n_cancel; i++)
		libusb_cancel_transfer(to_cancel[i]);

	const uint64_t drain_start = now_ns();
	unsigned drain_errors = 0;
	for (;;) {
		pthread_mutex_lock(&p->mutex);
		const unsigned active = p->active;
		pthread_mutex_unlock(&p->mutex);
		if (!active)
			break;
		if (now_ns() - drain_start >= DRAIN_TIMEOUT_NS)
			break;
		struct timeval tv = {0, 10000};
		const int er = libusb_handle_events_timeout(p->ctx, &tv);
		if (er == 0 || er == LIBUSB_ERROR_INTERRUPTED) {
			drain_errors = 0;
		} else if (++drain_errors >= DRAIN_EVENT_ERROR_LIMIT) {
			break;
		}
	}

	libusb_lock_events(p->ctx);
	libusb_unlock_events(p->ctx);
	pthread_mutex_lock(&p->mutex);
	pthread_cond_broadcast(&p->cond);
	pthread_mutex_unlock(&p->mutex);
	return NULL;
}

static void free_pump_storage(struct uvc_iso_pump *p)
{
	for (unsigned i = 0; i < XFER_COUNT; i++) {
		if (p->slots[i].x) {
			libusb_free_transfer(p->slots[i].x);
			p->slots[i].x = NULL;
		}
	}
	for (unsigned i = 0; i < p->buffer_count; i++) {
		if (p->buffers[i]) {
			free(p->buffers[i]);
			p->buffers[i] = NULL;
		}
	}
	pthread_cond_destroy(&p->cond);
	pthread_mutex_destroy(&p->mutex);
	free(p);
}

static int finish_pump_teardown(struct uvc_iso_pump *p, struct uvc_iso_pump **retained_out)
{
	if (retained_out)
		*retained_out = NULL;
	pthread_mutex_lock(&p->mutex);
	if (p->active > 0) {
		/* Abandoned drain: in-flight URBs never completed. Mark abandoned
		 * and only free inactive transfers + unpinned buffers now; active
		 * transfers/buffers are reclaimed by late callbacks or after close. */
		p->stopping = true;
		p->abandoned = true;
		p->owner_held = (retained_out != NULL);

		struct libusb_transfer *free_xfers[XFER_COUNT] = {0};
		unsigned n_xfers = 0;
		uint8_t *free_bufs[BUFFER_MAX] = {0};
		unsigned n_bufs = 0;
		bool pinned[BUFFER_MAX] = {0};

		for (unsigned i = 0; i < XFER_COUNT; i++) {
			if (p->slots[i].active) {
				if (p->slots[i].buffer_id < p->buffer_count)
					pinned[p->slots[i].buffer_id] = true;
			} else if (p->slots[i].x) {
				free_xfers[n_xfers++] = p->slots[i].x;
				p->slots[i].x = NULL;
			}
		}
		for (unsigned i = 0; i < p->buffer_count; i++) {
			if (!pinned[i] && p->buffers[i]) {
				free_bufs[n_bufs++] = p->buffers[i];
				p->buffers[i] = NULL;
			}
		}
		if (retained_out)
			*retained_out = p;
		pthread_mutex_unlock(&p->mutex);

		for (unsigned i = 0; i < n_xfers; i++)
			libusb_free_transfer(free_xfers[i]);
		for (unsigned i = 0; i < n_bufs; i++)
			free(free_bufs[i]);
		return UVC_PUMP_ERR_CANCEL_TIMEOUT;
	}
	pthread_mutex_unlock(&p->mutex);

	free_pump_storage(p);
	return 0;
}

struct uvc_iso_pump *uvc_iso_pump_start_ex(libusb_context *ctx, libusb_device_handle *handle, uint8_t endpoint,
					   unsigned packet_size, unsigned packets_per_xfer, int *error_out,
					   struct uvc_iso_pump **retained_out)
{
	if (retained_out)
		*retained_out = NULL;
	int err = LIBUSB_ERROR_NO_MEM;
	if (!ctx || !handle || !packet_size || packet_size > MAX_ISO_PACKET_BYTES || !packets_per_xfer ||
	    packets_per_xfer > UVC_ISO_MAX_PACKETS ||
	    (uint64_t)packet_size * packets_per_xfer > (uint64_t)INT_MAX) {
		if (error_out)
			*error_out = LIBUSB_ERROR_INVALID_PARAM;
		return NULL;
	}

	const unsigned transfer_size = packet_size * packets_per_xfer;
	struct uvc_iso_pump *p = calloc(1, sizeof(*p));
	if (!p)
		goto fail_null;

	if (pthread_mutex_init(&p->mutex, NULL)) {
		free(p);
		goto fail_null;
	}

	pthread_condattr_t attr;
	const bool have_attr = pthread_condattr_init(&attr) == 0;
	if (have_attr && pthread_condattr_setclock(&attr, CLOCK_MONOTONIC) == 0)
		p->monotonic_cond = true;
	const int cond_err = pthread_cond_init(&p->cond, have_attr ? &attr : NULL);
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
	p->packet_size = packet_size;
	p->packets_per_xfer = packets_per_xfer;
	p->transfer_size = transfer_size;
	p->borrowed = -1;

	p->ready_capacity = READY_BUDGET / transfer_size;
	if (p->ready_capacity > READY_MAX)
		p->ready_capacity = READY_MAX;
	if (p->ready_capacity < XFER_COUNT)
		p->ready_capacity = XFER_COUNT;
	p->buffer_count = XFER_COUNT + p->ready_capacity;
	p->stats.ready_capacity = p->ready_capacity;

	for (unsigned i = 0; i < p->buffer_count; i++) {
		p->buffers[i] = malloc(transfer_size);
		if (!p->buffers[i])
			goto fail;
	}
	for (unsigned i = XFER_COUNT; i < p->buffer_count; i++)
		p->free_ids[p->free_count++] = i;

	for (unsigned i = 0; i < XFER_COUNT; i++) {
		struct slot *slot = &p->slots[i];
		slot->pump = p;
		slot->index = i;
		slot->buffer_id = i;
		slot->x = libusb_alloc_transfer((int)packets_per_xfer);
		if (!slot->x)
			goto fail;
		libusb_fill_iso_transfer(slot->x, handle, endpoint, p->buffers[i], (int)transfer_size,
					 (int)packets_per_xfer, on_transfer, slot, 0);
		libusb_set_iso_packet_lengths(slot->x, packet_size);
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
		if (finish_pump_teardown(p, retained_out) == UVC_PUMP_ERR_CANCEL_TIMEOUT)
			err = UVC_PUMP_ERR_CANCEL_TIMEOUT;
		goto fail_null;
	}

	if (error_out)
		*error_out = 0;
	return p;

fail:
	finish_pump_teardown(p, NULL);
fail_null:
	if (error_out)
		*error_out = err;
	return NULL;
}

struct uvc_iso_pump *uvc_iso_pump_start(libusb_context *ctx, libusb_device_handle *handle, uint8_t endpoint,
					unsigned packet_size, unsigned packets_per_xfer, int *error_out)
{
	return uvc_iso_pump_start_ex(ctx, handle, endpoint, packet_size, packets_per_xfer, error_out, NULL);
}

int uvc_iso_pump_acquire(struct uvc_iso_pump *p, struct uvc_iso_block *block, unsigned timeout_ms)
{
	if (!p || !block)
		return -1;

	struct timespec until;
	clock_gettime(p->monotonic_cond ? CLOCK_MONOTONIC : CLOCK_REALTIME, &until);
	until.tv_sec += timeout_ms / 1000U;
	until.tv_nsec += (long)(timeout_ms % 1000U) * 1000000L;
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

	const struct ready_block *b = &p->ready[p->ready_head];
	p->ready_head = (p->ready_head + 1) % p->ready_capacity;
	p->ready_count--;
	p->borrowed = (int)b->buffer;
	block->data = p->buffers[b->buffer];
	block->num_packets = b->num_packets;
	memcpy(block->packets, b->packets, b->num_packets * sizeof(b->packets[0]));
	block->sequence = b->seq;
	block->received_ns = b->ns;
	pthread_mutex_unlock(&p->mutex);
	return 1;
}

void uvc_iso_pump_release(struct uvc_iso_pump *p, struct uvc_iso_block *block)
{
	if (!p || !block)
		return;
	pthread_mutex_lock(&p->mutex);
	if (p->borrowed >= 0 && block->data == p->buffers[p->borrowed] && p->free_count < BUFFER_MAX) {
		p->free_ids[p->free_count++] = (unsigned)p->borrowed;
		p->borrowed = -1;
		memset(block, 0, sizeof(*block));
	}
	pthread_mutex_unlock(&p->mutex);
}

int uvc_iso_pump_error(struct uvc_iso_pump *p)
{
	if (!p)
		return 0;
	pthread_mutex_lock(&p->mutex);
	const int e = p->fatal;
	pthread_mutex_unlock(&p->mutex);
	return e;
}

void uvc_iso_pump_snapshot(struct uvc_iso_pump *p, struct uvc_iso_stats *stats)
{
	if (!p || !stats)
		return;
	pthread_mutex_lock(&p->mutex);
	*stats = p->stats;
	pthread_mutex_unlock(&p->mutex);
}

int uvc_iso_pump_stop_ex(struct uvc_iso_pump *p, struct uvc_iso_pump **retained_out)
{
	if (retained_out)
		*retained_out = NULL;
	if (!p)
		return 0;
	pthread_mutex_lock(&p->mutex);
	p->stopping = true;
	pthread_cond_broadcast(&p->cond);
	while (p->waiters)
		pthread_cond_wait(&p->cond, &p->mutex);
	pthread_mutex_unlock(&p->mutex);
	if (p->thread_started)
		pthread_join(p->thread, NULL);
	return finish_pump_teardown(p, retained_out);
}

int uvc_iso_pump_stop(struct uvc_iso_pump *p)
{
	return uvc_iso_pump_stop_ex(p, NULL);
}

bool uvc_iso_pump_try_drain(struct uvc_iso_pump *p, unsigned timeout_ms)
{
	if (!p)
		return true;
	const uint64_t start = now_ns();
	const uint64_t limit = (uint64_t)timeout_ms * 1000000ULL;
	unsigned drain_errors = 0;
	for (;;) {
		pthread_mutex_lock(&p->mutex);
		const unsigned active = p->active;
		pthread_mutex_unlock(&p->mutex);
		if (!active)
			return true;
		if (now_ns() - start >= limit)
			return false;
		struct timeval tv = {0, 10000};
		const int er = libusb_handle_events_timeout(p->ctx, &tv);
		if (er == 0 || er == LIBUSB_ERROR_INTERRUPTED) {
			drain_errors = 0;
		} else if (++drain_errors >= DRAIN_EVENT_ERROR_LIMIT) {
			return false;
		}
	}
}

void uvc_iso_pump_reclaim_after_close(struct uvc_iso_pump *p)
{
	if (!p)
		return;
	free_pump_storage(p);
}
