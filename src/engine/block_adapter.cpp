#include "engine/block_adapter.h"

#include <algorithm>
#include <stdexcept>

namespace effetune::vst {

void PlanarRingBuffer::prepare(const std::uint32_t channels,
                               const std::uint32_t capacityFrames) {
  if (channels == 0 || capacityFrames == 0) {
    throw std::invalid_argument("Ring buffer dimensions must be non-zero");
  }
  channels_ = channels;
  capacity_ = capacityFrames;
  data_.assign(static_cast<std::size_t>(channels_) * capacity_, 0.0f);
  reset();
}

void PlanarRingBuffer::reset() noexcept {
  std::fill(data_.begin(), data_.end(), 0.0f);
  readPosition_ = 0;
  writePosition_ = 0;
  size_ = 0;
}

float &PlanarRingBuffer::sample(const std::uint32_t channel,
                                const std::uint32_t frame) noexcept {
  return data_[static_cast<std::size_t>(channel) * capacity_ + frame];
}

bool PlanarRingBuffer::write(const float *const *source, const std::uint32_t frames) noexcept {
  if (source == nullptr || frames > capacity_ - size_) {
    return false;
  }
  for (std::uint32_t frame = 0; frame < frames; ++frame) {
    const auto destinationFrame = (writePosition_ + frame) % capacity_;
    for (std::uint32_t channel = 0; channel < channels_; ++channel) {
      if (source[channel] == nullptr) {
        return false;
      }
      sample(channel, destinationFrame) = source[channel][frame];
    }
  }
  writePosition_ = (writePosition_ + frames) % capacity_;
  size_ += frames;
  return true;
}

bool PlanarRingBuffer::write(const float *sourcePlanar, const std::uint32_t sourceStride,
                             const std::uint32_t frames) noexcept {
  if (sourcePlanar == nullptr || frames > capacity_ - size_) {
    return false;
  }
  for (std::uint32_t frame = 0; frame < frames; ++frame) {
    const auto destinationFrame = (writePosition_ + frame) % capacity_;
    for (std::uint32_t channel = 0; channel < channels_; ++channel) {
      sample(channel, destinationFrame) = sourcePlanar[channel * sourceStride + frame];
    }
  }
  writePosition_ = (writePosition_ + frames) % capacity_;
  size_ += frames;
  return true;
}

bool PlanarRingBuffer::writeZeros(const std::uint32_t frames) noexcept {
  if (frames > capacity_ - size_) {
    return false;
  }
  for (std::uint32_t frame = 0; frame < frames; ++frame) {
    const auto destinationFrame = (writePosition_ + frame) % capacity_;
    for (std::uint32_t channel = 0; channel < channels_; ++channel) {
      sample(channel, destinationFrame) = 0.0f;
    }
  }
  writePosition_ = (writePosition_ + frames) % capacity_;
  size_ += frames;
  return true;
}

bool PlanarRingBuffer::read(float *const *destination, const std::uint32_t frames) noexcept {
  if (destination == nullptr || frames > size_) {
    return false;
  }
  for (std::uint32_t frame = 0; frame < frames; ++frame) {
    const auto sourceFrame = (readPosition_ + frame) % capacity_;
    for (std::uint32_t channel = 0; channel < channels_; ++channel) {
      if (destination[channel] == nullptr) {
        return false;
      }
      destination[channel][frame] = sample(channel, sourceFrame);
    }
  }
  readPosition_ = (readPosition_ + frames) % capacity_;
  size_ -= frames;
  return true;
}

bool PlanarRingBuffer::read(float *destinationPlanar, const std::uint32_t destinationStride,
                            const std::uint32_t frames) noexcept {
  if (destinationPlanar == nullptr || frames > size_) {
    return false;
  }
  for (std::uint32_t frame = 0; frame < frames; ++frame) {
    const auto sourceFrame = (readPosition_ + frame) % capacity_;
    for (std::uint32_t channel = 0; channel < channels_; ++channel) {
      destinationPlanar[channel * destinationStride + frame] = sample(channel, sourceFrame);
    }
  }
  readPosition_ = (readPosition_ + frames) % capacity_;
  size_ -= frames;
  return true;
}

void BlockAdapter::prepare(const std::uint32_t channels, const std::uint32_t maxFramesPerCall,
                           const std::uint32_t quantumFrames) {
  if (channels == 0 || channels > kMaxChannels || maxFramesPerCall == 0 || quantumFrames == 0) {
    throw std::invalid_argument("Invalid block adapter configuration");
  }
  channels_ = channels;
  maxFramesPerCall_ = maxFramesPerCall;
  quantumFrames_ = quantumFrames;
  const auto ringCapacity = maxFramesPerCall_ + quantumFrames_ * 2u;
  inputRing_.prepare(channels_, ringCapacity);
  outputRing_.prepare(channels_, ringCapacity);
  quantumBuffer_.assign(static_cast<std::size_t>(channels_) * quantumFrames_, 0.0f);
  reset();
}

void BlockAdapter::reset() noexcept {
  inputRing_.reset();
  outputRing_.reset();
  std::fill(quantumBuffer_.begin(), quantumBuffer_.end(), 0.0f);
  (void)outputRing_.writeZeros(quantumFrames_);
}

} // namespace effetune::vst
