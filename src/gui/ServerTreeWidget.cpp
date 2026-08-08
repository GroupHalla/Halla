#include "ServerTreeWidget.h"
#include "Icons.h"
#include "Settings.h"

#include <QContextMenuEvent>
#include <QMouseEvent>
#include <QMenu>
#include <QMimeData>
#include <QDropEvent>
#include <QPainter>
#include <QLinearGradient>
#include <QSet>
#include <QStyle>
#include <QRegularExpression>
#include <algorithm>
#include <QFile>
#include <QDir>

static QPixmap createGroupIconPixmap(const QString& iconText, ServerTreeWidget* tree) {
    if (iconText.isEmpty()) return QPixmap();
    
    // Se o ícone for uma imagem (extensão de arquivo)
    if (iconText.endsWith(".png", Qt::CaseInsensitive) || 
        iconText.endsWith(".jpg", Qt::CaseInsensitive) || 
        iconText.endsWith(".jpeg", Qt::CaseInsensitive) || 
        iconText.endsWith(".gif", Qt::CaseInsensitive)) {
        
        QString path = QStringLiteral("cache/icons/") + iconText;
        if (QFile::exists(path)) {
            QPixmap pm;
            pm.load(path);
            if (!pm.isNull()) {
                return pm.scaled(16, 14, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            }
        } else {
            // Solicita o ícone se ainda não foi solicitado nesta sessão
            static QSet<QString> requested;
            if (!requested.contains(iconText)) {
                requested.insert(iconText);
                if (tree) {
                    emit tree->iconRequested(iconText);
                }
            }
        }
        return QPixmap();
    }
    
    QPixmap pm(16, 14);
    pm.fill(Qt::transparent);
    QPainter painter(&pm);
    painter.setRenderHint(QPainter::TextAntialiasing);
    
    if (iconText.length() <= 2) {
        QFont font = painter.font();
        font.setPixelSize(11);
        painter.setFont(font);
        painter.setPen(Qt::black);
        painter.drawText(QRect(0, 0, 16, 14), Qt::AlignCenter, iconText);
    } else {
        int hash = 0;
        for (QChar c : iconText) {
            hash += c.unicode();
        }
        
        QColor bgColors[] = {
            QColor("#E25C5C"), QColor("#3D9BE9"), QColor("#33B46B"), 
            QColor("#F0A020"), QColor("#995DE8"), QColor("#1ABC9C")
        };
        QColor bgColor = bgColors[hash % 6];
        
        painter.setPen(Qt::NoPen);
        painter.setBrush(bgColor);
        painter.drawRoundedRect(QRectF(1, 1, 14, 12), 3, 3);
        
        painter.setPen(Qt::white);
        QFont font = painter.font();
        font.setPixelSize(9);
        font.setBold(true);
        painter.setFont(font);
        painter.drawText(QRect(0, 0, 16, 14), Qt::AlignCenter, iconText.left(1).toUpper());
    }
    
    return pm;
}

// Statuses are placed in the same leading area as the user's avatar. The
// reference shows microphone/headphone/away indicators before the nickname,
// never after it.
static QPixmap treeUserSphere(const User& u) {
    QPixmap pm(16, 16);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    const bool dark = HTheme::isDark();
    QLinearGradient gradient(0, 1, 0, 15);
    gradient.setColorAt(0, u.away ? QColor("#8B97A5") : (dark ? QColor("#9A8BCE") : QColor("#9FC4E4")));
    gradient.setColorAt(1, u.away ? QColor("#566271") : (dark ? QColor("#3B2875") : QColor("#1E527D")));
    p.setPen(QPen(u.away ? QColor("#66727F") : (dark ? QColor("#2A1C52") : QColor("#1E415F")), 0.8));
    p.setBrush(gradient);
    p.drawEllipse(QRectF(1, 1, 14, 14));
    if (u.talking) {
        const QColor ring = u.whispering ? QColor("#F59E0B") : QColor("#22C55E");
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(ring, 1.8, Qt::SolidLine, Qt::RoundCap));
        p.drawEllipse(QRectF(0.7, 0.7, 14.6, 14.6));
    }
    return pm;
}

