package org.libsdl.app;

import android.app.Activity;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.pm.Signature;
import android.net.Uri;
import android.os.Build;
import android.provider.Settings;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.security.MessageDigest;
import java.util.Arrays;
import java.util.Locale;

/** GitHub release checker/downloader for issue #83. */
final class UpdateManager {
    private static final String API_URL =
        "https://api.github.com/repos/SansNope/UnleashedRecomp-Android/releases/latest";
    private static final String PREFS = "updates";
    private static final String PENDING_INSTALL = "pending_install";
    private static final long MAX_API_BYTES = 2 * 1024 * 1024;
    private static final long MAX_APK_BYTES = 300L * 1024 * 1024;

    private UpdateManager() {}

    static final class UpdateInfo {
        final String version;
        final String notes;
        final String downloadUrl;
        final String digest;
        final long size;

        UpdateInfo(String version, String notes, String downloadUrl, String digest, long size) {
            this.version = version;
            this.notes = notes;
            this.downloadUrl = downloadUrl;
            this.digest = digest;
            this.size = size;
        }
    }

    interface CheckCallback {
        void complete(UpdateInfo update, String error);
    }

    interface DownloadCallback {
        void progress(long downloaded, long total);
        void complete(String error);
    }

    static void check(Activity activity, CheckCallback callback) {
        new Thread(() -> {
            UpdateInfo update = null;
            String error = null;
            try {
                HttpURLConnection connection = open(API_URL);
                String json = new String(readLimited(connection, MAX_API_BYTES), StandardCharsets.UTF_8);
                JSONObject release = new JSONObject(json);
                String version = normalizeVersion(release.optString("tag_name", ""));
                if (!version.isEmpty() && compareVersions(version, currentVersion(activity)) > 0) {
                    JSONArray assets = release.optJSONArray("assets");
                    JSONObject apk = null;
                    if (assets != null) {
                        for (int index = 0; index < assets.length(); index++) {
                            JSONObject candidate = assets.optJSONObject(index);
                            String name = candidate != null ? candidate.optString("name", "") : "";
                            if (name.toLowerCase(Locale.ROOT).endsWith(".apk")) {
                                apk = candidate;
                                if (name.contains(version)) break;
                            }
                        }
                    }
                    if (apk == null) throw new IOException("The latest release has no APK asset");
                    long size = apk.optLong("size", -1);
                    if (size <= 0 || size > MAX_APK_BYTES) throw new IOException("Invalid update APK size");
                    String url = apk.optString("browser_download_url", "");
                    if (!url.startsWith("https://github.com/SansNope/UnleashedRecomp-Android/releases/")) {
                        throw new IOException("Unexpected update download URL");
                    }
                    update = new UpdateInfo(version, release.optString("body", ""), url,
                        apk.optString("digest", ""), size);
                }
            } catch (Exception exception) {
                error = readableError(exception);
            }
            UpdateInfo result = update;
            String resultError = error;
            activity.runOnUiThread(() -> callback.complete(result, resultError));
        }, "update-check").start();
    }

    static void download(Activity activity, UpdateInfo update, DownloadCallback callback) {
        new Thread(() -> {
            String error = null;
            File temporary = null;
            try {
                File directory = new File(activity.getCacheDir(), "updates");
                if (!directory.isDirectory() && !directory.mkdirs()) {
                    throw new IOException("Cannot create the update cache");
                }
                temporary = new File(directory, UpdateFileProvider.FILE_NAME + ".part");
                File target = new File(directory, UpdateFileProvider.FILE_NAME);
                HttpURLConnection connection = open(update.downloadUrl);
                long total = connection.getContentLengthLong();
                if (total <= 0) total = update.size;
                if (total <= 0 || total > MAX_APK_BYTES) throw new IOException("Invalid download size");

                MessageDigest sha256 = MessageDigest.getInstance("SHA-256");
                long downloaded = 0;
                try (InputStream input = connection.getInputStream();
                     FileOutputStream output = new FileOutputStream(temporary)) {
                    byte[] buffer = new byte[256 * 1024];
                    int count;
                    long nextProgress = 0;
                    while ((count = input.read(buffer)) >= 0) {
                        downloaded += count;
                        if (downloaded > MAX_APK_BYTES) throw new IOException("Update APK is too large");
                        output.write(buffer, 0, count);
                        sha256.update(buffer, 0, count);
                        if (downloaded >= nextProgress) {
                            nextProgress = downloaded + 1024 * 1024;
                            long current = downloaded;
                            long expected = total;
                            activity.runOnUiThread(() -> callback.progress(current, expected));
                        }
                    }
                    output.flush();
                    output.getFD().sync();
                }
                if (update.size > 0 && downloaded != update.size) throw new IOException("Incomplete APK download");
                String actualDigest = "sha256:" + hex(sha256.digest());
                if (!update.digest.isEmpty() && !update.digest.equalsIgnoreCase(actualDigest)) {
                    throw new IOException("The downloaded APK checksum does not match GitHub");
                }
                if (!hasMatchingSignature(activity, temporary)) {
                    throw new IOException("The downloaded APK is signed by a different key");
                }
                Files.move(temporary.toPath(), target.toPath(), StandardCopyOption.REPLACE_EXISTING);
            } catch (Exception exception) {
                error = readableError(exception);
                if (temporary != null && temporary.isFile()) temporary.delete();
            }
            String resultError = error;
            activity.runOnUiThread(() -> {
                if (resultError == null) requestInstall(activity);
                callback.complete(resultError);
            });
        }, "update-download").start();
    }

