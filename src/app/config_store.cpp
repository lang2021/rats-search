#include "app/config_store.h"

#include "common/logging.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QSaveFile>
#include <QJsonDocument>
#include <QJsonValue>
#include <QStandardPaths>

namespace rats {
namespace app {

namespace {

constexpr int kMinP2pConnections = 10;
constexpr int kMaxP2pConnections = 1000;

// The keys with a valid range. Applied on every write and again after a load,
// so an out-of-range value can never reach the services.
QVariant clampToRange(const QString& key, const QVariant& value)
{
    if (key == QLatin1String("uiFontScale")) {
        return qBound(ConfigStore::kMinUiFontScale, value.toInt(), ConfigStore::kMaxUiFontScale);
    }
    if (key == QLatin1String("p2pConnections")) {
        return qBound(kMinP2pConnections, value.toInt(), kMaxP2pConnections);
    }
    if (key == QLatin1String("logMaxSizeMb")) {
        return qBound(common::kMinLogMaxSizeMb, value.toInt(), common::kMaxLogMaxSizeMb);
    }
    return value;
}

} // namespace

ConfigStore::ConfigStore(const QString& configPath, QObject* parent) : QObject(parent), configPath_(configPath)
{
    setDefaults();

    if (configPath_.isEmpty()) {
        QString dataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        QDir().mkpath(dataDir);
        configPath_ = dataDir + "/rats.json";
    }
}

ConfigStore::~ConfigStore()
{
    if (dirty_) {
        save();
    }
}

void ConfigStore::setDefaults()
{
    config_ = QJsonObject { // Network
        { "httpPort", 8095 }, { "p2pPort", 4444 }, { "dhtPort", 6881 }, { "networkEnabled", false },
        { "independentDirect", false },
        { "resumeBtSession", false }, { "btFileDownload", false }, { "btFileUpload", false },
        { "mdns", false }, { "indexExchange", false }, { "remoteSearch", false },

        // P2P
        { "p2pConnections", 10 }, { "p2pReplication", false }, { "p2pReplicationServer", false },
        { "databaseSharing", false }, { "databaseSnapshotMaxAgeHours", 6 }, { "databaseSnapshotMaxDriftPercent", 10 },

        // Indexer
        { "indexer", true }, { "trackers", true }, { "restApi", false }, { "upnp", false }, { "holePunch", false },
        { "relay", false }, { "relayServe", false },

        // Spider
        { "spider", QJsonObject { { "walkInterval", 100 } } },

        // Filters
        { "filters",
            QJsonObject { { "maxFiles", 0 }, { "namingRegExp", "" }, { "namingRegExpNegative", false },
                { "adultFilter", false }, { "sizeMin", 0 }, { "sizeMax", 0 }, { "contentType", "" } } },

        // Downloads
        { "downloadPath", QStandardPaths::writableLocation(QStandardPaths::DownloadLocation) },

        // UI
        { "language", "en" }, { "darkMode", false }, { "uiFontFamily", "" }, { "uiFontScale", kDefaultUiFontScale },
        { "safeSearch", false }, { "searchHistory", true }, { "trayOnClose", false }, { "trayOnMinimize", true },
        { "startMinimized", false }, { "confirmExit", true }, { "checkUpdatesOnStartup", true },

        // Legal
        { "agreementAccepted", false },

        // Logging
        { "logMaxSizeMb", common::kDefaultLogMaxSizeMb }
    };
}

bool ConfigStore::load()
{
    QFile file(configPath_);
    if (!file.exists()) {
        qInfo() << "Config file not found, using defaults:" << configPath_;
        return true;
    }

    if (!file.open(QIODevice::ReadOnly)) {
        qWarning() << "Failed to open config file:" << configPath_;
        return false;
    }

    QByteArray data = file.readAll();
    file.close();

    QJsonParseError error;
    QJsonDocument doc = QJsonDocument::fromJson(data, &error);
    if (error.error != QJsonParseError::NoError) {
        qWarning() << "Failed to parse config:" << error.errorString();
        return false;
    }

    // Merge loaded config with defaults
    QJsonObject loaded = doc.object();
    // Legacy configs predate the master gate and their sharing/mapping values
    // came from permissive defaults. Do not treat those defaults as consent when
    // the user later opts into networking. Keep the other stored settings.
    if (!loaded.contains("networkEnabled")) {
        loaded["databaseSharing"] = false;
        loaded["p2pReplication"] = false;
        loaded["upnp"] = false;
        loaded["holePunch"] = false;
        loaded["relay"] = false;
    }
    // Older releases defaulted the random-index serving flag to true and even
    // switched it on when only client replication was selected. Do not carry
    // that implicit permission into the separately controlled module.
    if (!loaded.contains("remoteSearch"))
        loaded["p2pReplicationServer"] = false;
    for (auto it = loaded.begin(); it != loaded.end(); ++it) {
        if (it.value().isObject() && config_.value(it.key()).isObject()) {
            // Merge nested objects
            QJsonObject base = config_.value(it.key()).toObject();
            QJsonObject overlay = it.value().toObject();
            for (auto oit = overlay.begin(); oit != overlay.end(); ++oit) {
                base[oit.key()] = oit.value();
            }
            config_[it.key()] = base;
        } else {
            config_[it.key()] = it.value();
        }
    }

    validateAndClamp();
    qInfo() << "Config loaded from" << configPath_;
    return true;
}

bool ConfigStore::save()
{
    QSaveFile file(configPath_);
    if (!file.open(QIODevice::WriteOnly)) {
        qWarning() << "Failed to open config for saving:" << configPath_ << file.errorString();
        return false;
    }

    QJsonDocument doc(config_);
    const QByteArray data = doc.toJson(QJsonDocument::Indented);
    if (file.write(data) != data.size()) {
        qWarning() << "Failed to write config:" << configPath_ << file.errorString();
        file.cancelWriting();
        return false;
    }
    if (!file.commit()) {
        qWarning() << "Failed to commit config:" << configPath_ << file.errorString();
        return false;
    }

    dirty_ = false;
    qInfo() << "Config saved to" << configPath_;
    return true;
}

void ConfigStore::validateAndClamp()
{
    config_["p2pConnections"]
        = QJsonValue::fromVariant(clampToRange("p2pConnections", config_["p2pConnections"].toInt()));
    config_["logMaxSizeMb"] = QJsonValue::fromVariant(
        clampToRange("logMaxSizeMb", config_["logMaxSizeMb"].toInt(common::kDefaultLogMaxSizeMb)));
    config_["uiFontScale"]
        = QJsonValue::fromVariant(clampToRange("uiFontScale", config_["uiFontScale"].toInt(kDefaultUiFontScale)));

}

// ============================================================================
// Nested-aware JSON accessors
// ============================================================================

QJsonValue ConfigStore::readJsonValue(const QString& key) const
{
    int dot = key.indexOf('.');
    if (dot < 0) {
        return config_.value(key);
    }
    QString group = key.left(dot);
    QString sub = key.mid(dot + 1);
    return config_.value(group).toObject().value(sub);
}

void ConfigStore::writeJsonValue(const QString& key, const QJsonValue& jsonValue)
{
    int dot = key.indexOf('.');
    if (dot < 0) {
        config_[key] = jsonValue;
        return;
    }
    QString group = key.left(dot);
    QString sub = key.mid(dot + 1);
    QJsonObject obj = config_.value(group).toObject();
    obj[sub] = jsonValue;
    config_[group] = obj;
}

// ============================================================================
// Network Settings
// ============================================================================

int ConfigStore::httpPort() const
{
    return config_["httpPort"].toInt(8095);
}
void ConfigStore::setHttpPort(int port)
{
    setValue("httpPort", port);
}

int ConfigStore::p2pPort() const
{
    return config_["p2pPort"].toInt(4444);
}
void ConfigStore::setP2pPort(int port)
{
    setValue("p2pPort", port);
}

int ConfigStore::dhtPort() const
{
    return config_["dhtPort"].toInt(6881);
}
void ConfigStore::setDhtPort(int port)
{
    setValue("dhtPort", port);
}

bool ConfigStore::networkEnabled() const
{
    return config_["networkEnabled"].toBool(false);
}
void ConfigStore::setNetworkEnabled(bool enabled)
{
    setValue("networkEnabled", enabled);
}
bool ConfigStore::independentDirect() const
{
    return config_["independentDirect"].toBool(false);
}
void ConfigStore::setIndependentDirect(bool enabled)
{
    setValue("independentDirect", enabled);
}
bool ConfigStore::resumeBtSession() const
{
    return config_["resumeBtSession"].toBool(false);
}
void ConfigStore::setResumeBtSession(bool enabled)
{
    setValue("resumeBtSession", enabled);
}
bool ConfigStore::btFileDownloadEnabled() const
{
    return config_["btFileDownload"].toBool(false);
}
void ConfigStore::setBtFileDownloadEnabled(bool enabled)
{
    setValue("btFileDownload", enabled);
}
bool ConfigStore::btFileUploadEnabled() const
{
    return config_["btFileUpload"].toBool(false);
}
void ConfigStore::setBtFileUploadEnabled(bool enabled)
{
    setValue("btFileUpload", enabled);
}
bool ConfigStore::mdnsEnabled() const
{
    return config_["mdns"].toBool(false);
}
void ConfigStore::setMdnsEnabled(bool enabled)
{
    setValue("mdns", enabled);
}
bool ConfigStore::indexExchangeEnabled() const
{
    return config_["indexExchange"].toBool(false);
}
void ConfigStore::setIndexExchangeEnabled(bool enabled)
{
    setValue("indexExchange", enabled);
}
bool ConfigStore::remoteSearchEnabled() const
{
    return config_["remoteSearch"].toBool(false);
}
void ConfigStore::setRemoteSearchEnabled(bool enabled)
{
    setValue("remoteSearch", enabled);
}

// ============================================================================
// P2P Settings
// ============================================================================

int ConfigStore::p2pConnections() const
{
    return config_["p2pConnections"].toInt(10);
}
void ConfigStore::setP2pConnections(int connections)
{
    setValue("p2pConnections", connections);
}

bool ConfigStore::p2pReplication() const
{
    return config_["p2pReplication"].toBool(true);
}
void ConfigStore::setP2pReplication(bool enabled)
{
    setValue("p2pReplication", enabled);
}

bool ConfigStore::p2pReplicationServer() const
{
    return config_["p2pReplicationServer"].toBool(false);
}
void ConfigStore::setP2pReplicationServer(bool enabled)
{
    setValue("p2pReplicationServer", enabled);
}

bool ConfigStore::databaseSharing() const
{
    return config_["databaseSharing"].toBool(true);
}
void ConfigStore::setDatabaseSharing(bool enabled)
{
    setValue("databaseSharing", enabled);
}

int ConfigStore::databaseSnapshotMaxAgeHours() const
{
    // Clamped rather than trusted: a zero would rebuild the snapshot on every
    // single request, which is the behaviour the snapshot exists to remove.
    return qBound(1, config_["databaseSnapshotMaxAgeHours"].toInt(6), 24 * 30);
}
void ConfigStore::setDatabaseSnapshotMaxAgeHours(int hours)
{
    setValue("databaseSnapshotMaxAgeHours", hours);
}

int ConfigStore::databaseSnapshotMaxDriftPercent() const
{
    return qBound(1, config_["databaseSnapshotMaxDriftPercent"].toInt(10), 100);
}
void ConfigStore::setDatabaseSnapshotMaxDriftPercent(int percent)
{
    setValue("databaseSnapshotMaxDriftPercent", percent);
}

// ============================================================================
// Indexer Settings
// ============================================================================

bool ConfigStore::indexerEnabled() const
{
    return config_["indexer"].toBool(true);
}
void ConfigStore::setIndexerEnabled(bool enabled)
{
    setValue("indexer", enabled);
}

bool ConfigStore::trackersEnabled() const
{
    return config_["trackers"].toBool(true);
}
void ConfigStore::setTrackersEnabled(bool enabled)
{
    setValue("trackers", enabled);
}

bool ConfigStore::restApiEnabled() const
{
    return config_["restApi"].toBool(false);
}
void ConfigStore::setRestApiEnabled(bool enabled)
{
    setValue("restApi", enabled);
}

bool ConfigStore::upnpEnabled() const
{
    return config_["upnp"].toBool(true);
}

bool ConfigStore::holePunchEnabled() const
{
    return config_["holePunch"].toBool(true);
}

bool ConfigStore::relayEnabled() const
{
    return config_["relay"].toBool(true);
}

bool ConfigStore::relayServeEnabled() const
{
    return config_["relayServe"].toBool(false);
}

// ============================================================================
// Spider Settings
// ============================================================================

int ConfigStore::spiderWalkInterval() const
{
    return config_["spider"].toObject()["walkInterval"].toInt(100);
}
void ConfigStore::setSpiderWalkInterval(int interval)
{
    setValue("spider.walkInterval", interval);
}

// ============================================================================
// Filter Settings
// ============================================================================

int ConfigStore::filtersMaxFiles() const
{
    return config_["filters"].toObject()["maxFiles"].toInt(0);
}
void ConfigStore::setFiltersMaxFiles(int max)
{
    setValue("filters.maxFiles", max);
}

QString ConfigStore::filtersNamingRegExp() const
{
    return config_["filters"].toObject()["namingRegExp"].toString();
}
void ConfigStore::setFiltersNamingRegExp(const QString& regexp)
{
    setValue("filters.namingRegExp", regexp);
}

bool ConfigStore::filtersNamingRegExpNegative() const
{
    return config_["filters"].toObject()["namingRegExpNegative"].toBool(false);
}
void ConfigStore::setFiltersNamingRegExpNegative(bool negative)
{
    setValue("filters.namingRegExpNegative", negative);
}

bool ConfigStore::filtersAdultFilter() const
{
    return config_["filters"].toObject()["adultFilter"].toBool(false);
}
void ConfigStore::setFiltersAdultFilter(bool enabled)
{
    setValue("filters.adultFilter", enabled);
}

qint64 ConfigStore::filtersSizeMin() const
{
    return config_["filters"].toObject()["sizeMin"].toVariant().toLongLong();
}
void ConfigStore::setFiltersSizeMin(qint64 min)
{
    setValue("filters.sizeMin", QVariant(static_cast<qlonglong>(min)));
}

qint64 ConfigStore::filtersSizeMax() const
{
    return config_["filters"].toObject()["sizeMax"].toVariant().toLongLong();
}
void ConfigStore::setFiltersSizeMax(qint64 max)
{
    setValue("filters.sizeMax", QVariant(static_cast<qlonglong>(max)));
}

QString ConfigStore::filtersContentType() const
{
    return config_["filters"].toObject()["contentType"].toString();
}
void ConfigStore::setFiltersContentType(const QString& type)
{
    setValue("filters.contentType", type);
}

service::FilterSettings ConfigStore::filterSettings() const
{
    service::FilterSettings fs;
    fs.maxFiles = filtersMaxFiles();
    fs.sizeMin = filtersSizeMin();
    fs.sizeMax = filtersSizeMax();
    fs.adultFilter = filtersAdultFilter();
    fs.namingRegExp = filtersNamingRegExp();
    fs.namingRegExpNegative = filtersNamingRegExpNegative();
    fs.contentTypeFilter = filtersContentType();
    return fs;
}

// ============================================================================
// Client Settings
// ============================================================================

QString ConfigStore::downloadPath() const
{
    return config_["downloadPath"].toString(QStandardPaths::writableLocation(QStandardPaths::DownloadLocation));
}
void ConfigStore::setDownloadPath(const QString& path)
{
    setValue("downloadPath", path);
}

// ============================================================================
// UI Settings
// ============================================================================

QString ConfigStore::language() const
{
    return config_["language"].toString("en");
}
void ConfigStore::setLanguage(const QString& lang)
{
    setValue("language", lang);
}

bool ConfigStore::darkMode() const
{
    return config_["darkMode"].toBool(false);
}
void ConfigStore::setDarkMode(bool enabled)
{
    setValue("darkMode", enabled);
}

QString ConfigStore::uiFontFamily() const
{
    return config_["uiFontFamily"].toString();
}
void ConfigStore::setUiFontFamily(const QString& family)
{
    setValue("uiFontFamily", family.trimmed());
}

int ConfigStore::uiFontScale() const
{
    return config_["uiFontScale"].toInt(kDefaultUiFontScale);
}
void ConfigStore::setUiFontScale(int percent)
{
    setValue("uiFontScale", percent);
}

bool ConfigStore::safeSearch() const
{
    return config_["safeSearch"].toBool(false);
}
void ConfigStore::setSafeSearch(bool enabled)
{
    setValue("safeSearch", enabled);
}

bool ConfigStore::searchHistoryEnabled() const
{
    return config_["searchHistory"].toBool(true);
}
void ConfigStore::setSearchHistoryEnabled(bool enabled)
{
    setValue("searchHistory", enabled);
}

bool ConfigStore::trayOnClose() const
{
    return config_["trayOnClose"].toBool(false);
}
void ConfigStore::setTrayOnClose(bool enabled)
{
    setValue("trayOnClose", enabled);
}

bool ConfigStore::confirmExit() const
{
    return config_["confirmExit"].toBool(true);
}
void ConfigStore::setConfirmExit(bool enabled)
{
    setValue("confirmExit", enabled);
}

bool ConfigStore::trayOnMinimize() const
{
    return config_["trayOnMinimize"].toBool(true);
}
void ConfigStore::setTrayOnMinimize(bool enabled)
{
    setValue("trayOnMinimize", enabled);
}

bool ConfigStore::startMinimized() const
{
    return config_["startMinimized"].toBool(false);
}
void ConfigStore::setStartMinimized(bool enabled)
{
    setValue("startMinimized", enabled);
}

bool ConfigStore::checkUpdatesOnStartup() const
{
    return config_["checkUpdatesOnStartup"].toBool(true);
}
void ConfigStore::setCheckUpdatesOnStartup(bool enabled)
{
    setValue("checkUpdatesOnStartup", enabled);
}

bool ConfigStore::agreementAccepted() const
{
    return config_["agreementAccepted"].toBool(false);
}
void ConfigStore::setAgreementAccepted(bool accepted)
{
    if (setValue("agreementAccepted", accepted)) {
        save(); // Immediately persist agreement acceptance
    }
}

// ============================================================================
// Logging
// ============================================================================

int ConfigStore::logMaxSizeMb() const
{
    return config_["logMaxSizeMb"].toInt(common::kDefaultLogMaxSizeMb);
}
void ConfigStore::setLogMaxSizeMb(int megabytes)
{
    setValue("logMaxSizeMb", megabytes);
}

// ============================================================================
// Generic Access
// ============================================================================

QJsonObject ConfigStore::toJson() const
{
    return config_;
}

QStringList ConfigStore::fromJson(const QJsonObject& options)
{
    QStringList changed;

    for (auto it = options.begin(); it != options.end(); ++it) {
        if (writeValue(it.key(), it.value().toVariant())) {
            changed.append(it.key());
        }
    }

    if (!changed.isEmpty()) {
        validateAndClamp();
        emit configChanged(changed);
    }

    return changed;
}

bool ConfigStore::writeValue(const QString& key, const QVariant& value)
{
    // Clamp before comparing, so an out-of-range write dedupes against the
    // already-clamped stored value on every path (typed setter, fromJson).
    const QVariant effective = clampToRange(key, value);

    const QJsonValue newVal = QJsonValue::fromVariant(effective);
    if (readJsonValue(key) == newVal) {
        return false;
    }

    writeJsonValue(key, newVal);
    dirty_ = true;

    if (key == QLatin1String("language")) {
        emit languageChanged(effective.toString());
    } else if (key == QLatin1String("darkMode")) {
        emit darkModeChanged(effective.toBool());
    }
    return true;
}

bool ConfigStore::setValue(const QString& key, const QVariant& value)
{
    if (!writeValue(key, value)) {
        return false;
    }
    emit configChanged(QStringList { key });
    return true;
}

} // namespace app
} // namespace rats
