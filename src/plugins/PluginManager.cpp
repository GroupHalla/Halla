#include "PluginManager.h"

#include "TalkingOverlay.h"
#include "RadioVoiceEffect.h"
#include "AppLog.h"
#include "Models.h"
#include "Settings.h"
#include "gui/ServerTab.h"
#include "net/NetSession.h"
#include "net/VoiceEngine.h"
#include "version.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QElapsedTimer>
#include <QHeaderView>
#include <QJsonDocument>
#include <QKeySequence>
#include <QLabel>
#include <QLibrary>
#include <QLineEdit>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QSet>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QThread>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>

struct PluginManager::Record {
    struct AudioUserState {
        bool hasPosition = false;
        HallaVec3 position{};
        float minDistance = 1.0f;
        float maxDistance = 100.0f;
        float rolloff = 1.0f;
        float gain = 1.0f;
        float pan = 0.0f;
        bool radio = false;
        float radioStrength = 0.0f;
        float radioNoise = 0.0f;
    };
    struct AudioConnectionState {
        bool hasListener = false;
        HallaTransform listener{};
        QHash<int, AudioUserState> users;
    };
    struct UiAction {
        QString label;
        QString shortcut;
        void* pluginContext = nullptr;
        HallaUiActionFn callback = nullptr;
    };

    static uint64_t nextInstanceToken() {
        static std::atomic<uint64_t> token{1};
        return token.fetch_add(1, std::memory_order_relaxed);
    }

    AddonInfo info;
    QJsonObject manifest;
    QLibrary* library = nullptr;
    const HallaPluginApi* api = nullptr;
    HallaHostApi host{};
    HallaCoreApiV1 core{};
    HallaConnectionApiV1 connection{};
    HallaAudioApiV1 audio{};
    HallaDataApiV1 data{};
    HallaUiApiV1 ui{};
    QByteArray settingsCache;
    HallaAudioProcessorFn audioProcessor = nullptr;
    void* audioProcessorContext = nullptr;
    uint32_t audioStages = 0;
    HallaPluginDataFn dataHandler = nullptr;
    void* dataHandlerContext = nullptr;
    QHash<quint64, AudioConnectionState> audioConnections;
    QHash<QString, UiAction> uiActions;
    uint64_t instanceToken = nextInstanceToken();

    bool hasCapability(const char* capability) const {
        return info.capabilities.contains(QString::fromLatin1(capability));
    }

    static size_t copyBytes(const QByteArray& bytes, char* buffer, size_t bufferSize) {
        const size_t required = size_t(bytes.size()) + 1;
        if (buffer && bufferSize > 0) {
            const size_t count = qMin(bufferSize - 1, size_t(bytes.size()));
            if (count) std::memcpy(buffer, bytes.constData(), count);
            buffer[count] = '\0';
        }
        return required;
    }

    static bool onUiThread() {
        return qApp && QThread::currentThread() == qApp->thread();
    }

    static void hostLog(void* context, HallaPluginLogLevel level,
                        const char* message) {
        Record* record = static_cast<Record*>(context);
        const QString text = QStringLiteral("Complemento %1: %2")
            .arg(record ? record->info.id : QStringLiteral("?"),
                 QString::fromUtf8(message ? message : ""));
        if (level == HALLA_PLUGIN_LOG_ERROR) AppLog::error(text);
        else if (level == HALLA_PLUGIN_LOG_WARNING) AppLog::warn(text);
        else if (level == HALLA_PLUGIN_LOG_DEBUG) AppLog::debug(text);
        else AppLog::info(text);
    }

    static size_t hostSettings(void* context, char* buffer, size_t bufferSize) {
        Record* record = static_cast<Record*>(context);
        return record ? copyBytes(record->settingsCache, buffer, bufferSize) : 0;
    }

    static void hostRequestState(void* context) {
        Record* record = static_cast<Record*>(context);
        PluginManager& manager = PluginManager::instance();
        if (record && !manager.m_currentState.isEmpty()) {
            manager.dispatchEvent(QJsonObject{
                {QStringLiteral("event"), QStringLiteral("client_state")},
                {QStringLiteral("payload"), manager.m_currentState}
            }, record->info.id);
        }
    }

    static uint64_t coreTime(void*) {
        using namespace std::chrono;
        return uint64_t(duration_cast<milliseconds>(
            steady_clock::now().time_since_epoch()).count());
    }

    static size_t coreAppInfo(void*, char* buffer, size_t bufferSize) {
        const QJsonObject info{
            {QStringLiteral("name"), QStringLiteral("Halla Desktop")},
            {QStringLiteral("version"), QString::fromUtf8(halla::kAppVersion)},
            {QStringLiteral("pluginAbi"), int(HALLA_PLUGIN_ABI_VERSION)},
            {QStringLiteral("platform"), PluginManager::platformKey()},
            {QStringLiteral("interfaces"), QJsonArray{
                QString::fromLatin1(HALLA_INTERFACE_CORE_V1),
                QString::fromLatin1(HALLA_INTERFACE_CONNECTION_V1),
                QString::fromLatin1(HALLA_INTERFACE_AUDIO_V1),
                QString::fromLatin1(HALLA_INTERFACE_DATA_V1),
                QString::fromLatin1(HALLA_INTERFACE_UI_V1)}}
        };
        return copyBytes(QJsonDocument(info).toJson(QJsonDocument::Compact),
                         buffer, bufferSize);
    }

    static int corePost(void* context, HallaUiTaskFn task, void* taskContext) {
        Record* record = static_cast<Record*>(context);
        if (!record || !task || !qApp) return HALLA_RESULT_INVALID_ARGUMENT;
        const QString pluginId = record->info.id;
        const uint64_t token = record->instanceToken;
        QMetaObject::invokeMethod(qApp, [pluginId, token, task, taskContext] {
            Record* current = PluginManager::instance().record(pluginId);
            if (current && current->instanceToken == token
                    && current->info.loaded && current->info.enabled) {
                try { task(taskContext); }
                catch (...) {
                    AppLog::error(PluginManager::tr(
                        "O complemento %1 lançou uma exceção em uma tarefa da interface.")
                        .arg(current->info.name));
                }
            }
        }, Qt::QueuedConnection);
        return HALLA_RESULT_OK;
    }

    static size_t connectionList(void*, char* buffer, size_t bufferSize) {
        PluginManager& manager = PluginManager::instance();
        QJsonArray list;
        QList<quint64> ids = manager.m_sessions.keys();
        std::sort(ids.begin(), ids.end());
        for (quint64 id : ids) list << manager.connectionState(id);
        const QJsonObject root{{QStringLiteral("activeConnectionId"), qint64(manager.m_activeSessionId)},
                               {QStringLiteral("connections"), list}};
        return copyBytes(QJsonDocument(root).toJson(QJsonDocument::Compact),
                         buffer, bufferSize);
    }

    static size_t connectionOne(void*, uint64_t connectionId,
                                char* buffer, size_t bufferSize) {
        PluginManager& manager = PluginManager::instance();
        const QJsonObject state = manager.connectionState(quint64(connectionId));
        if (state.isEmpty()) return 0;
        return copyBytes(QJsonDocument(state).toJson(QJsonDocument::Compact),
                         buffer, bufferSize);
    }

    static ServerTab* writableSession(Record* record, uint64_t connectionId, int* result) {
        if (!record || !record->hasCapability("connection.control")) {
            if (result) *result = HALLA_RESULT_PERMISSION_DENIED;
            return nullptr;
        }
        if (!onUiThread()) {
            if (result) *result = HALLA_RESULT_WRONG_THREAD;
            return nullptr;
        }
        ServerTab* tab = PluginManager::instance().session(quint64(connectionId));
        if (!tab || !tab->net() || !tab->net()->isConnected()) {
            if (result) *result = HALLA_RESULT_NOT_CONNECTED;
            return nullptr;
        }
        if (result) *result = HALLA_RESULT_OK;
        return tab;
    }

    static int connectionMove(void* context, uint64_t connectionId,
                              int32_t channelId, const char* password) {
        int result = 0;
        ServerTab* tab = writableSession(static_cast<Record*>(context), connectionId, &result);
        if (!tab) return result;
        if (!tab->data().channels.contains(channelId)) return HALLA_RESULT_NOT_FOUND;
        tab->net()->moveToChannel(channelId, QString::fromUtf8(password ? password : ""));
        return HALLA_RESULT_OK;
    }

    static int connectionFlags(void* context, uint64_t connectionId,
                               uint32_t mask, uint32_t values) {
        int result = 0;
        ServerTab* tab = writableSession(static_cast<Record*>(context), connectionId, &result);
        if (!tab) return result;
        if (mask & HALLA_SELF_INPUT_MUTED)
            tab->setMicMuted((values & HALLA_SELF_INPUT_MUTED) != 0);
        if (mask & HALLA_SELF_OUTPUT_MUTED)
            tab->setSpeakersMuted((values & HALLA_SELF_OUTPUT_MUTED) != 0);
        if (mask & HALLA_SELF_AWAY)
            tab->setAway((values & HALLA_SELF_AWAY) != 0);
        return HALLA_RESULT_OK;
    }

    static int connectionNickname(void* context, uint64_t connectionId,
                                  const char* nickname) {
        int result = 0;
        ServerTab* tab = writableSession(static_cast<Record*>(context), connectionId, &result);
        if (!tab) return result;
        const QString value = QString::fromUtf8(nickname ? nickname : "").trimmed();
        if (value.isEmpty() || value.size() > 64) return HALLA_RESULT_INVALID_ARGUMENT;
        tab->net()->rename(value);
        return HALLA_RESULT_OK;
    }

    static int connectionChat(void* context, uint64_t connectionId,
                              HallaChatScope scope, int32_t targetUserId,
                              const char* text) {
        int result = 0;
        ServerTab* tab = writableSession(static_cast<Record*>(context), connectionId, &result);
        if (!tab) return result;
        const QString value = QString::fromUtf8(text ? text : "");
        if (value.isEmpty() || value.size() > 4096) return HALLA_RESULT_INVALID_ARGUMENT;
        QString scopeName = QStringLiteral("channel");
        if (scope == HALLA_CHAT_SERVER) scopeName = QStringLiteral("server");
        else if (scope == HALLA_CHAT_PRIVATE) {
            if (!tab->data().users.contains(targetUserId)) return HALLA_RESULT_NOT_FOUND;
            scopeName = QStringLiteral("private");
        }
        tab->net()->sendChat(scopeName, targetUserId, value);
        return HALLA_RESULT_OK;
    }

