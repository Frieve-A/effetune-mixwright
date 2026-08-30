#include "engine/dry_delay.h"

#include <algorithm>
#include <limits>

namespace effetune::vst {

namespace {

[[nodiscard]] bool storageSize(const std::uint32_t channels, const std::uint32_t frames,
                               std::size_t &size) noexcept {
  if (channels == 0 || frames == 0 ||
      frames > std::numeric_limits<std::size_t>::max() / channels) {
    return false;
  }
  size = static_cast<std::size_t>(channels) * frames;
  return true;
}

} // namespace

bool DryDelayLine::prepare(const std::uint32_t channels, const std::uint32_t maxBlockFrames,
                           const std::uint32_t delayFrames) {
  if (channels == 0 || channels > outputPointers_.size() || maxBlockFrames == 0 ||
      delayFrames == std::numeric_limits<std::uint32_t>::max()) {
    return false;
  }
  const auto capacityFrames = delayFrames + 1u;
  std::size_t historySize = 0;
  std::size_t outputSize = 0;
  if (!storageSize(channels, capacityFrames, historySize) ||
      !storageSize(channels, maxBlockFrames, outputSize)) {
    return false;
  }

  try {
    std::vector<float> history(historySize, 0.0f);
    std::vector<float> output(outputSize, 0.0f);
    history_.swap(history);
    output_.swap(output);
  } catch (...) {
    return false;
  }

  channels_ = channels;
  maxBlockFrames_ = maxBlockFrames;
  capacityFrames_ = capacityFrames;
  delayFrames_ = delayFrames;
  writeFrame_ = 0;
  historyFrames_ = 0;
  outputPointers_.fill(nullptr);
  for (std::uint32_t channel = 0; channel < channels_; ++channel) {
    outputPointers_[channel] =
        output_.data() + static_cast<std::size_t>(channel) * maxBlockFrames_;
  }
  return true;
}

bool DryDelayLine::setDelay(const std::uint32_t delayFrames) {
  if (channels_ == 0 || delayFrames == std::numeric_limits<std::uint32_t>::max()) {
    return false;
  }
  const auto requiredFrames = delayFrames + 1u;
  if (requiredFrames <= capacityFrames_) {
    delayFrames_ = delayFrames;
    return true;
  }

  Update update;
  if (!prepareUpdate(update, channels_, delayFrames)) {
    return false;
  }
  applyUpdate(update);
  return true;
}

bool DryDelayLine::prepareUpdate(Update &update, const std::uint32_t channels,
                                 const std::uint32_t delayFrames) noexcept {
  std::size_t nextSize = 0;
  if (channels == 0 || channels > 8 ||
      delayFrames == std::numeric_limits<std::uint32_t>::max() ||
      !storageSize(channels, delayFrames + 1u, nextSize)) {
    return false;
  }
  try {
    update.history.assign(nextSize, 0.0f);
  } catch (...) {
    return false;
  }
  update.delayFrames = delayFrames;
  return true;
}

void DryDelayLine::applyUpdate(Update &update) noexcept {
  const auto requiredFrames = update.delayFrames + 1u;
  // The owner validates the configuration before committing the wet update.
  // Shrinks retain the existing capacity and history, just like unchanged wet
  // compensation delays. No vector is destroyed at this boundary.
  if (requiredFrames <= capacityFrames_) {
    delayFrames_ = update.delayFrames;
    return;
  }
  const auto keepFrames = std::min(historyFrames_, requiredFrames);
  const auto firstFrame =
      (writeFrame_ + capacityFrames_ - keepFrames) % capacityFrames_;
  for (std::uint32_t channel = 0; channel < channels_; ++channel) {
    const auto oldOffset = static_cast<std::size_t>(channel) * capacityFrames_;
    const auto newOffset = static_cast<std::size_t>(channel) * requiredFrames;
    for (std::uint32_t frame = 0; frame < keepFrames; ++frame) {
      update.history[newOffset + frame] =
          history_[oldOffset + (firstFrame + frame) % capacityFrames_];
    }
  }
  history_.swap(update.history);
  capacityFrames_ = requiredFrames;
  delayFrames_ = update.delayFrames;
  historyFrames_ = keepFrames;
  writeFrame_ = keepFrames % capacityFrames_;
}

void DryDelayLine::reset() noexcept {
  std::fill(history_.begin(), history_.end(), 0.0f);
  std::fill(output_.begin(), output_.end(), 0.0f);
  writeFrame_ = 0;
  historyFrames_ = 0;
}

const float *const *DryDelayLine::process(const float *const *input,
                                          const std::uint32_t channels,
                                          const std::uint32_t frames) noexcept {
  if (input == nullptr || channels != channels_ || frames == 0 || frames > maxBlockFrames_ ||
      capacityFrames_ == 0) {
    return nullptr;
  }
  for (std::uint32_t channel = 0; channel < channels; ++channel) {
    if (input[channel] == nullptr) {
      return nullptr;
    }
  }

  for (std::uint32_t frame = 0; frame < frames; ++frame) {
    for (std::uint32_t channel = 0; channel < channels; ++channel) {
      const auto historyOffset = static_cast<std::size_t>(channel) * capacityFrames_;
      auto *output = output_.data() + static_cast<std::size_t>(channel) * maxBlockFrames_;
      if (delayFrames_ == 0) {
        output[frame] = input[channel][frame];
      } else if (historyFrames_ >= delayFrames_) {
        const auto readFrame =
            (writeFrame_ + capacityFrames_ - delayFrames_) % capacityFrames_;
        output[frame] = history_[historyOffset + readFrame];
      } else {
        output[frame] = 0.0f;
      }
      history_[historyOffset + writeFrame_] = input[channel][frame];
    }
    writeFrame_ = (writeFrame_ + 1u) % capacityFrames_;
    historyFrames_ = std::min(historyFrames_ + 1u, capacityFrames_);
  }
  return outputPointers_.data();
}

} // namespace effetune::vst
