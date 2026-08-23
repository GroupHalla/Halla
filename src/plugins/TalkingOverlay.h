#pragma once

#include <QJsonObject>
#include <QTimer>
#include <QWidget>

class TalkingOverlay final : public QWidget {
    Q_OBJECT
public:
    explicit TalkingOverlay(QWidget* parent = nullptr);

    void applySettings(const QJsonObject& settings);
    void updateClientState(const QJsonObject& payload);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    struct Row {
        QString name;
        bool talking = false;
        bool whispering = false;
        bool muted = false;
        bool self = false;
    };

    void rebuildRows();
    void updateVisibility();
    void updatePosition();
    bool foregroundLooksLikeGame() const;

    QJsonObject m_settings;
    QJsonObject m_state;
    QList<Row> m_rows;
    QTimer m_foregroundTimer;
};
