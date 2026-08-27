#include "bridge/webview_host.h"

#include "choc/gui/choc_WebView.h"
#include "choc/text/choc_JSON.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <fstream>
#include <iterator>
#include <mutex>
#include <new>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <Windows.h>
#elif defined(__APPLE__)
#include <CoreGraphics/CoreGraphics.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <pthread.h>
#endif

namespace effetune::vst {

namespace {

#if defined(_WIN32)
constexpr std::string_view kWebViewHomeUri =
    "https://effetune-mixwright.localhost/";
#else
constexpr std::string_view kWebViewHomeUri =
    "effetune-mixwright://app/";
#endif

[[nodiscard]] std::string lowerExtension(const std::filesystem::path &path) {
  auto extension = path.extension().string();
  std::transform(extension.begin(), extension.end(), extension.begin(),
                 [](const unsigned char value) { return static_cast<char>(std::tolower(value)); });
  return extension;
}

[[nodiscard]] std::string mimeTypeFor(const std::filesystem::path &path) {
  const auto extension = lowerExtension(path);
  if (extension == ".html") return "text/html; charset=utf-8";
  if (extension == ".css") return "text/css; charset=utf-8";
  if (extension == ".js" || extension == ".mjs") return "text/javascript; charset=utf-8";
  if (extension == ".json" || extension == ".json5") return "application/json; charset=utf-8";
  if (extension == ".svg") return "image/svg+xml";
  if (extension == ".png") return "image/png";
  if (extension == ".jpg" || extension == ".jpeg") return "image/jpeg";
  if (extension == ".gif") return "image/gif";
  if (extension == ".ico") return "image/x-icon";
  if (extension == ".wasm") return "application/wasm";
  if (extension == ".txt" || extension == ".effetune_preset") {
    return "text/plain; charset=utf-8";
  }
  return "application/octet-stream";
}

[[nodiscard]] bool containsTraversal(const std::filesystem::path &path) {
  if (path.is_absolute() || path.has_root_name()) {
    return true;
  }
  return std::any_of(path.begin(), path.end(), [](const auto &part) {
    return part == ".." || part == ".";
  });
}

[[nodiscard]] bool hasRequiredResources(const std::filesystem::path &root) {
  constexpr std::array required{
      "effetune.html",
      "effetune.css",
      "vst-bootstrap.js",
      "js/app.js",
  };
  return std::all_of(required.begin(), required.end(), [&root](const auto *relative) {
    std::error_code error;
    return std::filesystem::is_regular_file(root / relative, error);
  });
}

[[nodiscard]] std::string missingResourcePage() {
  return R"(<!doctype html>
<html lang="en" data-effetune-load-error="missing-assets">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>EffeTune Mixwright</title>
<style>
html,body{height:100%;margin:0;background:#1e1e1e;color:#f0f0f0;font:16px/1.5 system-ui,sans-serif}
body{display:grid;place-items:center}
main{max-width:600px;margin:32px;padding:28px;border:1px solid #555;border-radius:8px;background:#292929}
h1{margin:0 0 16px;font-size:22px}
p{margin:0;color:#d0d0d0}
</style>
</head>
<body>
<main>
<h1>EffeTune Mixwright could not load its interface.</h1>
<p>Reinstall the complete EffeTune Mixwright.vst3 bundle, then restart your DAW.</p>
</main>
</body>
</html>)";
}

#if defined(_WIN32)

[[nodiscard]] std::wstring widen(const std::string_view text) {
  if (text.empty()) {
    return {};
  }
  const auto length = MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                          static_cast<int>(text.size()), nullptr, 0);
  if (length <= 0) {
    return {};
  }
  std::wstring wide(static_cast<std::size_t>(length), L'\0');
  if (MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                          wide.data(), length) != length) {
    return {};
  }
  return wide;
}

[[nodiscard]] std::string narrow(const std::wstring_view text) {
  if (text.empty()) {
    return {};
  }
  const auto length = WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                          static_cast<int>(text.size()), nullptr, 0,
                                          nullptr, nullptr);
  if (length <= 0) {
    return {};
  }
  std::string narrowed(static_cast<std::size_t>(length), '\0');
  if (WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                          narrowed.data(), length, nullptr, nullptr) != length) {
    return {};
  }
  return narrowed;
}

[[nodiscard]] std::string hostExecutableName() {
  std::wstring buffer(MAX_PATH, L'\0');
  const auto length = GetModuleFileNameW(nullptr, buffer.data(),
                                         static_cast<DWORD>(buffer.size()));
  if (length == 0 || length >= buffer.size()) {
    return {};
  }
  buffer.resize(length);
  const auto separator = buffer.find_last_of(L'\\');
  return narrow(separator == std::wstring::npos ? buffer : buffer.substr(separator + 1));
}

// WebView2 delivers its environment and controller callbacks through the calling
// thread's single-threaded apartment. When a host has already joined that thread
// to an MTA, CoInitialize() inside CHOC fails with RPC_E_CHANGED_MODE, is
// ignored, and construction then either never completes or completes on an
// arbitrary RPC thread - which is exactly how an editor ends up as a permanently
// empty window in one host while working everywhere else.
[[nodiscard]] bool callerIsInSingleThreadedApartment() {
  // Asking to join the STA is used rather than CoGetApartmentType() because it is
  // the only answer that distinguishes a thread explicitly joined to an MTA -
  // where WebView2 can never run - from one that merely reports MTA implicitly
  // and still joins an STA happily. CHOC performs the identical call moments
  // later, so this changes nothing about the apartment the plug-in ends up in.
  //
  // A thread's apartment is fixed once it has joined one, so the answer is cached
  // per thread. The reference taken on success is deliberately never released:
  // the apartment has to outlive every WebView2 object created on this thread.
  // CHOC leaks one of these per WebView construction already, so caching it per
  // thread here adds strictly less churn than the code it guards.
  static thread_local const bool singleThreaded = [] {
    return SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
  }();
  return singleThreaded;
}

constexpr UINT kWatchdogIntervalMilliseconds = 500;
constexpr UINT kDispatcherMessage = WM_APP + 0x45f;

[[nodiscard]] HFONT createDiagnosticFont() {
  NONCLIENTMETRICSW metrics{};
  metrics.cbSize = sizeof(metrics);
  if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0) ==
      FALSE) {
    return nullptr;
  }
  return CreateFontIndirectW(&metrics.lfMessageFont);
}

#endif // defined(_WIN32)

} // namespace

