#pragma once

// The SDK requires this header to precede other VST declarations.
#include "public.sdk/source/vst/vstsinglecomponenteffect.h"

#include "bridge/state_codec.h"
#include "bridge/config_store.h"
#include "bridge/preset_store.h"
#include "engine/block_adapter.h"
#include "engine/automation_catalog.h"
#include "engine/automation_scheduler.h"
#include "engine/command_queue.h"
#include "engine/dry_delay.h"
#include "engine/engine_host.h"
#include "engine/output_transition.h"
#include "engine/resampler.h"
#include "plugin/automation_parameters.h"
#include "plugin/automation_trace.h"
#include "plugin/plugin_ids.h"

#include "pluginterfaces/vst/ivstautomationstate.h"

#include <array>
#include <atomic>
#include <bitset>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace effetune::vst {
class WebViewHost;
}
namespace choc::value {
class Value;
}

namespace effetune::vst::plugin {

class PluginProcessorTestAccess;

class EffeTuneProcessor final : public Steinberg::Vst::SingleComponentEffect,
                                public Steinberg::Vst::IAutomationState {
public:
  EffeTuneProcessor();
  ~EffeTuneProcessor() override;

  static Steinberg::FUnknown *createInstance(void *context);

  Steinberg::tresult PLUGIN_API initialize(Steinberg::FUnknown *context) SMTG_OVERRIDE;
  Steinberg::tresult PLUGIN_API terminate() SMTG_OVERRIDE;
  Steinberg::tresult PLUGIN_API setActive(Steinberg::TBool state) SMTG_OVERRIDE;
  Steinberg::tresult PLUGIN_API setupProcessing(
      Steinberg::Vst::ProcessSetup &setup) SMTG_OVERRIDE;
  Steinberg::tresult PLUGIN_API setBusArrangements(
      Steinberg::Vst::SpeakerArrangement *inputs, Steinberg::int32 numInputs,
      Steinberg::Vst::SpeakerArrangement *outputs, Steinberg::int32 numOutputs) SMTG_OVERRIDE;
  Steinberg::tresult PLUGIN_API canProcessSampleSize(
      Steinberg::int32 symbolicSampleSize) SMTG_OVERRIDE;
  Steinberg::tresult PLUGIN_API process(Steinberg::Vst::ProcessData &data) SMTG_OVERRIDE;
  Steinberg::tresult PLUGIN_API setState(Steinberg::IBStream *stream) SMTG_OVERRIDE;
  Steinberg::tresult PLUGIN_API getState(Steinberg::IBStream *stream) SMTG_OVERRIDE;
  Steinberg::uint32 PLUGIN_API getLatencySamples() SMTG_OVERRIDE;
  Steinberg::IPlugView *PLUGIN_API createView(Steinberg::FIDString name) SMTG_OVERRIDE;
  Steinberg::tresult PLUGIN_API setAutomationState(Steinberg::int32 state) SMTG_OVERRIDE;
  // Observation-only pass-throughs for the opt-in automation trace.
  Steinberg::tresult PLUGIN_API getParameterInfo(
      Steinberg::int32 index, Steinberg::Vst::ParameterInfo &info) SMTG_OVERRIDE;
  Steinberg::Vst::ParamValue PLUGIN_API getParamNormalized(
      Steinberg::Vst::ParamID tag) SMTG_OVERRIDE;
  // Without these a host shows the raw 0..1 lane position next to the unit the
  // parameter published -- "0.5000 dB" where the control reads -6.000 dB. The
  // base class cannot do better: a Parameter holds only the normalized value,
  // and the scale that gives it meaning lives in the binding registry.
  Steinberg::tresult PLUGIN_API getParamStringByValue(
      Steinberg::Vst::ParamID tag, Steinberg::Vst::ParamValue valueNormalized,
      Steinberg::Vst::String128 string) SMTG_OVERRIDE;
  Steinberg::tresult PLUGIN_API getParamValueByString(
      Steinberg::Vst::ParamID tag, Steinberg::Vst::TChar *string,
      Steinberg::Vst::ParamValue &valueNormalized) SMTG_OVERRIDE;
  // Hosts also use this controller-facing entry point to restate values that
  // process() already received with sample offsets. Those queues remain the
  // playback authority. A finite fixed-size fallback retains controller-only
  // writes until the audio callback is known to be idle, so stopped generic
  // editors still reach the DSP and saved state without a late restatement
  // rewinding a running scheduler.
  Steinberg::tresult PLUGIN_API setParamNormalized(
      Steinberg::Vst::ParamID tag,
      Steinberg::Vst::ParamValue value) SMTG_OVERRIDE;

  [[nodiscard]] std::string handleUiMessage(std::string_view message);
  [[nodiscard]] bool attachEditor(void *owner, void *parent, std::int32_t width,
                                  std::int32_t height);
  void detachEditor(void *owner) noexcept;
  void resizeEditor(void *owner, std::int32_t width, std::int32_t height) noexcept;

  OBJ_METHODS(EffeTuneProcessor, SingleComponentEffect)
  REFCOUNT_METHODS(SingleComponentEffect)
  Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid,
                                               void **obj) SMTG_OVERRIDE {
    QUERY_INTERFACE(iid, obj, Steinberg::Vst::IAutomationState::iid,
                    Steinberg::Vst::IAutomationState)
    return SingleComponentEffect::queryInterface(iid, obj);
  }

private:
  friend class PluginProcessorTestAccess;

