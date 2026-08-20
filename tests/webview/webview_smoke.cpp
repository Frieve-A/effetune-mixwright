#include "bridge/webview_host.h"
#include "plugin/editor_size.h"

#include <Windows.h>
#include <TlHelp32.h>

#include <atomic>
#include <chrono>
#include <exception>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <vector>

#include "../support/crt_dialog_suppression.h"

namespace {

class ScopedWebViewProfile {
public:
  ScopedWebViewProfile() {
    constexpr wchar_t environmentName[] = L"WEBVIEW2_USER_DATA_FOLDER";
    const auto previousLength =
        GetEnvironmentVariableW(environmentName, nullptr, 0);
    if (previousLength != 0) {
      previousValue_.resize(previousLength);
      const auto copied = GetEnvironmentVariableW(
          environmentName, previousValue_.data(), previousLength);
      if (copied != 0 && copied < previousLength) {
        previousValue_.resize(copied);
        hadPreviousValue_ = true;
      }
    }
    std::error_code error;
    const auto tempRoot = std::filesystem::temp_directory_path(error);
    if (error) {
      return;
    }
    const auto prefix = L"effetune-webview-smoke-" +
                        std::to_wstring(GetCurrentProcessId()) + L"-" +
                        std::to_wstring(GetTickCount64());
    for (std::uint32_t attempt = 0; attempt < 32; ++attempt) {
      auto candidate = tempRoot / (prefix + L"-" + std::to_wstring(attempt));
      error.clear();
      if (std::filesystem::create_directory(candidate, error)) {
        path_ = std::move(candidate);
        break;
      }
      if (error) {
        return;
      }
    }
    if (path_.empty() ||
        SetEnvironmentVariableW(environmentName, path_.c_str()) == FALSE) {
      (void)SetEnvironmentVariableW(
          environmentName, hadPreviousValue_ ? previousValue_.c_str() : nullptr);
      error.clear();
      if (!path_.empty()) {
        std::filesystem::remove(path_, error);
      }
      path_.clear();
      return;
    }
    configured_ = true;
  }

  ~ScopedWebViewProfile() {
    if (!cleanupAttempted_) {
      (void)cleanup();
    }
  }

  ScopedWebViewProfile(const ScopedWebViewProfile &) = delete;
  ScopedWebViewProfile &operator=(const ScopedWebViewProfile &) = delete;

  [[nodiscard]] bool configured() const noexcept { return configured_; }
  [[nodiscard]] const std::filesystem::path &path() const noexcept { return path_; }

  void captureChildProcesses() {
    bool scanSucceeded = false;
    const auto descendants = currentDescendants(scanSucceeded);
    if (!scanSucceeded) {
      processScanFailed_ = true;
      return;
    }
    for (const auto processId : descendants) {
      if (capturedProcessIds_.contains(processId)) {
        continue;
      }
      const auto process = OpenProcess(SYNCHRONIZE, FALSE, processId);
      if (process != nullptr) {
        capturedProcessIds_.insert(processId);
        childProcesses_.push_back(process);
      }
    }
  }

  [[nodiscard]] bool cleanup() {
    cleanupAttempted_ = true;
    restoreEnvironment();
    captureChildProcesses();

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(10);
    bool childrenExited = false;
    while (std::chrono::steady_clock::now() < deadline) {
      captureChildProcesses();
      childrenExited = true;
      for (const auto process : childProcesses_) {
        if (WaitForSingleObject(process, 0) == WAIT_TIMEOUT) {
          childrenExited = false;
          break;
        }
      }
      bool scanSucceeded = false;
      const auto descendants = currentDescendants(scanSucceeded);
      if (childrenExited && scanSucceeded && descendants.empty()) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    for (const auto process : childProcesses_) {
      CloseHandle(process);
    }
    childProcesses_.clear();

    bool finalScanSucceeded = false;
    const auto finalDescendants = currentDescendants(finalScanSucceeded);
    if (processScanFailed_ || !childrenExited || !finalScanSucceeded ||
        !finalDescendants.empty()) {
      return false;
    }
    std::error_code error;
    std::filesystem::remove_all(path_, error);
    return !error && !std::filesystem::exists(path_, error);
  }

private:
  [[nodiscard]] static std::unordered_set<DWORD>
  currentDescendants(bool &scanSucceeded) {
    std::unordered_set<DWORD> descendants;
    const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
      return descendants;
    }

    struct ProcessEntry {
      DWORD id = 0;
      DWORD parentId = 0;
      std::wstring executable;
    };
    std::vector<ProcessEntry> processes;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry) == FALSE) {
      CloseHandle(snapshot);
      return descendants;
    }
    do {
      processes.push_back({entry.th32ProcessID, entry.th32ParentProcessID,
                           entry.szExeFile});
    } while (Process32NextW(snapshot, &entry) != FALSE);
    CloseHandle(snapshot);
    scanSucceeded = true;

    std::unordered_set<DWORD> ancestry{GetCurrentProcessId()};
    bool changed = true;
    while (changed) {
      changed = false;
      for (const auto &process : processes) {
        if (ancestry.contains(process.parentId) &&
            ancestry.insert(process.id).second) {
          changed = true;
        }
      }
    }
    for (const auto &process : processes) {
      if (process.id != GetCurrentProcessId() && ancestry.contains(process.id) &&
          _wcsicmp(process.executable.c_str(), L"msedgewebview2.exe") == 0) {
        descendants.insert(process.id);
      }
    }
    return descendants;
  }

  void restoreEnvironment() noexcept {
    if (!configured_) {
      return;
    }
    constexpr wchar_t environmentName[] = L"WEBVIEW2_USER_DATA_FOLDER";
    (void)SetEnvironmentVariableW(
        environmentName, hadPreviousValue_ ? previousValue_.c_str() : nullptr);
    configured_ = false;
  }

  std::filesystem::path path_;
  std::wstring previousValue_;
  std::unordered_set<DWORD> capturedProcessIds_;
  std::vector<HANDLE> childProcesses_;
  bool hadPreviousValue_ = false;
  bool configured_ = false;
  bool cleanupAttempted_ = false;
  bool processScanFailed_ = false;
};

LRESULT CALLBACK parentWindowProcedure(HWND window, const UINT message, const WPARAM wParam,
                                       const LPARAM lParam) {
  return DefWindowProcW(window, message, wParam, lParam);
}

struct SmokeState {
  std::atomic_uint32_t hostInfoCalls{0};
  std::atomic_uint32_t telemetryCalls{0};
  std::atomic_uint32_t telemetryDiscardCalls{0};
  std::atomic_uint32_t pipelineRebuildCalls{0};
  std::atomic_uint32_t prematureEmptyRebuildCalls{0};
  std::atomic_uint32_t historyRestoreCalls{0};
  std::atomic_uint32_t masterBypassCalls{0};
  std::atomic_uint32_t externalAppHelpCalls{0};
  std::atomic_uint32_t externalGitHubCalls{0};
  std::atomic_uint32_t externalHelpCalls{0};
  std::atomic_uint32_t remappedExternalHelpCalls{0};
  std::atomic_uint32_t externalAiCalls{0};
  std::atomic_uint32_t invalidExternalDocumentationCalls{0};
  std::atomic_uint64_t contextGeneration{1};
  std::atomic_uint32_t engineSampleRate{48000};
  std::atomic_uint32_t channels{2};
  std::atomic_bool lastMasterBypass{false};
  std::atomic_bool pipelineStateRead{false};
};

