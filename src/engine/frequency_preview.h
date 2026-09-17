#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>

namespace effetune::vst {

// Transient editor audition, mixed before the pipeline just like upstream.
// The control side publishes only a frequency; oscillator state is audio-owned.
class FrequencyPreview {
public:
  void setFrequency(const double frequency) noexcept {
    requestedFrequency_.store(std::isfinite(frequency) && frequency > 0.0
                                  ? frequency : 0.0,
                              std::memory_order_relaxed);
  }

  void resetAudio() noexcept {
    frequency_ = gain_ = phase_ = 0.0;
  }

  void mix(float *const *channels, const std::uint32_t channelCount,
           const std::uint32_t frames, const double sampleRate) noexcept {
    const auto requested = requestedFrequency_.load(std::memory_order_relaxed);
    const auto active = requested > 0.0 && requested < sampleRate * 0.5;
    if (active) frequency_ = requested;
    if (!active && gain_ == 0.0) return;
    constexpr auto twoPi = 6.28318530717958647692;
    constexpr auto amplitude = 0.251188643150958;
    const auto phaseStep = twoPi * std::min(frequency_, sampleRate * 0.499) / sampleRate;
    const auto gainStep = 1.0 / (sampleRate * 0.005);
    for (std::uint32_t frame = 0; frame < frames; ++frame) {
      gain_ = active ? std::min(1.0, gain_ + gainStep) : std::max(0.0, gain_ - gainStep);
      const auto sample = static_cast<float>(std::sin(phase_) * amplitude * gain_);
      for (std::uint32_t channel = 0; channel < std::min(channelCount, 2u); ++channel) {
        channels[channel][frame] += sample;
      }
      phase_ += phaseStep;
      if (phase_ >= twoPi) phase_ -= twoPi;
    }
  }

private:
  static_assert(std::atomic<double>::is_always_lock_free);
  std::atomic<double> requestedFrequency_{0.0};
  double frequency_ = 0.0;
  double gain_ = 0.0;
  double phase_ = 0.0;
};

} // namespace effetune::vst
