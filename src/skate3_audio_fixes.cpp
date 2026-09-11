/**
 * @file        skate3_audio_fixes.cpp
 * @brief       Guest hooks for the rw::audio::core command-queue race
 *
 * The AYN Thor (Snapdragon 8 Gen 2) filled its log with
 *
 *   [cpu] Call to invalid or unregistered function at guest address 0xFFFDFFFF
 *         (guest lr=0x82B3CD64) | r3=00000006 r10=00000000 r11=FFFDFFFF
 *
 * at a steady 187.5 calls a second - which is 48000/256, one per Xbox audio
 * quantum - for twenty-three minutes, and then died writing to guest
 * 0x70000000 on the render thread.
 *
 * Reading the dump register-by-register: `lr` is inside sub_82B3CD38, which
 * builds an audio decoder from a format descriptor:
 *
 *   lwz r11,0(r3)   ; r3 = the FormatDesc
 *   mtctr r11
 *   bctrl           ; desc->GetSize(channels, &size)
 *
 * r3 at the fault is 6 only because the recompiled code has already loaded the
 * NEXT argument into it; the descriptor pointer is r10, and r10 is 0 in every
 * occurrence in every report. The guest reads *(0+0), which is the low guest
 * page holding 0xFFFDFFFF, and calls that.
 *
 * The descriptor is NULL because the lookup that produced it missed. Its
 * callers key the System's format registry by a four-character codec tag taken
 * from a table indexed by a byte on the player object:
 *
 *   sub_82B29018  key = table_8210A310[*(this+352)]   (only 0 and 1 are valid:
 *                                                      'P6L0', 'PFN0')
 *   sub_82B33D40  key = table_82119870[codec id]
 *
 * Every format the game ships, including those two, is registered at startup,
 * so a miss means the INDEX BYTE IS GARBAGE, not that a format is missing.
 *
 * That byte is written by sub_82B28B78 (the SetFormat command handler) out of
 * a 20-byte command record. Records are appended to the System's command
 * queue - buffer *(sys+48), write offset *(sys+204) - and run later on the
 * audio thread by sub_82B48530, which holds the System lock while it does it.
 *
 * sub_82B28A00, the append, takes NO lock. Worse, it publishes the new write
 * offset before it stores the record:
 *
 *   lwz  r9,204(r11)     ; offset
 *   stw  r7,204(r11)     ; PUBLISH offset + 20   <-- first
 *   stw  r6,0(r10)       ; then the handler
 *   stw  r3,4(r10)       ; then the object
 *   stfs f0,8(r10)       ; then the payload
 *
 * A consumer that observes the advanced offset before those stores land runs a
 * record built from whatever the buffer held - typically a stale heap pointer
 * like 0x40C21870, which read as a float and truncated to an integer gives the
 * small values (6, 5, 1) the reports show. The format byte then lands outside
 * {0,1}, the lookup misses forever, and the player retries every audio frame.
 *
 * On the in-order Xenon the window between those stores is a few cycles and
 * the two threads were on the same die. On an out-of-order ARM64 core at
 * 3.2 GHz it is wide enough to hit repeatedly. The recompiler honours the
 * PowerPC barriers (lwsync becomes a seq_cst fence) - there simply are none on
 * this path to honour.
 *
 * sub_82B28A00 is not the only unlocked appender - sub_82B1E458,
 * sub_82B1E4F8, sub_82B1E780 and sub_82B1E8D8 all append the same way with no
 * lock, and any of them racing the drain corrupts the same queue. Only
 * sub_82B1DCD0 among the producers takes the lock. All five are wrapped below.
 * (sub_82B1EBA0 also appends, but it is the one-shot setup path behind an
 * "already initialised" flag, and it reaches its System through neither of the
 * two member paths, so it is left alone.)
 *
 * Nesting is safe: the guest critical section is recursive - see
 * RtlEnterCriticalSection_entry, which increments recursion_count when the
 * owning thread re-enters - and these hooks take the same lock the drain
 * already takes, so no new lock ordering is introduced.
 *
 * The hooks below:
 *   - the five appenders run under the System lock, exactly the lock the drain
 *     takes, which serialises them against it and against each other. The
 *     acquire NEVER blocks: these run on the game's own threads, and parking
 *     one behind an audio worker that Android stopped mid-callback would trade
 *     a glitch for a freeze. If the lock cannot be had, the append proceeds
 *     exactly as it does today.
 *   - sub_82B3CD38 refuses a NULL descriptor instead of calling through it.
 *     Its callers all handle a 0 return (sub_82B29018 stores 255 at +344 and
 *     reports failure), so the guest takes its own error path. If this one
 *     ever fires the log names the format byte, which says whether the race is
 *     really closed.
 */

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "generated/skate3_init.h"
#include "skate3_image_watch.h"

