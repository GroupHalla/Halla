#include "VoiceEngine.h"
#include "NetSession.h"
#include "core/Models.h"
#include "core/AppLog.h"
#include "core/Settings.h"

#include <QAudioSource>
#include <QAudioSink>
#include <QAudioFormat>
#include <QMediaDevices>
#include <QTimer>
#include <QFile>
#include <QtMath>
#include <cstring>

extern "C" {
#include <opus.h>
}

// ------------------------------------------------------------------ construção
VoiceEngine::VoiceEngine(NetSession* net, ServerData* data, QObject* parent)
    : QObject(parent), m_net(net), m_data(data) {

    // ---- Opus
    int err = 0;
    m_encoder = opus_encoder_create(48000, 1, OPUS_APPLICATION_VOIP, &err);
    m_decoder = opus_decoder_create(48000, 1, &err);
    if (m_encoder) {
        opus_encoder_ctl(m_encoder, OPUS_SET_BITRATE(32000));
        opus_encoder_ctl(m_encoder, OPUS_SET_VBR(1));
        opus_encoder_ctl(m_encoder, OPUS_SET_DTX(1)); // suprime quadros de silêncio

        // Registra o endpoint UDP do PC com um frame Opus válido. Alguns
        // relays antigos ignoram o pacote HALL de 10 bytes sem payload; sem
        // este aquecimento, o PC só se torna destinatário depois de falar.
        int16_t silence[960] = {};
        unsigned char registration[512];
        // DTX pode suprimir silêncio e produzir um payload vazio. Durante o
        // aquecimento do endpoint UDP forçamos um frame Opus não vazio.
        opus_encoder_ctl(m_encoder, OPUS_SET_DTX(0));
        const int encoded = opus_encode(m_encoder, silence, 960,
                                        registration, sizeof(registration));
        opus_encoder_ctl(m_encoder, OPUS_SET_DTX(1));
        if (encoded > 0 && m_net) {
            for (quint16 seq = 1; seq <= 3; ++seq)
                m_net->sendVoiceFrame(
                    QByteArray(reinterpret_cast<const char*>(registration), encoded), seq);
        }
        // Reenvia periodicamente para atravessar NATs e perdas de pacotes no
        // primeiro instante. Sem isso, o primeiro áudio podia depender de o
        // PC falar antes para registrar novamente seu endpoint UDP.
        m_endpointTimer = new QTimer(this);
        m_endpointTimer->setInterval(2000);
        connect(m_endpointTimer, &QTimer::timeout, this, &VoiceEngine::sendEndpointRegistration);
        m_endpointTimer->start();
    }

    QAudioFormat fmt;
    fmt.setSampleRate(48000);
    fmt.setChannelCount(1);
    fmt.setSampleFormat(QAudioFormat::Int16);

    QAudioDevice inDev = QMediaDevices::defaultAudioInput();
    const QString savedInId = S::str("capture/device");
    if (!savedInId.isEmpty()) {
        const auto inputs = QMediaDevices::audioInputs();
        for (const QAudioDevice& input : inputs) {
            if (input.id() == savedInId) {
                inDev = input;
                break;
            }
        }
    }

    QAudioDevice outDev = QMediaDevices::defaultAudioOutput();
    const QString savedOutId = S::str("playback/device");
    if (!savedOutId.isEmpty()) {
        const auto outputs = QMediaDevices::audioOutputs();
        for (const QAudioDevice& output : outputs) {
            if (output.id() == savedOutId) {
                outDev = output;
                break;
            }
        }
    }

    if (!inDev.isNull()) {
        m_source = new QAudioSource(inDev, fmt, this);
        m_srcDev = m_source->start();
        m_captureBuf.reserve(960 * 2 * 2);

        m_capTimer = new QTimer(this);
        m_capTimer->setInterval(10);
        connect(m_capTimer, &QTimer::timeout, this, &VoiceEngine::captureTick);
        m_capTimer->start();
    } else {
        AppLog::warn(QStringLiteral("Nenhum dispositivo de captura de áudio encontrado"));
    }

    if (!outDev.isNull()) {
        m_sink = new QAudioSink(outDev, fmt, this);
        m_sink->setBufferSize(960 * 2 * 20); // ~400 ms
        m_sinkDev = m_sink->start();

        m_playTimer = new QTimer(this);
        m_playTimer->setInterval(10);
        connect(m_playTimer, &QTimer::timeout, this, &VoiceEngine::playbackTick);
        m_playTimer->start();
    } else {
        AppLog::warn(QStringLiteral("Nenhum dispositivo de reprodução de áudio encontrado"));
    }

    m_active = (m_encoder != nullptr);
    if (m_active)
        AppLog::info(QStringLiteral("Motor de voz ativo (Opus 48 kHz mono, 20 ms)"));

    connect(m_net, &NetSession::voicePacketReceived, this,
            [this](int fromId, quint16, const QByteArray& payload) {
                if (payload.isEmpty() || !m_decoder) return;
                // decodifica e enfileira PCM do falante
                int16_t pcm[960];
                int n = opus_decode(m_decoder,
                                    reinterpret_cast<const unsigned char*>(payload.constData()),
                                    payload.size(), pcm, 960, 0);
                if (n <= 0) return;
                ++m_opusReceived;
                m_opusReceivedBytes += quint64(payload.size());
                // volume por usuário (dB -> fator linear)
                int vol = 0;
                bool muted = false;
                if (m_data && m_data->users.contains(fromId)) {
                    vol = m_data->users[fromId].volumeDb;
                    muted = m_data->users[fromId].locallyMuted;
                }
                if (muted) return;
                float gain = vol == 0 ? 1.0f : qPow(10.0f, vol / 20.0f);
                // volume MESTRE de reprodução (Opções > Reprodução), em dB x10
                const int masterX10 = S::num("playback/volumeDb", 0);
                if (masterX10 != 0) gain *= qPow(10.0f, (masterX10 / 10.0f) / 20.0f);
                // DUCKING (Opções > Capturar > DSP): reduz a reprodução em N dB
                // enquanto EU estiver transmitindo (evita realimentação de eco)
                if (m_talking && S::flag("capture/ducking", false))
                    gain *= qPow(10.0f, -float(S::num("capture/duckingDb", 10)) / 20.0f);
                if (gain != 1.0f) {
                    for (int i = 0; i < n; ++i) {
                        float s = pcm[i] * gain;
                        s = qBound(-32768.0f, s, 32767.0f);
                        pcm[i] = int16_t(s);
                    }
                }
                m_playQueue.push_back(QByteArray(reinterpret_cast<char*>(pcm), n * 2));
                while (m_playQueue.size() > 80) m_playQueue.pop_front(); // ~1.6 s
            });
}

