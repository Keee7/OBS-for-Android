/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * USB-audio-to-Android-input matcher used by ObsAudioHost.
 *
 * Correlates a USB video/capture device path (/dev/bus/usb/NNN/MMM) with the
 * corresponding Android AudioDeviceInfo input ID using USB bus paths, ALSA
 * /proc/asound card USB IDs, product/card names, and single-device fallback.
 */
package com.obsproject.studio;

import android.content.Context;
import android.hardware.usb.UsbConfiguration;
import android.hardware.usb.UsbConstants;
import android.hardware.usb.UsbDevice;
import android.hardware.usb.UsbManager;
import android.media.AudioDeviceInfo;
import android.os.Build;
import android.util.Log;

import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.Map;

final class ObsUsbAudioMatcher {
    private static final String TAG = "OBS-audio";

    private ObsUsbAudioMatcher() {}

    static final class UsbDevInfo {
        final String deviceName;
        final int vendorId;
        final int productId;
        final boolean hasAudio;
        final String productName;
        final String manufacturerName;

        UsbDevInfo(String deviceName, int vendorId, int productId, boolean hasAudio,
                   String productName, String manufacturerName) {
            this.deviceName = deviceName != null ? deviceName : "";
            this.vendorId = vendorId;
            this.productId = productId;
            this.hasAudio = hasAudio;
            this.productName = productName != null ? productName.trim() : "";
            this.manufacturerName = manufacturerName != null ? manufacturerName.trim() : "";
        }

        String vidPid() {
            return String.format(Locale.ROOT, "%04x:%04x", vendorId, productId);
        }
    }

    static final class AudioInInfo {
        final int id;
        final int type;
        final String rawAddress;
        final String resolvedUsbPath;
        final int cardNum;
        final String cardUsbId;
        final String cardAlsaName;
        final String productName;

        AudioInInfo(int id, int type, String rawAddress, String resolvedUsbPath,
                    int cardNum, String cardUsbId, String cardAlsaName, String productName) {
            this.id = id;
            this.type = type;
            this.rawAddress = rawAddress != null ? rawAddress.trim() : "";
            this.resolvedUsbPath = resolvedUsbPath != null ? resolvedUsbPath.trim() : "";
            this.cardNum = cardNum;
            this.cardUsbId = cardUsbId != null ? cardUsbId.trim().toLowerCase(Locale.ROOT) : "";
            this.cardAlsaName = cardAlsaName != null ? cardAlsaName.trim() : "";
            this.productName = productName != null ? productName.trim() : "";
        }

        String cardKey() {
            if (!resolvedUsbPath.isEmpty())
                return "usb:" + resolvedUsbPath;
            if (cardNum >= 0)
                return "card:" + cardNum;
            if (!rawAddress.isEmpty())
                return "addr:" + rawAddress;
            String norm = normalizeAlnum(stripAudioPrefix(productName));
            return norm.isEmpty() ? ("id:" + id) : ("name:" + norm);
        }
    }

    static int findUsbInput(Context context, AudioDeviceInfo[] inputs, String usbDeviceName) {
        if (context == null || inputs == null || usbDeviceName == null || usbDeviceName.isEmpty())
            return -1;
        UsbManager usb = (UsbManager) context.getSystemService(Context.USB_SERVICE);
        if (usb == null)
            return -1;
        Map<String, UsbDevice> devices = usb.getDeviceList();
        if (devices == null || !devices.containsKey(usbDeviceName))
            return -1;

        List<UsbDevInfo> usbDevs = new ArrayList<>();
        for (UsbDevice d : devices.values()) {
            if (d == null)
                continue;
            usbDevs.add(new UsbDevInfo(
                    d.getDeviceName(),
                    d.getVendorId(),
                    d.getProductId(),
                    hasUsbAudioInterface(d),
                    safeUsbProductName(d),
                    safeUsbManufacturerName(d)));
        }

        List<AudioInInfo> audioInputs = new ArrayList<>();
        for (AudioDeviceInfo input : inputs) {
            if (input == null || !input.isSource() || input.getId() <= 0 || !isUsbAudioType(input.getType()))
                continue;
            String rawAddr = safeAddress(input);
            int cardNum = parseAlsaCardNumber(rawAddr);
            audioInputs.add(new AudioInInfo(
                    input.getId(),
                    input.getType(),
                    rawAddr,
                    usbAddressOf(rawAddr),
                    cardNum,
                    cardNum >= 0 ? readAlsaCardUsbId(cardNum) : "",
                    cardNum >= 0 ? readAlsaCardName(cardNum) : "",
                    ObsAudioHost.safeProductName(input)));
        }

        int matched = matchUsbAudioInput(usbDeviceName, usbDevs, audioInputs, Build.MODEL);
        if (matched > 0) {
            Log.i(TAG, "USB 音频自动匹配成功：" + usbDeviceName + " -> inputId=" + matched);
        } else {
            StringBuilder detail = new StringBuilder();
            for (AudioInInfo in : audioInputs) {
                if (detail.length() > 0)
                    detail.append(", ");
                detail.append("[id=").append(in.id)
                      .append(" type=").append(in.type)
                      .append(" addr='").append(in.rawAddress)
                      .append("' usb='").append(in.resolvedUsbPath)
                      .append("' name='").append(in.productName).append("']");
            }
            Log.i(TAG, "USB 音频自动匹配未命中：" + usbDeviceName + "（USB 输入候选数=" + audioInputs.size()
                       + (detail.length() > 0 ? ": " + detail : "") + "）");
        }
        return matched;
    }

