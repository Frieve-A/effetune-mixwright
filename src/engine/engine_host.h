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
  static constexpr std::uint32_t kQuantumFrames = 128;
  static constexpr std::uint32_t kDefaultTelemetryBytes = 256u * 1024u;
  static constexpr std::uint32_t kMaximumAssetPayloadBytes = 32u * 1024u * 1024u;
  static constexpr std::uint32_t kAggregateAssetBudgetBytes = 128u * 1024u * 1024u;

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
  [[nodiscard]] bool updateParameters(std::uint32_t logicalId, std::span<const float> packed,
                                      std::uint32_t paramsHash,
                                      std::span<const std::uint8_t> parameterBytes = {});
  [[nodiscard]] bool setAsset(RuntimeAsset asset, std::string *error = nullptr);
  [[nodiscard]] bool clearAsset(std::uint32_t logicalId, std::uint32_t slot);
  [[nodiscard]] std::uint32_t assetState(std::uint32_t logicalId,
                                         std::uint32_t slot) const;
  void retainAssets(std::span<const std::uint32_t> logicalIds);

  [[nodiscard]] bool tryProcessQuantum(float *const *channels, std::uint32_t channelCount,
                                       std::uint32_t frameCount, double timeSeconds,
                                       bool masterBypass,
                                       AudioCommandQueue *commands = nullptr) noexcept;
  [[nodiscard]] bool tryProcessQuantum(float *const *channels, std::uint32_t channelCount,
                                       std::uint32_t frameCount, double timeSeconds,
                                       bool masterBypass, AudioCommandQueue *commands,
                                       LatestParameterMailbox *parameterMailbox) noexcept;

  [[nodiscard]] std::uint32_t pipelineLatency() const;
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
  };

  void discoverKernels();
  void clearInstancesUnlocked() noexcept;
  [[nodiscard]] bool applyCommandUnlocked(const AudioCommand &command) noexcept;
  void storeActiveDescriptorUnlocked(const std::uint8_t *descriptor,
                                     std::uint32_t byteCount) noexcept;
  [[nodiscard]] bool updateParametersUnlocked(std::uint32_t logicalId,
                                              std::span<const float> packed,
                                              std::uint32_t paramsHash,
                                              std::span<const std::uint8_t> parameterBytes) noexcept;
  [[nodiscard]] bool stageAssetUnlocked(const RuntimeAsset &asset,
                                        std::string *error = nullptr);
  [[nodiscard]] std::uint32_t resolveInstance(std::uint32_t logicalId) const noexcept;
  void refreshLatencyUnlocked() noexcept;
  void drainTelemetryUnlocked() noexcept;
  static void setError(std::string *destination, std::string message);

  std::unique_ptr<effetune::Engine> engine_;
  float *combined_ = nullptr;
  double sampleRate_ = 0.0;
  double processedFrames_ = 0.0;
  std::uint32_t channels_ = 0;
  bool prepared_ = false;
  std::array<std::uint8_t, AudioCommand::kMaxDescriptorBytes> activeDescriptor_{};
  std::uint32_t activeDescriptorByteCount_ = 0;
  std::unordered_map<std::string, KernelInfo> kernels_;
  std::unordered_map<std::uint32_t, InstanceEntry> instances_;
  std::unordered_map<std::uint64_t, RuntimeAsset> assets_;
  struct TelemetryStorage;
  std::unique_ptr<TelemetryStorage> telemetry_;
  std::atomic<std::uint32_t> pipelineLatency_{0};
  std::atomic<std::uint64_t> latencyRevision_{0};
  mutable std::mutex engineMutex_;
};

} // namespace effetune::vst
