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
#include "core/AppLog.h"
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
#include "net/VoiceEngine.h"
#include "SoundPack.h"
#include "version.h"

#include <QMenuBar>
#include <QToolBar>
#include <QStatusBar>
#include <QTabBar>
#include <QStackedWidget>
#include <QSplitter>
#include <QSystemTrayIcon>
#include <QMessageBox>
#include <QCloseEvent>
#include <QApplication>
#include <QShortcut>
#include <QLabel>
#include <QToolButton>
#include <QTimer>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QRandomGenerator>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle(QString::fromUtf8(halla::kAppName));
    setWindowIcon(QIcon(HIcons::appIcon(64)));
    resize(1180, 760);

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
                                          [this] {
                                              while (m_tabs->count() > 0)
                                                  disconnectTab(qobject_cast<ServerTab*>(
                                                                    m_tabs->widget(0)), false);
                                          });
    m_recentMenu = mConn->addMenu(tr("Conexões recentes"));
    mConn->addSeparator();
    mConn->addAction(tr("Sair"), this, &MainWindow::close)
        ->setShortcut(QKeySequence(QStringLiteral("Ctrl+Q")));

    m_bookmarksMenu = menuBar()->addMenu(tr("Fa&voritos"));
    m_actBookmarkAdd = m_bookmarksMenu->addAction(
        HIcons::bookmarkStar(), tr("Adicionar aos favoritos..."), this,
        [this] {
            ServerTab* t = currentTab();
            openBookmarksDialog(t ? t->data().name : QString(),
                                t ? t->data().address : QString());
        });
    m_bookmarksMenu->addAction(tr("Gerenciar favoritos..."), this,
                               [this] { openBookmarksDialog(); });
    // entradas dinâmicas são inseridas por rebuildBookmarksMenu()

    QMenu* mPerm = menuBar()->addMenu(tr("&Permissões"));
    m_actPrivilegeKey = mPerm->addAction(HIcons::key(), tr("Usar chave de privilégio..."), this,
                                         [this] {
                                             ServerTab* t = currentTab();
                                             if (!t) return;
                                             PrivilegeKeyDialog dlg(this);
                                             if (dlg.exec() == QDialog::Accepted &&
                                                 !dlg.key().isEmpty()) {
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
                                             m_info->refresh();
                                         });
    m_actMyPerms = mPerm->addAction(tr("Mostrar permissões do usuário..."), this,
                     [this] {
                         ServerTab* t = currentTab();
                         if (!t) return;
                         if (t->isNetworked()) {
                             const ServerData& d = t->data();
                             PermissionsOverviewDialog dlg(
                                 t->net()->myPerms(),
                                 d.users.value(d.selfId).serverGroups, this);
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
                      [this] {
                          if (!m_log) m_log = new LogDialog(this);
                          m_log->show();
                          m_log->raise();
                          m_log->activateWindow();
                      });
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

    // ---- sussurro (voz direcionada a usuários específicos)
    m_actWhisper = mTools->addAction(tr("Ativar sussurro"), this, [this](bool on) {
        ServerTab* t = currentTab();
        if (!t) return;
        if (on) {
            const QStringList uids = WhisperDialog::activeWhisperUids();
            if (uids.isEmpty()) {
                m_actWhisper->setChecked(false);
                QMessageBox::information(this, tr("Sussurro"),
                    tr("Configure uma lista em \"Listas de sussurro...\" e clique "
                       "em \"Usar esta lista\"."));
                return;
            }
            t->setWhisperUids(uids);
        } else {
            t->setWhisperUids({});
        }
    });
    m_actWhisper->setCheckable(true);
    mTools->addAction(tr("Listas de sussurro..."), this,
                      [this] {
                          ServerTab* t = currentTab();
                          WhisperDialog dlg(t ? &t->data() : nullptr, this);
                          dlg.exec();
                      });
    mTools->addSeparator();
    mTools->addAction(HIcons::optionsGear(), tr("Opções..."), this,
                      [this] {
                          OptionsDialog dlg(this);
                          connect(&dlg, &OptionsDialog::themeChanged, this,
                                  &MainWindow::applyTheme);
                          connect(&dlg, &OptionsDialog::designChanged, this, [this] {
                              for (int i = 0; i < m_tabs->count(); ++i)
                                  if (ServerTab* t = qobject_cast<ServerTab*>(m_tabs->widget(i)))
                                      t->applyDisplayOptions();
                          });
                          connect(&dlg, &OptionsDialog::hotkeysChanged, this,
                                  &MainWindow::applyHotkeys);
                          dlg.exec();
                      })->setShortcut(QKeySequence(QStringLiteral("Alt+P")));

    QMenu* mHelp = menuBar()->addMenu(tr("A&juda"));
    mHelp->addAction(tr("Sobre o Halla"), this,
                     [this] { AboutDialog dlg(this); dlg.exec(); });
    mHelp->addSeparator();
    mHelp->addAction(tr("Verificar atualizações"), this, [this] { checkUpdates(); });

    // ------------------------- barra de ferramentas ---------------------
    QToolBar* tb = addToolBar(tr("Principal"));
    tb->setObjectName(QStringLiteral("mainToolBar"));
    tb->setMovable(false);
    tb->setIconSize(QSize(18, 18));
    tb->setToolButtonStyle(Qt::ToolButtonIconOnly);

    tb->addAction(m_actConnect);
    tb->addAction(m_actDisconnect);
    tb->addSeparator();
    tb->addAction(m_actBookmarkAdd);
    tb->addSeparator();

    m_actAway = tb->addAction(HIcons::away(false), tr("Ausente"), this, [this] {
        if (ServerTab* t = currentTab()) t->setAway(m_actAway->isChecked());
        updateConnectionUi();
    });
    m_actAway->setCheckable(true);
    m_actAway->setToolTip(tr("Definir como ausente"));

    m_actMuteMic = tb->addAction(HIcons::muteMic(false), tr("Mudo (microfone)"), this, [this] {
        if (ServerTab* t = currentTab()) t->setMicMuted(m_actMuteMic->isChecked());
        updateConnectionUi();
    });
    m_actMuteMic->setCheckable(true);

    m_actMuteSpk = tb->addAction(HIcons::muteSpeaker(false), tr("Mudo (alto-falantes)"), this,
                                 [this] {
                                     if (ServerTab* t = currentTab())
                                         t->setSpeakersMuted(m_actMuteSpk->isChecked());
                                     updateConnectionUi();
                                 });
    m_actMuteSpk->setCheckable(true);

    tb->addAction(m_actRecord);
    m_actRecord->setToolTip(tr("Iniciar/parar gravação (arquivo WAV local)"));

    tb->addSeparator();
    QToolButton* logBtn = new QToolButton(tb);
    logBtn->setIcon(HIcons::logPage());
    logBtn->setToolTip(tr("Registro do cliente"));
    tb->addWidget(logBtn);
    connect(logBtn, &QToolButton::clicked, this, [this] {
        if (!m_log) m_log = new LogDialog(this);
        m_log->show();
    });

    QToolButton* optBtn = new QToolButton(tb);
    optBtn->setIcon(HIcons::optionsGear());
    optBtn->setToolTip(tr("Opções"));
    tb->addWidget(optBtn);

    QWidget* spacer = new QWidget(tb);
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    tb->addWidget(spacer);

    QToolButton* bellBtn = new QToolButton(tb);
    bellBtn->setIcon(HIcons::bell());
    bellBtn->setToolTip(tr("Notificações"));
    tb->addWidget(bellBtn);
    connect(bellBtn, &QToolButton::clicked, this, [this] { showNotifications(); });

    connect(optBtn, &QToolButton::clicked, this, [this] {
        menuBar()->actions().at(3)->menu()->actions().last()->trigger();
    });

    // ------------------------- área central -----------------------------
    m_stack = new QStackedWidget(this);

    m_welcome = new WelcomePage(m_stack);
    m_stack->addWidget(m_welcome);
    connect(m_welcome, &WelcomePage::connectRequested, this,
            [this] { openConnectDialog(false); });
    connect(m_welcome, &WelcomePage::bookmarksRequested, this,
            [this] { openBookmarksDialog(); });

    m_center = new QSplitter(Qt::Horizontal, m_stack);
    m_center->setChildrenCollapsible(false);

    m_tabs = new QTabWidget(m_center);
    m_tabs->setTabsClosable(true);
    m_tabs->setMovable(true);
    m_tabs->setDocumentMode(false);
    m_center->addWidget(m_tabs);

    m_info = new InfoPanel(m_center);
    m_info->setMinimumWidth(240);
    m_center->addWidget(m_info);
    m_center->setStretchFactor(0, 1);
    m_center->setStretchFactor(1, 0);
    m_center->setSizes({ 860, 320 });

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
        if (ServerTab* t = currentTab()) {
            m_info->setData(&t->data());
            m_info->setSelection(t->tree()->currentKind(), t->tree()->currentId());
        }
    });

    // menu de contexto nas abas (como no TS3)
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
                menu.addAction(HIcons::bookmarkStar(), tr("Adicionar aos favoritos"), this,
                               [this, t] {
                                   openBookmarksDialog(t->data().name, t->data().address);
                               });
                menu.exec(m_tabs->tabBar()->mapToGlobal(pos));
            });

    // ------------------------- barra de status --------------------------
    m_statusIcon = new QLabel(this);
    m_statusIcon->setPixmap(HIcons::disconnectPlug().pixmap(14, 14));
    m_statusText = new QLabel(tr("Não conectado"), this);
    statusBar()->addWidget(m_statusIcon, 0);
    statusBar()->addWidget(m_statusText, 1);
    m_pingLabel = new QLabel(QString(), this);
    statusBar()->addPermanentWidget(m_pingLabel, 0);

    QTimer* pingTimer = new QTimer(this);
    pingTimer->setInterval(3000);
    connect(pingTimer, &QTimer::timeout, this, &MainWindow::updateStatusBar);
    pingTimer->start();

    // ------------------------- bandeja do sistema ------------------------
    if (QSystemTrayIcon::isSystemTrayAvailable()) {
        m_tray = new QSystemTrayIcon(QIcon(HIcons::appIcon(32)), this);
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

    AppLog::info(tr("Halla %1 iniciado").arg(QString::fromUtf8(halla::kAppVersion)));
}

