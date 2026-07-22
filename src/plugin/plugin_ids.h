#pragma once

#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/vst/vsttypes.h"

namespace effetune::vst::plugin {

inline const Steinberg::FUID kProcessorId(0x72A0F755, 0xE18B4F5E, 0xA1D2F16B, 0x25BB0419);
inline constexpr Steinberg::Vst::ParamID kBypassParameterId = 0;

} // namespace effetune::vst::plugin
