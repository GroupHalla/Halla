#pragma once

#include <QDialog>
#include <QLineEdit>
#include <QComboBox>
#include <QPushButton>

// Diálogo "Conectar" — campos e disposição iguais aos do Halla:
// apelido, endereço do servidor, senha + seção expansível "Mais >>" com
// apelido fonético e perfis de captura/reprodução.
class ConnectDialog : public QDialog {
    Q_OBJECT
public:
    explicit ConnectDialog(QWidget* parent = nullptr);

    QString nickname() const;
    QString address() const;
    quint16 port() const;
    QString password() const;
    QString phoneticNickname() const;

    void setNickname(const QString& n);
    void setAddress(const QString& a, quint16 port = 9987);

private:
    QLineEdit* m_host;
    QLineEdit* m_port;
    QLineEdit* m_nick;
    QLineEdit* m_password;
    QLineEdit* m_phonetic;
    QComboBox* m_captureProfile;
    QComboBox* m_playbackProfile;
    QWidget* m_moreArea;
    QPushButton* m_moreBtn;
};
