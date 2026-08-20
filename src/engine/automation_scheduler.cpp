#include "engine/automation_scheduler.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <limits>
#include <new>

namespace effetune::vst {
namespace {

static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
              "Automation RT state requires lock-free 64-bit atomics");

[[nodiscard]] constexpr std::int64_t ceilToQuantum(const std::int64_t sample,
                                                   const std::uint32_t quantum) noexcept {
  if (quantum == 0) {
    return sample;
  }
  const auto divisor = static_cast<std::int64_t>(quantum);
  auto quotient = sample / divisor;
  const auto remainder = sample % divisor;
  if (remainder > 0) {
    if (quotient == std::numeric_limits<std::int64_t>::max() / divisor) {
      return std::numeric_limits<std::int64_t>::max();
    }
    ++quotient;
  }
  return quotient * divisor;
}

[[nodiscard]] constexpr std::int64_t
saturatingAdd(const std::int64_t value, const std::uint32_t increment) noexcept {
  const auto maximum = std::numeric_limits<std::int64_t>::max();
  if (value > maximum - static_cast<std::int64_t>(increment)) {
    return maximum;
  }
  return value + static_cast<std::int64_t>(increment);
}

[[nodiscard]] constexpr bool validNormalized(const double value) noexcept {
  return value >= 0.0 && value <= 1.0;
}

void increment(std::atomic<std::uint64_t> &counter) noexcept {
  counter.fetch_add(1, std::memory_order_relaxed);
}

} // namespace

std::uint32_t automationQuantumForSampleRate(const double sampleRate) noexcept {
  if (!std::isfinite(sampleRate) || sampleRate <= 0.0) {
    return 0;
  }
  if (sampleRate <= 48000.0) {
    return 8;
  }
  if (sampleRate <= 96000.0) {
    return 16;
  }
  if (sampleRate <= 192000.0) {
    return 32;
  }
  double ceiling = 192000.0;
  std::uint32_t quantum = 32;
  while (sampleRate > ceiling) {
    if (quantum > std::numeric_limits<std::uint32_t>::max() / 2u ||
        ceiling > std::numeric_limits<double>::max() / 2.0) {
      return 0;
    }
    quantum *= 2u;
    ceiling *= 2.0;
  }
  return quantum;
}

struct AutomationScheduler::Impl {
  struct Point {
    std::int32_t offset = 0;
    double value = 0.0;
  };

  struct Slot {
    std::atomic<std::uint64_t> pendingSequence{0};
    std::atomic<std::uint32_t> pendingParameterId{0};
    std::atomic<std::uint64_t> pendingValueBits{0};
    std::atomic<std::uint8_t> pendingMode{0};
    std::atomic_bool pendingActive{false};
    std::atomic<std::uint64_t> forcedThroughSequence{0};
    std::uint64_t consumedPendingSequence = 0;
    std::uint64_t consumedForceSequence = 0;
    bool active = false;
    AutomationMode mode = AutomationMode::continuous;
    std::uint32_t parameterId = 0;
    double current = 0.0;
    double blockStart = 0.0;
    bool previousPointValid = false;
    std::int64_t previousPointAbsolute = 0;
    double previousPointValue = 0.0;
    bool dirty = false;
    bool emittedThisBlock = false;
    bool carryValid = false;
    std::int64_t carryAbsolute = 0;
    double carryValue = 0.0;
    std::uint32_t pointCount = 0;
    std::int32_t lastAcceptedOffset = -1;
    bool hasLatestInput = false;
    double latestInput = 0.0;
    std::array<Point, kMaximumPointsPerQueue> points{};
    std::atomic<std::uint64_t> publishedSequence{0};
    std::atomic<std::uint64_t> publishedBits{0};
    std::atomic_bool publishedActive{false};
  };

  struct DiagnosticAtoms {
    std::atomic<std::uint64_t> unknownQueues{0};
    std::atomic<std::uint64_t> invalidOffsets{0};
    std::atomic<std::uint64_t> invalidValues{0};
    std::atomic<std::uint64_t> nonMonotonicPoints{0};
    std::atomic<std::uint64_t> pointOverflows{0};
    std::atomic<std::uint64_t> rebases{0};
    std::atomic<std::uint64_t> failedBlocks{0};
  } diagnostics;

