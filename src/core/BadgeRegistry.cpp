#include "BadgeRegistry.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUrl>

#include <openssl/evp.h>
#include <openssl/x509.h>

#include <algorithm>

namespace {
constexpr qsizetype kMaxManifestBytes = 1024 * 1024;
constexpr qsizetype kMaxSignatureTextBytes = 256;
constexpr qsizetype kMaxIconBytes = 128 * 1024;
const QUrl kManifestUrl(QStringLiteral("https://grouphalla.github.io/badges/v1/badges.json"));
const QUrl kSignatureUrl(QStringLiteral("https://grouphalla.github.io/badges/v1/badges.json.sig"));
const QUrl kBaseUrl(QStringLiteral("https://grouphalla.github.io/badges/v1/"));
const QByteArray kPublicKeyDerBase64(
    "MCowBQYDK2VwAyEA1kF6rKLb8h0zBE/MwSqf+KiPmstcmmWYd6f9GXfhfjA=");

bool saveAtomically(const QString& path, const QByteArray& bytes) {
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return false;
    if (file.write(bytes) != bytes.size()) return false;
    return file.commit();
}

QNetworkRequest registryRequest(const QUrl& url) {
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(15000);
    request.setRawHeader("Accept", "application/json, text/plain;q=0.9, */*;q=0.1");
    request.setRawHeader("User-Agent", "Halla-BadgeRegistry/1");
    return request;
}
}

BadgeRegistry& BadgeRegistry::instance() {
    static BadgeRegistry registry;
    return registry;
}

BadgeRegistry::BadgeRegistry(QObject* parent) : QObject(parent) {}

QString BadgeRegistry::cacheDirectory() const {
    return QStandardPaths::writableLocation(QStandardPaths::CacheLocation)
        + QStringLiteral("/global-badges-v1");
}

void BadgeRegistry::initialize() {
    if (m_initialized) return;
    m_initialized = true;
    m_network = new QNetworkAccessManager(this);
    QDir().mkpath(cacheDirectory());

    QFile manifestFile(cacheDirectory() + QStringLiteral("/badges.json"));
    QFile signatureFile(cacheDirectory() + QStringLiteral("/badges.json.sig"));
    if (manifestFile.open(QIODevice::ReadOnly) && signatureFile.open(QIODevice::ReadOnly)) {
        const QByteArray manifest = manifestFile.read(kMaxManifestBytes + 1);
        const QByteArray signature = signatureFile.read(kMaxSignatureTextBytes + 1);
        QMap<QString, GlobalBadge> badges;
        QHash<QString, QStringList> assignments;
        if (verifyAndParse(manifest, signature, badges, assignments))
            installRegistry(manifest, signature, std::move(badges), std::move(assignments), false);
    }

    fetchRegistry();
}

void BadgeRegistry::fetchRegistry() {
    m_manifestFinished = false;
    m_signatureFinished = false;
    m_pendingManifest.clear();
    m_pendingSignature.clear();

    QNetworkReply* manifestReply = m_network->get(registryRequest(kManifestUrl));
    connect(manifestReply, &QNetworkReply::finished, this, [this, manifestReply] {
        if (manifestReply->error() == QNetworkReply::NoError)
            m_pendingManifest = manifestReply->readAll();
        m_manifestFinished = true;
        manifestReply->deleteLater();
        tryInstallDownloadedRegistry();
    });

    QNetworkReply* signatureReply = m_network->get(registryRequest(kSignatureUrl));
    connect(signatureReply, &QNetworkReply::finished, this, [this, signatureReply] {
        if (signatureReply->error() == QNetworkReply::NoError)
            m_pendingSignature = signatureReply->readAll();
        m_signatureFinished = true;
        signatureReply->deleteLater();
        tryInstallDownloadedRegistry();
    });
}

void BadgeRegistry::tryInstallDownloadedRegistry() {
    if (!m_manifestFinished || !m_signatureFinished) return;
    QMap<QString, GlobalBadge> badges;
    QHash<QString, QStringList> assignments;
    if (!verifyAndParse(m_pendingManifest, m_pendingSignature, badges, assignments)) return;
    installRegistry(m_pendingManifest, m_pendingSignature,
                    std::move(badges), std::move(assignments), true);
}

bool BadgeRegistry::verifySignature(const QByteArray& manifest, const QByteArray& signature) const {
    const QByteArray publicDer = QByteArray::fromBase64(kPublicKeyDerBase64);
    const unsigned char* cursor = reinterpret_cast<const unsigned char*>(publicDer.constData());
    EVP_PKEY* key = d2i_PUBKEY(nullptr, &cursor, publicDer.size());
    EVP_MD_CTX* context = EVP_MD_CTX_new();
    bool valid = false;
    if (key && context && EVP_DigestVerifyInit(context, nullptr, nullptr, nullptr, key) == 1) {
        valid = EVP_DigestVerify(
            context,
            reinterpret_cast<const unsigned char*>(signature.constData()), signature.size(),
            reinterpret_cast<const unsigned char*>(manifest.constData()), manifest.size()) == 1;
    }
    EVP_MD_CTX_free(context);
    EVP_PKEY_free(key);
    return valid;
}

