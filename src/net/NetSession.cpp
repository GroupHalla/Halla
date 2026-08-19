#include "version.h"
#include "NetSession.h"
#include "HallaProtocol.h"
#include "core/AppLog.h"
#include "dialogs/IdentityDialog.h"

#include <QJsonDocument>
#include <QJsonArray>
#include <QNetworkDatagram>
#include <QHostInfo>
#include <QSettings>
#include <QCryptographicHash>
#include <QCoreApplication>
#include <QHash>
#include <QMessageBox>
#ifndef HALLA_WEBRTC_NATIVE
#include <openssl/evp.h>
#else
extern "C" {
struct evp_aead_st;
typedef struct evp_aead_st EVP_AEAD;
struct evp_aead_ctx_st;
typedef struct evp_aead_ctx_st EVP_AEAD_CTX;
const EVP_AEAD* EVP_aead_chacha20_poly1305(void);
EVP_AEAD_CTX* EVP_AEAD_CTX_new(const EVP_AEAD* aead, const unsigned char* key,
                               size_t key_len, size_t tag_len);
void EVP_AEAD_CTX_free(EVP_AEAD_CTX* ctx);
int EVP_AEAD_CTX_seal(const EVP_AEAD_CTX* ctx, unsigned char* out, size_t* out_len,
                      size_t max_out_len, const unsigned char* nonce, size_t nonce_len,
                      const unsigned char* in, size_t in_len,
                      const unsigned char* ad, size_t ad_len);
int EVP_AEAD_CTX_open(const EVP_AEAD_CTX* ctx, unsigned char* out, size_t* out_len,
                      size_t max_out_len, const unsigned char* nonce, size_t nonce_len,
                      const unsigned char* in, size_t in_len,
                      const unsigned char* ad, size_t ad_len);
}
#endif

class AeadVoiceCipher {
public:
    static QByteArray encrypt(const QByteArray& plain, const QByteArray& key,
                              quint32 senderId, quint16 seq, quint32 counter) {
#ifdef HALLA_WEBRTC_NATIVE
        if (plain.isEmpty() || key.size() < 32) return plain;
        QByteArray nonce = makeNonce(senderId, counter, seq);
        QByteArray sealed(plain.size() + 16, 0);
        size_t outLen = 0;
        EVP_AEAD_CTX* ctx = EVP_AEAD_CTX_new(EVP_aead_chacha20_poly1305(),
            reinterpret_cast<const unsigned char*>(key.constData()), 32, 16);
        const int ok = ctx && EVP_AEAD_CTX_seal(ctx,
            reinterpret_cast<unsigned char*>(sealed.data()), &outLen, sealed.size(),
            reinterpret_cast<const unsigned char*>(nonce.constData()), nonce.size(),
            reinterpret_cast<const unsigned char*>(plain.constData()), plain.size(),
            nullptr, 0);
        EVP_AEAD_CTX_free(ctx);
        if (!ok || outLen == 0) return QByteArray();
        sealed.resize(int(outLen));
        QByteArray out;
        out.reserve(4 + sealed.size());
        out.append(reinterpret_cast<const char*>(&counter), 4);
        out.append(sealed);
        return out;
#else
        if (plain.isEmpty() || key.size() < 32) return plain;
        QByteArray nonce = makeNonce(senderId, counter, seq);
        QByteArray cipher(plain.size(), 0);
        QByteArray tag(16, 0);
        EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
        int outLen = 0;
        int total = 0;
        bool ok = ctx
            && EVP_EncryptInit_ex(ctx, EVP_chacha20_poly1305(), nullptr, nullptr, nullptr) == 1
            && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN, nonce.size(), nullptr) == 1
            && EVP_EncryptInit_ex(ctx, nullptr, nullptr,
                                  reinterpret_cast<const unsigned char*>(key.constData()),
                                  reinterpret_cast<const unsigned char*>(nonce.constData())) == 1
            && EVP_EncryptUpdate(ctx, reinterpret_cast<unsigned char*>(cipher.data()), &outLen,
                                 reinterpret_cast<const unsigned char*>(plain.constData()), plain.size()) == 1;
        total = outLen;
        if (ok) ok = EVP_EncryptFinal_ex(ctx, reinterpret_cast<unsigned char*>(cipher.data()) + total, &outLen) == 1;
        if (ok) ok = EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_GET_TAG, tag.size(), tag.data()) == 1;
        EVP_CIPHER_CTX_free(ctx);
        if (!ok) return QByteArray();
        QByteArray out;
        out.reserve(4 + cipher.size() + tag.size());
        out.append(reinterpret_cast<const char*>(&counter), 4);
        out.append(cipher);
        out.append(tag);
        return out;
