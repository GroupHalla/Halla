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
#include "dialogs/ScreenShareDialog.h"
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
#include <QPainter>
#include <QFrame>
#include <QHBoxLayout>
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
#include <utility>
#ifdef Q_OS_WIN
#include <windows.h>
// definida mais abaixo (mesmo arquivo)
bool specToVk(const QKeySequence& ks, UINT& vk, UINT& mods);
#endif

class ScreenShareWindow : public QDialog {
public:
    explicit ScreenShareWindow(int userId, const QString& userName, QWidget* parent = nullptr) 
        : QDialog(parent), m_userId(userId) {
        setWindowTitle(tr("Compartilhamento de Tela - %1").arg(userName));
        resize(800, 480);
        setMinimumSize(400, 240);
        setWindowFlags(Qt::Window | Qt::WindowMinMaxButtonsHint | Qt::WindowCloseButtonHint);
        setStyleSheet(QStringLiteral("background-color: #0D0E15; color: #FFFFFF;"));
        
        QVBoxLayout* l = new QVBoxLayout(this);
        l->setContentsMargins(0, 0, 0, 0);
        m_label = new QLabel(this);
        m_label->setAlignment(Qt::AlignCenter);
        m_label->setText(tr("Aguardando transmissão..."));
        m_label->setStyleSheet(QStringLiteral("font-size: 16px; font-weight: bold; color: #8A939B;"));
        l->addWidget(m_label);
    }
    
    int userId() const { return m_userId; }
    
    void updateFrame(const QByteArray& jpegData) {
        if (m_currentPixmap.loadFromData(jpegData)) {
            scaleFrame();
        }
    }
    
protected:
    void resizeEvent(QResizeEvent* e) override {
        QDialog::resizeEvent(e);
        scaleFrame();
    }

private:
    void scaleFrame() {
        if (!m_currentPixmap.isNull()) {
            m_label->setPixmap(m_currentPixmap.scaled(m_label->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
        }
    }
    
    int m_userId;
    QLabel* m_label;
    QPixmap m_currentPixmap;
};

class ScreenshareHoverPopup : public QFrame {
public:
    explicit ScreenshareHoverPopup(int userId, const QString& userName, int channelId, const QByteArray& jpegData, class MainWindow* mw, QWidget* parent = nullptr)
        : QFrame(parent, Qt::ToolTip | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint),
          m_userId(userId), m_channelId(channelId), m_mw(mw) {
        setAttribute(Qt::WA_DeleteOnClose);
        setFixedSize(180, 160);
        setStyleSheet(QStringLiteral(
            "QFrame { background-color: #151322; border: 1px solid #8B5CF6; border-radius: 8px; }"
            "QLabel { color: #FFFFFF; font-size: 11px; font-weight: bold; border: none; background: transparent; }"
            "QPushButton { background-color: #8B5CF6; border: none; border-radius: 4px; color: #FFFFFF; font-weight: bold; padding: 6px; font-size: 11px; }"
            "QPushButton:hover { background-color: #A78BFA; }"
        ));
        
        QVBoxLayout* l = new QVBoxLayout(this);
        l->setContentsMargins(8, 8, 8, 8);
        l->setSpacing(6);
        
        QLabel* title = new QLabel(userName, this);
        title->setAlignment(Qt::AlignCenter);
        l->addWidget(title);
        
        m_imgLabel = new QLabel(this);
        m_imgLabel->setFixedSize(164, 92);
        m_imgLabel->setAlignment(Qt::AlignCenter);
        QPixmap pix;
        if (!jpegData.isEmpty() && pix.loadFromData(jpegData)) {
            m_imgLabel->setPixmap(pix.scaled(m_imgLabel->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
        } else {
            m_imgLabel->setText(tr("Sem sinal"));
            m_imgLabel->setStyleSheet(QStringLiteral("color: #8A939B; background-color: #0D0E15; border-radius: 4px;"));
        }
        l->addWidget(m_imgLabel);
        
        QPushButton* btn = new QPushButton(tr("Assista à transmissão"), this);
        connect(btn, &QPushButton::clicked, this, &ScreenshareHoverPopup::onWatchClicked);
        l->addWidget(btn);
    }
    
protected:
    void leaveEvent(QEvent* e) override {
        QFrame::leaveEvent(e);
        close();
    }
    
private:
    void onWatchClicked();
    
    int m_userId;
    int m_channelId;
    class MainWindow* m_mw;
    QLabel* m_imgLabel;
};

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
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
                                          [this] {
                                              while (m_tabs->count() > 0)
                                                  disconnectTab(qobject_cast<ServerTab*>(
                                                                    m_tabs->widget(0)), false);
                                          });
    m_recentMenu = mConn->addMenu(tr("Conexões recentes"));
    mConn->addAction(HIcons::info(), tr("Informações de conexão..."), this, [this] {
        ServerTab* t = currentTab();
        ServerConnectionInfoDialog dlg(t ? &t->data() : nullptr, t ? t->net() : nullptr, this);
        dlg.exec();
    });
    mConn->addSeparator();
    mConn->addAction(tr("Sair"), this, &MainWindow::close)
        ->setShortcut(QKeySequence(QStringLiteral("Ctrl+Q")));

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
                          connect(&dlg, &OptionsDialog::languageChanged, this, [this] {
                              // Os widgets são construídos com tr() durante a
                              // inicialização. Reiniciar após a escolha aplica
                              // a tradução inteira, não apenas o diálogo atual.
                              QProcess::startDetached(QCoreApplication::applicationFilePath(),
                                                      QCoreApplication::arguments().mid(1));
                              qApp->quit();
                          });
                          dlg.exec();
                      });
    m_actOptions->setShortcut(QKeySequence(QStringLiteral("Alt+P")));

