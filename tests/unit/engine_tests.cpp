#include "bridge/state_codec.h"
#include "bridge/message_router.h"
#include "bridge/config_store.h"
#include "engine/block_adapter.h"
#include "engine/command_queue.h"
#include "engine/dry_delay.h"
#include "engine/engine_host.h"
#include "engine/latency.h"
#include "engine/output_transition.h"
#include "engine/pipeline_model.h"
#include "engine/resampler.h"

#include <choc/text/choc_JSON.h>

#include <array>
#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "../support/crt_dialog_suppression.h"

namespace {

using namespace effetune::vst;

void expect(const bool condition, const std::string &message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

void testDescriptor() {
  PipelineState pipeline;
  pipeline.plugins = {
      PluginState{1, "Section", true},
      PluginState{2, "Volume", true, 0, 1, std::string("L")},
      PluginState{3, "Delay", true, 1, 0, std::string("34")},
  };
  const auto bytes = encodePipelineDescriptor(
      pipeline, [](const std::uint32_t id) { return id + 100u; }, false);
  expect(bytes.size() == 8u + 2u * 12u, "descriptor size");
  expect(bytes[0] == 1u && bytes[4] == 2u, "descriptor header");
  expect(bytes[8] == 102u && bytes[12] == 1u && bytes[13] == 0u && bytes[14] == 1u,
         "first descriptor node");
  expect(static_cast<std::int8_t>(bytes[15]) == 0 && bytes[16] == 1u,
         "first descriptor channel/gate");
  expect(bytes[20] == 103u && bytes[25] == 1u && bytes[26] == 0u,
         "second descriptor node");
  expect(static_cast<std::int8_t>(bytes[27]) == 17, "paired channel encoding");

  pipeline.plugins[0].enabled = false;
  const auto inactive = encodePipelineDescriptor(
      pipeline, [](const std::uint32_t id) { return id + 100u; }, true);
  expect(inactive.size() == 8u && inactive[4] == 0u, "disabled section omission");

  PipelineState maximum;
  maximum.plugins.reserve(kMaxPipelineNodes + 1u);
  for (std::uint32_t id = 1; id <= kMaxPipelineNodes; ++id) {
    maximum.plugins.push_back(PluginState{id, "Volume", true});
  }
  const auto maximumBytes = encodePipelineDescriptor(
      maximum, [](const std::uint32_t id) { return id; }, false);
  expect(maximumBytes.size() == kPipelineDescriptorHeaderBytes +
                                      kMaxPipelineNodes * kPipelineDescriptorNodeBytes,
         "128-node descriptor boundary");
  maximum.plugins.push_back(PluginState{129, "Volume", true});
  bool rejected = false;
  try {
    (void)encodePipelineDescriptor(
        maximum, [](const std::uint32_t id) { return id; }, false);
  } catch (const std::invalid_argument &) {
    rejected = true;
  }
  expect(rejected, "129-node descriptor rejection");
}

void testQueue() {
  SpscQueue<AudioCommand, 4> queue;
  AudioCommand input;
  input.logicalId = 42;
  input.type = AudioCommandType::setParameters;
  expect(queue.push(input), "queue push");
  AudioCommand output;
  expect(queue.pop(output), "queue pop");
  expect(output.logicalId == 42 && output.type == AudioCommandType::setParameters,
         "queue payload");
  expect(queue.empty(), "queue empty");

  auto mailbox = std::make_unique<LatestParameterMailbox>();
  for (std::uint32_t value = 1; value <= 1000; ++value) {
    input.logicalId = 42;
    input.floatCount = 1;
    input.packed[0] = static_cast<float>(value);
    expect(mailbox->publish(input), "mailbox latest-value publish");
  }
  std::uint32_t consumed = 0;
  mailbox->consumePending([&consumed](const AudioCommand &command) {
    consumed = static_cast<std::uint32_t>(command.packed[0]);
  });
  expect(consumed == 1000, "mailbox must deliver the final stopped-audio value");
  input.packed[0] = 1001.0f;
  expect(mailbox->publish(input), "mailbox pre-rebuild publish");
  mailbox->discardPending();
  mailbox->consumePending([&consumed](const AudioCommand &) { consumed = 0; });
  expect(consumed == 1000, "mailbox rebuild boundary discards stale parameters");
  input.packed[0] = 1002.0f;
  expect(mailbox->publish(input), "mailbox post-rebuild publish");
  mailbox->consumePending([&consumed](const AudioCommand &command) {
    consumed = static_cast<std::uint32_t>(command.packed[0]);
  });
  expect(consumed == 1002, "mailbox accepts fresh parameters after rebuild");
  for (std::uint32_t id = 1; id <= 2u * kMaxPluginInstances; ++id) {
    input.logicalId = 1000u + id;
    expect(mailbox->publish(input), "mailbox reclaimed logical id publish");
    mailbox->consumePending([](const AudioCommand &) {});
  }
}

void testOutputTransition() {
  constexpr std::uint32_t channels = 8;
  constexpr std::uint32_t frames = 4;
  std::array<std::array<float, frames>, channels> output{};
  std::array<std::array<float, frames>, channels> dry{};
  std::array<float *, channels> outputPointers{};
  std::array<const float *, channels> dryPointers{};
  for (std::uint32_t channel = 0; channel < channels; ++channel) {
    output[channel].fill(3.0f);
    dry[channel].fill(1.0f);
    outputPointers[channel] = output[channel].data();
    dryPointers[channel] = dry[channel].data();
  }

  OutputTransition transition;
  transition.apply(outputPointers.data(), dryPointers.data(), channels, frames, 1000.0, true);
  expect(output[0][0] == 3.0f && output[7][3] == 3.0f,
         "initial processed output is not faded");

  for (auto &channel : output) channel.fill(1.0f);
  transition.apply(outputPointers.data(), dryPointers.data(), channels, frames, 1000.0, false);
  expect(std::abs(output[0][0] - 2.8f) < 1.0e-6f &&
             std::abs(output[7][3] - 2.2f) < 1.0e-6f,
         "processed-to-dry transition starts continuously across 8 channels");
  for (auto &channel : output) channel.fill(1.0f);
  transition.apply(outputPointers.data(), dryPointers.data(), channels, frames, 1000.0, false);
  for (auto &channel : output) channel.fill(1.0f);
  transition.apply(outputPointers.data(), dryPointers.data(), channels, 2, 1000.0, false);
  expect(std::abs(output[0][1] - 1.0f) < 1.0e-6f,
         "processed-to-dry transition completes in 10 ms across blocks");

  for (auto &channel : output) channel.fill(3.0f);
  transition.apply(outputPointers.data(), dryPointers.data(), channels, frames, 1000.0, true);
  expect(std::abs(output[0][0] - 1.2f) < 1.0e-6f &&
             std::abs(output[7][3] - 1.8f) < 1.0e-6f,
         "in-place dry-to-processed transition uses the preserved dry scratch");

  for (auto &channel : output) channel.fill(1.0f);
  transition.apply(outputPointers.data(), dryPointers.data(), channels, 3, 1000.0, false);
  const auto beforeRapidSwitch = output[0][2];
  for (auto &channel : output) channel.fill(3.0f);
  transition.apply(outputPointers.data(), dryPointers.data(), channels, frames, 1000.0, true);
  expect(std::abs(output[0][0] - beforeRapidSwitch) < 0.2f,
         "rapid transition reversal remains continuous");
  transition.apply(outputPointers.data(), dryPointers.data(), channels, 0, 1000.0, true);
}

void testDryDelayLine() {
  static_assert(noexcept(std::declval<DryDelayLine &>().process(nullptr, 0, 0)));

  DryDelayLine delay;
  expect(delay.prepare(1, 2, 3), "dry delay prepare");
  std::array<float, 2> first{1.0f, 0.0f};
  std::array<float, 2> second{};
  const float *input[] = {first.data()};
  const auto *output = delay.process(input, 1, 2);
  expect(output != nullptr && output[0][0] == 0.0f && output[0][1] == 0.0f,
         "dry delay impulse prefix");
  input[0] = second.data();
  output = delay.process(input, 1, 2);
  expect(output != nullptr && output[0][0] == 0.0f && output[0][1] == 1.0f,
         "dry delay exact impulse position across blocks");

  expect(delay.prepare(1, 4, 4), "dry delay active warm prepare");
  std::array<float, 4> active{1.0f, 2.0f, 3.0f, 4.0f};
  std::array<float, 4> bypass{5.0f, 6.0f, 7.0f, 8.0f};
  input[0] = active.data();
  expect(delay.process(input, 1, 4) != nullptr, "dry delay active-path warm");
  input[0] = bypass.data();
  output = delay.process(input, 1, 4);
  expect(output != nullptr && std::equal(output[0], output[0] + 4, active.begin()),
         "dry delay active-to-bypass continuity");

  expect(delay.prepare(1, 4, 2), "dry delay grow prepare");
  input[0] = active.data();
  expect(delay.process(input, 1, 4) != nullptr, "dry delay pre-grow history");
  expect(delay.process(input, 1, 5) == nullptr,
         "dry delay process does not grow its prepared block size");
  expect(delay.setDelay(4), "dry delay external capacity grow");
  input[0] = bypass.data();
  output = delay.process(input, 1, 4);
  constexpr std::array expectedAfterGrow{0.0f, 2.0f, 3.0f, 4.0f};
  expect(output != nullptr &&
             std::equal(output[0], output[0] + 4, expectedAfterGrow.begin()),
         "dry delay grow preserves the latest available history");
}

void testConfigStore() {
  const auto path = std::filesystem::temp_directory_path() /
                    ("effetune-vst-config-test-" +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                     ".json");
  ConfigStore store(path);
  std::string content;
  std::string error;
  expect(store.load(content, &error) && content == "{}", "missing config default");
  constexpr std::string_view saved = R"({"language":"ja","columns":3})";
  expect(store.save(saved, &error), "config atomic save: " + error);
  expect(store.load(content, &error) && content == saved, "config file round trip");
  std::error_code ignored;
  std::filesystem::remove(path, ignored);
}

void testBlockAdapter() {
  BlockAdapter adapter;
  adapter.prepare(1, 4);
  std::array<float, 4> input{1.0f, 2.0f, 3.0f, 4.0f};
  std::array<float, 4> output{};
  const float *inputs[] = {input.data()};
  float *outputs[] = {output.data()};
  std::uint32_t receivedFrames = 0;
  const auto ok = adapter.process(
      inputs, outputs, 4,
      [&receivedFrames](float *const *channels, const std::uint32_t channelCount,
                        const std::uint32_t frames) {
        receivedFrames = frames;
        for (std::uint32_t channel = 0; channel < channelCount; ++channel) {
          for (std::uint32_t frame = 0; frame < frames; ++frame) {
            channels[channel][frame] *= 2.0f;
          }
        }
        return true;
      });
  expect(ok && receivedFrames == 4, "sub-128 host block is processed at its original size");
  expect(output == std::array<float, 4>{2.0f, 4.0f, 6.0f, 8.0f},
         "sub-128 host block returns without adapter delay");
  expect(adapter.latencyFrames() == 0, "direct block adapter has no latency");

  BlockAdapter failed;
  failed.prepare(1, BlockAdapter::kMaxChunkFrames);
  std::array<float, BlockAdapter::kMaxChunkFrames> failedInput{};
  std::array<float, BlockAdapter::kMaxChunkFrames> failedOutput{};
  const float *failedInputs[] = {failedInput.data()};
  float *failedOutputs[] = {failedOutput.data()};
  expect(!failed.process(failedInputs, failedOutputs, BlockAdapter::kMaxChunkFrames,
                         [](float *const *, std::uint32_t, std::uint32_t) { return false; }),
         "block adapter propagates a failed DSP chunk");
}

void testBlockSizeMatrix() {
  constexpr std::array blockSizes{1u, 4u, 32u, 64u, 127u, 128u, 255u, 512u, 4096u};
  for (const auto blockSize : blockSizes) {
    BlockAdapter adapter;
    adapter.prepare(1, blockSize);
    std::vector<float> input(blockSize);
    std::vector<float> output(blockSize);
    for (std::uint32_t frame = 0; frame < blockSize; ++frame) {
      input[frame] = static_cast<float>(frame + 1u);
    }
    const float *inputs[] = {input.data()};
    float *outputs[] = {output.data()};
    std::vector<std::uint32_t> chunks;
    expect(adapter.process(
               inputs, outputs, blockSize,
               [&chunks](float *const *channels, const std::uint32_t channelCount,
                         const std::uint32_t frames) {
                 chunks.push_back(frames);
                 for (std::uint32_t channel = 0; channel < channelCount; ++channel) {
                   for (std::uint32_t frame = 0; frame < frames; ++frame) {
                     channels[channel][frame] *= 2.0f;
                   }
                 }
                 return true;
               }),
           "block-size matrix processing");
    for (std::uint32_t frame = 0; frame < blockSize; ++frame) {
      expect(output[frame] == static_cast<float>(frame + 1u) * 2.0f,
             "block-size matrix immediate output for " + std::to_string(blockSize));
    }
    expect(chunks.size() ==
               (blockSize + BlockAdapter::kMaxChunkFrames - 1u) /
                   BlockAdapter::kMaxChunkFrames,
           "block-size matrix chunk count for " + std::to_string(blockSize));
    for (std::size_t chunk = 0; chunk < chunks.size(); ++chunk) {
      const auto offset = static_cast<std::uint32_t>(chunk) * BlockAdapter::kMaxChunkFrames;
      expect(chunks[chunk] ==
                 std::min(BlockAdapter::kMaxChunkFrames, blockSize - offset),
             "block-size matrix chunk length for " + std::to_string(blockSize));
    }
  }
}

void testLatency() {
  PipelineState pipeline;
  pipeline.plugins = {
      PluginState{1, "A", true, 0, 1}, PluginState{2, "B", true, 1, 0},
      PluginState{3, "C", true, 0, 0},
  };
  const auto latency = aggregatePipelineLatency(
      pipeline, [](const std::uint32_t id) { return id == 1 ? 10u : (id == 2 ? 20u : 5u); });
  expect(latency == 35u, "bus latency aggregation");
  expect(calculateTotalLatency(63, 2, 35) == 81u, "host latency conversion");
}

void testStateCodec() {
  PluginStateDocument fresh;
  const auto freshJson = StateCodec::encode(fresh);
  expect(choc::json::parse(freshJson)["pipelineB"].isVoid(),
         "fresh state keeps pipeline B uninitialized as null");
  PluginStateDocument freshDecoded;
  std::string error;
  expect(StateCodec::decode(freshJson, freshDecoded, &error) &&
             !freshDecoded.pipelineBInitialized,
         "fresh pipeline B null round trip");

  PluginStateDocument intentionalEmptyB;
  intentionalEmptyB.pipelineBInitialized = true;
  const auto intentionalEmptyJson = StateCodec::encode(intentionalEmptyB);
  expect(choc::json::parse(intentionalEmptyJson)["pipelineB"].isArray(),
         "intentional empty pipeline B remains an array");
  expect(StateCodec::decode(intentionalEmptyJson, freshDecoded, &error) &&
             freshDecoded.pipelineBInitialized && freshDecoded.pipelineB.plugins.empty(),
         "intentional empty pipeline B round trip");

  PluginStateDocument original;
  original.appVersion = "test";
  original.currentPipeline = 'B';
  original.masterBypass = true;
  original.oversampling = {4, OversamplingPhase::minimum, FilterQuality::high};
  original.ui = {2, 1.25};
  original.pipelineA.plugins = {
      PluginState{7, "Volume", true, 1, 2, std::string("L"), R"({"vl":-6})"},
  };
  original.pipelineB.plugins = {
      PluginState{8, "FuturePlugin", false, 0, 0, std::nullopt, R"({"x":1})", true,
                  R"({"futurePluginField":{"v":2}})"},
  };
  original.pipelineBInitialized = true;
  original.extraJson = R"({"futureTopLevel":{"enabled":true}})";

  const auto json = StateCodec::encode(original);
  PluginStateDocument decoded;
  expect(StateCodec::decode(json, decoded, &error), "state round trip: " + error);
  expect(decoded.currentPipeline == 'B' && decoded.masterBypass, "state globals");
  expect(decoded.oversampling.factor == 4 &&
             decoded.oversampling.phase == OversamplingPhase::minimum &&
             decoded.oversampling.quality == FilterQuality::high,
         "state oversampling");
  expect(decoded.pipelineA.plugins.size() == 1 && decoded.pipelineA.plugins[0].inputBus == 1 &&
             decoded.pipelineA.plugins[0].channel == std::optional<std::string>("L"),
         "long-form pipeline");
  expect(decoded.pipelineB.plugins[0].unknown, "unknown plugin preservation");
  const auto reencoded = StateCodec::encode(decoded);
  expect(reencoded.find("futurePluginField") != std::string::npos &&
             reencoded.find("futureTopLevel") != std::string::npos,
         "unknown top-level and plugin fields preservation");

  constexpr auto transientCapabilities = R"({"plugins":[{"name":"Volume","parameters":{"gain":0.5},"executionCapabilities":{"requiresWasm":true},"futurePluginField":{"v":2}}]})";
  expect(StateCodec::decode(transientCapabilities, decoded, &error),
         "state with transient execution capabilities");
  const auto transientReencoded = choc::json::parse(StateCodec::encode(decoded));
  const auto transientPlugin = transientReencoded["pipelineA"][0];
  expect(transientPlugin["executionCapabilities"].isVoid() &&
             transientPlugin["futurePluginField"]["v"].getWithDefault<std::int64_t>(0) == 2 &&
             transientPlugin["parameters"]["gain"].getWithDefault<double>(0.0) == 0.5,
         "state drops transient execution capabilities while preserving unknown fields");

  constexpr auto shortForm = R"({"plugins":[{"nm":"Volume","en":true,"vl":-3,"ch":"Left"}]})";
  expect(StateCodec::decode(shortForm, decoded, &error), "short-form state");
  expect(decoded.pipelineA.plugins[0].name == "Volume" &&
             decoded.pipelineA.plugins[0].channel == std::optional<std::string>("L") &&
             decoded.pipelineA.plugins[0].parametersJson.find("vl") != std::string::npos,
         "short-form normalization");

  constexpr auto legacyArray = R"([{"name":"Delay","enabled":false,"parameters":{"dl":42}}])";
  expect(StateCodec::decode(legacyArray, decoded, &error), "legacy array state");
  expect(decoded.pipelineA.plugins.size() == 1 &&
             decoded.pipelineA.plugins[0].name == "Delay" &&
             !decoded.pipelineA.plugins[0].enabled,
         "legacy array normalization");

  constexpr auto legacyPipelines =
      R"({"pipelineA":[{"name":"Legacy A"}],"pipelineB":[{"name":"Legacy B"}]})";
  expect(StateCodec::decode(legacyPipelines, decoded, &error) &&
             decoded.pipelineA.plugins[0].id == 1u &&
             decoded.pipelineB.plugins[0].id == 2u,
         "legacy A/B entries receive globally unique generated IDs");

  const auto stateBeforeInvalidPipeline = StateCodec::encode(decoded);
  constexpr auto duplicateIds =
      R"({"pipelineA":[{"id":7,"name":"Known","future":{"v":1}},{"id":7,"name":"Future","unknown":true,"future":{"v":2}}]})";
  expect(!StateCodec::decode(duplicateIds, decoded, &error) &&
             error == "State pipeline plugin IDs must be unique" &&
             StateCodec::encode(decoded) == stateBeforeInvalidPipeline,
         "state rejects duplicate positive IDs before replacing the decoded authority");

  constexpr auto duplicateIdsAcrossPipelines =
      R"({"pipelineA":[{"id":7,"name":"Known"}],"pipelineB":[{"id":7,"name":"Future","unknown":true}]})";
  expect(!StateCodec::decode(duplicateIdsAcrossPipelines, decoded, &error) &&
             error == "State pipeline plugin IDs must be unique" &&
             StateCodec::encode(decoded) == stateBeforeInvalidPipeline,
         "state rejects IDs duplicated across A and B before replacing the decoded authority");

  std::string excessivePipeline{R"({"pipelineA":[)"};
  for (std::uint32_t id = 1; id <= kMaxPipelineNodes + 1u; ++id) {
    if (id != 1u) {
      excessivePipeline += ',';
    }
    excessivePipeline += R"({"id":)" + std::to_string(id) +
                         R"(,"name":"Future","unknown":true})";
  }
  excessivePipeline += "]}";
  expect(!StateCodec::decode(excessivePipeline, decoded, &error) &&
             error == "State pipeline exceeds the node limit" &&
             StateCodec::encode(decoded) == stateBeforeInvalidPipeline,
         "state rejects 129 nodes before replacing the decoded authority");

  constexpr auto futureVersion = R"({"formatVersion":2,"pipelineA":[]})";
  expect(!StateCodec::decode(futureVersion, decoded, &error),
         "future state version must be rejected");
  expect(!StateCodec::decode("{broken", decoded, &error), "corrupt JSON state rejection");
  expect(!StateCodec::decode("{}", decoded, &error), "state without a pipeline rejection");

  PipelineState preserved;
  preserved.plugins = {
      PluginState{7, "Known A", true, 0, 0, std::nullopt, "{}", false,
                  R"({"type":"KnownAPlugin","futureA":1})"},
      PluginState{12, "Future", true, 0, 0, std::nullopt, R"({"future":2})", true,
                  R"({"type":"FuturePlugin","futureField":{"v":2}})"},
      PluginState{19, "Known B", true, 0, 0, std::nullopt,
                  R"({"gain":1,"futureParam":{"v":2}})", false,
                  R"({"type":"KnownBPlugin","futureB":3,"futureRetained":4})"},
  };
  PipelineState restored;
  restored.plugins = {PluginState{19, "Known B", true, 0, 0, std::nullopt,
                                  R"({"gain":9})", false,
                                  R"({"type":"KnownBPlugin","futureB":9})"},
                      PluginState{7, "Known A"}};
  auto reconciled = reconcilePipelineSnapshot(preserved, std::move(restored), true);
  expect(reconciled.plugins.size() == 3 && reconciled.plugins[0].name == "Future" &&
             reconciled.plugins[1].id == 19 && reconciled.plugins[2].id == 7,
         "restored pipeline keeps stable IDs while incoming reorder is authoritative");
  const auto knownBExtras = choc::json::parse(reconciled.plugins[1].extraJson);
  const auto knownBParameters = choc::json::parse(reconciled.plugins[1].parametersJson);
  expect(reconciled.plugins[0].unknown &&
             reconciled.plugins[0].extraJson.find("FuturePlugin") != std::string::npos &&
             knownBExtras["futureB"].getWithDefault<std::int64_t>(0) == 9 &&
             knownBExtras["futureRetained"].getWithDefault<std::int64_t>(0) == 4,
         "unknown type/fields and known extra fields survive reconciliation");
  expect(knownBParameters["gain"].getWithDefault<std::int64_t>(0) == 9 &&
             knownBParameters["futureParam"].isObject(),
         "incoming parameters win while future logical fields survive reconciliation");

  const auto recursiveParameters = choc::json::parse(mergeExtraJsonObjects(
      R"({"future":{"mode":"preserved","nested":{"keep":2,"replace":3},"items":[{"keep":4,"replace":5},{"deletedTail":true}]}})",
      R"({"future":{"nested":{"replace":9},"items":[{"replace":10}]}})"));
  expect(recursiveParameters["future"]["mode"].getWithDefault<std::string>({}) ==
                 "preserved" &&
             recursiveParameters["future"]["nested"]["keep"]
                     .getWithDefault<std::int64_t>(0) == 2 &&
             recursiveParameters["future"]["nested"]["replace"]
                     .getWithDefault<std::int64_t>(0) == 9 &&
             recursiveParameters["future"]["items"].size() == 1 &&
             recursiveParameters["future"]["items"][0]["keep"]
                     .getWithDefault<std::int64_t>(0) == 4 &&
             recursiveParameters["future"]["items"][0]["replace"]
                     .getWithDefault<std::int64_t>(0) == 10,
         "recursive parameter merge preserves future members without restoring deleted array tails");

  PipelineState inserted;
  inserted.plugins = {PluginState{19, "Known B"}, PluginState{23, "Inserted"},
                      PluginState{7, "Known A"}};
  reconciled = reconcilePipelineSnapshot(reconciled, std::move(inserted), false);
  expect(reconciled.plugins.size() == 4 && reconciled.plugins[0].name == "Future" &&
             reconciled.plugins[1].name == "Known B" &&
             reconciled.plugins[2].name == "Inserted" &&
             reconciled.plugins[3].name == "Known A",
         "normal rebuild keeps incoming reorder and intermediate insertion");

  PipelineState beforeRename;
  beforeRename.plugins = {PluginState{27, "Old Name", true, 0, 0, std::nullopt,
                                      R"({"futureParam":{"keep":2}})", false,
                                      R"({"type":"KnownPlugin","future":3})"}};
  PipelineState renameIncoming;
  renameIncoming.plugins = {PluginState{27, "New Name", true, 0, 0, std::nullopt,
                                        R"({"gain":4})", false,
                                        R"({"type":"KnownPlugin"})"}};
  const auto renamed =
      reconcilePipelineSnapshot(beforeRename, std::move(renameIncoming), true);
  expect(renamed.plugins.size() == 1 && renamed.plugins[0].name == "New Name" &&
             renamed.plugins[0].parametersJson.find("futureParam") != std::string::npos &&
             renamed.plugins[0].extraJson.find("future") != std::string::npos,
         "serialized type keeps renamed logical plug-in identity without duplicates");

  UndoOpaqueStateStore undoOpaqueState;
  PipelineState beforeDelete;
  beforeDelete.plugins = {PluginState{31, "Known", true, 0, 0, std::nullopt,
                                      R"({"gain":1,"futureParam":{"v":2,"nested":{"keep":4},"items":[{"future":5,"value":1},{"deletedTail":true}]}})", false,
                                      R"({"type":"KnownPlugin","future":3,"override":1})"}};
  auto deleted = undoOpaqueState.reconcile('A', beforeDelete, {}, false);
  expect(deleted.plugins.empty(), "delete removes known plug-in from current state");

  PipelineState undoIncoming;
  undoIncoming.plugins = {PluginState{31, "Known Renamed", true, 0, 0, std::nullopt,
                                      R"({"gain":9,"futureParam":{"nested":{"edited":6},"items":[{"value":7}]}})", false,
                                      R"({"type":"KnownPlugin","override":8})"}};
  auto undone = undoOpaqueState.reconcile('A', deleted, std::move(undoIncoming), false);
  const auto undoParameters = choc::json::parse(undone.plugins[0].parametersJson);
  const auto undoExtra = choc::json::parse(undone.plugins[0].extraJson);
  expect(undone.plugins.size() == 1 && undone.plugins[0].name == "Known Renamed" &&
             undoParameters["gain"].getWithDefault<std::int64_t>(0) == 9 &&
             undoParameters["futureParam"]["v"].getWithDefault<std::int64_t>(0) == 2 &&
             undoParameters["futureParam"]["nested"]["keep"]
                     .getWithDefault<std::int64_t>(0) == 4 &&
             undoParameters["futureParam"]["nested"]["edited"]
                     .getWithDefault<std::int64_t>(0) == 6 &&
             undoParameters["futureParam"]["items"].size() == 1 &&
             undoParameters["futureParam"]["items"][0]["future"]
                     .getWithDefault<std::int64_t>(0) == 5 &&
             undoParameters["futureParam"]["items"][0]["value"]
                     .getWithDefault<std::int64_t>(0) == 7 &&
             undoExtra["future"].getWithDefault<std::int64_t>(0) == 3 &&
             undoExtra["override"].getWithDefault<std::int64_t>(0) == 8,
         "rename undo restores nested opaque fields while incoming values win without duplicates");

  auto redone = undoOpaqueState.reconcile('A', undone, {}, false);
  expect(redone.plugins.empty(), "redo delete records opaque state again");
  PipelineState secondUndoIncoming;
  secondUndoIncoming.plugins = {PluginState{31, "Known Renamed", true, 0, 0, std::nullopt,
                                            R"({"gain":5})", false,
                                            R"({"type":"KnownPlugin"})"}};
  auto secondUndo =
      undoOpaqueState.reconcile('A', redone, std::move(secondUndoIncoming), false);
  const auto secondUndoParameters = choc::json::parse(secondUndo.plugins[0].parametersJson);
  expect(secondUndoParameters["gain"].getWithDefault<std::int64_t>(0) == 5 &&
             secondUndoParameters["futureParam"]["v"].getWithDefault<std::int64_t>(0) == 2,
         "redo deletion remains recoverable by a later undo");
}

void testMessageRouter() {
  constexpr auto rebuild = R"({
    "type":"pipeline/rebuild",
    "payload":{"pipeline":"B","masterBypass":true,"plugins":[
      {"id":1,"type":"SectionPlugin","name":"Section","enabled":true},
      {"id":2,"type":"TestGainPlugin","name":"Spectrum Analyzer","enabled":true,"inputBus":1,
       "outputBus":2,"channel":"L","parameters":{"gain":0.5},
       "wasmParams":[0.5],"wasmParamsHash":123,"wasmParamBytes":[1,255],
       "futurePayload":{"v":1}}
    ]}}
  )";
  RoutedUiMessage message;
  std::string error;
  expect(MessageRouter::decode(rebuild, message, &error), "bridge rebuild decode: " + error);
  expect(message.action == UiAction::rebuildPipeline && message.pipeline == 'B' &&
             message.masterBypass && message.plugins.size() == 2,
         "bridge rebuild routing");
  const auto &plugin = message.plugins[1];
  expect(plugin.logical.name == "Spectrum Analyzer" && plugin.logical.inputBus == 1 &&
             plugin.logical.outputBus == 2 && plugin.logical.channel == "L",
         "bridge logical parameters");
  expect(plugin.runtime.type == "TestGainPlugin" &&
             plugin.runtime.packedParameters == std::vector<float>{0.5f} &&
             plugin.runtime.parameterBytes == std::vector<std::uint8_t>({1, 255}),
         "bridge packed parameters");
  expect(plugin.logical.extraJson.find("futurePayload") != std::string::npos,
         "bridge unknown plugin fields preservation");
  expect(plugin.logical.extraJson.find("TestGainPlugin") != std::string::npos,
         "bridge runtime type preservation");

