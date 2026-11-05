"""Exercise the actual media scaling cache update with changing frame geometry."""
from pathlib import Path
import subprocess, tempfile
source = Path('obs-studio/shared/media-playback/media-playback/media.c').read_text(encoding='utf-8')
a = source.index('static bool mp_media_update_scaling(')
b = source.index('\nbool mp_media_prepare_frames(', a)
prefix = r'''
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
typedef struct {int width,height,format,colorspace,color_range;} AVFrame;
typedef struct {struct {AVFrame *frame;} v; bool scale_initialized; int scale_input_width,scale_input_height,scale_input_format,scale_input_colorspace,scale_input_range,scale_format; void *swscale; unsigned char *scale_pic[4]; int scale_linesizes[4];} mp_media_t;
static int builds, alloc_width, alloc_height, fail;
static void sws_freeContext(void *p) {free(p);}
static void av_freep(void *p) {void **x=p; free(*x); *x=NULL;}
static int closest_format(int fmt) {return fmt==1 ? 2 : fmt;}
static bool mp_media_init_scaling(mp_media_t *m) { if(fail) return false; builds++; alloc_width=m->v.frame->width; alloc_height=m->v.frame->height; m->swscale=malloc(1);m->scale_pic[0]=malloc((size_t)alloc_width*alloc_height);return m->swscale && m->scale_pic[0]; }
'''
suffix = r'''
#define CHECK(x) do { if(!(x)){fprintf(stderr,"failed line %d\n",__LINE__);return 1;} } while(0)
int main(void) { AVFrame f={320,240,1,0,0}; mp_media_t m={0};m.v.frame=&f;
 CHECK(mp_media_update_scaling(&m)); CHECK(builds==1 && alloc_width==320 && alloc_height==240);
 CHECK(mp_media_update_scaling(&m) && builds==1);
 f.width=3840; f.height=2160; CHECK(mp_media_update_scaling(&m)); CHECK(builds==2 && alloc_width==3840 && alloc_height==2160);
 f.format=2; CHECK(mp_media_update_scaling(&m)); CHECK(!m.swscale && !m.scale_pic[0] && m.scale_format==2);
 f.format=1; fail=1; CHECK(!mp_media_update_scaling(&m) && !m.scale_initialized); fail=0;
 CHECK(mp_media_update_scaling(&m) && builds==3); f.colorspace=1; CHECK(mp_media_update_scaling(&m) && builds==4);
 sws_freeContext(m.swscale); av_freep(&m.scale_pic[0]); puts("Media scaling geometry and format transitions passed");return 0; }
'''
with tempfile.TemporaryDirectory() as d:
 c=Path(d)/'test.c'; exe=Path(d)/'test.exe'; c.write_text(prefix+source[a:b]+suffix,encoding='utf-8')
 subprocess.run(['gcc','-std=c99','-O2',str(c),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
