// EchoGuard smoke test: network echo/crosstalk detector of the transmit gate.
// Synthetic scenarios that reproduce the real bug:
//
//  1. Pure crosstalk -- the microphone picks up the voice that IS being
//     received from the network (same waveform, attenuated, delayed by the
//     playout): opening the gate is blocked and a transmission opened by a
//     start collision is revoked as soon as the playout receives the matching
//     frame.
//  2. Genuine speech while the channel is active -- another voice in the
//     playout: the full validation window (400 ms) finds no match and the gate
//     opens (talking over the other side).
//  3. Echo plus own speech -- the echo does not dominate: no blocking.
//  4. Silent channel -- instant opening (no crosstalk source).
//
// Standalone build (no Qt): cl /std:c++20 src\audio\EchoGuard.cpp
// tests\echo_guard_smoke.cpp

#include "../src/audio/EchoGuard.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static int g_failures = 0;

#define CHECK(cond, msg)                                                     \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::printf("FAILED: %s (line %d)\n", msg, __LINE__);           \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

namespace {
constexpr int kRate = 48000;
constexpr int kFrame = 960;
constexpr double kPi = 3.14159265358979323846;

// Synthetic "voice": f0 modulated by "syllables", harmonics and envelope. Two
// instances with different pitch do not correlate with each other.
struct VoiceGen {
    double pitchBase = 140.0, pitchWobble = 40.0;   // f0 ~ 100-180 Hz
    double syllablePeriod = 0.12;                   // syllable rate
    double h2 = 0.35, h3 = 0.12;                    // timbre
    double phase = 0.0;
    int16_t sample(int n) {
        const double t = double(n) / kRate;
        const double f0 = pitchBase + pitchWobble * std::sin(2 * kPi * t / 0.7);
        phase += 2 * kPi * f0 / kRate;
        const double syl = std::fmod(t, syllablePeriod);
        const double on = syllablePeriod * 0.15;
        const double off = syllablePeriod * 0.85;
        double env = 1.0;
        if (syl < on) env = 0.25;
        else if (syl > off) env = 0.45;
        const double v = env * (std::sin(phase)
                                + h2 * std::sin(2 * phase)
                                + h3 * std::sin(3 * phase));
        // Gentle distortion (a real microphone is not linear).
        return int16_t(9000.0 * std::tanh(v * 0.9));
    }
    void fillFrame(int16_t* out, int frameIndex) {
        for (int i = 0; i < kFrame; ++i)
            out[i] = sample(frameIndex * kFrame + i);
    }
};

// "Local user" voice: clearly distinct timbre/pitch/syllable rate.
static void makeLocalVoice(VoiceGen& v) {
    v.pitchBase = 290.0;
    v.pitchWobble = 20.0;
    v.syllablePeriod = 0.09;
    v.h2 = 0.18;
    v.h3 = 0.05;
}

int16_t g_noiseState = 12345;
int16_t noise() { // light LCG, about +-120
    g_noiseState = int16_t((g_noiseState * 75) % 65537);
    return int16_t((g_noiseState % 241) - 120);
}
} // namespace

// Simulates one client: the microphone picks up crosstalk (the partner voice
// playing on their PC speaker / a shared microphone) and the playout receives
// the same voice over the network with a delay.
static void runCrosstalkScenario() {
    EchoGuard guard;
    VoiceGen remote;                     // voz do parceiro (o que a rede entrega)
    VoiceGen localSame = remote;         // a MESMA voz no mic (crosstalk)

    const int delayFrames = 10;          // 200 ms de rede+jitter+playout
    const int totalFrames = 120;         // 2,4 s de fala
    int heldFrames = 0, openedAtFrame = -1, revokedAtFrame = -1;
    bool gateOpen = false;

    for (int f = 0; f < totalFrames; ++f) {
        int16_t play[kFrame];
        if (f >= delayFrames)
            remote.fillFrame(play, f - delayFrames);   // o que toca agora
        else
            std::memset(play, 0, sizeof(play));

        int16_t mic[kFrame];
        for (int i = 0; i < kFrame; ++i)
            mic[i] = int16_t(int(localSame.sample(f * kFrame + i)) * 0.6)
                     + noise();

        guard.notePlayout(play, kFrame);
        const double rms = [] (const int16_t* p) {
            double s = 0;
            for (int i = 0; i < kFrame; ++i) s += double(p[i]) * double(p[i]);
            return std::sqrt(s / kFrame);
        }(mic);
        const EchoGuard::Decision d =
            guard.noteCapture(mic, kFrame, rms > 900.0, gateOpen);

        if (gateOpen) {
            if (d == EchoGuard::Decision::Blocked && revokedAtFrame < 0)
                revokedAtFrame = f;                     // revogou o eco
            if (d == EchoGuard::Decision::Blocked) gateOpen = false;
        } else {
            if (d == EchoGuard::Decision::Hold) ++heldFrames;
            if (d == EchoGuard::Decision::Open) {
                gateOpen = true;
                if (openedAtFrame < 0) openedAtFrame = f;
            }
        }
    }
    std::printf("crosstalk: opened at frame %d, revoked at %d (lag %d ms, score %.2f)\n",
                openedAtFrame, revokedAtFrame, guard.lastMatchLagMs(),
                double(guard.lastMatchScore()));
    CHECK(openedAtFrame == 0,
          "crosstalk: start collision opens on the first frame (fast path)");
    CHECK(revokedAtFrame >= delayFrames && revokedAtFrame <= delayFrames + 8,
          "crosstalk: revoked ~200 ms after opening (playout receives the match)");
    CHECK(guard.lastMatchLagMs() >= 180 && guard.lastMatchLagMs() <= 220,
          "crosstalk: match lag ~= 200 ms");
    // After the revocation and with the echo persisting, no reopening holds:
    // the microphone stays above the threshold until the end and the gate
    // finishes CLOSED.
    CHECK(!gateOpen, "crosstalk: gate closed at the end (echo blocked)");
}

