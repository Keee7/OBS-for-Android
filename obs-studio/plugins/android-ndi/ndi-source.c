/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * Android NDI® Input Source (android_ndi_source).
 *
 * Features:
 *   - Dynamically loads libndi.so at runtime (no hard DT_NEEDED dependency).
 *   - Shared background discovery via Android NsdManager + WifiManager.MulticastLock
 *     + NDIlib_find_create_v2, plus manual sender IP(s) and manual source name.
 *   - Direct UYVY -> VIDEO_FORMAT_UYVY delivery (zero extra BGRA conversion for SDR);
 *     transparently handles BGRA/BGRX/RGBA/RGBX/NV12/I420 when sent by the source.
 *   - 32-bit planar float audio (FLTP) directly into OBS mixer.
 *   - Unified 100 ns A/V clock with discontinuity rebase and unique-frame stats.
 *   - Non-blocking stop on deactivate/destroy and bounded reconnect on network drop.
 */

#include "ndi-abi.h"
#include "ndi-finder.h"
#include "ndi-output.h"
#include "ndi-runtime.h"
#include "ndi-sync.h"

#include <obs-module.h>
#include <util/dstr.h>
#include <util/platform.h>
#include <util/threading.h>

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NDI_RECV_POLL_MS 100U
#define NDI_IDLE_RECONNECT_NS 5000000000ULL
#define NDI_RETRY_MIN_MS 500U
#define NDI_RETRY_MAX_MS 5000U

enum ndi_bw_setting {
	NDI_BW_HIGHEST = 0,
	NDI_BW_LOWEST = 1,
	NDI_BW_AUDIO_ONLY = 2,
};

enum ndi_cs_setting {
	NDI_CS_AUTO = 0,
	NDI_CS_709 = 1,
	NDI_CS_601 = 2,
};

enum ndi_hidden_behavior {
	NDI_BEHAVIOR_KEEP_RUNNING = 0,
	NDI_BEHAVIOR_STOP_WHEN_HIDDEN = 1,
	NDI_BEHAVIOR_PAUSE_KEEP_FRAME = 2,
};

enum ndi_latency_setting {
	NDI_LATENCY_NORMAL = 0,
	NDI_LATENCY_LOW = 1,
};

struct ndi_source {
	obs_source_t *source;
	pthread_mutex_t mtx;
	pthread_t thread;
	volatile long thread_started;
	volatile long stopping;
	volatile long hidden;

	char source_name[NDI_FINDER_NAME_LEN];
	char manual_name[NDI_FINDER_NAME_LEN];
	char extra_ips[512];
	int bandwidth;
	int colorspace;
	int color_range;
	int sync_mode;
	int hidden_behavior;
	int latency_mode;
	bool disconnect_clear;

	char status[320];
	struct obs_source_frame2 video_frame;
	struct ndi_sync_clock sync;
	uint32_t cur_w, cur_h;
	double cur_fps;
};

static void set_status(struct ndi_source *s, const char *text)
{
	pthread_mutex_lock(&s->mtx);
	snprintf(s->status, sizeof(s->status), "%s", text ? text : "");
	pthread_mutex_unlock(&s->mtx);
}

static enum video_format fourcc_to_obs_format(NDIlib_FourCC_video_type_e fourcc)
{
	switch (fourcc) {
	case NDIlib_FourCC_video_type_UYVY:
		return VIDEO_FORMAT_UYVY;
	case NDIlib_FourCC_video_type_BGRA:
		return VIDEO_FORMAT_BGRA;
	case NDIlib_FourCC_video_type_BGRX:
		return VIDEO_FORMAT_BGRX;
	case NDIlib_FourCC_video_type_RGBA:
	case NDIlib_FourCC_video_type_RGBX:
		return VIDEO_FORMAT_RGBA;
	case NDIlib_FourCC_video_type_NV12:
		return VIDEO_FORMAT_NV12;
	case NDIlib_FourCC_video_type_I420:
		return VIDEO_FORMAT_I420;
	default:
		return VIDEO_FORMAT_NONE;
	}
}

static enum speaker_layout channels_to_speakers(int ch)
{
	switch (ch) {
	case 1:
		return SPEAKERS_MONO;
	case 2:
		return SPEAKERS_STEREO;
	case 3:
		return SPEAKERS_2POINT1;
	case 4:
		return SPEAKERS_4POINT0;
	case 5:
		return SPEAKERS_4POINT1;
	case 6:
		return SPEAKERS_5POINT1;
	case 8:
		return SPEAKERS_7POINT1;
	default:
		return ch > 2 ? SPEAKERS_STEREO : SPEAKERS_MONO;
	}
}

