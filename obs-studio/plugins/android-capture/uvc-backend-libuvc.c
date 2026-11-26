/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "uvc-backend-libuvc.h"

#include <libusb-1.0/libusb.h>
#include <libuvc/libuvc.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

struct uvc_libuvc_session {
	libusb_context *usb_ctx;
	uvc_context_t *uvc_ctx;
	uvc_device_handle_t *devh;
	uvc_stream_handle_t *strmh;

	pthread_t ev_thread;
	bool ev_started;
	atomic_bool ev_stop;
	atomic_int ev_error;

	enum uvc_libuvc_fmt active_fmt;
};

static uint64_t monotonic_now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void set_err(char *buf, size_t cap, const char *fmt, int code)
{
	if (!buf || !cap)
		return;
	snprintf(buf, cap, "%s (%d: %s)", fmt, code, uvc_strerror((uvc_error_t)code));
}

static void *libuvc_event_thread(void *arg)
{
	struct uvc_libuvc_session *sess = arg;
	while (!atomic_load_explicit(&sess->ev_stop, memory_order_relaxed)) {
		struct timeval tv = {0, 20000}; /* 20 ms bounded poll for responsive shutdown */
		int r = libusb_handle_events_timeout(sess->usb_ctx, &tv);
		if (r < 0 && r != LIBUSB_ERROR_INTERRUPTED) {
			atomic_store_explicit(&sess->ev_error, r, memory_order_relaxed);
			break;
		}
	}
	return NULL;
}

static void stop_event_thread(struct uvc_libuvc_session *sess)
{
	if (!sess || !sess->ev_started)
		return;
	atomic_store_explicit(&sess->ev_stop, true, memory_order_relaxed);
	pthread_join(sess->ev_thread, NULL);
	sess->ev_started = false;
}

static bool map_libuvc_guid(const uvc_format_desc_t *fdesc, bool allow_p010, enum uvc_libuvc_fmt *out)
{
	if (!fdesc || !out)
		return false;
	if (fdesc->bDescriptorSubtype == UVC_VS_FORMAT_MJPEG) {
		*out = UVC_LIBUVC_FMT_MJPEG;
		return true;
	}
	if (fdesc->bDescriptorSubtype != UVC_VS_FORMAT_UNCOMPRESSED)
		return false;

	static const uint8_t guid_bgr[16] = {0x7d, 0xeb, 0x36, 0xe4, 0x4f, 0x52, 0xce, 0x11,
					     0x9f, 0x53, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70};
	static const uint8_t guid_rgb[16] = {0x7e, 0xeb, 0x36, 0xe4, 0x4f, 0x52, 0xce, 0x11,
					     0x9f, 0x53, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70};

	if (memcmp(fdesc->guidFormat, "YUY2", 4) == 0) {
		*out = UVC_LIBUVC_FMT_YUYV;
		return true;
	}
	if (memcmp(fdesc->guidFormat, "UYVY", 4) == 0) {
		*out = UVC_LIBUVC_FMT_UYVY;
		return true;
	}
	if (memcmp(fdesc->guidFormat, "NV12", 4) == 0) {
		*out = UVC_LIBUVC_FMT_NV12;
		return true;
	}
	if (memcmp(fdesc->guidFormat, "P010", 4) == 0) {
		if (!allow_p010)
			return false;
		*out = UVC_LIBUVC_FMT_P010;
		return true;
	}
	if (memcmp(fdesc->guidFormat, guid_bgr, 16) == 0 || memcmp(fdesc->guidFormat, "BGR3", 4) == 0) {
		*out = UVC_LIBUVC_FMT_BGR24;
		return true;
	}
	if (memcmp(fdesc->guidFormat, guid_rgb, 16) == 0 || memcmp(fdesc->guidFormat, "RGB3", 4) == 0) {
		*out = UVC_LIBUVC_FMT_RGB24;
		return true;
	}
	return false;
}

