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

// Janela principal do Halla — reproduz fielmente a janela clássica do
// tema claro clássico: barra de menus (Conexões, Marcadores, Si mesmo,
// Permissões, Ferramentas, Ajuda), barra de ferramentas com setas suspensas,
// corpo 50/50 (árvore | informações) com chat embaixo e barra de status
// em três zonas (servidor | notícias | conexão).
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    // abre uma conexão REAL com um Halla Server (rede TCP+UDP).
    // Usado pelo diálogo Conectar, favoritos, conexões recentes e --auto-connect.
    void connectTo(const QString& address, quint16 port, const QString& nickname,
                   const QString& password = QString());

    // cria uma aba local (offline, usada apenas no modo --demo de capturas)
    void createLocalTab(const ServerData& initial);

    void loadDemoState(); // usado apenas pelo modo --demo (capturas de tela)

    // ---- Compartilhamento de Tela (screenshare)
    void toggleScreenShare();
    void captureAndSendScreen();
    void handleScreenshareStateChanged(int userId, bool on);
    void openScreenShareWindow(int userId);
    void handleScreenshareFrameReceived(int userId, const QByteArray& jpegData);
    void handleScreenshareHovered(int userId, int channelId, const QPoint& pos);
    void watchStream(int userId, int channelId);

protected:
    void closeEvent(QCloseEvent* e) override;
    void changeEvent(QEvent* e) override; // minimizar para a bandeja
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
    void publishPluginState();
    void saveSession();
    void openLogDialog();  // abre o "Registro do cliente" (log) — de Ajuda e Ferramentas
    void rebuildServerButtons();  // fileira horizontal de servidores conectados na barra de status

    QStackedWidget* m_stack = nullptr;
    WelcomePage* m_welcome = nullptr;
    QSplitter* m_center = nullptr;
    QTabWidget* m_tabs = nullptr;

    QSystemTrayIcon* m_tray = nullptr;
    LogDialog* m_log = nullptr;

    // ações da barra de ferramentas/menus (alternam com o estado da conexão)
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

    // barra de status em 3 zonas (servidores | notícias | conexão), como no Halla.
    // m_serverBar é uma fileira horizontal com um botão por servidor conectado.
    QWidget* m_serverBar = nullptr;
    QHBoxLayout* m_serverBarLayout = nullptr;
    QLabel* m_newsLabel = nullptr;
    QLabel* m_statusIcon = nullptr;
    QLabel* m_statusText = nullptr;
    QLabel* m_pingLabel = nullptr;

    QSet<ServerTab*> m_disconnectingTabs;
    bool m_closeDelayPending = false;
    bool m_closingAfterSound = false;
    bool m_restartAfterClose = false;

    void wireTab(ServerTab* tab);

    // ---- PTT global (hotkey de todo o sistema no Windows — tecla OU mouse)
    void registerPttHotkey();
    void unregisterPttHotkey();
    void pttSetHeld(bool held);
    void runConfiguredAction(const QString& action); // ação das "Teclas de atalho"
    unsigned int m_pttVk = 0;
    unsigned int m_pttMods = 0;
    bool m_pttRegistered = false;
    bool m_pttHeld = false;
    int  m_mousePttButton = 0;        // 0=tecla, 3=meio, 4/5=laterais
    bool m_rawInputRegistered = false;
    class QTimer* m_pttPoll = nullptr;

    // ---- v3.11: sussurro por tecla de atalho (segurar p/ falar, como no Halla)
    struct HoldKey {
        unsigned int vk = 0;      // tecla (0 = usa mouseBtn)
        unsigned int mods = 0;    // MOD_CONTROL/SHIFT/ALT
        int  mouseBtn = 0;        // 3 = meio, 4/5 = laterais
        int  scope = 1;           // 0 canal, 1 canal+subcanais, 2 usuários
        bool held = false;
        QString whisperListName;  // nome da lista de sussurros (opcional)
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
    bool m_whisperToggleOn = false;          // alternância (atalho sem "segurar")
    void whisperSetHeld(int idx, bool held);
    void pollGlobalInputs();                  // timer de 50 ms (pressões/solturas)

    QList<QPointer<QShortcut>> m_hotkeyShortcuts;
    QMap<int, QString> m_globalHotkeyActions; // id (100+) -> ação (Windows)
};
