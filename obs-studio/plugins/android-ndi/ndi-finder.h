/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NDI_FINDER_MAX_SOURCES 64
#define NDI_FINDER_NAME_LEN 192
#define NDI_FINDER_URL_LEN 128

struct ndi_discovered_source {
	char name[NDI_FINDER_NAME_LEN];
	char url[NDI_FINDER_URL_LEN];
	bool from_sdk;
};

/* Reference-counted shared background NDI finder (combines Android NsdManager
 * _ndi._tcp. peers + user extra_ips + NDIlib_find_instance_t). */
void ndi_finder_acquire(void);
void ndi_finder_release(void);

/* Updates per-source manual sender IP list (comma-separated, e.g. "192.168.1.50").
 * All active sources' IPs are merged & deduplicated so multiple NDI sources do not
 * overwrite each other. */
void ndi_finder_set_source_extra_ips(const void *owner_key, const char *extra_ips);
void ndi_finder_remove_source_extra_ips(const void *owner_key);

/* Convenience wrapper for global/default owner_key (NULL). */
void ndi_finder_set_extra_ips(const char *extra_ips);

/* Triggers an immediate background refresh. */
void ndi_finder_refresh(void);

/* Copies current discovered sources snapshot into out[0..max-1]. */
int ndi_finder_get_sources(struct ndi_discovered_source *out, int max);

#ifdef __cplusplus
}
#endif
