/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * Android NDI® Program Output (android_ndi_output).
 *
 * Publishes OBS Program video + audio over NDI® using the dynamically loaded
 * official libndi.so runtime with:
 *   - TWO independent sender worker threads (video_thread + audio_thread) and
 *     bounded queues so slow receivers never stall OBS callbacks or audio.
 *   - Heap-owned ndi_send_session retired asynchronously by a detached reaper
 *     thread on stop(), so ndi_output_stop() returns in < 5 ms even if a slow
 *     receiver is currently stalling NDIlib_send_send_video_async_v2()!
 *   - Android WifiManager.MulticastLock held per active send session.
 */

#include "ndi-output.h"
#include "ndi-abi.h"
#include "ndi-runtime.h"

#include <obs-module.h>
#include <util/platform.h>
#include <util/threading.h>

#if defined(__ANDROID__)
#include <android/log.h>
#include <obs-android.h>
#endif

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NDI_OUT_AUDIO_RING_CAP 16

struct ndi_queued_video {
	uint8_t *data;
	size_t cap;
	uint32_t width;
	uint32_t height;
	uint32_t stride;
	int64_t timecode;
	bool valid;
};

struct ndi_queued_audio {
	uint8_t *data;
	size_t cap;
	uint32_t sample_rate;
	uint32_t channels;
	uint32_t samples;
	uint32_t channel_stride;
	int64_t timecode;
};

struct ndi_send_session {
	volatile long refcount;
	pthread_mutex_t video_mtx;
	pthread_cond_t video_cond;
	pthread_mutex_t audio_mtx;
	pthread_cond_t audio_cond;

	char ndi_name[256];

	const struct ndi_api *api;
	NDIlib_send_instance_t send;
	pthread_t video_thread;
	pthread_t audio_thread;
	bool video_running;
	bool audio_running;
	bool stopping;
	bool multicast_held;

	uint32_t width;
	uint32_t height;
	uint32_t fps_num;
	uint32_t fps_den;
	uint32_t audio_sample_rate;
	uint32_t audio_channels;

	struct ndi_queued_video pending_video;
	uint8_t *send_video_buf[2];
	size_t send_video_cap[2];
	int send_video_idx;

	struct ndi_queued_audio audio_ring[NDI_OUT_AUDIO_RING_CAP];
	uint32_t audio_head;
	uint32_t audio_count;
	uint8_t *send_audio_buf;
	size_t send_audio_cap;

	uint64_t total_video_frames;
	uint64_t total_audio_frames;
	uint64_t dropped_video_frames;
	uint64_t dropped_audio_frames;
	uint64_t total_video_bytes;
	uint64_t total_audio_bytes;
	uint64_t last_log_ns;
};

struct ndi_output {
	obs_output_t *output;
	pthread_mutex_t mtx;

	char ndi_name[256];
	char ndi_groups[256];

	struct ndi_send_session *session;
	uint64_t last_total_bytes;
};

static void ndi_session_destroy_sync(struct ndi_send_session *sess)
{
	if (!sess)
		return;

	pthread_mutex_lock(&sess->video_mtx);
	pthread_mutex_lock(&sess->audio_mtx);
	sess->stopping = true;
	const bool join_video = sess->video_running;
	const bool join_audio = sess->audio_running;
	pthread_cond_broadcast(&sess->audio_cond);
	pthread_mutex_unlock(&sess->audio_mtx);
	pthread_cond_broadcast(&sess->video_cond);
	pthread_mutex_unlock(&sess->video_mtx);

	if (join_audio) {
		pthread_join(sess->audio_thread, NULL);
		sess->audio_running = false;
	}
	if (join_video) {
		pthread_join(sess->video_thread, NULL);
		sess->video_running = false;
	}

	if (sess->send && sess->api && sess->api->send_destroy) {
		sess->api->send_destroy(sess->send);
		sess->send = NULL;
	}
	if (sess->api) {
		ndi_runtime_release();
		sess->api = NULL;
	}
#if defined(__ANDROID__)
	if (sess->multicast_held) {
		obs_android_ndi_release_discovery();
		sess->multicast_held = false;
	}
#endif

	free(sess->pending_video.data);
	for (int i = 0; i < 2; i++)
		free(sess->send_video_buf[i]);
	for (int i = 0; i < NDI_OUT_AUDIO_RING_CAP; i++)
		free(sess->audio_ring[i].data);
	free(sess->send_audio_buf);

	pthread_cond_destroy(&sess->audio_cond);
	pthread_mutex_destroy(&sess->audio_mtx);
	pthread_cond_destroy(&sess->video_cond);
	pthread_mutex_destroy(&sess->video_mtx);
	free(sess);
}

