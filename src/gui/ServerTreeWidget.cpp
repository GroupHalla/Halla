#include "ServerTreeWidget.h"
#include "Icons.h"
#include "Settings.h"
#include "core/BadgeRegistry.h"
#include "core/GroupIconCache.h"

#include <QContextMenuEvent>
#include <QMouseEvent>
#include <QMenu>
#include <QSettings>
#include <QScrollBar>
#include <QGuiApplication>
#include <QMimeData>
#include <QDropEvent>
#include <QPainter>
#include <QLinearGradient>
#include <QSet>
#include <QHash>
#include <QStyle>
#include <QRegularExpression>
#include <algorithm>
#include <QFile>
#include <QDir>

// isImageIconName/shouldRequestIcon now live in GroupIconCache (shared with the
// information panel, which also renders role icons -- same rule for what counts as
// an image and the same request quota).

static QPixmap createGroupIconPixmap(const QString& iconText, const QString& serverKey,
                                     ServerTreeWidget* tree) {
    if (iconText.isEmpty()) return QPixmap();

    // If the icon is an image (file extension): in-memory + on-disk cache scoped per
    // server (GroupIconCache -- QStandardPaths, writable even with the app installed
    // in Program Files; the old relative "cache/icons/" never wrote anything and the
    // image never appeared).
    if (GroupIconCache::isImageName(iconText)) {
        const QPixmap pm = GroupIconCache::instance().pixmap(serverKey, iconText);
        if (tree && GroupIconCache::shouldRequest(serverKey + QLatin1Char('|') + iconText, !pm.isNull()))
            emit tree->iconRequested(iconText);
        return pm;
    }

    // v1.1.35: the TEXT/emoji role icon is a pure function of iconText -- but it was
    // redrawn (QFont + drawRoundedRect + drawText) on EVERY paint of every visible
    // delegate row, including while scrolling. Cache by text: the cost becomes a hash
    // lookup.
    static QHash<QString, QPixmap> textPixmapCache;
    const auto hit = textPixmapCache.constFind(iconText);
    if (hit != textPixmapCache.constEnd()) return hit.value();

    // Larger role icons (24x21): at 16x14 they were tiny next to the name -- the same
    // size as the image cache (GroupIconCache) so the row looks uniform between image
    // icons and letter/emoji icons.
    QPixmap pm(24, 21);
    pm.fill(Qt::transparent);
    QPainter painter(&pm);
    painter.setRenderHint(QPainter::TextAntialiasing);

    if (iconText.length() <= 2) {
        QFont font = painter.font();
        font.setPixelSize(15);
        painter.setFont(font);
        painter.setPen(Qt::black);
        painter.drawText(QRect(0, 0, 24, 21), Qt::AlignCenter, iconText);
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
        painter.drawRoundedRect(QRectF(1, 1, 22, 19), 4, 4);
        
        painter.setPen(Qt::white);
        QFont font = painter.font();
        font.setPixelSize(12);
        font.setBold(true);
        painter.setFont(font);
        painter.drawText(QRect(0, 0, 24, 21), Qt::AlignCenter, iconText.left(1).toUpper());
    }

    textPixmapCache.insert(iconText, pm);
    return pm;
}

// Statuses are placed in the same leading area as the user's avatar. The
// reference shows microphone/headphone/away indicators before the nickname,
// never after it.
static QPixmap liveBadgePixmap() {
    // v1.1.35: static pixmap -- the delegate requested a new one (with drawText) on
    // every paint of every row during a live stream.
    static const QPixmap pm = [] {
        QPixmap pm(54, 18);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing, true);
        QRectF r(0.5, 1.5, 53, 15);
        p.setPen(QPen(QColor("#EF4444"), 1));
        p.setBrush(QColor("#B91C1C"));
        p.drawRoundedRect(r, 8, 8);
        p.setPen(Qt::white);
        QFont f = p.font();
        f.setPixelSize(9);
        f.setBold(true);
        p.setFont(f);
        p.drawText(QRect(0, 0, 54, 18), Qt::AlignCenter, QStringLiteral("● LIVE"));
        return pm;
    }();
    return pm;
}

