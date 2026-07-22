#include "plugin_ids.h"
#include "plugin_processor.h"
#include "version.h"

#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "public.sdk/source/main/pluginfactory.h"

using namespace Steinberg;
using namespace Steinberg::Vst;

BEGIN_FACTORY_DEF("Frieve", "https://github.com/Frieve-A/effetune", "")

DEF_CLASS2(INLINE_UID_FROM_FUID(effetune::vst::plugin::kProcessorId),
           PClassInfo::kManyInstances, kVstAudioEffectClass, "EffeTune Mixwright", 0, "Fx",
           EFFETUNE_PLUGIN_VERSION_STR, kVstVersionString,
           effetune::vst::plugin::EffeTuneProcessor::createInstance)

END_FACTORY
