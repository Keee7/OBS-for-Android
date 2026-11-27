/*
 * Android MediaCodec Surface video encoders for OBS Studio.
 * Copyright (C) 2026 OBS for Android contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <obs-module.h>
#include <obs-android.h>
#include <graphics/graphics.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <android/native_window.h>
#include <android/data_space.h>
#include <android/log.h>
#include <stdarg.h>
#include <stdio.h>
#include <util/bmem.h>
#include <util/platform.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include "avc-utils.h"
#include "hevc-utils.h"
#include "av1-utils.h"
#include "encoder-health.h"
#include "shed-policy.h"
#include "color-policy.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("android-mediacodec", "en-US")
MODULE_EXPORT const char *obs_module_description(void)
{
	return "Android MediaCodec hardware video encoders";
}
struct codec_spec {
	const char *id, *codec, *mime, *name;
	int profile;
	bool p010;
};

static const struct codec_spec codecs[] = {
	{"android_mediacodec_h264", "h264", "video/avc", "Android MediaCodec H.264", 0, false},
#ifdef ENABLE_HEVC
	{"android_mediacodec_hevc", "hevc", "video/hevc", "Android MediaCodec HEVC (H.265)", 1, false},
	{"android_mediacodec_hevc_main10", "hevc", "video/hevc", "Android MediaCodec HEVC Main10", 2, true},
#endif
	{"android_mediacodec_av1", "av1", "video/av01", "Android MediaCodec AV1", 1, false},
	{"android_mediacodec_vp9", "vp9", "video/x-vnd.on2.vp9", "Android MediaCodec VP9", 1, false},
};
#define MC_COLOR_YUV420_PLANAR 19
#define MC_COLOR_YUV420_SEMIPLANAR 21
#define MC_COLOR_P010 54
#define MC_COLOR_SURFACE 0x7f000789
#define MC_REJECT_HW_FOUND (1 << 0)
#define MC_REJECT_PROFILE_OR_CF (1 << 1)
#define MC_REJECT_MODE (1 << 2)
#define MC_REJECT_SIZE_RATE (1 << 3)
#define MC_REJECT_BITRATE (1 << 4)
#define MC_REJECT_FORMAT (1 << 5)
#define MAX_QUEUED 120
/* High-stability policy (load shedding) lives in shed-policy.h so the host
 * regression test compiles the real decision logic instead of a copy. */
#define MAX_QUEUE_BYTES (32u * 1024u * 1024u)
#define MAX_ACCESS_UNIT (16u * 1024u * 1024u)
#define NO_OUTPUT_NS 5000000000ULL

static void mc_log(int level, const char *format, ...)
{
	char line[2048];
	va_list args, copy;
	va_start(args, format);
	va_copy(copy, args);
	vsnprintf(line, sizeof(line), format, copy);
	va_end(copy);
	blogva(level, format, args);
	va_end(args);
	__android_log_write(level <= LOG_ERROR ? ANDROID_LOG_ERROR :
		(level <= LOG_WARNING ? ANDROID_LOG_WARN : ANDROID_LOG_INFO), "OBS-MediaCodec", line);
}

struct packet_node {
	struct packet_node *next;
	uint8_t *data;
	size_t size;
	int64_t us;
	bool key;
};
enum mc_input_mode {
	MC_INPUT_SURFACE = 0,
	MC_INPUT_NV12,
	MC_INPUT_I420,
	MC_INPUT_P010,
};
struct encoder {
	obs_encoder_t *obs;
	AMediaCodec *codec;
	ANativeWindow *window;
	gs_android_encoder_surface_t *surface;
	pthread_t thread;
	bool thread_started, started, stopping, failed, drain_done, eos_signaled;
	pthread_mutex_t mutex;
	pthread_cond_t ready;
	struct packet_node *head, *tail;
	size_t queued;
	uint8_t *current, *extra;
	size_t extra_size;
	int fps_num, fps_den;
	int64_t bitrate, last_output_us;
	bool have_output_pts;
	bool bitrate_update_disabled;
	const char *failure_message;
	uint8_t *pending_config;
	size_t pending_size;
	const struct codec_spec *format;
	enum video_colorspace colorspace;
	enum mc_input_mode input_mode;
	int profile, color_standard, color_transfer, color_range;
	bool range_rejected;
	bool had_strict_candidate;
	bool quality_rejected;
	bool configure_attempted;
	int reject_flags;
	size_t yuv_stride, yuv_slice_height, yuv_row_bytes, yuv_height, yuv_size;
	size_t yuv_uv_stride, yuv_uv_row_bytes, yuv_uv_height, yuv_u_offset, yuv_v_offset;
	int mode;
	char *component;
	uint64_t session_id, first_submit_ns, last_report_ns;
	uint64_t delivered_frames, output_bytes, keyframes;
	struct mc_flow flow;
	uint64_t max_pending;
	uint64_t input_gap_ns;
	uint64_t input_busy_since_ns;
	size_t queue_bytes, peak_queue_bytes;
	/* 提交一帧到 MediaCodec 输入面（含 eglSwapBuffers）的耗时。稳定 60fps 时
	 * 应在几百微秒内；一旦接近 16666us 就说明交换又被 vsync 量化了，
	 * 表现为帧率掉一半、随后追帧把码率顶出尖峰。 */
	uint64_t swap_us_sum, swap_max_us;
	uint32_t swap_samples;
	/* High-stability policy: when input backs up, drop frames proactively
	 * instead of waiting for the hard limit and tearing the stream down.
	 * dropped_load counts frames skipped for this reason;
	 * submit_budget_us is the per-frame submit time budget, adapted from
	 * the measured swap cost. */
	uint64_t dropped_load;
	uint64_t submit_budget_us;
	/* 0 = healthy; rises when a submit exceeds the budget, falls when it is
	 * comfortably below. >=2 collapses the soft limit to the floor.
	 * degraded_until_ns bounds the collapse: while shedding we never submit,
	 * so the only way out is a time decay (see encode()). */
	int degraded;
	uint64_t degraded_until_ns;
	/* Current shedding state. Tracked only so each edge emits one
	 * transition log line, instead of leaving "did we drop quality?"
	 * answerable solely from the 5-second progress line. */
	bool shedding;
};

#define PROGRESS_FORMAT(event) "[android-mediacodec] session=%llu event=" event " codec=%s component=%s submitted=%llu output=%llu delivered=%llu bytes=%llu keyframes=%llu queue=%zu queue_bytes=%zu peak_queue_bytes=%zu swap_avg_us=%llu swap_max_us=%llu dropped_load=%llu elapsed_ms=%llu failed=%d"
static void progress(struct encoder *e, bool final)
{
	const uint64_t now = os_gettime_ns();
	pthread_mutex_lock(&e->mutex);
	if (!final && (!e->first_submit_ns || now - e->last_report_ns < 5000000000ULL)) {
		pthread_mutex_unlock(&e->mutex);
		return;
	}
	e->last_report_ns = now;
	const uint64_t elapsed_ms = e->first_submit_ns ? (now - e->first_submit_ns) / 1000000ULL : 0;
	const uint64_t swap_avg_us = e->swap_samples ? e->swap_us_sum / e->swap_samples : 0;
	const uint64_t swap_max_us = e->swap_max_us;
	e->swap_us_sum = e->swap_max_us = 0;
	e->swap_samples = 0;
	mc_log(LOG_INFO, final ? PROGRESS_FORMAT("summary") : PROGRESS_FORMAT("progress"),
	     (unsigned long long)e->session_id, e->format->codec,
	     e->component ? e->component : "none", (unsigned long long)e->flow.submitted,
	     (unsigned long long)e->flow.output, (unsigned long long)e->delivered_frames,
	     (unsigned long long)e->output_bytes, (unsigned long long)e->keyframes, e->queued, e->queue_bytes,
	     e->peak_queue_bytes, (unsigned long long)swap_avg_us, (unsigned long long)swap_max_us,
	     (unsigned long long)e->dropped_load, (unsigned long long)elapsed_ms, e->failed);
	pthread_mutex_unlock(&e->mutex);
}
static void fail(struct encoder *e, const char *message)
{
	mc_log(LOG_ERROR, "[android-mediacodec] session=%llu event=error codec=%s message=%s",
	     (unsigned long long)e->session_id, e->format->codec, message);
	pthread_mutex_lock(&e->mutex);
	if (!e->failed)
		e->failure_message = message;
	e->failed = true;
	pthread_cond_broadcast(&e->ready);
	pthread_mutex_unlock(&e->mutex);
}
static void publish_failure(struct encoder *e)
{
	pthread_mutex_lock(&e->mutex);
	const char *message = e->failure_message;
	pthread_mutex_unlock(&e->mutex);
	obs_encoder_set_last_error(e->obs, message ? message : "MediaCodec encoder failed");
}
/* MediaCodec generally emits Annex B. Convert four-byte length-prefixed access units if needed. */
static uint8_t *annexb(const uint8_t *src, size_t n)
{
	if (!src || n < 4)
		return NULL;
	uint8_t *out = bmemdup(src, n);
	if (!avc_to_annexb(out, n)) {
		bfree(out);
		return NULL;
	}
	return out;
}
static bool is_hevc(const struct encoder *e)
{
	return strcmp(e->format->codec, "hevc") == 0;
}

