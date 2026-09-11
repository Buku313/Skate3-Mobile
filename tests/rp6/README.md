# RP6 app/runtime host regression checks

These checks accompany the Andrew-to-Buku compatibility integration. They read
the current production methods and compile host harnesses around them. They do
not boot the game, emulate the GPU, or establish gameplay performance.

From the app repository root on macOS or Linux:

```sh
python3 tests/rp6/run_checks.py
```

Requirements: Python 3.9+, a C++20 Clang compiler with AddressSanitizer and
UndefinedBehaviorSanitizer, and the matching runtime checkout under
`third_party/rexglue-sdk`. The startup reproduction also reads the pre-fix runtime
commit `004344c6f4e0f265a1492ca292b484bb530f09ff` from Git history; use a full
runtime checkout, or fetch that commit if the checkout is shallow. The shader
check uses the Vulkan headers from the initialized runtime submodules.

Optional environment variables:

- `CXX`: host compiler command; defaults to `clang++`.
- `VULKAN_HEADERS`: include directory containing `vulkan/vulkan.h`.
- `RP6_TEST_OUTPUT`: scratch/output directory; defaults to `out/rp6-checks`.

Each check can also be run individually with `python3 tests/rp6/check-<name>.py`.
Generated harnesses, executables, logs, and JSON results stay in the output
directory. Retail executables, title updates, generated guest code, Android SDK,
FFmpeg, and an attached device are not required for these host checks.

## Coverage and limits

| Check | Coverage | Deliberately mocked or untested |
| --- | --- | --- |
| startup | Reproduces the lost first Resume; exercises six suspension/count scenarios and thread-exit notification | Synchronization adapters force scheduling boundaries; real POSIX signal delivery is stubbed |
| audio | Reproduces the 16416/16384-bit loop overrun; checks split headers, exact-end BitStream reads, and six channel impulses | No actual FFmpeg decode or audio-device playback |
| shaders | Checks all 75 shipped shader variants against five production binding layouts and a too-small device limit | Vulkan calls are mocked; no on-GPU rendering |
| io | Short reads/writes, interruption, EOF, partial errors, bounded retries | POSIX calls return scripted results |
| guest-guards | Preserved guest arguments/returns, balanced unlock, bounded lock contention, null-system bypass | Guest calls are stubbed; does not prove all game queue races are eliminated |

The scripts extract methods by signature so they exercise the current source
rather than a copied implementation. A changed signature or harness adapter can
require a test update. All five checks were rerun successfully from this layout
on macOS with host AddressSanitizer and UndefinedBehaviorSanitizer.

The combined APK still requires device testing. See
[the integration and test record](../../docs/RP6_INTEGRATION.md).
