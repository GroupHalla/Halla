from pathlib import Path
import json
import re

root = Path(__file__).resolve().parents[1]
api = (root / "sdk/halla_plugin_api.h").read_text(encoding="utf-8")
sdk_license = (root / "sdk/LICENSE.txt").read_text(encoding="utf-8")
manager = (root / "src/plugins/PluginManager.cpp").read_text(encoding="utf-8")
overlay = (root / "src/plugins/TalkingOverlay.cpp").read_text(encoding="utf-8")
radio = (root / "src/plugins/RadioVoiceEffect.cpp").read_text(encoding="utf-8")
radio_test = (root / "tests/radio_voice_smoke.cpp").read_text(encoding="utf-8")
options = (root / "src/dialogs/OptionsDialog.cpp").read_text(encoding="utf-8")
docs = (root / "docs/PLUGINS.md").read_text(encoding="utf-8")
packager = (root / "tools/package_plugin.py").read_text(encoding="utf-8")
manifest = json.loads((root / "examples/plugins/hello_world/manifest.json").read_text(encoding="utf-8"))
advanced_manifest = json.loads((root / "examples/plugins/advanced_sdk/manifest.json").read_text(encoding="utf-8"))
advanced_example = (root / "examples/plugins/advanced_sdk/advanced_sdk.cpp").read_text(encoding="utf-8")
voice = (root / "src/net/VoiceEngine.cpp").read_text(encoding="utf-8")
net = (root / "src/net/NetSession.cpp").read_text(encoding="utf-8")
main_window = (root / "src/app/MainWindow.cpp").read_text(encoding="utf-8")
installer = (root / "packaging/halla-setup.nsi").read_text(encoding="utf-8")
catalog = json.loads((root / "addons/catalog.json").read_text(encoding="utf-8"))

assert "HALLA_PLUGIN_ABI_VERSION 1u" in api
assert "Permission is hereby granted" in sdk_license
assert "HallaHostApi" in api and "HallaPluginApi" in api
assert "halla_plugin_entry" in api
assert "get_settings_json" in api and "request_client_state" in api
for interface in ("HALLA_INTERFACE_CORE_V1", "HALLA_INTERFACE_CONNECTION_V1",
                  "HALLA_INTERFACE_AUDIO_V1", "HALLA_INTERFACE_DATA_V1",
                  "HALLA_INTERFACE_UI_V1"):
    assert interface in api, interface
for required in ("HallaAudioFrame", "register_processor", "set_listener_transform",
                 "set_user_radio_effect", "play_pcm", "get_connections_json", "send_chat",
                 "move_user", "create_channel_json", "set_receive_handler",
                 "register_hotkey", "query_interface"):
    assert required in api, required
assert "HALLA_PLUGIN_API_BASE_SIZE" in api and "HALLA_HOST_API_BASE_SIZE" in api
assert "QLibrary" in manager and "ResolveAllSymbolsHint" in manager
assert "QCryptographicHash::Sha256" in manager
assert "NoLessSafeRedirectPolicy" in manager
assert "O pacote contém caminhos inseguros" in manager
assert "100ll * 1024 * 1024" in manager
assert "250ll * 1024 * 1024" in manager
assert "https://raw.githubusercontent.com/GroupHalla/Halla/main/addons/catalog.json" in manager
assert "official.talking-overlay" in manager
assert "official.radio-voice" in manager
assert "HALLA_AUDIO_FLAG_WHISPER" in api
assert "sendMode" in manager and "receiveMode" in manager
assert "std::tanh" in radio and "m_noise" in radio
assert "src/plugins/RadioVoiceEffect.cpp" in (root / "CMakeLists.txt").read_text(encoding="utf-8")
assert "Official radio voice DSP smoke OK" in radio_test
assert "HALLA_AUDIO_REMOTE_BEFORE_SPATIAL" in radio_test
assert "WindowTransparentForInput" in overlay
assert "WS_EX_NOACTIVATE" in overlay
assert "Instalar arquivo .halla-addon" in options
assert "Procurar complementos online" in options
assert "Ativo" in options and "Configurar" in options and "Remover" in options
assert "client_state" in docs
assert "HALLA_AUDIO_REMOTE_BEFORE_SPATIAL" in advanced_example
assert "get_connections_json" in advanced_example
assert set(advanced_manifest["capabilities"]) >= {
    "connection.read", "audio.playback", "audio.spatial", "plugin.data",
    "ui.notifications", "ui.actions"
}
assert "QMap<int, OpusDecoder*>" in (root / "src/net/VoiceEngine.h").read_text(encoding="utf-8")
assert "HALLA_AUDIO_MIXED_PLAYBACK" in voice and "spatializeFrame" in voice
assert 't == "plugin_data"' in net and "sendPluginData" in net
assert "m_pluginGlobalHotkeys" in main_window and "RegisterHotKey" in main_window
assert "Plugin SDK\\advanced_sdk" in installer
assert catalog["version"] == 1 and isinstance(catalog["addons"], list)
assert "zipfile.ZIP_DEFLATED" in packager
assert "hashlib.sha256" in packager

assert re.fullmatch(r"[a-z0-9][a-z0-9._-]{2,63}", manifest["id"])
assert manifest["apiVersion"] == 1
assert manifest["type"] == "native"
library = manifest["platforms"]["windows-x64"]["library"]
assert not library.startswith(("/", "\\")) and ".." not in Path(library).parts

print("Plugin API/package audit OK")
