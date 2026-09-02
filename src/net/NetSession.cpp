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
// v6 E2EE — helpers de formato do protocolo.
// Layout do plaintext do envelope de chave de grupo:
//   época(8 BE) | chave(32) | nº canais(4 BE) | channelId(4 BE)×n
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

    // v6 E2EE: manutenção periódica — re-pedido de chaves que não chegaram,
    // filas que expiram e re-embrulho do sussurro após rotação.
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
        // Sem chave pública o servidor responderia bad_identity ("identidade
        // ausente") — falha antes do round-trip com um erro acionável.
        // Emitido adiado: o chamador ancora connectionFailed depois deste
        // retorno (ver MainWindow::connectTo).
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
    // v6 E2EE: par X25519 + assinatura Ed25519 que liga a X25519 à
    // identidade. Sem isto o servidor recusa o login (bad_identity) — a
    // promessa de E2EE não admite cliente sem chave de criptografia.
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
    // v6 E2EE: chaves/filas de grupo morrem com a sessão — nova conexão gera
    // ou pede chaves novas (forward secrecy entre sessões).
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
            QByteArray payload;
            const int chanId = m_target ? m_target->channelOfUser(int(fromId)) : 0;
            // v6 E2EE: voz chega SEMPRE cifrada — sem chave conhecida não há
            // o que fazer com o pacote (passar ciphertext ao decodificador só
            // produz ruído). Tenta a chave do canal do remetente e as demais
            // (rotações recentes / sussurro cross-canal).
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
            QByteArray chunkPayload;

            const int uid = int(fromId);
            const int chanId = m_target ? m_target->channelOfUser(uid) : 0;
            // v6 E2EE: tela compartilhada também chega sempre cifrada — sem
            // chave, descarta (mesma política da voz).
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

