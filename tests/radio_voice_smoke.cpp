#include "plugins/RadioVoiceEffect.h"
#include "halla_plugin_api.h"

#include <QJsonObject>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>

static std::array<int16_t, 960> tone() {
    std::array<int16_t, 960> samples{};
    for (size_t i = 0; i < samples.size(); ++i)
        samples[i] = int16_t(std::sin(double(i) * 2.0 * 3.141592653589793
                                      * 220.0 / 48000.0) * 9000.0);
    return samples;
}

int main() {
    RadioVoiceEffect effect;
    effect.applySettings(QJsonObject{{"sendMode", "whisper"},
                                     {"receiveMode", "whisper"},
                                     {"intensity", 100}, {"noise", 0}, {"gain", 100}});

    auto normalCapture = tone();
    const auto original = normalCapture;
    if (effect.process(1, 5, HALLA_AUDIO_CAPTURE, false,
                       normalCapture.data(), normalCapture.size(), 1, 48000)) return 1;
    if (normalCapture != original) return 2;

    auto whisperCapture = tone();
    if (!effect.process(1, 5, HALLA_AUDIO_CAPTURE, true,
                        whisperCapture.data(), whisperCapture.size(), 1, 48000)) return 3;
    if (whisperCapture == original) return 4;

    auto normalRemote = tone();
    if (effect.process(1, 7, HALLA_AUDIO_REMOTE_BEFORE_SPATIAL, false,
                       normalRemote.data(), normalRemote.size(), 1, 48000)) return 5;
    if (normalRemote != original) return 6;

    auto whisperRemote = tone();
    if (!effect.process(1, 7, HALLA_AUDIO_REMOTE_BEFORE_SPATIAL, true,
                        whisperRemote.data(), whisperRemote.size(), 1, 48000)) return 7;
    if (whisperRemote == original) return 8;

    effect.applySettings(QJsonObject{{"sendMode", "normal"},
                                     {"receiveMode", "disabled"},
                                     {"intensity", 80}, {"noise", 5}, {"gain", 105}});
    auto selectedNormal = tone();
    if (!effect.process(2, 9, HALLA_AUDIO_CAPTURE, false,
                        selectedNormal.data(), selectedNormal.size(), 1, 48000)) return 9;
    auto disabledReceive = tone();
    if (effect.process(2, 8, HALLA_AUDIO_REMOTE_BEFORE_SPATIAL, true,
                       disabledReceive.data(), disabledReceive.size(), 1, 48000)) return 10;

    effect.applySettings(QJsonObject{{"sendMode", "both"}, {"receiveMode", "both"},
                                     {"intensity", 75}, {"noise", 0}, {"gain", 100}});
    auto bothNormal = tone();
    auto bothWhisper = tone();
    if (!effect.process(3, 10, HALLA_AUDIO_CAPTURE, false,
                        bothNormal.data(), bothNormal.size(), 1, 48000)) return 11;
    if (!effect.process(3, 10, HALLA_AUDIO_CAPTURE, true,
                        bothWhisper.data(), bothWhisper.size(), 1, 48000)) return 12;

    std::puts("Official radio voice DSP smoke OK");
    return 0;
}
