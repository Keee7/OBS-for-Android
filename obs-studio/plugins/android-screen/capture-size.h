/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <stdint.h>
#include <stdbool.h>

/* Fit inside a landscape/portrait Full HD envelope, without upscaling. */
static inline void screen_limit_size(uint32_t *w, uint32_t *h, bool native)
{
	if (native || !*w || !*h)
		return;
	uint32_t max_w = *w >= *h ? 1920 : 1080;
	uint32_t max_h = *w >= *h ? 1080 : 1920;
	if (*w <= max_w && *h <= max_h)
		return;
	uint32_t num, den;
	if ((uint64_t)max_w * *h <= (uint64_t)max_h * *w) {
		num = max_w;
		den = *w;
	} else {
		num = max_h;
		den = *h;
	}
	*w = (uint32_t)((uint64_t)*w * num / den) & ~1u;
	*h = (uint32_t)((uint64_t)*h * num / den) & ~1u;
	if (!*w) *w = 2;
	if (!*h) *h = 2;
}
