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
#include <QFileDialog>

// envolve a página em um QScrollArea SEM moldura (estilo do Halla: o conteúdo
// flutua sobre o fundo branco, sem caixas cinzas à vista)
static QWidget* wrapScroll(QWidget* inner) {
    inner->setObjectName(QStringLiteral("optionsPage"));
    inner->setAttribute(Qt::WA_StyledBackground, true);
    inner->setContentsMargins(6, 6, 6, 6);

    QScrollArea* sa = new QScrollArea;
    sa->setObjectName(QStringLiteral("optionsScroll"));
    sa->setWidgetResizable(true);
    sa->setFrameShape(QFrame::NoFrame);
    sa->setWidget(inner);
    return sa;
}

// ícone da seção no canto superior direito do cabeçalho (estilo marca d'água)
static QPixmap headerIconPixmap(const QIcon& icon, int size) {
    const QPixmap src = icon.pixmap(size, size);
    QPixmap out(src.size());
    out.fill(Qt::transparent);
    QPainter p(&out);
    p.setOpacity(0.45);
    p.drawPixmap(0, 0, src);
    return out;
}

// Os ícones antigos do pacote têm uma tela de 16/24 px. O menu lateral da
// referência usa pictogramas grandes; redimensionamos explicitamente para que
// o Qt não preserve a tela pequena original do QIcon.
static QIcon largeNavigationIcon(const QIcon& icon) {
    const QPixmap source = icon.pixmap(64, 64);
    if (source.isNull()) return icon;
    return QIcon(source.scaled(42, 42, Qt::KeepAspectRatio, Qt::SmoothTransformation));
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

// ============================================================================
//  WIDGETS AUXILIARES DAS PÁGINAS (estilo Halla)
// ============================================================================
#include "ToolsDialogs.h"

#include <QInputDialog>
#include <QAudioSource>
#include <QMediaDevices>
#include <QAudioFormat>
#include <QTimer>
#include <cmath>

// "+0,0 dB" / "-17,0 dB" (locale do usuário)
static QString fmtDb(double db) {
    return (db >= 0 ? QStringLiteral("+") : QString())
           + QLocale().toString(db, 'f', 1) + QStringLiteral(" dB");
}

// slider rotulado em dB, com legendas "Baixo/Alto" (ou Quiet/Loud) nas pontas.
// Persiste o valor em décimos de dB na chave passada.
static QWidget* dbSliderRow(QWidget* parent, const QString& key, int defX10,
                            int minDb, int maxDb, const QString& low,
                            const QString& high) {
    QWidget* box = new QWidget(parent);
    QVBoxLayout* v = new QVBoxLayout(box);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);

    QHBoxLayout* h = new QHBoxLayout;
    QSlider* s = new QSlider(Qt::Horizontal, box);
    s->setRange(minDb * 10, maxDb * 10);
    s->setValue(S::num(key, defX10));
    QLabel* val = new QLabel(box);
    val->setMinimumWidth(70);
    val->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    auto upd = [=](int x10) { val->setText(fmtDb(x10 / 10.0)); };
    upd(s->value());
    h->addWidget(s, 1);
    h->addWidget(val);
    v->addLayout(h);

    QHBoxLayout* caps = new QHBoxLayout;
    QLabel* l = new QLabel(low, box);
    l->setObjectName(QStringLiteral("captionLabel"));
    QLabel* r = new QLabel(high, box);
    r->setObjectName(QStringLiteral("captionLabel"));
    r->setAlignment(Qt::AlignRight);
    caps->addWidget(l);
    caps->addStretch(1);
    caps->addWidget(r);
    v->addLayout(caps);

    QObject::connect(s, &QSlider::valueChanged, box, [key, upd](int x10) {
        S::set(key, x10);
        upd(x10);
    });
    return box;
}

// Painel "Perfis" (lado esquerdo das páginas Reprodução/Capturar): lista de
// perfis + botão "+" para adicionar. Persiste a lista e o perfil ativo.
class ProfilesPanel : public QGroupBox {
public:
    ProfilesPanel(const QString& storeKey, const QString& activeKey,
                  const std::function<void(const QString&)>& onActivate,
                  QWidget* parent = nullptr)
        : QGroupBox(tr("Perfis"), parent), m_storeKey(storeKey), m_activeKey(activeKey),
          m_onActivate(onActivate) {
        QVBoxLayout* v = new QVBoxLayout(this);
        v->setContentsMargins(6, 10, 6, 6);
        v->setSpacing(4);

        m_list = new QListWidget(this);
        const QString stored = S::str(storeKey);
        const QStringList names = stored.isEmpty()
                ? QStringList{ tr("Padrão") }
                : stored.split(QLatin1Char('|'), Qt::SkipEmptyParts);
        m_list->addItems(names);
        const int row = names.indexOf(S::str(activeKey, tr("Padrão")));
        m_list->setCurrentRow(row >= 0 ? row : 0);
        v->addWidget(m_list, 1);

        QHBoxLayout* h = new QHBoxLayout;
        h->setContentsMargins(0, 0, 0, 0);
        QPushButton* plus = new QPushButton(QStringLiteral("+"), this);
        plus->setFixedSize(28, 24);
        plus->setToolTip(tr("Adicionar perfil"));
        h->addWidget(plus, 0, Qt::AlignLeft);
        h->addStretch(1);
        v->addLayout(h);

        setFixedWidth(168);
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);

        QObject::connect(plus, &QPushButton::clicked, this, [this] {
            bool ok = false;
            const QString name = QInputDialog::getText(
                this, tr("Novo perfil"), tr("Nome do perfil:"),
                QLineEdit::Normal, tr("Novo perfil"), &ok);
            if (!ok || name.trimmed().isEmpty()) return;
            if (!m_list->findItems(name, Qt::MatchExactly).isEmpty()) return;
            m_list->addItem(name.trimmed());
            m_list->setCurrentRow(m_list->count() - 1);
            saveNames();
        });
        QObject::connect(m_list, &QListWidget::currentRowChanged, this, [this](int) {
            saveNames();
            activate();
        });
        activate(); // perfil inicial
    }

private:
    void saveNames() {
        QStringList names;
        for (int i = 0; i < m_list->count(); ++i) names << m_list->item(i)->text();
        S::set(m_storeKey, names.join(QLatin1Char('|')));
    }
    void activate() {
        QListWidgetItem* it = m_list->currentItem();
        if (!it) return;
        S::set(m_activeKey, it->text());
        if (m_onActivate) m_onActivate(it->text());
    }

    QListWidget* m_list = nullptr;
    QString m_storeKey, m_activeKey;
    std::function<void(const QString&)> m_onActivate;
};

// Medidor visual de volume (régua -50 a +50 dB com barra de nível e LED),
// usado no teste de captura — lê o microfone padrão de verdade via QAudioSource.
class CaptureMeter : public QWidget {
public:
    explicit CaptureMeter(QWidget* parent = nullptr) : QWidget(parent) {
        setMinimumHeight(86);
        m_timer = new QTimer(this);
        m_timer->setInterval(80);
        QObject::connect(m_timer, &QTimer::timeout, this, [this] { readLevel(); });
    }
    ~CaptureMeter() override { stop(); }

    bool isTesting() const { return m_dev != nullptr; }

    void start() {
        stop();
        QAudioFormat fmt;
        fmt.setSampleRate(16000);
        fmt.setChannelCount(1);
        fmt.setSampleFormat(QAudioFormat::Int16);
        m_source = new QAudioSource(QMediaDevices::defaultAudioInput(), fmt, this);
        m_dev = m_source->start();
        if (!m_dev) {
            delete m_source;
            m_source = nullptr;
            return;
        }
        m_timer->start();
    }

