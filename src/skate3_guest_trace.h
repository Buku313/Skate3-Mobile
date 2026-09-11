#pragma once

// Guest call trace: "which guest functions ran during that window, in order,
// with their arguments".
//
// The generated code is one host function per guest function with no names or
// types, so finding the code behind an observable event (a world starting to
// stream, a menu confirm) means either guessing addresses or watching what
// actually runs. This watches. See src/skate3_guest_trace.cpp for the design.

#include <cstdint>

namespace skate3::guest_trace {

// Starts the arm/dump controller. Cheap and safe to call when the trace is
// switched off - it returns immediately unless --skate3_trace=true.
void Install();

// Arm/dump by hand (the controller does this on milestones; these exist for
// callers that know better, e.g. a hook that has just seen the event).
void Arm(const char* reason);
void Dump(const char* reason);

// ---- symbolization ---------------------------------------------------------
// Both directions of "guest function <-> host code", from the generated
// mapping table. This replaces resolving host frames against a saved `nm`
// dump, which goes stale the moment the binary is relinked.

// Guest address -> the guest function containing it. Returns 0 if no function
// covers the address; *offset (optional) gets the offset from its entry.
uint32_t GuestFunctionAt(uint32_t guest_address, uint32_t* offset = nullptr);

// Host pc (a backtrace frame) -> the guest function whose generated body it is
// in, or 0. Host-side frames (the runtime, the app) resolve to 0.
uint32_t GuestFunctionForHostPc(const void* pc, uint32_t* offset = nullptr);

// Log the calling thread's host backtrace with guest names attached.
void LogHostBacktrace(const char* tag);

// ---- sampling profiler -----------------------------------------------------
// Starts the guest sampler if --skate3_guest_profile=true, otherwise returns
// immediately. See src/skate3_guest_trace.cpp for what it samples and why.
void InstallSampler();

}  // namespace skate3::guest_trace
