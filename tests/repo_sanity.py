from pathlib import Path
import re

root = Path(__file__).resolve().parents[1]
version = (root / "VERSION").read_text().strip()
assert re.fullmatch(r"\d+\.\d+\.\d+", version), version
assert "@HALLA_VERSION@" in (root / "src/version.h.in").read_text()
assert "@HALLA_VERSION@" in (root / "src/halla.rc.in").read_text()
cmake = (root / "CMakeLists.txt").read_text()
assert "file(STRINGS" in cmake and "VERSION" in cmake
protocol = (root / "src/net/HallaProtocol.h").read_text()
assert "kProtoVersion = 4" in protocol
assert "kVoiceTokenBytes = 16" in protocol
updater = (root / "src/app/MainWindowUpdates.cpp").read_text()
for required in ("QCryptographicHash::Sha256", "verifyAuthenticode", "kMaxInstallerBytes"):
    assert required in updater, required
installer = (root / "packaging/halla-setup.nsi").read_text()
assert "APP_VERSION é obrigatório" in installer
print(f"Halla repository sanity OK: {version}")
