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
#ifdef Q_OS_WIN
#include <windows.h>
#include <QImage>
#include <QPixmap>

// Captura avançada de aplicativos com aceleração por hardware (GPU) via PrintWindow
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

    // Flag 2 (PW_RENDERFULLCONTENT) força a renderização do conteúdo com aceleração gráfica!
    BOOL ok = PrintWindow(hwnd, hdcMem, 2);
    if (!ok) {
        ok = PrintWindow(hwnd, hdcMem, 0); // fallback padrão
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

#ifdef Q_OS_WIN
// definida mais abaixo (mesmo arquivo)
bool specToVk(const QKeySequence& ks, UINT& vk, UINT& mods);
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

    void updateFrame(const QByteArray& jpegData) {
        if (m_currentPixmap.loadFromData(jpegData)) scaleFrame();
    }

    void updateImage(const QImage& image) {
        if (image.isNull()) return;
        m_currentPixmap = QPixmap::fromImage(image);
        scaleFrame();
    }

protected:
    void resizeEvent(QResizeEvent* event) override {
        QDialog::resizeEvent(event);
        scaleFrame();
        layoutViewerControls();
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
        // SmoothTransformation em todos os frames 30 FPS bloqueava a thread da
        // UI. O vídeo já chega escalado pelo WebRTC; FastTransformation evita
        // backlog e mantém o frame mais recente na tela.
        m_label->setPixmap(m_currentPixmap.scaled(
            m_label->size(), Qt::KeepAspectRatio, Qt::FastTransformation));
    }

    int m_userId = 0;
    bool m_viewerControls = false;
    bool m_audioMuted = false;
    bool m_controlsShown = false;
    bool m_cursorHidden = false;
    AudioMuteCallback m_audioMuteChanged;
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
    // forceQuit é transitório (menu da bandeja/atualizador), nunca uma
    // preferência persistente entre execuções.
    S::set("app/forceQuit", false);
    setWindowTitle(QString::fromUtf8(halla::kAppName));
    // Mantém uma fonte grande para que o Windows escolha uma versão nítida
    // na janela, na barra de tarefas e no Alt+Tab.
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
    // entradas dinâmicas são inseridas por rebuildBookmarksMenu()

    // ------------------------- Si mesmo (estado do próprio cliente) ------
    // ações de estado criadas aqui: usadas no menu "Si mesmo", na barra de
    // ferramentas e nos atalhos globais
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
                             // O campo "group" vem como linhas "<icone> <nome>"
                             // (ex.: "rota.png ROTA"): o banner mostra só os
                             // NOMES dos cargos, sem nomes de arquivo.
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

    // ---- gravação local (compartilhada com a barra de ferramentas)
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

    // ---- listas de sussurro: a lista ativa é aplicada automaticamente.
    // Não há um segundo interruptor de "ativar", pois ele criava estado
    // divergente entre a lista salva, a barra e o áudio.
    mTools->addAction(tr("Listas de sussurro..."), this,
                      [this] {
                          ServerTab* t = currentTab();
                          WhisperDialog dlg(t ? &t->data() : nullptr, this);
                          connect(&dlg, &WhisperDialog::settingsSaved, this, [this] { applyHotkeys(); });
                          dlg.exec();
                          // A lista apenas configura os destinos. O sussurro
                          // só é transmitido enquanto sua tecla estiver pressionada.
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
                .arg(d["playbackQueue"].toInt()).arg(n ? n->pingMs() : -1));
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
                              // transparência da janela (Opções > Aparência)
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
                                      // Fecha primeiro o diálogo. A janela
                                      // principal executará o fluxo normal de
                                      // confirmação/desconexão antes de abrir
                                      // a nova instância no idioma escolhido.
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

    // Ações declaradas por complementos. A API continua Qt-free: plugins
    // registram apenas id, texto, atalho e callback C.
    m_pluginsMenu = menuBar()->addMenu(tr("Co&mplementos"));
    // O menu é também a porta de entrada para o gerenciador. Antes ele ficava
    // desabilitado quando nenhum plugin registrava ações, parecendo decorativo.
    m_pluginsMenu->addAction(HIcons::addons(), tr("Gerenciar complementos..."), this, [this] {
        OptionsDialog dlg(this, currentTab() ? &currentTab()->data() : nullptr);
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
            if (specToVk(QKeySequence::fromString(shortcut), vk, mods)) {
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
        // "Gerenciar complementos..." permanece disponível mesmo quando o
        // último plugin remove sua ação dinâmica.
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

    // ------------------------- barra de ferramentas ---------------------
    // A referência usa dois grupos compactos de botões arredondados, em vez
    // da antiga barra cheia de controles. As QAction continuam ligadas aos
    // mesmos fluxos para não alterar nenhuma função do cliente.
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
        dlg.selectPage(tr("Reprodução"));
        dlg.exec();
    });
    addButton(audioGroup, m_actMuteSpk, spkMenu);
    addButton(audioGroup, m_actScreenShare, nullptr);

    tb->addSeparator();

    QHBoxLayout* utilityGroup = makeGroup();
    addButton(utilityGroup, m_actOptions, nullptr);

    // O indicador de notificações fica isolado à direita, como na referência.
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

    // ------------------------- área central -----------------------------
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
    // A aba visual fica dentro do cartão da árvore, como na imagem de
    // referência; o QTabBar continua existindo para manter a troca programática
    // de conexões sem introduzir uma faixa extra no layout.
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
    });

    // menu de contexto nas abas (como no Halla)
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

    // ------------------------- barra de status --------------------------
    // três zonas, como no Halla:
    // [aba do servidor atual]  |  [linha de notícias]  |  [ícone + conexão + ping]
    // Fileira horizontal de servidores conectados (um botão por servidor).
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
            showNormal();
            raise();
            activateWindow();
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
                        showNormal();
                        raise();
                        activateWindow();
                    }
                });
    }

    // restaurar sessão anterior
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
    // Transparência da janela (Opções → Aparência)
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
    // Só cria o novo processo depois que esta janela, suas conexões e o loop
    // atual já terminaram. Isso impede duas instâncias visíveis durante a
    // confirmação de saída ou durante o áudio de desconexão.
    if (m_restartAfterClose) {
        QProcess::startDetached(QCoreApplication::applicationFilePath(),
                                QCoreApplication::arguments().mid(1));
    }
}

