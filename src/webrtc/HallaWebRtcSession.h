#pragma once

#include <QObject>
#include <memory>
#include <string>
#include <QJsonObject>
#include <QByteArray>
#include <QString>
#include <QImage>
#include <QtGlobal>
class QTimer;

class NetSession;
#ifdef HALLA_WEBRTC_NATIVE
namespace webrtc {
template <typename T> class scoped_refptr;
class VideoTrackInterface;
class AudioTrackInterface;
}
#endif

// Adapter de WebRTC nativo do Halla Desktop.
//
// Esta camada é intencionalmente pequena: a UI e o NetSession falam com esta
// classe, e a implementação concreta usa libwebrtc quando HALLA_WEBRTC_NATIVE
// estiver definido. Sem o SDK pré-compilado, ela compila como stub seguro para
// manter o cliente e o CI funcionando enquanto a toolchain WebRTC é preparada.
class HallaWebRtcSession : public QObject {
    Q_OBJECT
public:
    explicit HallaWebRtcSession(NetSession* net, QObject* parent = nullptr);
    ~HallaWebRtcSession() override;

    bool isNativeAvailable() const;
    bool isBroadcasting() const { return m_broadcasting; }
    void setCaptureSource(int sourceType, quintptr sourceId);
    void setCaptureQuality(int width, int height, int fps, int bitrateKbps);
    void setCaptureSystemAudio(bool enabled);
    void startWatching(int userId);
    void stopWatching(int userId);

public slots:
    void startBroadcast();
    void stopBroadcast();
    void handleSignal(const QJsonObject& signal);

#ifdef HALLA_WEBRTC_NATIVE
public:
    struct PeerContext;
    bool ensureNativeFactory();
    PeerContext* ensurePeer(int peerId);
    void createOfferForPeer(int peerId);
    void setRemoteAnswer(int peerId, const QString& sdp);
    void setRemoteOffer(int peerId, const QString& sdp);
    void createAnswerForPeer(int peerId);
    void addRemoteIce(int peerId, const QJsonObject& signal);
    void remoteDescriptionReady(int peerId);
    void closePeer(int peerId);
    void sendNativeIce(int peerId, const std::string& candidate, const std::string& mid, int mline);
    void sendNativeOffer(int peerId, const std::string& sdp);
    void sendNativeAnswer(int peerId, const std::string& sdp);
    void attachRemoteVideoTrack(int peerId, webrtc::scoped_refptr<webrtc::VideoTrackInterface> track);
    void attachRemoteAudioTrack(int peerId, webrtc::scoped_refptr<webrtc::AudioTrackInterface> track);
    void deliverRemoteFrame(int peerId, const QImage& image);
    void deliverRemoteAudio(int peerId, const QByteArray& pcm, int sampleRate,
                            int channels, int frames);
    void captureFrame();
#endif

signals:
    void unavailable(const QString& reason);
    void broadcastStarted();
    void broadcastStopped();
    void localPreviewFrame(const QImage& image);
    void remoteFrameReceived(int userId, const QImage& image);
    void remoteAudioReceived(int userId, const QByteArray& pcm,
                             int sampleRate, int channels, int frames);

private:
    struct NativeState;
    std::unique_ptr<NativeState> m_native;
    NetSession* m_net = nullptr;
    QTimer* m_captureTimer = nullptr;
    int m_captureSourceType = 0;
    quintptr m_captureSourceId = 0;
    int m_captureWidth = 1920;
    int m_captureHeight = 1080;
    int m_captureFps = 60;
    int m_captureBitrateKbps = 8000;
    bool m_captureSystemAudio = false;
    bool m_broadcasting = false;
};
