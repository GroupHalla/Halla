
<p align="center">
  <img src="src/assets/halla-logo.png" width="120" alt="Halla" />
</p>

<h1 align="center">Halla</h1>

<p align="center">
  Desktop voice communication client (Windows, Linux), written in
  <b>C++17</b> and <b>Qt 6 Widgets</b>.
</p>

<p align="center">
  <img src="https://i.imgur.com/XAjDMvm.png" width="720" alt="Halla main window" />
</p>

---

## Contents

- [Overview](#overview)
- [Screenshots](#screenshots)
- [Key features](#key-features)
- [Project architecture](#project-architecture)
- [Network protocol](#network-protocol)
- [Audio and voice](#audio-and-voice)
- [Code structure](#code-structure)
- [Building](#building)
- [Related projects](#related-projects)
- [License](#license)

---

## Try it now!

Halla's first official server is already in continuous operation and open to
the public.
The goal of this instance is to provide a stable, accessible environment where
users and developers can test low-latency audio performance, screen sharing
and the ecosystem's features.

**Server structure and features:**
- Permanent channels: open rooms for general interaction, technical testing
  and project alignment.
- Dynamic temporary channels: a system that lets any user create their own
  voice room on demand.
- Cross-platform access: fully integrated across the Desktop (Windows/Linux)
  and Mobile (Android) clients.
- Security: connections authenticated via Ed25519 cryptographic keys and
  voice traffic encrypted with ChaCha20-Poly1305.

**Connection details:**
- Address: 163.176.35.133
- Port: 9987

---

### Feedback and Issue Reporting Program — Halla
As the Halla ecosystem (Desktop, Mobile and Server) keeps advancing, our
commitment is to guarantee maximum stability, security and performance in
voice and screen transmissions.
So that we can identify and fix any failures quickly, we have opened an
official, direct channel for collecting bug reports, inconsistencies and
technical improvement suggestions.

**What you can report:**
- Connectivity, latency or synchronization problems with the server.
- Audio capture or playback failures (noise, echo or dropouts).
- Instabilities in screen sharing (FPS drops, resolution or freezing).
- Visual and behavioral bugs in the Desktop (Windows/Linux) or Mobile
  (Android) interface.
- Suggestions for new features and usability improvements.

Your contribution is essential to the continuous improvement of this
open-source project.
Submit your report through the official form:
https://docs.google.com/forms/d/e/1FAIpQLScwy7k_HyeNnl8kuNfMSs8H-pHUGfhuKijAxkYkzd7m_aX4NA/viewform

We thank everyone for their collaboration in strengthening the platform.

---

## Overview

**Halla** is a VoIP client for communities and private servers.
You join a server, browse a channel tree, talk over voice and text, share
your screen, and everything is managed through a system of groups and
granular permissions.

It connects to a server in the **Halla Server** family (its own protocol,
not TeamSpeak's proprietary protocol) via **TCP** (control/JSON) and
**UDP** (voice, Opus codec).

The project **does not depend on any external visual assets** — every icon in
the interface (avatars, channel symbols, padlocks, speaking indicators, etc.)
is drawn at runtime with `QPainter` (see `src/gui/Icons.cpp`), which keeps
the executable light and the icons crisp at any resolution/DPI.

## Screenshots

| Main window | Options — Capture | Options — Hotkeys |
|---|---|---|
| ![main](https://i.imgur.com/XAjDMvm.png) | ![capture](https://i.imgur.com/JOIdczm.png) | ![hotkeys](https://i.imgur.com/POsrBRD.png) |

| Connect | Options — Whisper | - |
|---|---|---|
| ![connect](https://i.imgur.com/pn5oL8q.png) | ![whisper](https://i.imgur.com/QBUwQCL.png) |

## Key features

**Voice**
- Push-to-talk (key **or mouse button**, including side/extra buttons),
  voice activity detection (VAD) with adjustable sensitivity, or
  continuous transmission.
- Codecs: Opus Voice/Music, plus the legacy Speex and CELT (for protocol
  compatibility), with per-channel bitrate and quality control.
- Signal processing: echo cancellation, background noise removal, typing
  attenuation, echo ducking while listening to others speak, and a real-time
  volume meter with a visual threshold. The DSP is embedded in the
  distributed binary (neural noise suppression via RNNoise + the AUMDF echo
  canceller from speexdsp, both Xiph/BSD) and upgrades to the WebRTC neural
  AEC3/NS on builds with the native SDK.
- **Whisper**: speak only to a specific channel, a channel + its
  subchannels, or a fixed list of users — with its own visual indicator
  (orange circle on the avatar), distinct from the normal speaking
  indicator (green).
- Local (WAV) recording of calls, your own voice + participants.
- Screen sharing.

**Channels and users**
- Channel tree with subchannels, temporary/semi-permanent/permanent
  channels, password-protected channels, moderated and linked channels
  (audio shared across "linked" channels).
- Server and channel groups with granular permissions (permission grid with
  filter and advanced "grant" mode), talk power, channel operator, channel
  commander.
- Avatars, BBCode/emoji descriptions, poke, complaints, offline messages,
  file transfer, ban list.
- Official global badges bound to the UID, fetched from a signed Ed25519
  registry, verified and kept in cache for offline operation.

**Interface**
- Light/dark theme switchable in real time, with no restart needed.
- Chat with tabs per server/channel, BBCode (`[b] [i] [u] [color=] [size=] [url=]`)
  and emojis.
- Server bookmarks, recent connections, multiple local identities,
  capture/playback profiles.
- System tray, sound notifications with built-in spoken alerts (connection,
  channel join/leave, permissions, microphone and playback) and optional
  text-to-speech narration (`QTextToSpeech`).
- Fully configurable hotkeys — including mouse buttons, captured at multiple
  layers so they don't depend only on the classic Windows event (useful with
  gaming-mouse software that intercepts side buttons).

**Screen sharing**
- **WebRTC** mode (recommended): ask to watch someone's stream from your
  channel; the video travels P2P (DTLS-SRTP) while offer/answer/ICE go
  through the server. The quality picker offers 720p/1080p/1440p/2160p and
  30/60 FPS only when resolution, FPS and bitrate fit within the HallaServer
  maximums. Resolution is chosen YouTube-style (480p, 720p, 1080p, 2K and
  4K), preserving the server's maximum aspect ratio; FPS and bitrate live in
  separate controls and never exceed the INI. Under **Options → Capture**,
  the Windows hardware H.264 encoder can be enabled for 1440p/4K and 60 FPS;
  if the GPU does not offer a compatible MFT, the client falls back to
  software VP8. Requires the native
  [Halla WebRTC Builds](https://github.com/GroupHalla/Halla-WebRTC-Builds)
  SDK compiled in (see [Building](#building)).
- Optional PC audio via process loopback on Windows: it captures the streams
  of the other applications and excludes `Halla.exe` and its child processes,
  avoiding re-transmitting the client's own voices and alerts (Windows build
  20348+). The same PCM feeds exclusively the WebRTC audio track for all
  viewers. On the Desktop, an internal 10 ms playout keeps decoding active;
  a short 40 ms prebuffer absorbs jitter without perceptibly delaying the
  audio relative to the video, and the PCM is played back through the
  mixer/QAudioSink.
- The compact **Watch Live** button uses a blue/purple pill with live and
  play indicators, following the product's look. The viewer keeps only the
  most recent WebRTC frame so latency does not accumulate. Hovering the mouse
  over the stream reveals an animated bar that lets you mute just that
  stream or stop watching; it hides when you leave or go idle.
- Legacy mode (JPEG over UDP), always available as an alternative, with no
  dependency on the WebRTC SDK.
- The channel tree groups bursts of updates into a single repaint, rejects
  cyclic/duplicate moves and tolerates stale data with an invalid parent,
  avoiding freezes while channels are reorganized.
- The creator of a temporary channel gets an editor limited to password,
  bitrate and maximum clients, and can also kick members out of that
  channel.

**Security**
- Control channel over **TLS**, with TOFU pinning (the certificate is
  trusted on the first connection to the server; you are alerted if it
  changes later — like the SSH model).
- Client identity via an **Ed25519** key pair: login proves possession of
  the private key by answering a signed challenge; the UID is derived from
  the public key, not something the client can simply claim.
- Private key kept in the **operating system's native vault**
  (Credential Manager/Keychain/Secret Service, via QtKeychain) — not in
  plain text in the settings.
- **Real E2EE (protocol v6)**: in addition to the identity's Ed25519 pair,
  each session uses an X25519 pair (signed binding validated at login).
  Voice/chat/poke/offline keys are generated and distributed by the clients
  themselves (`src/core/E2eeCrypto` + the v6 engine in `NetSession`):
  `e2e_key` envelopes with ephemeral X25519 + HKDF-SHA256 + AES-256-GCM,
  static-static pairwise content, and identity verification via a
  **9-digit SAS code** in the user information dialog. The server never
  sees a content key.
- Voice and legacy screen sharing encrypted with **ChaCha20-Poly1305**
  (AEAD) using the E2EE group keys, rotated whenever the channel's
  composition changes.
- Updates verified by SHA-256 checksum and pinned download domain before
  anything is installed automatically.

## Project architecture

```
                     ┌───────────────────┐
                     │     MainWindow    │  main window, menus, tabs
                     └─────────┬─────────┘
                               │
                 ┌─────────────┴─────────────┐
                 │         ServerTab          │  one tab = one connection
                 │  (tree + chat + info)      │
                 └───┬─────────────┬──────────┘
                     │             │
             ┌───────┴───┐   ┌─────┴──────┐
             │NetSession │   │ VoiceEngine │
             │ TCP (JSON)│   │ UDP (Opus)  │
             └───────────┘   └─────────────┘
                     │             │
                     └──────┬──────┘
                             ▼
                     Halla Server (self-hosted)
```

- **`NetSession`** keeps the TCP control connection and a `ServerData`
  (`src/core/Models.h`) always in sync with the state the server sends
  — users, channels, permissions, etc. Every change fires Qt signals that
  the UI listens to in order to repaint.
- **`VoiceEngine`** handles only audio: it captures the microphone every
  20 ms, encodes it to Opus and sends it over UDP; on the other end, it
  decodes, dejitters (a queue per sender) and mixes to the speakers. It
  degrades gracefully when no audio device is available.
- **`ServerTab`** is the tab of a connection: it brings together the channel
  tree (`ServerTreeWidget`), the chat (`ChatPanel`) and the information panel
  (`InfoPanel`), and it is what wires `NetSession`/`VoiceEngine` signals
  into the interface (including the PTT/whisper "hold key" logic).
- **`OptionsDialog`** recreates the classic Options window, with sidebar
  navigation by category (Application, Playback, Capture, Appearance,
  Notifications, Hotkeys, Whisper, Security, Add-ons).

## Network protocol

Fully specified in
[`PROTOCOL.md`](https://github.com/GroupHalla/HallaServer/blob/main/PROTOCOL.md)
of `HallaServer`, and implemented here in `src/net/HallaProtocol.h`:

- **Control**: TCP + **TLS 1.2+**, compressed JSON messages, one per line
  (`\n` as the delimiter), up to 2 MiB per message. Each message has a `"t"`
  field with the type (`"talking"`, `"whisper"`, `"user_state"`, `"webrtc_*"`
  signaling, etc.).
- **Voice (UDP)**: 20 ms Opus packets encrypted with **ChaCha20-Poly1305**
  (AEAD), with a 4-byte magic, the speaker's ID, a sequence number and the
  authenticated payload — the server never decrypts, it only relays.
- **Identity**: one Ed25519 key pair per client; login requires signing a
  challenge (nonce) from the server — the UID comes from the hash of the
  public key, not from whatever the client claims to be.
- Default port: **9987/tcp+udp**.
- Versioned protocol (`kProtoVersion` / `kProtoMin`, currently **v6**):
  the server accepts only v6. v6 requires **E2EE** — content keys generated
  and distributed by the clients (`e2e_key`, SAS) — and the security layer
  (TLS, Ed25519 identity, encrypted voice) is mandatory regardless of the
  version.

## Audio and voice

- Capture via `QAudioSource` at 48 kHz mono and stereo playback via
  `QAudioSink`, in 20 ms frames.
- Opus encoding and one independent decoder per sender (`libopus`).
- Queue per user, SDK PCM callbacks, per-participant spatialization/radio
  and stereo mixing with saturation before the speaker.
- PTT and whisper are "hold keys": the app periodically polls (key or mouse
  button) whether the configured key is physically pressed, including via
  global capture on Windows, so it works even when Halla is in the
  background.

## Code structure

```
src/
├── app/            MainWindow (window/menus), Theme (light/dark),
│                   SoundPack (sounds), Speech (TTS)
├── core/           Models.h (session data), Settings.h (persistent
│                   config), SecureStore (OS vault via QtKeychain),
│                   E2eeCrypto (X25519/Ed25519 + HKDF + AES-256-GCM of
│                   E2EE v6), BadgeRegistry, AppLog (event logging)
├── net/            NetSession (TCP/control, TLS+TOFU, E2EE v6 engine —
│                   group keys, e2e_key envelopes, SAS), VoiceEngine
│                   (UDP/audio, AEAD), HallaProtocol.h (protocol
│                   shared with the server)
├── webrtc/         HallaWebRtcSession (WebRTC screen sharing,
│                   optional — requires the Halla WebRTC Builds SDK)
├── gui/            ServerTab, ServerTreeWidget, ChatPanel, InfoPanel,
│                   HotkeyEdit, Icons (icons drawn in code),
│                   WelcomePage, TsBanner, RichTextBrowser
├── dialogs/        OptionsDialog, ConnectDialog, ChannelDialog,
│                   GroupsDialog, IdentityDialog (Ed25519 keys),
│                   BookmarksDialog, AdminDialogs (banlist/complaints/
│                   groups/permissions), ToolsDialogs (whisper/contacts/
│                   file transfer), MiniDialogs
│                   (poke/kick-ban/volume/etc.), LogDialog, AboutDialog
├── assets/         logo and vector icons for the tree
└── main.cpp        entry point (Qt Application, theme, arguments)
```

## Client extensions and packages

The **Options → Add-ons** tab installs `.halla-addon` packages, enables and
disables plugins, opens settings declared by the package and browses the
HTTPS catalog. Native Windows plugins use `QLibrary` and the public C ABI in
[`sdk/halla_plugin_api.h`](sdk/halla_plugin_api.h). Beyond the compatible
base API, the SDK has modular interfaces for connections, clients/channels,
capture and playback PCM, 3D audio, radio filters, data transport over
protocol v5, notifications, actions and shortcuts. The transport respects
channel isolation and the `pluginData` permission; global broadcasts require
the administrative permission `pluginDataGlobal`. There is a minimal example
in [`examples/plugins/hello_world`](examples/plugins/hello_world) and an
advanced consumer in [`examples/plugins/advanced_sdk`](examples/plugins/advanced_sdk).

The Desktop also ships the official **call overlay** and the official
**police radio voice** add-on. The latter filters sending and listening
separately for whispers, normal voice or both, with configurable intensity
and static noise. See [`docs/PLUGINS.md`](docs/PLUGINS.md) for the manifest,
packaging, audio, events and security rules.

## Building

### Dependencies

- CMake ≥ 3.21
- Compiler with C++17
- Qt 6.2+ with the **Widgets**, **Network**, **Multimedia** and
  **TextToSpeech** modules
- **OpenSSL** (Ed25519 identity, password hashing, AEAD voice)
- **libopus** (on Windows, a static build in `third_party/opus/`; on
  Linux/macOS, the system version via `pkg-config`)
- **QtKeychain** (secure storage of the identity in the OS vault)
- Optional: the native SDK from
  [Halla WebRTC Builds](https://github.com/GroupHalla/Halla-WebRTC-Builds),
  for screen sharing over WebRTC (without it, the app builds normally and
  falls back to the legacy screen-sharing mode)

### Linux

```bash
./build-linux.sh        # installs cmake/ninja/qt6-base-dev if missing
./build/Halla
```

### Windows / manual (any platform)

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

To enable native WebRTC (P2P screen sharing), download/build the SDK from
[Halla WebRTC Builds](https://github.com/GroupHalla/Halla-WebRTC-Builds) and
add:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DHALLA_ENABLE_WEBRTC_NATIVE=ON \
  -DHALLA_WEBRTC_SDK_DIR=/path/to/halla-webrtc-sdk
```

On Windows, CMake also embeds the executable's icon and version information
(`src/halla.rc.in`) and builds the NSIS installer (`packaging/halla-setup.nsi`).

## Related projects

- **[Halla Server](https://github.com/GroupHalla/HallaServer)** — a
  self-hostable server (C++/Qt) that speaks the same protocol; see
  [`PROTOCOL.md`](https://github.com/GroupHalla/HallaServer/blob/main/PROTOCOL.md)
  for the full specification.
- **[Halla Mobile](https://github.com/GroupHalla/Halla-Mobile)** — native
  Android client (Kotlin + C++/JNI core), not Qt.
- **[Halla WebRTC Builds](https://github.com/GroupHalla/Halla-WebRTC-Builds)**
  — prebuilt native WebRTC SDK, used by this client's screen sharing.

## License

Free for non-commercial use ([`LICENSE`](LICENSE)): use, study, modify and
redistribute free of charge, without asking permission. Selling, renting or
embedding it in a commercial product requires written authorization from the
maintainers. Third-party components (Qt, Opus, OpenSSL, libwebrtc,
mbedTLS) follow their respective original licenses.
