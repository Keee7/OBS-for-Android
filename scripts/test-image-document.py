#!/usr/bin/env python3
"""Exercise production Android image I/O and decode functions with host FFmpeg.

The document bridge maps an opaque URI to fixture descriptors. This verifies
real PNG/JPEG/GIF demux/decode and ownership, not Android provider permissions,
GPU upload, or device rendering. Requires a matching host FFmpeg development SDK.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]


def function(text, name):
    import re
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


PREFIX = r'''
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

REFORMAT = r'''
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

CHECKS = r'''
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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ffmpeg-prefix', required=True, type=Path)
    parser.add_argument('--compiler', default=os.environ.get('CC', 'C:/msys64/ucrt64/bin/gcc.exe' if os.name == 'nt' else 'cc'))
    args = parser.parse_args()
    source = (ROOT / 'obs-studio/libobs/graphics/graphics-ffmpeg.c').read_text(encoding='utf-8')
    images = (ROOT / 'obs-studio/libobs/graphics/image-file.c').read_text(encoding='utf-8')
    struct_start = source.index('struct ffmpeg_image {')
    struct_end = source.index('};', struct_start) + 2
    code = PREFIX + source[struct_start:struct_end]
    for name in ('image_document_read', 'image_document_seek', 'ffmpeg_image_open_decoder_context',
                 'ffmpeg_image_free', 'ffmpeg_image_init'):
        code += '\n' + function(source, name).replace('av_malloc(', 'image_malloc(')
    code += '\n' + function(images, 'image_file_is_gif') + REFORMAT
    code += '\n' + function(source, 'ffmpeg_image_decode') + CHECKS
    parent = ROOT.parent / 'build-tools/checks/image-document'
    parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=parent, prefix='fixture-') as tmp:
        path = Path(tmp)
        cfile = path / 'image-test.c'
        cfile.write_text(code, encoding='utf-8')
        exe = path / 'image-test.exe'
        subprocess.run([args.compiler, '-std=c11', '-D_POSIX_C_SOURCE=200809L', '-Wall', '-Wextra', '-Werror',
                        str(cfile), '-I' + str(args.ffmpeg_prefix / 'include'),
                        '-L' + str(args.ffmpeg_prefix / 'lib'), '-lavformat', '-lavcodec', '-lavutil', '-lswscale',
                        '-o', str(exe)], check=True)
        env = os.environ.copy()
        binary_dirs = [str(args.ffmpeg_prefix.resolve() / 'bin')]
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


if __name__ == '__main__':
    main()
