#include "app/application.h"

#include "app/config_store.h"
#include "app/favorites_store.h"
#include "app/search_history_store.h"
#include "app/translation_manager.h"
#include "common/logging.h"
#include "data/database.h"
#include "data/feed_repository.h"
#include "data/manticore.h"
#include "data/torrent_repository.h"
#include "domain/torrent.h"
#include "net/crawler.h"
#include "net/p2p_transport.h"
#include "net/swarm_scraper.h"
#include "net/torrent_engine.h"
#include "net/tracker_site_scraper.h"
#include "peer/peer_api.h"
#include "rest/api_router.h"
#include "rest/api_server.h"
#include "services/database_sync_service.h"
#include "services/download_service.h"
#include "services/feed_service.h"
#include "services/filter_policy.h"
#include "services/indexing_service.h"
#include "services/migration_service.h"
#include "services/p2p_store.h"
#include "services/peer_registry.h"
#include "services/replication_service.h"
#include "services/search_service.h"
#include "services/torrent_creator.h"
#include "services/torrent_exporter.h"
#include "services/tracker_service.h"
#include "services/update_service.h"
#include "services/voting_service.h"

#include <QCoreApplication>
#include <QDebug>
#include <QThread>

namespace rats::app {

using namespace rats;

struct Application::Private {
    Application::Options options;
    bool running = false;
    bool downloadSessionLoaded = false;
    QString networkError;
    QString crawlerError;
    std::optional<bool> crawlerRunOverride; // this process only; config indexer is the next-start preference
    enum class NetworkState { Stopped, Starting, Running, Stopping, Failed } networkState = NetworkState::Stopped;

    // Adapters (owned, constructed in dependency order)
    std::unique_ptr<ConfigStore> config;
    std::unique_ptr<FavoritesStore> favorites;
    std::unique_ptr<SearchHistoryStore> searchHistory;
    std::unique_ptr<data::Manticore> manticore;
    std::unique_ptr<data::Database> database;
    std::unique_ptr<data::TorrentRepository> torrents;
    std::unique_ptr<data::FeedRepository> feedRepo;
    std::unique_ptr<net::P2PTransport> transport;
    std::unique_ptr<net::TorrentEngine> engine;
    std::unique_ptr<net::Crawler> crawler;
    std::unique_ptr<net::SwarmScraper> swarmScraper;
    std::unique_ptr<net::TrackerSiteScraper> siteScraper;

    // Services
    std::unique_ptr<service::FilterPolicy> filter;
    std::unique_ptr<service::IndexingService> indexing;
    std::unique_ptr<service::SearchService> search;
    std::unique_ptr<service::PeerRegistry> peers;
    std::unique_ptr<service::DownloadService> downloads;
    std::unique_ptr<service::TorrentCreator> creator;
    std::unique_ptr<service::TorrentExporter> exporter;
    std::unique_ptr<service::FeedService> feed;
    std::unique_ptr<service::P2PStore> p2pStore;
    std::unique_ptr<service::VotingService> voting;
    std::unique_ptr<service::ReplicationService> replication;
    std::unique_ptr<service::DatabaseSyncService> databaseSync;
    std::unique_ptr<service::TrackerService> trackers;
    std::unique_ptr<service::MigrationService> migrations;
    std::unique_ptr<service::UpdateService> updates;

