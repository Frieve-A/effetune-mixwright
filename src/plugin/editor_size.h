#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace effetune::vst::plugin {

inline constexpr std::int32_t kEditorContentWidth = 1464;
inline constexpr std::int32_t kVerticalScrollbarAllowance = 17;
inline constexpr std::int32_t kDefaultEditorWidth =
    kEditorContentWidth + kVerticalScrollbarAllowance;
inline constexpr std::int32_t kDefaultEditorHeight = 760;
inline constexpr std::int32_t kMinimumEditorWidth = 760;
inline constexpr std::int32_t kMinimumEditorHeight = 480;
inline constexpr std::int32_t kMaximumEditorWidth = 2560;
inline constexpr std::int32_t kMaximumEditorHeight = 1600;

[[nodiscard]] inline std::int32_t scaleEditorDimension(
    const std::int32_t dimension, const double scaleFactor) noexcept {
  const auto scaled = std::clamp(
      static_cast<double>(dimension) * scaleFactor, 1.0,
      static_cast<double>(std::numeric_limits<std::int32_t>::max()));
  return static_cast<std::int32_t>(std::llround(scaled));
}

} // namespace effetune::vst::plugin
