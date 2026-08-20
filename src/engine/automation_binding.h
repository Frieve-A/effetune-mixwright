#pragma once

#include "engine/pipeline_model.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace effetune::vst {

inline constexpr std::uint32_t kAutomationSlotCount = 256;

// Parameter key of the synthetic per-instance enable target. The generated
// catalog never produces this key, so it cannot collide with a packed
// parameter.
inline constexpr std::string_view kNodeEnableParameterKey = "__enabled";

enum class AutomationValueNormalization : std::uint8_t {
  linear,
  logarithmic,
  integer,
  boolean,
  enumeration
};

// How a published value reaches the DSP. Packed parameters are written into the
// runtime parameter image, node-enable targets change the pipeline topology.
enum class AutomationApplyKind : std::uint8_t { packedParameter, nodeEnable };

enum class AutomationValueTransform : std::uint8_t {
  identity,
  naturalLog,
  log10,
  decibelsFromReference
};

struct AutomationTargetIdentity {
  char pipeline = 'A';
  std::uint32_t pluginId = 0;
  std::string pluginType;
  std::string parameterKey;
  std::uint32_t elementIndex = 0;

  [[nodiscard]] bool operator==(const AutomationTargetIdentity &) const = default;
};

struct AutomationTargetDescriptor {
  AutomationTargetIdentity identity;
  std::string title;
  std::string shortTitle;
  std::string units;
  std::int32_t stepCount = 0;
  double defaultNormalized = 0.0;
  double currentNormalized = 0.0;
  // Unused when applyKind is nodeEnable.
  std::uint32_t packedOffset = 0;
  AutomationApplyKind applyKind = AutomationApplyKind::packedParameter;
  AutomationValueNormalization normalization = AutomationValueNormalization::linear;
  AutomationValueTransform transform = AutomationValueTransform::identity;
  double transformReference = 1.0;
  double minimum = 0.0;
  double maximum = 1.0;
  // The granularity the EffeTune window prints this parameter with, in the same
  // units as minimum and maximum. The generated catalog carries it so a host
  // readout shows the digits the plug-in's own UI shows.
  double step = 1.0;
  bool continuous = true;

  [[nodiscard]] bool operator==(const AutomationTargetDescriptor &) const = default;
};

struct AutomationReconcileResult {
  bool stateChanged = false;
  bool slotsChanged = false;
};

class AutomationBindingRegistry {
public:
  // Projects the existing bindings onto the fixed slot bank and refreshes their
  // lifecycle. New bindings are only created through bindTarget().
  [[nodiscard]] AutomationReconcileResult
  reconcile(PluginStateDocument &document,
            std::span<const AutomationTargetDescriptor> eligibleTargets);

  [[nodiscard]] const AutomationTargetDescriptor *slot(std::uint32_t slotIndex) const noexcept;

  [[nodiscard]] const AutomationBindingState *binding(std::uint32_t slotIndex) const noexcept;
  [[nodiscard]] std::optional<std::uint32_t>
  findActiveSlot(const AutomationTargetIdentity &identity) const noexcept;
  void setCurrentNormalized(std::uint32_t slotIndex, double normalized) noexcept;
  [[nodiscard]] std::span<const std::uint32_t> activeSlots() const noexcept {
    return activeSlots_;
  }

private:
  std::vector<std::optional<AutomationTargetDescriptor>> slots_ =
      std::vector<std::optional<AutomationTargetDescriptor>>(kAutomationSlotCount);
  std::vector<std::optional<AutomationBindingState>> slotBindings_ =
      std::vector<std::optional<AutomationBindingState>>(kAutomationSlotCount);
  std::vector<std::uint32_t> activeSlots_;
};

// Returns the slot already bound to the identity, or reserves the lowest slot
// that no binding has ever occupied. Tombstoned slots are never reused so a
// stale host lane cannot drive an unrelated parameter. Returns nullopt when the
// identity is not an eligible target, or when every slot has been used, which
// capacityExhausted reports. The caller must reconcile() to publish the change.
[[nodiscard]] std::optional<std::uint32_t>
bindAutomationTarget(PluginStateDocument &document,
                     std::span<const AutomationTargetDescriptor> eligibleTargets,
                     const AutomationTargetIdentity &identity,
                     bool &capacityExhausted);

[[nodiscard]] std::optional<float>
denormalizeAutomationPackedValue(const AutomationTargetDescriptor &target,
                                 double normalized) noexcept;

// Writes a packed catalog parameter into the plug-in parametersJson.
[[nodiscard]] bool applyAutomationNormalizedValue(PluginStateDocument &document,
                                                  const AutomationBindingState &binding,
                                                  double normalized);

// Applies a published value to the state authority, routing node-enable
// bindings to PluginState::enabled and everything else to the packed catalog.
[[nodiscard]] bool applyAutomationValue(PluginStateDocument &document,
                                        const AutomationBindingState &binding,
                                        double normalized);

// Packed parameters supplied by the generated effect catalog.
[[nodiscard]] std::vector<AutomationTargetDescriptor>
generatedAutomationTargets(const PluginStateDocument &document);

// Every automation target the pipeline exposes: the generated catalog
// parameters plus one synthetic enable target per plug-in instance.
[[nodiscard]] std::vector<AutomationTargetDescriptor>
eligibleAutomationTargets(const PluginStateDocument &document);

#if defined(EFFETUNE_AUTOMATION_HOST_GATE_FIXTURE)
[[nodiscard]] PluginStateDocument automationHostGateFixtureDocument();
#endif

} // namespace effetune::vst
