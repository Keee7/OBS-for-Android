/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * reader-ctx-test 用的最小 obs 桩。只提供 reader-ctx.h 真正用到的几样东西，
 * 但**语义按 obs-source.c 对齐**（否则测试会变成自证）：
 *
 *   - 真实实现里 obs_weak_source_t 与 obs_source_t 是同一块内存：
 *     get_weak(source) 就是 (obs_weak_source_t *)source->context.control，
 *     而 control 里同时有 weak_refs 与 source 两个字段（obs-internal.h:618-626、:779-782）。
 *     所以这里让 mock_source 内嵌 weak 视图，二者指向同一块。
 *   - obs_weak_source_get_source() 在 refs 已 < 0（源销毁）时返回 NULL，
 *     否则把 refs +1 并返回 source —— 这就是回调拿到 NULL 的那条判据。
 */
#pragma once

#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>

typedef struct obs_weak_ref {
long refs;
long weak_refs;
} obs_weak_ref;

typedef struct obs_source {
obs_weak_ref ref;
bool destroyed;
struct obs_weak_source *control; /* 指回自身，模拟 context.control */
} obs_source_t;

typedef struct obs_weak_source {
obs_weak_ref ref;
obs_source_t *source;
} obs_weak_source_t;

#define get_weak(s) ((obs_weak_source_t *)(s)->control)

static inline obs_source_t *mock_source_create(void)
{
obs_source_t *s = calloc(1, sizeof(*s));
/* bzalloc 语义：refs/weak_refs 都从 0 开始。真实实现里
 * obs_ref_release() 判的是 os_atomic_dec_long(&refs) == -1，
 * 即"最后一份引用走了"对应 refs 从 0 减到 -1。 */
s->ref.refs = 0;
s->ref.weak_refs = 0;
s->control = (struct obs_weak_source *)(void *)s;
return s;
}

static inline obs_weak_source_t *obs_source_get_weak_source(obs_source_t *source)
{
if (!source)
return NULL;
get_weak(source)->ref.weak_refs++;
return get_weak(source);
}

static inline void obs_weak_source_release(obs_weak_source_t *weak)
{
if (!weak)
return;
if (--weak->ref.weak_refs < 0)
free(weak);
}

static inline obs_source_t *obs_weak_source_get_source(obs_weak_source_t *weak)
{
if (!weak)
return NULL;
if (weak->ref.refs < 0)
return NULL; /* 源已销毁：这正是回调要判的那条 */
weak->ref.refs++;
return weak->source ? weak->source : (obs_source_t *)(void *)weak;
}

static inline void obs_source_release(obs_source_t *source)
{
if (!source)
return;
if (--source->ref.refs < 0)
source->destroyed = true;
}