static QIcon leadingUserIcon(const User& u, bool showStatus) {
    const bool outputMuted = u.outputMuted || u.locallyMuted;
    const bool audioMuted = u.inputMuted || outputMuted;
    const QPixmap avatar = treeUserSphere(u);

    // O indicador azul é o estado normal. Quando a entrada ou a saída de
    // áudio está bloqueada, ele é substituído pelos indicadores correspondentes
    // antes do nome: microfone riscado e/ou fones com cadeado. Assim não há
    // dois sinais concorrentes nem dúvida sobre o estado real do usuário.
    const QPixmap statuses = (showStatus || audioMuted)
        ? HIcons::userStatusMinis(
              u.inputMuted,
              outputMuted,
              showStatus ? u.away : false,
              showStatus ? u.recording : false,
              showStatus ? u.commander : false,
              showStatus ? u.op : false)
        : QPixmap();

    if (audioMuted) {
        const int width = statuses.isNull() ? 16 : statuses.width();
        QPixmap combined(width, 18);
        combined.fill(Qt::transparent);
        QPainter painter(&combined);
        if (!statuses.isNull()) painter.drawPixmap(0, 2, statuses);
        return QIcon(combined);
    }

    const int statusWidth = statuses.isNull() ? 0 : statuses.width();
    QPixmap combined(16 + (statusWidth > 0 ? statusWidth + 2 : 0), 18);
    combined.fill(Qt::transparent);
    QPainter painter(&combined);
    painter.drawPixmap(0, 1, avatar);
    if (!statuses.isNull()) painter.drawPixmap(17, 2, statuses);
    return QIcon(combined);
}

// ============================================================== Delegado
void ServerRowDelegate::paint(QPainter* p, const QStyleOptionViewItem& opt,
                              const QModelIndex& index) const {
    QStyledItemDelegate::paint(p, opt, index);

    if (!m_data) return;
    if (index.data(RoleKind).toInt() != NodeUser) return;

    const int uid = index.data(RoleId).toInt();
    if (!m_data->users.contains(uid)) return;
    const User& u = m_data->users[uid];

    ServerTreeWidget* tree = qobject_cast<ServerTreeWidget*>(const_cast<QObject*>(parent()));

    // Separamos e renderizamos múltiplos ícones/cargos se existirem (separados por vírgula ou ponto e vírgula)
    QStringList icons = u.groupIcon.split(QRegularExpression("[,;]"), Qt::SkipEmptyParts);
    QList<QPixmap> iconPms;
    for (const QString& ic : icons) {
        QString trimmed = ic.trimmed();
        if (!trimmed.isEmpty()) {
            QPixmap pm = createGroupIconPixmap(trimmed, tree);
            if (!pm.isNull()) {
                iconPms << pm;
            }
        }
    }

    int totalW = 0;
    for (const QPixmap& pm : iconPms) totalW += pm.width() + 4;
    if (totalW == 0) return;

    QPixmap combined(totalW, 14);
    combined.fill(Qt::transparent);
    QPainter cp(&combined);

    int currentX = 0;
    for (const QPixmap& pm : iconPms) {
        cp.drawPixmap(currentX, 0, pm);
        currentX += pm.width() + 4;
    }

    QStyleOptionViewItem o = opt;
    initStyleOption(&o, index);
    const QStyle* style = o.widget ? o.widget->style() : nullptr;
    const int iconW = o.decorationSize.width();
    QRect textRect = style ? style->subElementRect(QStyle::SE_ItemViewItemText, &o, o.widget)
                           : o.rect;
    QFontMetrics fm(o.font);
    const int textW = fm.horizontalAdvance(o.text);

    int x = o.rect.right() - combined.width() - 8;
    int textRightLimit = textRect.left() + iconW + 6 + textW + 8;
    if (x < textRightLimit) {
        x = textRightLimit;
    }
    const int y = o.rect.top() + (o.rect.height() - combined.height()) / 2;
    int w = combined.width();
    const int maxX = o.rect.right() - 4;
    if (x + w > maxX) w = maxX - x;
    if (w > 0) p->drawPixmap(x, y, combined.copy(0, 0, w, combined.height()));
}

