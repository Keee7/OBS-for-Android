/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

/*
 * ABI-exact FFmpeg 7.x (libavcodec.so.61 / libavutil.so.59) struct definitions
 * for AArch64 (LP64) Android, enabling official NDI v6 libndi.so to decode
 * NDI|HX / HX2 / HX3 streams (H.264, HEVC/H.265, AAC) using Android NDK
 * AMediaCodec hardware decoders without bundling proprietary or conflicting
 * FFmpeg 7 shared libraries.
 *
 * Every offset below is statically verified against upstream FFmpeg 7.1.3
 * (LIBAVCODEC_VERSION_MAJOR=61, LIBAVUTIL_VERSION_MAJOR=59) on aarch64-android.
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NDI_HX_AV_NUM_DATA_POINTERS 8
#define NDI_HX_AV_NOPTS_VALUE ((int64_t)UINT64_C(0x8000000000000000))

#define NDI_HX_AVERROR(e) (-(e))
#define NDI_HX_AVERROR_EOF (-(int)(('E') | ('O' << 8) | ('F' << 16) | (' ' << 24)))
#define NDI_HX_AVERROR_INVALIDDATA (-(int)(('I') | ('N' << 8) | ('D' << 16) | ('A' << 24)))

/* enum AVMediaType */
#define NDI_HX_AVMEDIA_TYPE_UNKNOWN (-1)
#define NDI_HX_AVMEDIA_TYPE_VIDEO 0
#define NDI_HX_AVMEDIA_TYPE_AUDIO 1

/* enum AVCodecID (FFmpeg 7.1) */
#define NDI_HX_AV_CODEC_ID_NONE 0
#define NDI_HX_AV_CODEC_ID_H264 27
#define NDI_HX_AV_CODEC_ID_HEVC 173
#define NDI_HX_AV_CODEC_ID_AAC 86018

/* enum AVPixelFormat (FFmpeg 7.1) */
#define NDI_HX_AV_PIX_FMT_NONE (-1)
#define NDI_HX_AV_PIX_FMT_YUV420P 0
#define NDI_HX_AV_PIX_FMT_NV12 23
#define NDI_HX_AV_PIX_FMT_P010LE 158

/* enum AVSampleFormat (FFmpeg 7.1) */
#define NDI_HX_AV_SAMPLE_FMT_NONE (-1)
#define NDI_HX_AV_SAMPLE_FMT_S16 1
#define NDI_HX_AV_SAMPLE_FMT_FLTP 8

/* enum AVChannelOrder (FFmpeg 7.1) */
#define NDI_HX_AV_CHANNEL_ORDER_UNSPEC 0
#define NDI_HX_AV_CHANNEL_ORDER_NATIVE 1

/* Standard native channel layout masks */
#define NDI_HX_AV_CH_LAYOUT_MONO 0x00000004ULL
#define NDI_HX_AV_CH_LAYOUT_STEREO 0x00000003ULL
#define NDI_HX_AV_CH_LAYOUT_2POINT1 0x0000000BULL
#define NDI_HX_AV_CH_LAYOUT_4POINT0 0x00000107ULL
#define NDI_HX_AV_CH_LAYOUT_5POINT0 0x00000607ULL
#define NDI_HX_AV_CH_LAYOUT_5POINT1 0x0000060FULL
#define NDI_HX_AV_CH_LAYOUT_7POINT1 0x0000063FULL

/* Frame / packet flags */
#define NDI_HX_AV_PKT_FLAG_KEY 0x0001
#define NDI_HX_AV_FRAME_FLAG_KEY (1 << 1)
#define NDI_HX_AV_PICTURE_TYPE_NONE 0
#define NDI_HX_AV_PICTURE_TYPE_I 1
#define NDI_HX_AV_PICTURE_TYPE_P 2

/* Color metadata enums (FFmpeg 7.1) */
#define NDI_HX_AVCOL_RANGE_UNSPECIFIED 0
#define NDI_HX_AVCOL_RANGE_MPEG 1
#define NDI_HX_AVCOL_RANGE_JPEG 2

#define NDI_HX_AVCOL_PRI_BT709 1
#define NDI_HX_AVCOL_PRI_UNSPECIFIED 2
#define NDI_HX_AVCOL_PRI_BT470BG 5
#define NDI_HX_AVCOL_PRI_SMPTE170M 6
#define NDI_HX_AVCOL_PRI_BT2020 9

