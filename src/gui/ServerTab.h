#pragma once

#include <QWidget>
#include <QSet>
#include <QPoint>
#include "core/Models.h"
#include "dialogs/AdminDialogs.h"

class ServerTreeWidget;
class ChatPanel;
class InfoPanel;
class QSplitter;
class QLabel;
class QToolButton;
class NetSession;
class VoiceEngine;

// One Halla "connection" = one server tab -- classic Halla layout: channel tree
// (50%) | information panel (50%) side by side on top, and the chat console taking
// 100% of the width below.
class ServerTab : public QWidget {
    Q_OBJECT
public:
    explicit ServerTab(const ServerData& initial, QWidget* parent = nullptr);

    ServerData& data() { return m_data; }
    const ServerData& data() const { return m_data; }
    ServerTreeWidget* tree() const { return m_tree; }
    ChatPanel* chat() const { return m_chat; }
    InfoPanel* info() const { return m_info; }
    NetSession* net() const { return m_net; }
    VoiceEngine* voice() const { return m_voice; }
    bool isNetworked() const { return m_net != nullptr; }

    // connects the tab to a network session (connected to a Halla Server)
    void attachNetwork(NetSession* net);

    QString tabTitle() const;

    // real client-side operations
    void joinChannel(int channelId);
    void createChannel(int parentId);
    void editChannel(int channelId);
    void deleteChannel(int channelId);
    void renameSelf();
    void renameUser(int userId);
    void setSelfDescription();
    void editVirtualServerName();

    // ---- v3: Halla feature parity
    void setAvatarInteractive();          // escolher imagem e enviar ao servidor
    void removeAvatar();
    void toggleRecording();               // local WAV recording
    bool isRecording() const;
    void openOfflineMessages();           // caixa de entrada offline
    void setWhisperUids(const QStringList& uids); // vazio = desligar sussurro
    bool whisperActive() const { return !m_whisperUids.isEmpty(); }

    // ---- v3.11: whisper by SHORTCUT KEY (hold to talk, as in Halla)
    // scope: 0 = current channel | 1 = current channel + subchannels | 2 = user list
    void setWhisperHold(bool on, int scope);
    // Reply key (Whisper Lists): target = whoever whispered last
    void setWhisperReplyHold(bool on);
    QList<int> whisperTargetIds(int scope) const;
    bool whisperHoldActive() const { return m_whisperHold; }

    void toggleCommander();               // v3.11: menu "Si mesmo"

    void setAway(bool on);
    void setMicMuted(bool on);
    void setSpeakersMuted(bool on);
    bool isAway() const        { return m_data.users[m_data.selfId].away; }
    bool isMicMuted() const    { return m_data.users[m_data.selfId].inputMuted; }
    bool isSpkMuted() const    { return m_data.users[m_data.selfId].outputMuted; }

    // Controle local exposto de forma segura ao SDK de complementos.
    bool setUserLocallyMuted(int userId, bool muted);
    bool setUserVolumeDb(int userId, int volumeDb);

    void applyDisplayOptions();

signals:
    void selectionChanged(int kind, int id);
    void statusChanged();                      // estados (mudo/ausente) mudaram
    void titleChanged();
    void disconnectRequested();
    void addBookmarkRequested();

private:
    ServerData m_data;
    ServerTreeWidget* m_tree = nullptr;
    ChatPanel* m_chat = nullptr;
    InfoPanel* m_info = nullptr;
    QSplitter* m_split = nullptr;   // vertical: corpo (tree|info) / chat
    QSplitter* m_hsplit = nullptr;  // horizontal: tree 50% | information 50%
    QToolButton* m_serverHeaderName = nullptr;
    QLabel* m_serverIntroTitle = nullptr;
    QLabel* m_serverIntroText = nullptr;
    NetSession* m_net = nullptr;
    VoiceEngine* m_voice = nullptr;

    QSet<int> m_knownUsers;               // join/leave detector (sounds)
    QMap<int, QString> m_lastNames;       // nomes por id (p/ anunciar quem saiu)
    QMap<int, int> m_lastChannels;         // previous channel per client
    QMap<int, bool> m_lastTalking;
    QMap<int, bool> m_lastWhispering;
    int m_myChan = -1;                    // my channel (channel switch sound)
    QVector<OfflineMsgItem> m_offlineInbox;
    QStringList m_whisperUids;
    bool m_whisperHold = false;           // atalho de sussurro pressionado agora
    int m_whisperHoldScope = 2;           // scope of the active whisper key (0/1/2)
    bool m_whisperReplyHold = false;      // hold de sussurro de RESPOSTA (alvo fixo)
    int m_lastWhisperFromId = -1;         // last user who whispered (reply key)
    QList<int> m_lastSentWhisperIds;      // last whisper set actually sent
    bool m_whisperHoldCuePlayed = false;  // whisper cue already played in this hold (does not retrigger with the VAD)
    bool m_stateRefreshPending = false;   // coalesce rajadas de user_state
    // v1.1.35: structural signature (users/channels by id+rev) -- decides between a
    // full rebuild and a lightweight icon update (updateUserVisuals).
    QString m_structSig;

    void hookSignals();
    void refreshServerState();     // estado consolidado (coalesce rajadas de user_state)
    bool hasPermission(const QStringList& keys) const;
    void updatePermissionUi();
    void applyWhisper();                  // maps uids -> ids and sends to the server
    void viewAvatar(int userId);
    void playSpeechCue(bool active);
    // Cue driven by REAL speech detection (voice/continuous modes), not by the
    // transmission state -- the sound only plays when the user speaks.
    void playSpeechCueOnSpeech(bool active);
    // Key-driven whisper cue: the whisper hold has its own button -- the sound plays
    // AS IT IS PRESSED in any voice/continuous mode (it depends neither on the
    // detection threshold opening nor on the selected "Emit on").
    void playWhisperHoldCue();
    void playRemoteSpeechCue(const User& user, bool active);
    void systemMsgServer(const QString& msg);
    void systemMsgChannel(const QString& msg);
    void showServerContextMenu(const QPoint& globalPos);
};
