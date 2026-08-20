#pragma once

// Observation-only instrumentation for the host automation path.
//
// Three rounds of fixes have been reasoned out from a model of what a host must
// be doing, and all three left the reported symptoms unchanged, which says the
// model is wrong rather than the fix. This records what the host actually does
// so the next round can be driven by observation.
//
// Nothing here changes behaviour. Every entry point is a pure sink: it reads
// values the surrounding code has already computed and appends them to a ring,
// and no decision anywhere in the plug-in consults anything defined here.
//
// It is off unless EFFETUNE_AUTOMATION_TRACE names a writable file. When it is
// unset -- the default, and how the user's DAW normally runs -- g_state stays
// null, no file is opened, nothing is allocated, and every call site is a
// not-taken branch on one already-loaded pointer.
//
// Thread rules:
//   * emit() is wait-free and allocation-free, so the audio thread may call it.
//     It claims a slot with one compare-exchange, writes a trivially copyable
//     record into it and publishes the slot. A full ring drops the record and
//     counts the drop rather than blocking.
//   * flush() writes the drained records to the file. It takes a mutex and
//     performs I/O, so only control threads may call it -- the ~50 ms control
//     service tick is what drives it.
//
// Environment:
//   EFFETUNE_AUTOMATION_TRACE        path of the file to append the trace to.
//                                    Unset or empty means no tracing at all.
//   EFFETUNE_AUTOMATION_TRACE_PARAM  optional decimal host parameter id. When
//                                    set, the per-block records -- input
//                                    queues, gesture and claim transitions, and
//                                    the value the DSP adopts -- are limited to
//                                    that parameter. Block records and the
//                                    host-facing edit records are always
//                                    emitted. Unset means trace everything.

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>

#include "version.h"

