/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Android NDI® mDNS / NsdManager + MulticastLock host bridge.
 *
 * Keeps Android Framework interactions isolated on the Java side (same boundary
 * as ObsUsbHost / ObsAudioHost):
 *   1. Holds a reference-counted WifiManager.MulticastLock while any NDI finder
 *      is active so Android Wi-Fi drivers deliver mDNS packets (224.0.0.251:5353).
 *   2. Runs NsdManager discovery for "_ndi._tcp." and resolves peer IP:port
 *      entries via a serialized resolve queue (avoiding FAILURE_ALREADY_ACTIVE).
 *   3. Exposes ApplicationInfo.nativeLibraryDir so native dynamic loading can
 *      probe bundled libndi.so cleanly.
 */
package com.obsproject.studio;

import android.content.Context;
import android.net.Uri;
import android.net.nsd.NsdManager;
import android.net.nsd.NsdServiceInfo;
import android.net.wifi.WifiManager;
import android.util.Log;

import java.io.BufferedInputStream;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.RandomAccessFile;
import java.net.Inet4Address;
import java.net.InetAddress;
import java.nio.charset.StandardCharsets;
import java.util.ArrayDeque;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.zip.GZIPInputStream;

public final class ObsNdiHost {
    private static final String TAG = "OBS-ndi";
    private static final String SERVICE_TYPE = "_ndi._tcp.";
    private static final char ENTRY_SEP = ';';
    private static final char FIELD_SEP = '|';

    private static Context appContext;
    private static WifiManager wifiManager;
    private static WifiManager.MulticastLock multicastLock;
    private static NsdManager nsdManager;
    private static NsdManager.DiscoveryListener discoveryListener;
    private static boolean discovering = false;
    private static int refCount = 0;

    private static final LinkedHashMap<String, String> peers = new LinkedHashMap<>();
    private static final ArrayDeque<NsdServiceInfo> resolveQueue = new ArrayDeque<>();
    private static boolean resolving = false;

    private ObsNdiHost() {}

    public static synchronized boolean install(Context ctx) {
        if (ctx == null) {
            Log.e(TAG, "install(null) failed: missing Context");
            return false;
        }
        appContext = ctx.getApplicationContext();
        wifiManager = (WifiManager) appContext.getSystemService(Context.WIFI_SERVICE);
        nsdManager = (NsdManager) appContext.getSystemService(Context.NSD_SERVICE);
        Log.i(TAG, "ObsNdiHost installed (wifi=" + (wifiManager != null)
                + " nsd=" + (nsdManager != null) + ")");
        return true;
    }

    public static synchronized String nativeLibDir() {
        if (appContext == null || appContext.getApplicationInfo() == null)
            return "";
        String dir = appContext.getApplicationInfo().nativeLibraryDir;
        return dir != null ? dir : "";
    }

    public static synchronized boolean acquireDiscovery() {
        refCount++;
        if (refCount > 1)
            return true;

        try {
            if (wifiManager != null) {
                if (multicastLock == null) {
                    multicastLock = wifiManager.createMulticastLock("obs-ndi-mdns");
                    multicastLock.setReferenceCounted(false);
                }
                if (!multicastLock.isHeld()) {
                    multicastLock.acquire();
                    Log.i(TAG, "WifiManager.MulticastLock acquired for NDI mDNS");
                }
            }
        } catch (Throwable t) {
            Log.w(TAG, "Failed to acquire MulticastLock: " + t.getMessage());
        }

        startNsdLocked();
        return true;
    }

    public static synchronized void releaseDiscovery() {
        if (refCount <= 0)
            return;
        refCount--;
        if (refCount > 0)
            return;

        stopNsdLocked();
        resolveQueue.clear();
        resolving = false;
        peers.clear();

        try {
            if (multicastLock != null && multicastLock.isHeld()) {
                multicastLock.release();
                Log.i(TAG, "WifiManager.MulticastLock released");
            }
        } catch (Throwable t) {
            Log.w(TAG, "Failed to release MulticastLock: " + t.getMessage());
        }
    }

