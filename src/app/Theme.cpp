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
    const QString window = dark ? QStringLiteral("#080D1C") : QStringLiteral("#FAFAFC");
    const QString surface = dark ? QStringLiteral("#0D1223") : QStringLiteral("#FFFFFF");
    const QString surfaceAlt = dark ? QStringLiteral("#11182B") : QStringLiteral("#FDFDFF");
    const QString input = dark ? QStringLiteral("#10172A") : QStringLiteral("#FFFFFF");
    const QString border = dark ? QStringLiteral("#202A45") : QStringLiteral("#E9E8F0");
    const QString borderStrong = dark ? QStringLiteral("#2A3555") : QStringLiteral("#DDDCE7");
    const QString text = dark ? QStringLiteral("#F5F4FF") : QStringLiteral("#242434");
    const QString muted = dark ? QStringLiteral("#A8AABD") : QStringLiteral("#747487");
    const QString accent = QStringLiteral("#7C3AED");
    const QString accentBright = QStringLiteral("#8B5CF6");
    const QString accentSoft = dark ? QStringLiteral("#24165B") : QStringLiteral("#F0E9FF");
    const QString hover = dark ? QStringLiteral("#17203A") : QStringLiteral("#F6F2FF");
    const QString selected = dark ? QStringLiteral("#281B68") : QStringLiteral("#F0E8FF");
    const QString green = QStringLiteral("#22C55E");

    QString css;
    css += QStringLiteral("QMainWindow, QWidget#mainSurface, QStackedWidget, QSplitter, QWidget { ")
           + QStringLiteral("background: ") + window + QStringLiteral("; color: ") + text + QStringLiteral("; }");
    css += QStringLiteral("QMainWindow { border: 0; }");
    css += QStringLiteral("QLabel { background: transparent; }");

    // Top navigation and the compact control rail from the reference design.
    css += QStringLiteral("QMenuBar { background: ") + window + QStringLiteral("; color: ") + text
           + QStringLiteral("; border: 0; border-bottom: 1px solid ") + border
           + QStringLiteral("; padding: 0 16px; min-height: 40px; font-size: 14px; }");
    css += QStringLiteral("QMenuBar::item { background: transparent; padding: 11px 14px 9px 14px; margin: 0 1px; }");
    css += QStringLiteral("QMenuBar::item:selected { color: ") + accentBright
           + QStringLiteral("; background: transparent; border-bottom: 2px solid ") + accentBright + QStringLiteral("; }");
    css += QStringLiteral("QMenuBar::item:pressed { color: ") + accent + QStringLiteral("; }");

    css += QStringLiteral("QToolBar#mainToolBar { background: ") + window
           + QStringLiteral("; border: 0; padding: 8px 16px; spacing: 12px; min-height: 54px; }");
    css += QStringLiteral("QFrame#toolbarGroup { background: ") + surface
           + QStringLiteral("; border: 1px solid ") + border + QStringLiteral("; border-radius: 15px; }");
    css += QStringLiteral("QToolBar#mainToolBar QToolButton { background: transparent; color: ") + text
           + QStringLiteral("; border: 0; border-radius: 11px; min-width: 38px; min-height: 36px; padding: 4px 8px; }");
    css += QStringLiteral("QToolBar#mainToolBar QToolButton:hover { background: ") + hover + QStringLiteral("; }");
    css += QStringLiteral("QToolBar#mainToolBar QToolButton:checked { background: ") + accentSoft
           + QStringLiteral("; color: ") + accentBright + QStringLiteral("; }");
    css += QStringLiteral("QToolBar#mainToolBar QToolButton::menu-button { width: 15px; border: 0; }");
    css += QStringLiteral("QToolBar#mainToolBar::separator { width: 1px; background: ") + border + QStringLiteral("; margin: 5px 1px; }");

    // Main split view: two floating cards and one full-width chat card.
    css += QStringLiteral("QFrame#panelCard, QFrame#chatCard { background: ") + surface
           + QStringLiteral("; border: 1px solid ") + border + QStringLiteral("; border-radius: 14px; }");
    css += QStringLiteral("QFrame#treeCardHeader { background: transparent; border: 0; }");
    css += QStringLiteral("QLabel#serverHeaderName { color: ") + text + QStringLiteral("; font-weight: 600; }");
    css += QStringLiteral("QLabel#statusDot { color: ") + green + QStringLiteral("; font-size: 15px; }");
    css += QStringLiteral("QToolButton#headerIconButton { background: transparent; color: ") + muted
           + QStringLiteral("; border: 0; border-radius: 10px; font-size: 22px; min-width: 28px; min-height: 28px; }");
    css += QStringLiteral("QToolButton#headerIconButton:hover { background: ") + hover + QStringLiteral("; color: ") + accentBright + QStringLiteral("; }");
    css += QStringLiteral("QSplitter#bodySplitter, QSplitter#topSplitter { background: transparent; border: 0; }");
    css += QStringLiteral("QSplitter::handle { background: transparent; }");
    css += QStringLiteral("QSplitter::handle:horizontal { width: 12px; }");
    css += QStringLiteral("QSplitter::handle:vertical { height: 12px; }");

    // Channel tree.
    css += QStringLiteral("QTreeWidget#serverTree { background: transparent; color: ") + text
           + QStringLiteral("; border: 0; outline: 0; padding: 2px 0 8px 0; show-decoration-selected: 1; font-size: 15px; }");
    css += QStringLiteral("QTreeWidget#serverTree::item { height: 44px; padding: 4px 5px; border: 0; border-radius: 10px; }");
    css += QStringLiteral("QTreeWidget#serverTree::item:hover:!selected { background: ") + hover + QStringLiteral("; }");
    css += QStringLiteral("QTreeWidget#serverTree::item:selected { background: ") + selected
           + QStringLiteral("; color: ") + text + QStringLiteral("; }");
    css += QStringLiteral("QTreeWidget#serverTree::branch { background: transparent; }");
    // Indicadores explícitos: seta para a direita quando os usuários/subcanais
    // estão ocultos e seta para baixo quando o conteúdo está visível.
    css += QStringLiteral("QTreeWidget#serverTree::branch:closed:has-children { image: url(:/halla/assets/tree-arrow-right.svg); }");
    css += QStringLiteral("QTreeWidget#serverTree::branch:open:has-children { image: url(:/halla/assets/tree-arrow-down.svg); }");

    // Information panel and banner.
    css += QStringLiteral("QLabel#infoBanner { border: 0; border-top-left-radius: 13px; border-top-right-radius: 13px; }");
    css += QStringLiteral("QTextBrowser#infoView { background: transparent; color: ") + text
           + QStringLiteral("; border: 0; padding: 8px 20px 16px 20px; font-size: 14px; }");

    // Chat tabs, editor row and formatting toolbar.
    css += QStringLiteral("QTabWidget#chatTabs { background: transparent; border: 0; }");
    css += QStringLiteral("QTabWidget#chatTabs::pane { background: transparent; border: 0; }");
    css += QStringLiteral("QTabBar::tab { background: transparent; color: ") + muted
           + QStringLiteral("; border: 0; border-radius: 10px; padding: 9px 14px; margin: 5px 2px 2px 2px; }");
    css += QStringLiteral("QTabBar::tab:selected { background: ") + accentSoft + QStringLiteral("; color: ") + accentBright + QStringLiteral("; font-weight: 600; }");
    css += QStringLiteral("QTabBar::tab:hover:!selected { background: ") + hover + QStringLiteral("; color: ") + text + QStringLiteral("; }");
    css += QStringLiteral("QTextBrowser#chatBrowser { background: transparent; color: ") + text
           + QStringLiteral("; border: 0; padding: 6px 10px; font-size: 13px; }");
    css += QStringLiteral("QToolBar#chatFormatBar { background: transparent; border: 0; padding: 3px 10px; spacing: 2px; }");
    css += QStringLiteral("QToolBar#chatFormatBar QToolButton { background: transparent; color: ") + muted
           + QStringLiteral("; border: 0; border-radius: 7px; min-width: 27px; min-height: 27px; font-size: 15px; }");
    css += QStringLiteral("QToolBar#chatFormatBar QToolButton:hover { background: ") + hover + QStringLiteral("; color: ") + accentBright + QStringLiteral("; }");
    css += QStringLiteral("QLineEdit#chatInput { background: ") + input + QStringLiteral("; color: ") + text
           + QStringLiteral("; border: 1px solid ") + border + QStringLiteral("; border-radius: 11px; padding: 9px 12px; min-height: 24px; }");
    css += QStringLiteral("QToolButton#chatSendButton { background: ") + accent
           + QStringLiteral("; color: white; border: 0; border-radius: 11px; min-width: 44px; min-height: 42px; font-size: 18px; }");
    css += QStringLiteral("QToolButton#chatSendButton:hover { background: ") + accentBright + QStringLiteral("; }");

    // Status row.
    css += QStringLiteral("QStatusBar { background: ") + surfaceAlt + QStringLiteral("; color: ") + muted
           + QStringLiteral("; border: 0; border-top: 1px solid ") + border + QStringLiteral("; min-height: 38px; padding: 0 10px; }");
    css += QStringLiteral("QStatusBar::item { border: 0; }");
    css += QStringLiteral("QLabel#newsLabel { color: ") + muted + QStringLiteral("; }");
    css += QStringLiteral("QToolButton#serverTabButton { background: ") + surface
           + QStringLiteral("; color: ") + text + QStringLiteral("; border: 1px solid ") + border
           + QStringLiteral("; border-radius: 11px; padding: 5px 11px; }");
    css += QStringLiteral("QToolButton#serverTabButton:hover { background: ") + hover + QStringLiteral("; }");

    // Controls used throughout dialogs/options, keeping the same rounded language.
    css += QStringLiteral("QPushButton { background: ") + surfaceAlt + QStringLiteral("; color: ") + text
           + QStringLiteral("; border: 1px solid ") + borderStrong + QStringLiteral("; border-radius: 10px; padding: 8px 14px; min-height: 20px; }");
    css += QStringLiteral("QPushButton:hover { background: ") + hover + QStringLiteral("; border-color: ") + accentBright + QStringLiteral("; }");
    css += QStringLiteral("QPushButton:pressed { background: ") + accentSoft + QStringLiteral("; }");
    css += QStringLiteral("QPushButton#primaryButton { background: ") + accent + QStringLiteral("; color: white; border: 0; }");
    css += QStringLiteral("QLineEdit, QTextEdit, QPlainTextEdit, QComboBox, QSpinBox, QDoubleSpinBox { background: ") + input
           + QStringLiteral("; color: ") + text + QStringLiteral("; border: 1px solid ") + borderStrong
           + QStringLiteral("; border-radius: 9px; padding: 6px 9px; selection-background-color: ") + accent + QStringLiteral("; }");
    css += QStringLiteral("QComboBox QAbstractItemView { background: ") + surface + QStringLiteral("; color: ") + text
           + QStringLiteral("; border: 1px solid ") + borderStrong + QStringLiteral("; selection-background-color: ") + accent + QStringLiteral("; }");
    css += QStringLiteral("QCheckBox, QRadioButton { spacing: 8px; }");
    css += QStringLiteral("QGroupBox { border: 1px solid ") + border + QStringLiteral("; border-radius: 12px; margin-top: 14px; padding: 14px 10px 10px 10px; }");
    css += QStringLiteral("QGroupBox::title { subcontrol-origin: margin; left: 12px; padding: 0 5px; background: ") + window + QStringLiteral("; color: ") + text + QStringLiteral("; }");
    css += QStringLiteral("QListWidget, QTableWidget { background: ") + surface + QStringLiteral("; color: ") + text
           + QStringLiteral("; border: 1px solid ") + border + QStringLiteral("; border-radius: 10px; alternate-background-color: ") + surfaceAlt + QStringLiteral("; }");
    css += QStringLiteral("QHeaderView::section { background: ") + surfaceAlt + QStringLiteral("; color: ") + muted
           + QStringLiteral("; border: 0; border-bottom: 1px solid ") + border + QStringLiteral("; padding: 7px; }");
    css += QStringLiteral("QScrollBar:vertical { background: transparent; width: 10px; margin: 2px; }");
    css += QStringLiteral("QScrollBar::handle:vertical { background: ") + borderStrong + QStringLiteral("; border-radius: 5px; min-height: 24px; }");
    css += QStringLiteral("QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical, QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: transparent; border: 0; }");
    css += QStringLiteral("QToolTip { background: ") + surface + QStringLiteral("; color: ") + text
           + QStringLiteral("; border: 1px solid ") + borderStrong + QStringLiteral("; border-radius: 7px; padding: 5px; }");

    // Options dialog keeps the same two-column layout but uses the new surfaces.
    css += QStringLiteral("QListWidget#optionsNav { background: ") + surface + QStringLiteral("; border: 0; outline: 0; padding: 6px; }");
    css += QStringLiteral("QListWidget#optionsNav::item { height: 44px; padding: 5px 10px; border-radius: 10px; }");
    css += QStringLiteral("QListWidget#optionsNav::item:selected { background: ") + selected + QStringLiteral("; color: ") + accentBright + QStringLiteral("; }");
    css += QStringLiteral("QWidget#pageHeader { background: ") + surfaceAlt + QStringLiteral("; border: 0; border-bottom: 1px solid ") + border + QStringLiteral("; }");
    css += QStringLiteral("QLabel#pageTitle { color: ") + text + QStringLiteral("; font-size: 16px; font-weight: 700; }");
    css += QStringLiteral("QLabel#pageSubtitle, QLabel#captionLabel { color: ") + muted + QStringLiteral("; }");
    css += QStringLiteral("QWidget#optionsStack, QScrollArea#optionsScroll, QWidget#optionsPage { background: ") + surface + QStringLiteral("; border: 0; }");
    css += QStringLiteral("QWidget#optionsSep { background: ") + border + QStringLiteral("; }");

    // ------------------------------------------------------------------
    // Tema branco fiel à referência: superfícies quase brancas, separadores
    // cinza-azulados, tipografia compacta e cartões sem o roxo do tema atual.
    // O ramo escuro continua usando a identidade escura existente.
    if (!dark) {
        css += QStringLiteral("QMainWindow, QWidget#mainSurface, QStackedWidget, QSplitter, QWidget { background: #F4F5F7; color: #263238; }");
        css += QStringLiteral("QMenuBar { background: #FFFFFF; color: #263238; border-bottom: 1px solid #DDE2E8; padding: 0 10px; min-height: 34px; font-size: 12px; }");
        css += QStringLiteral("QMenuBar::item { padding: 8px 13px 7px 13px; margin: 0; }");
        css += QStringLiteral("QMenuBar::item:selected { color: #2E6FAE; background: #F3F7FC; border-bottom: 2px solid #2E6FAE; }");
        css += QStringLiteral("QToolBar#mainToolBar { background: #FFFFFF; border: 0; border-bottom: 1px solid #E1E5EA; padding: 7px 10px; spacing: 8px; min-height: 46px; }");
        css += QStringLiteral("QFrame#toolbarGroup { background: #FFFFFF; border: 1px solid #E3E7EC; border-radius: 9px; }");
        css += QStringLiteral("QToolBar#mainToolBar QToolButton { color: #4B77A6; border-radius: 7px; min-width: 30px; min-height: 28px; padding: 2px 6px; }");
        css += QStringLiteral("QToolBar#mainToolBar QToolButton:hover { background: #EEF4FB; }");
        css += QStringLiteral("QToolBar#mainToolBar QToolButton:checked { background: #DDEBFA; color: #245D91; }");
        css += QStringLiteral("QToolBar#mainToolBar::separator { width: 1px; background: #E0E4E9; margin: 4px 2px; }");
        css += QStringLiteral("QFrame#panelCard, QFrame#chatCard { background: #FFFFFF; border: 1px solid #DEE3E9; border-radius: 7px; }");
        css += QStringLiteral("QFrame#treeCardHeader { min-height: 30px; }");
        css += QStringLiteral("QLabel#serverHeaderName { color: #263238; font-size: 12px; font-weight: 600; }");
        css += QStringLiteral("QLabel#statusDot { color: #7BA7D2; font-size: 11px; }");
        css += QStringLiteral("QToolButton#headerIconButton { color: #6A88A8; border-radius: 6px; font-size: 17px; min-width: 24px; min-height: 22px; }");
        css += QStringLiteral("QToolButton#headerIconButton:hover { background: #EEF4FB; color: #2E6FAE; }");
        css += QStringLiteral("QFrame#serverIntroCard { background: #F6F8FA; border: 1px solid #E4E8ED; border-radius: 8px; }");
        css += QStringLiteral("QLabel#serverIntroTitle { color: #34495E; font-size: 12px; font-weight: 600; }");
        css += QStringLiteral("QLabel#serverIntroText { color: #7B8794; font-size: 11px; }");
        css += QStringLiteral("QTreeWidget#serverTree { background: #FFFFFF; color: #263238; padding: 1px 0 4px 0; font-size: 12px; }");
        css += QStringLiteral("QTreeWidget#serverTree::item { height: 22px; padding: 1px 3px; border-radius: 3px; }");
        css += QStringLiteral("QTreeWidget#serverTree::item:hover:!selected { background: #F2F6FA; }");
        css += QStringLiteral("QTreeWidget#serverTree::item:selected { background: #DCE9F8; color: #1F3D5B; }");
        css += QStringLiteral("QTreeWidget#serverTree::branch { background: transparent; }");
        css += QStringLiteral("QTextBrowser#infoView { color: #263238; padding: 10px 22px 16px 22px; font-size: 12px; }");
        css += QStringLiteral("QLabel#infoBanner { border-radius: 6px; }");
        css += QStringLiteral("QTabBar::tab { color: #73808D; border-radius: 4px; padding: 5px 10px; margin: 2px 1px; font-size: 11px; }");
        css += QStringLiteral("QTabBar::tab:selected { background: #E5EEF8; color: #2E6FAE; font-weight: 600; }");
        css += QStringLiteral("QTabBar::tab:hover:!selected { background: #F1F5F9; color: #3E5C78; }");
        css += QStringLiteral("QTextBrowser#chatBrowser { color: #33414D; padding: 4px 8px; font-size: 11px; }");
        css += QStringLiteral("QToolBar#chatFormatBar { padding: 1px 6px; min-height: 24px; }");
        css += QStringLiteral("QToolBar#chatFormatBar QToolButton { color: #6A88A8; min-width: 22px; min-height: 21px; font-size: 12px; }");
        css += QStringLiteral("QLineEdit#chatInput { background: #FFFFFF; color: #263238; border: 1px solid #D7DEE6; border-radius: 5px; padding: 6px 9px; min-height: 20px; font-size: 11px; }");
        css += QStringLiteral("QToolButton#chatSendButton { background: #2E6FAE; border-radius: 5px; min-width: 32px; min-height: 30px; font-size: 14px; }");
        css += QStringLiteral("QStatusBar { background: #F8F9FA; color: #697784; border-top: 1px solid #DDE2E8; min-height: 27px; padding: 0 7px; font-size: 11px; }");
        css += QStringLiteral("QLabel#newsLabel { color: #647383; font-size: 10px; }");
        css += QStringLiteral("QToolButton#serverTabButton { background: #FFFFFF; color: #3D5266; border: 1px solid #D8E0E8; border-radius: 5px; padding: 3px 8px; font-size: 11px; }");
        css += QStringLiteral("QPushButton { background: #FFFFFF; color: #263238; border: 1px solid #C8D2DC; border-radius: 4px; padding: 5px 10px; min-height: 17px; font-size: 11px; }");
        css += QStringLiteral("QPushButton:hover { background: #EEF4FB; border-color: #8EB2D5; }");
        css += QStringLiteral("QPushButton#primaryButton { background: #2E6FAE; color: #FFFFFF; border: 0; }");
        css += QStringLiteral("QLineEdit, QTextEdit, QPlainTextEdit, QComboBox, QSpinBox, QDoubleSpinBox { background: #FFFFFF; color: #263238; border: 1px solid #C8D2DC; border-radius: 4px; padding: 5px 7px; }");
        css += QStringLiteral("QGroupBox { border: 1px solid #DDE3E9; border-radius: 6px; padding: 10px 8px 7px 8px; }");
        css += QStringLiteral("QGroupBox::title { background: #F4F5F7; color: #3D5266; }");
        css += QStringLiteral("QListWidget, QTableWidget { background: #FFFFFF; color: #263238; border: 1px solid #DDE3E9; border-radius: 5px; alternate-background-color: #F8FAFC; }");
        css += QStringLiteral("QHeaderView::section { background: #F1F4F7; color: #5E6E7D; border-bottom: 1px solid #DDE3E9; padding: 5px; }");
        css += QStringLiteral("QToolTip { background: #FFFFFF; color: #263238; border: 1px solid #C8D2DC; border-radius: 3px; padding: 4px; }");
    }

    return css;
}

