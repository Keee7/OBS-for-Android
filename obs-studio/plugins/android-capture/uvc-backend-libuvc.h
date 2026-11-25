/* SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum uvc_libuvc_fmt {
	UVC_LIBUVC_FMT_MJPEG = 0,
	UVC_LIBUVC_FMT_YUYV,
	UVC_LIBUVC_FMT_NV12,
	UVC_LIBUVC_FMT_RGB24,
	UVC_LIBUVC_FMT_BGR24,
	UVC_LIBUVC_FMT_P010,
	UVC_LIBUVC_FMT_UYVY,
};

struct uvc_libuvc_mode {
	enum uvc_libuvc_fmt fmt;
	uint8_t fmt_index;
	uint8_t frame_index;
	uint16_t w;
	uint16_t h;
	uint32_t max_frame_size;
	uint32_t def_interval;
	uint32_t min_iv;
	uint32_t max_iv;
	uint32_t step_iv;
	uint32_t intervals[16];
	int n_intervals;
};

struct uvc_libuvc_session;

/* Open an already-authorized Android USB file descriptor through our
 * Android-hardened libuvc 0.0.7 backend (with a dedicated libusb event thread
 * and LIBUSB_OPTION_NO_DEVICE_DISCOVERY). Does NOT take ownership of fd. */
int uvc_libuvc_open(int fd, struct uvc_libuvc_session **out, struct uvc_libuvc_mode *modes, int max_modes,
		    int *n_modes_out, int *usb_speed_out, char *err_buf, size_t err_cap);

/* Negotiate and start streaming for the chosen mode in polling mode
 * (uvc_libuvc_get_frame). Returns 0 on success or a negative uvc_error_t. */
int uvc_libuvc_start(struct uvc_libuvc_session *sess, const struct uvc_libuvc_mode *mode, uint32_t target_interval,
		     uint32_t *neg_interval_out, uint32_t *neg_payload_out, uint32_t *neg_maxframe_out,
		     char *err_buf, size_t err_cap);

/* Poll for the next completed frame.
 * Returns 1 when a frame is ready (valid until next call or close),
 * 0 on timeout, or < 0 on fatal error / disconnect. */
int uvc_libuvc_get_frame(struct uvc_libuvc_session *sess, const uint8_t **data_out, size_t *len_out,
			 uint32_t *width_out, uint32_t *height_out, enum uvc_libuvc_fmt *fmt_out,
			 uint64_t *received_ns_out, unsigned timeout_ms);

void uvc_libuvc_stats(const struct uvc_libuvc_session *sess, uint64_t *err_packets, uint64_t *bad_packets);

/* Stop streaming (with bounded cancel timeout), stop the libusb event pump
 * thread, and release all libuvc/libusb handles. Caller still closes fd. */
void uvc_libuvc_close(struct uvc_libuvc_session *sess);

/* One-shot descriptor probe for the Properties dialog mode list. */
int uvc_libuvc_probe_modes(int fd, struct uvc_libuvc_mode *modes, int max_modes);