// ======================================================================
void MainWindow::connectTo(const QString& address, quint16 port, const QString& nickname,
                           const QString& password) {
    // Entrar sem nome não é permitido: o servidor oficial vem pré-salvo nos
    // favoritos com o apelido vazio justamente para o app perguntar aqui.
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
    // ID único da identidade PADRÃO (estável — gerado e persistido uma vez)
    QString uid;
    for (const QStringList& r : IdentityDialog::loadAll())
        if (r.value(0) == "1") { uid = r.value(3); break; }
    if (uid.isEmpty()) uid = IdentityDialog::loadAll().value(0).value(3);

    // A identidade precisa estar completa ANTES de conectar: sem a chave
    // pública o servidor rejeitaria com bad_identity, sem dizer o porquê.
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

    // falha de conexão / login recusado
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

    // login aceito: cria a aba do servidor
    connect(net, &NetSession::welcomeReceived, this, [this, net, address, port] {
        ServerTab* tab = new ServerTab(net->data(), m_tabs);
        ServerData& d = tab->data();
        // copia tudo que o NetSession já descarregou no welcome
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
        connect(net, &NetSession::disconnectedUnexpected, this, [this, tab, net] {
            if (!net->serverTerminatedSession())
                HSound::play(QStringLiteral("connection_lost"));
            tab->chat()->addServerSystem(tr("Desconectado do servidor."));
            disconnectTab(tab, false);
        });

        if (S::flag("notify/connectSound", true)) HSound::play(QStringLiteral("connected"));
        HSpeech::say(tr("Conectado ao servidor"));

        S::set("connect/nickname", net->data().users[net->data().selfId].name);
        // memoriza o apelido aceito por servidor: reconectar não perde mais
        S::ServerNicks::set(address, port, net->data().users[net->data().selfId].name);
        addRecent(address, port);
        saveSession();

        m_stack->setCurrentWidget(m_center);
        updateConnectionUi();
        updateStatusBar();
    });
}

