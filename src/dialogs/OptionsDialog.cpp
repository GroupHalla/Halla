#include "OptionsDialog.h"
#include "TsBanner.h"
#include "Icons.h"
#include "Settings.h"
#include "AppLog.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QPushButton>
#include <QComboBox>
#include <QCheckBox>
#include <QSlider>
#include <QSpinBox>
#include <QLabel>
#include <QLineEdit>
#include <QGroupBox>
#include <QFontComboBox>
#include <QKeySequenceEdit>
#include <QTableWidget>
#include <QHeaderView>
#include <QStackedLayout>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QScrollArea>
#include <QApplication>
#include <QGuiApplication>
#include <QClipboard>

static QWidget* wrapScroll(QWidget* inner) {
    QScrollArea* sa = new QScrollArea;
    sa->setWidgetResizable(true);
    sa->setFrameShape(QFrame::NoFrame);
    sa->setWidget(inner);
    return sa;
}

OptionsDialog::OptionsDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("Opções"));
    resize(720, 500);

    QVBoxLayout* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 8);
    root->setSpacing(0);
    root->addWidget(new TsBanner(tr("Opções"), tr("Configurações do cliente Halla"),
                                 HIcons::optionsGear().pixmap(24, 24), this));
    root->addSpacing(8);

    QHBoxLayout* mid = new QHBoxLayout;
    mid->setContentsMargins(8, 0, 8, 0);
    mid->setSpacing(8);

    m_nav = new QListWidget(this);
    m_nav->setFixedWidth(190);
    m_nav->setIconSize(QSize(18, 18));
    m_nav->setSpacing(2);

    m_stack = new QStackedWidget(this);

    struct PageDef { QString name; QIcon icon; };
    const QList<PageDef> pages = {
        { tr("Aplicativo"),       HIcons::application() },
        { tr("Design"),           HIcons::design() },
        { tr("Notificações"),     HIcons::notifyBell() },
        { tr("Reprodução"),       HIcons::playbackSpeaker() },
        { tr("Captura"),          HIcons::captureMic() },
        { tr("Teclas de atalho"), HIcons::hotkeys() },
        { tr("Segurança"),        HIcons::security() },
        { tr("Complementos"),     HIcons::addons() },
    };
    for (const PageDef& d : pages) {
        QListWidgetItem* it = new QListWidgetItem(d.icon, d.name);
        m_nav->addItem(it);
    }

    m_stack->addWidget(wrapScroll(pageApplication()));
    m_stack->addWidget(wrapScroll(pageDesign()));
    m_stack->addWidget(wrapScroll(pageNotifications()));
    m_stack->addWidget(wrapScroll(pagePlayback()));
    m_stack->addWidget(wrapScroll(pageCapture()));
    m_stack->addWidget(wrapScroll(pageHotkeys()));
    m_stack->addWidget(wrapScroll(pageSecurity()));
    m_stack->addWidget(wrapScroll(pageAddons()));

    mid->addWidget(m_nav);
    mid->addWidget(m_stack, 1);
    root->addLayout(mid, 1);

    connect(m_nav, &QListWidget::currentRowChanged, m_stack,
            &QStackedWidget::setCurrentIndex);
    m_nav->setCurrentRow(0);

    QHBoxLayout* bottom = new QHBoxLayout;
    bottom->setContentsMargins(10, 4, 10, 0);
    bottom->addStretch(1);
    QPushButton* ok = new QPushButton(tr("OK"), this);
    QPushButton* cancel = new QPushButton(tr("Cancelar"), this);
    QPushButton* applyBtn = new QPushButton(tr("Aplicar"), this);
    bottom->addWidget(ok);
    bottom->addWidget(cancel);
    bottom->addWidget(applyBtn);
    root->addLayout(bottom);

    connect(ok, &QPushButton::clicked, this, [this] { apply(); accept(); });
    connect(applyBtn, &QPushButton::clicked, this, [this] { apply(); });
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
}

void OptionsDialog::selectPage(const QString& pageName) {
    for (int i = 0; i < m_nav->count(); ++i)
        if (m_nav->item(i)->text() == pageName) { m_nav->setCurrentRow(i); return; }
}

void OptionsDialog::apply() {
    S::store().sync();
    AppLog::info(tr("Configurações aplicadas"));
}

