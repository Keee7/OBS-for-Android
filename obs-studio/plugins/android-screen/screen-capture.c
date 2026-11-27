/* SPDX-License-Identifier: GPL-2.0-or-later */
// P-18 屏幕采集源（MediaProjection + GPU SurfaceTexture / CPU AImageReader）。
//
// 与相机那一路最根本的不同：
//   1) 不套相机的 capture_allowed：OBS 退到后台继续录别的应用是屏幕采集的核心场景，
//      只盯两件事：令牌还在不在、屏幕尺寸有没有变化。
//   2) 没有"长时间没帧就算故障"看门狗：屏幕静止时 VirtualDisplay 可能低至 ~1fps，
//      只检查"面建好却迟迟见不到第一帧"（FIRST_FRAME_NS）。
//   3) 系统音频内录通过 screen-audio.c 由 Java AudioRecord 线程直推 PCM，不再起 C 轮询线程。

#include <media/NdkImage.h>
#include <media/NdkImageReader.h>

#include <pthread.h>
#include <stdatomic.h>

#include <stdio.h>
#include <string.h>

#include <obs-module.h>
#include <util/dstr.h>
#include <util/platform.h>
#include <util/threading.h>

#include <aimage-source.h>
#include <android-locale.h>
#include <graphics/graphics.h>
#include <obs-android.h>

#include "capture-alpha.h"
#include "capture-size.h"
#include "gpu-fallback.h"
#include "screen-audio.h"

#define MAX_IMAGES 4
#define RETRY_INTERVAL_NS (1000ULL * 1000000ULL)
#define LIVE_CHECK_INTERVAL_NS (250ULL * 1000000ULL)
#define FIRST_FRAME_NS (3000ULL * 1000000ULL)

/* 全设备一份投影令牌；最后一颗源销毁时交还给系统。 */
static volatile long live_sources = 0;

enum screen_capture_state {
	SCREEN_STATE_IDLE = 0,
	SCREEN_STATE_CPU,
	SCREEN_STATE_GPU_PENDING,
	SCREEN_STATE_GPU_ACTIVE,
};

struct screen_source {
	obs_source_t *source;
	pthread_mutex_t lifecycle_mx; /* never acquired by video_render */
	pthread_mutex_t status_mx;

	/* GPU 采集字段及 state 均由 OBS 图形上下文串行访问；非渲染线程碰之前需 obs_enter_graphics()。 */
	enum screen_capture_state state;
	volatile long gpu_runtime_failed;
	bool audio_attempted;

	struct android_image_source img;
	_Atomic uint32_t width;
	_Atomic uint32_t height;

	volatile long native_resolution;
	volatile long gpu_requested;
	volatile long alpha_mode;
	bool gpu_applied;
	_Atomic bool gpu_received;
	_Atomic bool gpu_failed;
	uint32_t gpu_dpi;
	gs_android_external_texture_t *gpu_external;
	gs_texture_t *gpu_image;
	void *gpu_host;

	uint64_t next_try_ns;
	bool consent_asked;
	volatile long manual_consent_requested;
	bool display_started;

	uint64_t opened_ns;
	uint64_t frames_at_open;
	bool no_frame_warned;

	_Atomic uint64_t frames_out;
	_Atomic uint64_t frames_dropped;
	_Atomic uint64_t gpu_frames;
	_Atomic uint32_t gpu_retries;
	char last_error[1024];
};

static void set_error_key(struct screen_source *c, const char *key, const char *const *args, size_t nargs)
{
	struct dstr msg;
	obs_android_format_locale(&msg, obs_module_text(key), args, nargs);
	pthread_mutex_lock(&c->status_mx);
	obs_android_copy_utf8(c->last_error, sizeof(c->last_error), msg.array ? msg.array : "");
	dstr_free(&msg);
	blog(LOG_WARNING, "android-screen: %s", c->last_error);
	pthread_mutex_unlock(&c->status_mx);
}

