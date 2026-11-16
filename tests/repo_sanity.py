from pathlib import Path
import re
import runpy
import wave

root = Path(__file__).resolve().parents[1]
version = (root / "VERSION").read_text(encoding="utf-8").strip()
assert re.fullmatch(r"\d+\.\d+\.\d+", version), version
assert "@HALLA_VERSION@" in (root / "src/version.h.in").read_text(encoding="utf-8")
resource_template = (root / "src/halla.rc.in").read_text(encoding="utf-8")
assert "@HALLA_VERSION@" in resource_template
assert "@PROJECT_VERSION_MAJOR@" in resource_template
cmake = (root / "CMakeLists.txt").read_text(encoding="utf-8")
assert "file(STRINGS" in cmake and "VERSION" in cmake
protocol = (root / "src/net/HallaProtocol.h").read_text(encoding="utf-8")
assert "kProtoVersion = 6" in protocol
assert "kProtoMin = 6" in protocol  # v6: hard cut-over -- v1-v5 clients are refused
assert "kVoiceTokenBytes = 16" in protocol
updater = (root / "src/app/MainWindowUpdates.cpp").read_text(encoding="utf-8")
for required in ("QCryptographicHash::Sha256", "verifyAuthenticode", "kMaxInstallerBytes"):
    assert required in updater, required
installer = (root / "packaging/halla-setup.nsi").read_text(encoding="utf-8")
assert "APP_VERSION é obrigatório" in installer

sound_names = (
    "banned", "connected", "connection_lost", "disconnected", "error",
    "insufficient_permissions", "kicked", "mic_muted", "mic_unmuted",
    "moved", "poke", "sound_muted", "sound_resumed", "user_joined",
    "user_left",
)
qrc = (root / "src/halla.qrc").read_text(encoding="utf-8")
sound_pack = (root / "src/app/SoundPack.cpp").read_text(encoding="utf-8")
for name in sound_names:
    wav_path = root / "src/assets/sounds" / f"{name}.wav"
    assert wav_path.is_file() and wav_path.stat().st_size <= 128 * 1024, name
    assert f"assets/sounds/{name}.wav" in qrc, name
    assert f'StringLiteral("{name}")' in sound_pack, name
    with wave.open(str(wav_path), "rb") as wav:
        assert wav.getnchannels() == 1, name
        assert wav.getsampwidth() == 2, name
        assert wav.getframerate() == 24000, name
        assert 0 < wav.getnframes() <= 24000 * 3, name

main_window = (root / "src/app/MainWindow.cpp").read_text(encoding="utf-8")
net_session = (root / "src/net/NetSession.cpp").read_text(encoding="utf-8")
server_tab = (root / "src/gui/ServerTab.cpp").read_text(encoding="utf-8")
voice_engine = (root / "src/net/VoiceEngine.cpp").read_text(encoding="utf-8")
assert "m_closeDelayPending" in main_window and "QTimer::singleShot(1000" in main_window
# The PTT key may only become a global hotkey in "push to talk" mode
# (capture/pttMode == 0). Registering it in voice-activated (1, DEFAULT) and
# continuous (2) modes swallows a whole system key for nothing -- with the
# default Space, every user in voice-activated mode lost the space bar in
# every application.
assert 'if (S::num("capture/pttMode", 1) != 0) return;' in main_window
assert "m_intentionalDisconnect" in net_session and "waitForBytesWritten" not in net_session
assert 'HSound::play(QStringLiteral("moved"))' in server_tab
assert "Silenciar todos os avisos de áudio" in (root / "src/dialogs/OptionsDialog.cpp").read_text(encoding="utf-8")
webrtc = (root / "src/webrtc/HallaWebRtcSession.cpp").read_text(encoding="utf-8")
assert "PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE" in webrtc
# WebRTC threading (v1.1.6 -- app crash when clicking "Share" right after
# connecting): libwebrtc observers (OnIceCandidate/OnTrack/OnSuccess of
# offer/answer) run on the SIGNALING THREAD, while handleSignal/closePeer/
# startBroadcast run on the GUI thread. Three families of protection are
# MANDATORY in the source and must not regress:
#   1) sends from the signaling thread PACKAGED to the GUI -- writing to the
#      NetSession QTcpSocket from another thread corrupted the heap during the
#      offer/answer/ICE burst that follows the start of a broadcast;
#   2) every access to m_native->peers under peersMutex -- the std::map was
#      read and written by both threads at the same time;
#   3) Start/Stop of loopback ADM playout/recording under a mutex -- two
#      concurrent joins on the same std::thread terminate the process
#      (std::terminate) with no dialog: the app "closes by itself".
webrtc_code = re.sub(r"//[^\n]*", "", webrtc)
for required in ("std::recursive_mutex peersMutex",
                 "peersMutex", "peerConnection(int peerId)",
                 "setNetSession", "detachFromNet",
                 "Qt::QueuedConnection);\n}",):
    assert required in webrtc, required
