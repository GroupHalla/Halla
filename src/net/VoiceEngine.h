#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QIODevice>
#include <QMap>
#include <QSet>
#include <QObject>
#include <cstdint>
#include <deque>

#include "plugins/RadioVoiceDsp.h"
#include "audio/HallaAudioProcessing.h"
#include "audio/EchoGuard.h"

class NetSession;
class QAudioSource;
class QAudioSink;
class QAudioDevice;
struct OpusEncoder;
struct OpusDecoder;
struct ServerData;
struct PluginAudioControl;

// Client voice engine: microphone -> Opus (48 kHz, mono, 20 ms) -> UDP, and
// received packets -> per-user decoding -> DSP/spatialization -> stereo
// mixing -> speakers. Degrades gracefully without a device.
class VoiceEngine : public QObject {
    Q_OBJECT
public:
    explicit VoiceEngine(NetSession* net, ServerData* data, QObject* parent = nullptr);
    ~VoiceEngine() override;

    bool isActive() const { return m_active; }

    void setTransmitEnabled(bool on);
    void setSpeakersEnabled(bool on);
    void setPluginConnectionId(quint64 id) { m_pluginConnectionId = id; }
    void setWhisperTargetsConfigured(bool on) { m_whisperTargetsConfigured = on; }
    bool playPluginPcm(const int16_t* samples, uint32_t frames,
                       uint32_t channels, float gain);
    bool playStreamPcm(int streamUserId, const int16_t* samples, uint32_t frames,
                       uint32_t channels, float gain = 1.0f);
    void clearStreamPcm(int streamUserId);

    void setPttHeld(bool held);
    bool pttHeld() const { return m_pttHeld; }
    void setWhisperHeld(bool held);
    bool whisperHeld() const { return m_whisperHeld; }
    bool isTalking() const { return m_talking; }
    bool speechActive() const { return m_speechActive; }
    QJsonObject diagnostics() const;

