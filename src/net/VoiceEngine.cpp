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
    }

    QAudioFormat fmt;
    fmt.setSampleRate(48000);
    fmt.setChannelCount(1);
    fmt.setSampleFormat(QAudioFormat::Int16);

    const QAudioDevice inDev = QMediaDevices::defaultAudioInput();
    const QAudioDevice outDev = QMediaDevices::defaultAudioOutput();

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
                // volume por usuário (dB -> fator linear)
                int vol = 0;
                bool muted = false;
                if (m_data && m_data->users.contains(fromId)) {
                    vol = m_data->users[fromId].volumeDb;
                    muted = m_data->users[fromId].locallyMuted;
                }
                if (muted) return;
                const float gain = vol == 0 ? 1.0f : qPow(10.0f, vol / 20.0f);
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

VoiceEngine::~VoiceEngine() {
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
void VoiceEngine::captureTick() {
    if (!m_srcDev || !m_txEnabled) {
        if (m_srcDev) m_srcDev->readAll(); // drena p/ não estourar o buffer
        return;
    }
    if (!m_encoder) return;

    m_captureBuf.append(m_srcDev->readAll());

    while (m_captureBuf.size() >= 960 * 2) {
        const int16_t* pcm = reinterpret_cast<const int16_t*>(m_captureBuf.constData());

        // nível RMS p/ indicador "está falando"
        double sum = 0;
        for (int i = 0; i < 960; ++i) sum += double(pcm[i]) * double(pcm[i]);
        const double rms = qSqrt(sum / 960.0);
        const int levelDb = S::num("capture/voiceLevel", -45);
        const double threshold = qPow(10.0, levelDb / 20.0) * 32767.0;
        const bool voiceNow = rms > threshold;

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

        unsigned char out[512];
        const int n = opus_encode(m_encoder, pcm, 960, out, sizeof(out));
        if (n > 0) m_net->sendVoiceFrame(QByteArray(reinterpret_cast<char*>(out), n), ++m_seq);

        m_captureBuf.remove(0, 960 * 2);
    }
    if (m_captureBuf.size() > 960 * 2 * 8) m_captureBuf.clear(); // segurança
}

// ------------------------------------------------------------------ reprodução
void VoiceEngine::playbackTick() {
    if (!m_sinkDev) return;
    if (!m_spkEnabled) {
        m_playQueue.clear();
        return;
    }
    // escreve no dispositivo enquanto houver espaço e dados
    QByteArray chunk;
    while (!m_playQueue.empty() && chunk.size() < 960 * 2 * 8) {
        chunk.append(m_playQueue.front());
        m_playQueue.pop_front();
    }
    if (!chunk.isEmpty()) {
        const int free = int(m_sink->bytesFree());
        m_sinkDev->write(chunk.constData(), qMin<qint64>(chunk.size(), free));
    }
}
