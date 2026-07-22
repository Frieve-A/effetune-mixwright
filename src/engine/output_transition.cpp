#include "engine/output_transition.h"

#include <algorithm>
#include <cmath>

namespace effetune::vst {

void OutputTransition::reset() noexcept {
  start_.fill(0.0f);
  last_.fill(0.0f);
  totalFrames_ = 0;
  remainingFrames_ = 0;
  previousProcessed_ = false;
  initialized_ = false;
  useDrySource_ = false;
}

void OutputTransition::apply(float *const *output, const float *const *dry,
                             const std::uint32_t channels, const std::uint32_t frames,
                             const double sampleRate, const bool processed) noexcept {
  if (output == nullptr || dry == nullptr || channels == 0 || channels > last_.size() ||
      frames == 0 || !std::isfinite(sampleRate) || sampleRate <= 0.0) {
    return;
  }
  for (std::uint32_t channel = 0; channel < channels; ++channel) {
    if (output[channel] == nullptr || dry[channel] == nullptr) {
      return;
    }
  }

  if (!initialized_) {
    previousProcessed_ = processed;
    initialized_ = true;
    if (!processed) {
      for (std::uint32_t channel = 0; channel < channels; ++channel) {
        std::copy_n(dry[channel], frames, output[channel]);
      }
    }
    for (std::uint32_t channel = 0; channel < channels; ++channel) {
      last_[channel] = output[channel][frames - 1u];
    }
    return;
  }

  if (processed != previousProcessed_) {
    const auto wasTransitioning = remainingFrames_ != 0;
    totalFrames_ = std::max<std::uint32_t>(
        1u, static_cast<std::uint32_t>(std::llround(sampleRate * 0.01)));
    remainingFrames_ = totalFrames_;
    start_ = last_;
    useDrySource_ = processed && !wasTransitioning && initialized_;
    previousProcessed_ = processed;
  }

  for (std::uint32_t frame = 0; frame < frames; ++frame) {
    if (remainingFrames_ != 0) {
      const auto completed = totalFrames_ - remainingFrames_ + 1u;
      const auto mix = static_cast<float>(completed) / static_cast<float>(totalFrames_);
      for (std::uint32_t channel = 0; channel < channels; ++channel) {
        const auto source = useDrySource_ ? dry[channel][frame] : start_[channel];
        const auto target = processed ? output[channel][frame] : dry[channel][frame];
        output[channel][frame] = source + (target - source) * mix;
      }
      --remainingFrames_;
    } else if (!processed) {
      for (std::uint32_t channel = 0; channel < channels; ++channel) {
        output[channel][frame] = dry[channel][frame];
      }
    }
  }

  for (std::uint32_t channel = 0; channel < channels; ++channel) {
    last_[channel] = output[channel][frames - 1u];
  }
}

} // namespace effetune::vst