namespace effetune::vst::plugin::trace {

// What one record describes. The name is printed verbatim, so the log reads as
// a call log rather than as a table of numbers.
enum class Event : std::uint8_t {
  none = 0,
  // IEditController::setParamNormalized(), the host's editor-facing write.
  setParamNormalized,
  // The same call reaching a bound slot and adopting the value natively.
  setParamAdopted,
  // performHostEditTransaction(), entry and exit.
  txnBegin,
  txnEnd,
  // Calls we make into IComponentHandler / IComponentHandler2.
  hostBeginEdit,
  hostPerformEdit,
  hostEndEdit,
  hostStartGroupEdit,
  hostFinishGroupEdit,
  // One process() block and its transport context.
  block,
  // One input parameter queue in that block, then its points.
  queue,
  queuePoint,
  queueTruncated,
  // What the scheduler publishes for a slot once the block's intake is closed,
  // which is the value the DSP renders from.
  dspValue,
  // Gesture state transitions.
  touchOpened,
  touchClosed,
  // A value withheld from the host until the next process() boundary, and the
  // boundary -- or the gesture close that stood in for one -- releasing it.
  openHeld,
  openReleased,
  // IAutomationState, which is how a host says Read and Write are armed.
  automationState,
};

// Which thread issued the record. Call ordering across threads is the whole
// point of the log, so every line carries this.
enum class Role : std::uint8_t {
  host = 0,  // a host thread with no more specific role
  audio = 1, // inside process()
  timer = 2, // the ~50 ms control-service tick
  ui = 3,    // the WebView message handler
};

// Flag bits. Each event names its own; a record prints only the ones its event
// declares, so the same bit means different things on different lines.
inline constexpr std::uint16_t kFlagFinite = 1u << 0;      // setParamNormalized
inline constexpr std::uint16_t kFlagSuppressed = 1u << 1;  // setParamNormalized, queue
inline constexpr std::uint16_t kFlagBeginGesture = 1u << 0;  // txnBegin
inline constexpr std::uint16_t kFlagEndGesture = 1u << 1;    // txnBegin
inline constexpr std::uint16_t kFlagOpenedHere = 1u << 0;    // txnEnd
inline constexpr std::uint16_t kFlagClosedHere = 1u << 4;    // txnEnd
inline constexpr std::uint16_t kFlagSuccess = 1u << 5;       // txnEnd
inline constexpr std::uint16_t kFlagRejected = 1u << 6;      // txnEnd
inline constexpr std::uint16_t kFlagPlaying = 1u << 0;       // block
inline constexpr std::uint16_t kFlagCycleActive = 1u << 1;   // block
inline constexpr std::uint16_t kFlagContTimeValid = 1u << 2; // block
inline constexpr std::uint16_t kFlagRebase = 1u << 3;        // block
inline constexpr std::uint16_t kFlagFlushOnly = 1u << 4;     // block
inline constexpr std::uint16_t kFlagNoContext = 1u << 5;     // block
inline constexpr std::uint16_t kFlagHasEndValue = 1u << 0;   // queue
inline constexpr std::uint16_t kFlagBlockBoundary = 1u << 0;  // openReleased
inline constexpr std::uint16_t kFlagOpenedHold = 1u << 1;     // openHeld

// One trace record. Trivially copyable and self-contained on purpose: the audio
// thread copies it into the ring with a plain assignment and never touches a
// string, an allocation or a lock.
//
// The generic fields carry different things per event; formatRecord() below is
// the single place that says which.
struct Record {
  std::uint64_t sequence = 0;
  std::int64_t timeNs = 0;
  // The transport position last seen by a process() block, so the lane position
  // a host-facing report lands at can be inferred for every line.
  std::int64_t projectTime = 0;
  std::int64_t continuousTime = 0;
  std::int64_t i64 = 0;
  double d0 = 0.0;
  double d1 = 0.0;
  std::uint32_t threadId = 0;
  std::uint32_t parameterId = 0;
  std::uint32_t u32 = 0;
  std::int32_t i32 = 0;
  std::uint16_t flags = 0;
  std::uint8_t event = static_cast<std::uint8_t>(Event::none);
  std::uint8_t role = static_cast<std::uint8_t>(Role::host);
  std::uint32_t instance = 0;
};

static_assert(std::is_trivially_copyable_v<Record>,
              "the audio thread copies a record into the ring by assignment");

// Generous on purpose: a 50 ms drain against a 64-sample block at 48 kHz has
// about 37 blocks to cover, and a drag adds a handful of records per block.
inline constexpr std::size_t kRingCapacity = 1u << 16;
static_assert((kRingCapacity & (kRingCapacity - 1u)) == 0u,
              "the ring index is masked rather than divided");
// How many points of one input queue are written out. A host streaming a curve
// can put dozens in a block and the shape is readable from the first few plus
// the last; the truncation is recorded rather than hidden.
inline constexpr std::int32_t kMaxTracedPoints = 16;

// No parameter id is this, so it names a record that is about every parameter
// at once -- the wholesale claim clear, for instance.
inline constexpr std::uint32_t kAllParameters = 0xFFFFFFFFu;

struct State {
  std::unique_ptr<Record[]> records;
  std::unique_ptr<std::atomic<std::uint64_t>[]> published;
  // Claimed slot count. A producer takes one slot per record from here.
  std::atomic<std::uint64_t> claimed{0};
  // How far the consumer has drained. Published so a producer can tell a full
  // ring from an empty one without a lock.
  std::atomic<std::uint64_t> drainedIndex{0};
  std::atomic<std::uint64_t> drops{0};
  // The transport position every record is stamped with, updated by each block.
  std::atomic<std::int64_t> projectTime{0};
  std::atomic<std::int64_t> continuousTime{0};
  std::atomic<std::uint32_t> nextInstance{1};
  std::chrono::steady_clock::time_point started{};
  // Consumer-only state, guarded by drainMutex.
  std::mutex drainMutex;
  std::FILE *file = nullptr;
  std::uint64_t reportedDrops = 0;
  bool filtered = false;
  std::uint32_t filterParameterId = 0;
};

[[nodiscard]] inline std::string environmentValue(const char *const name) {
#if defined(_WIN32)
  char *buffer = nullptr;
  std::size_t size = 0;
  if (_dupenv_s(&buffer, &size, name) != 0 || buffer == nullptr) {
    return {};
  }
  std::string value(buffer);
  std::free(buffer);
  return value;
#else
  const char *const value = std::getenv(name);
  return value == nullptr ? std::string{} : std::string(value);
#endif
}

[[nodiscard]] inline std::FILE *openTraceFile(const std::string &path) noexcept {
#if defined(_WIN32)
  std::FILE *file = nullptr;
  if (fopen_s(&file, path.c_str(), "ab") != 0) {
    return nullptr;
  }
  return file;
#else
  return std::fopen(path.c_str(), "ab");
#endif
}

// Reads the environment and, when a path is named, builds the whole trace
// state. Runs once, from the dynamic initialiser of g_state below.
[[nodiscard]] inline State *installFromEnvironment() noexcept {
  try {
    const auto path = environmentValue("EFFETUNE_AUTOMATION_TRACE");
    if (path.empty()) {
      return nullptr;
    }
    auto *file = openTraceFile(path);
    if (file == nullptr) {
      return nullptr;
    }
    auto state = std::make_unique<State>();
    state->records = std::make_unique<Record[]>(kRingCapacity);
    state->published = std::make_unique<std::atomic<std::uint64_t>[]>(kRingCapacity);
    for (std::size_t index = 0; index < kRingCapacity; ++index) {
      state->published[index].store(0, std::memory_order_relaxed);
    }
    state->started = std::chrono::steady_clock::now();
    state->file = file;
    const auto filter = environmentValue("EFFETUNE_AUTOMATION_TRACE_PARAM");
    if (!filter.empty()) {
      char *end = nullptr;
      const auto parsed = std::strtoul(filter.c_str(), &end, 10);
      if (end != filter.c_str()) {
        state->filtered = true;
        state->filterParameterId = static_cast<std::uint32_t>(parsed);
      }
    }
    std::fprintf(file,
                 "# EffeTune Mixwright automation trace\n"
                 "# build=%s %s version=%s\n",
                 __DATE__, __TIME__, EFFETUNE_PLUGIN_VERSION_STR);
    if (state->filtered) {
      std::fprintf(file, "# parameter filter=%u\n", state->filterParameterId);
    } else {
      std::fprintf(file, "# parameter filter=none\n");
    }
    std::fprintf(file,
                 "# columns: seq t_ms role inst tid proj event param detail\n");
    std::fflush(file);
    return state.release();
  } catch (...) {
    // A trace that cannot be set up must never stop the plug-in loading.
    return nullptr;
  }
}

// Null unless tracing is on. Deliberately a plain pointer rather than an
// atomic: it is written once, by this dynamic initialiser, before any plug-in
// instance exists, so a load of it can be hoisted and shared across a whole
// process() block.
inline State *g_state = installFromEnvironment();

// The one check every call site makes. When tracing is off this is a load of a
// pointer that is null for the lifetime of the process, so a whole block's
// worth of call sites collapses to one load and a run of not-taken branches.
[[nodiscard]] inline bool enabled() noexcept { return g_state != nullptr; }

// Whether the per-block records for one parameter are wanted. Records about
// every parameter at once are never filtered out.
[[nodiscard]] inline bool tracksParameter(const std::uint32_t parameterId) noexcept {
  const auto *const state = g_state;
  if (state == nullptr) {
    return false;
  }
  return !state->filtered || parameterId == kAllParameters ||
         state->filterParameterId == parameterId;
}

// A small identifier for one plug-in instance, so a host with several of them
// loaded produces one readable log rather than an unattributable interleaving.
[[nodiscard]] inline std::uint32_t nextInstanceId() noexcept {
  auto *const state = g_state;
  return state == nullptr
             ? 0u
             : state->nextInstance.fetch_add(1u, std::memory_order_relaxed);
}

// Which thread the records emitted from here belong to. Constant-initialised,
// so reading it costs no guard variable.
inline thread_local Role g_role = Role::host;

// Names the calling thread's role for the duration of a scope. The role is set
// unconditionally: it is two plain thread-local accesses, and making it
// conditional would leave the previous role standing when a trace is enabled
// part-way through a scope.
class ScopedRole {
public:
  explicit ScopedRole(const Role role) noexcept : previous_(g_role) { g_role = role; }
  ~ScopedRole() noexcept { g_role = previous_; }
  ScopedRole(const ScopedRole &) = delete;
  ScopedRole &operator=(const ScopedRole &) = delete;
  ScopedRole(ScopedRole &&) = delete;
  ScopedRole &operator=(ScopedRole &&) = delete;

private:
  Role previous_;
};

[[nodiscard]] inline std::uint32_t currentThreadId() noexcept {
  return static_cast<std::uint32_t>(
      std::hash<std::thread::id>{}(std::this_thread::get_id()));
}

// Appends one record. Wait-free: one compare-exchange to claim a slot, a plain
// struct assignment into it, one release store to publish it. Never allocates,
// never locks, never performs I/O, so the audio thread may call it.
inline void emit(Record record) noexcept {
  auto *const state = g_state;
  if (state == nullptr) {
    return;
  }
  auto claim = state->claimed.load(std::memory_order_relaxed);
  for (;;) {
    if (claim - state->drainedIndex.load(std::memory_order_acquire) >=
        static_cast<std::uint64_t>(kRingCapacity)) {
      // A full ring drops the record and counts it. Blocking here would be a
      // priority inversion on the audio thread, and stalling a block to keep a
      // diagnostic would change the very behaviour being measured.
      state->drops.fetch_add(1u, std::memory_order_relaxed);
      return;
    }
    if (state->claimed.compare_exchange_weak(claim, claim + 1u,
                                             std::memory_order_acq_rel,
                                             std::memory_order_relaxed)) {
      break;
    }
  }
  record.sequence = claim;
  record.timeNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now() - state->started)
                      .count();
  record.threadId = currentThreadId();
  record.role = static_cast<std::uint8_t>(g_role);
  record.projectTime = state->projectTime.load(std::memory_order_relaxed);
  record.continuousTime = state->continuousTime.load(std::memory_order_relaxed);
  const auto index = static_cast<std::size_t>(claim & (kRingCapacity - 1u));
  state->records[index] = record;
  state->published[index].store(claim + 1u, std::memory_order_release);
}