    // 48 kHz mono WAV: received mix (20 ms frames).
    bool startRecording(const QString& wavPath);
    void stopRecording();
    bool isRecording() const { return m_recFile != nullptr; }

signals:
    void talkingChanged(bool talking);
    void recordingChanged(bool on);
    // The user REALLY spoke (voice detection on the microphone signal,
    // independent of whether transmission is open/closed and of the PTT mode).
    // Feeds the "on speech" sound cue in voice-activated/continuous modes.
    void speechActivityChanged(bool active);
    // A remote user started (or stopped) speaking according to received voice
    // packets -- not according to the server "user_state" message.
    void remoteVoiceActivityChanged();

private:
    void captureTick();
    void updateCodecSettings();
    void sendEndpointRegistration();
    void playbackTick();
    void sweepRemoteTalking();
    void adaptVoiceTarget();
    // Smooth transmission-gate ramps: 4 ms opening (no "pop") and 60 ms
    // closing (the last transmitted frame fades to zero -- no click and no
    // abrupt mid-word cut).
    void applyGateFades(int16_t* pcm, int frames);
    // Linear factor of "Boost microphone volume" (Options > Capture), read
    // per frame (Options write straight into QSettings).
    double micGainLinear() const;
    // Closes the transmission gate (key released/voice closed) arming the
    // closing ramp -- an abrupt mid-word cut produced an audible "click".
    void closeTransmissionGate();
    // Sends the closing-ramp frames when transmission already went silent
    // because of a key (PTT released): without this the last frame would end
    // abruptly.
    void flushGateFade();
    // Speech detection (for the cue) with the microphone drained: voice is
    // "closed"/PTT released, but the sound cue must still fire when the user
    // speaks -- detection must not depend on transmission.
    void analyzeCapturedSpeech();
    void updateSpeechDetection(double rms);
    // Frames held back by EchoGuard validation: once speech is confirmed as
    // legitimate they are transmitted at once so the phrase start is not lost
    // in the confirmation delay.
    void transmitHeldEchoFrames();
    void refreshDspSettings();
    // Configured capture device (Options > Capture) or the system default --
    // same choice as the constructor, re-evaluated on every reopen.
    QAudioDevice pickCaptureDevice() const;
    // Watches QAudioSource errors (IO/removed device): capture used to die in
    // silence -- the app kept "transmitting" with no samples at all and
    // nobody heard anything (the user concluded Halla could not capture the
    // microphone). Reacts by reopening the device.
    void watchSourceState();
    // Reopens the microphone QAudioSource after an error, with an anti-loop
    // guard.
    void reopenAudioCapture();
    // Drains the capture device into m_captureBuf, updating the activity
    // clock (last time the mic delivered samples) -- feeds the stall
    // watchdog below.
    void drainCaptureDevice();
    OpusDecoder* decoderFor(int userId);
    QByteArray spatializeFrame(int userId, int16_t* mono, int frames);
    void applyRadioEffect(int userId, int16_t* mono, int frames,
                          const PluginAudioControl& control);
    // v1.1.35: cache of the HOT PATH configuration keys. The capture/playback
    // tick used to run up to ~10 QSettings lookups per 20 ms frame (VAD
    // threshold, PTT mode, guard, gains, ducking) -- dozens of QVariant/QString
    // per second of active voice. Now reloaded every 250 ms (imperceptible to
    // the user, removes hot-path churn).
    void refreshCachedConfig();
    int  m_cfgPttMode = 1;
    int  m_cfgVoiceLevelDb = -45;
    bool m_cfgCrosstalkGuard = true;
    int  m_cfgMicGainX10 = 0;
    int  m_cfgMasterVolX10 = 0;
    bool m_cfgDucking = false;
    int  m_cfgDuckingDb = 10;
    QElapsedTimer m_cfgClock;
    // v1.1.35: updateCodecSettings used to apply 4 opus_encoder_ctl calls on
    // EVERY capture tick with the same values -- now applies only on change.
    int m_lastCodecChanId = -1;
    int m_lastCodecBitrate = -1;
    int m_lastCodecApp = -1;

    NetSession* m_net;
    ServerData* m_data;
    quint64 m_pluginConnectionId = 0;
    bool m_active = false;
    bool m_txEnabled = true;
    bool m_spkEnabled = true;

