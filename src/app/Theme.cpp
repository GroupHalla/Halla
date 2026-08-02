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
    // cores ajustadas por tema (o claro preserva exatamente o visual atual)
    const QString border      = dark ? QStringLiteral("#3E434A") : QStringLiteral("#C9CDD2");
    const QString hover       = dark ? QStringLiteral("#3A4048") : QStringLiteral("#E4EEF8");
    const QString sel         = QStringLiteral("#3B76B0");
    const QString welcomeBg   = dark ? QStringLiteral("#26292E") : QStringLiteral("#E8EAED");
    const QString welcomeMain = dark ? QStringLiteral("#AFBAC5") : QStringLiteral("#5A6B7A");
    const QString welcomeSub  = dark ? QStringLiteral("#8B959E") : QStringLiteral("#8A939B");

    return QStringLiteral(
        // árvore de canais (e demais árvores/listas)
        "QTreeWidget { background: palette(base); color: palette(text);"
        "  alternate-background-color: palette(alternate-base);"
        "  border: 1px solid %1; }"
        "QTreeWidget::item { height: 21px; padding: 0px; }"
        "QTreeWidget::item:selected { background: %2; color: #FFFFFF; }"
        "QTreeWidget::item:hover:!selected { background: %3; }"
        "QTreeWidget::branch { background: transparent; }"

        // painéis de texto (chat + informações)
        "QTextBrowser { background: palette(base); color: palette(text);"
        "  border: 1px solid %1; font-size: 13px; }"
        "QTextBrowser#infoView { border-top: none; }"

        // listas/tabelas seguem o mesmo esquema
        "QListWidget, QTableWidget { background: palette(base); color: palette(text);"
        "  alternate-background-color: palette(alternate-base);"
        "  border: 1px solid %1; }"

        // tela de boas-vindas
        "QWidget#welcomePage { background: %4; }"
        "QLabel#welcomeTitle { color: %5; }"
        "QLabel#welcomeHint { color: %6; }"

        // dicas de ferramenta legíveis nos dois temas
        "QToolTip { background: palette(tool-tip-base); color: palette(tool-tip-text);"
        "  border: 1px solid %1; padding: 2px; }"
    ).arg(border, sel, hover, welcomeBg, welcomeMain, welcomeSub);
}

void HTheme::apply() {
    const bool dark = isDark();
    QApplication::setStyle(QStyleFactory::create(QStringLiteral("fusion")));
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
        pal = QApplication::style()->standardPalette();
        pal.setColor(QPalette::Link, QColor("#2E7FC4"));
        pal.setColor(QPalette::Highlight, QColor("#3B76B0"));
        pal.setColor(QPalette::HighlightedText, Qt::white);
    }

    QApplication::setPalette(pal);
    qApp->setStyleSheet(styleSheet(dark));
}
