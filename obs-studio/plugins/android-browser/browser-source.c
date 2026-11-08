/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <obs-module.h>
#include <obs-android.h>
#include <graphics/graphics.h>
#include <util/platform.h>
#include <util/threading.h>
#include <pthread.h>
#include <string.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("android-browser", "en-US")

/* Same three modes the screen-capture source exposes, for the same reason: the
 * virtual-display compositor decides whether it hands us premultiplied or
 * straight alpha. Picking the wrong one does not change whether pixels are
 * transparent, but it does turn every soft edge into a black fringe. */
enum browser_alpha_mode {
	BROWSER_ALPHA_COMPAT = 0,
	BROWSER_ALPHA_STRAIGHT = 1,
	BROWSER_ALPHA_PREMULTIPLIED = 2,
};
MODULE_EXPORT const char *obs_module_description(void)
{
	return "Android WebView browser source";
}

struct browser_source {
	pthread_mutex_t mutex;
	uint32_t width, height, fps;
	char *url, *css;
	bool transparent;
	long alpha_mode;
	bool dirty, reload, probed;
	/* Transparent sources take the ImageReader path; see render(). */
	bool reader_mode, has_image;
	int status; /* 0 starting, 1 receiving frames, 2 retrying */
	uint64_t next_frame, retry_at, started_at;
	gs_android_external_texture_t *external;
	gs_texture_t *image;
	void *host;
};
static void close_graphics(struct browser_source *s)
{
	if (s->host)
		obs_android_browser_destroy(s->host);
	s->host = NULL;
	s->image = NULL;
	if (s->external)
		gs_android_external_texture_destroy(s->external);
	s->external = NULL;
	s->probed = false;
	s->has_image = false;
}
static const char *name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("BrowserSource");
}
static void update(void *data, obs_data_t *settings)
{
	struct browser_source *s = data;
	int64_t w = obs_data_get_int(settings, "width"), h = obs_data_get_int(settings, "height");
	int64_t fps = obs_data_get_int(settings, "fps");
	if (w < 1)
		w = 1;
	if (w > 4096)
		w = 4096;
	if (h < 1)
		h = 1;
	if (h > 4096)
		h = 4096;
	if (fps < 1)
		fps = 1;
	if (fps > 60)
		fps = 60;
	const char *url = obs_data_get_string(settings, "url");
	const char *css = obs_data_get_string(settings, "css");
	const bool transparent = obs_data_get_bool(settings, "transparent");
	long alpha = (long)obs_data_get_int(settings, "alpha_mode");
	if (alpha < BROWSER_ALPHA_COMPAT || alpha > BROWSER_ALPHA_PREMULTIPLIED)
		alpha = BROWSER_ALPHA_COMPAT;
	/* Read from the render thread, written here: atomic, not mutex-guarded, so
	 * a properties change cannot stall a frame. */
	os_atomic_set_long(&s->alpha_mode, alpha);
	pthread_mutex_lock(&s->mutex);
	if (!s->url || s->width != w || s->height != h || strcmp(s->url, url) || strcmp(s->css, css) ||
	    s->transparent != transparent) {
		bfree(s->url);
		bfree(s->css);
		s->url = bstrdup(url);
		s->css = bstrdup(css);
		s->width = (uint32_t)w;
		s->height = (uint32_t)h;
		s->transparent = transparent;
		s->reader_mode = transparent;
		s->dirty = true;
		s->retry_at = 0;
		s->status = 0;
	}
	s->fps = (uint32_t)fps;
	pthread_mutex_unlock(&s->mutex);
}
static void *create(obs_data_t *settings, obs_source_t *source)
{
	UNUSED_PARAMETER(source);
	struct browser_source *s = bzalloc(sizeof(*s));
	if (pthread_mutex_init(&s->mutex, NULL)) {
		bfree(s);
		return NULL;
	}
	update(s, settings);
	return s;
}
static void destroy(void *data)
{
	struct browser_source *s = data;
	obs_enter_graphics();
	pthread_mutex_lock(&s->mutex);
	close_graphics(s);
	pthread_mutex_unlock(&s->mutex);
	obs_leave_graphics();
	pthread_mutex_destroy(&s->mutex);
	bfree(s->url);
	bfree(s->css);
	bfree(s);
}
/* One-shot instrument for the transparency path. SurfaceFlinger owns the
 * virtual display, so the only way to tell "the compositor wrote opaque
 * black" from "OBS dropped the alpha" is to sample the texture the compositor
 * produced. One staging readback per source activation. */
