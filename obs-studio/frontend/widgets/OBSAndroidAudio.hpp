/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

/* Registers the Java audio-device bridge with libobs so the AAudio input source
 * can list and select inputs (including USB capture-card audio). */
bool OBSAndroidAudioInit();
void OBSAndroidAudioShutdown();
