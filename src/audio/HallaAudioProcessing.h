#pragma once

#include <cstdint>

// DSP de voz do microfone do Halla.
//
// Duas implementações, mesmo contrato:
//
//   * Com SDK do libwebrtc (HALLA_WEBRTC_NATIVE): cadeia completa do APM —
//     AEC3 com estimador neural de eco residual + supressão de ruído
//     neural + filtro passa-alta. É a mesma família de tecnologia de
//     comunicação usada pelo Krisp/Discord.
//
//   * Builds sem SDK (o release oficial atual): DSP EMBUTIDO, compilado
//     junto do binário — os checkboxes de redução de ruído/eco deixam de
//     ser enfeite. Supressão de ruído por rede neural via RNNoise (Xiph,
//     BSD) + cancelador de eco acústico AUMDF do speexdsp (Xiph, BSD) +
//     passa-alta de 80 Hz. Sem dependências externas.
//
// Contrato:
//   * Quadros de 480 amostras mono @ 48 kHz (10 ms), processados IN PLACE.
//   * processCaptureFrame() = microfone (antes do VAD/plugins/encoder).
//   * processReverseFrame() = far-end (o mix que está indo para o
//     alto-falante) — referência que o AEC usa para subtrair o eco.
//   * NÃO é thread-safe: todas as chamadas devem vir da mesma thread
//     (no Halla, a thread da GUI, onde vivem captureTick/playbackTick).
//
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
    // Fallback embutido: arma um reset do AEC quando a referência far-end
    // esteve ausente por um longo período (alto-falante mutado / silêncio
    // total) e acabou de voltar — o caminho acústico pode ter mudado.
    bool m_restartArmed = false;
};