std::string WebViewHost::diagnosticText(const WebViewStatus status) {
  // CRLF because this text is rendered by a native multi-line EDIT control.
  std::string message =
      "EffeTune Mixwright could not display its interface.\r\n\r\n";
  const char *code = "EFFETUNE-UI-UNKNOWN";
  switch (status) {
  case WebViewStatus::unsupportedApartment:
    message +=
        "This host opened the plug-in editor on a thread that COM has already "
        "joined to a multi-threaded apartment (MTA). Microsoft Edge WebView2, "
        "which draws this interface, only runs on a single-threaded apartment "
        "(STA).\r\n\r\n"
        "Nothing on your machine is broken and reinstalling will not help. "
        "Please report this message together with the name and version of your "
        "host application.";
    code = "EFFETUNE-UI-MTA";
    break;
  case WebViewStatus::runtimeUnavailable:
    message +=
        "The Microsoft Edge WebView2 component could not be started.\r\n\r\n"
        "Install or repair the Microsoft Edge WebView2 Runtime, then restart "
        "your host application.";
    code = "EFFETUNE-UI-RUNTIME";
    break;
  case WebViewStatus::initialisationTimedOut:
    message +=
        "Microsoft Edge WebView2 started but did not finish loading within " +
        std::to_string(kWebViewInitialisationTimeout.count()) +
        " seconds.\r\n\r\n"
        "Close and reopen this window. If the interface still does not appear, "
        "restart your host application and report this message together with "
        "the host name and version.";
    code = "EFFETUNE-UI-TIMEOUT";
    break;
  case WebViewStatus::ready:
  case WebViewStatus::initialising:
    message += "The interface is still starting.";
    code = "EFFETUNE-UI-PENDING";
    break;
  }
  message += "\r\n\r\nDiagnostic code: ";
  message += code;
#if defined(_WIN32)
  if (const auto host = hostExecutableName(); !host.empty()) {
    message += "\r\nHost: " + host;
  }
#endif
  return message;
}

namespace {

[[nodiscard]] std::filesystem::path locateResourceRoot() {
#if defined(_WIN32)
  static int moduleAnchor = 0;
  HMODULE module = nullptr;
  if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                             GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCWSTR>(&moduleAnchor), &module) != 0) {
    std::wstring buffer(32768, L'\0');
    const auto length = GetModuleFileNameW(module, buffer.data(),
                                           static_cast<DWORD>(buffer.size()));
    if (length > 0 && length < buffer.size()) {
      buffer.resize(length);
      return std::filesystem::path(buffer).parent_path().parent_path() / L"Resources" /
             L"webview";
    }
  }
#elif defined(__APPLE__)
  Dl_info info{};
  if (dladdr(reinterpret_cast<const void *>(&locateResourceRoot), &info) != 0 &&
      info.dli_fname != nullptr) {
    return std::filesystem::path(info.dli_fname).parent_path().parent_path() / "Resources" /
           "webview";
  }
#endif
  return std::filesystem::current_path() / "webview-assets";
}

struct WebViewResourceData {
  std::string mimeType;
  std::string bytes;
};

} // namespace

struct WebViewGeneration;

struct WebViewHostState : public std::enable_shared_from_this<WebViewHostState> {
  WebViewHostState(WebViewHost::MessageHandler messageHandler,
                   std::filesystem::path root)
      : handler(std::move(messageHandler)),
        resourceRoot(root.empty() ? locateResourceRoot() : std::move(root)),
        resourceBundleAvailable(hasRequiredResources(resourceRoot)) {}

  [[nodiscard]] std::optional<WebViewResourceData>
  fetchResource(std::string path) const {
    const auto suffix = path.find_first_of("?#");
    if (suffix != std::string::npos) {
      path.resize(suffix);
    }
    while (!path.empty() && path.front() == '/') {
      path.erase(path.begin());
    }
    if (path.empty()) {
      path = "effetune.html";
    }
    if (path.find('\\') != std::string::npos ||
        path.find(':') != std::string::npos) {
      return std::nullopt;
    }
    const std::filesystem::path relative(path);
    if (containsTraversal(relative)) {
      return std::nullopt;
    }
    if (!resourceBundleAvailable) {
      if (path == "effetune.html") {
        return WebViewResourceData{"text/html; charset=utf-8",
                                   missingResourcePage()};
      }
      return std::nullopt;
    }
    const auto candidate = resourceRoot / relative;
    std::error_code error;
    if (!std::filesystem::is_regular_file(candidate, error)) {
      return std::nullopt;
    }
    std::ifstream input(candidate, std::ios::binary);
    if (!input) {
      return std::nullopt;
    }
    WebViewResourceData resource;
    resource.mimeType = mimeTypeFor(candidate);
    resource.bytes.assign(std::istreambuf_iterator<char>(input),
                          std::istreambuf_iterator<char>());
    return resource;
  }

  void initialise(bool createImmediately);
  void shutdown() noexcept;
  [[nodiscard]] bool attach(void *owner, void *parent, std::int32_t width,
                            std::int32_t height);
  void resize(void *owner, std::int32_t width, std::int32_t height) noexcept;
  void detach(void *owner) noexcept;
  void serviceInitialisation() noexcept;
  [[nodiscard]] bool evaluate(std::string script,
                              WebViewHost::EvaluationHandler completion);
  void publishStatus(const WebViewGeneration *source,
                     WebViewStatus newStatus) noexcept;
  void publishLoaded(const WebViewGeneration *source, bool isLoaded) noexcept;

  WebViewHost::MessageHandler handler;
  const std::filesystem::path resourceRoot;
  const bool resourceBundleAvailable;
  std::atomic<WebViewStatus> status{WebViewStatus::initialising};
  std::atomic_bool loaded{false};
  std::atomic_bool stopping{false};
  std::mutex generationMutex;
  std::shared_ptr<WebViewGeneration> generation;
};

namespace {

void configureWebView(choc::ui::WebView &view,
                      const std::weak_ptr<WebViewHostState> &weakState,
                      const std::weak_ptr<WebViewGeneration> &weakGeneration);

#if defined(_WIN32)

constexpr DWORD kDispatcherWaitMilliseconds = 1000;
constexpr WPARAM kDispatcherInvoke = 0;
constexpr WPARAM kDispatcherRelease = 1;
constexpr wchar_t kDispatcherIdentityProperty[] =
    L"EffeTune.WebView.DispatcherIdentity";

LRESULT CALLBACK dispatcherWindowProc(HWND window, UINT message, WPARAM wParam,
                                      LPARAM lParam);

[[nodiscard]] std::uintptr_t nextDispatcherIdentity() noexcept {
  static std::atomic_uintptr_t next{1};
  auto identity = next.fetch_add(1, std::memory_order_relaxed);
  if (identity == 0) {
    identity = next.fetch_add(1, std::memory_order_relaxed);
  }
  return identity;
}

[[nodiscard]] HMODULE retainDispatcherModule() noexcept {
  HMODULE module = nullptr;
  return GetModuleHandleExW(
             GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
             reinterpret_cast<LPCWSTR>(&dispatcherWindowProc), &module) != FALSE
             ? module
             : nullptr;
}

struct WebViewOperation {
  std::function<bool(WebViewGeneration &)> callback;
  std::atomic_bool cancelled{false};
  std::atomic_bool result{false};
};

#elif defined(__APPLE__)

constexpr std::int64_t kMainThreadWaitNanoseconds = 1000000000;

struct MainThreadRequest {
  std::function<void(const std::shared_ptr<MainThreadRequest> &)> callback;
  std::atomic_bool cancelled{false};
};

void runMainThreadRequest(void *context) {
  std::unique_ptr<std::shared_ptr<MainThreadRequest>> holder(
      static_cast<std::shared_ptr<MainThreadRequest> *>(context));
  const auto request = **holder;
  if (!request->cancelled.load(std::memory_order_acquire)) {
    request->callback(request);
  }
  request->callback = {};
}

[[nodiscard]] void *retainCurrentModule() noexcept {
  Dl_info info{};
  if (dladdr(reinterpret_cast<const void *>(&runMainThreadRequest), &info) == 0 ||
      info.dli_fname == nullptr) {
    return nullptr;
  }
  auto *module = dlopen(info.dli_fname, RTLD_LAZY | RTLD_NOLOAD);
  return module != nullptr ? module : dlopen(info.dli_fname, RTLD_LAZY);
}

struct MainThreadObjectRelease {
  id object = nullptr;
  void *module = nullptr;
};

void releaseMainThreadObject(void *context) {
  std::unique_ptr<MainThreadObjectRelease> release(
      static_cast<MainThreadObjectRelease *>(context));
  using Release = void (*)(id, SEL);
  reinterpret_cast<Release>(objc_msgSend)(release->object,
                                          sel_registerName("release"));
  // The callback is running from this image, so its last module reference
  // cannot safely be dropped until after it returns. This path is only the
  // exception fallback; keeping the pin is safer than self-unloading here.
  (void)release->module;
}

class MainThreadObjectLease {
public:
  explicit MainThreadObjectLease(id object) noexcept {
    if (object != nullptr) {
      using Retain = id (*)(id, SEL);
      object_ = reinterpret_cast<Retain>(objc_msgSend)(
          object, sel_registerName("retain"));
    }
  }

