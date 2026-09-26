#include "engine/output_analyzers.h"

#include "engine.h"
#include "effetune/abi.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <unordered_set>

namespace effetune::vst {
namespace {
constexpr std::array<std::string_view, 7> kAnalyzerTypes{
    "SpectrumAnalyzerPlugin", "SpectrogramPlugin", "OscilloscopePlugin",
    "StereoMeterPlugin", "LevelMeterPlugin", "NoteSpectrogramPlugin", "ChromaSpiralPlugin"};
constexpr std::array<std::string_view, 26> kChannels{
    "", "L", "R", "1", "2", "3", "4", "5", "6", "7", "8", "9",
    "10", "11", "12", "13", "14", "15", "16", "34", "56", "78",
    "910", "1112", "1314", "1516"};

std::array<std::uint32_t, 2> selectedChannels(const std::string &channel) {
  if (channel.empty()) return {0, 1};
  if (channel == "L") return {0, 0};
  if (channel == "R") return {1, 1};
  const auto value = static_cast<std::uint32_t>(std::stoul(channel));
  if (value <= 16) return {value - 1, value - 1};
  const auto half = channel.size() / 2;
  return {static_cast<std::uint32_t>(std::stoul(channel.substr(0, half))) - 1,
          static_cast<std::uint32_t>(std::stoul(channel.substr(half))) - 1};
}
void setError(std::string *error, const char *message) { if (error) *error = message; }
} // namespace

struct OutputAnalyzers::Storage {
  static constexpr std::uint32_t kFrames = 256;
  static constexpr std::uint32_t kSlots = 256;
  struct Block {
    std::array<std::array<float, kFrames>, 8> audio{};
    std::uint32_t frames = 0;
    std::uint32_t channels = 0;
    double sampleRate = 0;
  };
  struct Instance {
    OutputAnalyzerSource source;
    et_instance instance = 0;
    std::array<std::uint32_t, 2> channels{};
  };
  std::array<Block, kSlots> blocks;
  std::atomic<std::uint32_t> read{0};
  std::atomic<std::uint32_t> write{0};
  std::atomic<std::uint32_t> dropped{0};
  // Control-owned fields, never inspected by the audio callback.
  std::unique_ptr<effetune::Engine> engine;
  std::uint64_t processedFrames = 0;
  std::vector<Instance> instances;
  std::array<float, kFrames * 2> stereo{};
};

OutputAnalyzers::OutputAnalyzers() : storage_(std::make_unique<Storage>()) {}
OutputAnalyzers::~OutputAnalyzers() = default;

bool OutputAnalyzers::validSource(const OutputAnalyzerSource &source) noexcept {
  return source.tapId >= 0xf0000000u && source.paramsHash != 0 &&
         std::find(kAnalyzerTypes.begin(), kAnalyzerTypes.end(), source.type) != kAnalyzerTypes.end() &&
         std::find(kChannels.begin(), kChannels.end(), source.channel) != kChannels.end() &&
         source.parameters.size() <= 1024 && std::isfinite(source.gain) &&
         source.gain >= 0.0630957f && source.gain <= 15.848933f &&
         std::all_of(source.parameters.begin(), source.parameters.end(),
                     [](float value) { return std::isfinite(value); });
}

bool OutputAnalyzers::configure(std::string *error) {
  auto &storage = *storage_;
  if (sources_.empty()) {
    storage.instances.clear();
    storage.engine.reset();
    return true;
  }
  if (sampleRate_ == 0) return true;
  if (!storage.engine) {
    storage.engine = std::make_unique<effetune::Engine>();
    storage.processedFrames = 0;
    if (storage.engine->prepare(static_cast<float>(sampleRate_), 2, Storage::kFrames,
                                 kTelemetryBytes) != ET_OK) {
      storage.engine.reset();
      setError(error, "Unable to prepare output analysis");
      return false;
    }
  }
  // Preserve history and telemetry sequence for every unchanged source.
  for (auto it = storage.instances.begin(); it != storage.instances.end();) {
    if (std::find(sources_.begin(), sources_.end(), it->source) != sources_.end()) {
      ++it;
    } else {
      storage.engine->destroyInstance(it->instance);
      it = storage.instances.erase(it);
    }
  }
  for (const auto &source : sources_) {
    if (std::any_of(storage.instances.begin(), storage.instances.end(),
                    [&](const auto &entry) { return entry.source == source; })) continue;
    const auto instance = storage.engine->createInstance(source.type.c_str());
    if (instance == 0 || storage.engine->setInstanceTap(instance, source.tapId) != ET_OK ||
        storage.engine->setInstanceParams(instance, source.parameters.data(),
            static_cast<std::uint32_t>(source.parameters.size()), source.paramsHash, 0) != ET_OK) {
      if (instance) storage.engine->destroyInstance(instance);
      setError(error, "Unable to configure output analysis");
      return false;
    }
    storage.instances.push_back({source, instance, selectedChannels(source.channel)});
  }
  return true;
}

bool OutputAnalyzers::setSources(std::vector<OutputAnalyzerSource> sources, std::string *error) {
  if (sources.size() > kMaxSources) {
    setError(error, "Too many output analyzers");
    return false;
  }
  std::unordered_set<std::uint32_t> taps;
  for (const auto &source : sources) {
    if (!validSource(source) || !taps.insert(source.tapId).second) {
      setError(error, "Invalid output analyzer source");
      return false;
    }
  }
  std::scoped_lock lock(controlMutex_);
  sources_ = std::move(sources);
  const auto configured = configure(error);
  enabled_.store(configured && !sources_.empty(), std::memory_order_release);
  if (sources_.empty()) storage_->read.store(storage_->write.load(std::memory_order_acquire),
                                           std::memory_order_release);
  return configured;
}

bool OutputAnalyzers::prepare(const double sampleRate, const std::uint32_t channels,
                              const std::uint32_t maxFrames, std::string *error) {
  if (!std::isfinite(sampleRate) || sampleRate <= 0 || channels == 0 || channels > 8 || maxFrames == 0) {
    setError(error, "Invalid output analysis dimensions");
    return false;
  }
  std::scoped_lock lock(controlMutex_);
  if (sampleRate != sampleRate_ || channels != channels_) {
    storage_->engine.reset();
    storage_->instances.clear();
    sampleRate_ = sampleRate;
    channels_ = channels;
  }
  const auto configured = configure(error);
  // Lifecycle changes must not restart capture after the editor closed. A new
  // explicit source message is what re-enables it; preparation only disables
  // an unavailable configuration.
  if (!configured) enabled_.store(false, std::memory_order_release);
  return configured;
}

void OutputAnalyzers::process(float *const *output, const std::uint32_t channels,
                              const std::uint32_t frames, const double sampleRate) noexcept {
  if (!enabled_.load(std::memory_order_acquire) || channels == 0 || channels > 8) return;
  auto &storage = *storage_;
  for (std::uint32_t offset = 0; offset < frames;) {
    const auto write = storage.write.load(std::memory_order_relaxed);
    const auto next = (write + 1u) % Storage::kSlots;
    if (next == storage.read.load(std::memory_order_acquire)) {
      storage.dropped.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    auto &block = storage.blocks[write];
    block.frames = std::min(Storage::kFrames, frames - offset);
    block.channels = channels;
    block.sampleRate = sampleRate;
    for (std::uint32_t channel = 0; channel < channels; ++channel)
      std::memcpy(block.audio[channel].data(), output[channel] + offset, sizeof(float) * block.frames);
    storage.write.store(next, std::memory_order_release);
    offset += block.frames;
  }
}

std::uint32_t OutputAnalyzers::readTelemetry(std::span<std::uint8_t> output,
                                            std::uint32_t &dropped) noexcept {
  std::scoped_lock lock(controlMutex_);
  auto &storage = *storage_;
  auto read = storage.read.load(std::memory_order_relaxed);
  const auto write = storage.write.load(std::memory_order_acquire);
  dropped = storage.dropped.exchange(0, std::memory_order_relaxed);
  while (read != write) {
    const auto &block = storage.blocks[read];
    if (storage.engine && block.sampleRate == sampleRate_ && block.channels == channels_) {
      // Analyzer state outlives host activation and transport clocks. Its time
      // advances only with consumed PCM and resets with the analyzer engine.
      // Every source observes the same start time for this captured block.
      const auto time = static_cast<double>(storage.processedFrames) / sampleRate_;
      for (const auto &entry : storage.instances) {
        for (std::size_t side = 0; side < 2; ++side) {
          auto *destination = storage.stereo.data() + side * block.frames;
          const auto channel = entry.channels[side];
          if (channel >= block.channels) std::fill_n(destination, block.frames, 0.0f);
          else for (std::uint32_t frame = 0; frame < block.frames; ++frame)
            destination[frame] = block.audio[channel][frame] * entry.source.gain;
        }
        if (storage.engine->processInstance(entry.instance, storage.stereo.data(), 2,
                                             block.frames, time) != ET_OK) ++dropped;
      }
      storage.processedFrames += block.frames;
    }
    read = (read + 1u) % Storage::kSlots;
    storage.read.store(read, std::memory_order_release);
  }
  if (!storage.engine) return 0;
  std::uint32_t engineDropped = 0;
  const auto bytes = storage.engine->readTelemetry(output.data(), static_cast<std::uint32_t>(output.size()),
                                                   &engineDropped);
  dropped += engineDropped;
  return bytes;
}

void OutputAnalyzers::discardTelemetry() noexcept {
  std::scoped_lock lock(controlMutex_);
  storage_->read.store(storage_->write.load(std::memory_order_acquire), std::memory_order_release);
}

} // namespace effetune::vst
