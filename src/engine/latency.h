#pragma once

#include "engine/pipeline_model.h"

#include <cstdint>
#include <functional>

namespace effetune::vst {

using InstanceLatencyResolver = std::function<std::uint32_t(std::uint32_t logicalId)>;

[[nodiscard]] std::uint32_t aggregatePipelineLatency(const PipelineState &pipeline,
                                                     const InstanceLatencyResolver &latency);
[[nodiscard]] std::uint32_t calculateTotalLatency(std::uint32_t resamplerLatency,
                                                  std::uint32_t oversamplingFactor,
                                                  std::uint32_t pipelineLatency);

} // namespace effetune::vst
