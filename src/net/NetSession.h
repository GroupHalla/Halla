#pragma once

#include <QObject>
#include <QTcpSocket>
#include <QUdpSocket>
#include <QTimer>
#include <QJsonObject>
#include <QElapsedTimer>
#include "core/Models.h"

// Sessão de rede do cliente Halla: TCP (controle) + UDP (voz) com o Halla Server.
// Mantém um ServerData sempre sincronizado com o que o servidor manda.
class NetSession : public QObject {
    Q_OBJECT
public:
    explicit NetSession(QObject* parent = nullptr);

    void connectToServer(const QString& host, quint16 port, const QString& nickname,
                         const QString& uid, const QString& password = QString(),
                         const QString& adminPassword = QString());

    ServerData& data() { return m_data; }
    const ServerData& data() const { return m_data; }
    QString hostPort() const { return m_hostPort; }
    int pingMs() const { return m_pingMs; }
    bool isConnected() const { return m_ready; }

    // ---- estrutura pública (aplicada em ServerData e emitindo stateChanged)
    void attachTo(ServerData* target) { m_target = target; }

    // ---- ações do cliente (enviadas ao servidor)
    void sendChat(const QString& scope, int to, const QString& text);
    void moveToChannel(int channelId, const QString& pass = QString());
    void moveOther(int userId, int channelId);
    void sendStatus();
    void sendTalking(bool on);
    void rename(const QString& newName);
    void setDescription(const QString& desc);
    void poke(int userId, const QString& msg);
    void createChannel(const QJsonObject& chan);
    void editChannel(const QJsonObject& chan);
    void deleteChannel(int id);
    void kick(int userId, bool fromServer, const QString& reason);
    void ban(int userId, const QString& reason, int minutes);
    void usePrivilegeKey(const QString& key);
    void quit();

    // ---- voz
    void sendVoiceFrame(const QByteArray& opus, quint16 seq);

signals:
    void welcomeReceived();                     // estado completo carregado
    void stateChanged();                        // ServerData mudou (rebuild da UI)
    void chatReceived(const QString& scope, int fromId, const QString& fromName,
                      const QString& text);
    void systemEvent(const QString& text);      // "* X entrou no canal Y" etc.
    void pokeReceived(const QString& fromName, const QString& msg);
    void kickedReceived(const QString& reason, bool ban, int minutes);
    void errorOccurred(const QString& code, const QString& msg); // erro do servidor
    void connectionFailed(const QString& reason);                // TCP falhou/negado
    void disconnectedUnexpected();
    void voicePacketReceived(int fromId, quint16 seq, const QByteArray& payload);
    void pingUpdated(int ms);

private slots:
    void onConnected();
    void onReadyRead();
    void onDisconnected();
    void onUdpReadyRead();
    void onPingTimer();

private:
    void send(const QJsonObject& obj);
    void handleMessage(const QJsonObject& obj);
    void applyChanJson(const QJsonObject& c);
    void applyUserJson(const QJsonObject& u);
    ServerData& target() { return m_target ? *m_target : m_data; }

    QTcpSocket* m_tcp = nullptr;
    QUdpSocket* m_udp = nullptr;
    QTimer* m_pingTimer = nullptr;
    QByteArray m_buffer;
    ServerData m_data;
    ServerData* m_target = nullptr;

    QString m_host;
    quint16 m_port = 9987;
    QString m_hostPort;
    QJsonObject m_pendingHello;
    quint16 m_udpPort = 0;
    quint32 m_voiceToken = 0;
    bool m_ready = false;
    bool m_fatalError = false;
    int m_pingMs = 0;
    QElapsedTimer m_pingClock;
};