// ============================================================== Árvore
ServerTreeWidget::ServerTreeWidget(QWidget* parent) : QTreeWidget(parent) {
    setHeaderHidden(true);
    setIndentation(12);
    setRootIsDecorated(true);
    setAnimated(false);
    setUniformRowHeights(true);
    setAllColumnsShowFocus(true);
    // Permite selecionar vários canais para as operações em conjunto, como
    // vincular/desvincular áudio. Usuários continuam funcionando normalmente
    // com seleção estendida.
    setSelectionMode(QAbstractItemView::ExtendedSelection);
    setDragDropMode(InternalMove);
    // Reserva largura para o avatar e até dois indicadores de áudio antes do
    // nome. Quando o áudio é bloqueado, o avatar deixa de ocupar esse espaço.
    setIconSize(QSize(48, 18));
    setFrameShape(QFrame::NoFrame);

    m_delegate = new ServerRowDelegate(this);
    setItemDelegate(m_delegate);

    setMouseTracking(true);
    connect(this, &QTreeWidget::itemEntered, this, &ServerTreeWidget::onItemEntered);

    // as cores vêm do tema global (HTheme) — stylesheet fixo aqui impedia
    // o tema escuro no Windows
    setObjectName(QStringLiteral("serverTree"));

    connect(this, &QTreeWidget::currentItemChanged, this,
            [this](QTreeWidgetItem* cur, QTreeWidgetItem*) {
                if (!cur) { emit selectionChanged(-1, -1); return; }
                emit selectionChanged(cur->data(0, RoleKind).toInt(),
                                      cur->data(0, RoleId).toInt());
            });
}

void ServerTreeWidget::setServerData(ServerData* d) {
    m_data = d;
    m_delegate->setServerData(d);
}

void ServerTreeWidget::setShowMinis(bool on) {
    m_delegate->setShowMinis(on);
    viewport()->update();
}

int ServerTreeWidget::currentKind() const {
    QTreeWidgetItem* it = currentItem();
    return it ? it->data(0, RoleKind).toInt() : -1;
}

int ServerTreeWidget::currentId() const {
    QTreeWidgetItem* it = currentItem();
    return it ? it->data(0, RoleId).toInt() : -1;
}

QList<int> ServerTreeWidget::selectedChannelIds() const {
    QList<int> ids;
    for (QTreeWidgetItem* item : selectedItems()) {
        if (!item || item->data(0, RoleKind).toInt() != NodeChannel) continue;
        const int id = item->data(0, RoleId).toInt();
        if (id > 0 && !ids.contains(id)) ids << id;
    }
    return ids;
}

void ServerTreeWidget::selectNode(int kind, int id) {
    std::function<bool(QTreeWidgetItem*)> walk = [&](QTreeWidgetItem* item) -> bool {
        if (item->data(0, RoleKind).toInt() == kind &&
            item->data(0, RoleId).toInt() == id) {
            setCurrentItem(item);
            return true;
        }
        for (int i = 0; i < item->childCount(); ++i)
            if (walk(item->child(i))) return true;
        return false;
    };
    for (int i = 0; i < topLevelItemCount(); ++i)
        if (walk(topLevelItem(i))) return;
}

QString ServerTreeWidget::userTooltip(const User& u) const {
    QString tip = QStringLiteral("<b>%1</b><br>").arg(u.name.toHtmlEscaped());
    if (u.id == m_data->selfId) tip += QStringLiteral("(você)<br>");
    if (u.away)        tip += QStringLiteral("Ausente<br>");
    if (u.inputMuted)  tip += QStringLiteral("Microfone mudo<br>");
    if (u.outputMuted) tip += QStringLiteral("Alto-falantes mudos<br>");
    if (u.recording)   tip += QStringLiteral("Gravando<br>");
    if (u.commander)   tip += QStringLiteral("Comandante do canal<br>");
    tip += QStringLiteral("Grupos: %1").arg(u.serverGroups.toHtmlEscaped());
    return tip;
}

QString ServerTreeWidget::channelTooltip(const Channel& c) const {
    QString tip = QStringLiteral("<b>%1</b><br>").arg(c.name.toHtmlEscaped());
    tip += QStringLiteral("Codec: %1 (qualidade %2)").arg(codecShortNames()[c.codec]).arg(c.codecQuality);
    if (c.hasPassword) tip += QStringLiteral("<br>Protegido por senha");
    if (c.moderated)   tip += QStringLiteral("<br>Moderado");
    if (!c.linkedChannels.isEmpty())
        tip += QStringLiteral("<br>Áudio vinculado a %1 canal(is)").arg(c.linkedChannels.size());
    static const char* tnames[] = { "Temporário", "Semi-permanente", "Permanente" };
    tip += QStringLiteral("<br>%1").arg(QString::fromUtf8(tnames[c.type]));
    return tip;
}

