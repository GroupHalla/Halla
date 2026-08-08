#include "ScreenShareDialog.h"
#include <QListWidgetItem>
#include <QScreen>
#include <QGuiApplication>

#ifdef Q_OS_WIN
#include <windows.h>

struct WindowInfo {
    HWND hwnd;
    QString title;
};

BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lParam) {
    if (!IsWindowVisible(hwnd)) return TRUE;
    
    int length = GetWindowTextLength(hwnd);
    if (length == 0) return TRUE;
    
    TCHAR title[512];
    GetWindowText(hwnd, title, 512);
    QString wTitle = QString::fromWCharArray(title);
    
    if (wTitle == QStringLiteral("Program Manager") || wTitle == QStringLiteral("Start")) return TRUE;
    
    QList<WindowInfo>* list = reinterpret_cast<QList<WindowInfo>*>(lParam);
    list->append({hwnd, wTitle});
    return TRUE;
}
#endif

ScreenShareDialog::ScreenShareDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("Compartilhar Tela"));
    setFixedSize(500, 420);
    setStyleSheet(QStringLiteral(
        "QDialog { background-color: #151322; color: #FFFFFF; }"
        "QLabel { color: #FFFFFF; font-size: 13px; font-weight: bold; }"
        "QTabWidget::pane { border: 1px solid #2B2A3A; border-radius: 8px; background: #0D0E15; padding: 10px; }"
        "QTabBar::tab { background: #1C1B2B; color: #8A939B; padding: 10px 20px; font-weight: bold; border-top-left-radius: 6px; border-top-right-radius: 6px; margin-right: 2px; }"
        "QTabBar::tab:selected { background: #8B5CF6; color: #FFFFFF; }"
        "QListWidget { background-color: #0D0E15; border: none; color: #FFFFFF; }"
        "QListWidget::item { padding: 12px; border-radius: 6px; margin-bottom: 4px; background-color: #1C1B2B; color: #FFFFFF; }"
        "QListWidget::item:hover { background-color: #2E2A3A; }"
        "QListWidget::item:selected { background-color: #8B5CF6; color: #FFFFFF; }"
        "QPushButton { background-color: #8B5CF6; border: none; border-radius: 6px; color: #FFFFFF; font-weight: bold; padding: 10px 20px; }"
        "QPushButton:hover { background-color: #A78BFA; }"
        "QPushButton#cancel { background-color: #2E2A3A; }"
        "QPushButton#cancel:hover { background-color: #3E3A4A; }"
    ));

    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(15, 15, 15, 15);
    mainLayout->setSpacing(15);

    QLabel* title = new QLabel(tr("Compartilhe o que você está vendo:"), this);
    mainLayout->addWidget(title);

    m_tabs = new QTabWidget(this);
    
    m_screenList = new QListWidget(m_tabs);
    m_tabs->addTab(m_screenList, tr("💻 Telas"));
    
    m_windowList = new QListWidget(m_tabs);
    m_tabs->addTab(m_windowList, tr("🗔 Janelas"));

    mainLayout->addWidget(m_tabs);

    populateScreens();
    populateWindows();

    QHBoxLayout* btnRow = new QHBoxLayout;
    btnRow->setSpacing(10);
    
    QPushButton* cancelBtn = new QPushButton(tr("Cancelar"), this);
    cancelBtn->setObjectName(QStringLiteral("cancel"));
    connect(cancelBtn, &QPushButton::clicked, this, &QDialog::reject);
    btnRow->addWidget(cancelBtn);

    QPushButton* shareBtn = new QPushButton(tr("Compartilhar"), this);
    connect(shareBtn, &QPushButton::clicked, this, [this]() {
        if (m_tabs->currentIndex() == 0) {
            m_selectedSourceType = 0;
            if (m_screenList->currentItem()) {
                m_selectedSourceId = m_screenList->currentItem()->data(Qt::UserRole).toULongLong();
            }
        } else {
            m_selectedSourceType = 1;
            if (m_windowList->currentItem()) {
                m_selectedSourceId = m_windowList->currentItem()->data(Qt::UserRole).toULongLong();
            }
        }
        accept();
    });
    btnRow->addWidget(shareBtn);

    mainLayout->addLayout(btnRow);
}

void ScreenShareDialog::populateScreens() {
    m_screenList->clear();
    QList<QScreen*> screens = QGuiApplication::screens();
    for (int i = 0; i < screens.size(); ++i) {
        QScreen* s = screens[i];
        QListWidgetItem* item = new QListWidgetItem(m_screenList);
        item->setText(tr("Tela %1 (%2x%3)").arg(QString::number(i + 1)).arg(s->geometry().width()).arg(s->geometry().height()));
        item->setData(Qt::UserRole, i);
        m_screenList->addItem(item);
    }
    if (m_screenList->count() > 0) m_screenList->setCurrentRow(0);
}

void ScreenShareDialog::populateWindows() {
    m_windowList->clear();
#ifdef Q_OS_WIN
    QList<WindowInfo> windows;
    EnumWindows(EnumWindowsProc, reinterpret_cast<LPARAM>(&windows));
    for (const WindowInfo& win : windows) {
        QListWidgetItem* item = new QListWidgetItem(m_windowList);
        item->setText(win.title);
        item->setData(Qt::UserRole, qulonglong(win.hwnd));
        m_windowList->addItem(item);
    }
#else
    QListWidgetItem* item = new QListWidgetItem(tr("Sem aplicativos detectados nesta plataforma"), m_windowList);
    item->setData(Qt::UserRole, 0);
    m_windowList->addItem(item);
#endif
    if (m_windowList->count() > 0) m_windowList->setCurrentRow(0);
}
