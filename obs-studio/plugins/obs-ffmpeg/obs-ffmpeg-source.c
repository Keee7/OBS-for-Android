/*
 * Copyright (c) 2015 John R. Bradley <jrb@turrettech.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <obs-module.h>
#include <util/threading.h>
#include <util/platform.h>
#include <util/dstr.h>

#include "obs-ffmpeg-compat.h"
#include "obs-ffmpeg-formats.h"

#include <media-playback/media-playback.h>

#ifdef __ANDROID__
#include <graphics/graphics.h>
#include <media/NdkImage.h>
#include <media/NdkImageReader.h>
#include <media/NdkMediaError.h>
#endif

#define FF_LOG_S(source, level, format, ...) \
	blog(level, "[Media Source '%s']: " format, obs_source_get_name(source), ##__VA_ARGS__)
#define FF_BLOG(level, format, ...) FF_LOG_S(s->source, level, format, ##__VA_ARGS__)

struct ffmpeg_source {
	media_playback_t *media;
	bool destroy_media;

	enum video_range_type range;
	bool is_linear_alpha;
	obs_source_t *source;
	obs_hotkey_id hotkey;

	char *input;
	char *input_format;
	char *ffmpeg_options;
	int buffering_mb;
	int speed_percent;
	bool is_looping;
	bool is_local_file;
	bool is_hw_decoding;
	bool full_decode;
	bool is_clear_on_media_end;
	bool restart_on_activate;
	bool close_when_inactive;
	bool seekable;
	bool is_stinger;
	bool is_track_matte;
	bool log_changes;

	pthread_t reconnect_thread;
	pthread_mutex_t reconnect_mutex;
	bool reconnect_thread_valid;
	os_event_t *reconnect_stop_event;
	volatile bool reconnecting;
	int reconnect_delay_sec;

	enum obs_media_state state;
	obs_hotkey_pair_id play_pause_hotkey;
	obs_hotkey_id stop_hotkey;
#ifdef __ANDROID__
	pthread_mutex_t gpu_mx;
	AImageReader *gpu_reader;
	ANativeWindow *gpu_window;
	AImage *gpu_acquired_image;
	gs_android_external_texture_t *gpu_external;
	gs_texture_t *gpu_image;
	uint32_t gpu_width;
	uint32_t gpu_height;
	uint32_t gpu_tex_width;
	uint32_t gpu_tex_height;
	volatile long gpu_active;
	volatile long gpu_surface_disabled;
	volatile long gpu_fallback_pending;
	bool gpu_cleared;
	volatile long cpu_width;
	volatile long cpu_height;
#endif
};

// Used to safely cancel and join any active reconnect threads
// Use this to join any finished reconnect thread too!
static void stop_reconnect_thread(struct ffmpeg_source *s)
{
	if (s->is_local_file)
		return;
	pthread_mutex_lock(&s->reconnect_mutex);
	if (s->reconnect_thread_valid) {
		os_event_signal(s->reconnect_stop_event);
		pthread_join(s->reconnect_thread, NULL);
		s->reconnect_thread_valid = false;
		os_atomic_set_bool(&s->reconnecting, false);
		os_event_reset(s->reconnect_stop_event);
	}
	pthread_mutex_unlock(&s->reconnect_mutex);
}

static void set_media_state(void *data, enum obs_media_state state)
{
	struct ffmpeg_source *s = data;
	s->state = state;
}

static bool is_local_file_modified(obs_properties_t *props, obs_property_t *prop, obs_data_t *settings)
{
	UNUSED_PARAMETER(prop);

	bool enabled = obs_data_get_bool(settings, "is_local_file");
	obs_property_t *input = obs_properties_get(props, "input");
	obs_property_t *input_format = obs_properties_get(props, "input_format");
	obs_property_t *local_file = obs_properties_get(props, "local_file");
	obs_property_t *looping = obs_properties_get(props, "looping");
	obs_property_t *buffering = obs_properties_get(props, "buffering_mb");
	obs_property_t *seekable = obs_properties_get(props, "seekable");
	obs_property_t *speed = obs_properties_get(props, "speed_percent");
	obs_property_t *reconnect_delay_sec = obs_properties_get(props, "reconnect_delay_sec");
	obs_property_set_visible(input, !enabled);
	obs_property_set_visible(input_format, !enabled);
	obs_property_set_visible(buffering, !enabled);
	obs_property_set_visible(local_file, enabled);
	obs_property_set_visible(looping, enabled);
	obs_property_set_visible(speed, enabled);
	obs_property_set_visible(seekable, !enabled);
	obs_property_set_visible(reconnect_delay_sec, !enabled);

	return true;
}

static void ffmpeg_source_defaults(obs_data_t *settings)
{
	obs_data_set_default_bool(settings, "is_local_file", true);
#ifdef __ANDROID__
	obs_data_set_default_bool(settings, "hw_decode", true);
#endif
	obs_data_set_default_bool(settings, "looping", false);
	obs_data_set_default_bool(settings, "clear_on_media_end", true);
	obs_data_set_default_bool(settings, "restart_on_activate", true);
	obs_data_set_default_bool(settings, "linear_alpha", false);
	obs_data_set_default_int(settings, "reconnect_delay_sec", 10);
	obs_data_set_default_int(settings, "buffering_mb", 2);
	obs_data_set_default_int(settings, "speed_percent", 100);
	obs_data_set_default_bool(settings, "log_changes", true);
}

static const char *media_filter =
	" (*.mp4 *.m4v *.ts *.mov *.mxf *.flv *.mkv *.avi *.mp3 *.ogg *.aac *.wav *.gif *.webm);;";
static const char *video_filter = " (*.mp4 *.m4v *.ts *.mov *.mxf *.flv *.mkv *.avi *.gif *.webm);;";
static const char *audio_filter = " (*.mp3 *.aac *.ogg *.wav);;";

static obs_properties_t *ffmpeg_source_getproperties(void *data)
{
	struct ffmpeg_source *s = data;
	struct dstr filter = {0};
	struct dstr path = {0};

	obs_properties_t *props = obs_properties_create();

	obs_properties_set_flags(props, OBS_PROPERTIES_DEFER_UPDATE);

	obs_property_t *prop;
	// use this when obs allows non-readonly paths
	prop = obs_properties_add_bool(props, "is_local_file", obs_module_text("LocalFile"));

	obs_property_set_modified_callback(prop, is_local_file_modified);

	dstr_copy(&filter, obs_module_text("MediaFileFilter.AllMediaFiles"));
	dstr_cat(&filter, media_filter);
	dstr_cat(&filter, obs_module_text("MediaFileFilter.VideoFiles"));
	dstr_cat(&filter, video_filter);
	dstr_cat(&filter, obs_module_text("MediaFileFilter.AudioFiles"));
	dstr_cat(&filter, audio_filter);
	dstr_cat(&filter, obs_module_text("MediaFileFilter.AllFiles"));
	dstr_cat(&filter, " (*.*)");

	if (s && s->input && *s->input) {
		const char *slash;

		dstr_copy(&path, s->input);
		dstr_replace(&path, "\\", "/");
		slash = strrchr(path.array, '/');
		if (slash)
			dstr_resize(&path, slash - path.array + 1);
	}

	obs_properties_add_path(props, "local_file", obs_module_text("LocalFile"), OBS_PATH_FILE, filter.array,
				path.array);
	dstr_free(&filter);
	dstr_free(&path);

	obs_properties_add_bool(props, "looping", obs_module_text("Looping"));

	obs_properties_add_bool(props, "restart_on_activate", obs_module_text("RestartWhenActivated"));

	prop = obs_properties_add_int_slider(props, "buffering_mb", obs_module_text("BufferingMB"), 0, 16, 1);
	obs_property_int_set_suffix(prop, " MB");

	obs_properties_add_text(props, "input", obs_module_text("Input"), OBS_TEXT_DEFAULT);

	obs_properties_add_text(props, "input_format", obs_module_text("InputFormat"), OBS_TEXT_DEFAULT);

	prop = obs_properties_add_int_slider(props, "reconnect_delay_sec", obs_module_text("ReconnectDelayTime"), 1, 60,
					     1);
	obs_property_int_set_suffix(prop, " S");

	obs_properties_add_bool(props, "hw_decode", obs_module_text("HardwareDecode"));

	obs_properties_add_bool(props, "clear_on_media_end", obs_module_text("ClearOnMediaEnd"));

	prop = obs_properties_add_bool(props, "close_when_inactive", obs_module_text("CloseFileWhenInactive"));

	obs_property_set_long_description(prop, obs_module_text("CloseFileWhenInactive.ToolTip"));

	prop = obs_properties_add_int_slider(props, "speed_percent", obs_module_text("SpeedPercentage"), 1, 200, 1);
	obs_property_int_set_suffix(prop, "%");

	prop = obs_properties_add_list(props, "color_range", obs_module_text("ColorRange"), OBS_COMBO_TYPE_LIST,
				       OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(prop, obs_module_text("ColorRange.Auto"), VIDEO_RANGE_DEFAULT);
	obs_property_list_add_int(prop, obs_module_text("ColorRange.Partial"), VIDEO_RANGE_PARTIAL);
	obs_property_list_add_int(prop, obs_module_text("ColorRange.Full"), VIDEO_RANGE_FULL);

	obs_properties_add_bool(props, "linear_alpha", obs_module_text("LinearAlpha"));

	obs_properties_add_bool(props, "seekable", obs_module_text("Seekable"));

	prop = obs_properties_add_text(props, "ffmpeg_options", obs_module_text("FFmpegOpts"), OBS_TEXT_DEFAULT);
	obs_property_set_long_description(prop, obs_module_text("FFmpegOpts.ToolTip.Source"));

	return props;
}

static void dump_source_info(struct ffmpeg_source *s, const char *input, const char *input_format)
{
	if (!s->log_changes)
		return;
	FF_BLOG(LOG_INFO,
		"settings:\n"
		"\tinput:                   %s\n"
		"\tinput_format:            %s\n"
		"\tspeed:                   %d\n"
		"\tis_looping:              %s\n"
		"\tis_linear_alpha:         %s\n"
		"\tis_hw_decoding:          %s\n"
		"\tis_clear_on_media_end:   %s\n"
		"\trestart_on_activate:     %s\n"
		"\tclose_when_inactive:     %s\n"
		"\tfull_decode:             %s\n"
		"\tffmpeg_options:          %s",
		input ? input : "(null)", input_format ? input_format : "(null)", s->speed_percent,
		s->is_looping ? "yes" : "no", s->is_linear_alpha ? "yes" : "no", s->is_hw_decoding ? "yes" : "no",
		s->is_clear_on_media_end ? "yes" : "no", s->restart_on_activate ? "yes" : "no",
		s->close_when_inactive ? "yes" : "no", s->full_decode ? "yes" : "no", s->ffmpeg_options);
}

static inline void set_cpu_video_size(struct ffmpeg_source *s, const struct obs_source_frame *f)
{
#ifdef __ANDROID__
	if (f) {
		os_atomic_set_long(&s->cpu_width, (long)f->width);
		os_atomic_set_long(&s->cpu_height, (long)f->height);
	} else {
		os_atomic_set_long(&s->cpu_width, 0);
		os_atomic_set_long(&s->cpu_height, 0);
	}
#else
	UNUSED_PARAMETER(s);
	UNUSED_PARAMETER(f);
#endif
}

static inline void clear_video_output(struct ffmpeg_source *s)
{
	set_cpu_video_size(s, NULL);
#ifdef __ANDROID__
	pthread_mutex_lock(&s->gpu_mx);
	s->gpu_cleared = true;
	pthread_mutex_unlock(&s->gpu_mx);
#endif
	obs_source_output_video(s->source, NULL);
}

#ifdef __ANDROID__
static void destroy_android_gpu_surface(struct ffmpeg_source *s)
{
	os_atomic_set_long(&s->gpu_active, 0);
	obs_enter_graphics();
	if (s->gpu_external) {
		gs_android_external_texture_destroy(s->gpu_external);
		s->gpu_external = NULL;
	}
	s->gpu_image = NULL;
	s->gpu_tex_width = 0;
	s->gpu_tex_height = 0;
	obs_leave_graphics();

	pthread_mutex_lock(&s->gpu_mx);
	if (s->gpu_acquired_image) {
		AImage_delete(s->gpu_acquired_image);
		s->gpu_acquired_image = NULL;
	}
	if (s->gpu_reader) {
		AImageReader_delete(s->gpu_reader);
		s->gpu_reader = NULL;
	}
	s->gpu_window = NULL;
	s->gpu_width = 0;
	s->gpu_height = 0;
	s->gpu_cleared = false;
	pthread_mutex_unlock(&s->gpu_mx);
}

static void *acquire_android_native_window(void *opaque, int width, int height)
{
	struct ffmpeg_source *s = opaque;
	int w = width > 0 ? width : 1920;
	int h = height > 0 ? height : 1080;

	pthread_mutex_lock(&s->gpu_mx);
	if (!s->gpu_reader) {
		media_status_t st = AImageReader_newWithUsage(w, h, AIMAGE_FORMAT_PRIVATE,
							      AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE, 4, &s->gpu_reader);
		if (st != AMEDIA_OK || !s->gpu_reader) {
			FF_BLOG(LOG_WARNING, "AImageReader_newWithUsage(%dx%d) failed (%d)", w, h, (int)st);
			s->gpu_reader = NULL;
			s->gpu_window = NULL;
		} else if (AImageReader_getWindow(s->gpu_reader, &s->gpu_window) != AMEDIA_OK || !s->gpu_window) {
			FF_BLOG(LOG_WARNING, "AImageReader_getWindow failed");
			AImageReader_delete(s->gpu_reader);
			s->gpu_reader = NULL;
			s->gpu_window = NULL;
		}
	}
	ANativeWindow *win = s->gpu_window;
	pthread_mutex_unlock(&s->gpu_mx);
	return win;
}

static void release_android_native_window(void *opaque)
{
	struct ffmpeg_source *s = opaque;
	pthread_mutex_lock(&s->gpu_mx);
	if (s->gpu_acquired_image) {
		AImage_delete(s->gpu_acquired_image);
		s->gpu_acquired_image = NULL;
	}
	if (s->gpu_reader) {
		AImageReader_delete(s->gpu_reader);
		s->gpu_reader = NULL;
	}
	s->gpu_window = NULL;
	pthread_mutex_unlock(&s->gpu_mx);
}

static void on_android_gpu_fail(void *opaque)
{
	struct ffmpeg_source *s = opaque;
	FF_BLOG(LOG_WARNING, "Android MediaCodec Surface output unusable; falling back to non-Surface decode");
	os_atomic_set_long(&s->gpu_surface_disabled, 1);
	os_atomic_set_long(&s->gpu_active, 0);
	os_atomic_set_long(&s->gpu_fallback_pending, 1);
}

static void on_android_gpu_frame(void *opaque, uint32_t width, uint32_t height, enum video_colorspace space,
				 enum video_range_type range, enum video_trc trc, bool preload)
{
	struct ffmpeg_source *s = opaque;
	UNUSED_PARAMETER(range);

	if (space == VIDEO_CS_2100_PQ || space == VIDEO_CS_2100_HLG || trc == VIDEO_TRC_PQ || trc == VIDEO_TRC_HLG) {
		on_android_gpu_fail(s);
		return;
	}

	pthread_mutex_lock(&s->gpu_mx);
	s->gpu_width = width;
	s->gpu_height = height;
	s->gpu_cleared = false;
	pthread_mutex_unlock(&s->gpu_mx);

	os_atomic_set_long(&s->gpu_active, 1);

	if (preload && !s->is_local_file && os_atomic_set_bool(&s->reconnecting, false))
		FF_BLOG(LOG_INFO, "Reconnected.");
}

static bool android_gpu_surface_supported(void)
{
	static pthread_mutex_t probe_mx = PTHREAD_MUTEX_INITIALIZER;
	static bool probed = false;
	static bool supported = false;

	pthread_mutex_lock(&probe_mx);
	if (!probed) {
		struct obs_video_info ovi;
		if (obs_get_video_info(&ovi)) {
			probed = true;
			obs_enter_graphics();
			gs_android_external_texture_t *probe = gs_android_external_texture_create(4, 4);
			if (probe) {
				gs_android_external_texture_destroy(probe);
				supported = true;
			}
			obs_leave_graphics();
			if (!supported)
				blog(LOG_WARNING,
				     "Android GPU Surface interop unsupported; using non-Surface decoding");
		}
	}
	const bool ok = supported;
	pthread_mutex_unlock(&probe_mx);
	return ok;
}

static void check_async_filter_cb(obs_source_t *parent, obs_source_t *filter, void *param)
{
	UNUSED_PARAMETER(parent);
	bool *found = param;
	if (*found || !obs_source_enabled(filter))
		return;
	const uint32_t flags = obs_source_get_output_flags(filter);
	if ((flags & OBS_SOURCE_ASYNC) != 0 && (flags & OBS_SOURCE_VIDEO) != 0)
		*found = true;
}

static bool has_active_async_video_filter(obs_source_t *source)
{
	bool found = false;
	obs_source_enum_filters(source, check_async_filter_cb, &found);
	return found;
}
#endif

static void get_frame(void *opaque, struct obs_source_frame *f)
{
	struct ffmpeg_source *s = opaque;
	set_cpu_video_size(s, f);
	obs_source_output_video(s->source, f);
}

static void preload_frame(void *opaque, struct obs_source_frame *f)
{
	struct ffmpeg_source *s = opaque;
	if (s->close_when_inactive)
		return;

	set_cpu_video_size(s, f);
	if (s->is_clear_on_media_end || s->is_looping)
		obs_source_preload_video(s->source, f);

	if (!s->is_local_file && os_atomic_set_bool(&s->reconnecting, false))
		FF_BLOG(LOG_INFO, "Reconnected.");
}

static void seek_frame(void *opaque, struct obs_source_frame *f)
{
	struct ffmpeg_source *s = opaque;
	set_cpu_video_size(s, f);
	obs_source_set_video_frame(s->source, f);
}

static void get_audio(void *opaque, struct obs_source_audio *a)
{
	struct ffmpeg_source *s = opaque;
	obs_source_output_audio(s->source, a);

	if (!s->is_local_file && os_atomic_set_bool(&s->reconnecting, false))
		FF_BLOG(LOG_INFO, "Reconnected.");
}

static void media_stopped(void *opaque)
{
	struct ffmpeg_source *s = opaque;
	if (s->is_clear_on_media_end && !s->is_track_matte) {
		clear_video_output(s);
	}

	if ((s->close_when_inactive || !s->is_local_file) && s->media)
		s->destroy_media = true;

	if (s->state != OBS_MEDIA_STATE_STOPPED) {
		set_media_state(s, OBS_MEDIA_STATE_ENDED);
		obs_source_media_ended(s->source);
	}
}

static void ffmpeg_source_open_internal(struct ffmpeg_source *s, bool allow_gpu_surface)
{
	if (s->input && *s->input) {
#ifdef __ANDROID__
		const bool use_gpu_surface = allow_gpu_surface && !os_atomic_load_long(&s->gpu_surface_disabled) &&
					     !s->full_decode && !s->is_stinger && !s->is_track_matte &&
					     !s->is_linear_alpha && !has_active_async_video_filter(s->source) &&
					     android_gpu_surface_supported();
#endif
		struct mp_media_info info = {
			.opaque = s,
			.v_cb = get_frame,
			.v_preload_cb = preload_frame,
			.v_seek_cb = seek_frame,
			.a_cb = get_audio,
			.stop_cb = media_stopped,
#ifdef __ANDROID__
			.acquire_native_window = use_gpu_surface ? acquire_android_native_window : NULL,
			.release_native_window = release_android_native_window,
			.v_gpu_cb = on_android_gpu_frame,
			.v_gpu_fail_cb = on_android_gpu_fail,
#endif
			.path = s->input,
			.format = s->input_format,
			.buffering = s->buffering_mb * 1024 * 1024,
			.speed = s->speed_percent,
			.force_range = s->range,
			.is_linear_alpha = s->is_linear_alpha,
			.hardware_decoding = s->is_hw_decoding,
			.ffmpeg_options = s->ffmpeg_options,
			.is_local_file = s->is_local_file || s->seekable,
			.reconnecting = s->reconnecting,
			.request_preload = s->is_stinger,
			.full_decode = s->full_decode,
		};

		s->media = media_playback_create(&info);
	}
}

static void ffmpeg_source_open(struct ffmpeg_source *s)
{
	ffmpeg_source_open_internal(s, true);
}

static void ffmpeg_source_start(struct ffmpeg_source *s)
{
	if (!s->media)
		ffmpeg_source_open(s);

	if (!s->media)
		return;

	media_playback_play(s->media, s->is_looping, s->reconnecting);
	if (s->is_local_file && media_playback_has_video(s->media) && (s->is_clear_on_media_end || s->is_looping))
		obs_source_show_preloaded_video(s->source);
	else
		clear_video_output(s);
	set_media_state(s, OBS_MEDIA_STATE_PLAYING);
	obs_source_media_started(s->source);
}

static void *ffmpeg_source_reconnect(void *data)
{
	struct ffmpeg_source *s = data;

	int ret = os_event_timedwait(s->reconnect_stop_event, s->reconnect_delay_sec * 1000);
	if (ret == 0 || s->media)
		return NULL;

	bool active = obs_source_active(s->source);
	if (!s->close_when_inactive || active)
		ffmpeg_source_open(s);

	if (!s->restart_on_activate || active)
		ffmpeg_source_start(s);

	return NULL;
}

#ifdef __ANDROID__
static void reopen_without_android_gpu_surface(struct ffmpeg_source *s, const char *reason, bool permanent)
{
	if (permanent)
		os_atomic_set_long(&s->gpu_surface_disabled, 1);
	os_atomic_set_long(&s->gpu_active, 0);
	os_atomic_set_long(&s->gpu_fallback_pending, 0);

	if (!s->media)
		return;

	const enum obs_media_state prev_state = s->state;
	const bool should_start = prev_state == OBS_MEDIA_STATE_PLAYING || prev_state == OBS_MEDIA_STATE_PAUSED;
	const int64_t pos_ms = should_start && s->is_local_file ? media_playback_get_current_time(s->media) : 0;
	FF_BLOG(LOG_WARNING, "%s", reason);

	media_playback_destroy(s->media);
	s->media = NULL;
	destroy_android_gpu_surface(s);

	if (!should_start && prev_state == OBS_MEDIA_STATE_ENDED && s->close_when_inactive &&
	    !obs_source_active(s->source)) {
		clear_video_output(s);
		set_media_state(s, prev_state);
		return;
	}

	ffmpeg_source_open_internal(s, false);
	if (!s->media) {
		set_media_state(s, prev_state);
		return;
	}

	if (!should_start) {
		if (prev_state == OBS_MEDIA_STATE_STOPPED ||
		    (prev_state == OBS_MEDIA_STATE_ENDED && s->is_clear_on_media_end))
			clear_video_output(s);
		set_media_state(s, prev_state);
		return;
	}

	ffmpeg_source_start(s);
	if (s->is_local_file && pos_ms > 0) {
		const int64_t seek_ms = s->speed_percent != 100 ? pos_ms * 100 / s->speed_percent : pos_ms;
		media_playback_seek(s->media, seek_ms);
	}
	if (prev_state == OBS_MEDIA_STATE_PAUSED) {
		media_playback_play_pause(s->media, true);
		set_media_state(s, OBS_MEDIA_STATE_PAUSED);
	}
}
#endif

static void ffmpeg_source_tick(void *data, float seconds)
{
	UNUSED_PARAMETER(seconds);

	struct ffmpeg_source *s = data;
#ifdef __ANDROID__
	if (!s->destroy_media && s->media) {
		if (os_atomic_load_long(&s->gpu_fallback_pending)) {
			reopen_without_android_gpu_surface(
				s, "Android GPU Surface unavailable or incompatible; falling back to non-Surface decode",
				true);
		} else if (os_atomic_load_long(&s->gpu_active) && has_active_async_video_filter(s->source)) {
			reopen_without_android_gpu_surface(
				s, "Async video filter active; falling back to non-Surface decode", false);
		}
	}
#endif
	if (s->destroy_media) {
		if (s->media) {
			media_playback_destroy(s->media);
			s->media = NULL;
		}
#ifdef __ANDROID__
		destroy_android_gpu_surface(s);
#endif

		s->destroy_media = false;

		if (!s->is_local_file) {
			pthread_mutex_lock(&s->reconnect_mutex);
			if (!os_atomic_set_bool(&s->reconnecting, true))
				FF_BLOG(LOG_WARNING, "Disconnected. "
						     "Reconnecting...");
			if (s->reconnect_thread_valid) {
				os_event_signal(s->reconnect_stop_event);
				pthread_join(s->reconnect_thread, NULL);
				s->reconnect_thread_valid = false;
				os_event_reset(s->reconnect_stop_event);
			}
			if (pthread_create(&s->reconnect_thread, NULL, ffmpeg_source_reconnect, s) != 0) {
				FF_BLOG(LOG_WARNING, "Could not create "
						     "reconnect thread");
				pthread_mutex_unlock(&s->reconnect_mutex);
				return;
			}
			s->reconnect_thread_valid = true;
			pthread_mutex_unlock(&s->reconnect_mutex);
		}
	}
}

#define RIST_PROTO "rist"

static void ffmpeg_source_update(void *data, obs_data_t *settings)
{
	struct ffmpeg_source *s = data;

	bool active = obs_source_active(s->source);
	bool is_local_file = obs_data_get_bool(settings, "is_local_file");
	bool is_stinger = obs_data_get_bool(settings, "is_stinger");
	bool is_track_matte = obs_data_get_bool(settings, "is_track_matte");
	bool should_restart_media = (is_local_file != s->is_local_file) || (is_stinger != s->is_stinger);

	const char *input;
	const char *input_format;
	const char *ffmpeg_options;

	bool is_hw_decoding;
	enum video_range_type range;
	bool is_linear_alpha;
	int speed_percent;
	bool is_looping;

	bfree(s->input_format);

	if (is_local_file) {
		input = obs_data_get_string(settings, "local_file");
		input_format = NULL;
		is_looping = obs_data_get_bool(settings, "looping");

		if (s->input && !should_restart_media)
			should_restart_media |= strcmp(s->input, input) != 0;
	} else {
		should_restart_media = true;
		input = obs_data_get_string(settings, "input");
		input_format = obs_data_get_string(settings, "input_format");
		s->reconnect_delay_sec = (int)obs_data_get_int(settings, "reconnect_delay_sec");
		s->reconnect_delay_sec = s->reconnect_delay_sec == 0 ? 10 : s->reconnect_delay_sec;
		is_looping = false;
	}

	stop_reconnect_thread(s);

	is_hw_decoding = obs_data_get_bool(settings, "hw_decode");
	range = obs_data_get_int(settings, "color_range");
	speed_percent = (int)obs_data_get_int(settings, "speed_percent");
	if (speed_percent < 1 || speed_percent > 200)
		speed_percent = 100;
	ffmpeg_options = obs_data_get_string(settings, "ffmpeg_options");

	/* Restart media source if these properties are changed */
	if (s->is_hw_decoding != is_hw_decoding || s->range != range || s->speed_percent != speed_percent ||
	    (s->ffmpeg_options && strcmp(s->ffmpeg_options, ffmpeg_options) != 0))
		should_restart_media = true;

	/* If media has ended and user enables looping, user expects that it restarts.
	 * Should still check if is_looping was changed, because users may stop them
	 * intentionally, which is why we only check for ENDED and not STOPPED. */
	if (active && s->state == OBS_MEDIA_STATE_ENDED && is_looping == true && s->is_looping == false) {
		should_restart_media = true;
	}

	bfree(s->input);
	bfree(s->ffmpeg_options);

	s->is_looping = is_looping;
	s->close_when_inactive = obs_data_get_bool(settings, "close_when_inactive");
	s->input = input ? bstrdup(input) : NULL;
	s->input_format = input_format ? bstrdup(input_format) : NULL;
	s->is_hw_decoding = is_hw_decoding;
	s->full_decode = obs_data_get_bool(settings, "full_decode");
	s->is_clear_on_media_end = obs_data_get_bool(settings, "clear_on_media_end");
	s->restart_on_activate = !astrcmpi_n(input, RIST_PROTO, sizeof(RIST_PROTO) - 1)
					 ? false
					 : obs_data_get_bool(settings, "restart_on_activate");
	s->range = range;
	is_linear_alpha = obs_data_get_bool(settings, "linear_alpha");
	s->is_linear_alpha = is_linear_alpha;
	s->buffering_mb = (int)obs_data_get_int(settings, "buffering_mb");
	s->speed_percent = speed_percent;
	s->is_local_file = is_local_file;
	s->seekable = obs_data_get_bool(settings, "seekable");
	s->ffmpeg_options = ffmpeg_options ? bstrdup(ffmpeg_options) : NULL;
	s->is_stinger = is_stinger;
	s->is_track_matte = is_track_matte;
	s->log_changes = obs_data_get_bool(settings, "log_changes");

	if (s->speed_percent < 1 || s->speed_percent > 200)
		s->speed_percent = 100;

	if (s->media && should_restart_media) {
		media_playback_destroy(s->media);
		s->media = NULL;
#ifdef __ANDROID__
		destroy_android_gpu_surface(s);
#endif
	}
