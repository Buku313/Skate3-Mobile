# Mod submission launcher checks

Checked on 2026-09-16, after the v2.1.0 release. This launcher change is not
included in the published v2.1.0 APKs or update feeds.

- `assembleReview lintReview --offline`: passed. No lint errors; 61 warnings.
  The new warning is the landscape orientation restriction, matching the
  existing launcher. Other warnings are pre-existing.
- `run_checks.py`: all nine host suites passed, including public submission
  URLs, every new native page string in English and Brazilian Portuguese,
  explicit language overrides, and private activity wiring.
- Both GitHub issue forms parsed as YAML, with unique field IDs and required
  permission checks.
- `check-apks.py`: the review APK retains the same five ARM64 native libraries
  as the published release, QA, and development builds. ELF alignment and
  bundled mod checks passed; no retail game files were found in the APKs.
- Browser checks: English/Portuguese submission page, language switching,
  correct issue-template links, and a 390-pixel-wide Portuguese layout passed.
  The main page's Portuguese download button now wraps without horizontal
  overflow at that width.

No Android device was connected. These checks do not establish on-device
button behavior, browser return behavior, controller focus, or rendering of
the new native page. Check those on a device before publishing a new APK.
