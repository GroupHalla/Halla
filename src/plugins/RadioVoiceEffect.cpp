#include "RadioVoiceEffect.h"

#include "halla_plugin_api.h"

#include <QtGlobal>
#include <cstdint>

void RadioVoiceEffect::applySettings(const QJsonObject& settings) {
    m_sendMode = settings.value(QStringLiteral("sendMode")).toString(QStringLiteral("whisper"));
    m_receiveMode = settings.value(QStringLiteral("receiveMode")).toString(QStringLiteral("whisper"));
    m_intensity = qBound(0.0f,
        float(settings.value(QStringLiteral("intensity")).toInt(90)) / 100.0f, 1.0f);
    m_noise = qBound(0.0f,
        float(settings.value(QStringLiteral("noise")).toInt(10)) / 100.0f, 1.0f);
    m_gain = qBound(0.50f,
        float(settings.value(QStringLiteral("gain")).toInt(105)) / 100.0f, 1.50f);
    reset();
}

void RadioVoiceEffect::reset() {
    m_states.clear();
}

void RadioVoiceEffect::resetConnection(quint64 connectionId) {
    m_states.remove(connectionId);
}

bool RadioVoiceEffect::modeMatches(const QString& mode, bool whisper) const {
    if (mode == QLatin1String("both")) return true;
    if (mode == QLatin1String("whisper")) return whisper;
    if (mode == QLatin1String("normal")) return !whisper;
    return false;
}

quint64 RadioVoiceEffect::streamKey(int userId, uint32_t stage, uint32_t channel) {
    return (quint64(stage & 0xffffu) << 48)
        | (quint64(channel & 0xffu) << 40)
        | quint64(quint32(userId));
}

bool RadioVoiceEffect::process(quint64 connectionId, int userId, uint32_t stage,
                               bool whisper, int16_t* samples, uint32_t frames,
                               uint32_t channels, uint32_t sampleRate) {
    if (!samples || frames == 0 || channels == 0 || channels > 8 || sampleRate != 48000)
        return false;
    // AFTER_VAD é a captura após a decisão de transmissão — o mesmo efeito
    // do envio, disparado pelo host depois do detector de voz.
    const bool capture = stage == HALLA_AUDIO_CAPTURE
        || stage == HALLA_AUDIO_CAPTURE_AFTER_VAD;
    const bool remote = stage == HALLA_AUDIO_REMOTE_BEFORE_SPATIAL;
    if ((!capture && !remote)
            || !modeMatches(capture ? m_sendMode : m_receiveMode, whisper))
        return false;

    auto& streams = m_states[connectionId];
    for (uint32_t channel = 0; channel < channels; ++channel) {
        RadioVoiceDsp& dsp = streams[streamKey(userId, stage, channel)];
        if (!dsp.seeded()) {
            dsp.seed(0xA341316Cu ^ quint32(userId * 2654435761u)
                ^ quint32(connectionId) ^ (stage << 8) ^ channel);
        }
        dsp.configure(m_intensity, m_noise, m_gain);
        dsp.process(samples + channel, frames, channels);
    }
    return true;
}