  // Defined below, once the touch bookkeeping it carries has been declared.
  // Every path that can retire an automation slot holds
  // processingResourcesMutex_ through this and through nothing else.
  class AutomationResourceLock;

  // Whether one explicit gesture ended up on an automation lane. Neither answer
  // is a failure and neither asks the UI for anything: the plug-in adopts the
  // user's value as its own DSP value either way, and an unbound target reached
  // the DSP through the plug-in image that carried it.
  enum class AutomationEditOutcome : std::uint8_t { bound, unbound };

  // Everything one explicit automation edit asks of the host besides its value.
  // The defaults are a discrete one-shot on a target that may claim a lane,
  // which is what every path meant before gestures were reported at all.
  struct AutomationEditIntent {
    // A user gesture may claim an automation lane on demand; a bulk overlay and
    // a gesture close may not.
    bool bindIfUnbound = true;
    bool beginGesture = true;
    bool endGesture = true;
  };

  enum class ProcessTransactionError : std::uint8_t {
    none,
    processingNotReady,
    invalidBuffer,
    dryDelayUnavailable,
    upsampleRejected,
    engineHostRejected,
    downsampleRejected
  };

  struct HostContextSnapshot {
    double sampleRate = 44100.0;
    double engineSampleRate = 44100.0;
    std::uint32_t channels = 2;
    std::uint32_t oversamplingFactor = 1;
    std::uint64_t generation = 1;
  };

