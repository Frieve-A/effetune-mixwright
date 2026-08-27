#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace effetune::vst {

struct WebViewHostState;

/// How long the asynchronous WebView2 construction is allowed to take before the
/// editor gives up waiting silently and puts native diagnostics in front of the
/// user. A cold WebView2 start on a slow disk is comfortably inside this.
inline constexpr std::chrono::seconds kWebViewInitialisationTimeout{12};

/// Why the editor is not currently showing the EffeTune UI.
///
/// Only `ready` and `initialising` are normal. Every other value is a state the
/// WebView cannot report itself - a WebView that never starts cannot render an
/// error page - so it is surfaced through a native diagnostic view instead of an
/// empty window.
enum class WebViewStatus : std::uint8_t {
  /// The WebView2 controller exists and the UI has been navigated.
  ready,
  /// Asynchronous WebView2 construction is still in flight.
  initialising,
  /// Windows only: the calling thread has already joined a multi-threaded COM
  /// apartment, which WebView2 cannot run on.
  unsupportedApartment,
  /// The native view could not be created at all, usually a missing or broken
  /// Microsoft Edge WebView2 Runtime.
  runtimeUnavailable,
  /// Construction started but never completed within kWebViewInitialisationTimeout.
  initialisationTimedOut,
};

class WebViewHost {
public:
  using MessageHandler = std::function<std::string(std::string_view)>;
  using EvaluationHandler = std::function<void(std::string, std::string)>;

  explicit WebViewHost(MessageHandler handler,
                       std::filesystem::path resourceRoot = {},
                       bool createImmediately = true);
  ~WebViewHost();
  WebViewHost(const WebViewHost &) = delete;
  WebViewHost &operator=(const WebViewHost &) = delete;

  /// Stops callbacks and retires the current platform generation. Safe to call
  /// repeatedly and before outstanding shared leases release the host object.
  void shutdown() noexcept;

  [[nodiscard]] bool attach(void *owner, void *parent, std::int32_t width,
                            std::int32_t height);
  void resize(void *owner, std::int32_t width, std::int32_t height) noexcept;
  void detach(void *owner) noexcept;
  [[nodiscard]] bool loaded() const noexcept;
  [[nodiscard]] bool evaluate(std::string script, EvaluationHandler handler);

  [[nodiscard]] WebViewStatus status() const noexcept;

  /// Converts a WebView2 construction that never completes into visible
  /// diagnostics, and hands the window back to the WebView if it does eventually
  /// come up.
  ///
  /// An attached editor drives this itself, from a timer owned by the thread that
  /// attached - the same thread whose message loop WebView2 needs in order to
  /// finish starting at all. Calling it explicitly is only useful for tests; it
  /// is a no-op while nothing is attached and on any thread other than the one
  /// that attached, because everything it touches is thread-affine window state.
  void serviceInitialisation() noexcept;

  /// The text shown to the user for a failed status, including the host
  /// executable and a stable diagnostic code so it can be pasted into a report.
  [[nodiscard]] static std::string diagnosticText(WebViewStatus status);

private:
  std::shared_ptr<WebViewHostState> state_;
};

} // namespace effetune::vst
