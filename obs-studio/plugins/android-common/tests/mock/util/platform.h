#pragma once
#include <stdint.h>
#include <time.h>
static inline void os_sleep_ms(uint32_t ms)
{
struct timespec ts = {(time_t)(ms / 1000), (long)(ms % 1000) * 1000000L};
nanosleep(&ts, NULL);
}

/* libobs 的 bmem 原语：reader-ctx.h 只用到这两个。 */
#include <string.h>
static inline void *bzalloc(size_t n)
{
void *p = calloc(1, n ? n : 1);
if (!p)
abort();
return p;
}
static inline void bfree(void *p)
{
free(p);
}
