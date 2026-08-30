#pragma once

#include "effetune/abi.h"
#include "engine/command_queue.h"
#include "engine/pipeline_model.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace effetune {
class Engine;
}

namespace effetune::vst {

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
};

class EngineHost {
public:
  static constexpr std::uint32_t kMaxChannels = 8;
  static constexpr std::uint32_t kMaxProcessFrames = 128;
  static constexpr std::uint32_t kDefaultTelemetryBytes = 256u * 1024u;
  static constexpr std::uint32_t kMaximumAssetPayloadBytes = 32u * 1024u * 1024u;
  static constexpr std::uint32_t kAggregateAssetBudgetBytes = 128u * 1024u * 1024u;

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
    engineProcessRejected
  };

  struct ProcessFailureDiagnostic {
    std::uint64_t sequence = 0;
    ProcessError error = ProcessError::none;
  };

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
                             std::uint32_t telemetryBytes = kDefaultTelemetryBytes,
                             std::string *error = nullptr);
  void reset();

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
  [[nodiscard]] bool clearAsset(std::uint32_t logicalId, std::uint32_t slot);
  [[nodiscard]] std::uint32_t assetState(std::uint32_t logicalId,
                                         std::uint32_t slot) const;
  void retainAssets(std::span<const std::uint32_t> logicalIds);

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
  [[nodiscard]] std::uint32_t readTelemetry(std::span<std::uint8_t> destination,
                                            std::uint32_t &droppedFrames) noexcept;
  void discardTelemetry() noexcept;
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
  struct InstanceEntry {
    et_instance instance = 0;
    std::uint32_t paramsHash = 0;
    std::uint32_t kernelIndex = 0;
    bool contextuallyBypassed = false;
  };

  void discoverKernels();
  void clearInstancesUnlocked() noexcept;
  [[nodiscard]] bool applyCommandUnlocked(const AudioCommand &command,
                                          bool *pipelinePlanDirty = nullptr) noexcept;
  void storeActiveDescriptorUnlocked(const std::uint8_t *descriptor,
                                     std::uint32_t byteCount) noexcept;
  [[nodiscard]] bool updateParametersUnlocked(std::uint32_t logicalId,
                                              std::span<const float> packed,
                                              std::uint32_t paramsHash,
                                              std::span<const std::uint8_t> parameterBytes) noexcept;
  [[nodiscard]] bool stageAssetUnlocked(const RuntimeAsset &asset,
                                        std::string *error = nullptr);
  // Republishes the asset-state projection and reports whether any asset is
  // still staged or preparing. Safe on the audio thread: it reads the kernel
  // state the block just advanced and stores it into atomics that already
  // exist, so no allocation and no lock is involved.
  [[nodiscard]] bool refreshAssetStatesUnlocked() noexcept;
  [[nodiscard]] std::uint32_t resolveInstance(std::uint32_t logicalId) const noexcept;
  [[nodiscard]] bool refreshLatencyUnlocked(
      bool *instanceLatencyChanged = nullptr) noexcept;
  void drainTelemetryUnlocked() noexcept;
  void recordProcessFailure(ProcessError error) noexcept;
  static void setError(std::string *destination, std::string message);

  std::unique_ptr<effetune::Engine> engine_;
  struct LatencyUpdateStorage;
  std::unique_ptr<LatencyUpdateStorage> latencyUpdate_;
  float *combined_ = nullptr;
  double sampleRate_ = 0.0;
  double processedFrames_ = 0.0;
  std::uint32_t channels_ = 0;
  bool prepared_ = false;
  std::array<std::uint8_t, AudioCommand::kMaxDescriptorBytes> activeDescriptor_{};
  std::uint32_t activeDescriptorByteCount_ = 0;
  std::unordered_map<std::string, KernelInfo> kernels_;
  // The kernel owns the preparation state as plain memory that the audio
  // thread advances, so it cannot be read from a control thread. Every path
  // that can observe a transition publishes it here instead, which lets a UI
  // poll read the state without stopping the DSP for it.
  struct AssetEntry {
    RuntimeAsset asset;
    std::atomic<std::uint32_t> state{
        static_cast<std::uint32_t>(ET_ASSET_STATE_NONE)};
  };
  std::unordered_map<std::uint32_t, InstanceEntry> instances_;
  std::unordered_map<std::uint64_t, AssetEntry> assets_;
  bool assetPreparationLatencyPolling_ = false;
  struct TelemetryStorage;
  std::unique_ptr<TelemetryStorage> telemetry_;
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
