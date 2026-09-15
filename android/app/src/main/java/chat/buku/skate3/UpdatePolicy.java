package chat.buku.skate3;

import java.io.IOException;
import java.net.URI;

/** Package identities are independent update channels, never interchangeable. */
final class UpdatePolicy {
    private static final String SITE = "https://buku313.github.io/Skate3-Mobile/";

    private UpdatePolicy() {}

    static String manifestUrl(String packageName) {
        switch (packageName) {
            case "chat.buku.skate3": return SITE + "update.json";
            case "chat.buku.skate3.qa": return SITE + "update-qa.json";
            case "chat.buku.skate3.dev": return SITE + "update-dev.json";
            default: return null; // Private review builds do not install public packages.
        }
    }

    static void validateManifest(String installedPackage, String updatePackage,
                                 long versionCode, String apkUrl, String sha256)
            throws IOException {
        if (manifestUrl(installedPackage) == null || !installedPackage.equals(updatePackage)
                || versionCode <= 0 || !sha256.matches("[0-9a-f]{64}")) {
            throw new IOException("The update manifest does not match this app.");
        }
        try {
            URI uri = new URI(apkUrl);
            if (!"https".equals(uri.getScheme()) || !"github.com".equals(uri.getHost())
                    || uri.getUserInfo() != null || uri.getPort() != -1
                    || !uri.getPath().startsWith("/Buku313/Skate3-Mobile/releases/download/")) {
                throw new IOException("The update download address is invalid.");
            }
        } catch (java.net.URISyntaxException exception) {
            throw new IOException("The update download address is invalid.", exception);
        }
    }

    static void validateArchive(String installedPackage, String archivePackage,
                                long expectedVersion, long archiveVersion) throws IOException {
        if (!installedPackage.equals(archivePackage) || expectedVersion != archiveVersion) {
            throw new IOException("The downloaded APK does not match this app and update version.");
        }
    }
}
