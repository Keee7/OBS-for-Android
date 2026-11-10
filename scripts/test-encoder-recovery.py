#!/usr/bin/env python3
"""Compile production timing helpers and bitrate update with deterministic host stubs."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
core = (root / 'obs-studio/libobs/obs-encoder.c').read_text(encoding='utf-8')
timing = core[core.index('#define MAX_ENCODER_PACKET_TIMES'):core.index('struct obs_encoder_info *find_encoder')]
gpu = (root / 'obs-studio/libobs/obs-video-gpu-encode.c').read_text(encoding='utf-8')
# Both producers must use the bounded helper; the MediaCodec path is GPU-only.
for producer in (core, gpu):
    assert 'encoder_packet_time_push(encoder)' in producer
    assert 'da_push_back_new(encoder->encoder_packet_times)' not in producer.replace(timing, '')
plugin = (root / 'obs-studio/plugins/android-mediacodec/android-mediacodec.c').read_text(encoding='utf-8')
update = plugin[plugin.index('static bool update('):plugin.index('static void video_info(')]
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
struct encoder_packet_time { int64_t pts; uint64_t fer, ferc, cts; };
typedef struct { struct { struct encoder_packet_time array[8192]; size_t num; } encoder_packet_times; } obs_encoder_t;
#define da_push_back_new(a) (&(a).array[(a).num++])
#define da_erase_range(a,start,count) do { memmove((a).array+(start), (a).array+(start)+(count), ((a).num-(start)-(count))*sizeof((a).array[0])); (a).num-=(count); } while(0)
#define da_erase(a,i) da_erase_range(a,i,1)
typedef int obs_data_t;
typedef int AMediaFormat;
typedef int media_status_t;
#define AMEDIA_OK 0
#define LOG_WARNING 1
struct encoder { int mode; bool bitrate_update_disabled; int64_t bitrate; void *codec; };
static int result, calls;
static int64_t obs_data_get_int(obs_data_t *s,const char *key) {(void)key;return *s;}
static AMediaFormat *AMediaFormat_new(void) { static int f; return &f; }
static void AMediaFormat_delete(AMediaFormat *f) {(void)f;}
static void AMediaFormat_setInt32(AMediaFormat *f,const char *k,int32_t v) {(void)f;(void)k;(void)v;}
static media_status_t AMediaCodec_setParameters(void *c, AMediaFormat *f) {(void)c;(void)f;calls++;return result;}
#define mc_log(...) ((void)0)
'''
checks = r'''
int main(void) {
    static obs_encoder_t e;
    struct encoder_packet_time out;
    for(int i=0;i<4;i++) encoder_packet_time_push(&e)->pts=i;
    const int reordered[]={0,3,1,2};
    for(int i=0;i<4;i++) { assert(encoder_packet_time_take(&e,reordered[i],&out)); assert(out.pts==reordered[i]); }
    assert(e.encoder_packet_times.num==0);
    for(int i=0;i<100000;i++) { encoder_packet_time_push(&e)->pts=i; assert(e.encoder_packet_times.num<=MAX_ENCODER_PACKET_TIMES); }
    assert(encoder_packet_time_take(&e,99999,&out));
    puts("PASS: reordered B-frame timing survives; 100000 abandoned inputs keep bounded history");
    struct encoder mc={.mode=1,.bitrate=6000000};
    int kbps=3000;
    result=-10000;
    assert(!update(&mc,&kbps));
    assert(mc.bitrate==6000000 && mc.bitrate_update_disabled && calls==1);
    assert(!update(&mc,&kbps) && calls==1);
    struct encoder good={.mode=1,.bitrate=6000000};
    result=0;
    assert(update(&good,&kbps) && good.bitrate==3000000 && !good.bitrate_update_disabled);
    puts("PASS: rejected dynamic bitrate preserves accepted rate and disables repeated attempts; accepted update commits");
}
'''
with tempfile.TemporaryDirectory(prefix='encoder-recovery-') as tmp:
    src = Path(tmp) / 'regression.c'
    exe = Path(tmp) / 'regression.exe'
    src.write_text(code + timing + update + checks, encoding='utf-8')
    subprocess.run([os.environ.get('CC', 'gcc'), '-std=c11', '-Wall', '-Wextra', '-Werror', str(src), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
