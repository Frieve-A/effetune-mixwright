#include "engine/resampler.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <numbers>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {

using namespace effetune::vst;

std::string_view phaseName(const OversamplingPhase phase) {
  return phase == OversamplingPhase::minimum ? "minimum" : "linear";
}

std::string_view qualityName(const FilterQuality quality) {
  switch (quality) {
  case FilterQuality::low: return "low";
  case FilterQuality::high: return "high";
  case FilterQuality::ultra: return "ultra";
  case FilterQuality::medium: return "medium";
  }
  return "unknown";
}

void runCase(const OversamplingSettings settings, const std::uint32_t blocks) {
  constexpr std::uint32_t channels = 8;
  constexpr std::uint32_t frames = 512;
  constexpr double sampleRate = 48000.0;
  std::vector<std::vector<float>> input(channels, std::vector<float>(frames));
  std::vector<std::vector<float>> output(channels, std::vector<float>(frames));
  std::vector<const float *> inputs(channels);
  std::vector<float *> outputs(channels);
  for (std::uint32_t channel = 0; channel < channels; ++channel) {
    for (std::uint32_t frame = 0; frame < frames; ++frame) {
      input[channel][frame] = static_cast<float>(
          0.25 * std::sin(2.0 * std::numbers::pi *
                          (0.01 + 0.002 * static_cast<double>(channel)) * frame));
    }
    inputs[channel] = input[channel].data();
    outputs[channel] = output[channel].data();
  }

  Oversampler oversampler;
  oversampler.prepare(settings, channels, frames);
  for (std::uint32_t warmup = 0; warmup < 4; ++warmup) {
    const auto *upsampled = oversampler.upsample(inputs.data(), frames);
    if (upsampled == nullptr ||
        !oversampler.downsample(upsampled, frames * settings.factor, outputs.data())) {
      throw std::runtime_error("Resampler benchmark warmup failed");
    }
  }

  const auto start = std::chrono::steady_clock::now();
  for (std::uint32_t block = 0; block < blocks; ++block) {
    const auto *upsampled = oversampler.upsample(inputs.data(), frames);
    if (upsampled == nullptr ||
        !oversampler.downsample(upsampled, frames * settings.factor, outputs.data())) {
      throw std::runtime_error("Resampler benchmark failed");
    }
  }
  const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start);
  const auto audioDuration = static_cast<double>(blocks * frames) / sampleRate;
  const auto millisecondsPerBlock = elapsed.count() * 1000.0 / blocks;
  const auto realtimePercent = elapsed.count() / audioDuration * 100.0;
  std::cout << settings.factor << "x " << phaseName(settings.phase) << ' '
            << qualityName(settings.quality) << ": " << std::fixed << std::setprecision(3)
            << millisecondsPerBlock << " ms/block, " << std::setprecision(1)
            << realtimePercent << "% of one real-time core"
            << ", checksum=" << output.back().back() << '\n';
}

} // namespace

int main() {
  try {
    std::cout << "EffeTune resampler benchmark: 48 kHz, 512 frames, 8 channels\n";
    runCase({2, OversamplingPhase::linear, FilterQuality::medium}, 100);
    runCase({8, OversamplingPhase::linear, FilterQuality::ultra}, 30);
    runCase({8, OversamplingPhase::minimum, FilterQuality::ultra}, 20);
    return 0;
  } catch (const std::exception &exception) {
    std::cerr << exception.what() << '\n';
    return 1;
  }
}