// fiações comuns de uma aba (local ou de rede)
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
        // apelido renomeado em sessão: memoriza para este servidor
        connect(tab->net(), &NetSession::selfRenamed, this, [this, tab](const QString& name) {
            if (!tab->net()) return;
            S::ServerNicks::set(tab->net()->hostPort(), 0, name);
            S::set("connect/nickname", name);
        });
        connect(tab->net(), &NetSession::screenshareStateChanged, this, &MainWindow::handleScreenshareStateChanged);
        connect(tab->net(), &NetSession::screenshareFrameReceived, this, &MainWindow::handleScreenshareFrameReceived);
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
                        if (m_screenShareWindows.contains(userId)) m_screenShareWindows[userId]->updateImage(image);
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
                        // Cada live mantém fila própria: isso permite mutar uma
                        // transmissão sem afetar vozes, plugins ou outras lives.
                        active->voice()->playStreamPcm(
                            userId, reinterpret_cast<const int16_t*>(pcm.constData()),
                            uint32_t(frames), uint32_t(channels), 1.0f);
                    });
        }
        // A sessão WebRTC é única e sobrevive às abas: ao (re)conectar, aponte
        // para o NetSession vivo desta aba. Sem isto, m_net ficava pendente
        // para a sessão antiga (deletada) após reconectar, e a transmissão
        // crashava o app (use-after-free) ou não fazia nada (estado preso).
        m_webrtcSession->setNetSession(tab->net());
        connect(tab->net(), &NetSession::webRtcSignalReceived,
                m_webrtcSession, &HallaWebRtcSession::handleSignal);
    }
    connect(tab->tree(), &ServerTreeWidget::screenshareHovered, this, &MainWindow::handleScreenshareHovered);
}

// aba local offline (modo --demo para capturas de tela)
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

    // O aviso começa primeiro; a conexão só é encerrada após um segundo para
    // que o áudio não seja cortado junto com a aba/dispositivo de reprodução.
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
        // Encerra a transmissão/watch da sessão WebRTC ENQUANTO o NetSession
        // ainda existe (o stream_stop precisa dele) e solta o ponteiro antes
        // do deleteLater — evita use-after-free e broadcast preso no estado
        // "ligado" ao reconectar.
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

// Abre (ou reabre) o "Registro do cliente" — o diálogo carrega o histórico do
// halla.log, então mostra também decisões passadas (ex.: encoder GPU/CPU).
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
    Q_UNUSED(newTab); // cada conexão já abre em sua própria aba, como no Halla
    ConnectDialog dlg(this);
    dlg.setNickname(S::str("connect/nickname", IdentityDialog::defaultNickname()));
    if (dlg.exec() != QDialog::Accepted) return;
    connectTo(dlg.address(), dlg.port(), dlg.nickname(), dlg.password());
}

