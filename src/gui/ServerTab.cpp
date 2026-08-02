#include "ServerTab.h"
#include "ServerTreeWidget.h"
#include "ChatPanel.h"
#include "Settings.h"
#include "AppLog.h"
#include "dialogs/ChannelDialog.h"
#include "dialogs/MiniDialogs.h"
#include "net/NetSession.h"
#include "net/VoiceEngine.h"

#include <QVBoxLayout>
#include <QSplitter>
#include <QMessageBox>
#include <QInputDialog>
#include <QApplication>
#include <QJsonObject>
#include <algorithm>

// ServerData::Channel -> JSON do protocolo
static QJsonObject chanToJson(const Channel& c) {
    QJsonObject o;
    o["id"] = c.id;
    o["parent"] = c.parentId;
    o["name"] = c.name;
    o["topic"] = c.topic;
    o["desc"] = c.description;
    o["pass"] = c.passwordHash;
    o["type"] = c.type;
    o["moderated"] = c.moderated;
    o["codec"] = c.codec;
    o["quality"] = c.codecQuality;
    o["max"] = c.maxClients;
    return o;
}

ServerTab::ServerTab(const ServerData& initial, QWidget* parent)
    : QWidget(parent), m_data(initial) {
    QVBoxLayout* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);

    m_split = new QSplitter(Qt::Vertical, this);
    m_split->setChildrenCollapsible(false);

    m_tree = new ServerTreeWidget(m_split);
    m_tree->setServerData(&m_data);
    m_split->addWidget(m_tree);

    m_chat = new ChatPanel(m_split);
    m_chat->setSelfName(m_data.users[m_data.selfId].name);
    m_split->addWidget(m_chat);
    m_split->setStretchFactor(0, 1);
    m_split->setStretchFactor(1, 0);
    m_split->setSizes({ 500, 190 });

    lay->addWidget(m_split);

    applyDisplayOptions();
    hookSignals();
    m_tree->rebuild();
}

QString ServerTab::tabTitle() const { return m_data.name; }

// ============================================================ modo de rede
void ServerTab::attachNetwork(NetSession* net) {
    m_net = net;
    net->attachTo(&m_data);

    // estado vindo do servidor -> redesenha a árvore/informações
    connect(net, &NetSession::stateChanged, this, [this] {
        m_tree->rebuild();
        emit statusChanged();
    });

    connect(net, &NetSession::chatReceived, this,
            [this](const QString& scope, int fromId, const QString& fromName, const QString& text) {
                if (scope == "server") {
                    m_chat->addServerChat(fromName, text);
                } else if (scope == "private") {
                    m_chat->addPrivateTab(fromId, fromName);
                    m_chat->addPrivateChat(fromId, fromName, text);
                    if (S::flag("notify/messageSound", true)) QApplication::beep();
                } else {
                    m_chat->addChannelChat(fromName, text);
                }
            });

    connect(net, &NetSession::systemEvent, this, [this](const QString& text) {
        systemMsgServer(text);
    });

    connect(net, &NetSession::pokeReceived, this,
            [this](const QString& fromName, const QString& msg) {
                systemMsgServer(tr("Você foi cutucado por %1: %2").arg(fromName, msg));
                if (S::flag("notify/pokeSound", true)) QApplication::beep();
            });

    connect(net, &NetSession::errorOccurred, this,
            [this](const QString& code, const QString& msg) {
                Q_UNUSED(code);
                systemMsgServer(tr("Erro do servidor: %1").arg(msg));
            });

    connect(net, &NetSession::kickedReceived, this,
            [this](const QString& reason, bool ban, int minutes) {
                if (ban) {
                    QMessageBox::warning(this, tr("Você foi banido"),
                        tr("Você foi banido deste servidor%1%2.")
                            .arg(reason.isEmpty() ? QString() : tr(".\nMotivo: %1").arg(reason))
                            .arg(minutes > 0 ? tr("\nDuração: %1 minutos").arg(minutes)
                                             : QString()));
                } else if (!reason.isEmpty()) {
                    systemMsgServer(tr("Você foi expulso%1").arg(tr(": %1").arg(reason)));
                }
            });

    // ---- voz
    m_voice = new VoiceEngine(net, &m_data, this);
    if (m_voice->isActive()) {
        connect(m_voice, &VoiceEngine::talkingChanged, this, [this](bool on) {
            m_data.users[m_data.selfId].talking = on;
            m_tree->rebuild();
        });
        emit statusChanged();
    }
}