    public static synchronized String snapshotPeers() {
        if (peers.isEmpty())
            return "";
        StringBuilder sb = new StringBuilder();
        for (Map.Entry<String, String> e : peers.entrySet()) {
            if (sb.length() > 0)
                sb.append(ENTRY_SEP);
            sb.append(scrub(e.getKey())).append(FIELD_SEP).append(scrub(e.getValue()));
        }
        return sb.toString();
    }

    private static String scrub(String s) {
        if (s == null)
            return "";
        return s.replace(ENTRY_SEP, ' ').replace(FIELD_SEP, ' ')
                .replace('\r', ' ').replace('\n', ' ').trim();
    }

    private static void startNsdLocked() {
        if (nsdManager == null || discovering)
            return;
        discoveryListener = new NsdManager.DiscoveryListener() {
            @Override
            public void onStartDiscoveryFailed(String serviceType, int errorCode) {
                Log.w(TAG, "NSD start discovery failed: err=" + errorCode);
                synchronized (ObsNdiHost.class) {
                    discovering = false;
                }
            }

            @Override
            public void onStopDiscoveryFailed(String serviceType, int errorCode) {
                Log.w(TAG, "NSD stop discovery failed: err=" + errorCode);
                synchronized (ObsNdiHost.class) {
                    discovering = false;
                }
            }

            @Override
            public void onDiscoveryStarted(String serviceType) {
                Log.i(TAG, "NSD discovery started for " + serviceType);
            }

            @Override
            public void onDiscoveryStopped(String serviceType) {
                Log.i(TAG, "NSD discovery stopped for " + serviceType);
                synchronized (ObsNdiHost.class) {
                    discovering = false;
                }
            }

            @Override
            public void onServiceFound(NsdServiceInfo serviceInfo) {
                if (serviceInfo == null)
                    return;
                synchronized (ObsNdiHost.class) {
                    if (refCount <= 0)
                        return;
                    if (resolveQueue.size() < 64) {
                        resolveQueue.addLast(serviceInfo);
                        pumpResolveQueueLocked();
                    }
                }
            }

            @Override
            public void onServiceLost(NsdServiceInfo serviceInfo) {
                if (serviceInfo == null || serviceInfo.getServiceName() == null)
                    return;
                synchronized (ObsNdiHost.class) {
                    peers.remove(serviceInfo.getServiceName());
                }
            }
        };

        try {
            nsdManager.discoverServices(SERVICE_TYPE, NsdManager.PROTOCOL_DNS_SD, discoveryListener);
            discovering = true;
        } catch (Throwable t) {
            Log.w(TAG, "discoverServices threw: " + t.getMessage());
            discovering = false;
            discoveryListener = null;
        }
    }

    private static void stopNsdLocked() {
        if (nsdManager == null || !discovering || discoveryListener == null)
            return;
        try {
            nsdManager.stopServiceDiscovery(discoveryListener);
        } catch (Throwable t) {
            Log.w(TAG, "stopServiceDiscovery threw: " + t.getMessage());
        }
        discovering = false;
        discoveryListener = null;
    }

