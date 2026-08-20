#pragma once

#include "engine/automation_binding.h"

#include <cstdint>
#include <optional>
#include <string_view>

namespace effetune::vst {

// The numeric half of an automation target: everything
// denormalizeAutomationPackedValue() reads, and nothing else. Trivially
// copyable so the audio thread can consume a pre-resolved copy without
// touching the descriptor's strings.
struct AutomationDenormalization {
  AutomationValueNormalization normalization = AutomationValueNormalization::linear;
  AutomationValueTransform transform = AutomationValueTransform::identity;
  double transformReference = 1.0;
  double minimum = 0.0;
  double maximum = 1.0;
  std::int32_t stepCount = 0;

  [[nodiscard]] bool operator==(const AutomationDenormalization &) const = default;
};

[[nodiscard]] std::optional<float>
denormalizeAutomationPackedValue(const AutomationDenormalization &denormalization,
                                 double normalized) noexcept;

// The public value behind a normalized host position: the number the target's
// minimum, maximum and unit are expressed in, and therefore the only number
// worth showing a user. denormalizeAutomationPackedValue() is this followed by
// the transform that turns a public value into the number the plug-in stores,
// so the two agree by construction rather than by a second copy of the switch.
[[nodiscard]] std::optional<double>
automationPublicValue(const AutomationDenormalization &denormalization,
                      double normalized) noexcept;

// The inverse of automationPublicValue(). Returns nullopt for a value the
// normalization cannot represent -- a non-positive value on a logarithmic
// scale, a degenerate range, a stepped target with no steps -- so a typed value
// is refused rather than guessed at.
[[nodiscard]] std::optional<double>
automationNormalizedFromPublicValue(const AutomationDenormalization &denormalization,
                                    double value) noexcept;

// The display name of one value of an enumeration-normalized target, looked up
// in the generated catalog by the target's identity. AutomationTargetDescriptor
// deliberately does not carry the names: it lives in a fixed 256-slot array
// that every reconcile rebuilds, and the names are only wanted when a host asks
// for a display string. Returns nullopt when the identity names no generated
// parameter, when that parameter carries no enum names, or when the index is
// outside them.
[[nodiscard]] std::optional<std::string_view>
automationEnumValueName(const AutomationTargetIdentity &identity,
                        std::uint32_t index) noexcept;

// The inverse lookup, for a value typed into a host's generic editor. The match
// is exact: guessing at a near miss would silently set a value the user did not
// ask for.
[[nodiscard]] std::optional<std::uint32_t>
automationEnumValueIndex(const AutomationTargetIdentity &identity,
                         std::string_view name) noexcept;

} // namespace effetune::vst