void ServerTab::applyDisplayOptions() {
    m_tree->setShowCounts(S::flag("design/showCounts", true));
    m_tree->setShowMinis(S::flag("design/showMinis", true));
    m_tree->rebuild();
}

void ServerTab::hookSignals() {
    connect(m_tree, &ServerTreeWidget::selectionChanged,
            this, &ServerTab::selectionChanged);
    connect(m_tree, &ServerTreeWidget::joinChannelRequested,
            this, &ServerTab::joinChannel);
    connect(m_tree, &ServerTreeWidget::createChannelRequested,
            this, &ServerTab::createChannel);
    connect(m_tree, &ServerTreeWidget::editChannelRequested,
            this, &ServerTab::editChannel);
    connect(m_tree, &ServerTreeWidget::deleteChannelRequested,
            this, &ServerTab::deleteChannel);
    connect(m_tree, &ServerTreeWidget::renameRequested,
            this, &ServerTab::renameSelf);
    connect(m_tree, &ServerTreeWidget::setDescriptionRequested,
            this, &ServerTab::setSelfDescription);
    connect(m_tree, &ServerTreeWidget::editVirtualServerRequested,
            this, &ServerTab::editVirtualServerName);
    connect(m_tree, &ServerTreeWidget::disconnectRequested,
            this, &ServerTab::disconnectRequested);
    connect(m_tree, &ServerTreeWidget::addBookmarkRequested,
            this, &ServerTab::addBookmarkRequested);

    connect(m_tree, &ServerTreeWidget::viewAvatarRequested, this, [this] {
        QMessageBox::information(this, tr("Avatar"),
                                 tr("Nenhum avatar definido para este cliente."));
    });

    connect(m_tree, &ServerTreeWidget::localMuteToggled, this, [this](int uid, bool muted) {
        if (!m_data.users.contains(uid)) return;
        m_data.users[uid].locallyMuted = muted;
        systemMsgChannel(muted
            ? tr("Cliente \"%1\" foi silenciado localmente.").arg(m_data.users[uid].name)
            : tr("Cliente \"%1\" deixou de ser silenciado.").arg(m_data.users[uid].name));
        m_tree->rebuild();
        emit statusChanged();
    });

    connect(m_tree, &ServerTreeWidget::commanderToggled, this, [this] {
        User& self = m_data.users[m_data.selfId];
        self.commander = !self.commander;
        if (m_net) { m_net->sendStatus(); m_tree->rebuild(); emit statusChanged(); return; }
        systemMsgChannel(self.commander
            ? tr("Você agora é o comandante do canal.")
            : tr("Você não é mais o comandante do canal."));
        emit statusChanged();
    });

    connect(m_tree, &ServerTreeWidget::volumeRequested, this, [this](int uid) {
        if (!m_data.users.contains(uid)) return;
        VolumeDialog dlg(m_data.users[uid].name, m_data.users[uid].volumeDb, this);
        if (dlg.exec() == QDialog::Accepted) {
            m_data.users[uid].volumeDb = dlg.volume();
            systemMsgChannel(tr("Volume de \"%1\" definido para %2 dB.")
                                 .arg(m_data.users[uid].name).arg(dlg.volume()));
            emit statusChanged();
        }
    });

    auto kickBan = [this](int uid, bool fromServer, bool isBan) {
        if (!m_data.users.contains(uid)) return;
        KickBanDialog dlg(m_data.users[uid].name,
                          isBan ? KickBanDialog::Ban
                                : (fromServer ? KickBanDialog::KickServer
                                              : KickBanDialog::KickChannel), this);
        if (dlg.exec() != QDialog::Accepted) return;
        if (m_net) {
            if (isBan) m_net->ban(uid, dlg.reason(), dlg.banMinutes());
            else       m_net->kick(uid, fromServer, dlg.reason());
            return;
        }
        User& self = m_data.users[m_data.selfId];
        if (uid == self.id) {
            systemMsgServer(tr("Você foi %1 do servidor.").arg(isBan ? tr("banido")
                                                                     : tr("expulso")));
            emit disconnectRequested();
        } else {
            // remove o cliente do canal (comportamento local do cliente)
            for (Channel& c : m_data.channels) c.users.removeAll(uid);
            m_data.users.remove(uid);
            systemMsgServer(isBan ? tr("Cliente banido do servidor.")
                                  : tr("Cliente expulso do servidor."));
            m_tree->rebuild();
            emit statusChanged();
        }
    };

    connect(m_tree, &ServerTreeWidget::kickRequested, this,
            [this, kickBan](int uid, bool fromServer) { kickBan(uid, fromServer, false); });
    connect(m_tree, &ServerTreeWidget::banRequested, this,
            [this, kickBan](int uid) { kickBan(uid, true, true); });

    connect(m_tree, &ServerTreeWidget::pokeRequested, this, [this](int uid) {
        if (!m_data.users.contains(uid)) return;
        PokeDialog dlg(m_data.users[uid].name, this);
        if (dlg.exec() == QDialog::Accepted) {
            if (m_net) { m_net->poke(uid, dlg.message()); return; }
            systemMsgChannel(tr("Você cutucou \"%1\": %2")
                                 .arg(m_data.users[uid].name, dlg.message()));
            if (S::flag("notify/pokeSound", true)) QApplication::beep();
        }
    });

    connect(m_tree, &ServerTreeWidget::moveToMyChannelRequested, this, [this](int uid) {
        if (!m_data.users.contains(uid)) return;
        if (m_net) {
            systemMsgServer(tr("'Mover para o seu canal' requer permissões elevadas "
                               "nesta versão do protocolo."));
            return;
        }
        const int myChan = m_data.channelOfUser(m_data.selfId);
        for (Channel& c : m_data.channels) c.users.removeAll(uid);
        m_data.channels[myChan].users << uid;
        m_tree->rebuild();
    });

    connect(m_tree, &ServerTreeWidget::privateMessageRequested, this, [this](int uid) {
        if (!m_data.users.contains(uid)) return;
        m_chat->addPrivateTab(uid, m_data.users[uid].name);
    });

    connect(m_chat, &ChatPanel::messageSent, this,
            [this](const QString& target, int targetId, const QString& text) {
                if (m_net) {
                    m_net->sendChat(target, targetId, text);
                    return;
                }
                const QString& me = m_data.users[m_data.selfId].name;
                if (target == "server") {
                    m_chat->addServerChat(me, text);
                } else if (target == "private") {
                    m_chat->addPrivateChat(targetId, me, text);
                } else {
                    m_chat->addChannelChat(me, text);
                }
            });
}

