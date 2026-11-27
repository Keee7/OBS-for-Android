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

/** Strict candidates first, with controlled relaxed hardware fallback for vendor capability quirks. */
public final class ObsCodecCapabilities {
    private static final String TAG = "ObsCodecCapabilities";
    private ObsCodecCapabilities() {}
    private static final LinkedHashMap<String, QueryResult> cache = new LinkedHashMap<>();
    private static final LinkedHashMap<String, Boolean> fullRangeCache = new LinkedHashMap<>();
    private static MediaCodecInfo[] codecs;
    private static final int COLOR_FORMAT_YUV_P010 = 54;
    private static final int PROBE_REJECTED = 0;
    private static final int PROBE_ACCEPTED = 1;
    private static final int PROBE_TRANSIENT = 2;

    static final int REJECT_HW_FOUND = 1 << 0;
    static final int REJECT_PROFILE_OR_CF = 1 << 1;
    static final int REJECT_MODE = 1 << 2;
    static final int REJECT_SIZE_RATE = 1 << 3;
    static final int REJECT_BITRATE = 1 << 4;
    static final int REJECT_FORMAT = 1 << 5;

    private static final class QueryResult {
        final String[] strictNames;
        final String[] candidateEntries;
        final int rejectFlags;
        final String diagnostics;

        QueryResult(String[] strictNames, String[] candidateEntries, int rejectFlags, String diagnostics) {
            this.strictNames = strictNames;
            this.candidateEntries = candidateEntries;
            this.rejectFlags = rejectFlags;
            this.diagnostics = diagnostics;
        }
    }

    private static String modeName(int mode) {
        switch (mode) {
        case MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CQ: return "CQ";
        case MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_VBR: return "VBR";
        case MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CBR: return "CBR";
        default: return "mode" + mode;
        }
    }