static void drop_frame(struct screen_source *c, const char *why, int32_t a, int32_t b)
{
	c->frames_dropped++;
	if (c->frames_dropped == 1 || (c->frames_dropped % 300) == 0)
		blog(LOG_WARNING, "android-screen: 丢帧（%s，参数=%d,%d，累计丢弃=%llu 帧）", why, (int)a, (int)b,
		     (unsigned long long)c->frames_dropped);
}

static void push_frame(struct screen_source *c, obs_source_t *src, AImage *image)
{
	struct obs_source_frame out = {0};
	int32_t fw = 0, fh = 0, planes = 0, row = 0, px = 0, len = 0;
	uint8_t *buf = NULL;
	AImageCropRect crop = {0};

	const bool meta_ok = (AImage_getWidth(image, &fw) == AMEDIA_OK) &&
			     (AImage_getHeight(image, &fh) == AMEDIA_OK) &&
			     (AImage_getNumberOfPlanes(image, &planes) == AMEDIA_OK) &&
			     (AImage_getCropRect(image, &crop) == AMEDIA_OK) &&
			     (AImage_getPlaneData(image, 0, &buf, &len) == AMEDIA_OK) &&
			     (AImage_getPlaneRowStride(image, 0, &row) == AMEDIA_OK) &&
			     (AImage_getPlanePixelStride(image, 0, &px) == AMEDIA_OK);
	if (!meta_ok || planes != 1 || fw <= 0 || fh <= 0 || row <= 0) {
		drop_frame(c, "读 AImage 元数据失败或平面数不是 1", fw, planes);
		return;
	}
	if (px != 4) {
		drop_frame(c, "RGBA 平面的 pixelStride 不是 4", px, row);
		return;
	}

	int32_t l = crop.left, t = crop.top;
	int32_t cw = crop.right - l, ch = crop.bottom - t;
	if (l < 0) {
		cw += l;
		l = 0;
	}
	if (t < 0) {
		ch += t;
		t = 0;
	}
	if (crop.right > fw)
		cw -= crop.right - fw;
	if (crop.bottom > fh)
		ch -= crop.bottom - fh;
	if (cw <= 0 || ch <= 0) {
		drop_frame(c, "裁剪矩形不成立", (int)crop.right, (int)crop.bottom);
		return;
	}

	const int32_t need = t * row + l * 4 + (ch - 1) * row + cw * 4;
	if (need > len) {
		drop_frame(c, "按裁剪后平面不够读", need, len);
		return;
	}

	out.format = VIDEO_FORMAT_RGBA;
	out.width = (uint32_t)cw;
	out.height = (uint32_t)ch;
	out.data[0] = buf + t * row + l * 4;
	out.linesize[0] = (uint32_t)row;
	out.timestamp = os_gettime_ns();
	out.full_range = true;

	if (c->frames_out == c->frames_at_open || (c->frames_out % 300) == 0) {
		uint8_t amin = 255, amax = 0;
		for (int32_t y = 0; y < ch; y += 16) {
			const uint8_t *rp = out.data[0] + (size_t)y * row;
			for (int32_t x = 0; x < cw; x += 16) {
				const uint8_t a = rp[x * 4 + 3];
				if (a < amin)
					amin = a;
				if (a > amax)
					amax = a;
			}
		}
		blog(LOG_INFO,
		     "android-screen: 第 %llu 帧 出图 %dx%d 行距=%d 裁剪=(%d,%d,%d,%d) 源给 %dx%d alpha=[%u,%u]",
		     (unsigned long long)(c->frames_out + 1), (int)cw, (int)ch, row, (int)crop.left, (int)crop.top,
		     (int)crop.right, (int)crop.bottom, fw, fh, (unsigned)amin, (unsigned)amax);
	}

	uint8_t *straight = NULL;
	if (os_atomic_load_long(&c->alpha_mode) == SCREEN_ALPHA_PREMULTIPLIED) {
		const size_t stride = (size_t)cw * 4;
		if ((size_t)ch > SIZE_MAX / stride || stride > UINT32_MAX) {
			drop_frame(c, "透明帧尺寸过大", cw, ch);
			return;
		}
		straight = bmalloc(stride * (size_t)ch);
		for (int32_t y = 0; y < ch; ++y)
			screen_alpha_unpremultiply(straight + (size_t)y * stride,
						   out.data[0] + (size_t)y * row, (size_t)cw);
		out.data[0] = straight;
		out.linesize[0] = (uint32_t)stride;
	}
	obs_source_output_video(src, &out);
	bfree(straight);

	c->frames_out++;
	if (c->frames_out == 1)
		obs_source_update_properties(src);
}

