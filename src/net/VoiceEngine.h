#pragma once

#include <QObject>
#include <QElapsedTimer>
#include <QIODevice>
#include <QByteArray>
#include <QMap>
#include <deque>

class NetSession;
class QAudioSource;
class QAudioSink;
struct OpusEncoder;
struct OpusDecoder;
struct ServerData;

// Motor de voz do cliente: microfone -> Opus (48 kHz, mono, 20 ms) -> UDP,
// e pacotes recebidos -> decodificação -> mixagem -> alto-falantes.
// Degrada graciosamente se não houver dispositivo de áudio.
class VoiceEngine : public QObject {
    Q_OBJECT
public:
    explicit VoiceEngine(NetSession* net, ServerData* data, QObject* parent = nullptr);
    ~VoiceEngine() override;

    bool isActive() const { return m_active; }

    void setTransmitEnabled(bool on); // false = microfone localmente mudo/PTT suspenso
    void setSpeakersEnabled(bool on);

signals:
    void talkingChanged(bool talking);

private:
    // fluxo de captura disparado por timer de 20 ms
    void captureTick();
    // consumo da fila de jitter para reprodução
    void playbackTick();

    struct Jbuf { std::deque<QByteArray> frames; bool started = false; };
    QMap<int, Jbuf> m_jitter;

    NetSession* m_net;
    ServerData* m_data;
    bool m_active = false;
    bool m_txEnabled = true;
    bool m_spkEnabled = true;

    OpusEncoder* m_encoder = nullptr;
    OpusDecoder* m_decoder = nullptr;
    QAudioSource* m_source = nullptr;
    QAudioSink* m_sink = nullptr;
    QIODevice* m_srcDev = nullptr;
    QIODevice* m_sinkDev = nullptr;
    class QTimer* m_capTimer = nullptr;
    class QTimer* m_playTimer = nullptr;
    std::deque<QByteArray> m_playQueue; // PCM s16 pronto p/ tocar
    QByteArray m_captureBuf;
    quint16 m_seq = 0;
    bool m_talking = false;
    QElapsedTimer m_silenceClock;
};