// The transport position later records are stamped with. Called by the block
// record so every host-facing report that follows carries the position the host
// would write it at.
inline void publishTransport(const std::int64_t projectTime,
                             const std::int64_t continuousTime) noexcept {
  auto *const state = g_state;
  if (state == nullptr) {
    return;
  }
  state->projectTime.store(projectTime, std::memory_order_relaxed);
  state->continuousTime.store(continuousTime, std::memory_order_relaxed);
}

[[nodiscard]] inline const char *eventName(const Event event) noexcept {
  switch (event) {
  case Event::setParamNormalized:
    return "setParamNormalized";
  case Event::setParamAdopted:
    return "setParamAdopted";
  case Event::txnBegin:
    return "txnBegin";
  case Event::txnEnd:
    return "txnEnd";
  case Event::hostBeginEdit:
    return "beginEdit";
  case Event::hostPerformEdit:
    return "performEdit";
  case Event::hostEndEdit:
    return "endEdit";
  case Event::hostStartGroupEdit:
    return "startGroupEdit";
  case Event::hostFinishGroupEdit:
    return "finishGroupEdit";
  case Event::block:
    return "block";
  case Event::queue:
    return "queue";
  case Event::queuePoint:
    return "queuePoint";
  case Event::queueTruncated:
    return "queueTruncated";
  case Event::dspValue:
    return "dspValue";
  case Event::touchOpened:
    return "touchOpened";
  case Event::touchClosed:
    return "touchClosed";
  case Event::openHeld:
    return "openHeld";
  case Event::openReleased:
    return "openReleased";
  case Event::automationState:
    return "automationState";
  case Event::none:
    break;
  }
  return "none";
}

