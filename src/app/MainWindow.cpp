#include "MainWindow.h"
#include "Theme.h"
#include "gui/ServerTab.h"
#include "gui/ServerTreeWidget.h"
#include "gui/ChatPanel.h"
#include "gui/WelcomePage.h"
#include "gui/InfoPanel.h"
#include "gui/Icons.h"
#include "net/NetSession.h"
#include "core/Settings.h"
#include "core/GroupIconCache.h"
#include "core/AppLog.h"
#include "plugins/PluginManager.h"
#include "dialogs/ConnectDialog.h"
#include "dialogs/IdentityDialog.h"
#include "dialogs/BookmarksDialog.h"
#include "dialogs/OptionsDialog.h"
#include "dialogs/GroupsDialog.h"
#include "dialogs/MiniDialogs.h"
#include "dialogs/LogDialog.h"
#include "dialogs/ToolsDialogs.h"
#include "dialogs/AdminDialogs.h"
#include "dialogs/AboutDialog.h"
#include "dialogs/ScreenShareDialog.h"
#include "webrtc/HallaWebRtcSession.h"
#include <QScreen>
#include <QGuiApplication>
#include <QPixmap>
#include <QBuffer>
#include "net/VoiceEngine.h"
#include "SoundPack.h"
#include "Speech.h"
#include "gui/HotkeyEdit.h"
#include "version.h"

#include <QMenuBar>
#include <QToolBar>
#include <QStatusBar>
#include <QInputDialog>
#include <QLineEdit>
#include <QPainter>
#include <QPainterPath>
#include <QFrame>
#include <QGraphicsDropShadowEffect>
#include <QGraphicsOpacityEffect>
#include <QHBoxLayout>
#include <QPropertyAnimation>
#include <QEasingCurve>
#include <QMouseEvent>
#include <QEnterEvent>
#include <QTabBar>
#include <QStackedWidget>
#include <QSplitter>
#include <QSystemTrayIcon>
#include <QMessageBox>
#include <QProcess>
#include <QCloseEvent>
#include <QApplication>
#include <QShortcut>
#include <QLabel>
#include <QToolButton>
#include <QTimer>
#include <QDialog>
#include <QPushButton>
#include <QJsonObject>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QRandomGenerator>
#include <functional>
#include <utility>
#include "gui/HotkeyVk.h" // portable spec<->VK core (v1.1.27)
#ifdef Q_OS_WIN
#include <windows.h>
#include <QImage>
#include <QPixmap>

// Advanced application capture with hardware (GPU) acceleration via PrintWindow
static QPixmap grabWindowsApp(HWND hwnd) {
    if (!hwnd) return QPixmap();

    RECT rect;
    GetWindowRect(hwnd, &rect);
    int width = rect.right - rect.left;
    int height = rect.bottom - rect.top;
    if (width <= 0 || height <= 0) return QPixmap();

    HDC hdcWindow = GetWindowDC(hwnd);
    HDC hdcMem = CreateCompatibleDC(hdcWindow);
    HBITMAP hbmMem = CreateCompatibleBitmap(hdcWindow, width, height);
    HGDIOBJ hOld = SelectObject(hdcMem, hbmMem);

    // Flag 2 (PW_RENDERFULLCONTENT) forces rendering of hardware-accelerated content
    BOOL ok = PrintWindow(hwnd, hdcMem, 2);
    if (!ok) {
        ok = PrintWindow(hwnd, hdcMem, 0); // default fallback
    }

    SelectObject(hdcMem, hOld);
    DeleteDC(hdcMem);
    ReleaseDC(hwnd, hdcWindow);

    QPixmap pix;
    if (ok) {
        pix = QPixmap::fromImage(QImage::fromHBITMAP(hbmMem));
    }
    DeleteObject(hbmMem);
    return pix;
}
#endif

// hotkey spec ("Ctrl+F2", "\\", "Num+5", accented keys...) -> QKeySequence.
// Tries the QKeySequence parser; when it rejects the text -- bare punctuation,
// Latin-1 accents, "Num+"/"Meta+" -- falls back to the manual HotkeyVk parser.
// v1.1.27: without this, keys such as "\\" never came back from the profile.
static QKeySequence hotkeySpecToSequence(const QString& spec) {
    const QString s = spec.trimmed();
    if (s.isEmpty()) return QKeySequence();
    const QKeySequence ks = QKeySequence::fromString(s);
    if (!ks.isEmpty() && ks[0].toCombined() != 0)
        return ks;
    const int combined = hvk::parseSpec(s.toUtf8().constData());
    if (combined == 0) return QKeySequence();
    return QKeySequence(QKeyCombination::fromCombined(combined));
}

#ifdef Q_OS_WIN
// definidas mais abaixo (mesmo arquivo)
bool specToVk(const QKeySequence& ks, UINT& vk, UINT& mods);
// v1.1.27: converts the stored SPEC ("\\", "Ctrl+\\", "Num+5", accented keys...) straight
// into VK+mods -- tolerates what QKeySequence::fromString rejects
bool specToVkFromSpec(const QString& spec, UINT& vk, UINT& mods);
#endif

class ScreenShareWindow : public QDialog {
public:
    using AudioMuteCallback = std::function<void(bool)>;

    explicit ScreenShareWindow(int userId, const QString& userName,
                               bool viewerControls = false,
                               AudioMuteCallback audioMuteChanged = {},
                               QWidget* parent = nullptr)
        : QDialog(parent), m_userId(userId), m_viewerControls(viewerControls),
          m_audioMuteChanged(std::move(audioMuteChanged)) {
        setWindowTitle(tr("Compartilhamento de Tela - %1").arg(userName));
        resize(800, 480);
        setMinimumSize(400, 240);
        setWindowFlags(Qt::Window | Qt::WindowMinMaxButtonsHint | Qt::WindowCloseButtonHint);
        setStyleSheet(QStringLiteral("background-color: #0D0E15; color: #FFFFFF;"));
        setMouseTracking(true);
        setAttribute(Qt::WA_Hover, true);

        QVBoxLayout* l = new QVBoxLayout(this);
        l->setContentsMargins(0, 0, 0, 0);
        m_label = new QLabel(this);
        m_label->setAlignment(Qt::AlignCenter);
        m_label->setText(tr("Aguardando transmissão..."));
        m_label->setStyleSheet(QStringLiteral("font-size: 16px; font-weight: bold; color: #8A939B;"));
        m_label->setMouseTracking(true);
        m_label->setAttribute(Qt::WA_Hover, true);
        m_label->installEventFilter(this);
        l->addWidget(m_label);

        if (m_viewerControls) createViewerControls();
    }

    int userId() const { return m_userId; }
    bool isAudioMuted() const { return m_audioMuted; }

    // v1.1.28: the window is no longer an endless "Waiting for broadcast..." pit.
    // Connection/watch errors show up here, with an option to retry (resends the
    // webrtc_watch_request and recreates the peer).
    void setStatusMessage(const QString& message, bool isError,
                          std::function<void()> retry = {}) {
        m_retryCallback = std::move(retry);
        m_label->setText(message);
        m_label->setStyleSheet(isError
            ? QStringLiteral("font-size: 15px; font-weight: bold; color: #FF5C72;")
            : QStringLiteral("font-size: 16px; font-weight: bold; color: #8A939B;"));
        if (isError && m_retryCallback) showRetryButton();
        else if (m_retryButton) m_retryButton->hide();
    }

    // Returns to the waiting state (used when starting a new attempt).
    void resetWatchStatus() {
        if (!m_currentPixmap.isNull()) return; // video is already there -- nothing to report
        m_label->setText(tr("Aguardando transmissão..."));
        m_label->setStyleSheet(QStringLiteral("font-size: 16px; font-weight: bold; color: #8A939B;"));
        if (m_retryButton) m_retryButton->hide();
    }

    void updateFrame(const QByteArray& jpegData) {
        if (m_retryButton) m_retryButton->hide();
        if (m_currentPixmap.loadFromData(jpegData)) scaleFrame();
    }

    void updateImage(const QImage& image) {
        if (image.isNull()) return;
        if (m_retryButton) m_retryButton->hide();
        m_currentPixmap = QPixmap::fromImage(image);
        scaleFrame();
    }

protected:
    void resizeEvent(QResizeEvent* event) override {
        QDialog::resizeEvent(event);
        scaleFrame();
        layoutViewerControls();
        layoutRetryButton();
    }

    void enterEvent(QEnterEvent* event) override {
        QDialog::enterEvent(event);
        noteMouseActivity();
    }

    void leaveEvent(QEvent* event) override {
        QDialog::leaveEvent(event);
        hideViewerControls(false);
    }

    void mouseMoveEvent(QMouseEvent* event) override {
        QDialog::mouseMoveEvent(event);
        noteMouseActivity();
    }

    bool eventFilter(QObject* watched, QEvent* event) override {
        Q_UNUSED(watched);
        if (!m_viewerControls) return false;
        switch (event->type()) {
        case QEvent::Enter:
        case QEvent::MouseMove:
        case QEvent::HoverMove:
            noteMouseActivity();
            break;
        case QEvent::Leave:
            QTimer::singleShot(0, this, [this] {
                if (!underMouse()) hideViewerControls(false);
            });
            break;
        default:
            break;
        }
        return false;
    }

private:
    QRect controlsGeometry(bool visible) const {
        constexpr int kHeight = 66;
        const int panelWidth = qMin(520, qMax(300, width() - 32));
        const int x = (width() - panelWidth) / 2;
        const int y = visible ? height() - kHeight - 18 : height() + 6;
        return QRect(x, y, panelWidth, kHeight);
    }

    void createViewerControls() {
        m_controls = new QFrame(this);
        m_controls->setObjectName(QStringLiteral("liveControls"));
        m_controls->setMouseTracking(true);
        m_controls->setAttribute(Qt::WA_Hover, true);
        m_controls->setStyleSheet(QStringLiteral(
            "QFrame#liveControls { background: rgba(18, 20, 31, 238); border: 1px solid rgba(255,255,255,35); border-radius: 17px; }"
            "QPushButton { min-height: 42px; padding: 0 18px; border: none; border-radius: 12px; color: #FFFFFF; font-size: 13px; font-weight: 800; background: rgba(255,255,255,20); }"
            "QPushButton:hover { background: rgba(255,255,255,36); }"
            "QPushButton:pressed { background: rgba(255,255,255,48); }"
            "QPushButton#stopWatching { background: #D83B4D; }"
            "QPushButton#stopWatching:hover { background: #ED4A5D; }"));

        auto* row = new QHBoxLayout(m_controls);
        row->setContentsMargins(12, 11, 12, 11);
        row->setSpacing(10);

        m_audioButton = new QPushButton(m_controls);
        m_audioButton->setIconSize(QSize(20, 20));
        row->addWidget(m_audioButton, 1);

        m_stopButton = new QPushButton(HIcons::disconnectPlug(), tr("Parar de assistir"), m_controls);
        m_stopButton->setObjectName(QStringLiteral("stopWatching"));
        m_stopButton->setIconSize(QSize(20, 20));
        row->addWidget(m_stopButton, 1);

        updateAudioButton();
        for (QWidget* widget : {static_cast<QWidget*>(m_controls),
                                static_cast<QWidget*>(m_audioButton),
                                static_cast<QWidget*>(m_stopButton)}) {
            widget->setMouseTracking(true);
            widget->setAttribute(Qt::WA_Hover, true);
            widget->installEventFilter(this);
        }

        connect(m_audioButton, &QPushButton::clicked, this, [this] {
            m_audioMuted = !m_audioMuted;
            updateAudioButton();
            if (m_audioMuteChanged) m_audioMuteChanged(m_audioMuted);
            noteMouseActivity();
        });
        connect(m_stopButton, &QPushButton::clicked, this, [this] { close(); });

        auto* opacity = new QGraphicsOpacityEffect(m_controls);
        opacity->setOpacity(0.0);
        m_controls->setGraphicsEffect(opacity);
        m_slideAnimation = new QPropertyAnimation(m_controls, "geometry", this);
        m_opacityAnimation = new QPropertyAnimation(opacity, "opacity", this);
        for (QPropertyAnimation* animation : {m_slideAnimation, m_opacityAnimation}) {
            animation->setDuration(190);
            animation->setEasingCurve(QEasingCurve::OutCubic);
        }
        connect(m_opacityAnimation, &QPropertyAnimation::finished, this, [this] {
            if (!m_controlsShown && m_controls) m_controls->hide();
        });

        m_hideControlsTimer = new QTimer(this);
        m_hideControlsTimer->setSingleShot(true);
        m_hideControlsTimer->setInterval(1800);
        connect(m_hideControlsTimer, &QTimer::timeout, this, [this] {
            hideViewerControls(true);
        });

        m_controls->setGeometry(controlsGeometry(false));
        m_controls->hide();
    }

    void updateAudioButton() {
        if (!m_audioButton) return;
        m_audioButton->setIcon(HIcons::muteSpeaker(m_audioMuted));
        m_audioButton->setText(m_audioMuted ? tr("Ativar áudio") : tr("Mutar áudio"));
    }

    void setBlankCursor(bool blank) {
        if (m_cursorHidden == blank) return;
        m_cursorHidden = blank;
        const Qt::CursorShape shape = blank ? Qt::BlankCursor : Qt::ArrowCursor;
        for (QWidget* widget : {static_cast<QWidget*>(this),
                                static_cast<QWidget*>(m_label),
                                static_cast<QWidget*>(m_controls),
                                static_cast<QWidget*>(m_audioButton),
                                static_cast<QWidget*>(m_stopButton)}) {
            if (!widget) continue;
            if (blank) widget->setCursor(shape);
            else widget->unsetCursor();
        }
    }

    void noteMouseActivity() {
        if (!m_viewerControls || !m_controls) return;
        setBlankCursor(false);
        m_hideControlsTimer->start();
        if (m_controlsShown) return;
        m_controlsShown = true;
        m_controls->show();
        m_controls->raise();
        m_slideAnimation->stop();
        m_opacityAnimation->stop();
        m_slideAnimation->setStartValue(m_controls->geometry());
        m_slideAnimation->setEndValue(controlsGeometry(true));
        auto* opacity = qobject_cast<QGraphicsOpacityEffect*>(m_controls->graphicsEffect());
        m_opacityAnimation->setStartValue(opacity ? opacity->opacity() : 0.0);
        m_opacityAnimation->setEndValue(1.0);
        m_slideAnimation->start();
        m_opacityAnimation->start();
    }

    void hideViewerControls(bool hideCursor) {
        if (!m_viewerControls || !m_controls) return;
        if (hideCursor) setBlankCursor(true);
        if (!m_controlsShown) return;
        m_controlsShown = false;
        m_hideControlsTimer->stop();
        m_slideAnimation->stop();
        m_opacityAnimation->stop();
        m_slideAnimation->setStartValue(m_controls->geometry());
        m_slideAnimation->setEndValue(controlsGeometry(false));
        auto* opacity = qobject_cast<QGraphicsOpacityEffect*>(m_controls->graphicsEffect());
        m_opacityAnimation->setStartValue(opacity ? opacity->opacity() : 1.0);
        m_opacityAnimation->setEndValue(0.0);
        m_slideAnimation->start();
        m_opacityAnimation->start();
    }

    void layoutViewerControls() {
        if (!m_controls) return;
        m_slideAnimation->stop();
        m_controls->setGeometry(controlsGeometry(m_controlsShown));
    }

    void scaleFrame() {
        if (m_currentPixmap.isNull()) return;
        // SmoothTransformation on every 30 FPS frame blocked the UI thread. The
        // video already arrives scaled from WebRTC; FastTransformation avoids
        // backlog and keeps the most recent frame on screen.
        m_label->setPixmap(m_currentPixmap.scaled(
            m_label->size(), Qt::KeepAspectRatio, Qt::FastTransformation));
    }

    void showRetryButton() {
        if (!m_retryButton) {
            m_retryButton = new QPushButton(tr("Tentar novamente"), m_label);
            m_retryButton->setObjectName(QStringLiteral("retryWatch"));
            m_retryButton->setCursor(Qt::PointingHandCursor);
            m_retryButton->setStyleSheet(QStringLiteral(
                "QPushButton#retryWatch { min-height: 34px; padding: 0 22px;"
                " border: 1px solid rgba(255,92,114,140); border-radius: 10px;"
                " color: #FFD7DD; font-size: 13px; font-weight: 800;"
                " background: rgba(255,92,114,38); }"
                "QPushButton#retryWatch:hover { background: rgba(255,92,114,70); }"));
            connect(m_retryButton, &QPushButton::clicked, this, [this] {
                if (m_retryCallback) {
                    resetWatchStatus();
                    auto cb = std::move(m_retryCallback);
                    m_retryCallback = {};
                    cb();
                }
            });
        }
        layoutRetryButton();
        m_retryButton->show();
        m_retryButton->raise();
    }

    void layoutRetryButton() {
        if (!m_retryButton) return;
        m_retryButton->adjustSize();
        m_retryButton->setGeometry(
            (m_label->width() - m_retryButton->width()) / 2,
            m_label->height() / 2 + 46,
            m_retryButton->width(), m_retryButton->height());
    }

