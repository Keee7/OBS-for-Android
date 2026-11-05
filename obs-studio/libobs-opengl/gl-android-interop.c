/* Android GPU interop. Copyright (C) 2026 OBS Android contributors.
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <android/native_window.h>
#include <string.h>
#include "gl-subsystem.h"

#ifndef GL_TEXTURE_EXTERNAL_OES
#define GL_TEXTURE_EXTERNAL_OES 0x8D65
#define GL_TEXTURE_BINDING_EXTERNAL_OES 0x8D67
#endif
#ifndef EGL_RECORDABLE_ANDROID
#define EGL_RECORDABLE_ANDROID 0x3142
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

struct external_texture {
	GLuint external, program, vao, fbo;
	GLint transform, image;
	gs_texture_t *output;
	uint32_t width, height;
};

gs_android_external_texture_t *device_android_external_texture_create(gs_device_t *device, uint32_t width,
								      uint32_t height)
{
	if (!has_gl_extension("GL_OES_EGL_image_external_essl3")) {
		blog(LOG_ERROR, "Android interop: external OES ESSL3 textures unavailable");
		return NULL;
	}
	struct external_texture *t = bzalloc(sizeof(*t));
	t->width = width;
	t->height = height;
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

gs_texture_t *device_android_external_texture_render(gs_android_external_texture_t *texture, const float *transform)
{
	struct external_texture *t = texture;
	struct gl_state state;
	save_state(&state);
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
	bool ok = glGetError() == GL_NO_ERROR;
	restore_state(&state);
	return ok ? t->output : NULL;
}

void device_android_external_texture_destroy(gs_android_external_texture_t *texture)
{
	struct external_texture *t = texture;
	if (!t)
		return;
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