QJsonObject VoiceEngine::diagnostics() const {
    QJsonObject d;
    d["active"] = m_active;
    d["talking"] = m_talking;
    d["ptt"] = m_pttHeld;
    d["whisper"] = m_whisperHeld;
    d["inputRms"] = m_inputRms;
    d["opusSent"] = qint64(m_opusSent);
    d["opusSentBytes"] = qint64(m_opusSentBytes);
    d["opusReceived"] = qint64(m_opusReceived);
    d["opusReceivedBytes"] = qint64(m_opusReceivedBytes);
    d["playbackQueue"] = int(m_playQueue.size());
    return d;
}

VoiceEngine::~VoiceEngine() {
    stopRecording();
    if (m_source) m_source->stop();
    if (m_sink) m_sink->stop();
    if (m_encoder) opus_encoder_destroy(m_encoder);
    if (m_decoder) opus_decoder_destroy(m_decoder);
}

void VoiceEngine::setTransmitEnabled(bool on) {
    if (m_txEnabled == on) return;
    m_txEnabled = on;
    if (!on && m_talking) {
        m_talking = false;
        m_net->sendTalking(false);
        emit talkingChanged(false);
    }
}

void VoiceEngine::setSpeakersEnabled(bool on) {
    m_spkEnabled = on;
}

// ------------------------------------------------------------------ captura
void VoiceEngine::sendEndpointRegistration() {
    if (!m_encoder || !m_net) return;
    int16_t silence[960] = {};
    unsigned char registration[512];
    opus_encoder_ctl(m_encoder, OPUS_SET_DTX(0));
    const int encoded = opus_encode(m_encoder, silence, 960,
                                    registration, sizeof(registration));
    opus_encoder_ctl(m_encoder, OPUS_SET_DTX(1));
    if (encoded > 0)
        m_net->sendVoiceFrame(QByteArray(reinterpret_cast<const char*>(registration), encoded), ++m_seq);
}

