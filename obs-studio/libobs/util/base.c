/*
 * Copyright (c) 2023 Lain Bailey <lain@obsproject.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <stdio.h>
#include <stdlib.h>

#include "c99defs.h"
#include "base.h"
#include "threading.h"

static int crashing = 0;
static void *log_param = NULL;
static void *crash_param = NULL;

#ifdef __ANDROID__
#include <android/log.h>
#include <string.h>

static void android_log_write_chunked(int prio, const char *tag, const char *msg)
{
	static const size_t max_chunk = 3800;
	const char *p = msg;
	char buf[3801];

	if (!p || !*p) {
		__android_log_write(prio, tag, " ");
		return;
	}

	while (*p) {
		const char *nl = strchr(p, '\n');
		size_t line_len = nl ? (size_t)(nl - p) : strlen(p);

		if (line_len == 0) {
			/* Skip lone trailing newline; preserve embedded blank lines as a single space */
			if (!nl || !nl[1])
				break;
			__android_log_write(prio, tag, " ");
			p = nl + 1;
			continue;
		}

		size_t offset = 0;
		while (offset < line_len) {
			size_t chunk = line_len - offset;
			if (chunk > max_chunk) {
				chunk = max_chunk;
				while (chunk > 0 && ((unsigned char)p[offset + chunk] & 0xC0) == 0x80)
					chunk--;
				if (chunk == 0)
					chunk = max_chunk;
			}

			memcpy(buf, p + offset, chunk);
			buf[chunk] = '\0';
			__android_log_write(prio, tag, buf);
			offset += chunk;
		}

		if (!nl || !nl[1])
			break;
		p = nl + 1;
	}
}
#endif

static void def_log_handler(int log_level, const char *format, va_list args, void *param)
{
	char out[8192];
	if (vsnprintf(out, sizeof(out), format, args) < 0)
		out[0] = '\0';

#ifdef __ANDROID__
	int prio = ANDROID_LOG_INFO;
#endif
	switch (log_level) {
	case LOG_DEBUG:
#ifdef __ANDROID__
		prio = ANDROID_LOG_DEBUG;
#endif
		fprintf(stdout, "debug: %s\n", out);
		fflush(stdout);
		break;

	case LOG_INFO:
#ifdef __ANDROID__
		prio = ANDROID_LOG_INFO;
#endif
		fprintf(stdout, "info: %s\n", out);
		fflush(stdout);
		break;

	case LOG_WARNING:
#ifdef __ANDROID__
		prio = ANDROID_LOG_WARN;
#endif
		fprintf(stdout, "warning: %s\n", out);
		fflush(stdout);
		break;

	case LOG_ERROR:
#ifdef __ANDROID__
		prio = ANDROID_LOG_ERROR;
#endif
		fprintf(stderr, "error: %s\n", out);
		fflush(stderr);
		break;

	default:
		break;
	}

#ifdef __ANDROID__
	android_log_write_chunked(prio, "OBS", out);
#endif

	UNUSED_PARAMETER(param);
}

OBS_NORETURN static void def_crash_handler(const char *format, va_list args, void *param)
{
	vfprintf(stderr, format, args);
	exit(0);

	UNUSED_PARAMETER(param);
}

static log_handler_t log_handler = def_log_handler;
static void (*crash_handler)(const char *, va_list, void *) = def_crash_handler;

void base_get_log_handler(log_handler_t *handler, void **param)
{
	if (handler)
		*handler = log_handler;
	if (param)
		*param = log_param;
}

void base_set_log_handler(log_handler_t handler, void *param)
{
	if (!handler)
		handler = def_log_handler;

	log_param = param;
	log_handler = handler;
}

void base_set_crash_handler(void (*handler)(const char *, va_list, void *), void *param)
{
	static bool non_default_handler_set = false;

	if (os_atomic_exchange_bool(&non_default_handler_set, true)) {
		blog(LOG_WARNING, "Tried to set a crash handler when one already exists.");
		return;
	}

	crash_param = param;
	crash_handler = handler;
}

OBS_NORETURN void bcrash(const char *format, ...)
{
	va_list args;

	if (crashing) {
		fputs("Crashed in the crash handler", stderr);
		exit(2);
	}

	crashing = 1;
	va_start(args, format);
	crash_handler(format, args, crash_param);
	va_end(args);
	exit(0);
}

void blogva(int log_level, const char *format, va_list args)
{
	log_handler(log_level, format, args, log_param);
}

void blog(int log_level, const char *format, ...)
{
	va_list args;

	va_start(args, format);
	blogva(log_level, format, args);
	va_end(args);
}
