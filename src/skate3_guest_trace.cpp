// Guest call trace - see the header for what it is for.
//
// The problem it solves: the game's own logic is machine-translated PowerPC,
// one host function per guest function, named `sub_XXXXXXXX` and nothing else.
// To find the code behind an observable event you either guess addresses or
// watch what actually runs. Backtracing from the observable does not always
// work either - the world files are opened on the file thread, seconds after
// and one queue hop away from whatever asked for the world.
//
// So this records function ENTRIES, in two modes:
//
//   first  (default) the first call of each guest function since the trace was
//          armed. Bounded by the number of distinct functions, so it spans a
//          whole multi-second transition, and "what ran that had not run
//          before" is exactly the question worth asking about a state change.
//   ring   every call, into a circular buffer. Exact call order and repeat
//          counts for a short window, once `first` has said where to look.
//
// The recording hook is a local edit to generated/skate3_init.h (which is
// gitignored and regenerated away by codegen - the same arrangement as the
// heap store watch).
//
// IT IS COMPILED OUT BY DEFAULT. Disarmed it still cost two loads of a hot
// global and a not-taken branch at every one of 47,889 guest function entries,
// on every call, in every shipping frame - for a debugging aid that is off.
// The edit now wraps the macro in `#if SKATE3_GUEST_TRACE`, with an empty
// definition otherwise, so --skate3_trace does nothing unless the build sets
// -DSKATE3_GUEST_TRACE=1. Reapply it in that form:
//
//   extern "C" uint32_t g_skate3_trace_gen;   // 0 = off, bumped on each arm
//   extern "C" uint32_t g_skate3_trace_all;   // ring mode
//   extern "C" void skate3_trace_enter(const char* fn, const PPCContext& ctx,
//                                      uint8_t* base);
//
//   #define REX_FUNC_PROLOGUE()                                              \
//     do {                                                                   \
//       __builtin_assume(((size_t)base & 0x1F) == 0);                        \
//       static uint32_t _skate3_seen = 0;                                    \
//       const uint32_t _gen = g_skate3_trace_gen;                            \
//       if (__builtin_expect((_gen != _skate3_seen) | g_skate3_trace_all,    \
//                            0)) {                                           \
//         _skate3_seen = _gen;                                               \
//         if (_gen | g_skate3_trace_all)                                     \
//           skate3_trace_enter(__func__, ctx, base);                         \
//       }                                                                    \
//     } while (0)
//
// wrapped as:
//
//   #if SKATE3_GUEST_TRACE
//   ... the definition above ...
//   #else
//   #define REX_TRACE_ENTER() ((void)0)
//   #endif
//
// The per-function `static uint32_t` is what makes `first` mode cheap: no set,
// no hashing, one word per function. A generation counter rather than a bool
// so re-arming does not have to clear 100k flags. The race on it is benign -
// the worst case is one duplicate record.

#include "skate3_guest_trace.h"

#if defined(__linux__)
// For the guest sampling profiler's Linux path: thread discovery through
// /proc/self/task and pc capture from a signal handler.
#include <numeric>
#include <dirent.h>
#include <dlfcn.h>
#include <fstream>
#include <signal.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>
#endif

#include "generated/skate3_init.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/thread_act.h>
#include <mach/thread_status.h>
#include <pthread.h>
#endif

#if defined(__linux__)
#if defined(__ANDROID__)
// bionic has no <execinfo.h>; this supplies backtrace* over the unwinder.
#include <rex/execinfo_android.h>
#else
#include <execinfo.h>
#endif
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

#include <rex/cvar.h>
#include <rex/kernel/guest_presence.h>
#include <rex/logging.h>
#include <rex/logging/api.h>
#include <rex/ppc/context.h>

#include "skate3_demo_path.h"

// The two globals the generated prologue tests, and the recorder it calls.
// Defined here, declared by the local edit in generated/skate3_init.h.
extern "C" {
uint32_t g_skate3_trace_gen = 0;
uint32_t g_skate3_trace_all = 0;
void skate3_trace_enter(const char* fn, const PPCContext& ctx, uint8_t* base);
}

REXCVAR_DEFINE_BOOL(skate3_trace, false, "Skate 3",
                    "Record guest function entries during a window and write them to "
                    "<log_file>.trace. Off costs two global loads per guest call.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(skate3_trace_mode, "first", "Skate 3",
                      "Trace mode: 'first' = the first call of each guest function since "
                      "arming (bounded, spans seconds); 'ring' = every call into a "
                      "circular buffer (exact order, short window).");
REXCVAR_DEFINE_INT32(skate3_trace_capacity, 262144, "Skate 3",
                     "Trace entries to reserve (24 bytes each).")
    .range(1024, 16777216);
