#pragma once

#include <QHash>
#include <QJsonObject>
#include <QString>
#include <cstdint>

#include "RadioVoiceDsp.h"

// Official radio voice addon. Works directly on the pipeline's S16 PCM frames,
// before encoding or spatialization; the whole DSP lives in RadioVoiceDsp.h
// (shared with Halla Mobile).
class RadioVoiceEffect final {
public:
    void applySettings(const QJsonObject& settings);
    void reset();
    void resetConnection(quint64 connectionId);

    // Returns true when the frame was modified.
    bool process(quint64 connectionId, int userId, uint32_t stage,
                 bool whisper, int16_t* samples, uint32_t frames,
                 uint32_t channels, uint32_t sampleRate);

private:
    bool modeMatches(const QString& mode, bool whisper) const;
    static quint64 streamKey(int userId, uint32_t stage, uint32_t channel);

    QString m_sendMode = QStringLiteral("whisper");
    QString m_receiveMode = QStringLiteral("whisper");
    float m_intensity = 0.90f;
    float m_noise = 0.10f;
    float m_gain = 1.05f;
    QHash<quint64, QHash<quint64, RadioVoiceDsp>> m_states;
};
