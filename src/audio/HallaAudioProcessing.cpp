#include "audio/HallaAudioProcessing.h"

#include <QtGlobal>
#include <cmath>
#include <cstring>

// Deliberately no AppLog/Qt-widget dependency here: this module is also
// compiled by the standalone CI gate (tests/webrtc_apm_smoke.cpp + this file
// in a single cl), which has neither moc nor the rest of the app. State
// logging lives in VoiceEngine (app side).

#ifdef HALLA_WEBRTC_NATIVE
#include <cstddef>
using nullptr_t = std::nullptr_t;
#include "api/audio/audio_processing.h"
#include "api/audio/builtin_audio_processing_builder.h"
#include "api/environment/environment_factory.h"
#else
// ---- Embedded DSP (builds without the libwebrtc SDK) ----
// RNNoise: neural-network (GRU) noise suppression, Xiph.Org, BSD-like.
// AEC MDF: acoustic echo canceller from speexdsp (Valin/Xiph), BSD-like,
//          the same algorithm free softphones have used in production for
//          two decades. Compiled into the binary via speex_aec/speex_aec.c.
extern "C" {
#include "rnnoise/rnnoise.h"
#include "speex_aec/speex_aec.h"
}
#endif

// The concrete implementation is hidden in Impl so libwebrtc headers
// (C++20 + abseil) do not leak into the rest of the app -- the rest of Halla
// keeps compiling identically in the no-SDK fallback build.
struct HallaAudioProcessing::Impl {
#ifdef HALLA_WEBRTC_NATIVE
    webrtc::scoped_refptr<webrtc::AudioProcessing> apm;
    webrtc::StreamConfig streamConfig{48000, 1};
    bool denoiseEnabled = false;
    bool echoEnabled = false;
#else
    // ---- embedded fallback ----
    // NS: RNNoise state (one per engine; ~44 KB).
    DenoiseState* ns = nullptr;
    // AEC: speex canceller (one per engine; ~hundreds of KB with the 250 ms
    // tail -- created only when echo is enabled).
    void* aec = nullptr;
    bool denoise = false;
    bool echo = false;
    // Mixes original/processed signal according to the slider level (0-100):
    // 1.0 = full RNNoise; ~0.55 = gentle suppression.
    float denoiseAlpha = 0.775f;
    // 2nd-order high-pass (rumble/DC) -- same role as the APM high_pass_filter:
    // removes sub-bass energy that only wastes bitrate and opens the VAD.
    float hpX1 = 0, hpX2 = 0, hpY1 = 0, hpY2 = 0;
    // One-frame dry delay line: RNNoise output lags the input by one 480-sample
    // frame, so the dry signal is delayed by the same amount before mixing --
    // otherwise dry/wet mixing comb-filters the voice.
    float dryDelay[480] = {};
    bool dryPrimed = false;
    // The AEC only processes with a recent far-end reference: without playout
    // there is no echo, and calling capture without playback spams warnings
    // and diverges the speex filter. Counter of frames without reverse (10 ms).
    int reverseStarved = 100000;
#endif
};

HallaAudioProcessing::HallaAudioProcessing()
    : m_impl(new Impl) {}

HallaAudioProcessing::~HallaAudioProcessing() {
#ifndef HALLA_WEBRTC_NATIVE
    if (m_impl->ns) rnnoise_destroy(m_impl->ns);
    if (m_impl->aec) halla_speex_aec_destroy(m_impl->aec);
#endif
    delete m_impl;
}

#ifdef HALLA_WEBRTC_NATIVE
static webrtc::AudioProcessing::Config::NoiseSuppression::Level nsLevelFor(int level0to100) {
    using Level = webrtc::AudioProcessing::Config::NoiseSuppression::Level;
    if (level0to100 <= 25) return Level::kLow;
    if (level0to100 <= 50) return Level::kModerate;
    if (level0to100 <= 75) return Level::kHigh;
    return Level::kVeryHigh;
}
#endif