  expect(MessageRouter::decode(
             R"({"type":"pipeline/rebuild","payload":{"pipeline":"A","plugins":[{"id":9,"type":"TestGainPlugin","name":"Constrained here","parameters":{"gain":0.75},"wasmParams":[0.75],"wasmParamsHash":123,"executionCapabilities":{"requiresWasm":true,"supportedSampleRates":[48000,96000],"supportedChannelModes":["mono","stereo-pair"]},"futurePayload":{"v":2}}]}})",
             message, &error) && message.plugins.size() == 1 &&
             message.plugins[0].runtime.type == "TestGainPlugin" &&
             message.plugins[0].runtime.packedParameters == std::vector<float>{0.75f} &&
             message.plugins[0].runtime.executionCapabilities.constrainsSampleRate &&
             message.plugins[0].runtime.executionCapabilities.supportedSampleRateCount == 2u &&
             message.plugins[0].runtime.executionCapabilities.constrainsChannelMode &&
             message.plugins[0].logical.extraJson.find("futurePayload") != std::string::npos &&
             message.plugins[0].logical.extraJson.find("executionCapabilities") ==
                 std::string::npos,
         "bridge keeps bounded execution capabilities with the dormant runtime image");
  const auto &capabilities = message.plugins[0].runtime.executionCapabilities;
  expect(supportsExecutionContext(capabilities, std::nullopt, 48000.0, 2u) &&
             !supportsExecutionContext(capabilities, std::nullopt, 384000.0, 2u) &&
             !supportsExecutionContext(capabilities, std::optional<std::string>{"A"},
                                       48000.0, 2u) &&
             !supportsExecutionContext(capabilities, std::optional<std::string>{"78"},
                                       48000.0, 2u) &&
             supportsExecutionContext(capabilities, std::optional<std::string>{"78"},
                                      48000.0, 8u),
         "native admission matches upstream sample-rate and channel-mode routing");
  expect(!MessageRouter::decode(
             R"({"type":"pipeline/rebuild","payload":{"plugins":[{"id":9,"type":"TestGainPlugin","wasmParams":[0.75],"wasmParamsHash":123,"executionCapabilities":{"supportedChannelModes":["future-mode"]}}]}})",
             message, &error),
         "bridge rejects execution modes outside the shared vocabulary");

