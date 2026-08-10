#pragma once

#include <QObject>
#include <memory>
#include <QJsonObject>
#include <QString>

class NetSession;

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

public slots:
    void startBroadcast();
    void stopBroadcast();
    void handleSignal(const QJsonObject& signal);

signals:
    void unavailable(const QString& reason);
    void broadcastStarted();
    void broadcastStopped();

private:
    struct NativeState;
    std::unique_ptr<NativeState> m_native;
    NetSession* m_net = nullptr;
    bool m_broadcasting = false;
};