  ~MainThreadObjectLease() {
    if (object_ == nullptr) {
      return;
    }
    if (pthread_main_np() != 0) {
      using Release = void (*)(id, SEL);
      reinterpret_cast<Release>(objc_msgSend)(object_,
                                              sel_registerName("release"));
      return;
    }
    auto *module = retainCurrentModule();
    if (module == nullptr) {
      return;
    }
    auto *release = new (std::nothrow) MainThreadObjectRelease{object_, module};
    if (release == nullptr) {
      return;
    }
    dispatch_async_f(dispatch_get_main_queue(), release,
                     releaseMainThreadObject);
  }

  MainThreadObjectLease(const MainThreadObjectLease &) = delete;
  MainThreadObjectLease &operator=(const MainThreadObjectLease &) = delete;

  [[nodiscard]] id get() const noexcept { return object_; }

private:
  id object_ = nullptr;
};

[[nodiscard]] bool runCancellableOnMainThreadBounded(
    std::function<void(const std::shared_ptr<MainThreadRequest> &)> callback,
    const bool cancelOnTimeout = true) {
  try {
    const auto request = std::make_shared<MainThreadRequest>();
    request->callback = std::move(callback);
    if (pthread_main_np() != 0) {
      request->callback(request);
      return true;
    }
    auto *module = retainCurrentModule();
    auto group = dispatch_group_create();
    dispatch_group_async_f(
        group, dispatch_get_main_queue(),
        new std::shared_ptr<MainThreadRequest>(request), runMainThreadRequest);
    const auto completed =
        dispatch_group_wait(group, dispatch_time(DISPATCH_TIME_NOW,
                                                 kMainThreadWaitNanoseconds)) == 0;
#if !OS_OBJECT_USE_OBJC
    dispatch_release(group);
#endif
    if (completed) {
      if (module != nullptr) {
        dlclose(module);
      }
    } else {
      if (cancelOnTimeout) {
        request->cancelled.store(true, std::memory_order_release);
      }
      // The pending block contains code and state from this image. Retaining the
      // image is the only safe fallback when the host's main loop is unavailable.
      (void)module;
    }
    return completed;
  } catch (const std::exception &) {
    return false;
  }
}

[[nodiscard]] bool runOnMainThreadBounded(std::function<void()> callback,
                                          const bool cancelOnTimeout = true) {
  return runCancellableOnMainThreadBounded(
      [callback = std::move(callback)](
          const std::shared_ptr<MainThreadRequest> &) { callback(); },
      cancelOnTimeout);
}

#endif

} // namespace

struct WebViewGeneration : public std::enable_shared_from_this<WebViewGeneration> {
  explicit WebViewGeneration(std::weak_ptr<WebViewHostState> state)
      : hostState(std::move(state)) {}

  ~WebViewGeneration() {
#if defined(_WIN32)
    if (!released.load(std::memory_order_acquire) &&
        ownerThread != GetCurrentThreadId()) {
      // The dispatcher/window owns another shared reference in every normal
      // cross-thread path. If that window vanished unexpectedly, abandoning the
      // affine objects is safer than running CHOC/COM destruction here.
      (void)webView.release();
      diagnosticView = nullptr;
      diagnosticFont = nullptr;
      return;
    }
#elif defined(__APPLE__)
    if (webView != nullptr && pthread_main_np() == 0) {
      // A timed-out main-queue request retains the generation. This branch is
      // only for an unavailable queue during process teardown.
      (void)webView.release();
      return;
    }
    if (!released.load(std::memory_order_acquire)) {
      releaseOnOwner();
    }
#endif
    webView.reset();
  }

  [[nodiscard]] static std::shared_ptr<WebViewGeneration>
  create(const std::shared_ptr<WebViewHostState> &state) {
    auto result = std::shared_ptr<WebViewGeneration>(
        new WebViewGeneration(std::weak_ptr<WebViewHostState>(state)));
#if defined(_WIN32)
    result->ownerThread = GetCurrentThreadId();
    result->identity = nextDispatcherIdentity();
    if (!result->createDispatcher()) {
      result->setStatus(WebViewStatus::runtimeUnavailable);
      return result;
    }
#endif
    result->createWebViewOnOwner();
    return result;
  }

  [[nodiscard]] bool ownsCurrentThread() const noexcept {
#if defined(_WIN32)
    return ownerThread == GetCurrentThreadId();
#elif defined(__APPLE__)
    return pthread_main_np() != 0;
#else
    return true;
#endif
  }

  void setStatus(const WebViewStatus newStatus) noexcept {
    currentStatus.store(newStatus, std::memory_order_release);
    if (const auto state = hostState.lock()) {
      state->publishStatus(this, newStatus);
    }
  }

  void setLoaded(const bool isLoaded) noexcept {
    if (const auto state = hostState.lock()) {
      state->publishLoaded(this, isLoaded);
    }
  }

  [[nodiscard]] bool usableOnOwner() const noexcept {
    if (webView == nullptr || !webView->loadedOK() ||
        webView->getViewHandle() == nullptr) {
      return false;
    }
#if defined(_WIN32)
    return IsWindow(static_cast<HWND>(webView->getViewHandle())) != FALSE;
#else
    return true;
#endif
  }