  expect(!MessageRouter::decode(
             R"({"type":"pipeline/rebuild","payload":{"plugins":[{"id":7,"type":"SectionPlugin"},{"id":7,"type":"TestGainPlugin","wasmParams":[1],"wasmParamsHash":1}]}})",
             message, &error) &&
             error == "Pipeline plugin IDs must be unique",
         "bridge rejects duplicate plug-in IDs");

  expect(MessageRouter::decode(
             R"({"type":"pipeline/restoreHistory","payload":{"pipelineA":[],"pipelineB":null,"pipelineBInitialized":false,"currentPipeline":"A"}})",
             message, &error) && message.action == UiAction::restoreHistory &&
             message.pipelineA.empty() && !message.pipelineBInitialized && message.pipeline == 'A',
         "history restore preserves uninitialized pipeline B");
  expect(MessageRouter::decode(
             R"({"type":"pipeline/restoreHistory","payload":{"pipelineA":[],"pipelineB":[],"pipelineBInitialized":true,"currentPipeline":"B"}})",
             message, &error) && message.action == UiAction::restoreHistory &&
             message.pipelineBInitialized && message.pipelineB.empty() && message.pipeline == 'B',
         "history restore distinguishes initialized empty pipeline B");
  expect(MessageRouter::decode(
             R"({"type":"pipeline/restoreHistory","payload":{"pipelineA":[],"pipelineB":[{"id":44,"type":"TestGainPlugin","name":"Inactive","parameters":{"gain":2},"wasmParams":[2],"wasmParamsHash":9}],"pipelineBInitialized":true,"currentPipeline":"A"}})",
             message, &error) && message.action == UiAction::restoreHistory &&
             message.pipeline == 'A' && message.pipelineB.size() == 1 &&
             message.pipelineB[0].logical.id == 44 &&
             message.pipelineB[0].runtime.packedParameters == std::vector<float>{2.0f},
         "history restore carries an edited inactive pipeline in the same request");
  expect(!MessageRouter::decode(
             R"({"type":"pipeline/restoreHistory","payload":{"pipelineA":[{"id":44,"type":"TestGainPlugin"}],"pipelineB":[{"id":44,"type":"FuturePlugin"}],"pipelineBInitialized":true,"currentPipeline":"A"}})",
             message, &error) &&
             error == "Pipeline plugin IDs must be unique",
         "history restore rejects IDs duplicated across A and B");
  expect(MessageRouter::decode(
             R"({"type":"automation/edit","payload":{"pipeline":"B","pluginId":44,"pluginType":"TestGainPlugin","parameterKey":"gain","elementIndex":2,"normalized":0.75}})",
             message, &error) && message.action == UiAction::editAutomationParameter &&
             message.pipeline == 'B' && message.pluginId == 44 &&
             message.pluginType == "TestGainPlugin" && message.parameterKey == "gain" &&
             message.elementIndex == 2 && message.normalizedValue == 0.75,
         "automation edit routes a complete stable target identity");
  expect(!MessageRouter::decode(
             R"({"type":"automation/edit","payload":{"pluginId":44,"pluginType":"TestGainPlugin","parameterKey":"gain","normalized":1.01}})",
             message, &error),
         "automation edit rejects values outside the normalized host domain");
  expect(MessageRouter::decode(
             R"({"type":"pipeline/rebuild","payload":{"pipeline":"A","plugins":[],"automationEdits":[{"pipeline":"B","pluginId":44,"pluginType":"TestGainPlugin","parameterKey":"gain","elementIndex":2,"normalized":0.75}]}})",
             message, &error) && message.action == UiAction::rebuildPipeline &&
             message.automationEdits.size() == 1 &&
             message.automationEdits[0].pipeline == 'B' &&
             message.automationEdits[0].pluginId == 44 &&
             message.automationEdits[0].pluginType == "TestGainPlugin" &&
             message.automationEdits[0].parameterKey == "gain" &&
             message.automationEdits[0].elementIndex == 2 &&
             message.automationEdits[0].normalized == 0.75,
         "a preset load carries the automation targets it explicitly names");
  expect(MessageRouter::decode(
             R"({"type":"pipeline/restoreHistory","payload":{"pipelineA":[],"pipelineB":null,"pipelineBInitialized":false,"currentPipeline":"A","automationEdits":[{"pipeline":"A","pluginId":9,"pluginType":"DCOffsetPlugin","parameterKey":"of","elementIndex":0,"normalized":0.25}]}})",
             message, &error) && message.action == UiAction::restoreHistory &&
             message.automationEdits.size() == 1 &&
             message.automationEdits[0].pluginId == 9 &&
             message.automationEdits[0].normalized == 0.25,
         "an undo carries the automation targets it explicitly names");
  expect(MessageRouter::decode(
             R"({"type":"pipeline/rebuild","payload":{"pipeline":"A","plugins":[]}})",
             message, &error) && message.action == UiAction::rebuildPipeline &&
             message.automationEdits.empty(),
         "a bulk message from an older UI names no automation target");
  expect(MessageRouter::decode(
             R"({"type":"pipeline/updatePlugin","payload":{"pipeline":"A","plugin":{"id":44,"type":"TestGainPlugin","name":"Gain","parameters":{"gain":2},"wasmParams":[2],"wasmParamsHash":9},"automationEdits":[{"pipeline":"A","pluginId":44,"pluginType":"TestGainPlugin","parameterKey":"gain","elementIndex":2,"normalized":0.25},{"pipeline":"A","pluginId":44,"pluginType":"TestGainPlugin","parameterKey":"gain","elementIndex":2,"normalized":0.5}]}})",
             message, &error) && message.action == UiAction::updatePlugin &&
             message.plugins.size() == 1 && message.plugins[0].logical.id == 44 &&
             message.automationEdits.size() == 2 &&
             message.automationEdits[0].normalized == 0.25 &&
             message.automationEdits[1].normalized == 0.5 &&
             message.automationEdits[1].elementIndex == 2 &&
             message.automationEdits[0].bindIfUnbound &&
             message.automationEdits[1].bindIfUnbound,
         "a coalesced plug-in update carries every gesture of its frame in order");
  expect(MessageRouter::decode(
             R"({"type":"pipeline/updatePlugin","payload":{"pipeline":"A","plugin":{"id":44,"type":"TestGainPlugin","name":"Gain","parameters":{"gain":2},"wasmParams":[2],"wasmParamsHash":9},"automationEdits":[{"pipeline":"A","pluginId":44,"pluginType":"TestGainPlugin","parameterKey":"gain","elementIndex":2,"normalized":0.25,"bindIfUnbound":false},{"pipeline":"A","pluginId":44,"pluginType":"TestGainPlugin","parameterKey":"gain","elementIndex":2,"normalized":0.5}]}})",
             message, &error) && message.action == UiAction::updatePlugin &&
             message.automationEdits.size() == 2 &&
             !message.automationEdits[0].bindIfUnbound &&
             message.automationEdits[1].bindIfUnbound,
         "a correction keeps its place in the ordered list and claims no lane, "
         "while an edit that omits the field stays a gesture");
  expect(MessageRouter::decode(
             R"({"type":"pipeline/updatePlugin","payload":{"pipeline":"A","plugin":{"id":44,"type":"TestGainPlugin","name":"Gain","parameters":{"gain":2},"wasmParams":[2],"wasmParamsHash":9}}})",
             message, &error) && message.action == UiAction::updatePlugin &&
             message.automationEdits.empty(),
         "a plug-in update from an older UI names no automation target");
  expect(!MessageRouter::decode(
             R"({"type":"pipeline/rebuild","payload":{"pipeline":"A","plugins":[],"automationEdits":[{"pipeline":"A","pluginId":9,"pluginType":"DCOffsetPlugin","parameterKey":"of","elementIndex":0,"normalized":1.5}]}})",
             message, &error) &&
             error == "Automation edit has an invalid target or value",
         "a named automation target is validated exactly like a single gesture");

