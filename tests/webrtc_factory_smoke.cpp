#include <cstddef>
using nullptr_t = std::nullptr_t;
#include "api/create_peerconnection_factory.h"
#include "api/audio_codecs/builtin_audio_decoder_factory.h"
#include "api/audio_codecs/builtin_audio_encoder_factory.h"
#include "rtc_base/ssl_adapter.h"
#include "rtc_base/thread.h"
int main() {
    if (!webrtc::InitializeSSL()) return 10;
    auto network = webrtc::Thread::CreateWithSocketServer();
    auto worker = webrtc::Thread::Create();
    auto signaling = webrtc::Thread::Create();
    if (!network || !worker || !signaling) return 11;
    if (!network->Start() || !worker->Start() || !signaling->Start()) return 12;
    auto factory = webrtc::CreatePeerConnectionFactory(
        network.get(), worker.get(), signaling.get(), nullptr,
        webrtc::CreateBuiltinAudioEncoderFactory(),
        webrtc::CreateBuiltinAudioDecoderFactory(), nullptr, nullptr, nullptr, nullptr);
    if (!factory) return 13;
    factory = nullptr;
    signaling.reset(); worker.reset(); network.reset();
    webrtc::CleanupSSL();
    return 0;
}
