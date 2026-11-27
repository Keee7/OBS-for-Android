/* Android GPU interop. Copyright (C) 2026 OBS Android contributors.
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <android/native_window.h>
#include <android/hardware_buffer.h>
#include <dlfcn.h>
#include <errno.h>
#include <poll.h>
#include <string.h>
#include <unistd.h>
#include <util/platform.h>
#include "gl-subsystem.h"

#ifndef GL_TEXTURE_EXTERNAL_OES
#define GL_TEXTURE_EXTERNAL_OES 0x8D65
#define GL_TEXTURE_BINDING_EXTERNAL_OES 0x8D67
#endif
#ifndef EGL_RECORDABLE_ANDROID
#define EGL_RECORDABLE_ANDROID 0x3142
#endif
#ifndef EGL_NATIVE_BUFFER_ANDROID
#define EGL_NATIVE_BUFFER_ANDROID 0x3140
#endif
#ifndef EGL_IMAGE_PRESERVED_KHR
#define EGL_IMAGE_PRESERVED_KHR 0x30D2
#endif
#ifndef EGL_SYNC_NATIVE_FENCE_ANDROID
#define EGL_SYNC_NATIVE_FENCE_ANDROID 0x3144
#endif
#ifndef EGL_SYNC_NATIVE_FENCE_FD_ANDROID
#define EGL_SYNC_NATIVE_FENCE_FD_ANDROID 0x3145
#endif
#ifndef EGL_NO_NATIVE_FENCE_FD_ANDROID
#define EGL_NO_NATIVE_FENCE_FD_ANDROID -1
#endif
#ifndef EGL_NO_SYNC_KHR
#define EGL_NO_SYNC_KHR ((EGLSyncKHR)0)
#endif
#ifndef EGL_SYNC_FLUSH_COMMANDS_BIT_KHR
#define EGL_SYNC_FLUSH_COMMANDS_BIT_KHR 0x0001
#endif
#ifndef EGL_TIMEOUT_EXPIRED_KHR
#define EGL_TIMEOUT_EXPIRED_KHR 0x30F5
#endif
#ifndef EGL_CONDITION_SATISFIED_KHR
#define EGL_CONDITION_SATISFIED_KHR 0x30F6
#endif

/* OBS FBOs use a top-down texture convention. Flip once when crossing the
 * Android window/SurfaceTexture boundary; then apply the producer's matrix. */
static const char *vertex_source = "#version 300 es\n"
				   "precision highp float;\n"
				   "uniform mat4 transform; out vec2 uv;\n"
				   "void main() { vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);"
				   "gl_Position=vec4(p*2.0-1.0,0.0,1.0);"
				   "uv=(transform*vec4(p.x,1.0-p.y,0.0,1.0)).xy; }\n";

static const char *encoder_fragment = "#version 300 es\n"
				      "precision highp float; uniform sampler2D image; in vec2 uv; out vec4 color;\n"
				      "void main() { vec3 c=clamp(texture(image,uv).rgb,0.0,1.0);"
				      "c=mix(12.92*c,1.055*pow(c,vec3(1.0/2.4))-0.055,step(vec3(0.0031308),c));"
				      "color=vec4(c,1.0); }\n";

static const char *external_fragment =
	"#version 300 es\n"
	"#extension GL_OES_EGL_image_external_essl3 : require\n"
	"precision highp float; uniform samplerExternalOES image; in vec2 uv; out vec4 color;\n"
	"void main() { vec4 c=texture(image,uv);"
	/* Preserve the producer's premultiplied sRGB bytes. The browser source
	 * uses OBS's non-linear blending path; only undo the target's automatic
	 * sRGB encoding here, rather than changing its alpha convention. */
	"vec3 s=clamp(c.rgb,0.0,1.0);"
	"s=mix(s/12.92,pow((s+0.055)/1.055,vec3(2.4)),step(vec3(0.04045),s));"
	"color=vec4(s,c.a); }\n";

static const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

static GLuint compile_shader(GLenum type, const char *source)
{
	GLuint shader = glCreateShader(type);
	glShaderSource(shader, 1, &source, NULL);
	glCompileShader(shader);
	GLint ok = GL_FALSE;
	glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[1024];
		glGetShaderInfoLog(shader, sizeof(log), NULL, log);
		blog(LOG_ERROR, "Android interop shader: %s", log);
		glDeleteShader(shader);
		return 0;
	}
	return shader;
}

static GLuint create_program(const char *fragment)
{
	GLuint vertex = compile_shader(GL_VERTEX_SHADER, vertex_source);
	GLuint pixel = compile_shader(GL_FRAGMENT_SHADER, fragment);
	GLuint program = 0;
	if (vertex && pixel) {
		program = glCreateProgram();
		glAttachShader(program, vertex);
		glAttachShader(program, pixel);
		glLinkProgram(program);
		GLint ok = GL_FALSE;
		glGetProgramiv(program, GL_LINK_STATUS, &ok);
		if (!ok) {
			char log[1024];
			glGetProgramInfoLog(program, sizeof(log), NULL, log);
			blog(LOG_ERROR, "Android interop program: %s", log);
			glDeleteProgram(program);
			program = 0;
		}
	}
	if (vertex)
		glDeleteShader(vertex);
	if (pixel)
		glDeleteShader(pixel);
	return program;
}

