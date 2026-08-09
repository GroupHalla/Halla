#pragma once

#include <QDialog>
#include <QJsonObject>
#include <QPointer>

class QWebEngineView;
class QWebChannel;
class NetSession;

// Transmissor WebRTC inicial. A mídia é produzida pelo Chromium embutido
// (getDisplayMedia + RTCPeerConnection); o signaling passa pelo NetSession.
class WebRtcScreenShareBridge : public QDialog {
    Q_OBJECT
public:
    explicit WebRtcScreenShareBridge(NetSession* net, QWidget* parent = nullptr);
    ~WebRtcScreenShareBridge() override;

    void handleSignal(const QJsonObject& signal);
    void stopAll();

signals:
    void closedByUser();

public slots:
    Q_INVOKABLE void sendOffer(int toUserId, const QString& sdp);
    Q_INVOKABLE void sendIce(int toUserId, const QString& candidate,
                             const QString& sdpMid, int sdpMLineIndex);
    Q_INVOKABLE void captureStarted();
    Q_INVOKABLE void captureStopped();
    Q_INVOKABLE void jsLog(const QString& text);

protected:
    void closeEvent(QCloseEvent* e) override;

private:
    void initPage();
    void runJs(const QString& code);
    static QString html();

    QPointer<NetSession> m_net;
    QWebEngineView* m_view = nullptr;
    QWebChannel* m_channel = nullptr;
    bool m_closing = false;
};