#endif
    }

    static QByteArray decrypt(const QByteArray& packet, const QByteArray& key,
                              quint32 senderId, quint16 seq) {
#ifdef HALLA_WEBRTC_NATIVE
        if (key.size() >= 32 && packet.size() >= 4 + 16) {
            quint32 counter = 0;
            memcpy(&counter, packet.constData(), 4);
            QByteArray nonce = makeNonce(senderId, counter, seq);
            const char* sealed = packet.constData() + 4;
            const int sealedLen = packet.size() - 4;
            QByteArray plain(sealedLen - 16, 0);
            size_t outLen = 0;
            EVP_AEAD_CTX* ctx = EVP_AEAD_CTX_new(EVP_aead_chacha20_poly1305(),
                reinterpret_cast<const unsigned char*>(key.constData()), 32, 16);
            const int ok = ctx && EVP_AEAD_CTX_open(ctx,
                reinterpret_cast<unsigned char*>(plain.data()), &outLen, plain.size(),
                reinterpret_cast<const unsigned char*>(nonce.constData()), nonce.size(),
                reinterpret_cast<const unsigned char*>(sealed), sealedLen,
                nullptr, 0);
            EVP_AEAD_CTX_free(ctx);
            if (ok && outLen > 0) {
                plain.resize(int(outLen));
                return plain;
            }
        }
        // Protocolo v4+: falha de autenticação AEAD é terminal. Nunca tente
        // reinterpretar ciphertext adulterado como o XOR legado.
        return QByteArray();
#else
        if (key.size() < 32) return packet;
        if (packet.size() < 4 + 16) return QByteArray();
        quint32 counter = 0;
        memcpy(&counter, packet.constData(), 4);
        const int cipherLen = packet.size() - 4 - 16;
        QByteArray nonce = makeNonce(senderId, counter, seq);
        QByteArray plain(cipherLen, 0);
        const char* cipher = packet.constData() + 4;
        const char* tag = packet.constData() + 4 + cipherLen;
        EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
        int outLen = 0;
        int total = 0;
        bool ok = ctx
            && EVP_DecryptInit_ex(ctx, EVP_chacha20_poly1305(), nullptr, nullptr, nullptr) == 1
            && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN, nonce.size(), nullptr) == 1
            && EVP_DecryptInit_ex(ctx, nullptr, nullptr,
                                  reinterpret_cast<const unsigned char*>(key.constData()),
                                  reinterpret_cast<const unsigned char*>(nonce.constData())) == 1
            && EVP_DecryptUpdate(ctx, reinterpret_cast<unsigned char*>(plain.data()), &outLen,
                                 reinterpret_cast<const unsigned char*>(cipher), cipherLen) == 1;
        total = outLen;
        if (ok) ok = EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_TAG, 16, const_cast<char*>(tag)) == 1;
        if (ok) ok = EVP_DecryptFinal_ex(ctx, reinterpret_cast<unsigned char*>(plain.data()) + total, &outLen) == 1;
        EVP_CIPHER_CTX_free(ctx);
        return ok ? plain : QByteArray();
#endif
    }

private:
    static QByteArray makeNonce(quint32 senderId, quint32 counter, quint16 seq) {
        QByteArray n(12, '\0');
        memcpy(n.data(), &senderId, 4);
        memcpy(n.data() + 4, &counter, 4);
        memcpy(n.data() + 8, &seq, 2);
        return n;
    }

    static QByteArray legacyXor(const QByteArray& data, const QByteArray& key, quint16 seq) {
        if (data.isEmpty() || key.size() < 16) return data;
        QByteArray output = data;
        quint32 state[4];
        memcpy(state, key.constData(), 16);
        state[0] ^= seq;
        state[1] ^= (quint32(seq) << 16);
        state[2] ^= 0xDEADBEEF;
        state[3] ^= 0xCAFEBABE;
        auto rotl = [](quint32 x, int k) -> quint32 { return (x << k) | (x >> (32 - k)); };
        auto next = [&]() -> quint32 {
            const quint32 result = rotl(state[0] + state[3], 7) * 9;
            const quint32 t = state[1] << 9;
            state[2] ^= state[0]; state[3] ^= state[1]; state[1] ^= state[2]; state[0] ^= state[3];
            state[2] ^= t; state[3] = rotl(state[3], 11);
            return result;
        };
        for (int i = 0; i < output.size(); i += 4) {
            quint32 ks = next();
            const int limit = qMin(4, output.size() - i);
            for (int j = 0; j < limit; ++j) output[i + j] ^= reinterpret_cast<char*>(&ks)[j];
        }
        return output;
    }
};

static QHostAddress normalizedPeerAddress(QHostAddress address) {
    if (address.protocol() == QAbstractSocket::IPv6Protocol) {
        bool ok = false;
        const quint32 ipv4 = address.toIPv4Address(&ok);
        if (ok) return QHostAddress(ipv4);
    }
    return address;
}