# sendNative* may only touch the NetSession inside a lambda queued to the GUI
# (calling m_net->sendWebRtc* directly outside invokeMethod is the race that
# crashed the app). v1.1.28: the answer leaves through the peer connection
# (netForPeer, multi-tab) with a fallback to m_net -- BOTH forms are allowed,
# as long as they run INSIDE invokeMethod(..., Qt::QueuedConnection).
def _queued_spans(code):
    """[start, end) spans of the bodies of lambdas queued through
    QMetaObject::invokeMethod(this, [...] { ... }, Qt::QueuedConnection),
    with correct brace balancing (works with nested blocks)."""
    spans = []
    for m in re.finditer(r"invokeMethod\(this, \[[^\]]*\] \{", code):
        depth, i = 1, m.end()
        while i < len(code) and depth:
            if code[i] == '{':
                depth += 1
            elif code[i] == '}':
                depth -= 1
            i += 1
        if re.match(r"\s*,\s*Qt::QueuedConnection", code[i:i + 60]):
            spans.append((m.end() - 1, i - 1))
    return spans

_spans = _queued_spans(webrtc_code)
assert _spans, "invokeMethod enfileirado ausente no HallaWebRtcSession"
for call in ("sendWebRtcIce", "sendWebRtcOffer", "sendWebRtcAnswer"):
    hits = list(re.finditer(r"->" + call + r"\(", webrtc_code))
    assert hits, f"{call} sumiu do HallaWebRtcSession"
    for hit in hits:
        assert any(a <= hit.start() < b for a, b in _spans), \
            f"{call} fora de invokeMethod enfileirado (corrida GUI x signaling)"
# The loopback ADM needs the mutex on all four thread transitions.
assert webrtc_code.count("std::lock_guard<std::mutex> threadLock(m_threadMutex);") == 4, \
    "Start/StopPlayout/Recording do ADM precisam do mutex de thread"
# QScreen::grabWindow() must NEVER run on the capture thread (it only exists
# on the GUI thread): the fallback crashed when DXGI failed on a minimized
# window. (grabWindowsAppForWebRtc is the GDI/PrintWindow capture -- allowed.)
_capture_frame = webrtc_code[webrtc_code.find("void HallaWebRtcSession::captureFrame()"):]
_capture_frame = _capture_frame[:_capture_frame.find("\n}\n")]
assert "->grabWindow(" not in _capture_frame and "::grabWindow(" not in _capture_frame, \
    "captureFrame must not use QScreen::grabWindow"
