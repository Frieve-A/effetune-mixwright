#include "bridge/webview_host.h"

#include "choc/gui/choc_WebView.h"
#include "choc/text/choc_JSON.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <string>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#include <Windows.h>
#elif defined(__APPLE__)
#include <CoreGraphics/CoreGraphics.h>
#include <dlfcn.h>
#include <objc/message.h>
#include <objc/runtime.h>
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

// A timer with a null window posts WM_TIMER straight to the calling thread's
// queue, so the watchdog needs no window class of the plug-in's own - nothing
// that could leave a window procedure behind in an unloaded binary - and it
// always runs on the thread that owns the editor windows. The shared service
// timer cannot promise that: it belongs to whichever thread first initialised
// the plug-in, which is not necessarily the thread the host opens editors on.
constexpr UINT kWatchdogIntervalMilliseconds = 500;

using WatchdogKey = std::pair<DWORD, UINT_PTR>;

[[nodiscard]] std::mutex &watchdogMutex() {
  static std::mutex mutex;
  return mutex;
}

[[nodiscard]] std::map<WatchdogKey, WebViewHost *> &watchdogRegistry() {
  static std::map<WatchdogKey, WebViewHost *> registry;
  return registry;
}

void CALLBACK watchdogTimerProc(HWND, UINT, const UINT_PTR timerId, DWORD) {
  WebViewHost *host = nullptr;
  {
    const std::scoped_lock lock(watchdogMutex());
    auto &registry = watchdogRegistry();
    if (const auto entry = registry.find({GetCurrentThreadId(), timerId});
        entry != registry.end()) {
      host = entry->second;
    }
  }
  if (host != nullptr) {
    host->serviceInitialisation();
  }
}

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

WebViewHost::WebViewHost(MessageHandler handler, std::filesystem::path resourceRoot,
                         const bool createImmediately)
    : handler_(std::move(handler)),
      resourceRoot_(resourceRoot.empty() ? locateResourceRoot() : std::move(resourceRoot)),
      resourceBundleAvailable_(hasRequiredResources(resourceRoot_)) {
  if (createImmediately) {
    createWebView();
  }
}

WebViewHost::~WebViewHost() {
  endInitialisationWatch();
  destroyDiagnosticView();
  webView_.reset();
}

void WebViewHost::createWebView() {
  // Start from a clean verdict so a failure recorded on an earlier thread or an
  // earlier attempt cannot be reported as the reason for this one.
  status_ = WebViewStatus::initialising;
#if defined(_WIN32)
  if (!callerIsInSingleThreadedApartment()) {
    // Constructing the WebView anyway would leave a window that never paints and
    // no way to explain why, so stop here and let the diagnostic view speak.
    status_ = WebViewStatus::unsupportedApartment;
    return;
  }
#endif
  choc::ui::WebView::Options options;
#if !defined(NDEBUG)
  options.enableDebugMode = true;
#endif
  options.transparentBackground = true;
  // CHOC's default origin is shared by every CHOC WebView in the DAW's
  // WebView2 profile. Use a product-specific origin so another plug-in's
  // persisted site data or service worker cannot intercept this UI.
  options.customSchemeURI = std::string(kWebViewHomeUri);
  options.fetchResource = [this](const std::string &path)
      -> std::optional<choc::ui::WebView::Options::Resource> {
    const auto resource = fetchResource(path);
    if (!resource.has_value()) {
      return std::nullopt;
    }
    return choc::ui::WebView::Options::Resource(resource->bytes, resource->mimeType);
  };
  options.webviewIsReady = [this](choc::ui::WebView &view) { configure(view); };
  webView_ = std::make_unique<choc::ui::WebView>(options);
}

void WebViewHost::replaceWebView() noexcept {
  webView_.reset();
  try {
    createWebView();
  } catch (const std::exception &) {
    webView_.reset();
  }
}