static bool has_gl_extension(const char *extension)
{
	GLint count = 0;
	glGetIntegerv(GL_NUM_EXTENSIONS, &count);
	for (GLint i = 0; i < count; ++i) {
		const char *name = (const char *)glGetStringi(GL_EXTENSIONS, (GLuint)i);
		if (name && strcmp(name, extension) == 0)
			return true;
	}
	return false;
}

struct encoder_surface {
	graphics_t *graphics;
	gs_device_t *device;
	EGLDisplay display;
	EGLContext context;
	EGLSurface surface;
	ANativeWindow *window;
	PFNEGLPRESENTATIONTIMEANDROIDPROC presentation_time;
	GLuint program, vao;
	GLint transform, image;
	bool srgb_write_control;
};

static bool restore_egl(EGLDisplay display, EGLSurface draw, EGLSurface read, EGLContext context)
{
	if (eglMakeCurrent(display, draw, read, context))
		return true;
	blog(LOG_ERROR, "Android interop: could not restore EGL context (0x%x)", eglGetError());
	return false;
}

gs_android_encoder_surface_t *device_android_encoder_surface_create(gs_device_t *device, void *window)
{
	struct encoder_surface *s = bzalloc(sizeof(*s));
	s->device = device;
	s->graphics = gs_get_context();
	s->display = eglGetCurrentDisplay();
	s->window = window;
	ANativeWindow_acquire(s->window);
	EGLContext original = eglGetCurrentContext();
	EGLSurface draw = eglGetCurrentSurface(EGL_DRAW), read = eglGetCurrentSurface(EGL_READ);
	const char *extensions = eglQueryString(s->display, EGL_EXTENSIONS);
	if (!extensions || !strstr(extensions, "EGL_ANDROID_presentation_time"))
		goto fail;
	s->presentation_time = (PFNEGLPRESENTATIONTIMEANDROIDPROC)eglGetProcAddress("eglPresentationTimeANDROID");
	if (!s->presentation_time)
		goto fail;
	const EGLint config_attributes[] = {EGL_SURFACE_TYPE,
					    EGL_WINDOW_BIT,
					    EGL_RENDERABLE_TYPE,
					    EGL_OPENGL_ES3_BIT,
					    EGL_RED_SIZE,
					    8,
					    EGL_GREEN_SIZE,
					    8,
					    EGL_BLUE_SIZE,
					    8,
					    EGL_ALPHA_SIZE,
					    8,
					    EGL_RECORDABLE_ANDROID,
					    EGL_TRUE,
					    EGL_NONE};
	EGLint count = 0;
	EGLConfig config;
	if (!eglChooseConfig(s->display, config_attributes, &config, 1, &count) || !count)
		goto fail;
	const EGLint context_attributes[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
	s->context = eglCreateContext(s->display, config, original, context_attributes);
	if (s->context == EGL_NO_CONTEXT)
		goto fail;
	s->surface = eglCreateWindowSurface(s->display, config, s->window, NULL);
	if (s->surface == EGL_NO_SURFACE)
		goto fail;
	if (!eglMakeCurrent(s->display, s->surface, s->surface, s->context))
		goto fail;
	/* 这块面是 MediaCodec 的输入队列，不是屏幕：必须立即返回。
	 * EGL 默认 swap interval=1，eglSwapBuffers 会等下一个 vsync；只要有一帧
	 * 错过边界，视频线程就被锁到 30fps（60 的一半），随后 OBS 的追赶循环
	 * 又在一瞬间连发几十帧，把推流码率顶到 20Mbps 以上。X11/Wayland 后端
	 * 都是这么关的。 */
	if (eglSwapInterval(s->display, 0) == EGL_FALSE)
		blog(LOG_WARNING, "Android interop: eglSwapInterval(0) 失败（0x%x），编码帧率会被 vsync 量化",
		     eglGetError());
	s->srgb_write_control = has_gl_extension("GL_EXT_sRGB_write_control");
	s->program = create_program(encoder_fragment);
	if (s->program) {
		glGenVertexArrays(1, &s->vao);
		s->transform = glGetUniformLocation(s->program, "transform");
		s->image = glGetUniformLocation(s->program, "image");
	}
	bool ok = s->program && s->vao;
	if (!restore_egl(s->display, draw, read, original))
		ok = false;
	if (ok)
		return s;
fail:
	blog(LOG_ERROR, "Android interop: could not create recordable EGL Surface (0x%x)", eglGetError());
	device_android_encoder_surface_destroy(s);
	return NULL;
}

bool device_android_encoder_surface_submit(gs_android_encoder_surface_t *surface, gs_texture_t *texture,
					   int64_t timestamp_ns)
{
	struct encoder_surface *s = surface;
	GLuint source;
	/* The ring slot remains owned by the GPU encoder until this returns.
	 * Publish producer commands while holding the graphics lock, then release
	 * it before the potentially blocking codec swap. */
	gs_enter_context(s->graphics);
	if (texture->device != s->device || texture->type != GS_TEXTURE_2D) {
		gs_leave_context();
		return false;
	}
	source = texture->texture;
	GLsync produced = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
	glFlush();
	gs_leave_context();
	if (!produced)
		return false;
	if (!eglMakeCurrent(s->display, s->surface, s->surface, s->context)) {
		gs_enter_context(s->graphics);
		glDeleteSync(produced);
		gs_leave_context();
		return false;
	}
	glWaitSync(produced, 0, GL_TIMEOUT_IGNORED);
	glDeleteSync(produced);
	EGLint width = 0, height = 0;
	bool ok = eglQuerySurface(s->display, s->surface, EGL_WIDTH, &width) &&
		  eglQuerySurface(s->display, s->surface, EGL_HEIGHT, &height);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glViewport(0, 0, width, height);
	glDisable(GL_BLEND);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_STENCIL_TEST);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_CULL_FACE);
	if (s->srgb_write_control)
		glDisable(GL_FRAMEBUFFER_SRGB_EXT);
	glUseProgram(s->program);
	glBindVertexArray(s->vao);
	glActiveTexture(GL_TEXTURE0);
	glBindSampler(0, 0);
	glBindTexture(GL_TEXTURE_2D, source);
	glUniform1i(s->image, 0);
	glUniformMatrix4fv(s->transform, 1, GL_FALSE, identity);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	ok = glGetError() == GL_NO_ERROR && ok;
	GLsync consumed = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
	/* Fence allocation failure is fatal, but still protect the ring slot
	 * before returning it to the producer. This is never the steady path. */
	if (!consumed)
		glFinish();
	glFlush();
	if (ok)
		ok = s->presentation_time(s->display, s->surface, timestamp_ns) &&
		     eglSwapBuffers(s->display, s->surface);
	/* A server-side fence in the producer context protects the ring slot from
	 * reuse without a CPU wait or glFinish. Never enter OBS with our private
	 * context still current. */
	eglMakeCurrent(s->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	gs_enter_context(s->graphics);
	if (consumed) {
		glWaitSync(consumed, 0, GL_TIMEOUT_IGNORED);
		glDeleteSync(consumed);
	} else {
		ok = false;
	}
	gs_leave_context();
	return ok;
}

