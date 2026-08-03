#include "Icons.h"

#include <QPainter>
#include <QPainterPath>
#include <QLinearGradient>
#include <QFont>
#include <cmath>

namespace HIcons {

static QPixmap mk(int w, int h, const std::function<void(QPainter&)>& fn) {
    QPixmap pm(w, h);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::TextAntialiasing, true);
    fn(p);
    return pm;
}

static QPixmap mk(int s, const std::function<void(QPainter&)>& fn) { return mk(s, s, fn); }

// ---------------------------------------------------------------- marca d'água / logo
QPixmap waveMark(int size, const QColor& color) {
    return mk(size, [&](QPainter& p) {
        p.setPen(Qt::NoPen);
        p.setBrush(color);
        const qreal u = size / 24.0;
        const qreal cx = size / 2.0;
        static const qreal hb[5] = { 6, 11, 17, 11, 6 }; // barras de EQ (ondas de voz)
        for (int i = 0; i < 5; ++i) {
            qreal h = hb[i] * u;
            qreal w = 2.6 * u;
            qreal x = cx + (i - 2) * 4.0 * u - w / 2.0;
            p.drawRoundedRect(QRectF(x, size / 2.0 - h / 2.0, w, h), w / 2.0, w / 2.0);
        }
    });
}

QPixmap appIcon(int size) {
    return mk(size, [&](QPainter& p) {
        QLinearGradient g(0, 0, 0, size);
        g.setColorAt(0, QColor("#3789CE"));
        g.setColorAt(1, QColor("#15518F"));
        p.setPen(Qt::NoPen);
        p.setBrush(g);
        p.drawRoundedRect(QRectF(0.5, 0.5, size - 1.0, size - 1.0), size * 0.22, size * 0.22);
        // brilho superior
        QLinearGradient shine(0, 0, 0, size / 2);
        shine.setColorAt(0, QColor(255, 255, 255, 70));
        shine.setColorAt(1, QColor(255, 255, 255, 0));
        p.setBrush(shine);
        p.drawRoundedRect(QRectF(2, 2, size - 4, size / 2.2), size * 0.18, size * 0.18);
        p.drawPixmap(0, 0, waveMark(size, QColor(255, 255, 255)));
    });
}

QPixmap banner(int w, int h) {
    return mk(w, h, [&](QPainter& p) {
        QLinearGradient g(0, 0, w, h);
        g.setColorAt(0, QColor("#14304E"));
        g.setColorAt(0.55, QColor("#1E4470"));
        g.setColorAt(1, QColor("#2A628F"));
        p.setPen(Qt::NoPen);
        p.setBrush(g);
        p.drawRect(0, 0, w, h);
        // marca d'água grande à direita
        QPixmap wm = waveMark(h * 2, QColor(255, 255, 255, 26));
        p.drawPixmap(w - wm.width() + h / 2, (h - wm.height()) / 2, wm);
        // texto (título + subtítulo empilhados)
        QFont f = p.font();
        const int pad = h / 4;
        QPixmap logo = appIcon(h - pad * 2);
        p.drawPixmap(pad, pad, logo);
        const int tx = pad * 2 + logo.width();

        f.setPixelSize(h * 0.36);
        f.setBold(true);
        p.setFont(f);
        p.setPen(Qt::white);
        p.drawText(tx, 2, w - tx, h / 2 + 2, Qt::AlignVCenter | Qt::AlignLeft,
                   QStringLiteral("Halla"));

        f.setPixelSize(h * 0.17);
        f.setBold(false);
        p.setFont(f);
        p.setPen(QColor(255, 255, 255, 190));
        p.drawText(tx, h / 2 - 2, w - tx, h / 2, Qt::AlignVCenter | Qt::AlignLeft,
                   QStringLiteral("Cliente de comunicação de voz"));
    });
}

