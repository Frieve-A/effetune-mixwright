#include "external_url.h"

#include <algorithm>
#include <limits>
#include <string>
#include <utility>

#if defined(_WIN32)
#include <Windows.h>
#include <shellapi.h>
#elif defined(__APPLE__)
#include <objc/message.h>
#include <objc/runtime.h>
#endif

namespace effetune::vst::plugin {
namespace {

void setError(std::string *destination, std::string message) {
  if (destination != nullptr) {
    *destination = std::move(message);
  }
}

[[nodiscard]] bool isAllowedExternalUrl(const std::string_view url) {
  if (url.empty() || url.size() > 4096u ||
      (!url.starts_with("https://") && !url.starts_with("http://"))) {
    return false;
  }
  return std::none_of(url.begin(), url.end(), [](const unsigned char value) {
    return value <= 0x20u || value == 0x7fu;
  });
}

#if defined(_WIN32)
[[nodiscard]] std::wstring utf8ToWide(const std::string_view value) {
  if (value.empty() ||
      value.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
    return {};
  }
  const auto size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                        static_cast<int>(value.size()), nullptr, 0);
  if (size <= 0) {
    return {};
  }
  std::wstring result(static_cast<std::size_t>(size), L'\0');
  if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                          static_cast<int>(value.size()), result.data(), size) != size) {
    return {};
  }
  return result;
}
#endif

} // namespace

bool openExternalUrl(const std::string_view url, std::string *error) {
  if (!isAllowedExternalUrl(url)) {
    setError(error, "The external link is not a valid HTTP or HTTPS URL");
    return false;
  }

#if defined(_WIN32)
  const auto wideUrl = utf8ToWide(url);
  if (wideUrl.empty()) {
    setError(error, "The external link contains invalid text");
    return false;
  }
  const auto result = ShellExecuteW(nullptr, L"open", wideUrl.c_str(), nullptr, nullptr,
                                    SW_SHOWNORMAL);
  if (reinterpret_cast<INT_PTR>(result) <= 32) {
    setError(error, "Unable to open the link in the default browser");
    return false;
  }
  return true;
#elif defined(__APPLE__)
  using SendId = id (*)(id, SEL);
  using SendIdCString = id (*)(id, SEL, const char *);
  using SendIdId = id (*)(id, SEL, id);
  using SendBoolId = BOOL (*)(id, SEL, id);

  const auto stringClass = reinterpret_cast<id>(objc_getClass("NSString"));
  const auto urlClass = reinterpret_cast<id>(objc_getClass("NSURL"));
  const auto workspaceClass = reinterpret_cast<id>(objc_getClass("NSWorkspace"));
  const auto utf8 = std::string(url);
  const auto urlString = reinterpret_cast<SendIdCString>(objc_msgSend)(
      stringClass, sel_registerName("stringWithUTF8String:"), utf8.c_str());
  const auto nativeUrl = reinterpret_cast<SendIdId>(objc_msgSend)(
      urlClass, sel_registerName("URLWithString:"), urlString);
  const auto workspace = reinterpret_cast<SendId>(objc_msgSend)(
      workspaceClass, sel_registerName("sharedWorkspace"));
  if (urlString == nullptr || nativeUrl == nullptr || workspace == nullptr ||
      reinterpret_cast<SendBoolId>(objc_msgSend)(workspace, sel_registerName("openURL:"),
                                                 nativeUrl) == NO) {
    setError(error, "Unable to open the link in the default browser");
    return false;
  }
  return true;
#else
  setError(error, "External links are not supported on this platform");
  return false;
#endif
}

} // namespace effetune::vst::plugin
