#include "ScreenShareDialog.h"
#include "Theme.h"
#include <QListWidgetItem>
#include <QScreen>
#include <QGuiApplication>
#include <QListView>
#include <QFrame>
#include <QGridLayout>
#include <QMap>

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

ScreenShareDialog::ScreenShareDialog(int maxWidth, int maxHeight, int maxFps,
                                     int maxBitrateKbps, QWidget* parent)
    : QDialog(parent) {
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

    // Controles de qualidade da transmissão WebRTC
    QFrame* qualityBox = new QFrame(this);
    qualityBox->setStyleSheet(QStringLiteral("background-color: %1; border-radius: 8px; padding: 4px;").arg(qualityBoxBg));
    QGridLayout* qLayout = new QGridLayout(qualityBox);
    qLayout->setContentsMargins(12, 8, 12, 8);
    qLayout->setHorizontalSpacing(10);
    qLayout->setVerticalSpacing(5);

    auto makeLabel = [&](const QString& text) {
        QLabel* label = new QLabel(text, qualityBox);
        label->setStyleSheet(QStringLiteral(
            "color: %1; font-size: 10px; font-weight: bold;").arg(qualityBoxText));
        return label;
    };
    qLayout->addWidget(makeLabel(tr("RESOLUÇÃO:")), 0, 0);
    qLayout->addWidget(makeLabel(tr("FPS:")), 0, 1);
    qLayout->addWidget(makeLabel(tr("BITRATE:")), 0, 2);

    const QString comboStyle = QStringLiteral(
        "QComboBox, QSpinBox { background-color: %1; color: %2; border: 1px solid %3; border-radius: 6px; padding: 6px 10px; font-weight: bold; }"
        "QComboBox::drop-down { border: none; width: 24px; }"
        "QComboBox QAbstractItemView { background-color: %1; color: %2; selection-background-color: %4; border: 1px solid %3; }"
    ).arg(tabPaneBg, itemTextColor, tabBorder, tabBgSelected);

    m_qualityCombo = new QComboBox(qualityBox);
    m_qualityCombo->setStyleSheet(comboStyle);
    populateResolutionOptions(maxWidth, maxHeight);
    qLayout->addWidget(m_qualityCombo, 1, 0, 1, 1);

    m_fpsCombo = new QComboBox(qualityBox);
    m_fpsCombo->setStyleSheet(comboStyle);
    populateFpsOptions(maxFps);
    qLayout->addWidget(m_fpsCombo, 1, 1);

    m_maxBitrateKbps = qBound(500, maxBitrateKbps, 50000);
    m_bitrateSpin = new QSpinBox(qualityBox);
    m_bitrateSpin->setRange(500, m_maxBitrateKbps);
    m_bitrateSpin->setSingleStep(500);
    m_bitrateSpin->setSuffix(tr(" kbps"));
    m_bitrateSpin->setStyleSheet(comboStyle);
    qLayout->addWidget(m_bitrateSpin, 1, 2);

    connect(m_qualityCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) { updateRecommendedBitrate(); });
    connect(m_fpsCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) { updateRecommendedBitrate(); });
    updateRecommendedBitrate();

    QLabel* hintLabel = new QLabel(
        tr("Limite do servidor: %1x%2, %3 FPS, %4 kbps")
            .arg(maxWidth).arg(maxHeight).arg(maxFps).arg(maxBitrateKbps), qualityBox);
    hintLabel->setStyleSheet(QStringLiteral(
        "color: %1; font-size: 10px; font-weight: normal;").arg(qualityBoxText));
    qLayout->addWidget(hintLabel, 2, 0, 1, 3);


    mainLayout->addWidget(qualityBox);

    QFrame* audioBox = new QFrame(this);
    audioBox->setStyleSheet(QStringLiteral("background-color: %1; border-radius: 8px; padding: 4px;").arg(qualityBoxBg));
    QHBoxLayout* aLayout = new QHBoxLayout(audioBox);
    aLayout->setContentsMargins(12, 8, 12, 8);
    aLayout->setSpacing(10);

    QLabel* audioLabel = new QLabel(tr("ÁUDIO DA TRANSMISSÃO:"), audioBox);
    audioLabel->setStyleSheet(QStringLiteral("color: %1; font-size: 11px; font-weight: bold;").arg(qualityBoxText));
    aLayout->addWidget(audioLabel);

    m_audioCombo = new QComboBox(audioBox);
    m_audioCombo->addItem(tr("Sem áudio do PC"), 0);
    m_audioCombo->addItem(tr("Áudio de todo o PC"), 1);
    m_audioCombo->setToolTip(tr("Captura os outros aplicativos sem retransmitir vozes e avisos do Halla. Requer Windows build 20348 ou mais recente."));
    m_audioCombo->setCurrentIndex(0);
    m_audioCombo->setStyleSheet(m_qualityCombo->styleSheet());
    aLayout->addWidget(m_audioCombo, 1);

    mainLayout->addWidget(audioBox);

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


