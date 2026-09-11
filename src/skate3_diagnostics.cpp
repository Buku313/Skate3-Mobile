#include "skate3_diagnostics.h"

#include <string>
#include <string_view>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(skate3_diagnostics, false, "Skate 3",
                    "Turn on the performance instruments: the [pace] delivered-frame "
                    "summary, the per-window scene breakdown, the present-cadence line and "
                    "the Vulkan present breakdown, and raise the log level to info so they "
                    "are actually written. Off for play - the accounting behind these runs "
                    "every frame, not just on the frames that print. Turn it on to produce "
                    "a support log; no rebuild and no config file needed.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace skate3 {
namespace {

// Every instrument the switch owns, and what each is worth on the frame path.
//
// The log level belongs in this list rather than beside it: [pace] and [cp-sum]
// write at INFO, so turning the instruments on without raising the level
// produces a build that does all of the accounting and prints none of it -
// which is the exact combination that costs the most and says the least.
void ApplyDiagnostics(bool on) {
  const char* value = on ? "true" : "false";
  // Per-window scene breakdown. The accounting runs every frame; the line is
  // one per 600.
  rex::cvar::SetFlagByName("skate3_native_render_scene_perf_log", value);
  // Interval statistics on every guest-output refresh.
  rex::cvar::SetFlagByName("presenter_present_cadence_log", value);
  // Ten steady_clock reads per present.
  rex::cvar::SetFlagByName("vulkan_present_timing_log", value);
  rex::cvar::SetFlagByName("log_level", on ? "info" : "warn");
  REXLOG_WARN("Skate 3 diagnostics {}", on ? "ON - instruments running, log level info"
                                           : "off - instruments stopped, log level warn");
}

}  // namespace

void InstallDiagnosticsSwitch() {
  static bool installed = false;
  if (installed) {
    return;
  }
  installed = true;

  rex::cvar::RegisterChangeCallback("skate3_diagnostics",
                                    [](std::string_view, std::string_view value) {
                                      ApplyDiagnostics(value == "true" || value == "1");
                                    });

  // Startup is one-directional on purpose. Applying `false` here would clobber
  // an explicit --presenter_present_cadence_log=true from the command line or
  // ios_args.txt, and silently overriding a flag the operator named by hand is
  // how a whole measurement sweep once got invalidated on this codebase.
  if (REXCVAR_GET(skate3_diagnostics)) {
    ApplyDiagnostics(true);
  }
}

}  // namespace skate3
