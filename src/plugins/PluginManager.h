#pragma once

#include "halla_plugin_api.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>

struct ServerData;
class QWidget;
class QLibrary;
class TalkingOverlay;
class ServerTab;

struct AddonInfo {
    QString id;
    QString name;
    QString version;
    QString author;
    QString description;
    QString error;
    QString installPath;
    QStringList capabilities;
    QJsonArray settingsSchema;
    bool official = false;
    bool builtIn = false;
    bool enabled = false;
    bool loaded = false;
    bool configurable = false;
};

struct PluginAudioControl {
    float gain = 1.0f;
    float pan = 0.0f;
    bool radio = false;
    float radioStrength = 0.0f;
    float radioNoise = 0.0f;
};

class PluginManager final : public QObject {
    Q_OBJECT
public:
    static PluginManager& instance();

    void initialize();
    void shutdown();

    QList<AddonInfo> addons() const;
    bool setEnabled(const QString& id, bool enabled, QString* error = nullptr);
    bool configureAddon(const QString& id, QWidget* parent, QString* error = nullptr);
    bool installPackage(const QString& packagePath, QWidget* parent,
                        QString* installedId = nullptr, QString* error = nullptr);
    bool removeAddon(const QString& id, QString* error = nullptr);
    void showCatalog(QWidget* parent);
    void openAddonsFolder();

    /* Cada aba recebe um identificador estável durante sua vida útil. */
    quint64 registerSession(ServerTab* tab);
    void unregisterSession(ServerTab* tab);
    void setActiveSession(ServerTab* tab);
    quint64 connectionId(ServerTab* tab) const;
    void publishClientState(const ServerData* data); // compatibilidade/testes
    void publishClientState(ServerTab* tab);
    QJsonObject currentClientState() const { return m_currentState; }

    /* Chamadas internas do motor de voz; nunca atravessam a rede. */
    void processAudio(quint64 connectionId, int userId, uint32_t stage,
                      int16_t* samples, uint32_t frames, uint32_t channels,
                      uint32_t sampleRate);
    PluginAudioControl audioControl(quint64 connectionId, int userId) const;

    void triggerUiAction(const QString& pluginId, const QString& actionId);
    void announceUiActions();

    static QString catalogUrl();
    static QString addonsRoot();

signals:
    void addonsChanged();
    void pluginNotification(const QString& title, const QString& message,
                            int timeoutMs);
    void pluginActionRegistered(const QString& pluginId, const QString& actionId,
                                const QString& label, const QString& shortcut);
    void pluginActionRemoved(const QString& pluginId, const QString& actionId);

private:
    explicit PluginManager(QObject* parent = nullptr);
    ~PluginManager() override;
    Q_DISABLE_COPY_MOVE(PluginManager)

    struct Record;
    QHash<QString, Record*> m_records;
    TalkingOverlay* m_overlay = nullptr;
    QJsonObject m_currentState;
    QHash<quint64, QPointer<ServerTab>> m_sessions;
    QHash<ServerTab*, quint64> m_sessionIds;
    quint64 m_activeSessionId = 0;
    quint64 m_nextSessionId = 1;
    bool m_initialized = false;
    bool m_shuttingDown = false;

    void addOfficialOverlay();
    void scanInstalled();
    bool readManifest(const QString& directory, QJsonObject* manifest,
                      QString* error) const;
    bool load(Record* record, QString* error = nullptr);
    void unload(Record* record);
    void notifySettingsChanged(Record* record);
    QJsonObject settingsFor(const QString& id, const QJsonArray& schema) const;
    void saveSettings(const QString& id, const QJsonObject& settings);
    void dispatchEvent(const QJsonObject& event, const QString& onlyPlugin = QString());
    void publishSessionEvent(quint64 connectionId);
    void handlePluginData(quint64 connectionId, int senderId,
                          const QString& pluginId, const QString& topic,
                          const QByteArray& data);
    QJsonObject connectionState(quint64 connectionId) const;
    QJsonObject compactClientState(const ServerData* data, quint64 connectionId) const;
    ServerTab* session(quint64 connectionId) const;
    Record* record(const QString& id) const;
    static QString platformKey();
    static bool copyTree(const QString& source, const QString& destination,
                         QString* error);
};