void VoiceEngine::updateCodecSettings() {
    if (!m_encoder || !m_data) return;
    int myChanId = m_data->channelOfUser(m_data->selfId);
    if (!m_data->channels.contains(myChanId)) return;
    
    const Channel& c = m_data->channels[myChanId];
    int bitrate = qBound(16, c.bitrate, 384) * 1000; // de 16kbps a 384kbps
    
    int app = OPUS_APPLICATION_VOIP;
    if (c.codec == 5) { // Opus Music
        app = OPUS_APPLICATION_AUDIO;
    }
    
    opus_encoder_ctl(m_encoder, OPUS_SET_BITRATE(bitrate));
    opus_encoder_ctl(m_encoder, OPUS_SET_VBR(1));
    opus_encoder_ctl(m_encoder, OPUS_SET_DTX(1));
    opus_encoder_ctl(m_encoder, OPUS_SET_SIGNAL(app == OPUS_APPLICATION_AUDIO ? OPUS_SIGNAL_MUSIC : OPUS_SIGNAL_VOICE));
}

void VoiceEngine::captureTick() {
    if (!m_srcDev || !m_txEnabled) {
        if (m_srcDev) m_srcDev->readAll(); // drena p/ não estourar o buffer
        return;
    }
    if (!m_encoder) return;

    updateCodecSettings();

    // ativação de voz (Opções > Captura): 0 = PTT, 1 = detecção de voz, 2 = contínuo
    // (obs.: "capture/mode" é o backend de áudio — não confundir)
    const int mode = S::num("capture/pttMode", 1);
    if (mode == 0 && !m_pttHeld && !m_whisperHeld) {
        m_captureBuf.clear();
        m_srcDev->readAll();
        if (m_talking) {
            m_talking = false;
            m_net->sendTalking(false);
            emit talkingChanged(false);
        }
        return;
    }

    m_captureBuf.append(m_srcDev->readAll());

    while (m_captureBuf.size() >= 960 * 2) {
        const int16_t* pcm = reinterpret_cast<const int16_t*>(m_captureBuf.constData());

        // nível RMS p/ indicador "está falando"
        double sum = 0;
        for (int i = 0; i < 960; ++i) sum += double(pcm[i]) * double(pcm[i]);
        const double rms = qSqrt(sum / 960.0);
        m_inputRms = qBound(0, int(rms), 32767);
        const int levelDb = S::num("capture/voiceLevel", -45);
        const double threshold = qPow(10.0, levelDb / 20.0) * 32767.0;
        bool voiceNow = rms > threshold;
        if (mode == 0) voiceNow = true;       // PTT segurado: envia tudo
        else if (mode == 2) voiceNow = true;  // contínuo
        if (m_whisperHeld) voiceNow = true;   // sussurro força transmissão também no VAD

        if (voiceNow != m_talking) {
            if (voiceNow) {
                m_talking = true;
                m_net->sendTalking(true);
                emit talkingChanged(true);
            } else if (m_silenceClock.elapsed() > 350) { // histerese
                m_talking = false;
                m_net->sendTalking(false);
                emit talkingChanged(false);
            }
        }
        if (voiceNow) m_silenceClock.restart();

        // codifica e envia somente quando há voz (DTUX barato: silêncio não gasta pacotes)
        if (!voiceNow && m_captureBuf.size() < 960 * 2 * 4) {
            m_captureBuf.remove(0, 960 * 2);
            continue;
        }

        // Um pacote Opus pode chegar a 1275 bytes. O limite anterior de
        // 512 bytes falhava silenciosamente em canais com bitrate alto:
        // o indicador "falando" acendia, mas nenhum frame era transmitido.
        unsigned char out[1276];
        const int n = opus_encode(m_encoder, pcm, 960, out, sizeof(out));
        if (n > 0) {
            m_net->sendVoiceFrame(QByteArray(reinterpret_cast<char*>(out), n), ++m_seq);
            ++m_opusSent;
            m_opusSentBytes += quint64(n);
        }

        if (m_recFile) recWrite(reinterpret_cast<const char*>(pcm), 960 * 2); // próprio mic

        m_captureBuf.remove(0, 960 * 2);
    }
    if (m_captureBuf.size() > 960 * 2 * 8) m_captureBuf.clear(); // segurança
}