void NetSession::sendVoiceFrame(const QByteArray& opus, quint16 seq) {
    if (m_voiceToken.isEmpty() || m_udpPort == 0) return;
    const QHostAddress destination = m_udpHostAddress.isNull()
        ? QHostAddress(m_host) : m_udpHostAddress;
    if (destination.isNull()) return;
    
    QByteArray encryptedOpus = opus;
    if (m_target) {
        int chanId = m_target->channelOfUser(m_target->selfId);
        if (chanId > 0) {
            // v6 E2EE: sem chave de canal o frame NÃO SAI — a rota UDP não
            // tem TLS, então um frame em claro seria audível por qualquer
            // ouvinte da rede. Descarta até a chave chegar (milissegundos).
            if (!m_channelKeys.contains(chanId)) {
                if (!m_e2eeLoggedNoKeyVoice) {
                    m_e2eeLoggedNoKeyVoice = true;
                    AppLog::warn(tr("Frame de voz descartado: chave E2EE do canal ainda "
                                    "não recebida (a fala volta quando a chave chegar)."));
                }
                return;
            }
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
    // v6 E2EE: nenhum chat sai em claro — o servidor só relata metadados
    // (escopo/destinatário/apelido), o texto vai cifrado.
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
    // Escopos de grupo ("server" = canal lógico 0; "channel" = meu canal)
    const int myCh = d.channelOfUser(d.selfId);
    const int keyChannel = scope == QLatin1String("server") ? 0 : myCh;
    if (scope != QLatin1String("server") && myCh <= 0) {
        // Sem canal não existe "meu canal" para cifrar o chat de escopo
        // canal — o servidor nem distribuiria. Recusa em vez de cifrar com a
        // chave errada (a do escopo servidor).
        emit errorOccurred("bad_scope", tr("Entre em um canal para conversar no chat do canal."));
        return;
    }
    if (keyChannel < 0 || !m_channelKeys.contains(keyChannel)) {
        // Chave ainda não chegou: enfileira e pede (a fila reenvia no arrival
        // da chave; expira com aviso claro se o mestre não responder).
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

void NetSession::rename(const QString& newName, int targetUserId) {
    QJsonObject m = HProto::msg("nick");
    // Sem id: renomeia a si mesmo. Com id: renomeia outro cliente (o
    // servidor exige permissão de moderação e hierarquia compatível).
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
    // v6 E2EE: poke cifrado par-a-par — o servidor relê apenas quem cutucou quem.
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
        // Sem chave pública do alvo não há como cifrar — e poke em claro
        // quebraria a promessa. Recusa com erro acionável.
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
    // v6 E2EE: mensagem offline cifrada par-a-par (X25519 estático-estático).
    // O destinatário decifra no login com o fromUid — funciona mesmo com as
    // duas pontas nunca online juntas. Se a pública do alvo não estiver no
    // diretório local, pede ao servidor (identity_data) e enfileira.
    QByteArray theirDhPub;
    ServerData& d = target();
    for (const User& u : d.users)
        if (u.uniqueId == uid && u.e2eeValid) { theirDhPub = u.dhPub; break; }
    if (theirDhPub.isEmpty() && m_e2eeDirectory.contains(uid)) {
        const QJsonObject entry = m_e2eeDirectory[uid];
        // "#bad" é marcador de entrada rejeitada, não um diretório de verdade.
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
    // v6 E2EE: guarda os alvos para (re-)embrulhar a chave do MEU canal — os
    // frames de voz do sussurro seguem cifrados com a chave do canal em que
    // o remetente ESTÁ, e o alvo fora do componente precisa dela.
    m_whisperIds = ids;
    m_e2eeWhisperNeedsRewrap = true;
    e2eeDistributeWhisperKey();
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
    usr.groupSiglaPosition = u["siglaPosition"].toInt(0); // hierarquia da tag visível
    usr.inputMuted = u["mic"].toBool();
    usr.outputMuted = u["spk"].toBool();
    usr.away = u["away"].toBool();
    usr.recording = u["rec"].toBool();
    usr.commander = u["cc"].toBool();
    usr.avatarHash = u["av"].toString();               // v3
    // v6 E2EE: diretório de chaves públicas + verificação LOCAL completa
    // (uid == SHA-256(idPub) e dhSig abre com idPub). O servidor publica o
    // diretório, mas a confiança vem da criptografia — uma entrada forjada
    // não passa daqui e não é usada para cifrar/decifrar nada.
    usr.idPub = QByteArray::fromBase64(u["idPub"].toString().toLatin1());
    usr.dhPub = QByteArray::fromBase64(u["dhPub"].toString().toLatin1());
    usr.dhSig = QByteArray::fromBase64(u["dhSig"].toString().toLatin1());
    usr.e2eeValid = !usr.idPub.isEmpty()
        && usr.dhPub.size() == 32
        && usr.dhSig.size() == 64
        && E2ee::uidForIdPub(usr.idPub) == usr.uniqueId
        && E2ee::verifyDhBinding(usr.idPub, usr.dhPub, usr.dhSig);
    if (!usr.e2eeValid && !usr.uniqueId.isEmpty()) {
        // Entrada inválida/ausente: avisa uma vez por sessão — em servidor v6
        // isso só acontece se o diretório foi adulterado em trânsito.
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
    // A MINHA entrada é conferida contra o material local — o servidor
    // adulterando o próprio diretório do usuário quebraria o par-a-par dele.
    if (usr.id == d.selfId && usr.e2eeValid && m_e2eeDhPriv.size() == 32
            && (usr.idPub != m_e2eeMyIdPub
                || usr.dhPub != E2ee::dhPublicFromPrivate(m_e2eeDhPriv))) {
        emit e2eeSecurityNotice(
            tr("O servidor publicou chaves de criptografia diferentes das suas "
               "locais. Mensagens privadas podem não decifrar; verifique o código "
               "de segurança e o certificado TLS do servidor."));
    }
    usr.op = d.users.value(usr.id).op;                 // preserva flag de operador
    if (usr.id == d.selfId) {
        usr.talking = d.users.value(d.selfId).talking; // preserva estado de fala local ultra responsivo
        usr.whispering = d.users.value(d.selfId).whispering; // preserva estado de sussurro local
        usr.screensharing = d.users.value(d.selfId).screensharing; // transmissão local iniciada pelo usuário
    } else {
        usr.talking = u["talking"].toBool();
        usr.whispering = u["whispering"].toBool();
        // Sem isto, quem conectasse DEPOIS de alguém começar a transmitir não
        // via o selo LIVE dessa pessoa: o welcome/user_joined trazem o campo
        // "screensharing" de cada usuário (toJson), mas ele era ignorado
        // aqui — só o broadcast user_screenshare_state (ao INICIAR/PARAR a
        // transmissão) atualizava o estado.
        usr.screensharing = u["screensharing"].toBool();
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
    ch.temporaryOwnerUid = c["tempOwner"].toString();
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

void NetSession::scheduleChannelStateChanged() {
    if (m_channelStateEmitPending) return;
    m_channelStateEmitPending = true;
    // readyRead pode trazer dezenas de chan_update da mesma operação. Aplique
    // todos ao modelo, mas redesenhe a árvore somente uma vez ao voltar ao loop
    // de eventos. O contexto QObject cancela o callback se a sessão morrer.
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
        const QString msg = localizedServerError(code, serverText);
        // O detalhe do servidor distingue causas do mesmo código (ex.:
        // bad_identity por chave ausente vs. assinatura inválida).
        AppLog::warn(serverText.isEmpty()
                         ? tr("Erro do servidor: %1 (código: %2)").arg(msg, code)
                         : tr("Erro do servidor: %1 (código: %2; servidor: %3)")
                               .arg(msg, code, serverText));
        if (!m_ready) {
            m_fatalError = true;
            m_data = ServerData();
            // Apelido recusado no login: sinal dedicado para a UI pedir outro
            // nome e reconectar, em vez do aviso genérico de falha.
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

        // v3: minhas permissões + lista de grupos do servidor
        m_myPerms = obj["myPerms"].toObject();
        m_groups  = obj["groups"].toArray();

        QJsonObject voice = obj["voice"].toObject();
        m_udpPort = quint16(voice["udp"].toInt());
        m_voiceToken = QByteArray::fromHex(voice["token"].toString().toLatin1());
        if (m_voiceToken.size() != HProto::kVoiceTokenBytes) m_voiceToken.clear();

        // v6 E2EE: o welcome NÃO traz chaves — o servidor não as conhece.
        // Chaves de grupo chegam embrulhadas (e2e_key) do mestre do
        // componente/escopo, geradas localmente no bootstrap abaixo.

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
        // v6 E2EE: provisão de chaves (mestre gera; demais pedem) já com o
        // estado completo de usuários/canais aplicado.
        e2eeBootstrap();
        m_e2eeHousekeeper->start();
        emit welcomeReceived();
        emit stateChanged();
        return;
    }

    if (t == "channel_key") {
        // v6: o servidor não gera/distribui chaves de canal — a mensagem não
        // existe mais no protocolo. Aceitar uma aqui devolveria ao servidor o
        // controle da chave de grupo (= fim do E2EE). Ignora e avisa.
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
        // v6 E2EE: decifra ANTES de subir para a UI. O texto em claro nunca
        // existe fora das pontas.
        const QString scope = obj["scope"].toString();
        const int fromId = obj["from"].toInt();
        QString text = obj["text"].toString();
        if (obj["e2ee"].toBool(false)) {
            const QByteArray aad = chatDomainAad(scope).toLatin1();
            const QByteArray blob = QByteArray::fromBase64(obj["text"].toString().toLatin1());
            QByteArray plain;
            bool ok = false;
            if (scope == QLatin1String("private")) {
                const User& u = d.users.value(fromId);
                if (u.e2eeValid && u.dhPub.size() == 32)
                    plain = E2ee::pairwiseDecrypt(m_e2eeDhPriv, u.dhPub, aad, blob);
                ok = !plain.isEmpty();
            } else {
                // Grupo: tenta a chave do escopo e, por robustez a rotações
                // recentes, todas as chaves conhecidas.
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
        const QString name = d.users.value(id).name;
        // v6 E2EE: captura o canal ANTES da remoção — o mestre do componente
        // precisa saber de onde a pessoa saiu para rotacionar a chave.
        const int oldChan = d.channelOfUser(id);
        emit systemEvent(tr("%1 saiu do servidor").arg(name));
        for (Channel& c : d.channels) c.users.removeAll(id);
        d.users.remove(id);
        e2eeOnUserLeft(id, oldChan);
        emit stateChanged();
        return;
    }
    if (t == "user_moved") {
        const int id = obj["id"].toInt();
        const int chan = obj["channel"].toInt();
        const int old = d.channelOfUser(id);
        if (d.channels.contains(old)) d.channels[old].users.removeAll(id);
        if (d.channels.contains(chan)) d.channels[chan].users << id;
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
            // renomeação confirmada pelo servidor: avisa para memorizar o
            // apelido deste servidor (persistência por host:porta)
            if (t == "user_nick" && id == d.selfId && obj.contains("name"))
                emit selfRenamed(u.name);
        }
        emit stateChanged();
        return;
    }
    if (t == "chan_update") {
        applyChanJson(obj["chan"].toObject());
        e2eeOnTopologyChanged(); // v6: vínculos mudaram o componente de voz
        scheduleChannelStateChanged();
        return;
    }
    if (t == "chan_removed") {
        // v6 E2EE: canal sumiu — a chave deixa de ter uso (um canal recriado
        // com o mesmo id ganha chave nova no próximo mestre/bootstrap).
        const int goneId = obj["id"].toInt();
        m_channelKeys.remove(goneId);
        m_channelEpochs.remove(goneId);
        d.channels.remove(goneId);
        scheduleChannelStateChanged();
        return;
    }
    if (t == "poke") {
        // "self" marca o eco de confirmação do próprio poke enviado — não é
        // um poke recebido de outra pessoa.
        if (obj.contains("self") && obj["self"].toBool()) return;
        QString msg = obj["msg"].toString();
        // v6 E2EE: decifra antes de exibir (par-a-par com o remetente).
        if (obj["e2ee"].toBool(false)) {
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
    if (t == "icon_uploaded") {
        // Broadcast do servidor quando um admin envia/substitui um ícone —
        // sem isto o cliente jamais ficava sabendo que o ícone passou a
        // existir (ou que a imagem mudou) durante a sessão.
        emit iconUploaded(obj["name"].toString());
        return;
    }
    if (t == "offline_msg") {
        // v6 E2EE: entrega cifrada — decifra aqui (fila + identity_data se o
        // remetente não estiver no diretório local).
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
        // A resposta ao nosso pedido traz "members" em cada cargo; o
        // broadcast de mudança NÃO traz. Sem o merge, qualquer broadcast
        // apagaria a lista de membros dos painéis abertos.
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
        // Atribuição/remoção de membro: atualiza o cargo afetado em cache e
        // avisa os painéis abertos — sem fechar e reabrir a aba de grupos.
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

    int chanId = m_target ? m_target->channelOfUser(m_target->selfId) : 0;
    // v6: tela compartilhada segue a MESMA regra da voz — sem chave de canal
    // o frame não sai (a rota UDP não tem TLS: em claro seria audível/vizível
    // a qualquer ouvinte da rede).
    if (chanId > 0 && !m_channelKeys.contains(chanId)) {
        if (!m_e2eeLoggedNoKeyVoice) {
            m_e2eeLoggedNoKeyVoice = true;
            AppLog::warn(tr("Compartilhamento de tela adiado: chave E2EE do canal "
                            "ainda não recebida."));
        }
        return;
    }

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

// ============================================================ v6 E2EE — motor
// Toda a criptografia de conteúdo do cliente vive nesta seção. O servidor:
//   * publica o diretório de chaves públicas (user objects / identity_data);
//   * retransmite envelopes e2e_key/e2e_key_request sem conseguir abri-los;
//   * NUNCA conhece chave de grupo (channel_key não existe mais no v6).
// A eleição de mestre é determinística (menor UID do componente): qualquer
// cliente calcula o mesmo vencedor sem negociação. UID, e não id de sessão,
// porque sessão muda a cada reconexão — UID é a identidade estável.


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
    // Escopo servidor (chat público, canal lógico 0)
    if (e2eeIsServerScopeMaster()) {
        e2eeEnsureComponentKey(0);
    } else if (!m_channelKeys.contains(0)) {
        e2eeRequestKey(0);
    }
    // Meu canal (componente de voz)
    const int myCh = d.channelOfUser(d.selfId);
    if (myCh > 0) {
        if (e2eeIsMasterOfComponent(myCh)) {
            e2eeEnsureComponentKey(myCh);
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
        // Chave já vigente (eu entrei de novo, ou re-bootstrap): distribui
        // para quem estiver sem — idempotente, o receptor só aceita épocas
        // maiores (mesma época não retrocede nada).
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
    e2eeFlushPending();
    emit e2eeStateChanged();
}

void NetSession::e2eeDistributeComponentKey(const QSet<int>& comp, const E2eeGroupKey& gk) {
    if (gk.key.size() != 32) return;
    // Escopo servidor: todos os conectados. Canal: membros do componente.
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
    send(m);
}

void NetSession::e2eeRequestKey(int channelId) {
    if (channelId < 0) return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - m_e2eeLastRequestAt < 2000) return; // o servidor limita 1/2s
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
        // Envelope que não abre com a MINHA chave: ou é de outro destinatário
        // (servidor mal configurado), ou adulteração — AEAD já rejeitou.
        return;
    }
    // Épocas no futuro distante são impossíveis de clientes honestos (ms
    // Unix atuais); rejeitar limita injeção maliciosa a janela curta.
    if (epoch > QDateTime::currentMSecsSinceEpoch() + 60'000) return;
    const ServerData& d = target();
    for (int ch : chans) {
        // Só aceita chaves de canais que existem aqui (ou do escopo 0).
        if (ch != 0 && !d.channels.contains(ch)) return;
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
    // Só responde quem o servidor deixou perguntar (membro do componente ou
    // escopo servidor). Verificação local extra: o solicitante existe.
    if (!d.users.contains(from)) return;
    QSet<int> comp = channelId == 0
        ? QSet<int>{0}
        : e2eeComponentOf(channelId);
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
        return; // entrada adulterada: o AEAD/verificação já barrou
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
        // Alvo fora do componente precisa da chave do MEU canal para decifrar
        // a voz do sussurro (o relay entrega o pacote; a chave não decifra
        // tráfego que o relay não encaminharia de outra forma).
        if (members.contains(tid)) continue;
        e2eeShareKeyWith(tid, comp, gk);
    }
    m_e2eeWhisperNeedsRewrap = false;
}

void NetSession::e2eeOnUserJoined(const QJsonObject& user) {
    if (!m_ready) return;
    // Escopo servidor: quem era mestre continua (menor UID não mudou, só
    // entrou alguém MAIOR — se tivesse entrado menor, ELE é o mestre agora e
    // se auto-provisiona no próprio welcome). Basta embrulhar para o novato;
    // quem já estava pede se faltar algo (housekeeper).
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
    // Forward secrecy: quem saiu não pode continuar lendo o chat público nem
    // o canal de onde saiu — o mestre ROTACIONA imediatamente.
    if (e2eeIsServerScopeMaster() && m_channelKeys.contains(0))
        e2eeRotateComponentKey(0);
    if (oldChannel > 0 && e2eeIsMasterOfComponent(oldChannel))
        e2eeRotateComponentKey(oldChannel);
}

void NetSession::e2eeOnUserMoved(int userId, int newChannel, int oldChannel) {
    if (!m_ready) return;
    const ServerData& d = target();
    // Entrei num canal (eu mesmo): mestre provisiona; senão pede.
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
    // Outro entrou no meu componente: se eu sou o mestre, embrulho para ele
    // (e para os demais — idempotente).
    if (userId != d.selfId && newChannel > 0) {
        const QSet<int> comp = e2eeComponentOf(newChannel);
        const int myCh = d.channelOfUser(d.selfId);
        if (comp.contains(myCh) && e2eeIsMasterOfComponent(newChannel))
            e2eeEnsureComponentKey(newChannel);
    }
    // Saiu de um componente (eu ou outro): o mestre REMANESCENTE do componente
    // antigo rotaciona (o que saiu não volta a ouvir).
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
            // Vínculos mudaram o conjunto de ouvintes: rotação para forward
            // secrecy (quem entrou no componente sem passar por user_moved
            // ganha a chave nova; quem saiu perde).
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
    if (marker.isEmpty()) return; // nunca verificado: sem alerta
    const QString current = QString::fromLatin1(E2ee::sha256(u.idPub).toBase64());
    if (marker != current) {
        const QString text = tr("ATENÇÃO: a identidade de %1 MUDOU desde a última "
                                "verificação. Confirme o novo código de segurança antes de "
                                "confiar em mensagens desta pessoa.")
                                 .arg(u.name);
        emit e2eeSecurityNotice(text);
        emit systemEvent(text);
        settings.remove(key); // exige nova verificação explícita
    }
}

QString NetSession::e2eeSasCodeFor(int userId) const {
    const ServerData& d = target();
    const User& u = d.users.value(userId);
    if (u.idPub.isEmpty() || m_e2eeMyIdPub.isEmpty()) return QString();
    return E2ee::sasCode(m_e2eeMyIdPub, u.idPub);
}

bool NetSession::e2eeUserVerified(int userId) const {
    const ServerData& d = target();
    const User& u = d.users.value(userId);
    if (u.uniqueId.isEmpty() || u.idPub.isEmpty()) return false;
    QSettings settings;
    const QString marker = settings.value(
        QStringLiteral("e2ee/verified/%1").arg(u.uniqueId)).toString();
    return marker == QString::fromLatin1(E2ee::sha256(u.idPub).toBase64());
}

void NetSession::e2eeMarkUserVerified(int userId) {
    ServerData& d = target();
    const User& u = d.users.value(userId);
    if (u.uniqueId.isEmpty() || u.idPub.isEmpty()) return;
    QSettings settings;
    settings.setValue(QStringLiteral("e2ee/verified/%1").arg(u.uniqueId),
                      QString::fromLatin1(E2ee::sha256(u.idPub).toBase64()));
}

void NetSession::e2eeFlushPending() {
    // Chats à espera de chave de grupo
    if (!m_pendingChats.isEmpty()) {
        QList<PendingChat> retry = m_pendingChats;
        m_pendingChats.clear();
        for (const PendingChat& pc : retry)
            sendChat(pc.scope, pc.to, pc.text);
    }
    // Offlines à espera do diretório (identity_data / user online)
    if (!m_pendingOffline.isEmpty()) {
        QList<PendingOffline> retry = m_pendingOffline;
        m_pendingOffline.clear();
        for (const PendingOffline& po : retry)
            offlineSend(po.uid, po.text);
    }
    // Mensagens offline recebidas cujo remetente não estava no diretório
    if (!m_pendingOfflineInbox.isEmpty()) {
        QList<PendingOfflineInbox> retry = m_pendingOfflineInbox;
        m_pendingOfflineInbox.clear();
        for (const PendingOfflineInbox& pi : retry) {
            // Reaplica o caminho de recebimento agora que o diretório cresceu.
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
    m_e2eeLoggedNoKeyVoice = false;
    if (m_e2eeHousekeeper) m_e2eeHousekeeper->stop();
}

void NetSession::onE2eeHousekeeping() {
    if (!m_ready || m_e2eeDhPriv.size() != 32) return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    // Re-pede chaves que não chegaram (limite de tentativas evita laço eterno).
    const ServerData& d = target();
    const int myCh = d.channelOfUser(d.selfId);
    if (!m_channelKeys.contains(0)
            && m_e2eeKeyRequestTries.value(0, 0) < 5
            && now - m_e2eeLastRequestAt > 3000) {
        m_e2eeKeyRequestTries[0] = m_e2eeKeyRequestTries.value(0, 0) + 1;
        e2eeRequestKey(0);
    }
    if (myCh > 0) {
        const QSet<int> comp = e2eeComponentOf(myCh);
        bool missing = false;
        for (int ch : comp)
            if (!m_channelKeys.contains(ch)) { missing = true; break; }
        if (missing
                && m_e2eeKeyRequestTries.value(myCh, 0) < 5
                && now - m_e2eeLastRequestAt > 3000) {
            m_e2eeKeyRequestTries[myCh] = m_e2eeKeyRequestTries.value(myCh, 0) + 1;
            e2eeRequestKey(myCh);
        }
    }
    // Expira filas que nunca resolveram
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
    // Sussurro ativo: re-embrulha a chave vigente para os alvos após rotação
    if (m_e2eeWhisperNeedsRewrap && !m_whisperIds.isEmpty())
        e2eeDistributeWhisperKey();
}

void NetSession::e2eeDeliverOfflineMsg(const QJsonObject& obj, qint64 receivedAt) {
    const QString fromUid = obj["fromUid"].toString();
    const QString fromName = obj["fromName"].toString();
    const QString ts = obj["ts"].toString();
    const QString blobB64 = obj["text"].toString();
    if (!obj["e2ee"].toBool(false)) {
        emit offlineMsgReceived(fromName, blobB64, ts);
        return;
    }
    // Decifra com a X25519 do remetente (diretório local ou identidade conhecida)
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
    // Sem chave do remetente agora: pede ao registro do servidor e decifra
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
