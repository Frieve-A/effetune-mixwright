#pragma once

// The SDK requires this header to precede other VST declarations.
#include "public.sdk/source/vst/vstsinglecomponenteffect.h"

#include "bridge/state_codec.h"
#include "bridge/config_store.h"
#include "bridge/preset_store.h"
#include "engine/block_adapter.h"
#include "engine/command_queue.h"
#include "engine/dry_delay.h"
#include "engine/engine_host.h"
#include "engine/output_transition.h"
#include "engine/resampler.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace effetune::vst {
class WebViewHost;
}

namespace effetune::vst::plugin {

class EffeTuneProcessor final : public Steinberg::Vst::SingleComponentEffect {
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

  [[nodiscard]] std::string handleUiMessage(std::string_view message);
  [[nodiscard]] bool attachEditor(void *owner, void *parent, std::int32_t width,
                                  std::int32_t height);
  void detachEditor(void *owner) noexcept;
  void resizeEditor(void *owner, std::int32_t width, std::int32_t height) noexcept;

  OBJ_METHODS(EffeTuneProcessor, SingleComponentEffect)
  REFCOUNT_METHODS(SingleComponentEffect)

private:
  struct HostContextSnapshot {
    double sampleRate = 44100.0;
    double engineSampleRate = 44100.0;
    std::uint32_t channels = 2;
    std::uint32_t oversamplingFactor = 1;
    std::uint64_t generation = 1;
  };

  [[nodiscard]] bool configureDsp(std::string *error = nullptr,
                                  bool waitForUiRepack = false);
  void publishHostContext(double sampleRate, std::uint32_t channels,
                          std::uint32_t oversamplingFactor) noexcept;
  [[nodiscard]] HostContextSnapshot readHostContext() const noexcept;
  void updateBypassFromHost(Steinberg::Vst::IParameterChanges *changes) noexcept;
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
  [[nodiscard]] bool synchronizeLatencyLocked(bool &latencyChanged);
  void serviceLatencyUpdates(bool restartDebounce = false);
  void flushTopologyHistory() noexcept;
  [[nodiscard]] bool queueDescriptorUpdate(const PipelineState &pipeline,
                                           std::string *error = nullptr);
  void discardAudioCommandsLocked() noexcept;
  void pruneAssetCache();

  struct PendingAssetTransfer {
    RuntimeAsset asset;
    std::uint64_t operationRevision = 0;
    std::size_t receivedBytes = 0;
  };

  EngineHost engine_;
  BlockAdapter blockAdapter_;
  Oversampler oversampler_;
  DryDelayLine dryDelay_;
  OutputTransition outputTransition_;
  AudioCommandQueue commandQueue_;
  LatestParameterMailbox parameterMailbox_;
  PresetStore presetStore_;
  ConfigStore configStore_;
  PluginStateDocument state_;
  UndoOpaqueStateStore undoOpaqueState_;
  std::string configJson_ = "{}";
  bool preserveMissingPipelineA_ = false;
  bool preserveMissingPipelineB_ = false;
  bool hasSavedState_ = false;
  std::vector<RuntimePlugin> runtimePlugins_;
  std::vector<std::uint8_t> telemetryScratch_;
  std::unordered_map<std::uint64_t, PendingAssetTransfer> pendingAssetTransfers_;
  std::vector<float> engineOutputBuffer_;
  std::vector<float> dryTransitionBuffer_;
  std::array<float *, EngineHost::kMaxChannels> engineOutputPointers_{};
  std::array<float *, EngineHost::kMaxChannels> dryTransitionPointers_{};
  std::atomic<double> hostSampleRate_{44100.0};
  double engineFramesProcessed_ = 0.0;
  std::atomic<Steinberg::int32> maxHostFrames_{0};
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
  std::atomic_bool processingReady_{false};
  std::atomic_bool hasProcessedAudio_{false};
  std::atomic_bool topologyDryPending_{false};
  std::uint64_t servicedLatencyRevision_ = 0;
  bool latencyDebounceArmed_ = false;
  bool latencyNotificationPending_ = false;
  std::chrono::steady_clock::time_point latencyNotificationDeadline_{};
  mutable std::mutex stateMutex_;
  std::mutex processingResourcesMutex_;
  std::mutex editorMutex_;
  std::mutex assetTransferMutex_;
  std::unique_ptr<WebViewHost> webView_;
  std::mutex presetExchangeMutex_;
  std::optional<std::string> readablePresetPath_;
  std::optional<std::string> writablePresetPath_;
};

} // namespace effetune::vst::plugin
