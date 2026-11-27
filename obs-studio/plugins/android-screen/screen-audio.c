/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * 系统内录接收端（AudioPlaybackCapture → Java nativeOnPcm → obs_source_output_audio）。
 *
 * 屏幕源声明了 OBS_SOURCE_DO_NOT_DUPLICATE，且 ObsProjectionHost 同时只允许一个活跃采集源，
 * 因此只需单活跃接收端即可，无需多源扇出表或独立 C 轮询线程。
 */

#include "screen-audio.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include <android-locale.h>
#include <obs-android.h>
#include <util/platform.h>

/* 锁序：lifecycle_mx → audio_mx。回调线程仅短持 audio_mx，永不持 lifecycle_mx。 */
static pthread_mutex_t audio_lifecycle_mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t audio_mx = PTHREAD_MUTEX_INITIALIZER;

static obs_source_t *active_sink = NULL;
static bool audio_active = false;
static uint32_t audio_rate = 0;
static uint64_t audio_pushes = 0;
static uint64_t audio_frames_total = 0;
static uint64_t audio_ts = 0;
static uint32_t audio_ts_rate = 0;
static int audio_peak_win = 0;
static int audio_peak_all = 0;
static bool audio_nonzero_logged = false;

static void screen_audio_on_pcm(const int16_t *pcm, uint32_t frames, uint32_t sample_rate, uint32_t channels,
				void *param)
{
	UNUSED_PARAMETER(param);
	if (!pcm || frames == 0 || sample_rate == 0 || (channels != 1 && channels != 2))
		return;

	int peak = 0;
	const uint32_t samples = frames * channels;
	for (uint32_t i = 0; i < samples; i++) {
		const int v = (pcm[i] < 0) ? -(int)pcm[i] : (int)pcm[i];
		if (v > peak)
			peak = v;
	}

	const uint64_t now = os_gettime_ns();
	const uint64_t dur_ns = (uint64_t)frames * 1000000000ULL / (uint64_t)sample_rate;
	const uint64_t block_start = (now > dur_ns) ? (now - dur_ns) : now;

	pthread_mutex_lock(&audio_mx);
	obs_source_t *target = active_sink ? obs_source_get_ref(active_sink) : NULL;
	if (!target) {
		pthread_mutex_unlock(&audio_mx);
		return;
	}

	/* 若系统静音断流或采集切换导致与墙钟偏差超过 100 ms，则重对齐起点，防音画永久漂移。 */
	if (audio_ts == 0 || audio_ts_rate != sample_rate ||
	    block_start > audio_ts + 100000000ULL || audio_ts > block_start + 100000000ULL) {
		audio_ts = block_start;
		audio_ts_rate = sample_rate;
	}
	const uint64_t ts = audio_ts;
	audio_ts += dur_ns;

	if (peak > audio_peak_win)
		audio_peak_win = peak;
	if (peak > audio_peak_all)
		audio_peak_all = peak;

	const bool first_nonzero = (peak > 0 && !audio_nonzero_logged);
	if (first_nonzero)
		audio_nonzero_logged = true;

	audio_frames_total += frames;
	audio_pushes++;
	const uint64_t pushes = audio_pushes;
	const uint64_t total = audio_frames_total;
	const bool log_now = (pushes == 1 || (pushes % 200) == 0);
	const int win = audio_peak_win;
	const int all = audio_peak_all;
	if (log_now)
		audio_peak_win = 0;
	pthread_mutex_unlock(&audio_mx);

	struct obs_source_audio out = {0};
	out.data[0] = (const uint8_t *)pcm;
	out.frames = frames;
	out.format = AUDIO_FORMAT_16BIT;
	out.speakers = (channels == 1) ? SPEAKERS_MONO : SPEAKERS_STEREO;
	out.samples_per_sec = sample_rate;
	out.timestamp = ts;
	obs_source_output_audio(target, &out);
	obs_source_release(target);

	if (first_nonzero) {
		blog(LOG_INFO, "android-screen: 内录第一次量到非零样本（峰值 %d/32767，第 %llu 发）", peak,
		     (unsigned long long)pushes);
	}
	if (log_now) {
		blog(LOG_INFO,
		     "android-screen: 内录第 %llu 发（本批 %u 帧，累计 %llu 帧 ≈ %.2f 秒；这一窗峰值 %d，本场峰值 %d，满量程 32767）",
		     (unsigned long long)pushes, frames, (unsigned long long)total,
		     (double)total / (double)sample_rate, win, all);
	}
}

