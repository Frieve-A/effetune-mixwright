#include "plugin/plugin_processor.h"
#include "plugin/plugin_ids.h"

#include "pluginterfaces/base/ustring.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "public.sdk/source/vst/hosting/parameterchanges.h"
#include "public.sdk/source/vst/utility/memoryibstream.h"

#include "choc/text/choc_JSON.h"
#include "allocation_guard.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "../support/crt_dialog_suppression.h"

#if defined(_WIN32)
#include <Windows.h>
#endif

namespace effetune::vst::plugin {

class PluginProcessorTestAccess {
public:
  [[nodiscard]] static std::unique_lock<std::mutex>
  lockProcessingResources(EffeTuneProcessor &processor) {
    return std::unique_lock(processor.processingResourcesMutex_);
  }

  static void setAutomationFallbackClock(EffeTuneProcessor &processor,
                                         const std::int64_t clock) {
    processor.processedHostFrames_ = clock;
    processor.automationScheduler_.reset(clock);
  }

  [[nodiscard]] static std::int64_t automationBlockStart(
      EffeTuneProcessor &processor, const Steinberg::Vst::ProcessData &data,
      bool &rebase) {
    return processor.automationBlockStart(data, rebase);
  }

  [[nodiscard]] static EngineHost::ProcessCounters
  processCounters(const EffeTuneProcessor &processor) {
    return processor.engine_.processCounters();
  }

  [[nodiscard]] static bool
  automationDeltaPending(const EffeTuneProcessor &processor) noexcept {
    return processor.automationDeltaPending_.load(std::memory_order_acquire);
  }

  [[nodiscard]] static std::uint64_t
  pipelinePlanRevision(const EffeTuneProcessor &processor) noexcept {
    return processor.engine_.pipelinePlanRevision();
  }

  [[nodiscard]] static std::uint64_t
  servicedPipelinePlanRevision(const EffeTuneProcessor &processor) noexcept {
    return processor.servicedPipelinePlanRevision_.load(std::memory_order_acquire);
  }

  [[nodiscard]] static std::uint64_t
  descriptorGeneration(const EffeTuneProcessor &processor) noexcept {
    return processor.descriptorGeneration_.load(std::memory_order_acquire);
  }

  [[nodiscard]] static std::uint64_t
  servicedDescriptorGeneration(const EffeTuneProcessor &processor) noexcept {
    return processor.servicedDescriptorGeneration_.load(std::memory_order_acquire);
  }

  [[nodiscard]] static std::uint64_t
  parameterImageGeneration(const EffeTuneProcessor &processor) noexcept {
    return processor.parameterImageGeneration_.load(std::memory_order_acquire);
  }

  [[nodiscard]] static std::uint64_t
  servicedParameterImageGeneration(const EffeTuneProcessor &processor) noexcept {
    return processor.servicedParameterImageGeneration_.load(
        std::memory_order_acquire);
  }

  [[nodiscard]] static bool
  hasPendingControlWork(const EffeTuneProcessor &processor) noexcept {
    return processor.hasPendingControlWork();
  }

  // Stands in for the 50 ms control-service timer on a thread of its own, which
  // is what makes the service concurrent with the audio callback.
  static void serviceLatencyUpdates(EffeTuneProcessor &processor) {
    processor.serviceLatencyUpdates(false);
  }

  // The editor's telemetry and host-info polls reach the same service, but at
  // frame rate rather than once per block period. They take the same idleness
  // sample the timer does, so a timer that never fires cannot leave the service
  // believing a playing transport has stopped.
  static void pollLatencyUpdatesFromUi(EffeTuneProcessor &processor) {
    processor.serviceLatencyUpdates(false);
  }

  [[nodiscard]] static std::chrono::steady_clock::duration
  audioIdleThreshold(const EffeTuneProcessor &processor) noexcept {
    return processor.audioIdleThreshold();
  }

  [[nodiscard]] static bool observeAudioIdle(
      EffeTuneProcessor &processor,
      const std::chrono::steady_clock::time_point now) noexcept {
    return processor.observeAudioIdle(now);
  }

  static void setAudioIdleObservation(
      EffeTuneProcessor &processor, const Steinberg::int32 maxHostFrames,
      const double sampleRate, const std::uint64_t renderedBlocks,
      const std::chrono::steady_clock::time_point observedAt) noexcept {
    processor.maxHostFrames_.store(maxHostFrames, std::memory_order_release);
    processor.hostSampleRate_.store(sampleRate, std::memory_order_release);
    processor.renderedBlockCount_.store(renderedBlocks,
                                        std::memory_order_release);
    processor.observedRenderedBlockCount_.store(renderedBlocks,
                                                std::memory_order_release);
    processor.renderedBlockObservedAtTicks_.store(
        observedAt.time_since_epoch().count(), std::memory_order_release);
  }

  [[nodiscard]] static std::uint64_t
  renderedBlockCount(const EffeTuneProcessor &processor) noexcept {
    return processor.renderedBlockCount_.load(std::memory_order_acquire);
  }

  static void stagePendingControllerWrite(
      EffeTuneProcessor &processor, const std::uint32_t slot,
      const AutomationTargetIdentity &identity, const double normalized) {
    std::scoped_lock resources(processor.processingResourcesMutex_);
    const auto *binding = processor.automationBindings_.binding(slot);
    const auto *target = processor.automationBindings_.slot(slot);
    if (binding == nullptr || target == nullptr || target->identity != identity) {
      throw std::runtime_error("the automation target is not bound");
    }
    auto &write = processor.pendingControllerWrites_[slot];
    write.pending = true;
    write.normalized = normalized;
    write.authorityGeneration =
        processor.controllerAuthorityGenerations_[slot].load(
            std::memory_order_acquire);
    write.identity = identity;
    processor.controllerWritePending_.store(true, std::memory_order_release);
  }

  [[nodiscard]] static bool
  controllerWritePending(const EffeTuneProcessor &processor) noexcept {
    return processor.controllerWritePending_.load(std::memory_order_acquire);
  }

  static void commitPendingControllerWritesIfAudioIdle(
      EffeTuneProcessor &processor) {
    processor.commitPendingControllerWritesIfAudioIdle(
        /*waitForResources=*/true);
  }

  static void pauseControllerCommitBeforePublish(
      EffeTuneProcessor &processor, const bool pause) noexcept {
#if defined(EFFETUNE_PROCESSOR_TEST_HOOKS)
    processor.pauseControllerCommitBeforePublishForTesting_.store(
        pause, std::memory_order_release);
#else
    (void)processor;
    (void)pause;
#endif
  }

  [[nodiscard]] static bool
  controllerCommitPaused(const EffeTuneProcessor &processor) noexcept {
#if defined(EFFETUNE_PROCESSOR_TEST_HOOKS)
    return processor.controllerCommitPausedForTesting_.load(
        std::memory_order_acquire);
#else
    (void)processor;
    return false;
#endif
  }

  static void pausePluginUpdateBeforeRuntimeTransaction(
      EffeTuneProcessor &processor, const bool pause) noexcept {
#if defined(EFFETUNE_PROCESSOR_TEST_HOOKS)
    processor.pausePluginUpdateBeforeRuntimeTransactionForTesting_.store(
        pause, std::memory_order_release);
#else
    (void)processor;
    (void)pause;
#endif
  }

  [[nodiscard]] static bool pluginUpdatePausedBeforeRuntimeTransaction(
      const EffeTuneProcessor &processor) noexcept {
#if defined(EFFETUNE_PROCESSOR_TEST_HOOKS)
    return processor.pluginUpdatePausedBeforeRuntimeTransactionForTesting_.load(
        std::memory_order_acquire);
#else
    (void)processor;
    return false;
#endif
  }

  static void pausePluginUpdateBeforeAutomationEdits(
      EffeTuneProcessor &processor, const bool pause) noexcept {
#if defined(EFFETUNE_PROCESSOR_TEST_HOOKS)
    processor.pausePluginUpdateBeforeAutomationEditsForTesting_.store(
        pause, std::memory_order_release);
#else
    (void)processor;
    (void)pause;
#endif
  }

  [[nodiscard]] static bool pluginUpdatePausedBeforeAutomationEdits(
      const EffeTuneProcessor &processor) noexcept {
#if defined(EFFETUNE_PROCESSOR_TEST_HOOKS)
    return processor.pluginUpdatePausedBeforeAutomationEditsForTesting_.load(
        std::memory_order_acquire);
#else
    (void)processor;
    return false;
#endif
  }

  static void pauseNextBulkRequestBeforeCommit(
      EffeTuneProcessor &processor) noexcept {
#if defined(EFFETUNE_PROCESSOR_TEST_HOOKS)
    processor.releaseBulkRequestBeforeCommitForTesting_.store(
        false, std::memory_order_release);
    processor.pauseNextBulkRequestBeforeCommitForTesting_.store(
        true, std::memory_order_release);
#else
    (void)processor;
#endif
  }

  [[nodiscard]] static bool bulkRequestPausedBeforeCommit(
      const EffeTuneProcessor &processor) noexcept {
#if defined(EFFETUNE_PROCESSOR_TEST_HOOKS)
    return processor.bulkRequestPausedBeforeCommitForTesting_.load(
        std::memory_order_acquire);
#else
    (void)processor;
    return false;
#endif
  }

  static void releaseBulkRequestBeforeCommit(
      EffeTuneProcessor &processor) noexcept {
#if defined(EFFETUNE_PROCESSOR_TEST_HOOKS)
    processor.releaseBulkRequestBeforeCommitForTesting_.store(
        true, std::memory_order_release);
#else
    (void)processor;
#endif
  }

  static void pauseAutomationCatalogBeforeProjection(
      EffeTuneProcessor &processor, const bool pause) noexcept {
#if defined(EFFETUNE_PROCESSOR_TEST_HOOKS)
    processor.pauseAutomationCatalogBeforeProjectionForTesting_.store(
        pause, std::memory_order_release);
#else
    (void)processor;
    (void)pause;
#endif
  }

  [[nodiscard]] static bool automationCatalogPausedBeforeProjection(
      const EffeTuneProcessor &processor) noexcept {
#if defined(EFFETUNE_PROCESSOR_TEST_HOOKS)
    return processor.automationCatalogPausedBeforeProjectionForTesting_.load(
        std::memory_order_acquire);
#else
    (void)processor;
    return false;
#endif
  }

  static void synchronizeAutomationBindings(EffeTuneProcessor &processor) {
    processor.synchronizeAutomationBindings(false);
  }

  static void pauseAutomationDrainBeforeDescriptor(
      EffeTuneProcessor &processor, const bool pause) noexcept {
#if defined(EFFETUNE_PROCESSOR_TEST_HOOKS)
    processor.pauseAutomationDrainBeforeDescriptorForTesting_.store(
        pause, std::memory_order_release);
#else
    (void)processor;
    (void)pause;
#endif
  }

  [[nodiscard]] static bool automationDrainPausedBeforeDescriptor(
      const EffeTuneProcessor &processor) noexcept {
#if defined(EFFETUNE_PROCESSOR_TEST_HOOKS)
    return processor.automationDrainPausedBeforeDescriptorForTesting_.load(
        std::memory_order_acquire);
#else
    (void)processor;
    return false;
#endif
  }

  static void drainAutomationValues(EffeTuneProcessor &processor) {
    processor.drainAutomationValues();
  }

  // The held-gesture carrier the same 50 ms control-service timer runs. It is
  // the observation point, not the trigger: it reads processBlockEpoch_ on a
  // thread that is allowed to call into the host, and reports nothing until a
  // process() boundary has moved that epoch.
  static void serviceHeldHostEdits(EffeTuneProcessor &processor) noexcept {
    processor.serviceHeldHostEdits();
  }

  // The gesture transaction every explicit automation path runs, reached
  // without the bridge's own leading drain in front of it -- which is what
  // lets the test place a published-but-undrained block before the gesture.
  [[nodiscard]] static bool
  applyAutomationEdit(EffeTuneProcessor &processor,
                      const AutomationTargetIdentity &identity,
                      const double normalized,
                      const EffeTuneProcessor::AutomationEditIntent intent = {}) {
    return processor.applyAutomationEdit(identity, normalized, intent) ==
           EffeTuneProcessor::AutomationEditOutcome::bound;
  }

  // Whether the plug-in is still telling the host that the user's hand is on
  // this control, which is what makes the block ignore the host's own input for
  // it. Reached through the same wait-free load the audio callback makes.
  [[nodiscard]] static bool
  hostGestureOpen(const EffeTuneProcessor &processor,
                  const Steinberg::Vst::ParamID parameterId) noexcept {
    return processor.hostGestureOpen(parameterId);
  }

  // Whether another thread could take processingResourcesMutex_ right now. The
  // probe runs on a thread of its own because the caller is normally the thread
  // that would be holding it, and try_lock() on a mutex the calling thread
  // already owns is undefined. It retries until the deadline, so the spurious
  // failure try_lock() is permitted cannot decide the answer.
  [[nodiscard]] static bool
  processingResourcesLockable(EffeTuneProcessor &processor,
                              const std::chrono::milliseconds localTimeout) {
    auto acquired = false;
    std::thread prober([&processor, &acquired, localTimeout] {
      const auto limit = std::chrono::steady_clock::now() + localTimeout * 10;
      do {
        std::unique_lock probe(processor.processingResourcesMutex_, std::try_to_lock);
        if (probe.owns_lock()) {
          acquired = true;
          return;
        }
        std::this_thread::yield();
      } while (std::chrono::steady_clock::now() < limit);
    });
    prober.join();
    return acquired;
  }

  [[nodiscard]] static std::uint64_t
  blockEpoch(const EffeTuneProcessor &processor) noexcept {
    return processor.processBlockEpoch_.load(std::memory_order_seq_cst);
  }

  // The same hand-off pipeline/updatePlugin performs for a slider drag, without
  // the JSON round trip: the Debug allocation guard is process-wide, so a test
  // thread that runs alongside blocks has to stay allocation-free.
  [[nodiscard]] static bool publishParameterImage(
      EffeTuneProcessor &processor, AudioCommand &command,
      const std::uint32_t logicalId, const std::uint32_t paramsHash,
      const float packed) noexcept {
    command.type = AudioCommandType::setParameters;
    command.logicalId = logicalId;
    command.paramsHash = paramsHash;
    command.floatCount = 1;
    command.parameterByteCount = 0;
    command.packed[0] = packed;
    if (!processor.parameterMailbox_.publish(command)) {
      return false;
    }
    processor.parameterImageGeneration_.fetch_add(1, std::memory_order_acq_rel);
    return true;
  }

  // The same hand-off for a multi-parameter instance, which is what a drag on a
  // delay-bearing control publishes.
  [[nodiscard]] static bool publishParameterImage(
      EffeTuneProcessor &processor, AudioCommand &command,
      const std::uint32_t logicalId, const std::uint32_t paramsHash,
      const std::span<const float> packed) noexcept {
    command.type = AudioCommandType::setParameters;
    command.logicalId = logicalId;
    command.paramsHash = paramsHash;
    command.floatCount = static_cast<std::uint32_t>(packed.size());
    command.parameterByteCount = 0;
    std::copy(packed.begin(), packed.end(), command.packed.begin());
    if (!processor.parameterMailbox_.publish(command)) {
      return false;
    }
    processor.parameterImageGeneration_.fetch_add(1, std::memory_order_acq_rel);
    return true;
  }

  static void failPipelinePlanRefreshes(EffeTuneProcessor &processor,
                                        const std::uint32_t count) noexcept {
#if defined(EFFETUNE_PROCESSOR_TEST_HOOKS)
    processor.pipelinePlanRefreshFailuresForTesting_ = count;
#else
    (void)processor;
    (void)count;
#endif
  }

  [[nodiscard]] static std::uint32_t
  remainingPipelinePlanRefreshFailures(const EffeTuneProcessor &processor) noexcept {
#if defined(EFFETUNE_PROCESSOR_TEST_HOOKS)
    return processor.pipelinePlanRefreshFailuresForTesting_;
#else
    (void)processor;
    return 0;
#endif
  }

  [[nodiscard]] static std::uint64_t
  processFailureSequence(const EffeTuneProcessor &processor) noexcept {
    return processor.processTransactionFailureSequence_.load(std::memory_order_acquire);
  }

  // Which transaction the failure burst last recorded. Reported as the raw
  // enumerator so the tests can name the reason a diagnostic carries without
  // matching on its user-facing wording.
  [[nodiscard]] static int
  lastProcessFailure(const EffeTuneProcessor &processor) noexcept {
    return static_cast<int>(
        processor.lastProcessTransactionError_.load(std::memory_order_acquire));
  }

  // The two values the audio callback reads to tell a control thread holding
  // the engine from a DSP that cannot process at all.
  [[nodiscard]] static bool
  processingReady(const EffeTuneProcessor &processor) noexcept {
    return processor.processingReady_.load(std::memory_order_seq_cst);
  }

  [[nodiscard]] static std::uint32_t
  controlEngineClaims(const EffeTuneProcessor &processor) noexcept {
    return processor.controlEngineClaims_.load(std::memory_order_seq_cst);
  }

  static void deferPipelinePlanRetry(EffeTuneProcessor &processor,
                                     const std::chrono::milliseconds delay) noexcept {
    processor.pipelinePlanRetryDeadline_ = std::chrono::steady_clock::now() + delay;
  }

  [[nodiscard]] static bool setAsset(EffeTuneProcessor &processor,
                                     RuntimeAsset asset,
                                     std::string *error = nullptr) {
    return processor.engine_.setAsset(std::move(asset), error);
  }

  [[nodiscard]] static bool clearAsset(EffeTuneProcessor &processor,
                                       const std::uint32_t logicalId,
                                       const std::uint32_t slot) {
    return processor.engine_.clearAsset(logicalId, slot);
  }

  [[nodiscard]] static std::uint32_t assetState(
      const EffeTuneProcessor &processor, const std::uint32_t logicalId,
      const std::uint32_t slot) {
    return processor.engine_.assetState(logicalId, slot);
  }

  [[nodiscard]] static std::uint64_t
  latencyRevision(const EffeTuneProcessor &processor) noexcept {
    return processor.engine_.latencyRevision();
  }

  [[nodiscard]] static std::optional<std::uint32_t>
  activeAutomationSlot(const EffeTuneProcessor &processor,
                       const AutomationTargetIdentity &identity) noexcept {
    return processor.automationBindings_.findActiveSlot(identity);
  }

  [[nodiscard]] static std::optional<std::uint32_t>
  bindAutomationSlot(EffeTuneProcessor &processor,
                     const AutomationTargetIdentity &identity) {
    return processor.bindAutomationSlot(identity);
  }

  // The value the audio thread is playing for a slot, which only a consumed
  // block can change. It is what actually reaches the DSP every block.
  [[nodiscard]] static double
  playedAutomationValue(const EffeTuneProcessor &processor,
                        const std::uint32_t slot) noexcept {
    return processor.automationScheduler_.currentNormalized(slot);
  }

  [[nodiscard]] static float
  runtimePackedParameter(EffeTuneProcessor &processor, const std::uint32_t logicalId,
                         const std::size_t offset) {
    std::scoped_lock resources(processor.processingResourcesMutex_);
    const auto runtime = std::find_if(
        processor.runtimePlugins_.begin(), processor.runtimePlugins_.end(),
        [logicalId](const RuntimePlugin &plugin) {
          return plugin.logicalId == logicalId;
        });
    if (runtime == processor.runtimePlugins_.end() ||
        offset >= runtime->packedParameters.size()) {
      throw std::runtime_error("the runtime plug-in image is not available");
    }
    return runtime->packedParameters[offset];
  }

  [[nodiscard]] static std::size_t
  runtimePluginCount(EffeTuneProcessor &processor,
                     const std::uint32_t logicalId) {
    std::scoped_lock resources(processor.processingResourcesMutex_);
    return static_cast<std::size_t>(std::count_if(
        processor.runtimePlugins_.begin(), processor.runtimePlugins_.end(),
        [logicalId](const RuntimePlugin &plugin) {
          return plugin.logicalId == logicalId;
        }));
  }

  [[nodiscard]] static bool
  runtimeContextuallyBypassed(EffeTuneProcessor &processor,
                              const std::uint32_t logicalId) {
    std::scoped_lock resources(processor.processingResourcesMutex_);
    const auto runtime = std::find_if(
        processor.runtimePlugins_.begin(), processor.runtimePlugins_.end(),
        [logicalId](const RuntimePlugin &plugin) {
          return plugin.logicalId == logicalId;
        });
    return runtime != processor.runtimePlugins_.end() &&
           runtime->contextuallyBypassed;
  }

  [[nodiscard]] static std::uint32_t
  enginePipelineLatency(EffeTuneProcessor &processor) {
    std::scoped_lock resources(processor.processingResourcesMutex_);
    return processor.engine_.pipelineLatency();
  }

  // Stands in for the control service holding the claim across a block without
  // also holding processingResourcesMutex_, which is what lets the test observe
  // the audio thread's own decision.
  static void setControlOwnsRuntimeImage(EffeTuneProcessor &processor,
                                         const bool owned) noexcept {
    processor.controlOwnsRuntimeImage_.store(owned, std::memory_order_seq_cst);
  }

  // A stale parameter hash is the shortest route to a block the engine
  // rejects, which is the only path that used to write the dirty flags from
  // outside the staging guard. Returns the hash it replaced.
  [[nodiscard]] static std::uint32_t
  swapRuntimeParamsHash(EffeTuneProcessor &processor, const std::uint32_t logicalId,
                        const std::uint32_t paramsHash) {
    std::scoped_lock resources(processor.processingResourcesMutex_);
    const auto runtime = std::find_if(
        processor.runtimePlugins_.begin(), processor.runtimePlugins_.end(),
        [logicalId](const RuntimePlugin &plugin) {
          return plugin.logicalId == logicalId;
        });
    if (runtime == processor.runtimePlugins_.end()) {
      throw std::runtime_error("the runtime plug-in image is not available");
    }
    return std::exchange(runtime->paramsHash, paramsHash);
  }

  [[nodiscard]] static bool runtimeParameterDirty(EffeTuneProcessor &processor,
                                                  const std::uint32_t logicalId) {
    std::scoped_lock resources(processor.processingResourcesMutex_);
    const auto runtime = std::find_if(
        processor.runtimePlugins_.begin(), processor.runtimePlugins_.end(),
        [logicalId](const RuntimePlugin &plugin) {
          return plugin.logicalId == logicalId;
        });
    if (runtime == processor.runtimePlugins_.end()) {
      throw std::runtime_error("the runtime plug-in image is not available");
    }
    return processor.runtimeParameterDirty_[static_cast<std::size_t>(
        runtime - processor.runtimePlugins_.begin())];
  }

  static void setControlOwnsAudioTimeline(EffeTuneProcessor &processor,
                                          const bool owned) noexcept {
    processor.controlOwnsAudioTimeline_.store(owned, std::memory_order_seq_cst);
  }

  [[nodiscard]] static bool
  controlOwnsAudioTimeline(const EffeTuneProcessor &processor) noexcept {
    return processor.controlOwnsAudioTimeline_.load(std::memory_order_seq_cst);
  }

  // Stands in for a block that is inside process(): the epoch is odd exactly
  // while the callback is executing, so a control thread that waits for
  // quiescence cannot return until the block is released.
  static void beginSyntheticBlock(EffeTuneProcessor &processor) noexcept {
    processor.processBlockEpoch_.fetch_add(1, std::memory_order_seq_cst);
  }

  static void endSyntheticBlock(EffeTuneProcessor &processor) noexcept {
    processor.processBlockEpoch_.fetch_add(1, std::memory_order_seq_cst);
  }
};

} // namespace effetune::vst::plugin

namespace {

using namespace Steinberg;
using namespace Steinberg::Vst;
using effetune::vst::plugin::EffeTuneProcessor;
using effetune::vst::plugin::PluginProcessorTestAccess;

using effetune::vst::plugin::kFirstAutomationParameterId;
using effetune::vst::plugin::kLastAutomationParameterId;
using effetune::vst::plugin::kBypassParameterId;

// Stands in for a host that offers the extended handler as well as the basic
// one. EditController::setComponentHandler() queries IComponentHandler2 off the
// handler it is given, so implementing it here is what puts the plug-in's
// startGroupEdit()/finishGroupEdit() calls on a real path to a host.
class TestComponentHandler final : public IComponentHandler,
                                   public IComponentHandler2 {
public:
  TestComponentHandler() { FUNKNOWN_CTOR }
  ~TestComponentHandler() { FUNKNOWN_DTOR }

  struct HostEdit {
    ParamID id = 0;
    ParamValue value = 0.0;
    // Where the host would write this point. performEdit carries no time of its
    // own (ivsteditcontroller.h:226-230), so a host can only stamp it with the
    // position it is rendering when the call arrives: every call made between
    // two process() blocks lands on one and the same position, and of two
    // points sharing a position a lane keeps the later. The tests that measure
    // a recorded lane advance `lanePosition` exactly when they render a block.
    std::int64_t lanePosition = 0;
  };

  // The shape of a touch as the host sees it. A host keys its automation writer
  // on the window between begin and end, so the order and the count of these
  // are the behaviour under test, not just the values inside them. The group
  // boundaries share the log: what a host records depends on the begins and the
  // ends falling inside the group, not just on all four being called.
  // restartComponent shares the log for the same reason the group boundaries
  // do: what matters is where it falls relative to them. A host answers a
  // restart by re-reading the parameter bank, and being asked to do that while
  // it believes a group edit is open is the thing under test.
  enum class EditStep : std::uint8_t {
    begin,
    perform,
    end,
    startGroup,
    finishGroup,
    restart
  };

  struct EditRecord {
    EditStep step = EditStep::begin;
    ParamID id = 0;
  };

  void recordStep(const EditStep step, const ParamID id) noexcept {
    if (editLogCount < editLog.size()) {
      editLog[editLogCount] = {step, id};
    }
    ++editLogCount;
  }

  tresult PLUGIN_API beginEdit(const ParamID id) override {
    recordStep(EditStep::begin, id);
    return kResultOk;
  }
  tresult PLUGIN_API performEdit(const ParamID id, const ParamValue value) override {
    // A fixed record keeps the Debug allocation guard out of the picture: an
    // edit can be sent from a control thread while blocks are flowing.
    if (performedEditCount < performedEdits.size()) {
      performedEdits[performedEditCount] = {id, value, lanePosition};
    }
    ++performedEditCount;
    recordStep(EditStep::perform, id);
    // Stands in for a host that declines the edit transaction -- no Write lane
    // armed, a read-only pass, an automation writer that is not listening. The
    // plug-in adopts the value regardless, so this changes what the host
    // records and nothing else.
    return refuseEdits ? kResultFalse : kResultOk;
  }
  tresult PLUGIN_API endEdit(const ParamID id) override {
    recordStep(EditStep::end, id);
    // A host processes endEdit inline, and what it does with it -- marking the
    // project dirty, and in principle asking for the state, restoring one or
    // suspending the component -- re-enters the plug-in through a mutex that is
    // not recursive. Where the tests care, this stands in for that re-entry.
    if (lockProbe != nullptr &&
        !PluginProcessorTestAccess::processingResourcesLockable(
            *lockProbe, std::chrono::milliseconds{250})) {
      endEditFoundResourcesLocked = true;
    }
    return kResultOk;
  }

  [[nodiscard]] std::size_t stepCount(const EditStep step) const noexcept {
    return static_cast<std::size_t>(
        std::count_if(editLog.begin(),
                      editLog.begin() +
                          static_cast<std::ptrdiff_t>(
                              std::min(editLogCount, editLog.size())),
                      [step](const EditRecord &record) { return record.step == step; }));
  }

  void clearEditLog() noexcept {
    editLogCount = 0;
    performedEditCount = 0;
  }
  tresult PLUGIN_API restartComponent(const int32 flags) override {
    restartFlags |= flags;
    if ((flags & RestartFlags::kLatencyChanged) != 0) {
      ++latencyRestartCount;
      if (latencyProbe != nullptr &&
          latencyProbe->getLatencySamples() !=
              PluginProcessorTestAccess::enginePipelineLatency(*latencyProbe)) {
        latencyMismatch = true;
      }
    }
    recordStep(EditStep::restart, 0);
    // A host processes restartComponent inline too, and re-reading the
    // parameter bank or writing one back through setParamNormalized re-enters
    // the plug-in through the same non-recursive mutex. Opt-in, because the
    // probe costs a thread and only the tests that care about where a restart
    // is issued from need it.
    if (probeRestartLock && lockProbe != nullptr) {
      ++probedRestartCount;
      if (!PluginProcessorTestAccess::processingResourcesLockable(
              *lockProbe, std::chrono::milliseconds{250})) {
        restartFoundResourcesLocked = true;
      }
    }
    return kResultOk;
  }

  //---from IComponentHandler2---------------------------------------------
  tresult PLUGIN_API setDirty(TBool /*state*/) override {
    ++unloggedHandlerCallCount;
    return kResultOk;
  }
  tresult PLUGIN_API requestOpenEditor(FIDString /*name*/) override {
    ++unloggedHandlerCallCount;
    return kResultFalse;
  }
  tresult PLUGIN_API startGroupEdit() override {
    recordStep(EditStep::startGroup, 0);
    ++openGroupDepth;
    maximumGroupDepth = std::max(maximumGroupDepth, openGroupDepth);
    return kResultOk;
  }
  tresult PLUGIN_API finishGroupEdit() override {
    recordStep(EditStep::finishGroup, 0);
    if (openGroupDepth > 0) {
      --openGroupDepth;
    } else {
      unbalancedGroupFinish = true;
    }
    return kResultOk;
  }

  DECLARE_FUNKNOWN_METHODS

  int32 restartFlags = 0;
  std::uint32_t latencyRestartCount = 0;
  std::array<HostEdit, 16> performedEdits{};
  std::size_t performedEditCount = 0;
  // The transport position the host would stamp the next performEdit with.
  // Advanced only by the tests that render blocks between reports.
  std::int64_t lanePosition = 0;
  std::array<EditRecord, 64> editLog{};
  std::size_t editLogCount = 0;
  bool refuseEdits = false;
  std::size_t openGroupDepth = 0;
  std::size_t maximumGroupDepth = 0;
  bool unbalancedGroupFinish = false;
  // Set only by the tests that care where an endEdit is issued from.
  EffeTuneProcessor *lockProbe = nullptr;
  bool endEditFoundResourcesLocked = false;
  bool probeRestartLock = false;
  bool restartFoundResourcesLocked = false;
  std::size_t probedRestartCount = 0;
  EffeTuneProcessor *latencyProbe = nullptr;
  bool latencyMismatch = false;
  // The two handler methods that leave no record in the edit log. Counted so a
  // test can assert that a code path made no IComponentHandler or
  // IComponentHandler2 call of any kind, not merely no edit.
  std::size_t unloggedHandlerCallCount = 0;

  // Every call the plug-in has made into the host through either handler
  // interface.
  [[nodiscard]] std::size_t handlerCallCount() const noexcept {
    return editLogCount + unloggedHandlerCallCount;
  }
};

IMPLEMENT_REFCOUNT(TestComponentHandler)

// Two interfaces, so the single-interface convenience macro will not do. The
// extended handler has to be reachable through queryInterface or the base class
// never finds it and every group edit the plug-in issues goes nowhere.
tresult PLUGIN_API TestComponentHandler::queryInterface(const TUID iid, void **obj) {
  QUERY_INTERFACE(iid, obj, FUnknown::iid, IComponentHandler)
  QUERY_INTERFACE(iid, obj, IComponentHandler::iid, IComponentHandler)
  QUERY_INTERFACE(iid, obj, IComponentHandler2::iid, IComponentHandler2)
  *obj = nullptr;
  return kNoInterface;
}

void expect(const bool condition, const std::string &message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

// Holds processingResourcesMutex_ on another thread for the lifetime of the
// scope. It stands in for a control thread that is mid-edit while blocks keep
// arriving, which is the case the audio callback used to try_lock.
class HeldControlGuard {
public:
  explicit HeldControlGuard(EffeTuneProcessor &processor) {
    owner_ = std::thread([this, &processor] {
      auto resources =
          PluginProcessorTestAccess::lockProcessingResources(processor);
      {
        std::scoped_lock lock(mutex_);
        held_ = true;
      }
      changed_.notify_one();
      std::unique_lock lock(mutex_);
      changed_.wait(lock, [this] { return released_; });
    });
    std::unique_lock lock(mutex_);
    changed_.wait(lock, [this] { return held_; });
  }

  HeldControlGuard(const HeldControlGuard &) = delete;
  HeldControlGuard &operator=(const HeldControlGuard &) = delete;

  ~HeldControlGuard() {
    {
      std::scoped_lock lock(mutex_);
      released_ = true;
    }
    changed_.notify_one();
    owner_.join();
  }

private:
  std::mutex mutex_;
  std::condition_variable changed_;
  std::thread owner_;
  bool held_ = false;
  bool released_ = false;
};

template <typename Predicate>
[[nodiscard]] bool pumpMainThreadUntil(Predicate &&predicate,
                                       const std::chrono::milliseconds localTimeout) {
  // Existing passing local budgets are conservative upper bounds for CI waits.
  const auto deadline = std::chrono::steady_clock::now() + localTimeout * 10;
  while (!predicate()) {
#if defined(_WIN32)
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != FALSE) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
#endif
    if (std::chrono::steady_clock::now() >= deadline) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return true;
}

// Slots are only assigned when the user actually edits a parameter, so a test
// that needs a live lane binds the target explicitly first.
[[nodiscard]] ParamID
boundAutomationParameterId(EffeTuneProcessor &processor,
                           const effetune::vst::AutomationTargetIdentity &identity) {
  expect(!PluginProcessorTestAccess::activeAutomationSlot(processor, identity).has_value(),
         "an untouched target starts without an automation slot");
  const auto slot = PluginProcessorTestAccess::bindAutomationSlot(processor, identity);
  expect(slot.has_value(), "bind the automation target on demand");

  ParameterInfo info{};
  expect(processor.getParameterInfo(static_cast<int32>(*slot + 1u), info) == kResultOk &&
             info.id == effetune::vst::plugin::automationParameterId(*slot) &&
             (info.flags & ParameterInfo::kCanAutomate) != 0 &&
             (info.flags &
              (ParameterInfo::kIsHidden | ParameterInfo::kIsReadOnly)) == 0,
         "resolve the active automation parameter metadata");
  return info.id;
}

[[nodiscard]] choc::value::ValueView findAutomationDelta(
    const choc::value::Value &result,
    const effetune::vst::AutomationTargetIdentity &identity) {
  for (const auto delta : result["automationDeltas"]) {
    if (delta["pipeline"].getWithDefault<std::string>({}) ==
            std::string(1, identity.pipeline) &&
        delta["pluginId"].getWithDefault<std::int64_t>(-1) == identity.pluginId &&
        delta["pluginType"].getWithDefault<std::string>({}) == identity.pluginType &&
        delta["parameterKey"].getWithDefault<std::string>({}) == identity.parameterKey &&
        delta["elementIndex"].getWithDefault<std::int64_t>(-1) == identity.elementIndex) {
      return delta;
    }
  }
  return {};
}

[[nodiscard]] choc::value::ValueView findExecutionState(
    const choc::value::Value &result, const std::uint32_t pluginId) {
  for (const auto state : result["executionStates"]) {
    if (state["pluginId"].getWithDefault<std::int64_t>(-1) == pluginId) {
      return state;
    }
  }
  return {};
}

[[nodiscard]] std::string asciiString(const TChar *value) {
  std::array<char, 256> buffer{};
  Steinberg::UString(const_cast<TChar *>(value), 128)
      .toAscii(buffer.data(), static_cast<int32>(buffer.size()));
  return buffer.data();
}

// What a host would put in an automation lane or a generic-editor row.
[[nodiscard]] std::string displayString(EffeTuneProcessor &processor,
                                        const ParamID id,
                                        const ParamValue normalized) {
  String128 rendered{};
  expect(processor.getParamStringByValue(id, normalized, rendered) == kResultTrue,
         "render a display string for the parameter");
  return asciiString(rendered);
}

// The host hands typed text in as UTF-16; the tests only ever type ASCII.
[[nodiscard]] std::vector<TChar> utf16Of(const std::string &value) {
  std::vector<TChar> buffer(value.size() + 1u, 0);
  Steinberg::UString(buffer.data(), static_cast<int32>(buffer.size()))
      .fromAscii(value.c_str());
  return buffer;
}

[[nodiscard]] ProcessSetup setup(const double sampleRate, const int32 maxFrames) {
  ProcessSetup result{};
  result.processMode = kRealtime;
  result.symbolicSampleSize = kSample32;
  result.maxSamplesPerBlock = maxFrames;
  result.sampleRate = sampleRate;
  return result;
}

[[nodiscard]] choc::value::Value hostInfo(EffeTuneProcessor &processor) {
  return choc::json::parse(processor.handleUiMessage(
      R"({"type":"host/getInfo","payload":{}})"));
}

void installGainPipeline(EffeTuneProcessor &processor) {
  const auto response = choc::json::parse(processor.handleUiMessage(
      R"({"type":"pipeline/rebuild","payload":{"pipeline":"A","plugins":[)"
      R"({"id":1,"type":"VolumePlugin","name":"Volume","enabled":true,)"
      R"("parameters":{"vl":-6},"wasmParams":[-6],)"
      R"("wasmParamsHash":1719233191}]}})"));
  expect(response["ok"].getWithDefault<bool>(false), "install the native gain pipeline");
}

// Eligible targets and the registry projection must describe the same state
// generation. Both entry points used to build the catalog before taking the
// control transaction lock, allowing a concurrent removal to land in between
// and leaving the removed target active (and its never-reusable slot spent).
void testAutomationCatalogProjectionIsOneControlTransaction() {
  const auto runRace = [](const bool bindTarget) {
    auto processor = std::make_unique<EffeTuneProcessor>();
    expect(processor->initialize(nullptr) == kResultOk,
           "initialize the automation catalog transaction test");
    auto processSetup = setup(48000.0, 64);
    expect(processor->setupProcessing(processSetup) == kResultOk,
           "prepare the automation catalog transaction test");
    const auto installed = choc::json::parse(processor->handleUiMessage(
        R"({"type":"pipeline/rebuild","payload":{"pipeline":"A","plugins":[)"
        R"({"id":601,"type":"DCOffsetPlugin","name":"DC Offset","enabled":true,)"
        R"("parameters":{"of":0.25},"wasmParams":[0.25],)"
        R"("wasmParamsHash":1104945464}]}})"));
    expect(installed["ok"].getWithDefault<bool>(false),
           "install the automation catalog transaction pipeline");
    const effetune::vst::AutomationTargetIdentity identity{
        'A', 601, "DCOffsetPlugin", "of", 0};
    if (!bindTarget) {
      (void)boundAutomationParameterId(*processor, identity);
    }

    PluginProcessorTestAccess::pauseAutomationCatalogBeforeProjection(
        *processor, true);
    std::optional<std::uint32_t> bound;
    std::thread projector([&] {
      if (bindTarget) {
        bound = PluginProcessorTestAccess::bindAutomationSlot(*processor,
                                                               identity);
      } else {
        PluginProcessorTestAccess::synchronizeAutomationBindings(*processor);
      }
    });
    const auto paused = pumpMainThreadUntil(
        [&] {
          return PluginProcessorTestAccess::automationCatalogPausedBeforeProjection(
              *processor);
        },
        std::chrono::milliseconds(500));

    std::string removalResponse;
    std::atomic_bool removalComplete{false};
    std::thread remover([&] {
      removalResponse = processor->handleUiMessage(
          R"({"type":"pipeline/rebuild","payload":{"pipeline":"A","plugins":[]}})"
      );
      removalComplete.store(true, std::memory_order_release);
    });
    const auto probeDeadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(40);
    while (!removalComplete.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < probeDeadline) {
      std::this_thread::yield();
    }
    const auto removalCompletedBeforeProjection =
        removalComplete.load(std::memory_order_acquire);
    PluginProcessorTestAccess::pauseAutomationCatalogBeforeProjection(
        *processor, false);
    projector.join();
    remover.join();

    const auto removed = choc::json::parse(removalResponse);
    expect(paused && !removalCompletedBeforeProjection &&
               removed["ok"].getWithDefault<bool>(false),
           "a topology replacement cannot cross the catalog projection");
    expect(!PluginProcessorTestAccess::activeAutomationSlot(*processor,
                                                            identity)
                .has_value(),
           "the removed target is not projected from a stale catalog");
    if (bindTarget) {
      expect(bound.has_value(),
             "the binding transaction completed before the queued removal");
    }
    expect(processor->terminate() == kResultOk,
           "terminate the automation catalog transaction test");
  };

  runRace(false);
  runRace(true);
}

void installLimiterPipeline(EffeTuneProcessor &processor, const std::uint32_t id,
                            const float lookahead = 3.0f) {
  const auto plugin =
      std::string{"{\"id\":"} + std::to_string(id) +
      R"(,"type":"BrickwallLimiterPlugin","name":"Brickwall Limiter","enabled":true,)" +
      R"("parameters":{"th":0,"rl":100,"la":)" + std::to_string(lookahead) +
      R"(,"os":1,"ig":0,"sm":-1},"wasmParams":[0,100,)" +
      std::to_string(lookahead) +
      R"(,1,0,-1],"wasmParamsHash":3039928906})";
  const auto response = choc::json::parse(processor.handleUiMessage(
      std::string{R"({"type":"pipeline/rebuild","payload":{"pipeline":"A","plugins":[)"} +
      plugin + "]}}"));
  expect(response["ok"].getWithDefault<bool>(false),
         "install the native limiter pipeline");
}

void updateLimiterPlugin(EffeTuneProcessor &processor, const std::uint32_t id,
                         const float lookahead, const bool enabled = true) {
  const auto plugin =
      std::string{"{\"id\":"} + std::to_string(id) +
      R"(,"type":"BrickwallLimiterPlugin","name":"Brickwall Limiter","enabled":)" +
      (enabled ? "true" : "false") +
      R"(,"parameters":{"th":0,"rl":100,"la":)" + std::to_string(lookahead) +
      R"(,"os":1,"ig":0,"sm":-1},"wasmParams":[0,100,)" +
      std::to_string(lookahead) +
      R"(,1,0,-1],"wasmParamsHash":3039928906})";
  const auto response = choc::json::parse(processor.handleUiMessage(
      std::string{R"({"type":"pipeline/updatePlugin","payload":{"pipeline":"A","plugin":)"} +
      plugin + "}}"));
  expect(response["ok"].getWithDefault<bool>(false),
         "update the native limiter through pipeline/updatePlugin");
}

void updateLimiterRouting(EffeTuneProcessor &processor, const std::uint32_t id,
                          const std::string_view channel) {
  const auto routing = channel.empty()
                           ? std::string{R"("inputBus":0,"outputBus":0,)"}
                           : std::string{R"("inputBus":0,"outputBus":0,"channel":")"} +
                                 std::string(channel) + R"(",)";
  const auto plugin =
      std::string{"{\"id\":"} + std::to_string(id) +
      R"(,"type":"BrickwallLimiterPlugin","name":"Brickwall Limiter","enabled":true,)" +
      routing +
      R"("parameters":{"th":0,"rl":100,"la":3,"os":1,"ig":0,"sm":-1},)" +
      R"("wasmParams":[0,100,3,1,0,-1],"wasmParamsHash":3039928906})";
  const auto response = choc::json::parse(processor.handleUiMessage(
      std::string{R"({"type":"pipeline/updatePlugin","payload":{"pipeline":"A","plugin":)"} +
      plugin + "}}"));
  expect(response["ok"].getWithDefault<bool>(false),
         "update the native limiter routing through pipeline/updatePlugin");
}

void installParallelLimiterPipeline(EffeTuneProcessor &processor) {
  const auto response = choc::json::parse(processor.handleUiMessage(
      R"({"type":"pipeline/rebuild","payload":{"pipeline":"A","plugins":[)"
      R"({"id":41,"type":"BrickwallLimiterPlugin","name":"Left Limiter",)"
      R"("enabled":true,"inputBus":0,"outputBus":0,"channel":"L",)"
      R"("parameters":{"th":0,"rl":100,"la":1,"os":1,"ig":0,"sm":-1},)"
      R"("wasmParams":[0,100,1,1,0,-1],"wasmParamsHash":3039928906},)"
      R"({"id":42,"type":"BrickwallLimiterPlugin","name":"Right Limiter",)"
      R"("enabled":true,"inputBus":0,"outputBus":0,"channel":"R",)"
      R"("parameters":{"th":0,"rl":100,"la":3,"os":1,"ig":0,"sm":-1},)"
      R"("wasmParams":[0,100,3,1,0,-1],"wasmParamsHash":3039928906}]}})"));
  expect(response["ok"].getWithDefault<bool>(false),
         "install the parallel limiter pipeline");
}

void installIrReverbPipeline(EffeTuneProcessor &processor) {
  const auto response = choc::json::parse(processor.handleUiMessage(
      R"({"type":"pipeline/rebuild","payload":{"pipeline":"A","plugins":[)"
      R"({"id":91,"type":"IRReverbPlugin","name":"IR Reverb","enabled":true,)"
      R"("parameters":{"cm":"mono","lt":"128","cr":1,"dw":0,"de":1,"dl":-96,"pd":0},)"
      R"("wasmParams":[0,1,1,0,1,-96,0],"wasmParamsHash":2529061658}]}})"));
  expect(response["ok"].getWithDefault<bool>(false),
         "install the IR reverb pipeline");
}

[[nodiscard]] effetune::vst::RuntimeAsset makeIrReverbAsset() {
  constexpr std::uint32_t frames = 600;
  effetune::vst::RuntimeAsset asset;
  asset.logicalId = 91;
  asset.slot = 0;
  asset.formatTag = ET_ASSET_F32_MULTICH;
  asset.channels = 1;
  asset.frames = frames;
  asset.topology = 1;
  asset.headBlock = 128;
  asset.rateDivider = 1;
  asset.processingChannels = 2;
  asset.payload.resize(32u + static_cast<std::size_t>(frames) * sizeof(float));
  const auto writeU32 = [&asset](const std::size_t offset,
                                 const std::uint32_t value) {
    std::memcpy(asset.payload.data() + offset, &value, sizeof(value));
  };
  writeU32(0, 0x31415445u);
  writeU32(4, asset.channels);
  writeU32(8, asset.frames);
  writeU32(12, 48000u);
  writeU32(16, asset.topology);
  writeU32(20, 0u);
  constexpr float impulse = 1.0f;
  std::memcpy(asset.payload.data() + 32u, &impulse, sizeof(impulse));
  asset.footprintBytes = 4u * 1024u * 1024u;
  return asset;
}

void installGroupDelayEqPipeline(EffeTuneProcessor &processor) {
  const auto response = choc::json::parse(processor.handleUiMessage(
      R"({"type":"pipeline/rebuild","payload":{"pipeline":"A","plugins":[)"
      R"({"id":92,"type":"GroupDelayEqPlugin","name":"Group Delay EQ","enabled":true,)"
      R"("parameters":{"lt":"128","fd":8192,"tp":16384,"d0":0},)"
      R"("wasmParams":[128,8192],"wasmParamsHash":2941825417}]}})"));
  expect(response["ok"].getWithDefault<bool>(false),
         "install the Group Delay EQ pipeline");
}

[[nodiscard]] effetune::vst::RuntimeAsset makeGroupDelayEqAsset() {
  auto asset = makeIrReverbAsset();
  asset.logicalId = 92;
  return asset;
}

void expectGainProcessing(EffeTuneProcessor &processor, const int32 frames) {
  std::array<float, 256> left{};
  std::array<float, 256> right{};
  std::array<float, 256> outputLeft{};
  std::array<float, 256> outputRight{};
  left.fill(1.0f);
  right.fill(-1.0f);
  Sample32 *inputChannels[]{left.data(), right.data()};
  Sample32 *outputChannels[]{outputLeft.data(), outputRight.data()};
  AudioBusBuffers input{};
  input.numChannels = 2;
  input.channelBuffers32 = inputChannels;
  AudioBusBuffers output{};
  output.numChannels = 2;
  output.channelBuffers32 = outputChannels;
  ProcessData data{};
  data.processMode = kRealtime;
  data.symbolicSampleSize = kSample32;
  data.numSamples = frames;
  data.numInputs = 1;
  data.numOutputs = 1;
  data.inputs = &input;
  data.outputs = &output;

  expect(processor.process(data) == kResultOk, "process the gain pipeline");
  constexpr auto expected = 0.5011872336f;
  for (int32 frame = 0; frame < frames; ++frame) {
    expect(std::abs(outputLeft[static_cast<std::size_t>(frame)] - expected) < 1.0e-6f &&
               std::abs(outputRight[static_cast<std::size_t>(frame)] + expected) < 1.0e-6f,
           "preserve gain processing across host reconfiguration");
  }
}

void testClosedEditorHostAutomationUpdatesStateAndAudio() {
  auto processor = std::make_unique<EffeTuneProcessor>();
  expect(processor->initialize(nullptr) == kResultOk, "initialize closed-editor automation test");
  auto processSetup = setup(48000.0, 64);
  expect(processor->setupProcessing(processSetup) == kResultOk, "prepare automation processing");
  const auto response = choc::json::parse(processor->handleUiMessage(
      R"({"type":"pipeline/rebuild","payload":{"pipeline":"A","plugins":[)"
      R"({"id":9,"type":"DCOffsetPlugin","name":"DC Offset","enabled":true,)"
      R"("parameters":{"of":0},"wasmParams":[0],)"
      R"("wasmParamsHash":1104945464}]}})"));
  expect(response["ok"].getWithDefault<bool>(false), "install DCOffset automation pipeline");
  const auto dcOffsetParameterId = boundAutomationParameterId(
      *processor, {'A', 9, "DCOffsetPlugin", "of", 0});
  const auto previousHostValue = processor->getParamNormalized(dcOffsetParameterId);
  const auto unknownEdit = choc::json::parse(processor->handleUiMessage(
      R"({"type":"automation/edit","payload":{"pipeline":"A","pluginId":9,)"
      R"("pluginType":"DCOffsetPlugin","parameterKey":"offset",)"
      R"("elementIndex":0,"normalized":0.9}})"));
  expect(unknownEdit["ok"].getWithDefault<bool>(false) &&
             !unknownEdit["bound"].getWithDefault<bool>(true) &&
             processor->getParamNormalized(dcOffsetParameterId) == previousHostValue,
         "an unbindable gesture reports bound:false without touching any parameter");
  expect(processor->setActive(true) == kResultOk, "activate automation processing");

  ParameterChanges changes(1);
  int32 queueIndex = 0;
  auto *queue = changes.addParameterData(dcOffsetParameterId, queueIndex);
  expect(queue != nullptr, "create host automation queue");
  int32 pointIndex = 0;
  for (int32 point = 0; point < 70; ++point) {
    const auto value = point == 69 ? 0.75 : static_cast<double>(point) / 100.0;
    expect(queue->addPoint(0, value, pointIndex) == kResultTrue,
           "create overflowing host automation queue");
  }
  std::array<float, 64> inputLeft{};
  std::array<float, 64> inputRight{};
  std::array<float, 64> outputLeft{};
  std::array<float, 64> outputRight{};
  Sample32 *inputChannels[]{inputLeft.data(), inputRight.data()};
  Sample32 *outputChannels[]{outputLeft.data(), outputRight.data()};
  AudioBusBuffers input{};
  input.numChannels = 2;
  input.channelBuffers32 = inputChannels;
  AudioBusBuffers output{};
  output.numChannels = 2;
  output.channelBuffers32 = outputChannels;
  ProcessData data{};
  data.symbolicSampleSize = kSample32;
  data.numSamples = 64;
  data.numInputs = 1;
  data.numOutputs = 1;
  data.inputs = &input;
  data.outputs = &output;
  data.inputParameterChanges = &changes;
  expect(processor->process(data) == kResultOk, "apply closed-editor host automation");
  for (const auto sample : outputLeft) {
    expect(std::abs(sample - 0.5f) < 1.0e-6f,
           "host automation reaches DCOffset audio at the first grid boundary");
  }
  const auto staleRebuild = choc::json::parse(processor->handleUiMessage(
      R"({"type":"pipeline/rebuild","payload":{"pipeline":"A","plugins":[)"
      R"({"id":9,"type":"DCOffsetPlugin","name":"DC Offset","enabled":true,)"
      R"("parameters":{"of":-0.75},"wasmParams":[-0.75],)"
      R"("wasmParamsHash":1104945464}]}})"));
  expect(staleRebuild["ok"].getWithDefault<bool>(false),
          "rebuild a stale WebView full image");
  const auto dcOffsetIdentity =
      effetune::vst::AutomationTargetIdentity{'A', 9, "DCOffsetPlugin", "of", 0};
  const auto staleDelta = findAutomationDelta(staleRebuild, dcOffsetIdentity);
  expect(!staleDelta.isVoid() &&
             std::abs(staleDelta["normalized"].getWithDefault<double>(0.0) - 0.75) <
                 1.0e-12,
         "successful rebuild returns the active host-authoritative snapshot");
  const auto state = choc::json::parse(processor->handleUiMessage(
      R"({"type":"storage/readFile","payload":{"path":"pipeline-state.json"}})"));
  const auto persisted = choc::json::parse(state["content"].getWithDefault<std::string>({}));
  expect(std::abs(persisted["pipelineA"][0]["parameters"]["of"].getWithDefault<double>(0.0) -
                  0.5) < 1.0e-6,
         "a stale full image cannot overwrite the host-adopted parametersJson value");

  double latestNormalized = 0.0;
  for (int32 iteration = 0; iteration < 32; ++iteration) {
    ParameterChanges repeatedChanges(1);
    int32 repeatedQueueIndex = 0;
    auto *repeatedQueue =
        repeatedChanges.addParameterData(dcOffsetParameterId, repeatedQueueIndex);
    int32 repeatedPointIndex = 0;
    latestNormalized = 0.2 + static_cast<double>(iteration) / 100.0;
    expect(repeatedQueue != nullptr &&
               repeatedQueue->addPoint(0, latestNormalized, repeatedPointIndex) == kResultTrue,
           "create repeated closed-editor automation queue");
    data.inputParameterChanges = &repeatedChanges;
    expect(processor->process(data) == kResultOk,
           "process repeated closed-editor automation");
    ResizableMemoryIBStream savedState;
    expect(processor->getState(&savedState) == kResultOk,
           "save repeated closed-editor automation state");
  }
  const auto reconnect = hostInfo(*processor);
  const auto reconnectDelta = findAutomationDelta(reconnect, dcOffsetIdentity);
  expect(!reconnectDelta.isVoid() &&
             std::abs(reconnectDelta["normalized"].getWithDefault<double>(0.0) -
                      latestNormalized) < 1.0e-12,
         "closed-editor state saves coalesce one slot to its latest reconnect delta");

  const auto restoredHistory = choc::json::parse(processor->handleUiMessage(
      R"({"type":"pipeline/restoreHistory","payload":{"pipelineA":[)"
      R"({"id":9,"type":"DCOffsetPlugin","name":"DC Offset","enabled":true,)"
      R"("parameters":{"of":-0.75},"wasmParams":[-0.75],)"
      R"("wasmParamsHash":1104945464}],"pipelineB":null,)"
      R"("pipelineBInitialized":false,"currentPipeline":"A"}})"));
  const auto restoredDelta = findAutomationDelta(restoredHistory, dcOffsetIdentity);
  const auto restoredNormalized =
      restoredDelta["normalized"].getWithDefault<double>(-1.0);
  expect(restoredHistory["ok"].getWithDefault<bool>(false) &&
             !restoredDelta.isVoid() &&
             std::abs(restoredNormalized - latestNormalized) < 1.0e-6,
         "undo/redo restore returns the active host-authoritative snapshot: " +
             std::to_string(restoredHistory["automationDeltas"].size()) + "/" +
             std::to_string(restoredNormalized) + "/" + std::to_string(latestNormalized));
  expect(processor->setActive(false) == kResultOk, "deactivate automation processing");
  expect(processor->terminate() == kResultOk, "terminate closed-editor automation test");
}

void testAutomationWriteGateControlsOnDemandBinding() {
  constexpr const char *editMessage =
      R"({"type":"automation/edit","payload":{"pipeline":"A","pluginId":9,)"
      R"("pluginType":"DCOffsetPlugin","parameterKey":"of",)"
      R"("elementIndex":0,"normalized":0.75}})";
  const effetune::vst::AutomationTargetIdentity identity{
      'A', 9, "DCOffsetPlugin", "of", 0};

  const auto edit = [&](const int32 automationState, const bool notify) {
    struct Outcome {
      std::unique_ptr<EffeTuneProcessor> processor;
      TestComponentHandler *handler = nullptr;
      bool bound = false;
    };
    Outcome outcome;
    outcome.processor = std::make_unique<EffeTuneProcessor>();
    expect(outcome.processor->initialize(nullptr) == kResultOk,
           "initialize the automation write-gate test");
    outcome.handler = new TestComponentHandler();
    expect(outcome.processor->setComponentHandler(outcome.handler) == kResultOk,
           "install the automation write-gate handler");
    auto processSetup = setup(48000.0, 64);
    expect(outcome.processor->setupProcessing(processSetup) == kResultOk,
           "prepare the automation write-gate test");
    const auto installed = choc::json::parse(outcome.processor->handleUiMessage(
        R"({"type":"pipeline/rebuild","payload":{"pipeline":"A","plugins":[)"
        R"({"id":9,"type":"DCOffsetPlugin","name":"DC Offset","enabled":true,)"
        R"("parameters":{"of":0},"wasmParams":[0],)"
        R"("wasmParamsHash":1104945464}]}})"));
    expect(installed["ok"].getWithDefault<bool>(false),
           "install the automation write-gate pipeline");
    expect(!PluginProcessorTestAccess::activeAutomationSlot(*outcome.processor,
                                                            identity)
                .has_value(),
           "adding an effect consumes no automation slot");
    if (notify) {
      expect(outcome.processor->setAutomationState(automationState) == kResultOk,
             "accept the host automation state");
    }
    const auto response =
        choc::json::parse(outcome.processor->handleUiMessage(editMessage));
    expect(response["ok"].getWithDefault<bool>(false),
           "an automation edit never fails the already applied value");
    outcome.bound = response["bound"].getWithDefault<bool>(false);
    expect(outcome.bound ==
               PluginProcessorTestAccess::activeAutomationSlot(*outcome.processor,
                                                               identity)
                   .has_value(),
           "the reported bound flag matches the assigned slot");
    return outcome;
  };

  {
    auto outcome = edit(0, false);
    expect(outcome.bound, "an unreporting host always allows allocation");
    expect(hostInfo(*outcome.processor)["hostAutomationState"]
                   .getWithDefault<std::int64_t>(0) == -1,
           "diagnostics report the unreported host automation state");
    expect(outcome.processor->terminate() == kResultOk,
           "terminate the fallback write-gate case");
    outcome.handler->release();
  }
  {
    auto outcome = edit(IAutomationState::kWriteState, true);
    expect(outcome.bound, "a host in write state allows allocation");
    expect(outcome.processor->terminate() == kResultOk,
           "terminate the write-state case");
    outcome.handler->release();
  }
  {
    auto outcome = edit(IAutomationState::kNoAutomation, true);
    expect(!outcome.bound, "a host outside write state refuses allocation");
    const auto info = hostInfo(*outcome.processor);
    expect(info["hostAutomationState"].getWithDefault<std::int64_t>(-1) == 0,
           "diagnostics report the current host automation state");
    auto warnings = 0;
    for (const auto diagnostic : info["diagnostics"]) {
      if (diagnostic["code"].getWithDefault<std::string>({}) ==
          "automation-write-required") {
        ++warnings;
      }
    }
    expect(warnings == 1, "the refused allocation raises one visible diagnostic");
    (void)choc::json::parse(outcome.processor->handleUiMessage(editMessage));
    const auto repeated = hostInfo(*outcome.processor);
    for (const auto diagnostic : repeated["diagnostics"]) {
      expect(diagnostic["code"].getWithDefault<std::string>({}) !=
                 "automation-write-required",
             "the write-gate diagnostic is reported once per host automation state");
    }
    // The write gate is transient, so a changed host state re-arms the warning.
    expect(outcome.processor->setAutomationState(IAutomationState::kReadState) ==
               kResultOk,
           "accept the changed host automation state");
    (void)choc::json::parse(outcome.processor->handleUiMessage(editMessage));
    const auto rearmed = hostInfo(*outcome.processor);
    auto rearmedWarnings = 0;
    for (const auto diagnostic : rearmed["diagnostics"]) {
      if (diagnostic["code"].getWithDefault<std::string>({}) ==
          "automation-write-required") {
        ++rearmedWarnings;
      }
    }
    expect(rearmedWarnings == 1,
           "a changed host automation state raises the write-gate diagnostic again");
    expect(outcome.processor->setAutomationState(IAutomationState::kReadState) ==
               kResultOk,
           "accept the unchanged host automation state");
    (void)choc::json::parse(outcome.processor->handleUiMessage(editMessage));
    const auto unchanged = hostInfo(*outcome.processor);
    for (const auto diagnostic : unchanged["diagnostics"]) {
      expect(diagnostic["code"].getWithDefault<std::string>({}) !=
                 "automation-write-required",
             "an unchanged host automation state does not re-arm the diagnostic");
    }
    expect(outcome.processor->terminate() == kResultOk,
           "terminate the refused write-gate case");
    outcome.handler->release();
  }
}

// One displayed frame of a drag publishes a single plug-in image together with
// the whole run of gestures that produced it. Every one of them has to reach the
// host in the order the user made them: thinning them coarsens the recorded take.
void testBundledGesturesReachTheHostInCollectedOrder() {
  auto processor = std::make_unique<EffeTuneProcessor>();
  expect(processor->initialize(nullptr) == kResultOk,
         "initialize the bundled gesture test");
  auto *handler = new TestComponentHandler();
  expect(processor->setComponentHandler(handler) == kResultOk,
         "install the bundled gesture handler");
  auto processSetup = setup(48000.0, 64);
  expect(processor->setupProcessing(processSetup) == kResultOk,
         "prepare the bundled gesture test");
  const auto installed = choc::json::parse(processor->handleUiMessage(
      R"({"type":"pipeline/rebuild","payload":{"pipeline":"A","plugins":[)"
      R"({"id":23,"type":"DCOffsetPlugin","name":"DC Offset","enabled":true,)"
      R"("parameters":{"of":0},"wasmParams":[0],)"
      R"("wasmParamsHash":1104945464}]}})"));
  expect(installed["ok"].getWithDefault<bool>(false),
         "install the bundled gesture pipeline");
  const auto parameterId = boundAutomationParameterId(
      *processor, {'A', 23, "DCOffsetPlugin", "of", 0});
  handler->performedEditCount = 0;

  const std::array<double, 4> positions{0.5625, 0.625, 0.6875, 0.75};
  std::string bundled;
  for (const auto normalized : positions) {
    bundled += bundled.empty() ? "" : ",";
    bundled += std::string{R"({"pipeline":"A","pluginId":23,)"
                           R"("pluginType":"DCOffsetPlugin","parameterKey":"of",)"
                           R"("elementIndex":0,"normalized":)"} +
               std::to_string(normalized) + "}";
  }
  const auto updated = choc::json::parse(processor->handleUiMessage(
      std::string{R"({"type":"pipeline/updatePlugin","payload":{"pipeline":"A",)"
                  R"("plugin":{"id":23,"type":"DCOffsetPlugin","name":"DC Offset",)"
                  R"("enabled":true,"parameters":{"of":0.5},"wasmParams":[0.5],)"
                  R"("wasmParamsHash":1104945464},"automationEdits":[)"} +
      bundled + "]}}"));
  expect(updated["ok"].getWithDefault<bool>(false),
         "accept the coalesced image and the gestures it carries");
  expect(!updated["automationEditsAccepted"].isArray(),
         "the answer says nothing about the individual gestures: each one "
         "becomes the plug-in's own value however the host answers");
  // Each of these is a complete gesture of its own -- it opens and closes in the
  // same transaction -- so each one states the value the parameter held before
  // it as well as the value the user made. The two land on one and the same
  // host position, where a lane keeps the later of them, so what the recorded
  // take holds is still exactly the run of user values, in order.
  expect(handler->performedEditCount == positions.size() * 2u,
         "no gesture of the frame is thinned out on the way to the host -- each "
         "reports the value it opened over and then the value the user made, so "
         "the host was handed " +
             std::to_string(handler->performedEditCount) + " values, not " +
             std::to_string(positions.size() * 2u));
  for (std::uint32_t index = 0; index < positions.size(); ++index) {
    const auto opened = handler->performedEdits[index * 2u];
    const auto made = handler->performedEdits[index * 2u + 1u];
    const auto before = index == 0 ? 0.5 : positions[index - 1u];
    expect(opened.id == parameterId && std::abs(opened.value - before) < 1.0e-9,
           "each bundled gesture opens on the value the parameter already held");
    expect(made.id == parameterId &&
               std::abs(made.value - positions[index]) < 1.0e-9,
           "the bundled gestures reach the host in the order they were collected");
    expect(opened.lanePosition == made.lanePosition,
           "and both land on the one position the host is rendering, so the "
           "value the user made is the one the lane keeps");
  }
  expect(std::abs(processor->getParamNormalized(parameterId) - positions.back()) < 1.0e-9,
         "the value the frame ended on is the one the host and the plug-in keep");
  expect(processor->terminate() == kResultOk, "terminate the bundled gesture test");
  handler->release();
}

// A plug-in update is a user gesture, so a target it names that holds no lane
// yet claims one -- unlike the bulk routes, which adopt bound targets only. The
// host Write gate stays the authority on whether a lane may be claimed at all,
// and a refused allocation is still not a failure: the value already reached the
// DSP through the image that carried it.
void testBundledGestureBindsOnDemandUnderTheWriteGate() {
  const effetune::vst::AutomationTargetIdentity identity{
      'A', 23, "DCOffsetPlugin", "of", 0};
  constexpr const char *updateMessage =
      R"({"type":"pipeline/updatePlugin","payload":{"pipeline":"A",)"
      R"("plugin":{"id":23,"type":"DCOffsetPlugin","name":"DC Offset",)"
      R"("enabled":true,"parameters":{"of":0.5},"wasmParams":[0.5],)"
      R"("wasmParamsHash":1104945464},"automationEdits":[{"pipeline":"A",)"
      R"("pluginId":23,"pluginType":"DCOffsetPlugin","parameterKey":"of",)"
      R"("elementIndex":0,"normalized":0.75}]}})";

  struct Outcome {
    std::unique_ptr<EffeTuneProcessor> processor;
    TestComponentHandler *handler = nullptr;
    bool bound = false;
  };
  const auto update = [&](const int32 automationState, const bool notify) {
    Outcome outcome;
    outcome.processor = std::make_unique<EffeTuneProcessor>();
    expect(outcome.processor->initialize(nullptr) == kResultOk,
           "initialize the bundled write-gate test");
    outcome.handler = new TestComponentHandler();
    expect(outcome.processor->setComponentHandler(outcome.handler) == kResultOk,
           "install the bundled write-gate handler");
    auto processSetup = setup(48000.0, 64);
    expect(outcome.processor->setupProcessing(processSetup) == kResultOk,
           "prepare the bundled write-gate test");
    const auto installed = choc::json::parse(outcome.processor->handleUiMessage(
        R"({"type":"pipeline/rebuild","payload":{"pipeline":"A","plugins":[)"
        R"({"id":23,"type":"DCOffsetPlugin","name":"DC Offset","enabled":true,)"
        R"("parameters":{"of":0},"wasmParams":[0],)"
        R"("wasmParamsHash":1104945464}]}})"));
    expect(installed["ok"].getWithDefault<bool>(false),
           "install the bundled write-gate pipeline");
    expect(!PluginProcessorTestAccess::activeAutomationSlot(*outcome.processor, identity)
                .has_value(),
           "the bundled gesture target starts without an automation slot");
    if (notify) {
      expect(outcome.processor->setAutomationState(automationState) == kResultOk,
             "accept the host automation state");
    }
    const auto updated =
        choc::json::parse(outcome.processor->handleUiMessage(updateMessage));
    expect(updated["ok"].getWithDefault<bool>(false),
           "a bundled gesture never fails the image that already carried it");
    outcome.bound =
        PluginProcessorTestAccess::activeAutomationSlot(*outcome.processor, identity)
            .has_value();
    return outcome;
  };

  {
    auto outcome = update(0, false);
    expect(outcome.bound,
           "a gesture bundled with a plug-in image claims a lane on demand");
    // The gesture states the value the lane already held when it opened and
    // then the value the user made. A lane claimed on demand is born holding
    // the value of the image that carried it, so here the two coincide.
    expect(outcome.handler->performedEditCount == 2u &&
               std::abs(outcome.handler->performedEdits[0].value - 0.75) < 1.0e-9 &&
               std::abs(outcome.handler->performedEdits[1].value - 0.75) < 1.0e-9,
           "the newly bound lane reports the gesture value to the host");
    expect(outcome.processor->terminate() == kResultOk,
           "terminate the bundled on-demand binding case");
    outcome.handler->release();
  }
  {
    auto outcome = update(IAutomationState::kNoAutomation, true);
    expect(!outcome.bound,
           "a host outside write state refuses the bundled allocation");
    expect(outcome.handler->performedEditCount == 0u,
           "no host parameter is touched when no lane was claimed");
    const auto info = hostInfo(*outcome.processor);
    auto warnings = 0;
    for (const auto diagnostic : info["diagnostics"]) {
      if (diagnostic["code"].getWithDefault<std::string>({}) ==
          "automation-write-required") {
        ++warnings;
      }
    }
    expect(warnings == 1, "the refused allocation raises one visible diagnostic");
    expect(outcome.processor->terminate() == kResultOk,
           "terminate the refused bundled write-gate case");
    outcome.handler->release();
  }
}

// An edit that arrives with bindIfUnbound cleared -- the shape the bulk overlay
// paths and a gesture close use -- may reconfigure a lane that already exists,
// but claiming one would spend a finite slot permanently and record an
// automation point the user never made in a Write-enabled lane.
void testWithheldBindingNeverClaimsAnAutomationLane() {
  const effetune::vst::AutomationTargetIdentity identity{
      'A', 23, "DCOffsetPlugin", "of", 0};
  const auto updateMessage = [](const char *edits) {
    return std::string{R"({"type":"pipeline/updatePlugin","payload":{"pipeline":"A",)"
                       R"("plugin":{"id":23,"type":"DCOffsetPlugin","name":"DC Offset",)"
                       R"("enabled":true,"parameters":{"of":0.5},"wasmParams":[0.5],)"
                       R"("wasmParamsHash":1104945464},"automationEdits":[)"} +
           edits + "]}}";
  };
  constexpr const char *withheldBinding =
      R"({"pipeline":"A","pluginId":23,"pluginType":"DCOffsetPlugin",)"
      R"("parameterKey":"of","elementIndex":0,"normalized":0.75,)"
      R"("bindIfUnbound":false})";
  // An older UI omits the field entirely, which has always meant a gesture.
  constexpr const char *gesture =
      R"({"pipeline":"A","pluginId":23,"pluginType":"DCOffsetPlugin",)"
      R"("parameterKey":"of","elementIndex":0,"normalized":0.625})";

  auto processor = std::make_unique<EffeTuneProcessor>();
  expect(processor->initialize(nullptr) == kResultOk,
         "initialize the withheld-binding test");
  auto *handler = new TestComponentHandler();
  expect(processor->setComponentHandler(handler) == kResultOk,
         "install the withheld-binding handler");
  auto processSetup = setup(48000.0, 64);
  expect(processor->setupProcessing(processSetup) == kResultOk,
         "prepare the withheld-binding test");
  const auto installed = choc::json::parse(processor->handleUiMessage(
      R"({"type":"pipeline/rebuild","payload":{"pipeline":"A","plugins":[)"
      R"({"id":23,"type":"DCOffsetPlugin","name":"DC Offset","enabled":true,)"
      R"("parameters":{"of":0},"wasmParams":[0],)"
      R"("wasmParamsHash":1104945464}]}})"));
  expect(installed["ok"].getWithDefault<bool>(false),
         "install the withheld-binding pipeline");
  expect(!PluginProcessorTestAccess::activeAutomationSlot(*processor, identity)
              .has_value(),
         "the withheld-binding target starts without an automation slot");

  const auto corrected =
      choc::json::parse(processor->handleUiMessage(updateMessage(withheldBinding)));
  expect(corrected["ok"].getWithDefault<bool>(false),
         "an edit that may claim nothing never fails the image it travelled with");
  expect(!PluginProcessorTestAccess::activeAutomationSlot(*processor, identity)
              .has_value(),
         "and it consumes no automation slot of its own");
  expect(handler->performedEditCount == 0u,
         "no automation point the user never performed is written to the host");
  const auto info = hostInfo(*processor);
  for (const auto diagnostic : info["diagnostics"]) {
    expect(diagnostic["code"].getWithDefault<std::string>({}) !=
               "automation-write-required",
           "an edit that claims nothing owes the user no write-gate warning");
  }

  const auto moved =
      choc::json::parse(processor->handleUiMessage(updateMessage(gesture)));
  expect(moved["ok"].getWithDefault<bool>(false),
         "the gesture that follows is accepted");
  const auto slot =
      PluginProcessorTestAccess::activeAutomationSlot(*processor, identity);
  expect(slot.has_value(),
         "an edit that omits the field is still a gesture and claims its lane");
  // The gesture opens by restating the value the lane holds -- a lane claimed
  // on demand is born on the very value the gesture carries -- and then reports
  // the user's value, so one gesture is two reports and no more.
  // The gesture opens by restating the value the lane already held -- the one
  // the image it travelled with carries -- and then reports the user's value,
  // so one gesture is two reports and no more.
  expect(handler->performedEditCount == 2u &&
             std::abs(handler->performedEdits[0].value - 0.75) < 1.0e-9 &&
             std::abs(handler->performedEdits[1].value - 0.625) < 1.0e-9,
         "and only that gesture reaches the host");

  const auto reconfigured =
      choc::json::parse(processor->handleUiMessage(updateMessage(withheldBinding)));
  expect(reconfigured["ok"].getWithDefault<bool>(false),
         "the same edit is taken once the target owns a lane");
  expect(handler->performedEditCount == 4u &&
             std::abs(handler->performedEdits[2].value - 0.625) < 1.0e-9 &&
             std::abs(handler->performedEdits[3].value - 0.75) < 1.0e-9,
         "a bound lane is what such an edit exists to reconfigure");
  expect(std::abs(processor->getParamNormalized(
                      effetune::vst::plugin::automationParameterId(*slot)) - 0.75) < 1.0e-9,
         "and the scheduler stops pinning the value it replaced");
  expect(processor->terminate() == kResultOk,
         "terminate the withheld-binding test");
  handler->release();
}

void testNodeEnableAutomationDrivesTopologyNotPackedParameters() {
  auto processor = std::make_unique<EffeTuneProcessor>();
  expect(processor->initialize(nullptr) == kResultOk,
         "initialize the node-enable automation test");
  ParameterInfo unboundInfo{};
  expect(processor->getParameterInfo(1, unboundInfo) == kResultOk &&
             unboundInfo.id == kFirstAutomationParameterId &&
             unboundInfo.stepCount == 0,
         "the trace pass-through exposes the unchanged unbound metadata");
  ParameterInfo invalidInfo{};
  invalidInfo.id = 1234;
  invalidInfo.stepCount = 7;
  expect(processor->getParameterInfo(-1, invalidInfo) == kResultFalse &&
             invalidInfo.id == 1234 && invalidInfo.stepCount == 7,
         "a failed traced query preserves the SDK output contract");
  auto processSetup = setup(48000.0, 64);
  expect(processor->setupProcessing(processSetup) == kResultOk,
         "prepare the node-enable automation test");
  const auto installed = choc::json::parse(processor->handleUiMessage(
      R"({"type":"pipeline/rebuild","payload":{"pipeline":"A","plugins":[)"
      R"({"id":77,"type":"DCOffsetPlugin","name":"DC Offset","enabled":true,)"
      R"("parameters":{"of":0.5},"wasmParams":[0.5],)"
      R"("wasmParamsHash":1104945464}]}})"));
  expect(installed["ok"].getWithDefault<bool>(false),
         "install the node-enable automation pipeline");
  const auto enableParameterId = boundAutomationParameterId(
      *processor, {'A', 77, "DCOffsetPlugin", "__enabled", 0});
  ParameterInfo enableInfo{};
  expect(processor->getParameterInfo(
             static_cast<int32>(enableParameterId - kFirstAutomationParameterId + 1u),
             enableInfo) == kResultOk &&
             enableInfo.id == enableParameterId && enableInfo.stepCount == 1 &&
             (enableInfo.flags & ParameterInfo::kCanAutomate) != 0 &&
             enableInfo.defaultNormalizedValue == 1.0,
         "the traced controller exports a bound enable target as a two-state parameter");
  expect(processor->getParamNormalized(enableParameterId) == 1.0,
         "the bound enable slot starts at the current enabled state");
  expect(processor->setActive(true) == kResultOk,
         "activate the node-enable automation test");

  ParameterChanges changes(1);
  int32 queueIndex = 0;
  auto *queue = changes.addParameterData(enableParameterId, queueIndex);
  int32 pointIndex = 0;
  expect(queue != nullptr && queue->addPoint(0, 0.0, pointIndex) == kResultTrue,
         "create the node-enable automation point");
  std::array<float, 64> inputLeft{};
  std::array<float, 64> inputRight{};
  std::array<float, 64> outputLeft{};
  std::array<float, 64> outputRight{};
  Sample32 *inputChannels[]{inputLeft.data(), inputRight.data()};
  Sample32 *outputChannels[]{outputLeft.data(), outputRight.data()};
  AudioBusBuffers input{};
  input.numChannels = 2;
  input.channelBuffers32 = inputChannels;
  AudioBusBuffers output{};
  output.numChannels = 2;
  output.channelBuffers32 = outputChannels;
  ProcessData data{};
  data.symbolicSampleSize = kSample32;
  data.numSamples = 64;
  data.numInputs = 1;
  data.numOutputs = 1;
  data.inputs = &input;
  data.outputs = &output;
  data.inputParameterChanges = &changes;
  expect(processor->process(data) == kResultOk,
         "process the node-enable automation block");
  for (const auto sample : outputLeft) {
    expect(std::abs(sample - 0.5f) < 1.0e-6f,
           "the audio slice loop skips the node-enable slot instead of writing "
           "its packed parameter");
  }

  const auto descriptorGenerationBefore =
      PluginProcessorTestAccess::descriptorGeneration(*processor);
  const auto state = choc::json::parse(processor->handleUiMessage(
      R"({"type":"storage/readFile","payload":{"path":"pipeline-state.json"}})"));
  const auto persisted =
      choc::json::parse(state["content"].getWithDefault<std::string>({}));
  expect(!persisted["pipelineA"][0]["enabled"].getWithDefault<bool>(true) &&
             std::abs(persisted["pipelineA"][0]["parameters"]["of"]
                          .getWithDefault<double>(0.0) -
                      0.5) < 1.0e-6,
         "the control-thread drain moves enabled and leaves the packed value alone");
  const auto descriptorGeneration =
      PluginProcessorTestAccess::descriptorGeneration(*processor);
  expect(descriptorGeneration > descriptorGenerationBefore,
         "the node-enable change queues a descriptor update");

  expect(pumpMainThreadUntil(
             [&] {
               return PluginProcessorTestAccess::servicedDescriptorGeneration(
                          *processor) == descriptorGeneration;
             },
             std::chrono::milliseconds(500)),
         "the native control service adopts the node-enable descriptor");
  data.inputParameterChanges = nullptr;
  outputLeft.fill(1.0f);
  expect(processor->process(data) == kResultOk,
         "process after the node-enable descriptor is adopted");
  for (const auto sample : outputLeft) {
    expect(std::abs(sample) < 1.0e-6f,
           "the disabled node leaves the graph and stops offsetting the signal");
  }

  ParameterChanges enableChanges(1);
  queueIndex = 0;
  auto *enableQueue =
      enableChanges.addParameterData(enableParameterId, queueIndex);
  pointIndex = 0;
  expect(enableQueue != nullptr &&
             enableQueue->addPoint(0, 1.0, pointIndex) == kResultTrue,
         "create the node re-enable automation point");
  data.inputParameterChanges = &enableChanges;
  outputLeft.fill(1.0f);
  expect(processor->process(data) == kResultOk,
         "process the node re-enable automation block");
  for (const auto sample : outputLeft) {
    expect(std::abs(sample) < 1.0e-6f,
           "the old disabled graph renders until the re-enable value drains");
  }

  const auto enableDescriptorGenerationBefore =
      PluginProcessorTestAccess::descriptorGeneration(*processor);
  const auto enabledState = choc::json::parse(processor->handleUiMessage(
      R"({"type":"storage/readFile","payload":{"path":"pipeline-state.json"}})"));
  const auto enabledPersisted =
      choc::json::parse(enabledState["content"].getWithDefault<std::string>({}));
  expect(enabledPersisted["pipelineA"][0]["enabled"].getWithDefault<bool>(false),
         "the control-thread drain restores the node-enabled state");
  const auto enableDescriptorGeneration =
      PluginProcessorTestAccess::descriptorGeneration(*processor);
  expect(enableDescriptorGeneration > enableDescriptorGenerationBefore,
         "the node re-enable change queues a descriptor update");

  expect(pumpMainThreadUntil(
             [&] {
               return PluginProcessorTestAccess::servicedDescriptorGeneration(
                          *processor) == enableDescriptorGeneration;
             },
             std::chrono::milliseconds(500)),
         "the native control service adopts the node re-enable descriptor");
  data.inputParameterChanges = nullptr;
  outputLeft.fill(0.0f);
  expect(processor->process(data) == kResultOk,
         "process after the node re-enable descriptor is adopted");
  for (const auto sample : outputLeft) {
    expect(std::abs(sample - 0.5f) < 1.0e-6f,
           "the re-enabled node returns to the graph and offsets the signal");
  }
  expect(processor->setActive(false) == kResultOk,
         "deactivate the node-enable automation test");
  expect(processor->terminate() == kResultOk,
         "terminate the node-enable automation test");
}

void testBoundNodeEnableEditsDoNotInvalidateHostParameters() {
  auto processor = std::make_unique<EffeTuneProcessor>();
  expect(processor->initialize(nullptr) == kResultOk,
         "initialize the node-enable notification test");
  auto *handler = new TestComponentHandler();
  expect(processor->setComponentHandler(handler) == kResultOk,
         "install the node-enable notification handler");
  auto processSetup = setup(48000.0, 64);
  expect(processor->setupProcessing(processSetup) == kResultOk,
         "prepare the node-enable notification test");
  const auto installed = choc::json::parse(processor->handleUiMessage(
      R"({"type":"pipeline/rebuild","payload":{"pipeline":"A","plugins":[)"
      R"({"id":77,"type":"DCOffsetPlugin","name":"DC Offset","enabled":true,)"
      R"("parameters":{"of":0.5},"wasmParams":[0.5],)"
      R"("wasmParamsHash":1104945464}]}})"));
  expect(installed["ok"].getWithDefault<bool>(false),
         "install the node-enable notification pipeline");
  const auto enableId = boundAutomationParameterId(
      *processor, {'A', 77, "DCOffsetPlugin", "__enabled", 0});
  const auto offsetId = boundAutomationParameterId(
      *processor, {'A', 77, "DCOffsetPlugin", "of", 0});
  expect((handler->restartFlags & RestartFlags::kParamTitlesChanged) != 0,
         "new bindings still invalidate the placeholder parameter definitions");
  expect(processor->setAutomationState(IAutomationState::kReadState |
                                      IAutomationState::kWriteState) == kResultOk,
         "enable Read and Write for the node-enable notification test");
  expect(processor->setActive(true) == kResultOk,
         "activate the node-enable notification test");

  ParameterInfo before{};
  expect(processor->getParameterInfo(1, before) == kResultOk,
         "read the bound enable definition before the clicks");
  std::array<float, 64> inputLeft{}, inputRight{}, outputLeft{}, outputRight{};
  Sample32 *inputs[]{inputLeft.data(), inputRight.data()};
  Sample32 *outputs[]{outputLeft.data(), outputRight.data()};
  AudioBusBuffers input{}, output{};
  input.numChannels = output.numChannels = 2;
  input.channelBuffers32 = inputs;
  output.channelBuffers32 = outputs;
  ProcessContext context{};
  context.sampleRate = 48000.0;
  context.state = ProcessContext::kPlaying;
  ProcessData data{};
  data.symbolicSampleSize = kSample32;
  data.numSamples = 64;
  data.numInputs = data.numOutputs = 1;
  data.inputs = &input;
  data.outputs = &output;
  data.processContext = &context;

  const auto update = [&](const bool enabled, const double offset,
                          const char *key, const double normalized,
                          const char *name = "DC Offset",
                          const bool splitGesture = false) {
    handler->clearEditLog();
    handler->restartFlags = 0;
    const auto gestureTarget = std::string{
        R"({"pipeline":"A","pluginId":77,"pluginType":"DCOffsetPlugin",)"
        R"("parameterKey":")"} + key + R"(","elementIndex":0})";
    if (splitGesture) {
      const auto opened = choc::json::parse(processor->handleUiMessage(
          std::string{R"({"type":"automation/beginGesture","payload":{"targets":[)"} +
          gestureTarget + "]}}"));
      expect(opened["ok"].getWithDefault<bool>(false),
             "open the pointer gesture before its image update");
    }
    const auto message = std::string{
        R"({"type":"pipeline/updatePlugin","payload":{"pipeline":"A","plugin":{)"
        R"("id":77,"type":"DCOffsetPlugin","name":")"} + name +
        R"(","enabled":)" + (enabled ? "true" : "false") +
        R"(,"parameters":{"of":)" + std::to_string(offset) +
        R"(},"wasmParams":[)" + std::to_string(offset) +
        R"(],"wasmParamsHash":1104945464},"automationEdits":[{"pipeline":"A",)"
        R"("pluginId":77,"pluginType":"DCOffsetPlugin","parameterKey":")" + key +
        R"(","elementIndex":0,"normalized":)" + std::to_string(normalized) +
        R"(,"endGesture":)" + (splitGesture ? "false" : "true") + "}]}}";
    const auto result = choc::json::parse(processor->handleUiMessage(message));
    expect(result["ok"].getWithDefault<bool>(false),
         "accept the same bundled image and edit that a UI click sends");
    if (splitGesture) {
      expect(handler->stepCount(TestComponentHandler::EditStep::begin) == 1u &&
                 handler->stepCount(TestComponentHandler::EditStep::end) == 0u,
             "the image update neither reopens nor closes the pointer gesture");
      const auto closed = choc::json::parse(processor->handleUiMessage(
          std::string{R"({"type":"automation/endGesture","payload":{"targets":[)"} +
          gestureTarget + "]}}"));
      expect(closed["ok"].getWithDefault<bool>(false),
             "close the pointer gesture after its image update");
    }
    expect(handler->stepCount(TestComponentHandler::EditStep::begin) == 1u &&
               handler->stepCount(TestComponentHandler::EditStep::end) == 1u &&
               handler->performedEditCount != 0,
           "the click retains one balanced host gesture");
    const auto last = handler->performedEdits[handler->performedEditCount - 1u];
    expect(last.value == normalized &&
               processor->getParamNormalized(last.id) == normalized,
           "the host receives and can read back the user's absolute value");
  };

  for (const auto [enabled, splitGesture] :
       {std::pair{false, false}, std::pair{true, false},
        std::pair{false, true}, std::pair{true, true}}) {
    update(enabled, 0.5, "__enabled", enabled ? 1.0 : 0.0,
           "DC Offset", splitGesture);
    ParameterInfo after{};
    expect(processor->getParameterInfo(1, after) == kResultOk &&
               after.id == before.id && after.stepCount == before.stepCount &&
               after.flags == before.flags && after.unitId == before.unitId &&
               after.defaultNormalizedValue == before.defaultNormalizedValue &&
               std::equal(std::begin(before.title), std::end(before.title), after.title) &&
               std::equal(std::begin(before.shortTitle), std::end(before.shortTitle),
                          after.shortTitle) &&
               std::equal(std::begin(before.units), std::end(before.units), after.units),
           "a bound enable click changes no parameter metadata");
    expect(handler->restartFlags == 0,
           "a value-only enable click must not invalidate the host parameter bank");
    expect(handler->performedEdits[handler->performedEditCount - 1u].id == enableId,
           "the enable gesture stays on the original slot");
    const auto descriptor = PluginProcessorTestAccess::descriptorGeneration(*processor);
    expect(pumpMainThreadUntil([&] {
             expect(processor->process(data) == kResultOk,
                    "continue playback during the descriptor update");
             context.projectTimeSamples += data.numSamples;
             PluginProcessorTestAccess::serviceLatencyUpdates(*processor);
             return PluginProcessorTestAccess::servicedDescriptorGeneration(*processor) ==
                    descriptor;
           }, std::chrono::milliseconds(500)),
           "the click still reaches the native topology without a host echo");
    expect(processor->process(data) == kResultOk,
           "render the adopted enable state");
    context.projectTimeSamples += data.numSamples;
    for (const auto sample : outputLeft) {
      expect(std::abs(sample - (enabled ? 0.5f : 0.0f)) < 1.0e-6f,
             "the native output follows each OFF and ON click");
    }
    expect(handler->restartFlags == 0,
           "deferred descriptor service also leaves unchanged metadata alone");
  }

  update(true, 0.25, "of", 0.625);
  expect(handler->restartFlags == 0 &&
             handler->performedEdits[handler->performedEditCount - 1u].id == offsetId,
         "a packed parameter gesture also avoids a global parameter invalidation");
  update(true, 0.25, "of", 0.625, "Renamed DC Offset");
  expect((handler->restartFlags & RestartFlags::kParamTitlesChanged) != 0,
         "a real name change still invalidates the host parameter definitions");
  expect(processor->setActive(false) == kResultOk,
         "deactivate the node-enable notification test");
  expect(processor->terminate() == kResultOk,
         "terminate the node-enable notification test");
  handler->release();
}

// The node-enable drain releases processingResourcesMutex_ while it rewrites
// JSON. A setState() that lands in that gap publishes a replacement document
// which must never be encoded into a descriptor for the retained old runtime.
void testStateRestoreCancelsAnInFlightNodeEnableDescriptor() {
  auto processor = std::make_unique<EffeTuneProcessor>();
  expect(processor->initialize(nullptr) == kResultOk,
         "initialize the node-enable restore race test");
  auto processSetup = setup(48000.0, 64);
  expect(processor->setupProcessing(processSetup) == kResultOk,
         "prepare the node-enable restore race test");
  const auto installed = choc::json::parse(processor->handleUiMessage(
      R"({"type":"pipeline/rebuild","payload":{"pipeline":"A","plugins":[)"
      R"({"id":602,"type":"DCOffsetPlugin","name":"DC Offset","enabled":true,)"
      R"("parameters":{"of":0.5},"wasmParams":[0.5],)"
      R"("wasmParamsHash":1104945464}]}})"));
  expect(installed["ok"].getWithDefault<bool>(false),
         "install the node-enable restore race pipeline");
  const auto enableParameterId = boundAutomationParameterId(
      *processor, {'A', 602, "DCOffsetPlugin", "__enabled", 0});
  expect(processor->setActive(true) == kResultOk,
         "activate the node-enable restore race test");

  ResizableMemoryIBStream restoredState;
  expect(processor->getState(&restoredState) == kResultOk,
         "save the enabled replacement document");
  const auto descriptorBefore =
      PluginProcessorTestAccess::descriptorGeneration(*processor);
  PluginProcessorTestAccess::pauseAutomationDrainBeforeDescriptor(*processor,
                                                                   true);

  ParameterChanges changes(1);
  int32 queueIndex = 0;
  auto *queue = changes.addParameterData(enableParameterId, queueIndex);
  int32 pointIndex = 0;
  expect(queue != nullptr && queue->addPoint(0, 0.0, pointIndex) == kResultTrue,
         "publish the disabling automation value");
  std::array<float, 64> inputLeft{};
  std::array<float, 64> inputRight{};
  std::array<float, 64> outputLeft{};
  std::array<float, 64> outputRight{};
  Sample32 *inputChannels[]{inputLeft.data(), inputRight.data()};
  Sample32 *outputChannels[]{outputLeft.data(), outputRight.data()};
  AudioBusBuffers input{};
  input.numChannels = 2;
  input.channelBuffers32 = inputChannels;
  AudioBusBuffers output{};
  output.numChannels = 2;
  output.channelBuffers32 = outputChannels;
  ProcessData data{};
  data.symbolicSampleSize = kSample32;
  data.numSamples = 64;
  data.numInputs = 1;
  data.numOutputs = 1;
  data.inputs = &input;
  data.outputs = &output;
  data.inputParameterChanges = &changes;
  expect(processor->process(data) == kResultOk,
         "render the disabling automation block");

  std::thread drainer(
      [&] { PluginProcessorTestAccess::drainAutomationValues(*processor); });
  const auto paused = pumpMainThreadUntil(
      [&] {
        return PluginProcessorTestAccess::automationDrainPausedBeforeDescriptor(
            *processor);
      },
      std::chrono::milliseconds(500));
  tresult restored = kResultFalse;
  if (paused) {
    restoredState.rewind();
    restored = processor->setState(&restoredState);
  }
  PluginProcessorTestAccess::pauseAutomationDrainBeforeDescriptor(*processor,
                                                                   false);
  drainer.join();

  expect(paused && restored == kResultOk,
         "publish the replacement while the old drain awaits its descriptor");
  expect(PluginProcessorTestAccess::descriptorGeneration(*processor) ==
             descriptorBefore,
         "the old node-enable value queues no descriptor for the replacement state");
  expect(processor->terminate() == kResultOk,
         "terminate the node-enable restore race test");
}

// Hosts such as Cakewalk Sonar record a performEdit() without ever echoing it
// back through inputParameterChanges. A bound target must therefore reach the
// DSP from the gesture alone: performEdit() notifies the host, and the same
// automation/edit adopts the value locally.
void testBoundTargetGestureReachesAudioWithoutHostEcho() {
  auto processor = std::make_unique<EffeTuneProcessor>();
  expect(processor->initialize(nullptr) == kResultOk,
         "initialize the unechoed-gesture test");
  auto *handler = new TestComponentHandler();
  expect(processor->setComponentHandler(handler) == kResultOk,
         "install the unechoed-gesture handler");
  auto processSetup = setup(48000.0, 64);
  expect(processor->setupProcessing(processSetup) == kResultOk,
         "prepare the unechoed-gesture test");
  const auto installed = choc::json::parse(processor->handleUiMessage(
      R"({"type":"pipeline/rebuild","payload":{"pipeline":"A","plugins":[)"
      R"({"id":23,"type":"DCOffsetPlugin","name":"DC Offset","enabled":true,)"
      R"("parameters":{"of":0},"wasmParams":[0],)"
      R"("wasmParamsHash":1104945464}]}})"));
  expect(installed["ok"].getWithDefault<bool>(false),
         "install the unechoed-gesture pipeline");
  const effetune::vst::AutomationTargetIdentity identity{'A', 23, "DCOffsetPlugin",
                                                          "of", 0};
  (void)boundAutomationParameterId(*processor, identity);

  const auto sendUpdate = [&](const double offset) {
    const auto updated = choc::json::parse(processor->handleUiMessage(
        std::string{R"({"type":"pipeline/updatePlugin","payload":{"pipeline":"A",)"
                    R"("plugin":{"id":23,"type":"DCOffsetPlugin","name":"DC Offset",)"
                    R"("enabled":true,"parameters":{"of":)"} +
        std::to_string(offset) + R"(},"wasmParams":[)" + std::to_string(offset) +
        R"(],"wasmParamsHash":1104945464}}})"));
    expect(updated["ok"].getWithDefault<bool>(false),
           "send the slider value through pipeline/updatePlugin");
  };
  const auto sendEdit = [&](const double normalized) {
    const auto edited = choc::json::parse(processor->handleUiMessage(
        std::string{R"({"type":"automation/edit","payload":{"pipeline":"A",)"
                    R"("pluginId":23,"pluginType":"DCOffsetPlugin",)"
                    R"("parameterKey":"of","elementIndex":0,"normalized":)"} +
        std::to_string(normalized) + "}}"));
    expect(edited["ok"].getWithDefault<bool>(false) &&
               edited["bound"].getWithDefault<bool>(false),
           "the gesture keeps driving the already bound automation slot");
  };
  const auto persistedOffset = [&] {
    const auto state = choc::json::parse(processor->handleUiMessage(
        R"({"type":"storage/readFile","payload":{"path":"pipeline-state.json"}})"));
    return choc::json::parse(state["content"].getWithDefault<std::string>({}))
        ["pipelineA"][0]["parameters"]["of"]
            .getWithDefault<double>(0.0);
  };

  // With the transport stopped no block ever consumes the scheduler
  // configuration, so the single-plug-in update is the only thing that can carry
  // the gesture into the state authority.
  sendUpdate(0.125);
  sendEdit(0.5625);
  expect(std::abs(persistedOffset() - 0.125) < 1.0e-6,
         "a stopped-transport gesture keeps the UI value in the state document");

  expect(processor->setActive(true) == kResultOk,
         "activate the unechoed-gesture test");

  std::array<float, 64> inputLeft{};
  std::array<float, 64> inputRight{};
  std::array<float, 64> outputLeft{};
  std::array<float, 64> outputRight{};
  Sample32 *inputChannels[]{inputLeft.data(), inputRight.data()};
  Sample32 *outputChannels[]{outputLeft.data(), outputRight.data()};
  AudioBusBuffers input{};
  input.numChannels = 2;
  input.channelBuffers32 = inputChannels;
  AudioBusBuffers output{};
  output.numChannels = 2;
  output.channelBuffers32 = outputChannels;
  ProcessData data{};
  data.symbolicSampleSize = kSample32;
  data.numSamples = 64;
  data.numInputs = 1;
  data.numOutputs = 1;
  data.inputs = &input;
  data.outputs = &output;
  // The host records the edit but never plays it back, so the DSP has to follow
  // the gesture without a single input parameter change.
  data.inputParameterChanges = nullptr;

  const auto gesture = [&](const double offset, const double normalized,
                           const std::string &message) {
    sendUpdate(offset);
    sendEdit(normalized);
    outputLeft.fill(0.0f);
    outputRight.fill(0.0f);
    expect(processor->process(data) == kResultOk,
           "process the unechoed gesture block");
    for (const auto sample : outputLeft) {
      expect(std::abs(sample - static_cast<float>(offset)) < 1.0e-6f, message);
    }
  };

  gesture(0.5, 0.75,
          "a bound target follows the first UI gesture without a host echo");
  gesture(-0.25, 0.375,
          "a bound target keeps following repeated UI gestures without a host echo");

  const auto snapshot = hostInfo(*processor);
  const auto delta = findAutomationDelta(snapshot, identity);
  expect(!delta.isVoid() &&
             std::abs(delta["normalized"].getWithDefault<double>(0.0) - 0.375) <
                 1.0e-9,
         "the self-adopted gesture republishes through the existing drain");
  expect(std::abs(persistedOffset() + 0.25) < 1.0e-6,
         "the state document converges on the gesture value");

  expect(processor->setActive(false) == kResultOk,
         "deactivate the unechoed-gesture test");
  expect(processor->terminate() == kResultOk,
         "terminate the unechoed-gesture test");
  handler->release();
}

// A preset load and an undo/redo both arrive as a bulk image. The values they
// carry are the user's explicit intent, so a bound target they name has to
// follow them into the DSP, while a bound target they do not name keeps the
// value automation is playing.
void testNamedBulkAutomationEditsReachAudioAndUnnamedOnesStayOverlaid() {
  auto processor = std::make_unique<EffeTuneProcessor>();
  expect(processor->initialize(nullptr) == kResultOk,
         "initialize the bulk automation edit test");
  auto processSetup = setup(48000.0, 64);
  expect(processor->setupProcessing(processSetup) == kResultOk,
         "prepare the bulk automation edit test");

  // The two offsets sum into the output, so the processed audio reports both
  // targets at once while the runtime images report them individually.
  const auto offsets = [](const double first, const double second) {
    const auto offset = [](const std::uint32_t id, const double value) {
      return std::string{"{\"id\":"} + std::to_string(id) +
             R"(,"type":"DCOffsetPlugin","name":"DC Offset","enabled":true,)"
             R"("parameters":{"of":)" +
             std::to_string(value) + R"(},"wasmParams":[)" + std::to_string(value) +
             R"(],"wasmParamsHash":1104945464})";
    };
    return offset(71, first) + "," + offset(72, second);
  };
  const auto namedEdit = [](const std::uint32_t id, const double normalized) {
    return std::string{R"({"pipeline":"A","pluginId":)"} + std::to_string(id) +
           R"(,"pluginType":"DCOffsetPlugin","parameterKey":"of",)"
           R"("elementIndex":0,"normalized":)" +
           std::to_string(normalized) + "}";
  };

  const auto installed = choc::json::parse(processor->handleUiMessage(
      std::string{R"({"type":"pipeline/rebuild","payload":{"pipeline":"A","plugins":[)"} +
      offsets(0.0, 0.0) + "]}}"));
  expect(installed["ok"].getWithDefault<bool>(false),
         "install the bulk automation edit pipeline");
  const effetune::vst::AutomationTargetIdentity first{'A', 71, "DCOffsetPlugin", "of", 0};
  const effetune::vst::AutomationTargetIdentity second{'A', 72, "DCOffsetPlugin", "of", 0};
  const auto firstParameterId = boundAutomationParameterId(*processor, first);
  const auto secondParameterId = boundAutomationParameterId(*processor, second);
  const auto firstSlot = PluginProcessorTestAccess::activeAutomationSlot(*processor, first);
  const auto secondSlot = PluginProcessorTestAccess::activeAutomationSlot(*processor, second);
  expect(firstSlot.has_value() && secondSlot.has_value(),
         "both bulk automation edit targets hold a slot");
  expect(processor->setActive(true) == kResultOk,
         "activate the bulk automation edit test");

  std::array<float, 64> inputLeft{};
  std::array<float, 64> inputRight{};
  std::array<float, 64> outputLeft{};
  std::array<float, 64> outputRight{};
  Sample32 *inputChannels[]{inputLeft.data(), inputRight.data()};
  Sample32 *outputChannels[]{outputLeft.data(), outputRight.data()};
  AudioBusBuffers input{};
  input.numChannels = 2;
  input.channelBuffers32 = inputChannels;
  AudioBusBuffers output{};
  output.numChannels = 2;
  output.channelBuffers32 = outputChannels;
  ProcessData data{};
  data.symbolicSampleSize = kSample32;
  data.numSamples = 64;
  data.numInputs = 1;
  data.numOutputs = 1;
  data.inputs = &input;
  data.outputs = &output;

  // Both targets are left playing 0.25, so an overlaid target is visible as the
  // played value surviving the bulk image.
  ParameterChanges changes(2);
  int32 queueIndex = 0;
  auto *firstQueue = changes.addParameterData(firstParameterId, queueIndex);
  auto *secondQueue = changes.addParameterData(secondParameterId, queueIndex);
  int32 pointIndex = 0;
  expect(firstQueue != nullptr && secondQueue != nullptr &&
             firstQueue->addPoint(0, 0.625, pointIndex) == kResultTrue &&
             secondQueue->addPoint(0, 0.625, pointIndex) == kResultTrue,
         "play host automation into both bound targets");
  data.inputParameterChanges = &changes;
  expect(processor->process(data) == kResultOk, "consume the played automation");
  data.inputParameterChanges = nullptr;

  const auto processBlock = [&](const float expected, const std::string &message) {
    outputLeft.fill(0.0f);
    outputRight.fill(0.0f);
    expect(processor->process(data) == kResultOk, "process " + message);
    for (const auto sample : outputLeft) {
      expect(std::abs(sample - expected) < 1.0e-6f, message);
    }
  };
  processBlock(0.5f, "the played automation reaches both offsets");

  // A preset load naming only the first target.
  const auto rebuilt = choc::json::parse(processor->handleUiMessage(
      std::string{R"({"type":"pipeline/rebuild","payload":{"pipeline":"A","plugins":[)"} +
      offsets(-0.5, -0.5) + R"(],"automationEdits":[)" + namedEdit(71, 0.25) + "]}}"));
  expect(rebuilt["ok"].getWithDefault<bool>(false),
         "load a preset that names one of the bound targets");
  processBlock(-0.25f, "a named target follows a preset load into the audio "
                       "while an unnamed one keeps the played value");
  expect(std::abs(PluginProcessorTestAccess::playedAutomationValue(*processor, *firstSlot) -
                  0.25) < 1.0e-9 &&
             std::abs(PluginProcessorTestAccess::playedAutomationValue(*processor,
                                                                       *secondSlot) -
                      0.625) < 1.0e-9,
         "the preset load moves only the scheduler value it named");
  expect(std::abs(PluginProcessorTestAccess::runtimePackedParameter(*processor, 71, 0) +
                  0.5f) < 1.0e-6f &&
             std::abs(PluginProcessorTestAccess::runtimePackedParameter(*processor, 72, 0) -
                      0.25f) < 1.0e-6f,
         "the runtime image separates the named target from the overlaid one");
  expect(processor->getParamNormalized(firstParameterId) == 0.25,
         "the host parameter reports the value the preset load named");

  // An undo naming only the second target.
  const auto restored = choc::json::parse(processor->handleUiMessage(
      std::string{R"({"type":"pipeline/restoreHistory","payload":{"pipelineA":[)"} +
      offsets(0.75, 0.75) +
      R"(],"pipelineB":null,"pipelineBInitialized":false,"currentPipeline":"A",)"
      R"("automationEdits":[)" + namedEdit(72, 0.875) + "]}}"));
  expect(restored["ok"].getWithDefault<bool>(false),
         "restore a history entry that names the other bound target");
  processBlock(0.25f, "a named target follows an undo into the audio while an "
                      "unnamed one keeps the value it was left on");
  expect(std::abs(PluginProcessorTestAccess::playedAutomationValue(*processor, *firstSlot) -
                  0.25) < 1.0e-9 &&
             std::abs(PluginProcessorTestAccess::playedAutomationValue(*processor,
                                                                       *secondSlot) -
                      0.875) < 1.0e-9,
         "the history restore moves only the scheduler value it named");

  // A bulk image from a UI that does not send the array at all keeps overlaying
  // every bound target, exactly as it did before the array existed.
  const auto legacy = choc::json::parse(processor->handleUiMessage(
      std::string{R"({"type":"pipeline/rebuild","payload":{"pipeline":"A","plugins":[)"} +
      offsets(0.125, 0.125) + "]}}"));
  expect(legacy["ok"].getWithDefault<bool>(false),
         "accept a bulk image that carries no automation edits");
  processBlock(0.25f, "a bulk image without automation edits leaves every bound "
                      "target on the value it is playing");

  expect(processor->setActive(false) == kResultOk,
         "deactivate the bulk automation edit test");
  expect(processor->terminate() == kResultOk,
         "terminate the bulk automation edit test");
}

void testProcessorPublishesAutomationStateInterface() {
  auto processor = std::make_unique<EffeTuneProcessor>();
  expect(processor->initialize(nullptr) == kResultOk,
         "initialize the automation state interface test");
  IAutomationState *automationState = nullptr;
  expect(processor->queryInterface(IAutomationState::iid,
                                   reinterpret_cast<void **>(&automationState)) ==
                 kResultOk &&
             automationState != nullptr,
         "the processor answers IAutomationState");
  expect(automationState->setAutomationState(IAutomationState::kReadWriteState) ==
             kResultOk,
         "the processor accepts a host automation state");
  automationState->release();
  IAudioProcessor *audioProcessor = nullptr;
  expect(processor->queryInterface(IAudioProcessor::iid,
                                   reinterpret_cast<void **>(&audioProcessor)) ==
                 kResultOk &&
             audioProcessor != nullptr,
         "the added interface does not shadow the audio processor");
  audioProcessor->release();
  expect(processor->terminate() == kResultOk,
         "terminate the automation state interface test");
}

void testReconfigurationPreservesUndrainedAutomationCurrent() {
  auto processor = std::make_unique<EffeTuneProcessor>();
  expect(processor->initialize(nullptr) == kResultOk,
         "initialize automation reconfiguration test");
  auto initialSetup = setup(48000.0, 64);
  expect(processor->setupProcessing(initialSetup) == kResultOk,
         "prepare initial automation sample rate");
  const auto response = choc::json::parse(processor->handleUiMessage(
      R"({"type":"pipeline/rebuild","payload":{"pipeline":"A","plugins":[)"
      R"({"id":19,"type":"DCOffsetPlugin","name":"DC Offset","enabled":true,)"
      R"("parameters":{"of":0},"wasmParams":[0],)"
      R"("wasmParamsHash":1104945464}]}})"));
  expect(response["ok"].getWithDefault<bool>(false),
         "install reconfiguration automation pipeline");
  const auto dcOffsetParameterId = boundAutomationParameterId(
      *processor, {'A', 19, "DCOffsetPlugin", "of", 0});
  expect(processor->setActive(true) == kResultOk,
         "activate initial automation sample rate");

  std::array<float, 64> inputLeft{};
  std::array<float, 64> inputRight{};
  std::array<float, 64> outputLeft{};
  std::array<float, 64> outputRight{};
  Sample32 *inputChannels[]{inputLeft.data(), inputRight.data()};
  Sample32 *outputChannels[]{outputLeft.data(), outputRight.data()};
  AudioBusBuffers input{};
  input.numChannels = 2;
  input.channelBuffers32 = inputChannels;
  AudioBusBuffers output{};
  output.numChannels = 2;
  output.channelBuffers32 = outputChannels;
  ProcessData data{};
  data.symbolicSampleSize = kSample32;
  data.numSamples = 64;
  data.numInputs = 1;
  data.numOutputs = 1;
  data.inputs = &input;
  data.outputs = &output;

  ParameterChanges changes(1);
  int32 queueIndex = 0;
  auto *queue = changes.addParameterData(dcOffsetParameterId, queueIndex);
  int32 pointIndex = 0;
  expect(queue != nullptr && queue->addPoint(0, 0.75, pointIndex) == kResultTrue,
         "publish automation immediately before reconfiguration");
  data.inputParameterChanges = &changes;
  expect(processor->process(data) == kResultOk,
         "process automation immediately before reconfiguration");

  // Do not call getState(), hostInfo(), or another bridge service here. The
  // current value must survive while it is still owned only by the scheduler.
  expect(processor->setActive(false) == kResultOk,
         "deactivate before automation sample-rate update");
  auto updatedSetup = setup(96000.0, 64);
  expect(processor->setupProcessing(updatedSetup) == kResultOk,
         "update automation sample rate without a control-side drain");
  expect(processor->setActive(true) == kResultOk,
         "reactivate after automation sample-rate update");
  outputLeft.fill(0.0f);
  outputRight.fill(0.0f);
  data.inputParameterChanges = nullptr;
  expect(processor->process(data) == kResultOk,
         "process preserved automation after sample-rate update");
  for (const auto sample : outputLeft) {
    expect(std::abs(sample - 0.5f) < 1.0e-6f,
           "sample-rate update preserves the undrained audio-owned value");
  }
  expect(processor->setActive(false) == kResultOk,
         "deactivate automation reconfiguration test");
  expect(processor->terminate() == kResultOk,
         "terminate automation reconfiguration test");
}

// Both pipelines can carry the same logical plug-in ID, and only the active one
// owns the runtime image. A bound target on the idle pipeline must therefore
// never reach that image, or switching pipelines would be enough to hear the
// other side's automation on a plug-in that was never automated.
void testInactivePipelineAutomationLeavesTheActiveRuntimeImageAlone() {
  auto processor = std::make_unique<EffeTuneProcessor>();
  expect(processor->initialize(nullptr) == kResultOk,
         "initialize the inactive-pipeline automation ownership test");
  auto processSetup = setup(48000.0, 64);
  expect(processor->setupProcessing(processSetup) == kResultOk,
         "prepare the inactive-pipeline automation ownership test");
  const auto installedB = choc::json::parse(processor->handleUiMessage(
      R"({"type":"pipeline/rebuild","payload":{"pipeline":"B","plugins":[)"
      R"({"id":57,"type":"DCOffsetPlugin","name":"DC Offset","enabled":true,)"
      R"("parameters":{"of":0},"wasmParams":[0],)"
      R"("wasmParamsHash":1104945464}]}})"));
  expect(installedB["ok"].getWithDefault<bool>(false),
         "install the idle pipeline that shares the logical plug-in ID");
  const auto installedA = choc::json::parse(processor->handleUiMessage(
      R"({"type":"pipeline/rebuild","payload":{"pipeline":"A","plugins":[)"
      R"({"id":57,"type":"DCOffsetPlugin","name":"DC Offset","enabled":true,)"
      R"("parameters":{"of":0},"wasmParams":[0],)"
      R"("wasmParamsHash":1104945464}]}})"));
  expect(installedA["ok"].getWithDefault<bool>(false),
         "install the playing pipeline that owns the runtime image");
  const auto idleParameterId = boundAutomationParameterId(
      *processor, {'B', 57, "DCOffsetPlugin", "of", 0});
  expect(processor->setActive(true) == kResultOk,
         "activate the inactive-pipeline automation ownership test");

  std::array<float, 64> inputLeft{};
  std::array<float, 64> inputRight{};
  std::array<float, 64> outputLeft{};
  std::array<float, 64> outputRight{};
  Sample32 *inputChannels[]{inputLeft.data(), inputRight.data()};
  Sample32 *outputChannels[]{outputLeft.data(), outputRight.data()};
  AudioBusBuffers input{};
  input.numChannels = 2;
  input.channelBuffers32 = inputChannels;
  AudioBusBuffers output{};
  output.numChannels = 2;
  output.channelBuffers32 = outputChannels;
  ProcessData data{};
  data.symbolicSampleSize = kSample32;
  data.numSamples = 64;
  data.numInputs = 1;
  data.numOutputs = 1;
  data.inputs = &input;
  data.outputs = &output;

  ParameterChanges changes(1);
  int32 queueIndex = 0;
  auto *queue = changes.addParameterData(idleParameterId, queueIndex);
  int32 pointIndex = 0;
  expect(queue != nullptr && queue->addPoint(0, 0.75, pointIndex) == kResultTrue,
         "automate the target bound to the idle pipeline");
  data.inputParameterChanges = &changes;
  expect(processor->process(data) == kResultOk,
         "process the idle-pipeline automation point");

  ResizableMemoryIBStream savedState;
  expect(processor->getState(&savedState) == kResultOk,
         "drain the published idle-pipeline automation value");
  expect(std::abs(PluginProcessorTestAccess::runtimePackedParameter(*processor, 57, 0)) <
             1.0e-6f,
         "the idle pipeline's automation stays out of the playing runtime image");

  expect(processor->setActive(false) == kResultOk,
         "deactivate before rebuilding from the runtime image");
  auto reconfigured = setup(48000.0, 64);
  expect(processor->setupProcessing(reconfigured) == kResultOk,
         "rebuild native DSP from the playing pipeline's runtime image");
  expect(processor->setActive(true) == kResultOk,
         "reactivate after rebuilding from the runtime image");
  outputLeft.fill(1.0f);
  outputRight.fill(1.0f);
  data.inputParameterChanges = nullptr;
  expect(processor->process(data) == kResultOk,
         "process the rebuilt playing pipeline");
  for (const auto sample : outputLeft) {
    expect(std::abs(sample) < 1.0e-6f,
           "the rebuilt playing pipeline carries no idle-pipeline offset");
  }
  expect(processor->setActive(false) == kResultOk,
         "deactivate the inactive-pipeline automation ownership test");
  expect(processor->terminate() == kResultOk,
         "terminate the inactive-pipeline automation ownership test");
}

// The parameter mailbox and the runtime image have one consumer each. While the
// control service owns them a block must still be processed with the values the
// engine already holds; dropping to the input signal is exactly the audible
// defect the ownership claim exists to prevent.
void testControlOwnedRuntimeImageKeepsProcessedAudio() {
  auto processor = std::make_unique<EffeTuneProcessor>();
  expect(processor->initialize(nullptr) == kResultOk,
         "initialize the owned runtime-image test");
  auto processSetup = setup(48000.0, 64);
  expect(processor->setupProcessing(processSetup) == kResultOk,
         "prepare the owned runtime-image test");
  // An unsmoothed offset makes the block that carries the image unambiguous:
  // the dry input, the previous image and the deferred image are three
  // different constants.
  const auto installed = choc::json::parse(processor->handleUiMessage(
      R"({"type":"pipeline/rebuild","payload":{"pipeline":"A","plugins":[)"
      R"({"id":61,"type":"DCOffsetPlugin","name":"DC Offset","enabled":true,)"
      R"("parameters":{"of":0.25},"wasmParams":[0.25],)"
      R"("wasmParamsHash":1104945464}]}})"));
  expect(installed["ok"].getWithDefault<bool>(false),
         "install the owned runtime-image pipeline");
  expect(processor->setActive(true) == kResultOk,
         "activate the owned runtime-image test");

  std::array<float, 64> inputLeft{};
  std::array<float, 64> inputRight{};
  std::array<float, 64> outputLeft{};
  std::array<float, 64> outputRight{};
  inputLeft.fill(1.0f);
  inputRight.fill(1.0f);
  Sample32 *inputChannels[]{inputLeft.data(), inputRight.data()};
  Sample32 *outputChannels[]{outputLeft.data(), outputRight.data()};
  AudioBusBuffers input{};
  input.numChannels = 2;
  input.channelBuffers32 = inputChannels;
  AudioBusBuffers output{};
  output.numChannels = 2;
  output.channelBuffers32 = outputChannels;
  ProcessData data{};
  data.symbolicSampleSize = kSample32;
  data.numSamples = 64;
  data.numInputs = 1;
  data.numOutputs = 1;
  data.inputs = &input;
  data.outputs = &output;

  const auto expectOffset = [&](const float offset, const std::string &message) {
    for (const auto sample : outputLeft) {
      expect(std::abs(sample - (1.0f + offset)) < 1.0e-6f, message);
    }
  };

  expect(processor->process(data) == kResultOk, "warm the owned runtime-image test");
  expectOffset(0.25f, "the warm-up block is wet at the installed offset");

  PluginProcessorTestAccess::setControlOwnsRuntimeImage(*processor, true);
  const auto update = choc::json::parse(processor->handleUiMessage(
      R"({"type":"pipeline/updatePlugin","payload":{"pipeline":"A","plugin":)"
      R"({"id":61,"type":"DCOffsetPlugin","name":"DC Offset","enabled":true,)"
      R"("parameters":{"of":0.5},"wasmParams":[0.5],)"
      R"("wasmParamsHash":1104945464}}})"));
  expect(update["ok"].getWithDefault<bool>(false),
         "publish a full image while the control service owns it");
  outputLeft.fill(0.0f);
  outputRight.fill(0.0f);
  expect(processor->process(data) == kResultOk,
         "process a block while the control service owns the runtime image");
  expectOffset(0.25f,
               "an owned runtime image keeps the previous wet output instead of the dry input");

  PluginProcessorTestAccess::setControlOwnsRuntimeImage(*processor, false);
  outputLeft.fill(0.0f);
  outputRight.fill(0.0f);
  expect(processor->process(data) == kResultOk,
         "process a block after the control service releases the runtime image");
  expectOffset(0.5f, "the released runtime image applies the deferred full image");

  expect(processor->setActive(false) == kResultOk,
         "deactivate the owned runtime-image test");
  expect(processor->terminate() == kResultOk,
         "terminate the owned runtime-image test");
}

// Dragging a slider during playback reaches the native control service as a
// stream of parameter images while the transport keeps calling process(). None
// of that work may cost a block: every block has to reach the engine, complete
// inside it and come back wet, and no deferred fallback diagnostic may be
// raised. This is the case the audio callback used to try_lock its way through.
void testControlServiceEditsNeverCostAProcessedBlock() {
  auto processor = std::make_unique<EffeTuneProcessor>();
  expect(processor->initialize(nullptr) == kResultOk,
         "initialize the concurrent control-edit test");
  auto processSetup = setup(48000.0, 64);
  expect(processor->setupProcessing(processSetup) == kResultOk,
         "prepare the concurrent control-edit test");
  // An unsmoothed DC offset makes every block identify itself: the dry input is
  // exactly 1.0 and every published image adds a strictly positive offset.
  const auto installed = choc::json::parse(processor->handleUiMessage(
      R"({"type":"pipeline/rebuild","payload":{"pipeline":"A","plugins":[)"
      R"({"id":71,"type":"DCOffsetPlugin","name":"DC Offset","enabled":true,)"
      R"("parameters":{"of":0.25},"wasmParams":[0.25],)"
      R"("wasmParamsHash":1104945464}]}})"));
  expect(installed["ok"].getWithDefault<bool>(false),
         "install the concurrent control-edit pipeline");
  expect(processor->setActive(true) == kResultOk,
         "activate the concurrent control-edit test");

  std::array<float, 64> inputLeft{};
  std::array<float, 64> inputRight{};
  std::array<float, 64> outputLeft{};
  std::array<float, 64> outputRight{};
  inputLeft.fill(1.0f);
  inputRight.fill(1.0f);
  Sample32 *inputChannels[]{inputLeft.data(), inputRight.data()};
  Sample32 *outputChannels[]{outputLeft.data(), outputRight.data()};
  AudioBusBuffers input{};
  input.numChannels = 2;
  input.channelBuffers32 = inputChannels;
  AudioBusBuffers output{};
  output.numChannels = 2;
  output.channelBuffers32 = outputChannels;
  ProcessData data{};
  data.symbolicSampleSize = kSample32;
  data.numSamples = 64;
  data.numInputs = 1;
  data.numOutputs = 1;
  data.inputs = &input;
  data.outputs = &output;

  expect(processor->process(data) == kResultOk,
         "warm the concurrent control-edit test");
  expect(std::abs(outputLeft[0] - 1.25f) < 1.0e-6f,
         "the warm-up block is wet at the installed offset");

  const auto before = PluginProcessorTestAccess::processCounters(*processor);
  const auto failuresBefore =
      PluginProcessorTestAccess::processFailureSequence(*processor);

  std::atomic_bool running{true};
  std::atomic<std::uint64_t> serviceTicks{0};
  std::atomic_bool published{true};
  // The command is built once: the Debug allocation guard is process-wide, so
  // this thread must not reach the heap while a block is in the engine.
  auto command = std::make_unique<effetune::vst::AudioCommand>();
  std::thread control([&] {
    auto observedEpoch = PluginProcessorTestAccess::blockEpoch(*processor);
    std::uint32_t step = 0;
    while (running.load(std::memory_order_acquire)) {
      // A host runs the audio callback far more often than the 50 ms control
      // service, so the service only runs once a block has gone by. That keeps
      // the service off the idle path, where taking the engine over is free.
      const auto epoch = PluginProcessorTestAccess::blockEpoch(*processor);
      if (epoch == observedEpoch) {
        std::this_thread::yield();
        continue;
      }
      observedEpoch = epoch;
      const auto offset = 0.25f + static_cast<float>(step++ % 8u) * 0.03125f;
      if (!PluginProcessorTestAccess::publishParameterImage(
              *processor, *command, 71u, 1104945464u, offset)) {
        published.store(false, std::memory_order_release);
        return;
      }
      PluginProcessorTestAccess::serviceLatencyUpdates(*processor);
      serviceTicks.fetch_add(1, std::memory_order_acq_rel);
    }
  });

  constexpr std::uint64_t blocks = 1500;
  auto wetBlocks = std::uint64_t{0};
  for (std::uint64_t block = 0; block < blocks; ++block) {
    outputLeft.fill(0.0f);
    outputRight.fill(0.0f);
    expect(processor->process(data) == kResultOk,
           "process a block while the control service edits parameters");
    wetBlocks += outputLeft[0] > 1.0f + 1.0e-4f ? 1u : 0u;
  }
  running.store(false, std::memory_order_release);
  control.join();

  const auto after = PluginProcessorTestAccess::processCounters(*processor);
  expect(published.load(std::memory_order_acquire),
         "every parameter image reached the mailbox");
  expect(serviceTicks.load(std::memory_order_acquire) != 0,
         "the control service really ran alongside the audio callback");
  expect(after.batchAttempts - before.batchAttempts == blocks,
         "every block entered the engine instead of falling back to the input");
  expect(after.completedBatches - before.completedBatches == blocks,
         "every block completed inside the engine");
  expect(wetBlocks == blocks,
         "concurrent parameter edits never turn a block into dry audio");
  expect(PluginProcessorTestAccess::processFailureSequence(*processor) ==
             failuresBefore,
         "concurrent parameter edits raise no deferred audio diagnostic");

  expect(processor->setActive(false) == kResultOk,
         "deactivate the concurrent control-edit test");
  expect(processor->terminate() == kResultOk,
         "terminate the concurrent control-edit test");
}

void testBypassAtomicTracksSuccessfulBlockEndpoint() {
  auto processor = std::make_unique<EffeTuneProcessor>();
  expect(processor->initialize(nullptr) == kResultOk, "initialize bypass endpoint test");
  auto processSetup = setup(48000.0, 64);
  expect(processor->setupProcessing(processSetup) == kResultOk, "prepare bypass endpoint test");
  expect(processor->setActive(true) == kResultOk, "activate bypass endpoint test");

  ParameterChanges changes(1);
  int32 queueIndex = 0;
  auto *queue = changes.addParameterData(0, queueIndex);
  int32 pointIndex = 0;
  expect(queue != nullptr && queue->addPoint(32, 1.0, pointIndex) == kResultTrue,
         "create mid-block bypass point");
  std::array<float, 64> left{};
  std::array<float, 64> right{};
  std::array<float, 64> outputLeft{};
  std::array<float, 64> outputRight{};
  Sample32 *inputChannels[]{left.data(), right.data()};
  Sample32 *outputChannels[]{outputLeft.data(), outputRight.data()};
  AudioBusBuffers input{};
  input.numChannels = 2;
  input.channelBuffers32 = inputChannels;
  AudioBusBuffers output{};
  output.numChannels = 2;
  output.channelBuffers32 = outputChannels;
  ProcessData data{};
  data.symbolicSampleSize = kSample32;
  data.numSamples = 64;
  data.numInputs = 1;
  data.numOutputs = 1;
  data.inputs = &input;
  data.outputs = &output;
  data.inputParameterChanges = &changes;
  expect(processor->process(data) == kResultOk, "process mid-block bypass point");
  expect(hostInfo(*processor)["masterBypass"].getWithDefault<bool>(false),
         "successful completeBlock synchronizes the endpoint bypass atomic");
  expect(processor->setActive(false) == kResultOk, "deactivate bypass endpoint test");
  expect(processor->terminate() == kResultOk, "terminate bypass endpoint test");
}

// The master bypass button is the sibling of an automation gesture: performEdit()
// only notifies the host, so a host that never echoes the edit back through
// inputParameterChanges must still hear the toggle.
void testMasterBypassGestureReachesAudioWithoutHostEcho() {
  auto processor = std::make_unique<EffeTuneProcessor>();
  expect(processor->initialize(nullptr) == kResultOk,
         "initialize the unechoed-bypass test");
  auto *handler = new TestComponentHandler();
  expect(processor->setComponentHandler(handler) == kResultOk,
         "install the unechoed-bypass handler");
  auto processSetup = setup(48000.0, 64);
  expect(processor->setupProcessing(processSetup) == kResultOk,
         "prepare the unechoed-bypass test");
  const auto installed = choc::json::parse(processor->handleUiMessage(
      R"({"type":"pipeline/rebuild","payload":{"pipeline":"A","plugins":[)"
      R"({"id":31,"type":"DCOffsetPlugin","name":"DC Offset","enabled":true,)"
      R"("parameters":{"of":0.5},"wasmParams":[0.5],)"
      R"("wasmParamsHash":1104945464}]}})"));
  expect(installed["ok"].getWithDefault<bool>(false),
         "install the unechoed-bypass pipeline");
  expect(processor->setActive(true) == kResultOk,
         "activate the unechoed-bypass test");

  std::array<float, 64> inputLeft{};
  std::array<float, 64> inputRight{};
  std::array<float, 64> outputLeft{};
  std::array<float, 64> outputRight{};
  Sample32 *inputChannels[]{inputLeft.data(), inputRight.data()};
  Sample32 *outputChannels[]{outputLeft.data(), outputRight.data()};
  AudioBusBuffers input{};
  input.numChannels = 2;
  input.channelBuffers32 = inputChannels;
  AudioBusBuffers output{};
  output.numChannels = 2;
  output.channelBuffers32 = outputChannels;
  ProcessData data{};
  data.symbolicSampleSize = kSample32;
  data.numSamples = 64;
  data.numInputs = 1;
  data.numOutputs = 1;
  data.inputs = &input;
  data.outputs = &output;
  // The host records the bypass edit but never plays it back.
  data.inputParameterChanges = nullptr;

  const auto toggle = [&](const bool bypassed) {
    const auto response = choc::json::parse(processor->handleUiMessage(
        std::string{R"({"type":"pipeline/masterBypass","payload":{"value":)"} +
        (bypassed ? "true" : "false") + "}}"));
    expect(response["ok"].getWithDefault<bool>(false),
           "the master bypass gesture is accepted");
  };
  const auto render = [&](const float expected, const std::string &message) {
    outputLeft.fill(0.0f);
    outputRight.fill(0.0f);
    expect(processor->process(data) == kResultOk,
           "process the unechoed master bypass block");
    for (const auto sample : outputLeft) {
      expect(std::abs(sample - expected) < 1.0e-6f, message);
    }
  };

  render(0.5f, "the engaged pipeline offsets the silent input");

  toggle(true);
  expect(hostInfo(*processor)["masterBypass"].getWithDefault<bool>(false),
         "the gesture holds the reported bypass state before any block runs");
  render(0.0f,
         "master bypass reaches the audio path without a host parameter echo");
  expect(hostInfo(*processor)["masterBypass"].getWithDefault<bool>(false),
         "a completed block keeps the self-adopted bypass state");

  toggle(false);
  render(0.5f, "clearing master bypass restores the processed signal");
  expect(!hostInfo(*processor)["masterBypass"].getWithDefault<bool>(true),
         "a completed block keeps the self-adopted engaged state");

  expect(processor->setActive(false) == kResultOk,
         "deactivate the unechoed-bypass test");
  expect(processor->terminate() == kResultOk,
         "terminate the unechoed-bypass test");
  handler->release();
}

void testFailedTransactionRetriesLatestAutomationAndNotifiesOnce() {
  auto processor = std::make_unique<EffeTuneProcessor>();
  expect(processor->initialize(nullptr) == kResultOk,
         "initialize failed-transaction diagnostic test");
  auto processSetup = setup(48000.0, 64);
  expect(processor->setupProcessing(processSetup) == kResultOk,
         "prepare failed-transaction diagnostic test");
  const auto response = choc::json::parse(processor->handleUiMessage(
      R"({"type":"pipeline/rebuild","payload":{"pipeline":"A","plugins":[)"
      R"({"id":10,"type":"DCOffsetPlugin","name":"DC Offset","enabled":true,)"
      R"("parameters":{"of":0},"wasmParams":[0],)"
      R"("wasmParamsHash":1104945464}]}})"));
  expect(response["ok"].getWithDefault<bool>(false),
         "install failed-transaction automation pipeline");
  const auto dcOffsetParameterId = boundAutomationParameterId(
      *processor, {'A', 10, "DCOffsetPlugin", "of", 0});
  expect(processor->setActive(true) == kResultOk,
         "activate failed-transaction diagnostic test");

  ParameterChanges changes(1);
  int32 queueIndex = 0;
  auto *queue = changes.addParameterData(dcOffsetParameterId, queueIndex);
  int32 pointIndex = 0;
  expect(queue != nullptr && queue->addPoint(0, 0.75, pointIndex) == kResultTrue,
         "create latest automation value for failed transaction");

  std::array<float, 64> inputLeft{};
  std::array<float, 64> inputRight{};
  std::array<float, 64> outputLeft{};
  std::array<float, 64> outputRight{};
  float *inputChannels[]{inputLeft.data(), inputRight.data()};
  float *outputChannels[]{outputLeft.data(), outputRight.data()};
  AudioBusBuffers input{};
  input.numChannels = 2;
  input.channelBuffers32 = inputChannels;
  AudioBusBuffers output{};
  output.numChannels = 2;
  output.channelBuffers32 = outputChannels;
  ProcessData data{};
  data.symbolicSampleSize = kSample32;
  data.numSamples = 64;
  data.numInputs = 1;
  data.numOutputs = 1;
  data.inputs = &input;
  data.outputs = &output;
  data.inputParameterChanges = &changes;

  // An unsupported sample size is a genuine block failure: the automation
  // intake still runs, so the newest value has to survive to the next block.
  data.symbolicSampleSize = kSample64;
  expect(processor->process(data) == kResultOk,
         "an unsupported buffer fails the block that carries the automation");

  const auto diagnostic = hostInfo(*processor);
  expect(diagnostic["diagnostics"].size() == 1 &&
             diagnostic["diagnostics"][0]["code"].getWithDefault<std::string>({}) ==
                 "audio-processing-failure",
          "failed transaction burst publishes one deferred diagnostic");

  data.inputParameterChanges = nullptr;
  expect(processor->process(data) == kResultOk,
         "a second unsupported buffer continues the same failure burst");
  expect(hostInfo(*processor)["diagnostics"].size() == 0,
          "polling does not split consecutive failed blocks into new bursts");

  data.symbolicSampleSize = kSample32;
  for (int32 retry = 0; retry < 9; ++retry) {
    expect(processor->process(data) == kResultOk,
           "retry the latest automation value after a failed block");
  }
  for (int32 frame = 0; frame < data.numSamples; ++frame) {
    expect(std::abs(outputLeft[static_cast<std::size_t>(frame)] - 0.5f) < 1.0e-6f &&
               std::abs(outputRight[static_cast<std::size_t>(frame)] - 0.5f) < 1.0e-6f,
           "latest failed-block automation value reaches the successful retry");
  }
  expect(hostInfo(*processor)["diagnostics"].size() == 0,
          "drained transaction failure is not notified again after recovery");

  // A control thread holding the guard across a run of blocks is not a failure:
  // the audio callback never waits on it, so the blocks stay wet and silent.
  {
    const HeldControlGuard guard(*processor);
    for (int32 block = 0; block < 4; ++block) {
      outputLeft.fill(0.0f);
      outputRight.fill(0.0f);
      expect(processor->process(data) == kResultOk,
             "process a block while a control thread holds the guard");
    }
  }
  for (int32 frame = 0; frame < data.numSamples; ++frame) {
    expect(std::abs(outputLeft[static_cast<std::size_t>(frame)] - 0.5f) < 1.0e-6f &&
               std::abs(outputRight[static_cast<std::size_t>(frame)] - 0.5f) < 1.0e-6f,
           "a held control guard keeps the processed output instead of dry audio");
  }
  expect(hostInfo(*processor)["diagnostics"].size() == 0,
          "a held control guard raises no deferred diagnostic");

  data.symbolicSampleSize = kSample64;
  expect(processor->process(data) == kResultOk,
          "an invalid buffer begins the post-recovery failure burst");
  data.symbolicSampleSize = kSample32;
  const auto recoveredBurst = hostInfo(*processor);
  expect(recoveredBurst["diagnostics"].size() == 1,
          "a failed block after successful recovery starts one new diagnostic burst");
  const auto recoveredMessage =
      recoveredBurst["diagnostics"][0]["message"].getWithDefault<std::string>({});
  expect(recoveredMessage.find("audio buffer") != std::string::npos,
          "the post-recovery burst reports its latest failure reason");
  expect(processor->setActive(false) == kResultOk,
         "deactivate failed-transaction diagnostic test");
  expect(processor->terminate() == kResultOk,
         "terminate failed-transaction diagnostic test");
}

void testStateRestoreForceSurvivesUiRebuildBeforeAudioResume() {
  auto processor = std::make_unique<EffeTuneProcessor>();
  expect(processor->initialize(nullptr) == kResultOk,
         "initialize sticky state-restore test");
  auto *handler = new TestComponentHandler();
  expect(processor->setComponentHandler(handler) == kResultOk,
         "install the state-restore notification handler");
  auto processSetup = setup(48000.0, 64);
  expect(processor->setupProcessing(processSetup) == kResultOk,
         "prepare sticky state-restore test");
  const auto installed = choc::json::parse(processor->handleUiMessage(
      R"({"type":"pipeline/rebuild","payload":{"pipeline":"A","plugins":[)"
      R"({"id":9,"type":"DCOffsetPlugin","name":"DC Offset","enabled":true,)"
      R"("parameters":{"of":0},"wasmParams":[0],)"
      R"("wasmParamsHash":1104945464}]}})"));
  expect(installed["ok"].getWithDefault<bool>(false),
         "install sticky state-restore pipeline");
  const auto dcOffsetParameterId = boundAutomationParameterId(
      *processor, {'A', 9, "DCOffsetPlugin", "of", 0});
  expect(processor->setActive(true) == kResultOk,
         "activate sticky state-restore test");

  ParameterChanges changes(1);
  int32 queueIndex = 0;
  auto *queue = changes.addParameterData(dcOffsetParameterId, queueIndex);
  int32 pointIndex = 0;
  expect(queue != nullptr && queue->addPoint(0, 0.25, pointIndex) == kResultTrue,
         "establish pre-restore scheduler current");
  std::array<float, 64> inputLeft{};
  std::array<float, 64> inputRight{};
  std::array<float, 64> outputLeft{};
  std::array<float, 64> outputRight{};
  float *inputChannels[]{inputLeft.data(), inputRight.data()};
  float *outputChannels[]{outputLeft.data(), outputRight.data()};
  AudioBusBuffers input{};
  input.numChannels = 2;
  input.channelBuffers32 = inputChannels;
  AudioBusBuffers output{};
  output.numChannels = 2;
  output.channelBuffers32 = outputChannels;
  ProcessData data{};
  data.symbolicSampleSize = kSample32;
  data.numSamples = 64;
  data.numInputs = 1;
  data.numOutputs = 1;
  data.inputs = &input;
  data.outputs = &output;
  data.inputParameterChanges = &changes;
  expect(processor->process(data) == kResultOk,
         "consume pre-restore scheduler current");

  const auto stored = choc::json::parse(processor->handleUiMessage(
      R"({"type":"storage/readFile","payload":{"path":"pipeline-state.json"}})"));
  effetune::vst::PluginStateDocument restored;
  std::string decodeError;
  expect(effetune::vst::StateCodec::decode(
             stored["content"].getWithDefault<std::string>({}), restored, &decodeError),
         "decode sticky state-restore document: " + decodeError);
  const auto restoredPlugin = std::find_if(
      restored.pipelineA.plugins.begin(), restored.pipelineA.plugins.end(),
      [](const auto &plugin) { return plugin.id == 9; });
  expect(restoredPlugin != restored.pipelineA.plugins.end(),
         "sticky state-restore document contains the active effect");
  restoredPlugin->parametersJson = R"({"of":0.5})";
  const auto restoredJson = effetune::vst::StateCodec::encode(restored);
  ResizableMemoryIBStream restoredStream(restoredJson.size());
  int32 bytesWritten = 0;
  expect(restoredStream.write(const_cast<char *>(restoredJson.data()),
                              static_cast<int32>(restoredJson.size()),
                              &bytesWritten) == kResultOk &&
             bytesWritten == static_cast<int32>(restoredJson.size()),
         "write sticky state-restore document");
  restoredStream.rewind();
  expect(processor->setState(&restoredStream) == kResultOk,
         "publish forced state-restore automation configuration");

  // The chunk is the save authority immediately, but the current engine,
  // scheduler, runtime image and apply table remain one old playable
  // generation until the UI supplies their replacement. In particular, the
  // restored value must not be forced into the old DCOffset instance.
  ResizableMemoryIBStream immediateSave;
  expect(processor->getState(&immediateSave) == kResultOk,
         "save the decoded state before its runtime replacement arrives");
  effetune::vst::PluginStateDocument immediate;
  expect(effetune::vst::StateCodec::decode(
             std::string(static_cast<const char *>(immediateSave.getData()),
                         immediateSave.getCursor()),
             immediate, &decodeError),
         "decode the immediate restored save: " + decodeError);
  const auto immediatePlugin = std::find_if(
      immediate.pipelineA.plugins.begin(), immediate.pipelineA.plugins.end(),
      [](const auto &plugin) { return plugin.id == 9; });
  expect(immediatePlugin != immediate.pipelineA.plugins.end() &&
             std::abs(choc::json::parse(immediatePlugin->parametersJson)["of"]
                          .getWithDefault<double>(0.0) -
                      0.5) < 1.0e-9,
         "getState immediately carries the decoded replacement document");

  // Hosts commonly bracket setup changes with deactivation. Repeating the
  // exact conditions under which the retained generation was prepared is safe
  // and must not unnecessarily discard that still-playable DSP.
  expect(processor->setActive(false) == kResultOk,
         "deactivate before repeating the retained host conditions");
  expect(processor->setupProcessing(processSetup) == kResultOk,
         "repeat the unchanged host conditions during replacement wait");
  expect(PluginProcessorTestAccess::processingReady(*processor),
         "unchanged host conditions retain the old playable generation");
  expect(processor->setActive(true) == kResultOk,
         "reactivate the retained generation");

  data.inputParameterChanges = nullptr;
  for (int32 block = 0; block < 32; ++block) {
    expect(processor->process(data) == kResultOk,
           "keep rendering the old generation before the replacement");
  }
  for (const auto sample : outputLeft) {
    expect(std::abs(sample + 0.5f) < 1.0e-6f,
           "the old scheduler and old runtime stay wet and mutually consistent");
  }
  expect(hostInfo(*processor)["diagnostics"].size() == 0,
         "waiting for the replacement produces no not-ready diagnostic");

  // A different sample rate or maximum block size cannot be paired with those
  // old buffers and resamplers. setupProcessing may accept the host contract,
  // but the processing gate stays closed until the replacement image is fully
  // configured under the new conditions.
  expect(processor->setActive(false) == kResultOk,
         "deactivate before changing retained host conditions");
  auto replacementSetup = setup(96000.0, 128);
  expect(processor->setupProcessing(replacementSetup) == kResultOk,
         "accept new host conditions while the runtime replacement is pending");
  expect(!PluginProcessorTestAccess::processingReady(*processor),
         "changed host conditions gate the incompatible old generation");
  expect(processor->setupProcessing(replacementSetup) == kResultOk,
         "accept a repeated setup after the old generation is gated");
  expect(!PluginProcessorTestAccess::processingReady(*processor),
         "repeated setup cannot rebuild a mixed replacement generation");
  SpeakerArrangement monoInput = SpeakerArr::kMono;
  SpeakerArrangement monoOutput = SpeakerArr::kMono;
  expect(processor->setBusArrangements(&monoInput, 1, &monoOutput, 1) ==
             kResultTrue,
         "accept a channel change while the runtime replacement is pending");
  expect(!PluginProcessorTestAccess::processingReady(*processor),
         "a channel change keeps the incompatible old generation gated");
  SpeakerArrangement stereoInput = SpeakerArr::kStereo;
  SpeakerArrangement stereoOutput = SpeakerArr::kStereo;
  expect(processor->setBusArrangements(&stereoInput, 1, &stereoOutput, 1) ==
             kResultTrue,
         "publish the final replacement channel layout");
  expect(!PluginProcessorTestAccess::processingReady(*processor),
         "the final layout still waits for one coherent replacement rebuild");

  const auto replacementStartup = choc::json::parse(processor->handleUiMessage(
      R"({"type":"host/getInfo","payload":{"startup":true}})"));
  expect(replacementStartup["ok"].getWithDefault<bool>(false),
         "announce the page that owns the restored runtime image");
  handler->clearEditLog();
  handler->restartFlags = 0;
  const auto rebuilt = choc::json::parse(processor->handleUiMessage(
      R"({"type":"pipeline/rebuild","payload":{"pipeline":"A","plugins":[)"
      R"({"id":9,"type":"DCOffsetPlugin","name":"DC Offset","enabled":true,)"
      R"("parameters":{"of":0.5},"wasmParams":[0.5],)"
      R"("wasmParamsHash":1104945464}]}})"));
  expect(rebuilt["ok"].getWithDefault<bool>(false),
         "coalesce UI rebuild before audio consumes restored state");
  expect((handler->restartFlags & RestartFlags::kParamValuesChanged) != 0 &&
             (handler->restartFlags & RestartFlags::kParamTitlesChanged) == 0,
         "a value-only state restore refreshes values without invalidating definitions");
  expect(processor->getParamNormalized(dcOffsetParameterId) == 0.75,
         "the refreshed controller value belongs to the restored state");

  expect(processor->setActive(true) == kResultOk,
         "activate the replacement under its new host conditions");

  expect(processor->process(data) == kResultOk,
         "resume the first audio block after sticky state restore");
  for (int32 block = 0; block < 32; ++block) {
    expect(processor->process(data) == kResultOk,
           "resume audio after sticky state restore");
  }
  for (const auto sample : outputLeft) {
    expect(std::abs(sample - 0.5f) < 1.0e-6f,
           "restored current wins over the pre-restore scheduler current");
  }
  expect(processor->setActive(false) == kResultOk,
         "deactivate sticky state-restore test");
  expect(processor->terminate() == kResultOk,
         "terminate sticky state-restore test");
  handler->release();
}

// Idleness is a safety decision, not a responsiveness timer. A host whose
// maximum block spans 400 ms can legitimately leave more than the old 250 ms
// ceiling between rendered-block observations. The horizon therefore remains
// three complete block periods, and an odd process epoch is direct proof of a
// callback in flight regardless of how old the last completed block is.
void testAudioIdleHorizonCoversSlowHostBlocks() {
  auto processor = std::make_unique<EffeTuneProcessor>();
  using Clock = std::chrono::steady_clock;
  using namespace std::chrono_literals;
  const Clock::time_point observedAt{10s};
  PluginProcessorTestAccess::setAudioIdleObservation(
      *processor, /*maxHostFrames=*/400, /*sampleRate=*/1000.0,
      /*renderedBlocks=*/7, observedAt);
  expect(PluginProcessorTestAccess::audioIdleThreshold(*processor) == 1200ms,
         "a slow host keeps the complete three-block idle horizon");
  expect(!PluginProcessorTestAccess::observeAudioIdle(*processor,
                                                       observedAt + 399ms),
         "the middle of one large block is never idle");
  expect(!PluginProcessorTestAccess::observeAudioIdle(*processor,
                                                       observedAt + 800ms),
         "the interval between slow blocks stays inside the safety horizon");
  PluginProcessorTestAccess::beginSyntheticBlock(*processor);
  const auto idleWhileProcessing = PluginProcessorTestAccess::observeAudioIdle(
      *processor, observedAt + 5s);
  PluginProcessorTestAccess::endSyntheticBlock(*processor);
  expect(!idleWhileProcessing,
         "an odd process epoch always reports busy even beyond the horizon");
  expect(PluginProcessorTestAccess::observeAudioIdle(*processor,
                                                      observedAt + 1200ms),
         "the callback is idle after the full safety horizon has elapsed");
}

void testPendingFullImageSurvivesReconfiguration() {
  auto processor = std::make_unique<EffeTuneProcessor>();
  expect(processor->initialize(nullptr) == kResultOk,
         "initialize pending full-image reconfiguration test");
  auto initialSetup = setup(48000.0, 64);
  expect(processor->setupProcessing(initialSetup) == kResultOk,
         "prepare pending full-image reconfiguration test");
  installLimiterPipeline(*processor, 31);
  expect(processor->getLatencySamples() == 144u,
         "initial limiter full image publishes three millisecond latency");

  const auto update = choc::json::parse(processor->handleUiMessage(
      R"({"type":"pipeline/updatePlugin","payload":{"pipeline":"A","plugin":)"
      R"({"id":31,"type":"BrickwallLimiterPlugin","name":"Brickwall Limiter",)"
      R"("enabled":true,"parameters":{"th":0,"rl":100,"la":10,"os":1,"ig":0,"sm":-1},)"
      R"("wasmParams":[0,100,10,1,0,-1],"wasmParamsHash":3039928906}}})"));
  expect(update["ok"].getWithDefault<bool>(false),
         "publish a full image without processing it");
  expect(processor->getLatencySamples() == 144u,
         "an unconsumed mailbox image has not reached native DSP yet");

  auto reconfigured = setup(48000.0, 128);
  expect(processor->setupProcessing(reconfigured) == kResultOk,
         "reconfigure before the audio thread consumes the mailbox");
  expect(processor->getLatencySamples() == 480u,
         "reconfiguration adopts the mailbox image into the runtime shadow");
  expect(processor->terminate() == kResultOk,
         "terminate pending full-image reconfiguration test");
}

void testStoppedParameterImageIsServicedWithoutAudioPolling() {
  auto processor = std::make_unique<EffeTuneProcessor>();
  expect(processor->initialize(nullptr) == kResultOk,
         "initialize stopped parameter control-service test");
  auto *handler = new TestComponentHandler();
  expect(processor->setComponentHandler(handler) == kResultOk,
         "install stopped parameter latency handler");
  auto processSetup = setup(48000.0, 64);
  expect(processor->setupProcessing(processSetup) == kResultOk,
         "prepare stopped parameter control-service test");
  installLimiterPipeline(*processor, 34);
  expect(processor->getLatencySamples() == 144u &&
             processor->setActive(true) == kResultOk,
         "activate stopped three millisecond limiter");

  updateLimiterPlugin(*processor, 34, 10.0f);
  const auto latencyGeneration =
      PluginProcessorTestAccess::parameterImageGeneration(*processor);
  expect(latencyGeneration >
             PluginProcessorTestAccess::servicedParameterImageGeneration(*processor),
         "stopped parameter image wakes the native control service");
  expect(pumpMainThreadUntil(
             [&] {
               return PluginProcessorTestAccess::servicedParameterImageGeneration(
                          *processor) == latencyGeneration &&
                      PluginProcessorTestAccess::servicedPipelinePlanRevision(
                          *processor) ==
                          PluginProcessorTestAccess::pipelinePlanRevision(*processor) &&
                      processor->getLatencySamples() == 480u &&
                      handler->latencyRestartCount != 0u;
             },
             std::chrono::milliseconds(500)),
         "message-loop service updates stopped transport PDC without audio or UI polling");
  expect(!PluginProcessorTestAccess::hasPendingControlWork(*processor),
         "stopped latency image is fully acknowledged before audio resumes");

  const auto planBeforeIdentityUpdate =
      PluginProcessorTestAccess::pipelinePlanRevision(*processor);
  const auto restartsBeforeIdentityUpdate = handler->latencyRestartCount;
  const auto identityUpdate = choc::json::parse(processor->handleUiMessage(
      R"({"type":"pipeline/updatePlugin","payload":{"pipeline":"A","plugin":)"
      R"({"id":34,"type":"BrickwallLimiterPlugin","name":"Brickwall Limiter",)"
      R"("enabled":true,"parameters":{"th":-6,"rl":100,"la":10,"os":1,"ig":0,"sm":-1},)"
      R"("wasmParams":[-6,100,10,1,0,-1],"wasmParamsHash":3039928906}}})"));
  expect(identityUpdate["ok"].getWithDefault<bool>(false),
         "publish stopped identity-only parameter image");
  const auto identityGeneration =
      PluginProcessorTestAccess::parameterImageGeneration(*processor);
  expect(pumpMainThreadUntil(
             [&] {
               return PluginProcessorTestAccess::servicedParameterImageGeneration(
                          *processor) == identityGeneration;
             },
             std::chrono::milliseconds(250)),
         "native control service acknowledges identity-only image");
  const auto debounceDeadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(150);
  expect(pumpMainThreadUntil(
             [&] { return std::chrono::steady_clock::now() >= debounceDeadline; },
             std::chrono::milliseconds(250)),
         "allow the identity-only notification debounce to expire");
  expect(PluginProcessorTestAccess::pipelinePlanRevision(*processor) ==
             planBeforeIdentityUpdate &&
             processor->getLatencySamples() == 480u &&
             handler->latencyRestartCount == restartsBeforeIdentityUpdate &&
             !PluginProcessorTestAccess::hasPendingControlWork(*processor),
         "identity-only image avoids plan rebuild and PDC restart");

  std::array<float, 64> inputLeft{};
  std::array<float, 64> inputRight{};
  std::array<float, 64> outputLeft{};
  std::array<float, 64> outputRight{};
  inputLeft.fill(0.2f);
  inputRight.fill(-0.2f);
  Sample32 *inputChannels[]{inputLeft.data(), inputRight.data()};
  Sample32 *outputChannels[]{outputLeft.data(), outputRight.data()};
  AudioBusBuffers input{};
  input.numChannels = 2;
  input.channelBuffers32 = inputChannels;
  AudioBusBuffers output{};
  output.numChannels = 2;
  output.channelBuffers32 = outputChannels;
  ProcessData data{};
  data.symbolicSampleSize = kSample32;
  data.numSamples = 64;
  data.numInputs = 1;
  data.numOutputs = 1;
  data.inputs = &input;
  data.outputs = &output;
  expect(processor->process(data) == kResultOk,
         "process the first block after stopped control service");
  expect(std::all_of(outputLeft.begin(), outputLeft.end(),
                     [](const float sample) { return std::abs(sample) < 1.0e-7f; }) &&
             std::all_of(outputRight.begin(), outputRight.end(),
                         [](const float sample) { return std::abs(sample) < 1.0e-7f; }),
         "first resumed block is wet limiter latency, not pending raw fallback");

  expect(processor->setActive(false) == kResultOk,
         "deactivate stopped parameter control-service test");
  expect(processor->setComponentHandler(nullptr) == kResultOk,
         "remove stopped parameter latency handler");
  handler->release();
  expect(processor->terminate() == kResultOk,
         "terminate stopped parameter control-service test");
}

void testFullImageRefreshesLatencyOnceButAutomationDoesNot() {
  auto processor = std::make_unique<EffeTuneProcessor>();
  expect(processor->initialize(nullptr) == kResultOk,
         "initialize latency refresh classification test");
  auto processSetup = setup(48000.0, 64);
  expect(processor->setupProcessing(processSetup) == kResultOk,
         "prepare latency refresh classification test");
  installLimiterPipeline(*processor, 32);
  const auto thresholdParameterId = boundAutomationParameterId(
      *processor, {'A', 32, "BrickwallLimiterPlugin", "th", 0});
  expect(processor->setActive(true) == kResultOk,
         "activate latency refresh classification test");

  std::array<float, 64> inputLeft{};
  std::array<float, 64> inputRight{};
  std::array<float, 64> outputLeft{};
  std::array<float, 64> outputRight{};
  Sample32 *inputChannels[]{inputLeft.data(), inputRight.data()};
  Sample32 *outputChannels[]{outputLeft.data(), outputRight.data()};
  AudioBusBuffers input{};
  input.numChannels = 2;
  input.channelBuffers32 = inputChannels;
  AudioBusBuffers output{};
  output.numChannels = 2;
  output.channelBuffers32 = outputChannels;
  ProcessData data{};
  data.symbolicSampleSize = kSample32;
  data.numSamples = 64;
  data.numInputs = 1;
  data.numOutputs = 1;
  data.inputs = &input;
  data.outputs = &output;
  expect(processor->process(data) == kResultOk,
         "warm latency refresh classification processing");

  const auto update = choc::json::parse(processor->handleUiMessage(
      R"({"type":"pipeline/updatePlugin","payload":{"pipeline":"A","plugin":)"
      R"({"id":32,"type":"BrickwallLimiterPlugin","name":"Brickwall Limiter",)"
      R"("enabled":true,"parameters":{"th":0,"rl":100,"la":10,"os":1,"ig":0,"sm":-1},)"
      R"("wasmParams":[0,100,10,1,0,-1],"wasmParamsHash":3039928906}}})"));
  expect(update["ok"].getWithDefault<bool>(false),
         "publish the latency-changing full image");

  ParameterChanges fullImageChanges(1);
  int32 queueIndex = 0;
  auto *queue = fullImageChanges.addParameterData(thresholdParameterId, queueIndex);
  int32 pointIndex = 0;
  expect(queue != nullptr && queue->addPoint(0, 0.25, pointIndex) == kResultTrue &&
             queue->addPoint(32, 0.75, pointIndex) == kResultTrue,
         "create dense automation beside the full image");
  data.inputParameterChanges = &fullImageChanges;
  const auto before = PluginProcessorTestAccess::processCounters(*processor);
  expect(processor->process(data) == kResultOk,
         "stage one UI full image across dense automation slices");
  const auto afterFullImage = PluginProcessorTestAccess::processCounters(*processor);
  expect(afterFullImage.latencyRefreshes == before.latencyRefreshes + 1,
         "one host block refreshes latency once for its staged full image");
  const auto planRevisionAfterFullImage =
      PluginProcessorTestAccess::pipelinePlanRevision(*processor);

  ParameterChanges automationOnlyChanges(1);
  queueIndex = 0;
  queue = automationOnlyChanges.addParameterData(thresholdParameterId, queueIndex);
  pointIndex = 0;
  expect(queue != nullptr && queue->addPoint(0, 0.4, pointIndex) == kResultTrue &&
             queue->addPoint(32, 0.6, pointIndex) == kResultTrue,
         "create an automation-only host block");
  data.inputParameterChanges = &automationOnlyChanges;
  expect(processor->process(data) == kResultOk,
         "process the automation-only host block");
  const auto afterAutomation = PluginProcessorTestAccess::processCounters(*processor);
  expect(afterAutomation.latencyRefreshes == afterFullImage.latencyRefreshes &&
             PluginProcessorTestAccess::pipelinePlanRevision(*processor) ==
                 planRevisionAfterFullImage,
         "automation-only staging does not poll latency or request a plan rebuild");
  expect(processor->setActive(false) == kResultOk,
         "deactivate latency refresh classification test");
  expect(processor->terminate() == kResultOk,
         "terminate latency refresh classification test");
}

void testDeferredLatencyPlanRefreshAlignsParallelAndBypassPaths() {
  auto processor = std::make_unique<EffeTuneProcessor>();
  expect(processor->initialize(nullptr) == kResultOk,
         "initialize deferred latency-plan test");
  auto *handler = new TestComponentHandler();
  expect(processor->setComponentHandler(handler) == kResultOk,
         "install latency notification handler");
  auto processSetup = setup(48000.0, 64);
  expect(processor->setupProcessing(processSetup) == kResultOk,
         "prepare deferred latency-plan test");
  installParallelLimiterPipeline(*processor);
  expect(processor->getLatencySamples() == 144u,
         "initial parallel limiter latency is the three millisecond path");
  expect(processor->setActive(true) == kResultOk,
         "activate deferred latency-plan test");

  std::array<float, 64> inputLeft{};
  std::array<float, 64> inputRight{};
  std::array<float, 64> outputLeft{};
  std::array<float, 64> outputRight{};
  Sample32 *inputChannels[]{inputLeft.data(), inputRight.data()};
  Sample32 *outputChannels[]{outputLeft.data(), outputRight.data()};
  AudioBusBuffers input{};
  input.numChannels = 2;
  input.channelBuffers32 = inputChannels;
  AudioBusBuffers output{};
  output.numChannels = 2;
  output.channelBuffers32 = outputChannels;
  ProcessData data{};
  data.symbolicSampleSize = kSample32;
  data.numSamples = 64;
  data.numInputs = 1;
  data.numOutputs = 1;
  data.inputs = &input;
  data.outputs = &output;

  for (int32 block = 0; block < 10; ++block) {
    expect(processor->process(data) == kResultOk,
           "warm initial parallel limiter paths");
  }
  const auto previousPlanRevision =
      PluginProcessorTestAccess::pipelinePlanRevision(*processor);
  const auto previousLatencyRevision =
      PluginProcessorTestAccess::latencyRevision(*processor);
  PluginProcessorTestAccess::failPipelinePlanRefreshes(*processor, 2);
  const auto failureSequenceBeforeRefresh =
      PluginProcessorTestAccess::processFailureSequence(*processor);
  const auto update = choc::json::parse(processor->handleUiMessage(
      R"({"type":"pipeline/updatePlugin","payload":{"pipeline":"A","plugin":)"
      R"({"id":41,"type":"BrickwallLimiterPlugin","name":"Left Limiter",)"
      R"("enabled":true,"inputBus":0,"outputBus":0,"channel":"L",)"
      R"("parameters":{"th":0,"rl":100,"la":10,"os":1,"ig":0,"sm":-1},)"
      R"("wasmParams":[0,100,10,1,0,-1],"wasmParamsHash":3039928906}}})"));
  expect(update["ok"].getWithDefault<bool>(false),
         "publish the left limiter ten millisecond full image");
  expect(processor->process(data) == kResultOk,
         "audio thread stages the latency-changing full image");
  const auto pendingPlanRevision =
      PluginProcessorTestAccess::pipelinePlanRevision(*processor);
  expect(pendingPlanRevision > previousPlanRevision &&
             PluginProcessorTestAccess::latencyRevision(*processor) ==
                 previousLatencyRevision &&
             PluginProcessorTestAccess::servicedPipelinePlanRevision(*processor) <
                 pendingPlanRevision &&
             processor->getLatencySamples() == 144u,
         "audio staging requests a plan without changing the applied latency or host PDC");

  data.inputParameterChanges = nullptr;
  inputLeft.fill(0.25f);
  inputRight.fill(-0.25f);
  for (int32 block = 0; block < 10; ++block) {
    outputLeft.fill(0.0f);
    outputRight.fill(0.0f);
    expect(processor->process(data) == kResultOk,
           "pending latency plan keeps rendering audio");
  }
  expect(std::abs(outputLeft.back() - inputLeft.back()) < 1.0e-7f &&
             std::abs(outputRight.back() - inputRight.back()) < 1.0e-7f &&
             processor->getLatencySamples() == 144u,
         "pending latency plan keeps the transparent output and the previous PDC");

  const auto latencyRestartsBeforeRefresh = handler->latencyRestartCount;
  // The debounced PDC notification is issued from the control-service tick,
  // which holds processingResourcesMutex_ across everything it services. A host
  // processes restartComponent() inline and may answer it by asking for the
  // state or by writing a parameter back through setParamNormalized(), both of
  // which re-enter the plug-in through that same non-recursive mutex. This is
  // the one host call the tick makes, so it is the one that has to be issued
  // with the lock released. Armed before the pumps below, because the debounce
  // can expire inside any of them.
  handler->lockProbe = processor.get();
  handler->probeRestartLock = true;
  expect(pumpMainThreadUntil(
             [&] {
               return PluginProcessorTestAccess::remainingPipelinePlanRefreshFailures(
                          *processor) == 1u;
             },
             std::chrono::milliseconds(250)),
         "native control timer attempts the first injected plan refresh");
  expect(pumpMainThreadUntil(
             [&] {
               return PluginProcessorTestAccess::remainingPipelinePlanRefreshFailures(
                          *processor) == 0u;
             },
             std::chrono::milliseconds(250)),
         "native control timer retries the same failed revision with backoff");
  expect(PluginProcessorTestAccess::processFailureSequence(*processor) ==
             failureSequenceBeforeRefresh,
         "a control-thread refresh failure never joins the audio failure burst");
  expect(pumpMainThreadUntil(
             [&] {
               return PluginProcessorTestAccess::servicedPipelinePlanRevision(
                          *processor) == pendingPlanRevision;
             },
             std::chrono::milliseconds(500)) &&
             processor->getLatencySamples() == 480u,
         "native control timer recovers and publishes ten millisecond PDC");

  inputLeft.fill(0.0f);
  inputRight.fill(0.0f);
  expect(processor->process(data) == kResultOk &&
             processor->process(data) == kResultOk,
         "successful audio after the topology guard clears the recovered diagnostic burst");

  expect(pumpMainThreadUntil(
             [&] {
               return handler->latencyRestartCount ==
                      latencyRestartsBeforeRefresh + 1u;
             },
             std::chrono::milliseconds(500)) &&
             (handler->restartFlags & RestartFlags::kLatencyChanged) != 0,
         "deferred PDC update notifies the host once after debounce");
  handler->probeRestartLock = false;
  handler->lockProbe = nullptr;
  expect(handler->probedRestartCount != 0,
         "the probe saw the restarts it was armed for");
  expect(!handler->restartFoundResourcesLocked,
         "and every one of them was issued with processingResourcesMutex_ "
         "released, so a host that answers a restart by re-entering the plug-in "
         "cannot deadlock");

  const auto settle = [&] {
    data.inputParameterChanges = nullptr;
    inputLeft.fill(0.0f);
    inputRight.fill(0.0f);
    for (int32 block = 0; block < 12; ++block) {
      outputLeft.fill(0.0f);
      outputRight.fill(0.0f);
      expect(processor->process(data) == kResultOk,
             "settle deferred latency topology history");
    }
  };
  const auto renderImpulse = [&] {
    std::array<float, 768> renderedLeft{};
    std::array<float, 768> renderedRight{};
    for (std::size_t block = 0; block < 12; ++block) {
      inputLeft.fill(0.0f);
      inputRight.fill(0.0f);
      outputLeft.fill(0.0f);
      outputRight.fill(0.0f);
      if (block == 0) {
        inputLeft[0] = 0.5f;
        inputRight[0] = 0.5f;
      }
      expect(processor->process(data) == kResultOk,
             "render aligned latency impulse");
      std::copy(outputLeft.begin(), outputLeft.end(),
                renderedLeft.begin() + block * outputLeft.size());
      std::copy(outputRight.begin(), outputRight.end(),
                renderedRight.begin() + block * outputRight.size());
    }
    const auto peak = [](const auto &samples) {
      return static_cast<std::size_t>(std::max_element(
                 samples.begin(), samples.end(), [](const float left, const float right) {
                   return std::abs(left) < std::abs(right);
                 }) -
                                      samples.begin());
    };
    return std::array{peak(renderedLeft), peak(renderedRight)};
  };

  settle();
  const auto activePeaks = renderImpulse();
  expect(activePeaks[0] == 480u && activePeaks[1] == 480u,
         "updated parallel wet paths align at the reported latency");

  ParameterChanges bypassChanges(1);
  int32 queueIndex = 0;
  auto *queue = bypassChanges.addParameterData(kBypassParameterId, queueIndex);
  int32 pointIndex = 0;
  expect(queue != nullptr && queue->addPoint(0, 1.0, pointIndex) == kResultTrue,
         "enable master bypass for dry latency alignment");
  data.inputParameterChanges = &bypassChanges;
  expect(processor->process(data) == kResultOk,
         "apply master bypass before impulse rendering");
  settle();
  const auto bypassPeaks = renderImpulse();
  expect(bypassPeaks[0] == 480u && bypassPeaks[1] == 480u,
         "master-bypass dry paths align at the updated reported latency");

  expect(processor->setActive(false) == kResultOk,
         "deactivate deferred latency-plan test");
  expect(processor->setComponentHandler(nullptr) == kResultOk,
         "remove latency notification handler");
  handler->release();
  expect(processor->terminate() == kResultOk,
         "terminate deferred latency-plan test");
}

void testTopologyDescriptorIsServicedOutsideAudioCallback() {
  auto processor = std::make_unique<EffeTuneProcessor>();
  expect(processor->initialize(nullptr) == kResultOk,
         "initialize non-RT topology descriptor test");
  auto processSetup = setup(48000.0, 64);
  expect(processor->setupProcessing(processSetup) == kResultOk,
         "prepare non-RT topology descriptor test");
  installLimiterPipeline(*processor, 44);
  expect(processor->setActive(true) == kResultOk,
         "activate non-RT topology descriptor test");

  std::array<float, 64> inputLeft{};
  std::array<float, 64> inputRight{};
  std::array<float, 64> outputLeft{};
  std::array<float, 64> outputRight{};
  inputLeft.fill(0.2f);
  inputRight.fill(-0.2f);
  Sample32 *inputChannels[]{inputLeft.data(), inputRight.data()};
  Sample32 *outputChannels[]{outputLeft.data(), outputRight.data()};
  AudioBusBuffers input{};
  input.numChannels = 2;
  input.channelBuffers32 = inputChannels;
  AudioBusBuffers output{};
  output.numChannels = 2;
  output.channelBuffers32 = outputChannels;
  ProcessData data{};
  data.symbolicSampleSize = kSample32;
  data.numSamples = 64;
  data.numInputs = 1;
  data.numOutputs = 1;
  data.inputs = &input;
  data.outputs = &output;
  for (int block = 0; block < 4; ++block) {
    expect(processor->process(data) == kResultOk,
           "warm non-RT topology descriptor test");
  }

  updateLimiterRouting(*processor, 44, "L");
  const auto generation =
      PluginProcessorTestAccess::descriptorGeneration(*processor);
  expect(generation >
             PluginProcessorTestAccess::servicedDescriptorGeneration(*processor),
         "topology update is pending before the audio callback");

  effetune::allocation_guard::setAbortOnViolationForTesting(false);
  const auto allocationsBefore = effetune::allocation_guard::violationCount();
  Steinberg::tresult processResult = kResultFalse;
  {
    effetune::allocation_guard::Scope allocationScope;
    processResult = processor->process(data);
  }
  effetune::allocation_guard::setAbortOnViolationForTesting(true);
  expect(processResult == kResultOk,
         "render audio while the descriptor is pending");
  expect(effetune::allocation_guard::violationCount() == allocationsBefore,
         "audio callback does not allocate while a channel-specific descriptor is pending");
  expect(std::equal(outputLeft.begin(), outputLeft.end(), inputLeft.begin()) &&
             std::equal(outputRight.begin(), outputRight.end(), inputRight.begin()),
         "pending descriptor block keeps the transparent limiter output");

  expect(pumpMainThreadUntil(
             [&] {
               return PluginProcessorTestAccess::servicedDescriptorGeneration(
                          *processor) == generation;
             },
             std::chrono::milliseconds(500)),
         "native control timer applies the channel-specific descriptor");

  PluginProcessorTestAccess::failPipelinePlanRefreshes(*processor, 1);
  updateLimiterRouting(*processor, 44, "R");
  const auto failedGeneration =
      PluginProcessorTestAccess::descriptorGeneration(*processor);
  expect(pumpMainThreadUntil(
             [&] {
               return PluginProcessorTestAccess::remainingPipelinePlanRefreshFailures(
                          *processor) == 0u;
             },
             std::chrono::milliseconds(250)),
         "inject one non-RT descriptor configuration failure");
  PluginProcessorTestAccess::deferPipelinePlanRetry(*processor,
                                                    std::chrono::seconds(60));
  expect(PluginProcessorTestAccess::servicedDescriptorGeneration(*processor) <
             failedGeneration,
         "failed descriptor remains pending during retry backoff");
  PluginProcessorTestAccess::deferPipelinePlanRetry(*processor,
                                                    std::chrono::milliseconds(0));
  expect(pumpMainThreadUntil(
             [&] {
               return PluginProcessorTestAccess::servicedDescriptorGeneration(
                          *processor) == failedGeneration;
             },
             std::chrono::milliseconds(250)),
         "the retained descriptor succeeds on non-RT retry");

  updateLimiterRouting(*processor, 44, "L");
  const auto supersededGeneration =
      PluginProcessorTestAccess::descriptorGeneration(*processor);
  updateLimiterRouting(*processor, 44, "");
  const auto latestGeneration =
      PluginProcessorTestAccess::descriptorGeneration(*processor);
  expect(latestGeneration > supersededGeneration &&
             pumpMainThreadUntil(
                 [&] {
                   return PluginProcessorTestAccess::servicedDescriptorGeneration(
                              *processor) == latestGeneration;
                 },
                 std::chrono::milliseconds(250)),
         "multiple topology updates coalesce to the latest descriptor generation");
  expect(processor->setActive(false) == kResultOk,
         "deactivate non-RT topology descriptor test");
  expect(processor->terminate() == kResultOk,
         "terminate non-RT topology descriptor test");
}

void testAutomationSliceBlockDoesNotAllocate() {
  auto processor = std::make_unique<EffeTuneProcessor>();
  expect(processor->initialize(nullptr) == kResultOk,
         "initialize the automation-slice allocation test");
  auto processSetup = setup(48000.0, 64);
  expect(processor->setupProcessing(processSetup) == kResultOk,
         "prepare the automation-slice allocation test");
  const auto installed = choc::json::parse(processor->handleUiMessage(
      R"({"type":"pipeline/rebuild","payload":{"pipeline":"A","plugins":[)"
      R"({"id":57,"type":"DCOffsetPlugin","name":"DC Offset","enabled":true,)"
      R"("parameters":{"of":0},"wasmParams":[0],)"
      R"("wasmParamsHash":1104945464}]}})"));
  expect(installed["ok"].getWithDefault<bool>(false),
         "install the automation-slice allocation pipeline");
  const auto dcOffsetParameterId = boundAutomationParameterId(
      *processor, {'A', 57, "DCOffsetPlugin", "of", 0});
  expect(processor->setActive(true) == kResultOk,
         "activate the automation-slice allocation test");

  std::array<float, 64> inputLeft{};
  std::array<float, 64> inputRight{};
  std::array<float, 64> outputLeft{};
  std::array<float, 64> outputRight{};
  Sample32 *inputChannels[]{inputLeft.data(), inputRight.data()};
  Sample32 *outputChannels[]{outputLeft.data(), outputRight.data()};
  AudioBusBuffers input{};
  input.numChannels = 2;
  input.channelBuffers32 = inputChannels;
  AudioBusBuffers output{};
  output.numChannels = 2;
  output.channelBuffers32 = outputChannels;
  ProcessData data{};
  data.symbolicSampleSize = kSample32;
  data.numSamples = 64;
  data.numInputs = 1;
  data.numOutputs = 1;
  data.inputs = &input;
  data.outputs = &output;

  // Mid-block automation and bypass points force the sliced path, so the guard
  // below covers the apply-table read and the per-slice packed writes as well
  // as the block-epoch counter. The host queues are built once, outside the
  // guarded scope, because only process() is under measurement.
  ParameterChanges changes(2);
  int32 offsetQueueIndex = 0;
  auto *offsetQueue = changes.addParameterData(dcOffsetParameterId, offsetQueueIndex);
  int32 bypassQueueIndex = 0;
  auto *bypassQueue = changes.addParameterData(kBypassParameterId, bypassQueueIndex);
  expect(offsetQueue != nullptr && bypassQueue != nullptr,
         "create the sliced automation and bypass queues");
  int32 pointIndex = 0;
  expect(offsetQueue->addPoint(0, 0.625, pointIndex) == kResultTrue &&
             offsetQueue->addPoint(16, 0.75, pointIndex) == kResultTrue &&
             offsetQueue->addPoint(48, 0.25, pointIndex) == kResultTrue,
         "schedule mid-block automation points");
  expect(bypassQueue->addPoint(0, 0.0, pointIndex) == kResultTrue &&
             bypassQueue->addPoint(32, 1.0, pointIndex) == kResultTrue,
         "schedule a mid-block master-bypass point");
  data.inputParameterChanges = &changes;
  const auto renderSlicedBlock = [&] { return processor->process(data); };

  // The first blocks settle any one-time engine and scheduler state so the
  // measured block only exercises the steady-state audio path.
  for (int block = 0; block < 4; ++block) {
    expect(renderSlicedBlock() == kResultOk,
           "warm the automation-slice allocation test");
  }

  const auto failuresBefore =
      PluginProcessorTestAccess::processFailureSequence(*processor);
  effetune::allocation_guard::setAbortOnViolationForTesting(false);
  const auto allocationsBefore = effetune::allocation_guard::violationCount();
  Steinberg::tresult processResult = kResultFalse;
  {
    effetune::allocation_guard::Scope allocationScope;
    processResult = renderSlicedBlock();
  }
  effetune::allocation_guard::setAbortOnViolationForTesting(true);
  expect(processResult == kResultOk,
         "render the sliced automation and bypass block");
  expect(effetune::allocation_guard::violationCount() == allocationsBefore,
         "a sliced automation block reads the apply table and the block epoch "
         "without allocating");
  expect(PluginProcessorTestAccess::processFailureSequence(*processor) ==
             failuresBefore,
         "the measured block ran the full processing path instead of a "
         "degraded fallback");

  expect(processor->setActive(false) == kResultOk,
         "deactivate the automation-slice allocation test");
  expect(processor->terminate() == kResultOk,
         "terminate the automation-slice allocation test");
}

void testTelemetryPollLeavesTheAudioGuardAlone() {
  auto processor = std::make_unique<EffeTuneProcessor>();
  expect(processor->initialize(nullptr) == kResultOk,
         "initialize telemetry contention test");
  auto processSetup = setup(48000.0, 64);
  expect(processor->setupProcessing(processSetup) == kResultOk,
         "prepare telemetry contention test");
  const auto installed = choc::json::parse(processor->handleUiMessage(
      R"({"type":"pipeline/rebuild","payload":{"pipeline":"A","plugins":[)"
      R"({"id":9,"type":"DCOffsetPlugin","name":"DC Offset","enabled":true,)"
      R"("parameters":{"of":0},"wasmParams":[0],)"
      R"("wasmParamsHash":1104945464}]}})"));
  expect(installed["ok"].getWithDefault<bool>(false),
         "install the telemetry contention pipeline");
  const effetune::vst::AutomationTargetIdentity identity{'A', 9, "DCOffsetPlugin", "of", 0};
  const auto parameterId = boundAutomationParameterId(*processor, identity);
  expect(processor->setActive(true) == kResultOk,
         "activate telemetry contention test");

  // The editor polls telemetry at frame rate while the audio callback only
  // try_locks the processing guard. Holding the guard here stands in for a
  // block that is mid-flight: the poll must never wait on it.
  const auto pollWithoutTheGuard = [&](const std::string &message) {
    auto guard = PluginProcessorTestAccess::lockProcessingResources(*processor);
    std::atomic_bool completed{false};
    std::string response;
    std::thread poll([&] {
      response = processor->handleUiMessage(
          R"({"type":"telemetry/read","payload":{}})");
      completed.store(true, std::memory_order_release);
    });
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!completed.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    const auto completedWhileHeld = completed.load(std::memory_order_acquire);
    guard.unlock();
    poll.join();
    expect(completedWhileHeld, message);
    return completedWhileHeld ? choc::json::parse(response)
                              : choc::value::createObject({});
  };

  (void)pollWithoutTheGuard(
      "an idle telemetry poll does not wait on the audio processing guard");

  // Live host automation is the case that matters: every poll then carries a
  // delta, so the drained payload must still come from outside the guard.
  ParameterChanges changes(1);
  int32 queueIndex = 0;
  auto *queue = changes.addParameterData(parameterId, queueIndex);
  int32 pointIndex = 0;
  expect(queue != nullptr && queue->addPoint(0, 0.75, pointIndex) == kResultTrue,
         "queue host automation for the telemetry contention test");
  std::array<float, 64> inputLeft{};
  std::array<float, 64> inputRight{};
  std::array<float, 64> outputLeft{};
  std::array<float, 64> outputRight{};
  Sample32 *inputChannels[]{inputLeft.data(), inputRight.data()};
  Sample32 *outputChannels[]{outputLeft.data(), outputRight.data()};
  AudioBusBuffers input{};
  input.numChannels = 2;
  input.channelBuffers32 = inputChannels;
  AudioBusBuffers output{};
  output.numChannels = 2;
  output.channelBuffers32 = outputChannels;
  ProcessData data{};
  data.symbolicSampleSize = kSample32;
  data.numSamples = 64;
  data.numInputs = 1;
  data.numOutputs = 1;
  data.inputs = &input;
  data.outputs = &output;
  data.inputParameterChanges = &changes;
  expect(processor->process(data) == kResultOk,
         "apply host automation for the telemetry contention test");
  expect(pumpMainThreadUntil(
             [&] {
               return PluginProcessorTestAccess::automationDeltaPending(*processor);
             },
             std::chrono::milliseconds(500)),
         "the control service publishes an automation delta");

  const auto automated = pollWithoutTheGuard(
      "a telemetry poll carrying automation deltas does not wait on the guard");
  expect(!findAutomationDelta(automated, identity).isVoid(),
         "the poll drained the pending delta without the guard");

  expect(processor->setActive(false) == kResultOk,
         "deactivate telemetry contention test");
  expect(processor->terminate() == kResultOk,
         "terminate telemetry contention test");
}

void testPendingTopologyKeepsProcessedAudio() {
  auto processor = std::make_unique<EffeTuneProcessor>();
  expect(processor->initialize(nullptr) == kResultOk,
         "initialize pending-topology audio test");
  auto processSetup = setup(48000.0, 64);
  expect(processor->setupProcessing(processSetup) == kResultOk,
         "prepare pending-topology audio test");
  installGainPipeline(*processor);
  expect(processor->setActive(true) == kResultOk,
         "activate pending-topology audio test");

  std::array<float, 64> inputLeft{};
  std::array<float, 64> inputRight{};
  std::array<float, 64> outputLeft{};
  std::array<float, 64> outputRight{};
  inputLeft.fill(1.0f);
  inputRight.fill(-1.0f);
  Sample32 *inputChannels[]{inputLeft.data(), inputRight.data()};
  Sample32 *outputChannels[]{outputLeft.data(), outputRight.data()};
  AudioBusBuffers input{};
  input.numChannels = 2;
  input.channelBuffers32 = inputChannels;
  AudioBusBuffers output{};
  output.numChannels = 2;
  output.channelBuffers32 = outputChannels;
  ProcessData data{};
  data.processMode = kRealtime;
  data.symbolicSampleSize = kSample32;
  data.numSamples = 64;
  data.numInputs = 1;
  data.numOutputs = 1;
  data.inputs = &input;
  data.outputs = &output;

  constexpr auto processedGain = 0.5011872336f;
  const auto expectProcessedBlock = [&](const std::string &message) {
    for (std::size_t frame = 0; frame < outputLeft.size(); ++frame) {
      expect(std::abs(outputLeft[frame] - processedGain) < 1.0e-6f &&
                 std::abs(outputRight[frame] + processedGain) < 1.0e-6f,
             message);
    }
  };

  for (int block = 0; block < 4; ++block) {
    expect(processor->process(data) == kResultOk,
           "warm pending-topology audio test");
  }
  expectProcessedBlock("the gain pipeline is processing before the topology update");

  // A routing-only change reuses the native instance and is applied by the
  // non-real-time control service, so the audio thread still owns the
  // previously configured topology.
  const auto queued = choc::json::parse(processor->handleUiMessage(
      R"({"type":"pipeline/updatePlugin","payload":{"pipeline":"A","plugin":)"
      R"({"id":1,"type":"VolumePlugin","name":"Volume","enabled":true,)"
      R"("inputBus":0,"outputBus":0,"channel":"L",)"
      R"("parameters":{"vl":-6},"wasmParams":[-6],)"
      R"("wasmParamsHash":1719233191}}})"));
  expect(queued["ok"].getWithDefault<bool>(false),
         "queue the routing-only topology update");
  const auto generation =
      PluginProcessorTestAccess::descriptorGeneration(*processor);
  expect(generation >
             PluginProcessorTestAccess::servicedDescriptorGeneration(*processor),
         "the routing descriptor is pending before the audio callback");

  outputLeft.fill(0.0f);
  outputRight.fill(0.0f);
  expect(processor->process(data) == kResultOk,
         "process a block while the descriptor is pending");
  expectProcessedBlock(
      "a pending descriptor keeps the processed signal instead of the input");
  expect(generation >
             PluginProcessorTestAccess::servicedDescriptorGeneration(*processor),
         "the audio callback does not consume the pending descriptor");

  expect(pumpMainThreadUntil(
             [&] {
               return PluginProcessorTestAccess::servicedDescriptorGeneration(
                          *processor) == generation;
             },
             std::chrono::milliseconds(500)),
         "the native control timer applies the routing descriptor");
  outputLeft.fill(0.0f);
  outputRight.fill(0.0f);
  expect(processor->process(data) == kResultOk,
         "process a block after the descriptor is applied");
  for (std::size_t frame = 0; frame < outputLeft.size(); ++frame) {
    expect(std::abs(outputLeft[frame] - processedGain) < 1.0e-6f,
           "the applied routing keeps processing the left channel");
  }

  expect(processor->setActive(false) == kResultOk,
         "deactivate pending-topology audio test");
  expect(processor->terminate() == kResultOk,
         "terminate pending-topology audio test");
}

void testPendingPlanConsumesNewUiGenerationAndTopologyRemoval() {
  auto processor = std::make_unique<EffeTuneProcessor>();
  expect(processor->initialize(nullptr) == kResultOk,
         "initialize pending control-intake test");
  auto processSetup = setup(48000.0, 64);
  expect(processor->setupProcessing(processSetup) == kResultOk,
         "prepare pending control-intake test");
  installLimiterPipeline(*processor, 43);
  expect(processor->getLatencySamples() == 144u,
         "pending control-intake test begins at three milliseconds");
  expect(processor->setActive(true) == kResultOk,
         "activate pending control-intake test");

  std::array<float, 64> inputLeft{};
  std::array<float, 64> inputRight{};
  std::array<float, 64> outputLeft{};
  std::array<float, 64> outputRight{};
  Sample32 *inputChannels[]{inputLeft.data(), inputRight.data()};
  Sample32 *outputChannels[]{outputLeft.data(), outputRight.data()};
  AudioBusBuffers input{};
  input.numChannels = 2;
  input.channelBuffers32 = inputChannels;
  AudioBusBuffers output{};
  output.numChannels = 2;
  output.channelBuffers32 = outputChannels;
  ProcessData data{};
  data.symbolicSampleSize = kSample32;
  data.numSamples = 64;
  data.numInputs = 1;
  data.numOutputs = 1;
  data.inputs = &input;
  data.outputs = &output;
  inputLeft.fill(0.25f);
  inputRight.fill(-0.25f);

  for (int32 block = 0; block < 10; ++block) {
    expect(processor->process(data) == kResultOk,
           "warm pending control-intake processing");
  }

  PluginProcessorTestAccess::failPipelinePlanRefreshes(*processor, 1);
  updateLimiterPlugin(*processor, 43, 10.0f);
  expect(processor->process(data) == kResultOk,
         "stage the failing ten millisecond generation");
  const auto failedTenMillisecondRevision =
      PluginProcessorTestAccess::pipelinePlanRevision(*processor);
  expect(pumpMainThreadUntil(
             [&] {
               return PluginProcessorTestAccess::remainingPipelinePlanRefreshFailures(
                          *processor) == 0u;
             },
             std::chrono::milliseconds(250)),
         "inject the ten millisecond plan refresh failure");
  PluginProcessorTestAccess::deferPipelinePlanRetry(
      *processor, std::chrono::seconds(60));
  for (int32 block = 0; block < 8; ++block) {
    expect(processor->process(data) == kResultOk,
           "settle audio during the deferred retry");
  }

  updateLimiterPlugin(*processor, 43, 1.0f);
  outputLeft.fill(0.0f);
  outputRight.fill(0.0f);
  expect(processor->process(data) == kResultOk,
         "consume the one millisecond UI image while a plan is pending");
  expect(std::abs(outputLeft.back() - inputLeft.back()) < 1.0e-7f &&
             std::abs(outputRight.back() - inputRight.back()) < 1.0e-7f,
         "pending intake keeps the new image and that audio block transparent");
  expect(pumpMainThreadUntil(
             [&] {
               const auto published =
                   PluginProcessorTestAccess::pipelinePlanRevision(*processor);
               return published > failedTenMillisecondRevision &&
                      PluginProcessorTestAccess::servicedPipelinePlanRevision(
                          *processor) == published &&
                      processor->getLatencySamples() == 48u;
             },
             std::chrono::milliseconds(250)),
         "the newer UI generation bypasses the failed revision retry deadline");

  for (int32 block = 0; block < 12; ++block) {
    expect(processor->process(data) == kResultOk,
           "settle after the one millisecond plan recovery");
  }
  PluginProcessorTestAccess::failPipelinePlanRefreshes(*processor, 1);
  updateLimiterPlugin(*processor, 43, 10.0f);
  expect(processor->process(data) == kResultOk,
         "stage a second failing latency generation");
  const auto failedRemovalBaseRevision =
      PluginProcessorTestAccess::pipelinePlanRevision(*processor);
  expect(pumpMainThreadUntil(
             [&] {
               return PluginProcessorTestAccess::remainingPipelinePlanRefreshFailures(
                          *processor) == 0u;
             },
             std::chrono::milliseconds(250)),
         "inject the pre-removal plan refresh failure");
  PluginProcessorTestAccess::deferPipelinePlanRetry(
      *processor, std::chrono::seconds(60));

  updateLimiterPlugin(*processor, 43, 10.0f, false);
  const auto removalGeneration =
      PluginProcessorTestAccess::descriptorGeneration(*processor);
  expect(processor->process(data) == kResultOk,
         "keep rendering while the target-removing descriptor is pending");
  expect(removalGeneration >
             PluginProcessorTestAccess::servicedDescriptorGeneration(*processor),
         "the audio callback does not consume the target-removing descriptor");
  expect(pumpMainThreadUntil(
             [&] {
               return PluginProcessorTestAccess::servicedDescriptorGeneration(
                          *processor) == removalGeneration;
             },
             std::chrono::milliseconds(250)),
         "the target-removing descriptor bypasses the stale retry deadline");
  const auto removalRevision =
      PluginProcessorTestAccess::pipelinePlanRevision(*processor);
  expect(removalRevision > failedRemovalBaseRevision &&
             PluginProcessorTestAccess::servicedPipelinePlanRevision(*processor) ==
                 removalRevision &&
             processor->getLatencySamples() == 0u,
         "target removal publishes a new plan and clears host PDC");

  expect(processor->setActive(false) == kResultOk,
         "deactivate pending control-intake test");
  expect(processor->terminate() == kResultOk,
         "terminate pending control-intake test");
}

void testNativeControlServiceHandlesAssetReadyAndClear() {
  auto processor = std::make_unique<EffeTuneProcessor>();
  expect(processor->initialize(nullptr) == kResultOk,
         "initialize native asset control-service test");
  auto processSetup = setup(48000.0, 64);
  expect(processor->setupProcessing(processSetup) == kResultOk,
         "prepare native asset control-service test");
  installIrReverbPipeline(*processor);
  expect(processor->getLatencySamples() == 0u,
         "unprepared IR reverb begins without latency");
  expect(processor->setActive(true) == kResultOk,
         "activate native asset control-service test");

  const auto revisionBeforeStage =
      PluginProcessorTestAccess::pipelinePlanRevision(*processor);
  std::string error;
  expect(PluginProcessorTestAccess::setAsset(*processor, makeIrReverbAsset(), &error),
         "stage native IR asset without an editor: " + error);
  expect((PluginProcessorTestAccess::assetState(*processor, 91, 0) & 0xffu) ==
             ET_ASSET_STATE_PREPARING,
         "native IR asset enters preparing state");
  expect(PluginProcessorTestAccess::pipelinePlanRevision(*processor) >
             revisionBeforeStage,
         "asset commit publishes its compensation revision without waiting for audio");

  std::array<float, 64> inputLeft{};
  std::array<float, 64> inputRight{};
  std::array<float, 64> outputLeft{};
  std::array<float, 64> outputRight{};
  Sample32 *inputChannels[]{inputLeft.data(), inputRight.data()};
  Sample32 *outputChannels[]{outputLeft.data(), outputRight.data()};
  AudioBusBuffers input{};
  input.numChannels = 2;
  input.channelBuffers32 = inputChannels;
  AudioBusBuffers output{};
  output.numChannels = 2;
  output.channelBuffers32 = outputChannels;
  ProcessData data{};
  data.symbolicSampleSize = kSample32;
  data.numSamples = 64;
  data.numInputs = 1;
  data.numOutputs = 1;
  data.inputs = &input;
  data.outputs = &output;

  const auto revisionBeforeReady =
      PluginProcessorTestAccess::pipelinePlanRevision(*processor);
  auto assetPlanWasServiced = false;
  for (std::uint32_t block = 0;
       block < 512u &&
       (PluginProcessorTestAccess::assetState(*processor, 91, 0) & 0xffu) ==
           ET_ASSET_STATE_PREPARING;
       ++block) {
    expect(processor->process(data) == kResultOk,
           "prepare the native IR asset on the audio path");
    const auto publishedRevision =
        PluginProcessorTestAccess::pipelinePlanRevision(*processor);
    if (publishedRevision !=
        PluginProcessorTestAccess::servicedPipelinePlanRevision(*processor)) {
      expect(pumpMainThreadUntil(
                 [&] {
                   return PluginProcessorTestAccess::servicedPipelinePlanRevision(
                              *processor) == publishedRevision;
                 },
                 std::chrono::milliseconds(500)),
             "native control timer services an asset-preparation plan revision");
      assetPlanWasServiced = true;
    }
  }
  const auto readyState =
      PluginProcessorTestAccess::assetState(*processor, 91, 0) & 0xffu;
  const auto readyPlanRevision =
      PluginProcessorTestAccess::pipelinePlanRevision(*processor);
  const auto readyServicedRevision =
      PluginProcessorTestAccess::servicedPipelinePlanRevision(*processor);
  expect(readyState == ET_ASSET_STATE_ACTIVE && assetPlanWasServiced &&
             readyPlanRevision == revisionBeforeReady &&
             readyServicedRevision == readyPlanRevision &&
             processor->getLatencySamples() == 128u,
         "asset readiness is serviced without UI polling (state=" +
             std::to_string(readyState) + ", before=" +
             std::to_string(revisionBeforeReady) + ", plan=" +
             std::to_string(readyPlanRevision) + ", serviced=" +
             std::to_string(readyServicedRevision) + ")");
  const auto readyRevision = readyPlanRevision;

  expect(PluginProcessorTestAccess::clearAsset(*processor, 91, 0),
         "clear native IR asset without an editor");
  const auto clearRevision =
      PluginProcessorTestAccess::pipelinePlanRevision(*processor);
  expect(clearRevision > readyRevision &&
             PluginProcessorTestAccess::servicedPipelinePlanRevision(*processor) <
                 clearRevision,
         "asset clear publishes a pending compensation revision");
  expect(processor->process(data) == kResultOk &&
             pumpMainThreadUntil(
                 [&] {
                   return PluginProcessorTestAccess::servicedPipelinePlanRevision(
                              *processor) == clearRevision;
                 },
                 std::chrono::milliseconds(500)) &&
             processor->getLatencySamples() == 0u,
         "native control timer services asset clear without UI polling");

  expect(processor->setActive(false) == kResultOk,
         "deactivate native asset control-service test");
  expect(processor->terminate() == kResultOk,
         "terminate native asset control-service test");
}

// Group Delay EQ starts with no resident FIR and therefore zero latency. Its
// first non-zero edit commits an asset whose delay becomes effective before the
// host-facing PDC can safely be refreshed. The footer must follow that current
// DSP image instead of remaining at zero until a later topology edit.
void testGroupDelayAssetPublishesCurrentUiLatency() {
  auto processor = std::make_unique<EffeTuneProcessor>();
  expect(processor->initialize(nullptr) == kResultOk,
         "initialize Group Delay EQ latency publication test");
  auto processSetup = setup(48000.0, 64);
  expect(processor->setupProcessing(processSetup) == kResultOk,
         "prepare Group Delay EQ latency publication test");
  installGroupDelayEqPipeline(*processor);
  expect(processor->getLatencySamples() == 0u,
         "flat Group Delay EQ starts with zero reported latency");
  expect(processor->setActive(true) == kResultOk,
         "activate Group Delay EQ latency publication test");

  const auto revisionBeforeAsset =
      PluginProcessorTestAccess::pipelinePlanRevision(*processor);
  std::string error;
  expect(PluginProcessorTestAccess::setAsset(
             *processor, makeGroupDelayEqAsset(), &error),
         "commit the first non-flat Group Delay EQ FIR: " + error);
  expect(PluginProcessorTestAccess::pipelinePlanRevision(*processor) >
             revisionBeforeAsset,
         "Group Delay EQ asset commit immediately publishes a compensation revision");

  // A snapshot cannot be captured while this synthetic callback is in flight.
  // The UI must distinguish the applied plan from pending kernel latency.
  PluginProcessorTestAccess::beginSyntheticBlock(*processor);
  const auto info = hostInfo(*processor);
  PluginProcessorTestAccess::endSyntheticBlock(*processor);
  expect(info["latencySamples"].getWithDefault<std::int64_t>(-1) == 0 &&
             info["processingLatencySamples"].getWithDefault<std::int64_t>(-1) == 0 &&
             !info["latencyCompensated"].getWithDefault<bool>(true),
         "the UI reports only applied latency and marks the asset plan pending");

  std::array<float, 64> left{}, right{};
  Sample32 *channels[]{left.data(), right.data()};
  AudioBusBuffers buffer{};
  buffer.numChannels = 2;
  buffer.channelBuffers32 = channels;
  ProcessData data{};
  data.symbolicSampleSize = kSample32;
  data.numSamples = 64;
  data.numInputs = 1;
  data.numOutputs = 1;
  data.inputs = &buffer;
  data.outputs = &buffer;
  const auto before = PluginProcessorTestAccess::processCounters(*processor);
  for (int block = 0; block < 64; ++block) {
    expect(processor->process(data) == kResultOk, "activate the asset with callbacks running");
    PluginProcessorTestAccess::serviceLatencyUpdates(*processor);
  }
  const auto appliedInfo = hostInfo(*processor);
  expect(processor->getLatencySamples() == 128u + 8192u &&
             appliedInfo["latencyCompensated"].getWithDefault<bool>(false) &&
             PluginProcessorTestAccess::processCounters(*processor).completedBatches ==
                 before.completedBatches + 64,
         "asset compensation reaches wet, dry and public latency without dropping a block");

  expect(processor->setActive(false) == kResultOk,
         "deactivate Group Delay EQ latency publication test");
  expect(processor->terminate() == kResultOk,
         "terminate Group Delay EQ latency publication test");
}

// A pipeline that reports latency separates the three candidate timelines: the
// processed output lags the input, a dry fallback would not. Holding the control
// guard across a long run of blocks must therefore leave the output identical to
// an uncontended render of the same signal.
void testHeldControlGuardKeepsProcessedTimelineWithLatency() {
  constexpr int guardedFromBlock = 4;
  constexpr int guardedThroughBlock = 12;
  constexpr int totalBlocks = 13;

  std::array<float, 64> inputLeft{};
  std::array<float, 64> inputRight{};
  std::array<float, 64> outputLeft{};
  std::array<float, 64> outputRight{};
  Sample32 *inputChannels[]{inputLeft.data(), inputRight.data()};
  Sample32 *outputChannels[]{outputLeft.data(), outputRight.data()};
  AudioBusBuffers input{};
  input.numChannels = 2;
  input.channelBuffers32 = inputChannels;
  AudioBusBuffers output{};
  output.numChannels = 2;
  output.channelBuffers32 = outputChannels;
  ProcessData data{};
  data.symbolicSampleSize = kSample32;
  data.numSamples = 64;
  data.numInputs = 1;
  data.numOutputs = 1;
  data.inputs = &input;
  data.outputs = &output;

  // Only one processor is alive at a time: the WebView host registers a
  // process-wide window class, so two live instances cannot coexist.
  const auto renderRun = [&](const bool holdControlGuard) {
    auto processor = std::make_unique<EffeTuneProcessor>();
    expect(processor->initialize(nullptr) == kResultOk,
           "initialize held control-guard test");
    auto processSetup = setup(48000.0, 64);
    expect(processor->setupProcessing(processSetup) == kResultOk,
           "prepare held control-guard test");
    installLimiterPipeline(*processor, 33);
    expect(processor->getLatencySamples() > 0,
           "held control-guard test has non-zero native latency");
    expect(processor->setActive(true) == kResultOk,
           "activate held control-guard test");

    std::vector<float> rendered;
    rendered.reserve(static_cast<std::size_t>(totalBlocks) * inputLeft.size() * 2u);
    std::optional<HeldControlGuard> guard;
    std::uint32_t sourceFrame = 0;
    for (int block = 0; block < totalBlocks; ++block) {
      if (holdControlGuard && block == guardedFromBlock) {
        guard.emplace(*processor);
      }
      if (holdControlGuard && block == guardedThroughBlock) {
        guard.reset();
      }
      for (std::size_t frame = 0; frame < inputLeft.size(); ++frame) {
        const auto sample = static_cast<float>(sourceFrame++) * 0.00025f;
        inputLeft[frame] = sample;
        inputRight[frame] = -sample;
      }
      expect(processor->process(data) == kResultOk,
             "render a held control-guard block");
      if (block == guardedThroughBlock - 1) {
        // A dry fallback puts the undelayed input on the output. This pipeline
        // reports latency, so a processed block cannot coincide with it.
        expect(std::abs(outputLeft.back() - inputLeft.back()) > 1.0e-3f,
               "the block stays on the latency-delayed processed timeline");
      }
      rendered.insert(rendered.end(), outputLeft.begin(), outputLeft.end());
      rendered.insert(rendered.end(), outputRight.begin(), outputRight.end());
    }
    expect(hostInfo(*processor)["diagnostics"].size() == 0,
           "the rendered run raises no deferred diagnostic");
    expect(processor->setActive(false) == kResultOk,
           "deactivate held control-guard test");
    expect(processor->terminate() == kResultOk,
           "terminate held control-guard test");
    return rendered;
  };

  const auto uncontended = renderRun(false);
  const auto contended = renderRun(true);
  expect(contended.size() == uncontended.size(),
         "both held control-guard runs render the same frame count");
  for (std::size_t sample = 0; sample < uncontended.size(); ++sample) {
    expect(contended[sample] == uncontended[sample],
           "a held control guard leaves the processed output unchanged");
  }
}

void testProcessingSurvivesHostReconfiguration() {
  auto processor = std::make_unique<EffeTuneProcessor>();
  auto initialSetup = setup(44100.0, 128);
  expect(processor->setupProcessing(initialSetup) == kResultOk,
         "configure initial processing");
  installGainPipeline(*processor);
  expect(processor->setActive(true) == kResultOk, "activate initial processing");
  expectGainProcessing(*processor, 128);

  const auto before = hostInfo(*processor);
  const auto generation =
      before["contextGeneration"].getWithDefault<std::int64_t>(0);
  expect(generation > 1,
         "the first host setup publishes context even at the default sample rate");
  expect(before["dspReady"].getWithDefault<bool>(false),
         "initial native pipeline is ready");
  expect(before["latencySamples"].getWithDefault<std::int64_t>(-1) ==
             static_cast<std::int64_t>(processor->getLatencySamples()) &&
             before["pipelineCpuAverage"].getWithDefault<double>(-1.0) >= 0.0,
         "host info publishes the VST latency and CPU status");

  expect(processor->setActive(false) == kResultOk, "deactivate before block-size change");
  auto blockSizeSetup = setup(44100.0, 256);
  expect(processor->setupProcessing(blockSizeSetup) == kResultOk,
         "reconfigure the host block size");
  expect(processor->setActive(true) == kResultOk, "reactivate after block-size change");
  const auto afterBlockSize = hostInfo(*processor);
  expect(afterBlockSize["dspReady"].getWithDefault<bool>(false),
         "block-size reconfiguration keeps the native pipeline ready");
  expect(afterBlockSize["contextGeneration"].getWithDefault<std::int64_t>(0) == generation,
         "block-size-only reconfiguration does not request a UI rebuild");
  expectGainProcessing(*processor, 256);

  expect(processor->setActive(false) == kResultOk, "deactivate before sample-rate change");
  auto sampleRateSetup = setup(96000.0, 256);
  expect(processor->setupProcessing(sampleRateSetup) == kResultOk,
         "reconfigure the host sample rate");
  expect(processor->setActive(true) == kResultOk, "reactivate after sample-rate change");
  const auto afterSampleRate = hostInfo(*processor);
  expect(afterSampleRate["dspReady"].getWithDefault<bool>(false),
         "sample-rate reconfiguration keeps the native pipeline ready");
  expect(afterSampleRate["contextGeneration"].getWithDefault<std::int64_t>(0) ==
             generation + 1,
         "sample-rate reconfiguration requests one UI synchronization");
  expectGainProcessing(*processor, 256);
  for (int block = 0; block < 375; ++block) {
    expectGainProcessing(*processor, 256);
  }
  const auto measured = hostInfo(*processor);
  expect(measured["pipelineCpuAverage"].getWithDefault<double>(0.0) > 0.0,
         "one second of audio publishes a positive callback CPU average");
  const auto telemetry = choc::json::parse(processor->handleUiMessage(
      R"({"type":"telemetry/read","payload":{}})"));
  expect(telemetry["latencySamples"].getWithDefault<std::int64_t>(-1) ==
             static_cast<std::int64_t>(processor->getLatencySamples()) &&
             telemetry["pipelineCpuAverage"].getWithDefault<double>(0.0) > 0.0,
         "telemetry polling carries current latency and CPU status");
  expect(processor->setActive(false) == kResultOk, "deactivate final processing");
}

void testStoppedTransportFallbackAndDiscontinuities() {
  auto processor = std::make_unique<EffeTuneProcessor>();
  auto processSetup = setup(48000.0, 64);
  expect(processor->setupProcessing(processSetup) == kResultOk,
         "prepare transport fallback test");

  ProcessContext context{};
  context.projectTimeSamples = 100;
  ProcessData data{};
  data.numSamples = 64;
  data.processContext = &context;
  bool rebase = false;
  PluginProcessorTestAccess::setAutomationFallbackClock(*processor, 0);
  expect(PluginProcessorTestAccess::automationBlockStart(*processor, data, rebase) == 0 &&
             !rebase,
         "stopped transport starts on the rendered-frame fallback clock");

  PluginProcessorTestAccess::setAutomationFallbackClock(*processor, 64);
  rebase = false;
  expect(PluginProcessorTestAccess::automationBlockStart(*processor, data, rebase) == 64 &&
             !rebase,
         "unchanged stopped project position is not a discontinuity");

  PluginProcessorTestAccess::setAutomationFallbackClock(*processor, 128);
  context.state = ProcessContext::kPlaying;
  rebase = false;
  (void)PluginProcessorTestAccess::automationBlockStart(*processor, data, rebase);
  expect(rebase, "play start rebases the rendered automation clock once");

  PluginProcessorTestAccess::setAutomationFallbackClock(*processor, 192);
  context.projectTimeSamples = 164;
  rebase = false;
  (void)PluginProcessorTestAccess::automationBlockStart(*processor, data, rebase);
  expect(!rebase, "continuous playback does not repeatedly rebase");

  PluginProcessorTestAccess::setAutomationFallbackClock(*processor, 256);
  context.projectTimeSamples = 400;
  rebase = false;
  (void)PluginProcessorTestAccess::automationBlockStart(*processor, data, rebase);
  expect(rebase, "playback seek rebases the rendered automation clock");

  PluginProcessorTestAccess::setAutomationFallbackClock(*processor, 320);
  context.projectTimeSamples = 464;
  context.state = ProcessContext::kPlaying | ProcessContext::kCycleActive;
  rebase = false;
  (void)PluginProcessorTestAccess::automationBlockStart(*processor, data, rebase);
  expect(rebase, "cycle activation rebases the rendered automation clock once");
}

// The control service is not paced by the host block period. It runs from a
// 50 ms timer and from every editor poll, while a large host buffer produces a
// block only every 21 ms, so "the epoch has not moved since my previous call"
// is true during ordinary playback. Reading that as an idle transport lets the
// service take the engine over, and every one of those hand-overs costs a
// block. Servicing faster than blocks arrive must therefore change nothing
// about the audio.
void testUiPacedControlServiceKeepsEveryBlockWet() {
  auto processor = std::make_unique<EffeTuneProcessor>();
  expect(processor->initialize(nullptr) == kResultOk,
         "initialize the UI-paced service test");
  auto processSetup = setup(48000.0, 64);
  expect(processor->setupProcessing(processSetup) == kResultOk,
         "prepare the UI-paced service test");
  const auto installed = choc::json::parse(processor->handleUiMessage(
      R"({"type":"pipeline/rebuild","payload":{"pipeline":"A","plugins":[)"
      R"({"id":73,"type":"DCOffsetPlugin","name":"DC Offset","enabled":true,)"
      R"("parameters":{"of":0.25},"wasmParams":[0.25],)"
      R"("wasmParamsHash":1104945464}]}})"));
  expect(installed["ok"].getWithDefault<bool>(false),
         "install the UI-paced service pipeline");
  expect(processor->setActive(true) == kResultOk,
         "activate the UI-paced service test");

  std::array<float, 64> inputLeft{};
  std::array<float, 64> inputRight{};
  std::array<float, 64> outputLeft{};
  std::array<float, 64> outputRight{};
  inputLeft.fill(1.0f);
  inputRight.fill(1.0f);
  Sample32 *inputChannels[]{inputLeft.data(), inputRight.data()};
  Sample32 *outputChannels[]{outputLeft.data(), outputRight.data()};
  AudioBusBuffers input{};
  input.numChannels = 2;
  input.channelBuffers32 = inputChannels;
  AudioBusBuffers output{};
  output.numChannels = 2;
  output.channelBuffers32 = outputChannels;
  ProcessData data{};
  data.symbolicSampleSize = kSample32;
  data.numSamples = 64;
  data.numInputs = 1;
  data.numOutputs = 1;
  data.inputs = &input;
  data.outputs = &output;

  expect(processor->process(data) == kResultOk, "warm the UI-paced service test");
  expect(std::abs(outputLeft[0] - 1.25f) < 1.0e-6f,
         "the warm-up block is wet at the installed offset");
  // The first sample is taken while blocks are known to be flowing, exactly as
  // the timer would take it in a running host.
  PluginProcessorTestAccess::serviceLatencyUpdates(*processor);

  const auto before = PluginProcessorTestAccess::processCounters(*processor);
  const auto failuresBefore =
      PluginProcessorTestAccess::processFailureSequence(*processor);
  std::atomic_bool running{true};
  std::atomic_bool published{true};
  std::atomic<std::uint64_t> serviceTicks{0};
  // Built once: the Debug allocation guard is process-wide, so this thread must
  // not reach the heap while a block is in the engine.
  auto command = std::make_unique<effetune::vst::AudioCommand>();
  std::thread control([&] {
    std::uint32_t step = 0;
    while (running.load(std::memory_order_acquire)) {
      const auto offset = 0.25f + static_cast<float>(step++ % 8u) * 0.03125f;
      if (!PluginProcessorTestAccess::publishParameterImage(
              *processor, *command, 73u, 1104945464u, offset)) {
        published.store(false, std::memory_order_release);
        return;
      }
      // Two editor polls and one timer tick per iteration, all of them free to
      // run several times between two host blocks.
      PluginProcessorTestAccess::pollLatencyUpdatesFromUi(*processor);
      PluginProcessorTestAccess::pollLatencyUpdatesFromUi(*processor);
      PluginProcessorTestAccess::serviceLatencyUpdates(*processor);
      serviceTicks.fetch_add(1, std::memory_order_acq_rel);
    }
  });

  constexpr std::uint64_t blocks = 1500;
  auto wetBlocks = std::uint64_t{0};
  for (std::uint64_t block = 0; block < blocks; ++block) {
    outputLeft.fill(0.0f);
    outputRight.fill(0.0f);
    expect(processor->process(data) == kResultOk,
           "process a block while the service runs faster than blocks arrive");
    wetBlocks += outputLeft[0] > 1.0f + 1.0e-4f ? 1u : 0u;
  }
  running.store(false, std::memory_order_release);
  control.join();

  const auto after = PluginProcessorTestAccess::processCounters(*processor);
  expect(published.load(std::memory_order_acquire),
         "every parameter image reached the mailbox");
  expect(serviceTicks.load(std::memory_order_acquire) != 0,
         "the control service really ran alongside the audio callback");
  expect(after.batchAttempts - before.batchAttempts == blocks &&
             after.completedBatches - before.completedBatches == blocks,
         "every block entered and completed inside the engine");
  expect(wetBlocks == blocks,
         "a service that runs faster than the block period never turns a block dry");
  expect(PluginProcessorTestAccess::processFailureSequence(*processor) ==
             failuresBefore,
         "a service that runs faster than the block period raises no diagnostic");

  expect(processor->setActive(false) == kResultOk,
         "deactivate the UI-paced service test");
  expect(processor->terminate() == kResultOk,
         "terminate the UI-paced service test");
}

// The dirty flags belong to whoever owns the runtime image. A block that fails
// while the control service owns it staged nothing, so it has nothing to
// re-stage: writing the flags there would both race the owner and undo the
// bookkeeping the owner just did.
void testOwnedRuntimeImageKeepsItsDirtyFlagsThroughAFailedBlock() {
  auto processor = std::make_unique<EffeTuneProcessor>();
  expect(processor->initialize(nullptr) == kResultOk,
         "initialize the failed-block dirty-flag test");
  auto processSetup = setup(48000.0, 64);
  expect(processor->setupProcessing(processSetup) == kResultOk,
         "prepare the failed-block dirty-flag test");
  const auto installed = choc::json::parse(processor->handleUiMessage(
      R"({"type":"pipeline/rebuild","payload":{"pipeline":"A","plugins":[)"
      R"({"id":87,"type":"DCOffsetPlugin","name":"DC Offset","enabled":true,)"
      R"("parameters":{"of":0.25},"wasmParams":[0.25],)"
      R"("wasmParamsHash":1104945464}]}})"));
  expect(installed["ok"].getWithDefault<bool>(false),
         "install the failed-block dirty-flag pipeline");
  expect(processor->setActive(true) == kResultOk,
         "activate the failed-block dirty-flag test");

  std::array<float, 64> inputLeft{};
  std::array<float, 64> inputRight{};
  std::array<float, 64> outputLeft{};
  std::array<float, 64> outputRight{};
  inputLeft.fill(1.0f);
  inputRight.fill(1.0f);
  Sample32 *inputChannels[]{inputLeft.data(), inputRight.data()};
  Sample32 *outputChannels[]{outputLeft.data(), outputRight.data()};
  AudioBusBuffers input{};
  input.numChannels = 2;
  input.channelBuffers32 = inputChannels;
  AudioBusBuffers output{};
  output.numChannels = 2;
  output.channelBuffers32 = outputChannels;
  ProcessData data{};
  data.symbolicSampleSize = kSample32;
  data.numSamples = 64;
  data.numInputs = 1;
  data.numOutputs = 1;
  data.inputs = &input;
  data.outputs = &output;

  expect(processor->process(data) == kResultOk,
         "warm the failed-block dirty-flag test");
  expect(std::abs(outputLeft[0] - 1.25f) < 1.0e-6f,
         "the warm-up block is wet at the installed offset");
  expect(!PluginProcessorTestAccess::runtimeParameterDirty(*processor, 87),
         "a completed block leaves no image outstanding");

  PluginProcessorTestAccess::setControlOwnsRuntimeImage(*processor, true);
  const auto paramsHash =
      PluginProcessorTestAccess::swapRuntimeParamsHash(*processor, 87, 0xdeadbeefu);
  expect(processor->process(data) == kResultOk,
         "process a block the engine rejects while the image is owned");
  expect(!PluginProcessorTestAccess::runtimeParameterDirty(*processor, 87),
         "a failed block never dirties an image it does not own");
  (void)PluginProcessorTestAccess::swapRuntimeParamsHash(*processor, 87, paramsHash);
  PluginProcessorTestAccess::setControlOwnsRuntimeImage(*processor, false);

  for (int32 block = 0; block < 16; ++block) {
    outputLeft.fill(0.0f);
    outputRight.fill(0.0f);
    expect(processor->process(data) == kResultOk,
           "resume processing after the rejected block");
  }
  expect(std::abs(outputLeft.back() - 1.25f) < 1.0e-6f,
         "the released image resumes the wet output");

  expect(processor->setActive(false) == kResultOk,
         "deactivate the failed-block dirty-flag test");
  expect(processor->terminate() == kResultOk,
         "terminate the failed-block dirty-flag test");
}

// The UI polls the asset state every frame while a plug-in prepares one. That
// read must never stop the DSP, so it reads the projection the engine
// publishes instead of the kernel, and it takes nothing a control thread can
// be holding.
void testAssetStatePollLeavesTheDspRunning() {
  auto processor = std::make_unique<EffeTuneProcessor>();
  expect(processor->initialize(nullptr) == kResultOk,
         "initialize the asset-state poll test");
  auto processSetup = setup(48000.0, 64);
  expect(processor->setupProcessing(processSetup) == kResultOk,
         "prepare the asset-state poll test");
  installIrReverbPipeline(*processor);
  expect(processor->setActive(true) == kResultOk,
         "activate the asset-state poll test");
  std::string error;
  expect(PluginProcessorTestAccess::setAsset(*processor, makeIrReverbAsset(), &error),
         "stage the polled IR asset: " + error);

  // Holding the control guard stands in for an edit that is in progress. A
  // poll that has to wait for it is a poll that would also have to stop audio.
  const auto pollAssetState = [&](const std::string &message) {
    auto guard = PluginProcessorTestAccess::lockProcessingResources(*processor);
    std::atomic_bool completed{false};
    std::string response;
    std::thread poll([&] {
      response = processor->handleUiMessage(
          R"({"type":"pipeline/assetState","payload":{"pluginId":91,"slot":0}})");
      completed.store(true, std::memory_order_release);
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!completed.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    const auto completedWhileHeld = completed.load(std::memory_order_acquire);
    guard.unlock();
    poll.join();
    expect(completedWhileHeld, message);
    return static_cast<std::uint32_t>(
        choc::json::parse(response)["state"].getWithDefault<std::int64_t>(0));
  };

  expect((pollAssetState("a staged asset-state poll never waits on control work") &
          0xffu) == ET_ASSET_STATE_PREPARING,
         "the published projection reports the staged asset as preparing");

  std::array<float, 64> inputLeft{};
  std::array<float, 64> inputRight{};
  std::array<float, 64> outputLeft{};
  std::array<float, 64> outputRight{};
  Sample32 *inputChannels[]{inputLeft.data(), inputRight.data()};
  Sample32 *outputChannels[]{outputLeft.data(), outputRight.data()};
  AudioBusBuffers input{};
  input.numChannels = 2;
  input.channelBuffers32 = inputChannels;
  AudioBusBuffers output{};
  output.numChannels = 2;
  output.channelBuffers32 = outputChannels;
  ProcessData data{};
  data.symbolicSampleSize = kSample32;
  data.numSamples = 64;
  data.numInputs = 1;
  data.numOutputs = 1;
  data.inputs = &input;
  data.outputs = &output;
  for (std::uint32_t block = 0;
       block < 512u &&
       (PluginProcessorTestAccess::assetState(*processor, 91, 0) & 0xffu) ==
           ET_ASSET_STATE_PREPARING;
       ++block) {
    expect(processor->process(data) == kResultOk,
           "prepare the polled IR asset on the audio path");
  }
  expect((pollAssetState("a prepared asset-state poll never waits on control work") &
          0xffu) == ET_ASSET_STATE_ACTIVE,
         "the block that finished the preparation published the active state");

  // The rebuild drops the plug-in, and the eviction it performs inside the
  // region it has already made not ready still reaches the engine cache.
  installGainPipeline(*processor);
  expect((pollAssetState("an evicted asset-state poll never waits on control work") &
          0xffu) == ET_ASSET_STATE_NONE,
         "a rebuild without the plug-in evicts its asset");

  expect(processor->setActive(false) == kResultOk,
         "deactivate the asset-state poll test");
  expect(processor->terminate() == kResultOk,
         "terminate the asset-state poll test");
}

// A cleared processing flag says that somebody intends to stop the audio
// thread, not that it has stopped: the thread that cleared it is still waiting
// the block out, and a block that passed the gate before it was cleared is
// walking the runtime image. Re-seating that image can reallocate it, so this
// path has to run its own wait rather than trust the flag.
void testNotReadyPluginUpdateWaitsForTheInFlightBlock() {
  auto processor = std::make_unique<EffeTuneProcessor>();
  expect(processor->initialize(nullptr) == kResultOk,
         "initialize the not-ready update test");
  auto processSetup = setup(48000.0, 64);
  expect(processor->setupProcessing(processSetup) == kResultOk,
         "prepare the not-ready update test");
  expect(!hostInfo(*processor)["dspReady"].getWithDefault<bool>(true),
         "a processor without a pipeline is not ready to process");

  PluginProcessorTestAccess::beginSyntheticBlock(*processor);
  std::atomic_bool completed{false};
  std::atomic_bool accepted{false};
  std::thread update([&] {
    const auto response = choc::json::parse(processor->handleUiMessage(
        R"({"type":"pipeline/updatePlugin","payload":{"pipeline":"A","plugin":)"
        R"({"id":77,"type":"DCOffsetPlugin","name":"DC Offset","enabled":true,)"
        R"("parameters":{"of":0.25},"wasmParams":[0.25],)"
        R"("wasmParamsHash":1104945464}}})"));
    accepted.store(response["ok"].getWithDefault<bool>(false),
                   std::memory_order_release);
    completed.store(true, std::memory_order_release);
  });
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
  while (!completed.load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  const auto completedDuringTheBlock = completed.load(std::memory_order_acquire);
  PluginProcessorTestAccess::endSyntheticBlock(*processor);
  update.join();
  expect(!completedDuringTheBlock,
         "re-seating the runtime image waits for the in-flight block to leave");
  expect(accepted.load(std::memory_order_acquire),
         "the update is accepted once the block has left");
  expect(std::abs(PluginProcessorTestAccess::runtimePackedParameter(*processor, 77, 0) -
                  0.25f) < 1.0e-6f,
         "the update reached the runtime image");
  expect(processor->terminate() == kResultOk,
         "terminate the not-ready update test");
}

// Resetting the scheduler and the output transition is not covered by the
// processing gate: a block reaches both before it reaches the engine. While a
// control thread owns that timeline the block must leave the buffers alone,
// and it must still advertise itself through the epoch so the owner can wait
// it out.
void testOwnedAudioTimelineLeavesTheBlockUntouched() {
  auto processor = std::make_unique<EffeTuneProcessor>();
  expect(processor->initialize(nullptr) == kResultOk,
         "initialize the owned-timeline test");
  auto processSetup = setup(48000.0, 64);
  expect(processor->setupProcessing(processSetup) == kResultOk,
         "prepare the owned-timeline test");
  const auto installed = choc::json::parse(processor->handleUiMessage(
      R"({"type":"pipeline/rebuild","payload":{"pipeline":"A","plugins":[)"
      R"({"id":79,"type":"DCOffsetPlugin","name":"DC Offset","enabled":true,)"
      R"("parameters":{"of":0},"wasmParams":[0],)"
      R"("wasmParamsHash":1104945464}]}})"));
  expect(installed["ok"].getWithDefault<bool>(false),
         "install the owned-timeline pipeline");
  const effetune::vst::AutomationTargetIdentity identity{
      'A', 79, "DCOffsetPlugin", "of", 0};
  const auto slot = PluginProcessorTestAccess::bindAutomationSlot(*processor, identity);
  expect(slot.has_value(), "bind the owned-timeline automation target");
  const auto parameterId = effetune::vst::plugin::automationParameterId(*slot);
  expect(processor->setActive(true) == kResultOk,
         "activate the owned-timeline test");

  std::array<float, 64> inputLeft{};
  std::array<float, 64> inputRight{};
  std::array<float, 64> outputLeft{};
  std::array<float, 64> outputRight{};
  inputLeft.fill(1.0f);
  inputRight.fill(1.0f);
  Sample32 *inputChannels[]{inputLeft.data(), inputRight.data()};
  Sample32 *outputChannels[]{outputLeft.data(), outputRight.data()};
  AudioBusBuffers input{};
  input.numChannels = 2;
  input.channelBuffers32 = inputChannels;
  AudioBusBuffers output{};
  output.numChannels = 2;
  output.channelBuffers32 = outputChannels;
  ProcessData data{};
  data.symbolicSampleSize = kSample32;
  data.numSamples = 64;
  data.numInputs = 1;
  data.numOutputs = 1;
  data.inputs = &input;
  data.outputs = &output;

  ParameterChanges established(1);
  int32 queueIndex = 0;
  auto *queue = established.addParameterData(parameterId, queueIndex);
  int32 pointIndex = 0;
  expect(queue != nullptr && queue->addPoint(0, 0.75, pointIndex) == kResultTrue,
         "establish the played automation value");
  data.inputParameterChanges = &established;
  expect(processor->process(data) == kResultOk,
         "play the established automation value");
  const auto played = PluginProcessorTestAccess::playedAutomationValue(*processor, *slot);
  expect(std::abs(played - 0.75) < 1.0e-9, "the block adopted the automation value");

  ParameterChanges ignored(1);
  queueIndex = 0;
  queue = ignored.addParameterData(parameterId, queueIndex);
  pointIndex = 0;
  expect(queue != nullptr && queue->addPoint(0, 0.1, pointIndex) == kResultTrue,
         "queue automation the owned timeline must not take");
  data.inputParameterChanges = &ignored;
  PluginProcessorTestAccess::setControlOwnsAudioTimeline(*processor, true);
  const auto epochBefore = PluginProcessorTestAccess::blockEpoch(*processor);
  outputLeft.fill(0.0f);
  outputRight.fill(0.0f);
  expect(processor->process(data) == kResultOk,
         "process a block while the control thread owns the timeline");
  for (const auto sample : outputLeft) {
    expect(std::abs(sample - 1.0f) < 1.0e-6f,
           "an owned timeline passes the input through untouched");
  }
  expect(std::abs(PluginProcessorTestAccess::playedAutomationValue(*processor, *slot) -
                  played) < 1.0e-9,
         "an owned timeline leaves the scheduler alone");
  expect(PluginProcessorTestAccess::blockEpoch(*processor) > epochBefore,
         "the skipped block still advertises itself through the block epoch");

  PluginProcessorTestAccess::setControlOwnsAudioTimeline(*processor, false);
  data.inputParameterChanges = nullptr;
  outputLeft.fill(0.0f);
  outputRight.fill(0.0f);
  expect(processor->process(data) == kResultOk,
         "process a block after the control thread releases the timeline");
  for (const auto sample : outputLeft) {
    expect(std::abs(sample - 1.5f) < 1.0e-6f,
           "the released timeline resumes the value the scheduler kept");
  }

  expect(processor->setActive(false) == kResultOk,
         "deactivate the owned-timeline test");
  expect(processor->terminate() == kResultOk,
         "terminate the owned-timeline test");
}

// The audio thread publishes what automation is playing every block, and the
// drain picks that up whenever a control thread next runs. A gesture that
// arrives in between is newer than everything already published, so the drain
// must not replay the automated value over it -- and the overlay that writes
// the state document must see the adopted value, not the replayed one. This is
// the preset-load and undo regression: it only needs one undrained block, which
// ordinary playback produces continuously.
void testNamedAutomationEditSurvivesAnUndrainedPublish() {
  auto processor = std::make_unique<EffeTuneProcessor>();
  expect(processor->initialize(nullptr) == kResultOk,
         "initialize the undrained-publish test");
  auto *handler = new TestComponentHandler();
  expect(processor->setComponentHandler(handler) == kResultOk,
         "install the undrained-publish handler");
  auto processSetup = setup(48000.0, 64);
  expect(processor->setupProcessing(processSetup) == kResultOk,
         "prepare the undrained-publish test");
  const auto installed = choc::json::parse(processor->handleUiMessage(
      R"({"type":"pipeline/rebuild","payload":{"pipeline":"A","plugins":[)"
      R"({"id":83,"type":"DCOffsetPlugin","name":"DC Offset","enabled":true,)"
      R"("parameters":{"of":0},"wasmParams":[0],)"
      R"("wasmParamsHash":1104945464}]}})"));
  expect(installed["ok"].getWithDefault<bool>(false),
         "install the undrained-publish pipeline");
  const effetune::vst::AutomationTargetIdentity identity{
      'A', 83, "DCOffsetPlugin", "of", 0};
  const auto parameterId = boundAutomationParameterId(*processor, identity);
  expect(processor->setActive(true) == kResultOk,
         "activate the undrained-publish test");

  // A flush-only block is the cheapest published block: it takes the host queue
  // and publishes the played value without entering the engine.
  ParameterChanges playing(1);
  int32 queueIndex = 0;
  auto *queue = playing.addParameterData(parameterId, queueIndex);
  int32 pointIndex = 0;
  expect(queue != nullptr && queue->addPoint(0, 0.75, pointIndex) == kResultTrue,
         "publish the value automation is playing");
  ProcessData flush{};
  flush.symbolicSampleSize = kSample32;
  flush.numSamples = 0;
  flush.inputParameterChanges = &playing;
  expect(processor->process(flush) == kResultOk,
         "leave one published automation value undrained");

  expect(PluginProcessorTestAccess::applyAutomationEdit(*processor, identity, 0.25),
         "adopt the value the payload names on the bound target");
  PluginProcessorTestAccess::drainAutomationValues(*processor);

  // The overlay is the reader that decides what the state document keeps, so
  // rebuilding with a stale image is what makes the registry value visible.
  const auto rebuilt = choc::json::parse(processor->handleUiMessage(
      R"({"type":"pipeline/rebuild","payload":{"pipeline":"A","plugins":[)"
      R"({"id":83,"type":"DCOffsetPlugin","name":"DC Offset","enabled":true,)"
      R"("parameters":{"of":0.9},"wasmParams":[0.9],)"
      R"("wasmParamsHash":1104945464}]}})"));
  expect(rebuilt["ok"].getWithDefault<bool>(false),
         "rebuild over the adopted automation value");
  const auto delta = findAutomationDelta(rebuilt, identity);
  expect(!delta.isVoid() &&
             std::abs(delta["normalized"].getWithDefault<double>(0.0) - 0.25) < 1.0e-12,
         "the drain does not replay the played value over the adopted one");
  const auto stored = choc::json::parse(processor->handleUiMessage(
      R"({"type":"storage/readFile","payload":{"path":"pipeline-state.json"}})"));
  const auto persisted =
      choc::json::parse(stored["content"].getWithDefault<std::string>({}));
  expect(std::abs(persisted["pipelineA"][0]["parameters"]["of"].getWithDefault<double>(0.0) +
                  0.5) < 1.0e-6,
         "the overlay writes the adopted value into the state document");

  expect(processor->setActive(false) == kResultOk,
         "deactivate the undrained-publish test");
  expect(processor->setComponentHandler(nullptr) == kResultOk,
         "remove the undrained-publish handler");
  handler->release();
  expect(processor->terminate() == kResultOk,
         "terminate the undrained-publish test");
}

// The same transaction under concurrency: a drain running on another control
// thread may claim the published generations, but it must never land between
// the adoption and the overlay that reads its result.
void testNamedAutomationEditSurvivesAConcurrentDrain() {
  auto processor = std::make_unique<EffeTuneProcessor>();
  expect(processor->initialize(nullptr) == kResultOk,
         "initialize the named-edit drain race test");
  auto processSetup = setup(48000.0, 64);
  expect(processor->setupProcessing(processSetup) == kResultOk,
         "prepare the named-edit drain race test");
  const auto installed = choc::json::parse(processor->handleUiMessage(
      R"({"type":"pipeline/rebuild","payload":{"pipeline":"A","plugins":[)"
      R"({"id":83,"type":"DCOffsetPlugin","name":"DC Offset","enabled":true,)"
      R"("parameters":{"of":0},"wasmParams":[0],)"
      R"("wasmParamsHash":1104945464}]}})"));
  expect(installed["ok"].getWithDefault<bool>(false),
         "install the named-edit drain race pipeline");
  const effetune::vst::AutomationTargetIdentity identity{
      'A', 83, "DCOffsetPlugin", "of", 0};
  const auto parameterId = boundAutomationParameterId(*processor, identity);
  expect(processor->setActive(true) == kResultOk,
         "activate the named-edit drain race test");

  // A flush-only block is the cheapest way to publish what automation is
  // playing: it takes the host queue and publishes the value without entering
  // the engine, so the control threads below stay free to allocate.
  ParameterChanges playing(1);
  int32 queueIndex = 0;
  auto *queue = playing.addParameterData(parameterId, queueIndex);
  int32 pointIndex = 0;
  expect(queue != nullptr && queue->addPoint(0, 0.75, pointIndex) == kResultTrue,
         "publish the value automation is playing");
  ProcessData flush{};
  flush.symbolicSampleSize = kSample32;
  flush.numSamples = 0;
  flush.inputParameterChanges = &playing;

  constexpr const char *namedRebuild =
      R"({"type":"pipeline/rebuild","payload":{"pipeline":"A","plugins":[)"
      R"({"id":83,"type":"DCOffsetPlugin","name":"DC Offset","enabled":true,)"
      R"("parameters":{"of":-0.5},"wasmParams":[-0.5],)"
      R"("wasmParamsHash":1104945464}],"automationEdits":[{"pipeline":"A",)"
      R"("pluginId":83,"pluginType":"DCOffsetPlugin","parameterKey":"of",)"
      R"("elementIndex":0,"normalized":0.25}]}})";

  std::mutex drainMutex;
  std::atomic_bool running{true};
  std::atomic_bool draining{true};
  std::thread drain([&] {
    while (running.load(std::memory_order_acquire)) {
      {
        std::scoped_lock lock(drainMutex);
        if (draining.load(std::memory_order_acquire)) {
          PluginProcessorTestAccess::drainAutomationValues(*processor);
        }
      }
      std::this_thread::yield();
    }
  });

  for (int iteration = 0; iteration < 400; ++iteration) {
    // Leave one published value undrained, which is what the transaction has
    // to supersede.
    expect(processor->process(flush) == kResultOk,
           "publish an undrained automation value");
    const auto rebuilt = choc::json::parse(processor->handleUiMessage(namedRebuild));
    expect(rebuilt["ok"].getWithDefault<bool>(false),
           "rebuild the pipeline with a named automation edit");
    std::scoped_lock lock(drainMutex);
    draining.store(false, std::memory_order_release);
    const auto stored = choc::json::parse(processor->handleUiMessage(
        R"({"type":"storage/readFile","payload":{"path":"pipeline-state.json"}})"));
    const auto persisted =
        choc::json::parse(stored["content"].getWithDefault<std::string>({}));
    const auto offset =
        persisted["pipelineA"][0]["parameters"]["of"].getWithDefault<double>(0.0);
    expect(std::abs(offset + 0.5) < 1.0e-6,
           "the named value survives the drain (iteration " +
               std::to_string(iteration) + " kept " + std::to_string(offset) + ")");
    draining.store(true, std::memory_order_release);
  }
  running.store(false, std::memory_order_release);
  drain.join();

  expect(processor->setActive(false) == kResultOk,
         "deactivate the named-edit drain race test");
  expect(processor->terminate() == kResultOk,
         "terminate the named-edit drain race test");
}

// The UI rolls a refused update back without republishing it, so a refusal
// that had already written the state document would strand the native
// document ahead of both the editor and the host.
void testRefusedPluginUpdateLeavesTheStateDocumentUnchanged() {
  auto processor = std::make_unique<EffeTuneProcessor>();
  expect(processor->initialize(nullptr) == kResultOk,
         "initialize the refused update test");
  auto processSetup = setup(48000.0, 64);
  expect(processor->setupProcessing(processSetup) == kResultOk,
         "prepare the refused update test");
  std::string plugins;
  for (std::uint32_t id = 1; id <= 96u; ++id) {
    plugins += plugins.empty() ? "" : ",";
    plugins += std::string{R"({"id":)"} + std::to_string(id) +
               R"(,"type":"DCOffsetPlugin","name":"DC Offset","enabled":true,)"
               R"("parameters":{"of":0},"wasmParams":[0],)"
               R"("wasmParamsHash":1104945464})";
  }
  const auto installed = choc::json::parse(processor->handleUiMessage(
      std::string{R"({"type":"pipeline/rebuild","payload":{"pipeline":"A","plugins":[)"} +
      plugins + "]}}"));
  expect(installed["ok"].getWithDefault<bool>(false),
         "install a pipeline at the native instance limit");
  const auto storedPipelineSize = [&] {
    const auto stored = choc::json::parse(processor->handleUiMessage(
        R"({"type":"storage/readFile","payload":{"path":"pipeline-state.json"}})"));
    const auto persisted =
        choc::json::parse(stored["content"].getWithDefault<std::string>({}));
    for (const auto plugin : persisted["pipelineA"]) {
      expect(plugin["id"].getWithDefault<std::int64_t>(0) != 97,
             "a refused update never leaves its plug-in in the state document");
    }
    return persisted["pipelineA"].size();
  };
  const auto sizeBeforeRefusal = storedPipelineSize();

  const auto refused = choc::json::parse(processor->handleUiMessage(
      R"({"type":"pipeline/updatePlugin","payload":{"pipeline":"A","plugin":)"
      R"({"id":97,"type":"DCOffsetPlugin","name":"DC Offset","enabled":true,)"
      R"("parameters":{"of":0.5},"wasmParams":[0.5],)"
      R"("wasmParamsHash":1104945464}}})"));
  expect(!refused["ok"].getWithDefault<bool>(true),
         "the update past the native instance limit is refused");

  expect(storedPipelineSize() == sizeBeforeRefusal,
         "a refused update leaves the state document at its previous size");
  auto runtimeImageChanged = true;
  try {
    (void)PluginProcessorTestAccess::runtimePackedParameter(*processor, 97, 0);
  } catch (const std::runtime_error &) {
    runtimeImageChanged = false;
  }
  expect(!runtimeImageChanged,
         "a refused update never leaves its plug-in in the runtime image");
  expect(processor->terminate() == kResultOk,
         "terminate the refused update test");
}

// Dragging a delay-bearing control republishes an instance latency from every
// block. Compensation preparation must run alongside those blocks and apply
// without taking the engine away from the audio thread. The service is reached
// here through the editor's poll alone: the control-service timer lives on a
// message loop a second instance can take
// over, so a service that only sampled idleness from the timer would read a
// playing transport as stopped and open a window on every tick.
void testDelayBearingEditsDuringPlaybackKeepEveryBlockWet() {
  auto processor = std::make_unique<EffeTuneProcessor>();
  expect(processor->initialize(nullptr) == kResultOk,
         "initialize the delay-bearing drag test");
  auto processSetup = setup(48000.0, 64);
  expect(processor->setupProcessing(processSetup) == kResultOk,
         "prepare the delay-bearing drag test");
  installLimiterPipeline(*processor, 63);
  expect(processor->setActive(true) == kResultOk,
         "activate the delay-bearing drag test");

  std::array<float, 64> inputLeft{};
  std::array<float, 64> inputRight{};
  std::array<float, 64> outputLeft{};
  std::array<float, 64> outputRight{};
  inputLeft.fill(0.2f);
  inputRight.fill(-0.2f);
  Sample32 *inputChannels[]{inputLeft.data(), inputRight.data()};
  Sample32 *outputChannels[]{outputLeft.data(), outputRight.data()};
  AudioBusBuffers input{};
  input.numChannels = 2;
  input.channelBuffers32 = inputChannels;
  AudioBusBuffers output{};
  output.numChannels = 2;
  output.channelBuffers32 = outputChannels;
  ProcessData data{};
  data.symbolicSampleSize = kSample32;
  data.numSamples = 64;
  data.numInputs = 1;
  data.numOutputs = 1;
  data.inputs = &input;
  data.outputs = &output;
  expect(processor->process(data) == kResultOk,
         "warm the delay-bearing drag test");

  const auto before = PluginProcessorTestAccess::processCounters(*processor);
  const auto failuresBefore =
      PluginProcessorTestAccess::processFailureSequence(*processor);
  std::atomic_bool running{true};
  std::atomic_bool published{true};
  std::atomic<std::uint64_t> serviceTicks{0};
  // Reuse the mailbox payload throughout the gesture.
  auto command = std::make_unique<effetune::vst::AudioCommand>();
  std::array<float, 6> packed{0.0f, 100.0f, 3.0f, 1.0f, 0.0f, -1.0f};
  std::thread control([&] {
    std::uint32_t step = 0;
    while (running.load(std::memory_order_acquire)) {
      // Lookahead is the delay-bearing parameter: every distinct value moves
      // the instance latency and therefore the compensation plan.
      packed[2] = 1.0f + static_cast<float>(step++ % 8u);
      if (!PluginProcessorTestAccess::publishParameterImage(
              *processor, *command, 63u, 3039928906u, std::span<const float>(packed))) {
        published.store(false, std::memory_order_release);
        return;
      }
      PluginProcessorTestAccess::pollLatencyUpdatesFromUi(*processor);
      PluginProcessorTestAccess::pollLatencyUpdatesFromUi(*processor);
      serviceTicks.fetch_add(1, std::memory_order_acq_rel);
    }
  });

  constexpr std::uint64_t blocks = 1500;
  for (std::uint64_t block = 0; block < blocks; ++block) {
    expect(processor->process(data) == kResultOk,
           "process a block while a delay-bearing parameter is dragged");
  }
  running.store(false, std::memory_order_release);
  control.join();

  const auto after = PluginProcessorTestAccess::processCounters(*processor);
  expect(published.load(std::memory_order_acquire),
         "every delay-bearing image reached the mailbox");
  expect(serviceTicks.load(std::memory_order_acquire) != 0,
         "the control service really ran alongside the audio callback");
  expect(after.batchAttempts - before.batchAttempts == blocks &&
             after.completedBatches - before.completedBatches == blocks,
         "a delay-bearing drag turns no block into dry audio");
  expect(PluginProcessorTestAccess::processFailureSequence(*processor) ==
             failuresBefore,
         "a delay-bearing drag raises no deferred diagnostic");
  for (int block = 0; block < 8; ++block) {
    expect(processor->process(data) == kResultOk, "settle the final drag image while running");
    PluginProcessorTestAccess::serviceLatencyUpdates(*processor);
  }
  expect(!PluginProcessorTestAccess::hasPendingControlWork(*processor),
         "the final drag compensation converges without stopping the callback");

  expect(processor->setActive(false) == kResultOk,
         "deactivate the delay-bearing drag test");
  expect(processor->terminate() == kResultOk,
         "terminate the delay-bearing drag test");
}

// A descriptor that failed to apply is retried behind a backoff that grows to
// 800 ms. A tick inside that backoff has nothing to do, so it must claim
// neither the runtime image nor the processing gate: both wait an in-flight
// block out, and closing the gate would cost the next block its processed
// signal for no work at all.
void testFailedDescriptorBackoffClaimsNothing() {
  auto processor = std::make_unique<EffeTuneProcessor>();
  expect(processor->initialize(nullptr) == kResultOk,
         "initialize the descriptor backoff test");
  auto processSetup = setup(48000.0, 64);
  expect(processor->setupProcessing(processSetup) == kResultOk,
         "prepare the descriptor backoff test");
  installLimiterPipeline(*processor, 64);
  expect(processor->setActive(true) == kResultOk,
         "activate the descriptor backoff test");

  std::array<float, 64> inputLeft{};
  std::array<float, 64> inputRight{};
  std::array<float, 64> outputLeft{};
  std::array<float, 64> outputRight{};
  inputLeft.fill(0.2f);
  inputRight.fill(-0.2f);
  Sample32 *inputChannels[]{inputLeft.data(), inputRight.data()};
  Sample32 *outputChannels[]{outputLeft.data(), outputRight.data()};
  AudioBusBuffers input{};
  input.numChannels = 2;
  input.channelBuffers32 = inputChannels;
  AudioBusBuffers output{};
  output.numChannels = 2;
  output.channelBuffers32 = outputChannels;
  ProcessData data{};
  data.symbolicSampleSize = kSample32;
  data.numSamples = 64;
  data.numInputs = 1;
  data.numOutputs = 1;
  data.inputs = &input;
  data.outputs = &output;
  for (int block = 0; block < 4; ++block) {
    expect(processor->process(data) == kResultOk,
           "warm the descriptor backoff test");
  }

  PluginProcessorTestAccess::failPipelinePlanRefreshes(*processor, 1);
  updateLimiterRouting(*processor, 64, "L");
  const auto generation =
      PluginProcessorTestAccess::descriptorGeneration(*processor);
  PluginProcessorTestAccess::serviceLatencyUpdates(*processor);
  expect(PluginProcessorTestAccess::remainingPipelinePlanRefreshFailures(
             *processor) == 0u,
         "the injected descriptor failure was consumed");
  expect(PluginProcessorTestAccess::servicedDescriptorGeneration(*processor) <
             generation,
         "the failed descriptor is still pending");
  // Let the audio thread stage the image the topology update carried and let
  // one tick acknowledge it, so the descriptor waiting on its backoff is the
  // only work still outstanding.
  expect(processor->process(data) == kResultOk,
         "stage the image the topology update carried");
  PluginProcessorTestAccess::serviceLatencyUpdates(*processor);
  expect(processor->process(data) == kResultOk,
         "render another block before the backoff probe");
  PluginProcessorTestAccess::deferPipelinePlanRetry(*processor,
                                                    std::chrono::seconds(60));
  const auto failuresBefore =
      PluginProcessorTestAccess::processFailureSequence(*processor);

  // A block is held inside process() for the whole tick. Claiming the runtime
  // image and closing the processing gate both wait it out, so only a tick that
  // does neither can return here.
  PluginProcessorTestAccess::beginSyntheticBlock(*processor);
  std::atomic_bool completed{false};
  std::thread service([&] {
    PluginProcessorTestAccess::serviceLatencyUpdates(*processor);
    completed.store(true, std::memory_order_release);
  });
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
  while (!completed.load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  const auto completedWhileBlocked = completed.load(std::memory_order_acquire);
  PluginProcessorTestAccess::endSyntheticBlock(*processor);
  service.join();
  expect(completedWhileBlocked,
         "a tick inside the retry backoff claims neither the runtime image nor "
         "the processing gate");
  expect(PluginProcessorTestAccess::processFailureSequence(*processor) ==
             failuresBefore,
         "a tick that does no work raises no diagnostic");
  expect(PluginProcessorTestAccess::servicedDescriptorGeneration(*processor) <
             generation,
         "the descriptor is still waiting for its retry");

  expect(processor->setActive(false) == kResultOk,
         "deactivate the descriptor backoff test");
  expect(processor->terminate() == kResultOk,
         "terminate the descriptor backoff test");
}

// A stopped host may keep sending flush-only blocks. They carry no audio, so
// the audio thread stages nothing from them; if they counted as playback the
// control service would never take the engine over either, and an edit made
// while the transport is stopped would reach the DSP through neither path.
void testFlushOnlyBlocksLetTheControlServiceReachTheDsp() {
  auto processor = std::make_unique<EffeTuneProcessor>();
  expect(processor->initialize(nullptr) == kResultOk,
         "initialize the flush-only service test");
  auto processSetup = setup(48000.0, 64);
  expect(processor->setupProcessing(processSetup) == kResultOk,
         "prepare the flush-only service test");
  const auto installed = choc::json::parse(processor->handleUiMessage(
      R"({"type":"pipeline/rebuild","payload":{"pipeline":"A","plugins":[)"
      R"({"id":74,"type":"DCOffsetPlugin","name":"DC Offset","enabled":true,)"
      R"("parameters":{"of":0.25},"wasmParams":[0.25],)"
      R"("wasmParamsHash":1104945464}]}})"));
  expect(installed["ok"].getWithDefault<bool>(false),
         "install the flush-only service pipeline");
  expect(processor->setActive(true) == kResultOk,
         "activate the flush-only service test");

  std::array<float, 64> inputLeft{};
  std::array<float, 64> inputRight{};
  std::array<float, 64> outputLeft{};
  std::array<float, 64> outputRight{};
  inputLeft.fill(1.0f);
  inputRight.fill(1.0f);
  Sample32 *inputChannels[]{inputLeft.data(), inputRight.data()};
  Sample32 *outputChannels[]{outputLeft.data(), outputRight.data()};
  AudioBusBuffers input{};
  input.numChannels = 2;
  input.channelBuffers32 = inputChannels;
  AudioBusBuffers output{};
  output.numChannels = 2;
  output.channelBuffers32 = outputChannels;
  ProcessData data{};
  data.symbolicSampleSize = kSample32;
  data.numSamples = 64;
  data.numInputs = 1;
  data.numOutputs = 1;
  data.inputs = &input;
  data.outputs = &output;
  expect(processor->process(data) == kResultOk,
         "render one block before the transport stops");
  expect(std::abs(outputLeft[0] - 1.25f) < 1.0e-6f,
         "the rendered block carries the installed offset");

  auto command = std::make_unique<effetune::vst::AudioCommand>();
  expect(PluginProcessorTestAccess::publishParameterImage(*processor, *command,
                                                          74u, 1104945464u, 0.5f),
         "publish an edit while the transport is stopped");

  ProcessData flush = data;
  flush.numSamples = 0;
  const auto epochBefore = PluginProcessorTestAccess::blockEpoch(*processor);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  auto serviced = false;
  while (!serviced && std::chrono::steady_clock::now() < deadline) {
    expect(processor->process(flush) == kResultOk,
           "a stopped host keeps sending flush-only blocks");
    PluginProcessorTestAccess::serviceLatencyUpdates(*processor);
    serviced = !PluginProcessorTestAccess::hasPendingControlWork(*processor);
    if (!serviced) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }
  expect(PluginProcessorTestAccess::blockEpoch(*processor) > epochBefore,
         "the flush-only blocks really advanced the quiescence epoch");
  expect(serviced,
         "flush-only blocks do not keep the control service out of the engine");
  expect(PluginProcessorTestAccess::servicedParameterImageGeneration(*processor) ==
             PluginProcessorTestAccess::parameterImageGeneration(*processor),
         "the stopped edit is acknowledged instead of left dirty for a block "
         "that never comes");

  outputLeft.fill(0.0f);
  outputRight.fill(0.0f);
  expect(processor->process(data) == kResultOk,
         "resume rendering after the stopped edit");
  expect(std::abs(outputLeft[0] - 1.5f) < 1.0e-6f,
         "the resumed block carries the edit the control service delivered");

  expect(processor->setActive(false) == kResultOk,
         "deactivate the flush-only service test");
  expect(processor->terminate() == kResultOk,
         "terminate the flush-only service test");
}

// A rebuild that the engine refuses destroys its instances on the way out, so
// rolling the runtime image back is not enough: the previous topology has to be
// rebuilt from it, or one refused plug-in update leaves the whole plug-in dry
// for good.
void testFailedPluginRebuildKeepsProcessing() {
  auto processor = std::make_unique<EffeTuneProcessor>();
  expect(processor->initialize(nullptr) == kResultOk,
         "initialize the failed-rebuild rollback test");
  auto processSetup = setup(48000.0, 64);
  expect(processor->setupProcessing(processSetup) == kResultOk,
         "prepare the failed-rebuild rollback test");
  const auto installed = choc::json::parse(processor->handleUiMessage(
      R"({"type":"pipeline/rebuild","payload":{"pipeline":"A","plugins":[)"
      R"({"id":101,"type":"DCOffsetPlugin","name":"DC Offset","enabled":true,)"
      R"("parameters":{"of":0.25},"wasmParams":[0.25],)"
      R"("wasmParamsHash":1104945464}]}})"));
  expect(installed["ok"].getWithDefault<bool>(false),
         "install the failed-rebuild rollback pipeline");
  expect(processor->setActive(true) == kResultOk,
         "activate the failed-rebuild rollback test");

  std::array<float, 64> inputLeft{};
  std::array<float, 64> inputRight{};
  std::array<float, 64> outputLeft{};
  std::array<float, 64> outputRight{};
  inputLeft.fill(1.0f);
  inputRight.fill(1.0f);
  Sample32 *inputChannels[]{inputLeft.data(), inputRight.data()};
  Sample32 *outputChannels[]{outputLeft.data(), outputRight.data()};
  AudioBusBuffers input{};
  input.numChannels = 2;
  input.channelBuffers32 = inputChannels;
  AudioBusBuffers output{};
  output.numChannels = 2;
  output.channelBuffers32 = outputChannels;
  ProcessData data{};
  data.symbolicSampleSize = kSample32;
  data.numSamples = 64;
  data.numInputs = 1;
  data.numOutputs = 1;
  data.inputs = &input;
  data.outputs = &output;
  expect(processor->process(data) == kResultOk,
         "render before the refused plug-in update");
  expect(std::abs(outputLeft[0] - 1.25f) < 1.0e-6f,
         "the pre-update block carries the installed offset");

  // A parameter hash the kernel does not recognise cannot reuse the instance,
  // so this takes the rebuild branch and the rebuild itself is rejected.
  const auto refused = choc::json::parse(processor->handleUiMessage(
      R"({"type":"pipeline/updatePlugin","payload":{"pipeline":"A","plugin":)"
      R"({"id":101,"type":"DCOffsetPlugin","name":"DC Offset","enabled":true,)"
      R"("parameters":{"of":0.75},"wasmParams":[0.75],)"
      R"("wasmParamsHash":12345}}})"));
  expect(!refused["ok"].getWithDefault<bool>(true),
         "the engine refuses a plug-in update with an unknown parameter hash");

  const auto before = PluginProcessorTestAccess::processCounters(*processor);
  outputLeft.fill(0.0f);
  outputRight.fill(0.0f);
  expect(processor->process(data) == kResultOk,
         "render after the refused plug-in update");
  const auto after = PluginProcessorTestAccess::processCounters(*processor);
  expect(after.batchAttempts == before.batchAttempts + 1u &&
             after.completedBatches == before.completedBatches + 1u,
         "a refused plug-in update leaves the engine processing");
  expect(std::abs(outputLeft[0] - 1.25f) < 1.0e-6f,
         "the rolled-back topology keeps producing the previous offset");

  // The rolled-back image and the engine still agree, so an accepted update
  // reaches the DSP as usual.
  const auto accepted = choc::json::parse(processor->handleUiMessage(
      R"({"type":"pipeline/updatePlugin","payload":{"pipeline":"A","plugin":)"
      R"({"id":101,"type":"DCOffsetPlugin","name":"DC Offset","enabled":true,)"
      R"("parameters":{"of":0.5},"wasmParams":[0.5],)"
      R"("wasmParamsHash":1104945464}}})"));
  expect(accepted["ok"].getWithDefault<bool>(false),
         "a valid plug-in update is accepted after the rollback");
  outputLeft.fill(0.0f);
  outputRight.fill(0.0f);
  expect(processor->process(data) == kResultOk,
         "render the accepted update after the rollback");
  expect(std::abs(outputLeft[0] - 1.5f) < 1.0e-6f,
         "the accepted update reaches the DSP after the rollback");

  expect(processor->setActive(false) == kResultOk,
         "deactivate the failed-rebuild rollback test");
  expect(processor->terminate() == kResultOk,
         "terminate the failed-rebuild rollback test");
}

// Full-image rebuilds and history restores are bulk UI transactions: when the
// engine refuses their candidate runtime, the UI keeps showing the old image.
// The engine, document, binding values and scheduler must therefore all keep
// the same old playable generation too.
void testFailedBulkRebuildsPreserveTheWholePlayableGeneration() {
  auto processor = std::make_unique<EffeTuneProcessor>();
  expect(processor->initialize(nullptr) == kResultOk,
         "initialize the failed bulk transaction test");
  auto processSetup = setup(48000.0, 64);
  expect(processor->setupProcessing(processSetup) == kResultOk,
         "prepare the failed bulk transaction test");
  const auto installed = choc::json::parse(processor->handleUiMessage(
      R"({"type":"pipeline/rebuild","payload":{"pipeline":"A","plugins":[)"
      R"({"id":603,"type":"DCOffsetPlugin","name":"DC Offset","enabled":true,)"
      R"("parameters":{"of":0.25},"wasmParams":[0.25],)"
      R"("wasmParamsHash":1104945464}]}})"));
  expect(installed["ok"].getWithDefault<bool>(false),
         "install the failed bulk transaction pipeline");
  const effetune::vst::AutomationTargetIdentity identity{
      'A', 603, "DCOffsetPlugin", "of", 0};
  const auto parameterId = boundAutomationParameterId(*processor, identity);
  const auto slot =
      PluginProcessorTestAccess::activeAutomationSlot(*processor, identity);
  expect(slot.has_value(), "the bulk rollback target owns an automation lane");
  expect(processor->setActive(true) == kResultOk,
         "activate the failed bulk transaction test");

  std::array<float, 64> inputLeft{};
  std::array<float, 64> inputRight{};
  std::array<float, 64> outputLeft{};
  std::array<float, 64> outputRight{};
  Sample32 *inputChannels[]{inputLeft.data(), inputRight.data()};
  Sample32 *outputChannels[]{outputLeft.data(), outputRight.data()};
  AudioBusBuffers input{};
  input.numChannels = 2;
  input.channelBuffers32 = inputChannels;
  AudioBusBuffers output{};
  output.numChannels = 2;
  output.channelBuffers32 = outputChannels;
  ProcessData data{};
  data.symbolicSampleSize = kSample32;
  data.numSamples = 64;
  data.numInputs = 1;
  data.numOutputs = 1;
  data.inputs = &input;
  data.outputs = &output;

  ParameterChanges changes(1);
  int32 queueIndex = 0;
  auto *queue = changes.addParameterData(parameterId, queueIndex);
  int32 pointIndex = 0;
  constexpr double kPreviousNormalized = 0.625;
  expect(queue != nullptr &&
             queue->addPoint(0, kPreviousNormalized, pointIndex) == kResultTrue,
         "publish the automation value the old generation must retain");
  data.inputParameterChanges = &changes;
  expect(processor->process(data) == kResultOk,
         "render the old automated generation");
  data.inputParameterChanges = nullptr;
  PluginProcessorTestAccess::drainAutomationValues(*processor);

  const auto savedBefore = choc::json::parse(processor->handleUiMessage(
      R"({"type":"storage/readFile","payload":{"path":"pipeline-state.json"}})"))
                               ["content"]
                                   .getWithDefault<std::string>({});
  const auto assertOldGeneration = [&](const std::string &operation) {
    const auto savedAfter = choc::json::parse(processor->handleUiMessage(
        R"({"type":"storage/readFile","payload":{"path":"pipeline-state.json"}})"))
                                ["content"]
                                    .getWithDefault<std::string>({});
    const auto activeSlot =
        PluginProcessorTestAccess::activeAutomationSlot(*processor, identity);
    expect(savedAfter == savedBefore && activeSlot == slot &&
               std::abs(PluginProcessorTestAccess::playedAutomationValue(
                            *processor, *slot) -
                        kPreviousNormalized) < 1.0e-9 &&
               std::abs(processor->getParamNormalized(parameterId) -
                        kPreviousNormalized) < 1.0e-9 &&
               std::abs(PluginProcessorTestAccess::runtimePackedParameter(
                            *processor, 603, 0) -
                        0.25f) < 1.0e-6f &&
               PluginProcessorTestAccess::processingReady(*processor),
           operation +
               " leaves the logical state, binding, scheduler and runtime unchanged");

    outputLeft.fill(0.0f);
    outputRight.fill(0.0f);
    const auto countersBefore =
        PluginProcessorTestAccess::processCounters(*processor);
    expect(processor->process(data) == kResultOk,
           "render after " + operation);
    const auto countersAfter =
        PluginProcessorTestAccess::processCounters(*processor);
    expect(countersAfter.batchAttempts == countersBefore.batchAttempts + 1u &&
               countersAfter.completedBatches ==
                   countersBefore.completedBatches + 1u &&
               std::abs(outputLeft[0] - 0.25f) < 1.0e-6f,
           operation + " restores the old wet engine");
  };

  const auto refusedRebuild = choc::json::parse(processor->handleUiMessage(
      R"({"type":"pipeline/rebuild","payload":{"pipeline":"A","plugins":[)"
      R"({"id":603,"type":"DCOffsetPlugin","name":"DC Offset","enabled":true,)"
      R"("parameters":{"of":-0.5},"wasmParams":[-0.5],"wasmParamsHash":12345}],)"
      R"("automationEdits":[{"pipeline":"A","pluginId":603,)"
      R"("pluginType":"DCOffsetPlugin","parameterKey":"of","elementIndex":0,)"
      R"("normalized":0.25}]}})"));
  expect(!refusedRebuild["ok"].getWithDefault<bool>(true),
         "the engine refuses the invalid-hash full rebuild");
  assertOldGeneration("a refused full rebuild");

  const auto refusedHistory = choc::json::parse(processor->handleUiMessage(
      R"({"type":"pipeline/restoreHistory","payload":{"pipelineA":[)"
      R"({"id":603,"type":"DCOffsetPlugin","name":"DC Offset","enabled":true,)"
      R"("parameters":{"of":0.75},"wasmParams":[0.75],"wasmParamsHash":12345}],)"
      R"("pipelineB":null,"pipelineBInitialized":false,"currentPipeline":"A",)"
      R"("automationEdits":[{"pipeline":"A","pluginId":603,)"
      R"("pluginType":"DCOffsetPlugin","parameterKey":"of","elementIndex":0,)"
      R"("normalized":0.875}]}})"));
  expect(!refusedHistory["ok"].getWithDefault<bool>(true),
         "the engine refuses the invalid-hash history restore");
  assertOldGeneration("a refused history restore");

  expect(processor->setActive(false) == kResultOk,
         "deactivate the failed bulk transaction test");
  expect(processor->terminate() == kResultOk,
         "terminate the failed bulk transaction test");
}

namespace gate_ordering {

// Everything the four tests below need to render one block of ones through a
// pipeline. Declared once because each of them drives audio while another
// thread edits the pipeline underneath it.
struct AudioRig {
  std::array<float, 64> inputLeft{};
  std::array<float, 64> inputRight{};
  std::array<float, 64> outputLeft{};
  std::array<float, 64> outputRight{};
  Sample32 *inputChannels[2]{inputLeft.data(), inputRight.data()};
  Sample32 *outputChannels[2]{outputLeft.data(), outputRight.data()};
  AudioBusBuffers input{};
  AudioBusBuffers output{};
  ProcessData data{};

  explicit AudioRig(const float level) {
    inputLeft.fill(level);
    inputRight.fill(level);
    input.numChannels = 2;
    input.channelBuffers32 = inputChannels;
    output.numChannels = 2;
    output.channelBuffers32 = outputChannels;
    data.symbolicSampleSize = kSample32;
    data.numSamples = 64;
    data.numInputs = 1;
    data.numOutputs = 1;
    data.inputs = &input;
    data.outputs = &output;
  }

  AudioRig(const AudioRig &) = delete;
  AudioRig &operator=(const AudioRig &) = delete;
};

} // namespace gate_ordering

void testContextualExecutionAdmissionSurvivesEngineContextChanges() {
  constexpr std::uint32_t pluginId = 301;
  auto processor = std::make_unique<EffeTuneProcessor>();
  expect(processor->initialize(nullptr) == kResultOk,
         "initialize contextual execution-admission test");
  auto processSetup = setup(96000.0, 64);
  expect(processor->setupProcessing(processSetup) == kResultOk,
         "prepare contextual execution-admission test");

  const auto installed = choc::json::parse(processor->handleUiMessage(
      R"({"type":"pipeline/rebuild","payload":{"pipeline":"A","plugins":[)"
      R"({"id":301,"type":"PitchShifterHQPlugin","name":"Constrained Pitch",)"
      R"("enabled":true,"parameters":{"ps":0,"ft":0,)"
      R"("futureParameter":{"nested":9}},"wasmParams":[0,0],)"
      R"("wasmParamsHash":3982397611,"executionCapabilities":{)"
      R"("requiresWasm":true,"supportedSampleRates":[44100,48000,88200,96000,176400,192000]},)"
      R"("futurePayload":{"v":7}},)"
      R"({"id":302,"type":"VolumePlugin","name":"Following Volume","enabled":true,)"
      R"("parameters":{"vl":-6},"wasmParams":[-6],)"
      R"("wasmParamsHash":1719233191}]}})"));
  const auto admittedLatency =
      PluginProcessorTestAccess::enginePipelineLatency(*processor);
  const auto installedConstrainedState = findExecutionState(installed, pluginId);
  const auto installedFollowingState = findExecutionState(installed, 302u);
  expect(installed["ok"].getWithDefault<bool>(false) &&
             installedConstrainedState["pluginType"].getWithDefault<std::string>({}) ==
                 "PitchShifterHQPlugin" &&
             installedConstrainedState["state"].getWithDefault<std::string>({}) ==
                 "active" &&
             installedFollowingState["state"].getWithDefault<std::string>({}) ==
                 "active" &&
             PluginProcessorTestAccess::runtimePluginCount(*processor, pluginId) == 1u &&
             PluginProcessorTestAccess::runtimePluginCount(*processor, 302u) == 1u &&
             !PluginProcessorTestAccess::runtimeContextuallyBypassed(*processor, pluginId) &&
             admittedLatency > 0u,
         "install one admitted constrained runtime with its latency");
  expect(processor->setActive(true) == kResultOk,
         "activate contextual execution-admission test");

  gate_ordering::AudioRig rig(0.0f);
  const auto renderSteady = [&] {
    rig.inputLeft.fill(0.25f);
    rig.inputRight.fill(-0.25f);
    for (int block = 0; block < 180; ++block) {
      rig.outputLeft.fill(0.0f);
      rig.outputRight.fill(0.0f);
      expect(processor->process(rig.data) == kResultOk,
             "settle contextual execution-admission audio");
    }
    return std::array{rig.outputLeft.back(), rig.outputRight.back()};
  };

  constexpr auto volumeGain = 0.5011872336f;
  const auto admitted = renderSteady();
  expect(std::abs(admitted[0] - 0.25f * volumeGain) < 1.0e-4f &&
             std::abs(admitted[1] + 0.25f * volumeGain) < 1.0e-4f,
         "the admitted runtime produces the expected steady output");

  const auto unsupported = choc::json::parse(processor->handleUiMessage(
      R"({"type":"os/set","payload":{"factor":4,"phase":"linear","quality":"medium"}})"));
  const auto unsupportedContext = hostInfo(*processor);
  const auto bypassedMutationState = findExecutionState(unsupported, pluginId);
  const auto bypassedHostState = findExecutionState(unsupportedContext, pluginId);
  expect(unsupported["ok"].getWithDefault<bool>(false) &&
             unsupported["skippedUnsupported"].getWithDefault<bool>(false) &&
             bypassedMutationState["state"].getWithDefault<std::string>({}) ==
                 "bypassed" &&
             bypassedMutationState["reason"].getWithDefault<std::string>({}) ==
                 "unsupportedSampleRate" &&
             bypassedHostState["state"].getWithDefault<std::string>({}) ==
                 "bypassed" &&
             bypassedHostState["reason"].getWithDefault<std::string>({}) ==
                 "unsupportedSampleRate" &&
             PluginProcessorTestAccess::runtimePluginCount(*processor, pluginId) == 1u &&
             PluginProcessorTestAccess::runtimeContextuallyBypassed(*processor, pluginId) &&
             PluginProcessorTestAccess::enginePipelineLatency(*processor) == 0u &&
             unsupportedContext["engineSampleRate"].getWithDefault<double>(0.0) == 384000.0 &&
             unsupportedContext["oversamplingFactor"].getWithDefault<std::int64_t>(0) == 4 &&
             PluginProcessorTestAccess::processingReady(*processor),
         "96 kHz to 4x succeeds and publishes a ready contextual bypass");

  const auto bypassed = renderSteady();
  expect(std::abs(bypassed[0] - 0.25f * volumeGain) < 1.0e-4f &&
             std::abs(bypassed[1] + 0.25f * volumeGain) < 1.0e-4f &&
             PluginProcessorTestAccess::runtimePluginCount(*processor, 302u) == 1u,
         "the contextual bypass keeps output flowing through the following runtime");

  const auto storedWhileUnsupported = choc::json::parse(processor->handleUiMessage(
      R"({"type":"storage/readFile","payload":{"path":"pipeline-state.json"}})"));
  const auto unsupportedState = choc::json::parse(
      storedWhileUnsupported["content"].getWithDefault<std::string>({}));
  const auto unsupportedPlugin = unsupportedState["pipelineA"][0];
  expect(unsupportedPlugin["futurePayload"]["v"].getWithDefault<std::int64_t>(0) == 7 &&
             unsupportedPlugin["parameters"]["futureParameter"]["nested"]
                     .getWithDefault<std::int64_t>(0) == 9 &&
             !unsupportedPlugin.hasObjectMember("executionCapabilities"),
         "contextual bypass preserves opaque logical state without serializing admission");

  const auto readmitted = choc::json::parse(processor->handleUiMessage(
      R"({"type":"os/set","payload":{"factor":1,"phase":"linear","quality":"medium"}})"));
  const auto readmittedState = findExecutionState(readmitted, pluginId);
  expect(readmitted["ok"].getWithDefault<bool>(false) &&
             !readmitted["skippedUnsupported"].getWithDefault<bool>(false) &&
             readmittedState["state"].getWithDefault<std::string>({}) == "active" &&
             !readmittedState.hasObjectMember("reason") &&
             PluginProcessorTestAccess::runtimePluginCount(*processor, pluginId) == 1u &&
             PluginProcessorTestAccess::runtimePluginCount(*processor, 302u) == 1u &&
             !PluginProcessorTestAccess::runtimeContextuallyBypassed(*processor, pluginId) &&
             PluginProcessorTestAccess::enginePipelineLatency(*processor) == admittedLatency &&
             PluginProcessorTestAccess::processingReady(*processor),
         "the supported context restores exactly one original runtime and its latency");

  const auto recovered = renderSteady();
  expect(std::abs(recovered[0] - 0.25f * volumeGain) < 1.0e-4f &&
             std::abs(recovered[1] + 0.25f * volumeGain) < 1.0e-4f,
         "the re-admitted runtime returns to processed output");
  const auto storedAfterRecovery = choc::json::parse(processor->handleUiMessage(
      R"({"type":"storage/readFile","payload":{"path":"pipeline-state.json"}})"));
  const auto recoveredState = choc::json::parse(
      storedAfterRecovery["content"].getWithDefault<std::string>({}));
  expect(recoveredState["pipelineA"][0]["futurePayload"]["v"]
                     .getWithDefault<std::int64_t>(0) == 7 &&
             recoveredState["pipelineA"][0]["parameters"]["futureParameter"]["nested"]
                     .getWithDefault<std::int64_t>(0) == 9 &&
             hostInfo(*processor)["diagnostics"].size() == 0,
         "re-admission keeps opaque state and raises no processing diagnostic");

  expect(processor->setActive(false) == kResultOk,
         "deactivate contextual execution-admission test");
  expect(processor->terminate() == kResultOk,
         "terminate contextual execution-admission test");
}

void testConcurrentHostContextChangeWinsPluginUpdateAdmission() {
  constexpr std::uint32_t pluginId = 321;
  auto processor = std::make_unique<EffeTuneProcessor>();
  expect(processor->initialize(nullptr) == kResultOk,
         "initialize concurrent execution-admission test");
  SpeakerArrangement fourChannelInput = SpeakerArr::k40Cine;
  SpeakerArrangement fourChannelOutput = SpeakerArr::k40Cine;
  expect(processor->setBusArrangements(&fourChannelInput, 1,
                                       &fourChannelOutput, 1) == kResultTrue,
         "select the initially supported four-channel arrangement");
  auto processSetup = setup(48000.0, 64);
  expect(processor->setupProcessing(processSetup) == kResultOk,
         "prepare concurrent execution-admission test");

  const auto installed = choc::json::parse(processor->handleUiMessage(
      R"({"type":"pipeline/rebuild","payload":{"pipeline":"A","plugins":[)"
      R"({"id":321,"type":"VolumePlugin","name":"Pair Send","enabled":true,)"
      R"("inputBus":0,"outputBus":1,"channel":"34",)"
      R"("parameters":{"vl":-6},"wasmParams":[-6],)"
      R"("wasmParamsHash":1719233191,"executionCapabilities":{)"
      R"("supportedChannelModes":["stereo-pair"]}}]}})"));
  expect(installed["ok"].getWithDefault<bool>(false) &&
             !PluginProcessorTestAccess::runtimeContextuallyBypassed(
                 *processor, pluginId),
         "install channel 34 while both selected channels exist");

  PluginProcessorTestAccess::pausePluginUpdateBeforeRuntimeTransaction(
      *processor, true);
  std::string updateResponse;
  std::atomic_bool updateCompleted{false};
  std::thread updater([&] {
    updateResponse = processor->handleUiMessage(
        R"({"type":"pipeline/updatePlugin","payload":{"pipeline":"A","plugin":)"
        R"({"id":321,"type":"VolumePlugin","name":"Pair Send","enabled":true,)"
        R"("inputBus":0,"outputBus":1,"channel":"34",)"
        R"("parameters":{"vl":-12},"wasmParams":[-12],)"
        R"("wasmParamsHash":1719233191,"executionCapabilities":{)"
        R"("supportedChannelModes":["stereo-pair"]}}}})");
    updateCompleted.store(true, std::memory_order_release);
  });
  const auto pauseDeadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (!PluginProcessorTestAccess::pluginUpdatePausedBeforeRuntimeTransaction(
             *processor) &&
         !updateCompleted.load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < pauseDeadline) {
    std::this_thread::yield();
  }
  const auto reachedRuntimeBoundary =
      PluginProcessorTestAccess::pluginUpdatePausedBeforeRuntimeTransaction(
          *processor);

  SpeakerArrangement threeChannelInput = SpeakerArr::k30Cine;
  SpeakerArrangement threeChannelOutput = SpeakerArr::k30Cine;
  const auto arrangementResult = processor->setBusArrangements(
      &threeChannelInput, 1, &threeChannelOutput, 1);
  const auto bypassedBeforeUpdateResumes =
      PluginProcessorTestAccess::runtimeContextuallyBypassed(*processor,
                                                             pluginId);
  PluginProcessorTestAccess::pausePluginUpdateBeforeRuntimeTransaction(
      *processor, false);
  updater.join();

  const auto updated = choc::json::parse(updateResponse);
  const auto finalContext = hostInfo(*processor);
  expect(reachedRuntimeBoundary && arrangementResult == kResultTrue &&
             bypassedBeforeUpdateResumes,
         "the three-channel rebuild lands while the UI update is paused");
  expect(updated["ok"].getWithDefault<bool>(false) &&
             updated["skippedUnsupported"].getWithDefault<bool>(false) &&
             !updated.hasObjectMember("rebuildAssets") &&
             finalContext["channels"].getWithDefault<std::int64_t>(0) == 3 &&
             PluginProcessorTestAccess::runtimePluginCount(*processor,
                                                           pluginId) == 1u &&
             PluginProcessorTestAccess::runtimeContextuallyBypassed(
                 *processor, pluginId) &&
             PluginProcessorTestAccess::processingReady(*processor),
         "the resumed update uses the fresh context and keeps the bypassed "
         "runtime generation ready");

  expect(processor->terminate() == kResultOk,
         "terminate concurrent execution-admission test");
}

void testOversamplingFailureRestoresPreviousPlayableGeneration() {
  auto processor = std::make_unique<EffeTuneProcessor>();
  expect(processor->initialize(nullptr) == kResultOk,
         "initialize oversampling rollback test");
  auto processSetup = setup(96000.0, 64);
  expect(processor->setupProcessing(processSetup) == kResultOk,
         "prepare oversampling rollback test");
  const auto installed = choc::json::parse(processor->handleUiMessage(
      R"({"type":"pipeline/rebuild","payload":{"pipeline":"A","plugins":[)"
      R"({"id":311,"type":"PitchShifterHQPlugin","name":"Legacy Pitch",)"
      R"("enabled":true,"parameters":{"ps":0,"ft":0},"wasmParams":[0,0],)"
      R"("wasmParamsHash":3982397611}]}})"));
  const auto previousLatency =
      PluginProcessorTestAccess::enginePipelineLatency(*processor);
  expect(installed["ok"].getWithDefault<bool>(false) && previousLatency > 0u,
         "install legacy runtime without contextual metadata");

  const auto refused = choc::json::parse(processor->handleUiMessage(
      R"({"type":"os/set","payload":{"factor":4,"phase":"linear","quality":"medium"}})"));
  const auto restoredContext = hostInfo(*processor);
  expect(!refused["ok"].getWithDefault<bool>(true) &&
             PluginProcessorTestAccess::processingReady(*processor) &&
             PluginProcessorTestAccess::enginePipelineLatency(*processor) ==
                 previousLatency &&
             restoredContext["engineSampleRate"].getWithDefault<double>(0.0) == 96000.0 &&
             restoredContext["oversamplingFactor"].getWithDefault<std::int64_t>(0) == 1,
         "a refused oversampling change restores the old runtime, context, and ready gate");

  expect(processor->terminate() == kResultOk,
         "terminate oversampling rollback test");
}

// Closing the processing gate and proving the audio thread out of the engine
// belong inside processingResourcesMutex_. A path that closes the gate before
// taking the lock loses that decision to whichever control thread is already in
// a window: that thread restores the gate it captured on the way out, and this
// one then rewrites the engine instances, the runtime image and the delay line
// with the gate open and a block walking them. The gate staying open for as
// long as the path waits for the lock is what says the two are ordered.
void testGateClosesInsideTheControlLock() {
  auto processor = std::make_unique<EffeTuneProcessor>();
  expect(processor->initialize(nullptr) == kResultOk,
         "initialize the gate-ordering test");
  auto processSetup = setup(48000.0, 64);
  expect(processor->setupProcessing(processSetup) == kResultOk,
         "prepare the gate-ordering test");
  const auto installed = choc::json::parse(processor->handleUiMessage(
      R"({"type":"pipeline/rebuild","payload":{"pipeline":"A","plugins":[)"
      R"({"id":107,"type":"DCOffsetPlugin","name":"DC Offset","enabled":true,)"
      R"("parameters":{"of":0.25},"wasmParams":[0.25],)"
      R"("wasmParamsHash":1104945464}]}})"));
  expect(installed["ok"].getWithDefault<bool>(false),
         "install the gate-ordering pipeline");
  expect(processor->setActive(true) == kResultOk,
         "activate the gate-ordering test");

  gate_ordering::AudioRig rig(1.0f);
  expect(processor->process(rig.data) == kResultOk,
         "warm the gate-ordering test");
  expect(std::abs(rig.outputLeft[0] - 1.25f) < 1.0e-6f,
         "the warm-up block is wet at the installed offset");

  std::thread host;
  std::atomic_bool started{false};
  std::atomic_bool completed{false};
  std::atomic_bool reconfigured{false};
  auto rendered = true;
  auto waitedForTheLock = false;
  std::uint32_t wetBlocks = 0;
  {
    const HeldControlGuard guard(*processor);
    host = std::thread([&] {
      started.store(true, std::memory_order_release);
      auto again = setup(48000.0, 64);
      reconfigured.store(processor->setupProcessing(again) == kResultOk,
                         std::memory_order_release);
      completed.store(true, std::memory_order_release);
    });
    while (!started.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    // The reconfiguration is waiting for the control lock the guard holds. It
    // has touched nothing yet, so every block in the meantime still reaches the
    // engine and comes back wet. Nothing is asserted before the thread is
    // joined: an assertion that throws here would leave it joinable.
    for (int block = 0; block < 128; ++block) {
      rig.outputLeft.fill(0.0f);
      rig.outputRight.fill(0.0f);
      rendered = rendered && processor->process(rig.data) == kResultOk;
      wetBlocks += std::abs(rig.outputLeft[0] - 1.25f) < 1.0e-6f ? 1u : 0u;
      std::this_thread::yield();
    }
    waitedForTheLock = !completed.load(std::memory_order_acquire);
  }
  host.join();
  expect(rendered, "process while a reconfiguration waits for the control lock");
  expect(wetBlocks == 128u,
         "a reconfiguration waiting for the control lock leaves the processing "
         "gate open (" + std::to_string(wetBlocks) + " of 128 blocks stayed wet)");
  expect(waitedForTheLock,
         "the reconfiguration really was waiting for the control lock");
  expect(reconfigured.load(std::memory_order_acquire),
         "the reconfiguration succeeds once the control lock is free");

  rig.outputLeft.fill(0.0f);
  rig.outputRight.fill(0.0f);
  expect(processor->process(rig.data) == kResultOk,
         "process after the reconfiguration completed");
  expect(std::abs(rig.outputLeft[0] - 1.25f) < 1.0e-6f,
         "the reconfigured DSP resumes the wet output");

  expect(processor->setActive(false) == kResultOk,
         "deactivate the gate-ordering test");
  expect(processor->terminate() == kResultOk,
         "terminate the gate-ordering test");
}

// Adding, removing and reordering a node all rewrite the engine, and each of
// them costs the one block the exception admits. That block is the user's own
// operation, not a DSP that cannot produce a processed signal, so none of the
// three routes may raise a user-facing diagnostic -- otherwise every edit
// during playback reports "The DSP was not ready" in the status line.
void testTopologyEditsDuringPlaybackRaiseNoDiagnostic() {
  auto processor = std::make_unique<EffeTuneProcessor>();
  expect(processor->initialize(nullptr) == kResultOk,
         "initialize the topology-diagnostic test");
  auto processSetup = setup(48000.0, 64);
  expect(processor->setupProcessing(processSetup) == kResultOk,
         "prepare the topology-diagnostic test");
  constexpr const char *rebuildMessage =
      R"({"type":"pipeline/rebuild","payload":{"pipeline":"A","plugins":[)"
      R"({"id":108,"type":"DCOffsetPlugin","name":"DC Offset","enabled":true,)"
      R"("parameters":{"of":0.25},"wasmParams":[0.25],)"
      R"("wasmParamsHash":1104945464}]}})";
  // A logical ID the runtime image does not hold yet cannot reuse an instance,
  // so this takes the rebuild branch of pipeline/updatePlugin.
  constexpr const char *addNodeMessage =
      R"({"type":"pipeline/updatePlugin","payload":{"pipeline":"A","plugin":)"
      R"({"id":109,"type":"DCOffsetPlugin","name":"Added","enabled":true,)"
      R"("parameters":{"of":0.125},"wasmParams":[0.125],)"
      R"("wasmParamsHash":1104945464}}})";
  constexpr const char *restoreMessage =
      R"({"type":"pipeline/restoreHistory","payload":{"pipelineA":[)"
      R"({"id":108,"type":"DCOffsetPlugin","name":"DC Offset","enabled":true,)"
      R"("parameters":{"of":0.25},"wasmParams":[0.25],)"
      R"("wasmParamsHash":1104945464}],"pipelineB":null,)"
      R"("pipelineBInitialized":false,"currentPipeline":"A"}})";
  const auto installed = choc::json::parse(processor->handleUiMessage(rebuildMessage));
  expect(installed["ok"].getWithDefault<bool>(false),
         "install the topology-diagnostic pipeline");
  expect(processor->setActive(true) == kResultOk,
         "activate the topology-diagnostic test");

  gate_ordering::AudioRig rig(1.0f);
  expect(processor->process(rig.data) == kResultOk,
         "warm the topology-diagnostic test");

  // Each route is first held open on its own: a block is kept in flight, so the
  // route stops inside the window it opened. The gate has to be closed by then,
  // and the engine claimed along with it -- a route that closes the gate without
  // claiming the engine is exactly the one whose every block reports a DSP
  // failure the user never caused.
  const auto expectClaimedWhileNotReady = [&](const char *route,
                                              const std::string &message) {
    PluginProcessorTestAccess::beginSyntheticBlock(*processor);
    std::atomic_bool completed{false};
    std::atomic_bool routeAccepted{false};
    std::thread edit([&] {
      const auto response = choc::json::parse(processor->handleUiMessage(route));
      routeAccepted.store(response["ok"].getWithDefault<bool>(false),
                          std::memory_order_release);
      completed.store(true, std::memory_order_release);
    });
    const auto gateDeadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (PluginProcessorTestAccess::processingReady(*processor) &&
           !completed.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < gateDeadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const auto claims = PluginProcessorTestAccess::controlEngineClaims(*processor);
    const auto ready = PluginProcessorTestAccess::processingReady(*processor);
    PluginProcessorTestAccess::endSyntheticBlock(*processor);
    edit.join();
    expect(routeAccepted.load(std::memory_order_acquire),
           "the held route is accepted once the block leaves");
    expect(!ready && claims != 0u,
           message + " (ready " + std::to_string(ready ? 1 : 0) + ", claims " +
               std::to_string(claims) + ")");
  };
  expectClaimedWhileNotReady(
      rebuildMessage, "pipeline/rebuild claims the engine before it closes the gate");
  expectClaimedWhileNotReady(
      addNodeMessage, "pipeline/updatePlugin claims the engine before it closes the gate");
  expectClaimedWhileNotReady(
      restoreMessage,
      "pipeline/restoreHistory claims the engine before it closes the gate");

  const auto failuresBefore =
      PluginProcessorTestAccess::processFailureSequence(*processor);
  // Decoding a topology message allocates, and the Debug allocation guard is a
  // process-wide depth counter rather than a per-thread one, so an editor
  // thread would abort on a block that happens to be inside the engine. The
  // guard is what the RT-allocation tests measure; here it only has to stop
  // aborting, and nothing in this test reads the violation count.
  effetune::allocation_guard::setAbortOnViolationForTesting(false);
  std::atomic_bool running{true};
  std::atomic_bool accepted{true};
  std::atomic<std::uint64_t> edits{0};
  std::thread editor([&] {
    const std::array<const char *, 3> routes{rebuildMessage, addNodeMessage,
                                             restoreMessage};
    std::size_t step = 0;
    while (running.load(std::memory_order_acquire)) {
      const auto response = choc::json::parse(
          processor->handleUiMessage(routes[step++ % routes.size()]));
      if (!response["ok"].getWithDefault<bool>(false)) {
        accepted.store(false, std::memory_order_release);
        return;
      }
      edits.fetch_add(1, std::memory_order_acq_rel);
    }
  });

  // Nothing is asserted while the editor thread runs: an assertion that throws
  // here would leave it joinable.
  auto rendered = true;
  for (int block = 0; block < 3000; ++block) {
    rendered = rendered && processor->process(rig.data) == kResultOk;
  }
  running.store(false, std::memory_order_release);
  editor.join();
  effetune::allocation_guard::setAbortOnViolationForTesting(true);

  expect(rendered, "process a block while the pipeline topology is edited");
  expect(accepted.load(std::memory_order_acquire),
         "every topology edit is accepted");
  expect(edits.load(std::memory_order_acquire) >= 3u,
         "the editor really ran all three topology routes alongside the blocks");
  const auto reported = hostInfo(*processor);
  std::string reportedMessages;
  for (const auto diagnostic : reported["diagnostics"]) {
    reportedMessages += diagnostic["message"].getWithDefault<std::string>({});
  }
  expect(PluginProcessorTestAccess::processFailureSequence(*processor) ==
                 failuresBefore &&
             reported["diagnostics"].size() == 0,
         "an explicit topology edit raises no user-facing diagnostic: " +
             reportedMessages + " (reason " +
             std::to_string(PluginProcessorTestAccess::lastProcessFailure(*processor)) +
             ")");

  expect(processor->setActive(false) == kResultOk,
         "deactivate the topology-diagnostic test");
  expect(processor->terminate() == kResultOk,
         "terminate the topology-diagnostic test");
}

// A stopped transport is not a stopped audio device, and neither of them is what
// hands the engine to the control service. Hosts keep the audio engine turning
// for live input monitoring and go on sending ordinary blocks; those blocks stage
// the parameter images themselves and apply prepared compensation. When those
// callbacks cease entirely, the control owner must finish any pending work
// without requiring another edit or callback. A settled service claims nothing.
void testPendingWorkIsServicedWhenTheAudioCallbackQuiesces() {
  auto processor = std::make_unique<EffeTuneProcessor>();
  expect(processor->initialize(nullptr) == kResultOk,
         "initialize the stopped-transport service test");
  auto processSetup = setup(48000.0, 64);
  expect(processor->setupProcessing(processSetup) == kResultOk,
         "prepare the stopped-transport service test");
  installLimiterPipeline(*processor, 65);
  expect(processor->getLatencySamples() == 144u,
         "the stopped-transport test begins at three milliseconds");
  expect(processor->setActive(true) == kResultOk,
         "activate the stopped-transport service test");

  gate_ordering::AudioRig rig(0.2f);
  ProcessContext context{};
  context.state = ProcessContext::kPlaying;
  rig.data.processContext = &context;
  const auto renderBlock = [&](const std::string &message) {
    expect(processor->process(rig.data) == kResultOk, message);
    context.projectTimeSamples += rig.data.numSamples;
  };
  for (int block = 0; block < 4; ++block) {
    renderBlock("warm the stopped-transport service test");
  }

  // A lookahead change moves the instance latency, so the compensation plan and
  // the reported latency both go stale.
  auto command = std::make_unique<effetune::vst::AudioCommand>();
  const std::array<float, 6> packed{0.0f, 100.0f, 10.0f, 1.0f, 0.0f, -1.0f};
  expect(PluginProcessorTestAccess::publishParameterImage(
             *processor, *command, 65u, 3039928906u, std::span<const float>(packed)),
         "publish the delay-bearing image while the transport plays");
  renderBlock("stage the delay-bearing image on the audio thread");

  const auto playingCounters = PluginProcessorTestAccess::processCounters(*processor);
  for (int tick = 0; tick < 64; ++tick) {
    renderBlock("keep playing while the refresh is prepared and applied");
    PluginProcessorTestAccess::serviceLatencyUpdates(*processor);
  }
  const auto afterPlaying = PluginProcessorTestAccess::processCounters(*processor);
  expect(afterPlaying.completedBatches - playingCounters.completedBatches == 64u,
         "a playing transport keeps every block inside the engine");
  expect(processor->getLatencySamples() == 480u &&
             !PluginProcessorTestAccess::hasPendingControlWork(*processor),
         "the applied plan converges while playing without taking the engine");

  // The transport stops but the host keeps the audio engine running for live
  // input monitoring, so ordinary blocks keep arriving. The audio thread is
  // still the one staging images and applying plans: a control mutation window
  // opened here would cost a block in the middle of ordinary monitoring.
  context.state = 0;
  const std::array<float, 6> stoppedPacked{0.0f, 100.0f, 2.0f, 1.0f, 0.0f, -1.0f};
  expect(PluginProcessorTestAccess::publishParameterImage(
             *processor, *command, 65u, 3039928906u, std::span<const float>(stoppedPacked)),
         "publish a latency decrease with stopped transport and live monitoring");
  const auto stoppedCounters = PluginProcessorTestAccess::processCounters(*processor);
  for (int tick = 0; tick < 64; ++tick) {
    renderBlock("a stopped transport keeps the audio engine running");
    PluginProcessorTestAccess::serviceLatencyUpdates(*processor);
  }
  const auto afterStopped = PluginProcessorTestAccess::processCounters(*processor);
  expect(afterStopped.completedBatches - stoppedCounters.completedBatches == 64u,
         "a stopped transport with a running audio device keeps every block "
         "inside the engine too");
  expect(processor->getLatencySamples() == 96u &&
             !PluginProcessorTestAccess::hasPendingControlWork(*processor),
         "the decreased plan also converges while transport is stopped");

  expect(PluginProcessorTestAccess::publishParameterImage(
             *processor, *command, 65u, 3039928906u, std::span<const float>(packed)),
         "publish another change just before the callback quiesces");
  renderBlock("capture a plan before callback quiescence");

  // The audio callback goes quiet: the host stopped calling process(). That,
  // and not the transport state, is what hands the engine to the control
  // service, and the service has to reach it on its own timer rather than
  // waiting for the next edit to come along.
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  auto serviced = false;
  while (!serviced && std::chrono::steady_clock::now() < deadline) {
    PluginProcessorTestAccess::serviceLatencyUpdates(*processor);
    serviced = !PluginProcessorTestAccess::hasPendingControlWork(*processor);
    if (!serviced) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }
  expect(serviced,
         "a quiet audio callback lets the control service reach the engine");
  expect(processor->getLatencySamples() == 480u,
         "the deferred latency reaches the host once the callback quiesces");

  // The batch is spent, so no further tick may claim the engine again. A tick
  // that did would wait the held block out instead of returning.
  PluginProcessorTestAccess::beginSyntheticBlock(*processor);
  std::atomic_bool completed{false};
  std::thread service([&] {
    PluginProcessorTestAccess::serviceLatencyUpdates(*processor);
    PluginProcessorTestAccess::serviceLatencyUpdates(*processor);
    completed.store(true, std::memory_order_release);
  });
  const auto claimDeadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(20);
  while (!completed.load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < claimDeadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  const auto completedWhileBlocked = completed.load(std::memory_order_acquire);
  PluginProcessorTestAccess::endSyntheticBlock(*processor);
  service.join();
  expect(completedWhileBlocked,
         "the window does not reopen once the pending batch is serviced");

  const auto settled = PluginProcessorTestAccess::processCounters(*processor);
  for (int block = 0; block < 64; ++block) {
    renderBlock("render while the stopped transport has nothing pending");
    PluginProcessorTestAccess::serviceLatencyUpdates(*processor);
  }
  const auto afterSettled = PluginProcessorTestAccess::processCounters(*processor);
  expect(afterSettled.completedBatches - settled.completedBatches == 64u,
         "a serviced stopped transport turns no further block dry");

  expect(processor->setActive(false) == kResultOk,
         "deactivate the stopped-transport service test");
  expect(processor->terminate() == kResultOk,
         "terminate the stopped-transport service test");
}

// The same drag as testDelayBearingEditsDuringPlaybackKeepEveryBlockWet, with a
// process context whose transport is stopped while the audio device keeps
// delivering rendered blocks. That is the ordinary state of live input
// monitoring, and of every session where play was never pressed, so it is the
// most common condition a knob is dragged in -- not an exotic one. Each of those
// blocks stages the images itself, so the control service needs nothing from the
// engine; a service that read the stopped transport as an idle audio thread
// would instead open a window on every step of the gesture and turn the whole
// drag into audible dry-wet stutter.
void testDelayBearingEditsWithAStoppedTransportKeepEveryBlockWet() {
  auto processor = std::make_unique<EffeTuneProcessor>();
  expect(processor->initialize(nullptr) == kResultOk,
         "initialize the stopped-transport drag test");
  auto processSetup = setup(48000.0, 64);
  expect(processor->setupProcessing(processSetup) == kResultOk,
         "prepare the stopped-transport drag test");
  installLimiterPipeline(*processor, 67);
  expect(processor->setActive(true) == kResultOk,
         "activate the stopped-transport drag test");

  gate_ordering::AudioRig rig(0.2f);
  ProcessContext context{};
  context.state = 0;
  rig.data.processContext = &context;
  expect(processor->process(rig.data) == kResultOk,
         "warm the stopped-transport drag test");

  const auto before = PluginProcessorTestAccess::processCounters(*processor);
  const auto failuresBefore =
      PluginProcessorTestAccess::processFailureSequence(*processor);
  std::atomic_bool running{true};
  std::atomic_bool published{true};
  std::atomic<std::uint64_t> serviceTicks{0};
  // Reuse the mailbox payload throughout the gesture.
  auto command = std::make_unique<effetune::vst::AudioCommand>();
  std::array<float, 6> packed{0.0f, 100.0f, 3.0f, 1.0f, 0.0f, -1.0f};
  std::thread control([&] {
    std::uint32_t step = 0;
    while (running.load(std::memory_order_acquire)) {
      // Lookahead is the delay-bearing parameter: every distinct value moves
      // the instance latency and therefore the compensation plan.
      packed[2] = 1.0f + static_cast<float>(step++ % 8u);
      if (!PluginProcessorTestAccess::publishParameterImage(
              *processor, *command, 67u, 3039928906u, std::span<const float>(packed))) {
        published.store(false, std::memory_order_release);
        return;
      }
      // Both callers a real session runs for the whole gesture: the 50 ms
      // control-service timer, and the editor's frame-rate host-info and
      // telemetry polls.
      PluginProcessorTestAccess::serviceLatencyUpdates(*processor);
      PluginProcessorTestAccess::pollLatencyUpdatesFromUi(*processor);
      serviceTicks.fetch_add(1, std::memory_order_acq_rel);
    }
  });

  // Nothing is asserted while the control thread runs: an assertion that threw
  // here would leave it joinable.
  constexpr std::uint64_t blocks = 1500;
  auto rendered = true;
  for (std::uint64_t block = 0; block < blocks; ++block) {
    rendered = rendered && processor->process(rig.data) == kResultOk;
    context.projectTimeSamples += rig.data.numSamples;
  }
  running.store(false, std::memory_order_release);
  control.join();

  const auto after = PluginProcessorTestAccess::processCounters(*processor);
  expect(rendered,
         "process a block while a delay-bearing parameter is dragged with the "
         "transport stopped");
  expect(published.load(std::memory_order_acquire),
         "every delay-bearing image reached the mailbox");
  expect(serviceTicks.load(std::memory_order_acquire) != 0,
         "the control service really ran alongside the audio callback");
  expect(after.batchAttempts - before.batchAttempts == blocks &&
             after.completedBatches - before.completedBatches == blocks,
         "a drag with the transport stopped and the device running turns no "
         "block into dry audio (" +
             std::to_string(after.batchAttempts - before.batchAttempts) + " of " +
             std::to_string(blocks) + " blocks reached the engine)");
  expect(PluginProcessorTestAccess::processFailureSequence(*processor) ==
             failuresBefore,
         "and raises no deferred diagnostic");
  for (int block = 0; block < 8; ++block) {
    expect(processor->process(rig.data) == kResultOk,
           "settle the stopped-transport drag without stopping callbacks");
    PluginProcessorTestAccess::serviceLatencyUpdates(*processor);
  }
  expect(!PluginProcessorTestAccess::hasPendingControlWork(*processor),
         "the final stopped-transport compensation converges while blocks keep coming");

  expect(processor->setActive(false) == kResultOk,
         "deactivate the stopped-transport drag test");
  expect(processor->terminate() == kResultOk,
         "terminate the stopped-transport drag test");
}

void testLiveLatencyCommitAlignsWetBypassAndHost() {
  for (const bool parallel : {false, true}) {
    for (const int transport : {0, 1, 2}) {
      auto processor = std::make_unique<EffeTuneProcessor>();
      expect(processor->initialize(nullptr) == kResultOk, "initialize live latency commit");
      auto processSetup = setup(48000, 64);
      expect(processor->setupProcessing(processSetup) == kResultOk, "prepare live latency commit");
      if (parallel) {
        installParallelLimiterPipeline(*processor);
      } else {
        installLimiterPipeline(*processor, 41);
      }
      auto *handler = new TestComponentHandler();
      handler->latencyProbe = processor.get();
      expect(processor->setComponentHandler(handler) == kResultOk, "install applied latency probe");
      expect(processor->setActive(true) == kResultOk, "activate live latency commit");
      gate_ordering::AudioRig rig(0);
      ProcessContext context{};
      context.state = transport == 1 ? ProcessContext::kPlaying : 0;
      rig.data.processContext = transport == 0 ? nullptr : &context;
      const auto render = [&] {
        tresult result;
        {
          effetune::allocation_guard::Scope noAudioAllocation;
          result = processor->process(rig.data);
        }
        expect(result == kResultOk, "render through the live latency handoff");
        context.projectTimeSamples += rig.data.numSamples;
      };
      for (int block = 0; block < 16; ++block) {
        render();
      }
      auto command = std::make_unique<effetune::vst::AudioCommand>();
      const auto publish = [&](const float lookahead) {
        const std::array<float, 6> image{0, 100, lookahead, 1, 0, -1};
        expect(PluginProcessorTestAccess::publishParameterImage(
                   *processor, *command, 41, 3039928906u, image), "publish live lookahead");
      };
      const auto settlePlan = [&] {
        for (int block = 0; block < 8; ++block) {
          render();
          PluginProcessorTestAccess::serviceLatencyUpdates(*processor);
        }
        expect(!PluginProcessorTestAccess::hasPendingControlWork(*processor),
               "the matching plan converges with uninterrupted callbacks");
      };
      PluginProcessorTestAccess::failPipelinePlanRefreshes(*processor, 1);
      publish(7);
      render();
      PluginProcessorTestAccess::serviceLatencyUpdates(*processor);
      expect(processor->getLatencySamples() == 144u &&
                 PluginProcessorTestAccess::enginePipelineLatency(*processor) == 144u,
             "a preparation failure leaves both applied latency and bypass unchanged");
      expect(pumpMainThreadUntil([&] {
               render();
               PluginProcessorTestAccess::serviceLatencyUpdates(*processor);
               return processor->getLatencySamples() == 336u;
             }, std::chrono::milliseconds(500)),
             "bounded preparation retry recovers while callbacks keep running");
      settlePlan();
      const auto beforeStale = processor->getLatencySamples();
      // Prepare 10 ms, then stage a newer value at the very apply boundary.
      // Neither dry delay nor public latency may adopt the obsolete result.
      publish(10);
      render();
      PluginProcessorTestAccess::serviceLatencyUpdates(*processor);
      publish(5);
      render();
      expect(processor->getLatencySamples() == beforeStale,
             "a stale prepared result cannot advance public or dry latency");
      PluginProcessorTestAccess::serviceLatencyUpdates(*processor);
      settlePlan();
      expect(processor->getLatencySamples() == 240u, "a fresh result replaces the stale update");

      const auto before = PluginProcessorTestAccess::processCounters(*processor);
      std::uint64_t renderedBlocks = 0;
      for (const float lookahead : {10.0f, 2.0f, 1.0f}) {
        publish(lookahead);
        settlePlan();
        renderedBlocks += 8;
        const auto expected = static_cast<std::uint32_t>(
            (parallel ? std::max(lookahead, 3.0f) : lookahead) * 48);
        expect(processor->getLatencySamples() == expected,
               "host reports the successful applied increase or decrease");
        for (const bool bypass : {false, true}) {
          ParameterChanges changes(1);
          int32 index = 0;
          auto *queue = changes.addParameterData(kBypassParameterId, index);
          expect(queue && queue->addPoint(0, bypass ? 1.0 : 0.0, index) == kResultTrue,
                 "set master bypass at the block boundary");
          rig.data.inputParameterChanges = &changes;
          render();
          ++renderedBlocks;
          rig.data.inputParameterChanges = nullptr;
          rig.inputLeft.fill(0);
          rig.inputRight.fill(0);
          for (int block = 0; block < 16; ++block) {
            render();
            ++renderedBlocks;
          }
          std::array<std::uint32_t, 2> peaks{};
          std::array<float, 2> magnitudes{};
          for (std::uint32_t block = 0; block < 12; ++block) {
            rig.inputLeft.fill(0);
            rig.inputRight.fill(0);
            if (block == 0) {
              rig.inputLeft[0] = 0.25f;
              rig.inputRight[0] = 0.25f;
            }
            render();
            ++renderedBlocks;
            for (std::uint32_t channel = 0; channel < 2; ++channel) {
              for (std::uint32_t frame = 0; frame < 64; ++frame) {
                const auto value = std::abs(rig.outputChannels[channel][frame]);
                if (value > magnitudes[channel]) {
                  magnitudes[channel] = value;
                  peaks[channel] = block * 64 + frame;
                }
              }
            }
          }
          expect(peaks[0] == expected && peaks[1] == expected &&
                     magnitudes[0] > 0.2f && magnitudes[1] > 0.2f,
                 "wet and master-bypass impulses match applied host latency");
        }
      }
      const auto after = PluginProcessorTestAccess::processCounters(*processor);
      expect(after.completedBatches - before.completedBatches == renderedBlocks &&
                 after.processFailures == before.processFailures,
             "every block is processed across preparation, apply and retirement");
      expect(pumpMainThreadUntil([&] {
               render();
               PluginProcessorTestAccess::serviceLatencyUpdates(*processor);
               return handler->latencyRestartCount != 0;
             }, std::chrono::milliseconds(500)),
             "the control thread notifies latency while callbacks continue");
      expect(!handler->latencyMismatch, "every host notification observes the applied plan");
      handler->latencyProbe = nullptr;
      expect(processor->setComponentHandler(nullptr) == kResultOk, "remove applied latency probe");
      expect(processor->setActive(false) == kResultOk, "deactivate live latency commit");
      expect(processor->terminate() == kResultOk, "terminate live latency commit");
    }
  }
}

// A plan or latency refresh that keeps failing is control work, and the audio
// failure burst is re-armed by every successful block. Putting the refresh on
// that burst therefore raises a fresh warning at every retry, and it overwrites
// the reason an audio-side failure that is still pending would have reported.
void testRefreshFailuresStayOffTheAudioDiagnosticBurst() {
  auto processor = std::make_unique<EffeTuneProcessor>();
  expect(processor->initialize(nullptr) == kResultOk,
         "initialize the refresh-diagnostic test");
  auto processSetup = setup(48000.0, 64);
  expect(processor->setupProcessing(processSetup) == kResultOk,
         "prepare the refresh-diagnostic test");
  installLimiterPipeline(*processor, 66);
  expect(processor->setActive(true) == kResultOk,
         "activate the refresh-diagnostic test");

  gate_ordering::AudioRig rig(0.2f);
  // Successful blocks are interleaved with the retries: each of them used to
  // re-arm the burst gate the next failing retry then walked through. A retry
  // only reaches the engine once the callback has gone quiet, so the pause below
  // is what lets the two alternate.
  ProcessContext context{};
  context.state = 0;
  rig.data.processContext = &context;
  for (int block = 0; block < 4; ++block) {
    expect(processor->process(rig.data) == kResultOk,
           "warm the refresh-diagnostic test");
  }

  const auto countDiagnostics = [&](const choc::value::Value &info,
                                    const std::string_view code) {
    auto found = 0;
    for (const auto diagnostic : info["diagnostics"]) {
      if (diagnostic["code"].getWithDefault<std::string>({}) == code) {
        ++found;
      }
    }
    return found;
  };

  PluginProcessorTestAccess::failPipelinePlanRefreshes(*processor, 3);
  updateLimiterPlugin(*processor, 66, 10.0f);
  auto refreshNotices = 0;
  auto audioNotices = 0;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  while (PluginProcessorTestAccess::remainingPipelinePlanRefreshFailures(
             *processor) != 0u &&
         std::chrono::steady_clock::now() < deadline) {
    expect(processor->process(rig.data) == kResultOk,
           "render a successful block between two failing retries");
    // Samples the block that just ran, so the service after the pause is the
    // one that observes the callback stop delivering blocks.
    PluginProcessorTestAccess::serviceLatencyUpdates(*processor);
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    PluginProcessorTestAccess::serviceLatencyUpdates(*processor);
    const auto info = hostInfo(*processor);
    refreshNotices += countDiagnostics(info, "latency-plan-refresh-failed");
    audioNotices += countDiagnostics(info, "audio-processing-failure");
  }
  expect(PluginProcessorTestAccess::remainingPipelinePlanRefreshFailures(
             *processor) == 0u,
         "the injected refresh failures were all attempted");
  expect(refreshNotices == 1,
         "a refresh that keeps failing is reported to the user exactly once");
  expect(audioNotices == 0,
         "and a control-thread failure never reports itself as an audio failure");

  // An audio-side failure that is still pending must keep its own reason even
  // though a control-thread refresh failed after it.
  PluginProcessorTestAccess::failPipelinePlanRefreshes(*processor, 2);
  updateLimiterPlugin(*processor, 66, 5.0f);
  rig.data.symbolicSampleSize = kSample64;
  expect(processor->process(rig.data) == kResultOk,
         "an unsupported buffer opens the audio failure burst");
  rig.data.symbolicSampleSize = kSample32;
  expect(pumpMainThreadUntil(
             [&] {
               return PluginProcessorTestAccess::
                          remainingPipelinePlanRefreshFailures(*processor) == 0u;
             },
             std::chrono::seconds(3)),
         "the control service attempts the refresh after the failed block");
  const auto info = hostInfo(*processor);
  expect(countDiagnostics(info, "audio-processing-failure") == 1,
         "the pending audio failure is still reported");
  for (const auto diagnostic : info["diagnostics"]) {
    if (diagnostic["code"].getWithDefault<std::string>({}) ==
        "audio-processing-failure") {
      expect(diagnostic["message"].getWithDefault<std::string>({}).find(
                 "audio buffer") != std::string::npos,
             "a control-thread failure never overwrites the audio failure "
             "reason");
    }
  }

  expect(processor->setActive(false) == kResultOk,
         "deactivate the refresh-diagnostic test");
  expect(processor->terminate() == kResultOk,
         "terminate the refresh-diagnostic test");
}

// A host that refuses the edit transaction is not saying the user's edit should
// not take effect: it is saying it will not record it. The plug-in therefore
// adopts the value as its own DSP value either way, so there is only ever one
// value in play -- the DSP, the runtime image, the state document, the host
// parameter and the editor all end on the one the user asked for -- and nothing
// is answered back to the editor for it to put right.
void testHostRefusedGestureStillAdoptsTheUserValue() {
  auto processor = std::make_unique<EffeTuneProcessor>();
  expect(processor->initialize(nullptr) == kResultOk,
         "initialize the refused-gesture test");
  auto *handler = new TestComponentHandler();
  expect(processor->setComponentHandler(handler) == kResultOk,
         "install the refused-gesture handler");
  auto processSetup = setup(48000.0, 64);
  expect(processor->setupProcessing(processSetup) == kResultOk,
         "prepare the refused-gesture test");
  const auto installed = choc::json::parse(processor->handleUiMessage(
      R"({"type":"pipeline/rebuild","payload":{"pipeline":"A","plugins":[)"
      R"({"id":105,"type":"DCOffsetPlugin","name":"DC Offset","enabled":true,)"
      R"("parameters":{"of":0},"wasmParams":[0],)"
      R"("wasmParamsHash":1104945464},)"
      R"({"id":106,"type":"DCOffsetPlugin","name":"Unbound","enabled":true,)"
      R"("parameters":{"of":0},"wasmParams":[0],)"
      R"("wasmParamsHash":1104945464}]}})"));
  expect(installed["ok"].getWithDefault<bool>(false),
         "install the refused-gesture pipeline");
  const effetune::vst::AutomationTargetIdentity identity{
      'A', 105, "DCOffsetPlugin", "of", 0};
  const auto parameterId = boundAutomationParameterId(*processor, identity);
  const auto slot =
      PluginProcessorTestAccess::activeAutomationSlot(*processor, identity);
  expect(slot.has_value(), "the refused-gesture target owns a lane");
  expect(processor->setActive(true) == kResultOk,
         "activate the refused-gesture test");

  // The value the registry, the scheduler and therefore the DSP hold before the
  // user touches the control again.
  expect(PluginProcessorTestAccess::applyAutomationEdit(*processor, identity, 0.25),
         "adopt the value the lane starts on");

  handler->refuseEdits = true;
  handler->clearEditLog();
  const auto refused = choc::json::parse(processor->handleUiMessage(
      R"({"type":"pipeline/updatePlugin","payload":{"pipeline":"A",)"
      R"("plugin":{"id":105,"type":"DCOffsetPlugin","name":"DC Offset",)"
      R"("enabled":true,"parameters":{"of":0.5},"wasmParams":[0.5],)"
      R"("wasmParamsHash":1104945464},"automationEdits":[{"pipeline":"A",)"
      R"("pluginId":105,"pluginType":"DCOffsetPlugin","parameterKey":"of",)"
      R"("elementIndex":0,"normalized":0.75}]}})"));
  expect(refused["ok"].getWithDefault<bool>(false),
         "the image the refused gesture travelled with is still accepted");
  // The two members the editor used to converge on. Neither may come back: an
  // answer that reported the refusal is what asked the editor to roll a knob
  // back under the user's hand, and one that named a value the plug-in had put
  // back is what gave that rollback a second authority to converge on.
  expect(!refused["automationEditsAccepted"].isArray() &&
             !refused["automationEditsRestored"].isArray(),
         "the answer says nothing about the gesture, so the editor is never "
         "asked to put anything back");
  // Two values are offered inside that touch: the value the lane held when the
  // hand arrived, stated at the open, and the value the user made. No block
  // runs between them, so the second one is flushed by the close. The host
  // refuses both, and neither refusal changes what the plug-in adopts.
  expect(handler->stepCount(TestComponentHandler::EditStep::begin) == 1u &&
             handler->performedEditCount == 2u &&
             std::abs(handler->performedEdits[0].value - 0.25) < 1.0e-9 &&
             std::abs(handler->performedEdits[1].value - 0.75) < 1.0e-9 &&
             handler->stepCount(TestComponentHandler::EditStep::end) == 1u,
         "the host was still offered the edit inside a complete touch");
  expect(std::abs(processor->getParamNormalized(parameterId) - 0.75) < 1.0e-9,
         "and the host parameter reports the value the user asked for, not the "
         "one the failed transaction put back");

  // Read before any block runs. This is the window a save can be taken in while
  // the transport is stopped, and it is exactly where the two values used to
  // disagree.
  const auto stored = choc::json::parse(processor->handleUiMessage(
      R"({"type":"storage/readFile","payload":{"path":"pipeline-state.json"}})"));
  const auto persisted =
      choc::json::parse(stored["content"].getWithDefault<std::string>({}));
  expect(std::abs(persisted["pipelineA"][0]["parameters"]["of"]
                      .getWithDefault<double>(0.0) -
                  0.5) < 1.0e-6,
         "the state document holds the value the user asked for");

  gate_ordering::AudioRig rig(0.0f);
  expect(processor->process(rig.data) == kResultOk,
         "render a block after the refused gesture");
  expect(std::abs(PluginProcessorTestAccess::playedAutomationValue(*processor, *slot) -
                  0.75) < 1.0e-9,
         "a refused gesture still leaves the DSP on the value the user asked for");
  expect(std::abs(PluginProcessorTestAccess::runtimePackedParameter(*processor, 105, 0) -
                  0.5f) < 1.0e-6f,
         "the block pins that same value into the runtime image");
  for (const auto sample : rig.outputLeft) {
    expect(std::abs(sample - 0.5f) < 1.0e-6f, "and the audio carries it too");
  }

  // The other half of the same case. A refused gesture on a target that owned no
  // lane still claims one and still adopts: refusing to record automation is not
  // refusing the edit.
  const effetune::vst::AutomationTargetIdentity unbound{
      'A', 106, "DCOffsetPlugin", "of", 0};
  expect(!PluginProcessorTestAccess::activeAutomationSlot(*processor, unbound)
              .has_value(),
         "the bind-then-refuse target starts without an automation slot");
  const auto boundThenRefused = choc::json::parse(processor->handleUiMessage(
      R"({"type":"pipeline/updatePlugin","payload":{"pipeline":"A",)"
      R"("plugin":{"id":106,"type":"DCOffsetPlugin","name":"Unbound",)"
      R"("enabled":true,"parameters":{"of":0.5},"wasmParams":[0.5],)"
      R"("wasmParamsHash":1104945464},"automationEdits":[{"pipeline":"A",)"
      R"("pluginId":106,"pluginType":"DCOffsetPlugin","parameterKey":"of",)"
      R"("elementIndex":0,"normalized":0.75}]}})"));
  expect(boundThenRefused["ok"].getWithDefault<bool>(false),
         "the image the bind-then-refuse gesture travelled with is accepted");
  expect(!boundThenRefused["automationEditsAccepted"].isArray() &&
             !boundThenRefused["automationEditsRestored"].isArray(),
         "and it is answered with nothing to reconcile either");
  const auto boundSlot =
      PluginProcessorTestAccess::activeAutomationSlot(*processor, unbound);
  expect(boundSlot.has_value(),
         "a gesture the host will not record still claims the lane it asked for");
  expect(processor->process(rig.data) == kResultOk,
         "render a block after the bind-then-refuse gesture");
  expect(std::abs(PluginProcessorTestAccess::playedAutomationValue(*processor,
                                                                   *boundSlot) -
                  0.75) < 1.0e-9,
         "and the newly bound lane plays the value the user asked for");

  handler->refuseEdits = false;
  expect(processor->setActive(false) == kResultOk,
         "deactivate the refused-gesture test");
  expect(processor->setComponentHandler(nullptr) == kResultOk,
         "remove the refused-gesture handler");
  handler->release();
  expect(processor->terminate() == kResultOk,
         "terminate the refused-gesture test");
}

#if defined(EFFETUNE_AUTOMATION_HOST_GATE_FIXTURE)
void testHostGateFixturePublishesThreeParameters() {
  auto processor = std::make_unique<EffeTuneProcessor>();
  expect(processor->initialize(nullptr) == kResultOk,
         "initialize the automation host-gate fixture");
  expect(processor->getParameterCount() ==
             static_cast<int32>(effetune::vst::kAutomationSlotCount + 1u),
         "the host-gate fixture preserves the fixed 256-slot bank");
  for (int32 index = 1;
       index <= static_cast<int32>(effetune::vst::kAutomationSlotCount); ++index) {
    ParameterInfo unbound{};
    expect(processor->getParameterInfo(index, unbound) == kResultOk &&
               (unbound.flags & ParameterInfo::kCanAutomate) != 0 &&
               (unbound.flags &
                (ParameterInfo::kIsHidden | ParameterInfo::kIsReadOnly)) == 0,
           "every host-gate slot is automatable before anything is bound");
  }
  constexpr std::array expectedValues{0.375, 0.625, 0.75};
  const std::array<effetune::vst::AutomationTargetIdentity, 3> identities{
      {{'A', 1001, "DCOffsetPlugin", "of", 0},
       {'A', 1002, "DCOffsetPlugin", "of", 0},
       {'B', 1003, "DCOffsetPlugin", "of", 0}}};
  for (std::size_t index = 0; index < identities.size(); ++index) {
    expect(PluginProcessorTestAccess::bindAutomationSlot(*processor,
                                                         identities[index]) ==
               std::optional<std::uint32_t>{static_cast<std::uint32_t>(index)},
           "the host-gate fixture binds its targets in slot order");
    ParameterInfo info{};
    expect(processor->getParameterInfo(static_cast<int32>(index) + 1, info) == kResultOk &&
               info.id == kFirstAutomationParameterId +
                              static_cast<ParamID>(index) &&
               (info.flags & ParameterInfo::kCanAutomate) != 0 &&
               (info.flags &
                (ParameterInfo::kIsHidden | ParameterInfo::kIsReadOnly)) == 0 &&
               info.title[0] != 0 && info.stepCount == 0 &&
               info.defaultNormalizedValue == 0.5 &&
               processor->getParamNormalized(info.id) == expectedValues[index],
           "publish active host-gate metadata and parametersJson-derived value");
  }
  expect(processor->terminate() == kResultOk,
         "terminate the automation host-gate fixture");
}
#endif

// The DC offset target the touch tests move. Its normalization is linear over
// [-1, 1], so a normalized value maps to an audible offset of 2n - 1 and the
// rendered block reports exactly which value reached the DSP.
constexpr double kTouchOffsetForNormalized(const double normalized) {
  return normalized * 2.0 - 1.0;
}

struct TouchFixture {
  std::unique_ptr<EffeTuneProcessor> processor;
  TestComponentHandler *handler = nullptr;
  effetune::vst::AutomationTargetIdentity identity{'A', 41, "DCOffsetPlugin", "of", 0};
  ParamID parameterId = 0;
};

// A processor with one bound DC offset target and the user's hand on it: the
// touch is open, and nothing has ended it yet.
[[nodiscard]] TouchFixture openTouchFixture(const std::string &what,
                                            const std::uint32_t pluginId = 41) {
  TouchFixture fixture;
  fixture.identity.pluginId = pluginId;
  fixture.processor = std::make_unique<EffeTuneProcessor>();
  expect(fixture.processor->initialize(nullptr) == kResultOk, "initialize " + what);
  fixture.handler = new TestComponentHandler();
  expect(fixture.processor->setComponentHandler(fixture.handler) == kResultOk,
         "install the component handler for " + what);
  auto processSetup = setup(48000.0, 64);
  expect(fixture.processor->setupProcessing(processSetup) == kResultOk,
         "prepare " + what);
  const auto installed = choc::json::parse(fixture.processor->handleUiMessage(
      std::string{R"({"type":"pipeline/rebuild","payload":{"pipeline":"A","plugins":[)"
                  R"({"id":)"} +
      std::to_string(pluginId) +
      R"(,"type":"DCOffsetPlugin","name":"DC Offset","enabled":true,)"
      R"("parameters":{"of":0},"wasmParams":[0],)"
      R"("wasmParamsHash":1104945464}]}})"));
  expect(installed["ok"].getWithDefault<bool>(false), "install the pipeline for " + what);
  fixture.parameterId = boundAutomationParameterId(*fixture.processor, fixture.identity);
  // Binding republishes the whole parameter bank, so the log is cleared here:
  // what the tests measure is the shape of the touch, not the notification that
  // preceded it.
  fixture.handler->clearEditLog();
  expect(PluginProcessorTestAccess::applyAutomationEdit(
             *fixture.processor, fixture.identity, 0.875,
             {/*bindIfUnbound=*/true, /*beginGesture=*/true, /*endGesture=*/false}),
         "open the touch for " + what);
  expect(PluginProcessorTestAccess::hostGestureOpen(*fixture.processor,
                                                     fixture.parameterId),
         "the open touch is published for " + what);
  expect(fixture.handler->stepCount(TestComponentHandler::EditStep::begin) == 1 &&
             fixture.handler->stepCount(TestComponentHandler::EditStep::end) == 0,
         "the opened touch is begun once and not ended for " + what);
  return fixture;
}

// One process() call and nothing else: the boundary the value that opens a
// gesture waits for. It renders a block of silence with no host envelope in it,
// so the only thing it changes is that the boundary has now happened.
//
// Crossing it reports nothing to the host by itself, and may not:
// beginEdit/performEdit/endEdit belong to the UI/controller thread, so the
// audio callback only publishes the epoch. A control thread has to observe it,
// which is what the two helpers below stand in for.
void crossBlockBoundary(EffeTuneProcessor &processor, const std::string &what) {
  gate_ordering::AudioRig rig(0.0f);
  ProcessContext context{};
  context.state = ProcessContext::kPlaying;
  context.projectTimeSamples = 0;
  rig.data.processContext = &context;
  expect(processor.process(rig.data) == kResultOk,
         "cross a process() boundary for " + what);
}

// The 50 ms control-service tick, on the thread that is allowed to call into
// the host. It is a carrier and not a trigger: it releases a held value only
// once a process() boundary has already moved the epoch under it.
void observeBoundaryOnControlThread(EffeTuneProcessor &processor) noexcept {
  PluginProcessorTestAccess::serviceHeldHostEdits(processor);
}

// The pair a drag that has stopped moving takes: the block publishes the
// boundary, the control tick observes it and reports the value.
void crossBlockBoundaryAndObserve(EffeTuneProcessor &processor,
                                  const std::string &what) {
  crossBlockBoundary(processor, what);
  observeBoundaryOnControlThread(processor);
}

// With Read enabled the host plays its recorded curve back into the very slot
// the user is holding. Ingesting that queue replaces the adopted value before
// the first sample of the block is rendered, so the knob moves and nothing is
// heard. An open touch has to take the slot away from the host input entirely.
void testOpenTouchOutranksHostAutomationInput() {
  auto fixture = openTouchFixture("the touch precedence test");
  auto &processor = *fixture.processor;
  const auto slot =
      PluginProcessorTestAccess::activeAutomationSlot(processor, fixture.identity);
  expect(slot.has_value(), "the touched target holds a slot");
  expect(processor.setActive(true) == kResultOk, "activate the touch precedence test");

  std::array<float, 64> inputLeft{};
  std::array<float, 64> inputRight{};
  std::array<float, 64> outputLeft{};
  std::array<float, 64> outputRight{};
  Sample32 *inputChannels[]{inputLeft.data(), inputRight.data()};
  Sample32 *outputChannels[]{outputLeft.data(), outputRight.data()};
  AudioBusBuffers input{};
  input.numChannels = 2;
  input.channelBuffers32 = inputChannels;
  AudioBusBuffers output{};
  output.numChannels = 2;
  output.channelBuffers32 = outputChannels;
  ProcessData data{};
  data.symbolicSampleSize = kSample32;
  data.numSamples = 64;
  data.numInputs = 1;
  data.numOutputs = 1;
  data.inputs = &input;
  data.outputs = &output;

  // One block, with the host playing back one value of its own into the slot.
  const auto playBlock = [&](const double hostNormalized) {
    ParameterChanges changes(1);
    int32 queueIndex = 0;
    auto *queue = changes.addParameterData(fixture.parameterId, queueIndex);
    int32 pointIndex = 0;
    expect(queue != nullptr && queue->addPoint(0, hostNormalized, pointIndex) == kResultTrue,
           "play a host automation point into the touched slot");
    data.inputParameterChanges = &changes;
    outputLeft.fill(0.0f);
    outputRight.fill(0.0f);
    expect(processor.process(data) == kResultOk, "process the host-automated block");
    data.inputParameterChanges = nullptr;
  };
  const auto expectRendered = [&](const double normalized, const std::string &message) {
    const auto expected = static_cast<float>(kTouchOffsetForNormalized(normalized));
    for (const auto sample : outputLeft) {
      expect(std::abs(sample - expected) < 1.0e-6f, message);
    }
    expect(std::abs(PluginProcessorTestAccess::runtimePackedParameter(
                        processor, fixture.identity.pluginId, 0) -
                    expected) < 1.0e-6f,
           message + " -- packed parameter");
  };

  // Every block carries a different host value, which is what a Read-enabled
  // host does while the user drags: the touched value has to survive all of
  // them, not merely the first.
  for (int block = 0; block < 8; ++block) {
    playBlock(0.05 + 0.05 * block);
    expectRendered(0.875, "the touched value outranks the host automation input");
    expect(std::abs(PluginProcessorTestAccess::playedAutomationValue(processor, *slot) -
                    0.875) < 1.0e-9,
           "the scheduler keeps playing the touched value");
  }

  expect(PluginProcessorTestAccess::applyAutomationEdit(
             processor, fixture.identity, 0.875,
             {/*bindIfUnbound=*/true, /*beginGesture=*/false, /*endGesture=*/true}),
         "release the touch");
  expect(!PluginProcessorTestAccess::hostGestureOpen(processor, fixture.parameterId),
         "the released touch is no longer published");

  // The host now plays back the curve it has just recorded, which is exactly
  // what the released control should follow.
  playBlock(0.125);
  expectRendered(0.125, "host automation resumes on the block after the release");

  expect(processor.setActive(false) == kResultOk, "deactivate the touch precedence test");
  expect(processor.terminate() == kResultOk, "terminate the touch precedence test");
  fixture.handler->release();
}

// Finding 1 of the measured Cakewalk Sonar traces, stated as a test. On endEdit
// the host punches out and resumes streaming the envelope it already had --
// continuously, on every block. In trace A the value 0.586904764, bit-identical
// to what the lane held before the drag, arrives on the first block after the
// release and on every block after that. Anything the plug-in tries to hold
// against that stream survives exactly one block and then loses, so its only
// effect is to make that one block disagree with the host. The release
// therefore hands the slot straight back, on the first block, with no lag.
//
// Replaces testReleasedDragRendersTheSameOnTheWritePassAndOnPlayback, which
// asserted the opposite: that the released value stands while the host restates
// the lane it was recorded over. The traces show that claim losing on the very
// first block after every release and never once winning.
void testAReleasedTouchHandsTheSlotBackOnTheNextBlock() {
  constexpr double kOpeningValue = 0.875;
  constexpr double kLaneValue = 0.25;
  constexpr double kDragEndValue = 0.75;
  auto fixture = openTouchFixture("the release hand-back test", 121);
  auto &processor = *fixture.processor;
  const auto slot =
      PluginProcessorTestAccess::activeAutomationSlot(processor, fixture.identity);
  expect(slot.has_value(), "the released target holds a slot");
  expect(processor.setActive(true) == kResultOk, "activate the release hand-back test");

  gate_ordering::AudioRig rig(0.0f);
  ProcessContext context{};
  context.state = ProcessContext::kPlaying;
  context.projectTimeSamples = 0;
  rig.data.processContext = &context;
  // One block at the transport's current position, with the host streaming the
  // envelope it already holds for this slot into it.
  const auto playBlock = [&](const std::string &what) {
    ParameterChanges changes(1);
    int32 queueIndex = 0;
    auto *queue = changes.addParameterData(fixture.parameterId, queueIndex);
    int32 pointIndex = 0;
    expect(queue != nullptr &&
               queue->addPoint(0, kLaneValue, pointIndex) == kResultTrue,
           "emit the host's envelope for " + what);
    rig.data.inputParameterChanges = &changes;
    rig.outputLeft.fill(0.0f);
    rig.outputRight.fill(0.0f);
    expect(processor.process(rig.data) == kResultOk, "render " + what);
    rig.data.inputParameterChanges = nullptr;
    context.projectTimeSamples += rig.data.numSamples;
  };
  const auto renderedOffset = [&] { return static_cast<double>(rig.outputLeft[0]); };

  // The hand is down. The envelope the host keeps streaming is ignored for as
  // long as that is true, and only for as long as that is true.
  playBlock("the first block of the take");
  expect(std::abs(renderedOffset() - kTouchOffsetForNormalized(kOpeningValue)) < 1.0e-6,
         "the open touch outranks the envelope the host is streaming");
  expect(PluginProcessorTestAccess::applyAutomationEdit(
             processor, fixture.identity, kDragEndValue,
             {/*bindIfUnbound=*/true, /*beginGesture=*/false, /*endGesture=*/false}),
         "drag inside the open touch");
  playBlock("the second block of the take");
  expect(std::abs(renderedOffset() - kTouchOffsetForNormalized(kDragEndValue)) < 1.0e-6,
         "the dragged value is heard while the host records it");

  // Mouse-up.
  expect(PluginProcessorTestAccess::applyAutomationEdit(
             processor, fixture.identity, kDragEndValue,
             {/*bindIfUnbound=*/true, /*beginGesture=*/false, /*endGesture=*/true}),
         "release the drag");
  expect(!PluginProcessorTestAccess::hostGestureOpen(processor, fixture.parameterId),
         "the released drag leaves no touch open");

  // The very next block. Not the one after it.
  playBlock("the first block after the release");
  const auto rendered = renderedOffset();
  expect(std::abs(rendered - kTouchOffsetForNormalized(kLaneValue)) < 1.0e-6,
         "the first block after the release already renders what the host is "
         "streaming -- it rendered " +
             std::to_string(rendered) + " instead of " +
             std::to_string(kTouchOffsetForNormalized(kLaneValue)) +
             ", so the slot was withheld from the host for a block it no longer "
             "had any claim on");
  expect(std::abs(PluginProcessorTestAccess::playedAutomationValue(processor, *slot) -
                  kLaneValue) < 1.0e-9,
         "and the scheduler is on the host's value from that same block");

  expect(processor.setActive(false) == kResultOk,
         "deactivate the release hand-back test");
  expect(processor.terminate() == kResultOk, "terminate the release hand-back test");
  fixture.handler->release();
}

// The second interface a host states a lane through. The SDK is explicit about
// what it may do: "The controller must never pass this value-change back to the
// host via the IComponentHandler. It should update the according GUI element(s)
// only!" (ivsteditcontroller.h:522-524). EffeTuneProcessor derives from
// SingleComponentEffect, so it is both processor and controller and the rule
// binds it directly. The only thing that may hold this interface off is the
// hand on the control, and only while the hand is down.
//
// Replaces testARestatedConstantHostCurveMatchesAnImpliedOne, which measured
// the epsilon-compared hold claim across the two ways a host can describe a
// constant curve. Finding 1 of the traces puts the gap between the released
// value and the host's restatement at 0.025, not at 1e-6, so that comparison
// never decided anything in the host the behaviour was built for.
void testTheEditorFacingWriteFollowsTheHostOnceTheHandIsOff() {
  constexpr double kOpeningValue = 0.875;
  constexpr double kLaneValue = 0.25;
  auto fixture = openTouchFixture("the editor-facing write test", 122);
  auto &processor = *fixture.processor;
  auto &handler = *fixture.handler;
  handler.clearEditLog();

  // The hand is down. The host's restatement moves nothing.
  expect(processor.setParamNormalized(fixture.parameterId, kLaneValue) == kResultTrue,
         "the host states the lane while the touch is open");
  expect(std::abs(processor.getParamNormalized(fixture.parameterId) - kOpeningValue) <
             1.0e-9,
         "a parameter under an open touch keeps the value the hand put on it");

  // Mouse-up, and the very same statement again.
  expect(PluginProcessorTestAccess::applyAutomationEdit(
             processor, fixture.identity, kOpeningValue,
             {/*bindIfUnbound=*/true, /*beginGesture=*/false, /*endGesture=*/true}),
         "release the touch");
  expect(!PluginProcessorTestAccess::hostGestureOpen(processor, fixture.parameterId),
         "the released touch is no longer published");
  handler.clearEditLog();
  expect(processor.setParamNormalized(fixture.parameterId, kLaneValue) == kResultTrue,
         "the host states the lane once the hand is off");
  const auto readBack = processor.getParamNormalized(fixture.parameterId);
  expect(std::abs(readBack - kLaneValue) < 1.0e-9,
         "and the released parameter takes it with nothing standing in the way "
         "-- it reads back " +
             std::to_string(readBack) + " instead of " + std::to_string(kLaneValue));

  // Whatever it did with the value, it did not tell the host about it.
  expect(handler.editLogCount == 0u,
         "the editor-facing write reports nothing back through IComponentHandler");

  expect(processor.terminate() == kResultOk, "terminate the editor-facing write test");
  handler.release();
}

// The SDK reports a touch as beginEdit on mouse-down, one performEdit per moved
// value and endEdit on mouse-up. Reporting every value as a complete touch of
// its own is legal but degenerate: a host that opens its automation writer on
// the touch window is left with no window to write into, which is what collapsed
// a drag into a one-per-second staircase.
void testOneTouchReportsOneBeginEveryValueAndOneEnd() {
  auto fixture = openTouchFixture("the touch shape test");
  auto &processor = *fixture.processor;
  auto &handler = *fixture.handler;

  // The opening call stated the value the control held before the hand arrived
  // and then held its own value back; one block separates that value from the
  // beginEdit, and the control tick that observes the boundary reports it. From
  // here the drag flows value by value.
  crossBlockBoundaryAndObserve(processor, "the touch shape test");

  // The first value already opened the touch in the fixture; these ride inside
  // it, and the last one releases.
  constexpr std::array<double, 3> drag{0.75, 0.625, 0.5};
  for (std::size_t index = 0; index < drag.size(); ++index) {
    expect(PluginProcessorTestAccess::applyAutomationEdit(
               processor, fixture.identity, drag[index],
               {/*bindIfUnbound=*/true, /*beginGesture=*/false,
                /*endGesture=*/index + 1 == drag.size()}),
           "carry a dragged value inside the open touch");
  }

  // The value the touch opened over, the fixture's opening value and the three
  // carried above. One report per value the user's hand made, with the single
  // statement of the value it started from in front of them.
  const auto values = drag.size() + 2u;
  expect(handler.stepCount(TestComponentHandler::EditStep::begin) == 1,
         "a drag opens exactly one touch");
  expect(handler.stepCount(TestComponentHandler::EditStep::perform) == values,
         "every dragged value is reported inside that touch");
  expect(handler.stepCount(TestComponentHandler::EditStep::end) == 1,
         "a drag closes exactly one touch");
  expect(handler.editLogCount == values + 2u, "the drag reports nothing else");
  expect(handler.editLog[0].step == TestComponentHandler::EditStep::begin &&
             handler.editLog[handler.editLogCount - 1].step ==
                 TestComponentHandler::EditStep::end,
         "the touch begins before its first value and ends after its last");
  for (std::size_t index = 1; index <= values; ++index) {
    expect(handler.editLog[index].step == TestComponentHandler::EditStep::perform &&
               handler.editLog[index].id == fixture.parameterId,
           "every value between the boundaries is a performEdit on the touched "
           "parameter");
  }
  expect(!PluginProcessorTestAccess::hostGestureOpen(processor, fixture.parameterId),
         "the released drag leaves no touch open");

  // A discrete edit -- a keyboard arrow, a typed value -- is a complete touch of
  // its own, and still reports all three steps.
  handler.clearEditLog();
  expect(PluginProcessorTestAccess::applyAutomationEdit(processor, fixture.identity,
                                                        0.25),
         "apply a discrete one-shot edit");
  expect(handler.stepCount(TestComponentHandler::EditStep::begin) == 1 &&
             handler.stepCount(TestComponentHandler::EditStep::perform) == 2 &&
             handler.stepCount(TestComponentHandler::EditStep::end) == 1 &&
             handler.editLogCount == 4,
         "a discrete edit is begun, performed and ended on its own -- it states "
         "the value it opened over and then the value the user made, and the "
         "close flushes the second because no block ran between them");

  expect(processor.terminate() == kResultOk, "terminate the touch shape test");
  handler.release();
}

// Finding 2 of the measured Sonar traces. Log B, second gesture: the opening
// call reported the pre-drag value 0.5 as an anchor and held the first real
// drag value back, then ten further values of the same drag replaced each other
// in that held position. What finally reached the host was 0.408333333, 57.6 ms
// and nine process blocks after the anchor, and the ten intermediate values
// covering that 57 ms of finger motion were never reported at all. The whole of
// what VST3 asks for a knob drag is beginEdit, one performEdit per value change
// and endEdit (ivsteditcontroller.h:221-235), so that is what is asserted here:
// every value, in the order the user made it, and nothing else.
//
// Replaces testDragAnchorsTheValueHeldWhenTheTouchOpened, which pinned the
// anchor and the block-boundary wait that produced exactly that loss.
void testADragReportsEveryValueInOrderAndNothingElse() {
  // The fixture opens the touch on 0.875; these carry it on, and the last one
  // releases.
  constexpr std::array<double, 5> kDrag{0.75, 0.625, 0.5, 0.375, 0.25};
  auto fixture = openTouchFixture("the drag fidelity test", 51);
  auto &processor = *fixture.processor;
  auto &handler = *fixture.handler;
  // The one boundary the shape has in it: the value that opened the touch waits
  // for a process() call, and the control tick that observes that call reports
  // it. Everything after it is reported in the call that made it.
  crossBlockBoundaryAndObserve(processor, "the drag fidelity test");

  for (std::size_t index = 0; index < kDrag.size(); ++index) {
    expect(PluginProcessorTestAccess::applyAutomationEdit(
               processor, fixture.identity, kDrag[index],
               {/*bindIfUnbound=*/true, /*beginGesture=*/false,
                /*endGesture=*/index + 1 == kDrag.size()}),
           "carry a dragged value inside the open touch");
  }

  // One begin, the value the touch opened over, one performEdit per value the
  // user made, one end. Nothing dropped, nothing reordered, and no value of the
  // user's reported out of turn.
  expect(handler.editLogCount == kDrag.size() + 4u,
         "a drag of " + std::to_string(kDrag.size() + 1u) +
             " values reports exactly that many performEdits, plus the one "
             "statement of the value it opened over, between one begin and one "
             "end -- it reported " +
             std::to_string(handler.editLogCount) + " records in all, not " +
             std::to_string(kDrag.size() + 4u));
  expect(handler.editLog[0].step == TestComponentHandler::EditStep::begin &&
             handler.editLog[0].id == fixture.parameterId,
         "the touch is begun first");
  expect(handler.editLog[handler.editLogCount - 1u].step ==
             TestComponentHandler::EditStep::end,
         "and ended last");
  for (std::size_t index = 1; index + 1u < handler.editLogCount; ++index) {
    expect(handler.editLog[index].step == TestComponentHandler::EditStep::perform &&
               handler.editLog[index].id == fixture.parameterId,
           "everything between the boundaries is a performEdit on the touched "
           "parameter");
  }
  expect(handler.performedEditCount == kDrag.size() + 2u,
         "one performEdit per value change, and one for the value the touch "
         "opened over");
  expect(std::abs(handler.performedEdits[0].value - 0.5) < 1.0e-9,
         "the first value reported is the one the control held before the hand "
         "arrived, stated once at the open -- it reported " +
             std::to_string(handler.performedEdits[0].value));
  expect(std::abs(handler.performedEdits[1].value - 0.875) < 1.0e-9,
         "the value the drag opened on follows it, released by the first block "
         "boundary after the open -- it reported " +
             std::to_string(handler.performedEdits[1].value));
  for (std::size_t index = 0; index < kDrag.size(); ++index) {
    expect(std::abs(handler.performedEdits[index + 2u].value - kDrag[index]) < 1.0e-9,
           "and the rest arrive in the order the user made them");
  }

  expect(!PluginProcessorTestAccess::hostGestureOpen(processor, fixture.parameterId),
         "the released drag leaves no touch open");
  expect(processor.terminate() == kResultOk, "terminate the drag fidelity test");
  handler.release();
}

// The same finding measured at the other end: nothing waits for a clock. The
// deferral released its held value on the ~50 ms control-service tick, which is
// how a value the user made could sit unreported through nine process blocks
// while newer values overwrote it in place. Every value now reaches the host
// inside the call that made it.
//
// Replaces testASupersedingValueTakesOverTheHeldBackPosition, which pinned that
// overwriting as correct.
void testEveryDragValueReachesTheHostInTheCallThatMadeIt() {
  auto fixture = openTouchFixture("the no-clock test", 52);
  auto &processor = *fixture.processor;
  auto &handler = *fixture.handler;

  expect(handler.editLogCount == 2u && handler.performedEditCount == 1u &&
             std::abs(handler.performedEdits[0].value - 0.5) < 1.0e-9,
         "the call that opens the touch reports the touch and the value it "
         "opened over, and stops there -- it reported " +
             std::to_string(handler.editLogCount) + " records");
  // The one value that waits, the one thing that lets it go, and the control
  // thread that carries it.
  crossBlockBoundaryAndObserve(processor, "the no-clock test");
  expect(handler.editLogCount == 3u && handler.performedEditCount == 2u &&
             std::abs(handler.performedEdits[1].value - 0.875) < 1.0e-9,
         "the first block after the open releases the value that opened the "
         "touch, and adds nothing else to it");

  auto expected = handler.performedEditCount;
  for (const double value : {0.75, 0.625, 0.5}) {
    expect(PluginProcessorTestAccess::applyAutomationEdit(
               processor, fixture.identity, value,
               {/*bindIfUnbound=*/true, /*beginGesture=*/false, /*endGesture=*/false}),
           "carry the next value of the same drag");
    ++expected;
    expect(handler.performedEditCount == expected &&
               std::abs(handler.performedEdits[expected - 1u].value - value) < 1.0e-9,
           "and the host has it before the call returns, not one clock tick later");
  }

  // No clock owes the host anything behind the drag, and neither does a second
  // boundary: once the opening value is out, nothing is held for one to find.
  const auto reported = handler.editLogCount;
  PluginProcessorTestAccess::beginSyntheticBlock(processor);
  PluginProcessorTestAccess::endSyntheticBlock(processor);
  crossBlockBoundaryAndObserve(processor, "the no-clock test");
  expect(handler.editLogCount == reported,
         "a later block boundary releases nothing, because nothing is held "
         "behind the drag any more");

  expect(PluginProcessorTestAccess::applyAutomationEdit(
             processor, fixture.identity, 0.375,
             {/*bindIfUnbound=*/true, /*beginGesture=*/false, /*endGesture=*/true}),
         "release the drag");
  expect(handler.editLogCount == reported + 2u &&
             handler.editLog[reported].step == TestComponentHandler::EditStep::perform &&
             handler.editLog[reported + 1u].step == TestComponentHandler::EditStep::end &&
             std::abs(handler.performedEdits[handler.performedEditCount - 1u].value -
                      0.375) < 1.0e-9,
         "and the release reports its own value and then closes the touch");

  expect(processor.terminate() == kResultOk, "terminate the no-clock test");
  handler.release();
}

// The shortest drag there is: the pointer goes down, the control moves once and
// the pointer comes up. Under the deferral this was the case the mechanism had
// to special-case, because the value the drag made had never been reported and
// the close was its last chance to reach the host at all. With nothing held
// back it is simply a touch with one value in it.
//
// Replaces testAReleaseWithNoFurtherValueStillReportsTheOpeningValue.
void testTheShortestDragIsATouchWithOneValueInIt() {
  constexpr double kOnlyMovedValue = 0.875;
  auto fixture = openTouchFixture("the short drag test", 53);
  auto &processor = *fixture.processor;
  auto &handler = *fixture.handler;

  expect(handler.editLogCount == 2u,
         "the opening call has already reported the touch and the value it "
         "opened over");

  // The pointer comes up. The editor names the target it was holding and sends
  // no value with it, which is the only thing a release carries.
  const auto released = choc::json::parse(processor.handleUiMessage(
      std::string{R"({"type":"automation/endGesture","payload":{"targets":[)"
                  R"({"pipeline":"A","pluginId":)"} +
      std::to_string(fixture.identity.pluginId) +
      R"(,"pluginType":"DCOffsetPlugin","parameterKey":"of","elementIndex":0}]}})"));
  expect(released["ok"].getWithDefault<bool>(false), "the release is answered");

  // No block ran between the open and the close, so the close is the only
  // chance the one value the user made ever gets. It takes it, before the
  // endEdit and inside the touch.
  expect(handler.editLogCount == 4u,
         "the release flushes the value that was still waiting on a block and "
         "then closes the touch -- the log holds " +
             std::to_string(handler.editLogCount) + " records, not 4");
  expect(handler.editLog[0].step == TestComponentHandler::EditStep::begin &&
             handler.editLog[1].step == TestComponentHandler::EditStep::perform &&
             handler.editLog[2].step == TestComponentHandler::EditStep::perform &&
             handler.editLog[3].step == TestComponentHandler::EditStep::end,
         "begin, the value it opened over, the one value, end -- and the whole "
         "of the VST3 contract for a drag is kept");
  expect(handler.performedEditCount == 2u &&
             std::abs(handler.performedEdits[0].value - 0.5) < 1.0e-9 &&
             std::abs(handler.performedEdits[1].value - kOnlyMovedValue) < 1.0e-9,
         "and the last value reported is the one the user made, never lost to "
         "a boundary that never came");
  expect(!PluginProcessorTestAccess::hostGestureOpen(processor, fixture.parameterId),
         "and the touch is closed at the end of it");

  // Nothing is left behind for a later clock to report into a touch that has
  // already gone.
  PluginProcessorTestAccess::beginSyntheticBlock(processor);
  PluginProcessorTestAccess::endSyntheticBlock(processor);
  crossBlockBoundaryAndObserve(processor, "the short drag test");
  expect(handler.editLogCount == 4u,
         "a block boundary after the close, and the control tick that observes "
         "it, report nothing at all");

  expect(processor.terminate() == kResultOk, "terminate the short drag test");
  handler.release();
}

// The shape the change exists to produce, asserted end to end. Trace C of the
// measured Sonar session, first gesture on parameter 65556: txnBegin at
// t=8046.662 with previous=0.697619021, beginEdit at 8046.687 and the first
// drag value 0.702380952 at 8046.698 -- eleven microseconds later, inside the
// same automation tick and with no block between them. The envelope recovered
// from the playback pass held 0.702380955 from project time 0 to sample 110080,
// the position the drag started at: the whole take before the gesture had been
// repainted with the drag's opening value. One process() call between the
// beginEdit and that value is what separates them.
void testTheOpeningValueWaitsForOneBlockAndTheDragThenFlows() {
  // The fixture binds the target with the plug-in at of = 0, which is
  // normalized 0.5: the value the parameter held immediately before the gesture
  // opened.
  constexpr double kBeforeTheHand = 0.5;
  constexpr double kOpeningValue = 0.875;
  constexpr std::array<double, 3> kDrag{0.75, 0.625, 0.5};
  auto fixture = openTouchFixture("the open-hold shape test", 141);
  auto &processor = *fixture.processor;
  auto &handler = *fixture.handler;

  // beginEdit, then the value the parameter held immediately before the gesture
  // opened, and nothing else at all.
  expect(handler.editLogCount == 2u &&
             handler.editLog[0].step == TestComponentHandler::EditStep::begin &&
             handler.editLog[0].id == fixture.parameterId &&
             handler.editLog[1].step == TestComponentHandler::EditStep::perform &&
             handler.editLog[1].id == fixture.parameterId &&
             handler.performedEditCount == 1u &&
             std::abs(handler.performedEdits[0].value - kBeforeTheHand) < 1.0e-9,
         "the open reports beginEdit and then the value the parameter held "
         "before it, and stops -- it reported " +
             std::to_string(handler.editLogCount) + " records");

  // The value the gesture opened on is not among them, and no amount of
  // servicing produces it.
  PluginProcessorTestAccess::serviceLatencyUpdates(processor);
  PluginProcessorTestAccess::drainAutomationValues(processor);
  expect(handler.editLogCount == 2u,
         "nothing further reaches the host until a block has run");

  // The process() call publishes the boundary and reports nothing itself: an
  // IComponentHandler call from the audio thread is a VST3 threading violation,
  // so the callback may only advance the epoch.
  crossBlockBoundary(processor, "the open-hold shape test");
  expect(handler.editLogCount == 2u,
         "the process() call itself makes no IComponentHandler call -- the log "
         "grew to " +
             std::to_string(handler.editLogCount) + " records");

  // The control thread observes the crossed boundary and carries the value.
  observeBoundaryOnControlThread(processor);
  expect(handler.editLogCount == 3u && handler.performedEditCount == 2u &&
             handler.editLog[2].step == TestComponentHandler::EditStep::perform &&
             std::abs(handler.performedEdits[1].value - kOpeningValue) < 1.0e-9,
         "the first control-thread observation after that boundary reports the "
         "value the gesture opened on, and only it");

  // From there the drag flows: one performEdit per value, in the call that made
  // it, with no boundary in front of any of them.
  auto reported = handler.performedEditCount;
  for (const double value : kDrag) {
    expect(PluginProcessorTestAccess::applyAutomationEdit(
               processor, fixture.identity, value,
               {/*bindIfUnbound=*/true, /*beginGesture=*/false, /*endGesture=*/false}),
           "carry a value of the open-hold drag");
    ++reported;
    expect(handler.performedEditCount == reported &&
               std::abs(handler.performedEdits[reported - 1u].value - value) < 1.0e-9,
           "every value after the boundary is reported in the very call that "
           "produced it");
  }

  // And a later boundary has nothing of its own to say.
  crossBlockBoundaryAndObserve(processor, "the open-hold shape test");
  expect(handler.performedEditCount == reported,
         "a later process() boundary releases nothing, because nothing is held "
         "behind the drag any more");

  expect(PluginProcessorTestAccess::applyAutomationEdit(
             processor, fixture.identity, kDrag.back(),
             {/*bindIfUnbound=*/true, /*beginGesture=*/false, /*endGesture=*/true}),
         "release the open-hold drag");
  expect(handler.editLog[handler.editLogCount - 1u].step ==
                 TestComponentHandler::EditStep::end &&
             handler.stepCount(TestComponentHandler::EditStep::begin) == 1 &&
             handler.stepCount(TestComponentHandler::EditStep::end) == 1,
         "endEdit closes the one touch, last");

  expect(processor.terminate() == kResultOk, "terminate the open-hold shape test");
  handler.release();
}

// A gesture can begin and end inside one sub-block window -- a keyboard arrow,
// a typed value, a click on a control that is released before the next block.
// The boundary the value is waiting for never comes, so the close stands in for
// one. Nothing may be lost to a wait for something that never happens.
void testAGestureClosedBeforeAnyBlockStillReportsItsValue() {
  constexpr double kBeforeTheHand = 0.5;
  constexpr double kOpeningValue = 0.875;
  auto fixture = openTouchFixture("the close-flush test", 142);
  auto &processor = *fixture.processor;
  auto &handler = *fixture.handler;

  expect(handler.editLogCount == 2u && handler.performedEditCount == 1u,
         "the value the gesture opened on is still waiting on a block");

  // Mouse-up, with no block anywhere between it and the mouse-down.
  expect(PluginProcessorTestAccess::applyAutomationEdit(
             processor, fixture.identity, kOpeningValue,
             {/*bindIfUnbound=*/true, /*beginGesture=*/false, /*endGesture=*/true}),
         "close the gesture before any block runs");

  expect(handler.editLogCount == 4u && handler.performedEditCount == 2u &&
             handler.editLog[2].step == TestComponentHandler::EditStep::perform &&
             handler.editLog[3].step == TestComponentHandler::EditStep::end &&
             std::abs(handler.performedEdits[0].value - kBeforeTheHand) < 1.0e-9 &&
             std::abs(handler.performedEdits[1].value - kOpeningValue) < 1.0e-9,
         "the close flushes the held value before it ends the touch, so the "
         "user's value is reported inside the gesture and not lost -- the log "
         "holds " +
             std::to_string(handler.editLogCount) + " records, not 4");

  // And it outlives the gesture in no form: the block that finally arrives
  // finds nothing.
  crossBlockBoundaryAndObserve(processor, "the close-flush test");
  expect(handler.editLogCount == 4u,
         "the first block after the close reports nothing at all -- a value "
         "that outlived its gesture would land after the endEdit, outside any "
         "touch the host has open");

  expect(processor.terminate() == kResultOk, "terminate the close-flush test");
  handler.release();
}

// Several values can be made inside the one sub-block window that opens a
// gesture: an editor that reports at frame rate against a 64-sample block does
// it on any quick move. They occupy one held position, so the boundary reports
// the latest of them and one value only. That is the whole of what the change
// may cost, and it is bounded by the window itself.
void testEveryValueBeforeTheBoundaryCollapsesIntoTheLatest() {
  constexpr double kBeforeTheHand = 0.5;
  constexpr std::array<double, 3> kInsideTheWindow{0.8, 0.7, 0.6};
  auto fixture = openTouchFixture("the sub-block coalescing test", 143);
  auto &processor = *fixture.processor;
  auto &handler = *fixture.handler;

  for (const double value : kInsideTheWindow) {
    expect(PluginProcessorTestAccess::applyAutomationEdit(
               processor, fixture.identity, value,
               {/*bindIfUnbound=*/true, /*beginGesture=*/false, /*endGesture=*/false}),
           "carry a value inside the sub-block window");
  }
  expect(handler.editLogCount == 2u && handler.performedEditCount == 1u &&
             std::abs(handler.performedEdits[0].value - kBeforeTheHand) < 1.0e-9,
         "no value made inside the window reaches the host while it is open -- "
         "the log holds " +
             std::to_string(handler.editLogCount) + " records, not 2");

  crossBlockBoundaryAndObserve(processor, "the sub-block coalescing test");
  expect(handler.performedEditCount == 2u &&
             std::abs(handler.performedEdits[1].value - kInsideTheWindow.back()) <
                 1.0e-9,
         "the boundary reports exactly one value, the most recent one -- it "
         "reported " +
             std::to_string(handler.performedEditCount) + " values in all");
  expect(handler.editLogCount == 3u,
         "and it adds one record to the touch, not one per value it stood in "
         "for");

  // The window is closed behind it: the very next value is reported whole.
  expect(PluginProcessorTestAccess::applyAutomationEdit(
             processor, fixture.identity, 0.55,
             {/*bindIfUnbound=*/true, /*beginGesture=*/false, /*endGesture=*/false}),
         "carry a value after the window has closed");
  expect(handler.performedEditCount == 3u &&
             std::abs(handler.performedEdits[2].value - 0.55) < 1.0e-9,
         "and every value after it is reported in the call that made it");

  expect(processor.terminate() == kResultOk,
         "terminate the sub-block coalescing test");
  handler.release();
}

// The trigger is a process() boundary and nothing else. The mechanism this
// replaces released its held value on the ~50 ms control-service tick alone,
// which is how a value could sit unreported for 57.6 ms and nine blocks of a
// measured drag. No timer, no service call, no poll from the editor and no
// amount of elapsed time may stand in for the boundary.
//
// The control-service tick now carries the release, because performEdit is a
// UI/controller-thread call and the audio callback may not make one. That makes
// the distinction between carrier and trigger the thing this test has to pin:
// the carrier runs here as often as every other clock does and still reports
// nothing, because no boundary has happened. Only once a process() call has
// moved the epoch does an observation on the control thread release anything --
// and the process() call by itself releases nothing either.
void testNoClockReleasesTheValueWaitingOnABlock() {
  auto fixture = openTouchFixture("the no-timer release test", 144);
  auto &processor = *fixture.processor;
  auto &handler = *fixture.handler;
  const auto atOpen = handler.editLogCount;
  expect(atOpen == 2u && handler.performedEditCount == 1u,
         "the value the gesture opened on is waiting on a block");

  // Every clock the plug-in has, several times over, and no block behind any of
  // them: the control-service timer, the editor's own poll into the same
  // service, the automation drain, and the held-value carrier the timer runs.
  for (auto tick = 0; tick < 4; ++tick) {
    PluginProcessorTestAccess::serviceLatencyUpdates(processor);
    PluginProcessorTestAccess::pollLatencyUpdatesFromUi(processor);
    PluginProcessorTestAccess::drainAutomationValues(processor);
    observeBoundaryOnControlThread(processor);
  }
  // And real elapsed time, well past the 50 ms tick the old mechanism waited
  // for, with every clock run again on the other side of it. This is the part
  // that proves elapsed time is not the trigger: a timeout-based release would
  // have fired several times over by here.
  std::this_thread::sleep_for(std::chrono::milliseconds{150});
  PluginProcessorTestAccess::serviceLatencyUpdates(processor);
  PluginProcessorTestAccess::pollLatencyUpdatesFromUi(processor);
  observeBoundaryOnControlThread(processor);

  expect(handler.editLogCount == atOpen,
         "no tick, no service call, no carrier run and no elapsed time reports "
         "the held value -- the log grew to " +
             std::to_string(handler.editLogCount) + " records from " +
             std::to_string(atOpen));

  // The boundary, on its own. It publishes the epoch and calls nothing: an
  // IComponentHandler call from the audio thread is what this design exists to
  // remove.
  crossBlockBoundary(processor, "the no-timer release test");
  expect(handler.editLogCount == atOpen,
         "the process() call that crosses the boundary reports nothing itself "
         "-- the log grew to " +
             std::to_string(handler.editLogCount) + " records from " +
             std::to_string(atOpen));

  // And the first observation on the correct thread after it does what no
  // amount of clock without a boundary could.
  observeBoundaryOnControlThread(processor);
  expect(handler.editLogCount == atOpen + 1u && handler.performedEditCount == 2u &&
             std::abs(handler.performedEdits[1].value - 0.875) < 1.0e-9,
         "and the first control-thread observation after one process() call "
         "does what none of them could");

  expect(processor.terminate() == kResultOk, "terminate the no-timer release test");
  handler.release();
}

// VST3 puts beginEdit, performEdit and endEdit on the UI/controller thread
// (ivsteditcontroller.h:221-235); process() runs on the audio thread. A held
// value used to be released from inside the audio callback, which made every
// one of those calls -- and an unsynchronised read of componentHandler -- a
// real threading violation. The callback now publishes a counter and nothing
// else.
//
// The blocks below are rendered while a value is genuinely still held, which is
// exactly the state the old code called the host in. Nothing may reach the host
// from them, and the value may not be quietly lost either: the observation that
// follows has to still find it.
void testProcessCallsNoComponentHandlerMethodWhileAValueIsHeld() {
  auto fixture = openTouchFixture("the audio-thread silence test", 145);
  auto &processor = *fixture.processor;
  auto &handler = *fixture.handler;
  expect(handler.editLogCount == 2u && handler.performedEditCount == 1u,
         "the value the gesture opened on is held, which is the state the "
         "audio callback used to call the host in");
  const auto beforeTheBlocks = handler.handlerCallCount();

  // Several blocks, so a callback that reported on any exit path it has would
  // be caught whichever one it took.
  for (auto block = 0; block < 3; ++block) {
    crossBlockBoundary(processor, "the audio-thread silence test");
  }
  expect(handler.handlerCallCount() == beforeTheBlocks,
         "process() makes no IComponentHandler or IComponentHandler2 call at "
         "all while a value is held -- the host was called " +
             std::to_string(handler.handlerCallCount() - beforeTheBlocks) +
             " times from the audio thread");

  // And the value is still there to be reported, so the silence above is the
  // callback declining to call the host and not the hold having been dropped.
  observeBoundaryOnControlThread(processor);
  expect(handler.performedEditCount == 2u &&
             std::abs(handler.performedEdits[1].value - 0.875) < 1.0e-9,
         "the value was still held through those blocks, and the control "
         "thread is what finally reports it");

  expect(processor.terminate() == kResultOk,
         "terminate the audio-thread silence test");
  handler.release();
}

// The carrier, pinned at both edges. The control-service tick is what reaches
// the host for a finger that has stopped moving mid-drag -- without it that
// value would sit until the gesture closed and land at the release position
// instead of where the user made it. It is a carrier and not a trigger: it
// observes processBlockEpoch_ and releases only what a process() boundary has
// already separated from its beginEdit.
void testTheControlServiceCarriesAHeldValueOnlyAfterTheBoundary() {
  constexpr double kOpeningValue = 0.875;
  auto fixture = openTouchFixture("the carrier test", 146);
  auto &processor = *fixture.processor;
  auto &handler = *fixture.handler;
  expect(handler.editLogCount == 2u && handler.performedEditCount == 1u,
         "the value the gesture opened on is waiting on a block");

  // Before the boundary the carrier is inert, however often it runs.
  for (auto tick = 0; tick < 5; ++tick) {
    observeBoundaryOnControlThread(processor);
  }
  expect(handler.editLogCount == 2u,
         "the control service releases nothing before a boundary has been "
         "crossed -- the log grew to " +
             std::to_string(handler.editLogCount) + " records");

  crossBlockBoundary(processor, "the carrier test");
  expect(handler.editLogCount == 2u, "and the boundary itself reports nothing");

  // The finger has stopped moving, so no further value of the drag will come
  // for it. The tick is the only thing left that can carry it.
  observeBoundaryOnControlThread(processor);
  expect(handler.editLogCount == 3u && handler.performedEditCount == 2u &&
             handler.editLog[2].step == TestComponentHandler::EditStep::perform &&
             handler.editLog[2].id == fixture.parameterId &&
             std::abs(handler.performedEdits[1].value - kOpeningValue) < 1.0e-9,
         "the first tick after the boundary reports the held value, on the "
         "touched parameter");

  // Once, and not once per tick.
  for (auto tick = 0; tick < 5; ++tick) {
    observeBoundaryOnControlThread(processor);
  }
  crossBlockBoundary(processor, "the carrier test");
  for (auto tick = 0; tick < 5; ++tick) {
    observeBoundaryOnControlThread(processor);
  }
  expect(handler.editLogCount == 3u,
         "and no later tick or boundary reports it again -- the log grew to " +
             std::to_string(handler.editLogCount) + " records");

  expect(PluginProcessorTestAccess::hostGestureOpen(processor, fixture.parameterId),
         "the touch the carrier reported into is still the user's own");
  expect(processor.terminate() == kResultOk, "terminate the carrier test");
  handler.release();
}

// A finger that is still moving must never wait for a clock. Once the boundary
// has been crossed, the next value of the same gesture clears the hold and is
// itself what reaches the host, in the call that produced it. The older
// withheld value is not emitted in front of it: both would land at the same
// host position -- performEdit carries no time of its own -- so the lane would
// keep the later one anyway and the earlier point is pure waste.
void testALiveDragNeverRoutesThroughTheControlService() {
  constexpr double kBeforeTheHand = 0.5;
  constexpr double kOpeningValue = 0.875;
  constexpr std::array<double, 3> kDrag{0.75, 0.625, 0.375};
  auto fixture = openTouchFixture("the live drag test", 147);
  auto &processor = *fixture.processor;
  auto &handler = *fixture.handler;
  expect(handler.performedEditCount == 1u &&
             std::abs(handler.performedEdits[0].value - kBeforeTheHand) < 1.0e-9,
         "the open states the value the control held and holds the drag's own "
         "value back");

  // The boundary passes while the finger is still moving. Every control tick in
  // this test is asserted to do nothing: the drag has to carry itself.
  crossBlockBoundary(processor, "the live drag test");

  auto reported = handler.performedEditCount;
  for (const double value : kDrag) {
    expect(PluginProcessorTestAccess::applyAutomationEdit(
               processor, fixture.identity, value,
               {/*bindIfUnbound=*/true, /*beginGesture=*/false, /*endGesture=*/false}),
           "carry a value of the live drag");
    ++reported;
    expect(handler.performedEditCount == reported &&
               std::abs(handler.performedEdits[reported - 1u].value - value) < 1.0e-9,
           "the value the user just made is the one that reaches the host, in "
           "the call that produced it, with nothing emitted in front of it");
    observeBoundaryOnControlThread(processor);
    expect(handler.performedEditCount == reported,
           "and the control service has nothing to do for a drag that is still "
           "moving");
    // Blocks keep flowing underneath the drag, as they do in a host.
    crossBlockBoundary(processor, "the live drag test");
  }

  // The value the gesture opened on was superseded by the first drag value and
  // never reported: it and its successor would have landed on the same host
  // position, and the lane keeps the later of two points that share one.
  for (std::size_t index = 1; index < handler.performedEditCount; ++index) {
    expect(std::abs(handler.performedEdits[index].value - kOpeningValue) >= 1.0e-9,
           "the superseded opening value is never emitted behind the value "
           "that replaced it");
  }
  expect(handler.performedEditCount == kDrag.size() + 1u,
         "one report per value the drag made, behind the single statement of "
         "the value it opened over -- the host was told " +
             std::to_string(handler.performedEditCount) + " values in all");

  expect(processor.terminate() == kResultOk, "terminate the live drag test");
  handler.release();
}

// The hold belongs to the one gesture that opened it. A parameter with no hand
// on it is reported exactly as it always was, in the call that made it, while
// another parameter's value waits on a block -- and the boundary that finally
// arrives releases the waiting one and nothing besides.
void testTheHeldValueBelongsToTheParameterThatOpenedIt() {
  constexpr double kBeforeTheHand = 0.5;
  constexpr double kHeldValue = 0.875;
  constexpr double kNeighbourValue = 0.25;
  auto processor = std::make_unique<EffeTuneProcessor>();
  expect(processor->initialize(nullptr) == kResultOk,
         "initialize the hold scope test");
  auto *handler = new TestComponentHandler();
  expect(processor->setComponentHandler(handler) == kResultOk,
         "install the component handler for the hold scope test");
  auto processSetup = setup(48000.0, 64);
  expect(processor->setupProcessing(processSetup) == kResultOk,
         "prepare the hold scope test");
  const auto offset = [](const std::uint32_t id) {
    return std::string{"{\"id\":"} + std::to_string(id) +
           R"(,"type":"DCOffsetPlugin","name":"DC Offset","enabled":true,)"
           R"("parameters":{"of":0},"wasmParams":[0],)"
           R"("wasmParamsHash":1104945464})";
  };
  const auto installed = choc::json::parse(processor->handleUiMessage(
      std::string{R"({"type":"pipeline/rebuild","payload":{"pipeline":"A","plugins":[)"} +
      offset(91) + "," + offset(92) + "]}}"));
  expect(installed["ok"].getWithDefault<bool>(false),
         "install the hold scope pipeline");
  const effetune::vst::AutomationTargetIdentity held{'A', 91, "DCOffsetPlugin", "of", 0};
  const effetune::vst::AutomationTargetIdentity neighbour{'A', 92, "DCOffsetPlugin",
                                                          "of", 0};
  const auto heldParameterId = boundAutomationParameterId(*processor, held);
  const auto neighbourParameterId = boundAutomationParameterId(*processor, neighbour);
  handler->clearEditLog();

  // The hand goes down on one of the two, and its value waits on a block.
  expect(PluginProcessorTestAccess::applyAutomationEdit(
             *processor, held, kHeldValue,
             {/*bindIfUnbound=*/true, /*beginGesture=*/true, /*endGesture=*/false}),
         "open the gesture that holds a value back");
  const auto reportsFor = [&](const ParamID id) {
    std::size_t count = 0;
    for (std::size_t index = 0; index < handler->performedEditCount; ++index) {
      if (handler->performedEdits[index].id == id) {
        ++count;
      }
    }
    return count;
  };
  expect(reportsFor(heldParameterId) == 1u,
         "the open states the value the held parameter had, and nothing more");

  // A complete gesture on the neighbour, start to finish, while that one waits.
  expect(PluginProcessorTestAccess::applyAutomationEdit(*processor, neighbour,
                                                        kNeighbourValue),
         "make a complete edit on the untouched neighbour");
  expect(reportsFor(neighbourParameterId) == 2u &&
             std::abs(handler->performedEdits[handler->performedEditCount - 1u].value -
                      kNeighbourValue) < 1.0e-9,
         "the neighbour's own gesture is reported whole, on its own parameter");
  expect(reportsFor(heldParameterId) == 1u,
         "and nothing of it releases the value the other parameter is holding");
  expect(!PluginProcessorTestAccess::hostGestureOpen(*processor, neighbourParameterId),
         "the neighbour's gesture closed itself");

  // The boundary lets the one value that is waiting go, and the control thread
  // that observes it reports that value, once.
  crossBlockBoundaryAndObserve(*processor, "the hold scope test");
  expect(reportsFor(heldParameterId) == 2u &&
             std::abs(handler->performedEdits[handler->performedEditCount - 1u].value -
                      kHeldValue) < 1.0e-9,
         "the boundary reports the held parameter's waiting value");
  expect(reportsFor(neighbourParameterId) == 2u,
         "and adds nothing to the gesture that had already closed itself");
  const auto afterTheBoundary = handler->performedEditCount;
  crossBlockBoundaryAndObserve(*processor, "the hold scope test");
  expect(handler->performedEditCount == afterTheBoundary,
         "a second boundary finds nothing held on either parameter");
  expect(std::abs(handler->performedEdits[0].value - kBeforeTheHand) < 1.0e-9,
         "and the first thing the host was told is still the value the held "
         "parameter had before the hand arrived");

  expect(processor->terminate() == kResultOk, "terminate the hold scope test");
  handler->release();
}

// The one rule that is kept, pinned at its edges. While the pointer is down on
// a control the host does not get to move that control underneath the hand, and
// that is the whole of the suppression: it belongs to the touched parameter and
// to nothing else. A neighbouring lane playing back at the same moment has to
// arrive untouched, on the same block, out of the same queue.
//
// Replaces testAReleasedDragOutlivesTheHostsRestatedLane, which asserted that a
// released drag outlives the lane the host restates afterwards. The measured
// traces say the opposite happens and cannot be prevented from the plug-in
// side: on endEdit the host punches out and resumes streaming its pre-existing
// envelope on every block, bit-identically, so the value a release latches
// loses on the first block every time. What is testable is the part the
// plug-in does own -- the hand-off is exact and it is scoped.
void testAnOpenTouchSuppressesOnlyItsOwnParameter() {
  constexpr double kTouchedValue = 0.875;
  constexpr double kHostLaneValue = 0.125;
  auto processor = std::make_unique<EffeTuneProcessor>();
  expect(processor->initialize(nullptr) == kResultOk,
         "initialize the touch scope test");
  auto *handler = new TestComponentHandler();
  expect(processor->setComponentHandler(handler) == kResultOk,
         "install the component handler for the touch scope test");
  auto processSetup = setup(48000.0, 64);
  expect(processor->setupProcessing(processSetup) == kResultOk,
         "prepare the touch scope test");

  const auto offset = [](const std::uint32_t id) {
    return std::string{"{\"id\":"} + std::to_string(id) +
           R"(,"type":"DCOffsetPlugin","name":"DC Offset","enabled":true,)"
           R"("parameters":{"of":0},"wasmParams":[0],)"
           R"("wasmParamsHash":1104945464})";
  };
  const auto installed = choc::json::parse(processor->handleUiMessage(
      std::string{R"({"type":"pipeline/rebuild","payload":{"pipeline":"A","plugins":[)"} +
      offset(81) + "," + offset(82) + "]}}"));
  expect(installed["ok"].getWithDefault<bool>(false),
         "install the touch scope pipeline");

  const effetune::vst::AutomationTargetIdentity touched{'A', 81, "DCOffsetPlugin", "of", 0};
  const effetune::vst::AutomationTargetIdentity untouched{'A', 82, "DCOffsetPlugin", "of", 0};
  const auto touchedParameterId = boundAutomationParameterId(*processor, touched);
  const auto untouchedParameterId = boundAutomationParameterId(*processor, untouched);
  const auto touchedSlot =
      PluginProcessorTestAccess::activeAutomationSlot(*processor, touched);
  const auto untouchedSlot =
      PluginProcessorTestAccess::activeAutomationSlot(*processor, untouched);
  expect(touchedSlot.has_value() && untouchedSlot.has_value(),
         "both touch scope targets hold a slot");
  expect(processor->setActive(true) == kResultOk, "activate the touch scope test");

  gate_ordering::AudioRig rig(0.0f);
  ProcessContext context{};
  context.state = ProcessContext::kPlaying;
  context.projectTimeSamples = 0;
  rig.data.processContext = &context;
  // One block with the host playing the same lane value into both slots, which
  // is what a Read-enabled host does on every block whether a hand is down or
  // not.
  const auto playBothLanes = [&](const std::string &what) {
    ParameterChanges changes(2);
    int32 queueIndex = 0;
    auto *touchedQueue = changes.addParameterData(touchedParameterId, queueIndex);
    auto *untouchedQueue = changes.addParameterData(untouchedParameterId, queueIndex);
    int32 pointIndex = 0;
    expect(touchedQueue != nullptr && untouchedQueue != nullptr &&
               touchedQueue->addPoint(0, kHostLaneValue, pointIndex) == kResultTrue &&
               untouchedQueue->addPoint(0, kHostLaneValue, pointIndex) == kResultTrue,
           "play the host lane into both slots for " + what);
    rig.data.inputParameterChanges = &changes;
    expect(processor->process(rig.data) == kResultOk, "render " + what);
    rig.data.inputParameterChanges = nullptr;
    context.projectTimeSamples += rig.data.numSamples;
  };

  // The hand goes down on one of the two.
  expect(PluginProcessorTestAccess::applyAutomationEdit(
             *processor, touched, kTouchedValue,
             {/*bindIfUnbound=*/true, /*beginGesture=*/true, /*endGesture=*/false}),
         "open the touch on the first target");
  expect(PluginProcessorTestAccess::hostGestureOpen(*processor, touchedParameterId) &&
             !PluginProcessorTestAccess::hostGestureOpen(*processor,
                                                         untouchedParameterId),
         "only the touched parameter is published as held");

  playBothLanes("the block under the open touch");
  expect(std::abs(PluginProcessorTestAccess::playedAutomationValue(*processor,
                                                                   *touchedSlot) -
                  kTouchedValue) < 1.0e-9,
         "the held control keeps the value the hand put on it");
  expect(std::abs(PluginProcessorTestAccess::playedAutomationValue(*processor,
                                                                   *untouchedSlot) -
                  kHostLaneValue) < 1.0e-9,
         "and the neighbouring lane plays through untouched, out of the very "
         "same queue -- suppression that reached it would silence every other "
         "automated parameter for as long as one control is held");

  // The hand comes up. The host owns the slot again from its very next
  // statement, with no grace block and no carried-over claim: Finding 1 of the
  // measured traces is that the host resumes its own envelope immediately and
  // bit-identically, so anything the plug-in tried to hold here would lose on
  // this block anyway.
  expect(PluginProcessorTestAccess::applyAutomationEdit(
             *processor, touched, kTouchedValue,
             {/*bindIfUnbound=*/true, /*beginGesture=*/false, /*endGesture=*/true}),
         "release the touch on the first target");
  expect(!PluginProcessorTestAccess::hostGestureOpen(*processor, touchedParameterId),
         "the released touch is no longer published");

  playBothLanes("the first block after the release");
  expect(std::abs(PluginProcessorTestAccess::playedAutomationValue(*processor,
                                                                   *touchedSlot) -
                  kHostLaneValue) < 1.0e-9,
         "the released control follows the host on the very next block");
  expect(std::abs(PluginProcessorTestAccess::playedAutomationValue(*processor,
                                                                   *untouchedSlot) -
                  kHostLaneValue) < 1.0e-9,
         "and its neighbour never stopped following it");

  expect(processor->setActive(false) == kResultOk, "deactivate the touch scope test");
  expect(processor->terminate() == kResultOk, "terminate the touch scope test");
  handler->release();
}

// Finding 3 of the measured traces, stated as the thing the plug-in can be held
// to. A host with nothing recorded before the drag back-extrapolates the lane's
// first recorded point over the whole earlier region, so whatever value sits at
// the earliest recorded position decides what that region plays back as. The
// anchor was an attempt to put the pre-drag value there, and log C shows it did
// not work: the host recorded none of the first sixteen performEdit calls and
// back-extrapolated 0.177777782 -- a value from well inside the drag -- from
// project position 0. Which position a host stamps a report with is not
// something performEdit can express (ivsteditcontroller.h:226-230), so the
// plug-in cannot decide it. What the plug-in decides is which values exist at
// all, and a value the user never made must not be among them: an anchor that
// fails to hold its own position degrades into an extra point of the old value
// dropped somewhere inside the new drag.
//
// Replaces testTheAnchorKeepsALanePositionOfItsOwnAgainstTheNextDragValue,
// which asserted the anchor did hold that position.
void testADragReportsNoValueTheUserNeverMade() {
  // The fixture binds the target with the plug-in at of = 0, which is
  // normalized 0.5: that is the value the lane held before the drag, and the
  // one the anchor used to report.
  constexpr double kLaneBeforeTheDrag = 0.5;
  constexpr double kTouchedValue = 0.875;
  constexpr std::array<double, 3> kDrag{0.8, 0.7, 0.65};
  auto fixture = openTouchFixture("the no-invented-point test", 133);
  auto &processor = *fixture.processor;
  auto &handler = *fixture.handler;
  expect(processor.setActive(true) == kResultOk,
         "activate the no-invented-point test");

  // The lane position the host stamps a report with only moves when a block is
  // rendered, so the drag is carried across blocks: that is the configuration
  // in which an extra report is free to land on a position of the drag's own.
  gate_ordering::AudioRig rig(0.0f);
  for (std::size_t index = 0; index < kDrag.size(); ++index) {
    ++handler.lanePosition;
    expect(processor.process(rig.data) == kResultOk,
           "render a block of the no-invented-point drag");
    // The block publishes the boundary and calls nothing; the control tick that
    // observes it is what carries the value the gesture opened on to the host,
    // on the position that block moved the host to.
    observeBoundaryOnControlThread(processor);
    expect(PluginProcessorTestAccess::applyAutomationEdit(
               processor, fixture.identity, kDrag[index],
               {/*bindIfUnbound=*/true, /*beginGesture=*/false,
                /*endGesture=*/index + 1 == kDrag.size()}),
           "carry a value of the no-invented-point drag");
  }
  expect(!PluginProcessorTestAccess::hostGestureOpen(processor, fixture.parameterId),
         "the released drag leaves no touch open");
  expect(handler.performedEditCount <= handler.performedEdits.size(),
         "the recorded lane fits in the log");

  // One point per value the user made, and one statement of the value the
  // control already held -- which is a value the user did make, at the position
  // where it was still the truth.
  expect(handler.performedEditCount == kDrag.size() + 2u,
         "the drag reports one point per value the user made, behind the one "
         "statement of the value it opened over -- it reported " +
             std::to_string(handler.performedEditCount) + ", not " +
             std::to_string(kDrag.size() + 2u));
  for (std::size_t index = 0; index < handler.performedEditCount; ++index) {
    expect(handler.performedEdits[index].id == fixture.parameterId,
           "every recorded point belongs to the dragged parameter");
  }
  for (std::size_t index = 1; index < handler.performedEditCount; ++index) {
    expect(std::abs(handler.performedEdits[index].value - kLaneBeforeTheDrag) >= 1.0e-9,
           "no point inside the drag carries the value the control held before "
           "the hand arrived -- the host stamps a report with the position it "
           "is rendering when the call arrives, so a second point of the old "
           "value would land inside the new drag and be heard as the control "
           "jumping back to " +
               std::to_string(kLaneBeforeTheDrag) + " partway through the move");
  }

  // The point the whole change exists for. The statement of the old value sits
  // on the position the pointer went down on, and the value the drag opened on
  // reaches the host only after a block has moved the host's position off it,
  // so the two can never collapse onto one position and be back-extrapolated
  // over everything the take held before the drag.
  expect(std::abs(handler.performedEdits[0].value - kLaneBeforeTheDrag) < 1.0e-9 &&
             handler.performedEdits[0].lanePosition == 0,
         "the position the pointer went down on holds the value the control had "
         "then");
  expect(std::abs(handler.performedEdits[1].value - kTouchedValue) < 1.0e-9 &&
             handler.performedEdits[1].lanePosition >
                 handler.performedEdits[0].lanePosition,
         "and the value the drag opened on lands on a later position than it, "
         "one block after the touch was begun");
  for (std::size_t index = 0; index < kDrag.size(); ++index) {
    expect(std::abs(handler.performedEdits[index + 2u].value - kDrag[index]) < 1.0e-9,
           "and the drag's own values follow it in order");
    expect(handler.performedEdits[index + 2u].lanePosition >=
               handler.performedEdits[index + 1u].lanePosition,
           "each on the position it was made on or a later one");
  }
  expect(handler.performedEdits[2].lanePosition ==
             handler.performedEdits[1].lanePosition,
         "the first dragged value shares the block the opening value was "
         "released in, because it is reported in the call that made it");
  for (std::size_t index = 1; index < kDrag.size(); ++index) {
    expect(handler.performedEdits[index + 2u].lanePosition >
               handler.performedEdits[index + 1u].lanePosition,
           "and every value after that one is on a position of its own");
  }

  expect(processor.setActive(false) == kResultOk,
         "deactivate the no-invented-point test");
  expect(processor.terminate() == kResultOk, "terminate the no-invented-point test");
  handler.release();
}

// A touch that is never ended leaves the host believing the user's hand is
// still on the control, and leaves the slot deaf to host automation forever.
// Every way the editor can go away without releasing the pointer has to end it.
void testNoLeakedTouch() {
  {
    auto fixture = openTouchFixture("the terminate leak test");
    expect(fixture.processor->terminate() == kResultOk,
           "terminate with a touch still open");
    expect(fixture.handler->stepCount(TestComponentHandler::EditStep::end) == 1 &&
               !PluginProcessorTestAccess::hostGestureOpen(*fixture.processor,
                                                            fixture.parameterId),
           "terminate() ends the open touch");
    fixture.handler->release();
  }
  {
    auto fixture = openTouchFixture("the deactivate leak test");
    expect(fixture.processor->setActive(true) == kResultOk,
           "activate the deactivate leak test");
    expect(fixture.processor->setActive(false) == kResultOk,
           "deactivate with a touch still open");
    expect(fixture.handler->stepCount(TestComponentHandler::EditStep::end) == 1 &&
               !PluginProcessorTestAccess::hostGestureOpen(*fixture.processor,
                                                            fixture.parameterId),
           "setActive(false) ends the open touch");
    expect(fixture.processor->terminate() == kResultOk,
           "terminate the deactivate leak test");
    expect(fixture.handler->stepCount(TestComponentHandler::EditStep::end) == 1,
           "a touch already ended is not ended twice");
    fixture.handler->release();
  }
  {
    auto fixture = openTouchFixture("the editor close leak test");
    fixture.processor->detachEditor(nullptr);
    expect(fixture.handler->stepCount(TestComponentHandler::EditStep::end) == 1 &&
               !PluginProcessorTestAccess::hostGestureOpen(*fixture.processor,
                                                            fixture.parameterId),
           "closing the editor ends the open touch");
    expect(fixture.processor->terminate() == kResultOk,
           "terminate the editor close leak test");
    fixture.handler->release();
  }
  {
    auto fixture = openTouchFixture("the slot retirement leak test");
    // A preset load, an undo or any edit that drops the plug-in retires its
    // lane. Nothing the editor does afterwards could ever name that slot again.
    const auto rebuilt = choc::json::parse(fixture.processor->handleUiMessage(
        R"({"type":"pipeline/rebuild","payload":{"pipeline":"A","plugins":[)"
        R"({"id":91,"type":"DCOffsetPlugin","name":"DC Offset","enabled":true,)"
        R"("parameters":{"of":0},"wasmParams":[0],)"
        R"("wasmParamsHash":1104945464}]}})"));
    expect(rebuilt["ok"].getWithDefault<bool>(false),
           "rebuild the pipeline without the touched plug-in");
    expect(!PluginProcessorTestAccess::activeAutomationSlot(*fixture.processor,
                                                             fixture.identity)
                .has_value(),
           "the rebuild retired the touched slot");
    expect(fixture.handler->stepCount(TestComponentHandler::EditStep::end) == 1 &&
               !PluginProcessorTestAccess::hostGestureOpen(*fixture.processor,
                                                            fixture.parameterId),
           "retiring the slot ends the touch that was open on it");
    expect(fixture.processor->terminate() == kResultOk,
           "terminate the slot retirement leak test");
    fixture.handler->release();
  }
}

// A state restore replaces the pipeline the user's hand was on and then
// destroys the JS context that would have released it: the reloaded editor
// starts with no gesture targets and sends no close, and a binding that
// survives the restore is not retired either, so nothing else on any side would
// ever end the touch. It would leave the host writing a lane the user is not
// holding, and leave the block ignoring that parameter's input queue for the
// rest of the session.
void testStateRestoreEndsAnOpenTouch() {
  auto fixture = openTouchFixture("the state restore leak test", 81);
  ResizableMemoryIBStream savedState;
  expect(fixture.processor->getState(&savedState) == kResultOk,
         "save the document the restore replays");
  savedState.rewind();
  expect(fixture.processor->setState(&savedState) == kResultOk,
         "restore a state with a touch still open");
  expect(PluginProcessorTestAccess::activeAutomationSlot(*fixture.processor,
                                                          fixture.identity)
             .has_value(),
         "the restored document keeps the touched target bound");
  expect(fixture.handler->stepCount(TestComponentHandler::EditStep::end) == 1 &&
             !PluginProcessorTestAccess::hostGestureOpen(*fixture.processor,
                                                          fixture.parameterId),
         "setState() ends the touch the destroyed editor can no longer release");
  expect(fixture.processor->terminate() == kResultOk,
         "terminate the state restore leak test");
  expect(fixture.handler->stepCount(TestComponentHandler::EditStep::end) == 1,
         "a touch already ended is not ended twice");
  fixture.handler->release();
}

// setState() ends the open touches and then asks the editor to reload, but the
// reload is a request, not a boundary: the old context is still alive across
// it, and a drag still in progress there can flush one more value carrying
// beginGesture. That value reopens a touch on a slot the restore kept, and the
// page that comes back has no gesture targets at all, so nothing in it can ever
// name that touch to close it -- the host would keep writing a lane the user is
// not holding, and the block would keep ignoring that parameter's automation
// for the rest of the session. The reloaded page announcing itself is the
// boundary that closes it, and an ordinary poll must not be.
void testStateRestoreClosesATouchReopenedBeforeTheReload() {
  auto fixture = openTouchFixture("the reload boundary test", 91);
  auto &processor = *fixture.processor;
  auto &handler = *fixture.handler;

  ResizableMemoryIBStream savedState;
  expect(processor.getState(&savedState) == kResultOk,
         "save the document the reload boundary test replays");
  savedState.rewind();
  expect(processor.setState(&savedState) == kResultOk,
         "restore a state with a touch still open");
  expect(handler.stepCount(TestComponentHandler::EditStep::end) == 1 &&
             !PluginProcessorTestAccess::hostGestureOpen(processor, fixture.parameterId),
         "the restore ends the touch the editor can no longer release");

  // The dying page's last frame, flushed after the restore closed the touch and
  // before the reload destroyed the context that emitted it.
  expect(PluginProcessorTestAccess::applyAutomationEdit(
             processor, fixture.identity, 0.625,
             {/*bindIfUnbound=*/true, /*beginGesture=*/true, /*endGesture=*/false}),
         "carry a value the old context flushed between the restore and the reload");
  expect(PluginProcessorTestAccess::hostGestureOpen(processor, fixture.parameterId),
         "that value reopens the touch the restore had ended");

  expect(hostInfo(processor)["ok"].getWithDefault<bool>(false),
         "an ordinary poll is answered");
  expect(PluginProcessorTestAccess::hostGestureOpen(processor, fixture.parameterId),
         "an ordinary poll is not a page boundary and ends nothing");

  const auto startup = choc::json::parse(processor.handleUiMessage(
      R"({"type":"host/getInfo","payload":{"startup":true}})"));
  expect(startup["ok"].getWithDefault<bool>(false),
         "the reloaded page's announcement is answered");
  expect(!PluginProcessorTestAccess::hostGestureOpen(processor, fixture.parameterId),
         "the reloaded page announcing itself ends the touch it could never name");
  expect(handler.stepCount(TestComponentHandler::EditStep::end) == 2,
         "and ends it exactly once");

  // The page that announced itself owns everything after it: a touch it opens
  // is its own, and no later poll may take it away.
  expect(PluginProcessorTestAccess::applyAutomationEdit(
             processor, fixture.identity, 0.5,
             {/*bindIfUnbound=*/true, /*beginGesture=*/true, /*endGesture=*/false}),
         "open a touch inside the reloaded page");
  expect(hostInfo(processor)["ok"].getWithDefault<bool>(false),
         "poll the host while the reloaded page holds a touch");
  expect(PluginProcessorTestAccess::hostGestureOpen(processor, fixture.parameterId) &&
             handler.stepCount(TestComponentHandler::EditStep::end) == 2,
         "the touch the reloaded page is holding survives every poll");

  expect(processor.terminate() == kResultOk, "terminate the reload boundary test");
  handler.release();
}

// An effect power button changes its value from DOM click, after pointerup.
// Its pointerdown therefore has to open the already-bound node-enable touch by
// identity alone; otherwise the later value is emitted as begin/perform/end in
// one call and Read automation can reclaim the old value immediately.
void testClickActivatedControlOpensItsTouchBeforeItsValue() {
  auto fixture = openTouchFixture("the click-activated touch test", 72);
  auto &processor = *fixture.processor;
  auto &handler = *fixture.handler;

  // Return the fixture to a bound but untouched state, which is what exists
  // before the next physical press on an already-automated control.
  const auto initialClose = choc::json::parse(processor.handleUiMessage(
      std::string{R"({"type":"automation/endGesture","payload":{"targets":[)"
                  R"({"pipeline":"A","pluginId":)"} +
      std::to_string(fixture.identity.pluginId) +
      R"(,"pluginType":"DCOffsetPlugin","parameterKey":"of","elementIndex":0}]}})"));
  expect(initialClose["ok"].getWithDefault<bool>(false),
         "close the fixture touch before the click-activated test");
  handler.clearEditLog();

  const auto pressed = choc::json::parse(processor.handleUiMessage(
      std::string{R"({"type":"automation/beginGesture","payload":{"targets":[)"
                  R"({"pipeline":"A","pluginId":)"} +
      std::to_string(fixture.identity.pluginId) +
      R"(,"pluginType":"DCOffsetPlugin","parameterKey":"of","elementIndex":0}]}})"));
  expect(pressed["ok"].getWithDefault<bool>(false),
         "accept the pointerdown boundary without a value");
  expect(PluginProcessorTestAccess::hostGestureOpen(processor,
                                                     fixture.parameterId) &&
             handler.stepCount(TestComponentHandler::EditStep::begin) == 1u &&
             handler.stepCount(TestComponentHandler::EditStep::perform) == 1u &&
             handler.stepCount(TestComponentHandler::EditStep::end) == 0u,
         "pointerdown opens the touch and anchors its previous value");

  const auto beforeClick = processor.getParamNormalized(fixture.parameterId);
  expect(processor.setParamNormalized(fixture.parameterId, 0.0) == kResultTrue &&
             std::abs(processor.getParamNormalized(fixture.parameterId) -
                      beforeClick) < 1.0e-9,
         "Read automation cannot replace the control while its pointer is down");

  constexpr double kClickedValue = 0.25;
  expect(PluginProcessorTestAccess::applyAutomationEdit(
             processor, fixture.identity, kClickedValue,
             {/*bindIfUnbound=*/true, /*beginGesture=*/true,
              /*endGesture=*/false}),
         "apply the click value inside the pointerdown touch");
  expect(PluginProcessorTestAccess::hostGestureOpen(processor,
                                                     fixture.parameterId) &&
             handler.stepCount(TestComponentHandler::EditStep::begin) == 1u &&
             handler.stepCount(TestComponentHandler::EditStep::perform) == 2u &&
             handler.stepCount(TestComponentHandler::EditStep::end) == 0u &&
             std::abs(handler.performedEdits[1].value - kClickedValue) < 1.0e-9,
         "the click value is performed inside the existing touch");

  const auto released = choc::json::parse(processor.handleUiMessage(
      std::string{R"({"type":"automation/endGesture","payload":{"targets":[)"
                  R"({"pipeline":"A","pluginId":)"} +
      std::to_string(fixture.identity.pluginId) +
      R"(,"pluginType":"DCOffsetPlugin","parameterKey":"of","elementIndex":0}]}})"));
  expect(released["ok"].getWithDefault<bool>(false) &&
             !PluginProcessorTestAccess::hostGestureOpen(processor,
                                                          fixture.parameterId) &&
             handler.stepCount(TestComponentHandler::EditStep::begin) == 1u &&
             handler.stepCount(TestComponentHandler::EditStep::perform) == 2u &&
             handler.stepCount(TestComponentHandler::EditStep::end) == 1u,
         "the deferred pointerup closes the one complete touch after click");

  expect(processor.terminate() == kResultOk,
         "terminate the click-activated touch test");
  handler.release();
}

// The editor stamps every value of a drag with beginGesture, not only the first
// one. The open is idempotent, so the drag is still the single touch a host
// keys its automation writer on -- and asking every time is what lets a close
// the editor never saw, made natively by a suspend, a restore or an editor
// detach, be recovered from instead of leaving the rest of the drag with no
// touch window at all.
void testRepeatedBeginKeepsOneTouchAndReopensAfterANativeClose() {
  auto fixture = openTouchFixture("the repeated-begin test", 71);
  auto &processor = *fixture.processor;
  auto &handler = *fixture.handler;
  // The value that opened the touch is let go by the first block after it and
  // carried by the control tick that observes that block; from there every
  // value is reported in the call that made it.
  crossBlockBoundaryAndObserve(processor, "the repeated-begin test");

  for (const double value : {0.75, 0.625, 0.5}) {
    expect(PluginProcessorTestAccess::applyAutomationEdit(
               processor, fixture.identity, value,
               {/*bindIfUnbound=*/true, /*beginGesture=*/true, /*endGesture=*/false}),
           "carry a dragged value that asks for the touch again");
  }
  expect(handler.stepCount(TestComponentHandler::EditStep::begin) == 1,
         "a drag that asks for the touch on every value still opens exactly one");
  // Four dragged values, four reports, behind the one statement of the value
  // the touch opened over. A repeated beginGesture that finds the touch already
  // open opens nothing and reports nothing of its own.
  expect(handler.stepCount(TestComponentHandler::EditStep::perform) == 5,
         "every dragged value is still reported inside that one touch, and "
         "nothing else is");
  expect(handler.stepCount(TestComponentHandler::EditStep::end) == 0,
         "no value inside the drag ends it");
  expect(PluginProcessorTestAccess::hostGestureOpen(processor, fixture.parameterId),
         "the touch is still open at the end of the drag");

  // A close the editor cannot see, and cannot be told about: the pointer is
  // still down and the values keep arriving.
  processor.detachEditor(nullptr);
  expect(handler.stepCount(TestComponentHandler::EditStep::end) == 1 &&
             !PluginProcessorTestAccess::hostGestureOpen(processor,
                                                          fixture.parameterId),
         "the native close ends the open touch");

  expect(PluginProcessorTestAccess::applyAutomationEdit(
             processor, fixture.identity, 0.375,
             {/*bindIfUnbound=*/true, /*beginGesture=*/true, /*endGesture=*/false}),
         "carry a dragged value after the native close");
  expect(PluginProcessorTestAccess::hostGestureOpen(processor, fixture.parameterId),
         "the next dragged value reopens the touch the native side closed");
  expect(handler.stepCount(TestComponentHandler::EditStep::begin) == 2,
         "reopening costs exactly one further beginEdit");
  // The reopen is an opening call like any other: it states the value the
  // control held, holds the value that made it back for one block, and does
  // nothing besides.
  expect(handler.stepCount(TestComponentHandler::EditStep::perform) == 6,
         "the reopened touch states the value it opened over and stops there");
  crossBlockBoundaryAndObserve(processor, "the repeated-begin test");
  expect(handler.stepCount(TestComponentHandler::EditStep::perform) == 7,
         "and the first observation after the block that followed it reports "
         "the value that reopened it, once");

  expect(PluginProcessorTestAccess::applyAutomationEdit(
             processor, fixture.identity, 0.25,
             {/*bindIfUnbound=*/true, /*beginGesture=*/true, /*endGesture=*/true}),
         "release the reopened touch");
  expect(handler.stepCount(TestComponentHandler::EditStep::begin) == 2 &&
             handler.stepCount(TestComponentHandler::EditStep::perform) == 8 &&
             handler.stepCount(TestComponentHandler::EditStep::end) == 2 &&
             !PluginProcessorTestAccess::hostGestureOpen(processor,
                                                          fixture.parameterId),
         "the reopened touch reports its last value and is closed once by the "
         "release");

  expect(processor.terminate() == kResultOk, "terminate the repeated-begin test");
  handler.release();
}

// Retiring a slot ends the touch that was open on it, and where that endEdit is
// issued from matters: restartComponent() is specified as a request the host
// acts on later, but beginEdit and endEdit are how a host is told the user's
// hand arrived on and left a control, and hosts act on them inline. A host that
// answered one by asking for the state, restoring one, or suspending the
// component would re-enter through processingResourcesMutex_, which is not
// recursive.
void testSlotRetirementEndsTheTouchOutsideTheResourceLock() {
  auto fixture = openTouchFixture("the retirement lock test", 61);
  fixture.handler->lockProbe = fixture.processor.get();
  const auto rebuilt = choc::json::parse(fixture.processor->handleUiMessage(
      R"({"type":"pipeline/rebuild","payload":{"pipeline":"A","plugins":[)"
      R"({"id":93,"type":"DCOffsetPlugin","name":"DC Offset","enabled":true,)"
      R"("parameters":{"of":0},"wasmParams":[0],)"
      R"("wasmParamsHash":1104945464}]}})"));
  expect(rebuilt["ok"].getWithDefault<bool>(false),
         "rebuild the pipeline without the touched plug-in");
  expect(!PluginProcessorTestAccess::activeAutomationSlot(*fixture.processor,
                                                           fixture.identity)
              .has_value(),
         "the rebuild retired the touched slot");
  expect(fixture.handler->stepCount(TestComponentHandler::EditStep::end) == 1 &&
             !PluginProcessorTestAccess::hostGestureOpen(*fixture.processor,
                                                          fixture.parameterId),
         "retiring the slot still ends the touch that was open on it");
  expect(!fixture.handler->endEditFoundResourcesLocked,
         "the endEdit a retirement issues is made with processingResourcesMutex_ "
         "released");
  fixture.handler->lockProbe = nullptr;
  expect(fixture.processor->terminate() == kResultOk,
         "terminate the retirement lock test");
  fixture.handler->release();
}

// The master bypass button is the sibling of an automation gesture, and it
// follows the same invariant: the transaction only offers the value to the
// host, and the plug-in adopts it either way. Consulting the answer left the
// editor's toggle flipped over a DSP that was still processing, telemetry
// snapped the button back, and a save recorded the value the DSP was not
// playing.
void testRefusedMasterBypassStillBypassesTheDsp() {
  auto processor = std::make_unique<EffeTuneProcessor>();
  expect(processor->initialize(nullptr) == kResultOk,
         "initialize the refused-bypass test");
  auto *handler = new TestComponentHandler();
  handler->refuseEdits = true;
  expect(processor->setComponentHandler(handler) == kResultOk,
         "install the refused-bypass handler");
  auto processSetup = setup(48000.0, 64);
  expect(processor->setupProcessing(processSetup) == kResultOk,
         "prepare the refused-bypass test");
  const auto installed = choc::json::parse(processor->handleUiMessage(
      R"({"type":"pipeline/rebuild","payload":{"pipeline":"A","plugins":[)"
      R"({"id":33,"type":"DCOffsetPlugin","name":"DC Offset","enabled":true,)"
      R"("parameters":{"of":0.5},"wasmParams":[0.5],)"
      R"("wasmParamsHash":1104945464}]}})"));
  expect(installed["ok"].getWithDefault<bool>(false),
         "install the refused-bypass pipeline");
  expect(processor->setActive(true) == kResultOk, "activate the refused-bypass test");

  std::array<float, 64> inputLeft{};
  std::array<float, 64> inputRight{};
  std::array<float, 64> outputLeft{};
  std::array<float, 64> outputRight{};
  Sample32 *inputChannels[]{inputLeft.data(), inputRight.data()};
  Sample32 *outputChannels[]{outputLeft.data(), outputRight.data()};
  AudioBusBuffers input{};
  input.numChannels = 2;
  input.channelBuffers32 = inputChannels;
  AudioBusBuffers output{};
  output.numChannels = 2;
  output.channelBuffers32 = outputChannels;
  ProcessData data{};
  data.symbolicSampleSize = kSample32;
  data.numSamples = 64;
  data.numInputs = 1;
  data.numOutputs = 1;
  data.inputs = &input;
  data.outputs = &output;
  data.inputParameterChanges = nullptr;

  const auto render = [&](const float expected, const std::string &message) {
    outputLeft.fill(0.0f);
    outputRight.fill(0.0f);
    expect(processor->process(data) == kResultOk, "process the refused-bypass block");
    for (const auto sample : outputLeft) {
      expect(std::abs(sample - expected) < 1.0e-6f, message);
    }
  };

  render(0.5f, "the engaged pipeline offsets the silent input");

  const auto toggled = choc::json::parse(processor->handleUiMessage(
      R"({"type":"pipeline/masterBypass","payload":{"value":true}})"));
  expect(toggled["ok"].getWithDefault<bool>(false),
         "a bypass edit the host refuses to record is still answered as success");
  render(0.0f, "master bypass reaches the DSP though the host refused the edit");
  expect(hostInfo(*processor)["masterBypass"].getWithDefault<bool>(false),
         "telemetry reports the bypass the user asked for, so nothing snaps back");
  // A refused beginEdit returns before the transaction ever writes the
  // parameter object, so the explicit write after it is the only thing that
  // leaves the host parameter on the value the DSP is playing. Without it the
  // host reads the button as still off -- and the next getParamNormalized the
  // host asks for, or the next automation pass it renders from that parameter,
  // would unbypass the plug-in behind the user's back.
  expect(processor->getParamNormalized(kBypassParameterId) == 1.0,
         "the host parameter carries the bypass the DSP is playing");

  ResizableMemoryIBStream savedState;
  expect(processor->getState(&savedState) == kResultOk,
         "save the state a refused bypass leaves behind");
  effetune::vst::PluginStateDocument saved;
  std::string decodeError;
  expect(effetune::vst::StateCodec::decode(
             std::string(static_cast<const char *>(savedState.getData()),
                         savedState.getCursor()),
             saved, &decodeError),
         "decode the saved refused-bypass document: " + decodeError);
  expect(saved.masterBypass,
         "the saved document holds the bypass the DSP is playing");

  expect(processor->setActive(false) == kResultOk,
         "deactivate the refused-bypass test");
  expect(processor->terminate() == kResultOk, "terminate the refused-bypass test");
  handler->release();
}

// An older UI sends automation edits with no gesture fields at all. Both flags
// default to true there, so every such edit stays exactly what it has always
// been: one complete touch per value.
void testAutomationEditWithoutGestureFieldsStaysAOneShot() {
  auto processor = std::make_unique<EffeTuneProcessor>();
  expect(processor->initialize(nullptr) == kResultOk,
         "initialize the legacy gesture payload test");
  auto *handler = new TestComponentHandler();
  expect(processor->setComponentHandler(handler) == kResultOk,
         "install the legacy gesture payload handler");
  auto processSetup = setup(48000.0, 64);
  expect(processor->setupProcessing(processSetup) == kResultOk,
         "prepare the legacy gesture payload test");
  const auto installed = choc::json::parse(processor->handleUiMessage(
      R"({"type":"pipeline/rebuild","payload":{"pipeline":"A","plugins":[)"
      R"({"id":51,"type":"DCOffsetPlugin","name":"DC Offset","enabled":true,)"
      R"("parameters":{"of":0},"wasmParams":[0],)"
      R"("wasmParamsHash":1104945464}]}})"));
  expect(installed["ok"].getWithDefault<bool>(false),
         "install the legacy gesture payload pipeline");
  const effetune::vst::AutomationTargetIdentity identity{'A', 51, "DCOffsetPlugin",
                                                          "of", 0};
  const auto parameterId = boundAutomationParameterId(*processor, identity);
  handler->clearEditLog();

  const auto legacyEdit = choc::json::parse(processor->handleUiMessage(
      R"({"type":"automation/edit","payload":{"pipeline":"A","pluginId":51,)"
      R"("pluginType":"DCOffsetPlugin","parameterKey":"of","elementIndex":0,)"
      R"("normalized":0.625}})"));
  expect(legacyEdit["ok"].getWithDefault<bool>(false) &&
             legacyEdit["bound"].getWithDefault<bool>(false),
         "a payload without gesture fields is still an accepted edit");
  // One complete touch is a begin, the value it opened over, the value the
  // user made -- flushed by the close, since no block separates them -- and an
  // end.
  expect(handler->stepCount(TestComponentHandler::EditStep::begin) == 1 &&
             handler->stepCount(TestComponentHandler::EditStep::perform) == 2 &&
             handler->stepCount(TestComponentHandler::EditStep::end) == 1 &&
             handler->editLogCount == 4,
         "a payload without gesture fields is reported as one complete touch");
  expect(!PluginProcessorTestAccess::hostGestureOpen(*processor, parameterId),
         "a payload without gesture fields leaves no touch open");

  // The bulk array route reads the same defaults.
  handler->clearEditLog();
  const auto bundled = choc::json::parse(processor->handleUiMessage(
      R"({"type":"pipeline/updatePlugin","payload":{"pipeline":"A","plugin":)"
      R"({"id":51,"type":"DCOffsetPlugin","name":"DC Offset","enabled":true,)"
      R"("parameters":{"of":0.5},"wasmParams":[0.5],"wasmParamsHash":1104945464},)"
      R"("automationEdits":[{"pipeline":"A","pluginId":51,)"
      R"("pluginType":"DCOffsetPlugin","parameterKey":"of","elementIndex":0,)"
      R"("normalized":0.75}]}})"));
  expect(bundled["ok"].getWithDefault<bool>(false),
         "a bulk payload without gesture fields is accepted");
  expect(handler->stepCount(TestComponentHandler::EditStep::begin) == 1 &&
             handler->stepCount(TestComponentHandler::EditStep::perform) == 2 &&
             handler->stepCount(TestComponentHandler::EditStep::end) == 1,
         "a bundled edit without gesture fields is one complete touch too");
  expect(!PluginProcessorTestAccess::hostGestureOpen(*processor, parameterId),
         "a bundled edit without gesture fields leaves no touch open");

  expect(processor->terminate() == kResultOk,
         "terminate the legacy gesture payload test");
  handler->release();
}

// A linked control writes several parameters from one pointer move: a linked
// multi-channel panel propagates one fader across every linked channel, a PEQ
// graph marker writes a band's frequency and its gain together. The host has to
// stamp all of them at the same instant, which is what
// IComponentHandler2::startGroupEdit/finishGroupEdit are for -- without the
// group the recorded take is a staircase of points the user never made. A batch
// carrying a single edit earns no group: it has nothing to synchronize with.
void testLinkedMultiParameterBatchIsOneHostGroupEdit() {
  auto processor = std::make_unique<EffeTuneProcessor>();
  expect(processor->initialize(nullptr) == kResultOk,
         "initialize the group edit test");
  auto *handler = new TestComponentHandler();
  expect(processor->setComponentHandler(handler) == kResultOk,
         "install the group edit handler");
  auto processSetup = setup(48000.0, 64);
  expect(processor->setupProcessing(processSetup) == kResultOk,
         "prepare the group edit test");

  // Eight linked channel volumes on one plug-in: the widest batch the UI can
  // produce from a single drag.
  constexpr std::size_t kLinkedChannels = 8;
  const auto panelPlugin = [](const char *volumes) {
    constexpr std::size_t kPackedParameterCount = 79;
    std::string packedParameters = "0";
    for (std::size_t index = 1; index < kPackedParameterCount; ++index) {
      packedParameters += ",0";
    }
    return std::string{R"({"id":81,"type":"MultiChannelPanelPlugin",)"
                       R"("name":"Multi Channel Panel","enabled":true,)"
                       R"("parameters":{"v":)"} +
           volumes + R"(,"d":[0,0,0,0,0,0,0,0]},"wasmParams":[)" +
           packedParameters + R"(],"wasmParamsHash":2638026937})";
  };
  const auto installed = choc::json::parse(processor->handleUiMessage(
      std::string{R"({"type":"pipeline/rebuild","payload":{"pipeline":"A",)"
                  R"("plugins":[)"} +
      panelPlugin("[0,0,0,0,0,0,0,0]") + "]}}"));
  expect(installed["ok"].getWithDefault<bool>(false),
         "install the linked multi-channel panel pipeline");

  std::string edits;
  for (std::size_t channel = 0; channel < kLinkedChannels; ++channel) {
    edits += edits.empty() ? "" : ",";
    edits += std::string{R"({"pipeline":"A","pluginId":81,)"
                         R"("pluginType":"MultiChannelPanelPlugin",)"
                         R"("parameterKey":"v","elementIndex":)"} +
             std::to_string(channel) + R"(,"normalized":0.75})";
  }
  handler->clearEditLog();
  const auto linked = choc::json::parse(processor->handleUiMessage(
      std::string{R"({"type":"pipeline/updatePlugin","payload":{"pipeline":"A",)"
                  R"("plugin":)"} +
      panelPlugin("[2.5,2.5,2.5,2.5,2.5,2.5,2.5,2.5]") +
      R"(,"automationEdits":[)" + edits + "]}}"));
  expect(linked["ok"].getWithDefault<bool>(false),
         "accept the linked multi-parameter frame");
  expect(handler->stepCount(TestComponentHandler::EditStep::begin) ==
                 kLinkedChannels &&
             handler->stepCount(TestComponentHandler::EditStep::end) ==
                 kLinkedChannels,
         "every linked channel is still reported as its own touch");
  expect(handler->stepCount(TestComponentHandler::EditStep::startGroup) == 1 &&
             handler->stepCount(TestComponentHandler::EditStep::finishGroup) == 1,
         "the whole batch is wrapped in exactly one group edit");
  expect(handler->maximumGroupDepth == 1 && handler->openGroupDepth == 0 &&
             !handler->unbalancedGroupFinish,
         "the group opens once, nests no deeper and is closed again");
  const auto logged = std::min(handler->editLogCount, handler->editLog.size());
  // The group is located rather than assumed to be first: this batch binds
  // eight lanes on demand, and each claim republishes the parameter bank
  // through restartComponent(), which is deliberately issued before the group
  // opens. What the boundaries have to contain is the begin/perform/end run.
  const auto stepIndex = [&](const TestComponentHandler::EditStep step) {
    for (std::size_t index = 0; index < logged; ++index) {
      if (handler->editLog[index].step == step) {
        return index;
      }
    }
    return logged;
  };
  const auto groupOpen = stepIndex(TestComponentHandler::EditStep::startGroup);
  const auto groupClose = stepIndex(TestComponentHandler::EditStep::finishGroup);
  expect(groupOpen < groupClose && groupClose < logged,
         "the batch opens a group and closes it again");
  const auto isTouchStep = [](const TestComponentHandler::EditStep step) {
    return step == TestComponentHandler::EditStep::begin ||
           step == TestComponentHandler::EditStep::perform ||
           step == TestComponentHandler::EditStep::end;
  };
  for (std::size_t index = 0; index < logged; ++index) {
    const auto inside = index > groupOpen && index < groupClose;
    if (isTouchStep(handler->editLog[index].step)) {
      expect(inside, "every edit of the batch falls inside the one open group");
    }
    // The narrowing under test. A host answers restartComponent by
    // re-enumerating the parameter bank, and being asked to do that while it
    // believes a group edit is in progress is what the SDK's worked example
    // avoids by bracketing begin/perform/end and nothing else. On-demand
    // binding fires on the first frame of a drag on an unbound target, so this
    // is the common path, not a corner.
    if (handler->editLog[index].step == TestComponentHandler::EditStep::restart) {
      expect(!inside,
             "on-demand binding restarts the component outside the group edit");
    }
  }
  expect(handler->stepCount(TestComponentHandler::EditStep::restart) != 0,
         "the batch really did bind its lanes on demand, so the check above had "
         "a restart to place");

  // A single-edit frame is the ordinary case, and a group around one edit
  // synchronizes nothing.
  handler->clearEditLog();
  const auto single = choc::json::parse(processor->handleUiMessage(
      std::string{R"({"type":"pipeline/updatePlugin","payload":{"pipeline":"A",)"
                  R"("plugin":)"} +
      panelPlugin("[5,2.5,2.5,2.5,2.5,2.5,2.5,2.5]") +
      R"(,"automationEdits":[{"pipeline":"A","pluginId":81,)"
      R"("pluginType":"MultiChannelPanelPlugin","parameterKey":"v",)"
      R"("elementIndex":0,"normalized":0.8}]}})"));
  expect(single["ok"].getWithDefault<bool>(false),
         "accept the single-parameter frame");
  expect(handler->stepCount(TestComponentHandler::EditStep::startGroup) == 0 &&
             handler->stepCount(TestComponentHandler::EditStep::finishGroup) == 0,
         "a batch of one edit opens no group");

  // The release of the same linked control closes every touch, and those closes
  // have to land together too.
  std::string targets;
  for (std::size_t channel = 0; channel < kLinkedChannels; ++channel) {
    targets += targets.empty() ? "" : ",";
    targets += std::string{R"({"pipeline":"A","pluginId":81,)"
                           R"("pluginType":"MultiChannelPanelPlugin",)"
                           R"("parameterKey":"v","elementIndex":)"} +
               std::to_string(channel) + "}";
  }
  handler->clearEditLog();
  const auto released = choc::json::parse(processor->handleUiMessage(
      std::string{R"({"type":"automation/endGesture","payload":{"targets":[)"} +
      targets + "]}}"));
  expect(released["ok"].getWithDefault<bool>(false),
         "accept the linked gesture release");
  expect(handler->stepCount(TestComponentHandler::EditStep::startGroup) == 1 &&
             handler->stepCount(TestComponentHandler::EditStep::finishGroup) == 1 &&
             handler->openGroupDepth == 0 && !handler->unbalancedGroupFinish,
         "a linked release is wrapped in one balanced group edit as well");

  handler->clearEditLog();
  const auto releasedOne = choc::json::parse(processor->handleUiMessage(
      R"({"type":"automation/endGesture","payload":{"targets":[)"
      R"({"pipeline":"A","pluginId":81,)"
      R"("pluginType":"MultiChannelPanelPlugin","parameterKey":"v",)"
      R"("elementIndex":0}]}})"));
  expect(releasedOne["ok"].getWithDefault<bool>(false),
         "accept the single-target release");
  expect(handler->stepCount(TestComponentHandler::EditStep::startGroup) == 0,
         "a release naming one target opens no group either");

  expect(processor->terminate() == kResultOk, "terminate the group edit test");
  handler->release();
}

// A host draws its automation lane and its generic editor from
// getParamStringByValue. Without an override the base class prints the raw
// normalized position -- "0.5000" against a lane whose unit says dB -- because a
// Parameter holds nothing but the normalized value. The scale that gives it
// meaning lives in the binding registry, so the string has to be built there.
void testBoundSlotsRenderDenormalizedDisplayStrings() {
  auto processor = std::make_unique<EffeTuneProcessor>();
  expect(processor->initialize(nullptr) == kResultOk,
         "initialize the display string test");
  auto processSetup = setup(48000.0, 64);
  expect(processor->setupProcessing(processSetup) == kResultOk,
         "prepare the display string test");
  const auto installed = choc::json::parse(processor->handleUiMessage(
      R"({"type":"pipeline/rebuild","payload":{"pipeline":"A","plugins":[)"
      R"({"id":91,"type":"VolumePlugin","name":"Volume","enabled":true,)"
      R"("parameters":{"vl":-12},"wasmParams":[-12],)"
      R"("wasmParamsHash":1719233191},)"
      R"({"id":92,"type":"AutoFilterPlugin","name":"Auto Filter","enabled":true,)"
      R"("parameters":{"lf":200,"hf":4000,"rs":1.5,"mx":80,"rt":0.5,"sp":0,)"
      R"("sn":24,"at":20,"rl":250},)"
      R"("wasmParams":[0,0,200,4000,1.5,80,0.5,0,0,24,20,250,0],)"
      R"("wasmParamsHash":1612236675}]}})"));
  expect(installed["ok"].getWithDefault<bool>(false),
         "install the display string pipeline");

  // Linear, with a unit: -60..24 dB. Half the lane is -18 dB, and that is what
  // the row has to read -- not 0.5000. Volume declares step 0.1, so the digits
  // are the one decimal the EffeTune window itself shows.
  const auto volumeId =
      boundAutomationParameterId(*processor, {'A', 91, "VolumePlugin", "vl", 0});
  expect(displayString(*processor, volumeId, 0.5) == "-18.0",
         "a linear slot renders the denormalized value, not the lane position");
  expect(displayString(*processor, volumeId, 0.0) == "-60.0" &&
             displayString(*processor, volumeId, 1.0) == "24.0",
         "and it renders both ends of its own range");
  ParameterInfo volumeInfo{};
  expect(processor->getParameterInfoByTag(volumeId, volumeInfo) == kResultTrue &&
             asciiString(volumeInfo.units) == "dB",
         "the unit stays in ParameterInfo, where the host reads it from");
  expect((volumeInfo.flags & ParameterInfo::kIsList) == 0,
         "a continuous slot is not a list");

  // Logarithmic, 20..20000 Hz: the midpoint of the lane is the geometric mean,
  // which a linear reading would put at 10010 Hz instead of 632 Hz. The step is
  // 1 Hz, so the whole curve prints whole numbers, exactly as the window does.
  const auto filterId = boundAutomationParameterId(
      *processor, {'A', 92, "AutoFilterPlugin", "lf", 0});
  expect(displayString(*processor, filterId, 0.5) == "632",
         "a logarithmic slot renders its own curve");
  expect(displayString(*processor, filterId, 0.0) == "20" &&
             displayString(*processor, filterId, 1.0) == "20000",
         "and both ends of it");

  // The bypass parameter is not an automation slot. It carries stepCount 1 and
  // already renders correctly through the base class, so the override has to
  // leave it alone rather than denormalize it against a scale it does not have.
  expect(displayString(*processor, kBypassParameterId, 1.0) == "On" &&
             displayString(*processor, kBypassParameterId, 0.0) == "Off",
         "the bypass parameter keeps its base-class On/Off rendering");

  // An unbound slot has no scale and publishes no unit, so the lane position is
  // the only honest number there is. It must not crash or read descriptor state
  // that was never written.
  const auto unboundId = effetune::vst::plugin::automationParameterId(200);
  expect(displayString(*processor, unboundId, 0.25) == "0.2500",
         "an unbound slot falls back to the base-class rendering");
  ParamValue unboundParsed = -1.0;
  expect(processor->getParamValueByString(
             unboundId, utf16Of("0.25").data(), unboundParsed) == kResultTrue &&
             std::abs(unboundParsed - 0.25) < 1.0e-9,
         "and parses back through the base class too");

  // Round trip. A typed value that cannot be turned back into a lane position
  // would silently replace what the user typed with something else.
  const auto roundTrip = [&](const ParamID id, const double normalized) {
    const auto text = displayString(*processor, id, normalized);
    ParamValue parsed = -1.0;
    expect(processor->getParamValueByString(id, utf16Of(text).data(), parsed) ==
               kResultTrue,
           "the printed string parses back: " + text);
    return parsed;
  };
  expect(std::abs(roundTrip(volumeId, 0.5) - 0.5) < 1.0e-9,
         "a linear slot round-trips its display string exactly");
  // A logarithmic slot cannot round-trip to an arbitrary tolerance: 632 Hz is
  // what the window prints at the midpoint, and one printed hertz is 2.3e-4 of
  // this lane on its own. What must hold is that parsing the printed string
  // lands on a position that prints the same string again, so a user who edits
  // a row and presses Enter without changing anything does not move the value.
  expect(displayString(*processor, filterId, roundTrip(filterId, 0.5)) == "632",
         "a logarithmic slot round-trips to the printed resolution");
  // Hosts hand the unit back with the number, so the parser has to tolerate it.
  ParamValue withUnit = -1.0;
  expect(processor->getParamValueByString(volumeId, utf16Of("-18.00 dB").data(),
                                          withUnit) == kResultTrue &&
             std::abs(withUnit - 0.5) < 1.0e-9,
         "a typed value keeps its meaning when the unit comes with it");
  ParamValue refused = -1.0;
  expect(processor->getParamValueByString(volumeId, utf16Of("loud").data(),
                                          refused) == kResultFalse &&
             refused == -1.0,
         "text carrying no number is refused rather than read as zero");

  expect(processor->terminate() == kResultOk, "terminate the display string test");
}

// A host row has to carry the same digits the EffeTune window carries. The
// window gets them from the control's step -- plugins/plugin-base.js
// createParameterControl() prints toFixed(step < 0.01 ? 3 : (step < 0.1 ? 2 :
// (step < 1 ? 1 : 0))) -- so a threshold the window shows as -6.0 dB must not
// reach the host as -6.0000. Each parameter below sits in a different bucket of
// that rule, and the step travels with it from params.json through the
// generated catalog.
void testDisplayStringsCarryTheStepsOwnDecimalCount() {
  auto processor = std::make_unique<EffeTuneProcessor>();
  expect(processor->initialize(nullptr) == kResultOk,
         "initialize the display precision test");
  auto processSetup = setup(48000.0, 64);
  expect(processor->setupProcessing(processSetup) == kResultOk,
         "prepare the display precision test");
  const auto installed = choc::json::parse(processor->handleUiMessage(
      R"({"type":"pipeline/rebuild","payload":{"pipeline":"A","plugins":[)"
      R"({"id":81,"type":"CompressorPlugin","name":"Compressor","enabled":true,)"
      R"("parameters":{"th":-24,"rt":2,"at":10,"rl":100,"kn":3,"gn":0},)"
      R"("wasmParams":[-24,2,10,100,3,0],"wasmParamsHash":763850658},)"
      R"({"id":82,"type":"DattorroPlateReverbPlugin","name":"Plate Reverb",)"
      R"("enabled":true,"parameters":{"pd":10,"bw":0.9995,"id1":0.75,)"
      R"("id2":0.625,"dc":0.5,"dd1":0.7,"dp":0.0005,"md":1,"mr":1,)"
      R"("wm":30,"dm":100},)"
      R"("wasmParams":[10,0.9995,0.75,0.625,0.5,0.7,0.0005,1,1,30,100],)"
      R"("wasmParamsHash":582778991}]}})"));
  expect(installed["ok"].getWithDefault<bool>(false),
         "install the display precision pipeline");

  // step 1 dB over -60..0: whole numbers, so the midpoint reads -30.
  const auto thresholdId = boundAutomationParameterId(
      *processor, {'A', 81, "CompressorPlugin", "th", 0});
  // step 0.1 dB over -12..12: one decimal. This is the reported defect -- the
  // window shows -6.0 dB and the host row used to show -6.0000.
  const auto gainId = boundAutomationParameterId(
      *processor, {'A', 81, "CompressorPlugin", "gn", 0});
  // step 0.01 over a logarithmic 0.5..20: two decimals at the bottom of the
  // curve, where a value-derived rule would have printed four.
  const auto ratioId = boundAutomationParameterId(
      *processor, {'A', 81, "CompressorPlugin", "rt", 0});
  // step 0.001 over 0..1: three decimals, the finest the rule goes.
  const auto bandwidthId = boundAutomationParameterId(
      *processor, {'A', 82, "DattorroPlateReverbPlugin", "bw", 0});
  expect(displayString(*processor, thresholdId, 0.5) == "-30",
         "a step of 1 prints no decimals at all");
  expect(displayString(*processor, gainId, 0.25) == "-6.0",
         "a step of 0.1 prints exactly one decimal");
  expect(displayString(*processor, ratioId, 0.0) == "0.50",
         "a step of 0.01 prints exactly two decimals");
  expect(displayString(*processor, bandwidthId, 0.25) == "0.250",
         "a step of 0.001 prints exactly three decimals");

  expect(processor->terminate() == kResultOk,
         "terminate the display precision test");
}

// The value a plug-in's parameter carries in the saved document, found by
// logical id: the host-gate fixture build preloads pipeline A with plug-ins of
// its own, so the plug-in under test is not always first.
[[nodiscard]] double savedPluginParameter(EffeTuneProcessor &processor,
                                          const std::int64_t logicalId,
                                          const std::string &key) {
  ResizableMemoryIBStream savedState;
  expect(processor.getState(&savedState) == kResultOk, "save the state document");
  effetune::vst::PluginStateDocument saved;
  std::string decodeError;
  expect(effetune::vst::StateCodec::decode(
             std::string(static_cast<const char *>(savedState.getData()),
                         savedState.getCursor()),
             saved, &decodeError),
         "decode the saved document: " + decodeError);
  const auto savedPlugin =
      std::find_if(saved.pipelineA.plugins.begin(), saved.pipelineA.plugins.end(),
                   [logicalId](const effetune::vst::PluginState &plugin) {
                     return plugin.id == logicalId;
                   });
  expect(savedPlugin != saved.pipelineA.plugins.end(),
         "the saved document still carries the plug-in");
  return choc::json::parse(savedPlugin->parametersJson)[key.c_str()]
      .getWithDefault<double>(-1000.0);
}

[[nodiscard]] std::string savedPluginStringParameter(
    EffeTuneProcessor &processor, const std::int64_t logicalId,
    const std::string &key) {
  ResizableMemoryIBStream savedState;
  expect(processor.getState(&savedState) == kResultOk, "save the state document");
  effetune::vst::PluginStateDocument saved;
  std::string decodeError;
  expect(effetune::vst::StateCodec::decode(
             std::string(static_cast<const char *>(savedState.getData()),
                         savedState.getCursor()),
             saved, &decodeError),
         "decode the saved document: " + decodeError);
  const auto savedPlugin =
      std::find_if(saved.pipelineA.plugins.begin(), saved.pipelineA.plugins.end(),
                   [logicalId](const effetune::vst::PluginState &plugin) {
                     return plugin.id == logicalId;
                   });
  expect(savedPlugin != saved.pipelineA.plugins.end(),
         "the saved document still carries the plug-in");
  return choc::json::parse(savedPlugin->parametersJson)[key.c_str()]
      .getWithDefault<std::string>({});
}

void testSteppedIntegerAutomationMatchesStateTextAndAudio() {
  auto processor = std::make_unique<EffeTuneProcessor>();
  expect(processor->initialize(nullptr) == kResultOk, "initialize stepped integer automation");
  auto processSetup = setup(48000, 64);
  expect(processor->setupProcessing(processSetup) == kResultOk,
         "prepare stepped integer automation");
  const auto installed = choc::json::parse(processor->handleUiMessage(
      R"({"type":"pipeline/rebuild","payload":{"pipeline":"A","plugins":[)"
      R"({"id":181,"type":"PhaserPlugin","name":"Phaser","enabled":true,)"
      R"("parameters":{"md":"Classic","rt":0.5,"cf":1000,"rg":3,"st":6,)"
      R"("fb":20,"sp":90,"dr":"Up","mx":50},)"
      R"("wasmParams":[0,0.5,1000,3,6,20,90,0,50],"wasmParamsHash":2823426285},)"
      R"({"id":182,"type":"BandPassFilterPlugin","name":"Band Pass","enabled":true,)"
      R"("parameters":{"hf":1000,"lf":1000,"hs":-24,"ls":-24},)"
      R"("wasmParams":[1000,1000,-24,-24],"wasmParamsHash":2360494665}]}})"));
  expect(installed["ok"].getWithDefault<bool>(false), "install the real Phaser stage target");
  const auto stagesId = boundAutomationParameterId(
      *processor, {'A', 181, "PhaserPlugin", "st", 0});
  const auto slopeId = boundAutomationParameterId(
      *processor, {'A', 182, "BandPassFilterPlugin", "hs", 0});
  ParameterInfo info{};
  expect(processor->getParameterInfoByTag(stagesId, info) == kResultTrue &&
             info.stepCount == 5 && info.defaultNormalizedValue == 0.4 &&
             processor->getParamNormalized(stagesId) == 0.4,
         "Phaser defaults and current six stages occupy position 0.4");
  expect(processor->setActive(true) == kResultOk, "activate stepped integer automation");
  expect(processor->setParamNormalized(stagesId, 1.0) == kResultTrue &&
             savedPluginParameter(*processor, 181, "st") == 12.0,
         "a controller-only maximum writes twelve stages to saved state");
  gate_ordering::AudioRig rig(0.1f);
  constexpr std::array positions{0.0, 0.4, 1.0};
  constexpr std::array stages{2, 6, 12};
  for (std::size_t index = 0; index < positions.size(); ++index) {
    const auto text = std::to_string(stages[index]);
    expect(displayString(*processor, stagesId, positions[index]) == text,
           "host display prints the stepped stage count");
    ParamValue parsed = -1;
    expect(processor->getParamValueByString(stagesId, utf16Of(text).data(), parsed) ==
               kResultTrue && parsed == positions[index],
           "typed stage counts round-trip to the matching normalized position");
    ParameterChanges changes(1);
    int32 queueIndex = 0;
    auto *queue = changes.addParameterData(stagesId, queueIndex);
    int32 pointIndex = 0;
    expect(queue && queue->addPoint(0, positions[index], pointIndex) == kResultTrue,
           "queue the host stage position");
    rig.data.inputParameterChanges = &changes;
    tresult processed;
    {
      effetune::allocation_guard::Scope noAudioAllocation;
      processed = processor->process(rig.data);
    }
    rig.data.inputParameterChanges = nullptr;
    expect(processed == kResultOk &&
               PluginProcessorTestAccess::runtimePackedParameter(*processor, 181, 4) ==
                   stages[index] &&
               savedPluginParameter(*processor, 181, "st") == stages[index],
           "the real-time parameter image and saved state match the host stage count");
  }
  ParamValue refused = -1;
  expect(processor->getParamValueByString(stagesId, utf16Of("7").data(), refused) ==
             kResultFalse && refused == -1,
         "an unrepresentable stage count is not silently rounded to another value");
  ParamValue slope = -1;
  expect(displayString(*processor, slopeId, 0.25) == "-36" &&
             processor->getParamValueByString(slopeId, utf16Of("-36").data(), slope) ==
                 kResultTrue && slope == 0.25,
         "negative nondefault slope text round-trips with a twelve-unit step");
  expect(processor->getParamValueByString(slopeId, utf16Of("-42").data(), refused) ==
             kResultFalse && refused == -1,
         "a typed slope outside its step grid is refused");
  expect(processor->setActive(false) == kResultOk, "deactivate stepped integer automation");
  expect(processor->terminate() == kResultOk, "terminate stepped integer automation");
}

// State chunks become the save/UI authority before the WebView replacement is
// available. A pipeline the bridge can never rebuild must therefore be
// rejected by the codec, leaving the complete previous authority playable and
// never entering replacement-pending state.
void testInvalidStatePipelineNeverReplacesTheAuthority() {
  auto processor = std::make_unique<EffeTuneProcessor>();
  expect(processor->initialize(nullptr) == kResultOk,
         "initialize the invalid state-pipeline test");
  auto processSetup = setup(48000.0, 64);
  expect(processor->setupProcessing(processSetup) == kResultOk,
         "prepare the invalid state-pipeline test");
  const auto installed = choc::json::parse(processor->handleUiMessage(
      R"({"type":"pipeline/rebuild","payload":{"pipeline":"A","plugins":[)"
      R"({"id":901,"type":"DCOffsetPlugin","name":"Authoritative Offset",)"
      R"("enabled":true,"parameters":{"of":-0.25},"wasmParams":[-0.25],)"
      R"("wasmParamsHash":1104945464}]}})"));
  expect(installed["ok"].getWithDefault<bool>(false),
         "install the authority the invalid chunks must preserve");

  ResizableMemoryIBStream savedBefore;
  expect(processor->getState(&savedBefore) == kResultOk,
         "save the authority before invalid chunks");
  const std::string serializedBefore{
      static_cast<const char *>(savedBefore.getData()), savedBefore.getCursor()};
  const auto applyStateJson = [&](const std::string &json) {
    ResizableMemoryIBStream stream(json.size());
    int32 bytesWritten = 0;
    expect(stream.write(const_cast<char *>(json.data()),
                        static_cast<int32>(json.size()), &bytesWritten) ==
                   kResultOk &&
               bytesWritten == static_cast<int32>(json.size()),
           "write an invalid state chunk");
    stream.rewind();
    return processor->setState(&stream);
  };

  const std::string duplicateIds =
      R"({"pipelineA":[{"id":7,"name":"First"},{"id":7,"name":"Second","unknown":true}]})";
  expect(applyStateJson(duplicateIds) == kResultFalse,
         "reject a state chunk with duplicate positive IDs");

  std::string excessivePipeline{R"({"pipelineA":[)"};
  for (std::uint32_t id = 1; id <= 129u; ++id) {
    if (id != 1u) {
      excessivePipeline += ',';
    }
    excessivePipeline += R"({"id":)" + std::to_string(id) +
                         R"(,"name":"Future","unknown":true})";
  }
  excessivePipeline += "]}";
  expect(applyStateJson(excessivePipeline) == kResultFalse,
         "reject a state chunk with 129 nodes");

  std::string excessiveNativePipeline{R"({"pipelineA":[)"};
  for (std::uint32_t id = 1; id <= 97u; ++id) {
    if (id != 1u) {
      excessiveNativePipeline += ',';
    }
    excessiveNativePipeline += R"({"id":)" + std::to_string(id) +
                               R"(,"type":"DCOffsetPlugin","name":"Offset"})";
  }
  excessiveNativePipeline += "]}";
  expect(applyStateJson(excessiveNativePipeline) == kResultFalse,
         "reject a state chunk above the native instance capacity");

  ResizableMemoryIBStream savedAfter;
  expect(processor->getState(&savedAfter) == kResultOk,
         "save the authority after invalid chunks");
  const std::string serializedAfter{
      static_cast<const char *>(savedAfter.getData()), savedAfter.getCursor()};
  const auto info = hostInfo(*processor);
  expect(serializedAfter == serializedBefore &&
             std::abs(PluginProcessorTestAccess::runtimePackedParameter(
                          *processor, 901, 0) +
                      0.25f) < 1.0e-6f &&
             info["dspReady"].getWithDefault<bool>(false) &&
             !info["stateReplacementPending"].getWithDefault<bool>(true),
         "invalid chunks leave the old document, runtime and ready state authoritative");

  expect(processor->setActive(true) == kResultOk,
         "activate after the invalid chunks");
  gate_ordering::AudioRig rig(0.0f);
  expect(processor->process(rig.data) == kResultOk &&
             std::abs(rig.outputLeft[0] + 0.25f) < 1.0e-6f,
         "the rejected chunks leave the old wet DSP playable");
  expect(processor->setActive(false) == kResultOk,
         "deactivate the invalid state-pipeline test");

  std::string forwardCompatiblePipeline{R"({"pipelineA":[)"};
  for (std::uint32_t id = 1; id <= 128u; ++id) {
    if (id != 1u) {
      forwardCompatiblePipeline += ',';
    }
    if (id <= 96u) {
      forwardCompatiblePipeline += R"({"id":)" + std::to_string(id) +
                                   R"(,"type":"DCOffsetPlugin","name":"Offset"})";
    } else if (id <= 112u) {
      forwardCompatiblePipeline += R"({"id":)" + std::to_string(id) +
                                   R"(,"type":"FuturePlugin","name":"Future","unknown":true})";
    } else {
      forwardCompatiblePipeline += R"({"id":)" + std::to_string(id) +
                                   R"(,"type":"SectionPlugin","name":"Section"})";
    }
  }
  forwardCompatiblePipeline += "]}";
  expect(applyStateJson(forwardCompatiblePipeline) == kResultOk,
         "unknown and Section nodes do not consume native state capacity");
  ResizableMemoryIBStream forwardCompatibleState;
  expect(processor->getState(&forwardCompatibleState) == kResultOk,
         "save the admitted forward-compatible state");
  const auto admitted = choc::json::parse(std::string{
      static_cast<const char *>(forwardCompatibleState.getData()),
      forwardCompatibleState.getCursor()});
  expect(admitted["pipelineA"].size() == 128u,
         "the full forward-compatible pipeline becomes the state authority");
  expect(processor->terminate() == kResultOk,
         "terminate the invalid state-pipeline test");
}

// Structured parameter commands are adopted only when their byte shape
// matches the runtime shadow. A Matrix route-count edit changes that shape, so
// treating it as an in-place update would commit "mx" to state while the audio
// thread silently kept the old routes.
void testStructuredParameterShapeChangeRebuildsTheRuntime() {
  auto processor = std::make_unique<EffeTuneProcessor>();
  expect(processor->initialize(nullptr) == kResultOk,
         "initialize the structured parameter-shape test");
  auto processSetup = setup(48000.0, 64);
  expect(processor->setupProcessing(processSetup) == kResultOk,
         "prepare the structured parameter-shape test");
  const auto installed = choc::json::parse(processor->handleUiMessage(
      R"({"type":"pipeline/rebuild","payload":{"pipeline":"A","plugins":[)"
      R"({"id":701,"type":"MatrixPlugin","name":"Matrix","enabled":true,)"
      R"("parameters":{"mx":"0011"},"wasmParams":[],)"
      R"("wasmParamsHash":117968709,"wasmParamBytes":[1,0,2,0,0,0,0,1,1,0]}]}})"));
  expect(installed["ok"].getWithDefault<bool>(false),
         "install the two-route Matrix runtime");
  expect(processor->setActive(true) == kResultOk,
         "activate the structured parameter-shape test");

  gate_ordering::AudioRig rig(0.0f);
  rig.inputLeft.fill(0.25f);
  rig.inputRight.fill(-0.5f);
  expect(processor->process(rig.data) == kResultOk &&
             std::abs(rig.outputLeft[0] - 0.25f) < 1.0e-6f &&
             std::abs(rig.outputRight[0] + 0.5f) < 1.0e-6f,
         "the initial Matrix image routes both channels diagonally");

  const auto updated = choc::json::parse(processor->handleUiMessage(
      R"({"type":"pipeline/updatePlugin","payload":{"pipeline":"A","plugin":)"
      R"({"id":701,"type":"MatrixPlugin","name":"Matrix","enabled":true,)"
      R"("parameters":{"mx":"01"},"wasmParams":[],)"
      R"("wasmParamsHash":117968709,"wasmParamBytes":[1,0,1,0,0,1,0]}}})"));
  expect(updated["ok"].getWithDefault<bool>(false) &&
             updated["rebuildAssets"].getWithDefault<bool>(false),
         "a structured byte-shape change takes the non-RT rebuild path");

  rig.outputLeft.fill(0.0f);
  rig.outputRight.fill(0.0f);
  expect(processor->process(rig.data) == kResultOk &&
             std::abs(rig.outputLeft[0]) < 1.0e-6f &&
             std::abs(rig.outputRight[0] - 0.25f) < 1.0e-6f,
         "the rebuilt Matrix DSP plays the single replacement route");
  expect(savedPluginStringParameter(*processor, 701, "mx") == "01",
         "the saved Matrix state names the same route the DSP plays");

  expect(processor->setActive(false) == kResultOk,
         "deactivate the structured parameter-shape test");
  expect(processor->terminate() == kResultOk,
         "terminate the structured parameter-shape test");
}

// A bulk request owns the state/page generation in which it entered the
// bridge. Both routes decode before their control transaction, so setState()
// and a complete replacement page can otherwise pass them while they wait and
// the old request can overwrite the restored generation after pending is false
// again. History also must not become valid merely because a replacement that
// was pending when it started completes first.
void testStaleBulkRequestsCannotCrossStateReplacement() {
  auto processor = std::make_unique<EffeTuneProcessor>();
  expect(processor->initialize(nullptr) == kResultOk,
         "initialize the stale bulk-request test");
  auto processSetup = setup(48000.0, 64);
  expect(processor->setupProcessing(processSetup) == kResultOk,
         "prepare the stale bulk-request test");
  const auto installed = choc::json::parse(processor->handleUiMessage(
      R"({"type":"pipeline/rebuild","payload":{"pipeline":"A","plugins":[)"
      R"({"id":801,"type":"DCOffsetPlugin","name":"Restored Offset",)"
      R"("enabled":true,"parameters":{"of":-0.5},"wasmParams":[-0.5],)"
      R"("wasmParamsHash":1104945464}]}})"));
  expect(installed["ok"].getWithDefault<bool>(false),
         "install the state that every restore must preserve");
  const effetune::vst::AutomationTargetIdentity restoredIdentity{
      'A', 801, "DCOffsetPlugin", "of", 0};
  const auto restoredParameterId =
      boundAutomationParameterId(*processor, restoredIdentity);
  const auto restoredSlot =
      PluginProcessorTestAccess::activeAutomationSlot(*processor,
                                                       restoredIdentity);
  expect(restoredSlot.has_value(),
         "the restored target owns an automation lane");

  ResizableMemoryIBStream stateA;
  expect(processor->getState(&stateA) == kResultOk,
         "save the authoritative replacement document");
  const auto changed = choc::json::parse(processor->handleUiMessage(
      R"({"type":"pipeline/updatePlugin","payload":{"pipeline":"A","plugin":)"
      R"({"id":801,"type":"DCOffsetPlugin","name":"Restored Offset",)"
      R"("enabled":true,"parameters":{"of":0.5},"wasmParams":[0.5],)"
      R"("wasmParamsHash":1104945464},"automationEdits":[{"pipeline":"A",)"
      R"("pluginId":801,"pluginType":"DCOffsetPlugin","parameterKey":"of",)"
      R"("elementIndex":0,"normalized":0.75}]}})"));
  expect(changed["ok"].getWithDefault<bool>(false) &&
             std::abs(savedPluginParameter(*processor, 801, "of") - 0.5) <
                 1.0e-9,
         "move the old page away from the saved replacement state");

  const auto announceReplacementPage = [&] {
    const auto startup = choc::json::parse(processor->handleUiMessage(
        R"({"type":"host/getInfo","payload":{"startup":true}})"));
    expect(startup["ok"].getWithDefault<bool>(false) &&
               startup["dspReady"].getWithDefault<bool>(false) &&
               startup["stateReplacementPending"].getWithDefault<bool>(false),
           "announce that the ready old DSP still requires a replacement image");
  };
  const auto installReplacementRuntime = [&] {
    const auto replacement = choc::json::parse(processor->handleUiMessage(
        R"({"type":"pipeline/rebuild","payload":{"pipeline":"A","plugins":[)"
        R"({"id":801,"type":"DCOffsetPlugin","name":"Restored Offset",)"
        R"("enabled":true,"parameters":{"of":-0.5},"wasmParams":[-0.5],)"
        R"("wasmParamsHash":1104945464}]}})"));
    expect(replacement["ok"].getWithDefault<bool>(false),
           "accept the authorized replacement runtime");
    const auto completed = hostInfo(*processor);
    expect(!completed["stateReplacementPending"].getWithDefault<bool>(true),
           "publish completion after the authorized replacement succeeds");
  };
  const auto restoreAndRebuild = [&] {
    stateA.rewind();
    expect(processor->setState(&stateA) == kResultOk,
           "publish the saved replacement document");
    announceReplacementPage();
    installReplacementRuntime();
  };
  const auto waitForBulkPause = [&] {
    return pumpMainThreadUntil(
        [&] {
          return PluginProcessorTestAccess::bulkRequestPausedBeforeCommit(
              *processor);
        },
        std::chrono::milliseconds(500));
  };
  struct ReplacementSnapshot {
    std::optional<std::uint32_t> slot;
    double saved = 0.0;
    float runtime = 0.0f;
    double parameter = 0.0;
    double scheduler = 0.0;
  };
  const auto captureReplacement = [&] {
    return ReplacementSnapshot{
        PluginProcessorTestAccess::activeAutomationSlot(*processor,
                                                         restoredIdentity),
        savedPluginParameter(*processor, 801, "of"),
        PluginProcessorTestAccess::runtimePackedParameter(*processor, 801, 0),
        processor->getParamNormalized(restoredParameterId),
        PluginProcessorTestAccess::playedAutomationValue(*processor,
                                                          *restoredSlot)};
  };
  const auto assertReplacementUnchanged =
      [&](const ReplacementSnapshot &before, const std::string &operation) {
    const auto after = captureReplacement();
    expect(before.slot == restoredSlot && after.slot == before.slot &&
               std::abs(before.saved + 0.5) < 1.0e-9 &&
               std::abs(before.runtime + 0.5f) < 1.0e-6f &&
               std::abs(after.saved - before.saved) < 1.0e-9 &&
               std::abs(after.runtime - before.runtime) < 1.0e-6f &&
               std::abs(after.parameter - before.parameter) < 1.0e-9 &&
               std::abs(after.scheduler - before.scheduler) < 1.0e-9,
           operation +
               " leaves the restored document, runtime, binding and scheduler unchanged");
  };

  PluginProcessorTestAccess::pauseNextBulkRequestBeforeCommit(*processor);
  std::string staleRebuildResponse;
  std::thread staleRebuilder([&] {
    staleRebuildResponse = processor->handleUiMessage(
        R"({"type":"pipeline/rebuild","payload":{"pipeline":"A","plugins":[)"
        R"({"id":802,"type":"DCOffsetPlugin","name":"Stale Rebuild",)"
        R"("enabled":true,"parameters":{"of":0.25},"wasmParams":[0.25],)"
        R"("wasmParamsHash":1104945464}]}})");
  });
  const auto rebuildPaused = waitForBulkPause();
  restoreAndRebuild();
  const auto beforeStaleRebuild = captureReplacement();
  PluginProcessorTestAccess::releaseBulkRequestBeforeCommit(*processor);
  staleRebuilder.join();
  const auto staleRebuild = choc::json::parse(staleRebuildResponse);
  expect(rebuildPaused && staleRebuild["ok"].getWithDefault<bool>(false) &&
             staleRebuild["success"].getWithDefault<bool>(false),
         "acknowledge the old rebuild as a successful generation no-op");
  assertReplacementUnchanged(beforeStaleRebuild, "the old rebuild");

  PluginProcessorTestAccess::pauseNextBulkRequestBeforeCommit(*processor);
  std::string staleHistoryResponse;
  std::thread staleHistory([&] {
    staleHistoryResponse = processor->handleUiMessage(
        R"({"type":"pipeline/restoreHistory","payload":{"pipelineA":[)"
        R"({"id":803,"type":"DCOffsetPlugin","name":"Stale History",)"
        R"("enabled":true,"parameters":{"of":0.75},"wasmParams":[0.75],)"
        R"("wasmParamsHash":1104945464}],"pipelineB":null,)"
        R"("pipelineBInitialized":false,"currentPipeline":"A"}})");
  });
  const auto historyPaused = waitForBulkPause();
  restoreAndRebuild();
  const auto beforeStaleHistory = captureReplacement();
  PluginProcessorTestAccess::releaseBulkRequestBeforeCommit(*processor);
  staleHistory.join();
  const auto staleHistoryResult = choc::json::parse(staleHistoryResponse);
  expect(historyPaused &&
             staleHistoryResult["ok"].getWithDefault<bool>(false) &&
             staleHistoryResult["success"].getWithDefault<bool>(false),
         "acknowledge the old history restore as a successful generation no-op");
  assertReplacementUnchanged(beforeStaleHistory, "the old history restore");

  // This request belongs to the authorized new page, but starts while state
  // replacement is still pending. Completing the rebuild before it acquires
  // the transaction must not turn that history operation into a valid commit.
  stateA.rewind();
  expect(processor->setState(&stateA) == kResultOk,
         "publish one more replacement for the pending-history case");
  announceReplacementPage();
  PluginProcessorTestAccess::pauseNextBulkRequestBeforeCommit(*processor);
  std::string pendingHistoryResponse;
  std::thread pendingHistory([&] {
    pendingHistoryResponse = processor->handleUiMessage(
        R"({"type":"pipeline/restoreHistory","payload":{"pipelineA":[)"
        R"({"id":804,"type":"DCOffsetPlugin","name":"Pending History",)"
        R"("enabled":true,"parameters":{"of":1.0},"wasmParams":[1.0],)"
        R"("wasmParamsHash":1104945464}],"pipelineB":null,)"
        R"("pipelineBInitialized":false,"currentPipeline":"A"}})");
  });
  const auto pendingHistoryPaused = waitForBulkPause();
  installReplacementRuntime();
  const auto beforePendingHistory = captureReplacement();
  PluginProcessorTestAccess::releaseBulkRequestBeforeCommit(*processor);
  pendingHistory.join();
  const auto pendingHistoryResult = choc::json::parse(pendingHistoryResponse);
  expect(pendingHistoryPaused &&
             pendingHistoryResult["ok"].getWithDefault<bool>(false) &&
             pendingHistoryResult["success"].getWithDefault<bool>(false),
         "a history request that began pending remains a no-op after rebuild");
  assertReplacementUnchanged(beforePendingHistory,
                             "the pending history restore");

  expect(processor->setActive(true) == kResultOk,
         "activate the final restored generation");
  gate_ordering::AudioRig rig(0.0f);
  expect(processor->process(rig.data) == kResultOk &&
             std::abs(rig.outputLeft[0] + 0.5f) < 1.0e-6f,
         "the final wet engine still plays the restored state");
  expect(processor->setActive(false) == kResultOk,
         "deactivate the stale bulk-request test");
  expect(processor->terminate() == kResultOk,
         "terminate the stale bulk-request test");
}

// setState() publishes the restored document before the reloaded page supplies
// its native replacement. The old page remains alive long enough to flush one
// final animation frame, so its plug-in image and any not-yet-open touch are
// stale even though they were created against the retained old runtime. The
// image must not put B back over restored A or open a touch into the new state.
void testStalePluginUpdateCannotOverwritePendingStateReplacement() {
  auto processor = std::make_unique<EffeTuneProcessor>();
  expect(processor->initialize(nullptr) == kResultOk,
         "initialize the stale replacement-update test");
  auto *handler = new TestComponentHandler();
  expect(processor->setComponentHandler(handler) == kResultOk,
         "install the stale replacement-update handler");
  auto processSetup = setup(48000.0, 64);
  expect(processor->setupProcessing(processSetup) == kResultOk,
         "prepare the stale replacement-update test");

  constexpr std::uint32_t kPluginId = 109;
  constexpr double kStateA = -0.5;
  constexpr double kStateB = 0.5;
  const effetune::vst::AutomationTargetIdentity identity{
      'A', kPluginId, "DCOffsetPlugin", "of", 0};
  const auto installed = choc::json::parse(processor->handleUiMessage(
      R"({"type":"pipeline/rebuild","payload":{"pipeline":"A","plugins":[)"
      R"({"id":109,"type":"DCOffsetPlugin","name":"DC Offset","enabled":true,)"
      R"("parameters":{"of":-0.5},"wasmParams":[-0.5],)"
      R"("wasmParamsHash":1104945464}]}})"));
  expect(installed["ok"].getWithDefault<bool>(false),
         "install state A for the stale replacement-update test");
  const auto parameterId = boundAutomationParameterId(*processor, identity);

  ResizableMemoryIBStream stateA;
  expect(processor->getState(&stateA) == kResultOk,
         "save state A before the old page moves to B");

  const auto changedToB = choc::json::parse(processor->handleUiMessage(
      R"({"type":"pipeline/rebuild","payload":{"pipeline":"A","plugins":[)"
      R"({"id":109,"type":"DCOffsetPlugin","name":"DC Offset","enabled":true,)"
      R"("parameters":{"of":0.5},"wasmParams":[0.5],)"
      R"("wasmParamsHash":1104945464}],"automationEdits":[{"pipeline":"A",)"
      R"("pluginId":109,"pluginType":"DCOffsetPlugin","parameterKey":"of",)"
      R"("elementIndex":0,"normalized":0.75}]}})"));
  expect(changedToB["ok"].getWithDefault<bool>(false) &&
             std::abs(savedPluginParameter(*processor, kPluginId, "of") - kStateB) <
                 1.0e-9 &&
             std::abs(PluginProcessorTestAccess::runtimePackedParameter(
                          *processor, kPluginId, 0) -
                      static_cast<float>(kStateB)) < 1.0e-6f,
         "the old page and old playable generation both reach state B");

  // Stop the old page after its unlocked pending check and state snapshot, but
  // before it enters the runtime transaction. This is the precise interval in
  // which setState() used to publish A and the resumed update could still put B
  // back into the document, mailbox and runtime shadow.
  PluginProcessorTestAccess::pausePluginUpdateBeforeRuntimeTransaction(
      *processor, true);
  std::string staleResponse;
  std::atomic_bool staleUpdateCompleted{false};
  std::thread staleUpdater([&] {
    staleResponse = processor->handleUiMessage(
        R"({"type":"pipeline/updatePlugin","payload":{"pipeline":"A",)"
        R"("plugin":{"id":109,"type":"DCOffsetPlugin","name":"DC Offset",)"
        R"("enabled":true,"parameters":{"of":0.25},"wasmParams":[0.25],)"
        R"("wasmParamsHash":1104945464},"automationEdits":[{"pipeline":"A",)"
        R"("pluginId":109,"pluginType":"DCOffsetPlugin","parameterKey":"of",)"
        R"("elementIndex":0,"normalized":0.75,"beginGesture":true,)"
        R"("endGesture":false}]}})");
    staleUpdateCompleted.store(true, std::memory_order_release);
  });
  const auto pauseDeadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (!PluginProcessorTestAccess::pluginUpdatePausedBeforeRuntimeTransaction(
             *processor) &&
         !staleUpdateCompleted.load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < pauseDeadline) {
    std::this_thread::yield();
  }
  const auto reachedRuntimeBoundary =
      PluginProcessorTestAccess::pluginUpdatePausedBeforeRuntimeTransaction(
          *processor);

  stateA.rewind();
  const auto restored = processor->setState(&stateA);
  handler->clearEditLog();
  PluginProcessorTestAccess::pausePluginUpdateBeforeRuntimeTransaction(
      *processor, false);
  staleUpdater.join();

  const auto stale = choc::json::parse(staleResponse);
  expect(reachedRuntimeBoundary && restored == kResultOk,
         "publish restored state A while the stale B update is paused before "
         "its runtime transaction");
  expect(stale["ok"].getWithDefault<bool>(false) &&
             stale["success"].getWithDefault<bool>(false),
         "accept the dying page's final frame as a successful image no-op");
  expect(std::abs(savedPluginParameter(*processor, kPluginId, "of") - kStateA) <
                 1.0e-9 &&
             std::abs(PluginProcessorTestAccess::runtimePackedParameter(
                          *processor, kPluginId, 0) -
                      static_cast<float>(kStateB)) < 1.0e-6f,
         "the stale image cannot overwrite restored state A or the retained "
         "old playable runtime");
  expect(handler->stepCount(TestComponentHandler::EditStep::begin) == 0u &&
             handler->stepCount(TestComponentHandler::EditStep::end) == 0u &&
             !PluginProcessorTestAccess::hostGestureOpen(*processor, parameterId),
         "the stale frame cannot open a touch after setState closed the old page");

  // A pending stale frame may report only into a touch that is already open; it
  // may not reserve a lane from the restored binding state. That reservation is
  // permanent, even after the target is retired, so one dying-page frame used
  // to consume host automation capacity forever.
  const effetune::vst::AutomationTargetIdentity unboundIdentity{
      'A', 110, "DCOffsetPlugin", "of", 0};
  const auto beginCountBeforeUnbound =
      handler->stepCount(TestComponentHandler::EditStep::begin);
  const auto performedCountBeforeUnbound = handler->performedEditCount;
  const auto staleUnbound = choc::json::parse(processor->handleUiMessage(
      R"({"type":"pipeline/updatePlugin","payload":{"pipeline":"A",)"
      R"("plugin":{"id":110,"type":"DCOffsetPlugin","name":"Stale Unbound",)"
      R"("enabled":true,"parameters":{"of":0.5},"wasmParams":[0.5],)"
      R"("wasmParamsHash":1104945464},"automationEdits":[{"pipeline":"A",)"
      R"("pluginId":110,"pluginType":"DCOffsetPlugin","parameterKey":"of",)"
      R"("elementIndex":0,"normalized":0.75,"beginGesture":true,)"
      R"("endGesture":false,"bindIfUnbound":true}]}})"));
  expect(staleUnbound["ok"].getWithDefault<bool>(false) &&
             !PluginProcessorTestAccess::activeAutomationSlot(
                  *processor, unboundIdentity)
                  .has_value() &&
             handler->stepCount(TestComponentHandler::EditStep::begin) ==
                 beginCountBeforeUnbound &&
             handler->performedEditCount == performedCountBeforeUnbound,
         "a pending stale edit cannot claim a new lane or reach the host");

  const auto startup = choc::json::parse(processor->handleUiMessage(
      R"({"type":"host/getInfo","payload":{"startup":true}})"));
  expect(startup["ok"].getWithDefault<bool>(false) &&
             !PluginProcessorTestAccess::hostGestureOpen(*processor, parameterId) &&
             handler->stepCount(TestComponentHandler::EditStep::end) == 0u,
         "the replacement page finds no touch for the stale frame to leak");

  const auto replacement = choc::json::parse(processor->handleUiMessage(
      R"({"type":"pipeline/rebuild","payload":{"pipeline":"A","plugins":[)"
      R"({"id":109,"type":"DCOffsetPlugin","name":"DC Offset","enabled":true,)"
      R"("parameters":{"of":-0.5},"wasmParams":[-0.5],)"
      R"("wasmParamsHash":1104945464}]}})"));
  expect(replacement["ok"].getWithDefault<bool>(false),
         "install the replacement runtime supplied by the reloaded page");
  expect(std::abs(savedPluginParameter(*processor, kPluginId, "of") - kStateA) <
                 1.0e-9 &&
             std::abs(PluginProcessorTestAccess::runtimePackedParameter(
                          *processor, kPluginId, 0) -
                      static_cast<float>(kStateA)) < 1.0e-6f,
         "the final serialized document and runtime both remain state A");

  // Exercise the ABA window: this old update commits while no replacement is
  // pending, then stops after dropping processingResourcesMutex_ but before it
  // applies its automation edits. A complete setState -> replacement rebuild
  // makes pending true and false again while it sleeps. The monotonic restore
  // epoch, rather than that bool's final value, must still reject its edits.
  handler->clearEditLog();
  expect(PluginProcessorTestAccess::applyAutomationEdit(
             *processor, identity, 0.25,
             {/*bindIfUnbound=*/true, /*beginGesture=*/true,
              /*endGesture=*/false}),
         "open one old-generation touch before the ABA restore");
  expect(PluginProcessorTestAccess::hostGestureOpen(*processor, parameterId),
         "the ABA fixture starts with its old touch open");

  PluginProcessorTestAccess::pausePluginUpdateBeforeAutomationEdits(
      *processor, true);
  std::string crossedEpochResponse;
  std::atomic_bool crossedEpochUpdateCompleted{false};
  std::thread crossedEpochUpdater([&] {
    crossedEpochResponse = processor->handleUiMessage(
        R"({"type":"pipeline/updatePlugin","payload":{"pipeline":"A",)"
        R"("plugin":{"id":109,"type":"DCOffsetPlugin","name":"DC Offset",)"
        R"("enabled":true,"parameters":{"of":0.5},"wasmParams":[0.5],)"
        R"("wasmParamsHash":1104945464},"automationEdits":[{"pipeline":"A",)"
        R"("pluginId":109,"pluginType":"DCOffsetPlugin","parameterKey":"of",)"
        R"("elementIndex":0,"normalized":0.875,"beginGesture":false,)"
        R"("endGesture":true},{"pipeline":"A","pluginId":110,)"
        R"("pluginType":"DCOffsetPlugin","parameterKey":"of","elementIndex":0,)"
        R"("normalized":0.875,"beginGesture":true,"endGesture":false,)"
        R"("bindIfUnbound":true}]}})");
    crossedEpochUpdateCompleted.store(true, std::memory_order_release);
  });
  const auto automationPauseDeadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (!PluginProcessorTestAccess::pluginUpdatePausedBeforeAutomationEdits(
             *processor) &&
         !crossedEpochUpdateCompleted.load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < automationPauseDeadline) {
    std::this_thread::yield();
  }
  const auto reachedAutomationBoundary =
      PluginProcessorTestAccess::pluginUpdatePausedBeforeAutomationEdits(
          *processor);

  stateA.rewind();
  const auto restoredAcrossEpoch = processor->setState(&stateA);
  const auto replacementStartup = choc::json::parse(processor->handleUiMessage(
      R"({"type":"host/getInfo","payload":{"startup":true}})"));
  const auto rebuiltAcrossEpoch = choc::json::parse(processor->handleUiMessage(
      R"({"type":"pipeline/rebuild","payload":{"pipeline":"A","plugins":[)"
      R"({"id":109,"type":"DCOffsetPlugin","name":"DC Offset","enabled":true,)"
      R"("parameters":{"of":-0.5},"wasmParams":[-0.5],)"
      R"("wasmParamsHash":1104945464}]}})"));
  const auto beginsAfterReplacement =
      handler->stepCount(TestComponentHandler::EditStep::begin);
  const auto performsAfterReplacement = handler->performedEditCount;
  const auto endsAfterReplacement =
      handler->stepCount(TestComponentHandler::EditStep::end);
  const auto slotAfterReplacement =
      PluginProcessorTestAccess::activeAutomationSlot(*processor, identity);
  const auto parameterAfterReplacement =
      processor->getParamNormalized(parameterId);
  const auto schedulerAfterReplacement =
      slotAfterReplacement.has_value()
          ? PluginProcessorTestAccess::playedAutomationValue(
                *processor, *slotAfterReplacement)
          : -1.0;
  PluginProcessorTestAccess::pausePluginUpdateBeforeAutomationEdits(
      *processor, false);
  crossedEpochUpdater.join();

  const auto crossedEpoch = choc::json::parse(crossedEpochResponse);
  const auto restoredSlot =
      PluginProcessorTestAccess::activeAutomationSlot(*processor, identity);
  expect(reachedAutomationBoundary && restoredAcrossEpoch == kResultOk &&
             replacementStartup["ok"].getWithDefault<bool>(false) &&
             rebuiltAcrossEpoch["ok"].getWithDefault<bool>(false),
         "complete setState and replacement rebuild while the old update waits "
         "before its automation edits");
  expect(crossedEpoch["ok"].getWithDefault<bool>(false) &&
             crossedEpoch["success"].getWithDefault<bool>(false),
         "the crossed-generation image remains an acknowledged no-op");
  expect(slotAfterReplacement.has_value() && restoredSlot == slotAfterReplacement &&
             *restoredSlot == static_cast<std::uint32_t>(
                                  parameterId - kFirstAutomationParameterId) &&
             !PluginProcessorTestAccess::activeAutomationSlot(
                  *processor, unboundIdentity)
                  .has_value(),
         "the crossed edit neither replaces the restored binding nor consumes "
         "an unbound slot");
  const auto restoredParameter = processor->getParamNormalized(parameterId);
  const auto restoredScheduler = PluginProcessorTestAccess::playedAutomationValue(
      *processor, *restoredSlot);
  expect(std::abs(restoredParameter - parameterAfterReplacement) < 1.0e-9 &&
             std::abs(restoredScheduler - schedulerAfterReplacement) < 1.0e-9,
         "the crossed edit leaves the restored Parameter and scheduler "
         "unchanged after replacement");
  expect(std::abs(savedPluginParameter(*processor, kPluginId, "of") - kStateA) <
                 1.0e-9 &&
             std::abs(PluginProcessorTestAccess::runtimePackedParameter(
                          *processor, kPluginId, 0) -
                      static_cast<float>(kStateA)) < 1.0e-6f,
         "the crossed edit leaves the restored document and runtime unchanged");
  expect(!PluginProcessorTestAccess::hostGestureOpen(*processor, parameterId) &&
             beginsAfterReplacement == 1u && endsAfterReplacement == 1u &&
             handler->stepCount(TestComponentHandler::EditStep::begin) ==
                 beginsAfterReplacement &&
             handler->performedEditCount == performsAfterReplacement &&
             handler->stepCount(TestComponentHandler::EditStep::end) ==
                 endsAfterReplacement,
         "setState closes the old touch once and the crossed edit neither "
         "reopens nor reports into the replacement generation");

  expect(processor->setComponentHandler(nullptr) == kResultOk,
         "remove the stale replacement-update handler");
  handler->release();
  expect(processor->terminate() == kResultOk,
         "terminate the stale replacement-update test");
}

// A host can write a parameter through IEditController::setParamNormalized and
// through nothing else: a generic-editor field, an automation lane scrubbed
// with the transport stopped, a preset applied from the host's own parameter
// list. None of those deliver an inputParameterChanges queue, and process() is
// not running, so the only other route a host value has into this plug-in is
// closed. EditControllerEx1's implementation knows nothing about the binding
// registry, the state document or the DSP, so without an override the value
// would land in the Parameter object and stop there -- getParamNormalized()
// reporting the user's value while getState() saved the old one and the audio
// kept playing it, with no diagnostic anywhere.
//
// The override commits the same registry, parameter-bank and drain authority as
// every other explicit edit. Its scheduler value crosses the bounded handoff
// instead: process() ingests it before the host queues, which lets a queue on
// the first resumed block remain the later authority without a control thread
// taking that block's timeline away.
void testSetParamNormalizedAloneReachesTheSavedState() {
  auto processor = std::make_unique<EffeTuneProcessor>();
  expect(processor->initialize(nullptr) == kResultOk,
         "initialize the setParamNormalized persistence test");
  auto *handler = new TestComponentHandler();
  expect(processor->setComponentHandler(handler) == kResultOk,
         "install the setParamNormalized persistence handler");
  auto processSetup = setup(48000.0, 64);
  expect(processor->setupProcessing(processSetup) == kResultOk,
         "prepare the setParamNormalized persistence test");
  const auto installed = choc::json::parse(processor->handleUiMessage(
      R"({"type":"pipeline/rebuild","payload":{"pipeline":"A","plugins":[)"
      R"({"id":93,"type":"VolumePlugin","name":"Volume","enabled":true,)"
      R"("parameters":{"vl":-12},"wasmParams":[-12],)"
      R"("wasmParamsHash":1719233191}]}})"));
  expect(installed["ok"].getWithDefault<bool>(false),
         "install the setParamNormalized persistence pipeline");
  const effetune::vst::AutomationTargetIdentity identity{'A', 93, "VolumePlugin",
                                                         "vl", 0};
  const auto volumeId = boundAutomationParameterId(*processor, identity);
  const auto slot =
      PluginProcessorTestAccess::activeAutomationSlot(*processor, identity);
  expect(slot.has_value(), "the persistence target owns a lane");
  expect(processor->setActive(true) == kResultOk,
         "activate the setParamNormalized persistence test");

  // The whole point: no process() call anywhere between here and getState().
  constexpr double kWritten = 0.75;
  // -60..24 dB linear, so 0.75 of the lane is 3 dB. This is what the saved
  // document and the runtime image have to carry.
  constexpr double kExpectedVolumeDb = 3.0;
  expect(processor->setParamNormalized(volumeId, kWritten) == kResultTrue,
         "the host writes the parameter through IEditController alone");
  expect(processor->getParamNormalized(volumeId) == kWritten,
         "the host parameter holds the written value");
  // A control-service poll, exactly as before: the fix must not depend on one.
  (void)hostInfo(*processor);

  const auto savedVolume = savedPluginParameter(*processor, 93, "vl");
  expect(std::abs(savedVolume - kExpectedVolumeDb) < 1.0e-3,
         "a bare setParamNormalized survives the save -- saved " +
             std::to_string(savedVolume) + " dB, expected " +
             std::to_string(kExpectedVolumeDb) + " dB");

  // The DSP side. The runtime image is written by the audio thread, and the
  // first block ingests the bounded controller handoff before staging it.
  gate_ordering::AudioRig rig(0.0f);
  expect(processor->process(rig.data) == kResultOk,
         "render a block after the bare setParamNormalized");
  expect(std::abs(PluginProcessorTestAccess::playedAutomationValue(*processor, *slot) -
                  kWritten) < 1.0e-9,
         "a bare setParamNormalized reaches the value the DSP plays");
  expect(std::abs(PluginProcessorTestAccess::runtimePackedParameter(*processor, 93, 0) -
                  static_cast<float>(kExpectedVolumeDb)) < 1.0e-3f,
         "and the runtime image the engine reads");

  // A generic-editor write followed by an ordinary editor gesture on the same
  // control. The later value has to win outright: an adoption that left
  // anything of the earlier one behind -- an unclaimed publish the drain could
  // replay, a scheduler still holding the first value -- would show up here as
  // the state document and the DSP disagreeing with the host parameter.
  constexpr double kGesture = 0.25;
  constexpr double kGestureVolumeDb = -39.0;
  expect(PluginProcessorTestAccess::applyAutomationEdit(*processor, identity,
                                                        kGesture),
         "the editor moves the same control after the host wrote it");
  expect(std::abs(processor->getParamNormalized(volumeId) - kGesture) < 1.0e-9,
         "the host parameter follows the later gesture");
  // A gesture leaves the document to the block and the drain -- the editor that
  // made it is the authority until then -- so the block comes first here.
  expect(processor->process(rig.data) == kResultOk,
         "render a block after the later gesture");
  expect(std::abs(PluginProcessorTestAccess::playedAutomationValue(*processor, *slot) -
                  kGesture) < 1.0e-9,
         "the DSP is left on the later value with nothing of the earlier one "
         "replayed over it");
  const auto afterGesture = savedPluginParameter(*processor, 93, "vl");
  expect(std::abs(afterGesture - kGestureVolumeDb) < 1.0e-3,
         "and the saved document carries the later value, not a blend of the "
         "two -- saved " +
             std::to_string(afterGesture) + " dB");
  expect(std::abs(PluginProcessorTestAccess::runtimePackedParameter(*processor, 93, 0) -
                  static_cast<float>(kGestureVolumeDb)) < 1.0e-3f,
         "and so does the runtime image");

  // During playback the sample-offset queue is the DSP authority. Hosts often
  // reflect that same lane through setParamNormalized after process() returns;
  // a delayed controller restatement must update the Parameter the editor
  // draws without forcing the scheduler and state back behind the queue.
  ParameterChanges laneChanges(1);
  int32 laneQueueIndex = 0;
  auto *laneQueue = laneChanges.addParameterData(volumeId, laneQueueIndex);
  int32 lanePointIndex = 0;
  constexpr double kQueued = 0.6;
  expect(laneQueue != nullptr &&
             laneQueue->addPoint(0, kQueued, lanePointIndex) == kResultTrue,
         "publish the playback-authority queue");
  rig.data.inputParameterChanges = &laneChanges;
  expect(processor->process(rig.data) == kResultOk,
         "consume the playback-authority queue");
  rig.data.inputParameterChanges = nullptr;
  PluginProcessorTestAccess::drainAutomationValues(*processor);
  expect(processor->setParamNormalized(volumeId, 0.2) == kResultTrue,
         "deliver the delayed controller restatement after the queue");
  expect(std::abs(PluginProcessorTestAccess::playedAutomationValue(*processor, *slot) -
                  kQueued) < 1.0e-9,
         "the delayed restatement does not rewind the running scheduler");
  expect(std::abs(savedPluginParameter(*processor, 93, "vl") - (-9.6)) < 1.0e-3,
         "and it does not rewind the state published by the queue");

  // Proximity to the queue is deliberately irrelevant. The same controller
  // value can arrive several blocks late, and as long as audio is still flowing
  // it remains display-only even if all intervening blocks carried no queue.
  for (int block = 0; block < 8; ++block) {
    expect(processor->process(rig.data) == kResultOk,
           "render queue-free audio before the delayed restatement");
  }
  expect(processor->setParamNormalized(volumeId, 0.2) == kResultTrue,
         "deliver the controller restatement several blocks late");
  expect(std::abs(PluginProcessorTestAccess::playedAutomationValue(*processor, *slot) -
                  kQueued) < 1.0e-9,
         "a multi-block-late restatement remains display-only");
  expect(processor->setActive(false) == kResultOk,
         "deactivate after the playback restatement");
  expect(std::abs(savedPluginParameter(*processor, 93, "vl") - (-9.6)) < 1.0e-3,
         "deactivation cannot resurrect a restatement that arrived during playback");

  // Once the component is inactive the same interface is a genuine bounded
  // fallback: no audio queue can carry the host's generic-editor write, so it
  // becomes the save authority and the starting value of the next activation.
  constexpr double kStoppedFallback = 0.8;
  expect(processor->setParamNormalized(volumeId, kStoppedFallback) == kResultTrue,
         "write the bound slot while the component is inactive");
  expect(std::abs(savedPluginParameter(*processor, 93, "vl") - 7.2) < 1.0e-3,
         "the stopped save carries the controller-only fallback");
  expect(processor->setActive(true) == kResultOk,
         "reactivate after committing the stopped fallback");
  expect(processor->process(rig.data) == kResultOk,
         "render the first block after the stopped fallback");
  expect(std::abs(PluginProcessorTestAccess::playedAutomationValue(*processor, *slot) -
                  kStoppedFallback) < 1.0e-9,
         "the next playback starts from the stopped controller write");

  // An idle observation does not itself reserve the stopped boundary. First
  // leave an old fallback pending by resuming a callback while the committer is
  // waiting for the control resource. A newer native UI edit on the same lane
  // is a different authority generation and must invalidate that old write;
  // deactivation must not resurrect it afterwards.
  constexpr double kStaleFallback = 0.3;
  const auto renderedBlocks =
      PluginProcessorTestAccess::renderedBlockCount(*processor);
  PluginProcessorTestAccess::setAudioIdleObservation(
      *processor, 64, 48000.0, renderedBlocks,
      std::chrono::steady_clock::now() - std::chrono::seconds(1));
  PluginProcessorTestAccess::stagePendingControllerWrite(
      *processor, *slot, identity, kStaleFallback);
  auto heldResources =
      PluginProcessorTestAccess::lockProcessingResources(*processor);
  std::atomic_bool committerStarted{false};
  std::atomic_bool committerCompleted{false};
  std::thread committer([&] {
    committerStarted.store(true, std::memory_order_release);
    PluginProcessorTestAccess::commitPendingControllerWritesIfAudioIdle(
        *processor);
    committerCompleted.store(true, std::memory_order_release);
  });
  while (!committerStarted.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  const auto waitedForResources =
      !committerCompleted.load(std::memory_order_acquire);
  PluginProcessorTestAccess::beginSyntheticBlock(*processor);
  heldResources.unlock();
  const auto refusalDeadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(1000);
  while (!committerCompleted.load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < refusalDeadline) {
    std::this_thread::yield();
  }
  const auto refusedWhileBlockActive =
      committerCompleted.load(std::memory_order_acquire);
  PluginProcessorTestAccess::endSyntheticBlock(*processor);
  committer.join();
  expect(waitedForResources,
         "the stopped committer reaches the held control-resource boundary");
  expect(refusedWhileBlockActive,
         "the under-lock idle check refuses the write before the block leaves");
  expect(PluginProcessorTestAccess::controllerWritePending(*processor),
         "a block that resumes before the control lock cancels the idle commit");
  expect(std::abs(PluginProcessorTestAccess::playedAutomationValue(*processor, *slot) -
                  kStoppedFallback) < 1.0e-9,
         "the raced commit does not reconfigure the running scheduler");

  constexpr double kNewerUiEdit = 0.45;
  expect(PluginProcessorTestAccess::applyAutomationEdit(
             *processor, identity, kNewerUiEdit),
         "a newer native UI edit adopts the same lane without a host echo");
  expect(!PluginProcessorTestAccess::controllerWritePending(*processor),
         "the newer authority invalidates the old pending controller fallback");
  expect(processor->process(rig.data) == kResultOk,
         "render the newer native UI edit");
  PluginProcessorTestAccess::drainAutomationValues(*processor);
  expect(processor->setActive(false) == kResultOk,
         "deactivate after replacing the refused fallback");
  expect(std::abs(savedPluginParameter(*processor, 93, "vl") - (-22.2)) < 1.0e-3,
         "deactivation cannot restore the invalidated controller value");
  expect(processor->setActive(true) == kResultOk,
         "reactivate for the fallback handoff race");

  // Stop the committer after its idle decision, then resume real processing
  // with a host queue. The commit may publish concurrently, but it must not
  // claim the audio timeline: the block stays wet, ingests its queue durably,
  // and that same-block queue is the later authority.
  constexpr double kRacedFallback = 0.35;
  constexpr double kResumedQueue = 0.65;
  const auto resumedRenderedBlocks =
      PluginProcessorTestAccess::renderedBlockCount(*processor);
  PluginProcessorTestAccess::setAudioIdleObservation(
      *processor, 64, 48000.0, resumedRenderedBlocks,
      std::chrono::steady_clock::now() - std::chrono::seconds(1));
  PluginProcessorTestAccess::stagePendingControllerWrite(
      *processor, *slot, identity, kRacedFallback);
  PluginProcessorTestAccess::pauseControllerCommitBeforePublish(*processor, true);
  std::atomic_bool handoffCommitterCompleted{false};
  std::thread handoffCommitter([&] {
    PluginProcessorTestAccess::commitPendingControllerWritesIfAudioIdle(
        *processor);
    handoffCommitterCompleted.store(true, std::memory_order_release);
  });
  const auto handoffDeadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (!PluginProcessorTestAccess::controllerCommitPaused(*processor) &&
         !handoffCommitterCompleted.load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < handoffDeadline) {
    std::this_thread::yield();
  }
  const auto reachedPublishBarrier =
      PluginProcessorTestAccess::controllerCommitPaused(*processor);
  const auto timelineUnclaimed =
      !PluginProcessorTestAccess::controlOwnsAudioTimeline(*processor);

  ParameterChanges resumedChanges(1);
  int32 resumedQueueIndex = 0;
  auto *resumedQueue =
      resumedChanges.addParameterData(volumeId, resumedQueueIndex);
  int32 resumedPointIndex = 0;
  expect(resumedQueue != nullptr &&
             resumedQueue->addPoint(0, kResumedQueue, resumedPointIndex) ==
                 kResultTrue,
         "publish the queue on the first resumed block");
  gate_ordering::AudioRig resumedRig(1.0f);
  resumedRig.data.inputParameterChanges = &resumedChanges;
  const auto resumedResult = processor->process(resumedRig.data);
  const auto resumedPlayed =
      PluginProcessorTestAccess::playedAutomationValue(*processor, *slot);
  const auto resumedOutput = resumedRig.outputLeft[0];
  const auto expectedResumedOutput = static_cast<float>(
      std::pow(10.0, (-60.0 + kResumedQueue * 84.0) / 20.0));

  PluginProcessorTestAccess::pauseControllerCommitBeforePublish(*processor, false);
  handoffCommitter.join();
  expect(reachedPublishBarrier && timelineUnclaimed,
         "the controller commit waits without claiming the audio timeline");
  expect(resumedResult == kResultOk,
         "the first resumed block completes while the fallback is publishing");
  expect(std::abs(resumedOutput - expectedResumedOutput) < 1.0e-5f,
         "the first resumed block stays wet instead of returning dry");
  expect(std::abs(resumedPlayed - kResumedQueue) < 1.0e-9,
         "the first resumed block durably ingests the host queue");
  expect(!PluginProcessorTestAccess::controllerWritePending(*processor),
         "the same-block host queue supersedes the fallback generation");
  PluginProcessorTestAccess::drainAutomationValues(*processor);
  expect(std::abs(savedPluginParameter(*processor, 93, "vl") - (-5.4)) < 1.0e-3,
         "the resumed queue remains the saved authority");

  // The same write arriving while the user's hand is still on the control.
  //
  // Replaces the sub-case that asserted this was adopted like any other write.
  // With Read armed a host calls setParamNormalized continuously to keep the
  // plug-in's editor following the lane it is playing, and it does not stop
  // while the user drags over that lane: adopting it there hands the slot back
  // to the value the drag is replacing, on the UI thread, behind both the open
  // touch the block honours and the claim the release latches. What the user
  // sees is the control springing back under the pointer and the sound going
  // with it. The write reaches the Parameter for the editor to draw and stops
  // there, which is all the SDK asks of it: "The controller must never pass
  // this value-change back to the host via the IComponentHandler. It should
  // update the according GUI element(s) only!" (ivsteditcontroller.h).
  constexpr double kHeldValue = 0.4;
  // -60 + 0.4 * 84.
  constexpr double kHeldVolumeDb = -26.4;
  expect(PluginProcessorTestAccess::applyAutomationEdit(
             *processor, identity, kHeldValue,
             {/*bindIfUnbound=*/true, /*beginGesture=*/true, /*endGesture=*/false}),
         "the editor grabs the control and holds it");
  expect(PluginProcessorTestAccess::hostGestureOpen(*processor, volumeId),
         "the touch is open");
  expect(processor->process(rig.data) == kResultOk,
         "render a block of the held drag");
  PluginProcessorTestAccess::drainAutomationValues(*processor);
  expect(std::abs(savedPluginParameter(*processor, 93, "vl") - kHeldVolumeDb) < 1.0e-3,
         "the held value is what the document carries before the host writes");

  constexpr double kRestatedLane = 0.6;
  expect(processor->setParamNormalized(volumeId, kRestatedLane) == kResultTrue,
         "the host writes the parameter while the touch is open");
  expect(PluginProcessorTestAccess::hostGestureOpen(*processor, volumeId),
         "the write leaves the touch exactly as it found it");
  expect(std::abs(processor->getParamNormalized(volumeId) - kHeldValue) < 1.0e-9,
         "and the host parameter still reads the value the hand is holding");
  const auto underTouch = savedPluginParameter(*processor, 93, "vl");
  expect(std::abs(underTouch - kHeldVolumeDb) < 1.0e-3,
         "which is what the document keeps as well, rather than the lane the "
         "drag is being recorded over -- saved " +
             std::to_string(underTouch) + " dB");
  expect(std::abs(PluginProcessorTestAccess::playedAutomationValue(*processor, *slot) -
                  kHeldValue) < 1.0e-9,
         "and what the DSP is left playing under the hand");
  expect(PluginProcessorTestAccess::applyAutomationEdit(
             *processor, identity, kHeldValue,
             {/*bindIfUnbound=*/true, /*beginGesture=*/false, /*endGesture=*/true}),
         "the editor releases the control");
  expect(!PluginProcessorTestAccess::hostGestureOpen(*processor, volumeId),
         "and the touch closes normally afterwards");

  expect(processor->terminate() == kResultOk,
         "terminate the setParamNormalized persistence test");
}

// A stopped host can update a bound node-enable parameter through
// IEditController alone. The registry must not adopt that value before the
// active topology has been told about it, or the later drain sees no toggle and
// leaves the old engine descriptor playing.
void testStoppedControllerNodeEnableWritesReachTheActiveTopology() {
  auto processor = std::make_unique<EffeTuneProcessor>();
  expect(processor->initialize(nullptr) == kResultOk,
         "initialize the stopped controller node-enable test");
  auto processSetup = setup(48000.0, 64);
  expect(processor->setupProcessing(processSetup) == kResultOk,
         "prepare the stopped controller node-enable test");
  const auto installedB = choc::json::parse(processor->handleUiMessage(
      R"({"type":"pipeline/rebuild","payload":{"pipeline":"B","plugins":[)"
      R"({"id":95,"type":"DCOffsetPlugin","name":"Idle DC Offset","enabled":true,)"
      R"("parameters":{"of":0.5},"wasmParams":[0.5],)"
      R"("wasmParamsHash":1104945464}]}})"));
  const auto installedA = choc::json::parse(processor->handleUiMessage(
      R"({"type":"pipeline/rebuild","payload":{"pipeline":"A","plugins":[)"
      R"({"id":95,"type":"DCOffsetPlugin","name":"DC Offset","enabled":true,)"
      R"("parameters":{"of":0.5},"wasmParams":[0.5],)"
      R"("wasmParamsHash":1104945464}]}})"));
  expect(installedB["ok"].getWithDefault<bool>(false) &&
             installedA["ok"].getWithDefault<bool>(false),
         "install active and inactive node-enable targets");
  const auto activeEnableId = boundAutomationParameterId(
      *processor, {'A', 95, "DCOffsetPlugin", "__enabled", 0});
  const auto idleEnableId = boundAutomationParameterId(
      *processor, {'B', 95, "DCOffsetPlugin", "__enabled", 0});
  expect(processor->setActive(true) == kResultOk,
         "activate the stopped controller node-enable test");

  gate_ordering::AudioRig rig(0.0f);
  const auto render = [&](const float expected, const std::string &message) {
    expect(processor->process(rig.data) == kResultOk, "render " + message);
    for (const auto sample : rig.outputLeft) {
      expect(std::abs(sample - expected) < 1.0e-6f, message);
    }
  };
  render(0.5f, "the initial active node offsets silence");

  expect(processor->setActive(false) == kResultOk,
         "deactivate before the controller-only enable writes");
  const auto beforeIdleWrite =
      PluginProcessorTestAccess::descriptorGeneration(*processor);
  expect(processor->setParamNormalized(idleEnableId, 0.0) == kResultTrue,
         "disable the node on the inactive pipeline through the controller");
  expect(PluginProcessorTestAccess::descriptorGeneration(*processor) ==
             beforeIdleWrite,
         "an inactive-pipeline enable write does not queue the active descriptor");

  const auto disableGeneration =
      PluginProcessorTestAccess::descriptorGeneration(*processor);
  expect(processor->setParamNormalized(activeEnableId, 0.0) == kResultTrue,
         "disable the active node through the stopped controller");
  const auto disabledDescriptor =
      PluginProcessorTestAccess::descriptorGeneration(*processor);
  expect(disabledDescriptor == disableGeneration + 1u,
         "the stopped disable queues exactly one active descriptor");
  expect(processor->setActive(true) == kResultOk,
         "reactivate after the stopped disable");
  PluginProcessorTestAccess::serviceLatencyUpdates(*processor);
  expect(PluginProcessorTestAccess::servicedDescriptorGeneration(*processor) ==
             disabledDescriptor,
         "service the stopped disable descriptor");
  render(0.0f, "the stopped controller disable reaches the active engine");
  expect(PluginProcessorTestAccess::descriptorGeneration(*processor) ==
             disabledDescriptor,
         "draining the adopted disable does not queue a second descriptor");

  expect(processor->setActive(false) == kResultOk,
         "deactivate before the stopped controller re-enable");
  expect(processor->setParamNormalized(activeEnableId, 1.0) == kResultTrue,
         "re-enable the active node through the stopped controller");
  const auto enabledDescriptor =
      PluginProcessorTestAccess::descriptorGeneration(*processor);
  expect(enabledDescriptor == disabledDescriptor + 1u,
         "the stopped re-enable queues exactly one active descriptor");
  expect(processor->setActive(true) == kResultOk,
         "reactivate after the stopped re-enable");
  PluginProcessorTestAccess::serviceLatencyUpdates(*processor);
  expect(PluginProcessorTestAccess::servicedDescriptorGeneration(*processor) ==
             enabledDescriptor,
         "service the stopped re-enable descriptor");
  render(0.5f, "the stopped controller re-enable reaches the active engine");
  expect(PluginProcessorTestAccess::descriptorGeneration(*processor) ==
             enabledDescriptor,
         "draining the adopted re-enable does not queue a second descriptor");

  expect(processor->setActive(false) == kResultOk,
         "deactivate the stopped controller node-enable test");
  expect(processor->terminate() == kResultOk,
         "terminate the stopped controller node-enable test");
}

// Master bypass uses the same stopped controller-write fallback as a bound
// slot, while an unbound slot remains only one of the placeholders the bank
// publishes to keep its parameter count stable. Claiming a lane for a
// placeholder would spend one of the finite slots on a target the user never
// named.
void testSetParamNormalizedLeavesBypassAndUnboundSlotsAlone() {
  auto processor = std::make_unique<EffeTuneProcessor>();
  expect(processor->initialize(nullptr) == kResultOk,
         "initialize the untouched-parameter test");
  auto *handler = new TestComponentHandler();
  expect(processor->setComponentHandler(handler) == kResultOk,
         "install the untouched-parameter handler");
  auto processSetup = setup(48000.0, 64);
  expect(processor->setupProcessing(processSetup) == kResultOk,
         "prepare the untouched-parameter test");
  const auto installed = choc::json::parse(processor->handleUiMessage(
      R"({"type":"pipeline/rebuild","payload":{"pipeline":"A","plugins":[)"
      R"({"id":94,"type":"DCOffsetPlugin","name":"DC Offset","enabled":true,)"
      R"("parameters":{"of":0.5},"wasmParams":[0.5],)"
      R"("wasmParamsHash":1104945464}]}})"));
  expect(installed["ok"].getWithDefault<bool>(false),
         "install the untouched-parameter pipeline");
  expect(processor->setActive(true) == kResultOk,
         "activate the untouched-parameter test");

  const auto render = [&](const float expected, const std::string &message) {
    gate_ordering::AudioRig rig(0.0f);
    expect(processor->process(rig.data) == kResultOk,
           "process the untouched-parameter block");
    for (const auto sample : rig.outputLeft) {
      expect(std::abs(sample - expected) < 1.0e-6f, message);
    }
  };
  render(0.5f, "the engaged pipeline offsets the silent input");

  // Master bypass follows the same rule as a bound effect slot. A playback
  // restatement remains display-only and is not converted into a fallback by a
  // later deactivation; a write made after deactivation is the stopped value.
  expect(processor->setParamNormalized(kBypassParameterId, 1.0) == kResultTrue,
         "the host writes master bypass through IEditController alone");
  expect(processor->getParamNormalized(kBypassParameterId) == 1.0,
         "the bypass parameter holds the written value");
  render(0.5f, "the playback restatement does not override running audio");
  expect(processor->setActive(false) == kResultOk,
         "deactivate after the playback bypass restatement");
  ResizableMemoryIBStream bypassSave;
  expect(processor->getState(&bypassSave) == kResultOk,
         "save after the playback bypass restatement");
  effetune::vst::PluginStateDocument bypassState;
  std::string bypassDecodeError;
  expect(effetune::vst::StateCodec::decode(
             std::string(static_cast<const char *>(bypassSave.getData()),
                         bypassSave.getCursor()),
             bypassState, &bypassDecodeError),
         "decode the stopped bypass save: " + bypassDecodeError);
  expect(!bypassState.masterBypass,
         "deactivation does not resurrect the playback restatement");
  expect(processor->setParamNormalized(kBypassParameterId, 1.0) == kResultTrue,
         "write master bypass while the component is inactive");
  ResizableMemoryIBStream stoppedBypassSave;
  expect(processor->getState(&stoppedBypassSave) == kResultOk,
         "save the stopped bypass fallback");
  effetune::vst::PluginStateDocument stoppedBypassState;
  expect(effetune::vst::StateCodec::decode(
             std::string(static_cast<const char *>(stoppedBypassSave.getData()),
                         stoppedBypassSave.getCursor()),
             stoppedBypassState, &bypassDecodeError),
         "decode the stopped bypass fallback: " + bypassDecodeError);
  expect(stoppedBypassState.masterBypass,
         "the inactive controller write is the persisted master bypass");
  expect(processor->setActive(true) == kResultOk,
         "reactivate after the stopped bypass fallback");
  render(0.0f, "and the next playback starts master-bypassed");
  expect(processor->setActive(false) == kResultOk,
         "deactivate the bypassed pipeline");
  expect(processor->setParamNormalized(kBypassParameterId, 0.0) == kResultTrue,
         "write the stopped fallback back to engaged");
  expect(processor->setActive(true) == kResultOk,
         "reactivate the engaged pipeline");
  render(0.5f, "the common fallback restores wet playback as well");

  // A slot that holds no binding. The last one in the bank is never claimed:
  // binding starts from the first free slot, and the fixture builds claim only
  // a handful.
  constexpr auto kUnboundId = kLastAutomationParameterId;
  const auto unboundSlot =
      static_cast<std::uint32_t>(kUnboundId - kFirstAutomationParameterId);
  const auto playedBefore =
      PluginProcessorTestAccess::playedAutomationValue(*processor, unboundSlot);
  const auto savedBefore = savedPluginParameter(*processor, 94, "of");
  handler->clearEditLog();
  expect(processor->setParamNormalized(kUnboundId, 0.6) == kResultTrue,
         "the host writes an unbound placeholder slot");
  expect(processor->getParamNormalized(kUnboundId) == 0.6,
         "the placeholder Parameter takes the value like any other");
  expect(handler->stepCount(TestComponentHandler::EditStep::restart) == 0 &&
             handler->stepCount(TestComponentHandler::EditStep::begin) == 0 &&
             handler->performedEditCount == 0,
         "no lane is claimed for it, so the parameter bank is never "
         "republished and the host is told nothing");
  expect(PluginProcessorTestAccess::playedAutomationValue(*processor, unboundSlot) ==
             playedBefore,
         "and the scheduler is left alone -- an unbound slot drives no DSP");
  expect(std::abs(savedPluginParameter(*processor, 94, "of") - savedBefore) < 1.0e-9,
         "the state document is untouched as well");
  render(0.5f, "and the DSP is still rendering the same signal");

  expect(processor->setActive(false) == kResultOk,
         "deactivate the untouched-parameter test");
  expect(processor->terminate() == kResultOk,
         "terminate the untouched-parameter test");
}

// The automation trace is a diagnostic, and a diagnostic that costs a host
// something when nobody asked for it is a defect of its own. Both halves are
// checked here, because either alone would pass for the wrong reason: an
// always-off trace records nothing at all, and an always-on one is paid for by
// every session that never wanted it.
//
//  - Unset EFFETUNE_AUTOMATION_TRACE: the mechanism is absent. No file is
//    opened, no instance ids are handed out, and a whole drag -- touches,
//    blocks, queues, the close -- leaves nothing behind.
//  - EFFETUNE_AUTOMATION_TRACE naming a path: the same drag produces the
//    records of that drag in the order the drag made them.
//  - EFFETUNE_AUTOMATION_TRACE_PARAM naming a different parameter: the
//    per-parameter records go away and the block records stay, which is what
//    makes a log from a busy project readable without losing its timeline.
void testAutomationTraceFollowsItsEnvironmentVariable() {
  namespace trace = effetune::vst::plugin::trace;

  const auto tracePath =
      std::filesystem::temp_directory_path() /
      ("effetune-automation-trace-" +
       std::to_string(static_cast<unsigned long long>(
           std::chrono::steady_clock::now().time_since_epoch().count())) +
       ".log");
  std::error_code fileError;
  std::filesystem::remove(tracePath, fileError);

  const auto setVariable = [](const char *const name, const std::string &value) {
    // An empty value removes the variable, which is what "nobody asked for it"
    // has to mean for the first half of this test.
    expect(_putenv_s(name, value.c_str()) == 0,
           std::string{"set "} + name + " for the trace test");
  };

  const auto readTrace = [&]() {
    std::ifstream stream(tracePath, std::ios::binary);
    expect(stream.good(), "the traced drag left a file to read");
    return std::string{std::istreambuf_iterator<char>(stream),
                       std::istreambuf_iterator<char>()};
  };

  // One drag, start to finish, against a host that plays its recorded lane back
  // into the very slot being held. Run identically with the trace off and on, so
  // the two differ in nothing but the environment.
  const auto runOneDrag = [](const std::string &what) {
    auto fixture = openTouchFixture(what);
    auto &processor = *fixture.processor;
    expect(processor.setActive(true) == kResultOk, "activate " + what);
    gate_ordering::AudioRig rig(0.0f);
    ProcessContext context{};
    context.state = ProcessContext::kPlaying | ProcessContext::kContTimeValid;
    rig.data.processContext = &context;
    const auto playBlock = [&](const double hostNormalized) {
      ParameterChanges changes(1);
      int32 queueIndex = 0;
      auto *queue = changes.addParameterData(fixture.parameterId, queueIndex);
      int32 pointIndex = 0;
      expect(queue != nullptr &&
                 queue->addPoint(0, hostNormalized, pointIndex) == kResultTrue,
             "emit the host automation point for " + what);
      rig.data.inputParameterChanges = &changes;
      expect(processor.process(rig.data) == kResultOk, "render " + what);
      rig.data.inputParameterChanges = nullptr;
      context.projectTimeSamples += rig.data.numSamples;
      context.continousTimeSamples += rig.data.numSamples;
    };
    playBlock(0.5);
    expect(PluginProcessorTestAccess::applyAutomationEdit(
               processor, fixture.identity, 0.75,
               {/*bindIfUnbound=*/true, /*beginGesture=*/false,
                /*endGesture=*/false}),
           "drag inside the open touch of " + what);
    playBlock(0.5);
    expect(PluginProcessorTestAccess::applyAutomationEdit(
               processor, fixture.identity, 0.75,
               {/*bindIfUnbound=*/true, /*beginGesture=*/false,
                /*endGesture=*/true}),
           "release the drag of " + what);
    playBlock(0.5);
    expect(processor.setActive(false) == kResultOk, "deactivate " + what);
    expect(processor.terminate() == kResultOk, "terminate " + what);
    fixture.handler->release();
  };

  setVariable("EFFETUNE_AUTOMATION_TRACE", "");
  setVariable("EFFETUNE_AUTOMATION_TRACE_PARAM", "");
  trace::reinstallFromEnvironmentForTesting();
  expect(!trace::enabled(),
         "an unset EFFETUNE_AUTOMATION_TRACE leaves the trace mechanism absent");
  expect(trace::nextInstanceId() == 0u,
         "and hands out no instance id, because there is no state to record into");
  runOneDrag("the untraced drag");
  trace::flush();
  expect(!std::filesystem::exists(tracePath),
         "a drag with the variable unset opens no file at all");

  setVariable("EFFETUNE_AUTOMATION_TRACE", tracePath.string());
  trace::reinstallFromEnvironmentForTesting();
  expect(trace::enabled(),
         "a path in EFFETUNE_AUTOMATION_TRACE turns the trace on");
  runOneDrag("the traced drag");
  // Reuse the real topology and latency fixtures, whose audio/state assertions
  // also run with tracing off earlier in this suite. Recording may not alter
  // either path, and synthetic emitter calls would not verify the call sites.
  testNodeEnableAutomationDrivesTopologyNotPackedParameters();
  testDeferredLatencyPlanRefreshAlignsParallelAndBypassPaths();
  trace::flush();
  setVariable("EFFETUNE_AUTOMATION_TRACE", "");
  trace::reinstallFromEnvironmentForTesting();
  expect(!trace::enabled(),
         "and clearing the variable puts it back to absent for everything after");

  const auto log = readTrace();
  std::filesystem::remove(tracePath, fileError);
  expect(log.rfind("# EffeTune Mixwright automation trace", 0) == 0,
         "the log opens by naming the build the records came from");
  expect(log.find("# trace-schema=3 latency-and-descriptor-transitions") !=
             std::string::npos,
         "the trace identifies the latency/descriptor schema");
  std::size_t position = 0;
  const auto expectNext = [&](const std::string &token) {
    const auto found = log.find(token, position);
    expect(found != std::string::npos,
           "the traced drag records " + token + " after everything before it");
    position = found + token.size();
  };
  // The shape of one drag, in the order it happened: the touch opens with the
  // value that opened it, blocks carry the host's queue past it, and the
  // release closes the touch.
  expectNext("touchOpened");
  expectNext("beginEdit");
  expectNext("performEdit");
  expectNext("block");
  expectNext("queue");
  expectNext("dspValue");
  expectNext("endEdit");
  expectNext("touchClosed");

  for (const auto *event : {"latencyPrepared", "latencySynced", "getLatencySamples",
                            "descriptorQueued", "descriptorNode", "descriptorApplied",
                            "setActive"}) {
    expect(log.find(event) != std::string::npos,
           std::string{"the real processor path records "} + event);
  }
  expect(log.find("pluginId=77 enabled=0") != std::string::npos &&
             log.find("pluginId=77 enabled=1") != std::string::npos,
         "the queued topology trace distinguishes OFF from ON for the same node");
  expect(log.find("previous=144 reported=480 pipeline=480 resampler=0 factor=1") !=
             std::string::npos,
         "the latency trace preserves the old/new report and its engine components");
  expect(log.find("# dropped") == std::string::npos,
         "the diagnostic fixtures retain a complete transition sequence");

  setVariable("EFFETUNE_AUTOMATION_TRACE", tracePath.string());
  setVariable("EFFETUNE_AUTOMATION_TRACE_PARAM",
              std::to_string(static_cast<unsigned>(kFirstAutomationParameterId) + 7u));
  trace::reinstallFromEnvironmentForTesting();
  runOneDrag("the filtered drag");
  trace::flush();
  setVariable("EFFETUNE_AUTOMATION_TRACE", "");
  setVariable("EFFETUNE_AUTOMATION_TRACE_PARAM", "");
  trace::reinstallFromEnvironmentForTesting();
  expect(!trace::enabled(),
         "the filtered pass leaves the trace off behind it as well");

  const auto filtered = readTrace();
  std::filesystem::remove(tracePath, fileError);
  expect(filtered.find("block") != std::string::npos,
         "a parameter filter keeps every block record, so the timeline survives");
  expect(filtered.find("queue") == std::string::npos &&
             filtered.find("dspValue") == std::string::npos,
         "and drops the per-parameter records of every other parameter");
  expect(filtered.find("touchOpened") != std::string::npos,
         "while the gesture records, which are about the plug-in rather than one "
         "block, are never filtered out");
}

} // namespace

int main() {
  effetune::vst::testing::suppressCrtModalDialogs();
  try {
#if defined(EFFETUNE_AUTOMATION_HOST_GATE_FIXTURE)
    testHostGateFixturePublishesThreeParameters();
#endif
    testProcessingSurvivesHostReconfiguration();
    testContextualExecutionAdmissionSurvivesEngineContextChanges();
    testConcurrentHostContextChangeWinsPluginUpdateAdmission();
    testOversamplingFailureRestoresPreviousPlayableGeneration();
    testStoppedTransportFallbackAndDiscontinuities();
    testAutomationCatalogProjectionIsOneControlTransaction();
    testClosedEditorHostAutomationUpdatesStateAndAudio();
    testBoundTargetGestureReachesAudioWithoutHostEcho();
    testNamedBulkAutomationEditsReachAudioAndUnnamedOnesStayOverlaid();
    testProcessorPublishesAutomationStateInterface();
    testNodeEnableAutomationDrivesTopologyNotPackedParameters();
    testBoundNodeEnableEditsDoNotInvalidateHostParameters();
    testStateRestoreCancelsAnInFlightNodeEnableDescriptor();
    testAutomationWriteGateControlsOnDemandBinding();
    testBundledGesturesReachTheHostInCollectedOrder();
    testBundledGestureBindsOnDemandUnderTheWriteGate();
    testWithheldBindingNeverClaimsAnAutomationLane();
    testReconfigurationPreservesUndrainedAutomationCurrent();
    testInactivePipelineAutomationLeavesTheActiveRuntimeImageAlone();
    testControlOwnedRuntimeImageKeepsProcessedAudio();
    testControlServiceEditsNeverCostAProcessedBlock();
    testBypassAtomicTracksSuccessfulBlockEndpoint();
    testMasterBypassGestureReachesAudioWithoutHostEcho();
    testFailedTransactionRetriesLatestAutomationAndNotifiesOnce();
    testStateRestoreForceSurvivesUiRebuildBeforeAudioResume();
    testAudioIdleHorizonCoversSlowHostBlocks();
    testPendingFullImageSurvivesReconfiguration();
    testStoppedParameterImageIsServicedWithoutAudioPolling();
    testFullImageRefreshesLatencyOnceButAutomationDoesNot();
    testDeferredLatencyPlanRefreshAlignsParallelAndBypassPaths();
    testTopologyDescriptorIsServicedOutsideAudioCallback();
    testAutomationSliceBlockDoesNotAllocate();
    testTelemetryPollLeavesTheAudioGuardAlone();
    testPendingTopologyKeepsProcessedAudio();
    testPendingPlanConsumesNewUiGenerationAndTopologyRemoval();
    testNativeControlServiceHandlesAssetReadyAndClear();
    testGroupDelayAssetPublishesCurrentUiLatency();
    testHeldControlGuardKeepsProcessedTimelineWithLatency();
    testUiPacedControlServiceKeepsEveryBlockWet();
    testOwnedRuntimeImageKeepsItsDirtyFlagsThroughAFailedBlock();
    testAssetStatePollLeavesTheDspRunning();
    testNotReadyPluginUpdateWaitsForTheInFlightBlock();
    testOwnedAudioTimelineLeavesTheBlockUntouched();
    testNamedAutomationEditSurvivesAnUndrainedPublish();
    testNamedAutomationEditSurvivesAConcurrentDrain();
    testRefusedPluginUpdateLeavesTheStateDocumentUnchanged();
    testDelayBearingEditsDuringPlaybackKeepEveryBlockWet();
    testFailedDescriptorBackoffClaimsNothing();
    testFlushOnlyBlocksLetTheControlServiceReachTheDsp();
    testFailedPluginRebuildKeepsProcessing();
    testFailedBulkRebuildsPreserveTheWholePlayableGeneration();
    testGateClosesInsideTheControlLock();
    testTopologyEditsDuringPlaybackRaiseNoDiagnostic();
    testPendingWorkIsServicedWhenTheAudioCallbackQuiesces();
    testDelayBearingEditsWithAStoppedTransportKeepEveryBlockWet();
    testLiveLatencyCommitAlignsWetBypassAndHost();
    testRefreshFailuresStayOffTheAudioDiagnosticBurst();
    testHostRefusedGestureStillAdoptsTheUserValue();
    testOpenTouchOutranksHostAutomationInput();
    testAReleasedTouchHandsTheSlotBackOnTheNextBlock();
    testTheEditorFacingWriteFollowsTheHostOnceTheHandIsOff();
    testOneTouchReportsOneBeginEveryValueAndOneEnd();
    testADragReportsEveryValueInOrderAndNothingElse();
    testEveryDragValueReachesTheHostInTheCallThatMadeIt();
    testTheShortestDragIsATouchWithOneValueInIt();
    testTheOpeningValueWaitsForOneBlockAndTheDragThenFlows();
    testAGestureClosedBeforeAnyBlockStillReportsItsValue();
    testEveryValueBeforeTheBoundaryCollapsesIntoTheLatest();
    testNoClockReleasesTheValueWaitingOnABlock();
    testProcessCallsNoComponentHandlerMethodWhileAValueIsHeld();
    testTheControlServiceCarriesAHeldValueOnlyAfterTheBoundary();
    testALiveDragNeverRoutesThroughTheControlService();
    testTheHeldValueBelongsToTheParameterThatOpenedIt();
    testAnOpenTouchSuppressesOnlyItsOwnParameter();
    testADragReportsNoValueTheUserNeverMade();
    testNoLeakedTouch();
    testStateRestoreEndsAnOpenTouch();
    testStateRestoreClosesATouchReopenedBeforeTheReload();
    testClickActivatedControlOpensItsTouchBeforeItsValue();
    testRepeatedBeginKeepsOneTouchAndReopensAfterANativeClose();
    testSlotRetirementEndsTheTouchOutsideTheResourceLock();
    testRefusedMasterBypassStillBypassesTheDsp();
    testAutomationEditWithoutGestureFieldsStaysAOneShot();
    testLinkedMultiParameterBatchIsOneHostGroupEdit();
    testBoundSlotsRenderDenormalizedDisplayStrings();
    testDisplayStringsCarryTheStepsOwnDecimalCount();
    testSteppedIntegerAutomationMatchesStateTextAndAudio();
    testInvalidStatePipelineNeverReplacesTheAuthority();
    testStructuredParameterShapeChangeRebuildsTheRuntime();
    testStaleBulkRequestsCannotCrossStateReplacement();
    testStalePluginUpdateCannotOverwritePendingStateReplacement();
    testSetParamNormalizedAloneReachesTheSavedState();
    testStoppedControllerNodeEnableWritesReachTheActiveTopology();
    testSetParamNormalizedLeavesBypassAndUnboundSlotsAlone();
    testAutomationTraceFollowsItsEnvironmentVariable();
    std::cout << "EffeTune VST processing lifecycle tests passed\n";
    return 0;
  } catch (const std::exception &exception) {
    std::cerr << "Test failure: " << exception.what() << '\n';
    return 1;
  }
}
