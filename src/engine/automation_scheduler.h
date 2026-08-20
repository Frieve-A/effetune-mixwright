#pragma once

#include "engine/automation_binding.h"

#include <cstdint>
#include <memory>
#include <span>

namespace effetune::vst {

inline constexpr std::uint32_t kBypassAutomationParameterId = 0u;
inline constexpr std::uint32_t kFirstEffectAutomationParameterId = 0x00010000u;

enum class AutomationMode : std::uint8_t { continuous, stepped };

struct AutomationBlockContext {
  std::int64_t absoluteStart = 0;
  std::uint32_t frames = 0;
  bool rebase = false;
  bool flushOnly = false;
};

struct AutomationValueChange {
  std::uint32_t slot = 0;
  std::uint32_t parameterId = 0;
  double normalized = 0.0;
  bool bypass = false;
};

struct AutomationSlice {
  std::uint32_t hostOffset = 0;
  std::uint32_t hostFrames = 0;
  std::span<const AutomationValueChange> changes{};
};

struct PublishedAutomationValue {
  std::uint64_t generation = 0;
  double normalized = 0.0;
};

struct AutomationDiagnostics {
  std::uint64_t unknownQueues = 0;
  std::uint64_t invalidOffsets = 0;
  std::uint64_t invalidValues = 0;
  std::uint64_t nonMonotonicPoints = 0;
  std::uint64_t pointOverflows = 0;
  std::uint64_t rebases = 0;
  std::uint64_t failedBlocks = 0;
};

// Returns an absolute host-sample control quantum. Zero means that effect
// automation must fall back to a block boundary because the rate cannot be
// represented safely. Bypass remains available in that case.
[[nodiscard]] std::uint32_t automationQuantumForSampleRate(double sampleRate) noexcept;

// Audio-owned, fixed-capacity automation state. prepare() performs the only
// allocations; all remaining methods are safe to call from the audio thread.
class AutomationScheduler {
public:
  static constexpr std::uint32_t kEffectSlotCount = kAutomationSlotCount;
  static constexpr std::uint32_t kBypassSlot = kEffectSlotCount;
  static constexpr std::uint32_t kMaximumPointsPerQueue = 64;

  AutomationScheduler();
  ~AutomationScheduler();
  AutomationScheduler(const AutomationScheduler &) = delete;
  AutomationScheduler &operator=(const AutomationScheduler &) = delete;

  [[nodiscard]] bool prepare(double sampleRate) noexcept;
  [[nodiscard]] bool prepared() const noexcept;
  void reset(std::int64_t absoluteClock = 0) noexcept;

  // Configuration calls may run concurrently with audio intake. They publish
  // a fixed-slot generation that the audio thread adopts only at beginBlock(),
  // so an in-flight block always sees one immutable binding projection.
  [[nodiscard]] bool configure(std::uint32_t slot, std::uint32_t parameterId,
                               AutomationMode mode, double currentNormalized,
                               bool forceCurrentInitialization = false) noexcept;
  void deactivate(std::uint32_t slot) noexcept;
  void configureBypass(bool current,
                       bool forceCurrentInitialization = false) noexcept;

  void beginBlock(const AutomationBlockContext &context) noexcept;
  void beginQueue(std::uint32_t parameterId) noexcept;
  void pushPoint(std::int32_t sampleOffset, double normalized) noexcept;
  void endQueue() noexcept;
  void finishIntake() noexcept;

  [[nodiscard]] bool nextSlice(AutomationSlice &slice) noexcept;
  void completeBlock(bool success) noexcept;

  [[nodiscard]] bool hasScheduledWork() const noexcept;
  // Valid until the next beginBlock(). The span is sorted by ParamID and
  // includes the internal bypass slot at most once.
  [[nodiscard]] std::span<const std::uint16_t> activeSlots() const noexcept;
  [[nodiscard]] bool isActive(std::uint32_t slot) const noexcept;
  [[nodiscard]] double currentNormalized(std::uint32_t slot) const noexcept;
  [[nodiscard]] bool currentBypass() const noexcept;
  [[nodiscard]] std::uint32_t quantum() const noexcept;
  [[nodiscard]] std::int64_t renderedClock() const noexcept;

  [[nodiscard]] bool readPublished(std::uint32_t slot,
                                   std::uint64_t lastGeneration,
                                   PublishedAutomationValue &value) const noexcept;
  [[nodiscard]] std::uint64_t publishedGeneration(std::uint32_t slot) const noexcept;
  [[nodiscard]] AutomationDiagnostics diagnostics() const noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace effetune::vst
