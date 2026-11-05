/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <obs-module.h>
#include <obs-android.h>
#include <graphics/graphics.h>
#include <util/platform.h>
#include <pthread.h>
#include <string.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("android-browser", "en-US")
MODULE_EXPORT const char *obs_module_description(void)
{
	return "Android WebView browser source";
}

struct browser_source {
	pthread_mutex_t mutex;
	uint32_t width, height, fps;
	char *url, *css;
	bool dirty, reload;
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
	pthread_mutex_lock(&s->mutex);
	if (!s->url || s->width != w || s->height != h || strcmp(s->url, url) || strcmp(s->css, css)) {
		bfree(s->url);
		bfree(s->css);
		s->url = bstrdup(url);
		s->css = bstrdup(css);
		s->width = (uint32_t)w;
		s->height = (uint32_t)h;
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
		if (s->external)
			s->host = obs_android_browser_create(gs_android_external_texture_get_id(s->external), s->width,
							     s->height, s->url, s->css);
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
				s->image = gs_android_external_texture_render(s->external, matrix);
				if (s->image)
					s->status = 1;
				else {
					close_graphics(s);
					s->retry_at = now + 2000000000ULL;
					s->status = 2;
				}
			} else if (result < 0 || (s->status == 0 && now - s->started_at > 10000000000ULL)) {
				blog(LOG_WARNING, "android-browser: surface failed or no first frame; retrying");
				close_graphics(s);
				s->retry_at = now + 2000000000ULL;
				s->status = 2;
			}
			s->next_frame = now + 1000000000ULL / s->fps;
		}
		if (s->image) {
			gs_effect_t *draw = obs_get_base_effect(OBS_EFFECT_DEFAULT);
			while (gs_effect_loop(draw, "Draw"))
				obs_source_draw(s->image, 0, 0, s->width, s->height, false);
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
