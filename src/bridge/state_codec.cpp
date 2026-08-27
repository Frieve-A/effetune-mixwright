#include "bridge/state_codec.h"

#include "engine/automation_binding.h"

#include "choc/text/choc_JSON.h"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <iterator>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace effetune::vst {
namespace {

using choc::value::Value;
using choc::value::ValueView;

Value objectFromJson(const std::string &json) {
  try {
    auto value = choc::json::parse(json);
    if (value.isObject()) {
      return value;
    }
  } catch (const choc::json::ParseError &) {
  }
  return choc::value::createObject({});
}

Value mergeJsonValues(const ValueView preserved, const ValueView incoming) {
  if (preserved.isObject() && incoming.isObject()) {
    auto merged = choc::value::createObject({});
    for (std::uint32_t index = 0; index < preserved.size(); ++index) {
      const auto member = preserved.getObjectMemberAt(index);
      if (!incoming.hasObjectMember(member.name)) {
        merged.addMember(member.name, Value(member.value));
      }
    }
    for (std::uint32_t index = 0; index < incoming.size(); ++index) {
      const auto member = incoming.getObjectMemberAt(index);
      const auto oldValue = preserved[member.name];
      merged.addMember(member.name, oldValue.isVoid()
                                        ? Value(member.value)
                                        : mergeJsonValues(oldValue, member.value));
    }
    return merged;
  }

  if (preserved.isArray() && incoming.isArray()) {
    auto merged = choc::value::createEmptyArray();
    for (std::uint32_t index = 0; index < incoming.size(); ++index) {
      merged.addArrayElement(index < preserved.size()
                                 ? mergeJsonValues(preserved[index], incoming[index])
                                 : Value(incoming[index]));
    }
    return merged;
  }

  return Value(incoming);
}

std::string serializedPluginType(const PluginState &plugin) {
  return objectFromJson(plugin.extraJson)["type"].getWithDefault<std::string>({});
}

bool samePluginIdentity(const PluginState &left, const PluginState &right) {
  if (left.id != right.id) {
    return false;
  }
  const auto leftType = serializedPluginType(left);
  const auto rightType = serializedPluginType(right);
  if (!leftType.empty() && !rightType.empty()) {
    return leftType == rightType;
  }
  return left.name == right.name;
}

std::string unknownMembersJson(const ValueView object,
                               const std::unordered_set<std::string_view> &known) {
  auto extras = choc::value::createObject({});
  for (std::uint32_t index = 0; index < object.size(); ++index) {
    const auto member = object.getObjectMemberAt(index);
    if (!known.contains(member.name)) {
      extras.addMember(member.name, member.value);
    }
  }
  return choc::json::toString(extras);
}

std::string phaseToString(const OversamplingPhase phase) {
  return phase == OversamplingPhase::minimum ? "minimum" : "linear";
}

std::string qualityToString(const FilterQuality quality) {
  switch (quality) {
  case FilterQuality::low:
    return "low";
  case FilterQuality::high:
    return "high";
  case FilterQuality::ultra:
    return "ultra";
  case FilterQuality::medium:
  default:
    return "medium";
  }
}

std::string lifecycleToString(const AutomationBindingLifecycle lifecycle) {
  switch (lifecycle) {
  case AutomationBindingLifecycle::active:
    return "active";
  case AutomationBindingLifecycle::tombstone:
    return "tombstone";
  case AutomationBindingLifecycle::dormant:
  default:
    return "dormant";
  }
}

AutomationBindingLifecycle lifecycleFromString(const std::string_view lifecycle) noexcept {
  if (lifecycle == "active") {
    return AutomationBindingLifecycle::active;
  }
  if (lifecycle == "tombstone") {
    return AutomationBindingLifecycle::tombstone;
  }
  return AutomationBindingLifecycle::dormant;
}

Value encodeAutomation(const AutomationState &automation) {
  auto object = objectFromJson(automation.extraJson);
  object.addMember("logicalPluginIdWatermark",
                   static_cast<std::int64_t>(automation.logicalPluginIdWatermark));
  auto bindings = choc::value::createEmptyArray();
  for (const auto &binding : automation.bindings) {
    auto encoded = objectFromJson(binding.extraJson);
    encoded.addMember("slot", static_cast<std::int64_t>(binding.slot));
    encoded.addMember("pipeline", std::string(1, binding.pipeline == 'B' ? 'B' : 'A'));
    encoded.addMember("pluginId", static_cast<std::int64_t>(binding.pluginId));
    encoded.addMember("pluginType", binding.pluginType);
    encoded.addMember("parameterKey", binding.parameterKey);
    encoded.addMember("elementIndex", static_cast<std::int64_t>(binding.elementIndex));
    encoded.addMember("lifecycle", lifecycleToString(binding.lifecycle));
    bindings.addArrayElement(std::move(encoded));
  }
  object.addMember("bindings", std::move(bindings));
  return object;
}

Value encodePipeline(const PipelineState &pipeline) {
  auto array = choc::value::createEmptyArray();
  for (const auto &plugin : pipeline.plugins) {
    auto object = objectFromJson(plugin.extraJson);
    object.addMember("id", static_cast<std::int64_t>(plugin.id));
    object.addMember("name", plugin.name);
    object.addMember("enabled", plugin.enabled);
    try {
      object.addMember("parameters", choc::json::parse(plugin.parametersJson));
    } catch (const choc::json::ParseError &) {
      object.addMember("parameters", choc::value::createObject({}));
    }
    if (plugin.inputBus != 0) {
      object.addMember("inputBus", static_cast<std::int64_t>(plugin.inputBus));
    }
    if (plugin.outputBus != 0) {
      object.addMember("outputBus", static_cast<std::int64_t>(plugin.outputBus));
    }
    if (plugin.channel.has_value()) {
      object.addMember("channel", *plugin.channel);
    }
    if (plugin.unknown) {
      object.addMember("unknown", true);
    }
    array.addArrayElement(std::move(object));
  }
  return array;
}

std::uint32_t readId(const ValueView object, const std::uint32_t fallback) {
  const auto id = object["id"].getWithDefault<std::int64_t>(fallback);
  return id > 0 && id <= UINT32_MAX ? static_cast<std::uint32_t>(id) : fallback;
}

std::uint8_t readBus(const ValueView object, const char *longName, const char *shortName) {
  auto value = object[longName].getWithDefault<std::int64_t>(-1);
  if (value < 0) {
    value = object[shortName].getWithDefault<std::int64_t>(0);
  }
  return value >= 0 && value <= kMaxBus ? static_cast<std::uint8_t>(value) : 0;
}

std::optional<std::string> normalizeChannel(std::string channel) {
  if (channel.empty()) {
    return std::nullopt;
  }
  if (channel == "Left") {
    channel = "L";
  } else if (channel == "Right") {
    channel = "R";
  } else if (channel == "All") {
    channel = "A";
  }
  return channel;
}

std::string shortParametersToJson(const ValueView object) {
  static const std::unordered_set<std::string_view> metadata{
      "id", "nm", "en", "ib", "ob", "ch", "name", "enabled", "inputBus",
      "outputBus", "channel", "unknown", "executionCapabilities"};
  auto parameters = choc::value::createObject({});
  for (std::uint32_t index = 0; index < object.size(); ++index) {
    const auto member = object.getObjectMemberAt(index);
    if (!metadata.contains(member.name)) {
      parameters.addMember(member.name, member.value);
    }
  }
  return choc::json::toString(parameters);
}

bool decodePipeline(const ValueView array, PipelineState &pipeline,
                    std::uint32_t &nextId,
                    std::unordered_set<std::uint32_t> &pluginIds,
                    std::string *error) {
  if (!array.isArray()) {
    return false;
  }
  if (array.size() > kMaxPipelineNodes) {
    if (error != nullptr) {
      *error = "State pipeline exceeds the node limit";
    }
    return false;
  }
  pipeline.plugins.clear();
  pipeline.plugins.reserve(array.size());
  for (std::uint32_t index = 0; index < array.size(); ++index) {
    const auto item = array[index];
    if (!item.isObject()) {
      continue;
    }
    PluginState plugin;
    const auto encodedId = item["id"].getWithDefault<std::int64_t>(0);
    if ((encodedId <= 0 || encodedId > UINT32_MAX) && nextId == 0) {
      continue;
    }
    plugin.id = readId(item, nextId);
    if (plugin.id == UINT32_MAX) {
      nextId = 0;
    } else if (nextId != 0) {
      nextId = std::max(nextId, plugin.id + 1u);
    }
    plugin.name = item["name"].getWithDefault<std::string>({});
    const bool shortForm = plugin.name.empty();
    if (shortForm) {
      plugin.name = item["nm"].getWithDefault<std::string>({});
    }
    if (plugin.name.empty()) {
      continue;
    }
    if (!pluginIds.insert(plugin.id).second) {
      if (error != nullptr) {
        *error = "State pipeline plugin IDs must be unique";
      }
      return false;
    }
    plugin.enabled = shortForm ? item["en"].getWithDefault<bool>(true)
                               : item["enabled"].getWithDefault<bool>(true);
    plugin.inputBus = readBus(item, "inputBus", "ib");
    plugin.outputBus = readBus(item, "outputBus", "ob");
    auto channel = item["channel"].getWithDefault<std::string>({});
    if (channel.empty()) {
      channel = item["ch"].getWithDefault<std::string>({});
    }
    plugin.channel = normalizeChannel(std::move(channel));
    plugin.unknown = item["unknown"].getWithDefault<bool>(false);
    static const std::unordered_set<std::string_view> knownPluginFields{
        "id", "nm", "en", "ib", "ob", "ch", "name", "enabled",
        "parameters", "inputBus", "outputBus", "channel", "unknown",
        "executionCapabilities"};
    plugin.extraJson = unknownMembersJson(item, knownPluginFields);
    const auto parameters = item["parameters"];
    plugin.parametersJson = parameters.isObject() ? choc::json::toString(parameters)
                                                   : shortParametersToJson(item);
    pipeline.plugins.push_back(std::move(plugin));
  }
  return true;
}

void decodeOversampling(const ValueView object, OversamplingSettings &settings) {
  if (!object.isObject()) {
    return;
  }
  const auto factor = object["factor"].getWithDefault<std::int64_t>(1);
  if (factor == 1 || factor == 2 || factor == 4 || factor == 8) {
    settings.factor = static_cast<std::uint32_t>(factor);
  }
  settings.phase = object["phase"].getWithDefault<std::string>("linear") == "minimum"
                       ? OversamplingPhase::minimum
                       : OversamplingPhase::linear;
  const auto quality = object["quality"].getWithDefault<std::string>("medium");
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

bool decodeAutomation(const ValueView object, AutomationState &automation) {
  if (!object.isObject()) {
    return true;
  }
  automation.initialized = true;
  const auto watermark =
      object["logicalPluginIdWatermark"].getWithDefault<std::int64_t>(0);
  if (watermark >= 0 && watermark <= UINT32_MAX) {
    automation.logicalPluginIdWatermark = static_cast<std::uint32_t>(watermark);
  }
  static const std::unordered_set<std::string_view> knownAutomationFields{
      "logicalPluginIdWatermark", "bindings"};
  automation.extraJson = unknownMembersJson(object, knownAutomationFields);

  const auto bindings = object["bindings"];
  if (!bindings.isArray()) {
    return true;
  }
  if (bindings.size() > kAutomationSlotCount) {
    return false;
  }
  automation.bindings.reserve(bindings.size());
  for (std::uint32_t index = 0; index < bindings.size(); ++index) {
    const auto encoded = bindings[index];
    if (!encoded.isObject()) {
      continue;
    }
    const auto slot = encoded["slot"].getWithDefault<std::int64_t>(-1);
    const auto pluginId = encoded["pluginId"].getWithDefault<std::int64_t>(0);
    const auto elementIndex = encoded["elementIndex"].getWithDefault<std::int64_t>(0);
    const auto pluginType = encoded["pluginType"].getWithDefault<std::string>({});
    const auto parameterKey = encoded["parameterKey"].getWithDefault<std::string>({});
    if (slot >= static_cast<std::int64_t>(kAutomationSlotCount)) {
      return false;
    }
    if (slot < 0 || pluginId <= 0 || pluginId > UINT32_MAX ||
        elementIndex < 0 || elementIndex > UINT32_MAX || pluginType.empty() ||
        parameterKey.empty()) {
      continue;
    }
    AutomationBindingState binding;
    binding.slot = static_cast<std::uint32_t>(slot);
    binding.pipeline = encoded["pipeline"].getWithDefault<std::string>("A") == "B" ? 'B' : 'A';
    binding.pluginId = static_cast<std::uint32_t>(pluginId);
    binding.pluginType = pluginType;
    binding.parameterKey = parameterKey;
    binding.elementIndex = static_cast<std::uint32_t>(elementIndex);
    binding.lifecycle = lifecycleFromString(
        encoded["lifecycle"].getWithDefault<std::string>("dormant"));
    static const std::unordered_set<std::string_view> knownBindingFields{
        "slot",       "pipeline",     "pluginId",  "pluginType",
        "parameterKey", "elementIndex", "lifecycle"};
    binding.extraJson = unknownMembersJson(encoded, knownBindingFields);
    automation.bindings.push_back(std::move(binding));
  }
  return true;
}

} // namespace

std::string StateCodec::encode(const PluginStateDocument &state) {
  auto root = objectFromJson(state.extraJson);
  root.addMember("formatVersion", static_cast<std::int64_t>(state.formatVersion));
  root.addMember("appVersion", state.appVersion);
  root.addMember("pipelineA", encodePipeline(state.pipelineA));
  root.addMember("pipelineB", state.pipelineBInitialized ? encodePipeline(state.pipelineB)
                                                         : Value{});
  root.addMember("currentPipeline", std::string(1, state.currentPipeline == 'B' ? 'B' : 'A'));
  root.addMember("masterBypass", state.masterBypass);
  root.addMember("oversampling",
                 choc::json::create("factor", static_cast<std::int64_t>(state.oversampling.factor),
                                    "phase", phaseToString(state.oversampling.phase), "quality",
                                    qualityToString(state.oversampling.quality)));
  auto ui = choc::value::createObject({});
  ui.addMember("columns", static_cast<std::int64_t>(state.ui.columns));
  ui.addMember("zoom", state.ui.zoom);
  root.addMember("ui", std::move(ui));
  if (state.automation.initialized) {
    root.addMember("automation", encodeAutomation(state.automation));
  }
  return choc::json::toString(root, true);
}

bool StateCodec::decode(const std::string_view json, PluginStateDocument &state,
                        std::string *error) {
  try {
    const auto root = choc::json::parse(json);
    PluginStateDocument decoded;
    std::uint32_t nextId = 1;
    std::unordered_set<std::uint32_t> pluginIds;

    if (root.isArray()) {
      if (!decodePipeline(root, decoded.pipelineA, nextId, pluginIds, error)) {
        return false;
      }
    } else if (root.isObject()) {
      const auto formatVersion = root["formatVersion"].getWithDefault<std::int64_t>(1);
      if (formatVersion < 1 || formatVersion > 1) {
        if (error != nullptr) {
          *error = "Unsupported state formatVersion";
        }
        return false;
      }
      decoded.formatVersion = static_cast<std::uint32_t>(formatVersion);
      decoded.appVersion = root["appVersion"].getWithDefault<std::string>("unknown");
      static const std::unordered_set<std::string_view> knownRootFields{
          "formatVersion", "appVersion", "pipelineA", "pipelineB", "pipeline",
          "plugins", "currentPipeline", "masterBypass", "oversampling", "ui",
          "automation"};
      decoded.extraJson = unknownMembersJson(root, knownRootFields);

      // Reserve every historical logical ID before assigning IDs to legacy
      // pipeline entries which did not serialize one themselves.
      if (!decodeAutomation(root["automation"], decoded.automation)) {
        if (error != nullptr) {
          *error = "Invalid automation bindings";
        }
        return false;
      }
      if (decoded.automation.logicalPluginIdWatermark == UINT32_MAX) {
        nextId = 0;
      } else if (decoded.automation.logicalPluginIdWatermark != 0) {
        nextId = decoded.automation.logicalPluginIdWatermark + 1u;
      }

      if (root["pipelineA"].isArray()) {
        if (!decodePipeline(root["pipelineA"], decoded.pipelineA, nextId, pluginIds,
                            error)) {
          return false;
        }
        if (root["pipelineB"].isArray()) {
          if (!decodePipeline(root["pipelineB"], decoded.pipelineB, nextId, pluginIds,
                              error)) {
            return false;
          }
          decoded.pipelineBInitialized = true;
        }
      } else if (root["pipeline"].isArray()) {
        if (!decodePipeline(root["pipeline"], decoded.pipelineA, nextId, pluginIds,
                            error)) {
          return false;
        }
      } else if (root["plugins"].isArray()) {
        if (!decodePipeline(root["plugins"], decoded.pipelineA, nextId, pluginIds,
                            error)) {
          return false;
        }
      } else {
        if (error != nullptr) {
          *error = "State contains no recognized pipeline";
        }
        return false;
      }
      decoded.currentPipeline =
          root["currentPipeline"].getWithDefault<std::string>("A") == "B" ? 'B' : 'A';
      decoded.masterBypass = root["masterBypass"].getWithDefault<bool>(false);
      decodeOversampling(root["oversampling"], decoded.oversampling);
      const auto ui = root["ui"];
      if (ui.isObject()) {
        const auto columns = ui["columns"].getWithDefault<std::int64_t>(1);
        decoded.ui.columns = columns > 0 && columns <= 8 ? static_cast<std::uint32_t>(columns) : 1;
        decoded.ui.zoom = std::clamp(ui["zoom"].getWithDefault<double>(1.0), 0.5, 2.0);
      }
    } else {
      if (error != nullptr) {
        *error = "State root must be an object or array";
      }
      return false;
    }
    state = std::move(decoded);
    return true;
  } catch (const std::exception &exception) {
    if (error != nullptr) {
      *error = exception.what();
    }
    return false;
  }
}

std::string mergeExtraJsonObjects(const std::string_view preserved,
                                  const std::string_view incoming) {
  const auto base = objectFromJson(std::string(preserved));
  const auto overlay = objectFromJson(std::string(incoming));
  return choc::json::toString(mergeJsonValues(base, overlay));
}

PipelineState reconcilePipelineSnapshot(const PipelineState &preserved, PipelineState incoming,
                                        const bool preserveAllMissing) {
  PipelineState merged;
  merged.plugins.reserve(preserved.plugins.size() + incoming.plugins.size());

  for (auto &plugin : incoming.plugins) {
    const auto old = std::find_if(preserved.plugins.begin(), preserved.plugins.end(),
                                  [&plugin](const PluginState &candidate) {
                                    return samePluginIdentity(candidate, plugin);
                                  });
    if (old != preserved.plugins.end()) {
      plugin.extraJson = mergeExtraJsonObjects(old->extraJson, plugin.extraJson);
      plugin.parametersJson =
          mergeExtraJsonObjects(old->parametersJson, plugin.parametersJson);
    }
    merged.plugins.push_back(std::move(plugin));
  }

  for (std::size_t oldIndex = 0; oldIndex < preserved.plugins.size(); ++oldIndex) {
    const auto &old = preserved.plugins[oldIndex];
    const auto restored = std::find_if(merged.plugins.begin(), merged.plugins.end(),
                                       [&old](const PluginState &candidate) {
                                         return samePluginIdentity(old, candidate);
                                       });
    if (restored != merged.plugins.end() || (!old.unknown && !preserveAllMissing)) {
      continue;
    }

    auto sidecar = old;
    sidecar.unknown = true;
    auto insertion = merged.plugins.end();
    for (std::size_t nextIndex = oldIndex + 1; nextIndex < preserved.plugins.size();
         ++nextIndex) {
      const auto &next = preserved.plugins[nextIndex];
      insertion = std::find_if(merged.plugins.begin(), merged.plugins.end(),
                               [&next](const PluginState &candidate) {
                                 return samePluginIdentity(next, candidate);
                               });
      if (insertion != merged.plugins.end()) {
        break;
      }
    }
    merged.plugins.insert(insertion, std::move(sidecar));
  }
  return merged;
}

PipelineState UndoOpaqueStateStore::reconcile(const char pipeline,
                                              const PipelineState &preserved,
                                              PipelineState incoming,
                                              const bool preserveAllMissing) {
  for (auto &plugin : incoming.plugins) {
    const auto recovered = std::find_if(entries_.rbegin(), entries_.rend(),
                                        [pipeline, &plugin](const Entry &entry) {
                                          return entry.pipeline == pipeline &&
                                                 samePluginIdentity(entry.plugin, plugin);
                                        });
    if (recovered == entries_.rend()) {
      continue;
    }
    plugin.parametersJson =
        mergeExtraJsonObjects(recovered->plugin.parametersJson, plugin.parametersJson);
    plugin.extraJson = mergeExtraJsonObjects(recovered->plugin.extraJson, plugin.extraJson);
    entries_.erase(std::next(recovered).base());
  }

  if (!preserveAllMissing) {
    for (const auto &old : preserved.plugins) {
      const auto retained = std::find_if(incoming.plugins.begin(), incoming.plugins.end(),
                                         [&old](const PluginState &plugin) {
                                           return samePluginIdentity(old, plugin);
                                         });
      if (old.unknown || retained != incoming.plugins.end()) {
        continue;
      }
      std::erase_if(entries_, [pipeline, &old](const Entry &entry) {
        return entry.pipeline == pipeline && samePluginIdentity(entry.plugin, old);
      });
      if (entries_.size() == kMaxEntries) {
        entries_.erase(entries_.begin());
      }
      entries_.push_back({pipeline, old});
    }
  }

  return reconcilePipelineSnapshot(preserved, std::move(incoming), preserveAllMissing);
}

void UndoOpaqueStateStore::clear() noexcept { entries_.clear(); }

} // namespace effetune::vst
