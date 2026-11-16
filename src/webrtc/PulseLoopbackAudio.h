#pragma once

// System audio (loopback) for native WebRTC on Linux -- the equivalent of the
// Windows SystemLoopbackAudioDeviceModule (WASAPI process loopback).
//
// Captures the MONITOR of the PulseAudio default sink (all PC sound) and
// ISOLATES Halla itself on a dedicated virtual sink (module-null-sink) before
// starting: Halla voices/notifications now play through that sink (returned
// to the speaker by a module-loopback), so the default sink monitor contains
// "the whole PC EXCEPT Halla" -- no echo/retransmission of channel voices.
// It is the same result as Windows AUDCLNT_PROCESS_LOOPBACK, adapted to the
// PulseAudio architecture. Also works over PipeWire (pipewire-pulse speaks
// the protocol and supports the same modules).
//
// Graceful degradation: any exclusion step failing (server without permission
// to load modules, very old Pulse...) falls back to the default sink monitor
// WITHOUT exclusion, with a log warning -- the stream keeps audio (now
// including Halla sounds) instead of being born dead.
//
// Compiles empty outside "Linux + native WebRTC" (MediaFoundationH264 pattern).

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
    // Out-of-line (defined in the .cpp): needed because of the incomplete-type
    // unique_ptr<PulseExclusion> vs make_ref_counted/RefCountedObject.
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
    struct PulseExclusion; // declared BEFORE use in the parameter below

    void captureLoop();
    void playoutLoop();
    void pushSamples(const int16_t* samples, size_t count, uint32_t sampleRate);
    // Called by the Pulse mainloop callbacks (lock held): moves one Halla
    // process sink-input to the isolated virtual sink.
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
    // Symbol-less stub for builds without Linux/native WebRTC.
};

#endif
