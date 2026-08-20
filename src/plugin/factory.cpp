#include "plugin_ids.h"
#include "plugin_processor.h"
#include "version.h"

#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "public.sdk/source/main/pluginfactory.h"

using namespace Steinberg;
using namespace Steinberg::Vst;

#if defined(EFFETUNE_AUTOMATION_HOST_GATE_FIXTURE)
constexpr auto kPluginDisplayName = "EffeTune Mixwright Automation Host Gate";
#else
constexpr auto kPluginDisplayName = "EffeTune Mixwright";
#endif

BEGIN_FACTORY_DEF("Frieve", "https://github.com/Frieve-A/effetune", "")

DEF_CLASS2(INLINE_UID_FROM_FUID(effetune::vst::plugin::kProcessorId),
           PClassInfo::kManyInstances, kVstAudioEffectClass, kPluginDisplayName, 0, "Fx",
           EFFETUNE_PLUGIN_VERSION_STR, kVstVersionString,
           effetune::vst::plugin::EffeTuneProcessor::createInstance)

END_FACTORY