// ---------------------------------------------------------------- plug (conectar/desconectar)
static void drawPlug(QPainter& p, const QColor& c1, const QColor& c2) {
    QLinearGradient g(0, 3, 0, 21);
    g.setColorAt(0, c1.lighter(115));
    g.setColorAt(1, c2);
    p.setPen(QPen(c2.darker(130), 0.8));
    // corpo do plug
    p.setBrush(g);
    p.drawRoundedRect(QRectF(4, 4, 16, 12), 3, 3);
    // pinos
    p.drawRect(QRectF(7, 1, 3, 4));
    p.drawRect(QRectF(14, 1, 3, 4));
    // detalhe do corpo
    p.setPen(QPen(c2.darker(150), 0.8));
    p.drawLine(8, 4, 8, 16);
    p.drawLine(16, 4, 16, 16);
    // fio
    p.setPen(QPen(c2.darker(140), 1.6));
    p.setBrush(Qt::NoBrush);
    p.drawArc(QRectF(6, 14, 12, 10), 200 * 16, 140 * 16);
}

QIcon connectPlug() {
    return QIcon(mk(24, [&](QPainter& p) { drawPlug(p, QColor("#59C46D"), QColor("#2E7D3B")); }));
}

QIcon disconnectPlug() {
    return QIcon(mk(24, [&](QPainter& p) {
        drawPlug(p, QColor("#B9C2CB"), QColor("#6E7B86"));
        p.setPen(QPen(red(), 2.4, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(3, 21), QPointF(21, 3));
    }));
}

// ---------------------------------------------------------------- estrela (favoritos)
static void starPath(QPainterPath& path, const QPointF& c, qreal rOut, qreal rIn) {
    for (int i = 0; i < 10; ++i) {
        qreal ang = -M_PI / 2 + i * M_PI / 5;
        qreal r = (i % 2 == 0) ? rOut : rIn;
        QPointF pt(c.x() + r * std::cos(ang), c.y() + r * std::sin(ang));
        if (i == 0) path.moveTo(pt); else path.lineTo(pt);
    }
    path.closeSubpath();
}

QIcon bookmarkStar() {
    return QIcon(mk(24, [&](QPainter& p) {
        QPainterPath path;
        starPath(path, QPointF(12, 12.5), 10, 4.6);
        QLinearGradient g(0, 2, 0, 22);
        g.setColorAt(0, QColor("#FFD964"));
        g.setColorAt(1, QColor("#D9A017"));
        p.setPen(QPen(QColor("#8F6600"), 0.9));
        p.setBrush(g);
        p.drawPath(path);
    }));
}

// ---------------------------------------------------------------- engrenagem
QIcon optionsGear() {
    return QIcon(mk(24, [&](QPainter& p) {
        p.setPen(QPen(QColor("#5B6B7A"), 3.6, Qt::SolidLine, Qt::RoundCap));
        for (int i = 0; i < 8; ++i) {
            qreal a = i * M_PI / 4;
            p.drawLine(QPointF(12 + 5.4 * std::cos(a), 12 + 5.4 * std::sin(a)),
                       QPointF(12 + 8.8 * std::cos(a), 12 + 8.8 * std::sin(a)));
        }
        QLinearGradient g(0, 5, 0, 19);
        g.setColorAt(0, QColor("#9FB0BF"));
        g.setColorAt(1, QColor("#5B6B7A"));
        p.setBrush(g);
        p.setPen(QPen(QColor("#46545F"), 1));
        p.drawEllipse(QPointF(12, 12), 6.4, 6.4);
        p.setBrush(QColor("#E8EEF4"));
        p.drawEllipse(QPointF(12, 12), 2.4, 2.4);
    }));
}

// ---------------------------------------------------------------- página de log
QIcon logPage() {
    return QIcon(mk(24, [&](QPainter& p) {
        p.setPen(QPen(QColor("#4E5B68"), 1));
        p.setBrush(QColor("#F4F8FC"));
        p.drawRoundedRect(QRectF(5, 2.5, 14, 19), 1.5, 1.5);
        p.setPen(QPen(blue(), 1.6, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(8, 7),  QPointF(16, 7));
        p.drawLine(QPointF(8, 11), QPointF(16, 11));
        p.drawLine(QPointF(8, 15), QPointF(13, 15));
    }));
}

// ---------------------------------------------------------------- ausente
QIcon away(bool on) {
    return QIcon(mk(24, [&](QPainter& p) {
        QLinearGradient g(0, 3, 0, 21);
        g.setColorAt(0, on ? QColor("#7FB2E0") : QColor("#C6D3E0"));
        g.setColorAt(1, on ? QColor("#3B76B0") : QColor("#8FA3B5"));
        p.setPen(QPen(QColor("#2F5877"), 1));
        p.setBrush(g);
        p.drawEllipse(QRectF(3, 3, 18, 18));
        QFont f = p.font();
        f.setPixelSize(13);
        f.setBold(true);
        f.setItalic(true);
        p.setFont(f);
        p.setPen(Qt::white);
        p.drawText(QRectF(3, 2, 18, 18), Qt::AlignCenter, QStringLiteral("z"));
    }));
}

// ---------------------------------------------------------------- microfone
static void drawMicro(QPainter& p, const QColor& body) {
    QLinearGradient g(0, 2, 0, 22);
    g.setColorAt(0, body.lighter(125));
    g.setColorAt(1, body);
    p.setPen(QPen(body.darker(140), 1));
    p.setBrush(g);
    p.drawRoundedRect(QRectF(9, 2, 6, 11), 3, 3);   // cápsula
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(body.darker(120), 1.6, Qt::SolidLine, Qt::RoundCap));
    p.drawArc(QRectF(6, 7, 12, 9), 200 * 16, 140 * 16); // suporte
    p.drawLine(QPointF(12, 16), QPointF(12, 20));   // haste
    p.drawLine(QPointF(8, 20),  QPointF(16, 20));   // base
}

QIcon muteMic(bool muted) {
    return QIcon(mk(24, [&](QPainter& p) {
        drawMicro(p, muted ? QColor("#8A939B") : QColor("#4E7BA6"));
        if (muted) {
            p.setPen(QPen(red(), 2.4, Qt::SolidLine, Qt::RoundCap));
            p.drawLine(QPointF(4, 20), QPointF(20, 4));
        }
    }));
}

QIcon muteSpeaker(bool muted) {
    return QIcon(mk(24, [&](QPainter& p) {
        QColor body = muted ? QColor("#8A939B") : QColor("#4E7BA6");
        QLinearGradient g(0, 4, 0, 20);
        g.setColorAt(0, body.lighter(125));
        g.setColorAt(1, body);
        p.setPen(QPen(body.darker(140), 1));
        p.setBrush(g);
        // cone do alto-falante
        QPolygonF cone;
        cone << QPointF(4, 9) << QPointF(8, 9) << QPointF(13, 4)
             << QPointF(13, 20) << QPointF(8, 15) << QPointF(4, 15);
        p.drawPolygon(cone);
        if (!muted) {
            p.setPen(QPen(body.darker(130), 1.7, Qt::SolidLine, Qt::RoundCap));
            p.setBrush(Qt::NoBrush);
            p.drawArc(QRectF(13, 8, 8, 8), -45 * 16, 90 * 16);
            p.drawArc(QRectF(13, 5, 12, 14), -40 * 16, 80 * 16);
        } else {
            p.setPen(QPen(red(), 2.4, Qt::SolidLine, Qt::RoundCap));
            p.drawLine(QPointF(15, 19), QPointF(22, 5));
        }
    }));
}

QIcon bell() {
    return QIcon(mk(24, [&](QPainter& p) {
        QLinearGradient g(0, 3, 0, 19);
        g.setColorAt(0, QColor("#FFD964"));
        g.setColorAt(1, QColor("#D9A017"));
        p.setPen(QPen(QColor("#8F6600"), 1));
        p.setBrush(g);
        QPainterPath path;
        path.moveTo(12, 3.5);
        path.cubicTo(6.5, 3.5, 6.5, 9, 6.5, 12);
        path.lineTo(6.5, 15);
        path.lineTo(4.5, 17.5);
        path.lineTo(19.5, 17.5);
        path.lineTo(17.5, 15);
        path.lineTo(17.5, 12);
        path.cubicTo(17.5, 9, 17.5, 3.5, 12, 3.5);
        p.drawPath(path);
        p.drawArc(QRectF(9.8, 18.5, 4.4, 3.6), 0, -180 * 16);
    }));
}

// ---------------------------------------------------------------- servidor
QIcon server() {
    return QIcon(mk(24, [&](QPainter& p) {
        for (int i = 0; i < 3; ++i) {
            QLinearGradient g(0, 3 + i * 6, 0, 9 + i * 6);
            g.setColorAt(0, QColor("#9FC4E4"));
            g.setColorAt(1, QColor(i == 0 ? "#3B76B0" : "#2E6394"));
            p.setPen(QPen(QColor("#1E415F"), 0.9));
            p.setBrush(g);
            p.drawRoundedRect(QRectF(4, 3 + i * 6.2, 16, 5.4), 2, 2);
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(i == 0 ? "#7BE08D" : "#CDE5F8"));
            p.drawEllipse(QRectF(6, 4.6 + i * 6.2, 2.2, 2.2));
        }
    }));
}