  void createWebViewOnOwner() noexcept {
    if (!ownsCurrentThread() || retiring.load(std::memory_order_acquire)) {
      return;
    }
    webView.reset();
    setLoaded(false);
    setStatus(WebViewStatus::initialising);
#if defined(_WIN32)
    if (!callerIsInSingleThreadedApartment()) {
      setStatus(WebViewStatus::unsupportedApartment);
      return;
    }
#endif
    try {
      choc::ui::WebView::Options options;
#if !defined(NDEBUG)
      options.enableDebugMode = true;
#endif
      options.transparentBackground = true;
      options.customSchemeURI = std::string(kWebViewHomeUri);
      const auto weakState = hostState;
      const auto weakGeneration = weak_from_this();
      options.fetchResource =
          [weakState, weakGeneration](const std::string &path)
          -> std::optional<choc::ui::WebView::Options::Resource> {
        const auto state = weakState.lock();
        const auto generation = weakGeneration.lock();
        if (state == nullptr || state->stopping.load(std::memory_order_acquire)) {
          return std::nullopt;
        }
        if (generation == nullptr ||
            generation->retiring.load(std::memory_order_acquire)) {
          return std::nullopt;
        }
        const auto resource = state->fetchResource(path);
        if (!resource.has_value()) {
          return std::nullopt;
        }
        return choc::ui::WebView::Options::Resource(resource->bytes,
                                                    resource->mimeType);
      };
      options.webviewIsReady =
          [weakState, weakGeneration](choc::ui::WebView &view) {
        const auto generation = weakGeneration.lock();
        if (generation == nullptr ||
            generation->retiring.load(std::memory_order_acquire)) {
          return;
        }
        configureWebView(view, weakState, weakGeneration);
        generation->setStatus(WebViewStatus::ready);
        if (generation->parent != nullptr) {
          generation->destroyDiagnosticViewOnOwner();
          generation->presentNativeViewOnOwner();
          generation->endInitialisationWatchOnOwner();
        }
      };
      webView = std::make_unique<choc::ui::WebView>(options);
      setLoaded(webView != nullptr && webView->loadedOK());
    } catch (const std::exception &) {
      webView.reset();
      setLoaded(false);
      setStatus(WebViewStatus::runtimeUnavailable);
    }
  }

  void replaceWebViewOnOwner() noexcept {
    createWebViewOnOwner();
  }

  [[nodiscard]] bool ensureWebViewOnOwner() noexcept {
    if (usableOnOwner()) {
      return true;
    }
#if defined(_WIN32)
    constexpr int maximumAttempts = 3;
    for (int attempt = 0; attempt < maximumAttempts; ++attempt) {
      if (attempt != 0) {
        const auto tick = GetTickCount64();
        while (GetTickCount64() == tick) {
          Sleep(1);
        }
      }
      replaceWebViewOnOwner();
      if (usableOnOwner()) {
        return true;
      }
      if (currentStatus.load(std::memory_order_acquire) ==
          WebViewStatus::unsupportedApartment) {
        return false;
      }
    }
#else
    replaceWebViewOnOwner();
    if (usableOnOwner()) {
      return true;
    }
#endif
    setStatus(WebViewStatus::runtimeUnavailable);
    return false;
  }

  [[nodiscard]] bool attachOnOwner(void *newOwner, void *newParent,
                                   const std::int32_t width,
                                   const std::int32_t height,
                                   const std::atomic_bool *cancelled = nullptr) {
    if (!ownsCurrentThread() || newOwner == nullptr || newParent == nullptr ||
        retiring.load(std::memory_order_acquire) ||
        (cancelled != nullptr &&
         cancelled->load(std::memory_order_acquire))) {
      return false;
    }
#if defined(_WIN32)
    auto *parentWindow = static_cast<HWND>(newParent);
    if (IsWindow(parentWindow) == FALSE) {
      return false;
    }
#endif
    endInitialisationWatchOnOwner();
    owner = nullptr;
    parent = nullptr;
    lastWidth = std::max(1, width);
    lastHeight = std::max(1, height);
    destroyDiagnosticViewOnOwner();

    if (webView != nullptr && webView->isReady()) {
      replaceWebViewOnOwner();
    }
    if (!ensureWebViewOnOwner()) {
      owner = newOwner;
      parent = newParent;
      if (showDiagnosticViewOnOwner()) {
        return true;
      }
      owner = nullptr;
      parent = nullptr;
      return false;
    }
    auto *view = webView->getViewHandle();
    if (view == nullptr) {
      return false;
    }
    if (retiring.load(std::memory_order_acquire) ||
        (cancelled != nullptr &&
         cancelled->load(std::memory_order_acquire))) {
      return false;
    }
#if defined(_WIN32)
    auto *child = static_cast<HWND>(view);
    SetLastError(ERROR_SUCCESS);
    const auto style = GetWindowLongPtrW(child, GWL_STYLE);
    if (style == 0 && GetLastError() != ERROR_SUCCESS) {
      return false;
    }
    SetLastError(ERROR_SUCCESS);
    if (SetWindowLongPtrW(child, GWL_STYLE,
                          (style | WS_CHILD | WS_CLIPSIBLINGS) & ~WS_POPUP) == 0 &&
        GetLastError() != ERROR_SUCCESS) {
      return false;
    }
    SetLastError(ERROR_SUCCESS);
    const auto previousParent = SetParent(child, parentWindow);
    if (previousParent == nullptr && GetLastError() != ERROR_SUCCESS) {
      replaceWebViewOnOwner();
      return false;
    }
    if (SetWindowPos(child, nullptr, 0, 0, lastWidth, lastHeight,
                     SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOZORDER |
                         SWP_SHOWWINDOW) == FALSE) {
      replaceWebViewOnOwner();
      return false;
    }
#elif defined(__APPLE__)
    using AddSubview = void (*)(id, SEL, id);
    reinterpret_cast<AddSubview>(objc_msgSend)(static_cast<id>(newParent),
                                               sel_registerName("addSubview:"),
                                               static_cast<id>(view));
#else
    return false;
#endif
    owner = newOwner;
    parent = newParent;
    resizeOnOwner(newOwner, width, height);
    beginInitialisationWatchOnOwner();
    return true;
  }

  void resizeOnOwner(void *requestedOwner, const std::int32_t width,
                     const std::int32_t height) noexcept {
    if (!ownsCurrentThread() || requestedOwner == nullptr ||
        requestedOwner != owner || retiring.load(std::memory_order_acquire)) {
      return;
    }
    lastWidth = std::max(1, width);
    lastHeight = std::max(1, height);
#if defined(_WIN32)
    if (diagnosticView != nullptr) {
      SetWindowPos(diagnosticView, nullptr, 0, 0, lastWidth, lastHeight,
                   SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOZORDER);
    }
#endif
    if (webView == nullptr || webView->getViewHandle() == nullptr) {
      return;
    }
#if defined(_WIN32)
    auto *child = static_cast<HWND>(webView->getViewHandle());
    if (IsWindow(child) != FALSE) {
      SetWindowPos(child, nullptr, 0, 0, lastWidth, lastHeight,
                   SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOZORDER);
    }
#elif defined(__APPLE__)
    using SetFrame = void (*)(id, SEL, CGRect);
    reinterpret_cast<SetFrame>(objc_msgSend)(
        static_cast<id>(webView->getViewHandle()), sel_registerName("setFrame:"),
        CGRectMake(0, 0, lastWidth, lastHeight));
#endif
  }

