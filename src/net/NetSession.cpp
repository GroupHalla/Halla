#include "NetSession.h"
#include "HallaProtocol.h"
#include "core/AppLog.h"

#include <QJsonDocument>
#include <QJsonArray>
#include <QNetworkDatagram>
#include <QHostInfo>

NetSession::NetSession(QObject* parent) : QObject(parent) {
    m_tcp = new QTcpSocket(this);
    connect(m_tcp, &QTcpSocket::connected, this, &NetSession::onConnected);
    connect(m_tcp, &QTcpSocket::readyRead, this, &NetSession::onReadyRead);
    connect(m_tcp, &QTcpSocket::disconnected, this, &NetSession::onDisconnected);

    m_udp = new QUdpSocket(this);
    m_udp->bind();
    connect(m_udp, &QUdpSocket::readyRead, this, &NetSession::onUdpReadyRead);

    m_pingTimer = new QTimer(this);
    m_pingTimer->setInterval(3000);
    connect(m_pingTimer, &QTimer::timeout, this, &NetSession::onPingTimer);
}

void NetSession::connectToServer(const QString& host, quint16 port, const QString& nickname,
                                 const QString& uid, const QString& password,
                                 const QString& adminPassword) {
    m_host = host;
    m_udpHostAddress = QHostAddress(host);
    if (m_udpHostAddress.isNull()) {
        const QHostInfo resolved = QHostInfo::fromName(host);
        if (!resolved.addresses().isEmpty())
            m_udpHostAddress = resolved.addresses().constFirst();
    }
    m_port = port;
    m_hostPort = port == 9987 ? host : QStringLiteral("%1:%2").arg(host).arg(port);
    m_udpPort = 0;
    m_voiceToken = 0;
    m_udpRegistrationSeq = 0;
    m_buffer.clear();

    ServerData& d = target();
    d.name = host;
    d.address = m_hostPort;
    d.connectedAt = QDateTime::currentDateTime();
    User self;
    self.id = 1;
    self.name = nickname;
    self.uniqueId = uid;
    d.users.clear();
    d.users[self.id] = self;
    d.channels.clear();

    m_pendingHello = HProto::msg("hello");
    m_pendingHello["proto"] = HProto::kProtoVersion;
    m_pendingHello["uid"] = uid;
    m_pendingHello["nick"] = nickname;
    m_pendingHello["ver"] = "3.6.2";
    m_pendingHello["platform"] =
#ifdef Q_OS_WIN
        "Windows";
#else
        "Linux";
#endif
    if (!password.isEmpty()) m_pendingHello["pass"] = password;
    if (!adminPassword.isEmpty()) m_pendingHello["adminPass"] = adminPassword;

    m_tcp->connectToHost(host, port);
}

void NetSession::onConnected() {
    send(m_pendingHello);
}

void NetSession::send(const QJsonObject& obj) {
    if (m_tcp->state() == QAbstractSocket::ConnectedState)
        m_tcp->write(QJsonDocument(obj).toJson(QJsonDocument::Compact) + '\n');
}

void NetSession::onReadyRead() {
    m_buffer += m_tcp->readAll();
    int idx;
    while ((idx = m_buffer.indexOf('\n')) >= 0) {
        QByteArray line = m_buffer.left(idx).trimmed();
        m_buffer = m_buffer.mid(idx + 1);
        if (line.isEmpty()) continue;
        QJsonDocument doc = QJsonDocument::fromJson(line);
        if (doc.isObject()) handleMessage(doc.object());
    }
}

void NetSession::onDisconnected() {
    m_pingTimer->stop();
    m_udpPort = 0;
    m_voiceToken = 0;
    m_udpRegistrationSeq = 0;
    if (!m_ready && !m_fatalError)
        emit connectionFailed(QStringLiteral("Não foi possível conectar ao servidor"));
    else if (m_ready)
        emit disconnectedUnexpected();
    m_ready = false;
}

void NetSession::onPingTimer() {
    QJsonObject p = HProto::msg("ping");
    m_pingClock.restart();
    send(p);

    // Mantém o endpoint UDP do PC conhecido no relay mesmo quando o usuário
    // não abriu o microfone/encoder. O servidor registra o endereço antes de
    // validar o payload, então este frame não audível também atravessa NAT e
    // impede que a primeira fala do Mobile dependa de o PC falar antes.
    if (m_voiceToken && m_udpPort) {
        sendVoiceFrame(QByteArray(1, '\0'), ++m_udpRegistrationSeq);
    }
}