    @SuppressWarnings("deprecation")
    private static void pumpResolveQueueLocked() {
        if (resolving || nsdManager == null || refCount <= 0)
            return;
        NsdServiceInfo next = resolveQueue.pollFirst();
        if (next == null)
            return;
        resolving = true;
        try {
            nsdManager.resolveService(next, new NsdManager.ResolveListener() {
                @Override
                public void onResolveFailed(NsdServiceInfo serviceInfo, int errorCode) {
                    Log.w(TAG, "NSD resolve failed for "
                            + (serviceInfo != null ? serviceInfo.getServiceName() : "?")
                            + ": err=" + errorCode);
                    synchronized (ObsNdiHost.class) {
                        resolving = false;
                        pumpResolveQueueLocked();
                    }
                }

                @Override
                public void onServiceResolved(NsdServiceInfo serviceInfo) {
                    synchronized (ObsNdiHost.class) {
                        if (refCount > 0 && serviceInfo != null && serviceInfo.getServiceName() != null) {
                            InetAddress host = serviceInfo.getHost();
                            int port = serviceInfo.getPort();
                            if (host != null && host.getHostAddress() != null) {
                                String ip = host.getHostAddress();
                                int pct = ip.indexOf('%');
                                if (pct > 0)
                                    ip = ip.substring(0, pct);
                                String addr = (host instanceof Inet4Address)
                                        ? (port > 0 ? ip + ":" + port : ip)
                                        : (port > 0 ? "[" + ip + "]:" + port : ip);
                                peers.put(serviceInfo.getServiceName(), addr);
                            }
                        }
                        resolving = false;
                        pumpResolveQueueLocked();
                    }
                }
            });
        } catch (Throwable t) {
            Log.w(TAG, "resolveService threw: " + t.getMessage());
            resolving = false;
            pumpResolveQueueLocked();
        }
    }

    /**
     * Extract and validate a candidate arm64-v8a 16 KB-aligned libndi.so into a
     * staging file inside destDirPath WITHOUT overwriting any existing libndi.so.
     * Caller must verify NDI symbols via dlopen/dlsym on the staging file, then
     * call commitRuntimeStaging() or discardRuntimeStaging().
     * Returns "OK:<stagingSoPath>|<bytes>|<minAlign>" or "ERR:<reason>".
     */
    public static String importRuntimeToStaging(String sourceUriOrPath, String destDirPath) {
        if (sourceUriOrPath == null || sourceUriOrPath.isEmpty() || destDirPath == null || destDirPath.isEmpty())
            return "ERR:Invalid source or destination path";

        File destDir = new File(destDirPath);
        if (!destDir.exists() && !destDir.mkdirs())
            return "ERR:Failed to create directory " + destDirPath;

        File tmpSo = new File(destDir, "libndi.so.tmp");
        File tmpLic = new File(destDir, "libndi_licenses.txt.tmp");
        File tmpShell = new File(destDir, "ndi_installer.sh.tmp");
        File stageSo = new File(destDir, "libndi.so.staging");
        File stageLic = new File(destDir, "libndi_licenses.txt.staging");
        if (stageSo.exists())
            stageSo.delete();
        if (stageLic.exists())
            stageLic.delete();

        try {
            byte[] magic = new byte[4];
            try (InputStream raw = openSourceStream(sourceUriOrPath)) {
                if (raw == null)
                    return "ERR:Cannot open selected file";
                BufferedInputStream bis = new BufferedInputStream(raw, 65536);
                bis.mark(8);
                int n = readFully(bis, magic, 4);
                if (n < 4)
                    return "ERR:Selected file is too small";
                bis.reset();

                if (magic[0] == 0x7f && magic[1] == 'E' && magic[2] == 'L' && magic[3] == 'F') {
                    copyStreamToFile(bis, tmpSo, 64L * 1024 * 1024);
                } else if ((magic[0] & 0xff) == 0x1f && (magic[1] & 0xff) == 0x8b) {
                    boolean extractedSo = extractFromTarGz(bis, tmpSo, tmpLic, tmpShell);
                    if (!extractedSo && tmpShell.isFile() && tmpShell.length() > 0) {
                        extractedSo = extractFromSelfExtractingSh(tmpShell, tmpSo, tmpLic);
                    }
                    if (!extractedSo)
                        return "ERR:Archive does not contain arm64-v8a/libndi.so";
                } else if (magic[0] == '#' && magic[1] == '!') {
                    copyStreamToFile(bis, tmpShell, 128L * 1024 * 1024);
                    if (!extractFromSelfExtractingSh(tmpShell, tmpSo, tmpLic))
                        return "ERR:Installer script does not contain arm64-v8a/libndi.so";
                } else {
                    return "ERR:Unsupported file format (select libndi.so, Install_NDI_SDK_v6_Android.tar.gz, or .sh)";
                }
            }

            long minAlign = validateArm64Elf16k(tmpSo);
            if (!tmpSo.renameTo(stageSo))
                return "ERR:Failed to stage validated libndi.so";
            stageSo.setReadable(true, false);
            stageSo.setExecutable(true, false);

            if (tmpLic.isFile() && tmpLic.length() > 0) {
                tmpLic.renameTo(stageLic);
            }
            Log.i(TAG, "Staged candidate NDI runtime: " + stageSo.getAbsolutePath()
                    + " (" + stageSo.length() + " B, minAlign=" + minAlign + ")");
            return "OK:" + stageSo.getAbsolutePath() + "|" + stageSo.length() + "|" + minAlign;
        } catch (Throwable t) {
            Log.w(TAG, "importRuntimeToStaging failed: " + t.getMessage());
            if (stageSo.exists())
                stageSo.delete();
            if (stageLic.exists())
                stageLic.delete();
            return "ERR:" + (t.getMessage() != null ? t.getMessage() : t.getClass().getSimpleName());
        } finally {
            if (tmpSo.exists())
                tmpSo.delete();
            if (tmpLic.exists())
                tmpLic.delete();
            if (tmpShell.exists())
                tmpShell.delete();
        }
    }

