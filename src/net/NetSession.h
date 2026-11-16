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

// Halla client network session: TCP (control) + UDP (voice) with the Halla server.
// Keeps a ServerData model always synchronized with what the server sends.
//
// v6 E2EE: group keys are born HERE (master = lowest UID in the component),
// travel wrapped in e2e_key (X25519 + HKDF + AES-GCM), and the server is only
// an opaque relay. Chat, whisper, poke, offline messages, voice, and screen
// sharing never leave the endpoints in plaintext.
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

    // v3: my group permissions + server groups (from the welcome message).
    QJsonObject myPerms() const { return m_myPerms; }
    QJsonArray  serverGroups() const { return m_groups; }
    bool        allowScreenShare() const { return m_allowScreenShare; }
    int         screenshareWidth() const { return m_screenshareWidth; }
    int         screenshareHeight() const { return m_screenshareHeight; }
    int         screenshareFps() const    { return m_screenshareFps; }
    int         screenshareBitrateKbps() const { return m_screenshareBitrateKbps; }
    QJsonArray  webRtcIceServers() const  { return m_webRtcIceServers; }
    qint64      bytesToWrite() const      { return m_tcp ? m_tcp->bytesToWrite() : 0; }

    // ---- public structure (applied to ServerData, emitting stateChanged)
    void attachTo(ServerData* target) { m_target = target; }

    // ---- client actions (sent to the server)
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

    // ---- v3: avatars, offline messages, complaints, whisper, files, banlist
    void avatarSet(const QByteArray& imageBytes);       // empty = remove
    void avatarGet(const QString& uid);
    void offlineSend(const QString& uid, const QString& text);
    void complaintAdd(int userId, const QString& text);
    void complaintList();
    void complaintClear(const QString& uid = QString());
    void setWhisperIds(const QList<int>& ids);          // empty = normal speech
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
                  bool siglaAfter = false, bool orderEnabled = true); // id 0 = create, position=-1 = leave unchanged
    // Batch reorder: a single message with {id, order, position} per group --
    // the server stores and broadcasts ONCE instead of N times.
    void groupReorder(const QJsonArray& entries);
    void groupDelete(int id);
    void clientSetGroup(int userId, int gid, bool remove = false);
    void clientSetGroupUid(const QString& uid, int gid, bool remove = false);
    void serverEdit(const QString& name, const QString& motd,
                    const QByteArray& banner = QByteArray(), bool bannerChanged = false);

    // ---- voice
    void sendVoiceFrame(const QByteArray& opus, quint16 seq);
    void sendScreenShareStart();
    void sendScreenShareStop();
    void sendScreenShareFrame(const QByteArray& jpeg, quint16 seq);

    // WebRTC signaling v1 (TCP/TLS signaling; media is negotiated by clients).
    void sendWebRtcStreamStart(int width, int height, int fps, int bitrateKbps);
    void sendWebRtcStreamStop();
    void sendWebRtcWatchRequest(int userId);
    void sendWebRtcWatchStop(int userId);
    void sendWebRtcOffer(int toUserId, const QString& sdp);
    void sendWebRtcAnswer(int toUserId, const QString& sdp);
    void sendWebRtcIce(int toUserId, const QString& candidate,
                       const QString& sdpMid = QString(), int sdpMLineIndex = -1);

    // Protocol v5: reliable, delimited plugin data transport.
    bool sendPluginData(const QString& pluginId, int target,
                        const QList<int>& targetUserIds, const QString& topic,
                        const QByteArray& data);

    // ---- v6 E2EE (public, for the UI)
    // Available group keys (server scope + my channel): used by the status
    // bar lock indicator.
    bool e2eeKeysReady() const;
    // My identity public key (exposed so the E2EE binding and the backup
    // machinery can reference the local identity).
    QByteArray e2eeMyIdPub() const;