// ------------------------------------------------------------------ Aplicativo
QWidget* OptionsDialog::pageApplication() {
    QWidget* w = new QWidget;
    QVBoxLayout* lay = new QVBoxLayout(w);

    QFormLayout* form = new QFormLayout;
    form->setSpacing(8);

    QComboBox* lang = new QComboBox(w);
    lang->addItems({ QStringLiteral("Português (Brasil)"), QStringLiteral("English"),
                     QStringLiteral("Deutsch"), QStringLiteral("Español"),
                     QStringLiteral("Français") });
    lang->setCurrentIndex(S::num("app/language", 0));
    form->addRow(tr("Idioma:"), lang);
    connect(lang, &QComboBox::currentIndexChanged, this, [this](int idx) {
        S::set("app/language", idx);
    });

    lay->addLayout(form);
    lay->addSpacing(6);

    QGroupBox* gbStart = new QGroupBox(tr("Inicialização"), w);
    QVBoxLayout* v1 = new QVBoxLayout(gbStart);
    QCheckBox* restore = new QCheckBox(tr("Restaurar conexões abertas da última sessão"), gbStart);
    restore->setChecked(S::flag("app/restoreTabs", false));
    connect(restore, &QCheckBox::toggled, this, [](bool v) { S::set("app/restoreTabs", v); });
    v1->addWidget(restore);
    QCheckBox* autoupdate = new QCheckBox(tr("Procurar atualizações automaticamente"), gbStart);
    autoupdate->setChecked(S::flag("app/autoUpdate", true));
    connect(autoupdate, &QCheckBox::toggled, this, [](bool v) { S::set("app/autoUpdate", v); });
    v1->addWidget(autoupdate);
    lay->addWidget(gbStart);

    QGroupBox* gbClose = new QGroupBox(tr("Janela principal"), w);
    QVBoxLayout* v2 = new QVBoxLayout(gbClose);
    QCheckBox* tray = new QCheckBox(tr("Fechar para a bandeja do sistema"), gbClose);
    tray->setChecked(S::flag("app/closeToTray", false));
    connect(tray, &QCheckBox::toggled, this, [](bool v) { S::set("app/closeToTray", v); });
    v2->addWidget(tray);
    QCheckBox* confirm = new QCheckBox(tr("Confirmar antes de sair estando conectado"), gbClose);
    confirm->setChecked(S::flag("app/confirmQuit", true));
    connect(confirm, &QCheckBox::toggled, this, [](bool v) { S::set("app/confirmQuit", v); });
    v2->addWidget(confirm);
    lay->addWidget(gbClose);

    QGroupBox* gbPerm = new QGroupBox(tr("Permissões"), w);
    QVBoxLayout* v3 = new QVBoxLayout(gbPerm);
    QCheckBox* advanced = new QCheckBox(tr("Sistema de permissões avançado"), gbPerm);
    advanced->setChecked(S::flag("app/advancedPerms", false));
    connect(advanced, &QCheckBox::toggled, this, [](bool v) { S::set("app/advancedPerms", v); });
    v3->addWidget(advanced);
    lay->addWidget(gbPerm);

    lay->addStretch(1);
    return w;
}

// ------------------------------------------------------------------ Design
QWidget* OptionsDialog::pageDesign() {
    QWidget* w = new QWidget;
    QFormLayout* form = new QFormLayout(w);
    form->setSpacing(8);

    QComboBox* theme = new QComboBox(w);
    theme->addItems({ tr("Claro (padrão)"), tr("Escuro") });
    theme->setCurrentIndex(S::num("design/theme", 0));
    form->addRow(tr("Tema:"), theme);
    connect(theme, &QComboBox::currentIndexChanged, this, [this](int idx) {
        S::set("design/theme", idx);
        emit themeChanged();
    });

    QFontComboBox* font = new QFontComboBox(w);
    const QString savedFont = S::str("design/font", QFont().defaultFamily());
    font->setCurrentFont(QFont(savedFont));
    form->addRow(tr("Fonte:"), font);
    connect(font, &QFontComboBox::currentFontChanged, this, [this](const QFont& f) {
        S::set("design/font", f.family());
        emit designChanged();
    });

    QSpinBox* fontSize = new QSpinBox(w);
    fontSize->setRange(8, 16);
    fontSize->setValue(S::num("design/fontSize", 9));
    fontSize->setSuffix(QStringLiteral(" pt"));
    form->addRow(tr("Tamanho da fonte:"), fontSize);
    connect(fontSize, &QSpinBox::valueChanged, this, [this](int v) {
        S::set("design/fontSize", v);
        emit designChanged();
    });

    QCheckBox* counts = new QCheckBox(tr("Mostrar número de clientes ao lado dos canais"), w);
    counts->setChecked(S::flag("design/showCounts", true));
    connect(counts, &QCheckBox::toggled, this, [this](bool v) {
        S::set("design/showCounts", v);
        emit designChanged();
    });
    form->addRow(QString(), counts);

    QCheckBox* minis = new QCheckBox(tr("Mostrar mini-ícones de estado dos clientes"), w);
    minis->setChecked(S::flag("design/showMinis", true));
    connect(minis, &QCheckBox::toggled, this, [this](bool v) {
        S::set("design/showMinis", v);
        emit designChanged();
    });
    form->addRow(QString(), minis);

    QCheckBox* tooltips = new QCheckBox(tr("Mostrar dicas de ferramentas na árvore do servidor"), w);
    tooltips->setChecked(S::flag("design/tooltips", true));
    connect(tooltips, &QCheckBox::toggled, this, [](bool v) { S::set("design/tooltips", v); });
    form->addRow(QString(), tooltips);

    form->addItem(new QSpacerItem(0, 0, QSizePolicy::Minimum, QSizePolicy::Expanding));
    return w;
}