void HallaAudioProcessing::configure(bool denoiseEnabled, int denoiseLevel0to100,
                                     bool echoCancellation) {
#ifdef HALLA_WEBRTC_NATIVE
    if (!m_impl->apm) {
        // BuiltinAudioProcessingBuilder assembles the full chain: AEC3 with a
        // neural residual-echo estimator + neural-model noise suppression +
        // high-pass filter. It is the same class of processing commercial
        // voice apps use -- and Halla "echo cancellation" stopped being a
        // checkbox without effect.
        m_impl->apm = webrtc::BuiltinAudioProcessingBuilder().Build(
            webrtc::CreateEnvironment());
        if (!m_impl->apm) return; // caller detecta via echoActive()/denoiseActive()
    }

    webrtc::AudioProcessing::Config config;
    // Full-band high-pass: cuts rumble/DC that opens the VAD for nothing and
    // wastes Opus bitrate without contributing to speech.
    config.high_pass_filter.enabled = denoiseEnabled || echoCancellation;
    config.high_pass_filter.apply_in_full_band = true;

    // AEC3: uses the far-end reference (processReverseFrame) to subtract from
    // the microphone whatever is playing on the speaker. The internal delay
    // estimator tolerates the approximate Qt sink alignment.
    config.echo_canceller.enabled = echoCancellation;

    // Neural noise suppression (wiener + speech probability): removes fans,
    // keyboards, street noise -- without the "underwater effect" of old gates.
    config.noise_suppression.enabled = denoiseEnabled;
    config.noise_suppression.level = nsLevelFor(denoiseLevel0to100);

    m_impl->apm->ApplyConfig(config);
    m_impl->denoiseEnabled = denoiseEnabled;
    m_impl->echoEnabled = echoCancellation;
#else
    // ---- embedded fallback: RNNoise + speex MDF AEC ----
    // Creates/destroys state according to the options; ApplyConfig equivalent.
    if (denoiseEnabled && !m_impl->ns) {
        m_impl->ns = rnnoise_create(nullptr); // built-in default model
    } else if (!denoiseEnabled && m_impl->ns) {
        rnnoise_destroy(m_impl->ns);
        m_impl->ns = nullptr;
    }
    if (echoCancellation && !m_impl->aec) {
        // 480-sample frames (10 ms @ 48 kHz) and a 250 ms tail: covers the
        // QAudioSink buffer (~160 ms) + the room acoustic path.
        m_impl->aec = halla_speex_aec_init(480, 480 * 25);
    } else if (!echoCancellation && m_impl->aec) {
        halla_speex_aec_destroy(m_impl->aec);
        m_impl->aec = nullptr;
    }
    m_impl->denoise = denoiseEnabled && m_impl->ns;
    m_impl->echo = echoCancellation && m_impl->aec;
    m_impl->denoiseAlpha = 0.55f + 0.45f * qBound(0, denoiseLevel0to100, 100) / 100.0f;
#endif
}

bool HallaAudioProcessing::denoiseActive() const {
#ifdef HALLA_WEBRTC_NATIVE
    return m_impl->apm && m_impl->denoiseEnabled;
#else
    return m_impl->denoise;
#endif
}

bool HallaAudioProcessing::echoActive() const {
#ifdef HALLA_WEBRTC_NATIVE
    return m_impl->apm && m_impl->echoEnabled;
#else
    return m_impl->echo;
#endif
}

#ifdef HALLA_WEBRTC_NATIVE
void HallaAudioProcessing::processCaptureFrame(int16_t* samples480) {
    if (!m_impl->apm || !samples480) return;
    if (m_impl->echoEnabled) {
        // Far-end -> echo delay hint: updated by VoiceEngine with the real
        // QAudioSink buffer level (+ device latency).
        m_impl->apm->set_stream_delay_ms(m_streamDelayMs);
    }
    m_impl->apm->ProcessStream(samples480, m_impl->streamConfig,
                               m_impl->streamConfig, samples480);
}