void ServerTreeWidget::rebuild() {
    if (!m_data) return;

    // preserva itens COLAPSADOS (padrão Halla: tudo expandido — assim usuários
    // que entram depois do primeiro rebuild nunca ficam invisíveis)
    QSet<int> collapsed;
    std::function<void(QTreeWidgetItem*)> save = [&](QTreeWidgetItem* it) {
        if (!it) return;
        if (!it->isExpanded()) collapsed.insert((it->data(0, RoleKind).toInt() << 24) |
                                                it->data(0, RoleId).toInt());
        for (int i = 0; i < it->childCount(); ++i) save(it->child(i));
    };
    for (int i = 0; i < topLevelItemCount(); ++i) save(topLevelItem(i));

    int selKind = currentKind(), selId = currentId();
    blockSignals(true);
    clear();

    // A aba já exibe o nome do servidor no cabeçalho. A árvore usa a raiz
    // invisível do QTreeWidget para que o servidor não seja repetido como um
    // terceiro item visual antes dos canais.
    QTreeWidgetItem* root = invisibleRootItem();
    for (int cid : m_data->childChannels(0))
        buildChannelItem(m_data->channels[cid], root);

    blockSignals(false);

    // restaurar seleção sem criar um nó fictício de servidor
    if (selKind >= 0) {
        std::function<bool(QTreeWidgetItem*)> restore = [&](QTreeWidgetItem* it) -> bool {
            if (it->data(0, RoleKind).toInt() == selKind &&
                it->data(0, RoleId).toInt() == selId) {
                setCurrentItem(it);
                return true;
            }
            for (int i = 0; i < it->childCount(); ++i)
                if (restore(it->child(i))) return true;
            return false;
        };
        for (int i = 0; i < topLevelItemCount(); ++i)
            if (restore(topLevelItem(i))) break;
    } else {
        clearSelection();
    }

    // reexpandir tudo, exceto o que o usuário colapsou explicitamente
    std::function<void(QTreeWidgetItem*)> expand = [&](QTreeWidgetItem* it) {
        const int key = (it->data(0, RoleKind).toInt() << 24) | it->data(0, RoleId).toInt();
        it->setExpanded(!collapsed.contains(key));
        for (int i = 0; i < it->childCount(); ++i) expand(it->child(i));
    };
    for (int i = 0; i < topLevelItemCount(); ++i) expand(topLevelItem(i));
}