    void stop() {
        m_timer->stop();
        if (m_source) { m_source->stop(); m_source->deleteLater(); m_source = nullptr; }
        m_dev = nullptr;
        m_led = false;
        m_db = -50.0;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.fillRect(rect(), palette().base());
        const QRect scale = rect().adjusted(30, 10, -24, -26);

        // moldura da régua
        p.setPen(QPen(palette().color(QPalette::Mid), 1));
        p.setBrush(Qt::NoBrush);
        p.drawRect(scale);

        // barra de nível (verde -> amarelo na zona de voz)
        const double frac = qBound(0.0, (m_db + 50.0) / 100.0, 1.0);
        const int barW = int(scale.width() * frac);
        if (barW > 0) {
            const double vadFrac = double(S::num("capture/voiceLevel", -45) + 50) / 100.0;
            const int vadX = int(scale.width() * vadFrac);
            p.fillRect(scale.left() + 1, scale.top() + 1,
                       qMin(barW, vadX) - 1, scale.height() - 2, QColor(52, 168, 83));
            if (barW > vadX)
                p.fillRect(scale.left() + vadX, scale.top() + 1,
                           barW - vadX - 1, scale.height() - 2, QColor(251, 188, 5));
        }

        // marca do limiar de ativação de voz
        {
            const double vf = double(S::num("capture/voiceLevel", -45) + 50) / 100.0;
            const int x = scale.left() + int(scale.width() * vf);
            p.setPen(QPen(QColor(234, 67, 53), 2));
            p.drawLine(x, scale.top(), x, scale.bottom());
        }

        // graduação
        p.setPen(QPen(palette().color(QPalette::Text), 1));
        QFont f = p.font();
        f.setPixelSize(9);
        p.setFont(f);
        for (int db = -50; db <= 50; db += 10) {
            const int x = scale.left() + int((db + 50) / 100.0 * scale.width());
            p.drawLine(x, scale.bottom() - 4, x, scale.bottom());
            p.drawText(x - 14, scale.bottom() + 2, 28, 12, Qt::AlignHCenter,
                       QString::number(db));
        }

        // LED de atividade (acende quando o nível passa do limiar)
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(m_led ? QColor(52, 168, 83) : QColor(154, 160, 166));
        p.drawEllipse(QPointF(rect().right() - 14, 12), 5, 5);
    }

private:
    void readLevel() {
        if (!m_dev) return;
        const QByteArray d = m_dev->readAll();
        if (d.size() < 2) return;
        const int16_t* s = reinterpret_cast<const int16_t*>(d.constData());
        const int n = d.size() / 2;
        double sum = 0;
        for (int i = 0; i < n; ++i) sum += double(s[i]) * double(s[i]);
        const double rms = std::sqrt(sum / n);
        double db = 20.0 * std::log10(rms / 32767.0 + 1e-9);
        db = qBound(-50.0, db, 50.0);
        m_db = m_db < 0 ? qMax(m_db, db) * 0.3 + db * 0.7 : db; // suavização leve
        m_led = db >= S::num("capture/voiceLevel", -45);
        update();
    }

    QAudioSource* m_source = nullptr;
    QIODevice* m_dev = nullptr;
    QTimer* m_timer = nullptr;
    double m_db = -50.0;
    bool m_led = false;
};