bool WebViewHost::webViewIsUsable() const noexcept {
  if (webView_ == nullptr || !webView_->loadedOK() ||
      webView_->getViewHandle() == nullptr) {
    return false;
  }
#if defined(_WIN32)
  return IsWindow(static_cast<HWND>(webView_->getViewHandle())) != FALSE;
#else
  return true;
#endif
}

bool WebViewHost::ensureWebView() {
  if (webViewIsUsable()) {
    return true;
  }

#if defined(_WIN32)
  // CHOC derives its window class name from GetTickCount(), whose resolution is
  // about 16 ms, so two WebViews built inside the same tick collide: the second
  // RegisterClassExW fails and takes the whole construction with it. That happens
  // whenever a project loads several instances at once. Retrying after the tick
  // counter has moved on turns the collision into a brief delay instead of a
  // dead editor.
  constexpr int maximumAttempts = 3;
  for (int attempt = 0; attempt < maximumAttempts; ++attempt) {
    if (attempt != 0) {
      const auto tick = GetTickCount64();
      while (GetTickCount64() == tick) {
        Sleep(1);
      }
    }
    replaceWebView();
    if (webViewIsUsable()) {
      return true;
    }
    if (status_ == WebViewStatus::unsupportedApartment) {
      // Permanent for the lifetime of this thread; retrying only wastes time.
      return false;
    }
  }
  status_ = WebViewStatus::runtimeUnavailable;
  return false;
#else
  replaceWebView();
  if (webViewIsUsable()) {
    return true;
  }
  status_ = WebViewStatus::runtimeUnavailable;
  return false;
#endif
}

std::filesystem::path WebViewHost::locateResourceRoot() {
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
  if (dladdr(reinterpret_cast<const void *>(&WebViewHost::locateResourceRoot), &info) != 0 &&
      info.dli_fname != nullptr) {
    return std::filesystem::path(info.dli_fname).parent_path().parent_path() / "Resources" /
           "webview";
  }
#endif
  return std::filesystem::current_path() / "webview-assets";
}

std::optional<WebViewHost::ResourceData> WebViewHost::fetchResource(std::string path) const {
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
  if (path.find('\\') != std::string::npos || path.find(':') != std::string::npos) {
    return std::nullopt;
  }
  const std::filesystem::path relative(path);
  if (containsTraversal(relative)) {
    return std::nullopt;
  }
  if (!resourceBundleAvailable_) {
    if (path == "effetune.html") {
      return ResourceData{"text/html; charset=utf-8", missingResourcePage()};
    }
    return std::nullopt;
  }
  const auto candidate = resourceRoot_ / relative;
  std::error_code error;
  if (!std::filesystem::is_regular_file(candidate, error)) {
    return std::nullopt;
  }
  std::ifstream input(candidate, std::ios::binary);
  if (!input) {
    return std::nullopt;
  }
  ResourceData resource;
  resource.mimeType = mimeTypeFor(candidate);
  resource.bytes.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
  return resource;
}

void WebViewHost::configure(choc::ui::WebView &view) {
  view.bind("vst_hostMessage", [this](const choc::value::ValueView &arguments) {
    if (!handler_ || !arguments.isArray() || arguments.size() == 0) {
      return choc::json::create("ok", false, "error", "Invalid bridge call");
    }
    const auto request = arguments[0].getWithDefault<std::string>({});
    try {
      return choc::json::parse(handler_(request));
    } catch (const choc::json::ParseError &) {
      return choc::json::create("ok", false, "error", "Invalid native bridge response");
    }
  });
  (void)view.addInitScript(
      "window.__EFFETUNE_VST__=true;window.pipelineStateLoaded=true;"
      "document.documentElement.classList.add('effetune-vst-host');");
  (void)view.navigate(std::string(kWebViewHomeUri) + "effetune.html");
  // The controller only exists from here on, so everything the editor told the
  // native window before this point was dropped and has to be replayed. This
  // callback arrives on the thread that created the WebView, which is not
  // necessarily the thread that owns the editor windows - when it is not, the
  // replay is left to the watchdog, which always runs on the right one.
  if (parent_ != nullptr && callerOwnsEditorWindows()) {
    status_ = WebViewStatus::ready;
    destroyDiagnosticView();
    presentNativeView();
    endInitialisationWatch();
  }
}

