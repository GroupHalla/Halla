#pragma once

#include <QDialog>
#include <QTableWidget>
#include <QLineEdit>

struct ServerData;

// Janela "Listas de sussurro" (Whisper Lists) — destinos por ID único
class WhisperDialog : public QDialog {
    Q_OBJECT
public:
    explicit WhisperDialog(const ServerData* data, QWidget* parent = nullptr);
    // uids da lista de sussurro ATIVA (0 se nenhuma)
    static QStringList activeWhisperUids();
private:
    QTableWidget* m_table;
    const ServerData* m_data;
    void reload();
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