// ---------------------------------------------------------------- casa (canal padrão)
static void drawHouse(QPainter& p, const QRectF& r) {
    QLinearGradient g(0, r.top(), 0, r.bottom());
    g.setColorAt(0, QColor("#6FA8DC"));
    g.setColorAt(1, QColor("#2E6394"));
    p.setPen(QPen(QColor("#1E415F"), 0.9));
    p.setBrush(g);
    QPolygonF roof;
    roof << QPointF(r.center().x(), r.top())
         << QPointF(r.right(), r.top() + r.height() * 0.45)
         << QPointF(r.left(), r.top() + r.height() * 0.45);
    p.drawPolygon(roof);
    p.drawRect(QRectF(r.left() + r.width() * 0.14, r.top() + r.height() * 0.42,
                      r.width() * 0.72, r.height() * 0.58));
    p.setBrush(QColor("#EAF3FB"));
    p.setPen(Qt::NoPen);
    p.drawRect(QRectF(r.center().x() - r.width() * 0.11, r.top() + r.height() * 0.58,
                      r.width() * 0.22, r.height() * 0.42));
}

static void drawChannelDisc(QPainter& p, const QRectF& r) {
    QLinearGradient g(0, r.top(), 0, r.bottom());
    g.setColorAt(0, QColor("#7FB2E0"));
    g.setColorAt(1, QColor("#2E6394"));
    p.setPen(QPen(QColor("#1E415F"), 0.9));
    p.setBrush(g);
    p.drawEllipse(r);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor("#EAF3FB"));
    p.drawEllipse(r.adjusted(r.width() * 0.18, r.height() * 0.34,
                             -r.width() * 0.18, -r.height() * 0.30));
}

