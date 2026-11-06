#!/usr/bin/env python3
"""Compile production UVC mode, PROBE layout and raw frame-delivery functions."""
import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'obs-studio/plugins/android-capture/uvc-input.c'


def function(source, name):
    match = re.search(r'(?m)^static[^;{}]*\b' + name + r'\([^;{}]*\)\s*\{', source)
    if not match:
        raise RuntimeError('Missing production function ' + name)
    depth = 1
    pos = source.index('{', match.start()) + 1
    while depth:
        if source[pos] == '{':
            depth += 1
        elif source[pos] == '}':
            depth -= 1
        pos += 1
    return source[match.start():pos]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compiler', default=os.environ.get('CC', 'cc'))
    args = parser.parse_args()
    source = SOURCE.read_text(encoding='utf-8')
    mode = source[source.index('enum uvc_mode_kind {'):source.index('struct uvc_source {')]
    prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#define QUIRK_PREFER_YUYV 1
#define UNUSED_PARAMETER(x) ((void)(x))
#define blog(...) ((void)0)
#define VIDEO_CS_709 709
#define VIDEO_RANGE_PARTIAL 0
enum video_format { VIDEO_FORMAT_NONE, VIDEO_FORMAT_YUY2, VIDEO_FORMAT_NV12 };
struct obs_source_frame {
 enum video_format format;
 uint32_t width,height,linesize[8];
 uint8_t *data[8];
 uint64_t timestamp;
 bool full_range;
 float color_matrix[16],color_range_min[3],color_range_max[3];
};
static int matrix_calls,output_calls;
static struct obs_source_frame delivered;
static uint64_t os_gettime_ns(void) { return 100; }
static void video_format_get_parameters_for_format(int cs,int range,enum video_format fmt,float *matrix,float *mn,float *mx)
 { (void)fmt;(void)mn;(void)mx;assert(cs==709&&range==0);matrix_calls++;matrix[0]=709; }
static void obs_source_output_video(void *source,struct obs_source_frame *out) { (void)source;output_calls++;delivered=*out; }
'''
    structures = r'''
struct uvc_source {
 int width,height,fps,prefer_fmt;
 bool yuv_low_latency;
 char fmt_setting[16];
 struct obs_source_frame yuv_frame;
 uint64_t last_frame_timestamp,frames_dropped;
 void *source;
};
struct uvc_dev {
 struct uvc_mode modes[64];
 int n_modes,eff_fmt,eff_w,eff_h,eff_fps;
 uint32_t quirks,frame_interval,frame_buf_size,payload_size;
};
'''
    body = '\n'.join(function(source, name) for name in (
        'get_u32le', 'raw_frame_bytes', 'pick_interval', 'raw_low_latency',
        'interval_matches', 'mode_score', 'choose_mode', 'probe_readback',
        'frame_timestamp', 'push_frame_yuv'))
    suffix = r'''
int main(void) {
 struct uvc_source s={.width=1920,.height=1080,.fps=60,.prefer_fmt=MODE_YUYV,.yuv_low_latency=true};
 struct uvc_dev d={.n_modes=3,.eff_fmt=MODE_YUYV,.eff_w=1920,.eff_h=1080,.eff_fps=60};
 d.modes[0]=(struct uvc_mode){.kind=MODE_YUYV,.w=1920,.h=1080,.n_intervals=1,.intervals={333333}};
 d.modes[1]=(struct uvc_mode){.kind=MODE_MJPEG,.w=1920,.h=1080,.n_intervals=1,.intervals={166666}};
 d.modes[2]=(struct uvc_mode){.kind=MODE_NV12,.w=1920,.h=1080,.n_intervals=1,.intervals={166833}};
 assert(choose_mode(&d,&s)==-1);
 s.yuv_low_latency=false; assert(choose_mode(&d,&s)==1);
 s.yuv_low_latency=true;d.modes[0].intervals[0]=166666;assert(choose_mode(&d,&s)==0);
 s.prefer_fmt=MODE_NV12;assert(choose_mode(&d,&s)==2);
 d.quirks=QUIRK_PREFER_YUYV;assert(choose_mode(&d,&s)==-1);
 s.prefer_fmt=MODE_YUYV;assert(choose_mode(&d,&s)==0);
 d.modes[0].w=1280;assert(choose_mode(&d,&s)==-1);
 assert(interval_matches(166833,60)&&!interval_matches(333333,60)&&!interval_matches(0,60));
 uint8_t probe[34]={0};
 probe[4]=0x0a;probe[5]=0x8b;probe[6]=0x02;
 probe[18]=0;probe[19]=0x48;probe[20]=0x3f;
 probe[22]=0;probe[23]=0x80;
 probe[26]=0;probe[27]=0x6c;probe[28]=0xdc;probe[29]=2;
 probe_readback(&d,probe,sizeof(probe));
 assert(d.frame_buf_size==4147200&&d.payload_size==32768&&d.frame_interval==166666);
 uint8_t *pixels=malloc(1920U*1080U*2U);assert(pixels);
 struct uvc_mode m={.kind=MODE_YUYV,.w=1920,.h=1080};
 assert(push_frame_yuv(&s,&m,pixels,4147200,200));
 assert(delivered.format==VIDEO_FORMAT_YUY2&&delivered.linesize[0]==3840&&!delivered.full_range);
 assert(delivered.data[0]==pixels&&delivered.timestamp==200&&matrix_calls==1);
 assert(push_frame_yuv(&s,&m,pixels,4147200,200)&&delivered.timestamp==201&&matrix_calls==1);
 m.kind=MODE_NV12;
 assert(push_frame_yuv(&s,&m,pixels,3110400,190));
 assert(delivered.format==VIDEO_FORMAT_NV12&&delivered.data[1]==pixels+2073600);
 assert(delivered.linesize[0]==1920&&delivered.linesize[1]==1920&&delivered.timestamp==202&&matrix_calls==2);
 assert(!push_frame_yuv(&s,&m,pixels,3110399,300)&&output_calls==3&&s.frames_dropped==1);
 free(pixels);
 puts("PASS: production raw mode selection, 59.94/60 tolerance, PROBE offsets, YUYV/NV12 planes and monotonic arrival timestamps");
 return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix='uvc-policy-') as tmp:
        path = Path(tmp)
        cfile = path / 'uvc-policy.c'
        binary = path / ('uvc-policy.exe' if os.name == 'nt' else 'uvc-policy')
        cfile.write_text(prefix + mode + structures + body + suffix, encoding='utf-8')
        subprocess.run([args.compiler, '-std=c11', '-Wall', '-Wextra', '-Werror',
                        str(cfile), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    main()
