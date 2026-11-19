#include "plugin/plugin_processor.h"

#include "pluginterfaces/vst/ivstmessage.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "public.sdk/source/vst/utility/memoryibstream.h"
#include "choc/text/choc_JSON.h"

#include <array>
#include <iostream>
#include <limits>
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

choc::value::Value readSnapshots(EffeTuneProcessor &processor,
                                const std::string_view type = "host/getInfo",
                                const std::string_view payload =
                                    R"({"nowPlayingRevision":0,"lyricsRevision":0})") {
  const auto response = choc::json::parse(processor.handleUiMessage(
      "{\"type\":\"" + std::string(type) + "\",\"payload\":" + std::string(payload) + "}"));
  expect(response["ok"].getWithDefault<bool>(false), "snapshot bridge request succeeds");
  return response;
}

std::int64_t lyricsRevisionOf(const choc::value::Value &response) {
  return response["lyricsRevision"].getWithDefault<std::int64_t>(0);
}

IPtr<HostMessage> makeLyricsMessage(const std::string_view text,
                                   const TChar *format = STR16("text")) {
  auto message = makeMessage("EffeTune.NowPlayingLyrics");
  message->getAttributes()->setBinary("lyrics", text.data(), static_cast<uint32>(text.size()));
  if (format != nullptr) {
    message->getAttributes()->setString("format", format);
  }
  return message;
}

void expectLyricsRejected(EffeTuneProcessor &processor, IMessage *message,
                          const char *reason) {
  const auto before = readSnapshots(processor);
  expect(processor.notify(message) == kInvalidArgument, reason);
  const auto after = readSnapshots(processor);
  expect(lyricsRevisionOf(after) == lyricsRevisionOf(before) &&
             revisionOf(after) == revisionOf(before), "rejection preserves both revisions");
  expect(choc::json::toString(after["lyrics"]) == choc::json::toString(before["lyrics"]) &&
             choc::json::toString(after["nowPlaying"]) == choc::json::toString(before["nowPlaying"]),
         "rejection preserves both complete snapshots");
}

void testLyricsReplacementAndLifetime() {
  auto processor = makeProcessor();
  auto isolated = makeProcessor();
  const auto initial = readSnapshots(*processor);
  expect(lyricsRevisionOf(initial) > 0 && initial["lyrics"].isVoid(),
         "lyrics start empty with a nonzero revision");
  const std::array<std::string_view, 3> texts{
      "日本語 🎵\t[Chorus: Artist]\r\n\nLast line\r", "[00:12.50]<00:12.50>First line",
      R"({"synced":true,"lines":[{"time":12.5,"text":"First line"}],"future":true})"};
  const std::array<const TChar *, 3> formats{STR16("text"), STR16("lrc"), STR16("json")};
  for (std::size_t index = 0; index < formats.size(); ++index) {
    const auto before = readSnapshots(*processor);
    {
      auto message = makeLyricsMessage(texts[index], formats[index]);
      message->getAttributes()->setFloat("timeOffset", -12.5);
      message->getAttributes()->setFloat("duration", 180.0);
      message->getAttributes()->setInt("future", 42);
      expect(processor->notify(message.get()) == kResultOk, "accept each lyrics format");
      message->getAttributes()->setBinary("lyrics", "changed", 7);
    }
    const auto accepted = readSnapshots(*processor);
    expect(lyricsRevisionOf(accepted) > lyricsRevisionOf(before) &&
               accepted["lyrics"]["text"].getString() == texts[index] &&
               accepted["lyrics"]["timeOffset"].get<double>() == -12.5 &&
               accepted["lyrics"]["duration"].get<double>() == 180.0,
           "lyrics bytes and numbers are owned and round-trip unchanged");
    const std::array<std::string_view, 3> labels{"text", "lrc", "json"};
    expect(accepted["lyrics"]["format"].getString() == labels[index], "format round-trips");
    expect(readSnapshots(*isolated)["lyrics"].isVoid(), "lyrics are instance-local");
    expect(choc::json::toString(readSnapshots(*processor, "telemetry/read")["lyrics"]) ==
               choc::json::toString(accepted["lyrics"]), "fresh readers replay the owned lyrics");

    ResizableMemoryIBStream state;
    expect(processor->getState(&state) == kResultOk, "save state with lyrics loaded");
    const std::string encoded(static_cast<const char *>(state.getData()), state.getCursor());
    expect(encoded.find("lyrics") == std::string::npos, "lyrics are not persisted in state");
    state.rewind();
    expect(processor->setState(&state) == kResultOk &&
               choc::json::toString(readSnapshots(*processor)["lyrics"]) ==
                   choc::json::toString(accepted["lyrics"]), "state reload preserves lyrics");

    const auto bomText = std::string("\xef\xbb\xbf") + std::string(texts[index]);
    auto replacement = makeLyricsMessage(bomText, formats[index]);
    expect(processor->notify(replacement.get()) == kResultOk, "accept leading BOM for every format");
    const auto replaced = readSnapshots(*processor);
    expect(lyricsRevisionOf(replaced) > lyricsRevisionOf(accepted) &&
               replaced["lyrics"]["text"].getString() == bomText &&
               replaced["lyrics"]["timeOffset"].get<double>() == 0.0 &&
               replaced["lyrics"]["duration"].get<double>() == 0.0,
           "replacement resets omitted fields and preserves BOM for shim stripping");
    auto clear = makeLyricsMessage("", formats[index]);
    expect(processor->notify(clear.get()) == kResultOk, "empty binary clears each format");
    const auto cleared = readSnapshots(*processor);
    expect(cleared["lyrics"].isVoid() && lyricsRevisionOf(cleared) > lyricsRevisionOf(replaced),
           "clear publishes null lyrics and advances revision");
  }
  for (const auto *opaque : {"{broken", "[bad LRC"}) {
    auto message = makeLyricsMessage(opaque, STR16("json"));
    expect(processor->notify(message.get()) == kResultOk,
           "native validation accepts opaque payloads without parsing JSON or LRC");
  }
  auto clear = makeMessage("EffeTune.NowPlayingLyrics");
  expect(processor->notify(clear.get()) == kResultOk &&
             readSnapshots(*processor)["lyrics"].isVoid(), "empty attribute list clears lyrics");
  auto baseline = makeLyricsMessage("keep lyrics");
  expect(processor->notify(baseline.get()) == kResultOk, "restore lyrics before absent-list clear");
  auto noAttributes = owned(new IncompleteMessage("EffeTune.NowPlayingLyrics"));
  expect(processor->notify(noAttributes.get()) == kResultOk &&
             readSnapshots(*processor)["lyrics"].isVoid(), "absent attribute list clears lyrics");
  expect(processor->notify(baseline.get()) == kResultOk, "restore lyrics before termination");
  const auto beforeTermination = readSnapshots(*processor);
  expect(processor->terminate() == kResultOk, "terminate lyrics processor");
  const auto terminated = readSnapshots(*processor);
  expect(terminated["lyrics"].isVoid() &&
             lyricsRevisionOf(terminated) > lyricsRevisionOf(beforeTermination),
         "termination clears lyrics and advances revision");
  expect(isolated->terminate() == kResultOk, "terminate isolated lyrics processor");
}

