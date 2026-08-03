#pragma once

#include <QDateTime>
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
    QString version  = "3.6.2";
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
    QString sigla;                   // prefixo/sigla do cargo (ex: "[Mod]")
    QString groupIcon;               // ícone do cargo (nome ou emoji)
    int     groupOrder = 0;          // ordem/prioridade do cargo (ex: menor valor = maior prioridade)
    bool    talking = false;
    bool    whispering = false;      // sussurrando (sinal laranja, estilo TS3)
    QDateTime connectedAt = QDateTime::currentDateTime();
};

struct Channel {
    int     id = 0;
    int     parentId = 0;            // 0 = topo
    QString name;
    QString topic;
    QString description;
    QString passwordHash;
    bool    hasPassword = false;
    bool    isDefault = false;
    int     type = 2;                // 0 = temporário, 1 = semi-permanente, 2 = permanente
    bool    moderated = false;
    int     codec = 4;               // Opus Voice
    QStringList opUids;              // UIDs dos operadores deste canal (v3)
    int     codecQuality = 6;        // 0..10
    int     bitrate = 48;            // de 16kbps a 96kbps (padrão 48)
    QJsonObject groupPerms;          // permissões de canal por cargo { "groupId": { "perm": bool } }
    int     maxClients = -1;         // -1 = ilimitado
    QList<int> users;
};

struct ServerData {
    QString name;
    QString address;
    QString version  = "3.13.7";
    QString platform = "Linux";
    QString motd     = "Bem-vindo ao Halla!";
    QDateTime connectedAt = QDateTime::currentDateTime();
    int maxClients = 32; // Limite dinâmico de conexões/slots

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
