#include "ScreenShareDialog.h"
#include <QListWidgetItem>

ScreenShareDialog::ScreenShareDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("Compartilhar Tela"));
    setFixedSize(400, 300);
    setStyleSheet(QStringLiteral(
        "QDialog { background-color: #151322; color: #FFFFFF; }"
        "QLabel { color: #FFFFFF; font-size: 13px; font-weight: bold; }"
        "QListWidget { background-color: #0D0E15; border: 1px solid #2B2A3A; border-radius: 8px; color: #FFFFFF; padding: 5px; }"
        "QListWidget::item { padding: 10px; border-radius: 5px; }"
        "QListWidget::item:selected { background-color: #8B5CF6; color: #FFFFFF; }"
        "QPushButton { background-color: #8B5CF6; border: none; border-radius: 6px; color: #FFFFFF; font-weight: bold; padding: 8px 16px; }"
        "QPushButton:hover { background-color: #A78BFA; }"
        "QPushButton#cancel { background-color: #2E2A3A; }"
        "QPushButton#cancel:hover { background-color: #3E3A4A; }"
    ));

    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(15, 15, 15, 15);
    mainLayout->setSpacing(12);

    QLabel* title = new QLabel(tr("Escolha o que compartilhar:"), this);
    mainLayout->addWidget(title);

    m_list = new QListWidget(this);
    
    QListWidgetItem* itemScreen = new QListWidgetItem(tr("💻 Tela Inteira (Monitor Principal)"), m_list);
    itemScreen->setData(Qt::UserRole, 0);
    m_list->addItem(itemScreen);

    QListWidgetItem* itemWindow = new QListWidgetItem(tr("🗔 Janela do Aplicativo (Halla Client)"), m_list);
    itemWindow->setData(Qt::UserRole, 1);
    m_list->addItem(itemWindow);

    m_list->setCurrentRow(0);
    mainLayout->addWidget(m_list);

    QHBoxLayout* btnRow = new QHBoxLayout;
    btnRow->setSpacing(10);
    
    QPushButton* cancelBtn = new QPushButton(tr("Cancelar"), this);
    cancelBtn->setObjectName(QStringLiteral("cancel"));
    connect(cancelBtn, &QPushButton::clicked, this, &QDialog::reject);
    btnRow->addWidget(cancelBtn);

    QPushButton* shareBtn = new QPushButton(tr("Compartilhar"), this);
    connect(shareBtn, &QPushButton::clicked, this, [this]() {
        if (m_list->currentItem()) {
            m_selectedSource = m_list->currentItem()->data(Qt::UserRole).toInt();
        }
        accept();
    });
    btnRow->addWidget(shareBtn);

    mainLayout->addLayout(btnRow);
}
