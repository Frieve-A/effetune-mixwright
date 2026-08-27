#include "engine/engine_host.h"

#include "engine/latency.h"
#include "engine.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>

#if defined(_M_IX86) || defined(_M_X64) || defined(__i386__) || defined(__x86_64__)
#include <xmmintrin.h>
#endif

namespace effetune::vst {

struct EngineHost::TelemetryStorage {
  struct Slot {
    std::array<std::uint8_t, kDefaultTelemetryBytes> bytes{};
    std::uint32_t byteCount = 0;
    std::uint32_t droppedFrames = 0;
  };
  static constexpr std::size_t kSlotCount = 4;
  std::array<Slot, kSlotCount> slots{};
  std::array<std::uint8_t, kDefaultTelemetryBytes> discard{};
  std::atomic<std::size_t> write{0};
  std::atomic<std::size_t> read{0};
  std::atomic<std::uint32_t> producerDroppedFrames{0};
  std::atomic<std::uint32_t> deliveryDroppedFrames{0};
};

namespace {

template <typename IsContextuallyBypassed>
[[nodiscard]] PipelineState pipelineForNativeChannelContext(
    const PipelineState &pipeline, const std::uint32_t outputChannelCount,
    IsContextuallyBypassed &&isContextuallyBypassed) {
  auto routed = pipeline;
  for (auto &plugin : routed.plugins) {
    // The upstream null selection is mono for a mono destination, whereas the
    // packed native -1 channel spec always means a two-channel pair.
    if ((!plugin.channel.has_value() || plugin.channel->empty()) &&
        outputChannelCount == 1u) {
      plugin.channel = "1";
      continue;
    }
    if (!isContextuallyBypassed(plugin.id) || !plugin.channel.has_value()) {
      continue;
    }

    std::uint32_t firstChannel = 0;
    if (*plugin.channel == "34") {
      firstChannel = 2u;
    } else if (*plugin.channel == "56") {
      firstChannel = 4u;
    } else if (*plugin.channel == "78") {
      firstChannel = 6u;
    } else {
      continue;
    }
    // workletRoutingSelection() narrows an execution-bypassed pair to the
    // one remaining channel. With none remaining it skips the node, which the
    // unchanged pair spec already makes the native engine do as well.
    if (outputChannelCount == firstChannel + 1u) {
      plugin.channel = std::to_string(firstChannel + 1u);
    }
  }
  return routed;
}

#if defined(_M_IX86) || defined(_M_X64) || defined(__i386__) || defined(__x86_64__)
class ScopedMxcsrDenormalFlush final {
public:
  ScopedMxcsrDenormalFlush() noexcept : previous_(_mm_getcsr()) {
    _mm_setcsr(previous_ | kFlushToZero | kDenormalsAreZero);
  }

  ~ScopedMxcsrDenormalFlush() noexcept { _mm_setcsr(previous_); }

  ScopedMxcsrDenormalFlush(const ScopedMxcsrDenormalFlush &) = delete;
  ScopedMxcsrDenormalFlush &operator=(const ScopedMxcsrDenormalFlush &) = delete;

private:
  static constexpr unsigned int kFlushToZero = 1u << 15u;
  static constexpr unsigned int kDenormalsAreZero = 1u << 6u;

