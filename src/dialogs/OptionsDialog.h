#pragma once

#include <QDialog>
#include <QListWidget>
#include <QStackedWidget>

// Janela "Opções" — réplica do diálogo de opções do TS3:
// faixa azul no topo, lista de categorias com ícones à esquerda e páginas à direita.
class OptionsDialog : public QDialog {
    Q_OBJECT
public:
    explicit OptionsDialog(QWidget* parent = nullptr);
    void selectPage(const QString& pageName);

signals:
    void themeChanged();
    void designChanged();
    void hotkeysChanged();

private:
    QWidget* pageApplication();
    QWidget* pageDesign();
    QWidget* pageNotifications();
    QWidget* pagePlayback();
    QWidget* pageCapture();
    QWidget* pageHotkeys();
    QWidget* pageSecurity();
    QWidget* pageAddons();

    void apply();

    QListWidget* m_nav = nullptr;
    QStackedWidget* m_stack = nullptr;
};
