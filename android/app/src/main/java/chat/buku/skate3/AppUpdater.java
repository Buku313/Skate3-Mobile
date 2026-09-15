package chat.buku.skate3;

import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.content.pm.PackageInfo;
import android.content.pm.Signature;
import android.net.Uri;
import android.provider.Settings;

import org.json.JSONObject;

import java.io.BufferedInputStream;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.Locale;
import java.util.HashSet;
import java.util.Set;

final class AppUpdater {
    private static final long MAX_APK_SIZE = 350L * 1024 * 1024;

    interface Progress {
        void update(long copied, long total);
    }

    static final class UpdateInfo {
        final long versionCode;
        final String versionName;
        final String apkUrl;
        final String sha256;
        final String notes;

        UpdateInfo(long versionCode, String versionName, String apkUrl,
                   String sha256, String notes) {
            this.versionCode = versionCode;
            this.versionName = versionName;
            this.apkUrl = apkUrl;
            this.sha256 = sha256;
            this.notes = notes;
        }
    }

    private AppUpdater() {}

    static UpdateInfo check(Context context) throws Exception {
        String packageName = context.getPackageName();
        String manifestUrl = UpdatePolicy.manifestUrl(packageName);
        if (manifestUrl == null) return null;
        JSONObject json = new JSONObject(new String(
            downloadBytes(manifestUrl, 256 * 1024), StandardCharsets.UTF_8));
        UpdateInfo info = new UpdateInfo(
            json.getLong("versionCode"),
            json.getString("versionName"),
            json.getString("apkUrl"),
            json.getString("sha256").toLowerCase(Locale.US),
            json.optString("notes", "A new build is ready."));
        UpdatePolicy.validateManifest(packageName, json.getString("packageName"),
            info.versionCode, info.apkUrl, info.sha256);
        long installed = context.getPackageManager()
            .getPackageInfo(context.getPackageName(), 0).getLongVersionCode();
        return info.versionCode > installed ? info : null;
    }

    static File downloadApk(Context context, UpdateInfo info, Progress progress)
            throws Exception {
        File directory = new File(context.getCacheDir(), "updates");
        if (!directory.exists() && !directory.mkdirs()) {
            throw new IOException("Could not create the update cache.");
        }
        File pending = new File(directory, "update.apk.part");
        File ready = new File(directory, "update.apk");
        if (pending.exists() && !pending.delete()) {
            throw new IOException("Could not clear the previous update download.");
        }
        if (ready.exists() && !ready.delete()) {
            throw new IOException("Could not clear the previous update package.");
        }

        HttpURLConnection connection = open(info.apkUrl);
        long total = connection.getContentLengthLong();
        if (total > MAX_APK_SIZE) {
            connection.disconnect();
            throw new IOException("The update package is unexpectedly large.");
        }
        MessageDigest digest = sha256();
        long copied = 0;
        byte[] buffer = new byte[256 * 1024];
        try (BufferedInputStream input = new BufferedInputStream(connection.getInputStream());
             FileOutputStream output = new FileOutputStream(pending)) {
            for (;;) {
                int read = input.read(buffer);
                if (read < 0) break;
                copied += read;
                if (copied > MAX_APK_SIZE) {
                    throw new IOException("The update package is unexpectedly large.");
                }
                output.write(buffer, 0, read);
                digest.update(buffer, 0, read);
                if (progress != null) progress.update(copied, total);
            }
            output.getFD().sync();
        } catch (Exception exception) {
            pending.delete();
            throw exception;
        } finally {
            connection.disconnect();
        }
        String actual = hex(digest.digest());
        if (!actual.equals(info.sha256)) {
            pending.delete();
            throw new IOException("Update verification failed. Expected " + info.sha256 +
                                  " but downloaded " + actual + ".");
        }
        try {
            verifyPackage(context, pending, info.versionCode);
        } catch (Exception exception) {
            pending.delete();
            throw exception;
        }
        if (!pending.renameTo(ready)) {
            pending.delete();
            throw new IOException("Could not finalize the verified update package.");
        }
        return ready;
    }