void ServerTab::systemMsgServer(const QString& msg)  { m_chat->addServerSystem(msg); }
void ServerTab::systemMsgChannel(const QString& msg) { m_chat->addChannelSystem(msg); }

void ServerTab::joinChannel(int channelId) {
    if (!m_data.channels.contains(channelId)) return;
    User& self = m_data.users[m_data.selfId];
    const int oldChan = m_data.channelOfUser(self.id);
    if (oldChan == channelId) return;

    // modo de rede: pergunta a senha (se houver) e envia ao servidor
    if (m_net) {
        Channel& c = m_data.channels[channelId];
        QString pass;
        if (c.hasPassword) {
            bool ok = false;
            pass = QInputDialog::getText(this, tr("Senha do canal"),
                                         tr("O canal \"%1\" é protegido por senha.\nDigite a senha:")
                                             .arg(c.name),
                                         QLineEdit::Password, QString(), &ok);
            if (!ok) return;
        }
        m_net->moveToChannel(channelId, pass);
        return;
    }

    Channel& c = m_data.channels[channelId];
    if (c.maxClients >= 0 && c.users.size() >= c.maxClients) {
        QMessageBox::warning(this, tr("Canal cheio"),
                             tr("O canal \"%1\" está cheio.").arg(c.name));
        return;
    }
    if (c.hasPassword) {
        bool ok = false;
        QString pw = QInputDialog::getText(this, tr("Senha do canal"),
                                           tr("O canal \"%1\" é protegido por senha.\nDigite a senha:")
                                               .arg(c.name),
                                           QLineEdit::Password, QString(), &ok);
        if (!ok) return;
        if (pw != c.passwordHash) {
            QMessageBox::warning(this, tr("Senha inválida"),
                                 tr("Senha do canal incorreta."));
            return;
        }
    }

    if (m_data.channels.contains(oldChan))
        m_data.channels[oldChan].users.removeAll(self.id);
    m_data.channels[channelId].users << self.id;

    systemMsgChannel(tr("Você entrou no canal \"%1\".").arg(c.name));
    AppLog::info(tr("Entrou no canal \"%1\"").arg(c.name));
    m_tree->rebuild();
    emit statusChanged();
}

