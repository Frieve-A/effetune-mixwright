#define CHOC_FIND_WEBVIEW2LOADER_DLL \
  choc::file::DynamicLibrary(EFFETUNE_WEBVIEW_LOADER_FIXTURE)
#include "choc/gui/choc_WebView.h"

#include <iostream>
#include <stdexcept>
#include <string>

#include "../support/crt_dialog_suppression.h"

namespace {

void expect(const bool condition, const std::string &message) {
  if (!condition) throw std::runtime_error(message);
}

} // namespace

int main() {
  effetune::vst::testing::suppressCrtModalDialogs();
  try {
    choc::file::DynamicLibrary loader(EFFETUNE_WEBVIEW_LOADER_FIXTURE);
    const auto setStage = reinterpret_cast<void (*)(int)>(loader.findFunction("SetFailureStage"));
    const auto complete = reinterpret_cast<HRESULT (*)()>(loader.findFunction("CompleteEnvironment"));
    const auto completeController = reinterpret_cast<HRESULT (*)()>(loader.findFunction("CompleteController"));
    expect(setStage != nullptr && complete != nullptr && completeController != nullptr,
           "load the WebView2 API fixture");

    for (int stage = 0; stage < 5; ++stage) {
      setStage(stage);
      int errors = 0;
      int ready = 0;
      choc::ui::WebView::Options options;
      options.webviewInitialisationError = [&](const std::string &error) {
        expect(!error.empty(), "preserve the native creation diagnostic");
        ++errors;
      };
      options.webviewIsReady = [&](choc::ui::WebView &) { ++ready; };
      choc::ui::WebView view(options);
      if (stage == 0) {
        expect(!view.loadedOK() && errors == 1, "report a synchronous loader failure");
      } else {
        expect(view.loadedOK() && !view.isReady() && errors == 0,
               "keep a pending creation distinct from a failure");
        (void)complete();
        if (stage >= 3) {
          expect(errors == 0 && !view.isReady(), "wait for controller completion");
          (void)completeController();
        }
        expect(errors == 1, "report environment or controller failure exactly once");
      }
      expect(!view.isReady() && ready == 0, "a failed creation must never report ready");
    }

    setStage(1);
    int retiredErrors = 0;
    {
      choc::ui::WebView::Options options;
      options.webviewInitialisationError = [&](const std::string &) { ++retiredErrors; };
      choc::ui::WebView retired(options);
      expect(retired.loadedOK(), "start a creation that outlives its WebView");
    }
    (void)complete();
    expect(retiredErrors == 0, "ignore completion after the WebView is destroyed");
    setStage(3);
    {
      choc::ui::WebView::Options options;
      options.webviewInitialisationError = [&](const std::string &) { ++retiredErrors; };
      choc::ui::WebView retired(options);
      (void)complete();
    }
    (void)completeController();
    expect(retiredErrors == 0, "ignore controller completion after destruction");
    std::cout << "WebView2 loader, environment, controller, and retired callbacks verified\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
