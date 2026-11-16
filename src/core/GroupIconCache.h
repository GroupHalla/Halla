#pragma once

#include <QHash>
#include <QPixmap>
#include <QString>
#include <QStringList>

// Two-tier (memory + QStandardPaths::CacheLocation) per-server cache for custom role icons.
class GroupIconCache {
public:
    static GroupIconCache& instance();

    // Memoized filesystem-safe hash key for a server "host:port" address.
    static QString serverKey(const QString& serverAddress);

    // Returns the cached 24x21 aspect-preserving pixmap, or a null QPixmap on cache miss.
    QPixmap pixmap(const QString& serverKey, const QString& name);

    // Decodes, scales, caches in memory, and best-effort persists raw icon bytes.
    void store(const QString& serverKey, const QString& name, const QByteArray& bytes);

    // Clears all cached role icons in memory and on disk.
    void clearCache();

    // Convenience wrapper for UI call sites that only need the side effect.
    static void clear() { instance().clearCache(); }

    // Sanitizes an untrusted server icon filename so it cannot escape the cache directory.
    static QString safeName(const QString& name);

    // Returns true when the role token references an uploaded raster/animated image.
    static bool isImageName(const QString& name);

    // Splits a server "<icon> <role name>" line into image filename and display label.
    static void splitRoleLine(const QString& roleLine, QString* iconName, QString* label);

    // Strips image filenames from newline-delimited server group strings.
    static QStringList cleanRoleNames(const QString& serverGroups);

    // Returns the absolute on-disk path when cached, or an empty string otherwise.
    static QString iconFilePath(const QString& serverKey, const QString& name);

    // Shared exponential-backoff request gate across tree and info panel consumers.
    static bool shouldRequest(const QString& requestKey, bool haveIt);

    // Absolute directory used for persistent role icon storage.
    static QString cacheDir();

private:
    GroupIconCache() = default;

    QString diskPath(const QString& serverKey, const QString& safe) const;

    QHash<QString, QPixmap> m_pixmaps;
};
