package com.obsproject.studio;

import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.net.Uri;
import android.os.Build;
import android.util.Log;
import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.InterruptedIOException;
import java.net.ConnectException;
import java.net.HttpURLConnection;
import java.net.SocketTimeoutException;
import java.net.URL;
import java.net.UnknownHostException;
import java.nio.charset.StandardCharsets;
import org.json.JSONException;
import org.json.JSONObject;

public final class ObsUpdateHost {
    private static final String TAG = "ObsUpdateHost";
    private static final int CONNECT_TIMEOUT_MS = 2500;
    private static final int READ_TIMEOUT_MS = 3500;
    private static final int MAX_BODY_BYTES = 256 * 1024;
    private static final String USER_AGENT = "OBS-for-Android-Updater";
    private static final Object connectionLock = new Object();
    private static volatile HttpURLConnection activeConnection;
    private static volatile boolean cancelled = false;

    private ObsUpdateHost() {
    }

    public static void cancelChecks() {
        cancelled = true;
        HttpURLConnection conn;
        synchronized (connectionLock) {
            conn = activeConnection;
        }
        if (conn != null) {
            try {
                conn.disconnect();
            } catch (Throwable ignored) {
            }
        }
    }

    public static String installedVersionName(Context context) {
        if (context == null) {
            return "";
        }
        try {
            PackageManager pm = context.getPackageManager();
            if (pm == null) {
                return "";
            }
            PackageInfo info;
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                info = pm.getPackageInfo(
                        context.getPackageName(),
                        PackageManager.PackageInfoFlags.of(0));
            } else {
                info = getLegacyPackageInfo(pm, context.getPackageName());
            }
            return info != null && info.versionName != null ? info.versionName.trim() : "";
        } catch (Throwable t) {
            Log.w(TAG, "installedVersionName failed", t);
            return "";
        }
    }

    @SuppressWarnings("deprecation")
    private static PackageInfo getLegacyPackageInfo(PackageManager pm, String packageName)
            throws PackageManager.NameNotFoundException {
        return pm.getPackageInfo(packageName, 0);
    }

    public static String fetchLatestRelease(String repository) {
        if (repository == null || repository.trim().isEmpty()) {
            return buildError("Missing repository name");
        }
        if (cancelled || Thread.currentThread().isInterrupted()) {
            return buildError("Cancelled");
        }
        String repo = repository.trim();
        String apiError = null;
        boolean skipRedirectFallback = false;

        try {
            JSONObject apiResult = fetchFromGitHubApi(repo);
            if (apiResult != null) {
                return apiResult.toString();
            }
        } catch (Exception e) {
            apiError = e.getMessage() != null ? e.getMessage() : e.getClass().getSimpleName();
            skipRedirectFallback = cancelled || Thread.currentThread().isInterrupted() ||
                                   e instanceof SocketTimeoutException || e instanceof InterruptedIOException ||
                                   e instanceof UnknownHostException || e instanceof ConnectException;
            Log.w(TAG, "GitHub API release check failed"
                       + (skipRedirectFallback ? ": " : ", trying web redirect fallback: ") + apiError);
        }

        if (skipRedirectFallback || cancelled || Thread.currentThread().isInterrupted()) {
            return buildError(apiError != null ? apiError : "Cancelled");
        }

        try {
            JSONObject redirectResult = fetchFromReleasesRedirect(repo);
            if (redirectResult != null) {
                return redirectResult.toString();
            }
        } catch (Exception e) {
            String redirectError = e.getMessage() != null ? e.getMessage() : e.getClass().getSimpleName();
            Log.w(TAG, "GitHub redirect release check failed: " + redirectError);
            if (apiError == null) {
                apiError = redirectError;
            }
        }

        return buildError(apiError != null ? apiError : "Unable to query latest release");
    }

    public static boolean openUrl(Context context, String url) {
        if (context == null || url == null || url.trim().isEmpty()) {
            return false;
        }
        String trimmed = url.trim();
        if (!trimmed.startsWith("https://") && !trimmed.startsWith("http://")) {
            return false;
        }
        try {
            Intent intent = new Intent(Intent.ACTION_VIEW, Uri.parse(trimmed));
            if (!(context instanceof Activity)) {
                intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
            }
            context.startActivity(intent);
            return true;
        } catch (Throwable t) {
            Log.w(TAG, "openUrl failed for " + trimmed, t);
            return false;
        }
    }

    private static JSONObject fetchFromGitHubApi(String repo) throws IOException, JSONException {
        if (cancelled || Thread.currentThread().isInterrupted()) {
            throw new IOException("Cancelled");
        }
        String apiUrl = "https://api.github.com/repos/" + repo + "/releases/latest";
        HttpURLConnection conn = null;
        try {
            URL url = new URL(apiUrl);
            conn = (HttpURLConnection) url.openConnection();
            synchronized (connectionLock) {
                activeConnection = conn;
            }
            if (cancelled || Thread.currentThread().isInterrupted()) {
                conn.disconnect();
                throw new IOException("Cancelled");
            }
            conn.setRequestMethod("GET");
            conn.setConnectTimeout(CONNECT_TIMEOUT_MS);
            conn.setReadTimeout(READ_TIMEOUT_MS);
            conn.setInstanceFollowRedirects(true);
            conn.setRequestProperty("Accept", "application/vnd.github+json");
            conn.setRequestProperty("X-GitHub-Api-Version", "2022-11-28");
            conn.setRequestProperty("User-Agent", USER_AGENT);

            int code = conn.getResponseCode();
            if (code < 200 || code >= 300) {
                throw new IOException("HTTP " + code);
            }

            String payload = readUtf8(conn.getInputStream());
            JSONObject json = new JSONObject(payload);
            if (json.optBoolean("draft", false)) {
                throw new IOException("Latest release entry is a draft");
            }

            String tag = json.optString("tag_name", "").trim();
            if (tag.isEmpty()) {
                throw new IOException("Missing tag_name in release response");
            }
            String version = normalizeVersion(tag);
            String name = json.optString("name", "").trim();
            if (name.isEmpty()) {
                name = tag;
            }
            String htmlUrl = json.optString("html_url", "").trim();
            if (htmlUrl.isEmpty()) {
                htmlUrl = "https://github.com/" + repo + "/releases/tag/" + tag;
            }
            String notes = json.optString("body", "");

            return buildSuccess(tag, version, name, htmlUrl, notes);
        } finally {
            synchronized (connectionLock) {
                if (activeConnection == conn) {
                    activeConnection = null;
                }
            }
            if (conn != null) {
                conn.disconnect();
            }
        }
    }

    private static JSONObject fetchFromReleasesRedirect(String repo) throws IOException, JSONException {
        if (cancelled || Thread.currentThread().isInterrupted()) {
            throw new IOException("Cancelled");
        }
        String webUrl = "https://github.com/" + repo + "/releases/latest";
        HttpURLConnection conn = null;
        try {
            URL url = new URL(webUrl);
            conn = (HttpURLConnection) url.openConnection();
            synchronized (connectionLock) {
                activeConnection = conn;
            }
            if (cancelled || Thread.currentThread().isInterrupted()) {
                conn.disconnect();
                throw new IOException("Cancelled");
            }
            conn.setRequestMethod("HEAD");
            conn.setConnectTimeout(CONNECT_TIMEOUT_MS);
            conn.setReadTimeout(READ_TIMEOUT_MS);
            conn.setInstanceFollowRedirects(false);
            conn.setRequestProperty("User-Agent", USER_AGENT);

            int code = conn.getResponseCode();
            String location = conn.getHeaderField("Location");
            if (code < 300 || code >= 400 || location == null || location.isEmpty()) {
                throw new IOException("HTTP " + code);
            }
            if (location.startsWith("/")) {
                location = "https://github.com" + location;
            }

            final String marker = "/releases/tag/";
            int idx = location.indexOf(marker);
            if (idx < 0) {
                throw new IOException("Unexpected redirect location: " + location);
            }
            String tag = location.substring(idx + marker.length());
            int queryIdx = tag.indexOf('?');
            if (queryIdx >= 0) {
                tag = tag.substring(0, queryIdx);
            }
            int hashIdx = tag.indexOf('#');
            if (hashIdx >= 0) {
                tag = tag.substring(0, hashIdx);
            }
            tag = tag.trim();
            if (tag.isEmpty()) {
                throw new IOException("Empty release tag in redirect");
            }

            String version = normalizeVersion(tag);
            return buildSuccess(tag, version, tag, location, "");
        } finally {
            synchronized (connectionLock) {
                if (activeConnection == conn) {
                    activeConnection = null;
                }
            }
            if (conn != null) {
                conn.disconnect();
            }
        }
    }

    private static JSONObject buildSuccess(
            String tag,
            String version,
            String name,
            String htmlUrl,
            String notes) throws JSONException {
        JSONObject out = new JSONObject();
        out.put("ok", true);
        out.put("tag", tag);
        out.put("version", version);
        out.put("name", name);
        out.put("url", htmlUrl);
        out.put("notes", notes != null ? notes : "");
        return out;
    }

    private static String buildError(String message) {
        try {
            JSONObject out = new JSONObject();
            out.put("ok", false);
            out.put("error", message != null ? message : "Unknown error");
            return out.toString();
        } catch (JSONException e) {
            return "{\"ok\":false,\"error\":\"Unknown error\"}";
        }
    }

    private static String normalizeVersion(String tag) {
        if (tag == null) {
            return "";
        }
        String trimmed = tag.trim();
        if ((trimmed.startsWith("v") || trimmed.startsWith("V")) && trimmed.length() > 1) {
            return trimmed.substring(1).trim();
        }
        return trimmed;
    }

    private static String readUtf8(InputStream in) throws IOException {
        if (in == null) {
            return "";
        }
        try {
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            byte[] buf = new byte[4096];
            int total = 0;
            int read;
            while ((read = in.read(buf)) != -1) {
                total += read;
                if (total > MAX_BODY_BYTES) {
                    throw new IOException("Response exceeds max size");
                }
                out.write(buf, 0, read);
            }
            return new String(out.toByteArray(), StandardCharsets.UTF_8);
        } finally {
            in.close();
        }
    }
}
