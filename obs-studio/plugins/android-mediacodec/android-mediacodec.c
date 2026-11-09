/*
 * Android MediaCodec Surface AVC encoder for OBS Studio.
 * Copyright (C) 2026 OBS for Android contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <obs-module.h>
#include <graphics/graphics.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <android/native_window.h>
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
#include "encoder-health.h"
#include "shed-policy.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("android-mediacodec", "en-US")
MODULE_EXPORT const char *obs_module_description(void)
{
	return "Android MediaCodec hardware H.264 and HEVC encoders";
}
#define CODEC_ID "android_mediacodec_h264"
#define CODEC_HEVC_ID "android_mediacodec_hevc"
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
	bool hevc;
	int mode;
	char *component;
	uint64_t session_id, first_submit_ns, last_report_ns;
	uint64_t delivered_frames, output_bytes, keyframes;
	struct mc_flow flow;
	uint64_t max_pending;
	uint64_t input_gap_ns;
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
	     (unsigned long long)e->session_id, e->hevc ? "hevc" : "h264",
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
	     (unsigned long long)e->session_id, e->hevc ? "hevc" : "h264", message);
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
			bool complete = e->hevc ? hevc_headers_ready(e->pending_config, e->pending_size) :
				(avc_has_nal(e->pending_config, e->pending_size, 7) && avc_has_nal(e->pending_config, e->pending_size, 8));
			if (complete) {
				bool supported = e->hevc ? hevc_sps_8bit_420(e->pending_config, e->pending_size) :
					avc_sps_8bit_420(e->pending_config, e->pending_size);
				if (!supported)
					error = "Codec SPS is not supported single-layer 8-bit 4:2:0";
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
static void format_config(struct encoder *e)
{
	AMediaFormat *f = AMediaCodec_getOutputFormat(e->codec);
	if (!f)
		return;
	mc_log(LOG_INFO, "[android-mediacodec] session=%llu event=format codec=%s format=%s",
	     (unsigned long long)e->session_id, e->hevc ? "hevc" : "h264", AMediaFormat_toString(f));
	void *a = NULL, *b = NULL;
	size_t an = 0, bn = 0;
	AMediaFormat_getBuffer(f, "csd-0", &a, &an);
	AMediaFormat_getBuffer(f, "csd-1", &b, &bn);
	if (an > 1048576 || bn > 1048576 - an) {
		AMediaFormat_delete(f);
		fail(e, "Codec configuration exceeds 1 MiB");
		return;
	}
	if (a && an) {
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
		unsigned type = e->hevc ? (data[start] >> 1) & 63 : data[start] & 31;
		bool parameter = e->hevc ? (type >= 32 && type <= 34) : (type == 7 || type == 8);
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
		else if (info.size && (info.flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG))
			save_config(e, buf + info.offset, (size_t)info.size);
		else if (info.size) {
			if (info.presentationTimeUs < 0 ||
			    (e->have_output_pts && info.presentationTimeUs < e->last_output_us)) {
				AMediaCodec_releaseOutputBuffer(e->codec, (size_t)index, false);
				fail(e, "Unexpected reordered MediaCodec output; B frames are unsupported");
				break;
			}
			e->have_output_pts = true;
			e->last_output_us = info.presentationTimeUs;
			// Copy each access unit once; validation and queue ownership share it.
			uint8_t *access = (size_t)info.size <= MAX_ACCESS_UNIT ? annexb(buf + info.offset, (size_t)info.size) : NULL;
			if (!access)
				fail(e, "Invalid or oversized MediaCodec access unit");
			pthread_mutex_lock(&e->mutex);
			bool have_extra = e->extra != NULL;
			pthread_mutex_unlock(&e->mutex);
			if (access && !have_extra) {
				inband_config(e, access, (size_t)info.size);
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
							 (unsigned long long)e->session_id, e->hevc ? "hevc" : "h264", p->key, p->size);
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
static const char *name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("Encoder");
}
#ifdef ENABLE_HEVC
static const char *name_hevc(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("EncoderHEVC");
}
#endif
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
		if (signal && AMediaCodec_signalEndOfInputStream(e->codec) != AMEDIA_OK)
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
static bool start_candidate(struct encoder *e, const char *component, int w, int h, int query_num, int keyint, int control_value)
{
	e->codec = AMediaCodec_createCodecByName(component);
	if (!e->codec)
		return false;
	AMediaFormat *f = AMediaFormat_new();
	if (!f)
		goto error;
	AMediaFormat_setString(f, AMEDIAFORMAT_KEY_MIME, e->hevc ? "video/hevc" : "video/avc");
	if (e->hevc)
		AMediaFormat_setInt32(f, "profile", 1); /* HEVCProfileMain, 8-bit */
	AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_WIDTH, w);
	AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_HEIGHT, h);
	AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_COLOR_FORMAT, 0x7f000789);
	if (e->mode != 0)
		AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_BIT_RATE, (int32_t)e->bitrate);
	AMediaFormat_setFloat(f, AMEDIAFORMAT_KEY_FRAME_RATE, (float)query_num / e->fps_den);
	AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_I_FRAME_INTERVAL, keyint);
	AMediaFormat_setInt32(f, "bitrate-mode", e->mode);
	if (control_value >= 0)
		AMediaFormat_setInt32(f, e->mode == 0 ? "quality" : "complexity", control_value);
	AMediaFormat_setInt32(f, "max-bframes", 0);
	/* Surface RGB conversion and resulting bitstream metadata require device validation. */
	AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_COLOR_STANDARD, 1); /* BT.709 */
	AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_COLOR_TRANSFER, 3); /* SDR video */
	AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_COLOR_RANGE, 2); /* Limited */
	/* SDR BT.709, limited-range encoded YUV. Surface input is full-range RGB. */
	AMediaFormat_setInt32(f, "color-standard", 1);
	AMediaFormat_setInt32(f, "color-transfer", 3);
	AMediaFormat_setInt32(f, "color-range", 2);
	media_status_t status = AMediaCodec_configure(e->codec, f, NULL, NULL, AMEDIACODEC_CONFIGURE_FLAG_ENCODE);
	AMediaFormat_delete(f);
	if (status != AMEDIA_OK)
		goto error;
	if (AMediaCodec_createInputSurface(e->codec, &e->window) != AMEDIA_OK || !e->window)
		goto error;
	obs_enter_graphics();
	e->surface = gs_android_encoder_surface_create(e->window);
	obs_leave_graphics();
	if (!e->surface)
		goto error;
	if (AMediaCodec_start(e->codec) != AMEDIA_OK)
		goto error;
	e->started = true;
	e->component = bstrdup(component);
	mc_log(LOG_INFO, "[android-mediacodec] session=%llu event=started codec=%s component=%s hardware=true software=false input=Surface width=%d height=%d fps_num=%d fps_den=%d bitrate=%lld",
	     (unsigned long long)e->session_id, e->hevc ? "hevc" : "h264", component, w, h, query_num,
	     e->fps_den, (long long)e->bitrate);
	return true;
