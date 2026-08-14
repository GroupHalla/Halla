#include "ServerTab.h"
#include "ServerTreeWidget.h"
#include "ChatPanel.h"
#include "Icons.h"
#include "Settings.h"
#include "AppLog.h"
#include "SoundPack.h"
#include "Speech.h"
#include "dialogs/ChannelDialog.h"
#include "dialogs/MiniDialogs.h"
#include "dialogs/ToolsDialogs.h"
#include "InfoPanel.h"
#include "RichTextBrowser.h"
#include "net/NetSession.h"
#include "net/VoiceEngine.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QSplitter>
#include <QFrame>
#include <QToolButton>
#include <QPushButton>
#include <QMessageBox>
#include <QInputDialog>
#include <QApplication>
#include <QJsonObject>
#include <QJsonArray>
#include <QFileDialog>
#include <QImage>
#include <QBuffer>
#include <QStandardPaths>
#include <QDateTime>
#include <QDir>
#include <QLabel>
#include <QDialogButtonBox>
#include <QTextBrowser>
#include <QLineEdit>
#include <algorithm>
#include <utility>

// ServerData::Channel -> JSON do protocolo
static QJsonObject chanToJson(const Channel& c) {
    QJsonObject o;
    o["id"] = c.id;
    o["parent"] = c.parentId;
    o["order"] = c.order;
    o["name"] = c.name;
    o["topic"] = c.topic;
    o["desc"] = c.description;
    o["pass"] = c.passwordHash;
    o["noSymbol"] = c.noSymbol;
    o["type"] = c.type;
    o["moderated"] = c.moderated;
    o["codec"] = c.codec;
    o["quality"] = c.codecQuality;
    o["bitrate"] = c.bitrate;
    o["groupPerms"] = c.groupPerms;
    o["max"] = c.maxClients;
    QJsonArray linked;
    for (int id : c.linkedChannels) linked << id;
    o["linked"] = linked;
    return o;
}

ServerTab::ServerTab(const ServerData& initial, QWidget* parent)
    : QWidget(parent), m_data(initial) {
    QVBoxLayout* lay = new QVBoxLayout(this);
    lay->setContentsMargins(16, 4, 16, 12);
    lay->setSpacing(0);

    // A composição principal replica a referência: dois cartões flutuantes
    // no alto e um cartão de chat ocupando toda a largura abaixo.
    m_split = new QSplitter(Qt::Vertical, this);
    m_split->setObjectName(QStringLiteral("bodySplitter"));
    m_split->setChildrenCollapsible(false);
    m_split->setHandleWidth(12);

    m_hsplit = new QSplitter(Qt::Horizontal, m_split);
    m_hsplit->setObjectName(QStringLiteral("topSplitter"));
    m_hsplit->setChildrenCollapsible(false);
    m_hsplit->setHandleWidth(12);

    QFrame* treeCard = new QFrame(m_hsplit);
    treeCard->setObjectName(QStringLiteral("panelCard"));
    QVBoxLayout* treeLayout = new QVBoxLayout(treeCard);
    treeLayout->setContentsMargins(8, 7, 8, 8);
    treeLayout->setSpacing(2);

    QFrame* treeHeader = new QFrame(treeCard);
    treeHeader->setObjectName(QStringLiteral("treeCardHeader"));
    treeHeader->setMinimumHeight(38);
    QHBoxLayout* headerLayout = new QHBoxLayout(treeHeader);
    headerLayout->setContentsMargins(8, 2, 5, 2);
    headerLayout->setSpacing(8);
    QLabel* serverIcon = new QLabel(treeHeader);
    serverIcon->setPixmap(HIcons::server().pixmap(18, 18));
    headerLayout->addWidget(serverIcon);
    m_serverHeaderName = new QToolButton(treeHeader);
    m_serverHeaderName->setObjectName(QStringLiteral("serverHeaderName"));
    m_serverHeaderName->setText(m_data.name);
    m_serverHeaderName->setToolButtonStyle(Qt::ToolButtonTextOnly);
    m_serverHeaderName->setAutoRaise(true);
    m_serverHeaderName->setCursor(Qt::PointingHandCursor);
    m_serverHeaderName->setToolTip(tr("Mostrar descrição e informações do servidor"));
    headerLayout->addWidget(m_serverHeaderName);
    QLabel* dot = new QLabel(QStringLiteral("●"), treeHeader);
    dot->setObjectName(QStringLiteral("statusDot"));
    headerLayout->addWidget(dot);
    headerLayout->addStretch(1);
    QToolButton* star = new QToolButton(treeHeader);
    star->setObjectName(QStringLiteral("headerIconButton"));
    star->setText(QStringLiteral("☆"));
    star->setToolTip(tr("Adicionar aos favoritos"));
    connect(star, &QToolButton::clicked, this, [this] { emit addBookmarkRequested(); });
    headerLayout->addWidget(star);
    QToolButton* more = new QToolButton(treeHeader);
    more->setObjectName(QStringLiteral("headerIconButton"));
    more->setText(QStringLiteral("…"));
    more->setToolTip(tr("Opções do servidor"));
    headerLayout->addWidget(more);
    treeLayout->addWidget(treeHeader);

    // O cabeçalho mostra somente o nome do servidor. A descrição e as
    // informações ficam no painel ao lado quando o nome é clicado.


    m_tree = new ServerTreeWidget(treeCard);
    m_tree->setServerData(&m_data);
    treeLayout->addWidget(m_tree, 1);
    m_hsplit->addWidget(treeCard);

    QFrame* infoCard = new QFrame(m_hsplit);
    infoCard->setObjectName(QStringLiteral("panelCard"));
    QVBoxLayout* infoLayout = new QVBoxLayout(infoCard);
    infoLayout->setContentsMargins(0, 0, 0, 0);
    infoLayout->setSpacing(0);
    m_info = new InfoPanel(infoCard);
    m_info->setData(&m_data);
    infoLayout->addWidget(m_info);
    connect(m_serverHeaderName, &QToolButton::clicked, this, [this] {
        m_info->setSelection(0, 0);
    });
    m_hsplit->addWidget(infoCard);
    m_hsplit->setStretchFactor(0, 47);
    m_hsplit->setStretchFactor(1, 53);
    m_hsplit->setSizes({ 850, 850 });
    m_split->addWidget(m_hsplit);

    QFrame* chatCard = new QFrame(m_split);
    chatCard->setObjectName(QStringLiteral("chatCard"));
    QVBoxLayout* chatLayout = new QVBoxLayout(chatCard);
    chatLayout->setContentsMargins(8, 3, 8, 8);
    chatLayout->setSpacing(0);
    m_chat = new ChatPanel(chatCard);
    m_chat->setSelfName(m_data.users[m_data.selfId].name);
    chatLayout->addWidget(m_chat);
    m_split->addWidget(chatCard);
    m_split->setStretchFactor(0, 1);
    m_split->setStretchFactor(1, 0);
    m_split->setSizes({ 520, 165 });

    const QByteArray hv = QByteArray::fromBase64(S::str("design/splitVertical").toUtf8());
    if (!hv.isEmpty()) m_split->restoreState(hv);
    const QByteArray hh = QByteArray::fromBase64(S::str("design/splitHorizontal").toUtf8());
    if (!hh.isEmpty()) m_hsplit->restoreState(hh);
    connect(m_split, &QSplitter::splitterMoved, this, [this] {
        S::set("design/splitVertical", QString::fromLatin1(m_split->saveState().toBase64()));
    });
    connect(m_hsplit, &QSplitter::splitterMoved, this, [this] {
        S::set("design/splitHorizontal", QString::fromLatin1(m_hsplit->saveState().toBase64()));
    });

    lay->addWidget(m_split);

    connect(m_tree, &ServerTreeWidget::selectionChanged, this,
            [this](int kind, int id) { m_info->setSelection(kind, id); });
    connect(this, &ServerTab::statusChanged, this, [this] {
        m_serverHeaderName->setText(m_data.name);
        if (m_serverIntroTitle) m_serverIntroTitle->setText(tr("🎧  Bem-vindo ao Halla"));
        if (m_serverIntroText) m_serverIntroText->setText(m_data.motd);
        m_info->refresh();
    });
    m_info->setSelection(0, 0);

    applyDisplayOptions();
    hookSignals();
    m_tree->rebuild();
}