  std::unique_ptr<Slot[]> slots;
  std::array<std::uint16_t, kEffectSlotCount + 1u> activeIndices{};
  std::uint32_t activeCount = 0;
  std::array<AutomationValueChange, kEffectSlotCount + 1u> changes{};
  std::uint32_t changeCount = 0;
  std::uint32_t quantum = 0;
  std::atomic<std::uint32_t> requestedQuantum{0};
  std::atomic<std::uint32_t> activeQuantum{0};
  std::int64_t clock = 0;
  AutomationBlockContext block{};
  std::uint32_t cursor = 0;
  std::int64_t firstGrid = 0;
  Slot *intakeSlot = nullptr;
  bool intakeUnknown = false;
  std::atomic_bool prepared{false};
  bool blockOpen = false;
  bool intakeFinished = false;
  bool scheduledWork = false;
  std::uint64_t publishGeneration = 0;
  std::atomic<std::uint64_t> pendingConfigurationGeneration{0};
  std::uint64_t consumedConfigurationGeneration = 0;

  [[nodiscard]] Slot *slotForParameter(const std::uint32_t parameterId) noexcept {
    if (parameterId == kBypassAutomationParameterId) {
      return &slots[kBypassSlot];
    }
    if (parameterId < kFirstEffectAutomationParameterId) {
      return nullptr;
    }
    const auto slot = parameterId - kFirstEffectAutomationParameterId;
    if (slot >= kEffectSlotCount || !slots[slot].active ||
        slots[slot].parameterId != parameterId) {
      return nullptr;
    }
    return &slots[slot];
  }

  [[nodiscard]] std::uint32_t indexOf(const Slot &slot) const noexcept {
    return static_cast<std::uint32_t>(&slot - slots.get());
  }

  void publishConfiguration(Slot &slot, const bool active,
                            const std::uint32_t parameterId,
                            const AutomationMode mode,
                            const double current,
                            const bool forceCurrentInitialization) noexcept {
    auto token = slot.pendingSequence.load(std::memory_order_acquire);
    for (;;) {
      if ((token & 1u) != 0u) {
        token = slot.pendingSequence.load(std::memory_order_acquire);
        continue;
      }
      if (slot.pendingSequence.compare_exchange_weak(
              token, token + 1u, std::memory_order_acq_rel,
              std::memory_order_acquire)) {
        break;
      }
    }
    slot.pendingParameterId.store(parameterId, std::memory_order_relaxed);
    slot.pendingValueBits.store(std::bit_cast<std::uint64_t>(current),
                                std::memory_order_relaxed);
    slot.pendingMode.store(static_cast<std::uint8_t>(mode),
                           std::memory_order_relaxed);
    slot.pendingActive.store(active, std::memory_order_relaxed);
    if (forceCurrentInitialization) {
      slot.forcedThroughSequence.store(token + 2u, std::memory_order_relaxed);
    }
    slot.pendingSequence.store(token + 2u, std::memory_order_release);
    pendingConfigurationGeneration.fetch_add(1, std::memory_order_release);
  }

  void consumeConfigurations() noexcept {
    const auto pending =
        pendingConfigurationGeneration.load(std::memory_order_acquire);
    if (pending == consumedConfigurationGeneration) {
      return;
    }
    activeCount = 0;
    auto consumedEverySlot = true;
    for (std::uint32_t index = 0; index <= kBypassSlot; ++index) {
      auto &slot = slots[index];
      consumedEverySlot = consumeConfiguration(slot) && consumedEverySlot;
      if (slot.active) {
        activeIndices[activeCount++] = static_cast<std::uint16_t>(index);
      }
    }
    // A slot whose publish was still in flight keeps its previous state, so the
    // generation must stay outstanding for it. The audio thread neither loops
    // nor waits for it: the next block re-reads the slot, by which time the
    // control thread has long since finished the publish.
    if (consumedEverySlot) {
      consumedConfigurationGeneration = pending;
    }
  }

