/* SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define UVC_BULK_BMH_FID 0x01U
#define UVC_BULK_BMH_EOF 0x02U
#define UVC_BULK_BMH_PTS 0x04U
#define UVC_BULK_BMH_SCR 0x08U
#define UVC_BULK_BMH_RES 0x10U
#define UVC_BULK_BMH_ERR 0x40U
#define UVC_BULK_MAX_HLE 16U

struct uvc_bulk_frame {
	size_t len;
	bool have_pts;
	uint32_t pts;
};

/* Standard UVC 1.0/1.1/1.5 Bulk Payload Transfer assembler for devices whose
 * dwMaxPayloadTransferSize exceeds a single host URB (e.g. Sony A7M4/ZV-E10
 * sending an entire MJPEG frame in one 1.38 MB bulk payload across multiple
 * 64 KiB URBs terminated by a short packet or ZLP).
 *
 * Per UVC spec section 2.4.3.2 (matching Linux uvcvideo uvc_video_decode_bulk):
 * - A Bulk Payload Transfer begins with ONE UVC payload header at byte 0 of
 *   the first URB; all subsequent full-sized URBs of that payload carry raw
 *   image bytes with NO embedded headers.
 * - The payload transfer ends when a short URB (size < transfer_size, including
 *   a 0-byte ZLP) arrives OR accumulated payload bytes reach max_payload.
 * - A video frame ends when a payload whose header had EOF=1 finishes, or when
 *   the next payload header toggles FID. */
struct uvc_bulk_payload_assembler {
	uint8_t *frame_buf;
	size_t frame_cap;
	size_t frame_len;
	uint32_t frame_total;
	bool is_mjpeg;

	size_t max_payload;
	size_t transfer_size;

	/* Current Bulk Payload Transfer state */
	bool in_payload;
	bool payload_eof;
	bool payload_skip;
	size_t payload_bytes;

	/* Current Video Frame state */
	bool assembling;
	bool corrupt;
	bool have_fid;
	uint8_t fid;
	bool after_eof;
	bool have_pts;
	uint32_t pts;

	/* Diagnostics & automatic fallback counters */
	uint64_t good_frames;
	uint64_t bad_frames;
	uint64_t err_packets;
	uint64_t overflow_frames;
	uint64_t invalid_headers;
	uint32_t consec_invalid_headers;
	uint64_t fed_bytes;
	uint64_t non_err_bytes_since_good;
};