static QPixmap treeUserSphere(const User& u) {
    // v1.1.35: the ball is a pure function of (away, talking, whispering, theme) -- 16
    // variants at most. It was generated with a gradient + QPainter PER USER on every
    // rebuild (which ran on every speech transition); with updateUserVisuals it only
    // runs when the key changes -- and now not even that draws: it is a cache lookup.
    const bool dark = HTheme::isDark();
    const int key = (u.away ? 1 : 0) | (u.talking ? 2 : 0)
                  | (u.whispering ? 4 : 0) | (dark ? 8 : 0);
    static QPixmap cache[16];
    if (!cache[key].isNull()) return cache[key];

    QPixmap pm(16, 16);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
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
    cache[key] = pm;
    return pm;
}

// v1.1.35: key of EVERYTHING the user row icon/colors display. Speech transitions
// change only this -- comparing the key allows updating the row without rebuilding
// the tree. (showMinis/theme are part of it because the drawing depends on them.)
static quint32 userVisualKey(const User& u, bool showMinis) {
    return (u.talking ? 1u : 0u) | (u.whispering ? 2u : 0u)
         | (u.away ? 4u : 0u) | (u.inputMuted ? 8u : 0u)
         | (u.outputMuted ? 16u : 0u) | (u.locallyMuted ? 32u : 0u)
         | (u.recording ? 64u : 0u) | (u.commander ? 128u : 0u)
         | (u.op ? 256u : 0u) | (showMinis ? 512u : 0u)
         | (HTheme::isDark() ? 1024u : 0u);
}

static QIcon leadingUserIcon(const User& u, bool showStatus) {
    const bool outputMuted = u.outputMuted || u.locallyMuted;
    const bool audioMuted = u.inputMuted || outputMuted;
    const QPixmap avatar = treeUserSphere(u);

    // The blue indicator is the normal state. When audio input or output is blocked it
    // is replaced by the matching indicators before the name: crossed-out microphone
    // and/or headset with a padlock. That way there are no two competing signals and no
    // doubt about the user's real state.
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
    // Cache scope per server: two servers may have icons with the SAME file name and
    // different images.
    const QString serverKey = GroupIconCache::serverKey(m_data->address);

    // We split and render multiple icons/roles when present (separated by comma or
    // semicolon)
    // v1.1.35: static regex -- compiling the expression on every paint of every row was
    // pure waste (paint runs in bursts while scrolling/hovering).
    static const QRegularExpression splitRoleIcons("[,;]");
    QStringList icons = u.groupIcon.split(splitRoleIcons, Qt::SkipEmptyParts);
    QList<QPixmap> iconPms;
    for (const QString& ic : icons) {
        QString trimmed = ic.trimmed();
        if (!trimmed.isEmpty()) {
            QPixmap pm = createGroupIconPixmap(trimmed, serverKey, tree);
            if (!pm.isNull()) iconPms << pm;
        }
    }
    // Global badges are signed by the official registry and bound to the UID.
    // v1.1.31: this option is now honored (before, badges were always drawn, ignoring
    // the preference) and defaults to ON. The flag lives in the delegate (not in
    // QSettings) because paint runs every frame.
    if (m_showBadges) {
        for (const GlobalBadge& badge : BadgeRegistry::instance().badgesForUid(u.uniqueId)) {
            if (!badge.icon.isNull()) iconPms << badge.icon;
        }
    }
    const QPixmap liveBadge = u.screensharing ? liveBadgePixmap() : QPixmap();

    QStyleOptionViewItem o = opt;
    initStyleOption(&o, index);
    const QStyle* style = o.widget ? o.widget->style() : nullptr;
    QRect textRect = style ? style->subElementRect(QStyle::SE_ItemViewItemText, &o, o.widget)
                           : o.rect;
    QFontMetrics fm(o.font);
    const int textW = fm.horizontalAdvance(o.text);

    int liveRightLimit = textRect.left() + textW;
    if (!liveBadge.isNull()) {
        const int liveX = qMin(textRect.left() + textW + 8,
                               opt.rect.right() - liveBadge.width() - 4);
        const int liveY = opt.rect.top() + (opt.rect.height() - liveBadge.height()) / 2;
        if (liveX > textRect.left()) {
            p->drawPixmap(liveX, liveY, liveBadge);
            liveRightLimit = liveX + liveBadge.width();
        }
    }

    int totalW = 0;
    int maxHeight = 0;
    for (const QPixmap& pm : iconPms) {
        totalW += pm.width() + 4;
        maxHeight = qMax(maxHeight, pm.height());
    }
    if (totalW == 0) return;
    totalW -= 4;

    QPixmap combined(totalW, maxHeight);
    combined.fill(Qt::transparent);
    QPainter cp(&combined);

    int currentX = 0;
    for (const QPixmap& pm : iconPms) {
        cp.drawPixmap(currentX, (maxHeight - pm.height()) / 2, pm);
        currentX += pm.width() + 4;
    }

    int x = o.rect.right() - combined.width() - 8;
    const int rightLimit = liveRightLimit + 8;
    if (x < rightLimit) x = rightLimit;
    const int y = o.rect.top() + (o.rect.height() - combined.height()) / 2;
    int w = combined.width();
    const int maxX = o.rect.right() - 4;
    if (x + w > maxX) w = maxX - x;
    if (w > 0) p->drawPixmap(x, y, combined.copy(0, 0, w, combined.height()));
}