signals:
    void welcomeReceived();                     // full state loaded
    void selfRenamed(const QString& name);      // server confirmed our new nickname (user_nick)
    void stateChanged();                        // ServerData changed (UI rebuild)
    // toId: the real recipient of private messages. The server echoes a
    // private message back to the sender (from = me) so it appears in the
    // conversation; without "to" the client cannot tell echo from new message.
    void chatReceived(const QString& scope, int fromId, int toId, const QString& fromName,
                      const QString& text);
    void systemEvent(const QString& text);      // "* X joined channel Y" etc.
    void pokeReceived(int fromUserId, const QString& fromName, const QString& msg);
    void kickedReceived(const QString& reason, bool ban, int minutes);
    void errorOccurred(const QString& code, const QString& msg); // server error
    void connectionFailed(const QString& reason);                // TCP failed/refused
    // Login refused because of nickname (name_in_use/bad_nick): the server
    // closes the connection; the UI asks for another nickname and reconnects.
    void nickRejected(const QString& message);
    void disconnectedUnexpected();
    void voicePacketReceived(int fromId, quint16 seq, const QByteArray& payload);
    // v1.1.28: mode = "webrtc" | "jpeg" | empty (unknown/old server).
    // The viewer decides HOW to watch based on the broadcaster mode.
    void screenshareStateChanged(int userId, bool on, const QString& mode);
    void screenshareFrameReceived(int userId, const QByteArray& jpegData);
    void webRtcSignalReceived(const QJsonObject& signal);
    void pluginDataReceived(int senderUserId, const QString& pluginId,
                            const QString& topic, const QByteArray& data);
    void pingUpdated(int ms);

    // ---- v3
    void avatarDataReceived(const QString& uid, const QByteArray& bytes);
    void iconDataReceived(const QString& name, const QByteArray& bytes);
    // An administrator uploaded (or replaced) a group icon: the server
    // broadcasts icon_uploaded to every connected client.
    void iconUploaded(const QString& name);
    void userAvatarChanged(int userId, const QString& hash);
    void offlineMsgReceived(const QString& fromName, const QString& text, const QString& ts);
    void offlineSendConfirmed(const QString& uid);
    void complaintListReceived(const QJsonArray& complaints);
    void banListReceived(const QJsonArray& bans);
    void groupListReceived(const QJsonArray& groups);
    void groupSetConfirmed(const QJsonObject& group);
    // A group membership list changed on the server (assignment/removal):
    // updates open group panels in real time.
    void groupMembersUpdated(int gid, const QJsonArray& members);
    void ftListReceived(int channel, const QJsonArray& files);
    void ftDataReceived(int channel, const QString& name, const QByteArray& bytes);
    void ftUploadConfirmed(int channel, const QString& name);
    void ftDeleteConfirmed(int channel, const QString& name);
    void whisperConfirmed(int count);

    // ---- v6 E2EE
    // Security notice (identity changed, invalid directory, server tried to
    // send channel_key, etc.). Shown prominently in the UI.
    void e2eeSecurityNotice(const QString& text);
    // Key availability changed (UI lock indicator).
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
    // NAT/keepalive registration probe: a tiny encrypted frame that only
    // teaches the relay our UDP endpoint. Kept separate from real media so
    // sendVoiceFrame/sendScreenShareFrame never emit unencrypted payloads.
    void sendUdpNatProbe(quint16 seq);
    ServerData& target() { return m_target ? *m_target : m_data; }
    // v6 E2EE: topology queries (component/master/SAS) run in const methods --
    // reading the model must not require mutability.
    const ServerData& target() const { return m_target ? *m_target : m_data; }

    // ============================ v6 E2EE -- local engine
    struct E2eeGroupKey {
        QByteArray key;     // 32 bytes
        qint64 epoch = 0;   // monotonic (Unix ms) -- highest wins
    };
    bool e2eeLoadMaterial();                       // loads/generates the X25519 pair and signs the hello
    QSet<int> e2eeComponentOf(int channelId) const; // links in both directions
    QStringList e2eeComponentMemberUids(const QSet<int>& comp) const;
    bool e2eeIsMasterOfComponent(int channelId) const; // lowest online UID in the component
    bool e2eeIsServerScopeMaster() const;              // lowest UID on the whole server
    QList<int> e2eeComponentMembers(const QSet<int>& comp) const; // session ids
    void e2eeBootstrap();                          // welcome: generate/request keys
    void e2eeEnsureComponentKey(int channelId);    // generate if missing + distribute (master)
    void e2eeRotateComponentKey(int channelId);    // new epoch + distribute (master)
    void e2eeDistributeComponentKey(const QSet<int>& comp, const E2eeGroupKey& gk);
    void e2eeShareKeyWith(int sessionId, const QSet<int>& comp, const E2eeGroupKey& gk);
    void e2eeRequestKey(int channelId);
    void e2eeHandleKeyEnvelope(const QJsonObject& obj);
    void e2eeHandleKeyRequest(const QJsonObject& obj);
    void e2eeHandleIdentityData(const QJsonObject& obj);
    void e2eeApplyGroupKey(const QList<int>& channels, qint64 epoch, const QByteArray& key);
    void e2eeDistributeWhisperKey();               // my channel -> whisper targets
    void e2eeOnUserJoined(const QJsonObject& user);
    void e2eeOnUserLeft(int userId, int oldChannel);
    void e2eeOnUserMoved(int userId, int newChannel, int oldChannel);
    void e2eeOnTopologyChanged();                  // chan_update/chan_removed
    void e2eeSecurityCheckUser(const User& u);     // local marker + change detection
    void e2eeFlushPending();                       // chats/offlines waiting for a key
    void e2eeClearState();                         // disconnect: keys and queues vanish
    // Decrypts/delivers a received offline message (queued when the sender is
    // not in the local directory -- identity_data resolves and reapplies it).
    void e2eeDeliverOfflineMsg(const QJsonObject& obj, qint64 receivedAt = 0);

    QSslSocket* m_tcp = nullptr;
    QUdpSocket* m_udp = nullptr;
    QTimer* m_pingTimer = nullptr;
    QByteArray m_buffer;
    ServerData m_data;
    ServerData* m_target = nullptr;
    QMap<int, QByteArray> m_channelKeys; // channelId -> AEAD key (32 bytes)
    QMap<int, qint64> m_channelEpochs;   // channelId -> current epoch (v6)

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
    QByteArray m_voiceToken; // protocol v4: 128-bit CSPRNG token
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

    // ============================ v6 E2EE -- state
    QByteArray m_e2eeDhPriv;                 // session X25519 private key
    QByteArray m_e2eeMyIdPub;                // local Ed25519 public key (matches self)
    QTimer* m_e2eeHousekeeper = nullptr;     // re-requests, queues, whisper re-wrap
    QList<int> m_whisperIds;                 // active whisper targets (re-wrap)
    bool m_e2eeWhisperNeedsRewrap = false;
    // True while the current whisper target set includes at least one user
    // outside my voice component (they hold my channel key via e2ee_key).
    bool m_whisperSharedExternal = false;
    // Directory by UID (online entries + identity_data for offline users):
    // uid -> {idPub, dhPub, dhSig} in base64.
    QHash<QString, QJsonObject> m_e2eeDirectory;
    struct PendingChat { QString scope; int to; QString text; qint64 queuedAt; };
    QList<PendingChat> m_pendingChats;
    struct PendingOffline { QString uid; QString text; qint64 queuedAt; };
    QList<PendingOffline> m_pendingOffline;
    struct PendingOfflineInbox { QString fromUid; QString fromName;
                                  QString blobB64; QString ts; qint64 receivedAt; };
    QList<PendingOfflineInbox> m_pendingOfflineInbox;
    QMap<int, int> m_e2eeKeyRequestTries;    // channelId -> attempts (backoff)
    qint64 m_e2eeLastRequestAt = 0;
    qint64 m_e2eeLastNoKeyWarnMs = 0;        // cadence (30 s) of the missing-key warning
    // Anti-split-brain for clock skew: the master that generated a key with a
    // LOCAL-clock epoch also ASKS the others -- a reply with a higher epoch is
    // adopted (convergence); a lower one is discarded without effect.
    QList<int> m_e2eeVerifyPending;
    // Visibility of the voice lock caused by a missing key: start of the
    // current wait and the channel we already warned about. The affected user
    // must know WHY they are muted in both directions instead of guessing.
    qint64 m_e2eeKeyWaitStartMs = 0;
    int m_e2eeWaitChan = -1;
    int m_e2eeVoiceKeyWarnedChan = -1;
    bool m_e2eeSelfEntryWarned = false;      // one-shot warning: invalid self entry
    // v1.1.36 -- stale-key healing (the "it worked and then stopped" case):
    // when a channel key rotates (someone joins/leaves the component) and the
    // new-key push is lost, the client keeps the OLD key -- TX encrypts with
    // it (nobody hears) and RX decrypts nothing (hears nobody). The
    // housekeeper never re-requests because it "has" a key. Consecutive
    // decryption-failure counter on MY channel: at the threshold, forget the
    // local key (remove + reset attempts) and let the housekeeper re-request
    // from the group -- the reply applies without an epoch barrier
    // (!contains) and voice returns in both directions.
    int m_e2eeDecryptFails = 0;              // consecutive failures (reset on success)
    qint64 m_e2eeDecryptRecoverMs = 0;       // last recovery triggered (retry after 30 s)

    // v1.1.37 -- context of recent "move"/"move_other" requests. Server errors
    // arrive with only a code (no_permission, channel_full...) and the generic
    // table message does not say WHICH channel it refers to: the user clicked
    // a channel and read "You do not have permission to perform this action"
    // without understanding what went wrong. Since TCP preserves order, an
    // error is attributed to the oldest move still awaiting a reply (FIFO) --
    // user_moved for the matching channel consumes the entry. Short window
    // (6 s) and small list: a wrong attribution is practically impossible.
    struct PendingMove {
        int channel;        // destination channel
        QString name;       // its name at request time
        bool otherUser;     // move (self) or move_other (dragging/moving someone)
        qint64 atMs;        // when it was sent
    };
    QList<PendingMove> m_pendingMoves;
    // Builds the CONTEXTUAL error message for the oldest unanswered move;
    // returns QString() when there is no plausible candidate (the caller
    // shows the generic table message).
    QString consumeMoveErrorContext(const QString& code);
};
