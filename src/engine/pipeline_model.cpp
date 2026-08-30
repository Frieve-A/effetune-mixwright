#include "engine/pipeline_model.h"

#include <algorithm>
#include <array>
#include <span>
#include <stdexcept>
#include <string_view>
#include <unordered_set>

namespace effetune::vst {
namespace {

void appendUint32(std::vector<std::uint8_t> &bytes, const std::uint32_t value) {
  bytes.push_back(static_cast<std::uint8_t>(value & 0xffu));
  bytes.push_back(static_cast<std::uint8_t>((value >> 8u) & 0xffu));
  bytes.push_back(static_cast<std::uint8_t>((value >> 16u) & 0xffu));
  bytes.push_back(static_cast<std::uint8_t>((value >> 24u) & 0xffu));
}

struct EncodedNode {
  std::uint32_t instanceId = 0;
  bool enabled = false;
  std::uint8_t inputBus = 0;
  std::uint8_t outputBus = 0;
  std::int8_t channelSpec = -1;
  bool sectionGate = true;
};

} // namespace

bool isSectionPlugin(const PluginState &plugin) noexcept {
  return plugin.name == "Section" || plugin.name == "SectionPlugin";
}

ExecutionContextSupport executionContextSupport(
    const RuntimeExecutionCapabilities &capabilities,
    const std::optional<std::string> &channel, const double sampleRate,
    const std::uint32_t outputChannelCount) noexcept {
  if (capabilities.constrainsSampleRate) {
    if (capabilities.supportedSampleRateCount >
        capabilities.supportedSampleRates.size()) {
      return ExecutionContextSupport::unsupportedSampleRate;
    }
    const auto rates = std::span(capabilities.supportedSampleRates)
                           .first(capabilities.supportedSampleRateCount);
    if (std::find(rates.begin(), rates.end(), sampleRate) == rates.end()) {
      return ExecutionContextSupport::unsupportedSampleRate;
    }
  }
  if (!capabilities.constrainsChannelMode) {
    return ExecutionContextSupport::supported;
  }

  auto mode = ExecutionChannelMode::stereoPair;
  std::uint32_t firstChannel = 0;
  std::uint32_t requiredChannels = 2;
  const std::string_view selection = channel.has_value()
                                         ? std::string_view(channel->data(), channel->size())
                                         : std::string_view{};
  if (selection == "A") {
    mode = ExecutionChannelMode::all;
    requiredChannels = outputChannelCount;
  } else if (selection == "L" || selection == "1") {
    mode = ExecutionChannelMode::single;
    requiredChannels = 1;
  } else if (selection == "R" || selection == "2") {
    mode = ExecutionChannelMode::single;
    firstChannel = 1;
    requiredChannels = 1;
  } else if (selection.empty()) {
    mode = outputChannelCount == 1 ? ExecutionChannelMode::mono
                                   : ExecutionChannelMode::stereoPair;
    requiredChannels = outputChannelCount == 1 ? 1u : 2u;
  } else if (selection == "34" || selection == "56" || selection == "78") {
    mode = ExecutionChannelMode::stereoPair;
    firstChannel = selection == "34" ? 2u : selection == "56" ? 4u : 6u;
    requiredChannels = 2;
  } else if (selection.size() == 1 && selection.front() >= '1' &&
             selection.front() <= '8') {
    mode = ExecutionChannelMode::single;
    firstChannel = static_cast<std::uint32_t>(selection.front() - '1');
    requiredChannels = 1;
  } else {
    return ExecutionContextSupport::unsupportedChannelMode;
  }

  const auto modeBit = static_cast<std::uint8_t>(mode);
  const auto remainingChannels = outputChannelCount > firstChannel
                                     ? outputChannelCount - firstChannel
                                     : 0u;
  const auto availableChannels = std::min(remainingChannels, requiredChannels);
  return (capabilities.supportedChannelModes & modeBit) != 0u &&
                 availableChannels == requiredChannels
             ? ExecutionContextSupport::supported
             : ExecutionContextSupport::unsupportedChannelMode;
}

bool supportsExecutionContext(
    const RuntimeExecutionCapabilities &capabilities,
    const std::optional<std::string> &channel, const double sampleRate,
    const std::uint32_t outputChannelCount) noexcept {
  return executionContextSupport(capabilities, channel, sampleRate,
                                 outputChannelCount) ==
         ExecutionContextSupport::supported;
}

std::int8_t encodeChannelSpec(const std::optional<std::string> &channel) {
  if (!channel.has_value() || channel->empty()) {
    return -1;
  }
  if (*channel == "A") {
    return -2;
  }
  if (*channel == "L" || *channel == "1") {
    return 0;
  }
  if (*channel == "R" || *channel == "2") {
    return 1;
  }
  constexpr std::array<std::string_view, 7> pairSpecs{
      "34", "56", "78", "910", "1112", "1314", "1516"};
  const auto pair = std::find(pairSpecs.begin(), pairSpecs.end(), *channel);
  if (pair != pairSpecs.end()) {
    return static_cast<std::int8_t>(17 + std::distance(pairSpecs.begin(), pair));
  }
  constexpr std::array<std::string_view, 16> singleSpecs{
      "1", "2", "3", "4", "5", "6", "7", "8",
      "9", "10", "11", "12", "13", "14", "15", "16"};
  const auto single = std::find(singleSpecs.begin(), singleSpecs.end(), *channel);
  if (single != singleSpecs.end()) {
    return static_cast<std::int8_t>(std::distance(singleSpecs.begin(), single));
  }
  throw std::invalid_argument("Unsupported channel specifier: " + *channel);
}

std::vector<std::uint8_t>
encodePipelineDescriptor(const PipelineState &pipeline, const InstanceResolver &resolveInstance,
                         const bool omitInactive) {
  if (!resolveInstance) {
    throw std::invalid_argument("Instance resolver is required");
  }

  bool insideSection = false;
  bool sectionEnabled = true;
  std::vector<EncodedNode> nodes;
  nodes.reserve(pipeline.plugins.size());
  std::unordered_set<std::uint32_t> seenInstances;

  for (const auto &plugin : pipeline.plugins) {
    if (isSectionPlugin(plugin)) {
      insideSection = true;
      sectionEnabled = plugin.enabled;
      continue;
    }
    if (plugin.inputBus > kMaxBus || plugin.outputBus > kMaxBus) {
      throw std::invalid_argument("Pipeline bus index is outside 0..4");
    }

    const bool sectionGate = !insideSection || sectionEnabled;
    if (omitInactive && (!plugin.enabled || !sectionGate)) {
      continue;
    }

    const auto instance = resolveInstance(plugin.id);
    if (instance == 0) {
      continue; // The DSP ready gate excludes instances without valid packed parameters.
    }
    if (!seenInstances.insert(instance).second) {
      throw std::invalid_argument("Duplicate DSP instance in pipeline descriptor");
    }
    nodes.push_back({instance, plugin.enabled, plugin.inputBus, plugin.outputBus,
                     encodeChannelSpec(plugin.channel), sectionGate});
  }

  if (nodes.size() > kMaxPipelineNodes) {
    throw std::invalid_argument("Pipeline exceeds 128 DSP nodes");
  }

  std::vector<std::uint8_t> bytes;
  bytes.reserve(kPipelineDescriptorHeaderBytes + nodes.size() * kPipelineDescriptorNodeBytes);
  appendUint32(bytes, kPipelineDescriptorVersion);
  appendUint32(bytes, static_cast<std::uint32_t>(nodes.size()));
  for (const auto &node : nodes) {
    appendUint32(bytes, node.instanceId);
    bytes.push_back(node.enabled ? 1u : 0u);
    bytes.push_back(node.inputBus);
    bytes.push_back(node.outputBus);
    bytes.push_back(static_cast<std::uint8_t>(node.channelSpec));
    bytes.push_back(node.sectionGate ? 1u : 0u);
    bytes.insert(bytes.end(), {0u, 0u, 0u});
  }
  return bytes;
}

} // namespace effetune::vst
