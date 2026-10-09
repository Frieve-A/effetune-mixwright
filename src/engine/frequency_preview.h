#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>

namespace effetune::vst {

// Transient editor audition, mixed before the pipeline just like upstream.
// The control side publishes one atomic request; generator state is audio-owned.
class FrequencyPreview {
public:
  enum class Sound { sine, bandpassNoise };
  void setFrequency(const double frequency, const Sound sound = Sound::sine) noexcept {
    // The sign carries the sound in the same lock-free publication as frequency.
    requestedFrequency_.store(std::isfinite(frequency) && frequency > 0.0
                                  ? (sound == Sound::bandpassNoise ? -frequency : frequency) : 0.0,
                              std::memory_order_relaxed);
  }

  void resetAudio() noexcept {
    frequency_ = gain_ = phase_ = noiseFrequency_ = 0.0;
    noiseFade_ = 1.0;
    activeFilter_ = 0;
    filters_ = {};
    random_ = 0x9e3779b9u;
    sound_ = Sound::sine;
  }

  bool mix(float *const *channels, const std::uint32_t channelCount,
           const std::uint32_t frames, const double sampleRate) noexcept {
    const auto requested = requestedFrequency_.load(std::memory_order_relaxed);
    const auto frequency = std::abs(requested);
    const auto active = frequency > 0.0 && frequency < sampleRate * 0.5;
    if (active) {
      frequency_ = frequency;
      sound_ = requested < 0.0 ? Sound::bandpassNoise : Sound::sine;
    }
    if (!active && gain_ == 0.0) return false;
    constexpr auto twoPi = 6.28318530717958647692;
    constexpr auto amplitude = 0.251188643150958;
    const auto phaseStep = twoPi * std::min(frequency_, sampleRate * 0.499) / sampleRate;
    const auto gainStep = 1.0 / (sampleRate * 0.005);
    if (sound_ == Sound::bandpassNoise && noiseFade_ == 1.0 && noiseFrequency_ != frequency_) {
      // Upstream Q=4 TPT band-pass, with fixed-filter crossfade on retuning.
      activeFilter_ = 1u - activeFilter_;
      auto &filter = filters_[activeFilter_];
      const auto g = std::tan(twoPi * 0.5 * frequency_ / sampleRate);
      constexpr auto damping = 0.25;
      filter.a1 = 1.0 / (1.0 + g * (g + damping));
      filter.a2 = g * filter.a1;
      filter.a3 = g * filter.a2;
      filter.scale = std::sqrt(1.5 * damping / filter.a2);
      filter.ic1eq = filter.ic2eq = 0.0;
      noiseFade_ = noiseFrequency_ == 0.0 ? 1.0 : 0.0;
      noiseFrequency_ = frequency_;
    }
    for (std::uint32_t frame = 0; frame < frames; ++frame) {
      gain_ = active ? std::min(1.0, gain_ + gainStep) : std::max(0.0, gain_ - gainStep);
      auto value = std::sin(phase_);
      if (sound_ == Sound::bandpassNoise) {
        random_ ^= random_ << 13u;
        random_ ^= random_ >> 17u;
        random_ ^= random_ << 5u;
        const auto white = static_cast<double>(random_) * (2.0 / 4294967296.0) - 1.0;
        noiseFade_ = std::min(1.0, noiseFade_ + gainStep);
        value = 0.0;
        const auto count = noiseFade_ < 1.0 ? 2u : 1u;
        for (std::uint32_t index = 0; index < count; ++index) {
          auto &filter = filters_[activeFilter_ ^ index];
          const auto v3 = white * filter.scale - filter.ic2eq;
          const auto v1 = filter.a1 * filter.ic1eq + filter.a2 * v3;
          const auto v2 = filter.ic2eq + filter.a2 * filter.ic1eq + filter.a3 * v3;
          filter.ic1eq = 2.0 * v1 - filter.ic1eq;
          filter.ic2eq = 2.0 * v2 - filter.ic2eq;
          value += v1 * (index == 0 ? noiseFade_ : 1.0 - noiseFade_);
        }
      }
      const auto sample = static_cast<float>(value * amplitude * gain_);
      for (std::uint32_t channel = 0; channel < std::min(channelCount, 2u); ++channel) {
        channels[channel][frame] += sample;
      }
      phase_ += phaseStep;
      if (phase_ >= twoPi) phase_ -= twoPi;
    }
    // Include the release ramp: this entire block was exposed to audition PCM.
    return true;
  }

private:
  static_assert(std::atomic<double>::is_always_lock_free);
  std::atomic<double> requestedFrequency_{0.0};
  double frequency_ = 0.0;
  double gain_ = 0.0;
  double phase_ = 0.0;
  Sound sound_ = Sound::sine;
  struct Filter {
    double a1 = 0, a2 = 0, a3 = 0, scale = 0, ic1eq = 0, ic2eq = 0;
  };
  std::array<Filter, 2> filters_{};
  std::uint32_t activeFilter_ = 0;
  std::uint32_t random_ = 0x9e3779b9u;
  double noiseFrequency_ = 0.0;
  double noiseFade_ = 1.0;
};

} // namespace effetune::vst
