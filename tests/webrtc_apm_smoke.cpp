// Release gate for the voice DSP (v1.1.20): compiles the REAL app module
// (src/audio/HallaAudioProcessing.cpp) against the SDK that ships in the
// installer and proves, at runtime, that echo and background noise are really
// attenuated -- the same family of technology as Krisp (AEC3 + neural noise
// suppression from libwebrtc).
//
// Scenarios:
//   1. The module creates the effects and reports them as active.
//   2. Pure echo (capture identical to the far-end) loses >= 6 dB after
//      adaptation.
//   3. Stationary white noise (no voice) loses >= 6 dB at maximum NS.
//
// If this test fails the installer is NOT published: noise/echo reduction would
// have regressed to "config that processes nothing" (exactly the v1.1.19 state
// -- checkboxes with no effect at all on the pipeline).
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cmath>

#include <QtGlobal>

#include "audio/HallaAudioProcessing.h"

namespace {

double rmsDb(const int16_t* samples, size_t count) {
    if (!samples || count == 0) return -200.0;
    double sum = 0.0;
    for (size_t i = 0; i < count; ++i) sum += double(samples[i]) * double(samples[i]);
    const double rms = std::sqrt(sum / double(count));
    if (rms <= 0.0) return -200.0;
    return 20.0 * std::log10(rms / 32767.0);
}

// Deterministic LCG: the same sequence on any platform/compiler.
uint32_t lcgState = 0x12345678u;
uint32_t nextRandom() {
    lcgState = lcgState * 1664525u + 1013904223u;
    return lcgState >> 8;
}

void fillWhiteNoise(int16_t* out, size_t count, int amplitude) {
    for (size_t i = 0; i < count; ++i)
        out[i] = int16_t(int(nextRandom() % (2u * uint32_t(amplitude))) - amplitude);
}

} // namespace

int main() {
    // ---------- 1) pure echo: capture == far-end ----------
    {
        HallaAudioProcessing apm;
        apm.configure(/*denoise=*/false, /*level=*/0, /*echo=*/true);
        if (!apm.echoActive()) {
            std::printf("FAIL: echo cancellation reported as inactive\n");
            return 20;
        }

        int16_t reverse[480] = {};
        int16_t capture[480] = {};
        double beforeDb = 0.0;
        double afterDb = 0.0;
        const int frames = 100; // 1 s of AEC3 adaptation
        for (int frame = 0; frame < frames; ++frame) {
            fillWhiteNoise(reverse, 480, 9000);
            for (size_t i = 0; i < 480; ++i) capture[i] = reverse[i]; // perfect echo
            if (frame == frames - 20) beforeDb = rmsDb(capture, 480);
            apm.setPlayoutDelayMs(100);
            apm.processReverseFrame(reverse);
            apm.processCaptureFrame(capture);
            if (frame == frames - 1) afterDb = rmsDb(capture, 480);
        }
        const double erleDb = beforeDb - afterDb;
        std::printf("pure echo: input %.1f dB -> output %.1f dB (ERLE %.1f dB)\n",
                    beforeDb, afterDb, erleDb);
        if (erleDb < 6.0) {
            std::printf("FAIL: echo cancellation did not attenuate the echo (ERLE %.1f dB < 6 dB)\n",
                        erleDb);
            return 22;
        }
    }

    // ---------- 2) stationary noise without far-end ----------
    {
        HallaAudioProcessing apm;
        apm.configure(/*denoise=*/true, /*level=*/100, /*echo=*/false);
        if (!apm.denoiseActive()) {
            std::printf("FAIL: noise suppression reported as inactive\n");
            return 23;
        }

        int16_t capture[480] = {};
        double beforeDb = 0.0;
        double afterDb = 0.0;
        const int frames = 100; // 1 s for the noise estimator to converge
        for (int frame = 0; frame < frames; ++frame) {
            fillWhiteNoise(capture, 480, 3000);
            if (frame == frames - 20) beforeDb = rmsDb(capture, 480);
            apm.processCaptureFrame(capture);
            if (frame == frames - 1) afterDb = rmsDb(capture, 480);
        }
        const double reductionDb = beforeDb - afterDb;
        std::printf("white noise: input %.1f dB -> output %.1f dB (-%.1f dB)\n",
                    beforeDb, afterDb, reductionDb);
        if (reductionDb < 6.0) {
            std::printf("FAIL: noise suppression did not attenuate the noise (-%.1f dB < 6 dB)\n",
                        reductionDb);
            return 24;
        }
    }

    std::printf("APM smoke OK: the real voice module compiles, links and processes\n");
    return 0;
}
