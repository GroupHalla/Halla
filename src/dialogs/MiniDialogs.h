#pragma once

#include <QDialog>
#include <QLineEdit>
#include <QSlider>
#include <QSpinBox>
#include <QTextEdit>

// Diálogos pequenos do estilo Halla.
class PrivilegeKeyDialog : public QDialog {
    Q_OBJECT
public:
    explicit PrivilegeKeyDialog(QWidget* parent = nullptr);
    QString key() const;
private:
    QLineEdit* m_key;
};

class PokeDialog : public QDialog {
    Q_OBJECT
public:
    explicit PokeDialog(const QString& target, QWidget* parent = nullptr);
    QString message() const;
private:
    QLineEdit* m_msg;
};

class KickBanDialog : public QDialog {
    Q_OBJECT
public:
    enum Mode { KickChannel, KickServer, Ban };
    KickBanDialog(const QString& target, Mode mode, QWidget* parent = nullptr);
    QString reason() const;
    int banMinutes() const; // 0 = permanente
private:
    QTextEdit* m_reason;
    QSpinBox* m_duration;
};

class VolumeDialog : public QDialog {
    Q_OBJECT
public:
    VolumeDialog(const QString& target, int currentDb, QWidget* parent = nullptr);
    int volume() const;
private:
    QSlider* m_slider;
    class QLabel* m_label;
};

class ServerConnectionInfoDialog : public QDialog {
    Q_OBJECT
public:
    ServerConnectionInfoDialog(const struct ServerData* data, class NetSession* net, QWidget* parent = nullptr);
};