#define NDI_HX_AVCOL_TRC_BT709 1
#define NDI_HX_AVCOL_TRC_UNSPECIFIED 2
#define NDI_HX_AVCOL_TRC_SMPTE170M 6
#define NDI_HX_AVCOL_TRC_SMPTE2084 16
#define NDI_HX_AVCOL_TRC_ARIB_STD_B67 18

#define NDI_HX_AVCOL_SPC_BT709 1
#define NDI_HX_AVCOL_SPC_UNSPECIFIED 2
#define NDI_HX_AVCOL_SPC_BT470BG 5
#define NDI_HX_AVCOL_SPC_SMPTE170M 6
#define NDI_HX_AVCOL_SPC_BT2020_NCL 9

typedef struct AVRational7 {
	int num;
	int den;
} AVRational7;

typedef struct AVChannelLayout7 {
	int order;
	int nb_channels;
	union {
		uint64_t mask;
		void *map;
	} u;
	void *opaque;
} AVChannelLayout7;

typedef struct AVCodec7 {
	const char *name;
	const char *long_name;
	int type;
	int id;
	int capabilities;
	uint8_t max_lowres;
	const AVRational7 *supported_framerates;
	const int *pix_fmts;
	const int *supported_samplerates;
	const int *sample_fmts;
	const void *priv_class;
	const void *profiles;
	const char *wrapper_name;
	const AVChannelLayout7 *ch_layouts;
} AVCodec7;

typedef struct AVPacket7 {
	void *buf;
	int64_t pts;
	int64_t dts;
	uint8_t *data;
	int size;
	int stream_index;
	int flags;
	void *side_data;
	int side_data_elems;
	int64_t duration;
	int64_t pos;
	void *opaque;
	void *opaque_ref;
	AVRational7 time_base;
} AVPacket7;

typedef struct AVCodecParameters7 {
	int codec_type;
	int codec_id;
	uint32_t codec_tag;
	uint8_t *extradata;
	int extradata_size;
	void *coded_side_data;
	int nb_coded_side_data;
	int format;
	int64_t bit_rate;
	int bits_per_coded_sample;
	int bits_per_raw_sample;
	int profile;
	int level;
	int width;
	int height;
	AVRational7 sample_aspect_ratio;
	AVRational7 framerate;
	int field_order;
	int color_range;
	int color_primaries;
	int color_trc;
	int color_space;
	int chroma_location;
	int video_delay;
	AVChannelLayout7 ch_layout;
	int sample_rate;
	int block_align;
	int frame_size;
	int initial_padding;
	int trailing_padding;
	int seek_preroll;
} AVCodecParameters7;

typedef struct AVFrame7 {
	uint8_t *data[NDI_HX_AV_NUM_DATA_POINTERS];
	int linesize[NDI_HX_AV_NUM_DATA_POINTERS];
	uint8_t **extended_data;
	int width;
	int height;
	int nb_samples;
	int format;
	int key_frame;
	int pict_type;
	AVRational7 sample_aspect_ratio;
	int64_t pts;
	int64_t pkt_dts;
	AVRational7 time_base;
	int quality;
	void *opaque;
	int repeat_pict;
	int interlaced_frame;
	int top_field_first;
	int palette_has_changed;
	int sample_rate;
	void *buf[NDI_HX_AV_NUM_DATA_POINTERS];
	void **extended_buf;
	int nb_extended_buf;
	void **side_data;
	int nb_side_data;
	int flags;
	int color_range;
	int color_primaries;
	int color_trc;
	int colorspace;
	int chroma_location;
	int64_t best_effort_timestamp;
	int64_t pkt_pos;
	void *metadata;
	int decode_error_flags;
	int pkt_size;
	void *hw_frames_ctx;
	void *opaque_ref;
	size_t crop_top;
	size_t crop_bottom;
	size_t crop_left;
	size_t crop_right;
	void *private_ref;
	AVChannelLayout7 ch_layout;
	int64_t duration;
} AVFrame7;

