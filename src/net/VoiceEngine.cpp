#include "VoiceEngine.h"
#include "NetSession.h"
#include "core/Models.h"
#include "core/AppLog.h"
#include "core/Settings.h"
#include "plugins/PluginManager.h"

#include <QAudio>
#include <QAudioSource>
#include <QAudioSink>
#include <QAudioFormat>
#include <QMediaDevices>
#include <QTimer>
#include <QFile>
#include <QDateTime>
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
        // Buffer de ~160 ms: o antigo de 120 ms secava em rajadas curtas de
        // trabalho da GUI (rebuild da árvore, pintura de frames de live) e
        // era a causa da voz "pipocar" nas calls. 160 ms mantém folga sem
        // atraso perceptível — o áudio das lives tem sincronização própria
        // do WebRTC e o prebuffer delas continua enxuto.
        m_sink->setBufferSize(960 * 2 * 2 * 8); // ~160 ms estéreo
        m_sinkDev = m_sink->start();

        m_playTimer = new QTimer(this);
        m_playTimer->setTimerType(Qt::PreciseTimer);
        m_playTimer->setInterval(5);
        connect(m_playTimer, &QTimer::timeout, this, &VoiceEngine::playbackTick);
        m_playTimer->start();

        // Decai o alvo do jitter buffer quando a rede está estável: underruns
        // sobem o alvo na hora (em playbackTick); sem este decaimento lento a
        // latência adquirida num momento ruim de rede ficaria para sempre.
        m_voiceAdaptTimer = new QTimer(this);
        m_voiceAdaptTimer->setInterval(15000);
        connect(m_voiceAdaptTimer, &QTimer::timeout, this, &VoiceEngine::adaptVoiceTarget);
        m_voiceAdaptTimer->start();
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
            m_remoteLastVoiceMs.remove(userId);
        }
    });

    // Varredura periódica do indicador "falando" orientado a pacotes: apaga
    // o anel de usuários cujo áudio parou de chegar (mantém um atraso de
    // segurança que cobre a histerese de 500 ms do transmissor e o DTX).
    m_remoteVoiceClock.start();
    m_remoteTalkingTimer = new QTimer(this);
    m_remoteTalkingTimer->setInterval(200);
    connect(m_remoteTalkingTimer, &QTimer::timeout, this, &VoiceEngine::sweepRemoteTalking);
    m_remoteTalkingTimer->start();

    // DSP de voz (AEC3 + supressão de ruído neural do WebRTC): as Opções
    // gravam direto no QSettings, então re-lemos periodicamente para as
    // mudanças pegarem sem reiniciar a call. Aplicar configuração igual é
    // barato (ApplyConfig curto-circuita o que não mudou).
    m_speechClock.start();
    refreshDspSettings();
    m_dspTimer = new QTimer(this);
    m_dspTimer->setInterval(2000);
    connect(m_dspTimer, &QTimer::timeout, this, &VoiceEngine::refreshDspSettings);
    m_dspTimer->start();

    connect(m_net, &NetSession::voicePacketReceived, this,
            [this](int fromId, quint16, const QByteArray& payload) {
                if (payload.isEmpty()) return;
                // Indicador "falando" orientado a pacotes: o anel acende pelo
                // áudio que realmente chega. A mensagem user_state (TCP)
                // continua válida, mas qualquer atraso/perda nesse caminho
                // — ou estado preso no servidor — não esconde mais o símbolo
                // de quem está transmitindo de fato. Registra ANTES do mudo
                // local: quem está com o alto-falante individual desligado
                // continua sendo exibido como falando.
                if (m_data && m_data->users.contains(fromId)) {
                    m_remoteLastVoiceMs[fromId] = m_remoteVoiceClock.elapsed();
                    User& speaking = m_data->users[fromId];
                    if (!speaking.talking) {
                        speaking.talking = true;
                        emit remoteVoiceActivityChanged();
                    }
                }
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
    d["speechActive"] = m_speechActive;
    d["dspDenoise"] = m_apm.denoiseActive();
    d["dspEchoCancel"] = m_apm.echoActive();
    d["ptt"] = m_pttHeld;
    d["whisper"] = m_whisperHeld;
    d["inputRms"] = m_inputRms;
    d["opusSent"] = qint64(m_opusSent);
    d["opusSentBytes"] = qint64(m_opusSentBytes);
    d["opusReceived"] = qint64(m_opusReceived);
    d["opusReceivedBytes"] = qint64(m_opusReceivedBytes);
    int queued = 0;
    for (const auto& queue : m_remoteQueues) queued += int(queue.size());
    int streamQueued = 0;
    for (const auto& queue : m_streamQueues) streamQueued += int(queue.size());
    d["playbackQueue"] = queued;
    d["streamPlaybackQueue"] = streamQueued;
    d["primedStreams"] = m_primedStreams.size();
    d["voiceJitterTarget"] = m_voiceTargetFrames;
    d["voiceUnderruns"] = qint64(m_voiceUnderruns);
    d["voiceSheds"] = qint64(m_voiceSheds);
    d["primedVoices"] = m_voicePrimed.size();
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
    m_fadeInLeft = m_fadeOutLeft = 0; // corte limpo: sem transmissão não há rampa
    m_echoPending.clear();            // quadros retidos não valem para outra sessão
    if (!on && m_talking) {
        m_talking = false;
        m_net->sendTalking(false);
        emit talkingChanged(false);
    }
}

void VoiceEngine::setSpeakersEnabled(bool on) {
    m_spkEnabled = on;
}

static bool enqueueStereoPcm(std::deque<QByteArray>& queue,
                             const int16_t* samples, uint32_t frames,
                             uint32_t channels, float gain) {
    if (!samples || frames == 0 || frames > 480000 || (channels != 1 && channels != 2)
            || !std::isfinite(gain) || gain < 0.0f || gain > 4.0f)
        return false;
    constexpr uint32_t kFrameSize = 960;
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
    return true;
}

bool VoiceEngine::playPluginPcm(const int16_t* samples, uint32_t frames,
                                uint32_t channels, float gain) {
    auto& queue = m_remoteQueues[std::numeric_limits<int>::min()];
    if (!enqueueStereoPcm(queue, samples, frames, channels, gain)) return false;
    while (queue.size() > 500) queue.pop_front();
    return true;
}

bool VoiceEngine::playStreamPcm(int streamUserId, const int16_t* samples,
                                uint32_t frames, uint32_t channels, float gain) {
    if (streamUserId <= 0) return false;
    auto& queue = m_streamQueues[streamUserId];
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 previous = m_streamLastPacketMs.value(streamUserId, 0);
    // Depois de uma interrupção real, reconstrua o prebuffer em vez de tocar
    // cada pacote atrasado imediatamente e produzir cortes sucessivos.
    if (previous > 0 && now - previous > 120) {
        queue.clear();
        m_primedStreams.remove(streamUserId);
    }
    m_streamLastPacketMs[streamUserId] = now;
    if (!enqueueStereoPcm(queue, samples, frames, channels, gain)) return false;

    // Evita acumular segundos de latência se a UI/dispositivo de áudio pausar.
    // Ao ultrapassar 400 ms, mantenha apenas 120 ms dos quadros mais recentes.
    if (queue.size() > 20) {
        while (queue.size() > 6) queue.pop_front();
        m_primedStreams.insert(streamUserId);
    }
    return true;
}

void VoiceEngine::clearStreamPcm(int streamUserId) {
    m_streamQueues.remove(streamUserId);
    m_primedStreams.remove(streamUserId);
    m_streamLastPacketMs.remove(streamUserId);
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
    RadioVoiceDsp& dsp = m_radioStates[userId];
    if (!dsp.seeded())
        dsp.seed(0xA341316Cu ^ quint32(userId * 2654435761u));
    dsp.configure(qBound(0.0f, control.radioStrength, 1.0f),
                  qBound(0.0f, control.radioNoise, 1.0f), 1.0f);
    dsp.process(mono, uint32_t(frames));
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

// ------------------------------------------------- DSP + detecção de fala
void VoiceEngine::refreshDspSettings() {
    // Opções > Captura > Processamento digital de sinal. Antes do v1.1.20
    // estas chaves existiam na UI mas NÃO eram aplicadas em lugar nenhum —
    // a "redução de ruído" era literalmente um checkbox sem efeito.
    const bool denoise = S::flag("capture/denoise", true);
    const int level = S::num("capture/denoiseLevel", 50);
    const bool echo = S::flag("capture/echoCancellation", true);
    m_apm.configure(denoise, level, echo);
    // Logging no lado do app: o módulo é moc-free para poder ser compilado
    // standalone pelo gate de CI (tests/webrtc_apm_smoke.cpp).
    if (!m_dspAnnounced || denoise != m_lastDspDenoise || echo != m_lastDspEcho) {
        m_dspAnnounced = true;
        m_lastDspDenoise = denoise;
        m_lastDspEcho = echo;
        if (denoise || echo) {
            AppLog::info(tr("DSP de voz (WebRTC APM): supressão de ruído %1, cancelamento de eco %2")
                             .arg(denoise ? tr("ligada") : tr("desligada"))
                             .arg(echo ? tr("ligado") : tr("desligado")));
        }
        if ((denoise || echo) && !m_apm.denoiseActive() && !m_apm.echoActive()) {
            AppLog::warn(tr("Este build não contém o WebRTC nativo: redução de ruído/eco indisponível."));
        }
    }
}

void VoiceEngine::updateSpeechDetection(double rms) {
    // Detecção de fala para o sinal sonoro "ao falar": o MESMO limiar do VAD
    // (Opções > Captura > Atividade de voz), com histerese para não tremolar.
    // Roda no sinal JÁ processado (ruído/eco removidos): com alto-falantes, a
    // voz do outro lado captada pelo microfone não abre mais o cue.
    const int levelDb = S::num("capture/voiceLevel", -45);
    const double onThreshold = qPow(10.0, levelDb / 20.0) * 32767.0;
    const double offThreshold = onThreshold * 0.45; // ~-7 dB de histerese
    const qint64 now = m_speechClock.elapsed();
    if (rms > onThreshold) {
        m_lastSpeechAboveMs = now;
        if (!m_speechActive) {
            m_speechActive = true;
            emit speechActivityChanged(true);
        }
    } else if (m_speechActive && rms < offThreshold
               && now - m_lastSpeechAboveMs > 300) {
        m_speechActive = false;
        emit speechActivityChanged(false);
    }
}

// Ganho de microfone "além do limite do dispositivo" (Opções > Captura >
// Aumentar volume do microfone): 0 a +30 dB de amplificação por software.
// Aplicado DEPOIS do APM (AEC3/supressão neural) e ANTES do VAD — microfone
// mais alto também abre a detecção de voz mais fácil, que é o comportamento
// esperado. A saturação é suave (soft-clip acima de ~-3 dBFS): o sinal
// comprime em vez de estalar, para manter o áudio utilizável no extremo.
static void applyMicGain(int16_t* pcm, int frames, double gainLin) {
    if (!pcm || frames <= 0) return;
    constexpr double kKnee = 4096.0;                 // ~-18 dBFS abaixo do teto
    constexpr double kHead = 32767.0 - kKnee;
    for (int i = 0; i < frames; ++i) {
        double v = double(pcm[i]) * gainLin;
        if (v > kKnee)
            v = kKnee + kHead * std::tanh((v - kKnee) / kHead);
        else if (v < -kKnee)
            v = -kKnee - kHead * std::tanh((-v - kKnee) / kHead);
        pcm[i] = int16_t(qBound(-32768.0, v, 32767.0));
    }
}

void VoiceEngine::analyzeCapturedSpeech() {
    // Microfone drenado (voz fechada ou PTT solto): a transmissão está
    // desligada, mas o sinal sonoro "ao falar" precisa refletir o usuário
    // FALANDO — não a transmissão estar aberta. Processa e descarta.
    m_captureBuf.append(m_srcDev->readAll());
    const double micGain = micGainLinear();
    const int levelDb = S::num("capture/voiceLevel", -45);
    const double onThreshold = qPow(10.0, levelDb / 20.0) * 32767.0;
    while (m_captureBuf.size() >= 960 * 2) {
        int16_t* pcm = reinterpret_cast<int16_t*>(m_captureBuf.data());
        int16_t rawPcm[960];
        std::memcpy(rawPcm, pcm, 960 * 2);
        m_apm.processCaptureFrame(pcm);
        m_apm.processCaptureFrame(pcm + 480);
        if (micGain > 1.0) applyMicGain(pcm, 960, micGain);
        double sum = 0;
        for (int i = 0; i < 960; ++i) sum += double(pcm[i]) * double(pcm[i]);
        const double rms = qSqrt(sum / 960.0);
        // Crosstalk do EchoGuard também com a voz fechada: o cue "ao falar"
        // não pode disparar com a voz do parceiro no modo VAD. Nos outros
        // modos o quadro apenas alimenta o anel do guarda.
        double cueRms = rms;
        const bool guardActive = S::num("capture/pttMode", 1) == 1;
        const EchoGuard::Decision d = m_echoGuard.noteCapture(
            rawPcm, 960, guardActive && rms > onThreshold, false);
        if (guardActive && d != EchoGuard::Decision::Open) cueRms = 0.0;
        updateSpeechDetection(cueRms);
        m_captureBuf.remove(0, 960 * 2);
    }
    if (m_captureBuf.size() > 960 * 2 * 8) m_captureBuf.clear(); // segurança
}

// Quadros retidos pela validação do EchoGuard: transmitidos em rajada
// quando a fala é confirmada legítima (o jitter buffer do receptor absorve
// a rajada — o começo da frase chega inteiro, apenas 400 ms depois).
void VoiceEngine::transmitHeldEchoFrames() {
    if (m_echoPending.isEmpty() || !m_encoder) return;
    const uint32_t flags = (m_whisperHeld || m_whisperTargetsConfigured)
        ? uint32_t(HALLA_AUDIO_FLAG_WHISPER) : 0u;
    for (const QByteArray& held : m_echoPending) {
        int16_t heldPcm[960];
        std::memcpy(heldPcm, held.constData(), 960 * 2);
        applyGateFades(heldPcm, 960);
        PluginManager::instance().processAudio(
            m_pluginConnectionId, m_data ? m_data->selfId : 0,
            HALLA_AUDIO_CAPTURE_AFTER_VAD, flags, heldPcm, 960, 1, 48000);
        unsigned char out[1276];
        const int n = opus_encode(m_encoder, heldPcm, 960, out, sizeof(out));
        if (n > 0) {
            m_net->sendVoiceFrame(
                QByteArray(reinterpret_cast<char*>(out), n), ++m_seq);
            ++m_opusSent;
            m_opusSentBytes += quint64(n);
        }
        if (m_recFile) recWrite(reinterpret_cast<const char*>(heldPcm), 960 * 2);
    }
    m_echoPending.clear();
}

double VoiceEngine::micGainLinear() const {
    // "capture/micGainDb" é gravado em DÉCIMOS de dB (padrão dos sliders de
    // dB das Opções): 300 = +30 dB = ~31x. Default 0 = sem amplificação.
    const int gainX10 = S::num("capture/micGainDb", 0);
    if (gainX10 <= 0) return 1.0;
    return qPow(10.0, (qMin(gainX10, 300) / 10.0) / 20.0);
}

void VoiceEngine::applyGateFades(int16_t* pcm, int frames) {
    if (!pcm || frames <= 0) return;
    if (m_fadeInLeft <= 0 && m_fadeOutLeft <= 0) return;
    for (int i = 0; i < frames; ++i) {
        double scale = 1.0;
        if (m_fadeOutLeft > 0) {
            scale = double(m_fadeOutLeft) / double(kFadeOutSamples);
            --m_fadeOutLeft;
        } else if (m_fadeInLeft > 0) {
            scale = 1.0 - double(m_fadeInLeft) / double(kFadeInSamples);
            --m_fadeInLeft;
        }
        if (scale < 1.0)
            pcm[i] = int16_t(qBound(-32768.0, double(pcm[i]) * scale, 32767.0));
    }
}

void VoiceEngine::closeTransmissionGate() {
    // Tecla solta / sussurro desligado: arma a rampa de fechamento. O corte
    // seco no último quadro transmitido era audível como "clique" na outra
    // ponta; com a rampa o áudio desce suavemente para zero em 60 ms.
    m_fadeOutLeft = kFadeOutSamples;
    if (m_talking) {
        m_talking = false;
        m_net->sendTalking(false);
        emit talkingChanged(false);
    }
}

void VoiceEngine::flushGateFade() {
    // A transmissão acabou de calar por tecla (PTT solto): envia os até 60 ms
    // da rampa de fechamento antes de o silêncio assumir. Sem isto, o último
    // quadro transmitido terminaria em nível arbitrário (clique seco).
    if (m_fadeOutLeft <= 0 || !m_encoder || !m_txEnabled) return;
    m_captureBuf.append(m_srcDev->readAll());
    const double micGain = micGainLinear();
    while (m_captureBuf.size() >= 960 * 2 && m_fadeOutLeft > 0) {
        int16_t* pcm = reinterpret_cast<int16_t*>(m_captureBuf.data());
        m_apm.processCaptureFrame(pcm);
        m_apm.processCaptureFrame(pcm + 480);
        if (micGain > 1.0) applyMicGain(pcm, 960, micGain);
        applyGateFades(pcm, 960);
        unsigned char out[1276];
        const int n = opus_encode(m_encoder, pcm, 960, out, sizeof(out));
        if (n > 0) {
            m_net->sendVoiceFrame(
                QByteArray(reinterpret_cast<char*>(out), n), ++m_seq);
            ++m_opusSent;
            m_opusSentBytes += quint64(n);
        }
        m_captureBuf.remove(0, 960 * 2);
    }
    if (m_captureBuf.size() > 960 * 2 * 8) m_captureBuf.clear(); // segurança
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
    if (!m_srcDev) return;
    if (!m_txEnabled || !m_encoder) {
        // Voz fechada: drena o microfone mantendo a detecção de fala viva —
        // o cue "ao falar" é sobre o usuário falar, não sobre transmitir.
        analyzeCapturedSpeech();
        return;
    }

    updateCodecSettings();

    // ativação de voz (Opções > Captura): 0 = PTT, 1 = detecção de voz, 2 = contínuo
    // (obs.: "capture/mode" é o backend de áudio — não confundir)
    const int mode = S::num("capture/pttMode", 1);
    if (mode == 0 && !m_pttHeld && !m_whisperHeld) {
        if (m_talking) closeTransmissionGate(); // tecla solta sem flush prévio
        flushGateFade();       // envia a rampa de fechamento, se pendente
        analyzeCapturedSpeech();
        return;
    }

    m_captureBuf.append(m_srcDev->readAll());
    const double micGain = micGainLinear();

    while (m_captureBuf.size() >= 960 * 2) {
        int16_t* pcm = reinterpret_cast<int16_t*>(m_captureBuf.data());

        // Cópia CRUA (pré-APM) para o EchoGuard: o AEC3 remove do sinal
        // justamente a componente correlacionada com o playout — que é o
        // que o guarda precisa enxergar para detectar crosstalk.
        int16_t rawPcm[960];
        std::memcpy(rawPcm, pcm, 960 * 2);

        // DSP de voz ANTES de tudo: o AEC3 subtrai o que está tocando no
        // alto-falante e a supressão neural remove ruído de fundo. VAD,
        // plugins e Opus passam a ver o sinal limpo — o que também melhora
        // o limiar de detecção de voz (menos falsos positivos com ventoinha).
        m_apm.processCaptureFrame(pcm);
        m_apm.processCaptureFrame(pcm + 480);
        if (micGain > 1.0) applyMicGain(pcm, 960, micGain);

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

        // Guarda de crosstalk (v1.1.22): no modo VAD, a voz do PARCEIRO que
        // toca no alto-falante (ou vaza no microfone compartilhado de um
        // segundo cliente na mesma máquina) não pode abrir a transmissão
        // como se fosse fala do usuário — era o anel de "falando" acendendo
        // em dois usuários ao mesmo tempo, o sinal sonoro duplicado e o eco
        // voltando audível. O guarda compara o microfone cru com o playout:
        // cópia atrasada = crosstalk, não abre (e revoga se já abriu).
        // PTT/contínuo são escolhas explícitas do usuário: sem decisão, mas
        // o microfone continua alimentando o anel (trocar de modo no meio
        // da call não deixa buracos na referência).
        const bool guardActive = (mode == 1 && !m_whisperHeld);
        EchoGuard::Decision echo = m_echoGuard.noteCapture(
            rawPcm, 960, guardActive && voiceNow, guardActive && m_talking);
        if (guardActive) {
            if (echo == EchoGuard::Decision::Hold) {
                // Validação em curso: retém o quadro processado — se a fala
                // for confirmada legítima, o backfill devolve o começo.
                m_echoPending.append(
                    QByteArray(reinterpret_cast<const char*>(pcm), 960 * 2));
                while (m_echoPending.size() > 20) m_echoPending.removeFirst();
                updateSpeechDetection(0.0);
                m_captureBuf.remove(0, 960 * 2);
                continue;
            }
            if (echo == EchoGuard::Decision::Blocked) {
                if (m_talking) closeTransmissionGate();
                m_echoPending.clear();
                m_captureBuf.clear();     // crosstalk acumulado: fora
                updateSpeechDetection(0.0);
                continue;
            }
            // Open: a validação terminou sem casar. Se o VAD não abrir o
            // gate AGORA, a fala que gerou os quadros retidos já passou —
            // descarta (nada de backfill de fala velha na próxima).
            if (!voiceNow) m_echoPending.clear();
        }

        // Cue "ao falar": detecta fala de verdade (independente do VAD
        // transmitir), inclusive quando o sinal não passa do limiar no modo
        // contínuo — e roda também no caminho de drenagem acima.
        updateSpeechDetection(rms);

        if (voiceNow != m_talking) {
            if (voiceNow) {
                m_talking = true;
                m_net->sendTalking(true);
                emit talkingChanged(true);
                m_fadeInLeft = kFadeInSamples;   // abertura suave (sem "pop")
                // Backfill do EchoGuard: os quadros retidos durante a
                // validação são transmitidos agora — a fala legítima não
                // perde o começo nos 400 ms de confirmação.
                if (echo == EchoGuard::Decision::Open) transmitHeldEchoFrames();
            } else if (m_silenceClock.elapsed() > 500) { // histerese de ÁUDIO
                m_talking = false;
                m_net->sendTalking(false);
                emit talkingChanged(false);
                m_fadeOutLeft = kFadeOutSamples; // fechamento em rampa de 60 ms
            }
        }
        if (voiceNow) m_silenceClock.restart();

        // Gate de transmissão com histerese de ÁUDIO: enquanto o gate está
        // ABERTO (m_talking), todo quadro é codificado e enviado — o RMS de
        // um quadro de 20 ms oscila o tempo todo durante a fala normal
        // (consoantes fracas entre vogais fortes); descartar quadro a quadro
        // pelo limiar picotava a voz e comia o fim das palavras. O descarte
        // só volta com o gate fechado E a rampa de fechamento concluída.
        if (!m_talking && m_fadeOutLeft == 0 && m_captureBuf.size() < 960 * 2 * 4) {
            m_captureBuf.remove(0, 960 * 2);
            continue;
        }

        // O filtro oficial de rádio é propositalmente aplicado depois do VAD:
        // assim o chiado não abre o microfone sozinho, mas a voz já segue
        // modificada para o encoder e para todos os destinatários. O estágio
        // AFTER_VAD entrega o mesmo ponto do pipeline aos complementos em
        // pacote (.halla-addon) sem que o AGC deles abra o VAD.
        if (m_talking || m_fadeOutLeft > 0) {
            applyGateFades(pcm, 960);
            PluginManager::instance().processAudio(
                m_pluginConnectionId, m_data ? m_data->selfId : 0,
                HALLA_AUDIO_CAPTURE_AFTER_VAD, flags, pcm, 960, 1, 48000);
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
                if (m_talking) closeTransmissionGate();
                flushGateFade();
            });
            return; // continua "segurado" até o timer disparar
        }
    }

    m_pttHeld = false;
    if (m_talking) closeTransmissionGate(); // soltou a tecla: para de transmitir
    flushGateFade();
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
        closeTransmissionGate();
        flushGateFade();
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
// Apaga o indicador "falando" de usuários cujos pacotes de voz pararam de
// chegar. O atraso de segurança (700 ms) cobre integralmente a histerese de
// 500 ms do transmissor (durante a qual o DTX não envia quadros) e o jitter
// da rede; após isso, sem áudio não há anel. Isto também autocura qualquer
// estado preso: se o servidor ficou com talking=true (app do falante
// congelado no meio da fala), o anel some 700 ms após o último pacote em
// vez de ficar aceso para sempre.
void VoiceEngine::sweepRemoteTalking() {
    if (!m_data || m_remoteLastVoiceMs.isEmpty()) return;
    const qint64 now = m_remoteVoiceClock.elapsed();
    bool changed = false;
    QList<int> stale;
    for (auto it = m_remoteLastVoiceMs.constBegin();
         it != m_remoteLastVoiceMs.constEnd(); ++it) {
        const int userId = it.key();
        if (!m_data->users.contains(userId)) {
            stale << userId; // saiu do servidor: limpa o registro
            continue;
        }
        if (m_data->users[userId].talking && now - it.value() > 700) {
            m_data->users[userId].talking = false;
            changed = true;
        }
    }
    for (int userId : stale) m_remoteLastVoiceMs.remove(userId);
    if (changed) emit remoteVoiceActivityChanged();
}

void VoiceEngine::adaptVoiceTarget() {
    if (m_voiceUnderruns == m_voiceUnderrunsAtAdapt && m_voiceTargetFrames > 2) {
        // 15 s sem um único underrun: a rede aguenta um alvo menor.
        --m_voiceTargetFrames;
    }
    m_voiceUnderrunsAtAdapt = m_voiceUnderruns;
}

void VoiceEngine::playbackTick() {
    if (!m_sinkDev) return;
    if (!m_spkEnabled) {
        m_remoteQueues.clear();
        m_voicePrimed.clear();
        m_streamQueues.clear();
        m_primedStreams.clear();
        m_streamLastPacketMs.clear();
        return;
    }
    // Se o dispositivo realmente ficou sem dados, volte a pré-carregar cada
    // live. A chamada de voz continua sem esse atraso adicional.
    if (m_sink && m_sink->state() == QAudio::IdleState) {
        m_primedStreams.clear();
        // Underrun REAL de voz: o dispositivo secou enquanto havia usuários
        // primados tocando. O jitter atual é maior que o alvo — cresce o alvo
        // (até 8 quadros = 160 ms).
        if (!m_voicePrimed.isEmpty()) {
            ++m_voiceUnderruns;
            if (m_voiceTargetFrames < 8) ++m_voiceTargetFrames;
            // Sem re-prime forçado: o antigo clear() punia também quem
            // ainda tinha fila — cortava ~80 ms de áudio de TODOS os
            // falantes a cada underrun (a "pipocada" residual). Quem secou
            // retoma no próximo quadro que chegar; o alvo maior reconstrói
            // a folga para os próximos.
        }
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
            const int uid = it.key();
            if (uid == std::numeric_limits<int>::min()) {
                // Sons de complementos: fila local, latência mínima, sem
                // prebuffer — não participa do jitter buffer de voz.
                if (it.value().empty()) { emptyUsers << uid; continue; }
                const QByteArray frame = it.value().front();
                it.value().pop_front();
                if (frame.size() != kBytes) continue;
                hasFrame = true;
                const int16_t* samples = reinterpret_cast<const int16_t*>(frame.constData());
                for (int i = 0; i < kFrames * kChannels; ++i) mix[i] += samples[i];
                if (it.value().empty()) emptyUsers << uid;
                continue;
            }
            auto& queue = it.value();
            // Jitter buffer por usuário: segura os primeiros quadros até
            // acumular o alvo e só então começa a tocar. Um usuário que
            // ficou sem quadros (DTX, silêncio) volta a acumular na próxima
            // fala — o reinício de fala reconstrói o prebuffer.
            if (!m_voicePrimed.contains(uid)) {
                if (int(queue.size()) < m_voiceTargetFrames) continue;
                m_voicePrimed.insert(uid);
            }
            // Controle de latência: se os quadros se acumularam além do alvo
            // + tolerância (rajada depois de um travamento), descarta os mais
            // antigos em vez de tocar tudo atrasado.
            if (int(queue.size()) > m_voiceTargetFrames + 5) {
                while (int(queue.size()) > m_voiceTargetFrames) {
                    queue.pop_front();
                    ++m_voiceSheds;
                }
            }
            if (queue.empty()) { emptyUsers << uid; continue; }
            const QByteArray frame = queue.front();
            queue.pop_front();
            if (frame.size() != kBytes) continue;
            hasFrame = true;
            const int16_t* samples = reinterpret_cast<const int16_t*>(frame.constData());
            for (int i = 0; i < kFrames * kChannels; ++i) mix[i] += samples[i];
            if (queue.empty()) emptyUsers << uid;
        }
        for (int userId : emptyUsers) {
            m_remoteQueues.remove(userId);
            m_voicePrimed.remove(userId);
        }

        // 40 ms absorvem variações curtas sem deixar o áudio perceptivelmente
        // atrás do vídeo, que já é sincronizado pelo jitter buffer WebRTC.
        constexpr int kStreamPrebufferFrames = 2;
        for (auto it = m_streamQueues.begin(); it != m_streamQueues.end(); ++it) {
            if (!m_primedStreams.contains(it.key())) {
                if (int(it.value().size()) < kStreamPrebufferFrames) continue;
                m_primedStreams.insert(it.key());
            }
            if (it.value().empty()) continue;
            const QByteArray frame = it.value().front();
            it.value().pop_front();
            if (frame.size() != kBytes) continue;
            hasFrame = true;
            const int16_t* samples = reinterpret_cast<const int16_t*>(frame.constData());
            for (int i = 0; i < kFrames * kChannels; ++i) mix[i] += samples[i];
        }
        if (!hasFrame) break;

        QByteArray output(kBytes, '\0');
        int16_t* samples = reinterpret_cast<int16_t*>(output.data());
        for (int i = 0; i < kFrames * kChannels; ++i)
            samples[i] = int16_t(qBound(-32768, mix[i], 32767));
        PluginManager::instance().processAudio(
            m_pluginConnectionId, 0, HALLA_AUDIO_MIXED_PLAYBACK, 0,
            samples, kFrames, kChannels, 48000);

        // Referência far-end do cancelador de eco: exatamente o mix que vai
        // para o alto-falante AGORA. O AEC3 do WebRTC usa isto para subtrair
        // do microfone o que o alto-falante reproduzir — é o que faz a call
        // funcionar sem fone de ouvido.
        int16_t farEnd[kFrames];
        for (int i = 0; i < kFrames; ++i)
            farEnd[i] = int16_t((int(samples[i * 2]) + int(samples[i * 2 + 1])) / 2);
        m_apm.processReverseFrame(farEnd);
        m_apm.processReverseFrame(farEnd + 480);
        // Referência do guarda de crosstalk: o mesmo mix que vai para o
        // alto-falante — o que o microfone captar de parecido com isto
        // (atrasado) é eco da rede, não fala do usuário local.
        m_echoGuard.notePlayout(farEnd, kFrames);

        const qint64 written = m_sinkDev->write(output.constData(), output.size());
        if (written != output.size()) break;
        // Atraso de playout (far-end -> alto-falante) para o AEC: nível real
        // do buffer do sink + folga fixa para a latência do dispositivo.
        if (m_sink) {
            const int buffered = int(m_sink->bufferSize()) - int(m_sink->bytesFree());
            m_apm.setPlayoutDelayMs(buffered * 20 / kBytes + 40);
        }
        if (m_recFile) {
            int16_t mono[kFrames];
            for (int i = 0; i < kFrames; ++i)
                mono[i] = int16_t((int(samples[i * 2]) + int(samples[i * 2 + 1])) / 2);
            recWrite(reinterpret_cast<const char*>(mono), int(sizeof(mono)));
        }
        free -= kBytes;
    }
}