static void drawPadlock(QPainter& p, const QRectF& r) {
    p.setPen(QPen(QColor("#8F6600"), 1.1));
    p.setBrush(QColor("#FFD964"));
    p.drawRoundedRect(r.adjusted(r.width() * 0.08, r.height() * 0.38,
                                 -r.width() * 0.08, 0), 1.5, 1.5);
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(QColor("#8F6600"), 1.4, Qt::SolidLine, Qt::RoundCap));
    p.drawArc(QRectF(r.left() + r.width() * 0.22, r.top(),
                     r.width() * 0.56, r.height() * 0.62), 0, 180 * 16);
}

QIcon channel(bool hasPassword, bool moderated, bool isDefault, bool full) {
    return QIcon(mk(24, [&](QPainter& p) {
        QRectF r(3, 3, 18, 18);
        if (isDefault) drawHouse(p, r);
        else           drawChannelDisc(p, r);

        if (full) {
            p.setPen(QPen(QColor("#7F1D1D"), 0.8));
            p.setBrush(red());
            p.drawEllipse(QRectF(14, 14, 9, 9));
            p.setPen(QPen(Qt::white, 1.6, Qt::SolidLine, Qt::RoundCap));
            p.drawLine(QPointF(16, 18.5), QPointF(21, 18.5));
        } else if (moderated) {
            p.setPen(QPen(QColor("#7F1D1D"), 0.8));
            p.setBrush(red());
            p.drawEllipse(QRectF(14, 14, 9, 9));
            p.setPen(QPen(Qt::white, 1.6, Qt::SolidLine, Qt::RoundCap));
            p.drawLine(QPointF(18.5, 15.8), QPointF(18.5, 19.2));
            p.drawPoint(QPointF(18.5, 20.6));
        }
        if (hasPassword) drawPadlock(p, QRectF(13.5, 13.5, 9.5, 9.5));
    }));
}

