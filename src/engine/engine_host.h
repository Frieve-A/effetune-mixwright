#pragma once

#include "effetune/abi.h"
#include "engine/command_queue.h"
#include "engine/pipeline_model.h"
#include "engine/spectrum_capture.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace effetune {
class Engine;
}

namespace effetune::vst {
namespace plugin { class PluginProcessorTestAccess; }
class EngineHostTestAccess;

struct KernelInfo {
  std::uint32_t index = 0;
  std::uint32_t paramsHash = 0;
  std::uint32_t parameterBytesCapacity = 0;
  std::uint32_t assetCapacity = 0;
};

struct RuntimePlugin {
  std::uint32_t logicalId = 0;
  std::string type;
  std::vector<float> packedParameters;
  std::uint32_t paramsHash = 0;
  std::vector<std::uint8_t> parameterBytes;
  std::uint32_t tapId = 0;
  RuntimeExecutionCapabilities executionCapabilities;
  bool contextuallyBypassed = false;
};

struct RuntimeSamplePage {
  std::uint64_t index = 0;
  std::uint32_t sample = 0;
  std::uint32_t frames = 0;
  std::uint32_t channels = 0;
  std::uint32_t flags = 0;
  std::vector<float> pcm;
};

struct RuntimeAsset {
  std::uint32_t logicalId = 0;
  std::uint32_t slot = 0;
  std::uint32_t formatTag = 0;
  std::uint32_t channels = 0;
  std::uint32_t frames = 0;
  std::uint32_t topology = 0;
  std::uint32_t headBlock = 0;
  std::uint32_t rateDivider = 0;
  std::uint32_t pathCount = 0;
  std::uint32_t inputCount = 0;
  std::uint32_t processingChannels = 0;
  std::uint32_t footprintBytes = 0;
  std::vector<std::uint8_t> payload;
  std::uint64_t streamGeneration = 0;
  std::vector<RuntimeSamplePage> initialPages;
};

struct SoundFontMixerChannel {
  std::uint8_t bankMsb = 0;
  std::uint8_t bankLsb = 0;
  std::uint8_t program = 0;
  std::uint8_t volume = 100;
  std::uint8_t pan = 64;
  std::uint8_t expression = 127;
  std::uint8_t reverb = 0;
  std::uint8_t chorus = 0;
  std::uint8_t selectedBank = 0;
};

struct SoundFontMixerState {
  std::array<SoundFontMixerChannel, 16> channels{};
  SoundFontMixerState() noexcept { channels[9].selectedBank = 128; }
};

class EngineHost {
public:
  static constexpr std::uint32_t kMaxChannels = 8;
  static constexpr std::uint32_t kDefaultTelemetryBytes = 256u * 1024u;
  static constexpr std::uint32_t kMaximumAssetPayloadBytes = 32u * 1024u * 1024u;
  static constexpr std::uint32_t kAggregateAssetBudgetBytes = 128u * 1024u * 1024u;

  static constexpr std::uint32_t kSfzAssetBudgetBytes = 1024u * 1024u * 1024u;
  // Transport permits the largest supported bank; target admission uses the kernel.
  static constexpr std::uint32_t kMaximumAssetTransportBytes = kSfzAssetBudgetBytes;
  static constexpr std::uint32_t kMaxMidiEvents = 4096u;
  static constexpr std::uint32_t kMaxSamplePages = 32768u;
  static constexpr std::uint32_t kSamplePageFrames = 4096u;
  [[nodiscard]] static std::size_t maximumSoundFontSamples() noexcept;
  [[nodiscard]] static std::uint32_t samplePlayerProcessingChannels(
      std::int8_t channelSpec, std::uint32_t channels) noexcept;
  struct AssetLimits {
    std::uint32_t capacity = 0;
    std::uint32_t budget = 0;
    [[nodiscard]] bool accepts(std::size_t bytes, std::uint32_t footprint) const noexcept {
      return bytes != 0 && bytes <= capacity && footprint >= bytes && footprint <= capacity;
    }
  };
  [[nodiscard]] AssetLimits assetLimits(std::uint32_t logicalId, std::uint32_t slot) const;
  [[nodiscard]] std::uint64_t assetFootprint(std::uint32_t budget,
                                            std::span<const std::uint64_t> excludedKeys) const;

