#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#ifdef EFFETUNE_HAS_NATIVE_SPECTRUM_TAP
#include <effetune/spectrum_tap.h>
#endif

namespace effetune::vst {

enum class SpectrumMode : std::uint8_t { off, after, compare };
enum class SpectrumQuality : std::uint8_t { normal, hq };

struct SpectrumFrame {
  static constexpr std::uint32_t kSamples = 4096;
  std::uint32_t pluginId = 0;
  SpectrumMode mode = SpectrumMode::after;
  std::uint32_t bufferPosition = 0;
  double sampleRate = 0;
#ifdef EFFETUNE_HAS_NATIVE_SPECTRUM_TAP
  SpectrumTapFrame analysis;
#else
  std::array<float, kSamples> input{};
  std::array<float, kSamples> output{};
#endif
};

// Audio only averages/copies routed PCM into a fixed SPSC queue. Delay alignment,
// rolling display history and frame allocation belong to the polling thread.
class SpectrumCapture {
public:
  SpectrumCapture();
  ~SpectrumCapture();
  void prepare(std::uint32_t maxFrames, double sampleRate = 44100);
  [[nodiscard]] bool setTap(std::uint32_t pluginId, SpectrumMode mode,
                            SpectrumQuality quality = SpectrumQuality::normal);
  [[nodiscard]] bool nativeAnalysisSupported() const noexcept;
  void stopCapture() noexcept;
  void invalidate() noexcept;
  void beginBlock(bool masterBypass) noexcept;
  [[nodiscard]] bool capturing() const noexcept {
    return enabled_.load(std::memory_order_acquire);
  }
  void observe(std::uint32_t pluginId, const float *audio, std::uint32_t channels,
               std::uint32_t frames, std::uint32_t latency, double sampleRate,
               std::uint64_t firstFrame, bool before,
               std::uint32_t tapDelayFrames = 0) noexcept;
  [[nodiscard]] std::vector<SpectrumFrame> read(std::uint32_t &dropped);

private:
  struct Storage;
  std::unique_ptr<Storage> storage_;
  std::mutex controlMutex_;
  std::atomic_bool enabled_{false};
  std::atomic<std::uint64_t> epoch_{1};
};

} // namespace effetune::vst