error:
	mc_log(LOG_WARNING, "[android-mediacodec] Codec %s failed initialization", component);
	release_candidate(e);
	return false;
}
static void *create(obs_data_t *s, obs_encoder_t *obs)
{
	struct encoder *e = bzalloc(sizeof(*e));
	e->obs = obs;
	e->hevc = strcmp(obs_encoder_get_id(obs), CODEC_HEVC_ID) == 0;
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
	if (actual && actual->colorspace != VIDEO_CS_709 && actual->colorspace != VIDEO_CS_DEFAULT) {
		fail(e, "MediaCodec currently requires SDR BT.709 output; HDR and other color spaces are unsupported");
		goto error;
	}
	if (!base || !actual || !base->fps_num || base->fps_num > INT32_MAX || !actual->fps_num ||
	    actual->fps_num > INT32_MAX || !actual->fps_den || actual->fps_den > INT32_MAX) {
		fail(e, "Cannot determine video frame rate");
		goto error;
	}
	struct obs_video_info global_video;
	if (!obs_get_video_info(&global_video) ||
	    (global_video.colorspace != VIDEO_CS_709 && global_video.colorspace != VIDEO_CS_DEFAULT) ||
	    (actual->colorspace != VIDEO_CS_709 && actual->colorspace != VIDEO_CS_DEFAULT) ||
	    actual->range != VIDEO_RANGE_PARTIAL) {
		fail(e, "MediaCodec video requires SDR BT.709 with limited video range");
		goto error;
	}
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
	for (int index = 0; index < 128; index++) {
		calldata_t cd;
		calldata_init(&cd);
		calldata_set_int(&cd, "width", w);
		calldata_set_int(&cd, "height", h);
		calldata_set_int(&cd, "fps_num", query_num);
		calldata_set_int(&cd, "fps_den", e->fps_den);
		calldata_set_int(&cd, "index", index);
		calldata_set_string(&cd, "mime", e->hevc ? "video/hevc" : "video/avc");
		calldata_set_int(&cd, "bitrate", e->mode == 0 ? 0 : e->bitrate);
		calldata_set_int(&cd, "mode", e->mode);
		char *component = NULL;
		if (proc_handler_call(obs_get_proc_handler(), "android_mediacodec_query_component", &cd)) {
			const char *result = calldata_string(&cd, "name");
			if (result && *result)
				component = bstrdup(result);
		}
		calldata_free(&cd);
		if (!component)
			break;
		mc_log(LOG_INFO, "[android-mediacodec] trying %s for %dx%d at %d/%d fps", component, w, h, query_num,
		     e->fps_den);
		int control_value = -1;
		calldata_t controls;
		calldata_init(&controls);
		calldata_set_string(&controls, "name", component);
		calldata_set_string(&controls, "mime", e->hevc ? "video/hevc" : "video/avc");
		calldata_set_int(&controls, "mode", e->mode);
		bool supported = proc_handler_call(obs_get_proc_handler(), "android_mediacodec_query_controls", &controls) &&
			calldata_bool(&controls, "supported");
		int64_t low = calldata_int(&controls, e->mode == 0 ? "quality_min" : "complexity_min");
		int64_t high = calldata_int(&controls, e->mode == 0 ? "quality_max" : "complexity_max");
		if ((quality || e->mode == 0) && (low < 0 || high < low || high > INT32_MAX)) supported = false;
		if (quality && high == low) supported = false;
		if (supported && quality) control_value = (int)(low + ((high - low) * (quality - 1) + 1) / 2);
		else if (supported && e->mode == 0) control_value = (int)(low + (high - low) / 2);
		calldata_free(&controls);
		mc_log(LOG_INFO, "[android-mediacodec] component=%s mode=%d control=%d range=%lld..%lld supported=%d",
		       component, e->mode, control_value, (long long)low, (long long)high, supported);
		bool started = supported && start_candidate(e, component, w, h, query_num, keyint, control_value);
		bfree(component);
		if (started)
			break;
	}
	if (!e->started) {
		fail(e, "No compatible hardware encoder supports the selected rate control and adjustable quality range, or encoder initialization failed");
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
static bool encode(void *data, struct encoder_texture *t, int64_t pts, uint64_t lock_key, uint64_t *next_key,
		   struct encoder_packet *packet, bool *received)
{
	UNUSED_PARAMETER(lock_key);
	UNUSED_PARAMETER(next_key);
	struct encoder *e = data;
	*received = false;
	pthread_mutex_lock(&e->mutex);
	bool failed = e->failed || e->stopping;
	pthread_mutex_unlock(&e->mutex);
	if (failed) {
		publish_failure(e);
		return false;
	}
	int64_t ns;
	if (!t || !t->tex[0] || !avc_pts_to_ns(pts, e->fps_num, &ns)) {
		fail(e, "Invalid encoder texture or timestamp");
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
			e->hevc ? "hevc" : "h264", (unsigned long long)pending_now, shed_degraded,
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
	bool ok = gs_android_encoder_surface_submit(e->surface, t->tex[0], ns);
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
	if (!ok) {
		fail(e, "Submitting texture to MediaCodec Surface failed");
		publish_failure(e);
		return false;
	}
	uint64_t now = os_gettime_ns();
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
static bool flush(void *data, struct encoder_packet *packet, bool *received)
{
	struct encoder *e = data;
	pthread_mutex_lock(&e->mutex);
	bool signal = !e->eos_signaled;
	e->eos_signaled = true;
	e->stopping = true;
	pthread_mutex_unlock(&e->mutex);
	if (signal && AMediaCodec_signalEndOfInputStream(e->codec) != AMEDIA_OK) {
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
	UNUSED_PARAMETER(data);
	info->format = VIDEO_FORMAT_RGBA;
	info->colorspace = VIDEO_CS_709;
	info->range = VIDEO_RANGE_PARTIAL;
}
static struct obs_encoder_info encoder_info = {
	.id = CODEC_ID,
	.type = OBS_ENCODER_VIDEO,
	.codec = "h264",
	.get_name = name,
	.create = create,
	.destroy = destroy,
	.get_defaults = defaults,
	.get_properties = properties,
	.update = update,
	.get_extra_data = extra,
	.get_video_info = video_info,
	.encode_texture2 = encode,
	.encode_texture_flush = flush,
	.caps = OBS_ENCODER_CAP_PASS_TEXTURE | OBS_ENCODER_CAP_PASS_TEXTURE_RGB | OBS_ENCODER_CAP_DYN_BITRATE,
};
bool obs_module_load(void)
{
	obs_register_encoder(&encoder_info);
#ifdef ENABLE_HEVC
	struct obs_encoder_info hevc_info = encoder_info;
	hevc_info.id = CODEC_HEVC_ID;
	hevc_info.codec = "hevc";
	hevc_info.get_name = name_hevc;
	obs_register_encoder(&hevc_info);
#endif
	return true;
}