REXCVAR_DEFINE_STRING(skate3_trace_arm, "macro-final", "Skate 3",
                      "When to arm and dump: 'macro-final' = arm before the boot macro's "
                      "last input, dump after the sequence completes; 'boot' = arm at "
                      "startup, dump once gameplay is reached; 'gameplay' = arm when "
                      "gameplay is reached; 'manual' = arm at startup, never auto-dump.");
REXCVAR_DEFINE_INT32(skate3_trace_dump_delay_ms, 6000, "Skate 3",
                     "Keep recording this long after the dump trigger before writing the "
                     "trace out (the world starts streaming seconds after the confirm).")
    .range(0, 120000);

namespace skate3::guest_trace {
namespace {

// A world is named by a string ("DIST_sk8itbareclona"), so the argument that
// carries the identity is a POINTER, and comparing pointer values between two
// runs says nothing. Resolving them at DUMP time does not work either - the
// dump is seconds later and the memory has moved on. So each pointer-shaped
// argument is resolved to text as it is recorded, and the world name then
// appears in the trace right next to the function that received it.
constexpr size_t kStringPreview = 28;

struct Entry {
  const char* fn;   // "__imp__sub_82990768" - a static string in the generated code
  uint32_t r3;
  uint32_t r4;
  uint32_t r5;
  uint64_t stamp;
  uint32_t thread;
  // Text at r3/r4/r5 when they point at one, empty otherwise.
  char text[3][kStringPreview];
};

Entry* g_entries = nullptr;
size_t g_capacity = 0;
bool g_ring = false;
std::atomic<uint64_t> g_next{0};
std::atomic<bool> g_dumped{false};
std::atomic<int> g_full_warned{0};
uint32_t g_generation = 0;

std::mutex g_threads_mutex;
std::vector<std::string> g_threads;
thread_local int tl_thread = -1;

inline uint64_t Stamp() {
#if defined(__clang__) && (defined(__x86_64__) || defined(__aarch64__))
  return __builtin_readcyclecounter();
#elif defined(__x86_64__)
  return __builtin_ia32_rdtsc();
#else
  return uint64_t(std::chrono::steady_clock::now().time_since_epoch().count());
#endif
}

uint64_t NowMs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}

// Interned once per thread; the index goes in every entry so a dump can be
// filtered down to the thread that matters (the transition runs on the main
// thread, while streaming and audio produce most of the volume).
int ThreadIndex() {
  if (tl_thread >= 0) {
    return tl_thread;
  }
  char name[20] = {0};
#if defined(__linux__)
  if (prctl(PR_GET_NAME, reinterpret_cast<unsigned long>(name), 0, 0, 0) != 0) {
    name[0] = 0;
  }
  const long tid = syscall(SYS_gettid);
#else
  const long tid = 0;
#endif
  char label[48];
  std::snprintf(label, sizeof(label), "%s/%ld", name[0] ? name : "?", tid);
  std::lock_guard<std::mutex> lock(g_threads_mutex);
  g_threads.emplace_back(label);
  tl_thread = static_cast<int>(g_threads.size() - 1);
  return tl_thread;
}

std::string ThreadName(uint32_t index) {
  std::lock_guard<std::mutex> lock(g_threads_mutex);
  return index < g_threads.size() ? g_threads[index] : std::string("?");
}

// Guest regions worth dereferencing. Deliberately NOT the whole 4 GiB view:
// the runtime arms fault recovery over parts of it, and a speculative read of
// an arbitrary address is how a diagnostic turns into the crash it was meant
// to explain. These three cover the heap, the low heap and the XEX image,
// which is where every string lives (same regions as scripts/memfind.py).
bool ReadableGuest(uint32_t address) {
  return (address >= 0x00010000 && address < 0x40000000) ||
         (address >= 0x40000000 && address < 0x80000000) ||
         (address >= 0x82000000 && address < 0x84000000);
}

// Text at `address`, or "" if it does not look like a string. Requires the
// whole preview to be printable and NUL-terminated within it, so a struct that
// happens to start with a letter does not read as text.
void ReadGuestString(const uint8_t* base, uint32_t address, char* out, size_t cap) {
  out[0] = 0;
  if (base == nullptr || !ReadableGuest(address)) {
    return;
  }
  const char* src = reinterpret_cast<const char*>(base + address);
  size_t n = 0;
  while (n < cap - 1) {
    const char c = src[n];
    if (c == 0) {
      break;
    }
    if (c < 0x20 || c > 0x7E) {
      return;  // not text
    }
    ++n;
  }
  // One or two printable bytes is noise, not a name.
  if (n < 4) {
    return;
  }
  std::memcpy(out, src, n);
  out[n] = 0;
}

// "__imp__sub_82990768" / "sub_82990768" -> 0x82990768, or 0.
uint32_t GuestAddressOfName(const char* fn) {
  if (fn == nullptr) {
    return 0;
  }
  const char* p = std::strstr(fn, "sub_");
  if (p == nullptr) {
    return 0;
  }
  return uint32_t(std::strtoul(p + 4, nullptr, 16));
}

// ---- symbolization ---------------------------------------------------------

struct HostEntry {
  uintptr_t host;
  uint32_t guest;
};

std::vector<uint32_t> g_guest_sorted;
std::vector<HostEntry> g_host_sorted;
std::once_flag g_tables_once;

// A generated function body is large but not unbounded; refuse to attribute a
// host pc further than this past a function entry, so runtime and app frames
// resolve to "not guest" instead of to a plausible-looking wrong name.
constexpr uintptr_t kMaxFunctionBytes = 256 * 1024;

void BuildTables() {
  const PPCFuncMapping* map = skate3_PPCImageConfig.func_mappings;
  if (map == nullptr) {
    return;
  }
  size_t count = 0;
  while (map[count].guest != 0) {
    ++count;
  }
  g_guest_sorted.reserve(count);
  g_host_sorted.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    g_guest_sorted.push_back(uint32_t(map[i].guest));
    if (map[i].host != nullptr) {
      g_host_sorted.push_back({reinterpret_cast<uintptr_t>(map[i].host), uint32_t(map[i].guest)});
    }
  }
  std::sort(g_guest_sorted.begin(), g_guest_sorted.end());
  std::sort(g_host_sorted.begin(), g_host_sorted.end(),
            [](const HostEntry& a, const HostEntry& b) { return a.host < b.host; });
  REXLOG_INFO("skate3 trace: symbol table built - {} guest functions", count);
}