static void probe_alpha(struct browser_source *s)
{
	if (s->probed || !s->image || !s->width || !s->height)
		return;
	s->probed = true;
	if ((uint64_t)s->width * s->height > 8u * 1024u * 1024u) {
		blog(LOG_INFO, "android-browser: alpha probe skipped for %ux%u", s->width, s->height);
		return;
	}
	gs_stagesurf_t *stage = gs_stagesurface_create(s->width, s->height, GS_RGBA);
	if (!stage)
		return;
	gs_stage_texture(stage, s->image);
	uint8_t *ptr = NULL;
	uint32_t linesize = 0;
	if (gs_stagesurface_map(stage, &ptr, &linesize) && ptr) {
		uint8_t amin = 255, amax = 0;
		for (uint32_t y = 0; y < s->height; y++) {
			const uint8_t *row = ptr + (size_t)y * linesize;
			for (uint32_t x = 0; x < s->width; x++) {
				const uint8_t a = row[(size_t)x * 4 + 3];
				amin = a < amin ? a : amin;
				amax = a > amax ? a : amax;
			}
		}
		const uint32_t cx = s->width > 1 ? s->width - 2 : 0;
		const uint32_t cy = s->height > 1 ? s->height - 2 : 0;
		const uint8_t *p = ptr + (size_t)cy * linesize + (size_t)cx * 4;
		blog(LOG_INFO,
		     "android-browser: alpha probe a=[%u,%u] corner_rgba=(%u,%u,%u,%u) transparent=%d",
		     amin, amax, p[0], p[1], p[2], p[3], (int)s->transparent);
		gs_stagesurface_unmap(stage);
	} else {
		blog(LOG_WARNING, "android-browser: alpha probe read-back failed");
	}
	gs_stagesurface_destroy(stage);
}
static void render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	struct browser_source *s = data;
	pthread_mutex_lock(&s->mutex);
	if (s->dirty) {
		close_graphics(s);
		s->dirty = false;
	}
	uint64_t now = os_gettime_ns();
	if (!s->host && now >= s->retry_at) {
		close_graphics(s);
		s->external = gs_android_external_texture_create(s->width, s->height);
		if (s->external) {
			/* A transparent source must not use a SurfaceTexture: this
			 * device's hardware composer writes an opaque target, so alpha
			 * never reaches the texture (verified with a raw frame probe).
			 * ImageReader buffers cannot be scanned out, which keeps
			 * SurfaceFlinger on the GPU composition path, and the frames are
			 * then sampled through an EGLImage - no CPU copy either way. */
			const uint32_t texture =
				s->reader_mode ? 0 : gs_android_external_texture_get_id(s->external);
			s->host = obs_android_browser_create(texture, s->width, s->height, s->url, s->css,
							     s->transparent);
		}
		if (s->host) {
			s->started_at = now;
			s->status = 0;
		}
		if (!s->host) {
			close_graphics(s);
			s->retry_at = now + 2000000000ULL;
			s->status = 2;
		}
	}
	if (s->host) {
		if (s->reload) {
			obs_android_browser_reload(s->host);
			s->reload = false;
		}
		if (now >= s->next_frame) {
			float matrix[16];
			int result = obs_android_browser_update(s->host, matrix);
			if (result > 0) {
				if (s->reader_mode) {
					/* A gralloc image reaches GL bottom-up, while the
					 * interop shader already flips V once for the
					 * SurfaceTexture layout. Compensate with the opposite
					 * flip here; the SurfaceTexture path instead gets its
					 * orientation from the producer's matrix. */
					matrix[5] = -1.0f;
					matrix[13] = 1.0f;
					void *buffer = obs_android_browser_take_buffer(s->host);
					if (buffer && gs_android_external_texture_set_buffer(s->external, buffer))
						s->has_image = true;
				} else {
					s->has_image = true;
				}
				if (!s->has_image) {
					/* Nothing bound yet: sampling an unbound external
					 * texture would draw undefined pixels. */
				} else if ((s->image = gs_android_external_texture_render(s->external, matrix)))
					s->status = 1;
				else {
					close_graphics(s);
					s->has_image = false;
					s->retry_at = now + 2000000000ULL;
					s->status = 2;
				}
			} else if (result < 0 || (s->status == 0 && now - s->started_at > 10000000000ULL)) {
				blog(LOG_WARNING, "android-browser: surface failed or no first frame; retrying");
				close_graphics(s);
				s->has_image = false;
				s->retry_at = now + 2000000000ULL;
				s->status = 2;
			}
			s->next_frame = now + 1000000000ULL / s->fps;
		}
		if (s->image) {
			if (now - s->started_at > 1500000000ULL)
				probe_alpha(s);
			/* Same alpha contract as the screen-capture source: the blend
			 * state and the shader technique are stated explicitly, because a
			 * scene item's blend mode must not be able to turn clear pixels
			 * into opaque black, and because premultiplied versus straight
			 * input needs a different RGB source factor. */
			const long alpha_mode = os_atomic_load_long(&s->alpha_mode);
			const bool prev_linear = gs_get_linear_srgb();
			if (alpha_mode == BROWSER_ALPHA_COMPAT)
				gs_set_linear_srgb(false);
			gs_effect_t *draw = obs_get_base_effect(OBS_EFFECT_DEFAULT);
			const enum gs_color_space space = gs_get_color_space();
			bool linear = gs_get_linear_srgb() || (space != GS_CS_SRGB);
			const bool premultiplied = alpha_mode == BROWSER_ALPHA_PREMULTIPLIED;
			const bool straight = alpha_mode == BROWSER_ALPHA_STRAIGHT;
			const bool scrgb = space == GS_CS_709_SCRGB;
			const char *technique = "Draw";
			gs_blend_state_push();
			if (straight || premultiplied) {
				gs_set_linear_srgb(linear);
				if (scrgb) {
					technique = straight ? "DrawNonlinearAlphaMultiply" : "DrawMultiply";
					gs_effect_set_float(gs_effect_get_param_by_name(draw, "multiplier"),
							    obs_get_video_sdr_white_level() / 80.0f);
				} else if (linear && straight) {
					technique = "DrawNonlinearAlpha";
				}
				gs_blend_function_separate((premultiplied || linear) ? GS_BLEND_ONE : GS_BLEND_SRCALPHA,
							   GS_BLEND_INVSRCALPHA, GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
			} else {
				gs_blend_function_separate(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA, GS_BLEND_ONE,
							   GS_BLEND_INVSRCALPHA);
			}
			while (gs_effect_loop(draw, technique))
				obs_source_draw(s->image, 0, 0, s->width, s->height, false);
			gs_blend_state_pop();
			gs_set_linear_srgb(prev_linear);
		}
	}
	pthread_mutex_unlock(&s->mutex);
}
static uint32_t width(void *data)
{
	struct browser_source *s = data;
	pthread_mutex_lock(&s->mutex);
	uint32_t v = s->width;
	pthread_mutex_unlock(&s->mutex);
	return v;
}
static uint32_t height(void *data)
{
	struct browser_source *s = data;
	pthread_mutex_lock(&s->mutex);
	uint32_t v = s->height;
	pthread_mutex_unlock(&s->mutex);
	return v;
}
static void set_visible(void *data, bool visible)
{
	struct browser_source *s = data;
	pthread_mutex_lock(&s->mutex);
	if (s->host)
		obs_android_browser_set_visible(s->host, visible);
	pthread_mutex_unlock(&s->mutex);
}
static void show(void *data)
{
	set_visible(data, true);
}
static void hide(void *data)
{
	set_visible(data, false);
}
static void touch(void *data, const struct obs_mouse_event *event, int action)
{
	struct browser_source *s = data;
	if (!event)
		return;
	pthread_mutex_lock(&s->mutex);
	if (s->host && !s->dirty)
		obs_android_browser_touch(s->host, event->x, event->y, action);
	pthread_mutex_unlock(&s->mutex);
}
static void mouse_click(void *data, const struct obs_mouse_event *event, int32_t type, bool up, uint32_t count)
{
	UNUSED_PARAMETER(count);
	if (type == MOUSE_LEFT)
		touch(data, event, up ? 1 : 0);
}
static void mouse_move(void *data, const struct obs_mouse_event *event, bool leave)
{
	touch(data, event, leave ? 3 : 2);
}
static void defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "url", "https://obsproject.com/");
	obs_data_set_default_string(settings, "css", "");
	obs_data_set_default_int(settings, "width", 1280);
	obs_data_set_default_int(settings, "height", 720);
	obs_data_set_default_int(settings, "fps", 30);
	obs_data_set_default_bool(settings, "transparent", false);
	obs_data_set_default_int(settings, "alpha_mode", BROWSER_ALPHA_COMPAT);
}
static bool refresh(obs_properties_t *props, obs_property_t *property, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(property);
	struct browser_source *s = data;
	if (s) {
		pthread_mutex_lock(&s->mutex);
		s->reload = true;
		pthread_mutex_unlock(&s->mutex);
	}
	return false;
}
static obs_properties_t *properties(void *data)
{
	obs_properties_t *p = obs_properties_create();
	if (data) {
		struct browser_source *s = data;
		pthread_mutex_lock(&s->mutex);
		int status = s->status;
		pthread_mutex_unlock(&s->mutex);
		const char *key = status == 1 ? "StatusReceiving" : status == 2 ? "StatusRetrying" : "StatusStarting";
		obs_properties_add_text(p, "browser_status", obs_module_text(key), OBS_TEXT_INFO);
	}
	obs_properties_add_text(p, "url", obs_module_text("URL"), OBS_TEXT_DEFAULT);
	obs_properties_add_int(p, "width", obs_module_text("Width"), 1, 4096, 1);
	obs_properties_add_int(p, "height", obs_module_text("Height"), 1, 4096, 1);
	obs_properties_add_int(p, "fps", obs_module_text("FPS"), 1, 60, 1);
	obs_property_t *transparent = obs_properties_add_bool(p, "transparent", obs_module_text("Transparent"));
	obs_property_set_long_description(transparent, obs_module_text("Transparent.Description"));
	obs_property_t *alpha = obs_properties_add_list(p, "alpha_mode", obs_module_text("AlphaMode"),
							OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(alpha, obs_module_text("AlphaMode.Compat"), BROWSER_ALPHA_COMPAT);
	obs_property_list_add_int(alpha, obs_module_text("AlphaMode.Straight"), BROWSER_ALPHA_STRAIGHT);
	obs_property_list_add_int(alpha, obs_module_text("AlphaMode.Premultiplied"), BROWSER_ALPHA_PREMULTIPLIED);
	obs_property_set_long_description(alpha, obs_module_text("AlphaMode.Description"));
	obs_properties_add_text(p, "css", obs_module_text("CSS"), OBS_TEXT_MULTILINE);
	obs_properties_add_button2(p, "refresh", obs_module_text("Refresh"), refresh, data);
	return p;
}
static struct obs_source_info info = {
	.id = "android_browser_source",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_INTERACTION,
	.get_name = name,
	.create = create,
	.destroy = destroy,
	.update = update,
	.get_width = width,
	.get_height = height,
	.video_render = render,
	.get_defaults = defaults,
	.get_properties = properties,
	.show = show,
	.hide = hide,
	.mouse_click = mouse_click,
	.mouse_move = mouse_move,
};
bool obs_module_load(void)
{
	obs_register_source(&info);
	return true;
}