static enum uvc_frame_format to_libuvc_frame_format(enum uvc_libuvc_fmt fmt)
{
	switch (fmt) {
	case UVC_LIBUVC_FMT_MJPEG:
		return UVC_FRAME_FORMAT_MJPEG;
	case UVC_LIBUVC_FMT_YUYV:
		return UVC_FRAME_FORMAT_YUYV;
	case UVC_LIBUVC_FMT_UYVY:
		return UVC_FRAME_FORMAT_UYVY;
	case UVC_LIBUVC_FMT_NV12:
		return UVC_FRAME_FORMAT_NV12;
	case UVC_LIBUVC_FMT_P010:
		return UVC_FRAME_FORMAT_P010;
	case UVC_LIBUVC_FMT_RGB24:
		return UVC_FRAME_FORMAT_RGB;
	case UVC_LIBUVC_FMT_BGR24:
		return UVC_FRAME_FORMAT_BGR;
	default:
		return UVC_FRAME_FORMAT_ANY;
	}
}

static bool from_libuvc_frame_format(enum uvc_frame_format cf, enum uvc_libuvc_fmt *out)
{
	switch (cf) {
	case UVC_FRAME_FORMAT_MJPEG:
		*out = UVC_LIBUVC_FMT_MJPEG;
		return true;
	case UVC_FRAME_FORMAT_YUYV:
		*out = UVC_LIBUVC_FMT_YUYV;
		return true;
	case UVC_FRAME_FORMAT_UYVY:
		*out = UVC_LIBUVC_FMT_UYVY;
		return true;
	case UVC_FRAME_FORMAT_NV12:
		*out = UVC_LIBUVC_FMT_NV12;
		return true;
	case UVC_FRAME_FORMAT_P010:
		*out = UVC_LIBUVC_FMT_P010;
		return true;
	case UVC_FRAME_FORMAT_RGB:
		*out = UVC_LIBUVC_FMT_RGB24;
		return true;
	case UVC_FRAME_FORMAT_BGR:
		*out = UVC_LIBUVC_FMT_BGR24;
		return true;
	default:
		return false;
	}
}

static int collect_modes(uvc_device_handle_t *devh, bool allow_p010, struct uvc_libuvc_mode *modes, int max_modes)
{
	if (!devh || !modes || max_modes <= 0)
		return 0;
	int count = 0;
	const uvc_format_desc_t *fdesc = uvc_get_format_descs(devh);
	for (; fdesc && count < max_modes; fdesc = fdesc->next) {
		enum uvc_libuvc_fmt fmt;
		if (!map_libuvc_guid(fdesc, allow_p010, &fmt))
			continue;
		const uvc_frame_desc_t *frame = fdesc->frame_descs;
		for (; frame && count < max_modes; frame = frame->next) {
			if (frame->wWidth < 16 || frame->wWidth > 7680 || frame->wHeight < 16 || frame->wHeight > 4320)
				continue;
			if (((fmt == UVC_LIBUVC_FMT_YUYV || fmt == UVC_LIBUVC_FMT_UYVY) && (frame->wWidth & 1)) ||
			    ((fmt == UVC_LIBUVC_FMT_NV12 || fmt == UVC_LIBUVC_FMT_P010) &&
			     ((frame->wWidth | frame->wHeight) & 1)))
				continue;
			struct uvc_libuvc_mode *m = &modes[count++];
			memset(m, 0, sizeof(*m));
			m->fmt = fmt;
			m->fmt_index = fdesc->bFormatIndex;
			m->frame_index = frame->bFrameIndex;
			m->w = frame->wWidth;
			m->h = frame->wHeight;
			m->max_frame_size = frame->dwMaxVideoFrameBufferSize;
			m->def_interval = frame->dwDefaultFrameInterval;
			m->min_iv = frame->dwMinFrameInterval;
			m->max_iv = frame->dwMaxFrameInterval;
			m->step_iv = frame->dwFrameIntervalStep;
			if (frame->intervals) {
				for (const uint32_t *iv = frame->intervals; *iv && m->n_intervals < 16; ++iv)
					m->intervals[m->n_intervals++] = *iv;
			}
		}
	}
	return count;
}