typedef struct AVCodecContext7 {
	const void *av_class;            /* 0 */
	int log_level_offset;            /* 8 */
	int codec_type;                  /* 12 */
	const AVCodec7 *codec;           /* 16 */
	int codec_id;                    /* 24 */
	unsigned int codec_tag;          /* 28 */
	void *priv_data;                 /* 32 */
	void *internal;                  /* 40 */
	void *opaque;                    /* 48 */
	int64_t bit_rate;                /* 56 */
	int flags;                       /* 64 */
	int flags2;                      /* 68 */
	uint8_t *extradata;              /* 72 */
	int extradata_size;              /* 80 */
	AVRational7 time_base;           /* 84 */
	AVRational7 pkt_timebase;        /* 92 */
	AVRational7 framerate;           /* 100 */
	int ticks_per_frame;             /* 108 */
	int delay;                       /* 112 */
	int width;                       /* 116 */
	int height;                      /* 120 */
	int coded_width;                 /* 124 */
	int coded_height;                /* 128 */
	AVRational7 sample_aspect_ratio; /* 132 */
	int pix_fmt;                     /* 140 */
	int sw_pix_fmt;                  /* 144 */
	int color_primaries;             /* 148 */
	int color_trc;                   /* 152 */
	int colorspace;                  /* 156 */
	int color_range;                 /* 160 */
	uint8_t _pad_video[344 - 164];   /* 164..343 */
	int sample_rate;                 /* 344 */
	int sample_fmt;                  /* 348 */
	AVChannelLayout7 ch_layout;      /* 352..375 */
	int frame_size;                  /* 376 */
	uint8_t _pad_audio[656 - 380];   /* 380..655 */
	int thread_count;                /* 656 */
	int thread_type;                 /* 660 */
	uint8_t _pad_tail[1024 - 664];   /* 664..1023 (>= 864 bytes of FFmpeg 7.1) */
} AVCodecContext7;

#if defined(__aarch64__)
_Static_assert(sizeof(AVChannelLayout7) == 24, "AVChannelLayout7 size mismatch");
_Static_assert(offsetof(AVChannelLayout7, nb_channels) == 4, "AVChannelLayout7.nb_channels mismatch");
_Static_assert(offsetof(AVChannelLayout7, u) == 8, "AVChannelLayout7.u mismatch");
_Static_assert(offsetof(AVChannelLayout7, opaque) == 16, "AVChannelLayout7.opaque mismatch");

_Static_assert(sizeof(AVCodec7) == 96, "AVCodec7 size mismatch");
_Static_assert(offsetof(AVCodec7, id) == 20, "AVCodec7.id mismatch");
_Static_assert(offsetof(AVCodec7, pix_fmts) == 40, "AVCodec7.pix_fmts mismatch");
_Static_assert(offsetof(AVCodec7, sample_fmts) == 56, "AVCodec7.sample_fmts mismatch");
_Static_assert(offsetof(AVCodec7, ch_layouts) == 88, "AVCodec7.ch_layouts mismatch");

_Static_assert(sizeof(AVPacket7) == 104, "AVPacket7 size mismatch");
_Static_assert(offsetof(AVPacket7, pts) == 8, "AVPacket7.pts mismatch");
_Static_assert(offsetof(AVPacket7, dts) == 16, "AVPacket7.dts mismatch");
_Static_assert(offsetof(AVPacket7, data) == 24, "AVPacket7.data mismatch");
_Static_assert(offsetof(AVPacket7, size) == 32, "AVPacket7.size mismatch");
_Static_assert(offsetof(AVPacket7, flags) == 40, "AVPacket7.flags mismatch");
_Static_assert(offsetof(AVPacket7, opaque) == 80, "AVPacket7.opaque mismatch");
_Static_assert(offsetof(AVPacket7, time_base) == 96, "AVPacket7.time_base mismatch");

_Static_assert(sizeof(AVCodecParameters7) == 176, "AVCodecParameters7 size mismatch");
_Static_assert(offsetof(AVCodecParameters7, extradata) == 16, "AVCodecParameters7.extradata mismatch");
_Static_assert(offsetof(AVCodecParameters7, format) == 44, "AVCodecParameters7.format mismatch");
_Static_assert(offsetof(AVCodecParameters7, width) == 72, "AVCodecParameters7.width mismatch");
_Static_assert(offsetof(AVCodecParameters7, height) == 76, "AVCodecParameters7.height mismatch");
_Static_assert(offsetof(AVCodecParameters7, color_range) == 100, "AVCodecParameters7.color_range mismatch");
_Static_assert(offsetof(AVCodecParameters7, ch_layout) == 128, "AVCodecParameters7.ch_layout mismatch");
_Static_assert(offsetof(AVCodecParameters7, sample_rate) == 152, "AVCodecParameters7.sample_rate mismatch");