static void screen_on_frame(void *owner, obs_source_t *src, AImage *image)
{
	push_frame((struct screen_source *)owner, src, image);
}

static void screen_on_acquire_error(void *owner, media_status_t st)
{
	drop_frame((struct screen_source *)owner, "acquireLatestImage 没拿到帧", (int32_t)st, 0);
}

static void close_capture(struct screen_source *c, const char *why)
{
	blog(LOG_INFO, "android-screen: 停止屏幕采集（原因=%s）", why);

	/* 图形锁仅覆盖 GPU 句柄与 state 重置，绝不涵盖音频停止或 AImageReader 回调等待。 */
	obs_enter_graphics();
	if (c->gpu_host) {
		obs_android_screen_gpu_destroy(c->gpu_host);
		c->gpu_host = NULL;
	}
	c->gpu_image = NULL;
	if (c->gpu_external) {
		gs_android_external_texture_destroy(c->gpu_external);
		c->gpu_external = NULL;
	}
	c->gpu_received = false;
	c->state = SCREEN_STATE_IDLE;
	obs_leave_graphics();

	if (c->audio_attempted) {
		screen_audio_release(c->source);
		c->audio_attempted = false;
	}

	if (c->display_started) {
		obs_android_screen_stop_display();
		c->display_started = false;
	}

	android_image_source_close(&c->img, "android-screen", 2000);

	c->width = 0;
	c->height = 0;
}

static bool capture_size(struct screen_source *c, uint32_t *w, uint32_t *h, uint32_t *dpi)
{
	if (!obs_android_screen_display_size(w, h, dpi))
		return false;
	screen_limit_size(w, h, os_atomic_load_long(&c->native_resolution) != 0);
	return true;
}

static bool open_capture(struct screen_source *c)
{
	uint32_t w = 0, h = 0, dpi = 0;

	c->opened_ns = os_gettime_ns();
	c->frames_at_open = c->frames_out;
	c->no_frame_warned = false;

	if (!android_image_source_is_idle(&c->img)) {
		set_error_key(c, "Err.CallbackBusy", NULL, 0);
		return false;
	}

	if (!capture_size(c, &w, &h, &dpi)) {
		set_error_key(c, "Err.ScreenSize", NULL, 0);
		return false;
	}

	if (c->gpu_applied) {
		obs_enter_graphics();
		c->width = w;
		c->height = h;
		c->gpu_dpi = dpi;
		c->gpu_received = false;
		c->gpu_frames = 0;
		c->state = SCREEN_STATE_GPU_PENDING;
		obs_leave_graphics();
		obs_source_output_video(c->source, NULL);
		blog(LOG_INFO, "android-screen: 等待在图形线程启动实验性 GPU 采集 %ux%u", w, h);
		return true;
	}

	const enum android_image_open_err ierr = android_image_source_open(
		&c->img, (int32_t)w, (int32_t)h, AIMAGE_FORMAT_RGBA_8888, MAX_IMAGES, "android-screen");
	if (ierr != ANDROID_IMAGE_OK) {
		if (ierr == ANDROID_IMAGE_ERR_BUSY) {
			set_error_key(c, "Err.CallbackBusy", NULL, 0);
		} else if (ierr == ANDROID_IMAGE_ERR_READER) {
			char width[16], height[16], status[16];
			snprintf(width, sizeof(width), "%u", w);
			snprintf(height, sizeof(height), "%u", h);
			snprintf(status, sizeof(status), "%d", (int)c->img.last_status);
			const char *args[] = {width, height, status};
			set_error_key(c, "Err.ImageReader", args, 3);
		} else if (ierr == ANDROID_IMAGE_ERR_CTX) {
			set_error_key(c, "Err.CallbackAlloc", NULL, 0);
		} else if (ierr == ANDROID_IMAGE_ERR_LISTENER) {
			set_error_key(c, "Err.ImageListener", NULL, 0);
		} else {
			set_error_key(c, "Err.NoWindow", NULL, 0);
		}
		return false;
	}

	if (!obs_android_screen_start_display(c->img.window)) {
		char reason[768];
		obs_android_screen_capture_error(reason, sizeof(reason));
		char width[16], height[16];
		snprintf(width, sizeof(width), "%u", w);
		snprintf(height, sizeof(height), "%u", h);
		const char *args[] = {width, height, reason[0] ? reason : obs_module_text("Err.VirtualDisplayNoReason")};
		set_error_key(c, "Err.VirtualDisplay", args, 3);
		close_capture(c, "投影面没建成");
		return false;
	}
	c->display_started = true;
	c->width = w;
	c->height = h;

	obs_enter_graphics();
	c->state = SCREEN_STATE_CPU;
	obs_leave_graphics();

	screen_audio_acquire(c->source);
	c->audio_attempted = true;

	blog(LOG_INFO, "android-screen: 屏幕采集已启动（目标尺寸=%ux%u），等待首帧数据", w, h);
	return true;
}