// ======================================================================
void MainWindow::connectTo(const QString& address, quint16 port, const QString& nickname,
                           const QString& password) {
    const QString nick = nickname.isEmpty() ? IdentityDialog::defaultNickname() : nickname;
    // ID único da identidade PADRÃO (estável — gerado e persistido uma vez)
    QString uid;
    for (const QStringList& r : IdentityDialog::loadAll())
        if (r.value(0) == "1") { uid = r.value(3); break; }
    if (uid.isEmpty()) uid = IdentityDialog::loadAll().value(0).value(3);

    NetSession* net = new NetSession(this);
    net->connectToServer(address, port, nick, uid, password);

    // falha de conexão / login recusado
    connect(net, &NetSession::connectionFailed, this, [this, net, address, port](const QString& reason) {
        QMessageBox::warning(this, tr("Erro ao conectar"),
                             tr("<b>Falha ao conectar ao servidor %1:%2</b><br>%3")
                                 .arg(address).arg(port).arg(reason.toHtmlEscaped()));
        net->deleteLater();
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
        connect(net, &NetSession::disconnectedUnexpected, this, [this, tab] {
            tab->chat()->addServerSystem(tr("Desconectado do servidor."));
            disconnectTab(tab, false);
        });

        if (S::flag("notify/connectSound", true)) HSound::play(QStringLiteral("connected"));

        m_info->setData(&tab->data());
        m_info->setSelection(0, 0);

        S::set("connect/nickname", net->data().users[net->data().selfId].name);
        addRecent(address, port);
        saveSession();

        m_stack->setCurrentWidget(m_center);
        updateConnectionUi();
        updateStatusBar();
    });
}

