# Halla Desktop Add-on SDK

Halla Desktop loads community native plugins through a stable, Qt-free C ABI,
declared in [`sdk/halla_plugin_api.h`](../sdk/halla_plugin_api.h). On
Windows, each plugin is a DLL loaded with `QLibrary`. The header and the
examples can be used under the permissive license of
[`sdk/LICENSE.txt`](../sdk/LICENSE.txt).

> **Security:** a DLL runs in the same process and with the same privileges
> as Halla. Declared capabilities tell the user what the add-on intends to
> do, but they do not form a sandbox against malicious native code.

## ABI compatibility

The base ABI remains `HALLA_PLUGIN_ABI_VERSION 1`. Plugins built with
SDK 1.0.63 remain binary-compatible. Extensive features were not added
directly to a giant struct: the additive field
`HallaHostApi::query_interface` provides independent, versioned modules:

```text
halla.core.v1
halla.connection.v1
halla.audio.v1
halla.data.v1
halla.ui.v1
```

Before using `query_interface`, check `host->struct_size`. A plugin must
accept that an interface or optional function may be unavailable.

## The `.halla-addon` package

The package is a regular ZIP renamed to `.halla-addon`:

```text
manifest.json
bin/windows-x64/my_plugin.dll
assets/...
```

Advanced manifest example:

```json
{
  "id": "com.example.positional-audio",
  "name": "Positional audio",
  "version": "1.0.0",
  "author": "Halla Community",
  "description": "Integrates position and radio from a game.",
  "type": "native",
  "apiVersion": 1,
  "defaultEnabled": false,
  "capabilities": [
    "connection.read",
    "audio.playback",
    "audio.spatial",
    "plugin.data",
    "ui.notifications",
    "ui.actions"
  ],
  "platforms": {
    "windows-x64": {
      "library": "bin/windows-x64/positional_audio.dll"
    }
  },
  "settings": [
    {
      "key": "maxDistance",
      "type": "int",
      "label": "Maximum distance",
      "default": 60,
      "min": 5,
      "max": 1000
    }
  ]
}
```

IDs accept lowercase ASCII letters, numbers, dot, hyphen and underscore. The
library must use a relative path and remain inside the package.

The official packager creates a reproducible ZIP and checksum:

```bash
python tools/package_plugin.py my-plugin-folder my-plugin.halla-addon
```

## Capabilities

| Capability | Access granted |
|---|---|
| `connection.read` | Snapshots of connections, servers, users, channels and permissions |
| `connection.control` | Own channel, states, nickname, chat, whisper, local mute and volume |
| `audio.capture` | S16 PCM from the microphone before Opus encoding |
| `audio.playback` | PCM of each received voice and the final stereo mix |
| `audio.spatial` | 3D position, gain, pan and per-user radio filter |
| `plugin.data` | TLS messages between instances of the same add-on |
| `ui.notifications` | Client notifications |
| `ui.actions` | Actions in the Add-ons menu and window shortcuts |

The installer shows the requested capabilities before the final confirmation.
Undeclared interfaces return `NULL`; unauthorized operations return
`HALLA_RESULT_PERMISSION_DENIED`.

## Entry point and lifecycle

The DLL exports a C function without name mangling:

```cpp
extern "C" HALLA_PLUGIN_EXPORT
const HallaPluginApi* halla_plugin_entry(void);
```

The plugin provides:

- metadata that must match the manifest;
- `initialize` and `shutdown`;
- `on_event` for JSON events;
- `on_settings_changed` for updated settings without a restart.

`shutdown()` must cancel jobs, remove callbacks and release resources before
returning. Halla also removes any remaining processors, handlers, actions and
spatial state before unloading the library.

## Core interface

`HallaCoreApiV1` offers:

- a monotonic clock in milliseconds;
- JSON information about the application, platform and interfaces;
- `post_to_ui`, to schedule short work on the main thread.

`post_to_ui` can be called from a thread owned by the add-on. The scheduled
function must not block the interface.

## Connections, clients and channels

`HallaConnectionApiV1` works with `connection_id`. The zero value represents
the active connection. IDs remain stable for the lifetime of the tab.

