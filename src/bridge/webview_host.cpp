#include "bridge/webview_host.h"

#include "choc/gui/choc_WebView.h"
#include "choc/text/choc_JSON.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <iterator>
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

} // namespace

WebViewHost::WebViewHost(MessageHandler handler, std::filesystem::path resourceRoot)
    : handler_(std::move(handler)),
      resourceRoot_(resourceRoot.empty() ? locateResourceRoot() : std::move(resourceRoot)),
      resourceBundleAvailable_(hasRequiredResources(resourceRoot_)) {
  createWebView();
}

WebViewHost::~WebViewHost() { webView_.reset(); }

void WebViewHost::createWebView() {
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

bool WebViewHost::ensureWebView() {
  auto valid = webView_ != nullptr && webView_->loadedOK() &&
               webView_->getViewHandle() != nullptr;
#if defined(_WIN32)
  valid = valid && IsWindow(static_cast<HWND>(webView_->getViewHandle())) != FALSE;
#endif
  if (valid) {
    return true;
  }

  replaceWebView();

  valid = webView_ != nullptr && webView_->loadedOK() &&
          webView_->getViewHandle() != nullptr;
#if defined(_WIN32)
  valid = valid && IsWindow(static_cast<HWND>(webView_->getViewHandle())) != FALSE;
#endif
  return valid;
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

  // Construct the visible WebView only after the host has supplied the current
  // native parent. A hidden WebView may have been used for state work while the
  // editor was closed, but it is never reused for a visible attachment.
  owner_ = nullptr;
  parent_ = nullptr;
  replaceWebView();
  if (!ensureWebView()) {
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
  if (SetWindowPos(child, nullptr, 0, 0, std::max(1, width), std::max(1, height),
                   SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOZORDER |
                       SWP_SHOWWINDOW) == FALSE) {
    replaceWebView();
    return false;
  }
  owner_ = owner;
  parent_ = parent;
  return true;
#elif defined(__APPLE__)
  using AddSubview = void (*)(id, SEL, id);
  reinterpret_cast<AddSubview>(objc_msgSend)(static_cast<id>(parent),
                                             sel_registerName("addSubview:"),
                                             static_cast<id>(view));
  owner_ = owner;
  parent_ = parent;
  resize(owner, width, height);
  return true;
#else
  (void)width;
  (void)height;
  return false;
#endif
}

void WebViewHost::resize(void *owner, const std::int32_t width,
                         const std::int32_t height) noexcept {
  if (owner == nullptr || owner != owner_ || webView_ == nullptr ||
      webView_->getViewHandle() == nullptr) {
    return;
  }
#if defined(_WIN32)
  auto *child = static_cast<HWND>(webView_->getViewHandle());
  if (IsWindow(child) != FALSE) {
    SetWindowPos(child, nullptr, 0, 0, std::max(1, width), std::max(1, height),
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
  owner_ = nullptr;
  parent_ = nullptr;
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
