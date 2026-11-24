/* SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define UVC_ISO_MAX_ALTS 32
#define UVC_ISO_MAX_PACKETS 32U

#define UVC_ISO_ERR_IO -1
#define UVC_ISO_ERR_BUSY -6
#define UVC_ISO_ERR_OVERFLOW -8
#define UVC_ISO_ERR_NO_MEM -11
#define UVC_ISO_ERR_OTHER -99

/* Linux usbfs maps ENOSPC from USBDEVFS_SETINTERFACE to LIBUSB_ERROR_OTHER (-99),
 * and may report host bandwidth/resource limits via NO_MEM/IO/BUSY/OVERFLOW. */
static inline bool uvc_iso_is_bandwidth_error(int libusb_err)
{
	return libusb_err == UVC_ISO_ERR_OTHER || libusb_err == UVC_ISO_ERR_NO_MEM ||
	       libusb_err == UVC_ISO_ERR_IO || libusb_err == UVC_ISO_ERR_BUSY ||
	       libusb_err == UVC_ISO_ERR_OVERFLOW;
}

/* Returns the smallest supported frame interval strictly slower (> cur_iv) and
 * <= 2000000 (5 fps) for automatic ISO bandwidth step-down, or 0 if none. */
static inline uint32_t uvc_iso_next_slower_interval(uint32_t cur_iv, const uint32_t *intervals, int n_intervals,
						    uint32_t min_iv, uint32_t max_iv, uint32_t step_iv)
{
	const uint32_t floor_iv = 2000000U;
	if (cur_iv == 0 || cur_iv >= floor_iv)
		return 0;
	if (n_intervals > 0 && intervals) {
		uint32_t best = 0;
		for (int i = 0; i < n_intervals; i++) {
			const uint32_t iv = intervals[i];
			if (iv > cur_iv && iv <= floor_iv && (best == 0 || iv < best))
				best = iv;
		}
		return best;
	}
	if (max_iv > cur_iv) {
		const uint32_t cap = max_iv < floor_iv ? max_iv : floor_iv;
		uint32_t target = cur_iv <= UINT32_MAX / 2U ? cur_iv * 2U : cap;
		if (target > cap)
			target = cap;
		if (step_iv > 0 && target > min_iv)
			target = min_iv + (((target - min_iv) + step_iv - 1U) / step_iv) * step_iv;
		if (target > max_iv)
			target = max_iv;
		return target > cur_iv && target <= floor_iv ? target : 0;
	}
	return 0;
}

/* One Isochronous alternate setting of a UVC VideoStreaming interface. */
struct uvc_iso_alt {
	int alt_setting;
	uint8_t ep_in;
	uint8_t b_interval;
	uint32_t max_packet_size; /* bytes per (micro)frame interval */
};

/* Computes total bytes transferred per service interval for an ISO endpoint.
 * Handles USB 2.0 high-bandwidth transaction multiplier bits [12:11] and
 * USB 3.0 SuperSpeed Endpoint Companion descriptors. */
static inline uint32_t uvc_iso_calc_packet_size(uint16_t w_max_packet_size, bool has_ss_comp,
						uint16_t ss_bytes_per_interval, uint8_t ss_max_burst,
						uint8_t ss_bm_attributes)
{
	const uint32_t base = (uint32_t)(w_max_packet_size & 0x07FFU);
	if (has_ss_comp) {
		if (ss_bytes_per_interval > 0)
			return (uint32_t)ss_bytes_per_interval;
		const uint32_t mult = (uint32_t)(ss_bm_attributes & 0x03U) + 1U;
		return base * ((uint32_t)ss_max_burst + 1U) * mult;
	}
	uint32_t mult = (uint32_t)((w_max_packet_size >> 11) & 0x03U) + 1U;
	if (mult > 3U)
		mult = 3U; /* USB 2.0 allows at most 3 transactions per microframe. */
	return base * mult;
}

/* Insertion-sorts ISO alternate settings ascending by max_packet_size,
 * breaking ties ascending by alt_setting. */
static inline void uvc_iso_sort_alts(struct uvc_iso_alt *alts, int count)
{
	if (!alts || count <= 1)
		return;
	for (int i = 1; i < count; i++) {
		const struct uvc_iso_alt key = alts[i];
		int j = i - 1;
		while (j >= 0 && (alts[j].max_packet_size > key.max_packet_size ||
				  (alts[j].max_packet_size == key.max_packet_size &&
				   alts[j].alt_setting > key.alt_setting))) {
			alts[j + 1] = alts[j];
			j--;
		}
		alts[j + 1] = key;
	}
}

/* Picks the lowest-bandwidth ISO altsetting capable of carrying payload_size.
 * Expects alts sorted ascending by max_packet_size. Falls back to the largest
 * altsetting (setting *fallback_largest = true) ONLY when payload_size == 0
 * (omitted by device, skipped readback, or explicit device quirk).
 * Returns -1 with *fallback_largest = false when count <= 0 or when
 * payload_size exceeds all available altsetting packet sizes. */
static inline int uvc_iso_pick_alt(const struct uvc_iso_alt *alts, int count, uint32_t payload_size,
				   bool *fallback_largest)
{
	if (fallback_largest)
		*fallback_largest = false;
	if (!alts || count <= 0)
		return -1;
	if (payload_size > 0) {
		for (int i = 0; i < count; i++) {
			if (alts[i].max_packet_size >= payload_size)
				return i;
		}
		return -1;
	}
	if (fallback_largest)
		*fallback_largest = true;
	return count - 1;
}

/* Chooses the number of ISO packets per URB to keep transfer sizes within ~128 KiB
 * and UVC_ISO_MAX_PACKETS (well below Linux usbfs MAX_ISO_PACKETS_PER_URB = 128).
 * usb_speed follows enum libusb_speed values (1=LOW, 2=FULL, 3=HIGH, 4=SUPER, 5=SUPER_PLUS). */
static inline unsigned uvc_iso_packets_per_transfer(uint32_t packet_size, int usb_speed, uint32_t frame_bytes)
{
	if (packet_size == 0)
		return 0;
	if (usb_speed == 1 || usb_speed == 2)
		return 8U; /* Low/FullSpeed: 1 ms per frame -> 8 ms per URB */
	if (packet_size > 65536U)
		return 2U; /* Keep SuperSpeed URBs <= ~192 KiB */

	unsigned pkts = (128U * 1024U) / packet_size;
	if (pkts < 4U)
		pkts = 4U;
	if (pkts > UVC_ISO_MAX_PACKETS)
		pkts = UVC_ISO_MAX_PACKETS;

	if (frame_bytes > 0) {
		const uint64_t frame_pkts = ((uint64_t)frame_bytes + (uint64_t)packet_size - 1U) / packet_size;
		if (frame_pkts >= 8U && frame_pkts < (uint64_t)pkts)
			pkts = (unsigned)frame_pkts;
	}
	return pkts;
}