void HallaAudioProcessing::processReverseFrame(const int16_t* samples480) {
    if (!m_impl->apm || !samples480) return;
    // src and dest may share memory (int16 APM contract).
    m_impl->apm->ProcessReverseStream(samples480, m_impl->streamConfig,
                                      m_impl->streamConfig,
                                      const_cast<int16_t*>(samples480));
}
#else
// 2nd-order Butterworth high-pass @ 80 Hz (fs 48 kHz, Q=0.707): cuts
// rumble/DC without touching deep male voice (300 Hz passes at 99.75%).
static void highPassFrame(float* x, int n, float& x1, float& x2, float& y1, float& y2) {
    // Precise coefficients for fc=80 Hz (verified: H(10Hz)=0.016, H(80)=0.707,
    // H(300Hz)=0.998, H(1kHz)=1.000).
    constexpr float b0 = 0.99262254f, b1 = -1.98524509f, b2 = 0.99262254f;
    constexpr float a1 = -1.98519066f, a2 = 0.98529951f;
    for (int i = 0; i < n; ++i) {
        const float y = b0 * x[i] + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1; x1 = x[i];
        y2 = y1; y1 = y;
        x[i] = y;
    }
}

void HallaAudioProcessing::processCaptureFrame(int16_t* samples480) {
    if (!samples480) return;
    Impl& d = *m_impl;
    if (!d.denoise && !d.echo) return; // nothing requested: cheap passthrough

    // ---- 1. AEC (speex MDF, int16 domain) ----
    // Only with a fresh far-end reference (without playout there is no echo --
    // and speex floods the log with warnings if we capture without playback).
    ++d.reverseStarved;
    if (d.echo) {
        if (d.reverseStarved == 1) {
            // Resuming after a long pause: the acoustic path may have changed
            // (user put on/took off a headset) -- restart the filter.
            if (m_restartArmed) halla_speex_aec_reset(d.aec);
            m_restartArmed = false;
        }
        if (d.reverseStarved <= 50) { // reference within the last ~500 ms
            halla_speex_aec_capture(d.aec, samples480, samples480);
        } else if (!m_restartArmed) {
            m_restartArmed = true;   // no reverse: arm the resume reset
        }
    }

    if (!d.denoise) return; // echo only: done

    // ---- 2. NS (RNNoise, float domain) ----
    // RNNoise expects int16-scale floats (values in [-32768, 32767]): feeding
    // normalized [-1, 1] samples makes the model see near-silence and the
    // suppression never engages. The model's internal silence detector
    // calibrates with typical speech spectral energy at ~-10 dBFS; everyday
    // microphones arrive well below that. A FIXED +8 dB pre-gain (x2.5) -- no
    // dynamics, therefore no gain modulation between frames -- brings common
    // mics into the model regime, and the final division restores the
    // original scale: the model band gains are relative, so the result is
    // the same signal with attenuated noise.
    constexpr float kPreGain = 2.5f;
    float x[480];
    for (int i = 0; i < 480; ++i)
        x[i] = qBound(-32768.0f, float(samples480[i]) * kPreGain, 32767.0f);

    // ---- 3. high-pass (together with the NS, as in the APM) ----
    highPassFrame(x, 480, d.hpX1, d.hpX2, d.hpY1, d.hpY2);

    float y[480];
    rnnoise_process_frame(d.ns, y, x);

    // Slider level: original/processed mix. RNNoise gains preserve phase, so
    // linear mixing does not cancel. The dry signal is delayed by one frame
    // to align with the RNNoise output latency (480 samples).
    float dry[480];
    if (!d.dryPrimed) {
        std::memcpy(dry, x, sizeof(dry));
        d.dryPrimed = true;
    } else {
        std::memcpy(dry, d.dryDelay, sizeof(dry));
    }
    std::memcpy(d.dryDelay, x, sizeof(d.dryDelay));
    const float alpha = d.denoiseAlpha;
    const float invGain = 1.0f / kPreGain;
    for (int i = 0; i < 480; ++i) {
        const float mixed = alpha * y[i] + (1.0f - alpha) * dry[i];
        samples480[i] = int16_t(qBound(-32768.0f, mixed * invGain, 32767.0f));
    }
}

void HallaAudioProcessing::processReverseFrame(const int16_t* samples480) {
    if (!samples480) return;
    Impl& d = *m_impl;
    d.reverseStarved = 0;
    if (d.echo) halla_speex_aec_playback(d.aec, samples480);
}
#endif

void HallaAudioProcessing::setPlayoutDelayMs(int ms) {
    m_streamDelayMs = qBound(0, ms, 500);
}
