#include "engine/block_adapter.h"

#include <algorithm>
#include <stdexcept>

namespace effetune::vst {

void BlockAdapter::prepare(const std::uint32_t channels, const std::uint32_t maxFramesPerCall,
                           const std::uint32_t maxChunkFrames) {
  if (channels == 0 || channels > kMaxChannels || maxFramesPerCall == 0 || maxChunkFrames == 0) {
    throw std::invalid_argument("Invalid block adapter configuration");
  }
  channels_ = channels;
  maxFramesPerCall_ = maxFramesPerCall;
  maxChunkFrames_ = maxChunkFrames;
  chunkBuffer_.assign(static_cast<std::size_t>(channels_) * maxChunkFrames_, 0.0f);
  reset();
}

void BlockAdapter::reset() noexcept {
  std::fill(chunkBuffer_.begin(), chunkBuffer_.end(), 0.0f);
}

} // namespace effetune::vst
