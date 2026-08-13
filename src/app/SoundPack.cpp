#include "SoundPack.h"
#include "core/Settings.h"

#include <QStandardPaths>
#include <QDir>
#include <QFile>
#include <QSaveFile>
#include <QHash>
#include <QUrl>
#include <QSoundEffect>
#include <QMediaPlayer>
#include <QAudioOutput>
#include <QApplication>
#include <QFileInfo>
#include <QtMath>
#include <cstring>

namespace HSound {

QString dir() {
    QString d = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)
                + QStringLiteral("/sounds");
    QDir().mkpath(d);
    return d;
}

// ------------------------------------------------------------------ WAV 16-bit
// escreve um WAV mono 48 kHz com uma sequência de tons (freq, ms)
static bool writeWav(const QString& path, const QList<QPair<int,int>>& seq, int volPct = 70) {
    const int rate = 48000;
    int total = 0;
    for (const auto& s : seq) total += rate * s.second / 1000;
    // 60 ms de silêncio entre tons
    const int gap = rate * 60 / 1000;
    total += gap * (seq.size() - 1);

    QByteArray pcm(total * 2, 0);
    int16_t* out = reinterpret_cast<int16_t*>(pcm.data());
    int pos = 0;
    for (const auto& s : seq) {
        const int n = rate * s.second / 1000;
        const double w = 2.0 * M_PI * s.first / rate;
        for (int i = 0; i < n; ++i) {
            // envelope suave (8 ms) p/ evitar cliques
            const int env = qMin(i, n - i - 1);
            const double e = qMin(1.0, env / (rate * 0.008));
            out[pos + i] = int16_t(std::sin(w * i) * e * 32767 * (volPct / 100.0));
        }
        pos += n + gap;
    }

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return false;
    QByteArray h(44, 0);
    std::memcpy(h.data() + 0,  "RIFF", 4);
    std::memcpy(h.data() + 8,  "WAVEfmt ", 8);
    quint32 fmtLen = 16, dataLen = quint32(pcm.size()), riffLen = 36 + dataLen;
    quint16 audioFmt = 1, ch = 1;
    quint32 r = rate, byteRate = rate * 2;
    quint16 align = 2, bits = 16;
    std::memcpy(h.data() + 4,  &riffLen, 4);
    std::memcpy(h.data() + 16, &fmtLen, 4);
    std::memcpy(h.data() + 20, &audioFmt, 2);
    std::memcpy(h.data() + 22, &ch, 2);
    std::memcpy(h.data() + 24, &r, 4);
    std::memcpy(h.data() + 28, &byteRate, 4);
    std::memcpy(h.data() + 32, &align, 2);
    std::memcpy(h.data() + 34, &bits, 2);
    std::memcpy(h.data() + 36, "data", 4);
    std::memcpy(h.data() + 40, &dataLen, 4);
    f.write(h);
    f.write(pcm);
    return true;
}

static bool copyBundledSound(const QString& eventName, const QString& destination) {
    QFile source(QStringLiteral(":/halla/assets/sounds/") + eventName + QStringLiteral(".wav"));
    if (!source.open(QIODevice::ReadOnly)) return false;
    const QByteArray bytes = source.readAll();
    if (bytes.size() < 44 || !bytes.startsWith("RIFF") || bytes.mid(8, 4) != "WAVE") return false;
    QSaveFile output(destination);
    if (!output.open(QIODevice::WriteOnly)) return false;
    if (output.write(bytes) != bytes.size()) return false;
    return output.commit();
}

