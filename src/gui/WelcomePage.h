#pragma once

#include <QWidget>

// Start screen shown when no connection is open (watermark + shortcuts)
class WelcomePage : public QWidget {
    Q_OBJECT
public:
    explicit WelcomePage(QWidget* parent = nullptr);

signals:
    void connectRequested();
    void bookmarksRequested();

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override {} // placeholder
};
