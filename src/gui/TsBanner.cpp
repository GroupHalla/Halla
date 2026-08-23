#include "TsBanner.h"
#include "Icons.h"
#include "app/Theme.h"

#include <QPainter>
#include <QPainterPath>
#include <QLinearGradient>

TsBanner::TsBanner(const QString& title, const QString& subtitle,
                   const QPixmap& icon, QWidget* parent)
    : QWidget(parent), m_title(title), m_subtitle(subtitle), m_icon(icon) {
    setMinimumHeight(68);
    setMaximumHeight(68);
    setObjectName(QStringLiteral("dialogBanner"));
}

void TsBanner::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    const bool dark = HTheme::isDark();
    QPainterPath clip;
    clip.addRoundedRect(rect().adjusted(0, 0, 0, 1), 13, 13);
    p.setClipPath(clip);

    QLinearGradient g(0, 0, width(), height());
    if (dark) {
        g.setColorAt(0, QColor("#25104F"));
        g.setColorAt(0.55, QColor("#4B1C9B"));
        g.setColorAt(1, QColor("#7828E8"));
    } else {
        g.setColorAt(0, QColor("#FFFFFF"));
        g.setColorAt(1, QColor("#EFE7FF"));
    }
    p.fillRect(rect(), g);
    p.setClipping(false);

    QPixmap icon = m_icon.isNull() ? HIcons::appIcon(42) : m_icon;
    int x = 18;
    if (!icon.isNull()) {
        p.drawPixmap(x, (height() - 42) / 2,
                     icon.scaled(42, 42, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        x += 54;
    }

    QFont f = font();
    f.setPixelSize(18);
    f.setBold(true);
    p.setFont(f);
    p.setPen(dark ? Qt::white : QColor("#29223F"));
    p.drawText(x, 8, width() - x - 20, m_subtitle.isEmpty() ? height() - 16 : 34,
               Qt::AlignVCenter | Qt::AlignLeft, m_title);

    if (!m_subtitle.isEmpty()) {
        f.setPixelSize(11);
        f.setBold(false);
        p.setFont(f);
        p.setPen(dark ? QColor(255, 255, 255, 205) : QColor("#69617D"));
        p.drawText(x, 38, width() - x - 20, 22,
                   Qt::AlignVCenter | Qt::AlignLeft, m_subtitle);
    }

    QPixmap wm = HIcons::waveMark(qMax(90, height() * 2),
                                  dark ? QColor(255, 255, 255, 34) : QColor(124, 58, 237, 25));
    p.drawPixmap(width() - wm.width() + height() / 2,
                 (height() - wm.height()) / 2, wm);
}
