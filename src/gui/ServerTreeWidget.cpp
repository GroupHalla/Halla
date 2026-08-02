#include "ServerTreeWidget.h"
#include "Icons.h"
#include "Settings.h"

#include <QContextMenuEvent>
#include <QMouseEvent>
#include <QMenu>
#include <QMimeData>
#include <QPainter>
#include <QSet>
#include <QStyle>

// ============================================================== Delegado
void ServerRowDelegate::paint(QPainter* p, const QStyleOptionViewItem& opt,
                              const QModelIndex& index) const {
    QStyledItemDelegate::paint(p, opt, index);

    if (!m_data || !m_showMinis) return;
    if (index.data(RoleKind).toInt() != NodeUser) return;

    const int uid = index.data(RoleId).toInt();
    if (!m_data->users.contains(uid)) return;
    const User& u = m_data->users[uid];

    QPixmap minis = HIcons::userStatusMinis(u.inputMuted, u.outputMuted || u.locallyMuted,
                                            u.away, u.recording, u.commander);
    if (minis.isNull()) return;

    QStyleOptionViewItem o = opt;
    initStyleOption(&o, index);
    const QStyle* style = o.widget ? o.widget->style() : nullptr;
    const int iconW = o.decorationSize.width();
    QRect textRect = style ? style->subElementRect(QStyle::SE_ItemViewItemText, &o, o.widget)
                           : o.rect;
    QFontMetrics fm(o.font);
    const int textW = fm.horizontalAdvance(o.text);

    int x = textRect.left() + iconW + 6 + textW + 8;
    const int maxX = o.rect.right() - 4;
    const int y = o.rect.top() + (o.rect.height() - minis.height()) / 2;
    int w = minis.width();
    if (x + w > maxX) w = maxX - x;
    if (w > 0) p->drawPixmap(x, y, minis.copy(0, 0, w, minis.height()));
}

// ============================================================== Árvore
ServerTreeWidget::ServerTreeWidget(QWidget* parent) : QTreeWidget(parent) {
    setHeaderHidden(true);
    setIndentation(18);
    setRootIsDecorated(true);
    setAnimated(false);
    setUniformRowHeights(true);
    setAllColumnsShowFocus(true);
    setSelectionMode(SingleSelection);
    setDragDropMode(InternalMove);
    setIconSize(QSize(20, 20));
    setFrameShape(QFrame::StyledPanel);

    m_delegate = new ServerRowDelegate(this);
    setItemDelegate(m_delegate);

    setStyleSheet(QStringLiteral(
        "QTreeWidget { background: #FFFFFF; color: #202020; alternate-background-color: #F7F9FB; "
        "  border: 1px solid #C9CDD2; }"
        "QTreeWidget::item { height: 21px; padding: 0px; }"
        "QTreeWidget::item:selected { background: #3B76B0; color: #FFFFFF; }"
        "QTreeWidget::item:hover:!selected { background: #E4EEF8; }"
        "QTreeWidget::branch { background: transparent; }"));

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
    static const char* tnames[] = { "Temporário", "Semi-permanente", "Permanente" };
    tip += QStringLiteral("<br>%1").arg(QString::fromUtf8(tnames[c.type]));
    return tip;
}

void ServerTreeWidget::rebuild() {
    if (!m_data) return;

    // preserva itens COLAPSADOS (padrão TS3: tudo expandido — assim usuários
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

    // raiz: servidor
    QTreeWidgetItem* root = new QTreeWidgetItem(this);
    root->setText(0, m_data->name);
    root->setIcon(0, HIcons::server());
    root->setData(0, RoleKind, NodeServer);
    root->setData(0, RoleId, 0);
    QFont f = root->font(0);
    f.setBold(true);
    root->setFont(0, f);
    root->setToolTip(0, QStringLiteral("%1<br>%2").arg(m_data->name.toHtmlEscaped(),
                                                       m_data->address.toHtmlEscaped()));
    root->setFlags(root->flags() | Qt::ItemIsDropEnabled);
    root->setFlags(root->flags() & ~Qt::ItemIsDragEnabled);

    for (int cid : m_data->childChannels(0))
        buildChannelItem(m_data->channels[cid], root);

    addTopLevelItem(root);
    root->setExpanded(!collapsed.contains(NodeServer << 24));

    blockSignals(false);

    // restaurar seleção
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
        setCurrentItem(root);
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
    QString label = c.name;
    if (m_showCounts && !c.users.isEmpty())
        label += QStringLiteral("  (%1)").arg(c.users.size());
    item->setText(0, label);
    item->setIcon(0, HIcons::channel(c.hasPassword, c.moderated, c.isDefault, full));
    item->setData(0, RoleKind, NodeChannel);
    item->setData(0, RoleId, c.id);
    item->setToolTip(0, channelTooltip(c));
    item->setFlags((item->flags() | Qt::ItemIsDropEnabled) & ~Qt::ItemIsDragEnabled);
    if (c.isDefault) {
        QFont f = item->font(0);
        f.setBold(true);
        item->setFont(0, f);
    }
    if (c.type == 0) { // temporário: nome em cinza-azulado, como no TS3
        item->setForeground(0, QColor("#5C7285"));
    }

    for (int uid : c.users)
        if (m_data->users.contains(uid))
            addUserItem(item, m_data->users[uid]);

    for (int cid : m_data->childChannels(c.id))
        buildChannelItem(m_data->channels[cid], item);

    return item;
}

