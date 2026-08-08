#pragma once

#include <QDialog>
#include <QListWidget>
#include <QPushButton>
#include <QLabel>
#include <QVBoxLayout>
#include <QHBoxLayout>

class ScreenShareDialog : public QDialog {
    Q_OBJECT
public:
    explicit ScreenShareDialog(QWidget* parent = nullptr);
    int selectedSource() const { return m_selectedSource; } // 0 = entire screen, 1 = app window

private:
    QListWidget* m_list;
    int m_selectedSource = 0;
};