REXCVAR_DEFINE_BOOL(
    skate3_audio_repair_format_table, true, "Skate 3",
    "Put the two codec tags back when something has overwritten them.\n"
    "\n"
    "Two QCS8550 handhelds stop on a frontend screen and never load the world. "
    "The cause is upstream in audio: the table the codec lookup indexes has "
    "English text in it - 'd-forward and feed-back elements' - where a working "
    "device has 'P6L0' and 'PFN0'. Both devices load the same default.xex this "
    "build was compiled from and apply the title update successfully, so the "
    "table is correct when the image loads and wrong by the time audio starts.\n"
    "\n"
    "This is a mitigation and not a fix: the write that put a sentence there "
    "covered at least thirty-two bytes, this restores eight, and it will happen "
    "again. What it buys is a device that gets past the frontend and can report "
    "what else is broken. Turn it off to see the failure unrepaired.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace {

// System object layout, from sub_82B48530 (the queue drain) and sub_82B47E68
// (the constructor).
constexpr uint32_t kSysLockFn = 84;    // void (*)() - null on every build seen
constexpr uint32_t kSysUnlockFn = 88;  // unused: we only ever take the critical section
constexpr uint32_t kSysCritSec = 96;   // RTL_CRITICAL_SECTION*, the real lock

// PacketPlayer, from sub_82B28B78 (the SetFormat handler).
constexpr uint32_t kPlayerSystem = 8;      // System*
constexpr uint32_t kPlayerRate = 340;      // float
constexpr uint32_t kPlayerChannels = 351;  // u8
constexpr uint32_t kPlayerFormat = 352;    // u8, indexes the tag table below
constexpr uint32_t kFormatTagTable = 0x8210A310;
// How many entries of the tag table to PRINT. The table itself really does hold
// only two ('P6L0', 'PFN0'); a dump of the neighbourhood on a working device
// reads
//
//   table[0..7] = ['P6L0','PFN0','Pack','etPl','ayer','....','Pack','etPl']
//
// so entry 2 onwards is the string "PacketPlayer", not codec tags. The original
// two-entry assertion in this file was right, and a note here briefly claimed
// otherwise on the strength of the System's registry holding fourteen formats -
// but the registry is every format the System knows, while this table maps this
// player type's own two options. They are different things.
//
// Printing past the end is deliberate: an AYN Thor and a Retroid Pocket 6 both
// resolve index 1 to 'rwar' where this device resolves it to 'PFN0', and seeing
// the surrounding bytes says whether one entry was overwritten or the whole
// region belongs to a different image.
constexpr uint32_t kFormatTagReadable = 16;

// The two tags this table is supposed to hold, and a switch to put them back.
//
// A Retroid Pocket 6 and an AYN Thor read the table as
//
//   ['d-fo','rwar','d an','d fe','ed-b','ack ','elem','ents']
//
// which is the sentence "d-forward and feed-back elements" - filter
// terminology, so audio library text - sitting exactly where the codec tags
// belong. A healthy device reads ['P6L0','PFN0','Pack','etPl','ayer',...].
// So the 'rwar' those reports show was never a RenderWare four-character code;
// it is the letters inside the word "forward".
//
// Both devices pass the default.xex check and apply the title update
// successfully, so the image they load is the same file as the one this build
// was compiled from. The table is therefore right when it is loaded and wrong
// by the time audio starts: something writes over it at run time.
//
// Restoring it is a mitigation, not a cure. Whatever wrote that sentence wrote
// at least thirty-two bytes and this puts back eight; anything else it landed
// on stays broken, and the write will happen again. But the lookup that fails
// here is retried every audio quantum, so repairing the two entries lets the
// very next retry succeed, and a device that reaches the game is a device that
// can report what else is wrong. The log says loudly what was found.
constexpr uint32_t kFormatTag0 = 0x50364C30;  // 'P6L0'
constexpr uint32_t kFormatTag1 = 0x50464E30;  // 'PFN0'

std::atomic<uint64_t> g_table_repairs{0};

// True when the table no longer holds the two tags it is built with.
bool FormatTagTableIsCorrupt(uint8_t* base) {
  return REX_LOAD_U32(kFormatTagTable) != kFormatTag0 ||
         REX_LOAD_U32(kFormatTagTable + 4) != kFormatTag1;
}

// The byte the game ACTUALLY indexes the tag table with is not the one at
// +352. sub_82B29018 reads it from *(*(player + 80) + 4):
//
//   lwz  r30,80(r3)      ; the stream/source object
//   lbz  r10,4(r30)      ; the codec index
//   lwzx r9,r7,r8        ; table_8210A310[index]
//
// sub_82B28B78 writes both from the same command record, so they agree while
// the queue is intact and disagree exactly when it is not. An AYN Thor report
// showed the +352 copy reading 1 and resolving to tag 'rwar', which is not one
// of the two formats this title registers - so the report could not say
// whether the byte was wrong or the registry was short. Printing both, plus
// the registry itself, separates those two for the next report.
constexpr uint32_t kPlayerSource = 80;   // -> the object holding the codec index
constexpr uint32_t kSourceFormat = 4;    // u8, the index actually used

// The System's format registry: sub_82B488D0 stores it at System+60 and
// sub_82B48840 hands it back. Its list head is at +0; each link p is preceded
// by its descriptor (desc = p - 16), whose four-character codec tag lives at
// desc + 20, i.e. p + 4. sub_82B29018 walks exactly this.
constexpr uint32_t kSysFormatRegistry = 60;
constexpr uint32_t kRegistryTagFromLink = 4;
constexpr int kRegistryWalkMax = 32;  // a title with 2 entries; a cap, not a size

// The return address inside sub_82B29018, i.e. "this call came from
// PacketPlayer::CreateDecoder", which is the only caller whose object fields
// are worth printing.
constexpr uint32_t kCreateDecoderReturn = 0x82B290AC;

// How many times to try for the lock before giving up and appending without
// it. The append is a dozen stores and the drain holds the lock only for the
// commands already queued, so a handful of attempts is far more than a live
// holder ever needs.
//
// This was 256. The uncontended case takes the lock on the first attempt and
// never yields, so that number only ever showed up under real contention -
// but there it is 256 yields, and each yield is a syscall, on one of the
// game's own threads. Falling through unlocked is safe by design (see above),
// so spinning long enough to outlast a stalled holder buys nothing: it just
// makes the bad case more expensive than the fallback it is avoiding.
constexpr int kLockAttempts = 16;

// Take the System lock WITHOUT ever blocking, and say whether we got it.
//
// Blocking here would be a new way to freeze the whole game. These appends run
// on the game's own threads, which never touched this lock before; if the
// audio worker is stopped while holding it - which is exactly what happens
// when Android suspends the app mid-callback - a blocking acquire would park
// the game thread behind a holder that is not running. Falling through
// unlocked is precisely today's behaviour, so the worst case is the bug we
// are fixing, not a hang.
//
// The custom lock at +84 is null on this title. If some build ever sets it we
// cannot try-acquire a guest function, so leave that path alone rather than
// risk blocking on it.
bool SystemTryLock(PPCContext& __restrict ctx, uint8_t* base, uint32_t sys) {
  if (REX_LOAD_U32(sys + kSysLockFn) != 0) {
    return false;
  }
  const uint32_t cs = REX_LOAD_U32(sys + kSysCritSec);
  if (cs == 0) {
    return false;
  }
  for (int attempt = 0; attempt < kLockAttempts; ++attempt) {
    ctx.r3.u64 = cs;
    __imp__RtlTryEnterCriticalSection(ctx, base);
    if (ctx.r3.u32 != 0) {
      return true;  // recursion is handled inside; one release balances this
    }
    std::this_thread::yield();
  }
  return false;
}

void SystemUnlock(PPCContext& __restrict ctx, uint8_t* base, uint32_t sys) {
  ctx.r3.u64 = REX_LOAD_U32(sys + kSysCritSec);
  __imp__RtlLeaveCriticalSection(ctx, base);
}

// Geometric, like the invalid-call trap in the dispatcher: if this fires at
// all it fires every audio frame, and a synchronous log write per frame is
// worse than the fault it reports.
bool ShouldLog(std::atomic<uint64_t>& counter, uint64_t* out_n) {
  const uint64_t n = counter.fetch_add(1, std::memory_order_relaxed);
  *out_n = n + 1;
  return n < 8 || (n & (n - 1)) == 0;
}

std::atomic<uint64_t> g_null_desc{0};
std::atomic<uint64_t> g_decoder_ok{0};
std::atomic<uint64_t> g_named_lookup_miss{0};
std::atomic<uint64_t> g_bad_buffer_size{0};

// Render the System's registered codec tags into a caller-supplied buffer, as
// "'P6L0','PFN0'". Every load is bounds-checked against the guest address
// space and the walk is capped, because this runs on a path that has already
// established that something is wrong.
void DescribeFormatRegistry(uint8_t* base, uint32_t sys, std::string& out) {
  out.clear();
  if (sys == 0) {
    out = "<no system>";
    return;
  }
  const uint32_t registry = REX_LOAD_U32(sys + kSysFormatRegistry);
  if (registry == 0) {
    out = "<registry null - none registered>";
    return;
  }
  uint32_t link = REX_LOAD_U32(registry);
  int n = 0;
  while (link >= 0x10000 && n < kRegistryWalkMax) {
    const uint32_t tag = REX_LOAD_U32(link + kRegistryTagFromLink);
    char quad[8] = {};
    for (int i = 0; i < 4; ++i) {
      const char c = char((tag >> (8 * (3 - i))) & 0xFF);
      quad[i] = (c >= 0x20 && c < 0x7F) ? c : '.';
    }
    if (!out.empty()) {
      out += ',';
    }
    out += '\'';
    out += quad;
    out += '\'';
    link = REX_LOAD_U32(link);
    ++n;
  }
  if (n == 0) {
    out = "<empty>";
  } else if (n >= kRegistryWalkMax) {
    out += ",...";
  }
}

// Render the first entries of the codec tag table itself.
//
// This is here because of a result that rules out everything easy. A Retroid
// Pocket 6 and an AYN Thor both resolve index 1 to 'rwar', which is not in the
// registry; the developer's phone resolves the SAME index, through the same
// static address, in the same binary, to 'PFN0', which is. And the diagnostic
// report now proves the inputs are identical - size and hash of default.xex,
// default.xexp, EAWebkit.xexp and all three .mus stream files match byte for
// byte. Same code, same data, different value read from the same address.
//
// That leaves the table's CONTENT differing at run time, so print it. If only
// one entry is wrong, something wrote over it - and 'rwar' carries RenderWare's
// own two-letter prefix, so the writer is likely rw::audio itself. If the whole
// table is wrong, it was never initialised on these devices. The two need
// different fixes, and one line of log separates them.
void DescribeFormatTagTable(uint8_t* base, std::string& out) {
  out.clear();
  for (uint32_t i = 0; i < 8; ++i) {
    const uint32_t tag = REX_LOAD_U32(kFormatTagTable + i * 4);
    char quad[8] = {};
    for (int k = 0; k < 4; ++k) {
      const char c = char((tag >> (8 * (3 - k))) & 0xFF);
      quad[k] = (c >= 0x20 && c < 0x7F) ? c : '.';
    }
    if (!out.empty()) {
      out += ',';
    }
    out += '\'';
    out += quad;
    out += '\'';
  }
}

// Volatile guest state the lock helpers may disturb. RtlEnterCriticalSection
// is a host implementation that only writes r3, but the +84/+88 function
// pointers - null in this build, honoured anyway - would run guest code, and
// guest code clobbers the whole volatile set.
struct VolatileState {
  uint64_t r[8];  // r3..r10
  uint64_t f[13];  // f1..f13
  uint64_t lr;
  uint64_t ctr;

  explicit VolatileState(const PPCContext& ctx) {
    r[0] = ctx.r3.u64; r[1] = ctx.r4.u64; r[2] = ctx.r5.u64; r[3] = ctx.r6.u64;
    r[4] = ctx.r7.u64; r[5] = ctx.r8.u64; r[6] = ctx.r9.u64; r[7] = ctx.r10.u64;
    f[0] = ctx.f1.u64; f[1] = ctx.f2.u64; f[2] = ctx.f3.u64; f[3] = ctx.f4.u64;
    f[4] = ctx.f5.u64; f[5] = ctx.f6.u64; f[6] = ctx.f7.u64; f[7] = ctx.f8.u64;
    f[8] = ctx.f9.u64; f[9] = ctx.f10.u64; f[10] = ctx.f11.u64; f[11] = ctx.f12.u64;
    f[12] = ctx.f13.u64;
    lr = ctx.lr;
    ctr = ctx.ctr.u64;
  }

  void Restore(PPCContext& ctx) const {
    ctx.r3.u64 = r[0]; ctx.r4.u64 = r[1]; ctx.r5.u64 = r[2]; ctx.r6.u64 = r[3];
    ctx.r7.u64 = r[4]; ctx.r8.u64 = r[5]; ctx.r9.u64 = r[6]; ctx.r10.u64 = r[7];
    ctx.f1.u64 = f[0]; ctx.f2.u64 = f[1]; ctx.f3.u64 = f[2]; ctx.f4.u64 = f[3];
    ctx.f5.u64 = f[4]; ctx.f6.u64 = f[5]; ctx.f7.u64 = f[6]; ctx.f8.u64 = f[7];
    ctx.f9.u64 = f[8]; ctx.f10.u64 = f[9]; ctx.f11.u64 = f[10]; ctx.f12.u64 = f[11];
    ctx.f13.u64 = f[12];
    ctx.lr = lr;
    ctx.ctr.u64 = ctr;
  }
};

// System* at *(this + 8).
inline uint32_t SysFromMember8(uint8_t* base, uint32_t self) {
  return self != 0 ? REX_LOAD_U32(self + kPlayerSystem) : 0;
}

// System* at *(*(this + 4) + 16), which is how sub_82B1E458 reaches it.
inline uint32_t SysFromOwner16(uint8_t* base, uint32_t self) {
  if (self == 0) return 0;
  const uint32_t owner = REX_LOAD_U32(self + 4);
  return owner != 0 ? REX_LOAD_U32(owner + 16) : 0;
}

}  // namespace

