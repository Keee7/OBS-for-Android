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
    private final boolean recreateOnResize;
    private Display<S> display;
    private boolean attempted, closed, attached;
    private int attachedWidth, attachedHeight, attachedDpi;
    private int contentWidth, contentHeight;
    private int lastDisplayWidth, lastDisplayHeight, lastDisplayRot = -1;
    private boolean contentUpdatedSinceDisplayFlip;
    private boolean contentWasFullScreen;

    ObsProjectionSession(Factory<S> factory) { this(factory, false); }

    ObsProjectionSession(Factory<S> factory, boolean recreateOnResize) {
        this.factory = factory;
        this.recreateOnResize = recreateOnResize;
    }

    synchronized void attach(S surface, int width, int height, int dpi) {
        if (closed || surface == null || width <= 0 || height <= 0 || dpi <= 0)
            throw new IllegalStateException("Invalid or stopped screen capture session");
        if (display != null && recreateOnResize &&
            (width != attachedWidth || height != attachedHeight || dpi != attachedDpi)) {
            Display<S> old = display;
            display = null;
            attached = false;
            old.release();
        }
        if (display == null) {
            if (!recreateOnResize && attempted)
                throw new IllegalStateException("Screen capture consent already consumed; request it again");
            attempted = true;
            display = factory.create(surface, width, height, dpi);
            if (display == null) throw new IllegalStateException("createVirtualDisplay returned null");
        } else {
            display.resize(width, height, dpi);
            display.surface(surface);
        }
        attachedWidth = width;
        attachedHeight = height;
        attachedDpi = dpi;
        attached = true;
    }

    synchronized void detach() {
        attached = false;
        if (display != null) display.surface(null);
    }

    synchronized void release() {
        closed = true;
        attached = false;
        attachedWidth = attachedHeight = attachedDpi = 0;
        contentWidth = contentHeight = 0;
        lastDisplayWidth = lastDisplayHeight = 0;
        lastDisplayRot = -1;
        contentUpdatedSinceDisplayFlip = false;
        contentWasFullScreen = false;
        Display<S> old = display;
        display = null;
        if (old != null) old.release();
    }

    synchronized boolean hasDisplay() { return display != null; }
    synchronized boolean isAttached() { return attached; }

    /** Android 14+ reports the selected app's bounds, independently of our Surface size. */
    synchronized boolean contentResized(int width, int height) {
        if (closed || width <= 0 || height <= 0) return false;
        contentUpdatedSinceDisplayFlip = true;
        if (width == contentWidth && height == contentHeight) return false;
        contentWidth = width;
        contentHeight = height;
        return true;
    }

    synchronized int[] captureSize(int[] displaySize) {
        if (closed || displaySize == null || displaySize.length < 4) return displaySize;
        final int dw = displaySize[0], dh = displaySize[1], drot = displaySize[3];
        if (dw > 0 && dh > 0) {
            final boolean displayRotated = lastDisplayWidth > 0 && lastDisplayHeight > 0 &&
                (drot != lastDisplayRot || (dw > dh) != (lastDisplayWidth > lastDisplayHeight));
            if (displayRotated) {
                // Drop stale pre-rotation content bounds (or letterboxed transitional sizes on
                // full-screen capture) so rotating portrait <-> landscape never traps capture at
                // a smaller or wrong-orientation size.
                if (!contentUpdatedSinceDisplayFlip ||
                    (contentWidth > 0 && contentHeight > 0 &&
                     (contentWidth > dw || contentHeight > dh ||
                      (contentWasFullScreen && (contentWidth != dw || contentHeight != dh))))) {
                    contentWidth = 0;
                    contentHeight = 0;
                    contentWasFullScreen = false;
                }
            }
            lastDisplayWidth = dw;
            lastDisplayHeight = dh;
            lastDisplayRot = drot;
            contentUpdatedSinceDisplayFlip = false;
        }
        if (contentWidth <= 0 || contentHeight <= 0) return displaySize;
        // Ignore stale/unrotated content bounds that cannot fit in the current display orientation
        // (e.g. a portrait full-screen callback left over after rotating to landscape).
        if (dw > 0 && dh > 0) {
            if (contentWidth > dw || contentHeight > dh) {
                return displaySize;
            }
            if (contentWidth == dw && contentHeight == dh) {
                contentWasFullScreen = true;
            }
        }
        // Keep display density/rotation. Content dimensions already include rotation.
        return new int[] {contentWidth, contentHeight, displaySize[2], displaySize[3]};
    }
}