void ensure() {
    const QString d = dir();

    // Os arquivos fornecidos para o pacote oficial substituem uma única vez
    // os tons sintéticos antigos. Depois disso, arquivos personalizados pelo
    // usuário são preservados; arquivos apagados são restaurados do recurso.
    static const QString packVersion = QStringLiteral("official-voice-v3");
    static const QStringList bundled = {
        QStringLiteral("banned"),
        QStringLiteral("connected"),
        QStringLiteral("connection_lost"),
        QStringLiteral("disconnected"),
        QStringLiteral("error"),
        QStringLiteral("insufficient_permissions"),
        QStringLiteral("kicked"),
        QStringLiteral("mic_muted"),
        QStringLiteral("mic_unmuted"),
        QStringLiteral("moved"),
        QStringLiteral("poke"),
        QStringLiteral("sound_muted"),
        QStringLiteral("sound_resumed"),
        QStringLiteral("user_joined"),
        QStringLiteral("user_left")
    };
    const QString markerPath = d + QStringLiteral("/.pack-version");
    QFile marker(markerPath);
    QString installedVersion;
    if (marker.open(QIODevice::ReadOnly)) installedVersion = QString::fromUtf8(marker.readAll()).trimmed();
    const bool upgrade = installedVersion != packVersion;
    bool installed = true;
    for (const QString& eventName : bundled) {
        const QString destination = d + QLatin1Char('/') + eventName + QStringLiteral(".wav");
        if ((upgrade || !QFile::exists(destination)) && !copyBundledSound(eventName, destination))
            installed = false;
    }
    if (installed && upgrade) {
        QSaveFile versionFile(markerPath);
        if (versionFile.open(QIODevice::WriteOnly)) {
            versionFile.write(packVersion.toUtf8());
            versionFile.write("\n");
            versionFile.commit();
        }
    }

    static const QHash<QString, QList<QPair<int,int>>> presets = {
        { QStringLiteral("connected"),    { {520, 70}, {660, 70}, {880, 110} } },
        { QStringLiteral("disconnected"), { {660, 70}, {440, 140} } },
        { QStringLiteral("user_joined"),  { {587, 55}, {784, 85} } },
        { QStringLiteral("user_left"),    { {784, 55}, {587, 85} } },
        { QStringLiteral("message"),      { {988, 45}, {1319, 70} } },
        { QStringLiteral("poke"),         { {880, 40}, {880, 40}, {880, 110} } },
        { QStringLiteral("mic_muted"),    { {494, 90} } },
        { QStringLiteral("mic_unmuted"),  { {740, 90} } },
        { QStringLiteral("recording"),    { {988, 50}, {988, 100} } },
        { QStringLiteral("test"),         { {660, 70}, {880, 110} } },
    };
    for (auto it = presets.constBegin(); it != presets.constEnd(); ++it) {
        const QString p = d + QLatin1Char('/') + it.key() + QStringLiteral(".wav");
        if (!QFile::exists(p)) writeWav(p, it.value());
    }
}

void play(const QString& name) {
    if (!S::flag("notify/soundsEnabled", true)) return;
    ensure();
    static QHash<QString, QSoundEffect*> fx;
    QSoundEffect* e = fx.value(name, nullptr);
    if (!e) {
        e = new QSoundEffect;
        e->setSource(QUrl::fromLocalFile(dir() + QLatin1Char('/') + name +
                                         QStringLiteral(".wav")));
        fx.insert(name, e);
    }
    // Volume do pacote de som (Opções → Reprodução), armazenado em dB ×10.
    // Aplicado a cada reprodução: mudanças nas opções valem imediatamente.
    {
        const double db = S::num("playback/soundPackVolume", -170) / 10.0;
        e->setVolume(float(qBound(0.0, qPow(10.0, db / 20.0), 1.0)));
    }
    if (e->isLoaded()) e->play();
    else if (e->status() == QSoundEffect::Error) { // sem áudio: ignora silenciosamente
    } else {
        // ainda carregando: toca assim que estiver pronto
        QObject::connect(e, &QSoundEffect::loadedChanged, e, [e] { e->play(); },
                         Qt::SingleShotConnection);
    }
}

void playFile(const QString& path) {
    if (path.isEmpty() || !QFileInfo::exists(path)) return;

    struct FileCue {
        QMediaPlayer* player = nullptr;
        QAudioOutput* output = nullptr;
    };
    static QHash<QString, FileCue> cues;

    FileCue& cue = cues[path];
    if (!cue.player) {
        cue.output = new QAudioOutput(qApp);
        cue.player = new QMediaPlayer(qApp);
        cue.player->setAudioOutput(cue.output);
    }

    const double db = S::num("playback/soundPackVolume", -170) / 10.0;
    cue.output->setVolume(float(qBound(0.0, qPow(10.0, db / 20.0), 1.0)));
    cue.player->stop();
    cue.player->setSource(QUrl::fromLocalFile(path));
    cue.player->play();
}

} // namespace HSound
