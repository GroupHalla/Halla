#pragma once

#include <QDateTime>
#include <QByteArray>
#include <QList>
#include <QMap>
#include <QString>
#include <QJsonObject>
#include <algorithm>

// ============================================================================
// Modelo de dados do cliente Halla (lado do cliente apenas — igual à
// estrutura que o Halla mantém em memória para um servidor conectado)
// ============================================================================

// Codecs de áudio idênticos aos oferecidos pelo Halla
inline QStringList codecNames() {
    return { "Speex Narrowband (8 kHz)",
             "Speex Wideband (16 kHz)",
             "Speex Ultra-Wideband (32 kHz)",
             "CELT Mono (48 kHz)",
             "Opus Voice",
             "Opus Music" };
}

inline QStringList codecShortNames() {
    return { "Speex Narrowband", "Speex Wideband", "Speex Ultra-Wideband",
             "CELT Mono", "Opus Voice", "Opus Music" };
}

struct PermValue {
    bool active = false;
    int  value  = 0;
    int  grant  = 0;
};

struct User {
    int     id = 1;
    QString name;
    QString uniqueId = "HALLAself00000000000000000000=";
    QString version  = "desconhecida";
    QString platform = "Windows";
    QString description;
    QString serverGroups = "Normal";
    int     volumeDb = 0;            // -40 .. +12 dB
    bool    locallyMuted = false;
    bool    inputMuted = false;      // microfone mudo
    bool    outputMuted = false;     // alto-falantes mudos
    bool    away = false;
    bool    recording = false;
    bool    commander = false;
    bool    op = false;              // operador do canal em que está (v3)
    QString avatarHash;              // hash do avatar no servidor (v3)
    QString sigla;                   // siglas de cargos exibidas antes do nome
    QString siglaSuffix;             // siglas de cargos exibidas depois do nome
    QString groupIcon;               // ícone do cargo (nome ou emoji)
    int     groupOrder = 0;           // menor valor = maior prioridade visual
    bool    groupOrderEnabled = true; // servidores antigos sempre usavam a ordem
    int     groupId = 0;
    int     groupPosition = 0;       // Pilar 1: posição hierárquica do cargo (quanto maior, mais autoridade)
    int     groupSiglaPosition = 0;  // hierarquia do cargo com a tag visível (ordena a lista; 0 em servidores antigos)
    bool    talking = false;
    bool    whispering = false;      // sussurrando (sinal laranja)
    bool    screensharing = false;   // compartilhando tela (🔴 LIVE)
    QDateTime connectedAt = QDateTime::currentDateTime();
    // v6 E2EE — diretório de chaves públicas desta sessão (bytes crus):
    //   idPub  = Ed25519 pública em SPKI DER (uid = base64(SHA-256(idPub)))
    //   dhPub  = X25519 pública (32 bytes)
    //   dhSig  = Ed25519(idPriv, "HALLA-DH-V1" || dhPub) — liga a X25519 à identidade
    // e2eeValid: a entrada do diretório passou na verificação LOCAL (uid
    // confere e a assinatura do binding abre). Conteúdo de quem está sem
    // e2eeValid não é confiado para cifrar nem para decifrar.
    QByteArray idPub;
    QByteArray dhPub;
    QByteArray dhSig;
    bool    e2eeValid = false;
};

struct Channel {
    int     id = 0;
    int     parentId = 0;            // 0 = topo
    int     order = 0;               // posição entre os irmãos
    QString name;
    QString topic;
    QString description;
    QString passwordHash;
    bool    hasPassword = false;
    bool    isDefault = false;
    bool    noSymbol = false;        // oculta o símbolo visual antes do nome
    bool    tempChannelParent = false; // recebe novos canais temporários como subcanais
    int     type = 2;                // 0 = temporário, 1 = semi-permanente, 2 = permanente
    bool    moderated = false;
    int     codec = 4;               // Opus Voice
    QStringList opUids;              // UIDs dos operadores deste canal (v3)
    QString temporaryOwnerUid;       // criador do canal temporário; poderes locais limitados
    int     codecQuality = 6;        // 0..10
    int     bitrate = 96;            // de 16kbps a 384kbps (padrão 96)
    QJsonObject groupPerms;          // Pilar 3: permissões de canal por cargo { "groupId": { "perm": state } }
    QJsonObject groupPositionReqs;   // Pilar 1: requisitos de position por grupo no canal
    int     maxClients = -1;         // -1 = ilimitado
    QList<int> linkedChannels;       // canais que compartilham o áudio com este canal
    QList<int> users;
};

struct ServerData {
    QString name;
    QString address;
    QString version  = "desconhecida";
    QString platform = "Linux";
    QString motd     = "Bem-vindo ao Halla!";
    QDateTime connectedAt = QDateTime::currentDateTime();
    int maxClients = 32; // Limite dinâmico de conexões/slots
    QByteArray serverBanner; // PNG/JPEG/GIF/WebP enviado pelo administrador

    int selfId = 1;
    QMap<int, User>    users;
    QMap<int, Channel> channels;
    int  nextChannelId = 1;

    QMap<QString, PermValue> permissions;

    int channelOfUser(int userId) const {
        for (const Channel& c : channels)
            if (c.users.contains(userId)) return c.id;
        return 0;
    }

    QList<int> childChannels(int parentId) const {
        QList<int> out;
        for (const Channel& c : channels)
            if (c.parentId == parentId) out << c.id;
        // Canais temporários sempre por último (comportamento do Halla)
        std::sort(out.begin(), out.end(), [&](int a, int b) {
            const Channel& ca = channels[a];
            const Channel& cb = channels[b];
            if (ca.order != cb.order) return ca.order < cb.order;
            bool ta = ca.type == 0, tb = cb.type == 0;
            if (ta != tb) return tb;
            return ca.name.localeAwareCompare(cb.name) < 0;
        });
        return out;
    }

    int totalClients() const {
        int n = 0;
        for (const Channel& c : channels) n += c.users.size();
        return n;
    }
};
