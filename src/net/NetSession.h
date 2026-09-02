#pragma once

#include <QObject>
#include <QSslSocket>
#include <QSslError>
#include <QUdpSocket>
#include <QTimer>
#include <QJsonObject>
#include <QJsonArray>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QSet>
#include "core/Models.h"

// Sessão de rede do cliente Halla: TCP (controle) + UDP (voz) com o Halla Server.
// Mantém um ServerData sempre sincronizado com o que o servidor manda.
//
// v6 E2EE: chaves de grupo nascem AQUI (mestre = menor UID do componente),
// viajam embrulhadas em e2e_key (X25519+HKDF+AES-GCM), e o servidor é só um
// relay opaco. Chat/sussurro/poke/offline/voz/tela nunca saem em claro.
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
    bool serverTerminatedSession() const { return m_serverTerminatedSession; }

    // v3: permissões do meu grupo + grupos do servidor (vindos do welcome)
    QJsonObject myPerms() const { return m_myPerms; }
    QJsonArray  serverGroups() const { return m_groups; }
    bool        allowScreenShare() const { return m_allowScreenShare; }
    int         screenshareWidth() const { return m_screenshareWidth; }
    int         screenshareHeight() const { return m_screenshareHeight; }
    int         screenshareFps() const    { return m_screenshareFps; }
    int         screenshareBitrateKbps() const { return m_screenshareBitrateKbps; }
    QJsonArray  webRtcIceServers() const  { return m_webRtcIceServers; }
    qint64      bytesToWrite() const      { return m_tcp ? m_tcp->bytesToWrite() : 0; }

    // ---- estrutura pública (aplicada em ServerData e emitindo stateChanged)
    void attachTo(ServerData* target) { m_target = target; }

    // ---- ações do cliente (enviadas ao servidor)
    void sendChat(const QString& scope, int to, const QString& text);
    void moveToChannel(int channelId, const QString& pass = QString());
    void moveOther(int userId, int channelId);
    void moveChannel(int channelId, int parentId, int order);
    void linkChannels(const QList<int>& channelIds, bool link);
    void setCommander(int userId, bool on);
    void sendStatus();
    void sendTalking(bool on);
    void rename(const QString& newName, int targetUserId = 0);
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
    void groupSet(int id, const QString& name, const QJsonObject& perms,
                  const QString& sigla = QString(), int order = 0,
                  const QString& icon = QString(), int position = -1,
                  bool siglaAfter = false, bool orderEnabled = true); // id 0 = criar, position=-1 = não alterar
    // Reordenação em lote: uma única mensagem com {id, order, position} de
    // cada cargo — o servidor grava e transmite UMA vez em vez de N.
    void groupReorder(const QJsonArray& entries);
    void groupDelete(int id);
    void clientSetGroup(int userId, int gid, bool remove = false);
    void clientSetGroupUid(const QString& uid, int gid, bool remove = false);
    void serverEdit(const QString& name, const QString& motd,
                    const QByteArray& banner = QByteArray(), bool bannerChanged = false);

    // ---- voz
    void sendVoiceFrame(const QByteArray& opus, quint16 seq);
    void sendScreenShareStart();
    void sendScreenShareStop();
    void sendScreenShareFrame(const QByteArray& jpeg, quint16 seq);

    // WebRTC signaling v1 (TCP/TLS signaling; media is negotiated by clients)
    void sendWebRtcStreamStart(int width, int height, int fps, int bitrateKbps);
    void sendWebRtcStreamStop();
    void sendWebRtcWatchRequest(int userId);
    void sendWebRtcWatchStop(int userId);
    void sendWebRtcOffer(int toUserId, const QString& sdp);
    void sendWebRtcAnswer(int toUserId, const QString& sdp);
    void sendWebRtcIce(int toUserId, const QString& candidate,
                       const QString& sdpMid = QString(), int sdpMLineIndex = -1);

    // Protocolo v5: transporte confiável e delimitado por complemento.
    bool sendPluginData(const QString& pluginId, int target,
                        const QList<int>& targetUserIds, const QString& topic,
                        const QByteArray& data);

    // ---- v6 E2EE (público para a UI)
    // Chaves de grupo disponíveis (escopo servidor + meu canal): usado pelo
    // indicador de cadeado da barra de status.
    bool e2eeKeysReady() const;
    // Código SAS para verificação fora de banda com outro usuário (9 dígitos
    // derivados das duas chaves públicas de identidade).
    QString e2eeSasCodeFor(int userId) const;
    // Estado de verificação persistido localmente (por UID).
    bool e2eeUserVerified(int userId) const;
    void e2eeMarkUserVerified(int userId);
    // Minha chave pública de identidade (para diálogos de verificação).
    QByteArray e2eeMyIdPub() const;

