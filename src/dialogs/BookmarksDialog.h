#pragma once

#include <QDialog>
#include <QTableWidget>
#include <QLineEdit>
#include <QCheckBox>

struct Bookmark {
    QString label;
    QString address;
    quint16 port = 9987;
    QString nickname;
    QString password;
    bool autoconnect = false;
};

// Janela "Gerenciar favoritos" (Bookmarks Manager do Halla)
class BookmarksDialog : public QDialog {
    Q_OBJECT
public:
    explicit BookmarksDialog(QWidget* parent = nullptr);

    // pré-preenche o formulário com um novo favorito
    void prefill(const QString& label, const QString& address, quint16 port,
                 const QString& nickname);

    static QList<Bookmark> loadAll();
    static void saveAll(const QList<Bookmark>& list);

signals:
    void connectRequested(const QString& address, quint16 port,
                          const QString& nickname, const QString& password);
    void changed();

private:
    void reload();
    void storeForm();
    int currentRow() const;

    QTableWidget* m_table;
    QLineEdit* m_label;
    QLineEdit* m_address;
    QLineEdit* m_port;
    QLineEdit* m_nickname;
    QLineEdit* m_password;
    QCheckBox* m_autoconnect;
    QList<Bookmark> m_list;
};
