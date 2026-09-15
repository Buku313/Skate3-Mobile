"""Exercise the production Java update policy without Android or network access."""
import os
from pathlib import Path
import subprocess
from common import b, out

java_home = os.environ.get('JAVA_HOME')
javac = str(Path(java_home) / 'bin/javac') if java_home else 'javac'
java = str(Path(java_home) / 'bin/java') if java_home else 'java'
source = b / 'android/app/src/main/java/chat/buku/skate3/UpdatePolicy.java'
harness = out / 'UpdatePolicyTest.java'
harness.write_text('''package chat.buku.skate3;
import java.io.IOException;
public final class UpdatePolicyTest {
  interface Checked { void run() throws IOException; }
  static void rejected(Checked call) throws IOException {
    try { call.run(); } catch (IOException expected) { return; }
    throw new AssertionError("invalid update was accepted");
  }
  public static void main(String[] args) throws Exception {
    String[] packages = {"chat.buku.skate3", "chat.buku.skate3.qa", "chat.buku.skate3.dev"};
    String[] files = {"update.json", "update-qa.json", "update-dev.json"};
    String url = "https://github.com/Buku313/Skate3-Mobile/releases/download/v2.1.0/test.apk";
    String hash = "a".repeat(64);
    for (int i = 0; i < packages.length; ++i) {
      String current = packages[i];
      assert UpdatePolicy.manifestUrl(current).endsWith("/" + files[i]);
      UpdatePolicy.validateManifest(current, current, 20100, url, hash);
      UpdatePolicy.validateArchive(current, current, 20100, 20100);
      for (String other : packages) if (!other.equals(current)) {
        rejected(() -> UpdatePolicy.validateManifest(current, other, 20100, url, hash));
        rejected(() -> UpdatePolicy.validateArchive(current, other, 20100, 20100));
      }
      rejected(() -> UpdatePolicy.validateArchive(current, current, 20100, 20024));
      rejected(() -> UpdatePolicy.validateManifest(current, current, 0, url, hash));
      rejected(() -> UpdatePolicy.validateManifest(current, current, 20100, url, "bad"));
      for (String bad : new String[]{"http://github.com/Buku313/Skate3-Mobile/releases/download/x/a.apk",
          "https://github.com.evil.test/Buku313/Skate3-Mobile/releases/download/x/a.apk",
          "https://user@github.com/Buku313/Skate3-Mobile/releases/download/x/a.apk",
          "https://github.com/other/repo/releases/download/x/a.apk", "not a URL"}) {
        rejected(() -> UpdatePolicy.validateManifest(current, current, 20100, bad, hash));
      }
    }
    assert UpdatePolicy.manifestUrl("chat.buku.skate3.pr123") == null;
    System.out.println("PASS: three update channels; cross-package, wrong-version, invalid-hash and unsafe-URL rejection; private reviews do not update.");
  }
}
''')
subprocess.run([javac, '-d', str(out), str(source), str(harness)], check=True)
subprocess.run([java, '-ea', '-cp', str(out), 'chat.buku.skate3.UpdatePolicyTest'], check=True)