// ---------------------------------------------------------------- usuário
QIcon user(bool talking, bool away, int size, bool whispering) {
    return QIcon(mk(size, [&](QPainter& p) {
        const qreal u = size / 24.0;
        QColor top = away ? QColor("#C9CFD6") : QColor("#9FC4E4");
        QColor bot = away ? QColor("#8A939B") : QColor("#3B76B0");
        if (talking) {
            QColor circleColor = whispering ? orange() : green();
            p.setPen(QPen(circleColor, 2.4 * u, Qt::SolidLine, Qt::RoundCap));
            p.setBrush(Qt::NoBrush);
            p.drawEllipse(QRectF(2.2 * u, 2.2 * u, 19.6 * u, 19.6 * u));
        }
        QLinearGradient g(0, 4 * u, 0, 22 * u);
        g.setColorAt(0, top);
        g.setColorAt(1, bot);
        p.setPen(QPen(away ? QColor("#6E7B86") : QColor("#1E415F"), 0.9 * u));
        p.setBrush(g);
        p.drawEllipse(QRectF(8.2 * u, 3.6 * u, 7.6 * u, 7.6 * u));            // cabeça
        p.drawRoundedRect(QRectF(5 * u, 12.4 * u, 14 * u, 8.6 * u), 4 * u, 4 * u); // corpo
        if (away) {
            QFont f = p.font();
            f.setPixelSize(9 * u);
            f.setBold(true);
            f.setItalic(true);
            p.setFont(f);
            p.setPen(QColor("#34506B"));
            p.drawText(QRectF(13 * u, 12 * u, 10 * u, 10 * u), Qt::AlignCenter,
                       QStringLiteral("z"));
        }
    }));
}

static QPixmap miniMicSlash() {
    return mk(14, [&](QPainter& p) {
        p.setPen(QPen(QColor("#7F1D1D"), 1));
        p.setBrush(red());
        p.drawRoundedRect(QRectF(5.4, 1.4, 3.2, 5.6), 1.6, 1.6);
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(QColor("#B03A36"), 1.1, Qt::SolidLine, Qt::RoundCap));
        p.drawArc(QRectF(3.6, 4.4, 6.8, 5.2), 200 * 16, 140 * 16);
        p.drawLine(QPointF(7, 9.6), QPointF(7, 11.4));
        p.drawLine(QPointF(4.6, 11.4), QPointF(9.4, 11.4));
        p.setPen(QPen(QColor("#7F1D1D"), 1.7, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(2, 12.6), QPointF(12, 1.6));
    });
}

