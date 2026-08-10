#include "HallaWebRtcSession.h"
#include "net/NetSession.h"
#include "core/AppLog.h"

#ifdef HALLA_WEBRTC_NATIVE
#include <cstddef>
// Some Chromium/WebRTC headers refer to nullptr_t unqualified when compiled
// outside their GN toolchain. Provide the same global alias for C++ compilers
// that only expose std::nullptr_t.
using nullptr_t = std::nullptr_t;
#include "api/create_peerconnection_factory.h"
#include "api/audio_codecs/builtin_audio_decoder_factory.h"
#include "api/audio_codecs/builtin_audio_encoder_factory.h"
#include "api/video_codecs/builtin_video_decoder_factory.h"
#include "api/video_codecs/builtin_video_encoder_factory.h"
#include "rtc_base/ssl_adapter.h"
#include "rtc_base/thread.h"
#endif

struct HallaWebRtcSession::NativeState {
#ifdef HALLA_WEBRTC_NATIVE
    std::unique_ptr<webrtc::Thread> networkThread;
    std::unique_ptr<webrtc::Thread> workerThread;
    std::unique_ptr<webrtc::Thread> signalingThread;
    webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> factory;
    bool sslInitialized = false;
#endif
};

HallaWebRtcSession::HallaWebRtcSession(NetSession* net, QObject* parent)
    : QObject(parent), m_native(std::make_unique<NativeState>()), m_net(net) {}

HallaWebRtcSession::~HallaWebRtcSession() {
    stopBroadcast();
#ifdef HALLA_WEBRTC_NATIVE
    if (m_native) {
        m_native->factory = nullptr;
        m_native->signalingThread.reset();
        m_native->workerThread.reset();
        m_native->networkThread.reset();
        if (m_native->sslInitialized) {
            webrtc::CleanupSSL();
            m_native->sslInitialized = false;
        }
    }
#endif
}

bool HallaWebRtcSession::isNativeAvailable() const {
#ifdef HALLA_WEBRTC_NATIVE
    return true;
#else
    return false;
#endif
}

void HallaWebRtcSession::startBroadcast() {
#ifdef HALLA_WEBRTC_NATIVE
    if (!m_native->sslInitialized) {
        if (!webrtc::InitializeSSL()) {
            const QString reason = tr("Falha ao inicializar SSL do WebRTC nativo");
            AppLog::error(reason);
            emit unavailable(reason);
            return;
        }
        m_native->sslInitialized = true;
    }
    if (!m_native->factory) {
        m_native->networkThread = webrtc::Thread::CreateWithSocketServer();
        m_native->workerThread = webrtc::Thread::Create();
        m_native->signalingThread = webrtc::Thread::Create();
        if (!m_native->networkThread || !m_native->workerThread || !m_native->signalingThread
                || !m_native->networkThread->Start() || !m_native->workerThread->Start()
                || !m_native->signalingThread->Start()) {
            const QString reason = tr("Falha ao iniciar threads do WebRTC nativo");
            AppLog::error(reason);
            emit unavailable(reason);
            return;
        }
        m_native->factory = webrtc::CreatePeerConnectionFactory(
            m_native->networkThread.get(), m_native->workerThread.get(), m_native->signalingThread.get(),
            nullptr,
            webrtc::CreateBuiltinAudioEncoderFactory(),
            webrtc::CreateBuiltinAudioDecoderFactory(),
            webrtc::CreateBuiltinVideoEncoderFactory(),
            webrtc::CreateBuiltinVideoDecoderFactory(),
            nullptr, nullptr);
        if (!m_native->factory) {
            const QString reason = tr("Falha ao criar PeerConnectionFactory WebRTC nativa");
            AppLog::error(reason);
            emit unavailable(reason);
            return;
        }
        AppLog::info(tr("WebRTC nativo inicializado (PeerConnectionFactory pronta)"));
    }
    m_broadcasting = true;
    if (m_net) m_net->sendWebRtcStreamStart();
    emit broadcastStarted();
#else
    const QString reason = tr("WebRTC nativo ainda não está disponível neste build. "
                              "O cliente continuará usando a transmissão clássica por enquanto.");
    AppLog::warn(reason);
    emit unavailable(reason);
#endif
}

void HallaWebRtcSession::stopBroadcast() {
    if (!m_broadcasting) return;
    m_broadcasting = false;
    if (m_net) m_net->sendWebRtcStreamStop();
    emit broadcastStopped();
}

void HallaWebRtcSession::handleSignal(const QJsonObject& signal) {
#ifdef HALLA_WEBRTC_NATIVE
    Q_UNUSED(signal);
    // TODO: rotear answer/ICE/watch_request para PeerConnection nativo.
#else
    const QString type = signal.value(QStringLiteral("t")).toString();
    AppLog::info(QStringLiteral("WebRTC signaling recebido em build sem libwebrtc: %1").arg(type));
#endif
}
