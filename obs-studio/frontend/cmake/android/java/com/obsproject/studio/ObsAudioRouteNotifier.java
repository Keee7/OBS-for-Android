/* SPDX-License-Identifier: GPL-2.0-or-later */
package com.obsproject.studio;

public final class ObsAudioRouteNotifier implements Runnable {
    public static native void nativeOnRouteChanged();

    @Override
    public void run() {
        nativeOnRouteChanged();
    }
}
