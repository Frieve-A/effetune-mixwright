#include "engine/automation_binding.h"

#include "choc/text/choc_JSON.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <utility>

namespace effetune::vst {
namespace {

[[nodiscard]] AutomationTargetIdentity identityOf(const AutomationBindingState &binding) {
  return {binding.pipeline, binding.pluginId, binding.pluginType,
          binding.parameterKey, binding.elementIndex};
}

[[nodiscard]] bool validIdentity(const AutomationTargetIdentity &identity) noexcept {
  return (identity.pipeline == 'A' || identity.pipeline == 'B') && identity.pluginId != 0 &&
         !identity.pluginType.empty() && !identity.parameterKey.empty();
}

[[nodiscard]] bool validTarget(const AutomationTargetDescriptor &target) noexcept {
  return validIdentity(target.identity) && target.stepCount >= 0 &&
         std::isfinite(target.defaultNormalized) && target.defaultNormalized >= 0.0 &&
         target.defaultNormalized <= 1.0 && std::isfinite(target.currentNormalized) &&
         target.currentNormalized >= 0.0 && target.currentNormalized <= 1.0;
}

[[nodiscard]] bool pluginExists(const PluginStateDocument &document, const char pipeline,
                                const std::uint32_t pluginId) noexcept {
  const auto &plugins = pipeline == 'B' ? document.pipelineB.plugins
                                        : document.pipelineA.plugins;
  return std::any_of(plugins.begin(), plugins.end(), [pluginId](const PluginState &plugin) {
    return plugin.id == pluginId;
  });
}

[[nodiscard]] const AutomationTargetDescriptor *findTarget(
    const std::span<const AutomationTargetDescriptor> targets,
    const AutomationTargetIdentity &identity) noexcept {
  const auto found = std::find_if(targets.begin(), targets.end(),
                                  [&identity](const AutomationTargetDescriptor &target) {
                                    return validTarget(target) && target.identity == identity;
                                  });
  return found == targets.end() ? nullptr : &*found;
}

// Compares the identity fields in place. Materialising an
// AutomationTargetIdentity here would copy two std::strings per candidate, and
// the duplicate scan below is quadratic in the binding count while the caller
// holds the mutex that serializes every other control thread.
[[nodiscard]] bool sameTargetIdentity(const AutomationBindingState &left,
                                      const AutomationBindingState &right) noexcept {
  return left.pipeline == right.pipeline && left.pluginId == right.pluginId &&
         left.elementIndex == right.elementIndex && left.pluginType == right.pluginType &&
         left.parameterKey == right.parameterKey;
}

[[nodiscard]] bool hasEarlierTargetBinding(
    const std::vector<AutomationBindingState> &bindings, const std::size_t index) noexcept {
  const auto &binding = bindings[index];
  for (std::size_t candidate = 0; candidate < bindings.size(); ++candidate) {
    if (candidate == index || !sameTargetIdentity(bindings[candidate], binding)) {
      continue;
    }
    if (bindings[candidate].slot < binding.slot ||
        (bindings[candidate].slot == binding.slot && candidate < index)) {
      return true;
    }
  }
  return false;
}

[[nodiscard]] std::string pluginTypeOf(const PluginState &plugin) {
  try {
    const auto extra = choc::json::parse(plugin.extraJson);
    if (extra.isObject()) {
      return extra["type"].getWithDefault<std::string>({});
    }
  } catch (const choc::json::ParseError &) {
  }
  return {};
}

// One synthetic boolean target per instance. It is deliberately independent of
// the effect type so no upstream registry has to be duplicated here.
void appendNodeEnableTargets(std::vector<AutomationTargetDescriptor> &targets,
                             const PipelineState &pipeline, const char side) {
  for (const auto &plugin : pipeline.plugins) {
    if (plugin.unknown || plugin.id == 0) {
      continue;
    }
    auto type = pluginTypeOf(plugin);
    if (type.empty()) {
      continue;
    }
    AutomationTargetDescriptor target;
    target.identity = {side, plugin.id, std::move(type),
                       std::string(kNodeEnableParameterKey), 0};
    target.title = std::string(1, side) + " #" + std::to_string(plugin.id) +
                   " enabled[1] - " + plugin.name + " - Enabled";
    target.shortTitle = target.title;
    target.stepCount = 1;
    target.defaultNormalized = 1.0;
    target.currentNormalized = plugin.enabled ? 1.0 : 0.0;
    target.applyKind = AutomationApplyKind::nodeEnable;
    target.normalization = AutomationValueNormalization::boolean;
    target.continuous = false;
    targets.push_back(std::move(target));
  }
}

[[nodiscard]] std::uint32_t maximumLogicalPluginId(const PluginStateDocument &document) noexcept {
  auto maximum = document.automation.logicalPluginIdWatermark;
  for (const auto *pipeline : {&document.pipelineA, &document.pipelineB}) {
    for (const auto &plugin : pipeline->plugins) {
      maximum = std::max(maximum, plugin.id);
    }
  }
  for (const auto &binding : document.automation.bindings) {
    maximum = std::max(maximum, binding.pluginId);
  }
  return maximum;
}

} // namespace

AutomationReconcileResult AutomationBindingRegistry::reconcile(
    PluginStateDocument &document,
    const std::span<const AutomationTargetDescriptor> eligibleTargets) {
  AutomationReconcileResult result;
  const auto previousSlots = slots_;
  for (auto &slot : slots_) {
    slot.reset();
  }
  for (auto &binding : slotBindings_) {
    binding.reset();
  }
  activeSlots_.clear();

  if (!document.automation.initialized) {
    document.automation.initialized = true;
    result.stateChanged = true;
  }
  const auto watermark = maximumLogicalPluginId(document);
  if (document.automation.logicalPluginIdWatermark != watermark) {
    document.automation.logicalPluginIdWatermark = watermark;
    result.stateChanged = true;
  }

  for (std::size_t index = 0; index < document.automation.bindings.size(); ++index) {
    auto &binding = document.automation.bindings[index];
    const auto identity = identityOf(binding);
    const auto *target = findTarget(eligibleTargets, identity);
    const auto targetIsDuplicate = hasEarlierTargetBinding(document.automation.bindings, index);
    const auto slotConflict = binding.slot < kAutomationSlotCount &&
                              slots_[binding.slot].has_value();
    auto lifecycle = AutomationBindingLifecycle::dormant;
    if (!pluginExists(document, binding.pipeline, binding.pluginId)) {
      lifecycle = AutomationBindingLifecycle::tombstone;
    } else if (target != nullptr && !targetIsDuplicate && !slotConflict &&
               binding.slot < kAutomationSlotCount) {
      lifecycle = AutomationBindingLifecycle::active;
      slots_[binding.slot] = *target;
    }
    if (binding.lifecycle != lifecycle) {
      binding.lifecycle = lifecycle;
      result.stateChanged = true;
    }
    if (lifecycle == AutomationBindingLifecycle::active) {
      slotBindings_[binding.slot] = binding;
    }
  }

  activeSlots_.reserve(kAutomationSlotCount);
  for (std::uint32_t slot = 0; slot < kAutomationSlotCount; ++slot) {
    if (slots_[slot].has_value()) {
      activeSlots_.push_back(slot);
    }
  }

  result.slotsChanged = previousSlots != slots_;
  return result;
}

const AutomationTargetDescriptor *
AutomationBindingRegistry::slot(const std::uint32_t slotIndex) const noexcept {
  return slotIndex < slots_.size() && slots_[slotIndex].has_value()
             ? &*slots_[slotIndex]
             : nullptr;
}

const AutomationBindingState *
AutomationBindingRegistry::binding(const std::uint32_t slotIndex) const noexcept {
  return slotIndex < slotBindings_.size() && slotBindings_[slotIndex].has_value()
             ? &*slotBindings_[slotIndex]
             : nullptr;
}

std::optional<std::uint32_t> AutomationBindingRegistry::findActiveSlot(
    const AutomationTargetIdentity &identity) const noexcept {
  for (std::uint32_t slotIndex = 0; slotIndex < slots_.size(); ++slotIndex) {
    if (slots_[slotIndex].has_value() && slots_[slotIndex]->identity == identity) {
      return slotIndex;
    }
  }
  return std::nullopt;
}

std::optional<std::uint32_t> bindAutomationTarget(
    PluginStateDocument &document,
    const std::span<const AutomationTargetDescriptor> eligibleTargets,
    const AutomationTargetIdentity &identity, bool &capacityExhausted) {
  capacityExhausted = false;
  if (findTarget(eligibleTargets, identity) == nullptr ||
      !pluginExists(document, identity.pipeline, identity.pluginId)) {
    return std::nullopt;
  }
  const auto existing = std::find_if(
      document.automation.bindings.begin(), document.automation.bindings.end(),
      [&identity](const AutomationBindingState &binding) {
        return identityOf(binding) == identity;
      });
  if (existing != document.automation.bindings.end()) {
    return existing->slot < kAutomationSlotCount
               ? std::optional<std::uint32_t>{existing->slot}
               : std::nullopt;
  }

  std::array<bool, kAutomationSlotCount> historicallyUsed{};
  for (const auto &binding : document.automation.bindings) {
    if (binding.slot < kAutomationSlotCount) {
      historicallyUsed[binding.slot] = true;
    }
  }
  const auto freeSlot = std::find(historicallyUsed.begin(), historicallyUsed.end(), false);
  if (freeSlot == historicallyUsed.end()) {
    capacityExhausted = true;
    return std::nullopt;
  }
  const auto slotIndex = static_cast<std::uint32_t>(freeSlot - historicallyUsed.begin());
  document.automation.bindings.push_back(
      {slotIndex, identity.pipeline, identity.pluginId, identity.pluginType,
       identity.parameterKey, identity.elementIndex,
       AutomationBindingLifecycle::active, "{}"});
  return slotIndex;
}

std::vector<AutomationTargetDescriptor>
eligibleAutomationTargets(const PluginStateDocument &document) {
  auto targets = generatedAutomationTargets(document);
  appendNodeEnableTargets(targets, document.pipelineA, 'A');
  appendNodeEnableTargets(targets, document.pipelineB, 'B');
  return targets;
}

bool applyAutomationValue(PluginStateDocument &document,
                          const AutomationBindingState &binding,
                          const double normalized) {
  if (binding.parameterKey != kNodeEnableParameterKey) {
    return applyAutomationNormalizedValue(document, binding, normalized);
  }
  if (!std::isfinite(normalized)) {
    return false;
  }
  auto &plugins = binding.pipeline == 'B' ? document.pipelineB.plugins
                                          : document.pipelineA.plugins;
  const auto plugin = std::find_if(plugins.begin(), plugins.end(),
                                   [&binding](const PluginState &candidate) {
                                     return candidate.id == binding.pluginId;
                                   });
  if (plugin == plugins.end()) {
    return false;
  }
  plugin->enabled = normalized >= 0.5;
  return true;
}

void AutomationBindingRegistry::setCurrentNormalized(const std::uint32_t slotIndex,
                                                      const double normalized) noexcept {
  if (slotIndex < slots_.size() && slots_[slotIndex].has_value() &&
      std::isfinite(normalized)) {
    slots_[slotIndex]->currentNormalized = std::clamp(normalized, 0.0, 1.0);
  }
}

} // namespace effetune::vst
