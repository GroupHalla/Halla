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

    // abre uma conexão (cria a aba do servidor). Usado pelo diálogo Conectar,
    // favoritos, conexões recentes e restauração de sessão.
    void connectTo(const QString& address, quint16 port, const QString& nickname,
                   const QString& password = QString());

    void loadDemoState(); // usado apenas pelo modo --demo (capturas de tela)

protected:
    void closeEvent(QCloseEvent* e) override;
    bool eventFilter(QObject* obj, QEvent* ev) override;

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
    QAction* m_actBookmarkAdd = nullptr;
    QAction* m_actPrivilegeKey = nullptr;
    QAction* m_actServerGroups = nullptr;

    QMenu* m_bookmarksMenu = nullptr;
    QMenu* m_recentMenu = nullptr;

    QLabel* m_statusIcon = nullptr;
    QLabel* m_statusText = nullptr;
    QLabel* m_pingLabel = nullptr;

    QList<QPointer<QShortcut>> m_hotkeyShortcuts;
};