  void detachOnOwner(void *requestedOwner) noexcept {
    if (!ownsCurrentThread() || requestedOwner == nullptr ||
        requestedOwner != owner || retiring.load(std::memory_order_acquire)) {
      return;
    }
#if defined(__APPLE__)
    if (webView != nullptr && webView->getViewHandle() != nullptr &&
        parent != nullptr) {
      using RemoveFromSuperview = void (*)(id, SEL);
      reinterpret_cast<RemoveFromSuperview>(objc_msgSend)(
          static_cast<id>(webView->getViewHandle()),
          sel_registerName("removeFromSuperview"));
    }
#endif
    endInitialisationWatchOnOwner();
    destroyDiagnosticViewOnOwner();
    owner = nullptr;
    parent = nullptr;
    setStatus(WebViewStatus::initialising);
    replaceWebViewOnOwner();
  }

  [[nodiscard]] bool evaluateOnOwner(
      std::string script, WebViewHost::EvaluationHandler completion) {
    if (!ownsCurrentThread() || webView == nullptr || !webView->isReady() ||
        retiring.load(std::memory_order_acquire)) {
      return false;
    }
    const auto weakGeneration = weak_from_this();
    return webView->evaluateJavascript(
        script, [weakGeneration, completion = std::move(completion)](
                    const std::string &error,
                    const choc::value::ValueView &result) {
          const auto generation = weakGeneration.lock();
          if (generation == nullptr ||
              generation->retiring.load(std::memory_order_acquire) ||
              !completion) {
            return;
          }
          completion(error, choc::json::toString(result));
        });
  }

  void beginInitialisationWatchOnOwner() noexcept {
    attachedAt = std::chrono::steady_clock::now();
    if (webView != nullptr && webView->isReady()) {
      setStatus(WebViewStatus::ready);
      presentNativeViewOnOwner();
      return;
    }
    setStatus(WebViewStatus::initialising);
#if defined(_WIN32)
    if (const auto window = dispatcherWindow.load(std::memory_order_acquire);
        window != nullptr) {
      (void)SetTimer(window, identity,
                     kWatchdogIntervalMilliseconds, nullptr);
    }
#endif
  }

  void endInitialisationWatchOnOwner() noexcept {
#if defined(_WIN32)
    if (const auto window = dispatcherWindow.load(std::memory_order_acquire);
        window != nullptr) {
      (void)KillTimer(window, identity);
    }
#endif
  }

  void serviceInitialisationOnOwner() noexcept {
    if (!ownsCurrentThread() || parent == nullptr ||
        retiring.load(std::memory_order_acquire)) {
      return;
    }
    if (usableOnOwner() && webView->isReady()) {
      if (currentStatus.load(std::memory_order_acquire) != WebViewStatus::ready) {
        setStatus(WebViewStatus::ready);
        destroyDiagnosticViewOnOwner();
        presentNativeViewOnOwner();
      }
      endInitialisationWatchOnOwner();
      return;
    }
    bool statusTransitioned = false;
    if (currentStatus.load(std::memory_order_acquire) ==
            WebViewStatus::initialising &&
        std::chrono::steady_clock::now() - attachedAt >=
            kWebViewInitialisationTimeout) {
      setStatus(WebViewStatus::initialisationTimedOut);
      statusTransitioned = true;
    }
    if (statusTransitioned || !diagnosticVisible) {
      (void)showDiagnosticViewOnOwner();
    }
  }

  void presentNativeViewOnOwner() noexcept {
#if defined(_WIN32)
    if (webView == nullptr || parent == nullptr) {
      return;
    }
    auto *child = static_cast<HWND>(webView->getViewHandle());
    if (child == nullptr || IsWindow(child) == FALSE) {
      return;
    }
    SetWindowPos(child, nullptr, 0, 0, lastWidth, lastHeight,
                 SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOZORDER |
                     SWP_SHOWWINDOW);
    SendMessageW(child, WM_SHOWWINDOW, TRUE, 0);
    SendMessageW(child, WM_SIZE, SIZE_RESTORED,
                 MAKELPARAM(static_cast<WORD>(lastWidth),
                            static_cast<WORD>(lastHeight)));
#endif
  }

  [[nodiscard]] bool showDiagnosticViewOnOwner() noexcept {
#if defined(_WIN32)
    if (parent == nullptr || !ownsCurrentThread()) {
      return false;
    }
    auto *parentWindow = static_cast<HWND>(parent);
    if (IsWindow(parentWindow) == FALSE) {
      return false;
    }
    const auto text = widen(WebViewHost::diagnosticText(
        currentStatus.load(std::memory_order_acquire)));
    if (diagnosticView != nullptr) {
      SetWindowTextW(diagnosticView, text.c_str());
      SetWindowPos(diagnosticView, HWND_TOP, 0, 0, lastWidth, lastHeight,
                   SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
      diagnosticVisible = true;
      return true;
    }
    diagnosticView = CreateWindowExW(
        0, L"EDIT", text.c_str(),
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY |
            ES_AUTOVSCROLL,
        0, 0, lastWidth, lastHeight, parentWindow, nullptr, nullptr, nullptr);
    if (diagnosticView == nullptr) {
      return false;
    }
    if (diagnosticFont == nullptr) {
      diagnosticFont = createDiagnosticFont();
    }
    if (diagnosticFont != nullptr) {
      SendMessageW(diagnosticView, WM_SETFONT,
                   reinterpret_cast<WPARAM>(diagnosticFont), TRUE);
    }
    SendMessageW(diagnosticView, EM_SETMARGINS,
                 EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(16, 16));
    diagnosticVisible = true;
    return true;
#else
    return false;
#endif
  }

  void destroyDiagnosticViewOnOwner() noexcept {
    diagnosticVisible = false;
#if defined(_WIN32)
    if (!ownsCurrentThread()) {
      return;
    }
    if (diagnosticView != nullptr) {
      const auto view = std::exchange(diagnosticView, nullptr);
      if (IsWindow(view) != FALSE) {
        DestroyWindow(view);
      }
    }
    if (diagnosticFont != nullptr) {
      DeleteObject(diagnosticFont);
      diagnosticFont = nullptr;
    }
#endif
  }

#if defined(_WIN32)
  [[nodiscard]] bool createDispatcher() {
    const auto window =
        CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE,
                        nullptr, nullptr, nullptr);
    if (window == nullptr) {
      return false;
    }
    dispatcherWindow.store(window, std::memory_order_release);
    auto *holder = new std::shared_ptr<WebViewGeneration>(shared_from_this());
    SetLastError(ERROR_SUCCESS);
    if (SetWindowLongPtrW(window, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(holder)) == 0 &&
        GetLastError() != ERROR_SUCCESS) {
      delete holder;
      DestroyWindow(window);
      dispatcherWindow.store(nullptr, std::memory_order_release);
      return false;
    }
    if (SetPropW(window, kDispatcherIdentityProperty,
                 reinterpret_cast<HANDLE>(identity)) == FALSE) {
      SetWindowLongPtrW(window, GWLP_USERDATA, 0);
      delete holder;
      DestroyWindow(window);
      dispatcherWindow.store(nullptr, std::memory_order_release);
      return false;
    }
    SetLastError(ERROR_SUCCESS);
    const auto original = SetWindowLongPtrW(
        window, GWLP_WNDPROC,
        reinterpret_cast<LONG_PTR>(dispatcherWindowProc));
    if (original == 0 && GetLastError() != ERROR_SUCCESS) {
      RemovePropW(window, kDispatcherIdentityProperty);
      SetWindowLongPtrW(window, GWLP_USERDATA, 0);
      delete holder;
      DestroyWindow(window);
      dispatcherWindow.store(nullptr, std::memory_order_release);
      return false;
    }
    originalWindowProc = reinterpret_cast<WNDPROC>(original);
    return true;
  }

