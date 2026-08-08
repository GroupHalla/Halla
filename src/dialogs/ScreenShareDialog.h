#pragma once

#include <QDialog>
#include <QListWidget>
#include <QPushButton>
#include <QLabel>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QTabWidget>

class ScreenShareDialog : public QDialog {
    Q_OBJECT
public:
    explicit ScreenShareDialog(QWidget* parent = nullptr);
    int selectedSourceType() const { return m_selectedSourceType; } // 0 = screen, 1 = window
    quintptr selectedSourceId() const { return m_selectedSourceId; } // WId / HWND

private:
    void populateWindows();
    void populateScreens();

    QTabWidget* m_tabs;
    QListWidget* m_screenList;
    QListWidget* m_windowList;
    
    int m_selectedSourceType = 0;
    quintptr m_selectedSourceId = 0;
};
