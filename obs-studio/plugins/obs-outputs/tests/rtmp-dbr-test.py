# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise RTMP session reset and Android CQ dynamic bitrate eligibility."""
from pathlib import Path
import subprocess
import tempfile

plugin = Path(__file__).resolve().parents[1]
source = (plugin / "rtmp-stream.c").read_text(encoding="utf-8-sig")
reset = source[source.index("\tda_clear(stream->dbr_interpolation_table);", source.index("static bool init_connect")):
               source.index("\n\t/* Determine if the stream is multitrack", source.index("static bool init_connect"))]
start = source.index("\t\tconst char *encoder_id = obs_encoder_get_id(enc);", source.index("static bool init_connect"))
end = source.index("\n\t\tlong long bitrate", start)
eligibility = source[start:end]
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "util/darray.h"
void *bmalloc(size_t n) { return malloc(n); }
void *brealloc(void *p, size_t n) { return realloc(p, n); }
void bfree(void *p) { free(p); }
struct dbr_interpolation_point { long bitrates[2]; };
struct stream { DARRAY(struct dbr_interpolation_point) dbr_interpolation_table; };
struct encoder { const char *id; int mode; };
static const char *obs_encoder_get_id(struct encoder *enc) { return enc->id; }
static int obs_data_get_int(struct encoder *settings, const char *name) { (void)name; return settings->mode; }
#define info(...) ((void)0)
static void reset(struct stream *stream, long bitrate) {
''' + reset + r'''
 dbr_point->bitrates[0] = bitrate;
}
static bool capable(struct encoder *enc) {
 struct encoder *settings = enc; bool dbr_capable = true;
''' + eligibility + r'''
 return dbr_capable;
}
int main(void) {
 struct stream stream = {0};
 reset(&stream, 9000);
 reset(&stream, 4000);
 assert(stream.dbr_interpolation_table.num == 2);
 assert(stream.dbr_interpolation_table.array[0].bitrates[0] == 0);
 assert(stream.dbr_interpolation_table.array[1].bitrates[0] == 4000);
 struct encoder e = {"android_mediacodec_h264", 0};
 assert(!capable(&e)); e.id = "android_mediacodec_hevc"; assert(!capable(&e));
 e.mode = 1; assert(capable(&e)); e.mode = 2; assert(capable(&e));
 e.id = "other_encoder"; e.mode = 0; assert(capable(&e));
 da_free(stream.dbr_interpolation_table);
 return 0;
}
'''
with tempfile.TemporaryDirectory() as tmp:
    test = Path(tmp) / "test.c"
    exe = Path(tmp) / "test.exe"
    test.write_text(code, encoding="utf-8")
    subprocess.run(["clang", "-std=c11", "-I", str(plugin.parents[1] / "libobs"), str(test), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print("RTMP reconnect and MediaCodec mode tests passed")