// ------------------------------------------------------------------ Notificações
QWidget* OptionsDialog::pageNotifications() {
    QWidget* w = new QWidget;
    QVBoxLayout* lay = new QVBoxLayout(w);
    QLabel* info = new QLabel(
        tr("Escolha quais eventos reproduzem um som de notificação no cliente."), w);
    info->setWordWrap(true);
    lay->addWidget(info);

    const QList<QPair<QString, QString>> events = {
        { "notify/connectSound",    tr("Ao conectar a um servidor") },
        { "notify/disconnectSound", tr("Ao desconectar de um servidor") },
        { "notify/pokeSound",       tr("Ao ser cutucado") },
        { "notify/messageSound",    tr("Ao receber mensagem privada") },
        { "notify/channelSwitchSound", tr("Ao trocar de canal") },
        { "notify/muteSound",       tr("Ao ativar/desativar mudo") },
    };
    for (const auto& ev : events) {
        QCheckBox* cb = new QCheckBox(ev.second, w);
        cb->setChecked(S::flag(ev.first, true));
        connect(cb, &QCheckBox::toggled, this,
                [key = ev.first](bool v) { S::set(key, v); });
        lay->addWidget(cb);
    }

    QHBoxLayout* row = new QHBoxLayout;
    row->addSpacing(20);
    QPushButton* test = new QPushButton(tr("Reproduzir som de teste"), w);
    row->addWidget(test);
    row->addStretch(1);
    lay->addLayout(row);
    connect(test, &QPushButton::clicked, this, [] { QApplication::beep(); });

    lay->addStretch(1);
    return w;
}

// ------------------------------------------------------------------ Reprodução
QWidget* OptionsDialog::pagePlayback() {
    QWidget* w = new QWidget;
    QFormLayout* form = new QFormLayout(w);
    form->setSpacing(8);

    QComboBox* mode = new QComboBox(w);
    mode->addItems({ tr("Automaticamente selecionar melhor modo"), tr("Direct Sound"),
                     tr("Windows Audio Session"), tr("PulseAudio"), tr("ALSA") });
    mode->setCurrentIndex(S::num("playback/mode", 0));
    form->addRow(tr("Modo de reprodução:"), mode);
    connect(mode, &QComboBox::currentIndexChanged, this,
            [](int v) { S::set("playback/mode", v); });

    QComboBox* dev = new QComboBox(w);
    dev->addItem(tr("Padrão (dispositivo do sistema)"));
    dev->setEnabled(false);
    form->addRow(tr("Dispositivo de reprodução:"), dev);

    QHBoxLayout* vrow = new QHBoxLayout;
    QSlider* vol = new QSlider(Qt::Horizontal, w);
    vol->setRange(-40, 12);
    vol->setValue(S::num("playback/volumeDb", 0));
    QLabel* volLabel = new QLabel(QStringLiteral("%1 dB").arg(vol->value()), w);
    vrow->addWidget(vol, 1);
    vrow->addWidget(volLabel);
    QWidget* vw = new QWidget(w);
    vw->setLayout(vrow);
    form->addRow(tr("Volume:"), vw);
    connect(vol, &QSlider::valueChanged, this, [volLabel](int v) {
        S::set("playback/volumeDb", v);
        volLabel->setText(QStringLiteral("%1 dB").arg(v));
    });

    QCheckBox* duck = new QCheckBox(
        tr("Reduzir o volume de outros aplicativos quando alguém estiver falando"), w);
    duck->setChecked(S::flag("playback/ducking", false));
    connect(duck, &QCheckBox::toggled, this,
            [](bool v) { S::set("playback/ducking", v); });
    form->addRow(QString(), duck);

    QPushButton* test = new QPushButton(tr("Testar reprodução"), w);
    form->addRow(QString(), test);
    connect(test, &QPushButton::clicked, this, [] { QApplication::beep(); });

    form->addItem(new QSpacerItem(0, 0, QSizePolicy::Minimum, QSizePolicy::Expanding));
    return w;
}

