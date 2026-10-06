#include "engine/spectrum_capture.h"
#include "engine/pipeline_model.h"

#include <algorithm>

namespace effetune::vst {
namespace {
constexpr std::uint64_t kGenerationStep = std::uint64_t{1} << 35;
SpectrumMode modeOf(std::uint64_t token) noexcept {
  return static_cast<SpectrumMode>((token >> 32) & 3u);
}
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
static_assert(std::atomic<SpectrumTap *>::is_always_lock_free);
} // namespace

struct SpectrumCapture::Storage {
  struct Slot {
    std::atomic<std::uint64_t> token{0};
    std::atomic<SpectrumTap *> published{nullptr};
    // Published objects remain alive until destruction with the producer idle.
    std::unique_ptr<SpectrumTap> owner;
    std::uint64_t nativeGeneration = 0, publishedGeneration = 0;
  };
  std::array<Slot, kMaxPipelineNodes> slots;
  std::atomic_bool prepared{false};
  double sampleRate = 0;
  std::uint32_t maxFrames = 0;
  std::uint64_t generation = 0;
  std::uint64_t publishedGeneration = 0;
  // Producer-only state pairs Before/After across concurrent subscription edits.
  std::uint64_t pendingToken = 0, pendingEpoch = 0;
  std::size_t pendingSlot = 0;
  bool bypassed = false;
};

SpectrumCapture::SpectrumCapture() : storage_(std::make_unique<Storage>()) {}
SpectrumCapture::~SpectrumCapture() = default;

void SpectrumCapture::prepare(std::uint32_t maxFrames, double sampleRate) {
  std::scoped_lock lock(controlMutex_);
  auto &s = *storage_;
  s.sampleRate = sampleRate;
  s.maxFrames = maxFrames;
  // Probe the upstream contract without allowing display support to decide
  // whether the audio engine itself can be prepared.
  SpectrumTap probe;
  bool prepared = probe.prepare(sampleRate, maxFrames);
  for (auto &slot : s.slots)
    if (slot.owner) prepared = slot.owner->prepare(sampleRate, maxFrames) && prepared;
  s.prepared.store(prepared, std::memory_order_release);
  invalidate();
}

bool SpectrumCapture::nativeAnalysisSupported() const noexcept {
  return true;
}

bool SpectrumCapture::setTap(std::uint32_t pluginId, SpectrumMode mode, SpectrumQuality quality) {
  if (!pluginId || mode > SpectrumMode::compare || quality > SpectrumQuality::hq) return false;
  std::scoped_lock lock(controlMutex_);
  auto &s = *storage_;
  if (mode != SpectrumMode::off && !s.prepared.load(std::memory_order_acquire)) return false;
  auto selected = s.slots.size();
  for (std::size_t i = 0; i < s.slots.size(); ++i)
    if (static_cast<std::uint32_t>(s.slots[i].token.load(std::memory_order_acquire)) == pluginId) {
      selected = i;
      break;
    }
  if (selected == s.slots.size()) {
    if (mode == SpectrumMode::off) return true;
    for (std::size_t i = 0; i < s.slots.size(); ++i)
      if (modeOf(s.slots[i].token.load(std::memory_order_acquire)) == SpectrumMode::off) {
        selected = i;
        break;
      }
  }
  if (selected == s.slots.size()) return false;
  auto &slot = s.slots[selected];
  if (!slot.owner) {
    auto tap = std::make_unique<SpectrumTap>();
    if (!tap->prepare(s.sampleRate, s.maxFrames)) return false;
    slot.owner = std::move(tap);
    slot.published.store(slot.owner.get(), std::memory_order_release);
  }
  // Close the old adapter token before reusing a slot. Upstream's generation
  // must change even when a different plug-in uses the same mode and quality.
  slot.token.store(0, std::memory_order_release);
  slot.owner->invalidate();
  if (!slot.owner->configure(static_cast<SpectrumTapMode>(mode), static_cast<SpectrumTapQuality>(quality)))
    return false;
  s.generation += kGenerationStep;
  slot.token.store(s.generation | (static_cast<std::uint64_t>(quality) << 34) |
                      (static_cast<std::uint64_t>(mode) << 32) | pluginId,
                   std::memory_order_release);
  const bool active = std::any_of(s.slots.begin(), s.slots.end(), [](const auto &candidate) {
    return modeOf(candidate.token.load(std::memory_order_acquire)) != SpectrumMode::off;
  });
  enabled_.store(active, std::memory_order_release);
  return true;
}

void SpectrumCapture::invalidate() noexcept {
  epoch_.fetch_add(1, std::memory_order_acq_rel);
  for (auto &slot : storage_->slots)
    if (auto *tap = slot.published.load(std::memory_order_acquire)) tap->invalidate();
}

void SpectrumCapture::stopCapture() noexcept {
  std::scoped_lock lock(controlMutex_);
  enabled_.store(false, std::memory_order_release);
  for (auto &slot : storage_->slots) {
    slot.token.store(0, std::memory_order_release);
    if (slot.owner) (void)slot.owner->configure(SpectrumTapMode::Off, SpectrumTapQuality::Normal);
  }
  invalidate();
}

void SpectrumCapture::beginBlock(bool masterBypass) noexcept {
  if (storage_->bypassed != masterBypass) {
    storage_->bypassed = masterBypass;
    invalidate();
  }
}

void SpectrumCapture::observe(std::uint32_t pluginId, const float *audio, std::uint32_t channels,
                              std::uint32_t frames, std::uint32_t latency, double,
                              std::uint64_t firstFrame, bool before, std::uint32_t tapDelayFrames) noexcept {
  auto &s = *storage_;
  if (before) {
    s.pendingToken = 0;
    if (!capturing() || !s.prepared.load(std::memory_order_acquire)) return;
    for (std::size_t i = 0; i < s.slots.size(); ++i) {
      const auto token = s.slots[i].token.load(std::memory_order_acquire);
      if (static_cast<std::uint32_t>(token) != pluginId || modeOf(token) == SpectrumMode::off) continue;
      s.pendingSlot = i;
      s.pendingToken = token;
      s.pendingEpoch = epoch_.load(std::memory_order_acquire);
      break;
    }
  }
  if (!s.pendingToken || s.pendingEpoch != epoch_.load(std::memory_order_acquire)) return;
  auto &slot = s.slots[s.pendingSlot];
  if (slot.token.load(std::memory_order_acquire) != s.pendingToken) return;
  if (auto *tap = slot.published.load(std::memory_order_acquire))
    tap->capture(audio, channels, frames, firstFrame, latency, tapDelayFrames, before);
  if (!before) s.pendingToken = 0;
}

std::vector<SpectrumFrame> SpectrumCapture::read(std::uint32_t &dropped) {
  std::scoped_lock lock(controlMutex_);
  dropped = 0;
  const auto epoch = epoch_.load(std::memory_order_acquire);
  std::vector<SpectrumFrame> result;
  for (auto &slot : storage_->slots) {
    if (!slot.owner) continue;
    const auto token = slot.token.load(std::memory_order_acquire);
    std::uint32_t lost = 0;
    auto frames = slot.owner->read(lost);
    dropped += lost;
    if (modeOf(token) == SpectrumMode::off || !storage_->prepared.load(std::memory_order_acquire)) continue;
    // Unsynchronized overlays display the latest completed analysis per poll.
    if (!frames.empty()) {
      auto &analysis = frames.back();
      if (slot.nativeGeneration != analysis.timing.generation) {
        slot.nativeGeneration = analysis.timing.generation;
        slot.publishedGeneration = ++storage_->publishedGeneration;
      }
      // The WebView generation spans slot reuse as well as upstream timelines.
      analysis.timing.generation = slot.publishedGeneration;
      SpectrumFrame frame;
      frame.pluginId = static_cast<std::uint32_t>(token);
      frame.mode = static_cast<SpectrumMode>(analysis.mode);
      frame.sampleRate = analysis.sampleRate;
      frame.analysis = std::move(analysis);
      result.push_back(std::move(frame));
    }
  }
  if (epoch != epoch_.load(std::memory_order_acquire)) result.clear();
  return result;
}
} // namespace effetune::vst
