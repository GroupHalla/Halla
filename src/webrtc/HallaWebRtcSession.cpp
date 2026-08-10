#include "HallaWebRtcSession.h"
#include "net/NetSession.h"
#include "core/AppLog.h"

#ifdef HALLA_WEBRTC_NATIVE
#include <cstddef>
#include <map>
#include <memory>
#include <string>
#include <vector>
using nullptr_t = std::nullptr_t;
#include "api/create_peerconnection_factory.h"
#include "api/audio_codecs/builtin_audio_decoder_factory.h"
#include "api/audio_codecs/builtin_audio_encoder_factory.h"
#include "api/jsep.h"
#include "api/make_ref_counted.h"
#include "rtc_base/ref_counted_object.h"
#include "rtc_base/ssl_adapter.h"
#include "rtc_base/thread.h"
#endif

#ifdef HALLA_WEBRTC_NATIVE
namespace {
class NoopSetObserver : public webrtc::SetSessionDescriptionObserver {
public:
    void OnSuccess() override {}
    void OnFailure(webrtc::RTCError error) override {
        AppLog::warn(QStringLiteral("WebRTC SetDescription falhou: %1")
                         .arg(QString::fromStdString(error.message())));
    }
};

class PeerObserver : public webrtc::PeerConnectionObserver {
public:
    PeerObserver(HallaWebRtcSession* owner, int peerId) : m_owner(owner), m_peerId(peerId) {}
    void OnSignalingChange(webrtc::PeerConnectionInterface::SignalingState) override {}
    void OnDataChannel(webrtc::scoped_refptr<webrtc::DataChannelInterface>) override {}
    void OnIceGatheringChange(webrtc::PeerConnectionInterface::IceGatheringState) override {}
    void OnIceCandidate(const webrtc::IceCandidate* candidate) override;
private:
    HallaWebRtcSession* m_owner = nullptr;
    int m_peerId = 0;
};

class OfferObserver : public webrtc::CreateSessionDescriptionObserver {
public:
    OfferObserver(HallaWebRtcSession* owner, int peerId) : m_owner(owner), m_peerId(peerId) {}
    void OnSuccess(webrtc::SessionDescriptionInterface* desc) override;
    void OnFailure(webrtc::RTCError error) override {
        AppLog::warn(QStringLiteral("WebRTC CreateOffer falhou: %1")
                         .arg(QString::fromStdString(error.message())));
    }
private:
    HallaWebRtcSession* m_owner = nullptr;
    int m_peerId = 0;
};
}
#endif

#ifdef HALLA_WEBRTC_NATIVE
struct HallaWebRtcSession::PeerContext {
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc;
    std::unique_ptr<PeerObserver> observer;
    std::vector<webrtc::scoped_refptr<webrtc::CreateSessionDescriptionObserver>> pendingOffers;
};
#endif

struct HallaWebRtcSession::NativeState {
#ifdef HALLA_WEBRTC_NATIVE
    std::unique_ptr<webrtc::Thread> networkThread;
    std::unique_ptr<webrtc::Thread> workerThread;
    std::unique_ptr<webrtc::Thread> signalingThread;
    webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> factory;
    std::map<int, std::unique_ptr<PeerContext>> peers;
    bool sslInitialized = false;
#endif
};

HallaWebRtcSession::HallaWebRtcSession(NetSession* net, QObject* parent)
    : QObject(parent), m_native(std::make_unique<NativeState>()), m_net(net) {}

