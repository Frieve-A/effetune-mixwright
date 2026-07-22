#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace effetune::vst {

inline constexpr std::uint32_t kPipelineDescriptorVersion = 1;
inline constexpr std::size_t kPipelineDescriptorHeaderBytes = 8;
inline constexpr std::size_t kPipelineDescriptorNodeBytes = 12;
inline constexpr std::size_t kMaxPipelineNodes = 128;
inline constexpr std::size_t kMaxPluginInstances = 96;
inline constexpr std::uint8_t kMaxBus = 4;

enum class OversamplingPhase { linear, minimum };
enum class FilterQuality { low, medium, high, ultra };

struct OversamplingSettings {
  std::uint32_t factor = 1;
  OversamplingPhase phase = OversamplingPhase::linear;
  FilterQuality quality = FilterQuality::medium;

  [[nodiscard]] bool isValid() const noexcept {
    return factor == 1 || factor == 2 || factor == 4 || factor == 8;
  }
};

struct UiSettings {
  std::uint32_t columns = 1;
  double zoom = 1.0;
};

struct PluginState {
  std::uint32_t id = 0;
  std::string name;
  bool enabled = true;
  std::uint8_t inputBus = 0;
  std::uint8_t outputBus = 0;
  std::optional<std::string> channel = std::nullopt;
  std::string parametersJson = "{}";
  bool unknown = false;
  std::string extraJson = "{}";
};

struct PipelineState {
  std::vector<PluginState> plugins;
};

struct PluginStateDocument {
  std::uint32_t formatVersion = 1;
  std::string appVersion = "0.1.1";
  PipelineState pipelineA;
  PipelineState pipelineB;
  bool pipelineBInitialized = false;
  char currentPipeline = 'A';
  bool masterBypass = false;
  OversamplingSettings oversampling;
  UiSettings ui;
  std::string extraJson = "{}";
};

using InstanceResolver = std::function<std::uint32_t(std::uint32_t logicalId)>;

[[nodiscard]] bool isSectionPlugin(const PluginState &plugin) noexcept;
[[nodiscard]] std::int8_t encodeChannelSpec(const std::optional<std::string> &channel);
[[nodiscard]] std::vector<std::uint8_t>
encodePipelineDescriptor(const PipelineState &pipeline, const InstanceResolver &resolveInstance,
                         bool omitInactive = true);

} // namespace effetune::vst
