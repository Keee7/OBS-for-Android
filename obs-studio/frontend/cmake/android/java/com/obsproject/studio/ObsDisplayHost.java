package com.obsproject.studio;

import android.graphics.SurfaceTexture;
import android.os.Handler;
import android.os.Looper;
import android.view.Surface;
import android.view.TextureView;
import android.view.View;
import android.view.ViewGroup;

import java.util.ArrayDeque;

/** An OBS render target in the owning QtWindow's normal Android view hierarchy. */
public final class ObsDisplayHost implements TextureView.SurfaceTextureListener {
    private static final Handler ui = new Handler(Looper.getMainLooper());

    private static final class Retired {
        final int generation;
        final Surface surface;
        final SurfaceTexture texture;

        Retired(int generation, Surface surface, SurfaceTexture texture) {
            this.generation = generation;
            this.surface = surface;
            this.texture = texture;
        }

        void release() {
            surface.release();
            texture.release();
        }
    }

    private final long id;
    private final ViewGroup parent;
    private final ArrayDeque<Retired> retired = new ArrayDeque<>();
    private TextureView view;
    private Surface surface;
    private int generation;
    private boolean closed;

    private static native void nativeSurfaceChanged(long id, Surface surface, int generation);

    public ObsDisplayHost(ViewGroup parent, long id) {
        this.parent = parent;
        this.id = id;
        ui.post(this::mount);
    }

    private synchronized void mount() {
        if (closed)
            return;

        view = new TextureView(parent.getContext());
        view.setFocusable(false);
        view.setImportantForAccessibility(View.IMPORTANT_FOR_ACCESSIBILITY_NO);
        view.setSurfaceTextureListener(this);
        // Use QtLayout's own LayoutParams type. This preview fills its QtWindow,
        // which already follows Qt's geometry, insets and stacking order.
        parent.addView(view);
        ViewGroup.LayoutParams params = view.getLayoutParams();
        params.width = ViewGroup.LayoutParams.MATCH_PARENT;
        params.height = ViewGroup.LayoutParams.MATCH_PARENT;
        view.setLayoutParams(params);
    }

    @Override
    public synchronized void onSurfaceTextureAvailable(SurfaceTexture texture, int width, int height) {
        if (closed)
            return;
        surface = new Surface(texture);
        nativeSurfaceChanged(id, surface, ++generation);
    }

    @Override
    public void onSurfaceTextureSizeChanged(SurfaceTexture texture, int width, int height) {}

    @Override
    public synchronized boolean onSurfaceTextureDestroyed(SurfaceTexture texture) {
        if (surface == null)
            return true;

        Surface old = surface;
        surface = null;
        if (closed) {
            old.release();
            return true;
        }

        retired.add(new Retired(++generation, old, texture));
        nativeSurfaceChanged(id, null, generation);
        // Keep the buffer queue connected until OBS destroys its EGL swapchain.
        return false;
    }

    @Override
    public void onSurfaceTextureUpdated(SurfaceTexture texture) {}

    public synchronized void releaseRetired(int throughGeneration) {
        while (!retired.isEmpty() && retired.peek().generation <= throughGeneration)
            retired.remove().release();
    }

    public synchronized void close() {
        closed = true;
        releaseRetired(Integer.MAX_VALUE);
        ui.post(() -> {
            synchronized (ObsDisplayHost.this) {
                if (view != null) {
                    parent.removeView(view);
                    view = null;
                }
            }
        });
    }
}
