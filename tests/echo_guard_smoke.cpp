// Smoke do EchoGuard: detector de crosstalk/eco de rede do gate de
// transmissão. Cenários sintéticos que reproduzem o bug real:
//
//  1. Crosstalk puro — o microfone capta a voz que ESTÁ sendo recebida da
//     rede (mesma forma de onda, atenuada, atrasada no playout): a abertura
//     do gate é bloqueada e a transmissão aberta por colisão de início é
//     revogada assim que o playout recebe o correspondente.
//  2. Fala legítima com o canal ativo — outra voz no playout: a validação
//     completa (400 ms) sem casamento e o gate abre (falar por cima).
//  3. Mistura eco + fala própria — o eco não domina: não bloqueia.
//  4. Canal em silêncio — abertura instantânea (sem fonte de crosstalk).
//
// Build standalone (sem Qt): cl /std:c++20 src\audio\EchoGuard.cpp
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
            std::printf("FALHOU: %s (linha %d)\n", msg, __LINE__);           \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

namespace {
constexpr int kRate = 48000;
constexpr int kFrame = 960;
constexpr double kPi = 3.14159265358979323846;

// "Voz" sintética: f0 modulada por "sílabas", harmônicos e envelope. Duas
// instâncias com pitch diferente não correlacionam entre si.
struct VoiceGen {
    double pitchBase = 140.0, pitchWobble = 40.0;   // f0 ~ 100-180 Hz
    double syllablePeriod = 0.12;                   // cadência das sílabas
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
        // Distorção suave (microfone real não é linear).
        return int16_t(9000.0 * std::tanh(v * 0.9));
    }
    void fillFrame(int16_t* out, int frameIndex) {
        for (int i = 0; i < kFrame; ++i)
            out[i] = sample(frameIndex * kFrame + i);
    }
};

// Voz "do usuário local": timbre/pitch/cadência claramente distintos.
static void makeLocalVoice(VoiceGen& v) {
    v.pitchBase = 290.0;
    v.pitchWobble = 20.0;
    v.syllablePeriod = 0.09;
    v.h2 = 0.18;
    v.h3 = 0.05;
}

int16_t g_noiseState = 12345;
int16_t noise() { // LCG leve, ±~120
    g_noiseState = int16_t((g_noiseState * 75) % 65537);
    return int16_t((g_noiseState % 241) - 120);
}
} // namespace

// Simula um cliente: o mic capta crosstalk (voz do parceiro que toca no
// alto-falante do PC dele / mic compartilhado) e o playout recebe a mesma
// voz pela rede com delay.
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
    std::printf("crosstalk: abriu no quadro %d, revogado no %d (lag %d ms, score %.2f)\n",
                openedAtFrame, revokedAtFrame, guard.lastMatchLagMs(),
                double(guard.lastMatchScore()));
    CHECK(openedAtFrame == 0,
          "crosstalk: colisao de inicio abre no primeiro quadro (fast-path)");
    CHECK(revokedAtFrame >= delayFrames && revokedAtFrame <= delayFrames + 8,
          "crosstalk: revogacao ~200 ms apos o inicio (playout recebe o correspondente)");
    CHECK(guard.lastMatchLagMs() >= 180 && guard.lastMatchLagMs() <= 220,
          "crosstalk: lag do casamento ~= 200 ms");
    // Depois da revogacao + eco persistente, nenhuma reabertura se sustenta:
    // o mic continua acima do limiar ate o fim e o gate termina FECHADO.
    CHECK(!gateOpen, "crosstalk: gate fechado no fim (eco bloqueado)");
}

// Fala legítima por cima de um canal ativo (playout com a voz de outro):
// segura 400 ms para casar (ou nao) e abre — e a transmissão aberta NÃO é
// revogada (a mistura com a voz do parceiro não domina o microfone).
static void runLegitOverActiveChannel() {
    EchoGuard guard;
    VoiceGen remote;                     // voz do parceiro tocando
    VoiceGen mine;                       // voz propria (outra voz)
    makeLocalVoice(mine);

    const int totalFrames = 80;
    int holdFrames = 0;
    int openFrame = -1;
    bool gateOpen = false;
    for (int f = 0; f < totalFrames; ++f) {
        int16_t play[kFrame], mic[kFrame];
        remote.fillFrame(play, f);       // canal ativo o tempo todo
        mine.fillFrame(mic, f + 999);    // outra voz
        guard.notePlayout(play, kFrame);
        const EchoGuard::Decision d =
            guard.noteCapture(mic, kFrame, true, gateOpen);
        if (gateOpen) {
            // Revogação de fala legítima: NÃO pode acontecer aqui.
            CHECK(d != EchoGuard::Decision::Blocked,
                  "legitima: fala propria nao e revogada");
        } else if (d == EchoGuard::Decision::Hold) {
            ++holdFrames;
        } else if (d == EchoGuard::Decision::Open) {
            gateOpen = true;
            if (openFrame < 0) openFrame = f;
        }
    }
    std::printf("legitima: hold de %d quadros, abriu no quadro %d, gate=%d\n",
                holdFrames, openFrame, gateOpen ? 1 : 0);
    CHECK(openFrame == 20,
          "legitima: abre apos a validacao completa (20 quadros = 400 ms)");
    CHECK(holdFrames == 20, "legitima: exatamente uma validacao de 400 ms");
    CHECK(gateOpen, "legitima: gate aberto no fim (fala legitima)");
}

// Mistura: fala propria dominante + eco fraco — nao pode bloquear.
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
    std::printf("mistura 35%% eco: blocked=%d, abriu no quadro %d\n",
                blocked, openFrame);
    CHECK(blocked == 0, "mistura: eco minoritario nao bloqueia");
    CHECK(openFrame == 20, "mistura: valida 400 ms e abre");
}

// Canal silencioso: abertura instantânea (sem fonte de crosstalk).
static void runSilentChannelScenario() {
    EchoGuard guard;
    VoiceGen mine;
    int16_t silence[kFrame] = {};
    int16_t mic[kFrame];
    mine.fillFrame(mic, 7);
    guard.notePlayout(silence, kFrame);
    const EchoGuard::Decision d = guard.noteCapture(mic, kFrame, true, false);
    std::printf("silencio: decisao %d\n", int(d));
    CHECK(d == EchoGuard::Decision::Open, "silencio: abre no primeiro quadro");
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
    std::printf("EchoGuard smoke: %d falhas\n", g_failures);
    return 1;
}
