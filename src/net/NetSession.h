#pragma once

#include <QObject>
#include <QTcpSocket>
#include <QUdpSocket>
#include <QTimer>
#include <QJsonObject>
#include <QJsonArray>
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

    // v3: permissões do meu grupo + grupos do servidor (vindos do welcome)
    QJsonObject myPerms() const { return m_myPerms; }
    QJsonArray  serverGroups() const { return m_groups; }

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

    // ---- v3: avatares, offline, reclamações, sussurro, arquivos, banlist
    void avatarSet(const QByteArray& imageBytes);       // vazio = remover
    void avatarGet(const QString& uid);
    void offlineSend(const QString& uid, const QString& text);
    void complaintAdd(int userId, const QString& text);
    void complaintList();
    void complaintClear(const QString& uid = QString());
    void setWhisperIds(const QList<int>& ids);          // vazio = fala normal
    void ftUpload(int channel, const QString& name, const QByteArray& data);
    void ftList(int channel);
    void ftDownload(int channel, const QString& name);
    void ftDelete(int channel, const QString& name);
    void requestBanList();
    void iconGet(const QString& name);
    void iconSet(const QString& name, const QByteArray& bytes);
    void unban(const QString& uid);
    void requestGroupList();
    void groupSet(int id, const QString& name, const QJsonObject& perms, const QString& sigla = QString(), int order = 0, const QString& icon = QString()); // id 0 = criar
    void groupDelete(int id);
    void clientSetGroup(int userId, int gid);
    void serverEdit(const QString& name, const QString& motd);

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

    // ---- v3
    void avatarDataReceived(const QString& uid, const QByteArray& bytes);
    void iconDataReceived(const QString& name, const QByteArray& bytes);
    void userAvatarChanged(int userId, const QString& hash);
    void offlineMsgReceived(const QString& fromName, const QString& text, const QString& ts);
    void offlineSendConfirmed(const QString& uid);
    void complaintListReceived(const QJsonArray& complaints);
    void banListReceived(const QJsonArray& bans);
    void groupListReceived(const QJsonArray& groups);
    void ftListReceived(int channel, const QJsonArray& files);
    void ftDataReceived(int channel, const QString& name, const QByteArray& bytes);
    void ftUploadConfirmed(int channel, const QString& name);
    void ftDeleteConfirmed(int channel, const QString& name);
    void whisperConfirmed(int count);

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
    void refreshOperators();
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
    QJsonObject m_myPerms;        // v3 (welcome.myPerms)
    QJsonArray  m_groups;         // v3 (welcome.groups)
    quint16 m_udpPort = 0;
    quint32 m_voiceToken = 0;
    bool m_ready = false;
    bool m_fatalError = false;
    int m_pingMs = 0;
    QElapsedTimer m_pingClock;
};