// Take the System lock, run the original with its arguments intact, release,
// and hand back the original's return value.
//
// Each appender reaches its System differently - most from *(this+8),
// sub_82B1E458 from *(*(this+4)+16) - so the caller resolves it. A zero
// anywhere along the way means there is no queue to protect, and the hook
// steps aside rather than guessing.
//
// Written out one by one rather than generated from a macro because
// tools/gen_hooked_funcs.sh finds hooks by grepping for the literal
// REX_FUNC(sub_...) and __imp__sub_... spellings; a macro would hide them and
// the override would silently not happen.
namespace {

void RunUnderSystemLock(PPCContext& __restrict ctx, uint8_t* base, uint32_t sys,
                        PPCFunc* original) {
  if (sys == 0) {
    original(ctx, base);
    return;
  }
  const VolatileState saved(ctx);
  const bool locked = SystemTryLock(ctx, base, sys);
  saved.Restore(ctx);

  original(ctx, base);

  if (locked) {
    const VolatileState returned(ctx);
    SystemUnlock(ctx, base, sys);
    returned.Restore(ctx);
  }
}

}  // namespace

extern "C" REX_FUNC(sub_82B28A00) {
  RunUnderSystemLock(ctx, base, SysFromMember8(base, ctx.r3.u32), __imp__sub_82B28A00);
}

