/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * reader_ctx 的状态机回归。它守的是本轮最核心的三条不变量：
 *
 *   1) retire 之后 enter 一律失败 —— "关门前放新回调进工作区"是双采集的入口；
 *   2) 已经在工作区里的回调会被 users 计数等到，wait_idle 不会提前返回；
 *   3) 源销毁后 reader_ctx_source() 返回 NULL，回调不会拿到已释放的 obs_source。
 *      这条是 c->source 裸指针那个 UAF 的正面测试。
 *
 * 另加一条 build 期的顺序约束检查：ctx 必须在 reader 建好之后才创建，
 * 否则 AImageReader_new 之前的失败路径会让 close 早退、ctx 卡在未 retire 状态。
 * 那条用源码扫描在 test-android-reader-ctx.ps1 里做。
 */
#include "reader-ctx.h"

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

/* ---- 1) retire 之后不再放行 ---- */
static void test_retire_blocks_enter(void)
{
obs_source_t *s = mock_source_create();
struct reader_ctx *ctx = reader_ctx_create(s);
assert(ctx);

assert(reader_ctx_enter(ctx));
reader_ctx_leave(ctx);

reader_ctx_retire(ctx);
assert(!reader_ctx_enter(ctx));
assert(!reader_ctx_enter(ctx));

/* retire 幂等 */
reader_ctx_retire(ctx);
assert(!reader_ctx_enter(ctx));

obs_weak_source_release(ctx->weak);
pthread_mutex_destroy(&ctx->lock);
bfree(ctx);
obs_source_release(s);
puts("PASS: retire blocks enter");
}

/* ---- 2) 工作区里的回调被 wait_idle 等到 ---- */
struct holder {
struct reader_ctx *ctx;
pthread_mutex_t gate;
pthread_cond_t cv;
bool inside;
bool release;
};

static void *holder_thread(void *arg)
{
struct holder *h = arg;
assert(reader_ctx_enter(h->ctx));
pthread_mutex_lock(&h->gate);
h->inside = true;
pthread_cond_broadcast(&h->cv);
while (!h->release)
pthread_cond_wait(&h->cv, &h->gate);
pthread_mutex_unlock(&h->gate);
reader_ctx_leave(h->ctx);
return NULL;
}

static void test_wait_idle_sees_worker(void)
{
obs_source_t *s = mock_source_create();
struct reader_ctx *ctx = reader_ctx_create(s);
assert(ctx);

struct holder h = {.ctx = ctx, .inside = false, .release = false};
pthread_mutex_init(&h.gate, NULL);
pthread_cond_init(&h.cv, NULL);

pthread_t t;
assert(pthread_create(&t, NULL, holder_thread, &h) == 0);

pthread_mutex_lock(&h.gate);
while (!h.inside)
pthread_cond_wait(&h.cv, &h.gate);
pthread_mutex_unlock(&h.gate);

/* 回调还在里面：0 ms 只探一次，必须返回 false */
reader_ctx_retire(ctx);
assert(!reader_ctx_wait_idle(ctx, 0));

pthread_mutex_lock(&h.gate);
h.release = true;
pthread_cond_broadcast(&h.cv);
pthread_mutex_unlock(&h.gate);
pthread_join(t, NULL);

assert(reader_ctx_wait_idle(ctx, 100));

pthread_cond_destroy(&h.cv);
pthread_mutex_destroy(&h.gate);
obs_weak_source_release(ctx->weak);
pthread_mutex_destroy(&ctx->lock);
bfree(ctx);
obs_source_release(s);
puts("PASS: wait_idle sees worker in flight");
}

/* ---- 3) 源销毁后取不到 src（c->source 裸指针 UAF 的正面测试） ---- */
static void test_source_null_after_destroy(void)
{
obs_source_t *s = mock_source_create();
struct reader_ctx *ctx = reader_ctx_create(s);
assert(ctx);

obs_source_t *src = reader_ctx_source(ctx);
assert(src == s);
assert(s->ref.refs == 1); /* 取到的那一份强引用 */
obs_source_release(src);  /* 归还，回到 0 */

/* 外部最后一份引用走了 ⇒ refs 到 -1 ⇒ 源销毁 */
obs_source_release(s);
assert(s->destroyed);

/* 回调此刻进来：必须拿不到 src，而不是拿到悬垂指针 */
assert(reader_ctx_source(ctx) == NULL);

obs_weak_source_release(ctx->weak);
pthread_mutex_destroy(&ctx->lock);
bfree(ctx);
puts("PASS: source is NULL after destroy");
}

/* ---- 4) owner 原子切换：destroy 之后回调读不到 owner ---- */
static void test_owner_detach(void)
{
obs_source_t *s = mock_source_create();
struct reader_ctx *ctx = reader_ctx_create(s);
assert(ctx);

int dummy = 0;
reader_ctx_set_owner(ctx, &dummy);
assert(reader_ctx_owner(ctx) == &dummy);

reader_ctx_retire(ctx);
reader_ctx_set_owner(ctx, NULL);
assert(reader_ctx_owner(ctx) == NULL);
assert(!reader_ctx_enter(ctx));

obs_weak_source_release(ctx->weak);
pthread_mutex_destroy(&ctx->lock);
bfree(ctx);
obs_source_release(s);
puts("PASS: owner detaches before free");
}


/* ---- 5) ctx 的生命周期契约：允许 retire，但不允许 destroy ----
 *
 * reader-ctx.h 顶部写明 ctx「不随 close 释放」，因为框架可能"已决定派发、尚未进入
 * 函数体"，那一段不在 users 计数里。所以本文件**不提供** reader_ctx_destroy()：
 * 一旦提供，destroy 路径上 wait_idle()==true 就会被误当成"可以释放"，把 UAF 从
 * close 挪到 destroy。
 *
 * 这条断言防止后来者把 reader_ctx_destroy() 加回来。用源码扫描做（见
 * scripts/test-android-reader-ctx.ps1 的第二个断言），这里只钉住 retire 之后
 * enter 仍然安全这一半 —— 即 ctx 必须活到进程结束。
 */
static void test_ctx_survives_retire(void)
{
obs_source_t *s = mock_source_create();
struct reader_ctx *ctx = reader_ctx_create(s);
assert(ctx);

/* 模拟 close：retire + owner 解绑，但**不**释放 */
reader_ctx_retire(ctx);
reader_ctx_set_owner(ctx, NULL);

/* 模拟"已决定派发、尚未进入"的回调此刻进来：enter 必须安全地返回 false，
 * 既不能崩，也不能读到已释放内存。 */
assert(!reader_ctx_enter(ctx));
assert(reader_ctx_owner(ctx) == NULL);
/* 源已销毁时同样不能崩 */
obs_source_release(s);
assert(reader_ctx_source(ctx) == NULL);
assert(!reader_ctx_enter(ctx));

/* ctx 有意泄漏（进程级），此处不释放。 */
puts("PASS: ctx survives retire (no destroy by design)");
}
int main(void)
{
test_retire_blocks_enter();
test_wait_idle_sees_worker();
test_source_null_after_destroy();
test_owner_detach();
test_ctx_survives_retire();
puts("reader-ctx tests passed");
return 0;
}