    int m_userId = 0;
    bool m_viewerControls = false;
    bool m_audioMuted = false;
    bool m_controlsShown = false;
    bool m_cursorHidden = false;
    AudioMuteCallback m_audioMuteChanged;
    std::function<void()> m_retryCallback; // v1.1.28: retry de watch
    QPushButton* m_retryButton = nullptr;
    QLabel* m_label = nullptr;
    QFrame* m_controls = nullptr;
    QPushButton* m_audioButton = nullptr;
    QPushButton* m_stopButton = nullptr;
    QTimer* m_hideControlsTimer = nullptr;
    QPropertyAnimation* m_slideAnimation = nullptr;
    QPropertyAnimation* m_opacityAnimation = nullptr;
    QPixmap m_currentPixmap;
};

static QPixmap makePurpleLiveWavePixmap(const QSize& size) {
    QPixmap pm(size);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    QRectF r(0, 0, size.width(), size.height());
    QLinearGradient bg(r.topLeft(), r.bottomRight());
    bg.setColorAt(0.0, QColor(18, 12, 38));
    bg.setColorAt(1.0, QColor(7, 9, 22));
    p.setBrush(bg);
    p.setPen(Qt::NoPen);
    p.drawRoundedRect(r, 18, 18);

    for (int i = 0; i < 36; ++i) {
        const qreal x = QRandomGenerator::global()->bounded(size.width());
        const qreal y = QRandomGenerator::global()->bounded(size.height());
        const int a = QRandomGenerator::global()->bounded(35, 115);
        p.setPen(QPen(QColor(170, 70, 255, a), 1));
        p.drawPoint(QPointF(x, y));
    }

    auto drawWave = [&](QColor color, qreal amp, qreal offset, qreal width) {
        QPainterPath path;
        const qreal mid = size.height() * 0.42 + offset;
        path.moveTo(0, mid);
        for (int x = 0; x <= size.width(); x += 8) {
            const qreal t = qreal(x) / qreal(size.width());
            const qreal y = mid + qSin(t * 6.28 * 3.0) * amp * (0.45 + 0.55 * qSin(t * 6.28));
            path.lineTo(x, y);
        }
        QPen pen(color, width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        p.setPen(pen);
        p.drawPath(path);
    };

    drawWave(QColor(210, 72, 255, 210), 34, -6, 6);
    drawWave(QColor(108, 42, 255, 180), 26, 12, 18);
    drawWave(QColor(244, 89, 255, 95), 18, -2, 28);

    QRadialGradient pulse(QPointF(size.width() * 0.5, size.height() * 0.42), 86);
    pulse.setColorAt(0.0, QColor(222, 68, 255, 180));
    pulse.setColorAt(0.25, QColor(153, 56, 255, 70));
    pulse.setColorAt(1.0, QColor(153, 56, 255, 0));
    p.setBrush(pulse);
    p.setPen(Qt::NoPen);
    p.drawEllipse(QPointF(size.width() * 0.5, size.height() * 0.42), 86, 86);
    p.setBrush(QColor(212, 70, 255, 220));
    p.drawEllipse(QPointF(size.width() * 0.5, size.height() * 0.42), 11, 11);
    return pm;
}

class WatchLiveButton final : public QPushButton {
public:
    explicit WatchLiveButton(QWidget* parent = nullptr) : QPushButton(parent) {
        setFixedSize(232, 46);
        setText(tr("Assistir Live"));
        setCursor(Qt::PointingHandCursor);
        setFlat(true);
        setFocusPolicy(Qt::NoFocus);
        setAttribute(Qt::WA_TranslucentBackground, true);
    }

protected:
    void enterEvent(QEnterEvent* event) override {
        m_hovered = true;
        update();
        QPushButton::enterEvent(event);
    }

    void leaveEvent(QEvent* event) override {
        m_hovered = false;
        update();
        QPushButton::leaveEvent(event);
    }

    void paintEvent(QPaintEvent* event) override {
        Q_UNUSED(event);
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);

        const QRectF pill(2.5, 3.0, width() - 5.0, height() - 6.0);
        painter.setPen(QPen(QColor(49, 134, 255, m_hovered ? 210 : 155), 3.0));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(pill.adjusted(-0.2, -0.2, 0.2, 0.2), 21, 21);

        QLinearGradient background(pill.topLeft(), pill.topRight());
        background.setColorAt(0.0, m_hovered ? QColor("#712DFF") : QColor("#5B21E8"));
        background.setColorAt(0.48, m_hovered ? QColor("#263DFF") : QColor("#1732E8"));
        background.setColorAt(1.0, m_hovered ? QColor("#087BFF") : QColor("#075FEF"));
        painter.setPen(QPen(QColor(151, 120, 255, 210), 1.0));
        painter.setBrush(background);
        painter.drawRoundedRect(pill, 20, 20);

        const QRectF liveOrb(7.0, 6.0, 34.0, 34.0);
        QRadialGradient orb(liveOrb.center(), liveOrb.width() / 2.0);
        orb.setColorAt(0.0, QColor("#6B56FF"));
        orb.setColorAt(0.72, QColor("#6841F1"));
        orb.setColorAt(1.0, QColor("#A66CFF"));
        painter.setPen(QPen(QColor(225, 222, 255, 220), 1.1));
        painter.setBrush(orb);
        painter.drawEllipse(liveOrb);

        const QPointF center = liveOrb.center();
        painter.setPen(Qt::NoPen);
        painter.setBrush(Qt::white);
        painter.drawEllipse(center, 3.5, 3.5);
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(Qt::white, 2.0, Qt::SolidLine, Qt::RoundCap));
        auto wave = [&](qreal side, qreal distance, qreal height) {
            QPainterPath path;
            path.moveTo(center.x() + side * distance, center.y() - height);
            path.cubicTo(center.x() + side * (distance + 3.0), center.y() - height * 0.45,
                         center.x() + side * (distance + 3.0), center.y() + height * 0.45,
                         center.x() + side * distance, center.y() + height);
            painter.drawPath(path);
        };
        wave(-1.0, 7.0, 6.0);
        wave(1.0, 7.0, 6.0);
        wave(-1.0, 11.5, 10.0);
        wave(1.0, 11.5, 10.0);

        const QRectF playCapsule(width() - 50.0, 8.0, 42.0, 30.0);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(255, 255, 255, 246));
        painter.drawRoundedRect(playCapsule, 15, 15);
        QPolygonF triangle;
        triangle << QPointF(playCapsule.center().x() - 4.0, playCapsule.center().y() - 7.0)
                 << QPointF(playCapsule.center().x() - 4.0, playCapsule.center().y() + 7.0)
                 << QPointF(playCapsule.center().x() + 7.0, playCapsule.center().y());
        painter.setBrush(QColor("#2D30E9"));
        painter.drawPolygon(triangle);

        QFont labelFont = font();
        labelFont.setPointSize(12);
        labelFont.setWeight(QFont::Black);
        painter.setFont(labelFont);
        painter.setPen(Qt::white);
        painter.drawText(QRectF(47.0, 0.0, width() - 102.0, height()),
                         Qt::AlignCenter, text());
    }

private:
    bool m_hovered = false;
};

class ScreenshareHoverPopup : public QFrame {
public:
    explicit ScreenshareHoverPopup(int userId, const QString& userName, int channelId, const QByteArray& jpegData,
                                     const QRect& sourceRect, class MainWindow* mw, QWidget* parent = nullptr)
        : QFrame(parent, Qt::ToolTip | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint),
          m_userId(userId), m_channelId(channelId), m_sourceRect(sourceRect), m_mw(mw) {
        Q_UNUSED(userName);
        Q_UNUSED(jpegData);
        setAttribute(Qt::WA_DeleteOnClose);
        setAttribute(Qt::WA_TranslucentBackground, true);
        setProperty("streamUserId", userId);
        setFixedSize(244, 58);
        setObjectName(QStringLiteral("watchPill"));
        setStyleSheet(QStringLiteral("QFrame#watchPill { background: transparent; border: none; }"));

        auto* shadow = new QGraphicsDropShadowEffect(this);
        shadow->setBlurRadius(18);
        shadow->setColor(QColor(32, 91, 255, 150));
        shadow->setOffset(0, 4);
        setGraphicsEffect(shadow);

        QHBoxLayout* layout = new QHBoxLayout(this);
        layout->setContentsMargins(6, 6, 6, 6);
        WatchLiveButton* button = new WatchLiveButton(this);
        connect(button, &QPushButton::clicked, this, &ScreenshareHoverPopup::onWatchClicked);
        layout->addWidget(button);

        m_timer = new QTimer(this);
        connect(m_timer, &QTimer::timeout, this, &ScreenshareHoverPopup::checkMousePosition);
        m_timer->start(80);
    }

protected:
    void leaveEvent(QEvent* e) override {
        QFrame::leaveEvent(e);
        checkMousePosition();
    }

private:
    void checkMousePosition() {
        const QPoint cursor = QCursor::pos();
        const QRect buttonRect(mapToGlobal(QPoint(0, 0)), size());
        if (!buttonRect.adjusted(-4, -4, 4, 4).contains(cursor)
                && !m_sourceRect.adjusted(-6, -8, 6, 8).contains(cursor)) {
            close();
        }
    }

    void onWatchClicked();