extern "C" REX_FUNC(sub_82B1E4F8) {
  RunUnderSystemLock(ctx, base, SysFromMember8(base, ctx.r3.u32), __imp__sub_82B1E4F8);
}

extern "C" REX_FUNC(sub_82B1E780) {
  RunUnderSystemLock(ctx, base, SysFromMember8(base, ctx.r3.u32), __imp__sub_82B1E780);
}

extern "C" REX_FUNC(sub_82B1E8D8) {
  RunUnderSystemLock(ctx, base, SysFromMember8(base, ctx.r3.u32), __imp__sub_82B1E8D8);
}

extern "C" REX_FUNC(sub_82B1E458) {
  RunUnderSystemLock(ctx, base, SysFromOwner16(base, ctx.r3.u32), __imp__sub_82B1E458);
}

// rw::audio::core decoder factory: r3 = FormatDesc, r4 = channels, r5 = rate.
// A NULL descriptor here means a format lookup missed; calling through it
// reads guest address 0 and dispatches to whatever that page holds.
extern "C" REX_FUNC(sub_82B3CD38) {
  // A null descriptor means the tag lookup missed. Before blaming the caller,
  // check whether the table it searched still holds the tags this build was
  // compiled with - on two devices it holds a sentence instead. Repairing it
  // here works because this call is retried every audio quantum: the next
  // retry indexes a table that is right again.
  if (ctx.r3.u32 == 0 && FormatTagTableIsCorrupt(base)) {
    std::string found;
    DescribeFormatTagTable(base, found);
    const bool repair = REXCVAR_GET(skate3_audio_repair_format_table);
    uint64_t n = 0;
    if (ShouldLog(g_table_repairs, &n)) {
      REXLOG_ERROR(
          "skate3-audio: the codec tag table at {:08X} has been overwritten "
          "(occurrence {}). Found [{}]; this build was compiled with 'P6L0' and "
          "'PFN0' at [0] and [1]. Something wrote over static image data at run "
          "time - the executable itself matches, and the title update applied. "
          "{}",
          kFormatTagTable, n, found,
          repair ? "Putting both entries back so the next retry can resolve; "
                   "whatever else that write covered is still wrong."
                 : "Leaving it as found (skate3_audio_repair_format_table=false).");
    }
    if (repair) {
      // Show the whole damage first, then put the neighbourhood back from the
      // copy taken right after load: the two tags AND the two "PacketPlayer"
      // strings after them, which the player constructor hands out as names.
      skate3::image_watch::DiffNow("codec-miss");
      const uint32_t restored = skate3::image_watch::RestoreFromSnapshot(kFormatTagTable, 0x40);
      if (skate3::image_watch::Installed() && restored == 0) {
        // The table is corrupt AND matches the snapshot: the image was already
        // wrong when it finished loading. That is the loader, not a writer.
        REXLOG_ERROR(
            "skate3-audio: the load-time snapshot ALREADY holds these bytes at {:08X} - the "
            "image was wrong when loading finished, before any guest code ran. Compare the "
            "'hash of the static image after load+patch' line against a working device",
            kFormatTagTable);
      } else if (restored != 0) {
        REXLOG_ERROR("skate3-audio: restored {} byte(s) of {:08X}..{:08X} from the load-time "
                     "snapshot",
                     restored, kFormatTagTable, kFormatTagTable + 0x40);
      }
      if (REX_LOAD_U32(kFormatTagTable) != kFormatTag0 ||
          REX_LOAD_U32(kFormatTagTable + 4) != kFormatTag1) {
        REX_STORE_U32(kFormatTagTable, kFormatTag0);
        REX_STORE_U32(kFormatTagTable + 4, kFormatTag1);
      }
    }
  }

  if (ctx.r3.u32 != 0) {
    // Baseline for the failure below: on a device where audio works, say what
    // the first few decoders actually asked for. A report that carries both
    // this and the failure names the difference directly instead of leaving
    // "is the byte wrong, or is the format missing" open for another round.
    uint64_t ok = 0;
    if (ShouldLog(g_decoder_ok, &ok) && ok <= 16 &&
        uint32_t(ctx.lr) == kCreateDecoderReturn && ctx.r31.u32 != 0) {
      const uint32_t player = ctx.r31.u32;
      const uint32_t source = REX_LOAD_U32(player + kPlayerSource);
      const uint32_t used = source != 0 ? REX_LOAD_U8(source + kSourceFormat) : 0xFFu;
      const uint32_t stored = REX_LOAD_U8(player + kPlayerFormat);
      uint32_t ok_tag = 0;
      if (used < kFormatTagReadable) {
        ok_tag = REX_LOAD_U32(kFormatTagTable + used * 4);
      }
      char ok_quad[8] = {};
      for (int i = 0; i < 4; ++i) {
        const char c = char((ok_tag >> (8 * (3 - i))) & 0xFF);
        ok_quad[i] = (c >= 0x20 && c < 0x7F) ? c : '.';
      }
      std::string registry;
      DescribeFormatRegistry(base, REX_LOAD_U32(player + kPlayerSystem), registry);
      std::string table;
      DescribeFormatTagTable(base, table);
      REXLOG_WARN(
          "skate3-audio: decoder OK (occurrence {}) player={:08X} format byte={} "
          "(stored copy {}), tag={:08X} '{}', channels={}, registry=[{}], "
          "table[0..7]=[{}]",
          ok, player, used, stored, ok_tag, static_cast<const char*>(ok_quad),
          REX_LOAD_U8(player + kPlayerChannels), registry, table);
    }
    __imp__sub_82B3CD38(ctx, base);
    return;
  }

  uint64_t n = 0;
  if (ShouldLog(g_null_desc, &n)) {
    const uint32_t lr = uint32_t(ctx.lr);
    if (lr == kCreateDecoderReturn && ctx.r31.u32 != 0) {
      // PacketPlayer::CreateDecoder: r31 is the player, so the index byte that
      // produced the miss can be printed. Anything but 0 or 1 is the race.
      const uint32_t player = ctx.r31.u32;
      const uint32_t format = REX_LOAD_U8(player + kPlayerFormat);
      // The index the lookup actually used, and the tag it actually resolved
      // to. The previous version of this line printed only the +352 copy and
      // only tags it could find in the two-entry table, so an out-of-range
      // index printed tag=00000000 and an in-range one printed a tag the
      // lookup may never have seen. Print what the game read.
      const uint32_t source = REX_LOAD_U32(player + kPlayerSource);
      const uint32_t used = source != 0 ? REX_LOAD_U8(source + kSourceFormat) : 0xFFu;
      const uint32_t channels = REX_LOAD_U8(player + kPlayerChannels);
      const uint32_t rate_bits = REX_LOAD_U32(player + kPlayerRate);
      float rate = 0.0f;
      __builtin_memcpy(&rate, &rate_bits, sizeof(rate));
      uint32_t tag = 0;
      if (used < kFormatTagReadable) {
        tag = REX_LOAD_U32(kFormatTagTable + used * 4);
      }
      char quad[8] = {};
      for (int i = 0; i < 4; ++i) {
        const char c = char((tag >> (8 * (3 - i))) & 0xFF);
        quad[i] = (c >= 0x20 && c < 0x7F) ? c : '.';
      }
      // What IS registered. If the tag the index resolves to appears here the
      // lookup should have hit, and the fault is in the walk, not the byte;
      // if it does not, the byte is garbage or the registry never got its
      // entries - and an empty registry says which.
      std::string registry;
      DescribeFormatRegistry(base, REX_LOAD_U32(player + kPlayerSystem), registry);
      std::string table;
      DescribeFormatTagTable(base, table);
      REXLOG_ERROR(
          "skate3-audio: decoder requested with a NULL format descriptor "
          "(occurrence {}), from PacketPlayer::CreateDecoder player={:08X}: "
          "format byte={} (used; stored copy {}), tag={:08X} '{}', "
          "channels={}, rate={}, registry=[{}], table[0..7]=[{}] - the tag is "
          "what the lookup searched for. The table is static data in the game "
          "image; if an entry here differs between devices running the same "
          "build and the same disc, that memory was written over at run time. "
          "Returning failure instead of calling through guest address 0",
          n, player, used, format, tag,
          static_cast<const char*>(quad), channels, rate, registry, table);
    } else {
      // The other lookup. sub_82B33D40 keys the same registry off a codec id
      // through a DIFFERENT table (0x82119870), and an AYN Thor report showed it
      // failing here too, with channels=1 and 5 and an r5 that is not a sample
      // rate. Two independent lookups missing the same registry is worth
      // separating from one bad index, so dump the registry on this path as
      // well - the System is reachable from r31 in this caller the same way.
      std::string registry;
      const uint32_t maybe_sys = ctx.r31.u32 != 0
                                     ? REX_LOAD_U32(ctx.r31.u32 + kPlayerSystem)
                                     : 0;
      DescribeFormatRegistry(base, maybe_sys, registry);
      REXLOG_ERROR(
          "skate3-audio: decoder requested with a NULL format descriptor "
          "(occurrence {}), guest lr={:08X}, r4={} r5={:08X}, r31={:08X}, "
          "registry=[{}] - returning failure instead of calling through guest "
          "address 0",
          n, lr, ctx.r4.u32, ctx.r5.u32, ctx.r31.u32, registry);
    }
  }

  ctx.r3.u64 = 0;
}

