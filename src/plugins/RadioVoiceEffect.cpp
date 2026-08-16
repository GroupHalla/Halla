#include "RadioVoiceEffect.h"

#include "halla_plugin_api.h"

#include <QtGlobal>
#include <cmath>

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
    const bool capture = stage == HALLA_AUDIO_CAPTURE;
    const bool remote = stage == HALLA_AUDIO_REMOTE_BEFORE_SPATIAL;
    if ((!capture && !remote)
            || !modeMatches(capture ? m_sendMode : m_receiveMode, whisper))
        return false;

    auto& streams = m_states[connectionId];
    for (uint32_t channel = 0; channel < channels; ++channel) {
        State& state = streams[streamKey(userId, stage, channel)];
        if (state.noiseState == 0) {
            state.noiseState = 0xA341316Cu ^ quint32(userId * 2654435761u)
                ^ quint32(connectionId) ^ (stage << 8) ^ channel;
            if (state.noiseState == 0) state.noiseState = 1;
        }
        for (uint32_t frame = 0; frame < frames; ++frame) {
            const uint32_t index = frame * channels + channel;
            const float input = float(samples[index]);

            // Resposta de banda estreita semelhante a um comunicador: remove
            // graves, amortece agudos, comprime/satura e adiciona chiado.
            state.highPass = 0.94f * (state.highPass + input - state.previousInput);
            state.previousInput = input;
            state.lowPass += 0.30f * (state.highPass - state.lowPass);
            const float compressed = std::tanh(state.lowPass / 11500.0f) * 17500.0f;
            state.noiseState = state.noiseState * 1664525u + 1013904223u;
            const float random = float((state.noiseState >> 16) & 0xffffu) / 32767.5f - 1.0f;
            const float radio = compressed + random * (1900.0f * m_noise);
            const float output = (input * (1.0f - m_intensity)
                                  + radio * m_intensity) * m_gain;
            samples[index] = int16_t(qBound(-32768.0f, output, 32767.0f));
        }
    }
    return true;
}
