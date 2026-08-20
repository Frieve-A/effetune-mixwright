#include "engine/engine_host.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

#include "../support/crt_dialog_suppression.h"

using namespace effetune::vst;

namespace {

void expect(const bool condition, const std::string &message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

RuntimePlugin makeLimiter(const std::uint32_t logicalId, const float lookahead,
                          const std::uint32_t paramsHash) {
  RuntimePlugin runtime;
  runtime.logicalId = logicalId;
  runtime.type = "BrickwallLimiterPlugin";
  runtime.packedParameters = {0.0f, 100.0f, lookahead, 1.0f, 0.0f, -1.0f};
  runtime.paramsHash = paramsHash;
  return runtime;
}

RuntimePlugin makeGain(const std::uint32_t logicalId, const std::uint32_t paramsHash) {
  RuntimePlugin runtime;
  runtime.logicalId = logicalId;
  runtime.type = "TestGainPlugin";
  runtime.packedParameters = {1.0f};
  runtime.paramsHash = paramsHash;
  return runtime;
}

PluginState makeNode(const std::uint32_t logicalId, std::string type,
                     const std::string &channel, const std::uint8_t inputBus = 0,
                     const std::uint8_t outputBus = 0) {
  PluginState plugin;
  plugin.id = logicalId;
  plugin.name = std::move(type);
  plugin.inputBus = inputBus;
  plugin.outputBus = outputBus;
  plugin.channel = channel;
  return plugin;
}

void testSingleTransactionAndBoundaryStaging() {
  EngineHost engine;
  std::string error;
  expect(engine.prepare(48000.0, 2, EngineHost::kDefaultTelemetryBytes, &error),
         "batch engine prepare: " + error);
  const auto kernel = engine.kernels().find("TestGainPlugin");
  expect(kernel != engine.kernels().end(), "batch test kernel registration");
  PipelineState pipeline;
  pipeline.plugins = {PluginState{1, "TestGainPlugin", true}};
  RuntimePlugin runtime;
  runtime.logicalId = 1;
  runtime.type = "TestGainPlugin";
  runtime.packedParameters = {1.0f};
  runtime.paramsHash = kernel->second.paramsHash;
  expect(engine.rebuild(pipeline, {runtime}, &error), "batch engine rebuild: " + error);

  std::array<float, 8> left{};
  std::array<float, 8> right{};
  left.fill(1.0f);
  right.fill(-1.0f);
  float *channels[] = {left.data(), right.data()};

  const auto before = engine.processCounters();
  EngineHost::ProcessBatch batch;
  expect(engine.beginProcessBatch(batch), "begin a host-block transaction");
  EngineHost::ResolvedParameterTarget target;
  expect(batch.resolveParameterTarget(1, runtime.paramsHash, target),
         "resolve parameter target once at transaction start");
  constexpr std::array firstImage{0.25f};
  expect(batch.stageParameters(target, firstImage),
         "stage first full image");
  expect(batch.processChunk(channels, 2, 4, 0.0, false),
         "process first automation interval");
  constexpr std::array secondImage{0.75f};
  expect(batch.stageParameters(target, secondImage),
         "stage second full image");
  float *secondChannels[] = {left.data() + 4, right.data() + 4};
  expect(batch.processChunk(secondChannels, 2, 4, 4.0 / 48000.0, false),
         "process second automation interval");
  expect(batch.finish(), "finish a host-block transaction");

  for (std::uint32_t frame = 0; frame < 4; ++frame) {
    expect(std::abs(left[frame] - 0.25f) < 1.0e-7f &&
               std::abs(right[frame] + 0.25f) < 1.0e-7f,
           "first boundary image applies only to its interval");
  }
  for (std::uint32_t frame = 4; frame < 8; ++frame) {
    expect(std::abs(left[frame] - 0.75f) < 1.0e-7f &&
               std::abs(right[frame] + 0.75f) < 1.0e-7f,
           "second boundary image applies without crossing intervals");
  }
  const auto after = engine.processCounters();
  expect(after.batchAttempts == before.batchAttempts + 1 &&
             after.completedBatches == before.completedBatches + 1 &&
             after.telemetryDrains == before.telemetryDrains + 1 &&
             after.latencyRefreshes == before.latencyRefreshes,
         "dense slices perform one transaction and one housekeeping drain");
}

void testFailureCounters() {
  EngineHost engine;
  std::string error;
  expect(engine.prepare(48000.0, 1, EngineHost::kDefaultTelemetryBytes, &error),
         "failure engine prepare: " + error);
  const auto kernel = engine.kernels().find("TestGainPlugin");
  expect(kernel != engine.kernels().end(), "failure test kernel registration");
  PipelineState pipeline;
  pipeline.plugins = {PluginState{1, "TestGainPlugin", true}};
  RuntimePlugin runtime;
  runtime.logicalId = 1;
  runtime.type = "TestGainPlugin";
  runtime.packedParameters = {1.0f};
  runtime.paramsHash = kernel->second.paramsHash;
  expect(engine.rebuild(pipeline, {runtime}, &error), "failure engine rebuild: " + error);

  EngineHost::ProcessBatch owner;
  expect(engine.beginProcessBatch(owner), "owner transaction begins without a lock");
  expect(!engine.beginProcessBatch(owner), "an active batch is never begun twice");
  expect(owner.finish(), "owner finish");

  EngineHost::ProcessBatch failed;
  expect(engine.beginProcessBatch(failed), "failure transaction begin");
  constexpr std::array image{0.5f};
  expect(!failed.stageParameters(1, image, runtime.paramsHash + 1u),
         "invalid image fails the whole transaction");
  expect(!failed.finish(), "failed transaction reports block-wide failure");
  const auto counters = engine.processCounters();
  expect(counters.batchAttempts == 2 && counters.parameterStageFailures == 1 &&
             counters.completedBatches == 2 && counters.telemetryDrains == 2,
         "staging failures have bounded deferred counters");
  expect(engine.lastProcessError() ==
             EngineHost::ProcessError::parameterTargetUnavailable,
         "latest failure is available to a deferred non-RT diagnostic");
  const auto failure = engine.processFailureDiagnostic();
  expect(failure.sequence == 1 &&
             failure.error == EngineHost::ProcessError::parameterTargetUnavailable,
         "failure sequence retains the latest undrained reason");
  EngineHost::ProcessBatch recovered;
  expect(engine.beginProcessBatch(recovered) && recovered.finish(),
         "a later batch succeeds after the diagnostic failure");
  const auto sticky = engine.processFailureDiagnostic();
  expect(sticky.sequence == failure.sequence && sticky.error == failure.error,
         "success does not erase an undrained failure diagnostic");
}

void testChannelAwarePipelineLatencyRouting() {
  EngineHost engine;
  std::string error;
  expect(engine.prepare(48000.0, 2, EngineHost::kDefaultTelemetryBytes, &error),
         "latency routing engine prepare: " + error);
  const auto limiter = engine.kernels().find("BrickwallLimiterPlugin");
  const auto gain = engine.kernels().find("TestGainPlugin");
  expect(limiter != engine.kernels().end() && gain != engine.kernels().end(),
         "latency routing kernel registration");

  PipelineState parallel;
  parallel.plugins = {makeNode(101, "BrickwallLimiterPlugin", "L"),
                      makeNode(102, "BrickwallLimiterPlugin", "R")};
  const auto left = makeLimiter(101, 3.0f, limiter->second.paramsHash);
  const auto right = makeLimiter(102, 10.0f, limiter->second.paramsHash);
  expect(engine.rebuild(parallel, {left, right}, &error),
         "parallel L/R latency rebuild: " + error);
  expect(engine.pipelineLatency() == 480u,
         "parallel L/R lookahead paths publish their maximum latency");

  auto leftOnly = parallel;
  leftOnly.plugins[1].enabled = false;
  const std::array parallelRuntimes{left, right};
  AudioCommand descriptorCommand;
  expect(engine.makeDescriptorCommand(leftOnly, parallelRuntimes,
                                      descriptorCommand, &error),
         "build a target-removing descriptor command: " + error);
  const auto descriptorPlanRevision = engine.pipelinePlanRevision();
  std::uint64_t appliedDescriptorRevision = 0;
  expect(engine.applyDescriptorCommand(descriptorCommand,
                                       appliedDescriptorRevision, &error) &&
             engine.pipelinePlanRevision() > descriptorPlanRevision &&
             appliedDescriptorRevision == engine.pipelinePlanRevision() &&
             engine.pipelineLatency() == 144u,
         "non-RT descriptor service publishes the rebuilt compensation-plan revision");

  constexpr std::array updatedLeft{0.0f, 100.0f, 12.0f, 1.0f, 0.0f, -1.0f};
  const auto previousPlanRevision = engine.pipelinePlanRevision();
  expect(engine.updateParameters(101, updatedLeft, limiter->second.paramsHash),
         "live latency parameter update");
  expect(engine.pipelineLatency() == 576u &&
             engine.pipelinePlanRevision() > previousPlanRevision,
         "latency refresh reads the live instance latency");
  std::uint64_t refreshedPlanRevision = 0;
  expect(engine.refreshPipelinePlan(refreshedPlanRevision, &error) &&
             refreshedPlanRevision == engine.pipelinePlanRevision(),
         "non-RT refresh rebuilds the active compensation plan");

  PipelineState serial;
  serial.plugins = {makeNode(201, "BrickwallLimiterPlugin", "L"),
                    makeNode(202, "BrickwallLimiterPlugin", "L")};
  expect(engine.rebuild(serial,
                        {makeLimiter(201, 3.0f, limiter->second.paramsHash),
                         makeLimiter(202, 10.0f, limiter->second.paramsHash)},
                        &error),
         "serial channel latency rebuild: " + error);
  expect(engine.pipelineLatency() == 624u,
         "serial nodes on one channel sum their latency");

  PipelineState routed;
  routed.plugins = {
      makeNode(301, "BrickwallLimiterPlugin", "L", 0, 1),
      makeNode(302, "BrickwallLimiterPlugin", "R", 0, 1),
      makeNode(303, "TestGainPlugin", "L", 1, 0),
      makeNode(304, "TestGainPlugin", "R", 1, 0),
      makeNode(305, "BrickwallLimiterPlugin", "L"),
  };
  expect(engine.rebuild(routed,
                        {makeLimiter(301, 3.0f, limiter->second.paramsHash),
                         makeLimiter(302, 10.0f, limiter->second.paramsHash),
                         makeGain(303, gain->second.paramsHash),
                         makeGain(304, gain->second.paramsHash),
                         makeLimiter(305, 10.0f, limiter->second.paramsHash)},
                        &error),
         "bus merge latency rebuild: " + error);
  expect(engine.pipelineLatency() == 624u,
         "bus routing and merge preserve independent channel path latency");

  expect(engine.prepare(48000.0, 4, EngineHost::kDefaultTelemetryBytes, &error),
         "paired latency engine prepare: " + error);
  PipelineState paired;
  paired.plugins = {makeNode(401, "BrickwallLimiterPlugin", "34"),
                    makeNode(402, "BrickwallLimiterPlugin", "3")};
  expect(engine.rebuild(paired,
                        {makeLimiter(401, 3.0f, limiter->second.paramsHash),
                         makeLimiter(402, 10.0f, limiter->second.paramsHash)},
                        &error),
         "paired and numbered channel latency rebuild: " + error);
  expect(engine.pipelineLatency() == 624u,
         "paired and numbered channel specs share only their selected path");
}

} // namespace

int main() {
  effetune::vst::testing::suppressCrtModalDialogs();
  try {
    testSingleTransactionAndBoundaryStaging();
    testFailureCounters();
    testChannelAwarePipelineLatencyRouting();
    std::cout << "EffeTune engine batch tests passed\n";
    return 0;
  } catch (const std::exception &exception) {
    std::cerr << "Test failure: " << exception.what() << '\n';
    return 1;
  }
}