_Static_assert(sizeof(AVFrame7) == 440, "AVFrame7 size mismatch");
_Static_assert(offsetof(AVFrame7, linesize) == 64, "AVFrame7.linesize mismatch");
_Static_assert(offsetof(AVFrame7, extended_data) == 96, "AVFrame7.extended_data mismatch");
_Static_assert(offsetof(AVFrame7, width) == 104, "AVFrame7.width mismatch");
_Static_assert(offsetof(AVFrame7, height) == 108, "AVFrame7.height mismatch");
_Static_assert(offsetof(AVFrame7, nb_samples) == 112, "AVFrame7.nb_samples mismatch");
_Static_assert(offsetof(AVFrame7, format) == 116, "AVFrame7.format mismatch");
_Static_assert(offsetof(AVFrame7, key_frame) == 120, "AVFrame7.key_frame mismatch");
_Static_assert(offsetof(AVFrame7, pts) == 136, "AVFrame7.pts mismatch");
_Static_assert(offsetof(AVFrame7, pkt_dts) == 144, "AVFrame7.pkt_dts mismatch");
_Static_assert(offsetof(AVFrame7, opaque) == 168, "AVFrame7.opaque mismatch");
_Static_assert(offsetof(AVFrame7, sample_rate) == 192, "AVFrame7.sample_rate mismatch");
_Static_assert(offsetof(AVFrame7, flags) == 292, "AVFrame7.flags mismatch");
_Static_assert(offsetof(AVFrame7, color_range) == 296, "AVFrame7.color_range mismatch");
_Static_assert(offsetof(AVFrame7, colorspace) == 308, "AVFrame7.colorspace mismatch");
_Static_assert(offsetof(AVFrame7, best_effort_timestamp) == 320, "AVFrame7.best_effort_timestamp mismatch");
_Static_assert(offsetof(AVFrame7, crop_top) == 368, "AVFrame7.crop_top mismatch");
_Static_assert(offsetof(AVFrame7, ch_layout) == 408, "AVFrame7.ch_layout mismatch");
_Static_assert(offsetof(AVFrame7, duration) == 432, "AVFrame7.duration mismatch");

_Static_assert(offsetof(AVCodecContext7, codec_type) == 12, "AVCodecContext7.codec_type mismatch");
_Static_assert(offsetof(AVCodecContext7, codec) == 16, "AVCodecContext7.codec mismatch");
_Static_assert(offsetof(AVCodecContext7, codec_id) == 24, "AVCodecContext7.codec_id mismatch");
_Static_assert(offsetof(AVCodecContext7, priv_data) == 32, "AVCodecContext7.priv_data mismatch");
_Static_assert(offsetof(AVCodecContext7, extradata) == 72, "AVCodecContext7.extradata mismatch");
_Static_assert(offsetof(AVCodecContext7, width) == 116, "AVCodecContext7.width mismatch");
_Static_assert(offsetof(AVCodecContext7, height) == 120, "AVCodecContext7.height mismatch");
_Static_assert(offsetof(AVCodecContext7, pix_fmt) == 140, "AVCodecContext7.pix_fmt mismatch");
_Static_assert(offsetof(AVCodecContext7, color_range) == 160, "AVCodecContext7.color_range mismatch");
_Static_assert(offsetof(AVCodecContext7, sample_rate) == 344, "AVCodecContext7.sample_rate mismatch");
_Static_assert(offsetof(AVCodecContext7, sample_fmt) == 348, "AVCodecContext7.sample_fmt mismatch");
_Static_assert(offsetof(AVCodecContext7, ch_layout) == 352, "AVCodecContext7.ch_layout mismatch");
_Static_assert(offsetof(AVCodecContext7, frame_size) == 376, "AVCodecContext7.frame_size mismatch");
_Static_assert(offsetof(AVCodecContext7, thread_count) == 656, "AVCodecContext7.thread_count mismatch");
#endif