    QMenu* mHelp = menuBar()->addMenu(tr("A&juda"));
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
                menu.addAction(HIcons::bookmarkStar(), tr("Adicionar aos favoritos"), this,
                               [this, t] {
                                   openBookmarksDialog(t->data().name, t->data().address);
                               });
                menu.exec(m_tabs->tabBar()->mapToGlobal(pos));
            });

    // ------------------------- barra de status --------------------------
    // três zonas, como no Halla:
    // [aba do servidor atual]  |  [linha de notícias]  |  [ícone + conexão + ping]
    m_serverMenu = new QMenu(this);
    m_serverMenu->addAction(m_actDisconnect);
    m_serverMenu->addAction(m_actBookmarkAdd);
    m_serverMenu->addSeparator();
    m_serverMenu->addAction(m_actConnect);

    m_serverButton = new QToolButton(this);
    m_serverButton->setObjectName(QStringLiteral("serverTabButton"));
    m_serverButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_serverButton->setIcon(HIcons::server());
    m_serverButton->setText(tr("Nenhum servidor"));
    m_serverButton->setPopupMode(QToolButton::InstantPopup);
    m_serverButton->setMenu(m_serverMenu);
    statusBar()->addWidget(m_serverButton, 0);

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
        HSpeech::say(tr("Conectado ao servidor"));

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
    connect(tab, &ServerTab::statusChanged, this, [this, tab] {
        if (m_tabs->currentWidget() == tab) {
            updateConnectionUi();
            updateStatusBar();
        }
    });
    if (tab->net()) {
        connect(tab->net(), &NetSession::screenshareStateChanged, this, &MainWindow::handleScreenshareStateChanged);
        connect(tab->net(), &NetSession::screenshareFrameReceived, this, &MainWindow::handleScreenshareFrameReceived);
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
    if (!tab) return;
    if (m_screenShareTimer) {
        m_screenShareTimer->stop();
    }
    m_actScreenShare->setChecked(false);
    m_actScreenShare->setIcon(HIcons::screenShare(false));
    qDeleteAll(m_screenShareWindows);
    m_screenShareWindows.clear();

    if (tab->net()) tab->net()->quit();
    const int idx = m_tabs->indexOf(tab);
    const QString addr = tab->data().address;
    m_tabs->removeTab(idx);
    tab->deleteLater();

    AppLog::info(tr("Desconectado de %1").arg(addr));
    if (notify && S::flag("notify/disconnectSound", true))
        HSound::play(QStringLiteral("disconnected"));
    if (notify) HSpeech::say(tr("Desconectado do servidor"));

    saveSession();
    if (m_tabs->count() == 0) {
        m_stack->setCurrentWidget(m_welcome);
    }
    updateConnectionUi();
    updateStatusBar();
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
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QDialog>
#include <QProgressBar>
#include <QProcess>
#include <QStandardPaths>
#include <QFile>
#include <QVBoxLayout>
#include <QLabel>

void MainWindow::checkUpdates(bool manual) {
    QNetworkAccessManager* nam = new QNetworkAccessManager(this);
    QNetworkRequest req(QUrl(QStringLiteral("https://api.github.com/repos/GroupHalla/Halla/releases/latest")));
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("HallaUpdater"));
    
    QNetworkReply* reply = nam->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, nam, manual] {
        reply->deleteLater();
        nam->deleteLater();
        
        if (reply->error() != QNetworkReply::NoError) {
            if (manual) {
                QMessageBox::warning(this, tr("Verificar atualizações"),
                                     tr("Erro ao conectar ao servidor de atualizações."));
            }
            return;
        }
        
        QByteArray data = reply->readAll();
        QJsonDocument doc = QJsonDocument::fromJson(data);
        if (!doc.isObject()) return;
        
        QJsonObject obj = doc.object();
        QString latestVersion = obj["tag_name"].toString();
        if (latestVersion.startsWith('v')) latestVersion.remove(0, 1);
        
        QString currentVersion = QString::fromUtf8(halla::kAppVersion);
        
        if (latestVersion != currentVersion && !latestVersion.isEmpty()) {
            QString downloadUrl;
            QJsonArray assets = obj["assets"].toArray();
            for (const QJsonValue& val : assets) {
                QJsonObject assetObj = val.toObject();
                QString assetName = assetObj["name"].toString();
                if (assetName.contains("Setup") && assetName.endsWith(".exe")) {
                    downloadUrl = assetObj["browser_download_url"].toString();
                    break;
                }
            }
            
            if (downloadUrl.isEmpty() && !assets.isEmpty()) {
                downloadUrl = assets[0].toObject()["browser_download_url"].toString();
            }
            
            if (!downloadUrl.isEmpty()) {
                int ret = QMessageBox::question(this, tr("Nova atualização disponível"),
                    tr("Uma nova versão (%1) está disponível!\nDeseja baixar e instalar agora?").arg(latestVersion),
                    QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
                if (ret == QMessageBox::Yes) {
                    downloadAndInstallUpdate(downloadUrl, latestVersion);
                }
            }
        } else {
            if (manual) {
                QMessageBox::information(this, tr("Atualização"),
                                         tr("Você já está usando a versão mais recente do Halla."));
            }
        }
    });
}