#ifdef __ANDROID__
	if (should_restart_media) {
		os_atomic_set_long(&s->gpu_surface_disabled, 0);
		os_atomic_set_long(&s->gpu_fallback_pending, 0);
	}
#endif

	/* directly set options if media is playing */
	if (s->media) {
		media_playback_set_looping(s->media, is_looping);
		media_playback_set_is_linear_alpha(s->media, is_linear_alpha);
	}
	if ((!s->close_when_inactive || active) && should_restart_media)
		ffmpeg_source_open(s);

	dump_source_info(s, input, input_format);
	if ((!s->restart_on_activate || active) && should_restart_media)
		ffmpeg_source_start(s);
}

static const char *ffmpeg_source_getname(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("FFmpegSource");
}

static void restart_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	if (!pressed)
		return;

	struct ffmpeg_source *s = data;
	if (obs_source_showing(s->source))
		obs_source_media_restart(s->source);
}

static void restart_proc(void *data, calldata_t *cd)
{
	restart_hotkey(data, 0, NULL, true);
	UNUSED_PARAMETER(cd);
}

static void preload_first_frame_proc(void *data, calldata_t *cd)
{
	struct ffmpeg_source *s = data;
	if (s->is_track_matte)
		obs_source_output_video(s->source, NULL);
	media_playback_preload_frame(s->media);
	UNUSED_PARAMETER(cd);
}

