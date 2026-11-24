/* SPDX-License-Identifier: GPL-2.0-or-later */
package com.obsproject.studio;

import android.media.MediaCodec;
import android.media.MediaCodecInfo;
import android.media.MediaCodecList;
import android.media.MediaFormat;
import android.os.Build;
import android.util.Log;
import java.io.IOException;
import java.util.ArrayList;
import java.util.LinkedHashMap;

/** Strict candidates only: Surface/YUV input, real hardware, requested size/rate and mode. */
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
    private static final LinkedHashMap<String, Boolean> fullRangeCache = new LinkedHashMap<>();
    private static MediaCodecInfo[] codecs;
    private static final int COLOR_FORMAT_YUV_P010 = 54;

    private static int encoderProfile(String mime) {
        switch (mime) {
        case "video/avc": return 0;
        case "video/hevc": return MediaCodecInfo.CodecProfileLevel.HEVCProfileMain;
        case "video/av01": return MediaCodecInfo.CodecProfileLevel.AV1ProfileMain8;
        case "video/x-vnd.on2.vp9": return MediaCodecInfo.CodecProfileLevel.VP9Profile0;
        default: return -1;
        }
    }

    private static boolean hasColorFormat(MediaCodecInfo.CodecCapabilities caps, int target) {
        for (int color : caps.colorFormats)
            if (color == target) return true;
        return false;
    }

    private static boolean supportsEncoderProfile(MediaCodecInfo.CodecCapabilities caps, int profile, boolean p010,
            boolean allowByteBuffer) {
        if (caps.getVideoCapabilities() == null || caps.getEncoderCapabilities() == null)
            return false;
        if (!p010) {
            boolean surface = hasColorFormat(caps, MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface);
            boolean rawYuv = allowByteBuffer && (
                hasColorFormat(caps, MediaCodecInfo.CodecCapabilities.COLOR_FormatYUV420SemiPlanar)
                || hasColorFormat(caps, MediaCodecInfo.CodecCapabilities.COLOR_FormatYUV420Planar)
                || hasColorFormat(caps, MediaCodecInfo.CodecCapabilities.COLOR_FormatYUV420Flexible));
            if (!surface && !rawYuv) return false;
        }
        // Some codecs support P010 at configure time without listing it here.
        if (profile == 0) return true;
        for (MediaCodecInfo.CodecProfileLevel p : caps.profileLevels)
            if (p.profile == profile) return true;
        return false;
    }

    public static synchronized boolean hasHardwareEncoder(String mime, int mode) {
        return hasHardwareEncoderProfile(mime, mode, 0, false);
    }

    public static synchronized boolean hasHardwareEncoderProfile(String mime, int mode, int requestedProfile, boolean p010) {
        int profile = requestedProfile > 0 ? requestedProfile : encoderProfile(mime);
        if (profile < 0 || (p010 && Build.VERSION.SDK_INT < 33)) return false;
        if (codecs == null) codecs = new MediaCodecList(MediaCodecList.REGULAR_CODECS).getCodecInfos();
        for (MediaCodecInfo info : codecs) {
            if (!info.isEncoder() || !info.isHardwareAccelerated() || info.isSoftwareOnly() || info.isAlias()) continue;
            for (String type : info.getSupportedTypes()) {
                if (!mime.equalsIgnoreCase(type)) continue;
                try {
                    MediaCodecInfo.CodecCapabilities caps = info.getCapabilitiesForType(type);
                    if (supportsEncoderProfile(caps, profile, p010, !p010)
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
        return findEncodersProfile(mime, width, height, fps, bitrate, mode, 0, false);
    }
    public static synchronized String[] findEncodersProfile(String mime, int width, int height, double fps,
            int bitrate, int mode, int requestedProfile, boolean p010) {
        return findEncodersProfile(mime, width, height, fps, bitrate, mode, requestedProfile, p010, false);
    }
    public static synchronized String[] findEncodersProfile(String mime, int width, int height, double fps,
            int bitrate, int mode, int requestedProfile, boolean p010, boolean allowByteBuffer) {
        ArrayList<String> result = new ArrayList<>();
        int profile = requestedProfile > 0 ? requestedProfile : encoderProfile(mime);
        if (Build.VERSION.SDK_INT < 29 || width <= 0 || height <= 0 || !Double.isFinite(fps) || fps <= 0
                || bitrate < 0 || mode < 0 || mode > 2 || profile < 0 || (p010 && Build.VERSION.SDK_INT < 33))
            return new String[0];
        String key = mime + ":" + width + ":" + height + ":" + fps + ":" + bitrate + ":" + mode + ":" + profile + ":" + p010 + ":" + allowByteBuffer;
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
                    boolean supported = supportsEncoderProfile(caps, profile, p010, allowByteBuffer)
                        && (mode == 0 || bitrate == 0 || video.getBitrateRange().contains(bitrate))
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
                        + " bitrate=" + bitrate + " hardware=true software=false raw=" + allowByteBuffer + " eligible=" + supported);
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

    private static int alignProbeDimension(int preferred, android.util.Range<Integer> range, int alignment) {
        int v = range != null ? range.clamp(preferred) : preferred;
        int align = Math.max(2, alignment);
        if ((align & (align - 1)) == 0)
            v = ((v + align - 1) / align) * align;
        else
            v = ((v + 1) / 2) * 2;
        if (range != null)
            v = range.clamp(v);
        if ((v & 1) != 0)
            v = (range != null && v - 1 >= range.getLower()) ? v - 1 : v + 1;
        return v;
    }

    private static boolean probeCodecFullRangeFormat(String name, String mime, MediaCodecInfo.CodecCapabilities caps,
            int profile, boolean p010, int colorFormat) {
        MediaCodecInfo.VideoCapabilities video = caps.getVideoCapabilities();
        MediaCodecInfo.EncoderCapabilities enc = caps.getEncoderCapabilities();
        int w = alignProbeDimension(640, video.getSupportedWidths(), video.getWidthAlignment());
        int h = alignProbeDimension(360, video.getSupportedHeights(), video.getHeightAlignment());
        MediaFormat f = MediaFormat.createVideoFormat(mime, w, h);
        f.setInteger(MediaFormat.KEY_COLOR_FORMAT, colorFormat);
        f.setFloat(MediaFormat.KEY_FRAME_RATE, 30f);
        f.setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 1);
        if (profile > 0)
            f.setInteger(MediaFormat.KEY_PROFILE, profile);
        int bitrateMode = -1;
        if (enc.isBitrateModeSupported(MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CBR))
            bitrateMode = MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CBR;
        else if (enc.isBitrateModeSupported(MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_VBR))
            bitrateMode = MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_VBR;
        else if (enc.isBitrateModeSupported(MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CQ))
            bitrateMode = MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CQ;
        if (bitrateMode >= 0) {
            f.setInteger(MediaFormat.KEY_BITRATE_MODE, bitrateMode);
            if (bitrateMode != MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CQ)
                f.setInteger(MediaFormat.KEY_BIT_RATE, video.getBitrateRange().clamp(2000000));
        }
        int transfer = (p010 && profile == MediaCodecInfo.CodecProfileLevel.HEVCProfileMain10HDR10)
            ? MediaFormat.COLOR_TRANSFER_ST2084
            : MediaFormat.COLOR_TRANSFER_SDR_VIDEO;
        f.setInteger(MediaFormat.KEY_COLOR_STANDARD,
            p010 && transfer != MediaFormat.COLOR_TRANSFER_SDR_VIDEO ? MediaFormat.COLOR_STANDARD_BT2020
                                                                     : MediaFormat.COLOR_STANDARD_BT709);
        f.setInteger(MediaFormat.KEY_COLOR_TRANSFER, transfer);
        f.setInteger(MediaFormat.KEY_COLOR_RANGE, MediaFormat.COLOR_RANGE_FULL);
        MediaCodec codec = null;
        try {
            codec = MediaCodec.createByCodecName(name);
            codec.configure(f, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE);
            MediaFormat input = codec.getInputFormat();
            boolean accepted = input != null && input.containsKey(MediaFormat.KEY_COLOR_RANGE)
                && input.getInteger(MediaFormat.KEY_COLOR_RANGE) == MediaFormat.COLOR_RANGE_FULL;
            Log.i(TAG, "Full-range probe " + name + " cf=0x" + Integer.toHexString(colorFormat)
                + " profile=" + profile + " accepted=" + accepted);
            return accepted;
        } catch (IOException | IllegalArgumentException | IllegalStateException | SecurityException e) {
            Log.w(TAG, "Full-range probe rejected " + name + " cf=0x" + Integer.toHexString(colorFormat), e);
            return false;
        } finally {
            if (codec != null) {
                try {
                    codec.release();
                } catch (RuntimeException ignored) {
                }
            }
        }
    }

    public static synchronized boolean hasHardwareFullRangeSupport(String mime, int requestedProfile, boolean p010) {
        int profile = requestedProfile > 0 ? requestedProfile : encoderProfile(mime);
        if (Build.VERSION.SDK_INT < 29 || profile < 0 || (p010 && Build.VERSION.SDK_INT < 33))
            return false;
        String key = mime + ":" + profile + ":" + p010;
        Boolean cached = fullRangeCache.get(key);
        if (cached != null)
            return cached;
        if (codecs == null)
            codecs = new MediaCodecList(MediaCodecList.REGULAR_CODECS).getCodecInfos();
        boolean supported = false;
        for (MediaCodecInfo info : codecs) {
            if (!info.isEncoder() || !info.isHardwareAccelerated() || info.isSoftwareOnly() || info.isAlias())
                continue;
            for (String type : info.getSupportedTypes()) {
                if (!mime.equalsIgnoreCase(type))
                    continue;
                try {
                    MediaCodecInfo.CodecCapabilities caps = info.getCapabilitiesForType(type);
                    if (!supportsEncoderProfile(caps, profile, p010, !p010))
                        continue;
                    int[] formats = p010
                        ? new int[] { COLOR_FORMAT_YUV_P010 }
                        : new int[] {
                            MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface,
                            MediaCodecInfo.CodecCapabilities.COLOR_FormatYUV420SemiPlanar,
                            MediaCodecInfo.CodecCapabilities.COLOR_FormatYUV420Planar
                        };
                    for (int cf : formats) {
                        if (probeCodecFullRangeFormat(info.getName(), mime, caps, profile, p010, cf)) {
                            supported = true;
                            break;
                        }
                    }
                    if (supported)
                        break;
                } catch (IllegalArgumentException | SecurityException e) {
                    Log.w(TAG, "Ignoring codec during full-range probe " + info.getName(), e);
                }
            }
            if (supported)
                break;
        }
        if (fullRangeCache.size() >= 32)
            fullRangeCache.remove(fullRangeCache.keySet().iterator().next());
        fullRangeCache.put(key, supported);
        return supported;
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