`get_connections_json` lists all tabs; `get_connection_json` provides:

- server, address, version, platform and ping;
- the local user and current channel;
- effective permissions;
- all visible users, UIDs, channels, speaking and mute states;
- groups, description, local volume/mute and screen share;
- the full tree of visible channels, codec, bitrate, links and participants.

Control operations include:

- moving your own user;
- setting input/output mute and away;
- changing the nickname;
- sending channel, server or private chat;
- setting whisper targets;
- locally muting and adjusting a user's volume;
- moving, poking, setting channel commander, kicking or banning users;
- creating, editing and deleting channels through the protocol's JSON
  objects.

The server remains the authority. Permissions, hierarchy, password and other
rules can refuse an action requested by the plugin.

## Audio pipeline

The voice engine uses mono Opus, 48 kHz, 20 ms frames and stereo playback.
Each sender has its own Opus decoder and its own queue. Voices are processed
individually, spatialized and then mixed with saturation.

### PCM callbacks

`register_processor` registers a callback at the stages:

| Stage | Format |
|---|---|
| `HALLA_AUDIO_CAPTURE` | Mono S16 PCM, local user, before VAD/Opus |
| `HALLA_AUDIO_CAPTURE_AFTER_VAD` | Mono S16 PCM from the local user after the transmit decision (VAD/PTT) — the correct point for filters with AGC, which would otherwise raise the noise and open the voice detector on their own |
| `HALLA_AUDIO_REMOTE_BEFORE_SPATIAL` | Mono S16 PCM from a participant after Opus |
| `HALLA_AUDIO_MIXED_PLAYBACK` | Stereo S16 PCM from the final mix |

On Halla Mobile the transmit decision happens before the native layer, so
capture is reported as `HALLA_AUDIO_CAPTURE` and registering at
`HALLA_AUDIO_CAPTURE_AFTER_VAD` is accepted as a synonym.

`HallaAudioFrame` contains the connection, user, mutable samples, frames,
channels, sample rate and flags. `HALLA_AUDIO_FLAG_WHISPER` indicates that
the frame is being sent/received as a whisper. Test `struct_size` before
reading additive fields. The buffers belong to Halla and are valid only
during the callback.

### Real-time rules

Audio callbacks **must not**:

- block on network, files, a long-held mutex or the UI;
- open dialogs;
- wait on another thread;
- retain the samples pointer;
- throw exceptions across the C boundary.

Pre-allocate buffers and send non-urgent work to another thread. An exception
caught by the host disables that add-on's processor.

### High-level spatial audio

For most plugins, prefer the safe high-level functions:

```cpp
audio->set_listener_transform(ctx, connection, &listener);
audio->set_user_transform(ctx, connection, userId, &position,
                          1.0f, 80.0f, 1.4f);
audio->set_user_gain(ctx, connection, userId, 0.8f);
audio->set_user_pan(ctx, connection, userId, -0.25f);
audio->set_user_radio_effect(ctx, connection, userId, 1, 0.9f, 0.15f);
```

The host computes attenuation between the minimum/maximum distance, left and
right orientation, stereo pan and the radio filter. Multiple plugins can
contribute; gains are multiplied and pans are summed with clamping.

`reset_user` and `reset_connection` immediately remove the state applied by
the plugin. `play_pcm` injects 48 kHz mono/stereo S16 PCM effects, limited to
ten seconds per call — useful for clicks, beeps and radio noise. Halla also
removes all state when the add-on is disabled/unloaded.

## Plugin data transport — protocol v5

`HallaDataApiV1` sends binary payloads over the Halla Server TCP/TLS channel.
Messages are isolated by add-on ID and can target:

- specific users **in the same channel**;
- participants of the current channel;
- all compatible clients on the server, only with `pluginDataGlobal`.

Sending locally requires `pluginData` and `listen` in effect on the channel.
Explicit recipients outside the channel cause the entire message to be
refused; the global reach is administrative and denied by default.

Limits:

- 8 KiB payload;
- 64-byte UTF-8 topic;
- up to 64 explicit recipients;
- rate limit of 200 messages per 10 seconds per client.

