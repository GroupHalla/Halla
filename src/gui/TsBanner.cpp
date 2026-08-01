#include "TsBanner.h"
#include "Icons.h"

#include <QPainter>
#include <QPainterPath>
#include <QLinearGradient>

TsBanner::TsBanner(const QString& title, const QString& subtitle,
                   const QPixmap& icon, QWidget* parent)
    : QWidget(parent), m_title(title), m_subtitle(subtitle), m_icon(icon) {
    setMinimumHeight(56);
    setMaximumHeight(56);
}

void TsBanner::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    QPainterPath clip;
    clip.addRoundedRect(rect().adjusted(0, 0, 0, 10), 9, 9); // só os cantos superiores visíveis
    p.setClipPath(clip);

    QLinearGradient g(0, 0, width(), height());
    g.setColorAt(0, HIcons::blueDark());
    g.setColorAt(0.6, HIcons::navyMid());
    g.setColorAt(1, QColor("#2A628F"));
    p.fillRect(rect(), g);
    p.setClipping(false);

    int x = 14;
    if (!m_icon.isNull()) {
        p.drawPixmap(14, (height() - 30) / 2, m_icon.scaled(30, 30, Qt::KeepAspectRatio,
                                                            Qt::SmoothTransformation));
        x = 14 + 30 + 10;
    }

    QFont f = font();
    f.setPixelSize(19);
    f.setBold(true);
    p.setFont(f);
    p.setPen(Qt::white);
    int titleH = m_subtitle.isEmpty() ? height() : 34;
    p.drawText(x, 0, width() - x - 10, titleH, Qt::AlignVCenter | Qt::AlignLeft, m_title);

    if (!m_subtitle.isEmpty()) {
        f.setPixelSize(11);
        f.setBold(false);
        p.setFont(f);
        p.setPen(QColor(255, 255, 255, 190));
        p.drawText(x, 30, width() - x - 10, height() - 30, Qt::AlignVCenter | Qt::AlignLeft,
                   m_subtitle);
    }
}
