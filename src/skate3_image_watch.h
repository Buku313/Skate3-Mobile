// Static-image write watch.
//
// Three QCS8550 handhelds never reach the world because thirty-two bytes of
// STATIC image data - the codec tag table at guest 0x8210A310 - hold English
// text by the time the first audio decoder is created. Every input file on
// those devices is byte-identical to a working phone's, the loaded image is
// never verified after decompression, and the runtime keeps every image page
// writable; nothing in the recompiled code stores to that address. So this
// module makes the image's read-only pages actually read-only after load, and
// the first write to each such page is caught by the process's own fault
// handler, recorded with the writer's host pc and guest call chain, and then
// allowed through. It also keeps a copy of those pages as they were right
// after load, diffs it against the live image every heartbeat, and can put a
// damaged range back.
//
// Cost when nothing writes: zero. Each protected page traps at most once.
#pragma once

#include <cstdint>

namespace skate3::image_watch {

// Snapshot the static image, protect its pages and register the fault
// handler. Call once, after the base image and its title-update patch are
// applied and before any guest code runs (Skate3BaseApp::OnPostSetup).
void Install();

// True after a successful Install().
bool Installed();

// Drain trap records to the log and re-arm the hot pages. Called from the
// guest's Swap hook every frame, on the render thread.
void FlushPending();

// Integrity diff against the load-time snapshot, every N seconds of frames.
// Called from the 30 s heartbeat with the frame counter.
void Tick(uint64_t frames);

// A rate-limited diff with a reason, for the codec-miss hook.
void DiffNow(const char* why);

// Put [guest, guest + len) back to the bytes it held right after load. Returns
// the number of bytes that differed and were restored; 0 when nothing
// differed, the range is outside the snapshot, or the watch is not installed.
uint32_t RestoreFromSnapshot(uint32_t guest, uint32_t len);

}  // namespace skate3::image_watch