static int request_screen_consent(struct screen_source *c)
{
	const int result = obs_android_screen_request_consent();
	if (c && result < 0) {
		char code[16];
		snprintf(code, sizeof(code), "%d", result);
		const char *args[] = {code};
		set_error_key(c, "Err.ConsentFailed", args, 1);
	}
	return result;
}

static void try_start(struct screen_source *c)
{
	if (!obs_source_active(c->source))
		return;

	obs_enter_graphics();
	const enum screen_capture_state st = c->state;
	obs_leave_graphics();
	if (st != SCREEN_STATE_IDLE)
		return;

	if (os_atomic_load_long(&c->img.shutting_down)) {
		if (!android_image_source_is_idle(&c->img))
			return;
		os_atomic_set_long(&c->img.shutting_down, 0);
	}

	if (!obs_android_screen_ready()) {
		set_error_key(c, "Err.BusNotReady", NULL, 0);
		return;
	}

	if (!obs_android_screen_has_consent()) {
		/* Each activation asks once. After a cancellation or revocation,
		 * the properties button can open the consent dialog again. */
		if (!c->consent_asked) {
			c->consent_asked = true;
			const int r = request_screen_consent(c);
			if (r > 0)
				blog(LOG_INFO, "android-screen: 尚未获取投影授权，已发起系统授权请求（返回值=%d）", r);
		}
		return;
	}

	if (!open_capture(c)) {
		// A failed display may invalidate consent. Wait for another request.
		c->consent_asked = true;
		return;
	}

	c->consent_asked = false;
}

static void check_live(struct screen_source *c)
{
	if (!obs_android_screen_has_consent()) {
		close_capture(c, "令牌已不在（用户停止共享，或系统收回）");
		c->consent_asked = true;
		return;
	}

	uint32_t w = 0, h = 0, dpi = 0;
	if (!capture_size(c, &w, &h, &dpi))
		return;
	if (w == c->width && h == c->height)
		return;

	blog(LOG_INFO, "android-screen: 屏幕尺寸变了 %ux%u → %ux%u，重建投影面", c->width, c->height, w, h);
	close_capture(c, "屏幕尺寸变化");
	try_start(c);
}

static const char *screen_getname(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("ScreenCapture");
}

static void *screen_create(obs_data_t *settings, obs_source_t *source)
{
	struct screen_source *c = bzalloc(sizeof(*c));
	if (pthread_mutex_init(&c->lifecycle_mx, NULL) != 0) {
		bfree(c);
		return NULL;
	}
	if (pthread_mutex_init(&c->status_mx, NULL) != 0) {
		pthread_mutex_destroy(&c->lifecycle_mx);
		bfree(c);
		return NULL;
	}

	c->source = source;
	android_image_source_init(&c->img, source, c, screen_on_frame, screen_on_acquire_error);
	os_atomic_inc_long(&live_sources);
	os_atomic_set_long(&c->native_resolution, obs_data_get_bool(settings, "native_resolution"));
	long alpha = (long)obs_data_get_int(settings, "alpha_mode");
	os_atomic_set_long(&c->alpha_mode,
			   alpha >= SCREEN_ALPHA_COMPAT && alpha <= SCREEN_ALPHA_PREMULTIPLIED ? alpha
													       : SCREEN_ALPHA_COMPAT);
	c->gpu_applied = obs_data_get_bool(settings, "gpu_capture");
	os_atomic_set_long(&c->gpu_requested, c->gpu_applied);
	return c;
}

