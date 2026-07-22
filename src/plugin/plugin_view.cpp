#include "plugin_view.h"

#include "editor_size.h"
#include "plugin_processor.h"

#include "pluginterfaces/gui/iplugview.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace effetune::vst::plugin {

using namespace Steinberg;

IPlugView *EffeTuneView::create(EffeTuneProcessor *processor) {
  return new EffeTuneView(processor);
}

EffeTuneView::EffeTuneView(EffeTuneProcessor *processor) : processor_(processor) {
  setRect(ViewRect(0, 0, kDefaultEditorWidth,
                   kDefaultEditorHeight));
}

EffeTuneView::~EffeTuneView() = default;

tresult PLUGIN_API EffeTuneView::isPlatformTypeSupported(const FIDString type) {
#if defined(_WIN32)
  return type != nullptr && std::strcmp(type, kPlatformTypeHWND) == 0 ? kResultTrue
                                                                      : kResultFalse;
#elif defined(__APPLE__)
  return type != nullptr && std::strcmp(type, kPlatformTypeNSView) == 0 ? kResultTrue
                                                                        : kResultFalse;
#else
  (void)type;
  return kResultFalse;
#endif
}

tresult PLUGIN_API EffeTuneView::attached(void *parent, const FIDString type) {
  if (parent == nullptr || isPlatformTypeSupported(type) != kResultTrue || processor_ == nullptr) {
    return kInvalidArgument;
  }
  const auto result = CPluginView::attached(parent, type);
  if (result != kResultOk) {
    return result;
  }
  if (!processor_->attachEditor(this, parent, rect.getWidth(), rect.getHeight())) {
    (void)CPluginView::removed();
    return kResultFalse;
  }
  return kResultOk;
}

tresult PLUGIN_API EffeTuneView::removed() {
  if (processor_ != nullptr) {
    processor_->detachEditor(this);
  }
  return CPluginView::removed();
}

tresult PLUGIN_API EffeTuneView::onSize(ViewRect *newSize) {
  if (newSize == nullptr) {
    return kInvalidArgument;
  }
  const auto result = CPluginView::onSize(newSize);
  if (processor_ != nullptr) {
    processor_->resizeEditor(this, newSize->getWidth(), newSize->getHeight());
  }
  return result;
}

tresult PLUGIN_API EffeTuneView::canResize() { return kResultTrue; }

tresult PLUGIN_API EffeTuneView::checkSizeConstraint(ViewRect *requested) {
  if (requested == nullptr) {
    return kInvalidArgument;
  }
  const auto minimumWidth = scaleEditorDimension(kMinimumEditorWidth, contentScaleFactor_);
  const auto minimumHeight = scaleEditorDimension(kMinimumEditorHeight, contentScaleFactor_);
  const auto maximumWidth = scaleEditorDimension(kMaximumEditorWidth, contentScaleFactor_);
  const auto maximumHeight = scaleEditorDimension(kMaximumEditorHeight, contentScaleFactor_);
  requested->right = requested->left +
                     std::clamp<int32>(requested->getWidth(), minimumWidth, maximumWidth);
  requested->bottom = requested->top +
                      std::clamp<int32>(requested->getHeight(), minimumHeight, maximumHeight);
  return kResultTrue;
}

#if defined(_WIN32)
tresult PLUGIN_API EffeTuneView::setContentScaleFactor(const ScaleFactor factor) {
  if (!std::isfinite(factor) || factor <= 0.0f) {
    return kInvalidArgument;
  }
  if (factor == contentScaleFactor_) {
    return kResultTrue;
  }

  const auto scaleRatio = static_cast<double>(factor) / contentScaleFactor_;
  auto scaledRect = getRect();
  scaledRect.right = scaledRect.left +
                     scaleEditorDimension(scaledRect.getWidth(), scaleRatio);
  scaledRect.bottom = scaledRect.top +
                      scaleEditorDimension(scaledRect.getHeight(), scaleRatio);
  contentScaleFactor_ = factor;
  setRect(scaledRect);

  if (isAttached() && plugFrame != nullptr) {
    return plugFrame->resizeView(static_cast<IPlugView *>(this), &scaledRect);
  }

  return kResultTrue;
}
#endif

} // namespace effetune::vst::plugin
