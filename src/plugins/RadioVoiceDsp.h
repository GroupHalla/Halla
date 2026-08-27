#pragma once

// ---------------------------------------------------------------------------
// RadioVoiceDsp — DSP do complemento oficial "Voz de rádio policial".
//
// Header único, sem dependência de Qt, para ser compartilhado entre o Halla
// Desktop (src/plugins/RadioVoiceDsp.h) e o Halla Mobile
// (app/src/main/cpp/RadioVoiceDsp.h, cópia fiel — manter os dois sincronizados).
//
// Cadeia de processamento (48 kHz, PCM S16, um canal por instância):
//   1. AGC de pico (ataque rápido, liberação ~160 ms): voz sempre "no talo",
//      como o microfone de um rádio amassando a dinâmica da fala;
//   2. Banda estreita de radiocomunicação: 2x passa-altas ~420 Hz seguidos de
//      2x passa-baixas ~3,1 kHz — a voz "fininha" de transceptor PMR;
//   3. Saturação tanh com drive alto + quantização grosseira: o "grito"
//      distorcido de transmissor saturado;
//   4. Ressonância de alto-falante pequeno (pico ~1,9 kHz, +5 dB): o corpo
//      acústico do "radinho";
//   5. Detector de atividade de voz com limiar adaptativo e histerese que
//      abre e fecha o squelch: pausa vira estática fraca, fala volta ao
//      volume cheio (atenuação de 5% com o canal fechado);
//   6. Estática limitada à banda (ruído passa por filtro ~1,7 kHz, não é
//      chiado branco de espectro cheio) com estalos raros de propagação;
//   7. Ruído de squelch ao abrir (click curto) e ao fechar (rajada decaindo
//      ~146 ms + bipe de 1750 Hz — o "pshhh-bip" de fim de transmissão);
//   8. Flutuação lenta de sinal (±6%, troca a cada 2–8 s): desvanecimento
//      de rede sem fio.
//
// Todos os coeficientes abaixo estão pré-calculados para 48 kHz. Derivações:
//   passa-altas 1ª ordem  y[n] = a*(y[n-1] + x[n] - x[n-1]), a = 1/(1+2*pi*fc/fs)
//     fc=420 Hz  -> a = 0.94787
//   passa-baixas 1ª ordem  y[n] += g*(x[n] - y[n]), g = 1-exp(-2*pi*fc/fs)
//     fc=3100 Hz -> g = 0.33368 ; fc=1700 Hz -> g = 0.20
//   ressonância (biquad peaking RBJ, fc=1900 Hz, Q=2,2, +5 dB, normalizado):
//     y[n] = 1.031323*x[n] - 1.860469*x[n-1] + 0.888114*x[n-2]
//          + 1.860469*y[n-1] - 0.919482*y[n-2]
//   decaimentos exponenciais por amostra (a = exp(-1/(fs*tau))):
//     tau=160 ms -> 0.999861 ; tau=120 ms -> 0.999826 ; tau=2,5 s -> 0.999992
//     tau=45 ms  -> 0.999537
// ---------------------------------------------------------------------------

#include <cmath>
#include <cstddef>
#include <cstdint>

class RadioVoiceDsp final {
public:
    // Ajustes em unidades normalizadas: intensity/noise em [0, 1] e gain em
    // [0.5, 1.5]. Podem ser reconfigurados a qualquer momento (por quadro),
    // os filtros e o squelch preservam o estado entre chamadas.
    void configure(float intensity, float noise, float gain) {
        m_intensity = clampRange(intensity, 0.0f, 1.0f);
        m_noise = clampRange(noise, 0.0f, 1.0f);
        m_gain = clampRange(gain, 0.5f, 1.5f);
    }

    bool seeded() const { return m_rng != 0u; }

    // Semente determinística por fluxo (usuário/conexão/canal). Só tem
    // efeito na primeira chamada: reconfigurações posteriores não
    // ressemeiam o ruído nem reiniciam os filtros.
    void seed(uint32_t s) {
        if (m_rng != 0u) return;
        uint32_t v = s ? s : 1u;
        // Descarta as primeiras saídas do LCG para decorrelacionar sementes
        // próximas (por exemplo, ids de usuário consecutivos).
        for (int i = 0; i < 4; ++i) v = v * 1664525u + 1013904223u;
        m_rng = v ? v : 1u;
    }

    // Processa `frames` amostras a cada `stride` posições a partir de
    // `samples`. stride = número de canais do buffer intercalado
    // (passe samples + canal e stride = canais); 1 para mono.
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
        else m_env *= 0.999861f;                    // liberação ~160 ms
        float wanted = 0.36f / (m_env > 0.004f ? m_env : 0.004f);
        wanted = clampRange(wanted, 0.25f, 12.0f);
        m_agcGain += 0.003f * (wanted - m_agcGain); // suavização ~7 ms
        const float leveled = input * m_agcGain;

        // --- 2) Banda estreita ~420 Hz a ~3,1 kHz --------------------------
        m_hp1 = 0.94787f * (m_hp1 + leveled - m_hp1x); m_hp1x = leveled;
        m_hp2 = 0.94787f * (m_hp2 + m_hp1 - m_hp2x);   m_hp2x = m_hp1;
        m_lp1 += 0.33368f * (m_hp2 - m_lp1);
        m_lp2 += 0.33368f * (m_lp1 - m_lp2);
        const float band = m_lp2;

        // --- 3) Saturação de transmissor + quantização grosseira -----------
        float crushed = std::tanh(band * 3.4f) * 0.60f;
        crushed = std::round(crushed * 48.0f) / 48.0f;

