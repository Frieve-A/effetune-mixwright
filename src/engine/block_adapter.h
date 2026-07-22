#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace effetune::vst {

class PlanarRingBuffer {
public:
  void prepare(std::uint32_t channels, std::uint32_t capacityFrames);
  void reset() noexcept;
  [[nodiscard]] bool write(const float *const *source, std::uint32_t frames) noexcept;
  [[nodiscard]] bool write(const float *sourcePlanar, std::uint32_t sourceStride,
                           std::uint32_t frames) noexcept;
  [[nodiscard]] bool writeZeros(std::uint32_t frames) noexcept;
  [[nodiscard]] bool read(float *const *destination, std::uint32_t frames) noexcept;
  [[nodiscard]] bool read(float *destinationPlanar, std::uint32_t destinationStride,
                          std::uint32_t frames) noexcept;
  [[nodiscard]] std::uint32_t available() const noexcept { return size_; }

private:
  [[nodiscard]] float &sample(std::uint32_t channel, std::uint32_t frame) noexcept;

  std::vector<float> data_;
  std::uint32_t channels_ = 0;
  std::uint32_t capacity_ = 0;
  std::uint32_t readPosition_ = 0;
  std::uint32_t writePosition_ = 0;
  std::uint32_t size_ = 0;
};

class BlockAdapter {
public:
  static constexpr std::uint32_t kQuantumFrames = 128;
  static constexpr std::uint32_t kMaxChannels = 8;

  void prepare(std::uint32_t channels, std::uint32_t maxFramesPerCall,
               std::uint32_t quantumFrames = kQuantumFrames);
  void reset() noexcept;

  template <typename ProcessQuantum>
  [[nodiscard]] bool process(const float *const *input, float *const *output,
                             const std::uint32_t frames, ProcessQuantum &&processQuantum) noexcept {
    if (frames > maxFramesPerCall_ || input == nullptr || output == nullptr || channels_ == 0) {
      return false;
    }
    if (!inputRing_.write(input, frames)) {
      return false;
    }

    while (inputRing_.available() >= quantumFrames_) {
      if (!inputRing_.read(quantumBuffer_.data(), quantumFrames_, quantumFrames_)) {
        return false;
      }
      for (std::uint32_t channel = 0; channel < channels_; ++channel) {
        quantumPointers_[channel] = quantumBuffer_.data() + channel * quantumFrames_;
      }
      if (!processQuantum(quantumPointers_.data(), channels_, quantumFrames_)) {
        return false;
      }
      if (!outputRing_.write(quantumBuffer_.data(), quantumFrames_, quantumFrames_)) {
        return false;
      }
    }
    return outputRing_.read(output, frames);
  }

  [[nodiscard]] std::uint32_t latencyFrames() const noexcept { return quantumFrames_; }

private:
  PlanarRingBuffer inputRing_;
  PlanarRingBuffer outputRing_;
  std::vector<float> quantumBuffer_;
  std::array<float *, kMaxChannels> quantumPointers_{};
  std::uint32_t channels_ = 0;
  std::uint32_t maxFramesPerCall_ = 0;
  std::uint32_t quantumFrames_ = kQuantumFrames;
};

} // namespace effetune::vst
