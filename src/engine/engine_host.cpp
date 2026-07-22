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
  activeDescriptorByteCount_ = 0;
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

    const auto instance = engine_->createInstance(runtime.type.c_str());
    if (instance == 0) {
      clearInstancesUnlocked();
      setError(error, "Unable to create DSP instance for " + runtime.type);
      return false;
    }
    const auto *packed = runtime.packedParameters.empty() ? nullptr : runtime.packedParameters.data();
    const auto paramStatus = engine_->setInstanceParams(
        instance, packed, static_cast<std::uint32_t>(runtime.packedParameters.size()),
        runtime.paramsHash, 0);
    if (paramStatus != ET_OK) {
      engine_->destroyInstance(instance);
      clearInstancesUnlocked();
      setError(error, "Unable to initialize DSP parameters for " + runtime.type);
      return false;
    }
    if (!runtime.parameterBytes.empty() &&
        engine_->setInstanceParamBytes(instance, runtime.parameterBytes.data(),
                                       static_cast<std::uint32_t>(runtime.parameterBytes.size()),
                                       runtime.paramsHash, 0) != ET_OK) {
      engine_->destroyInstance(instance);
      clearInstancesUnlocked();
      setError(error, "Unable to initialize DSP parameter bytes for " + runtime.type);
      return false;
    }
    const auto tap = runtime.tapId == 0 ? runtime.logicalId : runtime.tapId;
    if (engine_->setInstanceTap(instance, tap) != ET_OK) {
      engine_->destroyInstance(instance);
      clearInstancesUnlocked();
      setError(error, "Unable to set DSP telemetry tap for " + runtime.type);
      return false;
    }
    try {
      const auto inserted = instances_
                                .emplace(runtime.logicalId,
                                         InstanceEntry{instance, runtime.paramsHash,
                                                       kernel->second.index})
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
    for (const auto &[key, asset] : assets_) {
      (void)key;
      if (asset.logicalId == runtime.logicalId) {
        (void)stageAssetUnlocked(asset);
      }
    }
  }

  try {
    const auto descriptor = encodePipelineDescriptor(
        pipeline, [this](const std::uint32_t logicalId) { return resolveInstance(logicalId); });
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
  refreshLatencyUnlocked();
  return combined_ != nullptr;
}