    int m_userId;
    int m_channelId;
    QRect m_sourceRect;
    class MainWindow* m_mw;
    QTimer* m_timer = nullptr;
};

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    // forceQuit is transient (tray menu/updater), never a preference persisted
    // between runs.
    S::set("app/forceQuit", false);
    setWindowTitle(QString::fromUtf8(halla::kAppName));
    // Keeps a large source so Windows picks a crisp version in the window, on the
    // taskbar and in Alt+Tab.
    setWindowIcon(QIcon(HIcons::appIcon(256)));
    setMinimumSize(1100, 700);
    resize(1706, 922);

    // ------------------------- menus -----------------------------------
    QMenu* mConn = menuBar()->addMenu(tr("&Conexões"));
    m_actConnect = mConn->addAction(HIcons::connectPlug(), tr("Conectar..."), this,
                                    [this] { openConnectDialog(false); });
    m_actConnect->setShortcut(QKeySequence(QStringLiteral("Ctrl+N")));
    mConn->addAction(tr("Conectar em nova aba..."), this,
                     [this] { openConnectDialog(true); });
    mConn->addSeparator();
    m_actDisconnect = mConn->addAction(HIcons::disconnectPlug(), tr("Desconectar"), this,
                                       [this] {
                                           if (ServerTab* t = currentTab())
                                               disconnectTab(t);
                                       });
    m_actDisconnect->setShortcut(QKeySequence(QStringLiteral("Ctrl+D")));
    m_actDisconnectAll = mConn->addAction(tr("Desconectar de todos os servidores"), this,
                                          [this] { disconnectAllTabs(true); });
    m_recentMenu = mConn->addMenu(tr("Conexões recentes"));
    mConn->addAction(HIcons::info(), tr("Informações de conexão..."), this, [this] {
        ServerTab* t = currentTab();
        ServerConnectionInfoDialog dlg(t ? &t->data() : nullptr, t ? t->net() : nullptr, this);
        dlg.exec();
    });
    mConn->addSeparator();
    mConn->addAction(tr("Sair"), this, [this] {
        S::set("app/forceQuit", true);
        close();
    })->setShortcut(QKeySequence(QStringLiteral("Ctrl+Q")));

    m_bookmarksMenu = menuBar()->addMenu(tr("&Marcadores"));
    m_actBookmarkAdd = m_bookmarksMenu->addAction(
        HIcons::bookmarkStar(), tr("Adicionar aos marcadores..."), this,
        [this] {
            ServerTab* t = currentTab();
            openBookmarksDialog(t ? t->data().name : QString(),
                                t ? t->data().address : QString());
        });
    m_bookmarksMenu->addAction(tr("Gerenciar marcadores..."), this,
                               [this] { openBookmarksDialog(); });
    // dynamic entries are inserted by rebuildBookmarksMenu()

    // ------------------------- Self (own client state) -------------------
    // state actions created here: used in the "Self" menu, on the toolbar and by
    // the global shortcuts
    m_actAway = new QAction(HIcons::away(false), tr("Ausente"), this);
    m_actAway->setCheckable(true);
    m_actAway->setToolTip(tr("Definir como ausente"));
    connect(m_actAway, &QAction::triggered, this, [this] {
        if (ServerTab* t = currentTab()) t->setAway(m_actAway->isChecked());
        updateConnectionUi();
    });

    m_actMuteMic = new QAction(HIcons::muteMic(false), tr("Mudo (microfone)"), this);
    m_actMuteMic->setCheckable(true);
    m_actMuteMic->setToolTip(tr("Silenciar o microfone"));
    connect(m_actMuteMic, &QAction::triggered, this, [this] {
        if (ServerTab* t = currentTab()) t->setMicMuted(m_actMuteMic->isChecked());
        updateConnectionUi();
    });

    m_actMuteSpk = new QAction(HIcons::muteSpeaker(false), tr("Mudo (alto-falantes)"), this);
    m_actMuteSpk->setCheckable(true);
    m_actMuteSpk->setToolTip(tr("Silenciar os alto-falantes"));
    connect(m_actMuteSpk, &QAction::triggered, this, [this] {
        if (ServerTab* t = currentTab()) t->setSpeakersMuted(m_actMuteSpk->isChecked());
        updateConnectionUi();
    });

    m_actScreenShare = new QAction(HIcons::screenShare(false), tr("Compartilhar tela"), this);
    m_actScreenShare->setCheckable(true);
    m_actScreenShare->setToolTip(tr("Compartilhar a tela do seu PC"));
    connect(m_actScreenShare, &QAction::triggered, this, &MainWindow::toggleScreenShare);

    m_actRenameSelf = new QAction(HIcons::identity(), tr("Alterar apelido..."), this);
    connect(m_actRenameSelf, &QAction::triggered, this, [this] {
        if (ServerTab* t = currentTab()) t->renameSelf();
    });

    m_actCommander = new QAction(tr("Alternar comandante do canal"), this);
    connect(m_actCommander, &QAction::triggered, this, [this] {
        if (ServerTab* t = currentTab()) t->toggleCommander();
    });

    QMenu* mSelf = menuBar()->addMenu(tr("&Si mesmo"));
    mSelf->addAction(m_actAway);
    mSelf->addAction(m_actMuteMic);
    mSelf->addAction(m_actMuteSpk);
    mSelf->addSeparator();
    mSelf->addAction(m_actRenameSelf);
    mSelf->addAction(m_actCommander);
    mSelf->addSeparator();
    mSelf->addAction(tr("Definir avatar..."), this, [this] {
        if (ServerTab* t = currentTab()) t->setAvatarInteractive();
    });
    mSelf->addAction(tr("Remover avatar"), this, [this] {
        if (ServerTab* t = currentTab()) t->removeAvatar();
    });

    QMenu* mPerm = menuBar()->addMenu(tr("&Permissões"));
    m_actPrivilegeKey = mPerm->addAction(HIcons::key(), tr("Usar chave de privilégio..."), this,
                                         [this] {
                                             ServerTab* t = currentTab();
                                             if (!t) return;
                                             PrivilegeKeyDialog dlg(this);
                                             if (dlg.exec() == QDialog::Accepted &&
                                                 !dlg.key().isEmpty()) {
                                                 if (t->isNetworked() && t->net()) {
                                                     t->net()->usePrivilegeKey(dlg.key());
                                                 }
                                                 t->chat()->addServerSystem(
                                                     tr("Chave de privilégio usada."));
                                                 AppLog::info(tr("Chave de privilégio usada"));
                                             }
                                         });
    mPerm->addSeparator();
    m_actServerGroups = mPerm->addAction(HIcons::groups(), tr("Grupos de servidores..."), this,
                                         [this] {
                                             ServerTab* t = currentTab();
                                             if (!t) return;
                                             if (t->isNetworked()) {
                                                 ServerGroupsDialog dlg(t->net(), &t->data(), this);
                                                 dlg.exec();
                                             } else {
                                                 GroupsDialog dlg(&t->data(), this);
                                                 dlg.exec();
                                             }
                                             t->tree()->rebuild();
                                             t->info()->refresh();
                                         });
    m_actMyPerms = mPerm->addAction(tr("Mostrar permissões do usuário..."), this,
                     [this] {
                         ServerTab* t = currentTab();
                         if (!t) return;
                         if (t->isNetworked()) {
                             const ServerData& d = t->data();
                             // The "group" field arrives as "<icon> <name>" lines
                             // (e.g. "rota.png ROTA"): the banner shows only the role
                             // NAMES, without file names.
                             PermissionsOverviewDialog dlg(
                                 t->net()->myPerms(),
                                 GroupIconCache::cleanRoleNames(
                                     d.users.value(d.selfId).serverGroups)
                                     .join(QStringLiteral(", ")), this);
                             dlg.exec();
                         } else {
                             GroupsDialog dlg(&t->data(), this);
                             dlg.exec();
                         }
                     });
    m_actBanList = mPerm->addAction(tr("Lista de banidos..."), this,
                     [this] {
                         ServerTab* t = currentTab();
                         if (!t || !t->isNetworked()) {
                             QMessageBox::information(this, tr("Lista de banidos"),
                                 tr("A lista de banidos está disponível apenas conectado "
                                    "a um Halla Server."));
                             return;
                         }
                         BanListDialog dlg(t->net(), this);
                         dlg.exec();
                     });
    m_actComplaints = mPerm->addAction(tr("Reclamações..."), this,
                     [this] {
                         ServerTab* t = currentTab();
                         if (!t || !t->isNetworked()) {
                             QMessageBox::information(this, tr("Reclamações"),
                                 tr("Reclamações estão disponíveis apenas conectado "
                                    "a um Halla Server."));
                             return;
                         }
                         ComplaintsDialog dlg(t->net(), this);
                         dlg.exec();
                     });
    mPerm->addSeparator();
    QMenu* mChanGroups = mPerm->addMenu(tr("Grupos de canais"));
    mChanGroups->addAction(tr("Operador de canal: quem cria o canal gerencia"))
        ->setEnabled(false);
    mChanGroups->addAction(tr("(membros temporários de canais seguem o grupo global)"))
        ->setEnabled(false);

    QMenu* mTools = menuBar()->addMenu(tr("Fer&ramentas"));
    mTools->addAction(HIcons::logPage(), tr("Registro do cliente"), this,
                      [this] { openLogDialog(); });
    mTools->addAction(HIcons::transfer(), tr("Transferência de arquivos..."), this,
                      [this] {
                          ServerTab* t = currentTab();
                          if (!t || !t->isNetworked()) {
                              QMessageBox::information(this, tr("Transferência de arquivos"),
                                  tr("Conecte-se a um Halla Server para compartilhar "
                                     "arquivos por canal."));
                              return;
                          }
                          FileTransferDialog dlg(t->net(), &t->data(), this);
                          dlg.exec();
                      });
    mTools->addAction(HIcons::contacts(), tr("Mensagens offline..."), this,
                      [this] {
                          if (ServerTab* t = currentTab()) t->openOfflineMessages();
                      });
    mTools->addSeparator();

    // ---- local recording (shared with the toolbar)
    m_actRecord = mTools->addAction(HIcons::record(false), tr("Iniciar gravação"), this,
                                    [this] {
                                        ServerTab* t = currentTab();
                                        if (!t) return;
                                        t->toggleRecording();
                                        updateConnectionUi();
                                    });
    m_actRecord->setCheckable(true);
    mTools->addSeparator();

    // ---- avatar
    QMenu* mAvatar = mTools->addMenu(tr("Avatar"));
    mAvatar->addAction(tr("Definir avatar..."), this, [this] {
        if (ServerTab* t = currentTab()) t->setAvatarInteractive();
    });
    mAvatar->addAction(tr("Remover avatar"), this, [this] {
        if (ServerTab* t = currentTab()) t->removeAvatar();
    });

    mTools->addAction(HIcons::identity(), tr("Identidades..."), this,
                      [this] { IdentityDialog dlg(this); dlg.exec(); });
    mTools->addAction(HIcons::contacts(), tr("Contatos..."), this,
                      [this] { ContactsDialog dlg(this); dlg.exec(); });

    // ---- whisper lists: the active list is applied automatically. There is no
    // second "enable" switch, because it created diverging state between the saved
    // list, the toolbar and the audio.
    mTools->addAction(tr("Listas de sussurro..."), this,
                      [this] {
                          ServerTab* t = currentTab();
                          WhisperDialog dlg(t ? &t->data() : nullptr, this);
                          connect(&dlg, &WhisperDialog::settingsSaved, this, [this] { applyHotkeys(); });
                          dlg.exec();
                          // The list only configures the targets. Whisper is only
                          // transmitted while its key is held down.
                      });
    mTools->addAction(tr("Diagnóstico de voz..."), this, [this] {
        ServerTab* tab = currentTab();
        if (!tab || !tab->voice()) return;
        QDialog dlg(this);
        dlg.setWindowTitle(tr("Diagnóstico de voz"));
        dlg.resize(460, 320);
        QVBoxLayout layout(&dlg);
        QLabel title(tr("Estado em tempo real do áudio, Opus e reprodução."), &dlg);
        title.setWordWrap(true);
        QLabel values(&dlg);
        values.setTextInteractionFlags(Qt::TextSelectableByMouse);
        QFont mono = values.font(); mono.setStyleHint(QFont::Monospace); values.setFont(mono);
        layout.addWidget(&title);
        layout.addWidget(&values, 1);
        QPushButton close(tr("Fechar"), &dlg);
        layout.addWidget(&close, 0, Qt::AlignRight);
        connect(&close, &QPushButton::clicked, &dlg, &QDialog::accept);
        QTimer timer(&dlg);
        connect(&timer, &QTimer::timeout, &dlg, [&values, tab] {
            if (!tab || !tab->voice()) return;
            const QJsonObject d = tab->voice()->diagnostics();
            const NetSession* n = tab->net();
            values.setText(QObject::tr("Motor: %1\nFalando: %2\nPTT: %3\nSussurro: %4\n"
                "Nível do microfone (RMS): %5 / 32767\n"
                "Opus enviados: %6 frames, %7 bytes\n"
                "Opus recebidos: %8 frames, %9 bytes\n"
                "Fila de reprodução: %10 frames\n"
                "Ping TCP: %11 ms")
                .arg(d["active"].toBool() ? QObject::tr("ativo") : QObject::tr("indisponível"))
                .arg(d["talking"].toBool() ? QObject::tr("sim") : QObject::tr("não"))
                .arg(d["ptt"].toBool() ? QObject::tr("pressionado") : QObject::tr("solto"))
                .arg(d["whisper"].toBool() ? QObject::tr("ativo") : QObject::tr("inativo"))
                .arg(d["inputRms"].toInt())
                .arg(d["opusSent"].toVariant().toLongLong()).arg(d["opusSentBytes"].toVariant().toLongLong())
                .arg(d["opusReceived"].toVariant().toLongLong()).arg(d["opusReceivedBytes"].toVariant().toLongLong())
                .arg(d["playbackQueue"].toInt()).arg(n ? n->pingMs() : -1)
                + QStringLiteral("\n")
                + QObject::tr("Proteção de crosstalk: %1 bloqueios, %2 revogações\n"
                              "Reaberturas por travamento do microfone: %3")
                      .arg(d["echoGuardBlocks"].toVariant().toLongLong())
                      .arg(d["echoGuardRevokes"].toVariant().toLongLong())
                      .arg(d["captureStallReopens"].toVariant().toLongLong()));
        });
        timer.start(400);
        dlg.exec();
    });
    mTools->addSeparator();
    m_actOptions = mTools->addAction(HIcons::optionsGear(), tr("Opções..."), this,
                      [this] {
                          OptionsDialog dlg(this, currentTab() ? &currentTab()->data() : nullptr);
                          connect(&dlg, &OptionsDialog::themeChanged, this,
                                  &MainWindow::applyTheme);
                          connect(&dlg, &OptionsDialog::designChanged, this, [this] {
                              // window transparency (Options > Appearance)
                              setWindowOpacity(S::num("design/opacity", 100) / 100.0);
                              for (int i = 0; i < m_tabs->count(); ++i)
                                  if (ServerTab* t = qobject_cast<ServerTab*>(m_tabs->widget(i)))
                                      t->applyDisplayOptions();
                          });
                          connect(&dlg, &OptionsDialog::hotkeysChanged, this,
                                  &MainWindow::applyHotkeys);
                          connect(&dlg, &OptionsDialog::whisperListsChanged, this,
                                  &MainWindow::applyHotkeys);
                          bool restartForLanguage = false;
                          connect(&dlg, &OptionsDialog::languageChanged, &dlg,
                                  [&dlg, &restartForLanguage] {
                                      // Close the dialog first. The main window will run the
                                      // normal confirmation/disconnect flow before opening the
                                      // new instance in the chosen language.
                                      restartForLanguage = true;
                                      dlg.accept();
                                  });
                          dlg.exec();
                          if (restartForLanguage) {
                              m_restartAfterClose = true;
                              close();
                          }
                      });
    m_actOptions->setShortcut(QKeySequence(QStringLiteral("Alt+P")));

    // Actions declared by addons. The API stays Qt-free: plugins register only
    // an id, text, shortcut and a C callback.
    m_pluginsMenu = menuBar()->addMenu(tr("Co&mplementos"));
    // The menu is also the entry point to the manager. It used to be disabled
    // when no plugin registered actions, looking purely decorative.
    m_pluginsMenu->addAction(HIcons::addons(), tr("Gerenciar complementos..."), this, [this] {
        OptionsDialog dlg(this, currentTab() ? &currentTab()->data() : nullptr);
        connect(&dlg, &OptionsDialog::hotkeysChanged, this, &MainWindow::applyHotkeys);
        dlg.selectPage(tr("Complementos"));
        dlg.exec();
    });
    m_pluginsMenu->addSeparator();
    PluginManager& plugins = PluginManager::instance();
    connect(&plugins, &PluginManager::pluginActionRegistered, this,
            [this](const QString& pluginId, const QString& actionId,
                   const QString& label, const QString& shortcut) {
        const QString key = pluginId + QLatin1Char('\n') + actionId;
        if (QAction* previous = m_pluginActions.take(key)) delete previous;
#ifdef Q_OS_WIN
        if (const int previousId = m_pluginHotkeyIds.take(key); previousId > 0) {
            UnregisterHotKey(HWND(winId()), previousId);
            m_pluginGlobalHotkeys.remove(previousId);
        }
#endif
        QAction* action = m_pluginsMenu->addAction(label);
        action->setToolTip(pluginId);
        bool globalRegistered = false;
#ifdef Q_OS_WIN
        if (!shortcut.isEmpty()) {
            UINT vk = 0, mods = 0;
            if (specToVkFromSpec(shortcut, vk, mods)) {
                const int hotkeyId = m_nextPluginHotkeyId++;
                if (RegisterHotKey(HWND(winId()), hotkeyId, mods | MOD_NOREPEAT, vk)) {
                    m_pluginHotkeyIds.insert(key, hotkeyId);
                    m_pluginGlobalHotkeys.insert(hotkeyId, qMakePair(pluginId, actionId));
                    globalRegistered = true;
                }
            }
        }
#endif
        if (!shortcut.isEmpty() && !globalRegistered)
            action->setShortcut(QKeySequence(shortcut));
        connect(action, &QAction::triggered, this, [pluginId, actionId] {
            PluginManager::instance().triggerUiAction(pluginId, actionId);
        });
        m_pluginActions.insert(key, action);
        m_pluginsMenu->setEnabled(true);
    });
    connect(&plugins, &PluginManager::pluginActionRemoved, this,
            [this](const QString& pluginId, const QString& actionId) {
        const QString key = pluginId + QLatin1Char('\n') + actionId;
        if (QAction* action = m_pluginActions.take(key)) delete action;
#ifdef Q_OS_WIN
        if (const int hotkeyId = m_pluginHotkeyIds.take(key); hotkeyId > 0) {
            UnregisterHotKey(HWND(winId()), hotkeyId);
            m_pluginGlobalHotkeys.remove(hotkeyId);
        }
#endif
        // "Manage addons..." stays available even when the last plugin removes its
        // dynamic action.
        m_pluginsMenu->setEnabled(true);
    });
    connect(&plugins, &PluginManager::pluginNotification, this,
            [this](const QString& title, const QString& message, int timeoutMs) {
        if (m_tray && m_tray->isVisible())
            m_tray->showMessage(title.isEmpty() ? tr("Complemento do Halla") : title,
                                message, QSystemTrayIcon::Information, timeoutMs);
        else
            statusBar()->showMessage(title.isEmpty() ? message
                                                     : QStringLiteral("%1 — %2").arg(title, message),
                                     timeoutMs);
    });
    plugins.announceUiActions();

    QMenu* mHelp = menuBar()->addMenu(tr("A&juda"));
    mHelp->addAction(HIcons::logPage(), tr("Registro do cliente"), this,
                     [this] { openLogDialog(); });
    mHelp->addAction(tr("Sobre o Halla"), this,
                     [this] { AboutDialog dlg(this); dlg.exec(); });
    mHelp->addSeparator();
    mHelp->addAction(tr("Verificar atualizações"), this, [this] { checkUpdates(true); });

    // ------------------------- toolbar -----------------------------------
    // The reference uses two compact groups of rounded buttons instead of the old
    // bar full of controls. The QActions remain wired to the same flows so that no
    // client feature changes.
    QToolBar* tb = addToolBar(tr("Principal"));
    tb->setObjectName(QStringLiteral("mainToolBar"));
    tb->setMovable(false);
    tb->setIconSize(QSize(22, 22));
    tb->setToolButtonStyle(Qt::ToolButtonIconOnly);

    auto makeGroup = [tb]() {
        QFrame* group = new QFrame(tb);
        group->setObjectName(QStringLiteral("toolbarGroup"));
        QHBoxLayout* layout = new QHBoxLayout(group);
        layout->setContentsMargins(5, 4, 5, 4);
        layout->setSpacing(1);
        tb->addWidget(group);
        return layout;
    };
    auto addButton = [](QHBoxLayout* layout, QAction* action, QMenu* menu) {
        QToolButton* button = new QToolButton(layout->parentWidget());
        button->setObjectName(QStringLiteral("toolbarIconButton"));
        button->setDefaultAction(action);
        button->setToolButtonStyle(Qt::ToolButtonIconOnly);
        button->setIconSize(QSize(22, 22));
        button->setAutoRaise(true);
        if (menu) {
            button->setPopupMode(QToolButton::MenuButtonPopup);
            button->setMenu(menu);
        }
        layout->addWidget(button);
        return button;
    };

    QMenu* disMenu = new QMenu(this);
    disMenu->addAction(m_actDisconnectAll);
    QHBoxLayout* audioGroup = makeGroup();
    addButton(audioGroup, m_actDisconnect, disMenu);
    addButton(audioGroup, m_actConnect, nullptr);
    addButton(audioGroup, m_actMuteMic, nullptr);

    QMenu* spkMenu = new QMenu(this);
    spkMenu->addAction(tr("Opções de reprodução..."), this, [this] {
        OptionsDialog dlg(this, currentTab() ? &currentTab()->data() : nullptr);
        connect(&dlg, &OptionsDialog::whisperListsChanged, this, &MainWindow::applyHotkeys);
        connect(&dlg, &OptionsDialog::hotkeysChanged, this, &MainWindow::applyHotkeys);
        dlg.selectPage(tr("Reprodução"));
        dlg.exec();
    });
    addButton(audioGroup, m_actMuteSpk, spkMenu);
    addButton(audioGroup, m_actScreenShare, nullptr);

    tb->addSeparator();

    QHBoxLayout* utilityGroup = makeGroup();
    addButton(utilityGroup, m_actOptions, nullptr);

    // The notification indicator sits isolated on the right, as in the reference.
    QWidget* toolbarSpacer = new QWidget(tb);
    toolbarSpacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    tb->addWidget(toolbarSpacer);
    QToolButton* notifications = new QToolButton(tb);
    notifications->setObjectName(QStringLiteral("toolbarIconButton"));
    notifications->setIcon(HIcons::bell());
    notifications->setIconSize(QSize(18, 18));
    notifications->setToolTip(tr("Notificações"));
    notifications->setAutoRaise(true);
    connect(notifications, &QToolButton::clicked, this, &MainWindow::showNotifications);
    tb->addWidget(notifications);

    // ------------------------- central area ------------------------------
    m_stack = new QStackedWidget(this);

    m_welcome = new WelcomePage(m_stack);
    m_stack->addWidget(m_welcome);
    connect(m_welcome, &WelcomePage::connectRequested, this,
            [this] { openConnectDialog(false); });
    connect(m_welcome, &WelcomePage::bookmarksRequested, this,
            [this] { openBookmarksDialog(); });

    m_center = new QSplitter(Qt::Horizontal, m_stack);
    m_center->setObjectName(QStringLiteral("mainSurface"));
    m_center->setChildrenCollapsible(false);

    m_tabs = new QTabWidget(m_center);
    m_tabs->setObjectName(QStringLiteral("serverTabs"));
    m_tabs->setTabsClosable(true);
    m_tabs->setMovable(true);
    m_tabs->setDocumentMode(true);
    // The visual tab sits inside the tree card, as in the reference image; the
    // QTabBar still exists so connections can be switched programmatically without
    // adding an extra strip to the layout.
    m_tabs->tabBar()->hide();
    m_center->addWidget(m_tabs);
    m_center->setStretchFactor(0, 1);

    m_stack->addWidget(m_center);

    setCentralWidget(m_stack);
    m_stack->setCurrentWidget(m_welcome);

    connect(m_tabs, &QTabWidget::tabCloseRequested, this, [this](int idx) {
        if (ServerTab* t = qobject_cast<ServerTab*>(m_tabs->widget(idx)))
            disconnectTab(t);
    });
    connect(m_tabs, &QTabWidget::currentChanged, this, [this](int) {
        updateConnectionUi();
        updateStatusBar();
        rebuildServerButtons();
        publishPluginState();
        // v1.1.28: the WebRTC session is unique and its m_net must follow the ACTIVE
        // tab. It used to stay bound to the last CONNECTED tab: with two servers open,
        // asking to watch from one tab sent the webrtc_watch_request to the other
        // tab's server -- nothing happened and the window stayed on "Waiting for
        // broadcast..." forever.
        if (m_webrtcSession)
            if (ServerTab* t = currentTab())
                if (t->net()) m_webrtcSession->setNetSession(t->net());
    });

    // context menu on the tabs (same behaviour as the rest of Halla)
    m_tabs->tabBar()->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_tabs->tabBar(), &QTabBar::customContextMenuRequested, this,
            [this](const QPoint& pos) {
                int idx = m_tabs->tabBar()->tabAt(pos);
                if (idx < 0) return;
                ServerTab* t = qobject_cast<ServerTab*>(m_tabs->widget(idx));
                if (!t) return;
                QMenu menu(this);
                menu.addAction(HIcons::disconnectPlug(), tr("Desconectar"), this,
                               [this, t] { disconnectTab(t); });
                menu.addSeparator();
                menu.addAction(HIcons::editPencil(), tr("Editar servidor virtual"), this,
                               [this, t] { t->editVirtualServerName(); });
                menu.addSeparator();
                menu.addAction(HIcons::bookmarkStar(), tr("Adicionar aos favoritos"), this,
                               [this, t] {
                                   openBookmarksDialog(t->data().name, t->data().address);
                               });
                menu.exec(m_tabs->tabBar()->mapToGlobal(pos));
            });

    // ------------------------- status bar --------------------------------
    // three zones, as in Halla:
    // [current server tab]  |  [news line]  |  [icon + connection + ping]
    // Horizontal row of connected servers (one button per server).
    m_serverBar = new QWidget(this);
    m_serverBar->setObjectName(QStringLiteral("serverBar"));
    m_serverBarLayout = new QHBoxLayout(m_serverBar);
    m_serverBarLayout->setContentsMargins(0, 0, 0, 0);
    m_serverBarLayout->setSpacing(4);
    statusBar()->addWidget(m_serverBar, 0);
    rebuildServerButtons();

    m_newsLabel = new QLabel(
        tr("Bem-vindo ao Halla!  •  Cliente de comunicação de voz  •  "
           "github.com/GroupHalla/Halla"), this);
    m_newsLabel->setObjectName(QStringLiteral("newsLabel"));
    m_newsLabel->setAlignment(Qt::AlignCenter);
    m_newsLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    statusBar()->addWidget(m_newsLabel, 1);

    m_statusIcon = new QLabel(this);
    m_statusIcon->setPixmap(HIcons::disconnectPlug().pixmap(14, 14));
    statusBar()->addPermanentWidget(m_statusIcon, 0);
    m_statusText = new QLabel(tr("Desconectado"), this);
    statusBar()->addPermanentWidget(m_statusText, 0);
    // v6 E2EE: padlock -- lit when the current server's group keys are
    // available; "waiting" while the master wraps them.
    m_e2eeLabel = new QLabel(this);
    m_e2eeLabel->setToolTip(tr("Criptografia de ponta a ponta"));
    statusBar()->addPermanentWidget(m_e2eeLabel, 0);
    m_pingLabel = new QLabel(QString(), this);
    statusBar()->addPermanentWidget(m_pingLabel, 0);

    QTimer* pingTimer = new QTimer(this);
    pingTimer->setInterval(3000);
    connect(pingTimer, &QTimer::timeout, this, &MainWindow::updateStatusBar);
    pingTimer->start();

    // ------------------------- bandeja do sistema ------------------------
    if (QSystemTrayIcon::isSystemTrayAvailable()) {
        m_tray = new QSystemTrayIcon(QIcon(HIcons::appIcon(64)), this);
        QMenu* trayMenu = new QMenu(this);
        QAction* show = trayMenu->addAction(tr("Mostrar Halla"), this, [this] {
            showFromTray();
        });
        Q_UNUSED(show);
        trayMenu->addSeparator();
        trayMenu->addAction(tr("Sair"), this, [this] {
            S::set("app/forceQuit", true);
            close();
        });
        m_tray->setContextMenu(trayMenu);
        m_tray->setToolTip(QStringLiteral("Halla"));
        m_tray->show();
        connect(m_tray, &QSystemTrayIcon::activated, this,
                [this](QSystemTrayIcon::ActivationReason reason) {
                    if (reason == QSystemTrayIcon::DoubleClick) {
                        showFromTray();
                    }
                });
    }

    // restore the previous session
    if (S::flag("app/restoreTabs", false)) {
        QJsonDocument doc = QJsonDocument::fromJson(S::str("session/tabs").toUtf8());
        if (doc.isArray()) {
            for (const QJsonValue& v : doc.array()) {
                QJsonObject o = v.toObject();
                connectTo(o["addr"].toString(), quint16(o["port"].toInt(9987)),
                          o["nick"].toString());
            }
        }
    }

    rebuildBookmarksMenu();
    rebuildRecentMenu();
    updateConnectionUi();
    updateStatusBar();
    applyTheme();
    applyHotkeys();
    // Window transparency (Options > Appearance)
    setWindowOpacity(qBound(0.5, S::num("design/opacity", 100) / 100.0, 1.0));

    AppLog::info(tr("Halla %1 iniciado").arg(QString::fromUtf8(halla::kAppVersion)));

    // Silently check for updates 3 seconds after startup
    QTimer::singleShot(3000, this, [this]{ checkUpdates(false); });
}