  struct ProcessCounters {
    std::uint64_t batchAttempts = 0;
    std::uint64_t commandFailures = 0;
    std::uint64_t parameterStageFailures = 0;
    std::uint64_t processFailures = 0;
    std::uint64_t completedBatches = 0;
    std::uint64_t telemetryDrains = 0;
    std::uint64_t latencyRefreshes = 0;
  };

  enum class ProcessError : std::uint8_t {
    none,
    notPrepared,
    commandRejected,
    parameterTargetUnavailable,
    parameterStageRejected,
    invalidProcessChunk,
    midiEventsRejected,
    sampleAssetUnavailable,
    engineProcessRejected
  };

  struct ProcessFailureDiagnostic {
    std::uint64_t sequence = 0;
    ProcessError error = ProcessError::none;
  };

  enum class RuntimeFaultKind { tubeCircuit, adaptivePrediction };
  struct CircuitFaultSnapshot {
    std::uint32_t pluginId = 0;
    std::uint32_t instanceEpoch = 0;
    std::uint32_t generation = 0;
    bool latched = false;
    std::uint32_t cause = 0;
    RuntimeFaultKind kind = RuntimeFaultKind::tubeCircuit;
  };
  [[nodiscard]] std::vector<CircuitFaultSnapshot> circuitFaults() const;

  struct ResolvedParameterTarget {
    std::uint32_t logicalId = 0;
    et_instance instance = 0;
    std::uint32_t paramsHash = 0;
    bool contextuallyBypassed = false;

    [[nodiscard]] explicit operator bool() const noexcept {
      return logicalId != 0 && instance != 0;
    }
  };

  class ProcessBatch {
  public:
    ProcessBatch() = default;
    ~ProcessBatch();
    ProcessBatch(const ProcessBatch &) = delete;
    ProcessBatch &operator=(const ProcessBatch &) = delete;

    [[nodiscard]] bool stageParameters(
        std::uint32_t logicalId, std::span<const float> packed,
        std::uint32_t paramsHash,
        std::span<const std::uint8_t> parameterBytes = {}) noexcept;
    [[nodiscard]] bool resolveParameterTarget(
        std::uint32_t logicalId, std::uint32_t paramsHash,
        ResolvedParameterTarget &target) noexcept;
    [[nodiscard]] bool stageParameters(
        const ResolvedParameterTarget &target, std::span<const float> packed,
        std::span<const std::uint8_t> parameterBytes = {}) noexcept;
    [[nodiscard]] bool processChunk(float *const *channels,
                                    std::uint32_t channelCount,
                                    std::uint32_t frameCount,
                                    double timeSeconds,
                                    bool masterBypass) noexcept;
    [[nodiscard]] bool processChunk(const float *const *input, float *const *output,
                                    std::uint32_t channelCount,
                                    std::uint32_t frameCount,
                                    double timeSeconds,
                                    bool masterBypass,
                                    std::span<const et_midi_event> midiEvents = {},
                                    const et_host_transport *transport = nullptr) noexcept;
    [[nodiscard]] bool finish(bool refreshLatency = false) noexcept;
    [[nodiscard]] bool active() const noexcept { return host_ != nullptr; }
    [[nodiscard]] bool failed() const noexcept { return failed_; }
    [[nodiscard]] bool pipelinePlanDirty() const noexcept {
      return pipelinePlanDirty_;
    }

  private:
    friend class EngineHost;
    EngineHost *host_ = nullptr;
    bool latencyDirty_ = false;
    bool pipelinePlanDirty_ = false;
    bool failed_ = false;
  };

