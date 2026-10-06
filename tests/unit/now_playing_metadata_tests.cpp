#include "plugin/plugin_processor.h"

#include "pluginterfaces/vst/ivstmessage.h"
#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "public.sdk/source/vst/utility/memoryibstream.h"
#include "choc/text/choc_JSON.h"

#include <array>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "../support/crt_dialog_suppression.h"

namespace {
using namespace Steinberg;
using namespace Steinberg::Vst;
using effetune::vst::plugin::EffeTuneProcessor;

class IncompleteMessage final : public U::Implements<U::Directly<IMessage>> {
public:
  explicit IncompleteMessage(const char *id) : id_(id) {}
  FIDString PLUGIN_API getMessageID() override { return id_; }
  void PLUGIN_API setMessageID(FIDString id) override { id_ = id; }
  IAttributeList *PLUGIN_API getAttributes() override { return nullptr; }

private:
  const char *id_;
};

void expect(const bool condition, const char *message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

IPtr<EffeTuneProcessor> makeProcessor() {
  auto processor = owned(new EffeTuneProcessor);
  expect(processor->initialize(nullptr) == kResultOk, "initialize metadata processor");
  return processor;
}

IPtr<HostMessage> makeMessage(const char *id = "EffeTune.NowPlaying") {
  auto message = owned(new HostMessage);
  message->setMessageID(id);
  return message;
}

choc::value::Value readSnapshot(EffeTuneProcessor &processor,
                               const std::string_view type = "host/getInfo",
                               const std::int64_t revision = 0) {
  return choc::json::parse(processor.handleUiMessage(
      "{\"type\":\"" + std::string(type) +
      "\",\"payload\":{\"nowPlayingRevision\":" + std::to_string(revision) + "}}"));
}

std::int64_t revisionOf(const choc::value::Value &response) {
  expect(response["ok"].getWithDefault<bool>(false), "metadata bridge request succeeds");
  return response["nowPlayingRevision"].getWithDefault<std::int64_t>(0);
}

std::string snapshotJson(EffeTuneProcessor &processor) {
  return choc::json::toString(readSnapshot(processor)["nowPlaying"]);
}

void expectRejected(EffeTuneProcessor &processor, IMessage *message,
                    const char *reason) {
  const auto before = readSnapshot(processor);
  expect(processor.notify(message) == kInvalidArgument, reason);
  const auto after = readSnapshot(processor);
  expect(revisionOf(after) == revisionOf(before), "rejection preserves revision");
  expect(choc::json::toString(after["nowPlaying"]) ==
             choc::json::toString(before["nowPlaying"]),
         "rejection preserves the complete previous snapshot");
}

void testInterfacesAndOrdinaryHost() {
  auto processor = makeProcessor();
  FUnknownPtr<IComponent> component(static_cast<IComponent *>(processor.get()));
  FUnknownPtr<IEditController> controller(static_cast<IComponent *>(processor.get()));
  expect(component != nullptr && controller != nullptr,
         "single component exposes both processor and controller");
  FUnknownPtr<IConnectionPoint> fromComponent(component.getInterface());
  FUnknownPtr<IConnectionPoint> fromController(controller.getInterface());
  expect(fromComponent != nullptr && fromController != nullptr,
         "host can discover metadata connection from both interfaces");
  FUnknownPtr<FUnknown> componentIdentity(component.getInterface());
  FUnknownPtr<FUnknown> controllerIdentity(controller.getInterface());
  FUnknownPtr<FUnknown> connectionIdentity(fromComponent.getInterface());
  expect(componentIdentity.getInterface() == controllerIdentity.getInterface() &&
             componentIdentity.getInterface() == connectionIdentity.getInterface(),
         "metadata interface preserves the single component identity");
  TUID controllerClass{};
  expect(component->getControllerClassId(controllerClass) == kNotImplemented,
         "no separate controller class is introduced");
  const auto initial = readSnapshot(*processor);
  expect(revisionOf(initial) > 0 && initial["nowPlaying"].isVoid(),
         "ordinary hosts start with an empty metadata snapshot");
  auto unknown = makeMessage("Unrelated.Host.Message");
  expect(fromComponent->notify(unknown.get()) == kResultFalse,
         "unknown messages retain the inherited response");
  auto text = makeMessage("TextMessage");
  text->getAttributes()->setString("Text", STR16("SDK base notification"));
  expect(fromComponent->notify(text.get()) == kResultOk,
         "inherited SDK text notifications still work");
  expect(revisionOf(readSnapshot(*processor)) == revisionOf(initial),
         "unrelated notifications leave metadata untouched");
  expectRejected(*processor, nullptr, "null message is rejected");
  auto missingId = owned(new IncompleteMessage(nullptr));
  expectRejected(*processor, missingId.get(), "missing message ID is rejected");
  auto missingAttributes = owned(new IncompleteMessage("EffeTune.NowPlaying"));
  expect(processor->notify(missingAttributes.get()) == kResultOk &&
             readSnapshot(*processor)["nowPlaying"].isVoid(),
         "recognized message without an attribute list clears metadata");
  auto peer = makeProcessor();
  FUnknownPtr<IConnectionPoint> peerConnection(static_cast<IComponent *>(peer.get()));
  expect(fromComponent->connect(nullptr) == kInvalidArgument,
         "inherited null connection rejection survives");
  expect(fromComponent->connect(peerConnection) == kResultOk,
         "inherited connection still accepts a peer");
  expect(fromComponent->connect(peerConnection) == kResultFalse,
         "inherited connection still refuses a second peer");
  expect(fromComponent->disconnect(peerConnection) == kResultOk,
         "inherited disconnect still releases the peer");
  expect(fromComponent->disconnect(peerConnection) == kResultFalse,
         "inherited disconnect still reports an absent peer");
  expect(peer->terminate() == kResultOk, "terminate peer");
  expect(processor->terminate() == kResultOk, "terminate ordinary host processor");
}

void testReplacementOwnershipAndPolling() {
  auto processor = makeProcessor();
  auto isolated = makeProcessor();
  {
    auto message = makeMessage();
    auto *attributes = message->getAttributes();
    attributes->setString("title", STR16("日本語 \U0001f3b5 <Title>"));
    attributes->setString("album", STR16("Album & \"quoted\""));
    attributes->setString("artist", STR16("Artist"));
    attributes->setString("artworkMimeType", STR16("image/png"));
    const std::array<std::uint8_t, 3> bytes{1, 2, 3};
    attributes->setBinary("artwork", bytes.data(), static_cast<uint32>(bytes.size()));
    expect(processor->notify(message.get()) == kResultOk, "accept complete snapshot");
    attributes->setString("title", STR16("Mutated host message"));
  }
  const auto snapshot = readSnapshot(*processor);
  const auto metadata = snapshot["nowPlaying"];
  expect(metadata["title"].getWithDefault<std::string>({}) == "日本語 🎵 <Title>",
         "UTF16 text is copied and converted without markup interpretation");
  expect(metadata["album"].getWithDefault<std::string>({}) == "Album & \"quoted\"" &&
             metadata["artist"].getWithDefault<std::string>({}) == "Artist",
         "all text fields survive host message destruction");
  expect(metadata["artwork"][0]["src"].getWithDefault<std::string>({}) ==
             "data:image/png;base64,AQID", "owned binary reaches the UI data URL");
  expect(readSnapshot(*isolated)["nowPlaying"].isVoid(), "metadata is instance-local");
  expect(snapshotJson(*processor) == choc::json::toString(metadata),
         "a startup or reopened editor receives the cached snapshot again");
  const auto matched = readSnapshot(*processor, "telemetry/read", revisionOf(snapshot));
  expect(revisionOf(matched) == revisionOf(snapshot) &&
             !matched.hasObjectMember("nowPlaying"),
         "unchanged telemetry omits the artwork payload");
  const auto replay = readSnapshot(*processor, "telemetry/read");
  expect(choc::json::toString(replay["nowPlaying"]) == choc::json::toString(metadata),
         "another reader can replay metadata without a global drain");

  ResizableMemoryIBStream state;
  expect(processor->getState(&state) == kResultOk, "save ordinary VST state");
  const std::string encodedState(static_cast<const char *>(state.getData()), state.getCursor());
  expect(encodedState.find("nowPlaying") == std::string::npos &&
             encodedState.find("data:image/png") == std::string::npos &&
             encodedState.find("<Title>") == std::string::npos,
         "transient host metadata is excluded from serialized state");
  state.rewind();
  expect(processor->setState(&state) == kResultOk, "restore ordinary VST state");
  expect(snapshotJson(*processor) == choc::json::toString(metadata),
         "pipeline state restoration preserves the current host snapshot");

  auto replacement = makeMessage();
  replacement->getAttributes()->setString("title", STR16("Next track"));
  expect(processor->notify(replacement.get()) == kResultOk, "accept title-only replacement");
  const auto next = readSnapshot(*processor);
  expect(revisionOf(next) > revisionOf(snapshot) &&
             next["nowPlaying"]["title"].getWithDefault<std::string>({}) == "Next track" &&
             next["nowPlaying"]["album"].getWithDefault<std::string>({}).empty() &&
             next["nowPlaying"]["artist"].getWithDefault<std::string>({}).empty() &&
             next["nowPlaying"]["artwork"].size() == 0,
         "full replacement clears every omitted field");
  auto clear = makeMessage();
  expect(processor->notify(clear.get()) == kResultOk, "empty attributes clear metadata");
  expect(readSnapshot(*processor)["nowPlaying"].isVoid(), "clear reaches the renderer");
  expect(processor->notify(replacement.get()) == kResultOk, "restore metadata before null-list clear");
  auto missingAttributes = owned(new IncompleteMessage("EffeTune.NowPlaying"));
  expect(processor->notify(missingAttributes.get()) == kResultOk &&
             readSnapshot(*processor)["nowPlaying"].isVoid(),
         "absent attribute list clears an existing snapshot");
  expect(isolated->terminate() == kResultOk, "terminate isolated processor");
  expect(processor->terminate() == kResultOk, "terminate snapshot processor");
}

void testAtomicTransportValidation() {
  auto processor = makeProcessor();
  auto baseline = makeMessage();
  baseline->getAttributes()->setString("title", STR16("Keep this track"));
  expect(processor->notify(baseline.get()) == kResultOk, "install rejection baseline");
  auto invalid = makeMessage();
  invalid->getAttributes()->setString("title", STR16("Must not replace baseline"));
  const std::array<std::uint8_t, 3> bytes{1, 2, 3};
  invalid->getAttributes()->setBinary("artwork", bytes.data(), static_cast<uint32>(bytes.size()));
  expectRejected(*processor, invalid.get(), "artwork without MIME is rejected");
  invalid->getAttributes()->setString("artworkMimeType", STR16("image/svg+xml"));
  expectRejected(*processor, invalid.get(), "unsupported artwork MIME is rejected");
  invalid->getAttributes()->setString("artworkMimeType", STR16("image/png"));
  const std::vector<std::uint8_t> tooLarge(8 * 1024 * 1024 + 1, 0);
  invalid->getAttributes()->setBinary("artwork", tooLarge.data(),
                                      static_cast<uint32>(tooLarge.size()));
  expectRejected(*processor, invalid.get(), "oversized artwork is rejected atomically");
  auto text = makeMessage();
  const std::u16string maximum(4096, u'x');
  text->getAttributes()->setString("title", maximum.c_str());
  expect(processor->notify(text.get()) == kResultOk, "4096 text code units are accepted");
  const std::u16string tooLong(4097, u'x');
  text->getAttributes()->setString("title", tooLong.c_str());
  expectRejected(*processor, text.get(), "text beyond 4096 code units is rejected");
  const std::array<TChar, 2> unpairedSurrogate{static_cast<TChar>(0xd800), 0};
  text->getAttributes()->setString("title", unpairedSurrogate.data());
  expectRejected(*processor, text.get(), "invalid UTF16 is rejected");
  text->getAttributes()->setInt("title", 123);
  expectRejected(*processor, text.get(), "incorrect known attribute type is rejected");
  for (const auto *mime : {STR16("image/png"), STR16("image/jpeg"),
                           STR16("image/gif"), STR16("image/webp")}) {
    auto image = makeMessage();
    image->getAttributes()->setString("artworkMimeType", mime);
    image->getAttributes()->setBinary("artwork", bytes.data(), static_cast<uint32>(bytes.size()));
    expect(processor->notify(image.get()) == kResultOk,
           "supported MIME accepts opaque binary without native image decoding");
  }
  expect(processor->terminate() == kResultOk, "terminate validation processor");
  expect(readSnapshot(*processor)["nowPlaying"].isVoid(), "terminate clears the transient snapshot");
}
} // namespace

int main() {
  effetune::vst::testing::suppressCrtModalDialogs();
  try {
    testInterfacesAndOrdinaryHost();
    testReplacementOwnershipAndPolling();
    testAtomicTransportValidation();
    std::cout << "EffeTune Now Playing metadata tests passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "Test failure: " << error.what() << '\n';
    return 1;
  }
}