    OpusEncoder* m_encoder = nullptr;
    QMap<int, OpusDecoder*> m_decoders;
    QAudioSource* m_source = nullptr;
    QAudioSink* m_sink = nullptr;
    QIODevice* m_srcDev = nullptr;
    QIODevice* m_sinkDev = nullptr;
    class QTimer* m_capTimer = nullptr;
    class QTimer* m_playTimer = nullptr;
    class QTimer* m_endpointTimer = nullptr;
    QMap<int, std::deque<QByteArray>> m_remoteQueues; // per-user stereo S16 PCM
    // Voice jitter buffer: frames are held per user until m_voiceTargetFrames
    // (20 ms each) accumulate before playback starts. Without this, any
    // network/UI delay drained the QAudioSink buffer and voice "stuttered".
    // The target is adaptive and decays slowly when stable.
    QSet<int> m_voicePrimed;
    // Initial target of 4 frames (80 ms) -- the old 3 let voice stutter on
    // any short GUI work burst -- growing to 8 frames (160 ms) on real
    // underruns and dry-outs beyond the grace period.
    int m_voiceTargetFrames = 4;
    quint64 m_voiceUnderruns = 0;
    quint64 m_voiceSheds = 0;
    quint64 m_voiceUnderrunsAtAdapt = 0;
    // Re-prime grace: when a primed user's queue dries out, the prime is kept
    // for ~300 ms so the next frame resumes IMMEDIATELY instead of waiting
    // for the prebuffer to rebuild (that was the "pauses for milliseconds"
    // bug). A dry-out beyond the grace becomes a normal re-prime and counts
    // as a jitter signal so the target grows.
    QMap<int, qint64> m_voiceDryMs;
    quint64 m_voiceDries = 0;
    quint64 m_voiceDriesAtAdapt = 0;
    class QTimer* m_voiceAdaptTimer = nullptr;
    // WebRTC live audio stays separate from voice/plugins: allows per-stream
    // mute and a small anti-jitter prebuffer without delaying the call.
    QMap<int, std::deque<QByteArray>> m_streamQueues;
    QSet<int> m_primedStreams;
    QMap<int, qint64> m_streamLastPacketMs;
    // Last instant (m_remoteVoiceClock) a voice packet arrived from each
    // remote user. Feeds the packet-driven "talking" indicator: the tree ring
    // lights up from audio that actually arrives, not only from the
    // user_state message (which can be delayed or never arrive -- e.g. state
    // stuck on the server after the speaker app froze mid-speech).
    QMap<int, qint64> m_remoteLastVoiceMs;
    // Last accepted UDP sequence per remote user: duplicate or replayed
    // datagrams (same or older seq) are dropped before decoding.
    QMap<int, quint16> m_lastVoiceSeq;
    class QTimer* m_remoteTalkingTimer = nullptr;
    QElapsedTimer m_remoteVoiceClock;
    QMap<int, RadioVoiceDsp> m_radioStates;
    QByteArray m_captureBuf;
    quint16 m_seq = 0;
    bool m_talking = false;
    bool m_speechActive = false;      // speech detection for the "on speech" cue
    qint64 m_lastSpeechAboveMs = 0;
    QElapsedTimer m_speechClock;
    // Microphone voice DSP: echo (AEC3) + noise (neural) via WebRTC APM.
    HallaAudioProcessing m_apm;
    // Crosstalk guard: prevents received voice (playing on the speaker /
    // leaking into a shared microphone) from opening transmission as if it
    // were the user's speech -- the duplicated "talking" ring in the room.
    EchoGuard m_echoGuard;
    QList<QByteArray> m_echoPending;   // frames held during validation
    class QTimer* m_dspTimer = nullptr;
    bool m_dspAnnounced = false;
    bool m_lastDspDenoise = true;
    bool m_lastDspEcho = true;
    bool m_pttHeld = false;
    bool m_whisperHeld = false;
    bool m_whisperTargetsConfigured = false;
    quint32 m_pttGen = 0;
    // Transmission gate with AUDIO hysteresis: frames keep flowing to Opus
    // while the gate is open (m_talking), regardless of the individual frame
    // RMS. Without this, any noise oscillating around the sensitivity chopped
    // voice frame by frame.
    static constexpr int kFadeInSamples = 192;    // 4 ms @ 48 kHz
    static constexpr int kFadeOutSamples = 2880;  // 60 ms @ 48 kHz
    int m_fadeInLeft = 0;
    int m_fadeOutLeft = 0;
    QElapsedTimer m_silenceClock;
    quint64 m_opusSent = 0, m_opusReceived = 0;
    quint64 m_opusSentBytes = 0, m_opusReceivedBytes = 0;
    int m_inputRms = 0;
    qint64 m_lastCaptureReopenMs = 0;   // capture reopen anti-loop
    bool m_captureReopenPending = false;
    // Stall watchdog (v1.1.34): a device that stops delivering samples
    // WITHOUT an error (driver glitch, USB power saving, Bluetooth) stayed
    // muted forever -- the state remained "active" and the v1.1.32 error
    // watchdog never fired. No samples for 2 s with the device open = stalled:
    // reopen.
    qint64 m_lastCaptureDataMs = 0;
    quint64 m_captureStallReopens = 0;

    void recWrite(const char* pcm, int bytes);
    void recFinalize();
    class QFile* m_recFile = nullptr;
    quint32 m_recBytes = 0;
};
