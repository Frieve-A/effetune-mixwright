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

#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/vstspeaker.h"
#include "public.sdk/source/vst/vstaudioprocessoralgo.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <exception>
#include <limits>
#include <new>
#include <string>
#include <utility>

namespace effetune::vst::plugin {

using namespace Steinberg;
using namespace Steinberg::Vst;

EffeTuneProcessor::EffeTuneProcessor()
    : telemetryScratch_(EngineHost::kDefaultTelemetryBytes) {
  state_.appVersion = EFFETUNE_PLUGIN_VERSION_STR;
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
  addAudioInput(STR16("Main Input"), SpeakerArr::kStereo);
  addAudioOutput(STR16("Main Output"), SpeakerArr::kStereo);
  parameters.addParameter(STR16("Bypass"), nullptr, 1, 0,
                          ParameterInfo::kCanAutomate | ParameterInfo::kIsBypass,
                          kBypassParameterId);
  // The UI also owns the canonical JS parameter packers. Keep it alive while
  // the editor is closed so project state can rebuild a playable pipeline.
  try {
    std::scoped_lock editorLock(editorMutex_);
    webView_ = std::make_unique<WebViewHost>(
        [this](const std::string_view message) { return handleUiMessage(message); });
  } catch (const std::exception &) {
    // A missing WebView runtime must not prevent the audio component loading;
    // attachEditor() retries and the processor continues with its current DSP.
    webView_.reset();
  }
  return kResultOk;
}

tresult PLUGIN_API EffeTuneProcessor::terminate() {
  processingReady_.store(false, std::memory_order_release);
  {
    std::scoped_lock editorLock(editorMutex_);
    webView_.reset();
  }
  std::scoped_lock resources(processingResourcesMutex_);
  engineOutputBuffer_.clear();
  dryTransitionBuffer_.clear();
  return SingleComponentEffect::terminate();
}

tresult PLUGIN_API EffeTuneProcessor::setActive(const TBool state) {
  if (state) {
    std::scoped_lock resources(processingResourcesMutex_);
    blockAdapter_.reset();
    oversampler_.reset();
    engine_.reset();
    dryDelay_.reset();
    engineFramesProcessed_ = 0.0;
    outputTransition_.reset();
    hasProcessedAudio_.store(false, std::memory_order_release);
    topologyDryPending_.store(false, std::memory_order_release);
  }
  return SingleComponentEffect::setActive(state);
}

bool EffeTuneProcessor::configureDsp(std::string *error, const bool waitForUiRepack) {
  std::scoped_lock resources(processingResourcesMutex_);
  parameterMailbox_.discardPending();
  discardAudioCommandsLocked();
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
      if (!engine_.rebuild(pipeline, runtimePlugins_, error)) {
        return false;
      }
    }
    engineOutputBuffer_.assign(
        static_cast<std::size_t>(configuredChannels) * maxEngineFrames, 0.0f);
    dryTransitionBuffer_.assign(
        static_cast<std::size_t>(configuredChannels) * static_cast<std::size_t>(maxHostFrames),
        0.0f);
    for (int32 channel = 0; channel < configuredChannels; ++channel) {
      engineOutputPointers_[static_cast<std::size_t>(channel)] =
          engineOutputBuffer_.data() + static_cast<std::size_t>(channel) * maxEngineFrames;
      dryTransitionPointers_[static_cast<std::size_t>(channel)] =
          dryTransitionBuffer_.data() + static_cast<std::size_t>(channel) * maxHostFrames;
    }
    const auto resamplerLatency = oversampler_.latencyHostFrames();
    const auto totalLatency = calculateTotalLatency(
        resamplerLatency, EngineHost::kQuantumFrames, snapshot.oversampling.factor,
        engine_.pipelineLatency());
    if (!dryDelay_.prepare(static_cast<std::uint32_t>(configuredChannels),
                           static_cast<std::uint32_t>(maxHostFrames), totalLatency)) {
      if (error != nullptr) {
        *error = "Unable to prepare the master-bypass delay";
      }
      return false;
    }
    resamplerLatencySamples_.store(resamplerLatency, std::memory_order_release);
    latencySamples_.store(totalLatency, std::memory_order_release);
    activeOversamplingFactor_.store(snapshot.oversampling.factor, std::memory_order_release);
    engineFramesProcessed_ = 0.0;
    flushTopologyHistory();
    publishHostContext(hostSampleRate, static_cast<std::uint32_t>(configuredChannels),
                       snapshot.oversampling.factor);
    return true;
  } catch (const std::exception &exception) {
    if (error != nullptr) {
      *error = exception.what();
    }
    return false;
  }
}