void EnsureTables() { std::call_once(g_tables_once, BuildTables); }

// ---- dump ------------------------------------------------------------------

std::string TracePath() {
  const std::string& log_file = REXCVAR_GET(log_file);
  return log_file.empty() ? std::string("skate3.trace") : log_file + ".trace";
}

}  // namespace

uint32_t GuestFunctionAt(uint32_t guest_address, uint32_t* offset) {
  EnsureTables();
  if (g_guest_sorted.empty()) {
    return 0;
  }
  auto it = std::upper_bound(g_guest_sorted.begin(), g_guest_sorted.end(), guest_address);
  if (it == g_guest_sorted.begin()) {
    return 0;
  }
  --it;
  if (guest_address - *it > kMaxFunctionBytes) {
    return 0;
  }
  if (offset != nullptr) {
    *offset = guest_address - *it;
  }
  return *it;
}

uint32_t GuestFunctionForHostPc(const void* pc, uint32_t* offset) {
  EnsureTables();
  if (g_host_sorted.empty()) {
    return 0;
  }
  const uintptr_t addr = reinterpret_cast<uintptr_t>(pc);
  auto it = std::upper_bound(g_host_sorted.begin(), g_host_sorted.end(), addr,
                             [](uintptr_t value, const HostEntry& e) { return value < e.host; });
  if (it == g_host_sorted.begin()) {
    return 0;
  }
  --it;
  if (addr - it->host > kMaxFunctionBytes) {
    return 0;
  }
  if (offset != nullptr) {
    *offset = uint32_t(addr - it->host);
  }
  return it->guest;
}

void LogHostBacktrace(const char* tag) {
#if defined(__linux__)
  void* frames[24];
  const int n = ::backtrace(frames, 24);
  for (int i = 0; i < n; ++i) {
    uint32_t off = 0;
    const uint32_t guest = GuestFunctionForHostPc(frames[i], &off);
    if (guest != 0) {
      REXLOG_INFO("skate3 trace: {} #{} sub_{:08X}+0x{:X}", tag, i, guest, off);
    } else {
      REXLOG_INFO("skate3 trace: {} #{} host {}", tag, i, frames[i]);
    }
  }
#else
  (void)tag;
#endif
}

void Arm(const char* reason) {
  if (g_entries == nullptr) {
    return;
  }
  g_next.store(0, std::memory_order_relaxed);
  g_dumped.store(false, std::memory_order_relaxed);
  ++g_generation;
  if (g_ring) {
    __atomic_store_n(&g_skate3_trace_all, 1u, __ATOMIC_RELEASE);
  }
  // Published last: this is what the prologue tests.
  __atomic_store_n(&g_skate3_trace_gen, g_generation, __ATOMIC_RELEASE);
  REXLOG_INFO("skate3 trace: ARMED ({} mode, {} entries) - {}", g_ring ? "ring" : "first",
              g_capacity, reason);
}