void MainWindow::downloadAndInstallUpdate(const QString& url, const QString& version) {
    QDialog* dlg = new QDialog(this);
    dlg->setWindowTitle(tr("Baixando atualização"));
    dlg->resize(350, 100);
    
    QVBoxLayout* lay = new QVBoxLayout(dlg);
    QLabel* label = new QLabel(tr("Baixando Halla v%1...").arg(version), dlg);
    lay->addWidget(label);
    
    QProgressBar* bar = new QProgressBar(dlg);
    bar->setRange(0, 100);
    bar->setValue(0);
    lay->addWidget(bar);
    
    dlg->show();
    
    QNetworkAccessManager* nam = new QNetworkAccessManager(dlg);
    QNetworkRequest req((QUrl(url)));
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("HallaUpdater"));
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    
    QNetworkReply* reply = nam->get(req);
    
    connect(reply, &QNetworkReply::downloadProgress, dlg, [bar](qint64 bytesReceived, qint64 bytesTotal) {
        if (bytesTotal > 0) {
            bar->setValue(int((bytesReceived * 100) / bytesTotal));
        }
    });
    
    connect(reply, &QNetworkReply::finished, dlg, [this, reply, dlg, version] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            QMessageBox::critical(this, tr("Erro de download"), tr("Não foi possível baixar o instalador da atualização."));
            dlg->close();
            return;
        }
        
        QString tempDir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
        QString installerPath = tempDir + QStringLiteral("/Halla-Setup-") + version + QStringLiteral(".exe");
        
        QFile file(installerPath);
        if (file.open(QIODevice::WriteOnly)) {
            file.write(reply->readAll());
            file.close();
            
            QMessageBox::information(this, tr("Download concluído"),
                tr("O download foi concluído com sucesso. O instalador será executado agora."));
                
            S::set("app/forceQuit", true); // Bypassa todos os diálogos de confirmação de saída!
            QProcess::startDetached(installerPath, QStringList());
            qApp->quit();
        } else {
            QMessageBox::critical(this, tr("Erro"), tr("Não foi possível salvar o arquivo de atualização no diretório temporário."));
        }
        dlg->close();
    });
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
    // zona esquerda: "aba" com o nome do servidor atual
    m_serverButton->setText(t ? t->data().name : tr("Nenhum servidor"));
    m_serverButton->setIcon(HIcons::server());

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
        m_serverButton->setText(tr("%1 servidores").arg(m_tabs->count()));
        m_statusIcon->setPixmap(HIcons::connectPlug().pixmap(14, 14));
        m_statusText->setText(tr("%1 conexões abertas").arg(m_tabs->count()));
        m_pingLabel->clear();
    } else {
        m_statusIcon->setPixmap(HIcons::disconnectPlug().pixmap(14, 14));
        m_statusText->setText(tr("Desconectado"));
        m_pingLabel->clear();
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
    if (S::flag("app/closeToTray", false) && m_tray && m_tray->isVisible() &&
        !S::flag("app/forceQuit", false)) {
        hide();
        m_tray->showMessage(QStringLiteral("Halla"),
                            tr("O Halla continua em execução na bandeja do sistema."),
                            QSystemTrayIcon::Information, 2400);
        e->ignore();
        return;
    }
    if (m_tabs->count() > 0 && S::flag("app/confirmQuit", true) && !S::flag("app/forceQuit", false)) {
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
                }
            }
            pollGlobalInputs();
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
    init.version = QStringLiteral("3.13.34");
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
        ScreenShareDialog dlg(this);
        if (dlg.exec() == QDialog::Accepted) {
            m_screenShareSourceType = dlg.selectedSourceType();
            m_screenShareSourceId = dlg.selectedSourceId();
            m_screenShareSeq = 0;
            
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
        if (screen && m_screenShareSourceId > 0) {
            pix = screen->grabWindow(WId(m_screenShareSourceId));
        }
    }
    
    if (pix.isNull()) return;
    
    // Algoritmo Ultra-Rápido de Detecção de Mudança de Tela (Delta Frame skipping):
    // Reduz o consumo de upload para quase zero quando a tela está estática,
    // garantindo ping baixo constante (abaixo de 30ms) e sem bufferbloat!
    QImage thumb = pix.toImage().scaled(16, 16, Qt::IgnoreAspectRatio, Qt::FastTransformation);
    if (m_prevThumbnail == thumb) {
        if (m_keepAliveTicks++ < 60) { // Envia apenas um frame a cada 3 segundos como keep-alive
            return; 
        }
    }
    m_keepAliveTicks = 0;
    m_prevThumbnail = thumb;
    
    QPixmap scaled = pix.scaled(t->net()->screenshareWidth(), t->net()->screenshareHeight(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
    
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    scaled.save(&buffer, "JPEG", 65); // Qualidade 65 (imagem super nítida e cristalina em tela cheia!)
    
    t->net()->sendScreenShareFrame(bytes, ++m_screenShareSeq);
    
    if (m_screenShareWindows.contains(t->data().selfId)) {
        m_screenShareWindows[t->data().selfId]->updateFrame(bytes);
    }
}

void MainWindow::handleScreenshareStateChanged(int userId, bool on) {
    ServerTab* t = currentTab();
    if (!t) return;
    
    if (on) {
        if (!m_screenShareWindows.contains(userId)) {
            QString userName = QStringLiteral("Usuário #%1").arg(userId);
            if (t->data().users.contains(userId)) {
                userName = t->data().users[userId].name;
            }
            ScreenShareWindow* win = new ScreenShareWindow(userId, userName, this);
            m_screenShareWindows[userId] = win;
            
            connect(win, &QDialog::finished, this, [this, userId]() {
                m_screenShareWindows.remove(userId);
            });
            win->show();
        }
    } else {
        if (m_screenShareWindows.contains(userId)) {
            ScreenShareWindow* win = m_screenShareWindows[userId];
            win->close();
            win->deleteLater();
            m_screenShareWindows.remove(userId);
        }
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
    
    // Evita abrir múltiplos popups redundantes
    for (QWidget* w : QApplication::topLevelWidgets()) {
        if (dynamic_cast<ScreenshareHoverPopup*>(w)) {
            return;
        }
    }
    
    QString userName = QStringLiteral("Usuário #%1").arg(userId);
    if (t->data().users.contains(userId)) {
        userName = t->data().users[userId].name;
    }
    
    QByteArray lastFrame = m_lastScreenshareFrames.value(userId);
    
    ScreenshareHoverPopup* popup = new ScreenshareHoverPopup(userId, userName, channelId, lastFrame, this, this);
    popup->move(pos + QPoint(15, 15));
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
    
    int myChan = t->data().channelOfUser(t->data().selfId);
    if (myChan != channelId) {
        t->net()->moveToChannel(channelId);
    }
    
    handleScreenshareStateChanged(userId, true);
}

// -- restauração de sessão usa a rede (já era) ---------------------------
