#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace choc::ui {
class WebView;
}

namespace effetune::vst {

class WebViewHost {
public:
  using MessageHandler = std::function<std::string(std::string_view)>;
  using EvaluationHandler = std::function<void(std::string, std::string)>;

  explicit WebViewHost(MessageHandler handler,
                       std::filesystem::path resourceRoot = {});
  ~WebViewHost();
  WebViewHost(const WebViewHost &) = delete;
  WebViewHost &operator=(const WebViewHost &) = delete;

  [[nodiscard]] bool attach(void *owner, void *parent, std::int32_t width,
                            std::int32_t height);
  void resize(void *owner, std::int32_t width, std::int32_t height) noexcept;
  void detach(void *owner) noexcept;
  [[nodiscard]] bool loaded() const noexcept;
  [[nodiscard]] bool evaluate(std::string script, EvaluationHandler handler);

private:
  struct ResourceData {
    std::string mimeType;
    std::string bytes;
  };

  [[nodiscard]] static std::filesystem::path locateResourceRoot();
  [[nodiscard]] std::optional<ResourceData> fetchResource(std::string path) const;
  void createWebView();
  void replaceWebView() noexcept;
  [[nodiscard]] bool ensureWebView();
  void configure(choc::ui::WebView &view);

  MessageHandler handler_;
  std::filesystem::path resourceRoot_;
  std::unique_ptr<choc::ui::WebView> webView_;
  void *owner_ = nullptr;
  void *parent_ = nullptr;
};

} // namespace effetune::vst