static void save_config(struct encoder *e, const uint8_t *data, size_t size)
{
	if (size > 1048576) {
		fail(e, "Codec configuration exceeds 1 MiB");
		return;
	}
	uint8_t *copy = annexb(data, size);
	if (!copy) {
		fail(e, "Unsupported Codec configuration format");
		return;
	}
	const char *error = NULL;
	pthread_mutex_lock(&e->mutex);
	if (!e->extra) {
		if (size > 1048576 || e->pending_size > 1048576 - size)
			error = "Codec configuration exceeds 1 MiB";
		else {
			e->pending_config = brealloc(e->pending_config, e->pending_size + size);
			memcpy(e->pending_config + e->pending_size, copy, size);
			e->pending_size += size;
			bool complete = is_hevc(e) ? hevc_headers_ready(e->pending_config, e->pending_size) :
				(avc_has_nal(e->pending_config, e->pending_size, 7) && avc_has_nal(e->pending_config, e->pending_size, 8));
			if (complete) {
				bool supported = is_hevc(e) ? hevc_sps_420(e->pending_config, e->pending_size, e->format->p010) :
					avc_sps_8bit_420(e->pending_config, e->pending_size);
				if (!supported)
					error = e->format->p010 ? "Codec SPS is not single-layer 10-bit 4:2:0" :
							"Codec SPS is not single-layer 8-bit 4:2:0";
				else {
					e->extra = e->pending_config;
					e->extra_size = e->pending_size;
					e->pending_config = NULL;
					e->pending_size = 0;
				}
			}
		}
	}
	pthread_mutex_unlock(&e->mutex);
	bfree(copy);
	if (error)
		fail(e, error);
}
static void save_av1_config(struct encoder *e, const uint8_t *data, size_t size)
{
	size_t header_size;
	const uint8_t *header = av1_sequence_header(data, size, &header_size);
	if (!header)
		return;
	if (header_size > 1048576) {
		fail(e, "AV1 sequence header exceeds 1 MiB");
		return;
	}
	pthread_mutex_lock(&e->mutex);
	if (!e->extra) {
		e->extra = bmemdup(header, header_size);
		e->extra_size = header_size;
	}
	pthread_mutex_unlock(&e->mutex);
}

static void format_config(struct encoder *e)
{
	AMediaFormat *f = AMediaCodec_getOutputFormat(e->codec);
	if (!f)
		return;
	mc_log(LOG_INFO, "[android-mediacodec] session=%llu event=format codec=%s format=%s",
	     (unsigned long long)e->session_id, e->format->codec, AMediaFormat_toString(f));
	int32_t std = 0, tr = 0, rng = 0;
	const bool has_std = AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_COLOR_STANDARD, &std);
	const bool has_tr = AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_COLOR_TRANSFER, &tr);
	const bool has_rng = AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_COLOR_RANGE, &rng);
	const char *color_error = NULL;
	if (!mc_color_format_accepted(e->format->p010, e->color_standard, e->color_transfer, e->color_range,
				      has_std, std, has_tr, tr, has_rng, rng, &color_error, NULL)) {
		AMediaFormat_delete(f);
		fail(e, color_error);
		return;
	}
	void *a = NULL, *b = NULL;
	size_t an = 0, bn = 0;
	AMediaFormat_getBuffer(f, "csd-0", &a, &an);
	AMediaFormat_getBuffer(f, "csd-1", &b, &bn);
	if (an > 1048576 || bn > 1048576 - an) {
		AMediaFormat_delete(f);
		fail(e, "Codec configuration exceeds 1 MiB");
		return;
	}
	if (a && an && !strcmp(e->format->codec, "av1"))
		save_av1_config(e, a, an);
	else if (a && an && strcmp(e->format->codec, "vp9")) {
		uint8_t *x = annexb(a, an), *y = b && bn ? annexb(b, bn) : NULL;
		if (x && (!bn || y)) {
			uint8_t *both = bmalloc(an + bn);
			memcpy(both, x, an);
			if (bn)
				memcpy(both + an, y, bn);
			save_config(e, both, an + bn);
			bfree(both);
		} else
			fail(e, "Unsupported codec output format headers");
		bfree(x);
		bfree(y);
	}
	AMediaFormat_delete(f);
}
/* A first access unit may contain headers AND a large keyframe. Only parameter
 * sets belong in extradata; copying VCL/SEI there corrupts hvcC/avcC muxing. */
