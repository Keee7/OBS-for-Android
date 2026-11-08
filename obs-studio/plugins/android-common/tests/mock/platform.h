#pragma once
#include <stdint.h>
#include <time.h>
static inline void os_sleep_ms(uint32_t ms)
{
struct timespec ts = {(time_t)(ms / 1000), (long)(ms % 1000) * 1000000L};
nanosleep(&ts, NULL);
}
