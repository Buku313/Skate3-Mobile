// See the header for why this exists.

#include "skate3_crash_report.h"

#include "skate3_native_scene.h"

// For the guest X_KTHREAD pointer, which is the number
// RtlEnterCriticalSection reports as owner_thread=.
#include <rex/system/xthread.h>

#if !defined(_WIN32)

#include <dirent.h>
#if defined(__ANDROID__)
// bionic has no <execinfo.h>; this supplies backtrace* over the unwinder.
#include <rex/execinfo_android.h>
#else
#include <execinfo.h>
#endif
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#if defined(__APPLE__)
// mach_vm.h is macOS-only; the vm_* entry points below are the ones the iOS
// SDK exposes, and they are equivalent for same-task reads.
#include <dlfcn.h>
#include <mach/mach.h>
#include <pthread/pthread.h>
#else
#include <sys/prctl.h>
#endif
#include <sys/syscall.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <thread>

#include <skate3_version.h>

#include <rex/cvar.h>
#include <rex/exception_handler.h>
#include <rex/logging.h>
#include <rex/logging/api.h>
#include <rex/ppc/context.h>
#include <rex/system/thread_state.h>

REXCVAR_DECLARE(int32_t, skate3_hang_watchdog_seconds);
REXCVAR_DECLARE(int32_t, skate3_stall_watchdog_seconds);

namespace skate3::crash_report {

namespace {

// Span of the runtime's guest views measured from the virtual membase: 4 GB of
// virtual address space plus the 512 MB physical-raw view mapped above it
// (same constant as the read-recovery handler in native/skate3_native_guest_read.cpp).
constexpr uint64_t kGuestSpanBytes = 0x120000000ull;

uint8_t* g_guest_base = nullptr;
int g_crash_fd = -1;

// Everything below runs in signal context: no allocation, no locks, no
// spdlog. A fixed stack buffer plus write(2) only.
class Report {
 public:
  void Str(const char* s) {
    while (*s && len_ < kCap - 1) {
      buf_[len_++] = *s++;
    }
  }
  void Hex(uint64_t v, int digits) {
    static const char kDigits[] = "0123456789ABCDEF";
    for (int i = digits - 1; i >= 0 && len_ < kCap - 1; --i) {
      buf_[len_++] = kDigits[(v >> (i * 4)) & 0xF];
    }
  }
  void Dec(uint64_t v) {
    char tmp[24];
    int n = 0;
    do {
      tmp[n++] = char('0' + (v % 10));
      v /= 10;
    } while (v && n < int(sizeof(tmp)));
    while (n-- > 0 && len_ < kCap - 1) {
      buf_[len_++] = tmp[n];
    }
  }
  // "  r26=00000000_400053B4" - the u64 split so a 32-bit guest pointer reads
  // straight off the low half.
  void Reg(const char* name, uint64_t v) {
    Str("  ");
    Str(name);
    Str("=");
    Hex(v >> 32, 8);
    Str("_");
    Hex(v & 0xFFFFFFFFull, 8);
  }
  void Flush() {
    if (len_ == 0) {
      return;
    }
    // Best effort on both sinks; a short write here is not worth a retry loop.
    (void)!::write(STDERR_FILENO, buf_, len_);
    if (g_crash_fd >= 0) {
      (void)!::write(g_crash_fd, buf_, len_);
    }
    len_ = 0;
  }

