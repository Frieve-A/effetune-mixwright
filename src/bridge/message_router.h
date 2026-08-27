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
  editAutomationParameter,
  beginAutomationGesture,
  endAutomationGesture,
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

// One automation target the user moved on purpose. A single gesture still
// routes as automation/edit; the array carries the same representation so a
// coalesced plug-in update, a preset load or an undo can name every target it
// means to move in one message.
struct RoutedAutomationEdit {
  char pipeline = 'A';
  std::uint32_t pluginId = 0;
  std::string pluginType;
  std::string parameterKey;
  std::uint32_t elementIndex = 0;
  double normalized = 0.0;
  // A user gesture may claim an automation lane on demand; an edit that only
  // restates a value the image it travels with already carries may not. An
  // unbound target needs no lane, no host edit and no diagnostic for such an
  // edit, and opening one would spend a finite slot permanently on something
  // the user never asked for. Defaults to a gesture, which is what an older UI
  // that omits the field always meant.
  bool bindIfUnbound = true;
  // Where this value sits inside the touch the user is performing. The upstream
  // controls report values only, so the shim derives the boundary from the
  // pointer: the first value of a drag opens the touch, the ones in the middle
  // carry neither flag, and the release closes it. Both default to true, which
  // is a complete touch of its own -- the right reporting for a discrete edit,
  // and exactly what an older UI that omits the fields always meant.
  bool beginGesture = true;
  bool endGesture = true;
};

struct RoutedUiMessage {
  UiAction action = UiAction::hostInfo;
  std::vector<RoutedPlugin> plugins;
  std::vector<RoutedPlugin> pipelineA;
  std::vector<RoutedPlugin> pipelineB;
  // Empty for every message type that carries no explicit automation intent,
  // including plug-in updates and bulk messages sent by an older UI. An
  // beginAutomationGesture and endAutomationGesture messages fill the identity
  // of every entry and nothing else: touch boundaries name no value.
  std::vector<RoutedAutomationEdit> automationEdits;
  RuntimeAsset asset;
  std::uint64_t operationRevision = 0;
  std::size_t assetByteSize = 0;
  std::size_t assetOffset = 0;
  char pipeline = 'A';
  // Set only by the one host/getInfo a page sends while starting up, before it
  // can have been touched. Every other host/getInfo is a poll, so it says
  // nothing about which context asked. Defaults to false, which is what an
  // older UI that omits the field always meant.
  bool startupHandshake = false;
  bool pipelineBInitialized = false;
  bool masterBypass = false;
  std::uint32_t pluginId = 0;
  std::uint32_t elementIndex = 0;
  double normalizedValue = 0.0;
  std::string pluginType;
  std::string parameterKey;
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