QString ServerTab::tabTitle() const { return m_data.name; }

// ============================================================ modo de rede
void ServerTab::attachNetwork(NetSession* net) {
    m_net = net;
    net->attachTo(&m_data);
    updatePermissionUi();

    // usuários já presentes no login (não tocar som para eles)
    for (const User& u : m_data.users) {
        m_knownUsers << u.id;
        m_lastChannels[u.id] = m_data.channelOfUser(u.id);
        m_lastTalking[u.id] = u.talking;
        m_lastWhispering[u.id] = u.whispering;
    }
    m_myChan = m_data.channelOfUser(m_data.selfId);

    connect(net, &NetSession::iconDataReceived, this, [this](const QString& name, const QByteArray& bytes) {
        QDir().mkpath(QStringLiteral("cache/icons"));
        QFile f(QStringLiteral("cache/icons/") + name);
        if (f.open(QIODevice::WriteOnly)) {
            f.write(bytes);
            f.close();
            m_tree->viewport()->update();
        }
    });

    // estado vindo do servidor -> redesenha a árvore/informações
    connect(net, &NetSession::stateChanged, this, [this] {
        // Detecta entradas e saídas especificamente no canal atual do usuário.
        // Usuários que entram em outros canais não disparam o aviso da call.
        QSet<int> now;
        QMap<int, int> currentChannels;
        for (const User& u : m_data.users) {
            now << u.id;
            currentChannels[u.id] = m_data.channelOfUser(u.id);
            m_lastNames[u.id] = u.name;
            if (u.id != m_data.selfId) {
                const bool wasTalking = m_lastTalking.value(u.id, false);
                const bool wasWhispering = m_lastWhispering.value(u.id, false);
                if (u.talking && (!wasTalking || (u.whispering && !wasWhispering))) {
                    playRemoteSpeechCue(u, true);
                } else if (!u.talking && wasTalking) {
                    playRemoteSpeechCue(u, false);
                }
                m_lastTalking[u.id] = u.talking;
                m_lastWhispering[u.id] = u.whispering;
            }
        }

        const int myChan = currentChannels.value(m_data.selfId, m_data.channelOfUser(m_data.selfId));
        const bool selfChangedChannel = m_myChan >= 0 && myChan != m_myChan;
        if (!selfChangedChannel && myChan > 0) {
            for (int id : now) {
                if (id == m_data.selfId) continue;
                const int current = currentChannels.value(id, 0);
                if (!m_knownUsers.contains(id)) {
                    if (current == myChan) {
                        if (S::flag("notify/userJoinSound", true))
                            HSound::play(QStringLiteral("user_joined"));
                        HSpeech::say(tr("%1 entrou no seu canal").arg(m_lastNames.value(id)));
                    }
                    continue;
                }
                const int previous = m_lastChannels.value(id, current);
                if (previous == current) continue;
                if (current == myChan) {
                    if (S::flag("notify/userJoinSound", true))
                        HSound::play(QStringLiteral("user_joined"));
                    HSpeech::say(tr("%1 entrou no seu canal").arg(m_lastNames.value(id)));
                } else if (previous == myChan) {
                    if (S::flag("notify/userLeftSound", true))
                        HSound::play(QStringLiteral("user_left"));
                    HSpeech::say(tr("%1 saiu do seu canal").arg(m_lastNames.value(id)));
                }
            }
            for (int id : m_knownUsers) {
                if (id == m_data.selfId || now.contains(id)) continue;
                if (m_lastChannels.value(id, 0) == myChan) {
                    if (S::flag("notify/userLeftSound", true))
                        HSound::play(QStringLiteral("user_left"));
                    HSpeech::say(tr("%1 saiu do seu canal").arg(m_lastNames.value(id)));
                }
            }
        }

        // A própria troca de canal usa um único aviso, sem tocar uma vez para
        // cada pessoa que já estava no destino.
        if (selfChangedChannel && S::flag("notify/channelSwitchSound", true))
            HSound::play(QStringLiteral("moved"));
        m_knownUsers = now;
        m_lastChannels = currentChannels;
        m_myChan = myChan;

        applyWhisper(); // mantém o alvo do sussurro sincronizado (ids mudam a cada login)
        updatePermissionUi();

        m_tree->rebuild();
        m_info->refresh();
        emit statusChanged();
    });

    connect(net, &NetSession::chatReceived, this,
            [this](const QString& scope, int fromId, const QString& fromName, const QString& text) {
                if (scope == "server") {
                    m_chat->addServerChat(fromName, text);
                } else if (scope == "private") {
                    m_chat->addPrivateTab(fromId, fromName);
                    m_chat->addPrivateChat(fromId, fromName, text);
                    if (S::flag("notify/messageSound", true)) HSound::play(QStringLiteral("message"));
                    HSpeech::say(tr("Mensagem de %1").arg(fromName));
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
                if (S::flag("notify/pokeSound", true)) HSound::play(QStringLiteral("poke"));
                HSpeech::say(tr("Cutucada de %1").arg(fromName));
            });

    // ---- v3: caixa de entrada offline (mensagens deixadas enquanto ausente)
    connect(net, &NetSession::offlineMsgReceived, this,
            [this](const QString& fromName, const QString& text, const QString& ts) {
                m_offlineInbox << OfflineMsgItem{fromName, text, ts};
                systemMsgServer(tr("Mensagem offline de %1 — abra Ferramentas > Mensagens offline.")
                                    .arg(fromName));
                if (S::flag("notify/messageSound", true)) HSound::play(QStringLiteral("message"));
            });

    connect(net, &NetSession::errorOccurred, this,
            [this](const QString& code, const QString& msg) {
                systemMsgServer(tr("Erro do servidor: %1").arg(msg));
                static const QSet<QString> permissionErrors = {
                    QStringLiteral("no_permission"), QStringLiteral("hierarchy"),
                    QStringLiteral("locked"), QStringLiteral("no_talk_power")
                };
                HSound::play(permissionErrors.contains(code)
                    ? QStringLiteral("insufficient_permissions")
                    : QStringLiteral("error"));
                if (code == QStringLiteral("bad_privkey") || code == QStringLiteral("privkey_used")) {
                    QMessageBox::critical(this, tr("Chave de privilégio"), msg);
                }
            });

    connect(net, &NetSession::kickedReceived, this,
            [this](const QString& reason, bool ban, int minutes) {
                HSound::play(ban ? QStringLiteral("banned") : QStringLiteral("kicked"));
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
    // A lista ativa configura destinos; ela não transforma o PTT normal em
    // sussurro. Apenas a tecla de sussurro ativa esse roteamento.
    m_voice = new VoiceEngine(net, &m_data, this);
    if (m_voice->isActive()) {
        connect(m_voice, &VoiceEngine::talkingChanged, this, [this](bool on) {
            m_data.users[m_data.selfId].talking = on;
            m_data.users[m_data.selfId].whispering = on && (m_whisperHold || !m_whisperUids.isEmpty());
            playSpeechCue(on);
            m_tree->rebuild();
        });
        emit statusChanged();
    }
}

bool ServerTab::hasPermission(const QStringList& keys) const {
    if (!m_net) return true; // abas locais/demo mantêm todos os controles disponíveis
    const QJsonObject perms = m_net->myPerms();
    if (perms.value(QStringLiteral("*")).toBool()) return true;
    for (const QString& key : keys) {
        const QJsonValue value = perms.value(key);
        if (value.toBool() || value.toInt(0) > 0) return true;
    }
    return false;
}

void ServerTab::updatePermissionUi() {
    const bool moveOthers = hasPermission({ QStringLiteral("move"),
                                             QStringLiteral("i_client_move_power") });
    const bool selfCommander = hasPermission({ QStringLiteral("selfCommander"),
                                               QStringLiteral("b_client_is_channel_commander"),
                                               QStringLiteral("setCommander"),
                                               QStringLiteral("b_client_set_channel_commander") });
    const bool otherCommander = hasPermission({ QStringLiteral("setCommander"),
                                                QStringLiteral("b_client_set_channel_commander") });
    m_tree->setCanMoveOthers(moveOthers);
    m_tree->setCommanderPermissions(selfCommander, otherCommander);
}

void ServerTab::applyDisplayOptions() {
    updatePermissionUi();
    m_tree->setShowCounts(S::flag("design/showCounts", true));
    m_tree->setShowMinis(S::flag("design/showMinis", true));
    m_tree->setSortClientsBelow(S::flag("design/sortClientsBelow", false));
    m_tree->rebuild();

    // Expansão ao fazer login (Opções → Aparência → Árvore do canal)
    const int mode = S::num("design/expandMode", 0);
    if (mode == 0) {
        m_tree->expandAll();
    } else if (mode == 1) {
        m_tree->expandChannelsToLevel(S::num("design/expandLevel", 0));
    } else {
        m_tree->expandOwnChannelOnly(m_data.channelOfUser(m_data.selfId));
    }
}

void ServerTab::hookSignals() {
    connect(m_tree, &ServerTreeWidget::iconRequested, this, [this](const QString& name) {
        if (m_net) {
            m_net->iconGet(name);
        }
    });
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
    connect(m_tree, &ServerTreeWidget::channelDescriptionRequested, this,
            [this](int channelId) {
                if (!m_data.channels.contains(channelId)) return;
                const Channel& c = m_data.channels[channelId];
                QDialog dlg(this);
                dlg.setWindowTitle(tr("Descrição — %1").arg(c.name));
                dlg.resize(480, 340);
                QVBoxLayout* l = new QVBoxLayout(&dlg);
                RichTextBrowser* view = new RichTextBrowser(&dlg);
                view->setOpenExternalLinks(true);
                view->setHtml(c.description.isEmpty()
                    ? tr("<i>Este canal não tem descrição.</i>")
                    : ChatPanel::bbToHtml(c.description));
                l->addWidget(view, 1);
                QDialogButtonBox* bb = new QDialogButtonBox(QDialogButtonBox::Close, &dlg);
                l->addWidget(bb);
                QObject::connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
                dlg.exec();
            });
    connect(m_tree, &ServerTreeWidget::editVirtualServerRequested,
            this, &ServerTab::editVirtualServerName);
    connect(m_tree, &ServerTreeWidget::disconnectRequested,
            this, &ServerTab::disconnectRequested);
    connect(m_tree, &ServerTreeWidget::addBookmarkRequested,
            this, &ServerTab::addBookmarkRequested);

    connect(m_tree, &ServerTreeWidget::viewAvatarRequested, this,
            [this](int userId) { viewAvatar(userId); });

    connect(m_tree, &ServerTreeWidget::complaintRequested, this, [this](int uid) {
        if (!m_data.users.contains(uid)) return;
        const QString target = m_data.users[uid].name;
        bool ok = false;
        const QString text = QInputDialog::getMultiLineText(
            this, tr("Registrar reclamação"),
            tr("Descreva a reclamação sobre \\\"%1\\\":").arg(target),
            QString(), &ok);
        if (!ok || text.trimmed().isEmpty()) return;
        if (m_net) {
            m_net->complaintAdd(uid, text.trimmed());
            systemMsgServer(tr("Reclamação sobre \\\"%1\\\" foi registrada.").arg(target));
        } else {
            systemMsgServer(tr("Reclamação sobre \\\"%1\\\" foi registrada.").arg(target));
        }
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

    connect(m_tree, &ServerTreeWidget::userInfoRequested, this, [this](int uid) {
        if (m_data.users.contains(uid)) m_info->setSelection(NodeUser, uid);
    });

    connect(m_tree, &ServerTreeWidget::commanderRequested, this,
            [this](int uid, bool on) {
        if (!m_data.users.contains(uid)) return;
        if (m_net) {
            const bool allowed = uid == m_data.selfId
                ? hasPermission({ QStringLiteral("selfCommander"),
                                  QStringLiteral("b_client_is_channel_commander"),
                                  QStringLiteral("setCommander"),
                                  QStringLiteral("b_client_set_channel_commander") })
                : hasPermission({ QStringLiteral("setCommander"),
                                  QStringLiteral("b_client_set_channel_commander") });
            if (!allowed) {
                systemMsgChannel(tr("Você não tem permissão para definir o comandante do canal."));
                return;
            }
            m_net->setCommander(uid, on);
            return;
        }
        m_data.users[uid].commander = on;
        m_tree->rebuild();
        emit statusChanged();
    });

    connect(m_tree, &ServerTreeWidget::moveUserRequested, this,
            [this](int uid, int channelId) {
        if (!m_data.users.contains(uid) || !m_data.channels.contains(channelId)) return;
        if (m_net) {
            if (uid == m_data.selfId) m_net->moveToChannel(channelId);
            else m_net->moveOther(uid, channelId);
            return;
        }
        for (Channel& c : m_data.channels) c.users.removeAll(uid);
        m_data.channels[channelId].users << uid;
        m_tree->rebuild();
        emit statusChanged();
    });

    connect(m_tree, &ServerTreeWidget::channelMoveRequested, this,
            [this](int channelId, int parentId, int order) {
        if (!m_data.channels.contains(channelId)) return;
        if (m_net) {
            m_net->moveChannel(channelId, parentId, order);
            return;
        }
        m_data.channels[channelId].parentId = parentId;
        m_data.channels[channelId].order = order;
        m_tree->rebuild();
        emit statusChanged();
    });

    connect(m_tree, &ServerTreeWidget::channelLinkRequested, this,
            [this](const QList<int>& channelIds, bool link) {
        if (channelIds.size() < 2) return;
        if (m_net) {
            bool canLink = hasPermission({ QStringLiteral("chanEdit") });
            if (!canLink) {
                canLink = std::all_of(channelIds.begin(), channelIds.end(),
                    [this](int id) {
                        return id != 1 && m_data.channels.contains(id)
                            && m_data.channels[id].opUids.contains(
                                   m_data.users.value(m_data.selfId).uniqueId);
                    });
            }
            if (!canLink) {
                QMessageBox::warning(this, tr("Canais vinculados"),
                                     tr("Você não tem permissão para vincular canais."));
                return;
            }
            m_net->linkChannels(channelIds, link);
            return;
        }

        // Abas locais/demo usam a mesma relação simétrica do servidor.
        for (int i = 0; i < channelIds.size(); ++i) {
            if (!m_data.channels.contains(channelIds[i])) continue;
            for (int j = i + 1; j < channelIds.size(); ++j) {
                if (!m_data.channels.contains(channelIds[j])) continue;
                const int a = channelIds[i];
                const int b = channelIds[j];
                if (link) {
                    if (!m_data.channels[a].linkedChannels.contains(b))
                        m_data.channels[a].linkedChannels << b;
                    if (!m_data.channels[b].linkedChannels.contains(a))
                        m_data.channels[b].linkedChannels << a;
                } else {
                    m_data.channels[a].linkedChannels.removeAll(b);
                    m_data.channels[b].linkedChannels.removeAll(a);
                }
            }
        }
        m_tree->rebuild();
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
            if (S::flag("notify/pokeSound", true)) HSound::play(QStringLiteral("poke"));
        }
    });

    connect(m_tree, &ServerTreeWidget::moveToMyChannelRequested, this, [this](int uid) {
        if (!m_data.users.contains(uid)) return;
        if (m_net) {
            // v2: o servidor move quem tem a permissão "move"
            const int myChan = m_data.channelOfUser(m_data.selfId);
            m_net->moveOther(uid, myChan);
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

void ServerTab::playSpeechCue(bool active) {
    if (!S::flag("capture/speechCueEnabled", false)) return;
    const int selectedMode = S::num("capture/speechCueMode", 1);
    const int captureMode = S::num("capture/pttMode", 1);
    if (selectedMode != captureMode) return;

    QString path;
    if (!active) {
        path = S::str("capture/speechCueInactive");
    } else if (m_whisperHold || !m_whisperUids.isEmpty()) {
        path = S::str("capture/speechCueWhisper");
        if (path.isEmpty()) path = S::str("capture/speechCueActive");
    } else {
        path = S::str("capture/speechCueActive");
    }
    HSound::playFile(path);
}

void ServerTab::playRemoteSpeechCue(const User& user, bool active) {
    const QString pathKey = active
        ? (user.whispering ? QStringLiteral("capture/speechCueWhisper")
                           : QStringLiteral("capture/speechCueActive"))
        : QStringLiteral("capture/speechCueInactive");
    const QString remoteKey = active
        ? (user.whispering ? QStringLiteral("capture/speechCueRemoteWhisper")
                           : QStringLiteral("capture/speechCueRemoteActive"))
        : QStringLiteral("capture/speechCueRemoteInactive");
    if (!S::flag(remoteKey, false)) return;
    HSound::playFile(S::str(pathKey));
}

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
    ChannelDialog dlg(tr("Criar canal"), &m_data, m_net, this);
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
    ChannelDialog dlg(tr("Editar canal"), &m_data, m_net, this);
    dlg.setChannel(m_data.channels[channelId]);
    if (dlg.exec() != QDialog::Accepted) return;

    Channel c = dlg.resultChannel();
    if (m_net) {
        QJsonObject o = chanToJson(c);
        o["id"] = channelId;
        o["parent"] = m_data.channels[channelId].parentId;
        m_data.channels[channelId].noSymbol = c.noSymbol;
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
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Editar servidor virtual"));
    QFormLayout* f = new QFormLayout(&dlg);
    QLineEdit* name = new QLineEdit(m_data.name, &dlg);
    QLineEdit* motd = new QLineEdit(m_data.motd, &dlg);
    f->addRow(tr("Nome do servidor:"), name);
    f->addRow(tr("Mensagem do dia:"), motd);

    QByteArray bannerBytes = m_data.serverBanner;
    bool bannerChanged = false;
    QHBoxLayout* bannerRow = new QHBoxLayout;
    QLabel* bannerPreview = new QLabel(&dlg);
    bannerPreview->setFixedSize(180, 52);
    bannerPreview->setAlignment(Qt::AlignCenter);
    bannerPreview->setText(m_data.serverBanner.isEmpty()
        ? tr("Banner padrão") : tr("Banner personalizado"));
    if (!m_data.serverBanner.isEmpty()) {
        const QImage current = QImage::fromData(m_data.serverBanner);
        if (!current.isNull())
            bannerPreview->setPixmap(QPixmap::fromImage(current.scaled(
                180, 52, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
    }
    QPushButton* chooseBanner = new QPushButton(tr("Escolher imagem..."), &dlg);
    QPushButton* clearBanner = new QPushButton(tr("Remover"), &dlg);
    bannerRow->addWidget(bannerPreview);
    bannerRow->addWidget(chooseBanner);
    bannerRow->addWidget(clearBanner);
    bannerRow->addStretch(1);
    f->addRow(tr("Imagem do banner:"), bannerRow);

    QObject::connect(chooseBanner, &QPushButton::clicked, &dlg, [&] {
        const QString path = QFileDialog::getOpenFileName(
            &dlg, tr("Selecionar imagem do banner"), QString(),
            tr("Imagens (*.png *.jpg *.jpeg *.gif *.webp)"));
        if (path.isEmpty()) return;
        QImage image(path);
        if (image.isNull()) {
            QMessageBox::warning(&dlg, tr("Banner"), tr("Não foi possível abrir essa imagem."));
            return;
        }
        image = image.scaled(1600, 500, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        QByteArray encoded;
        QBuffer buffer(&encoded);
        buffer.open(QIODevice::WriteOnly);
        image.save(&buffer, "PNG");
        if (encoded.size() > 512 * 1024) {
            encoded.clear();
            QBuffer jpg(&encoded);
            jpg.open(QIODevice::WriteOnly);
            image.convertToFormat(QImage::Format_RGB888).save(&jpg, "JPEG", 84);
        }
        if (encoded.size() > 512 * 1024) {
            QMessageBox::warning(&dlg, tr("Banner"),
                                 tr("A imagem precisa ter no máximo 512 KiB."));
            return;
        }
        bannerBytes = encoded;
        bannerChanged = true;
        bannerPreview->setText(tr("Banner personalizado"));
        bannerPreview->setPixmap(QPixmap::fromImage(image.scaled(
            180, 52, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
    });
    QObject::connect(clearBanner, &QPushButton::clicked, &dlg, [&] {
        bannerBytes.clear();
        bannerChanged = true;
        bannerPreview->setPixmap(QPixmap());
        bannerPreview->setText(tr("Banner padrão"));
    });

    QDialogButtonBox* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                                                &dlg);
    f->addRow(bb);
    QObject::connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    if (dlg.exec() != QDialog::Accepted) return;
    const QString n = name->text().trimmed();
    if (n.isEmpty()) return;

    if (m_net) {
        m_net->serverEdit(n, motd->text(), bannerBytes, bannerChanged);
        systemMsgServer(tr("Solicitação de edição do servidor virtual enviada."));
        return; // o estado atualizado chega do servidor
    }

    m_data.name = n;
    m_data.motd = motd->text();
    if (bannerChanged) m_data.serverBanner = bannerBytes;
    systemMsgServer(tr("Servidor renomeado para \"%1\".").arg(m_data.name));
    m_tree->rebuild();
    emit titleChanged();
    emit statusChanged();
}

// ==================================================================== v3: avatar
void ServerTab::viewAvatar(int userId) {
    if (!m_data.users.contains(userId)) return;
    const User& u = m_data.users[userId];
    if (!m_net || u.avatarHash.isEmpty()) {
        QMessageBox::information(this, tr("Avatar"),
                                 tr("Nenhum avatar definido para este cliente."));
        return;
    }
    // v3: busca a imagem no servidor e mostra quando chegar
    const QString uid = u.uniqueId;
    QObject* ctx = new QObject(this);
    connect(m_net, &NetSession::avatarDataReceived, ctx,
            [this, ctx, uid, name = u.name](const QString& uid2, const QByteArray& bytes) {
                if (uid2 != uid) return;
                ctx->deleteLater();
                QImage img = QImage::fromData(bytes);
                if (img.isNull()) {
                    QMessageBox::information(this, tr("Avatar"),
                        tr("Nenhum avatar definido para este cliente."));
                    return;
                }
                QDialog dlg(this);
                dlg.setWindowTitle(tr("Avatar de %1").arg(name));
                QVBoxLayout* l = new QVBoxLayout(&dlg);
                QLabel* pic = new QLabel(&dlg);
                pic->setPixmap(QPixmap::fromImage(img.scaled(256, 256, Qt::KeepAspectRatio,
                                                             Qt::SmoothTransformation)));
                pic->setAlignment(Qt::AlignCenter);
                l->addWidget(pic);
                QDialogButtonBox* bb = new QDialogButtonBox(QDialogButtonBox::Close, &dlg);
                l->addWidget(bb);
                QObject::connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
                dlg.exec();
            });
    m_net->avatarGet(uid);
}

void ServerTab::setAvatarInteractive() {
    if (!m_net) {
        QMessageBox::information(this, tr("Avatar"),
            tr("Avatares exigem conexão com um Halla Server."));
        return;
    }
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Definir avatar"), QString(),
        tr("Imagens (*.png *.jpg *.jpeg *.bmp *.webp)"));
    if (path.isEmpty()) return;
    QImage img(path);
    if (img.isNull()) {
        QMessageBox::warning(this, tr("Avatar"), tr("Não foi possível ler a imagem."));
        return;
    }
    img = img.scaled(192, 192, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    QByteArray bytes;
    for (int q = 85; q >= 40 && (bytes.size() > 96 * 1024 || bytes.isEmpty()); q -= 15) {
        bytes.clear();
        QBuffer buf(&bytes);
        buf.open(QIODevice::WriteOnly);
        img.save(&buf, "JPEG", q);
    }
    m_net->avatarSet(bytes);
    systemMsgServer(tr("Avatar enviado ao servidor."));
}

void ServerTab::removeAvatar() {
    if (!m_net) return;
    m_net->avatarSet(QByteArray());
    systemMsgServer(tr("Avatar removido."));
}

// ==================================================================== v3: gravação
bool ServerTab::isRecording() const { return m_voice && m_voice->isRecording(); }

void ServerTab::toggleRecording() {
    if (isRecording()) {
        m_voice->stopRecording();
        m_data.users[m_data.selfId].recording = false;
        if (m_net) m_net->sendStatus();
        systemMsgChannel(tr("Gravação interrompida."));
        m_tree->rebuild();
        emit statusChanged();
        return;
    }
    if (!m_voice || !m_voice->isActive()) {
        systemMsgChannel(tr("A gravação requer uma conexão ativa com captura de áudio."));
        return;
    }
    const QString dirPath = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)
                            + QStringLiteral("/Halla");
    QDir().mkpath(dirPath);
    const QString path = dirPath + QStringLiteral("/gravacao-%1.wav")
        .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-hhmmss")));
    if (!m_voice->startRecording(path)) {
        QMessageBox::warning(this, tr("Gravação"),
                             tr("Não foi possível criar o arquivo:\n%1").arg(path));
        return;
    }
    m_data.users[m_data.selfId].recording = true;
    if (m_net) m_net->sendStatus();
    HSound::play(QStringLiteral("recording"));
    systemMsgChannel(tr("Gravação iniciada: %1").arg(path));
    AppLog::info(tr("Gravação iniciada em %1").arg(path));
    m_tree->rebuild();
    emit statusChanged();
}

// ==================================================================== v3: offline
void ServerTab::openOfflineMessages() {
    if (!m_net) {
        QMessageBox::information(this, tr("Mensagens offline"),
            tr("Mensagens offline exigem conexão com um Halla Server."));
        return;
    }
    OfflineMessagesDialog dlg(m_net, &m_data, m_offlineInbox, this);
    dlg.exec();
}

// ==================================================================== v3: sussurro
void ServerTab::setWhisperUids(const QStringList& uids) {
    m_whisperUids = uids;
    if (m_data.users.contains(m_data.selfId)) {
        m_data.users[m_data.selfId].whispering = !uids.isEmpty();
    }
    if (!uids.isEmpty() && m_voice && m_voice->isTalking())
        playSpeechCue(true);
    if (!m_net) return;
    if (uids.isEmpty()) {
        m_net->setWhisperIds({});
        systemMsgChannel(tr("Sussurro desativado. Sua voz segue para o canal."));
        return;
    }
    applyWhisper();
    QStringList names;
    for (const User& u : m_data.users)
        if (m_whisperUids.contains(u.uniqueId) && u.id != m_data.selfId) names << u.name;
    systemMsgChannel(names.isEmpty()
        ? tr("Sussurro ativado (nenhum destinatário está conectado no momento).")
        : tr("Sussurro ativado para: %1").arg(names.join(QStringLiteral(", "))));
}

void ServerTab::applyWhisper() {
    if (!m_net) return;
    QList<int> ids;
    QSet<int> channelTargets;
    for (const QString& target : m_whisperUids) {
        bool ok = false;
        const int channelId = target.toInt(&ok);
        if (ok && m_data.channels.contains(channelId)) channelTargets.insert(channelId);
    }
    // Uma lista pode conter usuários (UID) e canais. Expandir os canais aqui,
    // no instante do envio, evita listas "vazias" quando seus membros entram,
    // saem ou mudam de canal.
    for (const User& u : m_data.users) {
        if (u.id == m_data.selfId) continue;
        const int userChannel = m_data.channelOfUser(u.id);
        if (m_whisperUids.contains(u.uniqueId) || channelTargets.contains(userChannel))
            if (!ids.contains(u.id)) ids << u.id;
    }
    m_net->setWhisperIds(ids);
}

// ==================================================== v3.11: sussurro por atalho
// Calcula os destinatários do sussurro conforme o alvo configurado na tecla
// de atalho: canal atual, canal atual + subcanais ou lista de usuários.
QList<int> ServerTab::whisperTargetIds(int scope) const {
    QList<int> ids;
    const int self = m_data.selfId;

    if (scope == 2) { // lista configurada em Ferramentas > Listas de sussurro
        const QStringList targets = WhisperDialog::activeWhisperUids();
        QSet<int> channels;
        for (const QString& target : targets) {
            bool ok = false;
            const int channelId = target.toInt(&ok);
            if (ok && m_data.channels.contains(channelId)) channels.insert(channelId);
        }
        for (const User& u : m_data.users) {
            if (u.id == self) continue;
            if (targets.contains(u.uniqueId) || channels.contains(m_data.channelOfUser(u.id)))
                if (!ids.contains(u.id)) ids << u.id;
        }
        return ids;
    }

    // escopo por canal: eu preciso estar em um canal
    const int my = m_data.channelOfUser(self);
    if (my < 0 || !m_data.channels.contains(my)) return ids;

    QSet<int> chans;
    chans << my;
    if (scope == 1) { // canal atual + TODOS os subcanais (recursivo)
        QList<int> stack = { my };
        while (!stack.isEmpty()) {
            const int cur = stack.takeLast();
            for (int child : m_data.childChannels(cur))
                if (!chans.contains(child)) { chans << child; stack << child; }
        }
    }
    for (int cid : std::as_const(chans)) {
        if (!m_data.channels.contains(cid)) continue;
        for (int uid : m_data.channels[cid].users)
            if (uid != self && !ids.contains(uid)) ids << uid;
    }
    return ids;
}

void ServerTab::setWhisperHold(bool on, int scope) {
    if (m_whisperHold == on) return;
    m_whisperHold = on;
    if (m_data.users.contains(m_data.selfId)) {
        m_data.users[m_data.selfId].whispering = on;
        m_data.users[m_data.selfId].talking = on;
    }
    if (m_voice) {
        m_voice->setWhisperHeld(on);
    }

    if (on) {
        const QList<int> ids = whisperTargetIds(scope);
        if (scope == 2 && ids.isEmpty()) {
            systemMsgChannel(tr("Sussurro: a lista de usuários está vazia — "
                                "configure em Ferramentas > Listas de sussurro."));
        }
        if (m_net) m_net->setWhisperIds(ids);
        const QStringList names = { tr("canal atual"), tr("canal atual + subcanais"),
                                    tr("lista de usuários") };
        systemMsgChannel(tr("Sussurro ativo (%1): sua voz vai para %2 usuário(s).")
                             .arg(names.value(scope >= 0 && scope <= 2 ? scope : 2))
                             .arg(ids.size()));
    } else {
        // ao soltar: restaura a lista fixa (se houver) ou volta a falar no canal
        if (!m_whisperUids.isEmpty()) applyWhisper();
        else if (m_net)              m_net->setWhisperIds({});
        systemMsgChannel(tr("Sussurro desativado. Sua voz segue para o canal."));
    }
    m_tree->rebuild();
    emit statusChanged();
}

void ServerTab::toggleCommander() {
    User& self = m_data.users[m_data.selfId];
    const bool on = !self.commander;
    if (m_net) {
        if (!hasPermission({ QStringLiteral("selfCommander"),
                             QStringLiteral("b_client_is_channel_commander"),
                             QStringLiteral("setCommander"),
                             QStringLiteral("b_client_set_channel_commander") })) {
            systemMsgChannel(tr("Você não tem permissão para ser comandante do canal."));
            return;
        }
        m_net->setCommander(self.id, on);
        return;
    }
    self.commander = on;
    systemMsgChannel(on
        ? tr("Você agora é o comandante do canal.")
        : tr("Você não é mais o comandante do canal."));
    m_tree->rebuild();
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
    if (S::flag("notify/muteSound", true))
        HSound::play(on ? QStringLiteral("mic_muted") : QStringLiteral("mic_unmuted"));
    if (m_net) { m_net->sendStatus(); m_tree->rebuild(); emit statusChanged(); return; }
    systemMsgChannel(on ? tr("Microfone mudo ativado.") : tr("Microfone mudo desativado."));
    m_tree->rebuild();
    emit statusChanged();
}

void ServerTab::setSpeakersMuted(bool on) {
    User& self = m_data.users[m_data.selfId];
    if (self.outputMuted == on) return;
    self.outputMuted = on;
    self.inputMuted = on; // o modo de alto-falantes mudos também silencia o microfone
    if (m_voice) {
        m_voice->setSpeakersEnabled(!on);
        m_voice->setTransmitEnabled(!on);
    }
    if (S::flag("notify/muteSound", true))
        HSound::play(on ? QStringLiteral("sound_muted") : QStringLiteral("sound_resumed"));
    if (m_net) { m_net->sendStatus(); m_tree->rebuild(); emit statusChanged(); return; }
    systemMsgChannel(on ? tr("Alto-falantes mudos.") : tr("Alto-falantes reativados."));
    m_tree->rebuild();
    emit statusChanged();
}