  expect(MessageRouter::decode(
             R"({"type":"pipeline/assetBegin","payload":{"pluginId":44,"slot":0,"formatTag":1,"channels":1,"frames":600,"topology":1,"headBlock":128,"rateDivider":1,"pathCount":0,"inputCount":0,"processingChannels":2,"footprintBytes":4194304,"byteSize":2432,"operationRevision":7}})",
             message, &error) && message.action == UiAction::beginPluginAsset &&
             message.asset.logicalId == 44 && message.asset.frames == 600 &&
             message.asset.headBlock == 128 && message.asset.footprintBytes == 4194304 &&
             message.assetByteSize == 2432 && message.operationRevision == 7,
         "DSP asset begin routing");
  expect(MessageRouter::decode(
             R"({"type":"pipeline/assetChunk","payload":{"pluginId":44,"slot":0,"operationRevision":7,"offset":192,"data":"AQID"}})",
             message, &error) && message.action == UiAction::appendPluginAsset &&
             message.asset.logicalId == 44 && message.assetOffset == 192 &&
             message.content == "AQID",
         "DSP asset chunk routing");
  expect(MessageRouter::decode(
             R"({"type":"pipeline/assetCommit","payload":{"pluginId":44,"slot":0,"operationRevision":7}})",
             message, &error) && message.action == UiAction::commitPluginAsset,
         "DSP asset commit routing");
  expect(MessageRouter::decode(
             R"({"type":"pipeline/assetClear","payload":{"pluginId":44,"slot":0}})",
             message, &error) && message.action == UiAction::clearPluginAsset,
         "DSP asset clear routing");
  expect(MessageRouter::decode(
             R"({"type":"pipeline/assetState","payload":{"pluginId":44,"slot":0}})",
             message, &error) && message.action == UiAction::readPluginAssetState,
         "DSP asset state routing");
  expect(!MessageRouter::decode(
             R"({"type":"pipeline/assetBegin","payload":{"pluginId":44,"slot":0,"formatTag":1,"channels":1,"frames":600,"topology":1,"headBlock":128,"rateDivider":1,"pathCount":0,"inputCount":0,"processingChannels":2,"footprintBytes":33554433,"byteSize":2432,"operationRevision":7}})",
             message, &error),
         "DSP asset footprint above the native limit must be rejected");
  expect(!MessageRouter::decode(
             R"({"type":"pipeline/assetCommit","payload":{"pluginId":44,"slot":0}})",
             message, &error),
         "DSP asset commit requires an operation revision");

