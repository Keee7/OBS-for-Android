#!/usr/bin/env python3
"""Exercise the actual Android media AVIO callbacks and cleanup with host file descriptors."""
from pathlib import Path
import os
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[1] / "obs-studio/shared/media-playback/media-playback/media.c").read_text()
callbacks = source[source.index("static int android_media_read"):source.index("#endif\n\nstatic int64_t base_sys_ts")]
cleanup = source[source.index("\tavformat_close_input(&media->fmt);"):source.index("\tpthread_mutex_destroy(&media->mutex);")]
harness = r'''
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <assert.h>
#ifndef O_BINARY
#define O_BINARY 0
#endif
#define __ANDROID__ 1
#define AVERROR(e) (-(e))
#define AVERROR_EOF (-12345)
#define AVSEEK_SIZE 0x10000
#define AVSEEK_FORCE 0x20000
#ifndef S_ISREG
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#endif
typedef struct AVIOContext { uint8_t *buffer; } AVIOContext;
typedef struct mp_media { int android_fd; AVIOContext *android_io; void *fmt; } mp_media_t;
static void avformat_close_input(void **fmt) { *fmt = NULL; }
static void av_freep(void *p) { void **q = p; free(*q); *q = NULL; }
static void avio_context_free(AVIOContext **p) { free(*p); *p = NULL; }
''' + callbacks + r'''
static void cleanup_media(mp_media_t *media) {
''' + cleanup + r'''
}
int main(int argc, char **argv) {
    assert(argc == 2);
    for (int i = 0; i < 3; i++) {
        mp_media_t media = {0};
        media.android_fd = open(argv[1], O_RDONLY | O_BINARY);
        assert(media.android_fd >= 0);
        int fd = media.android_fd;
        media.android_io = calloc(1, sizeof(*media.android_io));
        media.android_io->buffer = malloc(32);
        uint8_t b[8] = {0};
        assert(android_media_seek(&media, 0, AVSEEK_SIZE) == 6);
        assert(android_media_read(&media, b, 3) == 3 && memcmp(b, "abc", 3) == 0);
        assert(android_media_seek(&media, 1, SEEK_SET | AVSEEK_FORCE) == 1);
        assert(android_media_read(&media, b, 5) == 5 && memcmp(b, "bcdef", 5) == 0);
        assert(android_media_read(&media, b, 1) == AVERROR_EOF);
        assert(android_media_seek(&media, 0, SEEK_SET) == 0);
        assert(android_media_read(&media, b, 3) == 3 && memcmp(b, "abc", 3) == 0);
        cleanup_media(&media);
        assert(media.android_io == NULL);
        errno = 0;
        assert(read(fd, b, 1) < 0 && errno == EBADF);
    }
    puts("Android media AVIO read/seek/EOF/reopen/cleanup passed");
    return 0;
}
'''
with tempfile.TemporaryDirectory(dir=Path(__file__).resolve().parents[1]) as tmp:
    tmp = Path(tmp)
    (tmp / "test.c").write_text(harness)
    (tmp / "input").write_bytes(b"abcdef")
    env = dict(os.environ, TMP=str(tmp), TEMP=str(tmp), TMPDIR=str(tmp))
    subprocess.run(["gcc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", str(tmp / "test.c"), "-o", str(tmp / "test.exe")], check=True, env=env)
    subprocess.run([str(tmp / "test.exe"), str(tmp / "input")], check=True)
