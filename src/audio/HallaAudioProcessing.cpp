#include "audio/HallaAudioProcessing.h"

#include <QtGlobal>
#include <cmath>
#include <cstring>

// Sem dependência de AppLog/Qt widgets aqui de propósito: o módulo é
// compilado também pelo gate standalone de CI (tests/webrtc_apm_smoke.cpp
// + este arquivo num único cl), que não tem moc nem o resto do app. Logging
// de estado fica no VoiceEngine (lado do app).

#ifdef HALLA_WEBRTC_NATIVE
#include <cstddef>
using nullptr_t = std::nullptr_t;
#include "api/audio/audio_processing.h"
#include "api/audio/builtin_audio_processing_builder.h"
#include "api/environment/environment_factory.h"
#else
// ---- DSP embutido (builds sem o SDK do libwebrtc) ----
// RNNoise: supressão de ruído por rede neural (GRU), Xiph.Org, BSD-like.
// AEC MDF: cancelador de eco acústico do speexdsp (Valin/Xiph), BSD-like,
//          o mesmo algoritmo que softphones livres usam em produção há
//          duas décadas. Compilado junto do binário via speex_aec/speex_aec.c.
extern "C" {
#include "rnnoise/rnnoise.h"
#include "speex_aec/speex_aec.h"
}
#endif

// A implementação concreta fica escondida no Impl para não vazar headers do
// libwebrtc (C++20 + abseil) para o resto do app — o resto do Halla segue
// compilando igual no build fallback sem SDK.
struct HallaAudioProcessing::Impl {
#ifdef HALLA_WEBRTC_NATIVE
    webrtc::scoped_refptr<webrtc::AudioProcessing> apm;
    webrtc::StreamConfig streamConfig{48000, 1};
    bool denoiseEnabled = false;
    bool echoEnabled = false;
#else
    // ---- fallback embutido ----
    // NS: estado do RNNoise (um por engine; ~44 KB).
    DenoiseState* ns = nullptr;
    // AEC: cancelador do speex (um por engine; ~centenas de KB com a cauda
    // de 250 ms — criado apenas quando o eco está ligado).
    void* aec = nullptr;
    bool denoise = false;
    bool echo = false;
    // Mistura sinal original/processado conforme o nível do slider
    // (0-100): 1.0 = RNNoise integral; ~0.55 = supressão suave.
    float denoiseAlpha = 0.775f;
    // Passa-alta de 2a ordem (rumble/DC) — mesmo papel do high_pass_filter
    // do APM: tira energia sub-grave que só gasta bitrate e abre o VAD.
    float hpX1 = 0, hpX2 = 0, hpY1 = 0, hpY2 = 0;
    // O AEC só processa com referência far-end recente: sem playout não
    // existe eco, e chamar capture sem playback vira spam de warning + o
    // filtro divergendo no speex. Contador de frames sem reverse (10 ms).
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
        // BuiltinAudioProcessingBuilder monta a cadeia completa: AEC3 com
        // estimador neural de eco residual + supressão de ruído por modelo
        // neural + filtro passa-alta. É a mesma classe de processamento que
        // apps de voz comerciais usam — e o "cancelamento de eco" do Halla
        // deixou de ser um checkbox sem efeito.
        m_impl->apm = webrtc::BuiltinAudioProcessingBuilder().Build(
            webrtc::CreateEnvironment());
        if (!m_impl->apm) return; // caller detecta via echoActive()/denoiseActive()
    }

    webrtc::AudioProcessing::Config config;
    // Passa-alta em banda completa: corta rumble/DC que abre o VAD a toa e
    // gasta bitrate do Opus sem contribuir para a fala.
    config.high_pass_filter.enabled = denoiseEnabled || echoCancellation;
    config.high_pass_filter.apply_in_full_band = true;

    // AEC3: usa a referencia far-end (processReverseFrame) para subtrair do
    // microfone o que esta tocando no alto-falante. O estimador de atraso
    // interno tolera o alinhamento aproximado do sink do Qt.
    config.echo_canceller.enabled = echoCancellation;

    // Supressao de ruído neural (wiener + probabilidade de fala): remove
    // ventilador, teclado, rua — sem o "efeito submarino" dos gates antigos.
    config.noise_suppression.enabled = denoiseEnabled;
    config.noise_suppression.level = nsLevelFor(denoiseLevel0to100);

    m_impl->apm->ApplyConfig(config);
    m_impl->denoiseEnabled = denoiseEnabled;
    m_impl->echoEnabled = echoCancellation;
