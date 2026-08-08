#include "ScreenShareDialog.h"
#include "Theme.h"
#include <QListWidgetItem>
#include <QScreen>
#include <QGuiApplication>
#include <QListView>
#include <QFrame>

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
    setFixedSize(600, 520);
    
    bool dark = HTheme::isDark();
    
    QString qdialogBg = dark ? QStringLiteral("#151322") : QStringLiteral("#F4F5F7");
    QString labelColor = dark ? QStringLiteral("#FFFFFF") : QStringLiteral("#1F2937");
    QString tabPaneBg = dark ? QStringLiteral("#0D0E15") : QStringLiteral("#FFFFFF");
    QString tabBorder = dark ? QStringLiteral("#2B2A3A") : QStringLiteral("#D1D5DB");
    QString tabBgSelected = dark ? QStringLiteral("#8B5CF6") : QStringLiteral("#3B82F6");
    QString tabBgUnselected = dark ? QStringLiteral("#1C1B2B") : QStringLiteral("#E5E7EB");
    
    QString itemBg = dark ? QStringLiteral("#1C1B2B") : QStringLiteral("#E5E7EB");
    QString itemHover = dark ? QStringLiteral("#2E2A3A") : QStringLiteral("#D1D5DB");
    QString itemTextColor = dark ? QStringLiteral("#FFFFFF") : QStringLiteral("#1F2937");
    
    QString btnCancelTextColor = dark ? QStringLiteral("#8A939B") : QStringLiteral("#4B5563");
    QString btnCancelHoverColor = dark ? QStringLiteral("#FFFFFF") : QStringLiteral("#1F2937");
    
    QString qualityBoxBg = dark ? QStringLiteral("#1C1B2B") : QStringLiteral("#E5E7EB");
    QString qualityBoxText = dark ? QStringLiteral("#8A939B") : QStringLiteral("#4B5563");
    
    setStyleSheet(QString(
        "QDialog { background-color: %1; color: %2; }"
        "QLabel { color: %2; font-size: 14px; font-weight: bold; }"
        "QTabWidget::pane { border: none; background: transparent; padding: 0px; }"
        "QTabBar::tab { background: transparent; color: %3; padding: 8px 16px; font-weight: bold; font-size: 14px; border-bottom: 2px solid transparent; margin-right: 8px; }"
        "QTabBar::tab:selected { background: transparent; color: %2; border-bottom: 2px solid %4; }"
        "QListWidget { background-color: %5; border: 1px solid %6; border-radius: 8px; padding: 8px; color: %7; }"
        "QListWidget::item { padding: 6px; border-radius: 8px; background-color: %8; color: %7; font-weight: bold; font-size: 10px; border: 2px solid transparent; }"
        "QListWidget::item:hover { background-color: %9; }"
        "QListWidget::item:selected { background-color: %9; border: 2px solid %4; color: %2; }"
        "QPushButton { background-color: %4; border: none; border-radius: 6px; color: #FFFFFF; font-weight: bold; padding: 12px 24px; font-size: 13px; }"
        "QPushButton:hover { background-color: %10; }"
        "QPushButton#cancel { background-color: transparent; color: %3; padding: 12px 20px; }"
        "QPushButton#cancel:hover { color: %11; }"
    ).arg(
        qdialogBg, labelColor, btnCancelTextColor, tabBgSelected,
        tabPaneBg, tabBorder, itemTextColor, itemBg, itemHover,
        dark ? QStringLiteral("#A78BFA") : QStringLiteral("#60A5FA"),
        btnCancelHoverColor
    ));

    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(20, 20, 20, 20);
    mainLayout->setSpacing(15);

    QLabel* title = new QLabel(tr("Compartilhe sua tela"), this);
    title->setStyleSheet(QStringLiteral("font-size: 18px; font-weight: bold;"));
    mainLayout->addWidget(title);

    m_tabs = new QTabWidget(this);
    
    m_windowList = new QListWidget(m_tabs);
    m_windowList->setViewMode(QListView::IconMode);
    m_windowList->setResizeMode(QListView::Adjust);
    m_windowList->setGridSize(QSize(170, 130));
    m_windowList->setIconSize(QSize(150, 85));
    m_windowList->setMovement(QListView::Static);
    m_windowList->setSpacing(10);
    m_windowList->setWordWrap(true);
    m_tabs->addTab(m_windowList, tr("Aplicativos"));
    
    m_screenList = new QListWidget(m_tabs);
    m_screenList->setViewMode(QListView::IconMode);
    m_screenList->setResizeMode(QListView::Adjust);
    m_screenList->setGridSize(QSize(170, 130));
    m_screenList->setIconSize(QSize(150, 85));
    m_screenList->setMovement(QListView::Static);
    m_screenList->setSpacing(10);
    m_screenList->setWordWrap(true);
    m_tabs->addTab(m_screenList, tr("Telas"));

    mainLayout->addWidget(m_tabs);

    populateWindows(); // Mostra Aplicativos na aba "Aplicativos" (m_windowList)
    populateScreens(); // Mostra Telas na aba "Telas" (m_screenList)

    // Caixa decorativa de Qualidade de Transmissão (estilo Discord)
    QFrame* qualityBox = new QFrame(this);
    qualityBox->setStyleSheet(QStringLiteral("background-color: %1; border-radius: 8px; padding: 4px;").arg(qualityBoxBg));
    QHBoxLayout* qLayout = new QHBoxLayout(qualityBox);
    qLayout->setContentsMargins(12, 6, 12, 6);
    
    QLabel* resLabel = new QLabel(tr("RESOLUÇÃO: 720p"), qualityBox);
    resLabel->setStyleSheet(QStringLiteral("color: %1; font-size: 11px; font-weight: bold;").arg(qualityBoxText));
    qLayout->addWidget(resLabel);
    
    qLayout->addStretch(1);
    
    QLabel* fpsLabel = new QLabel(tr("TAXA DE QUADROS: 20 FPS"), qualityBox);
    fpsLabel->setStyleSheet(QStringLiteral("color: %1; font-size: 11px; font-weight: bold;").arg(qualityBoxText));
    qLayout->addWidget(fpsLabel);
    
    mainLayout->addWidget(qualityBox);

    QHBoxLayout* btnRow = new QHBoxLayout;
    btnRow->addStretch(1);
    btnRow->setSpacing(10);
    
    QPushButton* cancelBtn = new QPushButton(tr("Cancelar"), this);
    cancelBtn->setObjectName(QStringLiteral("cancel"));
    connect(cancelBtn, &QPushButton::clicked, this, &QDialog::reject);
    btnRow->addWidget(cancelBtn);

    QPushButton* shareBtn = new QPushButton(tr("Compartilhar"), this);
    connect(shareBtn, &QPushButton::clicked, this, [this]() {
        if (m_tabs->currentIndex() == 0) {
            m_selectedSourceType = 1; // Na aba Aplicativos (0), selecionamos Window (1)
            if (m_windowList->currentItem()) {
                m_selectedSourceId = m_windowList->currentItem()->data(Qt::UserRole).toULongLong();
            }
        } else {
            m_selectedSourceType = 0; // Na aba Telas (1), selecionamos Screen (0)
            if (m_screenList->currentItem()) {
                m_selectedSourceId = m_screenList->currentItem()->data(Qt::UserRole).toULongLong();
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
        
        QPixmap preview = s->grabWindow(0).scaled(150, 85, Qt::KeepAspectRatio, Qt::SmoothTransformation);
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
            QPixmap preview = screen->grabWindow(WId(win.hwnd)).scaled(150, 85, Qt::KeepAspectRatio, Qt::SmoothTransformation);
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