void device_android_encoder_surface_destroy(gs_android_encoder_surface_t *surface)
{
	struct encoder_surface *s = surface;
	if (!s)
		return;
	EGLContext original = eglGetCurrentContext();
	EGLSurface draw = eglGetCurrentSurface(EGL_DRAW), read = eglGetCurrentSurface(EGL_READ);
	if (s->context != EGL_NO_CONTEXT && s->surface != EGL_NO_SURFACE &&
	    eglMakeCurrent(s->display, s->surface, s->surface, s->context)) {
		if (s->program)
			glDeleteProgram(s->program);
		if (s->vao)
			glDeleteVertexArrays(1, &s->vao);
		glFlush();
		restore_egl(s->display, draw, read, original);
	}
	if (s->surface != EGL_NO_SURFACE)
		eglDestroySurface(s->display, s->surface);
	if (s->context != EGL_NO_CONTEXT)
		eglDestroyContext(s->display, s->context);
	ANativeWindow_release(s->window);
	bfree(s);
}

/* Raw GLES operations must restore actual state, rather than invalidating
 * OBS's cached GL state behind its back. Unit zero is the only texture unit
 * touched by the converter. */
struct gl_state {
	GLint program, vao, draw, read, viewport[4], active, texture, external, sampler, unpack;
	GLboolean blend, depth, stencil, scissor, cull, discard, color[4], srgb;
};

static void save_state(struct gl_state *s)
{
	glGetIntegerv(GL_CURRENT_PROGRAM, &s->program);
	glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &s->vao);
	glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &s->draw);
	glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &s->read);
	glGetIntegerv(GL_VIEWPORT, s->viewport);
	glGetIntegerv(GL_ACTIVE_TEXTURE, &s->active);
	glActiveTexture(GL_TEXTURE0);
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &s->texture);
	glGetIntegerv(GL_TEXTURE_BINDING_EXTERNAL_OES, &s->external);
	glGetIntegerv(GL_SAMPLER_BINDING, &s->sampler);
	glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &s->unpack);
	s->blend = glIsEnabled(GL_BLEND);
	s->depth = glIsEnabled(GL_DEPTH_TEST);
	s->stencil = glIsEnabled(GL_STENCIL_TEST);
	s->scissor = glIsEnabled(GL_SCISSOR_TEST);
	s->cull = glIsEnabled(GL_CULL_FACE);
	s->discard = glIsEnabled(GL_RASTERIZER_DISCARD);
	glGetBooleanv(GL_COLOR_WRITEMASK, s->color);
	s->srgb = GLAD_GL_EXT_sRGB_write_control ? glIsEnabled(GL_FRAMEBUFFER_SRGB_EXT) : GL_TRUE;
}

static void set_enabled(GLenum capability, GLboolean value)
{
	if (value)
		glEnable(capability);
	else
		glDisable(capability);
}

