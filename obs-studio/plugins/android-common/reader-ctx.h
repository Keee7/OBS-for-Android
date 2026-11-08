/*
 * reader-ctx.h —— AImageReader 回调上下文的生命周期
 *
 * 为什么需要这一层：
 *
 *   1) 源会被销毁。obs_source_destroy_defer() 在 info.destroy() 返回后**无条件**
 *      bfree(source)（obs-source.c:879）。而 AImageReader 的回调跑在框架自己的
 *      线程上，AImageReader_delete() 不 join 它（NdkImageReader.h 只写 "Do NOT
 *      access the reader object ... after this method returns"）。所以回调里
 *      不能裸借 source：采集结构体被 free 之后，下一次回调解引用 c->source
 *      就是 use-after-free。
 *
 *   2) 回调不能"证明不再来"。in_callback == 0 只覆盖函数体内部；框架"已决定派发、
 *      尚未进入函数体"的那一段不在计数里，而 setImageListener(NULL) 也没有
 *      "之后不再派发"的承诺。所以既不能在 in_callback 归零时断定安全，也不能靠
 *      比对 reader 指针（地址会被 AImageReader_new 复用，同时产生漏挡与误挡）。
 *
 * 这里的分工：
 *
 *   - 源：只持弱引用。每次回调进入工作区后现取 obs_weak_source_get_source()，
 *     取不到就退 —— 源进销毁流程后该调用返回 NULL，回调不会碰到已释放对象。
 *   - reader：close 先 retire（此后回调不再进工作区），再等 users 归零；归零即
 *     delete reader，队列里的 AImage 随之归还系统；等不到则把 reader 交给调用方
 *     做兜底处理（延迟回收或有意泄漏）。
 *   - ctx 自己：独立于采集结构体分配，且**不随 close 释放**。"已决定派发"的回调
 *     仍会来读 retired 与 weak，释放它等于把问题挪个位置。每个 ctx 不到 100 字节，
 *     一次 open 一份，这个有界代价换掉的是无法证明的 UAF。
 *
 * 用法（回调侧）：
 *
 *   struct reader_ctx *ctx = context;
 *   if (!reader_ctx_enter(ctx))
 *           return;
 *   obs_source_t *src = reader_ctx_source(ctx);
 *   void *owner = reader_ctx_owner(ctx);
 *   if (src && owner) { ... 用 owner 与 src ... }
 *   obs_source_release(src);
 *   reader_ctx_leave(ctx);
 *
 * 用法（close 侧）：
 *
 *   reader_ctx_retire(ctx);
 *   if (reader_ctx_wait_idle(ctx, 2000)) {
 *           AImageReader_delete(reader);      // 队列里的 AImage 一并归还
 *           reader_ctx_set_owner(ctx, NULL);
 *   } else {
 *           // 兜底：reader 与 owner 交给延迟回收，别在这里 delete
 *   }
 */

#pragma once

#include <pthread.h>
#include <stdbool.h>

#include <obs.h>
#include <util/platform.h>
#include <util/threading.h>

struct reader_ctx {
	pthread_mutex_t lock;
	bool retired;
	long users;
	void *owner;
	obs_weak_source_t *weak;
};

static inline struct reader_ctx *reader_ctx_create(obs_source_t *source)
{
	struct reader_ctx *ctx = bzalloc(sizeof(*ctx));

	pthread_mutex_init_value(&ctx->lock);
	if (pthread_mutex_init(&ctx->lock, NULL) != 0) {
		bfree(ctx);
		return NULL;
	}

	ctx->weak = obs_source_get_weak_source(source);
	if (!ctx->weak) {
		pthread_mutex_destroy(&ctx->lock);
		bfree(ctx);
		return NULL;
	}

	return ctx;
}

/* 源正在销毁时返回 NULL（弱引用已过期）。返回非 NULL 即为一份强引用，用完 release。 */
static inline obs_source_t *reader_ctx_source(struct reader_ctx *ctx)
{
	return ctx ? obs_weak_source_get_source(ctx->weak) : NULL;
}

/* 进入工作区。retired 之后一律拒绝，因此调用方拿到 true 时 owner 一定还有效。 */
static inline bool reader_ctx_enter(struct reader_ctx *ctx)
{
	if (!ctx)
		return false;

	pthread_mutex_lock(&ctx->lock);
	const bool ok = !ctx->retired;
	if (ok)
		ctx->users++;
	pthread_mutex_unlock(&ctx->lock);

	return ok;
}

static inline void reader_ctx_leave(struct reader_ctx *ctx)
{
	if (!ctx)
		return;

	pthread_mutex_lock(&ctx->lock);
	if (ctx->users > 0)
		ctx->users--;
	pthread_mutex_unlock(&ctx->lock);
}

/* 关门：此后不再有新回调进入工作区。已在里面的靠 users 计数等。 */
static inline void reader_ctx_retire(struct reader_ctx *ctx)
{
	if (!ctx)
		return;

	pthread_mutex_lock(&ctx->lock);
	ctx->retired = true;
	pthread_mutex_unlock(&ctx->lock);
}

/* 等已进入的回调退出。返回 true 表示没人还在用 owner/reader。
 * timeout_ms == 0 时只探一次（给"能不能安全拆"的判断用）。 */
static inline bool reader_ctx_wait_idle(struct reader_ctx *ctx, int timeout_ms)
{
	if (!ctx)
		return true;

	for (int waited = 0;; waited++) {
		pthread_mutex_lock(&ctx->lock);
		const long users = ctx->users;
		pthread_mutex_unlock(&ctx->lock);

		if (users == 0)
			return true;
		if (waited >= timeout_ms)
			return false;

		os_sleep_ms(1);
	}
}

static inline void *reader_ctx_owner(struct reader_ctx *ctx)
{
	if (!ctx)
		return NULL;

	pthread_mutex_lock(&ctx->lock);
	void *owner = ctx->owner;
	pthread_mutex_unlock(&ctx->lock);

	return owner;
}

static inline void reader_ctx_set_owner(struct reader_ctx *ctx, void *owner)
{
	if (!ctx)
		return;

	pthread_mutex_lock(&ctx->lock);
	ctx->owner = owner;
	pthread_mutex_unlock(&ctx->lock);
}