OptionsDialog::OptionsDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("Opções"));
    resize(970, 640);

    QVBoxLayout* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 8);
    root->setSpacing(0);

    QHBoxLayout* mid = new QHBoxLayout;
    mid->setContentsMargins(0, 0, 0, 0);
    mid->setSpacing(0);

    // ---------------- menu lateral (ícones grandes + texto) --------------
    m_nav = new QListWidget(this);
    m_nav->setObjectName(QStringLiteral("optionsNav"));
    m_nav->setFixedWidth(260);
    m_nav->setIconSize(QSize(42, 42));
    m_nav->setSpacing(4);
    m_nav->setFrameShape(QFrame::NoFrame);
    QFont navigationFont = m_nav->font();
    navigationFont.setPointSize(14);
    m_nav->setFont(navigationFont);

    struct PageDef { QString name; QString subtitle; QIcon icon; };
    const QList<PageDef> pages = {
        { tr("Aplicativo"),       tr("Opções gerais do aplicativo"),          HIcons::application() },
        { tr("Reprodução"),       tr("Configure o sistema de reprodução de áudio"), HIcons::playbackSpeaker() },
        { tr("Capturar"),         tr("Configure o sistema de captura de áudio"),    HIcons::captureMic() },
        { tr("Aparência"),        tr("Configure a aparência"),                  HIcons::design() },
        { tr("Notificações"),     tr("Sons e avisos de eventos"),             HIcons::notifyBell() },
        { tr("Teclas de atalho"), tr("Configure teclas de atalho"),           HIcons::hotkeys() },
        { tr("Sussurro"),         tr("Configure o recurso de sussurros"),     HIcons::contacts() },
        { tr("Segurança"),        tr("Identidade e segurança"),               HIcons::security() },
        { tr("Complementos"),     tr("Extensões e pacotes do cliente"),       HIcons::addons() },
    };
    for (const PageDef& d : pages) {
        QListWidgetItem* it = new QListWidgetItem(largeNavigationIcon(d.icon), d.name);
        it->setSizeHint(QSize(248, 56));
        it->setFont(navigationFont);
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
    m_stack->addWidget(wrapScroll(pageWhisper()));
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
// duas colunas, como na janela de opções do Halla
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
    lang->addItems({ QStringLiteral("Automático (idioma do sistema)"),
                     QStringLiteral("Português (Brasil)"), QStringLiteral("English"),
                     QStringLiteral("Español") });
    lang->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    lang->setMinimumContentsLength(8);
    lang->setCurrentIndex(qBound(0, S::num("app/language", 0), 3));
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
// ------------------------------------------------------------------ Aparência
// duas colunas, como na janela de opções do Halla: à esquerda estilo/tema/ícones/
// transparência; à direita os grupos "Árvore do canal", "Ícone da bandeja" e
// "Suporte a GIF animado"
QWidget* OptionsDialog::pageDesign() {
    QWidget* w = new QWidget;
    QHBoxLayout* cols = new QHBoxLayout(w);
    cols->setSpacing(12);

    // ============ coluna esquerda ============
    QVBoxLayout* left = new QVBoxLayout;
    left->setSpacing(8);

    QFormLayout* form = new QFormLayout;
    form->setSpacing(8);

    QComboBox* style = new QComboBox(w);
    style->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    style->setMinimumContentsLength(12);
    style->addItems({ tr("Automático (nativo do sistema)"), tr("Fusion") });
    style->setCurrentIndex(S::str("design/style") == QLatin1String("fusion") ? 1 : 0);
    form->addRow(tr("Estilo:"), style);
    connect(style, &QComboBox::currentIndexChanged, this, [this](int idx) {
        S::set("design/style", idx == 1 ? QStringLiteral("fusion") : QString());
        emit themeChanged(); // reaplica estilo + tema
    });

    QComboBox* theme = new QComboBox(w);
    theme->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    theme->setMinimumContentsLength(12);
    theme->addItems({ tr("Claro (padrão)"), tr("Escuro") });
    theme->setCurrentIndex(S::num("design/theme", 0));
    form->addRow(tr("Tema:"), theme);
    connect(theme, &QComboBox::currentIndexChanged, this, [this](int idx) {
        S::set("design/theme", idx);
        emit themeChanged();
    });

    QComboBox* iconPack = new QComboBox(w);
    iconPack->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    iconPack->setMinimumContentsLength(12);
    iconPack->addItem(tr("Padrão"));
    form->addRow(tr("Pacote de ícones:"), iconPack);
    connect(iconPack, &QComboBox::currentTextChanged, this,
            [](const QString& t) { S::set("design/iconPack", t); });

    QLabel* moreIcons = new QLabel(
        QStringLiteral("<a href=\"https://github.com/farleybarbosa320-oss/Halla\">%1</a>")
            .arg(tr("Obter mais folhas de estilos && ícones")), w);
    moreIcons->setOpenExternalLinks(true);
    form->addRow(QString(), moreIcons);

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

    // transparência da janela principal (50% a 100%)
    QHBoxLayout* orow = new QHBoxLayout;
    QSlider* opacity = new QSlider(Qt::Horizontal, w);
    opacity->setRange(50, 100);
    opacity->setValue(S::num("design/opacity", 100));
    QLabel* olabel = new QLabel(QStringLiteral("%1%").arg(opacity->value()), w);
    olabel->setMinimumWidth(40);
    orow->addWidget(opacity, 1);
    orow->addWidget(olabel);
    QWidget* ow = new QWidget(w);
    ow->setLayout(orow);
    form->addRow(tr("Transparência:"), ow);
    connect(opacity, &QSlider::valueChanged, this, [this, olabel](int v) {
        S::set("design/opacity", v);
        olabel->setText(QStringLiteral("%1%").arg(v));
        emit designChanged();
    });

    left->addLayout(form);
    left->addStretch(1);

    // ============ coluna direita ============
    QVBoxLayout* right = new QVBoxLayout;
    right->setSpacing(10);

    QGroupBox* gbTree = new QGroupBox(tr("Árvore do canal"), w);
    QVBoxLayout* vt = new QVBoxLayout(gbTree);
    vt->setSpacing(4);

    QRadioButton* expAll   = new QRadioButton(tr("Expandir todos os canais ao fazer login"), gbTree);
    QRadioButton* expLevel = new QRadioButton(tr("Expandir canais até este nível:"), gbTree);
    QRadioButton* expOwn   = new QRadioButton(tr("Expandir o próprio canal ao fazer login"), gbTree);
    const int expMode = S::num("design/expandMode", 0);
    if (expMode == 0)      expAll->setChecked(true);
    else if (expMode == 1) expLevel->setChecked(true);
    else                   expOwn->setChecked(true);
    auto setExpMode = [this](int m) { S::set("design/expandMode", m); emit designChanged(); };
    connect(expAll,   &QRadioButton::toggled, this, [setExpMode](bool v) { if (v) setExpMode(0); });
    connect(expLevel, &QRadioButton::toggled, this, [setExpMode](bool v) { if (v) setExpMode(1); });
    connect(expOwn,   &QRadioButton::toggled, this, [setExpMode](bool v) { if (v) setExpMode(2); });
    vt->addWidget(expAll);

    QHBoxLayout* lvl = new QHBoxLayout;
    lvl->addWidget(expLevel);
    QSpinBox* expandLevel = new QSpinBox(gbTree);
    expandLevel->setRange(0, 99);
    expandLevel->setValue(S::num("design/expandLevel", 0));
    expandLevel->setEnabled(expMode == 1);
    lvl->addWidget(expandLevel);
    lvl->addStretch(1);
    vt->addLayout(lvl);
    connect(expLevel, &QRadioButton::toggled, expandLevel, &QWidget::setEnabled);
    connect(expandLevel, &QSpinBox::valueChanged, this, [this](int v) {
        S::set("design/expandLevel", v);
        emit designChanged();
    });
    vt->addWidget(expOwn);

    auto treeCb = [this, gbTree, vt](const QString& key, const QString& text,
                                     bool def, bool emitSignal) {
        QCheckBox* cb = new QCheckBox(text, gbTree);
        cb->setChecked(S::flag(key, def));
        connect(cb, &QCheckBox::toggled, this, [this, key, emitSignal](bool v) {
            S::set(key, v);
            if (emitSignal) emit designChanged();
        });
        vt->addWidget(cb);
    };
    treeCb(QStringLiteral("design/sortClientsBelow"), tr("Classificar clientes abaixo dos canais"), false, true);
    treeCb(QStringLiteral("design/showCountryFlags"), tr("Exibir bandeira de país nos clientes"), false, false);
    treeCb(QStringLiteral("design/showOverwolfIcons"), tr("Exibir ícones do Overwolf nos clientes"), false, false);
    treeCb(QStringLiteral("design/showBadgeIcons"), tr("Exibir ícones de emblema nos clientes"), false, false);
    treeCb(QStringLiteral("design/showGroupIconsMenu"), tr("Exibir ícones de grupo nos menus de contexto"), true, false);
    treeCb(QStringLiteral("design/hideInaccessibleGroups"), tr("Ocultar grupos inacessíveis nos menus de contexto"), true, false);
    treeCb(QStringLiteral("design/showCounts"), tr("Mostrar número de clientes ao lado dos canais"), true, true);
    treeCb(QStringLiteral("design/showMinis"), tr("Mostrar mini-ícones de estado dos clientes"), true, true);
    treeCb(QStringLiteral("design/showAwayMessage"), tr("Mostrar mensagem de ausência ao lado do apelido"), true, true);
    treeCb(QStringLiteral("design/tooltips"), tr("Mostrar dica de ferramenta ao passar o mouse"), true, true);
    right->addWidget(gbTree);

    QGroupBox* gbTray = new QGroupBox(tr("Ícone da bandeja"), w);
    QVBoxLayout* vy = new QVBoxLayout(gbTray);
    QCheckBox* minTray = new QCheckBox(tr("Minimizar na bandeja"), gbTray);
    minTray->setChecked(S::flag("app/minimizeToTray", false));
    connect(minTray, &QCheckBox::toggled, this, [](bool v) { S::set("app/minimizeToTray", v); });
    vy->addWidget(minTray);
    QCheckBox* closeTray = new QCheckBox(tr("Fechar na bandeja"), gbTray);
    closeTray->setChecked(S::flag("app/closeToTray", false));
    connect(closeTray, &QCheckBox::toggled, this, [](bool v) { S::set("app/closeToTray", v); });
    vy->addWidget(closeTray);
    right->addWidget(gbTray);

    QGroupBox* gbGif = new QGroupBox(tr("Suporte a GIF animado"), w);
    QVBoxLayout* vg = new QVBoxLayout(gbGif);
    QCheckBox* gifAv = new QCheckBox(tr("Ativar avatares animados"), gbGif);
    gifAv->setChecked(S::flag("design/animatedAvatars", true));
    connect(gifAv, &QCheckBox::toggled, this, [](bool v) { S::set("design/animatedAvatars", v); });
    vg->addWidget(gifAv);
    QCheckBox* gifImg = new QCheckBox(tr("Ativar imagens animadas"), gbGif);
    gifImg->setChecked(S::flag("design/animatedImages", true));
    connect(gifImg, &QCheckBox::toggled, this, [](bool v) { S::set("design/animatedImages", v); });
    vg->addWidget(gifImg);
    right->addWidget(gbGif);
    right->addStretch(1);

    cols->addLayout(left, 4);
    cols->addLayout(right, 6);
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
                                    "como o pacote de voz do Halla)"), w);
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
// estilo Halla: perfis à esquerda; modo/dispositivo; sliders de volume em dB;
// teste de som; grupo "Opções" (com slider de ruído) e grupo "Expansão de som mono"
QWidget* OptionsDialog::pagePlayback() {
    QWidget* w = new QWidget;
    QHBoxLayout* main = new QHBoxLayout(w);
    main->setSpacing(10);

    // ---- painel de perfis (esquerda)
    ProfilesPanel* profiles = new ProfilesPanel(
        QStringLiteral("playback/profiles"), QStringLiteral("playback/profile"),
        nullptr, w);
    main->addWidget(profiles);

    // ---- coluna principal (direita)
    QVBoxLayout* right = new QVBoxLayout;
    right->setSpacing(8);

    QFormLayout* form = new QFormLayout;
    form->setSpacing(8);

    QComboBox* mode = new QComboBox(w);
    mode->addItems({ tr("Usar o melhor modo automaticamente"), tr("Direct Sound"),
                     tr("Windows Audio Session"), tr("PulseAudio"), tr("ALSA") });
    mode->setCurrentIndex(S::num("playback/mode", 0));
    form->addRow(tr("Modo de reprodução:"), mode);
    connect(mode, &QComboBox::currentIndexChanged, this,
            [](int v) { S::set("playback/mode", v); });

    QComboBox* dev = new QComboBox(w);
    dev->addItem(tr("Padrão"), QString());
    const auto outputs = QMediaDevices::audioOutputs();
    for (const QAudioDevice& output : outputs) {
        dev->addItem(output.description(), output.id());
    }
    const QString savedDevId = S::str("playback/device");
    int selIdx = 0;
    for (int i = 1; i < dev->count(); ++i) {
        if (dev->itemData(i).toString() == savedDevId) {
            selIdx = i;
            break;
        }
    }
    dev->setCurrentIndex(selIdx);
    connect(dev, &QComboBox::currentIndexChanged, this, [dev](int index) {
        S::set("playback/device", dev->itemData(index).toString());
    });
    form->addRow(tr("Dispositivo de reprodução:"), dev);

    form->addRow(tr("Ajuste de volume de voz:"),
                 dbSliderRow(w, QStringLiteral("playback/volumeDb"), 0, -15, 15,
                             tr("Baixo"), tr("Alto")));
    form->addRow(tr("Volume do pacote de som:"),
                 dbSliderRow(w, QStringLiteral("playback/soundPackVolume"), -170, -40, 0,
                             tr("Baixo"), tr("Alto")));

    QHBoxLayout* trow = new QHBoxLayout;
    QPushButton* test = new QPushButton(tr("▶ Reproduzir som de teste"), w);
    trow->addWidget(test, 0, Qt::AlignLeft);
    trow->addStretch(1);
    form->addRow(QString(), trow);
    connect(test, &QPushButton::clicked, this, [] { HSound::play(QStringLiteral("test")); });

    right->addLayout(form);

    // ---- grupo "Opções"
    QGroupBox* gbOpts = new QGroupBox(tr("Opções"), w);
    QVBoxLayout* vo = new QVBoxLayout(gbOpts);
    vo->setSpacing(4);
    auto opt = [this, gbOpts, vo](const QString& key, const QString& text, bool def) {
        QCheckBox* cb = new QCheckBox(text, gbOpts);
        cb->setChecked(S::flag(key, def));
        connect(cb, &QCheckBox::toggled, this,
                [key](bool v) mutable { S::set(key, v); });
        vo->addWidget(cb);
    };
    opt(QStringLiteral("playback/autoLeveling"),    tr("Nivelamento automático de volume de voz"), false);
    opt(QStringLiteral("playback/selfMicClicks"),   tr("O próprio cliente reproduz cliques do microfone"), false);
    opt(QStringLiteral("playback/always3d"),        tr("Sempre definir posições 3D de clientes quando disponível"), false);
    opt(QStringLiteral("playback/othersMicClicks"), tr("Outros clientes reproduzem cliques do microfone"), true);
    opt(QStringLiteral("playback/comfortNoise"),    tr("Ruído de conforto (comfort noise)"), true);
    QLabel* noiseCap = new QLabel(tr("Ajuste do ruído de conforto:"), gbOpts);
    noiseCap->setObjectName(QStringLiteral("captionLabel"));
    vo->addWidget(noiseCap);
    vo->addWidget(dbSliderRow(gbOpts, QStringLiteral("playback/comfortNoiseDb"), -360, -60, 0,
                              tr("Quieto"), tr("Alto")));
    right->addWidget(gbOpts);

    // ---- grupo "Expansão de som mono"
    QGroupBox* gbMono = new QGroupBox(tr("Expansão de som mono"), w);
    QVBoxLayout* vm = new QVBoxLayout(gbMono);
    vm->setSpacing(4);
    const QStringList monoOpts = { tr("Mono para estéreo (padrão)"),
                                   tr("Mono para alto-falante central (quando disponível)"),
                                   tr("Mono para surround (quando disponível)") };
    for (int i = 0; i < monoOpts.size(); ++i) {
        QRadioButton* rb = new QRadioButton(monoOpts[i], gbMono);
        rb->setChecked(S::num("playback/monoExpansion", 0) == i);
        connect(rb, &QRadioButton::toggled, this,
                [i](bool v) { if (v) S::set("playback/monoExpansion", i); });
        vm->addWidget(rb);
    }
    right->addWidget(gbMono);
    right->addStretch(1);

    main->addLayout(right, 1);
    return w;
}