  // Reports whether the slot is up to date: false only when the seqlock read
  // caught a publish in progress and the slot still holds its previous value.
  [[nodiscard]] bool consumeConfiguration(Slot &slot) noexcept {
    for (std::uint32_t attempt = 0; attempt < 2; ++attempt) {
      const auto first = slot.pendingSequence.load(std::memory_order_acquire);
      if ((first & 1u) != 0u) {
        return false;
      }
      if (first == slot.consumedPendingSequence) {
        return true;
      }
      const auto parameterId =
          slot.pendingParameterId.load(std::memory_order_relaxed);
      const auto valueBits =
          slot.pendingValueBits.load(std::memory_order_relaxed);
      const auto mode = slot.pendingMode.load(std::memory_order_relaxed);
      const auto active = slot.pendingActive.load(std::memory_order_relaxed);
      const auto forcedThroughSequence =
          slot.forcedThroughSequence.load(std::memory_order_relaxed);
      const auto second = slot.pendingSequence.load(std::memory_order_acquire);
      if (first != second) {
        continue;
      }
      // A configuration that does not force initialization is a metadata
      // refresh: the caller republished the slot without asking for a new
      // value, so a slot that was already playing keeps the value it is on.
      // Forcing is how a caller states that the published value is the one to
      // adopt.
      const auto forceCurrentInitialization =
          forcedThroughSequence > slot.consumedForceSequence;
      const auto preserveCurrent =
          slot.active && active && !forceCurrentInitialization;
      slot.consumedPendingSequence = first;
      slot.consumedForceSequence = forcedThroughSequence;
      slot.active = active;
      slot.publishedActive.store(active, std::memory_order_release);
      slot.pointCount = 0;
      slot.hasLatestInput = false;
      if (!active) {
        slot.carryValid = false;
        slot.previousPointValid = false;
        slot.dirty = false;
        return true;
      }
      slot.parameterId = parameterId;
      slot.mode = mode == static_cast<std::uint8_t>(AutomationMode::stepped)
                      ? AutomationMode::stepped
                      : AutomationMode::continuous;
      if (preserveCurrent) {
        return true;
      }
      slot.carryValid = false;
      slot.previousPointValid = false;
      slot.current = std::bit_cast<double>(valueBits);
      slot.blockStart = slot.current;
      slot.dirty = true;
      publish(slot, slot.current);
      return true;
    }
    return false;
  }

  void publish(Slot &slot, const double value) noexcept {
    const auto generation = ++publishGeneration;
    slot.publishedSequence.store(generation * 2u - 1u, std::memory_order_release);
    slot.publishedBits.store(std::bit_cast<std::uint64_t>(value),
                             std::memory_order_relaxed);
    slot.publishedSequence.store(generation * 2u, std::memory_order_release);
  }

  void appendChange(const std::uint32_t index, Slot &slot) noexcept {
    if (changeCount >= changes.size()) {
      return;
    }
    changes[changeCount++] = {index == kBypassSlot ? kBypassSlot : index,
                              slot.parameterId, slot.current,
                              index == kBypassSlot};
    slot.emittedThisBlock = true;
  }

  [[nodiscard]] double continuousValueAt(const Slot &slot,
                                         const std::uint32_t offset) const noexcept {
    const auto absolute = saturatingAdd(block.absoluteStart, offset);
    auto previousAbsolute = block.absoluteStart;
    double previousValue = slot.blockStart;
    if (slot.previousPointValid &&
        slot.previousPointAbsolute <= absolute) {
      previousAbsolute = slot.previousPointAbsolute;
      previousValue = slot.previousPointValue;
    }
    std::uint32_t index = 0;
    while (index < slot.pointCount) {
      const auto pointAbsolute = saturatingAdd(
          block.absoluteStart,
          static_cast<std::uint32_t>(slot.points[index].offset));
      if (pointAbsolute > absolute) {
        break;
      }
      previousAbsolute = pointAbsolute;
      previousValue = slot.points[index].value;
      ++index;
    }
    if (index == slot.pointCount || previousAbsolute == absolute) {
      return previousValue;
    }
    const auto &next = slot.points[index];
    const auto nextAbsolute = saturatingAdd(
        block.absoluteStart, static_cast<std::uint32_t>(next.offset));
    if (nextAbsolute <= previousAbsolute) {
      return next.value;
    }
    const auto position = static_cast<double>(absolute - previousAbsolute) /
                          static_cast<double>(nextAbsolute - previousAbsolute);
    return previousValue + (next.value - previousValue) * position;
  }