// ==================================================================== PTT
void VoiceEngine::setPttHeld(bool held) {
    const quint32 gen = ++m_pttGen; // marca esta transição (cancela timers velhos)

    if (held) {
        m_pttHeld = true; // uma nova pressão cancela qualquer soltura atrasada
        return;
    }
    if (!m_pttHeld) return;

    // "Atraso ao soltar a tecla do Push-to-Talk" (Opções > Capturar)
    if (S::flag("capture/pttDelayEnabled", false)) {
        const int ms = S::num("capture/pttDelayMs", 300);
        if (ms > 0) {
            QTimer::singleShot(ms, this, [this, gen] {
                if (gen != m_pttGen) return; // o usuário pressionou de novo
                m_pttHeld = false;
                if (m_talking) {
                    m_talking = false;
                    m_net->sendTalking(false);
                    emit talkingChanged(false);
                }
            });
            return; // continua "segurado" até o timer disparar
        }
    }

    m_pttHeld = false;
    if (m_talking) { // soltou a tecla: para de transmitir
        m_talking = false;
        m_net->sendTalking(false);
        emit talkingChanged(false);
    }
}

void VoiceEngine::setWhisperHeld(bool held) {
    const bool changed = m_whisperHeld != held;
    m_whisperHeld = held;
    // Se o VAD já estava transmitindo, a troca para sussurro não muda o
    // booleano talking e, portanto, não passaria pelo sinal normal. Emita
    // novamente o estado para que o ServerTab toque o cue de sussurro.
    if (held && changed && m_talking)
        emit talkingChanged(true);
    if (!held && m_talking && !m_pttHeld) {
        m_talking = false;
        m_net->sendTalking(false);
        emit talkingChanged(false);
    }
}

// ==================================================== gravação local (WAV)
bool VoiceEngine::startRecording(const QString& wavPath) {
    stopRecording();
    QFile* f = new QFile(wavPath, this);
    if (!f->open(QIODevice::WriteOnly)) { delete f; return false; }
    m_recFile = f;
    m_recBytes = 0;
    // cabeçalho WAV de 44 bytes (tamanhos corrigidos em recFinalize)
    QByteArray h(44, 0);
    memcpy(h.data() + 0, "RIFF", 4);
    memcpy(h.data() + 8, "WAVEfmt ", 8);
    quint32 fmtLen = 16; memcpy(h.data() + 16, &fmtLen, 4);
    quint16 audioFmt = 1, ch = 1; quint32 rate = 48000, byteRate = 48000 * 2;
    quint16 align = 2, bits = 16;
    memcpy(h.data() + 20, &audioFmt, 2); memcpy(h.data() + 22, &ch, 2);
    memcpy(h.data() + 24, &rate, 4); memcpy(h.data() + 28, &byteRate, 4);
    memcpy(h.data() + 32, &align, 2); memcpy(h.data() + 34, &bits, 2);
    memcpy(h.data() + 36, "data", 4);
    f->write(h);
    emit recordingChanged(true);
    return true;
}

void VoiceEngine::recWrite(const char* pcm, int bytes) {
    if (!m_recFile) return;
    m_recFile->write(pcm, bytes);
    m_recBytes += quint32(bytes);
}

void VoiceEngine::recFinalize() {
    if (!m_recFile) return;
    m_recFile->seek(4);
    quint32 riff = 36 + m_recBytes;
    m_recFile->write(reinterpret_cast<const char*>(&riff), 4);
    m_recFile->seek(40);
    m_recFile->write(reinterpret_cast<const char*>(&m_recBytes), 4);
    m_recFile->close();
}

void VoiceEngine::stopRecording() {
    if (!m_recFile) return;
    recFinalize();
    delete m_recFile;
    m_recFile = nullptr;
    emit recordingChanged(false);
}

// ------------------------------------------------------------------ reprodução
void VoiceEngine::playbackTick() {
    if (!m_sinkDev) return;
    if (!m_spkEnabled) {
        m_playQueue.clear();
        return;
    }
    // Escreve apenas frames completos quando houver espaço suficiente. A lógica
    // anterior removia o frame da fila e gravava só bytesFree(), descartando o
    // restante; isso picotava a voz principalmente durante screen share.
    int free = int(m_sink->bytesFree());
    while (!m_playQueue.empty()) {
        const QByteArray& frame = m_playQueue.front();
        if (free < frame.size()) break;
        const qint64 written = m_sinkDev->write(frame.constData(), frame.size());
        if (written <= 0) break;
        if (m_recFile) recWrite(frame.constData(), int(written));
        free -= int(written);
        m_playQueue.pop_front();
    }
}