// ------------------------------------------------------------------ Captura
QWidget* OptionsDialog::pageCapture() {
    QWidget* w = new QWidget;
    QFormLayout* form = new QFormLayout(w);
    form->setSpacing(8);

    QComboBox* mode = new QComboBox(w);
    mode->addItems({ tr("Automaticamente selecionar melhor modo"), tr("Direct Sound"),
                     tr("Windows Audio Session"), tr("PulseAudio"), tr("ALSA") });
    mode->setCurrentIndex(S::num("capture/mode", 0));
    form->addRow(tr("Modo de captura:"), mode);
    connect(mode, &QComboBox::currentIndexChanged, this,
            [](int v) { S::set("capture/mode", v); });

    QComboBox* dev = new QComboBox(w);
    dev->addItem(tr("Padrão (dispositivo do sistema)"));
    dev->setEnabled(false);
    form->addRow(tr("Dispositivo de captura:"), dev);

    QComboBox* profile = new QComboBox(w);
    profile->setEditable(true);
    profile->addItem(S::str("capture/profile", tr("Padrão")));
    form->addRow(tr("Perfil de captura:"), profile);
    connect(profile, &QComboBox::currentTextChanged, this,
            [](const QString& t) { S::set("capture/profile", t); });

    QComboBox* pttMode = new QComboBox(w);
    pttMode->addItems({ tr("Pressionar para falar (PTT)"),
                        tr("Detecção de voz"),
                        tr("Transmissão contínua") });
    pttMode->setCurrentIndex(S::num("capture/pttMode", 1));
    form->addRow(tr("Ativação de voz:"), pttMode);
    connect(pttMode, &QComboBox::currentIndexChanged, this,
            [](int v) { S::set("capture/pttMode", v); });

    QKeySequenceEdit* pttKey = new QKeySequenceEdit(
        QKeySequence::fromString(S::str("capture/pttKey", "Space")), w);
    form->addRow(tr("Tecla PTT:"), pttKey);
    connect(pttKey, &QKeySequenceEdit::keySequenceChanged, this,
            [](const QKeySequence& s) { S::set("capture/pttKey", s.toString()); });

    QHBoxLayout* srow = new QHBoxLayout;
    QSlider* level = new QSlider(Qt::Horizontal, w);
    level->setRange(-60, 0);
    level->setValue(S::num("capture/voiceLevel", -45));
    QLabel* levelLabel = new QLabel(QStringLiteral("%1 dB").arg(level->value()), w);
    srow->addWidget(level, 1);
    srow->addWidget(levelLabel);
    QWidget* sw = new QWidget(w);
    sw->setLayout(srow);
    form->addRow(tr("Nível de ativação de voz:"), sw);
    connect(level, &QSlider::valueChanged, this, [levelLabel](int v) {
        S::set("capture/voiceLevel", v);
        levelLabel->setText(QStringLiteral("%1 dB").arg(v));
    });

    QGroupBox* gbEcho = new QGroupBox(tr("Opções avançadas"), w);
    QVBoxLayout* gv = new QVBoxLayout(gbEcho);
    QCheckBox* echo = new QCheckBox(tr("Redução de eco"), gbEcho);
    echo->setChecked(S::flag("capture/echoReduction", true));
    connect(echo, &QCheckBox::toggled, this,
            [](bool v) { S::set("capture/echoReduction", v); });
    QCheckBox* cancel = new QCheckBox(tr("Cancelamento de eco acústico"), gbEcho);
    cancel->setChecked(S::flag("capture/echoCancellation", false));
    connect(cancel, &QCheckBox::toggled, this,
            [](bool v) { S::set("capture/echoCancellation", v); });
    QCheckBox* denoise = new QCheckBox(tr("Remover ruído de fundo"), gbEcho);
    denoise->setChecked(S::flag("capture/denoise", true));
    connect(denoise, &QCheckBox::toggled, this,
            [](bool v) { S::set("capture/denoise", v); });
    gv->addWidget(echo);
    gv->addWidget(cancel);
    gv->addWidget(denoise);
    form->addRow(gbEcho);

    form->addItem(new QSpacerItem(0, 0, QSizePolicy::Minimum, QSizePolicy::Expanding));
    return w;
}

