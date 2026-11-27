/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * Minimal FFmpeg 7.x libavutil-ndi.so.59 ABI provider for NDI|HX MediaCodec
 * decoding on Android. Exports the exact libavutil symbols resolved dynamically
 * by libndi.so (av_frame_alloc, av_frame_free, av_channel_layout_default).
 */

#include "ndi-hx-avabi7.h"

#include <stdlib.h>

#define NDI_HX_EXPORT __attribute__((visibility("default")))

NDI_HX_EXPORT AVFrame7 *av_frame_alloc(void)
{
	ndi_hx_frame_box_t *box = (ndi_hx_frame_box_t *)calloc(1, sizeof(*box));
	if (!box)
		return NULL;
	ndi_hx_frame_reset_defaults(&box->frame);
	box->magic = NDI_HX_FRAME_MAGIC;
	return &box->frame;
}

NDI_HX_EXPORT void av_frame_unref(AVFrame7 *frame)
{
	if (!frame)
		return;
	ndi_hx_frame_box_t *box = (ndi_hx_frame_box_t *)frame;
	if (box->magic == NDI_HX_FRAME_MAGIC) {
		uint8_t *kept_buf = box->owned_buf;
		size_t kept_cap = box->owned_cap;
		ndi_hx_frame_reset_defaults(&box->frame);
		box->magic = NDI_HX_FRAME_MAGIC;
		box->owned_buf = kept_buf;
		box->owned_cap = kept_cap;
	} else {
		ndi_hx_frame_reset_defaults(frame);
	}
}

NDI_HX_EXPORT void av_frame_free(AVFrame7 **frame)
{
	if (!frame || !*frame)
		return;
	ndi_hx_frame_box_t *box = (ndi_hx_frame_box_t *)(*frame);
	if (box->magic == NDI_HX_FRAME_MAGIC) {
		free(box->owned_buf);
		box->owned_buf = NULL;
		box->owned_cap = 0;
		box->magic = 0;
	}
	free(box);
	*frame = NULL;
}

NDI_HX_EXPORT void av_channel_layout_default(AVChannelLayout7 *ch_layout, int nb_channels)
{
	ndi_hx_channel_layout_default_inline(ch_layout, nb_channels);
}

NDI_HX_EXPORT const char *obs_ndi_hx_avutil_version(void)
{
	return "obs-ndi-hx-avutil59-mediacodec/1";
}