 private:
  static constexpr size_t kCap = 4096;
  char buf_[kCap];
  size_t len_ = 0;
};

// Thread identity, signal-safe on both OSes: Linux names threads through
// prctl and tids through a syscall; Darwin has no prctl or gettid and exposes
// both through pthread_np calls instead.
uint64_t HostTid() {
#if defined(__APPLE__)
  uint64_t tid = 0;
  pthread_threadid_np(nullptr, &tid);
  return tid;
#else
  return uint64_t(syscall(SYS_gettid));
#endif
}

bool HostThreadName(char* name, size_t len) {
#if defined(__APPLE__)
  return pthread_getname_np(pthread_self(), name, len) == 0 && name[0] != 0;
#else
  (void)len;
  return prctl(PR_GET_NAME, reinterpret_cast<unsigned long>(name), 0, 0, 0) == 0;
#endif
}

// Host backtrace. Not every fault is guest code - a null HOST pointer
// dereferenced on a guest-named thread looks identical in the register dump,
// and only the host frames tell the two apart. backtrace_symbols_fd is the
// signal-safe printer (it formats into a stack buffer and writes; no malloc,
// unlike backtrace_symbols). Without -g the frames print as
// "binary(+0xoffset)", which is enough: resolve with
//   addr2line -f -e out/build/linux-release/skate3 <offset>
// or `nm -C` for the nearest symbol.
void WriteHostBacktrace() {
  void* frames[32];
  const int n = ::backtrace(frames, 32);
  if (n <= 0) {
    return;
  }
  static const char kHdr[] = "  host backtrace:\n";
  (void)!::write(STDERR_FILENO, kHdr, sizeof(kHdr) - 1);
  ::backtrace_symbols_fd(frames, n, STDERR_FILENO);
  if (g_crash_fd >= 0) {
    (void)!::write(g_crash_fd, kHdr, sizeof(kHdr) - 1);
    ::backtrace_symbols_fd(frames, n, g_crash_fd);
  }
}

// Shared tail of every report: who was running, what the guest machine looked
// like, and the host frames. A null ThreadState is itself the answer to "was
// this guest code?".
void WriteGuestState(Report& r) {
  char name[20] = {0};
  if (HostThreadName(name, sizeof(name))) {
    r.Str("  thread        ");
    r.Str(name);
    r.Str("\n");
  }
  r.Str("  host tid      ");
  r.Dec(HostTid());
  r.Str("\n");

  auto* ts = rex::runtime::ThreadState::Get();
  if (ts == nullptr || ts->context() == nullptr) {
    r.Str("  guest thread  NO (no bound ThreadState - host-side fault)\n");
    r.Str("=== end ===\n");
    r.Flush();
    WriteHostBacktrace();
    return;
  }

  const ::PPCContext& c = *ts->context();
  r.Str("  guest thid    ");
  r.Dec(ts->thread_id());
  // lr is the guest return address: the function that made the call that went
  // wrong. It is the single most useful number in this dump - look it up with
  //   grep -n 'DEFINE_REX_FUNC' generated/*.cpp
  // and take the nearest definition below it.
  r.Str("\n  guest lr      0x");
  r.Hex(c.lr, 8);
  r.Str("   <- caller; find the nearest DEFINE_REX_FUNC below this address\n");
  r.Str("  guest ctr     0x");
  r.Hex(c.ctr.u64 & 0xFFFFFFFFull, 8);
  r.Str("   <- indirect call target\n");
  r.Str("  guest regs:\n");

  const struct {
    const char* name;
    uint64_t value;
  } gprs[] = {
      {" r0", c.r0.u64},  {" r1", c.r1.u64},  {" r2", c.r2.u64},  {" r3", c.r3.u64},
      {" r4", c.r4.u64},  {" r5", c.r5.u64},  {" r6", c.r6.u64},  {" r7", c.r7.u64},
      {" r8", c.r8.u64},  {" r9", c.r9.u64},  {"r10", c.r10.u64}, {"r11", c.r11.u64},
      {"r12", c.r12.u64}, {"r13", c.r13.u64}, {"r14", c.r14.u64}, {"r15", c.r15.u64},
      {"r16", c.r16.u64}, {"r17", c.r17.u64}, {"r18", c.r18.u64}, {"r19", c.r19.u64},
      {"r20", c.r20.u64}, {"r21", c.r21.u64}, {"r22", c.r22.u64}, {"r23", c.r23.u64},
      {"r24", c.r24.u64}, {"r25", c.r25.u64}, {"r26", c.r26.u64}, {"r27", c.r27.u64},
      {"r28", c.r28.u64}, {"r29", c.r29.u64}, {"r30", c.r30.u64}, {"r31", c.r31.u64},
  };
  for (size_t i = 0; i < sizeof(gprs) / sizeof(gprs[0]); ++i) {
    r.Reg(gprs[i].name, gprs[i].value);
    // The register block is the bulk of the report; flush per line so a
    // truncated write still leaves something readable.
    if ((i % 4) == 3) {
      r.Str("\n");
      r.Flush();
    }
  }
  r.Str("=== end ===\n");
  r.Flush();
  WriteHostBacktrace();
}

// ---- Hang watchdog --------------------------------------------------------
// A freeze produces NO signal, so the fault reporter above never fires and the
// log just stops advancing - which is exactly the state the user sees and the
// one we had no data for. This detects it from the inside: the guest frame hook
// bumps a heartbeat, a watchdog thread notices when it stops, and then every
// thread in the process is asked to print its own stack.
//
// "Ask each thread" is the only way to get other threads' stacks in-process:
// backtrace() only ever walks the CALLING thread. So the watchdog signals each
// tid in /proc/self/task and each one dumps itself from the signal handler.
// SIGRTMIN carries no default meaning and is not used by the runtime.
std::atomic<uint64_t> g_heartbeat{0};
// Work the guest has submitted, as reported at the frame boundary. Distinct
// from the heartbeat: frames can keep arriving while this stands still.
std::atomic<uint64_t> g_guest_work{0};
std::atomic<bool> g_hang_reported{false};

// Report attribution. The crash file is append-only and survives across
// sessions and builds, so without these every block in it is anonymous.
// Pre-formatted at install time and ticked from the watchdog thread; the
// signal handlers only Str()/Dec() them.
char g_stamp[160] = "";
std::atomic<uint64_t> g_uptime_seconds{0};
std::atomic<uint64_t> g_epoch0{0};

// Which front-end screen was on top when this report was written. The value is
// a relaxed atomic the scene code already publishes every frame, so reading it
// from a signal handler is safe, and it is the one piece of context that turns
// a stranger's crash report from an address into a place. Every crash in the
// v2.5.0 field reports named a screen ("selecting a photo", "adding a second
// skater") and the dump could not confirm any of them.
const char* FrontEndScreenName(uint32_t id) {
  // Ids surveyed from captures; see PortraitRttWindowActive and
  // YieldForCasEditor in skate3_native_scene.cpp, which classify the same set.
  switch (id) {
    case skate3::native_scene::kFrontEndStackEmpty: return "none/gameplay";
    case 0:  return "FE root";
    case 1:  return "photo editor";
    case 11: return "photo editor";
    case 15: return "create-a-skater editor";
    case 17: return "pause challenge map";
    case 24: return "FMV";
    case 56: return "pause root";
    case 59: return "skate-reel browser";
    case 63: return "team screen";
    default: return "unclassified";
  }
}

void WriteReportStamp(Report& r) {
  r.Str("  ");
  r.Str(g_stamp);
  r.Str(" uptime_s=");
  r.Dec(g_uptime_seconds.load(std::memory_order_relaxed));
  r.Str(" epoch0=");
  r.Dec(g_epoch0.load(std::memory_order_relaxed));
  r.Str("\n");
  const uint32_t fe = skate3::native_scene::FrontEndTopScreen();
  r.Str("  fe screen     ");
  r.Str(FrontEndScreenName(fe));
  if (fe != skate3::native_scene::kFrontEndStackEmpty) {
    r.Str(" (id ");
    r.Dec(fe);
    r.Str(")");
  }
  r.Str("\n");
}

#if defined(__linux__)
void ThreadDumpHandler(int, siginfo_t*, void*) {
  Report r;
  char name[20] = {0};
  r.Str("  --- tid ");
  r.Dec(HostTid());
  if (HostThreadName(name, sizeof(name))) {
    r.Str(" (");
    r.Str(name);
    r.Str(")");
  }
  // Guest threads carry a bound ThreadState; naming the guest pc/lr next to the
  // host frames is what tells us whether a stuck thread is spinning in guest
  // code or blocked in the runtime.
  auto* ts = rex::runtime::ThreadState::Get();
  if (ts != nullptr && ts->context() != nullptr) {
    r.Str(" guest thid=");
    r.Dec(ts->thread_id());
    // The same number RtlEnterCriticalSection prints as owner_thread=, which
    // is the guest X_KTHREAD pointer - NOT the handle set_name embeds and not
    // thread_id. Without it a "waiting 16s for cs=... owner_thread=..."
    // warning names a thread that appears nowhere else in the report, and the
    // one thread worth looking at is the one that cannot be identified.
    // IsInThread() first: GetCurrentThread() asserts on a null binding, and an
    // assert inside a signal handler is not something to find out about here.
    // Both read the same thread_local pointer, so this is signal-safe.
    if (rex::system::XThread::IsInThread()) {
      r.Str(" guest_obj=0x");
      r.Hex(rex::system::XThread::GetCurrentThread()->guest_object(), 8);
    }
    r.Str(" lr=0x");
    r.Hex(ts->context()->lr, 8);
    r.Str(" r1=0x");
    r.Hex(ts->context()->r1.u64 & 0xFFFFFFFFull, 8);
  }
  r.Str("\n");
  r.Flush();
  WriteHostBacktrace();
}
#endif  // __linux__

void DumpAllThreads() {
  Report r;
  r.Str("\n=== skate3: HANG detected - no guest frame for the watchdog period ===\n");
  WriteReportStamp(r);
#if defined(__linux__)
  r.Str("  every thread's stack follows; the one holding the lock everyone else\n");
  r.Str("  is waiting on is the interesting one.\n");
  r.Flush();
  // Enumerating /proc/self/task from a normal thread (not signal context), so
  // opendir is fine here.
  DIR* d = ::opendir("/proc/self/task");
  if (d == nullptr) {
    return;
  }
  const pid_t self_tid = pid_t(syscall(SYS_gettid));
  const pid_t pid = ::getpid();
  while (struct dirent* e = ::readdir(d)) {
    if (e->d_name[0] < '0' || e->d_name[0] > '9') {
      continue;
    }
    const pid_t tid = pid_t(atoi(e->d_name));
    if (tid == self_tid) {
      continue;  // the watchdog's own stack says nothing
    }
    if (syscall(SYS_tgkill, pid, tid, SIGRTMIN) != 0) {
      continue;
    }
    // Let the target finish writing before signalling the next one, so the
    // dumps do not interleave into nonsense.
    struct timespec ts = {0, 30 * 1000 * 1000};
    nanosleep(&ts, nullptr);
  }
  ::closedir(d);
#elif defined(__APPLE__)
  // Darwin has neither SIGRTMIN nor /proc, and on a device there is no
  // `sample` to run from outside, so the stacks are collected directly: each
  // thread is suspended, its saved pc/lr read, and its frame-pointer chain
  // walked. AArch64 keeps a reliable fp chain ([fp] = caller fp,
  // [fp+8] = caller lr), which is what makes this practical without an
  // unwinder. Not signal context - the watchdog owns this thread - so
  // dladdr and mach calls are fair game.
  r.Str("  every thread's stack follows; the one holding the lock everyone else\n");
  r.Str("  is waiting on is the interesting one.\n");
  r.Flush();

  thread_act_array_t threads = nullptr;
  mach_msg_type_number_t thread_count = 0;
  if (task_threads(mach_task_self(), &threads, &thread_count) != KERN_SUCCESS) {
    Report fail;
    fail.Str("  (task_threads failed)\n");
    fail.Flush();
    return;
  }
  const thread_t self_thread = mach_thread_self();
  for (mach_msg_type_number_t i = 0; i < thread_count; ++i) {
    if (threads[i] == self_thread) {
      continue;  // the watchdog's own stack says nothing
    }
    // Suspending keeps the register state and stack coherent while they are
    // read; without it the walk chases a chain that is still moving.
    if (thread_suspend(threads[i]) != KERN_SUCCESS) {
      continue;
    }
    arm_thread_state64_t state = {};
    mach_msg_type_number_t state_count = ARM_THREAD_STATE64_COUNT;
    const kern_return_t got_state = thread_get_state(
        threads[i], ARM_THREAD_STATE64, reinterpret_cast<thread_state_t>(&state), &state_count);

    Report t;
    t.Str("  --- thread ");
    t.Dec(uint64_t(i));
    char name[64] = {0};
    if (pthread_t pt = pthread_from_mach_thread_np(threads[i])) {
      if (pthread_getname_np(pt, name, sizeof(name)) == 0 && name[0]) {
        t.Str(" (");
        t.Str(name);
        t.Str(")");
      }
    }
    if (got_state != KERN_SUCCESS) {
      t.Str(" <no state>\n");
      t.Flush();
      thread_resume(threads[i]);
      continue;
    }
    t.Str("\n");

    // pc and lr first: for a thread parked in a syscall the pc alone usually
    // names the wait, and lr names who asked for it.
    uint64_t pc = arm_thread_state64_get_pc(state);
    uint64_t fp = arm_thread_state64_get_fp(state);
    const uint64_t lr = arm_thread_state64_get_lr(state);
    for (int frame = 0; frame < 24; ++frame) {
      t.Str("      0x");
      t.Hex(pc, 16);
      Dl_info info = {};
      if (dladdr(reinterpret_cast<void*>(pc), &info) && info.dli_sname) {
        t.Str("  ");
        t.Str(info.dli_sname);
      } else if (info.dli_fname) {
        t.Str("  ");
        t.Str(info.dli_fname);
      }
      t.Str("\n");
      t.Flush();

      if (fp == 0 || (fp & 7) != 0) {
        break;
      }
      // Read through mach_vm_read_overwrite rather than dereferencing: a
      // garbage fp in a corrupted frame would otherwise fault the watchdog
      // while it is reporting the original problem.
      uint64_t frame_data[2] = {0, 0};
      vm_size_t read_size = 0;
      if (vm_read_overwrite(mach_task_self(), vm_address_t(fp), sizeof(frame_data),
                            vm_address_t(&frame_data[0]), &read_size) != KERN_SUCCESS ||
          read_size != sizeof(frame_data)) {
        break;
      }
      const uint64_t next_fp = frame_data[0];
      const uint64_t next_pc = frame_data[1];
      if (next_pc == 0 || next_fp <= fp) {
        break;  // end of chain, or it stopped ascending: do not loop forever
      }
      // Strip pointer authentication bits before symbolizing.
      pc = next_pc & 0x0000007fffffffffull;
      fp = next_fp;
      if (frame == 0 && pc == 0) {
        pc = lr;
      }
    }
    thread_resume(threads[i]);
  }
  mach_port_deallocate(mach_task_self(), self_thread);
  vm_deallocate(mach_task_self(), vm_address_t(threads),
                vm_size_t(thread_count * sizeof(thread_t)));
#endif
  Report tail;
  tail.Str("=== end hang report ===\n");
  tail.Flush();
}

// How long the guest may take to produce its first frame before the watchdog
// calls it a hang. Boot to gameplay measures ~25s on an iPhone 13 mini.
constexpr int kPreFirstFrameGraceSeconds = 75;

void WatchdogMain() {
  uint64_t last_seen = 0;
  int stalled_ticks = 0;
  uint64_t last_work = 0;
  int idle_ticks = 0;
  for (;;) {
    struct timespec ts = {1, 0};
    nanosleep(&ts, nullptr);
    g_uptime_seconds.fetch_add(1, std::memory_order_relaxed);
    const int limit = REXCVAR_GET(skate3_hang_watchdog_seconds);
    if (limit <= 0) {
      continue;
    }
    // Frames still arriving but the guest submitting no work: two guest
    // threads deadlocked against each other, with the render thread happily
    // presenting an unchanging scene. The frame heartbeat below cannot see
    // this, because the frames never stopped - so it is checked separately,
    // and given a longer grace period since a legitimate load screen submits
    // nothing for a while too.
    // Its own limit, and shorter than three times the frame-stall one it used
    // to borrow. A player who hits a hang relaunches within a few seconds -
    // long before 45 - so the dump that would explain it never got written.
    int stall_limit = REXCVAR_GET(skate3_stall_watchdog_seconds);
    if (stall_limit <= 0) {
      stall_limit = limit * 3;
    }
    const uint64_t work = g_guest_work.load(std::memory_order_relaxed);
    if (work != last_work) {
      last_work = work;
      idle_ticks = 0;
    } else if (work != 0 && ++idle_ticks >= stall_limit &&
               !g_hang_reported.exchange(true, std::memory_order_relaxed)) {
      Report r;
      r.Str("\n=== skate3: STALL - frames still presenting, guest submitting no draws ===\n");
      WriteReportStamp(r);
      r.Flush();
      DumpAllThreads();
    }

    const uint64_t now = g_heartbeat.load(std::memory_order_relaxed);
    // Before the guest has ever presented a frame the heartbeat is legitimately
    // zero for the whole of boot - ISO scan, module load, the intro - which on
    // this game is comfortably longer than the normal watchdog period. Arming
    // the watchdog early (so a guest that never starts still gets a dump) made
    // that fire on EVERY healthy launch, and because the report latches, the
    // real hang later in the same run was then never dumped at all. Give the
    // pre-first-frame phase its own, much longer grace.
    if (now == 0 && stalled_ticks < kPreFirstFrameGraceSeconds) {
      ++stalled_ticks;
      continue;
    }
    if (now != last_seen) {
      last_seen = now;
      stalled_ticks = 0;
      if (idle_ticks == 0) {
        g_hang_reported.store(false, std::memory_order_relaxed);
      }
      continue;
    }
    // The heartbeat legitimately stops while the game is not presenting
    // frames at all (alt-tabbed, a long blocking load). Only the first stall
    // past the limit is reported; the next resumed frame re-arms it.
    if (++stalled_ticks >= limit && !g_hang_reported.exchange(true, std::memory_order_relaxed)) {
      DumpAllThreads();
    }
  }
}

void StartWatchdog() {
#if defined(__linux__)
  struct sigaction sa = {};
  sa.sa_sigaction = ThreadDumpHandler;
  sa.sa_flags = SA_SIGINFO | SA_RESTART;  // SA_RESTART: do not break blocking syscalls
  sigemptyset(&sa.sa_mask);
  if (sigaction(SIGRTMIN, &sa, nullptr) != 0) {
    return;
  }
#endif
  std::thread(WatchdogMain).detach();
}

// REX_FATAL (a guest call through a null/unregistered function pointer, among
// others) logs and then std::abort()s, which is a SIGABRT rather than an
// access violation - the runtime's exception chain never sees it. This is the
// signature that actually reproduces on map transitions, so it needs the same
// register dump: ctr names the bogus call target and lr names the guest caller
// that loaded it.
struct sigaction g_prev_sigabrt;

void AbortHandler(int sig, siginfo_t* info, void* uctx) {
  static std::atomic<int> reporting{0};
  int expected = 0;
  if (reporting.compare_exchange_strong(expected, 1)) {
    Report r;
    r.Str("\n=== skate3: guest abort (REX_FATAL / assert) ===\n");
    WriteReportStamp(r);
    WriteGuestState(r);
  }
  // Chain, then let abort() finish the job: returning from here re-raises with
  // the default disposition.
  if ((g_prev_sigabrt.sa_flags & SA_SIGINFO) && g_prev_sigabrt.sa_sigaction) {
    g_prev_sigabrt.sa_sigaction(sig, info, uctx);
  } else if (g_prev_sigabrt.sa_handler != SIG_DFL && g_prev_sigabrt.sa_handler != SIG_IGN &&
             g_prev_sigabrt.sa_handler) {
    g_prev_sigabrt.sa_handler(sig);
  }
}

bool CrashReportHandler(rex::arch::Exception* ex, void* /*data*/) {
  if (ex->code() != rex::arch::Exception::Code::kAccessViolation &&
      ex->code() != rex::arch::Exception::Code::kIllegalInstruction) {
    return false;
  }
  // A fault raised while reporting (a wild ThreadState pointer, say) must not
  // recurse - report the first one and let the process die.
  static std::atomic<int> reporting{0};
  int expected = 0;
  if (!reporting.compare_exchange_strong(expected, 1)) {
    return false;
  }

  Report r;
  r.Str("\n=== skate3: unhandled guest fault ===\n");
  WriteReportStamp(r);

  const bool illegal = ex->code() == rex::arch::Exception::Code::kIllegalInstruction;
  r.Str(illegal ? "  kind          illegal instruction\n" : "  kind          access violation\n");

  if (!illegal) {
    switch (ex->access_violation_operation()) {
      case rex::arch::Exception::AccessViolationOperation::kRead:
        r.Str("  operation     READ\n");
        break;
      case rex::arch::Exception::AccessViolationOperation::kWrite:
        r.Str("  operation     WRITE\n");
        break;
      default:
        r.Str("  operation     unknown\n");
        break;
    }
    const uint64_t fault = ex->fault_address();
    r.Str("  host fault    0x");
    r.Hex(fault, 16);
    r.Str("\n");
    const uint64_t lo = uint64_t(reinterpret_cast<uintptr_t>(g_guest_base));
    if (g_guest_base && fault >= lo && fault - lo < kGuestSpanBytes) {
      r.Str("  GUEST addr    0x");
      r.Hex(fault - lo, 8);
      r.Str("\n");
    } else {
      r.Str("  GUEST addr    (outside the guest mapping - host-side fault)\n");
    }
  }

  r.Str("  host pc       0x");
  r.Hex(ex->pc(), 16);
  r.Str("\n");

  WriteGuestState(r);
  return false;  // decline: the process must still die
}

}  // namespace

void StartWatchdogEarly() {
  static std::once_flag once;
  std::call_once(once, [] {
    if (g_crash_fd < 0) {
      const std::string& log_file = REXCVAR_GET(log_file);
      const std::string path =
          log_file.empty() ? std::string("skate3_crash.txt") : log_file + ".crash";
      g_crash_fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    }
    snprintf(g_stamp, sizeof(g_stamp), "v=%s platform=%s", SKATE3_VERSION_STRING,
             SKATE3_BUILD_PLATFORM);
    g_epoch0.store(uint64_t(time(nullptr)), std::memory_order_relaxed);
    StartWatchdog();
    REXLOG_INFO("skate3 crash reporter: hang watchdog armed before the guest starts");
  });
}

void EnsureInstalled(uint8_t* guest_base) {
  static std::once_flag once;
  std::call_once(once, [guest_base] {
    g_guest_base = guest_base;
    // Pre-open the sink here, on a normal thread: opening a file is not
    // something the handler can do safely.
    const std::string& log_file = REXCVAR_GET(log_file);
    const std::string path = log_file.empty() ? std::string("skate3_crash.txt") : log_file + ".crash";
    g_crash_fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (g_crash_fd < 0) {
      REXLOG_WARN("skate3 crash reporter: could not open '{}'; stderr only", path);
    }
    // Attribution for every block appended to the .crash file. Formatted here,
    // on a normal thread, so the handlers only ever copy bytes.
    snprintf(g_stamp, sizeof(g_stamp), "v=%s platform=%s", SKATE3_VERSION_STRING,
             SKATE3_BUILD_PLATFORM);
    g_epoch0.store(uint64_t(time(nullptr)), std::memory_order_relaxed);

    rex::arch::ExceptionHandler::Install(CrashReportHandler, nullptr);
    StartWatchdog();

    struct sigaction sa = {};
    sa.sa_sigaction = AbortHandler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGABRT, &sa, &g_prev_sigabrt);

    REXLOG_INFO("skate3 crash reporter installed (guest faults report to stderr and '{}')", path);
  });
}

void Heartbeat() { g_heartbeat.fetch_add(1, std::memory_order_relaxed); }

void NoteGuestWork(uint64_t submitted) {
  g_guest_work.store(submitted, std::memory_order_relaxed);
}

}  // namespace skate3::crash_report

#endif  // !_WIN32
