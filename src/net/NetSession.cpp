#include "NetSession.h"
#include "HallaProtocol.h"
#include "core/AppLog.h"

#include <QJsonDocument>
#include <QJsonArray>
#include <QNetworkDatagram>

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
    m_port = port;
    m_hostPort = port == 9987 ? host : QStringLiteral("%1:%2").arg(host).arg(port);

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
    m_udp->writeDatagram(HProto::encodeVoiceClient(m_voiceToken, seq, opus),
                         QHostAddress(m_host), m_udpPort);
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
    usr.inputMuted = u["mic"].toBool();
    usr.outputMuted = u["spk"].toBool();
    usr.away = u["away"].toBool();
    usr.recording = u["rec"].toBool();
    usr.commander = u["cc"].toBool();
    usr.talking = u["talking"].toBool();
    d.users[usr.id] = usr;
}

void NetSession::applyChanJson(const QJsonObject& c) {
    ServerData& d = target();
    Channel ch;
    ch.id = c["id"].toInt();
    ch.parentId = c["parent"].toInt(0);
    ch.name = c["name"].toString();
    ch.topic = c["topic"].toString();
    ch.description = c["desc"].toString();
    ch.hasPassword = c["pw"].toBool();
    ch.isDefault = c["def"].toBool();
    ch.type = c["type"].toInt(2);
    ch.moderated = c["moderated"].toBool();
    ch.codec = c["codec"].toInt(4);
    ch.codecQuality = c["quality"].toInt(6);
    ch.maxClients = c["max"].toInt(-1);
    ch.users.clear();
    for (const QJsonValue& v : c["users"].toArray()) ch.users << v.toInt();
    d.channels[ch.id] = ch;
    if (ch.id >= d.nextChannelId) d.nextChannelId = ch.id + 1;
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

        d.users.clear();
        for (const QJsonValue& v : obj["users"].toArray()) applyUserJson(v.toObject());
        d.channels.clear();
        d.nextChannelId = 1;
        for (const QJsonValue& v : obj["channels"].toArray()) applyChanJson(v.toObject());

        QJsonObject voice = obj["voice"].toObject();
        m_udpPort = quint16(voice["udp"].toInt());
        m_voiceToken = voice["token"].toString().toUInt();

        m_ready = true;
        m_pingTimer->start();

        // registra endpoint UDP (payload vazio — servidor aprende nosso endereço)
        if (m_voiceToken && m_udpPort)
            m_udp->writeDatagram(HProto::encodeVoiceClient(m_voiceToken, 1, QByteArray()),
                                 QHostAddress(m_host), m_udpPort);

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
            if (obj.contains("talking")) u.talking = obj["talking"].toBool();
            if (obj.contains("name")) u.name = obj["name"].toString();
            if (obj.contains("text")) u.description = obj["text"].toString();
            if (obj.contains("group")) u.serverGroups = obj["group"].toString();
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
    if (t == "kicked") {
        emit kickedReceived(obj["reason"].toString(), obj["ban"].toBool(),
                            obj["minutes"].toInt(0));
        return;
    }
    if (t == "voice_token") {
        m_udpPort = quint16(obj["udp"].toInt());
        m_voiceToken = obj["token"].toString().toUInt();
        if (m_voiceToken && m_udpPort)
            m_udp->writeDatagram(HProto::encodeVoiceClient(m_voiceToken, 1, QByteArray()),
                                 QHostAddress(m_host), m_udpPort);
        return;
    }
}
