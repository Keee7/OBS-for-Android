# SPDX-License-Identifier: GPL-2.0-or-later
"""Compile the RTMP send paths and inject socket/header/write failures."""
from pathlib import Path
import subprocess
import tempfile

plugin = Path(__file__).resolve().parents[1]
source = (plugin / "rtmp-stream.c").read_text(encoding="utf-8-sig")
start = source.index("static inline void release_sent_packet(")
end = source.index("static inline bool send_headers(", start)
functions = source[start:end]
start = source.index("\t\tif (!stream->sent_headers) {", source.index("static void *send_thread("))
end = source.index("\n\t\tif (stream->dbr_enabled)", start)
headers = source[start:end]
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
struct encoder_packet { uint8_t *data; };
struct rtmp_stream { int start_dts_offset, video_codec[2], audio_codec[2], rtmp; size_t total_bytes_sent; bool sent_headers, disconnected; };
static int releases, manual_frees, mux_frees, socket_error, write_error, header_success;
static void bfree(void *p) { if (p == (void *)1) manual_frees++; else if (p == (void *)2) mux_frees++; }
static void obs_encoder_packet_release(struct encoder_packet *p) { assert(p->data == (void *)1); releases++; }
static int handle_socket_read(struct rtmp_stream *s) { (void)s; return socket_error; }
static void mux(uint8_t **data, size_t *size) { *data = (void *)2; *size = 10; }
#define flv_packet_mux(p,t,d,s,h) mux(d,s)
#define flv_packet_start(p,c,d,s,i) mux(d,s)
#define flv_packet_end(p,c,d,s,i) mux(d,s)
#define flv_packet_frames(p,c,t,d,s,i) mux(d,s)
#define flv_packet_audio_start(p,c,d,s,i) mux(d,s)
#define flv_packet_audio_frames(p,c,t,d,s,i) mux(d,s)
static int RTMP_Write(int *rtmp, char *data, int size, int ignored) { (void)rtmp; (void)data; (void)size; (void)ignored; return write_error ? -1 : size; }
static bool send_headers(struct rtmp_stream *s) { (void)s; return header_success; }
#define os_atomic_set_bool(p, v) (*(p) = (v))
''' + functions + r'''
static void handle_headers(struct rtmp_stream *stream, struct encoder_packet packet) {
 do {
''' + headers + r'''
 } while (0);
}
static void clear_counts(void) { releases = manual_frees = mux_frees = 0; }
int main(void) {
 struct rtmp_stream s = {0}; struct encoder_packet p = {(void *)1};
 for (int failure = 0; failure < 3; failure++) {
  socket_error = failure == 1; write_error = failure == 2;
  for (int kind = 0; kind < 3; kind++) {
   for (int manual = 0; manual < (kind == 1 ? 3 : 2); manual++) {
    clear_counts(); int result;
    if (kind == 0) result = send_packet(&s, &p, manual != 0);
    else if (kind == 1) result = send_packet_ex(&s, &p, manual == 1, manual == 2, 0);
    else result = send_audio_packet_ex(&s, &p, manual != 0, 0);
    assert((result < 0) == (failure != 0));
    assert(releases == (manual == 0)); assert(manual_frees == (manual != 0));
    assert(mux_frees == (failure != 1));
   }
  }
 }
 clear_counts(); header_success = 0; handle_headers(&s, p);
 assert(releases == 1 && s.disconnected);
 clear_counts(); s.disconnected = false; header_success = 1; handle_headers(&s, p);
 assert(releases == 0 && !s.disconnected);
 return 0;
}
'''
with tempfile.TemporaryDirectory() as tmp:
    test = Path(tmp) / "test.c"
    exe = Path(tmp) / "test.exe"
    test.write_text(code, encoding="utf-8")
    subprocess.run(["clang", "-std=c11", "-Wall", "-Werror", "-Wno-unused-parameter", str(test), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print("RTMP send ownership failure-path tests passed")