[[nodiscard]] inline const char *roleName(const Role role) noexcept {
  switch (role) {
  case Role::audio:
    return "audio";
  case Role::timer:
    return "timer";
  case Role::ui:
    return "ui";
  case Role::host:
    break;
  }
  return "host";
}

// The one place that says what a record's generic fields mean. Writes the
// event-specific tail into `detail`; the common prefix is written by flush().
inline void formatDetail(const Record &record, char *const detail,
                         const std::size_t capacity) noexcept {
  const auto has = [&record](const std::uint16_t bit) noexcept {
    return (record.flags & bit) != 0 ? 1 : 0;
  };
  switch (static_cast<Event>(record.event)) {
  case Event::setParamNormalized:
    std::snprintf(detail, capacity,
                  "value=%.9f finite=%d disposition=%s", record.d0, has(kFlagFinite),
                  has(kFlagSuppressed) != 0 ? "suppress" : "apply");
    break;
  case Event::setParamAdopted:
    std::snprintf(detail, capacity, "slot=%u adopted=%.9f", record.u32, record.d0);
    break;
  case Event::txnBegin:
    std::snprintf(detail, capacity,
                  "normalized=%.9f previous=%.9f beginGesture=%d endGesture=%d",
                  record.d0, record.d1, has(kFlagBeginGesture), has(kFlagEndGesture));
    break;
  case Event::txnEnd:
    std::snprintf(detail, capacity,
                  "normalized=%.9f previous=%.9f openedHere=%d closedHere=%d "
                  "success=%d rejected=%d",
                  record.d0, record.d1, has(kFlagOpenedHere), has(kFlagClosedHere),
                  has(kFlagSuccess), has(kFlagRejected));
    break;
  case Event::hostBeginEdit:
  case Event::hostEndEdit:
    std::snprintf(detail, capacity, "result=0x%08x", record.u32);
    break;
  case Event::hostPerformEdit:
    std::snprintf(detail, capacity, "value=%.9f result=0x%08x", record.d0, record.u32);
    break;
  case Event::hostStartGroupEdit:
  case Event::hostFinishGroupEdit:
    std::snprintf(detail, capacity, "result=0x%08x", record.u32);
    break;
  case Event::block:
    std::snprintf(detail, capacity,
                  "numSamples=%d start=%lld state=0x%08x playing=%d cycle=%d "
                  "contValid=%d rebase=%d flushOnly=%d context=%d",
                  record.i32, static_cast<long long>(record.i64), record.u32,
                  has(kFlagPlaying), has(kFlagCycleActive), has(kFlagContTimeValid),
                  has(kFlagRebase), has(kFlagFlushOnly), has(kFlagNoContext) != 0 ? 0 : 1);
    break;
  case Event::queue:
    std::snprintf(detail, capacity,
                  "points=%u endValue=%.9f hasEndValue=%d disposition=%s",
                  record.u32, record.d0, has(kFlagHasEndValue),
                  has(kFlagSuppressed) != 0 ? "suppress" : "apply");
    break;
  case Event::queuePoint:
    std::snprintf(detail, capacity, "index=%u sampleOffset=%d value=%.9f", record.u32,
                  record.i32, record.d0);
    break;
  case Event::queueTruncated:
    std::snprintf(detail, capacity, "points=%u traced=%d", record.u32, record.i32);
    break;
  case Event::dspValue:
    std::snprintf(detail, capacity, "slot=%u adopted=%.9f generation=%lld", record.u32,
                  record.d0, static_cast<long long>(record.i64));
    break;
  case Event::touchOpened:
    std::snprintf(detail, capacity, "value=%.9f", record.d0);
    break;
  case Event::touchClosed:
    std::snprintf(detail, capacity, "endEditAccepted=%d", has(kFlagSuccess));
    break;
  case Event::openHeld:
    std::snprintf(detail, capacity, "value=%.9f coalesced=%u atOpen=%d", record.d0,
                  record.u32, has(kFlagOpenedHold));
    break;
  case Event::openReleased:
    std::snprintf(detail, capacity,
                  "value=%.9f coalesced=%u trigger=%s result=0x%08x", record.d0,
                  record.u32, has(kFlagBlockBoundary) != 0 ? "block" : "close",
                  static_cast<unsigned>(record.i32));
    break;
  case Event::automationState:
    std::snprintf(detail, capacity, "state=0x%08x read=%d write=%d", record.u32,
                  (record.u32 & 0x1u) != 0 ? 1 : 0, (record.u32 & 0x2u) != 0 ? 1 : 0);
    break;
  case Event::none:
    detail[0] = '\0';
    break;
  }
}

