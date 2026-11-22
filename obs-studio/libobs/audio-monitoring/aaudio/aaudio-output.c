#include "../../media-io/audio-resampler.h"
#include "../../util/platform.h"
#include "../../util/threading.h"
#include "../../obs-internal.h"
#include "../../obs-android.h"

#include <aaudio/AAudio.h>

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define do_log(level, format, ...) \
	blog(level, "[aaudio-monitor: '%s'] " format, \
	     (monitor && monitor->source) ? obs_source_get_name(monitor->source) : "(null)", ##__VA_ARGS__)

#define AAUDIO_MON_DEV_MAX 32
#define AAUDIO_REOPEN_COOLDOWN_NS 500000000ULL

struct audio_monitor {
	obs_source_t *source;
	AAudioStream *stream;
	audio_resampler_t *resampler;
	char device_id[32];

	uint32_t sample_rate;
	uint32_t channels;
	enum audio_format format;
	size_t bytes_per_frame;
	uint64_t last_open_attempt_ns;

	int16_t *conv_buf;
	size_t conv_cap_samples;

	uint8_t *ring_buf;
	size_t ring_cap;
	size_t ring_read;
	size_t ring_write;
	size_t ring_size;
	size_t prebuf_bytes;
	bool prebuffering;

	pthread_mutex_t playback_mutex;
	pthread_mutex_t ring_mutex;
	bool playback_mutex_inited;
	bool ring_mutex_inited;

	_Atomic bool disconnected;
	_Atomic bool callbacks_attached;
	bool ignore;
};

extern bool devices_match(const char *id1, const char *id2);

static enum speaker_layout channels_to_obs_speakers(int32_t channels)
{
	switch (channels) {
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
		return SPEAKERS_UNKNOWN;
	}
}

static enum audio_format aaudio_to_obs_audio_format(aaudio_format_t fmt)
{
	switch (fmt) {
	case AAUDIO_FORMAT_PCM_FLOAT:
		return AUDIO_FORMAT_FLOAT;
	case AAUDIO_FORMAT_PCM_I16:
		return AUDIO_FORMAT_16BIT;
	default:
		return AUDIO_FORMAT_UNKNOWN;
	}
}

static int32_t resolve_monitoring_device_id(struct audio_monitor *monitor, const char *id_str)
{
	if (!id_str || !*id_str || strcmp(id_str, "default") == 0)
		return AAUDIO_UNSPECIFIED;

	errno = 0;
	char *end = NULL;
	long parsed = strtol(id_str, &end, 10);
	if (errno != 0 || end == id_str || *end != '\0' || parsed <= 0 || parsed > INT32_MAX) {
		do_log(LOG_WARNING, "Invalid monitoring device id '%s', using system default", id_str);
		return AAUDIO_UNSPECIFIED;
	}

	struct obs_android_audio_device devs[AAUDIO_MON_DEV_MAX];
	int n = obs_android_audio_enum_output_devices(devs, AAUDIO_MON_DEV_MAX);
	if (n < 0)
		return (int32_t)parsed;

	for (int i = 0; i < n; i++) {
		if (devs[i].id == (int32_t)parsed && devs[i].is_sink)
			return (int32_t)parsed;
	}

	do_log(LOG_WARNING, "Monitoring device id=%ld not currently present (%d outputs), falling back to default",
	       parsed, n);
	return AAUDIO_UNSPECIFIED;
}

static aaudio_data_callback_result_t aaudio_data_cb(AAudioStream *stream, void *user, void *audio, int32_t frames)
{
	UNUSED_PARAMETER(stream);

	struct audio_monitor *monitor = user;
	if (!monitor || !audio || frames <= 0)
		return AAUDIO_CALLBACK_RESULT_CONTINUE;

	const size_t bytes_wanted = (size_t)frames * monitor->bytes_per_frame;
	uint8_t *dst = (uint8_t *)audio;
	size_t copied = 0;

	pthread_mutex_lock(&monitor->ring_mutex);
	if (monitor->ring_buf && monitor->ring_cap > 0) {
		if (monitor->prebuffering) {
			size_t threshold = monitor->prebuf_bytes > bytes_wanted ? monitor->prebuf_bytes : bytes_wanted;
			if (threshold > monitor->ring_cap)
				threshold = monitor->ring_cap;
			if (monitor->ring_size >= threshold)
				monitor->prebuffering = false;
		}
		if (!monitor->prebuffering) {
			size_t avail = monitor->ring_size;
			size_t to_copy = avail < bytes_wanted ? avail : bytes_wanted;
			if (to_copy > 0) {
				size_t first = monitor->ring_cap - monitor->ring_read;
				if (first > to_copy)
					first = to_copy;
				memcpy(dst, monitor->ring_buf + monitor->ring_read, first);
				if (to_copy > first)
					memcpy(dst + first, monitor->ring_buf, to_copy - first);
				monitor->ring_read = (monitor->ring_read + to_copy) % monitor->ring_cap;
				monitor->ring_size -= to_copy;
				copied = to_copy;
			}
			if (copied < bytes_wanted)
				monitor->prebuffering = true;
		}
	}
	pthread_mutex_unlock(&monitor->ring_mutex);

	if (copied < bytes_wanted)
		memset(dst + copied, 0, bytes_wanted - copied);

	return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

static void aaudio_error_cb(AAudioStream *stream, void *user, aaudio_result_t error)
{
	UNUSED_PARAMETER(stream);
	UNUSED_PARAMETER(error);

	struct audio_monitor *monitor = user;
	if (monitor)
		atomic_store_explicit(&monitor->disconnected, true, memory_order_release);
}

static void aaudio_close_playback(struct audio_monitor *monitor)
{
	if (monitor->stream) {
		AAudioStream_requestStop(monitor->stream);
		AAudioStream_close(monitor->stream);
		monitor->stream = NULL;
	}

	audio_resampler_destroy(monitor->resampler);
	monitor->resampler = NULL;

	bfree(monitor->conv_buf);
	monitor->conv_buf = NULL;
	monitor->conv_cap_samples = 0;

	if (monitor->ring_mutex_inited)
		pthread_mutex_lock(&monitor->ring_mutex);
	bfree(monitor->ring_buf);
	monitor->ring_buf = NULL;
	monitor->ring_cap = 0;
	monitor->ring_read = 0;
	monitor->ring_write = 0;
	monitor->ring_size = 0;
	monitor->prebuf_bytes = 0;
	monitor->prebuffering = true;
	if (monitor->ring_mutex_inited)
		pthread_mutex_unlock(&monitor->ring_mutex);
}

static bool aaudio_try_open_for_device(struct audio_monitor *monitor, const struct audio_output_info *info,
				       int32_t dev_id)
{
	AAudioStreamBuilder *builder = NULL;
	aaudio_result_t res = AAudio_createStreamBuilder(&builder);
	if (res != AAUDIO_OK || !builder) {
		do_log(LOG_WARNING, "AAudio_createStreamBuilder failed: %d (%s)", (int)res,
		       AAudio_convertResultToText(res));
		return false;
	}

	AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
	AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
	AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_NONE);
	AAudioStreamBuilder_setUsage(builder, AAUDIO_USAGE_MEDIA);
	AAudioStreamBuilder_setContentType(builder, AAUDIO_CONTENT_TYPE_MUSIC);
	/* Exclude OBS monitoring playback from Android AudioPlaybackCapture screen audio capture. */
	AAudioStreamBuilder_setAllowedCapturePolicy(builder, AAUDIO_ALLOW_CAPTURE_BY_NONE);
	if (dev_id != AAUDIO_UNSPECIFIED)
		AAudioStreamBuilder_setDeviceId(builder, dev_id);
	AAudioStreamBuilder_setDataCallback(builder, aaudio_data_cb, monitor);
	AAudioStreamBuilder_setErrorCallback(builder, aaudio_error_cb, monitor);

	const int32_t want_rate = info->samples_per_sec ? (int32_t)info->samples_per_sec : AAUDIO_UNSPECIFIED;
	uint32_t obs_ch = (uint32_t)get_audio_channels(info->speakers);
	const int32_t want_ch = (obs_ch > 0 && obs_ch <= 2) ? (int32_t)obs_ch : 2;

	const struct {
		aaudio_format_t fmt;
		int32_t rate;
		int32_t ch;
	} attempts[] = {
		{AAUDIO_FORMAT_PCM_FLOAT, want_rate, want_ch},
		{AAUDIO_FORMAT_PCM_FLOAT, AAUDIO_UNSPECIFIED, 0},
		{AAUDIO_FORMAT_PCM_I16, want_rate, want_ch},
		{AAUDIO_FORMAT_PCM_I16, AAUDIO_UNSPECIFIED, 0},
	};
	const size_t attempt_count = sizeof(attempts) / sizeof(attempts[0]);

	AAudioStream *stream = NULL;
	for (size_t i = 0; i < attempt_count; i++) {
		AAudioStreamBuilder_setFormat(builder, attempts[i].fmt);
		AAudioStreamBuilder_setSampleRate(builder, attempts[i].rate);
		AAudioStreamBuilder_setChannelCount(builder, attempts[i].ch);

		res = AAudioStreamBuilder_openStream(builder, &stream);
		if (res == AAUDIO_OK && stream) {
			enum audio_format obs_fmt = aaudio_to_obs_audio_format(AAudioStream_getFormat(stream));
			enum speaker_layout speakers = channels_to_obs_speakers(AAudioStream_getChannelCount(stream));
			int32_t got_rate = AAudioStream_getSampleRate(stream);
			if (obs_fmt != AUDIO_FORMAT_UNKNOWN && speakers != SPEAKERS_UNKNOWN && got_rate > 0)
				break;
			AAudioStream_close(stream);
			stream = NULL;
		}
	}
	AAudioStreamBuilder_delete(builder);

	if (!stream) {
		do_log(LOG_WARNING, "AAudioStreamBuilder_openStream failed for device=%d: %d (%s)", (int)dev_id,
		       (int)res, AAudio_convertResultToText(res));
		return false;
	}

	const int32_t got_rate = AAudioStream_getSampleRate(stream);
	const int32_t got_ch = AAudioStream_getChannelCount(stream);
	const int32_t burst = AAudioStream_getFramesPerBurst(stream);
	const aaudio_format_t got_fmt = AAudioStream_getFormat(stream);
	const enum audio_format obs_fmt = aaudio_to_obs_audio_format(got_fmt);
	const enum speaker_layout speakers = channels_to_obs_speakers(got_ch);
	const size_t sample_bytes = (obs_fmt == AUDIO_FORMAT_FLOAT) ? sizeof(float) : sizeof(int16_t);
	const size_t bpf = (size_t)got_ch * sample_bytes;

	size_t ring_frames = (size_t)got_rate / 4;
	if (ring_frames < 2048)
		ring_frames = 2048;
	if (burst > 0 && ring_frames < (size_t)burst * 4)
		ring_frames = (size_t)burst * 4;
	size_t prebuf_frames = (size_t)got_rate * 30 / 1000;
	if (prebuf_frames < 256)
		prebuf_frames = 256;
	if (prebuf_frames > ring_frames / 2)
		prebuf_frames = ring_frames / 2;

	/* Always resample into float interleaved so user_volume scales cleanly before any int16 quantization. */
	struct resample_info from = {
		.samples_per_sec = info->samples_per_sec,
		.speakers = info->speakers,
		.format = AUDIO_FORMAT_FLOAT_PLANAR,
	};
	struct resample_info to = {
		.samples_per_sec = (uint32_t)got_rate,
		.speakers = speakers,
		.format = AUDIO_FORMAT_FLOAT,
	};

	audio_resampler_t *resampler = audio_resampler_create(&to, &from);
	if (!resampler) {
		do_log(LOG_WARNING, "Failed to create audio resampler (rate=%d ch=%d fmt=%d)", (int)got_rate,
		       (int)got_ch, (int)obs_fmt);
		AAudioStream_close(stream);
		return false;
	}

	const size_t cap = ring_frames * bpf;
	uint8_t *ring_buf = bmalloc(cap);

	monitor->stream = stream;
	monitor->resampler = resampler;
	monitor->sample_rate = (uint32_t)got_rate;
	monitor->channels = (uint32_t)got_ch;
	monitor->format = obs_fmt;
	monitor->bytes_per_frame = bpf;

	pthread_mutex_lock(&monitor->ring_mutex);
	monitor->ring_buf = ring_buf;
	monitor->ring_cap = cap;
	monitor->ring_read = 0;
	monitor->ring_write = 0;
	monitor->ring_size = 0;
	monitor->prebuf_bytes = prebuf_frames * bpf;
	monitor->prebuffering = true;
	pthread_mutex_unlock(&monitor->ring_mutex);

	atomic_store_explicit(&monitor->disconnected, false, memory_order_release);

	res = AAudioStream_requestStart(stream);
	if (res != AAUDIO_OK) {
		do_log(LOG_WARNING, "AAudioStream_requestStart failed: %d (%s)", (int)res,
		       AAudio_convertResultToText(res));
		aaudio_close_playback(monitor);
		return false;
	}

	do_log(LOG_INFO, "Started AAudio monitoring: req_dev=%d actual_dev=%d rate=%d ch=%d fmt=%s", (int)dev_id,
	       (int)AAudioStream_getDeviceId(stream), (int)got_rate, (int)got_ch,
	       (obs_fmt == AUDIO_FORMAT_FLOAT) ? "float32" : "s16");
	return true;
}

static bool aaudio_open_playback(struct audio_monitor *monitor)
{
	const uint64_t now = os_gettime_ns();
	if (monitor->last_open_attempt_ns && now - monitor->last_open_attempt_ns < AAUDIO_REOPEN_COOLDOWN_NS)
		return false;
	monitor->last_open_attempt_ns = now;

	const struct audio_output_info *info = obs->audio.audio ? audio_output_get_info(obs->audio.audio) : NULL;
	if (!info || !info->samples_per_sec) {
		do_log(LOG_WARNING, "Main OBS audio output not initialized");
		return false;
	}

	const int32_t dev_id = resolve_monitoring_device_id(monitor, monitor->device_id);
	if (aaudio_try_open_for_device(monitor, info, dev_id))
		return true;

	if (dev_id != AAUDIO_UNSPECIFIED) {
		do_log(LOG_WARNING, "Retrying AAudio monitoring on system default output");
		return aaudio_try_open_for_device(monitor, info, AAUDIO_UNSPECIFIED);
	}
	return false;
}

static void apply_float_volume(float *buf, size_t samples, float vol)
{
	if (!(vol > 0.0f) || !isfinite(vol)) {
		memset(buf, 0, samples * sizeof(float));
		return;
	}
	if (close_float(vol, 1.0f, EPSILON))
		return;

	for (size_t i = 0; i < samples; i++)
		buf[i] *= vol;
}

static const uint8_t *prepare_pcm_payload(struct audio_monitor *monitor, float *fbuf, uint32_t frames, float vol,
					  size_t *out_bytes)
{
	const size_t samples = (size_t)frames * monitor->channels;
	apply_float_volume(fbuf, samples, vol);

	if (monitor->format == AUDIO_FORMAT_FLOAT) {
		*out_bytes = samples * sizeof(float);
		return (const uint8_t *)fbuf;
	}

	if (samples > monitor->conv_cap_samples) {
		monitor->conv_buf = brealloc(monitor->conv_buf, samples * sizeof(int16_t));
		monitor->conv_cap_samples = samples;
	}

	for (size_t i = 0; i < samples; i++) {
		float s = fbuf[i];
		if (!(s == s))
			s = 0.0f;
		else if (s > 1.0f)
			s = 1.0f;
		else if (s < -1.0f)
			s = -1.0f;
		long v = lroundf(s * 32767.0f);
		if (v > INT16_MAX)
			v = INT16_MAX;
		else if (v < INT16_MIN)
			v = INT16_MIN;
		monitor->conv_buf[i] = (int16_t)v;
	}

	*out_bytes = samples * sizeof(int16_t);
	return (const uint8_t *)monitor->conv_buf;
}

static void ring_push_bytes(struct audio_monitor *monitor, const uint8_t *src, size_t bytes)
{
	pthread_mutex_lock(&monitor->ring_mutex);
	if (monitor->ring_buf && monitor->ring_cap > 0 && bytes > 0) {
		if (bytes >= monitor->ring_cap) {
			src += bytes - monitor->ring_cap;
			bytes = monitor->ring_cap;
			monitor->ring_read = 0;
			monitor->ring_write = 0;
			monitor->ring_size = 0;
		} else if (monitor->ring_size + bytes > monitor->ring_cap) {
			size_t drop = (monitor->ring_size + bytes) - monitor->ring_cap;
			monitor->ring_read = (monitor->ring_read + drop) % monitor->ring_cap;
			monitor->ring_size -= drop;
		}

		size_t first = monitor->ring_cap - monitor->ring_write;
		if (first > bytes)
			first = bytes;
		memcpy(monitor->ring_buf + monitor->ring_write, src, first);
		if (bytes > first)
			memcpy(monitor->ring_buf, src + first, bytes - first);
		monitor->ring_write = (monitor->ring_write + bytes) % monitor->ring_cap;
		monitor->ring_size += bytes;
	}
	pthread_mutex_unlock(&monitor->ring_mutex);
}

static void on_audio_pause(void *data, calldata_t *calldata)
{
	UNUSED_PARAMETER(calldata);

	struct audio_monitor *monitor = data;
	if (!monitor || !monitor->playback_mutex_inited)
		return;

	pthread_mutex_lock(&monitor->playback_mutex);
	if (atomic_load_explicit(&monitor->callbacks_attached, memory_order_acquire) && monitor->ring_mutex_inited) {
		pthread_mutex_lock(&monitor->ring_mutex);
		monitor->ring_read = 0;
		monitor->ring_write = 0;
		monitor->ring_size = 0;
		monitor->prebuffering = true;
		pthread_mutex_unlock(&monitor->ring_mutex);
	}
	pthread_mutex_unlock(&monitor->playback_mutex);
}

static void on_audio_playback(void *param, obs_source_t *source, const struct audio_data *audio_data, bool muted)
{
	UNUSED_PARAMETER(muted);

	struct audio_monitor *monitor = param;
	if (!monitor || !audio_data || audio_data->frames == 0)
		return;

	if (pthread_mutex_trylock(&monitor->playback_mutex) != 0)
		return;

	if (os_atomic_load_long(&source->activate_refs) == 0)
		goto unlock;

	if (atomic_load_explicit(&monitor->disconnected, memory_order_acquire) || !monitor->stream) {
		aaudio_close_playback(monitor);
		if (!aaudio_open_playback(monitor))
			goto unlock;
	}

	if (!monitor->resampler)
		goto unlock;

	uint8_t *resample_data[MAX_AV_PLANES] = {0};
	uint32_t resample_frames = 0;
	uint64_t ts_offset = 0;
	if (!audio_resampler_resample(monitor->resampler, resample_data, &resample_frames, &ts_offset,
				      (const uint8_t *const *)audio_data->data, (uint32_t)audio_data->frames))
		goto unlock;

	if (resample_frames == 0 || !resample_data[0])
		goto unlock;

	size_t bytes = 0;
	const uint8_t *payload =
		prepare_pcm_payload(monitor, (float *)resample_data[0], resample_frames, source->user_volume, &bytes);
	ring_push_bytes(monitor, payload, bytes);

unlock:
	pthread_mutex_unlock(&monitor->playback_mutex);
}

static bool audio_monitor_prepare(struct audio_monitor *monitor, obs_source_t *source)
{
	pthread_mutex_init_value(&monitor->playback_mutex);
	pthread_mutex_init_value(&monitor->ring_mutex);
	monitor->source = source;

	pthread_mutex_lock(&obs->audio.monitoring_mutex);
	const char *id = obs->audio.monitoring_device_id;
	if (id && *id)
		snprintf(monitor->device_id, sizeof(monitor->device_id), "%s", id);
	else
		monitor->device_id[0] = '\0';
	pthread_mutex_unlock(&obs->audio.monitoring_mutex);

	if (!monitor->device_id[0]) {
		do_log(LOG_WARNING, "No monitoring device ID set");
		return false;
	}

	if (source->info.output_flags & OBS_SOURCE_DO_NOT_SELF_MONITOR) {
		obs_data_t *s = obs_source_get_settings(source);
		const char *s_dev_id = obs_data_get_string(s, "device_id");
		bool match = devices_match(s_dev_id, monitor->device_id);
		obs_data_release(s);

		if (match) {
			monitor->ignore = true;
			do_log(LOG_INFO, "Prevented feedback loop for self-monitored device '%s'", s_dev_id);
			return true;
		}
	}

	if (pthread_mutex_init(&monitor->playback_mutex, NULL) != 0) {
		do_log(LOG_WARNING, "Failed to init playback mutex");
		return false;
	}
	monitor->playback_mutex_inited = true;

	if (pthread_mutex_init(&monitor->ring_mutex, NULL) != 0) {
		do_log(LOG_WARNING, "Failed to init ring mutex");
		return false;
	}
	monitor->ring_mutex_inited = true;
	return true;
}

static void audio_monitor_init_final(struct audio_monitor *monitor)
{
	if (monitor->ignore)
		return;

	atomic_store_explicit(&monitor->callbacks_attached, true, memory_order_release);
	obs_source_add_audio_capture_callback(monitor->source, on_audio_playback, monitor);
	obs_source_add_audio_pause_callback(monitor->source, on_audio_pause, monitor);
}

static void audio_monitor_free(struct audio_monitor *monitor)
{
	if (!monitor)
		return;

	if (monitor->source && atomic_exchange_explicit(&monitor->callbacks_attached, false, memory_order_acq_rel)) {
		obs_source_remove_audio_capture_callback(monitor->source, on_audio_playback, monitor);
		obs_source_remove_audio_pause_callback(monitor->source, on_audio_pause, monitor);
	}

	if (monitor->playback_mutex_inited) {
		pthread_mutex_lock(&monitor->playback_mutex);
		aaudio_close_playback(monitor);
		pthread_mutex_unlock(&monitor->playback_mutex);
	} else {
		aaudio_close_playback(monitor);
	}

	if (monitor->ring_mutex_inited) {
		pthread_mutex_destroy(&monitor->ring_mutex);
		monitor->ring_mutex_inited = false;
	}
	if (monitor->playback_mutex_inited) {
		pthread_mutex_destroy(&monitor->playback_mutex);
		monitor->playback_mutex_inited = false;
	}
}

struct audio_monitor *audio_monitor_create(obs_source_t *source)
{
	if (!source)
		return NULL;

	struct audio_monitor *monitor = bzalloc(sizeof(*monitor));
	if (!audio_monitor_prepare(monitor, source))
		goto fail;
	if (!monitor->ignore && !aaudio_open_playback(monitor))
		goto fail;

	pthread_mutex_lock(&obs->audio.monitoring_mutex);
	da_push_back(obs->audio.monitors, &monitor);
	pthread_mutex_unlock(&obs->audio.monitoring_mutex);

	audio_monitor_init_final(monitor);
	return monitor;

fail:
	audio_monitor_free(monitor);
	bfree(monitor);
	return NULL;
}

void audio_monitor_reset(struct audio_monitor *monitor)
{
	if (!monitor)
		return;

	obs_source_t *source = monitor->source;
	audio_monitor_free(monitor);
	memset(monitor, 0, sizeof(*monitor));

	if (source && audio_monitor_prepare(monitor, source)) {
		if (!monitor->ignore && !aaudio_open_playback(monitor)) {
			do_log(LOG_WARNING,
			       "AAudio stream failed during monitor reset; will retry lazily on next audio packet");
		}
		audio_monitor_init_final(monitor);
	} else {
		audio_monitor_free(monitor);
		memset(monitor, 0, sizeof(*monitor));
		monitor->source = source;
	}
}

void audio_monitor_destroy(struct audio_monitor *monitor)
{
	if (!monitor)
		return;

	pthread_mutex_lock(&obs->audio.monitoring_mutex);
	da_erase_item(obs->audio.monitors, &monitor);
	pthread_mutex_unlock(&obs->audio.monitoring_mutex);

	audio_monitor_free(monitor);
	bfree(monitor);
}