// Genuine speech on top of an active channel (playout carrying another voice):
// holds for 400 ms looking for a match and then opens -- and the transmission
// that was opened is NOT revoked (the mix with the partner voice does not
// dominate the microphone).
static void runLegitOverActiveChannel() {
    EchoGuard guard;
    VoiceGen remote;                     // partner voice playing
    VoiceGen mine;                       // own voice (a different voice)
    makeLocalVoice(mine);

    const int totalFrames = 80;
    int holdFrames = 0;
    int openFrame = -1;
    bool gateOpen = false;
    for (int f = 0; f < totalFrames; ++f) {
        int16_t play[kFrame], mic[kFrame];
        remote.fillFrame(play, f);       // channel active the whole time
        mine.fillFrame(mic, f + 999);    // another voice
        guard.notePlayout(play, kFrame);
        const EchoGuard::Decision d =
            guard.noteCapture(mic, kFrame, true, gateOpen);
        if (gateOpen) {
            // Revoking genuine speech: must NOT happen here.
            CHECK(d != EchoGuard::Decision::Blocked,
                  "genuine speech: own voice is not revoked");
        } else if (d == EchoGuard::Decision::Hold) {
            ++holdFrames;
        } else if (d == EchoGuard::Decision::Open) {
            gateOpen = true;
            if (openFrame < 0) openFrame = f;
        }
    }
    std::printf("genuine echo: %d-frame hold, opened at frame %d, gate=%d\n",
                holdFrames, openFrame, gateOpen ? 1 : 0);
    CHECK(openFrame == 20,
          "genuine speech: opens after the full validation (20 frames = 400 ms)");
    CHECK(holdFrames == 20, "genuine speech: exactly one 400 ms validation");
    CHECK(gateOpen, "genuine speech: gate open at the end (genuine speech)");
}

// Mix: own speech dominant plus weak echo -- must not block.
static void runMixedEchoScenario() {
    EchoGuard guard;
    VoiceGen remote, mine;
    makeLocalVoice(mine);
    const int totalFrames = 60;
    int blocked = 0, openFrame = -1;
    for (int f = 0; f < totalFrames; ++f) {
        int16_t play[kFrame], mic[kFrame];
        remote.fillFrame(play, f);
        mine.fillFrame(mic, f + 4242);
        for (int i = 0; i < kFrame; ++i)
            mic[i] = int16_t(int(mic[i]) + int(play[i]) * 2 / 5); // ~35% eco
        guard.notePlayout(play, kFrame);
        const EchoGuard::Decision d =
            guard.noteCapture(mic, kFrame, true, false);
        if (d == EchoGuard::Decision::Blocked) ++blocked;
        if (d == EchoGuard::Decision::Open && openFrame < 0) openFrame = f;
    }
    std::printf("35%% echo mix: blocked=%d, opened at frame %d\n",
                blocked, openFrame);
    CHECK(blocked == 0, "mix: a minority echo does not block");
    CHECK(openFrame == 20, "mix: validates 400 ms and opens");
}

// Silent channel: instant opening (no crosstalk source).
static void runSilentChannelScenario() {
    EchoGuard guard;
    VoiceGen mine;
    int16_t silence[kFrame] = {};
    int16_t mic[kFrame];
    mine.fillFrame(mic, 7);
    guard.notePlayout(silence, kFrame);
    const EchoGuard::Decision d = guard.noteCapture(mic, kFrame, true, false);
    std::printf("silence: decision %d\n", int(d));
    CHECK(d == EchoGuard::Decision::Open, "silence: opens on the first frame");
}

int main() {
    runCrosstalkScenario();
    runLegitOverActiveChannel();
    runMixedEchoScenario();
    runSilentChannelScenario();
    if (g_failures == 0) {
        std::printf("EchoGuard smoke: OK\n");
        return 0;
    }
    std::printf("EchoGuard smoke: %d failure(s)\n", g_failures);
    return 1;
}
