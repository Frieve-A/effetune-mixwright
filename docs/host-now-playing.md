# Host-supplied Now Playing metadata

Any host embedding EffeTune Mixwright can supply the currently playing media's
title, album, artist, and artwork to Visualizer. This optional extension is
independent of the host name, audio processing, and the built-in media player.
Hosts that do not send metadata retain the existing empty metadata display.

## Message contract

After initializing the plug-in, query `Steinberg::Vst::IConnectionPoint` from
either its `IComponent` or `IEditController` and call `notify` on the UI thread.
For this extension, direct notification requires no extra `connect` handshake
or outgoing-message receiver. Do not send from the audio callback.

Set the `IMessage` ID to **`EffeTune.NowPlaying`**. This ID identifies version 1
of this contract; a breaking change will use a different ID. The attributes are:

| Attribute | VST3 type | Meaning and limit |
| --- | --- | --- |
| `title` | `setString` | Title, up to 4096 UTF-16 code units excluding the terminator |
| `album` | `setString` | Album, same limit |
| `artist` | `setString` | Artist, same limit |
| `artwork` | `setBinary` | Encoded image bytes, up to 8 MiB (8,388,608 bytes) |
| `artworkMimeType` | `setString` | `image/png`, `image/jpeg`, `image/gif`, or `image/webp` |

Strings must contain well-formed, null-terminated UTF-16. Artwork with nonempty
bytes requires its MIME attribute. Supply browser-decodable encoded image data
matching that MIME; the native receiver validates the transport and MIME, and
the WebView decodes the image. Successful notification does not certify that
the image can be decoded. Prefer PNG or JPEG for the widest support across
platforms and embedded WebView versions. Video thumbnails can be sent as artwork.

Every message replaces the complete snapshot. Missing or empty fields clear
the corresponding value; an empty or absent attribute list clears all metadata.
Send a fresh message on each media change and resend all values to retain them. Fields
outside this contract are ignored. Unknown message IDs retain the SDK's normal
handling and cannot change the snapshot.

`notify` returns `kResultOk` on acceptance. Null messages, missing message IDs,
invalid field types or UTF-16, excessive sizes, or nonempty artwork with an
unsupported MIME return `kInvalidArgument`; the previous snapshot is retained in
full. Check the result before considering the update delivered.

The plug-in copies accepted strings and artwork before `notify` returns. The
host can release the message immediately afterward. Metadata is private to the
plug-in instance, survives closing/reopening the editor and pipeline state
reloads, and is cleared by an explicit empty message or plug-in termination.
It is not saved in VST state, presets, or pipeline history. The host should send
the current snapshot after each new plug-in initialization.

## Host-side C++ example

This example uses the SDK's message-allocation helper and reference-counted
pointers. `pluginComponent` is the initialized component and `host` implements
`IHostApplication`. The artwork pointer remains valid through the call.

```cpp
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsthostapplication.h"

Steinberg::tresult sendNowPlaying(
    Steinberg::Vst::IComponent* pluginComponent,
    Steinberg::Vst::IHostApplication* host,
    const Steinberg::Vst::TChar* title,
    const Steinberg::Vst::TChar* album,
    const Steinberg::Vst::TChar* artist,
    const void* artwork, Steinberg::uint32 artworkBytes,
    const Steinberg::Vst::TChar* artworkMimeType)
{
    using namespace Steinberg;
    using namespace Steinberg::Vst;
    if (!pluginComponent || !host)
        return kInvalidArgument;
    FUnknownPtr<IConnectionPoint> receiver(pluginComponent);
    if (!receiver)
        return kNoInterface;
    auto message = owned(allocateMessage(host));
    if (!message)
        return kOutOfMemory;
    message->setMessageID("EffeTune.NowPlaying");
    auto* attributes = message->getAttributes();
    if (!attributes)
        return kInvalidArgument;
    if (title && attributes->setString("title", title) != kResultOk)
        return kInvalidArgument;
    if (album && attributes->setString("album", album) != kResultOk)
        return kInvalidArgument;
    if (artist && attributes->setString("artist", artist) != kResultOk)
        return kInvalidArgument;
    if (artworkBytes != 0) {
        if (!artwork || !artworkMimeType ||
            attributes->setString("artworkMimeType", artworkMimeType) != kResultOk ||
            attributes->setBinary("artwork", artwork, artworkBytes) != kResultOk)
            return kInvalidArgument;
    }
    return receiver->notify(message);
}
```

Pass null strings and zero artwork bytes to clear everything. To send text
without artwork, pass its strings and zero artwork bytes. No editor needs to
be open when the host sends either update.

## Native/WebView delivery

The plug-in caches the latest accepted snapshot and exposes it through the
existing `host/getInfo` startup and `telemetry/read` polling routes. Requests
carry the last `nowPlayingRevision` seen (zero for a fresh editor). Responses
always include `nowPlayingRevision`, and include `nowPlaying` only when the
revision differs. An empty snapshot is `null`; otherwise it contains
`title`, `album`, `artist`, and an `artwork` array containing `{src: dataURL}`
when artwork exists. Equal revisions omit the image payload. Reading a
snapshot does not consume it, so new editors can replay the cached value.

These JSON fields belong to the internal UI bridge. Hosts only implement the
VST3 message contract above.

VST3 API references: [IConnectionPoint](https://steinbergmedia.github.io/vst3_doc/vstinterfaces/classSteinberg_1_1Vst_1_1IConnectionPoint.html),
[IAttributeList](https://steinbergmedia.github.io/vst3_doc/vstinterfaces/classSteinberg_1_1Vst_1_1IAttributeList.html).