    static int connectionWhisper(void* context, uint64_t connectionId,
                                 const int32_t* ids, size_t count) {
        int result = 0;
        ServerTab* tab = writableSession(static_cast<Record*>(context), connectionId, &result);
        if (!tab) return result;
        if (count > 64 || (count && !ids)) return HALLA_RESULT_INVALID_ARGUMENT;
        QList<int> targets;
        for (size_t i = 0; i < count; ++i)
            if (ids[i] > 0 && tab->data().users.contains(ids[i])) targets << ids[i];
        tab->net()->setWhisperIds(targets);
        return HALLA_RESULT_OK;
    }

    static int connectionLocalMute(void* context, uint64_t connectionId,
                                   int32_t userId, int muted) {
        int result = 0;
        ServerTab* tab = writableSession(static_cast<Record*>(context), connectionId, &result);
        if (!tab) return result;
        return tab->setUserLocallyMuted(userId, muted != 0)
            ? HALLA_RESULT_OK : HALLA_RESULT_NOT_FOUND;
    }

    static int connectionLocalVolume(void* context, uint64_t connectionId,
                                     int32_t userId, float volumeDb) {
        int result = 0;
        ServerTab* tab = writableSession(static_cast<Record*>(context), connectionId, &result);
        if (!tab) return result;
        if (!std::isfinite(volumeDb) || volumeDb < -40.0f || volumeDb > 12.0f)
            return HALLA_RESULT_INVALID_ARGUMENT;
        return tab->setUserVolumeDb(userId, int(qRound(volumeDb)))
            ? HALLA_RESULT_OK : HALLA_RESULT_NOT_FOUND;
    }

    static int connectionMoveUser(void* context, uint64_t connectionId,
                                  int32_t userId, int32_t channelId) {
        int result = 0;
        ServerTab* tab = writableSession(static_cast<Record*>(context), connectionId, &result);
        if (!tab) return result;
        if (!tab->data().users.contains(userId) || !tab->data().channels.contains(channelId))
            return HALLA_RESULT_NOT_FOUND;
        tab->net()->moveOther(userId, channelId);
        return HALLA_RESULT_OK;
    }

    static int connectionPoke(void* context, uint64_t connectionId,
                              int32_t userId, const char* message) {
        int result = 0;
        ServerTab* tab = writableSession(static_cast<Record*>(context), connectionId, &result);
        if (!tab) return result;
        const QString text = QString::fromUtf8(message ? message : "");
        if (!tab->data().users.contains(userId)) return HALLA_RESULT_NOT_FOUND;
        if (text.isEmpty() || text.size() > 1024) return HALLA_RESULT_INVALID_ARGUMENT;
        tab->net()->poke(userId, text);
        return HALLA_RESULT_OK;
    }

    static int connectionCommander(void* context, uint64_t connectionId,
                                   int32_t userId, int enabled) {
        int result = 0;
        ServerTab* tab = writableSession(static_cast<Record*>(context), connectionId, &result);
        if (!tab) return result;
        if (!tab->data().users.contains(userId)) return HALLA_RESULT_NOT_FOUND;
        tab->net()->setCommander(userId, enabled != 0);
        return HALLA_RESULT_OK;
    }

    static int connectionKick(void* context, uint64_t connectionId,
                              int32_t userId, int fromServer, const char* reason) {
        int result = 0;
        ServerTab* tab = writableSession(static_cast<Record*>(context), connectionId, &result);
        if (!tab) return result;
        const QString text = QString::fromUtf8(reason ? reason : "");
        if (!tab->data().users.contains(userId)) return HALLA_RESULT_NOT_FOUND;
        if (text.size() > 1024) return HALLA_RESULT_INVALID_ARGUMENT;
        tab->net()->kick(userId, fromServer != 0, text);
        return HALLA_RESULT_OK;
    }

    static int connectionBan(void* context, uint64_t connectionId,
                             int32_t userId, uint32_t minutes, const char* reason) {
        int result = 0;
        ServerTab* tab = writableSession(static_cast<Record*>(context), connectionId, &result);
        if (!tab) return result;
        const QString text = QString::fromUtf8(reason ? reason : "");
        if (!tab->data().users.contains(userId)) return HALLA_RESULT_NOT_FOUND;
        if (text.size() > 1024 || minutes > 525600u) return HALLA_RESULT_INVALID_ARGUMENT;
        tab->net()->ban(userId, text, int(minutes));
        return HALLA_RESULT_OK;
    }

    static bool parseChannelJson(const char* json, QJsonObject* object) {
        if (!json || !object) return false;
        const QByteArray bytes(json);
        if (bytes.isEmpty() || bytes.size() > 64 * 1024) return false;
        QJsonParseError error;
        const QJsonDocument document = QJsonDocument::fromJson(bytes, &error);
        if (error.error != QJsonParseError::NoError || !document.isObject()) return false;
        *object = document.object();
        return true;
    }

    static int connectionCreateChannel(void* context, uint64_t connectionId,
                                       const char* json) {
        int result = 0;
        ServerTab* tab = writableSession(static_cast<Record*>(context), connectionId, &result);
        if (!tab) return result;
        QJsonObject channel;
        if (!parseChannelJson(json, &channel)) return HALLA_RESULT_INVALID_ARGUMENT;
        tab->net()->createChannel(channel);
        return HALLA_RESULT_OK;
    }

    static int connectionEditChannel(void* context, uint64_t connectionId,
                                     const char* json) {
        int result = 0;
        ServerTab* tab = writableSession(static_cast<Record*>(context), connectionId, &result);
        if (!tab) return result;
        QJsonObject channel;
        if (!parseChannelJson(json, &channel) || channel.value("id").toInt() <= 0)
            return HALLA_RESULT_INVALID_ARGUMENT;
        tab->net()->editChannel(channel);
        return HALLA_RESULT_OK;
    }

    static int connectionDeleteChannel(void* context, uint64_t connectionId,
                                       int32_t channelId) {
        int result = 0;
        ServerTab* tab = writableSession(static_cast<Record*>(context), connectionId, &result);
        if (!tab) return result;
        if (!tab->data().channels.contains(channelId)) return HALLA_RESULT_NOT_FOUND;
        tab->net()->deleteChannel(channelId);
        return HALLA_RESULT_OK;
    }

    static int audioRegister(void* context, void* pluginContext,
                             HallaAudioProcessorFn processor, uint32_t stages) {
        Record* record = static_cast<Record*>(context);
        if (!record || !processor || !(stages & (HALLA_AUDIO_CAPTURE |
                HALLA_AUDIO_REMOTE_BEFORE_SPATIAL | HALLA_AUDIO_MIXED_PLAYBACK)))
            return HALLA_RESULT_INVALID_ARGUMENT;
        if ((stages & HALLA_AUDIO_CAPTURE) && !record->hasCapability("audio.capture"))
            return HALLA_RESULT_PERMISSION_DENIED;
        if ((stages & (HALLA_AUDIO_REMOTE_BEFORE_SPATIAL | HALLA_AUDIO_MIXED_PLAYBACK))
                && !record->hasCapability("audio.playback"))
            return HALLA_RESULT_PERMISSION_DENIED;
        record->audioProcessorContext = pluginContext;
        record->audioProcessor = processor;
        record->audioStages = stages;
        return HALLA_RESULT_OK;
    }

    static void audioUnregister(void* context) {
        Record* record = static_cast<Record*>(context);
        if (!record) return;
        record->audioProcessor = nullptr;
        record->audioProcessorContext = nullptr;
        record->audioStages = 0;
    }

    static int requireSpatial(Record* record) {
        if (!record || !record->hasCapability("audio.spatial"))
            return HALLA_RESULT_PERMISSION_DENIED;
        if (!onUiThread()) return HALLA_RESULT_WRONG_THREAD;
        return HALLA_RESULT_OK;
    }

    static bool finiteVec(const HallaVec3& value) {
        return std::isfinite(value.x) && std::isfinite(value.y)
            && std::isfinite(value.z);
    }

    static int audioListener(void* context, uint64_t connectionId,
                             const HallaTransform* transform) {
        Record* record = static_cast<Record*>(context);
        const int allowed = requireSpatial(record);
        if (allowed != HALLA_RESULT_OK) return allowed;
        if (!transform || !finiteVec(transform->position)
                || !finiteVec(transform->forward) || !finiteVec(transform->up)
                || !PluginManager::instance().session(quint64(connectionId)))
            return HALLA_RESULT_INVALID_ARGUMENT;
        auto& state = record->audioConnections[PluginManager::instance().connectionId(
            PluginManager::instance().session(quint64(connectionId)))];
        state.listener = *transform;
        state.hasListener = true;
        return HALLA_RESULT_OK;
    }

    static int audioUserTransform(void* context, uint64_t connectionId,
                                  int32_t userId, const HallaVec3* position,
                                  float minDistance, float maxDistance, float rolloff) {
        Record* record = static_cast<Record*>(context);
        const int allowed = requireSpatial(record);
        if (allowed != HALLA_RESULT_OK) return allowed;
        const quint64 id = connectionId ? quint64(connectionId)
                                        : PluginManager::instance().m_activeSessionId;
        ServerTab* tab = PluginManager::instance().session(id);
        if (!position || !finiteVec(*position) || !tab || !tab->data().users.contains(userId)
                || !std::isfinite(minDistance) || !std::isfinite(maxDistance)
                || !std::isfinite(rolloff) || minDistance < 0.01f
                || maxDistance <= minDistance || maxDistance > 100000.0f
                || rolloff < 0.0f || rolloff > 8.0f)
            return HALLA_RESULT_INVALID_ARGUMENT;
        auto& user = record->audioConnections[id].users[userId];
        user.position = *position;
        user.minDistance = minDistance;
        user.maxDistance = maxDistance;
        user.rolloff = rolloff;
        user.hasPosition = true;
        return HALLA_RESULT_OK;
    }

    static int audioGain(void* context, uint64_t connectionId,
                         int32_t userId, float gain) {
        Record* record = static_cast<Record*>(context);
        const int allowed = requireSpatial(record);
        if (allowed != HALLA_RESULT_OK) return allowed;
        const quint64 id = connectionId ? quint64(connectionId)
                                        : PluginManager::instance().m_activeSessionId;
        ServerTab* tab = PluginManager::instance().session(id);
        if (!tab || !tab->data().users.contains(userId) || !std::isfinite(gain)
                || gain < 0.0f || gain > 4.0f)
            return HALLA_RESULT_INVALID_ARGUMENT;
        record->audioConnections[id].users[userId].gain = gain;
        return HALLA_RESULT_OK;
    }