    static int matchUsbAudioInput(String usbDeviceName, List<UsbDevInfo> usbDevs,
                                    List<AudioInInfo> audioInputs, String phoneModel) {
        if (usbDeviceName == null || usbDeviceName.isEmpty() || usbDevs == null || audioInputs == null ||
            audioInputs.isEmpty())
            return -1;

        UsbDevInfo target = null;
        List<UsbDevInfo> audioUsbDevs = new ArrayList<>();
        for (UsbDevInfo dev : usbDevs) {
            if (usbDeviceName.equals(dev.deviceName))
                target = dev;
            if (dev.hasAudio)
                audioUsbDevs.add(dev);
        }
        if (target == null)
            return -1;

        // 1) Exact USB bus/device address match (/dev/bus/usb/NNN/MMM).
        String targetVidPid = target.vidPid();
        AudioInInfo exactBest = null;
        for (AudioInInfo in : audioInputs) {
            if (!in.resolvedUsbPath.isEmpty() && usbDeviceName.equals(in.resolvedUsbPath) &&
                !belongsToDifferentUsbDevice(in, usbDeviceName, targetVidPid)) {
                if (isBetterCandidate(in, exactBest))
                    exactBest = in;
            }
        }
        if (exactBest != null)
            return exactBest.id;

        if (!target.hasAudio)
            return -1;

        // Disqualify any input whose resolved USB path or VID:PID explicitly
        // identifies a different physical USB device.
        List<AudioInInfo> eligibleInputs = new ArrayList<>();
        for (AudioInInfo in : audioInputs) {
            if (!belongsToDifferentUsbDevice(in, usbDeviceName, targetVidPid))
                eligibleInputs.add(in);
        }
        if (eligibleInputs.isEmpty())
            return -1;

        // 2) ALSA usbid (vid:pid) match if sysfs/procfs exposes it cleanly.
        int sameVidPidCount = 0;
        for (UsbDevInfo dev : audioUsbDevs) {
            if (targetVidPid.equals(dev.vidPid()))
                sameVidPidCount++;
        }
        if (sameVidPidCount == 1) {
            List<AudioInInfo> vidPidMatches = new ArrayList<>();
            for (AudioInInfo in : eligibleInputs) {
                if (!in.cardUsbId.isEmpty() && targetVidPid.equals(in.cardUsbId))
                    vidPidMatches.add(in);
            }
            AudioInInfo unique = uniqueCardWinner(vidPidMatches);
            if (unique != null)
                return unique.id;
        }

        // 3) Product / ALSA card name match (tolerant to prefixes, punctuation, and
        // multiple AudioDeviceInfo entries like TYPE_USB_DEVICE + TYPE_USB_HEADSET on one card).
        if (!target.productName.isEmpty() && !isGenericAudioName(target.productName, phoneModel)) {
            int bestScore = 0;
            List<AudioInInfo> scored = new ArrayList<>();
            for (AudioInInfo in : eligibleInputs) {
                int score = Math.max(
                        scoreProductMatch(target.productName, in.productName, phoneModel),
                        scoreProductMatch(target.productName, in.cardAlsaName, phoneModel));
                if (score <= 0)
                    continue;
                if (score > bestScore) {
                    bestScore = score;
                    scored.clear();
                    scored.add(in);
                } else if (score == bestScore) {
                    scored.add(in);
                }
            }
            AudioInInfo winner = uniqueCardWinner(scored);
            if (winner != null && isUniqueAudioUsbProduct(target, winner, audioUsbDevs, phoneModel))
                return winner.id;
        }

        // 4) Unambiguous single USB-audio device fallback (e.g. SELinux hides /proc/asound
        // and HAL reports generic "USB Audio" or phone Build.MODEL instead of USB product name).
        if (audioUsbDevs.size() == 1 && hasAtMostOneUsbAudioCard(audioInputs)) {
            AudioInInfo best = null;
            for (AudioInInfo in : eligibleInputs) {
                if (isBetterCandidate(in, best))
                    best = in;
            }
            if (best != null)
                return best.id;
        }

        return -1;
    }

