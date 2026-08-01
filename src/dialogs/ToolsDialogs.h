#pragma once

#include <QDialog>
#include <QTableWidget>
#include <QLineEdit>

// Janela "Listas de sussurro" (Whisper Lists)
class WhisperDialog : public QDialog {
    Q_OBJECT
public:
    explicit WhisperDialog(QWidget* parent = nullptr);
private:
    QTableWidget* m_table;
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

// Janela "Transferência de arquivos"
class FileTransferDialog : public QDialog {
    Q_OBJECT
public:
    explicit FileTransferDialog(QWidget* parent = nullptr);
private:
    QTableWidget* m_table;
};
