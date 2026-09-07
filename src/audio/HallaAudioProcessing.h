#pragma once

#include <cstdint>

// DSP de voz do microfone (v1.1.20): cancelamento de eco (AEC3) + supressão
// de ruído neural + filtro passa-alta, via AudioProcessing do libwebrtc —
// a mesma família de tecnologia de comunicação usada pelo Krisp/Discord.
//
// Contrato:
//   * Quadros de 480 amostras mono @ 48 kHz (10 ms), processados IN PLACE.
//   * processCaptureFrame() = microfone (antes do VAD/plugins/encoder).
//   * processReverseFrame() = far-end (o mix que está indo para o
//     alto-falante) — referência que o AEC3 usa para subtrair o eco.
//   * NÃO é thread-safe: todas as chamadas devem vir da mesma thread
//     (no Halla, a thread da GUI, onde vivem captureTick/playbackTick —
//     exatamente o requisito de serialização do APM do libwebrtc).
//   * Sem WebRTC nativo (builds fallback), tudo vira passthrough: as
//     opções existem na UI, mas nenhum processamento é aplicado.
class HallaAudioProcessing {
public:
    HallaAudioProcessing();
    ~HallaAudioProcessing();

    HallaAudioProcessing(const HallaAudioProcessing&) = delete;
    HallaAudioProcessing& operator=(const HallaAudioProcessing&) = delete;

    // (Re)aplica as configurações. Pode ser chamado a qualquer momento para
    // acompanhar mudanças nas Opções — ApplyConfig é barato quando nada muda.
    // denoiseLevel0to100: 0-25 baixo, 26-50 moderado, 51-75 alto, 76-100 máximo.
    void configure(bool denoiseEnabled, int denoiseLevel0to100, bool echoCancellation);

    bool denoiseActive() const;
    bool echoActive() const;

    // Microfone, 10 ms, in place. Aplica HPF + AEC3 + NS na ordem do APM.
    void processCaptureFrame(int16_t* samples480);
    // Far-end (o que toca no alto-falante), 10 ms. Alimenta o AEC3.
    void processReverseFrame(const int16_t* samples480);
    // Atraso estimado entre o far-end entrar aqui e eco voltar no microfone
    // (buffer do QAudioSink + latência do dispositivo). Melhora a convergência.
    void setPlayoutDelayMs(int ms);

private:
    struct Impl;
    Impl* m_impl = nullptr;
    int m_streamDelayMs = 100;
};
