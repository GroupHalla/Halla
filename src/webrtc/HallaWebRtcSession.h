#pragma once

#include <QObject>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <QJsonObject>
#include <QByteArray>
#include <QString>
#include <QImage>
#include <QMap>
#include <QMutex>
#include <QPointer>
#include <QSet>
#include <QVector>
#include <QRect>
#include <QtGlobal>
#include <atomic>
class QTimer;
class QThread;

class NetSession;
#ifdef HALLA_WEBRTC_NATIVE
namespace webrtc {
template <typename T> class scoped_refptr;
class VideoTrackInterface;
class AudioTrackInterface;
class PeerConnectionInterface;
}
#endif

// Halla Desktop native WebRTC adapter.
//
// This layer is intentionally small: the UI and NetSession talk to this
// class, and the concrete implementation uses libwebrtc when
// HALLA_WEBRTC_NATIVE is defined. Without the prebuilt SDK it compiles as a
// safe stub to keep the client and CI working while the WebRTC toolchain is
// being prepared.
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
    // Enables/disables the local preview pipeline (GPU->CPU copy + downscale).
    // MainWindow calls this when opening/closing the "My stream" window:
    // without a window, generating preview is CPU/memory wasted in the dark.
    void setLocalPreviewEnabled(bool enabled);
    void startWatching(int userId);
    void stopWatching(int userId);
    // v1.1.28: retries a view that did not connect (ICE stuck on
    // CGNAT/firewall, lost offer): tears down the peer and requests again.
    void restartWatch(int userId);
    // The WebRTC session outlives the tabs (one per window); on reconnect,
    // the old NetSession is destroyed. Without these methods, m_net would
    // dangle (use-after-free in sendWebRtcStreamStart) and m_broadcasting
    // would stay stuck at true (the next stream would do nothing).
    void setNetSession(NetSession* net);
    void detachFromNet(NetSession* net);

public slots:
    void startBroadcast();
    void stopBroadcast();
    void handleSignal(const QJsonObject& signal);

#ifdef HALLA_WEBRTC_NATIVE
public:
    struct PeerContext;
    bool ensureNativeFactory();
    void resetNativeFactoryForEncoderSetting();
    PeerContext* ensurePeer(int peerId);
    // Ref-counted copy of a peer's PeerConnection -- safe to use outside the
    // lock even if closePeer deletes the context from the GUI thread in the
    // meantime.
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> peerConnection(int peerId);
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
    // v1.1.28: P2P connection state of a peer ("new"/"connecting"/
    // "connected"/"disconnected"/"failed"/"closed"). Before, ICE failed
    // (NAT/CGNAT without TURN) and the UI stayed forever on "Waiting for
    // stream..." -- now the viewer window shows the error and offers to
    // retry.
    void peerStateChanged(int peerId, const QString& state);

private:
    struct NativeState;
    std::unique_ptr<NativeState> m_native;
    NetSession* m_net = nullptr;
    // v1.1.28: peerId -> connection that peer negotiates over. With two or
    // more server tabs, m_net points at the LAST connected tab: without this
    // map, offer/answer/ICE for a stream on tab A were sent via tab B (wrong
    // server!) and the viewer never received video.
    QMap<int, QPointer<NetSession>> m_peerNet;
    NetSession* netForPeer(int peerId) const;
    // Capture runs on a worker thread (not the GUI) so DXGI, scaling, and
    // preview do not freeze the interface during 4K streams.
    QThread* m_captureThread = nullptr;
    QTimer* m_captureTimer = nullptr;
    int m_captureSourceType = 0;
    quintptr m_captureSourceId = 0;
    // Linux: XRandR output name of the selected monitor (X11ScreenCapturer
    // resolves the monitor by name -- the QScreen order is not reliable).
    QString m_captureScreenName;
    // Snapshot of the logical screen geometries, collected ON THE GUI THREAD
    // (setCaptureSource). QGuiApplication::screens()/geometry() must not be
    // read from the capture thread -- the window crop uses this copy.
    QVector<QRect> m_screenGeometries;
    int m_captureWidth = 1920;
    int m_captureHeight = 1080;
    int m_captureFps = 60;
    int m_captureBitrateKbps = 8000;
    bool m_captureSystemAudio = false;
    bool m_broadcasting = false;
    // Local preview is downscaled BEFORE going to the GUI: a 4K QImage
    // crossing the thread boundary + QPixmap::fromImage + scaled froze the
    // window.
    int m_previewMaxWidth = 1280;
    // Prevents two capture frames from running at the same time (the first
    // frame and the worker-thread timer), which corrupted DxgiScreenCapturer
    // and crashed the app when clicking "Share".
    std::atomic<bool> m_captureInFlight{false};
    // Remote video coalescing: if the UI is busy, replace the pending frame
    // with the newest one instead of queueing seconds of delay.
    QMutex m_remoteFrameMutex;
    QMap<int, QImage> m_pendingRemoteFrames;
    QSet<int> m_remoteFrameDispatchPosted;
    // Local preview is only generated while a window exists to display it.
    std::atomic<bool> m_previewEnabled{true};
};