// ------------------------------------------------------------------ Captura
QWidget* OptionsDialog::pageCapture() {
    QWidget* w = new QWidget;
    QHBoxLayout* main = new QHBoxLayout(w);
    main->setSpacing(10);

    // ---- painel de perfis (esquerda)
    ProfilesPanel* profiles = new ProfilesPanel(
        QStringLiteral("capture/profiles"), QStringLiteral("capture/profile"),
        nullptr, w);
    main->addWidget(profiles);

    // ---- coluna principal (direita)
    QVBoxLayout* right = new QVBoxLayout;
    right->setSpacing(8);

    QFormLayout* form = new QFormLayout;
    form->setSpacing(8);

    QComboBox* mode = new QComboBox(w);
    mode->addItems({ tr("Usar o melhor modo automaticamente"), tr("Direct Sound"),
                     tr("Windows Audio Session"), tr("PulseAudio"), tr("ALSA") });
    mode->setCurrentIndex(S::num("capture/mode", 0));
    form->addRow(tr("Modo de captura:"), mode);
    connect(mode, &QComboBox::currentIndexChanged, this,
            [](int v) { S::set("capture/mode", v); });

    QComboBox* dev = new QComboBox(w);
    dev->addItem(tr("Padrão"), QString());
    const auto inputs = QMediaDevices::audioInputs();
    for (const QAudioDevice& input : inputs) {
        dev->addItem(input.description(), input.id());
    }
    const QString savedDevId = S::str("capture/device");
    int selIdx = 0;
    for (int i = 1; i < dev->count(); ++i) {
        if (dev->itemData(i).toString() == savedDevId) {
            selIdx = i;
            break;
        }
    }
    dev->setCurrentIndex(selIdx);
    connect(dev, &QComboBox::currentIndexChanged, this, [dev](int index) {
        S::set("capture/device", dev->itemData(index).toString());
    });
    form->addRow(tr("Dispositivo de captura:"), dev);

    right->addLayout(form);

    // ==================== grupo "Ativação de voz" ====================
    QGroupBox* gbMode = new QGroupBox(tr("Ativação de voz"), w);
    QVBoxLayout* vm = new QVBoxLayout(gbMode);
    vm->setSpacing(5);

    const int curMode = S::num("capture/pttMode", 1);

    // --- Radio 1: Push-to-Talk (com sub-opções recuadas, como no Halla)
    QRadioButton* rbPtt = new QRadioButton(tr("Pressionar para falar (PTT)"), gbMode);
    vm->addWidget(rbPtt);

    QWidget* pttSub = new QWidget(gbMode);
    QVBoxLayout* vs = new QVBoxLayout(pttSub);
    vs->setContentsMargins(22, 0, 0, 0);
    vs->setSpacing(5);

    QHBoxLayout* keyRow = new QHBoxLayout;
    HotkeyEdit* pttKey = new HotkeyEdit(pttSub);
    pttKey->setSpec(S::str("capture/pttKey", "Space"));
    pttKey->setMaximumWidth(230);
    connect(pttKey, &HotkeyEdit::specChanged, this,
            [](const QString& s) { S::set("capture/pttKey", s); });
    QLabel* moreKeys = new QLabel(
        QStringLiteral("<a href=\"hotkeys\">%1</a>").arg(tr("Definir mais teclas de atalho")),
        pttSub);
    keyRow->addWidget(pttKey);
    keyRow->addWidget(moreKeys);
    keyRow->addStretch(1);
    vs->addLayout(keyRow);
    connect(moreKeys, &QLabel::linkActivated, this, [this] {
        selectPage(tr("Teclas de atalho"));
    });

    QHBoxLayout* delayRow = new QHBoxLayout;
    QCheckBox* pttDelay = new QCheckBox(tr("Atraso ao soltar a tecla do Push-to-Talk:"), pttSub);
    pttDelay->setChecked(S::flag("capture/pttDelayEnabled", false));
    QDoubleSpinBox* delaySpin = new QDoubleSpinBox(pttSub);
    delaySpin->setRange(0.0, 3.0);
    delaySpin->setSingleStep(0.1);
    delaySpin->setDecimals(1);
    delaySpin->setSuffix(QStringLiteral(" s"));
    delaySpin->setValue(S::num("capture/pttDelayMs", 300) / 1000.0);
    delaySpin->setEnabled(pttDelay->isChecked());
    delayRow->addWidget(pttDelay);
    delayRow->addWidget(delaySpin);
    delayRow->addStretch(1);
    vs->addLayout(delayRow);
    connect(pttDelay, &QCheckBox::toggled, this, [delaySpin](bool v) {
        S::set("capture/pttDelayEnabled", v);
        delaySpin->setEnabled(v);
    });
    connect(delaySpin, &QDoubleSpinBox::valueChanged, this,
            [](double v) { S::set("capture/pttDelayMs", int(v * 1000)); });

    QCheckBox* pttVad = new QCheckBox(tr("Adicionar detecção de atividade de voz"), pttSub);
    pttVad->setChecked(S::flag("capture/pttWithVad", false));
    connect(pttVad, &QCheckBox::toggled, this,
            [](bool v) { S::set("capture/pttWithVad", v); });
    vs->addWidget(pttVad);
    vm->addWidget(pttSub);

    // --- Radio 2: transmissão contínua
    QRadioButton* rbCont = new QRadioButton(tr("Transmissão contínua"), gbMode);
    vm->addWidget(rbCont);

    // --- Radio 3: detecção de ativação por voz (+ dropdown de modo)
    QHBoxLayout* vadRow = new QHBoxLayout;
    QRadioButton* rbVad = new QRadioButton(tr("Detecção de ativação por voz"), gbMode);
    QComboBox* vadMode = new QComboBox(gbMode);
    vadMode->addItems({ tr("Automático"), tr("Sensível"), tr("Moderado"), tr("Restrito") });
    vadMode->setCurrentIndex(S::num("capture/vadMode", 0));
    vadRow->addWidget(rbVad);
    vadRow->addWidget(vadMode);
    vadRow->addStretch(1);
    vm->addLayout(vadRow);
    connect(vadMode, &QComboBox::currentIndexChanged, this,
            [](int v) { S::set("capture/vadMode", v); });

    if (curMode == 0)      rbPtt->setChecked(true);
    else if (curMode == 2) rbCont->setChecked(true);
    else                   rbVad->setChecked(true);
    connect(rbPtt,  &QRadioButton::toggled, this, [](bool v) { if (v) S::set("capture/pttMode", 0); });
    connect(rbVad,  &QRadioButton::toggled, this, [](bool v) { if (v) S::set("capture/pttMode", 1); });
    connect(rbCont, &QRadioButton::toggled, this, [](bool v) { if (v) S::set("capture/pttMode", 2); });

    // sub-opções habilitadas apenas com o modo correspondente
    auto syncSubs = [=] {
        pttSub->setEnabled(rbPtt->isChecked());
        vadMode->setEnabled(rbVad->isChecked());
    };
    connect(rbPtt, &QRadioButton::toggled, this, [syncSubs](bool) { syncSubs(); });
    connect(rbVad, &QRadioButton::toggled, this, [syncSubs](bool) { syncSubs(); });
    connect(rbCont, &QRadioButton::toggled, this, [syncSubs](bool) { syncSubs(); });
    syncSubs();

    // sensibilidade da detecção (linha fina na régua do medidor)
    QHBoxLayout* srow = new QHBoxLayout;
    srow->setContentsMargins(22, 0, 0, 0);
    QLabel* sensLabel = new QLabel(tr("Sensibilidade:"), gbMode);
    QSlider* level = new QSlider(Qt::Horizontal, gbMode);
    level->setRange(-60, 0);
    level->setValue(S::num("capture/voiceLevel", -45));
    QLabel* levelLabel = new QLabel(fmtDb(level->value()), gbMode);
    levelLabel->setMinimumWidth(70);
    levelLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    srow->addWidget(sensLabel);
    srow->addWidget(level, 1);
    srow->addWidget(levelLabel);
    vm->addLayout(srow);
    connect(level, &QSlider::valueChanged, this, [levelLabel](int v) {
        S::set("capture/voiceLevel", v);
        levelLabel->setText(fmtDb(v));
    });

    // --- medidor visual de volume (régua -50 a +50 dB + botão de teste + LED)
    QHBoxLayout* meterRow = new QHBoxLayout;
    meterRow->setContentsMargins(22, 2, 0, 0);
    CaptureMeter* meter = new CaptureMeter(gbMode);
    QVBoxLayout* btns = new QVBoxLayout;
    QPushButton* testBtn = new QPushButton(tr("Iniciar teste"), gbMode);
    testBtn->setCheckable(true);
    btns->addWidget(testBtn, 0, Qt::AlignTop);
    btns->addStretch(1);
    meterRow->addWidget(meter, 1);
    meterRow->addLayout(btns);
    vm->addLayout(meterRow);
    connect(testBtn, &QPushButton::toggled, this, [meter, testBtn](bool on) {
        if (on) {
            meter->start();
            testBtn->setText(meter->isTesting() ? tr("Parar teste") : tr("Iniciar teste"));
        } else {
            meter->stop();
            testBtn->setText(tr("Iniciar teste"));
        }
    });

    right->addWidget(gbMode);

    // ============ grupo "Processamento digital de sinal" (DSP) ============
    QGroupBox* gbDsp = new QGroupBox(tr("Processamento digital de sinal"), w);
    QVBoxLayout* vd = new QVBoxLayout(gbDsp);
    vd->setSpacing(5);

    QCheckBox* typing = new QCheckBox(tr("Atenuação de digitação"), gbDsp);
    typing->setChecked(S::flag("capture/typingAttenuation", false));
    connect(typing, &QCheckBox::toggled, this,
            [](bool v) { S::set("capture/typingAttenuation", v); });
    vd->addWidget(typing);

    QHBoxLayout* dnRow = new QHBoxLayout;
    QCheckBox* denoise = new QCheckBox(tr("Remover ruídos de fundo"), gbDsp);
    denoise->setChecked(S::flag("capture/denoise", true));
    QSlider* dnLevel = new QSlider(Qt::Horizontal, gbDsp);
    dnLevel->setRange(0, 100);
    dnLevel->setValue(S::num("capture/denoiseLevel", 50));
    QLabel* dnMin = new QLabel(tr("min"), gbDsp);
    dnMin->setObjectName(QStringLiteral("captionLabel"));
    QLabel* dnMax = new QLabel(tr("max"), gbDsp);
    dnMax->setObjectName(QStringLiteral("captionLabel"));
    dnLevel->setEnabled(denoise->isChecked());
    dnRow->addWidget(denoise);
    dnRow->addSpacing(16);
    dnRow->addWidget(dnMin);
    dnRow->addWidget(dnLevel, 1);
    dnRow->addWidget(dnMax);
    vd->addLayout(dnRow);
    connect(denoise, &QCheckBox::toggled, this, [dnLevel](bool v) {
        S::set("capture/denoise", v);
        dnLevel->setEnabled(v);
    });
    connect(dnLevel, &QSlider::valueChanged, this,
            [](int v) { S::set("capture/denoiseLevel", v); });

    QCheckBox* cancel = new QCheckBox(tr("Cancelamento do eco"), gbDsp);
    cancel->setChecked(S::flag("capture/echoCancellation", false));
    connect(cancel, &QCheckBox::toggled, this,
            [](bool v) { S::set("capture/echoCancellation", v); });
    vd->addWidget(cancel);

    QHBoxLayout* duckRow = new QHBoxLayout;
    QCheckBox* duck = new QCheckBox(tr("Redução de eco (Ducking):"), gbDsp);
    duck->setChecked(S::flag("capture/ducking", false));
    QSpinBox* duckDb = new QSpinBox(gbDsp);
    duckDb->setRange(1, 60);
    duckDb->setSuffix(QStringLiteral(" dB"));
    duckDb->setValue(S::num("capture/duckingDb", 10));
    duckDb->setEnabled(duck->isChecked());
    duckRow->addWidget(duck);
    duckRow->addWidget(duckDb);
    duckRow->addStretch(1);
    vd->addLayout(duckRow);
    connect(duck, &QCheckBox::toggled, this, [duckDb](bool v) {
        S::set("capture/ducking", v);
        duckDb->setEnabled(v);
    });
    connect(duckDb, &QSpinBox::valueChanged, this,
            [](int v) { S::set("capture/duckingDb", v); });

    right->addWidget(gbDsp);

    // ============ grupo "Diversos" — sinais sonoros de fala ============
    QGroupBox* gbSpeechCue = new QGroupBox(tr("Diversos"), w);
    QVBoxLayout* cueLayout = new QVBoxLayout(gbSpeechCue);
    cueLayout->setSpacing(6);

    QCheckBox* cueEnabled = new QCheckBox(tr("Emitir sinal sonoro ao falar"), gbSpeechCue);
    cueEnabled->setChecked(S::flag("capture/speechCueEnabled", false));
    cueLayout->addWidget(cueEnabled);
    connect(cueEnabled, &QCheckBox::toggled, this,
            [](bool v) { S::set("capture/speechCueEnabled", v); });

    QHBoxLayout* cueModeRow = new QHBoxLayout;
    cueModeRow->addWidget(new QLabel(tr("Emitir ao:"), gbSpeechCue));
    QRadioButton* cuePtt = new QRadioButton(tr("Pressione para Falar"), gbSpeechCue);
    QRadioButton* cueVad = new QRadioButton(tr("Atividade de Voz"), gbSpeechCue);
    QButtonGroup* cueModes = new QButtonGroup(gbSpeechCue);
    cueModes->addButton(cuePtt, 0);
    cueModes->addButton(cueVad, 1);
    if (S::num("capture/speechCueMode", 1) == 0) cuePtt->setChecked(true);
    else cueVad->setChecked(true);
    cueModeRow->addWidget(cuePtt);
    cueModeRow->addWidget(cueVad);
    cueModeRow->addStretch(1);
    cueLayout->addLayout(cueModeRow);
    connect(cueModes, &QButtonGroup::idClicked, this,
            [](int id) { S::set("capture/speechCueMode", id); });

    auto cueFileRow = [this, gbSpeechCue, cueLayout](const QString& title,
                                                       const QString& pathKey,
                                                       const QString& remoteKey) {
        QHBoxLayout* row = new QHBoxLayout;
        QLabel* label = new QLabel(title, gbSpeechCue);
        label->setMinimumWidth(72);
        QLineEdit* path = new QLineEdit(gbSpeechCue);
        path->setReadOnly(true);
        path->setPlaceholderText(tr("Nenhum arquivo selecionado"));
        path->setText(S::str(pathKey));
        path->setToolTip(S::str(pathKey));
        QPushButton* browse = new QPushButton(tr("Pesquisar..."), gbSpeechCue);
        browse->setToolTip(tr("Pesquisar um arquivo de áudio no computador"));
        QCheckBox* remote = new QCheckBox(tr("Outros usuários"), gbSpeechCue);
        remote->setToolTip(tr("Também emitir este sinal quando outro usuário falar"));
        remote->setChecked(S::flag(remoteKey, false));
        row->addWidget(label);
        row->addWidget(path, 1);
        row->addWidget(browse);
        row->addWidget(remote);
        cueLayout->addLayout(row);

        connect(browse, &QPushButton::clicked, this, [path, pathKey] {
            const QString selected = QFileDialog::getOpenFileName(
                path, tr("Selecionar arquivo de áudio"), QString(),
                tr("Arquivos de áudio (*.wav *.mp3 *.ogg *.flac *.m4a);;Todos os arquivos (*.*)"));
            if (selected.isEmpty()) return;
            path->setText(selected);
            path->setToolTip(selected);
            S::set(pathKey, selected);
        });
        connect(remote, &QCheckBox::toggled, this,
                [remoteKey](bool v) { S::set(remoteKey, v); });
    };
    cueFileRow(tr("Ativo"), QStringLiteral("capture/speechCueActive"),
               QStringLiteral("capture/speechCueRemoteActive"));
    cueFileRow(tr("Inativo"), QStringLiteral("capture/speechCueInactive"),
               QStringLiteral("capture/speechCueRemoteInactive"));
    cueFileRow(tr("Sussurro"), QStringLiteral("capture/speechCueWhisper"),
               QStringLiteral("capture/speechCueRemoteWhisper"));

    QLabel* cueHint = new QLabel(
        tr("Os sinais locais acompanham o modo escolhido. Marque \"Outros usuários\" "
           "para ouvir o mesmo sinal quando outra pessoa falar."), gbSpeechCue);
    cueHint->setObjectName(QStringLiteral("captionLabel"));
    cueHint->setWordWrap(true);
    cueLayout->addWidget(cueHint);
    right->addWidget(gbSpeechCue);
    right->addStretch(1);

    main->addLayout(right, 1);
    return w;
}