void Dump(const char* reason) {
  if (g_entries == nullptr || g_dumped.exchange(true, std::memory_order_relaxed)) {
    return;
  }
  __atomic_store_n(&g_skate3_trace_gen, 0u, __ATOMIC_RELEASE);
  __atomic_store_n(&g_skate3_trace_all, 0u, __ATOMIC_RELEASE);
  // Let any recorder already inside skate3_trace_enter finish its stores.
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  const uint64_t produced = g_next.load(std::memory_order_relaxed);
  const uint64_t first = (g_ring && produced > g_capacity) ? produced - g_capacity : 0;
  const uint64_t last = g_ring ? produced : std::min<uint64_t>(produced, g_capacity);

  const std::string path = TracePath();
  std::FILE* out = std::fopen(path.c_str(), "w");
  if (out == nullptr) {
    REXLOG_ERROR("skate3 trace: cannot write '{}'", path);
    return;
  }
  std::fprintf(out,
               "# skate3 guest trace: mode=%s reason=%s recorded=%llu written=%llu "
               "capacity=%zu%s\n",
               g_ring ? "ring" : "first", reason, static_cast<unsigned long long>(produced),
               static_cast<unsigned long long>(last - first), g_capacity,
               (!g_ring && produced > g_capacity) ? " TRUNCATED" : "");
  std::fprintf(out, "# seq\tdcycles\tthread\tfunction\tr3\tr4\tr5\ttext\n");

  const uint64_t base_stamp = (last > first) ? g_entries[first % g_capacity].stamp : 0;
  for (uint64_t i = first; i < last; ++i) {
    const Entry& e = g_entries[i % g_capacity];
    if (e.fn == nullptr) {
      continue;
    }
    // Most entries are `sub_XXXXXXXX`, but the recompiler also emits named
    // helpers (__savegprlr_26 and friends, called by nearly every function).
    // Printing those as sub_00000000 would read as a real address.
    char name[64];
    const uint32_t guest = GuestAddressOfName(e.fn);
    if (guest != 0) {
      std::snprintf(name, sizeof(name), "sub_%08X", guest);
    } else {
      const char* raw = e.fn;
      if (std::strncmp(raw, "__imp__", 7) == 0) {
        raw += 7;
      }
      std::snprintf(name, sizeof(name), "%s", raw);
    }
    // Strings go in one trailing column, tagged with the register they came
    // from, so the format stays one line per entry and greppable.
    char text[3 * (kStringPreview + 8)] = {0};
    size_t used = 0;
    for (int r = 0; r < 3; ++r) {
      if (e.text[r][0] == 0) {
        continue;
      }
      used += size_t(std::snprintf(text + used, sizeof(text) - used, "%sr%d=\"%s\"",
                                   used ? " " : "", r + 3, e.text[r]));
      if (used >= sizeof(text)) {
        break;
      }
    }
    std::fprintf(out, "%llu\t%lld\t%s\t%s\t%08X\t%08X\t%08X\t%s\n",
                 static_cast<unsigned long long>(i - first),
                 static_cast<long long>(e.stamp - base_stamp), ThreadName(e.thread).c_str(), name,
                 e.r3, e.r4, e.r5, text);
  }
  std::fclose(out);
  REXLOG_INFO("skate3 trace: DUMPED {} entries to '{}' - {}", last - first, path, reason);
  rex::FlushLogging();
}

namespace {

// The controller runs on its own host thread and only ever reads milestones
// that already exist: macro progress from the demo path, and the guest's own
// presence context. Nothing here touches guest memory.
void ControllerMain() {
  const std::string mode = REXCVAR_GET(skate3_trace_arm);
  const uint64_t delay_ms = uint64_t(REXCVAR_GET(skate3_trace_dump_delay_ms));
  bool armed = false;
  bool triggered = false;
  uint64_t dump_at = 0;

  if (mode == "boot" || mode == "manual") {
    Arm(mode.c_str());
    armed = true;
  }

  for (;;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    const bool in_gameplay = rex::kernel::guest_presence::GameplayContextValue() == 1;
    const int32_t injected = skate3::demo_path::GameplayInputsInjected();
    const int32_t total = skate3::demo_path::GameplayInputsTotal();

    if (!armed) {
      if (mode == "macro-final") {
        // One input before the end: the last press is the confirm that starts
        // the load, so this opens the window just ahead of it.
        if (total > 0 && injected >= total - 1) {
          Arm("macro about to confirm");
          armed = true;
        }
      } else if (mode == "gameplay") {
        if (in_gameplay) {
          Arm("gameplay reached");
          armed = true;
        }
      }
      continue;
    }

    if (!triggered) {
      if (mode == "macro-final" && skate3::demo_path::GameplayInputSequenceComplete()) {
        triggered = true;
      } else if (mode == "boot" && in_gameplay) {
        triggered = true;
      } else if (mode == "gameplay") {
        triggered = true;
      }
      if (triggered) {
        dump_at = NowMs() + delay_ms;
      }
      continue;
    }

    // A full buffer in `first` mode is not fatal - the entries kept are the
    // earliest, which is the interesting end - but a silent truncation would
    // be read as "nothing else ran".
    if (!g_ring && g_next.load(std::memory_order_relaxed) > g_capacity &&
        g_full_warned.fetch_add(1) == 0) {
      REXLOG_WARN("skate3 trace: buffer full at {} entries - raise skate3_trace_capacity",
                  g_capacity);
    }
    if (NowMs() >= dump_at) {
      Dump("controller");
      return;
    }
  }
}

}  // namespace