QTreeWidgetItem* ServerTreeWidget::buildChannelItem(const Channel& c, QTreeWidgetItem* parentItem) {
    QTreeWidgetItem* item = new QTreeWidgetItem(parentItem);

    bool full = (c.maxClients >= 0 && c.users.size() >= c.maxClients);
    
    // ---- Lógica de Canais "Spacers" (Organização Visual do Servidor)
    bool isSpacer = false;
    bool isCentered = false;
    bool isRepeating = false;
    QString spacerText = c.name;
    
    QRegularExpression rx(QStringLiteral("\\[(\\*?)(c)?spacer[^\\]]*\\](.*)"));
    QRegularExpressionMatch match = rx.match(c.name);
    if (match.hasMatch()) {
        isSpacer = true;
        isCentered = !match.captured(2).isEmpty();
        isRepeating = !match.captured(1).isEmpty();
        spacerText = match.captured(3);
        if (isRepeating && !spacerText.isEmpty()) {
            spacerText = QString(spacerText.at(0)).repeated(50);
        }
    }
    
    QString label = isSpacer ? spacerText : c.name;
    if (!isSpacer && m_showCounts && !c.users.isEmpty())
        label += QStringLiteral("  (%1)").arg(c.users.size());
        
    item->setText(0, label);
    
    if (isSpacer) {
        item->setIcon(0, QIcon()); // Sem ícone para spacers
        if (isCentered) {
            item->setTextAlignment(0, Qt::AlignCenter);
        }
        item->setFlags((item->flags() & ~Qt::ItemIsEnabled & ~Qt::ItemIsSelectable & ~Qt::ItemIsDragEnabled & ~Qt::ItemIsDropEnabled));
        item->setForeground(0, QColor("#8A939B")); // Cinza suave etched
    } else {
        bool canJoin = true;
        if (m_data && m_data->users.contains(m_data->selfId)) {
            const int gid = m_data->users[m_data->selfId].groupId;
            const QJsonObject own = c.groupPerms.value(QString::number(gid)).toObject();
            if (own.contains(QStringLiteral("join"))) canJoin = own.value(QStringLiteral("join")).toBool();
            // Sem traverse em qualquer ancestral também impede entrada.
            for (int parent = c.parentId; parent && m_data->channels.contains(parent); parent = m_data->channels[parent].parentId) {
                const QJsonObject parentPerms = m_data->channels[parent].groupPerms.value(QString::number(gid)).toObject();
                if (parentPerms.contains(QStringLiteral("traverse")) && !parentPerms.value(QStringLiteral("traverse")).toBool()) { canJoin = false; break; }
            }
        }
        item->setIcon(0, c.noSymbol ? QIcon()
            : HIcons::channelAccess(canJoin, c.type == 0, c.hasPassword, full));
        item->setData(0, RoleKind, NodeChannel);
        item->setData(0, RoleId, c.id);
        item->setToolTip(0, channelTooltip(c));
        item->setFlags(item->flags() | Qt::ItemIsDropEnabled | Qt::ItemIsDragEnabled);
        if (c.isDefault) {
            QFont f = item->font(0);
            f.setBold(true);
            item->setFont(0, f);
        }
        if (c.type == 0) { // temporário: nome em cinza-azulado, como no Halla
            item->setForeground(0, QColor("#5C7285"));
        }
    }

    // Ordena os usuários do canal baseado na propriedade groupOrder (ordem hierárquica dos cargos)
    // Menor valor de groupOrder significa maior prioridade/mais alto no topo.
    // Se empatado, ordena em ordem alfabética de apelido.
    QList<int> sortedUsers;
    for (int uid : c.users) {
        if (m_data->users.contains(uid)) {
            sortedUsers << uid;
        }
    }
    std::sort(sortedUsers.begin(), sortedUsers.end(), [&](int uidA, int uidB) {
        const User& uA = m_data->users[uidA];
        const User& uB = m_data->users[uidB];
        if (uA.groupOrder != uB.groupOrder) {
            return uA.groupOrder < uB.groupOrder;
        }
        return uA.name.localeAwareCompare(uB.name) < 0;
    });

    // Halla: por padrão clientes aparecem acima dos subcanais; com a opção
    // "Classificar clientes abaixo dos canais" os subcanais vêm primeiro.
    if (!m_sortClientsBelow) {
        for (int uid : sortedUsers)
            addUserItem(item, m_data->users[uid]);
        for (int cid : m_data->childChannels(c.id))
            buildChannelItem(m_data->channels[cid], item);
    } else {
        for (int cid : m_data->childChannels(c.id))
            buildChannelItem(m_data->channels[cid], item);
        for (int uid : sortedUsers)
            addUserItem(item, m_data->users[uid]);
    }

    if (!isSpacer) {
        item->setChildIndicatorPolicy(item->childCount() > 0
            ? QTreeWidgetItem::ShowIndicator
            : QTreeWidgetItem::DontShowIndicatorWhenChildless);
    }
    return item;
}

// ------------------------------------------------------------------ expansão
void ServerTreeWidget::expandChannelsToLevel(int level) {
    collapseAll();
    expandToDepth(qMax(0, level));
}

void ServerTreeWidget::expandOwnChannelOnly(int channelId) {
    collapseAll();
    std::function<bool(QTreeWidgetItem*)> find = [&](QTreeWidgetItem* it) -> bool {
        if (it->data(0, RoleKind).toInt() == NodeChannel &&
            it->data(0, RoleId).toInt() == channelId) {
            for (QTreeWidgetItem* p = it; p; p = p->parent()) p->setExpanded(true);
            return true;
        }
        for (int i = 0; i < it->childCount(); ++i)
            if (find(it->child(i))) return true;
        return false;
    };
    for (int i = 0; i < topLevelItemCount(); ++i)
        if (find(topLevelItem(i))) break;
}

void ServerTreeWidget::addUserItem(QTreeWidgetItem* chanItem, const User& u) {
    QTreeWidgetItem* item = new QTreeWidgetItem(chanItem);
    
    // ---- Se o usuário possui sigla (prefixo de cargo), exibe antes do nome!
    QString displayName = u.name;
    if (!u.sigla.isEmpty()) {
        displayName = u.sigla + " " + displayName;
    }
    if (u.screensharing) {
        displayName += QStringLiteral(" 🔴 LIVE");
    }
    item->setText(0, displayName);
    item->setIcon(0, leadingUserIcon(u, m_delegate ? m_delegate->showMinis() : true));
    item->setData(0, RoleKind, NodeUser);
    item->setData(0, RoleId, u.id);
    item->setToolTip(0, userTooltip(u));
    const bool draggable = (u.id == m_data->selfId) || m_canMoveOthers;
    item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable |
                   (draggable ? Qt::ItemIsDragEnabled : Qt::NoItemFlags));
    if (u.away) item->setForeground(0, QColor("#6E7B86"));
}