void WebViewHost::presentNativeView() noexcept {
#if defined(_WIN32)
  if (webView_ == nullptr || parent_ == nullptr) {
    return;
  }
  auto *child = static_cast<HWND>(webView_->getViewHandle());
  if (child == nullptr || IsWindow(child) == FALSE) {
    return;
  }
  // Deliberately the same geometry call attach() makes, z-order included: the
  // diagnostic view is always destroyed before this runs, so there is nothing to
  // rise above, and hosts that put their own siblings in the container must keep
  // the stacking they chose.
  SetWindowPos(child, nullptr, 0, 0, lastWidth_, lastHeight_,
               SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOZORDER | SWP_SHOWWINDOW);
  // CHOC forwards bounds and visibility to the WebView2 controller only from
  // WM_SIZE and WM_SHOWWINDOW, and drops both while the controller is still
  // being created. Neither message is re-sent for a window that is already
  // visible at the right size, so send them explicitly rather than leaving the
  // controller zero-sized or invisible in hosts that size and show the container
  // before WebView2 is up.
  //
  // A WebView built during initialize() belongs to whichever thread ran it, which
  // need not be this one, and a plain SendMessage to another thread's window
  // blocks for as long as that thread takes. Bound the wait so replaying these
  // two messages can never stall the editor thread.
  constexpr UINT sendFlags = SMTO_NORMAL | SMTO_ABORTIFHUNG;
  constexpr UINT sendTimeoutMilliseconds = 1000;
  SendMessageTimeoutW(child, WM_SHOWWINDOW, TRUE, 0, sendFlags,
                      sendTimeoutMilliseconds, nullptr);
  SendMessageTimeoutW(child, WM_SIZE, SIZE_RESTORED,
                      MAKELPARAM(static_cast<WORD>(lastWidth_),
                                 static_cast<WORD>(lastHeight_)),
                      sendFlags, sendTimeoutMilliseconds, nullptr);
#endif
}

bool WebViewHost::attach(void *owner, void *parent, const std::int32_t width,
                         const std::int32_t height) {
  if (owner == nullptr || parent == nullptr) {
    return false;
  }
#if defined(_WIN32)
  auto *parentWindow = static_cast<HWND>(parent);
  if (IsWindow(parentWindow) == FALSE) {
    return false;
  }
#endif

  endInitialisationWatch();
  owner_ = nullptr;
  parent_ = nullptr;
  lastWidth_ = std::max(1, width);
  lastHeight_ = std::max(1, height);
  destroyDiagnosticView();

  // The WebView2 controller must come into existence while the CHOC window is
  // already parented to the host window it will be shown in, or some hosts leave
  // the reopened editor blank. A ready WebView's controller was created under the
  // previous parent - and may also have hosted state work while the editor was
  // closed - so it is replaced rather than reparented. One whose construction is
  // still pending has no controller yet, so its controller will still land after
  // the SetParent below; replacing that one would only race two WebView2
  // environment creations for the same profile.
  if (webView_ != nullptr && webView_->isReady()) {
    replaceWebView();
  }
  if (!ensureWebView()) {
    owner_ = owner;
    parent_ = parent;
    if (showDiagnosticView()) {
      return true;
    }
    owner_ = nullptr;
    parent_ = nullptr;
    return false;
  }
  auto *view = webView_->getViewHandle();
  if (view == nullptr) {
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
    replaceWebView();
    return false;
  }
  if (SetWindowPos(child, nullptr, 0, 0, lastWidth_, lastHeight_,
                   SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOZORDER |
                       SWP_SHOWWINDOW) == FALSE) {
    replaceWebView();
    return false;
  }
  owner_ = owner;
  parent_ = parent;
  beginInitialisationWatch();
  return true;
#elif defined(__APPLE__)
  using AddSubview = void (*)(id, SEL, id);
  reinterpret_cast<AddSubview>(objc_msgSend)(static_cast<id>(parent),
                                             sel_registerName("addSubview:"),
                                             static_cast<id>(view));
  owner_ = owner;
  parent_ = parent;
  resize(owner, width, height);
  beginInitialisationWatch();
  return true;
#else
  (void)width;
  (void)height;
  return false;
#endif
}