void testLyricsValidation() {
  auto processor = makeProcessor();
  auto baseline = makeLyricsMessage("keep lyrics");
  expect(processor->notify(baseline.get()) == kResultOk, "install lyrics rejection baseline");
  for (const auto &invalid : std::vector<std::string>{
           "\x80", "\xe2\x82", "\xc0\xaf", "\xe0\x80\xaf", "\xed\xa0\x80",
           "\xf0\x80\x80\xaf", "\xf4\x90\x80\x80", "\xe2\x28\xa1",
           std::string("a\0b", 3), "a\ab"}) {
    auto message = makeLyricsMessage(invalid);
    expectLyricsRejected(*processor, message.get(), "invalid UTF-8 or forbidden control is rejected");
  }
  auto missingFormat = makeLyricsMessage("payload", nullptr);
  expectLyricsRejected(*processor, missingFormat.get(), "nonempty lyrics require a format");
  auto unknownFormat = makeLyricsMessage("payload", STR16("ttml"));
  expectLyricsRejected(*processor, unknownFormat.get(), "unknown format is rejected");
  for (const auto *key : {"lyrics", "format", "timeOffset", "duration"}) {
    for (const auto type : {0, 1, 2}) {
      auto message = makeLyricsMessage("payload");
      auto *attributes = message->getAttributes();
      if (type == 0) {
        attributes->setInt(key, 1);
      } else if (type == 1) {
        if (std::string_view(key) == "format") attributes->setFloat(key, 1.0);
        else attributes->setString(key, STR16("wrong type"));
      } else {
        if (std::string_view(key) == "lyrics") attributes->setFloat(key, 1.0);
        else attributes->setBinary(key, "x", 1);
      }
      expectLyricsRejected(*processor, message.get(), "known attribute with a wrong type is rejected");
    }
  }
  for (const auto *key : {"timeOffset", "duration"}) {
    for (const auto value : {std::numeric_limits<double>::infinity(),
                             -std::numeric_limits<double>::infinity(),
                             std::numeric_limits<double>::quiet_NaN()}) {
      auto message = makeLyricsMessage("payload");
      message->getAttributes()->setFloat(key, value);
      expectLyricsRejected(*processor, message.get(), "non-finite numbers are rejected");
    }
  }
  auto negativeDuration = makeLyricsMessage("payload");
  negativeDuration->getAttributes()->setFloat("duration", -1.0);
  expectLyricsRejected(*processor, negativeDuration.get(), "negative duration is rejected");
  const std::string maximum(3145728, 'x');
  auto maximumMessage = makeLyricsMessage(maximum);
  expect(processor->notify(maximumMessage.get()) == kResultOk &&
             readSnapshots(*processor)["lyrics"]["text"].getString() == maximum,
         "exactly 3 MiB is accepted and forwarded");
  auto oversized = makeLyricsMessage(maximum + 'x');
  expectLyricsRejected(*processor, oversized.get(), "3 MiB plus one byte is rejected");
  expect(processor->terminate() == kResultOk, "terminate lyrics validation processor");
}

