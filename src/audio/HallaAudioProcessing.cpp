#include "audio/HallaAudioProcessing.h"

#include <QtGlobal>

#include "core/AppLog.h"

#ifdef HALLA_WEBRTC_NATIVE
#include <cstddef>
using nullptr_t = std::nullptr_t;
#include "api/audio/audio_processing.h"
#include "api/audio/builtin_audio_processing_builder.h"
#include "api/environment/environment_factory.h"
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
    bool announced = false;
#endif
};

HallaAudioProcessing::HallaAudioProcessing()
    : m_impl(new Impl) {}

HallaAudioProcessing::~HallaAudioProcessing() {
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
        if (!m_impl->apm) {
            AppLog::warn(QStringLiteral(
                "DSP de voz: AudioProcessing do WebRTC indisponivel neste build; "
                "reducao de ruido/eco ficara inativa"));
            return;
        }
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
    if (!m_impl->announced) {
        m_impl->announced = true;
        AppLog::info(QStringLiteral(
            "DSP de voz ativo (WebRTC APM): supressao de ruido %1, cancelamento de eco %2")
            .arg(denoiseEnabled ? QStringLiteral("ligado") : QStringLiteral("desligado"))
            .arg(echoCancellation ? QStringLiteral("ligado") : QStringLiteral("desligado")));
    }
#else
    Q_UNUSED(denoiseEnabled)
    Q_UNUSED(denoiseLevel0to100)
    Q_UNUSED(echoCancellation)
#endif
}

bool HallaAudioProcessing::denoiseActive() const {
#ifdef HALLA_WEBRTC_NATIVE
    return m_impl->apm && m_impl->denoiseEnabled;
#else
    return false;
#endif
}

bool HallaAudioProcessing::echoActive() const {
#ifdef HALLA_WEBRTC_NATIVE
    return m_impl->apm && m_impl->echoEnabled;
#else
    return false;
#endif
}

void HallaAudioProcessing::processCaptureFrame(int16_t* samples480) {
#ifdef HALLA_WEBRTC_NATIVE
    if (!m_impl->apm || !samples480) return;
    if (m_impl->echoEnabled) {
        // Dica de atraso far-end -> eco: atualizada pelo VoiceEngine com o
        // nivel real do buffer do QAudioSink (+ latencia do dispositivo).
        m_impl->apm->set_stream_delay_ms(m_streamDelayMs);
    }
    m_impl->apm->ProcessStream(samples480, m_impl->streamConfig,
                               m_impl->streamConfig, samples480);
#else
    Q_UNUSED(samples480)
#endif
}

void HallaAudioProcessing::processReverseFrame(const int16_t* samples480) {
#ifdef HALLA_WEBRTC_NATIVE
    if (!m_impl->apm || !samples480) return;
    // src e dest podem compartilhar memoria (contrato do APM int16).
    m_impl->apm->ProcessReverseStream(samples480, m_impl->streamConfig,
                                      m_impl->streamConfig,
                                      const_cast<int16_t*>(samples480));
#else
    Q_UNUSED(samples480)
#endif
}

void HallaAudioProcessing::setPlayoutDelayMs(int ms) {
    m_streamDelayMs = qBound(0, ms, 500);
}
