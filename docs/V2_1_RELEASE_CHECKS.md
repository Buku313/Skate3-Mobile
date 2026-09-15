# v2.1 release checks

September 15, 2026. RG406V release smoke test passed; see coverage limits below.

## Source

Based on the merged [PR #123](https://github.com/Buku313/Skate3-Mobile/pull/123),
app `d9b73f230fad8dfe21408195d3febd4520602e76` and runtime
`8dd8b369ba2770259776472b981bcecd0dcc11f0`.
Andrew Nakas and contributors developed the underlying compatibility work.
AlanConstantino integrated it and tested on Retroid Pocket 6.

The original AV3 source manifest is kept as a historical provenance record.
These release-hardening changes are additional to that snapshot:

- Unknown, non-null guest calls stop by default instead of returning stale
  registers. Existing exact-call-site recoveries are unchanged. The diagnostic
  opt-out remains available for deliberate debugging, not recommended use.
- Failed GPU completion waits stop the process before destructive swapchain or
  fence cleanup. This is failure containment, not recovery from a hung driver.
  Healthy rendering and cleanup paths retain their existing behavior.
- Stable, QA, and developer packages use separate update manifests. Downloaded
  APKs must match the package, version, checksum, and installed signing key.
- The website and README credit the contribution in English and Portuguese.

The previous local, unpublished v2.1 audio/runtime experiments are not merged
over the contributor's working implementation. The original checkout and its
uncommitted changes remain untouched.

## Automated checks

Run with a C++20 compiler, Python 3, JDK 17, and initialized submodules:

```sh
python3 tests/rp6/run_checks.py
```

All eight suites passed: startup, audio, shader layouts, file I/O, guest guards,
CPU texture decoding, release failure paths, and Java update policy. Failure
tests extract actual production methods and use mocked GPU completion with
ASAN/UBSAN. Both failed-wait and successful/idempotent cleanup are covered.
They do not simulate a physical GPU, prove absence of races, or replace device
testing. The audio guard still has a bounded lock-attempt fallback and needs
concurrent stress testing. Presentation-completion lifetime handling beyond
the tested submission-timeout paths remains a separate review area.

ARM64 native Release compilation and stable/QA/developer/review APK assembly
passed. Release and QA lint each report 60 warnings and zero errors. APK
signatures and 16 KB ZIP alignment passed. This does not establish gameplay
compatibility on a 16 KB-page device.

`python3 tests/rp6/check-apks.py` verifies that all four APKs contain the same
five native libraries, each ARM64 with at least 16 KB ELF LOAD alignment, and
the bundled mod. It also checks for common retail game-file types in assets.

The stable signing certificate matches v2.0.17. QA and developer builds retain
the existing debug certificate; they are testing packages, not substitutes for
the main signed release. No signing secrets or retail game assets are included.

## Updating

Main-app users can use the existing in-app updater and approve installation in
Android. Older QA/developer builds shared the main app's manifest URL, so those
users must install the matching v2.1 APK once, without uninstalling. New builds
then remain on their own channel. APK updates do not replace extracted game
files. Separate app packages have separate settings and saves.

## Device coverage

The user confirmed successful RG406V gameplay on the original PR review build.
The hardened candidate was tested in a separate app with a copy of game data.
Existing stable/developer apps and saves were not replaced or cleared.

On RG406V, Android 13 with the Mali system driver and saved Quality profile,
the candidate passed startup, the intro/title flow, native world rendering
with the original skater, touch Start input, and a three-second Home/resume
cycle. Resume retained the same process and rebuilt its surface successfully.
Audio returned to about 187.5 guest frames per second, with zero output silence
in the following full five-second window. This is queue evidence, not a new
subjective audio-quality or frame-rate measurement. No unknown guest-call fatal
or GPU completion timeout was seen during this smoke test.

The contributor's Retroid Pocket 6 report applies to the original AV3 build,
not this subsequently hardened APK. RP5, AYN Thor, Pocket DS, Samsung, and other
devices still need reports for this exact release. Universal compatibility,
stable 60 FPS, and freedom from regressions are not claimed.