// ------------------------------------------------------------------ menus de contexto
void ServerTreeWidget::contextMenuEvent(QContextMenuEvent* e) {
    QTreeWidgetItem* it = itemAt(e->pos());
    // Guarda a seleção antes de chamar setCurrentItem(): em algumas versões
    // do Qt essa chamada limpa a seleção estendida mesmo quando o item clicado
    // já estava selecionado.
    const QList<int> selectedBefore = selectedChannelIds();
    const bool clickedWasSelected = it && it->isSelected();
    if (it) {
        // Um clique direito em um item já selecionado preserva a seleção
        // estendida. Em um item fora dela, começa uma nova seleção.
        if (!clickedWasSelected) {
            clearSelection();
            it->setSelected(true);
        }
        setCurrentItem(it);
    }

    const int kind = it ? it->data(0, RoleKind).toInt() : NodeServer;
    const int id   = it ? it->data(0, RoleId).toInt() : 0;
    const QList<int> selectedChannels = clickedWasSelected && selectedBefore.size() >= 2
        ? selectedBefore : selectedChannelIds();
    QMenu menu(this);

    // As ações ficam disponíveis independentemente de o clique direito ter
    // caído no canal, em um usuário ou na raiz do servidor. O que importa é a
    // seleção múltipla preservada acima.
    if (selectedChannels.size() >= 2) {
        const QList<int> ids = selectedChannels;
        menu.addAction(tr("Vincular canais selecionados"), this,
                       [this, ids] { emit channelLinkRequested(ids, true); });
        menu.addAction(tr("Desvincular canais selecionados"), this,
                       [this, ids] { emit channelLinkRequested(ids, false); });
        menu.addSeparator();
    }

    if (!it || kind == NodeServer) {
        // menu do servidor — como clicar na aba do servidor no Halla
        menu.addAction(HIcons::disconnectPlug(), tr("Desconectar"), this,
                       [this] { emit disconnectRequested(); });
        menu.addSeparator();
        menu.addAction(HIcons::editPencil(), tr("Editar servidor virtual"), this,
                       [this] { emit editVirtualServerRequested(); });
        menu.addSeparator();
        menu.addAction(HIcons::bookmarkStar(), tr("Adicionar aos favoritos"), this,
                       [this] { emit addBookmarkRequested(); });
    } else if (kind == NodeChannel) {
        const Channel& c = m_data->channels[id];
        const bool inside = c.users.contains(m_data->selfId);

        QAction* join = menu.addAction(HIcons::check(), tr("Alternar para o canal"), this,
                       [this, id] { emit joinChannelRequested(id); });
        join->setEnabled(!inside);
        menu.addSeparator();
        menu.addAction(HIcons::fileNew(), tr("Criar canal"), this,
                       [this] { emit createChannelRequested(0); });
        menu.addAction(HIcons::fileNew(), tr("Criar sub-canal"), this,
                       [this, id] { emit createChannelRequested(id); });
        menu.addSeparator();
        menu.addAction(HIcons::editPencil(), tr("Editar canal"), this,
                       [this, id] { emit editChannelRequested(id); });
        menu.addAction(HIcons::trash(), tr("Excluir canal"), this,
                       [this, id] { emit deleteChannelRequested(id); });
        menu.addSeparator();
        menu.addAction(HIcons::info(), tr("Ver descrição do canal"), this,
                       [this, id] { emit channelDescriptionRequested(id); });
    } else if (kind == NodeUser) {
        const User& u = m_data->users[id];
        const bool self = (id == m_data->selfId);

        QAction* msg = menu.addAction(tr("Enviar mensagem"), this,
            [this, id] { emit privateMessageRequested(id); });
        msg->setEnabled(!self);

        menu.addAction(tr("Ver avatar"), this,
                       [this, id] { emit viewAvatarRequested(id); });
        menu.addAction(tr("Ver informações do cliente"), this,
                       [this, id] { emit userInfoRequested(id); });
        menu.addSeparator();

        if (self) {
            if (m_canSetSelfCommander) {
                const QString label = u.commander ? tr("Remover comandante do canal")
                                                   : tr("Conceder comandante do canal");
                const bool turnOn = !u.commander;
                menu.addAction(label, this, [this, id, turnOn] {
                    emit commanderRequested(id, turnOn);
                });
            }
            menu.addAction(tr("Alterar apelido"), this, [this] { emit renameRequested(); });
            menu.addAction(tr("Definir descrição do cliente"), this,
                           [this] { emit setDescriptionRequested(); });
        } else {
            menu.addAction(tr("Cutucar"), this, [this, id] { emit pokeRequested(id); });
            menu.addAction(tr("Registrar reclamação..."), this,
                           [this, id] { emit complaintRequested(id); });
            menu.addSeparator();
            menu.addAction(tr("Definir volume..."), this,
                           [this, id] { emit volumeRequested(id); });
            menu.addAction(tr("Silenciar"), this,
                           [this, id, u] { emit localMuteToggled(id, !u.locallyMuted); })
                ->setCheckable(false);
            menu.addSeparator();
            if (m_canSetOtherCommander) {
                const QString label = u.commander ? tr("Revogar comandante do canal")
                                                   : tr("Conceder comandante do canal");
                const bool turnOn = !u.commander;
                menu.addAction(label, this, [this, id, turnOn] {
                    emit commanderRequested(id, turnOn);
                });
            }
            menu.addSeparator();
            QMenu* kick = menu.addMenu(tr("Expulsar"));
            kick->addAction(tr("Expulsar do canal"), this,
                            [this, id] { emit kickRequested(id, false); });
            kick->addAction(tr("Expulsar do servidor"), this,
                            [this, id] { emit kickRequested(id, true); });
            menu.addAction(tr("Banir cliente"), this, [this, id] { emit banRequested(id); });
            menu.addSeparator();
            menu.addAction(tr("Mover para o seu canal"), this,
                           [this, id] { emit moveToMyChannelRequested(id); });
        }
    }

    menu.exec(e->globalPos());
}

