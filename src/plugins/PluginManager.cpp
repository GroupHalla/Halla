#include "PluginManager.h"

#include "TalkingOverlay.h"
#include "AppLog.h"
#include "Models.h"
#include "Settings.h"
#include "halla_plugin_api.h"

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
#include <QHeaderView>
#include <QJsonDocument>
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
#include <QSpinBox>
#include <QStandardPaths>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QUrl>
#include <QVBoxLayout>

#include <cstring>
#include <memory>

struct PluginManager::Record {
    AddonInfo info;
    QJsonObject manifest;
    QLibrary* library = nullptr;
    const HallaPluginApi* api = nullptr;
    HallaHostApi host{};
    QByteArray settingsCache;

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
        if (!record) return 0;
        const size_t required = size_t(record->settingsCache.size()) + 1;
        if (buffer && bufferSize > 0) {
            const size_t count = qMin(bufferSize - 1, size_t(record->settingsCache.size()));
            if (count) std::memcpy(buffer, record->settingsCache.constData(), count);
            buffer[count] = '\0';
        }
        return required;
    }

    static void hostRequestState(void*) {
        PluginManager& manager = PluginManager::instance();
        if (!manager.m_currentState.isEmpty()) {
            manager.dispatchEvent(QJsonObject{
                {QStringLiteral("event"), QStringLiteral("client_state")},
                {QStringLiteral("payload"), manager.m_currentState}
            });
        }
    }
};

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
        if (!m_overlay) m_overlay = new TalkingOverlay;
        m_overlay->applySettings(settings);
        m_overlay->updateClientState(m_currentState);
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
    item->api = entry();
    if (!item->api || item->api->abi_version != HALLA_PLUGIN_ABI_VERSION
            || item->api->struct_size < sizeof(HallaPluginApi)
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
        if (item->api->on_event) item->api->on_event(bytes.constData(), size_t(bytes.size()));
    }
    AppLog::info(tr("Complemento carregado: %1 %2").arg(item->info.name, item->info.version));
    return true;
}

void PluginManager::unload(Record* item) {
    if (!item) return;
    if (item->info.builtIn) {
        if (m_overlay) {
            m_overlay->hide();
            delete m_overlay;
            m_overlay = nullptr;
        }
        item->info.loaded = false;
        return;
    }
    if (item->api && item->info.loaded && item->api->shutdown) {
        try { item->api->shutdown(); } catch (...) {}
    }
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
        if (m_overlay) m_overlay->applySettings(settings);
        return;
    }
    item->settingsCache = QJsonDocument(settings).toJson(QJsonDocument::Compact);
    if (item->info.loaded && item->api && item->api->on_settings_changed) {
        item->api->on_settings_changed(item->settingsCache.constData(),
                                       size_t(item->settingsCache.size()));
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
    const QString warning = tr(
        "Complementos nativos executam código no mesmo processo do Halla. Instale somente arquivos de autores confiáveis.\n\n"
        "Complemento: %1\nAutor: %2\nSHA-256: %3\n\nDeseja instalar?")
        .arg(name, author, sha256);
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

void PluginManager::publishClientState(const ServerData* data) {
    QJsonObject payload;
    payload["connected"] = data != nullptr && !data->channels.isEmpty();
    if (data) {
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
                {"id", user.id}, {"name", user.name},
                {"talking", user.talking}, {"whispering", user.whispering},
                {"muted", user.inputMuted || user.outputMuted},
                {"self", user.id == data->selfId}
            };
        }
        payload["users"] = users;
    }
    m_currentState = payload;
    dispatchEvent(QJsonObject{{"event","client_state"},{"payload",payload}});
}

void PluginManager::dispatchEvent(const QJsonObject& event) {
    const QByteArray bytes = QJsonDocument(event).toJson(QJsonDocument::Compact);
    if (m_overlay && event.value("event").toString() == QLatin1String("client_state"))
        m_overlay->updateClientState(event.value("payload").toObject());
    for (Record* item : m_records) {
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
