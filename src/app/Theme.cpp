#include "Theme.h"
#include "Settings.h"

#include <QApplication>
#include <QPalette>
#include <QStyle>
#include <QStyleFactory>

bool HTheme::isDark() {
    return S::num(QStringLiteral("design/theme"), 0) == 1;
}

QString HTheme::styleSheet(bool dark) {
    // cores ajustadas por tema
    const QString border      = dark ? QStringLiteral("#3E434A") : QStringLiteral("#C8C8C8");
    const QString hover       = dark ? QStringLiteral("#3A4048") : QStringLiteral("#E4EEF8");
    const QString sel         = dark ? QStringLiteral("#3B76B0") : QStringLiteral("#0078D7");
    const QString welcomeBg   = dark ? QStringLiteral("#26292E") : QStringLiteral("#E8EAED");
    const QString welcomeMain = dark ? QStringLiteral("#AFBAC5") : QStringLiteral("#5A6B7A");
    const QString welcomeSub  = dark ? QStringLiteral("#8B959E") : QStringLiteral("#8A939B");

    QString css = QStringLiteral(
        // árvore de canais (e demais árvores/listas) — cantos sempre retos
        "QTreeWidget { background: palette(base); color: palette(text);"
        "  alternate-background-color: palette(alternate-base);"
        "  border: 1px solid %1; border-radius: 0px; }"
        "QTreeWidget::item { height: 21px; padding: 0px; }"
        "QTreeWidget::item:selected { background: %2; color: #FFFFFF; }"
        "QTreeWidget::item:hover:!selected { background: %3; }"
        "QTreeWidget::branch { background: transparent; }"

        // painéis de texto (chat + informações)
        "QTextBrowser { background: palette(base); color: palette(text);"
        "  border: 1px solid %1; border-radius: 0px; font-size: 13px; }"
        "QTextBrowser#infoView { border-top: none; }"

        // listas/tabelas seguem o mesmo esquema
        "QListWidget, QTableWidget { background: palette(base); color: palette(text);"
        "  alternate-background-color: palette(alternate-base);"
        "  border: 1px solid %1; border-radius: 0px; }"

        // campos de entrada: linhas retas e finas, sem arredondamento
        "QLineEdit, QComboBox, QSpinBox, QDoubleSpinBox, QTextEdit, QPlainTextEdit {"
        "  border: 1px solid %1; border-radius: 0px; background: palette(base);"
        "  color: palette(text); selection-background-color: %2; }"
        "QComboBox QAbstractItemView { border: 1px solid %1; border-radius: 0px;"
        "  background: palette(base); color: palette(text);"
        "  selection-background-color: %2; selection-color: #FFFFFF; }"

        // divisores discretos
        "QSplitter::handle { background: palette(window); }"
        "QSplitter::handle:horizontal { width: 3px; }"
        "QSplitter::handle:vertical { height: 3px; }"

        // menus suspensos com cantos retos
        "QMenu { border: 1px solid %1; border-radius: 0px; }"

        // tela de boas-vindas
        "QWidget#welcomePage { background: %4; }"
        "QLabel#welcomeTitle { color: %5; }"
        "QLabel#welcomeHint { color: %6; }"

        // dicas de ferramenta legíveis nos dois temas
        "QToolTip { background: palette(tool-tip-base); color: palette(tool-tip-text);"
        "  border: 1px solid %1; border-radius: 0px; padding: 2px; }"
    ).arg(border, sel, hover, welcomeBg, welcomeMain, welcomeSub);

    if (!dark) {
        // ---------------------------------------------------------------
        // EXTRAS DO TEMA CLARO — visual clássico do Halla / Windows:
        // cromo #F0F0F0, conteúdo branco, bordas 1px e cantos retos
        // ---------------------------------------------------------------
        css += QStringLiteral(
            "QMenuBar { background: #F0F0F0; border-bottom: 1px solid #D5D5D5; }"
            "QMenuBar::item { background: transparent; padding: 4px 9px;"
            "  border-radius: 0px; }"
            "QMenuBar::item:selected { background: #CCE4F7; color: #000000; }"
            "QMenuBar::item:pressed { background: #99D1FF; color: #000000; }"

            // ---- barra de ferramentas: botões altos, com folga, e setas
            // suspensas SEMPRE ao lado direito do ícone/texto (nunca sobre)
            "QToolBar { background: #F0F0F0; border: 0px; border-radius: 0px;"
            "  spacing: 3px; padding: 3px 6px; }"
            "QToolBar::separator { background: #D5D5D5; width: 1px;"
            "  margin: 4px 5px; }"
            "QToolButton { border: 1px solid transparent; border-radius: 0px;"
            "  background: transparent; padding: 4px 7px; margin: 0px 2px; }"
            "QToolButton:hover { background: #E5F1FB; border: 1px solid #B8D7F0; }"
            "QToolButton:pressed { background: #CCE4F7; border: 1px solid #99C3EA; }"
            "QToolButton:checked { background: #CCE4F7; border: 1px solid #99C3EA; }"
            // reserva a faixa da seta à DIREITA do conteúdo (MenuButtonPopup)
            "QToolButton[popupMode=\"1\"] { padding-right: 18px; }"
            "QToolButton::menu-button { subcontrol-origin: padding;"
            "  subcontrol-position: center right; width: 13px;"
            "  border-left: 1px solid #DCDCDC; }"
            "QToolButton::menu-button:hover { background: #CBE3F5; }"
            "QToolButton::menu-button:pressed { background: #B8D7F0; }"

            "QStatusBar { background: #F0F0F0; border-top: 1px solid #D0D0D0; }"
            "QStatusBar::item { border: 0px; }"
            "QLabel#newsLabel { color: #777777; }"
            // "aba" do servidor no canto inferior esquerdo
            "QToolButton#serverTabButton { background: #FFFFFF; color: #111111;"
            "  border: 1px solid #C3C3C3; border-radius: 0px; padding: 2px 10px; }"
            "QToolButton#serverTabButton:hover { background: #E5F1FB; }"
            "QToolButton#serverTabButton::menu-indicator { image: none; width: 0px; }"

            // abas do servidor no topo (conexões), estilo clássico do Halla
            "QTabWidget::pane { border: 1px solid #D0D0D0; top: -1px;"
            "  background: #FFFFFF; border-radius: 0px; }"
            "QTabBar::tab { background: #E7E7E7; color: #111111;"
            "  border: 1px solid #C0C0C0; border-bottom: 0px; border-radius: 0px;"
            "  padding: 3px 10px; margin-right: 2px; }"
            "QTabBar::tab:selected { background: #FFFFFF; color: #000000; }"
            "QTabBar::tab:hover:!selected { background: #F0F6FC; }"

            // cabeçalhos de tabela e fieldsets clássicos (título cortando
            // a linha de 1px, interior transparente — sem caixas cinzas)
            "QHeaderView::section { background: #F0F0F0; color: #111111;"
            "  border: 0px; border-right: 1px solid #D6D6D6;"
            "  border-bottom: 1px solid #D0D0D0; border-radius: 0px;"
            "  padding: 3px 6px; }"
            "QGroupBox { border: 1px solid #D8D8D8; border-radius: 0px;"
            "  background: transparent; margin-top: 13px; padding-top: 8px; }"
            "QGroupBox::title { subcontrol-origin: margin;"
            "  subcontrol-position: top left; left: 8px; top: 1px;"
            "  padding: 0px 4px; background: palette(window); }"

            // ---- janela de OPÇÕES (estilo Halla) --------------------------
            "QListWidget#optionsNav { background: #FFFFFF; border: 0px;"
            "  outline: 0; padding: 5px 4px; }"
            "QListWidget#optionsNav::item { height: 34px; padding-left: 10px;"
            "  border: 1px solid transparent; border-radius: 0px; }"
            "QListWidget#optionsNav::item:hover:!selected { background: #EEF5FB; }"
            "QListWidget#optionsNav::item:selected { background: #DCE9F8;"
            "  color: #000000; border: 1px solid #A9CCEA; }"
            "QWidget#optionsSep { background: #DFDFDF; }"
            "QWidget#pageHeader { background: qlineargradient(x1:0, y1:0,"
            "  x2:0, y2:1, stop:0 #F1F1F1, stop:1 #FFFFFF);"
            "  border-bottom: 1px solid #E2E2E2; }"
            "QLabel#pageTitle { background: transparent; color: #1A1A1A;"
            "  font-size: 15px; font-weight: bold; }"
            "QLabel#pageSubtitle { background: transparent; color: #666666; }"
            "QWidget#optionsStack { background: #FFFFFF; }"
            "QScrollArea#optionsScroll { background: #FFFFFF; border: 0px; }"
            "QScrollArea#optionsScroll QWidget#qt_scrollarea_viewport {"
            "  background: #FFFFFF; }"
            "QWidget#optionsPage { background: #FFFFFF; }"
            "QWidget#optionsPage QGroupBox { border: 1px solid #DADADA;"
            "  background: transparent; }"
            "QWidget#optionsPage QGroupBox::title { background: #FFFFFF; }"

            "QLabel#captionLabel { color: #666666; }"
        );
    } else {
        css += QStringLiteral(
            "QToolBar { border: 0px; spacing: 3px; padding: 3px 6px; }"
            "QToolBar::separator { background: #3E434A; width: 1px; margin: 4px 5px; }"
            "QToolButton { border: 1px solid transparent; border-radius: 0px;"
            "  background: transparent; padding: 4px 7px; margin: 0px 2px; }"
            "QToolButton:hover { background: #3A4048; border: 1px solid #4A515A; }"
            "QToolButton:checked { background: #3B6E9E; border: 1px solid #5EA3E0; }"
            "QToolButton[popupMode=\"1\"] { padding-right: 18px; }"
            "QToolButton::menu-button { subcontrol-origin: padding;"
            "  subcontrol-position: center right; width: 13px;"
            "  border-left: 1px solid #3E434A; }"
            "QLabel#captionLabel { color: #9AA3AC; }"
            "QLabel#newsLabel { color: #7A828B; }"
            "QToolButton#serverTabButton { border: 1px solid #3E434A;"
            "  border-radius: 0px; padding: 2px 10px; }"
            "QGroupBox { border: 1px solid #3E434A; border-radius: 0px;"
            "  background: transparent; margin-top: 13px; padding-top: 8px; }"
            "QGroupBox::title { subcontrol-origin: margin;"
            "  subcontrol-position: top left; left: 8px; top: 1px;"
            "  padding: 0px 4px; background: palette(window); }"
            "QListWidget#optionsNav::item { height: 34px; padding-left: 10px; }"
            "QWidget#pageHeader { border-bottom: 1px solid #3E434A; }"
            "QLabel#pageTitle { font-size: 15px; font-weight: bold; }"
            "QLabel#pageSubtitle { color: #9AA3AC; }"
        );
    }
    return css;
}