    static int audioPan(void* context, uint64_t connectionId,
                        int32_t userId, float pan) {
        Record* record = static_cast<Record*>(context);
        const int allowed = requireSpatial(record);
        if (allowed != HALLA_RESULT_OK) return allowed;
        const quint64 id = connectionId ? quint64(connectionId)
                                        : PluginManager::instance().m_activeSessionId;
        ServerTab* tab = PluginManager::instance().session(id);
        if (!tab || !tab->data().users.contains(userId) || !std::isfinite(pan)
                || pan < -1.0f || pan > 1.0f)
            return HALLA_RESULT_INVALID_ARGUMENT;
        record->audioConnections[id].users[userId].pan = pan;
        return HALLA_RESULT_OK;
    }

    static int audioRadio(void* context, uint64_t connectionId,
                          int32_t userId, int enabled, float strength, float noise) {
        Record* record = static_cast<Record*>(context);
        const int allowed = requireSpatial(record);
        if (allowed != HALLA_RESULT_OK) return allowed;
        const quint64 id = connectionId ? quint64(connectionId)
                                        : PluginManager::instance().m_activeSessionId;
        ServerTab* tab = PluginManager::instance().session(id);
        if (!tab || !tab->data().users.contains(userId)
                || !std::isfinite(strength) || !std::isfinite(noise)
                || strength < 0.0f || strength > 1.0f || noise < 0.0f || noise > 1.0f)
            return HALLA_RESULT_INVALID_ARGUMENT;
        auto& user = record->audioConnections[id].users[userId];
        user.radio = enabled != 0;
        user.radioStrength = strength;
        user.radioNoise = noise;
        return HALLA_RESULT_OK;
    }

    static int audioPlayPcm(void* context, uint64_t connectionId,
                            const int16_t* samples, uint32_t frameCount,
                            uint32_t channels, float gain) {
        Record* record = static_cast<Record*>(context);
        if (!record || !record->hasCapability("audio.playback"))
            return HALLA_RESULT_PERMISSION_DENIED;
        if (!onUiThread()) return HALLA_RESULT_WRONG_THREAD;
        const quint64 id = connectionId ? quint64(connectionId)
                                        : PluginManager::instance().m_activeSessionId;
        ServerTab* tab = PluginManager::instance().session(id);
        if (!tab || !tab->voice()) return HALLA_RESULT_NOT_CONNECTED;
        return tab->voice()->playPluginPcm(samples, frameCount, channels, gain)
            ? HALLA_RESULT_OK : HALLA_RESULT_INVALID_ARGUMENT;
    }

    static void audioResetUser(void* context, uint64_t connectionId, int32_t userId) {
        Record* record = static_cast<Record*>(context);
        if (!record) return;
        const quint64 id = connectionId ? quint64(connectionId)
                                        : PluginManager::instance().m_activeSessionId;
        record->audioConnections[id].users.remove(userId);
    }

    static void audioResetConnection(void* context, uint64_t connectionId) {
        Record* record = static_cast<Record*>(context);
        if (!record) return;
        const quint64 id = connectionId ? quint64(connectionId)
                                        : PluginManager::instance().m_activeSessionId;
        record->audioConnections.remove(id);
    }

    static int dataSend(void* context, uint64_t connectionId,
                        HallaPluginDataTarget target, const int32_t* targetIds,
                        size_t targetCount, const char* topic,
                        const uint8_t* data, size_t dataSize) {
        Record* record = static_cast<Record*>(context);
        if (!record || !record->hasCapability("plugin.data"))
            return HALLA_RESULT_PERMISSION_DENIED;
        if (!onUiThread()) return HALLA_RESULT_WRONG_THREAD;
        if ((dataSize && !data) || dataSize > 8192 || targetCount > 64
                || (targetCount && !targetIds)) return HALLA_RESULT_INVALID_ARGUMENT;
        const quint64 id = connectionId ? quint64(connectionId)
                                        : PluginManager::instance().m_activeSessionId;
        ServerTab* tab = PluginManager::instance().session(id);
        if (!tab || !tab->net() || !tab->net()->isConnected())
            return HALLA_RESULT_NOT_CONNECTED;
        QList<int> ids;
        for (size_t i = 0; i < targetCount; ++i) ids << targetIds[i];
        const QString topicValue = QString::fromUtf8(topic ? topic : "");
        if (topicValue.toUtf8().size() > 64) return HALLA_RESULT_INVALID_ARGUMENT;
        const QByteArray payload = dataSize
            ? QByteArray(reinterpret_cast<const char*>(data), int(dataSize)) : QByteArray();
        return tab->net()->sendPluginData(record->info.id, int(target), ids,
            topicValue, payload) ? HALLA_RESULT_OK : HALLA_RESULT_UNAVAILABLE;
    }

    static int dataHandlerSet(void* context, void* pluginContext,
                              HallaPluginDataFn handler) {
        Record* record = static_cast<Record*>(context);
        if (!record || !record->hasCapability("plugin.data"))
            return HALLA_RESULT_PERMISSION_DENIED;
        record->dataHandlerContext = pluginContext;
        record->dataHandler = handler;
        return HALLA_RESULT_OK;
    }

    static int uiNotify(void* context, const char* title, const char* message,
                        uint32_t timeoutMs) {
        Record* record = static_cast<Record*>(context);
        if (!record || !record->hasCapability("ui.notifications"))
            return HALLA_RESULT_PERMISSION_DENIED;
        if (!onUiThread()) return HALLA_RESULT_WRONG_THREAD;
        const QString titleValue = QString::fromUtf8(title ? title : "").left(128);
        const QString messageValue = QString::fromUtf8(message ? message : "").left(2048);
        if (messageValue.isEmpty()) return HALLA_RESULT_INVALID_ARGUMENT;
        emit PluginManager::instance().pluginNotification(titleValue, messageValue,
                                                           int(qBound(1000u, timeoutMs, 30000u)));
        return HALLA_RESULT_OK;
    }

    static bool validActionId(const QString& id) {
        if (id.isEmpty() || id.size() > 64) return false;
        for (QChar c : id)
            if (!c.isLetterOrNumber() && c != QLatin1Char('.') && c != QLatin1Char('-')
                    && c != QLatin1Char('_')) return false;
        return true;
    }

    static int uiRegister(void* context, const char* actionId, const char* label,
                          const char* shortcut, void* pluginContext,
                          HallaUiActionFn callback) {
        Record* record = static_cast<Record*>(context);
        if (!record || !record->hasCapability("ui.actions"))
            return HALLA_RESULT_PERMISSION_DENIED;
        if (!onUiThread()) return HALLA_RESULT_WRONG_THREAD;
        const QString id = QString::fromUtf8(actionId ? actionId : "");
        const QString text = QString::fromUtf8(label ? label : "").trimmed().left(128);
        const QString sequence = QString::fromUtf8(shortcut ? shortcut : "").left(64);
        if (!validActionId(id) || text.isEmpty() || !callback)
            return HALLA_RESULT_INVALID_ARGUMENT;
        record->uiActions[id] = UiAction{text, sequence, pluginContext, callback};
        emit PluginManager::instance().pluginActionRegistered(record->info.id, id, text, sequence);
        return HALLA_RESULT_OK;
    }

    static int uiMenu(void* context, const char* actionId, const char* label,
                      void* pluginContext, HallaUiActionFn callback) {
        return uiRegister(context, actionId, label, "", pluginContext, callback);
    }

    static int uiHotkey(void* context, const char* actionId, const char* label,
                        const char* shortcut, void* pluginContext,
                        HallaUiActionFn callback) {
        const QString sequence = QString::fromUtf8(shortcut ? shortcut : "");
        if (!sequence.isEmpty() && QKeySequence(sequence).isEmpty())
            return HALLA_RESULT_INVALID_ARGUMENT;
        return uiRegister(context, actionId, label, shortcut, pluginContext, callback);
    }

    static void uiUnregister(void* context, const char* actionId) {
        Record* record = static_cast<Record*>(context);
        if (!record) return;
        const QString id = QString::fromUtf8(actionId ? actionId : "");
        if (record->uiActions.remove(id) > 0)
            emit PluginManager::instance().pluginActionRemoved(record->info.id, id);
    }

    static const void* hostQuery(void* context, const char* interfaceId,
                                 uint32_t minimumVersion) {
        Record* record = static_cast<Record*>(context);
        if (!record || !interfaceId || minimumVersion > 1) return nullptr;
        const QByteArray id(interfaceId);
        if (id == HALLA_INTERFACE_CORE_V1) return &record->core;
        if (id == HALLA_INTERFACE_CONNECTION_V1
                && (record->hasCapability("connection.read")
                    || record->hasCapability("connection.control"))) return &record->connection;
        if (id == HALLA_INTERFACE_AUDIO_V1
                && (record->hasCapability("audio.capture")
                    || record->hasCapability("audio.playback")
                    || record->hasCapability("audio.spatial"))) return &record->audio;
        if (id == HALLA_INTERFACE_DATA_V1 && record->hasCapability("plugin.data"))
            return &record->data;
        if (id == HALLA_INTERFACE_UI_V1
                && (record->hasCapability("ui.notifications")
                    || record->hasCapability("ui.actions"))) return &record->ui;
        return nullptr;
    }

    void initializeInterfaces() {
        core = {1u, uint32_t(sizeof(core)), this, &coreTime, &coreAppInfo, &corePost};
        connection = {1u, uint32_t(sizeof(connection)), this,
            &connectionList, &connectionOne, &connectionMove, &connectionFlags,
            &connectionNickname, &connectionChat, &connectionWhisper,
            &connectionLocalMute, &connectionLocalVolume, &connectionMoveUser,
            &connectionPoke, &connectionCommander, &connectionKick, &connectionBan,
            &connectionCreateChannel, &connectionEditChannel,
            &connectionDeleteChannel};
        audio = {1u, uint32_t(sizeof(audio)), this, &audioRegister, &audioUnregister,
            &audioListener, &audioUserTransform, &audioGain, &audioPan, &audioRadio,
            &audioResetUser, &audioResetConnection, &audioPlayPcm};
        data = {1u, uint32_t(sizeof(data)), this, &dataSend, &dataHandlerSet};
        ui = {1u, uint32_t(sizeof(ui)), this, &uiNotify, &uiMenu, &uiHotkey,
              &uiUnregister};
    }
};

