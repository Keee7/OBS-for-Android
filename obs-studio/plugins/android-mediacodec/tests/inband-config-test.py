# SPDX-License-Identifier: GPL-2.0-or-later
"""Compile and exercise the plugin's actual in-band configuration functions."""
from pathlib import Path
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
PLUGIN = HERE.parent
source = (PLUGIN / 'android-mediacodec.c').read_text(encoding='utf-8-sig')

def function(name):
    start = source.index('static ' + name)
    brace = source.index('{', start)
    depth = 0
    for i in range(brace, len(source)):
        if source[i] == '{':
            depth += 1
        elif source[i] == '}':
            depth -= 1
            if not depth:
                return source[start:i + 1]
    raise ValueError('unterminated function: ' + name)

stub = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "avc-utils.h"
#include "hevc-utils.h"
#define pthread_mutex_lock(x) ((void)(x))
#define pthread_mutex_unlock(x) ((void)(x))
struct encoder { int mutex; bool hevc, failed; uint8_t *extra, *pending_config; size_t extra_size, pending_size; };
static void fail(struct encoder *e, const char *msg) { (void)msg; e->failed = true; }
static void *brealloc(void *p, size_t n) { void *q = realloc(p, n); assert(q); return q; }
static void bfree(void *p) { free(p); }
static void *bmemdup(const void *p, size_t n) { void *q = malloc(n); assert(q); memcpy(q,p,n); return q; }
'''
checks = r'''
static void append(uint8_t *p, size_t *n, const uint8_t *nal, size_t size, bool four)
{
    p[(*n)++] = 0; p[(*n)++] = 0; if (four) p[(*n)++] = 0; p[(*n)++] = 1;
    memcpy(p + *n, nal, size); *n += size;
}
static void clear(struct encoder *e) { free(e->extra); free(e->pending_config); memset(e, 0, sizeof(*e)); }
static void check_types(const struct encoder *e, unsigned expected)
{
    size_t cursor = 0, start, end; unsigned count = 0;
    while (hevc_next_nal(e->extra, e->extra_size, &cursor, &start, &end)) {
        assert(end > start);
        unsigned type = e->hevc ? (e->extra[start] >> 1) & 63 : e->extra[start] & 31;
        assert(e->hevc ? (type >= 32 && type <= 34) : (type == 7 || type == 8));
        ++count;
    }
    assert(count == expected);
}
int main(void)
{
    uint8_t *au = malloc(1200000 + 256); assert(au); size_t n = 0;
    const uint8_t avc_sps[] = {0x67,0x64,0,0x1f,0xac};
    const uint8_t avc_pps[] = {0x68,0x80};
    const uint8_t avc_sei[] = {0x06,0x80};
    const uint8_t avc_vcl[] = {0x65,0x81};
    struct encoder e = {0};
    append(au,&n,avc_sei,sizeof avc_sei,false);
    append(au,&n,avc_sps,sizeof avc_sps,true);
    append(au,&n,avc_pps,sizeof avc_pps,false);
    append(au,&n,avc_vcl,sizeof avc_vcl,true);
    memset(au+n,0x55,1100000); n += 1100000;
    inband_config(&e,au,n);
    assert(!e.failed && e.extra && e.extra_size < 100); check_types(&e,2); clear(&e);

    const uint8_t vps[] = {0x40,1,0x80};
    /* SPS from Chromium bear.hevc via WebRTC, BSD-3-Clause. */
    const uint8_t sps[] = {0x42,0x01,0x01,0x01,0x60,0,0,3,0,0x80,0,0,3,0,0,3,0,0x3c,0xa0,0x0a,0x08,0x0b,0x9f,0x79,0x65,0x79,0x24,0xca,0xe0,0x10,0,0,3,0,0x10,0,0,3,1,0xe0,0x80};
    const uint8_t pps[] = {0x44,1,0x80};
    const uint8_t sei[] = {0x4e,1,0x80};
    const uint8_t vcl[] = {0x26,1,0x80};
    e.hevc = true; n = 0;
    append(au,&n,sei,sizeof sei,true); append(au,&n,vps,sizeof vps,false);
    append(au,&n,sps,sizeof sps,true); append(au,&n,pps,sizeof pps,false);
    append(au,&n,vcl,sizeof vcl,true);
    memset(au+n,0x55,1100000); n += 1100000;
    inband_config(&e,au,n);
    assert(!e.failed && e.extra && e.extra_size < 100); check_types(&e,3); clear(&e);

    n = 0; append(au,&n,avc_vcl,sizeof avc_vcl,false);
    inband_config(&e,au,n); assert(!e.failed && !e.extra && !e.pending_config); clear(&e);
    e.hevc = true; n = 0; append(au,&n,sei,sizeof sei,false);
    inband_config(&e,au,n); assert(!e.failed && !e.extra && !e.pending_config); clear(&e);
    /* Oversized parameter set itself still fails the limit. */
    n = 0; append(au,&n,avc_sps,sizeof avc_sps,false);
    memset(au+n,0x55,1050000); n += 1050000;
    inband_config(&e,au,n); assert(e.failed && !e.extra); clear(&e);
    free(au); return 0;
}
'''
program = stub + '\n'.join(function(x) for x in ('uint8_t *annexb(', 'void save_config(', 'void inband_config(')) + checks
with tempfile.TemporaryDirectory(prefix='inband-test-', dir=HERE) as temp:
    temp = Path(temp)
    cfile, exe = temp / 'test.c', temp / 'test.exe'
    cfile.write_text(program, encoding='utf-8')
    subprocess.run(['clang', '-std=c11', '-Wall', '-Wextra', '-Wno-unused-function', '-I', str(PLUGIN), str(cfile), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print('in-band configuration tests passed')