static void restore_state(const struct gl_state *s)
{
	glUseProgram((GLuint)s->program);
	glBindVertexArray((GLuint)s->vao);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)s->draw);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)s->read);
	glViewport(s->viewport[0], s->viewport[1], s->viewport[2], s->viewport[3]);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, (GLuint)s->texture);
	glBindTexture(GL_TEXTURE_EXTERNAL_OES, (GLuint)s->external);
	glBindSampler(0, (GLuint)s->sampler);
	glActiveTexture((GLenum)s->active);
	glBindBuffer(GL_PIXEL_UNPACK_BUFFER, (GLuint)s->unpack);
	set_enabled(GL_BLEND, s->blend);
	set_enabled(GL_DEPTH_TEST, s->depth);
	set_enabled(GL_STENCIL_TEST, s->stencil);
	set_enabled(GL_SCISSOR_TEST, s->scissor);
	set_enabled(GL_CULL_FACE, s->cull);
	set_enabled(GL_RASTERIZER_DISCARD, s->discard);
	glColorMask(s->color[0], s->color[1], s->color[2], s->color[3]);
	if (GLAD_GL_EXT_sRGB_write_control)
		set_enabled(GL_FRAMEBUFFER_SRGB_EXT, s->srgb);
}

#define EGL_IMAGE_CACHE_CAP 24

typedef int (*pfn_ahardwarebuffer_get_id_t)(const AHardwareBuffer *, uint64_t *);

struct external_image_slot {
	AHardwareBuffer *buffer;
	uint64_t id;
	bool has_id;
	EGLImageKHR image;
	uint32_t last_use;
};

struct external_texture {
	GLuint external, program, vao, fbo;
	GLint transform, image;
	gs_texture_t *output;
	uint32_t width, height;
	/* Set only when frames arrive as AHardwareBuffers (reader mode). */
	EGLDisplay egl_display;
	PFNEGLCREATEIMAGEKHRPROC create_image;
	PFNEGLDESTROYIMAGEKHRPROC destroy_image;
	PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC get_native_client_buffer;
	pfn_ahardwarebuffer_get_id_t get_buffer_id;
	bool get_buffer_id_probed;
	PFNEGLCREATESYNCKHRPROC create_sync;
	PFNEGLDESTROYSYNCKHRPROC destroy_sync;
	PFNEGLCLIENTWAITSYNCKHRPROC client_wait_sync;
	PFNEGLWAITSYNCKHRPROC wait_sync;
	PFNEGLDUPNATIVEFENCEFDANDROIDPROC dup_native_fence_fd;
	bool native_fence_sync;
	struct external_image_slot image_cache[EGL_IMAGE_CACHE_CAP];
	uint32_t cache_tick;
};

static bool has_egl_extension(const char *extensions, const char *name)
{
	if (!extensions || !name || !*name)
		return false;
	const size_t len = strlen(name);
	const char *p = extensions;
	while ((p = strstr(p, name)) != NULL) {
		const bool start_ok = (p == extensions) || (p[-1] == ' ');
		const char end = p[len];
		const bool end_ok = (end == '\0') || (end == ' ');
		if (start_ok && end_ok)
			return true;
		p += len;
	}
	return false;
}

static void drain_gl_errors(void)
{
	while (glGetError() != GL_NO_ERROR) {
	}
}

static bool should_log_throttled(uint64_t *last_ns, uint32_t *count)
{
	const uint64_t now = os_gettime_ns();
	(*count)++;
	if (*count <= 5 || *last_ns == 0 || now < *last_ns || (now - *last_ns) >= UINT64_C(5000000000)) {
		*last_ns = now;
		return true;
	}
	return false;
}

static bool wait_and_close_fence_fd(int fence_fd)
{
	if (fence_fd < 0)
		return true;
	struct pollfd pfd = {
		.fd = fence_fd,
		.events = POLLIN,
	};
	int ret;
	do {
		ret = poll(&pfd, 1, 1000);
	} while (ret < 0 && errno == EINTR);
	const int err = (ret < 0) ? errno : 0;
	close(fence_fd);
	if (ret > 0 && (pfd.revents & POLLIN) && !(pfd.revents & (POLLERR | POLLNVAL)))
		return true;
	static uint64_t last_log_ns;
	static uint32_t fail_count;
	if (should_log_throttled(&last_log_ns, &fail_count)) {
		if (ret == 0) {
			blog(LOG_WARNING,
			     "Android interop: timed out waiting on acquire fence fd (count=%u)", fail_count);
		} else if (ret < 0) {
			blog(LOG_WARNING,
			     "Android interop: poll on acquire fence fd failed (errno=%d count=%u)", err, fail_count);
		} else {
			blog(LOG_WARNING,
			     "Android interop: acquire fence fd abnormal revents=0x%x (count=%u)",
			     (unsigned int)pfd.revents, fail_count);
		}
	}
	return false;
}

