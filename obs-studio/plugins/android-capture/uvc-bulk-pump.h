/* SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <libusb-1.0/libusb.h>

#define UVC_BULK_CHUNK_SIZE (64U * 1024U)
#define UVC_BULK_IN_FLIGHT 32

struct uvc_bulk_pump;
struct uvc_bulk_block {
	const uint8_t *data;
	size_t size;
	uint64_t sequence;
	uint64_t received_ns;
};
struct uvc_bulk_stats {
	uint64_t received_bytes, dropped_blocks, dropped_bytes, transfers, errors, max_resubmit_ns;
	unsigned ready_high_water;
};
/* On failure returns NULL and sets error_out to a LIBUSB_ERROR_* value. */
struct uvc_bulk_pump *uvc_bulk_pump_start(libusb_context *ctx, libusb_device_handle *handle, uint8_t endpoint,
					  int *error_out);
/* Sized transfers preserve UVC bulk payload boundaries when the device's
 * negotiated maximum payload size is correct. Valid range is 1..64 KiB. */
struct uvc_bulk_pump *uvc_bulk_pump_start_sized(libusb_context *ctx, libusb_device_handle *handle, uint8_t endpoint,
						unsigned transfer_size, int *error_out);
/* Single consumer; acquire at most one block at a time. 1 = block, 0 = timeout, -1 = stopped/fatal.
 * On a sequence gap discard the partial parser frame before appending the returned block.
 * Waits use a monotonic clock when pthread condition attributes support it;
 * otherwise they fall back to the system realtime clock. */
int uvc_bulk_pump_acquire(struct uvc_bulk_pump *p, struct uvc_bulk_block *block, unsigned timeout_ms);
void uvc_bulk_pump_release(struct uvc_bulk_pump *p, struct uvc_bulk_block *block);
/* 0 or a negative LIBUSB_ERROR_* value. */
int uvc_bulk_pump_error(struct uvc_bulk_pump *p);
/* received_bytes counts all completed USB payload bytes, including bytes later
 * dropped under overload; these counters do not represent OBS output frames. */
void uvc_bulk_pump_snapshot(struct uvc_bulk_pump *p, struct uvc_bulk_stats *stats);
/* Caller must stop using/release any borrowed block before calling stop. Handle and context remain owned by caller. */
void uvc_bulk_pump_stop(struct uvc_bulk_pump *p);
