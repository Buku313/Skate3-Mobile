// See skate3_image_watch.h for what this is and why it exists.
#include "skate3_image_watch.h"

#include <dlfcn.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/prctl.h>
#else
#include <pthread.h>
#endif

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/exception_handler.h>
#include <rex/filesystem/host_buffer_fault_hook.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/memory/utils.h>
#include <rex/ppc/context.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/thread_state.h>
#include <rex/system/user_module.h>
#include <rex/system/util/xex2_info.h>
#include <rex/system/xex_module.h>
#include <rex/system/xmemory.h>
#include <rex/types.h>

#include "generated/skate3_init.h"

REXCVAR_DEFINE_BOOL(skate3_image_watch, false, "Skate 3",
                    "Make the game image's read-only pages read-only after load, catch and "
                    "log the first write to each, keep a load-time copy and diff it every "
                    "heartbeat. Built for the QCS8550 handhelds whose codec tag table at "
                    "0x8210A310 is overwritten with English text before the first decoder is "
                    "made. It did its job - the image turned out to be wrong before any guest "
                    "code ran, which is what led to the overlapping memcpy in the title-update "
                    "patcher - so it is OFF by default now. Turn it on to investigate a new "
                    "report of static data changing under the game.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_STRING(skate3_image_watch_mode, "rodata", "Skate 3",
                      "Which image pages to protect: 'rodata' = the pages the XEX marks "
                      "read-only data plus the hot ranges (default); 'data' = every non-code "
                      "page below the code base, i.e. the whole static data region, which "
                      "also traps once on each page the game legitimately writes (debugging "
                      "on a healthy device); 'off' = snapshot and diff only, no protection.");

REXCVAR_DEFINE_BOOL(skate3_image_watch_diff, true, "Skate 3",
                    "Compare the protected pages against the load-time snapshot at every "
                    "heartbeat and at every codec-table miss, and log what differs.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_INT32(skate3_image_watch_diff_seconds, 30, "Skate 3",
                     "Seconds of frames between heartbeat diffs.")
    .range(5, 3600)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_STRING(skate3_image_watch_allow, "", "Skate 3",
                      "Comma-separated hex guest page addresses to leave writable and "
                      "unreported, for pages known to be written legitimately.");

REXCVAR_DEFINE_STRING(skate3_image_watch_hot, "8210A310:40", "Skate 3",
                      "Comma-separated hex 'address:length' ranges that must be watched "
                      "whatever the mode. Their pages are re-armed every frame so a write "
                      "that arrives after a legitimate one is still caught; the last eight "
                      "writers of each are kept and printed when its bytes are found changed.");

