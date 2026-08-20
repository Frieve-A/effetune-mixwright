#include "plugin/automation_parameters.h"

#include "plugin/plugin_ids.h"

#include "base/source/fstring.h"
#include "pluginterfaces/base/ustring.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>

namespace effetune::vst::plugin {
namespace {

using Steinberg::String;
using Steinberg::UString;
using Steinberg::Vst::ParameterInfo;

void assignUtf8(Steinberg::Vst::String128 &destination,
                const std::string &value) {
  String converted;
  converted.fromUTF8(value.c_str());
  UString(destination, 128).assign(converted.text16());
}

// Unbound slots stay automatable so a host never has to re-enumerate parameter
// flags for automation to work. Only the displayed name changes on binding.
[[nodiscard]] ParameterInfo inactiveInfo(const std::uint32_t slot) {
  ParameterInfo info{};
  char title[32]{};
  (void)std::snprintf(title, sizeof(title), "Automation %03u", slot + 1u);
  UString(info.title, 128).fromAscii(title);
  UString(info.shortTitle, 128).fromAscii(title);
  info.id = automationParameterId(slot);
  info.stepCount = 0;
  info.defaultNormalizedValue = 0.0;
  info.unitId = Steinberg::Vst::kRootUnitId;
  info.flags = ParameterInfo::kCanAutomate;
  return info;
}

void applyActiveInfo(ParameterInfo &info, const AutomationTargetDescriptor &metadata) {
  std::memset(info.title, 0, sizeof(info.title));
  std::memset(info.shortTitle, 0, sizeof(info.shortTitle));
  std::memset(info.units, 0, sizeof(info.units));
  assignUtf8(info.title, metadata.title);
  assignUtf8(info.shortTitle,
             metadata.shortTitle.empty() ? metadata.title : metadata.shortTitle);
  assignUtf8(info.units, metadata.units);
  info.stepCount = metadata.stepCount;
  info.defaultNormalizedValue = metadata.defaultNormalized;
  info.flags = ParameterInfo::kCanAutomate;
  // An enumeration-backed slot has named positions and nothing between them, so
  // the host is told to draw it as a list rather than as a continuous control:
  // "Parameter should be displayed as list in generic editor or automation
  // editing" (ivsteditcontroller.h). Only the enumeration normalization earns
  // it -- an integer slot is a number the user may usefully type or sweep.
  if (metadata.normalization == AutomationValueNormalization::enumeration) {
    info.flags |= ParameterInfo::kIsList;
  }
}

} // namespace

bool AutomationParameterBank::registerParameters(
    Steinberg::Vst::ParameterContainer &parameters) {
  // The container owns every Parameter, so a terminate()/initialize() cycle
  // destroys the objects this bank points at. Re-arm the bank together with the
  // fresh container: the cached metadata describes parameters that no longer
  // exist, and keeping it would make apply() believe the newly created
  // placeholders already carry the active title and value.
  std::fill(parameters_.begin(), parameters_.end(), nullptr);
  std::fill(metadata_.begin(), metadata_.end(), std::nullopt);
  for (std::uint32_t slot = 0; slot < kAutomationSlotCount; ++slot) {
    if (parameters.getParameter(automationParameterId(slot)) != nullptr) {
      return false;
    }
    parameters_[slot] = parameters.addParameter(inactiveInfo(slot));
    if (parameters_[slot] == nullptr) {
      return false;
    }
  }
  return true;
}

bool AutomationParameterBank::apply(const AutomationBindingRegistry &bindings) {
  auto changed = false;
  for (std::uint32_t slot = 0; slot < kAutomationSlotCount; ++slot) {
    auto *parameter = parameters_[slot];
    if (parameter == nullptr) {
      continue;
    }
    const auto *metadata = bindings.slot(slot);
    if (metadata == nullptr) {
      if (metadata_[slot].has_value()) {
        parameter->getInfo() = inactiveInfo(slot);
        (void)parameter->setNormalized(0.0);
        metadata_[slot].reset();
        changed = true;
      }
      continue;
    }
    // The descriptor carries the live value as well as the metadata, and only a
    // metadata difference is worth reporting: the caller answers a true here by
    // invalidating all 257 parameters through
    // restartComponent(kParamTitlesChanged | kParamValuesChanged), which a knob
    // that only moved has not earned. Comparing against a copy that carries the
    // cached value, rather than listing the metadata members, keeps a member
    // added to the descriptor later inside the comparison by default.
    auto metadataDiffers = true;
    if (metadata_[slot].has_value()) {
      auto comparable = *metadata;
      comparable.currentNormalized = metadata_[slot]->currentNormalized;
      metadataDiffers = *metadata_[slot] != comparable;
    }
    if (metadataDiffers) {
      applyActiveInfo(parameter->getInfo(), *metadata);
      changed = true;
    }
    // The value is republished either way, exactly as it was before the
    // comparison above was narrowed: only what is reported to the caller
    // changed, never what the parameter holds.
    (void)parameter->setNormalized(metadata->currentNormalized);
    metadata_[slot] = *metadata;
  }
  return changed;
}

void AutomationParameterBank::setHostAdoptedValue(const std::uint32_t slot,
                                                  const double normalized) noexcept {
  if (slot >= parameters_.size() || parameters_[slot] == nullptr ||
      !metadata_[slot].has_value() || !std::isfinite(normalized)) {
    return;
  }
  const auto adopted = std::clamp(normalized, 0.0, 1.0);
  (void)parameters_[slot]->setNormalized(adopted);
  metadata_[slot]->currentNormalized = adopted;
}

} // namespace effetune::vst::plugin