static QString localizedServerError(const QString& code, const QString& serverText) {
    static const QHash<QString, const char*> messages = {
        { QStringLiteral("rate_limited"), QT_TRANSLATE_NOOP("ServerErrors", "Você está enviando solicitações rápido demais.") },
        { QStringLiteral("screenshare_disabled"), QT_TRANSLATE_NOOP("ServerErrors", "O compartilhamento de tela está desativado pelo servidor.") },
        { QStringLiteral("webrtc_target"), QT_TRANSLATE_NOOP("ServerErrors", "O destino da transmissão é inválido.") },
        { QStringLiteral("webrtc_channel"), QT_TRANSLATE_NOOP("ServerErrors", "A transmissão é permitida apenas entre usuários do mesmo canal.") },
        { QStringLiteral("webrtc_not_streaming"), QT_TRANSLATE_NOOP("ServerErrors", "O usuário selecionado não está transmitindo.") },
        { QStringLiteral("webrtc_sdp_too_big"), QT_TRANSLATE_NOOP("ServerErrors", "Os dados da transmissão excedem o limite permitido.") },
        { QStringLiteral("webrtc_ice_too_big"), QT_TRANSLATE_NOOP("ServerErrors", "Os dados de conexão da transmissão excedem o limite permitido.") },
        { QStringLiteral("bad_identity"), QT_TRANSLATE_NOOP("ServerErrors", "A identidade criptográfica é inválida ou está ausente.") },
        { QStringLiteral("bad_uid"), QT_TRANSLATE_NOOP("ServerErrors", "A identidade única está ausente.") },
        { QStringLiteral("bad_nick"), QT_TRANSLATE_NOOP("ServerErrors", "O apelido informado é inválido.") },
        { QStringLiteral("name_in_use"), QT_TRANSLATE_NOOP("ServerErrors", "Este apelido já está em uso.") },
        { QStringLiteral("server_full"), QT_TRANSLATE_NOOP("ServerErrors", "O servidor está cheio.") },
        { QStringLiteral("bad_password"), QT_TRANSLATE_NOOP("ServerErrors", "A senha do servidor está incorreta.") },
        { QStringLiteral("bad_channel_pass"), QT_TRANSLATE_NOOP("ServerErrors", "A senha do canal está incorreta.") },
        { QStringLiteral("no_permission"), QT_TRANSLATE_NOOP("ServerErrors", "Você não tem permissão para realizar esta ação.") },
        { QStringLiteral("hierarchy"), QT_TRANSLATE_NOOP("ServerErrors", "A hierarquia de cargos não permite esta ação.") },
        { QStringLiteral("locked"), QT_TRANSLATE_NOOP("ServerErrors", "Este item está protegido e não pode ser alterado.") },
        { QStringLiteral("no_talk_power"), QT_TRANSLATE_NOOP("ServerErrors", "Você não tem poder de fala suficiente neste canal.") },
        { QStringLiteral("invalid_channel"), QT_TRANSLATE_NOOP("ServerErrors", "O canal selecionado é inválido.") },
        { QStringLiteral("invalid_parent"), QT_TRANSLATE_NOOP("ServerErrors", "Um canal não pode ser colocado dentro da própria árvore.") },
        { QStringLiteral("invalid_channels"), QT_TRANSLATE_NOOP("ServerErrors", "A seleção de canais é inválida.") },
        { QStringLiteral("has_children"), QT_TRANSLATE_NOOP("ServerErrors", "Exclua primeiro os subcanais.") },
        { QStringLiteral("bad_scope"), QT_TRANSLATE_NOOP("ServerErrors", "O destino da mensagem é inválido.") },
        { QStringLiteral("bad_text"), QT_TRANSLATE_NOOP("ServerErrors", "O texto informado é inválido.") },
        { QStringLiteral("bad_channel_name"), QT_TRANSLATE_NOOP("ServerErrors", "O nome do canal é inválido.") },
        { QStringLiteral("bad_channel_fields"), QT_TRANSLATE_NOOP("ServerErrors", "Uma ou mais propriedades do canal são inválidas.") },
        { QStringLiteral("crypto_error"), QT_TRANSLATE_NOOP("ServerErrors", "Não foi possível proteger os dados confidenciais.") },
        { QStringLiteral("not_found"), QT_TRANSLATE_NOOP("ServerErrors", "O item solicitado não foi encontrado.") },
        { QStringLiteral("bad_privkey"), QT_TRANSLATE_NOOP("ServerErrors", "A chave de privilégio é inválida.") },
        { QStringLiteral("privkey_used"), QT_TRANSLATE_NOOP("ServerErrors", "Esta chave de privilégio já foi utilizada.") },
        { QStringLiteral("group_exists"), QT_TRANSLATE_NOOP("ServerErrors", "Já existe um cargo com este nome.") },
        { QStringLiteral("bad_group"), QT_TRANSLATE_NOOP("ServerErrors", "Os dados do cargo são inválidos.") },
        { QStringLiteral("bad_group_operation"), QT_TRANSLATE_NOOP("ServerErrors", "A operação de cargo é inválida.") },
        { QStringLiteral("invalid_banner"), QT_TRANSLATE_NOOP("ServerErrors", "A imagem do banner é inválida.") },
        { QStringLiteral("banner_too_big"), QT_TRANSLATE_NOOP("ServerErrors", "A imagem do banner excede o limite permitido.") },
        { QStringLiteral("avatar_too_big"), QT_TRANSLATE_NOOP("ServerErrors", "O avatar excede o limite permitido.") },
        { QStringLiteral("icon_too_big"), QT_TRANSLATE_NOOP("ServerErrors", "O ícone excede o limite permitido.") },
        { QStringLiteral("file_too_big"), QT_TRANSLATE_NOOP("ServerErrors", "O arquivo excede o limite permitido.") },
        { QStringLiteral("plugin_data_unsupported"), QT_TRANSLATE_NOOP("ServerErrors", "Este servidor não oferece transporte de dados para complementos.") },
        { QStringLiteral("bad_plugin_data"), QT_TRANSLATE_NOOP("ServerErrors", "Os dados enviados pelo complemento são inválidos.") },
        { QStringLiteral("plugin_data_too_big"), QT_TRANSLATE_NOOP("ServerErrors", "Os dados enviados pelo complemento excedem o limite permitido.") },
        { QStringLiteral("plugin_data_scope"), QT_TRANSLATE_NOOP("ServerErrors", "Dados de complementos só podem ser enviados a usuários do mesmo canal.") },
        { QStringLiteral("quota"), QT_TRANSLATE_NOOP("ServerErrors", "A cota de arquivos do canal foi excedida.") },
        { QStringLiteral("inbox_full"), QT_TRANSLATE_NOOP("ServerErrors", "A caixa de entrada do usuário está cheia.") },
        { QStringLiteral("io_error"), QT_TRANSLATE_NOOP("ServerErrors", "O servidor não conseguiu salvar os dados.") },
    };
    if (const char* source = messages.value(code, nullptr))
        return QCoreApplication::translate("ServerErrors", source);
    const QString genericSource = QStringLiteral("O servidor recusou a solicitação (%1).");
    const QString generic = QCoreApplication::translate(
        "ServerErrors", "O servidor recusou a solicitação (%1).");
    if (generic == genericSource && !serverText.isEmpty()) return serverText;
    return generic.arg(code);
}

