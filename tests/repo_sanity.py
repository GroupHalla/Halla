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
assert "kProtoVersion = 5" in protocol
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
# A tecla PTT só pode virar hotkey global no modo "pressionar para falar"
# (capture/pttMode == 0). Registrá-la nos modos por voz (1, PADRÃO) e
# contínuo (2) engole a tecla do sistema inteiro à toa — com o padrão Space,
# todo usuário em modo por voz perdia a barra de espaço em todos os apps.
assert 'if (S::num("capture/pttMode", 1) != 0) return;' in main_window
assert "m_intentionalDisconnect" in net_session and "waitForBytesWritten" not in net_session
assert 'HSound::play(QStringLiteral("moved"))' in server_tab
assert "Silenciar todos os avisos de áudio" in (root / "src/dialogs/OptionsDialog.cpp").read_text(encoding="utf-8")
webrtc = (root / "src/webrtc/HallaWebRtcSession.cpp").read_text(encoding="utf-8")
assert "PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE" in webrtc
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
assert 'S::flag("screenshare/hardwareEncoder", false)' in webrtc
options_source = (root / "src/dialogs/OptionsDialog.cpp").read_text(encoding="utf-8")
assert "screenshare/hardwareEncoder" in options_source
application_page = options_source[
    options_source.index("QWidget* OptionsDialog::pageApplication()"):
    options_source.index("QWidget* OptionsDialog::pageDesign()")
]
assert "screenshare/hardwareEncoder" in application_page
assert "TargetProcessId = GetCurrentProcessId()" in webrtc
assert "GetDefaultAudioEndpoint" not in webrtc
# Regressões Desktop↔Mobile: ICE precoce deve ser enfileirado, a live remota só
# abre por clique explícito e o UDP usa o peer realmente escolhido pelo TLS.
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
# Áudio da transmissão usa exclusivamente a track WebRTC. O ADM customizado
# precisa puxar o playout para que o libwebrtc decodifique e entregue OnData.
assert "NeedMorePlayData" in webrtc and "playoutLoop" in webrtc
assert "PlayoutIsAvailable" in webrtc and "StartPlayout" in webrtc
assert "RemoteAudioSink" in webrtc and "m_pendingRemoteFrames" in webrtc
assert "playStreamPcm" in main_window and "playStreamPcm" in voice_engine
assert "kStreamPrebufferFrames = 2" in voice_engine
assert "m_streamQueues" in voice_engine and "clearStreamPcm" in voice_engine
# Controles da live aparecem por hover, sobem pela parte inferior e permitem
# mudo individual/parar de assistir sem afetar a chamada.
for required in ("liveControls", "m_audioButton", "stopWatching",
                 "QPropertyAnimation", "isAudioMuted", "FastTransformation"):
    assert required in main_window, required
# Botão compacto inspirado no mockup: pill em gradiente, ícone live desenhado
# por QPainter e cápsula branca com play à direita.
for required in ("class WatchLiveButton", "Assistir Live", "setFixedSize(232, 46)",
                 "QLinearGradient background", "liveOrb", "playCapsule"):
    assert required in main_window, required
assert "if (userId != tab->data().selfId) return" in main_window
assert "openScreenShareWindow(userId)" in main_window
assert "normalizedPeerAddress(m_tcp->peerAddress())" in net_session
assert "for (const QByteArray& key : m_channelKeys)" in net_session
tree_widget = (root / "src/gui/ServerTreeWidget.cpp").read_text(encoding="utf-8")
# Reordenação de canais pode gerar uma rajada de updates. O modelo recebe todos,
# mas a árvore só redesenha uma vez; ciclos/orfãos não entram em recursão infinita.
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

