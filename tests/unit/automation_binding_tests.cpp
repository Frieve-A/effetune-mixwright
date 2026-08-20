#include "bridge/message_router.h"
#include "bridge/state_codec.h"
#include "engine/automation_binding.h"

#include "choc/text/choc_JSON.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "../support/crt_dialog_suppression.h"

namespace {

using namespace effetune::vst;

void expect(const bool condition, const std::string &message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

void expectNear(const double actual, const double expected, const double tolerance,
                const std::string &message) {
  expect(std::abs(actual - expected) <= tolerance, message);
}

AutomationTargetDescriptor target(const char pipeline, const std::uint32_t pluginId,
                                  std::string key, const double current,
                                  const std::uint32_t element = 0) {
  AutomationTargetDescriptor result;
  result.identity = {pipeline, pluginId, "SyntheticPlugin", std::move(key), element};
  result.title = "Synthetic Parameter";
  result.shortTitle = "Parameter";
  result.units = "dB";
  result.defaultNormalized = 0.25;
  result.currentNormalized = current;
  return result;
}

PluginState plugin(const std::uint32_t id, const std::string &parameters = "{}") {
  return PluginState{id, "Synthetic", true, 0, 0, std::nullopt, parameters, false,
                     R"({"type":"SyntheticPlugin"})"};
}

std::optional<std::uint32_t>
bindSlot(PluginStateDocument &document,
         const std::vector<AutomationTargetDescriptor> &targets,
         const AutomationTargetIdentity &identity,
         bool *capacityExhausted = nullptr) {
  auto exhausted = false;
  const auto slot = bindAutomationTarget(document, targets, identity, exhausted);
  if (capacityExhausted != nullptr) {
    *capacityExhausted = exhausted;
  }
  return slot;
}

void testAllocationLifecycleAndWatermark() {
  PluginStateDocument document;
  document.pipelineA.plugins = {plugin(7, R"({"gain":0.75})")};
  document.pipelineB.plugins = {plugin(9, R"({"mix":0.5})")};
  document.pipelineBInitialized = true;
  std::vector targets{target('A', 7, "gain", 0.75),
                      target('B', 9, "mix", 0.5)};

  AutomationBindingRegistry registry;
  auto result = registry.reconcile(document, targets);
  expect(result.stateChanged && !result.slotsChanged &&
             document.automation.bindings.empty() && registry.activeSlots().empty(),
         "reconcile never allocates a slot for an unbound eligible target");
  expect(document.automation.initialized &&
             document.automation.logicalPluginIdWatermark == 9,
         "automation state and watermark initialize from both pipelines");

  expect(bindSlot(document, targets, targets[0].identity) ==
             std::optional<std::uint32_t>{0u} &&
             bindSlot(document, targets, targets[1].identity) ==
                 std::optional<std::uint32_t>{1u},
         "explicit binding reserves the smallest never-used slots in order");
  result = registry.reconcile(document, targets);
  expect(result.slotsChanged && document.automation.bindings.size() == 2 &&
             registry.slot(0)->currentNormalized == 0.75,
         "reconcile projects the new bindings and reads current from the target");
  expect(bindSlot(document, targets, targets[0].identity) ==
             std::optional<std::uint32_t>{0u} &&
             document.automation.bindings.size() == 2,
         "rebinding an existing identity returns its slot without allocating");

  document.pipelineA.plugins.clear();
  targets.erase(targets.begin());
  result = registry.reconcile(document, targets);
  expect(document.automation.bindings[0].lifecycle ==
             AutomationBindingLifecycle::tombstone &&
             registry.slot(0) == nullptr,
         "deleted plug-in leaves a tombstone");

  document.pipelineA.plugins = {plugin(10)};
  targets.push_back(target('A', 10, "gain", 0.4));
  expect(bindSlot(document, targets, targets.back().identity) ==
             std::optional<std::uint32_t>{2u},
         "a new target cannot reuse a tombstoned slot");
  result = registry.reconcile(document, targets);
  expect(document.automation.logicalPluginIdWatermark == 10 &&
             document.automation.bindings.back().slot == 2,
         "the reactivated projection advances the watermark");

  document.pipelineA.plugins.push_back(plugin(7, R"({"gain":0.6})"));
  targets.push_back(target('A', 7, "gain", 0.6));
  result = registry.reconcile(document, targets);
  expect(document.automation.bindings[0].lifecycle ==
             AutomationBindingLifecycle::active &&
             registry.slot(0) != nullptr && registry.slot(0)->currentNormalized == 0.6,
         "historical target restoration reactivates its original slot");

  document.pipelineA.plugins.clear();
  document.pipelineB.plugins.clear();
  result = registry.reconcile(document, {});
  expect(document.automation.logicalPluginIdWatermark == 10,
         "logical plug-in watermark never decreases");
}

void testBindingRejectsTargetsOutsideTheGeneratedCatalog() {
  PluginStateDocument document;
  document.pipelineA.plugins = {plugin(5)};
  const std::vector targets{target('A', 5, "gain", 0.5)};

  expect(!bindSlot(document, targets, {'A', 5, "SyntheticPlugin", "missing", 0}).has_value() &&
             !bindSlot(document, targets, {'A', 6, "SyntheticPlugin", "gain", 0}).has_value() &&
             document.automation.bindings.empty(),
         "unknown parameters and absent plug-ins never consume a slot");
}

void testDuplicateAndDormantBindings() {
  PluginStateDocument document;
  document.pipelineA.plugins = {plugin(4)};
  document.automation.initialized = true;
  document.automation.bindings = {
      {5, 'A', 4, "SyntheticPlugin", "gain", 0,
       AutomationBindingLifecycle::active, "{}"},
      {2, 'A', 4, "SyntheticPlugin", "gain", 0,
       AutomationBindingLifecycle::active, "{}"},
      {6, 'A', 4, "FuturePlugin", "future", 0,
       AutomationBindingLifecycle::active, R"({"futureBinding":{"v":2}})"}};
  const std::vector targets{target('A', 4, "gain", 0.3)};

  AutomationBindingRegistry registry;
  (void)registry.reconcile(document, targets);
  expect(document.automation.bindings[0].lifecycle ==
             AutomationBindingLifecycle::dormant &&
             document.automation.bindings[1].lifecycle ==
                 AutomationBindingLifecycle::active &&
             registry.slot(2) != nullptr && registry.slot(5) == nullptr,
         "smallest ParamID wins duplicate target corruption");
  expect(document.automation.bindings[2].lifecycle ==
             AutomationBindingLifecycle::dormant &&
             document.automation.bindings[2].extraJson.find("futureBinding") !=
                 std::string::npos,
         "unknown target remains dormant with opaque members");
}

void testCapacityExhaustionPreservesExistingBindings() {
  PluginStateDocument document;
  document.pipelineA.plugins = {plugin(1)};
  document.automation.initialized = true;
  document.automation.bindings.reserve(kAutomationSlotCount);
  for (std::uint32_t slot = 0; slot < kAutomationSlotCount; ++slot) {
    document.automation.bindings.push_back(
        {slot, 'A', 1, "SyntheticPlugin", "historical" + std::to_string(slot), 0,
         AutomationBindingLifecycle::dormant, "{}"});
  }
  const std::vector targets{target('A', 1, "newTarget", 0.5)};

  auto capacityExhausted = false;
  expect(!bindSlot(document, targets, targets[0].identity, &capacityExhausted).has_value() &&
             capacityExhausted &&
             document.automation.bindings.size() == kAutomationSlotCount,
         "capacity exhaustion leaves every historical binding intact");

  AutomationBindingRegistry registry;
  const auto result = registry.reconcile(document, targets);
  expect(!result.slotsChanged && registry.activeSlots().empty() &&
             document.automation.bindings.size() == kAutomationSlotCount,
         "a refused allocation cannot replace or renumber historical lanes");
}

void testGeneratedCatalogPassThrough() {
  PluginStateDocument document;
  auto dcOffset = plugin(12, R"({"of":0.25})");
  dcOffset.name = "DC Offset";
  dcOffset.extraJson = R"({"type":"DCOffsetPlugin"})";
  document.pipelineA.plugins = {std::move(dcOffset)};

  const auto targets = generatedAutomationTargets(document);
  expect(targets.size() == 1 && targets[0].identity.parameterKey == "of" &&
             targets[0].currentNormalized == 0.625 && targets[0].packedOffset == 0 &&
             targets[0].continuous &&
             targets[0].title.find("A #12 offset[1] - DC Offset -") == 0 &&
             targets[0].shortTitle.find("A #12 offset[1] - DC Offset -") == 0,
         "the generated catalog exposes DCOffset with derived identity and conversion");
  AutomationBindingRegistry registry;
  expect(bindSlot(document, targets, targets[0].identity) ==
             std::optional<std::uint32_t>{0u},
         "the generated target binds the first fixed slot on demand");
  (void)registry.reconcile(document, targets);
  expect(document.automation.bindings.size() == 1 &&
             registry.slot(0) != nullptr && registry.binding(0) != nullptr &&
             registry.binding(0)->parameterKey == "of",
         "the generated key activates and persists the first fixed slot");
  RoutedUiMessage edit;
  std::string error;
  expect(MessageRouter::decode(
             R"({"type":"automation/edit","payload":{"pipeline":"A","pluginId":12,"pluginType":"DCOffsetPlugin","parameterKey":"of","elementIndex":0,"normalized":0.75}})",
             edit, &error) &&
             edit.action == UiAction::editAutomationParameter &&
             edit.parameterKey == targets[0].identity.parameterKey,
         "the WebView message key matches the native generated identity");
  const AutomationTargetIdentity routedIdentity{
      edit.pipeline, edit.pluginId, edit.pluginType, edit.parameterKey,
      edit.elementIndex};
  expect(registry.findActiveSlot(routedIdentity) ==
             std::optional<std::uint32_t>{0u},
         "the routed WebView identity resolves the native catalog slot");
  expect(applyAutomationNormalizedValue(document, *registry.binding(0),
                                        edit.normalizedValue),
         "the persisted generated key resolves back to the native catalog");
  const auto rewritten = choc::json::parse(document.pipelineA.plugins[0].parametersJson);
  expectNear(rewritten["of"].get<double>(), 0.5, 1e-6,
             "the native catalog round trip writes the generated packed key");
}

void testSyntheticNodeEnableTargets() {
  PluginStateDocument document;
  auto dcOffset = plugin(12, R"({"of":0.25})");
  dcOffset.name = "DC Offset";
  dcOffset.extraJson = R"({"type":"DCOffsetPlugin"})";
  document.pipelineA.plugins = {std::move(dcOffset), plugin(13)};

  const auto targets = eligibleAutomationTargets(document);
  const auto enableCount = std::count_if(
      targets.begin(), targets.end(), [](const AutomationTargetDescriptor &candidate) {
        return candidate.identity.parameterKey == kNodeEnableParameterKey;
      });
  expect(targets.size() == 3 && enableCount == 2,
         "every instance adds one enable target beside its catalog parameters");
  const auto enableTarget = std::find_if(
      targets.begin(), targets.end(), [](const AutomationTargetDescriptor &candidate) {
        return candidate.identity.parameterKey == kNodeEnableParameterKey &&
               candidate.identity.pluginId == 13;
      });
  expect(enableTarget != targets.end() &&
             enableTarget->identity.pluginType == "SyntheticPlugin" &&
             enableTarget->identity.elementIndex == 0 &&
             enableTarget->applyKind == AutomationApplyKind::nodeEnable &&
             enableTarget->stepCount == 1 &&
             enableTarget->normalization == AutomationValueNormalization::boolean &&
             !enableTarget->continuous && enableTarget->currentNormalized == 1.0,
         "the enable target is one generic boolean that ignores the effect type");

  AutomationBindingRegistry registry;
  expect(bindSlot(document, targets, enableTarget->identity) ==
             std::optional<std::uint32_t>{0u},
         "the enable target binds a fixed slot on demand");
  (void)registry.reconcile(document, targets);
  expect(registry.binding(0) != nullptr && registry.slot(0) != nullptr &&
             registry.slot(0)->applyKind == AutomationApplyKind::nodeEnable,
         "the projected slot keeps the node-enable apply kind");

  const auto parameters = document.pipelineA.plugins[1].parametersJson;
  expect(applyAutomationValue(document, *registry.binding(0), 0.0) &&
             !document.pipelineA.plugins[1].enabled &&
             document.pipelineA.plugins[1].parametersJson == parameters,
         "applying a node-enable value clears enabled without touching parameters");
  expect(applyAutomationValue(document, *registry.binding(0), 1.0) &&
             document.pipelineA.plugins[1].enabled &&
             document.pipelineA.plugins[0].enabled,
         "the node-enable value restores only its own instance");
}

void testGeneratedCatalogPublicPackedTransforms() {
  struct Case {
    const char *type;
    const char *packedJson;
    const char *key;
    const char *publicName;
    double minimum;
    double maximum;
    double defaultValue;
    double expectedPackedAtHalf;
    AutomationValueTransform transform;
  };
  const std::vector<Case> cases{
      {"TiltEQPlugin", R"({"f0":6.91})", "f0", "pivotFrequency", std::exp(3.0),
       std::exp(9.9), std::exp(6.91), 6.45,
       AutomationValueTransform::naturalLog},
      {"DigitalErrorEmulatorPlugin", R"({"be":-6.0})", "be", "bitErrorRate", 1e-12,
       1e-2, 1e-6, -7.0, AutomationValueTransform::log10},
      {"G726ADPCMSimulatorPlugin", R"({"re":-6.0})", "re", "radioBitErrorRate", 1e-6,
       1e-2, 1e-6, -4.0, AutomationValueTransform::log10},
      {"SimpleJitterPlugin", R"({"rj":100.0})", "rj", "rmsJitterNanoseconds", 0.001,
       1e7, 100.0, 100.0,
       AutomationValueTransform::decibelsFromReference}};

  for (std::size_t index = 0; index < cases.size(); ++index) {
    const auto &test = cases[index];
    PluginStateDocument document;
    auto transformed = plugin(static_cast<std::uint32_t>(100 + index), test.packedJson);
    transformed.extraJson = std::string(R"({"type":")") + test.type + R"("})";
    document.pipelineA.plugins = {std::move(transformed)};

    const auto targets = generatedAutomationTargets(document);
    expect(targets.size() == 1, std::string(test.type) + " exposes one supplied target");
    const auto &target = targets.front();
    expect(target.identity.parameterKey == test.key,
           std::string(test.type) + " uses the stable generated key identity");
    expect(target.title.find(test.publicName) != std::string::npos,
           std::string(test.type) + " reserves the public name for display");
    expect(target.transform == test.transform,
           std::string(test.type) + " preserves its packed transform");
    expectNear(target.minimum, test.minimum, test.maximum * 1e-6,
               std::string(test.type) + " exposes the public minimum");
    expectNear(target.maximum, test.maximum, test.maximum * 1e-6,
               std::string(test.type) + " exposes the public maximum");
    const auto expectedDefault = std::log(test.defaultValue / test.minimum) /
                                 std::log(test.maximum / test.minimum);
    expectNear(target.defaultNormalized, expectedDefault, 1e-6,
               std::string(test.type) + " normalizes its public default");
    const auto packed = denormalizeAutomationPackedValue(target, 0.5);
    expect(packed.has_value(), std::string(test.type) + " denormalizes to packed storage");
    expectNear(*packed, test.expectedPackedAtHalf, 1e-4,
               std::string(test.type) + " applies the inverse public transform");

    const AutomationBindingState binding{
        0, 'A', static_cast<std::uint32_t>(100 + index), test.type,
        test.key, 0, AutomationBindingLifecycle::active, "{}"};
    expect(applyAutomationNormalizedValue(document, binding, 0.5),
           std::string(test.type) + " writes a transformed packed value");
    const auto rewritten = choc::json::parse(document.pipelineA.plugins[0].parametersJson);
    const std::string_view packedKey = index == 0 ? "f0" : index == 1 ? "be" :
                                       index == 2 ? "re" : "rj";
    expectNear(rewritten[packedKey].get<double>(), test.expectedPackedAtHalf, 1e-4,
               std::string(test.type) + " keeps parametersJson in packed domain");
  }
}

#if defined(EFFETUNE_AUTOMATION_HOST_GATE_FIXTURE)
void testHostGateFixtureState() {
  auto document = automationHostGateFixtureDocument();
  auto targets = generatedAutomationTargets(document);
  expect(targets.size() == 3 && targets[0].identity.parameterKey == "of" &&
             targets[0].identity.pluginId == 1001 &&
             targets[1].identity.pluginId == 1002 &&
             targets[2].identity.pluginId == 1003 &&
             targets[0].currentNormalized == 0.375 &&
             targets[1].currentNormalized == 0.625 &&
             targets[2].currentNormalized == 0.75,
         "the host-gate state expands three logical DCOffset instances across A/B");

  AutomationBindingRegistry registry;
  for (const auto &fixtureTarget : targets) {
    expect(bindSlot(document, targets, fixtureTarget.identity).has_value(),
           "the host-gate state binds each fixture target on demand");
  }
  const auto initial = registry.reconcile(document, targets);
  expect(initial.stateChanged && initial.slotsChanged &&
             document.automation.logicalPluginIdWatermark == 1003 &&
             document.automation.bindings.size() == 3 &&
             registry.slot(0) != nullptr && registry.slot(1) != nullptr &&
             registry.slot(2) != nullptr,
         "the host-gate state activates three stable fixed-bank slots");

  const auto encoded = StateCodec::encode(document);
  PluginStateDocument restored;
  std::string error;
  expect(StateCodec::decode(encoded, restored, &error),
         "decode host-gate state fixture: " + error);
  targets = generatedAutomationTargets(restored);
  AutomationBindingRegistry restoredRegistry;
  (void)restoredRegistry.reconcile(restored, targets);
  expect(restored.automation.bindings.size() == 3 &&
             restored.automation.bindings[0].slot == 0 &&
             restored.automation.bindings[1].slot == 1 &&
             restored.automation.bindings[2].slot == 2,
         "the host-gate fixture restores the same ParamID mapping");
}
#endif

void testStateCodecUsesPipelineParametersAsValueAuthority() {
  PluginStateDocument original;
  original.pipelineA.plugins = {plugin(21, R"({"gain":0.8,"future":3})")};
  original.automation.initialized = true;
  original.automation.logicalPluginIdWatermark = 42;
  original.automation.extraJson = R"({"futureAutomation":{"v":1}})";
  original.automation.bindings = {
      {3, 'A', 21, "SyntheticPlugin", "gain", 0,
       AutomationBindingLifecycle::active,
       R"({"legacyNormalizedValue":0.1,"futureBinding":{"v":2}})"}};

  const auto json = StateCodec::encode(original);
  PluginStateDocument decoded;
  std::string error;
  expect(StateCodec::decode(json, decoded, &error), "automation state decode: " + error);
  expect(decoded.automation.initialized &&
             decoded.automation.logicalPluginIdWatermark == 42 &&
             decoded.automation.bindings.size() == 1,
         "additive automation state round trip");
  expect(decoded.pipelineA.plugins[0].parametersJson.find("0.8") != std::string::npos,
         "pipeline parameters remain the persistent scalar authority");
  expect(decoded.automation.bindings[0].extraJson.find("legacyNormalizedValue") !=
             std::string::npos,
         "legacy binding values are opaque and are not decoded as authority");
  const auto reencoded = StateCodec::encode(decoded);
  expect(reencoded.find("futureAutomation") != std::string::npos &&
             reencoded.find("futureBinding") != std::string::npos,
         "unknown automation and binding members survive round trip");

  AutomationBindingRegistry registry;
  const std::vector targets{target('A', 21, "gain", 0.8)};
  (void)registry.reconcile(decoded, targets);
  expect(registry.slot(3) != nullptr && registry.slot(3)->currentNormalized == 0.8,
         "restored runtime current is derived from the pipeline target, not binding JSON");
}

void testStateCodecRejectsBindingsOutsideTheFixedBank() {
  PluginStateDocument maximum;
  maximum.pipelineA.plugins = {plugin(21)};
  maximum.automation.initialized = true;
  maximum.automation.bindings.reserve(kAutomationSlotCount);
  for (std::uint32_t slot = 0; slot < kAutomationSlotCount; ++slot) {
    maximum.automation.bindings.push_back(
        {slot, 'A', 21, "SyntheticPlugin", "gain", slot,
         AutomationBindingLifecycle::tombstone, R"({"futureBinding":true})"});
  }

  PluginStateDocument decoded;
  std::string error;
  expect(StateCodec::decode(StateCodec::encode(maximum), decoded, &error) &&
             decoded.automation.bindings.size() == kAutomationSlotCount &&
             decoded.automation.bindings.back().extraJson.find("futureBinding") !=
                 std::string::npos,
         "the complete fixed bank and opaque binding fields round trip");

  auto tooMany = maximum;
  tooMany.automation.bindings.push_back(
      {0, 'A', 21, "SyntheticPlugin", "gain", 0,
       AutomationBindingLifecycle::dormant, "{}"});
  PluginStateDocument unchanged;
  unchanged.appVersion = "sentinel";
  unchanged.pipelineA.plugins = {plugin(99, R"({"sentinel":true})")};
  unchanged.automation.initialized = true;
  unchanged.automation.bindings = {
      {7, 'B', 99, "SentinelPlugin", "sentinel", 0,
       AutomationBindingLifecycle::active, R"({"sentinel":true})"}};
  const auto unchangedBefore = StateCodec::encode(unchanged);
  error.clear();
  expect(!StateCodec::decode(StateCodec::encode(tooMany), unchanged, &error) &&
             StateCodec::encode(unchanged) == unchangedBefore && !error.empty(),
         "a binding array larger than the fixed bank rejects the whole state");

  auto invalidSlot = maximum;
  invalidSlot.automation.bindings.back().slot = kAutomationSlotCount;
  error.clear();
  expect(!StateCodec::decode(StateCodec::encode(invalidSlot), unchanged, &error) &&
             StateCodec::encode(unchanged) == unchangedBefore && !error.empty(),
         "a slot outside the fixed bank rejects the whole state");
}

void testPresetMergeKeepsProjectBindingsAndAddsNewTargets() {
  PluginStateDocument document;
  document.pipelineA.plugins = {plugin(3), plugin(8)};
  document.automation.initialized = true;
  document.automation.logicalPluginIdWatermark = 8;
  document.automation.bindings = {
      {4, 'A', 3, "SyntheticPlugin", "gain", 0,
       AutomationBindingLifecycle::active, "{}"}};
  AutomationBindingRegistry registry;
  const std::vector targets{target('A', 3, "gain", 0.2),
                            target('A', 8, "mix", 0.7)};
  expect(bindSlot(document, targets, targets[0].identity) ==
             std::optional<std::uint32_t>{4u} &&
             bindSlot(document, targets, targets[1].identity) ==
                 std::optional<std::uint32_t>{0u},
         "an existing project binding is reused and the new target takes a free slot");
  (void)registry.reconcile(document, targets);
  expect(document.automation.bindings.size() == 2 &&
             document.automation.bindings[0].slot == 4 &&
             document.automation.bindings[1].slot == 0,
         "preset merge preserves project mapping and allocates only its new target");
}

void testLegacyIdAllocationStartsAfterAutomationWatermark() {
  const auto json = R"({
    "formatVersion":1,
    "pipelineA":[{"name":"Legacy","parameters":{"gain":0.5}}],
    "pipelineB":null,
    "automation":{"logicalPluginIdWatermark":41,"bindings":[{
      "slot":2,"pipeline":"A","pluginId":41,"pluginType":"FuturePlugin",
      "parameterKey":"future","elementIndex":0,"lifecycle":"dormant"
    }]}
  })";
  PluginStateDocument decoded;
  std::string error;
  expect(StateCodec::decode(json, decoded, &error), "legacy watermark decode: " + error);
  expect(decoded.pipelineA.plugins.size() == 1 &&
             decoded.pipelineA.plugins[0].id == 42,
         "legacy missing ID is allocated after every historical automation ID");
}

} // namespace

int main() {
  effetune::vst::testing::suppressCrtModalDialogs();
  try {
    testAllocationLifecycleAndWatermark();
    testBindingRejectsTargetsOutsideTheGeneratedCatalog();
    testDuplicateAndDormantBindings();
    testCapacityExhaustionPreservesExistingBindings();
    testGeneratedCatalogPassThrough();
    testSyntheticNodeEnableTargets();
    testGeneratedCatalogPublicPackedTransforms();
#if defined(EFFETUNE_AUTOMATION_HOST_GATE_FIXTURE)
    testHostGateFixtureState();
#endif
    testStateCodecUsesPipelineParametersAsValueAuthority();
    testStateCodecRejectsBindingsOutsideTheFixedBank();
    testPresetMergeKeepsProjectBindingsAndAddsNewTargets();
    testLegacyIdAllocationStartsAfterAutomationWatermark();
    std::cout << "EffeTune automation binding tests passed\n";
    return 0;
  } catch (const std::exception &exception) {
    std::cerr << "Test failure: " << exception.what() << '\n';
    return 1;
  }
}