    private static boolean belongsToDifferentUsbDevice(AudioInInfo in, String targetUsbPath, String targetVidPid) {
        if (!in.resolvedUsbPath.isEmpty() && !targetUsbPath.equals(in.resolvedUsbPath))
            return true;
        if (!in.cardUsbId.isEmpty() && !targetVidPid.isEmpty() && !targetVidPid.equals(in.cardUsbId))
            return true;
        return false;
    }

    private static boolean isUsbAudioType(int type) {
        return type == AudioDeviceInfo.TYPE_USB_DEVICE ||
               type == AudioDeviceInfo.TYPE_USB_HEADSET ||
               type == AudioDeviceInfo.TYPE_USB_ACCESSORY;
    }

    private static int usbTypeRank(int type) {
        if (type == AudioDeviceInfo.TYPE_USB_DEVICE)
            return 0;
        if (type == AudioDeviceInfo.TYPE_USB_HEADSET)
            return 1;
        return 2;
    }

    private static boolean isBetterCandidate(AudioInInfo candidate, AudioInInfo current) {
        if (candidate == null)
            return false;
        if (current == null)
            return true;
        int candRank = usbTypeRank(candidate.type);
        int currRank = usbTypeRank(current.type);
        if (candRank != currRank)
            return candRank < currRank;
        return candidate.id < current.id;
    }

    private static AudioInInfo uniqueCardWinner(List<AudioInInfo> candidates) {
        if (candidates == null || candidates.isEmpty())
            return null;
        String firstKey = null;
        AudioInInfo best = null;
        for (AudioInInfo c : candidates) {
            String key = c.cardKey();
            if (firstKey == null)
                firstKey = key;
            else if (!firstKey.equals(key))
                return null;
            if (isBetterCandidate(c, best))
                best = c;
        }
        return best;
    }

    private static boolean hasAtMostOneUsbAudioCard(List<AudioInInfo> inputs) {
        if (inputs == null || inputs.isEmpty())
            return false;
        String confirmedUsbPath = null;
        int confirmedCardNum = -1;
        for (AudioInInfo in : inputs) {
            final boolean hasPath = !in.resolvedUsbPath.isEmpty();
            final boolean hasCard = in.cardNum >= 0;
            if (!hasPath && !hasCard)
                return false;
            if (hasPath) {
                if (confirmedUsbPath == null)
                    confirmedUsbPath = in.resolvedUsbPath;
                else if (!confirmedUsbPath.equals(in.resolvedUsbPath))
                    return false;
            }
            if (hasCard) {
                if (confirmedCardNum < 0)
                    confirmedCardNum = in.cardNum;
                else if (confirmedCardNum != in.cardNum)
                    return false;
            }
        }
        // Reject mixed path-only + card-only entries with no shared confirming field.
        if (confirmedUsbPath != null && confirmedCardNum >= 0) {
            for (AudioInInfo in : inputs) {
                if (in.resolvedUsbPath.isEmpty() || in.cardNum < 0)
                    return false;
            }
        }
        return true;
    }