  std::string oversized = R"({"type":"pipeline/rebuild","payload":{"plugins":[)";
  for (std::size_t index = 0; index <= kMaxPluginInstances; ++index) {
    if (index != 0) oversized += ',';
    oversized += R"({"id":)" + std::to_string(index + 1u) +
                 R"(,"type":"TestGainPlugin","wasmParams":[1],"wasmParamsHash":1})";
  }
  oversized += "]}}";
  expect(!MessageRouter::decode(oversized, message, &error),
         "97 native instances must be rejected by the bridge");

  expect(MessageRouter::decode(
             R"({"type":"os/set","payload":{"factor":8,"phase":"minimum","quality":"ultra"}})",
             message, &error),
         "bridge oversampling decode");
  expect(message.action == UiAction::setOversampling && message.oversampling.factor == 8 &&
             message.oversampling.phase == OversamplingPhase::minimum &&
             message.oversampling.quality == FilterQuality::ultra,
         "bridge oversampling values");
  expect(MessageRouter::decode(
             R"({"type":"host/openExternal","payload":{"url":"https://effetune.frieve.com/docs/plugins/basics.html#volume"}})",
             message, &error) && message.action == UiAction::openExternalUrl &&
             message.url == "https://effetune.frieve.com/docs/plugins/basics.html#volume",
         "external HTTPS URL routing");
  expect(MessageRouter::decode(
             R"({"type":"host/openExternal","payload":{"url":"http://localhost/help"}})",
             message, &error) && message.action == UiAction::openExternalUrl,
         "external HTTP URL routing");
  expect(!MessageRouter::decode(
             R"({"type":"host/openExternal","payload":{"url":"file:///tmp/help.html"}})",
             message, &error),
         "external file URL must be rejected");
  expect(!MessageRouter::decode(
             R"({"type":"host/openExternal","payload":{"url":"https://example.com/a b"}})",
             message, &error),
         "external URL containing whitespace must be rejected");
  expect(!MessageRouter::decode(R"({"type":"unsupported"})", message, &error),
         "unsupported bridge message must be rejected");
  expect(MessageRouter::decode(
             R"({"type":"dialog/savePreset","payload":{"defaultName":"mix.effetune_preset"}})",
             message, &error) && message.action == UiAction::savePresetDialog &&
             message.defaultName == "mix.effetune_preset",
         "preset save dialog routing");
  expect(MessageRouter::decode(R"({"type":"telemetry/discard"})", message, &error) &&
             message.action == UiAction::discardTelemetry,
         "telemetry discard routing");
}

void testResampler() {
  constexpr std::uint32_t frames = 512;
  std::array<float, frames> input{};
  std::array<float, frames> output{};
  for (std::uint32_t index = 0; index < frames; ++index) {
    input[index] = static_cast<float>(index) / static_cast<float>(frames);
  }
  const float *inputs[] = {input.data()};
  float *outputs[] = {output.data()};

  Oversampler passthrough;
  passthrough.prepare({1, OversamplingPhase::linear, FilterQuality::medium}, 1, frames);
  const auto *unchanged = passthrough.upsample(inputs, frames);
  expect(unchanged != nullptr && passthrough.downsample(unchanged, frames, outputs),
         "1x resampler process");
  expect(input == output, "1x resampler bit transparency");

  Oversampler linear;
  linear.prepare({2, OversamplingPhase::linear, FilterQuality::medium}, 1, frames);
  expect(linear.latencyHostFrames() == 101u, "linear 2x reported latency");
  input.fill(1.0f);
  const auto *upsampled = linear.upsample(inputs, frames);
  expect(upsampled != nullptr && linear.downsample(upsampled, frames * 2u, outputs),
         "linear 2x round trip");
  expect(std::isfinite(output.back()) && std::abs(output.back() - 1.0f) < 1.0e-3f,
         "linear 2x DC transparency");

  Oversampler minimum;
  minimum.prepare({2, OversamplingPhase::minimum, FilterQuality::low}, 1, frames);
  expect(minimum.latencyHostFrames() < linear.latencyHostFrames(),
         "minimum phase lower energy delay");
  input.fill(0.0f);
  input[0] = 1.0f;
  upsampled = minimum.upsample(inputs, frames);
  expect(upsampled != nullptr && minimum.downsample(upsampled, frames * 2u, outputs),
         "minimum phase 2x round trip");
  for (const auto sample : output) {
    expect(std::isfinite(sample), "minimum phase finite impulse response");
  }
}

