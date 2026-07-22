#include "engine/pipeline_model.h"

#include <array>
#include <stdexcept>
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
  if (*channel == "34") {
    return 17;
  }
  if (*channel == "56") {
    return 18;
  }
  if (*channel == "78") {
    return 19;
  }
  if (channel->size() == 1 && channel->front() >= '1' && channel->front() <= '8') {
    return static_cast<std::int8_t>(channel->front() - '1');
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
