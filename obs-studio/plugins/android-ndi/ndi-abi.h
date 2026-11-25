/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

/*
 * Minimal ABI-stable C definitions for the NDI® v5/v6 runtime (libndi.so).
 * When built with -DOBS_ANDROID_NDI_SDK_DIR=<path> containing the official
 * Processing.NDI.Lib.h header, the official header is included directly.
 * Otherwise these ABI-frozen struct/enum definitions allow building and loading
 * libndi.so dynamically at runtime via dlopen/dlsym without bundling proprietary
 * SDK files in the GPL repository.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef OBS_ANDROID_NDI_HAVE_SDK_HEADERS
#include <Processing.NDI.Lib.h>
#else

#ifdef __cplusplus
extern "C" {
#endif

#define NDI_FOURCC(ch0, ch1, ch2, ch3)                                                                                \
	((uint32_t)(uint8_t)(ch0) | ((uint32_t)(uint8_t)(ch1) << 8) | ((uint32_t)(uint8_t)(ch2) << 16) |             \
	 ((uint32_t)(uint8_t)(ch3) << 24))

typedef void *NDIlib_find_instance_t;
typedef void *NDIlib_recv_instance_t;
typedef void *NDIlib_send_instance_t;

typedef enum NDIlib_frame_type_e {
	NDIlib_frame_type_none = 0,
	NDIlib_frame_type_video = 1,
	NDIlib_frame_type_audio = 2,
	NDIlib_frame_type_metadata = 3,
	NDIlib_frame_type_error = 4,
	NDIlib_frame_type_status_change = 100,
	NDIlib_frame_type_source_change = 101,
	NDIlib_frame_type_max = 0x7fffffff
} NDIlib_frame_type_e;

typedef enum NDIlib_FourCC_video_type_e {
	NDIlib_FourCC_video_type_UYVY = NDI_FOURCC('U', 'Y', 'V', 'Y'),
	NDIlib_FourCC_video_type_UYVA = NDI_FOURCC('U', 'Y', 'V', 'A'),
	NDIlib_FourCC_video_type_P216 = NDI_FOURCC('P', '2', '1', '6'),
	NDIlib_FourCC_video_type_PA16 = NDI_FOURCC('P', 'A', '1', '6'),
	NDIlib_FourCC_video_type_YV12 = NDI_FOURCC('Y', 'V', '1', '2'),
	NDIlib_FourCC_video_type_I420 = NDI_FOURCC('I', '4', '2', '0'),
	NDIlib_FourCC_video_type_NV12 = NDI_FOURCC('N', 'V', '1', '2'),
	NDIlib_FourCC_video_type_BGRA = NDI_FOURCC('B', 'G', 'R', 'A'),
	NDIlib_FourCC_video_type_BGRX = NDI_FOURCC('B', 'G', 'R', 'X'),
	NDIlib_FourCC_video_type_RGBA = NDI_FOURCC('R', 'G', 'B', 'A'),
	NDIlib_FourCC_video_type_RGBX = NDI_FOURCC('R', 'G', 'B', 'X'),
	NDIlib_FourCC_video_type_max = 0x7fffffff
} NDIlib_FourCC_video_type_e;

typedef enum NDIlib_FourCC_audio_type_e {
	NDIlib_FourCC_audio_type_FLTP = NDI_FOURCC('F', 'L', 'T', 'p'),
	NDIlib_FourCC_audio_type_max = 0x7fffffff
} NDIlib_FourCC_audio_type_e;

typedef enum NDIlib_frame_format_type_e {
	NDIlib_frame_format_type_interleaved = 0,
	NDIlib_frame_format_type_progressive = 1,
	NDIlib_frame_format_type_field_0 = 2,
	NDIlib_frame_format_type_field_1 = 3,
	NDIlib_frame_format_type_max = 0x7fffffff
} NDIlib_frame_format_type_e;

typedef enum NDIlib_recv_bandwidth_e {
	NDIlib_recv_bandwidth_metadata_only = -10,
	NDIlib_recv_bandwidth_audio_only = 10,
	NDIlib_recv_bandwidth_lowest = 0,
	NDIlib_recv_bandwidth_highest = 100,
	NDIlib_recv_bandwidth_max = 0x7fffffff
} NDIlib_recv_bandwidth_e;

typedef enum NDIlib_recv_color_format_e {
	NDIlib_recv_color_format_BGRX_BGRA = 0,
	NDIlib_recv_color_format_UYVY_BGRA = 1,
	NDIlib_recv_color_format_RGBX_RGBA = 2,
	NDIlib_recv_color_format_UYVY_RGBA = 3,
	NDIlib_recv_color_format_fastest = 100,
	NDIlib_recv_color_format_best = 101,
	NDIlib_recv_color_format_max = 0x7fffffff
} NDIlib_recv_color_format_e;

#define NDIlib_send_timecode_synthesize INT64_MAX
#define NDIlib_recv_timestamp_undefined INT64_MAX

typedef struct NDIlib_source_t {
	const char *p_ndi_name;
	union {
		const char *p_url_address;
		const char *p_ip_address;
	};
} NDIlib_source_t;

typedef struct NDIlib_find_create_t {
	bool show_local_sources;
	const char *p_groups;
	const char *p_extra_ips;
} NDIlib_find_create_t;

typedef struct NDIlib_recv_create_v3_t {
	NDIlib_source_t source_to_connect_to;
	NDIlib_recv_color_format_e color_format;
	NDIlib_recv_bandwidth_e bandwidth;
	bool allow_video_fields;
	const char *p_ndi_recv_name;
} NDIlib_recv_create_v3_t;

typedef struct NDIlib_video_frame_v2_t {
	int xres, yres;
	NDIlib_FourCC_video_type_e FourCC;
	int frame_rate_N, frame_rate_D;
	float picture_aspect_ratio;
	NDIlib_frame_format_type_e frame_format_type;
	int64_t timecode;
	uint8_t *p_data;
	union {
		int line_stride_in_bytes;
		int data_size_in_bytes;
	};
	const char *p_metadata;
	int64_t timestamp;
} NDIlib_video_frame_v2_t;

typedef struct NDIlib_audio_frame_v3_t {
	int sample_rate;
	int no_channels;
	int no_samples;
	int64_t timecode;
	NDIlib_FourCC_audio_type_e FourCC;
	uint8_t *p_data;
	union {
		int channel_stride_in_bytes;
		int data_size_in_bytes;
	};
	const char *p_metadata;
	int64_t timestamp;
} NDIlib_audio_frame_v3_t;

typedef struct NDIlib_metadata_frame_t {
	int length;
	int64_t timecode;
	char *p_data;
} NDIlib_metadata_frame_t;

typedef struct NDIlib_send_create_t {
	const char *p_ndi_name;
	const char *p_groups;
	bool clock_video, clock_audio;
} NDIlib_send_create_t;

#ifdef __cplusplus
}
#endif

#endif /* OBS_ANDROID_NDI_HAVE_SDK_HEADERS */