MainWindow::~MainWindow() {
#ifdef Q_OS_WIN
    for (int hotkeyId : m_pluginGlobalHotkeys.keys())
        UnregisterHotKey(HWND(winId()), hotkeyId);
    m_pluginGlobalHotkeys.clear();
    m_pluginHotkeyIds.clear();
#endif
    // Only spawn the new process after this window, its connections and the
    // current loop have finished. This prevents two visible instances during the
    // exit confirmation or the disconnect sound.
    if (m_restartAfterClose) {
        QProcess::startDetached(QCoreApplication::applicationFilePath(),
                                QCoreApplication::arguments().mid(1));
    }
}

// ======================================================================
void MainWindow::connectTo(const QString& address, quint16 port, const QString& nickname,
                           const QString& password) {
    // Joining without a name is not allowed: the official server comes pre-saved
    // in the bookmarks with an empty nickname precisely so the app asks here.
    QString nick = nickname.trimmed();
    if (nick.isEmpty()) {
        bool ok = false;
        const QString remembered = S::ServerNicks::get(address, port,
            S::str(QStringLiteral("connect/nickname"), QString()));
        nick = QInputDialog::getText(this, tr("Escolha um apelido"),
            tr("Digite o apelido com que você vai entrar em %1:%2.")
                .arg(address).arg(port),
            QLineEdit::Normal, remembered, &ok).trimmed();
        if (!ok || nick.isEmpty()) return;
    }
    // Unique id of the DEFAULT identity (stable -- generated and persisted once)
    QString uid;
    for (const QStringList& r : IdentityDialog::loadAll())
        if (r.value(0) == "1") { uid = r.value(3); break; }
    if (uid.isEmpty()) uid = IdentityDialog::loadAll().value(0).value(3);

    // The identity must be complete BEFORE connecting: without the public key the
    // server would reject the login with bad_identity, without saying why.
    if (uid.isEmpty() || IdentityDialog::publicKeyForUid(uid).isEmpty()) {
        QMessageBox::warning(
            this, tr("Erro ao conectar"),
            tr("Sua identidade não está disponível neste computador: o ID único está "
               "vazio ou a chave pública não foi encontrada.\n\nAbra a janela "
               "Identidades, crie uma nova identidade (ou restaure seu backup) e "
               "conecte de novo."));
        return;
    }

    NetSession* net = new NetSession(this);
    net->connectToServer(address, port, nick, uid, password);

    // connection failure / login refused
    connect(net, &NetSession::connectionFailed, this, [this, net, address, port](const QString& reason) {
        HSound::play(QStringLiteral("error"));
        QMessageBox::warning(this, tr("Erro ao conectar"),
                             tr("<b>Falha ao conectar ao servidor %1:%2</b><br>%3")
                                 .arg(address).arg(port).arg(reason.toHtmlEscaped()));
        net->deleteLater();
    });

    // apelido recusado (name_in_use/bad_nick): pede outro nome e reconecta
    connect(net, &NetSession::nickRejected, this,
            [this, net, address, port, password](const QString& message) {
        bool ok = false;
        const QString remembered = S::ServerNicks::get(address, port,
            S::str(QStringLiteral("connect/nickname"), QString()));
        const QString nick = QInputDialog::getText(this, tr("Apelido em uso"),
            tr("%1\nEscolha outro apelido para entrar em %2:%3.")
                .arg(message, address).arg(port),
            QLineEdit::Normal, remembered, &ok).trimmed();
        net->deleteLater();
        if (!ok || nick.isEmpty()) return;
        connectTo(address, port, nick, password);
    });

    // login accepted: creates the server tab
    connect(net, &NetSession::welcomeReceived, this, [this, net, address, port] {
        ServerTab* tab = new ServerTab(net->data(), m_tabs);
        ServerData& d = tab->data();
        // copies everything NetSession already downloaded in the welcome
        d = net->data();
        tab->attachNetwork(net);

        const int idx = m_tabs->addTab(tab, HIcons::server(), d.name);
        m_tabs->setCurrentIndex(idx);
        m_tabs->setTabToolTip(idx, tr("Servidor: %1").arg(d.address));
        wireTab(tab);

        tab->chat()->addServerSystem(tr("Conectado ao servidor: %1").arg(d.address));

        // ping real -> barra de status
        connect(net, &NetSession::pingUpdated, this,
                [this, net](int) { updateStatusBar(); });
        // v6 E2EE: the padlock in the bar follows key arrival/rotation; security
        // warnings (identity changed, forged channel_key) are always visible --
        // never only in the log.
        connect(net, &NetSession::e2eeStateChanged, this, [this] { updateStatusBar(); });
        connect(net, &NetSession::e2eeSecurityNotice, this, [this](const QString& text) {
            QMessageBox::warning(this, tr("Segurança da criptografia"), text);
        });
        connect(net, &NetSession::disconnectedUnexpected, this, [this, tab, net] {
            if (!net->serverTerminatedSession())
                HSound::play(QStringLiteral("connection_lost"));
            tab->chat()->addServerSystem(tr("Desconectado do servidor."));
            disconnectTab(tab, false);
        });

        if (S::flag("notify/connectSound", true)) HSound::play(QStringLiteral("connected"));
        HSpeech::say(tr("Conectado ao servidor"));

        S::set("connect/nickname", net->data().users[net->data().selfId].name);
        // remembers the nickname accepted per server: reconnecting no longer loses it
        S::ServerNicks::set(address, port, net->data().users[net->data().selfId].name);
        addRecent(address, port);
        saveSession();

        m_stack->setCurrentWidget(m_center);
        updateConnectionUi();
        updateStatusBar();
    });
}

// common wiring for a tab (local or network)
void MainWindow::wireTab(ServerTab* tab) {
    PluginManager::instance().registerSession(tab);
    connect(tab, &ServerTab::disconnectRequested, this, [this, tab] { disconnectTab(tab); });
    connect(tab, &ServerTab::addBookmarkRequested, this, [this, tab] {
        openBookmarksDialog(tab->data().name, tab->data().address);
    });
    connect(tab, &ServerTab::titleChanged, this, [this, tab] {
        int i = m_tabs->indexOf(tab);
        if (i >= 0) m_tabs->setTabText(i, tab->tabTitle());
    });
    connect(tab, &ServerTab::statusChanged, this, [this, tab] {
        if (m_tabs->currentWidget() == tab) {
            updateConnectionUi();
            updateStatusBar();
            publishPluginState();
        }
    });
    if (tab->net()) {
        // nickname renamed during the session: remember it for this server
        connect(tab->net(), &NetSession::selfRenamed, this, [this, tab](const QString& name) {
            if (!tab->net()) return;
            S::ServerNicks::set(tab->net()->hostPort(), 0, name);
            S::set("connect/nickname", name);
        });
        connect(tab->net(), &NetSession::screenshareStateChanged, this,
                &MainWindow::handleScreenshareStateChanged);
        connect(tab->net(), &NetSession::screenshareFrameReceived, this, &MainWindow::handleScreenshareFrameReceived);
        // v1.1.28: server errors related to "Watch" (wrong channel, broadcast
        // already finished, no permission, password-protected channel...) had a
        // single destination: one line in the server chat, easy to miss. They now
        // feed the broadcast window/dialog with real feedback.
        connect(tab->net(), &NetSession::errorOccurred, this,
                [this](const QString& code, const QString& msg) {
                    handleWatchServerError(code, msg);
                });
        if (!m_webrtcSession) {
            m_webrtcSession = new HallaWebRtcSession(tab->net(), this);
            connect(m_webrtcSession, &HallaWebRtcSession::unavailable, this,
                    [this](const QString& reason) { statusBar()->showMessage(reason, 7000); });
            connect(m_webrtcSession, &HallaWebRtcSession::localPreviewFrame, this,
                    [this](const QImage& image) {
                        if (ServerTab* tab = currentTab()) {
                            const int self = tab->data().selfId;
                            if (m_screenShareWindows.contains(self)) m_screenShareWindows[self]->updateImage(image);
                        }
                    });
            connect(m_webrtcSession, &HallaWebRtcSession::remoteFrameReceived, this,
                    [this](int userId, const QImage& image) {
                        markWatchAlive(userId);
                        if (m_screenShareWindows.contains(userId)) m_screenShareWindows[userId]->updateImage(image);
                    });
            // v1.1.28: peer ICE/connection state -- "failed" becomes a clear error
            // message in the viewer window (previously: eternal silence on "Waiting
            // for broadcast..." when the P2P link could not traverse the pair's
            // NAT/CGNAT without TURN).
            connect(m_webrtcSession, &HallaWebRtcSession::peerStateChanged, this,
                    [this](int peerId, const QString& state) {
                        if (state != QLatin1String("failed")) return;
                        cancelWatchdog(peerId);
                        ScreenShareWindow* window = m_screenShareWindows.value(peerId, nullptr);
                        if (window && window->isVisible()) {
                            window->setStatusMessage(
                                tr("Não foi possível conectar à transmissão.\n"
                                   "Verifique sua conexão/firewall — se o problema persistir, "
                                   "o servidor pode precisar de um servidor TURN configurado."),
                                true,
                                [this, peerId] {
                                    if (m_webrtcSession) m_webrtcSession->restartWatch(peerId);
                                    armWatchdog(peerId);
                                });
                        }
                    });
            connect(m_webrtcSession, &HallaWebRtcSession::remoteAudioReceived, this,
                    [this](int userId, const QByteArray& pcm, int sampleRate,
                           int channels, int frames) {
                        ServerTab* active = currentTab();
                        if (!active || !active->voice() || sampleRate != 48000
                                || (channels != 1 && channels != 2)
                                || frames <= 0 || pcm.size() != frames * channels * int(sizeof(int16_t)))
                            return;
                        ScreenShareWindow* window = m_screenShareWindows.value(userId, nullptr);
                        if (!window || window->isAudioMuted()) return;
                        // Each live stream keeps its own queue: this allows muting one
                        // broadcast without affecting voices, plugins or other streams.
                        active->voice()->playStreamPcm(
                            userId, reinterpret_cast<const int16_t*>(pcm.constData()),
                            uint32_t(frames), uint32_t(channels), 1.0f);
                    });
        }
        // The WebRTC session is unique and survives tabs: on (re)connect, point it
        // at this tab's live NetSession. Without this, m_net stayed bound to the old
        // (deleted) session after reconnecting, and broadcasting crashed the app
        // (use-after-free) or did nothing (stuck state).
        m_webrtcSession->setNetSession(tab->net());
        connect(tab->net(), &NetSession::webRtcSignalReceived,
                m_webrtcSession, &HallaWebRtcSession::handleSignal);
    }
    connect(tab->tree(), &ServerTreeWidget::screenshareHovered, this, &MainWindow::handleScreenshareHovered);
}

// local offline tab (--demo mode for screenshots)
void MainWindow::createLocalTab(const ServerData& initial) {
    ServerTab* tab = new ServerTab(initial, m_tabs);
    const int idx = m_tabs->addTab(tab, HIcons::server(), initial.name);
    m_tabs->setCurrentIndex(idx);
    wireTab(tab);
    tab->chat()->addServerSystem(tr("Conectado ao servidor: %1").arg(initial.address));
    tab->chat()->addChannelSystem(tr("Você entrou no canal \"%1\".")
                                      .arg(initial.channels.first().name));
    m_stack->setCurrentWidget(m_center);
    updateConnectionUi();
    updateStatusBar();
}

void MainWindow::disconnectTab(ServerTab* tab, bool notify) {
    if (!tab || m_tabs->indexOf(tab) < 0 || m_disconnectingTabs.contains(tab)) return;
    m_disconnectingTabs.insert(tab);

    // The notice starts first; the connection is only closed after a second so the
    // audio is not cut together with the tab/playback device.
    if (notify && S::flag("notify/disconnectSound", true))
        HSound::play(QStringLiteral("disconnected"));
    if (notify) HSpeech::say(tr("Desconectado do servidor"));

    QPointer<ServerTab> safeTab(tab);
    const int delayMs = notify ? 1000 : 0;
    QTimer::singleShot(delayMs, this, [this, safeTab] {
        if (safeTab) finishDisconnectTab(safeTab.data());
    });
}