void HTheme::apply() {
    const bool dark = isDark();

    // no Windows o tema claro usa o estilo nativo clássico (windowsvista):
    // é exatamente o "chrome" do Halla — cinza #F0F0F0, relevos Win32
    QStyle* style = nullptr;
#ifdef Q_OS_WIN
    if (!dark)
        style = QStyleFactory::create(QStringLiteral("windowsvista"));
#endif
    if (!style)
        style = QStyleFactory::create(QStringLiteral("fusion"));
    QApplication::setStyle(style);
    QPalette pal;

    if (dark) {
        const QColor window  ("#2B2E33");
        const QColor base    ("#24272C");
        const QColor text    ("#DCDFE3");
        const QColor disabled("#7A828B");
        const QColor button  ("#33373D");

        pal.setColor(QPalette::Window, window);
        pal.setColor(QPalette::WindowText, text);
        pal.setColor(QPalette::Base, base);
        pal.setColor(QPalette::AlternateBase, window);
        pal.setColor(QPalette::ToolTipBase, QColor("#1D1F23"));
        pal.setColor(QPalette::ToolTipText, text);
        pal.setColor(QPalette::Text, text);
        pal.setColor(QPalette::Button, button);
        pal.setColor(QPalette::ButtonText, text);
        pal.setColor(QPalette::BrightText, Qt::red);
        pal.setColor(QPalette::Link, QColor("#5EA3E0"));
        pal.setColor(QPalette::Highlight, QColor("#3B6E9E"));
        pal.setColor(QPalette::HighlightedText, Qt::white);
        pal.setColor(QPalette::PlaceholderText, disabled);
        // papéis derivados usados pelo Fusion (bordas, relevos, 3D)
        pal.setColor(QPalette::Light,    QColor("#4A4F56"));
        pal.setColor(QPalette::Midlight, QColor("#3A3E44"));
        pal.setColor(QPalette::Mid,      QColor("#454A51"));
        pal.setColor(QPalette::Dark,     QColor("#1E2125"));
        pal.setColor(QPalette::Shadow,   QColor("#141619"));
        // estado desabilitado coerente
        pal.setColor(QPalette::Disabled, QPalette::Text, disabled);
        pal.setColor(QPalette::Disabled, QPalette::WindowText, disabled);
        pal.setColor(QPalette::Disabled, QPalette::ButtonText, disabled);
        pal.setColor(QPalette::Disabled, QPalette::Base, QColor("#22252A"));
        pal.setColor(QPalette::Disabled, QPalette::Window, QColor("#282B30"));
    } else {
        // ---- tema claro clássico (Halla / Windows nativo)
        pal = QApplication::style()->standardPalette();
        pal.setColor(QPalette::Window, QColor("#F0F0F0"));
        pal.setColor(QPalette::Base, QColor("#FFFFFF"));
        pal.setColor(QPalette::AlternateBase, QColor("#F5F5F5"));
        pal.setColor(QPalette::Text, QColor("#111111"));
        pal.setColor(QPalette::WindowText, QColor("#111111"));
        pal.setColor(QPalette::Button, QColor("#F0F0F0"));
        pal.setColor(QPalette::ButtonText, QColor("#111111"));
        pal.setColor(QPalette::ToolTipBase, QColor("#FFFFE1"));
        pal.setColor(QPalette::ToolTipText, QColor("#000000"));
        pal.setColor(QPalette::Link, QColor("#2E7FC4"));
        pal.setColor(QPalette::Highlight, QColor("#0078D7"));
        pal.setColor(QPalette::HighlightedText, Qt::white);
        pal.setColor(QPalette::PlaceholderText, QColor("#808080"));
        pal.setColor(QPalette::Disabled, QPalette::Text, QColor("#6D6D6D"));
        pal.setColor(QPalette::Disabled, QPalette::WindowText, QColor("#6D6D6D"));
        pal.setColor(QPalette::Disabled, QPalette::ButtonText, QColor("#6D6D6D"));
    }

    QApplication::setPalette(pal);
    qApp->setStyleSheet(styleSheet(dark));
}
