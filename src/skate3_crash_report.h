// Guest fault reporter.
//
// A SIGSEGV on a guest thread currently produces NO output at all: the
// runtime's exception chain declines it (the MMIO handler only reports write
// faults, and guest threads are armed for read recovery only inside
// native-scene entry points), then DispatchUnhandledSignal re-raises with the
// default disposition and the process dies silently. Three sessions of log
// reading found nothing because there was nothing to find.
//
// EnsureInstalled registers a terminal handler that describes the fault -
// faulting host and GUEST address, read vs write, host pc, thread name, and
// the full guest register file - and then DECLINES it, so the process still
// dies exactly as before. It diagnoses; it does not recover.
//
// Must be installed AFTER the runtime's own handlers (MMIO write-watch
// recovery, GuestTryCopy's siglongjmp guard, the raw-load read recovery) so
// those keep first refusal on the faults they legitimately handle; the chain
// runs handlers in registration order. Call it from the same place
// ArmGuestReadRecoveryForThread is called - by then everything else is up.
// Idempotent and cheap.

#ifndef SKATE3_CRASH_REPORT_H_
#define SKATE3_CRASH_REPORT_H_

#include <cstdint>

namespace skate3::crash_report {

#if defined(_WIN32)
inline void EnsureInstalled(uint8_t* /*guest_base*/) {}
inline void StartWatchdogEarly() {}
inline void Heartbeat() {}
inline void NoteGuestWork(uint64_t /*submitted*/) {}
#else
// guest_base is the runtime's virtual membase, used to render the faulting
// host address as a guest address (the only form that means anything when
// reading recompiled code).
void EnsureInstalled(uint8_t* guest_base);

/// Start ONLY the hang watchdog, before the guest has run.
///
/// EnsureInstalled hangs off the guest's first D3D Swap so that its fault
/// handler lands last on the chain - which means a boot that never reaches a
/// Swap gets no watchdog and no thread dump at all. That is exactly the shape
/// of the "guest resumed but never executed" freeze, so the watchdog is
/// started separately here. Deliberately does NOT touch the exception or
/// SIGABRT handlers; EnsureInstalled still owns those and remains idempotent.
void StartWatchdogEarly();

// Called at the guest frame boundary. When it stops advancing for
// skate3_hang_watchdog_seconds, every thread's stack is dumped - a freeze
// raises no signal, so this is the only way to see one from the inside.
void Heartbeat();

// Called at the guest frame boundary with a monotonically rising count of the
// work the guest has SUBMITTED (draws and 2D draws). A guest can go on
// presenting frames at full rate while submitting nothing at all - that is
// what a deadlock between two guest threads looks like from here, and the
// frame heartbeat above cannot see it because the frames never stopped. When
// this count stops moving while frames keep arriving, the same thread dump
// fires.
void NoteGuestWork(uint64_t submitted);
#endif

}  // namespace skate3::crash_report

#endif  // SKATE3_CRASH_REPORT_H_
