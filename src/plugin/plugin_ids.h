#pragma once

#include "engine/automation_binding.h"

#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/vst/vsttypes.h"

namespace effetune::vst::plugin {

#if defined(EFFETUNE_AUTOMATION_HOST_GATE_FIXTURE)
inline const Steinberg::FUID kProcessorId(0xA74D1C62, 0x39E84751, 0xB6126D8F,
                                         0x2C45E093);
#else
inline const Steinberg::FUID kProcessorId(0x72A0F755, 0xE18B4F5E, 0xA1D2F16B,
                                         0x25BB0419);
#endif
inline constexpr Steinberg::Vst::ParamID kBypassParameterId = 0;
inline constexpr Steinberg::Vst::ParamID kFirstAutomationParameterId = 0x00010000u;
inline constexpr Steinberg::Vst::ParamID kLastAutomationParameterId =
    kFirstAutomationParameterId + kAutomationSlotCount - 1u;

[[nodiscard]] inline constexpr Steinberg::Vst::ParamID
automationParameterId(const std::uint32_t slot) noexcept {
  return kFirstAutomationParameterId + slot;
}

} // namespace effetune::vst::plugin
