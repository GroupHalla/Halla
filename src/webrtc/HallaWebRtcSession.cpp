#include "HallaWebRtcSession.h"
#include "net/NetSession.h"
#include "core/AppLog.h"

HallaWebRtcSession::HallaWebRtcSession(NetSession* net, QObject* parent)
    : QObject(parent), m_net(net) {}

HallaWebRtcSession::~HallaWebRtcSession() {
    stopBroadcast();
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
    // TODO: inicializar PeerConnectionFactory, capturer nativo e video track.
    // A API pública e o signaling já estão prontos; esta seção será ligada ao
    // SDK libwebrtc pré-compilado na próxima etapa.
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