static void *ndi_session_reaper_thread(void *arg)
{
	struct ndi_send_session *sess = arg;
	const uint64_t t0 = os_gettime_ns();
	ndi_session_destroy_sync(sess);
	const uint64_t drain_ms = (os_gettime_ns() - t0) / 1000000ULL;
	blog(LOG_INFO, "android-ndi-output: async session retired in %llu ms", (unsigned long long)drain_ms);
	return NULL;
}

static void ndi_session_signal_stop(struct ndi_send_session *sess)
{
	if (!sess)
		return;
	pthread_mutex_lock(&sess->video_mtx);
	pthread_mutex_lock(&sess->audio_mtx);
	sess->stopping = true;
	pthread_cond_broadcast(&sess->audio_cond);
	pthread_mutex_unlock(&sess->audio_mtx);
	pthread_cond_broadcast(&sess->video_cond);
	pthread_mutex_unlock(&sess->video_mtx);
}

static void ndi_session_retire_async(struct ndi_send_session *sess)
{
	if (!sess)
		return;

	ndi_session_signal_stop(sess);

	pthread_t reaper;
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	const int rc = pthread_create(&reaper, &attr, ndi_session_reaper_thread, sess);
	pthread_attr_destroy(&attr);
	if (rc != 0) {
		/* Fallback only if OS cannot spawn a thread. */
		ndi_session_destroy_sync(sess);
	}
}

static struct ndi_send_session *ndi_session_acquire(struct ndi_output *o)
{
	pthread_mutex_lock(&o->mtx);
	struct ndi_send_session *sess = o->session;
	if (sess)
		os_atomic_inc_long(&sess->refcount);
	pthread_mutex_unlock(&o->mtx);
	return sess;
}

static void ndi_session_release(struct ndi_send_session *sess)
{
	if (!sess)
		return;
	if (os_atomic_dec_long(&sess->refcount) == 0)
		ndi_session_retire_async(sess);
}

