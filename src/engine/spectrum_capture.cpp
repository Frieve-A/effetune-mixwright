#include "engine/spectrum_capture.h"
#include "engine/pipeline_model.h"

#include <algorithm>

namespace effetune::vst {
namespace {
constexpr std::uint32_t kBlockFrames = 256;
constexpr std::uint32_t kQueueSlots = 2048;
constexpr std::uint64_t kGenerationStep = std::uint64_t{1} << 34;
SpectrumMode modeOf(std::uint64_t token) noexcept {
  return static_cast<SpectrumMode>((token >> 32) & 3u);
}
float meanSample(const float *audio, std::uint32_t channels,
                 std::uint32_t frames, std::uint32_t frame) noexcept {
  float sum = audio[frame];
  for (std::uint32_t channel = 1; channel < channels; ++channel)
    sum += audio[static_cast<std::size_t>(channel) * frames + frame];
  return sum / static_cast<float>(channels);
}
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
} // namespace

struct SpectrumCapture::Storage {
  struct Block {
    std::array<float, kBlockFrames> input{}, output{};
    std::uint64_t token = 0, epoch = 0, firstFrame = 0;
    double sampleRate = 0;
    std::uint32_t slot = 0, frames = 0, latency = 0;
  };
  struct History {
    SpectrumFrame frame;
    std::vector<float> delay;
    std::uint64_t token = 0, epoch = 0, nextFrame = 0;
    std::uint32_t delayPosition = 0, sinceDelivery = 0;
  };
  std::array<std::atomic<std::uint64_t>, kMaxPipelineNodes> taps{};
  std::array<std::unique_ptr<History>, kMaxPipelineNodes> histories;
  std::array<Block, kQueueSlots> blocks;
  std::atomic<std::uint32_t> read{0}, write{0}, dropped{0};
  // Engine-owner fields. Preparation owns allocation; the callback only copies.
  std::vector<float> input;
  std::uint64_t pendingToken = 0, pendingEpoch = 0;
  std::uint32_t pendingSlot = 0;
  bool bypassed = false;
  // Serialized control-side generation; the callback only reads published tokens.
  std::uint64_t generation = 0;
};

SpectrumCapture::SpectrumCapture() : storage_(std::make_unique<Storage>()) {}
SpectrumCapture::~SpectrumCapture() = default;

void SpectrumCapture::prepare(std::uint32_t maxFrames) {
  storage_->input.resize(maxFrames);
  invalidate();
}

void SpectrumCapture::invalidate() noexcept {
  epoch_.fetch_add(1, std::memory_order_acq_rel);
}

bool SpectrumCapture::setTap(std::uint32_t pluginId, SpectrumMode mode) {
  if (pluginId == 0 || mode > SpectrumMode::compare) return false;
  std::scoped_lock lock(controlMutex_);
  auto &storage = *storage_;
  std::size_t selected = storage.taps.size();
  for (std::size_t index = 0; index < storage.taps.size(); ++index) {
    if (static_cast<std::uint32_t>(storage.taps[index].load(std::memory_order_acquire)) == pluginId) {
      selected = index;
      break;
    }
  }
  if (selected == storage.taps.size()) {
    if (mode == SpectrumMode::off) return true;
    for (std::size_t index = 0; index < storage.taps.size(); ++index) {
      if (modeOf(storage.taps[index].load(std::memory_order_acquire)) == SpectrumMode::off) {
        selected = index;
        break;
      }
    }
  }
  if (selected == storage.taps.size()) return false;
  if (mode != SpectrumMode::off && !storage.histories[selected])
    storage.histories[selected] = std::make_unique<Storage::History>();
  storage.generation += kGenerationStep;
  storage.taps[selected].store(storage.generation | (static_cast<std::uint64_t>(mode) << 32) |
                                  pluginId, std::memory_order_release);
  const bool active = std::any_of(storage.taps.begin(), storage.taps.end(), [](const auto &tap) {
    return modeOf(tap.load(std::memory_order_acquire)) != SpectrumMode::off;
  });
  enabled_.store(active, std::memory_order_release);
  return true;
}

void SpectrumCapture::stopCapture() noexcept {
  std::scoped_lock lock(controlMutex_);
  enabled_.store(false, std::memory_order_release);
  for (auto &tap : storage_->taps) tap.store(0, std::memory_order_release);
  invalidate();
}

void SpectrumCapture::beginBlock(bool masterBypass) noexcept {
  if (storage_->bypassed != masterBypass) {
    storage_->bypassed = masterBypass;
    invalidate();
  }
}

void SpectrumCapture::observe(std::uint32_t pluginId, const float *audio,
                              std::uint32_t channels, std::uint32_t frames,
                              std::uint32_t latency, double sampleRate,
                              std::uint64_t firstFrame, bool before) noexcept {
  auto &storage = *storage_;
  if (before) {
    storage.pendingToken = 0;
    if (!capturing() || frames > storage.input.size()) return;
    for (std::uint32_t index = 0; index < storage.taps.size(); ++index) {
      const auto token = storage.taps[index].load(std::memory_order_acquire);
      if (static_cast<std::uint32_t>(token) != pluginId || modeOf(token) == SpectrumMode::off) continue;
      storage.pendingToken = token;
      storage.pendingSlot = index;
      storage.pendingEpoch = epoch_.load(std::memory_order_acquire);
      if (modeOf(token) == SpectrumMode::compare)
        for (std::uint32_t frame = 0; frame < frames; ++frame)
          storage.input[frame] = meanSample(audio, channels, frames, frame);
      break;
    }
    return;
  }
  const auto token = storage.pendingToken;
  if (token == 0 || token != storage.taps[storage.pendingSlot].load(std::memory_order_acquire) ||
      storage.pendingEpoch != epoch_.load(std::memory_order_acquire)) return;
  for (std::uint32_t offset = 0; offset < frames;) {
    const auto write = storage.write.load(std::memory_order_relaxed);
    const auto next = (write + 1u) % kQueueSlots;
    if (next == storage.read.load(std::memory_order_acquire)) {
      storage.dropped.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    auto &block = storage.blocks[write];
    block.token = token;
    block.epoch = storage.pendingEpoch;
    block.slot = storage.pendingSlot;
    block.firstFrame = firstFrame + offset;
    block.sampleRate = sampleRate;
    block.latency = modeOf(token) == SpectrumMode::compare ? latency : 0;
    block.frames = std::min(kBlockFrames, frames - offset);
    for (std::uint32_t frame = 0; frame < block.frames; ++frame) {
      block.input[frame] = modeOf(token) == SpectrumMode::compare ? storage.input[offset + frame] : 0;
      block.output[frame] = meanSample(audio, channels, frames, offset + frame);
    }
    storage.write.store(next, std::memory_order_release);
    offset += block.frames;
  }
}

std::vector<SpectrumFrame> SpectrumCapture::read(std::uint32_t &dropped) {
  std::scoped_lock lock(controlMutex_);
  auto &storage = *storage_;
  const auto epoch = epoch_.load(std::memory_order_acquire);
  auto read = storage.read.load(std::memory_order_relaxed);
  const auto write = storage.write.load(std::memory_order_acquire);
  dropped = storage.dropped.exchange(0, std::memory_order_relaxed);
  if (dropped != 0) {
    // An overloaded display must resume from fresh, contiguous audio instead of
    // presenting an old queue as a current spectrum or joining a Compare gap.
    storage.read.store(write, std::memory_order_release);
    invalidate();
    return {};
  }
  std::array<bool, kMaxPipelineNodes> changed{};
  while (read != write) {
    const auto &block = storage.blocks[read];
    if (block.epoch == epoch &&
        block.token == storage.taps[block.slot].load(std::memory_order_acquire)) {
      auto &history = *storage.histories[block.slot];
      if (history.token != block.token || history.epoch != epoch ||
          history.nextFrame != block.firstFrame || history.frame.sampleRate != block.sampleRate ||
          history.delay.size() != block.latency) {
        history = {};
        history.token = block.token;
        history.epoch = epoch;
        history.frame.pluginId = static_cast<std::uint32_t>(block.token);
        history.frame.mode = modeOf(block.token);
        history.frame.sampleRate = block.sampleRate;
        history.delay.assign(block.latency, 0.0f);
      }
      for (std::uint32_t frame = 0; frame < block.frames; ++frame) {
        auto input = block.input[frame];
        if (!history.delay.empty()) {
          std::swap(input, history.delay[history.delayPosition]);
          history.delayPosition = (history.delayPosition + 1u) % block.latency;
        }
        const auto position = history.frame.bufferPosition;
        history.frame.input[position] = input;
        history.frame.output[position] = block.output[frame];
        history.frame.bufferPosition = (position + 1u) % SpectrumFrame::kSamples;
      }
      history.nextFrame = block.firstFrame + block.frames;
      history.sinceDelivery += block.frames;
      changed[block.slot] = true;
    }
    read = (read + 1u) % kQueueSlots;
    storage.read.store(read, std::memory_order_release);
  }
  std::vector<SpectrumFrame> result;
  for (std::size_t index = 0; index < changed.size(); ++index) {
    if (!changed[index]) continue;
    auto &history = *storage.histories[index];
    if (history.sinceDelivery < SpectrumFrame::kSamples / 2) continue;
    result.push_back(history.frame);
    history.sinceDelivery = 0;
  }
  // A concurrent topology/lifecycle transition invalidates even a completed drain.
  if (epoch_.load(std::memory_order_acquire) != epoch) result.clear();
  return result;
}

} // namespace effetune::vst