    private static boolean isUniqueAudioUsbProduct(UsbDevInfo target, AudioInInfo winner,
                                                   List<UsbDevInfo> audioUsbDevs, String phoneModel) {
        String targetNorm = normalizeAlnum(target.productName);
        for (UsbDevInfo other : audioUsbDevs) {
            if (target.deviceName.equals(other.deviceName))
                continue;
            if (target.vidPid().equals(other.vidPid()))
                return false;
            if (!targetNorm.isEmpty() && targetNorm.equals(normalizeAlnum(other.productName)))
                return false;
            if (!other.productName.isEmpty() &&
                (scoreProductMatch(other.productName, winner.productName, phoneModel) > 0 ||
                 scoreProductMatch(other.productName, winner.cardAlsaName, phoneModel) > 0))
                return false;
        }
        return true;
    }

    private static int scoreProductMatch(String usbNameRaw, String audioNameRaw, String phoneModel) {
        if (usbNameRaw == null || audioNameRaw == null)
            return 0;
        String usbName = stripAudioPrefix(usbNameRaw);
        String audioName = stripAudioPrefix(audioNameRaw);
        if (usbName.isEmpty() || audioName.isEmpty() ||
            isGenericAudioName(usbName, phoneModel) || isGenericAudioName(audioName, phoneModel))
            return 0;
        if (usbName.equalsIgnoreCase(audioName) || usbNameRaw.trim().equalsIgnoreCase(audioNameRaw.trim()))
            return 3;
        String usbNorm = normalizeAlnum(usbName);
        String audioNorm = normalizeAlnum(audioName);
        if (usbNorm.length() < 3 || audioNorm.length() < 3)
            return 0;
        if (usbNorm.equals(audioNorm))
            return 2;
        if ((usbNorm.length() >= 4 && audioNorm.contains(usbNorm)) ||
            (audioNorm.length() >= 4 && usbNorm.contains(audioNorm)))
            return 1;
        return 0;
    }

    private static String stripAudioPrefix(String raw) {
        if (raw == null)
            return "";
        String s = raw.trim();
        String lower = s.toLowerCase(Locale.ROOT);
        String[] prefixes = {"usb-audio - ", "usb audio - ", "usb-audio-", "usb audio-"};
        for (String p : prefixes) {
            if (lower.startsWith(p) && s.length() > p.length() + 2)
                return s.substring(p.length()).trim();
        }
        return s;
    }

    private static boolean isGenericAudioName(String raw, String phoneModel) {
        String s = raw == null ? "" : raw.trim().toLowerCase(Locale.ROOT);
        if (s.isEmpty())
            return true;
        if (phoneModel != null && !phoneModel.trim().isEmpty() &&
            s.equals(phoneModel.trim().toLowerCase(Locale.ROOT)))
            return true;
        String norm = normalizeAlnum(s);
        return norm.equals("usbaudio") || norm.equals("usbdevice") || norm.equals("usbheadset") ||
               norm.equals("usbdigitalaudio") || norm.equals("usbmicrophone") || norm.equals("usbmic") ||
               norm.equals("usb");
    }

