#include "engine/automation_binding.h"

#include "engine/automation_catalog.h"

#include "AutomationCatalog.h"
#include "choc/text/choc_JSON.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace effetune::vst {
namespace {

using effetune::generated::AutomationEffectDescriptor;
using effetune::generated::AutomationNormalization;
using effetune::generated::AutomationParameterDescriptor;
using GeneratedAutomationValueTransform =
    effetune::generated::AutomationValueTransform;
using choc::value::ValueView;

[[nodiscard]] std::optional<choc::value::Value>
parseObject(const std::string &json) {
  try {
    auto value = choc::json::parse(json);
    if (value.isObject()) {
      return value;
    }
  } catch (const choc::json::ParseError &) {
  }
  return std::nullopt;
}

[[nodiscard]] const AutomationEffectDescriptor *
findEffect(const std::string_view type) noexcept {
  const auto found = std::find_if(
      effetune::generated::kAutomationEffects.begin(),
      effetune::generated::kAutomationEffects.end(),
      [type](const AutomationEffectDescriptor &effect) {
        return effect.type == type;
      });
  return found == effetune::generated::kAutomationEffects.end() ? nullptr
                                                                  : &*found;
}

// The generated descriptor behind one automation target, found by the same
// (effect type, parameter key, element) triple both the binding state and the
// target identity carry. Bounds-checked against the flat parameter array so a
// catalog that disagrees with an effect's declared range cannot read past it.
[[nodiscard]] const AutomationParameterDescriptor *
findParameter(const std::string_view type, const std::string_view key,
              const std::uint32_t element) noexcept {
  const auto *effect = findEffect(type);
  if (effect == nullptr) {
    return nullptr;
  }
  const auto first = static_cast<std::size_t>(effect->firstParameter);
  const auto count = static_cast<std::size_t>(effect->parameterCount);
  if (first > effetune::generated::kAutomationParameters.size() ||
      count > effetune::generated::kAutomationParameters.size() - first) {
    return nullptr;
  }
  const auto last = effetune::generated::kAutomationParameters.begin() +
                    static_cast<std::ptrdiff_t>(first + count);
  const auto found = std::find_if(
      effetune::generated::kAutomationParameters.begin() +
          static_cast<std::ptrdiff_t>(first),
      last, [key, element](const AutomationParameterDescriptor &candidate) {
        return candidate.key == key && candidate.element == element;
      });
  return found == last ? nullptr : &*found;
}

// The enum names of one generated parameter, or an empty span when it carries
// none. Bounds-checked against the flat name array for the same reason.
[[nodiscard]] std::span<const std::string_view>
enumValueNames(const AutomationParameterDescriptor &descriptor) noexcept {
  if (descriptor.enumValueCount == 0 ||
      descriptor.firstEnumValue > effetune::generated::kAutomationEnumValues.size() ||
      descriptor.enumValueCount > effetune::generated::kAutomationEnumValues.size() -
                                      descriptor.firstEnumValue) {
    return {};
  }
  return std::span<const std::string_view>(
      effetune::generated::kAutomationEnumValues.data() + descriptor.firstEnumValue,
      descriptor.enumValueCount);
}

[[nodiscard]] std::optional<double>
packedToPublicValue(const AutomationParameterDescriptor &descriptor,
                    const double packed) noexcept {
  if (!std::isfinite(packed)) {
    return std::nullopt;
  }
  double value = 0.0;
  switch (descriptor.transform) {
  case GeneratedAutomationValueTransform::NaturalLog:
    value = std::exp(packed);
    break;
  case GeneratedAutomationValueTransform::Log10:
    value = std::pow(10.0, packed);
    break;
  case GeneratedAutomationValueTransform::DecibelsFromReference:
    if (!(descriptor.transformReference > 0.0f)) {
      return std::nullopt;
    }
    value = static_cast<double>(descriptor.transformReference) *
            std::pow(10.0, packed / 20.0);
    break;
  case GeneratedAutomationValueTransform::Identity:
  default:
    value = packed;
    break;
  }
  return std::isfinite(value) ? std::optional<double>(value) : std::nullopt;
}

[[nodiscard]] std::optional<double>
publicToPackedValue(const AutomationValueTransform transform,
                    const double reference, const double value) noexcept {
  if (!std::isfinite(value)) {
    return std::nullopt;
  }
  double packed = 0.0;
  switch (transform) {
  case AutomationValueTransform::naturalLog:
    if (!(value > 0.0)) {
      return std::nullopt;
    }
    packed = std::log(value);
    break;
  case AutomationValueTransform::log10:
    if (!(value > 0.0)) {
      return std::nullopt;
    }
    packed = std::log10(value);
    break;
  case AutomationValueTransform::decibelsFromReference:
    if (!(reference > 0.0) || !(value > 0.0)) {
      return std::nullopt;
    }
    packed = 20.0 * std::log10(value / reference);
    break;
  case AutomationValueTransform::identity:
  default:
    packed = value;
    break;
  }
  return std::isfinite(packed) ? std::optional<double>(packed) : std::nullopt;
}

[[nodiscard]] std::optional<double>
normalizePublicValue(const AutomationParameterDescriptor &descriptor,
                     const double value) noexcept {
  if (!std::isfinite(value)) {
    return std::nullopt;
  }
  double normalized = 0.0;
  switch (descriptor.normalization) {
  case AutomationNormalization::Linear:
    if (!(descriptor.maximum > descriptor.minimum)) {
      return std::nullopt;
    }
    normalized = (value - descriptor.minimum) /
                 (descriptor.maximum - descriptor.minimum);
    break;
  case AutomationNormalization::Logarithmic:
    if (!(descriptor.minimum > 0.0f) ||
        !(descriptor.maximum > descriptor.minimum) || !(value > 0.0)) {
      return std::nullopt;
    }
    normalized = std::log(value / descriptor.minimum) /
                 std::log(descriptor.maximum / descriptor.minimum);
    break;
  case AutomationNormalization::Integer:
    if (descriptor.stepCount == 0 || !(descriptor.step > 0.0f)) {
      return std::nullopt;
    }
    normalized = (value - descriptor.minimum) / descriptor.step /
                 static_cast<double>(descriptor.stepCount);
    break;
  case AutomationNormalization::Enum:
    if (descriptor.stepCount == 0) {
      return std::nullopt;
    }
    normalized = (value - descriptor.minimum) /
                 static_cast<double>(descriptor.stepCount);
    break;
  case AutomationNormalization::Bool:
    normalized = value == 0.0 ? 0.0 : 1.0;
    break;
  }
  return std::clamp(normalized, 0.0, 1.0);
}

[[nodiscard]] AutomationValueTransform
valueTransform(const GeneratedAutomationValueTransform transform) noexcept {
  switch (transform) {
  case GeneratedAutomationValueTransform::NaturalLog:
    return AutomationValueTransform::naturalLog;
  case GeneratedAutomationValueTransform::Log10:
    return AutomationValueTransform::log10;
  case GeneratedAutomationValueTransform::DecibelsFromReference:
    return AutomationValueTransform::decibelsFromReference;
  case GeneratedAutomationValueTransform::Identity:
  default:
    return AutomationValueTransform::identity;
  }
}

[[nodiscard]] std::optional<double>
readPackedValue(const ValueView parameters,
                const AutomationParameterDescriptor &descriptor) {
  auto value = descriptor.containerKey.empty() ? parameters[descriptor.field]
                                               : parameters[descriptor.containerKey];
  if (!descriptor.containerKey.empty()) {
    if (!value.isArray() || descriptor.element >= value.size()) {
      return std::nullopt;
    }
    value = value[descriptor.element];
    if (!descriptor.memberKey.empty()) {
      if (!value.isObject()) {
        return std::nullopt;
      }
      value = value[descriptor.memberKey];
    }
  }
  if (value.isBool()) {
    return value.getBool() ? 1.0 : 0.0;
  }
  if (value.isInt() || value.isFloat()) {
    return value.get<double>();
  }
  if (value.isString() && descriptor.enumValueCount != 0 &&
      descriptor.firstEnumValue <= effetune::generated::kAutomationEnumValues.size() &&
      descriptor.enumValueCount <= effetune::generated::kAutomationEnumValues.size() -
                                       descriptor.firstEnumValue) {
    const auto encoded = value.get<std::string>();
    for (std::uint32_t index = 0; index < descriptor.enumValueCount; ++index) {
      if (effetune::generated::kAutomationEnumValues[descriptor.firstEnumValue + index] ==
          encoded) {
        return static_cast<double>(index);
      }
    }
  }
  return std::nullopt;
}

void appendTarget(std::vector<AutomationTargetDescriptor> &targets,
                  const PluginState &plugin, const char side,
                  const std::string &type, const ValueView parameters,
                  const AutomationParameterDescriptor &descriptor) {
  if (descriptor.stepCount >
      static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max())) {
    return;
  }
  const auto current = readPackedValue(parameters, descriptor);
  const auto currentPublic = current.has_value()
                                 ? packedToPublicValue(descriptor, *current)
                                 : std::nullopt;
  const auto currentNormalized = currentPublic.has_value()
                                     ? normalizePublicValue(descriptor, *currentPublic)
                                     : std::nullopt;
  const auto defaultNormalized =
      normalizePublicValue(descriptor, descriptor.defaultValue);
  if (!currentNormalized.has_value() || !defaultNormalized.has_value()) {
    return;
  }
  AutomationTargetDescriptor target;
  target.identity = {side, plugin.id, type, std::string(descriptor.key),
                     descriptor.element};
  const auto discriminator = std::string(1, side) + " #" +
                             std::to_string(plugin.id) + " " +
                             std::string(descriptor.publicName) + "[" +
                             std::to_string(descriptor.element + 1u) + "] - " +
                             plugin.name + " - ";
  target.title = discriminator + std::string(descriptor.title);
  target.shortTitle = discriminator +
                      std::string(descriptor.shortTitle.empty()
                                      ? descriptor.title
                                      : descriptor.shortTitle);
  target.units = std::string(descriptor.unit);
  target.stepCount = static_cast<std::int32_t>(descriptor.stepCount);
  target.defaultNormalized = *defaultNormalized;
  target.currentNormalized = *currentNormalized;
  target.packedOffset = descriptor.packedOffset;
  target.transform = valueTransform(descriptor.transform);
  target.transformReference = descriptor.transformReference;
  switch (descriptor.normalization) {
  case AutomationNormalization::Logarithmic:
    target.normalization = AutomationValueNormalization::logarithmic;
    break;
  case AutomationNormalization::Integer:
    target.normalization = AutomationValueNormalization::integer;
    break;
  case AutomationNormalization::Bool:
    target.normalization = AutomationValueNormalization::boolean;
    break;
  case AutomationNormalization::Enum:
    target.normalization = AutomationValueNormalization::enumeration;
    break;
  case AutomationNormalization::Linear:
  default:
    target.normalization = AutomationValueNormalization::linear;
    break;
  }
  target.minimum = descriptor.minimum;
  target.maximum = descriptor.maximum;
  target.step = descriptor.step;
  target.continuous = descriptor.eligibility ==
                      effetune::generated::AutomationEligibility::Continuous;
  targets.push_back(std::move(target));
}