signals:
    void welcomeReceived();                     // estado completo carregado
    void selfRenamed(const QString& name);      // servidor confirmou nosso novo apelido (user_nick)
    void stateChanged();                        // ServerData mudou (rebuild da UI)
    // toId: destinatário real de mensagens privadas. O servidor ecoa a
    // mensagem privada de volta ao remetente (from = eu) para ela aparecer
    // na conversa; sem o "to" o cliente não distingue eco de mensagem nova.
    void chatReceived(const QString& scope, int fromId, int toId, const QString& fromName,
                      const QString& text);
    void systemEvent(const QString& text);      // "* X entrou no canal Y" etc.
    void pokeReceived(int fromUserId, const QString& fromName, const QString& msg);
    void kickedReceived(const QString& reason, bool ban, int minutes);
    void errorOccurred(const QString& code, const QString& msg); // erro do servidor
    void connectionFailed(const QString& reason);                // TCP falhou/negado
    // Login recusado por apelido (name_in_use/bad_nick): o servidor fecha a
    // conexão; a UI pede outro apelido e reconecta.
    void nickRejected(const QString& message);
    void disconnectedUnexpected();
    void voicePacketReceived(int fromId, quint16 seq, const QByteArray& payload);
    void screenshareStateChanged(int userId, bool on);
    void screenshareFrameReceived(int userId, const QByteArray& jpegData);
    void webRtcSignalReceived(const QJsonObject& signal);
    void pluginDataReceived(int senderUserId, const QString& pluginId,
                            const QString& topic, const QByteArray& data);
    void pingUpdated(int ms);

    // ---- v3
    void avatarDataReceived(const QString& uid, const QByteArray& bytes);
    void iconDataReceived(const QString& name, const QByteArray& bytes);
    // Administrador enviou (ou substituiu) um ícone de cargo: o servidor
    // faz broadcast de icon_uploaded para todos os clientes conectados.
    void iconUploaded(const QString& name);
    void userAvatarChanged(int userId, const QString& hash);
    void offlineMsgReceived(const QString& fromName, const QString& text, const QString& ts);
    void offlineSendConfirmed(const QString& uid);
    void complaintListReceived(const QJsonArray& complaints);
    void banListReceived(const QJsonArray& bans);
    void groupListReceived(const QJsonArray& groups);
    void groupSetConfirmed(const QJsonObject& group);
    // Lista de membros de um cargo mudou no servidor (atribuição/remoção):
    // atualiza os painéis de grupos abertos em tempo real.
    void groupMembersUpdated(int gid, const QJsonArray& members);
    void ftListReceived(int channel, const QJsonArray& files);
    void ftDataReceived(int channel, const QString& name, const QByteArray& bytes);
    void ftUploadConfirmed(int channel, const QString& name);
    void ftDeleteConfirmed(int channel, const QString& name);
    void whisperConfirmed(int count);

    // ---- v6 E2EE
    // Aviso de segurança (identidade mudou, diretório inválido, servidor
    // tentou enviar channel_key etc.). Exibido com destaque na UI.
    void e2eeSecurityNotice(const QString& text);
    // Disponibilidade de chaves mudou (indicador de cadeado na UI).
    void e2eeStateChanged();

private slots:
    void onConnected();
    void onReadyRead();
    void onDisconnected();
    void onUdpReadyRead();
    void onPingTimer();
    void onSslErrors(const QList<QSslError>& errors);
    void onE2eeHousekeeping();

