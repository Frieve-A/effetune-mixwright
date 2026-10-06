#include "bridge/backup_export.h"
#include "bridge/message_router.h"
#include "engine/engine_host.h"
#include "engine/output_analyzers.h"
#include "engine/frequency_preview.h"
#include "BassManagementPluginParams.h"
#include "OscilloscopePluginParams.h"
#include "PhaseSelectEqPluginParams.h"
#include "AnalogMeterPluginParams.h"
#include "RhythmAnalyzerPluginParams.h"
#include "TonalBalanceEQPluginParams.h"
#include "FiveBandPEQPluginParams.h"
#include "TubeSimulatorPluginParams.h"
#include "allocation_guard.h"
#include <choc/memory/choc_Base64.h>

#include <array>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <thread>
#include <unordered_set>

namespace {
using namespace effetune::vst;
void expect(bool value, const char *message) { if (!value) throw std::runtime_error(message); }

std::uint32_t u32(const std::uint8_t *data) {
  std::uint32_t result;
  std::memcpy(&result, data, sizeof(result));
  return result;
}
float f32(const std::uint8_t *data) {
  float result;
  std::memcpy(&result, data, sizeof(result));
  return result;
}

void testDeferredBassLatency() {
  auto engine = std::make_unique<EngineHost>();
  std::string error;
  expect(engine->prepare(48000, 2, 64, EngineHost::kDefaultTelemetryBytes, &error), "prepare bass");
  effetune::generated::BassManagementPluginParams params{};
  std::fill_n(params.frequencies, 16, 80.0f);
  std::fill_n(params.slopes, 16, 24.0f);
  params.lfeFrequency = 120;
  params.lfeSlope = 24;
  const auto packed = [&] { return std::span<const float>(reinterpret_cast<const float *>(&params), 89); };
  RuntimePlugin runtime;
  runtime.logicalId = 7;
  runtime.type = "BassManagementPlugin";
  runtime.paramsHash = params.kHash;
  runtime.packedParameters.assign(packed().begin(), packed().end());
  PipelineState pipeline;
  pipeline.plugins = {PluginState{7, "BassManagement", true}};
  expect(engine->rebuild(pipeline, {runtime}, &error), "rebuild bass");
  std::array<float, 64> left{}, right{};
  float *channels[]{left.data(), right.data()};
  const auto render = [&] {
    effetune::allocation_guard::Scope guard;
    return engine->tryProcessBlock(channels, 2, 64, 0, false);
  };
  for (int block = 0; block < 12; ++block) expect(render(), "warm bass");
  for (const auto setting : {std::pair{1.0f, 0.0f}, std::pair{1.0f, 1.0f}, std::pair{0.0f, 1.0f}}) {
    const auto previous = engine->pipelinePlanRevision();
    const auto oldLatency = engine->pipelineLatency();
    params.phase = setting.first;
    params.taps = setting.second;
    EngineHost::ProcessBatch batch;
    expect(engine->beginProcessBatch(batch), "begin deferred update");
    expect(batch.stageParameters(7, packed(), params.kHash), "stage deferred update");
    expect(batch.processChunk(channels, 2, 64, 0, false) && batch.finish(), "first transition block");
    expect(engine->pipelinePlanRevision() == previous, "latency remains old before transition midpoint");
    for (int block = 0; block < 16; ++block) expect(render(), "advance deferred latency");
    expect(engine->pipelinePlanRevision() > previous, "parameter-only transition requests a new plan without assets");
    expect(engine->pipelineLatency() == oldLatency, "reported latency changes only with applied plan");
    expect(engine->capturePipelineLatencyUpdate(), "capture deferred plan");
    std::uint32_t latency = 0;
    expect(engine->preparePipelineLatencyUpdate(latency), "prepare deferred plan");
    std::uint64_t revision = 0;
    expect(engine->applyPipelineLatencyUpdate(revision), "apply deferred plan");
    const auto expected = params.phase == 0 ? 0u : params.taps == 0 ? 4224u : 8320u;
    expect(latency == expected && engine->pipelineLatency() == expected, "new bass latency is applied");
    engine->discardPipelineLatencyUpdate();
  }
}

void testOutputAnalyzers() {
  OutputAnalyzers analyzers;
  std::string error;
  expect(analyzers.prepare(48000, 8, 1024, &error), "prepare output analyzers");
  const std::vector<OutputAnalyzerSource> sources{
      {0xf0000000u, "LevelMeterPlugin", "34", {}, 0x811c9dc5u, 2},
      {0xf0000001u, "SpectrumAnalyzerPlugin", "L", {-96, 10, 0}, 0x3e6e0819u, 1},
      {0xf0000002u, "SpectrogramPlugin", "R", {-96, 10, 0}, 0x3e6e0819u, 1},
      {0xf0000003u, "OscilloscopePlugin", "1", {.01f, 0, 0, 0, .0001f, 0, 0}, effetune::generated::OscilloscopePluginParams::kHash, 1},
      {0xf0000004u, "StereoMeterPlugin", "", {.1f}, 0xb0de3212u, 1},
      {0xf0000005u, "NoteSpectrogramPlugin", "", {60, 72, 2}, 0x0c9bdf4eu, 1},
      {0xf0000006u, "ChromaSpiralPlugin", "", {}, 0x811c9dc5u, 1},
      {0xf0000007u, "PhaseSelectEqPlugin", "", std::vector<float>(effetune::generated::PhaseSelectEqPluginParams::kFloatCount), effetune::generated::PhaseSelectEqPluginParams::kHash, 1},
      {0xf0000008u, "AnalogMeterPlugin", "", {0, .3f, 5, 1.5f}, effetune::generated::AnalogMeterPluginParams::kHash, 1},
      {0xf0000009u, "RhythmAnalyzerPlugin", "", {40, 240, 0}, effetune::generated::RhythmAnalyzerPluginParams::kHash, 1}};
  expect(analyzers.setSources(sources, &error), "all Visualizer analyzers configure");
  std::array<std::array<float, 1024>, 8> audio;
  std::array<float *, 8> pointers{};
  for (std::size_t channel = 0; channel < audio.size(); ++channel) {
    audio[channel].fill(static_cast<float>(channel + 1) * .05f);
    pointers[channel] = audio[channel].data();
  }
  std::vector<std::uint8_t> packet(OutputAnalyzers::kTelemetryBytes);
  std::unordered_set<std::uint32_t> seen;
  for (int block = 0; block < 100; ++block) {
    { effetune::allocation_guard::Scope guard;
      analyzers.process(pointers.data(), 8, 1024, 48000); }
    std::uint32_t dropped = 0;
    const auto bytes = analyzers.readTelemetry(packet, dropped);
    expect(dropped == 0, "ordinary telemetry polling does not drop output");
    for (std::uint32_t offset = 0; offset + 16 <= bytes;) {
      const auto tap = u32(packet.data() + offset + 4);
      seen.insert(tap);
      if (tap == sources[0].tapId) {
        expect(std::abs(f32(packet.data() + offset + 20) - .3f) < .00001f &&
               std::abs(f32(packet.data() + offset + 28) - .4f) < .00001f,
               "channel pair and gain apply only to captured analysis");
      }
      const auto payload = static_cast<std::uint32_t>(packet[offset + 12]) |
                           (static_cast<std::uint32_t>(packet[offset + 13]) << 8);
      offset += (16u + payload + 3u) & ~3u;
    }
  }
  expect(seen.size() == sources.size(), "every analyzer emits its reserved tap ID");
  for (std::size_t channel = 0; channel < audio.size(); ++channel)
    for (const auto value : audio[channel])
      expect(value == static_cast<float>(channel + 1) * .05f, "analyzers never modify host output");
  expect(analyzers.setSources({}, &error), "hide clears analyzers");
  analyzers.process(pointers.data(), 8, 1024, 48000);
  std::uint32_t dropped = 0;
  expect(analyzers.readTelemetry(packet, dropped) == 0, "hidden analyzer emits nothing");
  expect(analyzers.setSources({sources.front()}, &error), "resume analyzer");
  for (int block = 0; block < 300; ++block) analyzers.process(pointers.data(), 8, 1024, 48000);
  (void)analyzers.readTelemetry(packet, dropped);
  expect(dropped > 0, "bounded capture overflow is observable");
  expect(analyzers.prepare(96000, 1, 64, &error), "reprepare analyzer dimensions");
  analyzers.process(pointers.data(), 1, 64, 96000);
  (void)analyzers.readTelemetry(packet, dropped);
  analyzers.stopCapture();
  analyzers.discardTelemetry();
  expect(analyzers.prepare(48000, 8, 64, &error), "reprepare while editor is closed");
  for (int block = 0; block < 32; ++block) analyzers.process(pointers.data(), 8, 64, 48000);
  expect(analyzers.readTelemetry(packet, dropped) == 0, "lifecycle never resumes closed editor capture");
  std::vector<OutputAnalyzerSource> maximumSources;
  for (std::uint32_t index = 0; index < OutputAnalyzers::kMaxSources; ++index) {
    auto source = sources.front();
    source.tapId += index;
    maximumSources.push_back(std::move(source));
  }
  expect(analyzers.setSources(maximumSources, &error), "all 64 items plus two modulators fit independently");
  for (int block = 0; block < 32; ++block) analyzers.process(pointers.data(), 8, 64, 48000);
  expect(analyzers.readTelemetry(packet, dropped) > 0 && dropped == 0, "maximum source list processes");
  auto invalid = sources.front(); invalid.type = "VolumePlugin";
  expect(!analyzers.setSources({invalid}, &error), "reject effect as analyzer");
  expect(!analyzers.setSources({sources.front(), sources.front()}, &error), "reject duplicate taps");
  RoutedUiMessage message;
  expect(MessageRouter::decode(R"({"type":"visualizer/setSources","payload":{"sources":[{"tapId":4026531840,"type":"LevelMeterPlugin","params":[],"paramsHash":2166136261,"channel":null,"gain":1}]}})", message, &error), "decode upstream analyzer descriptor");
}

void testOutputAnalyzerClockFollowsConsumedAudio() {
  OutputAnalyzers analyzers;
  std::string error;
  double sampleRate = 48000;
  std::uint32_t channels = 2;
  const OutputAnalyzerSource left{0xf0000000u, "SpectrogramPlugin", "L", {-96, 10, 0}, 0x3e6e0819u, 1};
  const OutputAnalyzerSource right{0xf0000001u, "SpectrogramPlugin", "R", {-96, 10, 0}, 0x3e6e0819u, 1};
  const OutputAnalyzerSource level{0xf0000002u, "LevelMeterPlugin", "", {}, 0x811c9dc5u, 1};
  expect(analyzers.prepare(sampleRate, channels, 512, &error) &&
             analyzers.setSources({left, right}, &error), "prepare common analyzer clock");
  std::array<float, 512> audio{};
  float *pointers[]{audio.data(), audio.data()};
  std::vector<std::uint8_t> packet(OutputAnalyzers::kTelemetryBytes);
  std::array<std::vector<float>, 2> timestamps;
  std::uint64_t consumedFrames = 0;
  const auto render = [&] {
    const auto previousColumns = timestamps[0].size();
    for (int block = 0; block < 32; ++block) {
      // Unequal host blocks also cross the capture queue's chunk boundary.
      for (const auto frames : {17u, 95u, 400u}) {
        analyzers.process(pointers, channels, frames, sampleRate);
        consumedFrames += frames;
      }
      std::uint32_t dropped = 0;
      const auto bytes = analyzers.readTelemetry(packet, dropped);
      expect(dropped == 0, "clock fixture consumes every matching PCM block");
      for (std::uint32_t offset = 0; offset + 16 <= bytes;) {
        const auto tap = u32(packet.data() + offset + 4);
        const auto payload = static_cast<std::uint32_t>(packet[offset + 12]) |
                             (static_cast<std::uint32_t>(packet[offset + 13]) << 8);
        expect(offset + 16u + payload <= bytes, "complete timestamped analyzer frame");
        if (tap == left.tapId || tap == right.tapId) {
          expect(packet[offset] == 5 && packet[offset + 2] == 1 && payload >= 12,
                 "normal spectrogram columns carry sample timestamps");
          expect(f32(packet.data() + offset + 16) == static_cast<float>(sampleRate),
                 "column rate matches the prepared engine");
          const auto time = f32(packet.data() + offset + 20);
          auto &sourceTimes = timestamps[tap - left.tapId];
          expect(time >= 0 && time <= static_cast<double>(consumedFrames) / sampleRate + .000001,
                 "column time never advances past the PCM actually consumed");
          expect(sourceTimes.empty() || time > sourceTimes.back(),
                 "retained analyzer timestamps stay strictly increasing");
          sourceTimes.push_back(time);
        }
        offset += (16u + payload + 3u) & ~3u;
      }
    }
    expect(timestamps[0].size() > previousColumns && timestamps[0] == timestamps[1],
           "every analyzer observes the same block sample clock");
  };
  render();
  expect(analyzers.prepare(sampleRate, channels, 1024, &error) &&
             analyzers.setSources({left, right, level}, &error),
         "retain sources through capacity change and partial source edit");
  render();
  for (int block = 0; block < 16; ++block) analyzers.process(pointers, channels, 512, sampleRate);
  analyzers.discardTelemetry();
  analyzers.process(pointers, 1, 512, 96000);
  std::uint32_t dropped = 0;
  expect(analyzers.readTelemetry(packet, dropped) == 0, "discarded and mismatched PCM produces no columns");
  analyzers.stopCapture();
  for (int block = 0; block < 16; ++block) analyzers.process(pointers, channels, 512, sampleRate);
  expect(analyzers.setSources({left, right}, &error), "resume retained sources after discard");
  render();
  sampleRate = 96000;
  channels = 1;
  expect(analyzers.prepare(sampleRate, channels, 512, &error), "recreate analyzers for new rate and layout");
  timestamps = {};
  consumedFrames = 0;
  render();
  expect(analyzers.setSources({}, &error) && analyzers.setSources({left, right}, &error),
         "recreate cleared analyzer state");
  timestamps = {};
  consumedFrames = 0;
  render();
}

void testInstanceResetAndPreviewMeasurementGate() {
  using Params = effetune::generated::TonalBalanceEQPluginParams;
  auto engine = std::make_unique<EngineHost>();
  auto commands = std::make_unique<AudioCommandQueue>();
  std::string error;
  expect(engine->prepare(48000, 2, 1024, EngineHost::kDefaultTelemetryBytes, &error), "prepare measurement reset");
  Params params{};
  params.range = 6;
  params.smoothing = .5f;
  params.averagingTime = 30;
  params.low = 20;
  params.high = 16000;
  params.averageSpl = 83;
  params.tiltSlope = -6;
  params.tiltCorner = 250;
  std::fill_n(params.adjustFrequency, 5, 1000.0f);
  std::fill_n(params.adjustQ, 5, .7f);
  const auto packed = [&] { return std::span<const float>(reinterpret_cast<const float *>(&params), Params::kFloatCount); };
  RuntimePlugin tonal;
  tonal.logicalId = 41;
  tonal.type = "TonalBalanceEQPlugin";
  tonal.paramsHash = Params::kHash;
  tonal.packedParameters.assign(packed().begin(), packed().end());
  RuntimePlugin meter;
  meter.logicalId = 42;
  meter.type = "AnalogMeterPlugin";
  meter.paramsHash = effetune::generated::AnalogMeterPluginParams::kHash;
  meter.packedParameters = {3, .3f, 5, 1.5f};
  PipelineState pipeline;
  pipeline.plugins = {PluginState{41, "TonalBalanceEQPlugin", true}, PluginState{42, "AnalogMeterPlugin", true}};
  expect(engine->rebuild(pipeline, {tonal, meter}, &error), "build measurement reset pipeline");
  std::array<float, 1024> left{}, right{};
  float *channels[]{left.data(), right.data()};
  std::vector<std::uint8_t> telemetry(EngineHost::kDefaultTelemetryBytes);
  std::uint32_t hops = 0, meterSequence = 0, style = 0;
  std::uint64_t frames = 0;
  const auto render = [&](int blocks, bool stageImages = false) {
    for (int block = 0; block < blocks; ++block) {
      for (std::size_t frame = 0; frame < left.size(); ++frame)
        left[frame] = right[frame] = .25f * static_cast<float>(std::sin(6.283185307179586 * 1000 * static_cast<double>(frames + frame) / 48000));
      if (stageImages && block % 3 == 0)
        expect(engine->updateParameters(41, packed(), Params::kHash), "control parameter image preserves measurement gate");
      bool processed = false;
      {
        effetune::allocation_guard::Scope guard;
        EngineHost::ProcessBatch batch;
        processed = engine->beginProcessBatch(batch, commands.get());
        if (processed && stageImages)
          processed = batch.stageParameters(41, packed(), Params::kHash);
        processed = processed && batch.processChunk(channels, 2, 1024, static_cast<double>(frames) / 48000, false);
        processed = batch.finish() && processed;
      }
      expect(processed, "reset and preview overlay allocate nothing on audio");
      frames += 1024;
      std::uint32_t dropped = 0;
      const auto bytes = engine->readTelemetry(telemetry, dropped);
      expect(dropped == 0, "measurement telemetry is complete");
      for (std::uint32_t offset = 0; offset + 16 <= bytes;) {
        const auto tap = u32(telemetry.data() + offset + 4);
        const auto payload = static_cast<std::uint32_t>(telemetry[offset + 12]) |
                             (static_cast<std::uint32_t>(telemetry[offset + 13]) << 8);
        if (tap == 41 && payload >= 24) {
          hops = u32(telemetry.data() + offset + 36);
          style = telemetry[offset + 25];
        } else if (tap == 42) {
          const auto sequence = u32(telemetry.data() + offset + 8);
          expect(sequence >= meterSequence, "reset preserves unrelated instance telemetry sequence");
          meterSequence = sequence;
          expect(telemetry[offset + 16] == 3, "reset preserves unrelated parameters");
        }
        offset += (16u + payload + 3u) & ~3u;
      }
    }
  };
  render(150);
  expect(hops > 0, "ordinary input accumulates tonal measurements");
  engine->setFrequencyPreviewActive(true);
  render(16);
  const auto frozen = hops;
  params.target = 1;
  AudioCommand image;
  image.type = AudioCommandType::setParameters;
  image.logicalId = 41;
  image.paramsHash = Params::kHash;
  image.floatCount = Params::kFloatCount;
  std::copy(packed().begin(), packed().end(), image.packed.begin());
  expect(commands->push(image), "queue image during preview");
  render(64, true);
  expect(hops == frozen && style == 1, "preview freezes measurement across queued, control and automation images");
  AudioCommand reset;
  reset.type = AudioCommandType::resetInstance;
  reset.logicalId = 41;
  reset.paramsHash = Params::kHash;
  expect(commands->push(reset), "queue instance-only temporal reset");
  const auto meterBefore = meterSequence;
  render(64);
  expect(hops == 0 && style == 1 && meterSequence > meterBefore,
         "target history clears while its settings and other instances survive");
  engine->setFrequencyPreviewActive(false);
  render(100);
  expect(hops > 0 && style == 1, "measurement resumes on preview release without another parameter image");
  params.measurementPaused = 1;
  expect(engine->updateParameters(41, packed(), Params::kHash), "accept a stale UI transient field");
  const auto resumed = hops;
  render(64);
  expect(hops > resumed, "host-owned gate overrides stale serialized or UI pause values");
  FrequencyPreview preview;
  expect(!preview.mix(channels, 2, 1024, 48000), "silent preview block reports no gate");
  preview.setFrequency(1000);
  expect(preview.mix(channels, 2, 1024, 48000), "active preview block reports gate");
  preview.setFrequency(0);
  expect(preview.mix(channels, 2, 1024, 48000), "release-ramp block keeps gate closed");
  expect(!preview.mix(channels, 2, 1024, 48000), "following clean block reopens gate");
}

void testSpectrumCapture() {
  auto engine = std::make_unique<EngineHost>();
  std::string error;
  auto &capture = engine->spectrumCapture();
#ifdef EFFETUNE_HAS_NATIVE_SPECTRUM_TAP
  expect(capture.nativeAnalysisSupported(), "native analysis capability is available before preparation");
  for (const auto quality : {SpectrumQuality::normal, SpectrumQuality::hq})
    expect(!capture.setTap(1, SpectrumMode::after, quality), "unprepared native capture rejects active subscriptions");
  std::uint32_t initialDropped = 0;
  expect(!capture.capturing() && capture.read(initialDropped).empty() && initialDropped == 0,
         "native capability does not enable capture before preparation");
#else
  expect(!capture.nativeAnalysisSupported(), "legacy capture does not advertise native analysis before preparation");
#endif
  expect(engine->prepare(48000, 8, 257, EngineHost::kDefaultTelemetryBytes, &error), "prepare spectrum capture");
  const auto gainHash = engine->kernels().at("TestGainPlugin").paramsHash;
  const auto gain = [&](std::uint32_t id, float value) {
    RuntimePlugin runtime;
    runtime.logicalId = id;
    runtime.type = "TestGainPlugin";
    runtime.paramsHash = gainHash;
    runtime.packedParameters = {value};
    return runtime;
  };
  PipelineState pipeline;
  pipeline.plugins = {PluginState{1, "Gain", true}, PluginState{2, "Gain", true},
                      PluginState{3, "Gain", true}};
  pipeline.plugins[0].channel = "A";
  pipeline.plugins[1].outputBus = 4;
  pipeline.plugins[1].channel = "34";
  pipeline.plugins[2].inputBus = 4;
  pipeline.plugins[2].channel = "A";
  expect(engine->rebuild(pipeline, {gain(1, 2), gain(2, 3), gain(3, 4)}, &error), "build routed spectrum fixture");
  expect(capture.setTap(1, SpectrumMode::compare) && capture.setTap(2, SpectrumMode::compare) &&
         capture.setTap(3, SpectrumMode::after), "subscribe multiple node spectra");
  std::array<std::array<float, 257>, 8> audio{};
  std::array<float *, 8> channels{};
  for (std::size_t channel = 0; channel < channels.size(); ++channel) channels[channel] = audio[channel].data();
  std::uint64_t clock = 0;
  const auto render = [&](bool bypass = false) {
    const auto frames = clock % 2 ? 127u : 257u;
    for (std::size_t channel = 0; channel < channels.size(); ++channel)
      audio[channel].fill(static_cast<float>(channel + 1));
    bool success;
    { effetune::allocation_guard::Scope guard;
      success = engine->tryProcessBlock(channels.data(), 8, frames, 0, bypass); }
    expect(success, "spectrum capture preserves allocation-free variable block processing");
    clock += frames;
  };
  for (int block = 0; block < 32; ++block) render();
  std::uint32_t dropped = 0;
  auto frames = capture.read(dropped);
  expect(frames.size() == 3 && dropped == 0, "all selected nodes produce normal spectra");
  for (const auto &frame : frames) {
#ifdef EFFETUNE_HAS_NATIVE_SPECTRUM_TAP
    const auto &analysis = frame.analysis;
    const float input = frame.pluginId == 1 ? 4.5f : 7.0f;
    const float output = frame.pluginId == 1 ? 9.0f : 21.0f;
    expect(std::abs(analysis.output.current[0] - 20 * std::log10(output)) < 0.001f,
           "native spectrum observes the routed node output before bus mixing");
    if (frame.mode == SpectrumMode::compare)
      expect(std::abs(analysis.input.current[0] - 20 * std::log10(input)) < 0.001f,
             "native Compare observes the routed input channel mean");
    expect(analysis.timing.captureEndFrame <= clock && analysis.timing.generation > 0 &&
           analysis.timing.windowAgeFrames == 2048 && analysis.timing.tapDelayFrames == 0,
           "native Normal publishes the capture timeline and analysis window contract");
#else
    const auto index = (frame.bufferPosition + SpectrumFrame::kSamples - 1) % SpectrumFrame::kSamples;
    if (frame.pluginId == 1) expect(frame.input[index] == 4.5f && frame.output[index] == 9,
                                  "all-channel tap observes its own effect, not the final output");
    if (frame.pluginId == 2) expect(frame.input[index] == 7 && frame.output[index] == 21,
                                  "selected channel pair tap observes the send before destination mixing");
    if (frame.pluginId == 3) expect(frame.output[index] == 21,
                                  "return tap observes the routed bus before merging main output");
#endif
  }
  expect(audio[2][0] == 78 && audio[0][0] == 2 && engine->pipelineLatency() == 0,
         "capture does not alter audio, buses or latency");
  render();
  expect(capture.setTap(1, SpectrumMode::off) && capture.setTap(2, SpectrumMode::off) &&
         capture.setTap(3, SpectrumMode::off), "disable spectra");
  expect(capture.read(dropped).empty(), "disabled taps discard queued old frames");

  RuntimePlugin delay;
  delay.logicalId = 5;
  delay.type = "TestDelayPlugin";
  delay.paramsHash = engine->kernels().at(delay.type).paramsHash;
  pipeline.plugins = {PluginState{5, "Delay", true}};
  expect(engine->rebuild(pipeline, {delay}, &error), "build intrinsic latency fixture");
  expect(capture.setTap(5, SpectrumMode::compare), "compare delayed effect");
  for (int block = 0; block < 32; ++block) render();
  frames = capture.read(dropped);
  expect(frames.size() == 1 && engine->pipelineLatency() == 192 &&
#ifdef EFFETUNE_HAS_NATIVE_SPECTRUM_TAP
         frames[0].analysis.input.current == frames[0].analysis.output.current,
#else
         frames[0].input == frames[0].output,
#endif
         "Compare aligns input by intrinsic effect latency without adding host latency");
#ifdef EFFETUNE_HAS_NATIVE_SPECTRUM_TAP
  pipeline.plugins.insert(pipeline.plugins.begin(), PluginState{1, "Gain", true});
  expect(engine->rebuild(pipeline, {gain(1, 2), delay}, &error), "build taps before and after intrinsic delay");
  expect(capture.setTap(1, SpectrumMode::after), "observe early pipeline tap");
  for (int block = 0; block < 32; ++block) render();
  frames = capture.read(dropped);
  expect(frames.size() == 2, "both delay-path taps produce a spectrum");
  for (const auto &frame : frames)
    expect(frame.analysis.timing.tapDelayFrames == (frame.pluginId == 1 ? 192u : 0u),
           "bridge receives each tap's remaining delay rather than total pipeline latency");
  expect(capture.setTap(1, SpectrumMode::off), "disable early delay-path tap");
  pipeline.plugins.erase(pipeline.plugins.begin());
  expect(engine->rebuild(pipeline, {delay}, &error), "restore isolated delay fixture");
#endif
  render();
  render(true);
  expect(capture.read(dropped).empty(), "master bypass invalidates queued spectra");
  for (int block = 0; block < 32; ++block) render();
  expect(!capture.read(dropped).empty(), "spectra resume after bypass");
  render();
  engine->reset();
  expect(capture.read(dropped).empty(), "reset clears previous timeline spectra");
  for (int block = 0; block < 32; ++block) render();
  pipeline.plugins[0].enabled = false;
  expect(engine->updateDescriptor(pipeline, &error), "disable node descriptor");
  expect(capture.read(dropped).empty(), "descriptor changes invalidate old spectra");
  for (int block = 0; block < 32; ++block) render();
  expect(capture.read(dropped).empty(), "disabled nodes are not captured");
  pipeline.plugins[0].enabled = true;
  pipeline.plugins.insert(pipeline.plugins.begin(), PluginState{9, "Section", false});
  expect(engine->updateDescriptor(pipeline, &error), "disable containing section");
  for (int block = 0; block < 32; ++block) render();
  expect(capture.read(dropped).empty(), "disabled sections are not captured");

  using Params = effetune::generated::FiveBandPEQPluginParams;
  Params params{};
  std::fill_n(params.frequency, 5, 1000.0f);
  std::fill_n(params.q, 5, 1.0f);
  RuntimePlugin equalizer;
  equalizer.logicalId = 7;
  equalizer.type = "FiveBandPEQPlugin";
  equalizer.paramsHash = Params::kHash;
  const auto *packed = reinterpret_cast<const float *>(&params);
  equalizer.packedParameters.assign(packed, packed + Params::kFloatCount);
  pipeline.plugins = {PluginState{7, "5 Band PEQ", true}};
  expect(engine->rebuild(pipeline, {equalizer}, &error), "build real 5 Band PEQ");
  capture.stopCapture();
  expect(capture.setTap(7, SpectrumMode::compare), "enable real EQ spectrum");
  for (int block = 0; block < 32; ++block) render();
  frames = capture.read(dropped);
  expect(frames.size() == 1 && frames[0].pluginId == 7 &&
#ifdef EFFETUNE_HAS_NATIVE_SPECTRUM_TAP
         frames[0].analysis.input.current == frames[0].analysis.output.current,
#else
         frames[0].input == frames[0].output,
#endif
         "normal spectrum and Compare work for actual FiveBandPEQ");
#ifdef EFFETUNE_HAS_NATIVE_SPECTRUM_TAP
  const auto normalGeneration = frames[0].analysis.timing.generation;
  for (const auto mode : {SpectrumMode::after, SpectrumMode::compare}) {
    expect(capture.setTap(7, mode, SpectrumQuality::hq), "subscribe native HQ on actual EQ");
    expect(capture.read(dropped).empty(), "quality/mode edits discard old analyses");
    bool published = false;
    for (int poll = 0; poll < 8; ++poll) {
      for (int block = 0; block < 24; ++block) render();
      frames = capture.read(dropped);
      if (frames.empty()) continue;
      const auto &hq = frames[0].analysis;
      expect(frames.size() == 1 && dropped == 0 && hq.quality == effetune::SpectrumTapQuality::HQ &&
             hq.mode == static_cast<effetune::SpectrumTapMode>(mode) && hq.timing.generation > normalGeneration &&
             hq.timing.windowAgeFrames == 8192 && hq.timing.completionFrames > 0 &&
             hq.output.validCellCount > 0 && hq.output.validCellCount <= 2048,
             "HQ After/Compare publish native spectra and the shared timing profile");
      if (mode == SpectrumMode::compare)
        expect(hq.input.current == hq.output.current, "HQ Compare retains aligned input/output for flat EQ");
      published = true;
    }
    expect(published, "actual HQ analysis reaches the polling consumer");
  }
  expect(capture.setTap(7, SpectrumMode::compare), "restore Normal after HQ");
#endif
  for (int block = 0; block < 2200; ++block) render();
  expect(capture.read(dropped).empty() && dropped > 0, "queue saturation is bounded, observable and drops stale display data");
  for (int block = 0; block < 32; ++block) render();
  expect(!capture.read(dropped).empty() && dropped == 0, "capture recovers after display backpressure");
  capture.stopCapture();
  for (int block = 0; block < 32; ++block) render();
  expect(capture.read(dropped).empty(), "editor closure stops capture");
#ifdef EFFETUNE_HAS_NATIVE_SPECTRUM_TAP
  expect(capture.setTap(1, SpectrumMode::after), "retain an active tap across preparation changes");
  expect(engine->prepare(192000 * 8, 8, 257, EngineHost::kDefaultTelemetryBytes, &error),
         "a Spectrum Tap rate limit cannot prevent oversampled engine preparation");
  pipeline.plugins = {PluginState{1, "Gain", true}};
  expect(engine->rebuild(pipeline, {gain(1, 2)}, &error), "build high-rate audio independently of display support");
  expect(capture.nativeAnalysisSupported(), "unsupported capture rates do not change native API capability");
  for (const auto quality : {SpectrumQuality::normal, SpectrumQuality::hq})
    expect(!capture.setTap(1, SpectrumMode::after, quality), "unsupported capture rate refuses the display subscription explicitly");
  for (int block = 0; block < 32; ++block) render();
  expect(capture.read(dropped).empty() && dropped == 0,
         "unsupported-rate preparation prevents an existing tap from publishing analysis");
  expect(audio[0][0] == 2 && engine->pipelineLatency() == 0,
         "unavailable display analysis preserves high-rate processed audio and latency");
  expect(engine->prepare(48000, 8, 257, EngineHost::kDefaultTelemetryBytes, &error),
         "restore a valid native capture rate");
  expect(engine->rebuild(pipeline, {gain(1, 2)}, &error), "restore audio after unsupported capture preparation");
  for (const auto quality : {SpectrumQuality::normal, SpectrumQuality::hq}) {
    expect(capture.nativeAnalysisSupported() && capture.setTap(1, SpectrumMode::after, quality),
           "valid preparation admits Normal and HQ capture again");
    bool published = false;
    for (int poll = 0; poll < 8; ++poll) {
      for (int block = 0; block < 24; ++block) render();
      frames = capture.read(dropped);
      expect(dropped == 0, "restored capture stays within the bounded polling queue");
      if (frames.empty()) continue;
      expect(frames.size() == 1 &&
             frames[0].analysis.quality == static_cast<effetune::SpectrumTapQuality>(quality),
             "restored analysis retains the requested quality");
      published = true;
    }
    expect(published, "Normal and HQ analysis publish after valid preparation is restored");
  }
#endif
}

void testCircuitFaultPublication() {
  auto engine = std::make_unique<EngineHost>();
  std::string error;
  expect(engine->prepare(48000, 2, 128, EngineHost::kDefaultTelemetryBytes, &error), "prepare Tube fault publication");
  RuntimePlugin tube;
  tube.logicalId = 17;
  tube.type = "TubeSimulatorPlugin";
  tube.paramsHash = effetune::generated::TubeSimulatorPluginParams::kHash;
  tube.packedParameters = {-30, 2, 0, 250, 10, 10, 39, 100, 2.828f, 0, 0, 0,
                           320, 270, 0, 2, 1, 8, 0, 1, 0, 400, 1000, 1};
  PipelineState pipeline;
  pipeline.plugins = {PluginState{17, "Tube Simulator", true}};
  expect(engine->rebuild(pipeline, {tube}, &error), "build actual Tube fault fixture");
  auto events = engine->circuitFaults();
  expect(events.size() == 1 && events[0].pluginId == 17 && !events[0].latched && events[0].cause == 0,
         "new Tube instance publishes clear initial state before audio starts");
  const auto firstEpoch = events[0].instanceEpoch;
  std::array<float, 128> left{}, right{};
  float *channels[]{left.data(), right.data()};
  const auto render = [&] {
    bool succeeded;
    { effetune::allocation_guard::Scope guard;
      succeeded = engine->tryProcessBlock(channels, 2, 128, 0, false); }
    expect(succeeded, "Tube runtime state publication remains allocation-free");
  };
  render();
  // This is the production safety-fault trigger used by the upstream native
  // fixture, not an injected host projection or a replacement test kernel.
  left.fill(std::numeric_limits<float>::max());
  right.fill(std::numeric_limits<float>::max());
  render();
  left.fill(0);
  right.fill(0);
  left[0] = std::numeric_limits<float>::quiet_NaN();
  render();
  events = engine->circuitFaults();
  expect(events.size() == 1 && events[0].latched && events[0].cause == 2 && events[0].generation > 0 &&
         events[0].instanceEpoch == firstEpoch, "actual circuit safety latch reaches the control snapshot");
  const auto faultGeneration = events[0].generation;
  expect(engine->circuitFaults()[0].generation == faultGeneration,
         "repeated polls retain generation for renderer deduplication");
  engine->reset();
  expect(engine->circuitFaults()[0].latched, "host reset preserves the kernel's latched safety fault");
  expect(engine->rebuild(pipeline, {tube}, &error), "recreate the Tube circuit");
  events = engine->circuitFaults();
  expect(events.size() == 1 && events[0].instanceEpoch > firstEpoch && !events[0].latched && events[0].generation == 0,
         "recreation clears the latch with a new incarnation despite generation returning to zero");
  PipelineState empty;
  expect(engine->updateDescriptor(empty, &error), "remove Tube from the active descriptor");
  expect(engine->circuitFaults().empty(), "retained inactive instances cannot publish stale circuit faults");
  expect(engine->rebuild(empty, {}, &error), "rebuild empty active pipeline");
  expect(engine->circuitFaults().empty(), "A/B replacement removes old runtime-event identities");
}

void testSpectrumRouter() {
  RoutedUiMessage message;
  std::string error;
  expect(MessageRouter::decode(R"({"type":"spectrum/setTap","payload":{"pluginId":7,"enabled":true,"mode":"compare"}})", message, &error) &&
         message.action == UiAction::setSpectrumTap && message.pluginId == 7 && message.spectrumMode == SpectrumMode::compare,
         "route a Compare subscription");
  expect(message.spectrumQuality == SpectrumQuality::normal, "older spectrum requests default to Normal");
  expect(MessageRouter::decode(R"({"type":"spectrum/setTap","payload":{"pluginId":7,"enabled":true,"mode":"after","quality":"hq"}})", message, &error) &&
         message.spectrumQuality == SpectrumQuality::hq, "route HQ quality independently of mode");
  for (const auto *request : {
      R"({"type":"spectrum/setTap","payload":{"pluginId":0,"enabled":true}})",
      R"({"type":"spectrum/setTap","payload":{"pluginId":1.5,"enabled":true}})",
      R"({"type":"spectrum/setTap","payload":{"pluginId":7,"enabled":"yes"}})",
      R"({"type":"spectrum/setTap","payload":{"pluginId":7,"enabled":true,"quality":1}})",
      R"({"type":"spectrum/setTap","payload":{"pluginId":7,"enabled":true,"quality":"ultra"}})",
      R"({"type":"spectrum/setTap","payload":{"pluginId":7,"enabled":true,"mode":"hq"}})"})
    expect(!MessageRouter::decode(request, message, &error), "reject malformed spectrum subscription");
}

void testBackupExport() {
  const auto directory = std::filesystem::temp_directory_path() /
      ("effetune-backup-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directory(directory);
  const auto destination = directory / "backup.zip";
  { std::ofstream file(destination, std::ios::binary); file << "original"; }
  const auto contents = [&] {
    std::ifstream file(destination, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file), {});
  };
  bridge::BackupExport writer;
  std::string error;
  expect(writer.begin(destination, 4, &error), "begin backup");
  expect(!writer.append(1, "AAECAw==", &error), "reject wrong offset");
  expect(contents() == "original", "wrong offset preserves destination");
  expect(writer.begin(destination, 4, &error) && writer.append(0, "AAE=", &error), "stage partial backup");
  expect(!writer.commit(&error) && contents() == "original", "truncated backup preserves destination");
  expect(writer.begin(destination, 4, &error), "begin cancelled backup");
  writer.cancel();
  expect(contents() == "original", "cancel preserves destination");
  expect(!writer.begin(destination, bridge::BackupExport::maximumBytes + 1, &error), "reject oversize backup");
  expect(writer.begin(destination, 4, &error) && writer.append(0, "AAECAw==", &error) && writer.commit(&error), "commit binary backup");
  expect(contents() == std::string("\0\1\2\3", 4), "binary backup including NUL is exact");
  expect(std::distance(std::filesystem::directory_iterator(directory), std::filesystem::directory_iterator{}) == 1,
         "staging files are cleaned up");
  std::filesystem::remove(destination);
  std::filesystem::remove(directory);
}
} // namespace

int main() {
  try {
    testDeferredBassLatency();
    testOutputAnalyzers();
    testOutputAnalyzerClockFollowsConsumedAudio();
    testInstanceResetAndPreviewMeasurementGate();
    testSpectrumCapture();
    testSpectrumRouter();
    testCircuitFaultPublication();
    testBackupExport();
    std::cout << "Upstream integration tests passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
