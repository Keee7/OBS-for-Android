/* SPDX-License-Identifier: GPL-2.0-or-later */
package com.obsproject.studio;

import android.media.MediaCodecInfo;
import android.media.MediaCodecList;
import android.media.MediaFormat;
import android.os.Build;
import android.util.Log;
import java.util.ArrayList;
import java.util.LinkedHashMap;

/** Strict candidates only: Surface input, real hardware, requested size/rate and CBR. */
public final class ObsCodecCapabilities {
    public static String findHardwareDecoder(String mime) {
        for (MediaCodecInfo info : new MediaCodecList(MediaCodecList.REGULAR_CODECS).getCodecInfos()) {
            try {
                if (info.isEncoder() || info.isAlias() || !info.isHardwareAccelerated() || info.isSoftwareOnly()) continue;
                for (String type : info.getSupportedTypes()) {
                    if (!type.equalsIgnoreCase(mime)) continue;
                    MediaCodecInfo.CodecCapabilities caps = info.getCapabilitiesForType(type);
                    if (caps.isFeatureRequired(MediaCodecInfo.CodecCapabilities.FEATURE_SecurePlayback)
                        || caps.isFeatureRequired(MediaCodecInfo.CodecCapabilities.FEATURE_TunneledPlayback)) continue;
                    Log.i(TAG, "hardware decoder mime=" + mime + " component=" + info.getName());
                    return info.getName();
                }
            } catch (IllegalArgumentException | SecurityException e) { Log.w(TAG, "Ignoring decoder", e); }
        }
        return "";
    }
    private static final String TAG = "ObsCodecCapabilities";
    private ObsCodecCapabilities() {}
    private static final LinkedHashMap<String, String[]> cache = new LinkedHashMap<>();
    private static MediaCodecInfo[] codecs;

    private static int encoderProfile(String mime) {
        switch (mime) {
        case "video/avc": return 0;
        case "video/hevc": return MediaCodecInfo.CodecProfileLevel.HEVCProfileMain;
        case "video/av01": return MediaCodecInfo.CodecProfileLevel.AV1ProfileMain8;
        case "video/x-vnd.on2.vp9": return MediaCodecInfo.CodecProfileLevel.VP9Profile0;
        default: return -1;
        }
    }

    private static boolean supportsSurfaceProfile(MediaCodecInfo.CodecCapabilities caps, int profile) {
        boolean surface = false;
        for (int color : caps.colorFormats)
            if (color == MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface) surface = true;
        if (!surface || caps.getVideoCapabilities() == null || caps.getEncoderCapabilities() == null)
            return false;
        if (profile == 0) return true;
        for (MediaCodecInfo.CodecProfileLevel p : caps.profileLevels)
            if (p.profile == profile) return true;
        return false;
    }

    public static synchronized boolean hasHardwareEncoder(String mime, int mode) {
        int profile = encoderProfile(mime);
        if (profile < 0) return false;
        if (codecs == null) codecs = new MediaCodecList(MediaCodecList.REGULAR_CODECS).getCodecInfos();
        for (MediaCodecInfo info : codecs) {
            if (!info.isEncoder() || !info.isHardwareAccelerated() || info.isSoftwareOnly() || info.isAlias()) continue;
            for (String type : info.getSupportedTypes()) {
                if (!mime.equalsIgnoreCase(type)) continue;
                try {
                    MediaCodecInfo.CodecCapabilities caps = info.getCapabilitiesForType(type);
                    if (supportsSurfaceProfile(caps, profile)
                            && (mode < 0 || caps.getEncoderCapabilities().isBitrateModeSupported(mode))) return true;
                } catch (IllegalArgumentException | SecurityException e) {
                    Log.w(TAG, "Ignoring codec " + info.getName(), e);
                }
            }
        }
        return false;
    }