static void get_duration(void *data, calldata_t *cd)
{
	struct ffmpeg_source *s = data;
	int64_t dur = 0;
	if (s->media)
		dur = media_playback_get_duration(s->media);

	calldata_set_int(cd, "duration", dur * 1000);
}

static void get_nb_frames(void *data, calldata_t *cd)
{
	struct ffmpeg_source *s = data;
	int64_t frames = media_playback_get_frames(s->media);
	calldata_set_int(cd, "num_frames", frames);
}

static bool ffmpeg_source_play_hotkey(void *data, obs_hotkey_pair_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	if (!pressed)
		return false;

	struct ffmpeg_source *s = data;

	if (s->state == OBS_MEDIA_STATE_PLAYING || !obs_source_showing(s->source))
		return false;

	obs_source_media_play_pause(s->source, false);
	return true;
}

static bool ffmpeg_source_pause_hotkey(void *data, obs_hotkey_pair_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	if (!pressed)
		return false;

	struct ffmpeg_source *s = data;

	if (s->state != OBS_MEDIA_STATE_PLAYING || !obs_source_showing(s->source))
		return false;

	obs_source_media_play_pause(s->source, true);
	return true;
}

static void ffmpeg_source_stop_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);

	if (!pressed)
		return;

	struct ffmpeg_source *s = data;

	if (obs_source_showing(s->source))
		obs_source_media_stop(s->source);
}

