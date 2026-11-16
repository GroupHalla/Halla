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
class QListWidget;
class QTableWidget;
class QPushButton;
class QTabWidget;

// "Create channel" / "Edit channel" dialog -- editor for the properties and for
// the channel permissions already supported by the Halla protocol.
class ChannelDialog : public QDialog {
    Q_OBJECT
public:
    ChannelDialog(const QString& title, const ServerData* server, NetSession* net, QWidget* parent = nullptr);

    void setChannel(const Channel& c);
    void setTemporaryOwnerMode(bool enabled);
    bool temporaryOwnerMode() const { return m_temporaryOwnerMode; }
    bool passwordWasEdited() const { return m_password && m_password->isModified(); }
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
    QCheckBox* m_hideSymbol;
    QCheckBox* m_tempChannelParent;
    QTabWidget* m_tabs = nullptr;
    bool m_temporaryOwnerMode = false;

    // Two-column visual editor for the channel-specific permissions that the Halla
    // server applies per group, including visibility.
    QListWidget* m_lcaList = nullptr;
    QTableWidget* m_permTable = nullptr;
    QComboBox* m_permGroupCombo = nullptr;
    QPushButton* m_lcaAdd = nullptr;
    QPushButton* m_lcaDelete = nullptr;
    QJsonObject m_localGroupPerms;
    int m_lastGid = -1;
    bool m_isUpdatingPerms = false;

    void saveCurrentGroupPerms();
    void loadGroupPerms(int gid);
    void rebuildLcaList();
    int selectedGroupId() const;
};