// ---- sampling profiler -----------------------------------------------------
//
// Per-thread CPU accounting says render_thread is at 99% of a core and that it
// is what sets the frame rate. It does not say what the thread is doing, and
// the thread is machine-translated PowerPC: 47,889 functions named after their
// guest addresses, any of which could be the expensive one. Guessing which to
// optimise from a call trace does not work either - a trace says what ran, not
// what ran for a long time, and the two are barely related.
//
// So: suspend the thread, read its pc, resume, and count. This file already
// owns the host-pc-to-guest-function mapping the crash reporter uses, so a raw
// pc becomes `sub_82B298E0 +2172` without a symbol file that goes stale on the
// next relink.
//
// Two rules keep it from deadlocking against the thread it is watching. The
// sample is a suspend, one register read, and a resume - no allocation, no
// logging, no lock that the suspended thread could be holding, because it is
// suspended at an arbitrary instruction and may hold anything. And pcs are
// resolved afterwards, off the suspended path, when nothing is stopped.
#if defined(__APPLE__) || defined(__linux__)

REXCVAR_DEFINE_BOOL(skate3_guest_profile, false, "Skate 3",
                    "Sample the busiest guest threads and report the guest functions they spend "
                    "their time in. Costs one suspend/resume per thread per interval.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_INT32(skate3_guest_profile_interval_us, 1000, "Skate 3",
                     "Sampling interval, microseconds.")
    .range(200, 100000);

REXCVAR_DEFINE_INT32(skate3_guest_profile_report_s, 30, "Skate 3",
                     "Seconds between profile reports.")
    .range(5, 600);

REXCVAR_DEFINE_STRING(skate3_guest_profile_order_file, "", "Skate 3",
                      "Write the ranked hot guest functions here as an lld "
                      "--symbol-ordering-file (empty = off).");

REXCVAR_DEFINE_STRING(skate3_guest_profile_thread, "render_thread", "Skate 3",
                      "Name (prefix) of the thread to sample.");

