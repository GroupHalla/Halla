#pragma once

#include <QWidget>

// Tela inicial exibida quando não há conexões abertas (água de marca + atalhos)
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
