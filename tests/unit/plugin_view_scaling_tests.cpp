#include "plugin/editor_size.h"
#include "plugin/plugin_ids.h"

#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/gui/iplugviewcontentscalesupport.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"

#include <windows.h>

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

using namespace Steinberg;
using namespace Steinberg::Vst;
using namespace effetune::vst::plugin;

void expect(const bool condition, const std::string &message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

LRESULT CALLBACK testWindowProcedure(HWND window, const UINT message,
                                     const WPARAM wParam, const LPARAM lParam) {
  return DefWindowProcW(window, message, wParam, lParam);
}

class TestWindow final {
public:
  TestWindow() {
    const auto instance = GetModuleHandleW(nullptr);
    constexpr wchar_t className[] = L"EffeTunePluginViewScalingParent";
    WNDCLASSW windowClass{};
    windowClass.hInstance = instance;
    windowClass.lpfnWndProc = testWindowProcedure;
    windowClass.lpszClassName = className;
    if (RegisterClassW(&windowClass) == 0 &&
        GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
      throw std::runtime_error("Unable to register the test parent window class");
    }
    window_ = CreateWindowExW(0, className, L"EffeTune View Scaling Test",
                              WS_POPUP, -30000, -30000, 3000, 1800, nullptr,
                              nullptr, instance, nullptr);
    if (window_ == nullptr) {
      throw std::runtime_error("Unable to create the test parent window");
    }
  }

  ~TestWindow() {
    destroy();
  }

  TestWindow(const TestWindow &) = delete;
  TestWindow &operator=(const TestWindow &) = delete;

  [[nodiscard]] HWND get() const noexcept { return window_; }

  void destroy() noexcept {
    if (window_ != nullptr) {
      DestroyWindow(window_);
      window_ = nullptr;
    }
  }

private:
  HWND window_ = nullptr;
};

void pumpMessages(const std::chrono::milliseconds duration) {
  const auto deadline = std::chrono::steady_clock::now() + duration;
  do {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != 0) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  } while (std::chrono::steady_clock::now() < deadline);
}

class TestPlugFrame final : public IPlugFrame {
public:
  tresult PLUGIN_API resizeView(IPlugView *view, ViewRect *newSize) override {
    if (view == nullptr || newSize == nullptr) {
      return kInvalidArgument;
    }
    ++resizeCalls;
    requestedSize = *newSize;
    return view->onSize(newSize);
  }

  tresult PLUGIN_API queryInterface(const TUID interfaceId, void **object) override {
    if (object == nullptr) {
      return kInvalidArgument;
    }
    if (FUnknownPrivate::iidEqual(interfaceId, FUnknown::iid) ||
        FUnknownPrivate::iidEqual(interfaceId, IPlugFrame::iid)) {
      *object = static_cast<IPlugFrame *>(this);
      addRef();
      return kResultTrue;
    }
    *object = nullptr;
    return kNoInterface;
  }

  uint32 PLUGIN_API addRef() override { return 1; }
  uint32 PLUGIN_API release() override { return 1; }

  int resizeCalls = 0;
  ViewRect requestedSize{};
};

class LoadedPlugin final {
public:
  explicit LoadedPlugin(const char *path) {
    module_ = LoadLibraryA(path);
    if (module_ == nullptr) {
      throw std::runtime_error("Unable to load the VST3 module");
    }

    const auto init = reinterpret_cast<ModuleFunction>(GetProcAddress(module_, "InitDll"));
    if (init != nullptr && !init()) {
      throw std::runtime_error("The VST3 InitDll entry point failed");
    }
    initialized_ = true;

    const auto getFactory = reinterpret_cast<GetFactoryProc>(
        GetProcAddress(module_, "GetPluginFactory"));
    if (getFactory == nullptr) {
      throw std::runtime_error("The VST3 factory entry point is missing");
    }
    factory_ = getFactory();
    if (factory_ == nullptr) {
      throw std::runtime_error("The VST3 factory could not be created");
    }
  }

  ~LoadedPlugin() {
    if (factory_ != nullptr) {
      factory_->release();
    }
    if (initialized_) {
      const auto exit = reinterpret_cast<ModuleFunction>(GetProcAddress(module_, "ExitDll"));
      if (exit != nullptr) {
        (void)exit();
      }
    }
    if (module_ != nullptr) {
      FreeLibrary(module_);
    }
  }

  LoadedPlugin(const LoadedPlugin &) = delete;
  LoadedPlugin &operator=(const LoadedPlugin &) = delete;

  [[nodiscard]] IPluginFactory *factory() const noexcept { return factory_; }

private:
  using ModuleFunction = bool(PLUGIN_API *)();

  HMODULE module_ = nullptr;
  IPluginFactory *factory_ = nullptr;
  bool initialized_ = false;
};

