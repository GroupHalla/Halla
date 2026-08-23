#include "AboutDialog.h"
#include "TsBanner.h"
#include "Icons.h"
#include "version.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QDate>
#include <QtGlobal>

AboutDialog::AboutDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("Sobre o Halla"));
    setMinimumWidth(420);
    setSizeGripEnabled(false);

    QVBoxLayout* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 10);
    root->setSpacing(0);
    root->addWidget(new TsBanner(QStringLiteral("Halla"),
                                 tr("Cliente de comunicação de voz"),
                                 HIcons::appIcon(30), this));
    root->addSpacing(12);

    QHBoxLayout* mid = new QHBoxLayout;
    mid->setContentsMargins(16, 0, 16, 0);
    QLabel* logo = new QLabel(this);
    logo->setPixmap(HIcons::appIcon(72));
    mid->addWidget(logo, 0, Qt::AlignTop);

    QLabel* info = new QLabel(this);
    info->setTextFormat(Qt::RichText);
    info->setText(QStringLiteral(
        "<b style='font-size:16px'>Halla</b> <span style='color:#666666'>v%1 (%2)</span><br><br>"
        "%3 %4<br>"
        "Qt %5<br><br>"
        "<span style='color:#888888'>© 2026 Halla — %6</span>")
        .arg(QString::fromUtf8(halla::kAppVersion))
        .arg(QString::fromUtf8(halla::kAppBuild))
        .arg(tr("Compilado em:"))
        .arg(QDate::currentDate().toString("dd/MM/yyyy"))
        .arg(QString::fromLatin1(qVersion()))
        .arg(tr("Todos os direitos reservados.")));
    info->setWordWrap(true);
    mid->addWidget(info, 1);
    root->addLayout(mid);

    root->addSpacing(12);
    QHBoxLayout* brow = new QHBoxLayout;
    brow->setContentsMargins(16, 0, 16, 0);
    brow->addStretch(1);
    QPushButton* ok = new QPushButton(tr("OK"), this);
    brow->addWidget(ok);
    root->addLayout(brow);
    connect(ok, &QPushButton::clicked, this, &QDialog::accept);
}
