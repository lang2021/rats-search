#ifndef RATS_APP_CONFIG_STORE_H
#define RATS_APP_CONFIG_STORE_H

#include "services/filter_policy.h"

#include <QJsonObject>
#include <QObject>
#include <QVariant>

namespace rats {
namespace app {

/**
 * @brief ConfigStore - Application configuration management
 *
 * Manages application configuration with:
 * - Type-safe getters/setters
 * - Automatic persistence
 * - Change notifications
 * - Validation
 *
 * Configuration is organized into sections:
 * - Network: ports, P2P settings
 * - Spider: indexing settings
 * - Filters: content filtering
 * - Client: download settings
 * - UI: interface preferences
 *
 * Every write is routed through the single generic setValue(key, value) entry
 * point, so dedupe, clamping, dirty tracking and change notification happen in
 * exactly one place. Nested keys use dotted addressing internally
 * ("spider.walkInterval", "filters.maxFiles") while the on-disk JSON keeps the
 * original nested-object layout for backwards compatibility.
 *
 * Every successful write emits configChanged(changedKeys) — this is what drives
 * Application::applyConfig(), so a setting changed through a typed setter (the
 * settings dialog) reaches the running services exactly like one changed through
 * fromJson() (the `config.set` API method). fromJson() batches: one
 * configChanged carrying every key it touched.
 */
class ConfigStore : public QObject {
    Q_OBJECT

public:
    explicit ConfigStore(const QString& configPath = QString(), QObject* parent = nullptr);
    ~ConfigStore();

    /**
     * @brief Load configuration from file
     */
    bool load();

    /**
     * @brief Save configuration to file
     */
    bool save();

    // =========================================================================
    // Network Settings
    // =========================================================================

    int httpPort() const;
    void setHttpPort(int port);

    int p2pPort() const;
    void setP2pPort(int port);

    int dhtPort() const;
    void setDhtPort(int port);

    // Master gate for all non-HTTP network subsystems. A new installation
    // starts local-only; enabling it takes effect on the next application start.
    bool networkEnabled() const;
    void setNetworkEnabled(bool enabled);
    bool resumeBtSession() const;
    void setResumeBtSession(bool enabled);
    bool btFileDownloadEnabled() const;
    void setBtFileDownloadEnabled(bool enabled);
    bool btFileUploadEnabled() const;
    void setBtFileUploadEnabled(bool enabled);
    bool mdnsEnabled() const;
    void setMdnsEnabled(bool enabled);
    bool indexExchangeEnabled() const;
    void setIndexExchangeEnabled(bool enabled);
    // Sending the user's search terms to connected Rats peers is independent
    // from answering peers' index requests.
    bool remoteSearchEnabled() const;
    void setRemoteSearchEnabled(bool enabled);

    // =========================================================================
    // P2P Settings
    // =========================================================================

    int p2pConnections() const;
    void setP2pConnections(int connections);

    bool p2pReplication() const;
    void setP2pReplication(bool enabled);

    bool p2pReplicationServer() const;
    void setP2pReplicationServer(bool enabled);

    // Whether other peers may pull our whole index (databaseRequest). This is
    // advertised in the client_info handshake only while sharing is active.
    bool databaseSharing() const;
    void setDatabaseSharing(bool enabled);

    // When the dump we serve to peers is rebuilt. It is generated once and handed
    // to every peer that asks, so these two decide how stale a served index may be
    // — and, on the other side, how often this node spends a full export.
    //
    // Age is the backstop; drift is the real trigger, because an index that has
    // not moved does not need a new dump however old the one on disk is.
    int databaseSnapshotMaxAgeHours() const;
    void setDatabaseSnapshotMaxAgeHours(int hours);
    int databaseSnapshotMaxDriftPercent() const;
    void setDatabaseSnapshotMaxDriftPercent(int percent);

    // =========================================================================
    // Indexer Settings
    // =========================================================================

    bool indexerEnabled() const;
    void setIndexerEnabled(bool enabled);

    bool trackersEnabled() const;
    void setTrackersEnabled(bool enabled);

    bool restApiEnabled() const;
    void setRestApiEnabled(bool enabled);

    // Config-file only: read at startup to gate UPnP/NAT-PMP port mapping. There
    // is deliberately no setter — nothing in the app changes it at runtime.
    bool upnpEnabled() const;

    // Config-file only, same shape: gates NAT hole punching (and relaying other
    // peers' rendezvous), the fallback for networks where port mapping fails.
    bool holePunchEnabled() const;

    // Config-file only, same shape: the rung below punching — reach a peer
    // through a third node both ends already hold when no punch can land.
    bool relayEnabled() const;

    // Config-file only: carry OTHER peers' circuits. Off by default — unlike a
    // hole-punch rendezvous, this spends real uplink on somebody else's traffic.
    bool relayServeEnabled() const;

    // =========================================================================
    // Spider Settings
    // =========================================================================

    int spiderWalkInterval() const;
    void setSpiderWalkInterval(int interval);

    // =========================================================================
    // Filter Settings
    // =========================================================================

    int filtersMaxFiles() const;
    void setFiltersMaxFiles(int max);