  [[nodiscard]] bool steppedValueAt(const Slot &slot, const std::int64_t absolute,
                                    double &value) const noexcept {
    bool found = false;
    if (slot.carryValid && slot.carryAbsolute == absolute) {
      value = slot.carryValue;
      found = true;
    }
    for (std::uint32_t index = 0; index < slot.pointCount; ++index) {
      const auto pointAbsolute =
          saturatingAdd(block.absoluteStart,
                        static_cast<std::uint32_t>(slot.points[index].offset));
      const auto boundary = quantum == 0 ? block.absoluteStart
                                         : ceilToQuantum(pointAbsolute, quantum);
      if (boundary == absolute) {
        value = slot.points[index].value;
        found = true;
      }
    }
    return found;
  }

  void applyBoundary(const std::uint32_t offset) noexcept {
    changeCount = 0;
    const auto absolute = saturatingAdd(block.absoluteStart, offset);
    for (std::uint32_t active = 0; active < activeCount; ++active) {
      const auto index = static_cast<std::uint32_t>(activeIndices[active]);
      auto &slot = slots[index];
      const auto wasDirty = offset == 0 && slot.dirty;
      bool hasBoundaryValue = false;
      double boundaryValue = slot.current;
      if (quantum != 0 && absolute % static_cast<std::int64_t>(quantum) == 0) {
        if (slot.mode == AutomationMode::continuous &&
            (slot.pointCount != 0 ||
             (slot.carryValid && slot.carryAbsolute == absolute))) {
          boundaryValue = continuousValueAt(slot, offset);
          hasBoundaryValue = true;
        } else if (slot.mode == AutomationMode::stepped) {
          hasBoundaryValue = steppedValueAt(slot, absolute, boundaryValue);
        }
      } else if (quantum == 0 && offset == 0 && slot.pointCount != 0) {
        boundaryValue = slot.latestInput;
        hasBoundaryValue = true;
      }
      if (hasBoundaryValue) {
        slot.current = boundaryValue;
        slot.dirty = true;
        if (slot.carryValid && slot.carryAbsolute <= absolute) {
          slot.carryValid = false;
        }
      }
      if (wasDirty || hasBoundaryValue) {
        appendChange(index, slot);
      }
    }
  }

  [[nodiscard]] std::uint32_t nextBoundaryAfter(const std::uint32_t offset) const noexcept {
    if (quantum == 0 || !scheduledWork) {
      return block.frames;
    }
    const auto absolute = saturatingAdd(block.absoluteStart, offset);
    auto next = ceilToQuantum(absolute, quantum);
    if (next <= absolute) {
      next = saturatingAdd(next, quantum);
    }
    if (next >= saturatingAdd(block.absoluteStart, block.frames)) {
      return block.frames;
    }
    return static_cast<std::uint32_t>(next - block.absoluteStart);
  }
};

AutomationScheduler::AutomationScheduler() : impl_(std::make_unique<Impl>()) {}
AutomationScheduler::~AutomationScheduler() = default;

bool AutomationScheduler::prepare(const double sampleRate) noexcept {
  const auto quantum = automationQuantumForSampleRate(sampleRate);
  if (!std::isfinite(sampleRate) || sampleRate <= 0.0) {
    return false;
  }
  const auto firstPreparation = !impl_->slots;
  if (firstPreparation) {
    impl_->slots.reset(new (std::nothrow) Impl::Slot[kEffectSlotCount + 1u]);
    if (!impl_->slots) {
      return false;
    }
    impl_->quantum = quantum;
    impl_->requestedQuantum.store(quantum, std::memory_order_relaxed);
    impl_->activeQuantum.store(quantum, std::memory_order_relaxed);
    configureBypass(false);
    reset(0);
    impl_->prepared.store(true, std::memory_order_release);
    return true;
  }
  if (impl_->requestedQuantum.load(std::memory_order_acquire) == quantum) {
    return true;
  }
  impl_->requestedQuantum.store(quantum, std::memory_order_release);
  return true;
}

bool AutomationScheduler::prepared() const noexcept {
  return impl_->prepared.load(std::memory_order_acquire);
}