// rw::core named-object lookup: sub_82D19648(r3 = the collection, r4 = the
// name, r5, r6) walks a list calling sub_82D1B5B8 per entry and returns the
// match, or 0.
//
// This is here because of what a NULL return did on an AYN Thor. In
// sub_82B95620 the game formats a name into a stack buffer and looks it up:
//
//   sub_82861930(r1+80, 256, 256, fmt, *0x82FE1FF4)   ; snprintf
//   sub_82D19648(r3 = this, r4 = r1+80, ...)          ; find it
//   lwz r9,0(r3)                                      ; r3 is 0
//   lwz r8,20(r9)  /  mtctr r8  /  bctrl              ; call *(0 + 20)
//
// so the miss becomes "Call to invalid or unregistered function at guest
// address 0x00000000, guest lr=0x82B9568C", with r31 = 0 - which is the
// signature the Thor filled its log with, twice per run, ten seconds after the
// audio format lookup failed the same way. The report could name the fault but
// not the object, because the name never reached the log.
//
// The hook does nothing but read the name back on a miss. It is bounded to the
// first misses of a session: this lookup is generic and a miss is a legitimate
// answer for a caller that is only testing existence, so unbounded logging
// would be its own performance bug.
extern "C" REX_FUNC(sub_82D19648) {
  const uint32_t name_ptr = ctx.r4.u32;
  const uint32_t lr = uint32_t(ctx.lr);
  __imp__sub_82D19648(ctx, base);
  if (ctx.r3.u32 != 0) {
    return;
  }
  uint64_t n = 0;
  if (!ShouldLog(g_named_lookup_miss, &n) || n > 24) {
    return;
  }
  char name[96] = {};
  if (name_ptr >= 0x10000) {
    for (int i = 0; i < 95; ++i) {
      const char c = char(REX_LOAD_U8(name_ptr + uint32_t(i)));
      if (c == '\0') {
        break;
      }
      name[i] = (c >= 0x20 && c < 0x7F) ? c : '.';
    }
  }
  REXLOG_WARN(
      "skate3-audio: named lookup MISS (occurrence {}) name='{}' from guest "
      "lr={:08X} - the caller dereferences this result without checking it",
      n, static_cast<const char*>(name), lr);
}

