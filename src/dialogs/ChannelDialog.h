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

// Diálogo "Criar canal" / "Editar canal" — editor de propriedades e das
// permissões de canal já suportadas pelo protocolo Halla.
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
    QCheckBox* m_hideSymbol;

    // Editor visual em duas colunas, mantendo somente as seis permissões
    // específicas que o servidor Halla já aplica por grupo.
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