HallaWebRtcSession::~HallaWebRtcSession() {
    stopBroadcast();
#ifdef HALLA_WEBRTC_NATIVE
    if (m_native) {
        m_native->peers.clear();
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

#ifdef HALLA_WEBRTC_NATIVE
bool HallaWebRtcSession::ensureNativeFactory() {
    if (!m_native->sslInitialized) {
        if (!webrtc::InitializeSSL()) return false;
        m_native->sslInitialized = true;
    }
    if (m_native->factory) return true;
    m_native->networkThread = webrtc::Thread::CreateWithSocketServer();
    m_native->workerThread = webrtc::Thread::Create();
    m_native->signalingThread = webrtc::Thread::Create();
    if (!m_native->networkThread || !m_native->workerThread || !m_native->signalingThread)
        return false;
    if (!m_native->networkThread->Start() || !m_native->workerThread->Start() || !m_native->signalingThread->Start())
        return false;
    m_native->factory = webrtc::CreatePeerConnectionFactory(
        m_native->networkThread.get(), m_native->workerThread.get(), m_native->signalingThread.get(),
        nullptr,
        webrtc::CreateBuiltinAudioEncoderFactory(),
        webrtc::CreateBuiltinAudioDecoderFactory(),
        nullptr, nullptr,
        nullptr, nullptr);
    if (m_native->factory) AppLog::info(tr("WebRTC nativo inicializado (factory pronta)"));
    return m_native->factory != nullptr;
}

HallaWebRtcSession::PeerContext* HallaWebRtcSession::ensurePeer(int peerId) {
    if (peerId <= 0 || !ensureNativeFactory()) return nullptr;
    auto it = m_native->peers.find(peerId);
    if (it != m_native->peers.end()) return it->second.get();

    auto ctx = std::make_unique<PeerContext>();
    ctx->observer = std::make_unique<PeerObserver>(this, peerId);
    webrtc::PeerConnectionInterface::RTCConfiguration cfg;
    webrtc::PeerConnectionInterface::IceServer stun;
    stun.urls.push_back("stun:stun.l.google.com:19302");
    cfg.servers.push_back(stun);
    webrtc::PeerConnectionDependencies deps(ctx->observer.get());
    auto result = m_native->factory->CreatePeerConnectionOrError(cfg, std::move(deps));
    if (!result.ok()) {
        AppLog::warn(QStringLiteral("WebRTC PeerConnection falhou: %1")
                         .arg(QString::fromStdString(result.error().message())));
        return nullptr;
    }
    ctx->pc = result.MoveValue();
    PeerContext* raw = ctx.get();
    m_native->peers[peerId] = std::move(ctx);
    return raw;
}

void HallaWebRtcSession::sendNativeIce(int peerId, const std::string& candidate,
                                       const std::string& mid, int mline) {
    if (m_net) m_net->sendWebRtcIce(peerId, QString::fromStdString(candidate),
                                    QString::fromStdString(mid), mline);
}

void HallaWebRtcSession::sendNativeOffer(int peerId, const std::string& sdp) {
    if (m_net) m_net->sendWebRtcOffer(peerId, QString::fromStdString(sdp));
}

void HallaWebRtcSession::createOfferForPeer(int peerId) {
    PeerContext* ctx = ensurePeer(peerId);
    if (!ctx || !ctx->pc) return;
    auto obs = webrtc::make_ref_counted<OfferObserver>(this, peerId);
    ctx->pendingOffers.push_back(obs);
    webrtc::PeerConnectionInterface::RTCOfferAnswerOptions opts;
    ctx->pc->CreateOffer(obs.get(), opts);
}

void HallaWebRtcSession::setRemoteAnswer(int peerId, const QString& sdp) {
    PeerContext* ctx = ensurePeer(peerId);
    if (!ctx || !ctx->pc || sdp.isEmpty()) return;
    auto desc = webrtc::CreateSessionDescription(webrtc::SdpType::kAnswer, sdp.toStdString());
    if (!desc) return;
    ctx->pc->SetRemoteDescription(new webrtc::RefCountedObject<NoopSetObserver>(), desc.release());
}

void HallaWebRtcSession::addRemoteIce(int peerId, const QJsonObject& signal) {
    PeerContext* ctx = ensurePeer(peerId);
    if (!ctx || !ctx->pc) return;
    const QString candidate = signal.value(QStringLiteral("candidate")).toString();
    if (candidate.isEmpty()) return;
    std::unique_ptr<webrtc::IceCandidate> ice(webrtc::CreateIceCandidate(
        signal.value(QStringLiteral("sdpMid")).toString(QStringLiteral("0")).toStdString(),
        signal.value(QStringLiteral("sdpMLineIndex")).toInt(0),
        candidate.toStdString(), nullptr));
    if (ice) ctx->pc->AddIceCandidate(std::move(ice), [](webrtc::RTCError) {});
}

void HallaWebRtcSession::closePeer(int peerId) {
    auto it = m_native->peers.find(peerId);
    if (it != m_native->peers.end()) {
        if (it->second->pc) it->second->pc->Close();
        m_native->peers.erase(it);
    }
}
#endif

void HallaWebRtcSession::startBroadcast() {
#ifdef HALLA_WEBRTC_NATIVE
    if (!ensureNativeFactory()) {
        const QString reason = tr("Falha ao inicializar WebRTC nativo");
        AppLog::error(reason);
        emit unavailable(reason);
        return;
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
#ifdef HALLA_WEBRTC_NATIVE
    for (auto& kv : m_native->peers) if (kv.second->pc) kv.second->pc->Close();
    m_native->peers.clear();
#endif
    if (m_net) m_net->sendWebRtcStreamStop();
    emit broadcastStopped();
}

void HallaWebRtcSession::handleSignal(const QJsonObject& signal) {
#ifdef HALLA_WEBRTC_NATIVE
    const QString type = signal.value(QStringLiteral("t")).toString();
    const int from = signal.value(QStringLiteral("from")).toInt();
    if (type == QLatin1String("webrtc_watch_request")) createOfferForPeer(from);
    else if (type == QLatin1String("webrtc_watch_stop")) closePeer(from);
    else if (type == QLatin1String("webrtc_answer")) setRemoteAnswer(from, signal.value(QStringLiteral("sdp")).toString());
    else if (type == QLatin1String("webrtc_ice")) addRemoteIce(from, signal);
#else
    const QString type = signal.value(QStringLiteral("t")).toString();
    AppLog::info(QStringLiteral("WebRTC signaling recebido em build sem libwebrtc: %1").arg(type));
#endif
}

#ifdef HALLA_WEBRTC_NATIVE
void PeerObserver::OnIceCandidate(const webrtc::IceCandidate* candidate) {
    if (!candidate || !m_owner) return;
    m_owner->sendNativeIce(m_peerId, candidate->ToString(), candidate->sdp_mid(), candidate->sdp_mline_index());
}

void OfferObserver::OnSuccess(webrtc::SessionDescriptionInterface* desc) {
    if (!desc || !m_owner) return;
    std::string sdp = desc->ToString();
    if (auto* ctx = m_owner->ensurePeer(m_peerId)) {
        ctx->pc->SetLocalDescription(new webrtc::RefCountedObject<NoopSetObserver>(), desc);
    } else {
        delete desc;
    }
    m_owner->sendNativeOffer(m_peerId, sdp);
}
#endif
