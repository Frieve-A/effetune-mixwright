#include "plugin_processor.h"

#include "engine/latency.h"
#include "external_url.h"
#include "plugin_ids.h"
#include "native_file_dialog.h"
#include "plugin_view.h"
#include "version.h"

#include "bridge/message_router.h"
#include "bridge/webview_host.h"

#include "choc/text/choc_JSON.h"
#include "choc/memory/choc_Base64.h"
#include "choc/gui/choc_MessageLoop.h"

#include "base/source/fstring.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/base/ustring.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/vstspeaker.h"
#include "public.sdk/source/vst/vstaudioprocessoralgo.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <limits>
#include <new>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>

namespace effetune::vst::plugin {

using namespace Steinberg;
using namespace Steinberg::Vst;

struct EffeTuneProcessor::ControlServiceTimer {
  choc::messageloop::Timer timer;
};

namespace {

void traceRestartComponent(IComponentHandler &handler, const int32 flags,
                            const std::uint32_t instance) {
  const auto tracing = trace::enabled();
  if (tracing) {
    trace::restart(instance, trace::Event::restartBegin, flags);
  }
  const auto result = handler.restartComponent(flags);
  if (tracing) {
    trace::restart(instance, trace::Event::restartEnd, flags,
                    static_cast<std::int32_t>(result));
  }
}

[[nodiscard]] bool hasChannelPointers(const AudioBusBuffers &bus) noexcept {
  if (bus.numChannels < 0 || bus.channelBuffers32 == nullptr) {
    return false;
  }
  for (int32 channel = 0; channel < bus.numChannels; ++channel) {
    if (bus.channelBuffers32[channel] == nullptr) {
      return false;
    }
  }
  return true;
}

// Brackets a run of beginEdit/performEdit/endEdit calls in
// IComponentHandler2::startGroupEdit()/finishGroupEdit(), so a host that
// supports the interface stamps every parameter of the run at one timestamp
// instead of at whatever moment each individual edit happened to reach it. One
// pointer drag on a linked control writes several parameters at once -- a PEQ
// graph marker moves a band's frequency and its gain together, a linked
// multi-channel panel propagates one move across every linked channel -- and
// without the group the host records them as a staircase of separate points.
//
// finishGroupEdit() has to run on every exit path from the bracketed region,
// including an early return and an exception, so the pairing is owned by a
// scope guard rather than written out at each return.
//
// The base class already resolved IComponentHandler2 for us:
// EditController::setComponentHandler() queries the host handler for it, and
// startGroupEdit()/finishGroupEdit() answer kNotImplemented when the host has
// none. Nothing is closed that was not opened, so a host without the interface
// sees no calls at all.
//
// Control threads only: this is one half of the edit transaction, and the audio
// thread never issues those.
class ScopedHostGroupEdit {
public:
  // `traceInstance` names the plug-in instance in the automation trace and is
  // read by nothing else; see plugin/automation_trace.h.
  ScopedHostGroupEdit(EditController &controller, const bool wanted,
                      const std::uint32_t traceInstance = 0) noexcept
      : controller_(controller), traceInstance_(traceInstance) {
    if (!wanted) {
      return;
    }
    const auto result = controller_.startGroupEdit();
    if (trace::enabled()) {
      trace::hostEdit(traceInstance_, trace::Event::hostStartGroupEdit,
                      trace::kAllParameters, 0.0, static_cast<std::int32_t>(result));
    }
    open_ = result == kResultOk || result == kResultTrue;
  }

  ~ScopedHostGroupEdit() noexcept {
    if (open_) {
      const auto result = controller_.finishGroupEdit();
      if (trace::enabled()) {
        trace::hostEdit(traceInstance_, trace::Event::hostFinishGroupEdit,
                        trace::kAllParameters, 0.0, static_cast<std::int32_t>(result));
      }
    }
  }