assert "m_screenGeometries" in _capture_frame, "crop deve usar o snapshot de geometrias da GUI"
assert "QGuiApplication::screens()" not in _capture_frame, "screens() only on the GUI thread"
# MainWindow keeps the NetSession m_net of the WebRTC session alive: wireTab
# refreshes it, finishDisconnectTab detaches BEFORE the NetSession
# deleteLater.
assert "m_webrtcSession->setNetSession(tab->net());" in main_window
assert "m_webrtcSession->detachFromNet(net);" in main_window
mf_h264 = (root / "src/webrtc/MediaFoundationH264.cpp").read_text(encoding="utf-8")
assert "MFT_ENUM_FLAG_HARDWARE" in mf_h264
assert "MFVideoFormat_H264" in mf_h264
assert "I420ToNV12" in mf_h264
assert "MFCreateDXGISurfaceBuffer" in mf_h264
assert "MFT_MESSAGE_SET_D3D_MANAGER" in mf_h264
assert "supports_native_handle = true" in mf_h264
assert "is_hardware_accelerated = true" in mf_h264
assert "CreateVideoProcessor" in webrtc and "DXGI_FORMAT_NV12" in webrtc
assert "PushBuffer(native)" in webrtc
assert "resetNativeFactoryForEncoderSetting" in webrtc
assert 'S::flag("screenshare/hardwareEncoder", true)' in webrtc
options_source = (root / "src/dialogs/OptionsDialog.cpp").read_text(encoding="utf-8")
assert "screenshare/hardwareEncoder" in options_source
application_page = options_source[
    options_source.index("QWidget* OptionsDialog::pageApplication()"):
    options_source.index("QWidget* OptionsDialog::pageDesign()")
]
assert "screenshare/hardwareEncoder" in application_page
assert "TargetProcessId = GetCurrentProcessId()" in webrtc
assert "GetDefaultAudioEndpoint" not in webrtc
# Desktop <-> Mobile regressions: early ICE must be queued, a remote live
# stream only opens on an explicit click, and UDP uses the peer actually
# chosen by TLS.
assert "pendingRemoteIce" in webrtc and "remoteDescriptionReady" in webrtc
screen_dialog = (root / "src/dialogs/ScreenShareDialog.cpp").read_text(encoding="utf-8")
assert "populateResolutionOptions" in screen_dialog
assert "{480, 720, 1080, 1440, 2160}" in screen_dialog
assert "m_fpsCombo" in screen_dialog
assert "m_bitrateSpin->setRange(500, m_maxBitrateKbps)" in screen_dialog
assert "recommendedBitrateKbps" in screen_dialog
assert "screenshareBitrateKbps" in net_session
assert 'message["bitrate"] = bitrateKbps' in net_session
assert "AudioTrackSinkInterface" in webrtc
assert "attachRemoteAudioTrack" in webrtc
assert "remoteAudioReceived" in webrtc
# Broadcast audio uses the WebRTC track exclusively. The custom ADM must pull
# the playout so libwebrtc decodes and delivers OnData.
assert "NeedMorePlayData" in webrtc and "playoutLoop" in webrtc
assert "PlayoutIsAvailable" in webrtc and "StartPlayout" in webrtc
assert "RemoteAudioSink" in webrtc and "m_pendingRemoteFrames" in webrtc
assert "playStreamPcm" in main_window and "playStreamPcm" in voice_engine
assert "kStreamPrebufferFrames = 2" in voice_engine
assert "m_streamQueues" in voice_engine and "clearStreamPcm" in voice_engine
# The live controls appear on hover, slide up from the bottom and allow
# mudo individual/parar de assistir sem afetar a chamada.
for required in ("liveControls", "m_audioButton", "stopWatching",
                 "QPropertyAnimation", "isAudioMuted", "FastTransformation"):
    assert required in main_window, required
# Compact button from the design mockup: gradient pill, live icon drawn with
# QPainter and a white capsule with play on the right.
for required in ("class WatchLiveButton", "Assistir Live", "setFixedSize(232, 46)",
                 "QLinearGradient background", "liveOrb", "playCapsule"):
    assert required in main_window, required
assert "if (userId != tab->data().selfId) return" in main_window
assert "openScreenShareWindow(userId)" in main_window
assert "normalizedPeerAddress(m_tcp->peerAddress())" in net_session
assert "for (const QByteArray& key : m_channelKeys)" in net_session
tree_widget = (root / "src/gui/ServerTreeWidget.cpp").read_text(encoding="utf-8")
# Channel reordering can produce a burst of updates. The model receives all of
# them but the tree repaints only once; cycles/orphans never recurse forever.
assert "scheduleChannelStateChanged" in net_session
assert 'if (t == "chan_update")' in net_session
assert "QSet<int> path" in tree_widget
assert "wouldCreateChannelCycle" in tree_widget
assert "isDuplicateChannelMove" in tree_widget
assert "dropMimeData" not in tree_widget

models = (root / "src/core/Models.h").read_text(encoding="utf-8")
group_dialog = (root / "src/dialogs/AdminDialogs.cpp").read_text(encoding="utf-8")
tools_dialog = (root / "src/dialogs/ToolsDialogs.cpp").read_text(encoding="utf-8")
for required in ("siglaSuffix", "groupOrderEnabled"):
    assert required in models, required
for required in ("m_siglaPlacement", "m_orderEnabled", "m_pendingGroup",
                 "applyConfirmedGroup", '"siglaAfter"', '"orderEnabled"'):
    assert required in group_dialog, required