    private static String formatSupportedModes(MediaCodecInfo.EncoderCapabilities enc) {
        if (enc == null) return "none";
        ArrayList<String> modes = new ArrayList<>(3);
        if (enc.isBitrateModeSupported(MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CQ)) modes.add("CQ");
        if (enc.isBitrateModeSupported(MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_VBR)) modes.add("VBR");
        if (enc.isBitrateModeSupported(MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CBR)) modes.add("CBR");
        return modes.isEmpty() ? "none" : String.join("/", modes);
    }

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
    private static QueryResult queryEncoders(String mime, int width, int height, double fps,
            int bitrate, int mode, int requestedProfile, boolean p010, boolean allowByteBuffer) {
        int profile = requestedProfile > 0 ? requestedProfile : encoderProfile(mime);
        if (Build.VERSION.SDK_INT < 29 || width <= 0 || height <= 0 || !Double.isFinite(fps) || fps <= 0
                || bitrate < 0 || mode < 0 || mode > 2 || profile < 0 || (p010 && Build.VERSION.SDK_INT < 33))
            return new QueryResult(new String[0], new String[0], 0, "invalid-query-params");
        String key = mime + ":" + width + ":" + height + ":" + fps + ":" + bitrate + ":" + mode + ":" + profile + ":" + p010 + ":" + allowByteBuffer;
        QueryResult cached = cache.get(key);
        if (cached != null) return cached;
        if (codecs == null) codecs = new MediaCodecList(MediaCodecList.REGULAR_CODECS).getCodecInfos();
        ArrayList<String> strictNames = new ArrayList<>();
        ArrayList<String> strictEntries = new ArrayList<>();
        ArrayList<String> relaxedEntries = new ArrayList<>();
        ArrayList<String> relaxedModeEntries = new ArrayList<>();
        ArrayList<String> diagEntries = new ArrayList<>();
        int rejectFlags = 0;
        for (MediaCodecInfo info : codecs) {
            try {
                if (!info.isEncoder() || !info.isHardwareAccelerated() || info.isSoftwareOnly() || info.isAlias()) continue;
                for (String type : info.getSupportedTypes()) {
                    if (!mime.equalsIgnoreCase(type)) continue;
                    rejectFlags |= REJECT_HW_FOUND;
                    MediaCodecInfo.CodecCapabilities caps = info.getCapabilitiesForType(type);
                    MediaCodecInfo.VideoCapabilities video = caps.getVideoCapabilities();
                    MediaCodecInfo.EncoderCapabilities enc = caps.getEncoderCapabilities();
                    boolean profileOk = supportsEncoderProfile(caps, profile, p010, allowByteBuffer);
                    if (video == null || enc == null) {
                        rejectFlags |= REJECT_PROFILE_OR_CF;
                        diagEntries.add(info.getName() + "[tier=rejected missing-caps]");
                        continue;
                    }
                    android.util.Range<Integer> bitrateRange = video.getBitrateRange();
                    android.util.Range<Integer> widths = video.getSupportedWidths();
                    android.util.Range<Integer> heights = video.getSupportedHeights();
                    int wAlign = Math.max(1, video.getWidthAlignment());
                    int hAlign = Math.max(1, video.getHeightAlignment());
                    boolean bitrateOk = (mode == 0 || bitrate == 0 || bitrateRange.contains(bitrate));
                    boolean sizeOk = video.isSizeSupported(width, height);
                    boolean alignOk = (width % wAlign == 0) && (height % hAlign == 0);
                    boolean boundsOk = (widths.contains(width) && heights.contains(height))
                        || (widths.contains(height) && heights.contains(width));
                    boolean sizeRateOk = video.areSizeAndRateSupported(width, height, fps);
                    boolean modeOk = enc.isBitrateModeSupported(mode);
                    boolean formatOk = true;
                    if (profile > 0 && profileOk) {
                        MediaFormat f = MediaFormat.createVideoFormat(mime, width, height);
                        f.setInteger(MediaFormat.KEY_PROFILE, profile);
                        f.setFloat(MediaFormat.KEY_FRAME_RATE, (float)fps);
                        if (bitrate > 0) f.setInteger(MediaFormat.KEY_BIT_RATE, bitrate);
                        formatOk = caps.isFormatSupported(f);
                    }
                    String fpsRangeStr;
                    try {
                        fpsRangeStr = sizeOk
                            ? video.getSupportedFrameRatesFor(width, height).toString()
                            : video.getSupportedFrameRates().toString();
                    } catch (IllegalArgumentException e) {
                        fpsRangeStr = video.getSupportedFrameRates().toString();
                    }
                    String supportedModes = formatSupportedModes(enc);
                    ArrayList<String> mismatches = new ArrayList<>();
                    if (!profileOk) {
                        rejectFlags |= REJECT_PROFILE_OR_CF;
                        mismatches.add("profile_or_cf");
                    }
                    if (!modeOk) {
                        rejectFlags |= REJECT_MODE;
                        mismatches.add("mode(" + modeName(mode) + ";supported=" + supportedModes + ")");
                    }
                    if (!bitrateOk) {
                        rejectFlags |= REJECT_BITRATE;
                        mismatches.add("bitrate(" + bitrate + "!=" + bitrateRange + ")");
                    }
                    if (!sizeRateOk) {
                        rejectFlags |= REJECT_SIZE_RATE;
                        if (!boundsOk) mismatches.add("bounds(" + widths + "x" + heights + ")");
                        else if (!alignOk) mismatches.add("align(" + wAlign + "x" + hAlign + ")");
                        else if (!sizeOk) mismatches.add("size(" + widths + "x" + heights + ")");
                        else mismatches.add("rate(fps=" + fpsRangeStr + ")");
                    }
                    if (profile > 0 && profileOk && !formatOk) {
                        rejectFlags |= REJECT_FORMAT;
                        mismatches.add("format");
                    }

                    boolean strict = profileOk && modeOk && bitrateOk && sizeRateOk && formatOk;
                    String mismatchCsv = String.join(",", mismatches);
                    String tier;
                    if (strict) {
                        tier = "strict";
                        strictNames.add(info.getName());
                        strictEntries.add("strict|" + info.getName() + "|");
                    } else if (profileOk && boundsOk && modeOk) {
                        // Basic HW capability matches (codec, profile, color format, bounds, rate mode),
                        // only strict XML alignment/rate/bitrate/format declaration differed: allow configure() probe.
                        tier = "relaxed";
                        relaxedEntries.add("relaxed|" + info.getName() + "|" + mismatchCsv);
                    } else if (profileOk && boundsOk && mode != MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CQ) {
                        // Vendor XML omitted CBR/VBR support: allow lowest-priority configure() probe
                        // when Automatic quality is selected so real driver acceptance decides.
                        tier = "relaxed_mode";
                        relaxedModeEntries.add("relaxed_mode|" + info.getName() + "|" + mismatchCsv);
                    } else {
                        tier = "rejected";
                    }

                    String detail = info.getName() + "[tier=" + tier + " align=" + wAlign + "x" + hAlign
                        + " size=" + widths + "x" + heights + " fps=" + fpsRangeStr
                        + " br=" + bitrateRange + " modes=" + supportedModes
                        + (mismatchCsv.isEmpty() ? "" : " mismatch=" + mismatchCsv) + "]";
                    diagEntries.add(detail);
                    Log.i(TAG, info.getName() + " " + mime + " " + width + "x" + height + "@" + fps
                        + " bitrate=" + bitrate + " mode=" + modeName(mode) + " profile=" + profile
                        + " raw=" + allowByteBuffer + " tier=" + tier
                        + (mismatchCsv.isEmpty() ? "" : " mismatch=" + mismatchCsv));
                }
            } catch (IllegalArgumentException | SecurityException e) {
                Log.w(TAG, "Ignoring codec " + info.getName(), e);
                diagEntries.add(info.getName() + "[tier=error err=" + e.getClass().getSimpleName() + "]");
            }
        }
        ArrayList<String> allEntries = new ArrayList<>(strictEntries.size() + relaxedEntries.size() + relaxedModeEntries.size());
        allEntries.addAll(strictEntries);
        allEntries.addAll(relaxedEntries);
        allEntries.addAll(relaxedModeEntries);
        String diagnostics = diagEntries.isEmpty() ? "no-hardware-encoders-for-" + mime : String.join("; ", diagEntries);
        QueryResult qr = new QueryResult(
            strictNames.toArray(new String[0]),
            allEntries.toArray(new String[0]),
            rejectFlags,
            diagnostics);
        if (cache.size() >= 32) cache.remove(cache.keySet().iterator().next());
        cache.put(key, qr);
        return qr;
    }