static void *ffmpeg_source_create(obs_data_t *settings, obs_source_t *source)
{
	struct ffmpeg_source *s = bzalloc(sizeof(struct ffmpeg_source));
	s->source = source;

#ifdef __ANDROID__
	if (pthread_mutex_init(&s->gpu_mx, NULL)) {
		FF_BLOG(LOG_ERROR, "Failed to initialize GPU surface mutex");
		bfree(s);
		return NULL;
	}
#endif

	// Manual type since the event can be signalled without an active thread
	if (os_event_init(&s->reconnect_stop_event, OS_EVENT_TYPE_MANUAL)) {
		FF_BLOG(LOG_ERROR, "Failed to initialize reconnect stop event");
#ifdef __ANDROID__
		pthread_mutex_destroy(&s->gpu_mx);
#endif
		bfree(s);
		return NULL;
	}

	if (pthread_mutex_init(&s->reconnect_mutex, NULL)) {
		FF_BLOG(LOG_ERROR, "Failed to initialize reconnect mutex");
		os_event_destroy(s->reconnect_stop_event);
#ifdef __ANDROID__
		pthread_mutex_destroy(&s->gpu_mx);
#endif
		bfree(s);
		return NULL;
	}

	s->hotkey = obs_hotkey_register_source(source, "MediaSource.Restart", obs_module_text("RestartMedia"),
					       restart_hotkey, s);

	s->play_pause_hotkey = obs_hotkey_pair_register_source(s->source, "MediaSource.Play", obs_module_text("Play"),
							       "MediaSource.Pause", obs_module_text("Pause"),
							       ffmpeg_source_play_hotkey, ffmpeg_source_pause_hotkey, s,
							       s);

	s->stop_hotkey = obs_hotkey_register_source(source, "MediaSource.Stop", obs_module_text("Stop"),
						    ffmpeg_source_stop_hotkey, s);

	proc_handler_t *ph = obs_source_get_proc_handler(source);
	proc_handler_add(ph, "void restart()", restart_proc, s);
	proc_handler_add(ph, "void preload_first_frame()", preload_first_frame_proc, s);
	proc_handler_add(ph, "void get_duration(out int duration)", get_duration, s);
	proc_handler_add(ph, "void get_nb_frames(out int num_frames)", get_nb_frames, s);

	ffmpeg_source_update(s, settings);
	return s;
}