namespace {

struct SampleCounts {
  // Keyed by guest function entry address; 0 means the pc was not in generated
  // code at all, which is worth seeing as its own bucket - it is the runtime,
  // the kernel imports, or libc underneath the guest.
  std::map<uint32_t, uint64_t> by_function;
  // Host pcs, as an offset from the containing module's load address. The
  // <host> bucket was the largest single entry on the guest main thread and
  // the profiler could only say "not generated code", which named nothing.
  // These are logged as module+offset and symbolised afterwards against the
  // UNSTRIPPED library the build keeps for ndk-stack - the shipped one is
  // stripped, so dladdr on the device would resolve almost nothing.
  std::map<std::string, uint64_t> by_host;
  uint64_t total = 0;
};

std::string HostSite(uintptr_t pc) {
#if defined(__linux__)
  Dl_info info = {};
  if (dladdr(reinterpret_cast<const void*>(pc), &info) != 0 && info.dli_fname != nullptr) {
    const char* slash = std::strrchr(info.dli_fname, '/');
    const char* mod = slash != nullptr ? slash + 1 : info.dli_fname;
    if (info.dli_sname != nullptr) {
      return fmt::format("{}!{}", mod, info.dli_sname);
    }
    return fmt::format("{}+0x{:x}", mod,
                       uintptr_t(pc) - uintptr_t(info.dli_fbase));
  }
#endif
  return "unknown";
}

#if defined(__APPLE__)
using SampleTarget = mach_port_t;
constexpr SampleTarget kNoTarget = MACH_PORT_NULL;

mach_port_t FindThreadByName(const std::string& wanted) {
  thread_act_array_t threads = nullptr;
  mach_msg_type_number_t thread_count = 0;
  if (task_threads(mach_task_self(), &threads, &thread_count) != KERN_SUCCESS) {
    return MACH_PORT_NULL;
  }
  mach_port_t found = MACH_PORT_NULL;
  for (mach_msg_type_number_t i = 0; i < thread_count; ++i) {
    if (found == MACH_PORT_NULL) {
      char name[64] = {};
      pthread_t pt = pthread_from_mach_thread_np(threads[i]);
      if (pt != nullptr && pthread_getname_np(pt, name, sizeof(name)) == 0 &&
          std::strncmp(name, wanted.c_str(), wanted.size()) == 0) {
        // Keep this port; deallocate the rest.
        found = threads[i];
        continue;
      }
    }
    mach_port_deallocate(mach_task_self(), threads[i]);
  }
  vm_deallocate(mach_task_self(), vm_address_t(threads),
                vm_size_t(thread_count * sizeof(thread_act_t)));
  return found;
}

// Reads the pc of a running thread WITHOUT stopping it. Mach can suspend a
// thread and read its register state from outside; Linux cannot, so the thread
// is asked to report its own pc from a signal handler instead. The handler
// runs on the target thread with the interrupted context, which is exactly
// what a sampling profiler needs, and the target is only interrupted for the
// few instructions it takes to store one word.
bool SampleThreadPc(SampleTarget target, uintptr_t* out_pc, uint64_t* out_x8) {
  *out_x8 = 0;
  arm_thread_state64_t state = {};
  mach_msg_type_number_t state_count = ARM_THREAD_STATE64_COUNT;
  if (thread_suspend(target) != KERN_SUCCESS) {
    return false;
  }
  const bool ok = thread_get_state(target, ARM_THREAD_STATE64,
                                   reinterpret_cast<thread_state_t>(&state),
                                   &state_count) == KERN_SUCCESS;
  thread_resume(target);
  if (ok) {
    *out_pc = uintptr_t(arm_thread_state64_get_pc(state));
  }
  return ok;
}

void ReleaseTarget(SampleTarget target) { mach_port_deallocate(mach_task_self(), target); }

#else  // Linux / Android

using SampleTarget = pid_t;
constexpr SampleTarget kNoTarget = 0;

// Set by the handler on the sampled thread, read by the sampler thread.
std::atomic<uintptr_t> g_sample_pc{0};
std::atomic<int> g_sample_ready{0};

std::atomic<uint64_t> g_sample_x8{0};

void SampleSignalHandler(int, siginfo_t*, void* uctx) {
  const auto* uc = static_cast<const ucontext_t*>(uctx);
  g_sample_pc.store(uintptr_t(uc->uc_mcontext.pc), std::memory_order_relaxed);
  // arm64 puts the syscall number in x8 at the svc, and it is still there
  // while the thread is inside the call. "libc.so!syscall" was the biggest
  // single entry on the guest main thread and named nothing on its own; this
  // says which call it is, and therefore whether the thread is working or
  // waiting.
  g_sample_x8.store(uc->uc_mcontext.regs[8], std::memory_order_relaxed);
  g_sample_ready.store(1, std::memory_order_release);
}

// /proc/self/task/<tid>/comm holds the thread name, truncated to 15 bytes -
// the same truncation the thread-placement map has to allow for, so a prefix
// match is the only thing that works for names like "render_thread".
pid_t FindThreadByName(const std::string& wanted) {
  DIR* d = opendir("/proc/self/task");
  if (d == nullptr) {
    return 0;
  }
  pid_t found = 0;
  while (dirent* e = readdir(d)) {
    if (e->d_name[0] < '0' || e->d_name[0] > '9') {
      continue;
    }
    char path[128];
    std::snprintf(path, sizeof(path), "/proc/self/task/%s/comm", e->d_name);
    std::ifstream in(path);
    std::string name;
    if (!in || !std::getline(in, name)) {
      continue;
    }
    if (name.compare(0, wanted.size(), wanted) == 0) {
      found = pid_t(std::atoi(e->d_name));
      break;
    }
  }
  closedir(d);
  return found;
}

bool SampleThreadPc(SampleTarget target, uintptr_t* out_pc, uint64_t* out_x8) {
  static std::once_flag once;
  std::call_once(once, [] {
    struct sigaction sa = {};
    sa.sa_sigaction = SampleSignalHandler;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGPROF, &sa, nullptr);
  });
  g_sample_ready.store(0, std::memory_order_relaxed);
  if (syscall(SYS_tgkill, getpid(), target, SIGPROF) != 0) {
    return false;  // thread is gone
  }
  // Bounded: a thread blocked in a syscall may not run the handler promptly,
  // and a sampler that waits forever for it would stop sampling altogether.
  for (int spin = 0; spin < 2000; ++spin) {
    if (g_sample_ready.load(std::memory_order_acquire)) {
      *out_pc = g_sample_pc.load(std::memory_order_relaxed);
      *out_x8 = g_sample_x8.load(std::memory_order_relaxed);
      return true;
    }
    std::this_thread::sleep_for(std::chrono::microseconds(10));
  }
  return false;
}

void ReleaseTarget(SampleTarget) {}

#endif