bool BadgeRegistry::verifyAndParse(const QByteArray& manifest,
                                   const QByteArray& signatureText,
                                   QMap<QString, GlobalBadge>& badges,
                                   QHash<QString, QStringList>& assignments) const {
    if (manifest.isEmpty() || manifest.size() > kMaxManifestBytes
            || signatureText.isEmpty() || signatureText.size() > kMaxSignatureTextBytes)
        return false;
    const QByteArray signature = QByteArray::fromBase64(
        signatureText.trimmed(), QByteArray::AbortOnBase64DecodingErrors);
    if (signature.size() != 64 || !verifySignature(manifest, signature)) return false;

    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(manifest, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) return false;
    const QJsonObject root = document.object();
    if (root.value(QStringLiteral("version")).toInt() != 1) return false;

    const QJsonObject definitions = root.value(QStringLiteral("badges")).toObject();
    const QJsonObject users = root.value(QStringLiteral("users")).toObject();
    if (definitions.size() > 64 || users.size() > 100000) return false;
    static const QRegularExpression idPattern(QStringLiteral("^[a-z0-9_]{1,32}$"));
    static const QRegularExpression iconPattern(QStringLiteral("^icons/[a-z0-9_]{1,32}\\.png$"));
    static const QRegularExpression hashPattern(QStringLiteral("^[0-9a-f]{64}$"));

    for (auto it = definitions.begin(); it != definitions.end(); ++it) {
        if (!idPattern.match(it.key()).hasMatch() || !it.value().isObject()) return false;
        const QJsonObject object = it.value().toObject();
        GlobalBadge badge;
        badge.id = it.key();
        badge.name = object.value(QStringLiteral("name")).toString();
        badge.description = object.value(QStringLiteral("description")).toString();
        badge.iconPath = object.value(QStringLiteral("icon")).toString();
        const QString hash = object.value(QStringLiteral("iconSha256")).toString();
        badge.priority = object.value(QStringLiteral("priority")).toInt();
        if (badge.name.isEmpty() || badge.name.size() > 64
                || badge.description.isEmpty() || badge.description.size() > 256
                || !iconPattern.match(badge.iconPath).hasMatch()
                || !hashPattern.match(hash).hasMatch()
                || badge.priority < -10000 || badge.priority > 10000)
            return false;
        badge.iconSha256 = QByteArray::fromHex(hash.toLatin1());
        badges.insert(badge.id, badge);
    }

    for (auto it = users.begin(); it != users.end(); ++it) {
        if (it.key().isEmpty() || it.key().size() > 128 || !it.value().isArray()) return false;
        const QJsonArray array = it.value().toArray();
        if (array.isEmpty() || array.size() > 8) return false;
        QStringList ids;
        for (const QJsonValue& value : array) {
            const QString id = value.toString();
            if (!badges.contains(id) || ids.contains(id)) return false;
            ids << id;
        }
        assignments.insert(it.key(), ids);
    }
    return true;
}

void BadgeRegistry::installRegistry(const QByteArray& manifest,
                                    const QByteArray& signatureText,
                                    QMap<QString, GlobalBadge> badges,
                                    QHash<QString, QStringList> assignments,
                                    bool persist) {
    m_badges = std::move(badges);
    m_assignments = std::move(assignments);
    if (persist) {
        QDir().mkpath(cacheDirectory());
        saveAtomically(cacheDirectory() + QStringLiteral("/badges.json"), manifest);
        saveAtomically(cacheDirectory() + QStringLiteral("/badges.json.sig"), signatureText);
    }
    loadOrFetchIcons();
    emit updated();
}

void BadgeRegistry::loadOrFetchIcons() {
    for (auto it = m_badges.begin(); it != m_badges.end(); ++it) {
        const QString badgeId = it.key();
        const QString cachePath = cacheDirectory() + QStringLiteral("/icon-") + badgeId + QStringLiteral(".png");
        QFile cached(cachePath);
        if (cached.open(QIODevice::ReadOnly)) {
            const QByteArray bytes = cached.read(kMaxIconBytes + 1);
            if (bytes.size() <= kMaxIconBytes
                    && QCryptographicHash::hash(bytes, QCryptographicHash::Sha256) == it->iconSha256) {
                QPixmap icon;
                if (icon.loadFromData(bytes, "PNG")) {
                    it->icon = icon.scaled(24, 24, Qt::KeepAspectRatio, Qt::SmoothTransformation);
                    continue;
                }
            }
        }

        const QUrl iconUrl = kBaseUrl.resolved(QUrl(it->iconPath));
        QNetworkReply* reply = m_network->get(registryRequest(iconUrl));
        const QByteArray expectedHash = it->iconSha256;
        connect(reply, &QNetworkReply::finished, this,
                [this, reply, badgeId, cachePath, expectedHash] {
            const QByteArray bytes = reply->error() == QNetworkReply::NoError
                ? reply->readAll() : QByteArray();
            reply->deleteLater();
            if (bytes.isEmpty() || bytes.size() > kMaxIconBytes
                    || QCryptographicHash::hash(bytes, QCryptographicHash::Sha256) != expectedHash)
                return;
            QPixmap icon;
            if (!icon.loadFromData(bytes, "PNG") || !m_badges.contains(badgeId)) return;
            saveAtomically(cachePath, bytes);
            m_badges[badgeId].icon = icon.scaled(24, 24, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            emit updated();
        });
    }
}

QList<GlobalBadge> BadgeRegistry::badgesForUid(const QString& uid) const {
    QList<GlobalBadge> result;
    for (const QString& badgeId : m_assignments.value(uid)) {
        if (m_badges.contains(badgeId)) result << m_badges.value(badgeId);
    }
    std::sort(result.begin(), result.end(), [](const GlobalBadge& left, const GlobalBadge& right) {
        if (left.priority != right.priority) return left.priority > right.priority;
        return left.id < right.id;
    });
    return result;
}

QStringList BadgeRegistry::badgeNamesForUid(const QString& uid) const {
    QStringList names;
    for (const GlobalBadge& badge : badgesForUid(uid)) names << badge.name;
    return names;
}