void screen_audio_acquire(obs_source_t *source)
{
	obs_source_t *ref = obs_source_get_ref(source);
	if (!ref)
		return;

	pthread_mutex_lock(&audio_lifecycle_mx);
	pthread_mutex_lock(&audio_mx);
	obs_source_t *prev = active_sink;
	active_sink = ref;
	if (!audio_active) {
		audio_ts = 0;
		audio_ts_rate = 0;
	}
	pthread_mutex_unlock(&audio_mx);
	if (prev)
		obs_source_release(prev);

	if (!audio_active) {
		uint32_t rate = 0, ch = 0;
		if (!obs_android_screen_start_audio(screen_audio_on_pcm, NULL, &rate, &ch)) {
			blog(LOG_WARNING,
			     "android-screen: 系统音频内录启动失败（详见宿主接口日志），本次采集将不包含系统声音");
		} else if (ch != 1 && ch != 2) {
			blog(LOG_WARNING, "android-screen: 系统内录返回 %u 声道，当前仅支持单声道或立体声，已跳过内录",
			     (unsigned)ch);
			obs_android_screen_stop_audio();
		} else {
			pthread_mutex_lock(&audio_mx);
			audio_rate = rate;
			audio_active = true;
			pthread_mutex_unlock(&audio_mx);
			blog(LOG_INFO, "android-screen: 系统音频内录已启动（%uHz，%u 声道，Java 直推模式）",
			     (unsigned)rate, (unsigned)ch);
		}
	}
	pthread_mutex_unlock(&audio_lifecycle_mx);
}

void screen_audio_release(obs_source_t *source)
{
	pthread_mutex_lock(&audio_lifecycle_mx);
	pthread_mutex_lock(&audio_mx);
	if (!active_sink || active_sink != source) {
		pthread_mutex_unlock(&audio_mx);
		pthread_mutex_unlock(&audio_lifecycle_mx);
		return;
	}
	obs_source_t *ref = active_sink;
	active_sink = NULL;
	const bool was_active = audio_active;
	const uint64_t pushes = audio_pushes;
	const uint64_t total = audio_frames_total;
	const uint32_t rate = audio_rate;
	const int peak_all = audio_peak_all;
	audio_active = false;
	pthread_mutex_unlock(&audio_mx);

	if (was_active) {
		obs_android_screen_stop_audio();
		blog(LOG_INFO,
		     "android-screen: 系统音频内录已停止（共推送 %llu 次 / %llu 帧，约 %.2f 秒，历史峰值=%d/32767）",
		     (unsigned long long)pushes, (unsigned long long)total,
		     (double)total / (double)(rate ? rate : 1), peak_all);
	}

	pthread_mutex_lock(&audio_mx);
	audio_rate = 0;
	audio_pushes = 0;
	audio_frames_total = 0;
	audio_ts = 0;
	audio_ts_rate = 0;
	audio_peak_win = 0;
	audio_peak_all = 0;
	audio_nonzero_logged = false;
	pthread_mutex_unlock(&audio_mx);

	pthread_mutex_unlock(&audio_lifecycle_mx);
	obs_source_release(ref);
}

void screen_audio_format_state(struct dstr *out)
{
	pthread_mutex_lock(&audio_lifecycle_mx);
	pthread_mutex_lock(&audio_mx);
	const char *status = audio_active ? obs_module_text("AudioState.Running")
					  : obs_module_text("AudioState.Stopped");
	const long users = active_sink ? 1L : 0L;
	char users_buf[24], frames_buf[24], sec_buf[24], peak_buf[16];
	snprintf(users_buf, sizeof(users_buf), "%ld", users);
	snprintf(frames_buf, sizeof(frames_buf), "%llu", (unsigned long long)audio_frames_total);
	snprintf(sec_buf, sizeof(sec_buf), "%.2f", (double)audio_frames_total / (double)(audio_rate ? audio_rate : 1));
	snprintf(peak_buf, sizeof(peak_buf), "%d", audio_peak_all);
	const char *hint = (audio_active && audio_peak_all == 0) ? obs_module_text("AudioState.ZeroPeakHint") : "";
	pthread_mutex_unlock(&audio_mx);
	pthread_mutex_unlock(&audio_lifecycle_mx);
	const char *args[] = {status, users_buf, frames_buf, sec_buf, peak_buf, hint};
	obs_android_format_locale(out, obs_module_text("AudioState.Format"), args, 6);
}