static void screen_update(void *data, obs_data_t *settings)
{
	struct screen_source *c = data;
	os_atomic_set_long(&c->native_resolution, obs_data_get_bool(settings, "native_resolution"));
	os_atomic_set_long(&c->gpu_requested, obs_data_get_bool(settings, "gpu_capture"));
	long alpha = (long)obs_data_get_int(settings, "alpha_mode");
	os_atomic_set_long(&c->alpha_mode,
			   alpha >= SCREEN_ALPHA_COMPAT && alpha <= SCREEN_ALPHA_PREMULTIPLIED ? alpha
													       : SCREEN_ALPHA_COMPAT);
}

static void screen_destroy(void *data)
{
	struct screen_source *c = data;

	pthread_mutex_lock(&c->lifecycle_mx);
	close_capture(c, "源销毁");
	pthread_mutex_unlock(&c->lifecycle_mx);

	if (os_atomic_dec_long(&live_sources) == 0) {
		blog(LOG_INFO, "android-screen: 最后一个屏幕采集源已销毁，正在释放投影授权");
		obs_android_screen_release_consent();
	}

	if (!android_image_source_destroy(&c->img)) {
		blog(LOG_ERROR, "android-screen: 源 '%s' 的回调线程仍在场 —— 这次不释放 data（故意泄漏）",
		     obs_source_get_name(c->source));
		return;
	}

	pthread_mutex_destroy(&c->status_mx);
	pthread_mutex_destroy(&c->lifecycle_mx);
	bfree(c);
}

static void screen_activate(void *data)
{
	struct screen_source *c = data;

	pthread_mutex_lock(&c->lifecycle_mx);
	c->consent_asked = false;
	c->gpu_failed = false;
	os_atomic_set_long(&c->gpu_runtime_failed, 0);
	try_start(c);
	pthread_mutex_unlock(&c->lifecycle_mx);
}

static void screen_deactivate(void *data)
{
	struct screen_source *c = data;

	pthread_mutex_lock(&c->lifecycle_mx);
	close_capture(c, "源停用");
	pthread_mutex_unlock(&c->lifecycle_mx);
}