// rw::audio buffer setup: sub_82B7F828(r3 = the descriptor, r4, r5,
// r6 = buffer, r7 = size in bytes).
//
// It records the buffer and its size into the descriptor and then zeroes the
// buffer:
//
//   stw r6,0(r3)      ; desc->buffer = r6
//   stw r7,8(r3)      ; desc->size   = r7
//   cmplwi cr6,r6,0
//   beq   cr6,0x82b7f870
//   mr r5,r7 / li r4,0 / mr r3,r6
//   bl 0x82f52040     ; memset(buffer, 0, size)
//
// THIS IS THE AYN THOR CRASH. Five tombstones across three builds, including
// v0.1.12, are all the same: SIGSEGV on `render_thread` four to seven minutes
// in, escalated to SIGABRT by ART's signal chain. The engine's own fault report
// names the site exactly - WRITE to guest 0x70000000, `guest lr=0x82B7F870`,
// which is the return address of that memset call, with `r5 = r7 = 0xFFFFFFF4`.
//
// The size is -12. sub_82F52040 is a normal PowerPC memset: it takes the count
// as UNSIGNED, computes a 16-byte block count of `size >> 4` - and the crash
// dump's `r0 = 0x0FFFFFFF` is exactly 0xFFFFFFF4 >> 4 - then stores its way
// through 4.29 GB of guest address space until it reaches memory that is
// reserved but not committed, and dies there. 0x70000000 is not a meaningful
// address; it is simply how far it got.
//
// A negative size cannot be legitimate: the argument is a byte count and the
// whole guest space is 4 GB. Clamping it to zero leaves the descriptor in a
// coherent state (a buffer of zero length, which its callers already handle -
// sub_82B7F8A8 tests this family of results for negative values before using
// them) and turns a session-ending crash into one logged line naming the
// caller, which is what the next report needs to find whoever computed -12.
extern "C" REX_FUNC(sub_82B7F828) {
  const int32_t size = int32_t(ctx.r7.u32);
  if (size < 0) {
    // Report the neighbouring arguments too. The caller, sub_82B7F8A8, gets
    // this size out of a struct that sub_82B7F998 fills, and it validates the
    // fields at +96 and +116 for negative values while never checking the one
    // that lands here. Printing r4/r5/r6 alongside says how far off the whole
    // set is, which is what identifies the parse that produced it.
    uint64_t n = 0;
    if (ShouldLog(g_bad_buffer_size, &n)) {
      REXLOG_ERROR(
          "skate3-audio: buffer setup asked to zero {} bytes (size={:08X}) at "
          "guest {:08X}, from lr={:08X} - a negative byte count. Clamping to 0; "
          "unclamped this is the memset that walks 4 GB of guest memory and "
          "kills the process. base={:08X} span={} total_buf={:08X} "
          "(occurrence {})",
          size, ctx.r7.u32, ctx.r6.u32, uint32_t(ctx.lr), ctx.r4.u32,
          int32_t(ctx.r5.u32), ctx.r4.u32, n);
    }
    ctx.r7.u64 = 0;
  }
  __imp__sub_82B7F828(ctx, base);
}
