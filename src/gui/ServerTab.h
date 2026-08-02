#pragma once

#include <QWidget>
#include <QSet>
#include "core/Models.h"
#include "dialogs/AdminDialogs.h"

class ServerTreeWidget;
class ChatPanel;
class QSplitter;
class NetSession;
class VoiceEngine;

// Uma "conexão" do Halla = uma aba do servidor com árvore + chat (visual do TS3)
class ServerTab : public QWidget {
    Q_OBJECT
public:
    explicit ServerTab(const ServerData& initial, QWidget* parent = nullptr);

    ServerData& data() { return m_data; }
    const ServerData& data() const { return m_data; }
    ServerTreeWidget* tree() const { return m_tree; }
    ChatPanel* chat() const { return m_chat; }
    NetSession* net() const { return m_net; }
    VoiceEngine* voice() const { return m_voice; }
    bool isNetworked() const { return m_net != nullptr; }

    // conecta a aba a uma sessão de rede (modo conectado ao Halla Server)
    void attachNetwork(NetSession* net);

    QString tabTitle() const;

    // operações reais do lado do cliente
    void joinChannel(int channelId);
    void createChannel(int parentId);
    void editChannel(int channelId);
    void deleteChannel(int channelId);
    void renameSelf();
    void setSelfDescription();
    void editVirtualServerName();

    // ---- v3: TS3 feature parity
    void setAvatarInteractive();          // escolher imagem e enviar ao servidor
    void removeAvatar();
    void toggleRecording();               // gravação local WAV
    bool isRecording() const;
    void openOfflineMessages();           // caixa de entrada offline
    void setWhisperUids(const QStringList& uids); // vazio = desligar sussurro
    bool whisperActive() const { return !m_whisperUids.isEmpty(); }

    void setAway(bool on);
    void setMicMuted(bool on);
    void setSpeakersMuted(bool on);
    bool isAway() const        { return m_data.users[m_data.selfId].away; }
    bool isMicMuted() const    { return m_data.users[m_data.selfId].inputMuted; }
    bool isSpkMuted() const    { return m_data.users[m_data.selfId].outputMuted; }

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
    QSplitter* m_split = nullptr;
    NetSession* m_net = nullptr;
    VoiceEngine* m_voice = nullptr;

    QSet<int> m_knownUsers;               // detector de entrada/saída (sons)
    QMap<int, QString> m_lastNames;       // nomes por id (p/ anunciar quem saiu)
    int m_myChan = -1;                    // meu canal (som de troca de canal)
    QVector<OfflineMsgItem> m_offlineInbox;
    QStringList m_whisperUids;

    void hookSignals();
    void applyWhisper();                  // mapeia uids -> ids e envia ao servidor
    void viewAvatar(int userId);
    void systemMsgServer(const QString& msg);
    void systemMsgChannel(const QString& msg);
};
