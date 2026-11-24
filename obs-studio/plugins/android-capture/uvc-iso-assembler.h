/* SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define UVC_ISO_BMH_FID 0x01U
#define UVC_ISO_BMH_EOF 0x02U
#define UVC_ISO_BMH_PTS 0x04U
#define UVC_ISO_BMH_ERR 0x40U

struct uvc_iso_frame {
	size_t len;
	bool have_pts;
	uint32_t pts;
};

/* Per-packet UVC Isochronous frame assembler supporting standard FID+EOF,
 * FID-only (missing/dropped EOF), and EOF-only (stuck FID) cameras without
 * extra pixel copies. */
struct uvc_iso_assembler {
	uint8_t *frame_buf;
	size_t frame_cap;
	size_t frame_len;
	uint32_t frame_total;
	bool is_mjpeg;

	bool assembling;
	bool corrupt;
	bool have_fid;
	uint8_t fid;
	bool after_eof;
	bool have_pts;
	uint32_t pts;

	uint64_t err_packets;
	uint64_t overflow_frames;
	uint64_t invalid_headers;
};

static inline uint32_t uvc_iso_read_u32le(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline void uvc_iso_assembler_init(struct uvc_iso_assembler *st, uint8_t *frame_buf, size_t frame_cap,
					  uint32_t frame_total, bool is_mjpeg)
{
	memset(st, 0, sizeof(*st));
	st->frame_buf = frame_buf;
	st->frame_cap = frame_cap;
	st->frame_total = frame_total;
	st->is_mjpeg = is_mjpeg;
}

static inline void uvc_iso_assembler_reset(struct uvc_iso_assembler *st)
{
	st->frame_len = 0;
	st->assembling = false;
	st->corrupt = false;
	st->have_fid = false;
	st->fid = 0;
	st->after_eof = false;
	st->have_pts = false;
	st->pts = 0;
}

/* Marks the in-progress frame corrupt when an URB sequence gap or ISO packet
 * error occurs, keeping assembly locked to the current FID until the next frame. */
static inline void uvc_iso_assembler_on_gap(struct uvc_iso_assembler *st)
{
	if (st->assembling) {
		st->corrupt = true;
		st->frame_len = 0;
	}
}

/* Checks whether the incoming packet flips FID while a frame is already being
 * assembled, emitting that previous frame BEFORE push() writes the new frame's
 * first bytes into frame_buf. */
static inline bool uvc_iso_assembler_check_flush(struct uvc_iso_assembler *st, const uint8_t *pkt, size_t len,
						 struct uvc_iso_frame *out)
{
	if (!pkt || len < 2)
		return false;
	const uint8_t hle = pkt[0];
	if (hle < 2 || hle > len)
		return false;

	if (st->assembling && st->have_fid && ((pkt[1] & UVC_ISO_BMH_FID) != st->fid)) {
		const bool emit = !st->corrupt && st->frame_len > 0;
		if (emit && out)
			*out = (struct uvc_iso_frame){st->frame_len, st->have_pts, st->pts};
		st->assembling = false;
		st->corrupt = false;
		st->frame_len = 0;
		st->have_pts = false;
		st->after_eof = false;
		return emit;
	}
	return false;
}

/* Pushes one ISO packet into st. Returns true and fills *out when EOF completes
 * a valid frame; sets *had_bmh_err when UVC_BMH_ERR was flagged in the header. */
static inline bool uvc_iso_assembler_push(struct uvc_iso_assembler *st, const uint8_t *pkt, size_t len,
					  struct uvc_iso_frame *out, bool *had_bmh_err)
{
	if (had_bmh_err)
		*had_bmh_err = false;
	if (!pkt || len == 0)
		return false;

	const uint8_t hle = pkt[0];
	if (len < 2 || hle < 2 || hle > len) {
		st->invalid_headers++;
		return false;
	}

	const uint8_t bmh = pkt[1];
	const uint8_t fid = (uint8_t)(bmh & UVC_ISO_BMH_FID);
	const bool eof = (bmh & UVC_ISO_BMH_EOF) != 0;
	const bool err = (bmh & UVC_ISO_BMH_ERR) != 0;
	const bool has_pts = (bmh & UVC_ISO_BMH_PTS) != 0 && hle >= 6;
	const uint32_t pts = has_pts ? uvc_iso_read_u32le(pkt + 2) : 0;
	const uint8_t *payload = pkt + hle;
	const size_t payload_len = len - hle;

	if (err) {
		if (had_bmh_err)
			*had_bmh_err = true;
		st->err_packets++;
		st->frame_len = 0;
		st->have_pts = false;
		st->have_fid = true;
		st->fid = fid;
		if (eof) {
			st->assembling = false;
			st->corrupt = false;
			st->after_eof = true;
		} else {
			st->assembling = true;
			st->corrupt = true;
			st->after_eof = false;
		}
		return false;
	}

	if (!st->assembling) {
		if (st->after_eof && st->have_fid && fid == st->fid && payload_len == 0)
			return false;
		if (payload_len == 0 && eof) {
			st->have_fid = true;
			st->fid = fid;
			st->after_eof = true;
			return false;
		}
		st->assembling = true;
		st->corrupt = false;
		st->after_eof = false;
		st->frame_len = 0;
		st->have_fid = true;
		st->fid = fid;
		st->have_pts = has_pts;
		st->pts = pts;
	} else {
		/* Defensive reset if caller invoked push() without check_flush() on FID change. */
		if (st->have_fid && fid != st->fid) {
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

	if (!st->corrupt && payload_len > 0) {
		if (st->is_mjpeg && st->frame_len == 0 && payload_len >= 2 &&
		    (payload[0] != 0xFF || payload[1] != 0xD8)) {
			st->corrupt = true;
		} else if (!st->frame_buf || st->frame_len > st->frame_cap ||
			   payload_len > st->frame_cap - st->frame_len) {
			st->corrupt = true;
			st->frame_len = 0;
			st->overflow_frames++;
		} else {
			memcpy(st->frame_buf + st->frame_len, payload, payload_len);
			st->frame_len += payload_len;
		}
	}

	if (eof) {
		const bool emit = !st->corrupt && st->frame_len > 0;
		if (emit && out)
			*out = (struct uvc_iso_frame){st->frame_len, st->have_pts, st->pts};
		st->assembling = false;
		st->corrupt = false;
		st->frame_len = 0;
		st->have_pts = false;
		st->after_eof = true;
		return emit;
	}
	return false;
}