void MainWindow::disconnectAllTabs(bool notify) {
    QList<QPointer<ServerTab>> tabs;
    for (int i = 0; i < m_tabs->count(); ++i) {
        if (ServerTab* tab = qobject_cast<ServerTab*>(m_tabs->widget(i))) {
            if (!m_disconnectingTabs.contains(tab)) {
                m_disconnectingTabs.insert(tab);
                tabs << QPointer<ServerTab>(tab);
            }
        }
    }
    if (tabs.isEmpty()) return;

    if (notify && S::flag("notify/disconnectSound", true))
        HSound::play(QStringLiteral("disconnected"));
    if (notify) HSpeech::say(tr("Desconectado do servidor"));

    QTimer::singleShot(notify ? 1000 : 0, this, [this, tabs] {
        for (const QPointer<ServerTab>& tab : tabs)
            if (tab) finishDisconnectTab(tab.data());
    });
}

void MainWindow::finishDisconnectTab(ServerTab* tab) {
    if (!tab) return;
    const int idx = m_tabs->indexOf(tab);
    if (idx < 0) {
        m_disconnectingTabs.remove(tab);
        return;
    }

    if (m_screenShareTimer) m_screenShareTimer->stop();
    m_actScreenShare->setChecked(false);
    m_actScreenShare->setIcon(HIcons::screenShare(false));
    qDeleteAll(m_screenShareWindows);
    m_screenShareWindows.clear();

    NetSession* net = tab->net();
    if (net) {
        // Ends the WebRTC session's broadcast/watch WHILE NetSession still exists
        // (stream_stop needs it) and releases the pointer before deleteLater -- avoids
        // use-after-free and a broadcast stuck in the "on" state after reconnecting.
        if (m_webrtcSession) m_webrtcSession->detachFromNet(net);
        net->quit();
        QTimer::singleShot(500, net, &QObject::deleteLater);
    }
    const QString addr = tab->data().address;
    PluginManager::instance().unregisterSession(tab);
    m_tabs->removeTab(idx);
    m_disconnectingTabs.remove(tab);
    tab->deleteLater();

    AppLog::info(tr("Desconectado de %1").arg(addr));
    saveSession();
    if (m_tabs->count() == 0) m_stack->setCurrentWidget(m_welcome);
    updateConnectionUi();
    updateStatusBar();
    publishPluginState();
}

void MainWindow::publishPluginState() {
    PluginManager::instance().setActiveSession(currentTab());
}

// Opens (or reopens) the "Client log" -- the dialog loads the halla.log history, so
// it also shows past decisions (e.g. GPU/CPU encoder).
void MainWindow::openLogDialog() {
    if (!m_log) m_log = new LogDialog(this);
    m_log->show();
    m_log->raise();
    m_log->activateWindow();
}

ServerTab* MainWindow::currentTab() const {
    return qobject_cast<ServerTab*>(m_tabs->currentWidget());
}

// ======================================================================
void MainWindow::openConnectDialog(bool newTab) {
    Q_UNUSED(newTab); // each connection already opens in its own tab, as in Halla
    ConnectDialog dlg(this);
    dlg.setNickname(S::str("connect/nickname", IdentityDialog::defaultNickname()));
    if (dlg.exec() != QDialog::Accepted) return;
    connectTo(dlg.address(), dlg.port(), dlg.nickname(), dlg.password());
}

void MainWindow::openBookmarksDialog(const QString& prefillLabel, const QString& prefillAddr) {
    BookmarksDialog dlg(this);
    connect(&dlg, &BookmarksDialog::connectRequested, this,
            [this](const QString& a, quint16 p, const QString& n, const QString& pw) {
                // Empty nickname in the bookmark + nothing remembered -> connectTo asks
                // for the name (it does not invent a default).
                connectTo(a, p,
                          !n.trimmed().isEmpty()
                              ? n
                              : S::ServerNicks::get(a, p, QString()),
                          pw);
            });
    connect(&dlg, &BookmarksDialog::changed, this, [this] { rebuildBookmarksMenu(); });
    if (!prefillAddr.isEmpty())
        dlg.prefill(prefillLabel.isEmpty() ? prefillAddr : prefillLabel, prefillAddr, 9987,
                    S::ServerNicks::get(prefillAddr, 9987, QString()));
    dlg.exec();
    rebuildBookmarksMenu();
}

// ======================================================================
void MainWindow::rebuildBookmarksMenu() {
    // remove old dynamic entries (everything after "Manage bookmarks...")
    const QList<QAction*> acts = m_bookmarksMenu->actions();
    for (int i = 2; i < acts.size(); ++i) m_bookmarksMenu->removeAction(acts[i]);

    const QList<Bookmark> list = BookmarksDialog::loadAll();
    if (list.isEmpty()) {
        QAction* none = m_bookmarksMenu->addAction(tr("(nenhum favorito)"));
        none->setEnabled(false);
        return;
    }

    m_bookmarksMenu->addSeparator();
    for (const Bookmark& b : list) {
        QString text = b.label.isEmpty() ? b.address : b.label;
        m_bookmarksMenu->addAction(HIcons::bookmarkStar(), text, this,
                                   [this, b] {
                                       // bookmark with its own nickname wins; without one,
                                       // the server memory is used -- and empty makes
                                       // connectTo ask for the name right away
                                       const QString nick = !b.nickname.trimmed().isEmpty()
                                           ? b.nickname
                                           : S::ServerNicks::get(b.address, b.port, QString());
                                       connectTo(b.address, b.port, nick, b.password);
                                   });
    }
    m_bookmarksMenu->addSeparator();
    m_bookmarksMenu->addAction(tr("Conectar a todos os favoritos"), this, [this] {
        for (const Bookmark& b : BookmarksDialog::loadAll())
            if (!b.address.trimmed().isEmpty())
                connectTo(b.address, b.port,
                          !b.nickname.trimmed().isEmpty()
                              ? b.nickname
                              : S::ServerNicks::get(b.address, b.port, QString()),
                          b.password);
    });
}

void MainWindow::addRecent(const QString& address, quint16 port) {
    QJsonArray arr;
    QJsonDocument doc = QJsonDocument::fromJson(S::str("connect/recent").toUtf8());
    if (doc.isArray()) arr = doc.array();

    QJsonObject o;
    o["addr"] = address;
    o["port"] = static_cast<int>(port);
    const QString key = QJsonDocument(o).toJson(QJsonDocument::Compact);
    for (int i = arr.size() - 1; i >= 0; --i)
        if (QJsonDocument(arr[i].toObject()).toJson(QJsonDocument::Compact) == key)
            arr.removeAt(i);
    arr.prepend(o);
    while (arr.size() > 10) arr.removeLast();
    S::set("connect/recent",
           QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact)));
    rebuildRecentMenu();
}

void MainWindow::rebuildRecentMenu() {
    m_recentMenu->clear();
    QJsonDocument doc = QJsonDocument::fromJson(S::str("connect/recent").toUtf8());
    bool any = false;
    if (doc.isArray()) {
        for (const QJsonValue& v : doc.array()) {
            QJsonObject o = v.toObject();
            any = true;
            m_recentMenu->addAction(o["addr"].toString(), this,
                                    [this, o] {
                                        const QString addr = o["addr"].toString();
                                        const quint16 port = quint16(o["port"].toInt(9987));
                                        // last nickname used on this server; without memory, the global one
                                        connectTo(addr, port,
                                                  S::ServerNicks::get(addr, port,
                                                      S::str("connect/nickname",
                                                             IdentityDialog::defaultNickname())));
                                    });
        }
    }
    if (!any) {
        QAction* none = m_recentMenu->addAction(tr("(vazio)"));
        none->setEnabled(false);
    }
}

// Updates, notifications and session persistence live in MainWindowUpdates.cpp.


void MainWindow::updateConnectionUi() {
    ServerTab* t = currentTab();
    const bool connected = (t != nullptr);

    m_actDisconnect->setEnabled(connected);
    m_actDisconnectAll->setEnabled(m_tabs->count() > 0);
    m_actBookmarkAdd->setEnabled(connected);
    m_actPrivilegeKey->setEnabled(connected);
    m_actServerGroups->setEnabled(connected);
    m_actAway->setEnabled(connected);
    m_actMuteMic->setEnabled(connected);
    m_actMuteSpk->setEnabled(connected);
    m_actRecord->setEnabled(connected);
    m_actRenameSelf->setEnabled(connected);
    m_actCommander->setEnabled(connected);
    m_whisperToggleOn = (t && t->whisperHoldActive());

    if (connected) {
        m_actAway->blockSignals(true);
        m_actMuteMic->blockSignals(true);
        m_actMuteSpk->blockSignals(true);
        m_actRecord->blockSignals(true);
        m_actAway->setChecked(t->isAway());
        m_actMuteMic->setChecked(t->isMicMuted());
        m_actMuteSpk->setChecked(t->isSpkMuted());
        m_actRecord->setChecked(t->isRecording());
        m_actRecord->setText(t->isRecording() ? tr("Parar gravação")
                                              : tr("Iniciar gravação"));
        m_actAway->blockSignals(false);
        m_actMuteMic->blockSignals(false);
        m_actMuteSpk->blockSignals(false);
        m_actRecord->blockSignals(false);
        m_actAway->setIcon(HIcons::away(t->isAway()));
        m_actMuteMic->setIcon(HIcons::muteMic(t->isMicMuted()));
        m_actMuteSpk->setIcon(HIcons::muteSpeaker(t->isSpkMuted()));
        m_actRecord->setIcon(HIcons::record(t->isRecording()));
    }
}

void MainWindow::updateStatusBar() {
    ServerTab* t = currentTab();
    // left zone: row with one button per connected server (rebuilt below)
    rebuildServerButtons();

    // right zone: connection state + ping/loss
    if (t) {
        m_statusIcon->setPixmap(HIcons::connectPlug().pixmap(14, 14));
        m_statusText->setText(tr("Conectado como %1")
                                  .arg(t->data().users[t->data().selfId].name));
        if (NetSession* net = t->net()) {
            m_pingLabel->setText(tr("Ping: %1 ms   Perda de pacotes: %2%")
                                     .arg(net->pingMs()).arg(QStringLiteral("0,00")));
            // v6 E2EE: current session's group key state
            m_e2eeLabel->setText(net->e2eeKeysReady()
                                     ? QStringLiteral("\U0001F512 E2EE")
                                     : QStringLiteral("\U0001F512 …"));
            m_e2eeLabel->setToolTip(net->e2eeKeysReady()
                ? tr("Criptografia de ponta a ponta ativa: chaves geradas no seu "
                     "cliente, o servidor não consegue ler voz, chat, sussurros ou "
                     "tela.")
                : tr("Chaves de criptografia em negociação… (chegam em instantes; "
                     "nada sai em claro enquanto isso)"));
        } else {
            m_pingLabel->setText(tr("Ping: --"));
            m_e2eeLabel->clear();
        }
    } else if (m_tabs->count() > 0) {
        m_statusIcon->setPixmap(HIcons::connectPlug().pixmap(14, 14));
        m_statusText->setText(tr("%1 conexões abertas").arg(m_tabs->count()));
        m_pingLabel->clear();
        m_e2eeLabel->clear();
    } else {
        m_statusIcon->setPixmap(HIcons::disconnectPlug().pixmap(14, 14));
        m_statusText->setText(tr("Desconectado"));
        m_pingLabel->clear();
        m_e2eeLabel->clear();
    }
}

// Rebuilds the horizontal row of connected servers in the status bar.
// Each connected server becomes a clickable button (name next to the others) and
// the currently selected server is highlighted (bold + background color).
void MainWindow::rebuildServerButtons() {
    if (!m_serverBar || !m_serverBarLayout) return;

    // Removes every previous button.
    while (QLayoutItem* item = m_serverBarLayout->takeAt(0)) {
        if (QWidget* w = item->widget()) { w->deleteLater(); }
        delete item;
    }

    const int current = m_tabs ? m_tabs->currentIndex() : -1;

    if (m_tabs && m_tabs->count() > 0) {
        for (int i = 0; i < m_tabs->count(); ++i) {
            ServerTab* tab = qobject_cast<ServerTab*>(m_tabs->widget(i));
            if (!tab) continue;
            const QString name = tab->data().name;
            const bool selected = (i == current);

            QToolButton* button = new QToolButton(m_serverBar);
            button->setObjectName(QStringLiteral("serverTabButton"));
            button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
            button->setIcon(HIcons::server());
            button->setText(name);
            button->setToolTip(tab->data().address);
            button->setCheckable(true);
            button->setChecked(selected);
            button->setAutoRaise(true);
            // Highlight style of the selected server (bold + background).
            QString css = selected
                ? QStringLiteral(
                      "QToolButton#serverTabButton { font-weight: bold; "
                      "background: palette(highlight); color: palette(highlighted-text); "
                      "border-radius: 6px; padding: 2px 8px; }")
                : QStringLiteral(
                      "QToolButton#serverTabButton { border-radius: 6px; padding: 2px 8px; "
                      "background: transparent; }"
                      "QToolButton#serverTabButton:hover { background: palette(alternate-base); }");
            button->setStyleSheet(css);

            // Clicar seleciona a aba daquele servidor (mostrando seus canais/chat).
            connect(button, &QToolButton::clicked, this, [this, i] {
                if (m_tabs && i >= 0 && i < m_tabs->count()) {
                    m_tabs->setCurrentIndex(i);
                    updateConnectionUi();
                    updateStatusBar();
                    publishPluginState();
                }
            });
            // Right click on the status-bar server: same menu as the tab (Disconnect /
            // Edit virtual server / Bookmarks) -- captures the TAB (stable), not the
            // index (which changes as tabs close).
            button->setContextMenuPolicy(Qt::CustomContextMenu);
            connect(button, &QToolButton::customContextMenuRequested, this,
                    [this, tab, button](const QPoint& p) {
                        QMenu menu(this);
                        menu.addAction(HIcons::disconnectPlug(), tr("Desconectar"), this,
                                       [this, tab] { disconnectTab(tab); });
                        menu.addSeparator();
                        menu.addAction(HIcons::editPencil(), tr("Editar servidor virtual"), this,
                                       [this, tab] { tab->editVirtualServerName(); });
                        menu.addSeparator();
                        menu.addAction(HIcons::bookmarkStar(), tr("Adicionar aos favoritos"), this,
                                       [this, tab] {
                                           openBookmarksDialog(tab->data().name, tab->data().address);
                                       });
                        menu.exec(button->mapToGlobal(p));
                    });
            m_serverBarLayout->addWidget(button);
        }
    } else {
        // No server connected: show only the neutral label.
        QToolButton* button = new QToolButton(m_serverBar);
        button->setObjectName(QStringLiteral("serverTabButton"));
        button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        button->setIcon(HIcons::server());
        button->setText(tr("Nenhum servidor"));
        button->setEnabled(false);
        button->setStyleSheet(QStringLiteral(
            "QToolButton#serverTabButton { border-radius: 6px; padding: 2px 8px; background: transparent; }"));
        m_serverBarLayout->addWidget(button);
    }
}

// ======================================================================
void MainWindow::applyTheme() {
    // centralized theme: style + palette + global stylesheet (works on Windows
    // too, where a fixed stylesheet ignored the palette)
    HTheme::apply();
    // forces a repaint of the manually painted widgets and of the banners
    if (m_welcome) m_welcome->update();
    for (int i = 0; i < m_tabs->count(); ++i) {
        if (ServerTab* t = qobject_cast<ServerTab*>(m_tabs->widget(i))) {
            t->info()->refresh();
            t->tree()->viewport()->update();
            t->chat()->update();
        }
    }
}

// runs an action configured under "Hotkeys" (regardless of origin: a local Qt
// shortcut on Linux or a system-wide GLOBAL hotkey on Windows)
void MainWindow::runConfiguredAction(const QString& action) {
    if (isWhisperHotkeyAction(action)) {
        // whisper by TOGGLE (used on Linux/window shortcuts); on Windows the main
        // behavior is "hold to talk" (see applyHotkeys). Detection by
        // isWhisperHotkeyAction(): the value stored in the profile is the TRANSLATED
        // string (EN "Whisper (hold to speak)", ES "Susurrar ...") and the old match
        // on "ussurr" (PT) made the press fire NOTHING in other languages -- as if the
        // button had never been pressed.
        ServerTab* t = currentTab();
        if (!t) return;
        t->setWhisperHold(!t->whisperHoldActive(), S::num("hotkeys/whisperScope", 1));
        return;
    }
    if (action.contains(tr("microfone"))) {
        if (ServerTab* t = currentTab()) t->setMicMuted(!t->isMicMuted());
        updateConnectionUi();
    } else if (action.contains(tr("alto-falantes"))) {
        if (ServerTab* t = currentTab()) t->setSpeakersMuted(!t->isSpkMuted());
        updateConnectionUi();
    } else if (action.contains(tr("ausente"))) {
        if (ServerTab* t = currentTab()) t->setAway(!t->isAway());
        updateConnectionUi();
    } else if (action.contains(tr("comandante"))) {
        if (ServerTab* t = currentTab()) {
            ServerData& d = t->data();
            User& self = d.users[d.selfId];
            self.commander = !self.commander;
            t->tree()->rebuild();
        }
        updateConnectionUi();
    } else if (action.contains(tr("gravação"))) {
        if (ServerTab* t = currentTab()) t->toggleRecording();
        updateConnectionUi();
    } else if (action.contains(tr("transmissão contínua"))) {
        const int m = S::num("capture/pttMode", 1);
        S::set("capture/pttMode", m == 2 ? 1 : 2); // continuous <-> detection
        statusBar()->showMessage(m == 2 ? tr("Transmissão contínua desativada")
                                        : tr("Transmissão contínua ativada"), 3000);
    }
}