  [[nodiscard]] bool configureDsp(std::string *error = nullptr,
                                  bool waitForUiRepack = false);
  // Callers hold processingResourcesMutex_ through the lock they pass in.
  // Preparing the engine destroys and reallocates everything the audio callback
  // reads, so the whole body runs inside an EngineMutationWindow of its own.
  [[nodiscard]] bool configureDspLocked(AutomationResourceLock &resources,
                                        std::string *error, bool waitForUiRepack);
  [[nodiscard]] bool reconfigureDspPreservingPipeline(
      double hostSampleRate, Steinberg::int32 maxHostFrames,
      Steinberg::int32 configuredChannels, std::string *error = nullptr);
  void publishHostContext(double sampleRate, std::uint32_t channels,
                          std::uint32_t oversamplingFactor) noexcept;
  [[nodiscard]] HostContextSnapshot readHostContext() const noexcept;
  void copyDry(const Steinberg::Vst::AudioBusBuffers &input,
               Steinberg::Vst::AudioBusBuffers &output, Steinberg::int32 frames) noexcept;
  void copyDryToScratch(const Steinberg::Vst::AudioBusBuffers &input,
                        Steinberg::int32 frames) noexcept;
  void restoreDryFromScratch(Steinberg::Vst::AudioBusBuffers &output,
                             Steinberg::int32 frames) noexcept;
  [[nodiscard]] bool readStream(Steinberg::IBStream *stream, std::string &contents) const;
  void notifyLatencyChange(Steinberg::uint32 previousLatency);
  void armLatencyNotification();
  void queueLatencyNotification(bool restartDebounce);
  // The UI reports the latency of the DSP image currently being rendered. This
  // may lead getLatencySamples() briefly while the non-real-time compensation
  // plan and the host PDC notification wait for a safe control window.
  [[nodiscard]] std::uint32_t processingLatencySamples() const noexcept;
  // Resizing the dry delay swaps the buffer the audio thread reads, so every
  // caller runs it inside a window that proved the audio thread is out of
  // process() first.
  [[nodiscard]] bool synchronizeLatencyLocked(bool &latencyChanged);
  // How long the rendered-block counter has to stand still before the transport
  // counts as stopped. It has to outlast one host block, and the host already
  // told us how long that is through setupProcessing().
  [[nodiscard]] std::chrono::steady_clock::duration
  audioIdleThreshold() const noexcept;
  // Takes the idleness sample every control thread owes and reports whether the
  // transport is stopped. Lock-free, so no caller can be starved out of it and
  // leave the timestamp stale enough to look idle.
  [[nodiscard]] bool observeAudioIdle(
      std::chrono::steady_clock::time_point now) noexcept;
  void serviceLatencyUpdates(bool restartDebounce = false);
  [[nodiscard]] bool hasPendingControlWork() const noexcept;
  [[nodiscard]] bool queueDescriptorUpdate(const PipelineState &pipeline,
                                           std::string *error = nullptr);
  [[nodiscard]] bool queueDescriptorUpdateLocked(const PipelineState &pipeline,
                                                 std::string *error = nullptr);
  void discardPendingDescriptorLocked() noexcept;
  // Pushing the runtime image into the engine is only this thread's job when it
  // owns the engine. While blocks are flowing the audio callback is the engine's
  // sole writer and stages every dirty image itself, so the control service
  // leaves the flags standing instead of racing the block for the DSP.
  [[nodiscard]] bool consumePendingControlUpdatesLocked(bool engineOwned) noexcept;
  // Commits controller-only parameter writes once audio has been observed idle.
  // The fixed bank is guarded by processingResourcesMutex_; the resulting
  // fixed-capacity handoff is ingested by process() before the host queues, so
  // a queue in that same block always supersedes the fallback.
  void commitPendingControllerWritesLocked();
  void commitPendingControllerWritesIfAudioIdle(bool waitForResources = false);
  void clearPendingControllerWritesLocked() noexcept;
  void invalidatePendingControllerWriteLocked(std::size_t index) noexcept;
  void publishControllerWriteHandoffLocked(
      std::size_t index, double normalized,
      std::uint64_t authorityGeneration) noexcept;
  void ingestControllerWriteHandoffs() noexcept;
  void synchronizeAutomationBindings(bool notifyHost);
  // Hosts that never report an automation state fall back to always allowing
  // allocation, so the feature cannot go silent on an optional interface.
  [[nodiscard]] bool automationAllocationPermitted() const noexcept {
    const auto state = hostAutomationState_.load(std::memory_order_acquire);
    return state < 0 ||
           (state & Steinberg::Vst::IAutomationState::kWriteState) != 0;
  }
  [[nodiscard]] std::optional<std::uint32_t>
  bindAutomationSlot(
      const AutomationTargetIdentity &identity,
      std::optional<std::uint64_t> expectedStateEpoch = std::nullopt);
  // A copy of the descriptor behind the automation slot a host parameter
  // identifier names, or nullopt for an identifier that is not an automation
  // slot and for a slot that holds no binding. The registry is guarded by
  // processingResourcesMutex_ and every reconcile rebuilds it, so the copy is
  // taken under the lock and read outside it: the host asks for display strings
  // from its own thread, whenever it repaints a lane.
  [[nodiscard]] std::optional<AutomationTargetDescriptor>
  boundAutomationTarget(Steinberg::Vst::ParamID parameterId);
  // The lane one explicit edit belongs on: the one the target already owns, or
  // a freshly claimed one when the caller allows it and the write gate permits
  // it. Split out of applyAutomationEdit() so a batch can claim every lane it
  // needs before the group edit around its begin/perform/end run is opened --
  // claiming a lane republishes the parameter bank through restartComponent(),
  // which does not belong inside a group. Idempotent: a target that already
  // owns a lane is answered from the registry, so running it twice over the
  // same edit costs one lookup. Callers hold neither mutex.
  [[nodiscard]] std::optional<std::uint32_t>
  resolveAutomationSlot(const AutomationTargetIdentity &identity,
                        double normalized, bool bindIfUnbound,
                        std::optional<std::uint64_t> expectedStateEpoch =
                            std::nullopt);
  // The whole of one explicit gesture: bind the target when it has none, tell
  // the host, then adopt the value natively. The adoption does not depend on
  // the host taking the edit transaction -- a host that refuses to record
  // automation is not saying the user's edit should not take effect -- so the
  // value becomes the plug-in's own either way and the UI never has to roll
  // back. A gesture is the one automation path allowed to claim a lane, so both
  // the single automation/edit message and the gestures bundled with a plug-in
  // update share it; a caller that clears bindIfUnbound may reconfigure a lane
  // that already exists but never open one.
  [[nodiscard]] AutomationEditOutcome applyAutomationEdit(
      const AutomationTargetIdentity &identity, double normalized,
      AutomationEditIntent intent = {});
  // Return the VST restart flags earned by the publication. Value changes and
  // parameter-info changes are separate; callers notify only after unlocking.
  [[nodiscard]] Steinberg::int32
  reconcileAutomationBindingsLocked(AutomationResourceLock &resources,
                                    bool forceCurrentInitialization = false);
  [[nodiscard]] Steinberg::int32
  finishAutomationReconcileLocked(AutomationResourceLock &resources,
                                   const AutomationReconcileResult &result,
                                   bool forceCurrentInitialization);
  // Retiring a slot has to end the touch that was open on it, and the lock is
  // what carries that touch out to where it can be ended: see
  // AutomationResourceLock below for why it may not be ended here.
  void configureAutomationSchedulerLocked(
      AutomationResourceLock &resources,
      bool forceCurrentInitialization = false) noexcept;
  // Adopts a value the user explicitly asked for on a slot that is already
  // bound, in the three steps every explicit path needs: the registry becomes
  // the authority for the target, the host parameter reports the adopted value,
  // and the scheduler is forced to take it instead of preserving the value it
  // is playing. Callers hold processingResourcesMutex_.
  void adoptAutomationEditLocked(std::uint32_t slot, double normalized) noexcept;
  void adoptAutomationAuthorityLocked(std::uint32_t slot,
                                      double normalized) noexcept;
  void publishAutomationApplyTableLocked() noexcept;
  // Blocks until no process() call is executing. Callers must not hold
  // stateMutex_, and the audio thread must never call it.
  void waitForAudioQuiescence() noexcept;
  // The only way a control thread may touch EngineHost's DSP state. Clearing
  // the ready flag stops the next block from entering the engine and waiting
  // the current one out proves none is inside it, so the audio callback needs
  // no lock of its own. Callers hold processingResourcesMutex_ so the windows
  // of the different control threads cannot interleave. Nesting is harmless:
  // an inner window observes the flag already clear and leaves it alone. The
  // claim is published before the flag is cleared, so a block that finds the
  // gate closed can tell control occupancy from an unprepared DSP.
  class EngineMutationWindow {
  public:
    explicit EngineMutationWindow(EffeTuneProcessor &owner) noexcept;
    ~EngineMutationWindow() noexcept;
    EngineMutationWindow(const EngineMutationWindow &) = delete;
    EngineMutationWindow &operator=(const EngineMutationWindow &) = delete;

    // Whether the DSP was playable when the window took the gate over. A path
    // that only preserves the previous state needs to know that.
    [[nodiscard]] bool wasReady() const noexcept { return restoreReady_; }
    // Overrides what the window leaves behind. A rebuild is what first makes
    // the DSP playable after a UI repack, so it opens the gate even when the
    // gate was closed on entry; every failure path abandons the restore so no
    // block can enter a half-written engine.
    void setRestoreOnClose(const bool restore) noexcept { restoreReady_ = restore; }