void NetSession::onUdpReadyRead() {
    while (m_udp->hasPendingDatagrams()) {
        QNetworkDatagram dg = m_udp->receiveDatagram();
        QByteArray data = dg.data();
        if (data.size() < 10 || memcmp(data.constData(), "HALL", 4) != 0) continue;
        quint32 fromId;
        quint16 seq;
        memcpy(&fromId, data.constData() + 4, 4);
        memcpy(&seq, data.constData() + 8, 2);
        emit voicePacketReceived(int(fromId), seq, data.mid(10));
    }
}

void NetSession::sendVoiceFrame(const QByteArray& opus, quint16 seq) {
    if (!m_voiceToken || m_udpPort == 0) return;
    const QHostAddress destination = m_udpHostAddress.isNull()
        ? QHostAddress(m_host) : m_udpHostAddress;
    if (destination.isNull()) return;
    m_udp->writeDatagram(HProto::encodeVoiceClient(m_voiceToken, seq, opus),
                         destination, m_udpPort);
}

// ==================================================================== ações
void NetSession::sendChat(const QString& scope, int to, const QString& text) {
    QJsonObject m = HProto::msg("chat");
    m["scope"] = scope;
    if (to > 0) m["to"] = to;
    m["text"] = text;
    send(m);
}

void NetSession::moveToChannel(int channelId, const QString& pass) {
    QJsonObject m = HProto::msg("move");
    m["channel"] = channelId;
    if (!pass.isEmpty()) m["pass"] = pass;
    send(m);
}

void NetSession::moveOther(int userId, int channelId) {
    QJsonObject m = HProto::msg("move_other");
    m["id"] = userId;
    m["channel"] = channelId;
    send(m);
}

void NetSession::moveChannel(int channelId, int parentId, int order) {
    QJsonObject m = HProto::msg("chan_move");
    m["id"] = channelId;
    m["parent"] = parentId;
    m["order"] = order;
    send(m);
}

void NetSession::linkChannels(const QList<int>& channelIds, bool link) {
    QJsonObject m = HProto::msg("chan_link");
    QJsonArray ids;
    QList<int> seen;
    for (int id : channelIds) {
        if (id > 0 && !seen.contains(id)) {
            seen << id;
            ids << id;
        }
    }
    if (ids.size() < 2) return;
    m["ids"] = ids;
    m["link"] = link;
    send(m);
}

void NetSession::setCommander(int userId, bool on) {
    QJsonObject m = HProto::msg("commander");
    m["id"] = userId;
    m["on"] = on;
    send(m);
}

void NetSession::sendStatus() {
    const User& self = target().users[target().selfId];
    QJsonObject m = HProto::msg("status");
    m["mic"] = self.inputMuted;
    m["spk"] = self.outputMuted;
    m["away"] = self.away;
    m["rec"] = self.recording;
    m["cc"] = self.commander;
    send(m);
}

void NetSession::sendTalking(bool on) {
    QJsonObject m = HProto::msg("talking");
    m["on"] = on;
    send(m);
}

void NetSession::rename(const QString& newName) {
    QJsonObject m = HProto::msg("nick");
    m["name"] = newName;
    send(m);
}

void NetSession::setDescription(const QString& desc) {
    QJsonObject m = HProto::msg("desc");
    m["text"] = desc;
    send(m);
}

void NetSession::poke(int userId, const QString& msg) {
    QJsonObject m = HProto::msg("poke");
    m["to"] = userId;
    m["msg"] = msg;
    send(m);
}

void NetSession::createChannel(const QJsonObject& chan) {
    QJsonObject m = HProto::msg("chan_create");
    for (auto it = chan.begin(); it != chan.end(); ++it) m[it.key()] = it.value();
    m.remove("id");
    send(m);
}

void NetSession::editChannel(const QJsonObject& chan) {
    QJsonObject m = HProto::msg("chan_edit");
    for (auto it = chan.begin(); it != chan.end(); ++it) m[it.key()] = it.value();
    send(m);
}

void NetSession::deleteChannel(int id) {
    QJsonObject m = HProto::msg("chan_delete");
    m["id"] = id;
    send(m);
}

void NetSession::kick(int userId, bool fromServer, const QString& reason) {
    QJsonObject m = HProto::msg("kick");
    m["id"] = userId;
    m["from"] = fromServer ? "server" : "channel";
    m["reason"] = reason;
    send(m);
}