static void ffmpeg_source_destroy(void *data)
{
	struct ffmpeg_source *s = data;

	stop_reconnect_thread(s);

	if (s->hotkey)
		obs_hotkey_unregister(s->hotkey);
	if (s->media)
		media_playback_destroy(s->media);
#ifdef __ANDROID__
	destroy_android_gpu_surface(s);
	pthread_mutex_destroy(&s->gpu_mx);
#endif

	pthread_mutex_destroy(&s->reconnect_mutex);
	os_event_destroy(s->reconnect_stop_event);
	bfree(s->input);
	bfree(s->input_format);
	bfree(s->ffmpeg_options);
	bfree(s);
}

static void ffmpeg_source_activate(void *data)
{
	struct ffmpeg_source *s = data;

	if (s->restart_on_activate)
		obs_source_media_restart(s->source);
}

static void ffmpeg_source_deactivate(void *data)
{
	struct ffmpeg_source *s = data;

	if (s->restart_on_activate) {
		if (s->media) {
			media_playback_stop(s->media);

			if (s->is_clear_on_media_end)
				clear_video_output(s);
		}
	}
}

static void ffmpeg_source_play_pause(void *data, bool pause)
{
	struct ffmpeg_source *s = data;

	if (!s->media)
		ffmpeg_source_open(s);

	if (!s->media)
		return;

	media_playback_play_pause(s->media, pause);

	if (pause) {

		set_media_state(s, OBS_MEDIA_STATE_PAUSED);
	} else {

		set_media_state(s, OBS_MEDIA_STATE_PLAYING);
		obs_source_media_started(s->source);
	}
}