void testIndependentSnapshotDelivery() {
  auto processor = makeProcessor();
  auto metadata = makeMessage();
  metadata->getAttributes()->setString("title", STR16("Track"));
  metadata->getAttributes()->setString("artworkMimeType", STR16("image/png"));
  metadata->getAttributes()->setBinary("artwork", "abc", 3);
  expect(processor->notify(metadata.get()) == kResultOk, "install metadata with artwork");
  const auto beforeLyrics = readSnapshots(*processor);
  auto lyrics = makeLyricsMessage("lyrics");
  expect(processor->notify(lyrics.get()) == kResultOk, "install independent lyrics");
  const auto loaded = readSnapshots(*processor);
  expect(revisionOf(loaded) == revisionOf(beforeLyrics) &&
             choc::json::toString(loaded["nowPlaying"]) == choc::json::toString(beforeLyrics["nowPlaying"]),
         "lyrics acceptance leaves metadata untouched");
  for (const auto *route : {"host/getInfo", "telemetry/read"}) {
    for (const auto *payload : {"{}", R"({"nowPlayingRevision":0})",
                                R"({"lyricsRevision":0})",
                                R"({"nowPlayingRevision":0,"lyricsRevision":0})"}) {
      const auto reply = readSnapshots(*processor, route, payload);
      expect(revisionOf(reply) == revisionOf(loaded) &&
                 lyricsRevisionOf(reply) == lyricsRevisionOf(loaded), "both routes always carry both revisions");
      expect(reply.hasObjectMember("nowPlaying") ==
                 (std::string_view(payload).find("nowPlayingRevision") != std::string_view::npos) &&
                 reply.hasObjectMember("lyrics") ==
                 (std::string_view(payload).find("lyricsRevision") != std::string_view::npos),
             "only explicit stale revisions receive their respective snapshots");
    }
    const auto matching = "{\"nowPlayingRevision\":" + std::to_string(revisionOf(loaded)) +
                          ",\"lyricsRevision\":" + std::to_string(lyricsRevisionOf(loaded)) + "}";
    const auto reply = readSnapshots(*processor, route, matching);
    expect(!reply.hasObjectMember("nowPlaying") && !reply.hasObjectMember("lyrics"),
           "matching revisions omit both payloads on both routes");
  }
  auto clearMetadata = makeMessage();
  expect(processor->notify(clearMetadata.get()) == kResultOk, "clear only metadata");
  const auto metadataCleared = readSnapshots(*processor);
  expect(metadataCleared["nowPlaying"].isVoid() &&
             lyricsRevisionOf(metadataCleared) == lyricsRevisionOf(loaded) &&
             choc::json::toString(metadataCleared["lyrics"]) == choc::json::toString(loaded["lyrics"]),
         "clearing metadata preserves lyrics and its revision");
  expect(processor->notify(metadata.get()) == kResultOk, "restore metadata");
  const auto beforeClear = readSnapshots(*processor);
  auto clearLyrics = makeMessage("EffeTune.NowPlayingLyrics");
  expect(processor->notify(clearLyrics.get()) == kResultOk, "clear only lyrics");
  const auto lyricsCleared = readSnapshots(*processor);
  expect(lyricsCleared["lyrics"].isVoid() && revisionOf(lyricsCleared) == revisionOf(beforeClear) &&
             choc::json::toString(lyricsCleared["nowPlaying"]) == choc::json::toString(beforeClear["nowPlaying"]),
         "clearing lyrics preserves metadata and its revision");
  expect(processor->terminate() == kResultOk, "terminate independent snapshots processor");
}
void testHostTransportDiscontinuityRevision() {
  auto processor = makeProcessor();
  ProcessSetup setup{};
  setup.processMode = kRealtime;
  setup.symbolicSampleSize = kSample32;
  setup.maxSamplesPerBlock = 64;
  setup.sampleRate = 48000.0;
  expect(processor->setupProcessing(setup) == kResultOk && processor->setActive(true) == kResultOk &&
             processor->setProcessing(true) == kResultOk,
         "prepare and start processing for the host transport test");

  std::array<float, 64> leftIn{}, rightIn{}, leftOut{}, rightOut{};
  float *inputChannels[] = {leftIn.data(), rightIn.data()};
  float *outputChannels[] = {leftOut.data(), rightOut.data()};
  AudioBusBuffers input, output;
  input.numChannels = output.numChannels = 2;
  input.channelBuffers32 = inputChannels;
  output.channelBuffers32 = outputChannels;
  ProcessContext context{};
  context.sampleRate = setup.sampleRate;
  context.state = ProcessContext::kPlaying;
  ProcessData data;
  data.numInputs = data.numOutputs = 1;
  data.inputs = &input;
  data.outputs = &output;
  data.processContext = &context;
  const auto render = [&](const std::int64_t position, const int32 frames) {
    context.projectTimeSamples = position;
    data.numSamples = frames;
    expect(processor->process(data) == kResultOk, "render host transport block");
  };
  const auto readTransport = [&](const std::string_view route) {
    const auto reply = readSnapshots(*processor, route, "{}");
    expect(reply["ok"].getWithDefault<bool>(false), "host transport bridge request succeeds");
    return choc::value::Value(reply["hostTransport"]);
  };
  const auto readRevision = [&](const std::string_view route) {
    const auto transport = readTransport(route);
    const auto revision = transport["discontinuityRevision"];
    expect(revision.isInt(), "host transport carries the discontinuity revision");
    return revision.getWithDefault<std::int64_t>(0);
  };

  render(48000, 64);
  const auto playing = readRevision("host/getInfo");
  render(48064, 32);
  render(48096, 64);
  expect(readRevision("telemetry/read") == playing,
         "ordinary variable blocks retain the same clock revision");
  render(50400, 64);
  render(50464, 32);
  render(50496, 64);
  const auto sought = readRevision("telemetry/read");
  expect(sought == playing + 1, "a short forward seek survives later continuous blocks before the poll");

  context.state = 0;
  render(50560, 64);
  context.state = ProcessContext::kPlaying;
  render(50560, 64);
  expect(readRevision("host/getInfo") == sought + 2,
         "pause and resume between polls change the clock revision");
  data.processContext = nullptr;
  render(50624, 64);
  data.processContext = &context;
  render(50624, 64);
  expect(readRevision("host/getInfo") == sought + 4,
         "context loss and recovery between polls change the clock revision");
  expect(processor->setProcessing(false) == kResultOk, "stop processing without a final callback");
  const auto stopped = readTransport("telemetry/read");
  expect(stopped["available"].getWithDefault<bool>(false) &&
             !stopped["playing"].getWithDefault<bool>(true) &&
             stopped["projectTimeSamples"].getWithDefault<std::int64_t>(0) == 50624,
         "the stop freezes the last host position while preserving an available clock");
  expect(processor->setProcessing(true) == kResultOk, "arm processing again");
  expect(!readTransport("host/getInfo")["playing"].getWithDefault<bool>(true),
         "starting processing does not revive the old playing snapshot before a callback");
  render(50688, 64);
  const auto resumed = readTransport("host/getInfo");
  expect(resumed["playing"].getWithDefault<bool>(false) &&
             resumed["projectTimeSamples"].getWithDefault<std::int64_t>(0) == 50688,
         "the first fresh callback restores the playing clock");

  const auto resumedRevision = readRevision("host/getInfo");
  expect(processor->setProcessing(false) == kResultOk && processor->setProcessing(true) == kResultOk,
         "stop and resume processing entirely between polls");
  render(50752, 64);
  render(50816, 64);
  expect(readRevision("telemetry/read") == resumedRevision + 1,
         "an unobserved processing stop invalidates the old clock even at a contiguous position");
  expect(processor->setProcessing(false) == kResultOk && processor->setActive(false) == kResultOk &&
             processor->terminate() == kResultOk,
         "deactivate and terminate the host transport test");
}
} // namespace

int main() {
  effetune::vst::testing::suppressCrtModalDialogs();
  try {
    testInterfacesAndOrdinaryHost();
    testReplacementOwnershipAndPolling();
    testAtomicTransportValidation();
    testLyricsReplacementAndLifetime();
    testLyricsValidation();
    testIndependentSnapshotDelivery();
    testHostTransportDiscontinuityRevision();
    std::cout << "EffeTune Now Playing metadata tests passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "Test failure: " << error.what() << '\n';
    return 1;
  }
}
