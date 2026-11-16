#include "version.h"
#include "NetSession.h"
#include "HallaProtocol.h"
#include "core/AppLog.h"
#include "core/E2eeCrypto.h"
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
#include <algorithm>
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
        if (plain.isEmpty() || key.size() < 32) return QByteArray();
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
        if (plain.isEmpty() || key.size() < 32) return QByteArray();
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
        // Protocol v4+: AEAD authentication failure is terminal. Never try to
        // reinterpret tampered ciphertext as the legacy XOR.
        return QByteArray();
#else
        if (key.size() < 32) return QByteArray();
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

};
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
        { QStringLiteral("screenshare_quality"), QT_TRANSLATE_NOOP("ServerErrors", "A qualidade escolhida excede o limite do servidor.") },
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
        { QStringLiteral("temporary_owner_limit"), QT_TRANSLATE_NOOP("ServerErrors", "O dono do canal temporário só pode alterar senha, bitrate e máximo de clientes.") },
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

namespace {
// v6 E2EE -- protocol format helpers.
// Group key envelope plaintext layout:
//   epoch(8 BE) | key(32) | channel count(4 BE) | channelId(4 BE) x n
constexpr int kGroupKeyFixedBytes = 8 + 32 + 4;

inline QByteArray encodeGroupKeyPlain(qint64 epoch, const QByteArray& key, const QList<int>& chans) {
    QByteArray out;
    out.reserve(kGroupKeyFixedBytes + chans.size() * 4);
    const quint64 e = quint64(epoch);
    out.append(char((e >> 56) & 0xff));
    out.append(char((e >> 48) & 0xff));
    out.append(char((e >> 40) & 0xff));
    out.append(char((e >> 32) & 0xff));
    out.append(char((e >> 24) & 0xff));
    out.append(char((e >> 16) & 0xff));
    out.append(char((e >> 8) & 0xff));
    out.append(char(e & 0xff));
    out.append(key);
    const quint32 n = quint32(chans.size());
    out.append(char((n >> 24) & 0xff));
    out.append(char((n >> 16) & 0xff));
    out.append(char((n >> 8) & 0xff));
    out.append(char(n & 0xff));
    for (int c : chans) {
        const quint32 cc = quint32(c);
        out.append(char((cc >> 24) & 0xff));
        out.append(char((cc >> 16) & 0xff));
        out.append(char((cc >> 8) & 0xff));
        out.append(char(cc & 0xff));
    }
    return out;
}

inline bool decodeGroupKeyPlain(const QByteArray& plain, qint64* epoch, QByteArray* key, QList<int>* chans) {
    if (plain.size() < kGroupKeyFixedBytes) return false;
    quint64 e = 0;
    for (int i = 0; i < 8; ++i)
        e = (e << 8) | quint8(plain[uchar(i)]);
    *epoch = qint64(e);
    *key = plain.mid(8, 32);
    quint32 n = 0;
    for (int i = 0; i < 4; ++i)
        n = (n << 8) | quint8(plain[40 + uchar(i)]);
    if (n == 0 || n > 64 || plain.size() != int(kGroupKeyFixedBytes + n * 4)) return false;
    for (quint32 i = 0; i < n; ++i) {
        quint32 c = 0;
        for (int b = 0; b < 4; ++b)
            c = (c << 8) | quint8(plain[kGroupKeyFixedBytes + int(i) * 4 + b]);
        if (int(c) < 0) return false;
        chans->append(int(c));
    }
    return true;
}

inline QString chatDomainAad(const QString& scope) {
    return QString::fromLatin1(E2ee::kDomainChat) + QLatin1Char('|') + scope;
}
} // namespace

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

    // v6 E2EE: periodic housekeeping -- re-request keys that never arrived,
    // expire queues and re-wrap whisper state after a rotation.
    m_e2eeHousekeeper = new QTimer(this);
    m_e2eeHousekeeper->setInterval(2000);
    connect(m_e2eeHousekeeper, &QTimer::timeout, this, &NetSession::onE2eeHousekeeping);
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
    if (publicKey.isEmpty()) {
        // Without a public key the server would answer bad_identity ("missing
        // identity") -- fail before the round-trip with an actionable error.
        // Emitted deferred: the caller anchors connectionFailed after this
        // return (see MainWindow::connectTo).
        QMetaObject::invokeMethod(
            this,
            [this] {
                emit connectionFailed(
                    tr("Sua identidade não está disponível neste computador "
                       "(chave pública ausente).\n\nAbra a janela Identidades e crie "
                       "uma nova, ou restaure seu backup de identidade."));
            },
            Qt::QueuedConnection);
        return;
    }
    m_pendingHello["idPub"] = QString::fromLatin1(publicKey.toBase64());
    // v6 E2EE: X25519 pair plus an Ed25519 signature that binds the X25519 key
    // to the identity. Without it the server refuses the login (bad_identity) --
    // the E2EE promise does not allow a client without a crypto key.
    if (!e2eeLoadMaterial()) {
        QMetaObject::invokeMethod(
            this,
            [this] {
                emit connectionFailed(
                    tr("Não foi possível preparar a chave de criptografia ponta a ponta "
                       "desta identidade.\n\nAbra a janela Identidades e crie uma nova "
                       "identidade, ou restaure seu backup."));
            },
            Qt::QueuedConnection);
        return;
    }
    m_pendingHello["dhPub"] = QString::fromLatin1(
        IdentityDialog::dhPublicKeyForUid(uid).toBase64());
    m_pendingHello["dhSig"] = QString::fromLatin1(
        IdentityDialog::dhSignatureForUid(uid).toBase64());
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
    // Use the endpoint actually chosen by TCP/TLS for UDP as well. An
    // independent DNS resolution could select IPv6 while the real connection
    // used IPv4 (or an IPv4-mapped address), making the client drop all
    // received voice -- observed mostly with Mobile senders.
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
    // v6 E2EE: group keys/queues die with the session -- a new connection
    // generates or requests fresh keys (forward secrecy between sessions).
    e2eeClearState();
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

    // Keeps the PC's UDP endpoint known to the relay even when the user has not
    // opened the microphone/encoder. The server records the address before
    // validating the payload, so this silent frame also traverses NAT and keeps
    // the first Mobile speech from depending on the PC speaking first.
    if (!m_voiceToken.isEmpty() && m_udpPort) {
        sendUdpNatProbe(++m_udpRegistrationSeq);
    }
}

