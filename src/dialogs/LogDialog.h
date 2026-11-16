#pragma once

#include <QDialog>
#include <QTableWidget>
#include <QComboBox>
#include "AppLog.h"

// "Client log" window -- filterable level, colors and export.
class LogDialog : public QDialog {
    Q_OBJECT
public:
    explicit LogDialog(QWidget* parent = nullptr);

    void append(int level, const QString& timestamp, const QString& text);

    // Loads the history persisted in halla.log (messages written before the window
    // opened). Useful to diagnose decisions taken before the dialog existed (e.g.
    // the GPU/CPU encoder at the start of a broadcast).
    void loadFromFile();

private:
    void rebuild();
    static AppLog::Level levelFromName(const QString& name);
    struct Entry { int level; QString ts; QString text; };
    QList<Entry> m_entries;

    QTableWidget* m_table = nullptr;
    QComboBox* m_filter = nullptr;
    class QCheckBox* m_autoscroll = nullptr;
};
