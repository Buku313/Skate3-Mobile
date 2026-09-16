package chat.buku.skate3;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.ActivityNotFoundException;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Intent;
import android.content.res.ColorStateList;
import android.graphics.Color;
import android.graphics.Typeface;
import android.net.Uri;
import android.os.Bundle;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

/** Offline submission instructions; the creator reviews and sends files on GitHub. */
public final class ModSubmissionActivity extends Activity {
    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        setTitle(localize("SUBMIT A MOD"));
        ScrollView scroll = new ScrollView(this);
        scroll.setBackgroundColor(Color.rgb(10, 10, 12));
        scroll.setFillViewport(true);
        LinearLayout page = new LinearLayout(this);
        page.setOrientation(LinearLayout.VERTICAL);
        page.setPadding(dp(24), dp(24), dp(24), dp(24));
        scroll.addView(page, new ScrollView.LayoutParams(-1, -2));

        addText(page, "COMMUNITY MODS", 13, Color.rgb(255, 112, 28), true);
        addText(page, "SUBMIT A MOD", 28, Color.WHITE, true);
        addText(page, "Made a character? Send it in for the Mod Store. Character mods are currently supported; every submission is reviewed before publication.", 16, Color.LTGRAY, false);
        addText(page, "What to prepare", 20, Color.WHITE, true);
        addText(page, "1. A ZIP containing base.obj and texture_diffuse.png.\n2. A PNG or JPG preview.\n3. Your creator name, credit link, and redistribution permission.\n4. The app version, tested devices, and any known animation or board-contact problems.", 16, Color.LTGRAY, false);
        addText(page, "Only send work you created or have permission to share. Never attach an ISO, XEX, Title Update, save, or extracted retail game files.", 15, Color.rgb(255, 208, 184), false);
        addText(page, "The form opens on GitHub and requires an account. Submissions are public. You choose the attachments and press Submit on GitHub; this app does not upload files or publish a mod automatically.", 15, Color.LTGRAY, false);

        addButton(page, "OPEN SUBMISSION FORM", true, () -> openUrl(
            ModSubmissionLinks.form(LauncherStrings.isPortuguese(this))));
        addButton(page, "MODEL GUIDE AND SEIYU EXAMPLE", false, () -> openUrl(
            ModSubmissionLinks.guide(LauncherStrings.isPortuguese(this))));
        addButton(page, "BACK TO LAUNCHER", false, this::finish);
        setContentView(scroll);
    }

    private void openUrl(String url) {
        try {
            startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse(url)));
        } catch (ActivityNotFoundException exception) {
            new AlertDialog.Builder(this)
                .setTitle(localize("No browser available"))
                .setMessage(localize("Install a browser or copy this link to open it elsewhere.") + "\n\n" + url)
                .setNegativeButton(localize("Cancel"), null)
                .setPositiveButton(localize("Copy link"), (dialog, which) -> {
                    ClipboardManager clipboard = getSystemService(ClipboardManager.class);
                    if (clipboard != null) {
                        clipboard.setPrimaryClip(ClipData.newPlainText(localize("SUBMIT A MOD"), url));
                        Toast.makeText(this, localize("Link copied."), Toast.LENGTH_SHORT).show();
                    }
                })
                .show();
        }
    }

    private void addText(LinearLayout page, String value, int size, int color, boolean bold) {
        TextView view = new TextView(this);
        view.setText(localize(value));
        view.setTextSize(size);
        view.setTextColor(color);
        view.setLineSpacing(0, 1.12f);
        if (bold) view.setTypeface(Typeface.DEFAULT_BOLD);
        LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(-1, -2);
        params.bottomMargin = dp(16);
        page.addView(view, params);
    }

    private void addButton(LinearLayout page, String label, boolean primary, Runnable action) {
        Button button = new Button(this);
        button.setText(localize(label));
        button.setAllCaps(false);
        button.setTextSize(15);
        button.setTypeface(Typeface.DEFAULT_BOLD);
        button.setTextColor(primary ? Color.BLACK : Color.WHITE);
        button.setBackgroundTintList(ColorStateList.valueOf(
            primary ? Color.rgb(255, 104, 24) : Color.rgb(48, 48, 54)));
        button.setMinHeight(dp(52));
        button.setOnClickListener(view -> action.run());
        LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(-1, -2);
        params.bottomMargin = dp(10);
        page.addView(button, params);
    }

    private int dp(int value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }

    private String localize(String value) {
        return LauncherStrings.text(this, value);
    }
}