private:
    void send(const QJsonObject& obj);
    void handleMessage(const QJsonObject& obj);
    void applyChanJson(const QJsonObject& c);
    void applyUserJson(const QJsonObject& u);
    void refreshOperators();
    void scheduleChannelStateChanged();
    ServerData& target() { return m_target ? *m_target : m_data; }
    // v6 E2EE: as consultas de topo (componente/mestre/SAS) rodam em métodos
    // const — a leitura do modelo não pode exigir mutabilidade.
    const ServerData& target() const { return m_target ? *m_target : m_data; }

    // ============================ v6 E2EE — motor local
    struct E2eeGroupKey {
        QByteArray key;     // 32 bytes
        qint64 epoch = 0;   // monotônica (ms Unix) — maior vence
    };
    bool e2eeLoadMaterial();                       // carrega/gera o par X25519 e assina o hello
    QSet<int> e2eeComponentOf(int channelId) const; // vínculos em ambos os sentidos
    QStringList e2eeComponentMemberUids(const QSet<int>& comp) const;
    bool e2eeIsMasterOfComponent(int channelId) const; // menor UID online do componente
    bool e2eeIsServerScopeMaster() const;              // menor UID do servidor inteiro
    QList<int> e2eeComponentMembers(const QSet<int>& comp) const; // ids de sessão
    void e2eeBootstrap();                          // welcome: gera/pede chaves
    void e2eeEnsureComponentKey(int channelId);    // gera se faltar + distribui (mestre)
    void e2eeRotateComponentKey(int channelId);    // nova época + distribui (mestre)
    void e2eeDistributeComponentKey(const QSet<int>& comp, const E2eeGroupKey& gk);
    void e2eeShareKeyWith(int sessionId, const QSet<int>& comp, const E2eeGroupKey& gk);
    void e2eeRequestKey(int channelId);
    void e2eeHandleKeyEnvelope(const QJsonObject& obj);
    void e2eeHandleKeyRequest(const QJsonObject& obj);
    void e2eeHandleIdentityData(const QJsonObject& obj);
    void e2eeApplyGroupKey(const QList<int>& channels, qint64 epoch, const QByteArray& key);
    void e2eeDistributeWhisperKey();               // meu canal → alvos do sussurro
    void e2eeOnUserJoined(const QJsonObject& user);
    void e2eeOnUserLeft(int userId, int oldChannel);
    void e2eeOnUserMoved(int userId, int newChannel, int oldChannel);
    void e2eeOnTopologyChanged();                  // chan_update/chan_removed
    void e2eeSecurityCheckUser(const User& u);     // marcador local + detecção de troca
    void e2eeFlushPending();                       // chats/offlines à espera de chave
    void e2eeClearState();                         // desconexão: chaves e filas somem
    // Decifra/entrega mensagem offline recebida (fila se o remetente não
    // estiver no diretório local — identity_data resolve e reaplica).
    void e2eeDeliverOfflineMsg(const QJsonObject& obj, qint64 receivedAt = 0);

    QSslSocket* m_tcp = nullptr;
    QUdpSocket* m_udp = nullptr;
    QTimer* m_pingTimer = nullptr;
    QByteArray m_buffer;
    ServerData m_data;
    ServerData* m_target = nullptr;
    QMap<int, QByteArray> m_channelKeys; // channelId -> AEAD key (32 bytes)
    QMap<int, qint64> m_channelEpochs;   // channelId -> época vigente (v6)

    QString m_host;
    QHostAddress m_udpHostAddress;
    quint16 m_port = 9987;
    QString m_hostPort;
    QString m_identityUid;
    QJsonObject m_pendingHello;
    QJsonObject m_myPerms;        // v3 (welcome.myPerms)
    QJsonArray  m_groups;         // v3 (welcome.groups)
    bool        m_allowScreenShare = true;
    int         m_screenshareWidth = 1920;
    int         m_screenshareHeight = 1080;
    int         m_screenshareFps = 60;
    int         m_screenshareBitrateKbps = 8000;
    QJsonArray  m_webRtcIceServers;
    quint16 m_udpPort = 0;
    QByteArray m_voiceToken; // protocolo v4: token CSPRNG de 128 bits
    quint16 m_udpRegistrationSeq = 0;
    quint32 m_cryptoCounter = 0;
    QMap<int, QMap<quint16, QMap<int, QByteArray>>> m_reassembly;
    bool m_ready = false;
    bool m_fatalError = false;
    bool m_intentionalDisconnect = false;
    bool m_serverTerminatedSession = false;
    bool m_channelStateEmitPending = false;
    int m_pingMs = 0;
    QElapsedTimer m_pingClock;

    // ============================ v6 E2EE — estado
    QByteArray m_e2eeDhPriv;                 // X25519 privada da sessão
    QByteArray m_e2eeMyIdPub;                // Ed25519 pública local (confere o self)
    QTimer* m_e2eeHousekeeper = nullptr;     // re-pedidos, filas, re-wrap do sussurro
    QList<int> m_whisperIds;                 // alvos do sussurro ativo (re-wrap)
    bool m_e2eeWhisperNeedsRewrap = false;
    // Diretório por UID (entradas online + identity_data de quem está offline):
    // uid -> {idPub, dhPub, dhSig} em base64.
    QHash<QString, QJsonObject> m_e2eeDirectory;
    struct PendingChat { QString scope; int to; QString text; qint64 queuedAt; };
    QList<PendingChat> m_pendingChats;
    struct PendingOffline { QString uid; QString text; qint64 queuedAt; };
    QList<PendingOffline> m_pendingOffline;
    struct PendingOfflineInbox { QString fromUid; QString fromName;
                                  QString blobB64; QString ts; qint64 receivedAt; };
    QList<PendingOfflineInbox> m_pendingOfflineInbox;
    QMap<int, int> m_e2eeKeyRequestTries;    // channelId -> tentativas
    qint64 m_e2eeLastRequestAt = 0;
    bool m_e2eeLoggedNoKeyVoice = false;     // aviso único de frame descartado
};
