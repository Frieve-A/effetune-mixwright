#include "engine/engine_host.h"

#include "VolumePluginParams.h"
#include "choc/audio/choc_AudioFileFormat.h"
#include "choc/audio/choc_AudioFileFormat_WAV.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct Options {
  std::filesystem::path input;
  std::filesystem::path output;
  float gainDb = 0.0f;
  bool help = false;
};

void printUsage() {
  std::cout << "EffeTune native DSP headless host\n\n"
               "Usage:\n"
               "  effetune-headless --input <input.wav> --output <output.wav> "
               "[--gain-db <dB>]\n\n"
               "The fixed pipeline contains the native VolumePlugin kernel.\n";
}

[[nodiscard]] float parseFloat(const std::string_view text) {
  const std::string source(text);
  char *end = nullptr;
  errno = 0;
  const float value = std::strtof(source.c_str(), &end);
  if (end == source.c_str() || end != source.c_str() + source.size() || errno == ERANGE) {
    throw std::runtime_error("Invalid floating-point value: " + std::string(text));
  }
  return value;
}

[[nodiscard]] std::filesystem::path utf8Path(const std::string_view text) {
  return std::filesystem::path(std::u8string(
      reinterpret_cast<const char8_t *>(text.data()),
      reinterpret_cast<const char8_t *>(text.data() + text.size())));
}

[[nodiscard]] Options parseOptions(const int argc, const char *const *argv) {
  Options options;
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument(argv[index]);
    if (argument == "--help" || argument == "-h") {
      options.help = true;
      continue;
    }
    if (index + 1 >= argc) {
      throw std::runtime_error("Missing value after " + std::string(argument));
    }
    const std::string_view value(argv[++index]);
    if (argument == "--input") {
      options.input = utf8Path(value);
    } else if (argument == "--output") {
      options.output = utf8Path(value);
    } else if (argument == "--gain-db") {
      options.gainDb = parseFloat(value);
    } else {
      throw std::runtime_error("Unknown option: " + std::string(argument));
    }
  }
  if (!options.help && (options.input.empty() || options.output.empty())) {
    throw std::runtime_error("Both --input and --output are required");
  }
  if (!std::isfinite(options.gainDb) || options.gainDb < -120.0f || options.gainDb > 24.0f) {
    throw std::runtime_error("--gain-db must be in the range -120 to +24 dB");
  }
  return options;
}

void process(const Options &options) {
  choc::audio::WAVAudioFileFormat<true> wavFormat;
  auto reader = wavFormat.createReader(options.input);
  if (reader == nullptr) {
    throw std::runtime_error("Unable to open input WAV file");
  }

  const auto properties = reader->getProperties();
  if (properties.numChannels == 0 || properties.numChannels > effetune::vst::EngineHost::kMaxChannels) {
    throw std::runtime_error("Input WAV must contain between 1 and 8 channels");
  }
  if (properties.numFrames > std::numeric_limits<choc::buffer::FrameCount>::max()) {
    throw std::runtime_error("Input WAV is too large for this offline host");
  }

  auto input = reader->readEntireStream<float>();
  if (input.getNumFrames() != properties.numFrames) {
    throw std::runtime_error("Unable to read all input WAV frames");
  }

  effetune::vst::EngineHost engine;
  std::string error;
  if (!engine.prepare(properties.sampleRate, properties.numChannels,
                      effetune::vst::EngineHost::kDefaultTelemetryBytes, &error)) {
    throw std::runtime_error(error);
  }

  constexpr std::uint32_t logicalId = 1;
  effetune::vst::PluginState volume;
  volume.id = logicalId;
  volume.name = "VolumePlugin";
  volume.parametersJson = "{\"vl\":" + std::to_string(options.gainDb) + "}";
  effetune::vst::PipelineState pipeline{{volume}};

  effetune::vst::RuntimePlugin runtime;
  runtime.logicalId = logicalId;
  runtime.type = "VolumePlugin";
  runtime.packedParameters = {options.gainDb};
  runtime.paramsHash = effetune::generated::VolumePluginParams::kHash;
  if (!engine.rebuild(pipeline, {runtime}, &error)) {
    throw std::runtime_error(error);
  }

  choc::buffer::ChannelArrayBuffer<float> output(properties.numChannels,
                                                  input.getNumFrames());
  std::array<std::array<float, effetune::vst::EngineHost::kMaxProcessFrames>,
             effetune::vst::EngineHost::kMaxChannels>
      block{};
  std::array<float *, effetune::vst::EngineHost::kMaxChannels> pointers{};
  for (std::uint32_t channel = 0; channel < properties.numChannels; ++channel) {
    pointers[channel] = block[channel].data();
  }

  const auto frameCount = static_cast<std::uint32_t>(input.getNumFrames());
  for (std::uint32_t offset = 0; offset < frameCount;
       offset += effetune::vst::EngineHost::kMaxProcessFrames) {
    const auto validFrames = std::min(effetune::vst::EngineHost::kMaxProcessFrames,
                                      frameCount - offset);
    for (std::uint32_t channel = 0; channel < properties.numChannels; ++channel) {
      for (std::uint32_t frame = 0; frame < validFrames; ++frame) {
        block[channel][frame] = input.getView().getSample(channel, offset + frame);
      }
    }
    const auto timeSeconds = static_cast<double>(offset) / properties.sampleRate;
    if (!engine.tryProcessBlock(pointers.data(), properties.numChannels,
                                validFrames, timeSeconds, false)) {
      throw std::runtime_error("Native DSP processing failed");
    }
    for (std::uint32_t channel = 0; channel < properties.numChannels; ++channel) {
      for (std::uint32_t frame = 0; frame < validFrames; ++frame) {
        output.getView().getSample(channel, offset + frame) = block[channel][frame];
      }
    }
  }

  choc::audio::AudioFileProperties outputProperties;
  outputProperties.formatName = "WAV";
  outputProperties.sampleRate = properties.sampleRate;
  outputProperties.numFrames = properties.numFrames;
  outputProperties.numChannels = properties.numChannels;
  outputProperties.bitDepth = choc::audio::BitDepth::float32;
  outputProperties.speakers = properties.speakers;
  auto writer = wavFormat.createWriter(options.output, outputProperties);
  if (writer == nullptr || !writer->appendFrames(output.getView()) || !writer->flush()) {
    throw std::runtime_error("Unable to write output WAV file");
  }

  std::cout << "Processed " << properties.numFrames << " frames, "
            << properties.numChannels << " channel(s), " << properties.sampleRate
            << " Hz, gain " << options.gainDb << " dB\n";
}

} // namespace

int main(const int argc, const char *const *argv) {
  try {
    const auto options = parseOptions(argc, argv);
    if (options.help) {
      printUsage();
      return 0;
    }
    process(options);
    return 0;
  } catch (const std::exception &exception) {
    std::cerr << "error: " << exception.what() << '\n';
    printUsage();
    return 1;
  }
}