static void inband_config(struct encoder *e, const uint8_t *data, size_t size)
{
	size_t cursor = 0, start, end, used = 0;
	uint8_t *headers = NULL;
	while (hevc_next_nal(data, size, &cursor, &start, &end)) {
		if (start >= end)
			continue;
		unsigned type = is_hevc(e) ? (data[start] >> 1) & 63 : data[start] & 31;
		bool parameter = is_hevc(e) ? (type >= 32 && type <= 34) : (type == 7 || type == 8);
		if (!parameter)
			continue;
		size_t length = end - start;
		if (length > 1048576 - 4 || used > 1048576 - 4 - length) {
			fail(e, "In-band codec parameter sets exceed 1 MiB");
			bfree(headers);
			return;
		}
		headers = brealloc(headers, used + 4 + length);
		memcpy(headers + used, "\0\0\0\1", 4);
		memcpy(headers + used + 4, data + start, length);
		used += 4 + length;
	}
	if (used)
		save_config(e, headers, used);
	bfree(headers);
}
static void *drain(void *data)
{
	struct encoder *e = data;
	int64_t eos_deadline_ns = 0;
	const bool av1 = !strcmp(e->format->codec, "av1");
	const bool vp9 = !strcmp(e->format->codec, "vp9");
	for (;;) {
		pthread_mutex_lock(&e->mutex);
		bool stop = e->stopping;
		pthread_mutex_unlock(&e->mutex);
		if (stop) {
			struct timespec now;
			if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
				fail(e, "Cannot read clock while draining MediaCodec");
				break;
			}
			int64_t now_ns = (int64_t)now.tv_sec * 1000000000LL + now.tv_nsec;
			if (!eos_deadline_ns)
				eos_deadline_ns = now_ns + 3000000000LL;
			else if (now_ns >= eos_deadline_ns) {
				fail(e, "Timed out waiting for MediaCodec EOS");
				break;
			}
		}
		AMediaCodecBufferInfo info;
		ssize_t index = AMediaCodec_dequeueOutputBuffer(e->codec, &info, 10000);
		if (index == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
			format_config(e);
			continue;
		}
		if (index == AMEDIACODEC_INFO_TRY_AGAIN_LATER)
			continue;
		if (index == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED)
			continue;
		if (index < 0) {
			fail(e, "MediaCodec output dequeue failed");
			break;
		}
		size_t capacity = 0;
		uint8_t *buf = AMediaCodec_getOutputBuffer(e->codec, (size_t)index, &capacity);
		bool valid = info.offset >= 0 && info.size >= 0 && (size_t)info.offset <= capacity &&
			     (size_t)info.size <= capacity - (size_t)info.offset;
		if (!valid || (info.size && !buf))
			fail(e, "Invalid MediaCodec output buffer");
		else if (info.flags & AMEDIACODEC_BUFFER_FLAG_PARTIAL_FRAME)
			fail(e, "Partial video output buffers are unsupported");
		else if (info.size && (info.flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG)) {
			if (av1)
				save_av1_config(e, buf + info.offset, (size_t)info.size);
			else if (!vp9)
				save_config(e, buf + info.offset, (size_t)info.size);
		} else if (info.size) {
			if (info.presentationTimeUs < 0 ||
			    (e->have_output_pts && info.presentationTimeUs < e->last_output_us)) {
				AMediaCodec_releaseOutputBuffer(e->codec, (size_t)index, false);
				fail(e, "Unexpected reordered MediaCodec output; B frames are unsupported");
				break;
			}
			e->have_output_pts = true;
			e->last_output_us = info.presentationTimeUs;
			// Copy each access unit once; validation and queue ownership share it.
			const uint8_t *payload = buf + info.offset;
			const size_t size = (size_t)info.size;
			uint8_t *access = NULL;
			if (size <= MAX_ACCESS_UNIT) {
				if (av1 || vp9) {
					/* VP9 Profile 0 starts with frame marker 2 and profile bits 0. */
					if (!vp9 || (payload[0] & 0xf0) == 0x80)
						access = bmemdup(payload, size);
				} else {
					access = annexb(payload, size);
				}
			}
			if (!access)
				fail(e, "Invalid or oversized MediaCodec access unit");
			pthread_mutex_lock(&e->mutex);
			bool have_extra = vp9 || e->extra != NULL;
			pthread_mutex_unlock(&e->mutex);
			if (access && !have_extra) {
				if (av1)
					save_av1_config(e, access, size);
				else
					inband_config(e, access, size);
				pthread_mutex_lock(&e->mutex);
				have_extra = e->extra != NULL;
				pthread_mutex_unlock(&e->mutex);
				if (!have_extra)
					fail(e, "Codec parameter sets unavailable before first video frame");
			}
			if (access && have_extra) {
				struct packet_node *p = bzalloc(sizeof(*p));
				p->size = (size_t)info.size;
				p->data = access;
				access = NULL;
				p->us = info.presentationTimeUs;
				p->key = (info.flags & AMEDIACODEC_BUFFER_FLAG_KEY_FRAME) != 0;
				pthread_mutex_lock(&e->mutex);
				if (e->queued >= MAX_QUEUED || p->size > MAX_QUEUE_BYTES - e->queue_bytes) {
					e->failed = true;
					e->failure_message = "MediaCodec output queue exceeded its packet/byte limit";
					bfree(p->data);
					bfree(p);
				} else {
					if (e->tail)
						e->tail->next = p;
					else
						e->head = p;
					e->tail = p;
					e->queued++;
					e->queue_bytes += p->size;
					if (e->queue_bytes > e->peak_queue_bytes) e->peak_queue_bytes = e->queue_bytes;
					mc_flow_output(&e->flow, os_gettime_ns());
					e->output_bytes += p->size;
					if (p->key) e->keyframes++;
					if (e->flow.output == 1)
						mc_log(LOG_INFO, "[android-mediacodec] session=%llu event=first-packet codec=%s headers=valid keyframe=%d bytes=%zu",
							 (unsigned long long)e->session_id, e->format->codec, p->key, p->size);
				}
				pthread_cond_broadcast(&e->ready);
				pthread_mutex_unlock(&e->mutex);
			}
			bfree(access);
		}
		bool eos = (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) != 0;
		AMediaCodec_releaseOutputBuffer(e->codec, (size_t)index, false);
		pthread_mutex_lock(&e->mutex);
		bool failed = e->failed;
		pthread_mutex_unlock(&e->mutex);
		if (eos || failed)
			break;
	}
	pthread_mutex_lock(&e->mutex);
	e->drain_done = true;
	pthread_cond_broadcast(&e->ready);
	pthread_mutex_unlock(&e->mutex);
	return NULL;
}
static const char *name(void *type_data)
{
	return ((const struct codec_spec *)type_data)->name;
}

static void defaults(obs_data_t *s)
{
	obs_data_set_default_int(s, "bitrate", 10000);
	obs_data_set_default_int(s, "keyint_sec", 2);
	obs_data_set_default_int(s, "rate_mode", 2);
	obs_data_set_default_int(s, "hardware_quality", 0);
}
static bool mode_modified(obs_properties_t *p, obs_property_t *property, obs_data_t *s)
{
	UNUSED_PARAMETER(property);
	obs_property_set_visible(obs_properties_get(p, "bitrate"), obs_data_get_int(s, "rate_mode") != 0);
	return true;
}
static obs_properties_t *properties(void *unused)
{
	UNUSED_PARAMETER(unused);
	obs_properties_t *p = obs_properties_create();
	obs_property_t *mode = obs_properties_add_list(p, "rate_mode", "Rate control", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(mode, "CBR", 2);
	obs_property_list_add_int(mode, "VBR", 1);
	obs_property_list_add_int(mode, "CQ (device quality range)", 0);
	obs_property_set_modified_callback(mode, mode_modified);
	obs_property_t *quality = obs_properties_add_list(p, "hardware_quality", "Hardware quality", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(quality, "Automatic (CQ: midpoint)", 0);
	obs_property_list_add_int(quality, "Low", 1);
	obs_property_list_add_int(quality, "Medium", 2);
	obs_property_list_add_int(quality, "High", 3);
	obs_property_set_long_description(quality, "CQ uses the device quality range; CBR/VBR use its complexity range. Explicit levels require an adjustable range on a compatible hardware encoder; otherwise starting output fails. Higher complexity may reduce performance.");
	obs_properties_add_int(p, "bitrate", obs_module_text("Bitrate"), 100, 100000, 100);
	obs_properties_add_int(p, "keyint_sec", obs_module_text("KeyframeInterval"), 1, 10, 1);
	return p;
}
static bool signal_end_of_input(struct encoder *e)
{
	if (e->input_mode == MC_INPUT_SURFACE)
		return AMediaCodec_signalEndOfInputStream(e->codec) == AMEDIA_OK;
	const ssize_t index = AMediaCodec_dequeueInputBuffer(e->codec, 200000);
	return index >= 0 && AMediaCodec_queueInputBuffer(e->codec, (size_t)index, 0, 0, 0,
							 AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) == AMEDIA_OK;
}

static void destroy(void *data)
{
	struct encoder *e = data;
	if (!e)
		return;
	if (e->thread_started) {
		pthread_mutex_lock(&e->mutex);
		bool signal = !e->eos_signaled;
		e->eos_signaled = true;
		e->stopping = true;
		pthread_mutex_unlock(&e->mutex);
		if (signal && !signal_end_of_input(e))
			mc_log(LOG_WARNING, "[android-mediacodec] EOS signal failed");
		pthread_join(e->thread, NULL);
	}
	if (e->surface) {
		obs_enter_graphics();
		gs_android_encoder_surface_destroy(e->surface);
		obs_leave_graphics();
	}
	if (e->window)
		ANativeWindow_release(e->window);
	if (e->codec) {
		if (e->started)
			AMediaCodec_stop(e->codec);
		AMediaCodec_delete(e->codec);
	}
	while (e->head) {
		struct packet_node *p = e->head;
		e->head = p->next;
		bfree(p->data);
		bfree(p);
	}
	bfree(e->current);
	progress(e, true);
	bfree(e->component);
	bfree(e->extra);
	bfree(e->pending_config);
	pthread_cond_destroy(&e->ready);
	pthread_mutex_destroy(&e->mutex);
	bfree(e);
}
static void release_candidate(struct encoder *e)
{
	if (e->surface) {
		obs_enter_graphics();
		gs_android_encoder_surface_destroy(e->surface);
		obs_leave_graphics();
		e->surface = NULL;
	}
	if (e->window) {
		ANativeWindow_release(e->window);
		e->window = NULL;
	}
	if (e->codec) {
		if (e->started)
			AMediaCodec_stop(e->codec);
		AMediaCodec_delete(e->codec);
		e->codec = NULL;
		e->started = false;
	}
}

static bool read_yuv_layout(struct encoder *e, int width, int height, int32_t expected_color)
{
	const bool p010 = expected_color == MC_COLOR_P010;
	const bool i420 = expected_color == MC_COLOR_YUV420_PLANAR;
	if ((width | height) & 1)
		return false;
	AMediaFormat *input = AMediaCodec_getInputFormat(e->codec);
	if (!input)
		return false;
	int32_t color = 0, stride = 0, slice = 0;
	AMediaFormat_getInt32(input, AMEDIAFORMAT_KEY_COLOR_FORMAT, &color);
	AMediaFormat_getInt32(input, "stride", &stride);
	AMediaFormat_getInt32(input, "slice-height", &slice);
	AMediaFormat_delete(input);
	const int32_t min_stride = p010 ? width * 2 : width;
	if (!stride)
		stride = min_stride;
	if (!slice)
		slice = height;
	if (color != expected_color || stride < min_stride || slice < height || (stride & 1) || (slice & 1) ||
	    (uint64_t)stride * (uint64_t)slice * 3 / 2 > INT32_MAX)
		return false;
	e->yuv_stride = (size_t)stride;
	e->yuv_slice_height = (size_t)slice;
	e->yuv_row_bytes = (size_t)min_stride;
	e->yuv_height = (size_t)height;
	e->yuv_u_offset = e->yuv_stride * e->yuv_slice_height;
	if (i420) {
		e->input_mode = MC_INPUT_I420;
		e->yuv_uv_stride = e->yuv_stride / 2;
		e->yuv_uv_row_bytes = (size_t)width / 2;
		e->yuv_uv_height = (size_t)height / 2;
		const size_t uv_slice = e->yuv_slice_height / 2;
		e->yuv_v_offset = e->yuv_u_offset + e->yuv_uv_stride * uv_slice;
		e->yuv_size = e->yuv_v_offset + e->yuv_uv_stride * uv_slice;
	} else {
		e->input_mode = p010 ? MC_INPUT_P010 : MC_INPUT_NV12;
		e->yuv_uv_stride = e->yuv_stride;
		e->yuv_uv_row_bytes = e->yuv_row_bytes;
		e->yuv_uv_height = (size_t)height / 2;
		e->yuv_v_offset = 0;
		e->yuv_size = e->yuv_stride * e->yuv_slice_height * 3 / 2;
	}
	return true;
}

static int32_t mc_dataspace(int color_standard, int color_transfer, int color_range)
{
	int32_t std = ADATASPACE_STANDARD_BT709;
	if (color_standard == MC_COLOR_STANDARD_BT2020)
		std = ADATASPACE_STANDARD_BT2020;
	else if (color_standard == MC_COLOR_STANDARD_BT601_NTSC)
		std = ADATASPACE_STANDARD_BT601_525;
	else if (color_standard == MC_COLOR_STANDARD_BT601_PAL)
		std = ADATASPACE_STANDARD_BT601_625;

	int32_t tr = ADATASPACE_TRANSFER_SMPTE_170M;
	if (color_transfer == MC_COLOR_TRANSFER_ST2084)
		tr = ADATASPACE_TRANSFER_ST2084;
	else if (color_transfer == MC_COLOR_TRANSFER_HLG)
		tr = ADATASPACE_TRANSFER_HLG;

	int32_t rng = color_range == MC_COLOR_RANGE_FULL ? ADATASPACE_RANGE_FULL : ADATASPACE_RANGE_LIMITED;
	return std | tr | rng;
}

static void set_hdr10_metadata(AMediaFormat *format)
{
	float peak = obs_get_video_hdr_nominal_peak_level();
	const uint16_t nits = peak > 0.f && peak <= 10000.f ? (uint16_t)peak : 1000;
	/* CTA-861.3 static metadata type 1, with Rec.2020 primaries and D65 white. */
	const uint16_t values[] = {35400, 14600, 8500, 39850, 6550, 2300, 15635, 16450, nits, 0, nits, nits};
	uint8_t data[1 + sizeof(values)] = {0};
	for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
		data[1 + i * 2] = (uint8_t)values[i];
		data[2 + i * 2] = (uint8_t)(values[i] >> 8);
	}
	AMediaFormat_setBuffer(format, AMEDIAFORMAT_KEY_HDR_STATIC_INFO, data, sizeof(data));
}

static bool probe_limited_range_accepted(const char *component, AMediaFormat *format)
{
	AMediaCodec *probe = AMediaCodec_createCodecByName(component);
	if (!probe)
		return false;
	AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_COLOR_RANGE, MC_COLOR_RANGE_LIMITED);
	const bool ok = AMediaCodec_configure(probe, format, NULL, NULL, AMEDIACODEC_CONFIGURE_FLAG_ENCODE) == AMEDIA_OK;
	AMediaCodec_delete(probe);
	return ok;
}