  EngineHost();
  ~EngineHost();
  EngineHost(const EngineHost &) = delete;
  EngineHost &operator=(const EngineHost &) = delete;

  [[nodiscard]] bool prepare(double sampleRate, std::uint32_t channels,
                             std::uint32_t maxFrames,
                             std::uint32_t telemetryBytes = kDefaultTelemetryBytes,
                             std::string *error = nullptr);
  void reset();
  // Called by the engine owner with the actual audition state of the audio block.
  void setFrequencyPreviewActive(bool active) noexcept { frequencyPreviewActive_ = active; }

  [[nodiscard]] bool rebuild(const PipelineState &pipeline,
                             const std::vector<RuntimePlugin> &runtimePlugins,
                             std::string *error = nullptr);
  [[nodiscard]] bool updateDescriptor(const PipelineState &pipeline,
                                      std::string *error = nullptr);
  [[nodiscard]] bool makeDescriptorCommand(const PipelineState &pipeline,
                                           std::span<const RuntimePlugin> runtimePlugins,
                                           AudioCommand &command,
                                           std::string *error = nullptr) const;
  [[nodiscard]] bool applyDescriptorCommand(const AudioCommand &command,
                                            std::uint64_t &appliedRevision,
                                            std::string *error = nullptr);
  [[nodiscard]] bool updateParameters(std::uint32_t logicalId, std::span<const float> packed,
                                      std::uint32_t paramsHash,
                                      std::span<const std::uint8_t> parameterBytes = {});
  [[nodiscard]] bool setAsset(RuntimeAsset asset, std::string *error = nullptr);
  class PreparedAssetUpdate {
  public:
    PreparedAssetUpdate();
    ~PreparedAssetUpdate();
    PreparedAssetUpdate(const PreparedAssetUpdate &) = delete;
    PreparedAssetUpdate &operator=(const PreparedAssetUpdate &) = delete;
  private:
    friend class EngineHost;
    struct Storage;
    std::unique_ptr<Storage> storage_;
  };
  [[nodiscard]] bool supportsPreparedAsset(std::uint32_t logicalId, std::uint32_t slot) const;
  [[nodiscard]] std::uint64_t supersedeAssetRequest(std::uint32_t logicalId, std::uint32_t slot);
  // Allocate/create, prepare and finish on control; capture/apply on the engine
  // owner between blocks. The caller transfers exclusive ownership at each step.
  [[nodiscard]] std::unique_ptr<PreparedAssetUpdate> createAssetUpdate(
      RuntimeAsset asset, bool clear, std::string *error = nullptr, std::uint64_t requestRevision = 0);
  [[nodiscard]] bool captureAssetUpdate(PreparedAssetUpdate &update) noexcept;
  [[nodiscard]] static bool prepareAssetUpdate(PreparedAssetUpdate &update) noexcept;
  // Paged banks need the upstream kernel's trial reservation after exchange.
  // Only an exclusive control-side engine owner can allow that activation.
  [[nodiscard]] bool applyAssetUpdate(PreparedAssetUpdate &update,
                                      bool allowPagedActivation = false) noexcept;
  void finishAssetUpdate(PreparedAssetUpdate &update);
  [[nodiscard]] bool clearAsset(std::uint32_t logicalId, std::uint32_t slot);
  [[nodiscard]] std::uint32_t assetState(std::uint32_t logicalId,
                                         std::uint32_t slot) const;
  void retainAssets(std::span<const std::uint32_t> logicalIds);
  [[nodiscard]] std::optional<SoundFontMixerState> soundFontMixerState(
      std::uint32_t logicalId) const;
  [[nodiscard]] std::uint64_t soundFontControlTargetRevision(std::uint32_t logicalId) const;
  struct SoundFontControlResult {
    std::uint64_t requestId = 0;
    std::uint64_t pageGeneration = 0;
    std::optional<std::uint64_t> appliedFrame;
  };
  static constexpr std::size_t kMaxPendingSoundFontControls = AudioCommandQueue::kUsableCapacity;
  [[nodiscard]] bool popSoundFontControlResult(SoundFontControlResult &result) noexcept {
    return soundFontControlResults_.pop(result);
  }
  // A complete project replacement owns the engine while installing its state.
  void replaceSoundFontMixerStates(const std::map<std::uint32_t, SoundFontMixerState> &states);

