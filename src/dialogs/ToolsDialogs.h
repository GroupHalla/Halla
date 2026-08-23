#pragma once

#include <QDialog>
#include <QTableWidget>
#include <QLineEdit>

struct ServerData;

class QListWidget;
class QListWidgetItem;
class QTreeWidget;
class QTreeWidgetItem;
class QComboBox;
class QLineEdit;
class HotkeyEdit;

// Janela "Listas de sussurro" (Whisper Lists) — destinos por ID único
class WhisperDialog : public QDialog {
    Q_OBJECT
public:
    explicit WhisperDialog(const ServerData* data, QWidget* parent = nullptr);
    // uids da lista de sussurro ATIVA (0 se nenhuma)
    static QStringList activeWhisperUids();
signals:
    // Emitido após Aplicar/OK; permite registrar a tecla sem reiniciar.
    void settingsSaved();
private slots:
    void onNewList();
    void onRemoveList();
    void onRenameList();
    void onReload();
    void onSelectedListChanged();
    void onTargetsChanged();
    void onSearchTextChanged(const QString& text);
    void onFilterChanged(int index);
    void onServerTreeDoubleClicked(QTreeWidgetItem* item, int column);
    void onTargetItemChanged(QTreeWidgetItem* item, int column);
    void onApply();
    void onAccept();
private:
    const ServerData* m_data;
    
    // Left column
    QListWidget* m_syncList;
    QListWidget* m_localList;
    QPushButton* m_btnNew;
    QPushButton* m_btnRemove;
    QPushButton* m_btnRename;
    QPushButton* m_btnReload;

    // Center column
    HotkeyEdit* m_hotkeyEdit;
    HotkeyEdit* m_replyHotkeyEdit;
    QComboBox* m_scopeCombo;
    QTreeWidget* m_targetsTree;

    // Right column
    QComboBox* m_filterCombo;
    QTreeWidget* m_serverTree;
    QLineEdit* m_searchEdit;

    // Data lists
    QList<QJsonObject> m_whispers;
    bool m_isLoading = false;
    
    void loadSettings();
    void saveSettings();
    void populateServerTree();
    void populateTargetsTreeForSelected();
    void addTargetToSelected(const QString& name, const QString& uid, bool isChannel);
    void filterTree(QTreeWidgetItem* item, const QString& text);
};

// Janela "Contatos" — catálogo de amigos offline
class ContactsDialog : public QDialog {
    Q_OBJECT
public:
    explicit ContactsDialog(QWidget* parent = nullptr);
private:
    QTableWidget* m_table;
    void reload();
};

class QComboBox;
class QProgressBar;
class NetSession;
struct ServerData;

// Janela "Transferência de arquivos" — arquivos por canal no servidor (v3)
class FileTransferDialog : public QDialog {
    Q_OBJECT
public:
    explicit FileTransferDialog(NetSession* net, ServerData* data,
                                QWidget* parent = nullptr);
private:
    void refresh();
    int currentChannel() const;
    NetSession* m_net;
    ServerData* m_data;
    QComboBox* m_channels;
    QTableWidget* m_table;
};
