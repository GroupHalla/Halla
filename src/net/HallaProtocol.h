#pragma once

#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>
#include <QString>
#include <QHostAddress>
#include <QByteArray>

// Wire protocol v6: Ed25519 identity, AEAD voice, and a random 128-bit UDP token.
// Protocol v6 provides real end-to-end encryption: group keys are generated on
// the clients (master = lowest UID in the component) and distributed through
// opaque e2e_key envelopes relayed by the server. Clients v1-v5 are rejected
// with bad_proto: the promise that the server cannot read content requires a
// hard transition.
namespace HProto {

constexpr quint16 kDefaultPort = 9987;
constexpr int kProtoVersion = 6;
constexpr int kProtoMin = 6;
constexpr int kVoiceTokenBytes = 16;
constexpr int kClientMediaHeaderV4Bytes = 4 + kVoiceTokenBytes + 2;

inline QJsonObject msg(const QString& type) {
    QJsonObject o;
    o["t"] = type;
    return o;
}

// v4 client -> server: "HAL4" | token (16 bytes) | seq (u16 LE) | AEAD payload.
inline QByteArray encodeClientMediaV4(const char magic[4], const QByteArray& token,
                                      quint16 seq, const QByteArray& payload) {
    if (token.size() != kVoiceTokenBytes) return {};
    QByteArray p;
    p.reserve(kClientMediaHeaderV4Bytes + payload.size());
    p.append(magic, 4);
    p.append(token);
    p.append(reinterpret_cast<const char*>(&seq), 2);
    p.append(payload);
    return p;
}

inline QByteArray encodeVoiceClient(const QByteArray& token, quint16 seq,
                                    const QByteArray& opus) {
    return encodeClientMediaV4("HAL4", token, seq, opus);
}

inline QByteArray encodeScreenClient(const QByteArray& token, quint16 seq,
                                     const QByteArray& payload) {
    return encodeClientMediaV4("HAF4", token, seq, payload);
}

} // namespace HProto