static void ffmpeg_source_stop(void *data)
{
	struct ffmpeg_source *s = data;

	if (s->media) {
		media_playback_stop(s->media);
		clear_video_output(s);
		set_media_state(s, OBS_MEDIA_STATE_STOPPED);
	}
}

static void ffmpeg_source_restart(void *data)
{
	struct ffmpeg_source *s = data;

	if (obs_source_showing(s->source))
		ffmpeg_source_start(s);

	set_media_state(s, OBS_MEDIA_STATE_PLAYING);
}

static int64_t ffmpeg_source_get_duration(void *data)
{
	struct ffmpeg_source *s = data;
	int64_t dur = 0;

	if (s->media)
		dur = media_playback_get_duration(s->media) / INT64_C(1000);

	return dur;
}

static int64_t ffmpeg_source_get_time(void *data)
{
	struct ffmpeg_source *s = data;

	return media_playback_get_current_time(s->media);
}

static void ffmpeg_source_set_time(void *data, int64_t ms)
{
	struct ffmpeg_source *s = data;

	if (!s->media)
		return;

	media_playback_seek(s->media, ms);
}

static enum obs_media_state ffmpeg_source_get_state(void *data)
{
	struct ffmpeg_source *s = data;

	return s->state;
}

static void missing_file_callback(void *src, const char *new_path, void *data)
{
	struct ffmpeg_source *s = src;

	obs_source_t *source = s->source;
	obs_data_t *settings = obs_source_get_settings(source);
	obs_data_set_string(settings, "local_file", new_path);
	obs_source_update(source, settings);
	obs_data_release(settings);

	UNUSED_PARAMETER(data);
}