void WebViewHost::beginInitialisationWatch() noexcept {
  attachedAt_ = std::chrono::steady_clock::now();
#if defined(_WIN32)
  attachedThread_ = GetCurrentThreadId();
#endif
  if (webView_ != nullptr && webView_->isReady()) {
    status_ = WebViewStatus::ready;
    presentNativeView();
    return;
  }
  status_ = WebViewStatus::initialising;
#if defined(_WIN32)
  if (watchdogTimer_ == 0) {
    if (const auto timerId = SetTimer(nullptr, 0, kWatchdogIntervalMilliseconds,
                                      watchdogTimerProc);
        timerId != 0) {
      watchdogTimer_ = static_cast<std::uintptr_t>(timerId);
      const std::scoped_lock lock(watchdogMutex());
      watchdogRegistry()[{attachedThread_, timerId}] = this;
    }
  }
#endif
}

void WebViewHost::endInitialisationWatch() noexcept {
#if defined(_WIN32)
  if (watchdogTimer_ == 0) {
    return;
  }
  const auto timerId = static_cast<UINT_PTR>(watchdogTimer_);
  watchdogTimer_ = 0;
  {
    const std::scoped_lock lock(watchdogMutex());
    watchdogRegistry().erase({attachedThread_, timerId});
  }
  // KillTimer only works from the thread that owns the timer. Off-thread the
  // registry entry above is already gone, so a stray tick resolves to no host
  // and does nothing.
  if (attachedThread_ == GetCurrentThreadId()) {
    KillTimer(nullptr, timerId);
  }
#endif
}

bool WebViewHost::callerOwnsEditorWindows() const noexcept {
#if defined(_WIN32)
  return attachedThread_ == GetCurrentThreadId();
#else
  return true;
#endif
}

void WebViewHost::serviceInitialisation() noexcept {
  if (parent_ == nullptr || !callerOwnsEditorWindows()) {
    // Everything below manipulates windows, which have thread affinity, so any
    // other thread has to leave the work to the editor's own watchdog timer.
    return;
  }
  if (webViewIsUsable() && webView_->isReady()) {
    if (status_ != WebViewStatus::ready) {
      // A slow-but-working WebView2 takes the window back from the diagnostics.
      status_ = WebViewStatus::ready;
      destroyDiagnosticView();
      presentNativeView();
    }
    endInitialisationWatch();
    return;
  }
  if (status_ == WebViewStatus::initialising) {
    if (std::chrono::steady_clock::now() - attachedAt_ < kWebViewInitialisationTimeout) {
      return;
    }
    status_ = WebViewStatus::initialisationTimedOut;
  }
  if (!diagnosticVisible_) {
    // The pending WebView is deliberately left running underneath: if it does
    // finish, the branch above hands the window straight back to it.
    (void)showDiagnosticView();
  }
}