void appendPipelineTargets(std::vector<AutomationTargetDescriptor> &targets,
                           const PipelineState &pipeline, const char side) {
  for (const auto &plugin : pipeline.plugins) {
    if (plugin.unknown || plugin.id == 0) {
      continue;
    }
    const auto extra = parseObject(plugin.extraJson);
    const auto parameters = parseObject(plugin.parametersJson);
    if (!extra.has_value() || !parameters.has_value()) {
      continue;
    }
    const auto type = (*extra)["type"].getWithDefault<std::string>({});
    const auto *effect = findEffect(type);
    if (effect == nullptr) {
      continue;
    }
    const auto first = static_cast<std::size_t>(effect->firstParameter);
    const auto count = static_cast<std::size_t>(effect->parameterCount);
    if (first > effetune::generated::kAutomationParameters.size() ||
        count > effetune::generated::kAutomationParameters.size() - first) {
      continue;
    }
    for (std::size_t index = first; index < first + count; ++index) {
      const auto &generated = effetune::generated::kAutomationParameters[index];
      appendTarget(targets, plugin, side, type, *parameters, generated);
    }
  }
}

} // namespace

std::vector<AutomationTargetDescriptor>
generatedAutomationTargets(const PluginStateDocument &document) {
  std::vector<AutomationTargetDescriptor> targets;
  appendPipelineTargets(targets, document.pipelineA, 'A');
  appendPipelineTargets(targets, document.pipelineB, 'B');
  return targets;
}