static void screen_video_tick(void *data, float seconds)
{
	struct screen_source *c = data;
	const uint64_t now = os_gettime_ns();

	UNUSED_PARAMETER(seconds);
	// OBS ticks inactive sources too. Do not immediately reopen after deactivate.
	if (!obs_source_active(c->source))
		return;

	pthread_mutex_lock(&c->lifecycle_mx);
	if (os_atomic_exchange_long(&c->manual_consent_requested, 0)) {
		close_capture(c, "new screen capture consent requested");
		os_atomic_set_long(&c->gpu_runtime_failed, 0);
		c->gpu_retries = 0;
		c->consent_asked = true;
		c->next_try_ns = 0;
	}
	bool requested = os_atomic_load_long(&c->gpu_requested) != 0;
	if (requested != c->gpu_applied) {
		close_capture(c, "GPU 采集设置变化");
		c->gpu_applied = requested;
		c->gpu_failed = false;
		os_atomic_set_long(&c->gpu_runtime_failed, 0);
		c->next_try_ns = 0;
	}
	if (now < c->next_try_ns) {
		pthread_mutex_unlock(&c->lifecycle_mx);
		return;
	}
	c->next_try_ns = now + RETRY_INTERVAL_NS;

	obs_enter_graphics();
	const enum screen_capture_state st = c->state;
	const bool gpu_received = c->gpu_received;
	const uint64_t gpu_opened = c->opened_ns;
	obs_leave_graphics();

	if (st == SCREEN_STATE_GPU_ACTIVE && !c->audio_attempted) {
		screen_audio_acquire(c->source);
		c->audio_attempted = true;
	}

	if (screen_gpu_should_fallback(os_atomic_load_long(&c->gpu_runtime_failed) != 0,
				       st == SCREEN_STATE_GPU_ACTIVE, gpu_received, gpu_opened, now)) {
		c->gpu_failed = true;
		os_atomic_set_long(&c->gpu_runtime_failed, 0);
		uint32_t delay = screen_gpu_retry_seconds(c->gpu_retries++);
		char sec_str[16];
		snprintf(sec_str, sizeof(sec_str), "%u", delay);
		const char *args[] = {sec_str};
		set_error_key(c, "Err.GpuRetry", args, 1);
		close_capture(c, "GPU 采集失败，等待重试");
		c->next_try_ns = now + (uint64_t)delay * 1000000000ULL;
		if (!obs_android_screen_has_consent())
			c->consent_asked = true;
		pthread_mutex_unlock(&c->lifecycle_mx);
		return;
	}

	if (st != SCREEN_STATE_IDLE) {
		c->next_try_ns = now + LIVE_CHECK_INTERVAL_NS;
		if (st == SCREEN_STATE_CPU && !c->no_frame_warned && c->frames_out == c->frames_at_open &&
		    (now - c->opened_ns) > FIRST_FRAME_NS) {
			c->no_frame_warned = true;
			blog(LOG_WARNING, "android-screen: 投影面建成 %llu ms 了一帧没收到（面 %ux%u，累计已出 %llu 帧）"
					  "—— 建面的每一步都报成功了，那只剩 SurfaceFlinger 没往这块面上画。"
					  "注意这条只管第一帧：屏幕静止时几秒不出帧是正常的（见文件头第 2 条）",
			     (unsigned long long)((now - c->opened_ns) / 1000000ULL), c->width, c->height,
			     (unsigned long long)c->frames_out);
		}
		check_live(c);
		pthread_mutex_unlock(&c->lifecycle_mx);
		return;
	}

	try_start(c);
	pthread_mutex_unlock(&c->lifecycle_mx);
}

/* video_render already holds OBS's graphics context; all capture transitions
 * acquire that context before touching the GPU handles and state. */
