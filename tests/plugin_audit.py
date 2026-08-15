from pathlib import Path
import json
import re

root = Path(__file__).resolve().parents[1]
api = (root / "sdk/halla_plugin_api.h").read_text(encoding="utf-8")
sdk_license = (root / "sdk/LICENSE.txt").read_text(encoding="utf-8")
manager = (root / "src/plugins/PluginManager.cpp").read_text(encoding="utf-8")
overlay = (root / "src/plugins/TalkingOverlay.cpp").read_text(encoding="utf-8")
options = (root / "src/dialogs/OptionsDialog.cpp").read_text(encoding="utf-8")
docs = (root / "docs/PLUGINS.md").read_text(encoding="utf-8")
packager = (root / "tools/package_plugin.py").read_text(encoding="utf-8")
manifest = json.loads((root / "examples/plugins/hello_world/manifest.json").read_text(encoding="utf-8"))
catalog = json.loads((root / "addons/catalog.json").read_text(encoding="utf-8"))

assert "HALLA_PLUGIN_ABI_VERSION 1u" in api
assert "Permission is hereby granted" in sdk_license
assert "HallaHostApi" in api and "HallaPluginApi" in api
assert "halla_plugin_entry" in api
assert "get_settings_json" in api and "request_client_state" in api
assert "QLibrary" in manager and "ResolveAllSymbolsHint" in manager
assert "QCryptographicHash::Sha256" in manager
assert "NoLessSafeRedirectPolicy" in manager
assert "O pacote contém caminhos inseguros" in manager
assert "100ll * 1024 * 1024" in manager
assert "250ll * 1024 * 1024" in manager
assert "https://raw.githubusercontent.com/GroupHalla/Halla/main/addons/catalog.json" in manager
assert "official.talking-overlay" in manager
assert "WindowTransparentForInput" in overlay
assert "WS_EX_NOACTIVATE" in overlay
assert "Instalar arquivo .halla-addon" in options
assert "Procurar complementos online" in options
assert "Ativo" in options and "Configurar" in options and "Remover" in options
assert "client_state" in docs
assert catalog["version"] == 1 and isinstance(catalog["addons"], list)
assert "zipfile.ZIP_DEFLATED" in packager
assert "hashlib.sha256" in packager

assert re.fullmatch(r"[a-z0-9][a-z0-9._-]{2,63}", manifest["id"])
assert manifest["apiVersion"] == 1
assert manifest["type"] == "native"
library = manifest["platforms"]["windows-x64"]["library"]
assert not library.startswith(("/", "\\")) and ".." not in Path(library).parts

print("Plugin API/package audit OK")
