/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * FFmpeg 7.x libavcodec-ndi.so.61 ABI bridge backed by Android NDK AMediaCodec.
 *
 * Official NDI v6 libndi.so on Android dynamically loads libavcodec-ndi.so.61
 * and libavutil-ndi.so.59 to decode NDI|HX / HX2 / HX3 streams (H.264, HEVC,
 * AAC). This lightweight bridge implements the exact 12 libavcodec 61 entry
 * points resolved by libndi.so and delegates decoding directly to Android's
 * native AMediaCodec decoders (video/avc, video/hevc, audio/mp4a-latm).
 */

#include "ndi-hx-avabi7.h"

#include <android/log.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NDI_HX_EXPORT __attribute__((visibility("default")))
#define HX_TAG "OBS-ndi-hx"

#define HX_LOGI(...) __android_log_print(ANDROID_LOG_INFO, HX_TAG, __VA_ARGS__)
#define HX_LOGW(...) __android_log_print(ANDROID_LOG_WARN, HX_TAG, __VA_ARGS__)
#define HX_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, HX_TAG, __VA_ARGS__)

/* Android MediaCodecInfo.CodecCapabilities color formats */
#define MC_FMT_YUV420_PLANAR 19
#define MC_FMT_YUV420_SEMIPLANAR 21
#define MC_FMT_YUVP010 54
#define MC_FMT_YUV420_FLEXIBLE 2135033992 /* 0x7F420888 */

#define HX_TS_MAP_SIZE 128
#define HX_OUT_QUEUE_CAP 4
#define HX_MAX_CSD_BYTES 4096

/* Static FFmpeg 7.1 AVCodec descriptors */
static const int k_video_pix_fmts[] = {
	NDI_HX_AV_PIX_FMT_NV12,
	NDI_HX_AV_PIX_FMT_YUV420P,
	NDI_HX_AV_PIX_FMT_NONE,
};

static const int k_audio_sample_fmts[] = {
	NDI_HX_AV_SAMPLE_FMT_FLTP,
	NDI_HX_AV_SAMPLE_FMT_NONE,
};

static const AVCodec7 k_codec_h264 = {
	.name = "h264_mediacodec",
	.long_name = "H.264 / AVC (Android MediaCodec)",
	.type = NDI_HX_AVMEDIA_TYPE_VIDEO,
	.id = NDI_HX_AV_CODEC_ID_H264,
	.pix_fmts = k_video_pix_fmts,
	.wrapper_name = "mediacodec",
};

static const AVCodec7 k_codec_hevc = {
	.name = "hevc_mediacodec",
	.long_name = "H.265 / HEVC (Android MediaCodec)",
	.type = NDI_HX_AVMEDIA_TYPE_VIDEO,
	.id = NDI_HX_AV_CODEC_ID_HEVC,
	.pix_fmts = k_video_pix_fmts,
	.wrapper_name = "mediacodec",
};

static const AVCodec7 k_codec_aac = {
	.name = "aac_mediacodec",
	.long_name = "AAC (Android MediaCodec)",
	.type = NDI_HX_AVMEDIA_TYPE_AUDIO,
	.id = NDI_HX_AV_CODEC_ID_AAC,
	.sample_fmts = k_audio_sample_fmts,
	.wrapper_name = "mediacodec",
};

struct hx_ts_entry {
	int64_t seq;
	int64_t pts;
	int64_t dts;
	void *opaque;
};

struct hx_decoded_slot {
	bool is_audio;
	uint8_t *buf;
	size_t cap;
	size_t used;
	int width, height;
	int format;
	int linesize[3];
	size_t plane_off[3];
	int nb_samples;
	int sample_rate;
	int channels;
	int64_t pts;
	int64_t dts;
	void *opaque;
	int color_range;
	int color_primaries;
	int color_trc;
	int colorspace;
	bool key;
};

struct hx_decoder {
	int codec_id;
	bool is_audio;
	AMediaCodec *codec;
	bool configured;
	bool eos_sent;
	bool eos_reached;

	/* Video geometry & color from AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED */
	int out_width;
	int out_height;
	int out_stride;
	int out_slice_height;
	int out_color_fmt;
	int crop_left, crop_top, crop_right, crop_bottom;
	int color_range;
	int color_primaries;
	int color_trc;
	int colorspace;

	/* Audio output format */
	int audio_sample_rate;
	int audio_channels;

	/* Stored codec config (Annex-B SPS/PPS or VPS/SPS/PPS, or AAC ASC) */
	uint8_t csd0[HX_MAX_CSD_BYTES];
	size_t csd0_len;
	uint8_t csd1[HX_MAX_CSD_BYTES];
	size_t csd1_len;

	/* Monotonic sequence ID -> original AVPacket timestamps */
	int64_t next_seq;
	struct hx_ts_entry ts_map[HX_TS_MAP_SIZE];

	/* Small decoded output FIFO so AMediaCodec buffers are released promptly */
	struct hx_decoded_slot q[HX_OUT_QUEUE_CAP];
	int q_head;
	int q_count;

	/* Temp conversion buffer for AVCC/HVCC -> Annex-B */
	uint8_t *annexb_tmp;
	size_t annexb_cap;
	int nal_len_size;

	uint64_t frames_in;
	uint64_t frames_out;
};

static void hx_reset_output_format(struct hx_decoder *d)
{
	d->out_width = 0;
	d->out_height = 0;
	d->out_stride = 0;
	d->out_slice_height = 0;
	d->out_color_fmt = 0;
	d->crop_left = d->crop_top = d->crop_right = d->crop_bottom = 0;
	d->color_range = NDI_HX_AVCOL_RANGE_UNSPECIFIED;
	d->color_primaries = NDI_HX_AVCOL_PRI_UNSPECIFIED;
	d->color_trc = NDI_HX_AVCOL_TRC_UNSPECIFIED;
	d->colorspace = NDI_HX_AVCOL_SPC_UNSPECIFIED;
}

static void hx_release_codec(struct hx_decoder *d)
{
	if (d->codec) {
		AMediaCodec_stop(d->codec);
		AMediaCodec_delete(d->codec);
		d->codec = NULL;
	}
	d->configured = false;
	d->eos_sent = false;
	d->eos_reached = false;
	d->q_head = 0;
	d->q_count = 0;
	hx_reset_output_format(d);
}

static void hx_destroy_decoder(struct hx_decoder *d)
{
	if (!d)
		return;
	hx_release_codec(d);
	for (int i = 0; i < HX_OUT_QUEUE_CAP; i++) {
		free(d->q[i].buf);
		d->q[i].buf = NULL;
		d->q[i].cap = 0;
	}
	free(d->annexb_tmp);
	free(d);
}

