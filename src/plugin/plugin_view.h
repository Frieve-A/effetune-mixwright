#pragma once

#include "public.sdk/source/common/pluginview.h"

#if defined(_WIN32)
#include "pluginterfaces/gui/iplugviewcontentscalesupport.h"
#endif

namespace effetune::vst::plugin {

class EffeTuneProcessor;

class EffeTuneView final : public Steinberg::CPluginView
#if defined(_WIN32)
                         , public Steinberg::IPlugViewContentScaleSupport
#endif
{
public:
  [[nodiscard]] static Steinberg::IPlugView *create(EffeTuneProcessor *processor);

  ~EffeTuneView() override;

  Steinberg::tresult PLUGIN_API isPlatformTypeSupported(
      Steinberg::FIDString type) SMTG_OVERRIDE;
  Steinberg::tresult PLUGIN_API attached(void *parent,
                                         Steinberg::FIDString type) SMTG_OVERRIDE;
  Steinberg::tresult PLUGIN_API removed() SMTG_OVERRIDE;
  Steinberg::tresult PLUGIN_API onSize(Steinberg::ViewRect *newSize) SMTG_OVERRIDE;
  Steinberg::tresult PLUGIN_API canResize() SMTG_OVERRIDE;
  Steinberg::tresult PLUGIN_API checkSizeConstraint(
      Steinberg::ViewRect *rect) SMTG_OVERRIDE;

#if defined(_WIN32)
  Steinberg::tresult PLUGIN_API setContentScaleFactor(
      Steinberg::IPlugViewContentScaleSupport::ScaleFactor factor) SMTG_OVERRIDE;

  OBJ_METHODS(EffeTuneView, Steinberg::CPluginView)
  DEFINE_INTERFACES
    DEF_INTERFACE(Steinberg::IPlugViewContentScaleSupport)
  END_DEFINE_INTERFACES(Steinberg::CPluginView)
  REFCOUNT_METHODS(Steinberg::CPluginView)
#endif

private:
  explicit EffeTuneView(EffeTuneProcessor *processor);

  EffeTuneProcessor *processor_ = nullptr;
  float contentScaleFactor_ = 1.0f;
};

} // namespace effetune::vst::plugin