// fiações comuns de uma aba (local ou de rede)
void MainWindow::wireTab(ServerTab* tab) {
    connect(tab, &ServerTab::disconnectRequested, this, [this, tab] { disconnectTab(tab); });
    connect(tab, &ServerTab::addBookmarkRequested, this, [this, tab] {
        openBookmarksDialog(tab->data().name, tab->data().address);
    });
    connect(tab, &ServerTab::titleChanged, this, [this, tab] {
        int i = m_tabs->indexOf(tab);
        if (i >= 0) m_tabs->setTabText(i, tab->tabTitle());
    });
    connect(tab, &ServerTab::selectionChanged, this, [this, tab](int kind, int id) {
        if (m_tabs->currentWidget() == tab) {
            m_info->setData(&tab->data());
            m_info->setSelection(kind, id);
        }
    });
    connect(tab, &ServerTab::statusChanged, this, [this, tab] {
        if (m_tabs->currentWidget() == tab) {
            updateConnectionUi();
            updateStatusBar();
            m_info->refresh();
        }
    });
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
    m_info->setData(&tab->data());
    m_info->setSelection(0, 0);
    m_stack->setCurrentWidget(m_center);
    updateConnectionUi();
    updateStatusBar();
}

void MainWindow::disconnectTab(ServerTab* tab, bool notify) {
    if (!tab) return;
    if (tab->net()) tab->net()->quit();
    const int idx = m_tabs->indexOf(tab);
    const QString addr = tab->data().address;
    m_tabs->removeTab(idx);
    tab->deleteLater();

    AppLog::info(tr("Desconectado de %1").arg(addr));
    if (notify && S::flag("notify/disconnectSound", true))
        HSound::play(QStringLiteral("disconnected"));

    saveSession();
    if (m_tabs->count() == 0) {
        m_stack->setCurrentWidget(m_welcome);
        m_info->setData(nullptr);
    }
    updateConnectionUi();
    updateStatusBar();
}

