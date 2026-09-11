# Andrew-to-Buku RP6 compatibility integration

The underlying compatibility fixes were developed by **Andrew Nakas
(@andrewnakas) and contributors to [Skate 3 Android](https://github.com/andrewnakas/skate3-android)**.
This contribution adapts them to Buku's app and runtime, corrects integration
issues, and tests the combined result on a 12 GB Retroid Pocket 6. Credit also
remains with Buku313, Alex McHugh's Skate3Recomp, and ReXGlue/Xenia contributors.

## Source provenance and scope

- Andrew engine snapshot: [andrewnakas/SK8-Engine at
  a99aa43aa7d85da9bdd71fe860cd5c291e09ad23](https://github.com/andrewnakas/SK8-Engine/tree/a99aa43aa7d85da9bdd71fe860cd5c291e09ad23).
- Andrew runtime snapshot: [andrewnakas/rexglue-skate3 at
  a5c66d59b10bc1468685efec5ed55d2de0322251](https://github.com/andrewnakas/rexglue-skate3/tree/a5c66d59b10bc1468685efec5ed55d2de0322251).
- Buku app base: `91e4b747ae4fdafa11997d20d20ad23843ba3d3c`.
- Buku runtime base: `004344c6f4e0f265a1492ca292b484bb530f09ff`.

These are inspected source snapshots; byte-for-byte reproduction of Andrew's
published v0.1.19 APK is not claimed. The [difference inventory](RP6_DIFFERENCE_INVENTORY.json)
accounts for 210 file entries compared during the integration. It distinguishes
adapted fixes, exact matches, retained Buku code, and separate features.

The port includes coherent shader/layout/binding changes, texture and scene
corrections, proper Quality-mode behavior, audio decoding and guest guards,
startup/threading/memory/I/O fixes, and supporting lifecycle/input/settings work.
Additional integration fixes restore thread-exit notifications, prevent
BitStream packet-end overreads, join the Vulkan cache writer, and drain final
retired resources.

Buku's launcher, package identity, installation flow, physical/touch input,
aspect support, mods/multiplayer, memory workarounds, and explicit Performance
mode remain. The GPU driver selector, Andrew's additional content/level-picker
features, Apple packaging, and optional nonvolatile guest-memory experiment are
outside this contribution.

## Reviewing the runtime dependency

The app PR is the central review thread. The runtime source changes remain in
their own repository on [AlanConstantino/rexglue-skate3-android,
fix/rp6-audio-rendering](https://github.com/AlanConstantino/rexglue-skate3-android/tree/fix/rp6-audio-rendering).
The exact required commit is recorded by this app branch's
`third_party/rexglue-sdk` Git submodule entry. The `.gitmodules` URL remains
Buku's runtime repository.

Before merging the app PR, the maintainer must bring the runtime commits into
their runtime repository. If those commits are rebased or squashed, update the
app submodule to the resulting commit and repeat combined verification.
Merging the app alone does not import runtime source changes into the runtime
branch. Review the entire runtime comparison, not just the app's one-line
submodule diff.

For a pre-merge checkout, initialize the app/runtime submodules and explicitly
fetch the contribution runtime branch if the pinned commit is not yet
fetchable from the configured upstream remote:

```sh
git submodule update --init third_party/rexglue-sdk
git -C third_party/rexglue-sdk fetch \
  https://github.com/AlanConstantino/rexglue-skate3-android.git \
  fix/rp6-audio-rendering
runtime_revision=$(git rev-parse HEAD:third_party/rexglue-sdk)
git -C third_party/rexglue-sdk checkout --detach "$runtime_revision"
git submodule update --init --recursive
```

Use the existing [Android build guide](../android/README.md) or
`./build-android.sh` with your own supported game dump and title update. No
retail game inputs, generated guest code, signing keys, or local configuration
are included in this contribution.

## Test record

The original AV3 APK is available as a review prerelease in the
[contribution fork](https://github.com/AlanConstantino/Skate3-Mobile/releases/tag/rp6-av3-review).
It is the existing tested APK, not a newly rebuilt artifact or an official Buku
release. Its source-content record is [RP6_AV3_SOURCE_HASHES.json](RP6_AV3_SOURCE_HASHES.json).
All 975 recorded app/runtime source files still matched the tested build when
the contribution was prepared; subsequent publication work adds tests and
documentation without changing those production files.

- APK: `Skate3-Mobile-RP6-AV3.apk`, 73,017,815 bytes.
- SHA-256: `05872c36b63d4ba739a3e2334bb6bbbe81e7b082e520066db7ff9cdcf3a5a3c1`.
- Test app: `Skate 3 RP6 AV Fix`, `chat.buku.skate3.dev`,
  `2.0.18-rp6-av3-debug` (20032). This test identity is not a production rename.
- Device: 12 GB Retroid Pocket 6. Alan confirmed successful gameplay and
  resolution of the previously observed problems on September 11, 2026.
- Settings: Quality mode, 60 FPS cap, 512-frame audio buffer, draw/LOD scales
  1.0, MSAA 1, SSAO off. The source contribution does not overwrite existing
  user settings with this device's saved configuration.

Android native compilation, APK assembly, lint, signing/manifest/library
checks, and at least 16 KB ELF load alignment passed. The installed APK hash
matched the delivered file, and all eight guest audio hooks were verified as
strong replacements in the packaged ARM64 library.

The [host regression suite](../tests/rp6/README.md) covers six startup
scenarios, 75 shader variants against five production layouts, the reported
16416/16384-bit XMA loop overrun, packet-end reads, all six channel impulses,
audio-guard arguments/returns and contention, and short/interrupted file I/O.
Its publication-ready scripts were rerun successfully with ASAN/UBSAN.

These are component tests plus a user-reported combined device test, not a
claim of measured 60 FPS, exhaustive graphical correctness, all-device
compatibility, or long-session crash freedom. The bounded audio-queue fallback
is still a mitigation rather than proof that every queue race is eliminated.