// Drains everything the producers have published and writes it out. Control
// threads only: it takes a mutex and performs I/O.
inline void flush() noexcept {
  auto *const state = g_state;
  if (state == nullptr) {
    return;
  }
  std::scoped_lock drain(state->drainMutex);
  if (state->file == nullptr) {
    return;
  }
  auto index = state->drainedIndex.load(std::memory_order_relaxed);
  const auto claimed = state->claimed.load(std::memory_order_acquire);
  char detail[512];
  char line[768];
  auto wrote = false;
  while (index < claimed) {
    const auto slot = static_cast<std::size_t>(index & (kRingCapacity - 1u));
    if (state->published[slot].load(std::memory_order_acquire) != index + 1u) {
      // A producer claimed this slot and has not finished writing it. Stopping
      // here rather than spinning keeps the drain off any producer's back; the
      // next tick picks the record up.
      break;
    }
    const Record record = state->records[slot];
    formatDetail(record, detail, sizeof(detail));
    char parameter[16];
    if (record.parameterId == kAllParameters) {
      std::snprintf(parameter, sizeof(parameter), "%s", "*");
    } else if (static_cast<Event>(record.event) == Event::block) {
      std::snprintf(parameter, sizeof(parameter), "%s", "-");
    } else {
      std::snprintf(parameter, sizeof(parameter), "%u", record.parameterId);
    }
    const auto written = std::snprintf(
        line, sizeof(line), "%08llu %12.3f %-5s i%02u t%08x proj=%-12lld %-18s p=%-8s %s\n",
        static_cast<unsigned long long>(record.sequence),
        static_cast<double>(record.timeNs) / 1.0e6,
        roleName(static_cast<Role>(record.role)), record.instance, record.threadId,
        static_cast<long long>(record.projectTime),
        eventName(static_cast<Event>(record.event)), parameter, detail);
    if (written > 0) {
      std::fputs(line, state->file);
      wrote = true;
    }
    ++index;
    state->drainedIndex.store(index, std::memory_order_release);
  }
  const auto drops = state->drops.load(std::memory_order_relaxed);
  if (drops != state->reportedDrops) {
    std::fprintf(state->file, "# dropped %llu records (ring full)\n",
                 static_cast<unsigned long long>(drops - state->reportedDrops));
    state->reportedDrops = drops;
    wrote = true;
  }
  if (wrote) {
    // The interesting runs end with the host being killed or the project being
    // closed, so nothing is left sitting in a stdio buffer.
    std::fflush(state->file);
  }
}