// ------------------------------------------------------------------ Hotkeys
QWidget* OptionsDialog::pageHotkeys() {
    QWidget* w = new QWidget;
    QVBoxLayout* lay = new QVBoxLayout(w);

    QHBoxLayout* top = new QHBoxLayout;
    QLabel* l = new QLabel(tr("Perfil de teclas de atalho:"), w);
    QComboBox* profile = new QComboBox(w);
    profile->addItem(S::str("hotkeys/profile", tr("Padrão")));
    profile->setEditable(true);
    top->addWidget(l);
    top->addWidget(profile, 1);
    lay->addLayout(top);
    connect(profile, &QComboBox::currentTextChanged, this,
            [](const QString& t) { S::set("hotkeys/profile", t); });

    QTableWidget* table = new QTableWidget(0, 2, w);
    table->setHorizontalHeaderLabels({ tr("Ação"), tr("Atalho") });
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    lay->addWidget(table, 1);

    auto loadTable = [table] {
        table->setRowCount(0);
        QJsonDocument doc = QJsonDocument::fromJson(S::str("hotkeys/list").toUtf8());
        if (!doc.isArray()) return;
        for (const QJsonValue& v : doc.array()) {
            QJsonObject o = v.toObject();
            int r = table->rowCount();
            table->insertRow(r);
            table->setItem(r, 0, new QTableWidgetItem(o["action"].toString()));
            table->setItem(r, 1, new QTableWidgetItem(o["key"].toString()));
        }
    };
    loadTable();

    auto saveTable = [table, this] {
        QJsonArray arr;
        for (int r = 0; r < table->rowCount(); ++r) {
            QJsonObject o;
            o["action"] = table->item(r, 0)->text();
            o["key"] = table->item(r, 1)->text();
            arr << o;
        }
        S::set("hotkeys/list",
               QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact)));
        emit hotkeysChanged();
    };

    QHBoxLayout* btns = new QHBoxLayout;
    QPushButton* add = new QPushButton(tr("Adicionar"), w);
    QPushButton* edit = new QPushButton(tr("Editar"), w);
    QPushButton* del = new QPushButton(tr("Excluir"), w);
    btns->addWidget(add);
    btns->addWidget(edit);
    btns->addWidget(del);
    btns->addStretch(1);
    lay->addLayout(btns);

    const QStringList actions = {
        tr("Alternar mudo do microfone"),
        tr("Alternar mudo dos alto-falantes"),
        tr("Alternar estado ausente"),
        tr("Alternar comandante do canal"),
        tr("Alternar gravação"),
        tr("Alternar transmissão contínua"),
    };

    // IMPORTANTE: capturar por CÓPIA — esta lambda escapa para o connect()
    // dos botões e é chamada depois que pageHotkeys() retorna. Com [&] as
    // referências ficariam penduradas (stack morto) e o app fecha/crash,
    // principalmente no Windows.
    auto editRow = [=, this](int row) {
        QDialog d(w);
        d.setWindowTitle(row < 0 ? tr("Adicionar tecla de atalho") : tr("Editar tecla de atalho"));
        QFormLayout* f = new QFormLayout(&d);
        QComboBox* action = new QComboBox(&d);
        action->addItems(actions);
        QKeySequenceEdit* key = new QKeySequenceEdit(&d);
        if (row >= 0) {
            action->setCurrentText(table->item(row, 0)->text());
            key->setKeySequence(QKeySequence::fromString(table->item(row, 1)->text()));
        }
        f->addRow(tr("Ação:"), action);
        f->addRow(tr("Atalho:"), key);
        QHBoxLayout* rb = new QHBoxLayout;
        QPushButton* ok = new QPushButton(tr("OK"), &d);
        QPushButton* cancel = new QPushButton(tr("Cancelar"), &d);
        rb->addStretch(1);
        rb->addWidget(ok);
        rb->addWidget(cancel);
        f->addRow(rb);
        QObject::connect(ok, &QPushButton::clicked, &d, &QDialog::accept);
        QObject::connect(cancel, &QPushButton::clicked, &d, &QDialog::reject);
        if (d.exec() != QDialog::Accepted) return;
        if (row < 0) {
            row = table->rowCount();
            table->insertRow(row);
        }
        table->setItem(row, 0, new QTableWidgetItem(action->currentText()));
        table->setItem(row, 1, new QTableWidgetItem(key->keySequence().toString()));
        saveTable();
    };

    connect(add, &QPushButton::clicked, this, [=] { editRow(-1); });
    connect(edit, &QPushButton::clicked, this, [=] {
        auto items = table->selectedItems();
        if (!items.isEmpty()) editRow(items.first()->row());
    });
    connect(del, &QPushButton::clicked, this, [=] {
        auto items = table->selectedItems();
        if (items.isEmpty()) return;
        table->removeRow(items.first()->row());
        saveTable();
    });

    QLabel* note = new QLabel(
        tr("As teclas de atalho ficam ativas enquanto a janela do Halla estiver em foco."),
        w);
    note->setStyleSheet(QStringLiteral("color:#666666"));
    note->setWordWrap(true);
    lay->addWidget(note);
    return w;
}