void MainWindow::applyHotkeys() {
    // Rebuilding the keys with an ACTIVE whisper left the hold stuck: the old
    // HoldKey entries (with the "held" state) are dropped below -- if the key that
    // RELEASES the whisper disappeared in the meantime (imported list without the
    // key, list removed, shortcuts rewritten), the release never arrived and the
    // client kept "whispering" forever: orange ring stuck, voice going only to the
    // old targets, and the user unable to speak through the whisper or the channel.
    // Release the hold NOW; if the key still exists and is physically held down,
    // the 50 ms polling holds it again right away.
    if (ServerTab* t = currentTab()) {
        if (t->whisperHoldActive()) t->setWhisperHold(false, 2);
    }
    for (QShortcut* s : m_hotkeyShortcuts)
        if (s) s->deleteLater();
    m_hotkeyShortcuts.clear();

#ifdef Q_OS_WIN
    // remove hotkeys globais anteriores (ids 100+)
    for (int id : m_globalHotkeyActions.keys())
        UnregisterHotKey(HWND(winId()), id);
    m_globalHotkeyActions.clear();
    m_whisperHolds.clear();
    m_mouseHotkeys.clear();
    // shortcuts whose RegisterHotKey failed (another program already owns the
    // combination -- previously total silence: the user saw the configured key and
    // it simply did nothing). v1.1.27: reported in the status bar.
    QStringList failedRegistrations;
#endif

    // list of the ACTIVE profile (migration of the legacy "hotkeys/list" key)
    const QString prof = S::str(QStringLiteral("hotkeys/profile"), tr("Padrão"));
    QString listJson = S::str(QStringLiteral("hotkeys/list.") + prof);
    if (listJson.isEmpty() && prof == tr("Padrão"))
        listJson = S::str(QStringLiteral("hotkeys/list")); // legado
    QJsonDocument doc = QJsonDocument::fromJson(listJson.toUtf8());
    if (doc.isArray()) {
        int idx = 0;
        for (const QJsonValue& v : doc.array()) {
            QJsonObject o = v.toObject();
            const QString action = o["action"].toString();
            const QString keyStr = o["key"].toString();
            if (keyStr.isEmpty()) continue;

            // ---- Whisper under Hotkeys: becomes a "hold to whisper" key with the
            // target (scope) chosen in its own row -- the same polling engine as the
            // Whisper Lists. Regression fixed: the action kept being offered and saved
            // by the UI but was silently discarded here; for whoever configured whisper
            // this way, the key never fired.
            // Second regression fixed (1.1.26): the match was only the PT substring
            // "ussurr" -- EN/ES clients saved the translated action, which does not
            // match, and the press fell into the generic path without firing anything.
            // Now: canonical id ("whisper") OR any PT/EN/ES spelling
            // (isWhisperHotkeyAction).
            const bool isWhisperAction =
                o.value("id").toString() == QLatin1String("whisper")
                || isWhisperHotkeyAction(action);

            int mouseBtn = 0;
            if (keyStr == QLatin1String(HotkeyEdit::kMouse4))         mouseBtn = 4;
            else if (keyStr == QLatin1String(HotkeyEdit::kMouse5))    mouseBtn = 5;
            else if (keyStr == QLatin1String(HotkeyEdit::kMouseMiddle)) mouseBtn = 3;

            if (isWhisperAction) {
#ifdef Q_OS_WIN
                HoldKey hk;
                hk.scope = qBound(0, o.value("scope").toInt(1), 2);
                hk.mouseBtn = mouseBtn;
                if (mouseBtn == 0) {
                    UINT vk = 0, mods = 0;
                    if (!specToVkFromSpec(keyStr, vk, mods)) {
                        AppLog::info(tr("Tecla de atalho ignorada (não suportada): %1")
                                         .arg(keyStr));
                        continue;
                    }
                    hk.vk = vk;
                    hk.mods = mods & (MOD_ALT | MOD_CONTROL | MOD_SHIFT | MOD_WIN);
                }
                m_whisperHolds << hk;
#else
                const QKeySequence wseq = hotkeySpecToSequence(keyStr);
                if (!wseq.isEmpty()) {
                    const int wscope = qBound(0, o.value("scope").toInt(1), 2);
                    QShortcut* sc = new QShortcut(wseq, this);
                    sc->setContext(Qt::WindowShortcut);
                    connect(sc, &QShortcut::activated, this, [this, wscope] {
                        if (ServerTab* t = currentTab())
                            t->setWhisperHold(!t->whisperHoldActive(), wscope);
                    });
                    m_hotkeyShortcuts << sc;
                }
#endif
                continue;
            }

            const QKeySequence seq = hotkeySpecToSequence(keyStr);
            if (seq.isEmpty()) continue;
#ifdef Q_OS_WIN
            if (mouseBtn != 0) {
                MouseHotkey mh;
                mh.mouseBtn = mouseBtn;
                mh.action = action;
                mh.held = false;
                m_mouseHotkeys << mh;
            } else {
                // GLOBAL: works in the background, as everywhere in Halla
                UINT vk = 0, mods = 0;
                if (specToVkFromSpec(keyStr, vk, mods)) {
                    const int id = 100 + idx;
                    if (RegisterHotKey(HWND(winId()), id, mods | MOD_NOREPEAT, vk))
                        m_globalHotkeyActions[id] = action;
                    else
                        failedRegistrations << keyStr; // conflict: another app already owns it
                } else {
                    AppLog::info(tr("Tecla de atalho ignorada (não suportada): %1")
                                     .arg(keyStr));
                }
            }
#else
            QShortcut* sc = new QShortcut(seq, this);
            sc->setContext(Qt::WindowShortcut);
            connect(sc, &QShortcut::activated, this,
                    [this, action] { runConfiguredAction(action); });
            m_hotkeyShortcuts << sc;
#endif
            ++idx;
        }
    }

    // Loads the shortcuts configured in the Whisper Lists (Whisper -> Whisper List)
    QJsonDocument whisperDoc = QJsonDocument::fromJson(S::str("whispers").toUtf8());
    if (whisperDoc.isArray()) {
        for (const QJsonValue& v : whisperDoc.array()) {
            QJsonObject o = v.toObject();
            const QString listName = o["name"].toString();
            const QString keyStr = o["key"].toString();
            const QString replyStr = o["replyKey"].toString();
            if (keyStr.isEmpty() && replyStr.isEmpty()) continue;

#ifdef Q_OS_WIN
            // main list key ("hold to whisper")
            if (!keyStr.isEmpty()) {
                HoldKey hk;
                hk.scope = 2; // User list
                hk.whisperListName = listName;
                bool okKey = true;
                if (keyStr == QLatin1String(HotkeyEdit::kMouse4))         hk.mouseBtn = 4;
                else if (keyStr == QLatin1String(HotkeyEdit::kMouse5))    hk.mouseBtn = 5;
                else if (keyStr == QLatin1String(HotkeyEdit::kMouseMiddle)) hk.mouseBtn = 3;
                else {
                    UINT vk = 0, mods = 0;
                    if (!specToVkFromSpec(keyStr, vk, mods)) {
                        okKey = false; // unmappable key: continue with the reply only
                    } else {
                        hk.vk = vk;
                        hk.mods = mods & (MOD_ALT | MOD_CONTROL | MOD_SHIFT | MOD_WIN);
                    }
                }
                if (okKey) m_whisperHolds << hk;
            }

            // Reply key: hold to whisper back to whoever whispered last. The field was
            // saved by the lists dialog and never read -- configuring the reply key did
            // nothing (applies to both keyboard key and mouse button).
            if (!replyStr.isEmpty()) {
                HoldKey rk;
                rk.scope = 2;
                rk.reply = true;
                rk.whisperListName = listName;
                if (replyStr == QLatin1String(HotkeyEdit::kMouse4))         rk.mouseBtn = 4;
                else if (replyStr == QLatin1String(HotkeyEdit::kMouse5))    rk.mouseBtn = 5;
                else if (replyStr == QLatin1String(HotkeyEdit::kMouseMiddle)) rk.mouseBtn = 3;
                else {
                    UINT rvk = 0, rmods = 0;
                    if (specToVkFromSpec(replyStr, rvk, rmods)) {
                        rk.vk = rvk;
                        rk.mods = rmods & (MOD_ALT | MOD_CONTROL | MOD_SHIFT | MOD_WIN);
                        m_whisperHolds << rk;
                    }
                }
            }
#else
            const QKeySequence seq = hotkeySpecToSequence(keyStr);
            if (!seq.isEmpty()) {
                QShortcut* sc = new QShortcut(seq, this);
                sc->setContext(Qt::WindowShortcut);
                connect(sc, &QShortcut::activated, this, [this, listName] {
                    S::set("whisper/activeList", listName);
                    if (ServerTab* t = currentTab()) {
                        t->setWhisperHold(!t->whisperHoldActive(), 2);
                    }
                });
                m_hotkeyShortcuts << sc;
            }
#endif
        }
    }

#ifdef Q_OS_WIN
    // global 50 ms timer: detects PTT press/release (from any source) and the
    // whisper keys -- works with the window in the background, since it reads the
    // physical key/button state via GetAsyncKeyState
    if (!m_pttPoll) {
        m_pttPoll = new QTimer(this);
        m_pttPoll->setInterval(50);
        connect(m_pttPoll, &QTimer::timeout, this, &MainWindow::pollGlobalInputs);
    }
    m_pttPoll->start();

    // v1.1.27: a global hotkey conflict is no longer invisible -- the user learns
    // WHY the configured key does not fire
    if (!failedRegistrations.isEmpty()) {
        const QString msg = tr("Atalho global não registrado (em uso por outro programa): %1")
                                .arg(failedRegistrations.join(QStringLiteral(", ")));
        AppLog::info(msg);
        statusBar()->showMessage(msg, 8000);
    }
#endif

    // (re)registers the system-wide PTT key (Windows) -- key OR mouse
    registerPttHotkey();
}