// ---------------------------------------------------------------------------
// Typed emitters. Each one is a pure sink over values the caller already has.
// ---------------------------------------------------------------------------

inline void setParamNormalized(const std::uint32_t instance,
                               const std::uint32_t parameterId, const double value,
                               const bool finite, const bool suppressed) noexcept {
  Record record;
  record.instance = instance;
  record.event = static_cast<std::uint8_t>(Event::setParamNormalized);
  record.parameterId = parameterId;
  record.d0 = value;
  record.flags = static_cast<std::uint16_t>((finite ? kFlagFinite : 0u) |
                                            (suppressed ? kFlagSuppressed : 0u));
  emit(record);
}

inline void setParamAdopted(const std::uint32_t instance,
                            const std::uint32_t parameterId, const std::uint32_t slot,
                            const double adopted) noexcept {
  Record record;
  record.instance = instance;
  record.event = static_cast<std::uint8_t>(Event::setParamAdopted);
  record.parameterId = parameterId;
  record.u32 = slot;
  record.d0 = adopted;
  emit(record);
}

inline void txnBegin(const std::uint32_t instance, const std::uint32_t parameterId,
                     const double normalized, const double previous,
                     const bool beginGesture, const bool endGesture) noexcept {
  Record record;
  record.instance = instance;
  record.event = static_cast<std::uint8_t>(Event::txnBegin);
  record.parameterId = parameterId;
  record.d0 = normalized;
  record.d1 = previous;
  record.flags = static_cast<std::uint16_t>((beginGesture ? kFlagBeginGesture : 0u) |
                                            (endGesture ? kFlagEndGesture : 0u));
  emit(record);
}