void ServerTreeWidget::mouseDoubleClickEvent(QMouseEvent* e) {
    QTreeWidgetItem* it = itemAt(e->pos());
    if (it && it->data(0, RoleKind).toInt() == NodeChannel) {
        emit joinChannelRequested(it->data(0, RoleId).toInt());
        return;
    }
    if (it && it->data(0, RoleKind).toInt() == NodeUser &&
        it->data(0, RoleId).toInt() != m_data->selfId) {
        emit privateMessageRequested(it->data(0, RoleId).toInt());
        return;
    }
    QTreeWidget::mouseDoubleClickEvent(e);
}

// ------------------------------------------------------------------ arrastar e soltar
QStringList ServerTreeWidget::mimeTypes() const {
    return { QStringLiteral("application/x-halla-userid"),
             QStringLiteral("application/x-halla-channelid") };
}

QMimeData* ServerTreeWidget::mimeData(const QList<QTreeWidgetItem*>& items) const {
    QMimeData* md = new QMimeData;
    if (items.isEmpty()) return md;
    const int kind = items.first()->data(0, RoleKind).toInt();
    const int id = items.first()->data(0, RoleId).toInt();
    if (kind == NodeUser) {
        md->setData(QStringLiteral("application/x-halla-userid"), QByteArray::number(id));
    } else if (kind == NodeChannel) {
        md->setData(QStringLiteral("application/x-halla-channelid"), QByteArray::number(id));
    }
    return md;
}