static void deliver_video_frame(struct ndi_source *s, const NDIlib_video_frame_v2_t *vf)
{
	if (!vf || !vf->p_data || vf->xres <= 0 || vf->yres <= 0)
		return;
	const enum video_format fmt = fourcc_to_obs_format(vf->FourCC);
	if (fmt == VIDEO_FORMAT_NONE)
		return;

	const uint32_t w = (uint32_t)vf->xres;
	const uint32_t h = (uint32_t)vf->yres;
	const uint32_t stride = vf->line_stride_in_bytes > 0
					? (uint32_t)vf->line_stride_in_bytes
					: (fmt == VIDEO_FORMAT_UYVY ? w * 2U
								    : (fmt == VIDEO_FORMAT_NV12 || fmt == VIDEO_FORMAT_I420
									       ? w
									       : w * 4U));

	enum video_colorspace cs;
	if (s->colorspace == NDI_CS_709)
		cs = VIDEO_CS_709;
	else if (s->colorspace == NDI_CS_601)
		cs = VIDEO_CS_601;
	else
		cs = (h >= 720) ? VIDEO_CS_709 : VIDEO_CS_601;

	const enum video_range_type range = (s->color_range == VIDEO_RANGE_FULL) ? VIDEO_RANGE_FULL : VIDEO_RANGE_PARTIAL;

	struct obs_source_frame2 *out = &s->video_frame;
	if (out->format != fmt || out->width != w || out->height != h || out->range != range) {
		memset(out, 0, sizeof(*out));
		out->format = fmt;
		out->width = w;
		out->height = h;
		out->range = range;
		video_format_get_parameters_for_format(cs, range, fmt, out->color_matrix, out->color_range_min,
						       out->color_range_max);
	}

	out->data[0] = vf->p_data;
	out->linesize[0] = stride;
	if (fmt == VIDEO_FORMAT_NV12) {
		out->data[1] = vf->p_data + (size_t)stride * h;
		out->linesize[1] = stride;
	} else if (fmt == VIDEO_FORMAT_I420) {
		const size_t y_bytes = (size_t)stride * h;
		const uint32_t uv_stride = stride / 2U;
		out->data[1] = vf->p_data + y_bytes;
		out->linesize[1] = uv_stride;
		out->data[2] = vf->p_data + y_bytes + (size_t)uv_stride * (h / 2U);
		out->linesize[2] = uv_stride;
	}

	const uint64_t now_ns = os_gettime_ns();
	out->timestamp = ndi_sync_map_frame_mode(&s->sync, (enum ndi_sync_mode)s->sync_mode, vf->timestamp,
						 vf->timecode, now_ns, true);
	ndi_sync_observe_video_payload(&s->sync, vf->p_data, (size_t)stride * h);

	if (!(s->hidden_behavior == NDI_BEHAVIOR_PAUSE_KEEP_FRAME && os_atomic_load_long(&s->hidden)))
		obs_source_output_video2(s->source, out);

	s->cur_w = w;
	s->cur_h = h;
	if (vf->frame_rate_D > 0 && vf->frame_rate_N > 0)
		s->cur_fps = (double)vf->frame_rate_N / (double)vf->frame_rate_D;
}

static void deliver_audio_frame(struct ndi_source *s, const NDIlib_audio_frame_v3_t *af)
{
	if (!af || !af->p_data || af->sample_rate <= 0 || af->no_channels <= 0 || af->no_samples <= 0)
		return;
	if (af->FourCC != NDIlib_FourCC_audio_type_FLTP)
		return;

	const int ch = af->no_channels > MAX_AV_PLANES ? MAX_AV_PLANES : af->no_channels;
	const size_t stride = af->channel_stride_in_bytes > 0
				      ? (size_t)af->channel_stride_in_bytes
				      : (size_t)af->no_samples * sizeof(float);

	struct obs_source_audio audio;
	memset(&audio, 0, sizeof(audio));
	for (int i = 0; i < ch; i++)
		audio.data[i] = af->p_data + (size_t)i * stride;
	audio.frames = (uint32_t)af->no_samples;
	audio.speakers = channels_to_speakers(ch);
	audio.format = AUDIO_FORMAT_FLOAT_PLANAR;
	audio.samples_per_sec = (uint32_t)af->sample_rate;

	const uint64_t now_ns = os_gettime_ns();
	audio.timestamp = ndi_sync_map_frame_mode(&s->sync, (enum ndi_sync_mode)s->sync_mode, af->timestamp,
						  af->timecode, now_ns, false);

	if (!(s->hidden_behavior == NDI_BEHAVIOR_PAUSE_KEEP_FRAME && os_atomic_load_long(&s->hidden)))
		obs_source_output_audio(s->source, &audio);
}

