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

    // WAV 48 kHz mono: mixagem recebida + próprio microfone.
    bool startRecording(const QString& wavPath);
    void stopRecording();
    bool isRecording() const { return m_recFile != nullptr; }

signals:
    void talkingChanged(bool talking);
    void recordingChanged(bool on);
    // O usuário REALMENTE falou (detecção de voz no sinal do microfone,
    // independente da transmissão estar aberta/fechada e do modo PTT).
    // Alimenta o sinal sonoro "ao falar" nos modos por voz/contínuo.
    void speechActivityChanged(bool active);
    // Um usuário remoto começou (ou parou) de falar segundo os pacotes de
    // voz recebidos — não segundo a mensagem "user_state" do servidor.
    void remoteVoiceActivityChanged();

private:
    void captureTick();
    void updateCodecSettings();
    void sendEndpointRegistration();
    void playbackTick();
    void sweepRemoteTalking();
    void adaptVoiceTarget();
    // Ramps suaves do gate de transmissão: abertura em 4 ms (sem "pop") e
    // fechamento em 60 ms (o último quadro transmitido desce para zero —
    // sem clique e sem corte seco no meio da palavra).
    void applyGateFades(int16_t* pcm, int frames);
    // Fator linear do "Aumentar volume do microfone" (Opções > Captura),
    // lido por quadro (as Opções gravam direto no QSettings).
    double micGainLinear() const;
    // Fecha o gate de transmissão (tecla solta/voz fechada) armando a rampa
    // de fechamento — o corte seco no meio de uma palavra dava "clique".
    void closeTransmissionGate();
    // Envia os quadros da rampa de fechamento quando a transmissão já calou
    // por tecla (PTT solto): sem isso o último quadro terminaria abrupto.
    void flushGateFade();
    // Detecção de fala (para o cue) com o microfone drenado: a voz está
    // "fechada"/PTT solto, mas o sinal sonoro precisa soar quando o usuário
    // fala — a detecção não pode depender da transmissão.
    void analyzeCapturedSpeech();
    void updateSpeechDetection(double rms);
    void refreshDspSettings();
    OpusDecoder* decoderFor(int userId);
    QByteArray spatializeFrame(int userId, int16_t* mono, int frames);
    void applyRadioEffect(int userId, int16_t* mono, int frames,
                          const PluginAudioControl& control);

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
    // Jitter buffer de voz: quadros ficam retidos por usuário até acumular
    // m_voiceTargetFrames (20 ms cada) antes de começar a tocar. Sem isso,
    // qualquer atraso de rede/UI estourava o buffer do QAudioSink e a voz
    // "pipocava". O alvo é adaptativo e decai devagar quando estável.
    QSet<int> m_voicePrimed;
    // Alvo inicial de 4 quadros (80 ms) — o antigo 3 deixava a voz pipocar
    // em qualquer rajada curta de trabalho da GUI — crescendo até 8 quadros
    // (160 ms) em underruns reais.
    int m_voiceTargetFrames = 4;
    quint64 m_voiceUnderruns = 0;
    quint64 m_voiceSheds = 0;
    quint64 m_voiceUnderrunsAtAdapt = 0;
    class QTimer* m_voiceAdaptTimer = nullptr;
    // Áudio WebRTC das lives fica separado da voz/plugins: permite mudo por
    // transmissão e um pequeno prebuffer contra jitter sem atrasar a chamada.
    QMap<int, std::deque<QByteArray>> m_streamQueues;
    QSet<int> m_primedStreams;
    QMap<int, qint64> m_streamLastPacketMs;
    // Último instante (m_remoteVoiceClock) em que chegou um pacote de voz de
    // cada usuário remoto. Alimenta o indicador "falando" orientado a
    // pacotes: o anel na árvore acende pelo áudio que realmente chega, não
    // apenas pela mensagem user_state (que pode atrasar ou nunca chegar —
    // ex.: estado preso no servidor após o app do falante congelar no meio
    // de uma fala).
    QMap<int, qint64> m_remoteLastVoiceMs;
    class QTimer* m_remoteTalkingTimer = nullptr;
    QElapsedTimer m_remoteVoiceClock;
    QMap<int, RadioVoiceDsp> m_radioStates;
    QByteArray m_captureBuf;
    quint16 m_seq = 0;
    bool m_talking = false;
    bool m_speechActive = false;      // detecção de fala para o cue "ao falar"
    qint64 m_lastSpeechAboveMs = 0;
    QElapsedTimer m_speechClock;
    // DSP de voz do microfone: eco (AEC3) + ruído (neural) via WebRTC APM.
    HallaAudioProcessing m_apm;
    class QTimer* m_dspTimer = nullptr;
    bool m_dspAnnounced = false;
    bool m_lastDspDenoise = true;
    bool m_lastDspEcho = true;
    bool m_pttHeld = false;
    bool m_whisperHeld = false;
    bool m_whisperTargetsConfigured = false;
    quint32 m_pttGen = 0;
    // Gate de transmissão com histerese de ÁUDIO: quadros seguem para o
    // Opus enquanto o gate está aberto (m_talking), independentemente do
    // RMS do quadro individual. Sem isso, qualquer ruído oscilando em
    // torno da sensibilidade picotava a voz frame a frame.
    static constexpr int kFadeInSamples = 192;    // 4 ms @ 48 kHz
    static constexpr int kFadeOutSamples = 2880;  // 60 ms @ 48 kHz
    int m_fadeInLeft = 0;
    int m_fadeOutLeft = 0;
    QElapsedTimer m_silenceClock;
    quint64 m_opusSent = 0, m_opusReceived = 0;
    quint64 m_opusSentBytes = 0, m_opusReceivedBytes = 0;
    int m_inputRms = 0;

    void recWrite(const char* pcm, int bytes);
    void recFinalize();
    class QFile* m_recFile = nullptr;
    quint32 m_recBytes = 0;
};