void ServerTreeWidget::dropEvent(QDropEvent* event) {
    if (!m_data || !event || !event->mimeData()) {
        if (event) event->ignore();
        return;
    }

    const QMimeData* data = event->mimeData();
    QTreeWidgetItem* target = itemAt(event->position().toPoint());
    const QAbstractItemView::DropIndicatorPosition indicator = dropIndicatorPosition();

    if (data->hasFormat(QStringLiteral("application/x-halla-channelid"))) {
        const int channelId = data->data(QStringLiteral("application/x-halla-channelid")).toInt();
        if (!m_data->channels.contains(channelId) || channelId == 1) {
            event->ignore();
            return;
        }

        int parentId = 0;
        int order = m_data->childChannels(0).size();

        if (target && target->data(0, RoleKind).toInt() == NodeChannel) {
            const int targetId = target->data(0, RoleId).toInt();
            if (indicator == QAbstractItemView::OnItem) {
                // Soltar sobre o corpo do canal cria um subcanal de verdade.
                parentId = targetId;
                order = m_data->childChannels(parentId).size();
            } else {
                // Acima/abaixo mantém o canal no mesmo nível do alvo.
                QTreeWidgetItem* container = target->parent();
                parentId = container &&
                           container->data(0, RoleKind).toInt() == NodeChannel
                    ? container->data(0, RoleId).toInt() : 0;
                order = container ? container->indexOfChild(target)
                                  : indexOfTopLevelItem(target);
                if (indicator == QAbstractItemView::BelowItem) ++order;
            }
        } else if (target && target->data(0, RoleKind).toInt() == NodeServer) {
            parentId = 0;
            order = m_data->childChannels(0).size();
        } else if (target) {
            // Não transforma um arraste sobre um usuário em movimento
            // estrutural acidental.
            event->ignore();
            return;
        }

        emit channelMoveRequested(channelId, parentId, qMax(0, order));
        event->acceptProposedAction();
        return;
    }

    if (data->hasFormat(QStringLiteral("application/x-halla-userid"))) {
        const int userId = data->data(QStringLiteral("application/x-halla-userid")).toInt();
        if (userId != m_data->selfId && !m_canMoveOthers) {
            event->ignore();
            return;
        }

        // Só um canal real é um destino válido. Arrastar para o espaço vazio
        // da árvore é rejeitado, impedindo que o usuário fique "no nada".
        int targetChannel = 0;
        if (target && target->data(0, RoleKind).toInt() == NodeChannel) {
            targetChannel = target->data(0, RoleId).toInt();
        } else if (target && target->data(0, RoleKind).toInt() == NodeServer) {
            for (const Channel& channel : m_data->channels) {
                if (channel.isDefault) {
                    targetChannel = channel.id;
                    break;
                }
            }
        }
        if (targetChannel <= 0 || !m_data->channels.contains(targetChannel)) {
            event->ignore();
            return;
        }
        emit moveUserRequested(userId, targetChannel);
        event->acceptProposedAction();
        return;
    }

    event->ignore();
}

bool ServerTreeWidget::dropMimeData(QTreeWidgetItem* parent, int index, const QMimeData* data,
                                    Qt::DropAction) {
    if (!m_data || !parent) return false;
    const int parentKind = parent->data(0, RoleKind).toInt();
    if (parentKind != NodeChannel && parentKind != NodeServer) return false;

    if (data->hasFormat(QStringLiteral("application/x-halla-channelid"))) {
        const int channelId = data->data(QStringLiteral("application/x-halla-channelid")).toInt();
        if (!m_data->channels.contains(channelId)) return false;
        const Channel& source = m_data->channels[channelId];
        int targetParent = 0;
        int targetIndex = index;
        if (parentKind == NodeChannel) {
            QTreeWidgetItem* siblingParent = parent->parent();
            targetParent = siblingParent ? siblingParent->data(0, RoleId).toInt() : 0;
            targetIndex = siblingParent ? siblingParent->indexOfChild(parent)
                                        : indexOfTopLevelItem(parent);
            const int targetChannelId = parent->data(0, RoleId).toInt();
            if (source.parentId == targetParent && m_data->channels.contains(targetChannelId)
                    && source.order < m_data->channels[targetChannelId].order)
                targetIndex = qMax(0, targetIndex - 1);
        } else if (targetIndex < 0) {
            targetIndex = topLevelItemCount();
        }
        emit channelMoveRequested(channelId, targetParent, qMax(0, targetIndex));
        return true;
    }

    if (!data->hasFormat(QStringLiteral("application/x-halla-userid"))) return false;
    const int uid = data->data(QStringLiteral("application/x-halla-userid")).toInt();
    if (uid != m_data->selfId && !m_canMoveOthers) return false;
    const int target = parentKind == NodeServer
                         ? m_data->channels.begin().key()
                         : parent->data(0, RoleId).toInt();
    emit moveUserRequested(uid, target);
    return true;
}

void ServerTreeWidget::onItemEntered(QTreeWidgetItem* item, int column) {
    Q_UNUSED(column);
    if (!item || !m_data) return;
    int kind = item->data(0, RoleKind).toInt();
    int id = item->data(0, RoleId).toInt();
    if (kind == NodeUser && m_data->users.contains(id)) {
        const User& u = m_data->users[id];
        if (u.screensharing) {
            int chanId = m_data->channelOfUser(id);
            QPoint pos = QCursor::pos();
            emit screenshareHovered(id, chanId, pos);
        }
    }
}