void SamplerMain() {
  const std::string wanted = REXCVAR_GET(skate3_guest_profile_thread);
  const auto interval = std::chrono::microseconds(REXCVAR_GET(skate3_guest_profile_interval_us));
  const auto report_every = std::chrono::seconds(REXCVAR_GET(skate3_guest_profile_report_s));

  EnsureTables();

  SampleTarget target = kNoTarget;
  SampleCounts counts;
  auto next_report = std::chrono::steady_clock::now() + report_every;
  // Raw pcs, resolved after the target is running again.
  std::vector<uintptr_t> pending;
  pending.reserve(4096);
  std::vector<uint64_t> pending_x8;
  pending_x8.reserve(4096);
  std::map<uint64_t, uint64_t> syscalls;

  while (true) {
    if (target == kNoTarget) {
      target = FindThreadByName(wanted);
      if (target == kNoTarget) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        continue;
      }
      REXLOG_INFO("skate3 profile: sampling '{}' every {}us", wanted, interval.count());
    }

    uintptr_t pc = 0;
    uint64_t x8 = 0;
    if (!SampleThreadPc(target, &pc, &x8)) {
      // The thread went away - find it again next time round.
      ReleaseTarget(target);
      target = kNoTarget;
      continue;
    }
    pending.push_back(pc);
    pending_x8.push_back(x8);

    // Resolve off the suspended path.
    if (pending.size() >= 256) {
      for (size_t i = 0; i < pending.size(); ++i) {
        const uintptr_t pc = pending[i];
        const uint32_t guest =
            GuestFunctionForHostPc(reinterpret_cast<const void*>(pc), nullptr);
        counts.by_function[guest]++;
        if (guest == 0) {
          const std::string site = HostSite(pc);
          counts.by_host[site]++;
          if (site.find("syscall") != std::string::npos && i < pending_x8.size()) {
            syscalls[pending_x8[i]]++;
          }
        }
        counts.total++;
      }
      pending.clear();
      pending_x8.clear();
    }

    const auto now = std::chrono::steady_clock::now();
    if (now >= next_report) {
      for (size_t i = 0; i < pending.size(); ++i) {
        const uintptr_t pc = pending[i];
        const uint32_t guest =
            GuestFunctionForHostPc(reinterpret_cast<const void*>(pc), nullptr);
        counts.by_function[guest]++;
        if (guest == 0) {
          const std::string site = HostSite(pc);
          counts.by_host[site]++;
          if (site.find("syscall") != std::string::npos && i < pending_x8.size()) {
            syscalls[pending_x8[i]]++;
          }
        }
        counts.total++;
      }
      pending.clear();
      pending_x8.clear();
      next_report = now + report_every;

      if (counts.total) {
        std::vector<std::pair<uint64_t, uint32_t>> ranked;
        ranked.reserve(counts.by_function.size());
        for (const auto& [guest, hits] : counts.by_function) {
          ranked.emplace_back(hits, guest);
        }
        std::sort(ranked.begin(), ranked.end(),
                  [](const auto& a, const auto& b) { return a.first > b.first; });
        std::string line;
        for (size_t i = 0; i < ranked.size() && i < 12; ++i) {
          const double percent = 100.0 * double(ranked[i].first) / double(counts.total);
          if (percent < 0.5) {
            break;
          }
          if (!line.empty()) {
            line += ' ';
          }
          if (ranked[i].second == 0) {
            line += fmt::format("<host>={:.1f}%", percent);
          } else {
            line += fmt::format("sub_{:08X}={:.1f}%", ranked[i].second, percent);
          }
        }
        REXLOG_INFO("skate3 profile: {} samples of '{}' | {}", counts.total, wanted, line);
        // How concentrated is the cost? A flat profile spread over thousands
        // of functions is the signature of instruction-cache and iTLB
        // thrashing, which is the pathology to expect from 290 MB of
        // recompiled code on a 32 KB L1I - and unlike a hotspot it is fixed by
        // code LAYOUT, not by making any one function cheaper. The answer
        // decides whether a linker symbol-ordering file is worth building:
        // a hot set of a few hundred functions can be packed together, a hot
        // set of tens of thousands cannot.
        {
          std::vector<uint64_t> hits;
          hits.reserve(ranked.size());
          for (const auto& r : ranked) {
            if (r.second != 0) {
              hits.push_back(r.first);
            }
          }
          uint64_t acc = 0;
          size_t n50 = 0, n80 = 0, n95 = 0;
          const double guest_total = double(std::accumulate(hits.begin(), hits.end(), uint64_t(0)));
          for (size_t i = 0; i < hits.size(); ++i) {
            acc += hits[i];
            const double frac = double(acc) / guest_total;
            if (n50 == 0 && frac >= 0.50) n50 = i + 1;
            if (n80 == 0 && frac >= 0.80) n80 = i + 1;
            if (n95 == 0 && frac >= 0.95) n95 = i + 1;
          }
          REXLOG_INFO("skate3 profile spread: {} distinct guest functions; "
                      "50% of guest samples in {}, 80% in {}, 95% in {}",
                      hits.size(), n50, n80, n95);
          // Write the ranked hot set where the build can pick it up as a
          // linker symbol-ordering file. The functions are emitted with
          // -ffunction-sections, so lld can place them in this order and pack
          // the hot set contiguously instead of leaving it scattered through
          // 290 MB of recompiled code.
          if (const std::string path = REXCVAR_GET(skate3_guest_profile_order_file);
              !path.empty()) {
            std::ofstream out(path, std::ios::trunc);
            if (out) {
              for (const auto& r : ranked) {
                if (r.second == 0) {
                  continue;
                }
                // Both names: the recompiler emits the guest body as
                // __imp__sub_X and a thunk sub_X, and the hot path runs
                // through whichever the caller referenced.
                out << fmt::format("__imp__sub_{:08X}\n", r.second);
                out << fmt::format("sub_{:08X}\n", r.second);
              }
              REXLOG_INFO("skate3 profile: wrote {} ordered symbols to {}",
                          ranked.size() * 2, path);
            }
          }
        }
        if (!counts.by_host.empty()) {
          std::vector<std::pair<uint64_t, std::string>> hranked;
          hranked.reserve(counts.by_host.size());
          for (const auto& [site, hits] : counts.by_host) {
            hranked.emplace_back(hits, site);
          }
          std::sort(hranked.begin(), hranked.end(),
                    [](const auto& a, const auto& b) { return a.first > b.first; });
          std::string hline;
          for (size_t i = 0; i < hranked.size() && i < 10; ++i) {
            const double percent = 100.0 * double(hranked[i].first) / double(counts.total);
            if (percent < 0.3) {
              break;
            }
            if (!hline.empty()) {
              hline += ' ';
            }
            hline += fmt::format("{}={:.1f}%", hranked[i].second, percent);
          }
          REXLOG_INFO("skate3 profile host: {}", hline);
          if (!syscalls.empty()) {
            // arm64 numbers: 98 futex, 101 nanosleep, 113 clock_gettime,
            // 115 clock_nanosleep, 73 ppoll, 63 read, 64 write.
            std::vector<std::pair<uint64_t, uint64_t>> sr;
            for (const auto& [nr, hits] : syscalls) {
              sr.emplace_back(hits, nr);
            }
            std::sort(sr.begin(), sr.end(),
                      [](const auto& a, const auto& b) { return a.first > b.first; });
            std::string sline;
            for (size_t i = 0; i < sr.size() && i < 6; ++i) {
              if (!sline.empty()) {
                sline += ' ';
              }
              sline += fmt::format("nr={}:{:.1f}%", sr[i].second,
                                   100.0 * double(sr[i].first) / double(counts.total));
            }
            REXLOG_INFO("skate3 profile syscalls: {}", sline);
            syscalls.clear();
          }
        }
        counts.by_function.clear();
        counts.by_host.clear();
        counts.total = 0;
      }
    }

    std::this_thread::sleep_for(interval);
  }
}

}  // namespace