  [[nodiscard]] bool isDispatcherWindow() const noexcept {
    const auto window = dispatcherWindow.load(std::memory_order_acquire);
    if (window == nullptr || IsWindow(window) == FALSE) {
      return false;
    }
    DWORD process = 0;
    const auto thread = GetWindowThreadProcessId(window, &process);
    if (thread != ownerThread || process != GetCurrentProcessId() ||
        reinterpret_cast<WNDPROC>(GetWindowLongPtrW(window, GWLP_WNDPROC)) !=
            dispatcherWindowProc ||
        reinterpret_cast<std::uintptr_t>(
            GetPropW(window, kDispatcherIdentityProperty)) != identity) {
      return false;
    }
    return true;
  }

  [[nodiscard]] bool sendBounded(const WPARAM command,
                                 const bool releaseCommand) noexcept {
    auto module = retainDispatcherModule();
    if (!isDispatcherWindow()) {
      if (releaseCommand) {
        (void)module;
      } else if (module != nullptr) {
        FreeLibrary(module);
      }
      return false;
    }
    DWORD_PTR result = 0;
    SetLastError(ERROR_SUCCESS);
    const auto sent = SendMessageTimeoutW(
        dispatcherWindow.load(std::memory_order_acquire), kDispatcherMessage,
        command,
        static_cast<LPARAM>(identity), SMTO_ABORTIFHUNG | SMTO_BLOCK,
        kDispatcherWaitMilliseconds, &result);
    const auto timedOut = sent == 0 && GetLastError() == ERROR_TIMEOUT;
    if (timedOut || (releaseCommand && (sent == 0 || result == 0))) {
      // The message may already be executing, or may run when the STA resumes.
      // Keep this image loaded so its WNDPROC and CHOC callbacks remain valid.
      (void)module;
      if (releaseCommand) {
        const auto window = dispatcherWindow.load(std::memory_order_acquire);
        if (window != nullptr) {
          (void)PostMessageW(window, kDispatcherMessage, kDispatcherRelease,
                             static_cast<LPARAM>(identity));
        }
      }
    } else if (module != nullptr) {
      FreeLibrary(module);
    }
    return sent != 0 && result != 0;
  }

  [[nodiscard]] bool invoke(
      std::function<bool(WebViewGeneration &)> callback) {
    if (retiring.load(std::memory_order_acquire)) {
      return false;
    }
    if (ownsCurrentThread()) {
      return callback(*this);
    }
    const auto operation = std::make_shared<WebViewOperation>();
    operation->callback = std::move(callback);
    {
      const std::scoped_lock lock(operationMutex);
      if (retiring.load(std::memory_order_acquire)) {
        return false;
      }
      operations.push_back(operation);
    }
    if (!sendBounded(kDispatcherInvoke, false)) {
      operation->cancelled.store(true, std::memory_order_release);
      return false;
    }
    return operation->result.load(std::memory_order_acquire);
  }

  void drainOperationsOnOwner() noexcept {
    std::vector<std::shared_ptr<WebViewOperation>> pending;
    {
      const std::scoped_lock lock(operationMutex);
      pending.swap(operations);
    }
    for (const auto &operation : pending) {
      if (!operation->cancelled.load(std::memory_order_acquire) &&
          !retiring.load(std::memory_order_acquire)) {
        operation->result.store(operation->callback(*this),
                                std::memory_order_release);
      }
    }
  }

  void closeDispatcherOnOwner() noexcept {
    if (!ownsCurrentThread()) {
      return;
    }
    const auto window =
        dispatcherWindow.exchange(nullptr, std::memory_order_acq_rel);
    if (window == nullptr) {
      return;
    }
    auto *holder = reinterpret_cast<std::shared_ptr<WebViewGeneration> *>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    SetWindowLongPtrW(window, GWLP_WNDPROC,
                      reinterpret_cast<LONG_PTR>(originalWindowProc));
    SetWindowLongPtrW(window, GWLP_USERDATA, 0);
    RemovePropW(window, kDispatcherIdentityProperty);
    DestroyWindow(window);
    delete holder;
  }
#endif

  void releaseOnOwner(const bool closeDispatcher = true) noexcept {
    if (!ownsCurrentThread() ||
        released.exchange(true, std::memory_order_acq_rel)) {
      return;
    }
    retiring.store(true, std::memory_order_release);
    endInitialisationWatchOnOwner();
    destroyDiagnosticViewOnOwner();
#if defined(__APPLE__)
    if (webView != nullptr && webView->getViewHandle() != nullptr &&
        parent != nullptr) {
      using RemoveFromSuperview = void (*)(id, SEL);
      reinterpret_cast<RemoveFromSuperview>(objc_msgSend)(
          static_cast<id>(webView->getViewHandle()),
          sel_registerName("removeFromSuperview"));
    }
#endif
    owner = nullptr;
    parent = nullptr;
    webView.reset();
    setLoaded(false);
#if defined(_WIN32)
    if (closeDispatcher) {
      closeDispatcherOnOwner();
    }
#else
    (void)closeDispatcher;
#endif
  }

  void retire() noexcept {
    if (retiring.exchange(true, std::memory_order_acq_rel)) {
      return;
    }
#if defined(_WIN32)
    {
      const std::scoped_lock lock(operationMutex);
      for (const auto &operation : operations) {
        operation->cancelled.store(true, std::memory_order_release);
      }
    }
    if (ownsCurrentThread()) {
      releaseOnOwner();
    } else {
      (void)sendBounded(kDispatcherRelease, true);
    }
#elif defined(__APPLE__)
    if (ownsCurrentThread()) {
      releaseOnOwner();
    } else {
      try {
        const auto self = shared_from_this();
        if (!runOnMainThreadBounded([self] { self->releaseOnOwner(); }, false)) {
          (void)retainCurrentModule();
        }
      } catch (const std::exception &) {
        (void)retainCurrentModule();
      }
    }
#else
    releaseOnOwner();
#endif
  }

  std::weak_ptr<WebViewHostState> hostState;
  std::unique_ptr<choc::ui::WebView> webView;
  std::atomic<WebViewStatus> currentStatus{WebViewStatus::initialising};
  std::atomic_bool retiring{false};
  std::atomic_bool released{false};
  void *owner = nullptr;
  void *parent = nullptr;
  std::int32_t lastWidth = 1;
  std::int32_t lastHeight = 1;
  std::chrono::steady_clock::time_point attachedAt{};
  bool diagnosticVisible = false;
#if defined(_WIN32)
  std::atomic<HWND> dispatcherWindow{nullptr};
  WNDPROC originalWindowProc = nullptr;
  DWORD ownerThread = 0;
  std::uintptr_t identity = 0;
  HWND diagnosticView = nullptr;
  HFONT diagnosticFont = nullptr;
  std::mutex operationMutex;
  std::vector<std::shared_ptr<WebViewOperation>> operations;
#endif
};