    public static boolean commitRuntimeStaging(String destDirPath) {
        if (destDirPath == null || destDirPath.isEmpty())
            return false;
        File destDir = new File(destDirPath);
        File stageSo = new File(destDir, "libndi.so.staging");
        File stageLic = new File(destDir, "libndi_licenses.txt.staging");
        File finalSo = new File(destDir, "libndi.so");
        File finalLic = new File(destDir, "libndi_licenses.txt");
        if (!stageSo.isFile())
            return false;
        if (!stageSo.renameTo(finalSo)) {
            if (finalSo.exists())
                finalSo.delete();
            if (!stageSo.renameTo(finalSo))
                return false;
        }
        finalSo.setReadable(true, false);
        finalSo.setExecutable(true, false);
        if (stageLic.isFile()) {
            if (!stageLic.renameTo(finalLic)) {
                if (finalLic.exists())
                    finalLic.delete();
                stageLic.renameTo(finalLic);
            }
        }
        Log.i(TAG, "Committed verified user NDI runtime: " + finalSo.getAbsolutePath()
                + " (" + finalSo.length() + " B)");
        return true;
    }

    public static void discardRuntimeStaging(String destDirPath) {
        if (destDirPath == null || destDirPath.isEmpty())
            return;
        File destDir = new File(destDirPath);
        File stageSo = new File(destDir, "libndi.so.staging");
        File stageLic = new File(destDir, "libndi_licenses.txt.staging");
        if (stageSo.exists())
            stageSo.delete();
        if (stageLic.exists())
            stageLic.delete();
    }

    public static boolean removeRuntime(String destDirPath) {
        if (destDirPath == null || destDirPath.isEmpty())
            return false;
        File dir = new File(destDirPath);
        File so = new File(dir, "libndi.so");
        File lic = new File(dir, "libndi_licenses.txt");
        if (lic.exists())
            lic.delete();
        boolean ok = !so.exists() || so.delete();
        Log.i(TAG, "Removed user NDI runtime from " + destDirPath + ": ok=" + ok);
        return ok;
    }

    private static InputStream openSourceStream(String sourceUriOrPath) throws IOException {
        if (sourceUriOrPath.startsWith("content://") || sourceUriOrPath.startsWith("file://")) {
            if (appContext == null)
                throw new IOException("Android Context not initialized");
            return appContext.getContentResolver().openInputStream(Uri.parse(sourceUriOrPath));
        }
        return new FileInputStream(new File(sourceUriOrPath));
    }

    private static int readFully(InputStream in, byte[] buf, int len) throws IOException {
        int off = 0;
        while (off < len) {
            int r = in.read(buf, off, len - off);
            if (r < 0)
                break;
            off += r;
        }
        return off;
    }