        // --- 4) Ressonância do alto-falante pequeno ------------------------
        const float speaker = 1.031323f * crushed - 1.860469f * m_resX1
            + 0.888114f * m_resX2 + 1.860469f * m_resY1 - 0.919482f * m_resY2;
        m_resX2 = m_resX1; m_resX1 = crushed;
        m_resY2 = m_resY1; m_resY1 = speaker;

        // --- 5) Squelch: VAD com limiar adaptativo e histerese -------------
        const float absBand = std::fabs(band);
        if (absBand > m_vEnv) m_vEnv += 0.25f * (absBand - m_vEnv);
        else m_vEnv *= 0.999826f;                   // liberação ~120 ms
        m_vSlow = absBand > m_vSlow ? absBand : m_vSlow * 0.999992f; // ~2,5 s
        const float threshold = m_vSlow * 0.10f > 0.018f ? m_vSlow * 0.10f : 0.018f;
        if (!m_open) {
            if (m_vEnv > threshold * 1.6f) {
                m_open = true;
                m_hold = 0;
                m_click = 128;                      // click de abertura
                m_clickAmp = 0.30f;
            }
        } else {
            m_hold = m_vEnv > threshold ? 0u : m_hold + 1u;
            if (m_hold > 24000u) {                  // 500 ms abaixo do limiar
                m_open = false;
                m_tail = 7000;                      // rajada ~146 ms
                m_tailAmp = 0.20f + 0.30f * m_noise;
                m_beepPhase = 0.0f;
            }
        }

        // --- 6) Estática limitada à banda + estalos raros ------------------
        const float white = nextNoise();
        m_nLp += 0.20f * (white - m_nLp);           // passa-baixas ~1,7 kHz
        m_rng = m_rng * 1664525u + 1013904223u;
        if ((m_rng >> 12) < 12u) m_crackle = white * 0.9f; // ~0,5 estalo/s
        m_crackle *= 0.86f;
        const float noiseAmp = m_open
            ? 0.040f + 0.150f * m_noise
            : 0.010f + 0.045f * m_noise;
        const float staticNoise = m_nLp * noiseAmp + m_crackle * 0.050f;

        // --- 7) Click de abertura e rajada/bipe de fechamento --------------
        float burst = 0.0f;
        if (m_click > 0) {
            burst += white * m_clickAmp;
            m_clickAmp *= 0.88f;
            --m_click;
        }
        if (m_tail > 0) {
            --m_tail;
            burst += m_nLp * m_tailAmp;             // rajada "pshhh"
            m_tailAmp *= 0.999537f;                 // decaimento ~45 ms
            if (m_tail > 3500) {                   // bipe nos primeiros ~73 ms
                m_beepPhase += 0.229336f;           // 1750 Hz @ 48 kHz
                burst += std::sin(m_beepPhase) * 0.09f
                    * float(m_tail - 3500) / 3500.0f;
            }
        }

        // --- 8) Flutuação lenta de sinal ------------------------------------
        if (m_wobbleLeft > 0) {
            --m_wobbleLeft;
        } else {
            m_rng = m_rng * 1664525u + 1013904223u;
            m_wobbleLeft = 96000u + (m_rng % 288000u);      // 2–8 s
            m_rng = m_rng * 1664525u + 1013904223u;
            m_wobbleTarget = 0.94f + 0.12f * float(m_rng % 1000u) / 999.0f;
        }
        m_wobble += (m_wobbleTarget - m_wobble) * 0.0002f;

        // --- Saída: gate do squelch + mix dry/wet + saturação final ---------
        const float voice = m_open ? speaker : speaker * 0.05f;
        float wet = (voice + staticNoise + burst) * m_wobble;
        wet = std::tanh(wet * 1.1f) * 0.90f;        // headroom garantido
        return (input * (1.0f - m_intensity) + wet * m_intensity) * m_gain;
    }

    // Ajustes correntes (configure()).
    float m_intensity = 0.90f;
    float m_noise = 0.10f;
    float m_gain = 1.05f;

    // Estado do DSP (um por fluxo de áudio).
    uint32_t m_rng = 0;            // semente do ruído determinístico
    float m_env = 0.0f;            // seguidor de pico do AGC
    float m_agcGain = 1.0f;        // ganho suavizado do AGC
    float m_hp1 = 0.0f, m_hp1x = 0.0f, m_hp2 = 0.0f, m_hp2x = 0.0f;
    float m_lp1 = 0.0f, m_lp2 = 0.0f;
    float m_resX1 = 0.0f, m_resX2 = 0.0f, m_resY1 = 0.0f, m_resY2 = 0.0f;
    float m_vEnv = 0.0f;           // envelope rápido da voz pós-banda
    float m_vSlow = 0.0f;          // rastreador lento (limiar adaptativo)
    uint32_t m_hold = 0;           // amostras abaixo do limiar com canal aberto
    bool m_open = false;           // squelch aberto (alguém falando)
    float m_nLp = 0.0f;            // estática filtrada
    float m_crackle = 0.0f;        // estalo de propagação decaindo
    int m_tail = 0;                // amostras restantes da rajada de squelch
    float m_tailAmp = 0.0f;
    float m_beepPhase = 0.0f;      // fase do bipe de 1750 Hz
    int m_click = 0;               // amostras restantes do click de abertura
    float m_clickAmp = 0.0f;
    uint32_t m_wobbleLeft = 0;     // amostras até a próxima flutuação
    float m_wobble = 1.0f;         // flutuação corrente do sinal
    float m_wobbleTarget = 1.0f;
};