NetSession::NetSession(QObject* parent) : QObject(parent) {
    m_tcp = new QSslSocket(this);
    connect(m_tcp, &QTcpSocket::connected, this, &NetSession::onConnected);
    connect(m_tcp, &QTcpSocket::readyRead, this, &NetSession::onReadyRead);
    connect(m_tcp, &QTcpSocket::disconnected, this, &NetSession::onDisconnected);
    connect(m_tcp, &QSslSocket::sslErrors, this, &NetSession::onSslErrors);

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
    m_identityUid = uid;
    m_udpPort = 0;
    m_voiceToken.clear();
    m_udpRegistrationSeq = 0;
    m_buffer.clear();
    m_channelKeys.clear();
    m_reassembly.clear();
    m_cryptoCounter = 0;
    m_intentionalDisconnect = false;
    m_serverTerminatedSession = false;

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
    const QByteArray publicKey = IdentityDialog::publicKeyForUid(uid);
    if (!publicKey.isEmpty())
        m_pendingHello["idPub"] = QString::fromLatin1(publicKey.toBase64());
    m_pendingHello["nick"] = nickname;
    m_pendingHello["ver"] = QString::fromUtf8(halla::kAppVersion);
    m_pendingHello["platform"] =
#ifdef Q_OS_WIN
        "Windows";
#else
        "Linux";
#endif
    if (!password.isEmpty()) m_pendingHello["pass"] = password;
    if (!adminPassword.isEmpty()) m_pendingHello["adminPass"] = adminPassword;

    m_tcp->connectToHostEncrypted(host, port);
}

void NetSession::onSslErrors(const QList<QSslError>& errors) {
    const QSslCertificate cert = m_tcp->peerCertificate();
    const QByteArray fingerprint = cert.digest(QCryptographicHash::Sha256).toHex();
    if (cert.isNull() || fingerprint.isEmpty()) {
        emit connectionFailed(tr("O servidor não apresentou um certificado TLS válido."));
        m_tcp->abort();
        return;
    }

    QSettings settings;
    const QString key = QStringLiteral("ssl/fingerprint_%1_%2").arg(m_host).arg(m_port);
    const QByteArray saved = settings.value(key).toByteArray();
    if (!saved.isEmpty()) {
        if (saved == fingerprint) {
            m_tcp->ignoreSslErrors(errors);
            return;
        }
        emit connectionFailed(tr("ALERTA DE SEGURANÇA: A impressão digital TLS deste servidor mudou!\n"
                                 "Isso pode indicar um ataque Man-in-the-Middle (MITM).\n"
                                 "Conexão recusada para sua proteção."));
        m_tcp->abort();
        return;
    }

    QStringList details;
    for (const QSslError& error : errors) details << error.errorString();
    const QString formatted = QString::fromLatin1(fingerprint).toUpper();
    const auto answer = QMessageBox::warning(
        nullptr, tr("Confirmar certificado do servidor"),
        tr("Este é o primeiro contato com %1:%2 e o certificado não pôde ser validado pela autoridade do sistema.\n\n"
           "SHA-256:\n%3\n\nErros: %4\n\n"
           "Compare esta impressão digital com a publicada pelo administrador. Confiar e fixar este certificado?")
            .arg(m_host).arg(m_port).arg(formatted, details.join(QStringLiteral("; "))),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes) {
        emit connectionFailed(tr("Certificado TLS não confirmado pelo usuário."));
        m_tcp->abort();
        return;
    }
    settings.setValue(key, fingerprint);
    settings.sync();
    m_tcp->ignoreSslErrors(errors);
}

void NetSession::onConnected() {
    // Use o endpoint realmente escolhido pelo TCP/TLS também para UDP. Uma
    // resolução DNS independente podia selecionar IPv6 enquanto a conexão real
    // usava IPv4 (ou endereço IPv4-mapeado), fazendo o cliente descartar toda a
    // voz recebida — observado principalmente com remetentes Mobile.
    const QHostAddress peer = normalizedPeerAddress(m_tcp->peerAddress());
    if (!peer.isNull()) m_udpHostAddress = peer;
    send(m_pendingHello);
}

void NetSession::send(const QJsonObject& obj) {
    if (m_tcp->state() == QAbstractSocket::ConnectedState)
        m_tcp->write(QJsonDocument(obj).toJson(QJsonDocument::Compact) + '\n');
}