static void screen_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	struct screen_source *c = data;
	const long alpha_mode = os_atomic_load_long(&c->alpha_mode);
	/* Preserve the historic non-sRGB-aware rendering for existing sources. */
	const bool prev_linear = gs_get_linear_srgb();
	if (alpha_mode == SCREEN_ALPHA_COMPAT)
		gs_set_linear_srgb(false);
	if (os_atomic_load_long(&c->gpu_runtime_failed))
		goto done;
	if (c->state == SCREEN_STATE_GPU_PENDING && !c->gpu_host) {
		c->gpu_external = gs_android_external_texture_create(c->width, c->height);
		if (c->gpu_external)
			c->gpu_host = obs_android_screen_gpu_create(
				gs_android_external_texture_get_id(c->gpu_external), c->width, c->height, c->gpu_dpi);
		if (!c->gpu_host) {
			os_atomic_set_long(&c->gpu_runtime_failed, 1);
			goto done;
		}
		c->state = SCREEN_STATE_GPU_ACTIVE;
		c->opened_ns = os_gettime_ns();
		blog(LOG_INFO, "android-screen: 实验性 GPU 采集面已连接，等待首帧");
	}
	if (c->state == SCREEN_STATE_GPU_ACTIVE && c->gpu_host) {
		float matrix[16];
		int result = obs_android_screen_gpu_update(c->gpu_host, matrix);
		if (result > 0) {
			c->gpu_image = gs_android_external_texture_render(c->gpu_external, matrix);
			if (c->gpu_image) {
				if (!c->gpu_received)
					blog(LOG_INFO,
					     "android-screen: GPU mode received its first texture frame (no CPU frame upload)");
				c->gpu_received = true;
				c->gpu_retries = 0;
				c->gpu_failed = false;
				c->gpu_frames++;
			} else {
				result = -1;
			}
		}
		if (result < 0) {
			os_atomic_set_long(&c->gpu_runtime_failed, 1);
			goto done;
		}
		if (c->gpu_image) {
			gs_effect_t *draw = obs_get_base_effect(OBS_EFFECT_DEFAULT);
			const enum gs_color_space space = gs_get_color_space();
			bool linear = gs_get_linear_srgb() || (space != GS_CS_SRGB);
			const bool premultiplied = alpha_mode == SCREEN_ALPHA_PREMULTIPLIED;
			const bool straight = alpha_mode == SCREEN_ALPHA_STRAIGHT;
			const bool scrgb = space == GS_CS_709_SCRGB;
			const char *technique = "Draw";
			if (scrgb) {
				/* The projection delivers SDR. Scale its linear RGB to the
				 * configured SDR white level in the HDR composition. */
				gs_set_linear_srgb(true);
				technique = straight ? "DrawNonlinearAlphaMultiply" : "DrawMultiply";
				gs_effect_set_float(gs_effect_get_param_by_name(draw, "multiplier"),
						    obs_get_video_sdr_white_level() / 80.0f);
			}
			if (straight || premultiplied) {
				gs_set_linear_srgb(linear);
				if (!scrgb && linear && straight)
					technique = "DrawNonlinearAlpha";
				gs_blend_state_push();
				gs_blend_function_separate((premultiplied || linear) ? GS_BLEND_ONE : GS_BLEND_SRCALPHA,
							   GS_BLEND_INVSRCALPHA, GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
			}
			while (gs_effect_loop(draw, technique))
				obs_source_draw(c->gpu_image, 0, 0, c->width, c->height, false);
			if (straight || premultiplied)
				gs_blend_state_pop();
		}
	} else if (c->state == SCREEN_STATE_CPU) {
		obs_source_draw_async_frame(c->source);
	}
done:
	gs_set_linear_srgb(prev_linear);
}

static uint32_t screen_width(void *data) { return ((struct screen_source *)data)->width; }
static uint32_t screen_height(void *data) { return ((struct screen_source *)data)->height; }

static void screen_defaults(obs_data_t *settings)
{
	obs_data_set_default_bool(settings, "gpu_capture", SCREEN_GPU_DEFAULT);
	obs_data_set_default_int(settings, "alpha_mode", SCREEN_ALPHA_COMPAT);
}

static bool on_request_screen_consent(obs_properties_t *props, obs_property_t *property, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(property);
	struct screen_source *c = data;
	const int result = request_screen_consent(c);
	if (c && result > 0)
		os_atomic_set_long(&c->manual_consent_requested, 1);
	return result < 0;
}