ServerTab* MainWindow::currentTab() const {
    return qobject_cast<ServerTab*>(m_tabs->currentWidget());
}

// ======================================================================
void MainWindow::openConnectDialog(bool newTab) {
    Q_UNUSED(newTab); // cada conexão já abre em sua própria aba, como no TS3
    ConnectDialog dlg(this);
    dlg.setNickname(S::str("connect/nickname", IdentityDialog::defaultNickname()));
    if (dlg.exec() != QDialog::Accepted) return;
    connectTo(dlg.address(), dlg.port(), dlg.nickname(), dlg.password());
}

void MainWindow::openBookmarksDialog(const QString& prefillLabel, const QString& prefillAddr) {
    BookmarksDialog dlg(this);
    connect(&dlg, &BookmarksDialog::connectRequested, this,
            [this](const QString& a, quint16 p, const QString& n, const QString& pw) {
                connectTo(a, p, n, pw);
            });
    connect(&dlg, &BookmarksDialog::changed, this, [this] { rebuildBookmarksMenu(); });
    if (!prefillAddr.isEmpty())
        dlg.prefill(prefillLabel.isEmpty() ? prefillAddr : prefillLabel, prefillAddr, 9987,
                    S::str("connect/nickname", IdentityDialog::defaultNickname()));
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
                                   [this, b] { connectTo(b.address, b.port, b.nickname,
                                                         b.password); });
    }
    m_bookmarksMenu->addSeparator();
    m_bookmarksMenu->addAction(tr("Conectar a todos os favoritos"), this, [this] {
        for (const Bookmark& b : BookmarksDialog::loadAll())
            if (!b.address.trimmed().isEmpty())
                connectTo(b.address, b.port, b.nickname, b.password);
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
                                        connectTo(o["addr"].toString(),
                                                  quint16(o["port"].toInt(9987)),
                                                  S::str("connect/nickname",
                                                         IdentityDialog::defaultNickname()));
                                    });
        }
    }
    if (!any) {
        QAction* none = m_recentMenu->addAction(tr("(vazio)"));
        none->setEnabled(false);
    }
}

// ======================================================================
void MainWindow::checkUpdates() {
    QMessageBox::information(this, tr("Atualização"),
                             tr("Você já está usando a versão mais recente do Halla."));
}

void MainWindow::showNotifications() {
    QMenu menu(this);
    QAction* header = menu.addAction(tr("Notificações"));
    header->setEnabled(false);
    menu.addSeparator();
    QAction* none = menu.addAction(tr("Nenhuma notificação nova"));
    none->setEnabled(false);
    menu.exec(QCursor::pos());
}

void MainWindow::saveSession() {
    QJsonArray arr;
    for (int i = 0; i < m_tabs->count(); ++i) {
        ServerTab* t = qobject_cast<ServerTab*>(m_tabs->widget(i));
        if (!t) continue;
        QJsonObject o;
        o["addr"] = t->data().name;
        o["port"] = 9987;
        o["nick"] = t->data().users[t->data().selfId].name;
        arr << o;
    }
    S::set("session/tabs",
           QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact)));
}

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
    m_actWhisper->setEnabled(connected);

    if (connected) {
        m_actAway->blockSignals(true);
        m_actMuteMic->blockSignals(true);
        m_actMuteSpk->blockSignals(true);
        m_actRecord->blockSignals(true);
        m_actWhisper->blockSignals(true);
        m_actAway->setChecked(t->isAway());
        m_actMuteMic->setChecked(t->isMicMuted());
        m_actMuteSpk->setChecked(t->isSpkMuted());
        m_actRecord->setChecked(t->isRecording());
        m_actWhisper->setChecked(t->whisperActive());
        m_actRecord->setText(t->isRecording() ? tr("Parar gravação")
                                              : tr("Iniciar gravação"));
        m_actAway->blockSignals(false);
        m_actMuteMic->blockSignals(false);
        m_actMuteSpk->blockSignals(false);
        m_actRecord->blockSignals(false);
        m_actWhisper->blockSignals(false);
        m_actAway->setIcon(HIcons::away(t->isAway()));
        m_actMuteMic->setIcon(HIcons::muteMic(t->isMicMuted()));
        m_actMuteSpk->setIcon(HIcons::muteSpeaker(t->isSpkMuted()));
        m_actRecord->setIcon(HIcons::record(t->isRecording()));
    }
}

