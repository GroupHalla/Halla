#pragma once

#include <QMainWindow>
#include <QPointer>
#include <QSet>
#include "core/Models.h"

class QTabWidget;
class QStackedWidget;
class QSplitter;
class QSystemTrayIcon;
class QShortcut;
class QLabel;
class QToolButton;
class QHBoxLayout;
class ServerTab;
class WelcomePage;
class LogDialog;
class HallaWebRtcSession;

// Halla's main window -- faithfully reproduces the classic window of the classic
// light theme: menu bar (Connections, Bookmarks, Self, Permissions, Tools, Help),
// toolbar with drop-down arrows, 50/50 body (tree | information) with the chat
// below and a three-zone status bar (server | news | connection).
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    // opens a REAL connection to a Halla Server (TCP+UDP network). Used by the
    // Connect dialog, bookmarks, recent connections and --auto-connect.
    void connectTo(const QString& address, quint16 port, const QString& nickname,
                   const QString& password = QString());

    // creates a local tab (offline, used only by the --demo screenshot mode)
    void createLocalTab(const ServerData& initial);

    void loadDemoState(); // used only by --demo mode (screenshots)

    // ---- Compartilhamento de Tela (screenshare)
    void toggleScreenShare();
    void captureAndSendScreen();
    void handleScreenshareStateChanged(int userId, bool on, const QString& mode);
    void openScreenShareWindow(int userId);
    void handleScreenshareFrameReceived(int userId, const QByteArray& jpegData);
    void handleScreenshareHovered(int userId, int channelId, const QPoint& pos);
    void watchStream(int userId, int channelId);

protected:
    void closeEvent(QCloseEvent* e) override;
    void changeEvent(QEvent* e) override; // minimizar para a bandeja
    void hideEvent(QHideEvent* e) override; // remember which screen the window was on
    bool eventFilter(QObject* obj, QEvent* ev) override;
    bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result) override;