    private static String normalizeAlnum(String s) {
        if (s == null || s.isEmpty())
            return "";
        StringBuilder out = new StringBuilder(s.length());
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
                out.append(c);
            else if (c >= 'A' && c <= 'Z')
                out.append((char) (c + ('a' - 'A')));
        }
        return out.toString();
    }

    private static boolean hasUsbAudioInterface(UsbDevice device) {
        if (device == null)
            return false;
        if (device.getDeviceClass() == UsbConstants.USB_CLASS_AUDIO)
            return true;
        try {
            for (int i = 0; i < device.getInterfaceCount(); i++) {
                if (device.getInterface(i).getInterfaceClass() == UsbConstants.USB_CLASS_AUDIO)
                    return true;
            }
            for (int c = 0; c < device.getConfigurationCount(); c++) {
                UsbConfiguration cfg = device.getConfiguration(c);
                if (cfg == null)
                    continue;
                for (int i = 0; i < cfg.getInterfaceCount(); i++) {
                    if (cfg.getInterface(i).getInterfaceClass() == UsbConstants.USB_CLASS_AUDIO)
                        return true;
                }
            }
        } catch (RuntimeException ignored) {
        }
        return false;
    }

    private static String safeUsbProductName(UsbDevice device) {
        if (device == null)
            return "";
        try {
            String name = device.getProductName();
            return name == null ? "" : name.trim();
        } catch (SecurityException | IllegalArgumentException e) {
            return "";
        }
    }

    private static String safeUsbManufacturerName(UsbDevice device) {
        if (device == null)
            return "";
        try {
            String name = device.getManufacturerName();
            return name == null ? "" : name.trim();
        } catch (SecurityException | IllegalArgumentException e) {
            return "";
        }
    }

    private static String safeAddress(AudioDeviceInfo d) {
        try {
            String addr = d.getAddress();
            return addr == null ? "" : addr.trim();
        } catch (RuntimeException e) {
            return "";
        }
    }

    private static int parseAlsaCardNumber(String address) {
        if (address == null || address.isEmpty())
            return -1;
        for (String field : address.split(";")) {
            if (!field.startsWith("card="))
                continue;
            try {
                int card = Integer.parseInt(field.substring(5).trim());
                return card >= 0 ? card : -1;
            } catch (NumberFormatException ignored) {
                return -1;
            }
        }
        return -1;
    }

    private static String readAlsaCardUsbId(int card) {
        return readAddressFile(new File("/proc/asound/card" + card + "/usbid")).toLowerCase(Locale.ROOT);
    }

    private static String readAlsaCardName(int card) {
        String id = readAddressFile(new File("/proc/asound/card" + card + "/id"));
        if (!id.isEmpty())
            return id;
        return readAddressFile(new File("/sys/class/sound/card" + card + "/id"));
    }

    private static String formatUsbDevPath(String busStr, String devStr) {
        try {
            int bus = Integer.parseInt(busStr.trim());
            int dev = Integer.parseInt(devStr.trim());
            if (bus <= 0 || dev <= 0)
                return "";
            return String.format(Locale.ROOT, "/dev/bus/usb/%03d/%03d", bus, dev);
        } catch (NumberFormatException e) {
            return "";
        }
    }

    private static String usbAddressOf(String address) {
        final String usbPrefix = "/dev/bus/usb/";
        if (address == null || address.isEmpty())
            return "";
        if (address.startsWith(usbPrefix)) {
            String tail = address.substring(usbPrefix.length());
            String[] parts = tail.split("/");
            if (parts.length >= 2) {
                String formatted = formatUsbDevPath(parts[0], parts[1]);
                if (!formatted.isEmpty())
                    return formatted;
            }
            return address;
        }
        String busField = null;
        String devField = null;
        for (String field : address.split("[;,]")) {
            String f = field.trim();
            if (f.startsWith("bus="))
                busField = f.substring(4);
            else if (f.startsWith("busnum="))
                busField = f.substring(7);
            else if (f.startsWith("addr="))
                devField = f.substring(5);
            else if (f.startsWith("dev=") || f.startsWith("devnum="))
                devField = f.substring(f.indexOf('=') + 1);
        }
        if (busField != null && devField != null) {
            String direct = formatUsbDevPath(busField, devField);
            if (!direct.isEmpty())
                return direct;
        }
        int card = parseAlsaCardNumber(address);
        if (card < 0)
            return "";
        try {
            String bus = readAddressFile(new File("/proc/asound/card" + card + "/usbbus"));
            if (!bus.isEmpty()) {
                String[] parts = bus.split("/");
                if (parts.length == 2) {
                    String formatted = formatUsbDevPath(parts[0], parts[1]);
                    if (!formatted.isEmpty())
                        return formatted;
                }
                return usbPrefix + bus;
            }

            File usb = new File("/sys/class/sound/card" + card + "/device").getCanonicalFile();
            for (; usb != null; usb = usb.getParentFile()) {
                String busNumber = readAddressFile(new File(usb, "busnum"));
                String deviceNumber = readAddressFile(new File(usb, "devnum"));
                if (!busNumber.isEmpty() && !deviceNumber.isEmpty()) {
                    String formatted = formatUsbDevPath(busNumber, deviceNumber);
                    if (!formatted.isEmpty())
                        return formatted;
                }
            }
        } catch (IOException | NumberFormatException | SecurityException ignored) {
            // A product name or single USB-audio fallback may still identify the input.
        }
        return "";
    }

    private static String readAddressFile(File file) {
        try (FileInputStream stream = new FileInputStream(file)) {
            byte[] data = new byte[32];
            int length = stream.read(data);
            return length > 0 ? new String(data, 0, length, StandardCharsets.US_ASCII).trim() : "";
        } catch (IOException | SecurityException ignored) {
            return "";
        }
    }
}
