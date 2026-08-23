#include "TalkingOverlay.h"

#include <QCursor>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QJsonArray>
#include <QPainter>
#include <QScreen>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

TalkingOverlay::TalkingOverlay(QWidget* parent) : QWidget(parent) {
    setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint
                   | Qt::WindowDoesNotAcceptFocus | Qt::WindowTransparentForInput);
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_ShowWithoutActivating);
    setAttribute(Qt::WA_StyledBackground, false);

    m_foregroundTimer.setInterval(500);
    connect(&m_foregroundTimer, &QTimer::timeout, this, [this] {
        updateVisibility();
        if (isVisible()) updatePosition();
    });
    m_foregroundTimer.start();
}

void TalkingOverlay::applySettings(const QJsonObject& settings) {
    m_settings = settings;
    setWindowOpacity(qBound(0.25, settings.value("opacity").toInt(88) / 100.0, 1.0));
    rebuildRows();
}

void TalkingOverlay::updateClientState(const QJsonObject& payload) {
    m_state = payload;
    rebuildRows();
}

void TalkingOverlay::rebuildRows() {
    m_rows.clear();
    const bool onlyTalking = m_settings.value("onlyTalking").toBool(true);
    const bool showSelf = m_settings.value("showSelf").toBool(false);
    const int maxRows = qBound(1, m_settings.value("maxUsers").toInt(8), 24);

    const QJsonArray users = m_state.value("users").toArray();
    for (const QJsonValue& value : users) {
        const QJsonObject user = value.toObject();
        Row row;
        row.name = user.value("name").toString();
        row.talking = user.value("talking").toBool();
        row.whispering = user.value("whispering").toBool();
        row.muted = user.value("muted").toBool();
        row.self = user.value("self").toBool();
        if (row.name.isEmpty() || (row.self && !showSelf)) continue;
        if (onlyTalking && !row.talking) continue;
        m_rows << row;
        if (m_rows.size() >= maxRows) break;
    }

    const int scale = qBound(75, m_settings.value("scale").toInt(100), 160);
    QFont f = font();
    f.setPointSizeF(10.0 * scale / 100.0);
    f.setBold(true);
    setFont(f);
    QFontMetrics metrics(f);
    int textWidth = metrics.horizontalAdvance(m_state.value("channelName").toString());
    for (const Row& row : m_rows)
        textWidth = qMax(textWidth, metrics.horizontalAdvance(row.name));
    const int rowHeight = qMax(24, metrics.height() + 10);
    const int headerHeight = m_settings.value("showChannel").toBool(true) ? rowHeight : 0;
    resize(qBound(180, textWidth + 58, 430),
           qMax(1, 16 + headerHeight + rowHeight * m_rows.size()));
    updatePosition();
    updateVisibility();
    update();
}

bool TalkingOverlay::foregroundLooksLikeGame() const {
    if (!m_settings.value("gameOnly").toBool(true)) return true;
#ifdef Q_OS_WIN
    HWND hwnd = GetForegroundWindow();
    if (!hwnd || !IsWindowVisible(hwnd) || IsIconic(hwnd)) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == GetCurrentProcessId()) return false;
    RECT rect{};
    if (!GetWindowRect(hwnd, &rect)) return false;
    const qint64 width = qMax<LONG>(0, rect.right - rect.left);
    const qint64 height = qMax<LONG>(0, rect.bottom - rect.top);
    QScreen* screen = QGuiApplication::screenAt(
        QPoint((rect.left + rect.right) / 2, (rect.top + rect.bottom) / 2));
    if (!screen) screen = QGuiApplication::primaryScreen();
    if (!screen) return false;
    const QRect available = screen->geometry();
    const qint64 screenArea = qint64(available.width()) * available.height();
    return screenArea > 0 && width * height >= screenArea * 45 / 100;
#else
    return true;
#endif
}

void TalkingOverlay::updateVisibility() {
    const bool connected = m_state.value("connected").toBool(false);
    const bool shouldShow = connected && !m_rows.isEmpty() && foregroundLooksLikeGame();
    if (shouldShow) {
        if (!isVisible()) show();
#ifdef Q_OS_WIN
        if (HWND hwnd = reinterpret_cast<HWND>(winId())) {
            LONG_PTR style = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
            style |= WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW;
            SetWindowLongPtr(hwnd, GWL_EXSTYLE, style);
            SetWindowPos(hwnd, HWND_TOPMOST, x(), y(), width(), height(),
                         SWP_NOACTIVATE | SWP_SHOWWINDOW);
        }
#endif
    } else if (isVisible()) {
        hide();
    }
}

void TalkingOverlay::updatePosition() {
    QScreen* screen = QGuiApplication::screenAt(QCursor::pos());
    if (!screen) screen = QGuiApplication::primaryScreen();
    if (!screen) return;
    const QRect area = screen->availableGeometry();
    const int margin = qBound(0, m_settings.value("margin").toInt(24), 200);
    const QString position = m_settings.value("position").toString("top_right");
    int px = area.right() - width() - margin + 1;
    int py = area.top() + margin;
    if (position.contains("left")) px = area.left() + margin;
    if (position.startsWith("bottom")) py = area.bottom() - height() - margin + 1;
    move(px, py);
}

void TalkingOverlay::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(18, 20, 29, 218));
    painter.drawRoundedRect(rect().adjusted(1, 1, -1, -1), 10, 10);

    const QFontMetrics metrics(font());
    const int rowHeight = qMax(24, metrics.height() + 10);
    int y = 8;
    if (m_settings.value("showChannel").toBool(true)) {
        QFont headerFont = font();
        headerFont.setBold(false);
        headerFont.setPointSizeF(qMax(8.0, font().pointSizeF() - 1.0));
        painter.setFont(headerFont);
        painter.setPen(QColor(184, 180, 214));
        painter.drawText(QRect(16, y, width() - 30, rowHeight),
                         Qt::AlignVCenter | Qt::AlignLeft,
                         m_state.value("channelName").toString());
        y += rowHeight;
        painter.setPen(QColor(255, 255, 255, 30));
        painter.drawLine(12, y - 2, width() - 12, y - 2);
    }

    painter.setFont(font());
    for (const Row& row : m_rows) {
        const QColor dot = row.whispering ? QColor(255, 166, 62)
                                          : (row.talking ? QColor(76, 219, 126)
                                                         : QColor(111, 117, 137));
        painter.setPen(Qt::NoPen);
        painter.setBrush(dot);
        painter.drawEllipse(QPointF(22, y + rowHeight / 2.0), 5.5, 5.5);
        painter.setPen(row.muted ? QColor(156, 158, 171) : QColor(248, 248, 252));
        painter.drawText(QRect(38, y, width() - 50, rowHeight),
                         Qt::AlignVCenter | Qt::AlignLeft, row.name);
        y += rowHeight;
    }
}
