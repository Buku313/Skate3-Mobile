"""Host checks for submission links, launcher language coverage and private activity wiring."""
import os
from pathlib import Path
import re
import subprocess
import xml.etree.ElementTree as ET
from common import b, out

source = b / 'android/app/src/main/java/chat/buku/skate3'
page = (source / 'ModSubmissionActivity.java').read_text()
strings = set(re.findall(r'(?:addText\(page, |addButton\(page, |localize\()'
                         r'("(?:[^"\\]|\\.)*")', page))
assert strings
java_dir = out / 'mod-submission'
stub = java_dir / 'android/content'
stub.mkdir(parents=True, exist_ok=True)
(stub / 'Context.java').write_text('''package android.content;
public class Context {
  public static final int MODE_PRIVATE = 0;
  private final SharedPreferences preferences = new SharedPreferences();
  public SharedPreferences getSharedPreferences(String name, int mode) { return preferences; }
}''')
(stub / 'SharedPreferences.java').write_text('''package android.content;
public class SharedPreferences {
  private String value;
  public String getString(String key, String fallback) { return value == null ? fallback : value; }
  public Editor edit() { return new Editor(); }
  public class Editor {
    public Editor putString(String key, String updated) { value = updated; return this; }
    public void apply() {}
  }
}''')
harness = java_dir / 'ModSubmissionTest.java'
harness.write_text('''package chat.buku.skate3;
import android.content.Context;
import java.net.URI;
import java.util.Locale;
public class ModSubmissionTest {
  public static void main(String[] args) throws Exception {
    Locale original = Locale.getDefault();
    try {
      Locale.setDefault(Locale.forLanguageTag("pt-BR"));
      Context context = new Context();
      assert LauncherStrings.isPortuguese(context);
      String[] labels = new String[]{''' + ','.join(sorted(strings)) + '''};
      for (String label : labels) {
        String pt = LauncherStrings.text(context, label);
        assert !pt.isBlank() && !pt.equals(label) : "Missing Portuguese: " + label;
      }
      LauncherStrings.setLanguage(context, "en");
      for (String label : labels) assert label.equals(LauncherStrings.text(context, label));
      assert !LauncherStrings.isPortuguese(context);
      LauncherStrings.setLanguage(context, "pt-BR");
      assert LauncherStrings.isPortuguese(context);
      for (boolean pt : new boolean[]{false, true}) {
        URI form = new URI(ModSubmissionLinks.form(pt));
        URI guide = new URI(ModSubmissionLinks.guide(pt));
        assert form.getScheme().equals("https") && form.getHost().equals("github.com");
        assert form.getPath().equals("/Buku313/Skate3-Mobile/issues/new");
        assert form.getQuery().equals("template=character_mod_submission" + (pt ? "_pt_br" : "") + ".yml");
        assert guide.getScheme().equals("https") && guide.getHost().equals("buku313.github.io");
        assert guide.getPath().equals("/Skate3-Mobile/mods/submit.html");
        assert guide.getQuery().equals("lang=" + (pt ? "pt-BR" : "en"));
      }
    } finally { Locale.setDefault(original); }
    System.out.println("PASS: public submission links and every native page string in English/Portuguese.");
  }
}''')
java_home = os.environ.get('JAVA_HOME')
java_bin = Path(java_home) / 'bin' if java_home else None
subprocess.run([str(java_bin / 'javac') if java_bin else 'javac', '-d', str(java_dir),
                str(stub / 'Context.java'), str(stub / 'SharedPreferences.java'),
                str(source / 'LauncherStrings.java'), str(source / 'ModSubmissionLinks.java'),
                str(harness)], check=True)
subprocess.run([str(java_bin / 'java') if java_bin else 'java', '-ea', '-cp', str(java_dir),
                'chat.buku.skate3.ModSubmissionTest'], check=True)
android = '{http://schemas.android.com/apk/res/android}'
manifest = ET.parse(b / 'android/app/src/main/AndroidManifest.xml')
activities = [a for a in manifest.findall('./application/activity')
              if a.get(android + 'name') == 'chat.buku.skate3.ModSubmissionActivity']
assert len(activities) == 1 and activities[0].get(android + 'exported') == 'false'
for template in ['character_mod_submission.yml', 'character_mod_submission_pt_br.yml']:
    assert (b / '.github/ISSUE_TEMPLATE' / template).is_file(), template
launcher = (source / 'LauncherActivity.java').read_text()
assert 'submitModButton.setEnabled(enabled)' in launcher
assert 'hide(submitModButton)' not in launcher
assert 'dismissModStoreList();\n        startActivity(new Intent(this, ModSubmissionActivity.class));' in launcher
print('PASS: private activity, existing form templates, pre-setup availability and busy-state guard.')