static void sleep_interruptible(struct ndi_source *s, uint32_t ms)
{
	uint32_t slept = 0;
	while (slept < ms && !os_atomic_load_long(&s->stopping)) {
		const uint32_t step = (ms - slept) > 50U ? 50U : (ms - slept);
		os_sleep_ms(step);
		slept += step;
	}
}

static bool resolve_target_source(struct ndi_source *s, NDIlib_source_t *out, char *name_buf, size_t name_sz,
				  char *url_buf, size_t url_sz)
{
	pthread_mutex_lock(&s->mtx);
	const char *want = s->manual_name[0] ? s->manual_name : s->source_name;
	snprintf(name_buf, name_sz, "%s", want);
	pthread_mutex_unlock(&s->mtx);

	url_buf[0] = '\0';
	if (!name_buf[0])
		return false;

	struct ndi_discovered_source list[NDI_FINDER_MAX_SOURCES];
	const int n = ndi_finder_get_sources(list, NDI_FINDER_MAX_SOURCES);
	for (int i = 0; i < n; i++) {
		if (strcmp(list[i].name, name_buf) == 0) {
			snprintf(url_buf, url_sz, "%s", list[i].url);
			break;
		}
	}
	out->p_ndi_name = name_buf;
	out->p_url_address = url_buf[0] ? url_buf : NULL;
	return true;
}

