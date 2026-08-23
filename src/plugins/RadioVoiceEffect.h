#pragma once

#include <QHash>
#include <QJsonObject>
#include <QString>
#include <cstdint>

// DSP interno do complemento oficial de voz por rádio. Trabalha diretamente
// nos quadros PCM S16 do pipeline, antes da codificação ou da espacialização.
class RadioVoiceEffect final {
public:
    void applySettings(const QJsonObject& settings);
    void reset();
    void resetConnection(quint64 connectionId);

    // Retorna true quando o quadro foi alterado.
    bool process(quint64 connectionId, int userId, uint32_t stage,
                 bool whisper, int16_t* samples, uint32_t frames,
                 uint32_t channels, uint32_t sampleRate);

private:
    struct State {
        float previousInput = 0.0f;
        float highPass = 0.0f;
        float lowPass = 0.0f;
        quint32 noiseState = 0;
    };

    bool modeMatches(const QString& mode, bool whisper) const;
    static quint64 streamKey(int userId, uint32_t stage, uint32_t channel);

    QString m_sendMode = QStringLiteral("whisper");
    QString m_receiveMode = QStringLiteral("whisper");
    float m_intensity = 0.90f;
    float m_noise = 0.10f;
    float m_gain = 1.05f;
    QHash<quint64, QHash<quint64, State>> m_states;
};