void AutomationScheduler::reset(const std::int64_t absoluteClock) noexcept {
  if (!impl_->slots) {
    return;
  }
  impl_->clock = absoluteClock;
  impl_->blockOpen = false;
  impl_->intakeFinished = false;
  impl_->intakeSlot = nullptr;
  for (std::uint32_t active = 0; active < impl_->activeCount; ++active) {
    auto &slot = impl_->slots[impl_->activeIndices[active]];
    slot.pointCount = 0;
    slot.lastAcceptedOffset = -1;
    slot.hasLatestInput = false;
    slot.carryValid = false;
    slot.previousPointValid = false;
    slot.emittedThisBlock = false;
    if (slot.active) {
      slot.dirty = true;
    }
  }
}

bool AutomationScheduler::configure(const std::uint32_t slot,
                                    const std::uint32_t parameterId,
                                    const AutomationMode mode,
                                    const double currentNormalized,
                                    const bool forceCurrentInitialization) noexcept {
  if (!impl_->slots || slot >= kEffectSlotCount ||
      parameterId != kFirstEffectAutomationParameterId + slot ||
      !std::isfinite(currentNormalized) || !validNormalized(currentNormalized)) {
    return false;
  }
  impl_->publishConfiguration(impl_->slots[slot], true, parameterId, mode,
                              currentNormalized, forceCurrentInitialization);
  return true;
}

void AutomationScheduler::deactivate(const std::uint32_t slot) noexcept {
  if (!impl_->slots || slot >= kEffectSlotCount) {
    return;
  }
  impl_->publishConfiguration(impl_->slots[slot], false,
                              kFirstEffectAutomationParameterId + slot,
                              AutomationMode::continuous, 0.0, false);
}

void AutomationScheduler::configureBypass(
    const bool current, const bool forceCurrentInitialization) noexcept {
  if (!impl_->slots) {
    return;
  }
  impl_->publishConfiguration(impl_->slots[kBypassSlot], true,
                              kBypassAutomationParameterId,
                              AutomationMode::stepped, current ? 1.0 : 0.0,
                              forceCurrentInitialization);
}

void AutomationScheduler::beginBlock(const AutomationBlockContext &context) noexcept {
  if (!impl_->prepared.load(std::memory_order_acquire) || !impl_->slots) {
    return;
  }
  const auto requestedQuantum =
      impl_->requestedQuantum.load(std::memory_order_acquire);
  if (requestedQuantum != impl_->quantum) {
    impl_->quantum = requestedQuantum;
    impl_->activeQuantum.store(requestedQuantum, std::memory_order_release);
  }
  auto actual = context;
  actual.flushOnly = actual.flushOnly || actual.frames == 0;
  if (!actual.flushOnly && actual.absoluteStart != impl_->clock) {
    actual.rebase = true;
  }
  if (actual.rebase) {
    increment(impl_->diagnostics.rebases);
    for (std::uint32_t index = 0; index <= kBypassSlot; ++index) {
      impl_->slots[index].carryValid = false;
      impl_->slots[index].previousPointValid = false;
    }
    impl_->clock = actual.absoluteStart;
  }
  impl_->block = actual;
  impl_->cursor = 0;
  impl_->changeCount = 0;
  impl_->intakeSlot = nullptr;
  impl_->intakeUnknown = false;
  impl_->blockOpen = true;
  impl_->intakeFinished = false;
  impl_->scheduledWork = false;
  impl_->firstGrid = impl_->quantum == 0
                         ? actual.absoluteStart
                         : ceilToQuantum(actual.absoluteStart, impl_->quantum);
  impl_->consumeConfigurations();
  for (std::uint32_t active = 0; active < impl_->activeCount; ++active) {
    auto &slot = impl_->slots[impl_->activeIndices[active]];
    slot.pointCount = 0;
    slot.lastAcceptedOffset = -1;
    slot.hasLatestInput = false;
    slot.blockStart = slot.current;
    slot.emittedThisBlock = false;
    impl_->scheduledWork = impl_->scheduledWork || slot.dirty || slot.carryValid;
  }
}

void AutomationScheduler::beginQueue(const std::uint32_t parameterId) noexcept {
  if (!impl_->blockOpen || impl_->intakeFinished) {
    return;
  }
  impl_->intakeSlot = impl_->slotForParameter(parameterId);
  impl_->intakeUnknown = impl_->intakeSlot == nullptr;
  if (impl_->intakeUnknown) {
    increment(impl_->diagnostics.unknownQueues);
  }
}

