#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

namespace effetune::vst {

struct OutputAnalyzerSource {
  std::uint32_t tapId = 0;
  std::string type;
  std::string channel;
  std::vector<float> parameters;
  std::uint32_t paramsHash = 0;
  float gain = 1.0f;
  bool operator==(const OutputAnalyzerSource &) const = default;
};

// Independent analysis of the final output; never consumes pipeline capacity.
class OutputAnalyzers {
public:
  static constexpr std::uint32_t kMaxSources = 66;
  static constexpr std::uint32_t kTelemetryBytes = 256u * 1024u;
  OutputAnalyzers();
  // The processor must have stopped callbacks before destroying this object.
  ~OutputAnalyzers();
  [[nodiscard]] static bool validSource(const OutputAnalyzerSource &source) noexcept;
  [[nodiscard]] bool setSources(std::vector<OutputAnalyzerSource> sources,
                                std::string *error = nullptr);
  [[nodiscard]] bool prepare(double sampleRate, std::uint32_t channels,
                             std::uint32_t maxFrames, std::string *error = nullptr);
  void process(float *const *output, std::uint32_t channels,
               std::uint32_t frames, double sampleRate) noexcept;
  [[nodiscard]] std::uint32_t readTelemetry(std::span<std::uint8_t> output,
                                           std::uint32_t &dropped) noexcept;
  void discardTelemetry() noexcept;
  void stopCapture() noexcept { enabled_.store(false, std::memory_order_release); }

private:
  struct Storage;
  [[nodiscard]] bool configure(std::string *error);
  std::mutex controlMutex_;
  std::vector<OutputAnalyzerSource> sources_;
  double sampleRate_ = 0;
  std::uint32_t channels_ = 0;
  // The callback only copies PCM to the fixed SPSC storage. Every analyzer
  // engine operation, allocation and destruction belongs to the control side.
  std::atomic_bool enabled_{false};
  std::unique_ptr<Storage> storage_;
};

} // namespace effetune::vst
