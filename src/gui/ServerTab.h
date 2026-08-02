#pragma once

#include <QWidget>
#include <QSet>
#include "core/Models.h"
#include "dialogs/AdminDialogs.h"

class ServerTreeWidget;
class ChatPanel;
class InfoPanel;
class QSplitter;
class NetSession;
class VoiceEngine;

// Uma "conexão" do Halla = uma aba do servidor — layout clássico do Halla:
// árvore de canais (50%) | painel de informações (50%) lado a lado em cima,
// e o console de chat ocupando 100% da largura embaixo.
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

    // ---- v3: Halla feature parity
    void setAvatarInteractive();          // escolher imagem e enviar ao servidor
    void removeAvatar();
    void toggleRecording();               // gravação local WAV
    bool isRecording() const;
    void openOfflineMessages();           // caixa de entrada offline
    void setWhisperUids(const QStringList& uids); // vazio = desligar sussurro
    bool whisperActive() const { return !m_whisperUids.isEmpty(); }

    // ---- v3.11: sussurro por TECLA DE ATALHO (segurar para falar, como no Halla)
    // scope: 0 = canal atual | 1 = canal atual + subcanais | 2 = lista de usuários
    void setWhisperHold(bool on, int scope);
    QList<int> whisperTargetIds(int scope) const;
    bool whisperHoldActive() const { return m_whisperHold; }

    void toggleCommander();               // v3.11: menu "Si mesmo"

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
    InfoPanel* m_info = nullptr;
    QSplitter* m_split = nullptr;   // vertical: corpo (tree|info) / chat
    QSplitter* m_hsplit = nullptr;  // horizontal: árvore 50% | informações 50%
    NetSession* m_net = nullptr;
    VoiceEngine* m_voice = nullptr;

    QSet<int> m_knownUsers;               // detector de entrada/saída (sons)
    QMap<int, QString> m_lastNames;       // nomes por id (p/ anunciar quem saiu)
    int m_myChan = -1;                    // meu canal (som de troca de canal)
    QVector<OfflineMsgItem> m_offlineInbox;
    QStringList m_whisperUids;
    bool m_whisperHold = false;           // atalho de sussurro pressionado agora

    void hookSignals();
    void applyWhisper();                  // mapeia uids -> ids e envia ao servidor
    void viewAvatar(int userId);
    void systemMsgServer(const QString& msg);
    void systemMsgChannel(const QString& msg);
};