static int64_t hx_record_ts(struct hx_decoder *d, int64_t pts, int64_t dts, void *opaque)
{
	const int64_t seq = ++d->next_seq;
	struct hx_ts_entry *e = &d->ts_map[seq % HX_TS_MAP_SIZE];
	e->seq = seq;
	e->pts = pts;
	e->dts = dts;
	e->opaque = opaque;
	return seq;
}

static void hx_lookup_ts(struct hx_decoder *d, int64_t seq, int64_t *pts, int64_t *dts, void **opaque)
{
	const struct hx_ts_entry *e = &d->ts_map[(seq >= 0 ? seq : 0) % HX_TS_MAP_SIZE];
	if (e->seq == seq) {
		*pts = e->pts;
		*dts = e->dts;
		*opaque = e->opaque;
	} else {
		*pts = seq;
		*dts = seq;
		*opaque = NULL;
	}
}

/* Locate next Annex-B start code (00 00 01 or 00 00 00 01). */
static const uint8_t *find_start_code(const uint8_t *p, const uint8_t *end, size_t *sc_len)
{
	while (p + 3 <= end) {
		if (p[0] == 0 && p[1] == 0) {
			if (p[2] == 1) {
				*sc_len = 3;
				return p;
			}
			if (p + 4 <= end && p[2] == 0 && p[3] == 1) {
				*sc_len = 4;
				return p;
			}
		}
		p++;
	}
	return NULL;
}

static bool has_annexb_start_code(const uint8_t *data, size_t size)
{
	if (!data || size < 4)
		return false;
	return (data[0] == 0 && data[1] == 0 && data[2] == 1) ||
	       (data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 1);
}

/* Append a NAL unit with 4-byte Annex-B start code into a fixed buffer. */
static bool append_nal(uint8_t *dst, size_t *len, size_t cap, const uint8_t *nal, size_t nal_len)
{
	if (*len + 4 + nal_len > cap)
		return false;
	dst[(*len)++] = 0;
	dst[(*len)++] = 0;
	dst[(*len)++] = 0;
	dst[(*len)++] = 1;
	memcpy(dst + *len, nal, nal_len);
	*len += nal_len;
	return true;
}

/* Parse AVCC extradata (configurationVersion == 1) -> csd0 (SPS), csd1 (PPS). */
static bool parse_avcc_extradata(struct hx_decoder *d, const uint8_t *ext, size_t sz)
{
	if (!ext || sz < 7 || ext[0] != 1)
		return false;
	d->nal_len_size = (ext[4] & 0x03) + 1;
	size_t pos = 5;
	int num_sps = ext[pos++] & 0x1F;
	uint8_t sps_buf[HX_MAX_CSD_BYTES];
	uint8_t pps_buf[HX_MAX_CSD_BYTES];
	size_t sps_len = 0, pps_len = 0;

	for (int i = 0; i < num_sps && pos + 2 <= sz; i++) {
		uint16_t nlen = ((uint16_t)ext[pos] << 8) | ext[pos + 1];
		pos += 2;
		if (pos + nlen > sz)
			return false;
		append_nal(sps_buf, &sps_len, sizeof(sps_buf), ext + pos, nlen);
		pos += nlen;
	}
	if (pos >= sz)
		return false;
	int num_pps = ext[pos++];
	for (int i = 0; i < num_pps && pos + 2 <= sz; i++) {
		uint16_t nlen = ((uint16_t)ext[pos] << 8) | ext[pos + 1];
		pos += 2;
		if (pos + nlen > sz)
			return false;
		append_nal(pps_buf, &pps_len, sizeof(pps_buf), ext + pos, nlen);
		pos += nlen;
	}
	if (sps_len == 0 || pps_len == 0)
		return false;
	memcpy(d->csd0, sps_buf, sps_len);
	d->csd0_len = sps_len;
	memcpy(d->csd1, pps_buf, pps_len);
	d->csd1_len = pps_len;
	return true;
}

/* Parse HVCC extradata (configurationVersion == 1) -> csd0 (VPS+SPS+PPS). */
static bool parse_hvcc_extradata(struct hx_decoder *d, const uint8_t *ext, size_t sz)
{
	if (!ext || sz < 23 || ext[0] != 1)
		return false;
	d->nal_len_size = (ext[21] & 0x03) + 1;
	int num_arrays = ext[22];
	size_t pos = 23;
	uint8_t csd[HX_MAX_CSD_BYTES];
	size_t csd_len = 0;

	for (int i = 0; i < num_arrays && pos + 3 <= sz; i++) {
		pos++; /* array_completeness + nal_unit_type */
		uint16_t num_nals = ((uint16_t)ext[pos] << 8) | ext[pos + 1];
		pos += 2;
		for (int j = 0; j < num_nals && pos + 2 <= sz; j++) {
			uint16_t nlen = ((uint16_t)ext[pos] << 8) | ext[pos + 1];
			pos += 2;
			if (pos + nlen > sz)
				return false;
			append_nal(csd, &csd_len, sizeof(csd), ext + pos, nlen);
			pos += nlen;
		}
	}
	if (csd_len == 0)
		return false;
	memcpy(d->csd0, csd, csd_len);
	d->csd0_len = csd_len;
	d->csd1_len = 0;
	return true;
}

/* Extract Annex-B config NALs (H.264 SPS/PPS or HEVC VPS/SPS/PPS) and detect IDR. */
static bool scan_annexb_nals(int codec_id, const uint8_t *data, size_t size, uint8_t *csd0, size_t *csd0_len,
			     uint8_t *csd1, size_t *csd1_len, bool *is_keyframe)
{
	*csd0_len = 0;
	*csd1_len = 0;
	*is_keyframe = false;

	const uint8_t *end = data + size;
	size_t sc_len = 0;
	const uint8_t *p = find_start_code(data, end, &sc_len);
	while (p) {
		const uint8_t *nal = p + sc_len;
		size_t next_sc_len = 0;
		const uint8_t *next = find_start_code(nal, end, &next_sc_len);
		const uint8_t *nal_end = next ? next : end;
		while (nal_end > nal && nal_end[-1] == 0)
			nal_end--;
		const size_t nal_len = (size_t)(nal_end - nal);
		if (nal_len > 0) {
			if (codec_id == NDI_HX_AV_CODEC_ID_H264) {
				const int ntype = nal[0] & 0x1F;
				if (ntype == 7)
					append_nal(csd0, csd0_len, HX_MAX_CSD_BYTES, nal, nal_len);
				else if (ntype == 8)
					append_nal(csd1, csd1_len, HX_MAX_CSD_BYTES, nal, nal_len);
				else if (ntype == 5)
					*is_keyframe = true;
			} else if (codec_id == NDI_HX_AV_CODEC_ID_HEVC) {
				const int ntype = (nal[0] >> 1) & 0x3F;
				if (ntype == 32 || ntype == 33 || ntype == 34)
					append_nal(csd0, csd0_len, HX_MAX_CSD_BYTES, nal, nal_len);
				else if (ntype >= 16 && ntype <= 21)
					*is_keyframe = true;
			}
		}
		p = next;
		sc_len = next_sc_len;
	}
	if (codec_id == NDI_HX_AV_CODEC_ID_H264)
		return *csd0_len > 0 && *csd1_len > 0;
	return *csd0_len > 0;
}

