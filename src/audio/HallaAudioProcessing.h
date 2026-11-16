#pragma once

#include <cstdint>

// Halla microphone voice DSP.
//
// Two implementations, same contract:
//
//   * With the libwebrtc SDK (HALLA_WEBRTC_NATIVE): the full APM chain --
//     AEC3 with a neural residual-echo estimator + neural noise suppression
//     + high-pass filter. The same family of voice-communication technology
//     used by Krisp/Discord.
//
//   * Builds without the SDK (the current official release): EMBEDDED DSP,
//     compiled into the binary -- the noise-reduction/echo checkboxes stop
//     being decoration. Neural noise suppression via RNNoise (Xiph, BSD) +
//     AUMDF acoustic echo canceller from speexdsp (Xiph, BSD) + 80 Hz
//     high-pass. No external dependencies.
//
// Contract:
//   * 480-sample mono frames @ 48 kHz (10 ms), processed IN PLACE.
//   * processCaptureFrame() = microphone (before VAD/plugins/encoder).
//   * processReverseFrame() = far-end (the mix going to the speaker) -- the
//     reference the AEC uses to subtract echo.
//   * NOT thread-safe: all calls must come from the same thread (in Halla,
//     the GUI thread, where captureTick/playbackTick live).
//
class HallaAudioProcessing {
public:
    HallaAudioProcessing();
    ~HallaAudioProcessing();

    HallaAudioProcessing(const HallaAudioProcessing&) = delete;
    HallaAudioProcessing& operator=(const HallaAudioProcessing&) = delete;

    // (Re)applies the configuration. May be called at any time to follow
    // Options changes -- ApplyConfig is cheap when nothing changed.
    // denoiseLevel0to100: 0-25 low, 26-50 moderate, 51-75 high, 76-100 maximum.
    void configure(bool denoiseEnabled, int denoiseLevel0to100, bool echoCancellation);

    bool denoiseActive() const;
    bool echoActive() const;

    // Microphone, 10 ms, in place. Applies HPF + AEC3 + NS in APM order.
    void processCaptureFrame(int16_t* samples480);
    // Far-end (what plays on the speaker), 10 ms. Feeds the AEC3.
    void processReverseFrame(const int16_t* samples480);
    // Estimated delay between far-end entering here and echo returning on the
    // microphone (QAudioSink buffer + device latency). Improves convergence.
    void setPlayoutDelayMs(int ms);

private:
    struct Impl;
    Impl* m_impl = nullptr;
    int m_streamDelayMs = 100;
    // Embedded fallback: arms an AEC reset when the far-end reference was
    // absent for a long period (muted speaker / total silence) and just came
    // back -- the acoustic path may have changed.
    bool m_restartArmed = false;
};