#else
    // ---- fallback embutido: RNNoise + AEC MDF do speex ----
    // Criar/destruir estado conforme as opções; ApplyConfig equivalente.
    if (denoiseEnabled && !m_impl->ns) {
        m_impl->ns = rnnoise_create(nullptr); // modelo padrão embutido
    } else if (!denoiseEnabled && m_impl->ns) {
        rnnoise_destroy(m_impl->ns);
        m_impl->ns = nullptr;
    }
    if (echoCancellation && !m_impl->aec) {
        // Quadros de 480 amostras (10 ms @ 48 kHz) e cauda de 250 ms: cobre
        // o buffer do QAudioSink (~160 ms) + o caminho acústico da sala.
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
        // Dica de atraso far-end -> eco: atualizada pelo VoiceEngine com o
        // nivel real do buffer do QAudioSink (+ latencia do dispositivo).
        m_impl->apm->set_stream_delay_ms(m_streamDelayMs);
    }
    m_impl->apm->ProcessStream(samples480, m_impl->streamConfig,
                               m_impl->streamConfig, samples480);
}

void HallaAudioProcessing::processReverseFrame(const int16_t* samples480) {
    if (!m_impl->apm || !samples480) return;
    // src e dest podem compartilhar memoria (contrato do APM int16).
    m_impl->apm->ProcessReverseStream(samples480, m_impl->streamConfig,
                                      m_impl->streamConfig,
                                      const_cast<int16_t*>(samples480));
}
#else
// Filtro passa-alta Butterworth de 2a ordem @ 80 Hz (fs 48 kHz, Q=0.707):
// corta rumble/DC sem tocar na voz masculina grave (300 Hz passa a 99.75%).
static void highPassFrame(float* x, int n, float& x1, float& x2, float& y1, float& y2) {
    // Coeficientes precisos p/ fc=80 Hz (verificados: H(10Hz)=0.016, H(80)=0.707,
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
    if (!d.denoise && !d.echo) return; // nada pedido: passthrough barato

    // ---- 1. AEC (speex MDF, dominio int16) ----
    // Só com referência far-end fresca (sem playout não existe eco — e o
    // speex enche o log de warning se capturarmos sem playback).
    ++d.reverseStarved;
    if (d.echo) {
        if (d.reverseStarved == 1) {
            // Retomada depois de uma pausa longa: caminho acústico pode ter
            // mudado (usuário pôs/tirou fone) — filtro do zero.
            if (m_restartArmed) halla_speex_aec_reset(d.aec);
            m_restartArmed = false;
        }
        if (d.reverseStarved <= 50) { // referência nos últimos ~500 ms
            halla_speex_aec_capture(d.aec, samples480, samples480);
        } else if (!m_restartArmed) {
            m_restartArmed = true;   // sem reverse: arma o reset da retomada
        }
    }

    if (!d.denoise) return; // só eco: pronto

    // ---- 2. NS (RNNoise, dominio float) ----
    // O detector de silêncio interno do modelo calibra com energia
    // espectral típica de fala a ~-10 dBFS; microfones cotidianos chegam
    // bem abaixo. Pré-ganho FIXO de +8 dB (x2.5) — sem dinâmica, portanto
    // sem modulação de ganho entre quadros — leva mics comuns ao regime do
    // modelo, e a divisão final devolve a escala original: os ganhos por
    // banda do modelo são relativos, então o resultado é o mesmo sinal com
    // o ruído atenuado.
    constexpr float kPreGain = 2.5f;
    float x[480];
    for (int i = 0; i < 480; ++i)
        x[i] = (float(samples480[i]) / 32768.0f) * kPreGain;

    // ---- 3. passa-alta (junto do NS, como no APM) ----
    highPassFrame(x, 480, d.hpX1, d.hpX2, d.hpY1, d.hpY2);

    float y[480];
    rnnoise_process_frame(d.ns, y, x);

    // Nível do slider: mistura original/processado. Os ganhos do RNNoise
    // preservam a fase, então a mistura linear não gera cancelamento.
    const float alpha = d.denoiseAlpha;
    const float invGain = 1.0f / kPreGain;
    for (int i = 0; i < 480; ++i) {
        const float mixed = alpha * y[i] + (1.0f - alpha) * x[i];
        samples480[i] = int16_t(qBound(-32768.0f, mixed * 32768.0f * invGain, 32767.0f));
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