    private static void verifyPackage(Context context, File apk, long expectedVersion)
            throws Exception {
        PackageManager manager = context.getPackageManager();
        PackageInfo archive = manager.getPackageArchiveInfo(apk.getAbsolutePath(),
            PackageManager.GET_SIGNING_CERTIFICATES);
        PackageInfo installed = manager.getPackageInfo(context.getPackageName(),
            PackageManager.GET_SIGNING_CERTIFICATES);
        if (archive == null || archive.signingInfo == null || installed.signingInfo == null) {
            throw new IOException("Could not verify the APK signing certificate.");
        }
        UpdatePolicy.validateArchive(context.getPackageName(), archive.packageName,
            expectedVersion, archive.getLongVersionCode());
        Set<String> expected = signatures(installed.signingInfo.getApkContentsSigners());
        Set<String> actual = signatures(archive.signingInfo.getApkContentsSigners());
        if (expected.isEmpty() || !expected.equals(actual)) {
            throw new IOException("The update signing key does not match this app.");
        }
    }

    private static Set<String> signatures(Signature[] signatures) {
        Set<String> result = new HashSet<>();
        if (signatures != null) {
            for (Signature signature : signatures) result.add(signature.toCharsString());
        }
        return result;
    }

    static boolean install(Context context, File apk) {
        PackageManager manager = context.getPackageManager();
        if (!manager.canRequestPackageInstalls()) {
            Intent permission = new Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES,
                Uri.parse("package:" + context.getPackageName()));
            context.startActivity(permission);
            return false;
        }
        Uri uri = Uri.parse("content://" + context.getPackageName() +
                            ".updates/update.apk");
        Intent install = new Intent(Intent.ACTION_VIEW);
        install.setDataAndType(uri, "application/vnd.android.package-archive");
        install.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
        context.startActivity(install);
        return true;
    }

    private static byte[] downloadBytes(String url, int limit) throws IOException {
        HttpURLConnection connection = open(url);
        try (BufferedInputStream input = new BufferedInputStream(connection.getInputStream());
             ByteArrayOutputStream output = new ByteArrayOutputStream()) {
            byte[] buffer = new byte[8192];
            for (;;) {
                int read = input.read(buffer);
                if (read < 0) break;
                if (output.size() + read > limit) {
                    throw new IOException("The update manifest is unexpectedly large.");
                }
                output.write(buffer, 0, read);
            }
            return output.toByteArray();
        } finally {
            connection.disconnect();
        }
    }

    private static HttpURLConnection open(String address) throws IOException {
        HttpURLConnection connection = (HttpURLConnection) new URL(address).openConnection();
        connection.setConnectTimeout(12_000);
        connection.setReadTimeout(30_000);
        connection.setInstanceFollowRedirects(true);
        connection.setUseCaches(false);
        connection.setRequestProperty("Cache-Control", "no-cache");
        connection.setRequestProperty("Accept", "application/json, application/vnd.android.package-archive");
        connection.setRequestProperty("User-Agent", "Skate3-Mobile-Android-Updater");
        int status = connection.getResponseCode();
        if (status < 200 || status >= 300) {
            connection.disconnect();
            throw new IOException("Update server returned HTTP " + status + ".");
        }
        return connection;
    }

    private static MessageDigest sha256() throws IOException {
        try {
            return MessageDigest.getInstance("SHA-256");
        } catch (NoSuchAlgorithmException exception) {
            throw new IOException("SHA-256 is unavailable.", exception);
        }
    }

    private static String hex(byte[] bytes) {
        char[] digits = "0123456789abcdef".toCharArray();
        char[] result = new char[bytes.length * 2];
        for (int i = 0; i < bytes.length; ++i) {
            int value = Byte.toUnsignedInt(bytes[i]);
            result[i * 2] = digits[value >>> 4];
            result[i * 2 + 1] = digits[value & 15];
        }
        return new String(result);
    }
}