static bool input_color_format_accepted(struct encoder *e, const char *component, bool *range_mismatch)
{
	AMediaFormat *input = AMediaCodec_getInputFormat(e->codec);
	if (!input)
		return true;
	int32_t std = 0, tr = 0, rng = 0;
	const bool has_std = AMediaFormat_getInt32(input, AMEDIAFORMAT_KEY_COLOR_STANDARD, &std);
	const bool has_tr = AMediaFormat_getInt32(input, AMEDIAFORMAT_KEY_COLOR_TRANSFER, &tr);
	const bool has_rng = AMediaFormat_getInt32(input, AMEDIAFORMAT_KEY_COLOR_RANGE, &rng);
	const char *reason = NULL;
	const bool ok = mc_color_format_accepted(e->format->p010, e->color_standard, e->color_transfer, e->color_range,
						 has_std, std, has_tr, tr, has_rng, rng, &reason, range_mismatch);
	AMediaFormat_delete(input);
	if (!ok)
		mc_log(LOG_WARNING,
		       "[android-mediacodec] candidate %s rejected color configuration (%s): std=%d(%d) tr=%d(%d) range=%d(%d)",
		       component, reason ? reason : "mismatch", has_std ? std : -1, e->color_standard,
		       has_tr ? tr : -1, e->color_transfer, has_rng ? rng : -1, e->color_range);
	return ok;
}

static const char *input_mode_label(enum mc_input_mode mode)
{
	switch (mode) {
	case MC_INPUT_P010:
		return "P010";
	case MC_INPUT_NV12:
		return "NV12";
	case MC_INPUT_I420:
		return "I420";
	default:
		return "Surface";
	}
}

static bool start_candidate(struct encoder *e, const char *component, int w, int h, int query_num, int keyint,
			    int control_value, int32_t input_color_format, bool relaxed, const char *mismatch)
{
	const char *fail_stage = "createCodecByName";
	media_status_t fail_status = AMEDIA_OK;
	e->codec = AMediaCodec_createCodecByName(component);
	if (!e->codec)
		goto error;
	AMediaFormat *f = AMediaFormat_new();
	if (!f) {
		fail_stage = "AMediaFormat_new";
		goto error;
	}
	AMediaFormat_setString(f, AMEDIAFORMAT_KEY_MIME, e->format->mime);
	if (e->profile)
		AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_PROFILE, e->profile);
	AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_WIDTH, w);
	AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_HEIGHT, h);
	AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_COLOR_FORMAT, input_color_format);
	if (e->mode != 0)
		AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_BIT_RATE, (int32_t)e->bitrate);
	AMediaFormat_setFloat(f, AMEDIAFORMAT_KEY_FRAME_RATE, (float)query_num / e->fps_den);
	AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_I_FRAME_INTERVAL, keyint);
	AMediaFormat_setInt32(f, "bitrate-mode", e->mode);
	if (control_value >= 0)
		AMediaFormat_setInt32(f, e->mode == 0 ? "quality" : "complexity", control_value);
	AMediaFormat_setInt32(f, "max-bframes", 0);
	AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_COLOR_STANDARD, e->color_standard);
	AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_COLOR_TRANSFER, e->color_transfer);
	AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_COLOR_RANGE, e->color_range);
	if (e->colorspace == VIDEO_CS_2100_PQ)
		set_hdr10_metadata(f);
	e->configure_attempted = true;
	media_status_t status = AMediaCodec_configure(e->codec, f, NULL, NULL, AMEDIACODEC_CONFIGURE_FLAG_ENCODE);
	if (status != AMEDIA_OK) {
		fail_stage = "configure";
		fail_status = status;
		if (e->color_range == MC_COLOR_RANGE_FULL) {
			/* Probe on a clean instance only on failure so a driver that rejects
			 * COLOR_RANGE_FULL surfaces an actionable error instead of a rate-control error. */
			AMediaCodec_delete(e->codec);
			e->codec = NULL;
			if (probe_limited_range_accepted(component, f))
				e->range_rejected = true;
		}
		AMediaFormat_delete(f);
		goto error;
	}
	AMediaFormat_delete(f);
	bool range_mismatch = false;
	if (!input_color_format_accepted(e, component, &range_mismatch)) {
		fail_stage = "input_color_format";
		if (range_mismatch)
			e->range_rejected = true;
		goto error;
	}
	if (input_color_format == MC_COLOR_SURFACE) {
		status = AMediaCodec_createInputSurface(e->codec, &e->window);
		if (status != AMEDIA_OK || !e->window) {
			fail_stage = "createInputSurface";
			fail_status = status;
			goto error;
		}
		obs_enter_graphics();
		e->surface = gs_android_encoder_surface_create(e->window);
		obs_leave_graphics();
		if (!e->surface) {
			fail_stage = "encoder_surface_create";
			goto error;
		}
		if (e->color_range == MC_COLOR_RANGE_FULL) {
			const int32_t dataspace = mc_dataspace(e->color_standard, e->color_transfer, e->color_range);
			if (ANativeWindow_setBuffersDataSpace(e->window, dataspace) != 0)
				mc_log(LOG_WARNING,
				       "[android-mediacodec] ANativeWindow_setBuffersDataSpace(0x%08x) rejected for %s",
				       dataspace, component);
		}
		e->input_mode = MC_INPUT_SURFACE;
	} else {
		if (!read_yuv_layout(e, w, h, input_color_format)) {
			fail_stage = "read_yuv_layout";
			goto error;
		}
	}
	status = AMediaCodec_start(e->codec);
	if (status != AMEDIA_OK) {
		fail_stage = "start";
		fail_status = status;
		goto error;
	}
	e->started = true;
	e->component = bstrdup(component);
	mc_log(LOG_INFO,
	       "[android-mediacodec] session=%llu event=started codec=%s component=%s hardware=true software=false input=%s width=%d height=%d fps_num=%d fps_den=%d bitrate=%lld mode=%d profile=%d color_standard=%d color_transfer=%d color_range=%d relaxed=%d mismatch=%s",
	       (unsigned long long)e->session_id, e->format->codec, component, input_mode_label(e->input_mode),
	       w, h, query_num, e->fps_den, (long long)e->bitrate, e->mode, e->profile, e->color_standard,
	       e->color_transfer, e->color_range, relaxed ? 1 : 0, (mismatch && *mismatch) ? mismatch : "none");
	return true;