void EffeTuneProcessor::flushTopologyHistory() noexcept {
  blockAdapter_.reset();
  oversampler_.reset();
  if (hasProcessedAudio_.load(std::memory_order_acquire)) {
    topologyDryPending_.store(true, std::memory_order_release);
  }
}

bool EffeTuneProcessor::queueDescriptorUpdate(const PipelineState &pipeline,
                                              std::string *error) {
  AudioCommand command;
  if (!engine_.makeDescriptorCommand(pipeline, runtimePlugins_, command, error)) {
    return false;
  }
  if (!commandQueue_.push(command)) {
    if (error != nullptr) {
      *error = "Native DSP topology command queue is full";
    }
    return false;
  }
  return true;
}

void EffeTuneProcessor::discardAudioCommandsLocked() noexcept {
  AudioCommand discarded;
  while (commandQueue_.pop(discarded)) {
  }
}

void EffeTuneProcessor::publishHostContext(const double sampleRate, const std::uint32_t channels,
                                           const std::uint32_t oversamplingFactor) noexcept {
  contextSequence_.fetch_add(1, std::memory_order_seq_cst);
  publishedHostSampleRate_.store(sampleRate, std::memory_order_relaxed);
  publishedEngineSampleRate_.store(sampleRate * oversamplingFactor, std::memory_order_relaxed);
  publishedChannels_.store(channels, std::memory_order_relaxed);
  publishedOversamplingFactor_.store(oversamplingFactor, std::memory_order_relaxed);
  contextGeneration_.fetch_add(1, std::memory_order_relaxed);
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
  hostSampleRate_.store(setup.sampleRate, std::memory_order_release);
  maxHostFrames_.store(setup.maxSamplesPerBlock, std::memory_order_release);
  processingReady_.store(false, std::memory_order_release);
  if (!configureDsp(nullptr, true)) {
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
  configuredChannels_.store(channelCount, std::memory_order_release);
  if (maxHostFrames_.load(std::memory_order_acquire) > 0) {
    processingReady_.store(false, std::memory_order_release);
    if (!configureDsp(nullptr, true)) {
      return kResultFalse;
    }
  }
  return kResultTrue;
}

tresult PLUGIN_API EffeTuneProcessor::canProcessSampleSize(const int32 symbolicSampleSize) {
  return symbolicSampleSize == kSample32 ? kResultTrue : kResultFalse;
}

void EffeTuneProcessor::updateBypassFromHost(IParameterChanges *changes) noexcept {
  if (changes == nullptr) {
    return;
  }
  for (int32 index = 0; index < changes->getParameterCount(); ++index) {
    auto *queue = changes->getParameterData(index);
    if (queue == nullptr || queue->getParameterId() != kBypassParameterId ||
        queue->getPointCount() == 0) {
      continue;
    }
    for (int32 point = 0; point < queue->getPointCount(); ++point) {
      int32 sampleOffset = 0;
      ParamValue value = 0.0;
      if (queue->getPoint(point, sampleOffset, value) == kResultTrue) {
        bypass_.store(value > 0.5, std::memory_order_release);
      }
    }
  }
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
  updateBypassFromHost(data.inputParameterChanges);
  if (data.numSamples <= 0 || data.numInputs == 0 || data.numOutputs == 0) {
    return kResultOk;
  }
  auto &input = data.inputs[0];
  auto &output = data.outputs[0];
  const auto maxHostFrames = maxHostFrames_.load(std::memory_order_acquire);
  const auto configuredChannels = configuredChannels_.load(std::memory_order_acquire);
  const auto hostSampleRate = hostSampleRate_.load(std::memory_order_acquire);
  if (data.symbolicSampleSize != kSample32 || data.numSamples > maxHostFrames ||
      input.numChannels != configuredChannels ||
      output.numChannels != configuredChannels || input.channelBuffers32 == nullptr ||
      output.channelBuffers32 == nullptr) {
    copyDry(input, output, data.numSamples);
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

  if (!processingReady_.load(std::memory_order_acquire)) {
    finishDry();
    return kResultOk;
  }

  std::unique_lock resources(processingResourcesMutex_, std::try_to_lock);
  if (!resources.owns_lock()) {
    finishDry();
    return kResultOk;
  }

  copyDryToScratch(input, data.numSamples);
  for (int32 channel = 0; channel < output.numChannels; ++channel) {
    dryPointers[static_cast<std::size_t>(channel)] =
        dryTransitionPointers_[static_cast<std::size_t>(channel)];
  }
  const auto *delayedDry = dryDelay_.process(
      const_cast<const float *const *>(input.channelBuffers32),
      static_cast<std::uint32_t>(input.numChannels),
      static_cast<std::uint32_t>(data.numSamples));
  if (delayedDry == nullptr ||
      topologyDryPending_.exchange(false, std::memory_order_acq_rel)) {
    finishDry();
    return kResultOk;
  }
  const auto masterBypass = bypass_.load(std::memory_order_acquire);
  const auto oversamplingFactor = activeOversamplingFactor_.load(std::memory_order_acquire);

  const auto *upsampled = oversampler_.upsample(
      const_cast<const float *const *>(input.channelBuffers32),
      static_cast<std::uint32_t>(data.numSamples));
  const auto engineFrames =
      static_cast<std::uint32_t>(data.numSamples) * oversamplingFactor;
  const auto adapted = upsampled != nullptr && blockAdapter_.process(
      upsampled, engineOutputPointers_.data(), engineFrames,
      [this, oversamplingFactor, hostSampleRate, masterBypass](float *const *channels,
             const std::uint32_t channelCount,
             const std::uint32_t frames) noexcept {
        const auto time = engineFramesProcessed_ /
                          (hostSampleRate * static_cast<double>(oversamplingFactor));
        const auto result = engine_.tryProcessQuantum(
            channels, channelCount, frames, time,
            masterBypass, &commandQueue_, &parameterMailbox_);
        engineFramesProcessed_ += frames;
        return result;
      });

  if (!adapted || !oversampler_.downsample(
                      const_cast<const float *const *>(engineOutputPointers_.data()), engineFrames,
                      output.channelBuffers32)) {
    blockAdapter_.reset();
    oversampler_.reset();
    restoreDryFromScratch(output, data.numSamples);
    finish(false, dryPointers.data());
    return kResultOk;
  }
  if (masterBypass) {
    for (int32 channel = 0; channel < output.numChannels; ++channel) {
      std::memcpy(output.channelBuffers32[channel], delayedDry[channel],
                  static_cast<std::size_t>(data.numSamples) * sizeof(float));
    }
  }
  finish(true, delayedDry);
  hasProcessedAudio_.store(true, std::memory_order_release);
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
    (void)handler->restartComponent(RestartFlags::kLatencyChanged);
    latencyDebounceArmed_ = false;
    latencyNotificationPending_ = false;
  }
}

void EffeTuneProcessor::armLatencyNotification() {
  latencyDebounceArmed_ = true;
  latencyNotificationDeadline_ =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
}

void EffeTuneProcessor::queueLatencyNotification(const bool restartDebounce) {
  latencyNotificationPending_ = true;
  if (restartDebounce || !latencyDebounceArmed_) {
    armLatencyNotification();
  }
}

bool EffeTuneProcessor::synchronizeLatencyLocked(bool &latencyChanged) {
  const auto next = calculateTotalLatency(
      resamplerLatencySamples_.load(std::memory_order_acquire), EngineHost::kQuantumFrames,
      activeOversamplingFactor_.load(std::memory_order_acquire), engine_.pipelineLatency());
  const auto previous = latencySamples_.load(std::memory_order_acquire);
  if (previous != next && !dryDelay_.setDelay(next)) {
    return false;
  }
  servicedLatencyRevision_ = engine_.latencyRevision();
  latencySamples_.store(next, std::memory_order_release);
  latencyChanged = previous != next;
  return true;
}

void EffeTuneProcessor::serviceLatencyUpdates(const bool restartDebounce) {
  const auto now = std::chrono::steady_clock::now();
  auto latencyChanged = false;
  auto revision = engine_.latencyRevision();
  if (revision != servicedLatencyRevision_) {
    std::scoped_lock resources(processingResourcesMutex_);
    revision = engine_.latencyRevision();
    if (revision != servicedLatencyRevision_) {
      if (!synchronizeLatencyLocked(latencyChanged)) {
        return;
      }
    }
    if (latencyChanged) {
      queueLatencyNotification(restartDebounce);
    }
  }
  if (latencyDebounceArmed_ && !latencyNotificationPending_ &&
      now >= latencyNotificationDeadline_) {
    latencyDebounceArmed_ = false;
  }
  if (latencyDebounceArmed_ && latencyNotificationPending_ &&
      now >= latencyNotificationDeadline_) {
    latencyDebounceArmed_ = false;
    latencyNotificationPending_ = false;
    if (auto *handler = getComponentHandler(); handler != nullptr) {
      (void)handler->restartComponent(RestartFlags::kLatencyChanged);
    }
  }
}

tresult PLUGIN_API EffeTuneProcessor::setState(IBStream *stream) {
  std::string json;
  PluginStateDocument decoded;
  if (!readStream(stream, json) || !StateCodec::decode(json, decoded)) {
    return kResultFalse;
  }
  const auto restoredBypass = decoded.masterBypass;
  {
    std::scoped_lock stateLock(stateMutex_);
    state_ = std::move(decoded);
    undoOpaqueState_.clear();
    preserveMissingPipelineA_ = true;
    preserveMissingPipelineB_ = true;
    hasSavedState_ = true;
  }
  bypass_.store(restoredBypass, std::memory_order_release);
  setParamNormalized(kBypassParameterId, restoredBypass ? 1.0 : 0.0);
  const auto previousLatency = latencySamples_.load(std::memory_order_acquire);
  if (maxHostFrames_.load(std::memory_order_acquire) > 0) {
    processingReady_.store(false, std::memory_order_release);
    if (!configureDsp(nullptr, true)) {
      return kResultFalse;
    }
    notifyLatencyChange(previousLatency);
  }
  {
    std::scoped_lock editorLock(editorMutex_);
    if (webView_ != nullptr) {
      (void)webView_->evaluate("window.location.reload();", {});
    }
  }
  return kResultOk;
}

tresult PLUGIN_API EffeTuneProcessor::getState(IBStream *stream) {
  if (stream == nullptr) {
    return kResultFalse;
  }
  PluginStateDocument snapshot;
  {
    std::scoped_lock stateLock(stateMutex_);
    snapshot = state_;
  }
  snapshot.masterBypass = bypass_.load(std::memory_order_acquire);
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
  return latencySamples_.load(std::memory_order_acquire);
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
  std::scoped_lock editorLock(editorMutex_);
  try {
    if (webView_ == nullptr) {
      webView_ = std::make_unique<WebViewHost>(
          [this](const std::string_view message) { return handleUiMessage(message); });
    }
    return webView_->attach(owner, parent, width, height);
  } catch (const std::exception &) {
    webView_.reset();
    return false;
  }
}

void EffeTuneProcessor::detachEditor(void *owner) noexcept {
  std::scoped_lock editorLock(editorMutex_);
  if (webView_ != nullptr) {
    webView_->detach(owner);
  }
}

void EffeTuneProcessor::resizeEditor(void *owner, const std::int32_t width,
                                     const std::int32_t height) noexcept {
  std::scoped_lock editorLock(editorMutex_);
  if (webView_ != nullptr) {
    webView_->resize(owner, width, height);
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
  RoutedUiMessage message;
  std::string error;
  if (!MessageRouter::decode(request, message, &error)) {
    return bridgeResult(false, error);
  }

  if (message.action == UiAction::hostInfo) {
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
    result.addMember("latencySamples", static_cast<std::int64_t>(latencySamples_.load()));
    result.addMember("masterBypass", bypass_.load(std::memory_order_acquire));
    result.addMember("dspReady", processingReady_.load(std::memory_order_acquire));
    result.addMember("contextGeneration",
                     static_cast<std::int64_t>(context.generation));
    result.addMember("version", std::string(EFFETUNE_PLUGIN_VERSION_STR));
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
    result.addMember("packet", bytes == 0
                                   ? std::string{}
                                   : choc::base64::encodeToString(telemetryScratch_.data(), bytes));
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
    if (!engine_.setAsset(std::move(asset), &error)) {
      return bridgeResult(false, error);
    }
    auto result = choc::value::createObject({});
    result.addMember("ok", true);
    result.addMember("success", true);
    result.addMember("state", static_cast<std::int64_t>(
                                  engine_.assetState(message.asset.logicalId,
                                                     message.asset.slot)));
    return choc::json::toString(result);
  }

  if (message.action == UiAction::clearPluginAsset) {
    {
      std::scoped_lock transferLock(assetTransferMutex_);
      pendingAssetTransfers_.erase(assetKey(message.asset.logicalId, message.asset.slot));
    }
    (void)engine_.clearAsset(message.asset.logicalId, message.asset.slot);
    return bridgeResult(true);
  }

  if (message.action == UiAction::readPluginAssetState) {
    auto result = choc::value::createObject({});
    result.addMember("ok", true);
    result.addMember("state", static_cast<std::int64_t>(
                                  engine_.assetState(message.asset.logicalId,
                                                     message.asset.slot)));
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
      snapshot.masterBypass = bypass_.load(std::memory_order_acquire);
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
    bypass_.store(message.masterBypass, std::memory_order_release);
    {
      std::scoped_lock stateLock(stateMutex_);
      state_.masterBypass = message.masterBypass;
    }
    const auto normalized = message.masterBypass ? 1.0 : 0.0;
    (void)beginEdit(kBypassParameterId);
    setParamNormalized(kBypassParameterId, normalized);
    (void)performEdit(kBypassParameterId, normalized);
    (void)endEdit(kBypassParameterId);
    return bridgeResult(true);
  }

  if (message.action == UiAction::setOversampling) {
    {
      std::scoped_lock stateLock(stateMutex_);
      state_.oversampling = message.oversampling;
    }
    const auto previousLatency = latencySamples_.load(std::memory_order_acquire);
    if (maxHostFrames_.load(std::memory_order_acquire) > 0) {
      processingReady_.store(false, std::memory_order_release);
      if (!configureDsp(&error, true)) {
        return bridgeResult(false, error);
      }
      notifyLatencyChange(previousLatency);
    }
    return bridgeResult(true);
  }

  const auto decodePipeline = [this](std::vector<RoutedPlugin> &source,
                                     PipelineState &pipeline,
                                     std::vector<RuntimePlugin> &runtimes) {
    pipeline.plugins.reserve(source.size());
    runtimes.reserve(source.size());
    for (auto &plugin : source) {
      if (!plugin.runtime.type.empty() && engine_.kernels().contains(plugin.runtime.type)) {
        runtimes.push_back(std::move(plugin.runtime));
      } else if (!isSectionPlugin(plugin.logical)) {
        plugin.logical.unknown = true;
      }
      pipeline.plugins.push_back(std::move(plugin.logical));
    }
  };

  if (message.action == UiAction::rebuildPipeline) {
    PipelineState pipeline;
    std::vector<RuntimePlugin> runtimes;
    decodePipeline(message.plugins, pipeline, runtimes);
    {
      std::scoped_lock stateLock(stateMutex_);
      const auto &preserved = message.pipeline == 'B' ? state_.pipelineB : state_.pipelineA;
      auto &preserveMissing =
          message.pipeline == 'B' ? preserveMissingPipelineB_ : preserveMissingPipelineA_;
      pipeline = undoOpaqueState_.reconcile(message.pipeline, preserved, std::move(pipeline),
                                            preserveMissing);
      preserveMissing = false;
      state_.currentPipeline = message.pipeline;
      (message.pipeline == 'B' ? state_.pipelineB : state_.pipelineA) = pipeline;
      if (message.pipeline == 'B') {
        state_.pipelineBInitialized = true;
      }
      hasSavedState_ = true;
    }
    pruneAssetCache();
    const auto skippedUnsupported =
        std::any_of(pipeline.plugins.begin(), pipeline.plugins.end(),
                    [](const PluginState &plugin) { return plugin.unknown; });
    auto latencyChanged = false;
    processingReady_.store(false, std::memory_order_release);
    {
      std::scoped_lock resources(processingResourcesMutex_);
      parameterMailbox_.discardPending();
      discardAudioCommandsLocked();
      runtimePlugins_ = std::move(runtimes);
      if (!engine_.rebuild(pipeline, runtimePlugins_, &error)) {
        return bridgeResult(false, error);
      }
      if (!synchronizeLatencyLocked(latencyChanged)) {
        return bridgeResult(false, "Unable to prepare the master-bypass delay");
      }
      flushTopologyHistory();
      processingReady_.store(true, std::memory_order_release);
    }
    if (latencyChanged) {
      queueLatencyNotification(true);
    }
    auto result = choc::value::createObject({});
    result.addMember("ok", true);
    result.addMember("success", true);
    result.addMember("skippedUnsupported", skippedUnsupported);
    return choc::json::toString(result);
  }

  if (message.action == UiAction::restoreHistory) {
    PipelineState pipelineA;
    PipelineState pipelineB;
    std::vector<RuntimePlugin> runtimesA;
    std::vector<RuntimePlugin> runtimesB;
    decodePipeline(message.pipelineA, pipelineA, runtimesA);
    if (message.pipelineBInitialized) {
      decodePipeline(message.pipelineB, pipelineB, runtimesB);
    }

    PipelineState active;
    std::vector<RuntimePlugin> activeRuntimes;
    {
      std::scoped_lock stateLock(stateMutex_);
      pipelineA = undoOpaqueState_.reconcile('A', state_.pipelineA, std::move(pipelineA), false);
      if (message.pipelineBInitialized) {
        pipelineB = undoOpaqueState_.reconcile('B', state_.pipelineB, std::move(pipelineB), false);
      } else {
        (void)undoOpaqueState_.reconcile('B', state_.pipelineB, {}, false);
      }
      state_.pipelineA = pipelineA;
      state_.pipelineB = message.pipelineBInitialized ? pipelineB : PipelineState{};
      state_.pipelineBInitialized = message.pipelineBInitialized;
      state_.currentPipeline = message.pipeline;
      preserveMissingPipelineA_ = false;
      preserveMissingPipelineB_ = false;
      hasSavedState_ = true;
      active = message.pipeline == 'B' ? state_.pipelineB : state_.pipelineA;
      activeRuntimes = message.pipeline == 'B' ? std::move(runtimesB) : std::move(runtimesA);
    }
    pruneAssetCache();

    const auto skippedUnsupported =
        std::any_of(pipelineA.plugins.begin(), pipelineA.plugins.end(),
                    [](const PluginState &plugin) { return plugin.unknown; }) ||
        std::any_of(pipelineB.plugins.begin(), pipelineB.plugins.end(),
                    [](const PluginState &plugin) { return plugin.unknown; });
    auto latencyChanged = false;
    processingReady_.store(false, std::memory_order_release);
    {
      std::scoped_lock resources(processingResourcesMutex_);
      parameterMailbox_.discardPending();
      discardAudioCommandsLocked();
      runtimePlugins_ = std::move(activeRuntimes);
      if (!engine_.rebuild(active, runtimePlugins_, &error)) {
        return bridgeResult(false, error);
      }
      if (!synchronizeLatencyLocked(latencyChanged)) {
        return bridgeResult(false, "Unable to prepare the master-bypass delay");
      }
      flushTopologyHistory();
      processingReady_.store(true, std::memory_order_release);
    }
    if (latencyChanged) {
      queueLatencyNotification(true);
    }
    auto result = choc::value::createObject({});
    result.addMember("ok", true);
    result.addMember("success", true);
    result.addMember("skippedUnsupported", skippedUnsupported);
    return choc::json::toString(result);
  }

  if (message.action == UiAction::updatePlugin && !message.plugins.empty()) {
    auto &update = message.plugins.front();
    bool topologyChanged = true;
    PipelineState current;
    bool activePipeline = false;
    {
      std::scoped_lock stateLock(stateMutex_);
      auto &pipeline = message.pipeline == 'B' ? state_.pipelineB : state_.pipelineA;
      const auto found = std::find_if(pipeline.plugins.begin(), pipeline.plugins.end(),
                                      [&update](const PluginState &plugin) {
                                        return plugin.id == update.logical.id;
                                      });
      if (found != pipeline.plugins.end()) {
        topologyChanged = !sameTopology(*found, update.logical);
        auto replacement = update.logical;
        replacement.extraJson = mergeExtraJsonObjects(found->extraJson, replacement.extraJson);
        replacement.parametersJson =
            mergeExtraJsonObjects(found->parametersJson, replacement.parametersJson);
        *found = std::move(replacement);
      } else {
        pipeline.plugins.push_back(update.logical);
      }
      if (message.pipeline == 'B') {
        state_.pipelineBInitialized = true;
      }
      hasSavedState_ = true;
      if (state_.currentPipeline == message.pipeline) {
        current = pipeline;
        activePipeline = true;
      }
    }

    if (!activePipeline) {
      return bridgeResult(true);
    }
    if (update.runtime.type.empty()) {
      if (topologyChanged && processingReady_.load(std::memory_order_acquire)) {
        if (!queueDescriptorUpdate(current, &error)) {
          return bridgeResult(false, error);
        }
        armLatencyNotification();
      }
      return bridgeResult(true);
    }
    const auto runtime = std::find_if(runtimePlugins_.begin(), runtimePlugins_.end(),
                                      [&update](const RuntimePlugin &plugin) {
                                        return plugin.logicalId == update.runtime.logicalId;
                                      });
    const auto canReuseInstance = runtime != runtimePlugins_.end() &&
                                  runtime->type == update.runtime.type &&
                                  runtime->paramsHash == update.runtime.paramsHash &&
                                  runtime->tapId == update.runtime.tapId;
    if (runtime == runtimePlugins_.end()) {
      if (runtimePlugins_.size() >= kMaxPluginInstances) {
        return bridgeResult(false, "Pipeline exceeds 96 native DSP instances");
      }
      runtimePlugins_.push_back(update.runtime);
    } else {
      *runtime = update.runtime;
    }

    if (!processingReady_.load(std::memory_order_acquire)) {
      return bridgeResult(true);
    }
    if (canReuseInstance) {
      AudioCommand command;
      command.type = AudioCommandType::setParameters;
      command.logicalId = update.runtime.logicalId;
      command.paramsHash = update.runtime.paramsHash;
      command.floatCount = static_cast<std::uint32_t>(update.runtime.packedParameters.size());
      command.parameterByteCount =
          static_cast<std::uint32_t>(update.runtime.parameterBytes.size());
      std::copy(update.runtime.packedParameters.begin(), update.runtime.packedParameters.end(),
                command.packed.begin());
      std::copy(update.runtime.parameterBytes.begin(), update.runtime.parameterBytes.end(),
                command.parameterBytes.begin());
      if (!parameterMailbox_.publish(command)) {
        return bridgeResult(false, "Native DSP parameter mailbox is unavailable");
      }
      if (topologyChanged && !queueDescriptorUpdate(current, &error)) {
        return bridgeResult(false, error);
      }
      armLatencyNotification();
      return bridgeResult(true);
    }
    auto latencyChanged = false;
    processingReady_.store(false, std::memory_order_release);
    {
      std::scoped_lock resources(processingResourcesMutex_);
      parameterMailbox_.discardPending();
      discardAudioCommandsLocked();
      if (!engine_.rebuild(current, runtimePlugins_, &error)) {
        return bridgeResult(false, error);
      }
      if (!synchronizeLatencyLocked(latencyChanged)) {
        return bridgeResult(false, "Unable to prepare the master-bypass delay");
      }
      flushTopologyHistory();
      processingReady_.store(true, std::memory_order_release);
    }
    if (latencyChanged) {
      queueLatencyNotification(true);
    }
    auto result = choc::value::createObject({});
    result.addMember("ok", true);
    result.addMember("success", true);
    result.addMember("rebuildAssets", true);
    return choc::json::toString(result);
  }

  return bridgeResult(false, "Bridge message was not handled");
}

void EffeTuneProcessor::pruneAssetCache() {
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
  engine_.retainAssets(retained);
}

} // namespace effetune::vst::plugin
