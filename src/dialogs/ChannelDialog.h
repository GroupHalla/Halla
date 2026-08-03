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

class NetSession;

// Diálogo "Criar canal" / "Editar canal" — réplica fiel do diálogo do Halla.
class ChannelDialog : public QDialog {
    Q_OBJECT
public:
    ChannelDialog(const QString& title, const ServerData* server, NetSession* net, QWidget* parent = nullptr);

    void setChannel(const Channel& c);
    Channel resultChannel() const;

private:
    const ServerData* m_server;
    NetSession* m_net;
    QLineEdit* m_name;
    QLineEdit* m_topic;
    QTextEdit* m_desc;
    QLineEdit* m_password;
    QComboBox* m_codec;
    QSlider* m_quality;
    QLabel* m_qualityLabel;
    QSpinBox* m_bitrate;
    QComboBox* m_sortAfter;
    QSpinBox* m_maxClients;
    QRadioButton* m_temp;
    QRadioButton* m_semi;
    QRadioButton* m_perm;
    QCheckBox* m_default;
    QCheckBox* m_moderated;
    
    QComboBox* m_permGroupCombo;
    QCheckBox* m_chkJoin;
    QCheckBox* m_chkTalk;
    QCheckBox* m_chkWhisper;
    QCheckBox* m_chkUpload;
    QCheckBox* m_chkDownload;
    QCheckBox* m_chkChat;
    QJsonObject m_localGroupPerms;
    int m_lastGid = -1;
    bool m_isUpdatingPerms = false;
    
    void saveCurrentGroupPerms();
    void loadGroupPerms(int gid);
};