/* Convert length-prefixed AVCC/HVCC packet into Annex-B if needed. */
static const uint8_t *ensure_annexb_packet(struct hx_decoder *d, const uint8_t *data, size_t size, size_t *out_size)
{
	if (d->nal_len_size <= 0 || has_annexb_start_code(data, size)) {
		*out_size = size;
		return data;
	}
	const size_t need = size + 256;
	if (d->annexb_cap < need) {
		uint8_t *nb = (uint8_t *)realloc(d->annexb_tmp, need);
		if (!nb) {
			*out_size = size;
			return data;
		}
		d->annexb_tmp = nb;
		d->annexb_cap = need;
	}
	size_t pos = 0, out = 0;
	while (pos + (size_t)d->nal_len_size <= size) {
		uint32_t nlen = 0;
		for (int i = 0; i < d->nal_len_size; i++)
			nlen = (nlen << 8) | data[pos + (size_t)i];
		pos += (size_t)d->nal_len_size;
		if (nlen == 0 || pos + nlen > size)
			break;
		if (out + 4 + nlen > d->annexb_cap) {
			size_t ncap = (out + 4 + nlen) * 2;
			uint8_t *nb = (uint8_t *)realloc(d->annexb_tmp, ncap);
			if (!nb)
				break;
			d->annexb_tmp = nb;
			d->annexb_cap = ncap;
		}
		d->annexb_tmp[out++] = 0;
		d->annexb_tmp[out++] = 0;
		d->annexb_tmp[out++] = 0;
		d->annexb_tmp[out++] = 1;
		memcpy(d->annexb_tmp + out, data + pos, nlen);
		out += nlen;
		pos += nlen;
	}
	if (out == 0) {
		*out_size = size;
		return data;
	}
	*out_size = out;
	return d->annexb_tmp;
}

static bool hx_configure_video(struct hx_decoder *d, const AVCodecContext7 *avctx)
{
	hx_release_codec(d);

	const char *mime = (d->codec_id == NDI_HX_AV_CODEC_ID_HEVC) ? "video/hevc" : "video/avc";
	AMediaCodec *mc = AMediaCodec_createDecoderByType(mime);
	if (!mc) {
		HX_LOGE("AMediaCodec_createDecoderByType(%s) failed", mime);
		return false;
	}

	const int init_w = (avctx && avctx->width > 0) ? avctx->width : 1920;
	const int init_h = (avctx && avctx->height > 0) ? avctx->height : 1080;

	/* Try COLOR_FormatYUV420Flexible first, then SemiPlanar, then default. */
	const int fmts[] = {MC_FMT_YUV420_FLEXIBLE, MC_FMT_YUV420_SEMIPLANAR, 0};
	media_status_t st = AMEDIA_ERROR_UNKNOWN;
	for (int i = 0; i < 3; i++) {
		AMediaFormat *fmt = AMediaFormat_new();
		AMediaFormat_setString(fmt, AMEDIAFORMAT_KEY_MIME, mime);
		AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_WIDTH, init_w);
		AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_HEIGHT, init_h);
		AMediaFormat_setInt32(fmt, "low-latency", 1);
		if (fmts[i] != 0)
			AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_COLOR_FORMAT, fmts[i]);
		if (d->csd0_len > 0)
			AMediaFormat_setBuffer(fmt, "csd-0", d->csd0, d->csd0_len);
		if (d->csd1_len > 0)
			AMediaFormat_setBuffer(fmt, "csd-1", d->csd1, d->csd1_len);
		st = AMediaCodec_configure(mc, fmt, NULL, NULL, 0);
		AMediaFormat_delete(fmt);
		if (st == AMEDIA_OK)
			break;
	}
	if (st != AMEDIA_OK) {
		HX_LOGE("AMediaCodec_configure(%s %dx%d) failed: %d", mime, init_w, init_h, (int)st);
		AMediaCodec_delete(mc);
		return false;
	}
	st = AMediaCodec_start(mc);
	if (st != AMEDIA_OK) {
		HX_LOGE("AMediaCodec_start(%s) failed: %d", mime, (int)st);
		AMediaCodec_delete(mc);
		return false;
	}
	d->codec = mc;
	d->configured = true;
	d->out_width = init_w;
	d->out_height = init_h;
	HX_LOGI("Configured MediaCodec %s decoder (%dx%d csd0=%zu csd1=%zu)", mime, init_w, init_h, d->csd0_len,
		d->csd1_len);
	return true;
}

static int aac_sample_rate_index(int sr)
{
	static const int rates[] = {96000, 88200, 64000, 48000, 44100, 32000, 24000,
				    22050, 16000, 12000, 11025, 8000,  7350};
	for (int i = 0; i < 13; i++) {
		if (rates[i] == sr)
			return i;
	}
	return 3; /* 48000 Hz default */
}

static bool hx_configure_aac(struct hx_decoder *d, int sample_rate, int channels, const uint8_t *asc, size_t asc_len)
{
	hx_release_codec(d);

	AMediaCodec *mc = AMediaCodec_createDecoderByType("audio/mp4a-latm");
	if (!mc) {
		HX_LOGE("AMediaCodec_createDecoderByType(audio/mp4a-latm) failed");
		return false;
	}
	if (sample_rate <= 0)
		sample_rate = 48000;
	if (channels <= 0)
		channels = 2;

	uint8_t synth_asc[2];
	if (!asc || asc_len < 2) {
		const int obj_type = 2; /* AAC-LC */
		const int sr_idx = aac_sample_rate_index(sample_rate);
		synth_asc[0] = (uint8_t)((obj_type << 3) | ((sr_idx >> 1) & 0x07));
		synth_asc[1] = (uint8_t)(((sr_idx & 0x01) << 7) | ((channels & 0x0F) << 3));
		asc = synth_asc;
		asc_len = 2;
	}

	AMediaFormat *fmt = AMediaFormat_new();
	AMediaFormat_setString(fmt, AMEDIAFORMAT_KEY_MIME, "audio/mp4a-latm");
	AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_SAMPLE_RATE, sample_rate);
	AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_CHANNEL_COUNT, channels);
	AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_IS_ADTS, 0);
	AMediaFormat_setBuffer(fmt, "csd-0", asc, asc_len);
	media_status_t st = AMediaCodec_configure(mc, fmt, NULL, NULL, 0);
	AMediaFormat_delete(fmt);
	if (st != AMEDIA_OK) {
		HX_LOGE("AMediaCodec_configure(audio/mp4a-latm %dHz %dch) failed: %d", sample_rate, channels, (int)st);
		AMediaCodec_delete(mc);
		return false;
	}
	st = AMediaCodec_start(mc);
	if (st != AMEDIA_OK) {
		HX_LOGE("AMediaCodec_start(audio/mp4a-latm) failed: %d", (int)st);
		AMediaCodec_delete(mc);
		return false;
	}
	d->codec = mc;
	d->configured = true;
	d->audio_sample_rate = sample_rate;
	d->audio_channels = channels;
	HX_LOGI("Configured MediaCodec AAC decoder (%dHz %dch asc=%zu)", sample_rate, channels, asc_len);
	return true;
}

