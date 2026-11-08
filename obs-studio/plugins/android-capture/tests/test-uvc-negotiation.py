#!/usr/bin/env python3
"""Compile the production negotiation functions against deterministic fake USB calls."""
import os
import argparse
import pathlib
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--compiler', default=os.environ.get('CC', 'gcc'))
args = parser.parse_args()
source = pathlib.Path(__file__).resolve().parents[1] / "uvc-input.c"
text = source.read_text(encoding="utf-8")
start = text.index("static int probe_struct_len(")
end = text.index("/* ---------------- 帧送出", start)
production = text[start:end]
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
struct uvc_dev { void *h; int vs_iface, vs_alt, chosen, eff_fps; uint32_t uvc_version, probe_len, payload_size, frame_interval, frame_buf_size; struct uvc_mode modes[1]; };
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
with tempfile.TemporaryDirectory() as temp:
    src = pathlib.Path(temp) / "negotiation.c"
    binary = pathlib.Path(temp) / ("negotiation.exe" if os.name == "nt" else "negotiation")
    src.write_text(prefix + production + suffix, encoding="utf-8")
    subprocess.run([args.compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", str(src), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