void ServerTab::createChannel(int parentId) {
    ChannelDialog dlg(tr("Criar canal"), &m_data, this);
    if (dlg.exec() != QDialog::Accepted) return;

    Channel c = dlg.resultChannel();
    if (m_net) {
        c.parentId = parentId;
        m_net->createChannel(chanToJson(c));
        return;
    }

    c.id = m_data.nextChannelId++;
    c.parentId = parentId;
    m_data.channels[c.id] = c;
    systemMsgServer(tr("Canal \"%1\" foi criado.").arg(c.name));
    AppLog::info(tr("Canal \"%1\" criado").arg(c.name));
    m_tree->rebuild();
    emit statusChanged();
}

void ServerTab::editChannel(int channelId) {
    if (!m_data.channels.contains(channelId)) return;
    ChannelDialog dlg(tr("Editar canal"), &m_data, this);
    dlg.setChannel(m_data.channels[channelId]);
    if (dlg.exec() != QDialog::Accepted) return;

    Channel c = dlg.resultChannel();
    if (m_net) {
        QJsonObject o = chanToJson(c);
        o["id"] = channelId;
        o["parent"] = m_data.channels[channelId].parentId;
        m_net->editChannel(o);
        return;
    }

    c.id = channelId;
    c.parentId = m_data.channels[channelId].parentId;
    c.users = m_data.channels[channelId].users;
    m_data.channels[channelId] = c;
    AppLog::info(tr("Canal \"%1\" editado").arg(c.name));
    m_tree->rebuild();
    emit statusChanged();
}

void ServerTab::deleteChannel(int channelId) {
    if (!m_data.channels.contains(channelId)) return;
    const Channel c = m_data.channels[channelId];

    if (c.users.contains(m_data.selfId)) {
        QMessageBox::warning(this, tr("Excluir canal"),
                             tr("Você não pode excluir o canal em que está."));
        return;
    }
    bool hasChildren = std::any_of(m_data.channels.begin(), m_data.channels.end(),
                                   [&](const Channel& ch) { return ch.parentId == channelId; });
    if (hasChildren) {
        QMessageBox::warning(this, tr("Excluir canal"),
                             tr("Exclua primeiro os sub-canais de \"%1\".").arg(c.name));
        return;
    }

    if (QMessageBox::question(this, tr("Excluir canal"),
                              tr("Deseja realmente excluir o canal \"%1\"?").arg(c.name))
        != QMessageBox::Yes)
        return;

    if (m_net) {
        m_net->deleteChannel(channelId);
        return;
    }

    for (int uid : c.users)
        if (m_data.users.contains(uid)) {
            // move usuários para o canal padrão
            for (Channel& ch : m_data.channels)
                if (ch.isDefault) { ch.users << uid; break; }
        }
    m_data.channels.remove(channelId);
    systemMsgServer(tr("Canal \"%1\" foi excluído.").arg(c.name));
    AppLog::info(tr("Canal \"%1\" excluído").arg(c.name));
    m_tree->rebuild();
    emit statusChanged();
}

