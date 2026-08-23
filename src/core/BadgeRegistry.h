#pragma once

#include <QObject>
#include <QHash>
#include <QMap>
#include <QPixmap>
#include <QStringList>

class QNetworkAccessManager;

struct GlobalBadge {
    QString id;
    QString name;
    QString description;
    QString iconPath;
    QByteArray iconSha256;
    int priority = 0;
    QPixmap icon;
};

class BadgeRegistry final : public QObject {
    Q_OBJECT
public:
    static BadgeRegistry& instance();

    void initialize();
    QList<GlobalBadge> badgesForUid(const QString& uid) const;
    QStringList badgeNamesForUid(const QString& uid) const;

signals:
    void updated();

private:
    explicit BadgeRegistry(QObject* parent = nullptr);
    void fetchRegistry();
    void tryInstallDownloadedRegistry();
    bool verifyAndParse(const QByteArray& manifest, const QByteArray& signatureText,
                        QMap<QString, GlobalBadge>& badges,
                        QHash<QString, QStringList>& assignments) const;
    bool verifySignature(const QByteArray& manifest, const QByteArray& signature) const;
    void installRegistry(const QByteArray& manifest, const QByteArray& signatureText,
                         QMap<QString, GlobalBadge> badges,
                         QHash<QString, QStringList> assignments,
                         bool persist);
    void loadOrFetchIcons();
    QString cacheDirectory() const;

    QNetworkAccessManager* m_network = nullptr;
    QMap<QString, GlobalBadge> m_badges;
    QHash<QString, QStringList> m_assignments;
    QByteArray m_pendingManifest;
    QByteArray m_pendingSignature;
    bool m_manifestFinished = false;
    bool m_signatureFinished = false;
    bool m_initialized = false;
};