void AutomationScheduler::pushPoint(const std::int32_t sampleOffset,
                                    const double normalized) noexcept {
  if (!impl_->blockOpen || impl_->intakeFinished || impl_->intakeUnknown ||
      impl_->intakeSlot == nullptr) {
    return;
  }
  auto &slot = *impl_->intakeSlot;
  const auto maximumOffset = static_cast<std::int64_t>(impl_->block.frames);
  if (sampleOffset < 0 || static_cast<std::int64_t>(sampleOffset) > maximumOffset) {
    increment(impl_->diagnostics.invalidOffsets);
    return;
  }
  if (!std::isfinite(normalized) || !validNormalized(normalized)) {
    increment(impl_->diagnostics.invalidValues);
    return;
  }
  if (sampleOffset < slot.lastAcceptedOffset) {
    increment(impl_->diagnostics.nonMonotonicPoints);
    return;
  }
  slot.lastAcceptedOffset = sampleOffset;
  slot.hasLatestInput = true;
  slot.latestInput = normalized;
  if (slot.pointCount < slot.points.size()) {
    slot.points[slot.pointCount++] = {sampleOffset, normalized};
  } else {
    slot.points.back() = {sampleOffset, normalized};
    increment(impl_->diagnostics.pointOverflows);
  }
  impl_->scheduledWork = true;
}

void AutomationScheduler::endQueue() noexcept {
  impl_->intakeSlot = nullptr;
  impl_->intakeUnknown = false;
}

void AutomationScheduler::finishIntake() noexcept {
  if (!impl_->blockOpen || impl_->intakeFinished) {
    return;
  }
  endQueue();
  for (std::uint32_t active = 0; active < impl_->activeCount; ++active) {
    auto &slot = impl_->slots[impl_->activeIndices[active]];
    if (!slot.hasLatestInput) {
      continue;
    }
    impl_->publish(slot, slot.latestInput);
    if (impl_->block.flushOnly) {
      slot.current = slot.latestInput;
      slot.dirty = true;
      slot.carryValid = false;
      if (slot.mode == AutomationMode::continuous) {
        slot.previousPointValid = true;
        slot.previousPointAbsolute = impl_->block.absoluteStart;
        slot.previousPointValue = slot.latestInput;
      }
      continue;
    }
    const auto lastAbsolute = saturatingAdd(
        impl_->block.absoluteStart,
        static_cast<std::uint32_t>(slot.lastAcceptedOffset));
    const auto boundary = impl_->quantum == 0
                              ? impl_->block.absoluteStart
                              : ceilToQuantum(lastAbsolute, impl_->quantum);
    const auto end =
        saturatingAdd(impl_->block.absoluteStart, impl_->block.frames);
    if (boundary >= end) {
      slot.carryValid = true;
      slot.carryAbsolute = boundary;
      slot.carryValue = slot.latestInput;
    }
  }
  impl_->intakeFinished = true;
  if (impl_->block.flushOnly) {
    impl_->blockOpen = false;
  }
}

bool AutomationScheduler::nextSlice(AutomationSlice &slice) noexcept {
  slice = {};
  if (!impl_->blockOpen || !impl_->intakeFinished || impl_->block.flushOnly ||
      impl_->cursor >= impl_->block.frames) {
    return false;
  }
  impl_->applyBoundary(impl_->cursor);
  const auto next = impl_->nextBoundaryAfter(impl_->cursor);
  slice.hostOffset = impl_->cursor;
  slice.hostFrames = next - impl_->cursor;
  slice.changes = std::span<const AutomationValueChange>(impl_->changes.data(),
                                                         impl_->changeCount);
  impl_->cursor = next;
  return slice.hostFrames != 0;
}