static QPixmap miniHeadphoneSlash() {
    return mk(14, [&](QPainter& p) {
        p.setPen(QPen(QColor("#54616E"), 1.5, Qt::SolidLine, Qt::RoundCap));
        p.setBrush(Qt::NoBrush);
        p.drawArc(QRectF(2.6, 2.2, 8.8, 9.4), 0, 180 * 16);
        p.setPen(QPen(QColor("#3E4A56"), 0.8));
        p.setBrush(QColor("#84919E"));
        p.drawRoundedRect(QRectF(1.6, 6.6, 2.6, 4), 1, 1);
        p.drawRoundedRect(QRectF(9.8, 6.6, 2.6, 4), 1, 1);
        p.setPen(QPen(red(), 1.7, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(2, 12.6), QPointF(12, 1.6));
    });
}

static QPixmap miniRec() {
    return mk(14, [&](QPainter& p) {
        p.setPen(QPen(QColor(255, 90, 80, 160), 2.4));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(QRectF(2, 2, 10, 10));
        p.setPen(QPen(QColor("#7F1D1D"), 0.8));
        p.setBrush(QColor("#E33224"));
        p.drawEllipse(QRectF(3.4, 3.4, 7.2, 7.2));
    });
}

static QPixmap miniStar() {
    return mk(14, [&](QPainter& p) {
        QPainterPath path;
        starPath(path, QPointF(7, 7.4), 6.2, 2.8);
        p.setPen(QPen(QColor("#8F6600"), 0.8));
        p.setBrush(gold());
        p.drawPath(path);
    });
}

// v3: escudo do operador de canal
static QPixmap miniOp() {
    return mk(14, [&](QPainter& p) {
        QPainterPath path;
        path.moveTo(7, 1.2);
        path.lineTo(12.2, 3.2);
        path.lineTo(12.2, 7.2);
        path.quadTo(12.2, 11.2, 7, 13.2);
        path.quadTo(1.8, 11.2, 1.8, 7.2);
        path.lineTo(1.8, 3.2);
        path.closeSubpath();
        p.setPen(QPen(QColor("#1E5E2E"), 0.8));
        p.setBrush(QColor("#3FA85C"));
        p.drawPath(path);
        p.setPen(QPen(Qt::white, 1.4));
        p.drawLine(QPointF(4.2, 7.0), QPointF(6.2, 9.2));
        p.drawLine(QPointF(6.2, 9.2), QPointF(10.0, 4.6));
    });
}

static QPixmap miniAway() {
    return mk(14, [&](QPainter& p) {
        p.setPen(QPen(QColor("#2F5877"), 0.8));
        p.setBrush(QColor("#7FB2E0"));
        p.drawEllipse(QRectF(1.4, 1.4, 11.2, 11.2));
        QFont f = p.font();
        f.setPixelSize(8.5);
        f.setBold(true);
        f.setItalic(true);
        p.setFont(f);
        p.setPen(Qt::white);
        p.drawText(QRectF(1.4, 1.0, 11.2, 11.2), Qt::AlignCenter, QStringLiteral("z"));
    });
}

QPixmap userStatusMinis(bool inputMuted, bool outputMuted, bool away,
                        bool recording, bool commander, bool op) {
    QList<QPixmap> minis;
    if (inputMuted)  minis << miniMicSlash();
    if (outputMuted) minis << miniHeadphoneSlash();
    if (away)        minis << miniAway();
    if (recording)   minis << miniRec();
    if (commander)   minis << miniStar();
    if (op)          minis << miniOp();
    if (minis.isEmpty()) return QPixmap();

    QPixmap pm(minis.size() * 16, 14);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    for (int i = 0; i < minis.size(); ++i)
        p.drawPixmap(i * 16, 0, minis[i]);
    return pm;
}

// ---------------------------------------------------------------- ícones simples de menu
QIcon identity() {
    return QIcon(mk(16, [&](QPainter& p) {
        p.setPen(QPen(QColor("#1E415F"), 0.8));
        p.setBrush(QColor("#EAF3FB"));
        p.drawRoundedRect(QRectF(1.5, 2.5, 13, 11), 1.5, 1.5);
        p.setBrush(blue());
        p.setPen(Qt::NoPen);
        p.drawEllipse(QRectF(3, 4.4, 3, 3));
        p.drawRoundedRect(QRectF(2.6, 8, 3.8, 4), 1.4, 1.4);
        p.setPen(QPen(blueDark(), 1.1, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(8, 5.4), QPointF(13, 5.4));
        p.drawLine(QPointF(8, 8),   QPointF(13, 8));
        p.drawLine(QPointF(8, 10.6), QPointF(11, 10.6));
    }));
}

QIcon contacts() {
    return QIcon(mk(16, [&](QPainter& p) {
        p.setPen(QPen(QColor("#5B4310"), 0.8));
        p.setBrush(QColor("#F4E2B0"));
        p.drawRoundedRect(QRectF(2, 1.8, 12, 12.6), 1.4, 1.4);
        p.setPen(QPen(QColor("#8F6600"), 1));
        p.drawLine(QPointF(8, 2.2), QPointF(8, 14));
        p.setPen(QPen(QColor("#8F6600"), 0.8, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(3.4, 5),  QPointF(6.6, 5));
        p.drawLine(QPointF(3.4, 7.4), QPointF(6.6, 7.4));
        p.drawLine(QPointF(9.4, 5),  QPointF(12.6, 5));
    }));
}

QIcon transfer() {
    return QIcon(mk(16, [&](QPainter& p) {
        p.setPen(Qt::NoPen);
        p.setBrush(green());
        QPolygonF up;
        up << QPointF(8, 1.5) << QPointF(12.5, 6) << QPointF(9.8, 6)
           << QPointF(9.8, 10.5) << QPointF(6.2, 10.5) << QPointF(6.2, 6)
           << QPointF(3.5, 6);
        p.drawPolygon(up);
        p.setBrush(blue());
        QPolygonF down;
        down << QPointF(8, 14.8) << QPointF(3.5, 10.3) << QPointF(6.2, 10.3)
             << QPointF(6.2, 11.9) << QPointF(9.8, 11.9) << QPointF(9.8, 10.3)
             << QPointF(12.5, 10.3);
        p.drawPolygon(down);
    }));
}

QIcon key() {
    return QIcon(mk(16, [&](QPainter& p) {
        p.setPen(QPen(QColor("#8F6600"), 1));
        p.setBrush(QColor("#FFD964"));
        p.drawEllipse(QRectF(1.5, 1.5, 6.4, 6.4));
        p.setPen(QPen(QColor("#C9970D"), 2.2, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(7.6, 6.8), QPointF(13.6, 12.8));
        p.drawLine(QPointF(11.2, 10.4), QPointF(13.2, 8.6));
        p.drawLine(QPointF(12.8, 12), QPointF(14.6, 10.4));
    }));
}

QIcon groups() {
    return QIcon(mk(16, [&](QPainter& p) {
        auto person = [&](qreal x, qreal y, qreal s, const QColor& c) {
            p.setPen(QPen(c.darker(150), 0.7));
            p.setBrush(c);
            p.drawEllipse(QRectF(x + 1.6 * s, y, 3.2 * s, 3.2 * s));
            p.drawRoundedRect(QRectF(x, y + 3.6 * s, 6.4 * s, 3.6 * s), 1.6 * s, 1.6 * s);
        };
        person(1, 4.2, 1.15, QColor("#9FC4E4"));
        person(8.4, 4.2, 1.15, QColor("#9FC4E4"));
        person(4.7, 1.4, 1.15, blue());
    }));
}

QIcon captureMic() {
    return QIcon(mk(16, [&](QPainter& p) {
        p.save();
        p.scale(16.0 / 24.0, 16.0 / 24.0);
        drawMicro(p, blue());
        p.restore();
    }));
}

QIcon playbackSpeaker() {
    return QIcon(muteSpeaker(false).pixmap(24, 24).scaled(16, 16, Qt::KeepAspectRatio,
                                                          Qt::SmoothTransformation));
}

QIcon hotkeys() {
    return QIcon(mk(16, [&](QPainter& p) {
        p.setPen(QPen(QColor("#46545F"), 0.9));
        p.setBrush(QColor("#E8EEF4"));
        p.drawRoundedRect(QRectF(0.8, 4.2, 14.4, 8.6), 1.6, 1.6);
        p.setBrush(QColor("#B9C6D2"));
        for (int r = 0; r < 2; ++r)
            for (int c = 0; c < 5; ++c)
                p.drawRect(QRectF(2.2 + c * 2.5, 5.6 + r * 3, 1.9, 2.2));
    }));
}

QIcon design() {
    return QIcon(mk(16, [&](QPainter& p) {
        p.setPen(QPen(QColor("#46545F"), 0.8));
        p.setBrush(QColor("#F4F8FC"));
        p.drawEllipse(QRectF(1.4, 1.4, 12, 12));
        p.setBrush(red());
        p.drawEllipse(QRectF(4, 4, 3, 3));
        p.setBrush(green());
        p.drawEllipse(QRectF(9, 4, 3, 3));
        p.setBrush(blue());
        p.drawEllipse(QRectF(9, 9, 3.2, 3.2));
        p.setBrush(gold());
        p.drawEllipse(QRectF(4.2, 9, 3, 3));
    }));
}

QIcon notifyBell() {
    return QIcon(bell().pixmap(24, 24).scaled(16, 16, Qt::KeepAspectRatio,
                                              Qt::SmoothTransformation));
}

QIcon security() {
    return QIcon(mk(16, [&](QPainter& p) {
        QLinearGradient g(0, 1.5, 0, 14.5);
        g.setColorAt(0, QColor("#9FD6A6"));
        g.setColorAt(1, QColor("#3E9450"));
        QPainterPath path;
        path.moveTo(8, 1.5);
        path.lineTo(14, 4);
        path.cubicTo(14, 10, 11.5, 13.2, 8, 14.8);
        path.cubicTo(4.5, 13.2, 2, 10, 2, 4);
        path.closeSubpath();
        p.setPen(QPen(QColor("#2E6B3C"), 0.8));
        p.setBrush(g);
        p.drawPath(path);
        p.setPen(QPen(Qt::white, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.drawPolyline(QPolygonF() << QPointF(5, 7.6) << QPointF(7.2, 9.8) << QPointF(11.2, 5.4));
    }));
}

QIcon addons() {
    return QIcon(mk(16, [&](QPainter& p) {
        p.setPen(QPen(QColor("#7A528F"), 0.8));
        p.setBrush(QColor("#C9A7DE"));
        QPainterPath path;
        path.moveTo(2, 2);
        path.lineTo(7, 2);
        path.lineTo(7, 5);
        path.cubicTo(7, 3.6, 10, 3.6, 10, 6);
        path.cubicTo(10, 8.4, 7, 7.4, 7, 8.6);
        path.lineTo(7, 14);
        path.lineTo(2, 14);
        path.closeSubpath();
        p.drawPath(path);
        p.setBrush(QColor("#9FC4E4"));
        QPainterPath path2;
        path2.moveTo(9, 9);
        path2.lineTo(14, 9);
        path2.lineTo(14, 14);
        path2.lineTo(11, 14);
        path2.cubicTo(12.4, 14, 12.4, 11, 10, 11);
        path2.cubicTo(9, 11, 9, 10, 9, 9);
        p.drawPath(path2);
    }));
}

QIcon application() {
    return QIcon(mk(16, [&](QPainter& p) {
        p.setPen(QPen(QColor("#1E415F"), 0.8));
        p.setBrush(QColor("#EAF3FB"));
        p.drawRect(QRectF(1.5, 2.5, 13, 11));
        p.setBrush(blue());
        p.setPen(Qt::NoPen);
        p.drawRect(QRectF(2.2, 3.2, 11.6, 2.6));
        p.setBrush(QColor("#9FC4E4"));
        p.drawRect(QRectF(3, 7, 4.4, 5));
        p.drawRect(QRectF(8.6, 7, 4.4, 5));
    }));
}

QIcon info() {
    return QIcon(mk(16, [&](QPainter& p) {
        p.setPen(QPen(QColor("#1E415F"), 0.8));
        p.setBrush(QColor("#9FC4E4"));
        p.drawEllipse(QRectF(1.4, 1.4, 13.2, 13.2));
        p.setPen(QPen(Qt::white, 1.7, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(8, 7.2), QPointF(8, 11.4));
        p.setBrush(Qt::white);
        p.setPen(Qt::NoPen);
        p.drawEllipse(QRectF(7.2, 4.2, 1.7, 1.7));
    }));
}

QIcon fileNew() {
    return QIcon(mk(16, [&](QPainter& p) {
        p.setPen(QPen(QColor("#4E5B68"), 0.8));
        p.setBrush(Qt::white);
        p.drawRect(QRectF(3.2, 1.6, 9.6, 12.8));
        p.setPen(QPen(green().darker(120), 1.6, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(8, 5), QPointF(8, 11));
        p.drawLine(QPointF(5, 8), QPointF(11, 8));
    }));
}

QIcon editPencil() {
    return QIcon(mk(16, [&](QPainter& p) {
        p.setPen(QPen(QColor("#8F6600"), 1));
        p.setBrush(QColor("#FFD964"));
        p.drawPolygon(QPolygonF() << QPointF(3, 13) << QPointF(10.6, 5.4)
                                  << QPointF(12.6, 7.4) << QPointF(5, 15));
        p.setPen(QPen(QColor("#46545F"), 1.4, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(2.6, 13.4), QPointF(3.4, 12.8));
    }));
}

QIcon trash() {
    return QIcon(mk(16, [&](QPainter& p) {
        p.setPen(QPen(QColor("#7F1D1D"), 0.9));
        p.setBrush(QColor("#E33224"));
        p.drawRect(QRectF(3, 4.6, 10, 9.4));
        p.setBrush(QColor("#B03A36"));
        p.drawRect(QRectF(2, 2.6, 12, 2));
        p.drawRect(QRectF(6.2, 1, 3.4, 1.4));
        p.setPen(QPen(QColor("#FFD2CE"), 1, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(6, 6.4), QPointF(6, 12));
        p.drawLine(QPointF(10, 6.4), QPointF(10, 12));
    }));
}

QIcon check() {
    return QIcon(mk(16, [&](QPainter& p) {
        p.setPen(QPen(green().darker(130), 2.2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.drawPolyline(QPolygonF() << QPointF(2.5, 8.5) << QPointF(6.5, 12.4)
                                   << QPointF(13.5, 3.5));
    }));
}

QIcon record(bool on) {
    return QIcon(mk(16, [&](QPainter& p) {
        p.setPen(QPen(on ? QColor("#8f1d1d") : QColor("#666666"), 1.2));
        p.setBrush(on ? QColor("#d63b3b") : QColor("#9a9a9a"));
        p.drawEllipse(QRectF(2.5, 2.5, 11, 11));
    }));
}

} // namespace HIcons