error:
	mc_log(LOG_WARNING,
	       "[android-mediacodec] Codec %s (cf=0x%x relaxed=%d mismatch=%s) failed at stage=%s status=%d",
	       component, (unsigned int)input_color_format, relaxed ? 1 : 0,
	       (mismatch && *mismatch) ? mismatch : "none", fail_stage, (int)fail_status);
	release_candidate(e);
	return false;
}
static enum mc_cs_kind to_mc_cs(enum video_colorspace cs)
{
	switch (cs) {
	case VIDEO_CS_DEFAULT:
		return MC_CS_DEFAULT;
	case VIDEO_CS_709:
		return MC_CS_709;
	case VIDEO_CS_601:
		return MC_CS_601;
	case VIDEO_CS_SRGB:
		return MC_CS_SRGB;
	case VIDEO_CS_2100_PQ:
		return MC_CS_2100_PQ;
	case VIDEO_CS_2100_HLG:
		return MC_CS_2100_HLG;
	default:
		return MC_CS_UNKNOWN;
	}
}

static enum mc_range_kind to_mc_range(enum video_range_type range)
{
	switch (range) {
	case VIDEO_RANGE_DEFAULT:
		return MC_RANGE_DEFAULT;
	case VIDEO_RANGE_PARTIAL:
		return MC_RANGE_PARTIAL;
	case VIDEO_RANGE_FULL:
		return MC_RANGE_FULL;
	default:
		return MC_RANGE_UNKNOWN;
	}
}

static enum video_range_type from_mc_range(enum mc_range_kind range)
{
	return mc_resolve_range_kind(range) == MC_RANGE_FULL ? VIDEO_RANGE_FULL : VIDEO_RANGE_PARTIAL;
}

static bool try_start_candidates(struct encoder *e, int w, int h, int query_num, int keyint, int quality, bool raw_yuv)
{
	const char *query_proc = raw_yuv ? "android_mediacodec_query_raw_component" : "android_mediacodec_query_component";
	for (int index = 0; index < 128; index++) {
		calldata_t cd;
		calldata_init(&cd);
		calldata_set_int(&cd, "width", w);
		calldata_set_int(&cd, "height", h);
		calldata_set_int(&cd, "fps_num", query_num);
		calldata_set_int(&cd, "fps_den", e->fps_den);
		calldata_set_int(&cd, "index", index);
		calldata_set_string(&cd, "mime", e->format->mime);
		calldata_set_int(&cd, "bitrate", e->mode == 0 ? 0 : e->bitrate);
		calldata_set_int(&cd, "mode", e->mode);
		calldata_set_int(&cd, "profile", e->profile);
		calldata_set_bool(&cd, "p010", e->format->p010);
		char *component = NULL;
		char *mismatch = NULL;
		bool relaxed = false;
		bool relaxed_mode = false;
		if (proc_handler_call(obs_get_proc_handler(), query_proc, &cd)) {
			const char *result = calldata_string(&cd, "name");
			if (result && *result)
				component = bstrdup(result);
			relaxed = calldata_bool(&cd, "relaxed");
			relaxed_mode = calldata_bool(&cd, "relaxed_mode");
			const char *mm = calldata_string(&cd, "mismatch");
			if (mm && *mm)
				mismatch = bstrdup(mm);
			e->reject_flags |= (int)calldata_int(&cd, "reject_flags");
		}
		calldata_free(&cd);
		if (!component) {
			bfree(mismatch);
			break;
		}
		if (!relaxed)
			e->had_strict_candidate = true;
		mc_log(LOG_INFO,
		       "[android-mediacodec] trying %s (%s, tier=%s%s%s) for %dx%d at %d/%d fps mode=%d",
		       component, e->format->p010 ? "p010" : (raw_yuv ? "bytebuffer" : "surface"),
		       relaxed_mode ? "relaxed_mode" : (relaxed ? "relaxed" : "strict"),
		       mismatch ? " mismatch=" : "", mismatch ? mismatch : "", w, h, query_num, e->fps_den, e->mode);
		int control_value = -1;
		calldata_t controls;
		calldata_init(&controls);
		calldata_set_string(&controls, "name", component);
		calldata_set_string(&controls, "mime", e->format->mime);
		calldata_set_int(&controls, "mode", e->mode);
		bool controls_ok = proc_handler_call(obs_get_proc_handler(), "android_mediacodec_query_controls", &controls) &&
			calldata_bool(&controls, "supported");
		int64_t low = calldata_int(&controls, e->mode == 0 ? "quality_min" : "complexity_min");
		int64_t high = calldata_int(&controls, e->mode == 0 ? "quality_max" : "complexity_max");
		calldata_free(&controls);
		bool supported = controls_ok;
		if (controls_ok && (quality || e->mode == 0) && (low < 0 || high < low || high > INT32_MAX)) {
			supported = false;
			if (quality)
				e->quality_rejected = true;
		}
		if (controls_ok && quality && high == low) {
			supported = false;
			e->quality_rejected = true;
		}
		if (!supported && relaxed_mode && quality == 0 && e->mode != 0) {
			/* Vendor XML omitted CBR/VBR support, and Automatic quality needs no
			 * complexity range: probe real AMediaCodec_configure() to decide. */
			supported = true;
		}
		if (supported && controls_ok && quality)
			control_value = (int)(low + ((high - low) * (quality - 1) + 1) / 2);
		else if (supported && controls_ok && e->mode == 0)
			control_value = (int)(low + (high - low) / 2);
		mc_log(LOG_INFO,
		       "[android-mediacodec] component=%s mode=%d control=%d range=%lld..%lld controls_ok=%d try=%d",
		       component, e->mode, control_value, (long long)low, (long long)high, controls_ok, supported);
		bool started = false;
		if (supported) {
			if (e->format->p010)
				started = start_candidate(e, component, w, h, query_num, keyint, control_value,
							  MC_COLOR_P010, relaxed, mismatch);
			else if (!raw_yuv)
				started = start_candidate(e, component, w, h, query_num, keyint, control_value,
							  MC_COLOR_SURFACE, relaxed, mismatch);
			else
				started = start_candidate(e, component, w, h, query_num, keyint, control_value,
							  MC_COLOR_YUV420_SEMIPLANAR, relaxed, mismatch) ||
					  start_candidate(e, component, w, h, query_num, keyint, control_value,
							  MC_COLOR_YUV420_PLANAR, relaxed, mismatch);
		}
		bfree(component);
		bfree(mismatch);
		if (started)
			return true;
	}
	return false;
}

