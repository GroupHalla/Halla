#include "OptionsDialog.h"
#include "Icons.h"
#include "Settings.h"
#include "AppLog.h"
#include "SoundPack.h"
#include "HotkeyEdit.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QPushButton>
#include <QComboBox>
#include <QCheckBox>
#include <QRadioButton>
#include <QButtonGroup>
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
#include <QPainter>
#include <QMessageBox>

// envolve a página em um QScrollArea SEM moldura (estilo do TS3: o conteúdo
// flutua sobre o fundo branco, sem caixas cinzas à vista)
static QWidget* wrapScroll(QWidget* inner) {
    inner->setObjectName(QStringLiteral("optionsPage"));
    inner->setAttribute(Qt::WA_StyledBackground, true);
    inner->setContentsMargins(8, 8, 8, 8);

    QScrollArea* sa = new QScrollArea;
    sa->setObjectName(QStringLiteral("optionsScroll"));
    sa->setWidgetResizable(true);
    sa->setFrameShape(QFrame::NoFrame);
    sa->setWidget(inner);
    return sa;
}

// ícone da seção no canto superior direito do cabeçalho (levemente suavizado)
static QPixmap headerIconPixmap(const QIcon& icon, int size) {
    const QPixmap src = icon.pixmap(size, size);
    QPixmap out(src.size());
    out.fill(Qt::transparent);
    QPainter p(&out);
    p.setOpacity(0.92);
    p.drawPixmap(0, 0, src);
    return out;
}

// linha separadora de 1px (vertical na sidebar / horizontal sobre os botões)
static QWidget* separatorLine(bool vertical) {
    QWidget* w = new QWidget;
    w->setObjectName(QStringLiteral("optionsSep"));
    if (vertical) {
        w->setFixedWidth(1);
        w->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    } else {
        w->setFixedHeight(1);
        w->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }
    w->setAttribute(Qt::WA_StyledBackground, true);
    return w;
}

