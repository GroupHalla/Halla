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
#include "core/GroupIconCache.h"
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
#include <QMenu>
#include <QTimer>
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
    o["tempParent"] = c.tempChannelParent;
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

    // The main composition mirrors the reference: two floating cards at the top
    // and a chat card taking the full width below.
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

    // The "..." button opens the server menu (it used to be a dead button) and the
    // right click on the server name in the header opens the same menu --
    // "right-clicking the server" must work everywhere the server appears, not only
    // in the empty area of the tree.
    connect(more, &QToolButton::clicked, this, [this, more] {
        showServerContextMenu(more->mapToGlobal(QPoint(0, more->height())));
    });
    m_serverHeaderName->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_serverHeaderName, &QToolButton::customContextMenuRequested, this,
            [this](const QPoint& pos) {
                showServerContextMenu(m_serverHeaderName->mapToGlobal(pos));
            });

    // The header shows only the server name. The description and the details live
    // in the side panel when the name is clicked.


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

    // session ids from a previous connection are not valid for this one; forces the
    // first applyWhisper to resend the resolved set.
    m_lastSentWhisperIds.clear();

    // users already present at login (do not play a sound for them)
    for (const User& u : m_data.users) {
        m_knownUsers << u.id;
        m_lastChannels[u.id] = m_data.channelOfUser(u.id);
        m_lastTalking[u.id] = u.talking;
        m_lastWhispering[u.id] = u.whispering;
    }
    m_myChan = m_data.channelOfUser(m_data.selfId);

    connect(net, &NetSession::iconDataReceived, this, [this](const QString& name, const QByteArray& bytes) {
        // Keeps it in memory + on disk (QStandardPaths -- never a relative path:
        // with the app in Program Files the cwd is not writable) and repaints the
        // list right away. Scoped per server.
        GroupIconCache::instance().store(GroupIconCache::serverKey(m_data.address), name, bytes);
        m_tree->viewport()->update();
        // The information panel embeds the role icon in HTML -- it must re-render
        // when the bytes arrive (the row goes from name-only to image + name).
        m_info->refresh();
    });

    connect(net, &NetSession::iconUploaded, this, [this](const QString& name) {
        // An admin uploaded (or replaced) an icon: downloads IMMEDIATELY, even if a
        // local copy already exists -- the new bytes overwrite the cache and
        // iconDataReceived repaints the tree. This is what makes the image refresh
        // "instantly" for everyone connected.
        if (m_net) m_net->iconGet(name);
    });

    // state coming from the server -> repaints the tree/information panel
    connect(net, &NetSession::stateChanged, this, [this] {
        // user_state arrives on every speech transition (VAD) of any client; a
        // fluctuating microphone can generate dozens of events per second. Rebuilding
        // the tree on each event froze the interface and made the channel list "scroll
        // by itself". Coalesce the bursts: process at most once every 120 ms, always
        // with the most recent state.
        if (m_stateRefreshPending) return;
        m_stateRefreshPending = true;
        QTimer::singleShot(120, this, [this] {
            m_stateRefreshPending = false;
            refreshServerState();
        });
    });

    connect(net, &NetSession::chatReceived, this,
            [this](const QString& scope, int fromId, int toId, const QString& fromName, const QString& text) {
                if (scope == "server") {
                    m_chat->addServerChat(fromName, text);
                } else if (scope == "private") {
                    // The server echoes the private message back to the sender so it
                    // appears in the conversation. Without telling the echo apart (from ==
                    // me), it opened a private chat "with myself" and the message landed
                    // there instead of staying in the recipient's conversation -- exactly
                    // the reported bug. The correct peer is "to" on the echo.
                    const bool selfEcho = (fromId == m_data.selfId);
                    const int peerId = selfEcho ? toId : fromId;
                    QString peerName = fromName;
                    if (selfEcho) {
                        // Recipient name: current or last known state (they may have left
                        // the server between the send and the echo).
                        if (m_data.users.contains(peerId))
                            peerName = m_data.users[peerId].name;
                        else
                            peerName = m_lastNames.value(peerId, tr("Usuário"));
                    }
                    m_chat->addPrivateTab(peerId, peerName);
                    m_chat->addPrivateChat(peerId, fromName, text);
                    if (!selfEcho) {
                        if (S::flag("notify/messageSound", true)) HSound::play(QStringLiteral("message"));
                        HSpeech::say(tr("Mensagem de %1").arg(fromName));
                    }
                } else {
                    m_chat->addChannelChat(fromName, text);
                }
            });

    connect(net, &NetSession::systemEvent, this, [this](const QString& text) {
        systemMsgServer(text);
    });

    connect(net, &NetSession::pokeReceived, this,
            [this](int, const QString& fromName, const QString& msg) {
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
                // v1.1.37: not_found is the "not found" of BACKGROUND queries -- a role
                // icon the admin referenced but never uploaded (icon_get), a deleted
                // avatar, an identity that never connected (identity_get). None of these
                // is a user action nor needs a reply: it became a "Server error: ..." line
                // in the chat plus an error sound on every retry (the missing icon is
                // re-requested by the cache with exponential backoff). It stays only in
                // the log (Help > Client log), where whoever diagnoses looks.
                if (code == QLatin1String("not_found")) {
                    AppLog::warn(tr("Consulta de fundo sem resultado: %1").arg(msg));
                    return;
                }
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

    // ---- voice
    // The active list configures the targets; it does not turn normal PTT into
    // whisper. Only the whisper key enables that routing.
    m_voice = new VoiceEngine(net, &m_data, this);
    m_voice->setWhisperTargetsConfigured(!m_whisperUids.isEmpty());
    // Packet-driven "speaking" indicator: VoiceEngine lights/clears the ring from the
    // audio that actually arrives (UDP) and notifies here. Reuses the same coalescing
    // as stateChanged -- bursts of voice packets (20 ms) must not rebuild the tree on
    // every frame.
    connect(m_voice, &VoiceEngine::remoteVoiceActivityChanged, this, [this] {
        if (m_stateRefreshPending) return;
        m_stateRefreshPending = true;
        QTimer::singleShot(120, this, [this] {
            m_stateRefreshPending = false;
            refreshServerState();
        });
    });
    if (m_voice->isActive()) {
        connect(m_voice, &VoiceEngine::talkingChanged, this, [this](bool on) {
            m_data.users[m_data.selfId].talking = on;
            m_data.users[m_data.selfId].whispering = on && (m_whisperHold || !m_whisperUids.isEmpty());
            // PTT: the cue follows the KEY (press/release = speech start/end). In
            // voice-activated/continuous modes the cue comes from real speech detection
            // (speechActivityChanged) -- playing it here would fire the sound when the
            // transmission OPENS, not when the user speaks.
            if (S::num("capture/pttMode", 1) == 0) playSpeechCue(on);
            // v1.1.35: the user's OWN speech transition = lightweight update (only the
            // row ring/icon). A full rebuild here was a burst of item recreation on
            // every VAD on/off -- on the same thread as the audio.
            m_tree->updateUserVisuals();
        });
        // "On speak" cue driven by real speech: applies to voice detection AND
        // continuous transmission, with voice open or closed -- the VoiceEngine
        // detector keeps running even with the microphone drained.
        connect(m_voice, &VoiceEngine::speechActivityChanged, this,
                [this](bool active) { playSpeechCueOnSpeech(active); });
        emit statusChanged();
    }
}

bool ServerTab::hasPermission(const QStringList& keys) const {
    if (!m_net) return true; // local/demo tabs keep every control available
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
    m_tree->setCanRenameOthers(moveOthers);
    m_tree->setChannelManagementPermissions(
        hasPermission({QStringLiteral("chanEdit")}),
        hasPermission({QStringLiteral("chanDelete")}));
    m_tree->setCommanderPermissions(selfCommander, otherCommander);
}

void ServerTab::applyDisplayOptions() {
    updatePermissionUi();
    m_tree->setShowCounts(S::flag("design/showCounts", true));
    m_tree->setShowMinis(S::flag("design/showMinis", true));
    // v1.1.31: global badges -- default ON (they did not exist in a clean install;
    // whoever toggled them keeps the choice in QSettings).
    m_tree->setShowBadges(S::flag("design/showBadgeIcons", true));
    m_tree->setSortClientsBelow(S::flag("design/sortClientsBelow", false));
    m_tree->rebuild();

    // Expansion at login (Options > Appearance > Channel tree)
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
    // Information panel: same icon path as the tree (quota shared in
    // GroupIconCache -- no duplicated request).
    connect(m_info, &InfoPanel::iconRequested, this, [this](const QString& name) {
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
    connect(m_tree, &ServerTreeWidget::renameUserRequested,
            this, &ServerTab::renameUser);
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

        // Local/demo tabs use the same symmetric relation as the server.
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
            const int db = qBound(-60, dlg.volume(), 30);
            m_data.users[uid].volumeDb = db;
            // Persists by uniqueId (stable across reconnections): "set volume" now
            // survives restarting the app and the person leaving/joining -- before the
            // value lived only in memory and was still zeroed by the server's user_state
            // on every speech transition.
            const QString uniqueId = m_data.users[uid].uniqueId;
            if (!uniqueId.isEmpty() && uniqueId != m_data.users.value(m_data.selfId).uniqueId)
                S::set(QStringLiteral("userVolume/%1").arg(uniqueId), db);
            systemMsgChannel(tr("Volume de \"%1\" definido para %2 dB.")
                                 .arg(m_data.users[uid].name).arg(db));
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
            // removes the client from the channel (local client behavior)
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
            // v2: the server moves whoever holds the "move" permission
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

// Consolidated server state: join/leave sounds, whisper synchronization and tree
// repaint. Extracted from the stateChanged handler so it can be coalesced (bursts of
// VAD user_state must not rerun everything dozens of times per second).
void ServerTab::refreshServerState() {
    // Detects joins and leaves specifically in the user's current channel. Users
    // joining other channels do not trigger the call notice.
    QSet<int> now;
    QMap<int, int> currentChannels;
    // While the user is speaking, the partner's sound cue does not fire: with
    // speakers, the beginning of the partner's voice "comes back" through their
    // microphone as crosstalk and lit up their indicator/talking together with the
    // user's -- the "on speak" sound played twice (once for the speaker, once for the
    // echo on the other side). EchoGuard already cuts the echo at the source; this is
    // the safety net for old builds/peers: while I speak, the remote cue stays silent.
    const bool selfSpeaking = m_data.users.value(m_data.selfId).talking
        || (m_voice && m_voice->speechActive());
    for (const User& u : m_data.users) {
        now << u.id;
        currentChannels[u.id] = m_data.channelOfUser(u.id);
        m_lastNames[u.id] = u.name;
        if (u.id != m_data.selfId) {
            const bool wasTalking = m_lastTalking.value(u.id, false);
            const bool wasWhispering = m_lastWhispering.value(u.id, false);
            if (u.whispering && !wasWhispering)
                m_lastWhisperFromId = u.id; // reply key: last whisperer
            if (u.talking && (!wasTalking || (u.whispering && !wasWhispering))) {
                if (!selfSpeaking) playRemoteSpeechCue(u, true);
            } else if (!u.talking && wasTalking) {
                if (!selfSpeaking) playRemoteSpeechCue(u, false);
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

    // The channel switch itself uses a single notice, without playing once for each
    // person who was already at the destination.
    if (selfChangedChannel && S::flag("notify/channelSwitchSound", true))
        HSound::play(QStringLiteral("moved"));
    m_knownUsers = now;
    m_lastChannels = currentChannels;
    m_myChan = myChan;

    applyWhisper(); // keeps the whisper target in sync (ids change on every login)
    updatePermissionUi();

    // v1.1.35: structural server signature (id+revision of each user and channel --
    // the revision only bumps when something structural changes; speech transitions
    // do NOT count). Same signature = nothing joined/left/moved/renamed/changed
    // permissions: the tree receives the LIGHT update (updateUserVisuals repaints only
    // the ring/icon of the rows whose visual key changed) instead of being destroyed
    // and rebuilt. Before, every user_state burst (one per speech transition of each
    // client, coalesced to 120 ms) recreated ALL items/icons/tooltips/expansion/
    // scroll -- in full rooms that became a pixmap factory at 8x/s on the SAME thread
    // as the audio pipeline, and the voice chopped after some session time.
    QString sig;
    for (auto it = m_data.users.constBegin(); it != m_data.users.constEnd(); ++it)
        sig += QString::number(it.key()) + QLatin1Char(':')
             + QString::number(it.value().rev) + QLatin1Char(';');
    sig += QLatin1Char('#');
    // v1.1.36: the member count enters the signature as a safeguard -- any path that
    // changes a channel's membership WITHOUT bumping the revision (a v1.1.35
    // regression: user_moved mutated Channel.users directly) still changes the
    // signature and forces the rebuild.
    for (auto it = m_data.channels.constBegin(); it != m_data.channels.constEnd(); ++it)
        sig += QString::number(it.key()) + QLatin1Char(':')
             + QString::number(it.value().rev) + QLatin1Char(':')
             + QString::number(it.value().users.size()) + QLatin1Char(';');

    if (sig == m_structSig) {
        m_tree->updateUserVisuals(); // nothing structural: visual states only
    } else {
        m_structSig = sig;
        m_tree->rebuild();
        m_info->refresh();
    }
    emit statusChanged();
}

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

void ServerTab::playSpeechCueOnSpeech(bool active) {
    // Sound cue based on DETECTED SPEECH (not on transmission state): "if the user
    // is on continuous transmission or voice detection, the cue may only play when
    // the user SPEAKS" -- with the voice open or closed, because the detector also
    // runs on the drained microphone.
    if (!S::flag("capture/speechCueEnabled", false)) return;
    if (S::num("capture/pttMode", 1) == 0) return;   // PTT: the cue comes from the key
    if (S::num("capture/speechCueMode", 1) != 1) return; // "Emitir ao" = PTT
    // Whisper already played its own sound when the button was PRESSED (hold):
    // while the hold is open, detected speech does not retrigger the same cue --
    // whisper follows the key, everything else follows speech.
    if (active && m_whisperHold && m_whisperHoldCuePlayed) return;

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

void ServerTab::playWhisperHoldCue() {
    // Key-driven whisper: in voice-activated/continuous modes the whisper hold works
    // as its own PTT (pressing = the voice starts going to the whisper targets) -- so
    // the whisper sound plays AS THE KEY IS PRESSED, even if voice detection has not
    // opened (or never opens). It does not depend on the selected "Emit on": only
    // whisper has its own key in these modes; the normal cue keeps coming from
    // speech. Nothing changes in PTT (the cue follows the transmission key and the
    // whisper path is already applied there).
    if (!S::flag("capture/speechCueEnabled", false)) return;
    QString path = S::str("capture/speechCueWhisper");
    if (path.isEmpty()) path = S::str("capture/speechCueActive");
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

    // network mode: asks for the password (if any) and sends it to the server
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
    const Channel original = m_data.channels[channelId];
    const QString selfUid = m_data.users.value(m_data.selfId).uniqueId;
    const bool globalEditor = hasPermission({QStringLiteral("chanEdit")});
    const bool temporaryOwner = original.type == 0
        && !selfUid.isEmpty() && original.temporaryOwnerUid == selfUid;
    const bool limitedTemporaryOwner = temporaryOwner && !globalEditor;

    ChannelDialog dlg(tr("Editar canal"), &m_data, m_net, this);
    dlg.setChannel(original);
    dlg.setTemporaryOwnerMode(limitedTemporaryOwner);
    if (dlg.exec() != QDialog::Accepted) return;

    Channel c = dlg.resultChannel();
    if (m_net) {
        if (limitedTemporaryOwner) {
            QJsonObject limited;
            limited["id"] = channelId;
            limited["bitrate"] = c.bitrate;
            limited["max"] = c.maxClients;
            if (dlg.passwordWasEdited()) limited["pass"] = c.passwordHash;
            m_net->editChannel(limited);
            return;
        }
        QJsonObject o = chanToJson(c);
        o["id"] = channelId;
        o["parent"] = original.parentId;
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
            // moves users to the default channel
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

// Renames another client. The server validates permission (move) and hierarchy,
// persists the nickname for the target's identity and broadcasts user_nick.
void ServerTab::renameUser(int userId) {
    if (!m_data.users.contains(userId)) return;
    User& target = m_data.users[userId];
    bool ok = false;
    QString name = QInputDialog::getText(this, tr("Alterar apelido"),
                                         tr("Novo apelido para %1:").arg(target.name),
                                         QLineEdit::Normal, target.name, &ok);
    if (!ok || name.trimmed().isEmpty()) return;
    name = name.trimmed().left(30);
    if (m_net) {
        m_net->rename(name, userId);
        return;
    }
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

void ServerTab::showServerContextMenu(const QPoint& globalPos) {
    // Same server menu as the right click on the tree (empty area): Disconnect /
    // Edit virtual server (name, message of the day and BANNER) / Add to bookmarks.
    // Shared by the header "..." button, by the right click on the server name, by
    // the tab and by the server button in the status bar.
    QMenu menu(this);
    menu.addAction(HIcons::disconnectPlug(), tr("Desconectar"), this,
                   [this] { emit disconnectRequested(); });
    menu.addSeparator();
    menu.addAction(HIcons::editPencil(), tr("Editar servidor virtual"), this,
                   [this] { editVirtualServerName(); });
    menu.addSeparator();
    menu.addAction(HIcons::bookmarkStar(), tr("Adicionar aos favoritos"), this,
                   [this] { emit addBookmarkRequested(); });
    menu.exec(globalPos);
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
        return; // the updated state arrives from the server
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
    // v3: fetches the image from the server and shows it when it arrives
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

// ==================================================================== v3: recording
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
    if (m_voice) m_voice->setWhisperTargetsConfigured(!uids.isEmpty());
    if (m_data.users.contains(m_data.selfId)) {
        m_data.users[m_data.selfId].whispering = !uids.isEmpty();
    }
    if (!uids.isEmpty() && m_voice && m_voice->isTalking())
        playSpeechCueOnSpeech(true);
    if (!m_net) return;
    if (uids.isEmpty()) {
        m_lastSentWhisperIds.clear();
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
    if (m_whisperHold) {
        // REPLY hold: the target is fixed (whoever whispered last) and was set by
        // setWhisperReplyHold -- re-resolving by scope here would overwrite the reply
        // recipient.
        if (m_whisperReplyHold) return;
        // Key-driven whisper ACTIVE: re-resolves the targets from the key's scope.
        // Targets are SESSION ids -- when a target reconnects, moves channel or
        // someone new enters the scope, the set changes; sending only the different
        // set keeps whisper alive without flooding the server (dedup through
        // m_lastSentWhisperIds).
        ids = whisperTargetIds(m_whisperHoldScope);
    } else if (!m_whisperUids.isEmpty()) {
        // Fixed whisper list (uids + channels): expands the channels here, at send
        // time, so lists do not become "empty" when their members join, leave or
        // change channel.
        QSet<int> channelTargets;
        for (const QString& target : m_whisperUids) {
            bool ok = false;
            const int channelId = target.toInt(&ok);
            if (ok && m_data.channels.contains(channelId)) channelTargets.insert(channelId);
        }
        for (const User& u : m_data.users) {
            if (u.id == m_data.selfId) continue;
            const int userChannel = m_data.channelOfUser(u.id);
            if (m_whisperUids.contains(u.uniqueId) || channelTargets.contains(userChannel))
                if (!ids.contains(u.id)) ids << u.id;
        }
    } else {
        // NOTHING active: there is no set to synchronize. This was the heart of the
        // whisper bug (v1.1.31): with an empty fixed list the code fell through to
        // the end and sent whisper_ids = [] to the server on the FIRST state refresh
        // (any client's user_state -- a speech indicator oscillating with the VAD was
        // enough), WIPING the whisper routing the key had just configured. The voice
        // went back to the sender's channel: a target in another channel heard
        // nothing -- neither whisper nor normal voice -- with the orange ring lit at
        // the source.
        return;
    }
    // stateChanged fires on every user_state -- including any client's speech
    // indicator (talking) oscillating with the VAD. Resending an identical whisper at
    // that rate flooded the server and tripped the rate limit ("you are sending
    // messages too fast"). Send only when the resolved set really changes (e.g. a
    // target reconnected and got a new session id).
    if (ids == m_lastSentWhisperIds) return;
    m_lastSentWhisperIds = ids;
    m_net->setWhisperIds(ids);
}

// ==================================================== v3.11: whisper by shortcut
// Computes the whisper recipients according to the target configured on the
// shortcut key: current channel, current channel + subchannels, or a user list.
QList<int> ServerTab::whisperTargetIds(int scope) const {
    QList<int> ids;
    const int self = m_data.selfId;

    if (scope == 2) { // list configured in Tools > Whisper Lists
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

    // channel scope: I must be inside a channel
    const int my = m_data.channelOfUser(self);
    if (my < 0 || !m_data.channels.contains(my)) return ids;

    QSet<int> chans;
    chans << my;
    if (scope == 1) { // current channel + ALL subchannels (recursive)
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
    m_whisperHoldScope = qBound(0, scope, 2);
    m_whisperReplyHold = false; // regular hold: targets from the key's scope
    if (m_data.users.contains(m_data.selfId)) {
        m_data.users[m_data.selfId].whispering = on;
        m_data.users[m_data.selfId].talking = on;
    }
    if (m_voice) {
        m_voice->setWhisperHeld(on);
    }
    // Key-driven whisper cue (voice-activated/continuous modes): the whisper sound
    // plays as soon as the button is PRESSED -- even in Voice Activity mode and even
    // if speech has not opened the transmission yet. Only whisper behaves this way;
    // the VAD opening during the hold does not retrigger it (m_whisperHoldCuePlayed).
    if (S::num("capture/pttMode", 1) != 0) {
        m_whisperHoldCuePlayed = on;
        if (on) playWhisperHoldCue();
    }

    if (on) {
        const QList<int> ids = whisperTargetIds(scope);
        if (scope == 2 && ids.isEmpty()) {
            systemMsgChannel(tr("Sussurro: a lista de usuários está vazia — "
                                "configure em Ferramentas > Listas de sussurro."));
        }
        if (m_net) {
            m_lastSentWhisperIds = ids;
            m_net->setWhisperIds(ids);
        }
        const QStringList names = { tr("canal atual"), tr("canal atual + subcanais"),
                                    tr("lista de usuários") };
        systemMsgChannel(tr("Sussurro ativo (%1): sua voz vai para %2 usuário(s).")
                             .arg(names.value(scope >= 0 && scope <= 2 ? scope : 2))
                             .arg(ids.size()));
    } else {
        // on release: restores the fixed list (if any) or speaks on the channel again
        if (!m_whisperUids.isEmpty()) applyWhisper();
        else if (m_net) {
            m_lastSentWhisperIds.clear();
            m_net->setWhisperIds({});
        }
        systemMsgChannel(tr("Sussurro desativado. Sua voz segue para o canal."));
    }
    m_tree->rebuild();
    emit statusChanged();
}

// Reply key of the Whisper Lists: hold to whisper back to the last user who
// whispered to you in this session. Releasing follows the same restore path as the
// regular hold (fixed list or channel).
void ServerTab::setWhisperReplyHold(bool on) {
    if (!on) {
        setWhisperHold(false, 2); // restores the fixed list (if any) or the channel
        return;
    }
    const int from = m_lastWhisperFromId;
    if (from <= 0 || from == m_data.selfId || !m_data.users.contains(from)) {
        systemMsgChannel(tr("Sussurro (resposta): ninguém sussurrou para você "
                            "ainda, ou quem sussurrou saiu do servidor."));
        return;
    }
    if (m_data.users.contains(m_data.selfId)) {
        m_data.users[m_data.selfId].whispering = true;
        m_data.users[m_data.selfId].talking = true;
    }
    if (m_voice) m_voice->setWhisperHeld(true);
    m_whisperHold = true;
    m_whisperReplyHold = true; // reply: fixed target, applyWhisper does not re-resolve
    // Same rule as the regular hold: whisper cue when the reply key is pressed
    // (voice-activated/continuous modes).
    if (S::num("capture/pttMode", 1) != 0) {
        m_whisperHoldCuePlayed = true;
        playWhisperHoldCue();
    }
    if (m_net) {
        m_lastSentWhisperIds = QList<int>{ from };
        m_net->setWhisperIds(m_lastSentWhisperIds);
    }
    systemMsgChannel(tr("Sussurro (resposta): sua voz vai para %1.")
                         .arg(m_data.users.value(from).name));
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
    self.inputMuted = on; // muted-speakers mode also silences the microphone
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

bool ServerTab::setUserLocallyMuted(int userId, bool muted) {
    if (!m_data.users.contains(userId) || userId == m_data.selfId) return false;
    m_data.users[userId].locallyMuted = muted;
    m_tree->rebuild();
    m_info->refresh();
    emit statusChanged();
    return true;
}

bool ServerTab::setUserVolumeDb(int userId, int volumeDb) {
    if (!m_data.users.contains(userId) || userId == m_data.selfId) return false;
    const int db = qBound(-60, volumeDb, 30);
    m_data.users[userId].volumeDb = db;
    const QString uniqueId = m_data.users[userId].uniqueId;
    if (!uniqueId.isEmpty() && uniqueId != m_data.users.value(m_data.selfId).uniqueId)
        S::set(QStringLiteral("userVolume/%1").arg(uniqueId), db);
    m_info->refresh();
    emit statusChanged();
    return true;
}