QSize ServerRowDelegate::sizeHint(const QStyleOptionViewItem& option,
                                  const QModelIndex& index) const {
    QSize size = QStyledItemDelegate::sizeHint(option, index);
    if (index.data(RoleKind).toInt() == NodeUser) size.setHeight(qMax(size.height(), 30));
    return size;
}

// ================================================================ Tree
ServerTreeWidget::ServerTreeWidget(QWidget* parent) : QTreeWidget(parent) {
    setHeaderHidden(true);
    setIndentation(12);
    setRootIsDecorated(true);
    setAnimated(false);
    setUniformRowHeights(true);
    setAllColumnsShowFocus(true);
    // Allows selecting several channels for batch operations, such as linking/unlinking
    // audio. Users keep working normally with extended selection.
    setSelectionMode(QAbstractItemView::ExtendedSelection);
    setDragDropMode(InternalMove);
    setDefaultDropAction(Qt::MoveAction);
    // Reserves width for the avatar and up to two audio indicators before the name.
    // When audio is blocked, the avatar stops occupying that space.
    setIconSize(QSize(48, 24));
    setFrameShape(QFrame::NoFrame);

    m_delegate = new ServerRowDelegate(this);
    setItemDelegate(m_delegate);

    setMouseTracking(true);
    connect(this, &QTreeWidget::itemEntered, this, &ServerTreeWidget::onItemEntered);
    connect(&BadgeRegistry::instance(), &BadgeRegistry::updated, this, [this] {
        if (m_data) rebuild();
    });

    // the colors come from the global theme (HTheme) -- a fixed stylesheet here
    // prevented the dark theme on Windows
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

// v1.1.31: "Show badge icons in clients" (Options > Appearance). Kept in memory
// (paint queries it every frame) -- QSettings is read only in ServerTab's
// applyDisplayOptions.
void ServerTreeWidget::setShowBadges(bool on) {
    m_showBadges = on;
    m_delegate->setShowBadges(on);

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
    if (u.id == m_data->selfId) tip += tr("(você)<br>");
    if (u.away)        tip += QStringLiteral("Ausente<br>");
    if (u.inputMuted)  tip += QStringLiteral("Microfone mudo<br>");
    if (u.outputMuted) tip += QStringLiteral("Alto-falantes mudos<br>");
    if (u.recording)   tip += QStringLiteral("Gravando<br>");
    if (u.commander)   tip += QStringLiteral("Comandante do canal<br>");
    // Each role arrives as "<icon> <name>" (e.g. "rota.png ROTA") -- here we show the
    // real IMAGE icon (via file:// of the on-disk cache; QToolTip rich text renders
    // <img> from a file) followed by the role NAME. Before, the whole line was printed
    // and the tooltip literally showed "rota.png ROTA". Icon without bytes yet: name
    // only (the visible row's delegate already requested the icon_get; the next hover
    // shows the image).
    const QString serverKey = GroupIconCache::serverKey(m_data->address);
    QStringList roles;
    const QStringList lines = u.serverGroups.split(QStringLiteral("\n"));
    for (const QString& line : lines) {
        const QString trimmed = line.trimmed();
        if (trimmed.isEmpty()) continue;
        QString iconName, label;
        GroupIconCache::splitRoleLine(trimmed, &iconName, &label);
        if (iconName.isEmpty()) {
            roles << trimmed.toHtmlEscaped();
            continue;
        }
        const QString path = GroupIconCache::iconFilePath(serverKey, iconName);
        if (!path.isEmpty()) {
            roles << QStringLiteral("<img src=\"file:///%1\" width=\"24\" height=\"21\" "
                                    "style=\"vertical-align:middle\"/> %2")
                         .arg(path, label.toHtmlEscaped());
        } else {
            roles << label.toHtmlEscaped();
        }
    }
    if (!roles.isEmpty())
        tip += QStringLiteral("Grupos: %1").arg(roles.join(QStringLiteral(", ")));
    // Badge names in the tooltip follow the same option as the icons
    // (design/showBadgeIcons -- v1.1.31).
    if (m_showBadges) {
        const QStringList globalBadges = BadgeRegistry::instance().badgeNamesForUid(u.uniqueId);
        if (!globalBadges.isEmpty())
            tip += QStringLiteral("<br>Emblemas oficiais: %1")
                       .arg(globalBadges.join(QStringLiteral(", ")).toHtmlEscaped());
    }
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

    // Preserves the scroll position: rebuild() runs on every speech transition
    // (user_state -> refreshServerState, coalesced to 120 ms) and clear() resets the
    // scroll -- the channel list "jumped/scrolled by itself" when someone spoke or when
    // the user pressed push-to-talk. setCurrentItem in the selection restore also
    // scrolls to the item; saving beforehand and restoring last covers both cases. If
    // the content shrank (a user left), setValue clamps to the maximum automatically.
    const int savedScrollY = verticalScrollBar() ? verticalScrollBar()->value() : 0;
    const int savedScrollX = horizontalScrollBar() ? horizontalScrollBar()->value() : 0;

    // preserves COLLAPSED items (Halla default: everything expanded -- so users joining
    // after the first rebuild are never invisible)
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

    // The tab already shows the server name in the header. The tree uses the QTreeWidget
    // invisible root so the server is not repeated as a third visual item before the
    // channels.
    QTreeWidgetItem* root = invisibleRootItem();
    QSet<int> path;
    QSet<int> built;
    for (int cid : m_data->childChannels(0)) {
        if (m_data->channels.contains(cid))
            buildChannelItem(m_data->channels[cid], root, path, built);
    }
    // Old/corrupted data may contain a missing parent or even a cycle. Do not recurse
    // forever: show every channel not reached yet once at the root, visually breaking
    // the cycle without crashing the client.
    QList<int> remaining = m_data->channels.keys();
    std::sort(remaining.begin(), remaining.end(), [this](int a, int b) {
        const Channel& ca = m_data->channels[a];
        const Channel& cb = m_data->channels[b];
        if (ca.order != cb.order) return ca.order < cb.order;
        return ca.name.localeAwareCompare(cb.name) < 0;
    });
    for (int cid : remaining) {
        if (!built.contains(cid))
            buildChannelItem(m_data->channels[cid], root, path, built);
    }

    blockSignals(false);

    // restore the selection without creating a fictitious server node
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

    // re-expand everything except what the user explicitly collapsed
    std::function<void(QTreeWidgetItem*)> expand = [&](QTreeWidgetItem* it) {
        const int key = (it->data(0, RoleKind).toInt() << 24) | it->data(0, RoleId).toInt();
        it->setExpanded(!collapsed.contains(key));
        for (int i = 0; i < it->childCount(); ++i) expand(it->child(i));
    };
    for (int i = 0; i < topLevelItemCount(); ++i) expand(topLevelItem(i));

    // Returns the scroll position saved at the beginning (see the comment at the top):
    // it must be the LAST operation to beat clear()'s reset and the implicit scrollTo
    // of setCurrentItem above.
    if (verticalScrollBar()) verticalScrollBar()->setValue(savedScrollY);
    if (horizontalScrollBar()) horizontalScrollBar()->setValue(savedScrollX);
}

QTreeWidgetItem* ServerTreeWidget::buildChannelItem(const Channel& c,
                                                     QTreeWidgetItem* parentItem,
                                                     QSet<int>& path,
                                                     QSet<int>& built) {
    if (!parentItem || c.id <= 0 || path.contains(c.id) || built.contains(c.id))
        return nullptr;
    path.insert(c.id);
    built.insert(c.id);
    QTreeWidgetItem* item = new QTreeWidgetItem(parentItem);

    bool full = (c.maxClients >= 0 && c.users.size() >= c.maxClients);
    
    // ---- "Spacer" channel logic (visual server organization)
    bool isSpacer = false;
    bool isCentered = false;
    bool isRepeating = false;
    QString spacerText = c.name;

    // v1.1.35: static regex -- the rebuild runs on every state update; compiling the
    // expression per channel, per rebuild, was waste.
    static const QRegularExpression spacerRx(QStringLiteral("\\[(\\*?)(c)?spacer[^\\]]*\\](.*)"));
    QRegularExpressionMatch match = spacerRx.match(c.name);
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
        item->setIcon(0, QIcon()); // No icon for spacers
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
            // No traverse in any ancestor also blocks entry.
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
        if (c.type == 0) { // temporary: name in blue-gray, as in Halla
            item->setForeground(0, QColor("#5C7285"));
        }
    }

    // Orders users only by roles whose visual order is enabled. The hierarchy
    // (position) of the primary role is the main criterion -- the same tag shown
    // before the name -- with the visual order as a tiebreaker and the nickname last.
    // Old servers without a position sort as before (all at 0: visual order, then
    // alphabetical).
    QList<int> sortedUsers;
    for (int uid : c.users) {
        if (m_data->users.contains(uid)) {
            sortedUsers << uid;
        }
    }
    std::sort(sortedUsers.begin(), sortedUsers.end(), [&](int uidA, int uidB) {
        const User& uA = m_data->users[uidA];
        const User& uB = m_data->users[uidB];
        if (uA.groupOrderEnabled != uB.groupOrderEnabled)
            return uA.groupOrderEnabled; // quem tem uma ordem ativa vem primeiro
        if (uA.groupOrderEnabled) {
            // Visual hierarchy (siglaPosition) is sent by the server already filtered:
            // roles with "Use this role's order in the name list" turned off do not count
            // -- neither the order nor the hierarchy. The PERMISSION position never
            // enters here at all, or an admin with the order disabled would go back to
            // the top of the list.
            if (uA.groupSiglaPosition != uB.groupSiglaPosition)
                return uA.groupSiglaPosition > uB.groupSiglaPosition;
            if (uA.groupOrder != uB.groupOrder)
                return uA.groupOrder < uB.groupOrder;
        }
        return uA.name.localeAwareCompare(uB.name) < 0;
    });

    // Halla: by default clients appear above subchannels; with the "Sort clients below
    // the channels" option the subchannels come first.
    if (!m_sortClientsBelow) {
        for (int uid : sortedUsers)
            addUserItem(item, m_data->users[uid]);
        for (int cid : m_data->childChannels(c.id))
            if (m_data->channels.contains(cid))
                buildChannelItem(m_data->channels[cid], item, path, built);
    } else {
        for (int cid : m_data->childChannels(c.id))
            if (m_data->channels.contains(cid))
                buildChannelItem(m_data->channels[cid], item, path, built);
        for (int uid : sortedUsers)
            addUserItem(item, m_data->users[uid]);
    }

    if (!isSpacer) {
        item->setChildIndicatorPolicy(item->childCount() > 0
            ? QTreeWidgetItem::ShowIndicator
            : QTreeWidgetItem::DontShowIndicatorWhenChildless);
    }
    path.remove(c.id);
    return item;
}

