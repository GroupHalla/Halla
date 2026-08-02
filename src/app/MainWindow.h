#pragma once

#include <QMainWindow>
#include <QPointer>
#include "core/Models.h"

class QTabWidget;
class QStackedWidget;
class QSplitter;
class QSystemTrayIcon;
class QShortcut;
class QLabel;
class ServerTab;
class WelcomePage;
class InfoPanel;
class LogDialog;

// Janela principal do Halla — reproduz fielmente a janela do TeamSpeak 3:
// barra de menus (Conexões, Favoritos, Permissões, Ferramentas, Ajuda),
// barra de ferramentas com plugues de conexão, abas de servidor,
// painel de informações à direita e barra de status com ping.
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);

    // abre uma conexão REAL com um Halla Server (rede TCP+UDP).
    // Usado pelo diálogo Conectar, favoritos, conexões recentes e --auto-connect.
    void connectTo(const QString& address, quint16 port, const QString& nickname,
                   const QString& password = QString());

    // cria uma aba local (offline, usada apenas no modo --demo de capturas)
    void createLocalTab(const ServerData& initial);

    void loadDemoState(); // usado apenas pelo modo --demo (capturas de tela)

protected:
    void closeEvent(QCloseEvent* e) override;
    bool eventFilter(QObject* obj, QEvent* ev) override;
    bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result) override;

private:
    ServerTab* currentTab() const;
    void disconnectTab(ServerTab* tab, bool notify = true);
    void rebuildBookmarksMenu();
    void rebuildRecentMenu();
    void addRecent(const QString& address, quint16 port);
    void openConnectDialog(bool newTab = false);
    void openBookmarksDialog(const QString& prefillLabel = QString(),
                             const QString& prefillAddr = QString());
    void checkUpdates();
    void showNotifications();
    void applyTheme();
    void applyHotkeys();
    void updateConnectionUi();
    void updateStatusBar();
    void saveSession();

    QStackedWidget* m_stack = nullptr;
    WelcomePage* m_welcome = nullptr;
    QSplitter* m_center = nullptr;
    QTabWidget* m_tabs = nullptr;
    InfoPanel* m_info = nullptr;

    QSystemTrayIcon* m_tray = nullptr;
    LogDialog* m_log = nullptr;

    // ações da barra de ferramentas/menus (alternam com o estado da conexão)
    QAction* m_actConnect = nullptr;
    QAction* m_actDisconnect = nullptr;
    QAction* m_actDisconnectAll = nullptr;
    QAction* m_actAway = nullptr;
    QAction* m_actMuteMic = nullptr;
    QAction* m_actMuteSpk = nullptr;
    QAction* m_actRecord = nullptr;
    QAction* m_actWhisper = nullptr;
    QAction* m_actBookmarkAdd = nullptr;
    QAction* m_actPrivilegeKey = nullptr;
    QAction* m_actServerGroups = nullptr;
    QAction* m_actBanList = nullptr;
    QAction* m_actComplaints = nullptr;
    QAction* m_actMyPerms = nullptr;

    QMenu* m_bookmarksMenu = nullptr;
    QMenu* m_recentMenu = nullptr;

    QLabel* m_statusIcon = nullptr;
    QLabel* m_statusText = nullptr;
    QLabel* m_pingLabel = nullptr;

    void wireTab(ServerTab* tab);

    // ---- PTT global (hotkey de todo o sistema no Windows — tecla OU mouse)
    void registerPttHotkey();
    void unregisterPttHotkey();
    void pttSetHeld(bool held);
    void runConfiguredAction(const QString& action); // ação das "Teclas de atalho"
    unsigned int m_pttVk = 0;
    bool m_pttRegistered = false;
    bool m_pttHeld = false;
    int  m_mousePttButton = 0;        // 0=tecla, 3=meio, 4/5=laterais
    bool m_rawInputRegistered = false;
    class QTimer* m_pttPoll = nullptr;

    QList<QPointer<QShortcut>> m_hotkeyShortcuts;
    QMap<int, QString> m_globalHotkeyActions; // id (100+) -> ação (Windows)
};