assert 't == "group_set_ok"' in net_session
assert "groupSetConfirmed" in net_session
assert "plugin_data_scope" in net_session
assert 'QStringLiteral("pluginData")' in group_dialog
assert 'QStringLiteral("pluginDataGlobal")' in group_dialog
assert "Qt::ScrollBarAlwaysOn" in tools_dialog
assert "Qt::ElideNone" in tools_dialog
assert "u.siglaSuffix" in tree_widget
assert "uA.groupOrderEnabled" in tree_widget
assert 'm["op"] = remove ? QStringLiteral("remove") : QStringLiteral("add")' in net_session

# Icon fixes, restart on language change and temporary channel target.
assert "m_restartAfterClose" in main_window
assert "MainWindow::~MainWindow()" in main_window
assert main_window.count("QProcess::startDetached") == 1
assert main_window.index("MainWindow::~MainWindow()") < main_window.index("QProcess::startDetached")
assert "if (!m_restartAfterClose && !m_closingAfterSound" in main_window
assert 'S::flag("app/minimizeToTray", false)' in main_window
options_dialog = (root / "src/dialogs/OptionsDialog.cpp").read_text(encoding="utf-8")
assert "emit languageChanged" in options_dialog
assert "restartForLanguage = true" in main_window
assert "dlg.accept()" in main_window
assert "tempChannelParent" in models
channel_dialog = (root / "src/dialogs/ChannelDialog.cpp").read_text(encoding="utf-8")
assert 'QStringLiteral("pluginData")' in channel_dialog
assert "Receber canais temporários como subcanais" in channel_dialog
assert "temporaryOwnerUid" in models
assert "setTemporaryOwnerMode" in channel_dialog
assert "limitedTemporaryOwner" in server_tab
assert 'limited["bitrate"]' in server_tab
assert 'limited["max"]' in server_tab
assert '{ QStringLiteral("view"), tr("Ver canal") }' in channel_dialog
assert "continues being the authority" in channel_dialog
assert "m_tempChannelParent->setEnabled(!temporary)" in channel_dialog
assert 't == "privilege_granted"' in net_session
assert 'm_myPerms[QStringLiteral("*")] = true' in net_session
assert 'o["tempParent"]' in server_tab
assert 'contains("tempParent")' in net_session
# v6 E2EE -- the server no longer knows any channel key: the clients generate
# and wrap them. Regressing any of these points goes back to the "false
# promise" E2EE model (the server inside the trust boundary).
for required in ("e2eeBootstrap", "e2eeIsMasterOfComponent", "e2eeRotateComponentKey",
                 "e2eeHandleKeyEnvelope", "e2eeHandleKeyRequest", "e2eeRequestKey",
                 "e2eeOnUserLeft", "e2eeOnUserMoved", "e2eeOnTopologyChanged",
                 "e2eeDistributeWhisperKey", "e2eeClearState", "onE2eeHousekeeping"):
    assert required in net_session, required
# welcome must not accept channelKeys again; a channel_key coming from the
# server is refused with a security warning (v6: impossible -- the server has
# no keys).
assert 'obj["channelKeys"].toObject()' not in net_session
assert 't == "channel_key"' in net_session
assert 'e2eeSecurityNotice' in net_session
# E2EE primitives consolidated in a single module (no scattered EVP).
e2ee_crypto = (root / "src/core/E2eeCrypto.cpp").read_text(encoding="utf-8")
for required in ('envelopeWrap', 'envelopeUnwrap', 'pairwiseEncrypt',
                 'pairwiseDecrypt', 'hkdfSha256', 'sasCode', 'verifyDhBinding',
                 'dhBindingMessage', 'dhPublicFromPrivate'):
    assert required in e2ee_crypto, required
assert "halla-app-icon.png" in (root / "src/gui/Icons.cpp").read_text(encoding="utf-8")
identity_dialog = (root / "src/dialogs/IdentityDialog.cpp").read_text(encoding="utf-8")
# v6 E2EE: the per-identity X25519 pair is MANDATORY (the v6 login refuses
# without dhPub/dhSig) -- vault with a local fallback, backup carrying an
# encrypted "dh" field and automatic migration of v5 identities.
for required in ('ensureDhKeyPair', 'dhPrivateKeyForUid', 'dhPublicKeyForUid',
                 'dhSignatureForUid', 'QStringLiteral("dhPrivate")'):
    assert required in identity_dialog, required
