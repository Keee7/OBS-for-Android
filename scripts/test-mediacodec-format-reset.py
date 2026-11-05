from pathlib import Path
import subprocess, tempfile, sys
if len(sys.argv) != 3:
    raise SystemExit('usage: python test-mediacodec-format-reset.py FFMPEG_SOURCE GCC')
src = (Path(sys.argv[1]) / 'libavcodec/mediacodecdec_common.c').read_text()
macro=src[src.index('#define AMEDIAFORMAT_GET_INT32'):src.index('static int mediacodec_dec_parse_video_format')]
reset=src[src.index('    /* Optional keys describe this format only.'):src.index('    /* Mandatory fields */',src.index('    /* Optional keys describe this format only.'))]
layout=src[src.index('    AMEDIAFORMAT_GET_INT32(s->width,  "width", 1);'):src.index('    AMEDIAFORMAT_GET_INT32(s->color_format',src.index('    AMEDIAFORMAT_GET_INT32(s->width,  "width", 1);'))]
crop=src[src.index('    AMEDIAFORMAT_GET_INT32(s->crop_top,'):src.index('    AMEDIAFORMAT_GET_INT32(s->display_width,')]
code=r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#define AV_LOG_ERROR 0
#define AVERROR_EXTERNAL (-1)
#define FFALIGN(x,a) (((x)+(a)-1)&~((a)-1))
#define av_log(...) ((void)0)
typedef struct { int width,height; } AVCodecContext;
typedef struct { int width,height,stride,slice_height,crop_top,crop_bottom,crop_left,crop_right,display_width,display_height,use_ndk_codec; const char *codec_name; void *format; } MediaCodecDecContext;
typedef struct { int w,h,old; } Format;
static int ff_AMediaFormat_getInt32(void *p,const char *key,int32_t *v) {
 Format *f=p;
 if (!strcmp(key,"width")) *v=f->w;
 else if (!strcmp(key,"height")) *v=f->h;
 else if(f->old && !strcmp(key,"stride")) *v=192;
 else if(f->old && !strcmp(key,"slice-height")) *v=144;
 else if(f->old && !strcmp(key,"crop-right")) *v=175;
 else if(f->old && !strcmp(key,"crop-bottom")) *v=143;
 else return 0;
 return 1;
}
static int ff_AMediaFormat_getRect(void *f,const char *k,int *l,int *t,int *r,int *b) { return 0; }
''' + macro + r'''
static int parse(MediaCodecDecContext *s) {
 AVCodecContext av={0}, *avctx=&av; int ret=0,width=0,height=0; char *format="stub";
''' + reset + layout + crop + r'''
 assert(width == (s->width == 176 ? 176 : 3840));
 assert(height == (s->height == 144 ? 144 : 2160));
 return 0;
fail: return ret;
}
int main(void) {
 Format old={176,144,1}, next={3840,2160,0};
 MediaCodecDecContext s={0}; s.codec_name="c2.xring.avc.decoder"; s.use_ndk_codec=1; s.format=&old;
 assert(parse(&s)==0); assert(s.stride==192 && s.slice_height==144 && s.crop_right==175 && s.crop_bottom==143);
 s.format=&next; assert(parse(&s)==0); assert(s.stride==3840 && s.slice_height==2160 && !s.crop_right && !s.crop_bottom);
 puts("PASS: output format change resets crop and layout");
}
'''
with tempfile.TemporaryDirectory(dir=Path(__file__).parent) as d:
 p=Path(d); (p/'test.c').write_text(code)
 subprocess.run([sys.argv[2],str(p/'test.c'),'-o',str(p/'test.exe')],check=True)
 subprocess.run([str(p/'test.exe')],check=True)
