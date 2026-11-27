/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <obs-module.h>
#include <util/dstr.h>

void screen_audio_acquire(obs_source_t *source);
void screen_audio_release(obs_source_t *source);
void screen_audio_format_state(struct dstr *out);
