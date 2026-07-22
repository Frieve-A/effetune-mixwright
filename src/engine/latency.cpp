#include "engine/latency.h"

#include <algorithm>
#include <array>
#include <stdexcept>

namespace effetune::vst {

std::uint32_t aggregatePipelineLatency(const PipelineState &pipeline,
                                       const InstanceLatencyResolver &latency) {
  std::array<std::uint32_t, 5> busLatency{};
  bool insideSection = false;
  bool sectionEnabled = true;
  for (const auto &plugin : pipeline.plugins) {
    if (isSectionPlugin(plugin)) {
      insideSection = true;
      sectionEnabled = plugin.enabled;
      continue;
    }
    if (!plugin.enabled || (insideSection && !sectionEnabled) || plugin.inputBus > kMaxBus ||
        plugin.outputBus > kMaxBus) {
      continue;
    }
    const auto routedLatency = busLatency[plugin.inputBus] + (latency ? latency(plugin.id) : 0u);
    if (plugin.inputBus == plugin.outputBus || routedLatency > busLatency[plugin.outputBus]) {
      busLatency[plugin.outputBus] = routedLatency;
    }
  }
  return busLatency[0];
}

std::uint32_t calculateTotalLatency(const std::uint32_t resamplerLatency,
                                    const std::uint32_t engineQuantum,
                                    const std::uint32_t oversamplingFactor,
                                    const std::uint32_t pipelineLatency) {
  if (oversamplingFactor == 0) {
    throw std::invalid_argument("Oversampling factor cannot be zero");
  }
  const auto quantumAtHostRate =
      (engineQuantum + oversamplingFactor - 1u) / oversamplingFactor;
  const auto pipelineAtHostRate =
      (pipelineLatency + oversamplingFactor / 2u) / oversamplingFactor;
  return resamplerLatency + quantumAtHostRate + pipelineAtHostRate;
}

} // namespace effetune::vst
