#include "engine/resampler.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <complex>
#include <cstdint>
#include <exception>
#include <iostream>
#include <numbers>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace effetune::vst;

void expect(const bool condition, const std::string &message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

double magnitudeAt(const std::vector<float> &coefficients, const double radians) {
  std::complex<double> response{};
  for (std::size_t index = 0; index < coefficients.size(); ++index) {
    response += static_cast<double>(coefficients[index]) *
                std::polar(1.0, -radians * static_cast<double>(index));
  }
  return std::abs(response);
}

double toneAmplitude(const float *samples, const std::uint32_t first,
                     const std::uint32_t last, const double cyclesPerSample) {
  std::complex<double> projection{};
  for (std::uint32_t index = first; index < last; ++index) {
    projection += static_cast<double>(samples[index]) *
                  std::polar(1.0, -2.0 * std::numbers::pi * cyclesPerSample *
                                      static_cast<double>(index));
  }
  return 2.0 * std::abs(projection) / static_cast<double>(last - first);
}

void testFilterDesign() {
  constexpr std::array qualities{FilterQuality::low, FilterQuality::medium,
                                 FilterQuality::high, FilterQuality::ultra};
  constexpr std::array<std::array<std::uint32_t, 3>, 4> expectedTaps{{
      {119, 59, 31}, {203, 99, 47}, {335, 167, 83}, {511, 255, 127}}};
  constexpr std::array passEdges{0.45, 0.46, 0.47, 0.475};
  // Low/Medium/High encode the documented approximately 90/120/150 dB
  // contracts. Ultra is bounded by the measured float32 coefficient floor.
  constexpr std::array linearLimits{3.4e-5, 1.1e-6, 5.0e-8, 4.0e-8};
  constexpr std::array minimumStopLimits{3.4e-5, 1.1e-6, 5.0e-8, 4.0e-8};

  for (std::size_t qualityIndex = 0; qualityIndex < qualities.size(); ++qualityIndex) {
    for (std::uint32_t stage = 0; stage < 3; ++stage) {
      const auto linear = FirDesigner::designHalfBand(
          qualities[qualityIndex], OversamplingPhase::linear, stage);
      expect(linear.coefficients.size() == expectedTaps[qualityIndex][stage],
             "quality/stage tap count");
      expect(FirDesigner::tapCount(qualities[qualityIndex], stage) ==
                 expectedTaps[qualityIndex][stage],
             "tap-count API contract");
      const auto sum = std::accumulate(linear.coefficients.begin(),
                                       linear.coefficients.end(), 0.0);
      expect(std::abs(sum - 1.0) < 2.0e-6, "half-band DC normalization");
      for (std::size_t index = 0; index < linear.coefficients.size(); ++index) {
        expect(std::abs(linear.coefficients[index] -
                        linear.coefficients[linear.coefficients.size() - 1u - index]) <
                   1.0e-7f,
               "linear-phase coefficient symmetry");
      }

      const auto minimum = FirDesigner::designHalfBand(
          qualities[qualityIndex], OversamplingPhase::minimum, stage);
      expect(minimum.energyDelay < linear.energyDelay,
             "minimum-phase energy must move toward the leading edge");
      expect(minimum.energyDelay < static_cast<double>(minimum.coefficients.size()) * 0.35,
             "minimum-phase causal energy concentration");

      double maxMagnitudeDifference = 0.0;
      for (std::uint32_t bin = 0; bin <= 4096; ++bin) {
        const auto radians = std::numbers::pi * static_cast<double>(bin) / 4096.0;
        if (radians <= 0.2 * std::numbers::pi ||
            radians >= 0.8 * std::numbers::pi) {
          maxMagnitudeDifference =
              std::max(maxMagnitudeDifference,
                       std::abs(magnitudeAt(minimum.coefficients, radians) -
                                magnitudeAt(linear.coefficients, radians)));
        }
      }
      expect(maxMagnitudeDifference < 1.0e-4,
             "minimum-phase magnitude parity q" + std::to_string(qualityIndex) + "/s" +
                 std::to_string(stage) + "=" + std::to_string(maxMagnitudeDifference));

      if (stage == 0) {
        double maxPassError = 0.0;
        double maxLinearStop = 0.0;
        double maxMinimumStop = 0.0;
        double maxMinimumMagnitudeDifference = 0.0;
        for (std::uint32_t bin = 0; bin <= 16384; ++bin) {
          const auto radians = std::numbers::pi * static_cast<double>(bin) / 16384.0;
          const auto linearMagnitude = magnitudeAt(linear.coefficients, radians);
          if (radians <= passEdges[qualityIndex] * std::numbers::pi) {
            maxPassError = std::max(maxPassError, std::abs(linearMagnitude - 1.0));
            maxMinimumMagnitudeDifference =
                std::max(maxMinimumMagnitudeDifference,
                         std::abs(magnitudeAt(minimum.coefficients, radians) -
                                  linearMagnitude));
          }
          if (radians >= (1.0 - passEdges[qualityIndex]) * std::numbers::pi) {
            maxLinearStop = std::max(maxLinearStop, linearMagnitude);
            const auto minimumMagnitude = magnitudeAt(minimum.coefficients, radians);
            maxMinimumStop = std::max(maxMinimumStop, minimumMagnitude);
            maxMinimumMagnitudeDifference =
                std::max(maxMinimumMagnitudeDifference,
                         std::abs(minimumMagnitude - linearMagnitude));
          }
        }
        expect(maxPassError < linearLimits[qualityIndex],
               "linear documented-edge pass-band contract");
        expect(maxLinearStop < linearLimits[qualityIndex],
               "linear documented-edge stop-band contract");
        expect(maxMinimumStop < minimumStopLimits[qualityIndex],
               "minimum documented-edge stop-band contract");
        expect(maxMinimumMagnitudeDifference < 1.0e-4,
               "minimum documented-edge magnitude parity");
      }
    }
  }
}

void testOffBitTransparency() {
  constexpr std::uint32_t frames = 257;
  std::array<float, frames> input{};
  std::array<float, frames> output{};
  for (std::uint32_t index = 0; index < frames; ++index) {
    input[index] = std::bit_cast<float>(0x3f000000u + index);
  }
  const float *inputs[] = {input.data()};
  float *outputs[] = {output.data()};
  Oversampler oversampler;
  oversampler.prepare({1, OversamplingPhase::linear, FilterQuality::medium}, 1, frames);
  const auto *unchanged = oversampler.upsample(inputs, frames);
  expect(unchanged != nullptr && oversampler.downsample(unchanged, frames, outputs),
         "off process");
  expect(input == output, "off must be bit transparent");
}

void testImpulseLatency() {
  constexpr std::uint32_t frames = 1024;
  std::array<float, frames> input{};
  std::array<float, frames> output{};
  input[0] = 1.0f;
  const float *inputs[] = {input.data()};
  float *outputs[] = {output.data()};

  constexpr std::array qualities{FilterQuality::low, FilterQuality::medium,
                                 FilterQuality::high, FilterQuality::ultra};
  constexpr std::array factors{2u, 4u, 8u};
  constexpr std::array<std::array<std::uint32_t, 3>, 4> expectedLatencies{{
      {59, 73, 76}, {101, 125, 130}, {167, 208, 218}, {255, 318, 333}}};
  constexpr std::array<std::array<std::uint32_t, 3>, 4> expectedMinimumLatencies{{
      {7, 9, 10}, {10, 13, 14}, {13, 18, 20}, {16, 22, 24}}};
  for (std::size_t qualityIndex = 0; qualityIndex < qualities.size(); ++qualityIndex) {
    for (std::size_t factorIndex = 0; factorIndex < factors.size(); ++factorIndex) {
      const auto factor = factors[factorIndex];
      Oversampler oversampler;
      oversampler.prepare({factor, OversamplingPhase::linear, qualities[qualityIndex]}, 1,
                          frames);
      const auto *upsampled = oversampler.upsample(inputs, frames);
      expect(upsampled != nullptr &&
                 oversampler.downsample(upsampled, frames * factor, outputs),
             "linear impulse round trip");
      const auto peak = static_cast<std::uint32_t>(std::distance(
          output.begin(), std::max_element(output.begin(), output.end(),
                                           [](const float left, const float right) {
                                             return std::abs(left) < std::abs(right);
                                           })));
      const auto reported = oversampler.latencyHostFrames();
      expect(reported == expectedLatencies[qualityIndex][factorIndex],
             "reported linear-phase latency contract");
      expect(peak + 1u >= reported && peak <= reported + 1u,
             "reported linear-phase latency must match the impulse peak for " +
                 std::to_string(factor) + "x (peak=" + std::to_string(peak) +
                 ", reported=" + std::to_string(reported) + ")");
      Oversampler minimum;
      minimum.prepare({factor, OversamplingPhase::minimum, qualities[qualityIndex]}, 1, frames);
      expect(minimum.latencyHostFrames() ==
                 expectedMinimumLatencies[qualityIndex][factorIndex],
             "reported minimum-phase latency contract");
      expect(minimum.latencyHostFrames() < reported,
             "minimum-phase reported latency must be below linear phase");
    }
  }
}

void testNearNyquistImagesAndAliases() {
  constexpr std::uint32_t frames = 4096;
  std::vector<float> input(frames);
  for (std::uint32_t index = 0; index < frames; ++index) {
    input[index] = static_cast<float>(
        std::sin(2.0 * std::numbers::pi * 0.4375 * static_cast<double>(index)));
  }
  const float *inputs[] = {input.data()};
  constexpr std::array qualities{FilterQuality::low, FilterQuality::medium,
                                 FilterQuality::high, FilterQuality::ultra};
  constexpr std::array imageLimits{3.4e-5, 1.1e-6, 8.0e-8, 6.0e-8};
  for (std::size_t qualityIndex = 0; qualityIndex < qualities.size(); ++qualityIndex) {
    for (const auto phase : {OversamplingPhase::linear, OversamplingPhase::minimum}) {
      for (const auto factor : {2u, 4u, 8u}) {
        Oversampler oversampler;
        oversampler.prepare({factor, phase, qualities[qualityIndex]}, 1, frames);
        const auto *upsampled = oversampler.upsample(inputs, frames);
        expect(upsampled != nullptr, "near-Nyquist image-rejection upsample");
        // The projection interval contains an integer number of cycles and is
        // beyond the longest supported start-up transient.
        constexpr std::uint32_t firstUpsampled = frames;
        const auto lastUpsampled = frames * factor;
        const auto desired =
            toneAmplitude(upsampled[0], firstUpsampled, lastUpsampled, 0.4375 / factor);
        const auto image = toneAmplitude(upsampled[0], firstUpsampled, lastUpsampled,
                                         (1.0 - 0.4375) / factor);
        const auto caseName = std::to_string(factor) + "x/q" +
                              std::to_string(qualityIndex) +
                              (phase == OversamplingPhase::linear ? "/linear" : "/minimum");
        expect(std::abs(desired - 1.0) < 1.0e-4,
               "near-Nyquist desired-tone amplitude " + caseName + "=" +
                   std::to_string(desired));
        expect(image < imageLimits[qualityIndex],
               "near-Nyquist upsampling image " + caseName + "=" +
                   std::to_string(image));

        std::vector<float> engineInput(frames * factor);
        for (std::uint32_t index = 0; index < engineInput.size(); ++index) {
          engineInput[index] = static_cast<float>(std::sin(
              2.0 * std::numbers::pi * ((1.0 - 0.4375) / factor) *
              static_cast<double>(index)));
        }
        std::vector<float> aliasedOutput(frames);
        const float *engineInputs[] = {engineInput.data()};
        float *aliasOutputs[] = {aliasedOutput.data()};
        expect(oversampler.downsample(engineInputs, frames * factor, aliasOutputs),
               "near-Nyquist alias-rejection downsample");
        const auto alias = toneAmplitude(aliasedOutput.data(), frames / 2u, frames, 0.4375);
        expect(alias < imageLimits[qualityIndex],
               "near-Nyquist downsampling alias " + caseName + "=" +
                   std::to_string(alias));
      }
    }
  }
}

void testRoundTripTransparency() {
  constexpr std::uint32_t frames = 8192;
  constexpr std::array tones{0.0625, 0.125, 0.25, 0.375, 0.4375};
  constexpr double toneLevel = 0.1;
  std::vector<float> input(frames);
  std::vector<float> output(frames);
  for (std::uint32_t index = 0; index < frames; ++index) {
    double sample = 0.0;
    for (const auto tone : tones) {
      sample += toneLevel *
                std::sin(2.0 * std::numbers::pi * tone * static_cast<double>(index));
    }
    input[index] = static_cast<float>(sample);
  }
  const float *inputs[] = {input.data()};
  float *outputs[] = {output.data()};

  constexpr std::array qualities{FilterQuality::low, FilterQuality::medium,
                                 FilterQuality::high, FilterQuality::ultra};
  for (const auto quality : qualities) {
    for (const auto phase : {OversamplingPhase::linear, OversamplingPhase::minimum}) {
      for (const auto factor : {2u, 4u, 8u}) {
        Oversampler oversampler;
        oversampler.prepare({factor, phase, quality}, 1, frames);
        const auto *upsampled = oversampler.upsample(inputs, frames);
        expect(upsampled != nullptr &&
                   oversampler.downsample(upsampled, frames * factor, outputs),
               "round-trip process");
        constexpr std::uint32_t first = frames / 2u;
        for (const auto tone : tones) {
          const auto reference = toneAmplitude(input.data(), first, frames, tone);
          const auto actual = toneAmplitude(output.data(), first, frames, tone);
          expect(std::abs(actual / reference - 1.0) < 2.5e-4,
                 "pass-band round-trip transparency");
        }
      }
    }
  }
}

} // namespace

int main() {
  try {
    testFilterDesign();
    testOffBitTransparency();
    testImpulseLatency();
    testNearNyquistImagesAndAliases();
    testRoundTripTransparency();
    std::cout << "All EffeTune resampler tests passed\n";
    return 0;
  } catch (const std::exception &exception) {
    std::cerr << "Resampler test failure: " << exception.what() << '\n';
    return 1;
  }
}
