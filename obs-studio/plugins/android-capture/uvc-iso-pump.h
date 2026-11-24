/* SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <libusb-1.0/libusb.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "uvc-iso-select.h"

#ifndef UVC_PUMP_ERR_CANCEL_TIMEOUT
/* Returned when stopping/aborting a pump could not drain all in-flight USB
 * transfers before the drain error/timeout limit. Callers must NOT reuse
 * the libusb handle/context for new transfers and must retire/close them. */
#define UVC_PUMP_ERR_CANCEL_TIMEOUT (-1001)
#endif

#define UVC_ISO_IN_FLIGHT 8U
#define UVC_ISO_MIN_IN_FLIGHT 4U

struct uvc_iso_pump;

struct uvc_iso_packet_desc {
	uint32_t offset;
	uint32_t length;
	int status;
};

struct uvc_iso_block {
	const uint8_t *data;
	unsigned num_packets;
	struct uvc_iso_packet_desc packets[UVC_ISO_MAX_PACKETS];
	uint64_t sequence;
	uint64_t received_ns;
};

struct uvc_iso_stats {
	uint64_t received_bytes, dropped_blocks, dropped_bytes, transfers, errors, packet_errors, max_resubmit_ns;
	uint64_t timeouts, stalls, overflows, io_errors;
	unsigned ready_high_water, ready_capacity, active_xfers;
};

/* Starts an async Isochronous IN pump. Returns NULL and sets *error_out on
 * failure (or UVC_PUMP_ERR_CANCEL_TIMEOUT if partial submit cleanup timed out).
 * If retained_out is non-NULL and cancel timed out, *retained_out receives the
 * abandoned pump for background drain/reclaim after closing the handle/fd. */
struct uvc_iso_pump *uvc_iso_pump_start(libusb_context *ctx, libusb_device_handle *handle, uint8_t endpoint,
					unsigned packet_size, unsigned packets_per_xfer, int *error_out);
struct uvc_iso_pump *uvc_iso_pump_start_ex(libusb_context *ctx, libusb_device_handle *handle, uint8_t endpoint,
					   unsigned packet_size, unsigned packets_per_xfer, int *error_out,
					   struct uvc_iso_pump **retained_out);

/* Single consumer; acquire at most one block at a time. 1 = block, 0 = timeout, -1 = stopped/fatal. */
int uvc_iso_pump_acquire(struct uvc_iso_pump *p, struct uvc_iso_block *block, unsigned timeout_ms);
void uvc_iso_pump_release(struct uvc_iso_pump *p, struct uvc_iso_block *block);

/* 0 or a negative LIBUSB_ERROR_* value. */
int uvc_iso_pump_error(struct uvc_iso_pump *p);
void uvc_iso_pump_snapshot(struct uvc_iso_pump *p, struct uvc_iso_stats *stats);

/* Caller must stop using/release any borrowed block before calling stop.
 * Returns 0 if all in-flight transfers drained cleanly, or UVC_PUMP_ERR_CANCEL_TIMEOUT. */
int uvc_iso_pump_stop(struct uvc_iso_pump *p);
int uvc_iso_pump_stop_ex(struct uvc_iso_pump *p, struct uvc_iso_pump **retained_out);
/* Attempts to drain remaining abandoned transfers on an owner-held pump for up
 * to timeout_ms. Returns true when active count reaches 0. */
bool uvc_iso_pump_try_drain(struct uvc_iso_pump *p, unsigned timeout_ms);
/* Reclaims an owner-held abandoned pump after handle & fd have been closed. */
void uvc_iso_pump_reclaim_after_close(struct uvc_iso_pump *p);