  private:
    EffeTuneProcessor &processor_;
    bool restoreReady_ = false;
  };
  // Claims the block timeline the audio callback owns before it reaches the
  // processing gate: the automation scheduler, the output transition and the
  // host-frame counters. EngineMutationWindow cannot cover them, because
  // clearing the ready flag only keeps the next block out of the engine, not
  // out of process(). Only the two callers that rebuild the timeline take
  // this, so an ordinary edit still keeps its transition and its automation
  // intake; those two are preparing the DSP, where there is no timeline left
  // to carry anyway. Callers hold processingResourcesMutex_.
  class AudioTimelineWindow {
  public:
    explicit AudioTimelineWindow(EffeTuneProcessor &owner) noexcept;
    ~AudioTimelineWindow() noexcept;
    AudioTimelineWindow(const AudioTimelineWindow &) = delete;
    AudioTimelineWindow &operator=(const AudioTimelineWindow &) = delete;

  private:
    EffeTuneProcessor &processor_;
    bool releaseClaim_ = false;
  };
  // Control callers hold processingResourcesMutex_. The audio callback is the
  // one exception: it calls this only while controlOwnsRuntimeImage_ is false,
  // which already makes it the mailbox's sole consumer for that block.
  void adoptPendingParameterImagesLocked(bool stageForAudio) noexcept;
  void acknowledgePublishedAutomationLocked() noexcept;
  void completeAutomationBlock(bool success) noexcept;
  void drainAutomationValues();
  void appendAutomationDeltas(choc::value::Value &result);
  void appendActiveAutomationSnapshot(choc::value::Value &result);
  void appendExecutionStates(choc::value::Value &result);
  void appendDeferredDiagnostics(choc::value::Value &result);
  void recordProcessTransactionFailure(ProcessTransactionError error) noexcept;
  // Refreshing the compensation plan or the reported latency is recurring
  // control work, so its failures never reach the audio-side failure burst:
  // that burst is re-armed by every successful block, which would turn a
  // persistent refresh failure into a warning repeated at the backoff interval,
  // and it would report the control failure in place of an audio one that is
  // still pending. The notice is raised once and re-armed by a refresh that
  // succeeds, exactly like the automation capacity warning.
  void recordPipelinePlanRefreshOutcome(bool succeeded) noexcept;
  // Collects the plug-in IDs both pipelines still reference and drops every
  // pending transfer whose plug-in is gone. Evicting the engine-side cache is
  // left to the caller: it needs a quiet engine, and both callers open one
  // immediately afterwards, so doing it here would cost a block of its own.
  [[nodiscard]] std::vector<std::uint32_t> pruneAssetTransfers();
  // One value inside a touch. beginEdit and endEdit are the boundaries of that
  // touch, not of the value: the SDK asks for beginEdit on mouse-down, one
  // performEdit per moved value and endEdit on mouse-up, and a host that keys
  // its automation writer on the touch window has nothing to write into when
  // every value is reported as a complete touch of its own. beginGesture opens
  // the touch when it is not open already, endGesture closes one that is, and a
  // discrete edit passes both.
  [[nodiscard]] bool performHostEditTransaction(
      Steinberg::Vst::ParamID parameterId, double normalized,
      bool beginGesture = true, bool endGesture = true) noexcept;
  // Opens one already-resolved host touch before a click-activated control has
  // a changed value to report. Idempotent for a touch that is already open.
  [[nodiscard]] bool openHostGesture(std::size_t index,
                                     Steinberg::Vst::ParamID parameterId,
                                     double previous) noexcept;
  // Reports one stale value only when its old-generation touch is still open.
  // It never opens a touch, changes the host Parameter, or adopts into the
  // binding registry/scheduler, so it is safe after a state-replacement epoch
  // has invalidated the plug-in image that carried the edit.
  void reportHostEditToOpenGesture(Steinberg::Vst::ParamID parameterId,
                                   double normalized,
                                   bool endGesture) noexcept;
  // The gesture-state index of a host parameter, or kNoHostGestureIndex for an
  // identifier no gesture can hold open. Pure and total, so the audio thread
  // can resolve a queue's parameter without touching any shared state.
  [[nodiscard]] static constexpr std::size_t
  hostGestureIndex(Steinberg::Vst::ParamID parameterId) noexcept {
    if (parameterId == kBypassParameterId) {
      return kBypassHostGestureIndex;
    }
    if (parameterId >= kFirstAutomationParameterId &&
        parameterId <= kLastAutomationParameterId) {
      return static_cast<std::size_t>(parameterId - kFirstAutomationParameterId);
    }
    return kNoHostGestureIndex;
  }
  [[nodiscard]] static constexpr Steinberg::Vst::ParamID
  hostGestureParameterId(const std::size_t index) noexcept {
    return index == kBypassHostGestureIndex
               ? kBypassParameterId
               : automationParameterId(static_cast<std::uint32_t>(index));
  }
  // Wait-free, and the only thing the audio thread reads of all this.
  [[nodiscard]] bool hostGestureOpen(
      Steinberg::Vst::ParamID parameterId) const noexcept {
    const auto index = hostGestureIndex(parameterId);
    return index != kNoHostGestureIndex &&
           hostGestureOpen_[index].load(std::memory_order_seq_cst);
  }
  // What a block is allowed to do with one parameter's input queue.
  enum class HostInputDisposition {
    apply,
    suppress,
  };
  // Answers what may be done with one value the host has stated for a
  // gesture-capable parameter. A single wait-free load, so it allocates nothing
  // and never blocks, and both interfaces a host can state a value through are
  // answered by this one rule: the audio thread for an input queue, and
  // setParamNormalized() for the editor-facing write. The rule is the whole of
  // it -- the user's hand is on this control, or it is not. Once the hand is
  // off, the very next thing the host states is applied, on that block.
  [[nodiscard]] HostInputDisposition
  classifyHostInput(Steinberg::Vst::ParamID parameterId) noexcept;
  // Withholds one value of an open gesture until the next process() boundary,
  // and reports whether it took it. Answers false for a gesture that holds
  // nothing back any more, which is every value after that boundary: the caller
  // then reports the value in the very call that produced it.
  //
  // Measured on Cakewalk Sonar (trace C, parameter 65556): a performEdit that
  // reaches the host 11 microseconds after its own beginEdit lands on the
  // punch-in position itself, and a lane with no earlier event then
  // back-extrapolates that first drag value over the whole region before the
  // drag -- the region is repainted one UI step away from what it held. One
  // process() boundary between the beginEdit and the first drag value is what
  // separates them into two positions.
  // `atOpen` is the call that opened the touch, and it is the only one that can
  // start a hold; every later call either joins one that is already standing or
  // is reported immediately.
  //
  // Control/UI threads only. The audio thread never withholds, never releases
  // and never touches an IComponentHandler method: all it does is advance
  // processBlockEpoch_, which is the only thing this mechanism reads from it.
  [[nodiscard]] bool holdHostEditValue(std::size_t index, double normalized,
                                       bool atOpen) noexcept;
  // Whether a process() boundary has passed since the value now held for this
  // gesture was withheld. The whole of the release condition: an epoch that
  // differs from the one recorded at the hold. Never a timeout, never an
  // elapsed-time measurement and never a tick count -- releasing on elapsed
  // time was measured to cost 57.6 ms and ten values of real finger motion out
  // of a drag, and is permanently out.
  [[nodiscard]] bool heldHostEditBoundaryCrossed(std::size_t index) const noexcept;
  // Drops a hold without reporting its value, for the one case where the value
  // is superseded: a newer value of the same gesture arrives after the boundary
  // has been crossed. Both would land at the same host position, so emitting
  // the older one is a wasted point.
  void discardHeldHostEdit(std::size_t index) noexcept;
  // Reports a withheld value to the host and stops withholding. Reports whether
  // a value was released. Control/UI threads only: performEdit() is an
  // IComponentHandler call.
  bool releaseHeldHostEdit(std::size_t index, bool blockBoundary) noexcept;
  // The control-service carrier. It is not the trigger: the trigger is and
  // remains the process() boundary, which this only observes, on the thread
  // that is allowed to call into the host. It exists for the one case the two
  // other observation points cannot cover -- a finger that stopped moving
  // mid-drag, whose value would otherwise sit until the gesture closed and land
  // at the release position instead of where the user made it.
  void serviceHeldHostEdits() noexcept;
  // Ends the touch on one parameter if it is open, and reports whether it was.
  // Control threads only: ending an edit calls into the host.
  bool closeHostGesture(std::size_t index) noexcept;
  // Ends every touch still open. The editor is the only thing that can hold one,
  // so every way it can go away without releasing the pointer -- the editor
  // closing, the component deactivating, the plug-in terminating -- runs this.
  void closeOpenHostGestures() noexcept;
  [[nodiscard]] std::int64_t automationBlockStart(
      const Steinberg::Vst::ProcessData &data, bool &rebase) noexcept;