namespace {

void configureWebView(choc::ui::WebView &view,
                      const std::weak_ptr<WebViewHostState> &weakState,
                      const std::weak_ptr<WebViewGeneration> &weakGeneration) {
  view.bind("vst_hostMessage", [weakState, weakGeneration](
                                   const choc::value::ValueView &arguments) {
    const auto state = weakState.lock();
    const auto generation = weakGeneration.lock();
    if (state == nullptr || state->stopping.load(std::memory_order_acquire) ||
        generation == nullptr ||
        generation->retiring.load(std::memory_order_acquire) ||
        !state->handler || !arguments.isArray() || arguments.size() == 0) {
      return choc::json::create("ok", false, "error", "Invalid bridge call");
    }
    const auto request = arguments[0].getWithDefault<std::string>({});
    try {
      return choc::json::parse(state->handler(request));
    } catch (const choc::json::ParseError &) {
      return choc::json::create("ok", false, "error",
                                "Invalid native bridge response");
    }
  });
  (void)view.addInitScript(
      "window.__EFFETUNE_VST__=true;window.pipelineStateLoaded=true;"
      "document.documentElement.classList.add('effetune-vst-host');");
  (void)view.navigate(std::string(kWebViewHomeUri) + "effetune.html");
}

#if defined(_WIN32)
LRESULT CALLBACK dispatcherWindowProc(HWND window, const UINT message,
                                      const WPARAM wParam,
                                      const LPARAM lParam) {
  auto *holder = reinterpret_cast<std::shared_ptr<WebViewGeneration> *>(
      GetWindowLongPtrW(window, GWLP_USERDATA));
  if (holder == nullptr || *holder == nullptr) {
    return DefWindowProcW(window, message, wParam, lParam);
  }
  const auto generation = *holder;
  if (generation->dispatcherWindow.load(std::memory_order_acquire) != window ||
      generation->ownerThread != GetCurrentThreadId() ||
      reinterpret_cast<WNDPROC>(GetWindowLongPtrW(window, GWLP_WNDPROC)) !=
          dispatcherWindowProc) {
    return DefWindowProcW(window, message, wParam, lParam);
  }
  if (message == WM_TIMER && wParam == generation->identity) {
    generation->serviceInitialisationOnOwner();
    return 0;
  }
  if (message == kDispatcherMessage) {
    if (static_cast<std::uintptr_t>(lParam) != generation->identity) {
      return 0;
    }
    if (wParam == kDispatcherInvoke) {
      generation->drainOperationsOnOwner();
      return 1;
    }
    if (wParam == kDispatcherRelease) {
      generation->releaseOnOwner();
      return 1;
    }
    return 0;
  }
  if (message == WM_NCDESTROY) {
    const auto original = generation->originalWindowProc;
    generation->releaseOnOwner(false);
    generation->dispatcherWindow.store(nullptr, std::memory_order_release);
    SetWindowLongPtrW(window, GWLP_USERDATA, 0);
    RemovePropW(window, kDispatcherIdentityProperty);
    const auto result = CallWindowProcW(original, window, message, wParam, lParam);
    delete holder;
    return result;
  }
  return CallWindowProcW(generation->originalWindowProc, window, message, wParam,
                         lParam);
}
#endif

} // namespace

void WebViewHostState::publishStatus(const WebViewGeneration *source,
                                     const WebViewStatus newStatus) noexcept {
  const std::scoped_lock lock(generationMutex);
  if (generation.get() == source) {
    status.store(newStatus, std::memory_order_release);
  }
}

void WebViewHostState::publishLoaded(const WebViewGeneration *source,
                                     const bool isLoaded) noexcept {
  const std::scoped_lock lock(generationMutex);
  if (generation.get() == source) {
    loaded.store(isLoaded, std::memory_order_release);
  }
}

void WebViewHostState::initialise(const bool createImmediately) {
  if (!createImmediately) {
    return;
  }
#if defined(__APPLE__)
  const auto self = shared_from_this();
  (void)runOnMainThreadBounded([self] {
    if (self->stopping.load(std::memory_order_acquire)) {
      return;
    }
    auto created = WebViewGeneration::create(self);
    const std::scoped_lock lock(self->generationMutex);
    self->generation = std::move(created);
    self->status.store(self->generation->currentStatus.load(
                           std::memory_order_acquire),
                       std::memory_order_release);
    self->loaded.store(self->generation->webView != nullptr &&
                           self->generation->webView->loadedOK(),
                       std::memory_order_release);
  });
#else
  auto created = WebViewGeneration::create(shared_from_this());
  const std::scoped_lock lock(generationMutex);
  generation = std::move(created);
  status.store(generation->currentStatus.load(std::memory_order_acquire),
               std::memory_order_release);
  loaded.store(generation->webView != nullptr && generation->webView->loadedOK(),
               std::memory_order_release);
#endif
}

void WebViewHostState::shutdown() noexcept {
  if (stopping.exchange(true, std::memory_order_acq_rel)) {
    return;
  }
  std::shared_ptr<WebViewGeneration> retired;
  {
    const std::scoped_lock lock(generationMutex);
    retired = std::move(generation);
    loaded.store(false, std::memory_order_release);
  }
  if (retired != nullptr) {
    retired->retire();
  }
}

bool WebViewHostState::attach(void *owner, void *parent,
                              const std::int32_t width,
                              const std::int32_t height) {
  if (stopping.load(std::memory_order_acquire)) {
    return false;
  }
#if defined(_WIN32)
  std::shared_ptr<WebViewGeneration> current;
  std::shared_ptr<WebViewGeneration> retired;
  {
    const std::scoped_lock lock(generationMutex);
    current = generation;
    if (current == nullptr || !current->ownsCurrentThread()) {
      retired = std::move(generation);
      current.reset();
    }
  }
  if (retired != nullptr) {
    retired->retire();
  }
  if (current == nullptr) {
    auto created = WebViewGeneration::create(shared_from_this());
    {
      const std::scoped_lock lock(generationMutex);
      if (generation == nullptr) {
        generation = created;
        current = created;
      } else {
        current = generation;
      }
    }
    if (current != created) {
      created->retire();
    }
  }
  if (!current->ownsCurrentThread()) {
    return false;
  }
  if (stopping.load(std::memory_order_acquire)) {
    current->retire();
    return false;
  }
  status.store(current->currentStatus.load(std::memory_order_acquire),
               std::memory_order_release);
  loaded.store(current->webView != nullptr && current->webView->loadedOK(),
               std::memory_order_release);
  return current->attachOnOwner(owner, parent, width, height);
#elif defined(__APPLE__)
  struct Result {
    std::atomic_bool attached{false};
  };
  const auto result = std::make_shared<Result>();
  const auto self = shared_from_this();
  auto parentLease =
      std::make_shared<MainThreadObjectLease>(static_cast<id>(parent));
  if (parentLease->get() == nullptr ||
      !runCancellableOnMainThreadBounded(
          [self, result, owner, width, height,
           parentLease = std::move(parentLease)](
              const std::shared_ptr<MainThreadRequest> &request) {
            const auto cancelled = [&] {
              return request->cancelled.load(std::memory_order_acquire) ||
                     self->stopping.load(std::memory_order_acquire);
            };
            if (cancelled()) {
              return;
            }
            std::shared_ptr<WebViewGeneration> current;
            std::shared_ptr<WebViewGeneration> created;
            bool publishedCreated = false;
            {
              const std::scoped_lock lock(self->generationMutex);
              current = self->generation;
            }
            if (current == nullptr) {
              created = WebViewGeneration::create(self);
              if (cancelled()) {
                created->retire();
                return;
              }
              bool cancelledBeforePublish = false;
              {
                const std::scoped_lock lock(self->generationMutex);
                if (cancelled()) {
                  cancelledBeforePublish = true;
                } else if (self->generation == nullptr) {
                  self->generation = created;
                  current = created;
                  publishedCreated = true;
                } else {
                  current = self->generation;
                }
              }
              if (cancelledBeforePublish) {
                created->retire();
                return;
              }
              if (current != created) {
                created->retire();
              }
            }
            if (cancelled()) {
              if (publishedCreated) {
                const std::scoped_lock lock(self->generationMutex);
                if (self->generation == current) {
                  self->generation.reset();
                }
              }
              if (publishedCreated) {
                current->retire();
              }
              return;
            }
            const auto attached = current->attachOnOwner(
                owner, parentLease->get(), width, height, &request->cancelled);
            if (!attached) {
              return;
            }
            if (cancelled()) {
              current->detachOnOwner(owner);
              if (publishedCreated) {
                {
                  const std::scoped_lock lock(self->generationMutex);
                  if (self->generation == current) {
                    self->generation.reset();
                  }
                }
                current->retire();
              }
              return;
            }
            result->attached.store(true, std::memory_order_release);
          })) {
    return false;
  }
  return result->attached.load(std::memory_order_acquire);
#else
  (void)owner;
  (void)parent;
  (void)width;
  (void)height;
  return false;
#endif
}

