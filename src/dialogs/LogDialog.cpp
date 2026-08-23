#include "LogDialog.h"
#include "TsBanner.h"
#include "Icons.h"
#include "AppLog.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QLabel>
#include <QCheckBox>
#include <QFileDialog>
#include <QFile>
#include <QTextStream>
#include <QHeaderView>
#include <QStandardPaths>
#include <QDir>

LogDialog::LogDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("Registro do cliente"));
    resize(720, 420);

    QVBoxLayout* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 8);
    root->setSpacing(0);
    root->addWidget(new TsBanner(tr("Registro do cliente"),
                                 tr("Eventos do cliente Halla (salvos em halla.log)"),
                                 HIcons::logPage().pixmap(24, 24), this));
    root->addSpacing(8);

    // barra de ferramentas
    QHBoxLayout* bar = new QHBoxLayout;
    bar->setContentsMargins(10, 0, 10, 4);
    bar->addWidget(new QLabel(tr("Nível de log:"), this));
    m_filter = new QComboBox(this);
    m_filter->addItem(tr("Tudo"), -1);
    m_filter->addItem(tr("Info"), 0);
    m_filter->addItem(tr("Aviso"), 1);
    m_filter->addItem(tr("Erro"), 2);
    m_filter->addItem(tr("Depuração"), 3);
    bar->addWidget(m_filter);
    bar->addSpacing(12);
    m_autoscroll = new QCheckBox(tr("Rolagem automática"), this);
    m_autoscroll->setChecked(true);
    bar->addWidget(m_autoscroll);
    bar->addStretch(1);
    QPushButton* clear = new QPushButton(tr("Limpar"), this);
    QPushButton* save = new QPushButton(tr("Salvar como..."), this);
    bar->addWidget(clear);
    bar->addWidget(save);
    root->addLayout(bar);

    m_table = new QTableWidget(0, 3, this);
    m_table->setHorizontalHeaderLabels({ tr("Data/Hora"), tr("Nível"), tr("Mensagem") });
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->verticalHeader()->setVisible(false);
    m_table->setShowGrid(false);
    m_table->setAlternatingRowColors(true);
    root->addWidget(m_table, 1);

    QHBoxLayout* bottom = new QHBoxLayout;
    bottom->setContentsMargins(10, 6, 10, 0);
    bottom->addStretch(1);
    QPushButton* close = new QPushButton(tr("Fechar"), this);
    bottom->addWidget(close);
    root->addLayout(bottom);

    connect(close, &QPushButton::clicked, this, &QDialog::hide);
    connect(clear, &QPushButton::clicked, this, [this] {
        m_entries.clear();
        rebuild();
    });
    connect(save, &QPushButton::clicked, this, [this] {
        const QString fn = QFileDialog::getSaveFileName(
            this, tr("Salvar registro"), QStringLiteral("halla-log.txt"),
            tr("Arquivos de texto (*.txt)"));
        if (fn.isEmpty()) return;
        QFile f(fn);
        if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream out(&f);
            for (const Entry& e : m_entries)
                out << "[" << e.ts << "] [" << AppLog::levelName(AppLog::Level(e.level))
                    << "] " << e.text << "\n";
        }
    });
    connect(m_filter, &QComboBox::currentIndexChanged, this,
            [this](int) { rebuild(); });

    connect(&AppLog::instance(), &AppLog::message, this, &LogDialog::append);

    // Carrega o histórico do arquivo para que mensagens anteriores à abertura
    // (ex.: a escolha de encoder GPU/CPU no começo de uma transmissão 4K)
    // já apareçam na janela.
    loadFromFile();
}

AppLog::Level LogDialog::levelFromName(const QString& name) {
    const QString n = name.trimmed().toUpper();
    if (n == QLatin1String("ERRO") || n == QLatin1String("ERROR")) return AppLog::Error;
    if (n == QLatin1String("AVISO") || n == QLatin1String("WARN")
            || n == QLatin1String("WARNING")) return AppLog::Warning;
    if (n == QLatin1String("DEPURAÇÃO") || n == QLatin1String("DEBUG")
            || n == QLatin1String("DEPURACAO")) return AppLog::Debug;
    return AppLog::Info;
}

void LogDialog::loadFromFile() {
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QFile f(dir + QStringLiteral("/halla.log"));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return;

    // Mantém no máximo as últimas ~2000 linhas para não travar a janela.
    QStringList lines;
    QTextStream in(&f);
    QString line;
    while (in.readLineInto(&line))
        lines << line;
    if (lines.size() > 2000) {
        lines = lines.mid(lines.size() - 2000);
    }

    for (const QString& raw : lines) {
        // Formato: [dd/MM/yyyy HH:mm:ss] [NIVEL] mensagem
        const QString l = raw.trimmed();
        if (l.isEmpty()) continue;
        QString ts, levelText, text;
        int close = l.indexOf(QLatin1Char(']'));
        if (l.startsWith(QLatin1Char('[')) && close > 0) {
            ts = l.mid(1, close - 1);
            int nextOpen = l.indexOf(QLatin1Char('['), close);
            int nextClose = l.indexOf(QLatin1Char(']'), close + 1);
            if (nextOpen >= 0 && nextClose > nextOpen) {
                levelText = l.mid(nextOpen + 1, nextClose - nextOpen - 1);
                text = l.mid(nextClose + 1).trimmed();
            } else {
                text = l.mid(close + 1).trimmed();
            }
        } else {
            text = l;
        }
        append(int(levelFromName(levelText)), ts, text);
    }
}

void LogDialog::append(int level, const QString& timestamp, const QString& text) {
    m_entries << Entry{ level, timestamp, text };
    const int filter = m_filter->currentData().toInt();
    if (filter >= 0 && filter != level) return;

    const int r = m_table->rowCount();
    m_table->insertRow(r);
    QTableWidgetItem* ts = new QTableWidgetItem(timestamp);
    QTableWidgetItem* lvl = new QTableWidgetItem(AppLog::levelName(AppLog::Level(level)));
    QTableWidgetItem* msg = new QTableWidgetItem(text);
    QColor color;
    switch (level) {
        case AppLog::Warning: color = QColor("#8F6600"); break;
        case AppLog::Error:   color = QColor("#B03A36"); break;
        case AppLog::Debug:   color = QColor("#3B76B0"); break;
        default:              color = QColor("#408040"); break;
    }
    lvl->setForeground(color);
    m_table->setItem(r, 0, ts);
    m_table->setItem(r, 1, lvl);
    m_table->setItem(r, 2, msg);
    if (m_autoscroll->isChecked()) m_table->scrollToBottom();
}

void LogDialog::rebuild() {
    m_table->setRowCount(0);
    const int filter = m_filter->currentData().toInt();
    QList<Entry> keep = m_entries;
    m_entries.clear();
    for (const Entry& e : keep) {
        m_entries << e;
        if (filter >= 0 && e.level != filter) continue;
        m_table->insertRow(m_table->rowCount());
        QTableWidgetItem* ts = new QTableWidgetItem(e.ts);
        QTableWidgetItem* lvl = new QTableWidgetItem(
            AppLog::levelName(AppLog::Level(e.level)));
        QTableWidgetItem* msg = new QTableWidgetItem(e.text);
        const int r = m_table->rowCount() - 1;
        m_table->setItem(r, 0, ts);
        m_table->setItem(r, 1, lvl);
        m_table->setItem(r, 2, msg);
    }
    if (m_autoscroll->isChecked()) m_table->scrollToBottom();
}