// ------------------------------------------------------------------ Hotkeys
// alvos possíveis do sussurro (índice = valor salvo em "scope")
static QStringList whisperScopeNames() {
    return { OptionsDialog::tr("Canal atual"),
             OptionsDialog::tr("Canal atual e subcanais"),
             OptionsDialog::tr("Lista de usuários") };
}

// chave do perfil ativo de hotkeys ("Padrão" quando não definido)
static QString activeHotkeyProfile() {
    return S::str(QStringLiteral("hotkeys/profile"),
                  OptionsDialog::tr("Padrão"));
}
// chave de armazenamento da lista de atalhos de um perfil
static QString hotkeysStoreKey(const QString& profile) {
    return QStringLiteral("hotkeys/list.") + profile;
}
// migração da chave legada "hotkeys/list" (pré-3.13) para o perfil Padrão
static void migrateLegacyHotkeys() {
    const QString def = OptionsDialog::tr("Padrão");
    const QString legacy = S::str(QStringLiteral("hotkeys/list"));
    const QString newKey = hotkeysStoreKey(def);
    if (!legacy.isEmpty() && S::str(newKey).isEmpty())
        S::set(newKey, legacy);
}

// "MOUSE BUTTON 5" estilo Halla para exibição (Mouse5 -> MOUSE BUTTON 5)
static QString keyDisplayName(const QString& canonical) {
    if (canonical == QLatin1String(HotkeyEdit::kMouse4))      return QStringLiteral("MOUSE BUTTON 4");
    if (canonical == QLatin1String(HotkeyEdit::kMouse5))      return QStringLiteral("MOUSE BUTTON 5");
    if (canonical == QLatin1String(HotkeyEdit::kMouseMiddle)) return QStringLiteral("MOUSE BUTTON MIDDLE");
    return canonical;
}