OptionsDialog::OptionsDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("Opções"));
    resize(820, 580);

    QVBoxLayout* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 8);
    root->setSpacing(0);

    QHBoxLayout* mid = new QHBoxLayout;
    mid->setContentsMargins(0, 0, 0, 0);
    mid->setSpacing(0);

    // ---------------- menu lateral (ícones grandes + texto) --------------
    m_nav = new QListWidget(this);
    m_nav->setObjectName(QStringLiteral("optionsNav"));
    m_nav->setFixedWidth(192);
    m_nav->setIconSize(QSize(24, 24));
    m_nav->setSpacing(1);
    m_nav->setFrameShape(QFrame::NoFrame);

    struct PageDef { QString name; QString subtitle; QIcon icon; };
    const QList<PageDef> pages = {
        { tr("Aplicativo"),       tr("Opções gerais do aplicativo"),          HIcons::application() },
        { tr("Reprodução"),       tr("Volume e saída de áudio"),              HIcons::playbackSpeaker() },
        { tr("Captura"),          tr("Microfone, PTT e ativação de voz"),     HIcons::captureMic() },
        { tr("Aparência"),        tr("Tema, fonte e comportamento visual"),   HIcons::design() },
        { tr("Notificações"),     tr("Sons e avisos de eventos"),             HIcons::notifyBell() },
        { tr("Teclas de atalho"), tr("Atalhos globais, mouse e sussurro"),    HIcons::hotkeys() },
        { tr("Segurança"),        tr("Identidade e segurança"),               HIcons::security() },
        { tr("Complementos"),     tr("Extensões e pacotes do cliente"),       HIcons::addons() },
    };
    for (const PageDef& d : pages) {
        QListWidgetItem* it = new QListWidgetItem(d.icon, d.name);
        m_nav->addItem(it);
        m_pageSubtitles << d.subtitle;
    }

    // ---------------- painel de conteúdo: cabeçalho + páginas ------------
    // cabeçalho com gradiente suave (cinza bem claro -> branco), título em
    // negrito, subtítulo menor e ícone da seção no canto superior direito
    QWidget* header = new QWidget(this);
    header->setObjectName(QStringLiteral("pageHeader"));
    header->setMinimumHeight(66);
    header->setMaximumHeight(66);
    header->setAttribute(Qt::WA_StyledBackground, true);
    QHBoxLayout* hh = new QHBoxLayout(header);
    hh->setContentsMargins(16, 8, 14, 8);
    hh->setSpacing(10);
    QVBoxLayout* hv = new QVBoxLayout;
    hv->setContentsMargins(0, 0, 0, 0);
    hv->setSpacing(2);
    m_headerTitle = new QLabel(header);
    m_headerTitle->setObjectName(QStringLiteral("pageTitle"));
    m_headerSubtitle = new QLabel(header);
    m_headerSubtitle->setObjectName(QStringLiteral("pageSubtitle"));
    hv->addStretch(1);
    hv->addWidget(m_headerTitle);
    hv->addWidget(m_headerSubtitle);
    hv->addStretch(1);
    hh->addLayout(hv, 1);
    m_headerIcon = new QLabel(header);
    m_headerIcon->setObjectName(QStringLiteral("pageIcon"));
    m_headerIcon->setFixedSize(44, 44);
    m_headerIcon->setAlignment(Qt::AlignCenter);
    hh->addWidget(m_headerIcon, 0, Qt::AlignRight | Qt::AlignVCenter);

    m_stack = new QStackedWidget(this);
    m_stack->setObjectName(QStringLiteral("optionsStack"));
    m_stack->setAttribute(Qt::WA_StyledBackground, true);

    // páginas (mesma ordem do menu lateral)
    m_stack->addWidget(wrapScroll(pageApplication()));
    m_stack->addWidget(wrapScroll(pagePlayback()));
    m_stack->addWidget(wrapScroll(pageCapture()));
    m_stack->addWidget(wrapScroll(pageDesign()));
    m_stack->addWidget(wrapScroll(pageNotifications()));
    m_stack->addWidget(wrapScroll(pageHotkeys()));
    m_stack->addWidget(wrapScroll(pageSecurity()));
    m_stack->addWidget(wrapScroll(pageAddons()));

    QWidget* right = new QWidget(this);
    QVBoxLayout* rl = new QVBoxLayout(right);
    rl->setContentsMargins(0, 0, 0, 0);
    rl->setSpacing(0);
    rl->addWidget(header);
    rl->addWidget(m_stack, 1);

    mid->addWidget(m_nav);
    mid->addWidget(separatorLine(true)); // linha vertical de 1px
    mid->addWidget(right, 1);
    root->addLayout(mid, 1);

    connect(m_nav, &QListWidget::currentRowChanged, this, [this](int row) {
        if (row < 0 || row >= m_nav->count()) return;
        m_stack->setCurrentIndex(row);
        QListWidgetItem* it = m_nav->item(row);
        m_headerTitle->setText(it->text());
        m_headerSubtitle->setText(m_pageSubtitles.value(row));
        m_headerIcon->setPixmap(headerIconPixmap(it->icon(), 40));
    });
    m_nav->setCurrentRow(0);

    // ---------------- rodapé: OK / Cancelar / Aplicar --------------------
    root->addWidget(separatorLine(false)); // linha fina acima dos botões
    QHBoxLayout* bottom = new QHBoxLayout;
    bottom->setContentsMargins(10, 7, 10, 0);
    bottom->setSpacing(8);
    bottom->addStretch(1);
    QPushButton* ok = new QPushButton(tr("OK"), this);
    QPushButton* cancel = new QPushButton(tr("Cancelar"), this);
    QPushButton* applyBtn = new QPushButton(tr("Aplicar"), this);
    ok->setMinimumWidth(86);
    cancel->setMinimumWidth(86);
    applyBtn->setMinimumWidth(86);
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
// duas colunas, como na janela de opções do TS3
QWidget* OptionsDialog::pageApplication() {
    QWidget* w = new QWidget;
    QHBoxLayout* cols = new QHBoxLayout(w);
    cols->setSpacing(8);

    QVBoxLayout* left = new QVBoxLayout;
    QVBoxLayout* right = new QVBoxLayout;
    left->setSpacing(10);
    right->setSpacing(10);

    // ===== coluna esquerda =====
    QGroupBox* gbStart = new QGroupBox(tr("Inicialização"), w);
    QVBoxLayout* v1 = new QVBoxLayout(gbStart);
    QCheckBox* restore = new QCheckBox(tr("Restaurar as conexões da sessão anterior"), gbStart);
    restore->setChecked(S::flag("app/restoreTabs", false));
    connect(restore, &QCheckBox::toggled, this, [](bool v) { S::set("app/restoreTabs", v); });
    v1->addWidget(restore);
    left->addWidget(gbStart);

    QGroupBox* gbMisc = new QGroupBox(tr("Diversos"), w);
    QVBoxLayout* v2 = new QVBoxLayout(gbMisc);
    QCheckBox* advanced = new QCheckBox(tr("Sistema de permissões avançado"), gbMisc);
    advanced->setChecked(S::flag("app/advancedPerms", false));
    connect(advanced, &QCheckBox::toggled, this, [](bool v) { S::set("app/advancedPerms", v); });
    v2->addWidget(advanced);
    left->addWidget(gbMisc);
    left->addStretch(1);

    // ===== coluna direita =====
    QGroupBox* gbLang = new QGroupBox(tr("Idioma"), w);
    QFormLayout* fl = new QFormLayout(gbLang);
    QComboBox* lang = new QComboBox(gbLang);
    lang->addItems({ QStringLiteral("Português (Brasil)"), QStringLiteral("English"),
                     QStringLiteral("Deutsch"), QStringLiteral("Español"),
                     QStringLiteral("Français") });
    lang->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    lang->setMinimumContentsLength(8);
    lang->setCurrentIndex(S::num("app/language", 0));
    fl->addRow(tr("Idioma:"), lang);
    connect(lang, &QComboBox::currentIndexChanged, this, [this](int idx) {
        S::set("app/language", idx);
    });
    right->addWidget(gbLang);

    QGroupBox* gbUpd = new QGroupBox(tr("Atualizações"), w);
    QVBoxLayout* v3 = new QVBoxLayout(gbUpd);
    QCheckBox* autoupdate = new QCheckBox(tr("Procurar atualizações automaticamente"), gbUpd);
    autoupdate->setChecked(S::flag("app/autoUpdate", true));
    connect(autoupdate, &QCheckBox::toggled, this, [](bool v) { S::set("app/autoUpdate", v); });
    v3->addWidget(autoupdate);
    QPushButton* checkNow = new QPushButton(tr("Verificar agora"), gbUpd);
    connect(checkNow, &QPushButton::clicked, this, [this] {
        QMessageBox::information(this, tr("Atualização"),
            tr("Você já está usando a versão mais recente do Halla."));
    });
    v3->addWidget(checkNow, 0, Qt::AlignLeft);
    right->addWidget(gbUpd);

    QGroupBox* gbWin = new QGroupBox(tr("Janela principal"), w);
    QVBoxLayout* v4 = new QVBoxLayout(gbWin);
    QCheckBox* tray = new QCheckBox(tr("Fechar para a bandeja do sistema"), gbWin);
    tray->setChecked(S::flag("app/closeToTray", false));
    connect(tray, &QCheckBox::toggled, this, [](bool v) { S::set("app/closeToTray", v); });
    v4->addWidget(tray);
    QCheckBox* confirm = new QCheckBox(tr("Confirmar ao sair estando conectado"), gbWin);
    confirm->setChecked(S::flag("app/confirmQuit", true));
    connect(confirm, &QCheckBox::toggled, this, [](bool v) { S::set("app/confirmQuit", v); });
    v4->addWidget(confirm);
    right->addWidget(gbWin);
    right->addStretch(1);

    cols->addLayout(left, 1);
    cols->addLayout(right, 1);
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
        { "notify/userJoinSound",   tr("Quando um cliente entra no servidor") },
        { "notify/userLeftSound",   tr("Quando um cliente sai do servidor") },
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

    lay->addSpacing(10);
    QCheckBox* tts = new QCheckBox(tr("Narrar eventos com voz (texto-para-voz, "
                                    "como o pacote de voz do TS3)"), w);
    tts->setChecked(S::flag("notify/ttsEnabled", false));
    connect(tts, &QCheckBox::toggled, this,
            [](bool v) { S::set("notify/ttsEnabled", v); });
    lay->addWidget(tts);

    QHBoxLayout* row = new QHBoxLayout;
    row->addSpacing(20);
    QPushButton* test = new QPushButton(tr("Reproduzir som de teste"), w);
    row->addWidget(test);
    row->addStretch(1);
    lay->addLayout(row);
    connect(test, &QPushButton::clicked, this, [] { HSound::play(QStringLiteral("test")); });

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
    connect(test, &QPushButton::clicked, this, [] { HSound::play(QStringLiteral("test")); });

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

    // ativação de voz: escolha EXCLUSIVA via radio buttons (como no TS3)
    QGroupBox* gbMode = new QGroupBox(tr("Ativação de voz"), w);
    QVBoxLayout* vm = new QVBoxLayout(gbMode);
    QRadioButton* rbPtt = new QRadioButton(tr("Pressionar para falar (PTT)"), gbMode);
    QRadioButton* rbVad = new QRadioButton(tr("Detecção de voz"), gbMode);
    QRadioButton* rbCont = new QRadioButton(tr("Transmissão contínua"), gbMode);
    const int curMode = S::num("capture/pttMode", 1);
    if (curMode == 0)      rbPtt->setChecked(true);
    else if (curMode == 2) rbCont->setChecked(true);
    else                   rbVad->setChecked(true);
    connect(rbPtt,  &QRadioButton::toggled, this, [](bool v) { if (v) S::set("capture/pttMode", 0); });
    connect(rbVad,  &QRadioButton::toggled, this, [](bool v) { if (v) S::set("capture/pttMode", 1); });
    connect(rbCont, &QRadioButton::toggled, this, [](bool v) { if (v) S::set("capture/pttMode", 2); });
    vm->addWidget(rbPtt);
    vm->addWidget(rbVad);
    vm->addWidget(rbCont);
    form->addRow(gbMode);

    HotkeyEdit* pttKey = new HotkeyEdit(w);
    pttKey->setSpec(S::str("capture/pttKey", "Space"));
    form->addRow(tr("Tecla PTT:"), pttKey);
    connect(pttKey, &HotkeyEdit::specChanged, this,
            [](const QString& s) { S::set("capture/pttKey", s); });
    QLabel* pttHint = new QLabel(tr("Aceita teclas e botões laterais do mouse "
                                    "(Mouse4/Mouse5). Funciona em segundo plano."), w);
    pttHint->setWordWrap(true);
    pttHint->setObjectName(QStringLiteral("captionLabel"));
    form->addRow(QString(), pttHint);

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
// alvos possíveis do sussurro (índice = valor salvo em "scope")
static QStringList whisperScopeNames() {
    return { OptionsDialog::tr("Canal atual"),
             OptionsDialog::tr("Canal atual e subcanais"),
             OptionsDialog::tr("Lista de usuários") };
}

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

    const QString whisperAction = tr("Sussurrar (segurar para falar)");

    // texto de exibição da ação (adiciona o alvo do sussurro entre parênteses)
    auto displayAction = [whisperAction](const QString& raw, int scope) {
        if (raw == whisperAction && scope >= 0 && scope <= 2)
            return QStringLiteral("%1 — %2").arg(raw, whisperScopeNames().value(scope));
        return raw;
    };

    auto loadTable = [table, displayAction] {
        table->setRowCount(0);
        QJsonDocument doc = QJsonDocument::fromJson(S::str("hotkeys/list").toUtf8());
        if (!doc.isArray()) return;
        for (const QJsonValue& v : doc.array()) {
            QJsonObject o = v.toObject();
            const QString raw = o["action"].toString();
            const int scope = o.contains("scope") ? o["scope"].toInt(1) : -1;
            int r = table->rowCount();
            table->insertRow(r);
            QTableWidgetItem* a = new QTableWidgetItem(displayAction(raw, scope));
            a->setData(Qt::UserRole, raw);       // ação "cru" (sem o alvo)
            a->setData(Qt::UserRole + 1, scope); // alvo do sussurro (-1 = n/a)
            table->setItem(r, 0, a);
            table->setItem(r, 1, new QTableWidgetItem(o["key"].toString()));
        }
    };
    loadTable();

    auto saveTable = [table, this] {
        QJsonArray arr;
        for (int r = 0; r < table->rowCount(); ++r) {
            QJsonObject o;
            o["action"] = table->item(r, 0)->data(Qt::UserRole).toString();
            o["key"] = table->item(r, 1)->text();
            const int scope = table->item(r, 0)->data(Qt::UserRole + 1).toInt();
            if (scope >= 0) o["scope"] = scope;
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
        whisperAction,
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
        // captura tecla OU botão do mouse (inclusive laterais) — igual ao TS3
        HotkeyEdit* key = new HotkeyEdit(&d);
        key->setMinimumWidth(220);
        QComboBox* scope = new QComboBox(&d);
        scope->addItems(whisperScopeNames());

        int curScope = 1;
        if (row >= 0) {
            const QString raw = table->item(row, 0)->data(Qt::UserRole).toString();
            action->setCurrentText(raw);
            key->setSpec(table->item(row, 1)->text());
            curScope = table->item(row, 0)->data(Qt::UserRole + 1).toInt();
            if (curScope < 0) curScope = 1;
            scope->setCurrentIndex(curScope);
        }
        f->addRow(tr("Ação:"), action);
        f->addRow(tr("Atalho:"), key);
        f->addRow(tr("Sussurrar para:"), scope);
        // o seletor de alvo só aparece quando a ação é "Sussurrar"
        auto syncScope = [=] {
            scope->setEnabled(action->currentText() == whisperAction);
            if (QLabel* lb = qobject_cast<QLabel*>(f->labelForField(scope)))
                lb->setEnabled(action->currentText() == whisperAction);
        };
        QObject::connect(action, &QComboBox::currentTextChanged, &d,
                         [syncScope](const QString&) { syncScope(); });
        syncScope();
        QLabel* hint = new QLabel(tr("Aceita teclas e botões do mouse (laterais, meio).\n"
                                     "O sussurro funciona \"segurando\" a tecla/botão, "
                                     "como o PTT do TeamSpeak."), &d);
        hint->setWordWrap(true);
        hint->setObjectName(QStringLiteral("captionLabel"));
        f->addRow(QString(), hint);
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

        const QString raw = action->currentText();
        const bool isWhisper = (raw == whisperAction);
        const int sc = isWhisper ? scope->currentIndex() : -1;
        if (isWhisper) S::set("hotkeys/whisperScope", sc); // alvo p/ alternância
        if (row < 0) {
            row = table->rowCount();
            table->insertRow(row);
        }
        QTableWidgetItem* a = new QTableWidgetItem(displayAction(raw, sc));
        a->setData(Qt::UserRole, raw);
        a->setData(Qt::UserRole + 1, sc);
        table->setItem(row, 0, a);
        table->setItem(row, 1, new QTableWidgetItem(key->spec()));
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
        tr("No Windows, as teclas de atalho funcionam GLOBALMENTE (mesmo com o "
           "Halla em segundo plano) e aceitam botões do mouse. "
           "A ação \"Sussurrar\" envia sua voz apenas para o alvo escolhido "
           "enquanto a tecla estiver pressionada."),
        w);
    note->setObjectName(QStringLiteral("captionLabel"));
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
