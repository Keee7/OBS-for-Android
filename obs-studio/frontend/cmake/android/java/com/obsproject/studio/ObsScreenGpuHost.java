package com.obsproject.studio;

import android.content.Context;
import android.graphics.SurfaceTexture;
import android.opengl.GLES11Ext;
import android.opengl.GLES20;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.Surface;
import java.util.concurrent.atomic.AtomicBoolean;

/** SurfaceTexture consumer for the existing MediaProjection session. */
public final class ObsScreenGpuHost {
    private static final String TAG = "OBS-screen-gpu";
    private final AtomicBoolean frame = new AtomicBoolean();
    private final int[] binding = new int[1];
    private final SurfaceTexture texture;
    private final Surface surface;
    private boolean closed;

    private ObsScreenGpuHost(int id, int width, int height) {
        texture = new SurfaceTexture(id);
        Surface candidate;
        try {
            texture.setDefaultBufferSize(width, height);
            texture.setOnFrameAvailableListener(t -> frame.set(true), new Handler(Looper.getMainLooper()));
            candidate = new Surface(texture);
        } catch (RuntimeException e) {
            texture.release();
            throw e;
        }
        surface = candidate;
    }
    public static ObsScreenGpuHost create(Context context, int id, int width, int height, int dpi) {
        if (context == null || id == 0 || width <= 0 || height <= 0) return null;
        ObsScreenGpuHost host = null;
        try {
            host = new ObsScreenGpuHost(id, width, height);
            if (ObsProjectionHost.startCapture(context, host.surface, width, height, dpi)) return host;
        } catch (RuntimeException e) {
            Log.e(TAG, "GPU projection setup failed", e);
        }
        if (host != null) host.close();
        return null;
    }
    /** Called on the GL thread with OBS's EGL context current. */
    public synchronized int update(float[] matrix) {
        if (closed) return -1;
        if (!frame.getAndSet(false)) return 0;
        GLES20.glGetIntegerv(0x8D67, binding, 0);
        try {
            texture.updateTexImage();
            texture.getTransformMatrix(matrix);
            return 1;
        } catch (RuntimeException e) {
            Log.e(TAG, "updateTexImage failed", e);
            return -1;
        } finally {
            GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, binding[0]);
        }
    }
    public synchronized void close() {
        if (closed) return;
        closed = true;
        ObsProjectionHost.stopCaptureForSurface(surface);
        texture.setOnFrameAvailableListener(null);
        surface.release();
        texture.release();
    }
}
