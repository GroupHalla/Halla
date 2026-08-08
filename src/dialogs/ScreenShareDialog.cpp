#include "ScreenShareDialog.h"
#include "Theme.h"
#include <QListWidgetItem>
#include <QScreen>
#include <QGuiApplication>
#include <QListView>

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
    setFixedSize(560, 460);
    
    bool dark = HTheme::isDark();
    
    QString qdialogBg = dark ? QStringLiteral("#151322") : QStringLiteral("#F4F5F7");
    QString labelColor = dark ? QStringLiteral("#FFFFFF") : QStringLiteral("#1F2937");
    QString tabPaneBg = dark ? QStringLiteral("#0D0E15") : QStringLiteral("#FFFFFF");
    QString tabBorder = dark ? QStringLiteral("#2B2A3A") : QStringLiteral("#D1D5DB");
    QString tabBgSelected = dark ? QStringLiteral("#8B5CF6") : QStringLiteral("#3B82F6");
    QString tabBgUnselected = dark ? QStringLiteral("#1C1B2B") : QStringLiteral("#E5E7EB");
    QString tabTextColor = dark ? QStringLiteral("#FFFFFF") : QStringLiteral("#FFFFFF");
    QString tabUnselectedTextColor = dark ? QStringLiteral("#8A939B") : QStringLiteral("#4B5563");
    
    QString itemBg = dark ? QStringLiteral("#1C1B2B") : QStringLiteral("#E5E7EB");
    QString itemHover = dark ? QStringLiteral("#2E2A3A") : QStringLiteral("#D1D5DB");
    QString itemTextColor = dark ? QStringLiteral("#FFFFFF") : QStringLiteral("#1F2937");
    
    QString btnCancelBg = dark ? QStringLiteral("#2E2A3A") : QStringLiteral("#D1D5DB");
    QString btnCancelHover = dark ? QStringLiteral("#3E3A4A") : QStringLiteral("#9CA3AF");
    QString btnCancelTextColor = dark ? QStringLiteral("#FFFFFF") : QStringLiteral("#1F2937");
    
    setStyleSheet(QString(
        "QDialog { background-color: %1; color: %2; }"
        "QLabel { color: %2; font-size: 13px; font-weight: bold; }"
        "QTabWidget::pane { border: 1px solid %3; border-radius: 8px; background: %4; padding: 10px; }"
        "QTabBar::tab { background: %5; color: %6; padding: 10px 20px; font-weight: bold; border-top-left-radius: 6px; border-top-right-radius: 6px; margin-right: 4px; }"
        "QTabBar::tab:selected { background: %7; color: %8; }"
        "QListWidget { background-color: %4; border: none; color: %9; }"
        "QListWidget::item { padding: 6px; border-radius: 6px; background-color: %10; color: %9; font-weight: bold; font-size: 10px; }"
        "QListWidget::item:hover { background-color: %11; }"
        "QListWidget::item:selected { background-color: %7; color: #FFFFFF; }"
        "QPushButton { background-color: %7; border: none; border-radius: 6px; color: #FFFFFF; font-weight: bold; padding: 10px 20px; }"
        "QPushButton:hover { background-color: %12; }"
        "QPushButton#cancel { background-color: %13; color: %14; }"
        "QPushButton#cancel:hover { background-color: %15; }"
    ).arg(
        qdialogBg, labelColor, tabBorder, tabPaneBg,
        tabBgUnselected, tabUnselectedTextColor, tabBgSelected, tabTextColor,
        itemTextColor, itemBg, itemHover,
        dark ? QStringLiteral("#A78BFA") : QStringLiteral("#60A5FA"),
        btnCancelBg, btnCancelTextColor, btnCancelHover
    ));

    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(15, 15, 15, 15);
    mainLayout->setSpacing(15);

    QLabel* title = new QLabel(tr("Compartilhe o que você está vendo:"), this);
    mainLayout->addWidget(title);

    m_tabs = new QTabWidget(this);
    
    m_screenList = new QListWidget(m_tabs);
    m_screenList->setViewMode(QListView::IconMode);
    m_screenList->setResizeMode(QListView::Adjust);
    m_screenList->setGridSize(QSize(155, 125));
    m_screenList->setMovement(QListView::Static);
    m_screenList->setSpacing(8);
    m_screenList->setWordWrap(true);
    m_tabs->addTab(m_screenList, tr("💻 Telas"));
    
    m_windowList = new QListWidget(m_tabs);
    m_windowList->setViewMode(QListView::IconMode);
    m_windowList->setResizeMode(QListView::Adjust);
    m_windowList->setGridSize(QSize(155, 125));
    m_windowList->setMovement(QListView::Static);
    m_windowList->setSpacing(8);
    m_windowList->setWordWrap(true);
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
        item->setTextAlignment(Qt::AlignCenter);
        item->setData(Qt::UserRole, i);
        
        QPixmap preview = s->grabWindow(0).scaled(135, 80, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        if (!preview.isNull()) {
            item->setIcon(QIcon(preview));
        }
        
        m_screenList->addItem(item);
    }
    if (m_screenList->count() > 0) m_screenList->setCurrentRow(0);
}

void ScreenShareDialog::populateWindows() {
    m_windowList->clear();
#ifdef Q_OS_WIN
    QList<WindowInfo> windows;
    EnumWindows(EnumWindowsProc, reinterpret_cast<LPARAM>(&windows));
    QScreen* screen = QGuiApplication::primaryScreen();
    for (const WindowInfo& win : windows) {
        QListWidgetItem* item = new QListWidgetItem(m_windowList);
        
        QString t = win.title;
        if (t.length() > 28) {
            t = t.left(25) + QStringLiteral("...");
        }
        item->setText(t);
        item->setTextAlignment(Qt::AlignCenter);
        item->setData(Qt::UserRole, qulonglong(win.hwnd));
        
        if (screen && win.hwnd) {
            QPixmap preview = screen->grabWindow(WId(win.hwnd)).scaled(135, 80, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            if (!preview.isNull()) {
                item->setIcon(QIcon(preview));
            }
        }
        
        m_windowList->addItem(item);
    }
#else
    QListWidgetItem* item = new QListWidgetItem(tr("Sem aplicativos detectados nesta plataforma"), m_windowList);
    item->setData(Qt::UserRole, 0);
    m_windowList->addItem(item);
#endif
    if (m_windowList->count() > 0) m_windowList->setCurrentRow(0);
}