static void *ndi_output_video_worker(void *arg)
{
	struct ndi_send_session *sess = arg;

	for (;;) {
		uint32_t v_w = 0, v_h = 0, v_stride = 0;
		int64_t v_tc = 0;
		uint8_t *v_ptr = NULL;
		size_t v_bytes = 0;

		pthread_mutex_lock(&sess->video_mtx);
		while (!sess->stopping && !sess->pending_video.valid)
			pthread_cond_wait(&sess->video_cond, &sess->video_mtx);

		if (sess->stopping) {
			sess->pending_video.valid = false;
			pthread_mutex_unlock(&sess->video_mtx);
			break;
		}

		const int idx = sess->send_video_idx;
		uint8_t *tmp_ptr = sess->send_video_buf[idx];
		size_t tmp_cap = sess->send_video_cap[idx];

		sess->send_video_buf[idx] = sess->pending_video.data;
		sess->send_video_cap[idx] = sess->pending_video.cap;
		sess->pending_video.data = tmp_ptr;
		sess->pending_video.cap = tmp_cap;

		v_w = sess->pending_video.width;
		v_h = sess->pending_video.height;
		v_stride = sess->pending_video.stride;
		v_tc = sess->pending_video.timecode;
		v_ptr = sess->send_video_buf[idx];
		v_bytes = (size_t)v_stride * (size_t)v_h;
		sess->pending_video.valid = false;
		sess->send_video_idx ^= 1;
		pthread_mutex_unlock(&sess->video_mtx);

		if (v_ptr && v_bytes > 0 && sess->send && sess->api && sess->api->send_send_video_async_v2) {
			NDIlib_video_frame_v2_t vf;
			memset(&vf, 0, sizeof(vf));
			vf.xres = (int)v_w;
			vf.yres = (int)v_h;
			vf.FourCC = NDIlib_FourCC_video_type_UYVY;
			vf.frame_rate_N = (int)sess->fps_num;
			vf.frame_rate_D = (int)sess->fps_den;
			vf.picture_aspect_ratio = v_h > 0 ? ((float)v_w / (float)v_h) : (16.0f / 9.0f);
			vf.frame_format_type = NDIlib_frame_format_type_progressive;
			vf.timecode = v_tc;
			vf.p_data = v_ptr;
			vf.line_stride_in_bytes = (int)v_stride;
			sess->api->send_send_video_async_v2(sess->send, &vf);

			const uint64_t now_ns = os_gettime_ns();
			pthread_mutex_lock(&sess->video_mtx);
			sess->total_video_frames++;
			sess->total_video_bytes += v_bytes;
			const bool should_log = !sess->stopping && (now_ns - sess->last_log_ns >= 5000000000ULL);
			uint64_t vf_cnt = sess->total_video_frames;
			uint64_t dv_cnt = sess->dropped_video_frames;
			if (should_log)
				sess->last_log_ns = now_ns;
			pthread_mutex_unlock(&sess->video_mtx);

			if (should_log) {
				pthread_mutex_lock(&sess->audio_mtx);
				uint64_t af_cnt = sess->total_audio_frames;
				uint64_t da_cnt = sess->dropped_audio_frames;
				pthread_mutex_unlock(&sess->audio_mtx);
				int conns = sess->api->send_get_no_connections
						    ? sess->api->send_get_no_connections(sess->send, 0)
						    : -1;
				blog(LOG_INFO,
				     "android-ndi-output: '%s' video=%llu audio=%llu drop_v=%llu drop_a=%llu conns=%d",
				     sess->ndi_name, (unsigned long long)vf_cnt, (unsigned long long)af_cnt,
				     (unsigned long long)dv_cnt, (unsigned long long)da_cnt, conns);
			}
		}
	}

	/* Synchronize any in-flight async video frame before returning. */
	if (sess->send && sess->api && sess->api->send_send_video_async_v2)
		sess->api->send_send_video_async_v2(sess->send, NULL);
	return NULL;
}

static void *ndi_output_audio_worker(void *arg)
{
	struct ndi_send_session *sess = arg;

	for (;;) {
		struct ndi_queued_audio a_item;
		memset(&a_item, 0, sizeof(a_item));
		bool have_audio = false;

		pthread_mutex_lock(&sess->audio_mtx);
		while (!sess->stopping && sess->audio_count == 0)
			pthread_cond_wait(&sess->audio_cond, &sess->audio_mtx);

		if (sess->stopping) {
			sess->audio_count = 0;
			pthread_mutex_unlock(&sess->audio_mtx);
			break;
		}

		struct ndi_queued_audio *slot = &sess->audio_ring[sess->audio_head];
		const size_t bytes = (size_t)slot->channel_stride * (size_t)slot->channels;
		if (sess->send_audio_cap < bytes) {
			uint8_t *nb = realloc(sess->send_audio_buf, bytes);
			if (nb) {
				sess->send_audio_buf = nb;
				sess->send_audio_cap = bytes;
			}
		}
		if (sess->send_audio_buf && sess->send_audio_cap >= bytes && slot->data) {
			memcpy(sess->send_audio_buf, slot->data, bytes);
			a_item = *slot;
			a_item.data = sess->send_audio_buf;
			have_audio = true;
		}
		sess->audio_head = (sess->audio_head + 1U) % NDI_OUT_AUDIO_RING_CAP;
		sess->audio_count--;
		pthread_mutex_unlock(&sess->audio_mtx);

		if (have_audio && sess->send && sess->api && sess->api->send_send_audio_v3) {
			NDIlib_audio_frame_v3_t af;
			memset(&af, 0, sizeof(af));
			af.sample_rate = (int)a_item.sample_rate;
			af.no_channels = (int)a_item.channels;
			af.no_samples = (int)a_item.samples;
			af.timecode = a_item.timecode;
			af.FourCC = NDIlib_FourCC_audio_type_FLTP;
			af.p_data = a_item.data;
			af.channel_stride_in_bytes = (int)a_item.channel_stride;
			sess->api->send_send_audio_v3(sess->send, &af);

			pthread_mutex_lock(&sess->audio_mtx);
			sess->total_audio_frames++;
			sess->total_audio_bytes += (size_t)a_item.channel_stride * (size_t)a_item.channels;
			pthread_mutex_unlock(&sess->audio_mtx);
		}
	}
	return NULL;
}