    public static String[] findAvcEncoders(int width, int height, double fps) {
        return findEncoders("video/avc", width, height, fps, 0);
    }
    public static synchronized String[] findEncoders(String mime, int width, int height, double fps, int bitrate) {
        return findEncodersMode(mime, width, height, fps, bitrate, 2);
    }
    public static synchronized String[] findEncodersMode(String mime, int width, int height, double fps, int bitrate, int mode) {
        ArrayList<String> result = new ArrayList<>();
        int profile = encoderProfile(mime);
        if (Build.VERSION.SDK_INT < 29 || width <= 0 || height <= 0 || !Double.isFinite(fps) || fps <= 0
                || bitrate < 0 || mode < 0 || mode > 2 || profile < 0) return new String[0];
        String key = mime + ":" + width + ":" + height + ":" + fps + ":" + bitrate + ":" + mode;
        if (cache.containsKey(key)) return cache.get(key).clone();
        if (codecs == null) codecs = new MediaCodecList(MediaCodecList.REGULAR_CODECS).getCodecInfos();
        for (MediaCodecInfo info : codecs) {
            try {
                if (!info.isEncoder() || !info.isHardwareAccelerated() || info.isSoftwareOnly() || info.isAlias()) continue;
                for (String type : info.getSupportedTypes()) {
                    if (!mime.equalsIgnoreCase(type)) continue;
                    MediaCodecInfo.CodecCapabilities caps = info.getCapabilitiesForType(type);
                    MediaCodecInfo.VideoCapabilities video = caps.getVideoCapabilities();
                    MediaCodecInfo.EncoderCapabilities enc = caps.getEncoderCapabilities();
                    boolean supported = supportsSurfaceProfile(caps, profile) && (mode == 0 || bitrate == 0 || video.getBitrateRange().contains(bitrate))
                        && video.areSizeAndRateSupported(width, height, fps)
                        && enc.isBitrateModeSupported(mode);
                    if (supported && profile > 0) {
                        MediaFormat f = MediaFormat.createVideoFormat(mime, width, height);
                        f.setInteger(MediaFormat.KEY_PROFILE, profile);
                        f.setFloat(MediaFormat.KEY_FRAME_RATE, (float)fps);
                        if (bitrate > 0) f.setInteger(MediaFormat.KEY_BIT_RATE, bitrate);
                        supported = caps.isFormatSupported(f);
                    }
                    Log.i(TAG, info.getName() + " " + mime + " " + width + "x" + height + "@" + fps
                        + " bitrate=" + bitrate + " hardware=true software=false eligible=" + supported);
                    if (supported) result.add(info.getName());
                }
            } catch (IllegalArgumentException | SecurityException e) {
                Log.w(TAG, "Ignoring codec " + info.getName(), e);
            }
        }
        String[] names = result.toArray(new String[0]);
        if (cache.size() >= 32) cache.remove(cache.keySet().iterator().next());
        cache.put(key, names);
        return names.clone();
    }
    /** Returns mode support and the advertised complexity and CQ quality ranges. */
    public static int[] encoderControls(String name, String mime, int mode) {
        try {
            MediaCodecInfo info = null;
            for (MediaCodecInfo candidate : new MediaCodecList(MediaCodecList.REGULAR_CODECS).getCodecInfos())
                if (candidate.getName().equals(name)) { info = candidate; break; }
            if (info == null || !info.isEncoder() || !info.isHardwareAccelerated() || info.isSoftwareOnly())
                return new int[0];
            MediaCodecInfo.EncoderCapabilities enc = info.getCapabilitiesForType(mime).getEncoderCapabilities();
            if (enc == null || !enc.isBitrateModeSupported(mode)) return new int[0];
            android.util.Range<Integer> c = enc.getComplexityRange();
            android.util.Range<Integer> q = enc.getQualityRange();
            return new int[] { c.getLower(), c.getUpper(), q.getLower(), q.getUpper() };
        } catch (IllegalArgumentException | SecurityException e) {
            Log.w(TAG, "Cannot query encoder controls for " + name, e);
            return new int[0];
        }
    }

}