static obs_missing_files_t *ffmpeg_source_missingfiles(void *data)
{
	struct ffmpeg_source *s = data;
	obs_missing_files_t *files = obs_missing_files_create();

	if (s->is_local_file && s->input && *s->input) {
#ifdef __ANDROID__
		/* A document URI is not a filesystem path; stat cannot validate it. */
		if (strncmp(s->input, "content://", 10) == 0)
			return files;
#endif
		if (!os_file_exists(s->input)) {
			obs_missing_file_t *file = obs_missing_file_create(s->input, missing_file_callback,
									   OBS_MISSING_FILE_SOURCE, s->source, NULL);

			obs_missing_files_add_file(files, file);
		}
	}

	return files;
}

#ifdef __ANDROID__
static uint32_t ffmpeg_source_get_width(void *data)
{
	struct ffmpeg_source *s = data;
	if (!os_atomic_load_long(&s->gpu_active))
		return (uint32_t)os_atomic_load_long(&s->cpu_width);

	pthread_mutex_lock(&s->gpu_mx);
	uint32_t w = s->gpu_cleared ? 0 : (s->gpu_tex_width ? s->gpu_tex_width : s->gpu_width);
	pthread_mutex_unlock(&s->gpu_mx);
	return w;
}

static uint32_t ffmpeg_source_get_height(void *data)
{
	struct ffmpeg_source *s = data;
	if (!os_atomic_load_long(&s->gpu_active))
		return (uint32_t)os_atomic_load_long(&s->cpu_height);

	pthread_mutex_lock(&s->gpu_mx);
	uint32_t h = s->gpu_cleared ? 0 : (s->gpu_tex_height ? s->gpu_tex_height : s->gpu_height);
	pthread_mutex_unlock(&s->gpu_mx);
	return h;
}