private:
    ServerTab* currentTab() const;
    void disconnectTab(ServerTab* tab, bool notify = true);
    void disconnectAllTabs(bool notify = true);
    void finishDisconnectTab(ServerTab* tab);
    void rebuildBookmarksMenu();
    void rebuildRecentMenu();
    void addRecent(const QString& address, quint16 port);
    void openConnectDialog(bool newTab = false);
    void openBookmarksDialog(const QString& prefillLabel = QString(),
                             const QString& prefillAddr = QString());
    void checkUpdates(bool manual = false);
    void downloadAndInstallUpdate(const QString& url, const QString& checksumUrl,
                                  const QString& version);
    void showNotifications();
    void applyTheme();
    void applyHotkeys();
    void updateConnectionUi();
    void updateStatusBar();
    void updateWebRtcPreviewState(); // broadcast preview only with the window open
    void publishPluginState();
    void saveSession();
    void openLogDialog();  // opens the "Client log" -- from Help and Tools
    void rebuildServerButtons();  // fileira horizontal de servidores conectados na barra de status

    QStackedWidget* m_stack = nullptr;
    WelcomePage* m_welcome = nullptr;
    QSplitter* m_center = nullptr;
    QTabWidget* m_tabs = nullptr;

    QSystemTrayIcon* m_tray = nullptr;
    LogDialog* m_log = nullptr;

    // toolbar/menu actions (toggle with the connection state)
    QAction* m_actConnect = nullptr;
    QAction* m_actDisconnect = nullptr;
    QAction* m_actDisconnectAll = nullptr;
    QAction* m_actAway = nullptr;
    QAction* m_actMuteMic = nullptr;
    QAction* m_actMuteSpk = nullptr;
    QAction* m_actScreenShare = nullptr;
    QTimer*  m_screenShareTimer = nullptr;
    quint16  m_screenShareSeq = 0;
    int      m_screenShareSourceType = 0;
    quintptr m_screenShareSourceId = 0;
    QImage   m_prevThumbnail;
    int      m_keepAliveTicks = 0;
    QMap<int, class ScreenShareWindow*> m_screenShareWindows;
    QMap<int, QByteArray> m_lastScreenshareFrames;
    HallaWebRtcSession* m_webrtcSession = nullptr;
    // ---- v1.1.28: assistance for broadcasts that never connect ------------
    // The watch request (webrtc_watch_request) may only be sent AFTER the server
    // has moved us into the transmitter's channel -- before that it answered
    // "webrtc_channel" and the window stayed forever on "Waiting for broadcast...".
    // m_pendingWatch holds the request while moveToChannel is unconfirmed (local
    // poll + timeout).
    struct PendingWatch {
        int userId = 0;
        int channelId = 0;
    };
    PendingWatch m_pendingWatch;
    QPointer<ServerTab> m_pendingWatchTab;  // aba onde o watch nasceu
    QTimer* m_pendingWatchPoll = nullptr;   // 150 ms until the channel join (max 6 s)
    int m_pendingWatchTicks = 0;            // voltas do poll (6 s / 150 ms)
    // Per-user watchdog: no first frame within 12 s => visible error with a "Retry"
    // button (failed ICE behind CGNAT without TURN, lost watch, etc. -- previously
    // absolute silence).
    QMap<int, QTimer*> m_watchNoFrameTimers;
    void beginWatching(ServerTab* t, int userId);  // opens the window + requests the watch
    void armWatchdog(int userId);                  // arma o watchdog de 12 s
    void cancelWatchdog(int userId);               // 1º frame chegou — tudo bem
    void markWatchAlive(int userId);               // frame: cancela + limpa erro
    void handleWatchServerError(const QString& code, const QString& msg);
    QAction* m_actRecord = nullptr;
    QAction* m_actWhisper = nullptr;
    QAction* m_actBookmarkAdd = nullptr;
    QAction* m_actPrivilegeKey = nullptr;
    QAction* m_actServerGroups = nullptr;
    QAction* m_actBanList = nullptr;
    QAction* m_actComplaints = nullptr;
    QAction* m_actMyPerms = nullptr;
    QAction* m_actOptions = nullptr;
    QAction* m_actRenameSelf = nullptr;
    QAction* m_actCommander = nullptr;

    QMenu* m_bookmarksMenu = nullptr;
    QMenu* m_recentMenu = nullptr;
    QMenu* m_pluginsMenu = nullptr;
    QMap<QString, QAction*> m_pluginActions;
    QMap<QString, int> m_pluginHotkeyIds;
    QMap<int, QPair<QString, QString>> m_pluginGlobalHotkeys;
    int m_nextPluginHotkeyId = 10000;

    // three-zone status bar (servers | news | connection), as in Halla. m_serverBar
    // is a horizontal row with one button per connected server.
    QWidget* m_serverBar = nullptr;
    QHBoxLayout* m_serverBarLayout = nullptr;
    QLabel* m_newsLabel = nullptr;
    QLabel* m_statusIcon = nullptr;
    QLabel* m_statusText = nullptr;
    QLabel* m_pingLabel = nullptr;
    QLabel* m_e2eeLabel = nullptr;   // v6: cadeado (chaves de grupo prontas)

    QSet<ServerTab*> m_disconnectingTabs;
    bool m_closeDelayPending = false;
    bool m_closingAfterSound = false;
    bool m_restartAfterClose = false;

    // ---- v1.1.31: restore on the SAME screen after the tray -----------------
    // User report: with the app on screen 2, minimizing and coming back made the
    // window reappear on screen 1. Windows discards the placement when the window
    // is hidden while minimized (the hide() of the tray path), and a bare
    // showNormal() returns it on the primary screen. We store the "normal"
    // rectangle + the maximized state before hiding and reapply them on return --
    // this also covers re-maximizing on the right screen.
    QRect m_geometryBeforeHide;
    bool m_wasMaximizedBeforeHide = false;
    void savePlacementBeforeHide();
    void showFromTray();

    void wireTab(ServerTab* tab);

    // ---- global PTT (system-wide hotkey on Windows -- key OR mouse)
    void registerPttHotkey();
    void pttSetHeld(bool held);
    void runConfiguredAction(const QString& action); // "Hotkeys" action
    unsigned int m_pttVk = 0;
    unsigned int m_pttMods = 0;
    bool m_pttRegistered = false;
    bool m_pttHeld = false;
    int  m_mousePttButton = 0;        // 0=tecla, 3=meio, 4/5=laterais
    bool m_rawInputRegistered = false;
    class QTimer* m_pttPoll = nullptr;

    // ---- v3.11: whisper by shortcut key (hold to talk, as in Halla)
    struct HoldKey {
        unsigned int vk = 0;      // tecla (0 = usa mouseBtn)
        unsigned int mods = 0;    // MOD_CONTROL/SHIFT/ALT
        int  mouseBtn = 0;        // 3 = meio, 4/5 = laterais
        int  scope = 1;           // 0 channel, 1 channel+subchannels, 2 users
        bool held = false;
        bool reply = false;       // reply key: target = whoever whispered last
        QString whisperListName;  // whisper list name (optional)
    };
    struct MouseHotkey {
        int mouseBtn = 0; // 3, 4, 5
        QString action;
        bool held = false;
    };
    QList<HoldKey> m_whisperHolds;
    QList<MouseHotkey> m_mouseHotkeys;
    bool m_mouseButtonState[6] = { false };
    bool isMouseDown(int btn);
    bool m_whisperToggleOn = false;          // toggle (shortcut without "hold")
    void whisperSetHeld(int idx, bool held);
    void pollGlobalInputs();                  // 50 ms timer (presses/releases)

    QList<QPointer<QShortcut>> m_hotkeyShortcuts;
    QMap<int, QString> m_globalHotkeyActions; // id (100+) -> action (Windows)
};