static bool wait_acquire_fence(struct external_texture *t, int acquire_fence_fd)
{
	if (acquire_fence_fd < 0)
		return true;
	if (t->native_fence_sync && (t->wait_sync || t->client_wait_sync)) {
		/* Pass a duplicated fd to eglCreateSyncKHR so that if creation fails
		 * (and EGL closes or consumes the passed fd), we still hold our own
		 * untouched acquire_fence_fd for the poll() fallback without risking
		 * double-close or closing a recycled fd number. */
		const int egl_fd = dup(acquire_fence_fd);
		if (egl_fd >= 0) {
			const EGLint attribs[] = {
				EGL_SYNC_NATIVE_FENCE_FD_ANDROID,
				egl_fd,
				EGL_NONE,
			};
			EGLSyncKHR sync = t->create_sync(t->egl_display, EGL_SYNC_NATIVE_FENCE_ANDROID, attribs);
			if (sync != EGL_NO_SYNC_KHR) {
				/* eglCreateSyncKHR took ownership of egl_fd; close our copy now. */
				close(acquire_fence_fd);
				bool ready = false;
				if (t->wait_sync) {
					if (t->wait_sync(t->egl_display, sync, 0) == EGL_TRUE) {
						ready = true;
					} else {
						const EGLint err = eglGetError();
						static uint64_t last_wait_ns;
						static uint32_t wait_fail_count;
						if (should_log_throttled(&last_wait_ns, &wait_fail_count)) {
							blog(LOG_WARNING,
							     "Android interop: eglWaitSyncKHR failed (0x%x count=%u)%s",
							     err, wait_fail_count,
							     t->client_wait_sync
								     ? "; falling back to eglClientWaitSyncKHR"
								     : "");
						}
					}
				}
				if (!ready && t->client_wait_sync) {
					const EGLint status =
						t->client_wait_sync(t->egl_display, sync, 0, 1000000000ULL);
					if (status == EGL_CONDITION_SATISFIED_KHR) {
						ready = true;
					} else {
						const EGLint err = (status == EGL_FALSE) ? eglGetError() : EGL_SUCCESS;
						static uint64_t last_cwait_ns;
						static uint32_t cwait_fail_count;
						if (should_log_throttled(&last_cwait_ns, &cwait_fail_count)) {
							blog(LOG_WARNING,
							     "Android interop: eglClientWaitSyncKHR failed (status=0x%x err=0x%x count=%u)",
							     status, err, cwait_fail_count);
						}
					}
				}
				t->destroy_sync(t->egl_display, sync);
				return ready;
			}
			const EGLint create_err = eglGetError();
			static uint64_t last_create_ns;
			static uint32_t create_fail_count;
			if (should_log_throttled(&last_create_ns, &create_fail_count)) {
				blog(LOG_WARNING,
				     "Android interop: eglCreateSyncKHR(acquire fence) failed (0x%x count=%u); falling back to poll",
				     create_err, create_fail_count);
			}
		} else {
			const int dup_err = errno;
			static uint64_t last_dup_ns;
			static uint32_t dup_fail_count;
			if (should_log_throttled(&last_dup_ns, &dup_fail_count)) {
				blog(LOG_WARNING,
				     "Android interop: dup(acquire fence fd) failed (errno=%d count=%u); falling back to poll",
				     dup_err, dup_fail_count);
			}
		}
	}
	return wait_and_close_fence_fd(acquire_fence_fd);
}

static int create_release_fence(struct external_texture *t)
{
	if (t->native_fence_sync) {
		EGLSyncKHR sync = t->create_sync(t->egl_display, EGL_SYNC_NATIVE_FENCE_ANDROID, NULL);
		if (sync != EGL_NO_SYNC_KHR) {
			/* Mandatory flush before eglDupNativeFenceFDANDROID per EGL_ANDROID_native_fence_sync. */
			glFlush();
			const EGLint fd = t->dup_native_fence_fd(t->egl_display, sync);
			t->destroy_sync(t->egl_display, sync);
			if (fd != EGL_NO_NATIVE_FENCE_FD_ANDROID && fd >= 0)
				return (int)fd;
			(void)eglGetError();
		} else {
			(void)eglGetError();
		}
	}
	glFinish();
	return -1;
}