static void hx_parse_output_format(struct hx_decoder *d, AMediaFormat *fmt)
{
	if (!fmt)
		return;
	if (d->is_audio) {
		int32_t v = 0;
		if (AMediaFormat_getInt32(fmt, AMEDIAFORMAT_KEY_SAMPLE_RATE, &v) && v > 0)
			d->audio_sample_rate = v;
		if (AMediaFormat_getInt32(fmt, AMEDIAFORMAT_KEY_CHANNEL_COUNT, &v) && v > 0)
			d->audio_channels = v;
		HX_LOGI("MediaCodec AAC output format: %dHz %dch", d->audio_sample_rate, d->audio_channels);
		return;
	}

	/* Reset all optional layout keys before parsing the new format. */
	hx_reset_output_format(d);
	int32_t v = 0;
	if (AMediaFormat_getInt32(fmt, AMEDIAFORMAT_KEY_WIDTH, &v))
		d->out_width = v;
	if (AMediaFormat_getInt32(fmt, AMEDIAFORMAT_KEY_HEIGHT, &v))
		d->out_height = v;
	if (AMediaFormat_getInt32(fmt, AMEDIAFORMAT_KEY_STRIDE, &v))
		d->out_stride = v;
	if (AMediaFormat_getInt32(fmt, "slice-height", &v))
		d->out_slice_height = v;
	if (AMediaFormat_getInt32(fmt, AMEDIAFORMAT_KEY_COLOR_FORMAT, &v))
		d->out_color_fmt = v;
	if (AMediaFormat_getInt32(fmt, "crop-left", &v))
		d->crop_left = v;
	if (AMediaFormat_getInt32(fmt, "crop-top", &v))
		d->crop_top = v;
	if (AMediaFormat_getInt32(fmt, "crop-right", &v))
		d->crop_right = v;
	if (AMediaFormat_getInt32(fmt, "crop-bottom", &v))
		d->crop_bottom = v;

	if (AMediaFormat_getInt32(fmt, "color-range", &v)) {
		if (v == 1)
			d->color_range = NDI_HX_AVCOL_RANGE_JPEG;
		else if (v == 2)
			d->color_range = NDI_HX_AVCOL_RANGE_MPEG;
	}
	if (AMediaFormat_getInt32(fmt, "color-standard", &v)) {
		if (v == 1) {
			d->colorspace = NDI_HX_AVCOL_SPC_BT709;
			d->color_primaries = NDI_HX_AVCOL_PRI_BT709;
		} else if (v == 2) {
			d->colorspace = NDI_HX_AVCOL_SPC_BT470BG;
			d->color_primaries = NDI_HX_AVCOL_PRI_BT470BG;
		} else if (v == 4) {
			d->colorspace = NDI_HX_AVCOL_SPC_SMPTE170M;
			d->color_primaries = NDI_HX_AVCOL_PRI_SMPTE170M;
		} else if (v == 6) {
			d->colorspace = NDI_HX_AVCOL_SPC_BT2020_NCL;
			d->color_primaries = NDI_HX_AVCOL_PRI_BT2020;
		}
	}
	if (AMediaFormat_getInt32(fmt, "color-transfer", &v)) {
		if (v == 3)
			d->color_trc = NDI_HX_AVCOL_TRC_BT709;
		else if (v == 6)
			d->color_trc = NDI_HX_AVCOL_TRC_SMPTE2084;
		else if (v == 7)
			d->color_trc = NDI_HX_AVCOL_TRC_ARIB_STD_B67;
	}

	HX_LOGI("MediaCodec video output format: %dx%d stride=%d slice=%d fmt=0x%x crop=(%d,%d,%d,%d)", d->out_width,
		d->out_height, d->out_stride, d->out_slice_height, d->out_color_fmt, d->crop_left, d->crop_top,
		d->crop_right, d->crop_bottom);
}

static bool slot_ensure_buf(struct hx_decoded_slot *slot, size_t need)
{
	if (slot->cap >= need && slot->buf)
		return true;
	const size_t alloc_sz = (need + 63U) & ~(size_t)63U;
	uint8_t *nb = (uint8_t *)realloc(slot->buf, alloc_sz);
	if (!nb)
		return false;
	slot->buf = nb;
	slot->cap = alloc_sz;
	return true;
}