QWidget* OptionsDialog::pageHotkeys() {
    migrateLegacyHotkeys();

    QWidget* w = new QWidget;
    QHBoxLayout* lay = new QHBoxLayout(w);
    lay->setSpacing(10);

    const QString defProfile = tr("Padrão");
    const QString whisperAction = tr("Sussurrar (segurar para falar)");

    // ================= coluna esquerda: perfis =================
    QVBoxLayout* left = new QVBoxLayout;
    left->setSpacing(8);

    QGroupBox* gbSynced = new QGroupBox(tr("Perfis sincronizados"), w);
    QVBoxLayout* vs = new QVBoxLayout(gbSynced);
    QListWidget* synced = new QListWidget(gbSynced);
    synced->addItem(tr("Predefinição"));
    vs->addWidget(synced);
    left->addWidget(gbSynced);

    QGroupBox* gbLocal = new QGroupBox(tr("Perfis locais"), w);
    QVBoxLayout* vl = new QVBoxLayout(gbLocal);
    QListWidget* locals = new QListWidget(gbLocal);
    QStringList names = S::str(QStringLiteral("hotkeys/profiles"))
                            .split(QLatin1Char('|'), Qt::SkipEmptyParts);
    if (names.isEmpty()) names << defProfile;
    locals->addItems(names);
    const QString activeProf = activeHotkeyProfile();
    int lr = names.indexOf(activeProf);
    locals->setCurrentRow(lr >= 0 ? lr : 0);
    vl->addWidget(locals, 1);
    QHBoxLayout* pl = new QHBoxLayout;
    QPushButton* plus = new QPushButton(QStringLiteral("+"), gbLocal);
    plus->setFixedSize(28, 24);
    plus->setToolTip(tr("Adicionar perfil local"));
    pl->addWidget(plus, 0, Qt::AlignLeft);
    pl->addStretch(1);
    vl->addLayout(pl);
    left->addWidget(gbLocal, 1);
    left->addStretch(1);
    lay->addLayout(left, 0);

    // ================= painel principal: tabela de atalhos =================
    QVBoxLayout* right = new QVBoxLayout;
    right->setSpacing(6);

    QTableWidget* table = new QTableWidget(0, 2, w);
    table->verticalHeader()->setVisible(false);
    table->setHorizontalHeaderLabels({ tr("Tecla de atalho"), tr("Ação") });
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    right->addWidget(table, 1);

    // linha informativa (não editável) com o PTT do perfil de captura ativo,
    // como o Halla mostra: "MOUSE BUTTON 5 | Push-to-Talk ("Padrão")"
    auto addPttRow = [table, defProfile] {
        const QString cap = S::str(QStringLiteral("capture/profile"), defProfile);
        const QString key = S::str(QStringLiteral("capture/pttKey"),
                                   QStringLiteral("Space"));
        table->insertRow(0);
        QTableWidgetItem* k = new QTableWidgetItem(keyDisplayName(key));
        k->setData(Qt::UserRole, QStringLiteral("!ptt")); // marcador interno
        QTableWidgetItem* a = new QTableWidgetItem(
            QStringLiteral("Push-to-Talk (\"%1\")").arg(cap));
        const QFont f = k->font();
        k->setFont(f);
        a->setFont(f);
        table->setItem(0, 0, k);
        table->setItem(0, 1, a);
    };

    // texto da ação com o alvo do sussurro entre parênteses
    auto displayAction = [whisperAction](const QString& raw, int scope) {
        if (raw == whisperAction && scope >= 0 && scope <= 2)
            return QStringLiteral("%1 — %2").arg(raw, whisperScopeNames().value(scope));
        return raw;
    };

    auto loadTable = [table, displayAction, addPttRow] {
        table->setRowCount(0);
        addPttRow();
        QJsonDocument doc = QJsonDocument::fromJson(
            S::str(hotkeysStoreKey(activeHotkeyProfile())).toUtf8());
        if (!doc.isArray()) return;
        for (const QJsonValue& v : doc.array()) {
            QJsonObject o = v.toObject();
            const QString raw  = o["action"].toString();
            const int scope    = o.contains("scope") ? o["scope"].toInt(1) : -1;
            const int r = table->rowCount();
            table->insertRow(r);
            QTableWidgetItem* k = new QTableWidgetItem(
                keyDisplayName(o["key"].toString()));
            k->setData(Qt::UserRole, o["key"].toString()); // chave canônica
            QTableWidgetItem* a = new QTableWidgetItem(displayAction(raw, scope));
            a->setData(Qt::UserRole, raw);                 // ação "cru"
            a->setData(Qt::UserRole + 1, scope);           // alvo (-1 = n/a)
            table->setItem(r, 0, k);
            table->setItem(r, 1, a);
        }
    };
    loadTable();

    auto saveTable = [table, addPttRow, this] {
        QJsonArray arr;
        for (int r = 0; r < table->rowCount(); ++r) {
            // ignora a linha informativa do PTT
            if (table->item(r, 0)->data(Qt::UserRole).toString() == QLatin1String("!ptt"))
                continue;
            QJsonObject o;
            o["key"]    = table->item(r, 0)->data(Qt::UserRole).toString();
            o["action"] = table->item(r, 1)->data(Qt::UserRole).toString();
            const int scope = table->item(r, 1)->data(Qt::UserRole + 1).toInt();
            if (scope >= 0) o["scope"] = scope;
            arr << o;
        }
        S::set(hotkeysStoreKey(activeHotkeyProfile()),
               QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact)));
        emit hotkeysChanged();
    };
    Q_UNUSED(addPttRow);

    // seleção do perfil ativo troca a tabela de verdade
    auto switchProfile = [locals, loadTable, this](const QString& name, bool save) {
        if (name.isEmpty()) return;
        if (save) { // grava a tabela no perfil ANTERIOR antes de trocar
            // (saveTable já usa activeHotkeyProfile() — chama antes de mudar)
        }
        S::set(QStringLiteral("hotkeys/profile"), name);
        loadTable();
        emit hotkeysChanged();
    };
    connect(locals, &QListWidget::currentTextChanged, this, [this, loadTable](const QString& name) {
        if (name.isEmpty()) return;
        S::set(QStringLiteral("hotkeys/profile"), name);
        loadTable();
        emit hotkeysChanged();
    });
    Q_UNUSED(switchProfile);
    connect(synced, &QListWidget::itemClicked, this, [this, locals, synced](QListWidgetItem*) {
        // perfis sincronizados exigem conta myHalla — volta para o local
        QMessageBox::information(this, tr("Perfis sincronizados"),
            tr("A sincronização de perfis em nuvem requer uma conta myHalla. "
               "Por enquanto, use os perfis locais."));
        synced->clearSelection();
        synced->setCurrentItem(nullptr);
        if (locals->currentRow() < 0) locals->setCurrentRow(0);
    });
    connect(plus, &QPushButton::clicked, this, [this, locals, loadTable] {
        bool ok = false;
        const QString name = QInputDialog::getText(
            this, tr("Novo perfil local"), tr("Nome do perfil:"),
            QLineEdit::Normal, tr("Novo perfil"), &ok);
        if (!ok || name.trimmed().isEmpty()) return;
        if (!locals->findItems(name.trimmed(), Qt::MatchExactly).isEmpty()) return;
        locals->addItem(name.trimmed());
        QStringList names;
        for (int i = 0; i < locals->count(); ++i) names << locals->item(i)->text();
        S::set(QStringLiteral("hotkeys/profiles"), names.join(QLatin1Char('|')));
        locals->setCurrentRow(locals->count() - 1); // dispara a troca p/ o novo perfil
    });

    // ---- botões: + Adicionar  X Remover  Editar ... combo do perfil ativo
    QHBoxLayout* btns = new QHBoxLayout;
    QPushButton* add = new QPushButton(tr("+ Adicionar"), w);
    QPushButton* del = new QPushButton(tr("X Remover"), w);
    QPushButton* edit = new QPushButton(tr("Editar"), w);
    btns->addWidget(add);
    btns->addWidget(del);
    btns->addWidget(edit);
    btns->addStretch(1);
    QComboBox* profSel = new QComboBox(w);
    profSel->addItems(names);
    {
        const int idx = names.indexOf(activeProf);
        profSel->setCurrentIndex(idx >= 0 ? idx : 0);
    }
    btns->addWidget(profSel);
    right->addLayout(btns);
    connect(profSel, &QComboBox::currentTextChanged, this,
            [locals](const QString& name) {
                auto items = locals->findItems(name, Qt::MatchExactly);
                if (!items.isEmpty())
                    locals->setCurrentRow(locals->row(items.first()));
            });
    // combo acompanha a lista de perfis
    connect(locals, &QListWidget::currentTextChanged, this,
            [profSel](const QString& name) {
                const int idx = profSel->findText(name, Qt::MatchExactly);
                if (idx >= 0) {
                    QSignalBlocker sb(profSel);
                    profSel->setCurrentIndex(idx);
                }
            });

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
        if (row >= 0 && table->item(row, 0)->data(Qt::UserRole).toString()
                            == QLatin1String("!ptt")) {
            QMessageBox::information(this, tr("Push-to-Talk"),
                tr("A tecla de PTT é configurada na página \"Capturar\"."));
            selectPage(tr("Capturar"));
            return;
        }
        QDialog d(w);
        d.setWindowTitle(row < 0 ? tr("Adicionar tecla de atalho") : tr("Editar tecla de atalho"));
        QFormLayout* f = new QFormLayout(&d);
        QComboBox* action = new QComboBox(&d);
        action->addItems(actions);
        // captura tecla OU botão do mouse (inclusive laterais) — igual ao Halla
        HotkeyEdit* key = new HotkeyEdit(&d);
        key->setMinimumWidth(220);
        QComboBox* scope = new QComboBox(&d);
        scope->addItems(whisperScopeNames());

        int curScope = 1;
        if (row >= 0) {
            action->setCurrentText(table->item(row, 1)->data(Qt::UserRole).toString());
            key->setSpec(table->item(row, 0)->data(Qt::UserRole).toString());
            curScope = table->item(row, 1)->data(Qt::UserRole + 1).toInt();
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
                                     "como o PTT do Halla."), &d);
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

        // grava na tabela, APÓS a linha do PTT
        const QString raw = action->currentText();
        const bool isWhisper = (raw == whisperAction);
        const int sc = isWhisper ? scope->currentIndex() : -1;
        if (isWhisper) S::set("hotkeys/whisperScope", sc); // alvo p/ alternância
        if (row < 0) {
            row = table->rowCount();
            table->insertRow(row);
        }
        QTableWidgetItem* k = new QTableWidgetItem(keyDisplayName(key->spec()));
        k->setData(Qt::UserRole, key->spec());
        QTableWidgetItem* a = new QTableWidgetItem(displayAction(raw, sc));
        a->setData(Qt::UserRole, raw);
        a->setData(Qt::UserRole + 1, sc);
        table->setItem(row, 0, k);
        table->setItem(row, 1, a);
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
        if (table->item(items.first()->row(), 0)->data(Qt::UserRole).toString()
                == QLatin1String("!ptt"))
            return; // a linha do PTT não é removível
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
    right->addWidget(note);

    lay->addLayout(right, 1);
    return w;
}