// ------------------------------------------------------------------ expansion
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
    
    QString displayName = u.name;
    if (!u.sigla.isEmpty())
        displayName = u.sigla + QStringLiteral(" ") + displayName;
    if (!u.siglaSuffix.isEmpty())
        displayName += QStringLiteral(" ") + u.siglaSuffix;
    item->setText(0, displayName);
    item->setIcon(0, leadingUserIcon(u, m_delegate ? m_delegate->showMinis() : true));
    item->setData(0, RoleKind, NodeUser);
    item->setData(0, RoleId, u.id);
    // v1.1.35: visual key of the row -- updateUserVisuals compares this key with the
    // current state to repaint ONLY what changed (speech ring, mutes, away) without
    // rebuilding the tree.
    item->setData(0, RoleVisualKey,
                  userVisualKey(u, m_delegate ? m_delegate->showMinis() : true));
    item->setToolTip(0, userTooltip(u));
    const bool draggable = (u.id == m_data->selfId) || m_canMoveOthers;
    item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable |
                   (draggable ? Qt::ItemIsDragEnabled : Qt::NoItemFlags));
    if (u.away) item->setForeground(0, QColor("#6E7B86"));
}

// v1.1.35 -- lightweight tree update. Walks the EXISTING items and repaints
// (icon + color) only the users whose visual key changed. This is the path of
// "speaking" transitions: before, each transition (coalesced to 120 ms) destroyed
// and recreated the whole tree -- items, tooltips, icons, expansion and scroll --
// which in full rooms became tens of thousands of allocations per second on the GUI
// thread. The audio pipeline (capture and playback every 10 ms) shares that SAME
// thread: the rebuild burst delayed the ticks and the voice chopped ("after a while
// the sound freezes"). Now: speech transition = integer comparison + setIcon on the
// affected rows. Structure (joins/leaves/moves/renames) still goes through the full
// rebuild(), decided by signature in ServerTab.
void ServerTreeWidget::updateUserVisuals() {
    if (!m_data) return;
    const bool showMinis = m_delegate ? m_delegate->showMinis() : true;
    std::function<void(QTreeWidgetItem*)> walk = [&](QTreeWidgetItem* it) {
        if (!it) return;
        if (it->data(0, RoleKind).toInt() == NodeUser) {
            const int uid = it->data(0, RoleId).toInt();
            const auto found = m_data->users.constFind(uid);
            if (found != m_data->users.constEnd()) {
                const quint32 key = userVisualKey(found.value(), showMinis);
                if (it->data(0, RoleVisualKey).toUInt() != key) {
                    it->setIcon(0, leadingUserIcon(found.value(), showMinis));
                    it->setData(0, RoleVisualKey, key);
                    // away also desaturates the text (same rule as the rebuild)
                    if (found.value().away) it->setForeground(0, QColor("#6E7B86"));
                    else it->setForeground(0, QBrush()); // empty brush = default color
                }
            }
        }
        for (int i = 0; i < it->childCount(); ++i) walk(it->child(i));
    };
    for (int i = 0; i < topLevelItemCount(); ++i) walk(topLevelItem(i));
    // Live badges and role icons are painted by the delegate straight from m_data: a
    // general repaint ensures non-key changes (e.g. someone started a live stream)
    // show up even without a visual key change.
    viewport()->update();
}