static const char *classify_init_failure(const struct encoder *e)
{
	if (e->range_rejected)
		return "MediaCodec hardware encoder does not support Full color range; set Color Range to Partial in Video settings";
	if (e->format->p010 && (!(e->reject_flags & MC_REJECT_HW_FOUND) || (e->reject_flags & MC_REJECT_PROFILE_OR_CF)))
		return "No compatible MediaCodec Main10 hardware encoder found on this device";
	if (!(e->reject_flags & MC_REJECT_HW_FOUND))
		return "No compatible hardware encoder found for this codec on this device";
	if (e->quality_rejected && !e->configure_attempted)
		return "Hardware encoder does not support manual Hardware quality levels for the selected rate control (set Hardware quality to Automatic)";
	if (!e->had_strict_candidate) {
		if (e->reject_flags & MC_REJECT_MODE) {
			if (e->mode == 2)
				return "Hardware encoder does not support CBR rate control on this device; try switching Rate control to VBR in Output settings";
			if (e->mode == 1)
				return "Hardware encoder does not support VBR rate control on this device; try switching Rate control to CBR in Output settings";
			return "Hardware encoder does not support CQ rate control on this device; try switching Rate control to CBR or VBR in Output settings";
		}
		if (e->reject_flags & MC_REJECT_SIZE_RATE)
			return "Hardware encoder does not support the requested resolution or frame rate; try lowering output resolution or FPS in Video settings";
		if (e->reject_flags & MC_REJECT_BITRATE)
			return "Hardware encoder rejected the requested bitrate; try adjusting Bitrate in Output settings";
		if ((e->reject_flags & MC_REJECT_PROFILE_OR_CF) && !e->configure_attempted)
			return "Hardware encoder does not support the required codec profile or input format on this device";
	}
	if (e->configure_attempted)
		return e->format->p010
			       ? "MediaCodec Main10 hardware encoder initialization failed (configure/start rejected); see log for details"
			       : "Hardware encoder initialization failed (MediaCodec configure/start rejected); see log for details";
	return "No compatible hardware encoder supports the selected rate control and quality, or initialization failed";
}

static void *create(obs_data_t *s, obs_encoder_t *obs)
{
	struct encoder *e = bzalloc(sizeof(*e));
	e->obs = obs;
	e->format = obs_encoder_get_type_data(obs);
	e->session_id = os_gettime_ns();
	pthread_mutex_init(&e->mutex, NULL);
	pthread_condattr_t attr;
	if (pthread_condattr_init(&attr) != 0) {
		pthread_mutex_destroy(&e->mutex);
		bfree(e);
		obs_encoder_set_last_error(obs, "Cannot initialize MediaCodec synchronization");
		return NULL;
	}
	int sync_status = pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
	if (!sync_status)
		sync_status = pthread_cond_init(&e->ready, &attr);
	pthread_condattr_destroy(&attr);
	if (sync_status) {
		pthread_mutex_destroy(&e->mutex);
		bfree(e);
		obs_encoder_set_last_error(obs, "Cannot initialize MediaCodec synchronization");
		return NULL;
	}
	video_t *parent = obs_encoder_parent_video(obs), *effective = obs_encoder_video(obs);
	const struct video_output_info *base = parent ? video_output_get_info(parent) : NULL;
	const struct video_output_info *actual = effective ? video_output_get_info(effective) : NULL;
	if (!base || !actual || !base->fps_num || base->fps_num > INT32_MAX || !actual->fps_num ||
	    actual->fps_num > INT32_MAX || !actual->fps_den || actual->fps_den > INT32_MAX) {
		fail(e, "Cannot determine video frame rate");
		goto error;
	}
	struct obs_video_info global_video;
	if (!obs_get_video_info(&global_video)) {
		fail(e, "Cannot read active OBS video configuration");
		goto error;
	}
	struct mc_color_config color_cfg;
	const char *color_error = NULL;
	const bool p010_input = actual->format == VIDEO_FORMAT_P010 || actual->format == VIDEO_FORMAT_I010;
	if (!mc_resolve_color_config(e->format->p010, to_mc_cs(global_video.colorspace), to_mc_cs(actual->colorspace),
				     p010_input, to_mc_range(actual->range), &color_cfg, &color_error)) {
		fail(e, color_error);
		goto error;
	}
	e->colorspace = actual->colorspace;
	e->profile = e->format->p010 ? obs_android_hevc_main10_profile(color_cfg.pq) : e->format->profile;
	e->color_standard = color_cfg.color_standard;
	e->color_transfer = color_cfg.color_transfer;
	e->color_range = color_cfg.color_range;
	e->fps_num = (int)base->fps_num;
	e->fps_den = (int)actual->fps_den;
	int query_num = (int)actual->fps_num;
	/* Two-frame-time submit budget: exceeding it means eglSwapBuffers is
	 * costing more than the frame interval, i.e. the codec cannot keep up.
	 * Used to shrink the soft limit adaptively (see encode()). */
	e->submit_budget_us = (uint64_t)e->fps_den * 2000000ULL / (uint64_t)query_num;
	if (e->submit_budget_us < 4000)
		e->submit_budget_us = 4000;
	/* Hard abort depth: 5 seconds of unconsumed frames. Capped at
	 * MAX_QUEUED because the output packet queue cannot absorb more -- a
	 * deeper input backlog could only ever overflow the output queue,
	 * which aborts with a different and far more confusing error. Kept
	 * well above the soft limit so shedding, not aborting, is the first
	 * thing that happens. */
	e->max_pending = ((uint64_t)query_num * 5 + e->fps_den - 1) / e->fps_den;
	if (e->max_pending > MAX_QUEUED)
		e->max_pending = MAX_QUEUED;
	if (e->max_pending < (uint64_t)MC_SHED_SOFT_PENDING * 2)
		e->max_pending = (uint64_t)MC_SHED_SOFT_PENDING * 2;
	e->input_gap_ns = (uint64_t)e->fps_den * 5000000000ULL / query_num;
	if (e->input_gap_ns < 1000000000ULL)
		e->input_gap_ns = 1000000000ULL;
	int w = (int)obs_encoder_get_width(obs), h = (int)obs_encoder_get_height(obs);
	if (e->format->p010 && ((w | h) & 1)) {
		fail(e, "MediaCodec Main10 requires even frame dimensions");
		goto error;
	}
	int64_t kbps = obs_data_get_int(s, "bitrate"), keyint_value = obs_data_get_int(s, "keyint_sec");
	int64_t mode = obs_data_get_int(s, "rate_mode"), quality = obs_data_get_int(s, "hardware_quality");
	if (mode < 0 || mode > 2 || quality < 0 || quality > 3) {
		fail(e, "Invalid MediaCodec rate control or hardware quality setting");
		goto error;
	}
	e->mode = (int)mode;
	if (kbps <= 0 || kbps > INT32_MAX / 1000 || keyint_value < 0 || keyint_value > INT32_MAX) {
		fail(e, "Invalid bitrate or keyframe interval");
		goto error;
	}
	e->bitrate = kbps * 1000;
	int keyint = keyint_value ? (int)keyint_value : 2;
	if (e->format->p010) {
		try_start_candidates(e, w, h, query_num, keyint, (int)quality, false);
	} else {
		const bool try_surface = (obs_encoder_get_caps(obs) & OBS_ENCODER_CAP_PASS_TEXTURE) != 0;
		bool surface_range_rejected = false;
		if (try_surface) {
			try_start_candidates(e, w, h, query_num, keyint, (int)quality, false);
			surface_range_rejected = e->range_rejected;
		}
		if (!e->started && !((w | h) & 1)) {
			e->range_rejected = false;
			if (try_surface)
				mc_log(LOG_WARNING,
				       "[android-mediacodec] Surface direct encoding failed for %s (%dx%d range=%d); falling back to ByteBuffer YUV",
				       e->format->codec, w, h, e->color_range);
			if (try_start_candidates(e, w, h, query_num, keyint, (int)quality, true)) {
				obs_encoder_fallback_to_raw(obs);
				mc_log(LOG_INFO,
				       "[android-mediacodec] session=%llu event=bytebuffer-fallback codec=%s input=%s color_range=%d",
				       (unsigned long long)e->session_id, e->format->codec,
				       input_mode_label(e->input_mode), e->color_range);
			} else if (surface_range_rejected && !e->range_rejected) {
				e->range_rejected = true;
			}
		} else if (!e->started && try_surface && ((w | h) & 1)) {
			mc_log(LOG_WARNING,
			       "[android-mediacodec] Skipping ByteBuffer YUV fallback for odd frame dimensions %dx%d",
			       w, h);
		}
	}
	if (!e->started) {
		fail(e, classify_init_failure(e));
		goto error;
	}
	if (pthread_create(&e->thread, NULL, drain, e) != 0) {
		fail(e, "Cannot start MediaCodec output thread");
		goto error;
	}
	e->thread_started = true;
	return e;
error:
	publish_failure(e);
	destroy(e);
	return NULL;
}
static bool take_packet(struct encoder *e, struct encoder_packet *packet, bool *received, bool wait)
{
	*received = false;
	struct timespec deadline = {0};
	if (wait) {
		if (clock_gettime(CLOCK_MONOTONIC, &deadline) != 0) {
			fail(e, "Cannot read clock while waiting for MediaCodec");
			publish_failure(e);
			return false;
		}
		deadline.tv_sec += 4;
	}
	pthread_mutex_lock(&e->mutex);
	while (wait && !e->head && !e->drain_done && !e->failed) {
		if (pthread_cond_timedwait(&e->ready, &e->mutex, &deadline) != 0) {
			e->failed = true;
			e->failure_message = "Timed out waiting for MediaCodec output";
			break;
		}
	}
	bool failed = e->failed;
	struct packet_node *p = e->head;
	if (p) {
		e->head = p->next;
		if (!e->head)
			e->tail = NULL;
		e->queued--;
		e->queue_bytes -= p->size;
		e->delivered_frames++;
	}
	pthread_mutex_unlock(&e->mutex);
	if (failed) {
		if (p) {
			bfree(p->data);
			bfree(p);
		}
		publish_failure(e);
		return false;
	}
	if (p) {
		int64_t pts;
		if (!avc_us_to_pts(p->us, e->fps_num, &pts)) {
			bfree(p->data);
			bfree(p);
			fail(e, "Invalid MediaCodec output timestamp");
			publish_failure(e);
			return false;
		}
		bfree(e->current);
		e->current = p->data;
		packet->data = e->current;
		packet->size = p->size;
		packet->pts = pts;
		packet->dts = packet->pts;
		packet->type = OBS_ENCODER_VIDEO;
		packet->keyframe = p->key;
		*received = true;
		bfree(p);
	}
	return true;
}
enum submit_result { SUBMIT_OK, SUBMIT_BUSY, SUBMIT_FAILED };