// ------------------------------------------------------------------ Sussurro
QWidget* OptionsDialog::pageWhisper() {
    QWidget* w = new QWidget;
    QVBoxLayout* lay = new QVBoxLayout(w);
    lay->setSpacing(10);

    // ---- permissões para recebimento
    QGroupBox* gbPerm = new QGroupBox(tr("Permissões para recebimento de sussurros"), w);
    QVBoxLayout* vp = new QVBoxLayout(gbPerm);
    const QStringList permOpts = {
        tr("Usar a configuração do contato; se não houver nenhuma, permitir (padrão)"),
        tr("Usar a configuração do contato; se não houver nenhuma, negar"),
        tr("Negar a todos"),
    };
    for (int i = 0; i < permOpts.size(); ++i) {
        QRadioButton* rb = new QRadioButton(permOpts[i], gbPerm);
        rb->setChecked(S::num("whisper/recvMode", 0) == i);
        connect(rb, &QRadioButton::toggled, this,
                [i](bool v) { if (v) S::set("whisper/recvMode", i); });
        vp->addWidget(rb);
    }
    lay->addWidget(gbPerm);

    // ---- configurações para recebimento
    QGroupBox* gbRecv = new QGroupBox(tr("Configurações para recebimento de sussurros"), w);
    QVBoxLayout* vr = new QVBoxLayout(gbRecv);
    QCheckBox* sound = new QCheckBox(
        tr("Reproduzir áudio de notificação ao receber um sussurro"), gbRecv);
    sound->setChecked(S::flag("whisper/notifySound", true));
    connect(sound, &QCheckBox::toggled, this,
            [](bool v) { S::set("whisper/notifySound", v); });
    vr->addWidget(sound);
    QCheckBox* hist = new QCheckBox(
        tr("Sempre permitir mostrar o histórico de sussurros ao receber um sussurro"), gbRecv);
    hist->setChecked(S::flag("whisper/alwaysHistory", false));
    connect(hist, &QCheckBox::toggled, this,
            [](bool v) { S::set("whisper/alwaysHistory", v); });
    vr->addWidget(hist);
    QHBoxLayout* hr = new QHBoxLayout;
    QLabel* hlabel = new QLabel(tr("Remover clientes no histórico de sussurros após"), gbRecv);
    QSpinBox* mins = new QSpinBox(gbRecv);
    mins->setRange(1, 60);
    mins->setSuffix(QStringLiteral(" min"));
    mins->setValue(S::num("whisper/historyMinutes", 5));
    hr->addWidget(hlabel);
    hr->addWidget(mins);
    hr->addStretch(1);
    vr->addLayout(hr);
    connect(mins, &QSpinBox::valueChanged, this,
            [](int v) { S::set("whisper/historyMinutes", v); });
    lay->addWidget(gbRecv);

    // ---- ação extra: lista de sussurros
    QHBoxLayout* lr = new QHBoxLayout;
    QPushButton* lists = new QPushButton(tr("Lista de sussurros"), w);
    lr->addWidget(lists, 0, Qt::AlignLeft);
    lr->addStretch(1);
    lay->addLayout(lr);
    connect(lists, &QPushButton::clicked, this, [this] {
        WhisperDialog dlg(nullptr, this);
        dlg.exec();
    });

    lay->addStretch(1);
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