// ------------------------------------------------------------------ menus de contexto

// (v6 E2EE) Manual verification by SAS code was REMOVED from the UI: trust is now
// automatic (TOFU) in NetSession::e2eeSecurityCheckUser -- each person's first valid
// key is already marked trusted, without a dialog or button. The identity CHANGE
// warning still exists.

void ServerTreeWidget::contextMenuEvent(QContextMenuEvent* e) {
    QTreeWidgetItem* it = itemAt(e->pos());
    // Saves the selection before calling setCurrentItem(): in some Qt versions that
    // call clears the extended selection even when the clicked item was already
    // selected.
    const QList<int> selectedBefore = selectedChannelIds();
    const bool clickedWasSelected = it && it->isSelected();
    if (it) {
        // A right click on an already selected item preserves the extended selection.
        // On an item outside it, a new selection starts.
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

    // The actions are available regardless of whether the right click landed on a
    // channel, on a user or on the server root. What matters is the multiple
    // selection preserved above.
    if (selectedChannels.size() >= 2) {
        const QList<int> ids = selectedChannels;
        menu.addAction(tr("Vincular canais selecionados"), this,
                       [this, ids] { emit channelLinkRequested(ids, true); });
        menu.addAction(tr("Desvincular canais selecionados"), this,
                       [this, ids] { emit channelLinkRequested(ids, false); });
        menu.addSeparator();
    }

    if (!it || kind == NodeServer) {
        // server menu -- like clicking the server tab in Halla
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
        const QString selfUid = m_data->users.value(m_data->selfId).uniqueId;
        const bool localOperator = !selfUid.isEmpty() && c.opUids.contains(selfUid);
        const bool temporaryOwner = c.type == 0 && c.temporaryOwnerUid == selfUid;
        QAction* edit = menu.addAction(HIcons::editPencil(), tr("Editar canal"), this,
                       [this, id] { emit editChannelRequested(id); });
        edit->setEnabled(m_canEditChannels || temporaryOwner
                         || (c.type != 0 && localOperator));
        QAction* remove = menu.addAction(HIcons::trash(), tr("Excluir canal"), this,
                       [this, id] { emit deleteChannelRequested(id); });
        remove->setEnabled(m_canDeleteChannels);
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
            // Renaming a third party: the server requires moderation permission (move)
            // and hierarchy; the option only appears for whoever can use it.
            if (m_canRenameOthers) {
                menu.addAction(tr("Alterar apelido"), this,
                               [this, id] { emit renameUserRequested(id); });
            }
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

void ServerTreeWidget::startDrag(Qt::DropActions actions) {
    // Qt starts dragging an item with ANY pressed button. With the right button, on
    // Windows the OLE loop of QDrag::exec() captures the mouse and consumes
    // WM_RBUTTONUP -- WM_CONTEXTMENU is never generated and the context menu does not
    // open (the item was only selected, "the lists did not open"). Dragging a
    // client/channel is now left-button only; the right button always delivers the
    // context menu.
    if (QGuiApplication::mouseButtons() & Qt::LeftButton) {
        QTreeWidget::startDrag(actions);
        return;
    }
    setState(QAbstractItemView::NoState);
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

bool ServerTreeWidget::wouldCreateChannelCycle(int channelId, int parentId) const {
    if (!m_data || channelId <= 0) return true;
    QSet<int> visited;
    for (int current = parentId; current != 0; ) {
        if (current == channelId || visited.contains(current)) return true;
        visited.insert(current);
        if (!m_data->channels.contains(current)) return true;
        current = m_data->channels.value(current).parentId;
    }
    return false;
}

bool ServerTreeWidget::isDuplicateChannelMove(int channelId, int parentId, int order) {
    const bool duplicate = m_lastChannelMoveClock.isValid()
        && m_lastChannelMoveClock.elapsed() < 750
        && m_lastMovedChannel == channelId
        && m_lastMoveParent == parentId
        && m_lastMoveOrder == order;
    if (!duplicate) {
        m_lastMovedChannel = channelId;
        m_lastMoveParent = parentId;
        m_lastMoveOrder = order;
        m_lastChannelMoveClock.restart();
    }
    return duplicate;
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

        const Channel source = m_data->channels.value(channelId);
        int parentId = 0;
        int order = m_data->childChannels(0).size();

        if (target && target->data(0, RoleKind).toInt() == NodeChannel) {
            const int targetId = target->data(0, RoleId).toInt();
            if (!m_data->channels.contains(targetId)) {
                event->ignore();
                return;
            }
            if (indicator == QAbstractItemView::OnItem) {
                parentId = targetId;
                QList<int> children = m_data->childChannels(parentId);
                children.removeAll(channelId);
                order = children.size();
            } else {
                parentId = m_data->channels.value(targetId).parentId;
                QList<int> siblings = m_data->childChannels(parentId);
                siblings.removeAll(channelId);
                order = siblings.indexOf(targetId);
                if (order < 0) {
                    event->ignore();
                    return;
                }
                if (indicator == QAbstractItemView::BelowItem) ++order;
            }
        } else if (target && target->data(0, RoleKind).toInt() == NodeServer) {
            parentId = 0;
            QList<int> roots = m_data->childChannels(0);
            roots.removeAll(channelId);
            order = roots.size();
        } else if (target) {
            // Does not turn a drag over a user into an accidental structural move.
            event->ignore();
            return;
        } else {
            QList<int> roots = m_data->childChannels(0);
            roots.removeAll(channelId);
            order = roots.size();
        }

        if (wouldCreateChannelCycle(channelId, parentId)) {
            event->ignore();
            return;
        }
        QList<int> desired = m_data->childChannels(parentId);
        desired.removeAll(channelId);
        order = qBound(0, order, desired.size());
        desired.insert(order, channelId);
        if (source.parentId == parentId
                && desired == m_data->childChannels(source.parentId)) {
            event->ignore(); // dropped at the current position
            return;
        }
        if (isDuplicateChannelMove(channelId, parentId, order)) {
            event->ignore();
            return;
        }

        emit channelMoveRequested(channelId, parentId, order);
        event->setDropAction(Qt::MoveAction);
        event->accept();
        return;
    }

    if (data->hasFormat(QStringLiteral("application/x-halla-userid"))) {
        const int userId = data->data(QStringLiteral("application/x-halla-userid")).toInt();
        if (userId != m_data->selfId && !m_canMoveOthers) {
            event->ignore();
            return;
        }

        // Only a real channel is a valid target. Dragging to the empty area of the tree
        // is rejected, preventing the user from ending up "nowhere".
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

void ServerTreeWidget::mouseMoveEvent(QMouseEvent* e) {
    QTreeWidget::mouseMoveEvent(e);
    if (!m_data) return;
    QTreeWidgetItem* item = itemAt(e->pos());
    bool overStreamingUser = false;
    if (item && item->data(0, RoleKind).toInt() == NodeUser) {
        const int id = item->data(0, RoleId).toInt();
        overStreamingUser = m_data->users.contains(id) && m_data->users[id].screensharing;
    }
    if (!overStreamingUser) emit screenshareHoverLeft();
}

void ServerTreeWidget::leaveEvent(QEvent* e) {
    QTreeWidget::leaveEvent(e);
    emit screenshareHoverLeft();
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