// ======================================================================
void MainWindow::closeEvent(QCloseEvent* e) {
    if (m_closeDelayPending) {
        e->ignore();
        return;
    }

    if (!m_restartAfterClose && !m_closingAfterSound
            && S::flag("app/closeToTray", false)
            && m_tray && m_tray->isVisible() && !S::flag("app/forceQuit", false)) {
        hide();
        m_tray->showMessage(QStringLiteral("Halla"),
                            tr("O Halla continua em execução na bandeja do sistema."),
                            QSystemTrayIcon::Information, 2400);
        e->ignore();
        return;
    }
    if (!m_closingAfterSound && m_tabs->count() > 0
            && S::flag("app/confirmQuit", true) && !S::flag("app/forceQuit", false)) {
        const auto ret = QMessageBox::question(
            this, tr("Sair"),
            tr("Você ainda está conectado a servidores.\nDeseja realmente sair?"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (ret != QMessageBox::Yes) {
            // The language preference stays saved for the next launch, but another
            // instance must not appear if the user cancelled the exit.
            m_restartAfterClose = false;
            e->ignore();
            return;
        }
    }

    if (!m_closingAfterSound && m_tabs->count() > 0) {
        e->ignore();
        m_closeDelayPending = true;
        if (S::flag("notify/disconnectSound", true))
            HSound::play(QStringLiteral("disconnected"));
        HSpeech::say(tr("Desconectado do servidor"));
        QTimer::singleShot(1000, this, [this] {
            while (m_tabs->count() > 0) {
                ServerTab* tab = qobject_cast<ServerTab*>(m_tabs->widget(0));
                if (!tab) { m_tabs->removeTab(0); continue; }
                finishDisconnectTab(tab);
            }
            m_closeDelayPending = false;
            m_closingAfterSound = true;
            close();
        });
        return;
    }

    saveSession();
    if (m_log) m_log->close();
    AppLog::info(tr("Halla encerrado"));
    QMainWindow::closeEvent(e);
}

bool MainWindow::eventFilter(QObject* obj, QEvent* ev) {
    return QMainWindow::eventFilter(obj, ev);
}

// "Minimize to tray" (Options > Appearance > Tray icon)
void MainWindow::changeEvent(QEvent* e) {
    if (e->type() == QEvent::WindowStateChange && isMinimized())
        savePlacementBeforeHide();
    if (e->type() == QEvent::WindowStateChange && isMinimized() &&
        S::flag("app/minimizeToTray", false) && m_tray && m_tray->isVisible()) {
        QTimer::singleShot(0, this, [this] {
            hide();
            m_tray->showMessage(QStringLiteral("Halla"),
                                tr("O Halla continua em execução na bandeja do sistema."),
                                QSystemTrayIcon::Information, 1800);
        });
    }
    QMainWindow::changeEvent(e);
}

// ----------------------------------------------------------------------
// v1.1.31 -- restore on the same screen after the tray
//
// Report: with the app on screen 2, minimizing to the tray and coming back made
// the window reappear on screen 1. Cause: Windows discards the placement of
// hidden windows that are still minimized (the hide() of the tray path) and a
// bare showNormal() returns the window on the primary screen. Solution: store
// the "normal" rectangle + the maximized state before hiding and reapply them on
// return -- including re-maximizing on the right screen (Windows maximizes on
// the screen where the window currently is).
void MainWindow::savePlacementBeforeHide() {
    const QRect g = normalGeometry();
    if (!g.isValid()) return;
    m_geometryBeforeHide = g;
    m_wasMaximizedBeforeHide = isMaximized();
    // Maximized: the "normal" rectangle may be left over from an old screen (the
    // user moved the maximized window between screens). Ensures re-maximizing on
    // the screen where the window was at hide time.
    if (m_wasMaximizedBeforeHide) {
        if (QScreen* s = screen()) {
            const QRect avail = s->availableGeometry();
            if (!avail.contains(g.center())) {
                m_geometryBeforeHide = QRect(avail.topLeft() + QPoint(48, 48),
                                             g.size().boundedTo(avail.size()));
            }
        }
    }
}

void MainWindow::hideEvent(QHideEvent* e) {
    savePlacementBeforeHide();
    QMainWindow::hideEvent(e);
}

void MainWindow::showFromTray() {
    const QRect g = m_geometryBeforeHide;
    showNormal();
    // Only reapplies when the saved rectangle still falls on an existing screen --
    // the second screen may have been disconnected while the app was in the tray.
    if (!g.isNull() && QGuiApplication::screenAt(g.center()))
        setGeometry(g);
    if (m_wasMaximizedBeforeHide)
        showMaximized();
    raise();
    activateWindow();
}

// ======================================================================
// Global PTT: RegisterHotKey (keyboard) + Raw Input (mouse buttons)
// work with the window IN THE BACKGROUND (global voice PTT)
// ======================================================================
#ifdef Q_OS_WIN
// adapts VkKeyScanW to the HotkeyVk resolver: returns the VK in the low byte
// with the 0x100/0x200/0x400 bits (shift/ctrl/alt that the CURRENT LAYOUT requires
// for the character -- includes AltGr) or -1 if the character does not exist in
// the layout
static int vkScanAdapter(unsigned ch) {
    const SHORT r = VkKeyScanW(static_cast<WCHAR>(ch));
    if (r == SHORT(-1)) return -1;
    return int(static_cast<unsigned short>(r));
}

// converts a QKeySequence (e.g. "Ctrl+F2", "Space") into VK + modifiers. The real
// work lives in gui/HotkeyVk.h (portable core, tested by
// tests/hotkey_vk_smoke.cpp).
static bool specToVkImpl(const QKeySequence& ks, UINT& vk, UINT& mods) {
    if (ks.isEmpty()) return false;
    unsigned v = 0, m = 0;
    if (!hvk::specToVkCore(ks[0].toCombined(), &vkScanAdapter, v, m)) return false;
    vk = v;
    mods = m | MOD_NOREPEAT;
    return true;
}
// visibilidade p/ applyHotkeys()
bool specToVk(const QKeySequence& ks, UINT& vk, UINT& mods) {
    return specToVkImpl(ks, vk, mods);
}

// v1.1.27: the main path -- the spec stored in the profile straight to VK+mods.
// Covers the keys the old converter silently dropped: OEM punctuation ("\\", ";",
// "'", "[", "]", ",", "/", "=", "`"...), Latin-1 accents ("c" with cedilla on
// ABNT2), the numeric keypad ("Num+5") and the Windows key ("Meta+X").
bool specToVkFromSpec(const QString& spec, UINT& vk, UINT& mods) {
    const QKeySequence seq = hotkeySpecToSequence(spec);
    if (seq.isEmpty()) return false;
    unsigned v = 0, m = 0;
    if (!hvk::specToVkCore(seq[0].toCombined(), &vkScanAdapter, v, m)) return false;
    vk = v;
    mods = m | MOD_NOREPEAT;
    return true;
}
#endif

#ifdef Q_OS_WIN
// Candidate VKs for each mouse button: covers both the real button (XButton) and
// what mouse software (Logitech/Razer/etc.) emits in its place
static QVector<int> mouseVkCandidates(int btn) {
    switch (btn) {
    case 3:  return { VK_MBUTTON };
    case 4:  return { VK_XBUTTON1, VK_BROWSER_BACK };
    case 5:  return { VK_XBUTTON2, VK_BROWSER_FORWARD };
    default: return {};
    }
}

static bool anyVkDown(const QVector<int>& vks) {
    for (int vk : vks)
        if (GetAsyncKeyState(vk) & 0x8000) return true;
    return false;
}

static bool modsHeld(UINT mods) {
    if ((mods & MOD_CONTROL) && !(GetAsyncKeyState(VK_CONTROL) & 0x8000)) return false;
    if ((mods & MOD_SHIFT)   && !(GetAsyncKeyState(VK_SHIFT)   & 0x8000)) return false;
    if ((mods & MOD_ALT)     && !(GetAsyncKeyState(VK_MENU)    & 0x8000)) return false;
    // v1.1.27: Windows key as whisper/PTT modifier ("Meta+X")
    if ((mods & MOD_WIN) &&
        !((GetAsyncKeyState(VK_LWIN) | GetAsyncKeyState(VK_RWIN)) & 0x8000))
        return false;
    return true;
}
#endif

bool MainWindow::isMouseDown(int btn) {
#ifdef Q_OS_WIN
    bool rawDown = false;
    if (btn >= 0 && btn < 6) {
        rawDown = m_mouseButtonState[btn];
    }
    bool vkDown = anyVkDown(mouseVkCandidates(btn));
    return rawDown || vkDown;
#else
    Q_UNUSED(btn);
    return false;
#endif
}

// 50 ms timer: reads the PHYSICAL state of PTT and of the whisper keys. Detects
// both press and release -- including in the background -- without depending on
// which window received the mouse message.
void MainWindow::pollGlobalInputs() {
#ifdef Q_OS_WIN
    // ---- PTT
    if (m_mousePttButton != 0) {
        pttSetHeld(isMouseDown(m_mousePttButton));
    } else if (m_pttVk != 0) {
        const bool down = (GetAsyncKeyState(int(m_pttVk)) & 0x8000) &&
                          modsHeld(m_pttMods);
        pttSetHeld(down);
    }

    // ---- whisper (hold to talk)
    for (int i = 0; i < m_whisperHolds.size(); ++i) {
        HoldKey& h = m_whisperHolds[i];
        const bool down = h.mouseBtn != 0
            ? isMouseDown(h.mouseBtn)
            : (h.vk != 0 && (GetAsyncKeyState(int(h.vk)) & 0x8000) && modsHeld(h.mods));
        if (down != h.held) {
            h.held = down;
            whisperSetHeld(i, down);
        }
    }

    // ---- outros atalhos de mouse (um clique ativa, outro solta)
    for (int i = 0; i < m_mouseHotkeys.size(); ++i) {
        MouseHotkey& mh = m_mouseHotkeys[i];
        const bool down = isMouseDown(mh.mouseBtn);
        if (down && !mh.held) {
            mh.held = true;
            runConfiguredAction(mh.action);
        } else if (!down && mh.held) {
            mh.held = false;
        }
    }
#endif
}

void MainWindow::whisperSetHeld(int idx, bool held) {
    if (idx < 0 || idx >= m_whisperHolds.size()) return;
    ServerTab* t = currentTab();
    if (!t) return;

    const HoldKey& h = m_whisperHolds.at(idx);
    if (h.reply) {
        // reply key: the target is whoever whispered last
        t->setWhisperReplyHold(held);
        return;
    }

    if (held && !h.whisperListName.isEmpty()) {
        S::set("whisper/activeList", h.whisperListName);
    }

    t->setWhisperHold(held, h.scope);
}

bool MainWindow::nativeEvent(const QByteArray& eventType, void* message,
                             qintptr* result) {
#ifdef Q_OS_WIN
    if (eventType == QByteArrayLiteral("windows_generic_MSG") ||
        eventType == QByteArrayLiteral("windows_dispatcher_MSG")) {
        MSG* msg = static_cast<MSG*>(message);

        if (msg->message == WM_HOTKEY) {
            const int id = int(msg->wParam);
            if (id == 1)
                pttSetHeld(true); // soltura detectada por polling (GetAsyncKeyState)
            else if (m_globalHotkeyActions.contains(id))
                runConfiguredAction(m_globalHotkeyActions.value(id));
            else if (m_pluginGlobalHotkeys.contains(id)) {
                const auto action = m_pluginGlobalHotkeys.value(id);
                PluginManager::instance().triggerUiAction(action.first, action.second);
            }
        } else if (msg->message == WM_INPUT &&
                   (msg->wParam == RIM_INPUT || msg->wParam == RIM_INPUTSINK)) {
            // mouse button (PTT/whisper/shortcuts) pressed in any window -- fast
            // path; the 50 ms timer covers the remaining cases
            BYTE buf[64];
            UINT sz = sizeof(buf);
            if (GetRawInputData(reinterpret_cast<HRAWINPUT>(msg->lParam),
                                RID_INPUT, buf, &sz,
                                sizeof(RAWINPUTHEADER)) != UINT(-1)) {
                RAWINPUT* ri = reinterpret_cast<RAWINPUT*>(buf);
                if (ri->header.dwType == RIM_TYPEMOUSE) {
                    const USHORT f = ri->data.mouse.usButtonFlags;
                    auto hit = [&](int btn, USHORT down, USHORT up) {
                        if (f & down) m_mouseButtonState[btn] = true;
                        if (f & up)   m_mouseButtonState[btn] = false;
                    };
                    hit(3, RI_MOUSE_MIDDLE_BUTTON_DOWN, RI_MOUSE_MIDDLE_BUTTON_UP);
                    hit(4, RI_MOUSE_BUTTON_4_DOWN, RI_MOUSE_BUTTON_4_UP);
                    hit(5, RI_MOUSE_BUTTON_5_DOWN, RI_MOUSE_BUTTON_5_UP);
                    // Mouse raw input arrives for EVERY movement (not only buttons):
                    // with RIDEV_INPUTSINK and a 1000 Hz mouse that was ~1000
                    // WM_INPUT/s, each one waking the GUI and running the full poll. The
                    // state only changes on a BUTTON event -- movement stays with the
                    // 50 ms timer.
                    if (f & (RI_MOUSE_MIDDLE_BUTTON_DOWN | RI_MOUSE_MIDDLE_BUTTON_UP |
                             RI_MOUSE_BUTTON_4_DOWN | RI_MOUSE_BUTTON_4_UP |
                             RI_MOUSE_BUTTON_5_DOWN | RI_MOUSE_BUTTON_5_UP)) {
                        pollGlobalInputs();
                    }
                }
            }
        } else if (msg->message == WM_XBUTTONDOWN || msg->message == WM_XBUTTONUP ||
                   msg->message == WM_MBUTTONDOWN || msg->message == WM_MBUTTONUP ||
                   msg->message == WM_APPCOMMAND) {
            // alternative path with the window focused (including mice whose software
            // sends the browser "Back/Forward" keys)
            pollGlobalInputs();
        }
    }
#endif
    return QMainWindow::nativeEvent(eventType, message, result);
}

void MainWindow::registerPttHotkey() {
#ifdef Q_OS_WIN
    // limpa o modo anterior
    if (m_pttRegistered) { UnregisterHotKey(HWND(winId()), 1); m_pttRegistered = false; }
    m_mousePttButton = 0;
    m_pttVk = 0;
    m_pttMods = 0;

    // ---- Always register the mouse Raw Input devices so mouse buttons and shortcuts
    // ---- work with maximum responsiveness and globally
    RAWINPUTDEVICE rid = {};
    rid.usUsagePage = 0x01;          // generic desktop
    rid.usUsage     = 0x02;          // mouse
    rid.dwFlags     = RIDEV_INPUTSINK; // receives even without focus
    rid.hwndTarget  = HWND(winId());
    if (RegisterRawInputDevices(&rid, 1, sizeof(rid)))
        m_rawInputRegistered = true;

    // The PTT key only becomes a global hotkey in "press to talk" mode (0). In
    // voice-activated (1, DEFAULT) and continuous (2) modes, registering the key
    // swallowed it from the ENTIRE system for nothing: with the default Space, EVERY
    // user in voice mode lost the space bar in ALL applications while Halla was
    // open -- one of the drivers behind the "keyboard stopped working" reports.
    if (S::num("capture/pttMode", 1) != 0) return;

    const QString spec = S::str("capture/pttKey", QStringLiteral("Space"));
    if (spec.isEmpty()) return;

    // ---- mouse buttons: Raw Input (fast path) + polling (guaranteed)
    int btn = 0;
    if (spec == QLatin1String(HotkeyEdit::kMouse4))        btn = 4;
    else if (spec == QLatin1String(HotkeyEdit::kMouse5))   btn = 5;
    else if (spec == QLatin1String(HotkeyEdit::kMouseMiddle)) btn = 3;
    if (btn != 0) {
        m_mousePttButton = btn; // the 50 ms timer reads the button's physical state
        AppLog::info(tr("PTT global registrado no mouse: %1").arg(spec));
        return;
    }

    // ---- key: RegisterHotKey (fast path) + release polling/backup
    UINT vk = 0, mods = 0;
    if (!specToVkFromSpec(spec, vk, mods)) {
        // v1.1.27: punctuation keys ("\\", ";", accented keys...) became valid as PTT
        // -- and a truly incompatible key is LOGGED, not silenced
        AppLog::info(tr("Não foi possível registrar a tecla PTT: %1").arg(spec));
        return;
    }
    m_pttVk = vk;
    m_pttMods = mods & (MOD_ALT | MOD_CONTROL | MOD_SHIFT | MOD_WIN);
    if (RegisterHotKey(HWND(winId()), 1, mods, vk)) {
        m_pttRegistered = true;
        AppLog::info(tr("Tecla PTT global registrada: %1").arg(spec));
    } else {
        AppLog::info(tr("Não foi possível registrar a tecla PTT: %1").arg(spec));
    }
#endif
}


void MainWindow::pttSetHeld(bool held) {
    if (m_pttHeld == held) return;
    // only takes effect in "Press to talk" mode (Options > Capture)
    if (S::num("capture/pttMode", 1) != 0) return;
    m_pttHeld = held;
    if (ServerTab* t = currentTab())
        if (VoiceEngine* v = t->voice()) v->setPttHeld(held);
}

// ======================================================================
void MainWindow::loadDemoState() {
    // Local state used by the captures: the names and proportions reproduce the
    // exact composition provided by the user.
    ServerData init;
    init.name = QStringLiteral("Servidor Halla");
    init.address = QStringLiteral("163.176.35.133");
    init.version = QString::fromUtf8(halla::kAppVersion);
    init.platform = QStringLiteral("Windows");
    init.maxClients = 500;

    User self;
    self.id = 1;
    self.name = QStringLiteral("Farley Barbosa");
    self.platform = QStringLiteral("Windows");
    self.serverGroups = QStringLiteral("Normal");
    User mobile;
    mobile.id = 2;
    mobile.name = QStringLiteral("Farley Barbosa Mobile");
    mobile.platform = QStringLiteral("Android");
    mobile.serverGroups = QStringLiteral("Normal");

    Channel def;
    def.id = 1;
    def.name = tr("Canal padrão");
    def.isDefault = true;
    def.users << 1 << 2;
    Channel test;
    test.id = 2;
    test.name = QStringLiteral("teste");
    test.users << 1;
    test.codec = 4;
    Channel test2;
    test2.id = 3;
    test2.name = QStringLiteral("teste 2");
    test2.codec = 4;

    init.users[1] = self;
    init.users[2] = mobile;
    init.channels[1] = def;
    init.channels[2] = test;
    init.channels[3] = test2;
    init.nextChannelId = 4;
    createLocalTab(init);

    ServerTab* t = currentTab();
    if (!t) return;
    t->tree()->rebuild();
    t->tree()->expandAll();
    t->tree()->selectNode(NodeChannel, 2);
    t->chat()->addServerSystem(tr("Conectado ao servidor: %1").arg(init.address));
    t->chat()->addChannelSystem(tr("Você entrou no canal \"Canal padrão\"."));
    t->chat()->addServerChat(QStringLiteral("Farley Barbosa Mobile"),
                             QStringLiteral("Olá! [b]Bem-vindo ao Halla[/b]"));
}

void MainWindow::toggleScreenShare() {
    ServerTab* t = currentTab();
    if (!t || !t->net()) {
        m_actScreenShare->setChecked(false);
        return;
    }

    if (!t->net()->allowScreenShare()) {
        m_actScreenShare->setChecked(false);
        QMessageBox::warning(this, tr("Compartilhamento de Tela"),
                             tr("O compartilhamento de tela está desativado pelas configurações deste servidor."));
        return;
    }

    if (m_actScreenShare->isChecked()) {
        ScreenShareDialog dlg(
            t->net()->screenshareWidth(), t->net()->screenshareHeight(),
            t->net()->screenshareFps(), t->net()->screenshareBitrateKbps(), this);
        if (dlg.exec() == QDialog::Accepted) {
            m_screenShareSourceType = dlg.selectedSourceType();
            m_screenShareSourceId = dlg.selectedSourceId();
            m_screenShareSeq = 0;

            if (m_webrtcSession && m_webrtcSession->isNativeAvailable()) {
                // v1.1.28: broadcast from the ACTIVE tab -- m_net could be
                // pointing at another connected tab (multi-server).
                m_webrtcSession->setNetSession(t->net());
                m_webrtcSession->setCaptureSource(m_screenShareSourceType, m_screenShareSourceId);
                m_webrtcSession->setCaptureQuality(dlg.selectedWidth(), dlg.selectedHeight(),
                                                   dlg.selectedFps(), dlg.selectedBitrateKbps());
                m_webrtcSession->setCaptureSystemAudio(dlg.captureSystemAudio());
                m_webrtcSession->startBroadcast();
                m_actScreenShare->setIcon(HIcons::screenShare(true));
                const int selfId = t->data().selfId;
                if (!m_screenShareWindows.contains(selfId)) {
                    QString userName = t->data().users.contains(selfId) ? t->data().users[selfId].name : tr("Minha transmissão");
                    ScreenShareWindow* win = new ScreenShareWindow(
                        selfId, userName, false, ScreenShareWindow::AudioMuteCallback(), this);
                    m_screenShareWindows[selfId] = win;
                    connect(win, &QDialog::finished, this, [this, selfId]() {
                        m_screenShareWindows.remove(selfId);
                        updateWebRtcPreviewState();
                    });
                    win->show();
                }
                updateWebRtcPreviewState();
                t->data().users[t->data().selfId].screensharing = true;
                emit t->net()->stateChanged();
                return;
            }

            if (!m_screenShareTimer) {
                m_screenShareTimer = new QTimer(this);
                connect(m_screenShareTimer, &QTimer::timeout, this, &MainWindow::captureAndSendScreen);
            }
            int interval = qBound(10, 1000 / t->net()->screenshareFps(), 1000);
            m_screenShareTimer->start(interval);

            t->net()->sendScreenShareStart();
            m_actScreenShare->setIcon(HIcons::screenShare(true));
        } else {
            m_actScreenShare->setChecked(false);
        }
    } else {
        if (m_webrtcSession && m_webrtcSession->isBroadcasting()) {
            m_webrtcSession->stopBroadcast();
            updateWebRtcPreviewState();
        }
        if (m_screenShareTimer) {
            m_screenShareTimer->stop();
        }
        t->net()->sendScreenShareStop();
        m_actScreenShare->setIcon(HIcons::screenShare(false));

        handleScreenshareStateChanged(t->data().selfId, false, QString());
        t->data().users[t->data().selfId].screensharing = false;
        emit t->net()->stateChanged();
    }
}

void MainWindow::captureAndSendScreen() {
    ServerTab* t = currentTab();
    if (!t || !t->net()) {
        if (m_screenShareTimer) m_screenShareTimer->stop();
        m_actScreenShare->setChecked(false);
        m_actScreenShare->setIcon(HIcons::screenShare(false));
        return;
    }

    // Controle de Congestionamento Ativo (Active Congestion Control):
    if (t->net()->bytesToWrite() > 45000) {
        return;
    }

    QPixmap pix;
    QScreen* screen = QGuiApplication::primaryScreen();
    if (m_screenShareSourceType == 0) {
        int sIdx = int(m_screenShareSourceId);
        QList<QScreen*> screens = QGuiApplication::screens();
        if (sIdx >= 0 && sIdx < screens.size() && screens[sIdx]) {
            pix = screens[sIdx]->grabWindow(0);
        } else if (screen) {
            pix = screen->grabWindow(0);
        }
    } else {
        #ifdef Q_OS_WIN
        if (m_screenShareSourceId > 0) {
            pix = grabWindowsApp(HWND(m_screenShareSourceId));
        }
        #else
        if (screen && m_screenShareSourceId > 0) {
            pix = screen->grabWindow(WId(m_screenShareSourceId));
        }
        #endif
    }

    if (pix.isNull()) return;

    // Sends frames continuously at the configured FPS. The old delta-skip based on a
    // 16x16 thumbnail saved upload, but on Mobile clients the broadcast looked frozen
    // when small changes/cursor movement did not alter the thumbnail. Stream
    // smoothness takes priority here.
    m_keepAliveTicks = 0;
    m_prevThumbnail = QImage();

    // Screen share uses UDP without retransmission. At 1920x1080 each frame becomes
    // hundreds of datagrams; losing a single chunk makes the Mobile client drop the
    // frame and the broadcast look stuck. We cap the outgoing stream at a
    // mobile-friendly resolution and moderate quality, keeping it smooth.
    const int targetW = qMin(t->net()->screenshareWidth(), 960);
    const int targetH = qMin(t->net()->screenshareHeight(), 540);
    QPixmap scaled = pix.scaled(targetW, targetH, Qt::KeepAspectRatio, Qt::SmoothTransformation);

    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    scaled.save(&buffer, "JPEG", 45);

    t->net()->sendScreenShareFrame(bytes, ++m_screenShareSeq);

    if (m_screenShareWindows.contains(t->data().selfId)) {
        m_screenShareWindows[t->data().selfId]->updateFrame(bytes);
    }
}

void MainWindow::updateWebRtcPreviewState() {
    // The broadcast preview pipeline (GPU->CPU copy + smooth downscale) may only run
    // while a "My broadcast" window exists to consume it. Before v1.1.20, closing
    // the window left Halla generating ~20 full-resolution previews per second into
    // the void -- a heavy app burning memory for the whole broadcast, even with
    // nobody (not even the user) watching.
    if (!m_webrtcSession) return;
    ServerTab* tab = currentTab();
    const int selfId = tab ? tab->data().selfId : 0;
    m_webrtcSession->setLocalPreviewEnabled(m_screenShareWindows.contains(selfId));
}

void MainWindow::openScreenShareWindow(int userId) {
    ServerTab* tab = currentTab();
    if (!tab || userId <= 0 || m_screenShareWindows.contains(userId)) return;
    QString userName = tr("Usuário #%1").arg(userId);
    if (tab->data().users.contains(userId)) userName = tab->data().users[userId].name;
    const bool isViewer = userId != tab->data().selfId;
    const QPointer<ServerTab> screenTab(tab);
    ScreenShareWindow* window = new ScreenShareWindow(
        userId, userName, isViewer,
        [screenTab, userId](bool muted) {
            if (muted && screenTab && screenTab->voice())
                screenTab->voice()->clearStreamPcm(userId);
        }, this);
    m_screenShareWindows[userId] = window;
    connect(window, &QDialog::finished, this, [this, screenTab, userId] {
        if (screenTab && screenTab->voice()) screenTab->voice()->clearStreamPcm(userId);
        m_screenShareWindows.remove(userId);
        cancelWatchdog(userId);
        updateWebRtcPreviewState();
        if (m_webrtcSession) m_webrtcSession->stopWatching(userId);
    });
    window->show();
    updateWebRtcPreviewState();
}

void MainWindow::handleScreenshareStateChanged(int userId, bool on, const QString& mode) {
    ServerTab* tab = currentTab();
    if (!tab) return;

    if (on) {
        // v1.1.28: the mode reported by the server ("webrtc"/"jpeg"/empty) already
        // arrived stored in users[userId].screenshareMode from NetSession; there is
        // nothing to do here beyond the visual flow.
        Q_UNUSED(mode);
        // The "live" state updates only the tree/icon. A remote broadcast only opens
        // after the explicit click on Watch.
        if (userId != tab->data().selfId) return;
        // The local WebRTC preview is already created by the broadcast flow.
        if (m_webrtcSession && m_webrtcSession->isBroadcasting()) return;
        openScreenShareWindow(userId); // preview do modo JPEG legado
        return;
    }

    // Broadcast finished: releases the watchdog and pending state for that user.
    cancelWatchdog(userId);
    if (m_pendingWatch.userId == userId) {
        m_pendingWatch = {};
        m_pendingWatchTab = nullptr;
        if (m_pendingWatchPoll) m_pendingWatchPoll->stop();
    }

    if (m_screenShareWindows.contains(userId)) {
        ScreenShareWindow* window = m_screenShareWindows.take(userId);
        window->close();
        window->deleteLater();
    }
}

void MainWindow::handleScreenshareFrameReceived(int userId, const QByteArray& jpegData) {
    markWatchAlive(userId);
    m_lastScreenshareFrames[userId] = jpegData;
    if (m_screenShareWindows.contains(userId)) {
        m_screenShareWindows[userId]->updateFrame(jpegData);
    }
}

// ---- v1.1.28: assistance for broadcasts that never connect ----------------

void MainWindow::armWatchdog(int userId) {
    if (userId <= 0) return;
    cancelWatchdog(userId);
    QTimer* timer = new QTimer(this);
    timer->setSingleShot(true);
    timer->setInterval(12000);
    connect(timer, &QTimer::timeout, this, [this, userId] {
        m_watchNoFrameTimers.remove(userId);
        ScreenShareWindow* window = m_screenShareWindows.value(userId, nullptr);
        if (!window || !window->isVisible()) return;
        // No explicit ICE error and not a single frame in 12 s: something along the
        // way swallowed the request/offer (legacy broadcast without a reply, lost
        // relay, P2P that never woke up). Previously: eternal silence.
        window->setStatusMessage(
            tr("A transmissão não chegou.\n"
               "O transmissor pode estar em uma versão antiga do Halla ou a "
               "conexão direta foi bloqueada pela rede."),
            true,
            [this, userId] {
                // Retry: only renegotiates if the broadcast is WebRTC -- legacy JPEG
                // mode does not answer any watch_request.
                ServerTab* tab = currentTab();
                const QString mode = tab && tab->data().users.contains(userId)
                    ? tab->data().users[userId].screenshareMode : QString();
                if (m_webrtcSession && m_webrtcSession->isNativeAvailable()
                        && mode != QLatin1String("jpeg"))
                    m_webrtcSession->restartWatch(userId);
                armWatchdog(userId);
            });
    });
    m_watchNoFrameTimers[userId] = timer;
    timer->start();
}

void MainWindow::cancelWatchdog(int userId) {
    QTimer* timer = m_watchNoFrameTimers.take(userId);
    if (timer) {
        timer->stop();
        timer->deleteLater();
    }
}

void MainWindow::markWatchAlive(int userId) {
    cancelWatchdog(userId);
    ScreenShareWindow* window = m_screenShareWindows.value(userId, nullptr);
    if (window && !window->isVisible()) return;
    if (window) window->resetWatchStatus();
}

void MainWindow::beginWatching(ServerTab* t, int userId) {
    if (!t || !t->net() || userId <= 0) return;
    // v1.1.28: guarantees that the WebRTC session answers for THIS tab/server (m_net
    // may be pointing at another connected tab).
    if (m_webrtcSession) m_webrtcSession->setNetSession(t->net());

    const QString mode = t->data().users.contains(userId)
        ? t->data().users[userId].screenshareMode : QString();
    const bool native = m_webrtcSession && m_webrtcSession->isNativeAvailable();

    openScreenShareWindow(userId);
    if (native && mode != QLatin1String("jpeg")) {
        // WebRTC broadcast (or unknown mode on an old server): requests the
        // negotiation and arms the 12 s watchdog.
        m_webrtcSession->startWatching(userId);
        armWatchdog(userId);
    } else {
        // Legacy JPEG mode (transmitter on an old build): frames arrive over UDP
        // from the relay for everyone in the channel -- there is nothing to
        // negotiate. Sending webrtc_watch_request here was useless: the old
        // transmitter never answered and the window stayed on "Waiting..." forever.
        armWatchdog(userId);
    }
}

void MainWindow::handleWatchServerError(const QString& code, const QString& msg) {
    // Only relevant while a "Watch" is in progress; the other errors follow the
    // normal flow (message in the server chat).
    const bool movePending = m_pendingWatch.userId > 0;
    if (!movePending && m_watchNoFrameTimers.isEmpty()) return;
    // MOVE (channel) codes are only ours when there is a pending move --
    // no_permission/channel_full from any other action must not show up as
    // "failed to watch".
    if (!movePending && (code == QLatin1String("no_permission")
            || code == QLatin1String("invalid_channel")
            || code == QLatin1String("bad_channel_pass")
            || code == QLatin1String("channel_full"))) return;
    static const QSet<QString> kWatchCodes = {
        QStringLiteral("webrtc_channel"), QStringLiteral("webrtc_target"),
        QStringLiteral("webrtc_not_streaming"), QStringLiteral("no_permission"),
        QStringLiteral("bad_channel_pass"), QStringLiteral("channel_full"),
        QStringLiteral("invalid_channel"), QStringLiteral("webrtc_sdp_too_big"),
        QStringLiteral("webrtc_ice_too_big"),
    };
    if (!kWatchCodes.contains(code)) return;

    // Password-protected channel: ask for the password and try again -- before, the
    // moveToChannel went WITHOUT the password, the server refused and nobody said
    // anything.
    if (code == QLatin1String("bad_channel_pass") && m_pendingWatch.channelId > 0) {
        ServerTab* watchTab = m_pendingWatchTab.data();
        if (!watchTab || !watchTab->net()) {
            m_pendingWatch = {};
            m_pendingWatchTab = nullptr;
            return;
        }
        bool ok = false;
        const QString pass = QInputDialog::getText(
            this, tr("Canal protegido por senha"),
            tr("Este canal exige senha para entrar (necessária para assistir à transmissão):"),
            QLineEdit::Password, QString(), &ok);
        if (ok && !pass.isEmpty()) {
            watchTab->net()->moveToChannel(m_pendingWatch.channelId, pass);
            m_pendingWatchTicks = 0;
            if (m_pendingWatchPoll) m_pendingWatchPoll->start(150);
        } else {
            m_pendingWatch = {};
            m_pendingWatchTab = nullptr;
            if (m_pendingWatchPoll) m_pendingWatchPoll->stop();
        }
        return;
    }

    // Other causes: cancel the pending state and report clearly -- in the window if
    // it already opened, otherwise in a dialog.
    const int userId = m_pendingWatch.userId;
    m_pendingWatch = {};
    m_pendingWatchTab = nullptr;
    if (m_pendingWatchPoll) m_pendingWatchPoll->stop();
    cancelWatchdog(userId);
    ScreenShareWindow* window = userId > 0 ? m_screenShareWindows.value(userId, nullptr) : nullptr;
    if (window && window->isVisible()) {
        window->setStatusMessage(tr("Não foi possível assistir: %1").arg(msg), true);
    } else {
        QMessageBox::warning(this, tr("Assistir transmissão"),
                             tr("Não foi possível assistir à transmissão:\n%1").arg(msg));
    }
}

void ScreenshareHoverPopup::onWatchClicked() {
    close();
    m_mw->watchStream(m_userId, m_channelId);
}

void MainWindow::handleScreenshareHovered(int userId, int channelId, const QPoint& pos) {
    ServerTab* t = currentTab();
    if (!t || !t->net()) return;

    User selfUser;
    if (t->data().users.contains(t->data().selfId)) {
        selfUser = t->data().users[t->data().selfId];
    }

    bool canJoin = true;
    if (t->data().channels.contains(channelId)) {
        const Channel& ch = t->data().channels[channelId];
        if (selfUser.groupId == 3 || selfUser.serverGroups.toLower() == "admin") {
            canJoin = true;
        } else {
            QString myGidStr = QString::number(selfUser.groupId);
            if (ch.groupPerms.contains(myGidStr)) {
                QJsonObject gPerms = ch.groupPerms[myGidStr].toObject();
                if (gPerms.value(QStringLiteral("join")).toInt(-1) == 0) {
                    canJoin = false;
                }
            }
            QString normalGidStr = QStringLiteral("2");
            if (ch.groupPerms.contains(normalGidStr)) {
                QJsonObject gPerms = ch.groupPerms[normalGidStr].toObject();
                if (gPerms.value(QStringLiteral("join")).toInt(-1) == 0) {
                    canJoin = false;
                }
            }
        }
    }
    if (!canJoin) {
        return;
    }

    // Avoids flicker: if a button already exists for the same user, do not recreate it.
    for (QWidget* w : QApplication::topLevelWidgets()) {
        if (dynamic_cast<ScreenshareHoverPopup*>(w)) {
            if (w->property("streamUserId").toInt() == userId) return;
            w->close();
        }
    }

    QString userName = tr("Usuário #%1").arg(userId);
    if (t->data().users.contains(userId)) {
        userName = t->data().users[userId].name;
    }

    QByteArray lastFrame = m_lastScreenshareFrames.value(userId);

    const QRect sourceRect(pos - QPoint(190, 14), QSize(380, 30));
    ScreenshareHoverPopup* popup = new ScreenshareHoverPopup(userId, userName, channelId, lastFrame, sourceRect, this, this);
    popup->move(pos + QPoint(18, 2));
    popup->show();
}

void MainWindow::watchStream(int userId, int channelId) {
    ServerTab* t = currentTab();
    if (!t || !t->net()) return;

    User selfUser;
    if (t->data().users.contains(t->data().selfId)) {
        selfUser = t->data().users[t->data().selfId];
    }

    bool canJoin = true;
    if (t->data().channels.contains(channelId)) {
        const Channel& ch = t->data().channels[channelId];
        if (selfUser.groupId == 3 || selfUser.serverGroups.toLower() == "admin") {
            canJoin = true;
        } else {
            QString myGidStr = QString::number(selfUser.groupId);
            if (ch.groupPerms.contains(myGidStr)) {
                QJsonObject gPerms = ch.groupPerms[myGidStr].toObject();
                if (gPerms.value(QStringLiteral("join")).toInt(-1) == 0) {
                    canJoin = false;
                }
            }
            QString normalGidStr = QStringLiteral("2");
            if (ch.groupPerms.contains(normalGidStr)) {
                QJsonObject gPerms = ch.groupPerms[normalGidStr].toObject();
                if (gPerms.value(QStringLiteral("join")).toInt(-1) == 0) {
                    canJoin = false;
                }
            }
        }
    }
    if (!canJoin) {
        QMessageBox::warning(this, tr("Transmissão"), tr("Você não tem permissão para entrar no canal desta transmissão."));
        return;
    }

    const int selfId = t->data().selfId;
    if (userId == selfId) {
        // The server rejects watch_request for yourself. For your own stream, just
        // open/reopen the local preview; broadcasting stays controlled by the main
        // broadcast button.
        if (!m_screenShareWindows.contains(selfId)) {
            QString userName = t->data().users.contains(selfId) ? t->data().users[selfId].name : tr("Minha transmissão");
            ScreenShareWindow* win = new ScreenShareWindow(
                selfId, userName, false, ScreenShareWindow::AudioMuteCallback(), this);
            m_screenShareWindows[selfId] = win;
            connect(win, &QDialog::finished, this, [this, selfId]() {
                m_screenShareWindows.remove(selfId);
                updateWebRtcPreviewState();
            });
            win->show();
        } else {
            m_screenShareWindows[selfId]->show();
            m_screenShareWindows[selfId]->raise();
            m_screenShareWindows[selfId]->activateWindow();
        }
        updateWebRtcPreviewState();
        return;
    }

    // v1.1.28: broadcast in a known WebRTC mode and this build without native
    // WebRTC? Warn immediately -- before, the request never left and the window
    // stayed on "Waiting for broadcast..." forever (the WebRTC transmitter does not
    // send legacy JPEG frames).
    const QString streamMode = t->data().users.contains(userId)
        ? t->data().users[userId].screenshareMode : QString();
    const bool nativeHere = m_webrtcSession && m_webrtcSession->isNativeAvailable();
    if (streamMode == QLatin1String("webrtc") && !nativeHere) {
        QMessageBox::warning(this, tr("Assistir transmissão"),
            tr("Esta transmissão usa o novo protocolo WebRTC, mas esta versão "
               "do Halla não o possui. Atualize o Halla para assisti-la."));
        return;
    }

    int myChan = t->data().channelOfUser(t->data().selfId);
    if (myChan != channelId) {
        // v1.1.28: do NOT fire webrtc_watch_request yet! The server only accepts
        // WebRTC signaling between users of the SAME channel -- the immediate request
        // was answered with "webrtc_channel" (which only appeared in the chat) and the
        // window waited forever. Hold the request and fire it once the local state
        // confirms the channel switch.
        t->net()->moveToChannel(channelId);
        m_pendingWatch = { userId, channelId };
        m_pendingWatchTab = t;
        m_pendingWatchTicks = 0;
        if (!m_pendingWatchPoll) {
            m_pendingWatchPoll = new QTimer(this);
            m_pendingWatchPoll->setInterval(150);
            connect(m_pendingWatchPoll, &QTimer::timeout, this, [this] {
                ServerTab* tab = m_pendingWatchTab.data();
                if (m_pendingWatch.userId <= 0 || !tab || !tab->net()) {
                    m_pendingWatch = {};
                    m_pendingWatchTab = nullptr;
                    m_pendingWatchPoll->stop();
                    return;
                }
                if (tab->data().channelOfUser(tab->data().selfId)
                        == m_pendingWatch.channelId) {
                    const PendingWatch w = m_pendingWatch;
                    m_pendingWatch = {};
                    m_pendingWatchTab = nullptr;
                    m_pendingWatchPoll->stop();
                    beginWatching(tab, w.userId);
                    return;
                }
                if (++m_pendingWatchTicks > 40) { // ~6 s sem conseguir entrar
                    m_pendingWatchTicks = 0;
                    m_pendingWatch = {};
                    m_pendingWatchTab = nullptr;
                    m_pendingWatchPoll->stop();
                    QMessageBox::warning(this, tr("Assistir transmissão"),
                        tr("Não foi possível entrar no canal da transmissão.\n"
                           "Verifique permissões, senha ou limite de usuários do canal."));
                }
            });
        }
        m_pendingWatchPoll->start(150);
        return;
    }

    beginWatching(t, userId);
}

// -- session restoration uses the network (as before) ---------------------