/*
 * Boxed AVFrame7 owned by av_frame_alloc() / av_frame_free().
 * Because libndi.so never calls av_frame_unref(), attaching our owned pixel/PCM
 * backing buffer right behind AVFrame7 lets avcodec_receive_frame() safely reuse
 * the backing allocation across frames and av_frame_free() release it cleanly.
 */
#define NDI_HX_FRAME_MAGIC 0x4E4449485846524DULL /* "NDIHXFRM" */

typedef struct ndi_hx_frame_box {
	AVFrame7 frame;
	uint64_t magic;
	uint8_t *owned_buf;
	size_t owned_cap;
} ndi_hx_frame_box_t;

static inline void ndi_hx_channel_layout_default_inline(AVChannelLayout7 *ch_layout, int nb_channels)
{
	if (!ch_layout)
		return;
	memset(ch_layout, 0, sizeof(*ch_layout));
	if (nb_channels <= 0)
		return;
	ch_layout->nb_channels = nb_channels;
	switch (nb_channels) {
	case 1:
		ch_layout->order = NDI_HX_AV_CHANNEL_ORDER_NATIVE;
		ch_layout->u.mask = NDI_HX_AV_CH_LAYOUT_MONO;
		break;
	case 2:
		ch_layout->order = NDI_HX_AV_CHANNEL_ORDER_NATIVE;
		ch_layout->u.mask = NDI_HX_AV_CH_LAYOUT_STEREO;
		break;
	case 3:
		ch_layout->order = NDI_HX_AV_CHANNEL_ORDER_NATIVE;
		ch_layout->u.mask = NDI_HX_AV_CH_LAYOUT_2POINT1;
		break;
	case 4:
		ch_layout->order = NDI_HX_AV_CHANNEL_ORDER_NATIVE;
		ch_layout->u.mask = NDI_HX_AV_CH_LAYOUT_4POINT0;
		break;
	case 5:
		ch_layout->order = NDI_HX_AV_CHANNEL_ORDER_NATIVE;
		ch_layout->u.mask = NDI_HX_AV_CH_LAYOUT_5POINT0;
		break;
	case 6:
		ch_layout->order = NDI_HX_AV_CHANNEL_ORDER_NATIVE;
		ch_layout->u.mask = NDI_HX_AV_CH_LAYOUT_5POINT1;
		break;
	case 8:
		ch_layout->order = NDI_HX_AV_CHANNEL_ORDER_NATIVE;
		ch_layout->u.mask = NDI_HX_AV_CH_LAYOUT_7POINT1;
		break;
	default:
		ch_layout->order = NDI_HX_AV_CHANNEL_ORDER_UNSPEC;
		ch_layout->u.mask = 0;
		break;
	}
}

static inline void ndi_hx_frame_reset_defaults(AVFrame7 *f)
{
	memset(f, 0, sizeof(*f));
	f->extended_data = f->data;
	f->format = -1;
	f->pts = NDI_HX_AV_NOPTS_VALUE;
	f->pkt_dts = NDI_HX_AV_NOPTS_VALUE;
	f->best_effort_timestamp = NDI_HX_AV_NOPTS_VALUE;
	f->pkt_pos = -1;
	f->pkt_size = -1;
	f->color_range = NDI_HX_AVCOL_RANGE_UNSPECIFIED;
	f->color_primaries = NDI_HX_AVCOL_PRI_UNSPECIFIED;
	f->color_trc = NDI_HX_AVCOL_TRC_UNSPECIFIED;
	f->colorspace = NDI_HX_AVCOL_SPC_UNSPECIFIED;
}

static inline uint8_t *ndi_hx_frame_ensure_buf(AVFrame7 *f, size_t need_bytes)
{
	if (!f || need_bytes == 0)
		return NULL;
	ndi_hx_frame_box_t *box = (ndi_hx_frame_box_t *)f;
	if (box->magic != NDI_HX_FRAME_MAGIC)
		return NULL;
	if (box->owned_cap >= need_bytes && box->owned_buf)
		return box->owned_buf;
	const size_t alloc_sz = (need_bytes + 63U) & ~(size_t)63U;
	uint8_t *nb = (uint8_t *)realloc(box->owned_buf, alloc_sz);
	if (!nb)
		return NULL;
	box->owned_buf = nb;
	box->owned_cap = alloc_sz;
	return nb;
}

#ifdef __cplusplus
}
#endif
