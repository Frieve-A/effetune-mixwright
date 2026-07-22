#pragma once

#include "engine/engine_host.h"

#include <string>
#include <string_view>
#include <vector>

namespace effetune::vst {

enum class UiAction {
  hostInfo,
  openExternalUrl,
  rebuildPipeline,
  restoreHistory,
  updatePlugin,
  beginPluginAsset,
  appendPluginAsset,
  commitPluginAsset,
  clearPluginAsset,
  readPluginAssetState,
  setMasterBypass,
  setOversampling,
  readTelemetry,
  discardTelemetry,
  storageFileExists,
  storageReadFile,
  storageWriteFile,
  loadConfig,
  saveConfig,
  openPresetDialog,
  savePresetDialog
};

struct RoutedPlugin {
  PluginState logical;
  RuntimePlugin runtime;
};

struct RoutedUiMessage {
  UiAction action = UiAction::hostInfo;
  std::vector<RoutedPlugin> plugins;
  std::vector<RoutedPlugin> pipelineA;
  std::vector<RoutedPlugin> pipelineB;
  RuntimeAsset asset;
  std::uint64_t operationRevision = 0;
  std::size_t assetByteSize = 0;
  std::size_t assetOffset = 0;
  char pipeline = 'A';
  bool pipelineBInitialized = false;
  bool masterBypass = false;
  OversamplingSettings oversampling;
  std::string path;
  std::string url;
  std::string content;
  std::string defaultName;
};

class MessageRouter {
public:
  [[nodiscard]] static bool decode(std::string_view json, RoutedUiMessage &message,
                                   std::string *error = nullptr);
};

} // namespace effetune::vst