void testEngineHost() {
  EngineHost engine;
  std::string error;
  expect(engine.prepare(48000.0, 2, EngineHost::kDefaultTelemetryBytes, &error),
         "engine prepare: " + error);

  const auto kernel = engine.kernels().find("TestGainPlugin");
  expect(kernel != engine.kernels().end(), "test gain kernel registration");
  PipelineState pipeline;
  pipeline.plugins = {PluginState{2, "Section", true}, PluginState{1, "TestGainPlugin", true}};
  RuntimePlugin runtime;
  runtime.logicalId = 1;
  runtime.type = "TestGainPlugin";
  runtime.packedParameters = {0.5f};
  runtime.paramsHash = kernel->second.paramsHash;
  expect(engine.rebuild(pipeline, {runtime}, &error), "engine rebuild: " + error);
  for (std::uint32_t attempt = 0; attempt < 100; ++attempt) {
    expect(!engine.rebuild(pipeline, {runtime, runtime}, &error) &&
               error == "Native DSP plug-in IDs must be unique",
           "engine rejects duplicate logical IDs without consuming instances");
  }
  expect(engine.rebuild(pipeline, {runtime}, &error),
         "engine rebuild after duplicate logical IDs: " + error);

  std::array<float, EngineHost::kMaxProcessFrames> left{};
  std::array<float, EngineHost::kMaxProcessFrames> right{};
  left.fill(1.0f);
  right.fill(-1.0f);
  float *channels[] = {left.data(), right.data()};
  expect(engine.tryProcessBlock(channels, 2, EngineHost::kMaxProcessFrames, 0.0, false),
         "engine process");
  for (std::size_t index = 0; index < left.size(); ++index) {
    expect(std::abs(left[index] - 0.5f) < 1.0e-7f &&
               std::abs(right[index] + 0.5f) < 1.0e-7f,
           "native DSP gain output");
  }

  std::array<float, 4> smallLeft{2.0f, 2.0f, 2.0f, 2.0f};
  std::array<float, 4> smallRight{-2.0f, -2.0f, -2.0f, -2.0f};
  float *smallChannels[] = {smallLeft.data(), smallRight.data()};
  expect(engine.tryProcessBlock(smallChannels, 2, 4, 0.1, false),
         "engine accepts a four-frame host block");
  expect(smallLeft == std::array<float, 4>{1.0f, 1.0f, 1.0f, 1.0f} &&
             smallRight == std::array<float, 4>{-1.0f, -1.0f, -1.0f, -1.0f},
         "engine processes a four-frame host block immediately");
  expect(!engine.tryProcessBlock(channels, 2, 0, 0.2, false) &&
             !engine.tryProcessBlock(channels, 2, EngineHost::kMaxProcessFrames + 1u,
                                     0.2, false),
         "engine enforces the variable process-frame bounds");

  pipeline.plugins[0].enabled = false;
  AudioCommand descriptorCommand;
  expect(engine.makeDescriptorCommand(pipeline, std::span<const RuntimePlugin>(&runtime, 1),
                                      descriptorCommand, &error),
         "section disable descriptor command: " + error);
  std::uint64_t appliedDescriptorRevision = 0;
  expect(engine.applyDescriptorCommand(descriptorCommand, appliedDescriptorRevision, &error),
         "apply section disable descriptor off the audio path: " + error);
  left.fill(1.0f);
  right.fill(-1.0f);
  expect(engine.tryProcessBlock(channels, 2, EngineHost::kMaxProcessFrames, 0.0, true),
         "disabled section command under master bypass");
  expect(left[0] == 1.0f && right[0] == -1.0f,
         "descriptor command preserves master-bypassed audio");
  pipeline.plugins[0].enabled = true;
  expect(engine.makeDescriptorCommand(pipeline, std::span<const RuntimePlugin>(&runtime, 1),
                                      descriptorCommand, &error),
         "section enable descriptor command: " + error);
  expect(engine.applyDescriptorCommand(descriptorCommand, appliedDescriptorRevision, &error),
         "apply section enable descriptor off the audio path: " + error);
  left.fill(1.0f);
  right.fill(-1.0f);
  expect(engine.tryProcessBlock(channels, 2, EngineHost::kMaxProcessFrames, 0.0, false),
         "re-enabled section command");
  expect(std::abs(left[0] - 0.5f) < 1.0e-7f,
         "queued section toggle preserves the native instance");

  pipeline.plugins[0].enabled = false;
  expect(engine.updateDescriptor(pipeline, &error), "direct descriptor update: " + error);
  left.fill(1.0f);
  right.fill(-1.0f);
  expect(engine.tryProcessBlock(channels, 2, EngineHost::kMaxProcessFrames, 0.0, false),
         "direct disabled section process");
  expect(left[0] == 1.0f && right[0] == -1.0f,
         "direct descriptor update remains available off the audio path");
}

void testContextualBypassPreservesCrossBusRouting() {
  EngineHost engine;
  std::string error;
  expect(engine.prepare(48000.0, 2, EngineHost::kDefaultTelemetryBytes, &error),
         "contextual bypass engine prepare: " + error);
  const auto kernel = engine.kernels().find("TestGainPlugin");
  expect(kernel != engine.kernels().end(), "contextual bypass test-gain kernel");

  PipelineState pipeline;
  PluginState send;
  send.id = 81;
  send.name = "Constrained send";
  send.inputBus = 0;
  send.outputBus = 1;
  send.channel = "L";
  PluginState returning;
  returning.id = 82;
  returning.name = "Return gain";
  returning.inputBus = 1;
  returning.outputBus = 0;
  returning.channel = "L";
  pipeline.plugins = {send, returning};

  RuntimePlugin constrained;
  constrained.logicalId = 81;
  constrained.type = "TestGainPlugin";
  constrained.packedParameters = {0.5f};
  constrained.paramsHash = kernel->second.paramsHash;
  RuntimePlugin returnGain;
  returnGain.logicalId = 82;
  returnGain.type = "TestGainPlugin";
  returnGain.packedParameters = {0.5f};
  returnGain.paramsHash = kernel->second.paramsHash;
  std::vector runtimes{constrained, returnGain};

  std::array<float, EngineHost::kMaxProcessFrames> left{};
  std::array<float, EngineHost::kMaxProcessFrames> right{};
  float *channels[] = {left.data(), right.data()};
  const auto render = [&] {
    left.fill(1.0f);
    right.fill(-1.0f);
    expect(engine.tryProcessBlock(channels, 2, EngineHost::kMaxProcessFrames,
                                  0.0, false),
           "contextual cross-bus process");
  };

  expect(engine.rebuild(pipeline, runtimes, &error),
         "admitted cross-bus rebuild: " + error);
  render();
  expect(std::abs(left[0] - 1.25f) < 1.0e-7f && right[0] == -1.0f,
         "admitted node uses the selected left-channel cross-bus route");

  runtimes[0].contextuallyBypassed = true;
  expect(engine.rebuild(pipeline, runtimes, &error),
         "contextually bypassed cross-bus rebuild: " + error);
  constexpr std::array bypassedParameterUpdate{0.0f};
  expect(engine.pipelineLatency() == 0u &&
             engine.updateParameters(81, bypassedParameterUpdate,
                                     constrained.paramsHash),
         "synthetic bypass is zero latency and accepts original-hash parameters as a no-op");
  EngineHost::ProcessBatch batch;
  EngineHost::ResolvedParameterTarget target;
  left.fill(1.0f);
  right.fill(-1.0f);
  expect(engine.beginProcessBatch(batch) &&
             batch.resolveParameterTarget(81, constrained.paramsHash, target) &&
             target.contextuallyBypassed &&
             batch.stageParameters(target, bypassedParameterUpdate) &&
             batch.processChunk(channels, 2, EngineHost::kMaxProcessFrames,
                                0.0, false) &&
             batch.finish(),
         "audio-batch automation treats the dormant original image as a successful no-op");
  expect(std::abs(left[0] - 1.5f) < 1.0e-7f && right[0] == -1.0f,
         "contextual bypass preserves bus send, unity gain, and channel selection");

  runtimes[0].contextuallyBypassed = false;
  expect(engine.rebuild(pipeline, runtimes, &error),
         "re-admitted cross-bus rebuild: " + error);
  render();
  expect(std::abs(left[0] - 1.25f) < 1.0e-7f && right[0] == -1.0f,
         "re-admission restores the dormant original runtime image");

  {
    EngineHost monoEngine;
    expect(monoEngine.prepare(48000.0, 1, EngineHost::kDefaultTelemetryBytes,
                              &error),
           "mono channel-context engine prepare: " + error);
    const auto monoKernel = monoEngine.kernels().find("TestGainPlugin");
    expect(monoKernel != monoEngine.kernels().end(),
           "mono channel-context test-gain kernel");
    PipelineState monoPipeline;
    PluginState monoSend;
    monoSend.id = 83;
    monoSend.name = "Mono send";
    monoSend.outputBus = 1;
    PluginState monoReturn;
    monoReturn.id = 84;
    monoReturn.name = "Mono return";
    monoReturn.inputBus = 1;
    monoPipeline.plugins = {monoSend, monoReturn};
    RuntimePlugin monoSendRuntime;
    monoSendRuntime.logicalId = 83;
    monoSendRuntime.type = "TestGainPlugin";
    monoSendRuntime.packedParameters = {0.5f};
    monoSendRuntime.paramsHash = monoKernel->second.paramsHash;
    RuntimePlugin monoReturnRuntime = monoSendRuntime;
    monoReturnRuntime.logicalId = 84;
    std::array<float, EngineHost::kMaxProcessFrames> mono{};
    mono.fill(1.0f);
    float *monoChannels[] = {mono.data()};
    expect(monoEngine.rebuild(
               monoPipeline, {monoSendRuntime, monoReturnRuntime}, &error) &&
               monoEngine.tryProcessBlock(monoChannels, 1,
                                          EngineHost::kMaxProcessFrames,
                                          0.0, false),
           "mono null-selection cross-bus process: " + error);
    expect(std::abs(mono[0] - 1.25f) < 1.0e-7f,
           "mono null selection processes channel zero instead of skipping a stereo pair");
  }

  {
    EngineHost partialPairEngine;
    expect(partialPairEngine.prepare(
               48000.0, 3, EngineHost::kDefaultTelemetryBytes, &error),
           "partial-pair channel-context engine prepare: " + error);
    const auto partialKernel =
        partialPairEngine.kernels().find("TestGainPlugin");
    expect(partialKernel != partialPairEngine.kernels().end(),
           "partial-pair channel-context test-gain kernel");
    PipelineState partialPipeline;
    PluginState partialSend;
    partialSend.id = 85;
    partialSend.name = "Partial-pair send";
    partialSend.outputBus = 1;
    partialSend.channel = "34";
    PluginState partialReturn;
    partialReturn.id = 86;
    partialReturn.name = "Partial-pair return";
    partialReturn.inputBus = 1;
    partialReturn.channel = "3";
    partialPipeline.plugins = {partialSend, partialReturn};
    RuntimePlugin partialSendRuntime;
    partialSendRuntime.logicalId = 85;
    partialSendRuntime.type = "TestGainPlugin";
    partialSendRuntime.packedParameters = {0.5f};
    partialSendRuntime.paramsHash = partialKernel->second.paramsHash;
    partialSendRuntime.contextuallyBypassed = true;
    RuntimePlugin partialReturnRuntime = partialSendRuntime;
    partialReturnRuntime.logicalId = 86;
    partialReturnRuntime.contextuallyBypassed = false;
    std::array<float, EngineHost::kMaxProcessFrames> first{};
    std::array<float, EngineHost::kMaxProcessFrames> second{};
    std::array<float, EngineHost::kMaxProcessFrames> third{};
    first.fill(10.0f);
    second.fill(-10.0f);
    third.fill(1.0f);
    float *partialChannels[] = {first.data(), second.data(), third.data()};
    expect(partialPairEngine.rebuild(
               partialPipeline, {partialSendRuntime, partialReturnRuntime},
               &error) &&
               partialPairEngine.tryProcessBlock(
                   partialChannels, 3, EngineHost::kMaxProcessFrames, 0.0,
                   false),
           "contextual partial-pair cross-bus process: " + error);
    expect(first[0] == 10.0f && second[0] == -10.0f &&
               std::abs(third[0] - 1.5f) < 1.0e-7f,
           "contextual bypass narrows channel 34 to the available third channel");
  }
}

