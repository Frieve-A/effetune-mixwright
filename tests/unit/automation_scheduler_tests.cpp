#include "engine/automation_scheduler.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "../support/crt_dialog_suppression.h"

using namespace effetune::vst;

namespace {

void expect(const bool condition, const std::string &message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

[[nodiscard]] bool close(const double left, const double right) {
  return std::abs(left - right) < 1.0e-12;
}

struct Applied {
  std::int64_t sample = 0;
  std::uint32_t parameterId = 0;
  double value = 0.0;
};

std::vector<Applied> drain(AutomationScheduler &scheduler,
                           const std::int64_t start) {
  std::vector<Applied> applied;
  AutomationSlice slice;
  while (scheduler.nextSlice(slice)) {
    for (const auto &change : slice.changes) {
      applied.push_back(
          {start + slice.hostOffset, change.parameterId, change.normalized});
    }
  }
  return applied;
}

// The audio thread reads a configuration through a seqlock and never retries
// past two attempts, so a publish caught in flight leaves that slot on its
// previous value. Advancing the consumed generation regardless would strand it
// until the slot happened to be configured again, so the generation now only
// moves once every slot has been read. The audio thread still neither loops nor
// waits: the block that follows re-reads whatever it missed.
void testConcurrentConfigurationPublishReachesTheAudioSide() {
  AutomationScheduler scheduler;
  expect(scheduler.prepare(48000.0), "concurrent publish scheduler prepare");

  std::atomic_bool running{true};
  std::atomic_bool published{true};
  std::atomic<std::uint64_t> publishCount{0};
  double lastPublished = 0.0;
  std::thread publisher([&] {
    std::uint32_t step = 0;
    while (running.load(std::memory_order_acquire)) {
      const auto value = static_cast<double>(step++ % 997u) / 997.0;
      if (!scheduler.configure(0, kFirstEffectAutomationParameterId,
                               AutomationMode::continuous, value, true)) {
        published.store(false, std::memory_order_release);
        return;
      }
      lastPublished = value;
      publishCount.fetch_add(1, std::memory_order_acq_rel);
    }
  });

  std::int64_t clock = 0;
  const auto renderBlock = [&] {
    scheduler.beginBlock({clock, 64, false, false});
    scheduler.finishIntake();
    (void)drain(scheduler, clock);
    scheduler.completeBlock(true);
    clock += 64;
  };
  // Rendering is far cheaper than publishing, so the loop is bounded by how far
  // the publisher gets rather than by a block count.
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (publishCount.load(std::memory_order_acquire) < 20000u &&
         published.load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < deadline) {
    renderBlock();
  }
  running.store(false, std::memory_order_release);
  publisher.join();

  expect(published.load(std::memory_order_acquire),
         "every concurrent configuration was accepted");
  expect(publishCount.load(std::memory_order_acquire) >= 20000u,
         "the publisher really raced the consuming blocks");
  // The first block can still find the final publish in flight; the one after
  // it cannot, so the value has to be playing by then.
  renderBlock();
  renderBlock();
  expect(close(scheduler.currentNormalized(0), lastPublished),
         "the last configuration reaches the audio side within the next block");
}

void testQuantumRule() {
  expect(automationQuantumForSampleRate(44100.0) == 8 &&
             automationQuantumForSampleRate(48000.0) == 8 &&
             automationQuantumForSampleRate(88200.0) == 16 &&
             automationQuantumForSampleRate(96000.0) == 16 &&
             automationQuantumForSampleRate(176400.0) == 32 &&
             automationQuantumForSampleRate(192000.0) == 32,
         "exact sample-rate quantum mapping");
  expect(automationQuantumForSampleRate(22050.0) == 8 &&
             automationQuantumForSampleRate(50000.0) == 16 &&
             automationQuantumForSampleRate(100000.0) == 32 &&
             automationQuantumForSampleRate(384000.0) == 64 &&
             automationQuantumForSampleRate(768000.0) == 128,
         "unsupported-rate band rule");
  expect(automationQuantumForSampleRate(0.0) == 0 &&
             automationQuantumForSampleRate(-1.0) == 0,
         "invalid rate fallback");
}

void testDeactivateSupersedesPendingConfiguration() {
  AutomationScheduler scheduler;
  expect(scheduler.prepare(48000.0), "deactivation scheduler prepare");
  expect(scheduler.configure(0, kFirstEffectAutomationParameterId,
                             AutomationMode::continuous, 0.5),
         "pending slot configure");
  scheduler.deactivate(0);

  scheduler.beginBlock({0, 8, false, false});
  scheduler.finishIntake();
  const auto active = scheduler.activeSlots();
  expect(!scheduler.isActive(0) && active.size() == 1u &&
             active.front() == AutomationScheduler::kBypassSlot,
         "deactivation supersedes an unconsumed configuration");
  const auto emitted = drain(scheduler, 0);
  for (const auto &change : emitted) {
    expect(change.parameterId != kFirstEffectAutomationParameterId,
           "deactivated pending slot emits no automation");
  }
  scheduler.completeBlock(true);
}

void testUnchangedConfigurationPreservesEffectAndBypassRetry() {
  AutomationScheduler scheduler;
  expect(scheduler.prepare(48000.0), "configuration preservation scheduler prepare");
  expect(scheduler.configure(0, kFirstEffectAutomationParameterId,
                             AutomationMode::continuous, 0.2),
         "configure preserved effect slot");
  scheduler.configureBypass(false);

  scheduler.beginBlock({0, 8, false, false});
  scheduler.beginQueue(kFirstEffectAutomationParameterId);
  scheduler.pushPoint(7, 0.8);
  scheduler.endQueue();
  scheduler.beginQueue(kBypassAutomationParameterId);
  scheduler.pushPoint(7, 1.0);
  scheduler.endQueue();
  scheduler.finishIntake();
  (void)drain(scheduler, 0);
  scheduler.completeBlock(true);

  expect(scheduler.configure(0, kFirstEffectAutomationParameterId,
                             AutomationMode::continuous, 0.2),
         "republish unchanged effect metadata");
  scheduler.configureBypass(false);
  scheduler.beginBlock({8, 8, false, false});
  scheduler.finishIntake();
  const auto interleaved = drain(scheduler, 8);
  expect(interleaved.size() == 2 &&
             interleaved[0].parameterId == kFirstEffectAutomationParameterId &&
             close(interleaved[0].value, 0.8) &&
             interleaved[1].parameterId == kBypassAutomationParameterId &&
             close(interleaved[1].value, 1.0),
         "unchanged metadata preserves interleaved effect and bypass carry");
  scheduler.completeBlock(false);

  expect(scheduler.configure(0, kFirstEffectAutomationParameterId,
                             AutomationMode::continuous, 0.2),
         "republish unchanged effect metadata before retry");
  scheduler.configureBypass(false);
  scheduler.beginBlock({16, 8, false, false});
  scheduler.finishIntake();
  const auto retry = drain(scheduler, 16);
  expect(retry.size() == 2 && close(retry[0].value, 0.8) &&
             close(retry[1].value, 1.0) && scheduler.currentBypass(),
         "failed interleaved block retries the latest effect and bypass values");
  scheduler.completeBlock(true);
}

void testForcedInitializationSurvivesPendingMetadataCoalescing() {
  AutomationScheduler scheduler;
  expect(scheduler.prepare(48000.0), "sticky force scheduler prepare");
  expect(scheduler.configure(0, kFirstEffectAutomationParameterId,
                             AutomationMode::continuous, 0.1, true),
         "publish forced effect initialization");
  expect(scheduler.configure(0, kFirstEffectAutomationParameterId,
                             AutomationMode::continuous, 0.8),
         "coalesce latest effect metadata and current");
  scheduler.configureBypass(true, true);
  scheduler.configureBypass(false);

  scheduler.beginBlock({0, 8, false, false});
  scheduler.finishIntake();
  const auto initialized = drain(scheduler, 0);
  expect(initialized.size() == 2 && close(initialized[0].value, 0.8) &&
             close(initialized[1].value, 0.0) && !scheduler.currentBypass(),
         "latest effect and bypass values retain sticky force initialization");
  scheduler.completeBlock(true);

  expect(scheduler.configure(0, kFirstEffectAutomationParameterId,
                             AutomationMode::stepped, 0.2),
         "publish metadata after force consumption");
  scheduler.configureBypass(true);
  scheduler.beginBlock({8, 8, false, false});
  scheduler.finishIntake();
  const auto preserved = drain(scheduler, 8);
  expect(preserved.empty() && close(scheduler.currentNormalized(0), 0.8) &&
             !scheduler.currentBypass(),
         "audio consumption clears sticky force while preserving current values");
  scheduler.completeBlock(true);
}

// The preserve rule decides whether a republished configuration is a metadata
// refresh or an adoption. Both callers exist: a topology reconcile republishes
// every slot from a registry that is older than what automation is playing,
// while a preset load or an undo forces the value it means to install.
void testDefaultConfigurationPreservesTheLivePlayedValue() {
  AutomationScheduler scheduler;
  expect(scheduler.prepare(48000.0), "live-value preservation scheduler prepare");
  expect(scheduler.configure(0, kFirstEffectAutomationParameterId,
                             AutomationMode::continuous, 0.1, true),
         "adopt the initial current of the live slot");
  scheduler.beginBlock({0, 8, false, false});
  scheduler.beginQueue(kFirstEffectAutomationParameterId);
  scheduler.pushPoint(0, 0.9);
  scheduler.endQueue();
  scheduler.finishIntake();
  (void)drain(scheduler, 0);
  scheduler.completeBlock(true);
  expect(close(scheduler.currentNormalized(0), 0.9),
         "the live slot plays the value the host published");

  expect(scheduler.configure(0, kFirstEffectAutomationParameterId,
                             AutomationMode::stepped, 0.1),
         "republish the live slot without forcing initialization");
  scheduler.beginBlock({8, 8, false, false});
  scheduler.finishIntake();
  (void)drain(scheduler, 8);
  scheduler.completeBlock(true);
  expect(close(scheduler.currentNormalized(0), 0.9),
         "a reconcile keeps the value a live slot is playing");

  expect(scheduler.configure(0, kFirstEffectAutomationParameterId,
                             AutomationMode::continuous, 0.1, true),
         "force the named value onto the same live slot");
  scheduler.beginBlock({16, 8, false, false});
  scheduler.finishIntake();
  (void)drain(scheduler, 16);
  scheduler.completeBlock(true);
  expect(close(scheduler.currentNormalized(0), 0.1),
         "a forced configuration replaces the value a live slot is playing");

  scheduler.deactivate(0);
  scheduler.beginBlock({24, 8, false, false});
  scheduler.finishIntake();
  (void)drain(scheduler, 24);
  scheduler.completeBlock(true);
  expect(scheduler.configure(0, kFirstEffectAutomationParameterId,
                             AutomationMode::continuous, 0.35),
         "rebind the retired slot without forcing initialization");
  scheduler.beginBlock({32, 8, false, false});
  scheduler.finishIntake();
  (void)drain(scheduler, 32);
  scheduler.completeBlock(true);
  expect(close(scheduler.currentNormalized(0), 0.35),
         "a slot that was not live has nothing to preserve");
}

void testInterpolationOrderingAndCarry() {
  AutomationScheduler scheduler;
  expect(scheduler.prepare(48000.0), "scheduler prepare");
  expect(scheduler.configure(0, kFirstEffectAutomationParameterId,
                             AutomationMode::continuous, 0.0),
         "continuous slot configure");
  expect(scheduler.configure(1, kFirstEffectAutomationParameterId + 1u,
                             AutomationMode::stepped, 0.0),
         "stepped slot configure");
  scheduler.beginBlock({0, 20, false, false});
  // Deliberately reverse queue order. Emission remains ParamID ordered.
  scheduler.beginQueue(kFirstEffectAutomationParameterId + 1u);
  scheduler.pushPoint(1, 0.3);
  scheduler.pushPoint(7, 0.7);
  scheduler.pushPoint(19, 1.0);
  scheduler.endQueue();
  scheduler.beginQueue(kFirstEffectAutomationParameterId);
  scheduler.pushPoint(16, 1.0);
  scheduler.endQueue();
  scheduler.finishIntake();
  const auto applied = drain(scheduler, 0);
  scheduler.completeBlock(true);

  bool interpolated = false;
  bool bucketLast = false;
  std::uint32_t boundaryEightIndex = 0;
  for (std::uint32_t index = 0; index < applied.size(); ++index) {
    const auto &change = applied[index];
    if (change.sample == 8 &&
        change.parameterId == kFirstEffectAutomationParameterId) {
      interpolated = close(change.value, 0.5);
      boundaryEightIndex = index;
    }
    if (change.sample == 8 &&
        change.parameterId == kFirstEffectAutomationParameterId + 1u) {
      bucketLast = close(change.value, 0.7) && index > boundaryEightIndex;
    }
  }
  expect(interpolated, "continuous value is evaluated at the absolute grid");
  expect(bucketLast, "same stepped bucket keeps last point and ParamID order");

  scheduler.beginBlock({20, 12, false, false});
  scheduler.finishIntake();
  const auto carried = drain(scheduler, 20);
  scheduler.completeBlock(true);
  bool foundCarry = false;
  for (const auto &change : carried) {
    foundCarry = foundCarry ||
                 (change.sample == 24 &&
                  change.parameterId == kFirstEffectAutomationParameterId + 1u &&
                  close(change.value, 1.0));
  }
  expect(foundCarry, "event quantized beyond block end carries once");

  scheduler.beginBlock({32, 7, false, false});
  scheduler.beginQueue(kFirstEffectAutomationParameterId);
  scheduler.pushPoint(7, 0.25);
  scheduler.endQueue();
  scheduler.finishIntake();
  (void)drain(scheduler, 32);
  scheduler.completeBlock(true);
  scheduler.beginBlock({39, 9, false, false});
  scheduler.finishIntake();
  const auto continuousCarry = drain(scheduler, 39);
  scheduler.completeBlock(true);
  bool foundContinuousCarry = false;
  for (const auto &change : continuousCarry) {
    foundContinuousCarry = foundContinuousCarry ||
                           (change.sample == 40 &&
                            change.parameterId ==
                                kFirstEffectAutomationParameterId &&
                            close(change.value, 0.25));
  }
  expect(foundContinuousCarry,
         "continuous endpoint beyond a host block carries to the absolute grid");
}

void testFlushPublishAndRetry() {
  AutomationScheduler scheduler;
  expect(scheduler.prepare(48000.0), "flush scheduler prepare");
  expect(scheduler.configure(3, kFirstEffectAutomationParameterId + 3u,
                             AutomationMode::continuous, 0.1),
         "flush slot configure");
  scheduler.beginBlock({0, 0, false, true});
  scheduler.finishIntake();
  PublishedAutomationValue published;
  expect(scheduler.readPublished(3, 0, published), "initial value publication");
  const auto initialGeneration = published.generation;

  scheduler.beginBlock({0, 0, false, true});
  scheduler.beginQueue(kFirstEffectAutomationParameterId + 3u);
  scheduler.pushPoint(0, 0.9);
  scheduler.endQueue();
  scheduler.finishIntake();
  expect(close(scheduler.currentNormalized(3), 0.9),
         "zero-frame intake commits latest logical current");
  expect(scheduler.readPublished(3, initialGeneration, published) &&
             close(published.normalized, 0.9),
         "zero-frame intake publishes a newer generation");

  scheduler.beginBlock({0, 8, false, false});
  scheduler.finishIntake();
  auto applied = drain(scheduler, 0);
  scheduler.completeBlock(false);
  bool retried = false;
  for (const auto &change : applied) {
    retried = retried || (change.sample == 0 &&
                          change.parameterId ==
                              kFirstEffectAutomationParameterId + 3u &&
                          close(change.value, 0.9));
  }
  expect(retried, "flush dirty value is staged at next positive block");

  scheduler.beginBlock({8, 8, false, false});
  scheduler.finishIntake();
  applied = drain(scheduler, 8);
  scheduler.completeBlock(true);
  retried = false;
  for (const auto &change : applied) {
    retried = retried || (change.sample == 8 &&
                          change.parameterId ==
                              kFirstEffectAutomationParameterId + 3u &&
                          close(change.value, 0.9));
  }
  expect(retried && scheduler.diagnostics().failedBlocks == 1,
         "failed transaction retains latest dirty image exactly to next block");
}

std::vector<Applied> renderPartition(const std::vector<std::uint32_t> &blocks) {
  AutomationScheduler scheduler;
  expect(scheduler.prepare(48000.0), "partition scheduler prepare");
  expect(scheduler.configure(0, kFirstEffectAutomationParameterId,
                             AutomationMode::continuous, 0.0),
         "partition slot configure");
  std::vector<Applied> result;
  std::int64_t start = 0;
  for (const auto frames : blocks) {
    scheduler.beginBlock({start, frames, false, false});
    scheduler.beginQueue(kFirstEffectAutomationParameterId);
    const auto beginning = static_cast<double>(start) / 32.0;
    const auto ending = static_cast<double>(start + frames) / 32.0;
    scheduler.pushPoint(0, std::min(1.0, beginning));
    scheduler.pushPoint(static_cast<std::int32_t>(frames),
                        std::min(1.0, ending));
    scheduler.endQueue();
    scheduler.finishIntake();
    auto block = drain(scheduler, start);
    result.insert(result.end(), block.begin(), block.end());
    scheduler.completeBlock(true);
    start += frames;
  }
  std::vector<Applied> grid;
  for (const auto &change : result) {
    if ((change.sample == 8 || change.sample == 16 || change.sample == 24) &&
        change.parameterId == kFirstEffectAutomationParameterId) {
      grid.push_back(change);
    }
  }
  return grid;
}

void testPartitionEquivalenceAndDiagnostics() {
  const auto monolithic = renderPartition({32});
  const auto partitioned = renderPartition({7, 13, 12});
  expect(monolithic.size() == partitioned.size(), "partition grid size");
  for (std::size_t index = 0; index < monolithic.size(); ++index) {
    expect(monolithic[index].sample == partitioned[index].sample &&
               close(monolithic[index].value, partitioned[index].value),
           "absolute grid is independent of host block partition");
  }

  AutomationScheduler scheduler;
  expect(scheduler.prepare(48000.0), "diagnostic scheduler prepare");
  scheduler.beginBlock({-9, 20, true, false});
  scheduler.beginQueue(0xdeadbeefu);
  scheduler.pushPoint(0, 0.5);
  scheduler.endQueue();
  scheduler.beginQueue(kBypassAutomationParameterId);
  scheduler.pushPoint(-1, 0.5);
  scheduler.pushPoint(2, 2.0);
  scheduler.pushPoint(4, 1.0);
  scheduler.pushPoint(3, 0.0);
  scheduler.endQueue();
  scheduler.finishIntake();
  (void)drain(scheduler, -9);
  scheduler.completeBlock(true);
  const auto diagnostics = scheduler.diagnostics();
  expect(diagnostics.unknownQueues == 1 && diagnostics.invalidOffsets == 1 &&
             diagnostics.invalidValues == 1 &&
             diagnostics.nonMonotonicPoints == 1 && diagnostics.rebases == 1,
         "invalid queue portions are ignored with bounded diagnostics");
}

void testContinuousCursorAcrossBlocksAndReprepare() {
  const auto renderBoundary = [](const bool reprepare) {
    AutomationScheduler scheduler;
    expect(scheduler.prepare(48000.0), "cross-block cursor scheduler prepare");
    expect(scheduler.configure(0, kFirstEffectAutomationParameterId,
                               AutomationMode::continuous, 0.0),
           "cross-block cursor slot configure");

    scheduler.beginBlock({0, 13, false, false});
    scheduler.beginQueue(kFirstEffectAutomationParameterId);
    scheduler.pushPoint(13, 1.0);
    scheduler.endQueue();
    scheduler.finishIntake();
    (void)drain(scheduler, 0);
    scheduler.completeBlock(true);

    if (reprepare) {
      expect(scheduler.prepared() && scheduler.prepare(96000.0),
             "sample-rate update preserves the prepared scheduler");
      expect(scheduler.configure(0, kFirstEffectAutomationParameterId,
                                 AutomationMode::continuous, 0.0),
             "republish stale control-side current after sample-rate update");
    }

    scheduler.beginBlock({13, 11, false, false});
    scheduler.beginQueue(kFirstEffectAutomationParameterId);
    scheduler.pushPoint(8, 0.0);
    scheduler.endQueue();
    scheduler.finishIntake();
    const auto applied = drain(scheduler, 13);
    scheduler.completeBlock(true);
    for (const auto &change : applied) {
      if (change.sample == 16 &&
          change.parameterId == kFirstEffectAutomationParameterId) {
        return change.value;
      }
    }
    throw std::runtime_error("cross-block cursor emitted no value at sample 16");
  };

  expect(close(renderBoundary(false), 0.625),
         "continuous interpolation keeps the prior block endpoint as its cursor");
  expect(close(renderBoundary(true), 0.625),
         "sample-rate prepare preserves audio-owned current and interpolation cursor");
}

void testQuantumReconfigurationAdoptsAtBlockBoundary() {
  AutomationScheduler scheduler;
  expect(scheduler.prepare(48000.0), "atomic quantum scheduler prepare");
  expect(scheduler.configure(0, kFirstEffectAutomationParameterId,
                             AutomationMode::continuous, 0.0),
         "atomic quantum slot configure");

  scheduler.beginBlock({0, 24, false, false});
  scheduler.beginQueue(kFirstEffectAutomationParameterId);
  scheduler.pushPoint(24, 1.0);
  scheduler.endQueue();
  scheduler.finishIntake();
  expect(scheduler.prepare(44100.0),
         "same-quantum live prepare is a no-op");
  expect(scheduler.prepare(96000.0),
         "different-quantum live prepare publishes a request");
  const auto first = drain(scheduler, 0);
  scheduler.completeBlock(true);
  expect(std::any_of(first.begin(), first.end(), [](const Applied &change) {
           return change.sample == 8 &&
                  change.parameterId == kFirstEffectAutomationParameterId &&
                  close(change.value, 1.0 / 3.0);
         }) &&
             std::any_of(first.begin(), first.end(), [](const Applied &change) {
               return change.sample == 16 &&
                      change.parameterId == kFirstEffectAutomationParameterId &&
                      close(change.value, 2.0 / 3.0);
             }),
         "an in-flight block keeps its original quantum and cursor");

  scheduler.beginBlock({24, 16, false, false});
  scheduler.beginQueue(kFirstEffectAutomationParameterId);
  scheduler.pushPoint(16, 0.0);
  scheduler.endQueue();
  scheduler.finishIntake();
  const auto second = drain(scheduler, 24);
  scheduler.completeBlock(true);
  expect(scheduler.quantum() == 16 &&
             std::any_of(second.begin(), second.end(), [](const Applied &change) {
               return change.sample == 32 &&
                      change.parameterId == kFirstEffectAutomationParameterId &&
                      close(change.value, 0.5);
             }),
         "the next block adopts the requested quantum without losing audio-owned state");
}

void testOverflowLatestAndFailureCollapsePriority() {
  AutomationScheduler scheduler;
  expect(scheduler.prepare(48000.0), "overflow scheduler prepare");
  expect(scheduler.configure(0, kFirstEffectAutomationParameterId,
                             AutomationMode::continuous, 0.1),
         "overflow slot configure");
  scheduler.beginBlock({0, 128, false, false});
  scheduler.beginQueue(kFirstEffectAutomationParameterId);
  for (std::int32_t index = 0;
       index < static_cast<std::int32_t>(AutomationScheduler::kMaximumPointsPerQueue);
       ++index) {
    scheduler.pushPoint(index, static_cast<double>(index) / 128.0);
  }
  scheduler.pushPoint(127, 0.99);
  scheduler.endQueue();
  scheduler.finishIntake();
  PublishedAutomationValue published;
  expect(scheduler.readPublished(0, 0, published) && close(published.normalized, 0.99) &&
             scheduler.publishedGeneration(0) == published.generation &&
             scheduler.diagnostics().pointOverflows == 1,
         "overflow preserves and publishes the raw queue endpoint");
  const auto acknowledged = scheduler.publishedGeneration(0);
  expect(!scheduler.readPublished(0, acknowledged, published),
         "generation acknowledgement suppresses a superseded publication");
  (void)drain(scheduler, 0);
  scheduler.completeBlock(true);

  scheduler.reset(0);
  scheduler.beginBlock({0, 7, false, false});
  scheduler.beginQueue(kFirstEffectAutomationParameterId);
  scheduler.pushPoint(7, 0.8);
  scheduler.endQueue();
  scheduler.finishIntake();
  (void)drain(scheduler, 0);
  scheduler.completeBlock(true);

  scheduler.beginBlock({7, 1, false, false});
  scheduler.finishIntake();
  (void)drain(scheduler, 7);
  scheduler.completeBlock(false);
  expect(close(scheduler.currentNormalized(0), 0.8),
         "failed block collapses carry ahead of the prior current");
  scheduler.beginBlock({8, 8, false, false});
  scheduler.finishIntake();
  const auto retry = drain(scheduler, 8);
  scheduler.completeBlock(true);
  expect(!retry.empty() && close(retry.front().value, 0.8),
         "collapsed carry remains dirty for the next successful transaction");
}

} // namespace

int main() {
  effetune::vst::testing::suppressCrtModalDialogs();
  try {
    testConcurrentConfigurationPublishReachesTheAudioSide();
    testQuantumRule();
    testDeactivateSupersedesPendingConfiguration();
    testUnchangedConfigurationPreservesEffectAndBypassRetry();
    testForcedInitializationSurvivesPendingMetadataCoalescing();
    testDefaultConfigurationPreservesTheLivePlayedValue();
    testInterpolationOrderingAndCarry();
    testFlushPublishAndRetry();
    testPartitionEquivalenceAndDiagnostics();
    testContinuousCursorAcrossBlocksAndReprepare();
    testQuantumReconfigurationAdoptsAtBlockBoundary();
    testOverflowLatestAndFailureCollapsePriority();
    std::cout << "EffeTune automation scheduler tests passed\n";
    return 0;
  } catch (const std::exception &exception) {
    std::cerr << "Test failure: " << exception.what() << '\n';
    return 1;
  }
}
