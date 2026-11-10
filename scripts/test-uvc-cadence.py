#!/usr/bin/env python3
"""Exercise the real OBS async-frame selector with equal-rate, jittered input."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'obs-studio/libobs/obs-source.c').read_text(encoding='utf-8')
start = source.index('static bool ready_async_frame(obs_source_t *source, uint64_t sys_time)\n{')
end = source.index('/*\n * Ensures that cached frames', start)
production = source[start:end]
prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#define DEBUG_ASYNC_FRAMES 0
#define MAX_TS_VAR 2000000000ULL
struct obs_source_frame {uint64_t timestamp; int id;};
typedef struct { struct {struct obs_source_frame *array[16]; size_t num;} async_frames; uint64_t last_frame_ts,last_sys_timestamp; bool async_unbuffered; } obs_source_t;
#define da_erase(a,i) do {memmove((a).array+(i),(a).array+(i)+1,((a).num-(i)-1)*sizeof((a).array[0]));(a).num--;}while(0)
static void remove_async_frame(obs_source_t *s, struct obs_source_frame *f) {(void)s;(void)f;}
static bool frame_out_of_bounds(obs_source_t *s,uint64_t t) {(void)s;(void)t;return false;}
'''
suffix = r'''
static int simulate(bool unbuffered) {
    obs_source_t s={.async_unbuffered=unbuffered};
    static struct obs_source_frame frames[10000];
    const uint64_t period=16666667, base=1000000000;
    for(int i=0;i<10000;i++) frames[i]=(struct obs_source_frame){base+i*period+(i%2)*4000000ULL,i};
    int input=0,unique=0,last=-1;
    for(int i=0;i<10000;i++) {
        uint64_t now=base+i*period+2000000ULL;
        while(input<10000 && frames[input].timestamp<=now) {
            assert(s.async_frames.num<16);
            s.async_frames.array[s.async_frames.num++]=&frames[input++];
        }
        struct obs_source_frame *f=get_closest_frame(&s,now);
        if(f && f->id!=last) {unique++;last=f->id;}
        s.last_sys_timestamp=now;
    }
    return unique;
}
int main(void) {
    int latest=simulate(true),smooth=simulate(false);
    printf("60fps input/output, alternating 4ms arrival jitter: latest=%d/10000, buffered=%d/10000 distinct frame IDs\n",latest,smooth);
    assert(latest<6000 && smooth>9900);
}
'''
with tempfile.TemporaryDirectory(prefix='uvc-cadence-') as tmp:
    src=Path(tmp)/'cadence.c';exe=Path(tmp)/'cadence.exe'
    src.write_text(prefix+production+suffix,encoding='utf-8')
    subprocess.run([os.environ.get('CC','gcc'),'-std=c11','-Wall','-Wextra','-Werror',str(src),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