static obs_properties_t *screen_properties(void *data)
{
	obs_properties_t *props = obs_properties_create();
	obs_properties_add_button2(props, "request_consent", obs_module_text("RequestConsent"),
				   on_request_screen_consent, data);
	struct screen_source *c = data;
	struct dstr state;
	struct dstr audio;
	obs_property_t *resolution = obs_properties_add_list(props, "native_resolution", obs_module_text("Resolution"),
							    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_BOOL);
	obs_property_list_add_bool(resolution, obs_module_text("Resolution.Limit"), false);
	obs_property_list_add_bool(resolution, obs_module_text("Resolution.Native"), true);
	obs_property_t *alpha = obs_properties_add_list(props, "alpha_mode", obs_module_text("AlphaMode"),
						       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(alpha, obs_module_text("AlphaMode.Compatible"), SCREEN_ALPHA_COMPAT);
	obs_property_list_add_int(alpha, obs_module_text("AlphaMode.Straight"), SCREEN_ALPHA_STRAIGHT);
	obs_property_list_add_int(alpha, obs_module_text("AlphaMode.Premultiplied"), SCREEN_ALPHA_PREMULTIPLIED);
	obs_property_set_long_description(alpha, obs_module_text("AlphaMode.Description"));
	obs_properties_add_text(props, "alpha_info", obs_module_text("AlphaMode.Info"), OBS_TEXT_INFO);

	enum screen_capture_state cap_state = SCREEN_STATE_IDLE;
	bool gpu_received = false;
	char error[1024] = {0};
	if (c) {
		obs_enter_graphics();
		cap_state = c->state;
		gpu_received = c->gpu_received;
		obs_leave_graphics();
		pthread_mutex_lock(&c->status_mx);
		snprintf(error, sizeof(error), "%s", c->last_error);
		pthread_mutex_unlock(&c->status_mx);
	}
	if (c && cap_state != SCREEN_STATE_IDLE) {
		char width[16], height[16];
		snprintf(width, sizeof(width), "%u", (unsigned)c->width);
		snprintf(height, sizeof(height), "%u", (unsigned)c->height);
		if (cap_state == SCREEN_STATE_GPU_PENDING || cap_state == SCREEN_STATE_GPU_ACTIVE) {
			char frames[24];
			snprintf(frames, sizeof(frames), "%llu", (unsigned long long)c->gpu_frames);
			const char *status = obs_module_text(gpu_received ? "State.GpuReady" : "State.GpuWaiting");
			const char *args[] = {width, height, status, frames};
			obs_android_format_locale(&state, obs_module_text("State.GpuCapturing"), args, 4);
		} else if (c->gpu_failed) {
			const char *args[] = {width, height};
			obs_android_format_locale(&state, obs_module_text("State.ManualCpu"), args, 2);
		} else {
			char frames[24], dropped[24];
			snprintf(frames, sizeof(frames), "%llu", (unsigned long long)c->frames_out);
			snprintf(dropped, sizeof(dropped), "%llu", (unsigned long long)c->frames_dropped);
			const char *args[] = {width, height, frames, dropped};
			obs_android_format_locale(&state, obs_module_text("State.Capturing"), args, 4);
		}
	} else if (c && error[0]) {
		const char *args[] = {error};
		obs_android_format_locale(&state, obs_module_text("State.Error"), args, 1);
	} else if (c) {
		char frames[24], dropped[24];
		snprintf(frames, sizeof(frames), "%llu", (unsigned long long)c->frames_out);
		snprintf(dropped, sizeof(dropped), "%llu", (unsigned long long)c->frames_dropped);
		const char *args[] = {frames, dropped};
		obs_android_format_locale(&state, obs_module_text("State.Idle"), args, 2);
	} else {
		obs_android_format_locale(&state, obs_module_text("State.BusNotReady"), NULL, 0);
	}

	obs_properties_add_bool(props, "gpu_capture", obs_module_text("GpuCapture"));
	obs_properties_add_text(props, "gpu_info", obs_module_text("GpuCapture.Info"), OBS_TEXT_INFO);
	obs_properties_add_text(props, "cap_state", state.array ? state.array : "", OBS_TEXT_INFO);
	dstr_free(&state);

	screen_audio_format_state(&audio);
	obs_properties_add_text(props, "cap_audio", audio.array ? audio.array : "", OBS_TEXT_INFO);
	dstr_free(&audio);
	return props;
}

struct obs_source_info android_screen_capture_info = {
	.id = "android_screen_capture",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE |
			OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_SRGB,
	.get_name = screen_getname,
	.create = screen_create,
	.update = screen_update,
	.destroy = screen_destroy,
	.activate = screen_activate,
	.deactivate = screen_deactivate,
	.video_tick = screen_video_tick,
	.video_render = screen_render,
	.get_width = screen_width,
	.get_height = screen_height,
	.get_defaults = screen_defaults,
	.get_properties = screen_properties,
};

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("android-screen", "en-US")

bool obs_module_load(void)
{
	obs_register_source(&android_screen_capture_info);

	const uint32_t flags = obs_get_source_output_flags("android_screen_capture");
	if (!flags) {
		blog(LOG_ERROR, "android-screen: 注册源类型 'android_screen_capture' 失败，请查看 libobs 错误日志");
		return false;
	}

	blog(LOG_INFO, "android-screen: 已注册源类型 'android_screen_capture'（flags=0x%x，宿主接口就绪=%d）",
	     (unsigned)flags, (int)obs_android_screen_ready());
	return true;
}