void ScreenShareDialog::populateResolutionOptions(int maxWidth, int maxHeight) {
    maxWidth = qBound(640, maxWidth, 3840);
    maxHeight = qBound(360, maxHeight, 2160);
    const int standardHeights[] = {480, 720, 1080, 1440, 2160};
    const QMap<int, QString> names{
        {480, tr("480p")},
        {720, tr("720p HD")},
        {1080, tr("1080p Full HD")},
        {1440, tr("1440p 2K")},
        {2160, tr("2160p 4K")},
    };
    auto widthForHeight = [&](int height) {
        int width = qRound(double(height) * double(maxWidth) / double(maxHeight));
        width = qBound(2, width & ~1, maxWidth);
        return width;
    };
    for (int height : standardHeights) {
        if (height > maxHeight) continue;
        const int width = widthForHeight(height);
        if (width < 640) continue;
        const QString label = QStringLiteral("%1  (%2x%3)")
            .arg(names.value(height)).arg(width).arg(height);
        m_resolutionOptions.push_back({width, height, label});
        m_qualityCombo->addItem(label, m_resolutionOptions.size() - 1);
    }
    const QList<int> standards{480, 720, 1080, 1440, 2160};
    if (!standards.contains(maxHeight) && maxHeight >= 360) {
        const QString label = tr("%1p — máximo do servidor (%2x%3)")
            .arg(maxHeight).arg(maxWidth).arg(maxHeight);
        m_resolutionOptions.push_back({maxWidth, maxHeight, label});
        m_qualityCombo->addItem(label, m_resolutionOptions.size() - 1);
    }
    if (m_resolutionOptions.isEmpty()) {
        const QString label = tr("Máximo do servidor (%1x%2)")
            .arg(maxWidth).arg(maxHeight);
        m_resolutionOptions.push_back({maxWidth, maxHeight, label});
        m_qualityCombo->addItem(label, 0);
    }
    m_qualityCombo->setCurrentIndex(m_qualityCombo->count() - 1);
}

void ScreenShareDialog::populateFpsOptions(int maxFps) {
    maxFps = qBound(1, maxFps, 60);
    if (maxFps < 30) {
        m_fpsCombo->addItem(tr("%1 FPS").arg(maxFps), maxFps);
    } else {
        m_fpsCombo->addItem(tr("30 FPS"), 30);
        if (maxFps > 30) m_fpsCombo->addItem(tr("%1 FPS").arg(maxFps), maxFps);
    }
    m_fpsCombo->setCurrentIndex(m_fpsCombo->count() - 1);
}

int ScreenShareDialog::recommendedBitrateKbps(int width, int height, int fps) const {
    int bitrate30 = 1200;
    int bitrate60 = 2500;
    if (height > 480 && height <= 720) { bitrate30 = 2500; bitrate60 = 4500; }
    else if (height > 720 && height <= 1080) { bitrate30 = 4500; bitrate60 = 8000; }
    else if (height > 1080 && height <= 1440) { bitrate30 = 9000; bitrate60 = 16000; }
    else if (height > 1440) { bitrate30 = 18000; bitrate60 = 32000; }
    int bitrate = fps <= 30 ? bitrate30
        : bitrate30 + (bitrate60 - bitrate30) * (fps - 30) / 30;
    const int standardWidth = qMax(1, qRound(double(height) * 16.0 / 9.0));
    bitrate = qMax(500, bitrate * width / standardWidth);
    return qMin(bitrate, m_maxBitrateKbps);
}

void ScreenShareDialog::updateRecommendedBitrate() {
    if (!m_qualityCombo || !m_fpsCombo || !m_bitrateSpin
            || m_resolutionOptions.isEmpty()) return;
    const int index = m_qualityCombo->currentData().toInt();
    const ResolutionOption option = index >= 0 && index < m_resolutionOptions.size()
        ? m_resolutionOptions[index] : m_resolutionOptions.last();
    m_bitrateSpin->setValue(recommendedBitrateKbps(
        option.width, option.height, m_fpsCombo->currentData().toInt()));
}

int ScreenShareDialog::selectedWidth() const {
    const int index = m_qualityCombo ? m_qualityCombo->currentData().toInt() : -1;
    return index >= 0 && index < m_resolutionOptions.size()
        ? m_resolutionOptions[index].width : 854;
}

int ScreenShareDialog::selectedHeight() const {
    const int index = m_qualityCombo ? m_qualityCombo->currentData().toInt() : -1;
    return index >= 0 && index < m_resolutionOptions.size()
        ? m_resolutionOptions[index].height : 480;
}

int ScreenShareDialog::selectedFps() const {
    return m_fpsCombo ? m_fpsCombo->currentData().toInt() : 30;
}

int ScreenShareDialog::selectedBitrateKbps() const {
    return m_bitrateSpin ? m_bitrateSpin->value() : 1200;
}

bool ScreenShareDialog::captureSystemAudio() const {
    return m_audioCombo && m_audioCombo->currentData().toInt() == 1;
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
