#include "engine/automation_binding.h"
#include "plugin/automation_parameters.h"
#include "plugin/plugin_ids.h"

#include "pluginterfaces/base/ustring.h"

#include <array>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "../support/crt_dialog_suppression.h"

namespace {

using namespace Steinberg::Vst;
using namespace effetune::vst;
using namespace effetune::vst::plugin;

void expect(const bool condition, const std::string &message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

[[nodiscard]] std::string asciiTitle(ParameterInfo &info) {
  std::array<char, 128> buffer{};
  Steinberg::UString(info.title, 128).toAscii(buffer.data(),
                                              static_cast<Steinberg::int32>(buffer.size()));
  return buffer.data();
}

[[nodiscard]] bool automatableOnly(const ParameterInfo &info) {
  return (info.flags & ParameterInfo::kCanAutomate) != 0 &&
         (info.flags & (ParameterInfo::kIsHidden | ParameterInfo::kIsReadOnly)) == 0;
}

void testUnboundSlotsAreAlwaysAutomatable() {
  ParameterContainer parameters;
  AutomationParameterBank bank;
  expect(bank.registerParameters(parameters), "register fixed automation parameter bank");
  expect(parameters.getParameterCount() ==
             static_cast<Steinberg::int32>(kAutomationSlotCount),
         "register exactly one parameter per automation slot");
  for (std::uint32_t slot = 0; slot < kAutomationSlotCount; ++slot) {
    auto *parameter = parameters.getParameter(automationParameterId(slot));
    expect(parameter != nullptr && automatableOnly(parameter->getInfo()),
           "every unbound slot is automatable and neither hidden nor read-only");
  }
  expect(asciiTitle(parameters.getParameter(kFirstAutomationParameterId)->getInfo()) ==
             "Automation 001" &&
             asciiTitle(parameters.getParameter(kLastAutomationParameterId)->getInfo()) ==
                 "Automation 256",
         "unbound slots carry a zero-padded placeholder name");
}

void testFixedBankAndDynamicMetadata() {
  ParameterContainer parameters;
  parameters.addParameter(STR16("Bypass"), nullptr, 1, 0.0,
                          ParameterInfo::kCanAutomate | ParameterInfo::kIsBypass,
                          kBypassParameterId);
  AutomationParameterBank bank;
  expect(bank.registerParameters(parameters), "register fixed automation parameter bank");
  expect(parameters.getParameterCount() ==
                 static_cast<Steinberg::int32>(kAutomationSlotCount + 1u) &&
             parameters.getParameter(kBypassParameterId) != nullptr,
         "preserve bypass ID zero and register exactly 256 automation slots");
  auto *first = parameters.getParameter(kFirstAutomationParameterId);
  auto *last = parameters.getParameter(kLastAutomationParameterId);
  expect(first != nullptr && last != nullptr && automatableOnly(first->getInfo()) &&
             automatableOnly(last->getInfo()),
         "fixed range endpoints start automatable");

  PluginStateDocument document;
  document.pipelineA.plugins = {
      PluginState{7, "Synthetic", true, 0, 0, std::nullopt, R"({"gain":0.75})",
                  false, R"({"type":"SyntheticPlugin"})"}};
  AutomationTargetDescriptor descriptor;
  descriptor.identity = {'A', 7, "SyntheticPlugin", "gain", 0};
  descriptor.title = "A - Synthetic - Gain";
  descriptor.shortTitle = "Gain";
  descriptor.units = "dB";
  descriptor.stepCount = 0;
  descriptor.defaultNormalized = 0.25;
  descriptor.currentNormalized = 0.75;

  AutomationBindingRegistry registry;
  const std::vector targets{descriptor};
  auto capacityExhausted = false;
  expect(bindAutomationTarget(document, targets, descriptor.identity,
                              capacityExhausted) == std::optional<std::uint32_t>{0u} &&
             !capacityExhausted,
         "bind the synthetic target to the first slot");
  (void)registry.reconcile(document, targets);
  expect(bank.apply(registry), "publish active synthetic metadata");
  const auto &active = first->getInfo();
  expect(automatableOnly(active) && active.defaultNormalizedValue == 0.25 &&
             first->getNormalized() == 0.75,
         "active slot exposes generated metadata and parametersJson-derived current");
  expect(asciiTitle(first->getInfo()) == descriptor.title,
         "binding replaces the placeholder name with the catalog name");

  registry.setCurrentNormalized(0, 2.0);
  bank.setHostAdoptedValue(0, 2.0);
  expect(first->getNormalized() == 1.0,
         "host-adopted values are clamped in the exposed parameter");
  expect(!bank.apply(registry),
         "host-adopted current stays synchronized with unchanged slot metadata");

  document.pipelineA.plugins.clear();
  (void)registry.reconcile(document, {});
  expect(bank.apply(registry), "retire dormant synthetic metadata");
  expect(automatableOnly(first->getInfo()) &&
             asciiTitle(first->getInfo()) == "Automation 001" &&
             first->getNormalized() == 0.0 &&
             parameters.getParameterCount() ==
                 static_cast<Steinberg::int32>(kAutomationSlotCount + 1u),
         "an unbound slot returns to its placeholder name and stays automatable");
}

// EditController::terminate() destroys every Parameter the container owns while
// the binding registry survives on the processor. A host that terminates and
// re-initializes the same instance must still see the catalog title and the
// bound current value on the freshly created parameters.
void testActiveMetadataIsRepublishedAfterReinitialize() {
  ParameterContainer parameters;
  parameters.addParameter(STR16("Bypass"), nullptr, 1, 0.0,
                          ParameterInfo::kCanAutomate | ParameterInfo::kIsBypass,
                          kBypassParameterId);
  AutomationParameterBank bank;
  expect(bank.registerParameters(parameters), "register fixed automation parameter bank");

  PluginStateDocument document;
  document.pipelineA.plugins = {
      PluginState{9, "Synthetic", true, 0, 0, std::nullopt, R"({"gain":0.5})",
                  false, R"({"type":"SyntheticPlugin"})"}};
  AutomationTargetDescriptor descriptor;
  descriptor.identity = {'A', 9, "SyntheticPlugin", "gain", 0};
  descriptor.title = "A - Synthetic - Gain";
  descriptor.shortTitle = "Gain";
  descriptor.defaultNormalized = 0.25;
  descriptor.currentNormalized = 0.75;

  AutomationBindingRegistry registry;
  const std::vector targets{descriptor};
  auto capacityExhausted = false;
  expect(bindAutomationTarget(document, targets, descriptor.identity,
                              capacityExhausted) == std::optional<std::uint32_t>{0u} &&
             !capacityExhausted,
         "bind the synthetic target to the first slot");
  (void)registry.reconcile(document, targets);
  expect(bank.apply(registry), "publish active synthetic metadata");
  expect(asciiTitle(parameters.getParameter(kFirstAutomationParameterId)->getInfo()) ==
             descriptor.title,
         "the bound slot carries the catalog name before terminate");

  // terminate() -> initialize(): EditController::terminate() clears the
  // container, then initialize() re-adds bypass and the fixed slot bank.
  parameters.removeAll();
  parameters.addParameter(STR16("Bypass"), nullptr, 1, 0.0,
                          ParameterInfo::kCanAutomate | ParameterInfo::kIsBypass,
                          kBypassParameterId);
  expect(bank.registerParameters(parameters),
         "re-register the fixed automation parameter bank after terminate");
  auto *first = parameters.getParameter(kFirstAutomationParameterId);
  expect(first != nullptr && asciiTitle(first->getInfo()) == "Automation 001" &&
             first->getNormalized() == 0.0,
         "re-registered slots start as unbound placeholders");

  (void)registry.reconcile(document, targets);
  expect(bank.apply(registry), "republish active metadata onto the recreated parameters");
  expect(asciiTitle(first->getInfo()) == descriptor.title &&
             first->getInfo().defaultNormalizedValue == 0.25 &&
             first->getNormalized() == 0.75 && automatableOnly(first->getInfo()),
         "a re-initialized instance re-exposes the catalog name and the bound current value");
}

// The descriptor carries the live value alongside the metadata, so comparing
// whole descriptors made a knob movement look like a metadata change. The
// processor answers a metadata change with
// restartComponent(kParamTitlesChanged | kParamValuesChanged), which invalidates
// every one of the 257 published parameters -- once per moved value, for a
// change no host caches anything about.
void testValueOnlyChangeIsNotAMetadataChange() {
  ParameterContainer parameters;
  AutomationParameterBank bank;
  expect(bank.registerParameters(parameters), "register fixed automation parameter bank");

  PluginStateDocument document;
  document.pipelineA.plugins = {
      PluginState{11, "Synthetic", true, 0, 0, std::nullopt, R"({"gain":0.5})",
                  false, R"({"type":"SyntheticPlugin"})"}};
  AutomationTargetDescriptor descriptor;
  descriptor.identity = {'A', 11, "SyntheticPlugin", "gain", 0};
  descriptor.title = "A - Synthetic - Gain";
  descriptor.shortTitle = "Gain";
  descriptor.defaultNormalized = 0.25;
  descriptor.currentNormalized = 0.75;

  AutomationBindingRegistry registry;
  auto capacityExhausted = false;
  expect(bindAutomationTarget(document, std::vector{descriptor}, descriptor.identity,
                              capacityExhausted) == std::optional<std::uint32_t>{0u} &&
             !capacityExhausted,
         "bind the value-only target to the first slot");
  (void)registry.reconcile(document, std::vector{descriptor});
  expect(bank.apply(registry),
         "the first projection of a bound slot is a metadata change");
  auto *first = parameters.getParameter(kFirstAutomationParameterId);
  expect(first != nullptr && first->getNormalized() == 0.75,
         "and it publishes the bound value");

  auto moved = descriptor;
  moved.currentNormalized = 0.3;
  (void)registry.reconcile(document, std::vector{moved});
  expect(!bank.apply(registry),
         "a slot whose value alone moved is not reported as a metadata change");
  expect(first->getNormalized() == 0.3,
         "and the moved value is still published to the host parameter");

  // Every field a host does cache still is one. The comparison is written
  // against a copy carrying the cached value rather than as a list of members,
  // so a field added to the descriptor later stays inside it by default.
  auto renamed = moved;
  renamed.title = "A - Synthetic - Output";
  (void)registry.reconcile(document, std::vector{renamed});
  expect(bank.apply(registry), "a title the host caches is a metadata change");
  expect(asciiTitle(first->getInfo()) == renamed.title, "and it is republished");

  auto restepped = renamed;
  restepped.stepCount = 4;
  (void)registry.reconcile(document, std::vector{restepped});
  expect(bank.apply(registry), "so is a step count");
  expect(first->getInfo().stepCount == 4, "and it is republished too");

  auto redefaulted = restepped;
  redefaulted.defaultNormalized = 0.5;
  (void)registry.reconcile(document, std::vector{redefaulted});
  expect(bank.apply(registry), "so is a default value");
  expect(first->getInfo().defaultNormalizedValue == 0.5, "and it is republished too");
}

// An enumeration-backed slot has named positions and nothing in between, so a
// host must be told to draw it as a list rather than as a continuous control:
// kIsList is "Parameter should be displayed as list in generic editor or
// automation editing". Nothing else earns the flag -- an integer slot is a
// number a user may usefully type or sweep, and a continuous one certainly is.
void testEnumerationSlotsArePublishedAsLists() {
  ParameterContainer parameters;
  AutomationParameterBank bank;
  expect(bank.registerParameters(parameters), "register fixed automation parameter bank");

  PluginStateDocument document;
  document.pipelineA.plugins = {
      PluginState{13, "Synthetic", true, 0, 0, std::nullopt,
                  R"({"mode":"lowpass","gain":0.5,"taps":8})", false,
                  R"({"type":"SyntheticPlugin"})"}};

  AutomationTargetDescriptor enumerated;
  enumerated.identity = {'A', 13, "SyntheticPlugin", "mode", 0};
  enumerated.title = "A - Synthetic - Mode";
  enumerated.shortTitle = "Mode";
  enumerated.stepCount = 2;
  enumerated.normalization = AutomationValueNormalization::enumeration;
  enumerated.continuous = false;

  AutomationTargetDescriptor stepped;
  stepped.identity = {'A', 13, "SyntheticPlugin", "taps", 0};
  stepped.title = "A - Synthetic - Taps";
  stepped.shortTitle = "Taps";
  stepped.stepCount = 16;
  stepped.normalization = AutomationValueNormalization::integer;
  stepped.continuous = false;

  AutomationTargetDescriptor continuous;
  continuous.identity = {'A', 13, "SyntheticPlugin", "gain", 0};
  continuous.title = "A - Synthetic - Gain";
  continuous.shortTitle = "Gain";
  continuous.units = "dB";

  const std::vector targets{enumerated, stepped, continuous};
  auto capacityExhausted = false;
  for (const auto &target : targets) {
    expect(bindAutomationTarget(document, targets, target.identity, capacityExhausted)
               .has_value() &&
               !capacityExhausted,
           "bind every synthetic display target");
  }
  AutomationBindingRegistry registry;
  (void)registry.reconcile(document, targets);
  expect(bank.apply(registry), "publish the synthetic display metadata");

  const auto flagsFor = [&](const AutomationTargetIdentity &identity) {
    const auto slot = registry.findActiveSlot(identity);
    expect(slot.has_value(), "the display target holds a slot");
    auto *parameter = parameters.getParameter(automationParameterId(*slot));
    expect(parameter != nullptr, "the slot has a published parameter");
    return parameter->getInfo().flags;
  };
  expect((flagsFor(enumerated.identity) & ParameterInfo::kIsList) != 0,
         "an enumeration-backed slot is published as a list");
  expect((flagsFor(enumerated.identity) & ParameterInfo::kCanAutomate) != 0,
         "and stays automatable alongside it");
  expect((flagsFor(stepped.identity) & ParameterInfo::kIsList) == 0,
         "an integer slot is a number, not a list");
  expect((flagsFor(continuous.identity) & ParameterInfo::kIsList) == 0,
         "and neither is a continuous one");

  // Retiring the binding has to take the flag with it, or the placeholder that
  // replaces it would keep being drawn as a list of positions it does not have.
  document.pipelineA.plugins.clear();
  (void)registry.reconcile(document, {});
  expect(bank.apply(registry), "retire the synthetic display metadata");
  auto *first = parameters.getParameter(kFirstAutomationParameterId);
  expect(first != nullptr && (first->getInfo().flags & ParameterInfo::kIsList) == 0 &&
             automatableOnly(first->getInfo()),
         "an unbound slot carries no list flag");
}

void testGeneratedSteppedIntegerMetadata() {
  ParameterContainer parameters;
  AutomationParameterBank bank;
  expect(bank.registerParameters(parameters), "register the stepped integer parameter bank");
  PluginStateDocument document;
  document.pipelineA.plugins = {
      PluginState{81, "Phaser", true, 0, 0, std::nullopt, R"({"st":12})", false,
                  R"({"type":"PhaserPlugin"})"}};
  const auto targets = generatedAutomationTargets(document);
  expect(targets.size() == 1, "resolve the real Phaser stages descriptor");
  auto capacityExhausted = false;
  const auto slot = bindAutomationTarget(document, targets, targets[0].identity,
                                         capacityExhausted);
  expect(slot.has_value() && !capacityExhausted, "bind Phaser stages");
  AutomationBindingRegistry registry;
  (void)registry.reconcile(document, targets);
  expect(bank.apply(registry), "publish Phaser stages metadata");
  const auto *parameter = parameters.getParameter(automationParameterId(*slot));
  expect(parameter != nullptr && parameter->getInfo().stepCount == 5 &&
             parameter->getInfo().defaultNormalizedValue == 0.4 &&
             parameter->getNormalized() == 1.0 &&
             (parameter->getInfo().flags & ParameterInfo::kIsList) == 0,
         "six integer stage choices publish five steps, default six and current twelve");
}

} // namespace

int main() {
  effetune::vst::testing::suppressCrtModalDialogs();
  try {
    testUnboundSlotsAreAlwaysAutomatable();
    testFixedBankAndDynamicMetadata();
    testActiveMetadataIsRepublishedAfterReinitialize();
    testValueOnlyChangeIsNotAMetadataChange();
    testEnumerationSlotsArePublishedAsLists();
    testGeneratedSteppedIntegerMetadata();
    std::cout << "EffeTune automation parameter bank tests passed\n";
    return 0;
  } catch (const std::exception &exception) {
    std::cerr << "Test failure: " << exception.what() << '\n';
    return 1;
  }
}