static void *ndi_recv_thread(void *arg)
{
	struct ndi_source *s = arg;
	uint32_t backoff_ms = NDI_RETRY_MIN_MS;

	while (!os_atomic_load_long(&s->stopping)) {
		const struct ndi_api *api = ndi_runtime_acquire();
		if (!api) {
			set_status(s, obs_module_text("Ndi.Status.RuntimeMissing"));
			sleep_interruptible(s, 2000);
			continue;
		}

		char target_name[NDI_FINDER_NAME_LEN] = {0};
		char target_url[NDI_FINDER_URL_LEN] = {0};
		NDIlib_source_t target = {0};
		if (!resolve_target_source(s, &target, target_name, sizeof(target_name), target_url,
					   sizeof(target_url))) {
			set_status(s, obs_module_text("Ndi.Status.NoSource"));
			ndi_runtime_release();
			sleep_interruptible(s, 500);
			continue;
		}

		set_status(s, obs_module_text("Ndi.Status.Connecting"));

		NDIlib_recv_create_v3_t cfg;
		memset(&cfg, 0, sizeof(cfg));
		cfg.source_to_connect_to = target;
		cfg.color_format = NDIlib_recv_color_format_UYVY_BGRA;
		cfg.bandwidth = s->bandwidth == NDI_BW_LOWEST      ? NDIlib_recv_bandwidth_lowest
				: s->bandwidth == NDI_BW_AUDIO_ONLY ? NDIlib_recv_bandwidth_audio_only
								    : NDIlib_recv_bandwidth_highest;
		cfg.allow_video_fields = false;
		cfg.p_ndi_recv_name = "OBS for Android";

		NDIlib_recv_instance_t recv = api->recv_create_v3(&cfg);
		if (!recv) {
			set_status(s, obs_module_text("Ndi.Status.ConnectFailed"));
			ndi_runtime_release();
			sleep_interruptible(s, backoff_ms);
			backoff_ms = backoff_ms * 2U > NDI_RETRY_MAX_MS ? NDI_RETRY_MAX_MS : backoff_ms * 2U;
			continue;
		}

		ndi_sync_reset(&s->sync);
		set_status(s, obs_module_text("Ndi.Status.WaitingFrame"));
		uint64_t last_rx_ns = os_gettime_ns();
		uint64_t last_status_ns = last_rx_ns;
		bool got_any_frame = false;

		while (!os_atomic_load_long(&s->stopping)) {
			NDIlib_video_frame_v2_t vf;
			NDIlib_audio_frame_v3_t af;
			NDIlib_metadata_frame_t mf;
			memset(&vf, 0, sizeof(vf));
			memset(&af, 0, sizeof(af));
			memset(&mf, 0, sizeof(mf));

			const NDIlib_frame_type_e ft =
				api->recv_capture_v3(recv, &vf, &af, &mf, NDI_RECV_POLL_MS);

			if (ft == NDIlib_frame_type_video) {
				deliver_video_frame(s, &vf);
				api->recv_free_video_v2(recv, &vf);
				last_rx_ns = os_gettime_ns();
				got_any_frame = true;
				backoff_ms = NDI_RETRY_MIN_MS;
			} else if (ft == NDIlib_frame_type_audio) {
				deliver_audio_frame(s, &af);
				api->recv_free_audio_v3(recv, &af);
				last_rx_ns = os_gettime_ns();
				got_any_frame = true;
				backoff_ms = NDI_RETRY_MIN_MS;
			} else if (ft == NDIlib_frame_type_metadata) {
				api->recv_free_metadata(recv, &mf);
			} else if (ft == NDIlib_frame_type_error) {
				blog(LOG_WARNING, "android-ndi: receiver error on '%s', reconnecting", target_name);
				if (s->disconnect_clear)
					obs_source_output_video2(s->source, NULL);
				set_status(s, obs_module_text("Ndi.Status.Reconnecting"));
				break;
			} else if (ft == NDIlib_frame_type_none) {
				const uint64_t now = os_gettime_ns();
				if (now - last_rx_ns >= NDI_IDLE_RECONNECT_NS) {
					blog(LOG_WARNING, "android-ndi: stream idle timeout on '%s', reconnecting",
					     target_name);
					if (s->disconnect_clear)
						obs_source_output_video2(s->source, NULL);
					set_status(s, obs_module_text("Ndi.Status.Reconnecting"));
					break;
				}
			}

			const uint64_t now = os_gettime_ns();
			if (got_any_frame && now - last_status_ns >= 1000000000ULL) {
				char msg[320];
				snprintf(msg, sizeof(msg), "%s: %ux%u @ %.2f fps (unique=%llu dup=%llu av=%lldms)",
					 obs_module_text("Ndi.Status.Receiving"), s->cur_w, s->cur_h, s->cur_fps,
					 (unsigned long long)s->sync.unique_video_frames,
					 (unsigned long long)s->sync.duplicate_video_frames,
					 (long long)s->sync.last_av_offset_ms);
				set_status(s, msg);
				last_status_ns = now;
			}
		}

		api->recv_destroy(recv);
		ndi_runtime_release();

		if (!os_atomic_load_long(&s->stopping)) {
			sleep_interruptible(s, backoff_ms);
			backoff_ms = backoff_ms * 2U > NDI_RETRY_MAX_MS ? NDI_RETRY_MAX_MS : backoff_ms * 2U;
		}
	}

	return NULL;
}

static void ndi_start_thread(struct ndi_source *s)
{
	if (os_atomic_load_long(&s->thread_started))
		return;
	os_atomic_set_long(&s->stopping, 0);
	if (pthread_create(&s->thread, NULL, ndi_recv_thread, s) == 0)
		os_atomic_set_long(&s->thread_started, 1);
}

static void ndi_stop_thread(struct ndi_source *s)
{
	if (!os_atomic_load_long(&s->thread_started))
		return;
	os_atomic_set_long(&s->stopping, 1);
	pthread_join(s->thread, NULL);
	os_atomic_set_long(&s->thread_started, 0);
}

static const char *ndi_source_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("NdiSource");
}

static void ndi_source_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "ndi_source_name", "");
	obs_data_set_default_string(settings, "ndi_manual_name", "");
	obs_data_set_default_string(settings, "ndi_extra_ips", "");
	obs_data_set_default_int(settings, "ndi_bandwidth", NDI_BW_HIGHEST);
	obs_data_set_default_int(settings, "ndi_sync", NDI_SYNC_MODE_SOURCE_TIMING);
	obs_data_set_default_int(settings, "ndi_behavior", NDI_BEHAVIOR_KEEP_RUNNING);
	obs_data_set_default_int(settings, "ndi_latency", NDI_LATENCY_NORMAL);
	obs_data_set_default_bool(settings, "ndi_disconnect_clear", true);
	obs_data_set_default_int(settings, "ndi_colorspace", NDI_CS_AUTO);
	obs_data_set_default_int(settings, "ndi_color_range", VIDEO_RANGE_PARTIAL);
}

