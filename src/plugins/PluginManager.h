#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QHash>
#include <QObject>
#include <QString>
#include <QList>

struct ServerData;
class QWidget;
class QLibrary;
class TalkingOverlay;
struct HallaPluginApi;
struct HallaHostApi;

struct AddonInfo {
    QString id;
    QString name;
    QString version;
    QString author;
    QString description;
    QString error;
    QString installPath;
    bool official = false;
    bool builtIn = false;
    bool enabled = false;
    bool loaded = false;
    bool configurable = false;
    QJsonArray settingsSchema;
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

    void publishClientState(const ServerData* data);
    QJsonObject currentClientState() const { return m_currentState; }

    static QString catalogUrl();
    static QString addonsRoot();

signals:
    void addonsChanged();

private:
    explicit PluginManager(QObject* parent = nullptr);
    ~PluginManager() override;
    Q_DISABLE_COPY_MOVE(PluginManager)

    struct Record;
    QHash<QString, Record*> m_records;
    TalkingOverlay* m_overlay = nullptr;
    QJsonObject m_currentState;
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
    void dispatchEvent(const QJsonObject& event);
    Record* record(const QString& id) const;
    static QString platformKey();
    static bool copyTree(const QString& source, const QString& destination,
                         QString* error);
};