# Comment-free version: the prohibitions below apply to CODE -- the comments
# document exactly the forbidden functions, and naming them in the explanation
# must not violate the policy.
import re as _re
_identity_code = _re.sub(r"//[^\n]*", "", identity_dialog)
_identity_code = _re.sub(r"/\*.*?\*/", "", _identity_code, flags=_re.S)
# An identity is never born without an ID: when the system vault
# (qtkeychain/Credential Manager) is unavailable, the private key falls back to
# the legacy local storage.
assert 'a chave privada será guardada apenas no perfil local' in identity_dialog
assert 'S::set(keyBase(uid, QStringLiteral("privateDer")), QString::fromLatin1(priv.toBase64()))' in identity_dialog
# signNonce() uses the local material even when the re-migration to the vault
# fails again -- without that, the login challenge failed on machines with a
# blocked vault even though the key was present in the local profile.
# (Invariant keyed on the English comment now that the code comments are
# English-only; the behavior is identical.)
assert 'local material EVEN IF the re-write to the vault fails' in identity_dialog
assert 'if (!legacy.isEmpty() && SecureStore::write(privateKeyName, legacy)) {' not in identity_dialog
# BoringSSL (embedded in the Windows WebRTC SDK) does NOT implement
# i2d_PrivateKey for Ed25519 (only RSA/EC/DSA -- returns -1): the private key
# is persisted as a raw seed through EVP_PKEY_get_raw_private_key. Without it
# EVERY identity was born with an empty unique ID and every login fell into
# bad_identity on the WebRTC build.
assert 'EVP_PKEY_get_raw_private_key(key' in identity_dialog
assert 'i2d_PrivateKey(' not in identity_dialog
assert 'd2i_AutoPrivateKey(nullptr, &p, priv.size())' in identity_dialog
# Identity crypto is 100% NID-FREE: the Windows build compiles against the
# vcpkg OpenSSL 3 headers (NID_ED25519=1087) but LINKS the BoringSSL inside
# webrtc.lib (NID_ED25519=949) -- ANY call taking the header NID
# (EVP_PKEY_CTX_new_id, EVP_PKEY_new_raw_private_key, EVP_PKEY_id -- which does
# not even exist as a symbol in OpenSSL 3) returns UNSUPPORTED_ALGORITHM on
# the WebRTC build and no identity is created (bug v1.1.0-v1.1.2, confirmed at
# runtime by the CI smoke). The whole sequence uses OID-identified paths:
# RAND_bytes (seed) + d2i_AutoPrivateKey over the minimal RFC 8410 PKCS#8
# (kPkcs8SeedHeader) + i2d_PUBKEY + EVP_DigestSign/Verify.
assert 'kPkcs8SeedHeader' in _identity_code
assert 'RAND_bytes(seed, sizeof(seed)) != 1' in _identity_code
assert 'EVP_PKEY_CTX_new_id' not in _identity_code
assert 'EVP_PKEY_new_raw_private_key' not in _identity_code
assert 'EVP_PKEY_id' not in _identity_code
assert 'EVP_PKEY_get_id' not in _identity_code
# Identity recovery (v1.1.5): the unique ID is base64(SHA-256(public key)) --
# the SAME computation the server performs -- and the private key survives in
# the system vault (qtkeychain/Credential Manager) when the Windows
# Windows is cleaned. "Restore..." re-derives the public key from the private
# one, CHECKS the hash against the pasted ID and re-registers it in the
# profile: roles and permissions come back without generating a new ID.
# Without that check, material swapped in the vault would register an identity
# whose ID does not match the keys.
for required in ('uidForPublicDer', 'privateMaterialForUid', 'keyFromMaterial',
                 'restoreIdentityFromVault', 'vaultIdentityUids',
                 'CredEnumerateW', 'exportIdentityBackupFile',
                 'importIdentityBackupFile'):
    assert required in _identity_code, required
