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

// ---------------------------------------------------------------- construction
VoiceEngine::VoiceEngine(NetSession* net, ServerData* data, QObject* parent)
    : QObject(parent), m_net(net), m_data(data) {

    // ---- Opus
    int err = 0;
    m_encoder = opus_encoder_create(48000, 1, OPUS_APPLICATION_VOIP, &err);
    if (m_encoder) {
        opus_encoder_ctl(m_encoder, OPUS_SET_BITRATE(32000));
        opus_encoder_ctl(m_encoder, OPUS_SET_VBR(1));
        opus_encoder_ctl(m_encoder, OPUS_SET_DTX(1)); // suppresses silence frames

        // Registers the PC's UDP endpoint with a valid Opus frame. Some
        // legacy relays ignore the 10-byte HALL packet without a payload; without
        // this warm-up the PC only becomes a target after it speaks first.
        int16_t silence[960] = {};
        unsigned char registration[512];
        // DTX may suppress silence and produce an empty payload. During the
        // UDP endpoint warm-up we force a non-empty Opus frame.
        opus_encoder_ctl(m_encoder, OPUS_SET_DTX(0));
        const int encoded = opus_encode(m_encoder, silence, 960,
                                        registration, sizeof(registration));
        opus_encoder_ctl(m_encoder, OPUS_SET_DTX(1));
        if (encoded > 0 && m_net) {
            for (quint16 seq = 1; seq <= 3; ++seq)
                m_net->sendVoiceFrame(
                    QByteArray(reinterpret_cast<const char*>(registration), encoded), seq);
        }
        // Re-sent periodically to traverse NATs and packet loss in the first
        // moments. Without it, the first audio could depend on the PC speaking
        // first to register its UDP endpoint again.
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

    QAudioDevice inDev = pickCaptureDevice();

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
        // Slack so the voice capture survives small CPU spikes caused by
        // screen grab/encode without dropping samples. v1.1.34: 400 ms ->
        // 800 ms -- long GUI bursts (screen encoding on a modest machine)
        // overflowed the buffer and dropped samples in the MIDDLE of speech
        // (the "mic cutting out" reports); the reader drains every 5 ms, so
        // the larger buffer adds no steady-state latency -- it is just slack.
        m_source->setBufferSize(960 * 2 * 40); // ~800 ms
        m_srcDev = m_source->start();
        m_captureBuf.reserve(960 * 2 * 40);
        m_lastCaptureDataMs = QDateTime::currentMSecsSinceEpoch();
        watchSourceState(); // a device error cannot mean eternal silence

        m_capTimer = new QTimer(this);
        m_capTimer->setTimerType(Qt::PreciseTimer);
        // v1.1.35: 5 ms -> 10 ms. The QAudioSource buffer is 800 ms, so a
        // 10 ms drain still leaves plenty of slack; this halves the event-loop
        // wake-ups (200/s -> 100/s) on the thread shared with the whole GUI.
        // The added latency is at most 5 ms at the start of speech.
        m_capTimer->setInterval(10);
        connect(m_capTimer, &QTimer::timeout, this, &VoiceEngine::captureTick);
        m_capTimer->start();
    } else {
        AppLog::warn(tr("Nenhum dispositivo de captura de áudio encontrado"));
    }

    if (!outDev.isNull()) {
        m_sink = new QAudioSink(outDev, outputFmt, this);
        // ~240 ms buffer: the GUI shares the playback thread (Qt delivers
        // packets, decodes and feeds the sink on the same event loop), so UI
        // bursts -- tree rebuilds, live-frame painting, antivirus -- of
        // 100-200 ms drained the former 160 ms buffer and the audio "stopped
        // for milliseconds" (the user really heard the sink run dry). 240 ms
        // plus the re-prime grace absorbs those bursts without the cut; the
        // extra delay only appears during a burst, never in steady state.
        m_sink->setBufferSize(960 * 2 * 2 * 12); // ~240 ms stereo
        m_sinkDev = m_sink->start();

        m_playTimer = new QTimer(this);
        m_playTimer->setTimerType(Qt::PreciseTimer);
        // v1.1.35: 5 ms -> 10 ms (same as capture -- the sink has a 240 ms
        // buffer plus the per-user jitter buffer on top; the tick writes 0-2
        // frames of 20 ms, so a 10 ms cadence covers it comfortably).
        m_playTimer->setInterval(10);
        connect(m_playTimer, &QTimer::timeout, this, &VoiceEngine::playbackTick);
        m_playTimer->start();

        // Decays the jitter-buffer target when the network is stable:
        // underruns raise the target immediately (in playbackTick); without
        // this slow decay, latency acquired during a bad network moment would
        // stay forever.
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
            m_voiceDryMs.remove(userId);
            m_radioStates.remove(userId);
            m_remoteLastVoiceMs.remove(userId);
        }
    });

    // Periodic sweep of the packet-driven "speaking" indicator: clears the
    // ring for users whose audio stopped arriving (keeps a safety delay that
    // covers the transmitter's 500 ms hysteresis and DTX).
    m_remoteVoiceClock.start();
    m_remoteTalkingTimer = new QTimer(this);
    m_remoteTalkingTimer->setInterval(200);
    connect(m_remoteTalkingTimer, &QTimer::timeout, this, &VoiceEngine::sweepRemoteTalking);
    m_remoteTalkingTimer->start();

    // Voice DSP (WebRTC AEC3 + neural noise suppression): the Options dialog
    // writes straight to QSettings, so we re-read them periodically to make
    // changes apply without restarting the call. Applying an identical
    // configuration is cheap (ApplyConfig short-circuits what did not change).
    m_speechClock.start();
    refreshDspSettings();
    m_dspTimer = new QTimer(this);
    m_dspTimer->setInterval(2000);
    connect(m_dspTimer, &QTimer::timeout, this, &VoiceEngine::refreshDspSettings);
    m_dspTimer->start();

    connect(m_net, &NetSession::voicePacketReceived, this,
            [this](int fromId, quint16 seq, const QByteArray& payload) {
                if (payload.isEmpty()) return;
                // Drop duplicate or replayed datagrams: UDP relays can deliver
                // the same packet twice, and a replayed seq must never be
                // decoded again (stale audio / amplification). Sequence space
                // is 16-bit; "older" means behind the last accepted seq.
                if (m_lastVoiceSeq.contains(fromId)) {
                    const quint16 last = m_lastVoiceSeq.value(fromId);
                    const quint16 diff = quint16(seq - last);
                    if (diff == 0 || diff > 32768) return;
                }
                m_lastVoiceSeq[fromId] = seq;
                // Packet-driven "speaking" indicator: the ring lights up from
                // the audio that actually arrives. The user_state message (TCP)
                // is still valid, but any delay/loss on that path -- or a state
                // stuck on the server -- no longer hides who is really
                // transmitting. Recorded BEFORE the local mute: someone with the
                // individual speaker turned off is still shown as speaking.
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
                while (queue.size() > 25) queue.pop_front(); // ~500 ms per user
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
    d["voiceDries"] = qint64(m_voiceDries);
    d["voiceSheds"] = qint64(m_voiceSheds);
    d["primedVoices"] = m_voicePrimed.size();
    d["remoteDecoders"] = m_decoders.size();
    // Diagnosis of the "mic freezing, cutting out" reports (v1.1.34): if the
    // guard counters rise while the user speaks, the crosstalk protection is
    // cutting legitimate speech (can be disabled in Options > Capture); if the
    // stall reopenings rise, the user's device freezes without reporting an
    // error (driver/USB/Bluetooth).
    d["echoGuardBlocks"] = qint64(m_echoGuard.blockCount());
    d["echoGuardRevokes"] = qint64(m_echoGuard.revokeCount());
    d["captureStallReopens"] = qint64(m_captureStallReopens);
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

// ------------------------------------------------- dispositivo de captura
QAudioDevice VoiceEngine::pickCaptureDevice() const {
    // Same choice as in the constructor: the device saved in Options >
    // Capture when it still exists, otherwise the default. Re-evaluating on
    // every reopen covers Bluetooth headsets being switched on/off and USB
    // devices being swapped.
    QAudioDevice dev = QMediaDevices::defaultAudioInput();
    const QString savedInId = S::str("capture/device");
    if (!savedInId.isEmpty()) {
        const auto inputs = QMediaDevices::audioInputs();
        for (const QAudioDevice& input : inputs) {
            if (input.id() == savedInId) {
                dev = input;
                break;
            }
        }
    }
    return dev;
}

void VoiceEngine::watchSourceState() {
    if (!m_source) return;
    connect(m_source, &QAudioSource::stateChanged, this, [this](QtAudio::State state) {
        if (state != QtAudio::StoppedState || !m_source) return;
        const QtAudio::Error err = m_source->error();
        if (err == QtAudio::NoError) return; // stop() intencional (destrutor)
        // IO failed / device disappeared: without this the capture died
        // silently -- the app kept "transmitting" without a single sample.
        AppLog::warn(tr("A captura do microfone caiu (erro %1); reabrindo o dispositivo.")
                         .arg(int(err)));
        reopenAudioCapture();
    });
}

void VoiceEngine::reopenAudioCapture() {
    if (m_captureReopenPending) return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - m_lastCaptureReopenMs < 2000) return; // guarda anti-loop
    m_lastCaptureReopenMs = now;

    const QAudioDevice dev = pickCaptureDevice();
    m_srcDev = nullptr;
    if (m_source) {
        m_source->disconnect(this); // nada de callbacks do objeto moribundo
        m_source->stop();
        m_source->deleteLater();
        m_source = nullptr;
    }
    m_captureBuf.clear();

    QAudioFormat inputFmt;
    inputFmt.setSampleRate(48000);
    inputFmt.setChannelCount(1);
    inputFmt.setSampleFormat(QAudioFormat::Int16);

    if (!dev.isNull()) {
        m_source = new QAudioSource(dev, inputFmt, this);
        m_source->setBufferSize(960 * 2 * 40); // ~800 ms (idem construtor)
        m_srcDev = m_source->start();
        watchSourceState();
    }

    if (m_srcDev) {
        m_captureBuf.reserve(960 * 2 * 40);
        m_lastCaptureDataMs = QDateTime::currentMSecsSinceEpoch();
        AppLog::info(tr("Captura do microfone reaberta."));
        return;
    }

    // Device disappeared or failed to open: retry in 5 s (headset becomes
    // available again, default device changes, etc.) -- without log spam.
    if (m_source) {
        m_source->disconnect(this);
        m_source->deleteLater();
        m_source = nullptr;
    }
    AppLog::warn(tr("Nenhum dispositivo de captura disponível agora; "
                    "nova tentativa em 5 s."));
    m_captureReopenPending = true;
    QTimer::singleShot(5000, this, [this] {
        m_captureReopenPending = false;
        reopenAudioCapture();
    });
}

// Drains the capture device and keeps the activity clock up to date.
// Every read path (transmission, draining with voice closed and ramp flush)
// goes through here -- it is the single place that knows when the microphone
// delivered its last sample.
void VoiceEngine::drainCaptureDevice() {
    if (!m_srcDev) return; // flushGateFade also arrives here through setPttHeld
    const QByteArray chunk = m_srcDev->readAll();
    if (!chunk.isEmpty()) {
        m_lastCaptureDataMs = QDateTime::currentMSecsSinceEpoch();
        m_captureBuf.append(chunk);
    }
}

void VoiceEngine::setTransmitEnabled(bool on) {
    if (m_txEnabled == on) return;
    m_txEnabled = on;
    m_fadeInLeft = m_fadeOutLeft = 0; // clean cut: no transmission, no ramp
    m_echoPending.clear();            // held frames are not valid for another session
    if (!on) {
        if (m_talking) {
            m_talking = false;
            m_net->sendTalking(false);
            emit talkingChanged(false);
        }
        // MICROPHONE MUTED: the "on speak" cue's voice detection also turns
        // off immediately. analyzeCapturedSpeech() keeps draining the device
        // (EchoGuard reference), but no speech sound may fire -- muted is muted.
        if (m_speechActive) {
            m_speechActive = false;
            emit speechActivityChanged(false);
        }
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
    // After a real interruption, rebuild the prebuffer instead of playing
    // every late packet immediately and producing successive cuts.
    if (previous > 0 && now - previous > 120) {
        queue.clear();
        m_primedStreams.remove(streamUserId);
    }
    m_streamLastPacketMs[streamUserId] = now;
    if (!enqueueStereoPcm(queue, samples, frames, channels, gain)) return false;

    // Avoids accumulating seconds of latency when the UI/audio device stalls.
    // Above 400 ms, keep only 120 ms of the most recent frames.
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
    const int masterX10 = m_cfgMasterVolX10; // v1.1.35: cache (ver refreshCachedConfig)
    if (masterX10 != 0)
        gain *= qPow(10.0f, (masterX10 / 10.0f) / 20.0f);
    if (m_talking && m_cfgDucking)
        gain *= qPow(10.0f, -float(m_cfgDuckingDb) / 20.0f);

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

// ------------------------------------------------------------- config cache
// v1.1.35: the keys below used to be read from QSettings INSIDE the per-frame
// path (up to ~10 reads every 20 ms of voice). QSettings caches in memory, but
// each read still builds QString/QVariant -- dozens per second during a call.
// A 250 ms TTL keeps Options changes applying at an imperceptible delay and
// turns the hot path into a member read.
void VoiceEngine::refreshCachedConfig() {
    if (m_cfgClock.isValid() && m_cfgClock.elapsed() < 250) return;
    m_cfgClock.restart();
    m_cfgPttMode = S::num("capture/pttMode", 1);
    m_cfgVoiceLevelDb = S::num("capture/voiceLevel", -45);
    m_cfgCrosstalkGuard = S::flag("capture/crosstalkGuard", true);
    m_cfgMicGainX10 = S::num("capture/micGainDb", 0);
    m_cfgMasterVolX10 = S::num("playback/volumeDb", 0);
    m_cfgDucking = S::flag("capture/ducking", false);
    m_cfgDuckingDb = S::num("capture/duckingDb", 10);
}

// ------------------------------------------------------ DSP + speech detection
void VoiceEngine::refreshDspSettings() {
    // Options > Capture > Digital signal processing. Before v1.1.20 these keys
    // existed in the UI but were NOT applied anywhere -- the "noise reduction"
    // was literally a checkbox with no effect.
    const bool denoise = S::flag("capture/denoise", true);
    const int level = S::num("capture/denoiseLevel", 50);
    const bool echo = S::flag("capture/echoCancellation", true);
    m_apm.configure(denoise, level, echo);
    // Logging on the app side: the module is moc-free so it can be compiled
    // standalone by the CI gate (tests/webrtc_apm_smoke.cpp).
    if (!m_dspAnnounced || denoise != m_lastDspDenoise || echo != m_lastDspEcho) {
        m_dspAnnounced = true;
        m_lastDspDenoise = denoise;
        m_lastDspEcho = echo;
        if (denoise || echo) {
#ifdef HALLA_WEBRTC_NATIVE
            AppLog::info(tr("DSP de voz (WebRTC APM): supressão de ruído %1, cancelamento de eco %2")
                             .arg(denoise ? tr("ligada") : tr("desligada"))
                             .arg(echo ? tr("ligado") : tr("desligado")));
#else
            AppLog::info(tr("DSP de voz embutido (RNNoise + AEC do speex): supressão de ruído %1, cancelamento de eco %2")
                             .arg(denoise ? tr("ligada") : tr("desligada"))
                             .arg(echo ? tr("ligado") : tr("desligado")));
#endif
        }
        // Only when the user asked for processing and NOTHING is active is
        // there something to report (DSP allocation failure).
        if ((denoise || echo) && !m_apm.denoiseActive() && !m_apm.echoActive()) {
            AppLog::warn(tr("Não foi possível iniciar o processamento de voz."));
        }
    }
}

void VoiceEngine::updateSpeechDetection(double rms) {
    // Speech detection for the "on speak" cue: the SAME VAD threshold
    // (Options > Capture > Voice activity), with hysteresis to avoid flicker.
    // Runs on the ALREADY processed signal (noise/echo removed): with speakers,
    // the other side's voice picked up by the microphone no longer opens the cue.
    const int levelDb = m_cfgVoiceLevelDb; // v1.1.35: cache (ver refreshCachedConfig)
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

// Microphone gain "beyond the device limit" (Options > Capture > Boost
// microphone volume): 0 to +30 dB of software amplification. Applied AFTER
// the APM (AEC3/neural suppression) and BEFORE the VAD -- a louder microphone
// also trips voice detection more easily, which is the expected behavior.
// Saturation is soft (soft-clip above ~-3 dBFS): the signal compresses instead
// of crackling, keeping the audio usable at the extreme.
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
    // Microphone drained (voice closed or PTT released): transmission is off,
    // but the "on speak" cue must reflect the user SPEAKING -- not the
    // transmission being open. Process and discard.
    // Exception: with the microphone MUTED (transmission disabled by mute) the
    // cue never fires -- the user muted the microphone on purpose; the "on
    // speak" beep on every sentence would only be annoying.
    const bool cueAllowed = m_txEnabled;
    drainCaptureDevice();
    const double micGain = micGainLinear();
    const int levelDb = m_cfgVoiceLevelDb; // v1.1.35: cache (ver refreshCachedConfig)
    const double onThreshold = qPow(10.0, levelDb / 20.0) * 32767.0;
    while (m_captureBuf.size() >= 960 * 2) {
        int16_t* pcm = reinterpret_cast<int16_t*>(m_captureBuf.data());
        int16_t rawPcm[960];
        std::memcpy(rawPcm, pcm, 960 * 2);
        m_apm.processCaptureFrame(pcm);
        m_apm.processCaptureFrame(pcm + 480);
        if (micGain > 1.0) applyMicGain(pcm, 960, micGain);
        if (!cueAllowed) {
            // Muted: keeps the EchoGuard reference ring fed (switching
            // mode/mute mid-call leaves no holes), but detected speech emits no
            // cue and transmits nothing.
            m_echoGuard.noteCapture(rawPcm, 960, false, false);
            if (m_speechActive) {
                m_speechActive = false;
                emit speechActivityChanged(false);
            }
            m_captureBuf.remove(0, 960 * 2);
            continue;
        }
        double sum = 0;
        for (int i = 0; i < 960; ++i) sum += double(pcm[i]) * double(pcm[i]);
        const double rms = qSqrt(sum / 960.0);
        // EchoGuard crosstalk also with voice closed: the "on speak" cue must
        // not fire on the partner's voice in VAD mode. In the other modes the
        // frame only feeds the guard's ring.
        double cueRms = rms;
        // Crosstalk protection is optional since v1.1.34: with speakers in use
        // and the room talking along, guard false positives chopped the user's
        // own voice ("mic freezing, cutting out"). The checkbox lives in
        // Options > Capture > Digital signal processing.
        const bool guardActive = m_cfgPttMode == 1 && m_cfgCrosstalkGuard;
        const EchoGuard::Decision d = m_echoGuard.noteCapture(
            rawPcm, 960, guardActive && rms > onThreshold, false);
        if (guardActive && d != EchoGuard::Decision::Open) cueRms = 0.0;
        updateSpeechDetection(cueRms);
        m_captureBuf.remove(0, 960 * 2);
    }
    if (m_captureBuf.size() > 960 * 2 * 8) m_captureBuf.clear(); // safety
}

// Frames held back by the EchoGuard validation: transmitted in a burst when
// the speech is confirmed legitimate (the receiver's jitter buffer absorbs the
// burst -- the start of the sentence arrives whole, just 400 ms later).
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
    }
    m_echoPending.clear();
}

double VoiceEngine::micGainLinear() const {
    // "capture/micGainDb" is stored in TENTHS of dB (the convention of the
    // Options dB sliders): 300 = +30 dB = ~31x. Default 0 = no amplification.
    // v1.1.35: the value comes from the 250 ms cache (m_cfgMicGainX10) -- it
    // used to be a QSettings read per 20 ms frame.
    const int gainX10 = m_cfgMicGainX10;
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
    // Key released / whisper disabled: arms the closing ramp. The hard cut on
    // the last transmitted frame was audible as a "click" on the far end; with
    // the ramp the audio fades smoothly to zero over 60 ms.
    m_fadeOutLeft = kFadeOutSamples;
    if (m_talking) {
        m_talking = false;
        m_net->sendTalking(false);
        emit talkingChanged(false);
    }
}

void VoiceEngine::flushGateFade() {
    // Transmission just went silent by key (PTT released): sends the up to
    // 60 ms closing ramp before silence takes over. Without it, the last
    // transmitted frame would end at an arbitrary level (hard click).
    if (m_fadeOutLeft <= 0 || !m_encoder || !m_txEnabled) return;
    drainCaptureDevice();
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

    // v1.1.35: this ran on EVERY capture tick applying the same ctl's
    // -- now it applies only when channel/bitrate/codec actually change.
    if (myChanId == m_lastCodecChanId && bitrate == m_lastCodecBitrate
            && app == m_lastCodecApp)
        return;
    m_lastCodecChanId = myChanId;
    m_lastCodecBitrate = bitrate;
    m_lastCodecApp = app;

    opus_encoder_ctl(m_encoder, OPUS_SET_BITRATE(bitrate));
    opus_encoder_ctl(m_encoder, OPUS_SET_VBR(1));
    opus_encoder_ctl(m_encoder, OPUS_SET_DTX(1));
    opus_encoder_ctl(m_encoder, OPUS_SET_SIGNAL(app == OPUS_APPLICATION_AUDIO ? OPUS_SIGNAL_MUSIC : OPUS_SIGNAL_VOICE));
}

void VoiceEngine::captureTick() {
    if (!m_srcDev) return;
    refreshCachedConfig(); // v1.1.35: the hot path reads members, not QSettings

    // Stall watchdog (v1.1.34): device open, state "active", but NO samples for
    // more than 2 s. Driver glitches, USB power saving and Bluetooth stall like
    // this -- no error, no state change, so the v1.1.32 error watchdog never
    // fired and the mic stayed silent until reconnecting. A healthy device
    // delivers a sample (or silence) every ~10 ms; 2 s of nothing is a stall.
    // Reopens (with the anti-loop guard).
    if (m_source && m_source->state() != QAudio::StoppedState) {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (now - m_lastCaptureDataMs > 2000) {
            AppLog::warn(tr("O microfone parou de entregar amostras (travou sem erro); reabrindo o dispositivo."));
            ++m_captureStallReopens;
            m_lastCaptureDataMs = now; // reinicia a janela (reopen tem guarda de 2 s)
            reopenAudioCapture();
            return;
        }
    }

    if (!m_txEnabled || !m_encoder) {
        // Voice closed: drains the microphone while keeping speech detection
        // alive -- the "on speak" cue is about the user speaking, not about
        // transmitting.
        analyzeCapturedSpeech();
        return;
    }

    updateCodecSettings();

    // voice activation (Options > Capture): 0 = PTT, 1 = voice detection, 2 = continuous
    // (note: "capture/mode" is the audio backend -- do not confuse the two)
    const int mode = m_cfgPttMode; // v1.1.35: cache (ver refreshCachedConfig)
    if (mode == 0 && !m_pttHeld && !m_whisperHeld) {
        if (m_talking) closeTransmissionGate(); // key released without a previous flush
        flushGateFade();       // envia a rampa de fechamento, se pendente
        analyzeCapturedSpeech();
        return;
    }

    drainCaptureDevice();
    const double micGain = micGainLinear();

    while (m_captureBuf.size() >= 960 * 2) {
        int16_t* pcm = reinterpret_cast<int16_t*>(m_captureBuf.data());

        // RAW copy (pre-APM) for the EchoGuard: AEC3 removes from the signal
        // exactly the component correlated with the playout -- which is what the
        // guard needs to see in order to detect crosstalk.
        int16_t rawPcm[960];
        std::memcpy(rawPcm, pcm, 960 * 2);

        // Voice DSP BEFORE everything else: AEC3 subtracts what is playing on
        // the speaker and neural suppression removes background noise. VAD,
        // plugins and Opus then see the clean signal -- which also improves the
        // voice-detection threshold (fewer false positives with a fan running).
        m_apm.processCaptureFrame(pcm);
        m_apm.processCaptureFrame(pcm + 480);
        if (micGain > 1.0) applyMicGain(pcm, 960, micGain);

        const uint32_t flags = (m_whisperHeld || m_whisperTargetsConfigured)
            ? uint32_t(HALLA_AUDIO_FLAG_WHISPER) : 0u;
        PluginManager::instance().processAudio(
            m_pluginConnectionId, m_data ? m_data->selfId : 0,
            HALLA_AUDIO_CAPTURE, flags, pcm, 960, 1, 48000);

        // RMS level for the "is speaking" indicator
        double sum = 0;
        for (int i = 0; i < 960; ++i) sum += double(pcm[i]) * double(pcm[i]);
        const double rms = qSqrt(sum / 960.0);
        m_inputRms = qBound(0, int(rms), 32767);
        const int levelDb = m_cfgVoiceLevelDb; // v1.1.35: cache
        const double threshold = qPow(10.0, levelDb / 20.0) * 32767.0;
        bool voiceNow = rms > threshold;
        if (mode == 0) voiceNow = true;       // PTT segurado: envia tudo
        else if (mode == 2) voiceNow = true;  // continuous
        if (m_whisperHeld) voiceNow = true;   // whisper forces transmission in VAD mode too

        // Crosstalk guard (v1.1.22): in VAD mode, the PARTNER's voice playing on
        // the speaker (or leaking into the shared microphone of a second client
        // on the same machine) must not open transmission as if it were the
        // user speaking -- that was the "speaking" ring lighting up on two users
        // at once, the duplicated sound cue and the echo coming back audible.
        // The guard compares the raw microphone against the playout: a delayed
        // copy = crosstalk, so it does not open (and revokes if already open).
        // PTT/continuous are explicit user choices: no decision, but the
        // microphone keeps feeding the ring (switching mode mid-call leaves no
        // holes in the reference). Since v1.1.34 the protection can also be
        // disabled in Options > Capture (false positives chopped the voice of
        // users on speakers).
        const bool guardActive = (mode == 1 && !m_whisperHeld
            && m_cfgCrosstalkGuard);
        EchoGuard::Decision echo = m_echoGuard.noteCapture(
            rawPcm, 960, guardActive && voiceNow, guardActive && m_talking);
        if (guardActive) {
            if (echo == EchoGuard::Decision::Hold) {
                // Validation in progress: holds the processed frame back -- if
                // the speech is confirmed legitimate, the backfill returns its
                // beginning.
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
                // Revocation closing ramp: blocked frames that still have a
                // pending ramp keep being transmitted (the hard cut mid-word was
                // the "click" on the far end); once the ramp is exhausted the
                // block becomes a plain discard.
                if (m_fadeOutLeft > 0) {
                    applyGateFades(pcm, 960);
                    unsigned char out[1276];
                    const int n = opus_encode(m_encoder, pcm, 960, out, sizeof(out));
                    if (n > 0) {
                        m_net->sendVoiceFrame(
                            QByteArray(reinterpret_cast<char*>(out), n), ++m_seq);
                        ++m_opusSent;
                        m_opusSentBytes += quint64(n);
                    }
                }
                // v1.1.34: this used to be m_captureBuf.clear() -- it threw away
                // up to 7 captured frames at once (a hole of up to 140 ms plus a
                // click on the far end, and the closing ramp was never sent).
                // Now it drops ONE frame at a time: the blocking behavior is the
                // same, but the output stays continuous.
                m_captureBuf.remove(0, 960 * 2);
                updateSpeechDetection(0.0);
                continue;
            }
            // Open: the validation finished without a match. If the VAD does not
            // open the gate NOW, the speech that produced the held frames is
            // already gone -- discard (no backfill of stale speech next time).
            if (!voiceNow) m_echoPending.clear();
        }

        // "On speak" cue: detects real speech (regardless of the VAD
        // transmitting), including when the signal stays below the threshold in
        // continuous mode -- and it also runs on the drain path above.
        updateSpeechDetection(rms);

        if (voiceNow != m_talking) {
            if (voiceNow) {
                m_talking = true;
                m_net->sendTalking(true);
                emit talkingChanged(true);
                m_fadeInLeft = kFadeInSamples;   // abertura suave (sem "pop")
                // A closing ramp left pending (e.g. transmission revoked by the
                // guard and speech resumed) must not eat the start of the
                // resumption: the 4 ms fadeIn already provides a smooth opening --
                // the old downward ramp would make the voice "fade out" during
                // the first 60 ms.
                m_fadeOutLeft = 0;
                // EchoGuard backfill: the frames held during validation are
                // transmitted now -- legitimate speech does not lose its start
                // during the 400 ms confirmation window.
                if (echo == EchoGuard::Decision::Open) transmitHeldEchoFrames();
            } else if (m_silenceClock.elapsed() > 500) { // AUDIO hysteresis
                m_talking = false;
                m_net->sendTalking(false);
                emit talkingChanged(false);
                m_fadeOutLeft = kFadeOutSamples; // fechamento em rampa de 60 ms
            }
        }
        if (voiceNow) m_silenceClock.restart();

        // Transmission gate with AUDIO hysteresis: while the gate is OPEN
        // (m_talking) every frame is encoded and sent -- the RMS of a 20 ms
        // frame fluctuates constantly during normal speech (weak consonants
        // between strong vowels); dropping frame by frame on the threshold
        // chopped the voice and ate the ends of words. Discarding only returns
        // with the gate closed AND the closing ramp finished.
        if (!m_talking && m_fadeOutLeft == 0 && m_captureBuf.size() < 960 * 2 * 4) {
            m_captureBuf.remove(0, 960 * 2);
            continue;
        }

        // The official radio filter is deliberately applied after the VAD: that
        // way the hiss does not open the microphone by itself, but the voice is
        // already modified for the encoder and for every recipient. The
        // AFTER_VAD stage hands the same pipeline point to packaged addons
        // (.halla-addon) so their AGC cannot open the VAD either.
        if (m_talking || m_fadeOutLeft > 0) {
            applyGateFades(pcm, 960);
            PluginManager::instance().processAudio(
                m_pluginConnectionId, m_data ? m_data->selfId : 0,
                HALLA_AUDIO_CAPTURE_AFTER_VAD, flags, pcm, 960, 1, 48000);
        }

        // An Opus packet can reach 1275 bytes. The previous 512-byte limit
        // failed silently on high-bitrate channels: the "speaking" indicator lit
        // up but no frame was transmitted.
        unsigned char out[1276];
        const int n = opus_encode(m_encoder, pcm, 960, out, sizeof(out));
        if (n > 0) {
            m_net->sendVoiceFrame(QByteArray(reinterpret_cast<char*>(out), n), ++m_seq);
            ++m_opusSent;
            m_opusSentBytes += quint64(n);
        }


        m_captureBuf.remove(0, 960 * 2);
    }
    if (m_captureBuf.size() > 960 * 2 * 8) m_captureBuf.clear(); // segurança
}

// ==================================================================== PTT
void VoiceEngine::setPttHeld(bool held) {
    const quint32 gen = ++m_pttGen; // marks this transition (cancels stale timers)

    if (held) {
        m_pttHeld = true; // a new press cancels any delayed release
        return;
    }
    if (!m_pttHeld) return;

    // "Delay when releasing the Push-to-Talk key" (Options > Capture)
    if (S::flag("capture/pttDelayEnabled", false)) {
        const int ms = S::num("capture/pttDelayMs", 300);
        if (ms > 0) {
            QTimer::singleShot(ms, this, [this, gen] {
                if (gen != m_pttGen) return; // the user pressed again
                m_pttHeld = false;
                if (m_talking) closeTransmissionGate();
                flushGateFade();
            });
            return; // stays "held" until the timer fires
        }
    }

    m_pttHeld = false;
    if (m_talking) closeTransmissionGate(); // key released: stops transmitting
    flushGateFade();
}

void VoiceEngine::setWhisperHeld(bool held) {
    const bool changed = m_whisperHeld != held;
    m_whisperHeld = held;
    // If the VAD was already transmitting, switching to whisper does not change
    // the talking boolean and therefore would not go through the normal signal.
    // Emit the state again so ServerTab plays the whisper cue.
    if (held && changed && m_talking)
        emit talkingChanged(true);
    if (!held && m_talking && !m_pttHeld) {
        closeTransmissionGate();
        flushGateFade();
    }
}

// ===================================================== local recording (WAV)
bool VoiceEngine::startRecording(const QString& wavPath) {
    stopRecording();
    QFile* f = new QFile(wavPath, this);
    if (!f->open(QIODevice::WriteOnly)) { delete f; return false; }
    m_recFile = f;
    m_recBytes = 0;
    // 44-byte WAV header (sizes corrected in recFinalize)
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

// ------------------------------------------------------------------ playback
// Clears the "speaking" indicator for users whose voice packets stopped
// arriving. The safety delay (700 ms) fully covers the transmitter's 500 ms
// hysteresis (during which DTX sends no frames) and network jitter; after that,
// no audio means no ring. It also self-heals any stuck state: if the server
// kept talking=true (speaker's app frozen mid-speech), the ring clears 700 ms
// after the last packet instead of staying lit forever.
void VoiceEngine::sweepRemoteTalking() {
    if (!m_data || m_remoteLastVoiceMs.isEmpty()) return;
    const qint64 now = m_remoteVoiceClock.elapsed();
    bool changed = false;
    QList<int> stale;
    for (auto it = m_remoteLastVoiceMs.constBegin();
         it != m_remoteLastVoiceMs.constEnd(); ++it) {
        const int userId = it.key();
        if (!m_data->users.contains(userId)) {
            stale << userId; // left the server: clears the record
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
    if (m_voiceUnderruns == m_voiceUnderrunsAtAdapt && m_voiceDries == m_voiceDriesAtAdapt
            && m_voiceTargetFrames > 2) {
        // 15 s without a single underrun and no dry spell beyond the grace: the
        // network and the GUI can handle a smaller target.
        --m_voiceTargetFrames;
    }
    m_voiceUnderrunsAtAdapt = m_voiceUnderruns;
    m_voiceDriesAtAdapt = m_voiceDries;
}

void VoiceEngine::playbackTick() {
    if (!m_sinkDev) return;
    refreshCachedConfig(); // v1.1.35: spatializeFrame gains/ducking come from here
    if (!m_spkEnabled) {
        m_remoteQueues.clear();
        m_voicePrimed.clear();
        m_voiceDryMs.clear();
        m_streamQueues.clear();
        m_primedStreams.clear();
        m_streamLastPacketMs.clear();
        return;
    }
    // If the device really ran out of data, go back to pre-buffering each
    // stream. Voice calls continue without this extra delay.
    if (m_sink && m_sink->state() == QAudio::IdleState) {
        m_primedStreams.clear();
        // REAL voice underrun: the device ran dry while primed users were
        // playing. The current jitter is above the target -- raise the target
        // (up to 8 frames = 160 ms).
        if (!m_voicePrimed.isEmpty()) {
            ++m_voiceUnderruns;
            if (m_voiceTargetFrames < 8) ++m_voiceTargetFrames;
            // No forced re-prime: the former clear() also punished users who
            // still had a queue -- it cut ~80 ms of audio from ALL speakers on
            // every underrun (the residual "popping"). Whoever ran dry resumes
            // on the next frame that arrives; the higher target rebuilds the
            // slack for the next ones.
        }
    }
    // Dry-spell grace: the whole voice pipeline shares the GUI thread, so
    // hiccups of tens of ms (UI, network) are normal. Keeping the prime during
    // the dry spell makes the audio RESUME IMMEDIATELY when the next frame
    // arrives; unpriming on the first empty queue turned every hiccup into
    // "stopping for milliseconds" (silence while the prebuffer rebuilt). 300 ms
    // covers GUI/network bursts without holding the prime across a real
    // speech silence/DTX (typically >= 400 ms).
    constexpr qint64 kVoiceDryGraceMs = 300;
    // DEEP shed: drop an accumulated burst only when the delay passes ~400 ms,
    // and then return to ~200 ms. The old threshold (target+5 = 7-13 frames)
    // threw away ordinary 100-200 ms bursts -- in music, EVERY dropped frame is
    // audible (the "cut"); accumulated delay that drains by itself is not.
    // 400 ms remains acceptable for conversation (what the reference jitter
    // buffer uses under bad conditions).
    constexpr int kVoiceShedCeilingFrames = 20;
    constexpr int kVoiceShedFloorFrames = 10;
    constexpr int kFrames = 960;
    constexpr int kChannels = 2;
    constexpr int kBytes = kFrames * kChannels * int(sizeof(int16_t));
    int free = int(m_sink->bytesFree());
    // v1.1.35: allocations hoisted out of the frame loop (the loop runs per
    // 20 ms frame; a QList/QDateTime per iteration was needless churn).
    QList<int> emptyUsers;
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    while (free >= kBytes) {
        bool hasFrame = false;
        int32_t mix[kFrames * kChannels] = {};
        emptyUsers.clear();
        for (auto it = m_remoteQueues.begin(); it != m_remoteQueues.end(); ++it) {
            const int uid = it.key();
            if (uid == std::numeric_limits<int>::min()) {
                // Addon sounds: local queue, minimal latency, no prebuffer --
                // they do not take part in the voice jitter buffer.
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
            // Per-user jitter buffer: holds the first frames until the target
            // accumulates and only then starts playing. A user who went without
            // frames for longer than the grace period (DTX, silence) accumulates
            // again on the next speech -- the speech restart rebuilds the
            // prebuffer.
            if (!m_voicePrimed.contains(uid)) {
                if (int(queue.size()) < m_voiceTargetFrames) continue;
                m_voicePrimed.insert(uid);
            }
            if (queue.empty()) {
                // GRACE: the queue ran dry now or less than 300 ms ago -- keep
                // the prime and wait for the next frame to resume immediately.
                // Only after the grace period is the prebuffer rebuilt; the dry
                // spell still counts as a signal to raise the target (the jitter
                // existed, even if the sink buffer hid the hole).
                const qint64 drySince = m_voiceDryMs.value(uid, 0);
                if (drySince == 0) {
                    m_voiceDryMs.insert(uid, nowMs);
                    continue;
                }
                if (nowMs - drySince <= kVoiceDryGraceMs) continue;
                m_voiceDryMs.remove(uid);
                m_voicePrimed.remove(uid);
                ++m_voiceDries;
                if (m_voiceTargetFrames < 8) ++m_voiceTargetFrames;
                continue;
            }
            m_voiceDryMs.remove(uid);
            // Latency control (deep shed): an accumulated burst above ~400 ms
            // returns to ~200 ms instead of playing everything late -- but
            // ordinary GUI/network bursts now play in full.
            if (int(queue.size()) > kVoiceShedCeilingFrames) {
                while (int(queue.size()) > kVoiceShedFloorFrames) {
                    queue.pop_front();
                    ++m_voiceSheds;
                }
            }
            const QByteArray frame = queue.front();
            queue.pop_front();
            if (frame.size() != kBytes) continue;
            hasFrame = true;
            const int16_t* samples = reinterpret_cast<const int16_t*>(frame.constData());
            for (int i = 0; i < kFrames * kChannels; ++i) mix[i] += samples[i];
        }
        for (int userId : emptyUsers) {
            m_remoteQueues.remove(userId);
            m_voicePrimed.remove(userId);
        }

        // 40 ms absorb short variations without leaving the audio perceptibly
        // behind the video, which is already synchronized by the WebRTC jitter
        // buffer.
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

        // Far-end reference for the echo canceller: exactly the mix that goes to
        // the speaker NOW. WebRTC's AEC3 uses it to subtract from the microphone
        // whatever the speaker reproduces -- that is what makes calls work
        // without headphones.
        int16_t farEnd[kFrames];
        for (int i = 0; i < kFrames; ++i)
            farEnd[i] = int16_t((int(samples[i * 2]) + int(samples[i * 2 + 1])) / 2);
        m_apm.processReverseFrame(farEnd);
        m_apm.processReverseFrame(farEnd + 480);
        // Crosstalk-guard reference: the same mix that goes to the speaker --
        // whatever the microphone picks up that resembles it (delayed) is network
        // echo, not the local user speaking.
        m_echoGuard.notePlayout(farEnd, kFrames);

        const qint64 written = m_sinkDev->write(output.constData(), output.size());
        if (written != output.size()) break;
        // Playout delay (far-end -> speaker) for the AEC: real sink buffer level
        // plus a fixed allowance for the device latency.
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