  ScopedHostGroupEdit(const ScopedHostGroupEdit &) = delete;
  ScopedHostGroupEdit &operator=(const ScopedHostGroupEdit &) = delete;

private:
  EditController &controller_;
  std::uint32_t traceInstance_ = 0;
  bool open_ = false;
};

// What the two boolean-normalized display strings are. A boolean target has no
// unit and exactly two positions, so a number would tell the user nothing the
// words do not.
inline constexpr std::string_view kBooleanOnText = "On";
inline constexpr std::string_view kBooleanOffText = "Off";

// Writes a UTF-8 string into a host String128. UString::assign() copies through
// a bounded StringCopy, so a longer string is truncated rather than written past
// the 128 UTF-16 units the host owns.
void assignString128(TChar *destination, const std::string &value) noexcept {
  String converted;
  converted.fromUTF8(value.c_str());
  UString(destination, 128).assign(converted.text16());
}

[[nodiscard]] std::string utf8FromTChars(const TChar *string) {
  String converted(const_cast<TChar *>(string));
  converted.toMultiByte(kCP_Utf8);
  const auto *text = converted.text8();
  return text == nullptr ? std::string{} : std::string(text);
}

[[nodiscard]] std::string trimmed(const std::string &value) {
  const auto isSpace = [](const unsigned char character) noexcept {
    return character == ' ' || character == '\t' || character == '\r' ||
           character == '\n';
  };
  auto first = value.begin();
  while (first != value.end() && isSpace(static_cast<unsigned char>(*first))) {
    ++first;
  }
  auto last = value.end();
  while (last != first && isSpace(static_cast<unsigned char>(*(last - 1)))) {
    --last;
  }
  return std::string(first, last);
}

[[nodiscard]] bool equalsIgnoringCase(const std::string &value,
                                      const std::string_view expected) noexcept {
  if (value.size() != expected.size()) {
    return false;
  }
  for (std::size_t index = 0; index < value.size(); ++index) {
    const auto left = static_cast<unsigned char>(value[index]);
    const auto right = static_cast<unsigned char>(expected[index]);
    if ((left | 0x20u) != (right | 0x20u)) {
      return false;
    }
  }
  return true;
}

// How many decimals a public value is printed with. The authority is the
// EffeTune window itself: plugins/plugin-base.js createParameterControl() fills
// its number box with toFixed(step < 0.01 ? 3 : (step < 0.1 ? 2 : (step < 1 ? 1
// : 0))), where step is the control's granularity. Deriving the count from the
// value instead would print a threshold the window shows as -6.0 as -6.0000, so
// the step travels with the parameter -- declared in each params.json, carried
// through the generated catalog into AutomationTargetDescriptor -- and the same
// rule is applied here. The step is expressed in the units the parameter is
// published in, which is what minimum, maximum and the printed value use.
[[nodiscard]] int displayedDecimals(const double step) noexcept {
  if (!(step > 0.0)) {
    // A target with no usable granularity, such as the synthetic enable lane.
    // Whole numbers are the least surprising fallback.
    return 0;
  }
  // Compared at float precision on purpose. The generated catalog stores the
  // step as a float, and 0.01 has no exact binary form: widened back to double
  // it is 0.00999999977, just under the boundary, which would give a ratio
  // stepped by 0.01 three decimals where the window gives two. Rounding the
  // boundary the same way the stored value was rounded puts a step written as
  // 0.01 back exactly on it.
  const auto granularity = static_cast<float>(step);
  return granularity < 0.01f ? 3 : (granularity < 0.1f ? 2 : (granularity < 1.0f ? 1 : 0));
}

// The number a user should see behind a normalized lane position, without the
// unit. Enum positions print their generated name; every other position prints
// the public value, which is what the target's minimum, maximum and unit are
// expressed in.
[[nodiscard]] std::string
displayedAutomationValue(const AutomationTargetDescriptor &target,
                         const double publicValue) {
  if (target.normalization == AutomationValueNormalization::boolean) {
    return std::string(publicValue == 0.0 ? kBooleanOffText : kBooleanOnText);
  }
  if (target.normalization == AutomationValueNormalization::enumeration) {
    // The enum index is the public value, exactly as the state writer reads it:
    // applyAutomationNormalizedValue() indexes the generated name table with
    // lround() of the same number.
    const auto index = std::llround(publicValue);
    if (index >= 0) {
      const auto name = automationEnumValueName(
          target.identity, static_cast<std::uint32_t>(index));
      if (name.has_value()) {
        return std::string(*name);
      }
    }
    // A position the generated catalog has no name for. The index is still what
    // the plug-in stores, so it is the truthful answer rather than a blank.
    return std::to_string(index);
  }
  const auto decimals =
      target.normalization == AutomationValueNormalization::integer
          ? 0
          : displayedDecimals(target.step);
  std::array<char, 64> buffer{};
  const auto written = std::snprintf(buffer.data(), buffer.size(), "%.*f",
                                     decimals, publicValue);
  if (written <= 0) {
    return {};
  }
  return std::string(buffer.data(),
                     std::min(static_cast<std::size_t>(written), buffer.size() - 1u));
}

// Reads the leading number of a typed string, ignoring whatever unit the user
// left on the end. Returns nullopt when the text carries no number at all, so a
// typo is refused instead of silently becoming zero.
[[nodiscard]] std::optional<double> parseLeadingNumber(const std::string &text) noexcept {
  char *end = nullptr;
  const auto value = std::strtod(text.c_str(), &end);
  if (end == text.c_str() || !std::isfinite(value)) {
    return std::nullopt;
  }
  return value;
}

[[nodiscard]] bool refreshRuntimeExecutionAdmission(
    const PipelineState &pipeline, std::vector<RuntimePlugin> &runtimes,
    const double sampleRate, const std::uint32_t channels) noexcept {
  auto skipped = false;
  for (auto &runtime : runtimes) {
    const auto logical = std::find_if(
        pipeline.plugins.begin(), pipeline.plugins.end(),
        [&runtime](const PluginState &candidate) {
          return candidate.id == runtime.logicalId;
        });
    runtime.contextuallyBypassed =
        logical != pipeline.plugins.end() &&
        !supportsExecutionContext(runtime.executionCapabilities,
                                  logical->channel, sampleRate, channels);
    skipped = skipped || runtime.contextuallyBypassed;
  }
  return skipped;
}

[[nodiscard]] bool exceedsNativeStateCapacity(
    const PipelineState &pipeline,
    const std::unordered_map<std::string, KernelInfo> &kernels) {
  std::size_t nativeInstances = 0;
  for (const auto &plugin : pipeline.plugins) {
    try {
      const auto extra = choc::json::parse(plugin.extraJson);
      const auto type = extra["type"].getWithDefault<std::string>({});
      if (!type.empty() && kernels.contains(type) &&
          ++nativeInstances > kMaxPluginInstances) {
        return true;
      }
    } catch (const choc::json::ParseError &) {
      // StateCodec produced extraJson, so malformed extras are only possible
      // in an internally constructed legacy document. They carry no reliable
      // serialized type and therefore cannot claim a native instance here.
    }
  }
  return false;
}

} // namespace

EffeTuneProcessor::EffeTuneProcessor()
    : telemetryScratch_(EngineHost::kDefaultTelemetryBytes) {
  state_.appVersion = EFFETUNE_PLUGIN_VERSION_STR;
#if defined(EFFETUNE_AUTOMATION_HOST_GATE_FIXTURE)
  state_ = automationHostGateFixtureDocument();
  state_.appVersion = EFFETUNE_PLUGIN_VERSION_STR;
  hasSavedState_ = true;
  preserveMissingPipelineA_ = true;
  preserveMissingPipelineB_ = true;
#endif
  std::string config;
  if (configStore_.load(config)) {
    try {
      const auto parsed = choc::json::parse(config);
      if (parsed.isObject()) {
        configJson_ = std::move(config);
        const auto columns = parsed["columns"].getWithDefault<std::int64_t>(1);
        if (columns >= 1 && columns <= 8) {
          state_.ui.columns = static_cast<std::uint32_t>(columns);
        }
      }
    } catch (const choc::json::ParseError &) {
    }
  }
}

EffeTuneProcessor::~EffeTuneProcessor() = default;

FUnknown *EffeTuneProcessor::createInstance(void *context) {
  (void)context;
  return static_cast<IAudioProcessor *>(new EffeTuneProcessor());
}

tresult PLUGIN_API EffeTuneProcessor::initialize(FUnknown *context) {
  const auto result = SingleComponentEffect::initialize(context);
  if (result != kResultOk) {
    return result;
  }
  processContextRequirements.flags =
      IProcessContextRequirements::kNeedContinousTimeSamples |
      IProcessContextRequirements::kNeedTransportState |
      IProcessContextRequirements::kNeedProjectTimeMusic |
      IProcessContextRequirements::kNeedCycleMusic;
  addAudioInput(STR16("Main Input"), SpeakerArr::kStereo);
  addAudioOutput(STR16("Main Output"), SpeakerArr::kStereo);
  parameters.addParameter(STR16("Bypass"), nullptr, 1, 0,
                          ParameterInfo::kCanAutomate | ParameterInfo::kIsBypass,
                          kBypassParameterId);
  if (!automationParameters_.registerParameters(parameters)) {
    return kResultFalse;
  }
  synchronizeAutomationBindings(false);
  // The UI also owns the canonical JS parameter packers. Keep it alive while
  // the editor is closed so project state can rebuild a playable pipeline.
  try {
    auto webView = std::make_shared<WebViewHost>(
        [this](const std::string_view message) { return handleUiMessage(message); });
    std::scoped_lock editorLock(editorMutex_);
    editorTerminating_ = false;
    webView_ = std::move(webView);
  } catch (const std::exception &) {
    // A missing WebView runtime must not prevent the audio component loading;
    // attachEditor() retries and the processor continues with its current DSP.
    webView_.reset();
  }
  try {
    choc::messageloop::initialise();
    controlServiceTimer_ = std::make_unique<ControlServiceTimer>();
    controlServiceTimer_->timer = choc::messageloop::Timer(
        50u, [this] {
          const trace::ScopedRole traceRole{trace::Role::timer};
          // Node-enable automation reaches the DSP through the descriptor path,
          // so it must not depend on the editor polling the bridge.
          drainAutomationValues();
          serviceLatencyUpdates();
          // Carries a held gesture value to the host once a process() boundary
          // has separated it from its own beginEdit. This tick is a carrier and
          // not a trigger: it only observes processBlockEpoch_, on the thread
          // VST3 permits an IComponentHandler call from. See
          // serviceHeldHostEdits().
          serviceHeldHostEdits();
          // The only thread that writes the trace file. Everything above, and
          // every audio block since the last tick, has only appended to the
          // ring; this is where those records become lines. A no-op returning
          // on a null pointer when the trace is off.
          trace::flush();
          return true;
        });
  } catch (const std::exception &) {
    controlServiceTimer_.reset();
    return kResultFalse;
  }
  return kResultOk;
}

tresult PLUGIN_API EffeTuneProcessor::terminate() {
  // Before the component handler goes away with the base class: an edit left
  // open past it can never be ended at all.
  closeOpenHostGestures();
  controlServiceTimer_.reset();
  processingReady_.store(false, std::memory_order_seq_cst);
  waitForAudioQuiescence();
  std::shared_ptr<WebViewHost> retiredWebView;
  {
    std::scoped_lock editorLock(editorMutex_);
    editorTerminating_ = true;
    retiredWebView = std::move(webView_);
  }
  if (retiredWebView != nullptr) {
    retiredWebView->shutdown();
  }
  retiredWebView.reset();
  std::scoped_lock resources(processingResourcesMutex_);
  engineOutputBuffer_.clear();
  dryTransitionBuffer_.clear();
  // The timer that normally drains the ring is already gone, so the records
  // this teardown made would otherwise never be written.
  trace::flush();
  return SingleComponentEffect::terminate();
}

tresult PLUGIN_API EffeTuneProcessor::setActive(const TBool state) {
  if (!state) {
    componentActive_.store(false, std::memory_order_release);
    // A suspended component renders nothing and answers no more edits, so a
    // touch that is still open here would never be closed by the editor either.
    closeOpenHostGestures();
    // The host has now established the boundary the idle heuristic cannot infer
    // from a single instant between blocks. Controller-only writes that no
    // input queue superseded become the stopped DSP and save authority here.
    std::scoped_lock resources(processingResourcesMutex_);
    commitPendingControllerWritesLocked();
  }
  if (state) {
    // The VST3 contract already excludes a concurrent process() call here, but
    // the reset below must not depend on the host honouring it.
    std::scoped_lock resources(processingResourcesMutex_);
    const EngineMutationWindow engineWindow{*this};
    // The scheduler and the output transition are reset below. A block reaches
    // both before it reaches the engine gate, so keeping it out of the engine
    // is not enough: it has to stay out of the callback entirely.
    const AudioTimelineWindow timelineWindow{*this};
    blockAdapter_.reset();
    oversampler_.reset();
    engine_.reset();
    dryDelay_.reset();
    engineFramesProcessed_ = 0.0;
    processedHostFrames_ = 0;
    previousProjectTimeValid_ = false;
    previousPlaying_ = false;
    previousCycleActive_ = false;
    automationScheduler_.reset();
    outputTransition_.reset();
  }
  const auto result = SingleComponentEffect::setActive(state);
  if (state && (result == kResultOk || result == kResultTrue)) {
    componentActive_.store(true, std::memory_order_release);
  }
  if (trace::enabled()) {
    trace::active(traceInstance_, state != 0, result);
  }
  return result;
}

bool EffeTuneProcessor::configureDsp(std::string *error, const bool waitForUiRepack) {
  AutomationResourceLock resources{*this};
  return configureDspLocked(resources, error, waitForUiRepack);
}

bool EffeTuneProcessor::configureDspLocked(AutomationResourceLock &resources,
                                           std::string *error,
                                           const bool waitForUiRepack) {
  // Preparing the engine tears its instances down, so the gate has to be closed
  // and the audio thread proved out of process() from inside this lock: a window
  // opened before the lock would be reopened by whichever control thread already
  // held it, and this one would then rewrite the engine with the gate open.
  EngineMutationWindow engineWindow{*this};
  // Preparing the scheduler installs its slot storage, which a block reads
  // before the engine gate, so the whole callback has to stand aside.
  const AudioTimelineWindow timelineWindow{*this};
  // A configuration that stops short of rebuilding the pipeline leaves the
  // engine with no topology to process, so the gate stays closed until the UI
  // repacks and rebuilds. Every failure below leaves it closed as well.
  const auto restoreOnClose = engineWindow.wasReady() && !waitForUiRepack;
  engineWindow.setRestoreOnClose(false);
  adoptPendingParameterImagesLocked(false);
  discardPendingDescriptorLocked();
  const auto maxHostFrames = maxHostFrames_.load(std::memory_order_acquire);
  const auto configuredChannels = configuredChannels_.load(std::memory_order_acquire);
  const auto hostSampleRate = hostSampleRate_.load(std::memory_order_acquire);
  if (maxHostFrames <= 0 || configuredChannels <= 0) {
    if (error != nullptr) {
      *error = "Processing dimensions are not configured";
    }
    return false;
  }
  try {
    PluginStateDocument snapshot;
    {
      std::scoped_lock stateLock(stateMutex_);
      snapshot = state_;
    }
    oversampler_.prepare(snapshot.oversampling, static_cast<std::uint32_t>(configuredChannels),
                         static_cast<std::uint32_t>(maxHostFrames));
    const auto maxEngineFrames = static_cast<std::uint32_t>(maxHostFrames) *
                                 snapshot.oversampling.factor;
    blockAdapter_.prepare(static_cast<std::uint32_t>(configuredChannels), maxEngineFrames);
    if (!engine_.prepare(hostSampleRate * snapshot.oversampling.factor,
                         static_cast<std::uint32_t>(configuredChannels),
                         EngineHost::kDefaultTelemetryBytes, error)) {
      return false;
    }
    if (!waitForUiRepack) {
      const auto &pipeline =
          snapshot.currentPipeline == 'B' ? snapshot.pipelineB : snapshot.pipelineA;
      (void)refreshRuntimeExecutionAdmission(
          pipeline, runtimePlugins_, hostSampleRate * snapshot.oversampling.factor,
          static_cast<std::uint32_t>(configuredChannels));
      if (!engine_.rebuild(pipeline, runtimePlugins_, error)) {
        return false;
      }
      runtimeParameterDirty_.fill(false);
      runtimeFullImageDirty_.fill(false);
    }
    engineOutputBuffer_.assign(
        static_cast<std::size_t>(configuredChannels) * maxEngineFrames, 0.0f);
    dryTransitionBuffer_.assign(
        static_cast<std::size_t>(configuredChannels) * static_cast<std::size_t>(maxHostFrames),
        0.0f);
    hostBypassMask_.assign(static_cast<std::size_t>(maxHostFrames), 0u);
    for (int32 channel = 0; channel < configuredChannels; ++channel) {
      engineOutputPointers_[static_cast<std::size_t>(channel)] =
          engineOutputBuffer_.data() + static_cast<std::size_t>(channel) * maxEngineFrames;
      dryTransitionPointers_[static_cast<std::size_t>(channel)] =
          dryTransitionBuffer_.data() + static_cast<std::size_t>(channel) * maxHostFrames;
    }
    const auto resamplerLatency = oversampler_.latencyHostFrames();
    const auto totalLatency = calculateTotalLatency(
        resamplerLatency, snapshot.oversampling.factor, engine_.pipelineLatency());
    if (!dryDelay_.prepare(static_cast<std::uint32_t>(configuredChannels),
                           static_cast<std::uint32_t>(maxHostFrames), totalLatency)) {
      if (error != nullptr) {
        *error = "Unable to prepare the master-bypass delay";
      }
      return false;
    }
    resamplerLatencySamples_.store(resamplerLatency, std::memory_order_release);
    const auto previousReportedLatency = trace::enabled()
                                             ? latencySamples_.load(std::memory_order_acquire)
                                             : 0u;
    latencySamples_.store(totalLatency, std::memory_order_release);
    if (trace::enabled()) {
      trace::latency(traceInstance_, trace::Event::latencyPrepared,
                       previousReportedLatency, totalLatency, engine_.pipelineLatency(),
                       resamplerLatency, snapshot.oversampling.factor);
    }
    activeOversamplingFactor_.store(snapshot.oversampling.factor, std::memory_order_release);
    servicedLatencyRevision_.store(engine_.latencyRevision(),
                                   std::memory_order_release);
    servicedPipelinePlanRevision_.store(engine_.pipelinePlanRevision(),
                                         std::memory_order_release);
    failedPipelinePlanRevision_ = 0;
    pipelinePlanRefreshFailureCount_ = 0;
    recordPipelinePlanRefreshOutcome(true);
    const auto initializeAutomationScheduler = !automationScheduler_.prepared();
    if (!automationScheduler_.prepare(hostSampleRate)) {
      if (error != nullptr) {
        *error = "Unable to prepare the automation scheduler";
      }
      return false;
    }
    configureAutomationSchedulerLocked(resources, initializeAutomationScheduler);
    engineFramesProcessed_ = 0.0;
    publishHostContext(hostSampleRate, static_cast<std::uint32_t>(configuredChannels),
                       snapshot.oversampling.factor);
    servicedParameterImageGeneration_.store(
        parameterImageGeneration_.load(std::memory_order_acquire),
        std::memory_order_release);
    preparedMaxHostFrames_ = maxHostFrames;
    engineWindow.setRestoreOnClose(restoreOnClose);
    return true;
  } catch (const std::exception &exception) {
    if (error != nullptr) {
      *error = exception.what();
    }
    return false;
  }
}

bool EffeTuneProcessor::reconfigureDspPreservingPipeline(
    const double hostSampleRate, const int32 maxHostFrames,
    const int32 configuredChannels, std::string *error) {
  AutomationResourceLock resources{*this};
  // A live state restore has already published its document, but its runtime
  // image has not arrived yet. Re-preparing from that document here would pair
  // the new oversampling/scheduler state with the old runtime image and destroy
  // the playable engine setState() deliberately retained. The replacement
  // rebuild performs the complete reconfiguration as one transaction.
  if (stateReplacementPending_.load(std::memory_order_acquire)) {
    if (processingReady_.load(std::memory_order_seq_cst)) {
      const auto prepared = readHostContext();
      if (prepared.sampleRate == hostSampleRate &&
          prepared.channels == static_cast<std::uint32_t>(configuredChannels) &&
          preparedMaxHostFrames_ == maxHostFrames) {
        return true;
      }
    }
    // The old generation is only playable under the conditions it was prepared
    // for. Stop the whole callback before publishing different host dimensions,
    // then leave the DSP gate closed across every later setup/bus change until
    // the replacement runtime image can be configured against the final
    // dimensions as one generation.
    const AudioTimelineWindow timelineWindow{*this};
    EngineMutationWindow engineWindow{*this};
    engineWindow.setRestoreOnClose(false);
    hostSampleRate_.store(hostSampleRate, std::memory_order_release);
    maxHostFrames_.store(maxHostFrames, std::memory_order_release);
    configuredChannels_.store(configuredChannels, std::memory_order_release);
    return true;
  }
  // Publish new host dimensions only after the complete callback is quiet. A
  // block must never pair them with buffers or a resampler from the preceding
  // generation, even if a host violates setup's ordinary serialization rule.
  const AudioTimelineWindow timelineWindow{*this};
  hostSampleRate_.store(hostSampleRate, std::memory_order_release);
  maxHostFrames_.store(maxHostFrames, std::memory_order_release);
  configuredChannels_.store(configuredChannels, std::memory_order_release);
  // A DSP that is not playable right now is one whose pipeline the UI has not
  // repacked yet, and the gate only ever changes under this lock, so the read
  // is exact. The window inside preserves whatever it finds.
  return configureDspLocked(resources, error,
                            !processingReady_.load(std::memory_order_seq_cst));
}

bool EffeTuneProcessor::queueDescriptorUpdate(const PipelineState &pipeline,
                                              std::string *error) {
  std::scoped_lock resources(processingResourcesMutex_);
  return queueDescriptorUpdateLocked(pipeline, error);
}

bool EffeTuneProcessor::queueDescriptorUpdateLocked(const PipelineState &pipeline,
                                                    std::string *error) {
  AudioCommand command;
  if (!engine_.makeDescriptorCommand(pipeline, runtimePlugins_, command, error)) {
    return false;
  }
  pendingDescriptorCommand_ = std::move(command);
  const auto generation = descriptorGeneration_.fetch_add(1, std::memory_order_acq_rel) + 1u;
  if (trace::enabled()) {
    trace::descriptor(traceInstance_, trace::Event::descriptorQueued, generation,
                        static_cast<std::uint32_t>(pipeline.plugins.size()));
    for (const auto &plugin : pipeline.plugins) {
      trace::descriptor(traceInstance_, trace::Event::descriptorNode, generation,
                          plugin.id, plugin.enabled);
    }
  }
  return true;
}

void EffeTuneProcessor::discardPendingDescriptorLocked() noexcept {
  pendingDescriptorCommand_.reset();
  servicedDescriptorGeneration_.store(
      descriptorGeneration_.load(std::memory_order_acquire),
      std::memory_order_release);
  failedDescriptorGeneration_ = 0;
  failedParameterImageGeneration_ = 0;
}

bool EffeTuneProcessor::consumePendingControlUpdatesLocked(
    const bool engineOwned) noexcept {
  adoptPendingParameterImagesLocked(true);
  if (!engineOwned) {
    // Blocks are flowing, so the audio callback stages the dirty images itself
    // from its next block. Writing them from here would need the DSP while the
    // callback is in it, which is exactly what must never cost a block.
    return true;
  }
  for (std::size_t runtimeIndex = 0; runtimeIndex < runtimePlugins_.size();
       ++runtimeIndex) {
    if (!runtimeFullImageDirty_[runtimeIndex]) {
      continue;
    }
    const auto &runtime = runtimePlugins_[runtimeIndex];
    if (!engine_.updateParameters(runtime.logicalId, runtime.packedParameters,
                                  runtime.paramsHash, runtime.parameterBytes)) {
      return false;
    }
    runtimeParameterDirty_[runtimeIndex] = false;
    runtimeFullImageDirty_[runtimeIndex] = false;
  }
  return true;
}

void EffeTuneProcessor::adoptPendingParameterImagesLocked(
    const bool stageForAudio) noexcept {
  parameterMailbox_.consumePending([&](const AudioCommand &command) noexcept {
    const auto found = std::find_if(
        runtimePlugins_.begin(), runtimePlugins_.end(),
        [&command](const RuntimePlugin &runtime) {
          return runtime.logicalId == command.logicalId &&
                 runtime.paramsHash == command.paramsHash;
        });
    if (found == runtimePlugins_.end() ||
        command.floatCount != found->packedParameters.size() ||
        command.parameterByteCount != found->parameterBytes.size()) {
      return;
    }
    std::copy_n(command.packed.begin(), command.floatCount,
                found->packedParameters.begin());
    std::copy_n(command.parameterBytes.begin(), command.parameterByteCount,
                found->parameterBytes.begin());
    if (stageForAudio) {
      const auto runtimeIndex =
          static_cast<std::size_t>(found - runtimePlugins_.begin());
      runtimeParameterDirty_[runtimeIndex] = true;
      runtimeFullImageDirty_[runtimeIndex] = true;
    }
  });
}

void EffeTuneProcessor::clearPendingControllerWritesLocked() noexcept {
  for (std::size_t index = 0; index < kHostGestureCount; ++index) {
    auto &write = pendingControllerWrites_[index];
    write.pending = false;
    write.identity = {};
    controllerAuthorityGenerations_[index].fetch_add(1u,
                                                      std::memory_order_acq_rel);
  }
  controllerWritePending_.store(false, std::memory_order_release);
}

void EffeTuneProcessor::invalidatePendingControllerWriteLocked(
    const std::size_t index) noexcept {
  if (index >= kHostGestureCount) {
    return;
  }
  auto &write = pendingControllerWrites_[index];
  write.pending = false;
  write.identity = {};
  controllerAuthorityGenerations_[index].fetch_add(1u,
                                                    std::memory_order_acq_rel);
  const auto anyPending =
      std::any_of(pendingControllerWrites_.begin(), pendingControllerWrites_.end(),
                  [](const PendingControllerWrite &candidate) {
                    return candidate.pending;
                  });
  controllerWritePending_.store(anyPending, std::memory_order_release);
}

void EffeTuneProcessor::publishControllerWriteHandoffLocked(
    const std::size_t index, const double normalized,
    const std::uint64_t authorityGeneration) noexcept {
  auto &handoff = controllerWriteHandoffs_[index];
  const auto sequence = handoff.sequence.load(std::memory_order_relaxed);
  handoff.sequence.store(sequence + 1u, std::memory_order_release);
  handoff.normalizedBits.store(std::bit_cast<std::uint64_t>(normalized),
                               std::memory_order_relaxed);
  handoff.authorityGeneration.store(authorityGeneration,
                                    std::memory_order_relaxed);
  handoff.sequence.store(sequence + 2u, std::memory_order_release);
  controllerWriteHandoffGeneration_.fetch_add(1u, std::memory_order_release);
}

void EffeTuneProcessor::ingestControllerWriteHandoffs() noexcept {
  const auto published =
      controllerWriteHandoffGeneration_.load(std::memory_order_acquire);
  if (published == consumedControllerWriteHandoffGeneration_) {
    return;
  }
  auto consumedEveryWrite = true;
  for (std::size_t index = 0; index < kHostGestureCount; ++index) {
    const auto &handoff = controllerWriteHandoffs_[index];
    const auto first = handoff.sequence.load(std::memory_order_acquire);
    if ((first & 1u) != 0u) {
      consumedEveryWrite = false;
      continue;
    }
    if (first == consumedControllerWriteSequences_[index]) {
      continue;
    }
    const auto normalizedBits =
        handoff.normalizedBits.load(std::memory_order_relaxed);
    const auto authorityGeneration =
        handoff.authorityGeneration.load(std::memory_order_relaxed);
    const auto second = handoff.sequence.load(std::memory_order_acquire);
    if (first != second) {
      consumedEveryWrite = false;
      continue;
    }
    consumedControllerWriteSequences_[index] = first;
    if (controllerAuthorityGenerations_[index].load(std::memory_order_acquire) !=
        authorityGeneration) {
      continue;
    }
    const auto parameterId = index == kBypassHostGestureIndex
                                 ? kBypassParameterId
                                 : automationParameterId(
                                       static_cast<std::uint32_t>(index));
    automationScheduler_.beginQueue(parameterId);
    automationScheduler_.pushPoint(
        0, std::bit_cast<double>(normalizedBits));
    automationScheduler_.endQueue();
  }
  if (consumedEveryWrite) {
    consumedControllerWriteHandoffGeneration_ = published;
  }
}

void EffeTuneProcessor::commitPendingControllerWritesLocked() {
  if (!controllerWritePending_.load(std::memory_order_acquire)) {
    return;
  }
  std::scoped_lock stateLock(stateMutex_);
  auto activeTopologyChanged = false;
  for (std::size_t index = 0; index < kHostGestureCount; ++index) {
    auto &write = pendingControllerWrites_[index];
    if (!write.pending) {
      continue;
    }
    write.pending = false;
    if (controllerAuthorityGenerations_[index].load(std::memory_order_acquire) !=
        write.authorityGeneration) {
      continue;
    }
    if (index == kBypassHostGestureIndex) {
      const auto bypass = write.normalized >= 0.5;
      bypass_.store(bypass, std::memory_order_release);
      state_.masterBypass = bypass;
      publishControllerWriteHandoffLocked(index, write.normalized,
                                          write.authorityGeneration);
      continue;
    }
    const auto slot = static_cast<std::uint32_t>(index);
    const auto *binding = automationBindings_.binding(slot);
    const auto *target = automationBindings_.slot(slot);
    if (binding == nullptr || target == nullptr ||
        target->identity != write.identity) {
      continue;
    }
    const auto activeNodeEnableToggled =
        target->applyKind == AutomationApplyKind::nodeEnable &&
        (target->currentNormalized >= 0.5) != (write.normalized >= 0.5) &&
        binding->pipeline == state_.currentPipeline;
    adoptAutomationAuthorityLocked(slot, write.normalized);
    (void)applyAutomationValue(state_, *binding, write.normalized);
    activeTopologyChanged = activeTopologyChanged || activeNodeEnableToggled;
    publishControllerWriteHandoffLocked(index, write.normalized,
                                        write.authorityGeneration);
  }
  controllerWritePending_.store(false, std::memory_order_release);
  // Registry adoption makes the later automation drain correctly see no new
  // edge. Queue the descriptor here, after the document rewrite, while the
  // pre-adoption comparison still tells whether the active graph changed.
  if (activeTopologyChanged &&
      processingReady_.load(std::memory_order_seq_cst) &&
      !stateReplacementPending_.load(std::memory_order_acquire) &&
      queueDescriptorUpdateLocked(state_.currentPipeline == 'B' ? state_.pipelineB
                                                                : state_.pipelineA)) {
    armLatencyNotification();
  }
}

void EffeTuneProcessor::commitPendingControllerWritesIfAudioIdle(
    const bool waitForResources) {
  if (!controllerWritePending_.load(std::memory_order_acquire)) {
    return;
  }
  auto active = componentActive_.load(std::memory_order_acquire);
  if (active &&
      !observeAudioIdle(std::chrono::steady_clock::now())) {
    return;
  }
  std::unique_lock resources(processingResourcesMutex_, std::defer_lock);
  if (waitForResources) {
    resources.lock();
  } else {
    (void)resources.try_lock();
  }
  if (!resources.owns_lock()) {
    return;
  }
  // The first check deliberately precedes the mutex so a timer tick during
  // ordinary playback does no control work. Recheck after acquiring it. A
  // process() call that starts after this point remains uninterrupted: the
  // commit publishes a bounded handoff that the callback ingests before its
  // host queues instead of taking ownership of the block timeline.
  active = componentActive_.load(std::memory_order_acquire);
  if (active && !observeAudioIdle(std::chrono::steady_clock::now())) {
    return;
  }
#if defined(EFFETUNE_PROCESSOR_TEST_HOOKS)
  if (pauseControllerCommitBeforePublishForTesting_.load(
          std::memory_order_acquire)) {
    controllerCommitPausedForTesting_.store(true, std::memory_order_release);
    while (pauseControllerCommitBeforePublishForTesting_.load(
        std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    controllerCommitPausedForTesting_.store(false, std::memory_order_release);
  }
#endif
  commitPendingControllerWritesLocked();
}

void EffeTuneProcessor::synchronizeAutomationBindings(const bool notifyHost) {
  // This is the main topology-change path: it runs on every add/remove/reorder/
  // rename/enable while audio is playing and processingReady_ is still true.
  // The catalog and its projection are one control transaction. Building the
  // catalog before taking processingResourcesMutex_ allowed another topology
  // edit to commit in between, after which the old catalog could be projected
  // onto the new document and keep a removed target active. The audio callback
  // never takes this mutex, so keeping the unbounded JSON walk inside the
  // control-side transaction does not weaken the real-time contract.
  int32 restartFlags = 0;
  {
    // Leaving this scope releases the lock and only then ends the touches the
    // reconcile retired, so both host calls this function makes -- the endEdit
    // and the restartComponent below -- are issued with the mutex free.
    AutomationResourceLock resources{*this};
    // A restore deliberately keeps the old projection paired with the old
    // playable runtime until rebuildPipeline installs the replacement as one
    // generation. A topology update that linearized before setState() may
    // arrive here after it; in that ordering the restore wins and performs the
    // next projection itself.
    if (stateReplacementPending_.load(std::memory_order_acquire)) {
      return;
    }
    restartFlags = reconcileAutomationBindingsLocked(resources);
  }
  // An enable click changes topology, but not the bound parameter definition.
  // Its value is reported by the gesture, just like a packed-parameter edit.
  // Only an actual metadata change earns a global refresh on this path. Bulk
  // restores use the value-change flag separately below.
  if (notifyHost && (restartFlags & RestartFlags::kParamTitlesChanged) != 0) {
    if (auto *handler = getComponentHandler(); handler != nullptr) {
      traceRestartComponent(*handler, restartFlags, traceInstance_);
    }
  }
}

std::optional<std::uint32_t> EffeTuneProcessor::bindAutomationSlot(
    const AutomationTargetIdentity &identity,
    const std::optional<std::uint64_t> expectedStateEpoch) {
  // The eligibility decision, binding reservation and projection belong to
  // one control transaction. Otherwise a topology edit can replace state_
  // after the catalog is built and the stale target can permanently consume a
  // host slot. The audio callback never takes processingResourcesMutex_.
  std::optional<std::uint32_t> slot;
  auto capacityExhausted = false;
  int32 restartFlags = 0;
  {
    AutomationResourceLock resources{*this};
    // setState() keeps the old binding projection alive only so its existing
    // lanes can finish gestures against the retained playable generation. A
    // target that owns no lane must wait for the replacement runtime image:
    // reserving one now would project the restored document onto the old DSP
    // generation and permanently spend a host slot on a stale page update.
    if (stateReplacementPending_.load(std::memory_order_acquire) ||
        (expectedStateEpoch.has_value() &&
         stateReplacementEpoch_.load(std::memory_order_acquire) !=
             *expectedStateEpoch)) {
      return std::nullopt;
    }
    AutomationReconcileResult reconcileResult;
    {
      std::scoped_lock stateLock(stateMutex_);
      const auto eligibleTargets = eligibleAutomationTargets(state_);
#if defined(EFFETUNE_PROCESSOR_TEST_HOOKS)
      if (pauseAutomationCatalogBeforeProjectionForTesting_.load(
              std::memory_order_acquire)) {
        automationCatalogPausedBeforeProjectionForTesting_.store(
            true, std::memory_order_release);
        while (pauseAutomationCatalogBeforeProjectionForTesting_.load(
            std::memory_order_acquire)) {
          std::this_thread::yield();
        }
        automationCatalogPausedBeforeProjectionForTesting_.store(
            false, std::memory_order_release);
      }
#endif
      slot = bindAutomationTarget(state_, eligibleTargets, identity, capacityExhausted);
      if (slot.has_value()) {
        reconcileResult = automationBindings_.reconcile(state_, eligibleTargets);
      }
    }
    if (slot.has_value()) {
      restartFlags = finishAutomationReconcileLocked(
          resources, reconcileResult, /*forceCurrentInitialization=*/false);
      // reconcile() is the authority on which slot is actually projected.
      slot = automationBindings_.findActiveSlot(identity);
    }
  }
  if (capacityExhausted &&
      !automationCapacityWarningIssued_.exchange(true, std::memory_order_acq_rel)) {
    automationCapacityWarningPending_.store(true, std::memory_order_release);
  }
  if (restartFlags != 0) {
    if (auto *handler = getComponentHandler(); handler != nullptr) {
      traceRestartComponent(*handler, restartFlags, traceInstance_);
    }
  }
  return slot;
}

std::optional<AutomationTargetDescriptor>
EffeTuneProcessor::boundAutomationTarget(const ParamID parameterId) {
  if (parameterId < kFirstAutomationParameterId ||
      parameterId > kLastAutomationParameterId) {
    return std::nullopt;
  }
  const auto slot =
      static_cast<std::uint32_t>(parameterId - kFirstAutomationParameterId);
  std::scoped_lock resources(processingResourcesMutex_);
  const auto *target = automationBindings_.slot(slot);
  return target == nullptr ? std::nullopt
                           : std::optional<AutomationTargetDescriptor>(*target);
}

tresult PLUGIN_API EffeTuneProcessor::getParameterInfo(const int32 index,
                                                       ParameterInfo &info) {
  const auto result = SingleComponentEffect::getParameterInfo(index, info);
  if (trace::enabled()) {
    const auto accepted = result == kResultOk || result == kResultTrue;
    // A failed query leaves the caller's output untouched; do not read it.
    trace::parameterInfo(traceInstance_, trace::Event::getParameterInfo, index,
                          accepted ? info.id : trace::kAllParameters,
                          accepted ? info.stepCount : 0, accepted ? info.flags : 0,
                          accepted ? info.defaultNormalizedValue : 0.0, accepted);
  }
  return result;
}

ParamValue PLUGIN_API EffeTuneProcessor::getParamNormalized(const ParamID tag) {
  const auto value = SingleComponentEffect::getParamNormalized(tag);
  if (trace::tracksParameter(tag)) {
    trace::getParamNormalized(traceInstance_, tag, value);
  }
  return value;
}

tresult PLUGIN_API EffeTuneProcessor::getParamStringByValue(
    const ParamID tag, const ParamValue valueNormalized, String128 string) {
  const auto target = boundAutomationTarget(tag);
  if (!target.has_value()) {
    // The bypass parameter and every unbound slot. Bypass carries stepCount 1
    // and already renders as On/Off through the base class, and an unbound slot
    // has no scale to denormalize against: its placeholder publishes no unit,
    // so the lane position is the only honest number there is to show.
    return SingleComponentEffect::getParamStringByValue(tag, valueNormalized, string);
  }
  const auto publicValue = automationPublicValue(
      AutomationDenormalization{target->normalization, target->transform,
                                target->transformReference, target->minimum,
                                target->maximum, target->stepCount, target->step},
      valueNormalized);
  if (!publicValue.has_value()) {
    // A degenerate range or a position outside 0..1: the scale cannot say what
    // the value means, so the base class prints the lane position rather than a
    // number this code would have to invent.
    return SingleComponentEffect::getParamStringByValue(tag, valueNormalized, string);
  }
  // Deliberately without the unit. ParameterInfo::units already carries it --
  // applyActiveInfo() publishes the catalog's unit there -- and a host renders
  // that next to this string, so repeating it here reads as "-18.00 dB dB".
  // Both SDK parameter classes print the bare number for the same reason:
  // Parameter::toString and RangeParameter::toString never touch info.units.
  assignString128(string, displayedAutomationValue(*target, *publicValue));
  return kResultTrue;
}

tresult PLUGIN_API EffeTuneProcessor::getParamValueByString(
    const ParamID tag, TChar *string, ParamValue &valueNormalized) {
  const auto target = boundAutomationTarget(tag);
  if (!target.has_value()) {
    return SingleComponentEffect::getParamValueByString(tag, string, valueNormalized);
  }
  if (string == nullptr) {
    return kResultFalse;
  }
  const auto typed = trimmed(utf8FromTChars(string));
  const AutomationDenormalization denormalization{
      target->normalization, target->transform, target->transformReference,
      target->minimum,       target->maximum,   target->stepCount, target->step};
  std::optional<double> publicValue;
  if (target->normalization == AutomationValueNormalization::boolean) {
    if (equalsIgnoringCase(typed, kBooleanOnText)) {
      publicValue = 1.0;
    } else if (equalsIgnoringCase(typed, kBooleanOffText)) {
      publicValue = 0.0;
    }
  } else if (target->normalization == AutomationValueNormalization::enumeration) {
    // The exact name first, which is what getParamStringByValue() printed. The
    // match is exact on purpose: a near miss would set a position the user did
    // not name. A user who types the index instead falls through to the number
    // below, and the index is the public value there too.
    const auto index = automationEnumValueIndex(target->identity, typed);
    if (index.has_value()) {
      publicValue = static_cast<double>(*index);
    }
  }
  if (!publicValue.has_value()) {
    publicValue = parseLeadingNumber(typed);
  }
  if (!publicValue.has_value()) {
    return kResultFalse;
  }
  const auto normalized =
      automationNormalizedFromPublicValue(denormalization, *publicValue);
  if (!normalized.has_value()) {
    // The normalization cannot represent the value -- a non-positive number on a
    // logarithmic scale, a stepped target with no steps. Answering kResultFalse
    // leaves the host holding the value it had; a guess would quietly replace it
    // with one the user never typed.
    return kResultFalse;
  }
  valueNormalized = *normalized;
  return kResultTrue;
}

tresult PLUGIN_API EffeTuneProcessor::setParamNormalized(const ParamID tag,
                                                         const ParamValue value) {
  // This is the second interface a host states a lane through, and the SDK
  // describes it as the editor-facing one: "The controller must never pass this
  // value-change back to the host via the IComponentHandler. It should update
  // the according GUI element(s) only!" (ivsteditcontroller.h). With Read armed
  // a host calls it continuously to keep the plug-in's own editor following the
  // lane it is playing -- including through a drag it is recording over that
  // lane.
  //
  // So it is answered by the same rule the block applies to an input queue, and
  // decided before the Parameter takes the value: while the hand is on the
  // control the plug-in's own value is the authority for that slot. Letting the
  // restatement into the Parameter anyway would leave the value the host reads
  // back describing the lane rather than what the plug-in is playing. Once the
  // hand is off, the host is the authority again from the very next statement.
  const auto finiteValue = std::isfinite(value);
  const auto suppressed =
      finiteValue && classifyHostInput(tag) == HostInputDisposition::suppress;
  if (trace::enabled()) {
    trace::setParamNormalized(traceInstance_, tag, static_cast<double>(value),
                              finiteValue, suppressed);
  }
  if (suppressed) {
    return kResultTrue;
  }
  // The base class owns the Parameter object, and everything below depends on
  // it having taken the value, so it goes first and its answer is the answer.
  const auto result = SingleComponentEffect::setParamNormalized(tag, value);
  if (result != kResultOk && result != kResultTrue) {
    return result;
  }
  if (!std::isfinite(value)) {
    return result;
  }
  // Parameter::setNormalized() clamps, so any stopped fallback must retain the
  // clamped value the host reads back rather than the raw input.
  const auto adopted = std::clamp(static_cast<double>(value), 0.0, 1.0);
  const auto index = hostGestureIndex(tag);
  if (index == kNoHostGestureIndex) {
    return result;
  }
  const auto audioIdle =
      !componentActive_.load(std::memory_order_acquire) ||
      observeAudioIdle(std::chrono::steady_clock::now());
  // While audio is flowing this interface is display-only, however many blocks
  // after the queue the host happens to call it. There is no timing window that
  // can turn a playback restatement into DSP authority: only a call that begins
  // with the component inactive or the callback already proven idle may arm the
  // stopped fallback below.
  if (!audioIdle) {
    return result;
  }
  // No lane is ever claimed here. A host writing a slot that holds no binding is
  // writing one of the inactive placeholders the bank publishes to keep the
  // parameter count stable; there is no target for it to reach, and spending one
  // of the finite slots on it would be permanent. Such a slot leaves through the
  // null binding below, so the Parameter keeps the value and nothing else moves.
  //
  // Taking processingResourcesMutex_ on a host-called entry point is safe
  // because no path in this file calls into the host while holding it: every
  // restartComponent(), beginEdit(), performEdit(), endEdit() and group-edit
  // boundary is issued with the mutex free, by construction and on purpose. A
  // host therefore has no callback it could answer by re-entering here on a
  // thread that already owns the lock, which is the only way a non-recursive
  // mutex could deadlock on this path.
  {
    std::scoped_lock resources(processingResourcesMutex_);
    // Until the replacement runtime image arrives, every live automation
    // object still belongs to the old playable generation. A controller
    // restatement during the page reload may update its Parameter for the host,
    // but it must not be carried into either generation's DSP state.
    if (stateReplacementPending_.load(std::memory_order_acquire)) {
      return result;
    }
    if (index != kBypassHostGestureIndex) {
      const auto slot = static_cast<std::uint32_t>(index);
      const auto *target = automationBindings_.slot(slot);
      if (automationBindings_.binding(slot) == nullptr || target == nullptr) {
        return result;
      }
      pendingControllerWrites_[index].identity = target->identity;
    }

    auto &pending = pendingControllerWrites_[index];
    pending.normalized = adopted;
    pending.authorityGeneration =
        controllerAuthorityGenerations_[index].load(std::memory_order_acquire);
    pending.pending = true;
    controllerWritePending_.store(true, std::memory_order_release);
  }
  // The call was already proven stopped above. A generation that changed while
  // this control thread waited for the resource lock still cancels the fallback
  // in commitPendingControllerWritesLocked().
  commitPendingControllerWritesIfAudioIdle(
      /*waitForResources=*/true);
  return result;
}

std::optional<std::uint32_t> EffeTuneProcessor::resolveAutomationSlot(
    const AutomationTargetIdentity &identity, const double normalized,
    const bool bindIfUnbound,
    const std::optional<std::uint64_t> expectedStateEpoch) {
  // A value outside the normalized range describes no position of any control,
  // so nothing adopts it and no lane is claimed for it. Every other rejection
  // below is about lanes; this is the only one about the value itself, and it
  // is decided before the target can spend a slot on it.
  if (!std::isfinite(normalized) || normalized < 0.0 || normalized > 1.0) {
    return std::nullopt;
  }
  // The registry is guarded by processingResourcesMutex_, and reconcile()
  // rebuilds every slot. bindAutomationSlot() takes the same lock, so the
  // lookup keeps its own scope.
  std::optional<std::uint32_t> slot;
  {
    std::scoped_lock resources(processingResourcesMutex_);
    if (expectedStateEpoch.has_value() &&
        stateReplacementEpoch_.load(std::memory_order_acquire) !=
            *expectedStateEpoch) {
      return std::nullopt;
    }
    slot = automationBindings_.findActiveSlot(identity);
  }
  if (slot.has_value()) {
    return slot;
  }
  // An edit that may not claim a lane on a target that owns none is already
  // satisfied: the DSP takes the value from the image the edit travelled with,
  // and nothing is playing over it. Opening a lane for it would spend one of
  // the finite slots permanently -- a retired binding is never reused -- and
  // write an automation point the user never performed into a Write-enabled
  // lane.
  if (!bindIfUnbound) {
    return std::nullopt;
  }
  if (!automationAllocationPermitted()) {
    if (!automationWriteGateWarningIssued_.exchange(true, std::memory_order_acq_rel)) {
      automationWriteGateWarningPending_.store(true, std::memory_order_release);
    }
    return std::nullopt;
  }
  // Claiming the lane republishes the whole parameter bank through
  // restartComponent(). It happens before the touch is opened, and never
  // between the touch opening and the first value inside it.
  return bindAutomationSlot(identity, expectedStateEpoch);
}

EffeTuneProcessor::AutomationEditOutcome EffeTuneProcessor::applyAutomationEdit(
    const AutomationTargetIdentity &identity, const double normalized,
    const AutomationEditIntent intent) {
  const auto slot =
      resolveAutomationSlot(identity, normalized, intent.bindIfUnbound);
  if (!slot.has_value()) {
    return AutomationEditOutcome::unbound;
  }
  // The transaction only offers the value to the host, and its answer is
  // deliberately not consulted. A host that declines to record the edit -- no
  // Write lane armed, a read-only pass, an automation writer that is simply not
  // listening -- is not saying the user's edit should not take effect, and there
  // is no second value for anything to converge on: the transaction restores the
  // host parameter to its previous value when it fails, and the adoption below
  // overwrites that with the user's value again.
  (void)performHostEditTransaction(automationParameterId(*slot), normalized,
                                   intent.beginGesture, intent.endGesture);
  // beginEdit/performEdit/endEdit only notifies the host; it is not a route to
  // our own DSP. Adopt the gesture here with the same three steps the drain
  // applies to host-published values, so the block-start pin and the scheduler
  // follow the knob even when the host never echoes the edit back through
  // inputParameterChanges. An echo re-applies the identical value, so the two
  // paths stay idempotent. The forced configuration republishes the value, and
  // the existing drain converges the state document and the UI delta from it.
  std::scoped_lock resources(processingResourcesMutex_);
  adoptAutomationEditLocked(*slot, normalized);
  return AutomationEditOutcome::bound;
}

int32 EffeTuneProcessor::reconcileAutomationBindingsLocked(
    AutomationResourceLock &resources, const bool forceCurrentInitialization) {
  AutomationReconcileResult result;
  {
    std::scoped_lock stateLock(stateMutex_);
    const auto eligibleTargets = eligibleAutomationTargets(state_);
#if defined(EFFETUNE_PROCESSOR_TEST_HOOKS)
    if (pauseAutomationCatalogBeforeProjectionForTesting_.load(
            std::memory_order_acquire)) {
      automationCatalogPausedBeforeProjectionForTesting_.store(
          true, std::memory_order_release);
      while (pauseAutomationCatalogBeforeProjectionForTesting_.load(
          std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      automationCatalogPausedBeforeProjectionForTesting_.store(
          false, std::memory_order_release);
    }
#endif
    result = automationBindings_.reconcile(state_, eligibleTargets);
  }
  return finishAutomationReconcileLocked(resources, result,
                                          forceCurrentInitialization);
}

int32 EffeTuneProcessor::finishAutomationReconcileLocked(
    AutomationResourceLock &resources, const AutomationReconcileResult &result,
    const bool forceCurrentInitialization) {
  const auto metadataChanged = automationParameters_.apply(automationBindings_);
  if (metadataChanged && trace::enabled()) {
    // Read the bank directly: these are our published values, not host queries.
    for (int32 index = 0; index < parameters.getParameterCount(); ++index) {
      const auto *parameter = parameters.getParameterByIndex(index);
      const auto &info = parameter->getInfo();
      trace::parameterInfo(traceInstance_, trace::Event::parameterInfoPublished,
                            index, info.id, info.stepCount, info.flags,
                            info.defaultNormalizedValue, true);
    }
  }
  configureAutomationSchedulerLocked(resources, forceCurrentInitialization);
  publishAutomationApplyTableLocked();
  // Registry slots include currentNormalized, so slotsChanged alone cannot
  // establish that the host's cached ParameterInfo became invalid.
  if (metadataChanged) {
    return RestartFlags::kParamTitlesChanged | RestartFlags::kParamValuesChanged;
  }
  return result.slotsChanged ? RestartFlags::kParamValuesChanged : 0;
}

void EffeTuneProcessor::configureAutomationSchedulerLocked(
    AutomationResourceLock &resources,
    const bool forceCurrentInitialization) noexcept {
  automationScheduler_.configureBypass(bypass_.load(std::memory_order_acquire),
                                       forceCurrentInitialization);
  for (std::uint32_t slot = 0; slot < kAutomationSlotCount; ++slot) {
    const auto *target = automationBindings_.slot(slot);
    if (target == nullptr) {
      // A slot can be retired under a live touch: a plug-in removed by an undo,
      // a preset load or a state restore takes its lane with it. Nothing else
      // would ever end that touch, and the host would keep believing the user's
      // hand is on a control that no longer exists. The touch is handed to the
      // lock rather than ended here: the host processes endEdit() inline, and a
      // host that answered it by asking for the state would deadlock on the
      // mutex this loop is holding.
      resources.retireHostGesture(slot);
      automationScheduler_.deactivate(slot);
      continue;
    }
    (void)automationScheduler_.configure(
        slot, automationParameterId(slot),
        target->continuous ? AutomationMode::continuous : AutomationMode::stepped,
        target->currentNormalized, forceCurrentInitialization);
  }
}

void EffeTuneProcessor::adoptAutomationEditLocked(const std::uint32_t slot,
                                                  const double normalized) noexcept {
  invalidatePendingControllerWriteLocked(slot);
  adoptAutomationAuthorityLocked(slot, normalized);
  const auto *target = automationBindings_.slot(slot);
  if (target == nullptr) {
    return;
  }
  (void)automationScheduler_.configure(
      slot, automationParameterId(slot),
      target->continuous ? AutomationMode::continuous : AutomationMode::stepped,
      normalized, /*forceCurrentInitialization=*/true);
}

void EffeTuneProcessor::adoptAutomationAuthorityLocked(
    const std::uint32_t slot, const double normalized) noexcept {
  const auto *target = automationBindings_.slot(slot);
  if (target == nullptr) {
    return;
  }
  automationBindings_.setCurrentNormalized(slot, normalized);
  automationParameters_.setHostAdoptedValue(slot, normalized);
  // Anything the audio thread published before this gesture is older than it,
  // so the drain must not replay it over the adopted value. Claiming the
  // published generation here is what makes the adoption final; the drain
  // takes the same lock, so it either runs entirely before this or sees the
  // claim. Suppressing the host's input queue for a slot the user is holding
  // does not replace this: the bulk routes adopt through here with no touch
  // open at all, and a discrete edit -- one that opens and closes its touch in
  // the same transaction -- has already released the slot by the time it gets
  // here, so in both cases the queue is being ingested again around it.
  drainedAutomationGenerations_[slot].store(
      std::max(drainedAutomationGenerations_[slot].load(std::memory_order_relaxed),
               automationScheduler_.publishedGeneration(slot)),
      std::memory_order_release);
}

void EffeTuneProcessor::waitForAudioQuiescence() noexcept {
  const auto observed = processBlockEpoch_.load(std::memory_order_seq_cst);
  if ((observed & 1u) == 0u) {
    return;
  }
  while (processBlockEpoch_.load(std::memory_order_seq_cst) == observed) {
    std::this_thread::yield();
  }
}

EffeTuneProcessor::EngineMutationWindow::EngineMutationWindow(
    EffeTuneProcessor &owner) noexcept
    : processor_(owner) {
  // The claim is taken before the gate is closed, so a block that observes the
  // closed gate is guaranteed to observe the claim as well and can tell a
  // control thread holding the engine from a DSP that is not prepared.
  processor_.controlEngineClaims_.fetch_add(1u, std::memory_order_seq_cst);
  // Sequentially consistent on both sides: this thread clears the flag before
  // waiting the block epoch out, the audio thread bumps the epoch before
  // reading the flag, so no block can be inside the engine once the wait
  // returns.
  restoreReady_ =
      processor_.processingReady_.exchange(false, std::memory_order_seq_cst);
  processor_.waitForAudioQuiescence();
}

EffeTuneProcessor::EngineMutationWindow::~EngineMutationWindow() noexcept {
  if (restoreReady_) {
    processor_.processingReady_.store(true, std::memory_order_seq_cst);
  }
  processor_.controlEngineClaims_.fetch_sub(1u, std::memory_order_seq_cst);
}

EffeTuneProcessor::AudioTimelineWindow::AudioTimelineWindow(
    EffeTuneProcessor &owner) noexcept
    : processor_(owner),
      // Sequentially consistent on both sides, exactly like the runtime-image
      // claim: this thread stores the claim before waiting the block epoch
      // out, the audio thread bumps the epoch before reading it, so no block
      // can be inside the callback once the wait returns. Nesting is harmless:
      // an inner window observes the claim already held and leaves it alone.
      releaseClaim_(!owner.controlOwnsAudioTimeline_.exchange(
          true, std::memory_order_seq_cst)) {
  processor_.waitForAudioQuiescence();
}

EffeTuneProcessor::AudioTimelineWindow::~AudioTimelineWindow() noexcept {
  if (releaseClaim_) {
    processor_.controlOwnsAudioTimeline_.store(false, std::memory_order_release);
  }
}

void EffeTuneProcessor::publishAutomationApplyTableLocked() noexcept {
  const auto published = publishedAutomationTable_.load(std::memory_order_relaxed);
  const auto next = 1u - published;
  if (!automationApplyTableQuiesced_) {
    // A block that latched the retired face before the previous publish may
    // still be reading it, so prove it has left before overwriting the face.
    waitForAudioQuiescence();
    automationApplyTableQuiesced_ = true;
  }
  auto &staging = automationApplyTables_[next];
  staging.fill(AutomationApplyEntry{});
  const auto activePipeline = activePipeline_.load(std::memory_order_acquire);
  for (const auto slot : automationBindings_.activeSlots()) {
    if (slot >= kAutomationSlotCount) {
      continue;
    }
    const auto *binding = automationBindings_.binding(slot);
    const auto *target = automationBindings_.slot(slot);
    // Node-enable slots reach the DSP as descriptor updates from the control
    // thread, so they never carry a packed parameter for the audio thread.
    if (binding == nullptr || target == nullptr ||
        binding->pipeline != activePipeline ||
        target->applyKind != AutomationApplyKind::packedParameter) {
      continue;
    }
    const auto runtime = std::find_if(
        runtimePlugins_.begin(), runtimePlugins_.end(),
        [binding](const RuntimePlugin &candidate) {
          return candidate.logicalId == binding->pluginId &&
                 candidate.type == binding->pluginType;
        });
    if (runtime == runtimePlugins_.end() ||
        target->packedOffset >= runtime->packedParameters.size()) {
      continue;
    }
    auto &entry = staging[slot];
    entry.runtimeIndex =
        static_cast<std::uint16_t>(runtime - runtimePlugins_.begin());
    entry.packedOffset = target->packedOffset;
    entry.denormalization = {target->normalization, target->transform,
                             target->transformReference, target->minimum,
                             target->maximum, target->stepCount, target->step};
  }
  if (staging == automationApplyTables_[published]) {
    // Nothing the audio thread can observe changed. Keeping the current face
    // published leaves the retired face proven quiet, so a parameter-only edit
    // never costs a quiescence wait.
    return;
  }
  // Sequentially consistent on both sides: the control thread stores the index
  // before loading the block epoch, the audio thread bumps the epoch before
  // loading the index, so no block can latch a face this thread is about to
  // overwrite.
  publishedAutomationTable_.store(next, std::memory_order_seq_cst);
  automationApplyTableQuiesced_ = false;
}

void EffeTuneProcessor::completeAutomationBlock(const bool success) noexcept {
  automationScheduler_.completeBlock(success);
  bypass_.store(automationScheduler_.currentBypass(), std::memory_order_release);
  if (success) {
    processTransactionFailureBurstActive_.store(false, std::memory_order_release);
  }
}

void EffeTuneProcessor::acknowledgePublishedAutomationLocked() noexcept {
  for (std::uint32_t slot = 0; slot < kAutomationSlotCount; ++slot) {
    drainedAutomationGenerations_[slot].store(
        std::max(drainedAutomationGenerations_[slot].load(std::memory_order_relaxed),
                 automationScheduler_.publishedGeneration(slot)),
        std::memory_order_release);
  }
}

void EffeTuneProcessor::drainAutomationValues() {
  struct Drain {
    std::uint32_t slot = 0;
    PublishedAutomationValue value;
  };
  // A decoded state document is already the save/UI authority while its old DSP
  // generation keeps rendering. Values published by that old scheduler may not
  // be projected into the replacement document.
  if (stateReplacementPending_.load(std::memory_order_acquire)) {
    return;
  }
  // The editor polls this at frame rate, and a poll with nothing to drain must
  // not contend with control work, so the unlocked scan below decides whether
  // there is anything to do at all. It is only a hint: the generations are
  // claimed again under the lock, which is where the decision is binding.
  auto anyPublished = false;
  for (std::uint32_t slot = 0; slot < kAutomationSlotCount && !anyPublished; ++slot) {
    anyPublished =
        automationScheduler_.publishedGeneration(slot) >
        drainedAutomationGenerations_[slot].load(std::memory_order_acquire);
  }
  if (!anyPublished) {
    return;
  }

  std::vector<Drain> drains;
  drains.reserve(32);

  // Rewriting the state document re-parses and re-serializes plug-in JSON, which
  // is unbounded work. processingResourcesMutex_ therefore covers the bounded
  // registry, runtime image and descriptor work alone and is released before the
  // JSON rewrite, so the other control threads are not blocked across it. Both
  // sections keep the established processingResourcesMutex_ -> stateMutex_
  // acquisition order.
  struct PendingApply {
    AutomationBindingState binding;
    double normalized = 0.0;
  };
  std::vector<PendingApply> applies;

  std::unique_lock resources(processingResourcesMutex_);
  // setState() publishes the marker under this same lock. The unlocked check
  // above is only a fast path; this one closes the race with a concurrent
  // restore before any old generation is claimed or written into its document.
  if (stateReplacementPending_.load(std::memory_order_acquire)) {
    return;
  }
  // Claiming the published generations belongs inside the same section that
  // rewrites the registry from them. An explicit gesture claims them too, so
  // holding one lock across both is what keeps a value the audio thread
  // published before the gesture from being replayed over it.
  for (std::uint32_t slot = 0; slot < kAutomationSlotCount; ++slot) {
    PublishedAutomationValue published;
    if (automationScheduler_.readPublished(
            slot, drainedAutomationGenerations_[slot].load(std::memory_order_relaxed),
            published)) {
      drainedAutomationGenerations_[slot].store(published.generation,
                                                std::memory_order_release);
      drains.push_back({slot, published});
    }
  }
  if (drains.empty()) {
    return;
  }
  applies.reserve(drains.size());
  std::unique_lock stateLock(stateMutex_);
  // The editor drains pending deltas at frame rate. Snapshotting the identity
  // here keeps that read off processingResourcesMutex_, so a frame-rate poll
  // never contends with the control work that rebuilds the registry.
  std::unique_lock deltaLock(automationDeltaMutex_);
  auto activeTopologyChanged = false;
  for (const auto &drain : drains) {
    const auto *binding = automationBindings_.binding(drain.slot);
    const auto *target = automationBindings_.slot(drain.slot);
    if (binding == nullptr || target == nullptr ||
        !std::isfinite(drain.value.normalized)) {
      continue;
    }
    const auto isPacked = target->applyKind == AutomationApplyKind::packedParameter;
    const auto packed = isPacked ? denormalizeAutomationPackedValue(
                                       *target, drain.value.normalized)
                                 : std::optional<float>{};
    // A value the parameter conversion rejects never reaches the state document,
    // so it must not reach the registry or the runtime image either.
    if (isPacked && !packed.has_value()) {
      continue;
    }
    const auto enableToggled =
        !isPacked &&
        (target->currentNormalized >= 0.5) != (drain.value.normalized >= 0.5);
    automationBindings_.setCurrentNormalized(drain.slot, drain.value.normalized);
    automationParameters_.setHostAdoptedValue(drain.slot, drain.value.normalized);
    // The runtime image is not written here. The audio thread owns it: every
    // block pins the scheduler's current value for each bound slot on the
    // active pipeline, and the rebuild paths overlay the registry value the
    // call above just recorded. Writing it from this thread would only repeat
    // that, and it could not tell an inactive-pipeline binding from an active
    // one, so a bound target on the idle pipeline would overwrite the same
    // logical ID on the pipeline that is playing.
    if (!isPacked && enableToggled && binding->pipeline == state_.currentPipeline) {
      activeTopologyChanged = true;
    }
    pendingAutomationDeltaValues_[drain.slot] = drain.value.normalized;
    pendingAutomationDeltaIdentities_[drain.slot] = target->identity;
    pendingAutomationDeltaDirty_.set(drain.slot);
    applies.push_back({*binding, drain.value.normalized});
  }
  automationDeltaPending_.store(pendingAutomationDeltaDirty_.any(),
                                std::memory_order_release);
  deltaLock.unlock();
  resources.unlock();

  for (const auto &apply : applies) {
    (void)applyAutomationValue(state_, apply.binding, apply.normalized);
  }
  stateLock.unlock();

  // Adding or removing a node is a topology change, so it takes the established
  // non-real-time descriptor path and may move the reported latency. The
  // descriptor is encoded from the pipeline the rewrite above produced.
  if (!activeTopologyChanged || !processingReady_.load(std::memory_order_seq_cst)) {
    return;
  }
#if defined(EFFETUNE_PROCESSOR_TEST_HOOKS)
  if (pauseAutomationDrainBeforeDescriptorForTesting_.load(
          std::memory_order_acquire)) {
    automationDrainPausedBeforeDescriptorForTesting_.store(
        true, std::memory_order_release);
    while (pauseAutomationDrainBeforeDescriptorForTesting_.load(
        std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    automationDrainPausedBeforeDescriptorForTesting_.store(
        false, std::memory_order_release);
  }
#endif
  std::scoped_lock descriptorResources(processingResourcesMutex_);
  // setState() can publish a replacement after the first under-lock recheck
  // and while the unbounded document rewrite runs without this mutex. Do not
  // encode that replacement into a descriptor for the old runtime generation.
  if (stateReplacementPending_.load(std::memory_order_acquire)) {
    return;
  }
  std::scoped_lock descriptorState(stateMutex_);
  if (queueDescriptorUpdateLocked(state_.currentPipeline == 'B' ? state_.pipelineB
                                                                : state_.pipelineA)) {
    armLatencyNotification();
  }
}

void EffeTuneProcessor::appendAutomationDeltas(choc::value::Value &result) {
  auto deltas = choc::value::createEmptyArray();
  // The editor polls this at frame rate. It reads the identities the drain
  // snapshotted, so it never touches processingResourcesMutex_ and cannot
  // contend with the control work that rebuilds the registry.
  if (!automationDeltaPending_.load(std::memory_order_acquire)) {
    result.addMember("automationDeltas", std::move(deltas));
    return;
  }
  std::scoped_lock deltaLock(automationDeltaMutex_);
  for (std::uint32_t slot = 0; slot < kAutomationSlotCount; ++slot) {
    if (!pendingAutomationDeltaDirty_.test(slot)) {
      continue;
    }
    const auto &identity = pendingAutomationDeltaIdentities_[slot];
    auto encoded = choc::value::createObject({});
    encoded.addMember("pipeline", std::string(1, identity.pipeline));
    encoded.addMember("pluginId", static_cast<std::int64_t>(identity.pluginId));
    encoded.addMember("pluginType", identity.pluginType);
    encoded.addMember("parameterKey", identity.parameterKey);
    encoded.addMember("elementIndex",
                      static_cast<std::int64_t>(identity.elementIndex));
    encoded.addMember("normalized", pendingAutomationDeltaValues_[slot]);
    deltas.addArrayElement(std::move(encoded));
    pendingAutomationDeltaDirty_.reset(slot);
  }
  automationDeltaPending_.store(pendingAutomationDeltaDirty_.any(),
                                std::memory_order_release);
  result.addMember("automationDeltas", std::move(deltas));
}

void EffeTuneProcessor::appendActiveAutomationSnapshot(choc::value::Value &result) {
  drainAutomationValues();
  auto deltas = choc::value::createEmptyArray();
  std::scoped_lock resources(processingResourcesMutex_);
  for (const auto slot : automationBindings_.activeSlots()) {
    const auto *target = automationBindings_.slot(slot);
    if (target == nullptr) {
      continue;
    }
    auto encoded = choc::value::createObject({});
    encoded.addMember("pipeline", std::string(1, target->identity.pipeline));
    encoded.addMember("pluginId", static_cast<std::int64_t>(target->identity.pluginId));
    encoded.addMember("pluginType", target->identity.pluginType);
    encoded.addMember("parameterKey", target->identity.parameterKey);
    encoded.addMember("elementIndex",
                      static_cast<std::int64_t>(target->identity.elementIndex));
    encoded.addMember("normalized", target->currentNormalized);
    deltas.addArrayElement(std::move(encoded));
  }
  {
    std::scoped_lock deltaLock(automationDeltaMutex_);
    pendingAutomationDeltaDirty_.reset();
  }
  automationDeltaPending_.store(false, std::memory_order_release);
  result.addMember("automationDeltas", std::move(deltas));
}

void EffeTuneProcessor::appendExecutionStates(choc::value::Value &result) {
  auto states = choc::value::createEmptyArray();
  std::scoped_lock runtimeState(processingResourcesMutex_, stateMutex_);
  // Host-context publication also owns processingResourcesMutex_, so reading
  // after taking it keeps the reason paired with the admission flags below.
  const auto context = readHostContext();
  const auto &pipeline = state_.currentPipeline == 'B' ? state_.pipelineB
                                                       : state_.pipelineA;
  for (const auto &logical : pipeline.plugins) {
    if (isSectionPlugin(logical)) {
      continue;
    }

    const auto runtime = std::find_if(
        runtimePlugins_.begin(), runtimePlugins_.end(),
        [&logical](const RuntimePlugin &candidate) {
          return candidate.logicalId == logical.id;
        });
    auto pluginType = runtime != runtimePlugins_.end() ? runtime->type
                                                       : std::string{};
    if (pluginType.empty()) {
      try {
        const auto extra = choc::json::parse(logical.extraJson);
        pluginType = extra["type"].getWithDefault<std::string>({});
      } catch (const choc::json::ParseError &) {
        // StateCodec owns this JSON. A malformed legacy extra has no stable
        // identity to route back to a WebView plug-in, so omit it.
      }
    }
    if (pluginType.empty()) {
      continue;
    }

    auto encoded = choc::value::createObject({});
    encoded.addMember("pluginId", static_cast<std::int64_t>(logical.id));
    encoded.addMember("pluginType", pluginType);
    if (runtime == runtimePlugins_.end()) {
      encoded.addMember("state", "bypassed");
      encoded.addMember("reason", "wasmUnavailable");
    } else if (!runtime->contextuallyBypassed) {
      encoded.addMember("state", "active");
    } else {
      encoded.addMember("state", "bypassed");
      const auto support = executionContextSupport(
          runtime->executionCapabilities, logical.channel,
          context.engineSampleRate, context.channels);
      encoded.addMember(
          "reason",
          support == ExecutionContextSupport::unsupportedSampleRate
              ? "unsupportedSampleRate"
              : "unsupportedChannelMode");
    }
    states.addArrayElement(std::move(encoded));
  }
  result.addMember("executionStates", std::move(states));
}

void EffeTuneProcessor::appendDeferredDiagnostics(choc::value::Value &result) {
  auto diagnostics = choc::value::createEmptyArray();
  const auto transactionSequence =
      processTransactionFailureSequence_.load(std::memory_order_acquire);
  const auto transactionPending =
      transactionSequence > drainedProcessTransactionFailureSequence_;
  if (transactionPending) {
    drainedProcessTransactionFailureSequence_ = transactionSequence;
    auto diagnostic = choc::value::createObject({});
    diagnostic.addMember("code", "audio-processing-failure");
    const char *message =
        "Audio processing was temporarily unavailable, so dry audio was used.";
    const auto transactionError =
        lastProcessTransactionError_.load(std::memory_order_acquire);
    switch (transactionError) {
    case ProcessTransactionError::processingNotReady:
      message = "The DSP was not ready; dry audio was used. Reopen the project if this persists.";
      break;
    case ProcessTransactionError::invalidBuffer:
      message = "The host supplied an unsupported audio buffer; dry audio was used. Check the "
                "plug-in bus and block-size settings.";
      break;
    case ProcessTransactionError::dryDelayUnavailable:
    case ProcessTransactionError::upsampleRejected:
    case ProcessTransactionError::downsampleRejected:
      message = "Audio conversion could not complete; dry audio was used. Reopen the project if "
                "this persists.";
      break;
    case ProcessTransactionError::engineHostRejected:
    case ProcessTransactionError::none:
      const auto engineFailure = engine_.processFailureDiagnostic();
      switch (engineFailure.error) {
      case EngineHost::ProcessError::notPrepared:
        message =
            "The DSP was not ready; dry audio was used. Reopen the project if this persists.";
        break;
      case EngineHost::ProcessError::commandRejected:
      case EngineHost::ProcessError::parameterTargetUnavailable:
      case EngineHost::ProcessError::parameterStageRejected:
        message = "A DSP update was rejected; dry audio was used. Retry the last edit.";
        break;
      case EngineHost::ProcessError::invalidProcessChunk:
      case EngineHost::ProcessError::engineProcessRejected:
        message = "The DSP could not process an audio block; dry audio was used. "
                  "Reduce system load or reopen the project if this continues.";
        break;
      case EngineHost::ProcessError::none:
        break;
      }
      break;
    }
    diagnostic.addMember("message", message);
    diagnostics.addArrayElement(std::move(diagnostic));
  }
  if (pipelinePlanRefreshWarningPending_.exchange(false, std::memory_order_acq_rel)) {
    auto diagnostic = choc::value::createObject({});
    diagnostic.addMember("code", "latency-plan-refresh-failed");
    // Audio keeps running on the plan the engine already holds, so only the
    // compensation update is outstanding here.
    diagnostic.addMember(
        "message",
        "DSP latency compensation could not be refreshed. Audio keeps processing and "
        "the update retries automatically.");
    diagnostics.addArrayElement(std::move(diagnostic));
  }
  if (automationCapacityWarningPending_.exchange(false, std::memory_order_acq_rel)) {
    auto diagnostic = choc::value::createObject({});
    diagnostic.addMember("code", "automation-capacity-exhausted");
    diagnostic.addMember(
        "message",
        "No automation slots remain. Existing automation lanes are unchanged; remove unused "
        "plug-ins or start a new project before creating more lanes.");
    diagnostics.addArrayElement(std::move(diagnostic));
  }
  if (automationWriteGateWarningPending_.exchange(false, std::memory_order_acq_rel)) {
    auto diagnostic = choc::value::createObject({});
    diagnostic.addMember("code", "automation-write-required");
    diagnostic.addMember(
        "message",
        "The control was changed but no automation lane was created. Switch the track to "
        "automation write mode, then adjust the control again.");
    diagnostics.addArrayElement(std::move(diagnostic));
  }
  result.addMember("hostAutomationState",
                   static_cast<std::int64_t>(
                       hostAutomationState_.load(std::memory_order_acquire)));
  result.addMember("diagnostics", std::move(diagnostics));
}

void EffeTuneProcessor::recordProcessTransactionFailure(
    const ProcessTransactionError error) noexcept {
  lastProcessTransactionError_.store(error, std::memory_order_release);
  auto expected = false;
  if (processTransactionFailureBurstActive_.compare_exchange_strong(
          expected, true, std::memory_order_acq_rel, std::memory_order_relaxed)) {
    processTransactionFailureSequence_.fetch_add(1, std::memory_order_release);
  }
}

void EffeTuneProcessor::recordPipelinePlanRefreshOutcome(
    const bool succeeded) noexcept {
  if (succeeded) {
    pipelinePlanRefreshWarningIssued_.store(false, std::memory_order_release);
    return;
  }
  if (!pipelinePlanRefreshWarningIssued_.exchange(true, std::memory_order_acq_rel)) {
    pipelinePlanRefreshWarningPending_.store(true, std::memory_order_release);
  }
}

bool EffeTuneProcessor::performHostEditTransaction(
    const ParamID parameterId, const double normalized, const bool beginGesture,
    const bool endGesture) noexcept {
  const auto tracing = trace::enabled();
  if (!std::isfinite(normalized) || normalized < 0.0 || normalized > 1.0) {
    if (tracing) {
      trace::txnBegin(traceInstance_, parameterId, normalized, 0.0, beginGesture,
                      endGesture);
      trace::txnEnd(traceInstance_, parameterId, normalized, 0.0, trace::kFlagRejected);
    }
    return false;
  }
  const auto gestureIndex = hostGestureIndex(parameterId);
  const auto previous = SingleComponentEffect::getParamNormalized(parameterId);
  if (tracing) {
    trace::txnBegin(traceInstance_, parameterId, normalized, previous, beginGesture,
                    endGesture);
  }
  // Records which branches this transaction took, and is read by nothing else.
  std::uint16_t traceFlags = 0;
  const auto accepted = [](const tresult result) noexcept {
    return result == kResultOk || result == kResultTrue;
  };
  // Whether this call is the one that opened the touch, which is the only call
  // that may start a hold.
  auto openedHere = false;
  // Opening the touch is published before the host is told, so a block that
  // observes the flag can never be ingesting host input for a parameter the
  // host already believes is being held.
  if (beginGesture && gestureIndex != kNoHostGestureIndex &&
      !hostGestureOpen_[gestureIndex].load(std::memory_order_seq_cst)) {
    if (!openHostGesture(gestureIndex, parameterId, previous)) {
      if (tracing) {
        trace::txnEnd(traceInstance_, parameterId, normalized, previous, traceFlags);
      }
      return false;
    }
    traceFlags |= trace::kFlagOpenedHere;
    openedHere = true;
  }
  // The base class, deliberately, and not our own override. This transaction is
  // the host-facing half of an edit and nothing else: the caller adopts the
  // value natively the moment the transaction returns, and every caller does.
  // Routing these two writes through the override would adopt on its behalf,
  // which is harmless where they agree -- the forward write below carries the
  // very value the caller is about to adopt -- but not on the rollback, which
  // carries `previous`. That rollback exists only to leave the host parameter
  // where the host left it after refusing the edit; adopting it would push the
  // stale value into the scheduler and the state document, where it would be
  // visible to any block that ran before the caller's own adoption landed.
  auto success =
      accepted(SingleComponentEffect::setParamNormalized(parameterId, normalized));
  if (success) {
    // Every value change is reported, in the order the user made it, with
    // nothing held back and nothing dropped. That is the whole of what
    // IEditController promises a host for a drag: beginEdit, a performEdit per
    // change, endEdit (ivsteditcontroller.h). A value delayed here is a value
    // reordered behind the ones that follow it, and one withheld is a stretch
    // of the gesture the host never sees.
    //
    // The single exception is the sub-block window that opens the touch. Until
    // one process() call has separated it from the beginEdit, the value would
    // land on the host's own punch-in position and be back-extrapolated over
    // everything before the drag, so it waits there and nowhere else. Every
    // value after that boundary is reported in the call that produced it.
    const auto withheld =
        gestureIndex != kNoHostGestureIndex &&
        hostGestureOpen_[gestureIndex].load(std::memory_order_seq_cst) &&
        holdHostEditValue(gestureIndex, normalized, openedHere);
    if (!withheld) {
      const auto performResult = performEdit(parameterId, normalized);
      if (tracing) {
        trace::hostEdit(traceInstance_, trace::Event::hostPerformEdit, parameterId,
                        normalized, static_cast<std::int32_t>(performResult));
      }
      success = accepted(performResult);
    }
  }
  // A value the host refused still leaves the touch to be ended, so the close
  // is decided by the gesture and never by the outcome above. A close that
  // finds nothing open -- a gesture the host declined to begin, or one another
  // path already ended -- has nothing to report either way.
  if (endGesture && gestureIndex != kNoHostGestureIndex &&
      hostGestureOpen_[gestureIndex].load(std::memory_order_seq_cst)) {
    success = closeHostGesture(gestureIndex) && success;
    traceFlags |= trace::kFlagClosedHere;
  }
  if (!success) {
    (void)SingleComponentEffect::setParamNormalized(parameterId, previous);
  }
  if (tracing) {
    if (success) {
      traceFlags |= trace::kFlagSuccess;
    }
    trace::txnEnd(traceInstance_, parameterId, normalized, previous, traceFlags);
  }
  return success;
}

bool EffeTuneProcessor::openHostGesture(const std::size_t index,
                                        const ParamID parameterId,
                                        const double previous) noexcept {
  if (index >= kHostGestureCount) {
    return false;
  }
  if (hostGestureOpen_[index].load(std::memory_order_seq_cst)) {
    return true;
  }

  hostGestureOpen_[index].store(true, std::memory_order_seq_cst);
  const auto tracing = trace::enabled();
  if (tracing) {
    trace::touchOpened(traceInstance_, parameterId, previous);
  }
  const auto beginResult = beginEdit(parameterId);
  if (tracing) {
    trace::hostEdit(traceInstance_, trace::Event::hostBeginEdit, parameterId,
                    previous, static_cast<std::int32_t>(beginResult));
  }
  const auto accepted = beginResult == kResultOk || beginResult == kResultTrue;
  if (!accepted) {
    hostGestureOpen_[index].store(false, std::memory_order_seq_cst);
    if (tracing) {
      trace::touchClosed(traceInstance_, parameterId, false);
    }
    return false;
  }

  // State the value the parameter held when the hand arrived. This is the same
  // punch-in anchor used by an ordinary value-led gesture, but pointerdown can
  // now place it before a click-activated control reports its changed value.
  const auto anchorResult = performEdit(parameterId, previous);
  if (tracing) {
    trace::hostEdit(traceInstance_, trace::Event::hostPerformEdit, parameterId,
                    previous, static_cast<std::int32_t>(anchorResult));
  }
  return true;
}

void EffeTuneProcessor::reportHostEditToOpenGesture(
    const ParamID parameterId, const double normalized,
    const bool endGesture) noexcept {
  if (!std::isfinite(normalized) || normalized < 0.0 || normalized > 1.0) {
    return;
  }
  const auto gestureIndex = hostGestureIndex(parameterId);
  if (gestureIndex == kNoHostGestureIndex ||
      !hostGestureOpen_[gestureIndex].load(std::memory_order_seq_cst)) {
    return;
  }
  // Preserve the ordinary sub-block hold rule for a touch that genuinely
  // predates the restore, but deliberately skip setParamNormalized(): this
  // stale value belongs only in the host's old touch, never in the restored
  // Parameter, registry or scheduler.
  if (!holdHostEditValue(gestureIndex, normalized, /*atOpen=*/false)) {
    const auto result = performEdit(parameterId, normalized);
    if (trace::enabled()) {
      trace::hostEdit(traceInstance_, trace::Event::hostPerformEdit, parameterId,
                      normalized, static_cast<std::int32_t>(result));
    }
  }
  if (endGesture &&
      hostGestureOpen_[gestureIndex].load(std::memory_order_seq_cst)) {
    (void)closeHostGesture(gestureIndex);
  }
}

bool EffeTuneProcessor::holdHostEditValue(const std::size_t index,
                                          const double normalized,
                                          const bool atOpen) noexcept {
  if (index >= kHostGestureCount) {
    return false;
  }
  if (!atOpen) {
    if (!hostGestureHoldPending_[index].load(std::memory_order_acquire)) {
      // Nothing is held any more, so there is nothing to join and the caller
      // reports the value itself.
      return false;
    }
    if (heldHostEditBoundaryCrossed(index)) {
      // Observation point one, and the path a live drag takes: the boundary the
      // held value was waiting for has passed, and a newer value of the same
      // gesture has arrived to supersede it. Dropping the older one and letting
      // the caller report this one is the whole of the release here.
      //
      // The older value is not replayed first, and cannot usefully be. performEdit
      // carries no timestamp (ivsteditcontroller.h:226-230) -- the host stamps it
      // with the playback position at the moment the call arrives -- so a value
      // held back has already lost its own position for good. The only position
      // it can still be given is this call's, which is where the newer value is
      // about to land and overwrite it. And its own position is the beginEdit
      // position, which is exactly where it must never be written: that is the
      // defect this whole path exists to remove. A moving finger therefore never
      // waits for anything but the boundary itself.
      discardHeldHostEdit(index);
      return false;
    }
  }
  hostGestureHeldValue_[index].store(normalized, std::memory_order_release);
  std::uint32_t coalesced = 1u;
  if (atOpen) {
    hostGestureHeldCount_[index].store(1u, std::memory_order_release);
  } else {
    coalesced =
        hostGestureHeldCount_[index].fetch_add(1u, std::memory_order_acq_rel) + 1u;
  }
  // The epoch this value is measured against. Taken after the value is stored
  // and before the claim, so an observer that sees the claim sees both. Every
  // value that reaches here is inside the same un-crossed window as the one it
  // replaces, so re-recording it cannot move the deadline forward.
  hostGestureHoldEpoch_[index].store(processBlockEpoch_.load(std::memory_order_acquire),
                                     std::memory_order_release);
  if (!hostGestureHoldPending_[index].exchange(true, std::memory_order_acq_rel)) {
    hostGestureHoldCount_.fetch_add(1u, std::memory_order_acq_rel);
  }
  if (trace::enabled()) {
    trace::openHeld(traceInstance_, hostGestureParameterId(index), normalized,
                    coalesced, atOpen);
  }
  return true;
}

bool EffeTuneProcessor::heldHostEditBoundaryCrossed(
    const std::size_t index) const noexcept {
  if (index >= kHostGestureCount) {
    return false;
  }
  // The audio thread's whole contribution: it advances this counter at the end
  // of every block and does nothing else. A value differing from the one
  // recorded at the hold means real audio time has separated the withheld value
  // from the beginEdit that preceded it, which is the only thing that can --
  // performEdit carries no timestamp of its own (ivsteditcontroller.h:226-230),
  // so the host stamps it with the playback position at the moment the call
  // arrives.
  return hostGestureHoldEpoch_[index].load(std::memory_order_acquire) !=
         processBlockEpoch_.load(std::memory_order_acquire);
}

void EffeTuneProcessor::discardHeldHostEdit(const std::size_t index) noexcept {
  if (index >= kHostGestureCount ||
      !hostGestureHoldPending_[index].exchange(false, std::memory_order_acq_rel)) {
    return;
  }
  hostGestureHoldCount_.fetch_sub(1u, std::memory_order_acq_rel);
  hostGestureHeldCount_[index].store(0u, std::memory_order_release);
}

bool EffeTuneProcessor::releaseHeldHostEdit(const std::size_t index,
                                            const bool blockBoundary) noexcept {
  if (index >= kHostGestureCount ||
      !hostGestureHoldPending_[index].exchange(false, std::memory_order_acq_rel)) {
    return false;
  }
  hostGestureHoldCount_.fetch_sub(1u, std::memory_order_acq_rel);
  const auto normalized = hostGestureHeldValue_[index].load(std::memory_order_acquire);
  const auto coalesced = hostGestureHeldCount_[index].exchange(0u, std::memory_order_acq_rel);
  const auto parameterId = hostGestureParameterId(index);
  const auto result = performEdit(parameterId, normalized);
  if (trace::enabled()) {
    trace::openReleased(traceInstance_, parameterId, normalized, coalesced,
                        blockBoundary, static_cast<std::int32_t>(result));
    trace::hostEdit(traceInstance_, trace::Event::hostPerformEdit, parameterId,
                    normalized, static_cast<std::int32_t>(result));
  }
  return result == kResultOk || result == kResultTrue;
}

void EffeTuneProcessor::serviceHeldHostEdits() noexcept {
  // Observation point two, on the ~50 ms control-service tick. Be precise about
  // what this is: the timer is a *carrier*, not the trigger. It calls into the
  // host on the thread VST3 allows an IComponentHandler call from, and the only
  // thing it decides is whether the epoch has already moved. The trigger is and
  // remains the process() boundary -- releasing on elapsed time is what the
  // banned earlier design did, and it cost a measured 57.6 ms and ten values of
  // real finger motion. A tick that arrives before the boundary releases
  // nothing, however many of them arrive.
  //
  // It covers exactly one case: a finger that stopped moving mid-drag, whose
  // value observation point one will never come back for. Without it that value
  // would sit until the gesture closed and land at the release position instead
  // of where the user made it.
  //
  // The whole cost on a tick with no hand on a control: one load of a counter
  // that is zero for the entire session but the sub-block window after a touch
  // opens.
  if (hostGestureHoldCount_.load(std::memory_order_acquire) == 0u) {
    return;
  }
  for (std::size_t index = 0; index < kHostGestureCount; ++index) {
    if (hostGestureHoldPending_[index].load(std::memory_order_acquire) &&
        heldHostEditBoundaryCrossed(index)) {
      (void)releaseHeldHostEdit(index, /*blockBoundary=*/true);
    }
  }
}

EffeTuneProcessor::HostInputDisposition
EffeTuneProcessor::classifyHostInput(const ParamID parameterId) noexcept {
  const auto index = hostGestureIndex(parameterId);
  if (index == kNoHostGestureIndex) {
    return HostInputDisposition::apply;
  }
  // The hand on the control outranks the lane the host is playing into it, and
  // that is the only thing that does. The moment the touch closes the host owns
  // the slot again, from the very next statement it makes -- no grace, no
  // carried-over claim, no block of lag.
  return hostGestureOpen_[index].load(std::memory_order_seq_cst)
             ? HostInputDisposition::suppress
             : HostInputDisposition::apply;
}

bool EffeTuneProcessor::closeHostGesture(const std::size_t index) noexcept {
  if (index >= kHostGestureCount ||
      !hostGestureOpen_[index].load(std::memory_order_seq_cst)) {
    return false;
  }
  // The open flag drops before the host is told, so the first block after this
  // point already ingests the host's input for the slot again. That is the
  // contract: endEdit hands the parameter straight back.
  if (!hostGestureOpen_[index].exchange(false, std::memory_order_seq_cst)) {
    if (trace::enabled()) {
      trace::touchClosed(traceInstance_, hostGestureParameterId(index),
                         /*accepted=*/false);
    }
    return false;
  }
  // Observation point three, and the last one a gesture has. A gesture may not
  // carry a value out of itself, so whatever is still held reaches the host
  // here, inside the touch and before the endEdit that closes it -- and it does
  // so whether or not the boundary has been crossed. An editor open with no
  // audio running has no boundaries at all: an offline render, a transport that
  // issues no callbacks, a mouse-down and mouse-up inside one sub-block window.
  // The trace flag records which of the two this was.
  (void)releaseHeldHostEdit(index, heldHostEditBoundaryCrossed(index));
  const auto tracing = trace::enabled();
  const auto result = endEdit(hostGestureParameterId(index));
  const auto acceptedClose = result == kResultOk || result == kResultTrue;
  if (tracing) {
    trace::hostEdit(traceInstance_, trace::Event::hostEndEdit,
                    hostGestureParameterId(index), 0.0,
                    static_cast<std::int32_t>(result));
    trace::touchClosed(traceInstance_, hostGestureParameterId(index), acceptedClose);
  }
  return acceptedClose;
}

void EffeTuneProcessor::closeOpenHostGestures() noexcept {
  for (std::size_t index = 0; index < kHostGestureCount; ++index) {
    (void)closeHostGesture(index);
  }
}

void EffeTuneProcessor::AutomationResourceLock::retireHostGesture(
    const std::size_t index) noexcept {
  // Only a touch that is actually open is worth carrying out of the lock; the
  // close re-reads the flag anyway, so a stale bit costs nothing but a load.
  if (index < kHostGestureCount &&
      processor_.hostGestureOpen_[index].load(std::memory_order_seq_cst)) {
    retiring_.set(index);
  }
}

EffeTuneProcessor::AutomationResourceLock::~AutomationResourceLock() {
  lock_.unlock();
  if (retiring_.none()) {
    return;
  }
  for (std::size_t index = 0; index < kHostGestureCount; ++index) {
    if (retiring_[index]) {
      (void)processor_.closeHostGesture(index);
    }
  }
}

std::int64_t EffeTuneProcessor::automationBlockStart(const ProcessData &data,
                                                     bool &rebase) noexcept {
  const auto *context = data.processContext;
  const auto continuousValid =
      context != nullptr && (context->state & ProcessContext::kContTimeValid) != 0;
  const auto start = continuousValid ? context->continousTimeSamples : processedHostFrames_;
  rebase = start != automationScheduler_.renderedClock();
  if (context != nullptr) {
    const auto playing = (context->state & ProcessContext::kPlaying) != 0;
    const auto cycle = (context->state & ProcessContext::kCycleActive) != 0;
    if (playing != previousPlaying_ || cycle != previousCycleActive_) {
      rebase = true;
    }
    if (previousProjectTimeValid_ && previousPlaying_ && playing &&
        context->projectTimeSamples !=
            previousProjectTimeSamples_ + static_cast<std::int64_t>(previousBlockFrames_)) {
      rebase = true;
    }
    previousProjectTimeSamples_ = context->projectTimeSamples;
    previousProjectTimeValid_ = true;
    previousPlaying_ = playing;
    previousCycleActive_ = cycle;
    previousBlockFrames_ = data.numSamples > 0 ? static_cast<std::uint32_t>(data.numSamples) : 0u;
  }
  return start;
}

void EffeTuneProcessor::publishHostContext(const double sampleRate, const std::uint32_t channels,
                                           const std::uint32_t oversamplingFactor) noexcept {
  const auto engineSampleRate = sampleRate * oversamplingFactor;
  const auto changed =
      !hostContextPublished_ ||
      publishedHostSampleRate_.load(std::memory_order_relaxed) != sampleRate ||
      publishedEngineSampleRate_.load(std::memory_order_relaxed) != engineSampleRate ||
      publishedChannels_.load(std::memory_order_relaxed) != channels ||
      publishedOversamplingFactor_.load(std::memory_order_relaxed) != oversamplingFactor;
  hostContextPublished_ = true;
  contextSequence_.fetch_add(1, std::memory_order_seq_cst);
  publishedHostSampleRate_.store(sampleRate, std::memory_order_relaxed);
  publishedEngineSampleRate_.store(engineSampleRate, std::memory_order_relaxed);
  publishedChannels_.store(channels, std::memory_order_relaxed);
  publishedOversamplingFactor_.store(oversamplingFactor, std::memory_order_relaxed);
  if (changed) {
    contextGeneration_.fetch_add(1, std::memory_order_relaxed);
  }
  contextSequence_.fetch_add(1, std::memory_order_seq_cst);
}

EffeTuneProcessor::HostContextSnapshot EffeTuneProcessor::readHostContext() const noexcept {
  HostContextSnapshot snapshot;
  for (;;) {
    const auto sequence = contextSequence_.load(std::memory_order_seq_cst);
    if ((sequence & 1u) != 0u) {
      continue;
    }
    snapshot.sampleRate = publishedHostSampleRate_.load(std::memory_order_relaxed);
    snapshot.engineSampleRate = publishedEngineSampleRate_.load(std::memory_order_relaxed);
    snapshot.channels = publishedChannels_.load(std::memory_order_relaxed);
    snapshot.oversamplingFactor = publishedOversamplingFactor_.load(std::memory_order_relaxed);
    snapshot.generation = contextGeneration_.load(std::memory_order_relaxed);
    if (contextSequence_.load(std::memory_order_seq_cst) == sequence) {
      return snapshot;
    }
  }
}

tresult PLUGIN_API EffeTuneProcessor::setupProcessing(ProcessSetup &setup) {
  if (setup.symbolicSampleSize != kSample32 || setup.maxSamplesPerBlock <= 0 ||
      setup.sampleRate <= 0.0) {
    return kResultFalse;
  }
  if (!reconfigureDspPreservingPipeline(
          setup.sampleRate, setup.maxSamplesPerBlock,
          configuredChannels_.load(std::memory_order_acquire))) {
    return kResultFalse;
  }
  return SingleComponentEffect::setupProcessing(setup);
}

tresult PLUGIN_API EffeTuneProcessor::setBusArrangements(SpeakerArrangement *inputs,
                                                         const int32 numInputs,
                                                         SpeakerArrangement *outputs,
                                                         const int32 numOutputs) {
  if (inputs == nullptr || outputs == nullptr || numInputs != 1 || numOutputs != 1 ||
      inputs[0] != outputs[0]) {
    return kResultFalse;
  }
  const auto channelCount = SpeakerArr::getChannelCount(inputs[0]);
  if (channelCount < 1 || channelCount > static_cast<int32>(EngineHost::kMaxChannels)) {
    return kResultFalse;
  }
  auto *inputBus = FCast<AudioBus>(audioInputs.at(0));
  auto *outputBus = FCast<AudioBus>(audioOutputs.at(0));
  if (inputBus == nullptr || outputBus == nullptr) {
    return kResultFalse;
  }
  inputBus->setArrangement(inputs[0]);
  outputBus->setArrangement(outputs[0]);
  inputBus->setName(STR16("Main Input"));
  outputBus->setName(STR16("Main Output"));
  if (maxHostFrames_.load(std::memory_order_acquire) > 0) {
    if (!reconfigureDspPreservingPipeline(
            hostSampleRate_.load(std::memory_order_acquire),
            maxHostFrames_.load(std::memory_order_acquire), channelCount)) {
      return kResultFalse;
    }
  } else {
    configuredChannels_.store(channelCount, std::memory_order_release);
  }
  return kResultTrue;
}

tresult PLUGIN_API EffeTuneProcessor::canProcessSampleSize(const int32 symbolicSampleSize) {
  return symbolicSampleSize == kSample32 ? kResultTrue : kResultFalse;
}

void EffeTuneProcessor::copyDry(const AudioBusBuffers &input, AudioBusBuffers &output,
                                const int32 frames) noexcept {
  if (input.channelBuffers32 == nullptr || output.channelBuffers32 == nullptr || frames <= 0) {
    return;
  }
  const auto channels = std::min(input.numChannels, output.numChannels);
  for (int32 channel = 0; channel < channels; ++channel) {
    if (input.channelBuffers32[channel] != output.channelBuffers32[channel]) {
      std::memcpy(output.channelBuffers32[channel], input.channelBuffers32[channel],
                  static_cast<std::size_t>(frames) * sizeof(float));
    }
  }
  for (int32 channel = channels; channel < output.numChannels; ++channel) {
    std::fill_n(output.channelBuffers32[channel], frames, 0.0f);
  }
  output.silenceFlags = input.silenceFlags;
}

void EffeTuneProcessor::copyDryToScratch(const AudioBusBuffers &input, const int32 frames) noexcept {
  for (int32 channel = 0; channel < input.numChannels; ++channel) {
    std::memcpy(dryTransitionPointers_[static_cast<std::size_t>(channel)],
                input.channelBuffers32[channel], static_cast<std::size_t>(frames) * sizeof(float));
  }
}

void EffeTuneProcessor::restoreDryFromScratch(AudioBusBuffers &output, const int32 frames) noexcept {
  for (int32 channel = 0; channel < output.numChannels; ++channel) {
    std::memcpy(output.channelBuffers32[channel],
                dryTransitionPointers_[static_cast<std::size_t>(channel)],
                static_cast<std::size_t>(frames) * sizeof(float));
  }
  output.silenceFlags = 0;
}

tresult PLUGIN_API EffeTuneProcessor::process(ProcessData &data) {
  // Names this thread for every record emitted below it, including the ones the
  // helpers it calls emit. Two thread-local accesses; see
  // plugin/automation_trace.h.
  const trace::ScopedRole traceRole{trace::Role::audio};
  const auto tracing = trace::enabled();
  // Marks the block as executing for every exit path. Two atomic increments,
  // no allocation and no lock, so a control thread can wait the block out.
  //
  // It is also the whole of what this callback does for the gesture-hold
  // mechanism, and the whole of what it may do: VST3 requires
  // beginEdit/performEdit/endEdit on the UI/controller thread, so process()
  // makes no IComponentHandler call of any kind. It publishes the fact that a
  // boundary occurred and nothing more; the control threads observe this
  // counter and do the reporting themselves.
  //
  // A process() boundary remains the whole of the trigger. No clock, no service
  // tick and no elapsed time releases anything on its own -- releasing on the
  // ~50 ms control tick is exactly what an earlier mechanism did, and it cost
  // 57.6 ms and ten values of real finger motion out of a measured drag.
  struct BlockEpochScope {
    std::atomic<std::uint64_t> &epoch;
    explicit BlockEpochScope(std::atomic<std::uint64_t> &value) noexcept
        : epoch(value) {
      epoch.fetch_add(1, std::memory_order_seq_cst);
    }
    BlockEpochScope(const BlockEpochScope &) = delete;
    BlockEpochScope &operator=(const BlockEpochScope &) = delete;
    ~BlockEpochScope() noexcept { epoch.fetch_add(1, std::memory_order_seq_cst); }
  } const blockEpoch{processBlockEpoch_};
  // Match the upstream AudioWorklet CPU meter: average this instance's whole
  // callback time over approximately one second of rendered audio. The scope
  // owns every return path, performs no allocation or locking, and publishes a
  // fixed-point atomic value for the control side to read.
  struct PipelineCpuMeasurementScope {
    std::chrono::steady_clock::time_point startedAt;
    Steinberg::int32 frames;
    double sampleRate;
    double &audioSeconds;
    std::chrono::steady_clock::duration &elapsed;
    std::atomic<std::uint32_t> &publishedHundredths;

    ~PipelineCpuMeasurementScope() noexcept {
      if (frames <= 0 || !std::isfinite(sampleRate) || sampleRate <= 0.0) {
        return;
      }
      elapsed += std::chrono::steady_clock::now() - startedAt;
      audioSeconds += static_cast<double>(frames) / sampleRate;
      if (audioSeconds < 1.0) {
        return;
      }
      const auto elapsedSeconds = std::chrono::duration<double>(elapsed).count();
      const auto hundredths = std::clamp(
          std::round(elapsedSeconds * 10000.0 / audioSeconds), 0.0,
          static_cast<double>(std::numeric_limits<std::uint32_t>::max()));
      publishedHundredths.store(static_cast<std::uint32_t>(hundredths),
                                std::memory_order_release);
      audioSeconds = 0.0;
      elapsed = {};
    }
  } const pipelineCpuMeasurement{
      std::chrono::steady_clock::now(), data.numSamples,
      hostSampleRate_.load(std::memory_order_acquire), pipelineCpuAudioSeconds_,
      pipelineCpuElapsed_, pipelineCpuAverageHundredths_};
  // A control thread is rebuilding the block timeline this callback would
  // touch before it ever reaches the processing gate. Nothing below is safe to
  // run against that, and nothing is lost either: the scheduler and the output
  // transition are being reset, so this block has no timeline to carry.
  if (controlOwnsAudioTimeline_.load(std::memory_order_seq_cst)) {
    if (data.symbolicSampleSize == kSample32 && data.numSamples > 0 &&
        data.numInputs > 0 && data.numOutputs > 0 && data.inputs != nullptr &&
        data.outputs != nullptr && hasChannelPointers(data.inputs[0]) &&
        hasChannelPointers(data.outputs[0])) {
      copyDry(data.inputs[0], data.outputs[0], data.numSamples);
    }
    return kResultOk;
  }
  // Sequentially consistent on both sides: the control thread stores the claim
  // before loading the block epoch, this thread bumps the epoch before loading
  // the claim, so a block that observes false cannot overlap a consumer. While
  // the claim is held this block keeps processing with the values the engine
  // already holds; only its own parameter staging is skipped, and the next
  // block applies whatever it missed.
  const auto stageParameterImages =
      !controlOwnsRuntimeImage_.load(std::memory_order_seq_cst);
  // One face for the whole block, matching how the scheduler adopts its
  // configuration at beginBlock().
  const auto &automationTable =
      automationApplyTables_[publishedAutomationTable_.load(std::memory_order_seq_cst)];
  bool rebase = false;
  const auto absoluteStart = automationBlockStart(data, rebase);
  if (tracing) {
    const auto *const context = data.processContext;
    const auto contextState =
        context != nullptr ? static_cast<std::uint32_t>(context->state) : 0u;
    std::uint16_t blockFlags =
        static_cast<std::uint16_t>(rebase ? trace::kFlagRebase : 0u);
    if (context == nullptr) {
      blockFlags |= trace::kFlagNoContext;
    } else {
      if ((context->state & ProcessContext::kPlaying) != 0) {
        blockFlags |= trace::kFlagPlaying;
      }
      if ((context->state & ProcessContext::kCycleActive) != 0) {
        blockFlags |= trace::kFlagCycleActive;
      }
      if ((context->state & ProcessContext::kContTimeValid) != 0) {
        blockFlags |= trace::kFlagContTimeValid;
      }
    }
    if (data.numSamples <= 0 || data.numInputs == 0 || data.numOutputs == 0) {
      blockFlags |= trace::kFlagFlushOnly;
    }
    trace::block(traceInstance_,
                 context != nullptr ? context->projectTimeSamples : 0,
                 context != nullptr ? context->continousTimeSamples : 0,
                 contextState, data.numSamples, absoluteStart, blockFlags);
  }
  const auto flushOnly = data.numSamples <= 0 || data.numInputs == 0 || data.numOutputs == 0;
  automationScheduler_.beginBlock(
      {absoluteStart, data.numSamples > 0 ? static_cast<std::uint32_t>(data.numSamples) : 0u,
       rebase, flushOnly});
  ingestControllerWriteHandoffs();
  if (data.inputParameterChanges != nullptr) {
    const auto queueCount = std::clamp<int32>(
        data.inputParameterChanges->getParameterCount(), 0,
        static_cast<int32>(kAutomationSlotCount + 1u));
    for (int32 queueIndex = 0; queueIndex < queueCount; ++queueIndex) {
      auto *queue = data.inputParameterChanges->getParameterData(queueIndex);
      if (queue == nullptr) {
        continue;
      }
      const auto parameterId = queue->getParameterId();
      const auto rawPointCount = std::max<int32>(queue->getPointCount(), 0);
      // The user's hand is on this control. Skipping the whole queue is what
      // gives the hand precedence: the slot then receives no points and no
      // latest input, so finishIntake() publishes nothing for it and the
      // block-start boundary preserves the value the gesture adopted -- exactly
      // the path that already carries the value when the host's Read is off.
      const auto disposition = classifyHostInput(parameterId);
      if (tracing && trace::tracksParameter(parameterId)) {
        // Where the host's curve has arrived by the end of this block, read for
        // the trace alone: every point of a queue is a section of one curve, so
        // the last point is that arrival. One extra getPoint() per traced
        // queue, and none at all when the trace is off.
        auto queueEndValue = 0.0;
        auto hasQueueEndValue = false;
        if (rawPointCount > 0) {
          int32 endOffset = 0;
          ParamValue endValue = 0.0;
          if (queue->getPoint(rawPointCount - 1, endOffset, endValue) == kResultTrue) {
            queueEndValue = endValue;
            hasQueueEndValue = true;
          }
        }
        trace::queue(traceInstance_, parameterId,
                     static_cast<std::uint32_t>(rawPointCount), queueEndValue,
                     hasQueueEndValue, disposition == HostInputDisposition::suppress);
        // Every queue is reported point by point, suppressed ones included: a
        // queue the block throws away is still what the host said, and the
        // ordinary point loop below runs only for the queues that survive.
        const auto tracedPoints =
            std::min<int32>(rawPointCount, trace::kMaxTracedPoints);
        for (int32 tracedIndex = 0; tracedIndex < tracedPoints; ++tracedIndex) {
          int32 tracedOffset = 0;
          ParamValue tracedValue = 0.0;
          if (queue->getPoint(tracedIndex, tracedOffset, tracedValue) == kResultTrue) {
            trace::queuePoint(traceInstance_, parameterId,
                              static_cast<std::uint32_t>(tracedIndex), tracedOffset,
                              tracedValue);
          }
        }
        if (rawPointCount > tracedPoints) {
          trace::queueTruncated(traceInstance_, parameterId,
                                static_cast<std::uint32_t>(rawPointCount),
                                tracedPoints);
        }
      }
      if (disposition == HostInputDisposition::suppress) {
        continue;
      }
      if (rawPointCount > 0) {
        const auto inputIndex = hostGestureIndex(parameterId);
        if (inputIndex != kNoHostGestureIndex) {
          controllerAuthorityGenerations_[inputIndex].fetch_add(
              1u, std::memory_order_acq_rel);
        }
      }
      automationScheduler_.beginQueue(parameterId);
      const auto pointCount = std::min<int32>(
          rawPointCount,
          static_cast<int32>(AutomationScheduler::kMaximumPointsPerQueue));
      for (int32 pointIndex = 0; pointIndex < pointCount; ++pointIndex) {
        int32 offset = 0;
        ParamValue value = 0.0;
        if (queue->getPoint(pointIndex, offset, value) == kResultTrue) {
          automationScheduler_.pushPoint(offset, value);
        }
      }
      if (rawPointCount > pointCount) {
        int32 offset = 0;
        ParamValue value = 0.0;
        if (queue->getPoint(rawPointCount - 1, offset, value) == kResultTrue) {
          automationScheduler_.pushPoint(offset, value);
        }
      }
      automationScheduler_.endQueue();
    }
  }
  automationScheduler_.finishIntake();
  if (tracing) {
    // What the DSP will actually run this block with, read after intake has
    // published it and before any slice is rendered.
    for (const auto slot : automationScheduler_.activeSlots()) {
      const auto slotIndex = static_cast<std::uint32_t>(slot);
      const auto slotParameterId = automationParameterId(slotIndex);
      if (!trace::tracksParameter(slotParameterId)) {
        continue;
      }
      trace::dspValue(traceInstance_, slotParameterId, slotIndex,
                      automationScheduler_.currentNormalized(slotIndex),
                      automationScheduler_.publishedGeneration(slotIndex));
    }
  }
  bypass_.store(automationScheduler_.currentBypass(), std::memory_order_release);
  if (flushOnly) {
    return kResultOk;
  }
  // The counter answers exactly one question: is the audio callback still
  // rendering, and therefore still staging the parameter images itself? The
  // flushOnly return above is what makes that answer right -- it leaves before
  // the staging below, so a host that has gone quiet and only flushes stalls the
  // counter and reads as idle. The transport state is deliberately not consulted:
  // a stopped transport with the audio device still turning for live input
  // monitoring keeps sending ordinary blocks, and those blocks stage the images
  // themselves, so nothing on the control side needs the engine taken away from
  // them.
  renderedBlockCount_.fetch_add(1, std::memory_order_release);
  processedHostFrames_ += data.numSamples;
  if (data.inputs == nullptr || data.outputs == nullptr) {
    recordProcessTransactionFailure(ProcessTransactionError::invalidBuffer);
    completeAutomationBlock(false);
    return kResultOk;
  }
  auto &input = data.inputs[0];
  auto &output = data.outputs[0];
  const auto maxHostFrames = maxHostFrames_.load(std::memory_order_acquire);
  const auto configuredChannels = configuredChannels_.load(std::memory_order_acquire);
  const auto hostSampleRate = hostSampleRate_.load(std::memory_order_acquire);
  const auto inputPointersValid = hasChannelPointers(input);
  const auto outputPointersValid = hasChannelPointers(output);
  if (data.symbolicSampleSize != kSample32 || data.numSamples > maxHostFrames ||
      input.numChannels != configuredChannels ||
      output.numChannels != configuredChannels || !inputPointersValid ||
      !outputPointersValid) {
    if (data.symbolicSampleSize == kSample32 && inputPointersValid && outputPointersValid) {
      copyDry(input, output, data.numSamples);
    }
    recordProcessTransactionFailure(ProcessTransactionError::invalidBuffer);
    completeAutomationBlock(false);
    return kResultOk;
  }

  auto finish = [&](const bool processed, const float *const *dry) noexcept {
    outputTransition_.apply(output.channelBuffers32, dry,
                            static_cast<std::uint32_t>(output.numChannels),
                            static_cast<std::uint32_t>(data.numSamples), hostSampleRate, processed);
    output.silenceFlags = 0;
    for (int32 channel = 0; channel < output.numChannels; ++channel) {
      const auto *samples = output.channelBuffers32[channel];
      const auto silent = std::all_of(samples, samples + data.numSamples,
                                      [](const float sample) { return sample == 0.0f; });
      if (silent) {
        output.silenceFlags |= static_cast<uint64>(1) << channel;
      }
    }
  };

  std::array<const float *, EngineHost::kMaxChannels> dryPointers{};
  const auto finishDry = [&]() noexcept {
    copyDry(input, output, data.numSamples);
    for (int32 channel = 0; channel < output.numChannels; ++channel) {
      dryPointers[static_cast<std::size_t>(channel)] = output.channelBuffers32[channel];
    }
    finish(false, dryPointers.data());
  };

  // A control thread holding the engine is not an unprepared DSP. The user
  // asked for the operation that opened the window, so the block it costs is
  // not a failure to report; only a DSP that cannot produce a processed signal
  // at all earns a diagnostic. The claim is read on both sides of the gate
  // because the window restores the gate before it drops the claim, and takes
  // the claim before it closes the gate: reading only one side would report the
  // instant on the other. A window cannot both open and close between the two
  // reads either, because its constructor waits this block out before it
  // returns, so a claim missing from both reads never existed while this block
  // ran.
  const auto claimedBeforeGate =
      controlEngineClaims_.load(std::memory_order_seq_cst) != 0u;
  if (!processingReady_.load(std::memory_order_seq_cst)) {
    finishDry();
    if (!claimedBeforeGate &&
        controlEngineClaims_.load(std::memory_order_seq_cst) == 0u) {
      recordProcessTransactionFailure(ProcessTransactionError::processingNotReady);
    }
    completeAutomationBlock(false);
    return kResultOk;
  }

  // No lock is taken from here on. Everything below is either owned by this
  // thread, published through a lock-free mechanism, or written by a control
  // thread only inside a not-ready window it proved quiet first, so concurrent
  // control work can never cost this block.
  //
  // A queued descriptor or a stale pipeline plan is serviced by the
  // non-real-time control thread. The engine keeps its previously configured
  // topology until then, so audio continues to be processed instead of falling
  // back to the input signal.
  copyDryToScratch(input, data.numSamples);
  for (int32 channel = 0; channel < output.numChannels; ++channel) {
    dryPointers[static_cast<std::size_t>(channel)] =
        dryTransitionPointers_[static_cast<std::size_t>(channel)];
  }
  const float *const *delayedDry = nullptr;
  const auto oversamplingFactor = activeOversamplingFactor_.load(std::memory_order_acquire);

  const auto *upsampled = oversampler_.upsample(
      const_cast<const float *const *>(input.channelBuffers32),
      static_cast<std::uint32_t>(data.numSamples));
  if (upsampled == nullptr) {
    finishDry();
    recordProcessTransactionFailure(ProcessTransactionError::upsampleRejected);
    completeAutomationBlock(false);
    return kResultOk;
  }
  const auto engineFrames =
      static_cast<std::uint32_t>(data.numSamples) * oversamplingFactor;
  if (stageParameterImages) {
    adoptPendingParameterImagesLocked(true);
    for (const auto slot : automationScheduler_.activeSlots()) {
      if (slot >= kAutomationSlotCount) {
        continue;
      }
      // Slots without a packed parameter on the active pipeline carry no runtime
      // index: node-enable targets reach the DSP as control-thread descriptor
      // updates instead.
      const auto &entry = automationTable[slot];
      const auto runtimeIndex = entry.runtimeIndex;
      if (runtimeIndex == kNoAutomationRuntimeIndex ||
          runtimeIndex >= runtimePlugins_.size()) {
        continue;
      }
      auto &runtime = runtimePlugins_[runtimeIndex];
      const auto packed = denormalizeAutomationPackedValue(
          entry.denormalization, automationScheduler_.currentNormalized(slot));
      if (packed.has_value() && entry.packedOffset < runtime.packedParameters.size() &&
          runtime.packedParameters[entry.packedOffset] != *packed) {
        runtime.packedParameters[entry.packedOffset] = *packed;
        runtimeParameterDirty_[runtimeIndex] = true;
      }
    }
  }

  EngineHost::ProcessBatch batch;
  auto processed = engine_.beginProcessBatch(batch, nullptr, nullptr);
  auto refreshLatencyAtBlockEnd = false;
  std::array<EngineHost::ResolvedParameterTarget, kMaxPluginInstances> parameterTargets{};
  for (std::size_t runtimeIndex = 0;
       processed && runtimeIndex < runtimePlugins_.size(); ++runtimeIndex) {
    processed = batch.resolveParameterTarget(runtimePlugins_[runtimeIndex].logicalId,
                                             runtimePlugins_[runtimeIndex].paramsHash,
                                             parameterTargets[runtimeIndex]);
  }
  std::fill_n(hostBypassMask_.begin(), static_cast<std::size_t>(data.numSamples),
              automationScheduler_.currentBypass() ? 1u : 0u);
  std::array<const float *, EngineHost::kMaxChannels> sliceInput{};
  std::array<float *, EngineHost::kMaxChannels> sliceOutput{};
  AutomationSlice slice;
  while (processed && automationScheduler_.nextSlice(slice)) {
    auto sliceBypass = slice.hostOffset == 0
                           ? automationScheduler_.currentBypass()
                           : hostBypassMask_[slice.hostOffset - 1u] != 0;
    for (const auto &change : slice.changes) {
      if (change.bypass) {
        sliceBypass = change.normalized >= 0.5;
        continue;
      }
      // Master bypass above is engine state, not part of the runtime image, so
      // it still follows the slice while the control service owns the image.
      if (!stageParameterImages) {
        continue;
      }
      // Node-enable slots are skipped: this loop only writes packed parameters.
      const auto &entry = automationTable[change.slot];
      const auto runtimeIndex = entry.runtimeIndex;
      if (runtimeIndex == kNoAutomationRuntimeIndex ||
          runtimeIndex >= runtimePlugins_.size()) {
        continue;
      }
      auto &runtime = runtimePlugins_[runtimeIndex];
      const auto packed =
          denormalizeAutomationPackedValue(entry.denormalization, change.normalized);
      if (!packed.has_value() || entry.packedOffset >= runtime.packedParameters.size()) {
        continue;
      }
      runtime.packedParameters[entry.packedOffset] = *packed;
      runtimeParameterDirty_[runtimeIndex] = true;
    }
    std::fill_n(hostBypassMask_.begin() + slice.hostOffset, slice.hostFrames,
                sliceBypass ? 1u : 0u);
    // Stage before validating the prepared plan, so a newer image cannot
    // accidentally commit stale compensation.
    // The dirty flags are left standing while the control service owns the
    // image, so whatever is pending reaches the engine from the next block.
    for (std::size_t runtimeIndex = 0;
         stageParameterImages && runtimeIndex < runtimePlugins_.size(); ++runtimeIndex) {
      if (!runtimeParameterDirty_[runtimeIndex]) {
        continue;
      }
      const auto &runtime = runtimePlugins_[runtimeIndex];
      if (!batch.stageParameters(parameterTargets[runtimeIndex], runtime.packedParameters,
                                 runtime.parameterBytes)) {
        processed = false;
        break;
      }
      if (runtimeFullImageDirty_[runtimeIndex]) {
        refreshLatencyAtBlockEnd = true;
        runtimeFullImageDirty_[runtimeIndex] = false;
      }
      runtimeParameterDirty_[runtimeIndex] = false;
    }
    if (!processed || slice.hostFrames == 0) {
      continue;
    }
    if (delayedDry == nullptr) {
      // Commit wet compensation, dry history and the published host latency
      // together, before either path processes this host block. Later slices
      // may stage newer parameters; their plan is captured at block end.
      (void)applyPreparedLatencyUpdate();
      delayedDry = dryDelay_.process(
          const_cast<const float *const *>(input.channelBuffers32),
          static_cast<std::uint32_t>(input.numChannels),
          static_cast<std::uint32_t>(data.numSamples));
      if (delayedDry == nullptr) {
        processed = false;
        break;
      }
    }
    const auto engineOffset = slice.hostOffset * oversamplingFactor;
    const auto sliceEngineFrames = slice.hostFrames * oversamplingFactor;
    for (std::uint32_t channel = 0; channel < static_cast<std::uint32_t>(input.numChannels);
         ++channel) {
      sliceInput[channel] = upsampled[channel] + engineOffset;
      sliceOutput[channel] = engineOutputPointers_[channel] + engineOffset;
    }
    std::uint32_t sliceFramesProcessed = 0;
    processed = blockAdapter_.process(
        sliceInput.data(), sliceOutput.data(), sliceEngineFrames,
        [&](float *const *channels, const std::uint32_t channelCount,
            const std::uint32_t frames) noexcept {
          const auto time = static_cast<double>(absoluteStart + slice.hostOffset) /
                                hostSampleRate +
                            static_cast<double>(sliceFramesProcessed) /
                                (hostSampleRate * oversamplingFactor);
          sliceFramesProcessed += frames;
          return batch.processChunk(channels, channelCount, frames, time, sliceBypass);
        });
  }
  const auto batchFinished = batch.finish(refreshLatencyAtBlockEnd);
  capturePendingLatencyUpdate();
  processed = processed && batchFinished;

  auto failure = ProcessTransactionError::none;
  if (!processed) {
    failure = ProcessTransactionError::engineHostRejected;
  } else if (!oversampler_.downsample(
                 const_cast<const float *const *>(engineOutputPointers_.data()), engineFrames,
                 output.channelBuffers32)) {
    failure = ProcessTransactionError::downsampleRejected;
  }
  if (failure != ProcessTransactionError::none) {
    blockAdapter_.reset();
    oversampler_.reset();
    if (stageParameterImages) {
      // The dirty flags belong to whoever owns the runtime image. While the
      // control service owns it this block staged nothing, so there is nothing
      // to re-stage, and writing the flags here would race the owner.
      std::fill_n(runtimeParameterDirty_.begin(), runtimePlugins_.size(), true);
    }
    restoreDryFromScratch(output, data.numSamples);
    finish(false, dryPointers.data());
    recordProcessTransactionFailure(failure);
    completeAutomationBlock(false);
    return kResultOk;
  }
  for (int32 channel = 0; channel < output.numChannels; ++channel) {
    for (int32 frame = 0; frame < data.numSamples; ++frame) {
      if (hostBypassMask_[static_cast<std::size_t>(frame)] != 0) {
        output.channelBuffers32[channel][frame] = delayedDry[channel][frame];
      }
    }
  }
  finish(true, dryPointers.data());
  completeAutomationBlock(true);
  return kResultOk;
}

bool EffeTuneProcessor::readStream(IBStream *stream, std::string &contents) const {
  if (stream == nullptr) {
    return false;
  }
  contents.clear();
  std::array<char, 4096> buffer{};
  constexpr std::size_t maximumStateBytes = 4u * 1024u * 1024u;
  while (contents.size() < maximumStateBytes) {
    int32 bytesRead = 0;
    const auto result = stream->read(buffer.data(), static_cast<int32>(buffer.size()), &bytesRead);
    if (result != kResultOk && result != kResultTrue) {
      return false;
    }
    if (bytesRead <= 0) {
      break;
    }
    contents.append(buffer.data(), static_cast<std::size_t>(bytesRead));
    if (bytesRead < static_cast<int32>(buffer.size())) {
      break;
    }
  }
  return !contents.empty() && contents.size() < maximumStateBytes;
}

void EffeTuneProcessor::notifyLatencyChange(const uint32 previousLatency) {
  if (previousLatency == latencySamples_.load(std::memory_order_acquire)) {
    return;
  }
  if (auto *handler = getComponentHandler(); handler != nullptr) {
    traceRestartComponent(*handler, RestartFlags::kLatencyChanged, traceInstance_);
    latencyDebounceArmed_.store(false, std::memory_order_release);
    latencyNotificationPending_.store(false, std::memory_order_release);
  }
}

void EffeTuneProcessor::armLatencyNotification() {
  latencyNotificationDeadlineTicks_.store(
      (std::chrono::steady_clock::now() + std::chrono::milliseconds(250))
          .time_since_epoch()
          .count(),
      std::memory_order_release);
  latencyDebounceArmed_.store(true, std::memory_order_release);
}

void EffeTuneProcessor::queueLatencyNotification(const bool restartDebounce) {
  latencyNotificationPending_.store(true, std::memory_order_release);
  if (restartDebounce || !latencyDebounceArmed_.load(std::memory_order_acquire)) {
    armLatencyNotification();
  }
}

std::uint32_t EffeTuneProcessor::processingLatencySamples() const noexcept {
  const auto oversamplingFactor =
      std::max(activeOversamplingFactor_.load(std::memory_order_acquire), 1u);
  return calculateTotalLatency(
      resamplerLatencySamples_.load(std::memory_order_acquire), oversamplingFactor,
      engine_.pipelineLatency());
}

bool EffeTuneProcessor::synchronizeLatencyLocked(bool &latencyChanged) {
  // Every caller owns the engine window and control mutex, so no captured or
  // prepared result can survive a lifecycle/configuration replacement.
  discardPendingLatencyUpdate();
  const auto next = calculateTotalLatency(
      resamplerLatencySamples_.load(std::memory_order_acquire),
      activeOversamplingFactor_.load(std::memory_order_acquire), engine_.pipelineLatency());
  const auto previous = latencySamples_.load(std::memory_order_acquire);
  if (previous != next && !dryDelay_.setDelay(next)) {
    // Growing the line swaps the history buffer and rewrites the read cursor,
    // so the audio thread must not be inside process(). Every caller reaches
    // here inside a window that already proved that.
    return false;
  }
  servicedLatencyRevision_.store(engine_.latencyRevision(),
                                 std::memory_order_release);
  servicedPipelinePlanRevision_.store(engine_.pipelinePlanRevision(),
                                       std::memory_order_release);
  failedPipelinePlanRevision_ = 0;
  failedParameterImageGeneration_ = 0;
  pipelinePlanRefreshFailureCount_ = 0;
  recordPipelinePlanRefreshOutcome(true);
  latencySamples_.store(next, std::memory_order_release);
  latencyChanged = previous != next;
  if (trace::enabled()) {
    trace::latency(traceInstance_, trace::Event::latencySynced, previous, next,
                     engine_.pipelineLatency(),
                     resamplerLatencySamples_.load(std::memory_order_acquire),
                     activeOversamplingFactor_.load(std::memory_order_acquire));
  }
  return true;
}

void EffeTuneProcessor::capturePendingLatencyUpdate() noexcept {
  if (latencyUpdateState_.load(std::memory_order_acquire) != LatencyUpdateState::idle ||
      engine_.pipelinePlanRevision() ==
          servicedPipelinePlanRevision_.load(std::memory_order_acquire)) {
    return;
  }
  if (engine_.capturePipelineLatencyUpdate()) {
    latencyUpdateChannels_ = engine_.channels();
    latencyUpdateResampler_ = resamplerLatencySamples_.load(std::memory_order_relaxed);
    latencyUpdateOversampling_ = activeOversamplingFactor_.load(std::memory_order_relaxed);
    latencyUpdateRevision_ = engine_.pipelinePlanRevision();
    latencyUpdateState_.store(LatencyUpdateState::captured, std::memory_order_release);
  }
}

bool EffeTuneProcessor::prepareCapturedLatencyUpdate() noexcept {
  std::uint32_t plannedLatency = 0;
  return engine_.preparePipelineLatencyUpdate(plannedLatency) &&
         DryDelayLine::prepareUpdate(
             dryLatencyUpdate_, latencyUpdateChannels_,
             calculateTotalLatency(latencyUpdateResampler_,
                                   latencyUpdateOversampling_, plannedLatency));
}

void EffeTuneProcessor::discardPendingLatencyUpdate() noexcept {
  engine_.discardPipelineLatencyUpdate();
  dryLatencyUpdate_ = {};
  latencyUpdateState_.store(LatencyUpdateState::idle, std::memory_order_release);
}

bool EffeTuneProcessor::applyPreparedLatencyUpdate() noexcept {
  if (latencyUpdateState_.load(std::memory_order_acquire) !=
      LatencyUpdateState::prepared) {
    return false;
  }
  std::uint64_t revision = 0;
  const auto applied = engine_.applyPipelineLatencyUpdate(revision);
  if (applied) {
    dryDelay_.applyUpdate(dryLatencyUpdate_);
    const auto next = dryLatencyUpdate_.delayFrames;
    const auto previous = latencySamples_.exchange(next, std::memory_order_acq_rel);
    servicedLatencyRevision_.store(engine_.latencyRevision(), std::memory_order_release);
    servicedPipelinePlanRevision_.store(revision, std::memory_order_release);
    if (previous != next) {
      appliedLatencyNotificationPending_.store(true, std::memory_order_release);
    }
    recordPipelinePlanRefreshOutcome(true);
    if (trace::enabled()) {
      trace::latency(traceInstance_, trace::Event::latencySynced, previous, next,
                     engine_.pipelineLatency(), latencyUpdateResampler_,
                     latencyUpdateOversampling_);
    }
  }
  // Staleness is normal during a drag. Preserve the old plan, retire on the
  // control thread, and capture again after it returns the slot. No callback
  // allocation, destruction, lock, retry loop or host notification occurs here.
  latencyUpdateState_.store(LatencyUpdateState::retired, std::memory_order_release);
  return applied;
}

bool EffeTuneProcessor::hasPendingControlWork() const noexcept {
  return latencyUpdateState_.load(std::memory_order_acquire) != LatencyUpdateState::idle ||
         appliedLatencyNotificationPending_.load(std::memory_order_acquire) ||
         descriptorGeneration_.load(std::memory_order_acquire) !=
             servicedDescriptorGeneration_.load(std::memory_order_acquire) ||
         parameterImageGeneration_.load(std::memory_order_acquire) !=
             servicedParameterImageGeneration_.load(std::memory_order_acquire) ||
         parameterMailbox_.hasPending() ||
         controllerWritePending_.load(std::memory_order_acquire) ||
         engine_.pipelinePlanRevision() !=
             servicedPipelinePlanRevision_.load(std::memory_order_acquire) ||
         engine_.latencyRevision() !=
             servicedLatencyRevision_.load(std::memory_order_acquire);
}

std::chrono::steady_clock::duration
EffeTuneProcessor::audioIdleThreshold() const noexcept {
  // Idleness has to outlast one host block, and setupProcessing() already told
  // us how long that is. A fixed threshold either reports a slow host as
  // stopped or makes a genuinely stopped transport wait far longer than it
  // needs to before its edits reach the DSP.
  const auto frames = maxHostFrames_.load(std::memory_order_acquire);
  const auto sampleRate = hostSampleRate_.load(std::memory_order_acquire);
  auto threshold = std::chrono::milliseconds(0);
  if (frames > 0 && sampleRate > 0.0) {
    threshold = std::chrono::milliseconds(static_cast<std::int64_t>(
        std::ceil(3000.0 * static_cast<double>(frames) / sampleRate)));
  }
  // Three full maximum-size block periods are the safety horizon. A ceiling
  // would turn a valid slow callback into "idle" between blocks and let a
  // control thread take ownership from audio that is still running.
  return std::max(threshold, std::chrono::milliseconds(60));
}

bool EffeTuneProcessor::observeAudioIdle(
    const std::chrono::steady_clock::time_point now) noexcept {
  // The epoch is odd for the complete duration of process(), including the
  // portion before renderedBlockCount_ advances. That direct evidence always
  // outranks the elapsed-time heuristic.
  if ((processBlockEpoch_.load(std::memory_order_seq_cst) & 1u) != 0u) {
    return false;
  }
  const auto rendered = renderedBlockCount_.load(std::memory_order_acquire);
  if (rendered != observedRenderedBlockCount_.load(std::memory_order_acquire)) {
    // The instant is published before the count it belongs to, so a second
    // control thread that observes the new count can never read the instant
    // that preceded it and mistake a running transport for a stopped one.
    renderedBlockObservedAtTicks_.store(now.time_since_epoch().count(),
                                        std::memory_order_release);
    observedRenderedBlockCount_.store(rendered, std::memory_order_release);
    return false;
  }
  // Never having rendered a block is the one state the timestamp cannot
  // describe, so it is answered explicitly instead of letting a default-
  // constructed instant read as infinitely old.
  if (rendered == 0) {
    return true;
  }
  const std::chrono::steady_clock::time_point observedAt{
      std::chrono::steady_clock::duration{
          renderedBlockObservedAtTicks_.load(std::memory_order_acquire)}};
  return now - observedAt >= audioIdleThreshold();
}

void EffeTuneProcessor::serviceLatencyUpdates(const bool restartDebounce) {
  // The parameter mailbox keeps one reading position per entry and the runtime
  // image has one set of dirty flags, so exactly one thread may consume them.
  // Claiming them here and waiting the current block out gives this section
  // sole ownership without the audio callback taking a lock.
  struct RuntimeImageOwnership {
    EffeTuneProcessor &processor;
    explicit RuntimeImageOwnership(EffeTuneProcessor &owner) noexcept
        : processor(owner) {
      processor.controlOwnsRuntimeImage_.store(true, std::memory_order_seq_cst);
      processor.waitForAudioQuiescence();
    }
    RuntimeImageOwnership(const RuntimeImageOwnership &) = delete;
    RuntimeImageOwnership &operator=(const RuntimeImageOwnership &) = delete;
    ~RuntimeImageOwnership() noexcept {
      processor.controlOwnsRuntimeImage_.store(false, std::memory_order_release);
    }
  };

  const auto now = std::chrono::steady_clock::now();
  // Taken before the lock and by every caller, so no starved thread can leave
  // the sample stale long enough for a playing transport to read as stopped.
  const auto audioIdle = observeAudioIdle(now);
  if (audioIdle) {
    commitPendingControllerWritesIfAudioIdle();
  }
  if (!hasPendingControlWork() &&
      !latencyDebounceArmed_.load(std::memory_order_acquire)) {
    return;
  }

  // Everything below is sequential control state, and the control-service timer,
  // the WebView message handler and the host thread all reach it, so it belongs
  // under the control mutex. It is taken without waiting: the editor polls this
  // at frame rate and must never block behind another control thread, and a tick
  // that finds one inside has nothing of its own to contribute -- the work is
  // derived from published generations, so whoever is inside, or the next tick,
  // performs exactly the same work.
  std::unique_lock resources(processingResourcesMutex_, std::try_to_lock);
  if (!resources.owns_lock()) {
    return;
  }
  if (appliedLatencyNotificationPending_.exchange(false, std::memory_order_acq_rel)) {
    queueLatencyNotification(restartDebounce);
  }
  auto updateState = latencyUpdateState_.load(std::memory_order_acquire);
  if (updateState == LatencyUpdateState::retired) {
    discardPendingLatencyUpdate();
    updateState = LatencyUpdateState::idle;
  }
  if (updateState == LatencyUpdateState::captured &&
      latencyUpdateRevision_ != engine_.pipelinePlanRevision()) {
    discardPendingLatencyUpdate();
    updateState = LatencyUpdateState::idle;
  }
  if (updateState == LatencyUpdateState::captured &&
      (latencyUpdateRevision_ != failedPipelinePlanRevision_ ||
       now >= pipelinePlanRetryDeadline_)) {
    auto prepared = false;
#if defined(EFFETUNE_PROCESSOR_TEST_HOOKS)
    if (pipelinePlanRefreshFailuresForTesting_ != 0) {
      --pipelinePlanRefreshFailuresForTesting_;
    } else
#endif
    {
      prepared = prepareCapturedLatencyUpdate();
    }
    if (prepared) {
      pipelinePlanRefreshFailureCount_ = 0;
      latencyUpdateState_.store(LatencyUpdateState::prepared, std::memory_order_release);
    } else {
      failedPipelinePlanRevision_ = latencyUpdateRevision_;
      pipelinePlanRefreshFailureCount_ =
          std::min(pipelinePlanRefreshFailureCount_ + 1u, 6u);
      pipelinePlanRetryDeadline_ = now + std::chrono::milliseconds(
          50u << std::min(pipelinePlanRefreshFailureCount_ - 1u, 4u));
      recordPipelinePlanRefreshOutcome(false);
    }
  }
  // Runs as its own scope because every deferral below leaves through an early
  // return, while the debounce that follows still has to be evaluated.
  const auto servicePendingControlWork = [&] {
    const auto descriptorGeneration =
        descriptorGeneration_.load(std::memory_order_acquire);
    const auto descriptorOutstanding =
        descriptorGeneration !=
        servicedDescriptorGeneration_.load(std::memory_order_acquire);
    const auto planStale =
        engine_.pipelinePlanRevision() !=
        servicedPipelinePlanRevision_.load(std::memory_order_acquire);
    const auto latencyStale = engine_.latencyRevision() !=
                              servicedLatencyRevision_.load(std::memory_order_acquire);
    auto parameterImageGeneration =
        parameterImageGeneration_.load(std::memory_order_acquire);
    const auto imageWork =
        parameterMailbox_.hasPending() ||
        parameterImageGeneration !=
            servicedParameterImageGeneration_.load(std::memory_order_acquire);

    // Whether the engine is really written this tick is decided before the
    // window is opened. A tick that only finds a retry backoff must not close
    // the processing gate to discover it has nothing to do.
    const auto retryDeferred = now < pipelinePlanRetryDeadline_;
    const auto descriptorIsNew =
        descriptorGeneration != failedDescriptorGeneration_ ||
        parameterImageGeneration != failedParameterImageGeneration_;
    const auto applyDescriptor =
        descriptorOutstanding && (descriptorIsNew || !retryDeferred);
    // Taking the engine over closes the processing gate, so a block that starts
    // inside the window loses its processed signal. Only work with no other way
    // in earns that: a descriptor being applied now, which is the explicit
    // topology change the user is waiting for, and anything at all once the
    // audio callback has stopped rendering and is no longer there to apply it.
    // Running compensation updates use the snapshot handoff above and never
    // close this gate. Once audio is idle the same upstream API can be applied
    // synchronously by the exclusive control owner.
    const auto ownEngine = applyDescriptor || audioIdle;
    const auto planRevisionIsNew =
        engine_.pipelinePlanRevision() != failedPipelinePlanRevision_ ||
        parameterImageGeneration != failedParameterImageGeneration_;
    const auto refreshPlan =
        ownEngine && planStale && (planRevisionIsNew || !retryDeferred);
    const auto engineOwned = applyDescriptor || refreshPlan ||
                             (ownEngine && (latencyStale || imageWork));
    if (!engineOwned && !imageWork) {
      return;
    }

    const RuntimeImageOwnership owned{*this};
    std::optional<EngineMutationWindow> engineWindow;
    if (engineOwned) {
      engineWindow.emplace(*this);
      discardPendingLatencyUpdate();
    }
    parameterImageGeneration =
        parameterImageGeneration_.load(std::memory_order_acquire);
    const auto recordRefreshFailure = [&](const bool freshInput) noexcept {
      pipelinePlanRefreshFailureCount_ =
          freshInput ? 1u : std::min(pipelinePlanRefreshFailureCount_ + 1u, 6u);
      const auto retryMilliseconds =
          50u << std::min(pipelinePlanRefreshFailureCount_ - 1u, 4u);
      pipelinePlanRetryDeadline_ = now + std::chrono::milliseconds(retryMilliseconds);
      recordPipelinePlanRefreshOutcome(false);
    };

    if (applyDescriptor) {
      if (!consumePendingControlUpdatesLocked(engineOwned)) {
        failedDescriptorGeneration_ = descriptorGeneration;
        failedParameterImageGeneration_ = parameterImageGeneration;
        recordRefreshFailure(descriptorIsNew);
        return;
      }

      std::uint64_t appliedRevision = 0;
      std::string error;
      auto applied = false;
      const auto previousPipelineLatency = trace::enabled() ? engine_.pipelineLatency() : 0u;
#if defined(EFFETUNE_PROCESSOR_TEST_HOOKS)
      if (pipelinePlanRefreshFailuresForTesting_ != 0) {
        --pipelinePlanRefreshFailuresForTesting_;
      } else
#endif
      if (pendingDescriptorCommand_.has_value()) {
        applied = engine_.applyDescriptorCommand(*pendingDescriptorCommand_,
                                                 appliedRevision, &error);
      }
      if (trace::enabled()) {
        trace::descriptorApplied(traceInstance_, descriptorGeneration,
                                   previousPipelineLatency, engine_.pipelineLatency(), applied);
      }
      if (!applied) {
        failedDescriptorGeneration_ = descriptorGeneration;
        failedParameterImageGeneration_ = parameterImageGeneration;
        recordRefreshFailure(descriptorIsNew);
        return;
      }
      pendingDescriptorCommand_.reset();
      servicedDescriptorGeneration_.store(descriptorGeneration,
                                           std::memory_order_release);
      servicedPipelinePlanRevision_.store(appliedRevision,
                                          std::memory_order_release);
      failedDescriptorGeneration_ = 0;
      failedParameterImageGeneration_ = 0;
      failedPipelinePlanRevision_ = 0;
      pipelinePlanRefreshFailureCount_ = 0;
      recordPipelinePlanRefreshOutcome(true);
    }

    parameterImageGeneration =
        parameterImageGeneration_.load(std::memory_order_acquire);
    if (!consumePendingControlUpdatesLocked(engineOwned)) {
      recordPipelinePlanRefreshOutcome(false);
      return;
    }
    // Staging the images above can publish a newer plan revision, so the
    // refresh is re-tested here; the pre-window decision only governs whether
    // closing the gate was worth it.
    const auto planRevision = engine_.pipelinePlanRevision();
    if (engineOwned && planRevision != servicedPipelinePlanRevision_.load(
                                           std::memory_order_acquire)) {
      const auto newRevision =
          planRevision != failedPipelinePlanRevision_ ||
          parameterImageGeneration != failedParameterImageGeneration_;
      if (!newRevision && retryDeferred) {
        return;
      }
      auto refreshed = false;
#if defined(EFFETUNE_PROCESSOR_TEST_HOOKS)
      if (pipelinePlanRefreshFailuresForTesting_ != 0) {
        --pipelinePlanRefreshFailuresForTesting_;
      } else
#endif
      {
        capturePendingLatencyUpdate();
        if (latencyUpdateState_.load(std::memory_order_acquire) ==
                LatencyUpdateState::captured && prepareCapturedLatencyUpdate()) {
          latencyUpdateState_.store(LatencyUpdateState::prepared, std::memory_order_release);
          refreshed = applyPreparedLatencyUpdate();
        }
        discardPendingLatencyUpdate();
      }
      if (!refreshed) {
        failedPipelinePlanRevision_ = planRevision;
        failedParameterImageGeneration_ = parameterImageGeneration;
        recordRefreshFailure(newRevision);
        return;
      }
      failedPipelinePlanRevision_ = 0;
      failedParameterImageGeneration_ = 0;
      pipelinePlanRefreshFailureCount_ = 0;
      recordPipelinePlanRefreshOutcome(true);
    }
    auto latencyChanged = false;
    if (engineOwned && engine_.latencyRevision() !=
                           servicedLatencyRevision_.load(std::memory_order_acquire)) {
      // The window has already proved the audio thread is out of process(), so
      // the delay-line resize needs nothing of its own.
      if (!synchronizeLatencyLocked(latencyChanged)) {
        return;
      }
    }
    if (latencyChanged) {
      queueLatencyNotification(restartDebounce);
    }
    // Images the audio callback has not staged yet are not serviced: the next
    // tick re-reads them, and by then the callback has either applied them or
    // stopped running, which is what hands the engine back to this thread.
    const auto imagesOutstanding =
        std::any_of(runtimeFullImageDirty_.begin(),
                    runtimeFullImageDirty_.begin() +
                        static_cast<std::ptrdiff_t>(runtimePlugins_.size()),
                    [](const bool dirty) { return dirty; });
    if (!imagesOutstanding) {
      servicedParameterImageGeneration_.store(parameterImageGeneration,
                                               std::memory_order_release);
    }
  };
  servicePendingControlWork();
  if (appliedLatencyNotificationPending_.exchange(false, std::memory_order_acq_rel)) {
    queueLatencyNotification(restartDebounce);
  }

  // Nothing below reads control state that the mutex guards -- the debounce is
  // carried entirely by atomics -- and what follows is a call into the host. The
  // discipline this file keeps everywhere else is that the host is never called
  // while processingResourcesMutex_ is held: a host processes restartComponent()
  // inline, and anything it does in answer -- asking for the state, re-reading
  // the parameter bank, writing a parameter back through setParamNormalized() --
  // re-enters the plug-in through a mutex that is not recursive. This was the
  // one site that still held it across such a call.
  resources.unlock();

  if (!latencyDebounceArmed_.load(std::memory_order_acquire)) {
    return;
  }
  const std::chrono::steady_clock::time_point deadline{
      std::chrono::steady_clock::duration{
          latencyNotificationDeadlineTicks_.load(std::memory_order_acquire)}};
  if (now < deadline) {
    return;
  }
  latencyDebounceArmed_.store(false, std::memory_order_release);
  if (latencyNotificationPending_.exchange(false, std::memory_order_acq_rel)) {
    if (auto *handler = getComponentHandler(); handler != nullptr) {
      traceRestartComponent(*handler, RestartFlags::kLatencyChanged, traceInstance_);
    }
  }
}

tresult PLUGIN_API EffeTuneProcessor::setState(IBStream *stream) {
  std::string json;
  PluginStateDocument decoded;
  if (!readStream(stream, json) || !StateCodec::decode(json, decoded)) {
    return kResultFalse;
  }
  if (exceedsNativeStateCapacity(decoded.pipelineA, engine_.kernels()) ||
      (decoded.pipelineBInitialized &&
       exceedsNativeStateCapacity(decoded.pipelineB, engine_.kernels()))) {
    return kResultFalse;
  }
  // A restore replaces the pipeline the user's hand was on and then destroys
  // the JS context outright, so the editor's own release path can never run for
  // whatever is open here: the reloaded page starts with no gesture targets at
  // all and sends no close. A touch that survived this would leave the host
  // writing a lane the user is not holding, and leave the block ignoring the
  // host's input for that parameter for the rest of the session. The bindings
  // below are reconciled against the restored document, so this also has to run
  // before a slot that is about to be retired stops being nameable.
  closeOpenHostGestures();
  drainAutomationValues();
  const auto restoredBypass = decoded.masterBypass;
  {
    AutomationResourceLock resources{*this};
    acknowledgePublishedAutomationLocked();
    clearPendingControllerWritesLocked();
    {
      std::scoped_lock deltaLock(automationDeltaMutex_);
      pendingAutomationDeltaDirty_.reset();
    }
    automationDeltaPending_.store(false, std::memory_order_release);
    {
      std::scoped_lock stateLock(stateMutex_);
      state_ = std::move(decoded);
      undoOpaqueState_.clear();
      preserveMissingPipelineA_ = true;
      preserveMissingPipelineB_ = true;
      hasSavedState_ = true;
    }
    // The decoded document is now the save/UI authority, but every object the
    // audio callback reads still belongs to the old generation. Publishing the
    // monotonic epoch and marker last lets an update that outlives the complete
    // pending true -> false cycle distinguish those worlds.
    stateReplacementEpoch_.fetch_add(1, std::memory_order_release);
    replacementPageAuthorized_ = false;
    stateReplacementPending_.store(true, std::memory_order_release);
  }
  // The host-facing Parameter may reflect the restored chunk immediately. The
  // override sees the pending marker and deliberately leaves the old bypass
  // atomic and scheduler untouched until the runtime replacement arrives.
  setParamNormalized(kBypassParameterId, restoredBypass ? 1.0 : 0.0);
  std::shared_ptr<WebViewHost> webView;
  {
    std::scoped_lock editorLock(editorMutex_);
    if (!editorTerminating_) {
      webView = webView_;
    }
  }
  if (webView != nullptr) {
    (void)webView->evaluate("window.location.reload();", {});
  }
  return kResultOk;
}

tresult PLUGIN_API EffeTuneProcessor::getState(IBStream *stream) {
  if (stream == nullptr) {
    return kResultFalse;
  }
  commitPendingControllerWritesIfAudioIdle(
      /*waitForResources=*/true);
  drainAutomationValues();
  PluginStateDocument snapshot;
  {
    std::scoped_lock stateLock(stateMutex_);
    snapshot = state_;
  }
  if (!stateReplacementPending_.load(std::memory_order_acquire)) {
    snapshot.masterBypass = bypass_.load(std::memory_order_acquire);
  }
  const auto json = StateCodec::encode(snapshot);
  if (json.size() > static_cast<std::size_t>(std::numeric_limits<int32>::max())) {
    return kResultFalse;
  }
  int32 bytesWritten = 0;
  const auto result = stream->write(const_cast<char *>(json.data()), static_cast<int32>(json.size()),
                                    &bytesWritten);
  return (result == kResultOk || result == kResultTrue) &&
                 bytesWritten == static_cast<int32>(json.size())
             ? kResultOk
             : kResultFalse;
}

uint32 PLUGIN_API EffeTuneProcessor::getLatencySamples() {
  const auto latency = latencySamples_.load(std::memory_order_acquire);
  if (trace::enabled()) {
    trace::latencyRead(traceInstance_, latency);
  }
  return latency;
}

tresult PLUGIN_API EffeTuneProcessor::setAutomationState(const int32 state) {
  const auto reported = static_cast<std::int32_t>(state);
  if (trace::enabled()) {
    trace::automationState(traceInstance_, reported);
  }
  if (hostAutomationState_.exchange(reported, std::memory_order_acq_rel) !=
      reported) {
    // The write gate is a transient host state the user toggles repeatedly, so
    // the one-shot diagnostic is re-armed whenever that state changes. Refusals
    // within one state stay silent, which keeps a knob drag from spamming.
    automationWriteGateWarningIssued_.store(false, std::memory_order_release);
  }
  return kResultOk;
}

IPlugView *PLUGIN_API EffeTuneProcessor::createView(const FIDString name) {
  if (name != nullptr && std::strcmp(name, ViewType::kEditor) == 0) {
    return EffeTuneView::create(this);
  }
  return nullptr;
}

bool EffeTuneProcessor::attachEditor(void *owner, void *parent,
                                     const std::int32_t width,
                                     const std::int32_t height) {
  std::shared_ptr<WebViewHost> webView;
  try {
    {
      std::scoped_lock editorLock(editorMutex_);
      if (editorTerminating_) {
        return false;
      }
      webView = webView_;
    }
    if (webView == nullptr) {
      auto created = std::make_shared<WebViewHost>(
          [this](const std::string_view message) {
            return handleUiMessage(message);
          });
      std::scoped_lock editorLock(editorMutex_);
      if (!editorTerminating_ && webView_ == nullptr) {
        webView_ = created;
      }
      webView = webView_;
    }
    if (webView == nullptr) {
      return false;
    }
    return webView->attach(owner, parent, width, height);
  } catch (const std::exception &) {
    {
      std::scoped_lock editorLock(editorMutex_);
      if (webView_ == webView) {
        webView_.reset();
      }
    }
    webView.reset();
    return false;
  }
}

void EffeTuneProcessor::detachEditor(void *owner) noexcept {
  std::shared_ptr<WebViewHost> webView;
  {
    std::scoped_lock editorLock(editorMutex_);
    if (!editorTerminating_) {
      webView = webView_;
    }
  }
  if (webView != nullptr) {
    webView->detach(owner);
  }
  // The editor is the only thing that can hold a touch open, and it is gone.
  // Its own pointer-release path can no longer run, so this is the last chance
  // to tell the host the user's hand has left the control.
  closeOpenHostGestures();
}

void EffeTuneProcessor::resizeEditor(void *owner, const std::int32_t width,
                                     const std::int32_t height) noexcept {
  std::shared_ptr<WebViewHost> webView;
  {
    std::scoped_lock editorLock(editorMutex_);
    if (!editorTerminating_) {
      webView = webView_;
    }
  }
  if (webView != nullptr) {
    webView->resize(owner, width, height);
  }
}

namespace {

[[nodiscard]] std::string bridgeResult(const bool ok, const std::string &error = {}) {
  auto result = choc::value::createObject({});
  result.addMember("ok", ok);
  result.addMember("success", ok);
  if (!error.empty()) {
    result.addMember("error", error);
  }
  return choc::json::toString(result);
}

// An unbound edit is not a failure: the value already reached the DSP through
// the preceding plug-in update, so the UI must keep it instead of rolling back.
[[nodiscard]] std::string automationEditResult(const bool bound) {
  auto result = choc::value::createObject({});
  result.addMember("ok", true);
  result.addMember("success", true);
  result.addMember("bound", bound);
  return choc::json::toString(result);
}

[[nodiscard]] bool virtualPathEndsWith(const std::string_view path,
                                       const std::string_view filename) {
  return path.size() >= filename.size() && path.substr(path.size() - filename.size()) == filename;
}

[[nodiscard]] bool sameTopology(const PluginState &left, const PluginState &right) {
  return left.id == right.id && left.name == right.name && left.enabled == right.enabled &&
         left.inputBus == right.inputBus && left.outputBus == right.outputBus &&
         left.channel == right.channel;
}

[[nodiscard]] std::uint64_t assetKey(const std::uint32_t logicalId,
                                     const std::uint32_t slot) noexcept {
  return (static_cast<std::uint64_t>(logicalId) << 32u) | slot;
}

} // namespace

std::string EffeTuneProcessor::handleUiMessage(const std::string_view request) {
  const trace::ScopedRole traceRole{trace::Role::ui};
  std::uint64_t requestStateEpoch = 0;
  std::uint64_t requestPageGeneration = 0;
  auto requestStartedDuringStateReplacement = false;
  requestStateEpoch = stateReplacementEpoch_.load(std::memory_order_acquire);
  requestPageGeneration = uiPageGeneration_.load(std::memory_order_acquire);
  requestStartedDuringStateReplacement =
      stateReplacementPending_.load(std::memory_order_acquire);
  RoutedUiMessage message;
  std::string error;
  if (!MessageRouter::decode(request, message, &error)) {
    return bridgeResult(false, error);
  }
  drainAutomationValues();

  const auto pauseBulkRequestBeforeCommitForTesting = [&] {
#if defined(EFFETUNE_PROCESSOR_TEST_HOOKS)
    if (pauseNextBulkRequestBeforeCommitForTesting_.exchange(
            false, std::memory_order_acq_rel)) {
      bulkRequestPausedBeforeCommitForTesting_.store(true,
                                                     std::memory_order_release);
      while (!releaseBulkRequestBeforeCommitForTesting_.load(
          std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      bulkRequestPausedBeforeCommitForTesting_.store(false,
                                                     std::memory_order_release);
      releaseBulkRequestBeforeCommitForTesting_.store(false,
                                                      std::memory_order_release);
    }
#endif
  };
  // Callers hold processingResourcesMutex_. A bulk request is allowed to
  // commit only into the state and page generation in which it entered the
  // bridge; observing the same instantaneous pending flag is insufficient
  // because a complete restore can make it false again.
  const auto bulkRequestCrossedGenerationLocked = [&] {
    return stateReplacementEpoch_.load(std::memory_order_acquire) !=
               requestStateEpoch ||
           uiPageGeneration_.load(std::memory_order_acquire) !=
               requestPageGeneration;
  };
  const auto authorizedReplacementRequestLocked = [&] {
    return requestStartedDuringStateReplacement &&
           stateReplacementPending_.load(std::memory_order_acquire) &&
           replacementPageAuthorized_ &&
           replacementPageStateEpoch_ ==
               stateReplacementEpoch_.load(std::memory_order_acquire) &&
           replacementPageGeneration_ ==
               uiPageGeneration_.load(std::memory_order_acquire) &&
           !bulkRequestCrossedGenerationLocked();
  };

  if (message.action == UiAction::hostInfo) {
    // A page announcing its own startup is the boundary a state restore cannot
    // draw for itself. setState() ends every open touch and then asks the
    // editor to reload, but the reload is a request the old context outlives:
    // a drag still in progress there can flush one more value between the two,
    // and that value reopens a touch on a slot that survived the restore. The
    // page that comes back starts with no gesture targets, so nothing in it
    // could ever name that touch to close it. Whatever is open when a context
    // announces itself therefore belongs to a context that is already gone --
    // no page has been touched before its own first call -- so ending it here
    // is unconditional and cannot be missed by a race between the two.
    if (message.startupHandshake) {
      closeOpenHostGestures();
      std::scoped_lock resources(processingResourcesMutex_);
      const auto pageGeneration =
          uiPageGeneration_.fetch_add(1, std::memory_order_release) + 1;
      if (stateReplacementPending_.load(std::memory_order_acquire)) {
        replacementPageGeneration_ = pageGeneration;
        replacementPageStateEpoch_ =
            stateReplacementEpoch_.load(std::memory_order_acquire);
        replacementPageAuthorized_ = true;
      } else {
        replacementPageAuthorized_ = false;
      }
    }
    serviceLatencyUpdates();
    const auto context = readHostContext();
    OversamplingSettings oversampling;
    {
      std::scoped_lock stateLock(stateMutex_);
      oversampling = state_.oversampling;
    }
    auto result = choc::value::createObject({});
    result.addMember("ok", true);
    result.addMember("sampleRate", context.sampleRate);
    result.addMember("engineSampleRate", context.engineSampleRate);
    result.addMember("channels", static_cast<std::int64_t>(context.channels));
    result.addMember("oversamplingFactor",
                     static_cast<std::int64_t>(context.oversamplingFactor));
    result.addMember("oversamplingPhase",
                     oversampling.phase == OversamplingPhase::minimum ? "minimum" : "linear");
    const char *quality = "medium";
    if (oversampling.quality == FilterQuality::low) quality = "low";
    if (oversampling.quality == FilterQuality::high) quality = "high";
    if (oversampling.quality == FilterQuality::ultra) quality = "ultra";
    result.addMember("oversamplingQuality", quality);
    const auto reportedLatency = latencySamples_.load(std::memory_order_acquire);
    const auto processingLatency = processingLatencySamples();
    result.addMember("latencySamples", static_cast<std::int64_t>(reportedLatency));
    result.addMember("processingLatencySamples",
                     static_cast<std::int64_t>(processingLatency));
    result.addMember("latencyCompensated",
                     processingLatency == reportedLatency &&
                         engine_.pipelinePlanRevision() ==
                             servicedPipelinePlanRevision_.load(std::memory_order_acquire));
    result.addMember(
        "pipelineCpuAverage",
        static_cast<double>(pipelineCpuAverageHundredths_.load(std::memory_order_acquire)) /
            100.0);
    result.addMember("masterBypass", bypass_.load(std::memory_order_acquire));
    result.addMember("dspReady", processingReady_.load(std::memory_order_seq_cst));
    result.addMember(
        "stateReplacementPending",
        stateReplacementPending_.load(std::memory_order_acquire));
    result.addMember("contextGeneration",
                     static_cast<std::int64_t>(context.generation));
    result.addMember("version", std::string(EFFETUNE_PLUGIN_VERSION_STR));
    appendExecutionStates(result);
    appendAutomationDeltas(result);
    appendDeferredDiagnostics(result);
    return choc::json::toString(result);
  }

  if (message.action == UiAction::openExternalUrl) {
    return bridgeResult(openExternalUrl(message.url, &error), error);
  }

  if (message.action == UiAction::storageFileExists) {
    bool exists = false;
    if (virtualPathEndsWith(message.path, "pipeline-state.json")) {
      std::scoped_lock stateLock(stateMutex_);
      exists = hasSavedState_;
    } else {
      exists = presetStore_.exists(message.path);
    }
    auto result = choc::value::createObject({});
    result.addMember("ok", true);
    result.addMember("exists", exists);
    return choc::json::toString(result);
  }

  if (message.action == UiAction::readTelemetry) {
    serviceLatencyUpdates();
    const auto context = readHostContext();
    std::uint32_t droppedFrames = 0;
    const auto bytes = engine_.readTelemetry(telemetryScratch_, droppedFrames);
    auto result = choc::value::createObject({});
    result.addMember("ok", true);
    result.addMember("bytes", static_cast<std::int64_t>(bytes));
    result.addMember("droppedFrames", static_cast<std::int64_t>(droppedFrames));
    result.addMember("masterBypass", bypass_.load(std::memory_order_acquire));
    result.addMember("contextGeneration",
                     static_cast<std::int64_t>(context.generation));
    result.addMember("engineSampleRate", context.engineSampleRate);
    result.addMember("channels", static_cast<std::int64_t>(context.channels));
    const auto reportedLatency = latencySamples_.load(std::memory_order_acquire);
    const auto processingLatency = processingLatencySamples();
    result.addMember("latencySamples", static_cast<std::int64_t>(reportedLatency));
    result.addMember("processingLatencySamples",
                     static_cast<std::int64_t>(processingLatency));
    result.addMember("latencyCompensated",
                     processingLatency == reportedLatency &&
                         engine_.pipelinePlanRevision() ==
                             servicedPipelinePlanRevision_.load(std::memory_order_acquire));
    result.addMember(
        "pipelineCpuAverage",
        static_cast<double>(pipelineCpuAverageHundredths_.load(std::memory_order_acquire)) /
            100.0);
    result.addMember("packet", bytes == 0
                                   ? std::string{}
                                   : choc::base64::encodeToString(telemetryScratch_.data(), bytes));
    appendAutomationDeltas(result);
    appendDeferredDiagnostics(result);
    return choc::json::toString(result);
  }

  if (message.action == UiAction::discardTelemetry) {
    engine_.discardTelemetry();
    return bridgeResult(true);
  }

  if (message.action == UiAction::beginPluginAsset) {
    PendingAssetTransfer transfer;
    transfer.asset = std::move(message.asset);
    transfer.operationRevision = message.operationRevision;
    try {
      std::scoped_lock transferLock(assetTransferMutex_);
      const auto key = assetKey(transfer.asset.logicalId, transfer.asset.slot);
      std::uint64_t pendingFootprint = transfer.asset.footprintBytes;
      for (const auto &[pendingKey, pending] : pendingAssetTransfers_) {
        if (pendingKey != key) {
          pendingFootprint += pending.asset.footprintBytes;
        }
      }
      if (pendingFootprint > EngineHost::kAggregateAssetBudgetBytes) {
        return bridgeResult(false, "DSP asset transfer budget exceeded");
      }
      transfer.asset.payload.resize(message.assetByteSize);
      pendingAssetTransfers_.insert_or_assign(key, std::move(transfer));
    } catch (const std::bad_alloc &) {
      return bridgeResult(false, "DSP asset transfer could not be allocated");
    }
    return bridgeResult(true);
  }

  if (message.action == UiAction::appendPluginAsset) {
    std::vector<std::uint8_t> decoded;
    try {
      if (!choc::base64::decodeToContainer(decoded, message.content)) {
        return bridgeResult(false, "DSP asset chunk encoding is invalid");
      }
    } catch (const std::bad_alloc &) {
      return bridgeResult(false, "DSP asset chunk could not be allocated");
    }
    if (decoded.empty()) {
      return bridgeResult(false, "DSP asset chunk encoding is invalid");
    }
    std::scoped_lock transferLock(assetTransferMutex_);
    const auto found = pendingAssetTransfers_.find(
        assetKey(message.asset.logicalId, message.asset.slot));
    if (found == pendingAssetTransfers_.end() ||
        found->second.operationRevision != message.operationRevision ||
        found->second.receivedBytes != message.assetOffset ||
        decoded.size() > found->second.asset.payload.size() - found->second.receivedBytes) {
      return bridgeResult(false, "DSP asset chunks arrived out of sequence");
    }
    std::copy(decoded.begin(), decoded.end(),
              found->second.asset.payload.begin() +
                  static_cast<std::ptrdiff_t>(found->second.receivedBytes));
    found->second.receivedBytes += decoded.size();
    return bridgeResult(true);
  }

  if (message.action == UiAction::commitPluginAsset) {
    RuntimeAsset asset;
    {
      std::scoped_lock transferLock(assetTransferMutex_);
      const auto key = assetKey(message.asset.logicalId, message.asset.slot);
      const auto found = pendingAssetTransfers_.find(key);
      if (found == pendingAssetTransfers_.end() ||
          found->second.operationRevision != message.operationRevision ||
          found->second.receivedBytes != found->second.asset.payload.size()) {
        return bridgeResult(false, "DSP asset transfer is incomplete");
      }
      asset = std::move(found->second.asset);
      pendingAssetTransfers_.erase(found);
    }
    std::uint32_t assetState = 0;
    {
      // Staging copies the payload into the engine's arena and commits it, so
      // the audio thread has to be outside the DSP for the whole operation.
      std::scoped_lock resources(processingResourcesMutex_);
      const EngineMutationWindow engineWindow{*this};
      if (!engine_.setAsset(std::move(asset), &error)) {
        return bridgeResult(false, error);
      }
      assetState = engine_.assetState(message.asset.logicalId, message.asset.slot);
    }
    auto result = choc::value::createObject({});
    result.addMember("ok", true);
    result.addMember("success", true);
    result.addMember("state", static_cast<std::int64_t>(assetState));
    return choc::json::toString(result);
  }

  if (message.action == UiAction::clearPluginAsset) {
    {
      std::scoped_lock transferLock(assetTransferMutex_);
      pendingAssetTransfers_.erase(assetKey(message.asset.logicalId, message.asset.slot));
    }
    std::scoped_lock resources(processingResourcesMutex_);
    const EngineMutationWindow engineWindow{*this};
    (void)engine_.clearAsset(message.asset.logicalId, message.asset.slot);
    return bridgeResult(true);
  }

  if (message.action == UiAction::readPluginAssetState) {
    // The UI polls this while an asset prepares, so it must never stop the
    // DSP: the engine publishes the preparation state the block observed, and
    // this only reads that projection.
    const auto assetState =
        engine_.assetState(message.asset.logicalId, message.asset.slot);
    auto result = choc::value::createObject({});
    result.addMember("ok", true);
    result.addMember("state", static_cast<std::int64_t>(assetState));
    return choc::json::toString(result);
  }

  if (message.action == UiAction::loadConfig) {
    UiSettings settings;
    std::string configJson;
    {
      std::scoped_lock stateLock(stateMutex_);
      settings = state_.ui;
      configJson = configJson_;
    }
    auto config = choc::value::createObject({});
    try {
      const auto stored = choc::json::parse(configJson);
      if (stored.isObject()) {
        for (std::uint32_t index = 0; index < stored.size(); ++index) {
          const auto member = stored.getObjectMemberAt(index);
          if (std::string_view(member.name) != "columns") {
            config.addMember(member.name, member.value);
          }
        }
      }
    } catch (const choc::json::ParseError &) {
    }
    config.addMember("columns", static_cast<std::int64_t>(settings.columns));
    auto result = choc::value::createObject({});
    result.addMember("ok", true);
    result.addMember("success", true);
    result.addMember("config", std::move(config));
    return choc::json::toString(result);
  }

  if (message.action == UiAction::saveConfig) {
    try {
      const auto config = choc::json::parse(message.content);
      if (!config.isObject()) {
        return bridgeResult(false, "Config payload must be an object");
      }
      if (!configStore_.save(message.content, &error)) {
        return bridgeResult(false, error);
      }
      std::scoped_lock stateLock(stateMutex_);
      const auto columns = config["columns"].getWithDefault<std::int64_t>(state_.ui.columns);
      if (columns >= 1 && columns <= 8) {
        state_.ui.columns = static_cast<std::uint32_t>(columns);
      }
      configJson_ = message.content;
      return bridgeResult(true);
    } catch (const choc::json::ParseError &) {
      return bridgeResult(false, "Config payload is invalid");
    }
  }

  if (message.action == UiAction::storageReadFile) {
    std::string content;
    if (virtualPathEndsWith(message.path, "pipeline-state.json")) {
      PluginStateDocument snapshot;
      {
        std::scoped_lock stateLock(stateMutex_);
        snapshot = state_;
      }
      if (!stateReplacementPending_.load(std::memory_order_acquire)) {
        snapshot.masterBypass = bypass_.load(std::memory_order_acquire);
      }
      content = StateCodec::encode(snapshot);
    } else if (presetStore_.handles(message.path)) {
      if (!presetStore_.read(message.path, content, &error)) {
        return bridgeResult(false, error);
      }
    } else {
      std::scoped_lock exchangeLock(presetExchangeMutex_);
      if (!readablePresetPath_.has_value() || *readablePresetPath_ != message.path ||
          !readPresetExchangeFile(presetPathFromUtf8(message.path), content, &error)) {
        return bridgeResult(false, error.empty() ? "Preset path was not approved" : error);
      }
    }
    auto result = choc::value::createObject({});
    result.addMember("ok", true);
    result.addMember("success", true);
    result.addMember("content", content);
    return choc::json::toString(result);
  }

  if (message.action == UiAction::storageWriteFile) {
    if (virtualPathEndsWith(message.path, "pipeline-state.json")) {
      return bridgeResult(true);
    }
    if (presetStore_.handles(message.path)) {
      return presetStore_.write(message.path, message.content, &error)
                 ? bridgeResult(true)
                 : bridgeResult(false, error);
    }
    std::scoped_lock exchangeLock(presetExchangeMutex_);
    if (!writablePresetPath_.has_value() || *writablePresetPath_ != message.path) {
      return bridgeResult(false, "Preset path was not approved");
    }
    return writePresetExchangeFile(presetPathFromUtf8(message.path), message.content, &error)
               ? bridgeResult(true)
               : bridgeResult(false, error);
  }

  if (message.action == UiAction::openPresetDialog) {
    const auto selected = choosePresetToOpen();
    auto result = choc::value::createObject({});
    result.addMember("ok", true);
    result.addMember("canceled", !selected.has_value());
    auto paths = choc::value::createEmptyArray();
    if (selected.has_value()) {
      const auto path = presetPathToUtf8(*selected);
      {
        std::scoped_lock exchangeLock(presetExchangeMutex_);
        readablePresetPath_ = path;
      }
      paths.addArrayElement(path);
    }
    result.addMember("filePaths", std::move(paths));
    return choc::json::toString(result);
  }

  if (message.action == UiAction::savePresetDialog) {
    const auto selected = choosePresetToSave(message.defaultName);
    auto result = choc::value::createObject({});
    result.addMember("ok", true);
    result.addMember("canceled", !selected.has_value());
    if (selected.has_value()) {
      const auto path = presetPathToUtf8(*selected);
      {
        std::scoped_lock exchangeLock(presetExchangeMutex_);
        writablePresetPath_ = path;
      }
      result.addMember("filePath", path);
    }
    return choc::json::toString(result);
  }

  if (message.action == UiAction::setMasterBypass) {
    const auto normalized = message.masterBypass ? 1.0 : 0.0;
    // The transaction only offers the value to the host, and its answer is
    // deliberately not consulted -- the same invariant every other explicit
    // edit follows. beginEdit, performEdit and endEdit all answer kResultFalse
    // with no component handler installed, and a host may decline any of them
    // for reasons that say nothing about whether the user's button press should
    // take effect: no Write lane armed, a read-only pass, an automation writer
    // that is simply not listening. The editor has already flipped the button,
    // so refusing here left the toggle on and the DSP unbypassed, telemetry
    // snapped the button back, and getState() saved the value the DSP was not
    // playing. The transaction restores the host parameter to its previous
    // value when it fails; the adoption below overwrites that with the user's
    // value again.
    (void)performHostEditTransaction(kBypassParameterId, normalized);
    (void)setParamNormalized(kBypassParameterId, normalized);
    // beginEdit/performEdit/endEdit only notifies the host; it is not a route to
    // our own DSP. Adopt the gesture here so the bypass atomic and the scheduler
    // follow the button even when the host never echoes the edit back through
    // inputParameterChanges. The forced configuration overrides the scheduler's
    // preserve-current rule, and an echo re-applies the identical value, so the
    // two paths stay idempotent.
    {
      std::scoped_lock resources(processingResourcesMutex_);
      invalidatePendingControllerWriteLocked(kBypassHostGestureIndex);
      bypass_.store(message.masterBypass, std::memory_order_release);
      automationScheduler_.configureBypass(message.masterBypass,
                                           /*forceCurrentInitialization=*/true);
    }
    return bridgeResult(true);
  }

  if (message.action == UiAction::editAutomationParameter) {
    // A gesture that arrives on its own -- from an older UI or from a path that
    // sends no plug-in image -- is the same transaction the bundled gestures
    // below run, so it keeps no logic of its own.
    const auto outcome = applyAutomationEdit(
        {message.pipeline, message.pluginId, message.pluginType, message.parameterKey,
         message.elementIndex},
        message.normalizedValue, {});
    return automationEditResult(outcome == AutomationEditOutcome::bound);
  }

  if (message.action == UiAction::beginAutomationGesture) {
    // The pointer is down on a click-activated control, but the click carrying
    // its new value has not happened yet. Only an existing binding can be
    // opened here: merely pressing an unbound control must not permanently
    // consume a finite automation slot or create a lane before a value changes.
    const ScopedHostGroupEdit group{*this, message.automationEdits.size() > 1u,
                                    traceInstance_};
    for (const auto &target : message.automationEdits) {
      std::optional<std::uint32_t> slot;
      {
        std::scoped_lock resources(processingResourcesMutex_);
        slot = automationBindings_.findActiveSlot(
            {target.pipeline, target.pluginId, target.pluginType,
             target.parameterKey, target.elementIndex});
      }
      if (slot.has_value()) {
        const auto parameterId = automationParameterId(*slot);
        (void)openHostGesture(*slot, parameterId,
                              SingleComponentEffect::getParamNormalized(parameterId));
      }
    }
    return bridgeResult(true);
  }

  if (message.action == UiAction::endAutomationGesture) {
    // The user released the pointer. Every target the gesture moved has to stop
    // being reported as held, or the host keeps believing the hand is on the
    // control and the block keeps ignoring the automation it plays back. A
    // target that never claimed a lane has nothing open and is simply skipped.
    //
    // One release of a linked control closes several touches at once, and the
    // host has to see them close together: a group is opened for a batch that
    // carries more than one target so every endEdit lands at one timestamp. The
    // width of the batch is the criterion rather than the number of touches
    // actually found open, which is only known target by target once the loop is
    // already running -- a group that turns out to contain one close or none is
    // harmless, whereas a single-target release earns no group at all.
    const ScopedHostGroupEdit group{*this, message.automationEdits.size() > 1u,
                                    traceInstance_};
    for (const auto &target : message.automationEdits) {
      std::optional<std::uint32_t> slot;
      {
        std::scoped_lock resources(processingResourcesMutex_);
        slot = automationBindings_.findActiveSlot(
            {target.pipeline, target.pluginId, target.pluginType,
             target.parameterKey, target.elementIndex});
      }
      if (slot.has_value()) {
        (void)closeHostGesture(*slot);
      }
    }
    return bridgeResult(true);
  }

  if (message.action == UiAction::setOversampling) {
    OversamplingSettings previousOversampling;
    const auto previousProcessingReady =
        processingReady_.load(std::memory_order_seq_cst);
    {
      std::scoped_lock stateLock(stateMutex_);
      previousOversampling = state_.oversampling;
      state_.oversampling = message.oversampling;
    }
    const auto previousLatency = latencySamples_.load(std::memory_order_acquire);
    if (maxHostFrames_.load(std::memory_order_acquire) > 0) {
      if (!reconfigureDspPreservingPipeline(
              hostSampleRate_.load(std::memory_order_acquire),
              maxHostFrames_.load(std::memory_order_acquire),
              configuredChannels_.load(std::memory_order_acquire), &error)) {
        {
          std::scoped_lock stateLock(stateMutex_);
          state_.oversampling = previousOversampling;
        }
        std::string restoreError;
        AutomationResourceLock resources{*this};
        EngineMutationWindow restoreWindow{*this};
        const auto restored = configureDspLocked(
            resources, &restoreError,
            /*waitForUiRepack=*/!previousProcessingReady);
        restoreWindow.setRestoreOnClose(previousProcessingReady && restored);
        return bridgeResult(false, error);
      }
      notifyLatencyChange(previousLatency);
    }
    auto result = choc::value::createObject({});
    result.addMember("ok", true);
    result.addMember("success", true);
    {
      std::scoped_lock resources(processingResourcesMutex_);
      if (std::any_of(runtimePlugins_.begin(), runtimePlugins_.end(),
                      [](const RuntimePlugin &runtime) {
                        return runtime.contextuallyBypassed;
                      })) {
        result.addMember("skippedUnsupported", true);
      }
    }
    appendExecutionStates(result);
    return choc::json::toString(result);
  }

  const auto decodePipeline = [this](std::vector<RoutedPlugin> &source,
                                     PipelineState &pipeline,
                                     std::vector<RuntimePlugin> &runtimes) {
    pipeline.plugins.reserve(source.size());
    runtimes.reserve(source.size());
    for (auto &plugin : source) {
      if (!plugin.runtime.type.empty() &&
          engine_.kernels().contains(plugin.runtime.type)) {
        runtimes.push_back(std::move(plugin.runtime));
      } else if (!isSectionPlugin(plugin.logical)) {
        plugin.logical.unknown = true;
      }
      pipeline.plugins.push_back(std::move(plugin.logical));
    }
  };
  // A preset load or an undo carries values the user explicitly asked for, so a
  // bound target it names must follow the payload rather than be overlaid with
  // the value automation last played. Adopting before the overlay is what makes
  // that work: the overlay then writes back the value the payload already
  // holds, while every target the message does not name keeps being overlaid.
  // Only slots that are already bound are adopted -- a bulk message never
  // claims an automation lane the user did not ask for. The adoption, the
  // overlay that reads its result and the state document it produces are one
  // transaction: the caller holds processingResourcesMutex_ across all three,
  // so the drain cannot land between them and put the value automation last
  // played back over the one the payload named.
  const auto adoptNamedAutomationEditsLocked =
      [this](const std::vector<RoutedAutomationEdit> &edits) {
    for (const auto &edit : edits) {
      const auto slot = automationBindings_.findActiveSlot(
          {edit.pipeline, edit.pluginId, edit.pluginType, edit.parameterKey,
           edit.elementIndex});
      if (slot.has_value()) {
        adoptAutomationEditLocked(*slot, edit.normalized);
      }
    }
  };
  const auto normalizedForBulkTarget = [&message](
                                           const AutomationTargetIdentity &identity,
                                           const double current) {
    auto normalized = current;
    for (const auto &edit : message.automationEdits) {
      if (identity == AutomationTargetIdentity{
                          edit.pipeline, edit.pluginId, edit.pluginType,
                          edit.parameterKey, edit.elementIndex}) {
        normalized = edit.normalized;
      }
    }
    return normalized;
  };
  const auto overlayBoundPipelineValues = [this, &normalizedForBulkTarget](
                                               const char side,
                                               PipelineState &pipeline) {
    PluginStateDocument authority;
    auto &authorityPipeline = side == 'B' ? authority.pipelineB : authority.pipelineA;
    authorityPipeline = std::move(pipeline);
    for (const auto slot : automationBindings_.activeSlots()) {
      const auto *binding = automationBindings_.binding(slot);
      const auto *target = automationBindings_.slot(slot);
      if (binding != nullptr && target != nullptr && binding->pipeline == side) {
        (void)applyAutomationValue(
            authority, *binding,
            normalizedForBulkTarget(target->identity, target->currentNormalized));
      }
    }
    pipeline = std::move(authorityPipeline);
  };
  const auto overlayBoundRuntimeValues =
      [this, &normalizedForBulkTarget](const char side,
                                      std::vector<RuntimePlugin> &runtimes) {
    for (const auto slot : automationBindings_.activeSlots()) {
      const auto *binding = automationBindings_.binding(slot);
      const auto *target = automationBindings_.slot(slot);
      if (binding == nullptr || target == nullptr || binding->pipeline != side ||
          target->applyKind != AutomationApplyKind::packedParameter) {
        continue;
      }
      const auto runtime = std::find_if(
          runtimes.begin(), runtimes.end(), [binding](const RuntimePlugin &candidate) {
            return candidate.logicalId == binding->pluginId;
          });
      const auto packed = denormalizeAutomationPackedValue(
          *target,
          normalizedForBulkTarget(target->identity, target->currentNormalized));
      if (runtime != runtimes.end() && packed.has_value() &&
          target->packedOffset < runtime->packedParameters.size()) {
        runtime->packedParameters[target->packedOffset] = *packed;
      }
    }
  };
  const auto restorePreviousPlayableGeneration =
      [this](EngineMutationWindow &engineWindow,
             const PipelineState &previousPipeline) {
        if (!engineWindow.wasReady()) {
          engineWindow.setRestoreOnClose(false);
          return;
        }
        std::string restoreError;
        auto restoredLatencyChanged = false;
        if (engine_.rebuild(previousPipeline, runtimePlugins_, &restoreError) &&
            synchronizeLatencyLocked(restoredLatencyChanged)) {
          engineWindow.setRestoreOnClose(true);
          return;
        }
        engineWindow.setRestoreOnClose(false);
        recordProcessTransactionFailure(
            ProcessTransactionError::processingNotReady);
      };

  if (message.action == UiAction::rebuildPipeline) {
    PipelineState pipeline;
    std::vector<RuntimePlugin> runtimes;
    decodePipeline(message.plugins, pipeline, runtimes);
    pauseBulkRequestBeforeCommitForTesting();
    auto contextuallyUnsupported = false;
    if (stateReplacementPending_.load(std::memory_order_acquire)) {
      const auto previousLatency = latencySamples_.load(std::memory_order_acquire);
      int32 automationRestartFlags = 0;
      {
        // setState() retained one complete old generation. The decoded
        // document, replacement runtime image, bindings, scheduler, apply table
        // and engine become live together inside this single quiet window.
        AutomationResourceLock resources{*this};
        if (!authorizedReplacementRequestLocked()) {
          return bridgeResult(true);
        }
        EngineMutationWindow engineWindow{*this};
        const AudioTimelineWindow timelineWindow{*this};
        acknowledgePublishedAutomationLocked();
        clearPendingControllerWritesLocked();
        {
          std::scoped_lock deltaLock(automationDeltaMutex_);
          pendingAutomationDeltaDirty_.reset();
        }
        automationDeltaPending_.store(false, std::memory_order_release);
        parameterMailbox_.discardPending();
        discardPendingDescriptorLocked();
        {
          std::scoped_lock stateLock(stateMutex_);
          const auto &preserved =
              message.pipeline == 'B' ? state_.pipelineB : state_.pipelineA;
          auto &preserveMissing = message.pipeline == 'B'
                                      ? preserveMissingPipelineB_
                                      : preserveMissingPipelineA_;
          pipeline = undoOpaqueState_.reconcile(
              message.pipeline, preserved, std::move(pipeline), preserveMissing);
          preserveMissing = false;
          state_.currentPipeline = message.pipeline;
          (message.pipeline == 'B' ? state_.pipelineB : state_.pipelineA) =
              pipeline;
          if (message.pipeline == 'B') {
            state_.pipelineBInitialized = true;
          }
          hasSavedState_ = true;
          bypass_.store(state_.masterBypass, std::memory_order_release);
        }
        activePipeline_.store(message.pipeline, std::memory_order_release);
        runtimePlugins_ = std::move(runtimes);
        runtimeParameterDirty_.fill(false);
        runtimeFullImageDirty_.fill(false);
        automationRestartFlags = reconcileAutomationBindingsLocked(
            resources, /*forceCurrentInitialization=*/true);
        overlayBoundRuntimeValues(message.pipeline, runtimePlugins_);
        publishAutomationApplyTableLocked();
        engine_.retainAssets(pruneAssetTransfers());
        processedHostFrames_ = 0;
        previousProjectTimeValid_ = false;
        previousPlaying_ = false;
        previousCycleActive_ = false;
        automationScheduler_.reset();
        outputTransition_.reset();
        if (maxHostFrames_.load(std::memory_order_acquire) > 0 &&
            !configureDspLocked(resources, &error,
                                /*waitForUiRepack=*/false)) {
          engineWindow.setRestoreOnClose(false);
          return bridgeResult(false, error);
        }
        contextuallyUnsupported = std::any_of(
            runtimePlugins_.begin(), runtimePlugins_.end(),
            [](const RuntimePlugin &runtime) {
              return runtime.contextuallyBypassed;
            });
        stateReplacementPending_.store(false, std::memory_order_release);
        replacementPageAuthorized_ = false;
        engineWindow.setRestoreOnClose(
            maxHostFrames_.load(std::memory_order_acquire) > 0);
      }
      if (automationRestartFlags != 0) {
        if (auto *handler = getComponentHandler(); handler != nullptr) {
          traceRestartComponent(*handler, automationRestartFlags, traceInstance_);
        }
      }
      notifyLatencyChange(previousLatency);
      auto result = choc::value::createObject({});
      result.addMember("ok", true);
      result.addMember("success", true);
      result.addMember(
          "skippedUnsupported",
          contextuallyUnsupported ||
              std::any_of(pipeline.plugins.begin(), pipeline.plugins.end(),
                          [](const PluginState &plugin) { return plugin.unknown; }));
      appendExecutionStates(result);
      appendActiveAutomationSnapshot(result);
      return choc::json::toString(result);
    }
    auto skippedUnsupported = std::any_of(
        pipeline.plugins.begin(), pipeline.plugins.end(),
        [](const PluginState &plugin) { return plugin.unknown; });
    auto latencyChanged = false;
    int32 automationRestartFlags = 0;
    {
      AutomationResourceLock resources{*this};
      if (requestStartedDuringStateReplacement ||
          stateReplacementPending_.load(std::memory_order_acquire) ||
          bulkRequestCrossedGenerationLocked()) {
        return bridgeResult(true);
      }
      // Closing the gate and proving the audio thread out of process() belong
      // inside this lock: a window opened before it would be reopened by
      // whichever control thread already held the lock, and the instances and
      // the runtime image below would then be rewritten with the gate open. The
      // window also claims the engine, which is what keeps the one block an
      // explicit topology edit costs from reporting itself as a DSP failure.
      EngineMutationWindow engineWindow{*this};
      PipelineState previousPipeline;
      auto candidateUndoOpaqueState = undoOpaqueState_;
      {
        std::scoped_lock stateLock(stateMutex_);
        previousPipeline = state_.currentPipeline == 'B' ? state_.pipelineB
                                                         : state_.pipelineA;
        const auto &preserved =
            message.pipeline == 'B' ? state_.pipelineB : state_.pipelineA;
        const auto preserveMissing =
            message.pipeline == 'B' ? preserveMissingPipelineB_
                                    : preserveMissingPipelineA_;
        pipeline = candidateUndoOpaqueState.reconcile(
            message.pipeline, preserved, std::move(pipeline), preserveMissing);
        overlayBoundPipelineValues(message.pipeline, pipeline);
      }
      overlayBoundRuntimeValues(message.pipeline, runtimes);
      const auto executionContext = readHostContext();
      contextuallyUnsupported = refreshRuntimeExecutionAdmission(
          pipeline, runtimes, executionContext.engineSampleRate,
          executionContext.channels);
      skippedUnsupported = skippedUnsupported || contextuallyUnsupported;
      if (!engine_.rebuild(pipeline, runtimes, &error)) {
        restorePreviousPlayableGeneration(engineWindow, previousPipeline);
        return bridgeResult(false, error);
      }
      if (!synchronizeLatencyLocked(latencyChanged)) {
        restorePreviousPlayableGeneration(engineWindow, previousPipeline);
        return bridgeResult(false, "Unable to prepare the master-bypass delay");
      }
      // Only a fully rebuilt and latency-valid engine may become the logical,
      // automation and runtime authority. A refused image leaves every old
      // control-side generation untouched and is rebuilt above before the gate
      // can reopen.
      adoptNamedAutomationEditsLocked(message.automationEdits);
      undoOpaqueState_ = std::move(candidateUndoOpaqueState);
      {
        std::scoped_lock stateLock(stateMutex_);
        state_.currentPipeline = message.pipeline;
        (message.pipeline == 'B' ? state_.pipelineB : state_.pipelineA) =
            pipeline;
        if (message.pipeline == 'B') {
          state_.pipelineBInitialized = true;
        }
        (message.pipeline == 'B' ? preserveMissingPipelineB_
                                 : preserveMissingPipelineA_) = false;
        hasSavedState_ = true;
      }
      activePipeline_.store(message.pipeline, std::memory_order_release);
      runtimePlugins_ = std::move(runtimes);
      automationRestartFlags = reconcileAutomationBindingsLocked(resources);
      publishAutomationApplyTableLocked();
      runtimeParameterDirty_.fill(false);
      runtimeFullImageDirty_.fill(false);
      parameterMailbox_.discardPending();
      discardPendingDescriptorLocked();
      // Asset eviction is destructive too, so it follows the commit rather
      // than making a failed candidate unable to restore its old instances.
      engine_.retainAssets(pruneAssetTransfers());
      // This is the path that first makes the DSP playable after a UI repack,
      // so it opens the gate rather than restoring what it found.
      engineWindow.setRestoreOnClose(true);
    }
    if (automationRestartFlags != 0) {
      if (auto *handler = getComponentHandler(); handler != nullptr) {
        traceRestartComponent(*handler, automationRestartFlags, traceInstance_);
      }
    }
    if (latencyChanged) {
      queueLatencyNotification(true);
    }
    auto result = choc::value::createObject({});
    result.addMember("ok", true);
    result.addMember("success", true);
    result.addMember("skippedUnsupported", skippedUnsupported);
    appendExecutionStates(result);
    appendActiveAutomationSnapshot(result);
    return choc::json::toString(result);
  }

  if (message.action == UiAction::restoreHistory) {
    PipelineState pipelineA;
    PipelineState pipelineB;
    std::vector<RuntimePlugin> runtimesA;
    std::vector<RuntimePlugin> runtimesB;
    decodePipeline(message.pipelineA, pipelineA, runtimesA);
    auto contextuallyUnsupportedA = false;
    auto contextuallyUnsupportedB = false;
    if (message.pipelineBInitialized) {
      decodePipeline(message.pipelineB, pipelineB, runtimesB);
    }
    pauseBulkRequestBeforeCommitForTesting();

    PipelineState active;
    std::vector<RuntimePlugin> activeRuntimes;
    auto latencyChanged = false;
    int32 automationRestartFlags = 0;
    {
      AutomationResourceLock resources{*this};
      if (requestStartedDuringStateReplacement ||
          stateReplacementPending_.load(std::memory_order_acquire) ||
          bulkRequestCrossedGenerationLocked()) {
        return bridgeResult(true);
      }
      // Same window as pipeline/rebuild, and for the same two reasons: the gate
      // may only be closed under this lock, and the block the edit costs is the
      // user's own operation rather than a DSP failure to report.
      EngineMutationWindow engineWindow{*this};
      PipelineState previousPipeline;
      auto candidateUndoOpaqueState = undoOpaqueState_;
      {
        std::scoped_lock stateLock(stateMutex_);
        previousPipeline = state_.currentPipeline == 'B' ? state_.pipelineB
                                                         : state_.pipelineA;
        pipelineA = candidateUndoOpaqueState.reconcile(
            'A', state_.pipelineA, std::move(pipelineA), false);
        if (message.pipelineBInitialized) {
          pipelineB = candidateUndoOpaqueState.reconcile(
              'B', state_.pipelineB, std::move(pipelineB), false);
        } else {
          (void)candidateUndoOpaqueState.reconcile(
              'B', state_.pipelineB, {}, false);
        }
        overlayBoundPipelineValues('A', pipelineA);
        if (message.pipelineBInitialized) {
          overlayBoundPipelineValues('B', pipelineB);
        }
        active = message.pipeline == 'B' ? pipelineB : pipelineA;
      }
      activeRuntimes = message.pipeline == 'B' ? std::move(runtimesB)
                                               : std::move(runtimesA);
      overlayBoundRuntimeValues(message.pipeline, activeRuntimes);
      // Decoding happens before this transaction, but admission belongs here,
      // after reconciliation and against the fresh host context, with the
      // runtime generation that is actually rebuilt.
      const auto executionContext = readHostContext();
      if (message.pipeline == 'B') {
        contextuallyUnsupportedA = refreshRuntimeExecutionAdmission(
            pipelineA, runtimesA, executionContext.engineSampleRate,
            executionContext.channels);
        contextuallyUnsupportedB = refreshRuntimeExecutionAdmission(
            pipelineB, activeRuntimes, executionContext.engineSampleRate,
            executionContext.channels);
      } else {
        contextuallyUnsupportedA = refreshRuntimeExecutionAdmission(
            pipelineA, activeRuntimes, executionContext.engineSampleRate,
            executionContext.channels);
        if (message.pipelineBInitialized) {
          contextuallyUnsupportedB = refreshRuntimeExecutionAdmission(
              pipelineB, runtimesB, executionContext.engineSampleRate,
              executionContext.channels);
        }
      }
      if (!engine_.rebuild(active, activeRuntimes, &error)) {
        restorePreviousPlayableGeneration(engineWindow, previousPipeline);
        return bridgeResult(false, error);
      }
      if (!synchronizeLatencyLocked(latencyChanged)) {
        restorePreviousPlayableGeneration(engineWindow, previousPipeline);
        return bridgeResult(false, "Unable to prepare the master-bypass delay");
      }
      adoptNamedAutomationEditsLocked(message.automationEdits);
      undoOpaqueState_ = std::move(candidateUndoOpaqueState);
      {
        std::scoped_lock stateLock(stateMutex_);
        state_.pipelineA = pipelineA;
        state_.pipelineB = message.pipelineBInitialized ? pipelineB
                                                        : PipelineState{};
        state_.pipelineBInitialized = message.pipelineBInitialized;
        state_.currentPipeline = message.pipeline;
        preserveMissingPipelineA_ = false;
        preserveMissingPipelineB_ = false;
        hasSavedState_ = true;
      }
      activePipeline_.store(message.pipeline, std::memory_order_release);
      runtimePlugins_ = std::move(activeRuntimes);
      automationRestartFlags = reconcileAutomationBindingsLocked(resources);
      publishAutomationApplyTableLocked();
      runtimeParameterDirty_.fill(false);
      runtimeFullImageDirty_.fill(false);
      parameterMailbox_.discardPending();
      discardPendingDescriptorLocked();
      engine_.retainAssets(pruneAssetTransfers());
      engineWindow.setRestoreOnClose(true);
    }
    if (automationRestartFlags != 0) {
      if (auto *handler = getComponentHandler(); handler != nullptr) {
        traceRestartComponent(*handler, automationRestartFlags, traceInstance_);
      }
    }
    if (latencyChanged) {
      queueLatencyNotification(true);
    }
    auto result = choc::value::createObject({});
    result.addMember("ok", true);
    result.addMember("success", true);
    const auto skippedUnsupported =
        contextuallyUnsupportedA || contextuallyUnsupportedB ||
        std::any_of(pipelineA.plugins.begin(), pipelineA.plugins.end(),
                    [](const PluginState &plugin) { return plugin.unknown; }) ||
        std::any_of(pipelineB.plugins.begin(), pipelineB.plugins.end(),
                    [](const PluginState &plugin) { return plugin.unknown; });
    result.addMember("skippedUnsupported", skippedUnsupported);
    appendExecutionStates(result);
    appendActiveAutomationSnapshot(result);
    return choc::json::toString(result);
  }

  if (message.action == UiAction::updatePlugin && !message.plugins.empty()) {
    auto &update = message.plugins.front();
    std::uint64_t updateStateEpoch = 0;
    std::vector<std::optional<std::uint32_t>> updateGestureSlots(
        message.automationEdits.size());
    // Callers hold processingResourcesMutex_. The slot snapshot belongs to the
    // same generation as the image decision: if a later restore replaces that
    // generation, these are the only lanes whose already-open host touches a
    // stale edit may still report into.
    const auto captureUpdateGenerationLocked = [&] {
      updateStateEpoch =
          stateReplacementEpoch_.load(std::memory_order_acquire);
      for (std::size_t index = 0; index < message.automationEdits.size(); ++index) {
        const auto &edit = message.automationEdits[index];
        updateGestureSlots[index] = automationBindings_.findActiveSlot(
            {edit.pipeline, edit.pluginId, edit.pluginType, edit.parameterKey,
             edit.elementIndex});
      }
    };
    const auto pauseBeforeAutomationEditsForTesting = [&] {
#if defined(EFFETUNE_PROCESSOR_TEST_HOOKS)
      if (pausePluginUpdateBeforeAutomationEditsForTesting_.load(
              std::memory_order_acquire)) {
        pluginUpdatePausedBeforeAutomationEditsForTesting_.store(
            true, std::memory_order_release);
        while (pausePluginUpdateBeforeAutomationEditsForTesting_.load(
            std::memory_order_acquire)) {
          std::this_thread::yield();
        }
        pluginUpdatePausedBeforeAutomationEditsForTesting_.store(
            false, std::memory_order_release);
      }
#endif
    };
    // Every way this message can be accepted ends here, so the gestures the UI
    // bundled with the image are written to the host in one place: after the
    // image itself reached the DSP, and in the order the user made them. The
    // bulk routes adopt only bound targets, but an ordinary plug-in update is a
    // user gesture, so an unbound target still claims a lane through the Write
    // gate. The one exception is a frame made stale by state replacement: its
    // captured slot may only finish reporting an already-open host touch and
    // never reaches the normal bind/adopt path. An entry that clears
    // bindIfUnbound claims nothing.
    // Nothing about the edits is answered: each bound one becomes the plug-in's
    // own value whether or not the host takes it, so there is nothing for the UI
    // to reconcile and no entry it could be asked to put back.
    const auto updateAccepted =
        [this, &message, &update, &updateGestureSlots,
         &updateStateEpoch,
         &pauseBeforeAutomationEditsForTesting](const bool rebuildAssets,
                                                const bool imageAlreadyStale) {
      auto result = choc::value::createObject({});
      result.addMember("ok", true);
      result.addMember("success", true);
      if (update.runtime.contextuallyBypassed) {
        result.addMember("skippedUnsupported", true);
      }
      if (rebuildAssets) {
        result.addMember("rebuildAssets", true);
      }
      const auto finishResult = [this, &result] {
        appendExecutionStates(result);
        return choc::json::toString(result);
      };
      const auto reportStaleEdits = [&](const std::size_t first) {
        for (auto index = first; index < message.automationEdits.size(); ++index) {
          if (!updateGestureSlots[index].has_value()) {
            continue;
          }
          const auto &edit = message.automationEdits[index];
          reportHostEditToOpenGesture(
              automationParameterId(*updateGestureSlots[index]), edit.normalized,
              edit.endGesture);
        }
      };
      auto generationCrossed = imageAlreadyStale;
      {
        std::scoped_lock resources(processingResourcesMutex_);
        generationCrossed = generationCrossed ||
                            stateReplacementEpoch_.load(
                                std::memory_order_acquire) != updateStateEpoch;
      }
      // Test the boundary after the first epoch comparison, not merely the gap
      // before this function. Every actual side effect below performs its own
      // under-lock comparison too, so a restore here or between batch members
      // cannot turn a stale edit current again.
      if (!imageAlreadyStale) {
        pauseBeforeAutomationEditsForTesting();
        std::scoped_lock resources(processingResourcesMutex_);
        generationCrossed = generationCrossed ||
                            stateReplacementEpoch_.load(
                                std::memory_order_acquire) != updateStateEpoch;
      }
      if (generationCrossed) {
        // The plug-in image and every value-adoption side effect belong to a
        // generation that no longer exists. Preserve only host reporting for a
        // touch that was both bound in that generation and is still open now;
        // never open one, claim a slot, or touch restored native authority.
        reportStaleEdits(0);
        return finishResult();
      }
      // Every lane the batch needs is claimed ahead of the host group. The
      // expected epoch is rechecked inside each resolve/bind transaction, so a
      // restore between entries prevents every later reservation.
      for (std::size_t index = 0; index < message.automationEdits.size(); ++index) {
        const auto &edit = message.automationEdits[index];
        updateGestureSlots[index] = resolveAutomationSlot(
            {edit.pipeline, edit.pluginId, edit.pluginType, edit.parameterKey,
             edit.elementIndex},
            edit.normalized, edit.bindIfUnbound, updateStateEpoch);
        auto staleAfterResolve = false;
        {
          std::scoped_lock resources(processingResourcesMutex_);
          staleAfterResolve =
              stateReplacementEpoch_.load(std::memory_order_acquire) !=
              updateStateEpoch;
        }
        if (staleAfterResolve) {
          reportStaleEdits(0);
          return finishResult();
        }
      }

      // The shim coalesces per plug-in per animation frame, so this batch is
      // one frame of one drag on one plug-in: exactly the run the SDK's group
      // edit is for. Binding has already finished above, outside the group.
      const ScopedHostGroupEdit group{*this, message.automationEdits.size() > 1u,
                                      traceInstance_};
      for (std::size_t index = 0; index < message.automationEdits.size(); ++index) {
        if (!updateGestureSlots[index].has_value()) {
          continue;
        }
        const auto slot = *updateGestureSlots[index];
        const auto &edit = message.automationEdits[index];
        const AutomationTargetIdentity identity{
            edit.pipeline, edit.pluginId, edit.pluginType, edit.parameterKey,
            edit.elementIndex};
        auto staleBeforeHostCall = false;
        {
          std::scoped_lock resources(processingResourcesMutex_);
          const auto *target = automationBindings_.slot(slot);
          staleBeforeHostCall =
              stateReplacementEpoch_.load(std::memory_order_acquire) !=
                  updateStateEpoch ||
                                target == nullptr ||
                                target->identity != identity;
        }
        if (staleBeforeHostCall) {
          reportStaleEdits(index);
          return finishResult();
        }

        (void)performHostEditTransaction(automationParameterId(slot),
                                         edit.normalized, edit.beginGesture,
                                         edit.endGesture);
        auto staleAfterHostCall = false;
        {
          std::scoped_lock resources(processingResourcesMutex_);
          const auto *target = automationBindings_.slot(slot);
          if (stateReplacementEpoch_.load(std::memory_order_acquire) ==
                  updateStateEpoch &&
              target != nullptr &&
              target->identity == identity) {
            adoptAutomationEditLocked(slot, edit.normalized);
          } else {
            // performHostEditTransaction updates the host-facing Parameter
            // before notifying the host. Put that display value back without
            // adopting it into any native authority when a restore crossed the
            // host call itself.
            if (target != nullptr) {
              automationParameters_.setHostAdoptedValue(
                  slot, target->currentNormalized);
            } else {
              (void)SingleComponentEffect::setParamNormalized(
                  automationParameterId(slot), 0.0);
            }
            staleAfterHostCall = true;
          }
        }
        if (staleAfterHostCall) {
          // setState() closes every touch before incrementing the epoch. Any
          // touch still open after the mismatch was opened by this stale host
          // transaction after that close and must not outlive it.
          (void)closeHostGesture(slot);
          reportStaleEdits(index + 1u);
          return finishResult();
        }
      }
      return finishResult();
    };
    // setState() has already made the decoded document authoritative, while
    // deliberately leaving one complete old runtime generation alive until the
    // reloaded page supplies its replacement. The dying page can still flush a
    // final plug-in image in that interval. It is stale by definition and may
    // not overwrite the restored document, runtime shadow or pending engine
    // work. Its bundled gesture may only finish reporting an old-generation
    // host touch that was already open; it may not open one or adopt into the
    // restored binding/scheduler state.
    {
      std::unique_lock resources(processingResourcesMutex_);
      if (stateReplacementPending_.load(std::memory_order_acquire)) {
        captureUpdateGenerationLocked();
        resources.unlock();
        return updateAccepted(false, /*imageAlreadyStale=*/true);
      }
    }
    // Nothing is committed while the update can still be refused. The UI rolls
    // an update that fails back without republishing it, so a state document or
    // a runtime image that already carries the new value would be stranded
    // ahead of both the editor and the host.
    bool topologyChanged = true;
    PipelineState current;
    // The topology the engine is running now. A rebuild that fails takes the
    // instances with it, so the rollback needs it to put the engine back.
    PipelineState previous;
    bool activePipeline = false;
    auto replacement = update.logical;
    {
      std::scoped_lock stateLock(stateMutex_);
      const auto &pipeline = message.pipeline == 'B' ? state_.pipelineB : state_.pipelineA;
      const auto found = std::find_if(pipeline.plugins.begin(), pipeline.plugins.end(),
                                      [&update](const PluginState &plugin) {
                                        return plugin.id == update.logical.id;
                                      });
      if (found != pipeline.plugins.end()) {
        topologyChanged = !sameTopology(*found, update.logical);
        replacement.extraJson = mergeExtraJsonObjects(found->extraJson, replacement.extraJson);
        replacement.parametersJson =
            mergeExtraJsonObjects(found->parametersJson, replacement.parametersJson);
        // A single-plug-in update is a UI gesture, and the gestures it carries
        // adopt the very same values. Overlaying the registry here would replace
        // the gesture with the previous value, so this path lets the gesture
        // stand. The full-rebuild paths still overlay: they adopt only the
        // targets they name, so the registry stays the authority for the rest.
        update.logical.parametersJson = replacement.parametersJson;
      }
      activePipeline = state_.currentPipeline == message.pipeline;
      if (activePipeline) {
        // The pipeline this update would produce. The DSP work below needs it
        // before the document may be written, so it is built as a copy.
        previous = pipeline;
        current = pipeline;
        const auto target = std::find_if(current.plugins.begin(), current.plugins.end(),
                                         [&update](const PluginState &plugin) {
                                           return plugin.id == update.logical.id;
                                         });
        if (target != current.plugins.end()) {
          *target = replacement;
        } else {
          current.plugins.push_back(replacement);
        }
      }
    }

    // Runs once every step that could still fail has succeeded, so the state
    // document only ever holds a value the operation actually delivered.
    // Every caller holds processingResourcesMutex_: setState() publishes its
    // pending marker under that same lock, so the logical image cannot cross a
    // restore after the caller's final under-lock recheck.
    const auto commitPluginUpdate = [&] {
      {
        std::scoped_lock stateLock(stateMutex_);
        auto &pipeline = message.pipeline == 'B' ? state_.pipelineB : state_.pipelineA;
        const auto found = std::find_if(pipeline.plugins.begin(), pipeline.plugins.end(),
                                        [&update](const PluginState &plugin) {
                                          return plugin.id == update.logical.id;
                                        });
        if (found != pipeline.plugins.end()) {
          *found = replacement;
        } else {
          pipeline.plugins.push_back(replacement);
        }
        if (message.pipeline == 'B') {
          state_.pipelineBInitialized = true;
        }
        hasSavedState_ = true;
      }
    };

    if (!activePipeline) {
      std::unique_lock resources(processingResourcesMutex_);
      if (stateReplacementPending_.load(std::memory_order_acquire)) {
        captureUpdateGenerationLocked();
        resources.unlock();
        return updateAccepted(false, /*imageAlreadyStale=*/true);
      }
      commitPluginUpdate();
      captureUpdateGenerationLocked();
      resources.unlock();
      if (topologyChanged) {
        synchronizeAutomationBindings(true);
      }
      return updateAccepted(false, /*imageAlreadyStale=*/false);
    }
    if (update.runtime.type.empty()) {
      std::unique_lock resources(processingResourcesMutex_);
      if (stateReplacementPending_.load(std::memory_order_acquire)) {
        captureUpdateGenerationLocked();
        resources.unlock();
        return updateAccepted(false, /*imageAlreadyStale=*/true);
      }
      if (topologyChanged && processingReady_.load(std::memory_order_seq_cst)) {
        if (!queueDescriptorUpdateLocked(current, &error)) {
          return bridgeResult(false, error);
        }
        armLatencyNotification();
      }
      commitPluginUpdate();
      captureUpdateGenerationLocked();
      resources.unlock();
      if (topologyChanged) {
        synchronizeAutomationBindings(true);
      }
      return updateAccepted(false, /*imageAlreadyStale=*/false);
    }
#if defined(EFFETUNE_PROCESSOR_TEST_HOOKS)
    if (pausePluginUpdateBeforeRuntimeTransactionForTesting_.load(
            std::memory_order_acquire)) {
      pluginUpdatePausedBeforeRuntimeTransactionForTesting_.store(
          true, std::memory_order_release);
      while (pausePluginUpdateBeforeRuntimeTransactionForTesting_.load(
          std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      pluginUpdatePausedBeforeRuntimeTransactionForTesting_.store(
          false, std::memory_order_release);
    }
#endif
    auto latencyChanged = false;
    auto rebuildAssets = false;
    {
      std::unique_lock resources(processingResourcesMutex_);
      // The unlocked fast-path check above avoids decoding work for an update
      // that was already stale. This check is the transaction boundary: a
      // concurrent setState() either publishes first and makes the complete
      // plug-in image a no-op, or waits until this logical/runtime generation
      // has committed and then replaces it as a whole.
      if (stateReplacementPending_.load(std::memory_order_acquire)) {
        captureUpdateGenerationLocked();
        resources.unlock();
        return updateAccepted(false, /*imageAlreadyStale=*/true);
      }
      // Context publication and runtime replacement use this same lock. Reading
      // the context here makes admission, reuse, replacement, and the response
      // one decision instead of allowing setupProcessing() to land between
      // them and have a stale UI update overwrite its new admission state.
      const auto context = readHostContext();
      update.runtime.contextuallyBypassed = !supportsExecutionContext(
          update.runtime.executionCapabilities, replacement.channel,
          context.engineSampleRate, context.channels);
      auto runtime = std::find_if(
          runtimePlugins_.begin(), runtimePlugins_.end(),
          [&update](const RuntimePlugin &plugin) {
            return plugin.logicalId == update.logical.id;
          });
      const auto runtimeMissing = runtime == runtimePlugins_.end();
      if (runtimeMissing && runtimePlugins_.size() >= kMaxPluginInstances) {
        return bridgeResult(false, "Pipeline exceeds 96 native DSP instances");
      }
      const auto canReuseInstance = !runtimeMissing &&
                                    runtime->type == update.runtime.type &&
                                    runtime->paramsHash == update.runtime.paramsHash &&
                                    runtime->tapId == update.runtime.tapId &&
                                    runtime->executionCapabilities ==
                                        update.runtime.executionCapabilities &&
                                    runtime->contextuallyBypassed ==
                                        update.runtime.contextuallyBypassed &&
                                    runtime->packedParameters.size() ==
                                        update.runtime.packedParameters.size() &&
                                    runtime->parameterBytes.size() ==
                                        update.runtime.parameterBytes.size();
      if (!processingReady_.load(std::memory_order_seq_cst)) {
        // Re-seating the runtime image can reallocate the vector a block is
        // walking. The flag being clear says only that somebody else intends to
        // stop the audio thread, not that it has left: safety comes from this
        // thread waiting the block out itself. The window is free here -- the
        // flag is already clear, so nothing is restored when it closes.
        const EngineMutationWindow engineWindow{*this};
        if (runtimeMissing) {
          runtimePlugins_.push_back(update.runtime);
          runtimeParameterDirty_[runtimePlugins_.size() - 1u] = false;
          runtimeFullImageDirty_[runtimePlugins_.size() - 1u] = false;
        } else {
          const auto runtimeIndex =
              static_cast<std::size_t>(runtime - runtimePlugins_.begin());
          *runtime = update.runtime;
          runtimeParameterDirty_[runtimeIndex] = false;
          runtimeFullImageDirty_[runtimeIndex] = false;
        }
        publishAutomationApplyTableLocked();
      } else if (canReuseInstance) {
        if (topologyChanged) {
          if (!queueDescriptorUpdateLocked(current, &error)) {
            return bridgeResult(false, error);
          }
        }
        AudioCommand command;
        command.type = AudioCommandType::setParameters;
        command.logicalId = update.runtime.logicalId;
        command.paramsHash = update.runtime.paramsHash;
        command.floatCount =
            static_cast<std::uint32_t>(update.runtime.packedParameters.size());
        command.parameterByteCount =
            static_cast<std::uint32_t>(update.runtime.parameterBytes.size());
        std::copy(update.runtime.packedParameters.begin(),
                  update.runtime.packedParameters.end(), command.packed.begin());
        std::copy(update.runtime.parameterBytes.begin(),
                  update.runtime.parameterBytes.end(),
                  command.parameterBytes.begin());
        if (!parameterMailbox_.publish(command)) {
          return bridgeResult(false, "Native DSP parameter mailbox is unavailable");
        }
        parameterImageGeneration_.fetch_add(1, std::memory_order_acq_rel);
        armLatencyNotification();
      } else {
        // Same window as the two bulk rebuild routes: the gate may only be
        // closed under this lock, and the single block this topology edit costs
        // is the user's own operation rather than a DSP failure to report.
        EngineMutationWindow engineWindow{*this};
        const auto runtimeIndex =
            static_cast<std::size_t>(runtime - runtimePlugins_.begin());
        RuntimePlugin previousRuntime;
        if (runtimeMissing) {
          runtimePlugins_.push_back(update.runtime);
        } else {
          previousRuntime = std::move(*runtime);
          *runtime = update.runtime;
        }
        publishAutomationApplyTableLocked();
        const auto restoreRuntimeImage = [&] {
          if (runtimeMissing) {
            runtimePlugins_.pop_back();
          } else {
            runtimePlugins_[runtimeIndex] = std::move(previousRuntime);
          }
          publishAutomationApplyTableLocked();
          std::string restoreError;
          auto restoredLatencyChanged = false;
          if (engine_.rebuild(previous, runtimePlugins_, &restoreError) &&
              synchronizeLatencyLocked(restoredLatencyChanged)) {
            engineWindow.setRestoreOnClose(true);
            return;
          }
          engineWindow.setRestoreOnClose(false);
          recordProcessTransactionFailure(
              ProcessTransactionError::processingNotReady);
        };
        if (!engine_.rebuild(current, runtimePlugins_, &error)) {
          restoreRuntimeImage();
          return bridgeResult(false, error);
        }
        if (!synchronizeLatencyLocked(latencyChanged)) {
          restoreRuntimeImage();
          return bridgeResult(false, "Unable to prepare the master-bypass delay");
        }
        parameterMailbox_.discardPending();
        discardPendingDescriptorLocked();
        runtimeParameterDirty_.fill(false);
        runtimeFullImageDirty_.fill(false);
        engineWindow.setRestoreOnClose(true);
        rebuildAssets = true;
      }
      // Publish the matching logical image before releasing the context/runtime
      // transaction. A host reconfiguration takes this lock before it reads
      // state, so it can now observe either the complete old generation or the
      // complete updated one, never the new runtime against the old channel
      // selection.
      commitPluginUpdate();
      captureUpdateGenerationLocked();
    }
    if (latencyChanged) {
      queueLatencyNotification(true);
    }
    if (topologyChanged) {
      synchronizeAutomationBindings(true);
    }
    return updateAccepted(rebuildAssets, /*imageAlreadyStale=*/false);
  }

  return bridgeResult(false, "Bridge message was not handled");
}

std::vector<std::uint32_t> EffeTuneProcessor::pruneAssetTransfers() {
  std::vector<std::uint32_t> retained;
  {
    std::scoped_lock stateLock(stateMutex_);
    retained.reserve(state_.pipelineA.plugins.size() + state_.pipelineB.plugins.size());
    for (const auto &pipeline : {&state_.pipelineA, &state_.pipelineB}) {
      for (const auto &plugin : pipeline->plugins) {
        if (plugin.id != 0) {
          retained.push_back(plugin.id);
        }
      }
    }
  }
  {
    std::scoped_lock transferLock(assetTransferMutex_);
    for (auto pending = pendingAssetTransfers_.begin(); pending != pendingAssetTransfers_.end();) {
      const auto logicalId = static_cast<std::uint32_t>(pending->first >> 32u);
      if (std::find(retained.begin(), retained.end(), logicalId) == retained.end()) {
        pending = pendingAssetTransfers_.erase(pending);
      } else {
        ++pending;
      }
    }
  }
  return retained;
}

} // namespace effetune::vst::plugin
