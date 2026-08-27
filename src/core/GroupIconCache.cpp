#include "GroupIconCache.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QSet>
#include <QStandardPaths>

GroupIconCache& GroupIconCache::instance() {
    static GroupIconCache cache;
    return cache;
}

QString GroupIconCache::safeName(const QString& name) {
    // Espelha o sanitizeFileName do servidor: ícone com nome malicioso
    // ("../../x") vira um nome inofensivo dentro do cache.
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
    // Memoizado: o delegado chama a cada repaint de cada linha.
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
    // O servidor concatena "<icone> <nome>" (applyGroup) sem separador
    // explícito. Ícones de IMAGEM terminam em extensão conhecida: varremos os
    // espaços da esquerda para a direita e a PRIMEIRA quebra cujo lado
    // esquerdo é um nome de imagem delimita o ícone — cobre também nomes de
    // arquivo com espaços ("meu cargo.png ROTA"). Emoji/letra/sigla e cargo
    // sem ícone não casam com extensão: a linha inteira é o nome do cargo.
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

QString GroupIconCache::iconFilePath(const QString& serverKey, const QString& name) {
    const QString safe = safeName(name);
    if (safe.isEmpty()) return QString();
    const QString path = GroupIconCache::instance().diskPath(serverKey, safe);
    return QFile::exists(path) ? path : QString();
}

bool GroupIconCache::shouldRequest(const QString& requestKey, bool haveIt) {
    // Estado compartilhado por processo: a árvore e o painel de informações
    // pedem os MESMOS ícones — quem chegar primeiro consome a cota.
    static QHash<QString, qint64> lastAsked;
    static QSet<QString> refreshed;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (haveIt) {
        if (refreshed.contains(requestKey)) return false;
        refreshed.insert(requestKey);
        lastAsked.insert(requestKey, now);
        return true;
    }
    const auto it = lastAsked.constFind(requestKey);
    if (it != lastAsked.constEnd() && now - it.value() < 5000) return false;
    lastAsked.insert(requestKey, now);
    return true;
}

QPixmap GroupIconCache::pixmap(const QString& serverKey, const QString& name) {
    const QString safe = safeName(name);
    const QString memKey = serverKey + QLatin1Char('|') + safe;

    // 1) memória — caminho quente do delegado (paint roda o tempo todo).
    const auto it = m_pixmaps.constFind(memKey);
    if (it != m_pixmaps.constEnd()) return it.value();

    // 2) disco — sobrevive entre execuções; se a imagem estiver corrompida,
    //    remove para que o re-request traga uma cópia boa.
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
    if (img.isNull()) return; // bytes inválidos: mantém o que já existe

    const QPixmap pm = QPixmap::fromImage(img).scaled(
        24, 21, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    if (pm.isNull()) return;
    m_pixmaps.insert(memKey, pm);

    // Disco é best effort: em dir só de leitura a memória já garantiu a
    // exibição nesta sessão.
    QDir().mkpath(cacheDir() + QStringLiteral("/") + serverKey);
    QFile f(diskPath(serverKey, safe));
    if (f.open(QIODevice::WriteOnly)) {
        f.write(bytes);
        f.close();
    }
}
