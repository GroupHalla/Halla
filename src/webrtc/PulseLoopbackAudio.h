#pragma once

// Áudio do sistema (loopback) para o WebRTC nativo no Linux — o equivalente
// do SystemLoopbackAudioDeviceModule do Windows (WASAPI process loopback).
//
// Captura o MONITOR do sink default do PulseAudio (todo o som do PC) e
// ISOLA o próprio Halla num sink virtual dedicado (module-null-sink) antes
// de começar: as vozes/notificações do Halla passam a tocar por esse sink
// (devolvido ao alto-falante por um module-loopback), então o monitor do
// sink default contém "todo o PC MENOS o Halla" — sem eco/retransmissão
// das vozes do canal. É o mesmo resultado do AUDCLNT_PROCESS_LOOPBACK do
// Windows, adaptado à arquitetura do PulseAudio. Funciona também sobre
// PipeWire (pipewire-pulse fala o protocolo e suporta os mesmos módulos).
//
// Degradação graciosa: qualquer passo da exclusão falhar (servidor sem
// permissão para carregar módulos, Pulse muito antigo...) cai no monitor
// do sink default SEM exclusão, com aviso no log — o stream continua com
// áudio (agora incluindo sons do Halla) em vez de nascer morto.
//
// Compila vazio fora de "Linux + WebRTC nativo" (padrão MediaFoundationH264).

#include <QtGlobal>

#if defined(HALLA_WEBRTC_NATIVE) && defined(Q_OS_LINUX)

#include "api/audio/audio_device_defines.h"
#include "modules/audio_device/include/audio_device_default.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

class LinuxLoopbackAudioDeviceModule
    : public webrtc::webrtc_impl::AudioDeviceModuleDefault<webrtc::AudioDeviceModuleForTest> {
public:
    // Out-of-line (definido no .cpp): necessário por causa do unique_ptr do
    // PulseExclusion incomplete-type vs make_ref_counted/RefCountedObject.
    LinuxLoopbackAudioDeviceModule();
    ~LinuxLoopbackAudioDeviceModule() override;

    int32_t RegisterAudioCallback(webrtc::AudioTransport* audioCallback) override;
    int32_t ActiveAudioLayer(webrtc::AudioDeviceModule::AudioLayer* audioLayer) const override;
    int32_t RecordingIsAvailable(bool* available) override;
    bool RecordingIsInitialized() const override;
    int32_t InitRecording() override;
    int32_t PlayoutIsAvailable(bool* available) override;
    bool PlayoutIsInitialized() const override;
    int32_t InitPlayout() override;
    int32_t StartPlayout() override;
    int32_t StopPlayout() override;
    bool Playing() const override;
    int32_t StartRecording() override;
    int32_t StopRecording() override;
    bool Recording() const override;
    int RestartPlayoutInternally() override;
    int RestartRecordingInternally() override;
    int SetPlayoutSampleRate(uint32_t) override;
    int SetRecordingSampleRate(uint32_t sample_rate) override;

private:
    struct PulseExclusion; // declarado ANTES do uso no parâmetro abaixo

    void captureLoop();
    void playoutLoop();
    void pushSamples(const int16_t* samples, size_t count, uint32_t sampleRate);
    // Chamado pelos callbacks da mainloop Pulse (lock held): move um
    // sink-input do processo do Halla para o sink virtual isolado.
    static void moveSinkInput(PulseExclusion* excl, uint32_t inputIndex);

    std::unique_ptr<PulseExclusion> m_exclusion;

    std::atomic<webrtc::AudioTransport*> m_audioCallback{nullptr};
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_playoutRunning{false};
    std::mutex m_threadMutex;
    std::thread m_thread;
    std::thread m_playoutThread;
    std::vector<int16_t> m_pcm;
    uint32_t m_forcedSampleRate = 48000;
};

#else

class LinuxLoopbackAudioDeviceModule {
public:
    // Stub sem símbolos para builds sem Linux/WebRTC nativo.
};

#endif
