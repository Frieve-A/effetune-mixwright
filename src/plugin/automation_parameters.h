#pragma once

#include "engine/automation_binding.h"

#include "public.sdk/source/vst/vstparameters.h"

#include <optional>
#include <vector>

namespace effetune::vst::plugin {

class AutomationParameterBank {
public:
  [[nodiscard]] bool registerParameters(Steinberg::Vst::ParameterContainer &parameters);
  // Projects the registry onto the parameter bank. Reports whether any slot's
  // metadata -- its title, unit, step count, default or automatability --
  // changed, which is the only thing a host has to re-enumerate for. A slot
  // whose value alone moved is published like any other and reported as no
  // change: it is what every knob movement does, and answering it would cost a
  // full 257-parameter cache invalidation each time.
  [[nodiscard]] bool apply(const AutomationBindingRegistry &bindings);
  void setHostAdoptedValue(std::uint32_t slot, double normalized) noexcept;

private:
  std::vector<Steinberg::Vst::Parameter *> parameters_ =
      std::vector<Steinberg::Vst::Parameter *>(kAutomationSlotCount, nullptr);
  std::vector<std::optional<AutomationTargetDescriptor>> metadata_ =
      std::vector<std::optional<AutomationTargetDescriptor>>(kAutomationSlotCount);
};

} // namespace effetune::vst::plugin