static void destroy_session_storage(struct uvc_libuvc_session *sess)
{
	if (!sess)
		return;
	if (sess->strmh) {
		uvc_stream_close(sess->strmh);
		sess->strmh = NULL;
	}
	if (sess->devh) {
		uvc_close(sess->devh);
		sess->devh = NULL;
	}
	stop_event_thread(sess);
	if (sess->uvc_ctx) {
		uvc_exit(sess->uvc_ctx);
		sess->uvc_ctx = NULL;
	}
	if (sess->usb_ctx) {
		libusb_exit(sess->usb_ctx);
		sess->usb_ctx = NULL;
	}
	free(sess);
}

int uvc_libuvc_open(int fd, bool allow_p010, struct uvc_libuvc_session **out, struct uvc_libuvc_mode *modes,
		    int max_modes, int *n_modes_out, int *usb_speed_out, char *err_buf, size_t err_cap)
{
	if (out)
		*out = NULL;
	if (n_modes_out)
		*n_modes_out = 0;
	if (usb_speed_out)
		*usb_speed_out = LIBUSB_SPEED_UNKNOWN;
	if (err_buf && err_cap)
		err_buf[0] = '\0';
	if (fd < 0 || !out)
		return UVC_ERROR_INVALID_PARAM;

	struct uvc_libuvc_session *sess = calloc(1, sizeof(*sess));
	if (!sess)
		return UVC_ERROR_NO_MEM;
	atomic_init(&sess->ev_stop, false);
	atomic_init(&sess->ev_error, 0);

	const struct libusb_init_option opts[] = {{LIBUSB_OPTION_NO_DEVICE_DISCOVERY, {0}}};
	int r = libusb_init_context(&sess->usb_ctx, opts, 1);
	if (r != 0) {
		set_err(err_buf, err_cap, "libusb_init_context", r);
		free(sess);
		return r;
	}

	uvc_error_t ur = uvc_init(&sess->uvc_ctx, sess->usb_ctx);
	if (ur != UVC_SUCCESS) {
		set_err(err_buf, err_cap, "uvc_init", ur);
		libusb_exit(sess->usb_ctx);
		free(sess);
		return ur;
	}

	ur = uvc_wrap(fd, sess->uvc_ctx, &sess->devh);
	if (ur != UVC_SUCCESS || !sess->devh) {
		set_err(err_buf, err_cap, "uvc_wrap", ur);
		uvc_exit(sess->uvc_ctx);
		libusb_exit(sess->usb_ctx);
		free(sess);
		return ur ? ur : UVC_ERROR_OTHER;
	}

	if (usb_speed_out) {
		libusb_device *ld = uvc_get_libusb_handle(sess->devh)
					    ? libusb_get_device(uvc_get_libusb_handle(sess->devh))
					    : NULL;
		*usb_speed_out = ld ? libusb_get_device_speed(ld) : LIBUSB_SPEED_UNKNOWN;
	}

	if (pthread_create(&sess->ev_thread, NULL, libuvc_event_thread, sess) != 0) {
		set_err(err_buf, err_cap, "pthread_create(libuvc_event)", UVC_ERROR_OTHER);
		uvc_close(sess->devh);
		uvc_exit(sess->uvc_ctx);
		libusb_exit(sess->usb_ctx);
		free(sess);
		return UVC_ERROR_OTHER;
	}
	sess->ev_started = true;

	const int count = collect_modes(sess->devh, allow_p010, modes, max_modes);
	if (n_modes_out)
		*n_modes_out = count;

	*out = sess;
	return UVC_SUCCESS;
}