static void apply_settings(struct ndi_source *s, obs_data_t *settings)
{
	pthread_mutex_lock(&s->mtx);
	snprintf(s->source_name, sizeof(s->source_name), "%s", obs_data_get_string(settings, "ndi_source_name"));
	snprintf(s->manual_name, sizeof(s->manual_name), "%s", obs_data_get_string(settings, "ndi_manual_name"));
	snprintf(s->extra_ips, sizeof(s->extra_ips), "%s", obs_data_get_string(settings, "ndi_extra_ips"));
	s->bandwidth = (int)obs_data_get_int(settings, "ndi_bandwidth");
	s->sync_mode = (int)obs_data_get_int(settings, "ndi_sync");
	s->hidden_behavior = (int)obs_data_get_int(settings, "ndi_behavior");
	s->latency_mode = (int)obs_data_get_int(settings, "ndi_latency");
	s->disconnect_clear = obs_data_get_bool(settings, "ndi_disconnect_clear");
	s->colorspace = (int)obs_data_get_int(settings, "ndi_colorspace");
	s->color_range = (int)obs_data_get_int(settings, "ndi_color_range");
	pthread_mutex_unlock(&s->mtx);

	obs_source_set_async_unbuffered(s->source, s->latency_mode == NDI_LATENCY_LOW);
	ndi_finder_set_source_extra_ips(s, s->extra_ips);
}

static void *ndi_source_create(obs_data_t *settings, obs_source_t *source)
{
	struct ndi_source *s = bzalloc(sizeof(*s));
	s->source = source;
	pthread_mutex_init(&s->mtx, NULL);
	ndi_finder_acquire();
	apply_settings(s, settings);
	os_atomic_set_long(&s->hidden, obs_source_showing(source) ? 0 : 1);
	if (obs_source_active(source) ||
	    (s->hidden_behavior == NDI_BEHAVIOR_KEEP_RUNNING && obs_source_showing(source)))
		ndi_start_thread(s);
	return s;
}

static void ndi_source_destroy(void *data)
{
	struct ndi_source *s = data;
	ndi_stop_thread(s);
	ndi_finder_remove_source_extra_ips(s);
	ndi_finder_release();
	pthread_mutex_destroy(&s->mtx);
	bfree(s);
}

static void ndi_source_show(void *data)
{
	struct ndi_source *s = data;
	os_atomic_set_long(&s->hidden, 0);
	if (s->hidden_behavior == NDI_BEHAVIOR_STOP_WHEN_HIDDEN)
		ndi_start_thread(s);
}

static void ndi_source_hide(void *data)
{
	struct ndi_source *s = data;
	os_atomic_set_long(&s->hidden, 1);
	if (s->hidden_behavior == NDI_BEHAVIOR_STOP_WHEN_HIDDEN) {
		ndi_stop_thread(s);
		if (s->disconnect_clear)
			obs_source_output_video2(s->source, NULL);
	}
}

static void ndi_source_activate(void *data)
{
	struct ndi_source *s = data;
	if (s->hidden_behavior == NDI_BEHAVIOR_STOP_WHEN_HIDDEN && os_atomic_load_long(&s->hidden))
		return;
	ndi_start_thread(s);
}

static void ndi_source_deactivate(void *data)
{
	struct ndi_source *s = data;
	if (s->hidden_behavior == NDI_BEHAVIOR_KEEP_RUNNING)
		return;
	ndi_stop_thread(s);
	/* Never clear the video frame when user chose PAUSE_KEEP_FRAME ("keep last frame"). */
	if (s->disconnect_clear && s->hidden_behavior != NDI_BEHAVIOR_PAUSE_KEEP_FRAME)
		obs_source_output_video2(s->source, NULL);
}

static void ndi_source_update(void *data, obs_data_t *settings)
{
	struct ndi_source *s = data;
	const bool was_running = os_atomic_load_long(&s->thread_started) != 0;
	ndi_stop_thread(s);
	apply_settings(s, settings);
	const bool can_run = !(s->hidden_behavior == NDI_BEHAVIOR_STOP_WHEN_HIDDEN && os_atomic_load_long(&s->hidden));
	if (can_run && (was_running || obs_source_active(s->source) || obs_source_showing(s->source)))
		ndi_start_thread(s);
}