static bool copy_video_output_to_slot(struct hx_decoder *d, const uint8_t *src, size_t src_size,
				      const AMediaCodecBufferInfo *info, struct hx_decoded_slot *slot)
{
	const int w_raw = d->out_width;
	const int h_raw = d->out_height;
	if (w_raw <= 0 || h_raw <= 0)
		return false;

	const int cl = d->crop_left > 0 ? d->crop_left : 0;
	const int ct = d->crop_top > 0 ? d->crop_top : 0;
	const int vis_w = (d->crop_right > cl && d->crop_right < w_raw) ? (d->crop_right - cl + 1) : (w_raw - cl);
	const int vis_h = (d->crop_bottom > ct && d->crop_bottom < h_raw) ? (d->crop_bottom - ct + 1) : (h_raw - ct);
	if (vis_w <= 0 || vis_h <= 0)
		return false;

	const int stride = d->out_stride > 0 ? d->out_stride : w_raw;
	const int slice_h = d->out_slice_height > 0 ? d->out_slice_height : h_raw;

	if ((size_t)info->offset >= src_size)
		return false;
	const uint8_t *base = src + info->offset;
	const size_t avail = src_size - (size_t)info->offset;

	slot->is_audio = false;
	slot->width = vis_w;
	slot->height = vis_h;
	slot->color_range = d->color_range;
	slot->color_primaries = d->color_primaries;
	slot->color_trc = d->color_trc;
	slot->colorspace = d->colorspace;
	slot->key = (info->flags & 1) != 0; /* BUFFER_FLAG_KEY_FRAME */
	hx_lookup_ts(d, info->presentationTimeUs, &slot->pts, &slot->dts, &slot->opaque);

	if (d->out_color_fmt == MC_FMT_YUV420_PLANAR) {
		const int uv_w = vis_w / 2;
		const int uv_h = vis_h / 2;
		const int uv_stride = stride / 2;
		const size_t y_bytes = (size_t)vis_w * (size_t)vis_h;
		const size_t uv_bytes = (size_t)uv_w * (size_t)uv_h;
		if (!slot_ensure_buf(slot, y_bytes + uv_bytes * 2))
			return false;

		const size_t src_y_plane = (size_t)slice_h * (size_t)stride;
		const size_t src_u_plane = (size_t)(slice_h / 2) * (size_t)uv_stride;
		if (src_y_plane + src_u_plane * 2 > avail)
			return false;

		uint8_t *dst_y = slot->buf;
		uint8_t *dst_u = dst_y + y_bytes;
		uint8_t *dst_v = dst_u + uv_bytes;
		for (int y = 0; y < vis_h; y++)
			memcpy(dst_y + (size_t)y * (size_t)vis_w, base + (size_t)(ct + y) * (size_t)stride + (size_t)cl,
			       (size_t)vis_w);
		for (int y = 0; y < uv_h; y++) {
			memcpy(dst_u + (size_t)y * (size_t)uv_w,
			       base + src_y_plane + (size_t)(ct / 2 + y) * (size_t)uv_stride + (size_t)(cl / 2),
			       (size_t)uv_w);
			memcpy(dst_v + (size_t)y * (size_t)uv_w,
			       base + src_y_plane + src_u_plane + (size_t)(ct / 2 + y) * (size_t)uv_stride +
				       (size_t)(cl / 2),
			       (size_t)uv_w);
		}
		slot->format = NDI_HX_AV_PIX_FMT_YUV420P;
		slot->linesize[0] = vis_w;
		slot->linesize[1] = uv_w;
		slot->linesize[2] = uv_w;
		slot->plane_off[0] = 0;
		slot->plane_off[1] = y_bytes;
		slot->plane_off[2] = y_bytes + uv_bytes;
		slot->used = y_bytes + uv_bytes * 2;
		return true;
	}

	if (d->out_color_fmt == MC_FMT_YUVP010) {
		/* Downshift 10-bit P010 to 8-bit NV12 for universal NDI UYVY/NV12 delivery. */
		const int uv_h = vis_h / 2;
		const size_t y_bytes = (size_t)vis_w * (size_t)vis_h;
		const size_t uv_bytes = (size_t)vis_w * (size_t)uv_h;
		if (!slot_ensure_buf(slot, y_bytes + uv_bytes))
			return false;

		const size_t stride_b = (size_t)stride * 2U;
		const size_t src_uv_off = (size_t)slice_h * stride_b;
		if (src_uv_off + (size_t)(ct / 2 + uv_h) * stride_b > avail)
			return false;

		uint8_t *dst_y = slot->buf;
		uint8_t *dst_uv = dst_y + y_bytes;
		for (int y = 0; y < vis_h; y++) {
			const uint16_t *sy = (const uint16_t *)(base + (size_t)(ct + y) * stride_b) + cl;
			uint8_t *dy = dst_y + (size_t)y * (size_t)vis_w;
			for (int x = 0; x < vis_w; x++)
				dy[x] = (uint8_t)(sy[x] >> 8);
		}
		for (int y = 0; y < uv_h; y++) {
			const uint16_t *suv = (const uint16_t *)(base + src_uv_off + (size_t)(ct / 2 + y) * stride_b) +
					      (cl & ~1);
			uint8_t *duv = dst_uv + (size_t)y * (size_t)vis_w;
			for (int x = 0; x < vis_w; x++)
				duv[x] = (uint8_t)(suv[x] >> 8);
		}
		slot->format = NDI_HX_AV_PIX_FMT_NV12;
		slot->linesize[0] = vis_w;
		slot->linesize[1] = vis_w;
		slot->linesize[2] = 0;
		slot->plane_off[0] = 0;
		slot->plane_off[1] = y_bytes;
		slot->plane_off[2] = 0;
		slot->used = y_bytes + uv_bytes;
		return true;
	}

	/* Default: semi-planar NV12 (COLOR_FormatYUV420SemiPlanar / QOMX / Flexible). */
	const int uv_h = vis_h / 2;
	const size_t y_bytes = (size_t)vis_w * (size_t)vis_h;
	const size_t uv_bytes = (size_t)vis_w * (size_t)uv_h;
	if (!slot_ensure_buf(slot, y_bytes + uv_bytes))
		return false;

	const size_t src_uv_off = (size_t)slice_h * (size_t)stride;
	if (src_uv_off + (size_t)(ct / 2 + uv_h - 1) * (size_t)stride + (size_t)(cl & ~1) + (size_t)vis_w > avail)
		return false;

	uint8_t *dst_y = slot->buf;
	uint8_t *dst_uv = dst_y + y_bytes;
	for (int y = 0; y < vis_h; y++)
		memcpy(dst_y + (size_t)y * (size_t)vis_w, base + (size_t)(ct + y) * (size_t)stride + (size_t)cl,
		       (size_t)vis_w);
	for (int y = 0; y < uv_h; y++)
		memcpy(dst_uv + (size_t)y * (size_t)vis_w,
		       base + src_uv_off + (size_t)(ct / 2 + y) * (size_t)stride + (size_t)(cl & ~1), (size_t)vis_w);

	slot->format = NDI_HX_AV_PIX_FMT_NV12;
	slot->linesize[0] = vis_w;
	slot->linesize[1] = vis_w;
	slot->linesize[2] = 0;
	slot->plane_off[0] = 0;
	slot->plane_off[1] = y_bytes;
	slot->plane_off[2] = 0;
	slot->used = y_bytes + uv_bytes;
	return true;
}

static bool copy_audio_output_to_slot(struct hx_decoder *d, const uint8_t *src, size_t src_size,
				      const AMediaCodecBufferInfo *info, struct hx_decoded_slot *slot)
{
	const int ch = d->audio_channels > 0 ? d->audio_channels : 2;
	if (ch > NDI_HX_AV_NUM_DATA_POINTERS || info->size <= 0 || (size_t)(info->offset + info->size) > src_size)
		return false;
	const int total_samples = info->size / (int)sizeof(int16_t);
	const int nb_samples = total_samples / ch;
	if (nb_samples <= 0)
		return false;

	const size_t plane_bytes = (size_t)nb_samples * sizeof(float);
	const size_t total_bytes = plane_bytes * (size_t)ch;
	if (!slot_ensure_buf(slot, total_bytes))
		return false;

	const int16_t *pcm = (const int16_t *)(src + info->offset);
	for (int c = 0; c < ch; c++) {
		float *dst = (float *)(slot->buf + (size_t)c * plane_bytes);
		for (int i = 0; i < nb_samples; i++)
			dst[i] = (float)pcm[i * ch + c] * (1.0f / 32768.0f);
	}

	slot->is_audio = true;
	slot->format = NDI_HX_AV_SAMPLE_FMT_FLTP;
	slot->nb_samples = nb_samples;
	slot->sample_rate = d->audio_sample_rate > 0 ? d->audio_sample_rate : 48000;
	slot->channels = ch;
	slot->linesize[0] = (int)plane_bytes;
	slot->used = total_bytes;
	hx_lookup_ts(d, info->presentationTimeUs, &slot->pts, &slot->dts, &slot->opaque);
	return true;
}

