#pragma once

// ---------------------------------------------------------------------------
// RadioVoiceDsp -- DSP of the official "Police radio voice" addon.
//
// Single header, no Qt dependency, shared between Halla Desktop
// (src/plugins/RadioVoiceDsp.h) and Halla Mobile
// (app/src/main/cpp/RadioVoiceDsp.h, faithful copy -- keep the two in sync).
//
// Processing chain (48 kHz, S16 PCM, one channel per instance):
//   1. Peak AGC (fast attack, ~160 ms release): voice always "pinned", like
//      a radio microphone crushing speech dynamics;
//   2. Narrow radiocomm band: 2x ~350 Hz high-passes followed by 2x ~3.1 kHz
//      low-passes -- the "thin" PMR transceiver voice;
//   3. High-drive tanh saturation + coarse quantization: the distorted
//      "shout" of a saturated transmitter;
//   4. Small-speaker resonance (peak ~1.9 kHz, +5 dB): the acoustic body of
//      the "little radio";
//   5. Voice-activity detector with adaptive threshold and hysteresis that
//      opens/closes the squelch: pause becomes faint static, speech returns
//      to full volume (5% attenuation with the channel closed);
//   6. Band-limited static (noise passes through a ~1.7 kHz filter, not
//      full-spectrum white hiss) with rare propagation crackles;
//   7. Squelch noise on open (short click) and on close (decaying burst
//      ~146 ms + 1750 Hz beep -- the end-of-transmission "pshhh-beep");
//   8. Slow signal wobble (+/-6%, changing every 2-8 s): wireless fading.
//
// All coefficients below are precomputed for 48 kHz. Derivations:
//   1st-order high-pass  y[n] = a*(y[n-1] + x[n] - x[n-1]), a = 1/(1+2*pi*fc/fs)
//     fc=350 Hz  -> a = 0.95630
//   1st-order low-pass   y[n] += g*(x[n] - y[n]), g = 1-exp(-2*pi*fc/fs)
//     fc=3100 Hz -> g = 0.33368 ; fc=1700 Hz -> g = 0.20
//   resonance (RBJ peaking biquad, fc=1900 Hz, Q=2.2, +5 dB, normalized):
//     y[n] = 1.031323*x[n] - 1.860469*x[n-1] + 0.888114*x[n-2]
//          + 1.860469*y[n-1] - 0.919482*y[n-2]
//   exponential decays per sample (a = exp(-1/(fs*tau))):
//     tau=160 ms -> 0.999861 ; tau=120 ms -> 0.999826 ; tau=2.5 s -> 0.999992
//     tau=45 ms  -> 0.999537
// ---------------------------------------------------------------------------

#include <cmath>
#include <cstddef>
#include <cstdint>

class RadioVoiceDsp final {
public:
    // Settings in normalized units: intensity/noise in [0, 1] and gain in
    // [0.5, 1.5]. May be reconfigured at any time (per frame); filters and
    // squelch preserve state between calls.
    void configure(float intensity, float noise, float gain) {
        m_intensity = clampRange(intensity, 0.0f, 1.0f);
        m_noise = clampRange(noise, 0.0f, 1.0f);
        m_gain = clampRange(gain, 0.5f, 1.5f);
    }

    bool seeded() const { return m_rng != 0u; }

    // Deterministic seed per stream (user/connection/channel). Only takes
    // effect on the first call: later reconfigurations neither reseed the
    // noise nor restart the filters.
    void seed(uint32_t s) {
        if (m_rng != 0u) return;
        uint32_t v = s ? s : 1u;
        // Discards the first LCG outputs to decorrelate close seeds (e.g.
        // consecutive user ids).
        for (int i = 0; i < 4; ++i) v = v * 1664525u + 1013904223u;
        m_rng = v ? v : 1u;
    }

    // Processes `frames` samples every `stride` positions starting at
    // `samples`. stride = channel count of the interleaved buffer (pass
    // samples + channel and stride = channels); 1 for mono.
    void process(int16_t* samples, uint32_t frames, uint32_t stride = 1) {
        if (!samples || frames == 0 || stride == 0) return;
        for (uint32_t f = 0; f < frames; ++f) {
            int16_t* cell = samples + std::ptrdiff_t(f) * stride;
            const float input = float(*cell) * (1.0f / 32768.0f);
            const float output = processSample(input);
            *cell = int16_t(clampRange(output * 32768.0f, -32768.0f, 32767.0f));
        }
    }

private:
    static float clampRange(float v, float lo, float hi) {
        return v < lo ? lo : (v > hi ? hi : v);
    }

    float nextNoise() {
        m_rng = m_rng * 1664525u + 1013904223u;
        return float((m_rng >> 16) & 0xffffu) * (1.0f / 32767.5f) - 1.0f;
    }

