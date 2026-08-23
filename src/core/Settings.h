#pragma once

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QString>
#include <QVariant>

// Acesso centralizado às configurações persistentes (estilo Halla: tudo guardado localmente)
namespace S {

inline QSettings& store() {
    static QSettings s("Halla", "Halla");
    return s;
}

inline QVariant v(const QString& key, const QVariant& def = QVariant()) {
    return store().value(key, def);
}

inline QString str(const QString& key, const QString& def = QString()) {
    return store().value(key, def).toString();
}

inline int num(const QString& key, int def = 0) {
    return store().value(key, def).toInt();
}

inline bool flag(const QString& key, bool def = false) {
    return store().value(key, def).toBool();
}

inline void set(const QString& key, const QVariant& value) {
    store().setValue(key, value);
    store().sync();
}

// ---- Apelido memorizado por servidor ----------------------------------
// Guarda o último apelido aceito por "host:porta" para que reconectar a um
// servidor não perca o apelido definido lá (connect/recentes/renomeação).
namespace ServerNicks {

constexpr int kMaxEntries = 50;

// Normaliza endereço + porta em uma chave estável. Aceita "host",
// "host:porta" (sempre reinterpretada quando a porta explícita vem em
// separado) e ignora caixa/espaços. Porta ausente = 9987.
inline QString key(const QString& address, quint16 port = 0) {
    QString a = address.trimmed().toLower();
    if (a.isEmpty()) return QString();
    quint16 p = port;
    const int cut = a.lastIndexOf(QLatin1Char(':'));
    if (cut > 0) {
        bool ok = false;
        const int v = a.mid(cut + 1).toInt(&ok);
        if (ok && v >= 1 && v <= 65535) {
            a = a.left(cut);
            if (!p) p = quint16(v);
        }
    }
    if (a.isEmpty()) return QString();
    if (!p) p = 9987;
    return a + QLatin1Char(':') + QString::number(p);
}

inline QString get(const QString& address, quint16 port, const QString& def = QString()) {
    const QString k = key(address, port);
    if (k.isEmpty()) return def;
    const QJsonDocument doc = QJsonDocument::fromJson(
        S::str(QStringLiteral("connect/serverNicks")).toUtf8());
    if (!doc.isArray()) return def;
    for (const QJsonValue& v : doc.array()) {
        const QJsonObject o = v.toObject();
        if (o.value(QStringLiteral("k")).toString() == k)
            return o.value(QStringLiteral("nick")).toString();
    }
    return def;
}

inline void set(const QString& address, quint16 port, const QString& nick) {
    const QString k = key(address, port);
    const QString clean = nick.trimmed();
    if (k.isEmpty() || clean.isEmpty()) return;
    QJsonArray arr;
    const QJsonDocument doc = QJsonDocument::fromJson(
        S::str(QStringLiteral("connect/serverNicks")).toUtf8());
    if (doc.isArray()) {
        for (const QJsonValue& v : doc.array()) {
            const QJsonObject o = v.toObject();
            const QString other = o.value(QStringLiteral("k")).toString();
            if (!other.isEmpty() && other != k) arr.append(o);
        }
    }
    QJsonObject o;
    o[QStringLiteral("k")] = k;
    o[QStringLiteral("nick")] = clean;
    arr.prepend(o);
    while (arr.size() > kMaxEntries) arr.removeLast();
    S::set(QStringLiteral("connect/serverNicks"),
           QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact)));
}

} // namespace ServerNicks

} // namespace S