void NetSession::onReadyRead() {
    static constexpr qsizetype kMaxTcpMessageBytes = 2 * 1024 * 1024;
    m_buffer += m_tcp->readAll();
    if (m_buffer.size() > kMaxTcpMessageBytes) {
        m_fatalError = true;
        emit connectionFailed(tr("O servidor enviou uma mensagem acima do limite de 2 MiB."));
        m_tcp->abort();
        return;
    }
    int idx;
    while ((idx = m_buffer.indexOf('\n')) >= 0) {
        if (idx > kMaxTcpMessageBytes) {
            m_fatalError = true;
            emit connectionFailed(tr("O servidor enviou uma linha acima do limite de 2 MiB."));
            m_tcp->abort();
            return;
        }
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
    m_voiceToken.clear();
    m_udpRegistrationSeq = 0;
    if (!m_intentionalDisconnect) {
        if (!m_ready && !m_fatalError)
            emit connectionFailed(tr("Não foi possível conectar ao servidor"));
        else if (m_ready)
            emit disconnectedUnexpected();
    }
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
    if (!m_voiceToken.isEmpty() && m_udpPort) {
        sendVoiceFrame(QByteArray(1, '\0'), ++m_udpRegistrationSeq);
    }
}

void NetSession::onUdpReadyRead() {
    while (m_udp->hasPendingDatagrams()) {
        QNetworkDatagram dg = m_udp->receiveDatagram();
        // O relay oficial é o único emissor UDP aceito. Sem esta verificação,
        // qualquer host que descobrisse a porta local poderia injetar HALL/HALF.
        if (dg.senderPort() != m_udpPort || (!m_udpHostAddress.isNull()
                && normalizedPeerAddress(dg.senderAddress())
                    != normalizedPeerAddress(m_udpHostAddress))) continue;
        QByteArray data = dg.data();
        if (data.size() < 10) continue;
        const bool isVoice = memcmp(data.constData(), "HALL", 4) == 0;
        const bool isScreenShare = memcmp(data.constData(), "HALF", 4) == 0;
        if (!isVoice && !isScreenShare) continue;
        
        if (isVoice) {
            quint32 fromId;
            quint16 seq;
            memcpy(&fromId, data.constData() + 4, 4);
            memcpy(&seq, data.constData() + 8, 2);
            
            const QByteArray encryptedPayload = data.mid(10);
            QByteArray payload = encryptedPayload;
            const int chanId = m_target ? m_target->channelOfUser(int(fromId)) : 0;
            if (!m_channelKeys.isEmpty()) {
                payload.clear();
                QList<QByteArray> candidates;
                if (chanId > 0 && m_channelKeys.contains(chanId))
                    candidates << m_channelKeys.value(chanId);
                for (const QByteArray& key : m_channelKeys)
                    if (!key.isEmpty() && !candidates.contains(key)) candidates << key;
                for (const QByteArray& key : candidates) {
                    payload = AeadVoiceCipher::decrypt(encryptedPayload, key, fromId, seq);
                    if (!payload.isEmpty()) break;
                }
                if (payload.isEmpty()) continue;
            }

            emit voicePacketReceived(int(fromId), seq, payload);
        } else {
            quint32 fromId;
            quint16 seq;
            memcpy(&fromId, data.constData() + 4, 4);
            memcpy(&seq, data.constData() + 8, 2);

            if (data.size() < 12) continue;
            const quint8 chunkIdx = quint8(data[10]);
            const quint8 chunkCount = quint8(data[11]);
            if (chunkCount == 0 || chunkCount > 64 || chunkIdx >= chunkCount) continue;
            const QByteArray encryptedChunk = data.mid(12);
            QByteArray chunkPayload = encryptedChunk;

            const int uid = int(fromId);
            const int chanId = m_target ? m_target->channelOfUser(uid) : 0;
            if (!m_channelKeys.isEmpty()) {
                chunkPayload.clear();
                QList<QByteArray> candidates;
                if (chanId > 0 && m_channelKeys.contains(chanId))
                    candidates << m_channelKeys.value(chanId);
                for (const QByteArray& key : m_channelKeys)
                    if (!key.isEmpty() && !candidates.contains(key)) candidates << key;
                for (const QByteArray& key : candidates) {
                    chunkPayload = AeadVoiceCipher::decrypt(encryptedChunk, key, fromId, seq);
                    if (!chunkPayload.isEmpty()) break;
                }
                if (chunkPayload.isEmpty()) continue;
            }

            int pendingFrames = 0;
            for (auto userIt = m_reassembly.cbegin(); userIt != m_reassembly.cend(); ++userIt)
                pendingFrames += userIt.value().size();
            if (!m_reassembly[uid].contains(seq) && pendingFrames >= 128) continue;

            auto& frame = m_reassembly[uid][seq];
            qsizetype existingBytes = 0;
            for (const QByteArray& part : frame) existingBytes += part.size();
            if (existingBytes + chunkPayload.size() > 4 * 1024 * 1024) {
                m_reassembly[uid].remove(seq);
                if (m_reassembly[uid].isEmpty()) m_reassembly.remove(uid);
                continue;
            }
            frame[chunkIdx] = chunkPayload;

            if (frame.size() == chunkCount) {
                QByteArray combined;
                for (int i = 0; i < chunkCount; ++i) {
                    if (!frame.contains(i)) { combined.clear(); break; }
                    combined.append(frame[i]);
                }
                if (!combined.isEmpty()) emit screenshareFrameReceived(uid, combined);

                QList<quint16> seqs = m_reassembly[uid].keys();
                for (quint16 s : seqs) {
                    if (s < seq || (seq < 100 && s > 64000)) {
                        m_reassembly[uid].remove(s);
                    }
                }
            }
        }
    }
}

void NetSession::sendVoiceFrame(const QByteArray& opus, quint16 seq) {
    if (m_voiceToken.isEmpty() || m_udpPort == 0) return;
    const QHostAddress destination = m_udpHostAddress.isNull()
        ? QHostAddress(m_host) : m_udpHostAddress;
    if (destination.isNull()) return;
    
    QByteArray encryptedOpus = opus;
    if (m_target) {
        int chanId = m_target->channelOfUser(m_target->selfId);
        if (chanId > 0 && m_channelKeys.contains(chanId)) {
            encryptedOpus = AeadVoiceCipher::encrypt(opus, m_channelKeys[chanId],
                                                     quint32(m_target->selfId), seq, ++m_cryptoCounter);
            if (encryptedOpus.isEmpty()) return;
        }
    }
    
    m_udp->writeDatagram(HProto::encodeVoiceClient(m_voiceToken, seq, encryptedOpus),
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
    if (m_intentionalDisconnect) return;
    m_intentionalDisconnect = true;
    send(HProto::msg("quit"));
    m_tcp->flush();
    // Nunca bloqueie a thread da interface esperando o socket. O pequeno
    // atraso permite que a mensagem quit saia e evita reentrância/crash no
    // fechamento da aba.
    QTimer::singleShot(250, m_tcp, [socket = m_tcp] {
        if (socket->state() != QAbstractSocket::UnconnectedState)
            socket->disconnectFromHost();
    });
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
                          const QString& sigla, int order, const QString& icon, int position,
                          bool siglaAfter, bool orderEnabled) {
    QJsonObject m = HProto::msg("group_set");
    if (id > 0) m["id"] = id;
    if (!name.isEmpty()) m["name"] = name;
    if (!perms.isEmpty()) m["perms"] = perms;
    m["sigla"] = sigla;
    m["siglaAfter"] = siglaAfter;
    m["order"] = order;
    m["orderEnabled"] = orderEnabled;
    m["icon"] = icon;
    if (position >= 0) m["position"] = position;  // Pilar 1: position hierárquica
    send(m);
}

void NetSession::groupDelete(int id) {
    QJsonObject m = HProto::msg("group_delete");
    m["id"] = id;
    send(m);
}

void NetSession::clientSetGroup(int userId, int gid, bool remove) {
    QJsonObject m = HProto::msg("client_set_group");
    m["id"] = userId;
    m["gid"] = gid;
    m["op"] = remove ? QStringLiteral("remove") : QStringLiteral("add");
    send(m);
}

void NetSession::clientSetGroupUid(const QString& uid, int gid, bool remove) {
    QJsonObject m = HProto::msg("client_set_group");
    m["uid"] = uid;
    m["gid"] = gid;
    m["op"] = remove ? QStringLiteral("remove") : QStringLiteral("add");
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
    usr.siglaSuffix = u["siglaSuffix"].toString();
    usr.groupIcon = u["icon"].toString();
    usr.groupOrder = u["order"].toInt(0);
    usr.groupOrderEnabled = u["orderEnabled"].toBool(true);
    usr.groupId = u["gid"].toInt(0);
    usr.groupPosition = u["position"].toInt(0);  // Pilar 1: posição hierárquica
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
    ch.tempChannelParent = c.contains("tempParent")
        ? c["tempParent"].toBool()
        : d.channels.value(ch.id).tempChannelParent;
    ch.type = c["type"].toInt(2);
    ch.moderated = c["moderated"].toBool();
    ch.codec = c["codec"].toInt(4);
    ch.codecQuality = c["quality"].toInt(6);
    ch.bitrate = c["bitrate"].toInt(96);
    ch.groupPerms = c["groupPerms"].toObject();  // Pilar 3: Allow/Deny/Inherit
    ch.groupPositionReqs = c["groupPositionReqs"].toObject();  // Pilar 1: requisitos de position
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

    if (t == "identity_challenge") {
        const QByteArray nonce = QByteArray::fromBase64(obj["nonce"].toString().toLatin1());
        const QByteArray sig = IdentityDialog::signNonce(m_identityUid, nonce);
        if (sig.isEmpty()) {
            emit connectionFailed(tr("Não foi possível assinar o desafio da identidade"));
            m_tcp->abort();
            return;
        }
        QJsonObject proof = HProto::msg("identity_proof");
        proof["sig"] = QString::fromLatin1(sig.toBase64());
        send(proof);
        return;
    }

    if (t == "error") {
        const QString code = obj["code"].toString();
        const QString msg = localizedServerError(code, obj["msg"].toString());
        AppLog::warn(tr("Erro do servidor: %1 (%2)").arg(msg, code));
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
        m_allowScreenShare = srv["screenshare"].toBool(true);
        m_screenshareWidth = srv["screenshare_w"].toInt(800);
        m_screenshareHeight = srv["screenshare_h"].toInt(450);
        m_screenshareFps = srv["screenshare_fps"].toInt(20);
        m_webRtcIceServers = obj["iceServers"].toArray();
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
        m_voiceToken = QByteArray::fromHex(voice["token"].toString().toLatin1());
        if (m_voiceToken.size() != HProto::kVoiceTokenBytes) m_voiceToken.clear();

        // Recebe as chaves de canal junto do welcome para evitar a corrida em
        // que channel_key chegava antes de m_ready e era ignorado.
        const QJsonObject keys = obj["channelKeys"].toObject();
        for (auto it = keys.begin(); it != keys.end(); ++it) {
            bool ok = false;
            const int channelId = it.key().toInt(&ok);
            if (ok && channelId > 0)
                m_channelKeys[channelId] = QByteArray::fromBase64(it.value().toString().toLatin1());
        }

        m_ready = true;
        m_pingTimer->start();

        // Registra o endpoint UDP com um payload não vazio. Relays antigos
        // ignoravam datagramas HALL de apenas 10 bytes; nesse cenário o PC
        // só passava a ser destino válido depois de falar uma vez.
        if (!m_voiceToken.isEmpty() && m_udpPort) {
            for (quint16 seq = 1; seq <= 3; ++seq)
                sendVoiceFrame(QByteArray(1, '\0'), seq);
            m_udpRegistrationSeq = 3;
        }

        AppLog::info(tr("Conectado a %1 como %2")
                         .arg(m_hostPort, d.users[d.selfId].name));
        emit welcomeReceived();
        emit stateChanged();
        return;
    }

    if (t == "channel_key") {
        m_channelKeys[obj["channel"].toInt()] = QByteArray::fromBase64(obj["key"].toString().toLatin1());
        return;
    }

    if (!m_ready) return;

    if (t == "privilege_granted") {
        const QJsonObject effective = obj["myPerms"].toObject();
        if (!effective.isEmpty()) m_myPerms = effective;
        if (obj["individual"].toBool(false))
            m_myPerms[QStringLiteral("*")] = true;
        // Atualiza imediatamente menus e diálogos que dependem das permissões,
        // sem exigir reconexão depois de usar uma privilege key.
        emit stateChanged();
        return;
    }

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
        emit systemEvent(tr("%1 entrou no servidor")
                             .arg(obj["user"].toObject()["name"].toString()));
        emit stateChanged();
        return;
    }
    if (t == "server_edit") {
        // v2: nome/MOTD alterados por um administrador, em tempo real
        const QString newName = obj["name"].toString().trimmed();
        if (!newName.isEmpty() && newName != d.name) {
            d.name = newName;
            emit systemEvent(tr("O servidor agora se chama \"%1\"").arg(newName));
        }
        if (obj.contains("motd")) d.motd = obj["motd"].toString();
        if (obj.contains("banner"))
            d.serverBanner = QByteArray::fromBase64(obj["banner"].toString().toLatin1());
        emit stateChanged();
        return;
    }
    if (t == "user_left") {
        const int id = obj["id"].toInt();
        emit systemEvent(tr("%1 saiu do servidor")
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
            emit systemEvent(tr("Você entrou no canal \"%1\"").arg(cname));
        else
            emit systemEvent(tr("%1 entrou no canal \"%2\"").arg(uname, cname));
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
            if (obj.contains("siglaSuffix")) u.siglaSuffix = obj["siglaSuffix"].toString();
            if (obj.contains("icon")) u.groupIcon = obj["icon"].toString();
            if (obj.contains("order")) u.groupOrder = obj["order"].toInt(0);
            if (obj.contains("orderEnabled")) u.groupOrderEnabled = obj["orderEnabled"].toBool(true);
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
        emit pokeReceived(obj["from"].toInt(), obj["fromName"].toString(""),
                          obj["msg"].toString());
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
                             ? tr("Reclamação registrada")
                             : tr("Reclamações limpas"));
        return;
    }
    if (t == "banlist") {
        emit banListReceived(obj["bans"].toArray());
        return;
    }
    if (t == "ban_removed") {
        emit systemEvent(tr("Banimento removido"));
        return;
    }
    if (t == "group_list") {
        m_groups = obj["groups"].toArray();
        emit groupListReceived(m_groups);
        return;
    }
    if (t == "group_set_ok") {
        const QJsonObject confirmed = obj["group"].toObject();
        const int confirmedId = confirmed["id"].toInt();
        bool replaced = false;
        for (int i = 0; i < m_groups.size(); ++i) {
            QJsonObject cached = m_groups.at(i).toObject();
            if (cached["id"].toInt() != confirmedId) continue;
            for (auto it = confirmed.begin(); it != confirmed.end(); ++it)
                cached[it.key()] = it.value();
            m_groups[i] = cached; // preserva membros, ausentes na confirmação
            replaced = true;
            break;
        }
        if (!replaced && confirmedId > 0) m_groups << confirmed;
        emit groupSetConfirmed(confirmed);
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
    if (t == "user_screenshare_state") {
        const int id = obj["id"].toInt();
        const bool on = obj["on"].toBool();
        if (d.users.contains(id)) {
            d.users[id].screensharing = on;
        }
        emit screenshareStateChanged(id, on);
        emit stateChanged();
        return;
    }
    if (t == "webrtc_watch_request" || t == "webrtc_watch_stop" ||
        t == "webrtc_offer" || t == "webrtc_answer" || t == "webrtc_ice") {
        if (obj.value(QStringLiteral("iceServers")).isArray())
            m_webRtcIceServers = obj.value(QStringLiteral("iceServers")).toArray();
        emit webRtcSignalReceived(obj);
        return;
    }
    if (t == "plugin_data") {
        const QString pluginId = obj["plugin"].toString();
        const QString topic = obj["topic"].toString();
        const QByteArray data = QByteArray::fromBase64(obj["data"].toString().toLatin1());
        if (!pluginId.isEmpty() && topic.toUtf8().size() <= 64 && data.size() <= 8192)
            emit pluginDataReceived(obj["from"].toInt(), pluginId, topic, data);
        return;
    }
    if (t == "kicked") {
        m_serverTerminatedSession = true;
        emit kickedReceived(obj["reason"].toString(), obj["ban"].toBool(),
                            obj["minutes"].toInt(0));
        return;
    }
    if (t == "voice_token") {
        m_udpPort = quint16(obj["udp"].toInt());
        m_voiceToken = QByteArray::fromHex(obj["token"].toString().toLatin1());
        if (m_voiceToken.size() != HProto::kVoiceTokenBytes) m_voiceToken.clear();
        if (!m_voiceToken.isEmpty() && m_udpPort) {
            for (quint16 seq = 1; seq <= 3; ++seq)
                sendVoiceFrame(QByteArray(1, '\0'), seq);
            m_udpRegistrationSeq = 3;
        }
        return;
    }
}

void NetSession::sendWebRtcStreamStart() {
    send(HProto::msg("webrtc_stream_start"));
}

void NetSession::sendWebRtcStreamStop() {
    send(HProto::msg("webrtc_stream_stop"));
}

void NetSession::sendWebRtcWatchRequest(int userId) {
    QJsonObject m = HProto::msg("webrtc_watch_request");
    m["to"] = userId;
    send(m);
}

void NetSession::sendWebRtcWatchStop(int userId) {
    QJsonObject m = HProto::msg("webrtc_watch_stop");
    m["to"] = userId;
    send(m);
}

void NetSession::sendWebRtcOffer(int toUserId, const QString& sdp) {
    QJsonObject m = HProto::msg("webrtc_offer");
    m["to"] = toUserId;
    m["sdp"] = sdp;
    send(m);
}

void NetSession::sendWebRtcAnswer(int toUserId, const QString& sdp) {
    QJsonObject m = HProto::msg("webrtc_answer");
    m["to"] = toUserId;
    m["sdp"] = sdp;
    send(m);
}

void NetSession::sendWebRtcIce(int toUserId, const QString& candidate,
                               const QString& sdpMid, int sdpMLineIndex) {
    QJsonObject m = HProto::msg("webrtc_ice");
    m["to"] = toUserId;
    m["candidate"] = candidate;
    if (!sdpMid.isEmpty()) m["sdpMid"] = sdpMid;
    if (sdpMLineIndex >= 0) m["sdpMLineIndex"] = sdpMLineIndex;
    send(m);
}

bool NetSession::sendPluginData(const QString& pluginId, int target,
                                const QList<int>& targetUserIds,
                                const QString& topic, const QByteArray& data) {
    if (!m_ready || pluginId.isEmpty() || pluginId.size() > 64
            || topic.toUtf8().size() > 64 || data.size() > 8192
            || target < 0 || target > 2 || targetUserIds.size() > 64)
        return false;
    QJsonObject message = HProto::msg("plugin_data");
    message["plugin"] = pluginId;
    message["target"] = target;
    message["topic"] = topic;
    message["data"] = QString::fromLatin1(data.toBase64());
    if (target == 1) {
        QJsonArray ids;
        for (int id : targetUserIds) if (id > 0) ids << id;
        if (ids.isEmpty()) return false;
        message["ids"] = ids;
    }
    send(message);
    return true;
}

void NetSession::sendScreenShareStart() {
    send(HProto::msg("screenshare_start"));
}

void NetSession::sendScreenShareStop() {
    send(HProto::msg("screenshare_stop"));
}

void NetSession::sendScreenShareFrame(const QByteArray& jpeg, quint16 seq) {
    if (!m_ready || m_voiceToken.isEmpty() || !m_udpPort || jpeg.isEmpty()) return;
    const QHostAddress destination = m_udpHostAddress.isNull()
        ? QHostAddress(m_host) : m_udpHostAddress;
    if (destination.isNull()) return;

    const int maxChunkSize = 1200;
    const int totalChunks = (jpeg.size() + maxChunkSize - 1) / maxChunkSize;
    if (totalChunks > 255) return;

    int chanId = m_target ? m_target->channelOfUser(m_target->selfId) : 0;

    for (int i = 0; i < totalChunks; ++i) {
        int offset = i * maxChunkSize;
        int size = qMin(maxChunkSize, jpeg.size() - offset);
        QByteArray chunkData = jpeg.mid(offset, size);

        if (chanId > 0 && m_channelKeys.contains(chanId)) {
            chunkData = AeadVoiceCipher::encrypt(chunkData, m_channelKeys[chanId],
                                                 quint32(m_target ? m_target->selfId : 0), seq, ++m_cryptoCounter);
            if (chunkData.isEmpty()) continue;
        }

        QByteArray framed;
        quint8 idx = quint8(i);
        quint8 count = quint8(totalChunks);
        framed.append(reinterpret_cast<const char*>(&idx), 1);
        framed.append(reinterpret_cast<const char*>(&count), 1);
        framed.append(chunkData);
        const QByteArray packet = HProto::encodeScreenClient(m_voiceToken, seq, framed);
        if (!packet.isEmpty()) m_udp->writeDatagram(packet, destination, m_udpPort);
    }
}