void InstallSampler() {
  if (!REXCVAR_GET(skate3_guest_profile)) {
    return;
  }
  std::thread(SamplerMain).detach();
}

#else
void InstallSampler() {}
#endif

void Install() {
  if (!REXCVAR_GET(skate3_trace)) {
    return;
  }
  g_ring = REXCVAR_GET(skate3_trace_mode) == "ring";
  g_capacity = size_t(REXCVAR_GET(skate3_trace_capacity));
  // Allocated once, before the guest boots, and never resized: the recorder
  // runs on guest threads with no lock, so the buffer must not move.
  g_entries = static_cast<Entry*>(std::calloc(g_capacity, sizeof(Entry)));
  if (g_entries == nullptr) {
    g_capacity = 0;
    REXLOG_ERROR("skate3 trace: could not allocate {} entries", REXCVAR_GET(skate3_trace_capacity));
    return;
  }
  EnsureTables();
  REXLOG_INFO("skate3 trace: enabled ({} mode, {} entries, arm={}) -> {}",
              g_ring ? "ring" : "first", g_capacity, REXCVAR_GET(skate3_trace_arm), TracePath());
  std::thread(ControllerMain).detach();
}

}  // namespace skate3::guest_trace

// Recording hook, called from REX_FUNC_PROLOGUE in generated/skate3_init.h.
// Only ever reached while armed.
extern "C" {

void skate3_trace_enter(const char* fn, const PPCContext& ctx, uint8_t* base) {
  using namespace skate3::guest_trace;
  const size_t capacity = g_capacity;
  if (capacity == 0) {
    return;
  }
  const uint64_t slot = g_next.fetch_add(1, std::memory_order_relaxed);
  if (!g_ring && slot >= capacity) {
    return;
  }
  Entry& e = g_entries[slot % capacity];
  e.fn = fn;
  e.r3 = ctx.r3.u32;
  e.r4 = ctx.r4.u32;
  e.r5 = ctx.r5.u32;
  e.stamp = Stamp();
  e.thread = uint32_t(ThreadIndex());
  // Resolved here rather than at dump time: by then the pointer is stale.
  ReadGuestString(base, e.r3, e.text[0], kStringPreview);
  ReadGuestString(base, e.r4, e.text[1], kStringPreview);
  ReadGuestString(base, e.r5, e.text[2], kStringPreview);
}

}  // extern "C"
