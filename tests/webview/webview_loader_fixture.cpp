#include "choc/gui/choc_WebView.h"

#include <utility>

namespace {

int failureStage = 0;
ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *pending = nullptr;
ICoreWebView2CreateCoreWebView2ControllerCompletedHandler *pendingController = nullptr;

class FailingEnvironment final : public ICoreWebView2Environment {
public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void **object) override {
    if (object != nullptr) *object = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
  ULONG STDMETHODCALLTYPE Release() override { return 1; }
  HRESULT STDMETHODCALLTYPE CreateCoreWebView2Controller(
      HWND, ICoreWebView2CreateCoreWebView2ControllerCompletedHandler *handler) override {
    if (failureStage == 2) return E_ACCESSDENIED;
    pendingController = handler;
    handler->AddRef();
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE CreateWebResourceResponse(
      IStream *, int, LPCWSTR, LPCWSTR, ICoreWebView2WebResourceResponse **) override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE get_BrowserVersionString(LPWSTR *) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE add_NewBrowserVersionAvailable(
      void *, EventRegistrationToken *) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE remove_NewBrowserVersionAvailable(
      EventRegistrationToken) override { return E_NOTIMPL; }
};

FailingEnvironment environment;

} // namespace

extern "C" __declspec(dllexport) void SetFailureStage(const int stage) {
  failureStage = stage;
}

extern "C" __declspec(dllexport) HRESULT CompleteEnvironment() {
  auto *handler = std::exchange(pending, nullptr);
  if (handler == nullptr) return E_UNEXPECTED;
  const auto result = failureStage == 1
                          ? handler->Invoke(E_FAIL, nullptr)
                          : handler->Invoke(S_OK, &environment);
  handler->Release();
  return result;
}

extern "C" __declspec(dllexport) HRESULT CompleteController() {
  auto *handler = std::exchange(pendingController, nullptr);
  if (handler == nullptr) return E_UNEXPECTED;
  const auto result = handler->Invoke(failureStage == 3 ? E_FAIL : S_OK, nullptr);
  handler->Release();
  return result;
}

STDAPI CreateCoreWebView2EnvironmentWithOptions(
    PCWSTR, PCWSTR, void *,
    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *handler) {
  if (failureStage == 0) return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
  if (pending != nullptr) return E_UNEXPECTED;
  pending = handler;
  handler->AddRef();
  return S_OK;
}