    public static synchronized String[] findEncodersProfile(String mime, int width, int height, double fps,
            int bitrate, int mode, int requestedProfile, boolean p010, boolean allowByteBuffer) {
        return queryEncoders(mime, width, height, fps, bitrate, mode, requestedProfile, p010, allowByteBuffer)
            .strictNames.clone();
    }

    public static synchronized String[] findEncoderCandidates(String mime, int width, int height, double fps,
            int bitrate, int mode, int requestedProfile, boolean p010, boolean allowByteBuffer) {
        return queryEncoders(mime, width, height, fps, bitrate, mode, requestedProfile, p010, allowByteBuffer)
            .candidateEntries.clone();
    }

    public static synchronized int queryEncoderRejectFlags(String mime, int width, int height, double fps,
            int bitrate, int mode, int requestedProfile, boolean p010, boolean allowByteBuffer) {
        return queryEncoders(mime, width, height, fps, bitrate, mode, requestedProfile, p010, allowByteBuffer)
            .rejectFlags;
    }

    public static synchronized String describeEncoderQuery(String mime, int width, int height, double fps,
            int bitrate, int mode, int requestedProfile, boolean p010, boolean allowByteBuffer) {
        return queryEncoders(mime, width, height, fps, bitrate, mode, requestedProfile, p010, allowByteBuffer)
            .diagnostics;
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

    private static boolean colorStandardMatches(int expected, int reported) {
        return reported == expected
            || (expected == MediaFormat.COLOR_STANDARD_BT601_NTSC
                && reported == MediaFormat.COLOR_STANDARD_BT601_PAL);
    }

    private static int probeCodecFullRangeFormat(String name, String mime, MediaCodecInfo.CodecCapabilities caps,
            int profile, boolean p010, int colorFormat, int colorStandard, int colorTransfer, boolean requireCbr) {
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
        if (requireCbr) {
            if (!enc.isBitrateModeSupported(MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CBR))
                return PROBE_REJECTED;
            bitrateMode = MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CBR;
        } else if (enc.isBitrateModeSupported(MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CBR)) {
            bitrateMode = MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CBR;
        } else if (enc.isBitrateModeSupported(MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_VBR)) {
            bitrateMode = MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_VBR;
        } else if (enc.isBitrateModeSupported(MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CQ)) {
            bitrateMode = MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CQ;
        }
        if (bitrateMode >= 0) {
            f.setInteger(MediaFormat.KEY_BITRATE_MODE, bitrateMode);
            if (bitrateMode != MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CQ)
                f.setInteger(MediaFormat.KEY_BIT_RATE, video.getBitrateRange().clamp(2000000));
        }
        f.setInteger(MediaFormat.KEY_COLOR_STANDARD, colorStandard);
        f.setInteger(MediaFormat.KEY_COLOR_TRANSFER, colorTransfer);
        f.setInteger(MediaFormat.KEY_COLOR_RANGE, MediaFormat.COLOR_RANGE_FULL);
        MediaCodec codec = null;
        try {
            codec = MediaCodec.createByCodecName(name);
            codec.configure(f, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE);
            MediaFormat input = codec.getInputFormat();
            boolean accepted = input != null && input.containsKey(MediaFormat.KEY_COLOR_RANGE)
                && input.getInteger(MediaFormat.KEY_COLOR_RANGE) == MediaFormat.COLOR_RANGE_FULL;
            if (accepted && p010) {
                if ((input.containsKey(MediaFormat.KEY_COLOR_STANDARD)
                        && !colorStandardMatches(colorStandard, input.getInteger(MediaFormat.KEY_COLOR_STANDARD)))
                    || (input.containsKey(MediaFormat.KEY_COLOR_TRANSFER)
                        && input.getInteger(MediaFormat.KEY_COLOR_TRANSFER) != colorTransfer)) {
                    accepted = false;
                }
            }
            Log.i(TAG, "Full-range probe " + name + " cf=0x" + Integer.toHexString(colorFormat)
                + " profile=" + profile + " std=" + colorStandard + " tr=" + colorTransfer
                + " cbr=" + requireCbr + " accepted=" + accepted);
            return accepted ? PROBE_ACCEPTED : PROBE_REJECTED;
        } catch (MediaCodec.CodecException e) {
            boolean transientErr = e.isTransient() || e.isRecoverable()
                || e.getErrorCode() == MediaCodec.CodecException.ERROR_INSUFFICIENT_RESOURCE
                || e.getErrorCode() == MediaCodec.CodecException.ERROR_RECLAIMED;
            Log.w(TAG, "Full-range probe CodecException " + name + " cf=0x" + Integer.toHexString(colorFormat)
                + " transient=" + transientErr, e);
            return transientErr ? PROBE_TRANSIENT : PROBE_REJECTED;
        } catch (IOException | IllegalArgumentException | IllegalStateException | SecurityException e) {
            Log.w(TAG, "Full-range probe rejected " + name + " cf=0x" + Integer.toHexString(colorFormat), e);
            return PROBE_REJECTED;
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
        int transfer = (p010 && requestedProfile == MediaCodecInfo.CodecProfileLevel.HEVCProfileMain10HDR10)
            ? MediaFormat.COLOR_TRANSFER_ST2084
            : MediaFormat.COLOR_TRANSFER_SDR_VIDEO;
        int standard = (p010 && transfer != MediaFormat.COLOR_TRANSFER_SDR_VIDEO)
            ? MediaFormat.COLOR_STANDARD_BT2020
            : MediaFormat.COLOR_STANDARD_BT709;
        return hasHardwareFullRangeSupport(mime, requestedProfile, p010, standard, transfer, false);
    }

    public static synchronized boolean hasHardwareFullRangeSupport(String mime, int requestedProfile, boolean p010,
            int colorStandard, int colorTransfer, boolean requireCbr) {
        int profile = requestedProfile > 0 ? requestedProfile : encoderProfile(mime);
        if (Build.VERSION.SDK_INT < 29 || profile < 0 || (p010 && Build.VERSION.SDK_INT < 33))
            return false;
        String key = mime + ":" + profile + ":" + p010 + ":" + colorStandard + ":" + colorTransfer + ":" + requireCbr;
        Boolean cached = fullRangeCache.get(key);
        if (cached != null)
            return cached;
        if (codecs == null)
            codecs = new MediaCodecList(MediaCodecList.REGULAR_CODECS).getCodecInfos();
        boolean supported = false;
        boolean sawTransient = false;
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
                    if (requireCbr && !caps.getEncoderCapabilities().isBitrateModeSupported(
                            MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CBR))
                        continue;
                    int[] formats;
                    if (p010) {
                        formats = new int[] { COLOR_FORMAT_YUV_P010 };
                    } else {
                        int[] candidateFormats = new int[] {
                            MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface,
                            MediaCodecInfo.CodecCapabilities.COLOR_FormatYUV420SemiPlanar,
                            MediaCodecInfo.CodecCapabilities.COLOR_FormatYUV420Planar
                        };
                        ArrayList<Integer> supportedFormats = new ArrayList<>(3);
                        for (int cf : candidateFormats) {
                            if (hasColorFormat(caps, cf))
                                supportedFormats.add(cf);
                        }
                        if (supportedFormats.isEmpty())
                            continue;
                        formats = new int[supportedFormats.size()];
                        for (int i = 0; i < supportedFormats.size(); ++i)
                            formats[i] = supportedFormats.get(i);
                    }
                    for (int cf : formats) {
                        int res = probeCodecFullRangeFormat(info.getName(), mime, caps, profile, p010, cf,
                            colorStandard, colorTransfer, requireCbr);
                        if (res == PROBE_ACCEPTED) {
                            supported = true;
                            break;
                        }
                        if (res == PROBE_TRANSIENT)
                            sawTransient = true;
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
        if (supported || !sawTransient) {
            if (fullRangeCache.size() >= 32)
                fullRangeCache.remove(fullRangeCache.keySet().iterator().next());
            fullRangeCache.put(key, supported);
        }
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