std::optional<double>
automationPublicValue(const AutomationDenormalization &denormalization,
                      const double normalized) noexcept {
  if (!std::isfinite(normalized) || normalized < 0.0 || normalized > 1.0) {
    return std::nullopt;
  }
  double value = 0.0;
  switch (denormalization.normalization) {
  case AutomationValueNormalization::logarithmic:
    if (!(denormalization.minimum > 0.0) ||
        !(denormalization.maximum > denormalization.minimum)) {
      return std::nullopt;
    }
    value = denormalization.minimum *
            std::pow(denormalization.maximum / denormalization.minimum, normalized);
    break;
  case AutomationValueNormalization::integer:
    if (denormalization.stepCount <= 0 || !(denormalization.step > 0.0)) {
      return std::nullopt;
    }
    value = denormalization.minimum +
            std::round(normalized * denormalization.stepCount) * denormalization.step;
    break;
  case AutomationValueNormalization::enumeration:
    value = denormalization.minimum +
            std::round(normalized * denormalization.stepCount);
    break;
  case AutomationValueNormalization::boolean:
    value = normalized >= 0.5 ? 1.0 : 0.0;
    break;
  case AutomationValueNormalization::linear:
  default:
    value = denormalization.minimum +
            normalized * (denormalization.maximum - denormalization.minimum);
    break;
  }
  return std::isfinite(value) ? std::optional<double>(value) : std::nullopt;
}

