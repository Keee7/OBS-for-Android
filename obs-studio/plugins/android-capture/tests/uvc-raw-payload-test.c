/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "uvc-raw-payload.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
	uint8_t frame[8];
	struct uvc_raw_payload_state s;
	uvc_raw_payload_init(&s, frame, sizeof(frame));
	/* Pixel data deliberately contains fake UVC headers including ERR. */
	const uint8_t a[] = {6, 0x84, 1, 0, 0, 0, 6, 0xc4, 1, 0};
	const uint8_t b[] = {6, 0x86, 1, 0, 0, 0, 0, 0, 2, 0x82};
	assert(!uvc_raw_payload_feed(&s, a, sizeof(a)));
	assert(uvc_raw_payload_feed(&s, b, sizeof(b)) == 1);
	const uint8_t pixels[] = {6, 0xc4, 1, 0, 0, 0, 2, 0x82};
	assert(!memcmp(frame, pixels, 8) && s.bad_frames == 0);
	const uint8_t single[] = {2, 0x83, 1, 2, 3, 4, 5, 6, 7, 8};
	assert(uvc_raw_payload_feed(&s, single, sizeof(single)) == 1);
	const uint8_t first[] = {2, 0x80, 1, 2, 3, 4, 5, 6, 7, 8};
	const uint8_t eof[] = {2, 0x82};
	assert(!uvc_raw_payload_feed(&s, first, sizeof(first)));
	assert(uvc_raw_payload_feed(&s, eof, sizeof(eof)) == 1);
	const uint8_t shortf[] = {2, 0x83, 1, 2};
	assert(!uvc_raw_payload_feed(&s, shortf, sizeof(shortf)) && s.bad_frames == 1);
	assert(!uvc_raw_payload_feed(&s, single, sizeof(single)) && s.bad_frames == 1);
	assert(uvc_raw_payload_feed(&s, (const uint8_t[]){2, 0x82, 1, 2, 3, 4, 5, 6, 7, 8}, 10) == 1);
	const uint8_t err[] = {2, 0xc1};
	assert(!uvc_raw_payload_feed(&s, err, sizeof(err)) && s.bad_frames == 2);
	assert(!uvc_raw_payload_feed(&s, single, sizeof(single)) && s.bad_frames == 2);
	assert(uvc_raw_payload_feed(&s, (const uint8_t[]){2, 0x82, 1, 2, 3, 4, 5, 6, 7, 8}, 10) == 1);
	const uint8_t overflow[] = {2, 0x83, 1, 2, 3, 4, 5, 6, 7, 8, 9};
	assert(!uvc_raw_payload_feed(&s, overflow, sizeof(overflow)) && s.bad_frames == 3);
	uvc_raw_payload_reset(&s);
	const uint8_t malformed[] = {6, 0x80, 1, 2, 3, 4};
	assert(!uvc_raw_payload_feed(&s, malformed, sizeof(malformed)) && s.bad_frames == 4);
	assert(uvc_raw_payload_feed(&s, single, sizeof(single)) == 1);
	/* Six-byte NV12 frame across SCR and PTS+SCR payloads; same FID but
	 * changed PTS starts a fresh frame. */
	uvc_raw_payload_init(&s, frame, 6);
	const uint8_t scr[] = {8, 0x88, 0, 0, 0, 0, 0, 0, 11, 12, 13};
	const uint8_t pts_scr[] = {12, 0x8e, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 14, 15, 16};
	assert(!uvc_raw_payload_feed(&s, scr, sizeof(scr)));
	assert(uvc_raw_payload_feed(&s, pts_scr, sizeof(pts_scr)) == 1);
	assert(!memcmp(frame, (const uint8_t[]){11, 12, 13, 14, 15, 16}, 6));
	const uint8_t next_pts[] = {6, 0x86, 2, 0, 0, 0, 21, 22, 23, 24, 25, 26};
	assert(uvc_raw_payload_feed(&s, next_pts, sizeof(next_pts)) == 1);
	/* Missing EOF and a subsequent identity change drops the partial frame. */
	const uint8_t partial[] = {2, 0x81, 1};
	assert(!uvc_raw_payload_feed(&s, partial, sizeof(partial)));
	assert(uvc_raw_payload_feed(&s, next_pts, sizeof(next_pts)) == 1 && s.bad_frames == 1);
	puts("uvc raw payload tests passed");
}
