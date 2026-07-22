#include "engine/resampler.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <numeric>
#include <stdexcept>

#if defined(_M_AVX2) || defined(__AVX2__) || defined(_M_X64) || defined(__SSE2__)
#include <immintrin.h>
#elif defined(__aarch64__) || defined(_M_ARM64)
#include <arm_neon.h>
#endif

namespace effetune::vst {
namespace {

double besselI0(const double value) {
  double sum = 1.0;
  double term = 1.0;
  const double squared = value * value * 0.25;
  for (int index = 1; index < 32; ++index) {
    term *= squared / static_cast<double>(index * index);
    sum += term;
    if (term < sum * 1.0e-16) {
      break;
    }
  }
  return sum;
}

double attenuationForQuality(const FilterQuality quality) {
  switch (quality) {
  case FilterQuality::low:
    return 90.0;
  case FilterQuality::high:
    return 150.0;
  case FilterQuality::ultra:
    return 170.0;
  case FilterQuality::medium:
  default:
    return 120.0;
  }
}

double kaiserBeta(const double attenuation) {
  if (attenuation > 50.0) {
    return 0.1102 * (attenuation - 8.7);
  }
  if (attenuation >= 21.0) {
    const auto delta = attenuation - 21.0;
    return 0.5842 * std::pow(delta, 0.4) + 0.07886 * delta;
  }
  return 0.0;
}

std::uint32_t stageCount(const std::uint32_t factor) {
  switch (factor) {
  case 1:
    return 0;
  case 2:
    return 1;
  case 4:
    return 2;
  case 8:
    return 3;
  default:
    throw std::invalid_argument("Oversampling factor must be 1, 2, 4, or 8");
  }
}

std::uint32_t nextPowerOfTwo(std::uint32_t value) {
  std::uint32_t result = 1;
  while (result < value) {
    result <<= 1u;
  }
  return result;
}

float dotProduct(const float *left, const float *right, const std::uint32_t count) noexcept {
  std::uint32_t index = 0;
  float result = 0.0f;
#if defined(_M_AVX2) || defined(__AVX2__)
  auto sum = _mm256_setzero_ps();
  for (; index + 8u <= count; index += 8u) {
    sum = _mm256_fmadd_ps(_mm256_loadu_ps(left + index),
                          _mm256_loadu_ps(right + index), sum);
  }
  const auto low = _mm256_castps256_ps128(sum);
  const auto high = _mm256_extractf128_ps(sum, 1);
  auto reduced = _mm_add_ps(low, high);
  reduced = _mm_hadd_ps(reduced, reduced);
  reduced = _mm_hadd_ps(reduced, reduced);
  result = _mm_cvtss_f32(reduced);
#elif defined(_M_X64) || defined(__SSE2__)
  auto sum = _mm_setzero_ps();
  for (; index + 4u <= count; index += 4u) {
    sum = _mm_add_ps(sum, _mm_mul_ps(_mm_loadu_ps(left + index),
                                     _mm_loadu_ps(right + index)));
  }
  auto high = _mm_movehl_ps(sum, sum);
  sum = _mm_add_ps(sum, high);
  high = _mm_shuffle_ps(sum, sum, 1);
  result = _mm_cvtss_f32(_mm_add_ss(sum, high));
#elif defined(__aarch64__) || defined(_M_ARM64)
  auto sum = vdupq_n_f32(0.0f);
  for (; index + 4u <= count; index += 4u) {
    sum = vmlaq_f32(sum, vld1q_f32(left + index), vld1q_f32(right + index));
  }
  result = vaddvq_f32(sum);
#endif
  for (; index < count; ++index) {
    result += left[index] * right[index];
  }
  return result;
}

} // namespace

std::uint32_t FirDesigner::tapCount(const FilterQuality quality,
                                    const std::uint32_t stageIndex) {
  std::uint32_t base = 127;
  switch (quality) {
  case FilterQuality::low:
    base = 119;
    break;
  case FilterQuality::high:
    base = 335;
    break;
  case FilterQuality::ultra:
    base = 511;
    break;
  case FilterQuality::medium:
  default:
    base = 203;
    break;
  }
  auto taps = ((base + 1u) >> std::min(stageIndex, 3u)) - 1u;
  taps = std::max(31u, taps);
  // A 4m-1 length places the half-band centre on the non-zero polyphase branch.
  taps -= (taps + 1u) % 4u;
  return std::max(31u, taps);
}

void FirDesigner::fft(std::vector<std::complex<double>> &values, const bool inverse) {
  const auto size = values.size();
  for (std::size_t index = 1, reversed = 0; index < size; ++index) {
    std::size_t bit = size >> 1u;
    for (; (reversed & bit) != 0u; bit >>= 1u) {
      reversed ^= bit;
    }
    reversed ^= bit;
    if (index < reversed) {
      std::swap(values[index], values[reversed]);
    }
  }

  for (std::size_t length = 2; length <= size; length <<= 1u) {
    const auto angle = (inverse ? 2.0 : -2.0) * std::numbers::pi /
                       static_cast<double>(length);
    const std::complex<double> root(std::cos(angle), std::sin(angle));
    for (std::size_t offset = 0; offset < size; offset += length) {
      std::complex<double> weight(1.0, 0.0);
      for (std::size_t index = 0; index < length / 2u; ++index) {
        const auto even = values[offset + index];
        const auto odd = values[offset + index + length / 2u] * weight;
        values[offset + index] = even + odd;
        values[offset + index + length / 2u] = even - odd;
        weight *= root;
      }
    }
  }
  if (inverse) {
    const auto scale = 1.0 / static_cast<double>(size);
    for (auto &value : values) {
      value *= scale;
    }
  }
}

std::vector<double> FirDesigner::minimumPhase(const std::vector<double> &linear) {
  const auto fftSize = nextPowerOfTwo(static_cast<std::uint32_t>(linear.size() * 16u));
  std::vector<std::complex<double>> spectrum(fftSize);
  for (std::size_t index = 0; index < linear.size(); ++index) {
    spectrum[index] = linear[index];
  }
  fft(spectrum, false);
  for (auto &bin : spectrum) {
    bin = std::log(std::max(std::abs(bin), 1.0e-12));
  }
  fft(spectrum, true);

  for (std::size_t index = 1; index < fftSize / 2u; ++index) {
    spectrum[index] *= 2.0;
  }
  for (std::size_t index = fftSize / 2u + 1u; index < fftSize; ++index) {
    spectrum[index] = 0.0;
  }
  fft(spectrum, false);
  for (auto &bin : spectrum) {
    bin = std::exp(bin);
  }
  fft(spectrum, true);

  std::vector<double> result(linear.size());
  for (std::size_t index = 0; index < result.size(); ++index) {
    result[index] = spectrum[index].real();
  }
  return result;
}

ResamplerDesign FirDesigner::designHalfBand(const FilterQuality quality,
                                            const OversamplingPhase phase,
                                            const std::uint32_t stageIndex) {
  const auto taps = tapCount(quality, stageIndex);
  const auto centre = static_cast<std::int32_t>((taps - 1u) / 2u);
  const auto beta = kaiserBeta(attenuationForQuality(quality));
  const auto betaDenominator = besselI0(beta);
  std::vector<double> coefficients(taps);

  for (std::uint32_t index = 0; index < taps; ++index) {
    const auto distance = static_cast<std::int32_t>(index) - centre;
    double ideal = 0.5;
    if (distance != 0) {
      // Every second non-centre coefficient of an ideal half-band filter is
      // exactly zero. Set it explicitly so the RT convolution can skip it;
      // evaluating sin(pi * integer) would otherwise leave tiny residues.
      ideal = (distance & 1) == 0
                  ? 0.0
                  : std::sin(0.5 * std::numbers::pi * static_cast<double>(distance)) /
                        (std::numbers::pi * static_cast<double>(distance));
    }
    const auto normalized =
        (2.0 * static_cast<double>(index) / static_cast<double>(taps - 1u)) - 1.0;
    const auto window = besselI0(beta * std::sqrt(std::max(0.0, 1.0 - normalized * normalized))) /
                        betaDenominator;
    coefficients[index] = ideal * window;
  }
  if (phase == OversamplingPhase::minimum) {
    coefficients = minimumPhase(coefficients);
  }

  const auto sum = std::accumulate(coefficients.begin(), coefficients.end(), 0.0);
  if (std::abs(sum) < 1.0e-12) {
    throw std::runtime_error("Invalid half-band FIR design");
  }
  ResamplerDesign design;
  design.coefficients.resize(coefficients.size());
  double energy = 0.0;
  double weightedEnergy = 0.0;
  for (std::size_t index = 0; index < coefficients.size(); ++index) {
    const auto normalized = coefficients[index] / sum;
    design.coefficients[index] = static_cast<float>(normalized);
    energy += normalized * normalized;
    weightedEnergy += static_cast<double>(index) * normalized * normalized;
  }
  design.energyDelay = energy > 0.0 ? weightedEnergy / energy : 0.0;
  return design;
}

void HalfBandStage::prepare(const std::uint32_t channels, const ResamplerDesign &design) {
  if (channels == 0 || design.coefficients.empty()) {
    throw std::invalid_argument("Invalid half-band stage configuration");
  }
  channels_ = channels;
  coefficients_ = design.coefficients;
  energyDelay_ = design.energyDelay;
  const auto upHistoryFrames = (coefficients_.size() + 1u) / 2u;
  reversedCoefficients_.assign(coefficients_.rbegin(), coefficients_.rend());
  upEvenReversed_.assign(upHistoryFrames, 0.0f);
  upOddReversed_.assign(upHistoryFrames, 0.0f);
  for (std::uint32_t historyIndex = 0; historyIndex < upHistoryFrames; ++historyIndex) {
    const auto lag = upHistoryFrames - 1u - historyIndex;
    const auto even = lag * 2u;
    const auto odd = even + 1u;
    if (even < coefficients_.size()) upEvenReversed_[historyIndex] = coefficients_[even];
    if (odd < coefficients_.size()) upOddReversed_[historyIndex] = coefficients_[odd];
  }
  upHistory_.assign(static_cast<std::size_t>(channels_) * upHistoryFrames * 2u, 0.0f);
  downHistory_.assign(static_cast<std::size_t>(channels_) * coefficients_.size() * 2u, 0.0f);
  upPositions_.assign(channels_, 0u);
  downPositions_.assign(channels_, 0u);
}

void HalfBandStage::reset() noexcept {
  std::fill(upHistory_.begin(), upHistory_.end(), 0.0f);
  std::fill(downHistory_.begin(), downHistory_.end(), 0.0f);
  std::fill(upPositions_.begin(), upPositions_.end(), 0u);
  std::fill(downPositions_.begin(), downPositions_.end(), 0u);
}

float HalfBandStage::filterSample(std::vector<float> &history,
                                  std::uint32_t &position,
                                  const std::uint32_t channel, const float input) noexcept {
  const auto taps = static_cast<std::uint32_t>(coefficients_.size());
  const auto historyOffset = static_cast<std::size_t>(channel) * taps * 2u;
  history[historyOffset + position] = input;
  history[historyOffset + position + taps] = input;
  position = (position + 1u) % taps;
  return dotProduct(reversedCoefficients_.data(), history.data() + historyOffset + position,
                    taps);
}

void HalfBandStage::processUp(const float *const *input, const std::uint32_t inputFrames,
                              float *outputPlanar, const std::uint32_t outputStride) noexcept {
  const auto historyFrames = static_cast<std::uint32_t>((coefficients_.size() + 1u) / 2u);
  for (std::uint32_t channel = 0; channel < channels_; ++channel) {
    auto *output = outputPlanar + static_cast<std::size_t>(channel) * outputStride;
    const auto historyOffset = static_cast<std::size_t>(channel) * historyFrames * 2u;
    auto &position = upPositions_[channel];
    for (std::uint32_t frame = 0; frame < inputFrames; ++frame) {
      upHistory_[historyOffset + position] = input[channel][frame];
      upHistory_[historyOffset + position + historyFrames] = input[channel][frame];
      position = (position + 1u) % historyFrames;
      const auto *history = upHistory_.data() + historyOffset + position;
      output[frame * 2u] =
          2.0f * dotProduct(upEvenReversed_.data(), history, historyFrames);
      output[frame * 2u + 1u] =
          2.0f * dotProduct(upOddReversed_.data(), history, historyFrames);
    }
  }
}

void HalfBandStage::processDown(const float *const *input, const std::uint32_t inputFrames,
                                float *outputPlanar,
                                const std::uint32_t outputStride) noexcept {
  for (std::uint32_t channel = 0; channel < channels_; ++channel) {
    auto *output = outputPlanar + static_cast<std::size_t>(channel) * outputStride;
    std::uint32_t outputFrame = 0;
    auto &position = downPositions_[channel];
    for (std::uint32_t frame = 0; frame + 1u < inputFrames; frame += 2u) {
      (void)filterSample(downHistory_, position, channel, input[channel][frame]);
      output[outputFrame++] = filterSample(downHistory_, position, channel,
                                            input[channel][frame + 1u]);
    }
  }
}

void Oversampler::prepare(const OversamplingSettings &settings, const std::uint32_t channels,
                          const std::uint32_t maxHostFrames) {
  if (!settings.isValid() || channels == 0 || channels > kMaxChannels || maxHostFrames == 0) {
    throw std::invalid_argument("Invalid oversampler configuration");
  }
  settings_ = settings;
  channels_ = channels;
  maxHostFrames_ = maxHostFrames;
  latencyHostFrames_ = 0;
  const auto stages = stageCount(settings.factor);
  upStages_.resize(stages);
  downStages_.resize(stages);
  upBuffers_.resize(stages);
  downBuffers_.resize(stages);

  double latency = 0.0;
  for (std::uint32_t stage = 0; stage < stages; ++stage) {
    const auto design = FirDesigner::designHalfBand(settings.quality, settings.phase, stage);
    upStages_[stage].prepare(channels_, design);
    downStages_[stage].prepare(channels_, design);
    const auto rateFactor = static_cast<double>(1u << (stage + 1u));
    const auto delay = settings.phase == OversamplingPhase::linear
                           ? static_cast<double>(design.coefficients.size() - 1u) * 0.5
                           : design.energyDelay;
    // The decimator emits the odd polyphase branch (samples 1, 3, ...), which
    // advances the down path by one sample at that stage's rate. Include that
    // alignment in the latency reported to the host instead of conservatively
    // rounding every stage upward.
    latency += std::max(0.0, 2.0 * delay - 1.0) / rateFactor;

    const auto upFrames = maxHostFrames_ * (1u << (stage + 1u));
    const auto downFrames = maxHostFrames_ * (1u << stage);
    upBuffers_[stage].assign(static_cast<std::size_t>(channels_) * upFrames, 0.0f);
    downBuffers_[stage].assign(static_cast<std::size_t>(channels_) * downFrames, 0.0f);
  }
  passthroughBuffer_.assign(static_cast<std::size_t>(channels_) * maxHostFrames_, 0.0f);
  latencyHostFrames_ = static_cast<std::uint32_t>(std::lround(latency));
  reset();
}

void Oversampler::reset() noexcept {
  for (auto &stage : upStages_) {
    stage.reset();
  }
  for (auto &stage : downStages_) {
    stage.reset();
  }
  for (auto &buffer : upBuffers_) {
    std::fill(buffer.begin(), buffer.end(), 0.0f);
  }
  for (auto &buffer : downBuffers_) {
    std::fill(buffer.begin(), buffer.end(), 0.0f);
  }
  std::fill(passthroughBuffer_.begin(), passthroughBuffer_.end(), 0.0f);
}

const float *const *Oversampler::upsample(const float *const *input,
                                          const std::uint32_t hostFrames) noexcept {
  if (input == nullptr || hostFrames > maxHostFrames_) {
    return nullptr;
  }
  if (settings_.factor == 1) {
    for (std::uint32_t channel = 0; channel < channels_; ++channel) {
      auto *destination = passthroughBuffer_.data() +
                          static_cast<std::size_t>(channel) * maxHostFrames_;
      std::copy_n(input[channel], hostFrames, destination);
      outputPointers_[channel] = destination;
    }
    return outputPointers_.data();
  }

  std::array<const float *, kMaxChannels> sourcePointers{};
  for (std::uint32_t channel = 0; channel < channels_; ++channel) {
    sourcePointers[channel] = input[channel];
  }
  std::uint32_t inputFrames = hostFrames;
  for (std::uint32_t stage = 0; stage < upStages_.size(); ++stage) {
    const auto outputFrames = inputFrames * 2u;
    upStages_[stage].processUp(sourcePointers.data(), inputFrames, upBuffers_[stage].data(),
                              outputFrames);
    for (std::uint32_t channel = 0; channel < channels_; ++channel) {
      sourcePointers[channel] = upBuffers_[stage].data() +
                                static_cast<std::size_t>(channel) * outputFrames;
    }
    inputFrames = outputFrames;
  }
  outputPointers_ = sourcePointers;
  return outputPointers_.data();
}

bool Oversampler::downsample(const float *const *input, const std::uint32_t engineFrames,
                             float *const *output) noexcept {
  if (input == nullptr || output == nullptr || engineFrames > maxHostFrames_ * settings_.factor ||
      engineFrames % settings_.factor != 0) {
    return false;
  }
  if (settings_.factor == 1) {
    for (std::uint32_t channel = 0; channel < channels_; ++channel) {
      std::copy_n(input[channel], engineFrames, output[channel]);
    }
    return true;
  }

  std::array<const float *, kMaxChannels> sourcePointers{};
  for (std::uint32_t channel = 0; channel < channels_; ++channel) {
    sourcePointers[channel] = input[channel];
  }
  std::uint32_t inputFrames = engineFrames;
  for (std::size_t reverse = downStages_.size(); reverse-- > 0;) {
    const auto outputFrames = inputFrames / 2u;
    downStages_[reverse].processDown(sourcePointers.data(), inputFrames,
                                    downBuffers_[reverse].data(), outputFrames);
    for (std::uint32_t channel = 0; channel < channels_; ++channel) {
      sourcePointers[channel] = downBuffers_[reverse].data() +
                                static_cast<std::size_t>(channel) * outputFrames;
    }
    inputFrames = outputFrames;
  }
  for (std::uint32_t channel = 0; channel < channels_; ++channel) {
    std::copy_n(sourcePointers[channel], inputFrames, output[channel]);
  }
  return true;
}

} // namespace effetune::vst