static void format_status_text(struct ndi_source *s, char *out, size_t out_sz)
{
	if (!out || !out_sz)
		return;
	out[0] = '\0';
	if (s) {
		pthread_mutex_lock(&s->mtx);
		snprintf(out, out_sz, "%s", s->status);
		pthread_mutex_unlock(&s->mtx);
	}
	if (!out[0]) {
		if (ndi_runtime_is_loaded()) {
			char ver[128] = {0};
			ndi_runtime_get_info(ver, sizeof(ver), NULL, 0, NULL, 0);
			snprintf(out, out_sz, "%s (%s%s)", obs_module_text("Ndi.Status.Ready"), ver,
				 ndi_runtime_has_hx_bridge() ? " + MediaCodec HX" : "");
		} else {
			snprintf(out, out_sz, "%s", obs_module_text("Ndi.Status.RuntimeMissing"));
		}
	}
}

static void refresh_status_property(obs_properties_t *props, struct ndi_source *s)
{
	obs_property_t *st = obs_properties_get(props, "ndi_status");
	if (!st)
		return;
	char status_text[384] = {0};
	format_status_text(s, status_text, sizeof(status_text));
	obs_property_set_description(st, status_text);
}

static void populate_source_list(obs_property_t *list, const char *current)
{
	obs_property_list_clear(list);
	obs_property_list_add_string(list, obs_module_text("Ndi.Source.None"), "");

	struct ndi_discovered_source srcs[NDI_FINDER_MAX_SOURCES];
	const int n = ndi_finder_get_sources(srcs, NDI_FINDER_MAX_SOURCES);
	bool found_cur = !current || !current[0];
	for (int i = 0; i < n; i++) {
		bool dup = false;
		for (int j = 0; j < i; j++) {
			if (strcmp(srcs[j].name, srcs[i].name) == 0) {
				dup = true;
				break;
			}
		}
		if (dup)
			continue;
		char label[NDI_FINDER_NAME_LEN + NDI_FINDER_URL_LEN + 16];
		if (srcs[i].url[0])
			snprintf(label, sizeof(label), "%s [%s]", srcs[i].name, srcs[i].url);
		else
			snprintf(label, sizeof(label), "%s", srcs[i].name);
		obs_property_list_add_string(list, label, srcs[i].name);
		if (current && strcmp(current, srcs[i].name) == 0)
			found_cur = true;
	}
	if (!found_cur)
		obs_property_list_add_string(list, current, current);
}

static bool on_refresh_sources(obs_properties_t *props, obs_property_t *property, void *data)
{
	UNUSED_PARAMETER(property);
	struct ndi_source *s = data;
	ndi_finder_refresh();
	os_sleep_ms(450);
	obs_property_t *list = obs_properties_get(props, "ndi_source_name");
	if (list)
		populate_source_list(list, s ? s->source_name : "");
	refresh_status_property(props, s);
	return true;
}

static bool on_extra_ips_modified(void *priv, obs_properties_t *props, obs_property_t *property, obs_data_t *settings)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(property);
	/* Keep the background finder in sync without returning true: rebuilding the
	 * properties view on every keystroke destroys the focused QLineEdit and
	 * dismisses the Android soft keyboard after each character. */
	ndi_finder_set_source_extra_ips(priv, obs_data_get_string(settings, "ndi_extra_ips"));
	return false;
}