bool WebViewHost::showDiagnosticView() noexcept {
#if defined(_WIN32)
  if (parent_ == nullptr) {
    return false;
  }
  auto *parentWindow = static_cast<HWND>(parent_);
  if (IsWindow(parentWindow) == FALSE) {
    return false;
  }
  const auto text = widen(diagnosticText(status_));
  if (diagnosticView_ != nullptr) {
    auto *existing = static_cast<HWND>(diagnosticView_);
    SetWindowTextW(existing, text.c_str());
    SetWindowPos(existing, HWND_TOP, 0, 0, lastWidth_, lastHeight_,
                 SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
    diagnosticVisible_ = true;
    return true;
  }
  // A read-only multi-line EDIT control needs no window class of the plug-in's
  // own, so no window procedure can outlive an unloaded binary, and the user can
  // select the text and paste it straight into a bug report.
  auto *created = CreateWindowExW(
      0, L"EDIT", text.c_str(),
      WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY |
          ES_AUTOVSCROLL,
      0, 0, lastWidth_, lastHeight_, parentWindow, nullptr, nullptr, nullptr);
  if (created == nullptr) {
    return false;
  }
  if (diagnosticFont_ == nullptr) {
    diagnosticFont_ = createDiagnosticFont();
  }
  if (diagnosticFont_ != nullptr) {
    SendMessageW(created, WM_SETFONT, reinterpret_cast<WPARAM>(diagnosticFont_), TRUE);
  }
  SendMessageW(created, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
               MAKELPARAM(16, 16));
  diagnosticView_ = created;
  diagnosticVisible_ = true;
  return true;
#else
  return false;
#endif
}

void WebViewHost::destroyDiagnosticView() noexcept {
  diagnosticVisible_ = false;
#if defined(_WIN32)
  if (diagnosticView_ != nullptr) {
    auto *view = static_cast<HWND>(diagnosticView_);
    diagnosticView_ = nullptr;
    if (IsWindow(view) != FALSE) {
      DestroyWindow(view);
    }
  }
  if (diagnosticFont_ != nullptr) {
    DeleteObject(static_cast<HGDIOBJ>(diagnosticFont_));
    diagnosticFont_ = nullptr;
  }
#endif
}

void WebViewHost::resize(void *owner, const std::int32_t width,
                         const std::int32_t height) noexcept {
  if (owner == nullptr || owner != owner_) {
    return;
  }
  lastWidth_ = std::max(1, width);
  lastHeight_ = std::max(1, height);
#if defined(_WIN32)
  if (diagnosticView_ != nullptr) {
    SetWindowPos(static_cast<HWND>(diagnosticView_), nullptr, 0, 0, lastWidth_,
                 lastHeight_, SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOZORDER);
  }
#endif
  if (webView_ == nullptr || webView_->getViewHandle() == nullptr) {
    return;
  }
#if defined(_WIN32)
  auto *child = static_cast<HWND>(webView_->getViewHandle());
  if (IsWindow(child) != FALSE) {
    SetWindowPos(child, nullptr, 0, 0, lastWidth_, lastHeight_,
                 SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOZORDER);
  }
#elif defined(__APPLE__)
  using SetFrame = void (*)(id, SEL, CGRect);
  reinterpret_cast<SetFrame>(objc_msgSend)(static_cast<id>(webView_->getViewHandle()),
                                           sel_registerName("setFrame:"),
                                           CGRectMake(0, 0, std::max(1, width),
                                                      std::max(1, height)));
#else
  (void)width;
  (void)height;
#endif
}

void WebViewHost::detach(void *owner) noexcept {
  if (owner == nullptr || owner != owner_) {
    return;
  }
  if (webView_ != nullptr && webView_->getViewHandle() != nullptr && parent_ != nullptr) {
#if defined(__APPLE__)
    using RemoveFromSuperview = void (*)(id, SEL);
    reinterpret_cast<RemoveFromSuperview>(objc_msgSend)(
        static_cast<id>(webView_->getViewHandle()), sel_registerName("removeFromSuperview"));
#endif
  }
  endInitialisationWatch();
  destroyDiagnosticView();
  owner_ = nullptr;
  parent_ = nullptr;
  status_ = WebViewStatus::initialising;
  replaceWebView();
}

bool WebViewHost::loaded() const noexcept {
  return webView_ != nullptr && webView_->loadedOK();
}

bool WebViewHost::evaluate(std::string script, EvaluationHandler handler) {
  if (webView_ == nullptr || !webView_->isReady()) {
    return false;
  }
  return webView_->evaluateJavascript(
      script, [handler = std::move(handler)](const std::string &error,
                                             const choc::value::ValueView &result) {
        if (handler) {
          handler(error, choc::json::toString(result));
        }
      });
}

} // namespace effetune::vst