static QJsonArray officialRadioModeOptions() {
    return QJsonArray{
        QJsonObject{{"value","disabled"},{"label",PluginManager::tr("Não aplicar")}},
        QJsonObject{{"value","whisper"},{"label",PluginManager::tr("Somente sussurros")}},
        QJsonObject{{"value","normal"},{"label",PluginManager::tr("Somente voz normal")}},
        QJsonObject{{"value","both"},{"label",PluginManager::tr("Sussurros e voz normal")}}
    };
}

static QJsonArray officialRadioVoiceSchema() {
    return QJsonArray{
        QJsonObject{{"key","sendMode"},{"type","choice"},
                    {"label",PluginManager::tr("Aplicar ao enviar minha voz")},
                    {"default","whisper"},{"options",officialRadioModeOptions()}},
        QJsonObject{{"key","receiveMode"},{"type","choice"},
                    {"label",PluginManager::tr("Aplicar às vozes que escuto")},
                    {"default","whisper"},{"options",officialRadioModeOptions()}},
        QJsonObject{{"key","intensity"},{"type","int"},
                    {"label",PluginManager::tr("Intensidade do efeito (%)")},
                    {"default",90},{"min",0},{"max",100}},
        QJsonObject{{"key","noise"},{"type","int"},
                    {"label",PluginManager::tr("Chiado do rádio (%)")},
                    {"default",10},{"min",0},{"max",100}},
        QJsonObject{{"key","gain"},{"type","int"},
                    {"label",PluginManager::tr("Volume após o efeito (%)")},
                    {"default",105},{"min",50},{"max",150}}
    };
}

static QJsonArray officialOverlaySchema() {
    return QJsonArray{
        QJsonObject{{"key","onlyTalking"},{"type","bool"},
                    {"label",PluginManager::tr("Mostrar somente usuários falando")},{"default",true}},
        QJsonObject{{"key","showSelf"},{"type","bool"},
                    {"label",PluginManager::tr("Mostrar meu próprio usuário")},{"default",false}},
        QJsonObject{{"key","showChannel"},{"type","bool"},
                    {"label",PluginManager::tr("Mostrar nome do canal")},{"default",true}},
        QJsonObject{{"key","gameOnly"},{"type","bool"},
                    {"label",PluginManager::tr("Aparecer automaticamente sobre jogos/tela cheia")},{"default",true}},
        QJsonObject{{"key","position"},{"type","choice"},{"label",PluginManager::tr("Posição")},
                    {"default","top_right"},
                    {"options",QJsonArray{
                        QJsonObject{{"value","top_left"},{"label",PluginManager::tr("Superior esquerdo")}},
                        QJsonObject{{"value","top_right"},{"label",PluginManager::tr("Superior direito")}},
                        QJsonObject{{"value","bottom_left"},{"label",PluginManager::tr("Inferior esquerdo")}},
                        QJsonObject{{"value","bottom_right"},{"label",PluginManager::tr("Inferior direito")}}
                    }}},
        QJsonObject{{"key","opacity"},{"type","int"},{"label",PluginManager::tr("Opacidade (%)")},
                    {"default",88},{"min",25},{"max",100}},
        QJsonObject{{"key","scale"},{"type","int"},{"label",PluginManager::tr("Escala (%)")},
                    {"default",100},{"min",75},{"max",160}},
        QJsonObject{{"key","maxUsers"},{"type","int"},{"label",PluginManager::tr("Máximo de usuários")},
                    {"default",8},{"min",1},{"max",24}},
        QJsonObject{{"key","margin"},{"type","int"},{"label",PluginManager::tr("Margem da tela")},
                    {"default",24},{"min",0},{"max",200}}
    };
}

PluginManager& PluginManager::instance() {
    static PluginManager* manager = new PluginManager(qApp);
    return *manager;
}

PluginManager::PluginManager(QObject* parent) : QObject(parent) {}

PluginManager::~PluginManager() {
    shutdown();
    qDeleteAll(m_records);
    m_records.clear();
}

QString PluginManager::addonsRoot() {
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
        + QStringLiteral("/addons");
}

QString PluginManager::catalogUrl() {
    return QStringLiteral("https://raw.githubusercontent.com/GroupHalla/Halla/main/addons/catalog.json");
}

QString PluginManager::platformKey() {
#ifdef Q_OS_WIN
    return QStringLiteral("windows-x64");
#elif defined(Q_PROCESSOR_X86_64)
    return QStringLiteral("linux-x64");
#elif defined(Q_PROCESSOR_ARM_64)
    return QStringLiteral("linux-arm64");
#else
    return QStringLiteral("unsupported");
#endif
}

PluginManager::Record* PluginManager::record(const QString& id) const {
    return m_records.value(id, nullptr);
}

void PluginManager::initialize() {
    if (m_initialized) return;
    m_initialized = true;
    QDir().mkpath(addonsRoot());
    addOfficialOverlay();
    addOfficialRadioVoice();
    scanInstalled();
    for (Record* item : m_records) {
        if (item->info.enabled) {
            QString error;
            if (!load(item, &error)) {
                item->info.error = error;
                AppLog::warn(QStringLiteral("Não foi possível carregar %1: %2")
                             .arg(item->info.name, error));
            }
        }
    }
}

void PluginManager::shutdown() {
    if (!m_initialized || m_shuttingDown) return;
    m_shuttingDown = true;
    dispatchEvent(QJsonObject{{"event","application_shutdown"}});
    for (Record* item : m_records) unload(item);
    if (m_overlay) {
        delete m_overlay;
        m_overlay = nullptr;
    }
    if (m_radioVoice) {
        delete m_radioVoice;
        m_radioVoice = nullptr;
    }
    m_shuttingDown = false;
}

void PluginManager::addOfficialOverlay() {
    auto* item = new Record;
    item->info.id = QStringLiteral("official.talking-overlay");
    item->info.name = tr("Overlay oficial da call");
    item->info.version = QStringLiteral("1.0.0");
    item->info.author = QStringLiteral("Halla-DEV");
    item->info.description = tr("Mostra no jogo os usuários da call e destaca quem está falando ou sussurrando.");
    item->info.official = true;
    item->info.builtIn = true;
    item->info.configurable = true;
    item->info.settingsSchema = officialOverlaySchema();
    item->info.enabled = S::flag(QStringLiteral("addons/%1/enabled").arg(item->info.id), false);
    m_records.insert(item->info.id, item);
}

void PluginManager::addOfficialRadioVoice() {
    auto* item = new Record;
    item->info.id = QStringLiteral("official.radio-voice");
    item->info.name = tr("Voz de rádio policial");
    item->info.version = QStringLiteral("1.0.0");
    item->info.author = QStringLiteral("Halla-DEV");
    item->info.description = tr(
        "Simula um comunicador policial no microfone e nas vozes recebidas, "
        "com regras separadas para fala normal e sussurros.");
    item->info.official = true;
    item->info.builtIn = true;
    item->info.configurable = true;
    item->info.settingsSchema = officialRadioVoiceSchema();
    item->info.enabled = S::flag(QStringLiteral("addons/%1/enabled").arg(item->info.id), false);
    m_records.insert(item->info.id, item);
}

bool PluginManager::readManifest(const QString& directory, QJsonObject* manifest,
                                 QString* error) const {
    QFile file(QDir(directory).filePath(QStringLiteral("manifest.json")));
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = tr("manifest.json não encontrado.");
        return false;
    }
    if (file.size() > 256 * 1024) {
        if (error) *error = tr("O manifesto excede o limite de 256 KiB.");
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (error) *error = tr("Manifesto JSON inválido: %1").arg(parseError.errorString());
        return false;
    }
    const QJsonObject object = document.object();
    const QString id = object.value("id").toString();
    static const QRegularExpression validId(QStringLiteral("^[a-z0-9][a-z0-9._-]{2,63}$"));
    if (!validId.match(id).hasMatch()
            || object.value("name").toString().trimmed().isEmpty()
            || object.value("version").toString().trimmed().isEmpty()
            || object.value("apiVersion").toInt() != int(HALLA_PLUGIN_ABI_VERSION)
            || object.value("type").toString() != QLatin1String("native")) {
        if (error) *error = tr("O manifesto não contém metadados válidos para a API de plugins do Halla.");
        return false;
    }
    QJsonObject platform = object.value("platforms").toObject().value(platformKey()).toObject();
    QString library = platform.value("library").toString();
    if (library.isEmpty()) library = object.value("library").toString();
    const QString cleaned = QDir::cleanPath(library);
    if (cleaned.isEmpty() || QDir::isAbsolutePath(cleaned)
            || cleaned == QLatin1String("..") || cleaned.startsWith(QStringLiteral("../"))) {
        if (error) *error = tr("O caminho da biblioteca no manifesto é inválido para esta plataforma.");
        return false;
    }
    if (!QFileInfo(QDir(directory).filePath(cleaned)).isFile()) {
        if (error) *error = tr("A biblioteca declarada para %1 não existe.").arg(platformKey());
        return false;
    }
    static const QSet<QString> supportedCapabilities{
        QStringLiteral("connection.read"), QStringLiteral("connection.control"),
        QStringLiteral("audio.capture"), QStringLiteral("audio.playback"),
        QStringLiteral("audio.spatial"), QStringLiteral("plugin.data"),
        QStringLiteral("ui.notifications"), QStringLiteral("ui.actions")
    };
    const QJsonValue capabilitiesValue = object.value("capabilities");
    if (!capabilitiesValue.isUndefined() && !capabilitiesValue.isArray()) {
        if (error) *error = tr("A lista de capacidades do complemento é inválida.");
        return false;
    }
    for (const QJsonValue& value : capabilitiesValue.toArray()) {
        if (!value.isString() || !supportedCapabilities.contains(value.toString())) {
            if (error) *error = tr("O complemento solicita uma capacidade desconhecida: %1")
                .arg(value.toVariant().toString());
            return false;
        }
    }
    if (manifest) {
        *manifest = object;
        (*manifest)["resolvedLibrary"] = cleaned;
    }
    return true;
}

