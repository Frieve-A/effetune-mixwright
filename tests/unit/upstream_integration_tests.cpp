#include "bridge/backup_export.h"
#include "bridge/message_router.h"
#include "engine/engine_host.h"
#include "engine/output_analyzers.h"
#include "BassManagementPluginParams.h"
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
      {0xf0000003u, "OscilloscopePlugin", "1", {.01f, 0, 0, 0, .0001f, 0, 0}, 0x84e21dd2u, 1},
      {0xf0000004u, "StereoMeterPlugin", "", {.1f}, 0xb0de3212u, 1},
      {0xf0000005u, "NoteSpectrogramPlugin", "", {60, 72, 2}, 0x0c9bdf4eu, 1},
      {0xf0000006u, "ChromaSpiralPlugin", "", {}, 0x811c9dc5u, 1}};
  expect(analyzers.setSources(sources, &error), "all seven analyzers configure");
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
    testBackupExport();
    std::cout << "Upstream integration tests passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