std::optional<double> automationNormalizedFromPublicValue(
    const AutomationDenormalization &denormalization, const double value) noexcept {
  if (!std::isfinite(value)) {
    return std::nullopt;
  }
  double normalized = 0.0;
  switch (denormalization.normalization) {
  case AutomationValueNormalization::logarithmic:
    if (!(denormalization.minimum > 0.0) ||
        !(denormalization.maximum > denormalization.minimum) || !(value > 0.0)) {
      return std::nullopt;
    }
    normalized = std::log(value / denormalization.minimum) /
                 std::log(denormalization.maximum / denormalization.minimum);
    break;
  case AutomationValueNormalization::integer: {
    if (denormalization.stepCount <= 0 || !(denormalization.step > 0.0)) {
      return std::nullopt;
    }
    const auto stepPosition = (value - denormalization.minimum) / denormalization.step;
    if (stepPosition != std::round(stepPosition)) {
      return std::nullopt;
    }
    normalized = stepPosition / static_cast<double>(denormalization.stepCount);
    break;
  }
  case AutomationValueNormalization::enumeration:
    if (denormalization.stepCount == 0) {
      return std::nullopt;
    }
    normalized = (value - denormalization.minimum) /
                 static_cast<double>(denormalization.stepCount);
    break;
  case AutomationValueNormalization::boolean:
    normalized = value == 0.0 ? 0.0 : 1.0;
    break;
  case AutomationValueNormalization::linear:
  default:
    if (!(denormalization.maximum > denormalization.minimum)) {
      return std::nullopt;
    }
    normalized = (value - denormalization.minimum) /
                 (denormalization.maximum - denormalization.minimum);
    break;
  }
  return std::isfinite(normalized) ? std::optional<double>(std::clamp(normalized, 0.0, 1.0))
                                   : std::nullopt;
}

std::optional<float> denormalizeAutomationPackedValue(
    const AutomationDenormalization &denormalization,
    const double normalized) noexcept {
  const auto value = automationPublicValue(denormalization, normalized);
  if (!value.has_value()) {
    return std::nullopt;
  }
  const auto packed = publicToPackedValue(
      denormalization.transform, denormalization.transformReference, *value);
  return packed.has_value()
             ? std::optional<float>(static_cast<float>(*packed))
             : std::nullopt;
}

std::optional<float> denormalizeAutomationPackedValue(
    const AutomationTargetDescriptor &target, const double normalized) noexcept {
  return denormalizeAutomationPackedValue(
      AutomationDenormalization{target.normalization, target.transform,
                                target.transformReference, target.minimum,
                                target.maximum, target.stepCount, target.step},
      normalized);
}

std::optional<std::string_view>
automationEnumValueName(const AutomationTargetIdentity &identity,
                        const std::uint32_t index) noexcept {
  const auto *descriptor =
      findParameter(identity.pluginType, identity.parameterKey, identity.elementIndex);
  if (descriptor == nullptr) {
    return std::nullopt;
  }
  const auto names = enumValueNames(*descriptor);
  return index < names.size() ? std::optional<std::string_view>(names[index])
                              : std::nullopt;
}

std::optional<std::uint32_t>
automationEnumValueIndex(const AutomationTargetIdentity &identity,
                         const std::string_view name) noexcept {
  const auto *descriptor =
      findParameter(identity.pluginType, identity.parameterKey, identity.elementIndex);
  if (descriptor == nullptr) {
    return std::nullopt;
  }
  const auto names = enumValueNames(*descriptor);
  for (std::size_t index = 0; index < names.size(); ++index) {
    if (names[index] == name) {
      return static_cast<std::uint32_t>(index);
    }
  }
  return std::nullopt;
}