    QString filtersNamingRegExp() const;
    void setFiltersNamingRegExp(const QString& regexp);

    bool filtersNamingRegExpNegative() const;
    void setFiltersNamingRegExpNegative(bool negative);

    bool filtersAdultFilter() const;
    void setFiltersAdultFilter(bool enabled);

    qint64 filtersSizeMin() const;
    void setFiltersSizeMin(qint64 min);

    qint64 filtersSizeMax() const;
    void setFiltersSizeMax(qint64 max);

    QString filtersContentType() const;
    void setFiltersContentType(const QString& type);

    // The stored filter keys as one struct, ready for service::FilterPolicy.
    // Application::applyConfig() and the `torrent.cleanup` API method both go
    // through this, so a sweep judges torrents with exactly the rules the
    // indexer applies to new ones.
    service::FilterSettings filterSettings() const;

    // =========================================================================
    // Client Settings
    // =========================================================================

    QString downloadPath() const;
    void setDownloadPath(const QString& path);

    // =========================================================================
    // UI Settings
    // =========================================================================

    QString language() const;
    void setLanguage(const QString& lang);

    bool darkMode() const;
    void setDarkMode(bool enabled);

    // Interface font. The family is put in front of the theme's own font stack
    // (empty = the theme's stack as is); the scale multiplies every font size in
    // the stylesheet, in percent, clamped to [kMinUiFontScale, kMaxUiFontScale].
    // Both are applied by ui::Theme the moment they change, no restart.
    static constexpr int kMinUiFontScale = 50;
    static constexpr int kMaxUiFontScale = 200;
    static constexpr int kDefaultUiFontScale = 100;

    QString uiFontFamily() const;
    void setUiFontFamily(const QString& family);

    int uiFontScale() const;
    void setUiFontScale(int percent);

    // Search-time adult filter: hides the XXX category from search results. This
    // is distinct from filters.adultFilter, which drops adult torrents at index
    // time and so cannot be undone by toggling it back off.
    bool safeSearch() const;
    void setSafeSearch(bool enabled);

    // Whether user-initiated searches are remembered in app::SearchHistoryStore.
    // Turning it off stops new queries from being recorded; already-stored ones
    // stay until the user clears the history explicitly.
    bool searchHistoryEnabled() const;
    void setSearchHistoryEnabled(bool enabled);

    bool trayOnClose() const;
    void setTrayOnClose(bool enabled);

    // Whether quitting the GUI asks "Are you sure you want to exit?" first.
    bool confirmExit() const;
    void setConfirmExit(bool enabled);

    bool trayOnMinimize() const;
    void setTrayOnMinimize(bool enabled);

    bool startMinimized() const;
    void setStartMinimized(bool enabled);

    bool checkUpdatesOnStartup() const;
    void setCheckUpdatesOnStartup(bool enabled);

    bool agreementAccepted() const;
    void setAgreementAccepted(bool accepted);

    // =========================================================================
    // Logging
    // =========================================================================

    // Total disk budget for rats-search.log and its rotated archives, in MB.
    // Applied through common::applyLogSizeBudget() from Application::applyConfig(),
    // so a change takes effect on the running logger without a restart. Clamped
    // to [common::kMinLogMaxSizeMb, common::kMaxLogMaxSizeMb].
    int logMaxSizeMb() const;
    void setLogMaxSizeMb(int megabytes);

    // =========================================================================
    // Generic Access (for API)
    // =========================================================================

    /**
     * @brief Get all config as JSON
     */
    QJsonObject toJson() const;

    /**
     * @brief Update from JSON (bulk update)
     * @param options Key-value pairs to update
     * @return List of changed keys
     */
    QStringList fromJson(const QJsonObject& options);

    /**
     * @brief Set a specific value
     *
     * The single write path for the whole store: dedupes (no-op if unchanged),
     * clamps, sets the dirty flag and emits configChanged for the key.
     * @return true if value was changed
     */
    bool setValue(const QString& key, const QVariant& value);

signals:
    // Emitted after every successful write, carrying the keys that changed.
    void configChanged(const QStringList& changedKeys);

    // Two keys whose consumers need to react on their own, without inspecting
    // changedKeys: the translators and the stylesheet are swapped immediately.
    void languageChanged(const QString& lang);
    void darkModeChanged(bool enabled);

private:
    void setDefaults();

    // Repair values loaded from disk (clamp out-of-range, enforce the
    // replication/replication-server dependency).
    void validateAndClamp();

    // Write without notifying — the shared core of setValue() and fromJson().
    // Returns true when the stored value actually changed.
    bool writeValue(const QString& key, const QVariant& value);

    // Nested-aware JSON accessors. Dotted keys ("filters.maxFiles") address a
    // value inside a nested object; plain keys address the top level.
    QJsonValue readJsonValue(const QString& key) const;
    void writeJsonValue(const QString& key, const QJsonValue& jsonValue);

    QString configPath_;
    QJsonObject config_;
    bool dirty_ = false;
};

} // namespace app
} // namespace rats

#endif // RATS_APP_CONFIG_STORE_H