    static void resumePendingInstall(Activity activity) {
        SharedPreferences preferences = activity.getSharedPreferences(PREFS, Activity.MODE_PRIVATE);
        if (!preferences.getBoolean(PENDING_INSTALL, false)) return;
        File apk = updateFile(activity);
        if (!apk.isFile()) {
            preferences.edit().remove(PENDING_INSTALL).apply();
            return;
        }
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O ||
                activity.getPackageManager().canRequestPackageInstalls()) {
            preferences.edit().remove(PENDING_INSTALL).apply();
            launchInstaller(activity);
        }
    }

    private static void requestInstall(Activity activity) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O &&
                !activity.getPackageManager().canRequestPackageInstalls()) {
            activity.getSharedPreferences(PREFS, Activity.MODE_PRIVATE).edit()
                .putBoolean(PENDING_INSTALL, true).apply();
            Intent permission = new Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES,
                Uri.parse("package:" + activity.getPackageName()));
            activity.startActivity(permission);
            return;
        }
        launchInstaller(activity);
    }

    private static void launchInstaller(Activity activity) {
        Uri apk = new Uri.Builder().scheme("content")
            .authority(activity.getPackageName() + ".updates").path("apk").build();
        Intent install = new Intent(Intent.ACTION_VIEW);
        install.setDataAndType(apk, "application/vnd.android.package-archive");
        install.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
        activity.startActivity(install);
    }

    private static File updateFile(Activity activity) {
        return new File(new File(activity.getCacheDir(), "updates"), UpdateFileProvider.FILE_NAME);
    }

    private static HttpURLConnection open(String value) throws IOException {
        HttpURLConnection connection = (HttpURLConnection) new URL(value).openConnection();
        connection.setConnectTimeout(15000);
        connection.setReadTimeout(30000);
        connection.setInstanceFollowRedirects(true);
        connection.setRequestProperty("Accept", "application/vnd.github+json");
        connection.setRequestProperty("User-Agent", "UnleashedRecomp-Android/Android");
        int status = connection.getResponseCode();
        if (status < 200 || status >= 300) throw new IOException("GitHub returned HTTP " + status);
        return connection;
    }

    private static byte[] readLimited(HttpURLConnection connection, long limit) throws IOException {
        try (InputStream input = connection.getInputStream();
             ByteArrayOutputStream output = new ByteArrayOutputStream()) {
            byte[] buffer = new byte[32 * 1024];
            long total = 0;
            int count;
            while ((count = input.read(buffer)) >= 0) {
                total += count;
                if (total > limit) throw new IOException("GitHub response is too large");
                output.write(buffer, 0, count);
            }
            return output.toByteArray();
        }
    }

    private static boolean hasMatchingSignature(Activity activity, File candidate) throws Exception {
        PackageManager manager = activity.getPackageManager();
        PackageInfo current = manager.getPackageInfo(activity.getPackageName(),
            PackageManager.GET_SIGNING_CERTIFICATES);
        PackageInfo archive = manager.getPackageArchiveInfo(candidate.getAbsolutePath(),
            PackageManager.GET_SIGNING_CERTIFICATES);
        if (archive == null || archive.signingInfo == null || current.signingInfo == null ||
                !activity.getPackageName().equals(archive.packageName)) return false;
        Signature[] currentSigners = current.signingInfo.getApkContentsSigners();
        Signature[] archiveSigners = archive.signingInfo.getApkContentsSigners();
        if (currentSigners.length != archiveSigners.length) return false;
        byte[][] currentDigests = signatureDigests(currentSigners);
        byte[][] archiveDigests = signatureDigests(archiveSigners);
        for (byte[] digest : currentDigests) {
            boolean found = false;
            for (byte[] other : archiveDigests) found |= Arrays.equals(digest, other);
            if (!found) return false;
        }
        return true;
    }

    private static byte[][] signatureDigests(Signature[] signatures) throws Exception {
        byte[][] result = new byte[signatures.length][];
        for (int index = 0; index < signatures.length; index++) {
            result[index] = MessageDigest.getInstance("SHA-256").digest(signatures[index].toByteArray());
        }
        return result;
    }

    static String currentVersion(Activity activity) {
        try {
            return activity.getPackageManager().getPackageInfo(activity.getPackageName(), 0).versionName;
        } catch (PackageManager.NameNotFoundException exception) {
            return "0.0.0";
        }
    }

    private static String normalizeVersion(String value) {
        value = value != null ? value.trim() : "";
        return value.startsWith("v") || value.startsWith("V") ? value.substring(1) : value;
    }

    static int compareVersions(String left, String right) {
        String[] a = normalizeVersion(left).split("[-+]", 2)[0].split("\\.");
        String[] b = normalizeVersion(right).split("[-+]", 2)[0].split("\\.");
        int count = Math.max(a.length, b.length);
        for (int index = 0; index < count; index++) {
            int av = index < a.length ? parseVersionPart(a[index]) : 0;
            int bv = index < b.length ? parseVersionPart(b[index]) : 0;
            if (av != bv) return Integer.compare(av, bv);
        }
        return 0;
    }

    private static int parseVersionPart(String value) {
        try { return Integer.parseInt(value.replaceAll("[^0-9].*$", "")); }
        catch (NumberFormatException exception) { return 0; }
    }

    private static String hex(byte[] bytes) {
        StringBuilder result = new StringBuilder(bytes.length * 2);
        for (byte value : bytes) result.append(String.format(Locale.ROOT, "%02x", value & 0xff));
        return result.toString();
    }

    private static String readableError(Exception exception) {
        return exception.getMessage() != null ? exception.getMessage() : exception.getClass().getSimpleName();
    }
}
