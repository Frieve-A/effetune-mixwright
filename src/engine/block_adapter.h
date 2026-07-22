#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace effetune::vst {

class BlockAdapter {
public:
  static constexpr std::uint32_t kMaxChunkFrames = 128;
  static constexpr std::uint32_t kMaxChannels = 8;

  void prepare(std::uint32_t channels, std::uint32_t maxFramesPerCall,
               std::uint32_t maxChunkFrames = kMaxChunkFrames);
  void reset() noexcept;

  template <typename ProcessChunk>
  [[nodiscard]] bool process(const float *const *input, float *const *output,
                             const std::uint32_t frames, ProcessChunk &&processChunk) noexcept {
    if (frames > maxFramesPerCall_ || input == nullptr || output == nullptr || channels_ == 0) {
      return false;
    }
    for (std::uint32_t channel = 0; channel < channels_; ++channel) {
      if (input[channel] == nullptr || output[channel] == nullptr) {
        return false;
      }
    }

    for (std::uint32_t offset = 0; offset < frames; offset += maxChunkFrames_) {
      const auto chunkFrames = std::min(maxChunkFrames_, frames - offset);
      for (std::uint32_t channel = 0; channel < channels_; ++channel) {
        chunkPointers_[channel] = chunkBuffer_.data() + channel * maxChunkFrames_;
        std::copy_n(input[channel] + offset, chunkFrames, chunkPointers_[channel]);
      }
      if (!processChunk(chunkPointers_.data(), channels_, chunkFrames)) {
        return false;
      }
      for (std::uint32_t channel = 0; channel < channels_; ++channel) {
        std::copy_n(chunkPointers_[channel], chunkFrames, output[channel] + offset);
      }
    }
    return true;
  }

  [[nodiscard]] std::uint32_t latencyFrames() const noexcept { return 0; }

private:
  std::vector<float> chunkBuffer_;
  std::array<float *, kMaxChannels> chunkPointers_{};
  std::uint32_t channels_ = 0;
  std::uint32_t maxFramesPerCall_ = 0;
  std::uint32_t maxChunkFrames_ = kMaxChunkFrames;
};

} // namespace effetune::vst