    // Front-ends
    std::unique_ptr<rest::ApiRouter> apiRouter;
    std::unique_ptr<peer::PeerApi> peerApi;
    std::unique_ptr<rest::ApiServer> apiServer;
};

Application::Application(Options options, QObject* parent) : QObject(parent), d_(std::make_unique<Private>())
{
    d_->options = std::move(options);

    // Metatypes for cross-thread queued signals carrying our value types.
    qRegisterMetaType<domain::Torrent>("rats::domain::Torrent");
    qRegisterMetaType<domain::SearchHit>("rats::domain::SearchHit");

    const QString dataDir = d_->options.dataDirectory;
    auto* cfg = new ConfigStore(dataDir + QStringLiteral("/rats.json"), this);
    d_->config.reset(cfg);
    d_->config->load();

    // Translators must be installed before any front-end builds its UI, so the
    // first tr() call already resolves in the user's language.
    TranslationManager& translations = TranslationManager::instance();
    translations.initialize(QCoreApplication::instance(), dataDir + QStringLiteral("/translations"));
    QString language = cfg->language();
    if (language.isEmpty())
        language = TranslationManager::systemLanguage();
    translations.setLanguage(language);

    d_->favorites = std::make_unique<FavoritesStore>(dataDir);
    d_->searchHistory = std::make_unique<SearchHistoryStore>(dataDir);

    // --- Data layer -------------------------------------------------------
    d_->manticore = std::make_unique<data::Manticore>(dataDir);
    d_->database = std::make_unique<data::Database>(d_->manticore.get());
    d_->torrents = std::make_unique<data::TorrentRepository>(d_->database.get());
    d_->feedRepo = std::make_unique<data::FeedRepository>(d_->database.get());

    // --- Net layer --------------------------------------------------------
    const int p2pPort = d_->options.p2pPort > 0 ? d_->options.p2pPort : cfg->p2pPort();
    const int dhtPort = d_->options.dhtPort > 0 ? d_->options.dhtPort : cfg->dhtPort();
    const int maxPeers = d_->options.maxPeers > 0 ? d_->options.maxPeers : cfg->p2pConnections();
    d_->transport = std::make_unique<net::P2PTransport>(p2pPort, dhtPort, dataDir, maxPeers);
    d_->transport->setIndependentDirect(cfg->independentDirect());
    d_->engine = std::make_unique<net::TorrentEngine>(d_->transport.get());
    d_->crawler = std::make_unique<net::Crawler>(d_->transport.get());
    d_->swarmScraper = std::make_unique<net::SwarmScraper>();
    d_->siteScraper = std::make_unique<net::TrackerSiteScraper>();

    // --- Services ---------------------------------------------------------
    d_->filter = std::make_unique<service::FilterPolicy>();
    d_->indexing = std::make_unique<service::IndexingService>(d_->torrents.get(), d_->filter.get());
    d_->search = std::make_unique<service::SearchService>(d_->torrents.get());
    d_->peers = std::make_unique<service::PeerRegistry>(d_->transport.get(), d_->options.clientVersion);
    d_->downloads = std::make_unique<service::DownloadService>(d_->engine.get());
    d_->creator = std::make_unique<service::TorrentCreator>(d_->engine.get(), d_->downloads.get());
    d_->exporter = std::make_unique<service::TorrentExporter>(d_->engine.get(), dataDir, this);
    d_->feed = std::make_unique<service::FeedService>(d_->feedRepo.get(), d_->torrents.get());
    d_->p2pStore = std::make_unique<service::P2PStore>(d_->transport.get());
    d_->voting = std::make_unique<service::VotingService>(d_->p2pStore.get(), d_->torrents.get());
    d_->replication = std::make_unique<service::ReplicationService>(d_->transport.get());
    d_->databaseSync = std::make_unique<service::DatabaseSyncService>(
        d_->torrents.get(), d_->indexing.get(), d_->transport.get(), dataDir, d_->options.clientVersion);
    d_->trackers
        = std::make_unique<service::TrackerService>(d_->swarmScraper.get(), d_->siteScraper.get(), d_->torrents.get());
    d_->migrations = std::make_unique<service::MigrationService>(
        dataDir, d_->database.get(), d_->torrents.get(), d_->config.get());
    d_->updates = std::make_unique<service::UpdateService>();

    // --- Front-ends -------------------------------------------------------
    // PeerApi is built before ApiRouter: the router's wireEvents() connects to
    // PeerApi::remoteSearchResults during construction, so PeerApi must exist.
    d_->peerApi = std::make_unique<peer::PeerApi>(this); // registers P2P handlers
    d_->apiRouter = std::make_unique<rest::ApiRouter>(this);
    d_->apiServer = std::make_unique<rest::ApiServer>(d_->apiRouter.get());
    d_->apiServer->setApplication(this);

    wireSignals();
    applyConfig();
}

Application::~Application()
{
    stop();
}

void Application::applyConfig()
{
    ConfigStore* c = d_->config.get();
    const bool network = d_->transport->isRunning();
    // A user may enable or disable session restore while the network is already
    // running. Keep shutdown persistence aligned with the current choice; never
    // load an old session as a side effect of changing this setting at runtime.
    if (d_->running && d_->transport->isRunning())
        d_->downloadSessionLoaded = c->resumeBtSession();

    d_->filter->setSettings(c->filterSettings());

    d_->downloads->setDefaultDownloadPath(c->downloadPath());
    d_->engine->setFileTransferPermissions(network && c->btFileDownloadEnabled(),
        network && c->btFileUploadEnabled());
    d_->downloads->enforceFileTransferPermissions();
    d_->trackers->setCountScrapingEnabled(network && c->trackersEnabled());
    d_->trackers->setInfoScrapingEnabled(network && c->trackersEnabled());
    d_->replication->setEnabled(network && c->p2pReplication());
    // Pulling a peer's index is an outbound, explicitly requested operation;
    // it does not grant permission to answer anyone else's index requests.
    d_->databaseSync->setPeerSyncEnabled(network);
    // One value drives both: what we actually do when asked, and what we tell
    // peers we would do. They must not disagree, or peers offer us to users who
    // will only ever get a refusal.
    const bool shareDatabase = network && c->indexExchangeEnabled()
        && d_->options.shareDatabase.value_or(c->databaseSharing());
    d_->databaseSync->setSharingEnabled(shareDatabase);
    d_->peers->setDatabaseSharing(shareDatabase);
    d_->peers->setIndexSharingEnabled(network && c->indexExchangeEnabled());
    {
        service::DatabaseSnapshot::Policy policy;
        policy.maxAgeSecs = static_cast<qint64>(c->databaseSnapshotMaxAgeHours()) * 3600;
        policy.maxDriftRatio = c->databaseSnapshotMaxDriftPercent() / 100.0;
        d_->databaseSync->setSnapshotPolicy(policy);
    }
    d_->transport->setPortMappingEnabled(c->upnpEnabled());
    d_->transport->setMdnsEnabled(c->mdnsEnabled());
    d_->transport->setHolePunchEnabled(c->holePunchEnabled());
    d_->transport->setRelayEnabled(c->relayEnabled());
    d_->transport->setRelayServeEnabled(c->relayServeEnabled());
    d_->crawler->setWalkInterval(c->spiderWalkInterval());
    // Re-derives the rotation threshold, so lowering the budget starts capping
    // the current file immediately instead of at the next launch.
    common::applyLogSizeBudget(c->logMaxSizeMb());
    d_->searchHistory->setEnabled(c->searchHistoryEnabled());
    d_->updates->setNetworkAllowed(network);

    // Replication remains configuration-driven. Crawler runtime control is
    // separate: saving the indexer preference cannot silently start it.
    if (d_->running && network) {
        if (c->p2pReplication())
            d_->replication->start();
        else
            d_->replication->stop();
    }
}

void Application::wireSignals()
{
    // Newly crawled torrents flow into the single insertion path.
    connect(d_->crawler.get(), &net::Crawler::discovered, d_->indexing.get(),
        [this](const domain::Torrent& t) {
            const auto result = d_->indexing->insert(t);
            if (result.success && (!result.alreadyExists || result.filesBackfilled))
                d_->crawler->recordIndexed();
            if (result.filesBackfilled)
                qInfo() << "[Crawler] file list backfilled for" << t.hash.left(8);
            else if (!result.success)
                qWarning() << "[Crawler] metadata indexing failed for" << t.hash.left(8) << result.error;
        });

    // Let the crawler skip a metadata fetch for torrents already in the index.
    d_->crawler->setKnownHashFilter([this](const QString& hash) {
        using Need = data::TorrentRepository::MetadataNeed;
        using Decision = net::Crawler::MetadataDecision;
        switch (d_->torrents->inspectMetadataNeed(hash)) {
        case Need::New:
        case Need::MissingFiles:
            return Decision::Fetch;
        case Need::Error:
            return Decision::Error;
        case Need::CompleteOrAmbiguous:
            return Decision::Skip;
        }
        return Decision::Error;
    });

    // A freshly indexed torrent triggers tracker scrapes.
    connect(d_->indexing.get(), &service::IndexingService::torrentIndexed, d_->trackers.get(),
        &service::TrackerService::onTorrentIndexed);

    // Keep our advertised peer stats in sync with the database totals.
    connect(d_->torrents.get(), &data::TorrentRepository::statisticsChanged, d_->peers.get(),
        &service::PeerRegistry::updateOurStats);

    // A vote surfaces the torrent into the feed.
    connect(d_->voting.get(), &service::VotingService::votesUpdated, d_->feed.get(),
        [this](const QString& hash, int, int) { d_->feed->addByHash(hash); });

    // Re-push config into the services whenever it changes.
    connect(d_->config.get(), &ConfigStore::configChanged, this, [this](const QStringList&) { applyConfig(); });

    // A language switch in the settings dialog swaps the installed translators.
    connect(d_->config.get(), &ConfigStore::languageChanged, this,
        [](const QString& code) { TranslationManager::instance().setLanguage(code); });
}

bool Application::start()
{
    if (d_->running)
        return true;

    qInfo() << "[Application] starting (dataDir:" << d_->options.dataDirectory << "headless:" << d_->options.headless
            << ")";

    if (!d_->manticore->start() || !d_->manticore->waitForReady()) {
        qCritical() << "[Application] database failed to start";
        return false;
    }
    d_->torrents->primeFromDatabase();

    // Blocking pre-start migrations run before anything reads the data. These are
    // housekeeping migrations, so a failure is non-fatal — but it must not pass
    // silently (the service logs the specific migration; we log the outcome).
    // They run on this thread, before any window exists: a front-end that wants
    // to show them (MigrationProgressWindow in GUI mode, a stdout bar in console
    // mode) connects to MigrationService::syncMigrationStarted before start() is
    // called and pumps the event queue itself. The async migrations below run
    // while the window is up and are surfaced in its status bar.
    if (!d_->migrations->runSyncMigrations()) {
        qWarning() << "[Application] one or more synchronous migrations failed; "
                      "continuing startup";
    }

    d_->feed->load();
    d_->running = true;
    if (d_->config->networkEnabled() && startNetwork() && d_->config->resumeBtSession()) {
        // Only the initial program startup may load a persisted BT session.
        d_->downloads->loadSession(d_->options.dataDirectory + QStringLiteral("/torrents_session.json"));
    }

    // Background (resumable) migrations.
    d_->migrations->startAsyncMigrations();

    // An explicit Web UI directory enables the HTTP API for headless deployments.
    if (d_->config->restApiEnabled() || !d_->options.webuiDir.isEmpty()) {
        if (!d_->options.webuiDir.isEmpty())
            d_->apiServer->setWebuiDir(d_->options.webuiDir);
        d_->apiServer->setLegacyWebuiDir(d_->options.legacyWebuiDir.isEmpty()
                ? QDir::currentPath() + QStringLiteral("/webui")
                : d_->options.legacyWebuiDir);
        d_->apiServer->setAuthorizedFileRoots(d_->options.apiFileRoots);
        if (!d_->apiServer->start(d_->config->httpPort(), -1,
                d_->options.apiBindAddress.isEmpty() ? QStringLiteral("127.0.0.1") : d_->options.apiBindAddress)) {
            qCritical() << "[Application] HTTP/WebSocket API failed to start";
            stopNetwork();
            d_->databaseSync->shutdown();
            d_->manticore->stop();
            d_->running = false;
            return false;
        }
    }

    return true;
}

void Application::stop()
{
    if (!d_->running)
        return;
    qInfo() << "[Application] stopping";

    d_->apiServer->stop();
    // Stop the crawler (the source of new torrents) and the tracker scrapers early
    // so no fresh tracker requests are issued during shutdown, and in-flight
    // announces / HTTP requests are drained rather than left blocking teardown.
    stopNetwork();
    d_->feed->save();
    // The sync worker reads/writes the database on its own thread; it has to be
    // gone before Manticore is torn down under it.
    d_->databaseSync->shutdown();
    d_->manticore->stop();

    d_->running = false;
}

bool Application::startNetwork()
{
    if (QThread::currentThread() != thread() || !d_->running ||
        d_->networkState == Private::NetworkState::Starting ||
        d_->networkState == Private::NetworkState::Stopping)
        return false;
    if (d_->transport->isRunning()) return true;

    d_->networkState = Private::NetworkState::Starting;
    d_->networkError.clear();
    d_->crawlerError.clear();
    // Preferences and CLI port overrides are read on every fresh Node creation.
    d_->transport->setListenOptions(
        d_->options.p2pPort > 0 ? d_->options.p2pPort : d_->config->p2pPort(),
        d_->options.dhtPort > 0 ? d_->options.dhtPort : d_->config->dhtPort(),
        d_->options.maxPeers > 0 ? d_->options.maxPeers : d_->config->p2pConnections());
    d_->transport->setIndependentDirect(d_->config->independentDirect());
    applyConfig();
    if (!d_->transport->start()) {
        d_->transport->stop();
        d_->networkError = d_->transport->lastStartError();
        if (d_->networkError.isEmpty()) d_->networkError = QStringLiteral("P2P transport failed to start");
        d_->networkState = Private::NetworkState::Failed;
        qWarning() << "[Application] network startup failed:" << d_->networkError;
        return false;
    }

    d_->networkState = Private::NetworkState::Running;
    applyConfig(); // permissions and sharing are applied before any task is attached
    if (d_->crawlerRunOverride.value_or(d_->config->indexerEnabled() || d_->options.forceSpider)) {
        const auto runtimeChoice = d_->crawlerRunOverride;
        startCrawler();
        d_->crawlerRunOverride = runtimeChoice;
    }
    if (!d_->downloads->reattachAfterNetworkStart())
        d_->networkError = QStringLiteral("Some paused BT tasks could not be reattached");
    d_->downloads->enableMonitoring();
    return true;
}

bool Application::stopNetwork()
{
    if (QThread::currentThread() != thread() ||
        d_->networkState == Private::NetworkState::Starting ||
        d_->networkState == Private::NetworkState::Stopping)
        return false;
    if (!d_->transport->isRunning()) {
        d_->networkState = Private::NetworkState::Stopped;
        return true;
    }
    d_->networkState = Private::NetworkState::Stopping;
    // Runtime crawler choices belong to this network run. The next run uses
    // the stored startup preference (and --spider, if supplied).
    d_->crawlerRunOverride.reset();
    d_->apiRouter->cancelRemoteSearches(QStringLiteral("P2P network stopping"));
    d_->crawler->stop();
    d_->trackers->stop();
    d_->replication->stop();
    d_->databaseSync->setSharingEnabled(false);
    d_->databaseSync->setPeerSyncEnabled(false);
    d_->peers->setDatabaseSharing(false);
    d_->peers->setIndexSharingEnabled(false);
    d_->databaseSync->stopNetworkTransfers();
    d_->downloads->suspendForNetworkStop();
    if (d_->downloadSessionLoaded)
        d_->downloads->saveSession(d_->options.dataDirectory + QStringLiteral("/torrents_session.json"));
    d_->downloadSessionLoaded = false;
    d_->engine->setFileTransferPermissions(false, false);
    d_->transport->stop();
    // A failed retry while metadata was draining is no longer an active
    // crawler failure once the Node has cancelled and drained its requests.
    d_->crawlerError.clear();
    d_->updates->setNetworkAllowed(false);
    d_->networkState = Private::NetworkState::Stopped;
    qInfo() << "[Application] network features stopped; local search remains available";
    return true;
}

bool Application::startCrawler()
{
    if (QThread::currentThread() != thread() || !d_->running) return false;
    d_->crawlerError.clear();
    if (!d_->transport->isRunning())
        d_->crawlerError = QStringLiteral("Start the shared network before DHT collection");
    else if (!d_->transport->isDhtRunning())
        d_->crawlerError = QStringLiteral("DHT socket is not running");
    else if (!d_->engine->isReady())
        d_->crawlerError = QStringLiteral("BT metadata engine is not ready");
    else if (d_->crawler->isStopping())
        d_->crawlerError = QStringLiteral("Previous metadata requests are still draining");
    else if (!d_->crawler->start())
        d_->crawlerError = QStringLiteral("DHT crawler failed to start");
    if (!d_->crawlerError.isEmpty()) return false;
    d_->crawlerRunOverride = true;
    return true;
}

bool Application::stopCrawler()
{
    if (QThread::currentThread() != thread() || !d_->running) return false;
    d_->crawlerRunOverride = false;
    d_->crawlerError.clear();
    d_->crawler->stop();
    return true;
}

QJsonObject Application::featureStatus() const
{
    const bool actual = d_->transport->isRunning();
    const auto serve = d_->databaseSync->serveStatus();
#ifdef Q_OS_MACOS
    const bool directSupported = true;
#else
    const bool directSupported = false;
#endif
    return QJsonObject {
        { "networkDesired", d_->config->networkEnabled() },
        { "networkRunning", actual },
        { "networkState", d_->networkState == Private::NetworkState::Starting ? "starting" :
            d_->networkState == Private::NetworkState::Stopping ? "stopping" :
            d_->networkState == Private::NetworkState::Failed ? "failed" :
            actual ? "running" : "stopped" },
        { "restartRequired", actual &&
            ((directSupported && d_->config->independentDirect() != d_->transport->independentDirectActive()) ||
                d_->config->mdnsEnabled() != d_->transport->isMdnsRunning()) },
        { "dhtRunning", d_->transport->isDhtRunning() },
        { "independentDirectSupported", directSupported },
        { "independentDirectDesired", d_->config->independentDirect() },
        { "independentDirectSocketBound", actual && d_->transport->independentDirectActive() },
        { "mdnsRunning", d_->transport->isMdnsRunning() },
        { "p2pRunning", actual },
        { "btEngineReady", d_->engine->isReady() },
        { "btFileDownloadAllowed", actual && d_->engine->fileDownloadAllowed() },
        { "btFileUploadAllowed", actual && d_->engine->fileUploadAllowed() },
        { "btSessionRestoreDesired", d_->config->resumeBtSession() },
        { "crawlerRunning", d_->crawler->isRunning() },
        { "crawlerState", d_->crawler->isRunning() ? "running" :
            d_->crawler->isStopping() ? "stopping" :
            !d_->crawlerError.isEmpty() ? "failed" : "stopped" },
        { "crawlerPendingFetches", d_->crawler->pendingFetches() },
        { "crawlerConfigured", d_->config->indexerEnabled() },
        { "crawlerForcedByCli", d_->options.forceSpider },
        { "crawlerOverrideActive", d_->crawlerRunOverride.has_value() },
        { "replicationRunning", d_->replication->isRunning() },
        { "databaseSharingRunning", d_->databaseSync->sharingEnabled() },
        { "indexExchangeAllowed", d_->config->indexExchangeEnabled() },
        { "indexExchangeRunning", actual && d_->config->indexExchangeEnabled() },
        { "indexSharingDesired", d_->config->indexExchangeEnabled() },
        { "indexSharingRunning", d_->peers->indexSharingEnabled() },
        { "indexSharingServing", serve.generating || serve.waiting > 0 || serve.sending > 0 },
        { "remoteSearchDesired", d_->config->remoteSearchEnabled() },
        { "remoteSearchRunning", actual && d_->config->remoteSearchEnabled() },
        { "peerCount", d_->transport->peerCount() },
        { "lastError", d_->networkError },
        { "crawlerLastError", d_->crawlerError },
    };
}

const Application::Options& Application::options() const
{
    return d_->options;
}

ConfigStore* Application::config() const
{
    return d_->config.get();
}
FavoritesStore* Application::favorites() const
{
    return d_->favorites.get();
}
SearchHistoryStore* Application::searchHistory() const
{
    return d_->searchHistory.get();
}
data::TorrentRepository* Application::torrents() const
{
    return d_->torrents.get();
}
net::P2PTransport* Application::transport() const
{
    return d_->transport.get();
}
net::TorrentEngine* Application::engine() const
{
    return d_->engine.get();
}
net::Crawler* Application::crawler() const
{
    return d_->crawler.get();
}
service::IndexingService* Application::indexing() const
{
    return d_->indexing.get();
}
service::SearchService* Application::search() const
{
    return d_->search.get();
}
service::DownloadService* Application::downloads() const
{
    return d_->downloads.get();
}
service::TorrentCreator* Application::creator() const
{
    return d_->creator.get();
}

service::TorrentExporter* Application::exporter() const
{
    return d_->exporter.get();
}
service::FeedService* Application::feed() const
{
    return d_->feed.get();
}
service::VotingService* Application::voting() const
{
    return d_->voting.get();
}
service::ReplicationService* Application::replication() const
{
    return d_->replication.get();
}
service::DatabaseSyncService* Application::databaseSync() const
{
    return d_->databaseSync.get();
}
service::TrackerService* Application::trackers() const
{
    return d_->trackers.get();
}
service::PeerRegistry* Application::peers() const
{
    return d_->peers.get();
}
service::UpdateService* Application::updates() const
{
    return d_->updates.get();
}
service::MigrationService* Application::migrations() const
{
    return d_->migrations.get();
}
rest::ApiRouter* Application::api() const
{
    return d_->apiRouter.get();
}
peer::PeerApi* Application::peerApi() const
{
    return d_->peerApi.get();
}

} // namespace rats::app
