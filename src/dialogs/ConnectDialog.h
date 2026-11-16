#pragma once

#include <QDialog>
#include <QLineEdit>
#include <QComboBox>
#include <QPushButton>

// "Connect" dialog -- fields and layout identical to Halla's: nickname, server
// address, password + expandable "More >>" section with the phonetic nickname and
// the capture/playback profiles.
class ConnectDialog : public QDialog {
    Q_OBJECT
public:
    explicit ConnectDialog(QWidget* parent = nullptr);

    QString nickname() const;
    QString address() const;
    quint16 port() const;
    QString password() const;

    void setNickname(const QString& n);
    void setAddress(const QString& a, quint16 port = 9987);

private:
    QLineEdit* m_host;
    QLineEdit* m_port;
    QLineEdit* m_nick;
    QLineEdit* m_password;
    QWidget* m_moreArea;
    QPushButton* m_moreBtn;
};