static void copy_plane_padded(uint8_t *dst, size_t dst_stride, const uint8_t *src, size_t src_stride,
			      size_t row_bytes, size_t rows, size_t slice_rows, uint8_t pad_byte)
{
	if (dst_stride == row_bytes && src_stride == row_bytes) {
		memcpy(dst, src, row_bytes * rows);
	} else {
		const size_t pad_bytes = dst_stride - row_bytes;
		uint8_t *out = dst;
		for (size_t row = 0; row < rows; row++) {
			memcpy(out, src, row_bytes);
			if (pad_bytes)
				memset(out + row_bytes, pad_byte, pad_bytes);
			out += dst_stride;
			src += src_stride;
		}
	}
	if (slice_rows > rows)
		memset(dst + rows * dst_stride, pad_byte, (slice_rows - rows) * dst_stride);
}

static enum submit_result submit_yuv(struct encoder *e, const struct encoder_frame *frame, int64_t ns)
{
	const bool i420 = e->input_mode == MC_INPUT_I420;
	if (!frame->data[0] || !frame->data[1] || (i420 && !frame->data[2]) ||
	    frame->linesize[0] < e->yuv_row_bytes || frame->linesize[1] < e->yuv_uv_row_bytes ||
	    (i420 && frame->linesize[2] < e->yuv_uv_row_bytes))
		return SUBMIT_FAILED;
	const ssize_t index = AMediaCodec_dequeueInputBuffer(e->codec, 1500);
	if (index == AMEDIACODEC_INFO_TRY_AGAIN_LATER)
		return SUBMIT_BUSY;
	if (index < 0)
		return SUBMIT_FAILED;
	size_t capacity = 0;
	uint8_t *dst = AMediaCodec_getInputBuffer(e->codec, (size_t)index, &capacity);
	if (!dst || capacity < e->yuv_size)
		return SUBMIT_FAILED;
	const size_t uv_slice_height = e->yuv_slice_height / 2;
	copy_plane_padded(dst, e->yuv_stride, frame->data[0], frame->linesize[0], e->yuv_row_bytes, e->yuv_height,
			  e->yuv_slice_height, 0x00);
	copy_plane_padded(dst + e->yuv_u_offset, e->yuv_uv_stride, frame->data[1], frame->linesize[1],
			  e->yuv_uv_row_bytes, e->yuv_uv_height, uv_slice_height, 0x80);
	if (i420)
		copy_plane_padded(dst + e->yuv_v_offset, e->yuv_uv_stride, frame->data[2], frame->linesize[2],
				  e->yuv_uv_row_bytes, e->yuv_uv_height, uv_slice_height, 0x80);
	return AMediaCodec_queueInputBuffer(e->codec, (size_t)index, 0, e->yuv_size, (uint64_t)ns / 1000, 0) == AMEDIA_OK
		       ? SUBMIT_OK : SUBMIT_FAILED;
}

static bool encode_frame(struct encoder *e, gs_texture_t *texture, const struct encoder_frame *frame, int64_t pts,
			 struct encoder_packet *packet, bool *received)
{
	*received = false;
	pthread_mutex_lock(&e->mutex);
	bool failed = e->failed || e->stopping;
	pthread_mutex_unlock(&e->mutex);
	if (failed) {
		publish_failure(e);
		return false;
	}
	int64_t ns;
	if ((!texture && !frame) || !avc_pts_to_ns(pts, e->fps_num, &ns)) {
		fail(e, "Invalid encoder input frame or timestamp");
		publish_failure(e);
		return false;
	}
	/* High-stability gate: if the codec is already behind, submitting one more
	 * frame would block inside eglSwapBuffers and push the pending backlog
	 * toward the hard limit that tears the stream down. Dropping this frame
	 * keeps the connection alive and lets the encoder catch up. The hard
	 * limit below stays as a last resort only. */
	const uint64_t gate_now = os_gettime_ns();
	pthread_mutex_lock(&e->mutex);
	/* While shedding we do not submit, so `degraded` can never decay through
	 * the normal "submit was fast" path; the retry window is the only exit. */
	mc_shed_tick(&e->degraded, &e->degraded_until_ns, gate_now);
	const uint64_t pending_now = e->flow.submitted > e->flow.output ? e->flow.submitted - e->flow.output : 0;
	/* See shed-policy.h for why the soft limit is an absolute depth and why
	 * the decision has a second, submit-cost arm: on the 25053RP5CC the
	 * backlog sat at 0-2 frames for a full minute while eglSwapBuffers was
	 * already 36-44ms, and then jumped straight to the hard limit. */
	const uint64_t soft_limit = mc_shed_soft_limit(e->degraded);
	const bool shed = mc_shed_decide(pending_now, e->degraded, soft_limit);
	if (shed)
		e->dropped_load++;
	const bool shed_changed = e->shedding != shed;
	e->shedding = shed;
	const uint64_t shed_dropped = e->dropped_load;
	const int shed_degraded = e->degraded;
	pthread_mutex_unlock(&e->mutex);
	/* One line per state edge, so "did we drop quality, and when" is
	 * answerable from the log without diffing two 5-second progress lines. */
	if (shed_changed)
		mc_log(shed ? LOG_WARNING : LOG_INFO,
			"[android-mediacodec] session=%llu event=%s codec=%s pending=%llu degraded=%d dropped_load=%llu",
			(unsigned long long)e->session_id, shed ? "shedding" : "recovered",
			e->format->codec, (unsigned long long)pending_now, shed_degraded,
			(unsigned long long)shed_dropped);
	if (shed) {
		/* Keep draining while shedding: the codec is still producing packets
		 * for frames submitted before the backlog appeared. Skipping
		 * take_packet() here would strand them in e->head until the output
		 * queue hit MAX_QUEUED (a different, and worse, abort), and would
		 * leave the watchdog below unreachable, turning a dead codec into a
		 * silent frozen stream instead of an error. */
		pthread_mutex_lock(&e->mutex);
		const uint64_t shed_now = os_gettime_ns();
		const bool shed_stalled = mc_flow_stalled(&e->flow, shed_now, NO_OUTPUT_NS);
		const bool shed_backlogged = mc_flow_backlogged(&e->flow, e->max_pending);
		pthread_mutex_unlock(&e->mutex);
		if (shed_stalled || shed_backlogged) {
			fail(e, shed_stalled
					? "MediaCodec produced no video for 5 seconds while input is active"
					: "MediaCodec pending input exceeded its frame limit (5 seconds, minimum 120 frames)");
			publish_failure(e);
			return false;
		}
		const bool shed_result = take_packet(e, packet, received, false);
		progress(e, false);
		return shed_result;
	}

