/* SPDX-License-Identifier: GPL-2.0-or-later */
package com.obsproject.studio;

import android.content.ContentValues;
import android.content.Context;
import android.database.Cursor;
import android.net.Uri;
import android.os.Environment;
import android.os.ParcelFileDescriptor;
import android.os.StatFs;
import android.os.Build;
import android.os.storage.StorageManager;
import android.os.storage.StorageVolume;
import android.provider.DocumentsContract;
import android.provider.MediaStore;
import android.util.Log;
import java.io.File;

/** Scoped-storage recording destinations. No all-files-access permission. */
public final class ObsStorageHost {
    private static final String TAG = "OBS-storage";
    private ObsStorageHost() {}
    private static String relative(String path) {
        if (path == null || path.isEmpty()) return null;
        String root = Environment.getExternalStorageDirectory().getAbsolutePath();
        String p = path.startsWith(root + "/") ? path.substring(root.length() + 1) : path;
        if (p.startsWith("/sdcard/")) p = p.substring(8);
        if (p.endsWith("/")) p = p.substring(0, p.length() - 1);
        if (p.contains("..") || p.contains("\\")) return null;
        for (String folder : new String[]{Environment.DIRECTORY_DOWNLOADS, Environment.DIRECTORY_MOVIES, Environment.DIRECTORY_DCIM})
            if (p.equals(folder) || p.startsWith(folder + "/")) return p + "/";
        return null;
    }
    public static boolean managed(String path) { return path != null && (path.startsWith("content://") || relative(path) != null); }
    public static boolean writable(Context context, String path) {
        if (relative(path) != null) return true;
        try {
            Uri tree = Uri.parse(path);
            Uri doc = DocumentsContract.buildDocumentUriUsingTree(tree, DocumentsContract.getTreeDocumentId(tree));
            try (Cursor c = context.getContentResolver().query(doc,
                    new String[]{DocumentsContract.Document.COLUMN_FLAGS}, null, null, null)) {
                return c != null && c.moveToFirst() && (c.getLong(0) & DocumentsContract.Document.FLAG_DIR_SUPPORTS_CREATE) != 0
                        && context.checkUriPermission(doc, android.os.Process.myPid(), android.os.Process.myUid(),
                            android.content.Intent.FLAG_GRANT_WRITE_URI_PERMISSION) == android.content.pm.PackageManager.PERMISSION_GRANTED;
            }
        } catch (Exception e) { Log.e(TAG, "Directory permission unavailable; select it again", e); return false; }
    }
    public static String create(Context context, String path, String name) {
        try {
            if (name.isEmpty() || name.contains("/") || name.contains("\\"))
                throw new IllegalArgumentException("Filename subdirectories are unsupported for document destinations");
            String mime = name.endsWith(".mov") ? "video/quicktime" : name.endsWith(".flv") ? "video/x-flv" :
                    name.endsWith(".mkv") ? "video/x-matroska" : "video/mp4";
            Uri result;
            if (path.startsWith("content://")) {
                Uri tree = Uri.parse(path);
                try {
                    context.getContentResolver().takePersistableUriPermission(tree,
                        android.content.Intent.FLAG_GRANT_READ_URI_PERMISSION | android.content.Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
                } catch (SecurityException e) { Log.w(TAG, "Cannot extend directory grant; using existing permission", e); }
                Uri parent = DocumentsContract.buildDocumentUriUsingTree(tree, DocumentsContract.getTreeDocumentId(tree));
                result = DocumentsContract.createDocument(context.getContentResolver(), parent, mime, name);
            } else {
                String rel = relative(path);
                if (rel == null) return "";
                ContentValues values = new ContentValues();
                values.put(MediaStore.MediaColumns.DISPLAY_NAME, name);
                values.put(MediaStore.MediaColumns.MIME_TYPE, mime);
                values.put(MediaStore.MediaColumns.RELATIVE_PATH, rel);
                Uri collection = rel.startsWith(Environment.DIRECTORY_DOWNLOADS + "/") ?
                    MediaStore.Downloads.EXTERNAL_CONTENT_URI : MediaStore.Video.Media.EXTERNAL_CONTENT_URI;
                result = context.getContentResolver().insert(collection, values);
            }
            if (result == null) throw new IllegalStateException("Provider could not create recording");
            return result.toString();
        } catch (Exception e) { Log.e(TAG, "Cannot create recording in " + path, e); return ""; }
    }
    public static int open(Context context, String path, String mode) {
        try (ParcelFileDescriptor fd = context.getContentResolver().openFileDescriptor(Uri.parse(path),
                mode.startsWith("w") ? "rwt" : mode.startsWith("a") ? "wa" : "r")) {
            if (fd == null) return -1;
            // MP4 finalisation seeks. Reject pipe/cloud providers before recording.
            android.system.Os.lseek(fd.getFileDescriptor(), 0, android.system.OsConstants.SEEK_CUR);
            return fd.detachFd(); // fclose in libobs owns this descriptor.
        } catch (Exception e) { Log.e(TAG, "Cannot open seekable recording document", e); return -1; }
    }
    public static long space(Context context, String path) {
        try {
            if (relative(path) != null) return new StatFs(Environment.getExternalStorageDirectory().getAbsolutePath()).getAvailableBytes();
            if (path.startsWith("content://")) {
                Uri tree = Uri.parse(path);
                if (!"com.android.externalstorage.documents".equals(tree.getAuthority())) return -1;
                String id = DocumentsContract.getTreeDocumentId(tree);
                int separator = id.indexOf(':');
                if (separator < 0) return -1;
                String volumeId = id.substring(0, separator);
                if ("primary".equalsIgnoreCase(volumeId))
                    return new StatFs(Environment.getExternalStorageDirectory().getAbsolutePath()).getAvailableBytes();
                // Match only an OS-reported mounted volume, never a path supplied by a provider.
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                    StorageManager manager = (StorageManager) context.getSystemService(Context.STORAGE_SERVICE);
                    if (manager == null) return -1;
                    for (StorageVolume volume : manager.getStorageVolumes()) {
                        String uuid = volume.getUuid();
                        File root = volume.getDirectory();
                        if (uuid != null && uuid.equalsIgnoreCase(volumeId) && root != null &&
                                Environment.MEDIA_MOUNTED.equals(volume.getState()))
                            return new StatFs(root.getAbsolutePath()).getAvailableBytes();
                    }
                }
            } else {
                File f = new File(path);
                while (f != null && !f.exists()) f = f.getParentFile();
                if (f != null) return new StatFs(f.getAbsolutePath()).getAvailableBytes();
            }
        } catch (Exception e) { Log.w(TAG, "Available space unknown", e); }
        return -1; // Unknown is not an empty disk; actual writes still report ENOSPC.
    }
}
