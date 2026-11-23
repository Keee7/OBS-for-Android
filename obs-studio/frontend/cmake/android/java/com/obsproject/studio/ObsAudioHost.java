/*
 * Android 音频输入设备宿主（plan.md 第四节 · 阶段 2 的 A4）
 *
 * 职责与 ObsUsbHost 同构：只做 Framework 那一侧必须做的事 —— 枚举输入设备、报告
 * RECORD_AUDIO 状态、监听设备增减。它不理解 OBS，也不 import Qt：Context 由 native 侧
 * （android-shell/obs_audio_host.cpp）用 QtNative.getContext() 取到后当参数传进来。
 *
 * 两处和 USB 域不一样的地方，是实测 android-35 的 android.jar 之后定的，别照记忆改回去：
 *   1) 枚举用的是 AudioManager.getDevices(GET_DEVICES_INPUTS)。plan.md 里写的
 *      listDevices() 在 SDK 35 的 AudioManager 上根本不存在（javap 全文无 listDevices），
 *      见 .qoder/a4-java-audio-api.log。
 *   2) 设备增减没有广播可收：AudioManager 里唯一的 String 常量只有 BECOMING_NOISY /
 *      HEADSET_PLUG / HDMI_AUDIO_PLUG / SCO_* / MICROPHONE_MUTE_CHANGED 这些，没有
 *      ACTION_AUDIO_DEVICE_ADDED 之类。真正的通道是
 *      registerAudioDeviceCallback(AudioDeviceCallback, Handler)，所以这里同时用两条腿：
 *      回调管"增减"，广播管"静音/拔出耳机"，两条都往同一段刷新逻辑上汇。
 */
package com.obsproject.studio;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.PackageManager;
import android.hardware.usb.UsbConfiguration;
import android.hardware.usb.UsbConstants;
import android.hardware.usb.UsbDevice;
import android.hardware.usb.UsbManager;
import android.media.AudioDeviceCallback;
import android.media.AudioDeviceInfo;
import android.media.AudioManager;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;

import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.Map;

public final class ObsAudioHost {
    private static final String TAG = "OBS-audio";

    /** 冒烟用的自注入广播：模拟器上造不出真的设备插拔，至少能证明 receiver 链路是通的 */
    public static final String TEST_UPDATE_ACTION = "com.obsproject.studio.AUDIO_TEST_UPDATE";

    /** 快照一次最多带多少条（与 native 侧 AAUDIO_MON_DEV_MAX 一致，不足由调用方截断）。 */
    private static final int MAX_ENTRIES = 32;

    private static Context context;
    private static AudioManager manager;
    private static boolean callbackRegistered = false;
    private static boolean receiverRegistered = false;
    private static DeviceCallback callback;
    private static AudioReceiver receiver;

    private static int deviceCallbacks = 0;
    private static int hotplugEvents = 0;
    private static String lastAction = "<none>";
    private static int lastCallbackOnMain = -1; // -1 = 回调一次都没跑过

    private ObsAudioHost() {}

    /** 幂等：native 在 obs_startup 前后调都行，重复调用不会重复注册。 */
    public static synchronized boolean install(Context ctx) {
        if (ctx == null) {
            Log.e(TAG, "install(null) —— 拿不到 Context，音频总线不会接通");
            return false;
        }
        context = ctx.getApplicationContext();
        manager = (AudioManager) context.getSystemService(Context.AUDIO_SERVICE);
        if (manager == null) {
            Log.e(TAG, "getSystemService(AUDIO_SERVICE) 返回 null —— 这台设备没有音频服务？");
            return false;
        }

        if (!callbackRegistered) {
            callback = new DeviceCallback();
            /* 显式挂主线程 Handler：不传的话回调排在调用者所在线程的 looper 上，
             * 而 native 冒烟恰好不在主线程（A3 实测过），会多一个变量。 */
            manager.registerAudioDeviceCallback(callback, new Handler(Looper.getMainLooper()));
            callbackRegistered = true;
        }

        if (!receiverRegistered) {
            receiver = new AudioReceiver();
            IntentFilter f = new IntentFilter();
            f.addAction(AudioManager.ACTION_HEADSET_PLUG);
            f.addAction(AudioManager.ACTION_MICROPHONE_MUTE_CHANGED);
            f.addAction(AudioManager.ACTION_AUDIO_BECOMING_NOISY);
            f.addAction(TEST_UPDATE_ACTION);
            /* Android 14 起注册非系统广播必须声明导出性；这里全是系统广播或自己发的，
               NOT_EXPORTED 够用，也不需要新权限（与 ObsUsbHost 同一处理）。 */
            int flags = 0;
            if (Build.VERSION.SDK_INT >= 33)
                flags = Context.RECEIVER_NOT_EXPORTED;
            context.registerReceiver(receiver, f, flags);
            receiverRegistered = true;
        }

        /* Log every input port once: a capture card's UAC input has to appear
         * here (type 11/22, isSource=true) before AAudio can even name it. */
        Log.i(TAG, "音频宿主已安装：callback=" + callbackRegistered + " receiver=" + receiverRegistered
                       + " 输入设备=" + inputs().length + " 输出设备=" + outputs().length
                       + " RECORD_AUDIO=" + hasRecordPermission() + describeInputs());
        return true;
    }