void ServerTab::renameSelf() {
    User& self = m_data.users[m_data.selfId];
    bool ok = false;
    QString name = QInputDialog::getText(this, tr("Alterar apelido"),
                                         tr("Novo apelido:"), QLineEdit::Normal,
                                         self.name, &ok);
    if (!ok || name.trimmed().isEmpty()) return;
    name = name.trimmed().left(30);
    if (m_net) {
        m_net->rename(name);
        return;
    }
    systemMsgServer(tr("Você alterou o apelido de \"%1\" para \"%2\".").arg(self.name, name));
    AppLog::info(tr("Apelido alterado para \"%1\"").arg(name));
    self.name = name;
    m_chat->setSelfName(name);
    m_tree->rebuild();
    emit statusChanged();
}

void ServerTab::setSelfDescription() {
    User& self = m_data.users[m_data.selfId];
    bool ok = false;
    QString desc = QInputDialog::getMultiLineText(this, tr("Descrição do cliente"),
                                                  tr("Descrição:"), self.description, &ok);
    if (!ok) return;
    if (m_net) {
        m_net->setDescription(desc);
        return;
    }
    self.description = desc;
    m_tree->rebuild();
    emit statusChanged();
}

void ServerTab::editVirtualServerName() {
    bool ok = false;
    QString name = QInputDialog::getText(this, tr("Editar servidor virtual"),
                                         tr("Nome do servidor:"), QLineEdit::Normal,
                                         m_data.name, &ok);
    if (!ok || name.trimmed().isEmpty()) return;
    m_data.name = name.trimmed();
    systemMsgServer(tr("Servidor renomeado para \"%1\".").arg(m_data.name));
    m_tree->rebuild();
    emit titleChanged();
    emit statusChanged();
}

void ServerTab::setAway(bool on) {
    User& self = m_data.users[m_data.selfId];
    if (self.away == on) return;
    self.away = on;
    if (m_net) { m_net->sendStatus(); m_tree->rebuild(); emit statusChanged(); return; }
    systemMsgChannel(on ? tr("Você está ausente agora.") : tr("Você não está mais ausente."));
    AppLog::info(on ? tr("Estado: ausente") : tr("Estado: de volta"));
    m_tree->rebuild();
    emit statusChanged();
}

void ServerTab::setMicMuted(bool on) {
    User& self = m_data.users[m_data.selfId];
    if (self.inputMuted == on) return;
    self.inputMuted = on;
    if (m_voice) m_voice->setTransmitEnabled(!on);
    if (m_net) { m_net->sendStatus(); m_tree->rebuild(); emit statusChanged(); return; }
    systemMsgChannel(on ? tr("Microfone mudo ativado.") : tr("Microfone mudo desativado."));
    m_tree->rebuild();
    emit statusChanged();
}

void ServerTab::setSpeakersMuted(bool on) {
    User& self = m_data.users[m_data.selfId];
    if (self.outputMuted == on) return;
    self.outputMuted = on;
    if (m_voice) m_voice->setSpeakersEnabled(!on);
    if (m_net) { m_net->sendStatus(); m_tree->rebuild(); emit statusChanged(); return; }
    systemMsgChannel(on ? tr("Alto-falantes mudos.") : tr("Alto-falantes reativados."));
    m_tree->rebuild();
    emit statusChanged();
}
