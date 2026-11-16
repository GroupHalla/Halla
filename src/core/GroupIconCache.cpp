#include "GroupIconCache.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QSet>
#include <QStandardPaths>

namespace {
QHash<QString, qint64> g_iconLastAsked;
QHash<QString, qint64> g_iconNextAllowed;
QHash<QString, int> g_iconBackoffSecs;
QSet<QString> g_iconRefreshed;
}

GroupIconCache& GroupIconCache::instance() {
    static GroupIconCache cache;
    return cache;
}

QString GroupIconCache::safeName(const QString& name) {
    QString out;
    for (const QChar& ch : name.left(60)) {
        if (ch.isLetterOrNumber() || ch == QLatin1Char('.') || ch == QLatin1Char('_')
                || ch == QLatin1Char('-') || ch == QLatin1Char(' ')) {
            out += ch;
        }
    }
    if (out.isEmpty() || out.startsWith(QLatin1Char('.'))) out.prepend(QLatin1Char('_'));
    return out;
}

QString GroupIconCache::serverKey(const QString& serverAddress) {
    static QHash<QString, QString> memo;
    const auto it = memo.constFind(serverAddress);
    if (it != memo.constEnd()) return it.value();
    const QString key = QString::fromLatin1(QCryptographicHash::hash(
        serverAddress.toUtf8(), QCryptographicHash::Sha1).toHex()).left(16);
    memo.insert(serverAddress, key);
    return key;
}

QString GroupIconCache::cacheDir() {
    return QStandardPaths::writableLocation(QStandardPaths::CacheLocation)
            + QStringLiteral("/role-icons");
}

QString GroupIconCache::diskPath(const QString& serverKey, const QString& safe) const {
    return cacheDir() + QStringLiteral("/") + serverKey + QStringLiteral("/") + safe;
}

bool GroupIconCache::isImageName(const QString& name) {
    return name.endsWith(QStringLiteral(".png"), Qt::CaseInsensitive)
        || name.endsWith(QStringLiteral(".jpg"), Qt::CaseInsensitive)
        || name.endsWith(QStringLiteral(".jpeg"), Qt::CaseInsensitive)
        || name.endsWith(QStringLiteral(".gif"), Qt::CaseInsensitive);
}

void GroupIconCache::splitRoleLine(const QString& roleLine, QString* iconName, QString* label) {
    // Scan spaces left-to-right so filenames containing spaces ("my role.png ADMIN") parse cleanly.
    QString icon;
    int sp = -1;
    while ((sp = roleLine.indexOf(QLatin1Char(' '), sp + 1)) > 0) {
        if (isImageName(roleLine.left(sp))) {
            icon = roleLine.left(sp);
            break;
        }
    }
    if (iconName) *iconName = icon;
    if (label) *label = icon.isEmpty() ? QString() : roleLine.mid(sp + 1).trimmed();
}

QStringList GroupIconCache::cleanRoleNames(const QString& serverGroups) {
    QStringList names;
    const QStringList lines = serverGroups.split(QStringLiteral("\n"));
    for (const QString& line : lines) {
        const QString trimmed = line.trimmed();
        if (trimmed.isEmpty()) continue;
        QString icon, label;
        splitRoleLine(trimmed, &icon, &label);
        names << (icon.isEmpty() ? trimmed : label);
    }
    return names;
}

QString GroupIconCache::iconFilePath(const QString& serverKey, const QString& name) {
    const QString safe = safeName(name);
    if (safe.isEmpty()) return QString();
    const QString path = GroupIconCache::instance().diskPath(serverKey, safe);
    return QFile::exists(path) ? path : QString();
}

bool GroupIconCache::shouldRequest(const QString& requestKey, bool haveIt) {
    // Exponential backoff (5s -> 600s) prevents flooding the server when a missing icon returns not_found.
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (haveIt) {
        if (g_iconRefreshed.contains(requestKey)) return false;
        g_iconRefreshed.insert(requestKey);
        g_iconLastAsked.insert(requestKey, now);
        g_iconBackoffSecs.remove(requestKey);
        g_iconNextAllowed.remove(requestKey);
        return true;
    }
    if (now < g_iconNextAllowed.value(requestKey, 0)) return false;
    const int backoff = qBound(5, g_iconBackoffSecs.value(requestKey, 0) * 2, 600);
    g_iconBackoffSecs.insert(requestKey, backoff);
    g_iconNextAllowed.insert(requestKey, now + qint64(backoff) * 1000);
    g_iconLastAsked.insert(requestKey, now);
    return true;
}

QPixmap GroupIconCache::pixmap(const QString& serverKey, const QString& name) {
    const QString safe = safeName(name);
    const QString memKey = serverKey + QLatin1Char('|') + safe;

    const auto it = m_pixmaps.constFind(memKey);
    if (it != m_pixmaps.constEnd()) return it.value();

    QFile f(diskPath(serverKey, safe));
    if (f.open(QIODevice::ReadOnly)) {
        const QByteArray bytes = f.read(256 * 1024);
        f.close();
        QImage img = QImage::fromData(bytes);
        if (!img.isNull()) {
            const QPixmap pm = QPixmap::fromImage(img).scaled(
                24, 21, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            if (!pm.isNull()) {
                m_pixmaps.insert(memKey, pm);
                return pm;
            }
        } else {
            QFile::remove(diskPath(serverKey, safe));
        }
    }
    return QPixmap();
}

void GroupIconCache::store(const QString& serverKey, const QString& name,
                           const QByteArray& bytes) {
    const QString safe = safeName(name);
    const QString memKey = serverKey + QLatin1Char('|') + safe;

    QImage img = QImage::fromData(bytes);
    if (img.isNull()) return;

    const QString requestKey = serverKey + QLatin1Char('|') + name;
    g_iconBackoffSecs.remove(requestKey);
    g_iconNextAllowed.remove(requestKey);

    const QPixmap pm = QPixmap::fromImage(img).scaled(
        24, 21, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    if (pm.isNull()) return;
    m_pixmaps.insert(memKey, pm);

    QDir().mkpath(cacheDir() + QStringLiteral("/") + serverKey);
    QFile f(diskPath(serverKey, safe));
    if (f.open(QIODevice::WriteOnly)) {
        f.write(bytes);
        f.close();
    }
}

void GroupIconCache::clearCache() {
    m_pixmaps.clear();
    g_iconLastAsked.clear();
    g_iconNextAllowed.clear();
    g_iconBackoffSecs.clear();
    g_iconRefreshed.clear();
    QDir(cacheDir()).removeRecursively();
}