inline void txnEnd(const std::uint32_t instance, const std::uint32_t parameterId,
                   const double normalized, const double previous,
                   const std::uint16_t flags) noexcept {
  Record record;
  record.instance = instance;
  record.event = static_cast<std::uint8_t>(Event::txnEnd);
  record.parameterId = parameterId;
  record.d0 = normalized;
  record.d1 = previous;
  record.flags = flags;
  emit(record);
}

inline void hostEdit(const std::uint32_t instance, const Event event,
                     const std::uint32_t parameterId, const double value,
                     const std::int32_t result) noexcept {
  Record record;
  record.instance = instance;
  record.event = static_cast<std::uint8_t>(event);
  record.parameterId = parameterId;
  record.d0 = value;
  record.u32 = static_cast<std::uint32_t>(result);
  emit(record);
}

inline void block(const std::uint32_t instance, const std::int64_t projectTime,
                  const std::int64_t continuousTime, const std::uint32_t state,
                  const std::int32_t numSamples, const std::int64_t absoluteStart,
                  const std::uint16_t flags) noexcept {
  publishTransport(projectTime, continuousTime);
  Record record;
  record.instance = instance;
  record.event = static_cast<std::uint8_t>(Event::block);
  record.parameterId = kAllParameters;
  record.u32 = state;
  record.i32 = numSamples;
  record.i64 = absoluteStart;
  record.flags = flags;
  emit(record);
}

inline void queue(const std::uint32_t instance, const std::uint32_t parameterId,
                  const std::uint32_t pointCount, const double endValue,
                  const bool hasEndValue, const bool suppressed) noexcept {
  Record record;
  record.instance = instance;
  record.event = static_cast<std::uint8_t>(Event::queue);
  record.parameterId = parameterId;
  record.u32 = pointCount;
  record.d0 = endValue;
  record.flags = static_cast<std::uint16_t>((hasEndValue ? kFlagHasEndValue : 0u) |
                                            (suppressed ? kFlagSuppressed : 0u));
  emit(record);
}

inline void queuePoint(const std::uint32_t instance, const std::uint32_t parameterId,
                       const std::uint32_t pointIndex, const std::int32_t sampleOffset,
                       const double value) noexcept {
  Record record;
  record.instance = instance;
  record.event = static_cast<std::uint8_t>(Event::queuePoint);
  record.parameterId = parameterId;
  record.u32 = pointIndex;
  record.i32 = sampleOffset;
  record.d0 = value;
  emit(record);
}

