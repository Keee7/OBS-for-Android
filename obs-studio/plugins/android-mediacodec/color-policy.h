/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Android MediaFormat color constants (API 28+).
 * Kept standalone so host regression tests compile without libobs/NDK headers. */
#define MC_COLOR_RANGE_FULL 1
#define MC_COLOR_RANGE_LIMITED 2

#define MC_COLOR_STANDARD_BT709 1
#define MC_COLOR_STANDARD_BT601_PAL 2
#define MC_COLOR_STANDARD_BT601_NTSC 4
#define MC_COLOR_STANDARD_BT2020 6

#define MC_COLOR_TRANSFER_SDR_VIDEO 3
#define MC_COLOR_TRANSFER_ST2084 6
#define MC_COLOR_TRANSFER_HLG 7

enum mc_cs_kind {
	MC_CS_DEFAULT = 0,
	MC_CS_709,
	MC_CS_601,
	MC_CS_SRGB,
	MC_CS_2100_PQ,
	MC_CS_2100_HLG,
	MC_CS_UNKNOWN,
};

enum mc_range_kind {
	MC_RANGE_DEFAULT = 0,
	MC_RANGE_PARTIAL,
	MC_RANGE_FULL,
	MC_RANGE_UNKNOWN,
};

struct mc_color_config {
	enum mc_cs_kind colorspace;
	enum mc_range_kind range;
	int32_t color_standard;
	int32_t color_transfer;
	int32_t color_range;
	bool pq;
};

static inline enum mc_range_kind mc_resolve_range_kind(enum mc_range_kind range)
{
	return range == MC_RANGE_FULL ? MC_RANGE_FULL : MC_RANGE_PARTIAL;
}

static inline bool mc_cs_is_sdr(enum mc_cs_kind cs)
{
	return cs == MC_CS_DEFAULT || cs == MC_CS_709 || cs == MC_CS_601 || cs == MC_CS_SRGB;
}

static inline bool mc_cs_is_hdr(enum mc_cs_kind cs)
{
	return cs == MC_CS_2100_PQ || cs == MC_CS_2100_HLG;
}

static inline bool mc_resolve_color_config(bool p010, enum mc_cs_kind global_cs, enum mc_cs_kind actual_cs,
					   bool actual_fmt_p010_or_i010, enum mc_range_kind actual_range,
					   struct mc_color_config *out, const char **error_out)
{
	if (actual_range != MC_RANGE_DEFAULT && actual_range != MC_RANGE_PARTIAL && actual_range != MC_RANGE_FULL) {
		if (error_out)
			*error_out = "Unsupported MediaCodec video color range";
		return false;
	}
	if (p010) {
		if ((!mc_cs_is_sdr(actual_cs) && !mc_cs_is_hdr(actual_cs)) || !actual_fmt_p010_or_i010) {
			if (error_out)
				*error_out = "MediaCodec Main10 requires P010 or I010 with SDR or Rec. 2100";
			return false;
		}
	} else if (mc_cs_is_hdr(global_cs) || mc_cs_is_hdr(actual_cs)) {
		if (error_out)
			*error_out = "HDR requires a compatible 10-bit encoder. Use hardware HEVC Main10 when available, or software x265/AV1.";
		return false;
	} else if (!mc_cs_is_sdr(actual_cs) || !mc_cs_is_sdr(global_cs)) {
		if (error_out)
			*error_out = "This MediaCodec encoder requires an SDR color space.";
		return false;
	}

	out->colorspace = actual_cs;
	out->range = mc_resolve_range_kind(actual_range);
	out->color_range = out->range == MC_RANGE_FULL ? MC_COLOR_RANGE_FULL : MC_COLOR_RANGE_LIMITED;
	out->pq = actual_cs == MC_CS_2100_PQ;
	if (actual_cs == MC_CS_2100_PQ) {
		out->color_standard = MC_COLOR_STANDARD_BT2020;
		out->color_transfer = MC_COLOR_TRANSFER_ST2084;
	} else if (actual_cs == MC_CS_2100_HLG) {
		out->color_standard = MC_COLOR_STANDARD_BT2020;
		out->color_transfer = MC_COLOR_TRANSFER_HLG;
	} else if (actual_cs == MC_CS_601) {
		out->color_standard = MC_COLOR_STANDARD_BT601_NTSC;
		out->color_transfer = MC_COLOR_TRANSFER_SDR_VIDEO;
	} else {
		out->color_standard = MC_COLOR_STANDARD_BT709;
		out->color_transfer = MC_COLOR_TRANSFER_SDR_VIDEO;
	}
	if (error_out)
		*error_out = NULL;
	return true;
}

static inline bool mc_color_standard_matches(int32_t expected, int32_t reported)
{
	if (reported == expected)
		return true;
	/* BT.601 PAL and NTSC share the Rec. 601 color matrix in YUV encoding. */
	if (expected == MC_COLOR_STANDARD_BT601_NTSC && reported == MC_COLOR_STANDARD_BT601_PAL)
		return true;
	return false;
}

static inline bool mc_color_format_accepted(bool p010, int32_t expected_std, int32_t expected_tr, int32_t expected_range,
					    bool has_std, int32_t reported_std, bool has_tr, int32_t reported_tr,
					    bool has_range, int32_t reported_range, const char **error_out,
					    bool *range_mismatch)
{
	if (range_mismatch)
		*range_mismatch = false;
	/* Full range is non-default and must be confirmed explicitly; otherwise a driver
	 * that drops KEY_COLOR_RANGE would silently clamp input/output to Limited range. */
	const bool range_ok = (has_range && reported_range == expected_range) ||
			      (expected_range != MC_COLOR_RANGE_FULL &&
			       (!has_range || (!p010 && reported_range == 0)));
	if (!range_ok) {
		if (range_mismatch)
			*range_mismatch = true;
		if (error_out)
			*error_out = p010 ? "MediaCodec changed the requested Main10 color range"
					  : "MediaCodec encoder rejected the requested video color range";
		return false;
	}
	if (p010 && ((has_std && !mc_color_standard_matches(expected_std, reported_std)) ||
		     (has_tr && reported_tr != expected_tr))) {
		if (error_out)
			*error_out = "MediaCodec changed the requested Main10 color information";
		return false;
	}
	if (error_out)
		*error_out = NULL;
	return true;
}
