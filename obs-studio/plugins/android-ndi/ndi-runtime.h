/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "ndi-abi.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ndi_api {
	bool (*initialize)(void);
	void (*destroy)(void);
	const char *(*version)(void);

	NDIlib_find_instance_t (*find_create_v2)(const NDIlib_find_create_t *p_create_settings);
	void (*find_destroy)(NDIlib_find_instance_t p_instance);
	const NDIlib_source_t *(*find_get_current_sources)(NDIlib_find_instance_t p_instance, uint32_t *p_no_sources);
	bool (*find_wait_for_sources)(NDIlib_find_instance_t p_instance, uint32_t timeout_in_ms);

	NDIlib_recv_instance_t (*recv_create_v3)(const NDIlib_recv_create_v3_t *p_create_settings);
	void (*recv_destroy)(NDIlib_recv_instance_t p_instance);
	void (*recv_connect)(NDIlib_recv_instance_t p_instance, const NDIlib_source_t *p_src);
	NDIlib_frame_type_e (*recv_capture_v3)(NDIlib_recv_instance_t p_instance, NDIlib_video_frame_v2_t *p_video_data,
					       NDIlib_audio_frame_v3_t *p_audio_data,
					       NDIlib_metadata_frame_t *p_metadata, uint32_t timeout_in_ms);
	void (*recv_free_video_v2)(NDIlib_recv_instance_t p_instance, const NDIlib_video_frame_v2_t *p_video_data);
	void (*recv_free_audio_v3)(NDIlib_recv_instance_t p_instance, const NDIlib_audio_frame_v3_t *p_audio_data);
	void (*recv_free_metadata)(NDIlib_recv_instance_t p_instance, const NDIlib_metadata_frame_t *p_metadata);

	/* Optional send-side symbols (for Phase 2 NDI Program Output). */
	NDIlib_send_instance_t (*send_create)(const NDIlib_send_create_t *p_create_settings);
	void (*send_destroy)(NDIlib_send_instance_t p_instance);
	void (*send_send_video_async_v2)(NDIlib_send_instance_t p_instance,
					 const NDIlib_video_frame_v2_t *p_video_data);
	void (*send_send_audio_v3)(NDIlib_send_instance_t p_instance, const NDIlib_audio_frame_v3_t *p_audio_data);
	int (*send_get_no_connections)(NDIlib_send_instance_t p_instance, uint32_t timeout_in_ms);
};

/* Acquire a reference to the dynamically loaded NDI runtime (dlopen libndi.so).
 * Returns pointer to the resolved API table on success, or NULL if libndi.so is
 * missing or fails to initialize. */
const struct ndi_api *ndi_runtime_acquire(void);
void ndi_runtime_release(void);

/* Copies diagnostic info about the loaded runtime (or last load error). */
bool ndi_runtime_is_loaded(void);
bool ndi_runtime_has_hx_bridge(void);
void ndi_runtime_get_info(char *version_out, size_t version_size, char *path_out, size_t path_size, char *error_out,
			  size_t error_size);

/* Recommended drop-in path inside app private storage for sideloading libndi.so. */
void ndi_runtime_default_dropin_path(char *out, size_t size);

#ifdef __cplusplus
}
#endif