void MainWindow::updateStatusBar() {
    ServerTab* t = currentTab();
    if (t) {
        m_statusIcon->setPixmap(HIcons::connectPlug().pixmap(14, 14));
        m_statusText->setText(tr("Conectado a %1 como %2")
                                  .arg(t->data().address,
                                       t->data().users[t->data().selfId].name));
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
        m_statusText->setText(tr("Não conectado"));
        m_pingLabel->clear();
    }
}

// ======================================================================
void MainWindow::applyTheme() {
    // tema centralizado: estilo + paleta + stylesheet global (funciona
    // também no Windows, onde stylesheet fixo ignorava a paleta)
    HTheme::apply();
    // força repintura dos widgets que pintam manualmente
    if (m_welcome) m_welcome->update();
}

void MainWindow::applyHotkeys() {
    for (QShortcut* s : m_hotkeyShortcuts)
        if (s) s->deleteLater();
    m_hotkeyShortcuts.clear();

    QJsonDocument doc = QJsonDocument::fromJson(S::str("hotkeys/list").toUtf8());
    if (!doc.isArray()) return;

    for (const QJsonValue& v : doc.array()) {
        QJsonObject o = v.toObject();
        const QString action = o["action"].toString();
        const QKeySequence seq = QKeySequence::fromString(o["key"].toString());
        if (seq.isEmpty()) continue;
        QShortcut* sc = new QShortcut(seq, this);
        sc->setContext(Qt::WindowShortcut);

        if (action.contains(tr("microfone"))) {
            connect(sc, &QShortcut::activated, this, [this] {
                if (ServerTab* t = currentTab()) t->setMicMuted(!t->isMicMuted());
                updateConnectionUi();
            });
        } else if (action.contains(tr("alto-falantes"))) {
            connect(sc, &QShortcut::activated, this, [this] {
                if (ServerTab* t = currentTab()) t->setSpeakersMuted(!t->isSpkMuted());
                updateConnectionUi();
            });
        } else if (action.contains(tr("ausente"))) {
            connect(sc, &QShortcut::activated, this, [this] {
                if (ServerTab* t = currentTab()) t->setAway(!t->isAway());
                updateConnectionUi();
            });
        } else if (action.contains(tr("comandante"))) {
            connect(sc, &QShortcut::activated, this, [this] {
                if (ServerTab* t = currentTab()) {
                    ServerData& d = t->data();
                    User& self = d.users[d.selfId];
                    self.commander = !self.commander;
                    t->tree()->rebuild();
                }
                updateConnectionUi();
            });
        } else {
            connect(sc, &QShortcut::activated, this, [] { QApplication::beep(); });
        }
        m_hotkeyShortcuts << sc;
    }

    // (re)registra a tecla PTT global do sistema (Windows)
    registerPttHotkey();
}

