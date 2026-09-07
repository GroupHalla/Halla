// Gate de release para o DSP de voz (v1.1.20): compila o módulo REAL do app
// (src/audio/HallaAudioProcessing.cpp) contra o SDK que será linkado ao
// instalador e prova, em runtime, que eco e ruído de fundo são realmente
// atenuados — a mesma família de tecnologia do Krisp (AEC3 + supressão de
// ruído neural do libwebrtc).
//
// Cenários:
//   1. O módulo cria e reporta os efeitos como ativos.
//   2. Eco puro (captura idêntica ao far-end) perde >= 6 dB após adaptação.
//   3. Ruído branco estacionário (sem voz) perde >= 6 dB com NS máximo.
//
// Se este teste falhar, o instalador NÃO é publicado: a redução de
// ruído/eco teria regredido para "config que não processa nada" (exatamente
// o estado do v1.1.19 — checkboxes sem efeito algum no pipeline).

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

// LCG determinístico: mesma sequência em qualquer plataforma/compilador.
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
    // ---------- 1) eco puro: captura == far-end ----------
    {
        HallaAudioProcessing apm;
        apm.configure(/*denoise=*/false, /*level=*/0, /*echo=*/true);
        if (!apm.echoActive()) {
            std::printf("FAIL: cancelamento de eco reportado como inativo\n");
            return 20;
        }

        int16_t reverse[480] = {};
        int16_t capture[480] = {};
        double beforeDb = 0.0;
        double afterDb = 0.0;
        const int frames = 100; // 1 s de adaptação do AEC3
        for (int frame = 0; frame < frames; ++frame) {
            fillWhiteNoise(reverse, 480, 9000);
            for (size_t i = 0; i < 480; ++i) capture[i] = reverse[i]; // eco perfeito
            if (frame == frames - 20) beforeDb = rmsDb(capture, 480);
            apm.setPlayoutDelayMs(100);
            apm.processReverseFrame(reverse);
            apm.processCaptureFrame(capture);
            if (frame == frames - 1) afterDb = rmsDb(capture, 480);
        }
        const double erleDb = beforeDb - afterDb;
        std::printf("eco puro: entrada %.1f dB -> saida %.1f dB (ERLE %.1f dB)\n",
                    beforeDb, afterDb, erleDb);
        if (erleDb < 6.0) {
            std::printf("FAIL: cancelamento de eco nao atenuou o eco (ERLE %.1f dB < 6 dB)\n",
                        erleDb);
            return 22;
        }
    }

    // ---------- 2) ruído estacionário sem far-end ----------
    {
        HallaAudioProcessing apm;
        apm.configure(/*denoise=*/true, /*level=*/100, /*echo=*/false);
        if (!apm.denoiseActive()) {
            std::printf("FAIL: supressao de ruido reportada como inativa\n");
            return 23;
        }

        int16_t capture[480] = {};
        double beforeDb = 0.0;
        double afterDb = 0.0;
        const int frames = 100; // 1 s para o estimador de ruído convergir
        for (int frame = 0; frame < frames; ++frame) {
            fillWhiteNoise(capture, 480, 3000);
            if (frame == frames - 20) beforeDb = rmsDb(capture, 480);
            apm.processCaptureFrame(capture);
            if (frame == frames - 1) afterDb = rmsDb(capture, 480);
        }
        const double reductionDb = beforeDb - afterDb;
        std::printf("ruido branco: entrada %.1f dB -> saida %.1f dB (-%.1f dB)\n",
                    beforeDb, afterDb, reductionDb);
        if (reductionDb < 6.0) {
            std::printf("FAIL: supressao de ruido nao atenuou o ruido (-%.1f dB < 6 dB)\n",
                        reductionDb);
            return 24;
        }
    }

    std::printf("APM smoke OK: modulo de voz real compila, linka e processa\n");
    return 0;
}