static const char *ndi_output_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("Ndi.Output.Name");
}

static void ndi_output_update(void *data, obs_data_t *settings)
{
	struct ndi_output *o = data;
	const char *name = obs_data_get_string(settings, "ndi_name");
	const char *groups = obs_data_get_string(settings, "ndi_groups");

	pthread_mutex_lock(&o->mtx);
	snprintf(o->ndi_name, sizeof(o->ndi_name), "%s", (name && name[0]) ? name : "OBS for Android");
	snprintf(o->ndi_groups, sizeof(o->ndi_groups), "%s", groups ? groups : "");
	pthread_mutex_unlock(&o->mtx);
}

static void *ndi_output_create(obs_data_t *settings, obs_output_t *output)
{
	struct ndi_output *o = bzalloc(sizeof(*o));
	o->output = output;
	pthread_mutex_init(&o->mtx, NULL);
	ndi_output_update(o, settings);
	return o;
}

static void ndi_output_destroy(void *data)
{
	struct ndi_output *o = data;
	if (!o)
		return;
	pthread_mutex_lock(&o->mtx);
	struct ndi_send_session *sess = o->session;
	o->session = NULL;
	pthread_mutex_unlock(&o->mtx);

	if (sess) {
		ndi_session_signal_stop(sess);
		ndi_session_release(sess);
	}

	pthread_mutex_destroy(&o->mtx);
	bfree(o);
}