void NetSession::onUdpReadyRead() {
    while (m_udp->hasPendingDatagrams()) {
        QNetworkDatagram dg = m_udp->receiveDatagram();
        // The official relay is the only accepted UDP sender. Without this check
        // any host that discovered the local port could inject HALL/HALF packets.
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
            QByteArray payload;
            const int chanId = m_target ? m_target->channelOfUser(int(fromId)) : 0;
            // v6 E2EE: voice ALWAYS arrives encrypted -- without a known key there
            // is nothing to do with the packet (handing ciphertext to the decoder
            // only produces noise). Tries the sender's channel key and then the
            // others (recent rotations / cross-channel whisper).
            {
                QList<QByteArray> candidates;
                if (chanId > 0 && m_channelKeys.contains(chanId))
                    candidates << m_channelKeys.value(chanId);
                for (const QByteArray& key : m_channelKeys)
                    if (!key.isEmpty() && !candidates.contains(key)) candidates << key;
                for (const QByteArray& key : candidates) {
                    payload = AeadVoiceCipher::decrypt(encryptedPayload, key, fromId, seq);
                    if (!payload.isEmpty()) break;
                }
                if (payload.isEmpty()) {
                    // v1.1.36 -- STALE key: voice packets from MY channel that do
                    // not open with ANY local key in sequence means the local key is
                    // old (the channel rotation never reached this client). The user
                    // is doubly muted forever: TX encrypts with the old key (nobody
                    // hears it) and this path drops everything (hears nobody) -- the
                    // housekeeper does not ask again because it "has" a key. Threshold
                    // of 15 packets (~300 ms of continuous speech) so ordinary UDP
                    // loss does not trigger it: forget the local key and the
                    // housekeeper re-requests it from the group within the next 2 s.
                    // An isolated loss resets on the next success.
                    const int myCh = m_target
                        ? m_target->channelOfUser(m_target->selfId) : 0;
                    if (chanId > 0 && chanId == myCh) {
                        ++m_e2eeDecryptFails;
                        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
                        if (m_e2eeDecryptFails >= 15
                                && nowMs - m_e2eeDecryptRecoverMs > 30000) {
                            m_e2eeDecryptRecoverMs = nowMs;
                            m_e2eeDecryptFails = 0;
                            AppLog::warn(tr(
                                "A chave de criptografia local deste canal estava "
                                "desatualizada (uma rotação não chegou até aqui) — "
                                "sincronizando com o canal; a voz volta em alguns "
                                "segundos."));
                            emit systemEvent(tr(
                                "Sincronizando a chave de criptografia do canal — "
                                "sua voz pode falhar por alguns segundos."));
                            m_channelKeys.remove(myCh);
                            m_channelEpochs.remove(myCh);
                            m_e2eeKeyRequestTries[myCh] = 0;
                        }
                    }
                    continue;
                }
                m_e2eeDecryptFails = 0; // decifrou: as chaves locais servem
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
            QByteArray chunkPayload;

            const int uid = int(fromId);
            const int chanId = m_target ? m_target->channelOfUser(uid) : 0;
            // v6 E2EE: the shared screen always arrives encrypted too -- without a
            // key, drop it (same policy as voice).
            {
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

void NetSession::sendUdpNatProbe(quint16 seq) {
    // NAT/keepalive registration probe: a tiny encrypted frame whose only job
    // is to teach the relay our UDP endpoint. It carries no audio and is kept
    // strictly separate from real media so that sendVoiceFrame and
    // sendScreenShareFrame can refuse to emit anything without a channel key.
    if (m_voiceToken.isEmpty() || m_udpPort == 0 || !m_target) return;
    const QHostAddress destination = m_udpHostAddress.isNull()
        ? QHostAddress(m_host) : m_udpHostAddress;
    if (destination.isNull()) return;
    const int chanId = m_target->channelOfUser(m_target->selfId);
    if (chanId <= 0 || !m_channelKeys.contains(chanId)) return;
    const QByteArray probe = AeadVoiceCipher::encrypt(
        QByteArray(1, '\0'), m_channelKeys[chanId],
        quint32(m_target->selfId), seq, ++m_cryptoCounter);
    if (probe.isEmpty()) return;
    m_udp->writeDatagram(HProto::encodeVoiceClient(m_voiceToken, seq, probe),
                         destination, m_udpPort);
}

void NetSession::sendVoiceFrame(const QByteArray& opus, quint16 seq) {
    if (m_voiceToken.isEmpty() || m_udpPort == 0) return;
    const QHostAddress destination = m_udpHostAddress.isNull()
        ? QHostAddress(m_host) : m_udpHostAddress;
    if (destination.isNull()) return;

    // v6 E2EE: voice frames are ALWAYS encrypted. Without a channel key the
    // frame does not leave -- the UDP route has no TLS, so a plaintext frame
    // would be audible to any network listener. Drop until the key arrives.
    if (!m_target) return;
    const int chanId = m_target->channelOfUser(m_target->selfId);
    if (chanId <= 0 || !m_channelKeys.contains(chanId)) {
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        if (nowMs - m_e2eeLastNoKeyWarnMs > 30000) {
            m_e2eeLastNoKeyWarnMs = nowMs;
            AppLog::warn(tr("Voice frame dropped: channel E2EE key not received yet "
                            "(speech returns when the key arrives)."));
        }
        return;
    }
    const QByteArray encryptedOpus = AeadVoiceCipher::encrypt(
        opus, m_channelKeys[chanId], quint32(m_target->selfId), seq, ++m_cryptoCounter);
    if (encryptedOpus.isEmpty()) return;

    m_udp->writeDatagram(HProto::encodeVoiceClient(m_voiceToken, seq, encryptedOpus),
                         destination, m_udpPort);
}

// ==================================================================== actions
void NetSession::sendChat(const QString& scope, int to, const QString& text) {
    // v6 E2EE: no chat leaves in plaintext -- the server only sees metadata
    // (scope/recipient/nickname); the text travels encrypted.
    ServerData& d = target();
    QJsonObject m = HProto::msg("chat");
    m["scope"] = scope;
    if (to > 0) m["to"] = to;
    const QByteArray plain = text.toUtf8();
    const QByteArray aad = chatDomainAad(scope).toLatin1();
    if (scope == QLatin1String("private")) {
        const User& u = d.users.value(to);
        if (to <= 0 || !u.e2eeValid || u.dhPub.size() != 32) {
            emit errorOccurred("e2ee_nokey",
                tr("Não foi possível cifrar a mensagem privada: a chave pública de %1 "
                   "não está disponível/verificada.")
                    .arg(u.name.isEmpty() ? QString::number(to) : u.name));
            return;
        }
        const QByteArray blob = E2ee::pairwiseEncrypt(m_e2eeDhPriv, u.dhPub, aad, plain);
        if (blob.isEmpty()) { emit errorOccurred("e2ee_nokey", tr("Falha ao cifrar a mensagem.")); return; }
        m["text"] = QString::fromLatin1(blob.toBase64());
        m["e2ee"] = true;
        send(m);
        return;
    }
    // Group scopes ("server" = logical channel 0; "channel" = my channel)
    const int myCh = d.channelOfUser(d.selfId);
    const int keyChannel = scope == QLatin1String("server") ? 0 : myCh;
    if (scope != QLatin1String("server") && myCh <= 0) {
        // Without a channel there is no "my channel" to encrypt channel-scope
        // chat with -- the server would not distribute it anyway. Refuse instead
        // of encrypting with the wrong key (the server-scope one).
        emit errorOccurred("bad_scope", tr("Entre em um canal para conversar no chat do canal."));
        return;
    }
    if (keyChannel < 0 || !m_channelKeys.contains(keyChannel)) {
        // Key not received yet: queue and request it (the queue is resent when
        // the key arrives; it expires with a clear warning if the master does not
        // answer).
        PendingChat pc;
        pc.scope = scope;
        pc.to = to;
        pc.text = text;
        pc.queuedAt = QDateTime::currentMSecsSinceEpoch();
        m_pendingChats << pc;
        e2eeRequestKey(keyChannel);
        return;
    }
    const QByteArray nonce = E2ee::randomBytes(12);
    QByteArray ct;
    if (!E2ee::aeadSeal(m_channelKeys[keyChannel], nonce, aad, plain, ct)) {
        emit errorOccurred("e2ee_nokey", tr("Falha ao cifrar a mensagem."));
        return;
    }
    QByteArray blob;
    blob.append(nonce);
    blob.append(ct);
    m["text"] = QString::fromLatin1(blob.toBase64());
    m["e2ee"] = true;
    send(m);
}

void NetSession::moveToChannel(int channelId, const QString& pass) {
    QJsonObject m = HProto::msg("move");
    m["channel"] = channelId;
    if (!pass.isEmpty()) m["pass"] = pass;
    send(m);
    // v1.1.37: context to attribute errors to the clicked channel (see
    // consumeMoveErrorContext) — o TCP preserva a ordem pedido/resposta.
    const QString chanName = m_target
        ? m_target->channels.value(channelId).name : QString();
    PendingMove pm;
    pm.channel = channelId;
    pm.name = chanName;
    pm.otherUser = false;
    pm.atMs = QDateTime::currentMSecsSinceEpoch();
    m_pendingMoves << pm;
    while (m_pendingMoves.size() > 6)
        m_pendingMoves.removeFirst();
}

void NetSession::moveOther(int userId, int channelId) {
    QJsonObject m = HProto::msg("move_other");
    m["id"] = userId;
    m["channel"] = channelId;
    send(m);
    // v1.1.37: same as moveToChannel -- "move to your channel"/dragging produce
    // the same error codes and deserve a specific message.
    const QString chanName = m_target
        ? m_target->channels.value(channelId).name : QString();
    PendingMove pm;
    pm.channel = channelId;
    pm.name = chanName;
    pm.otherUser = true;
    pm.atMs = QDateTime::currentMSecsSinceEpoch();
    m_pendingMoves << pm;
    while (m_pendingMoves.size() > 6)
        m_pendingMoves.removeFirst();
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

void NetSession::rename(const QString& newName, int targetUserId) {
    QJsonObject m = HProto::msg("nick");
    // Without an id: renames yourself. With an id: renames another client (the
    // server requires moderation permission and compatible hierarchy).
    if (targetUserId > 0) m["id"] = targetUserId;
    m["name"] = newName;
    send(m);
}

void NetSession::setDescription(const QString& desc) {
    QJsonObject m = HProto::msg("desc");
    m["text"] = desc;
    send(m);
}

void NetSession::poke(int userId, const QString& msg) {
    // v6 E2EE: poke encrypted pair-to-pair -- the server only relays who poked whom.
    ServerData& d = target();
    const User& u = d.users.value(userId);
    QJsonObject m = HProto::msg("poke");
    m["to"] = userId;
    if (userId > 0 && u.e2eeValid && u.dhPub.size() == 32 && m_e2eeDhPriv.size() == 32) {
        const QByteArray blob = E2ee::pairwiseEncrypt(
            m_e2eeDhPriv, u.dhPub, QByteArray(E2ee::kDomainPoke), msg.toUtf8());
        if (blob.isEmpty()) {
            emit errorOccurred("e2ee_nokey", tr("Não foi possível cifrar o poke."));
            return;
        }
        m["msg"] = QString::fromLatin1(blob.toBase64());
        m["e2ee"] = true;
    } else {
        // Without the target's public key there is no way to encrypt -- and a
        // plaintext poke would break the promise. Refuse with an actionable error.
        emit errorOccurred("e2ee_nokey",
            tr("Não foi possível cifrar o poke: a chave pública de %1 não está "
               "disponível/verificada.")
                .arg(u.name.isEmpty() ? QString::number(userId) : u.name));
        return;
    }
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
    // Never block the UI thread waiting on the socket. The short delay lets the
    // quit message leave and avoids re-entrancy/crash while closing the tab.
    QTimer::singleShot(250, m_tcp, [socket = m_tcp] {
        if (socket->state() != QAbstractSocket::UnconnectedState)
            socket->disconnectFromHost();
    });
}

// ================================================== v3 actions
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
    // v6 E2EE: offline message encrypted pair-to-pair (static-static X25519).
    // The recipient decrypts at login with fromUid -- it works even when both
    // ends are never online together. If the target's public key is missing from
    // the local directory, request it from the server (identity_data) and queue.
    QByteArray theirDhPub;
    ServerData& d = target();
    for (const User& u : d.users)
        if (u.uniqueId == uid && u.e2eeValid) { theirDhPub = u.dhPub; break; }
    if (theirDhPub.isEmpty() && m_e2eeDirectory.contains(uid)) {
        const QJsonObject entry = m_e2eeDirectory[uid];
        // "#bad" marks a rejected entry, not a real directory.
        if (!entry.isEmpty())
            theirDhPub = QByteArray::fromBase64(entry["dhPub"].toString().toLatin1());
    }
    if (theirDhPub.size() != 32 || m_e2eeDhPriv.size() != 32) {
        if (m_e2eeDirectory.contains(uid)) {
            emit errorOccurred("e2ee_nokey",
                tr("Não foi possível obter a chave pública desta identidade para "
                   "cifrar a mensagem offline."));
            return;
        }
        QJsonObject q = HProto::msg("identity_get");
        q["uid"] = uid;
        send(q);
        PendingOffline po;
        po.uid = uid;
        po.text = text;
        po.queuedAt = QDateTime::currentMSecsSinceEpoch();
        m_pendingOffline << po;
        return;
    }
    QJsonObject m = HProto::msg("offline_send");
    m["uid"] = uid;
    const QByteArray blob = E2ee::pairwiseEncrypt(
        m_e2eeDhPriv, theirDhPub, QByteArray(E2ee::kDomainOffline), text.toUtf8());
    if (blob.isEmpty()) {
        emit errorOccurred("e2ee_nokey", tr("Falha ao cifrar a mensagem offline."));
        return;
    }
    m["text"] = QString::fromLatin1(blob.toBase64());
    m["e2ee"] = true;
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
    // v6 E2EE: keeps the targets to (re-)wrap MY channel key -- whisper voice
    // frames stay encrypted with the key of the channel the sender IS in, and a
    // target outside the component needs it.
    const ServerData& d = target();
    const int myCh = d.channelOfUser(d.selfId);
    const QSet<int> comp = myCh > 0 ? e2eeComponentOf(myCh) : QSet<int>{};
    const QList<int> memberList = e2eeComponentMembers(comp);
    const QSet<int> members(memberList.cbegin(), memberList.cend());
    const bool wasSharedExternal = m_whisperSharedExternal;
    m_whisperIds = ids;
    m_whisperSharedExternal = false;
    for (int tid : ids)
        if (!members.contains(tid)) { m_whisperSharedExternal = true; break; }
    m_e2eeWhisperNeedsRewrap = true;
    e2eeDistributeWhisperKey();
    // External whisper targets just cleared: they still hold our channel key
    // from the previous sharing window. Rotate it so they lose access.
    if (wasSharedExternal && !m_whisperSharedExternal && myCh > 0
            && e2eeIsMasterOfComponent(myCh))
        e2eeRotateComponentKey(myCh);
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
    if (position >= 0) m["position"] = position;  // Pillar 1: hierarchical position
    send(m);
}

void NetSession::groupReorder(const QJsonArray& entries) {
    QJsonObject m = HProto::msg("group_reorder");
    m["list"] = entries;
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

// ==================================================================== state
// v1.1.36: single predicate for a STRUCTURAL user change (name, roles, mutes,
// away, permissions...) -- used by applyUserJson (full record) AND by the
// incremental user_state handler. Speech transitions (talking/whispering/
// screensharing) do not count: high frequency, they only affect the icon (the
// lightweight updateUserVisuals path of the tree).
static bool userStructuralChanged(const User& a, const User& b) {
    return a.name != b.name
        || a.uniqueId != b.uniqueId
        || a.description != b.description
        || a.serverGroups != b.serverGroups
        || a.sigla != b.sigla || a.siglaSuffix != b.siglaSuffix
        || a.groupIcon != b.groupIcon || a.groupOrder != b.groupOrder
        || a.groupOrderEnabled != b.groupOrderEnabled
        || a.groupId != b.groupId || a.groupPosition != b.groupPosition
        || a.groupSiglaPosition != b.groupSiglaPosition
        || a.inputMuted != b.inputMuted || a.outputMuted != b.outputMuted
        || a.away != b.away || a.recording != b.recording
        || a.commander != b.commander || a.avatarHash != b.avatarHash
        || a.e2eeValid != b.e2eeValid;
}

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
    usr.groupPosition = u["position"].toInt(0);  // Pillar 1: hierarchical position
    usr.groupSiglaPosition = u["siglaPosition"].toInt(0); // visible tag hierarchy
    usr.inputMuted = u["mic"].toBool();
    usr.outputMuted = u["spk"].toBool();
    usr.away = u["away"].toBool();
    usr.recording = u["rec"].toBool();
    usr.commander = u["cc"].toBool();
    usr.avatarHash = u["av"].toString();               // v3
    // v6 E2EE: public key directory + full LOCAL verification
    // (uid == SHA-256(idPub) and dhSig opens with idPub). The server publishes
    // the directory, but trust comes from cryptography -- a forged entry never
    // gets past this point and is never used to encrypt/decrypt anything.
    usr.idPub = QByteArray::fromBase64(u["idPub"].toString().toLatin1());
    usr.dhPub = QByteArray::fromBase64(u["dhPub"].toString().toLatin1());
    usr.dhSig = QByteArray::fromBase64(u["dhSig"].toString().toLatin1());
    usr.e2eeValid = !usr.idPub.isEmpty()
        && usr.dhPub.size() == 32
        && usr.dhSig.size() == 64
        && E2ee::uidForIdPub(usr.idPub) == usr.uniqueId
        && E2ee::verifyDhBinding(usr.idPub, usr.dhPub, usr.dhSig);
    if (!usr.e2eeValid && !usr.uniqueId.isEmpty()) {
        // Invalid/missing entry: warn once per session -- on a v6 server this
        // only happens if the directory was tampered with in transit.
        if (!m_e2eeDirectory.contains(usr.uniqueId + QStringLiteral("#bad"))) {
            m_e2eeDirectory[usr.uniqueId + QStringLiteral("#bad")] = QJsonObject();
            emit e2eeSecurityNotice(
                tr("A entrada de criptografia de %1 no servidor não passou na "
                   "verificação local (chave pública ausente ou assinatura inválida). "
                   "Conversas com esta pessoa permanecerão não cifradas até ela "
                   "reconectar.")
                    .arg(usr.name.isEmpty() ? QStringLiteral("#%1").arg(usr.id) : usr.name));
        }
    } else if (usr.e2eeValid && !usr.uniqueId.isEmpty()) {
        QJsonObject entry;
        entry["idPub"] = QString::fromLatin1(usr.idPub.toBase64());
        entry["dhPub"] = QString::fromLatin1(usr.dhPub.toBase64());
        entry["dhSig"] = QString::fromLatin1(usr.dhSig.toBase64());
        m_e2eeDirectory[usr.uniqueId] = entry;
        e2eeSecurityCheckUser(usr);
    }
    // MY entry is checked against the local material -- a server tampering with
    // the user's own directory entry would break their pair-to-pair channel.
    if (usr.id == d.selfId && usr.e2eeValid && m_e2eeDhPriv.size() == 32
            && (usr.idPub != m_e2eeMyIdPub
                || usr.dhPub != E2ee::dhPublicFromPrivate(m_e2eeDhPriv))) {
        emit e2eeSecurityNotice(
            tr("O servidor publicou chaves de criptografia diferentes das suas "
               "locais. Mensagens privadas podem não decifrar; confira o "
               "certificado TLS do servidor e reconecte."));
    }
    // An invalid entry for MYSELF was the blind spot: the warning above only
    // existed for the e2eeValid=TRUE case. When my own entry fails
    // verification, NOBODY can wrap keys for me -- I cannot speak (the frame
    // never leaves without a key) and cannot hear (nothing decrypts) -- and the
    // client said absolutely nothing to the victim of the double mute.
    if (usr.id == d.selfId && !usr.e2eeValid && !m_e2eeSelfEntryWarned
            && m_e2eeDhPriv.size() == 32) {
        m_e2eeSelfEntryWarned = true;
        const QString text = tr("Sua identidade de criptografia não passou na "
                                "verificação do diretório deste servidor. Sem ela "
                                "ninguém consegue trocar chaves com você — a voz "
                                "fica bloqueada nos dois sentidos. Reconecte-se; se "
                                "o problema persistir, crie uma nova identidade em "
                                "Ferramentas > Identidades.");
        emit e2eeSecurityNotice(text);
        emit systemEvent(text);
    }
    usr.op = d.users.value(usr.id).op;                 // preserva flag de operador
    // Per-user volume and local mute survive user_state: the server resends the
    // user record on every speech transition (VAD on/off), and the object rebuilt
    // here zeroed volumeDb/locallyMuted -- the volume "was not saved" seconds
    // after being set. Preserved in memory and, on the person's first appearance,
    // restored from the value persisted by uniqueId (what the user set in earlier
    // sessions).
    if (!d.users.contains(usr.id) && !usr.uniqueId.isEmpty()
            && usr.uniqueId != d.users.value(d.selfId).uniqueId) {
        QSettings settings;
        usr.volumeDb = qBound(-60, settings.value(
            QStringLiteral("userVolume/%1").arg(usr.uniqueId), 0).toInt(), 30);
    } else {
        usr.volumeDb = d.users.value(usr.id).volumeDb;
    }
    usr.locallyMuted = d.users.value(usr.id).locallyMuted;
    if (usr.id == d.selfId) {
        usr.talking = d.users.value(d.selfId).talking; // preserva estado de fala local ultra responsivo
        usr.whispering = d.users.value(d.selfId).whispering; // preserves local whisper state
        usr.screensharing = d.users.value(d.selfId).screensharing; // local broadcast started by the user
        usr.screenshareMode = d.users.value(d.selfId).screenshareMode; // idem: modo (v1.1.28)
    } else {
        usr.talking = u["talking"].toBool();
        usr.whispering = u["whispering"].toBool();
        // Without this, whoever connected AFTER someone started broadcasting did
        // not see that person's LIVE badge: welcome/user_joined carry each user's
        // "screensharing" field (toJson), but it was ignored here -- only the
        // user_screenshare_state broadcast (on START/STOP) updated the state.
        usr.screensharing = u["screensharing"].toBool();
        // v1.1.28: newer servers also publish the broadcast MODE
        // ("webrtc"/"jpeg"). Whoever connects after it starts still knows how to
        // watch; an old server does not send the field and the mode stays unknown
        // (the viewer watchdog covers that case).
        usr.screenshareMode = u["ssmode"].toString();
    }
    // v1.1.35: revision of the record. user_state arrives on EVERY speech
    // transition (the server resends the whole user record when the VAD turns on
    // or off) -- so the revision only bumps when a STRUCTURAL field actually
    // changed (v1.1.36: shared predicate userStructuralChanged).
    {
        const User prev = d.users.value(usr.id);
        usr.rev = userStructuralChanged(usr, prev) ? prev.rev + 1 : prev.rev;
    }
    d.users[usr.id] = usr;
    refreshOperators();                                // recomputes ops per channel
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
    ch.type = qBound(0, c["type"].toInt(2), 2);
    ch.moderated = c["moderated"].toBool();
    ch.codec = qBound(0, c["codec"].toInt(4), 5);
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
    ch.temporaryOwnerUid = c["tempOwner"].toString();
    // v1.1.35: chan_update is always structural (name/permissions/topics/
    // membership) -- an unconditional revision feeds the tree signature (ServerTab).
    ch.rev = d.channels.value(ch.id).rev + 1;
    d.channels[ch.id] = ch;
    if (ch.id >= d.nextChannelId) d.nextChannelId = ch.id + 1;
    refreshOperators();
}

// v3: marks user.op according to the channel's operator list (UID) for the channel the user is in
void NetSession::refreshOperators() {
    ServerData& d = target();
    for (User& u : d.users) u.op = false;
    for (const Channel& ch : d.channels)
        for (int uid : ch.users)
            if (d.users.contains(uid)
                    && ch.opUids.contains(d.users[uid].uniqueId))
                d.users[uid].op = true;
}

void NetSession::scheduleChannelStateChanged() {
    if (m_channelStateEmitPending) return;
    m_channelStateEmitPending = true;
    // readyRead can deliver dozens of chan_update messages from the same
    // operation. Apply all of them to the model, but repaint the tree only once
    // when returning to the event loop. The QObject context cancels the callback
    // if the session dies.
    QTimer::singleShot(0, this, [this] {
        m_channelStateEmitPending = false;
        emit stateChanged();
    });
}

void NetSession::handleMessage(const QJsonObject& obj) {
    const QString t = obj["t"].toString();
    ServerData& d = target();

    if (t == "identity_challenge") {
        const QByteArray nonce = QByteArray::fromBase64(obj["nonce"].toString().toLatin1());
        const QByteArray sig = IdentityDialog::signNonce(m_identityUid, nonce);
        if (sig.isEmpty()) {
            emit connectionFailed(tr(
                "Não foi possível assinar o desafio da identidade: a chave privada não "
                "está acessível no cofre deste computador.\n\nRestaure seu backup de "
                "identidade ou crie uma nova (janela Identidades)."));
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
        const QString serverText = obj["msg"].toString();
        QString msg = localizedServerError(code, serverText);
        // v1.1.37: a "move"/"move_other" error becomes a CONTEXTUAL message --
        // "no permission to ENTER CHANNEL X" instead of the generic "perform this
        // action" (the user clicked a channel and deserves to know which one and
        // why). Only when there is a recent unanswered move in the window (TCP
        // preserves order; user_moved consumes the entry on success).
        const QString moveContext = consumeMoveErrorContext(code);
        if (!moveContext.isEmpty()) msg = moveContext;
        // The server detail distinguishes causes of the same code (e.g.
        // bad_identity for a missing key vs. an invalid signature).
        AppLog::warn(serverText.isEmpty()
                         ? tr("Erro do servidor: %1 (código: %2)").arg(msg, code)
                         : tr("Erro do servidor: %1 (código: %2; servidor: %3)")
                               .arg(msg, code, serverText));
        if (!m_ready) {
            m_fatalError = true;
            m_data = ServerData();
            // Nickname rejected at login: dedicated signal so the UI can ask for
            // another name and reconnect, instead of the generic failure warning.
            if (code == QLatin1String("name_in_use") || code == QLatin1String("bad_nick")) {
                emit nickRejected(msg.isEmpty() ? code : msg);
                return;
            }
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
        m_screenshareWidth = qBound(640, srv["screenshare_w"].toInt(1920), 3840);
        m_screenshareHeight = qBound(360, srv["screenshare_h"].toInt(1080), 2160);
        m_screenshareFps = qBound(1, srv["screenshare_fps"].toInt(60), 60);
        m_screenshareBitrateKbps = qBound(500,
            srv["screenshare_bitrate"].toInt(8000), 50000);
        m_webRtcIceServers = obj["iceServers"].toArray();
        d.serverBanner = QByteArray::fromBase64(srv["banner"].toString().toLatin1());

        d.users.clear();
        for (const QJsonValue& v : obj["users"].toArray()) applyUserJson(v.toObject());
        d.channels.clear();
        d.nextChannelId = 1;
        for (const QJsonValue& v : obj["channels"].toArray()) applyChanJson(v.toObject());

        // v3: my permissions + the server's group list
        m_myPerms = obj["myPerms"].toObject();
        m_groups  = obj["groups"].toArray();

        QJsonObject voice = obj["voice"].toObject();
        m_udpPort = quint16(voice["udp"].toInt());
        m_voiceToken = QByteArray::fromHex(voice["token"].toString().toLatin1());
        if (m_voiceToken.size() != HProto::kVoiceTokenBytes) m_voiceToken.clear();

        // v6 E2EE: the welcome message does NOT carry keys -- the server does not
        // know them. Group keys arrive wrapped (e2e_key) from the component/scope
        // master, generated locally in the bootstrap below.

        m_ready = true;
        m_pingTimer->start();

        // Registers the UDP endpoint with a non-empty payload. Legacy relays
        // ignored 10-byte HALL datagrams; in that scenario the PC only became a
        // valid target after speaking once.
        if (!m_voiceToken.isEmpty() && m_udpPort) {
            for (quint16 seq = 1; seq <= 3; ++seq)
                sendUdpNatProbe(seq);
            m_udpRegistrationSeq = 3;
        }

        AppLog::info(tr("Conectado a %1 como %2")
                         .arg(m_hostPort, d.users[d.selfId].name));
        // v6 E2EE: key provisioning (master generates; others request) already
        // with the full user/channel state applied.
        e2eeBootstrap();
        m_e2eeHousekeeper->start();
        emit welcomeReceived();
        emit stateChanged();
        return;
    }

    if (t == "channel_key") {
        // v6: the server neither generates nor distributes channel keys -- the
        // message no longer exists in the protocol. Accepting one here would hand
        // group key control back to the server (= the end of E2EE). Ignore and warn.
        emit e2eeSecurityNotice(
            tr("O servidor enviou \"channel_key\" — impossível no protocolo v6 "
               "(o servidor não conhece chaves). A mensagem foi descartada; a "
               "criptografia ponta a ponta continua intacta."));
        AppLog::warn(tr("Mensagem channel_key descartada (v6: servidor não possui chaves)."));
        return;
    }

    if (t == "e2e_key") {
        e2eeHandleKeyEnvelope(obj);
        return;
    }
    if (t == "e2e_key_request") {
        e2eeHandleKeyRequest(obj);
        return;
    }
    if (t == "identity_data") {
        e2eeHandleIdentityData(obj);
        return;
    }

    if (!m_ready) return;

    if (t == "privilege_granted") {
        const QJsonObject effective = obj["myPerms"].toObject();
        if (!effective.isEmpty()) m_myPerms = effective;
        if (obj["individual"].toBool(false))
            m_myPerms[QStringLiteral("*")] = true;
        // Immediately refreshes menus and dialogs that depend on permissions,
        // without requiring a reconnect after using a privilege key.
        emit stateChanged();
        return;
    }

    if (t == "pong") {
        m_pingMs = int(m_pingClock.elapsed());
        emit pingUpdated(m_pingMs);
        return;
    }
    if (t == "chat") {
        // v6 E2EE: decrypts BEFORE handing it to the UI. The plaintext never
        // existe fora das pontas.
        const QString scope = obj["scope"].toString();
        const int fromId = obj["from"].toInt();
        QString text = obj["text"].toString();
        if (!obj["e2ee"].toBool(false)) {
            // Plaintext chat downgrade: v6 never emits plaintext chat. Drop
            // the message instead of displaying server-readable content.
            emit e2eeSecurityNotice(
                tr("O servidor enviou uma mensagem de chat sem criptografia "
                   "ponta a ponta — ela foi descartada."));
            return;
        }
        {
            const QByteArray aad = chatDomainAad(scope).toLatin1();
            const QByteArray blob = QByteArray::fromBase64(obj["text"].toString().toLatin1());
            QByteArray plain;
            bool ok = false;
            if (scope == QLatin1String("private")) {
                // Echo of our own message: the server sends the private chat back to
                // the sender so it shows up in the conversation. The correct key pair
                // is the RECIPIENT's ("to"), not the sender's -- static-static X25519
                // is symmetric, so our own end decrypts what it encrypted. The echo
                // used to use the dhPub of "from" (the sender itself) and turned into
                // "[encrypted message that could not be decrypted]" for BOTH sides.
                const bool selfEcho = (fromId == d.selfId);
                const int peerId = selfEcho ? obj["to"].toInt(0) : fromId;
                const User& u = d.users.value(peerId);
                if (u.e2eeValid && u.dhPub.size() == 32)
                    plain = E2ee::pairwiseDecrypt(m_e2eeDhPriv, u.dhPub, aad, blob);
                ok = !plain.isEmpty();
            } else {
                // Group: try the scope key and, for robustness against recent
                // rotations, every known key.
                const int keyChannel = scope == QLatin1String("server")
                    ? 0 : d.channelOfUser(d.selfId);
                QList<QByteArray> candidates;
                if (keyChannel >= 0 && m_channelKeys.contains(keyChannel))
                    candidates << m_channelKeys.value(keyChannel);
                for (const QByteArray& key : m_channelKeys)
                    if (!key.isEmpty() && !candidates.contains(key)) candidates << key;
                if (blob.size() >= 12 + 16) {
                    const QByteArray nonce = blob.left(12);
                    const QByteArray ctTag = blob.mid(12);
                    for (const QByteArray& key : candidates) {
                        plain.clear();
                        if (E2ee::aeadOpen(key, nonce, aad, ctTag, plain) && !plain.isEmpty()) {
                            ok = true;
                            break;
                        }
                    }
                }
            }
            text = ok ? QString::fromUtf8(plain)
                      : tr("[mensagem cifrada que não pôde ser decifrada]");
        }
        emit chatReceived(scope, fromId,
                          obj["to"].toInt(0),
                          obj["fromName"].toString(""), text);
        return;
    }
    if (t == "user_joined") {
        applyUserJson(obj["user"].toObject());
        e2eeOnUserJoined(obj["user"].toObject());
        emit systemEvent(tr("%1 entrou no servidor")
                             .arg(obj["user"].toObject()["name"].toString()));
        emit stateChanged();
        return;
    }
    if (t == "server_edit") {
        // v2: name/MOTD changed by an administrator, in real time
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
        const QString name = d.users.value(id).name;
        // v6 E2EE: capture the channel BEFORE the removal -- the component master
        // needs to know where the person left from in order to rotate the key.
        const int oldChan = d.channelOfUser(id);
        emit systemEvent(tr("%1 saiu do servidor").arg(name));
        // v1.1.36: membership changed outside applyChanJson -- bump the revision of
        // every affected channel, otherwise the structural tree signature (v1.1.35)
        // does not see the departure and the list is not updated.
        for (Channel& c : d.channels) {
            if (c.users.removeAll(id) > 0) ++c.rev;
        }
        d.users.remove(id);
        e2eeOnUserLeft(id, oldChan);
        emit stateChanged();
        return;
    }
    if (t == "user_moved") {
        const int id = obj["id"].toInt();
        const int chan = obj["channel"].toInt();
        const int old = d.channelOfUser(id);
        // v1.1.37: the move SUCCEEDED -- consume the pending error-context entry
        // that corresponds to this channel (FIFO: the reply of an earlier move
        // already arrived before this one, in TCP order).
        for (int i = 0; i < m_pendingMoves.size(); ++i) {
            if (m_pendingMoves[i].channel == chan) {
                m_pendingMoves.removeAt(i);
                break;
            }
        }
        // v1.1.36: the channel switch mutates membership DIRECTLY (without
        // applyChanJson) -- without bumping the revision of the affected channels
        // the structural tree signature (v1.1.35) did not change and the tree was
        // NEVER rebuilt: the app announced "You joined the channel", but the user's
        // row stayed in the old channel (a v1.1.35 regression -- v1.1.34 rebuilt
        // unconditionally and hid the bug).
        if (d.channels.contains(old)) {
            Channel& oldCh = d.channels[old];
            if (oldCh.users.removeAll(id) > 0) ++oldCh.rev;
        }
        if (d.channels.contains(chan)) {
            Channel& newCh = d.channels[chan];
            if (!newCh.users.contains(id)) {
                newCh.users << id;
                ++newCh.rev;
            }
        }
        e2eeOnUserMoved(id, chan, old);
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
            // v1.1.36: pre-mutation snapshot to bump the revision when a STRUCTURAL
            // field changes in this INCREMENTAL handler (rename, role change, mute...)
            // -- without it the tree signature missed the change and the row text/icon
            // stayed stale (v1.1.34 rebuilt unconditionally on every stateChanged).
            const User before = u;
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
            if (obj.contains("position")) u.groupPosition = obj["position"].toInt(0);
            if (obj.contains("siglaPosition")) u.groupSiglaPosition = obj["siglaPosition"].toInt(0);
            if (userStructuralChanged(u, before)) ++u.rev;
            // rename confirmed by the server: signal it so the nickname for this
            // server is remembered (persistence per host:port)
            if (t == "user_nick" && id == d.selfId && obj.contains("name"))
                emit selfRenamed(u.name);
        }
        emit stateChanged();
        return;
    }
    if (t == "chan_update") {
        applyChanJson(obj["chan"].toObject());
        e2eeOnTopologyChanged(); // v6: links changed the voice component
        scheduleChannelStateChanged();
        return;
    }
    if (t == "chan_removed") {
        // v6 E2EE: the channel is gone -- the key has no further use (a channel
        // recreated with the same id gets a new key from the next master/bootstrap).
        const int goneId = obj["id"].toInt();
        m_channelKeys.remove(goneId);
        m_channelEpochs.remove(goneId);
        d.channels.remove(goneId);
        scheduleChannelStateChanged();
        return;
    }
    if (t == "poke") {
        // "self" marks the confirmation echo of a poke we sent -- it is not a poke
        // received from someone else.
        if (obj.contains("self") && obj["self"].toBool()) return;
        QString msg = obj["msg"].toString();
        if (!obj["e2ee"].toBool(false)) {
            // Plaintext poke downgrade: v6 pokes are always pairwise
            // encrypted. Drop instead of displaying plaintext.
            emit e2eeSecurityNotice(
                tr("O servidor enviou um poke sem criptografia ponta a ponta "
                   "— ele foi descartado."));
            return;
        }
        // v6 E2EE: decrypt before displaying (pairwise with the sender).
        {
            const int fromId = obj["from"].toInt();
            const User& u = d.users.value(fromId);
            QByteArray plain;
            if (u.e2eeValid && u.dhPub.size() == 32 && m_e2eeDhPriv.size() == 32)
                plain = E2ee::pairwiseDecrypt(
                    m_e2eeDhPriv, u.dhPub, QByteArray(E2ee::kDomainPoke),
                    QByteArray::fromBase64(msg.toLatin1()));
            msg = plain.isEmpty()
                ? tr("[poke cifrado que não pôde ser decifrado]")
                : QString::fromUtf8(plain);
        }
        emit pokeReceived(obj["from"].toInt(), obj["fromName"].toString(""),
                          msg);
        return;
    }
    if (t == "user_avatar") {
        const int id = obj["id"].toInt();
        if (d.users.contains(id)) {
            // v1.1.36: avatarHash is structural (part of the tree signature via
            // userStructuralChanged) -- bump along with it.
            User& u = d.users[id];
            const QString av = obj["av"].toString();
            if (u.avatarHash != av) {
                u.avatarHash = av;
                ++u.rev;
            }
        }
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
    if (t == "icon_uploaded") {
        // Server broadcast when an admin uploads/replaces an icon -- without it the
        // client never learned that the icon started to exist (or that the image
        // changed) during the session.
        emit iconUploaded(obj["name"].toString());
        return;
    }
    if (t == "offline_msg") {
        // v6 E2EE: encrypted delivery -- decrypt here (queue + identity_data if the
        // sender is not in the local directory).
        e2eeDeliverOfflineMsg(obj);
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
        // The reply to our request carries "members" in each role; the change
        // broadcast does NOT. Without the merge, any broadcast would wipe the
        // member list of open panels.
        QJsonArray incoming = obj["groups"].toArray();
        for (int i = 0; i < incoming.size(); ++i) {
            QJsonObject g = incoming.at(i).toObject();
            if (g.contains(QStringLiteral("members"))) continue;
            const int gid = g["id"].toInt();
            for (int j = 0; j < m_groups.size(); ++j) {
                const QJsonObject cached = m_groups.at(j).toObject();
                if (cached["id"].toInt() != gid) continue;
                if (cached.contains(QStringLiteral("members")))
                    g["members"] = cached["members"];
                break;
            }
            incoming[i] = g;
        }
        m_groups = incoming;
        emit groupListReceived(m_groups);
        return;
    }
    if (t == "group_member_update") {
        // Member assignment/removal: refresh the affected role in cache and notify
        // the open panels -- without closing and reopening the groups tab.
        const int gid = obj["gid"].toInt();
        const QJsonArray members = obj["members"].toArray();
        for (int i = 0; i < m_groups.size(); ++i) {
            QJsonObject cached = m_groups.at(i).toObject();
            if (cached["id"].toInt() != gid) continue;
            cached["members"] = members;
            m_groups[i] = cached;
            break;
        }
        emit groupMembersUpdated(gid, members);
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
            m_groups[i] = cached; // preserves members, absent in the confirmation
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
        const QString mode = obj["mode"].toString();
        if (d.users.contains(id)) {
            d.users[id].screensharing = on;
            // v1.1.28: stores the mode reported by the server ("webrtc" from
            // webrtc_stream_start; "jpeg" from screenshare_start on a
            // novo; vazio = legado desconhecido).
            if (on) d.users[id].screenshareMode = mode;
            else d.users[id].screenshareMode.clear();
        }
        emit screenshareStateChanged(id, on, mode);
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
                sendUdpNatProbe(seq);
            m_udpRegistrationSeq = 3;
        }
        return;
    }
}

void NetSession::sendWebRtcStreamStart(int width, int height, int fps,
                                       int bitrateKbps) {
    QJsonObject message = HProto::msg("webrtc_stream_start");
    message["width"] = width;
    message["height"] = height;
    message["fps"] = fps;
    message["bitrate"] = bitrateKbps;
    send(message);
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

    if (!m_target) return;
    const int chanId = m_target->channelOfUser(m_target->selfId);
    // v6: screen sharing follows the SAME rule as voice -- without a channel
    // key the frame does not leave (the UDP route has no TLS: in plaintext it
    // would be visible to any network listener).
    if (chanId <= 0 || !m_channelKeys.contains(chanId)) {
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        if (nowMs - m_e2eeLastNoKeyWarnMs > 30000) {
            m_e2eeLastNoKeyWarnMs = nowMs;
            AppLog::warn(tr("Compartilhamento de tela adiado: chave E2EE do canal "
                            "ainda não recebida."));
        }
        return;
    }

    for (int i = 0; i < totalChunks; ++i) {
        int offset = i * maxChunkSize;
        int size = qMin(maxChunkSize, jpeg.size() - offset);
        QByteArray chunkData = jpeg.mid(offset, size);

        chunkData = AeadVoiceCipher::encrypt(chunkData, m_channelKeys[chanId],
                                             quint32(m_target->selfId), seq, ++m_cryptoCounter);
        if (chunkData.isEmpty()) continue;

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

// ====================================================== v6 E2EE -- engine
// All content cryptography on the client lives in this section. The server:
//   * publishes the public key directory (user objects / identity_data);
//   * relays e2e_key/e2e_key_request envelopes without being able to open them;
//   * NEVER knows a group key (channel_key no longer exists in v6).
// Master election is deterministic (lowest UID in the component): every client
// computes the same winner without negotiation. UID, not session id, because the
// session changes on every reconnection -- the UID is the stable identity.


bool NetSession::e2eeKeysReady() const {
    if (!m_ready || m_e2eeDhPriv.size() != 32) return false;
    if (!m_channelKeys.contains(0)) return false;
    const ServerData& d = target();
    const int myCh = d.channelOfUser(d.selfId);
    if (myCh > 0 && !m_channelKeys.contains(myCh)) return false;
    return true;
}

bool NetSession::e2eeLoadMaterial() {
    if (m_identityUid.isEmpty()) return false;
    if (!IdentityDialog::ensureDhKeyPair(m_identityUid)) return false;
    m_e2eeDhPriv = IdentityDialog::dhPrivateKeyForUid(m_identityUid);
    m_e2eeMyIdPub = IdentityDialog::publicKeyForUid(m_identityUid);
    return m_e2eeDhPriv.size() == 32 && m_e2eeMyIdPub.size() > 0;
}

QByteArray NetSession::e2eeMyIdPub() const {
    return m_e2eeMyIdPub;
}

QSet<int> NetSession::e2eeComponentOf(int channelId) const {
    const ServerData& d = target();
    QSet<int> comp;
    if (channelId <= 0 || !d.channels.contains(channelId)) return comp;
    QList<int> todo;
    comp << channelId;
    todo << channelId;
    while (!todo.isEmpty()) {
        const int cur = todo.takeFirst();
        QList<int> neigh = d.channels[cur].linkedChannels;
        for (auto it = d.channels.cbegin(); it != d.channels.cend(); ++it)
            if (it.value().linkedChannels.contains(cur)) neigh << it.key();
        for (int n : neigh) {
            if (n > 0 && d.channels.contains(n) && !comp.contains(n)) {
                comp << n;
                todo << n;
            }
        }
    }
    return comp;
}

QStringList NetSession::e2eeComponentMemberUids(const QSet<int>& comp) const {
    const ServerData& d = target();
    QStringList uids;
    for (int ch : comp) {
        const auto it = d.channels.constFind(ch);
        if (it == d.channels.constEnd()) continue;
        for (int sid : it.value().users) {
            const User& u = d.users.value(sid);
            if (!u.uniqueId.isEmpty()) uids << u.uniqueId;
        }
    }
    return uids;
}

QList<int> NetSession::e2eeComponentMembers(const QSet<int>& comp) const {
    const ServerData& d = target();
    QList<int> members;
    QSet<int> seen;
    for (int ch : comp) {
        const auto it = d.channels.constFind(ch);
        if (it == d.channels.constEnd()) continue;
        for (int sid : it.value().users)
            if (!seen.contains(sid)) { seen << sid; members << sid; }
    }
    return members;
}

bool NetSession::e2eeIsMasterOfComponent(int channelId) const {
    if (channelId == 0) return e2eeIsServerScopeMaster();
    const QSet<int> comp = e2eeComponentOf(channelId);
    const QStringList uids = e2eeComponentMemberUids(comp);
    if (uids.isEmpty()) return true; // sozinho no componente: eu sou o mestre
    const QString myUid = m_identityUid;
    for (const QString& uid : uids)
        if (uid < myUid) return false;
    return true;
}

bool NetSession::e2eeIsServerScopeMaster() const {
    const ServerData& d = target();
    const QString myUid = m_identityUid;
    for (const User& u : d.users)
        if (!u.uniqueId.isEmpty() && u.uniqueId < myUid) return false;
    return true;
}

void NetSession::e2eeBootstrap() {
    if (!m_ready || m_e2eeDhPriv.size() != 32) return;
    const ServerData& d = target();
    // Server scope (public chat, logical channel 0)
    if (e2eeIsServerScopeMaster()) {
        e2eeEnsureComponentKey(0);
        // Clock skew: the generated epoch comes from the LOCAL clock. If some other
        // client holds a key with a "future" epoch (clock ahead), ours would be
        // silently rejected by everyone who already has one -- a double mute with no
        // cure, because both sides "have" a key. The master asks too: a reply with a
        // higher epoch is adopted (convergence).
        m_e2eeVerifyPending << 0;
    } else if (!m_channelKeys.contains(0)) {
        e2eeRequestKey(0);
    }
    // My channel (voice component)
    const int myCh = d.channelOfUser(d.selfId);
    if (myCh > 0) {
        if (e2eeIsMasterOfComponent(myCh)) {
            e2eeEnsureComponentKey(myCh);
            m_e2eeVerifyPending << myCh; // same: check the epoch against the others
        } else {
            const QSet<int> comp = e2eeComponentOf(myCh);
            bool missing = false;
            for (int ch : comp)
                if (!m_channelKeys.contains(ch)) { missing = true; break; }
            if (missing) e2eeRequestKey(myCh);
        }
    }
    e2eeDistributeWhisperKey();
    e2eeFlushPending();
    emit e2eeStateChanged();
}

void NetSession::e2eeEnsureComponentKey(int channelId) {
    const QSet<int> comp = channelId == 0
        ? QSet<int>{0}
        : e2eeComponentOf(channelId);
    if (comp.isEmpty()) return;
    E2eeGroupKey gk;
    bool have = true;
    for (int ch : comp) {
        if (!m_channelKeys.contains(ch)) { have = false; break; }
    }
    if (have) {
        // Key already current (I joined again, or re-bootstrap): distribute to those
        // who lack it -- idempotent, the receiver only accepts higher epochs (the same
        // epoch never rolls anything back).
        gk.key = m_channelKeys.value(*comp.constBegin());
        gk.epoch = m_channelEpochs.value(*comp.constBegin());
        if (gk.key.size() != 32 || gk.epoch == 0) return;
    } else {
        gk.key = E2ee::randomBytes(32);
        if (gk.key.size() != 32) return;
        qint64 epoch = QDateTime::currentMSecsSinceEpoch();
        for (int ch : comp)
            epoch = qMax(epoch, m_channelEpochs.value(ch) + 1);
        gk.epoch = epoch;
        for (int ch : comp) {
            m_channelKeys[ch] = gk.key;
            m_channelEpochs[ch] = gk.epoch;
        }
        if (channelId != 0) m_e2eeWhisperNeedsRewrap = true;
        AppLog::info(channelId == 0
            ? tr("Chave E2EE do escopo servidor gerada (mestre).")
            : tr("Chave E2EE do canal %1 gerada (mestre).").arg(channelId));
    }
    e2eeDistributeComponentKey(comp, gk);
    // Channel key generated/re-shared with whisper active: re-wraps the current key
    // to the targets right away (same urgency as a rotation).
    if (!m_whisperIds.isEmpty()) e2eeDistributeWhisperKey();
    e2eeFlushPending();
    emit e2eeStateChanged();
}

void NetSession::e2eeRotateComponentKey(int channelId) {
    const QSet<int> comp = channelId == 0
        ? QSet<int>{0}
        : e2eeComponentOf(channelId);
    if (comp.isEmpty()) return;
    E2eeGroupKey gk;
    gk.key = E2ee::randomBytes(32);
    if (gk.key.size() != 32) return;
    qint64 epoch = QDateTime::currentMSecsSinceEpoch();
    for (int ch : comp)
        epoch = qMax(epoch, m_channelEpochs.value(ch) + 1);
    gk.epoch = epoch;
    for (int ch : comp) {
        m_channelKeys[ch] = gk.key;
        m_channelEpochs[ch] = gk.epoch;
    }
    if (channelId != 0) m_e2eeWhisperNeedsRewrap = true;
    AppLog::info(channelId == 0
        ? tr("Chave E2EE do escopo servidor rotacionada (membro saiu).")
        : tr("Chave E2EE do canal %1 rotacionada (membro saiu).").arg(channelId));
    e2eeDistributeComponentKey(comp, gk);
    // Rotation with whisper active: the whisperer must re-wrap the new key to the
    // targets IMMEDIATELY -- the following whisper frames already leave encrypted
    // with it.
    if (!m_whisperIds.isEmpty()) e2eeDistributeWhisperKey();
    e2eeFlushPending();
    emit e2eeStateChanged();
}

void NetSession::e2eeDistributeComponentKey(const QSet<int>& comp, const E2eeGroupKey& gk) {
    if (gk.key.size() != 32) return;
    // Server scope: everyone connected. Channel: component members.
    QList<int> targets;
    if (comp.contains(0)) {
        const ServerData& d = target();
        for (auto it = d.users.cbegin(); it != d.users.cend(); ++it)
            if (it.key() != d.selfId) targets << it.key();
    } else {
        targets = e2eeComponentMembers(comp);
        targets.removeAll(target().selfId);
    }
    for (int sid : targets) e2eeShareKeyWith(sid, comp, gk);
}

void NetSession::e2eeShareKeyWith(int sessionId, const QSet<int>& comp, const E2eeGroupKey& gk) {
    const ServerData& d = target();
    const User& u = d.users.value(sessionId);
    if (sessionId <= 0 || sessionId == d.selfId || !u.e2eeValid || u.dhPub.size() != 32)
        return;
    QList<int> chans = comp.values();
    std::sort(chans.begin(), chans.end());
    const QByteArray plain = encodeGroupKeyPlain(gk.epoch, gk.key, chans);
    const QByteArray envelope = E2ee::envelopeWrap(u.dhPub, E2ee::kDomainKeyWrap, plain);
    if (envelope.isEmpty()) return;
    QJsonObject m = HProto::msg("e2e_key");
    m["to"] = sessionId;
    m["enc"] = QString::fromLatin1(envelope.toBase64());
    // Sender authentication: sign the (recipient, envelope) transcript with
    // our Ed25519 identity key so the recipient can prove the envelope came
    // from the claimed master and not from the server or a third party.
    const QByteArray sig = IdentityDialog::signNonce(
        m_identityUid, E2ee::keyEnvelopeSignaturePayload(sessionId, envelope));
    if (!sig.isEmpty())
        m["sig"] = QString::fromLatin1(sig.toBase64());
    send(m);
}

void NetSession::e2eeRequestKey(int channelId) {
    if (channelId < 0) return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    // v1.1.37: 2000 -> 2500 ms. The server accepts 1 e2e_key_request per 2 s window,
    // but only drops the old record when the difference is STRICTLY greater than
    // 2000 ms -- two requests exactly 2000 ms apart (the old limit, <=) were
    // REJECTED with "you are sending messages too fast" exactly when the user
    // switched channels (each entry asks for the new channel's key). 2500 ms gives
    // a safe margin without changing the protocol.
    if (now - m_e2eeLastRequestAt < 2500) return; // o servidor limita 1/2s
    m_e2eeLastRequestAt = now;
    QJsonObject m = HProto::msg("e2e_key_request");
    m["channel"] = channelId;
    send(m);
}

void NetSession::e2eeHandleKeyEnvelope(const QJsonObject& obj) {
    if (m_e2eeDhPriv.size() != 32) return;
    const QByteArray envelope = QByteArray::fromBase64(obj["enc"].toString().toLatin1());
    const QByteArray plain = E2ee::envelopeUnwrap(m_e2eeDhPriv, E2ee::kDomainKeyWrap, envelope);
    qint64 epoch = 0;
    QByteArray key;
    QList<int> chans;
    if (plain.isEmpty()
            || !decodeGroupKeyPlain(plain, &epoch, &key, &chans)
            || key.size() != 32 || epoch <= 0) {
        // Envelope that does not open with MY key: either it belongs to another
        // recipient (misconfigured server) or it was tampered with -- AEAD already
        // rejected it.
        return;
    }
    // Epochs far in the future are impossible from honest clients. The cap
    // must still tolerate real clock skew between machines (minutes on PCs
    // without NTP): 120 s keeps the anti-injection barrier while covering
    // credible clock drift.
    if (epoch > QDateTime::currentMSecsSinceEpoch() + 120'000) return;
    const ServerData& d = target();
    for (int ch : chans) {
        // Only accept keys for channels that exist here (or scope 0).
        if (ch != 0 && !d.channels.contains(ch)) return;
    }
    // Sender authentication (wire-compatible): the server stamps "from" with
    // the sender session id. The sender must be a verified member of the
    // component the key belongs to, and -- when a signature is present -- the
    // Ed25519 signature over the envelope transcript must verify against the
    // sender identity key. Envelopes without a signature are accepted only
    // from verified component members (legacy peers).
    const int fromId = obj["from"].toInt();
    if (fromId <= 0 || fromId == d.selfId) return;
    const User& sender = d.users.value(fromId);
    if (!sender.e2eeValid || sender.idPub.isEmpty()) return;
    for (int ch : chans) {
        if (ch == 0) continue;
        if (!e2eeComponentOf(ch).contains(d.channelOfUser(fromId))) return;
    }
    const QByteArray sig = QByteArray::fromBase64(obj["sig"].toString().toLatin1());
    if (!sig.isEmpty()) {
        if (sig.size() != 64
                || !E2ee::ed25519Verify(sender.idPub,
                    E2ee::keyEnvelopeSignaturePayload(obj["to"].toInt(), envelope), sig))
            return;
    }
    e2eeApplyGroupKey(chans, epoch, key);
}

void NetSession::e2eeApplyGroupKey(const QList<int>& chans, qint64 epoch, const QByteArray& key) {
    const ServerData& d = target();
    const int myCh = d.channelOfUser(d.selfId);
    for (int ch : chans) {
        if (ch < 0) continue;
        if (!m_channelEpochs.contains(ch) || epoch > m_channelEpochs.value(ch)) {
            m_channelKeys[ch] = key;
            m_channelEpochs[ch] = epoch;
        }
    }
    if (chans.contains(myCh)) m_e2eeWhisperNeedsRewrap = true; // sussurro ativo: re-embrulha
    // Whisper active: my channel key just changed -- targets outside the component
    // need the new key NOW (every whisper voice frame leaves encrypted with it).
    // Without this the re-wrap waited for the 2 s housekeeper: the target spent up
    // to 2 s unable to decrypt the whisper after every rotation.
    if (!m_whisperIds.isEmpty()) e2eeDistributeWhisperKey();
    e2eeFlushPending();
    emit e2eeStateChanged();
}

void NetSession::e2eeHandleKeyRequest(const QJsonObject& obj) {
    const int channelId = obj["channel"].toInt();
    const int from = obj["from"].toInt();
    const ServerData& d = target();
    if (channelId < 0 || from <= 0 || from == d.selfId) return;
    if (!m_channelKeys.contains(channelId) || m_channelKeys[channelId].size() != 32)
        return;
    // Only answer requesters that belong to the component (or the server
    // scope). Extra local check: the requester exists and is a member.
    if (!d.users.contains(from)) return;
    QSet<int> comp = channelId == 0
        ? QSet<int>{0}
        : e2eeComponentOf(channelId);
    if (channelId != 0 && !comp.contains(d.channelOfUser(from))) return;
    E2eeGroupKey gk;
    gk.key = m_channelKeys[channelId];
    gk.epoch = m_channelEpochs.value(channelId);
    if (comp.isEmpty()) comp << channelId;
    e2eeShareKeyWith(from, comp, gk);
}

void NetSession::e2eeHandleIdentityData(const QJsonObject& obj) {
    const QString uid = obj["uid"].toString();
    if (uid.isEmpty()) return;
    const QByteArray idPub = QByteArray::fromBase64(obj["idPub"].toString().toLatin1());
    const QByteArray dhPub = QByteArray::fromBase64(obj["dhPub"].toString().toLatin1());
    const QByteArray dhSig = QByteArray::fromBase64(obj["dhSig"].toString().toLatin1());
    if (idPub.isEmpty() || dhPub.size() != 32 || dhSig.size() != 64) return;
    if (E2ee::uidForIdPub(idPub) != uid
            || !E2ee::verifyDhBinding(idPub, dhPub, dhSig))
        return; // tampered entry: AEAD/verification already blocked it
    QJsonObject entry;
    entry["idPub"] = QString::fromLatin1(idPub.toBase64());
    entry["dhPub"] = QString::fromLatin1(dhPub.toBase64());
    entry["dhSig"] = QString::fromLatin1(dhSig.toBase64());
    m_e2eeDirectory[uid] = entry;
    e2eeFlushPending();
}

void NetSession::e2eeDistributeWhisperKey() {
    if (m_whisperIds.isEmpty() || m_e2eeDhPriv.size() != 32) return;
    const ServerData& d = target();
    const int myCh = d.channelOfUser(d.selfId);
    if (myCh <= 0 || !m_channelKeys.contains(myCh)) return;
    const QSet<int> comp = e2eeComponentOf(myCh);
    const QList<int> memberList = e2eeComponentMembers(comp);
    const QSet<int> members(memberList.cbegin(), memberList.cend());
    E2eeGroupKey gk;
    gk.key = m_channelKeys[myCh];
    gk.epoch = m_channelEpochs.value(myCh);
    for (int tid : m_whisperIds) {
        // A target outside the component needs MY channel key to decrypt the whisper
        // voice (the relay delivers the packet; the key does not decrypt traffic the
        // relay would not forward otherwise).
        if (members.contains(tid)) continue;
        e2eeShareKeyWith(tid, comp, gk);
    }
    m_e2eeWhisperNeedsRewrap = false;
}

void NetSession::e2eeOnUserJoined(const QJsonObject& user) {
    if (!m_ready) return;
    // Server scope: whoever was master stays (the lowest UID did not change, someone
    // HIGHER just joined -- had a lower one joined, HE is the master now and
    // self-provisions in his own welcome). Wrapping for the newcomer is enough;
    // those already present request what they miss (housekeeper).
    const int newcomerId = user["id"].toInt();
    if (newcomerId > 0 && e2eeIsServerScopeMaster() && m_channelKeys.contains(0)) {
        QSet<int> comp;
        comp << 0;
        E2eeGroupKey gk;
        gk.key = m_channelKeys[0];
        gk.epoch = m_channelEpochs.value(0);
        e2eeShareKeyWith(newcomerId, comp, gk);
    }
}

void NetSession::e2eeOnUserLeft(int userId, int oldChannel) {
    if (!m_ready) return;
    Q_UNUSED(userId)
    // Forward secrecy: whoever left may not keep reading the public chat or the
    // channel they left -- the master ROTATES immediately.
    if (e2eeIsServerScopeMaster() && m_channelKeys.contains(0))
        e2eeRotateComponentKey(0);
    if (oldChannel > 0 && e2eeIsMasterOfComponent(oldChannel))
        e2eeRotateComponentKey(oldChannel);
}

void NetSession::e2eeOnUserMoved(int userId, int newChannel, int oldChannel) {
    if (!m_ready) return;
    const ServerData& d = target();
    // I joined a channel (myself): the master provisions; otherwise request.
    if (userId == d.selfId && newChannel > 0) {
        if (e2eeIsMasterOfComponent(newChannel)) {
            e2eeEnsureComponentKey(newChannel);
        } else {
            const QSet<int> comp = e2eeComponentOf(newChannel);
            bool missing = false;
            for (int ch : comp)
                if (!m_channelKeys.contains(ch)) { missing = true; break; }
            if (missing) e2eeRequestKey(newChannel);
        }
    }
    // Someone joined my component: if I am the master, I wrap the key for them
    // (and for the others -- idempotent).
    if (userId != d.selfId && newChannel > 0) {
        const QSet<int> comp = e2eeComponentOf(newChannel);
        const int myCh = d.channelOfUser(d.selfId);
        if (comp.contains(myCh) && e2eeIsMasterOfComponent(newChannel))
            e2eeEnsureComponentKey(newChannel);
    }
    // Left a component (me or someone else): the REMAINING master of the old
    // component rotates (whoever left does not hear it again).
    if (oldChannel > 0 && oldChannel != newChannel) {
        if (e2eeIsMasterOfComponent(oldChannel))
            e2eeRotateComponentKey(oldChannel);
    }
}

void NetSession::e2eeOnTopologyChanged() {
    if (!m_ready) return;
    const ServerData& d = target();
    const int myCh = d.channelOfUser(d.selfId);
    if (myCh > 0) {
        if (e2eeIsMasterOfComponent(myCh)) {
            // Links changed the set of listeners: rotation for forward secrecy
            // (whoever joined the component without going through user_moved gets the
            // new key; whoever left loses it).
            e2eeRotateComponentKey(myCh);
        } else {
            const QSet<int> comp = e2eeComponentOf(myCh);
            bool missing = false;
            for (int ch : comp)
                if (!m_channelKeys.contains(ch)) { missing = true; break; }
            if (missing) e2eeRequestKey(myCh);
        }
    }
}

void NetSession::e2eeSecurityCheckUser(const User& u) {
    if (u.uniqueId.isEmpty() || u.idPub.isEmpty()) return;
    QSettings settings;
    const QString key = QStringLiteral("e2ee/verified/%1").arg(u.uniqueId);
    const QString marker = settings.value(key).toString();
    const QString current = QString::fromLatin1(E2ee::sha256(u.idPub).toBase64());
    if (marker.isEmpty()) {
        // Automatic trust (TOFU -- trust on first use): the person's first key that
        // passes local verification (uid + Ed25519 signature over the X25519 binding)
        // is marked as trusted. Nobody has to click anything; cryptography stays
        // end-to-end intact.
        settings.setValue(key, current);
        return;
    }
    if (marker != current) {
        // Identity change since the last time: the only alert that remains (and
        // the one that matters). The conversation stays encrypted; the new key is
        // re-trusted automatically (TOFU) on the next contact.
        const QString text = tr("A identidade de criptografia de %1 mudou "
                                "(novo aparelho ou chave reinstalada). Se você "
                                "não esperava isso, redobre cuidado com esta pessoa.")
                                 .arg(u.name);
        emit e2eeSecurityNotice(text);
        emit systemEvent(text);
        settings.remove(key); // TOFU re-trusts the new key on the next contact
    }
}

void NetSession::e2eeFlushPending() {
    // Chats waiting for a group key
    if (!m_pendingChats.isEmpty()) {
        QList<PendingChat> retry = m_pendingChats;
        m_pendingChats.clear();
        for (const PendingChat& pc : retry)
            sendChat(pc.scope, pc.to, pc.text);
    }
    // Offline messages waiting for the directory (identity_data / user online)
    if (!m_pendingOffline.isEmpty()) {
        QList<PendingOffline> retry = m_pendingOffline;
        m_pendingOffline.clear();
        for (const PendingOffline& po : retry)
            offlineSend(po.uid, po.text);
    }
    // Received offline messages whose sender was not in the directory
    if (!m_pendingOfflineInbox.isEmpty()) {
        QList<PendingOfflineInbox> retry = m_pendingOfflineInbox;
        m_pendingOfflineInbox.clear();
        for (const PendingOfflineInbox& pi : retry) {
            // Replays the receive path now that the directory has grown.
            QJsonObject fake;
            fake["fromUid"] = pi.fromUid;
            fake["fromName"] = pi.fromName;
            fake["text"] = pi.blobB64;
            fake["ts"] = pi.ts;
            fake["e2ee"] = true;
            e2eeDeliverOfflineMsg(fake, pi.receivedAt);
        }
    }
}

void NetSession::e2eeClearState() {
    m_channelKeys.clear();
    m_channelEpochs.clear();
    m_whisperIds.clear();
    m_e2eeDirectory.clear();
    m_pendingChats.clear();
    m_pendingOffline.clear();
    m_pendingOfflineInbox.clear();
    m_e2eeKeyRequestTries.clear();
    m_e2eeWhisperNeedsRewrap = false;
    m_e2eeLastNoKeyWarnMs = 0;
    m_e2eeVerifyPending.clear();
    m_e2eeKeyWaitStartMs = 0;
    m_e2eeWaitChan = -1;
    m_e2eeVoiceKeyWarnedChan = -1;
    m_e2eeSelfEntryWarned = false;
    if (m_e2eeHousekeeper) m_e2eeHousekeeper->stop();
}

void NetSession::onE2eeHousekeeping() {
    if (!m_ready || m_e2eeDhPriv.size() != 32) return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const ServerData& d = target();
    const int myCh = d.channelOfUser(d.selfId);

    // A newly arrived master checks its own epoch against the others (anti
    // split-brain for clock skew). One request per tick respects the server's
    // rate limiter (1 e2e_key_request / 2 s); only those who ALREADY have a key
    // check -- those without fall into the normal re-request right below.
    if (!m_e2eeVerifyPending.isEmpty()) {
        const int verifyCh = m_e2eeVerifyPending.takeFirst();
        if (m_channelKeys.contains(verifyCh))
            e2eeRequestKey(verifyCh);
    }

    // Missing channel key = double mute (cannot speak: the frame never leaves;
    // cannot hear: nothing decrypts). Giving up after 5 attempts left the user
    // blocked FOREVER until reconnecting -- every reconnection became a lottery.
    // Now: 10 attempts every ~3 s and then 1 every 15 s, forever -- the request
    // is tiny and the server already limits it to 1 per 2 s.
    const bool missingScope0 = !m_channelKeys.contains(0);
    bool missingVoiceKey = false;
    if (myCh > 0) {
        const QSet<int> comp = e2eeComponentOf(myCh);
        for (int ch : comp)
            if (!m_channelKeys.contains(ch)) { missingVoiceKey = true; break; }
    }
    auto requestMissing = [&](int ch) {
        const int tries = m_e2eeKeyRequestTries.value(ch, 0);
        const qint64 interval = tries < 10 ? 3000 : 15000;
        if (now - m_e2eeLastRequestAt > interval) {
            m_e2eeKeyRequestTries[ch] = tries + 1;
            e2eeRequestKey(ch);
        }
    };
    if (missingScope0) requestMissing(0);
    if (missingVoiceKey) requestMissing(myCh);

    // Blocking visibility: 12 s connected, inside a channel and without a key is
    // time to warn WITH WHAT TO DO. Without it the user only knows that "it does
    // not speak or hear at all" -- switching channels forces a new E2EE bootstrap
    // and reconnecting rebuilds the whole session.
    if (myCh != m_e2eeWaitChan) {
        m_e2eeWaitChan = myCh;
        m_e2eeKeyWaitStartMs = 0;
    }
    if (missingVoiceKey && myCh > 0) {
        if (m_e2eeKeyWaitStartMs == 0) m_e2eeKeyWaitStartMs = now;
        if (m_e2eeVoiceKeyWarnedChan != myCh && now - m_e2eeKeyWaitStartMs > 12000) {
            m_e2eeVoiceKeyWarnedChan = myCh;
            emit systemEvent(
                tr("A chave de criptografia deste canal não chegou: sua voz está "
                   "bloqueada e você não ouve ninguém. Troque de canal e volte, ou "
                   "reconecte-se ao servidor."));
        }
    } else if (m_e2eeVoiceKeyWarnedChan != -1) {
        const bool recovered = myCh > 0; // in a channel AND with a key = voice is back
        m_e2eeVoiceKeyWarnedChan = -1;
        m_e2eeKeyWaitStartMs = 0;
        if (recovered)
            emit systemEvent(
                tr("Chave de criptografia do canal recebida — voz liberada."));
    } else {
        m_e2eeKeyWaitStartMs = 0;
    }
    // Expires queues that never resolved
    if (!m_pendingChats.isEmpty()) {
        QStringList scopes;
        for (auto it = m_pendingChats.begin(); it != m_pendingChats.end();) {
            if (now - it->queuedAt > 10'000) {
                scopes << it->scope;
                it = m_pendingChats.erase(it);
            } else {
                ++it;
            }
        }
        if (!scopes.isEmpty())
            emit errorOccurred("e2ee_nokey",
                tr("Mensagem não enviada: a chave de criptografia do canal "
                   "não chegou a tempo (reconecte ou troque de canal e volte)."));
    }
    if (!m_pendingOffline.isEmpty()) {
        for (auto it = m_pendingOffline.begin(); it != m_pendingOffline.end();) {
            if (now - it->queuedAt > 15'000) {
                emit errorOccurred("e2ee_nokey",
                    tr("Mensagem offline não enviada: não foi possível obter a chave "
                       "pública do destinatário neste servidor."));
                it = m_pendingOffline.erase(it);
            } else {
                ++it;
            }
        }
    }
    if (!m_pendingOfflineInbox.isEmpty()) {
        for (auto it = m_pendingOfflineInbox.begin(); it != m_pendingOfflineInbox.end();) {
            if (now - it->receivedAt > 15'000) {
                emit offlineMsgReceived(it->fromName,
                    tr("[mensagem cifrada — não foi possível decifrar]"), it->ts);
                it = m_pendingOfflineInbox.erase(it);
            } else {
                ++it;
            }
        }
    }
    // Whisper active: re-wraps the current key to the targets after a rotation
    if (m_e2eeWhisperNeedsRewrap && !m_whisperIds.isEmpty())
        e2eeDistributeWhisperKey();
}

void NetSession::e2eeDeliverOfflineMsg(const QJsonObject& obj, qint64 receivedAt) {
    const QString fromUid = obj["fromUid"].toString();
    const QString fromName = obj["fromName"].toString();
    const QString ts = obj["ts"].toString();
    const QString blobB64 = obj["text"].toString();
    if (!obj["e2ee"].toBool(false)) {
        // Plaintext offline downgrade: v6 offline messages are always
        // pairwise encrypted. Drop instead of displaying plaintext.
        emit e2eeSecurityNotice(
            tr("O servidor entregou uma mensagem offline sem criptografia "
               "ponta a ponta — ela foi descartada."));
        return;
    }
    // The sender, when currently online, must have a verified identity entry;
    // otherwise the directory lookup below could be fed by a forged entry.
    {
        const ServerData& dd = target();
        for (const User& u : dd.users)
            if (u.uniqueId == fromUid && !u.e2eeValid) return;
    }
    // Decrypt with the sender X25519 (local directory or known identity).
    const QByteArray myPriv = m_e2eeDhPriv;
    QByteArray theirDhPub;
    const ServerData& d = target();
    for (const User& u : d.users)
        if (u.uniqueId == fromUid) { theirDhPub = u.dhPub; break; }
    if (theirDhPub.isEmpty() && m_e2eeDirectory.contains(fromUid))
        theirDhPub = QByteArray::fromBase64(
            m_e2eeDirectory[fromUid]["dhPub"].toString().toLatin1());
    if (theirDhPub.size() == 32 && myPriv.size() == 32) {
        const QByteArray plain = E2ee::pairwiseDecrypt(
            myPriv, theirDhPub, E2ee::kDomainOffline,
            QByteArray::fromBase64(blobB64.toLatin1()));
        if (!plain.isEmpty()) {
            emit offlineMsgReceived(fromName, QString::fromUtf8(plain), ts);
            return;
        }
    }
    // No sender key right now: request it from the server registry and decrypt
    // quando a resposta chegar (flushPending reaplica).
    if (!m_e2eeDirectory.contains(fromUid)) {
        QJsonObject q = HProto::msg("identity_get");
        q["uid"] = fromUid;
        send(q);
    }
    PendingOfflineInbox pi;
    pi.fromUid = fromUid;
    pi.fromName = fromName;
    pi.blobB64 = blobB64;
    pi.ts = ts;
    pi.receivedAt = receivedAt > 0 ? receivedAt : QDateTime::currentMSecsSinceEpoch();
    m_pendingOfflineInbox << pi;
}

QString NetSession::consumeMoveErrorContext(const QString& code) {
    // v1.1.37 -- see the PendingMove struct comment in the header. The codes that
    // handleMove/handleMoveOther may return (rate_limited happens BEFORE the
    // handler: the move died in the rate limiter and never executed).
    static const QSet<QString> moveCodes = {
        QStringLiteral("no_permission"), QStringLiteral("hierarchy"),
        QStringLiteral("channel_full"), QStringLiteral("bad_channel_pass"),
        QStringLiteral("invalid_channel"), QStringLiteral("rate_limited"),
    };
    if (!moveCodes.contains(code)) return QString();
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    // Short window: expired, drop without consuming (the error is NOT from this move).
    while (!m_pendingMoves.isEmpty() && now - m_pendingMoves.first().atMs > 6000)
        m_pendingMoves.removeFirst();
    if (m_pendingMoves.isEmpty()) return QString();
    const PendingMove pm = m_pendingMoves.takeFirst();
    const QString name = pm.name.isEmpty() ? tr("canal") : pm.name;
    if (code == QLatin1String("rate_limited"))
        return tr("Muitas solicitações em pouco tempo — aguarde alguns "
                  "segundos e tente novamente.");
    if (code == QLatin1String("invalid_channel"))
        return tr("O canal \"%1\" não existe mais.").arg(name);
    if (code == QLatin1String("channel_full"))
        return tr("O canal \"%1\" está cheio.").arg(name);
    if (code == QLatin1String("bad_channel_pass"))
        return tr("A senha do canal \"%1\" está incorreta.").arg(name);
    // no_permission / hierarchy: entering (move) or taking someone along (move_other).
    return pm.otherUser
        ? tr("Você não tem permissão para mover este cliente para o canal \"%1\".").arg(name)
        : tr("Você não tem permissão para entrar no canal \"%1\".").arg(name);
}
