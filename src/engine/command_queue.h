#pragma once

#include "engine/pipeline_model.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

namespace effetune::vst {

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324) // Intentional cache-line padding for producer/consumer indices.
#endif

template <typename Item, std::size_t Capacity> class SpscQueue {
  static_assert(Capacity >= 2);
  static_assert(std::is_default_constructible_v<Item>);

public:
  [[nodiscard]] bool push(const Item &item) noexcept {
    const auto write = writeIndex_.load(std::memory_order_relaxed);
    const auto next = increment(write);
    if (next == readIndex_.load(std::memory_order_acquire)) {
      return false;
    }
    items_[write] = item;
    writeIndex_.store(next, std::memory_order_release);
    return true;
  }

  [[nodiscard]] bool pop(Item &item) noexcept {
    const auto read = readIndex_.load(std::memory_order_relaxed);
    if (read == writeIndex_.load(std::memory_order_acquire)) {
      return false;
    }
    item = items_[read];
    readIndex_.store(increment(read), std::memory_order_release);
    return true;
  }

  [[nodiscard]] bool empty() const noexcept {
    return readIndex_.load(std::memory_order_acquire) ==
           writeIndex_.load(std::memory_order_acquire);
  }

private:
  [[nodiscard]] static constexpr std::size_t increment(const std::size_t index) noexcept {
    return (index + 1u) % Capacity;
  }

  alignas(64) std::array<Item, Capacity> items_{};
  alignas(64) std::atomic<std::size_t> writeIndex_{0};
  alignas(64) std::atomic<std::size_t> readIndex_{0};
};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

// Descriptor commands carry logical plug-in IDs. EngineHost resolves them to the
// current native instances at the next DSP block boundary.
enum class AudioCommandType : std::uint8_t { setParameters, setDescriptor, reset };

struct AudioCommand {
  static constexpr std::size_t kMaxPackedFloats = 512;
  static constexpr std::size_t kMaxParameterBytes = 8192;
  static constexpr std::size_t kMaxDescriptorBytes =
      kPipelineDescriptorHeaderBytes + kMaxPipelineNodes * kPipelineDescriptorNodeBytes;

  AudioCommandType type = AudioCommandType::reset;
  std::uint32_t logicalId = 0;
  std::uint32_t paramsHash = 0;
  std::uint32_t floatCount = 0;
  std::uint32_t parameterByteCount = 0;
  std::uint32_t descriptorByteCount = 0;
  std::array<float, kMaxPackedFloats> packed{};
  std::array<std::uint8_t, kMaxParameterBytes> parameterBytes{};
  std::array<std::uint8_t, kMaxDescriptorBytes> descriptor{};
};

using AudioCommandQueue = SpscQueue<AudioCommand, 64>;

class LatestParameterMailbox {
public:
  [[nodiscard]] bool publish(const AudioCommand &command) noexcept {
    if (command.type != AudioCommandType::setParameters || command.logicalId == 0) {
      return false;
    }
    Entry *entry = nullptr;
    for (auto &candidate : entries_) {
      auto id = candidate.logicalId.load(std::memory_order_acquire);
      if (id == command.logicalId) {
        entry = &candidate;
        break;
      }
      if (id == 0 && candidate.logicalId.compare_exchange_strong(
                         id, command.logicalId, std::memory_order_acq_rel)) {
        entry = &candidate;
        break;
      }
    }
    if (entry == nullptr) {
      for (auto &candidate : entries_) {
        auto id = candidate.logicalId.load(std::memory_order_acquire);
        const auto published = candidate.published.load(std::memory_order_acquire);
        if (candidate.reading.load(std::memory_order_acquire) == kNotReading &&
            candidate.consumed.load(std::memory_order_acquire) == published &&
            candidate.logicalId.compare_exchange_strong(
                id, command.logicalId, std::memory_order_acq_rel)) {
          entry = &candidate;
          break;
        }
      }
    }
    if (entry == nullptr) {
      return false;
    }
    const auto published = entry->published.load(std::memory_order_acquire);
    const auto publishedIndex = static_cast<std::uint8_t>(published & 3u);
    const auto readingIndex = entry->reading.load(std::memory_order_acquire);
    std::uint8_t writeIndex = 0;
    while (writeIndex == publishedIndex || writeIndex == readingIndex) {
      ++writeIndex;
    }
    entry->commands[writeIndex] = command;
    const auto generation = ++generation_;
    entry->published.store((generation << 2u) | writeIndex, std::memory_order_release);
    return true;
  }

  template <typename Consumer> void consumePending(Consumer &&consumer) noexcept {
    for (auto &entry : entries_) {
      if (entry.logicalId.load(std::memory_order_acquire) == 0) {
        continue;
      }
      auto token = entry.published.load(std::memory_order_acquire);
      if (token == 0 || token == entry.consumed.load(std::memory_order_acquire)) {
        continue;
      }
      constexpr std::uint32_t kMaxAttempts = 2;
      for (std::uint32_t attempt = 0; attempt < kMaxAttempts; ++attempt) {
        const auto index = static_cast<std::uint8_t>(token & 3u);
        entry.reading.store(index, std::memory_order_release);
        const auto confirmed = entry.published.load(std::memory_order_acquire);
        if (confirmed == token) {
          consumer(entry.commands[index]);
          entry.consumed.store(token, std::memory_order_release);
          break;
        }
        token = confirmed;
      }
      entry.reading.store(kNotReading, std::memory_order_release);
    }
  }

  void discardPending() noexcept {
    for (auto &entry : entries_) {
      const auto published = entry.published.load(std::memory_order_acquire);
      entry.consumed.store(published, std::memory_order_release);
    }
  }

private:
  static constexpr std::uint8_t kNotReading = 3;
  struct Entry {
    std::atomic<std::uint32_t> logicalId{0};
    std::array<AudioCommand, 3> commands{};
    std::atomic<std::uint64_t> published{0};
    std::atomic<std::uint8_t> reading{kNotReading};
    std::atomic<std::uint64_t> consumed{0};
  };
  std::array<Entry, kMaxPluginInstances> entries_{};
  std::uint64_t generation_ = 0;
};

} // namespace effetune::vst