int uvc_libuvc_start(struct uvc_libuvc_session *sess, const struct uvc_libuvc_mode *mode, uint32_t target_interval,
		     bool strict_mode, uint32_t max_interval_delta, uint32_t *neg_interval_out,
		     uint32_t *neg_payload_out, uint32_t *neg_maxframe_out, char *err_buf, size_t err_cap)
{
	if (err_buf && err_cap)
		err_buf[0] = '\0';
	if (!sess || !sess->devh || !mode || sess->strmh)
		return UVC_ERROR_INVALID_PARAM;

	const enum uvc_frame_format cf = to_libuvc_frame_format(mode->fmt);

	uvc_stream_ctrl_t ctrl;
	memset(&ctrl, 0, sizeof(ctrl));
	/* Pass exact 100 ns dwFrameInterval straight into UVC PROBE so fractional
	 * NTSC rates (59.94 / 29.97 / 23.976 / 119.88 fps) are never rounded. */
	uvc_error_t ur = uvc_get_stream_ctrl_format_size_interval(sess->devh, &ctrl, cf, mode->w, mode->h,
								  target_interval);
	if (ur != UVC_SUCCESS && !strict_mode && target_interval != 0)
		ur = uvc_get_stream_ctrl_format_size_interval(sess->devh, &ctrl, cf, mode->w, mode->h, 0);
	if (ur != UVC_SUCCESS) {
		set_err(err_buf, err_cap, "uvc_get_stream_ctrl_format_size_interval", ur);
		return ur;
	}
	if (strict_mode) {
		if (ctrl.bFormatIndex != mode->fmt_index || ctrl.bFrameIndex != mode->frame_index) {
			if (err_buf && err_cap)
				snprintf(err_buf, err_cap, "device declined mode %u:%u (reported %u:%u)",
					 (unsigned)mode->fmt_index, (unsigned)mode->frame_index,
					 (unsigned)ctrl.bFormatIndex, (unsigned)ctrl.bFrameIndex);
			return UVC_LIBUVC_ERR_NOT_SUPPORTED;
		}
		if (target_interval != 0) {
			const uint32_t diff = ctrl.dwFrameInterval > target_interval
						      ? ctrl.dwFrameInterval - target_interval
						      : target_interval - ctrl.dwFrameInterval;
			if (diff > max_interval_delta) {
				if (err_buf && err_cap)
					snprintf(err_buf, err_cap,
						 "device declined interval %u (reported %u, tol=%u)",
						 (unsigned)target_interval, (unsigned)ctrl.dwFrameInterval,
						 (unsigned)max_interval_delta);
				return UVC_LIBUVC_ERR_NOT_SUPPORTED;
			}
		}
	}

	ur = uvc_stream_open_ctrl(sess->devh, &sess->strmh, &ctrl);
	if (ur != UVC_SUCCESS || !sess->strmh) {
		set_err(err_buf, err_cap, "uvc_stream_open_ctrl", ur);
		sess->strmh = NULL;
		return ur ? ur : UVC_ERROR_OTHER;
	}

	/* Polling mode (cb = NULL): zero extra callback thread, consumer drives
	 * frame reads and TurboJPEG/OBS submission on the source capture thread. */
	ur = uvc_stream_start(sess->strmh, NULL, NULL, 0);
	if (ur != UVC_SUCCESS) {
		set_err(err_buf, err_cap, "uvc_stream_start", ur);
		uvc_stream_close(sess->strmh);
		sess->strmh = NULL;
		return ur;
	}

	sess->active_fmt = mode->fmt;
	if (neg_interval_out)
		*neg_interval_out = ctrl.dwFrameInterval;
	if (neg_payload_out)
		*neg_payload_out = ctrl.dwMaxPayloadTransferSize;
	if (neg_maxframe_out)
		*neg_maxframe_out = ctrl.dwMaxVideoFrameSize;
	return UVC_SUCCESS;
}

