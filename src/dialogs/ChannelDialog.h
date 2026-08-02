#pragma once

#include <QDialog>
#include <QLineEdit>
#include <QTextEdit>
#include <QComboBox>
#include <QSlider>
#include <QSpinBox>
#include <QRadioButton>
#include <QCheckBox>
#include <QLabel>
#include "core/Models.h"

// Diálogo "Criar canal" / "Editar canal" — réplica fiel do diálogo do Halla.
class ChannelDialog : public QDialog {
    Q_OBJECT
public:
    ChannelDialog(const QString& title, const ServerData* server, QWidget* parent = nullptr);

    void setChannel(const Channel& c);
    Channel resultChannel() const;

private:
    const ServerData* m_server;
    QLineEdit* m_name;
    QLineEdit* m_topic;
    QTextEdit* m_desc;
    QLineEdit* m_password;
    QComboBox* m_codec;
    QSlider* m_quality;
    QLabel* m_qualityLabel;
    QComboBox* m_sortAfter;
    QSpinBox* m_maxClients;
    QRadioButton* m_temp;
    QRadioButton* m_semi;
    QRadioButton* m_perm;
    QCheckBox* m_default;
    QCheckBox* m_moderated;
};
