#pragma once
#include <stdbool.h>
#include <stdint.h>

static inline bool screen_gpu_should_fallback(bool failed, bool host, bool received,
                                               uint64_t opened_ns, uint64_t now_ns)
{
    return failed || (host && !received && now_ns >= opened_ns && now_ns - opened_ns > 10000000000ULL);
}

#define SCREEN_GPU_DEFAULT true
static inline uint32_t screen_gpu_retry_seconds(uint32_t failures)
{
    return failures >= 4 ? 30 : 2u << failures;
}