  [[nodiscard]] bool tryProcessBlock(float *const *channels, std::uint32_t channelCount,
                                     std::uint32_t frameCount, double timeSeconds,
                                     bool masterBypass,
                                     AudioCommandQueue *commands = nullptr) noexcept;
  [[nodiscard]] bool tryProcessBlock(float *const *channels, std::uint32_t channelCount,
                                     std::uint32_t frameCount, double timeSeconds,
                                     bool masterBypass, AudioCommandQueue *commands,
                                     LatestParameterMailbox *parameterMailbox) noexcept;
  [[nodiscard]] bool beginProcessBatch(
      ProcessBatch &batch, AudioCommandQueue *commands = nullptr,
      LatestParameterMailbox *parameterMailbox = nullptr) noexcept;
  [[nodiscard]] ProcessCounters processCounters() const noexcept;
  [[nodiscard]] ProcessError lastProcessError() const noexcept {
    return lastProcessError_.load(std::memory_order_acquire);
  }
  [[nodiscard]] ProcessFailureDiagnostic processFailureDiagnostic() const noexcept;

  [[nodiscard]] std::uint32_t pipelineLatency() const;
  [[nodiscard]] bool refreshPipelinePlan(std::uint64_t &refreshedRevision,
                                         std::string *error = nullptr);
  // The caller transfers exclusive ownership of this single slot between
  // capture/apply on the engine owner and prepare/discard on a control thread.
  [[nodiscard]] bool capturePipelineLatencyUpdate() noexcept;
  [[nodiscard]] bool preparePipelineLatencyUpdate(std::uint32_t &latency) noexcept;
  [[nodiscard]] bool applyPipelineLatencyUpdate(std::uint64_t &revision) noexcept;
  void discardPipelineLatencyUpdate() noexcept;
  [[nodiscard]] std::uint64_t pipelinePlanRevision() const noexcept {
    return pipelinePlanRevision_.load(std::memory_order_acquire);
  }
  // Control-side snapshot: source identities and bytes share the same lifetime.
  [[nodiscard]] std::uint32_t readTelemetry(std::span<std::uint8_t> destination,
                                            std::uint32_t &droppedFrames,
                                            std::map<std::uint32_t, std::uint64_t> *sources = nullptr,
                                            std::optional<std::uint64_t> *mixerEndFrame = nullptr);
  void discardTelemetry() noexcept;
  [[nodiscard]] SpectrumCapture &spectrumCapture() noexcept { return spectrumCapture_; }
  [[nodiscard]] std::uint64_t latencyRevision() const noexcept {
    return latencyRevision_.load(std::memory_order_acquire);
  }

