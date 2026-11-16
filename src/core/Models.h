#pragma once

#include <QDateTime>
#include <QByteArray>
#include <QList>
#include <QMap>
#include <QString>
#include <QJsonObject>
#include <algorithm>

// Client-side data models representing the state of a connected Halla server.

// Canonical whisper target scopes shared across WhisperDialog, OptionsDialog, and ServerTab.
enum class WhisperScope : int {
    CurrentChannel        = 0,
    ChannelAndSubchannels = 1,
    TargetList            = 2,
};

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
    // Per-user playback gain in dB (-60 .. +30), persisted under "userVolume/<uid>".
    int     volumeDb = 0;
    bool    locallyMuted = false;
    bool    inputMuted = false;
    bool    outputMuted = false;
    bool    away = false;
    bool    recording = false;
    bool    commander = false;
    bool    op = false;
    QString avatarHash;
    QString sigla;
    QString siglaSuffix;
    QString groupIcon;
    int     groupOrder = 0;
    bool    groupOrderEnabled = true;
    int     groupId = 0;
    int     groupPosition = 0;
    int     groupSiglaPosition = 0;
    bool    talking = false;
    bool    whispering = false;
    bool    screensharing = false;
    QString screenshareMode;
    QDateTime connectedAt = QDateTime::currentDateTime();
    // Protocol v6 end-to-end encryption directory material:
    //   idPub = Ed25519 SPKI DER public key (uid = base64(SHA-256(idPub)))
    //   dhPub = 32-byte X25519 public key
    //   dhSig = Ed25519(idPriv, "HALLA-DH-V1" || dhPub)
    QByteArray idPub;
    QByteArray dhPub;
    QByteArray dhSig;
    bool    e2eeValid = false;
    // Incremented on structural state changes to drive lightweight vs. full tree updates.
    quint32 rev = 0;
};

struct Channel {
    int     id = 0;
    int     parentId = 0;
    int     order = 0;
    QString name;
    QString topic;
    QString description;
    QString passwordHash;
    bool    hasPassword = false;
    bool    isDefault = false;
    bool    noSymbol = false;
    bool    tempChannelParent = false;
    int     type = 2; // 0 = temporary, 1 = semi-permanent, 2 = permanent
    bool    moderated = false;
    int     codec = 4; // Opus Voice
    QStringList opUids;
    QString temporaryOwnerUid;
    int     codecQuality = 6;
    int     bitrate = 96;
    QJsonObject groupPerms;
    QJsonObject groupPositionReqs;
    int     maxClients = -1;
    QList<int> linkedChannels;
    QList<int> users;
    quint32 rev = 0;
};

struct ServerData {
    QString name;
    QString address;
    QString version  = "desconhecida";
    QString platform = "Linux";
    QString motd     = "Bem-vindo ao Halla!";
    QDateTime connectedAt = QDateTime::currentDateTime();
    int maxClients = 32;
    QByteArray serverBanner;

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
        std::sort(out.begin(), out.end(), [&](int a, int b) {
            const Channel& ca = channels[a];
            const Channel& cb = channels[b];
            if (ca.order != cb.order) return ca.order < cb.order;
            const bool ta = ca.type == 0;
            const bool tb = cb.type == 0;
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