// ------------------------------------------------------------------ Segurança
QWidget* OptionsDialog::pageSecurity() {
    QWidget* w = new QWidget;
    QFormLayout* form = new QFormLayout(w);
    form->setSpacing(8);

    QLabel* uidCaption = new QLabel(tr("Identidade (ID único):"), w);
    QLineEdit* uid = new QLineEdit(w);
    uid->setReadOnly(true);
    QJsonDocument doc = QJsonDocument::fromJson(S::str("identities").toUtf8());
    QString unique = QStringLiteral("—");
    if (doc.isArray() && !doc.array().isEmpty())
        unique = doc.array().first().toObject()["uid"].toString();
    uid->setText(unique);
    form->addRow(uidCaption, uid);

    QPushButton* copy = new QPushButton(tr("Copiar ID único para a área de transferência"), w);
    form->addRow(QString(), copy);
    connect(copy, &QPushButton::clicked, this, [uid] {
        QGuiApplication::clipboard()->setText(uid->text());
    });

    QCheckBox* remember = new QCheckBox(tr("Lembrar senhas inseridas durante a sessão"), w);
    remember->setChecked(S::flag("security/rememberPasswords", false));
    connect(remember, &QCheckBox::toggled, this,
            [](bool v) { S::set("security/rememberPasswords", v); });
    form->addRow(QString(), remember);

    QCheckBox* warn = new QCheckBox(
        tr("Avisar quando um servidor alterar suas permissões"), w);
    warn->setChecked(S::flag("security/warnPermissionChange", true));
    connect(warn, &QCheckBox::toggled, this,
            [](bool v) { S::set("security/warnPermissionChange", v); });
    form->addRow(QString(), warn);

    QPushButton* clear = new QPushButton(tr("Limpar atalhos, logs e cache local..."), w);
    form->addRow(QString(), clear);
    connect(clear, &QPushButton::clicked, this, [] {
        AppLog::info(QStringLiteral("Cache local limpo pelo usuário"));
    });

    form->addItem(new QSpacerItem(0, 0, QSizePolicy::Minimum, QSizePolicy::Expanding));
    return w;
}

// ------------------------------------------------------------------ Complementos
QWidget* OptionsDialog::pageAddons() {
    QWidget* w = new QWidget;
    QVBoxLayout* lay = new QVBoxLayout(w);
    QLabel* none = new QLabel(tr("Nenhum complemento instalado."), w);
    none->setAlignment(Qt::AlignCenter);
    none->setStyleSheet(QStringLiteral("color:#888888; font-style:italic"));
    lay->addStretch(1);
    lay->addWidget(none);
    QHBoxLayout* row = new QHBoxLayout;
    row->addStretch(1);
    QPushButton* search = new QPushButton(tr("Procurar complementos online"), w);
    search->setEnabled(false);
    row->addWidget(search);
    row->addStretch(1);
    lay->addLayout(row);
    lay->addStretch(1);
    return w;
}
