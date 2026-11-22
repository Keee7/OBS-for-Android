/* SPDX-License-Identifier: GPL-2.0-or-later */
package com.obsproject.studio;

import android.content.ContentValues;
import android.content.ContentResolver;
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
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.Arrays;
import java.util.List;

/** Scoped-storage recording destinations. No all-files-access permission. */
public final class ObsStorageHost {
    private static final String TAG = "OBS-storage";
    private ObsStorageHost() {}

    public static String defaultVideoDirectory() {
        if (!Environment.MEDIA_MOUNTED.equals(Environment.getExternalStorageState())) return "";
        File movies = Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_MOVIES);
        return movies == null ? "" : new File(movies, "OBS").getAbsolutePath();
    }

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
        return create(context, path, name, false);
    }

    public static String createReplay(Context context, String path, String name) {
        return create(context, path, name, true);
    }

    private static String create(Context context, String path, String name, boolean pending) {
        try {
            if (name.isEmpty() || name.contains("/") || name.contains("\\"))
                throw new IllegalArgumentException("Filename subdirectories are unsupported for document destinations");
            boolean image = name.endsWith(".png");
            String mime = image ? "image/png" : name.endsWith(".mov") ? "video/quicktime" :
                    name.endsWith(".flv") ? "video/x-flv" :
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
                // MediaStore stores screenshots in Pictures when recordings are under Movies.
                if (image && rel.startsWith(Environment.DIRECTORY_MOVIES + "/"))
                    rel = Environment.DIRECTORY_PICTURES + rel.substring(Environment.DIRECTORY_MOVIES.length());
                ContentValues values = new ContentValues();
                values.put(MediaStore.MediaColumns.DISPLAY_NAME, name);
                values.put(MediaStore.MediaColumns.MIME_TYPE, mime);
                values.put(MediaStore.MediaColumns.RELATIVE_PATH, rel);
                if (pending) values.put(MediaStore.MediaColumns.IS_PENDING, 1);
                Uri collection;
                if (rel.startsWith(Environment.DIRECTORY_DOWNLOADS + "/"))
                    collection = MediaStore.Downloads.EXTERNAL_CONTENT_URI;
                else if (image)
                    collection = MediaStore.Images.Media.getContentUri(MediaStore.VOLUME_EXTERNAL_PRIMARY);
                else
                    collection = MediaStore.Video.Media.EXTERNAL_CONTENT_URI;
                result = context.getContentResolver().insert(collection, values);
            }
            if (result == null) throw new IllegalStateException("Provider could not create output document");
            return result.toString();
        } catch (Exception e) { Log.e(TAG, "Cannot create output document in " + path, e); return ""; }
    }

    public static boolean remove(Context context, String path) {
        try {
            Uri uri = Uri.parse(path);
            if (DocumentsContract.isDocumentUri(context, uri))
                return DocumentsContract.deleteDocument(context.getContentResolver(), uri);
            return context.getContentResolver().delete(uri, null, null) == 1;
        } catch (Exception e) {
            Log.w(TAG, "Cannot remove incomplete document", e);
            return false;
        }
    }

    public static boolean finishReplay(Context context, String path, boolean publish) {
        if (!publish) return remove(context, path);
        Uri uri = Uri.parse(path);
        try {
            if (DocumentsContract.isDocumentUri(context, uri)) return true;
            ContentValues values = new ContentValues();
            values.put(MediaStore.MediaColumns.IS_PENDING, 0);
            return context.getContentResolver().update(uri, values, null, null) == 1;
        } catch (Exception e) {
            Log.e(TAG, "Cannot finish replay document", e);
            return false;
        }
    }

    public static boolean exportProfile(Context context, String treePath, String sourcePath,
                                        String directoryName, String[] files) {
        ContentResolver resolver = context.getContentResolver();
        Uri folder = null;
        try {
            Uri tree = Uri.parse(treePath);
            Uri parent = DocumentsContract.buildDocumentUriUsingTree(tree, DocumentsContract.getTreeDocumentId(tree));
            File source = new File(sourcePath);
            if (!new File(source, "basic.ini").isFile()) throw new IOException("Profile has no basic.ini");

            // Use a new folder so a failed export cannot damage an earlier backup.
            folder = DocumentsContract.createDocument(resolver, parent,
                    DocumentsContract.Document.MIME_TYPE_DIR, directoryName);
            if (folder == null) throw new IOException("Could not create profile directory");

            byte[] buffer = new byte[16384];
            for (String name : files) {
                File file = new File(source, name);
                if (!file.exists() && !"basic.ini".equals(name)) continue;
                String mime = name.endsWith(".json") ? "application/json" : "application/octet-stream";
                Uri document = DocumentsContract.createDocument(resolver, folder, mime, name);
                if (document == null) throw new IOException("Could not create " + name);
                try (InputStream input = new FileInputStream(file);
                     OutputStream output = resolver.openOutputStream(document, "wt")) {
                    copyProfileFile(input, output, buffer);
                }
            }
            return true;
        } catch (Exception e) {
            Log.e(TAG, "Could not export profile", e);
            if (folder != null) remove(context, folder.toString());
            return false;
        }
    }

    public static String importProfile(Context context, String treePath, String destinationPath, String[] files) {
        ContentResolver resolver = context.getContentResolver();
        try {
            Uri tree = Uri.parse(treePath);
            String id = DocumentsContract.getTreeDocumentId(tree);
            Uri folder = DocumentsContract.buildDocumentUriUsingTree(tree, id);
            String directoryName;
            try (Cursor directory = resolver.query(folder,
                    new String[]{DocumentsContract.Document.COLUMN_DISPLAY_NAME}, null, null, null)) {
                if (directory == null || !directory.moveToFirst()) throw new IOException("Could not read profile directory");
                directoryName = directory.getString(0);
            }

            Uri children = DocumentsContract.buildChildDocumentsUriUsingTree(tree, id);
            String[] columns = {DocumentsContract.Document.COLUMN_DOCUMENT_ID, DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                    DocumentsContract.Document.COLUMN_MIME_TYPE};
            byte[] buffer = new byte[16384];
            List<String> profileFiles = Arrays.asList(files);
            try (Cursor list = resolver.query(children, columns, null, null, null)) {
                if (list == null) throw new IOException("Could not list profile directory");
                while (list.moveToNext()) {
                    String name = list.getString(1);
                    if (!profileFiles.contains(name) ||
                            DocumentsContract.Document.MIME_TYPE_DIR.equals(list.getString(2))) continue;
                    File destination = new File(destinationPath, name);
                    if (destination.exists()) throw new IOException("Duplicate profile file " + name);
                    Uri document = DocumentsContract.buildDocumentUriUsingTree(tree, list.getString(0));
                    try (InputStream input = resolver.openInputStream(document);
                         OutputStream output = new FileOutputStream(destination)) {
                        copyProfileFile(input, output, buffer);
                    }
                }
            }
            return directoryName;
        } catch (Exception e) {
            Log.e(TAG, "Could not import profile", e);
            return "";
        }
    }

    private static void copyProfileFile(InputStream input, OutputStream output, byte[] buffer) throws IOException {
        if (input == null || output == null) throw new IOException("Could not open profile file");
        int size;
        while ((size = input.read(buffer)) != -1) output.write(buffer, 0, size);
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