bool EngineHost::updateDescriptor(const PipelineState &pipeline, std::string *error) {
  std::scoped_lock lock(engineMutex_);
  if (!prepared_) {
    setError(error, "DSP engine has not been prepared");
    return false;
  }
  try {
    const auto descriptor = encodePipelineDescriptor(
        pipeline, [this](const std::uint32_t logicalId) { return resolveInstance(logicalId); });
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
  refreshLatencyUnlocked();
  return true;
}

bool EngineHost::makeDescriptorCommand(
    const PipelineState &pipeline, const std::span<const RuntimePlugin> runtimePlugins,
    AudioCommand &command, std::string *error) const {
  try {
    const auto descriptor = encodePipelineDescriptor(
        pipeline, [this, runtimePlugins](const std::uint32_t logicalId) {
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
  return prepared_ &&
         updateParametersUnlocked(logicalId, packed, paramsHash, parameterBytes);
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
  return true;
}

bool EngineHost::setAsset(RuntimeAsset asset, std::string *error) {
  std::scoped_lock lock(engineMutex_);
  const auto key = (static_cast<std::uint64_t>(asset.logicalId) << 32u) | asset.slot;
  std::uint64_t aggregate = asset.footprintBytes;
  for (const auto &[cachedKey, cached] : assets_) {
    if (cachedKey != key) {
      aggregate += cached.footprintBytes;
    }
  }
  if (aggregate > kAggregateAssetBudgetBytes) {
    setError(error, "DSP asset budget exceeded");
    return false;
  }
  const auto cached = assets_.find(key);
  const auto instance = instances_.find(asset.logicalId);
  if (cached != assets_.end() && instance != instances_.end() && sameAsset(cached->second, asset)) {
    const auto state = engine_->instanceAssetState(instance->second.instance, asset.slot) & 0xffu;
    if (state >= ET_ASSET_STATE_STAGED && state <= ET_ASSET_STATE_ACTIVE) {
      return true;
    }
  }
  if (!stageAssetUnlocked(asset, error)) {
    return false;
  }
  const auto logicalId = asset.logicalId;
  const auto slot = asset.slot;
  try {
    assets_.insert_or_assign(key, std::move(asset));
  } catch (const std::bad_alloc &) {
    const auto found = instances_.find(logicalId);
    if (found != instances_.end()) {
      engine_->abortInstanceAsset(found->second.instance, slot);
    }
    setError(error, "DSP asset cache could not be allocated");
    return false;
  }
  return true;
}

bool EngineHost::clearAsset(const std::uint32_t logicalId, const std::uint32_t slot) {
  std::scoped_lock lock(engineMutex_);
  const auto found = instances_.find(logicalId);
  if (found != instances_.end()) {
    engine_->abortInstanceAsset(found->second.instance, slot);
  }
  const auto key = (static_cast<std::uint64_t>(logicalId) << 32u) | slot;
  assets_.erase(key);
  refreshLatencyUnlocked();
  return found != instances_.end();
}

std::uint32_t EngineHost::assetState(const std::uint32_t logicalId,
                                     const std::uint32_t slot) const {
  std::scoped_lock lock(engineMutex_);
  const auto found = instances_.find(logicalId);
  return found == instances_.end()
             ? static_cast<std::uint32_t>(ET_ASSET_STATE_NONE)
             : engine_->instanceAssetState(found->second.instance, slot);
}

void EngineHost::retainAssets(const std::span<const std::uint32_t> logicalIds) {
  std::scoped_lock lock(engineMutex_);
  for (auto item = assets_.begin(); item != assets_.end();) {
    const auto retained = std::find(logicalIds.begin(), logicalIds.end(), item->second.logicalId) !=
                          logicalIds.end();
    if (!retained) {
      item = assets_.erase(item);
    } else {
      ++item;
    }
  }
}

bool EngineHost::applyCommandUnlocked(const AudioCommand &command) noexcept {
  switch (command.type) {
  case AudioCommandType::setParameters:
    if (command.floatCount > command.packed.size() ||
        command.parameterByteCount > command.parameterBytes.size()) {
      return false;
    }
    if (!updateParametersUnlocked(
        command.logicalId,
        std::span<const float>(command.packed.data(), command.floatCount), command.paramsHash,
        std::span<const std::uint8_t>(command.parameterBytes.data(),
                                     command.parameterByteCount))) {
      return false;
    }
    refreshLatencyUnlocked();
    return true;
  case AudioCommandType::setDescriptor:
    if (command.descriptorByteCount > command.descriptor.size() ||
        !validDescriptor(command.descriptor.data(), command.descriptorByteCount)) {
      return false;
    }
    {
      auto descriptor = command.descriptor;
      const auto nodeCount = readUint32(descriptor.data() + 4u);
      for (std::uint32_t index = 0; index < nodeCount; ++index) {
        auto *node = descriptor.data() + kPipelineDescriptorHeaderBytes +
                     index * kPipelineDescriptorNodeBytes;
        const auto instance = instances_.find(readUint32(node));
        if (instance == instances_.end()) {
          return false;
        }
        writeUint32(node, instance->second.instance);
      }
      if (engine_->configurePipeline(descriptor.data(), command.descriptorByteCount) != ET_OK) {
        return false;
      }
      storeActiveDescriptorUnlocked(descriptor.data(), command.descriptorByteCount);
      return true;
    }
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
      frameCount == 0 || frameCount > kMaxProcessFrames || !std::isfinite(timeSeconds)) {
    return false;
  }
  std::unique_lock lock(engineMutex_, std::try_to_lock);
  if (!lock.owns_lock() || !prepared_ || combined_ == nullptr) {
    return false;
  }

  if (commands != nullptr) {
    AudioCommand command;
    while (commands->pop(command)) {
      (void)applyCommandUnlocked(command);
    }
  }
  if (parameterMailbox != nullptr) {
    parameterMailbox->consumePending(
        [this](const AudioCommand &command) noexcept { (void)applyCommandUnlocked(command); });
  }

  for (std::uint32_t channel = 0; channel < channelCount; ++channel) {
    if (channels[channel] == nullptr) {
      return false;
    }
    std::memcpy(combined_ + static_cast<std::size_t>(channel) * frameCount, channels[channel],
                sizeof(float) * frameCount);
  }
  const auto status =
      engine_->processPipeline(channelCount, frameCount, timeSeconds, masterBypass ? 1u : 0u);
  if (status != ET_OK) {
    return false;
  }
  refreshLatencyUnlocked();
  for (std::uint32_t channel = 0; channel < channelCount; ++channel) {
    std::memcpy(channels[channel], combined_ + static_cast<std::size_t>(channel) * frameCount,
                sizeof(float) * frameCount);
  }
  processedFrames_ += frameCount;
  drainTelemetryUnlocked();
  return true;
}

std::uint32_t EngineHost::pipelineLatency() const {
  return pipelineLatency_.load(std::memory_order_acquire);
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

void EngineHost::refreshLatencyUnlocked() noexcept {
  std::array<std::uint32_t, 5> busLatency{};
  if (!validDescriptor(activeDescriptor_.data(), activeDescriptorByteCount_)) {
    const auto previous = pipelineLatency_.exchange(0, std::memory_order_acq_rel);
    if (previous != 0) {
      latencyRevision_.fetch_add(1, std::memory_order_acq_rel);
    }
    return;
  }
  const auto nodeCount = readUint32(activeDescriptor_.data() + 4u);
  for (std::uint32_t index = 0; index < nodeCount; ++index) {
    const auto *node = activeDescriptor_.data() + kPipelineDescriptorHeaderBytes +
                       index * kPipelineDescriptorNodeBytes;
    const auto inputBus = node[5];
    const auto outputBus = node[6];
    if (node[4] == 0 || inputBus > kMaxBus || outputBus > kMaxBus) {
      continue;
    }
    const auto instanceLatency = engine_->instanceLatency(readUint32(node));
    const auto routedLatency = busLatency[inputBus] + instanceLatency;
    if (inputBus == outputBus || routedLatency > busLatency[outputBus]) {
      busLatency[outputBus] = routedLatency;
    }
  }
  const auto previous = pipelineLatency_.exchange(busLatency[0], std::memory_order_acq_rel);
  if (previous != busLatency[0]) {
    latencyRevision_.fetch_add(1, std::memory_order_acq_rel);
  }
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