void AutomationScheduler::completeBlock(const bool success) noexcept {
  if (!impl_->blockOpen) {
    return;
  }
  if (success) {
    for (std::uint32_t active = 0; active < impl_->activeCount; ++active) {
      auto &slot = impl_->slots[impl_->activeIndices[active]];
      if (slot.mode == AutomationMode::continuous && slot.hasLatestInput) {
        slot.previousPointValid = true;
        slot.previousPointAbsolute = saturatingAdd(
            impl_->block.absoluteStart,
            static_cast<std::uint32_t>(slot.lastAcceptedOffset));
        slot.previousPointValue = slot.latestInput;
      }
      if (slot.emittedThisBlock) {
        slot.dirty = false;
      }
    }
  } else {
    increment(impl_->diagnostics.failedBlocks);
    for (std::uint32_t active = 0; active < impl_->activeCount; ++active) {
      auto &slot = impl_->slots[impl_->activeIndices[active]];
      if (slot.hasLatestInput) {
        slot.current = slot.latestInput;
        if (slot.mode == AutomationMode::continuous) {
          slot.previousPointValid = true;
          slot.previousPointAbsolute = saturatingAdd(
              impl_->block.absoluteStart,
              static_cast<std::uint32_t>(slot.lastAcceptedOffset));
          slot.previousPointValue = slot.latestInput;
        }
      } else if (slot.carryValid) {
        slot.current = slot.carryValue;
      }
      slot.carryValid = false;
      slot.dirty = true;
    }
  }
  impl_->clock = saturatingAdd(impl_->block.absoluteStart, impl_->block.frames);
  impl_->blockOpen = false;
}

bool AutomationScheduler::hasScheduledWork() const noexcept {
  return impl_->scheduledWork;
}

std::span<const std::uint16_t> AutomationScheduler::activeSlots() const noexcept {
  return {impl_->activeIndices.data(), impl_->activeCount};
}

bool AutomationScheduler::isActive(const std::uint32_t slot) const noexcept {
  return impl_->slots && slot < kEffectSlotCount && impl_->slots[slot].active;
}

double AutomationScheduler::currentNormalized(const std::uint32_t slot) const noexcept {
  return isActive(slot) ? impl_->slots[slot].current : 0.0;
}

bool AutomationScheduler::currentBypass() const noexcept {
  return impl_->slots && impl_->slots[kBypassSlot].current > 0.5;
}

std::uint32_t AutomationScheduler::quantum() const noexcept {
  return impl_->activeQuantum.load(std::memory_order_acquire);
}

std::int64_t AutomationScheduler::renderedClock() const noexcept { return impl_->clock; }

bool AutomationScheduler::readPublished(const std::uint32_t slot,
                                        const std::uint64_t lastGeneration,
                                        PublishedAutomationValue &value) const noexcept {
  if (!impl_->slots || slot >= kEffectSlotCount) {
    return false;
  }
  const auto &state = impl_->slots[slot];
  for (std::uint32_t attempt = 0; attempt < 2; ++attempt) {
    const auto first = state.publishedSequence.load(std::memory_order_acquire);
    if ((first & 1u) != 0u) {
      continue;
    }
    const auto bits = state.publishedBits.load(std::memory_order_relaxed);
    const auto second = state.publishedSequence.load(std::memory_order_acquire);
    if (first == second && first != 0) {
      const auto generation = first / 2u;
      if (generation <= lastGeneration) {
        return false;
      }
      value = {generation, std::bit_cast<double>(bits)};
      return true;
    }
  }
  return false;
}

std::uint64_t AutomationScheduler::publishedGeneration(
    const std::uint32_t slot) const noexcept {
  if (!impl_->slots || slot >= kEffectSlotCount) {
    return 0;
  }
  const auto &state = impl_->slots[slot];
  for (std::uint32_t attempt = 0; attempt < 2; ++attempt) {
    const auto sequence = state.publishedSequence.load(std::memory_order_acquire);
    if ((sequence & 1u) == 0u &&
        state.publishedSequence.load(std::memory_order_acquire) == sequence) {
      return sequence / 2u;
    }
  }
  return 0;
}

AutomationDiagnostics AutomationScheduler::diagnostics() const noexcept {
  return {impl_->diagnostics.unknownQueues.load(std::memory_order_relaxed),
          impl_->diagnostics.invalidOffsets.load(std::memory_order_relaxed),
          impl_->diagnostics.invalidValues.load(std::memory_order_relaxed),
          impl_->diagnostics.nonMonotonicPoints.load(std::memory_order_relaxed),
          impl_->diagnostics.pointOverflows.load(std::memory_order_relaxed),
          impl_->diagnostics.rebases.load(std::memory_order_relaxed),
          impl_->diagnostics.failedBlocks.load(std::memory_order_relaxed)};
}

} // namespace effetune::vst