static void ffmpeg_source_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	struct ffmpeg_source *s = data;
	if (!os_atomic_load_long(&s->gpu_active)) {
		obs_source_draw_async_frame(s->source);
		return;
	}

	pthread_mutex_lock(&s->gpu_mx);
	if (s->gpu_cleared) {
		if (s->gpu_acquired_image) {
			AImage_delete(s->gpu_acquired_image);
			s->gpu_acquired_image = NULL;
		}
		s->gpu_image = NULL;
		pthread_mutex_unlock(&s->gpu_mx);
		return;
	}

	AImageReader *reader = s->gpu_reader;
	AImage *new_img = NULL;
	bool gpu_failed = false;
	if (reader) {
		const media_status_t acq = AImageReader_acquireLatestImage(reader, &new_img);
		if (acq != AMEDIA_OK) {
			new_img = NULL;
			if (acq != AMEDIA_IMGREADER_NO_BUFFER_AVAILABLE) {
				FF_BLOG(LOG_WARNING, "AImageReader_acquireLatestImage failed (%d)", (int)acq);
				gpu_failed = true;
			}
		}
	}

	if (new_img) {
		AHardwareBuffer *hb = NULL;
		int32_t iw = 0, ih = 0;
		AImageCropRect crop = {0};
		bool ok = AImage_getHardwareBuffer(new_img, &hb) == AMEDIA_OK && hb != NULL &&
			  AImage_getWidth(new_img, &iw) == AMEDIA_OK && AImage_getHeight(new_img, &ih) == AMEDIA_OK &&
			  iw > 0 && ih > 0;
		if (ok) {
			if (AImage_getCropRect(new_img, &crop) != AMEDIA_OK || crop.right <= crop.left ||
			    crop.bottom <= crop.top || crop.left < 0 || crop.top < 0 || crop.right > iw ||
			    crop.bottom > ih) {
				crop.left = 0;
				crop.top = 0;
				crop.right = s->gpu_width > 0 && (int32_t)s->gpu_width <= iw ? (int32_t)s->gpu_width
											     : iw;
				crop.bottom = s->gpu_height > 0 && (int32_t)s->gpu_height <= ih
						      ? (int32_t)s->gpu_height
						      : ih;
			}
			const uint32_t vis_w = (uint32_t)(crop.right - crop.left);
			const uint32_t vis_h = (uint32_t)(crop.bottom - crop.top);
			if (s->gpu_external && (s->gpu_tex_width != vis_w || s->gpu_tex_height != vis_h)) {
				gs_android_external_texture_destroy(s->gpu_external);
				s->gpu_external = NULL;
				s->gpu_image = NULL;
			}
			if (!s->gpu_external) {
				s->gpu_external = gs_android_external_texture_create(vis_w, vis_h);
				s->gpu_tex_width = s->gpu_external ? vis_w : 0;
				s->gpu_tex_height = s->gpu_external ? vis_h : 0;
			}
			if (s->gpu_external && gs_android_external_texture_set_buffer(s->gpu_external, hb)) {
				const float u0 = (float)crop.left / (float)iw;
				const float du = (float)vis_w / (float)iw;
				const float v0 = (float)crop.top / (float)ih;
				const float dv = (float)vis_h / (float)ih;
				float matrix[16] = {
					du, 0.0f, 0.0f, 0.0f,
					0.0f, -dv, 0.0f, 0.0f,
					0.0f, 0.0f, 1.0f, 0.0f,
					u0, v0 + dv, 0.0f, 1.0f,
				};
				gs_texture_t *rendered = gs_android_external_texture_render(s->gpu_external, matrix);
				if (rendered) {
					s->gpu_image = rendered;
					if (s->gpu_acquired_image)
						AImage_delete(s->gpu_acquired_image);
					s->gpu_acquired_image = new_img;
					new_img = NULL;
				} else {
					gpu_failed = true;
				}
			} else {
				gpu_failed = true;
			}
		} else {
			gpu_failed = true;
		}
		if (new_img)
			AImage_delete(new_img);
	}

	if (gpu_failed) {
		s->gpu_image = NULL;
		pthread_mutex_unlock(&s->gpu_mx);
		os_atomic_set_long(&s->gpu_surface_disabled, 1);
		os_atomic_set_long(&s->gpu_active, 0);
		os_atomic_set_long(&s->gpu_fallback_pending, 1);
		return;
	}

	gs_texture_t *tex = s->gpu_image;
	uint32_t draw_w = s->gpu_tex_width;
	uint32_t draw_h = s->gpu_tex_height;
	pthread_mutex_unlock(&s->gpu_mx);

	if (tex && draw_w && draw_h) {
		const bool prev_linear = gs_set_linear_srgb(false);
		gs_effect_t *draw = obs_get_base_effect(OBS_EFFECT_DEFAULT);
		while (gs_effect_loop(draw, "Draw"))
			obs_source_draw(tex, 0, 0, draw_w, draw_h, false);
		gs_set_linear_srgb(prev_linear);
	}
}
#endif

struct obs_source_info ffmpeg_source = {
	.id = "ffmpeg_source",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE |
			OBS_SOURCE_CONTROLLABLE_MEDIA
#ifdef __ANDROID__
			| OBS_SOURCE_CUSTOM_DRAW
#endif
			,
	.get_name = ffmpeg_source_getname,
	.create = ffmpeg_source_create,
	.destroy = ffmpeg_source_destroy,
	.get_defaults = ffmpeg_source_defaults,
	.get_properties = ffmpeg_source_getproperties,
	.activate = ffmpeg_source_activate,
	.deactivate = ffmpeg_source_deactivate,
	.video_tick = ffmpeg_source_tick,
#ifdef __ANDROID__
	.video_render = ffmpeg_source_render,
	.get_width = ffmpeg_source_get_width,
	.get_height = ffmpeg_source_get_height,
#endif
	.missing_files = ffmpeg_source_missingfiles,
	.update = ffmpeg_source_update,
	.icon_type = OBS_ICON_TYPE_MEDIA,
	.media_play_pause = ffmpeg_source_play_pause,
	.media_restart = ffmpeg_source_restart,
	.media_stop = ffmpeg_source_stop,
	.media_get_duration = ffmpeg_source_get_duration,
	.media_get_time = ffmpeg_source_get_time,
	.media_set_time = ffmpeg_source_set_time,
	.media_get_state = ffmpeg_source_get_state,
};
