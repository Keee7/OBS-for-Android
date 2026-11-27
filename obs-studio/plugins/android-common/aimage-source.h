/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * aimage-source.h —— AImageReader + reader_ctx 防 UAF 生命周期封装
 *
 * 把 camera2 与 screen 采集源共用的 AImageReader 建/拆/监听/隔离契约收口到一处：
 *   1. open(): wait_idle 校验 → AImageReader_new → 新 reader_ctx → retire 旧 ctx →
 *      清除 shutting_down → setImageListener → getWindow；任一步失败自清并归还错误码。
 *   2. on_image_available(): reader_ctx_enter → shutting_down 短路 → 弱引用取 obs_source_t →
 *      AImageReader_acquireLatestImage → on_frame → AImage_delete → obs_source_release → leave。
 *   3. close(): shutting_down=1 → setImageListener(NULL) → retire → wait_idle(timeout_ms)；
 *      超时时保留 reader/owner/shutting_down=1 隔离旧回调（不制造 UAF）。
 *   4. destroy(): 调用方释放自身结构体前询问 wait_idle(0)，返回 false 表示仍有回调在场需隔离。
 */

#pragma once

#include <media/NdkImage.h>
#include <media/NdkImageReader.h>
#include <stdbool.h>
#include <stdint.h>

#include <obs.h>
#include <util/threading.h>

#include "reader-ctx.h"

typedef void (*android_image_frame_cb)(void *owner, obs_source_t *src, AImage *image);
typedef void (*android_image_error_cb)(void *owner, media_status_t status);

enum android_image_open_err {
	ANDROID_IMAGE_OK = 0,
	ANDROID_IMAGE_ERR_BUSY,
	ANDROID_IMAGE_ERR_READER,
	ANDROID_IMAGE_ERR_CTX,
	ANDROID_IMAGE_ERR_LISTENER,
	ANDROID_IMAGE_ERR_WINDOW,
};

struct android_image_source {
	AImageReader *reader;
	ANativeWindow *window;
	struct reader_ctx *ctx;
	volatile long shutting_down;

	obs_source_t *source;
	void *owner;
	android_image_frame_cb on_frame;
	android_image_error_cb on_error;
	media_status_t last_status;
};

static inline void android_image_source_init(struct android_image_source *s, obs_source_t *source, void *owner,
					   android_image_frame_cb on_frame, android_image_error_cb on_error)
{
	if (!s)
		return;
	s->reader = NULL;
	s->window = NULL;
	s->ctx = NULL;
	s->shutting_down = 0;
	s->source = source;
	s->owner = owner;
	s->on_frame = on_frame;
	s->on_error = on_error;
	s->last_status = AMEDIA_OK;
}

static inline bool android_image_source_is_idle(const struct android_image_source *s)
{
	return !s || reader_ctx_wait_idle(s->ctx, 0);
}

static inline void android_image_source_on_available(void *context, AImageReader *reader)
{
	struct reader_ctx *ctx = context;
	if (!reader_ctx_enter(ctx))
		return;

	struct android_image_source *s = reader_ctx_owner(ctx);
	if (!s || os_atomic_load_long(&s->shutting_down) || !reader)
		goto out;

	obs_source_t *src = reader_ctx_source(ctx);
	if (!src)
		goto out;

	AImage *image = NULL;
	const media_status_t st = AImageReader_acquireLatestImage(reader, &image);
	if (st != AMEDIA_OK || !image) {
		if (s->on_error)
			s->on_error(s->owner, st);
		obs_source_release(src);
		goto out;
	}

	if (s->on_frame)
		s->on_frame(s->owner, src, image);
	AImage_delete(image);
	obs_source_release(src);

out:
	reader_ctx_leave(ctx);
}

static inline void android_image_source_close(struct android_image_source *s, const char *log_tag, int wait_ms)
{
	if (!s || !s->reader)
		return;
	os_atomic_set_long(&s->shutting_down, 1);

	AImageReader_setImageListener(s->reader, NULL);
	reader_ctx_retire(s->ctx);
	if (reader_ctx_wait_idle(s->ctx, wait_ms)) {
		reader_ctx_set_owner(s->ctx, NULL);
		AImageReader_delete(s->reader);
		s->ctx = NULL;
		os_atomic_set_long(&s->shutting_down, 0);
	} else {
		blog(LOG_ERROR,
		     "%s: 等 %d ms 回调仍未退出，这次不 delete reader（隔离旧上下文，宁可泄漏不制造 UAF）",
		     log_tag ? log_tag : "android-image", wait_ms);
	}
	s->reader = NULL;
	s->window = NULL;
}

static inline enum android_image_open_err android_image_source_open(struct android_image_source *s, int32_t width,
								    int32_t height, int32_t format,
								    int32_t max_images, const char *log_tag)
{
	if (!s)
		return ANDROID_IMAGE_ERR_READER;
	s->last_status = AMEDIA_OK;

	if (!reader_ctx_wait_idle(s->ctx, 0))
		return ANDROID_IMAGE_ERR_BUSY;

	const media_status_t rst = AImageReader_new(width, height, format, max_images, &s->reader);
	if (rst != AMEDIA_OK || !s->reader) {
		s->reader = NULL;
		s->last_status = rst;
		return ANDROID_IMAGE_ERR_READER;
	}

	struct reader_ctx *fresh = reader_ctx_create(s->source);
	if (!fresh) {
		android_image_source_close(s, log_tag, 2000);
		return ANDROID_IMAGE_ERR_CTX;
	}
	reader_ctx_retire(s->ctx);
	reader_ctx_set_owner(fresh, s);
	s->ctx = fresh;
	os_atomic_set_long(&s->shutting_down, 0);

	AImageReader_ImageListener listener = {s->ctx, android_image_source_on_available};
	const media_status_t lst = AImageReader_setImageListener(s->reader, &listener);
	if (lst != AMEDIA_OK) {
		s->last_status = lst;
		android_image_source_close(s, log_tag, 2000);
		return ANDROID_IMAGE_ERR_LISTENER;
	}

	const media_status_t wst = AImageReader_getWindow(s->reader, &s->window);
	if (wst != AMEDIA_OK || !s->window) {
		s->window = NULL;
		s->last_status = wst;
		android_image_source_close(s, log_tag, 2000);
		return ANDROID_IMAGE_ERR_WINDOW;
	}

	return ANDROID_IMAGE_OK;
}

static inline bool android_image_source_destroy(struct android_image_source *s)
{
	if (!s)
		return true;
	if (!reader_ctx_wait_idle(s->ctx, 0))
		return false;
	reader_ctx_set_owner(s->ctx, NULL);
	s->ctx = NULL;
	return true;
}