void testViewScaling(const char *pluginPath) {
  LoadedPlugin plugin(pluginPath);
  TestWindow parent;

  IEditController *controller = nullptr;
  expect(plugin.factory()->createInstance(
             kProcessorId.toTUID(), IEditController::iid.toTUID(),
             reinterpret_cast<void **>(&controller)) == kResultOk &&
             controller != nullptr,
         "create the edit controller");
  expect(controller->initialize(nullptr) == kResultOk, "initialize the edit controller");

  auto *view = controller->createView(ViewType::kEditor);
  expect(view != nullptr, "create the plug-in view");

  IPlugViewContentScaleSupport *scaleSupport = nullptr;
  expect(view->queryInterface(IPlugViewContentScaleSupport::iid,
                              reinterpret_cast<void **>(&scaleSupport)) == kResultTrue &&
             scaleSupport != nullptr,
         "query IPlugViewContentScaleSupport");

  expect(scaleSupport->setContentScaleFactor(1.5f) == kResultTrue,
         "apply 150% scale before the frame is set");
  ViewRect size{};
  expect(view->getSize(&size) == kResultTrue && size.getWidth() == 2222 &&
             size.getHeight() == 1140,
         "150% initial physical size");

  expect(scaleSupport->setContentScaleFactor(1.0f) == kResultTrue,
         "restore 100% scale before the frame is set");
  expect(view->getSize(&size) == kResultTrue && size.getWidth() == 1481 &&
             size.getHeight() == 760,
         "100% initial physical size");

  TestPlugFrame frame;
  expect(view->setFrame(&frame) == kResultTrue, "set the plug-in frame");
  expect(scaleSupport->setContentScaleFactor(1.5f) == kResultTrue &&
             frame.resizeCalls == 0,
         "do not resize through the host before the view is attached");
  expect(view->getSize(&size) == kResultTrue && size.getWidth() == 2222 &&
             size.getHeight() == 1140,
         "update the pre-attach size without a host callback");

  ViewRect minimum(0, 0, 1, 1);
  expect(view->checkSizeConstraint(&minimum) == kResultTrue &&
             minimum.getWidth() == 1140 && minimum.getHeight() == 720,
         "150% minimum size constraint");
  ViewRect maximum(0, 0, 10000, 10000);
  expect(view->checkSizeConstraint(&maximum) == kResultTrue &&
             maximum.getWidth() == 3840 && maximum.getHeight() == 2400,
         "150% maximum size constraint");

  expect(view->attached(parent.get(), kPlatformTypeHWND) == kResultTrue,
         "attach the plug-in view");
  pumpMessages(std::chrono::milliseconds(100));
  const auto firstWebViewWindow = GetWindow(parent.get(), GW_CHILD);
  expect(firstWebViewWindow != nullptr, "find the first WebView window");

  ViewRect resized(0, 0, 2700, 1350);
  expect(view->onSize(&resized) == kResultTrue, "accept a user resize at 150%");
  expect(scaleSupport->setContentScaleFactor(1.0f) == kResultTrue &&
             frame.resizeCalls == 1 && frame.requestedSize.getWidth() == 1800 &&
             frame.requestedSize.getHeight() == 900,
         "preserve a user-resized logical size at 100%");
  expect(scaleSupport->setContentScaleFactor(1.5f) == kResultTrue &&
             frame.resizeCalls == 2 && frame.requestedSize.getWidth() == 2700 &&
             frame.requestedSize.getHeight() == 1350,
         "restore the user-resized logical size at 150%");

  expect(view->setFrame(nullptr) == kResultTrue, "clear the plug-in frame");
  expect(view->removed() == kResultTrue, "remove the plug-in view");
  pumpMessages(std::chrono::milliseconds(100));
  expect(IsWindow(firstWebViewWindow) == FALSE,
         "release the first WebView window");
  parent.destroy();

  TestWindow reopenedParent;
  expect(view->setFrame(&frame) == kResultTrue,
         "restore the plug-in frame for the second open");
  expect(view->attached(reopenedParent.get(), kPlatformTypeHWND) == kResultTrue,
         "attach the plug-in view a second time");
  pumpMessages(std::chrono::milliseconds(100));
  expect(GetWindow(reopenedParent.get(), GW_CHILD) != nullptr,
         "find the second WebView window");
  expect(view->setFrame(nullptr) == kResultTrue,
         "clear the plug-in frame after the second open");
  expect(view->removed() == kResultTrue,
         "remove the plug-in view a second time");
  pumpMessages(std::chrono::milliseconds(500));
  scaleSupport->release();
  view->release();
  expect(controller->terminate() == kResultOk, "terminate the edit controller");
  controller->release();
  pumpMessages(std::chrono::milliseconds(500));
}

} // namespace

int main(const int argc, char **argv) {
  const auto errorMode = SetErrorMode(0);
  SetErrorMode(errorMode | SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
  try {
    expect(argc == 2, "expected the VST3 module path");
    testViewScaling(argv[1]);
    std::cout << "EffeTune VST view scaling tests passed\n";
    return 0;
  } catch (const std::exception &exception) {
    std::cerr << "Test failure: " << exception.what() << '\n';
    return 1;
  }
}