void testEngineAssetTransferAndReplay() {
  EngineHost engine;
  std::string error;
  expect(engine.prepare(48000.0, 2, EngineHost::kDefaultTelemetryBytes, &error),
         "asset engine prepare: " + error);

  const auto kernel = engine.kernels().find("IRReverbPlugin");
  expect(kernel != engine.kernels().end(), "IR reverb kernel registration");
  expect(kernel->second.assetCapacity == EngineHost::kMaximumAssetPayloadBytes,
         "IR reverb native asset capacity");

  PipelineState pipeline;
  pipeline.plugins = {PluginState{91, "IRReverbPlugin", true}};
  RuntimePlugin runtime;
  runtime.logicalId = 91;
  runtime.type = "IRReverbPlugin";
  runtime.packedParameters = {0.0f, 1.0f, 1.0f, 0.0f, 1.0f, -96.0f, 0.0f};
  runtime.paramsHash = kernel->second.paramsHash;
  expect(engine.rebuild(pipeline, {runtime}, &error), "IR reverb engine rebuild: " + error);

  constexpr std::uint32_t frames = 600;
  RuntimeAsset asset;
  asset.logicalId = 91;
  asset.slot = 0;
  asset.formatTag = ET_ASSET_F32_MULTICH;
  asset.channels = 1;
  asset.frames = frames;
  asset.topology = 1;
  asset.headBlock = 128;
  asset.rateDivider = 1;
  asset.processingChannels = 2;
  asset.payload.resize(32u + static_cast<std::size_t>(frames) * sizeof(float));
  const auto writeU32 = [&asset](const std::size_t offset, const std::uint32_t value) {
    std::memcpy(asset.payload.data() + offset, &value, sizeof(value));
  };
  writeU32(0, 0x31415445u);
  writeU32(4, asset.channels);
  writeU32(8, asset.frames);
  writeU32(12, 48000u);
  writeU32(16, asset.topology);
  writeU32(20, 0u);
  constexpr float impulse = 1.0f;
  std::memcpy(asset.payload.data() + 32u, &impulse, sizeof(impulse));
  asset.footprintBytes = 4u * 1024u * 1024u;

  const auto planRevisionBeforeAsset = engine.pipelinePlanRevision();
  expect(engine.setAsset(asset, &error), "IR reverb asset staging: " + error);
  expect((engine.assetState(91, 0) & 0xffu) == ET_ASSET_STATE_PREPARING,
         "IR reverb asset enters preparing state");
  expect(engine.pipelineLatency() == 128u &&
             engine.pipelinePlanRevision() > planRevisionBeforeAsset,
         "asset commit publishes its latency before another audio block is rendered");

  std::array<float, EngineHost::kMaxProcessFrames> left{};
  std::array<float, EngineHost::kMaxProcessFrames> right{};
  float *channels[] = {left.data(), right.data()};
  const auto prepareAsset = [](EngineHost &target, float *const *targetChannels,
                               const char *context) {
    std::uint32_t quantum = 0;
    for (; quantum < 128u &&
           (target.assetState(91, 0) & 0xffu) == ET_ASSET_STATE_PREPARING;
         ++quantum) {
      expect(target.tryProcessBlock(targetChannels, 2, EngineHost::kMaxProcessFrames,
                                    static_cast<double>(quantum) / 375.0, false),
             context);
    }
    expect((target.assetState(91, 0) & 0xffu) == ET_ASSET_STATE_ACTIVE, context);
  };
  const auto stagedPlanRevision = engine.pipelinePlanRevision();
  prepareAsset(engine, channels, "IR reverb asset becomes active");
  expect(engine.pipelineLatency() == 128u &&
             engine.pipelinePlanRevision() == stagedPlanRevision,
         "asset readiness preserves the latency already published at commit");
  const auto activeAssetRefreshes = engine.processCounters().latencyRefreshes;
  for (std::uint32_t quantum = 0; quantum < 4u; ++quantum) {
    expect(engine.tryProcessBlock(channels, 2, EngineHost::kMaxProcessFrames,
                                  static_cast<double>(quantum) / 375.0, false),
           "active IR reverb latency polling remains idle");
  }
  expect(engine.processCounters().latencyRefreshes == activeAssetRefreshes,
         "persistent active asset cache does not keep latency polling alive");
  expect(engine.setAsset(asset, &error), "identical IR reverb asset replay: " + error);
  expect((engine.assetState(91, 0) & 0xffu) == ET_ASSET_STATE_ACTIVE,
         "identical IR reverb asset replay preserves the active convolution");

  runtime.contextuallyBypassed = true;
  expect(engine.rebuild(pipeline, {runtime}, &error) &&
             (engine.assetState(91, 0) & 0xffu) == ET_ASSET_STATE_NONE &&
             engine.pipelineLatency() == 0u && engine.setAsset(asset, &error),
         "contextual bypass keeps the original asset cached without addressing unity DSP");
  runtime.contextuallyBypassed = false;
  expect(engine.rebuild(pipeline, {runtime}, &error) &&
             (engine.assetState(91, 0) & 0xffu) == ET_ASSET_STATE_PREPARING,
         "re-admission stages the cached asset on the restored original runtime");
  prepareAsset(engine, channels, "re-admitted IR reverb asset becomes active");

  engine.reset();
  float wetPeak = 0.0f;
  for (std::uint32_t quantum = 0; quantum < 4u; ++quantum) {
    left.fill(0.0f);
    right.fill(0.0f);
    if (quantum == 0u) {
      left[0] = 1.0f;
    }
    expect(engine.tryProcessBlock(channels, 2, EngineHost::kMaxProcessFrames,
                                  static_cast<double>(quantum) / 375.0, false),
           "preserved IR reverb process");
    wetPeak = std::max(wetPeak, *std::max_element(left.begin(), left.end()));
  }
  expect(wetPeak > 0.9f, "identical IR reverb asset replay preserves wet output");

  expect(engine.rebuild(pipeline, {runtime}, &error), "IR reverb cached asset rebuild: " + error);
  expect((engine.assetState(91, 0) & 0xffu) == ET_ASSET_STATE_PREPARING,
         "cached IR reverb asset is replayed after a topology rebuild");
  prepareAsset(engine, channels, "replayed IR reverb asset becomes active");
  const auto activePlanRevision = engine.pipelinePlanRevision();
  expect(engine.clearAsset(91, 0), "IR reverb asset clear");
  expect((engine.assetState(91, 0) & 0xffu) == ET_ASSET_STATE_NONE,
         "IR reverb asset clear resets native state");
  expect(engine.pipelineLatency() == 0u &&
             engine.pipelinePlanRevision() > activePlanRevision,
         "cleared IR reverb removes latency and requests compensation refresh");
}