void WebViewHostState::resize(void *owner, const std::int32_t width,
                              const std::int32_t height) noexcept {
  try {
  if (stopping.load(std::memory_order_acquire)) {
    return;
  }
  std::shared_ptr<WebViewGeneration> current;
  {
    const std::scoped_lock lock(generationMutex);
    current = generation;
  }
  if (current == nullptr) {
    return;
  }
#if defined(_WIN32)
  (void)current->invoke([owner, width, height](WebViewGeneration &target) {
    target.resizeOnOwner(owner, width, height);
    return true;
  });
#elif defined(__APPLE__)
  (void)runOnMainThreadBounded([current, owner, width, height] {
    current->resizeOnOwner(owner, width, height);
  });
#endif
  } catch (const std::exception &) {
  }
}

void WebViewHostState::detach(void *owner) noexcept {
  try {
  if (stopping.load(std::memory_order_acquire)) {
    return;
  }
  std::shared_ptr<WebViewGeneration> current;
  {
    const std::scoped_lock lock(generationMutex);
    current = generation;
  }
  if (current == nullptr) {
    return;
  }
#if defined(_WIN32)
  (void)current->invoke([owner](WebViewGeneration &target) {
    target.detachOnOwner(owner);
    return true;
  });
#elif defined(__APPLE__)
  (void)runOnMainThreadBounded(
      [current, owner] { current->detachOnOwner(owner); });
#endif
  } catch (const std::exception &) {
  }
}

void WebViewHostState::serviceInitialisation() noexcept {
  try {
  if (stopping.load(std::memory_order_acquire)) {
    return;
  }
  std::shared_ptr<WebViewGeneration> current;
  {
    const std::scoped_lock lock(generationMutex);
    current = generation;
  }
  if (current == nullptr) {
    return;
  }
#if defined(_WIN32)
  (void)current->invoke([](WebViewGeneration &target) {
    target.serviceInitialisationOnOwner();
    return true;
  });
#elif defined(__APPLE__)
  (void)runOnMainThreadBounded(
      [current] { current->serviceInitialisationOnOwner(); });
#endif
  } catch (const std::exception &) {
  }
}

bool WebViewHostState::evaluate(
    std::string script, WebViewHost::EvaluationHandler completion) {
  std::shared_ptr<WebViewGeneration> current;
  {
    const std::scoped_lock lock(generationMutex);
    current = generation;
  }
  if (current == nullptr || stopping.load(std::memory_order_acquire)) {
    return false;
  }
  const auto completionAllowed = std::make_shared<std::atomic_bool>(true);
  WebViewHost::EvaluationHandler guardedCompletion;
  if (completion) {
    guardedCompletion =
        [completionAllowed, completion = std::move(completion)](
            std::string error, std::string result) mutable {
          if (completionAllowed->load(std::memory_order_acquire)) {
            completion(std::move(error), std::move(result));
          }
        };
  }
#if defined(_WIN32)
  const auto started = current->invoke(
      [script = std::move(script), completion = std::move(guardedCompletion)](
          WebViewGeneration &target) mutable {
        return target.evaluateOnOwner(std::move(script), std::move(completion));
      });
  if (!started) {
    completionAllowed->store(false, std::memory_order_release);
  }
  return started;
#elif defined(__APPLE__)
  struct Result {
    bool started = false;
  };
  const auto result = std::make_shared<Result>();
  if (!runOnMainThreadBounded(
          [current, result, script = std::move(script),
           completion = std::move(guardedCompletion)]() mutable {
            result->started = current->evaluateOnOwner(
                std::move(script), std::move(completion));
          })) {
    completionAllowed->store(false, std::memory_order_release);
    return false;
  }
  if (!result->started) {
    completionAllowed->store(false, std::memory_order_release);
  }
  return result->started;
#else
  return false;
#endif
}

WebViewHost::WebViewHost(MessageHandler handler,
                         std::filesystem::path resourceRoot,
                         const bool createImmediately)
    : state_(std::make_shared<WebViewHostState>(std::move(handler),
                                               std::move(resourceRoot))) {
  state_->initialise(createImmediately);
}

WebViewHost::~WebViewHost() { shutdown(); }

void WebViewHost::shutdown() noexcept {
  if (state_ != nullptr) {
    state_->shutdown();
  }
}

bool WebViewHost::attach(void *owner, void *parent, const std::int32_t width,
                         const std::int32_t height) {
  return state_ != nullptr && state_->attach(owner, parent, width, height);
}

void WebViewHost::resize(void *owner, const std::int32_t width,
                         const std::int32_t height) noexcept {
  if (state_ != nullptr) {
    state_->resize(owner, width, height);
  }
}

void WebViewHost::detach(void *owner) noexcept {
  if (state_ != nullptr) {
    state_->detach(owner);
  }
}

bool WebViewHost::loaded() const noexcept {
  return state_ != nullptr && state_->loaded.load(std::memory_order_acquire);
}

bool WebViewHost::evaluate(std::string script, EvaluationHandler handler) {
  return state_ != nullptr &&
         state_->evaluate(std::move(script), std::move(handler));
}

WebViewStatus WebViewHost::status() const noexcept {
  return state_ != nullptr
             ? state_->status.load(std::memory_order_acquire)
             : WebViewStatus::runtimeUnavailable;
}

void WebViewHost::serviceInitialisation() noexcept {
  if (state_ != nullptr) {
    state_->serviceInitialisation();
  }
}

} // namespace effetune::vst