void NetSession::ban(int userId, const QString& reason, int minutes) {
    QJsonObject m = HProto::msg("ban");
    m["id"] = userId;
    m["reason"] = reason;
    m["minutes"] = minutes;
    send(m);
}

void NetSession::usePrivilegeKey(const QString& key) {
    QJsonObject m = HProto::msg("privkey");
    m["key"] = key;
    send(m);
}

void NetSession::quit() {
    send(HProto::msg("quit"));
    m_tcp->flush();
    m_tcp->waitForBytesWritten(300);
    m_tcp->disconnectFromHost();
}

// ================================================== ações v3
void NetSession::avatarSet(const QByteArray& imageBytes) {
    QJsonObject m = HProto::msg("avatar_set");
    m["data"] = QString::fromLatin1(imageBytes.toBase64());
    send(m);
}

void NetSession::avatarGet(const QString& uid) {
    QJsonObject m = HProto::msg("avatar_get");
    m["uid"] = uid;
    send(m);
}

void NetSession::iconGet(const QString& name) {
    QJsonObject m = HProto::msg("icon_get");
    m["name"] = name;
    send(m);
}

void NetSession::iconSet(const QString& name, const QByteArray& bytes) {
    QJsonObject m = HProto::msg("icon_set");
    m["name"] = name;
    m["data"] = QString::fromLatin1(bytes.toBase64());
    send(m);
}

void NetSession::offlineSend(const QString& uid, const QString& text) {
    QJsonObject m = HProto::msg("offline_send");
    m["uid"] = uid;
    m["text"] = text;
    send(m);
}

void NetSession::complaintAdd(int userId, const QString& text) {
    QJsonObject m = HProto::msg("complaint_add");
    m["id"] = userId;
    m["text"] = text;
    send(m);
}

void NetSession::complaintList() { send(HProto::msg("complaint_list")); }

void NetSession::complaintClear(const QString& uid) {
    QJsonObject m = HProto::msg("complaint_clear");
    if (!uid.isEmpty()) m["uid"] = uid;
    send(m);
}

void NetSession::setWhisperIds(const QList<int>& ids) {
    QJsonObject m = HProto::msg("whisper");
    QJsonArray arr;
    for (int id : ids) arr << id;
    m["ids"] = arr;
    send(m);
}

void NetSession::ftUpload(int channel, const QString& name, const QByteArray& data) {
    QJsonObject m = HProto::msg("ft_upload");
    m["channel"] = channel;
    m["name"] = name;
    m["data"] = QString::fromLatin1(data.toBase64());
    send(m);
}

void NetSession::ftList(int channel) {
    QJsonObject m = HProto::msg("ft_list");
    m["channel"] = channel;
    send(m);
}

void NetSession::ftDownload(int channel, const QString& name) {
    QJsonObject m = HProto::msg("ft_download");
    m["channel"] = channel;
    m["name"] = name;
    send(m);
}

void NetSession::ftDelete(int channel, const QString& name) {
    QJsonObject m = HProto::msg("ft_delete");
    m["channel"] = channel;
    m["name"] = name;
    send(m);
}

void NetSession::requestBanList() { send(HProto::msg("banlist")); }

void NetSession::unban(const QString& uid) {
    QJsonObject m = HProto::msg("unban");
    m["uid"] = uid;
    send(m);
}

void NetSession::requestGroupList() { send(HProto::msg("group_list")); }

void NetSession::groupSet(int id, const QString& name, const QJsonObject& perms,
                          const QString& sigla, int order, const QString& icon) {
    QJsonObject m = HProto::msg("group_set");
    if (id > 0) m["id"] = id;
    if (!name.isEmpty()) m["name"] = name;
    if (!perms.isEmpty()) m["perms"] = perms;
    m["sigla"] = sigla;
    m["order"] = order;
    m["icon"] = icon;
    send(m);
}

void NetSession::groupDelete(int id) {
    QJsonObject m = HProto::msg("group_delete");
    m["id"] = id;
    send(m);
}

void NetSession::clientSetGroup(int userId, int gid) {
    QJsonObject m = HProto::msg("client_set_group");
    m["id"] = userId;
    m["gid"] = gid;
    send(m);
}

