#pragma once

#include <QDialog>
#include <QTableWidget>
#include <QComboBox>

// Janela "Registro do cliente" (Client Log) — nivel filtrável, cores e exportação.
class LogDialog : public QDialog {
    Q_OBJECT
public:
    explicit LogDialog(QWidget* parent = nullptr);

    void append(int level, const QString& timestamp, const QString& text);

private:
    void rebuild();
    struct Entry { int level; QString ts; QString text; };
    QList<Entry> m_entries;

    QTableWidget* m_table = nullptr;
    QComboBox* m_filter = nullptr;
    class QCheckBox* m_autoscroll = nullptr;
};