  struct PendingAssetTransfer {
    RuntimeAsset asset;
    std::uint64_t operationRevision = 0;
    std::size_t receivedBytes = 0;
  };

  struct ControlServiceTimer;

  EngineHost engine_;
  BlockAdapter blockAdapter_;
  Oversampler oversampler_;
  DryDelayLine dryDelay_;
  OutputTransition outputTransition_;
  LatestParameterMailbox parameterMailbox_;
  std::optional<AudioCommand> pendingDescriptorCommand_;
  AutomationBindingRegistry automationBindings_;
  AutomationParameterBank automationParameters_;
  AutomationScheduler automationScheduler_;
  PresetStore presetStore_;
  ConfigStore configStore_;
  PluginStateDocument state_;
  UndoOpaqueStateStore undoOpaqueState_;
  std::string configJson_ = "{}";
  bool preserveMissingPipelineA_ = false;
  bool preserveMissingPipelineB_ = false;
  bool hasSavedState_ = false;
  std::vector<RuntimePlugin> runtimePlugins_;
  std::array<bool, kMaxPluginInstances> runtimeParameterDirty_{};
  std::array<bool, kMaxPluginInstances> runtimeFullImageDirty_{};
  static constexpr std::uint16_t kNoAutomationRuntimeIndex = UINT16_MAX;
  // Everything the audio thread needs to apply one automation slot, resolved
  // by the control thread. Trivially copyable and string-free, so the two
  // pre-allocated faces below can be swapped without the audio thread ever
  // observing a registry rebuild.
  struct AutomationApplyEntry {
    std::uint16_t runtimeIndex = kNoAutomationRuntimeIndex;
    std::uint32_t packedOffset = 0;
    AutomationDenormalization denormalization{};