/* Drain available MediaCodec outputs into our small FIFO without blocking. */
static void hx_drain_outputs(struct hx_decoder *d, int64_t first_timeout_us)
{
	if (!d->codec || !d->configured)
		return;
	int64_t timeout = first_timeout_us;
	while (d->q_count < HX_OUT_QUEUE_CAP) {
		AMediaCodecBufferInfo info;
		memset(&info, 0, sizeof(info));
		const ssize_t idx = AMediaCodec_dequeueOutputBuffer(d->codec, &info, timeout);
		timeout = 0;
		if (idx == AMEDIACODEC_INFO_TRY_AGAIN_LATER)
			break;
		if (idx == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
			AMediaFormat *fmt = AMediaCodec_getOutputFormat(d->codec);
			hx_parse_output_format(d, fmt);
			if (fmt)
				AMediaFormat_delete(fmt);
			continue;
		}
		if (idx == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED)
			continue;
		if (idx < 0)
			break;

		if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM)
			d->eos_reached = true;

		size_t buf_sz = 0;
		uint8_t *buf = AMediaCodec_getOutputBuffer(d->codec, (size_t)idx, &buf_sz);
		if (buf && info.size > 0) {
			const int tail = (d->q_head + d->q_count) % HX_OUT_QUEUE_CAP;
			bool ok = d->is_audio ? copy_audio_output_to_slot(d, buf, buf_sz, &info, &d->q[tail])
					      : copy_video_output_to_slot(d, buf, buf_sz, &info, &d->q[tail]);
			if (ok) {
				d->q_count++;
				d->frames_out++;
			}
		}
		AMediaCodec_releaseOutputBuffer(d->codec, (size_t)idx, false);
	}
}

/* ==================== Exported FFmpeg 7.1 libavcodec API ==================== */

NDI_HX_EXPORT const AVCodec7 *avcodec_find_decoder(int id)
{
	switch (id) {
	case NDI_HX_AV_CODEC_ID_H264:
		return &k_codec_h264;
	case NDI_HX_AV_CODEC_ID_HEVC:
		return &k_codec_hevc;
	case NDI_HX_AV_CODEC_ID_AAC:
		return &k_codec_aac;
	default:
		return NULL;
	}
}

NDI_HX_EXPORT const AVCodec7 *avcodec_find_decoder_by_name(const char *name)
{
	if (!name)
		return NULL;
	if (strcmp(name, "h264") == 0 || strcmp(name, "h264_mediacodec") == 0 || strcmp(name, "avc") == 0)
		return &k_codec_h264;
	if (strcmp(name, "hevc") == 0 || strcmp(name, "h265") == 0 || strcmp(name, "hevc_mediacodec") == 0)
		return &k_codec_hevc;
	if (strcmp(name, "aac") == 0 || strcmp(name, "aac_mediacodec") == 0)
		return &k_codec_aac;
	return NULL;
}

NDI_HX_EXPORT AVCodecParameters7 *avcodec_parameters_alloc(void)
{
	AVCodecParameters7 *par = (AVCodecParameters7 *)calloc(1, sizeof(*par));
	if (!par)
		return NULL;
	par->codec_type = NDI_HX_AVMEDIA_TYPE_UNKNOWN;
	par->codec_id = NDI_HX_AV_CODEC_ID_NONE;
	par->format = -1;
	par->color_range = NDI_HX_AVCOL_RANGE_UNSPECIFIED;
	par->color_primaries = NDI_HX_AVCOL_PRI_UNSPECIFIED;
	par->color_trc = NDI_HX_AVCOL_TRC_UNSPECIFIED;
	par->color_space = NDI_HX_AVCOL_SPC_UNSPECIFIED;
	return par;
}

NDI_HX_EXPORT void avcodec_parameters_free(AVCodecParameters7 **par)
{
	if (!par || !*par)
		return;
	free(*par);
	*par = NULL;
}

NDI_HX_EXPORT AVCodecContext7 *avcodec_alloc_context3(const AVCodec7 *codec)
{
	AVCodecContext7 *ctx = (AVCodecContext7 *)calloc(1, sizeof(*ctx));
	if (!ctx)
		return NULL;
	ctx->codec = codec;
	ctx->codec_type = codec ? codec->type : NDI_HX_AVMEDIA_TYPE_UNKNOWN;
	ctx->codec_id = codec ? codec->id : NDI_HX_AV_CODEC_ID_NONE;
	ctx->pix_fmt = NDI_HX_AV_PIX_FMT_NONE;
	ctx->sw_pix_fmt = NDI_HX_AV_PIX_FMT_NONE;
	ctx->sample_fmt = NDI_HX_AV_SAMPLE_FMT_NONE;
	ctx->color_primaries = NDI_HX_AVCOL_PRI_UNSPECIFIED;
	ctx->color_trc = NDI_HX_AVCOL_TRC_UNSPECIFIED;
	ctx->colorspace = NDI_HX_AVCOL_SPC_UNSPECIFIED;
	ctx->color_range = NDI_HX_AVCOL_RANGE_UNSPECIFIED;
	return ctx;
}

NDI_HX_EXPORT void avcodec_free_context(AVCodecContext7 **avctx)
{
	if (!avctx || !*avctx)
		return;
	AVCodecContext7 *ctx = *avctx;
	if (ctx->priv_data) {
		hx_destroy_decoder((struct hx_decoder *)ctx->priv_data);
		ctx->priv_data = NULL;
	}
	free(ctx);
	*avctx = NULL;
}