// ======================================================================
void MainWindow::closeEvent(QCloseEvent* e) {
    if (S::flag("app/closeToTray", false) && m_tray && m_tray->isVisible() &&
        !S::flag("app/forceQuit", false)) {
        hide();
        m_tray->showMessage(QStringLiteral("Halla"),
                            tr("O Halla continua em execução na bandeja do sistema."),
                            QSystemTrayIcon::Information, 2400);
        e->ignore();
        return;
    }
    if (m_tabs->count() > 0 && S::flag("app/confirmQuit", true)) {
        const auto ret = QMessageBox::question(
            this, tr("Sair"),
            tr("Você ainda está conectado a servidores.\nDeseja realmente sair?"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (ret != QMessageBox::Yes) {
            e->ignore();
            return;
        }
    }
    saveSession();
    if (m_log) m_log->close();
    AppLog::info(tr("Halla encerrado"));
    QMainWindow::closeEvent(e);
}

bool MainWindow::eventFilter(QObject* obj, QEvent* ev) {
    return QMainWindow::eventFilter(obj, ev);
}

// ======================================================================
// PTT global: RegisterHotKey funciona com a janela EM SEGUNDO PLANO
// (equivale ao push-to-talk do TeamSpeak no Windows)
// ======================================================================
bool MainWindow::nativeEvent(const QByteArray& eventType, void* message,
                             qintptr* result) {
#ifdef Q_OS_WIN
    if (eventType == QByteArrayLiteral("windows_generic_MSG") ||
        eventType == QByteArrayLiteral("windows_dispatcher_MSG")) {
        MSG* msg = static_cast<MSG*>(message);
        if (msg->message == WM_HOTKEY && msg->wParam == 1) {
            pttSetHeld(true); // a soltura é detectada por polling (GetAsyncKeyState)
        }
    }
#endif
    return QMainWindow::nativeEvent(eventType, message, result);
}

void MainWindow::registerPttHotkey() {
#ifdef Q_OS_WIN
    unregisterPttHotkey();
    const QKeySequence ks =
        QKeySequence::fromString(S::str("capture/pttKey", QStringLiteral("Space")));
    if (ks.isEmpty()) return;
    const QKeyCombination comb = ks[0];
    const int k = comb.toCombined();
    const int key = k & ~int(Qt::KeyboardModifierMask);
    UINT vk = 0;
    if (key >= Qt::Key_A && key <= Qt::Key_Z)      vk = UINT(key);
    else if (key >= Qt::Key_0 && key <= Qt::Key_9) vk = UINT(key);
    else if (key == Qt::Key_Space)     vk = VK_SPACE;
    else if (key == Qt::Key_Tab)       vk = VK_TAB;
    else if (key == Qt::Key_CapsLock)  vk = VK_CAPITAL;
    else if (key == Qt::Key_Return)    vk = VK_RETURN;
    else if (key >= Qt::Key_F1 && key <= Qt::Key_F12)
        vk = VK_F1 + UINT(key - Qt::Key_F1);
    UINT mods = MOD_NOREPEAT;
    if (k & int(Qt::ShiftModifier))   mods |= MOD_SHIFT;
    if (k & int(Qt::ControlModifier)) mods |= MOD_CONTROL;
    if (k & int(Qt::AltModifier))     mods |= MOD_ALT;
    if (!vk) return;
    if (RegisterHotKey(HWND(winId()), 1, mods, vk)) {
        m_pttRegistered = true;
        m_pttVk = vk;
        if (!m_pttPoll) {
            m_pttPoll = new QTimer(this);
            m_pttPoll->setInterval(50);
            connect(m_pttPoll, &QTimer::timeout, this, [this] {
                if (!(GetAsyncKeyState(int(m_pttVk)) & 0x8000)) pttSetHeld(false);
            });
        }
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
    if (m_pttPoll) {
        if (held) m_pttPoll->start();
        else      m_pttPoll->stop();
    }
}

// ======================================================================
void MainWindow::loadDemoState() {
    // estado local offline para capturas de tela
    ServerData init;
    init.name = QStringLiteral("meuservidor.exemplo.com");
    init.address = init.name;
    User self;
    self.id = 1; self.name = QStringLiteral("Admin");
    Channel def; def.id = 1; def.name = tr("Canal padrão"); def.isDefault = true;
    def.users << 1;
    init.users[1] = self;
    init.channels[1] = def;
    init.nextChannelId = 2;
    createLocalTab(init);

    ServerTab* t = currentTab();
    if (!t) return;
    ServerData& d = t->data();
    // adiciona canais de exemplo para demonstração visual
    Channel c1; c1.id = d.nextChannelId++; c1.name = tr("Sala de jogos"); c1.codec = 4;
    Channel c2; c2.id = d.nextChannelId++; c2.name = tr("AFK"); c2.codec = 4; c2.moderated = true;
    Channel c3; c3.id = d.nextChannelId++; c3.name = tr("Reuniões"); c3.codec = 5;
    c3.hasPassword = true; c3.passwordHash = QStringLiteral("1234");
    Channel c4; c4.id = d.nextChannelId++; c4.name = tr("Apenas conversa"); c4.codec = 4;
    d.channels[c1.id] = c1;
    d.channels[c2.id] = c2;
    d.channels[c3.id] = c3;
    d.channels[c4.id] = c4;
    t->tree()->rebuild();
    t->tree()->expandAll();
    t->chat()->addServerChat(QStringLiteral("Admin"), QStringLiteral("Olá! [b]Bem-vindo[/b] ao Halla =)"));
    t->chat()->addChannelSystem(tr("Você entrou no canal \"Canal padrão\"."));
}

// -- restauração de sessão usa a rede (já era) ---------------------------