gs_android_external_texture_t *device_android_external_texture_create(gs_device_t *device, uint32_t width,
								      uint32_t height)
{
	if (!has_gl_extension("GL_OES_EGL_image_external_essl3")) {
		blog(LOG_ERROR, "Android interop: external OES ESSL3 textures unavailable");
		return NULL;
	}
	EGLDisplay egl_display = eglGetCurrentDisplay();
	if (egl_display == EGL_NO_DISPLAY)
		egl_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	PFNEGLCREATEIMAGEKHRPROC create_image =
		(PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
	PFNEGLDESTROYIMAGEKHRPROC destroy_image =
		(PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
	if (egl_display == EGL_NO_DISPLAY || !create_image || !destroy_image || !glEGLImageTargetTexture2DOES) {
		blog(LOG_ERROR,
		     "Android interop: EGL image entry points unavailable (display=%p create=%p destroy=%p)",
		     (void *)egl_display, (void *)create_image, (void *)destroy_image);
		return NULL;
	}
	struct external_texture *t = bzalloc(sizeof(*t));
	t->width = width;
	t->height = height;
	t->egl_display = egl_display;
	t->create_image = create_image;
	t->destroy_image = destroy_image;
	t->get_native_client_buffer =
		(PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC)eglGetProcAddress("eglGetNativeClientBufferANDROID");

	const char *egl_exts = eglQueryString(egl_display, EGL_EXTENSIONS);
	const bool has_native_fence = has_egl_extension(egl_exts, "EGL_ANDROID_native_fence_sync") &&
				      has_egl_extension(egl_exts, "EGL_KHR_fence_sync");
	const bool has_wait_sync = has_egl_extension(egl_exts, "EGL_KHR_wait_sync");
	t->create_sync = has_native_fence ? (PFNEGLCREATESYNCKHRPROC)eglGetProcAddress("eglCreateSyncKHR") : NULL;
	t->destroy_sync = has_native_fence ? (PFNEGLDESTROYSYNCKHRPROC)eglGetProcAddress("eglDestroySyncKHR") : NULL;
	t->client_wait_sync =
		has_native_fence ? (PFNEGLCLIENTWAITSYNCKHRPROC)eglGetProcAddress("eglClientWaitSyncKHR") : NULL;
	t->wait_sync = (has_native_fence && has_wait_sync)
			       ? (PFNEGLWAITSYNCKHRPROC)eglGetProcAddress("eglWaitSyncKHR")
			       : NULL;
	t->dup_native_fence_fd =
		has_native_fence ? (PFNEGLDUPNATIVEFENCEFDANDROIDPROC)eglGetProcAddress("eglDupNativeFenceFDANDROID")
				 : NULL;
	t->native_fence_sync = has_native_fence && t->create_sync && t->destroy_sync && t->dup_native_fence_fd;
	static bool logged_fence_mode;
	if (!logged_fence_mode) {
		logged_fence_mode = true;
		if (t->native_fence_sync) {
			blog(LOG_INFO,
			     "Android interop: external texture sync using EGL_ANDROID_native_fence_sync (gpu_wait=%s)",
			     t->wait_sync ? "yes" : "no");
		} else {
			blog(LOG_WARNING,
			     "Android interop: EGL_ANDROID_native_fence_sync unavailable; falling back to glFinish() sync");
		}
	}

	struct gl_state state;
	save_state(&state);
	glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
	t->output = device_texture_create(device, width, height, GS_RGBA, 1, NULL, GS_RENDER_TARGET);
	t->program = create_program(external_fragment);
	if (!t->output || !t->program)
		goto fail;
	t->transform = glGetUniformLocation(t->program, "transform");
	t->image = glGetUniformLocation(t->program, "image");
	glGenTextures(1, &t->external);
	glBindTexture(GL_TEXTURE_EXTERNAL_OES, t->external);
	glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glGenVertexArrays(1, &t->vao);
	glGenFramebuffers(1, &t->fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t->output->texture, 0);
	if (!t->external || !t->vao || glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE ||
	    glGetError() != GL_NO_ERROR)
		goto fail;
	restore_state(&state);
	return t;
fail:
	device_android_external_texture_destroy(t);
	restore_state(&state);
	return NULL;
}

uint32_t device_android_external_texture_get_id(gs_android_external_texture_t *texture)
{
	return ((struct external_texture *)texture)->external;
}

static gs_texture_t *render_external_texture(struct external_texture *t, const float *transform, int acquire_fence_fd,
					     int *release_fence_fd)
{
	if (release_fence_fd)
		*release_fence_fd = -1;
	if (!wait_acquire_fence(t, acquire_fence_fd)) {
		static uint64_t last_skip_ns;
		static uint32_t skip_count;
		if (should_log_throttled(&last_skip_ns, &skip_count)) {
			blog(LOG_WARNING,
			     "Android interop: skipping OES->2D render (%ux%u) because acquire fence wait failed (count=%u)",
			     t->width, t->height, skip_count);
		}
		return NULL;
	}

	struct gl_state state;
	save_state(&state);
	drain_gl_errors();
	glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
	glViewport(0, 0, (GLsizei)t->width, (GLsizei)t->height);
	glDisable(GL_BLEND);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_STENCIL_TEST);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_CULL_FACE);
	glDisable(GL_RASTERIZER_DISCARD);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	if (GLAD_GL_EXT_sRGB_write_control)
		glEnable(GL_FRAMEBUFFER_SRGB_EXT);
	glUseProgram(t->program);
	glBindVertexArray(t->vao);
	glActiveTexture(GL_TEXTURE0);
	glBindSampler(0, 0);
	glBindTexture(GL_TEXTURE_EXTERNAL_OES, t->external);
	glUniform1i(t->image, 0);
	glUniformMatrix4fv(t->transform, 1, GL_FALSE, transform);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	const GLenum gl_err = glGetError();
	const bool ok = gl_err == GL_NO_ERROR;
	if (!ok) {
		static uint64_t last_gl_ns;
		static uint32_t gl_err_count;
		if (should_log_throttled(&last_gl_ns, &gl_err_count)) {
			blog(LOG_WARNING, "Android interop: OES->2D render GL error 0x%x (size=%ux%u count=%u)",
			     (unsigned int)gl_err, t->width, t->height, gl_err_count);
		}
	}
	if (release_fence_fd) {
		if (ok) {
			*release_fence_fd = create_release_fence(t);
		} else {
			glFinish();
			*release_fence_fd = -1;
		}
	}
	restore_state(&state);
	return ok ? t->output : NULL;
}