inline void queueTruncated(const std::uint32_t instance,
                           const std::uint32_t parameterId,
                           const std::uint32_t pointCount,
                           const std::int32_t traced) noexcept {
  Record record;
  record.instance = instance;
  record.event = static_cast<std::uint8_t>(Event::queueTruncated);
  record.parameterId = parameterId;
  record.u32 = pointCount;
  record.i32 = traced;
  emit(record);
}

inline void dspValue(const std::uint32_t instance, const std::uint32_t parameterId,
                     const std::uint32_t slot, const double adopted,
                     const std::uint64_t generation) noexcept {
  Record record;
  record.instance = instance;
  record.event = static_cast<std::uint8_t>(Event::dspValue);
  record.parameterId = parameterId;
  record.u32 = slot;
  record.d0 = adopted;
  record.i64 = static_cast<std::int64_t>(generation);
  emit(record);
}

inline void touchOpened(const std::uint32_t instance, const std::uint32_t parameterId,
                        const double value) noexcept {
  Record record;
  record.instance = instance;
  record.event = static_cast<std::uint8_t>(Event::touchOpened);
  record.parameterId = parameterId;
  record.d0 = value;
  emit(record);
}

inline void touchClosed(const std::uint32_t instance, const std::uint32_t parameterId,
                        const bool accepted) noexcept {
  Record record;
  record.instance = instance;
  record.event = static_cast<std::uint8_t>(Event::touchClosed);
  record.parameterId = parameterId;
  record.flags = static_cast<std::uint16_t>(accepted ? kFlagSuccess : 0u);
  emit(record);
}

// One value withheld from the host until the next process() boundary.
// `coalesced` counts how many values of this gesture the held position has
// carried so far, so a hold that swallowed more than the sub-block window is
// visible in the log rather than inferred from a gap.
inline void openHeld(const std::uint32_t instance, const std::uint32_t parameterId,
                     const double value, const std::uint32_t coalesced,
                     const bool atOpen) noexcept {
  Record record;
  record.instance = instance;
  record.event = static_cast<std::uint8_t>(Event::openHeld);
  record.parameterId = parameterId;
  record.d0 = value;
  record.u32 = coalesced;
  record.flags = static_cast<std::uint16_t>(atOpen ? kFlagOpenedHold : 0u);
  emit(record);
}

// The withheld value reaching the host, always from a UI/controller thread.
// `blockBoundary` says whether the release observed a crossed process()
// boundary -- the ordinary case, whether the control-service tick or the
// gesture close carried it -- or was the close-time flush of a value no
// boundary ever separated from its beginEdit.
inline void openReleased(const std::uint32_t instance, const std::uint32_t parameterId,
                         const double value, const std::uint32_t coalesced,
                         const bool blockBoundary,
                         const std::int32_t result) noexcept {
  Record record;
  record.instance = instance;
  record.event = static_cast<std::uint8_t>(Event::openReleased);
  record.parameterId = parameterId;
  record.d0 = value;
  record.u32 = coalesced;
  record.i32 = result;
  record.flags = static_cast<std::uint16_t>(blockBoundary ? kFlagBlockBoundary : 0u);
  emit(record);
}

inline void automationState(const std::uint32_t instance,
                            const std::int32_t state) noexcept {
  Record record;
  record.instance = instance;
  record.event = static_cast<std::uint8_t>(Event::automationState);
  record.parameterId = kAllParameters;
  record.u32 = static_cast<std::uint32_t>(state);
  emit(record);
}

// Rebuilds the trace state from the current environment. The plug-in never
// calls this -- g_state is installed once, at load -- but a test process has to
// be able to set the variable and then observe the effect, which is not
// possible through a load-time initialiser. Not thread safe by design: it is
// only ever called with no other thread running.
inline void reinstallFromEnvironmentForTesting() noexcept {
  auto *const previous = g_state;
  g_state = nullptr;
  if (previous != nullptr) {
    if (previous->file != nullptr) {
      std::fclose(previous->file);
      previous->file = nullptr;
    }
    delete previous;
  }
  g_state = installFromEnvironment();
}

} // namespace effetune::vst::plugin::trace
