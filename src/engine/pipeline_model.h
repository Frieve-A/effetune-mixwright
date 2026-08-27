#pragma once

#include <array>
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
inline constexpr std::size_t kMaxExecutionSampleRates = 16;

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

enum class ExecutionChannelMode : std::uint8_t {
  all = 1u << 0u,
  single = 1u << 1u,
  mono = 1u << 2u,
  stereoPair = 1u << 3u
};

enum class ExecutionContextSupport {
  supported,
  unsupportedSampleRate,
  unsupportedChannelMode
};

// Transient metadata copied from the upstream executionCapabilities contract.
// It is deliberately absent from PluginStateDocument: admission depends on the
// current engine context and must never become serialized project state.
struct RuntimeExecutionCapabilities {
  std::array<double, kMaxExecutionSampleRates> supportedSampleRates{};
  std::uint8_t supportedSampleRateCount = 0;
  std::uint8_t supportedChannelModes = 0;
  bool constrainsSampleRate = false;
  bool constrainsChannelMode = false;

  [[nodiscard]] bool operator==(
      const RuntimeExecutionCapabilities &) const noexcept = default;
};

struct PipelineState {
  std::vector<PluginState> plugins;
};

enum class AutomationBindingLifecycle { active, dormant, tombstone };

struct AutomationBindingState {
  std::uint32_t slot = 0;
  char pipeline = 'A';
  std::uint32_t pluginId = 0;
  std::string pluginType;
  std::string parameterKey;
  std::uint32_t elementIndex = 0;
  AutomationBindingLifecycle lifecycle = AutomationBindingLifecycle::dormant;
  std::string extraJson = "{}";
};

struct AutomationState {
  bool initialized = false;
  std::uint32_t logicalPluginIdWatermark = 0;
  std::vector<AutomationBindingState> bindings;
  std::string extraJson = "{}";
};

struct PluginStateDocument {
  std::uint32_t formatVersion = 1;
  std::string appVersion = "0.6.0";
  PipelineState pipelineA;
  PipelineState pipelineB;
  bool pipelineBInitialized = false;
  char currentPipeline = 'A';
  bool masterBypass = false;
  OversamplingSettings oversampling;
  UiSettings ui;
  AutomationState automation;
  std::string extraJson = "{}";
};

using InstanceResolver = std::function<std::uint32_t(std::uint32_t logicalId)>;

[[nodiscard]] bool isSectionPlugin(const PluginState &plugin) noexcept;
[[nodiscard]] std::int8_t encodeChannelSpec(const std::optional<std::string> &channel);
[[nodiscard]] ExecutionContextSupport executionContextSupport(
    const RuntimeExecutionCapabilities &capabilities,
    const std::optional<std::string> &channel, double sampleRate,
    std::uint32_t outputChannelCount) noexcept;
[[nodiscard]] bool supportsExecutionContext(
    const RuntimeExecutionCapabilities &capabilities,
    const std::optional<std::string> &channel, double sampleRate,
    std::uint32_t outputChannelCount) noexcept;
[[nodiscard]] std::vector<std::uint8_t>
encodePipelineDescriptor(const PipelineState &pipeline, const InstanceResolver &resolveInstance,
                         bool omitInactive = true);

} // namespace effetune::vst