  [[nodiscard]] const std::unordered_map<std::string, KernelInfo> &kernels() const noexcept {
    return kernels_;
  }
  [[nodiscard]] double sampleRate() const noexcept { return sampleRate_; }
  [[nodiscard]] std::uint32_t channels() const noexcept { return channels_; }
  [[nodiscard]] bool prepared() const noexcept { return prepared_; }

private:
  friend class plugin::PluginProcessorTestAccess;
  friend class EngineHostTestAccess;
  enum class AnalysisRole { other, display, rhythm, producer, consumer };
  struct CircuitFaultProjection {
    RuntimeFaultKind kind = RuntimeFaultKind::tubeCircuit;
    std::uint32_t instanceEpoch = 0;
    std::atomic<std::uint64_t> state{0};
  };
  struct SoundFontMixerProjection;
  struct InstanceEntry {
    et_instance instance = 0;
    std::uint32_t paramsHash = 0;
    std::uint32_t kernelIndex = 0;
    bool contextuallyBypassed = false;
    // Allocated only at rebuild for kernels with a host-owned measurement gate.
    std::vector<float> measurementParameters;
    std::unique_ptr<CircuitFaultProjection> circuitFault;
    std::uint32_t assetBudget = kAggregateAssetBudgetBytes;
    std::uint32_t samplePlayerChannels = 0;
    bool sampleAssetUnavailable = false;
    bool soundFont = false;
    bool midiRendering = false;
    SoundFontMixerProjection *soundFontMixer = nullptr;
    AnalysisRole analysisRole = AnalysisRole::other;
    float minimumMidi = 0.0f;
    float maximumMidi = 0.0f;
    bool metronomeClick = false;
    et_instance analysisSource = 0;
    et_instance pendingAnalysisSource = 0;
  };

  void discoverKernels();
  [[nodiscard]] bool stageFloatsUnlocked(InstanceEntry &entry,
                                        std::span<const float> packed,
                                        std::uint32_t paramsHash) noexcept;
  void clearInstancesUnlocked(std::span<const std::uint32_t> retain = {}) noexcept;
  [[nodiscard]] bool applyCommandUnlocked(const AudioCommand &command,
                                          bool *pipelinePlanDirty = nullptr) noexcept;
  void storeActiveDescriptorUnlocked(const std::uint8_t *descriptor,
                                     std::uint32_t byteCount) noexcept;
  [[nodiscard]] bool refreshAnalysisSourcesUnlocked(std::uint32_t channels,
                                                    bool masterBypass) noexcept;
  [[nodiscard]] bool updateParametersUnlocked(std::uint32_t logicalId,
                                              std::span<const float> packed,
                                              std::uint32_t paramsHash,
                                              std::span<const std::uint8_t> parameterBytes) noexcept;
  [[nodiscard]] bool stageAssetUnlocked(RuntimeAsset &asset,
                                        std::string *error = nullptr);
  [[nodiscard]] bool descriptorPreservesSamplePlayerWidths(const std::uint8_t *descriptor) const noexcept;
  [[nodiscard]] std::uint32_t remainingAssetBudgetUnlocked(std::uint64_t key,
                                                          std::uint32_t budget) const noexcept;
  void resetSoundFontInputUnlocked(InstanceEntry &entry) noexcept;
  void restoreSoundFontMixerUnlocked(InstanceEntry &entry) noexcept;
  [[nodiscard]] bool updateSoundFontMixerTelemetryUnlocked(
      std::span<const std::uint8_t> bytes) noexcept;
  void stageMidiUnlocked(std::span<const et_midi_event> events, std::uint32_t channels,
                         bool masterBypass) noexcept;
  [[nodiscard]] bool applySoundFontControlUnlocked(const AudioCommand &command) noexcept;
  // Republishes the asset-state projection and reports whether any asset is
  // still staged or preparing. Safe on the audio thread: it reads the kernel
  // state the block just advanced and stores it into atomics that already
  // exist, so no allocation and no lock is involved.
  [[nodiscard]] bool refreshAssetStatesUnlocked() noexcept;
  [[nodiscard]] std::uint32_t resolveInstance(std::uint32_t logicalId) const noexcept;
  [[nodiscard]] bool refreshLatencyUnlocked(
      bool *instanceLatencyChanged = nullptr) noexcept;
  void drainTelemetryUnlocked() noexcept;
  void refreshCircuitFaultsUnlocked() noexcept;
  void recordProcessFailure(ProcessError error) noexcept;
  static void setError(std::string *destination, std::string message);

