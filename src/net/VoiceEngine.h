#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QIODevice>
#include <QMap>
#include <QObject>
#include <cstdint>
#include <deque>

class NetSession;
class QAudioSource;
class QAudioSink;
struct OpusEncoder;
struct OpusDecoder;
struct ServerData;
struct PluginAudioControl;

// Motor de voz do cliente: microfone -> Opus (48 kHz, mono, 20 ms) -> UDP,
// e pacotes recebidos -> decodificação individual -> DSP/espacialização ->
// mixagem estéreo -> alto-falantes. Degrada graciosamente sem dispositivo.
class VoiceEngine : public QObject {
    Q_OBJECT
public:
    explicit VoiceEngine(NetSession* net, ServerData* data, QObject* parent = nullptr);
    ~VoiceEngine() override;

    bool isActive() const { return m_active; }

    void setTransmitEnabled(bool on);
    void setSpeakersEnabled(bool on);
    void setPluginConnectionId(quint64 id) { m_pluginConnectionId = id; }
    bool playPluginPcm(const int16_t* samples, uint32_t frames,
                       uint32_t channels, float gain);

    void setPttHeld(bool held);
    bool pttHeld() const { return m_pttHeld; }
    void setWhisperHeld(bool held);
    bool whisperHeld() const { return m_whisperHeld; }
    bool isTalking() const { return m_talking; }
    QJsonObject diagnostics() const;

    // WAV 48 kHz mono: mixagem recebida + próprio microfone.
    bool startRecording(const QString& wavPath);
    void stopRecording();
    bool isRecording() const { return m_recFile != nullptr; }

signals:
    void talkingChanged(bool talking);
    void recordingChanged(bool on);

private:
    void captureTick();
    void updateCodecSettings();
    void sendEndpointRegistration();
    void playbackTick();
    OpusDecoder* decoderFor(int userId);
    QByteArray spatializeFrame(int userId, int16_t* mono, int frames);
    void applyRadioEffect(int userId, int16_t* mono, int frames,
                          const PluginAudioControl& control);

    struct RadioState {
        float previousInput = 0.0f;
        float highPass = 0.0f;
        float lowPass = 0.0f;
        quint32 noiseState = 0xA341316Cu;
    };

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
    QMap<int, std::deque<QByteArray>> m_remoteQueues; // PCM S16 estéreo por usuário
    QMap<int, RadioState> m_radioStates;
    QByteArray m_captureBuf;
    quint16 m_seq = 0;
    bool m_talking = false;
    bool m_pttHeld = false;
    bool m_whisperHeld = false;
    quint32 m_pttGen = 0;
    QElapsedTimer m_silenceClock;
    quint64 m_opusSent = 0, m_opusReceived = 0;
    quint64 m_opusSentBytes = 0, m_opusReceivedBytes = 0;
    int m_inputRms = 0;

    void recWrite(const char* pcm, int bytes);
    void recFinalize();
    class QFile* m_recFile = nullptr;
    quint32 m_recBytes = 0;
};
