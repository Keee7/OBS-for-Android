#include "gpu-fallback.h"
#include <assert.h>
#include <stdio.h>
int main(void)
{
    assert(SCREEN_GPU_DEFAULT);
    assert(screen_gpu_retry_seconds(0)==2);
    assert(screen_gpu_retry_seconds(1)==4);
    assert(screen_gpu_retry_seconds(3)==16);
    assert(screen_gpu_retry_seconds(4)==30);
    assert(screen_gpu_retry_seconds(100)==30);
    assert(!screen_gpu_should_fallback(false, false, false, 0, 20000000000ULL));
    assert(!screen_gpu_should_fallback(false, true, false, 1, 10000000001ULL));
    assert(screen_gpu_should_fallback(false, true, false, 1, 10000000002ULL));
    assert(!screen_gpu_should_fallback(false, true, true, 1, 99999999999ULL));
    assert(screen_gpu_should_fallback(true, true, true, 1, 2));
    assert(!screen_gpu_should_fallback(false, true, false, 20, 10));
    puts("GPU first-frame fallback checks passed");
}