    private static void copyStreamToFile(InputStream in, File out, long maxBytes) throws IOException {
        long total = 0;
        byte[] buf = new byte[32768];
        try (FileOutputStream fos = new FileOutputStream(out)) {
            int r;
            while ((r = in.read(buf)) >= 0) {
                total += r;
                if (total > maxBytes)
                    throw new IOException("File exceeds maximum allowed size");
                fos.write(buf, 0, r);
            }
        }
    }

    private static void copyExactToFile(InputStream in, File out, long size) throws IOException {
        long rem = size;
        byte[] buf = new byte[32768];
        try (FileOutputStream fos = new FileOutputStream(out)) {
            while (rem > 0) {
                int want = (int) Math.min(buf.length, rem);
                int r = in.read(buf, 0, want);
                if (r < 0)
                    throw new IOException("Unexpected EOF in tar entry");
                fos.write(buf, 0, r);
                rem -= r;
            }
        }
    }

    private static void skipExact(InputStream in, long bytes) throws IOException {
        long rem = bytes;
        byte[] buf = new byte[16384];
        while (rem > 0) {
            int r = in.read(buf, 0, (int) Math.min(buf.length, rem));
            if (r < 0)
                break;
            rem -= r;
        }
    }

    private static long parseOctal(byte[] hdr, int off, int len) {
        long val = 0;
        int end = off + len;
        int i = off;
        while (i < end && (hdr[i] == ' ' || hdr[i] == 0))
            i++;
        while (i < end && hdr[i] >= '0' && hdr[i] <= '7') {
            val = (val << 3) | (hdr[i] - '0');
            i++;
        }
        return val;
    }

    private static String parseTarString(byte[] hdr, int off, int len) {
        int end = off;
        while (end < off + len && hdr[end] != 0)
            end++;
        return new String(hdr, off, end - off, StandardCharsets.UTF_8);
    }

    private static boolean extractFromTarGz(InputStream rawGz, File outSo, File outLic, File outShell)
            throws IOException {
        boolean foundSo = false;
        try (GZIPInputStream gis = new GZIPInputStream(rawGz, 65536)) {
            byte[] hdr = new byte[512];
            String pendingLongName = null;
            while (true) {
                int r = readFully(gis, hdr, 512);
                if (r < 512)
                    break;
                boolean allZero = true;
                for (int i = 0; i < 512; i++) {
                    if (hdr[i] != 0) {
                        allZero = false;
                        break;
                    }
                }
                if (allZero)
                    break;

                byte typeFlag = hdr[156];
                long size = parseOctal(hdr, 124, 12);
                String name = parseTarString(hdr, 0, 100);
                String prefix = parseTarString(hdr, 345, 155);
                if (!prefix.isEmpty())
                    name = prefix + "/" + name;
                if (pendingLongName != null) {
                    name = pendingLongName;
                    pendingLongName = null;
                }

                long pad = (512 - (size % 512)) % 512;
                if (typeFlag == 'L' && size > 0 && size < 4096) {
                    ByteArrayOutputStream baos = new ByteArrayOutputStream((int) size);
                    byte[] tmp = new byte[(int) size];
                    readFully(gis, tmp, (int) size);
                    int nLen = tmp.length;
                    while (nLen > 0 && tmp[nLen - 1] == 0)
                        nLen--;
                    pendingLongName = new String(tmp, 0, nLen, StandardCharsets.UTF_8);
                    skipExact(gis, pad);
                    continue;
                }

                String norm = name.replace('\\', '/');
                if ((typeFlag == 0 || typeFlag == '0') && size > 0) {
                    if (norm.endsWith("/arm64-v8a/libndi.so") || norm.equals("libndi.so")) {
                        copyExactToFile(gis, outSo, size);
                        foundSo = true;
                    } else if (norm.endsWith("/libndi_licenses.txt") || norm.equals("libndi_licenses.txt")) {
                        copyExactToFile(gis, outLic, size);
                    } else if (outShell != null && norm.endsWith(".sh") && norm.contains("NDI_SDK") && norm.contains("Android")) {
                        copyExactToFile(gis, outShell, size);
                    } else {
                        skipExact(gis, size);
                    }
                } else if (size > 0) {
                    skipExact(gis, size);
                }
                if (pad > 0)
                    skipExact(gis, pad);
            }
        }
        return foundSo;
    }