void NetSession::serverEdit(const QString& name, const QString& motd,
                             const QByteArray& banner, bool bannerChanged) {
    QJsonObject m = HProto::msg("server_edit");
    if (!name.isEmpty()) m["name"] = name;
    m["motd"] = motd;
    if (bannerChanged)
        m["banner"] = QString::fromLatin1(banner.toBase64());
    send(m);
}

// ==================================================================== estado
void NetSession::applyUserJson(const QJsonObject& u) {
    ServerData& d = target();
    User usr;
    usr.id = u["id"].toInt();
    usr.name = u["name"].toString();
    usr.uniqueId = u["uid"].toString();
    usr.version = u["ver"].toString();
    usr.platform = u["platform"].toString();
    usr.description = u["desc"].toString();
    usr.serverGroups = u["group"].toString("normal");
    usr.sigla = u["sigla"].toString();
    usr.groupIcon = u["icon"].toString();
    usr.groupOrder = u["order"].toInt(0);
    usr.inputMuted = u["mic"].toBool();
    usr.outputMuted = u["spk"].toBool();
    usr.away = u["away"].toBool();
    usr.recording = u["rec"].toBool();
    usr.commander = u["cc"].toBool();
    usr.avatarHash = u["av"].toString();               // v3
    usr.op = d.users.value(usr.id).op;                 // preserva flag de operador
    if (usr.id == d.selfId) {
        usr.talking = d.users.value(d.selfId).talking; // preserva estado de fala local ultra responsivo
        usr.whispering = d.users.value(d.selfId).whispering; // preserva estado de sussurro local
    } else {
        usr.talking = u["talking"].toBool();
        usr.whispering = u["whispering"].toBool();
    }
    d.users[usr.id] = usr;
    refreshOperators();                                // recalcula ops por canal
}

void NetSession::applyChanJson(const QJsonObject& c) {
    ServerData& d = target();
    Channel ch;
    ch.id = c["id"].toInt();
    ch.parentId = c["parent"].toInt(0);
    ch.order = c["order"].toInt(0);
    ch.name = c["name"].toString();
    ch.topic = c["topic"].toString();
    ch.description = c["desc"].toString();
    ch.hasPassword = c["pw"].toBool();
    ch.isDefault = c["def"].toBool();
    ch.noSymbol = c.contains("noSymbol")
        ? c["noSymbol"].toBool()
        : d.channels.value(ch.id).noSymbol;
    ch.type = c["type"].toInt(2);
    ch.moderated = c["moderated"].toBool();
    ch.codec = c["codec"].toInt(4);
    ch.codecQuality = c["quality"].toInt(6);
    ch.bitrate = c["bitrate"].toInt(96);
    ch.groupPerms = c["groupPerms"].toObject();
    ch.maxClients = c["max"].toInt(-1);
    ch.linkedChannels.clear();
    for (const QJsonValue& v : c["linked"].toArray()) {
        const int linkedId = v.toInt();
        if (linkedId > 0 && linkedId != ch.id && !ch.linkedChannels.contains(linkedId))
            ch.linkedChannels << linkedId;
    }
    ch.users.clear();
    for (const QJsonValue& v : c["users"].toArray()) ch.users << v.toInt();
    ch.opUids.clear();
    for (const QJsonValue& v : c["ops"].toArray()) ch.opUids << v.toString(); // v3
    d.channels[ch.id] = ch;
    if (ch.id >= d.nextChannelId) d.nextChannelId = ch.id + 1;
    refreshOperators();
}

// v3: marca user.op conforme a lista de operadores (UID) do canal em que ele está
void NetSession::refreshOperators() {
    ServerData& d = target();
    for (User& u : d.users) u.op = false;
    for (const Channel& ch : d.channels)
        for (int uid : ch.users)
            if (d.users.contains(uid)
                    && ch.opUids.contains(d.users[uid].uniqueId))
                d.users[uid].op = true;
}