void MainWindow::openBookmarksDialog(const QString& prefillLabel, const QString& prefillAddr) {
    BookmarksDialog dlg(this);
    connect(&dlg, &BookmarksDialog::connectRequested, this,
            [this](const QString& a, quint16 p, const QString& n, const QString& pw) {
                // Apelido vazio no favorito + nada memorizado -> connectTo
                // pergunta o nome (não inventa um padrão).
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
    // remove entradas dinâmicas antigas (tudo após "Gerenciar favoritos...")
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
                                       // favorito com apelido próprio prevalece; sem um,
                                       // usa a memória do servidor — e vazio faz
                                       // connectTo perguntar o nome na hora
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
                                        // último apelido usado neste servidor; sem memória, global
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

// Atualizações, notificações e persistência de sessão ficam em MainWindowUpdates.cpp.


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
    // zona esquerda: fileira com um botão por servidor conectado (rebuild abaixo)
    rebuildServerButtons();

    // zona direita: estado da conexão + ping/perda
    if (t) {
        m_statusIcon->setPixmap(HIcons::connectPlug().pixmap(14, 14));
        m_statusText->setText(tr("Conectado como %1")
                                  .arg(t->data().users[t->data().selfId].name));
        if (NetSession* net = t->net()) {
            m_pingLabel->setText(tr("Ping: %1 ms   Perda de pacotes: %2%")
                                     .arg(net->pingMs()).arg(QStringLiteral("0,00")));
        } else {
            m_pingLabel->setText(tr("Ping: --"));
        }
    } else if (m_tabs->count() > 0) {
        m_statusIcon->setPixmap(HIcons::connectPlug().pixmap(14, 14));
        m_statusText->setText(tr("%1 conexões abertas").arg(m_tabs->count()));
        m_pingLabel->clear();
    } else {
        m_statusIcon->setPixmap(HIcons::disconnectPlug().pixmap(14, 14));
        m_statusText->setText(tr("Desconectado"));
        m_pingLabel->clear();
    }
}

// Reconstrói a fileira horizontal de servidores conectados na barra de status.
// Cada servidor conectado vira um botão clicável (nome ao lado dos demais) e o
// servidor atualmente selecionado fica destacado (negrito + cor de fundo).
void MainWindow::rebuildServerButtons() {
    if (!m_serverBar || !m_serverBarLayout) return;

    // Remove todos os botões anteriores.
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
            // Estilo de destaque do servidor selecionado (negrito + fundo).
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
            // Botão direito no servidor da barra de status: mesmo menu da
            // aba (Desconectar / Editar servidor virtual / Favoritos) —
            // captura a ABA (estável), não o índice (muda ao fechar abas).
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
        // Nenhum servidor conectado: mostra apenas o rótulo neutro.
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
    // tema centralizado: estilo + paleta + stylesheet global (funciona
    // também no Windows, onde stylesheet fixo ignorava a paleta)
    HTheme::apply();
    // força repintura dos widgets que pintam manualmente e dos banners
    if (m_welcome) m_welcome->update();
    for (int i = 0; i < m_tabs->count(); ++i) {
        if (ServerTab* t = qobject_cast<ServerTab*>(m_tabs->widget(i))) {
            t->info()->refresh();
            t->tree()->viewport()->update();
            t->chat()->update();
        }
    }
}

// executa uma ação configurada em "Teclas de atalho" (independente da origem:
// atalho local do Qt no Linux ou hotkey GLOBAL do sistema no Windows)
void MainWindow::runConfiguredAction(const QString& action) {
    if (action.contains(QStringLiteral("ussurr"), Qt::CaseInsensitive)) {
        // sussurro por TOGGLE (usado no Linux/atalhos de janela); no Windows
        // o comportamento principal é "segurar para falar" (ver applyHotkeys)
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
        S::set("capture/pttMode", m == 2 ? 1 : 2); // contínuo <-> detecção
        statusBar()->showMessage(m == 2 ? tr("Transmissão contínua desativada")
                                        : tr("Transmissão contínua ativada"), 3000);
    }
}

void MainWindow::applyHotkeys() {
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
#endif

    // lista do PERFIL ativo (migração da chave legada "hotkeys/list")
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

            // ---- Sussurrar foi removido das Teclas de Atalho gerais e agora é lido exclusivamente das Listas de Sussurros
            if (action.contains(QStringLiteral("ussurr"), Qt::CaseInsensitive)) {
                continue;
            }

            const QKeySequence seq = QKeySequence::fromString(keyStr);
            if (seq.isEmpty()) continue;
#ifdef Q_OS_WIN
            int mouseBtn = 0;
            if (keyStr == QLatin1String(HotkeyEdit::kMouse4))         mouseBtn = 4;
            else if (keyStr == QLatin1String(HotkeyEdit::kMouse5))    mouseBtn = 5;
            else if (keyStr == QLatin1String(HotkeyEdit::kMouseMiddle)) mouseBtn = 3;

            if (mouseBtn != 0) {
                MouseHotkey mh;
                mh.mouseBtn = mouseBtn;
                mh.action = action;
                mh.held = false;
                m_mouseHotkeys << mh;
            } else {
                // GLOBAL: funciona em segundo plano, como no Halla
                UINT vk = 0, mods = 0;
                if (specToVk(seq, vk, mods)) {
                    const int id = 100 + idx;
                    if (RegisterHotKey(HWND(winId()), id, mods | MOD_NOREPEAT, vk))
                        m_globalHotkeyActions[id] = action;
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

    // Carrega as teclas de atalho configuradas nas Listas de Sussurros (Sussurro -> Lista de Sussurros)
    QJsonDocument whisperDoc = QJsonDocument::fromJson(S::str("whispers").toUtf8());
    if (whisperDoc.isArray()) {
        for (const QJsonValue& v : whisperDoc.array()) {
            QJsonObject o = v.toObject();
            const QString listName = o["name"].toString();
            const QString keyStr = o["key"].toString();
            if (keyStr.isEmpty()) continue;

#ifdef Q_OS_WIN
            HoldKey hk;
            hk.scope = 2; // Lista de usuários
            hk.whisperListName = listName;
            if (keyStr == QLatin1String(HotkeyEdit::kMouse4))         hk.mouseBtn = 4;
            else if (keyStr == QLatin1String(HotkeyEdit::kMouse5))    hk.mouseBtn = 5;
            else if (keyStr == QLatin1String(HotkeyEdit::kMouseMiddle)) hk.mouseBtn = 3;
            else {
                UINT vk = 0, mods = 0;
                if (!specToVk(QKeySequence::fromString(keyStr), vk, mods)) continue;
                hk.vk = vk;
                hk.mods = mods & (MOD_ALT | MOD_CONTROL | MOD_SHIFT | MOD_WIN);
            }
            m_whisperHolds << hk;
#else
            const QKeySequence seq = QKeySequence::fromString(keyStr);
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
    // timer global de 50 ms: detecta pressão/soltura de PTT (qualquer origem)
    // e das teclas de sussurro — funciona com a janela em segundo plano,
    // pois lê o estado físico das teclas/botões via GetAsyncKeyState
    if (!m_pttPoll) {
        m_pttPoll = new QTimer(this);
        m_pttPoll->setInterval(50);
        connect(m_pttPoll, &QTimer::timeout, this, &MainWindow::pollGlobalInputs);
    }
    m_pttPoll->start();
#endif

    // (re)registra a tecla PTT global do sistema (Windows) — tecla OU mouse
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
            // A preferência de idioma fica salva para a próxima abertura, mas
            // não deve surgir outra instância se o usuário cancelou a saída.
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

// "Minimizar na bandeja" (Opções > Aparência > Ícone da bandeja)
void MainWindow::changeEvent(QEvent* e) {
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

// ======================================================================
// PTT global: RegisterHotKey (teclado) + Raw Input (botões do mouse)
// funcionam com a janela EM SEGUNDO PLANO (PTT global de voz)
// ======================================================================
#ifdef Q_OS_WIN
// converte QKeySequence (ex.: "Ctrl+F2", "Space") em VK + modificadores
static bool specToVkImpl(const QKeySequence& ks, UINT& vk, UINT& mods) {
    if (ks.isEmpty()) return false;
    const QKeyCombination comb = ks[0];
    const int k = comb.toCombined();
    const int key = k & ~int(Qt::KeyboardModifierMask);
    vk = 0;
    if (key >= Qt::Key_A && key <= Qt::Key_Z)      vk = UINT(key);
    else if (key >= Qt::Key_0 && key <= Qt::Key_9) vk = UINT(key);
    else if (key == Qt::Key_Space)     vk = VK_SPACE;
    else if (key == Qt::Key_Tab)       vk = VK_TAB;
    else if (key == Qt::Key_CapsLock)  vk = VK_CAPITAL;
    else if (key == Qt::Key_Return)    vk = VK_RETURN;
    else if (key == Qt::Key_Backspace) vk = VK_BACK;
    else if (key == Qt::Key_Insert)    vk = VK_INSERT;
    else if (key == Qt::Key_Delete)    vk = VK_DELETE;
    else if (key == Qt::Key_Home)      vk = VK_HOME;
    else if (key == Qt::Key_End)       vk = VK_END;
    else if (key == Qt::Key_PageUp)    vk = VK_PRIOR;
    else if (key == Qt::Key_PageDown)  vk = VK_NEXT;
    else if (key == Qt::Key_Print)     vk = VK_SNAPSHOT;
    else if (key == Qt::Key_Pause)     vk = VK_PAUSE;
    else if (key == Qt::Key_Left)      vk = VK_LEFT;
    else if (key == Qt::Key_Up)        vk = VK_UP;
    else if (key == Qt::Key_Right)     vk = VK_RIGHT;
    else if (key == Qt::Key_Down)      vk = VK_DOWN;
    else if (key >= Qt::Key_F1 && key <= Qt::Key_F24)
        vk = VK_F1 + UINT(key - Qt::Key_F1);
    mods = MOD_NOREPEAT;
    if (k & int(Qt::ShiftModifier))   mods |= MOD_SHIFT;
    if (k & int(Qt::ControlModifier)) mods |= MOD_CONTROL;
    if (k & int(Qt::AltModifier))     mods |= MOD_ALT;
    return vk != 0;
}
// visibilidade p/ applyHotkeys()
bool specToVk(const QKeySequence& ks, UINT& vk, UINT& mods) {
    return specToVkImpl(ks, vk, mods);
}
#endif

#ifdef Q_OS_WIN
// VKs candidatos de cada botão de mouse: cobre tanto o botão real (XButton)
// quanto o que softwares de mouse (Logitech/Razer/etc.) emitem no lugar
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

// timer de 50 ms: lê o estado FÍSICO do PTT e das teclas de sussurro.
// Detecta tanto a pressão quanto a soltura — inclusive em segundo plano —
// sem depender de qual janela recebeu a mensagem do mouse.
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

    // ---- sussurro (segurar para falar)
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

    if (held && !m_whisperHolds[idx].whisperListName.isEmpty()) {
        S::set("whisper/activeList", m_whisperHolds[idx].whisperListName);
    }

    t->setWhisperHold(held, m_whisperHolds[idx].scope);
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
            // botão de mouse (PTT/sussurro/atalhos) pressionado em qualquer janela —
            // caminho rápido; o timer de 50 ms cobre os demais casos
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
                    // O raw input do mouse chega para TODO movimento (não só
                    // botões): com RIDEV_INPUTSINK e um mouse de 1000 Hz eram
                    // ~1000 WM_INPUT/s, cada um despertando a GUI e rodando o
                    // poll completo. O estado só muda em evento de BOTÃO — o
                    // movimento fica para o timer de 50 ms.
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
            // caminho alternativo com a janela em foco (inclusive mouses
            // cujo software envia "Voltar/Avançar" do navegador)
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

    // ---- Sempre registre os dispositivos de Raw Input para o mouse para garantir que os botões do mouse e atalhos funcionem de forma ultra responsiva e global
    RAWINPUTDEVICE rid = {};
    rid.usUsagePage = 0x01;          // generic desktop
    rid.usUsage     = 0x02;          // mouse
    rid.dwFlags     = RIDEV_INPUTSINK; // recebe mesmo sem foco
    rid.hwndTarget  = HWND(winId());
    if (RegisterRawInputDevices(&rid, 1, sizeof(rid)))
        m_rawInputRegistered = true;

    // A tecla PTT só vira hotkey global no modo "pressionar para falar" (0).
    // Nos modos por voz (1, PADRÃO) e contínuo (2), registrar a tecla engolia
    // ela do sistema INTEIRO à toa: com o padrão Space, TODO usuário em modo
    // por voz perdia a barra de espaço em TODOS os aplicativos enquanto o
    // Halla estivesse aberto — era um dos motivadores dos relatos de
    // "teclado parou de funcionar".
    if (S::num("capture/pttMode", 1) != 0) return;

    const QString spec = S::str("capture/pttKey", QStringLiteral("Space"));
    if (spec.isEmpty()) return;

    // ---- botões do mouse: Raw Input (caminho rápido) + polling (garantido)
    int btn = 0;
    if (spec == QLatin1String(HotkeyEdit::kMouse4))        btn = 4;
    else if (spec == QLatin1String(HotkeyEdit::kMouse5))   btn = 5;
    else if (spec == QLatin1String(HotkeyEdit::kMouseMiddle)) btn = 3;
    if (btn != 0) {
        m_mousePttButton = btn; // o timer de 50 ms lê o estado físico do botão
        AppLog::info(tr("PTT global registrado no mouse: %1").arg(spec));
        return;
    }

    // ---- tecla: RegisterHotKey (caminho rápido) + polling de soltura/backup
    UINT vk = 0, mods = 0;
    const QKeySequence ks = QKeySequence::fromString(spec);
    if (!specToVkImpl(ks, vk, mods)) return;
    m_pttVk = vk;
    m_pttMods = mods & (MOD_ALT | MOD_CONTROL | MOD_SHIFT);
    if (RegisterHotKey(HWND(winId()), 1, mods, vk)) {
        m_pttRegistered = true;
        AppLog::info(tr("Tecla PTT global registrada: %1").arg(ks.toString()));
    } else {
        AppLog::info(tr("Não foi possível registrar a tecla PTT: %1").arg(ks.toString()));
    }
#endif
}

void MainWindow::unregisterPttHotkey() {
#ifdef Q_OS_WIN
    if (m_pttRegistered) {
        UnregisterHotKey(HWND(winId()), 1);
        m_pttRegistered = false;
    }
    m_mousePttButton = 0;
    m_pttVk = 0;
    m_pttMods = 0;
#endif
    pttSetHeld(false);
}

void MainWindow::pttSetHeld(bool held) {
    if (m_pttHeld == held) return;
    // só faz efeito no modo "Pressionar para falar" (Opções > Captura)
    if (S::num("capture/pttMode", 1) != 0) return;
    m_pttHeld = held;
    if (ServerTab* t = currentTab())
        if (VoiceEngine* v = t->voice()) v->setPttHeld(held);
}

// ======================================================================
void MainWindow::loadDemoState() {
    // Estado local usado pelas capturas: os nomes e proporções reproduzem
    // exatamente a composição fornecida pelo usuário.
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
                    connect(win, &QDialog::finished, this, [this, selfId]() { m_screenShareWindows.remove(selfId); });
                    win->show();
                }
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
        }
        if (m_screenShareTimer) {
            m_screenShareTimer->stop();
        }
        t->net()->sendScreenShareStop();
        m_actScreenShare->setIcon(HIcons::screenShare(false));

        handleScreenshareStateChanged(t->data().selfId, false);
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

    // Envia frames continuamente no FPS configurado. O antigo delta-skip por
    // thumbnail 16x16 economizava upload, mas em clientes Mobile a transmissão
    // aparentava ficar congelada quando mudanças pequenas/cursor não alteravam
    // a miniatura. A fluidez da transmissão tem prioridade aqui.
    m_keepAliveTicks = 0;
    m_prevThumbnail = QImage();

    // Screen share usa UDP sem retransmissão. Em 1920x1080 cada frame vira
    // centenas de datagramas; basta perder um chunk para o Mobile descartar o
    // frame e a transmissão aparentar ficar travada. Limitamos o stream enviado
    // para uma resolução mobile-friendly e qualidade moderada, mantendo fluidez.
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
        if (m_webrtcSession) m_webrtcSession->stopWatching(userId);
    });
    window->show();
}

void MainWindow::handleScreenshareStateChanged(int userId, bool on) {
    ServerTab* tab = currentTab();
    if (!tab) return;

    if (on) {
        // Estado "ao vivo" atualiza apenas a árvore/ícone. Uma transmissão
        // remota só abre depois do clique explícito em Assistir.
        if (userId != tab->data().selfId) return;
        // Preview local do WebRTC já é criado pelo fluxo de transmissão.
        if (m_webrtcSession && m_webrtcSession->isBroadcasting()) return;
        openScreenShareWindow(userId); // preview do modo JPEG legado
        return;
    }

    if (m_screenShareWindows.contains(userId)) {
        ScreenShareWindow* window = m_screenShareWindows.take(userId);
        window->close();
        window->deleteLater();
    }
}

void MainWindow::handleScreenshareFrameReceived(int userId, const QByteArray& jpegData) {
    m_lastScreenshareFrames[userId] = jpegData;
    if (m_screenShareWindows.contains(userId)) {
        m_screenShareWindows[userId]->updateFrame(jpegData);
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

    // Evita flicker: se já existe botão para o mesmo usuário, não recria.
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
        // O servidor rejeita watch_request para si mesmo. Para a própria live,
        // apenas abre/reabre a preview local; a transmissão continua controlada
        // pelo botão principal de transmitir.
        if (!m_screenShareWindows.contains(selfId)) {
            QString userName = t->data().users.contains(selfId) ? t->data().users[selfId].name : tr("Minha transmissão");
            ScreenShareWindow* win = new ScreenShareWindow(
                selfId, userName, false, ScreenShareWindow::AudioMuteCallback(), this);
            m_screenShareWindows[selfId] = win;
            connect(win, &QDialog::finished, this, [this, selfId]() { m_screenShareWindows.remove(selfId); });
            win->show();
        } else {
            m_screenShareWindows[selfId]->show();
            m_screenShareWindows[selfId]->raise();
            m_screenShareWindows[selfId]->activateWindow();
        }
        return;
    }

    int myChan = t->data().channelOfUser(t->data().selfId);
    if (myChan != channelId) {
        t->net()->moveToChannel(channelId);
    }

    openScreenShareWindow(userId);
    if (m_webrtcSession && m_webrtcSession->isNativeAvailable()) {
        m_webrtcSession->startWatching(userId);
    }
}

// -- restauração de sessão usa a rede (já era) ---------------------------