static inline uint32_t uvc_bulk_read_u32le(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline void uvc_bulk_payload_init(struct uvc_bulk_payload_assembler *st, uint8_t *frame_buf, size_t frame_cap,
					 uint32_t frame_total, bool is_mjpeg, size_t max_payload, size_t transfer_size)
{
	memset(st, 0, sizeof(*st));
	st->frame_buf = frame_buf;
	st->frame_cap = frame_cap;
	st->frame_total = frame_total;
	st->is_mjpeg = is_mjpeg;
	st->max_payload = max_payload;
	st->transfer_size = transfer_size;
}

/* Reset active frame/payload assembly after a transport sequence gap while
 * keeping diagnostics and fallback counters intact. */
static inline void uvc_bulk_payload_on_gap(struct uvc_bulk_payload_assembler *st)
{
	if (st->assembling && !st->corrupt)
		st->bad_frames++;
	st->assembling = false;
	st->corrupt = false;
	st->frame_len = 0;
	st->have_pts = false;
	st->after_eof = false;
	st->in_payload = false;
	st->payload_eof = false;
	st->payload_skip = false;
	st->payload_bytes = 0;
}

static inline bool uvc_bulk_header_valid(const uint8_t *data, size_t size, uint8_t *hle_out, uint8_t *bmh_out)
{
	if (!data || size < 2)
		return false;
	const uint8_t hle = data[0];
	const uint8_t bmh = data[1];
	const unsigned expected_hle =
		2U + ((bmh & UVC_BULK_BMH_PTS) ? 4U : 0U) + ((bmh & UVC_BULK_BMH_SCR) ? 6U : 0U);
	if ((bmh & UVC_BULK_BMH_RES) != 0 || hle < expected_hle || hle > UVC_BULK_MAX_HLE || hle > size)
		return false;
	if (hle_out)
		*hle_out = hle;
	if (bmh_out)
		*bmh_out = bmh;
	return true;
}

static inline void uvc_bulk_payload_note_delivered(struct uvc_bulk_payload_assembler *st)
{
	st->good_frames++;
	st->non_err_bytes_since_good = 0;
	st->consec_invalid_headers = 0;
}

/* Before push() writes a new payload into frame_buf, check whether a new
 * payload header toggles FID while an earlier frame is still open (EOF=0
 * cameras). If so, return true and populate *out with the completed frame. */
static inline bool uvc_bulk_payload_check_flush(struct uvc_bulk_payload_assembler *st, const uint8_t *data, size_t size,
						struct uvc_bulk_frame *out)
{
	if (!st || st->in_payload || size < 2)
		return false;
	uint8_t hle = 0, bmh = 0;
	if (!uvc_bulk_header_valid(data, size, &hle, &bmh))
		return false;
	if (st->assembling && st->have_fid && ((bmh & UVC_BULK_BMH_FID) != st->fid)) {
		const bool emit = !st->corrupt && st->frame_len > 0;
		if (emit && out)
			*out = (struct uvc_bulk_frame){st->frame_len, st->have_pts, st->pts};
		st->assembling = false;
		st->corrupt = false;
		st->frame_len = 0;
		st->have_pts = false;
		st->after_eof = false;
		return emit;
	}
	return false;
}

/* Feed one completed USB Bulk URB (or a 0-byte ZLP). Returns true and fills
 * *out when a frame finishes (either on payload end with EOF=1 or ZLP). */
static inline bool uvc_bulk_payload_push(struct uvc_bulk_payload_assembler *st, const uint8_t *data, size_t size,
					 struct uvc_bulk_frame *out, bool *had_bmh_err)
{
	if (had_bmh_err)
		*had_bmh_err = false;
	if (!st)
		return false;

	/* Zero-Length Packet (ZLP): terminates an exact-multiple payload transfer. */
	if (size == 0) {
		const bool was_eof = st->in_payload && !st->payload_skip && st->payload_eof;
		st->in_payload = false;
		st->payload_eof = false;
		st->payload_skip = false;
		st->payload_bytes = 0;
		if (was_eof && st->assembling) {
			const bool emit = !st->corrupt && st->frame_len > 0;
			if (emit && out)
				*out = (struct uvc_bulk_frame){st->frame_len, st->have_pts, st->pts};
			st->assembling = false;
			st->corrupt = false;
			st->frame_len = 0;
			st->have_pts = false;
			st->after_eof = true;
			return emit;
		}
		return false;
	}

	if (!data)
		return false;

	st->fed_bytes += size;

	const uint8_t *payload = data;
	size_t payload_len = size;

	if (!st->in_payload) {
		uint8_t hle = 0, bmh = 0;
		if (!uvc_bulk_header_valid(data, size, &hle, &bmh)) {
			/* Mid-frame startup or non-framed stream: skip until short packet. */
			st->invalid_headers++;
			st->consec_invalid_headers++;
			st->non_err_bytes_since_good += size;
			if (st->assembling && !st->corrupt) {
				st->corrupt = true;
				st->frame_len = 0;
				st->bad_frames++;
			}
			const bool short_pkt = st->transfer_size > 0 && size < st->transfer_size;
			const bool hit_max = st->max_payload > 0 && size >= st->max_payload;
			if (!short_pkt && !hit_max) {
				st->in_payload = true;
				st->payload_skip = true;
				st->payload_eof = false;
				st->payload_bytes = size;
			}
			return false;
		}

		st->consec_invalid_headers = 0;
		const uint8_t fid = (uint8_t)(bmh & UVC_BULK_BMH_FID);
		const bool eof = (bmh & UVC_BULK_BMH_EOF) != 0;
		const bool err = (bmh & UVC_BULK_BMH_ERR) != 0;
		const bool has_pts = (bmh & UVC_BULK_BMH_PTS) != 0 && hle >= 6;
		const uint32_t pts = has_pts ? uvc_bulk_read_u32le(data + 2) : 0;

		payload = data + hle;
		payload_len = size - hle;

		st->in_payload = true;
		st->payload_bytes = size;
		st->payload_eof = eof;
		st->payload_skip = false;

		if (err) {
			if (had_bmh_err)
				*had_bmh_err = true;
			st->err_packets++;
			if (st->assembling && !st->corrupt)
				st->bad_frames++;
			st->frame_len = 0;
			st->have_pts = false;
			st->have_fid = true;
			st->fid = fid;
			st->payload_skip = true;
			if (eof) {
				st->assembling = false;
				st->corrupt = false;
				st->after_eof = true;
			} else {
				st->assembling = true;
				st->corrupt = true;
				st->after_eof = false;
			}
		} else {
			st->non_err_bytes_since_good += size;
			if (!st->assembling) {
				if (st->after_eof && st->have_fid && fid == st->fid && payload_len == 0) {
					st->payload_skip = true;
				} else if (payload_len == 0 && eof) {
					st->have_fid = true;
					st->fid = fid;
					st->after_eof = true;
					st->payload_skip = true;
				} else {
					st->assembling = true;
					st->corrupt = false;
					st->after_eof = false;
					st->frame_len = 0;
					st->have_fid = true;
					st->fid = fid;
					st->have_pts = has_pts;
					st->pts = pts;
				}
			} else {
				if (st->have_fid && fid != st->fid) {
					/* Defensive reset if caller skipped check_flush(). */
					st->corrupt = false;
					st->frame_len = 0;
					st->after_eof = false;
					st->fid = fid;
					st->have_pts = has_pts;
					st->pts = pts;
				} else if (!st->have_pts && has_pts) {
					st->have_pts = true;
					st->pts = pts;
				}
			}
		}
	} else {
		st->payload_bytes += size;
		if (!st->payload_skip)
			st->non_err_bytes_since_good += size;
	}

	if (!st->payload_skip && st->assembling && !st->corrupt && payload_len > 0) {
		if (st->is_mjpeg && st->frame_len == 0 && payload_len >= 2 &&
		    (payload[0] != 0xFF || payload[1] != 0xD8)) {
			st->corrupt = true;
			st->payload_skip = true;
			st->bad_frames++;
		} else if (!st->frame_buf || st->frame_len > st->frame_cap ||
			   payload_len > st->frame_cap - st->frame_len) {
			st->corrupt = true;
			st->payload_skip = true;
			st->frame_len = 0;
			st->overflow_frames++;
			st->bad_frames++;
		} else {
			memcpy(st->frame_buf + st->frame_len, payload, payload_len);
			st->frame_len += payload_len;
		}
	}

	const bool short_pkt = st->transfer_size > 0 && size < st->transfer_size;
	const bool hit_max = st->max_payload > 0 && st->payload_bytes >= st->max_payload;
	if (short_pkt || hit_max) {
		const bool was_eof = !st->payload_skip && st->payload_eof;
		st->in_payload = false;
		st->payload_eof = false;
		st->payload_skip = false;
		st->payload_bytes = 0;

		if (was_eof && st->assembling) {
			const bool emit = !st->corrupt && st->frame_len > 0;
			if (emit && out)
				*out = (struct uvc_bulk_frame){st->frame_len, st->have_pts, st->pts};
			st->assembling = false;
			st->corrupt = false;
			st->frame_len = 0;
			st->have_pts = false;
			st->after_eof = true;
			return emit;
		}
	}
	return false;
}

/* Fallback safety net if a device advertises a large dwMaxPayloadTransferSize
 * yet does not delimit Bulk payloads per the UVC specification. */
static inline bool uvc_bulk_payload_should_fallback(const struct uvc_bulk_payload_assembler *st)
{
	if (!st)
		return false;
	const bool established = st->good_frames >= 2U;
	const uint32_t invalid_limit = established ? 12U : 8U;
	if (st->consec_invalid_headers >= invalid_limit)
		return true;
	uint64_t byte_limit = 8ULL * 1024ULL * 1024ULL;
	if (st->max_payload > 0) {
		const uint64_t scaled = (uint64_t)st->max_payload * (established ? 8ULL : 6ULL);
		if (scaled > byte_limit)
			byte_limit = scaled;
	}
	if (st->non_err_bytes_since_good >= byte_limit)
		return true;
	return false;
}