  std::unique_ptr<effetune::Engine> engine_;
  static constexpr std::uint32_t kMinimumArenaFrames = 32;
  struct LatencyUpdateStorage;
  std::unique_ptr<LatencyUpdateStorage> latencyUpdate_;
  float *combined_ = nullptr;
  double sampleRate_ = 0.0;
  double processedFrames_ = 0.0;
  // Acknowledgements and buffered mixer telemetry use the same frame count
  // across engine prepares and resets.
  std::uint64_t mixerProcessedFrames_ = 0;
  std::uint32_t channels_ = 0;
  std::uint32_t maxProcessFrames_ = 0;
  bool prepared_ = false;
  bool frequencyPreviewActive_ = false;
  bool hasSoundFont_ = false;
  std::array<std::uint8_t, AudioCommand::kMaxDescriptorBytes> activeDescriptor_{};
  std::uint32_t activeDescriptorByteCount_ = 0;
  std::array<InstanceEntry *, kMaxPipelineNodes> analysisNodes_{};
  bool analysisSourcesDirty_ = true;
  bool analysisMasterBypass_ = false;
  std::uint32_t analysisChannels_ = 0;
  std::unordered_map<std::string, KernelInfo> kernels_;
  // The kernel owns the preparation state as plain memory that the audio
  // thread advances, so it cannot be read from a control thread. Every path
  // that can observe a transition publishes it here instead, which lets a UI
  // poll read the state without stopping the DSP for it.
  struct AssetEntry {
    RuntimeAsset asset;
    std::uint32_t budget = kAggregateAssetBudgetBytes;
    std::atomic<std::uint32_t> state{
        static_cast<std::uint32_t>(ET_ASSET_STATE_NONE)};
    std::atomic<std::uint64_t> requestRevision{0};
  };
  std::unordered_map<std::uint32_t, InstanceEntry> instances_;
  std::uint32_t nextCircuitFaultEpoch_ = 0;
  std::unordered_map<et_instance, std::uint32_t> spectrumInstanceIds_;
  SpectrumCapture spectrumCapture_;
  std::unordered_map<std::uint64_t, AssetEntry> assets_;
  std::map<std::uint32_t, std::unique_ptr<SoundFontMixerProjection>> soundFontMixers_;
  std::unordered_map<std::uint32_t, InstanceEntry *> soundFontMixerTaps_;
  SpscQueue<SoundFontControlResult, kMaxPendingSoundFontControls + 1u> soundFontControlResults_;
  bool assetPreparationLatencyPolling_ = false;
  struct TelemetryStorage;
  std::unique_ptr<TelemetryStorage> telemetry_;
  std::map<std::uint32_t, std::uint64_t> telemetrySources_;
  std::uint64_t nextTelemetrySourceId_ = 0;
  std::atomic<std::uint32_t> pipelineLatency_{0};
  std::atomic<std::uint64_t> latencyRevision_{0};
  std::atomic<std::uint64_t> pipelinePlanRevision_{0};
  std::array<std::uint32_t, kMaxPipelineNodes> observedInstanceLatencies_{};
  std::array<bool, kMaxPipelineNodes> observedLatencyNodes_{};
  struct ProcessCounterAtoms {
    std::atomic<std::uint64_t> batchAttempts{0};
    std::atomic<std::uint64_t> commandFailures{0};
    std::atomic<std::uint64_t> parameterStageFailures{0};
    std::atomic<std::uint64_t> processFailures{0};
    std::atomic<std::uint64_t> completedBatches{0};
    std::atomic<std::uint64_t> telemetryDrains{0};
    std::atomic<std::uint64_t> latencyRefreshes{0};
  } processCounterAtoms_;
  std::atomic<ProcessError> lastProcessError_{ProcessError::none};
  std::atomic<std::uint64_t> processFailureSequence_{0};
  // Serializes the control threads against one another. The audio callback
  // never takes it: every control-thread mutation runs in a window the owner
  // proved free of an in-flight block first, so exclusion is established before
  // the block starts instead of being contended inside it.
  mutable std::mutex engineMutex_;
};

} // namespace effetune::vst