    float processSample(float input) {
        // --- 1) AGC de pico ------------------------------------------------
        const float absIn = std::fabs(input);
        if (absIn > m_env) m_env += 0.35f * (absIn - m_env);
        else m_env *= 0.999861f;                    // ~160 ms release
        float wanted = 0.36f / (m_env > 0.004f ? m_env : 0.004f);
        wanted = clampRange(wanted, 0.25f, 12.0f);
        m_agcGain += 0.003f * (wanted - m_agcGain); // ~7 ms smoothing
        const float leveled = input * m_agcGain;

        // --- 2) Banda estreita ~350 Hz a ~3,1 kHz --------------------------
        m_hp1 = 0.95630f * (m_hp1 + leveled - m_hp1x); m_hp1x = leveled;
        m_hp2 = 0.95630f * (m_hp2 + m_hp1 - m_hp2x);   m_hp2x = m_hp1;
        m_lp1 += 0.33368f * (m_hp2 - m_lp1);
        m_lp2 += 0.33368f * (m_lp1 - m_lp2);
        const float band = m_lp2;

        // --- 3) Transmitter saturation + coarse quantization ---------------
        float crushed = std::tanh(band * 2.6f) * 0.60f;
        crushed = std::round(crushed * 48.0f) / 48.0f;

        // --- 4) Small-speaker resonance ------------------------------------
        const float speaker = 1.031323f * crushed - 1.860469f * m_resX1
            + 0.888114f * m_resX2 + 1.860469f * m_resY1 - 0.919482f * m_resY2;
        m_resX2 = m_resX1; m_resX1 = crushed;
        m_resY2 = m_resY1; m_resY1 = speaker;

        // --- 5) Squelch: VAD with adaptive threshold and hysteresis --------
        const float absBand = std::fabs(band);
        if (absBand > m_vEnv) m_vEnv += 0.25f * (absBand - m_vEnv);
        else m_vEnv *= 0.999826f;                   // ~120 ms release
        m_vSlow = absBand > m_vSlow ? absBand : m_vSlow * 0.999992f; // ~2,5 s
        const float threshold = m_vSlow * 0.10f > 0.018f ? m_vSlow * 0.10f : 0.018f;
        if (!m_open) {
            if (m_vEnv > threshold * 1.6f) {
                m_open = true;
                m_hold = 0;
                m_click = 128;                      // opening click
                m_clickAmp = 0.30f;
            }
        } else {
            m_hold = m_vEnv > threshold ? 0u : m_hold + 1u;
            if (m_hold > 24000u) {                  // 500 ms below threshold
                m_open = false;
                m_tail = 7000;                      // ~146 ms burst
                m_tailAmp = 0.20f + 0.30f * m_noise;
                m_beepPhase = 0.0f;
            }
        }

        // --- 6) Band-limited static + rare crackles ------------------------
        const float white = nextNoise();
        m_nLp += 0.20f * (white - m_nLp);           // ~1.7 kHz low-pass
        m_rng = m_rng * 1664525u + 1013904223u;
        if ((m_rng >> 12) < 12u) m_crackle = white * 0.9f; // ~0.5 crackle/s
        m_crackle *= 0.86f;
        const float noiseAmp = m_open
            ? 0.040f + 0.150f * m_noise
            : 0.010f + 0.045f * m_noise;
        const float staticNoise = m_nLp * noiseAmp + m_crackle * 0.050f;

        // --- 7) Opening click and closing burst/beep -----------------------
        float burst = 0.0f;
        if (m_click > 0) {
            burst += white * m_clickAmp;
            m_clickAmp *= 0.88f;
            --m_click;
        }
        if (m_tail > 0) {
            --m_tail;
            burst += m_nLp * m_tailAmp;             // "pshhh" burst
            m_tailAmp *= 0.999537f;                 // ~45 ms decay
            if (m_tail > 3500) {                   // beep in the first ~73 ms
                m_beepPhase += 0.229336f;           // 1750 Hz @ 48 kHz
                burst += std::sin(m_beepPhase) * 0.09f
                    * float(m_tail - 3500) / 3500.0f;
            }
        }

        // --- 8) Slow signal wobble -----------------------------------------
        if (m_wobbleLeft > 0) {
            --m_wobbleLeft;
        } else {
            m_rng = m_rng * 1664525u + 1013904223u;
            m_wobbleLeft = 96000u + (m_rng % 288000u);      // 2-8 s
            m_rng = m_rng * 1664525u + 1013904223u;
            m_wobbleTarget = 0.94f + 0.12f * float(m_rng % 1000u) / 999.0f;
        }
        m_wobble += (m_wobbleTarget - m_wobble) * 0.0002f;

        // --- Output: squelch gate + dry/wet mix + final saturation ---------
        const float voice = m_open ? speaker : speaker * 0.05f;
        float wet = (voice + staticNoise + burst) * m_wobble;
        wet = std::tanh(wet * 1.1f) * 0.90f;        // guaranteed headroom
        return (input * (1.0f - m_intensity) + wet * m_intensity) * m_gain;
    }

    // Current settings (configure()).
    float m_intensity = 0.90f;
    float m_noise = 0.10f;
    float m_gain = 1.05f;

    // DSP state (one per audio stream).
    uint32_t m_rng = 0;            // deterministic noise seed
    float m_env = 0.0f;            // AGC peak follower
    float m_agcGain = 1.0f;        // smoothed AGC gain
    float m_hp1 = 0.0f, m_hp1x = 0.0f, m_hp2 = 0.0f, m_hp2x = 0.0f;
    float m_lp1 = 0.0f, m_lp2 = 0.0f;
    float m_resX1 = 0.0f, m_resX2 = 0.0f, m_resY1 = 0.0f, m_resY2 = 0.0f;
    float m_vEnv = 0.0f;           // fast post-band voice envelope
    float m_vSlow = 0.0f;          // slow tracker (adaptive threshold)
    uint32_t m_hold = 0;           // samples below threshold with channel open
    bool m_open = false;           // squelch open (someone speaking)
    float m_nLp = 0.0f;            // filtered static
    float m_crackle = 0.0f;        // decaying propagation crackle
    int m_tail = 0;                // remaining squelch-burst samples
    float m_tailAmp = 0.0f;
    float m_beepPhase = 0.0f;      // 1750 Hz beep phase
    int m_click = 0;               // remaining opening-click samples
    float m_clickAmp = 0.0f;
    uint32_t m_wobbleLeft = 0;     // samples until the next wobble
    float m_wobble = 1.0f;         // current signal wobble
    float m_wobbleTarget = 1.0f;
};
