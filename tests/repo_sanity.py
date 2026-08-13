from pathlib import Path
import re
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
assert "kProtoVersion = 4" in protocol
assert "kVoiceTokenBytes = 16" in protocol
updater = (root / "src/app/MainWindowUpdates.cpp").read_text(encoding="utf-8")
for required in ("QCryptographicHash::Sha256", "verifyAuthenticode", "kMaxInstallerBytes"):
    assert required in updater, required
installer = (root / "packaging/halla-setup.nsi").read_text(encoding="utf-8")
assert "APP_VERSION é obrigatório" in installer

sound_names = (
    "connected", "connection_lost", "error", "insufficient_permissions",
    "mic_unmuted", "poke", "sound_muted", "sound_resumed",
    "user_joined", "user_left",
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
print(f"Halla repository sanity OK: {version}")
