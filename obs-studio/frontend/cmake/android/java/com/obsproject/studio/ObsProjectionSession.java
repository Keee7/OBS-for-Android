/* SPDX-License-Identifier: GPL-2.0-or-later */
package com.obsproject.studio;

/** One consent owns one display; surfaces can be detached and replaced. */
final class ObsProjectionSession<S> {
    interface Display<S> {
        void resize(int width, int height, int dpi);
        void surface(S surface);
        void release();
    }
    interface Factory<S> {
        Display<S> create(S surface, int width, int height, int dpi);
    }
    private final Factory<S> factory;
    private Display<S> display;
    private boolean attempted, closed, attached;
    private int contentWidth, contentHeight;

    ObsProjectionSession(Factory<S> factory) { this.factory = factory; }

    synchronized void attach(S surface, int width, int height, int dpi) {
        if (closed || surface == null || width <= 0 || height <= 0 || dpi <= 0)
            throw new IllegalStateException("Invalid or stopped screen capture session");
        if (display == null) {
            if (attempted) throw new IllegalStateException("Screen capture consent already consumed; request it again");
            attempted = true;
            display = factory.create(surface, width, height, dpi);
            if (display == null) throw new IllegalStateException("createVirtualDisplay returned null");
        } else {
            display.resize(width, height, dpi);
            display.surface(surface);
        }
        attached = true;
    }

    synchronized void detach() {
        attached = false;
        if (display != null) display.surface(null);
    }

    synchronized void release() {
        closed = true;
        attached = false;
        contentWidth = contentHeight = 0;
        Display<S> old = display;
        display = null;
        if (old != null) old.release();
    }

    synchronized boolean hasDisplay() { return display != null; }
    synchronized boolean isAttached() { return attached; }

    /** Android 14+ reports the selected app's bounds, independently of our Surface size. */
    synchronized boolean contentResized(int width, int height) {
        if (closed || width <= 0 || height <= 0 ||
            (width == contentWidth && height == contentHeight)) return false;
        contentWidth = width;
        contentHeight = height;
        return true;
    }

    synchronized int[] captureSize(int[] displaySize) {
        if (closed || contentWidth <= 0 || contentHeight <= 0) return displaySize;
        // Keep display density/rotation. Content dimensions already include rotation.
        return new int[] {contentWidth, contentHeight, displaySize[2], displaySize[3]};
    }
}