static bool ndi_output_start(void *data)
{
	struct ndi_output *o = data;
	pthread_mutex_lock(&o->mtx);
	if (o->session != NULL) {
		pthread_mutex_unlock(&o->mtx);
		return true;
	}
	char name_copy[256];
	char groups_copy[256];
	snprintf(name_copy, sizeof(name_copy), "%s", o->ndi_name[0] ? o->ndi_name : "OBS for Android");
	snprintf(groups_copy, sizeof(groups_copy), "%s", o->ndi_groups);
	pthread_mutex_unlock(&o->mtx);

	const struct ndi_api *api = ndi_runtime_acquire();
	if (!api || !api->send_create || !api->send_destroy || !api->send_send_video_async_v2 ||
	    !api->send_send_audio_v3) {
		if (api)
			ndi_runtime_release();
		blog(LOG_WARNING, "android-ndi-output: libndi.so unavailable or missing send symbols");
		return false;
	}

	video_t *video = obs_output_video(o->output);
	audio_t *audio = obs_output_audio(o->output);
	if (!video) {
		ndi_runtime_release();
		blog(LOG_WARNING, "android-ndi-output: obs_output_video() returned NULL");
		return false;
	}

	struct ndi_send_session *sess = calloc(1, sizeof(*sess));
	if (!sess) {
		ndi_runtime_release();
		return false;
	}
	os_atomic_set_long(&sess->refcount, 1);
	pthread_mutex_init(&sess->video_mtx, NULL);
	pthread_cond_init(&sess->video_cond, NULL);
	pthread_mutex_init(&sess->audio_mtx, NULL);
	pthread_cond_init(&sess->audio_cond, NULL);
	snprintf(sess->ndi_name, sizeof(sess->ndi_name), "%s", name_copy);
	sess->api = api;

	const struct video_output_info *voi = video_output_get_info(video);
	sess->width = obs_output_get_width(o->output);
	sess->height = obs_output_get_height(o->output);
	sess->fps_num = voi && voi->fps_num ? voi->fps_num : 30;
	sess->fps_den = voi && voi->fps_den ? voi->fps_den : 1;

	struct video_scale_info vsi;
	memset(&vsi, 0, sizeof(vsi));
	vsi.format = VIDEO_FORMAT_UYVY;
	vsi.width = sess->width;
	vsi.height = sess->height;
	vsi.colorspace = voi ? voi->colorspace : VIDEO_CS_709;
	vsi.range = voi ? voi->range : VIDEO_RANGE_PARTIAL;
	obs_output_set_video_conversion(o->output, &vsi);

	if (audio) {
		const struct audio_output_info *aoi = audio_output_get_info(audio);
		sess->audio_sample_rate = aoi && aoi->samples_per_sec ? aoi->samples_per_sec : 48000;
		sess->audio_channels = (uint32_t)audio_output_get_channels(audio);
		if (sess->audio_channels == 0)
			sess->audio_channels = 2;
		struct audio_convert_info aci;
		memset(&aci, 0, sizeof(aci));
		aci.format = AUDIO_FORMAT_FLOAT_PLANAR;
		aci.samples_per_sec = sess->audio_sample_rate;
		aci.speakers = aoi ? aoi->speakers : SPEAKERS_STEREO;
		obs_output_set_audio_conversion(o->output, &aci);
	}

#if defined(__ANDROID__)
	if (obs_android_ndi_ready()) {
		obs_android_ndi_acquire_discovery();
		sess->multicast_held = true;
	}
#endif

	NDIlib_send_create_t cfg;
	memset(&cfg, 0, sizeof(cfg));
	cfg.p_ndi_name = name_copy;
	cfg.p_groups = groups_copy[0] ? groups_copy : NULL;
	cfg.clock_video = false;
	cfg.clock_audio = false;

	sess->send = api->send_create(&cfg);
	if (!sess->send) {
		blog(LOG_WARNING, "android-ndi-output: NDIlib_send_create('%s') failed", cfg.p_ndi_name);
		ndi_session_destroy_sync(sess);
		return false;
	}
	sess->last_log_ns = os_gettime_ns();

	if (pthread_create(&sess->video_thread, NULL, ndi_output_video_worker, sess) != 0) {
		blog(LOG_WARNING, "android-ndi-output: failed to start video worker thread");
		ndi_session_destroy_sync(sess);
		return false;
	}
	sess->video_running = true;

	if (pthread_create(&sess->audio_thread, NULL, ndi_output_audio_worker, sess) != 0) {
		blog(LOG_WARNING, "android-ndi-output: failed to start audio worker thread");
		ndi_session_destroy_sync(sess);
		return false;
	}
	sess->audio_running = true;

	pthread_mutex_lock(&o->mtx);
	o->session = sess;
	o->last_total_bytes = 0;
	pthread_mutex_unlock(&o->mtx);

	if (!obs_output_begin_data_capture(o->output, 0)) {
		blog(LOG_WARNING, "android-ndi-output: obs_output_begin_data_capture failed");
		pthread_mutex_lock(&o->mtx);
		o->session = NULL;
		pthread_mutex_unlock(&o->mtx);
		ndi_session_destroy_sync(sess);
		return false;
	}

	blog(LOG_INFO, "android-ndi-output: started '%s' (%ux%u @ %u/%u fps, %u Hz %u ch, split A/V workers)",
	     name_copy, sess->width, sess->height, sess->fps_num, sess->fps_den, sess->audio_sample_rate,
	     sess->audio_channels);
#if defined(__ANDROID__)
	__android_log_print(ANDROID_LOG_INFO, "OBS-ndi", "NDI Program Output started: '%s' %ux%u@%u/%u", name_copy,
			    sess->width, sess->height, sess->fps_num, sess->fps_den);
#endif
	return true;
}