void PluginManager::scanInstalled() {
    QDir root(addonsRoot());
    const QFileInfoList directories = root.entryInfoList(
        QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QFileInfo& directory : directories) {
        QJsonObject manifest;
        QString error;
        if (!readManifest(directory.absoluteFilePath(), &manifest, &error)) {
            AppLog::warn(QStringLiteral("Complemento ignorado em %1: %2")
                         .arg(directory.fileName(), error));
            continue;
        }
        const QString id = manifest.value("id").toString();
        if (m_records.contains(id)) continue;
        auto* item = new Record;
        item->manifest = manifest;
        item->info.id = id;
        item->info.name = manifest.value("name").toString();
        item->info.version = manifest.value("version").toString();
        item->info.author = manifest.value("author").toString(tr("Autor desconhecido"));
        item->info.description = manifest.value("description").toString();
        item->info.installPath = directory.absoluteFilePath();
        item->info.official = manifest.value("official").toBool(false);
        for (const QJsonValue& capability : manifest.value("capabilities").toArray()) {
            const QString name = capability.toString();
            if (!name.isEmpty() && !item->info.capabilities.contains(name))
                item->info.capabilities << name;
        }
        item->info.settingsSchema = manifest.value("settings").toArray();
        item->info.configurable = !item->info.settingsSchema.isEmpty();
        item->info.enabled = S::flag(QStringLiteral("addons/%1/enabled").arg(id),
                                    manifest.value("defaultEnabled").toBool(false));
        m_records.insert(id, item);
    }
}

QList<AddonInfo> PluginManager::addons() const {
    QList<AddonInfo> result;
    for (Record* item : m_records) result << item->info;
    std::sort(result.begin(), result.end(), [](const AddonInfo& a, const AddonInfo& b) {
        if (a.official != b.official) return a.official;
        return a.name.localeAwareCompare(b.name) < 0;
    });
    return result;
}

QJsonObject PluginManager::settingsFor(const QString& id, const QJsonArray& schema) const {
    QJsonObject settings;
    const QJsonDocument stored = QJsonDocument::fromJson(
        S::str(QStringLiteral("addons/%1/settings").arg(id)).toUtf8());
    if (stored.isObject()) settings = stored.object();
    for (const QJsonValue& value : schema) {
        const QJsonObject field = value.toObject();
        const QString key = field.value("key").toString();
        if (!key.isEmpty() && !settings.contains(key)) settings[key] = field.value("default");
    }
    return settings;
}

void PluginManager::saveSettings(const QString& id, const QJsonObject& settings) {
    S::set(QStringLiteral("addons/%1/settings").arg(id),
           QString::fromUtf8(QJsonDocument(settings).toJson(QJsonDocument::Compact)));
}

bool PluginManager::load(Record* item, QString* error) {
    if (!item) return false;
    if (item->info.loaded) return true;
    item->info.error.clear();

    const QJsonObject settings = settingsFor(item->info.id, item->info.settingsSchema);
    if (item->info.builtIn) {
        if (item->info.id == QLatin1String("official.talking-overlay")) {
            if (!m_overlay) m_overlay = new TalkingOverlay;
            m_overlay->applySettings(settings);
            m_overlay->updateClientState(m_currentState);
        } else if (item->info.id == QLatin1String("official.radio-voice")) {
            if (!m_radioVoice) m_radioVoice = new RadioVoiceEffect;
            m_radioVoice->applySettings(settings);
        } else {
            if (error) *error = tr("Complemento oficial desconhecido.");
            return false;
        }
        item->info.loaded = true;
        return true;
    }

    const QString relative = item->manifest.value("resolvedLibrary").toString();
    const QString libraryPath = QDir(item->info.installPath).filePath(relative);
    item->library = new QLibrary(libraryPath);
    item->library->setLoadHints(QLibrary::ResolveAllSymbolsHint);
    if (!item->library->load()) {
        const QString reason = item->library->errorString();
        delete item->library;
        item->library = nullptr;
        item->info.error = reason;
        if (error) *error = reason;
        return false;
    }
    auto entry = reinterpret_cast<HallaPluginEntryFn>(
        item->library->resolve(HALLA_PLUGIN_ENTRY_SYMBOL));
    if (!entry) {
        const QString reason = tr("A DLL não exporta halla_plugin_entry.");
        if (error) *error = reason;
        item->info.error = reason;
        unload(item);
        return false;
    }
    try { item->api = entry(); }
    catch (...) { item->api = nullptr; }
    if (!item->api || item->api->abi_version != HALLA_PLUGIN_ABI_VERSION
            || item->api->struct_size < HALLA_PLUGIN_API_BASE_SIZE
            || !item->api->id || QString::fromUtf8(item->api->id) != item->info.id
            || !item->api->initialize) {
        const QString reason = tr("A DLL usa uma ABI incompatível ou não corresponde ao manifesto.");
        if (error) *error = reason;
        item->info.error = reason;
        unload(item);
        return false;
    }

    item->settingsCache = QJsonDocument(settings).toJson(QJsonDocument::Compact);
    item->host.abi_version = HALLA_PLUGIN_ABI_VERSION;
    item->host.struct_size = sizeof(HallaHostApi);
    item->host.context = item;
    item->host.log = &Record::hostLog;
    item->host.get_settings_json = &Record::hostSettings;
    item->host.request_client_state = &Record::hostRequestState;
    item->host.query_interface = &Record::hostQuery;
    item->initializeInterfaces();
    bool initialized = false;
    try { initialized = item->api->initialize(&item->host) != 0; }
    catch (...) { initialized = false; }
    if (!initialized) {
        const QString reason = tr("O plugin recusou a inicialização.");
        if (error) *error = reason;
        item->info.error = reason;
        unload(item);
        return false;
    }
    item->info.loaded = true;
    if (!m_currentState.isEmpty()) {
        const QJsonObject event{{"event","client_state"},{"payload",m_currentState}};
        const QByteArray bytes = QJsonDocument(event).toJson(QJsonDocument::Compact);
        if (item->api->on_event) {
            try { item->api->on_event(bytes.constData(), size_t(bytes.size())); }
            catch (...) {
                AppLog::error(tr("O complemento %1 lançou uma exceção ao receber o estado inicial.")
                              .arg(item->info.name));
            }
        }
    }
    AppLog::info(tr("Complemento carregado: %1 %2").arg(item->info.name, item->info.version));
    return true;
}

void PluginManager::unload(Record* item) {
    if (!item) return;
    if (item->info.builtIn) {
        if (item->info.id == QLatin1String("official.talking-overlay") && m_overlay) {
            m_overlay->hide();
            delete m_overlay;
            m_overlay = nullptr;
        } else if (item->info.id == QLatin1String("official.radio-voice") && m_radioVoice) {
            delete m_radioVoice;
            m_radioVoice = nullptr;
        }
        item->info.loaded = false;
        return;
    }
    ++item->instanceToken; // invalida tarefas post_to_ui ainda enfileiradas
    if (item->api && item->info.loaded && item->api->shutdown) {
        try { item->api->shutdown(); } catch (...) {}
    }
    item->audioProcessor = nullptr;
    item->audioProcessorContext = nullptr;
    item->audioStages = 0;
    item->dataHandler = nullptr;
    item->dataHandlerContext = nullptr;
    item->audioConnections.clear();
    const QStringList actionIds = item->uiActions.keys();
    item->uiActions.clear();
    for (const QString& actionId : actionIds)
        emit pluginActionRemoved(item->info.id, actionId);
    item->info.loaded = false;
    item->api = nullptr;
    if (item->library) {
        if (!item->library->unload())
            AppLog::warn(tr("A DLL de %1 não pôde ser descarregada com segurança.").arg(item->info.name));
        delete item->library;
        item->library = nullptr;
    }
}

bool PluginManager::setEnabled(const QString& id, bool enabled, QString* error) {
    Record* item = record(id);
    if (!item) {
        if (error) *error = tr("Complemento não encontrado.");
        return false;
    }
    if (enabled && !load(item, error)) {
        item->info.enabled = false;
        S::set(QStringLiteral("addons/%1/enabled").arg(id), false);
        emit addonsChanged();
        return false;
    }
    if (!enabled) unload(item);
    item->info.enabled = enabled;
    item->info.error.clear();
    S::set(QStringLiteral("addons/%1/enabled").arg(id), enabled);
    emit addonsChanged();
    return true;
}

void PluginManager::notifySettingsChanged(Record* item) {
    if (!item) return;
    const QJsonObject settings = settingsFor(item->info.id, item->info.settingsSchema);
    if (item->info.builtIn) {
        if (item->info.id == QLatin1String("official.talking-overlay") && m_overlay)
            m_overlay->applySettings(settings);
        else if (item->info.id == QLatin1String("official.radio-voice") && m_radioVoice)
            m_radioVoice->applySettings(settings);
        return;
    }
    item->settingsCache = QJsonDocument(settings).toJson(QJsonDocument::Compact);
    if (item->info.loaded && item->api && item->api->on_settings_changed) {
        try {
            item->api->on_settings_changed(item->settingsCache.constData(),
                                           size_t(item->settingsCache.size()));
        } catch (...) {
            AppLog::error(tr("O complemento %1 lançou uma exceção ao atualizar configurações.")
                          .arg(item->info.name));
        }
    }
}

