#include "WelcomePage.h"
#include "Icons.h"

#include <QPainter>
#include <QVBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QStyle>

WelcomePage::WelcomePage(QWidget* parent) : QWidget(parent) {
    // fundo/cores vêm do tema global (HTheme) via objectName
    setObjectName(QStringLiteral("welcomePage"));
    setAutoFillBackground(true);

    QVBoxLayout* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->addStretch(3);

    QLabel* logo = new QLabel(this);
    logo->setPixmap(HIcons::appIcon(96));
    logo->setAlignment(Qt::AlignHCenter);
    lay->addWidget(logo);

    QLabel* title = new QLabel(QStringLiteral("Halla"), this);
    title->setObjectName(QStringLiteral("welcomeTitle"));
    QFont f = title->font();
    f.setPixelSize(30);
    f.setBold(true);
    title->setFont(f);
    title->setAlignment(Qt::AlignHCenter);
    lay->addWidget(title);

    QLabel* hint = new QLabel(tr("Não conectado — use Conexões ▸ Conectar… para entrar em um servidor"), this);
    f.setPixelSize(13);
    f.setBold(false);
    hint->setFont(f);
    hint->setObjectName(QStringLiteral("welcomeHint"));
    hint->setAlignment(Qt::AlignHCenter);
    lay->addWidget(hint);

    lay->addSpacing(24);

    QHBoxLayout* row = new QHBoxLayout;
    row->addStretch(1);
    QPushButton* connectBtn = new QPushButton(tr("Conectar a um servidor"), this);
    QPushButton* bookmarksBtn = new QPushButton(tr("Gerenciar favoritos"), this);
    connectBtn->setDefault(true);
    row->addWidget(connectBtn);
    row->addWidget(bookmarksBtn);
    row->addStretch(1);
    lay->addLayout(row);

    lay->addStretch(4);

    connect(connectBtn, &QPushButton::clicked, this, &WelcomePage::connectRequested);
    connect(bookmarksBtn, &QPushButton::clicked, this, &WelcomePage::bookmarksRequested);
}

void WelcomePage::paintEvent(QPaintEvent* e) {
    QWidget::paintEvent(e);
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    // marca d'água sutil no canto inferior direito
    QPixmap wm = HIcons::waveMark(220, QColor(90, 107, 122, 14));
    p.drawPixmap(width() - wm.width() - 30, height() - wm.height() - 20, wm);
}