    public static synchronized boolean isInstalled() {
        return manager != null && callbackRegistered && receiverRegistered;
    }

    public static synchronized int deviceCallbacks() {
        return deviceCallbacks;
    }

    public static synchronized int hotplugEvents() {
        return hotplugEvents;
    }

    public static synchronized String lastAction() {
        return lastAction;
    }

    /** 上一次 AudioDeviceCallback 是不是跑在主线程：-1 从没跑过 / 0 不是 / 1 是 */
    public static synchronized int lastCallbackOnMain() {
        return lastCallbackOnMain;
    }

    /** 冒烟专用：走与真广播同一段 onReceive，证明"注册上了 + 过滤器命中 + 刷新逻辑执行" */
    public static synchronized boolean sendTestUpdate() {
        if (context == null) {
            Log.e(TAG, "sendTestUpdate()：还没 install()");
            return false;
        }
        context.sendBroadcast(new Intent(TEST_UPDATE_ACTION).setPackage(context.getPackageName()));
        return true;
    }

    /** RECORD_AUDIO 有没有授予。没它 Android 10+ 读不到输入设备的属性。 */
    public static synchronized boolean hasRecordPermission() {
        if (context == null)
            return false;
        return context.checkSelfPermission("android.permission.RECORD_AUDIO") == PackageManager.PERMISSION_GRANTED;
    }

    public static synchronized int deviceCount() {
        return manager == null ? -1 : inputs().length;
    }

    /** 只取采集侧（GET_DEVICES_INPUTS=1，值取自 javap）。调用方须保证 manager 已就位。 */
    private static AudioDeviceInfo[] inputs() {
        return manager.getDevices(AudioManager.GET_DEVICES_INPUTS);
    }

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

    /** Returns the Android input ID when it can be associated with this USB device. */
    public static synchronized int findUsbInput(String usbDeviceName) {
        if (manager == null || context == null || usbDeviceName == null || usbDeviceName.isEmpty())
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
        for (AudioDeviceInfo input : inputs()) {
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
                    safeProductName(input)));
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

    /** 只取播放侧（用于音频监听设备枚举）。调用方须保证 manager 已就位。 */
    private static AudioDeviceInfo[] outputs() {
        return manager.getDevices(AudioManager.GET_DEVICES_OUTPUTS);
    }

    /** "id|type|isAvailable|product|label" 用 ';' 拼成的单串，native 侧拆开填结构体。 */
    private static String buildSnapshot(AudioDeviceInfo[] all, boolean outputMode) {
        StringBuilder sb = new StringBuilder();
        int included = 0;
        int eligible = 0;
        for (int i = 0; i < all.length; i++) {
            AudioDeviceInfo d = all[i];
            if (outputMode) {
                final int t = d.getType();
                if (!d.isSink() || d.getId() <= 0 ||
                    t == AudioDeviceInfo.TYPE_TELEPHONY || t == AudioDeviceInfo.TYPE_REMOTE_SUBMIX) {
                    continue;
                }
            }
            eligible++;
            if (included >= MAX_ENTRIES)
                continue;
            if (sb.length() > 0)
                sb.append(ENTRY_SEP);
            String product = scrub(safeProductName(d));
            boolean available = outputMode ? d.isSink() : d.isSource();
            sb.append(d.getId()).append(FIELD_SEP).append(d.getType()).append(FIELD_SEP)
              .append(available ? "1" : "0").append(FIELD_SEP).append(product).append(FIELD_SEP)
              .append(scrub(labelOf(d, product, outputMode)));
            included++;
        }
        if (eligible > MAX_ENTRIES) {
            Log.w(TAG, (outputMode ? "输出设备共 " : "输入设备共 ") + eligible +
                       " 台，快照只带前 " + MAX_ENTRIES + " 条进 native");
        }
        return sb.toString();
    }

    /** "id|type|isSource|product|label" 用 ';' 拼成的单串，native 侧拆开填结构体。 */
    public static synchronized String snapshot() {
        if (manager == null)
            return "";
        return buildSnapshot(inputs(), false);
    }

    /** "id|type|isSink|product|label" 用 ';' 拼成的单串，供音频监听枚举播放设备。 */
    public static synchronized String snapshotOutputs() {
        if (manager == null)
            return "";
        return buildSnapshot(outputs(), true);
    }

    /* --- 快照协议 ---------------------------------------------------------- */