void testRuntimeLatencyAndTelemetryPublication() {
  EngineHost engine;
  std::string error;
  expect(engine.prepare(48000.0, 2, EngineHost::kDefaultTelemetryBytes, &error),
         "variable-latency engine prepare: " + error);

  const auto kernel = engine.kernels().find("BrickwallLimiterPlugin");
  expect(kernel != engine.kernels().end(), "brickwall limiter kernel registration");
  PipelineState pipeline;
  pipeline.plugins = {PluginState{17, "BrickwallLimiterPlugin", true}};
  RuntimePlugin runtime;
  runtime.logicalId = 17;
  runtime.type = "BrickwallLimiterPlugin";
  runtime.packedParameters = {0.0f, 100.0f, 3.0f, 1.0f, 0.0f, -1.0f};
  runtime.paramsHash = kernel->second.paramsHash;
  expect(engine.rebuild(pipeline, {runtime}, &error),
         "variable-latency engine rebuild: " + error);
  expect(engine.pipelineLatency() == 144u, "initial limiter lookahead latency");

  std::array<float, EngineHost::kMaxProcessFrames> left{};
  std::array<float, EngineHost::kMaxProcessFrames> right{};
  float *channels[] = {left.data(), right.data()};
  std::vector<std::uint8_t> telemetry(EngineHost::kDefaultTelemetryBytes);
  std::uint32_t droppedFrames = 0;
  std::uint32_t telemetryBytes = 0;
  for (std::uint32_t quantum = 0; quantum < 8 && telemetryBytes == 0; ++quantum) {
    expect(engine.tryProcessBlock(channels, 2, EngineHost::kMaxProcessFrames,
                                  static_cast<double>(quantum) / 375.0, false),
           "limiter telemetry process");
    telemetryBytes = engine.readTelemetry(telemetry, droppedFrames);
  }
  expect(telemetryBytes > 0,
         "audio-thread telemetry publication must expose a non-empty packet");
  const auto initialSequence = static_cast<std::uint32_t>(telemetry[8]) |
                               (static_cast<std::uint32_t>(telemetry[9]) << 8u) |
                               (static_cast<std::uint32_t>(telemetry[10]) << 16u) |
                               (static_cast<std::uint32_t>(telemetry[11]) << 24u);
  for (std::uint32_t quantum = 8; quantum < 32; ++quantum) {
    expect(engine.tryProcessBlock(channels, 2, EngineHost::kMaxProcessFrames,
                                  static_cast<double>(quantum) / 375.0, false),
           "queued telemetry process");
  }
  telemetryBytes = engine.readTelemetry(telemetry, droppedFrames);
  const auto latestSequence = static_cast<std::uint32_t>(telemetry[8]) |
                              (static_cast<std::uint32_t>(telemetry[9]) << 8u) |
                              (static_cast<std::uint32_t>(telemetry[10]) << 16u) |
                              (static_cast<std::uint32_t>(telemetry[11]) << 24u);
  expect(telemetryBytes > 0 && latestSequence > initialSequence,
         "telemetry consumer returns the latest queued frame");
  expect(droppedFrames > 0, "skipped telemetry frame count is carried to the delivery");

  for (std::uint32_t quantum = 32; quantum < 48; ++quantum) {
    expect(engine.tryProcessBlock(channels, 2, EngineHost::kMaxProcessFrames,
                                  static_cast<double>(quantum) / 375.0, false),
           "hidden telemetry process");
  }
  engine.discardTelemetry();
  for (std::uint32_t quantum = 48; quantum < 56; ++quantum) {
    expect(engine.tryProcessBlock(channels, 2, EngineHost::kMaxProcessFrames,
                                  static_cast<double>(quantum) / 375.0, false),
           "resumed telemetry process");
  }
  telemetryBytes = engine.readTelemetry(telemetry, droppedFrames);
  expect(telemetryBytes > 0 && droppedFrames > 0,
         "hidden telemetry discard count is carried to the next fresh packet");

  AudioCommand command;
  command.type = AudioCommandType::setParameters;
  command.logicalId = 17;
  command.paramsHash = runtime.paramsHash;
  command.floatCount = 6;
  constexpr std::array updatedParameters{0.0f, 100.0f, 10.0f, 1.0f, 0.0f, -1.0f};
  std::copy(updatedParameters.begin(), updatedParameters.end(), command.packed.begin());
  AudioCommandQueue queue;
  expect(queue.push(command), "enqueue variable-latency parameter update");
  const auto previousRevision = engine.latencyRevision();
  expect(engine.tryProcessBlock(channels, 2, EngineHost::kMaxProcessFrames, 1.0, false, &queue),
         "apply variable-latency parameter update");
  expect(engine.pipelineLatency() == 480u, "updated limiter lookahead latency");
  expect(engine.latencyRevision() > previousRevision,
         "audio-thread latency update must publish a new revision");

  pipeline.plugins[0].enabled = false;
  expect(engine.makeDescriptorCommand(pipeline, std::span<const RuntimePlugin>(&runtime, 1),
                                      command, &error),
         "limiter bypass descriptor command: " + error);
  const auto enabledRevision = engine.latencyRevision();
  std::uint64_t appliedDescriptorRevision = 0;
  expect(engine.applyDescriptorCommand(command, appliedDescriptorRevision, &error),
         "apply limiter bypass descriptor off the audio path: " + error);
  expect(engine.pipelineLatency() == 0u && engine.latencyRevision() > enabledRevision,
         "non-RT descriptor publishes bypassed pipeline latency");
}

void testMasterBypassLatencyAlignment() {
  EngineHost engine;
  std::string error;
  expect(engine.prepare(48000.0, 2, EngineHost::kDefaultTelemetryBytes, &error),
         "master-bypass alignment engine prepare: " + error);

  const auto kernel = engine.kernels().find("BrickwallLimiterPlugin");
  expect(kernel != engine.kernels().end(), "master-bypass limiter kernel registration");
  PipelineState pipeline;
  pipeline.plugins = {PluginState{70, "Section", true},
                      PluginState{71, "BrickwallLimiterPlugin", true}};
  RuntimePlugin runtime;
  runtime.logicalId = 71;
  runtime.type = "BrickwallLimiterPlugin";
  runtime.packedParameters = {0.0f, 100.0f, 10.0f, 1.0f, 0.0f, -1.0f};
  runtime.paramsHash = kernel->second.paramsHash;
  expect(engine.rebuild(pipeline, {runtime}, &error),
         "master-bypass alignment engine rebuild: " + error);
  expect(engine.pipelineLatency() == 480u, "master-bypass limiter latency");

  constexpr std::uint32_t blockFrames = EngineHost::kMaxProcessFrames;
  const auto reportedLatency = calculateTotalLatency(0, 1, engine.pipelineLatency());
  expect(reportedLatency == 480u, "master-bypass reported total latency");

  BlockAdapter adapter;
  adapter.prepare(2, blockFrames);
  DryDelayLine dryDelay;
  expect(dryDelay.prepare(2, blockFrames, reportedLatency),
         "master-bypass dry delay prepare");

  std::array<std::array<float, blockFrames>, 2> input{};
  std::array<std::array<float, blockFrames>, 2> processedBlock{};
  const float *inputPointers[] = {input[0].data(), input[1].data()};
  float *outputPointers[] = {processedBlock[0].data(), processedBlock[1].data()};
  std::vector<float> processed;
  std::vector<float> bypassed;
  processed.reserve(blockFrames * 10u);
  bypassed.reserve(blockFrames * 10u);
  std::uint64_t processedFrames = 0;
  for (std::uint32_t block = 0; block < 10; ++block) {
    input[0].fill(0.0f);
    input[1].fill(0.0f);
    if (block == 0) {
      input[0][0] = 0.5f;
    }
    expect(adapter.process(
               inputPointers, outputPointers, blockFrames,
               [&engine, &processedFrames](float *const *channels,
                                           const std::uint32_t channelCount,
                                           const std::uint32_t frames) {
                 const auto ok = engine.tryProcessBlock(
                     channels, channelCount, frames,
                     static_cast<double>(processedFrames) / 48000.0, false);
                 processedFrames += frames;
                 return ok;
               }),
           "master-bypass active impulse process");
    processed.insert(processed.end(), processedBlock[0].begin(), processedBlock[0].end());

    const auto *delayed = dryDelay.process(inputPointers, 2, blockFrames);
    expect(delayed != nullptr, "master-bypass delayed dry process");
    bypassed.insert(bypassed.end(), delayed[0], delayed[0] + blockFrames);
  }

  const auto peakIndex = [](const std::vector<float> &samples) {
    return static_cast<std::size_t>(std::distance(
        samples.begin(),
        std::max_element(samples.begin(), samples.end(), [](const float left, const float right) {
          return std::abs(left) < std::abs(right);
        })));
  };
  const auto activePeak = peakIndex(processed);
  const auto bypassPeak = peakIndex(bypassed);
  expect(std::abs(processed[reportedLatency]) > 1.0e-6f && activePeak == reportedLatency,
         "active limiter impulse peak matches reported latency (peak=" +
             std::to_string(activePeak) + ", sample128=" +
             std::to_string(processed[blockFrames]) + ", reported=" +
             std::to_string(processed[reportedLatency]) + ")");
  expect(std::abs(bypassed[reportedLatency] - 0.5f) < 1.0e-7f &&
             bypassPeak == reportedLatency,
         "master-bypass dry impulse peak matches reported latency (peak=" +
             std::to_string(bypassPeak) + ")");
}

} // namespace

int main() {
  effetune::vst::testing::suppressCrtModalDialogs();
  try {
    testDescriptor();
    testQueue();
    testOutputTransition();
    testDryDelayLine();
    testConfigStore();
    testBlockAdapter();
    testBlockSizeMatrix();
    testLatency();
    testStateCodec();
    testMessageRouter();
    testResampler();
    testEngineHost();
    testContextualBypassPreservesCrossBusRouting();
    testEngineAssetTransferAndReplay();
    testRuntimeLatencyAndTelemetryPublication();
    testMasterBypassLatencyAlignment();
    std::cout << "All EffeTune VST unit tests passed\n";
    return 0;
  } catch (const std::exception &exception) {
    std::cerr << "Test failure: " << exception.what() << '\n';
    return 1;
  }
}