NDI_HX_EXPORT int avcodec_parameters_to_context(AVCodecContext7 *codec, const AVCodecParameters7 *par)
{
	if (!codec || !par)
		return NDI_HX_AVERROR(EINVAL);
	codec->codec_type = par->codec_type;
	codec->codec_id = par->codec_id;
	codec->codec_tag = par->codec_tag;
	codec->bit_rate = par->bit_rate;
	codec->width = par->width;
	codec->height = par->height;
	codec->coded_width = par->width;
	codec->coded_height = par->height;
	codec->sample_aspect_ratio = par->sample_aspect_ratio;
	codec->framerate = par->framerate;
	codec->color_range = par->color_range;
	codec->color_primaries = par->color_primaries;
	codec->color_trc = par->color_trc;
	codec->colorspace = par->color_space;
	codec->sample_rate = par->sample_rate;
	codec->ch_layout = par->ch_layout;
	codec->frame_size = par->frame_size;
	if (par->codec_type == NDI_HX_AVMEDIA_TYPE_VIDEO)
		codec->pix_fmt = par->format;
	else if (par->codec_type == NDI_HX_AVMEDIA_TYPE_AUDIO)
		codec->sample_fmt = par->format;
	codec->extradata = par->extradata;
	codec->extradata_size = par->extradata_size;
	return 0;
}

NDI_HX_EXPORT int avcodec_open2(AVCodecContext7 *avctx, const AVCodec7 *codec, void **options)
{
	(void)options;
	if (!avctx)
		return NDI_HX_AVERROR(EINVAL);
	if (codec) {
		avctx->codec = codec;
		avctx->codec_id = codec->id;
		avctx->codec_type = codec->type;
	}
	if (avctx->codec_id != NDI_HX_AV_CODEC_ID_H264 && avctx->codec_id != NDI_HX_AV_CODEC_ID_HEVC &&
	    avctx->codec_id != NDI_HX_AV_CODEC_ID_AAC) {
		HX_LOGW("avcodec_open2: unsupported codec_id=%d", avctx->codec_id);
		return NDI_HX_AVERROR(ENOSYS);
	}

	if (avctx->priv_data) {
		hx_destroy_decoder((struct hx_decoder *)avctx->priv_data);
		avctx->priv_data = NULL;
	}

	struct hx_decoder *d = (struct hx_decoder *)calloc(1, sizeof(*d));
	if (!d)
		return NDI_HX_AVERROR(ENOMEM);
	d->codec_id = avctx->codec_id;
	d->is_audio = (avctx->codec_id == NDI_HX_AV_CODEC_ID_AAC);
	avctx->priv_data = d;

	if (d->is_audio) {
		const int sr = avctx->sample_rate > 0 ? avctx->sample_rate : 48000;
		const int ch = avctx->ch_layout.nb_channels > 0 ? avctx->ch_layout.nb_channels : 2;
		if (avctx->extradata && avctx->extradata_size >= 2) {
			if (!hx_configure_aac(d, sr, ch, avctx->extradata, (size_t)avctx->extradata_size))
				return NDI_HX_AVERROR(EINVAL);
		}
		avctx->sample_fmt = NDI_HX_AV_SAMPLE_FMT_FLTP;
		return 0;
	}

	/* Video: parse extradata if already provided; otherwise configure lazily on first IDR+SPS/PPS. */
	if (avctx->extradata && avctx->extradata_size > 0) {
		bool parsed = false;
		if (d->codec_id == NDI_HX_AV_CODEC_ID_H264)
			parsed = parse_avcc_extradata(d, avctx->extradata, (size_t)avctx->extradata_size);
		else if (d->codec_id == NDI_HX_AV_CODEC_ID_HEVC)
			parsed = parse_hvcc_extradata(d, avctx->extradata, (size_t)avctx->extradata_size);
		if (!parsed) {
			bool kf = false;
			parsed = scan_annexb_nals(d->codec_id, avctx->extradata, (size_t)avctx->extradata_size,
						  d->csd0, &d->csd0_len, d->csd1, &d->csd1_len, &kf);
		}
		if (parsed)
			hx_configure_video(d, avctx);
	}
	avctx->pix_fmt = NDI_HX_AV_PIX_FMT_NV12;
	return 0;
}

NDI_HX_EXPORT AVPacket7 *av_packet_alloc(void)
{
	AVPacket7 *pkt = (AVPacket7 *)calloc(1, sizeof(*pkt));
	if (!pkt)
		return NULL;
	pkt->pts = NDI_HX_AV_NOPTS_VALUE;
	pkt->dts = NDI_HX_AV_NOPTS_VALUE;
	pkt->pos = -1;
	return pkt;
}

NDI_HX_EXPORT void av_packet_free(AVPacket7 **pkt)
{
	if (!pkt || !*pkt)
		return;
	free(*pkt);
	*pkt = NULL;
}