static obs_properties_t *ndi_source_properties(void *data)
{
	struct ndi_source *s = data;
	obs_properties_t *props = obs_properties_create();

	obs_property_t *src_list = obs_properties_add_list(
		props, "ndi_source_name", obs_module_text("Ndi.Prop.Source"), OBS_COMBO_TYPE_LIST,
		OBS_COMBO_FORMAT_STRING);
	populate_source_list(src_list, s ? s->source_name : "");
	obs_properties_add_button2(props, "ndi_refresh", obs_module_text("Ndi.Prop.Refresh"), on_refresh_sources, s);

	obs_property_t *ips =
		obs_properties_add_text(props, "ndi_extra_ips", obs_module_text("Ndi.Prop.ExtraIps"), OBS_TEXT_DEFAULT);
	obs_property_set_long_description(ips, obs_module_text("Ndi.Prop.ExtraIps.Desc"));
	obs_property_set_modified_callback2(ips, on_extra_ips_modified, s);

	obs_property_t *manual = obs_properties_add_text(
		props, "ndi_manual_name", obs_module_text("Ndi.Prop.ManualName"), OBS_TEXT_DEFAULT);
	obs_property_set_long_description(manual, obs_module_text("Ndi.Prop.ManualName.Desc"));

	obs_property_t *bw = obs_properties_add_list(props, "ndi_bandwidth", obs_module_text("Ndi.Prop.Bandwidth"),
						     OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(bw, obs_module_text("Ndi.Bandwidth.Highest"), NDI_BW_HIGHEST);
	obs_property_list_add_int(bw, obs_module_text("Ndi.Bandwidth.Lowest"), NDI_BW_LOWEST);
	obs_property_list_add_int(bw, obs_module_text("Ndi.Bandwidth.AudioOnly"), NDI_BW_AUDIO_ONLY);

	obs_property_t *sync = obs_properties_add_list(props, "ndi_sync", obs_module_text("Ndi.Prop.Sync"),
						       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(sync, obs_module_text("Ndi.Sync.SourceTiming"), NDI_SYNC_MODE_SOURCE_TIMING);
	obs_property_list_add_int(sync, obs_module_text("Ndi.Sync.Internal"), NDI_SYNC_MODE_INTERNAL);
	obs_property_list_add_int(sync, obs_module_text("Ndi.Sync.Network"), NDI_SYNC_MODE_NETWORK);

	obs_property_t *beh = obs_properties_add_list(props, "ndi_behavior", obs_module_text("Ndi.Prop.Behavior"),
						      OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(beh, obs_module_text("Ndi.Behavior.KeepRunning"), NDI_BEHAVIOR_KEEP_RUNNING);
	obs_property_list_add_int(beh, obs_module_text("Ndi.Behavior.StopWhenHidden"), NDI_BEHAVIOR_STOP_WHEN_HIDDEN);
	obs_property_list_add_int(beh, obs_module_text("Ndi.Behavior.PauseKeepFrame"), NDI_BEHAVIOR_PAUSE_KEEP_FRAME);

	obs_property_t *lat = obs_properties_add_list(props, "ndi_latency", obs_module_text("Ndi.Prop.Latency"),
						      OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(lat, obs_module_text("Ndi.Latency.Normal"), NDI_LATENCY_NORMAL);
	obs_property_list_add_int(lat, obs_module_text("Ndi.Latency.Low"), NDI_LATENCY_LOW);

	obs_properties_add_bool(props, "ndi_disconnect_clear", obs_module_text("Ndi.Prop.DisconnectClear"));

	obs_property_t *cs = obs_properties_add_list(props, "ndi_colorspace", obs_module_text("Ndi.Prop.ColorSpace"),
						     OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(cs, obs_module_text("Ndi.ColorSpace.Auto"), NDI_CS_AUTO);
	obs_property_list_add_int(cs, "Rec. 709", NDI_CS_709);
	obs_property_list_add_int(cs, "Rec. 601", NDI_CS_601);

	obs_property_t *cr = obs_properties_add_list(props, "ndi_color_range", obs_module_text("Ndi.Prop.ColorRange"),
						     OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(cr, obs_module_text("Ndi.ColorRange.Partial"), VIDEO_RANGE_PARTIAL);
	obs_property_list_add_int(cr, obs_module_text("Ndi.ColorRange.Full"), VIDEO_RANGE_FULL);

	char status_text[384] = {0};
	format_status_text(s, status_text, sizeof(status_text));
	obs_properties_add_text(props, "ndi_status", status_text, OBS_TEXT_INFO);
	return props;
}

static struct obs_source_info android_ndi_source_info = {
	.id = "android_ndi_source",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE,
	.get_name = ndi_source_get_name,
	.create = ndi_source_create,
	.destroy = ndi_source_destroy,
	.show = ndi_source_show,
	.hide = ndi_source_hide,
	.activate = ndi_source_activate,
	.deactivate = ndi_source_deactivate,
	.update = ndi_source_update,
	.get_defaults = ndi_source_get_defaults,
	.get_properties = ndi_source_properties,
	.icon_type = OBS_ICON_TYPE_MEDIA,
};

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("android-ndi", "en-US")

bool obs_module_load(void)
{
	obs_register_source(&android_ndi_source_info);
	android_ndi_register_output();
	blog(LOG_INFO, "android-ndi: module loaded (input source + program output, dynamic libndi.so loader ready)");
	return true;
}

void obs_module_unload(void)
{
	blog(LOG_INFO, "android-ndi: module unloaded");
}
