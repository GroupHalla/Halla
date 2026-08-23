#pragma once

#include <QDialog>
#include <QListWidget>
#include <QStackedWidget>
#include <QStringList>

class QLabel;
struct ServerData;

// Janela "Opções" — réplica do diálogo de opções do Halla (clássico):
// menu lateral de categorias com ícones grandes à esquerda (separado por
// linha de 1px), cabeçalho com gradiente claro dentro do painel de conteúdo
// (título em negrito + subtítulo + ícone da seção à direita), fieldsets
// estilo Windows e os botões OK/Cancelar/Aplicar no canto inferior direito.
class OptionsDialog : public QDialog {
    Q_OBJECT
public:
    explicit OptionsDialog(QWidget* parent = nullptr, const ServerData* whisperData = nullptr);
    void selectPage(const QString& pageName);

signals:
    void themeChanged();
    void designChanged();
    void hotkeysChanged();
    void languageChanged();
    void whisperListsChanged();

private:
    QWidget* pageApplication();
    QWidget* pageDesign();
    QWidget* pageNotifications();
    QWidget* pagePlayback();
    QWidget* pageCapture();
    QWidget* pageHotkeys();
    QWidget* pageWhisper();
    QWidget* pageSecurity();
    QWidget* pageAddons();

    void apply();

    QListWidget* m_nav = nullptr;
    QStackedWidget* m_stack = nullptr;
    QLabel* m_headerTitle = nullptr;    // título da seção (negrito)
    QLabel* m_headerSubtitle = nullptr; // subtítulo da seção (menor)
    QLabel* m_headerIcon = nullptr;     // ícone da seção no canto direito
    QStringList m_pageSubtitles;        // subtítulo de cada página
    const ServerData* m_whisperData = nullptr;
};
