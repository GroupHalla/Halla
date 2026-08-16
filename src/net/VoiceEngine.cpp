#include "VoiceEngine.h"
#include "NetSession.h"
#include "core/Models.h"
#include "core/AppLog.h"
#include "core/Settings.h"
#include "plugins/PluginManager.h"

#include <QAudioSource>
#include <QAudioSink>
#include <QAudioFormat>
#include <QMediaDevices>
#include <QTimer>
#include <QFile>
#include <QtMath>
#include <cmath>
#include <cstring>
#include <limits>

extern "C" {
#include <opus.h>
}

// ------------------------------------------------------------------ construção
VoiceEngine::VoiceEngine(NetSession* net, ServerData* data, QObject* parent)
    : QObject(parent), m_net(net), m_data(data) {

    // ---- Opus
    int err = 0;
    m_encoder = opus_encoder_create(48000, 1, OPUS_APPLICATION_VOIP, &err);
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

    QAudioFormat inputFmt;
    inputFmt.setSampleRate(48000);
    inputFmt.setChannelCount(1);
    inputFmt.setSampleFormat(QAudioFormat::Int16);
    QAudioFormat outputFmt = inputFmt;
    outputFmt.setChannelCount(2);

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
        m_source = new QAudioSource(inDev, inputFmt, this);
        // Dá folga para a captura de voz sobreviver a pequenos picos de CPU
        // causados pelo grab/encode de tela sem perder amostras.
        m_source->setBufferSize(960 * 2 * 20); // ~400 ms
        m_srcDev = m_source->start();
        m_captureBuf.reserve(960 * 2 * 20);

        m_capTimer = new QTimer(this);
        m_capTimer->setTimerType(Qt::PreciseTimer);
        m_capTimer->setInterval(5);
        connect(m_capTimer, &QTimer::timeout, this, &VoiceEngine::captureTick);
        m_capTimer->start();
    } else {
        AppLog::warn(tr("Nenhum dispositivo de captura de áudio encontrado"));
    }

    if (!outDev.isNull()) {
        m_sink = new QAudioSink(outDev, outputFmt, this);
        m_sink->setBufferSize(960 * 2 * 2 * 20); // ~400 ms estéreo
        m_sinkDev = m_sink->start();

        m_playTimer = new QTimer(this);
        m_playTimer->setTimerType(Qt::PreciseTimer);
        m_playTimer->setInterval(5);
        connect(m_playTimer, &QTimer::timeout, this, &VoiceEngine::playbackTick);
        m_playTimer->start();
    } else {
        AppLog::warn(tr("Nenhum dispositivo de reprodução de áudio encontrado"));
    }

    m_active = (m_encoder != nullptr);
    if (m_active)
        AppLog::info(tr("Motor de voz ativo (Opus 48 kHz mono, reprodução estéreo, 20 ms)"));

    connect(m_net, &NetSession::stateChanged, this, [this] {
        if (!m_data) return;
        const QList<int> decoderIds = m_decoders.keys();
        for (int userId : decoderIds) {
            if (m_data->users.contains(userId)) continue;
            opus_decoder_destroy(m_decoders.take(userId));
            m_remoteQueues.remove(userId);
            m_radioStates.remove(userId);
        }
    });

    connect(m_net, &NetSession::voicePacketReceived, this,
            [this](int fromId, quint16, const QByteArray& payload) {
                if (payload.isEmpty()) return;
                OpusDecoder* decoder = decoderFor(fromId);
                if (!decoder) return;
                int16_t pcm[960];
                const int n = opus_decode(decoder,
                    reinterpret_cast<const unsigned char*>(payload.constData()),
                    payload.size(), pcm, 960, 0);
                if (n <= 0) return;
                ++m_opusReceived;
                m_opusReceivedBytes += quint64(payload.size());
                if (m_data && m_data->users.value(fromId).locallyMuted) return;

                const uint32_t flags = m_data && m_data->users.value(fromId).whispering
                    ? uint32_t(HALLA_AUDIO_FLAG_WHISPER) : 0u;
                PluginManager::instance().processAudio(
                    m_pluginConnectionId, fromId,
                    HALLA_AUDIO_REMOTE_BEFORE_SPATIAL, flags,
                    pcm, uint32_t(n), 1, 48000);
                QByteArray stereo = spatializeFrame(fromId, pcm, n);
                if (stereo.isEmpty()) return;
                auto& queue = m_remoteQueues[fromId];
                queue.push_back(stereo);
                while (queue.size() > 25) queue.pop_front(); // ~500 ms por usuário
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
    int queued = 0;
    for (const auto& queue : m_remoteQueues) queued += int(queue.size());
    d["playbackQueue"] = queued;
    d["remoteDecoders"] = m_decoders.size();
    return d;
}

VoiceEngine::~VoiceEngine() {
    stopRecording();
    if (m_source) m_source->stop();
    if (m_sink) m_sink->stop();
    if (m_encoder) opus_encoder_destroy(m_encoder);
    for (OpusDecoder* decoder : m_decoders) opus_decoder_destroy(decoder);
    m_decoders.clear();
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

bool VoiceEngine::playPluginPcm(const int16_t* samples, uint32_t frames,
                                uint32_t channels, float gain) {
    if (!samples || frames == 0 || frames > 480000 || (channels != 1 && channels != 2)
            || !std::isfinite(gain) || gain < 0.0f || gain > 4.0f)
        return false;
    constexpr uint32_t kFrameSize = 960;
    auto& queue = m_remoteQueues[std::numeric_limits<int>::min()];
    for (uint32_t offset = 0; offset < frames; offset += kFrameSize) {
        const uint32_t count = qMin(kFrameSize, frames - offset);
        QByteArray output(int(kFrameSize * 2 * sizeof(int16_t)), '\0');
        int16_t* destination = reinterpret_cast<int16_t*>(output.data());
        for (uint32_t frame = 0; frame < count; ++frame) {
            const float left = channels == 1 ? samples[offset + frame]
                : samples[(offset + frame) * 2];
            const float right = channels == 1 ? samples[offset + frame]
                : samples[(offset + frame) * 2 + 1];
            destination[frame * 2] = int16_t(qBound(-32768.0f, left * gain, 32767.0f));
            destination[frame * 2 + 1] = int16_t(qBound(-32768.0f, right * gain, 32767.0f));
        }
        queue.push_back(output);
    }
    while (queue.size() > 500) queue.pop_front();
    return true;
}

OpusDecoder* VoiceEngine::decoderFor(int userId) {
    if (m_decoders.contains(userId)) return m_decoders.value(userId);
    int error = OPUS_OK;
    OpusDecoder* decoder = opus_decoder_create(48000, 1, &error);
    if (!decoder || error != OPUS_OK) {
        if (decoder) opus_decoder_destroy(decoder);
        return nullptr;
    }
    m_decoders.insert(userId, decoder);
    return decoder;
}

void VoiceEngine::applyRadioEffect(int userId, int16_t* mono, int frames,
                                   const PluginAudioControl& control) {
    if (!control.radio || !mono || frames <= 0) return;
    RadioState& state = m_radioStates[userId];
    const float strength = qBound(0.0f, control.radioStrength, 1.0f);
    const float noiseLevel = qBound(0.0f, control.radioNoise, 1.0f);
    for (int i = 0; i < frames; ++i) {
        const float input = float(mono[i]);
        // Banda estreita aproximada: passa-altas de um polo seguido de
        // passa-baixas, saturação e ruído determinístico de rádio.
        state.highPass = 0.94f * (state.highPass + input - state.previousInput);
        state.previousInput = input;
        state.lowPass += 0.32f * (state.highPass - state.lowPass);
        float filtered = qBound(-22000.0f, state.lowPass * 1.65f, 22000.0f);
        state.noiseState = state.noiseState * 1664525u + 1013904223u;
        const float noise = (float((state.noiseState >> 16) & 0xffffu) / 32767.5f - 1.0f)
            * 1800.0f * noiseLevel;
        const float output = input * (1.0f - strength)
            + (filtered + noise) * strength;
        mono[i] = int16_t(qBound(-32768.0f, output, 32767.0f));
    }
}

QByteArray VoiceEngine::spatializeFrame(int userId, int16_t* mono, int frames) {
    if (!mono || frames <= 0) return {};
    PluginAudioControl control = PluginManager::instance().audioControl(
        m_pluginConnectionId, userId);
    applyRadioEffect(userId, mono, frames, control);

    float gain = control.gain;
    if (m_data && m_data->users.contains(userId)) {
        const int volumeDb = m_data->users[userId].volumeDb;
        if (volumeDb != 0) gain *= qPow(10.0f, volumeDb / 20.0f);
    }
    const int masterX10 = S::num("playback/volumeDb", 0);
    if (masterX10 != 0)
        gain *= qPow(10.0f, (masterX10 / 10.0f) / 20.0f);
    if (m_talking && S::flag("capture/ducking", false))
        gain *= qPow(10.0f, -float(S::num("capture/duckingDb", 10)) / 20.0f);

    const float pan = qBound(-1.0f, control.pan, 1.0f);
    const float angle = (pan + 1.0f) * float(M_PI) * 0.25f;
    const float leftGain = gain * qCos(angle);
    const float rightGain = gain * qSin(angle);
    QByteArray stereo(frames * 2 * int(sizeof(int16_t)), '\0');
    int16_t* output = reinterpret_cast<int16_t*>(stereo.data());
    for (int i = 0; i < frames; ++i) {
        output[i * 2] = int16_t(qBound(-32768.0f, mono[i] * leftGain, 32767.0f));
        output[i * 2 + 1] = int16_t(qBound(-32768.0f, mono[i] * rightGain, 32767.0f));
    }
    return stereo;
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
        int16_t* pcm = reinterpret_cast<int16_t*>(m_captureBuf.data());
        const uint32_t flags = (m_whisperHeld || m_whisperTargetsConfigured)
            ? uint32_t(HALLA_AUDIO_FLAG_WHISPER) : 0u;
        PluginManager::instance().processAudio(
            m_pluginConnectionId, m_data ? m_data->selfId : 0,
            HALLA_AUDIO_CAPTURE, flags, pcm, 960, 1, 48000);

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

        // O filtro oficial de rádio é propositalmente aplicado depois do VAD:
        // assim o chiado não abre o microfone sozinho, mas a voz já segue
        // modificada para o encoder e para todos os destinatários.
        if (voiceNow) {
            PluginManager::instance().processOfficialRadio(
                m_pluginConnectionId, m_data ? m_data->selfId : 0,
                HALLA_AUDIO_CAPTURE, flags, pcm, 960, 1, 48000);
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
        m_remoteQueues.clear();
        return;
    }
    constexpr int kFrames = 960;
    constexpr int kChannels = 2;
    constexpr int kBytes = kFrames * kChannels * int(sizeof(int16_t));
    int free = int(m_sink->bytesFree());
    while (free >= kBytes) {
        bool hasFrame = false;
        int32_t mix[kFrames * kChannels] = {};
        QList<int> emptyUsers;
        for (auto it = m_remoteQueues.begin(); it != m_remoteQueues.end(); ++it) {
            if (it.value().empty()) { emptyUsers << it.key(); continue; }
            const QByteArray frame = it.value().front();
            it.value().pop_front();
            if (frame.size() != kBytes) continue;
            hasFrame = true;
            const int16_t* samples = reinterpret_cast<const int16_t*>(frame.constData());
            for (int i = 0; i < kFrames * kChannels; ++i) mix[i] += samples[i];
            if (it.value().empty()) emptyUsers << it.key();
        }
        for (int userId : emptyUsers) m_remoteQueues.remove(userId);
        if (!hasFrame) break;

        QByteArray output(kBytes, '\0');
        int16_t* samples = reinterpret_cast<int16_t*>(output.data());
        for (int i = 0; i < kFrames * kChannels; ++i)
            samples[i] = int16_t(qBound(-32768, mix[i], 32767));
        PluginManager::instance().processAudio(
            m_pluginConnectionId, 0, HALLA_AUDIO_MIXED_PLAYBACK, 0,
            samples, kFrames, kChannels, 48000);

        const qint64 written = m_sinkDev->write(output.constData(), output.size());
        if (written != output.size()) break;
        if (m_recFile) {
            int16_t mono[kFrames];
            for (int i = 0; i < kFrames; ++i)
                mono[i] = int16_t((int(samples[i * 2]) + int(samples[i * 2 + 1])) / 2);
            recWrite(reinterpret_cast<const char*>(mono), int(sizeof(mono)));
        }
        free -= kBytes;
    }
}