static void ndi_output_stop(void *data, uint64_t ts)
{
	UNUSED_PARAMETER(ts);
	struct ndi_output *o = data;
	const uint64_t t0 = os_gettime_ns();
	obs_output_end_data_capture(o->output);

	pthread_mutex_lock(&o->mtx);
	struct ndi_send_session *sess = o->session;
	o->session = NULL;
	pthread_mutex_unlock(&o->mtx);

	if (!sess)
		return;

	uint64_t vf = 0, af = 0, dv = 0, da = 0;
	{
		pthread_mutex_lock(&sess->video_mtx);
		vf = sess->total_video_frames;
		dv = sess->dropped_video_frames;
		uint64_t vb = sess->total_video_bytes;
		pthread_mutex_unlock(&sess->video_mtx);

		pthread_mutex_lock(&sess->audio_mtx);
		af = sess->total_audio_frames;
		da = sess->dropped_audio_frames;
		uint64_t ab = sess->total_audio_bytes;
		pthread_mutex_unlock(&sess->audio_mtx);

		pthread_mutex_lock(&o->mtx);
		o->last_total_bytes = vb + ab;
		pthread_mutex_unlock(&o->mtx);

		/* Signal workers to exit immediately, then drop owner ref; if an OBS
		 * raw_video/raw_audio callback is still in-flight (obs_output_end_data_capture
		 * disconnects callbacks asynchronously), the last callback to release its ref
		 * triggers the detached async reaper so sess is never freed while in use. */
		ndi_session_signal_stop(sess);
		ndi_session_release(sess);
	}

	const uint64_t stop_ms = (os_gettime_ns() - t0) / 1000000ULL;
	blog(LOG_INFO, "android-ndi-output: stopped in %llu ms (video=%llu audio=%llu drop_v=%llu drop_a=%llu)",
	     (unsigned long long)stop_ms, (unsigned long long)vf, (unsigned long long)af, (unsigned long long)dv,
	     (unsigned long long)da);
#if defined(__ANDROID__)
	__android_log_print(ANDROID_LOG_INFO, "OBS-ndi",
			    "NDI Program Output stopped in %llu ms (video=%llu audio=%llu drop_v=%llu drop_a=%llu)",
			    (unsigned long long)stop_ms, (unsigned long long)vf, (unsigned long long)af,
			    (unsigned long long)dv, (unsigned long long)da);
#endif
}

static void ndi_output_raw_video(void *data, struct video_data *frame)
{
	struct ndi_output *o = data;
	if (!frame || !frame->data[0])
		return;

	struct ndi_send_session *sess = ndi_session_acquire(o);
	if (!sess)
		return;

	const uint32_t w = sess->width;
	const uint32_t h = sess->height;
	const uint32_t src_stride = frame->linesize[0];
	const uint32_t dst_stride = w * 2U; /* UYVY: 2 bytes per pixel */
	const size_t need = (size_t)dst_stride * (size_t)h;
	if (w == 0 || h == 0 || src_stride < dst_stride) {
		ndi_session_release(sess);
		return;
	}

	pthread_mutex_lock(&sess->video_mtx);
	if (sess->stopping) {
		pthread_mutex_unlock(&sess->video_mtx);
		ndi_session_release(sess);
		return;
	}
	if (sess->pending_video.cap < need) {
		uint8_t *nb = realloc(sess->pending_video.data, need);
		if (!nb) {
			pthread_mutex_unlock(&sess->video_mtx);
			ndi_session_release(sess);
			return;
		}
		sess->pending_video.data = nb;
		sess->pending_video.cap = need;
	}
	if (sess->pending_video.valid)
		sess->dropped_video_frames++;

	uint8_t *dst = sess->pending_video.data;
	if (src_stride == dst_stride) {
		memcpy(dst, frame->data[0], need);
	} else {
		for (uint32_t y = 0; y < h; y++)
			memcpy(dst + (size_t)y * dst_stride, frame->data[0] + (size_t)y * src_stride, dst_stride);
	}
	sess->pending_video.width = w;
	sess->pending_video.height = h;
	sess->pending_video.stride = dst_stride;
	sess->pending_video.timecode = (int64_t)(frame->timestamp / 100ULL);
	sess->pending_video.valid = true;
	pthread_cond_signal(&sess->video_cond);
	pthread_mutex_unlock(&sess->video_mtx);
	ndi_session_release(sess);
}

