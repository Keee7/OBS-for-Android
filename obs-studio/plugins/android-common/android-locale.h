#pragma once

#include <util/dstr.h>

static inline void obs_android_format_locale(struct dstr *out, const char *text, const char *const *args, size_t nargs)
{
	dstr_init(out);
	for (const char *p = text; *p;) {
		if (p[0] == '%' && p[1] >= '1' && p[1] <= '9') {
			const size_t index = (size_t)(p[1] - '1');
			if (index < nargs && args[index]) {
				dstr_cat(out, args[index]);
				p += 2;
				continue;
			}
		}
		dstr_cat_ch(out, *p++);
	}
}

static inline void obs_android_copy_utf8(char *out, size_t capacity, const char *text)
{
	if (!capacity)
		return;

	size_t length = strlen(text);
	if (length >= capacity) {
		length = capacity - 1;
		// Leave a complete prefix if the limit falls inside a UTF-8 character.
		while (length && ((unsigned char)text[length] & 0xC0) == 0x80)
			length--;
	}
	memcpy(out, text, length);
	out[length] = '\0';
}