    private static final char ENTRY_SEP = ';';
    private static final char FIELD_SEP = '|';

    /** 产品名是设备/HAL 报上来的字符串，属于不可信输入：分隔符与控制字符全换成空格，
     *  否则一台设备就能自己改写快照串的字段结构（ObsUsbHost 的 scrub 同一处理）。 */
    private static String scrub(String s) {
        if (s == null)
            return "";
        StringBuilder out = new StringBuilder(s.length());
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            out.append(c == ENTRY_SEP || c == FIELD_SEP || Character.isISOControl(c) ? ' ' : c);
        }
        return out.toString();
    }

    /** getProductName() 在没授予 RECORD_AUDIO 时可能返回 null 或抛 SecurityException。 */
    private static String safeProductName(AudioDeviceInfo d) {
        try {
            CharSequence cs = d.getProductName();
            return cs == null ? "" : cs.toString();
        } catch (SecurityException e) {
            return "";
        }
    }

    private static String labelOf(AudioDeviceInfo d, String product, boolean outputMode) {
        StringBuilder sb = new StringBuilder();
        sb.append(typeName(d.getType()));
        if (!product.isEmpty())
            sb.append(' ').append(product);
        sb.append("（id=").append(d.getId()).append(", type=").append(d.getType());
        if (outputMode ? !d.isSink() : !d.isSource())
            sb.append(outputMode ? ", 不可输出" : ", 不可采集");
        sb.append('）');
        return sb.toString();
    }

    /** 数值取自 javap android.media.AudioDeviceInfo（.qoder/a4-java-audio-api.log），
     *  AAudio.h 的 AAudio_DeviceType 注释自述与这套值同源。 */
    private static String typeName(int t) {
        switch (t) {
            case 0: return "未知设备";
            case 1: return "内置听筒";
            case 2: return "内置扬声器";
            case 3: return "有线耳机（带麦）";
            case 4: return "有线耳机";
            case 5: return "模拟线路输入";
            case 6: return "数字线路输入";
            case 7: return "蓝牙 SCO 耳机";
            case 8: return "蓝牙 A2DP";
            case 9: return "HDMI";
            case 10: return "HDMI ARC";
            case 11: return "USB 设备";
            case 12: return "USB 配件";
            case 13: return "底座";
            case 15: return "内置麦克风";
            case 16: return "FM 调谐器";
            case 17: return "TV 调谐器";
            case 18: return "电话线路";
            case 19: return "辅助线路";
            case 20: return "IP 音频";
            case 21: return "BUS";
            case 22: return "USB 耳机";
            case 23: return "助听器";
            case 24: return "内置扬声器（安全输出）";
            case 25: return "远程混音（虚拟设备）";
            case 26: return "蓝牙 LE 耳机";
            case 27: return "蓝牙 LE 扬声器";
            case 29: return "HDMI eARC";
            default: return "type=" + t;
        }
    }

    /** One line per input port, prefixed with a newline so logcat stays readable. */
    private static String describeInputs() {
        if (manager == null)
            return "";
        StringBuilder sb = new StringBuilder();
        for (AudioDeviceInfo d : inputs())
            sb.append("\n  id=").append(d.getId()).append(" type=").append(d.getType())
              .append(" isSource=").append(d.isSource()).append(" name=").append(safeProductName(d));
        return sb.toString();
    }

    private static void refresh(String why) {
        lastAction = why;
        hotplugEvents++;
        Log.i(TAG, "刷新音频输入清单（" + why + "）：共 " + (manager == null ? -1 : inputs().length)
                       + " 台，RECORD_AUDIO=" + hasRecordPermission() + describeInputs());
    }

    private static final class DeviceCallback extends AudioDeviceCallback {
        @Override
        public void onAudioDevicesAdded(AudioDeviceInfo[] added) {
            record("added");
            refresh("AudioDeviceCallback.added/" + (added == null ? 0 : added.length));
        }

        @Override
        public void onAudioDevicesRemoved(AudioDeviceInfo[] removed) {
            record("removed");
            refresh("AudioDeviceCallback.removed/" + (removed == null ? 0 : removed.length));
        }

        /** 回调跑在哪个线程要留证据：native 侧靠这个判断"没收到回调"是链路坏了还是压根没插拔 */
        private void record(String what) {
            synchronized (ObsAudioHost.class) {
                deviceCallbacks++;
                lastCallbackOnMain =
                    (Looper.myLooper() == Looper.getMainLooper()) ? 1 : 0;
            }
        }
    }

    private static final class AudioReceiver extends BroadcastReceiver {
        @Override
        public void onReceive(Context ctx, Intent intent) {
            String action = intent == null ? "<null>" : intent.getAction();
            if (action == null)
                action = "<null>";
            refresh("broadcast/" + action);
        }
    }
}