  unsigned int previous_ = 0;
};
#endif

[[nodiscard]] std::uint32_t countTelemetryFrames(const std::uint8_t *bytes,
                                                 const std::uint32_t byteCount) noexcept {
  std::uint32_t count = 0;
  std::uint32_t offset = 0;
  while (byteCount - offset >= 16u) {
    const auto payload = static_cast<std::uint32_t>(bytes[offset + 12u]) |
                         (static_cast<std::uint32_t>(bytes[offset + 13u]) << 8u);
    const auto frameBytes = (16u + payload + 3u) & ~3u;
    if (frameBytes < 16u || frameBytes > byteCount - offset) {
      break;
    }
    ++count;
    offset += frameBytes;
  }
  return count;
}

void addDroppedFrames(std::atomic<std::uint32_t> &destination,
                      const std::uint64_t value) noexcept {
  auto current = destination.load(std::memory_order_relaxed);
  for (;;) {
    const auto sum = std::min<std::uint64_t>(
        std::numeric_limits<std::uint32_t>::max(), static_cast<std::uint64_t>(current) + value);
    if (destination.compare_exchange_weak(current, static_cast<std::uint32_t>(sum),
                                          std::memory_order_release,
                                          std::memory_order_relaxed)) {
      return;
    }
  }
}

[[nodiscard]] bool sameAsset(const RuntimeAsset &left, const RuntimeAsset &right) noexcept {
  return left.logicalId == right.logicalId && left.slot == right.slot &&
         left.formatTag == right.formatTag && left.channels == right.channels &&
         left.frames == right.frames && left.topology == right.topology &&
         left.headBlock == right.headBlock && left.rateDivider == right.rateDivider &&
         left.pathCount == right.pathCount && left.inputCount == right.inputCount &&
         left.processingChannels == right.processingChannels &&
         left.footprintBytes == right.footprintBytes && left.payload == right.payload;
}

[[nodiscard]] std::uint32_t readUint32(const std::uint8_t *bytes) noexcept {
  return static_cast<std::uint32_t>(bytes[0]) |
         (static_cast<std::uint32_t>(bytes[1]) << 8u) |
         (static_cast<std::uint32_t>(bytes[2]) << 16u) |
         (static_cast<std::uint32_t>(bytes[3]) << 24u);
}

void writeUint32(std::uint8_t *bytes, const std::uint32_t value) noexcept {
  bytes[0] = static_cast<std::uint8_t>(value & 0xffu);
  bytes[1] = static_cast<std::uint8_t>((value >> 8u) & 0xffu);
  bytes[2] = static_cast<std::uint8_t>((value >> 16u) & 0xffu);
  bytes[3] = static_cast<std::uint8_t>((value >> 24u) & 0xffu);
}

[[nodiscard]] bool validDescriptor(const std::uint8_t *bytes,
                                   const std::uint32_t byteCount) noexcept {
  if (bytes == nullptr || byteCount < kPipelineDescriptorHeaderBytes ||
      readUint32(bytes) != kPipelineDescriptorVersion) {
    return false;
  }
  const auto nodeCount = readUint32(bytes + 4u);
  return nodeCount <= kMaxPipelineNodes &&
         byteCount == kPipelineDescriptorHeaderBytes +
                          nodeCount * kPipelineDescriptorNodeBytes;
}

} // namespace

EngineHost::EngineHost() : telemetry_(std::make_unique<TelemetryStorage>()) {
  if (et_abi_version() != EFFETUNE_DSP_ABI_VERSION) {
    throw std::runtime_error("Unsupported EffeTune DSP ABI");
  }
  engine_ = std::make_unique<effetune::Engine>();
  discoverKernels();
}

EngineHost::~EngineHost() {
  std::scoped_lock lock(engineMutex_);
  clearInstancesUnlocked();
}

void EngineHost::setError(std::string *destination, std::string message) {
  if (destination != nullptr) {
    *destination = std::move(message);
  }
}

void EngineHost::discoverKernels() {
  kernels_.clear();
  const auto count = et_kernel_count();
  for (std::uint32_t index = 0; index < count; ++index) {
    const auto length = et_kernel_name(index, nullptr, 0);
    if (length <= 0) {
      continue;
    }
    std::string name(static_cast<std::size_t>(length) + 1u, '\0');
    if (et_kernel_name(index, name.data(), static_cast<std::uint32_t>(name.size())) < 0) {
      continue;
    }
    name.resize(static_cast<std::size_t>(length));
    kernels_.emplace(std::move(name),
                     KernelInfo{index, et_kernel_params_hash(index),
                                et_kernel_param_bytes_capacity(index),
                                et_kernel_asset_capacity(index, 0u)});
  }
}

bool EngineHost::prepare(const double sampleRate, const std::uint32_t channels,
                         const std::uint32_t telemetryBytes, std::string *error) {
  if (!std::isfinite(sampleRate) || sampleRate <= 0.0 || channels == 0 ||
      channels > kMaxChannels || telemetryBytes == 0) {
    setError(error, "Invalid DSP prepare configuration");
    return false;
  }
  std::scoped_lock lock(engineMutex_);
  clearInstancesUnlocked();
  const auto status =
      engine_->prepare(static_cast<float>(sampleRate), channels, kMaxProcessFrames, telemetryBytes);
  if (status != ET_OK) {
    prepared_ = false;
    setError(error, "et_engine_prepare failed with status " + std::to_string(status));
    return false;
  }
  if (engine_->setTelemetryRate(60.0f) != ET_OK) {
    prepared_ = false;
    setError(error, "Unable to set DSP telemetry rate");
    return false;
  }
  combined_ = engine_->combined();
  if (combined_ == nullptr) {
    prepared_ = false;
    setError(error, "DSP combined arena is unavailable");
    return false;
  }
  sampleRate_ = sampleRate;
  channels_ = channels;
  processedFrames_ = 0.0;
  prepared_ = true;
  activeDescriptorByteCount_ = 0;
  const auto previousLatency = pipelineLatency_.exchange(0, std::memory_order_acq_rel);
  if (previousLatency != 0) {
    latencyRevision_.fetch_add(1, std::memory_order_acq_rel);
  }
  telemetry_->read.store(0, std::memory_order_release);
  telemetry_->write.store(0, std::memory_order_release);
  telemetry_->producerDroppedFrames.store(0, std::memory_order_release);
  telemetry_->deliveryDroppedFrames.store(0, std::memory_order_release);
  return true;
}

void EngineHost::reset() {
  std::scoped_lock lock(engineMutex_);
  if (prepared_) {
    (void)engine_->reset();
    combined_ = engine_->combined();
    processedFrames_ = 0.0;
  }
}

void EngineHost::clearInstancesUnlocked() noexcept {
  for (const auto &[logicalId, entry] : instances_) {
    (void)logicalId;
    if (entry.instance != 0) {
      engine_->destroyInstance(entry.instance);
    }
  }
  instances_.clear();
  // Without an instance there is no kernel to hold a preparation state, so the
  // projection has to say so until the assets are staged again.
  for (auto &[key, asset] : assets_) {
    (void)key;
    asset.state.store(static_cast<std::uint32_t>(ET_ASSET_STATE_NONE),
                      std::memory_order_release);
  }
  activeDescriptorByteCount_ = 0;
  assetPreparationLatencyPolling_ = false;
}

std::uint32_t EngineHost::resolveInstance(const std::uint32_t logicalId) const noexcept {
  const auto found = instances_.find(logicalId);
  return found == instances_.end() ? 0u : found->second.instance;
}

bool EngineHost::rebuild(const PipelineState &pipeline,
                         const std::vector<RuntimePlugin> &runtimePlugins, std::string *error) {
  std::scoped_lock lock(engineMutex_);
  if (!prepared_) {
    setError(error, "DSP engine has not been prepared");
    return false;
  }
  if (runtimePlugins.size() > kMaxPluginInstances) {
    setError(error, "Pipeline exceeds 96 native DSP instances");
    return false;
  }
  for (std::size_t index = 0; index < runtimePlugins.size(); ++index) {
    const auto &runtime = runtimePlugins[index];
    if (runtime.logicalId == 0 || runtime.type.empty()) {
      continue;
    }
    for (std::size_t previous = 0; previous < index; ++previous) {
      const auto &existing = runtimePlugins[previous];
      if (existing.logicalId == runtime.logicalId && existing.logicalId != 0 &&
          !existing.type.empty()) {
        setError(error, "Native DSP plug-in IDs must be unique");
        return false;
      }
    }
  }

  clearInstancesUnlocked();
  for (const auto &runtime : runtimePlugins) {
    if (runtime.logicalId == 0 || runtime.type.empty()) {
      continue;
    }
    const auto kernel = kernels_.find(runtime.type);
    if (kernel == kernels_.end()) {
      continue; // Preserve unknown logical state, but never put it in the native pipeline.
    }
    if (runtime.paramsHash != kernel->second.paramsHash) {
      clearInstancesUnlocked();
      setError(error, "DSP parameter hash mismatch for " + runtime.type);
      return false;
    }
    if (runtime.parameterBytes.size() > kernel->second.parameterBytesCapacity) {
      clearInstancesUnlocked();
      setError(error, "DSP parameter byte payload is too large for " + runtime.type);
      return false;
    }

    const auto instanceKernel = runtime.contextuallyBypassed
                                    ? kernels_.find("VolumePlugin")
                                    : kernel;
    if (instanceKernel == kernels_.end()) {
      clearInstancesUnlocked();
      setError(error, "Contextual bypass DSP is unavailable");
      return false;
    }
    const auto *instanceType = runtime.contextuallyBypassed
                                   ? "VolumePlugin"
                                   : runtime.type.c_str();
    const auto instance = engine_->createInstance(instanceType);
    if (instance == 0) {
      clearInstancesUnlocked();
      setError(error, "Unable to create DSP instance for " + runtime.type);
      return false;
    }
    constexpr std::array<float, 1> unityVolume{0.0f};
    const auto packed = runtime.contextuallyBypassed
                            ? std::span<const float>(unityVolume)
                            : std::span<const float>(runtime.packedParameters);
    const auto instanceParamsHash = runtime.contextuallyBypassed
                                        ? instanceKernel->second.paramsHash
                                        : runtime.paramsHash;
    const auto paramStatus = engine_->setInstanceParams(
        instance, packed.empty() ? nullptr : packed.data(),
        static_cast<std::uint32_t>(packed.size()), instanceParamsHash, 0);
    if (paramStatus != ET_OK) {
      engine_->destroyInstance(instance);
      clearInstancesUnlocked();
      setError(error, "Unable to initialize DSP parameters for " + runtime.type);
      return false;
    }
    if (!runtime.contextuallyBypassed && !runtime.parameterBytes.empty() &&
        engine_->setInstanceParamBytes(instance, runtime.parameterBytes.data(),
                                       static_cast<std::uint32_t>(runtime.parameterBytes.size()),
                                       runtime.paramsHash, 0) != ET_OK) {
      engine_->destroyInstance(instance);
      clearInstancesUnlocked();
      setError(error, "Unable to initialize DSP parameter bytes for " + runtime.type);
      return false;
    }
    const auto tap = runtime.tapId == 0 ? runtime.logicalId : runtime.tapId;
    if (!runtime.contextuallyBypassed && engine_->setInstanceTap(instance, tap) != ET_OK) {
      engine_->destroyInstance(instance);
      clearInstancesUnlocked();
      setError(error, "Unable to set DSP telemetry tap for " + runtime.type);
      return false;
    }
    try {
      const auto inserted = instances_
                                .emplace(runtime.logicalId,
                                         InstanceEntry{instance, runtime.paramsHash,
                                                       kernel->second.index,
                                                       runtime.contextuallyBypassed})
                                .second;
      if (!inserted) {
        engine_->destroyInstance(instance);
        clearInstancesUnlocked();
        setError(error, "Native DSP plug-in IDs must be unique");
        return false;
      }
    } catch (const std::bad_alloc &) {
      engine_->destroyInstance(instance);
      clearInstancesUnlocked();
      setError(error, "Unable to track the DSP instance");
      return false;
    }
    for (const auto &[key, entry] : assets_) {
      (void)key;
      if (entry.asset.logicalId == runtime.logicalId) {
        (void)stageAssetUnlocked(entry.asset);
      }
    }
  }

  try {
    const auto routedPipeline = pipelineForNativeChannelContext(
        pipeline, channels_, [this](const std::uint32_t logicalId) {
          const auto instance = instances_.find(logicalId);
          return instance != instances_.end() &&
                 instance->second.contextuallyBypassed;
        });
    const auto descriptor = encodePipelineDescriptor(
        routedPipeline,
        [this](const std::uint32_t logicalId) { return resolveInstance(logicalId); });
    const auto status =
        engine_->configurePipeline(descriptor.data(), static_cast<std::uint32_t>(descriptor.size()));
    if (status != ET_OK) {
      clearInstancesUnlocked();
      setError(error, "et_pipeline_configure failed with status " + std::to_string(status));
      return false;
    }
    storeActiveDescriptorUnlocked(descriptor.data(),
                                  static_cast<std::uint32_t>(descriptor.size()));
  } catch (const std::exception &exception) {
    clearInstancesUnlocked();
    setError(error, exception.what());
    return false;
  }

  combined_ = engine_->combined();
  (void)refreshLatencyUnlocked();
  (void)refreshAssetStatesUnlocked();
  return combined_ != nullptr;
}

bool EngineHost::updateDescriptor(const PipelineState &pipeline, std::string *error) {
  std::scoped_lock lock(engineMutex_);
  if (!prepared_) {
    setError(error, "DSP engine has not been prepared");
    return false;
  }
  try {
    const auto routedPipeline = pipelineForNativeChannelContext(
        pipeline, channels_, [this](const std::uint32_t logicalId) {
          const auto instance = instances_.find(logicalId);
          return instance != instances_.end() &&
                 instance->second.contextuallyBypassed;
        });
    const auto descriptor = encodePipelineDescriptor(
        routedPipeline,
        [this](const std::uint32_t logicalId) { return resolveInstance(logicalId); });
    const auto status =
        engine_->configurePipeline(descriptor.data(), static_cast<std::uint32_t>(descriptor.size()));
    if (status != ET_OK) {
      setError(error, "et_pipeline_configure failed with status " + std::to_string(status));
      return false;
    }
    storeActiveDescriptorUnlocked(descriptor.data(),
                                  static_cast<std::uint32_t>(descriptor.size()));
  } catch (const std::exception &exception) {
    setError(error, exception.what());
    return false;
  }
  (void)refreshLatencyUnlocked();
  return true;
}

bool EngineHost::makeDescriptorCommand(
    const PipelineState &pipeline, const std::span<const RuntimePlugin> runtimePlugins,
    AudioCommand &command, std::string *error) const {
  try {
    const auto routedPipeline = pipelineForNativeChannelContext(
        pipeline, channels_, [this](const std::uint32_t logicalId) {
          const auto instance = instances_.find(logicalId);
          return instance != instances_.end() &&
                 instance->second.contextuallyBypassed;
        });
    const auto descriptor = encodePipelineDescriptor(
        routedPipeline, [this, runtimePlugins](const std::uint32_t logicalId) {
          const auto runtime = std::find_if(
              runtimePlugins.begin(), runtimePlugins.end(),
              [logicalId](const RuntimePlugin &candidate) {
                return candidate.logicalId == logicalId;
              });
          return runtime != runtimePlugins.end() && kernels_.contains(runtime->type)
                     ? logicalId
                     : 0u;
        });
    if (descriptor.size() > command.descriptor.size()) {
      setError(error, "Pipeline descriptor exceeds the audio command capacity");
      return false;
    }
    command = {};
    command.type = AudioCommandType::setDescriptor;
    command.descriptorByteCount = static_cast<std::uint32_t>(descriptor.size());
    std::copy(descriptor.begin(), descriptor.end(), command.descriptor.begin());
    return true;
  } catch (const std::exception &exception) {
    setError(error, exception.what());
    return false;
  }
}

bool EngineHost::applyDescriptorCommand(const AudioCommand &command,
                                        std::uint64_t &appliedRevision,
                                        std::string *error) {
  std::scoped_lock lock(engineMutex_);
  if (!prepared_ || command.type != AudioCommandType::setDescriptor ||
      command.descriptorByteCount > command.descriptor.size() ||
      !validDescriptor(command.descriptor.data(), command.descriptorByteCount)) {
    setError(error, "Native DSP topology command is invalid");
    return false;
  }

  auto descriptor = command.descriptor;
  const auto nodeCount = readUint32(descriptor.data() + 4u);
  for (std::uint32_t index = 0; index < nodeCount; ++index) {
    auto *node = descriptor.data() + kPipelineDescriptorHeaderBytes +
                 index * kPipelineDescriptorNodeBytes;
    const auto instance = instances_.find(readUint32(node));
    if (instance == instances_.end()) {
      setError(error, "Native DSP topology references an unavailable instance");
      return false;
    }
    writeUint32(node, instance->second.instance);
  }
  const auto status = engine_->configurePipeline(descriptor.data(),
                                                  command.descriptorByteCount);
  if (status != ET_OK) {
    setError(error, "et_pipeline_configure failed with status " +
                        std::to_string(status));
    return false;
  }
  storeActiveDescriptorUnlocked(descriptor.data(), command.descriptorByteCount);
  (void)refreshLatencyUnlocked();
  appliedRevision = pipelinePlanRevision_.fetch_add(1, std::memory_order_acq_rel) + 1u;
  return true;
}

bool EngineHost::updateParametersUnlocked(const std::uint32_t logicalId,
                                          const std::span<const float> packed,
                                          const std::uint32_t paramsHash,
                                          const std::span<const std::uint8_t> parameterBytes) noexcept {
  const auto found = instances_.find(logicalId);
  if (found == instances_.end() || found->second.paramsHash != paramsHash ||
      packed.size() > std::numeric_limits<std::uint32_t>::max() ||
      parameterBytes.size() > std::numeric_limits<std::uint32_t>::max()) {
    return false;
  }
  if (found->second.contextuallyBypassed) {
    return true;
  }
  if (engine_->setInstanceParams(found->second.instance,
                                 packed.empty() ? nullptr : packed.data(),
                                 static_cast<std::uint32_t>(packed.size()), paramsHash, 0) != ET_OK) {
    return false;
  }
  return parameterBytes.empty() ||
         engine_->setInstanceParamBytes(found->second.instance, parameterBytes.data(),
                                        static_cast<std::uint32_t>(parameterBytes.size()),
                                        paramsHash, 0) == ET_OK;
}

bool EngineHost::updateParameters(const std::uint32_t logicalId,
                                  const std::span<const float> packed,
                                  const std::uint32_t paramsHash,
                                  const std::span<const std::uint8_t> parameterBytes) {
  std::scoped_lock lock(engineMutex_);
  const auto instance = instances_.find(logicalId);
  const auto previousLatency =
      instance == instances_.end() ? 0u : engine_->instanceLatency(instance->second.instance);
  if (!prepared_) {
    return false;
  }
  const auto updated = updateParametersUnlocked(logicalId, packed, paramsHash, parameterBytes);
  const auto latencyChanged = instance != instances_.end() &&
                              engine_->instanceLatency(instance->second.instance) !=
                                  previousLatency;
  if (latencyChanged) {
    pipelinePlanRevision_.fetch_add(1, std::memory_order_acq_rel);
    (void)refreshLatencyUnlocked();
  }
  return updated;
}

bool EngineHost::stageAssetUnlocked(const RuntimeAsset &asset, std::string *error) {
  const auto found = instances_.find(asset.logicalId);
  if (found == instances_.end()) {
    setError(error, "DSP asset target is unavailable");
    return false;
  }
  const auto capacity = et_kernel_asset_capacity(found->second.kernelIndex, asset.slot);
  if (capacity == 0u || asset.payload.empty() || asset.payload.size() > capacity ||
      asset.payload.size() > kMaximumAssetPayloadBytes ||
      asset.footprintBytes < asset.payload.size() ||
      asset.footprintBytes > capacity) {
    setError(error, "DSP asset exceeds the plug-in capacity");
    return false;
  }
  if (found->second.contextuallyBypassed) {
    return true;
  }
  const AssetBeginInfo beginInfo{asset.channels,
                                 asset.frames,
                                 asset.topology,
                                 asset.headBlock,
                                 asset.rateDivider,
                                 asset.pathCount,
                                 asset.inputCount,
                                 asset.processingChannels,
                                 asset.footprintBytes,
                                 static_cast<std::uint32_t>(asset.payload.size())};
  auto *staging = engine_->beginInstanceAsset(found->second.instance, asset.slot, beginInfo);
  if (staging == nullptr) {
    setError(error, "DSP asset could not be staged");
    return false;
  }
  std::memcpy(staging, asset.payload.data(), asset.payload.size());
  if (engine_->commitInstanceAsset(found->second.instance, asset.slot,
                                   static_cast<std::uint32_t>(asset.payload.size()),
                                   asset.formatTag) != ET_OK) {
    setError(error, "DSP asset could not be committed");
    return false;
  }
  assetPreparationLatencyPolling_ = true;
  return true;
}

bool EngineHost::refreshAssetStatesUnlocked() noexcept {
  auto pending = false;
  for (auto &[key, entry] : assets_) {
    (void)key;
    const auto instance = instances_.find(entry.asset.logicalId);
    const auto state = instance == instances_.end() ||
                               instance->second.contextuallyBypassed
                           ? static_cast<std::uint32_t>(ET_ASSET_STATE_NONE)
                           : engine_->instanceAssetState(instance->second.instance,
                                                         entry.asset.slot);
    entry.state.store(state, std::memory_order_release);
    const auto phase = state & 0xffu;
    pending = pending || phase == ET_ASSET_STATE_STAGED ||
              phase == ET_ASSET_STATE_PREPARING;
  }
  return pending;
}

bool EngineHost::setAsset(RuntimeAsset asset, std::string *error) {
  std::scoped_lock lock(engineMutex_);
  const auto key = (static_cast<std::uint64_t>(asset.logicalId) << 32u) | asset.slot;
  std::uint64_t aggregate = asset.footprintBytes;
  for (const auto &[cachedKey, cached] : assets_) {
    if (cachedKey != key) {
      aggregate += cached.asset.footprintBytes;
    }
  }
  if (aggregate > kAggregateAssetBudgetBytes) {
    setError(error, "DSP asset budget exceeded");
    return false;
  }
  const auto cached = assets_.find(key);
  const auto instance = instances_.find(asset.logicalId);
  if (cached != assets_.end() && instance != instances_.end() &&
      sameAsset(cached->second.asset, asset)) {
    if (instance->second.contextuallyBypassed) {
      return true;
    }
    const auto state = engine_->instanceAssetState(instance->second.instance, asset.slot) & 0xffu;
    if (state >= ET_ASSET_STATE_STAGED && state <= ET_ASSET_STATE_ACTIVE) {
      (void)refreshAssetStatesUnlocked();
      return true;
    }
  }
  const auto previousLatency =
      instance == instances_.end() ? 0u : engine_->instanceLatency(instance->second.instance);
  if (!stageAssetUnlocked(asset, error)) {
    return false;
  }
  const auto logicalId = asset.logicalId;
  const auto slot = asset.slot;
  try {
    assets_[key].asset = std::move(asset);
  } catch (const std::bad_alloc &) {
    const auto found = instances_.find(logicalId);
    if (found != instances_.end() && !found->second.contextuallyBypassed) {
      engine_->abortInstanceAsset(found->second.instance, slot);
    }
    assetPreparationLatencyPolling_ = refreshAssetStatesUnlocked();
    setError(error, "DSP asset cache could not be allocated");
    return false;
  }
  // Asset commit changes the resident convolution, and therefore its latency,
  // inside this already non-real-time mutation window. Publish that change now:
  // a stopped host may not render another block, so leaving discovery to block
  // housekeeping would strand both the compensation refresh and the UI delay
  // display until an unrelated topology edit.
  const auto instanceLatencyChanged =
      instance != instances_.end() &&
      engine_->instanceLatency(instance->second.instance) != previousLatency;
  (void)refreshLatencyUnlocked();
  if (instanceLatencyChanged) {
    pipelinePlanRevision_.fetch_add(1, std::memory_order_acq_rel);
  }
  // Staging leaves the latency polling flag set on purpose: the state the
  // block-end scan will observe is published here so the UI can follow the
  // preparation without opening a window of its own.
  (void)refreshAssetStatesUnlocked();
  return true;
}

bool EngineHost::clearAsset(const std::uint32_t logicalId, const std::uint32_t slot) {
  std::scoped_lock lock(engineMutex_);
  const auto found = instances_.find(logicalId);
  const auto previousLatency =
      found == instances_.end() ? 0u : engine_->instanceLatency(found->second.instance);
  if (found != instances_.end() && !found->second.contextuallyBypassed) {
    engine_->abortInstanceAsset(found->second.instance, slot);
  }
  const auto key = (static_cast<std::uint64_t>(logicalId) << 32u) | slot;
  assets_.erase(key);
  assetPreparationLatencyPolling_ = refreshAssetStatesUnlocked();
  const auto instanceLatencyChanged =
      found != instances_.end() &&
      engine_->instanceLatency(found->second.instance) != previousLatency;
  (void)refreshLatencyUnlocked();
  if (instanceLatencyChanged) {
    pipelinePlanRevision_.fetch_add(1, std::memory_order_acq_rel);
  }
  return found != instances_.end();
}

std::uint32_t EngineHost::assetState(const std::uint32_t logicalId,
                                     const std::uint32_t slot) const {
  // Reads the published projection instead of the kernel, so the caller needs
  // no quiet engine: polling the preparation state must never cost a block.
  std::scoped_lock lock(engineMutex_);
  const auto found =
      assets_.find((static_cast<std::uint64_t>(logicalId) << 32u) | slot);
  return found == assets_.end()
             ? static_cast<std::uint32_t>(ET_ASSET_STATE_NONE)
             : found->second.state.load(std::memory_order_acquire);
}

void EngineHost::retainAssets(const std::span<const std::uint32_t> logicalIds) {
  std::scoped_lock lock(engineMutex_);
  for (auto item = assets_.begin(); item != assets_.end();) {
    const auto retained =
        std::find(logicalIds.begin(), logicalIds.end(), item->second.asset.logicalId) !=
        logicalIds.end();
    if (!retained) {
      item = assets_.erase(item);
    } else {
      ++item;
    }
  }
  assetPreparationLatencyPolling_ = refreshAssetStatesUnlocked();
}

bool EngineHost::applyCommandUnlocked(const AudioCommand &command,
                                      bool *pipelinePlanDirty) noexcept {
  switch (command.type) {
  case AudioCommandType::setParameters:
    if (command.floatCount > command.packed.size() ||
        command.parameterByteCount > command.parameterBytes.size()) {
      return false;
    }
    {
      const auto instance = instances_.find(command.logicalId);
      const auto previousLatency =
          instance == instances_.end() ? 0u : engine_->instanceLatency(instance->second.instance);
      const auto updated = updateParametersUnlocked(
          command.logicalId,
          std::span<const float>(command.packed.data(), command.floatCount), command.paramsHash,
          std::span<const std::uint8_t>(command.parameterBytes.data(),
                                        command.parameterByteCount));
      if (pipelinePlanDirty != nullptr && instance != instances_.end() &&
          engine_->instanceLatency(instance->second.instance) != previousLatency) {
        *pipelinePlanDirty = true;
      }
      return updated;
    }
  case AudioCommandType::setDescriptor:
    // Pipeline configuration may allocate compensation delay storage. Descriptor
    // commands are therefore serviced only by the processor's non-real-time
    // control service and are rejected from ProcessBatch.
    return false;
  case AudioCommandType::reset:
    return engine_->reset() == ET_OK;
  }
  return false;
}

bool EngineHost::tryProcessBlock(float *const *channels, const std::uint32_t channelCount,
                                 const std::uint32_t frameCount, const double timeSeconds,
                                 const bool masterBypass,
                                 AudioCommandQueue *commands) noexcept {
  return tryProcessBlock(channels, channelCount, frameCount, timeSeconds, masterBypass,
                         commands, nullptr);
}

bool EngineHost::tryProcessBlock(float *const *channels, const std::uint32_t channelCount,
                                 const std::uint32_t frameCount, const double timeSeconds,
                                 const bool masterBypass,
                                 AudioCommandQueue *commands,
                                 LatestParameterMailbox *parameterMailbox) noexcept {
  if (channels == nullptr || channelCount == 0 || channelCount > channels_ ||
      frameCount == 0 || frameCount > kMaxProcessFrames ||
      !std::isfinite(timeSeconds)) {
    return false;
  }
  ProcessBatch batch;
  if (!beginProcessBatch(batch, commands, parameterMailbox)) {
    return false;
  }
  const auto processed =
      batch.processChunk(channels, channelCount, frameCount, timeSeconds, masterBypass);
  return batch.finish() && processed;
}

EngineHost::ProcessBatch::~ProcessBatch() { (void)finish(); }

bool EngineHost::ProcessBatch::stageParameters(
    const std::uint32_t logicalId, const std::span<const float> packed,
    const std::uint32_t paramsHash,
    const std::span<const std::uint8_t> parameterBytes) noexcept {
  ResolvedParameterTarget target;
  if (!resolveParameterTarget(logicalId, paramsHash, target)) {
    return false;
  }
  return stageParameters(target, packed, parameterBytes);
}

bool EngineHost::ProcessBatch::resolveParameterTarget(
    const std::uint32_t logicalId, const std::uint32_t paramsHash,
    ResolvedParameterTarget &target) noexcept {
  target = {};
  if (host_ == nullptr || failed_) {
    return false;
  }
  const auto found = host_->instances_.find(logicalId);
  if (found == host_->instances_.end() || found->second.paramsHash != paramsHash) {
    host_->processCounterAtoms_.parameterStageFailures.fetch_add(
        1, std::memory_order_relaxed);
    host_->recordProcessFailure(ProcessError::parameterTargetUnavailable);
    failed_ = true;
    return false;
  }
  target = {logicalId, found->second.instance, paramsHash,
            found->second.contextuallyBypassed};
  return true;
}

bool EngineHost::ProcessBatch::stageParameters(
    const ResolvedParameterTarget &target, const std::span<const float> packed,
    const std::span<const std::uint8_t> parameterBytes) noexcept {
  if (host_ == nullptr || failed_ || !target ||
      packed.size() > std::numeric_limits<std::uint32_t>::max() ||
      parameterBytes.size() > std::numeric_limits<std::uint32_t>::max()) {
    if (host_ != nullptr) {
      host_->processCounterAtoms_.parameterStageFailures.fetch_add(
          1, std::memory_order_relaxed);
      host_->recordProcessFailure(ProcessError::parameterStageRejected);
    }
    failed_ = true;
    return false;
  }
  if (target.contextuallyBypassed) {
    return true;
  }
  const auto previousLatency = host_->engine_->instanceLatency(target.instance);
  const auto parametersStaged =
      host_->engine_->setInstanceParams(
          target.instance, packed.empty() ? nullptr : packed.data(),
          static_cast<std::uint32_t>(packed.size()), target.paramsHash, 0) == ET_OK;
  const auto bytesStaged =
      parametersStaged && (parameterBytes.empty() ||
                           host_->engine_->setInstanceParamBytes(
                               target.instance, parameterBytes.data(),
                               static_cast<std::uint32_t>(parameterBytes.size()),
                               target.paramsHash, 0) == ET_OK);
  if (host_->engine_->instanceLatency(target.instance) != previousLatency) {
    pipelinePlanDirty_ = true;
  }
  if (!parametersStaged || !bytesStaged) {
    host_->processCounterAtoms_.parameterStageFailures.fetch_add(
        1, std::memory_order_relaxed);
    host_->recordProcessFailure(ProcessError::parameterStageRejected);
    failed_ = true;
    return false;
  }
  return true;
}

bool EngineHost::ProcessBatch::processChunk(float *const *channels,
                                            const std::uint32_t channelCount,
                                            const std::uint32_t frameCount,
                                            const double timeSeconds,
                                            const bool masterBypass) noexcept {
  if (host_ == nullptr || failed_ || channels == nullptr || channelCount == 0 ||
      channelCount > host_->channels_ || frameCount == 0 ||
      frameCount > kMaxProcessFrames || !std::isfinite(timeSeconds)) {
    if (host_ != nullptr) {
      host_->processCounterAtoms_.processFailures.fetch_add(1,
                                                            std::memory_order_relaxed);
      host_->recordProcessFailure(ProcessError::invalidProcessChunk);
    }
    failed_ = true;
    return false;
  }
  for (std::uint32_t channel = 0; channel < channelCount; ++channel) {
    if (channels[channel] == nullptr) {
      host_->processCounterAtoms_.processFailures.fetch_add(1,
                                                            std::memory_order_relaxed);
      host_->recordProcessFailure(ProcessError::invalidProcessChunk);
      failed_ = true;
      return false;
    }
    std::memcpy(host_->combined_ + static_cast<std::size_t>(channel) * frameCount,
                channels[channel], sizeof(float) * frameCount);
  }
  et_status status = ET_ERR_STATE;
  {
#if defined(_M_IX86) || defined(_M_X64) || defined(__i386__) || defined(__x86_64__)
    const ScopedMxcsrDenormalFlush mxcsrDenormalFlush;
#endif
    status = host_->engine_->processPipeline(channelCount, frameCount, timeSeconds,
                                             masterBypass ? 1u : 0u);
  }
  if (status != ET_OK) {
    host_->processCounterAtoms_.processFailures.fetch_add(1,
                                                          std::memory_order_relaxed);
    host_->recordProcessFailure(ProcessError::engineProcessRejected);
    failed_ = true;
    return false;
  }
  for (std::uint32_t channel = 0; channel < channelCount; ++channel) {
    std::memcpy(channels[channel],
                host_->combined_ + static_cast<std::size_t>(channel) * frameCount,
                sizeof(float) * frameCount);
  }
  host_->processedFrames_ += frameCount;
  return true;
}

bool EngineHost::ProcessBatch::finish(const bool refreshLatency) noexcept {
  if (host_ == nullptr) {
    return !failed_;
  }
  auto *host = host_;
  const auto assetLatencyMayHaveChanged = host->assetPreparationLatencyPolling_;
  auto instanceLatencyChanged = false;
  if (latencyDirty_ || refreshLatency) {
    (void)host->refreshLatencyUnlocked(&instanceLatencyChanged);
    host->processCounterAtoms_.latencyRefreshes.fetch_add(1,
                                                          std::memory_order_relaxed);
  }
  if (pipelinePlanDirty_ ||
      (assetLatencyMayHaveChanged && instanceLatencyChanged)) {
    host->pipelinePlanRevision_.fetch_add(1, std::memory_order_acq_rel);
  }
  if (host->assetPreparationLatencyPolling_) {
    host->assetPreparationLatencyPolling_ = host->refreshAssetStatesUnlocked();
  }
  host->drainTelemetryUnlocked();
  host->processCounterAtoms_.telemetryDrains.fetch_add(1,
                                                       std::memory_order_relaxed);
  host->processCounterAtoms_.completedBatches.fetch_add(1,
                                                        std::memory_order_relaxed);
  const auto succeeded = !failed_;
  host_ = nullptr;
  return succeeded;
}

bool EngineHost::beginProcessBatch(
    ProcessBatch &batch, AudioCommandQueue *commands,
    LatestParameterMailbox *parameterMailbox) noexcept {
  if (batch.active()) {
    return false;
  }
  processCounterAtoms_.batchAttempts.fetch_add(1, std::memory_order_relaxed);
  // No lock is taken here. The owner only mutates the engine from a control
  // thread after it has proven that no block is in flight, so concurrent
  // control work can never cost the audio callback a block.
  if (!prepared_ || combined_ == nullptr) {
    recordProcessFailure(ProcessError::notPrepared);
    return false;
  }
  batch.host_ = this;
  // Asset preparation becomes active from processPipeline(), so its latency
  // must be observed at block housekeeping until it reaches a terminal state.
  batch.latencyDirty_ = assetPreparationLatencyPolling_;
  batch.pipelinePlanDirty_ = false;
  batch.failed_ = false;

  const auto apply = [this, &batch](const AudioCommand &command) noexcept {
    if (applyCommandUnlocked(command, &batch.pipelinePlanDirty_)) {
      batch.latencyDirty_ = true;
    } else {
      batch.failed_ = true;
      processCounterAtoms_.commandFailures.fetch_add(1,
                                                      std::memory_order_relaxed);
      recordProcessFailure(ProcessError::commandRejected);
    }
  };
  if (commands != nullptr) {
    AudioCommand command;
    while (commands->pop(command)) {
      apply(command);
    }
  }
  if (parameterMailbox != nullptr) {
    parameterMailbox->consumePending(apply);
  }
  if (batch.failed_) {
    (void)batch.finish();
    return false;
  }
  return true;
}

EngineHost::ProcessCounters EngineHost::processCounters() const noexcept {
  return {processCounterAtoms_.batchAttempts.load(std::memory_order_relaxed),
          processCounterAtoms_.commandFailures.load(std::memory_order_relaxed),
          processCounterAtoms_.parameterStageFailures.load(std::memory_order_relaxed),
          processCounterAtoms_.processFailures.load(std::memory_order_relaxed),
          processCounterAtoms_.completedBatches.load(std::memory_order_relaxed),
          processCounterAtoms_.telemetryDrains.load(std::memory_order_relaxed),
          processCounterAtoms_.latencyRefreshes.load(std::memory_order_relaxed)};
}

void EngineHost::recordProcessFailure(const ProcessError error) noexcept {
  lastProcessError_.store(error, std::memory_order_relaxed);
  processFailureSequence_.fetch_add(1, std::memory_order_release);
}

EngineHost::ProcessFailureDiagnostic EngineHost::processFailureDiagnostic() const noexcept {
  const auto sequence = processFailureSequence_.load(std::memory_order_acquire);
  return {sequence, lastProcessError_.load(std::memory_order_relaxed)};
}

std::uint32_t EngineHost::pipelineLatency() const {
  return pipelineLatency_.load(std::memory_order_acquire);
}

bool EngineHost::refreshPipelinePlan(std::uint64_t &refreshedRevision,
                                     std::string *error) {
  std::scoped_lock lock(engineMutex_);
  if (!prepared_ || !validDescriptor(activeDescriptor_.data(), activeDescriptorByteCount_)) {
    setError(error, "DSP pipeline is not configured");
    return false;
  }
  const auto status = engine_->configurePipeline(activeDescriptor_.data(),
                                                  activeDescriptorByteCount_);
  if (status != ET_OK) {
    setError(error, "et_pipeline_configure failed with status " + std::to_string(status));
    return false;
  }
  refreshedRevision = pipelinePlanRevision_.load(std::memory_order_acquire);
  (void)refreshLatencyUnlocked();
  return true;
}

void EngineHost::storeActiveDescriptorUnlocked(const std::uint8_t *descriptor,
                                               const std::uint32_t byteCount) noexcept {
  if (!validDescriptor(descriptor, byteCount) || byteCount > activeDescriptor_.size()) {
    activeDescriptorByteCount_ = 0;
    return;
  }
  std::copy_n(descriptor, byteCount, activeDescriptor_.begin());
  activeDescriptorByteCount_ = byteCount;
}

bool EngineHost::refreshLatencyUnlocked(bool *instanceLatencyChanged) noexcept {
  if (instanceLatencyChanged != nullptr) {
    *instanceLatencyChanged = false;
  }
  std::array<std::array<std::uint32_t, kMaxChannels>, kMaxBus + 1u> latency{};
  std::array<std::array<bool, kMaxChannels>, kMaxBus + 1u> hasContent{};
  if (!validDescriptor(activeDescriptor_.data(), activeDescriptorByteCount_)) {
    observedLatencyNodes_.fill(false);
    const auto previous = pipelineLatency_.exchange(0, std::memory_order_acq_rel);
    if (previous != 0) {
      latencyRevision_.fetch_add(1, std::memory_order_acq_rel);
    }
    return previous != 0;
  }
  const auto previousLatencies = observedInstanceLatencies_;
  const auto previousNodes = observedLatencyNodes_;
  observedLatencyNodes_.fill(false);
  for (std::uint32_t channel = 0; channel < channels_; ++channel) {
    hasContent[0][channel] = true;
  }
  const auto nodeCount = readUint32(activeDescriptor_.data() + 4u);
  for (std::uint32_t index = 0; index < nodeCount; ++index) {
    const auto *node = activeDescriptor_.data() + kPipelineDescriptorHeaderBytes +
                       index * kPipelineDescriptorNodeBytes;
    const auto inputBus = node[5];
    const auto outputBus = node[6];
    if (node[4] == 0 || node[8] == 0 || inputBus > kMaxBus || outputBus > kMaxBus) {
      continue;
    }

    const auto channelSpec = static_cast<std::int8_t>(node[7]);
    std::uint32_t firstChannel = 0;
    std::uint32_t routedChannels = channels_;
    if (channelSpec != -2) {
      if (channelSpec == -1) {
        routedChannels = 2;
      } else if (channelSpec >= 16 && channelSpec <= 19) {
        firstChannel = static_cast<std::uint32_t>(channelSpec - 16) * 2u;
        routedChannels = 2;
      } else if (channelSpec >= 0 && channelSpec < 8) {
        firstChannel = static_cast<std::uint32_t>(channelSpec);
        routedChannels = 1;
      } else {
        continue;
      }
    }
    if (firstChannel + routedChannels > channels_) {
      continue;
    }

    const auto instanceLatency = engine_->instanceLatency(readUint32(node));
    observedInstanceLatencies_[index] = instanceLatency;
    observedLatencyNodes_[index] = true;
    if (instanceLatencyChanged != nullptr &&
        ((!previousNodes[index] && instanceLatency != 0u) ||
         (previousNodes[index] && previousLatencies[index] != instanceLatency))) {
      *instanceLatencyChanged = true;
    }
    for (std::uint32_t offset = 0; offset < routedChannels; ++offset) {
      const auto channel = firstChannel + offset;
      const auto inputLatency = hasContent[inputBus][channel] ? latency[inputBus][channel] : 0u;
      const auto incomingLatency =
          instanceLatency > std::numeric_limits<std::uint32_t>::max() - inputLatency
              ? std::numeric_limits<std::uint32_t>::max()
              : inputLatency + instanceLatency;
      if (inputBus == outputBus || !hasContent[outputBus][channel]) {
        latency[outputBus][channel] = incomingLatency;
        hasContent[outputBus][channel] = true;
      } else if (incomingLatency > latency[outputBus][channel]) {
        latency[outputBus][channel] = incomingLatency;
      }
    }
  }

  std::uint32_t pipelineLatency = 0;
  for (std::uint32_t channel = 0; channel < channels_; ++channel) {
    if (hasContent[0][channel] && latency[0][channel] > pipelineLatency) {
      pipelineLatency = latency[0][channel];
    }
  }
  const auto previous = pipelineLatency_.exchange(pipelineLatency, std::memory_order_acq_rel);
  if (previous != pipelineLatency) {
    latencyRevision_.fetch_add(1, std::memory_order_acq_rel);
  }
  return previous != pipelineLatency;
}

void EngineHost::drainTelemetryUnlocked() noexcept {
  const auto write = telemetry_->write.load(std::memory_order_relaxed);
  const auto next = (write + 1u) % TelemetryStorage::kSlotCount;
  std::uint32_t dropped = 0;
  if (next == telemetry_->read.load(std::memory_order_acquire)) {
    const auto bytes = engine_->readTelemetry(
        telemetry_->discard.data(), static_cast<std::uint32_t>(telemetry_->discard.size()),
        &dropped);
    addDroppedFrames(telemetry_->producerDroppedFrames,
                     static_cast<std::uint64_t>(dropped) +
                         countTelemetryFrames(telemetry_->discard.data(), bytes));
    return;
  }
  auto &slot = telemetry_->slots[write];
  slot.byteCount = engine_->readTelemetry(slot.bytes.data(),
                                          static_cast<std::uint32_t>(slot.bytes.size()), &dropped);
  slot.droppedFrames = dropped;
  if (slot.byteCount != 0 || dropped != 0) {
    telemetry_->write.store(next, std::memory_order_release);
  }
}

std::uint32_t EngineHost::readTelemetry(const std::span<std::uint8_t> destination,
                                        std::uint32_t &droppedFrames) noexcept {
  const auto read = telemetry_->read.load(std::memory_order_relaxed);
  const auto write = telemetry_->write.load(std::memory_order_acquire);
  if (read == write || destination.empty()) {
    droppedFrames = 0;
    return 0;
  }
  const auto latest = (write + TelemetryStorage::kSlotCount - 1u) %
                      TelemetryStorage::kSlotCount;
  std::uint64_t dropped = telemetry_->producerDroppedFrames.exchange(
      0, std::memory_order_acq_rel);
  dropped += telemetry_->deliveryDroppedFrames.exchange(0, std::memory_order_acq_rel);
  auto index = read;
  while (index != latest) {
    const auto &skipped = telemetry_->slots[index];
    dropped += skipped.droppedFrames;
    dropped += countTelemetryFrames(skipped.bytes.data(), skipped.byteCount);
    index = (index + 1u) % TelemetryStorage::kSlotCount;
  }
  const auto &slot = telemetry_->slots[latest];
  const auto bytes = std::min<std::size_t>(slot.byteCount, destination.size());
  std::memcpy(destination.data(), slot.bytes.data(), bytes);
  dropped += slot.droppedFrames;
  droppedFrames = static_cast<std::uint32_t>(std::min<std::uint64_t>(
      dropped, std::numeric_limits<std::uint32_t>::max()));
  telemetry_->read.store(write, std::memory_order_release);
  return static_cast<std::uint32_t>(bytes);
}

void EngineHost::discardTelemetry() noexcept {
  const auto read = telemetry_->read.load(std::memory_order_relaxed);
  const auto write = telemetry_->write.load(std::memory_order_acquire);
  std::uint64_t dropped = telemetry_->producerDroppedFrames.exchange(
      0, std::memory_order_acq_rel);
  auto index = read;
  while (index != write) {
    const auto &slot = telemetry_->slots[index];
    dropped += slot.droppedFrames;
    dropped += countTelemetryFrames(slot.bytes.data(), slot.byteCount);
    index = (index + 1u) % TelemetryStorage::kSlotCount;
  }
  telemetry_->read.store(write, std::memory_order_release);
  addDroppedFrames(telemetry_->deliveryDroppedFrames, dropped);
}

} // namespace effetune::vst
