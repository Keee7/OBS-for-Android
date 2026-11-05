"""Compile and test the patched FFmpeg MediaCodec YUV copy routines on the host."""
import pathlib, subprocess, tempfile, sys

source = pathlib.Path(sys.argv[1]).read_text(encoding="utf-8")
start = source.index("void ff_mediacodec_sw_buffer_copy_yuv420_planar(")
end = source.index("/**\n * The code handling the QCOM_", start)
prefix = r'''
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#define FFMIN(a,b) ((a)<(b)?(a):(b))
#define FFALIGN(a,b) (((a)+(b)-1)&~((b)-1))
typedef struct { int width, height; } AVCodecContext;
typedef struct { int stride, slice_height, crop_top, crop_left; } MediaCodecDecContext;
typedef struct { int offset; } FFAMediaCodecBufferInfo;
typedef struct { uint8_t *data[3]; int linesize[3]; } AVFrame;
'''
suffix = r'''
static int check(int mode, int dststride) {
  AVCodecContext ctx={4,4}; MediaCodecDecContext s={8,8,2,2};
  FFAMediaCodecBufferInfo info={3}; uint8_t in[128], out[3][64];
  AVFrame f={{out[0],out[1],out[2]},{dststride,dststride,dststride}};
  memset(in,0,sizeof(in)); memset(out,0xcc,sizeof(out));
  for(int y=0;y<8;y++) for(int x=0;x<8;x++) in[3+y*8+x]=(uint8_t)(10*y+x);
  if(mode==0) {
    for(int p=0;p<2;p++) for(int y=0;y<4;y++) for(int x=0;x<4;x++) in[3+64+p*16+y*4+x]=(uint8_t)(80+p*40+10*y+x);
    ff_mediacodec_sw_buffer_copy_yuv420_planar(&ctx,&s,in,sizeof(in),&info,&f);
  } else {
    for(int y=0;y<4;y++) for(int x=0;x<8;x++) in[3+64+y*8+x]=(uint8_t)(80+10*y+x);
    if(mode==1) ff_mediacodec_sw_buffer_copy_yuv420_semi_planar(&ctx,&s,in,sizeof(in),&info,&f);
    else ff_mediacodec_sw_buffer_copy_yuv420_packed_semi_planar(&ctx,&s,in,sizeof(in),&info,&f);
  }
  for(int y=0;y<4;y++) for(int x=0;x<4;x++) if(out[0][y*dststride+x]!=22+10*y+x) return 1;
  for(int y=0;y<2;y++) for(int x=0;x<(mode==0?2:4);x++) if(out[1][y*dststride+x]!=(mode==0?91:92)+10*y+x) return 2;
  if(mode==0) for(int y=0;y<2;y++) for(int x=0;x<2;x++) if(out[2][y*dststride+x]!=131+10*y+x) return 3;
  return 0;
}
int main(void) { for(int m=0;m<3;m++) for(int stride=4;stride<=8;stride+=4) { int r=check(m,stride); if(r){fprintf(stderr,"mode=%d stride=%d failed=%d\n",m,stride,r);return 1;} } puts("MediaCodec crop/stride copy tests passed"); return 0; }
'''
with tempfile.TemporaryDirectory() as d:
    c = pathlib.Path(d) / 'test.c'; exe = pathlib.Path(d) / 'test.exe'
    c.write_text(prefix + source[start:end] + suffix, encoding='utf-8')
    subprocess.run(['gcc', '-std=c99', '-O2', str(c), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
