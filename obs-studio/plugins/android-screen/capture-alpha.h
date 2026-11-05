#pragma once

#include <stddef.h>
#include <stdint.h>

enum screen_alpha_mode {
	SCREEN_ALPHA_COMPAT = 0,
	SCREEN_ALPHA_STRAIGHT = 1,
	SCREEN_ALPHA_PREMULTIPLIED = 2,
};

/* Android premultiplied sRGB bytes -> straight sRGB bytes, as expected by
 * libobs async video. Preserve alpha and avoid division by zero. */
static inline void screen_alpha_unpremultiply(uint8_t *dst, const uint8_t *src, size_t pixels)
{
	for (size_t i = 0; i < pixels; ++i, dst += 4, src += 4) {
		unsigned a = src[3];
		for (int channel = 0; channel < 3; ++channel) {
			unsigned value = a ? (src[channel] * 255u + a / 2u) / a : 0;
			dst[channel] = (uint8_t)(value > 255u ? 255u : value);
		}
		dst[3] = (uint8_t)a;
	}
}