bool PluginManager::configureAddon(const QString& id, QWidget* parent, QString* error) {
    Record* item = record(id);
    if (!item || item->info.settingsSchema.isEmpty()) {
        if (error) *error = tr("Este complemento não possui opções configuráveis.");
        return false;
    }
    QDialog dialog(parent);
    dialog.setWindowTitle(tr("Configurar %1").arg(item->info.name));
    dialog.setMinimumWidth(470);
    auto* root = new QVBoxLayout(&dialog);
    auto* form = new QFormLayout;
    root->addLayout(form);
    const QJsonObject current = settingsFor(id, item->info.settingsSchema);
    struct Editor { QString key; QString type; QWidget* widget; };
    QList<Editor> editors;
    for (const QJsonValue& value : item->info.settingsSchema) {
        const QJsonObject field = value.toObject();
        const QString key = field.value("key").toString();
        const QString type = field.value("type").toString();
        const QString label = field.value("label").toString(key);
        QWidget* editor = nullptr;
        if (type == QLatin1String("bool")) {
            auto* box = new QCheckBox(&dialog);
            box->setChecked(current.value(key).toBool(field.value("default").toBool()));
            editor = box;
        } else if (type == QLatin1String("int")) {
            auto* spin = new QSpinBox(&dialog);
            spin->setRange(field.value("min").toInt(-100000), field.value("max").toInt(100000));
            spin->setValue(current.value(key).toInt(field.value("default").toInt()));
            editor = spin;
        } else if (type == QLatin1String("choice")) {
            auto* combo = new QComboBox(&dialog);
            for (const QJsonValue& optionValue : field.value("options").toArray()) {
                const QJsonObject option = optionValue.toObject();
                combo->addItem(option.value("label").toString(), option.value("value").toVariant());
            }
            const int index = combo->findData(current.value(key).toVariant());
            if (index >= 0) combo->setCurrentIndex(index);
            editor = combo;
        } else {
            auto* line = new QLineEdit(current.value(key).toString(), &dialog);
            editor = line;
        }
        form->addRow(label + QLatin1Char(':'), editor);
        editors << Editor{key, type, editor};
    }
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Save)->setText(tr("Salvar"));
    buttons->button(QDialogButtonBox::Cancel)->setText(tr("Cancelar"));
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    root->addWidget(buttons);
    if (dialog.exec() != QDialog::Accepted) return false;

    QJsonObject updated = current;
    for (const Editor& editor : editors) {
        if (editor.type == QLatin1String("bool"))
            updated[editor.key] = qobject_cast<QCheckBox*>(editor.widget)->isChecked();
        else if (editor.type == QLatin1String("int"))
            updated[editor.key] = qobject_cast<QSpinBox*>(editor.widget)->value();
        else if (editor.type == QLatin1String("choice"))
            updated[editor.key] = QJsonValue::fromVariant(qobject_cast<QComboBox*>(editor.widget)->currentData());
        else
            updated[editor.key] = qobject_cast<QLineEdit*>(editor.widget)->text();
    }
    saveSettings(id, updated);
    notifySettingsChanged(item);
    emit addonsChanged();
    return true;
}

bool PluginManager::copyTree(const QString& source, const QString& destination,
                             QString* error) {
    if (!QDir().mkpath(destination)) {
        if (error) *error = QObject::tr("Não foi possível criar a pasta de destino.");
        return false;
    }
    QDirIterator iterator(source, QDir::AllEntries | QDir::NoDotAndDotDot,
                          QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        iterator.next();
        const QFileInfo info = iterator.fileInfo();
        if (info.isSymLink()) {
            if (error) *error = QObject::tr("O pacote contém links simbólicos não permitidos.");
            return false;
        }
        const QString relative = QDir(source).relativeFilePath(info.absoluteFilePath());
        const QString target = QDir(destination).filePath(relative);
        if (info.isDir()) {
            if (!QDir().mkpath(target)) return false;
        } else if (info.isFile()) {
            QDir().mkpath(QFileInfo(target).absolutePath());
            QFile::remove(target);
            if (!QFile::copy(info.absoluteFilePath(), target)) {
                if (error) *error = QObject::tr("Não foi possível copiar %1.").arg(relative);
                return false;
            }
        }
    }
    return true;
}