void HTheme::apply() {
    const bool dark = isDark();
    QStyle* style = QStyleFactory::create(QStringLiteral("Fusion"));
    if (style) QApplication::setStyle(style);

    QPalette pal;
    if (dark) {
        const QColor window("#080D1C");
        const QColor base("#0D1223");
        const QColor text("#F5F4FF");
        const QColor muted("#A8AABD");
        pal.setColor(QPalette::Window, window);
        pal.setColor(QPalette::WindowText, text);
        pal.setColor(QPalette::Base, base);
        pal.setColor(QPalette::AlternateBase, QColor("#11182B"));
        pal.setColor(QPalette::Text, text);
        pal.setColor(QPalette::Button, QColor("#11182B"));
        pal.setColor(QPalette::ButtonText, text);
        pal.setColor(QPalette::ToolTipBase, QColor("#11182B"));
        pal.setColor(QPalette::ToolTipText, text);
        pal.setColor(QPalette::PlaceholderText, muted);
        pal.setColor(QPalette::Highlight, QColor("#7C3AED"));
        pal.setColor(QPalette::HighlightedText, Qt::white);
        pal.setColor(QPalette::Link, QColor("#A78BFA"));
        pal.setColor(QPalette::Light, QColor("#2A3555"));
        pal.setColor(QPalette::Midlight, QColor("#202A45"));
        pal.setColor(QPalette::Mid, QColor("#1B2540"));
        pal.setColor(QPalette::Dark, QColor("#050817"));
        pal.setColor(QPalette::Shadow, QColor("#030510"));
        pal.setColor(QPalette::Disabled, QPalette::Text, QColor("#62667B"));
        pal.setColor(QPalette::Disabled, QPalette::WindowText, QColor("#62667B"));
        pal.setColor(QPalette::Disabled, QPalette::ButtonText, QColor("#62667B"));
    } else {
        pal = QApplication::style()->standardPalette();
        pal.setColor(QPalette::Window, QColor("#F4F5F7"));
        pal.setColor(QPalette::WindowText, QColor("#263238"));
        pal.setColor(QPalette::Base, QColor("#FFFFFF"));
        pal.setColor(QPalette::AlternateBase, QColor("#F8FAFC"));
        pal.setColor(QPalette::Text, QColor("#263238"));
        pal.setColor(QPalette::Button, QColor("#FFFFFF"));
        pal.setColor(QPalette::ButtonText, QColor("#263238"));
        pal.setColor(QPalette::ToolTipBase, QColor("#FFFFFF"));
        pal.setColor(QPalette::ToolTipText, QColor("#263238"));
        pal.setColor(QPalette::PlaceholderText, QColor("#8A98A6"));
        pal.setColor(QPalette::Highlight, QColor("#2E6FAE"));
        pal.setColor(QPalette::HighlightedText, Qt::white);
        pal.setColor(QPalette::Link, QColor("#2E6FAE"));
        pal.setColor(QPalette::Disabled, QPalette::Text, QColor("#9AA5AF"));
        pal.setColor(QPalette::Disabled, QPalette::WindowText, QColor("#9AA5AF"));
        pal.setColor(QPalette::Disabled, QPalette::ButtonText, QColor("#9AA5AF"));
    }
    QApplication::setPalette(pal);
    qApp->setStyleSheet(styleSheet(dark));
}
