#pragma once

// One switch for every instrument that bills the frame path.
//
// The measurement instruments - [pace], the per-window scene breakdown, the
// present cadence line, the Vulkan present breakdown - used to be compiled ON
// for iOS, because the people who can reproduce a frame-pacing report are
// strangers on the internet and the alternative was asking them to hand-edit
// Documents/user/ios_args.txt, where one duplicated key silently discards every
// argument the app was built with.
//
// That reasoning was right about the problem and wrong about the answer. This
// is the answer: one cvar, reachable from the System page of the settings
// overlay, that turns the whole set on at runtime and raises the log level to
// match. A player gets a build that does not narrate its own frame timing to
// flash; somebody chasing a hang flips one switch and sends a log.

namespace skate3 {

// Apply skate3_diagnostics, and keep applying it whenever it changes.
//
// Deliberately asymmetric at startup: switching it ON turns the instruments on,
// but leaving it off does NOT force them off, so an explicit
// --skate3_native_render_scene_perf_log=true on the command line or in
// ios_args.txt still wins. Only an actual toggle turns things back off.
void InstallDiagnosticsSwitch();

}  // namespace skate3