REXCVAR_DEFINE_INT32(skate3_image_watch_rearm_frames, 1, "Skate 3",
                     "Re-protect the hot pages every this many frames (0 = one-shot like every "
                     "other page).")
    .range(0, 100000)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(skate3_image_watch_log_hot_traps, false, "Skate 3",
                    "Log every write into a hot page as it happens instead of only the last "
                    "eight when the bytes are found changed. Noisy if a global in the same "
                    "page is written each frame.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace skate3::image_watch {
namespace {

constexpr uint32_t kAliasOffset = 0x10000000u;  // 0x9xxxxxxx is the same bytes as 0x8xxxxxxx
constexpr size_t kRingSize = 128;
constexpr size_t kHotHistory = 8;
constexpr size_t kMaxHotRanges = 8;
constexpr int kChainDepth = 8;

struct TrapRecord {
  std::atomic<uint32_t> state{0};  // 0 free, 1 being written, 2 ready to flush
  uint64_t seq = 0;
  uint32_t guest = 0;
  uint64_t host_pc = 0;
  uint64_t tid = 0;
  char thread_name[20] = {};
  bool has_guest_ctx = false;
  uint64_t guest_lr = 0;
  uint32_t guest_r1 = 0;
  uint32_t chain[kChainDepth] = {};
  uint32_t chain_len = 0;
  uint32_t pre_addr = 0;
  uint8_t pre[64] = {};
};

struct HotWriter {
  uint64_t seq = 0;
  uint32_t guest = 0;
  uint64_t host_pc = 0;
  uint64_t guest_lr = 0;
  uint32_t chain[kChainDepth] = {};
  uint32_t chain_len = 0;
  char thread_name[20] = {};
};

struct HotRange {
  uint32_t lo = 0;
  uint32_t hi = 0;
  HotWriter writers[kHotHistory];
  uint32_t writer_count = 0;
  uint64_t traps = 0;
};

struct State {
  std::atomic<bool> installed{false};
  uint8_t* base = nullptr;  // host address of guest 0
  uint32_t host_page = 4096;
  uint32_t lo = 0;  // watched span [lo, hi), page-aligned
  uint32_t hi = 0;
  size_t page_count = 0;
  std::vector<uint8_t> snapshot;              // bytes of [lo, hi) right after load
  std::vector<uint8_t> page_class;            // per host page: XEX_SECTION_* (0 = unknown)
  std::vector<uint8_t> diff_eligible;         // per host page: compare against the snapshot
  std::unique_ptr<std::atomic<uint8_t>[]> watched;  // per host page: 1 = protected right now
  std::unique_ptr<std::atomic<uint8_t>[]> trapped;  // per host page: 1 = trapped at least once
  std::unique_ptr<std::atomic<uint8_t>[]> hot;      // per host page: 1 = belongs to a hot range
  HotRange hot_ranges[kMaxHotRanges];
  size_t hot_range_count = 0;
  TrapRecord ring[kRingSize];
  std::atomic<uint64_t> ring_head{0};
  uint64_t ring_tail = 0;  // consumer side, render thread only
  std::atomic<uint64_t> traps_total{0};
  std::atomic<uint64_t> traps_dropped{0};
  std::atomic<uint32_t> rearm_pending{0};
  uint64_t frames_seen = 0;
  uint64_t last_rearm_frame = 0;
  uint64_t last_diff_frame = 0;
  std::string last_diff_text;
  std::atomic<uint64_t> diff_now_count{0};
  std::mutex diff_mutex;
  uint64_t snapshot_hash = 0;
  std::string mode;
};

State g;
thread_local bool tl_in_handler = false;

const char* ClassName(uint8_t c) {
  switch (c) {
    case rex::XEX_SECTION_CODE:
      return "CODE";
    case rex::XEX_SECTION_DATA:
      return "DATA";
    case rex::XEX_SECTION_READONLY_DATA:
      return "READONLY_DATA";
    default:
      return "unclassified";
  }
}

uint32_t PageFloor(uint32_t a) { return a & ~(g.host_page - 1); }

size_t PageIndex(uint32_t guest) { return size_t(guest - g.lo) / g.host_page; }

uint64_t Fnv1a64(const uint8_t* p, size_t n) {
  uint64_t h = 1469598103934665603ull;
  for (size_t i = 0; i < n; ++i) {
    h ^= p[i];
    h *= 1099511628211ull;
  }
  return h;
}

uint32_t LoadBE32(uint32_t guest) {
  const uint8_t* p = g.base + guest;
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

// PowerPC keeps the previous frame pointer at 0(r1) and that frame's return
// address at -8 from it. Bounded and range-checked: r1 may be the thing that
// went wrong. Reads only committed guest memory (the stack is), through the
// same host mapping the guest uses, so it is safe in the fault handler.
uint32_t WalkBackchain(uint32_t r1, uint32_t* out, uint32_t max) {
  uint32_t n = 0;
  uint32_t sp = r1;
  for (int depth = 0; depth < int(max) && sp != 0; ++depth) {
    if (sp < 0x10000u || sp > 0xFFFFFFF0u || (sp & 3u) != 0) break;
    const uint32_t next = LoadBE32(sp);
    if (next <= sp || next == 0 || next > 0xFFFFFFF0u || (next & 3u) != 0) break;
    const uint32_t ret = LoadBE32(next - 8);
    if (ret == 0) break;
    out[n++] = ret;
    sp = next;
  }
  return n;
}

void HostThreadName(char* name, size_t len) {
#if defined(__linux__)
  (void)len;
  name[0] = 0;
  prctl(PR_GET_NAME, reinterpret_cast<unsigned long>(name), 0, 0, 0);
#else
  pthread_getname_np(pthread_self(), name, len);
#endif
}

uint64_t HostTid() {
#if defined(__linux__)
  return uint64_t(syscall(SYS_gettid));
#else
  uint64_t tid = 0;
  pthread_threadid_np(nullptr, &tid);
  return tid;
#endif
}

bool ProtectPage(uint32_t page_guest, int prot) {
  bool ok = mprotect(g.base + page_guest, g.host_page, prot) == 0;
  // The alias view shares the backing; a store through it would bypass the
  // protection on the primary view.
  ok = (mprotect(g.base + page_guest + kAliasOffset, g.host_page, prot) == 0) && ok;
  return ok;
}

bool ArmPage(size_t idx) {
  const uint32_t page_guest = g.lo + uint32_t(idx) * g.host_page;
  if (!ProtectPage(page_guest, PROT_READ)) {
    return false;
  }
  g.watched[idx].store(1, std::memory_order_release);
  return true;
}

void DisarmPage(size_t idx) {
  const uint32_t page_guest = g.lo + uint32_t(idx) * g.host_page;
  ProtectPage(page_guest, PROT_READ | PROT_WRITE);
  g.watched[idx].store(0, std::memory_order_release);
}

// The fault handler. Runs in signal context on whichever thread stored into a
// protected page: syscalls, atomics, plain memory only. Any fault inside the
// watched span is ours - the span is committed image memory, so nothing else
// can fault there - and the answer is always "let the store through", after
// recording who made it the first time the page is hit.
bool ImageWatchHandler(rex::arch::Exception* ex, void*) {
  using Code = rex::arch::Exception::Code;
  if (ex->code() != Code::kAccessViolation) {
    return false;
  }
  if (!g.installed.load(std::memory_order_acquire) || g.base == nullptr) {
    return false;
  }
  const uint64_t fault = ex->fault_address();
  const uint64_t base = reinterpret_cast<uint64_t>(g.base);
  if (fault < base || fault - base >= 0x100000000ull) {
    return false;
  }
  uint32_t guest = uint32_t(fault - base);
  if (guest - 0x90000000u < 0x10000000u) {
    guest -= kAliasOffset;
  }
  if (guest < g.lo || guest >= g.hi) {
    return false;
  }
  if (tl_in_handler) {
    return false;  // a fault inside this handler: let the crash reporter have it
  }
  tl_in_handler = true;
  const size_t idx = PageIndex(guest);
  uint8_t was_armed = 1;
  if (g.watched[idx].compare_exchange_strong(was_armed, 0, std::memory_order_acq_rel)) {
    // First writer to this page since it was armed. Record before unprotecting,
    // while the bytes about to be overwritten are still the old ones.
    g.traps_total.fetch_add(1, std::memory_order_relaxed);
    const uint64_t seq = g.ring_head.fetch_add(1, std::memory_order_acq_rel);
    TrapRecord& r = g.ring[seq % kRingSize];
    uint32_t expected = 0;
    if (r.state.compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) {
      r.seq = seq;
      r.guest = guest;
      r.host_pc = ex->pc();
      r.tid = HostTid();
      HostThreadName(r.thread_name, sizeof(r.thread_name));
      r.thread_name[sizeof(r.thread_name) - 1] = 0;
      r.pre_addr = (guest & ~63u) >= g.lo ? (guest & ~63u) : g.lo;
      if (r.pre_addr + 64 > g.hi) r.pre_addr = g.hi - 64;
      std::memcpy(r.pre, g.base + r.pre_addr, sizeof(r.pre));
      r.has_guest_ctx = false;
      r.chain_len = 0;
      if (auto* ts = rex::runtime::ThreadState::Get(); ts != nullptr && ts->context() != nullptr) {
        const ::PPCContext& c = *ts->context();
        r.has_guest_ctx = true;
        r.guest_lr = c.lr;
        r.guest_r1 = c.r1.u32;
        r.chain_len = WalkBackchain(c.r1.u32, r.chain, kChainDepth);
      }
      r.state.store(2, std::memory_order_release);
    } else {
      g.traps_dropped.fetch_add(1, std::memory_order_relaxed);
    }
    g.trapped[idx].store(1, std::memory_order_release);
    ProtectPage(PageFloor(guest), PROT_READ | PROT_WRITE);
    if (g.hot[idx].load(std::memory_order_relaxed)) {
      g.rearm_pending.store(1, std::memory_order_release);
    }
  } else {
    // Another thread trapped on this page first and has already unprotected
    // it (or is about to). Retrying the store is the right answer either way.
  }
  tl_in_handler = false;
  return true;
}

// Called by the host file layer when pread() into a protected page fails with
// EFAULT. That read WAS going to land file bytes in static image data: exactly
// the kind of writer this watch exists to name. Log it, unprotect, retry.
bool OnHostBufferFault(void* buffer, size_t length, const char* path, size_t file_offset) {
  if (!g.installed.load(std::memory_order_acquire)) {
    return false;
  }
  const uint64_t b = reinterpret_cast<uint64_t>(buffer);
  const uint64_t base = reinterpret_cast<uint64_t>(g.base);
  if (b < base || b - base >= 0x100000000ull) {
    return false;
  }
  uint32_t guest = uint32_t(b - base);
  if (guest - 0x90000000u < 0x10000000u) guest -= kAliasOffset;
  const uint64_t end = uint64_t(guest) + length;
  if (end <= g.lo || guest >= g.hi) {
    return false;
  }
  REXLOG_ERROR(
      "image-watch: HOST FILE READ INTO STATIC IMAGE DATA: '{}' offset {} length {} lands at "
      "guest {:08X}..{:08X}. Unprotecting and letting it through; this is a writer",
      path ? path : "?", file_offset, length, guest, uint32_t(end));
  bool any = false;
  for (uint32_t p = PageFloor(guest < g.lo ? g.lo : guest); p < end && p < g.hi; p += g.host_page) {
    const size_t idx = PageIndex(p);
    if (g.watched[idx].load(std::memory_order_acquire)) {
      DisarmPage(idx);
      g.trapped[idx].store(1, std::memory_order_release);
      any = true;
    }
  }
  return any;
}

void HexDump(const uint8_t* p, size_t n, std::string& out) {
  char buf[4];
  for (size_t i = 0; i < n; ++i) {
    std::snprintf(buf, sizeof(buf), "%02X", p[i]);
    out += buf;
    if (i + 1 < n) out += (i % 16 == 15) ? " | " : " ";
  }
  out += "  |";
  for (size_t i = 0; i < n; ++i) {
    const char c = char(p[i]);
    out += (c >= 0x20 && c < 0x7F) ? c : '.';
  }
  out += '|';
}

std::string ModuleOffset(uint64_t pc) {
  Dl_info info;
  if (pc != 0 && dladdr(reinterpret_cast<void*>(pc), &info) != 0 && info.dli_fbase != nullptr) {
    const char* file = info.dli_fname ? info.dli_fname : "?";
    const char* slash = std::strrchr(file, '/');
    char buf[160];
    std::snprintf(buf, sizeof(buf), "%s+0x%llx", slash ? slash + 1 : file,
                  static_cast<unsigned long long>(pc - reinterpret_cast<uint64_t>(info.dli_fbase)));
    return buf;
  }
  return "?";
}

std::string ChainText(const uint32_t* chain, uint32_t n) {
  std::string s;
  char buf[16];
  for (uint32_t i = 0; i < n; ++i) {
    std::snprintf(buf, sizeof(buf), " %08X", chain[i]);
    s += buf;
  }
  return s.empty() ? " (none)" : s;
}

HotRange* HotRangeFor(uint32_t guest) {
  for (size_t i = 0; i < g.hot_range_count; ++i) {
    HotRange& h = g.hot_ranges[i];
    if (PageFloor(guest) < PageFloor(h.hi - 1) + g.host_page && PageFloor(guest) >= PageFloor(h.lo)) {
      return &h;
    }
  }
  return nullptr;
}

void LogTrap(const TrapRecord& r, bool hot) {
  std::string pre;
  HexDump(r.pre, sizeof(r.pre), pre);
  const size_t idx = PageIndex(r.guest);
  REXLOG_ERROR(
      "image-watch: WRITE into static image at {:08X} (page {:08X}, {}{}) from thread '{}' tid "
      "{}; host pc {:#x} ({}); guest {}; bytes before the write @{:08X}: {}",
      r.guest, PageFloor(r.guest), ClassName(g.page_class[idx]), hot ? ", hot" : "",
      r.thread_name[0] ? r.thread_name : "?", r.tid, r.host_pc, ModuleOffset(r.host_pc),
      r.has_guest_ctx ? fmt::format("lr {:08X} r1 {:08X} backchain:{}", uint32_t(r.guest_lr),
                                    r.guest_r1, ChainText(r.chain, r.chain_len))
                      : std::string("thread has no guest context - host code"),
      r.pre_addr, pre);
}

void LogHotWriters(HotRange& h, const char* why) {
  if (h.writer_count == 0) {
    REXLOG_ERROR("image-watch: hot range {:08X}..{:08X} changed ({}) but no write into its page "
                 "was trapped - the page was writable at the time (before Install, or between a "
                 "trap and the next re-arm)",
                 h.lo, h.hi, why);
    return;
  }
  REXLOG_ERROR("image-watch: hot range {:08X}..{:08X} changed ({}); {} trap(s) on its page, the "
               "last {} writers, newest first:",
               h.lo, h.hi, why, h.traps, std::min<uint32_t>(h.writer_count, kHotHistory));
  const uint32_t n = std::min<uint32_t>(h.writer_count, kHotHistory);
  for (uint32_t i = 0; i < n; ++i) {
    const HotWriter& w = h.writers[(h.writer_count - 1 - i) % kHotHistory];
    REXLOG_ERROR("image-watch:   #{} wrote {:08X} from '{}' host pc {:#x} ({}) guest lr {:08X} "
                 "backchain:{}",
                 w.seq, w.guest, w.thread_name[0] ? w.thread_name : "?", w.host_pc,
                 ModuleOffset(w.host_pc), uint32_t(w.guest_lr), ChainText(w.chain, w.chain_len));
  }
}

// Compare the snapshot with the live image over the diff-eligible pages.
// Returns the number of differing ranges; fills `text` with the report.
size_t Diff(std::string& text, bool* hot_changed) {
  size_t ranges = 0;
  *hot_changed = false;
  const uint8_t* live = g.base + g.lo;
  const uint8_t* snap = g.snapshot.data();
  const size_t span = g.snapshot.size();
  size_t i = 0;
  while (i < span) {
    const size_t idx = i / g.host_page;
    if (!g.diff_eligible[idx]) {
      i = (idx + 1) * g.host_page;
      continue;
    }
    const size_t page_end = std::min(span, (idx + 1) * g.host_page);
    if (std::memcmp(live + i, snap + i, page_end - i) == 0) {
      i = page_end;
      continue;
    }
    // Find the differing runs inside this page, merging gaps of <= 16 bytes.
    size_t j = i;
    while (j < page_end) {
      if (live[j] == snap[j]) {
        ++j;
        continue;
      }
      size_t k = j;
      size_t last_diff = j;
      while (k < page_end && (k - last_diff) <= 16) {
        if (live[k] != snap[k]) last_diff = k;
        ++k;
      }
      const uint32_t addr = g.lo + uint32_t(j);
      const uint32_t len = uint32_t(last_diff + 1 - j);
      ++ranges;
      for (size_t h = 0; h < g.hot_range_count; ++h) {
        if (addr < g.hot_ranges[h].hi && addr + len > g.hot_ranges[h].lo) *hot_changed = true;
      }
      if (ranges <= 16) {
        std::string now, was;
        const size_t n = std::min<size_t>(len, 64);
        HexDump(live + j, n, now);
        HexDump(snap + j, n, was);
        text += fmt::format("\n  {:08X} +{} ({} page)\n      now {}\n      was {}", addr, len,
                            ClassName(g.page_class[idx]), now, was);
      }
      j = last_diff + 1;
    }
    i = page_end;
  }
  return ranges;
}

void RunDiff(const char* why) {
  if (!g.installed.load(std::memory_order_acquire) || !REXCVAR_GET(skate3_image_watch_diff)) {
    return;
  }
  std::lock_guard<std::mutex> lock(g.diff_mutex);
  std::string text;
  bool hot_changed = false;
  const size_t ranges = Diff(text, &hot_changed);
  if (ranges == 0) {
    REXLOG_WARN("image-watch: diff ({}): 0 differing ranges; the static image still matches "
                "its load-time snapshot ({} page(s) trapped so far, {} record(s) dropped)",
                why, [] {
                  uint64_t n = 0;
                  for (size_t i = 0; i < g.page_count; ++i) n += g.trapped[i].load();
                  return n;
                }(),
                g.traps_dropped.load());
    g.last_diff_text.clear();
    return;
  }
  if (text == g.last_diff_text) {
    REXLOG_ERROR("image-watch: diff ({}): {} differing range(s), unchanged since the previous "
                 "report",
                 why, ranges);
    return;
  }
  g.last_diff_text = text;
  REXLOG_ERROR("image-watch: diff ({}): {} differing range(s) between the live static image and "
               "its load-time snapshot{}:{}",
               why, ranges, ranges > 16 ? " (first 16 shown)" : "", text);
  if (hot_changed) {
    for (size_t h = 0; h < g.hot_range_count; ++h) {
      LogHotWriters(g.hot_ranges[h], why);
    }
  }
}

bool ParseHex(const std::string& s, uint32_t* out) {
  if (s.empty()) return false;
  char* end = nullptr;
  const unsigned long v = std::strtoul(s.c_str(), &end, 16);
  if (end == s.c_str() || *end != 0) return false;
  *out = uint32_t(v);
  return true;
}

std::vector<std::string> Split(const std::string& s, char sep) {
  std::vector<std::string> out;
  size_t start = 0;
  while (start <= s.size()) {
    size_t e = s.find(sep, start);
    if (e == std::string::npos) e = s.size();
    std::string part = s.substr(start, e - start);
    // trim
    while (!part.empty() && (part.front() == ' ' || part.front() == '\t')) part.erase(0, 1);
    while (!part.empty() && (part.back() == ' ' || part.back() == '\t')) part.pop_back();
    if (!part.empty()) out.push_back(part);
    start = e + 1;
  }
  return out;
}

}  // namespace

bool Installed() { return g.installed.load(std::memory_order_acquire); }

void Install() {
  if (g.installed.load()) {
    return;
  }
  if (!REXCVAR_GET(skate3_image_watch)) {
    REXLOG_WARN("image-watch: off (skate3_image_watch=false)");
    return;
  }
  g.mode = REXCVAR_GET(skate3_image_watch_mode);
  auto* runtime = rex::Runtime::instance();
  if (runtime == nullptr || runtime->memory() == nullptr || runtime->kernel_state() == nullptr) {
    REXLOG_WARN("image-watch: no runtime at install time; not installed");
    return;
  }
  auto module = runtime->kernel_state()->GetExecutableModule();
  auto* xex = module ? module->xex_module() : nullptr;
  if (xex == nullptr) {
    REXLOG_WARN("image-watch: no executable module at install time; not installed");
    return;
  }
  g.base = runtime->memory()->virtual_membase();
  g.host_page = uint32_t(rex::memory::page_size());
  if (g.host_page == 0 || (g.host_page & (g.host_page - 1)) != 0) g.host_page = 4096;

  const uint32_t image_base = xex->base_address();
  const uint32_t image_end = image_base + xex->image_size();
  const uint32_t code_base = uint32_t(REX_CODE_BASE);
  auto* heap = runtime->memory()->LookupHeap(image_base);
  const uint32_t heap_page = heap ? heap->page_size() : 0x10000u;

  // Classify the image by the XEX page descriptors (64 KB granularity).
  struct Range {
    uint32_t lo, hi;
    uint8_t cls;
  };
  std::vector<Range> classes;
  const auto* sec = xex->xex_security_info();
  {
    uint32_t page = 0;
    for (uint32_t i = 0; sec && i < sec->page_descriptor_count; ++i) {
      rex::xex2_page_descriptor desc;
      desc.value = rex::byte_swap(sec->page_descriptors[i].value);
      const uint32_t start = image_base + page * heap_page;
      const uint32_t end = start + desc.page_count * heap_page;
      classes.push_back({start, end, uint8_t(desc.info)});
      page += desc.page_count;
    }
  }
  auto class_of = [&](uint32_t a) -> uint8_t {
    for (const Range& r : classes) {
      if (a >= r.lo && a < r.hi) return r.cls;
    }
    return 0;
  };

  // Hot ranges.
  g.hot_range_count = 0;
  for (const std::string& part : Split(REXCVAR_GET(skate3_image_watch_hot), ',')) {
    const size_t colon = part.find(':');
    uint32_t a = 0, len = 0x40;
    if (!ParseHex(part.substr(0, colon), &a)) continue;
    if (colon != std::string::npos && !ParseHex(part.substr(colon + 1), &len)) continue;
    if (g.hot_range_count < kMaxHotRanges && a >= image_base && a + len <= image_end) {
      g.hot_ranges[g.hot_range_count].lo = a;
      g.hot_ranges[g.hot_range_count].hi = a + len;
      ++g.hot_range_count;
    }
  }

  // The watched span: data mode covers everything below the code; rodata
  // mode covers the read-only descriptors; both always include the hot ranges.
  uint32_t lo = UINT32_MAX, hi = 0;
  const bool data_mode = g.mode == "data";
  const bool protect = g.mode != "off";
  if (data_mode) {
    lo = image_base;
    hi = code_base > image_base && code_base <= image_end ? code_base : image_end;
  } else {
    for (const Range& r : classes) {
      if (r.cls == rex::XEX_SECTION_READONLY_DATA) {
        lo = std::min(lo, r.lo);
        hi = std::max(hi, r.hi);
      }
    }
  }
  for (size_t h = 0; h < g.hot_range_count; ++h) {
    lo = std::min(lo, PageFloor(g.hot_ranges[h].lo));
    hi = std::max(hi, PageFloor(g.hot_ranges[h].hi - 1) + g.host_page);
  }
  if (lo >= hi) {
    REXLOG_WARN("image-watch: nothing to watch (mode '{}', {} descriptors); not installed", g.mode,
                classes.size());
    return;
  }
  lo = PageFloor(lo);
  hi = PageFloor(hi - 1) + g.host_page;
  g.lo = lo;
  g.hi = hi;
  g.page_count = size_t(hi - lo) / g.host_page;

  // Snapshot and hash.
  g.snapshot.assign(g.base + lo, g.base + hi);
  g.snapshot_hash = Fnv1a64(g.snapshot.data(), g.snapshot.size());
  g.page_class.assign(g.page_count, 0);
  g.diff_eligible.assign(g.page_count, 0);
  g.watched.reset(new std::atomic<uint8_t>[g.page_count]);
  g.trapped.reset(new std::atomic<uint8_t>[g.page_count]);
  g.hot.reset(new std::atomic<uint8_t>[g.page_count]);
  for (size_t i = 0; i < g.page_count; ++i) {
    g.watched[i].store(0);
    g.trapped[i].store(0);
    g.hot[i].store(0);
    const uint32_t a = lo + uint32_t(i) * g.host_page;
    g.page_class[i] = class_of(a);
  }
  for (size_t h = 0; h < g.hot_range_count; ++h) {
    for (uint32_t p = PageFloor(g.hot_ranges[h].lo); p < g.hot_ranges[h].hi; p += g.host_page) {
      g.hot[PageIndex(p)].store(1);
    }
  }

  // Which pages to protect: rodata pages (rodata mode) or every non-code page
  // (data mode), plus hot pages, minus the allow-list. The same set is diffed,
  // except that in data mode a DATA page that has trapped is legitimately
  // different and is skipped by the diff (see FlushPending).
  std::vector<uint8_t> to_protect(g.page_count, 0);
  for (size_t i = 0; i < g.page_count; ++i) {
    const uint8_t cls = g.page_class[i];
    bool want = g.hot[i].load() != 0;
    if (data_mode) {
      want = want || cls != rex::XEX_SECTION_CODE;
    } else {
      want = want || cls == rex::XEX_SECTION_READONLY_DATA;
    }
    to_protect[i] = want ? 1 : 0;
    g.diff_eligible[i] = want ? 1 : 0;
  }
  size_t allowed = 0;
  for (const std::string& part : Split(REXCVAR_GET(skate3_image_watch_allow), ',')) {
    uint32_t a = 0;
    if (ParseHex(part, &a) && a >= lo && a < hi) {
      to_protect[PageIndex(a)] = 0;
      g.diff_eligible[PageIndex(a)] = 0;
      ++allowed;
    }
  }

  // Register the handler before arming anything, then arm.
  rex::arch::ExceptionHandler::Install(ImageWatchHandler, nullptr);
  rex::filesystem::SetHostBufferFaultHook(OnHostBufferFault);
  g.installed.store(true, std::memory_order_release);
  size_t armed = 0, failed = 0, ro_pages = 0, data_pages = 0;
  for (size_t i = 0; i < g.page_count; ++i) {
    if (g.page_class[i] == rex::XEX_SECTION_READONLY_DATA) ++ro_pages;
    if (g.page_class[i] == rex::XEX_SECTION_DATA) ++data_pages;
    if (!protect || !to_protect[i]) continue;
    if (ArmPage(i)) {
      ++armed;
    } else {
      ++failed;
    }
  }

  // Where does the codec table live? This is the open question the reports
  // could not answer: a READONLY_DATA page has no legitimate writer at all.
  std::string table_where;
  {
    const uint32_t probe = g.hot_range_count ? g.hot_ranges[0].lo : 0x8210A310u;
    const char* section = "?";
    for (const PESection& s : xex->pe_sections()) {
      const uint32_t s_lo = s.address < image_base ? image_base + s.address : s.address;
      if (probe >= s_lo && probe < s_lo + s.size) section = s.name;
    }
    table_where = fmt::format("{:08X} is in a {} page, PE section '{}'", probe,
                              ClassName(class_of(probe)), section);
  }
  REXLOG_WARN(
      "image-watch: installed mode={} span {:08X}-{:08X} ({} KB, {} host pages of {} bytes; {} "
      "READONLY_DATA, {} DATA), {} page(s) protected, {} failed, {} allow-listed, {} hot range(s); "
      "hash of the static image after load+patch = {:016X}; {}",
      g.mode, lo, hi, (hi - lo) / 1024, g.page_count, g.host_page, ro_pages, data_pages, armed,
      failed, allowed, g.hot_range_count, g.snapshot_hash, table_where);
}

void FlushPending() {
  if (!g.installed.load(std::memory_order_acquire)) {
    return;
  }
  ++g.frames_seen;
  const uint64_t head = g.ring_head.load(std::memory_order_acquire);
  // Consume in order; a record still being written is left for next frame.
  while (g.ring_tail < head) {
    TrapRecord& r = g.ring[g.ring_tail % kRingSize];
    if (r.state.load(std::memory_order_acquire) != 2 || r.seq != g.ring_tail) {
      if (r.state.load(std::memory_order_acquire) == 1) break;  // writer mid-flight
      // Overwritten before we got here (ring wrapped): count it and move on.
      if (r.seq != g.ring_tail) {
        g.traps_dropped.fetch_add(1, std::memory_order_relaxed);
        ++g.ring_tail;
        continue;
      }
      break;
    }
    const size_t idx = PageIndex(r.guest);
    const bool hot = g.hot[idx].load(std::memory_order_relaxed) != 0;
    if (hot) {
      if (HotRange* h = HotRangeFor(r.guest)) {
        HotWriter& w = h->writers[h->writer_count % kHotHistory];
        w.seq = r.seq;
        w.guest = r.guest;
        w.host_pc = r.host_pc;
        w.guest_lr = r.has_guest_ctx ? r.guest_lr : 0;
        w.chain_len = r.chain_len;
        std::memcpy(w.chain, r.chain, sizeof(w.chain));
        std::memcpy(w.thread_name, r.thread_name, sizeof(w.thread_name));
        ++h->writer_count;
        ++h->traps;
        // The first few writers to a hot page are worth seeing as they happen:
        // they say whether the page has a legitimate per-frame writer at all.
        if (h->traps <= 3 || REXCVAR_GET(skate3_image_watch_log_hot_traps)) {
          LogTrap(r, true);
        }
      } else {
        LogTrap(r, true);
      }
    } else {
      LogTrap(r, false);
      // In data mode a trapped DATA page is legitimately different from now on.
      if (g.page_class[idx] == rex::XEX_SECTION_DATA) g.diff_eligible[idx] = 0;
    }
    r.state.store(0, std::memory_order_release);
    ++g.ring_tail;
  }

  // Re-arm the hot pages so a later write is caught too.
  const int32_t rearm = REXCVAR_GET(skate3_image_watch_rearm_frames);
  if (rearm > 0 && g.mode != "off" && g.rearm_pending.load(std::memory_order_acquire) &&
      g.frames_seen - g.last_rearm_frame >= uint64_t(rearm)) {
    g.rearm_pending.store(0, std::memory_order_release);
    g.last_rearm_frame = g.frames_seen;
    for (size_t h = 0; h < g.hot_range_count; ++h) {
      for (uint32_t p = PageFloor(g.hot_ranges[h].lo); p < g.hot_ranges[h].hi; p += g.host_page) {
        const size_t idx = PageIndex(p);
        if (!g.watched[idx].load(std::memory_order_acquire)) ArmPage(idx);
      }
    }
  }
}

void Tick(uint64_t frames) {
  if (!g.installed.load(std::memory_order_acquire)) {
    return;
  }
  const uint64_t every = uint64_t(std::max(5, REXCVAR_GET(skate3_image_watch_diff_seconds))) * 60;
  if (frames - g.last_diff_frame < every && g.last_diff_frame != 0) {
    return;
  }
  g.last_diff_frame = frames;
  RunDiff("heartbeat");
}

void DiffNow(const char* why) {
  if (!g.installed.load(std::memory_order_acquire)) {
    return;
  }
  // First four, then powers of two.
  const uint64_t n = g.diff_now_count.fetch_add(1, std::memory_order_relaxed);
  if (n < 4 || (n & (n - 1)) == 0) {
    RunDiff(why);
  }
}

uint32_t RestoreFromSnapshot(uint32_t guest, uint32_t len) {
  if (!g.installed.load(std::memory_order_acquire) || guest < g.lo || len == 0 ||
      uint64_t(guest) + len > g.hi) {
    return 0;
  }
  const uint8_t* snap = g.snapshot.data() + (guest - g.lo);
  uint8_t* live = g.base + guest;
  uint32_t differing = 0;
  for (uint32_t i = 0; i < len; ++i) {
    if (live[i] != snap[i]) ++differing;
  }
  if (differing == 0) {
    return 0;
  }
  // Write through the protection without being recorded as a writer ourselves:
  // drop it for the pages involved, copy, put it back where it was armed.
  std::vector<size_t> rearm;
  for (uint32_t p = PageFloor(guest); p < guest + len; p += g.host_page) {
    const size_t idx = PageIndex(p);
    if (g.watched[idx].load(std::memory_order_acquire)) {
      ProtectPage(p, PROT_READ | PROT_WRITE);
      rearm.push_back(idx);
    }
  }
  std::memcpy(live, snap, len);
  for (size_t idx : rearm) {
    ProtectPage(g.lo + uint32_t(idx) * g.host_page, PROT_READ);
  }
  return differing;
}

}  // namespace skate3::image_watch