assert 'uidForPublicDer(pub) != uid' in _identity_code
# The portable backup uses the SAME format as Halla Mobile
# ("halla-identity-backup" v1: PBKDF2-HMAC-SHA256 + AES-256-GCM with AAD
# alias|algorithm|public key) -- files cross Desktop <-> Mobile in either
# direction. The NID policy applies here too: only APIs whose SIGNATURE is
# identical in OpenSSL and BoringSSL.
#
# PKCS5_PBKDF2_HMAC and HMAC()/HMAC_Init_ex are FORBIDDEN: OpenSSL 3 declares
# (int passlen, int saltlen, int iter, int keylen) while the BoringSSL inside
# webrtc.lib implements (size_t, size_t, uint32_t, size_t). Compiling against
# the OpenSSL headers and linking BoringSSL turns passlen=-1 (the OpenSSL
# "strlen" convention) into 4294967295 on the BoringSSL size_t -- an access
# violation with no output at all (run 33026636018; the cl stderr was swallowed
# by pwsh and only surfaced with cmd redirection in run 33028370189). PBKDF2
# is therefore written by hand on top of
# EVP_DigestInit_ex/EVP_DigestUpdate(size_t in both)/EVP_DigestFinal_ex/
# EVP_sha256 -- identical ABI in both libraries -- and the smoke checks the
# result against the public RFC 8018 vectors.
for required in ('halla-identity-backup', 'pbkdf2HmacSha256', 'hmacSha256',
                 'EVP_DigestInit_ex', 'EVP_DigestUpdate', 'EVP_sha256',
                 'EVP_aes_256_gcm',
                 'EVP_CTRL_GCM_GET_TAG', 'EVP_CTRL_GCM_SET_TAG',
                 'OPENSSL_cleanse'):
    assert required in _identity_code, required
for forbidden in ('PKCS5_PBKDF2_HMAC', 'HMAC(', 'HMAC_Init_ex', 'HMAC_Update', 'HMAC_Final',
                  'HMAC_CTX_new'):
    assert forbidden not in _identity_code, forbidden
# The smoke mirrors the hand-written PBKDF2 and checks the RFC 8018 vectors --
# that is what keeps the backup byte-for-byte compatible with the Mobile Java
# implementation.
smoke_src = (root / "tests/ed25519_identity_smoke.cpp").read_text(encoding="utf-8")
_smoke_code = _re.sub(r"//[^\n]*", "", smoke_src)
_smoke_code = _re.sub(r"/\*.*?\*/", "", _smoke_code, flags=_re.S)
for required in ('smokePbkdf2', 'smokeHmacSha256', 'kPbkdf2Vec1', 'kPbkdf2Vec2',
                 'kPbkdf2Vec3', 'smokeHexEq'):
    assert required in _smoke_code, required
assert 'PKCS5_PBKDF2_HMAC' not in _smoke_code, 'PKCS5_PBKDF2_HMAC forbidden in the smoke'
# v1.1.9 -- a custom banner must not block splitter dragging: a QLabel with
# scaledContents uses the PIXMAP size as minimumSizeHint (a 1600 px banner
# made the information card 1600 px wide -- the QSplitter refused to drag and
# the panel covered the channel list; even the default 820 px banner limited
# the drag).
info_panel = (root / "src/gui/InfoPanel.cpp").read_text(encoding="utf-8")
assert "m_banner->setMinimumWidth(1);" in info_panel
# Whoever connects AFTER someone started a broadcast must see the LIVE badge:
# welcome/user_joined already carry "screensharing" (toJson) -- the field was
# simply not read here; only the start/stop broadcast updated the state.
assert 'usr.screensharing = u["screensharing"].toBool();' in net_session
# The icon file name is an internal detail: the tooltip shows the IMAGE (a
# file:// of the on-disk cache) plus the role name, and the "<icon> <name>"
# parsing lives in GroupIconCache -- shared with the information panel.
group_icon_cache = (root / "src/core/GroupIconCache.cpp").read_text(encoding="utf-8")
group_icon_header = (root / "src/core/GroupIconCache.h").read_text(encoding="utf-8")
assert "splitRoleLine" in group_icon_cache and "splitRoleLine" in group_icon_header
assert "iconFilePath" in group_icon_cache and "iconFilePath" in group_icon_header
assert "u.serverGroups.toHtmlEscaped()" not in tree_widget, \
    "tooltip must use splitRoleLine (never print the raw server line)"
# Larger role icons: 16x14 -> 24x21 (image cache and the tree letter/emoji
# fallback use the same size).
assert group_icon_cache.count("24, 21, Qt::KeepAspectRatio") == 2
assert "QPixmap pm(24, 21);" in tree_widget
runpy.run_path(str(root / "tests/icon_audit.py"), run_name="__main__")
runpy.run_path(str(root / "tests/plugin_audit.py"), run_name="__main__")
runpy.run_path(str(root / "tests/translation_audit.py"), run_name="__main__")
print(f"Halla repository sanity OK: {version}")
