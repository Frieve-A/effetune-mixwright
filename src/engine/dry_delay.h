#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace effetune::vst {

class DryDelayLine {
public:
  struct Update {
    std::vector<float> history;
    std::uint32_t delayFrames = 0;
  };
  [[nodiscard]] bool prepare(std::uint32_t channels, std::uint32_t maxBlockFrames,
                             std::uint32_t delayFrames);
  [[nodiscard]] bool setDelay(std::uint32_t delayFrames);
  [[nodiscard]] static bool prepareUpdate(Update &update, std::uint32_t channels,
                                          std::uint32_t delayFrames) noexcept;
  // Apply swaps retired storage into update; reclaim it on the control thread.
  void applyUpdate(Update &update) noexcept;
  void reset() noexcept;
  [[nodiscard]] const float *const *process(const float *const *input,
                                            std::uint32_t channels,
                                            std::uint32_t frames) noexcept;

private:
  std::vector<float> history_;
  std::vector<float> output_;
  std::array<const float *, 8> outputPointers_{};
  std::uint32_t channels_ = 0;
  std::uint32_t maxBlockFrames_ = 0;
  std::uint32_t capacityFrames_ = 0;
  std::uint32_t delayFrames_ = 0;
  std::uint32_t writeFrame_ = 0;
  std::uint32_t historyFrames_ = 0;
};

} // namespace effetune::vst