NDI_HX_EXPORT int avcodec_send_packet(AVCodecContext7 *avctx, const AVPacket7 *avpkt)
{
	if (!avctx || !avctx->priv_data)
		return NDI_HX_AVERROR(EINVAL);
	struct hx_decoder *d = (struct hx_decoder *)avctx->priv_data;

	/* Flush / EOF packet */
	if (!avpkt || !avpkt->data || avpkt->size <= 0) {
		if (!d->configured || !d->codec || d->eos_sent)
			return 0;
		const ssize_t idx = AMediaCodec_dequeueInputBuffer(d->codec, 10000);
		if (idx >= 0) {
			AMediaCodec_queueInputBuffer(d->codec, (size_t)idx, 0, 0, 0,
						     AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
			d->eos_sent = true;
		}
		hx_drain_outputs(d, 5000);
		return 0;
	}

	const uint8_t *payload = avpkt->data;
	size_t payload_sz = (size_t)avpkt->size;

	if (d->is_audio) {
		/* Handle optional ADTS header (7 or 9 bytes). */
		int sr = avctx->sample_rate > 0 ? avctx->sample_rate : 48000;
		int ch = avctx->ch_layout.nb_channels > 0 ? avctx->ch_layout.nb_channels : 2;
		if (payload_sz >= 7 && payload[0] == 0xFF && (payload[1] & 0xF0) == 0xF0) {
			static const int adts_rates[] = {96000, 88200, 64000, 48000, 44100, 32000, 24000,
							 22050, 16000, 12000, 11025, 8000,  7350};
			const int sr_idx = (payload[2] >> 2) & 0x0F;
			if (sr_idx < 13)
				sr = adts_rates[sr_idx];
			const int adts_ch = ((payload[2] & 0x01) << 2) | ((payload[3] >> 6) & 0x03);
			if (adts_ch > 0)
				ch = adts_ch;
			const size_t hdr = (payload[1] & 0x01) ? 7 : 9;
			if (payload_sz > hdr) {
				payload += hdr;
				payload_sz -= hdr;
			}
		}
		if (!d->configured) {
			if (!hx_configure_aac(d, sr, ch, avctx->extradata,
					      avctx->extradata_size > 0 ? (size_t)avctx->extradata_size : 0))
				return NDI_HX_AVERROR(EINVAL);
		}
	} else {
		payload = ensure_annexb_packet(d, payload, payload_sz, &payload_sz);
		uint8_t new_csd0[HX_MAX_CSD_BYTES], new_csd1[HX_MAX_CSD_BYTES];
		size_t new_csd0_len = 0, new_csd1_len = 0;
		bool is_kf = false;
		const bool has_csd = scan_annexb_nals(d->codec_id, payload, payload_sz, new_csd0, &new_csd0_len,
						      new_csd1, &new_csd1_len, &is_kf);
		if (has_csd) {
			const bool changed = (new_csd0_len != d->csd0_len || new_csd1_len != d->csd1_len ||
					      (new_csd0_len > 0 && memcmp(new_csd0, d->csd0, new_csd0_len) != 0) ||
					      (new_csd1_len > 0 && memcmp(new_csd1, d->csd1, new_csd1_len) != 0));
			if (!d->configured || changed) {
				memcpy(d->csd0, new_csd0, new_csd0_len);
				d->csd0_len = new_csd0_len;
				memcpy(d->csd1, new_csd1, new_csd1_len);
				d->csd1_len = new_csd1_len;
				if (!hx_configure_video(d, avctx))
					return NDI_HX_AVERROR(EINVAL);
			}
		} else if (!d->configured) {
			/* Wait for first keyframe carrying SPS/PPS before starting MediaCodec. */
			return 0;
		}
	}

	/* Drain any pending output first so MediaCodec input buffers stay free. */
	hx_drain_outputs(d, 0);

	ssize_t in_idx = -1;
	for (int attempt = 0; attempt < 5; attempt++) {
		in_idx = AMediaCodec_dequeueInputBuffer(d->codec, 10000);
		if (in_idx >= 0)
			break;
		hx_drain_outputs(d, 2000);
	}
	if (in_idx < 0)
		return NDI_HX_AVERROR(EAGAIN);

	size_t in_cap = 0;
	uint8_t *in_buf = AMediaCodec_getInputBuffer(d->codec, (size_t)in_idx, &in_cap);
	if (!in_buf || payload_sz > in_cap) {
		HX_LOGW("MediaCodec input buffer too small (%zu > %zu)", payload_sz, in_cap);
		AMediaCodec_queueInputBuffer(d->codec, (size_t)in_idx, 0, 0, 0, 0);
		return NDI_HX_AVERROR(EINVAL);
	}

	memcpy(in_buf, payload, payload_sz);
	const int64_t seq = hx_record_ts(d, avpkt->pts, avpkt->dts, avpkt->opaque);
	const uint32_t flags = (avpkt->flags & NDI_HX_AV_PKT_FLAG_KEY) ? 1U : 0U;
	const media_status_t st = AMediaCodec_queueInputBuffer(d->codec, (size_t)in_idx, 0, payload_sz, seq, flags);
	if (st != AMEDIA_OK) {
		HX_LOGW("AMediaCodec_queueInputBuffer failed: %d", (int)st);
		return NDI_HX_AVERROR(EIO);
	}
	d->frames_in++;
	hx_drain_outputs(d, 0);
	return 0;
}

NDI_HX_EXPORT int avcodec_receive_frame(AVCodecContext7 *avctx, AVFrame7 *frame)
{
	if (!avctx || !avctx->priv_data || !frame)
		return NDI_HX_AVERROR(EINVAL);
	struct hx_decoder *d = (struct hx_decoder *)avctx->priv_data;
	if (!d->configured || !d->codec)
		return NDI_HX_AVERROR(EAGAIN);

	if (d->q_count == 0)
		hx_drain_outputs(d, 8000);
	if (d->q_count == 0)
		return d->eos_reached ? NDI_HX_AVERROR_EOF : NDI_HX_AVERROR(EAGAIN);

	struct hx_decoded_slot *slot = &d->q[d->q_head];
	uint8_t *dst = ndi_hx_frame_ensure_buf(frame, slot->used);
	if (!dst)
		return NDI_HX_AVERROR(ENOMEM);
	memcpy(dst, slot->buf, slot->used);

	/* Reset public AVFrame7 fields without disturbing the trailing box header. */
	ndi_hx_frame_reset_defaults(frame);
	frame->pts = slot->pts;
	frame->pkt_dts = slot->dts;
	frame->best_effort_timestamp = (slot->pts != NDI_HX_AV_NOPTS_VALUE) ? slot->pts : slot->dts;
	frame->opaque = slot->opaque;

	if (slot->is_audio) {
		const size_t plane_bytes = (size_t)slot->linesize[0];
		for (int c = 0; c < slot->channels && c < NDI_HX_AV_NUM_DATA_POINTERS; c++)
			frame->data[c] = dst + (size_t)c * plane_bytes;
		frame->linesize[0] = slot->linesize[0];
		frame->extended_data = frame->data;
		frame->format = slot->format;
		frame->nb_samples = slot->nb_samples;
		frame->sample_rate = slot->sample_rate;
		ndi_hx_channel_layout_default_inline(&frame->ch_layout, slot->channels);
		avctx->sample_rate = slot->sample_rate;
		avctx->sample_fmt = slot->format;
		avctx->ch_layout = frame->ch_layout;
	} else {
		frame->width = slot->width;
		frame->height = slot->height;
		frame->format = slot->format;
		frame->data[0] = dst + slot->plane_off[0];
		frame->linesize[0] = slot->linesize[0];
		frame->data[1] = dst + slot->plane_off[1];
		frame->linesize[1] = slot->linesize[1];
		if (slot->format == NDI_HX_AV_PIX_FMT_YUV420P) {
			frame->data[2] = dst + slot->plane_off[2];
			frame->linesize[2] = slot->linesize[2];
		}
		frame->extended_data = frame->data;
		frame->color_range = slot->color_range;
		frame->color_primaries = slot->color_primaries;
		frame->color_trc = slot->color_trc;
		frame->colorspace = slot->colorspace;
		if (slot->key) {
			frame->key_frame = 1;
			frame->pict_type = NDI_HX_AV_PICTURE_TYPE_I;
			frame->flags |= NDI_HX_AV_FRAME_FLAG_KEY;
		} else {
			frame->pict_type = NDI_HX_AV_PICTURE_TYPE_P;
		}
		avctx->width = slot->width;
		avctx->height = slot->height;
		avctx->coded_width = slot->width;
		avctx->coded_height = slot->height;
		avctx->pix_fmt = slot->format;
		avctx->color_range = slot->color_range;
		avctx->color_primaries = slot->color_primaries;
		avctx->color_trc = slot->color_trc;
		avctx->colorspace = slot->colorspace;
	}

	d->q_head = (d->q_head + 1) % HX_OUT_QUEUE_CAP;
	d->q_count--;
	return 0;
}

NDI_HX_EXPORT const char *obs_ndi_hx_bridge_version(void)
{
	return "obs-ndi-hx-avcodec61-mediacodec/1";
}
