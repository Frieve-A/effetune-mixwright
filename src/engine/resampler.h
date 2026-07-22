#pragma once

#include "engine/pipeline_model.h"

#include <array>
#include <complex>
#include <cstdint>
#include <vector>

namespace effetune::vst {

struct ResamplerDesign {
  std::vector<float> coefficients;
  double energyDelay = 0.0;
};

class FirDesigner {
public:
  [[nodiscard]] static std::uint32_t tapCount(FilterQuality quality,
                                              std::uint32_t stageIndex);
  [[nodiscard]] static ResamplerDesign designHalfBand(FilterQuality quality,
                                                      OversamplingPhase phase,
                                                      std::uint32_t stageIndex);

private:
  static void fft(std::vector<std::complex<double>> &values, bool inverse);
  [[nodiscard]] static std::vector<double> minimumPhase(const std::vector<double> &linear);
};

class HalfBandStage {
public:
  void prepare(std::uint32_t channels, const ResamplerDesign &design);
  void reset() noexcept;
  void processUp(const float *const *input, std::uint32_t inputFrames, float *outputPlanar,
                 std::uint32_t outputStride) noexcept;
  void processDown(const float *const *input, std::uint32_t inputFrames, float *outputPlanar,
                   std::uint32_t outputStride) noexcept;

  [[nodiscard]] double energyDelay() const noexcept { return energyDelay_; }
  [[nodiscard]] std::uint32_t tapCount() const noexcept {
    return static_cast<std::uint32_t>(coefficients_.size());
  }

private:
  [[nodiscard]] float filterSample(std::vector<float> &history,
                                   std::uint32_t &position,
                                   std::uint32_t channel, float input) noexcept;

  std::vector<float> coefficients_;
  std::vector<float> reversedCoefficients_;
  std::vector<float> upEvenReversed_;
  std::vector<float> upOddReversed_;
  std::vector<float> upHistory_;
  std::vector<float> downHistory_;
  std::vector<std::uint32_t> upPositions_;
  std::vector<std::uint32_t> downPositions_;
  std::uint32_t channels_ = 0;
  double energyDelay_ = 0.0;
};

class Oversampler {
public:
  static constexpr std::uint32_t kMaxChannels = 8;

  void prepare(const OversamplingSettings &settings, std::uint32_t channels,
               std::uint32_t maxHostFrames);
  void reset() noexcept;

  [[nodiscard]] const float *const *upsample(const float *const *input,
                                             std::uint32_t hostFrames) noexcept;
  [[nodiscard]] bool downsample(const float *const *input, std::uint32_t engineFrames,
                                float *const *output) noexcept;

  [[nodiscard]] std::uint32_t factor() const noexcept { return settings_.factor; }
  [[nodiscard]] std::uint32_t latencyHostFrames() const noexcept { return latencyHostFrames_; }
  [[nodiscard]] std::uint32_t upsampledFrames(std::uint32_t hostFrames) const noexcept {
    return hostFrames * settings_.factor;
  }

private:
  OversamplingSettings settings_;
  std::uint32_t channels_ = 0;
  std::uint32_t maxHostFrames_ = 0;
  std::uint32_t latencyHostFrames_ = 0;
  std::vector<HalfBandStage> upStages_;
  std::vector<HalfBandStage> downStages_;
  std::vector<std::vector<float>> upBuffers_;
  std::vector<std::vector<float>> downBuffers_;
  std::vector<float> passthroughBuffer_;
  std::array<const float *, kMaxChannels> outputPointers_{};
};

} // namespace effetune::vst
