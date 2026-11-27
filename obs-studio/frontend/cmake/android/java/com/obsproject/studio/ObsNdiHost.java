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
import android.net.nsd.NsdManager;
import android.net.nsd.NsdServiceInfo;
import android.net.wifi.WifiManager;
import android.util.Log;

import java.net.Inet4Address;
import java.net.InetAddress;
import java.util.ArrayDeque;
import java.util.LinkedHashMap;
import java.util.Map;

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

    static synchronized Context appContext() {
        return appContext;
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
}