[[nodiscard]] std::string responseFor(const std::string_view request, SmokeState &state) {
  if (request.find("host/getInfo") != std::string_view::npos) {
    state.hostInfoCalls.fetch_add(1, std::memory_order_relaxed);
    return "{\"ok\":true,\"sampleRate\":48000,\"engineSampleRate\":" +
           std::to_string(state.engineSampleRate.load(std::memory_order_relaxed)) +
           ",\"channels\":" + std::to_string(state.channels.load(std::memory_order_relaxed)) +
           ",\"oversamplingFactor\":1,\"latencySamples\":0,\"masterBypass\":false,"
           "\"dspReady\":true,"
           "\"contextGeneration\":" +
           std::to_string(state.contextGeneration.load(std::memory_order_relaxed)) +
           ",\"version\":\"test\"}";
  }
  if (request.find("storage/fileExists") != std::string_view::npos) {
    if (request.find("pipeline-state.json") != std::string_view::npos) {
      return R"({"ok":true,"exists":true})";
    }
    return R"({"ok":true,"exists":false})";
  }
  if (request.find("storage/readFile") != std::string_view::npos) {
    if (request.find("pipeline-state.json") != std::string_view::npos) {
      state.pipelineStateRead.store(true, std::memory_order_release);
      return R"({"ok":true,"success":true,"content":"{\"pipelineA\":[],\"pipelineB\":[{\"name\":\"UnavailableFutureEffect\",\"enabled\":true,\"parameters\":{\"future\":1}}],\"currentPipeline\":\"A\"}"})";
    }
    return R"({"ok":true,"success":false,"content":"{}"})";
  }
  if (request.find("config/load") != std::string_view::npos) {
    return R"({"ok":true,"config":{"startupView":"library","columns":2}})";
  }
  if (request.find("telemetry/read") != std::string_view::npos) {
    state.telemetryCalls.fetch_add(1, std::memory_order_relaxed);
    return "{\"ok\":true,\"bytes\":16,\"droppedFrames\":0,"
           "\"packet\":\"AQABAJIQAAABAAAAAAAAAA==\",\"masterBypass\":false,"
           "\"contextGeneration\":" +
           std::to_string(state.contextGeneration.load(std::memory_order_relaxed)) +
           ",\"engineSampleRate\":" +
           std::to_string(state.engineSampleRate.load(std::memory_order_relaxed)) +
           ",\"channels\":" + std::to_string(state.channels.load(std::memory_order_relaxed)) +
           "}";
  }
  if (request.find("telemetry/discard") != std::string_view::npos) {
    state.telemetryDiscardCalls.fetch_add(1, std::memory_order_relaxed);
    return R"({"ok":true})";
  }
  if (request.find("pipeline/rebuild") != std::string_view::npos) {
    state.pipelineRebuildCalls.fetch_add(1, std::memory_order_relaxed);
    if (!state.pipelineStateRead.load(std::memory_order_acquire) &&
        request.find("\"plugins\":[]") != std::string_view::npos) {
      state.prematureEmptyRebuildCalls.fetch_add(1, std::memory_order_relaxed);
    }
    return R"({"ok":true})";
  }
  if (request.find("pipeline/restoreHistory") != std::string_view::npos) {
    state.historyRestoreCalls.fetch_add(1, std::memory_order_relaxed);
    return R"({"ok":true})";
  }
  if (request.find("pipeline/masterBypass") != std::string_view::npos) {
    state.masterBypassCalls.fetch_add(1, std::memory_order_relaxed);
    state.lastMasterBypass.store(request.find("\"value\":true") != std::string_view::npos,
                                 std::memory_order_relaxed);
    return R"({"ok":true})";
  }
  if (request.find("host/openExternal") != std::string_view::npos) {
    if (request.find("\"url\":\"https://effetune.frieve.com/\"") !=
        std::string_view::npos) {
      state.externalAppHelpCalls.fetch_add(1, std::memory_order_relaxed);
    }
    if (request.find(
            "\"url\":\"https://github.com/Frieve-A/effetune-mixwright\"") !=
        std::string_view::npos) {
      state.externalGitHubCalls.fetch_add(1, std::memory_order_relaxed);
    }
    if (request.find(
            "effetune.frieve.com/docs/i18n/ja/plugins/basics.html#volume") !=
        std::string_view::npos) {
      state.externalHelpCalls.fetch_add(1, std::memory_order_relaxed);
    }
    if (request.find(
            "effetune.frieve.com/docs/i18n/ja/plugins/analyzer.html#level-meter") !=
        std::string_view::npos) {
      state.remappedExternalHelpCalls.fetch_add(1, std::memory_order_relaxed);
    }
    if (request.find("chatgpt.com") != std::string_view::npos) {
      state.externalAiCalls.fetch_add(1, std::memory_order_relaxed);
    }
    if (request.find("choc.localhost") != std::string_view::npos ||
        request.find("effetune-mixwright.localhost") != std::string_view::npos ||
        request.find("effetune-mixwright:") != std::string_view::npos ||
        request.find("analyzer.md") != std::string_view::npos) {
      state.invalidExternalDocumentationCalls.fetch_add(1, std::memory_order_relaxed);
    }
    return R"({"ok":true})";
  }
  return R"({"ok":true,"success":true})";
}

} // namespace