The server does not persist or interpret the payload. It validates,
throttles and forwards only to clients with protocol v5. The callback
receives the connection, sender, topic and bytes. For coordinates, use
versioned structs, a defined endianness and a moderate frequency (usually
10–20 Hz).

## UI and shortcuts

`HallaUiApiV1` allows:

- showing notifications;
- registering actions in the **Add-ons** menu;
- assigning a window shortcut to the action;
- removing the action during `shutdown`.

Action callbacks run on the main thread. On Windows, compatible keyboard
sequences are registered with `RegisterHotKey` and keep working even with
the game in focus; if the global registration is taken or cannot be
represented, Halla keeps the shortcut in the window context. On the other
platforms, this version uses window shortcuts.

## JSON events

Current events:

- `client_state`: compact snapshot of the active connection, also used by
  the overlay;
- `connection_opened` / `connection_closed`;
- `connection_state`: detailed snapshot of any updated connection;
- `chat_message` and `poke_received`;
- `server_error`: asynchronous refusal of an action by the server;
- `plugin_data`: base64 copy for compatibility with event-driven plugins;
- `application_shutdown`.

Plugins that register `HallaPluginDataFn` also receive the binary payload
directly, with no base64 conversion.

## Architecture for SaltyChat, TFAR and ACRE

A recommended integration has three parts:

1. A game resource/mod obtains position, orientation, vehicle and radio
   through the official API and delivers the data to the DLL via named
   pipe/local socket.
2. The DLL sends versioned metadata via `HallaDataApiV1` and receives the
   states of the other participants.
3. The DLL updates listener/sources via `HallaAudioApiV1`; Halla performs
   the attenuation, pan and filters inside the audio pipeline.

Reading game memory with `ReadProcessMemory` is technically possible for a
native DLL, but it is neither offered nor recommended by Halla: it can break
on updates and trigger anticheats. Prefer FiveM/Arma APIs and authenticated
local IPC.

## Examples

- [`examples/plugins/hello_world`](../examples/plugins/hello_world): base
  ABI, logging, settings and events.
- [`examples/plugins/advanced_sdk`](../examples/plugins/advanced_sdk):
  modular discovery, connection snapshots, PCM callback, binary data,
  notification, action and shortcut.

## Catalog

The Add-ons tab reads the official add-on hub over HTTPS:

```text
https://grouphalla.github.io/Halla-Addons/api/v1/addons.json
```

Each item provides an HTTPS URL and a SHA-256. The download is canceled if
the checksum does not match.

## Package security limits

- 100 MiB compressed;
- 2,000 entries;
- 250 MiB extracted;
- 256 KiB per manifest;
- blocking of absolute paths, `..` and symbolic links;
- catalog of at most 1 MiB and 500 entries displayed.

## Official add-ons

### Call overlay

`official.talking-overlay` is an internal extension, disabled by default. It
uses a transparent, click-through, always-on-top window, with no injection
or hooking. It is aimed at windowed and borderless fullscreen games.

### Police radio voice

`official.radio-voice` is also internal and disabled by default. It
processes PCM before Opus when sending and before spatialization when
listening. The user chooses whether the effect is applied, separately in
each direction, to:

- no audio;
- whispers only;
- normal voice only;
- whispers and normal voice.

Intensity, static noise and post-effect volume are configurable. The DSP
combines peak AGC, a narrow radio-communication band, saturation,
small-speaker resonance, squelch with an adaptive threshold and crackling
static. If the sender filters the microphone, all recipients receive the
already modified voice; the listening filter is local and only affects
whoever enabled it. If both sides apply the effect to the same speech, it is
filtered twice; in that case, the recipient can leave that direction at
**Do not apply**.

The same effect is also distributed as a `.halla-addon` package through the
official catalog ([Halla-Addons](https://grouphalla.github.io/Halla-Addons/)):
installed, the package **replaces** the internal add-on with the same id;
removed, it restores the previous state. This is the path through which the
radio filter is updated without publishing a new application version — the
package source lives in `plugins/official/radio-voice/` and CI publishes the
`radio_voice.dll` as a build artifact (alongside Halla Mobile's Android
libraries, it goes into the catalog's cross-platform package).
