#include "bridge/message_router.h"

#include "choc/text/choc_JSON.h"

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_set>

namespace effetune::vst {
namespace {

using choc::value::ValueView;

constexpr std::size_t kMaximumExternalUrlBytes = 4096;
constexpr std::size_t kMaximumAssetChunkBase64Bytes = 256u * 1024u;

[[nodiscard]] bool isAllowedExternalUrl(const std::string_view url) {
  if (url.empty() || url.size() > kMaximumExternalUrlBytes ||
      (!url.starts_with("https://") && !url.starts_with("http://"))) {
    return false;
  }
  return std::none_of(url.begin(), url.end(), [](const unsigned char value) {
    return value <= 0x20u || value == 0x7fu;
  });
}

[[nodiscard]] std::string unknownMembersJson(
    const ValueView object, const std::unordered_set<std::string_view> &known) {
  auto extras = choc::value::createObject({});
  for (std::uint32_t index = 0; index < object.size(); ++index) {
    const auto member = object.getObjectMemberAt(index);
    if (!known.contains(member.name)) {
      extras.addMember(member.name, member.value);
    }
  }
  return choc::json::toString(extras);
}

void setError(std::string *destination, std::string value) {
  if (destination != nullptr) {
    *destination = std::move(value);
  }
}

[[nodiscard]] std::string stateNameForType(std::string type) {
  constexpr std::string_view suffix = "Plugin";
  if (type.size() > suffix.size() && type.ends_with(suffix)) {
    type.resize(type.size() - suffix.size());
  }
  return type;
}

[[nodiscard]] std::uint32_t readId(const ValueView item) {
  const auto id = item["id"].getWithDefault<std::int64_t>(0);
  return id > 0 && id <= std::numeric_limits<std::uint32_t>::max()
             ? static_cast<std::uint32_t>(id)
             : 0u;
}

[[nodiscard]] bool decodeAssetIdentity(const ValueView payload, RoutedUiMessage &message,
                                       std::string *error, const bool requireRevision) {
  const auto pluginId = payload["pluginId"].getWithDefault<std::int64_t>(0);
  const auto slot = payload["slot"].getWithDefault<std::int64_t>(-1);
  const auto revision = payload["operationRevision"].getWithDefault<std::int64_t>(0);
  if (pluginId <= 0 || pluginId > std::numeric_limits<std::uint32_t>::max() || slot < 0 ||
      slot > std::numeric_limits<std::uint32_t>::max() || (requireRevision && revision <= 0)) {
    setError(error, "DSP asset message has an invalid target");
    return false;
  }
  message.asset.logicalId = static_cast<std::uint32_t>(pluginId);
  message.asset.slot = static_cast<std::uint32_t>(slot);
  message.operationRevision = revision > 0 ? static_cast<std::uint64_t>(revision) : 0u;
  return true;
}

[[nodiscard]] bool decodeAssetBegin(const ValueView payload, RoutedUiMessage &message,
                                    std::string *error) {
  if (!decodeAssetIdentity(payload, message, error, true)) {
    return false;
  }
  const auto formatTag = payload["formatTag"].getWithDefault<std::int64_t>(0);
  const auto channels = payload["channels"].getWithDefault<std::int64_t>(0);
  const auto frames = payload["frames"].getWithDefault<std::int64_t>(0);
  const auto topology = payload["topology"].getWithDefault<std::int64_t>(0);
  const auto headBlock = payload["headBlock"].getWithDefault<std::int64_t>(-1);
  const auto rateDivider = payload["rateDivider"].getWithDefault<std::int64_t>(0);
  const auto pathCount = payload["pathCount"].getWithDefault<std::int64_t>(-1);
  const auto inputCount = payload["inputCount"].getWithDefault<std::int64_t>(-1);
  const auto processingChannels =
      payload["processingChannels"].getWithDefault<std::int64_t>(0);
  const auto footprintBytes = payload["footprintBytes"].getWithDefault<std::int64_t>(0);
  const auto byteSize = payload["byteSize"].getWithDefault<std::int64_t>(0);
  const auto validHeadBlock =
      headBlock == 0 || headBlock == 128 || headBlock == 256 || headBlock == 512 ||
      headBlock == 1024;
  if (formatTag <= 0 || formatTag > std::numeric_limits<std::uint32_t>::max() || channels < 1 ||
      channels > 8 || frames < 1 || frames > std::numeric_limits<std::uint32_t>::max() ||
      topology < 1 || topology > 4 || !validHeadBlock ||
      (rateDivider != 1 && rateDivider != 2 && rateDivider != 4) || pathCount < 0 ||
      pathCount > 8 || inputCount < 0 || inputCount > 8 || processingChannels < 1 ||
      processingChannels > 8 || byteSize < 1 ||
      byteSize > EngineHost::kMaximumAssetPayloadBytes || footprintBytes < byteSize ||
      footprintBytes > EngineHost::kMaximumAssetPayloadBytes) {
    setError(error, "DSP asset metadata is invalid or exceeds the plug-in capacity");
    return false;
  }
  message.asset.formatTag = static_cast<std::uint32_t>(formatTag);
  message.asset.channels = static_cast<std::uint32_t>(channels);
  message.asset.frames = static_cast<std::uint32_t>(frames);
  message.asset.topology = static_cast<std::uint32_t>(topology);
  message.asset.headBlock = static_cast<std::uint32_t>(headBlock);
  message.asset.rateDivider = static_cast<std::uint32_t>(rateDivider);
  message.asset.pathCount = static_cast<std::uint32_t>(pathCount);
  message.asset.inputCount = static_cast<std::uint32_t>(inputCount);
  message.asset.processingChannels = static_cast<std::uint32_t>(processingChannels);
  message.asset.footprintBytes = static_cast<std::uint32_t>(footprintBytes);
  message.assetByteSize = static_cast<std::size_t>(byteSize);
  return true;
}

[[nodiscard]] std::uint8_t readBus(const ValueView item, const char *name) {
  const auto value = item[name].getWithDefault<std::int64_t>(0);
  return value >= 0 && value <= kMaxBus ? static_cast<std::uint8_t>(value) : 0;
}

[[nodiscard]] std::string logicalParametersJson(const ValueView parameters) {
  if (!parameters.isObject()) {
    return "{}";
  }
  static const std::unordered_set<std::string_view> metadata{
      "type", "id", "enabled", "inputBus", "outputBus", "channel", "name"};
  auto logical = choc::value::createObject({});
  for (std::uint32_t index = 0; index < parameters.size(); ++index) {
    const auto member = parameters.getObjectMemberAt(index);
    if (!metadata.contains(member.name)) {
      logical.addMember(member.name, member.value);
    }
  }
  return choc::json::toString(logical);
}

[[nodiscard]] bool decodePlugin(const ValueView item, RoutedPlugin &destination,
                                std::string *error) {
  if (!item.isObject()) {
    setError(error, "Pipeline plugin must be an object");
    return false;
  }
  const auto type = item["type"].getWithDefault<std::string>({});
  const auto id = readId(item);
  if (type.empty() || id == 0) {
    setError(error, "Pipeline plugin is missing id or type");
    return false;
  }

  destination.logical.id = id;
  destination.logical.name = item["name"].getWithDefault<std::string>(stateNameForType(type));
  destination.logical.enabled = item["enabled"].getWithDefault<bool>(true);
  destination.logical.unknown = item["unknown"].getWithDefault<bool>(false);
  destination.logical.inputBus = readBus(item, "inputBus");
  destination.logical.outputBus = readBus(item, "outputBus");
  const auto channel = item["channel"].getWithDefault<std::string>({});
  if (!channel.empty()) {
    destination.logical.channel = channel;
  }
  destination.logical.parametersJson = logicalParametersJson(item["parameters"]);
  static const std::unordered_set<std::string_view> knownPluginFields{
      "id",                           "name",          "enabled",
      "parameters",  "inputBus",       "outputBus",     "channel", "unknown",
      "wasmParams",  "wasmParamsHash", "wasmParamBytes"};
  destination.logical.extraJson = unknownMembersJson(item, knownPluginFields);

  const auto packed = item["wasmParams"];
  const auto hash = item["wasmParamsHash"].getWithDefault<std::int64_t>(0);
  if (!packed.isArray() || hash <= 0 || hash > std::numeric_limits<std::uint32_t>::max()) {
    return true; // Section and future/unknown plug-ins intentionally have no native payload.
  }
  if (packed.size() > AudioCommand::kMaxPackedFloats) {
    setError(error, "Packed DSP parameter block exceeds the bridge limit");
    return false;
  }

  destination.runtime.logicalId = id;
  destination.runtime.type = type;
  destination.runtime.paramsHash = static_cast<std::uint32_t>(hash);
  destination.runtime.packedParameters.reserve(packed.size());
  for (std::uint32_t index = 0; index < packed.size(); ++index) {
    destination.runtime.packedParameters.push_back(
        static_cast<float>(packed[index].getWithDefault<double>(0.0)));
  }

  const auto bytes = item["wasmParamBytes"];
  if (bytes.isArray()) {
    if (bytes.size() > AudioCommand::kMaxParameterBytes) {
      setError(error, "Structured DSP parameter block exceeds the bridge limit");
      return false;
    }
    destination.runtime.parameterBytes.reserve(bytes.size());
    for (std::uint32_t index = 0; index < bytes.size(); ++index) {
      const auto value = bytes[index].getWithDefault<std::int64_t>(0);
      destination.runtime.parameterBytes.push_back(
          static_cast<std::uint8_t>(std::clamp<std::int64_t>(value, 0, 255)));
    }
  }
  return true;
}

[[nodiscard]] bool decodePlugins(const ValueView array, std::vector<RoutedPlugin> &plugins,
                                 std::string *error) {
  if (!array.isArray() || array.size() > kMaxPipelineNodes) {
    setError(error, "Pipeline payload is missing or exceeds the node limit");
    return false;
  }
  plugins.clear();
  plugins.reserve(array.size());
  std::size_t instanceCount = 0;
  for (std::uint32_t index = 0; index < array.size(); ++index) {
    RoutedPlugin plugin;
    if (!decodePlugin(array[index], plugin, error)) {
      return false;
    }
    const auto duplicate = std::find_if(
        plugins.begin(), plugins.end(), [&plugin](const RoutedPlugin &existing) {
          return existing.logical.id == plugin.logical.id;
        });
    if (duplicate != plugins.end()) {
      setError(error, "Pipeline plugin IDs must be unique");
      return false;
    }
    if (!plugin.runtime.type.empty() && ++instanceCount > kMaxPluginInstances) {
      setError(error, "Pipeline exceeds the native plug-in instance limit");
      return false;
    }
    plugins.push_back(std::move(plugin));
  }
  return true;
}

// The identity every automation message names. A gesture close carries this
// much and nothing else: ending a touch names no value.
[[nodiscard]] bool decodeAutomationTarget(const ValueView payload,
                                          RoutedAutomationEdit &edit,
                                          std::string *error) {
  const auto pluginId = payload["pluginId"].getWithDefault<std::int64_t>(0);
  const auto elementIndex = payload["elementIndex"].getWithDefault<std::int64_t>(0);
  edit.pipeline = payload["pipeline"].getWithDefault<std::string>("A") == "B" ? 'B' : 'A';
  edit.pluginType = payload["pluginType"].getWithDefault<std::string>({});
  edit.parameterKey = payload["parameterKey"].getWithDefault<std::string>({});
  if (pluginId <= 0 || pluginId > std::numeric_limits<std::uint32_t>::max() ||
      elementIndex < 0 || elementIndex > std::numeric_limits<std::uint32_t>::max() ||
      edit.pluginType.empty() || edit.parameterKey.empty()) {
    setError(error, "Automation edit has an invalid target or value");
    return false;
  }
  edit.pluginId = static_cast<std::uint32_t>(pluginId);
  edit.elementIndex = static_cast<std::uint32_t>(elementIndex);
  return true;
}

[[nodiscard]] bool decodeAutomationEdit(const ValueView payload,
                                        RoutedAutomationEdit &edit, std::string *error) {
  edit.normalized = payload["normalized"].getWithDefault<double>(-1.0);
  edit.bindIfUnbound = payload["bindIfUnbound"].getWithDefault<bool>(true);
  edit.beginGesture = payload["beginGesture"].getWithDefault<bool>(true);
  edit.endGesture = payload["endGesture"].getWithDefault<bool>(true);
  if (!decodeAutomationTarget(payload, edit, error)) {
    return false;
  }
  if (!std::isfinite(edit.normalized) || edit.normalized < 0.0 ||
      edit.normalized > 1.0) {
    setError(error, "Automation edit has an invalid target or value");
    return false;
  }
  return true;
}

// A message without the array comes from an older UI: it simply names no
// explicit automation intent, so it keeps the previous behaviour.
[[nodiscard]] bool decodeAutomationEdits(const ValueView array,
                                         std::vector<RoutedAutomationEdit> &edits,
                                         std::string *error) {
  if (array.isVoid()) {
    return true;
  }
  if (!array.isArray()) {
    setError(error, "Automation edits payload must be an array");
    return false;
  }
  edits.reserve(array.size());
  for (std::uint32_t index = 0; index < array.size(); ++index) {
    RoutedAutomationEdit edit;
    if (!decodeAutomationEdit(array[index], edit, error)) {
      return false;
    }
    edits.push_back(std::move(edit));
  }
  return true;
}

// The identities one closing touch releases. A malformed entry aborts the whole
// message, exactly as it does for the value-carrying array above.
[[nodiscard]] bool decodeAutomationTargets(const ValueView array,
                                           std::vector<RoutedAutomationEdit> &targets,
                                           std::string *error) {
  if (!array.isArray()) {
    setError(error, "Automation gesture targets must be an array");
    return false;
  }
  targets.reserve(array.size());
  for (std::uint32_t index = 0; index < array.size(); ++index) {
    RoutedAutomationEdit target;
    if (!decodeAutomationTarget(array[index], target, error)) {
      return false;
    }
    targets.push_back(std::move(target));
  }
  return true;
}

void decodeOversampling(const ValueView payload, OversamplingSettings &settings) {
  const auto factor = payload["factor"].getWithDefault<std::int64_t>(1);
  if (factor == 1 || factor == 2 || factor == 4 || factor == 8) {
    settings.factor = static_cast<std::uint32_t>(factor);
  }
  settings.phase = payload["phase"].getWithDefault<std::string>("linear") == "minimum"
                       ? OversamplingPhase::minimum
                       : OversamplingPhase::linear;
  const auto quality = payload["quality"].getWithDefault<std::string>("medium");
  if (quality == "low") {
    settings.quality = FilterQuality::low;
  } else if (quality == "high") {
    settings.quality = FilterQuality::high;
  } else if (quality == "ultra") {
    settings.quality = FilterQuality::ultra;
  } else {
    settings.quality = FilterQuality::medium;
  }
}

} // namespace

bool MessageRouter::decode(const std::string_view json, RoutedUiMessage &message,
                           std::string *error) {
  try {
    const auto root = choc::json::parse(json);
    if (!root.isObject()) {
      setError(error, "Bridge message root must be an object");
      return false;
    }
    const auto type = root["type"].getWithDefault<std::string>({});
    const auto payload = root["payload"].isObject() ? root["payload"] : root.getView();
    RoutedUiMessage decoded;

    if (type == "host/getInfo") {
      decoded.action = UiAction::hostInfo;
      decoded.startupHandshake = payload["startup"].getWithDefault<bool>(false);
    } else if (type == "host/openExternal") {
      decoded.action = UiAction::openExternalUrl;
      decoded.url = payload["url"].getWithDefault<std::string>({});
      if (!isAllowedExternalUrl(decoded.url)) {
        setError(error, "External links must use a valid HTTP or HTTPS URL");
        return false;
      }
    } else if (type == "pipeline/rebuild") {
      decoded.action = UiAction::rebuildPipeline;
      decoded.pipeline = payload["pipeline"].getWithDefault<std::string>("A") == "B" ? 'B' : 'A';
      decoded.masterBypass = payload["masterBypass"].getWithDefault<bool>(false);
      if (!decodePlugins(payload["plugins"], decoded.plugins, error) ||
          !decodeAutomationEdits(payload["automationEdits"], decoded.automationEdits,
                                 error)) {
        return false;
      }
    } else if (type == "pipeline/restoreHistory") {
      decoded.action = UiAction::restoreHistory;
      decoded.pipeline =
          payload["currentPipeline"].getWithDefault<std::string>("A") == "B" ? 'B' : 'A';
      if (!decodePlugins(payload["pipelineA"], decoded.pipelineA, error)) {
        return false;
      }
      decoded.pipelineBInitialized = payload["pipelineBInitialized"].getWithDefault<bool>(false);
      if (decoded.pipelineBInitialized &&
          !decodePlugins(payload["pipelineB"], decoded.pipelineB, error)) {
        return false;
      }
      if (!decodeAutomationEdits(payload["automationEdits"], decoded.automationEdits,
                                 error)) {
        return false;
      }
    } else if (type == "pipeline/updatePlugin") {
      decoded.action = UiAction::updatePlugin;
      decoded.pipeline = payload["pipeline"].getWithDefault<std::string>("A") == "B" ? 'B' : 'A';
      RoutedPlugin plugin;
      const auto value = payload["plugin"].isObject() ? payload["plugin"] : payload;
      if (!decodePlugin(value, plugin, error) ||
          !decodeAutomationEdits(payload["automationEdits"], decoded.automationEdits,
                                 error)) {
        return false;
      }
      decoded.plugins.push_back(std::move(plugin));
    } else if (type == "automation/edit") {
      decoded.action = UiAction::editAutomationParameter;
      RoutedAutomationEdit edit;
      if (!decodeAutomationEdit(payload, edit, error)) {
        return false;
      }
      // A single gesture keeps its flat shape: the bulk array exists only so a
      // message that also carries pipelines can name several targets at once.
      decoded.pipeline = edit.pipeline;
      decoded.pluginId = edit.pluginId;
      decoded.elementIndex = edit.elementIndex;
      decoded.normalizedValue = edit.normalized;
      decoded.pluginType = std::move(edit.pluginType);
      decoded.parameterKey = std::move(edit.parameterKey);
    } else if (type == "automation/endGesture") {
      // The release of a pointer gesture, which names only the targets it
      // moved. There is no value here on purpose: the values already travelled
      // with the plug-in images that carried them, and a close that restated
      // one would write an automation point the user never performed.
      decoded.action = UiAction::endAutomationGesture;
      if (!decodeAutomationTargets(payload["targets"], decoded.automationEdits,
                                   error)) {
        return false;
      }
    } else if (type == "pipeline/assetBegin") {
      decoded.action = UiAction::beginPluginAsset;
      if (!decodeAssetBegin(payload, decoded, error)) {
        return false;
      }
    } else if (type == "pipeline/assetChunk") {
      decoded.action = UiAction::appendPluginAsset;
      if (!decodeAssetIdentity(payload, decoded, error, true)) {
        return false;
      }
      const auto offset = payload["offset"].getWithDefault<std::int64_t>(-1);
      decoded.content = payload["data"].getWithDefault<std::string>({});
      if (offset < 0 || offset > EngineHost::kMaximumAssetPayloadBytes ||
          decoded.content.empty() || decoded.content.size() > kMaximumAssetChunkBase64Bytes) {
        setError(error, "DSP asset chunk is invalid");
        return false;
      }
      decoded.assetOffset = static_cast<std::size_t>(offset);
    } else if (type == "pipeline/assetCommit") {
      decoded.action = UiAction::commitPluginAsset;
      if (!decodeAssetIdentity(payload, decoded, error, true)) {
        return false;
      }
    } else if (type == "pipeline/assetClear") {
      decoded.action = UiAction::clearPluginAsset;
      if (!decodeAssetIdentity(payload, decoded, error, false)) {
        return false;
      }
    } else if (type == "pipeline/assetState") {
      decoded.action = UiAction::readPluginAssetState;
      if (!decodeAssetIdentity(payload, decoded, error, false)) {
        return false;
      }
    } else if (type == "pipeline/masterBypass") {
      decoded.action = UiAction::setMasterBypass;
      decoded.masterBypass = payload["value"].getWithDefault<bool>(false);
    } else if (type == "os/set") {
      decoded.action = UiAction::setOversampling;
      decodeOversampling(payload, decoded.oversampling);
    } else if (type == "telemetry/read") {
      decoded.action = UiAction::readTelemetry;
    } else if (type == "telemetry/discard") {
      decoded.action = UiAction::discardTelemetry;
    } else if (type == "storage/fileExists") {
      decoded.action = UiAction::storageFileExists;
      decoded.path = payload["path"].getWithDefault<std::string>({});
    } else if (type == "storage/readFile") {
      decoded.action = UiAction::storageReadFile;
      decoded.path = payload["path"].getWithDefault<std::string>({});
    } else if (type == "storage/writeFile") {
      decoded.action = UiAction::storageWriteFile;
      decoded.path = payload["path"].getWithDefault<std::string>({});
      decoded.content = payload["content"].getWithDefault<std::string>({});
    } else if (type == "config/load") {
      decoded.action = UiAction::loadConfig;
    } else if (type == "config/save") {
      decoded.action = UiAction::saveConfig;
      if (!payload["config"].isObject()) {
        setError(error, "Config payload must be an object");
        return false;
      }
      decoded.content = choc::json::toString(payload["config"]);
    } else if (type == "dialog/openPreset") {
      decoded.action = UiAction::openPresetDialog;
    } else if (type == "dialog/savePreset") {
      decoded.action = UiAction::savePresetDialog;
      decoded.defaultName = payload["defaultName"].getWithDefault<std::string>("preset.effetune_preset");
    } else {
      setError(error, "Unsupported bridge message type: " + type);
      return false;
    }

    message = std::move(decoded);
    return true;
  } catch (const choc::json::ParseError &exception) {
    setError(error, exception.what());
    return false;
  }
}

} // namespace effetune::vst