bool applyAutomationNormalizedValue(PluginStateDocument &document,
                                    const AutomationBindingState &binding,
                                    const double normalized) {
  auto &plugins = binding.pipeline == 'B' ? document.pipelineB.plugins
                                          : document.pipelineA.plugins;
  const auto plugin = std::find_if(plugins.begin(), plugins.end(), [&binding](const auto &item) {
    return item.id == binding.pluginId;
  });
  if (plugin == plugins.end()) {
    return false;
  }
  const auto *descriptor =
      findParameter(binding.pluginType, binding.parameterKey, binding.elementIndex);
  if (descriptor == nullptr) {
    return false;
  }
  AutomationTargetDescriptor conversion;
  conversion.normalization = descriptor->normalization == AutomationNormalization::Logarithmic
                                 ? AutomationValueNormalization::logarithmic
                             : descriptor->normalization == AutomationNormalization::Integer
                                 ? AutomationValueNormalization::integer
                             : descriptor->normalization == AutomationNormalization::Bool
                                 ? AutomationValueNormalization::boolean
                             : descriptor->normalization == AutomationNormalization::Enum
                                 ? AutomationValueNormalization::enumeration
                                 : AutomationValueNormalization::linear;
  conversion.minimum = descriptor->minimum;
  conversion.maximum = descriptor->maximum;
  conversion.stepCount = static_cast<std::int32_t>(descriptor->stepCount);
  conversion.step = descriptor->step;
  conversion.transform = valueTransform(descriptor->transform);
  conversion.transformReference = descriptor->transformReference;
  const auto packed = denormalizeAutomationPackedValue(conversion, normalized);
  if (!packed.has_value()) {
    return false;
  }

  choc::value::Value plain;
  if (descriptor->normalization == AutomationNormalization::Bool) {
    plain = choc::value::Value(*packed >= 0.5f);
  } else if (descriptor->normalization == AutomationNormalization::Enum) {
    const auto index = static_cast<std::uint32_t>(std::lround(*packed));
    if (index >= descriptor->enumValueCount ||
        descriptor->firstEnumValue + index >=
            effetune::generated::kAutomationEnumValues.size()) {
      return false;
    }
    plain = choc::value::Value(
        effetune::generated::kAutomationEnumValues[descriptor->firstEnumValue + index]);
  } else if (descriptor->normalization == AutomationNormalization::Integer) {
    plain = choc::value::Value(static_cast<std::int64_t>(std::lround(*packed)));
  } else {
    plain = choc::value::Value(static_cast<double>(*packed));
  }

  auto parameters = parseObject(plugin->parametersJson)
                        .value_or(choc::value::createObject({}));
  if (descriptor->containerKey.empty()) {
    parameters.setMember(descriptor->field, plain);
  } else {
    const auto existing = parameters[descriptor->containerKey];
    auto array = existing.isArray() ? choc::value::Value(existing)
                                    : choc::value::createEmptyArray();
    while (array.size() <= descriptor->element) {
      array.addArrayElement(descriptor->memberKey.empty()
                                ? choc::value::Value(0.0)
                                : choc::value::createObject({}));
    }
    auto replaced = choc::value::createEmptyArray();
    for (std::uint32_t index = 0; index < array.size(); ++index) {
      if (index != descriptor->element) {
        replaced.addArrayElement(choc::value::Value(array[index]));
      } else if (descriptor->memberKey.empty()) {
        replaced.addArrayElement(plain);
      } else {
        auto object = array[index].isObject() ? choc::value::Value(array[index])
                                              : choc::value::createObject({});
        object.setMember(descriptor->memberKey, plain);
        replaced.addArrayElement(std::move(object));
      }
    }
    parameters.setMember(descriptor->containerKey, std::move(replaced));
  }
  plugin->parametersJson = choc::json::toString(parameters);
  return true;
}

#if defined(EFFETUNE_AUTOMATION_HOST_GATE_FIXTURE)
PluginStateDocument automationHostGateFixtureDocument() {
  PluginStateDocument document;
  document.pipelineA.plugins = {
      PluginState{1001u, "DC Offset Gate A1", true, 0, 0, std::nullopt,
                  R"({"of":-0.25})", false,
                  R"({"type":"DCOffsetPlugin"})"},
      PluginState{1002u, "DC Offset Gate A2", true, 0, 0, std::nullopt,
                  R"({"of":0.25})", false,
                  R"({"type":"DCOffsetPlugin"})"}};
  document.pipelineB.plugins = {
      PluginState{1003u, "DC Offset Gate B1", true, 0, 0, std::nullopt,
                  R"({"of":0.5})", false,
                  R"({"type":"DCOffsetPlugin"})"}};
  document.pipelineBInitialized = true;
  return document;
}
#endif

} // namespace effetune::vst
