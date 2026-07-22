#pragma once

#include <array>
#include <cstdint>

namespace effetune::vst {

class OutputTransition {
public:
  void reset() noexcept;
  void apply(float *const *output, const float *const *dry, std::uint32_t channels,
             std::uint32_t frames, double sampleRate, bool processed) noexcept;

private:
  std::array<float, 8> start_{};
  std::array<float, 8> last_{};
  std::uint32_t totalFrames_ = 0;
  std::uint32_t remainingFrames_ = 0;
  bool previousProcessed_ = false;
  bool initialized_ = false;
  bool useDrySource_ = false;
};

} // namespace effetune::vst