bool PluginManager::installPackage(const QString& packagePath, QWidget* parent,
                                   QString* installedId, QString* error) {
    QFile package(packagePath);
    if (!package.open(QIODevice::ReadOnly)) {
        if (error) *error = tr("Não foi possível abrir o pacote.");
        return false;
    }
    if (package.size() > 100ll * 1024 * 1024) {
        if (error) *error = tr("O pacote excede o limite de 100 MiB.");
        return false;
    }
    const QString sha256 = QString::fromLatin1(
        QCryptographicHash::hash(package.readAll(), QCryptographicHash::Sha256).toHex());
    package.close();
    if (parent && QMessageBox::warning(
            parent, tr("Analisar pacote nativo"),
            tr("Este arquivo pode conter DLLs e outros conteúdos nativos. A extração só deve continuar se você confia na origem.\n\n"
               "Arquivo: %1\nSHA-256: %2\n\nDeseja analisar o pacote?")
                .arg(QFileInfo(packagePath).fileName(), sha256),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) {
        return false;
    }

    QTemporaryDir temporary;
    if (!temporary.isValid()) {
        if (error) *error = tr("Não foi possível criar uma pasta temporária.");
        return false;
    }
    const QString zipPath = temporary.filePath(QStringLiteral("package.zip"));
    if (!QFile::copy(packagePath, zipPath)) {
        if (error) *error = tr("Não foi possível preparar o pacote para extração.");
        return false;
    }
    const QString output = temporary.filePath(QStringLiteral("content"));
    QDir().mkpath(output);

    QProcess list;
#ifdef Q_OS_WIN
    list.start(QStringLiteral("tar.exe"), {QStringLiteral("-tf"), zipPath});
#else
    list.start(QStringLiteral("unzip"), {QStringLiteral("-Z1"), zipPath});
#endif
    if (!list.waitForFinished(30000) || list.exitCode() != 0) {
        if (error) *error = tr("O arquivo não é um pacote .halla-addon ZIP válido.");
        return false;
    }
    const QStringList entries = QString::fromUtf8(list.readAllStandardOutput()).split('\n', Qt::SkipEmptyParts);
    if (entries.size() > 2000) {
        if (error) *error = tr("O pacote contém arquivos demais.");
        return false;
    }
    for (QString entry : entries) {
        entry = entry.trimmed();
        entry.replace('\\', '/');
        const QString cleaned = QDir::cleanPath(entry);
        if (entry.startsWith('/') || entry.contains(':') || cleaned == QLatin1String("..")
                || cleaned.startsWith(QStringLiteral("../"))) {
            if (error) *error = tr("O pacote contém caminhos inseguros.");
            return false;
        }
    }

    QProcess extract;
#ifdef Q_OS_WIN
    extract.start(QStringLiteral("tar.exe"), {QStringLiteral("-xf"), zipPath,
                                               QStringLiteral("-C"), output});
#else
    extract.start(QStringLiteral("unzip"), {QStringLiteral("-qq"), zipPath,
                                             QStringLiteral("-d"), output});
#endif
    if (!extract.waitForFinished(60000) || extract.exitCode() != 0) {
        if (error) *error = tr("Falha ao extrair o pacote.");
        return false;
    }
    qint64 extractedBytes = 0;
    int extractedFiles = 0;
    QDirIterator extracted(output, QDir::Files | QDir::NoDotAndDotDot,
                           QDirIterator::Subdirectories);
    while (extracted.hasNext()) {
        extracted.next();
        extractedBytes += extracted.fileInfo().size();
        if (++extractedFiles > 2000 || extractedBytes > 250ll * 1024 * 1024) {
            if (error) *error = tr("O conteúdo extraído excede os limites de segurança.");
            return false;
        }
    }

    QString root = output;
    if (!QFileInfo(QDir(root).filePath("manifest.json")).isFile()) {
        const QFileInfoList dirs = QDir(root).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
        if (dirs.size() == 1 && QFileInfo(QDir(dirs.first().absoluteFilePath()).filePath("manifest.json")).isFile())
            root = dirs.first().absoluteFilePath();
    }
    QJsonObject manifest;
    QString validationError;
    if (!readManifest(root, &manifest, &validationError)) {
        if (error) *error = validationError;
        return false;
    }
    const QString id = manifest.value("id").toString();
    const QString name = manifest.value("name").toString();
    const QString author = manifest.value("author").toString(tr("Autor desconhecido"));
    QStringList requested;
    static const QHash<QString, QString> capabilityLabels{
        {QStringLiteral("connection.read"), tr("Ler conexões, usuários e canais")},
        {QStringLiteral("connection.control"), tr("Controlar sua conexão, estado e mensagens")},
        {QStringLiteral("audio.capture"), tr("Ler e modificar o áudio do microfone")},
        {QStringLiteral("audio.playback"), tr("Ler e modificar vozes recebidas")},
        {QStringLiteral("audio.spatial"), tr("Controlar volume, posição 3D e filtros por usuário")},
        {QStringLiteral("plugin.data"), tr("Trocar dados do complemento pelo servidor")},
        {QStringLiteral("ui.notifications"), tr("Mostrar notificações")},
        {QStringLiteral("ui.actions"), tr("Adicionar ações e atalhos à interface")}
    };
    for (const QJsonValue& value : manifest.value("capabilities").toArray())
        requested << QStringLiteral("• %1").arg(capabilityLabels.value(value.toString(), value.toString()));
    const QString capabilityText = requested.isEmpty()
        ? tr("Nenhuma capacidade avançada declarada.") : requested.join(QLatin1Char('\n'));
    const QString warning = tr(
        "Complementos nativos executam código no mesmo processo do Halla. Instale somente arquivos de autores confiáveis.\n\n"
        "Complemento: %1\nAutor: %2\nSHA-256: %3\n\nCapacidades solicitadas:\n%4\n\nDeseja instalar?")
        .arg(name, author, sha256, capabilityText);
    if (parent && QMessageBox::warning(parent, tr("Instalar complemento nativo"), warning,
                                       QMessageBox::Yes | QMessageBox::No,
                                       QMessageBox::No) != QMessageBox::Yes) {
        return false;
    }

    if (Record* existing = record(id)) {
        if (existing->info.builtIn) {
            if (error) *error = tr("O ID pertence a um complemento interno do Halla.");
            return false;
        }
        unload(existing);
        m_records.remove(id);
        delete existing;
    }
    const QString destination = QDir(addonsRoot()).filePath(id);
    const QString backup = destination + QStringLiteral(".backup");
    QDir(backup).removeRecursively();
    if (QFileInfo::exists(destination) && !QDir().rename(destination, backup)) {
        if (error) *error = tr("Não foi possível substituir a versão instalada.");
        return false;
    }
    if (!copyTree(root, destination, error)) {
        QDir(destination).removeRecursively();
        if (QFileInfo::exists(backup)) QDir().rename(backup, destination);
        return false;
    }
    QDir(backup).removeRecursively();
    scanInstalled();
    if (Record* installed = record(id); installed && installed->info.enabled) {
        QString loadError;
        if (!load(installed, &loadError)) installed->info.error = loadError;
    }
    if (installedId) *installedId = id;
    emit addonsChanged();
    AppLog::info(tr("Complemento instalado: %1 (%2)").arg(name, sha256));
    return true;
}

bool PluginManager::removeAddon(const QString& id, QString* error) {
    Record* item = record(id);
    if (!item || item->info.builtIn) {
        if (error) *error = tr("Este complemento não pode ser removido.");
        return false;
    }
    unload(item);
    if (!QDir(item->info.installPath).removeRecursively()) {
        if (error) *error = tr("Não foi possível remover os arquivos do complemento.");
        return false;
    }
    m_records.remove(id);
    delete item;
    S::set(QStringLiteral("addons/%1/enabled").arg(id), false);
    emit addonsChanged();
    return true;
}

void PluginManager::openAddonsFolder() {
    QDir().mkpath(addonsRoot());
    QDesktopServices::openUrl(QUrl::fromLocalFile(addonsRoot()));
}

void PluginManager::showCatalog(QWidget* parent) {
    auto* dialog = new QDialog(parent);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(tr("Catálogo de complementos do Halla"));
    dialog->resize(760, 470);
    auto* root = new QVBoxLayout(dialog);
    auto* status = new QLabel(tr("Carregando catálogo seguro por HTTPS..."), dialog);
    status->setWordWrap(true);
    root->addWidget(status);
    auto* table = new QTableWidget(0, 4, dialog);
    table->setHorizontalHeaderLabels({tr("Nome"), tr("Versão"), tr("Autor"), tr("Descrição")});
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    root->addWidget(table, 1);
    auto* row = new QHBoxLayout;
    auto* install = new QPushButton(tr("Instalar selecionado"), dialog);
    install->setEnabled(false);
    auto* close = new QPushButton(tr("Fechar"), dialog);
    row->addStretch(1); row->addWidget(install); row->addWidget(close);
    root->addLayout(row);
    connect(close, &QPushButton::clicked, dialog, &QDialog::close);

    auto entries = std::make_shared<QJsonArray>();
    auto* network = new QNetworkAccessManager(dialog);
    QNetworkRequest request{QUrl(catalogUrl())};
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply* reply = network->get(request);
    connect(reply, &QNetworkReply::downloadProgress, dialog,
            [reply](qint64 received, qint64 total) {
        if (received > 1024 * 1024 || total > 1024 * 1024) reply->abort();
    });
    connect(reply, &QNetworkReply::finished, dialog, [=] {
        const QByteArray bytes = reply->readAll();
        const QString networkError = reply->errorString();
        const bool ok = reply->error() == QNetworkReply::NoError;
        reply->deleteLater();
        if (!ok) {
            status->setText(tr("Não foi possível carregar o catálogo: %1\nVocê ainda pode instalar arquivos .halla-addon.")
                            .arg(networkError));
            return;
        }
        const QJsonDocument document = QJsonDocument::fromJson(bytes);
        if (!document.isObject() || document.object().value("version").toInt() != 1) {
            status->setText(tr("O catálogo possui um formato incompatível."));
            return;
        }
        const QJsonArray received = document.object().value("addons").toArray();
        *entries = QJsonArray();
        for (int i = 0; i < received.size() && i < 500; ++i)
            entries->append(received.at(i));
        table->setRowCount(0);
        for (const QJsonValue& value : *entries) {
            const QJsonObject addon = value.toObject();
            const int index = table->rowCount();
            table->insertRow(index);
            auto* name = new QTableWidgetItem(addon.value("name").toString());
            name->setData(Qt::UserRole, index);
            table->setItem(index, 0, name);
            table->setItem(index, 1, new QTableWidgetItem(addon.value("version").toString()));
            table->setItem(index, 2, new QTableWidgetItem(addon.value("author").toString()));
            table->setItem(index, 3, new QTableWidgetItem(addon.value("description").toString()));
        }
        status->setText(entries->isEmpty()
            ? tr("O catálogo está disponível, mas ainda não possui pacotes publicados.")
            : tr("Selecione um pacote. Downloads são validados por SHA-256 antes da instalação."));
        install->setEnabled(!entries->isEmpty());
    });

    connect(install, &QPushButton::clicked, dialog, [=, this] {
        const int rowIndex = table->currentRow();
        if (rowIndex < 0 || rowIndex >= entries->size()) return;
        const QJsonObject addon = entries->at(rowIndex).toObject();
        const QUrl url(addon.value("downloadUrl").toString());
        if (!url.isValid() || url.scheme() != QLatin1String("https")) {
            QMessageBox::critical(dialog, tr("Catálogo"), tr("O pacote possui uma URL não segura."));
            return;
        }
        install->setEnabled(false);
        status->setText(tr("Baixando %1...").arg(addon.value("name").toString()));
        QNetworkReply* download = network->get(QNetworkRequest(url));
        connect(download, &QNetworkReply::downloadProgress, dialog,
                [download](qint64 received, qint64 total) {
            if (received > 100ll * 1024 * 1024 || total > 100ll * 1024 * 1024)
                download->abort();
        });
        connect(download, &QNetworkReply::finished, dialog, [=, this] {
            const QByteArray data = download->readAll();
            const QString downloadError = download->errorString();
            const bool ok = download->error() == QNetworkReply::NoError;
            download->deleteLater();
            install->setEnabled(true);
            if (!ok) {
                status->setText(tr("Falha no download: %1").arg(downloadError));
                return;
            }
            const QString actual = QString::fromLatin1(
                QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
            const QString expected = addon.value("sha256").toString().toLower();
            if (expected.size() != 64 || actual != expected) {
                status->setText(tr("O SHA-256 do pacote não corresponde ao catálogo. Instalação cancelada."));
                return;
            }
            QTemporaryFile file(QDir::tempPath() + QStringLiteral("/halla-addon-XXXXXX.halla-addon"));
            if (!file.open()) return;
            file.write(data); file.flush(); file.close();
            QString installedId, installError;
            if (installPackage(file.fileName(), dialog, &installedId, &installError)) {
                status->setText(tr("Complemento instalado. Ative-o na aba Complementos."));
            } else if (!installError.isEmpty()) {
                status->setText(installError);
            }
        });
    });
    dialog->show();
}

QJsonObject PluginManager::compactClientState(const ServerData* data,
                                                  quint64 connectionIdValue) const {
    QJsonObject payload;
    payload["connectionId"] = qint64(connectionIdValue);
    payload["connected"] = data != nullptr && !data->channels.isEmpty();
    if (!data) return payload;
    payload["serverName"] = data->name;
    payload["serverAddress"] = data->address;
    payload["selfId"] = data->selfId;
    const int channelId = data->channelOfUser(data->selfId);
    payload["channelId"] = channelId;
    payload["channelName"] = data->channels.value(channelId).name;
    QJsonArray users;
    const QList<int> channelUsers = data->channels.value(channelId).users;
    for (int id : channelUsers) {
        if (!data->users.contains(id)) continue;
        const User& user = data->users[id];
        users << QJsonObject{
            {"id", user.id}, {"uid", user.uniqueId}, {"name", user.name},
            {"talking", user.talking}, {"whispering", user.whispering},
            {"muted", user.inputMuted || user.outputMuted},
            {"inputMuted", user.inputMuted}, {"outputMuted", user.outputMuted},
            {"away", user.away}, {"self", user.id == data->selfId}
        };
    }
    payload["users"] = users;
    return payload;
}

ServerTab* PluginManager::session(quint64 connectionIdValue) const {
    const quint64 id = connectionIdValue ? connectionIdValue : m_activeSessionId;
    return m_sessions.value(id).data();
}

quint64 PluginManager::connectionId(ServerTab* tab) const {
    return tab ? m_sessionIds.value(tab, 0) : 0;
}

quint64 PluginManager::registerSession(ServerTab* tab) {
    if (!tab) return 0;
    if (m_sessionIds.contains(tab)) return m_sessionIds.value(tab);
    const quint64 id = m_nextSessionId++;
    m_sessions.insert(id, tab);
    m_sessionIds.insert(tab, id);
    if (tab->voice()) tab->voice()->setPluginConnectionId(id);
    if (tab->net()) {
        connect(tab->net(), &NetSession::pluginDataReceived, this,
                [this, id](int senderId, const QString& pluginId,
                           const QString& topic, const QByteArray& data) {
            handlePluginData(id, senderId, pluginId, topic, data);
        });
        connect(tab->net(), &NetSession::chatReceived, this,
                [this, id](const QString& scope, int senderId, int targetId,
                           const QString& senderName, const QString& text) {
            dispatchEvent(QJsonObject{{"event", "chat_message"},
                {"payload", QJsonObject{{"connectionId", qint64(id)},
                    {"scope", scope}, {"senderId", senderId},
                    {"targetId", targetId},
                    {"senderName", senderName}, {"text", text}}}});
        });
        connect(tab->net(), &NetSession::pokeReceived, this,
                [this, id](int senderId, const QString& senderName,
                           const QString& message) {
            dispatchEvent(QJsonObject{{"event", "poke_received"},
                {"payload", QJsonObject{{"connectionId", qint64(id)},
                    {"senderId", senderId}, {"senderName", senderName},
                    {"message", message}}}});
        });
        connect(tab->net(), &NetSession::errorOccurred, this,
                [this, id](const QString& code, const QString& message) {
            dispatchEvent(QJsonObject{{"event", "server_error"},
                {"payload", QJsonObject{{"connectionId", qint64(id)},
                    {"code", code}, {"message", message}}}});
        });
        connect(tab->net(), &NetSession::pingUpdated, this,
                [this, id](int) { publishSessionEvent(id); });
    }
    connect(tab, &ServerTab::statusChanged, this, [this, tab, id] {
        publishSessionEvent(id);
        if (m_activeSessionId == id) {
            m_currentState = compactClientState(&tab->data(), id);
            dispatchEvent(QJsonObject{{"event", "client_state"},
                                      {"payload", m_currentState}});
        }
    });
    connect(tab, &QObject::destroyed, this, [this, tab] {
        if (m_sessionIds.contains(tab)) unregisterSession(tab);
    });
    dispatchEvent(QJsonObject{{"event", "connection_opened"},
                              {"payload", connectionState(id)}});
    return id;
}

void PluginManager::unregisterSession(ServerTab* tab) {
    const quint64 id = m_sessionIds.take(tab);
    if (!id) return;
    m_sessions.remove(id);
    for (Record* item : m_records) item->audioConnections.remove(id);
    if (m_radioVoice) m_radioVoice->resetConnection(id);
    dispatchEvent(QJsonObject{{"event", "connection_closed"},
                              {"payload", QJsonObject{{"connectionId", qint64(id)}}}});
    if (m_activeSessionId == id) {
        m_activeSessionId = 0;
        m_currentState = compactClientState(nullptr, 0);
    }
}

void PluginManager::setActiveSession(ServerTab* tab) {
    const quint64 id = tab ? registerSession(tab) : 0;
    m_activeSessionId = id;
    publishClientState(tab);
}

QJsonObject PluginManager::connectionState(quint64 connectionIdValue) const {
    const quint64 id = connectionIdValue ? connectionIdValue : m_activeSessionId;
    ServerTab* tab = m_sessions.value(id).data();
    if (!tab) return {};
    const ServerData& data = tab->data();
    QJsonObject state;
    state["connectionId"] = qint64(id);
    state["active"] = id == m_activeSessionId;
    state["connected"] = tab->net() && tab->net()->isConnected();
    state["server"] = QJsonObject{
        {"name", data.name}, {"address", data.address}, {"version", data.version},
        {"platform", data.platform}, {"maxClients", data.maxClients},
        {"pingMs", tab->net() ? tab->net()->pingMs() : -1}
    };
    state["selfId"] = data.selfId;
    state["channelId"] = data.channelOfUser(data.selfId);
    if (tab->net()) state["permissions"] = tab->net()->myPerms();

    QJsonArray users;
    for (const User& user : data.users) {
        users << QJsonObject{
            {"id", user.id}, {"uid", user.uniqueId}, {"name", user.name},
            {"channelId", data.channelOfUser(user.id)}, {"version", user.version},
            {"platform", user.platform}, {"description", user.description},
            {"groupId", user.groupId}, {"groups", user.serverGroups},
            {"groupPosition", user.groupPosition}, {"inputMuted", user.inputMuted},
            {"outputMuted", user.outputMuted}, {"locallyMuted", user.locallyMuted},
            {"localVolumeDb", user.volumeDb}, {"away", user.away},
            {"recording", user.recording}, {"commander", user.commander},
            {"talking", user.talking}, {"whispering", user.whispering},
            {"screensharing", user.screensharing}, {"self", user.id == data.selfId}
        };
    }
    state["users"] = users;

    QJsonArray channels;
    for (const Channel& channel : data.channels) {
        QJsonArray channelUsers;
        for (int userId : channel.users) channelUsers << userId;
        QJsonArray linked;
        for (int linkedId : channel.linkedChannels) linked << linkedId;
        channels << QJsonObject{
            {"id", channel.id}, {"parentId", channel.parentId}, {"order", channel.order},
            {"name", channel.name}, {"topic", channel.topic},
            {"description", channel.description}, {"password", channel.hasPassword},
            {"default", channel.isDefault}, {"type", channel.type},
            {"moderated", channel.moderated}, {"codec", channel.codec},
            {"codecQuality", channel.codecQuality}, {"bitrate", channel.bitrate},
            {"maxClients", channel.maxClients}, {"linkedChannels", linked},
            {"users", channelUsers}
        };
    }
    state["channels"] = channels;
    return state;
}

void PluginManager::publishClientState(const ServerData* data) {
    m_currentState = compactClientState(data, 0);
    dispatchEvent(QJsonObject{{"event","client_state"},{"payload",m_currentState}});
}

void PluginManager::publishClientState(ServerTab* tab) {
    if (!tab) {
        m_activeSessionId = 0;
        publishClientState(static_cast<const ServerData*>(nullptr));
        return;
    }
    const quint64 id = registerSession(tab);
    m_activeSessionId = id;
    m_currentState = compactClientState(&tab->data(), id);
    dispatchEvent(QJsonObject{{"event","client_state"},{"payload",m_currentState}});
    publishSessionEvent(id);
}

void PluginManager::publishSessionEvent(quint64 connectionIdValue) {
    const QJsonObject state = connectionState(connectionIdValue);
    if (!state.isEmpty())
        dispatchEvent(QJsonObject{{"event", "connection_state"}, {"payload", state}});
}

void PluginManager::processAudio(quint64 connectionIdValue, int userId, uint32_t stage,
                                 uint32_t flags, int16_t* samples, uint32_t frames,
                                 uint32_t channels, uint32_t sampleRate) {
    if (!samples || !frames || !channels) return;
    if (stage != HALLA_AUDIO_CAPTURE)
        processOfficialRadio(connectionIdValue, userId, stage, flags,
                             samples, frames, channels, sampleRate);
    for (Record* item : m_records) {
        if (!item || !item->info.loaded || !item->info.enabled
                || !item->audioProcessor || !(item->audioStages & stage)) continue;
        HallaAudioFrame frame{uint32_t(sizeof(HallaAudioFrame)), stage,
                              connectionIdValue, userId, samples, frames, channels,
                              sampleRate, flags};
        try { item->audioProcessor(item->audioProcessorContext, &frame); }
        catch (...) {
            AppLog::error(tr("O complemento %1 lançou uma exceção no processamento de áudio.")
                          .arg(item->info.name));
            item->audioProcessor = nullptr;
            item->audioStages = 0;
        }
    }
}

void PluginManager::processOfficialRadio(quint64 connectionIdValue, int userId,
                                         uint32_t stage, uint32_t flags,
                                         int16_t* samples, uint32_t frames,
                                         uint32_t channels, uint32_t sampleRate) {
    if (!m_radioVoice || !samples || !frames || !channels) return;
    m_radioVoice->process(connectionIdValue, userId, stage,
                          (flags & HALLA_AUDIO_FLAG_WHISPER) != 0,
                          samples, frames, channels, sampleRate);
}

PluginAudioControl PluginManager::audioControl(quint64 connectionIdValue,
                                                int userId) const {
    PluginAudioControl result;
    for (const Record* item : m_records) {
        if (!item || !item->info.loaded || !item->info.enabled
                || !item->hasCapability("audio.spatial")) continue;
        auto connectionIt = item->audioConnections.constFind(connectionIdValue);
        if (connectionIt == item->audioConnections.cend()) continue;
        auto userIt = connectionIt->users.constFind(userId);
        if (userIt == connectionIt->users.cend()) continue;
        const Record::AudioUserState& user = userIt.value();
        result.gain *= user.gain;
        result.pan = qBound(-1.0f, result.pan + user.pan, 1.0f);
        if (user.radio) {
            result.radio = true;
            result.radioStrength = qMax(result.radioStrength, user.radioStrength);
            result.radioNoise = qMax(result.radioNoise, user.radioNoise);
        }
        if (!user.hasPosition || !connectionIt->hasListener) continue;
        const HallaTransform& listener = connectionIt->listener;
        const float dx = user.position.x - listener.position.x;
        const float dy = user.position.y - listener.position.y;
        const float dz = user.position.z - listener.position.z;
        const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
        float attenuation = 1.0f;
        if (distance >= user.maxDistance) attenuation = 0.0f;
        else if (distance > user.minDistance) {
            const float normalized = (distance - user.minDistance)
                / (user.maxDistance - user.minDistance);
            attenuation = std::pow(qMax(0.0f, 1.0f - normalized), user.rolloff);
        }
        result.gain *= attenuation;
        if (distance > 0.0001f) {
            const float rx = listener.forward.y * listener.up.z
                - listener.forward.z * listener.up.y;
            const float ry = listener.forward.z * listener.up.x
                - listener.forward.x * listener.up.z;
            const float rz = listener.forward.x * listener.up.y
                - listener.forward.y * listener.up.x;
            const float rightLength = std::sqrt(rx * rx + ry * ry + rz * rz);
            if (rightLength > 0.0001f) {
                const float spatialPan = (dx * rx + dy * ry + dz * rz)
                    / (distance * rightLength);
                result.pan = qBound(-1.0f, result.pan + spatialPan, 1.0f);
            }
        }
    }
    result.gain = qBound(0.0f, result.gain, 4.0f);
    return result;
}

void PluginManager::handlePluginData(quint64 connectionIdValue, int senderId,
                                     const QString& pluginId, const QString& topic,
                                     const QByteArray& data) {
    Record* item = record(pluginId);
    if (!item || !item->info.loaded || !item->info.enabled
            || !item->hasCapability("plugin.data")) return;
    if (item->dataHandler) {
        try {
            item->dataHandler(item->dataHandlerContext, connectionIdValue, senderId,
                topic.toUtf8().constData(),
                reinterpret_cast<const uint8_t*>(data.constData()), size_t(data.size()));
        } catch (...) {
            AppLog::error(tr("O complemento %1 lançou uma exceção ao receber dados.")
                          .arg(item->info.name));
        }
    }
    dispatchEvent(QJsonObject{
        {"event", "plugin_data"},
        {"payload", QJsonObject{{"connectionId", qint64(connectionIdValue)},
                                 {"senderId", senderId}, {"topic", topic},
                                 {"data", QString::fromLatin1(data.toBase64())}}}
    }, pluginId);
}

void PluginManager::announceUiActions() {
    for (Record* item : m_records) {
        if (!item || !item->info.loaded || !item->info.enabled) continue;
        for (auto it = item->uiActions.cbegin(); it != item->uiActions.cend(); ++it)
            emit pluginActionRegistered(item->info.id, it.key(), it->label, it->shortcut);
    }
}

void PluginManager::triggerUiAction(const QString& pluginId,
                                    const QString& actionId) {
    Record* item = record(pluginId);
    if (!item || !item->info.loaded || !item->info.enabled) return;
    auto it = item->uiActions.find(actionId);
    if (it == item->uiActions.end() || !it->callback) return;
    const QByteArray id = actionId.toUtf8();
    try { it->callback(it->pluginContext, id.constData()); }
    catch (...) {
        AppLog::error(tr("O complemento %1 lançou uma exceção em uma ação da interface.")
                      .arg(item->info.name));
    }
}

void PluginManager::dispatchEvent(const QJsonObject& event,
                                  const QString& onlyPlugin) {
    const QByteArray bytes = QJsonDocument(event).toJson(QJsonDocument::Compact);
    if (onlyPlugin.isEmpty() && m_overlay
            && event.value("event").toString() == QLatin1String("client_state"))
        m_overlay->updateClientState(event.value("payload").toObject());
    for (Record* item : m_records) {
        if (!onlyPlugin.isEmpty() && item->info.id != onlyPlugin) continue;
        if (!item->info.builtIn && item->info.enabled && item->info.loaded
                && item->api && item->api->on_event) {
            try { item->api->on_event(bytes.constData(), size_t(bytes.size())); }
            catch (...) {
                AppLog::error(tr("O complemento %1 lançou uma exceção ao processar um evento.")
                              .arg(item->info.name));
            }
        }
    }
}