gs_texture_t *device_android_external_texture_render(gs_android_external_texture_t *texture, const float *transform)
{
	return render_external_texture((struct external_texture *)texture, transform, -1, NULL);
}

gs_texture_t *device_android_external_texture_render_fenced(gs_android_external_texture_t *texture,
							    const float *transform, int acquire_fence_fd,
							    int *release_fence_fd)
{
	if (release_fence_fd)
		*release_fence_fd = -1;
	if (!texture || !transform) {
		if (acquire_fence_fd >= 0)
			close(acquire_fence_fd);
		return NULL;
	}
	return render_external_texture((struct external_texture *)texture, transform, acquire_fence_fd,
				       release_fence_fd);
}

bool device_android_external_texture_set_buffer(gs_android_external_texture_t *texture, void *hardware_buffer)
{
	struct external_texture *t = texture;
	if (!t || !t->external || !hardware_buffer)
		return false;

	if (t->egl_display == EGL_NO_DISPLAY)
		t->egl_display = eglGetCurrentDisplay();
	if (t->egl_display == EGL_NO_DISPLAY)
		t->egl_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	if (!t->create_image)
		t->create_image = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
	if (!t->destroy_image)
		t->destroy_image = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
	if (t->egl_display == EGL_NO_DISPLAY || !t->create_image || !t->destroy_image || !glEGLImageTargetTexture2DOES) {
		static bool warned;
		if (!warned) {
			warned = true;
			blog(LOG_ERROR, "Android interop: EGL image entry points unavailable (display=%p create=%p destroy=%p)",
			     (void *)t->egl_display, (void *)t->create_image, (void *)t->destroy_image);
		}
		return false;
	}

	if (!t->get_native_client_buffer)
		t->get_native_client_buffer =
			(PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC)eglGetProcAddress("eglGetNativeClientBufferANDROID");
	if (!t->get_buffer_id_probed) {
		t->get_buffer_id_probed = true;
		t->get_buffer_id = (pfn_ahardwarebuffer_get_id_t)dlsym(RTLD_DEFAULT, "AHardwareBuffer_getId");
	}

	AHardwareBuffer *hb = (AHardwareBuffer *)hardware_buffer;
	uint64_t hb_id = 0;
	const bool has_id = (t->get_buffer_id && t->get_buffer_id(hb, &hb_id) == 0);
	EGLImageKHR image = EGL_NO_IMAGE_KHR;
	for (size_t i = 0; i < EGL_IMAGE_CACHE_CAP; i++) {
		if (t->image_cache[i].image == EGL_NO_IMAGE_KHR || !t->image_cache[i].buffer)
			continue;
		const bool match = has_id ? (t->image_cache[i].has_id && t->image_cache[i].id == hb_id)
					  : (t->image_cache[i].buffer == hb);
		if (match) {
			image = t->image_cache[i].image;
			t->image_cache[i].last_use = ++t->cache_tick;
			break;
		}
	}

	if (image == EGL_NO_IMAGE_KHR) {
		/* EGL_IMAGE_PRESERVED_KHR is not part of EGL_ANDROID_image_native_buffer
		 * and the driver rejects it for AHardwareBuffers (EGL_BAD_PARAMETER), so
		 * no attributes are passed.
		 * Per EGL_ANDROID_image_native_buffer, ctx must be EGL_NO_CONTEXT and
		 * AHardwareBuffer is wrapped via eglGetNativeClientBufferANDROID. Probe
		 * spec-compliant paths first and only cache once a path succeeds so a
		 * transient first-buffer failure never locks out subsequent imports. */
		enum {
			IMPORT_UNKNOWN = -1,
			IMPORT_WRAPPED_NO_CTX = 0,
			IMPORT_DIRECT_NO_CTX = 1,
			IMPORT_WRAPPED_CTX = 2,
			IMPORT_DIRECT_CTX = 3,
		};
		static const char *const import_names[4] = {
			"wrapped",
			"direct",
			"wrapped+context",
			"direct+context",
		};
		static int working = IMPORT_UNKNOWN;
		if (working == IMPORT_UNKNOWN) {
			EGLClientBuffer wrapped =
				t->get_native_client_buffer ? t->get_native_client_buffer(hb) : NULL;
			EGLContext ctx = eglGetCurrentContext();
			const struct {
				EGLClientBuffer buffer;
				EGLContext context;
			} variants[4] = {
				{wrapped, EGL_NO_CONTEXT},
				{(EGLClientBuffer)hb, EGL_NO_CONTEXT},
				{wrapped, ctx},
				{(EGLClientBuffer)hb, ctx},
			};
			EGLint errors[4] = {EGL_SUCCESS, EGL_SUCCESS, EGL_SUCCESS, EGL_SUCCESS};
			for (int i = 0; i < 4; i++) {
				if (!variants[i].buffer)
					continue;
				EGLImageKHR candidate = t->create_image(t->egl_display, variants[i].context,
									EGL_NATIVE_BUFFER_ANDROID,
									variants[i].buffer, NULL);
				if (candidate != EGL_NO_IMAGE_KHR) {
					working = i;
					blog(LOG_INFO, "Android interop: EGL image via %s", import_names[i]);
					image = candidate;
					break;
				}
				errors[i] = eglGetError();
			}
			if (working == IMPORT_UNKNOWN) {
				static uint64_t last_probe_fail_ns;
				static uint32_t probe_fail_count;
				if (should_log_throttled(&last_probe_fail_ns, &probe_fail_count)) {
					AHardwareBuffer_Desc desc = {0};
					AHardwareBuffer_describe(hb, &desc);
					blog(LOG_ERROR,
					     "Android interop: no EGL image path for %ux%u stride=%u format=%u usage=0x%llx "
					     "(errs: wrapped=0x%x direct=0x%x wrapped+ctx=0x%x direct+ctx=0x%x count=%u)",
					     desc.width, desc.height, desc.stride, desc.format,
					     (unsigned long long)desc.usage, errors[0], errors[1], errors[2],
					     errors[3], probe_fail_count);
				}
				return false;
			}
		} else {
			const bool use_wrapped = (working == IMPORT_WRAPPED_NO_CTX || working == IMPORT_WRAPPED_CTX);
			const bool use_ctx = (working == IMPORT_WRAPPED_CTX || working == IMPORT_DIRECT_CTX);
			EGLClientBuffer client =
				(use_wrapped && t->get_native_client_buffer) ? t->get_native_client_buffer(hb)
									     : (EGLClientBuffer)hb;
			const EGLContext ctx = use_ctx ? eglGetCurrentContext() : EGL_NO_CONTEXT;
			if (client)
				image = t->create_image(t->egl_display, ctx, EGL_NATIVE_BUFFER_ANDROID, client, NULL);
			if (image == EGL_NO_IMAGE_KHR) {
				const EGLint err = client ? eglGetError() : EGL_BAD_PARAMETER;
				static uint64_t last_import_fail_ns;
				static uint32_t import_fail_count;
				if (should_log_throttled(&last_import_fail_ns, &import_fail_count)) {
					AHardwareBuffer_Desc desc = {0};
					AHardwareBuffer_describe(hb, &desc);
					blog(LOG_WARNING,
					     "Android interop: EGL image via %s failed (0x%x client=%p) for %ux%u "
					     "stride=%u format=%u usage=0x%llx (count=%u)",
					     import_names[working], err, (void *)client, desc.width, desc.height,
					     desc.stride, desc.format, (unsigned long long)desc.usage,
					     import_fail_count);
				}
				return false;
			}
		}

		size_t slot = 0;
		for (size_t i = 0; i < EGL_IMAGE_CACHE_CAP; i++) {
			if (!t->image_cache[i].buffer) {
				slot = i;
				break;
			}
			if (t->image_cache[i].last_use < t->image_cache[slot].last_use)
				slot = i;
		}
		if (t->image_cache[slot].image != EGL_NO_IMAGE_KHR)
			t->destroy_image(t->egl_display, t->image_cache[slot].image);
		if (t->image_cache[slot].buffer)
			AHardwareBuffer_release(t->image_cache[slot].buffer);
		AHardwareBuffer_acquire(hb);
		t->image_cache[slot].buffer = hb;
		t->image_cache[slot].id = hb_id;
		t->image_cache[slot].has_id = has_id;
		t->image_cache[slot].image = image;
		t->image_cache[slot].last_use = ++t->cache_tick;
	}

	struct gl_state state;
	save_state(&state);
	drain_gl_errors();
	glBindTexture(GL_TEXTURE_EXTERNAL_OES, t->external);
	glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, (GLeglImageOES)image);
	const GLenum gl_err = glGetError();
	const bool ok = gl_err == GL_NO_ERROR;
	restore_state(&state);
	if (!ok) {
		static uint64_t last_bind_ns;
		static uint32_t bind_fail_count;
		if (should_log_throttled(&last_bind_ns, &bind_fail_count)) {
			blog(LOG_ERROR, "Android interop: glEGLImageTargetTexture2DOES failed (0x%x count=%u)",
			     (unsigned int)gl_err, bind_fail_count);
		}
	}
	return ok;
}

void device_android_external_texture_destroy(gs_android_external_texture_t *texture)
{
	struct external_texture *t = texture;
	if (!t)
		return;
	for (size_t i = 0; i < EGL_IMAGE_CACHE_CAP; i++) {
		if (t->image_cache[i].buffer) {
			if (t->image_cache[i].image != EGL_NO_IMAGE_KHR && t->destroy_image &&
			    t->egl_display != EGL_NO_DISPLAY)
				t->destroy_image(t->egl_display, t->image_cache[i].image);
			AHardwareBuffer_release(t->image_cache[i].buffer);
		}
		t->image_cache[i].image = EGL_NO_IMAGE_KHR;
		t->image_cache[i].buffer = NULL;
	}
	if (t->fbo)
		glDeleteFramebuffers(1, &t->fbo);
	if (t->vao)
		glDeleteVertexArrays(1, &t->vao);
	if (t->external)
		glDeleteTextures(1, &t->external);
	if (t->program)
		glDeleteProgram(t->program);
	gs_texture_destroy(t->output);
	bfree(t);
}