int uvc_libuvc_stop_stream(struct uvc_libuvc_session *sess, unsigned timeout_ms)
{
	if (!sess || !sess->strmh)
		return UVC_SUCCESS;
	uvc_stream_stop_timeout(sess->strmh, timeout_ms);
	if (uvc_stream_stop_timed_out(sess->strmh))
		return UVC_ERROR_TIMEOUT;
	uvc_stream_close(sess->strmh);
	sess->strmh = NULL;
	return UVC_SUCCESS;
}

int uvc_libuvc_get_frame(struct uvc_libuvc_session *sess, const uint8_t **data_out, size_t *len_out,
			 uint32_t *width_out, uint32_t *height_out, enum uvc_libuvc_fmt *fmt_out,
			 uint64_t *received_ns_out, unsigned timeout_ms)
{
	if (!sess || !sess->strmh)
		return UVC_ERROR_INVALID_PARAM;
	const int ev_err = atomic_load_explicit(&sess->ev_error, memory_order_relaxed);
	if (ev_err < 0)
		return ev_err;

	uvc_frame_t *frame = NULL;
	const uvc_error_t ur = uvc_stream_get_frame(sess->strmh, &frame, (int32_t)(timeout_ms * 1000U));
	if (ur == UVC_ERROR_TIMEOUT)
		return 0;
	if (ur != UVC_SUCCESS)
		return ur;
	if (!frame || !frame->data || !frame->data_bytes)
		return 0;

	if (data_out)
		*data_out = (const uint8_t *)frame->data;
	if (len_out)
		*len_out = frame->data_bytes;
	if (width_out)
		*width_out = frame->width;
	if (height_out)
		*height_out = frame->height;
	if (fmt_out) {
		enum uvc_libuvc_fmt mapped = sess->active_fmt;
		from_libuvc_frame_format(frame->frame_format, &mapped);
		*fmt_out = mapped;
	}
	if (received_ns_out)
		*received_ns_out = monotonic_now_ns();
	return 1;
}

void uvc_libuvc_stats(const struct uvc_libuvc_session *sess, uint64_t *err_packets, uint64_t *bad_packets)
{
	if (!sess || !sess->strmh) {
		if (err_packets)
			*err_packets = 0;
		if (bad_packets)
			*bad_packets = 0;
		return;
	}
	uvc_stream_get_stats(sess->strmh, err_packets, bad_packets);
}

bool uvc_libuvc_close_or_keep_for_retire(struct uvc_libuvc_session *sess, unsigned timeout_ms)
{
	if (!sess)
		return true;
	if (sess->strmh) {
		uvc_stream_stop_timeout(sess->strmh, timeout_ms);
		if (uvc_stream_stop_timed_out(sess->strmh)) {
			/* Keep ev_thread running so background retirement can finish
			 * draining URBs before closing devh, usb_ctx, and Android fd. */
			return false;
		}
	}
	destroy_session_storage(sess);
	return true;
}

bool uvc_libuvc_try_drain(struct uvc_libuvc_session *sess, unsigned timeout_ms)
{
	if (!sess || !sess->strmh)
		return true;
	return uvc_stream_try_drain(sess->strmh, timeout_ms) != 0;
}

void uvc_libuvc_finish_retire(struct uvc_libuvc_session *sess)
{
	if (!sess)
		return;
	/* Stop the event thread before closing devh/usb_ctx if transfers are still
	 * abandoned after background drain budget expired. */
	if (sess->strmh && uvc_stream_stop_timed_out(sess->strmh))
		stop_event_thread(sess);
	destroy_session_storage(sess);
}

int uvc_libuvc_probe_modes(int fd, bool allow_p010, struct uvc_libuvc_mode *modes, int max_modes)
{
	struct uvc_libuvc_session *sess = NULL;
	int count = 0;
	if (uvc_libuvc_open(fd, allow_p010, &sess, modes, max_modes, &count, NULL, NULL, 0) != UVC_SUCCESS || !sess)
		return 0;
	uvc_libuvc_close_or_keep_for_retire(sess, 1000);
	return count;
}