void NetSession::handleMessage(const QJsonObject& obj) {
    const QString t = obj["t"].toString();
    ServerData& d = target();

    if (t == "error") {
        const QString code = obj["code"].toString();
        const QString msg = obj["msg"].toString();
        AppLog::warn(QStringLiteral("Erro do servidor: %1 (%2)").arg(msg, code));
        if (!m_ready) {
            m_fatalError = true;
            m_data = ServerData();
            emit connectionFailed(msg.isEmpty() ? code : msg);
            return;
        }
        emit errorOccurred(code, msg);
        return;
    }

    if (t == "welcome") {
        d.selfId = obj["selfId"].toInt();
        QJsonObject srv = obj["server"].toObject();
        d.name = srv["name"].toString(d.name);
        d.motd = srv["motd"].toString();
        d.version = srv["ver"].toString();
        d.platform = srv["platform"].toString("Linux");
        d.maxClients = srv["maxClients"].toInt(32);
        d.serverBanner = QByteArray::fromBase64(srv["banner"].toString().toLatin1());

        d.users.clear();
        for (const QJsonValue& v : obj["users"].toArray()) applyUserJson(v.toObject());
        d.channels.clear();
        d.nextChannelId = 1;
        for (const QJsonValue& v : obj["channels"].toArray()) applyChanJson(v.toObject());

        // v3: minhas permissões + lista de grupos do servidor
        m_myPerms = obj["myPerms"].toObject();
        m_groups  = obj["groups"].toArray();

        QJsonObject voice = obj["voice"].toObject();
        m_udpPort = quint16(voice["udp"].toInt());
        m_voiceToken = voice["token"].toString().toUInt();

        m_ready = true;
        m_pingTimer->start();

        // Registra o endpoint UDP com um payload não vazio. Relays antigos
        // ignoravam datagramas HALL de apenas 10 bytes; nesse cenário o PC
        // só passava a ser destino válido depois de falar uma vez.
        if (m_voiceToken && m_udpPort) {
            for (quint16 seq = 1; seq <= 3; ++seq)
                sendVoiceFrame(QByteArray(1, '\0'), seq);
            m_udpRegistrationSeq = 3;
        }

        AppLog::info(QStringLiteral("Conectado a %1 como %2")
                         .arg(m_hostPort, d.users[d.selfId].name));
        emit welcomeReceived();
        emit stateChanged();
        return;
    }

    if (!m_ready) return;

    if (t == "pong") {
        m_pingMs = int(m_pingClock.elapsed());
        emit pingUpdated(m_pingMs);
        return;
    }
    if (t == "chat") {
        emit chatReceived(obj["scope"].toString(), obj["from"].toInt(),
                          obj["fromName"].toString(""), obj["text"].toString());
        return;
    }
    if (t == "user_joined") {
        applyUserJson(obj["user"].toObject());
        emit systemEvent(QStringLiteral("%1 entrou no servidor")
                             .arg(obj["user"].toObject()["name"].toString()));
        emit stateChanged();
        return;
    }
    if (t == "server_edit") {
        // v2: nome/MOTD alterados por um administrador, em tempo real
        const QString newName = obj["name"].toString().trimmed();
        if (!newName.isEmpty() && newName != d.name) {
            d.name = newName;
            emit systemEvent(QStringLiteral("O servidor agora se chama \"%1\"").arg(newName));
        }
        if (obj.contains("motd")) d.motd = obj["motd"].toString();
        if (obj.contains("banner"))
            d.serverBanner = QByteArray::fromBase64(obj["banner"].toString().toLatin1());
        emit stateChanged();
        return;
    }
    if (t == "user_left") {
        const int id = obj["id"].toInt();
        emit systemEvent(QStringLiteral("%1 saiu do servidor")
                             .arg(d.users.value(id).name));
        for (Channel& c : d.channels) c.users.removeAll(id);
        d.users.remove(id);
        emit stateChanged();
        return;
    }
    if (t == "user_moved") {
        const int id = obj["id"].toInt();
        const int chan = obj["channel"].toInt();
        const int old = d.channelOfUser(id);
        if (d.channels.contains(old)) d.channels[old].users.removeAll(id);
        if (d.channels.contains(chan)) d.channels[chan].users << id;
        const QString uname = d.users.value(id).name;
        const QString cname = d.channels.value(chan).name;
        if (id == d.selfId)
            emit systemEvent(QStringLiteral("Você entrou no canal \"%1\"").arg(cname));
        else
            emit systemEvent(QStringLiteral("%1 entrou no canal \"%2\"").arg(uname, cname));
        emit stateChanged();
        return;
    }
    if (t == "user_state" || t == "user_nick" || t == "user_desc" || t == "user_group") {
        const int id = obj["id"].toInt();
        if (d.users.contains(id)) {
            User& u = d.users[id];
            if (obj.contains("mic")) u.inputMuted = obj["mic"].toBool();
            if (obj.contains("spk")) u.outputMuted = obj["spk"].toBool();
            if (obj.contains("away")) u.away = obj["away"].toBool();
            if (obj.contains("rec")) u.recording = obj["rec"].toBool();
            if (obj.contains("cc")) u.commander = obj["cc"].toBool();
            if (obj.contains("talking")) {
                if (id != d.selfId) {
                    u.talking = obj["talking"].toBool();
                }
            }
            if (obj.contains("whispering")) {
                if (id != d.selfId) {
                    u.whispering = obj["whispering"].toBool();
                }
            }
            if (obj.contains("name")) u.name = obj["name"].toString();
            if (obj.contains("text")) u.description = obj["text"].toString();
            if (obj.contains("group")) u.serverGroups = obj["group"].toString();
            if (obj.contains("sigla")) u.sigla = obj["sigla"].toString();
            if (obj.contains("icon")) u.groupIcon = obj["icon"].toString();
            if (obj.contains("order")) u.groupOrder = obj["order"].toInt(0);
        }
        emit stateChanged();
        return;
    }
    if (t == "chan_update") {
        applyChanJson(obj["chan"].toObject());
        emit stateChanged();
        return;
    }
    if (t == "chan_removed") {
        d.channels.remove(obj["id"].toInt());
        emit stateChanged();
        return;
    }
    if (t == "poke") {
        emit pokeReceived(obj["fromName"].toString(""), obj["msg"].toString());
        return;
    }
    if (t == "user_avatar") {
        const int id = obj["id"].toInt();
        if (d.users.contains(id)) d.users[id].avatarHash = obj["av"].toString();
        emit userAvatarChanged(id, obj["av"].toString());
        emit stateChanged();
        return;
    }
    if (t == "avatar_data") {
        emit avatarDataReceived(obj["uid"].toString(),
                                QByteArray::fromBase64(obj["data"].toString().toLatin1()));
        return;
    }
    if (t == "icon_data") {
        emit iconDataReceived(obj["name"].toString(),
                              QByteArray::fromBase64(obj["data"].toString().toLatin1()));
        return;
    }
    if (t == "offline_msg") {
        emit offlineMsgReceived(obj["fromName"].toString(), obj["text"].toString(),
                                obj["ts"].toString());
        return;
    }
    if (t == "offline_sent") {
        emit offlineSendConfirmed(obj["uid"].toString());
        return;
    }
    if (t == "complaint_list") {
        emit complaintListReceived(obj["complaints"].toArray());
        return;
    }
    if (t == "complaint_added" || t == "complaint_cleared") {
        emit systemEvent(t == "complaint_added"
                             ? QStringLiteral("Reclamação registrada")
                             : QStringLiteral("Reclamações limpas"));
        return;
    }
    if (t == "banlist") {
        emit banListReceived(obj["bans"].toArray());
        return;
    }
    if (t == "ban_removed") {
        emit systemEvent(QStringLiteral("Banimento removido"));
        return;
    }
    if (t == "group_list") {
        emit groupListReceived(obj["groups"].toArray());
        return;
    }
    if (t == "ft_list") {
        emit ftListReceived(obj["channel"].toInt(), obj["files"].toArray());
        return;
    }
    if (t == "ft_data") {
        emit ftDataReceived(obj["channel"].toInt(), obj["name"].toString(),
                            QByteArray::fromBase64(obj["data"].toString().toLatin1()));
        return;
    }
    if (t == "ft_uploaded") {
        emit ftUploadConfirmed(obj["channel"].toInt(), obj["name"].toString());
        return;
    }
    if (t == "ft_deleted") {
        emit ftDeleteConfirmed(obj["channel"].toInt(), obj["name"].toString());
        return;
    }
    if (t == "whisper_ok") {
        emit whisperConfirmed(obj["count"].toInt());
        return;
    }
    if (t == "kicked") {
        emit kickedReceived(obj["reason"].toString(), obj["ban"].toBool(),
                            obj["minutes"].toInt(0));
        return;
    }
    if (t == "voice_token") {
        m_udpPort = quint16(obj["udp"].toInt());
        m_voiceToken = obj["token"].toString().toUInt();
        if (m_voiceToken && m_udpPort) {
            for (quint16 seq = 1; seq <= 3; ++seq)
                sendVoiceFrame(QByteArray(1, '\0'), seq);
            m_udpRegistrationSeq = 3;
        }
        return;
    }
}
