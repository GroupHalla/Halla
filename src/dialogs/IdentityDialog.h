#pragma once

#include <QDialog>
#include <QTableWidget>

// Janela "Identidades" — gerencia as identidades locais (ID único),
// como a janela de identidades do Halla.
class IdentityDialog : public QDialog {
    Q_OBJECT
public:
    explicit IdentityDialog(QWidget* parent = nullptr);

    static QString generateUniqueId();
    static QList<QStringList> loadAll();     // [default, nome, fonético, id único]
    static void saveAll(const QList<QStringList>& rows);
    static QString defaultNickname();

private:
    void reload();
    int selectedRow() const;

    QTableWidget* m_table;
    class QToolButton* m_defaultBtn;
};
