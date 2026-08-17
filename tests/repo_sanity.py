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
assert "m_closeDelayPending" in main_window and "QTimer::singleShot(1000" in main_window
assert "m_intentionalDisconnect" in net_session and "waitForBytesWritten" not in net_session
assert 'HSound::play(QStringLiteral("moved"))' in server_tab
assert "Silenciar todos os avisos de áudio" in (root / "src/dialogs/OptionsDialog.cpp").read_text(encoding="utf-8")
webrtc = (root / "src/webrtc/HallaWebRtcSession.cpp").read_text(encoding="utf-8")
assert "PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE" in webrtc
assert "TargetProcessId = GetCurrentProcessId()" in webrtc
assert "GetDefaultAudioEndpoint" not in webrtc
# Regressões Desktop↔Mobile: ICE precoce deve ser enfileirado, a live remota só
# abre por clique explícito e o UDP usa o peer realmente escolhido pelo TLS.
assert "pendingRemoteIce" in webrtc and "remoteDescriptionReady" in webrtc
assert "if (userId != tab->data().selfId) return" in main_window
assert "openScreenShareWindow(userId)" in main_window
assert "normalizedPeerAddress(m_tcp->peerAddress())" in net_session
assert "for (const QByteArray& key : m_channelKeys)" in net_session

models = (root / "src/core/Models.h").read_text(encoding="utf-8")
group_dialog = (root / "src/dialogs/AdminDialogs.cpp").read_text(encoding="utf-8")
tools_dialog = (root / "src/dialogs/ToolsDialogs.cpp").read_text(encoding="utf-8")
tree_widget = (root / "src/gui/ServerTreeWidget.cpp").read_text(encoding="utf-8")
for required in ("siglaSuffix", "groupOrderEnabled"):
    assert required in models, required
for required in ("m_siglaPlacement", "m_orderEnabled", "m_pendingGroup",
                 "applyConfirmedGroup", '"siglaAfter"', '"orderEnabled"'):
    assert required in group_dialog, required
assert 't == "group_set_ok"' in net_session
assert "groupSetConfirmed" in net_session
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
assert "Receber canais temporários como subcanais" in channel_dialog
assert '{ QStringLiteral("view"), tr("Ver canal") }' in channel_dialog
assert "O servidor continua sendo a autoridade" in channel_dialog
assert "m_tempChannelParent->setEnabled(!temporary)" in channel_dialog
assert 't == "privilege_granted"' in net_session
assert 'm_myPerms[QStringLiteral("*")] = true' in net_session
assert 'o["tempParent"]' in server_tab
assert 'contains("tempParent")' in net_session
assert "halla-app-icon.png" in (root / "src/gui/Icons.cpp").read_text(encoding="utf-8")
runpy.run_path(str(root / "tests/icon_audit.py"), run_name="__main__")
runpy.run_path(str(root / "tests/plugin_audit.py"), run_name="__main__")
runpy.run_path(str(root / "tests/translation_audit.py"), run_name="__main__")
print(f"Halla repository sanity OK: {version}")
