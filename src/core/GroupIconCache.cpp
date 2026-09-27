#include "GroupIconCache.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QSet>
#include <QStandardPaths>

// v1.1.37: estado de pedido COMPARTILHADO entre shouldRequest() e store()
// (escopo de arquivo). Antes era local estático dentro de shouldRequest e o
// store() não tinha como zerar o recuo quando os bytes finalmente chegavam.
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

QStringList GroupIconCache::cleanRoleNames(const QString& serverGroups) {
    // O campo "group" do servidor é uma lista de linhas "<icone> <nome>"
    // (ex.: "rota.png ROTA"). Aqui interessa só o TEXTO: devolvemos os
    // nomes dos cargos sem os nomes de arquivo dos ícones, preservando a
    // ordem. Cargo sem ícone de imagem (emoji/sigla/nada) entra inteiro.
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
    // Estado compartilhado por processo: a árvore e o painel de informações
    // pedem os MESMOS ícones — quem chegar primeiro consome a cota.
    //
    // v1.1.37: recuo EXPONENCIAL quando o ícone não chega. O fluxo antigo
    // re-pedia a cada 5 s PARA SEMPRE o ícone que o servidor não tem (cargo
    // referenciando "x.png" nunca enviado: o icon_get volta como not_found
    // e o cache nunca enche) — dezenas desses somavam pedidos por segundo
    // em servidores grandes e colidiam com o limitador genérico do servidor
    // ("você está enviando mensagens rápido demais"). Agora cada falha dobra
    // o intervalo (5 s -> 10 -> 20 -> ... -> 10 min de teto): um ícone
    // quebrado custa ~12 pedidos na primeira hora, não ~720. Quando os
    // bytes chegam, store() zera o recuo (a próxima troca de imagem volta
    // a ser rápida).
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (haveIt) {
        if (g_iconRefreshed.contains(requestKey)) return false;
        g_iconRefreshed.insert(requestKey);
        g_iconLastAsked.insert(requestKey, now);
        g_iconBackoffSecs.remove(requestKey); // em mãos: sem recuo acumulado
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

    // v1.1.37: o ícone CHEGOU — zera o recuo exponencial do pedido para que
    // uma futura atualização de imagem volte a ser buscada rápido (a chave é
    // a mesma do shouldRequest: serverKey + '|' + nome vindo do servidor).
    const QString requestKey = serverKey + QLatin1Char('|') + name;
    g_iconBackoffSecs.remove(requestKey);
    g_iconNextAllowed.remove(requestKey);

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