	const uint64_t submit_start_ns = os_gettime_ns();
	enum submit_result submitted;
	if (frame)
		submitted = submit_yuv(e, frame, ns);
	else
		submitted = gs_android_encoder_surface_submit(e->surface, texture, ns) ? SUBMIT_OK : SUBMIT_FAILED;
	const uint64_t submit_us = (os_gettime_ns() - submit_start_ns) / 1000ULL;
	pthread_mutex_lock(&e->mutex);
	e->swap_us_sum += submit_us;
	if (submit_us > e->swap_max_us)
		e->swap_max_us = submit_us;
	e->swap_samples++;
	/* Adaptive shedding: once a single submit costs more than the budget the
	 * codec is behind and every further submit will block again, so remember
	 * the degraded state and let the gate collapse to the floor. */
	mc_shed_note_submit(submit_us, e->submit_budget_us, &e->degraded,
			&e->degraded_until_ns, os_gettime_ns());
	pthread_mutex_unlock(&e->mutex);
	if (submitted == SUBMIT_FAILED) {
		fail(e, frame ? (e->input_mode == MC_INPUT_P010 ? "Submitting P010 frame to MediaCodec failed"
								 : "Submitting YUV frame to MediaCodec failed")
			      : "Submitting texture to MediaCodec Surface failed");
		publish_failure(e);
		return false;
	}
	uint64_t now = os_gettime_ns();
	if (submitted == SUBMIT_BUSY) {
		if (!e->input_busy_since_ns)
			e->input_busy_since_ns = now;
		if (now - e->input_busy_since_ns > NO_OUTPUT_NS) {
			fail(e, e->input_mode == MC_INPUT_P010 ? "MediaCodec provides no P010 input buffer for 5 seconds"
							       : "MediaCodec provides no YUV input buffer for 5 seconds");
			publish_failure(e);
			return false;
		}
		pthread_mutex_lock(&e->mutex);
		e->dropped_load++;
		pthread_mutex_unlock(&e->mutex);
		bool result = take_packet(e, packet, received, false);
		progress(e, false);
		return result;
	}
	e->input_busy_since_ns = 0;
	pthread_mutex_lock(&e->mutex);
	if (!e->first_submit_ns) {
		e->first_submit_ns = now;
		e->last_report_ns = now;
	}
	mc_flow_input(&e->flow, now, e->input_gap_ns);
	bool stalled = mc_flow_stalled(&e->flow, now, NO_OUTPUT_NS);
	bool backlogged = mc_flow_backlogged(&e->flow, e->max_pending);
	pthread_mutex_unlock(&e->mutex);
	if (stalled || backlogged) {
		fail(e, stalled ? "MediaCodec produced no video for 5 seconds while input is active" :
			"MediaCodec pending input exceeded its frame limit (5 seconds, minimum 120 frames)");
		publish_failure(e);
		return false;
	}
	bool result = take_packet(e, packet, received, false);
	progress(e, false);
	return result;
}

static bool encode(void *data, struct encoder_texture *t, int64_t pts, uint64_t lock_key, uint64_t *next_key,
		   struct encoder_packet *packet, bool *received)
{
	UNUSED_PARAMETER(lock_key);
	UNUSED_PARAMETER(next_key);
	return encode_frame(data, t ? t->tex[0] : NULL, NULL, pts, packet, received);
}

static bool encode_raw(void *data, struct encoder_frame *frame, struct encoder_packet *packet, bool *received)
{
	return encode_frame(data, NULL, frame, frame ? frame->pts : 0, packet, received);
}

static bool flush(void *data, struct encoder_packet *packet, bool *received)
{
	struct encoder *e = data;
	pthread_mutex_lock(&e->mutex);
	bool signal = !e->eos_signaled;
	e->eos_signaled = true;
	e->stopping = true;
	pthread_mutex_unlock(&e->mutex);
	if (signal && !signal_end_of_input(e)) {
		fail(e, "MediaCodec EOS signal failed");
		publish_failure(e);
		return false;
	}
	return take_packet(e, packet, received, true);
}
static bool extra(void *data, uint8_t **bytes, size_t *size)
{
	struct encoder *e = data;
	pthread_mutex_lock(&e->mutex);
	*bytes = e->extra;
	*size = e->extra_size;
	bool ok = e->extra != NULL;
	pthread_mutex_unlock(&e->mutex);
	return ok;
}
static bool update(void *data, obs_data_t *s)
{
	struct encoder *e = data;
	int64_t kbps = obs_data_get_int(s, "bitrate");
	if (e->mode == 0) return true;
	if (e->bitrate_update_disabled) return false;
	if (kbps <= 0 || kbps > INT32_MAX / 1000)
		return false;
	int64_t rate = kbps * 1000;
	if (rate == e->bitrate)
		return true;
	AMediaFormat *f = AMediaFormat_new();
	if (!f)
		return false;
	AMediaFormat_setInt32(f, "video-bitrate", (int32_t)rate);
	media_status_t st = AMediaCodec_setParameters(e->codec, f);
	AMediaFormat_delete(f);
	if (st != AMEDIA_OK) {
		/* A component may encode correctly but reject runtime parameters.
		 * Preserve the last accepted rate; the output watchdog still detects
		 * a genuinely dead codec. Avoid retrying a rejected change every frame. */
		e->bitrate_update_disabled = true;
		mc_log(LOG_WARNING, "[android-mediacodec] event=bitrate-update-rejected status=%d retained_bitrate=%lld",
		       (int)st, (long long)e->bitrate);
		return false;
	}
	e->bitrate = rate;
	return true;
}
static void video_info(void *data, struct video_scale_info *info)
{
	const struct encoder *e = data;
	if (!e || e->input_mode == MC_INPUT_SURFACE) {
		info->format = VIDEO_FORMAT_RGBA;
		if (!mc_cs_is_sdr(to_mc_cs(info->colorspace)))
			info->colorspace = VIDEO_CS_709;
	} else if (e->input_mode == MC_INPUT_P010) {
		info->format = VIDEO_FORMAT_P010;
	} else {
		info->format = e->input_mode == MC_INPUT_I420 ? VIDEO_FORMAT_I420 : VIDEO_FORMAT_NV12;
		if (!mc_cs_is_sdr(to_mc_cs(info->colorspace)))
			info->colorspace = VIDEO_CS_709;
	}
	info->range = from_mc_range(to_mc_range(info->range));
}
static void p010_video_info(void *data, struct video_scale_info *info)
{
	UNUSED_PARAMETER(data);
	info->format = VIDEO_FORMAT_P010;
	info->range = from_mc_range(to_mc_range(info->range));
}
static struct obs_encoder_info encoder_info = {
	.type = OBS_ENCODER_VIDEO,
	.get_name = name,
	.create = create,
	.destroy = destroy,
	.get_defaults = defaults,
	.get_properties = properties,
	.update = update,
	.get_extra_data = extra,
	.get_video_info = video_info,
	.encode = encode_raw,
	.encode_raw_flush = flush,
	.encode_texture2 = encode,
	.encode_texture_flush = flush,
	.caps = OBS_ENCODER_CAP_PASS_TEXTURE | OBS_ENCODER_CAP_PASS_TEXTURE_RGB | OBS_ENCODER_CAP_DYN_BITRATE,
};
static bool encoder_available(const struct codec_spec *format, int profile)
{
	calldata_t cd;
	calldata_init(&cd);
	calldata_set_string(&cd, "mime", format->mime);
	calldata_set_int(&cd, "profile", profile);
	calldata_set_bool(&cd, "p010", format->p010);
	const bool available = proc_handler_call(obs_get_proc_handler(), "android_mediacodec_encoder_available", &cd) &&
			       calldata_bool(&cd, "supported");
	calldata_free(&cd);
	return available;
}

bool obs_module_load(void)
{
	for (size_t i = 0; i < sizeof(codecs) / sizeof(codecs[0]); ++i) {
		const struct codec_spec *format = &codecs[i];
		if (!encoder_available(format, format->profile) &&
		    !(format->p010 && encoder_available(format, obs_android_hevc_main10_profile(true))))
			continue;

		struct obs_encoder_info info = encoder_info;
		info.id = format->id;
		info.codec = format->codec;
		info.type_data = (void *)format;
		if (format->p010) {
			info.caps = OBS_ENCODER_CAP_DYN_BITRATE;
			info.encode_texture2 = NULL;
			info.encode_texture_flush = NULL;
			info.get_video_info = p010_video_info;
		}
		obs_register_encoder(&info);
	}
	return true;
}