static void ndi_output_raw_audio(void *data, struct audio_data *frame)
{
	struct ndi_output *o = data;
	if (!frame || frame->frames == 0 || !frame->data[0])
		return;

	struct ndi_send_session *sess = ndi_session_acquire(o);
	if (!sess)
		return;

	const uint32_t ch = sess->audio_channels > 0 ? sess->audio_channels : 2;
	const uint32_t samples = frame->frames;
	const size_t ch_bytes = (size_t)samples * sizeof(float);
	const size_t total = ch_bytes * (size_t)ch;

	pthread_mutex_lock(&sess->audio_mtx);
	if (sess->stopping) {
		pthread_mutex_unlock(&sess->audio_mtx);
		ndi_session_release(sess);
		return;
	}
	if (sess->audio_count >= NDI_OUT_AUDIO_RING_CAP) {
		sess->audio_head = (sess->audio_head + 1U) % NDI_OUT_AUDIO_RING_CAP;
		sess->audio_count--;
		sess->dropped_audio_frames++;
	}
	const uint32_t tail = (sess->audio_head + sess->audio_count) % NDI_OUT_AUDIO_RING_CAP;
	struct ndi_queued_audio *slot = &sess->audio_ring[tail];
	if (slot->cap < total) {
		uint8_t *nb = realloc(slot->data, total);
		if (!nb) {
			pthread_mutex_unlock(&sess->audio_mtx);
			ndi_session_release(sess);
			return;
		}
		slot->data = nb;
		slot->cap = total;
	}
	for (uint32_t c = 0; c < ch; c++) {
		if (frame->data[c])
			memcpy(slot->data + c * ch_bytes, frame->data[c], ch_bytes);
		else
			memset(slot->data + c * ch_bytes, 0, ch_bytes);
	}
	slot->sample_rate = sess->audio_sample_rate ? sess->audio_sample_rate : 48000;
	slot->channels = ch;
	slot->samples = samples;
	slot->channel_stride = (uint32_t)ch_bytes;
	slot->timecode = (int64_t)(frame->timestamp / 100ULL);
	sess->audio_count++;
	pthread_cond_signal(&sess->audio_cond);
	pthread_mutex_unlock(&sess->audio_mtx);
	ndi_session_release(sess);
}

static uint64_t ndi_output_total_bytes(void *data)
{
	struct ndi_output *o = data;
	struct ndi_send_session *sess = ndi_session_acquire(o);
	if (sess) {
		pthread_mutex_lock(&sess->video_mtx);
		uint64_t vb = sess->total_video_bytes;
		pthread_mutex_unlock(&sess->video_mtx);
		pthread_mutex_lock(&sess->audio_mtx);
		uint64_t ab = sess->total_audio_bytes;
		pthread_mutex_unlock(&sess->audio_mtx);
		ndi_session_release(sess);
		return vb + ab;
	}
	pthread_mutex_lock(&o->mtx);
	uint64_t bytes = o->last_total_bytes;
	pthread_mutex_unlock(&o->mtx);
	return bytes;
}

static void ndi_output_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "ndi_name", "OBS for Android");
	obs_data_set_default_string(settings, "ndi_groups", "");
}

static struct obs_output_info android_ndi_output_info = {
	.id = "android_ndi_output",
	.flags = OBS_OUTPUT_VIDEO | OBS_OUTPUT_AUDIO,
	.get_name = ndi_output_get_name,
	.create = ndi_output_create,
	.destroy = ndi_output_destroy,
	.start = ndi_output_start,
	.stop = ndi_output_stop,
	.update = ndi_output_update,
	.raw_video = ndi_output_raw_video,
	.raw_audio = ndi_output_raw_audio,
	.get_total_bytes = ndi_output_total_bytes,
	.get_defaults = ndi_output_get_defaults,
};

void android_ndi_register_output(void)
{
	obs_register_output(&android_ndi_output_info);
}