    [[nodiscard]] bool operator==(const AutomationApplyEntry &) const = default;
  };
  using AutomationApplyTable = std::array<AutomationApplyEntry, kAutomationSlotCount>;
  std::array<AutomationApplyTable, 2> automationApplyTables_{};
  std::atomic<std::uint32_t> publishedAutomationTable_{0};
  // Control-thread only: true while the retired face is proven to be out of
  // reach of any in-flight block, so the next publish can skip the wait.
  bool automationApplyTableQuiesced_ = true;
  // One flag per parameter a touch can hold open: the 256 automation slots plus
  // the bypass parameter. Pre-allocated with the object, so opening or ending a
  // touch allocates nothing, and the audio thread's read of one entry is a
  // single wait-free load. While an entry is set the block ignores the host's
  // input queue for that parameter, which is what gives the hand on the control
  // precedence over the automation the host is playing back into it.
  static constexpr std::size_t kBypassHostGestureIndex = kAutomationSlotCount;
  static constexpr std::size_t kHostGestureCount = kAutomationSlotCount + 1u;
  static constexpr std::size_t kNoHostGestureIndex = kHostGestureCount;
  static_assert(std::atomic_bool::is_always_lock_free,
                "the audio thread reads the open-touch flags wait-free");
  std::array<std::atomic_bool, kHostGestureCount> hostGestureOpen_{};
  // The one value a gesture withholds from the host until a process() boundary
  // has passed, so the host cannot stamp it on the position its own punch-in
  // took at beginEdit. Armed by the call that opens the touch and dropped at
  // the first control-thread observation point after the boundary; from then on
  // the gesture holds nothing back at all and every value is reported in the
  // call that produced it.
  //
  // hostGestureHoldEpoch_ is the value processBlockEpoch_ had when the hold was
  // taken. The audio thread publishes nothing else and does nothing else here:
  // it advances that epoch at the end of every block, and the three
  // control-thread observation points -- the next value of the same gesture,
  // the control-service tick, and the close of the gesture -- are the only
  // places a performEdit is ever made. VST3 requires
  // beginEdit/performEdit/endEdit on the UI/controller thread, so the audio
  // callback may not make one at all.
  //
  // A value arriving while one is already held replaces it, so the whole of
  // what a gesture can lose is the values made inside that single sub-block
  // window. hostGestureHeldCount_ counts them for the trace, and is not read by
  // any decision.
  static_assert(std::atomic<double>::is_always_lock_free,
                "a withheld value is read without a lock");
  std::array<std::atomic_bool, kHostGestureCount> hostGestureHoldPending_{};
  std::array<std::atomic<double>, kHostGestureCount> hostGestureHeldValue_{};
  std::array<std::atomic<std::uint32_t>, kHostGestureCount> hostGestureHeldCount_{};
  std::array<std::atomic<std::uint64_t>, kHostGestureCount> hostGestureHoldEpoch_{};
  // How many parameters are withholding a value. A control-service tick that
  // reads zero here does nothing else, and it reads zero on every tick but the
  // ones inside the sub-block window that opens a touch.
  std::atomic<std::uint32_t> hostGestureHoldCount_{0};

  // processingResourcesMutex_ for every path that can retire an automation
  // slot, and the only way those paths take it.
  //
  // A slot retired under a live touch has to end that touch, and endEdit() is
  // not a request a host acts on later the way restartComponent() is: it is how
  // the host is told the user's hand has left a control, and hosts act on it
  // inline -- marking the project dirty, and in principle reaching straight
  // back into getState(), setState() or setActive(), each of which takes this
  // same non-recursive mutex. So the touch is only collected while the lock is
  // held, and ended by ~AutomationResourceLock() once it has been released.
  //
  // Requiring one of these by reference is what stops a future call site from
  // forgetting: configureAutomationSchedulerLocked() cannot be reached without
  // one, and the only object that can be passed is one that owns the lock. A
  // site that tried to hold the mutex some other way and construct this under
  // it would deadlock on its first run rather than deadlocking rarely, in a
  // host, months later.
  class AutomationResourceLock {
  public:
    explicit AutomationResourceLock(EffeTuneProcessor &processor)
        : processor_(processor), lock_(processor.processingResourcesMutex_) {}
    ~AutomationResourceLock();
    AutomationResourceLock(const AutomationResourceLock &) = delete;
    AutomationResourceLock &operator=(const AutomationResourceLock &) = delete;
    AutomationResourceLock(AutomationResourceLock &&) = delete;
    AutomationResourceLock &operator=(AutomationResourceLock &&) = delete;

    // Called with the lock held, for a slot whose binding has just gone away.
    void retireHostGesture(std::size_t index) noexcept;

  private:
    EffeTuneProcessor &processor_;
    std::bitset<kHostGestureCount> retiring_;
    std::unique_lock<std::mutex> lock_;
  };

