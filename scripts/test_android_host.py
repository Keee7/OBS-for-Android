#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Host regressions for UVC and Android image document I/O."""
import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
UVC_SOURCE = ROOT / 'obs-studio/plugins/android-capture/uvc-input.c'


def source_function(text, name):
    match = re.search(r'(?m)^static[^;{}]*\b' + re.escape(name) + r'\([^;{}]*\)\s*\{', text)
    if not match:
        raise RuntimeError('Missing production function: ' + name)
    start = match.start()
    end = text.index('{', start) + 1
    depth = 1
    while depth:
        if text[end] == '{':
            depth += 1
        elif text[end] == '}':
            depth -= 1
        end += 1
    return text[start:end]


def run_uvc_case(compiler, name, code):
    with tempfile.TemporaryDirectory(prefix=f'{name}-') as directory:
        work = Path(directory)
        source = work / f'{name}.c'
        binary = work / (f'{name}.exe' if os.name == 'nt' else name)
        source.write_text(code, encoding='utf-8')
        subprocess.run([compiler, '-std=c11', '-Wall', '-Wextra', '-Werror',
                        str(source), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


def test_uvc_policy(compiler, source):
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
    prefix += (UVC_SOURCE.parent / 'uvc-cadence-stats.h').read_text(encoding='utf-8-sig').replace('#pragma once', '')
    structures = r'''
struct uvc_source {
 int width,height,fps,prefer_fmt;
 bool yuv_low_latency;
 char fmt_setting[16];
 struct obs_source_frame yuv_frame;
 struct uvc_cadence_stats cadence;
 uint64_t last_frame_timestamp,frames_dropped;
 void *source;
};
struct uvc_dev {
 struct uvc_mode modes[64];
 int n_modes,eff_fmt,eff_w,eff_h,eff_fps;
 uint32_t quirks,frame_interval,frame_buf_size,payload_size,clock_frequency;
};
'''
    body = '\n'.join(source_function(source, name) for name in (
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
 assert(d.frame_buf_size==4147200&&d.payload_size==32768&&d.frame_interval==166666&&d.clock_frequency==48000000);
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
    run_uvc_case(compiler, 'uvc-policy', prefix + mode + structures + body + suffix)


def test_uvc_negotiation(compiler, source):
    start = source.index("static int probe_struct_len(")
    end = source.index("/* ---------------- 帧送出", start)
    production = source[start:end]
    prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define LIBUSB_ERROR_IO -1
#define LIBUSB_ERROR_INVALID_PARAM -2
#define LIBUSB_ERROR_ACCESS -3
#define LIBUSB_ERROR_NO_DEVICE -4
#define LIBUSB_ERROR_NOT_SUPPORTED -12
#define LIBUSB_ERROR_TIMEOUT -7
#define LIBUSB_ERROR_PIPE -9
#define UVC_GET_LEN 0x85
#define UVC_GET_DEF 0x87
#define UVC_GET_CUR 0x81
#define UVC_SET_CUR 1
#define UVC_VS_PROBE_CONTROL 1
#define UVC_VS_COMMIT_CONTROL 2
#define LOG_INFO 1
#define LOG_WARNING 2
#define LOG_ERROR 3
enum {MODE_MJPEG, MODE_YUYV};
struct uvc_mode { int kind, n_intervals; uint8_t fmt_index, frame_index; uint32_t max_frame_size, def_interval; unsigned w,h; };
struct uvc_dev { void *h; int vs_iface, vs_alt, chosen, eff_fps; uint32_t uvc_version, probe_len, payload_size, frame_interval, frame_buf_size, clock_frequency; struct uvc_mode modes[1]; };
struct uvc_source { bool raw; int fps; };
static int calls, alt_calls, fail_call, fail_code, alt_fail_call, alt_fail_code, get_error, short_read, wrong_mode;
static void blog(int l, const char *fmt, ...) {(void)l;(void)fmt;}
static void os_sleep_ms(int n) {(void)n;}
static uint16_t get_u16le(const uint8_t *p) {return (uint16_t)(p[0]|p[1]<<8);}
static uint32_t get_u32le(const uint8_t *p) {return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static void put_u16le(uint8_t *p,uint16_t n) {p[0]=(uint8_t)n;p[1]=(uint8_t)(n>>8);}
static void put_u32le(uint8_t *p,uint32_t n) {p[0]=(uint8_t)n;p[1]=(uint8_t)(n>>8);p[2]=(uint8_t)(n>>16);p[3]=(uint8_t)(n>>24);}
static uint32_t pick_interval(const struct uvc_mode *m,int fps) {(void)m;(void)fps;return 166666;}
static uint32_t raw_frame_bytes(const struct uvc_mode *m) {(void)m;return 16;}
static bool raw_low_latency(const struct uvc_source *s) {return s->raw;}
static bool interval_matches(uint32_t n,int fps) {(void)fps;return n==166666;}
static const char *mode_kind_name(int n) {(void)n;return "test";}
static int libusb_control_transfer(void *h, uint8_t type, uint8_t req, uint16_t val, uint16_t index, uint8_t *p, uint16_t len, unsigned timeout) {
 (void)h;(void)type;(void)val;(void)index;(void)timeout; calls++;
 if(calls==fail_call) return fail_code;
 if(req==UVC_GET_CUR && get_error) return get_error;
 if(req==UVC_GET_CUR && short_read) return 5;
 if(req==UVC_GET_LEN) {p[0]=26;p[1]=0;return 2;}
 if(req==UVC_GET_DEF || req==UVC_GET_CUR) {memset(p,0,len);p[2]=(req==UVC_GET_CUR && wrong_mode)?2:1;p[3]=1;put_u32le(p+4,166666);}
 return len;
}
static int libusb_set_interface_alt_setting(void *h,int iface,int alt) {(void)h;(void)iface;(void)alt;alt_calls++;return alt_calls==alt_fail_call?alt_fail_code:0;}
'''
    suffix = r'''
static void reset(void) {calls=alt_calls=fail_call=fail_code=alt_fail_call=alt_fail_code=get_error=short_read=wrong_mode=0;}
int main(void) {
 struct uvc_dev d={.vs_iface=1,.vs_alt=1,.eff_fps=60,.uvc_version=0x100,.modes={{.kind=MODE_YUYV,.fmt_index=1,.frame_index=1,.def_interval=166666,.w=4,.h=2}}};
 struct uvc_source s={.raw=true,.fps=60};
 for(int point=1;point<=6;point++) {reset();fail_call=point;fail_code=LIBUSB_ERROR_NO_DEVICE;assert(uvc_negotiate(&d,&s)==LIBUSB_ERROR_NO_DEVICE);assert(calls==point && alt_calls==0);}
 reset();fail_call=3;fail_code=LIBUSB_ERROR_INVALID_PARAM;assert(uvc_negotiate(&d,&s)==LIBUSB_ERROR_INVALID_PARAM && calls==3 && alt_calls==0);
 reset();fail_call=3;fail_code=LIBUSB_ERROR_ACCESS;assert(uvc_negotiate(&d,&s)==LIBUSB_ERROR_ACCESS && calls==3 && alt_calls==0);
 reset();fail_call=3;fail_code=LIBUSB_ERROR_NOT_SUPPORTED;assert(uvc_negotiate(&d,&s)==LIBUSB_ERROR_NOT_SUPPORTED && calls==3 && alt_calls==0);
 reset();wrong_mode=1;assert(uvc_negotiate(&d,&s)==LIBUSB_ERROR_NOT_SUPPORTED && calls==4 && alt_calls==0);
 reset();get_error=LIBUSB_ERROR_TIMEOUT;assert(negotiate_try(&d,&s)==LIBUSB_ERROR_TIMEOUT && calls==6);
 reset();short_read=1;assert(negotiate_try(&d,&s)==LIBUSB_ERROR_IO && calls==6);
 reset();fail_call=3;fail_code=LIBUSB_ERROR_IO;alt_fail_call=1;alt_fail_code=LIBUSB_ERROR_NO_DEVICE;assert(uvc_negotiate(&d,&s)==LIBUSB_ERROR_NO_DEVICE && calls==3 && alt_calls==1);
 reset();fail_call=3;fail_code=LIBUSB_ERROR_PIPE;alt_fail_call=2;alt_fail_code=LIBUSB_ERROR_ACCESS;assert(uvc_negotiate(&d,&s)==LIBUSB_ERROR_ACCESS && calls==3 && alt_calls==2);
 reset();fail_call=3;fail_code=LIBUSB_ERROR_IO;assert(uvc_negotiate(&d,&s)==0 && alt_calls==2 && calls==9);
 reset();s.raw=false;get_error=LIBUSB_ERROR_PIPE;assert(negotiate_try(&d,&s)==0 && calls==8);
 puts("uvc negotiation tests passed");return 0;
}
'''
    run_uvc_case(compiler, 'uvc-negotiation', prefix + production + suffix)


IMAGE_PREFIX = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/mem.h>
#include <libswscale/swscale.h>
#define __ANDROID__ 1
#define LOG_WARNING 1
#define blog(...) ((void)0)
#ifdef _WIN32
#define astrcmpi _stricmp
#else
#include <strings.h>
#define astrcmpi strcasecmp
#define O_BINARY 0
#endif
enum gs_image_alpha_mode { GS_IMAGE_ALPHA_STRAIGHT };
static const char *fixture;
static int owned_fd = -1;
static bool deny_document, fail_buffer;
static int obs_android_storage_open(const char *uri, const char *mode)
{
    assert((strncmp(uri, "content://", 10) == 0 || strncmp(uri, "file://", 7) == 0) && strcmp(mode, "r") == 0);
    if (deny_document) { errno = EACCES; return -1; }
    assert(owned_fd == -1);
    owned_fd = open(fixture, O_RDONLY | O_BINARY);
    assert(owned_fd >= 0);
    return owned_fd;
}
static int tracked_close(int fd)
{
    assert(fd >= 0 && fd == owned_fd);
    owned_fd = -1;
    return close(fd);
}
static void *image_malloc(size_t size) { return fail_buffer ? NULL : av_malloc(size); }
static FILE *os_fopen(const char *path, const char *mode)
{
    if (strncmp(path, "content://", 10) != 0 && strncmp(path, "file://", 7) != 0) return fopen(path, mode);
    int fd = obs_android_storage_open(path, "r");
    if (fd < 0) return NULL;
    FILE *file = fdopen(fd, mode);
    if (!file) tracked_close(fd);
    return file;
}
static int tracked_fclose(FILE *file)
{
    assert(fileno(file) == owned_fd);
    owned_fd = -1;
    return fclose(file);
}
#define close tracked_close
#define fclose tracked_fclose
'''

IMAGE_REFORMAT = r'''
static uint8_t first_pixel[4];
static void *ffmpeg_image_reformat_frame(struct ffmpeg_image *info, AVFrame *frame,
                                        enum gs_image_alpha_mode mode)
{
    (void)mode;
    assert(frame->width == info->cx && frame->height == info->cy);
    uint8_t *data = malloc((size_t)info->cx * info->cy * 4);
    assert(data);
    struct SwsContext *sws = sws_getContext(info->cx, info->cy, frame->format,
        info->cx, info->cy, AV_PIX_FMT_RGBA, SWS_POINT, NULL, NULL, NULL);
    assert(sws);
    uint8_t *dest[4] = {data};
    int linesize[4] = {info->cx * 4};
    assert(sws_scale(sws, (const uint8_t *const *)frame->data, frame->linesize,
                     0, info->cy, dest, linesize) == info->cy);
    sws_freeContext(sws);
    memcpy(first_pixel, data, 4);
    return data;
}
'''

IMAGE_CHECKS = r'''
int main(int argc, char **argv)
{
    assert(argc == 3);
    fixture = argv[1];
    const bool gif = strcmp(argv[2], "gif") == 0;
    const bool valid = strcmp(argv[2], "invalid") != 0;
    const char *uri = "content://fixture/document/42%3Aopaque?name=wrong.bin";
    struct ffmpeg_image image;
    if (valid) {
        assert(image_file_is_gif(uri) == gif);
        assert(owned_fd == -1);
        assert(ffmpeg_image_init(&image, uri));
        assert(image.document_io && image.document_fd >= 0);
        assert(image_document_seek(&image, 0, AVSEEK_SIZE) > 0);
        assert(image_document_seek(&image, 0, AVSEEK_SIZE | AVSEEK_FORCE) > 0);
        void *data = ffmpeg_image_decode(&image, GS_IMAGE_ALPHA_STRAIGHT);
        assert(data && image.cx == 16 && image.cy == 12);
        if (strcmp(argv[2], "png") == 0) {
            assert(first_pixel[0] == 12 && first_pixel[1] == 34 && first_pixel[2] == 56 && first_pixel[3] == 128);
        }
        free(data);
        ffmpeg_image_free(&image);
        assert(owned_fd == -1 && image.document_fd == -1 && !image.document_io);
        ffmpeg_image_free(&image); /* Idempotent ownership cleanup. */
        assert(ffmpeg_image_init(&image, fixture));
        data = ffmpeg_image_decode(&image, GS_IMAGE_ALPHA_STRAIGHT);
        assert(data);
        free(data);
        ffmpeg_image_free(&image);
        assert(owned_fd == -1);
        assert(ffmpeg_image_init(&image, "file:///fixture/image%20with%20spaces.png"));
        data = ffmpeg_image_decode(&image, GS_IMAGE_ALPHA_STRAIGHT);
        assert(data);
        free(data);
        ffmpeg_image_free(&image);
        assert(owned_fd == -1);
        fail_buffer = true;
        assert(!ffmpeg_image_init(&image, uri));
        assert(owned_fd == -1 && image.document_fd == -1);
        fail_buffer = false;
        deny_document = true;
        assert(!ffmpeg_image_init(&image, uri));
        assert(owned_fd == -1);
    } else {
        bool opened = ffmpeg_image_init(&image, uri);
        if (opened) {
            void *data = ffmpeg_image_decode(&image, GS_IMAGE_ALPHA_STRAIGHT);
            free(data);
            ffmpeg_image_free(&image);
        }
        assert(owned_fd == -1);
    }
    puts("PASS: image document/local decode, EOF/failure cleanup and descriptor ownership");
}
'''


def test_image_document(compiler, ffmpeg_prefix):
    from PIL import Image

    source = (ROOT / 'obs-studio/libobs/graphics/graphics-ffmpeg.c').read_text(encoding='utf-8')
    images = (ROOT / 'obs-studio/libobs/graphics/image-file.c').read_text(encoding='utf-8')
    struct_start = source.index('struct ffmpeg_image {')
    struct_end = source.index('};', struct_start) + 2
    code = IMAGE_PREFIX + source[struct_start:struct_end]
    for name in ('image_document_read', 'image_document_seek', 'ffmpeg_image_open_decoder_context',
                 'ffmpeg_image_free', 'ffmpeg_image_init'):
        code += '\n' + source_function(source, name).replace('av_malloc(', 'image_malloc(')
    code += '\n' + source_function(images, 'image_file_is_gif') + IMAGE_REFORMAT
    code += '\n' + source_function(source, 'ffmpeg_image_decode') + IMAGE_CHECKS
    with tempfile.TemporaryDirectory(prefix='image-document-') as tmp:
        path = Path(tmp)
        cfile = path / 'image-test.c'
        cfile.write_text(code, encoding='utf-8')
        exe = path / 'image-test.exe'
        subprocess.run([compiler, '-std=c11', '-D_POSIX_C_SOURCE=200809L', '-Wall', '-Wextra', '-Werror',
                        str(cfile), '-I' + str(ffmpeg_prefix / 'include'),
                        '-L' + str(ffmpeg_prefix / 'lib'), '-lavformat', '-lavcodec', '-lavutil', '-lswscale',
                        '-o', str(exe)], check=True)
        env = os.environ.copy()
        binary_dirs = [str(ffmpeg_prefix.resolve() / 'bin')]
        if os.name == 'nt':
            binary_dirs.append('C:/msys64/ucrt64/bin')
        env['PATH'] = os.pathsep.join(binary_dirs + [env['PATH']])
        rgba = Image.new('RGBA', (16, 12), (12, 34, 56, 128))
        rgba.save(path / 'fixture.png')
        rgba.convert('RGB').save(path / 'fixture.jpg')
        rgba.convert('RGB').save(path / 'fixture.gif', save_all=True,
                                append_images=[Image.new('RGB', rgba.size, (80, 90, 100))], duration=50, loop=0)
        (path / 'invalid.bin').write_bytes(b'not an image')
        raw = (path / 'fixture.png').read_bytes()
        (path / 'truncated.png').write_bytes(raw[:len(raw) // 2])
        for name, kind in (('fixture.png', 'png'), ('fixture.jpg', 'jpeg'), ('fixture.gif', 'gif'),
                           ('invalid.bin', 'invalid'), ('truncated.png', 'invalid')):
            subprocess.run([str(exe), str(path / name), kind], env=env, check=True, timeout=15)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    cases = parser.add_subparsers(dest='case', required=True)

    uvc = cases.add_parser('uvc', help='UVC mode selection, raw-frame delivery and negotiation')
    uvc.add_argument('--compiler', default=os.environ.get('CC', 'cc'))

    images = cases.add_parser('image-document', help='Android document I/O and decode with host FFmpeg and Pillow')
    images.add_argument('--ffmpeg-prefix', required=True, type=Path)
    images.add_argument('--compiler', default=os.environ.get('CC', 'cc'))

    args = parser.parse_args()
    if args.case == 'uvc':
        source = UVC_SOURCE.read_text(encoding='utf-8')
        test_uvc_policy(args.compiler, source)
        test_uvc_negotiation(args.compiler, source)
    else:
        test_image_document(args.compiler, args.ffmpeg_prefix)


if __name__ == '__main__':
    main()