    private static boolean extractFromSelfExtractingSh(File shFile, File outSo, File outLic) throws IOException {
        byte[] head = new byte[(int) Math.min(shFile.length(), 131072)];
        try (FileInputStream fis = new FileInputStream(shFile)) {
            int n = readFully(fis, head, head.length);
            String text = new String(head, 0, n, StandardCharsets.ISO_8859_1);
            int marker = text.indexOf("__NDI_ARCHIVE_BEGIN__");
            int start = marker >= 0 ? marker : 0;
            int gzOff = -1;
            for (int i = start; i + 2 < n; i++) {
                if ((head[i] & 0xff) == 0x1f && (head[i + 1] & 0xff) == 0x8b && (head[i + 2] & 0xff) == 0x08) {
                    gzOff = i;
                    break;
                }
            }
            if (gzOff < 0)
                return false;
            try (FileInputStream fis2 = new FileInputStream(shFile)) {
                skipExact(fis2, gzOff);
                return extractFromTarGz(new BufferedInputStream(fis2, 65536), outSo, outLic, null);
            }
        }
    }

    private static long readU16LE(byte[] b, int o) {
        return (b[o] & 0xffL) | ((b[o + 1] & 0xffL) << 8);
    }

    private static long readU32LE(byte[] b, int o) {
        return (b[o] & 0xffL) | ((b[o + 1] & 0xffL) << 8) | ((b[o + 2] & 0xffL) << 16) | ((b[o + 3] & 0xffL) << 24);
    }

    private static long readU64LE(byte[] b, int o) {
        return readU32LE(b, o) | (readU32LE(b, o + 4) << 32);
    }

    private static long validateArm64Elf16k(File elf) throws IOException {
        if (!elf.isFile() || elf.length() < 64)
            throw new IOException("Invalid libndi.so (file too small)");
        try (RandomAccessFile raf = new RandomAccessFile(elf, "r")) {
            byte[] ehdr = new byte[64];
            raf.readFully(ehdr);
            if (ehdr[0] != 0x7f || ehdr[1] != 'E' || ehdr[2] != 'L' || ehdr[3] != 'F')
                throw new IOException("Not an ELF binary");
            if (ehdr[4] != 2 || ehdr[5] != 1)
                throw new IOException("Expected 64-bit little-endian ELF");
            long machine = readU16LE(ehdr, 18);
            if (machine != 183)
                throw new IOException("Wrong CPU architecture (expected arm64-v8a / EM_AARCH64, got " + machine + ")");

            long phoff = readU64LE(ehdr, 32);
            int phentsize = (int) readU16LE(ehdr, 54);
            int phnum = (int) readU16LE(ehdr, 56);
            if (phentsize < 56 || phnum <= 0 || phnum > 128)
                throw new IOException("Malformed ELF program headers");

            long minAlign = Long.MAX_VALUE;
            byte[] ph = new byte[phentsize];
            for (int i = 0; i < phnum; i++) {
                raf.seek(phoff + (long) i * phentsize);
                raf.readFully(ph);
                long pType = readU32LE(ph, 0);
                if (pType == 1) { /* PT_LOAD */
                    long pAlign = readU64LE(ph, 48);
                    if (pAlign < minAlign)
                        minAlign = pAlign;
                }
            }
            if (minAlign == Long.MAX_VALUE)
                throw new IOException("No PT_LOAD segments found");
            if (minAlign < 16384)
                throw new IOException("libndi.so is not 16 KB page-aligned (p_align=" + minAlign
                        + "); please use NDI SDK v6+ for Android");
            return minAlign;
        }
    }
}
