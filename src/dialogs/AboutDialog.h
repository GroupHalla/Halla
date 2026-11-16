#pragma once

#include <QDialog>

// "About Halla" window with banner, logo and version information
class AboutDialog : public QDialog {
    Q_OBJECT
public:
    explicit AboutDialog(QWidget* parent = nullptr);
};