  std::vector<std::uint8_t> telemetryScratch_;
  std::unordered_map<std::uint64_t, PendingAssetTransfer> pendingAssetTransfers_;
  std::vector<float> engineOutputBuffer_;
  std::vector<float> dryTransitionBuffer_;
  std::vector<std::uint8_t> hostBypassMask_;
  std::array<float *, EngineHost::kMaxChannels> engineOutputPointers_{};
  std::array<float *, EngineHost::kMaxChannels> dryTransitionPointers_{};
  std::atomic<double> hostSampleRate_{44100.0};
  double engineFramesProcessed_ = 0.0;
  std::int64_t processedHostFrames_ = 0;
  std::int64_t previousProjectTimeSamples_ = 0;
  std::uint32_t previousBlockFrames_ = 0;
  bool previousProjectTimeValid_ = false;
  bool previousPlaying_ = false;
  bool previousCycleActive_ = false;
  // Claimed by the control threads, and read outside processingResourcesMutex_
  // by the editor's frame-rate poll to decide whether a drain has anything to
  // do at all, so every entry is an atomic of its own.
  std::array<std::atomic<std::uint64_t>, kAutomationSlotCount>
      drainedAutomationGenerations_{};
  std::array<double, kAutomationSlotCount> pendingAutomationDeltaValues_{};
  std::array<AutomationTargetIdentity, kAutomationSlotCount>
      pendingAutomationDeltaIdentities_{};
  std::bitset<kAutomationSlotCount> pendingAutomationDeltaDirty_;
  std::atomic_bool automationDeltaPending_{false};
  struct PendingControllerWrite {
    bool pending = false;
    double normalized = 0.0;
    std::uint64_t authorityGeneration = 0;
    AutomationTargetIdentity identity;
  };
  std::array<PendingControllerWrite, kHostGestureCount> pendingControllerWrites_{};
  struct ControllerWriteHandoff {
    std::atomic<std::uint64_t> sequence{0};
    std::atomic<std::uint64_t> normalizedBits{0};
    std::atomic<std::uint64_t> authorityGeneration{0};
  };
  std::array<ControllerWriteHandoff, kHostGestureCount>
      controllerWriteHandoffs_{};
  std::array<std::uint64_t, kHostGestureCount>
      consumedControllerWriteSequences_{};
  std::array<std::atomic<std::uint64_t>, kHostGestureCount>
      controllerAuthorityGenerations_{};
  std::atomic<std::uint64_t> controllerWriteHandoffGeneration_{0};
  std::uint64_t consumedControllerWriteHandoffGeneration_ = 0;
  std::atomic_bool controllerWritePending_{false};
  std::uint64_t drainedProcessTransactionFailureSequence_ = 0;
  std::atomic<ProcessTransactionError> lastProcessTransactionError_{
      ProcessTransactionError::none};
  std::atomic_bool processTransactionFailureBurstActive_{false};
  std::atomic<std::uint64_t> processTransactionFailureSequence_{0};
  std::atomic_bool pipelinePlanRefreshWarningIssued_{false};
  std::atomic_bool pipelinePlanRefreshWarningPending_{false};
  std::atomic_bool automationCapacityWarningIssued_{false};
  std::atomic_bool automationCapacityWarningPending_{false};
  std::atomic_bool automationWriteGateWarningIssued_{false};
  std::atomic_bool automationWriteGateWarningPending_{false};
  // -1 until the host reports an automation state through IAutomationState.
  std::atomic<std::int32_t> hostAutomationState_{-1};
  std::atomic<Steinberg::int32> maxHostFrames_{0};
  // Capacity of the buffers belonging to the currently prepared DSP
  // generation. Unlike maxHostFrames_, this is not overwritten until a full
  // configuration succeeds. Guarded by processingResourcesMutex_.
  Steinberg::int32 preparedMaxHostFrames_ = 0;
  std::atomic<Steinberg::int32> configuredChannels_{2};
  std::atomic<Steinberg::uint32> latencySamples_{0};
  std::atomic<std::uint32_t> resamplerLatencySamples_{0};
  std::atomic<std::uint32_t> activeOversamplingFactor_{1};
  std::atomic<std::uint64_t> contextSequence_{0};
  std::atomic<double> publishedHostSampleRate_{44100.0};
  std::atomic<double> publishedEngineSampleRate_{44100.0};
  std::atomic<std::uint32_t> publishedChannels_{2};
  std::atomic<std::uint32_t> publishedOversamplingFactor_{1};
  std::atomic<std::uint64_t> contextGeneration_{1};
  std::atomic_bool bypass_{false};
  std::atomic_bool componentActive_{false};
  std::atomic_bool processingReady_{false};
  // setState() publishes the decoded document immediately, but the old DSP,
  // scheduler and binding projection remain one coherent playable generation
  // until the UI supplies the replacement runtime image.
  std::atomic_bool stateReplacementPending_{false};
  // Monotonically names the state generation across the pending true -> false
  // cycle. Writes and commit checks are guarded by processingResourcesMutex_;
  // the atomic publication lets a bulk bridge call capture its generation
  // before decoding without making ordinary telemetry wait on control work.
  std::atomic<std::uint64_t> stateReplacementEpoch_{0};
  // A startup handshake is the only signal that a request belongs to the page
  // reloaded by setState(). Bulk requests capture both values at entry and
  // recheck them inside their resource transaction, so an old page cannot be
  // mistaken for the replacement merely because it arrived while pending.
  // Writes and commit checks are guarded by processingResourcesMutex_; bridge
  // entry reads the published value without taking that control-side mutex.
  std::atomic<std::uint64_t> uiPageGeneration_{0};
  std::uint64_t replacementPageGeneration_ = 0;
  std::uint64_t replacementPageStateEpoch_ = 0;
  bool replacementPageAuthorized_ = false;
  // Odd while process() is executing. Lets a control thread observe that the
  // audio callback has left the block without the callback taking any lock.
  // It proves quiescence and nothing else: a flush-only block bumps it too, so
  // it cannot say whether audio is still being rendered.
  std::atomic<std::uint64_t> processBlockEpoch_{0};
  // Counts the blocks that carried audio. This says whether the audio callback
  // is still rendering, which is what decides whether the control thread has to
  // stage the pending images itself: process() returns on a flush-only block
  // before it stages anything, so a host that has gone quiet stops advancing
  // this and reads as idle. The transport state is deliberately not measured --
  // hosts keep the audio engine turning for live input monitoring, and treating
  // that as idle would let an ordinary parameter drag take the engine away from
  // a callback that is still rendering.
  std::atomic<std::uint64_t> renderedBlockCount_{0};
  // Accumulated only by the audio thread. The published fixed-point value is
  // read by the WebView/control threads without touching the accumulator.
  double pipelineCpuAudioSeconds_ = 0.0;
  std::chrono::steady_clock::duration pipelineCpuElapsed_{};
  std::atomic<std::uint32_t> pipelineCpuAverageHundredths_{0};
  // Number of control threads currently inside an EngineMutationWindow. A block
  // that finds the processing gate closed while this is set is not looking at an
  // unprepared DSP, so it stays out of the deferred diagnostics.
  std::atomic<std::uint32_t> controlEngineClaims_{0};
  // True while the control service owns the parameter mailbox and the runtime
  // parameter image. Both have a single reading position, so exactly one thread
  // may consume them. A block that starts while this is set keeps processing
  // with the values the engine already holds and skips its own staging.
  std::atomic_bool controlOwnsRuntimeImage_{false};
  // True while a control thread is rebuilding the block timeline. A block that
  // observes it leaves the input untouched instead of entering the callback.
  std::atomic_bool controlOwnsAudioTimeline_{false};
  std::atomic<char> activePipeline_{'A'};
  bool hostContextPublished_ = false;
  // The rendered-block count the control side last observed and when it last
  // changed. Idleness is the age of that change, not the difference between two
  // samples: the service is called far more often than a host block arrives, so
  // two consecutive samples are equal during ordinary playback. Every control
  // thread that reaches the service takes the sample, and both fields are
  // atomic so none of them needs a lock to do it.
  std::atomic<std::uint64_t> observedRenderedBlockCount_{0};
  std::atomic<std::int64_t> renderedBlockObservedAtTicks_{0};
  std::atomic<std::uint64_t> servicedLatencyRevision_{0};
  std::atomic<std::uint64_t> servicedPipelinePlanRevision_{0};
  std::atomic<std::uint64_t> descriptorGeneration_{0};
  std::atomic<std::uint64_t> servicedDescriptorGeneration_{0};
  std::atomic<std::uint64_t> parameterImageGeneration_{0};
  std::atomic<std::uint64_t> servicedParameterImageGeneration_{0};
  std::uint64_t failedPipelinePlanRevision_ = 0;
  std::uint64_t failedDescriptorGeneration_ = 0;
  std::uint64_t failedParameterImageGeneration_ = 0;
  std::uint32_t pipelinePlanRefreshFailureCount_ = 0;
  std::chrono::steady_clock::time_point pipelinePlanRetryDeadline_{};
  // Armed and fired from whichever control thread ran the edit, so the debounce
  // state is atomic rather than mutex-guarded: every field stands on its own and
  // the worst a lost race can cost is one extra host notification.
  std::atomic_bool latencyDebounceArmed_{false};
  std::atomic_bool latencyNotificationPending_{false};
  std::atomic<std::int64_t> latencyNotificationDeadlineTicks_{0};
  std::unique_ptr<ControlServiceTimer> controlServiceTimer_;
  // Which of the loaded plug-in instances a trace record came from, so a host
  // with several of them produces one readable log instead of an
  // unattributable interleaving. Zero, and read by nothing, while the trace is
  // off; see plugin/automation_trace.h.
  const std::uint32_t traceInstance_ = trace::nextInstanceId();
#if defined(EFFETUNE_PROCESSOR_TEST_HOOKS)
  std::uint32_t pipelinePlanRefreshFailuresForTesting_ = 0;
  std::atomic_bool pauseControllerCommitBeforePublishForTesting_{false};
  std::atomic_bool controllerCommitPausedForTesting_{false};
  std::atomic_bool pausePluginUpdateBeforeRuntimeTransactionForTesting_{false};
  std::atomic_bool pluginUpdatePausedBeforeRuntimeTransactionForTesting_{false};
  std::atomic_bool pausePluginUpdateBeforeAutomationEditsForTesting_{false};
  std::atomic_bool pluginUpdatePausedBeforeAutomationEditsForTesting_{false};
  std::atomic_bool pauseNextBulkRequestBeforeCommitForTesting_{false};
  std::atomic_bool bulkRequestPausedBeforeCommitForTesting_{false};
  std::atomic_bool releaseBulkRequestBeforeCommitForTesting_{false};
  std::atomic_bool pauseAutomationCatalogBeforeProjectionForTesting_{false};
  std::atomic_bool automationCatalogPausedBeforeProjectionForTesting_{false};
  std::atomic_bool pauseAutomationDrainBeforeDescriptorForTesting_{false};
  std::atomic_bool automationDrainPausedBeforeDescriptorForTesting_{false};
#endif
  mutable std::mutex stateMutex_;
  // Serializes the control threads against one another: the host thread, the
  // control-service timer, the WebView message handler and the automation-state
  // notification all reach the registry, the runtime image and the pending
  // descriptor. The audio callback never takes this mutex, so no amount of
  // concurrent control work can cost a block.
  std::mutex processingResourcesMutex_;
  // Guards the pending automation deltas alone. The audio callback never
  // takes it, so the editor's frame-rate drain cannot stall a block.
  std::mutex automationDeltaMutex_;
  std::mutex editorMutex_;
  std::mutex assetTransferMutex_;
  std::shared_ptr<WebViewHost> webView_;
  bool editorTerminating_ = false;
  std::mutex presetExchangeMutex_;
  std::optional<std::string> readablePresetPath_;
  std::optional<std::string> writablePresetPath_;
};

} // namespace effetune::vst::plugin
