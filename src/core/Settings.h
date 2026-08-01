#pragma once

#include <QSettings>
#include <QString>
#include <QVariant>

// Acesso centralizado às configurações persistentes (estilo TeamSpeak: tudo guardado localmente)
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

} // namespace S