int main() {
  effetune::vst::testing::suppressCrtModalDialogs();
  const auto errorMode = SetErrorMode(0);
  SetErrorMode(errorMode | SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
  ScopedWebViewProfile webViewProfile;
  if (!webViewProfile.configured()) {
    std::cerr << "Unable to create an isolated WebView2 user-data profile\n";
    return 1;
  }
  const auto instance = GetModuleHandleW(nullptr);
  constexpr wchar_t className[] = L"EffeTuneWebViewSmokeParent";
  WNDCLASSW windowClass{};
  windowClass.hInstance = instance;
  windowClass.lpfnWndProc = parentWindowProcedure;
  windowClass.lpszClassName = className;
  if (RegisterClassW(&windowClass) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    std::cerr << "Unable to register the WebView smoke parent class\n";
    return 1;
  }
  const auto createParent = [instance, className] {
    return CreateWindowExW(0, className, L"EffeTune WebView Smoke",
                           WS_OVERLAPPEDWINDOW, -30000, -30000, 900, 650,
                           nullptr, nullptr, instance, nullptr);
  };
  auto parent = createParent();
  if (parent == nullptr) {
    std::cerr << "Unable to create the WebView smoke parent window\n";
    return 1;
  }

  SmokeState smokeState;
  std::atomic_bool evaluationComplete{false};
  std::atomic_bool evaluationPending{false};
  std::atomic_bool uiReady{false};
  std::string evaluationError;
  std::string evaluationResult;
  std::string readinessDiagnostics;
  bool telemetryDelivered = false;
  bool dynamicLatencyServiced = false;
  bool historyUsedSingleNativeRestore = false;
  bool updatePluginRoutedByOwnership = false;
  bool unsupportedStateWarningShown = false;
  bool hiddenContextSynchronized = false;
  bool hiddenTelemetryDiscarded = false;
  bool contextMenuDisabled = false;
  bool clipboardReachable = false;
  bool libraryEntriesDisabled = false;
  bool headerControlsAreVstSpecific = false;
  bool configDialogIsLanguageOnly = false;
  bool languageChangedImmediately = false;
  bool measurementImportAvailable = false;
  bool aboutNoticesReachable = false;
  bool externalLinksRouted = false;
  bool isolatedWebOrigin = false;
  bool masterToggleReachedNative = false;
  bool defaultViewportFits = false;
  bool reattachedWithFreshWebView = false;
  bool recoveredAfterParentDestruction = false;
  bool reopenedReadyEditorWasRebuilt = false;
  bool missingResourceGuidanceShown = false;
  bool multiThreadedApartmentDiagnosed = false;
  bool editorReportedReady = false;
  bool attached = false;
  {
    int firstEditorOwner = 0;
    int secondEditorOwner = 0;
    int thirdEditorOwner = 0;
    int fourthEditorOwner = 0;
    void *activeEditorOwner = &firstEditorOwner;
    effetune::vst::WebViewHost webView(
        [&smokeState](const std::string_view request) {
          return responseFor(request, smokeState);
        },
        std::filesystem::path(EFFETUNE_WEBVIEW_ASSET_DIR), false);
    attached = webView.attach(activeEditorOwner, parent,
                              effetune::vst::plugin::kDefaultEditorWidth,
                              effetune::vst::plugin::kDefaultEditorHeight);
    if (attached) {
      ShowWindow(parent, SW_SHOWNOACTIVATE);
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
      auto nextEvaluation = std::chrono::steady_clock::now() + std::chrono::seconds(1);
      while ((!uiReady.load(std::memory_order_acquire) ||
              smokeState.hostInfoCalls.load(std::memory_order_relaxed) == 0) &&
             std::chrono::steady_clock::now() < deadline) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != 0) {
          TranslateMessage(&message);
          DispatchMessageW(&message);
        }
        if (!evaluationPending.load(std::memory_order_acquire) &&
            std::chrono::steady_clock::now() >= nextEvaluation) {
          evaluationPending.store(true, std::memory_order_release);
          const auto requested = webView.evaluate(
              "Object.keys(window.pluginManager?.pluginClasses || {}).length",
              [&](std::string error, std::string result) {
                evaluationError = std::move(error);
                evaluationResult = std::move(result);
                try {
                  uiReady.store(std::stoul(evaluationResult) >= 60u,
                                std::memory_order_release);
                } catch (const std::exception &) {
                  uiReady.store(false, std::memory_order_release);
                }
                evaluationComplete.store(true, std::memory_order_release);
                evaluationPending.store(false, std::memory_order_release);
              });
          if (!requested) {
            evaluationPending.store(false, std::memory_order_release);
          }
          nextEvaluation = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }

      auto pumpMessages = [] {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != 0) {
          TranslateMessage(&message);
          DispatchMessageW(&message);
        }
      };
      auto evaluate = [&](std::string script, std::string &result) {
        std::atomic_bool complete{false};
        std::string error;
        if (!webView.evaluate(std::move(script), [&](std::string callbackError,
                                                     std::string callbackResult) {
              error = std::move(callbackError);
              result = std::move(callbackResult);
              complete.store(true, std::memory_order_release);
            })) {
          return false;
        }
        const auto evaluateDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!complete.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < evaluateDeadline) {
          pumpMessages();
          std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return complete.load(std::memory_order_acquire) && error.empty();
      };
      auto waitForJavascript = [&](const std::string &expression,
                                   const std::chrono::seconds timeout) {
        const auto conditionDeadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < conditionDeadline) {
          std::string result;
          if (evaluate(expression, result) && result == "true") {
            return true;
          }
          pumpMessages();
          std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
        return false;
      };

      if (!uiReady.load(std::memory_order_acquire)) {
        (void)evaluate(
            "JSON.stringify({href:window.location.href,readyState:document.readyState,"
            "title:document.title,loadError:document.documentElement.dataset.effetuneLoadError||'',"
            "body:(document.body?.innerText||'').slice(0,240)})",
            readinessDiagnostics);
      }

      if (uiReady.load(std::memory_order_acquire)) {
        std::string ignored;
        isolatedWebOrigin = evaluate(
            "window.location.origin === 'https://effetune-mixwright.localhost'",
            ignored) && ignored == "true";
        webView.serviceInitialisation();
        editorReportedReady =
            webView.status() == effetune::vst::WebViewStatus::ready;
        unsupportedStateWarningShown = waitForJavascript(
            "window.app?.initialized === true && "
            "window.audioManager?.unsupportedWarningShown === true && "
            "document.getElementById('errorDisplay')?.textContent === "
            "'Some effects are unavailable and were bypassed.'",
            std::chrono::seconds(10));
        const auto latencyServiceStarted = std::chrono::steady_clock::now();
        const auto latencyServiceScheduled = evaluate(
            "(() => { const hostCall = window.__effetuneHostCall; "
            "window.__vstLatencyHostInfoCalls = 0; "
            "window.__effetuneHostCall = (type, payload) => { "
            "if (type === 'host/getInfo') window.__vstLatencyHostInfoCalls += 1; "
            "return hostCall(type, payload); }; "
            "window.audioManager.scheduleLatencyService(); "
            "window.audioManager.scheduleLatencyService(); return true; })()",
            ignored);
        dynamicLatencyServiced = latencyServiceScheduled && waitForJavascript(
            "window.__vstLatencyHostInfoCalls === 1", std::chrono::seconds(5)) &&
            std::chrono::steady_clock::now() - latencyServiceStarted >=
                std::chrono::milliseconds(200);

        const auto historyReady = waitForJavascript(
            "window.app?.initialized === true && !!window.audioManager?.workletNode",
            std::chrono::seconds(10));
        const auto historyRestoreStarted = historyReady && evaluate(
            "(() => { const history = window.pipelineManager?.historyManager; "
            "if (!history) return false; "
            "window.__vstHistoryOriginalHostCall = window.__effetuneHostCall; "
            "window.__vstHistoryCalls = []; "
            "window.__effetuneHostCall = (type, payload) => { "
            "if (typeof type === 'string' && type.startsWith('pipeline/')) "
            "window.__vstHistoryCalls.push(type); "
            "return window.__vstHistoryOriginalHostCall(type, payload); }; "
            "history.history = [{ pipelineA: [], pipelineB: null, currentPipeline: 'A' }]; "
            "history.historyIndex = 0; history.loadStateFromHistory(); return true; })()",
            ignored) && ignored == "true";
        const auto historyRequestObserved = historyRestoreStarted && waitForJavascript(
            "window.__vstHistoryCalls?.filter(type => type === 'pipeline/restoreHistory').length "
            "=== 1 && window.__vstHistoryCalls?.filter(type => type === "
            "'pipeline/rebuild').length === 0",
            std::chrono::seconds(5));
        const auto historyDrainDeadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
        while (std::chrono::steady_clock::now() < historyDrainDeadline) {
          pumpMessages();
          std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        std::string historyResult;
        const auto historyRequestVerified = historyRequestObserved && evaluate(
            "window.__vstHistoryCalls?.filter(type => type === 'pipeline/restoreHistory').length "
            "=== 1 && window.__vstHistoryCalls?.filter(type => type === "
            "'pipeline/rebuild').length === 0",
            historyResult) && historyResult == "true";
        std::string historyCleanupResult;
        const auto historyWrapperRestored = evaluate(
            "(() => { const original = window.__vstHistoryOriginalHostCall; "
            "if (typeof original === 'function') window.__effetuneHostCall = original; "
            "delete window.__vstHistoryOriginalHostCall; delete window.__vstHistoryCalls; "
            "return true; })()",
            historyCleanupResult) && historyCleanupResult == "true";
        historyUsedSingleNativeRestore =
            historyReady && historyRestoreStarted && historyRequestVerified && historyWrapperRestored;

        // What is under test here is which pipeline an individual update is
        // routed to, not when the coalesced image leaves. This WebView is parented
        // offscreen and never composited, so it may receive no animation frames at
        // all while document.hidden stays false; publishing the queue explicitly
        // keeps the verdict independent of that. The per-frame coalescing itself is
        // verified deterministically in tests/esm/vst-audio-manager.test.mjs.
        // Staying synchronous also confines the window in which the real pipelines
        // and the real bridge are replaced by stubs to this single evaluation, so a
        // concurrent update from the live UI cannot be silently dropped.
        std::string updateRouteResult;
        updatePluginRoutedByOwnership = evaluate(
            R"JS((() => {
              const audio = window.audioManager;
              if (!audio?.nativePort) return false;
              // Anything the running UI already queued belongs to the real bridge,
              // so it leaves before the stub is installed.
              audio.nativePort.flushPluginUpdates();
              const originalHostCall = window.__effetuneHostCall;
              const restored = {
                pipelineA: audio.pipelineA,
                pipelineB: audio.pipelineB,
                pipeline: audio.pipeline,
                currentPipeline: audio.currentPipeline
              };
              const calls = [];
              try {
                window.__effetuneHostCall = (type, payload) => {
                  if (type !== 'pipeline/updatePlugin') return originalHostCall(type, payload);
                  calls.push(payload);
                  return Promise.resolve({ ok: true });
                };
                audio.pipelineA = [{ id: 6101, name: 'Active A' }];
                audio.pipelineB = [{ id: 6102, name: 'Inactive B' }];
                audio.currentPipeline = 'A';
                audio.pipeline = audio.pipelineA;
                audio.nativePort.postMessage({ type: 'updatePlugin',
                  plugin: { id: 6102, type: 'Gain', enabled: true, parameters: {} } });
                audio.nativePort.postMessage({ type: 'updatePlugin',
                  plugin: { id: 6103, type: 'Gain', enabled: true, parameters: {} } });
                audio.nativePort.flushPluginUpdates();
              } finally {
                audio.pipelineA = restored.pipelineA;
                audio.pipelineB = restored.pipelineB;
                audio.pipeline = restored.pipeline;
                audio.currentPipeline = restored.currentPipeline;
                window.__effetuneHostCall = originalHostCall;
              }
              // The update owned by the inactive pipeline is routed to B, and the
              // one owned by neither is dropped rather than routed anywhere.
              return calls.length === 1 && calls[0].pipeline === 'B' &&
                calls[0].plugin.name === 'Inactive B';
            })())JS",
            updateRouteResult) && updateRouteResult == "true";

        const auto installedTelemetrySubscriber = evaluate(
            "(() => { window.__vstSmokeTelemetryFrames = 0; "
            "window.dspTelemetryHub?.subscribe(4242, 1, () => "
            "{ window.__vstSmokeTelemetryFrames += 1; }); "
            "void window.audioManager?.pollNativeTelemetry(); return true; })()",
            ignored);
        telemetryDelivered = installedTelemetrySubscriber && waitForJavascript(
            "window.__vstSmokeTelemetryFrames > 0", std::chrono::seconds(5));

        const auto hiddenInstalled = evaluate(
            "(() => { Object.defineProperty(document, 'hidden', "
            "{ configurable: true, value: true }); return document.hidden === true; })()",
            ignored);
        const auto rebuildCallsBefore =
            smokeState.pipelineRebuildCalls.load(std::memory_order_relaxed);
        smokeState.engineSampleRate.store(96000, std::memory_order_relaxed);
        smokeState.channels.store(4, std::memory_order_relaxed);
        smokeState.contextGeneration.store(2, std::memory_order_relaxed);
        const auto hiddenPollStarted = evaluate(
            "(() => { void window.audioManager?.pollNativeTelemetry(); return document.hidden; })()",
            ignored);
        hiddenContextSynchronized = hiddenInstalled && hiddenPollStarted && waitForJavascript(
            "window.audioManager?.nativeContextGeneration === 2 && "
            "window.audioManager?.audioContext?.sampleRate === 96000 && "
            "window.audioManager?.audioContext?.destination?.channelCount === 4",
            std::chrono::seconds(5)) &&
            smokeState.pipelineRebuildCalls.load(std::memory_order_relaxed) > rebuildCallsBefore;

        const auto discardCallsBefore =
            smokeState.telemetryDiscardCalls.load(std::memory_order_relaxed);
        const auto resumedTelemetry = evaluate(
            "(() => { window.__vstResumeTelemetryChecked = false; "
            "void (async () => { Object.defineProperty(document, 'hidden', "
            "{ configurable: true, value: false }); "
            "const before = window.__vstSmokeTelemetryFrames; "
            "await window.audioManager.pollNativeTelemetry(); "
            "const afterDiscard = window.__vstSmokeTelemetryFrames; "
            "await window.audioManager.pollNativeTelemetry(); "
            "window.__vstResumeTelemetryChecked = before === afterDiscard && "
            "window.__vstSmokeTelemetryFrames > afterDiscard; })(); return true; })()",
            ignored);
        hiddenTelemetryDiscarded = resumedTelemetry && waitForJavascript(
            "window.__vstResumeTelemetryChecked === true", std::chrono::seconds(5)) &&
            smokeState.telemetryDiscardCalls.load(std::memory_order_relaxed) > discardCallsBefore;

        contextMenuDisabled = evaluate(
            "(() => { const event = new MouseEvent('contextmenu', "
            "{ bubbles: true, cancelable: true }); document.body.dispatchEvent(event); "
            "return event.defaultPrevented; })()",
            ignored) && ignored == "true";

        clipboardReachable = evaluate(
            "(() => !!document.getElementById('pasteButton') && "
            "typeof window.pipelineManager?.clipboardManager?.handlePaste === 'function' && "
            "!String(window.pipelineManager.clipboardManager.handlePaste).includes(\"searchParams.get('dbt')\"))()",
            ignored) && ignored == "true";

        libraryEntriesDisabled = evaluate(
            "(() => { const before = !document.body.classList.contains('view-library'); "
            "const event = new KeyboardEvent('keydown', { key: 'l', ctrlKey: true, "
            "bubbles: true, cancelable: true }); document.dispatchEvent(event); "
            "return before && !event.defaultPrevented && "
            "!document.body.classList.contains('view-library') && "
            "window.appConfig?.startupView === 'effects'; })()",
            ignored) && ignored == "true";

        headerControlsAreVstSpecific = evaluate(
            "(() => { const controls = document.querySelector('.header-buttons > "
            ".vst-os-controls'); const labels = [...(controls?.querySelectorAll("
            "'.vst-os-control > span') || [])].map(label => label.textContent.trim()); "
            "const firstLabelStyle = getComputedStyle(controls?.querySelector("
            "'.vst-os-control > span')); const firstSelectStyle = getComputedStyle("
            "controls?.querySelector('.vst-os-control > select')); "
            "const settings = document.querySelector('.settings-menu-container'); "
            "return !!controls && controls.nextElementSibling === settings && "
            "labels.join('|') === 'Upsampling Factor:|Phase:|Quality:' && "
            "firstLabelStyle.fontSize === '14px' && "
            "firstLabelStyle.color === 'rgb(195, 199, 204)' && "
            "firstSelectStyle.fontSize === '14px' && "
            "firstSelectStyle.backgroundImage !== 'none' && "
            "firstSelectStyle.borderTopColor === 'rgb(86, 86, 86)' && "
            "getComputedStyle(controls.querySelector('.vst-os-control')).flexDirection === "
            "'row' && "
            "getComputedStyle(document.querySelector('.subtitle-container')).display === "
            "'none' && "
            "getComputedStyle(document.getElementById('effectPipelineButton')).display === "
            "'none' && getComputedStyle(document.getElementById('whatsThisLink')).display === "
            "'none'; })()",
            ignored) && ignored == "true";

        const auto languageFixtureStarted = evaluate(
            "(() => { window.__vstLanguageFixtureReady = false; void (async () => { "
            "await window.uiManager.setLanguagePreference('en', { persist: false }); "
            "const plugin = window.pluginManager.createPlugin('Volume'); "
            "if (!plugin) return; window.__vstLanguagePluginId = plugin.id; "
            "window.audioManager.pipeline.push(plugin); "
            "window.uiManager.pipelineManager.updatePipelineUI(true); "
            "window.__vstLanguageFixtureReady = "
            "document.querySelector('.delete-button')?.title === 'Delete effect' && "
            "document.getElementById('effectCount')?.textContent.includes('effects available'); "
            "})(); return true; })()",
            ignored) && ignored == "true";
        const auto languageFixtureReady = languageFixtureStarted && waitForJavascript(
            "window.__vstLanguageFixtureReady === true", std::chrono::seconds(5));
        const auto measurementImportStarted = languageFixtureReady && evaluate(
            R"JS((() => {
              window.__vstMeasurementImportReady = false;
              window.__vstMeasurementImportError = '';
              void (async () => {
                const importer = window.__effetuneMeasurementImport;
                if (!importer || importer.maximumBytes !== 134217728) return;
                const plugin = window.pluginManager.createPlugin('Room EQ');
                window.audioManager.pipeline.push(plugin);
                window.uiManager.pipelineManager.updatePipelineUI(true);
                let invalidRejected = false;
                try {
                  await importer.importText('{}', plugin);
                } catch {
                  invalidRejected = true;
                }
                const measurementId = await importer.importText(JSON.stringify({
                  name: 'Imported WebView Measurement',
                  timestamp: '2026-07-25T00:00:00.000Z',
                  sampleRate: 48000,
                  averageFrequencyResponse: [
                    [20, -1], [100, 0], [1000, 1], [5000, -1], [10000, 0], [20000, -2]
                  ],
                  points: [{
                    pointId: 0,
                    name: 'Center',
                    frequencyResponse: [
                      [20, -1], [100, 0], [1000, 1], [5000, -1], [10000, 0], [20000, -2]
                    ]
                  }]
                }), plugin);
                const option = document.querySelector(
                  `#room-eq-measurement-${plugin.id} option[value="${measurementId}"]`
                );
                const row = document.querySelector(
                  `#room-eq-measurement-${plugin.id}`
                )?.parentElement;
                const importButton = row?.querySelector('.vst-measurement-import');
                const deleteButton = row?.querySelector('.vst-measurement-delete');
                for (let attempt = 0; deleteButton?.disabled && attempt < 50; ++attempt) {
                  await new Promise(resolve => setTimeout(resolve, 10));
                }
                const importedUiReady = invalidRejected &&
                  plugin.measurementId === measurementId &&
                  option?.textContent.includes('Imported WebView Measurement') &&
                  importButton?.textContent === 'Import\u2026' &&
                  deleteButton?.textContent === 'Delete' &&
                  deleteButton.disabled === false;
                deleteButton?.click();
                let confirmation = null;
                for (let attempt = 0; !confirmation && attempt < 50; ++attempt) {
                  await new Promise(resolve => setTimeout(resolve, 10));
                  confirmation = document.querySelector('.vst-measurement-delete-confirm');
                }
                const confirmationReady =
                  document.querySelector('.vst-measurement-delete-dialog p')
                    ?.textContent.includes('Imported WebView Measurement') &&
                  confirmation?.textContent === 'Delete';
                confirmation?.click();
                let measurementDeleted = false;
                for (let attempt = 0; attempt < 100; ++attempt) {
                  await new Promise(resolve => setTimeout(resolve, 10));
                  const store = await plugin._getMeasurementStore(true);
                  if (!await store?.getMeasurement(measurementId) &&
                      deleteButton.disabled && plugin.measurementId === '') {
                    measurementDeleted = true;
                    break;
                  }
                }
                const measurementSelect = document.querySelector(
                  `#room-eq-measurement-${plugin.id}`
                );
                const deletedOption = document.querySelector(
                  `#room-eq-measurement-${plugin.id} option[value="${measurementId}"]`
                );
                window.__vstMeasurementImportReady = importedUiReady && confirmationReady &&
                  measurementDeleted && measurementSelect?.value === '' && !deletedOption;
              })().catch(error => {
                window.__vstMeasurementImportError = String(error?.stack || error);
              });
              return true;
            })())JS",
            ignored) && ignored == "true";
        measurementImportAvailable = measurementImportStarted && waitForJavascript(
            "window.__vstMeasurementImportReady === true",
            std::chrono::seconds(10));
        const auto configDialogOpened = languageFixtureReady && evaluate(
            "(() => { document.getElementById('configSettingsButton')?.click(); return true; })()",
            ignored) && ignored == "true" && waitForJavascript(
                "!!document.getElementById('language-select')", std::chrono::seconds(5));
        configDialogIsLanguageOnly = configDialogOpened && evaluate(
            "(() => { const content = document.querySelector('.config-dialog-content'); "
            "const powerColumn = document.querySelector('.config-dialog-power-column'); "
            "const sections = [...document.querySelectorAll('.config-dialog .device-section')]; "
            "if (!content || !powerColumn || sections.length <= 1) return false; "
            "const visible = sections.filter(section => "
            "getComputedStyle(section).display !== 'none'); "
            "return visible.length === 1 && "
            "visible[0].contains(document.getElementById('language-select')) && "
            "getComputedStyle(content).display === 'block' && "
            "getComputedStyle(powerColumn).display === 'none'; })()",
            ignored) && ignored == "true";
        const auto languageChangeStarted = configDialogOpened && evaluate(
            "(() => { const select = document.getElementById('language-select'); "
            "if (!select) return false; select.value = 'ja'; "
            "select.dispatchEvent(new Event('change', { bubbles: true })); return true; })()",
            ignored) && ignored == "true";
        languageChangedImmediately = languageChangeStarted && waitForJavascript(
            "window.uiManager?.userLanguage === 'ja' && "
            "document.getElementById('config-title')?.textContent === '\u8a2d\u5b9a' && "
            "document.getElementById('configSettingsButton')?.textContent === '\u8a2d\u5b9a' && "
            "document.querySelector('.delete-button')?.title === "
            "'\u30a8\u30d5\u30a7\u30af\u30c8\u3092\u524a\u9664' && "
            "document.getElementById('effectCount')?.textContent.includes("
            "'\u7a2e\u985e\u306e\u30a8\u30d5\u30a7\u30af\u30c8\u304c\u5229\u7528\u53ef\u80fd')",
            std::chrono::seconds(5));
        if (configDialogOpened) {
          evaluate("document.getElementById('close-btn')?.click(); true", ignored);
        }

        const auto aboutStarted = languageChangedImmediately && evaluate(
            "(() => { const menu = document.getElementById('settingsMenu'); "
            "document.getElementById('settingsMenuButton')?.click(); "
            "const about = document.getElementById('aboutSettingsButton'); "
            "if (!menu?.classList.contains('show') || "
            "about?.textContent !== '\u30a2\u30d7\u30ea\u306b\u3064\u3044\u3066') "
            "return false; about.click(); return !menu.classList.contains('show'); })()",
            ignored) && ignored == "true";
        const auto aboutOpened = aboutStarted && waitForJavascript(
            "(() => { const dialog = document.querySelector('.about-dialog'); "
            "return dialog?.querySelector('h2')?.textContent === 'EffeTune Mixwright' && "
            "dialog?.querySelector('.about-version')?.textContent === 'Version test' && "
            "dialog?.querySelector('.about-description')?.textContent === "
            "'Multi-Effect Audio Plug-in for DAWs' && "
            "!!document.getElementById('vst-third-party-notices-button'); })()",
            std::chrono::seconds(5));
        const auto noticesStarted = aboutOpened && evaluate(
            "document.getElementById('vst-third-party-notices-button')?.click(); true", ignored) &&
            ignored == "true";
        aboutNoticesReachable = noticesStarted && waitForJavascript(
            "(() => { const text = document.querySelector("
            "'.vst-third-party-notices-content')?.textContent || ''; "
            "return text.startsWith('EffeTune Mixwright - Third-Party Notices') && "
            "text.trimEnd().endsWith("
            "'VST is a registered trademark of Steinberg Media Technologies GmbH.'); })()",
            std::chrono::seconds(5));
        if (aboutOpened) {
          evaluate("document.getElementById('vst-third-party-notices-close')?.click(); "
                   "document.querySelector('.about-dialog #close-button')?.click(); true",
                   ignored);
        }

        const auto appHelpStarted = aboutNoticesReachable && evaluate(
            "(() => { const menu = document.getElementById('settingsMenu'); "
            "document.getElementById('settingsMenuButton')?.click(); "
            "const help = document.getElementById('helpSettingsButton'); "
            "if (!menu?.classList.contains('show') || help?.textContent !== '\u30d8\u30eb\u30d7') "
            "return false; help.click(); return !menu.classList.contains('show'); })()",
            ignored) && ignored == "true";
        const auto appHelpDeadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (smokeState.externalAppHelpCalls.load(std::memory_order_relaxed) == 0 &&
               std::chrono::steady_clock::now() < appHelpDeadline) {
          pumpMessages();
          std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        const auto githubStarted = appHelpStarted && evaluate(
            "(() => { const menu = document.getElementById('settingsMenu'); "
            "document.getElementById('settingsMenuButton')?.click(); "
            "const github = document.getElementById('githubSettingsButton'); "
            "if (!menu?.classList.contains('show') || github?.textContent !== 'GitHub') "
            "return false; github.click(); return !menu.classList.contains('show'); })()",
            ignored) && ignored == "true";
        const auto githubDeadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (smokeState.externalGitHubCalls.load(std::memory_order_relaxed) == 0 &&
               std::chrono::steady_clock::now() < githubDeadline) {
          pumpMessages();
          std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        const auto productionDocumentationTarget = githubStarted &&
            smokeState.externalAppHelpCalls.load(std::memory_order_relaxed) == 1 && evaluate(
            "window.uiManager?.getLocalizedDocPath('/plugins/analyzer#level-meter') === "
            "'https://effetune.frieve.com/docs/i18n/ja/plugins/analyzer.html#level-meter'",
            ignored) && ignored == "true";
        const auto externalHelpStarted = productionDocumentationTarget && evaluate(
            "(() => { const item = document.querySelector('.pipeline-item[data-plugin-id=\"' + "
            "window.__vstLanguagePluginId + '\"]'); const help = item?.querySelector('.help-button'); "
            "if (!help) return false; help.click(); return true; })()",
            ignored) && ignored == "true";
        const auto externalHelpDeadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (smokeState.externalHelpCalls.load(std::memory_order_relaxed) == 0 &&
               std::chrono::steady_clock::now() < externalHelpDeadline) {
          pumpMessages();
          std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        const auto remappedExternalHelpStarted = externalHelpStarted &&
            smokeState.externalHelpCalls.load(std::memory_order_relaxed) == 1 && evaluate(
            "window.electronAPI.openExternalUrl("
            "'https://effetune-mixwright.localhost/docs/i18n/ja/plugins/"
            "analyzer.md#level-meter'); true",
            ignored) && ignored == "true";
        const auto remappedExternalHelpDeadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (smokeState.remappedExternalHelpCalls.load(std::memory_order_relaxed) == 0 &&
               std::chrono::steady_clock::now() < remappedExternalHelpDeadline) {
          pumpMessages();
          std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        const auto externalAiStarted = remappedExternalHelpStarted &&
            smokeState.remappedExternalHelpCalls.load(std::memory_order_relaxed) == 1 && evaluate(
            "(() => { const item = document.querySelector('.pipeline-item[data-plugin-id=\"' + "
            "window.__vstLanguagePluginId + '\"]'); const ai = item?.querySelector('.ai-button'); "
            "if (!ai) return false; ai.click(); "
            "const text = document.querySelector('.ai-dialog textarea'); "
            "const chat = document.querySelector('.ai-dialog-button'); "
            "if (!text || !chat) return false; text.value = 'How does it work?'; chat.click(); "
            "return true; })()",
            ignored) && ignored == "true";
        const auto externalAiDeadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (smokeState.externalAiCalls.load(std::memory_order_relaxed) == 0 &&
               std::chrono::steady_clock::now() < externalAiDeadline) {
          pumpMessages();
          std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        externalLinksRouted = appHelpStarted && githubStarted && externalHelpStarted &&
            remappedExternalHelpStarted && externalAiStarted &&
            smokeState.externalAppHelpCalls.load(std::memory_order_relaxed) == 1 &&
            smokeState.externalGitHubCalls.load(std::memory_order_relaxed) == 1 &&
            smokeState.externalHelpCalls.load(std::memory_order_relaxed) == 1 &&
            smokeState.remappedExternalHelpCalls.load(std::memory_order_relaxed) == 1 &&
            smokeState.externalAiCalls.load(std::memory_order_relaxed) == 1 &&
            smokeState.invalidExternalDocumentationCalls.load(std::memory_order_relaxed) == 0;

        const auto bypassCallsBefore =
            smokeState.masterBypassCalls.load(std::memory_order_relaxed);
        const auto clickedMasterToggle = evaluate(
            "(() => { const button = document.querySelector('.toggle-button.master-toggle'); "
            "button?.click(); return !!button; })()",
            ignored);
        const auto bypassDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (smokeState.masterBypassCalls.load(std::memory_order_relaxed) <= bypassCallsBefore &&
               std::chrono::steady_clock::now() < bypassDeadline) {
          pumpMessages();
          std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        masterToggleReachedNative = clickedMasterToggle &&
            smokeState.masterBypassCalls.load(std::memory_order_relaxed) > bypassCallsBefore &&
            smokeState.lastMasterBypass.load(std::memory_order_relaxed);

        const auto selectedSingleColumn = evaluate(
            "(() => { const columns = window.pipelineManager?.core?.columnManager; "
            "if (!columns || columns.getCurrentColumns() !== 2) return false; "
            "columns.updatePipelineColumns(1); "
            "document.body.style.minHeight = `${window.innerHeight + 100}px`; "
            "return true; })()",
            ignored);
        defaultViewportFits = selectedSingleColumn && ignored == "true" && waitForJavascript(
            "(() => { const viewport = document.scrollingElement; "
            "return viewport.scrollHeight > viewport.clientHeight && "
            "viewport.scrollWidth <= viewport.clientWidth; })()",
            std::chrono::seconds(5));

        const auto markerInstalled = evaluate(
            "window.__vstReattachMarker = 'preserved'; true", ignored) && ignored == "true";
        const auto hostInfoCallsBeforeReattach =
            smokeState.hostInfoCalls.load(std::memory_order_relaxed);
        const auto rebuildCallsBeforeReattach =
            smokeState.pipelineRebuildCalls.load(std::memory_order_relaxed);
        const auto replacementParent = createParent();
        const auto parentHandleChanged =
            replacementParent != nullptr && replacementParent != parent;
        smokeState.pipelineStateRead.store(false, std::memory_order_release);
        webView.detach(&firstEditorOwner);
        DestroyWindow(parent);
        parent = replacementParent;
        activeEditorOwner = &secondEditorOwner;
        if (parentHandleChanged &&
            webView.attach(activeEditorOwner, parent,
                           effetune::vst::plugin::kDefaultEditorWidth,
                           effetune::vst::plugin::kDefaultEditorHeight)) {
          ShowWindow(parent, SW_SHOWNOACTIVATE);
          const auto child = GetWindow(parent, GW_CHILD);
          // A delayed removed() from the old IPlugView must not detach the new one.
          webView.detach(&firstEditorOwner);
          reattachedWithFreshWebView = child != nullptr && GetParent(child) == parent &&
              markerInstalled && waitForJavascript(
                  "window.__vstReattachMarker === undefined && "
                  "window.app?.initialized === true && "
                  "Object.keys(window.pluginManager?.pluginClasses || {}).length >= 60",
                  std::chrono::seconds(10)) &&
              smokeState.hostInfoCalls.load(std::memory_order_relaxed) >
                  hostInfoCallsBeforeReattach &&
              smokeState.pipelineRebuildCalls.load(std::memory_order_relaxed) ==
                  rebuildCallsBeforeReattach;
        }

        if (reattachedWithFreshWebView) {
          const auto recoveryParent = createParent();
          const auto recoveryParentHandleChanged =
              recoveryParent != nullptr && recoveryParent != parent;
          DestroyWindow(parent);
          parent = recoveryParent;
          const auto hostInfoCallsBeforeRecovery =
              smokeState.hostInfoCalls.load(std::memory_order_relaxed);
          const auto rebuildCallsBeforeRecovery =
              smokeState.pipelineRebuildCalls.load(std::memory_order_relaxed);
          smokeState.pipelineStateRead.store(false, std::memory_order_release);
          activeEditorOwner = &thirdEditorOwner;
          if (recoveryParentHandleChanged &&
              webView.attach(activeEditorOwner, parent,
                             effetune::vst::plugin::kDefaultEditorWidth,
                             effetune::vst::plugin::kDefaultEditorHeight)) {
            ShowWindow(parent, SW_SHOWNOACTIVATE);
            const auto child = GetWindow(parent, GW_CHILD);
            webView.detach(&secondEditorOwner);
            recoveredAfterParentDestruction = child != nullptr && GetParent(child) == parent &&
                waitForJavascript(
                    "window.app?.initialized === true && "
                    "Object.keys(window.pluginManager?.pluginClasses || {}).length >= 60",
                    std::chrono::seconds(10)) &&
                smokeState.hostInfoCalls.load(std::memory_order_relaxed) >
                    hostInfoCallsBeforeRecovery &&
                smokeState.pipelineRebuildCalls.load(std::memory_order_relaxed) ==
                    rebuildCallsBeforeRecovery;
          }
        }

        // Every reattachment above reopens before the replacement WebView has
        // finished starting, which is the one case attach() is allowed to reuse
        // it. Let it become fully ready first, so this covers the other case: a
        // WebView whose controller was created under the previous parent must be
        // rebuilt rather than reparented, or some hosts show a blank editor on
        // the second open.
        if (recoveredAfterParentDestruction) {
          webView.detach(&thirdEditorOwner);
          // sessionStorage is the discriminator that matters here: it belongs to
          // the WebView's browsing context, so it survives a navigation but not a
          // replacement. A document-level marker would prove nothing, because
          // navigating a reused WebView clears that too.
          std::string idleMarker;
          const auto replacementFinishedLoading = waitForJavascript(
              "window.app?.initialized === true", std::chrono::seconds(20)) &&
              evaluate("sessionStorage.setItem('vstIdleReopenProbe', 'stale'); "
                       "sessionStorage.getItem('vstIdleReopenProbe') === 'stale'",
                       idleMarker) &&
              idleMarker == "true";
          const auto idleParent = createParent();
          const auto idleParentHandleChanged =
              idleParent != nullptr && idleParent != parent;
          DestroyWindow(parent);
          parent = idleParent;
          smokeState.pipelineStateRead.store(false, std::memory_order_release);
          activeEditorOwner = &fourthEditorOwner;
          if (replacementFinishedLoading && idleParentHandleChanged &&
              webView.attach(activeEditorOwner, parent,
                             effetune::vst::plugin::kDefaultEditorWidth,
                             effetune::vst::plugin::kDefaultEditorHeight)) {
            ShowWindow(parent, SW_SHOWNOACTIVATE);
            const auto child = GetWindow(parent, GW_CHILD);
            reopenedReadyEditorWasRebuilt = child != nullptr &&
                GetParent(child) == parent &&
                waitForJavascript(
                    "sessionStorage.getItem('vstIdleReopenProbe') === null && "
                    "window.app?.initialized === true && "
                    "Object.keys(window.pluginManager?.pluginClasses || {}).length >= 60",
                    std::chrono::seconds(15));
          }
        }
      }
    }
    webView.detach(activeEditorOwner);
    webViewProfile.captureChildProcesses();
  }
  if (parent != nullptr && IsWindow(parent) != FALSE) {
    int fallbackOwner = 0;
    effetune::vst::WebViewHost fallbackWebView(
        [](const std::string_view) { return R"({"ok":true})"; },
        std::filesystem::path(EFFETUNE_WEBVIEW_ASSET_DIR) / "missing-resource-root",
        false);
    if (fallbackWebView.attach(
            &fallbackOwner, parent,
            effetune::vst::plugin::kDefaultEditorWidth,
            effetune::vst::plugin::kDefaultEditorHeight)) {
      ShowWindow(parent, SW_SHOWNOACTIVATE);
      std::atomic_bool fallbackEvaluationPending{false};
      const auto deadline =
          std::chrono::steady_clock::now() + std::chrono::seconds(10);
      auto nextFallbackEvaluation =
          std::chrono::steady_clock::now() + std::chrono::seconds(1);
      while (!missingResourceGuidanceShown &&
             std::chrono::steady_clock::now() < deadline) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != 0) {
          TranslateMessage(&message);
          DispatchMessageW(&message);
        }
        if (std::chrono::steady_clock::now() >= nextFallbackEvaluation &&
            !fallbackEvaluationPending.exchange(true, std::memory_order_acq_rel)) {
          if (!fallbackWebView.evaluate(
                  "document.documentElement.dataset.effetuneLoadError === "
                  "'missing-assets' && document.body?.innerText.includes("
                  "'Reinstall the complete EffeTune Mixwright.vst3 bundle')",
                  [&](std::string error, std::string result) {
                    missingResourceGuidanceShown =
                        error.empty() && result == "true";
                    fallbackEvaluationPending.store(false,
                                                    std::memory_order_release);
                  })) {
            fallbackEvaluationPending.store(false, std::memory_order_release);
          }
          nextFallbackEvaluation =
              std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
      fallbackWebView.detach(&fallbackOwner);
    }
    webViewProfile.captureChildProcesses();
  }
  // CHOC discards the result of every asynchronous WebView2 construction step, so
  // a runtime that starts but never completes leaves an editor that is attached,
  // reports success, and stays empty forever. Pointing the loader at a runtime
  // that does not exist reproduces exactly that, and the editor has to end up
  // showing readable native text instead.
  bool initialisationTimeoutDiagnosed = false;
  {
    constexpr wchar_t browserFolderVariable[] = L"WEBVIEW2_BROWSER_EXECUTABLE_FOLDER";
    const auto stalledParent = createParent();
    if (stalledParent != nullptr &&
        SetEnvironmentVariableW(browserFolderVariable,
                                L"C:\\effetune-nonexistent-webview2-runtime") != FALSE) {
      int stalledOwner = 0;
      {
        effetune::vst::WebViewHost stalled(
            [](const std::string_view) { return R"({"ok":true})"; },
            std::filesystem::path(EFFETUNE_WEBVIEW_ASSET_DIR), false);
        if (stalled.attach(&stalledOwner, stalledParent,
                           effetune::vst::plugin::kDefaultEditorWidth,
                           effetune::vst::plugin::kDefaultEditorHeight)) {
          ShowWindow(stalledParent, SW_SHOWNOACTIVATE);
          const auto deadline = std::chrono::steady_clock::now() +
                                effetune::vst::kWebViewInitialisationTimeout +
                                std::chrono::seconds(15);
          while (!initialisationTimeoutDiagnosed &&
                 std::chrono::steady_clock::now() < deadline) {
            // Dispatch everything, including the null-window WM_TIMER the editor
            // watchdog relies on.
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != 0) {
              TranslateMessage(&message);
              DispatchMessageW(&message);
            }
            const auto diagnostic =
                FindWindowExW(stalledParent, nullptr, L"EDIT", nullptr);
            if (diagnostic != nullptr) {
              std::wstring text(4096, L'\0');
              const auto length = GetWindowTextW(diagnostic, text.data(),
                                                 static_cast<int>(text.size()));
              text.resize(static_cast<std::size_t>(length));
              initialisationTimeoutDiagnosed =
                  stalled.status() ==
                      effetune::vst::WebViewStatus::initialisationTimedOut &&
                  text.find(L"EFFETUNE-UI-TIMEOUT") != std::wstring::npos;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
          }
        }
        stalled.detach(&stalledOwner);
      }
      (void)SetEnvironmentVariableW(browserFolderVariable, nullptr);
    }
    if (stalledParent != nullptr) {
      DestroyWindow(stalledParent);
    }
    webViewProfile.captureChildProcesses();
  }

  // A host that opens the editor from a multi-threaded apartment can never run
  // WebView2, so the editor has to say so natively instead of handing back an
  // empty window that no one can diagnose.
  std::thread apartmentProbe([&] {
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {
      return;
    }
    const auto probeParent =
        CreateWindowExW(0, className, L"EffeTune WebView MTA Probe",
                        WS_OVERLAPPEDWINDOW, -30000, -30000, 900, 650, nullptr,
                        nullptr, instance, nullptr);
    if (probeParent != nullptr) {
      int probeOwner = 0;
      {
        effetune::vst::WebViewHost probe(
            [](const std::string_view) { return R"({"ok":true})"; },
            std::filesystem::path(EFFETUNE_WEBVIEW_ASSET_DIR), false);
        if (probe.attach(&probeOwner, probeParent,
                         effetune::vst::plugin::kDefaultEditorWidth,
                         effetune::vst::plugin::kDefaultEditorHeight) &&
            probe.status() == effetune::vst::WebViewStatus::unsupportedApartment) {
          std::wstring text(4096, L'\0');
          const auto diagnostic = GetWindow(probeParent, GW_CHILD);
          const auto length =
              diagnostic == nullptr
                  ? 0
                  : GetWindowTextW(diagnostic, text.data(),
                                   static_cast<int>(text.size()));
          text.resize(static_cast<std::size_t>(length));
          multiThreadedApartmentDiagnosed =
              text.find(L"EFFETUNE-UI-MTA") != std::wstring::npos &&
              text.find(L"multi-threaded apartment") != std::wstring::npos;
        }
        probe.detach(&probeOwner);
      }
      DestroyWindow(probeParent);
    }
    CoUninitialize();
  });
  apartmentProbe.join();

  if (parent != nullptr) {
    DestroyWindow(parent);
  }
  UnregisterClassW(className, instance);

  if (!webViewProfile.cleanup()) {
    std::cerr << "WebView2 child processes did not release the isolated profile: "
              << webViewProfile.path().string() << '\n';
    return 1;
  }

  if (!attached) {
    std::cerr << "WebView2 could not be attached\n";
    return 1;
  }
  if (smokeState.hostInfoCalls.load(std::memory_order_relaxed) == 0) {
    if (evaluationComplete.load(std::memory_order_acquire)) {
      std::cerr << "WebView evaluation error: " << evaluationError << '\n'
                << "WebView state: " << evaluationResult << '\n'
                << "Readiness diagnostics: " << readinessDiagnostics << '\n';
    }
    std::cerr << "The EffeTune UI did not reach the native bridge\n";
    return 1;
  }
  if (!uiReady.load(std::memory_order_acquire)) {
    std::cerr << "WebView evaluation error: " << evaluationError << '\n'
              << "Registered plug-ins: " << evaluationResult << '\n'
              << "Readiness diagnostics: " << readinessDiagnostics << '\n'
              << "The EffeTune UI did not finish loading its plug-in catalog\n";
    return 1;
  }
  if (!isolatedWebOrigin) {
    std::cerr << "The EffeTune UI reused CHOC's shared default WebView origin\n";
    return 1;
  }
  if (!telemetryDelivered) {
    std::cerr << "A non-empty native telemetry packet did not reach a TelemetryHub subscriber\n";
    return 1;
  }
  if (!dynamicLatencyServiced) {
    std::cerr << "Dynamic latency servicing was not independently debounced\n";
    return 1;
  }
  if (!historyUsedSingleNativeRestore) {
    std::cerr << "Undo/redo sent more than its single native history-restore request\n";
    return 1;
  }
  if (!updatePluginRoutedByOwnership) {
    std::cerr << "An individual plug-in update ignored its actual A/B ownership\n";
    return 1;
  }
  if (!unsupportedStateWarningShown) {
    std::cerr << "An unavailable inactive-pipeline effect did not show the restore warning\n";
    return 1;
  }
  if (!hiddenContextSynchronized) {
    std::cerr << "A hidden WebView did not rebuild after a native context generation change\n";
    return 1;
  }
  if (!hiddenTelemetryDiscarded) {
    std::cerr << "A hidden telemetry queue was delivered before the resume discard\n";
    return 1;
  }
  if (!contextMenuDisabled) {
    std::cerr << "The browser context menu remains enabled\n";
    return 1;
  }
  if (!clipboardReachable) {
    std::cerr << "Normal pipeline clipboard paste is not reachable\n";
    return 1;
  }
  if (!libraryEntriesDisabled) {
    std::cerr << "A music-library startup or keyboard entry remains reachable\n";
    return 1;
  }
  if (!headerControlsAreVstSpecific) {
    std::cerr << "The VST upsampling controls or single-view header exclusions are incorrect\n";
    return 1;
  }
  if (!configDialogIsLanguageOnly) {
    std::cerr << "The VST settings dialog exposed non-language settings\n";
    return 1;
  }
  if (!languageChangedImmediately) {
    std::cerr << "A language change did not immediately refresh static and dynamic UI\n";
    return 1;
  }
  if (!measurementImportAvailable) {
    std::cerr << "Room EQ did not import, delete, and clear an exported measurement JSON file\n";
    return 1;
  }
  if (!aboutNoticesReachable) {
    std::cerr << "The About dialog did not show the Mixwright version and complete third-party "
                 "notices\n";
    return 1;
  }
  if (!externalLinksRouted) {
    std::cerr << "A Help, GitHub, or AI action did not route its production URL to the native host "
                 "(app-help="
              << smokeState.externalAppHelpCalls.load(std::memory_order_relaxed) << ", github="
              << smokeState.externalGitHubCalls.load(std::memory_order_relaxed) << ", help="
              << smokeState.externalHelpCalls.load(std::memory_order_relaxed) << ", ai="
              << smokeState.externalAiCalls.load(std::memory_order_relaxed) << ", remapped="
              << smokeState.remappedExternalHelpCalls.load(std::memory_order_relaxed)
              << ", invalid="
              << smokeState.invalidExternalDocumentationCalls.load(std::memory_order_relaxed)
              << ")\n";
    return 1;
  }
  if (!masterToggleReachedNative) {
    std::cerr << "The UI master toggle did not call the native master-bypass route\n";
    return 1;
  }
  if (!defaultViewportFits) {
    std::cerr << "The default one-column editor viewport has horizontal overflow\n";
    return 1;
  }
  if (!reattachedWithFreshWebView) {
    std::cerr << "A replacement parent did not receive a freshly-created WebView\n";
    return 1;
  }
  if (!recoveredAfterParentDestruction) {
    std::cerr << "A WebView with a destroyed parent was not recreated on reattachment\n";
    return 1;
  }
  if (!reopenedReadyEditorWasRebuilt) {
    std::cerr << "Reopening the editor reused a WebView whose controller belonged to the "
                 "previous parent instead of rebuilding it\n";
    return 1;
  }
  if (!missingResourceGuidanceShown) {
    std::cerr << "Missing WebView assets did not show actionable reinstall guidance\n";
    return 1;
  }
  if (!editorReportedReady) {
    std::cerr << "A working editor did not report a ready WebView status\n";
    return 1;
  }
  if (!multiThreadedApartmentDiagnosed) {
    std::cerr << "An editor opened from a multi-threaded apartment showed no native "
                 "diagnostics\n";
    return 1;
  }
  if (!initialisationTimeoutDiagnosed) {
    std::cerr << "A WebView2 construction that never completed left an unexplained "
                 "empty editor\n";
    return 1;
  }
  if (smokeState.prematureEmptyRebuildCalls.load(std::memory_order_relaxed) != 0) {
    std::cerr << "A recreated WebView cleared the native pipeline before restoring its state\n";
    return 1;
  }
  std::cout << "EffeTune WebView exercised telemetry resume, history restore, A/B routing, "
               "unavailable-state warning, clipboard, excluded library, context, language, "
               "Room EQ measurement import, About notices, external links, bypass, and editor "
               "reattachment\n";
  return 0;
}