# Correções de ícone, reinício por idioma e destino de canais temporários.
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
assert "O servidor continua sendo a autoridade" in channel_dialog
assert "m_tempChannelParent->setEnabled(!temporary)" in channel_dialog
assert 't == "privilege_granted"' in net_session
assert 'm_myPerms[QStringLiteral("*")] = true' in net_session
assert 'o["tempParent"]' in server_tab
assert 'contains("tempParent")' in net_session
assert "halla-app-icon.png" in (root / "src/gui/Icons.cpp").read_text(encoding="utf-8")
identity_dialog = (root / "src/dialogs/IdentityDialog.cpp").read_text(encoding="utf-8")
# Versão sem comentários: as proibições abaixo valem para CÓDIGO — os
# comentários documentam justamente as funções proibidas, e citar o nome
# delas na explicação não pode violar a política.
import re as _re
_identity_code = _re.sub(r"//[^\n]*", "", identity_dialog)
_identity_code = _re.sub(r"/\*.*?\*/", "", _identity_code, flags=_re.S)
# Identidade nunca nasce sem ID: com o cofre do sistema (qtkeychain/Credential
# Manager) indisponível, a chave privada cai no armazenamento local legado.
assert 'a chave privada será guardada apenas no perfil local' in identity_dialog
assert 'S::set(keyBase(uid, QStringLiteral("privateDer")), QString::fromLatin1(priv.toBase64()))' in identity_dialog
# signNonce() usa o material local mesmo quando a re-migração para o cofre
# volta a falhar — sem isso o desafio de login falhava em máquinas com o
# cofre bloqueado mesmo com a chave presente no perfil local.
assert 'usa o material local MESMO' in identity_dialog
assert 'if (!legacy.isEmpty() && SecureStore::write(privateKeyName, legacy)) {' not in identity_dialog
# BoringSSL (embutido no SDK WebRTC do build Windows) NÃO implementa
# i2d_PrivateKey para Ed25519 (só RSA/EC/DSA — devolve -1): a chave privada é
# persistida como seed crua via EVP_PKEY_get_raw_private_key. Sem isso TODA
# identidade nascia com ID único vazio e todo login caía em bad_identity no
# build WebRTC.
assert 'EVP_PKEY_get_raw_private_key(key' in identity_dialog
assert 'i2d_PrivateKey(' not in identity_dialog
assert 'd2i_AutoPrivateKey(nullptr, &p, priv.size())' in identity_dialog
# A cripto de identidade é 100% LIVRE DE NID: o build Windows compila com os
# headers do OpenSSL 3 do vcpkg (NID_ED25519=1087) mas LINKA o BoringSSL do
# webrtc.lib (NID_ED25519=949) — QUALQUER chamada que receba o NID do header
# (EVP_PKEY_CTX_new_id, EVP_PKEY_new_raw_private_key, EVP_PKEY_id — que nem
# existe como símbolo no OpenSSL 3) devolve UNSUPPORTED_ALGORITHM no build
# WebRTC e nenhuma identidade é criada (bug v1.1.0–v1.1.2, confirmado em
# runtime pelo smoke do CI). Toda a sequência usa caminhos identificados por
# OID: RAND_bytes (seed) + d2i_AutoPrivateKey sobre o PKCS#8 mínimo do
# RFC 8410 (kPkcs8SeedHeader) + i2d_PUBKEY + EVP_DigestSign/Verify.
assert 'kPkcs8SeedHeader' in _identity_code
assert 'RAND_bytes(seed, sizeof(seed)) != 1' in _identity_code
assert 'EVP_PKEY_CTX_new_id' not in _identity_code
assert 'EVP_PKEY_new_raw_private_key' not in _identity_code
assert 'EVP_PKEY_id' not in _identity_code
assert 'EVP_PKEY_get_id' not in _identity_code
runpy.run_path(str(root / "tests/icon_audit.py"), run_name="__main__")
runpy.run_path(str(root / "tests/plugin_audit.py"), run_name="__main__")
runpy.run_path(str(root / "tests/translation_audit.py"), run_name="__main__")
print(f"Halla repository sanity OK: {version}")
