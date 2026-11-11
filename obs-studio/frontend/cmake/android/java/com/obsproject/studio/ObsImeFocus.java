package com.obsproject.studio;

import java.util.concurrent.atomic.AtomicLong;

/** Changes whenever Qt changes its focused input object, including within one QtWindow. */
public final class ObsImeFocus {
    private static final AtomicLong generation = new AtomicLong();
    private ObsImeFocus() {}
    public static void focusChanged() { generation.incrementAndGet(); }
    public static long generation() { return generation.get(); }
    public static boolean shouldSkip(boolean visible, boolean focused, boolean sameView,
                                     long requestGeneration, long currentGeneration,
                                     long lastGeneration, boolean sameOptions) {
        return visible && focused && sameView && sameOptions && requestGeneration == currentGeneration
                && requestGeneration == lastGeneration;
    }
}