void ServerTreeWidget::addUserItem(QTreeWidgetItem* chanItem, const User& u) {
    QTreeWidgetItem* item = new QTreeWidgetItem(chanItem);
    item->setText(0, u.name);
    item->setIcon(0, HIcons::user(u.talking, u.away));
    item->setData(0, RoleKind, NodeUser);
    item->setData(0, RoleId, u.id);
    item->setToolTip(0, userTooltip(u));
    item->setFlags((Qt::ItemIsEnabled | Qt::ItemIsSelectable |
                    (u.id == m_data->selfId ? Qt::ItemIsDragEnabled : Qt::NoItemFlags)));
    if (u.away) item->setForeground(0, QColor("#6E7B86"));
}

// ------------------------------------------------------------------ menus de contexto
void ServerTreeWidget::contextMenuEvent(QContextMenuEvent* e) {
    QTreeWidgetItem* it = itemAt(e->pos());
    if (it) setCurrentItem(it);

    const int kind = it ? it->data(0, RoleKind).toInt() : NodeServer;
    const int id   = it ? it->data(0, RoleId).toInt() : 0;
    QMenu menu(this);

    if (!it || kind == NodeServer) {
        // menu do servidor — como clicar na aba do servidor no TS3
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
    } else if (kind == NodeUser) {
        const User& u = m_data->users[id];
        const bool self = (id == m_data->selfId);

        QAction* msg = menu.addAction(tr("Enviar mensagem"), this,
            [this, id] { emit privateMessageRequested(id); });
        msg->setEnabled(!self);

        menu.addAction(tr("Ver avatar"), this, [this] { emit viewAvatarRequested(); });
        menu.addSeparator();

        if (self) {
            menu.addAction(tr("Alterar apelido"), this, [this] { emit renameRequested(); });
            menu.addAction(tr("Definir descrição do cliente"), this,
                           [this] { emit setDescriptionRequested(); });
        } else {
            menu.addAction(tr("Cutucar"), this, [this, id] { emit pokeRequested(id); });
            menu.addSeparator();
            menu.addAction(tr("Definir volume..."), this,
                           [this, id] { emit volumeRequested(id); });
            menu.addAction(tr("Silenciar"), this,
                           [this, id, u] { emit localMuteToggled(id, !u.locallyMuted); })
                ->setCheckable(false);
            menu.addSeparator();
            menu.addAction(tr("Conceder/Revogar comandante do canal"), this,
                           [this] { emit commanderToggled(); });
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
    return { QStringLiteral("application/x-halla-userid") };
}

QMimeData* ServerTreeWidget::mimeData(const QList<QTreeWidgetItem*>& items) const {
    QMimeData* md = new QMimeData;
    if (!items.isEmpty() && items.first()->data(0, RoleKind).toInt() == NodeUser)
        md->setData(QStringLiteral("application/x-halla-userid"),
                    QByteArray::number(items.first()->data(0, RoleId).toInt()));
    return md;
}

bool ServerTreeWidget::dropMimeData(QTreeWidgetItem* parent, int, const QMimeData* data,
                                    Qt::DropAction) {
    if (!m_data || !parent) return false;
    if (!data->hasFormat(QStringLiteral("application/x-halla-userid"))) return false;
    if (parent->data(0, RoleKind).toInt() != NodeChannel &&
        parent->data(0, RoleKind).toInt() != NodeServer)
        return false;

    const int uid = data->data(QStringLiteral("application/x-halla-userid")).toInt();
    if (uid != m_data->selfId) return false; // só é possível mover a si mesmo

    int target = parent->data(0, RoleKind).toInt() == NodeServer
                     ? m_data->channels.begin().key()
                     : parent->data(0, RoleId).toInt();
    emit joinChannelRequested(target);
    return true;
}
