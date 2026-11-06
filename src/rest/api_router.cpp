#include "rest/api_router.h"

#include "app/application.h"
#include "app/config_store.h"
#include "app/search_history_store.h"
#include "app/watch_store.h"
#include "common/infohash.h"
#include "data/torrent_repository.h"
#include "domain/peer.h"
#include "domain/torrent_codec.h"
#include "net/librats_convert.h"
#include "net/crawler.h"
#include "net/p2p_transport.h"
#include "net/torrent_engine.h"
#include "peer/peer_api.h"
#include "services/database_sync_service.h"
#include "services/download_service.h"
#include "services/feed_service.h"
#include "services/filter_policy.h"
#include "services/indexing_service.h"
#include "services/peer_registry.h"
#include "services/search_service.h"
#include "services/torrent_creator.h"
#include "services/torrent_exporter.h"
#include "services/tracker_service.h"
#include "services/update_service.h"
#include "services/voting_service.h"

// The DHT metadata fallback reaches straight into the librats BitTorrent
// subsystem, whose headers expose an EventBus emit() that collides with Qt's
// `emit` keyword macro. Neutralise the macros across the librats includes, then
// restore them so the router's own `emit event(...)` keeps compiling.
#ifdef RATS_SEARCH_FEATURES
#pragma push_macro("emit")
#pragma push_macro("slots")
#pragma push_macro("signals")
#undef emit
#undef slots
#undef signals
#include "librats/bittorrent/torrent_info.h"
#include "librats/subsystems/bittorrent.h"
#pragma pop_macro("signals")
#pragma pop_macro("slots")
#pragma pop_macro("emit")
#endif

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonValue>
#include <QRegularExpression>
#include <QStringList>
#include <QTimer>
#include <QUuid>
#include <memory>

namespace rats::rest {

namespace {

// --- Parameter helpers ------------------------------------------------------

// Query text may arrive as a JSON string or, when a numeric-looking query like
// "1987" is parsed by the HTTP layer, as a JSON number — toVariant().toString()
// handles both. Accepts either "text" or "query".
QString queryText(const QJsonObject& params)
{
    if (params.contains("text"))
        return params["text"].toVariant().toString();
    return params["query"].toVariant().toString();
}

QStringList jsonToStringList(const QJsonValue& value)
{
    QStringList list;
    for (const QJsonValue& v : value.toArray())
        list.append(v.toString());
    return list;
}

service::SearchService::Request buildSearchRequest(const QJsonObject& params)
{
    service::SearchService::Request req;
    req.query = queryText(params);
    req.offset = params.contains("offset") ? params["offset"].toInt(0) : params["index"].toInt(0);
    req.limit = params["limit"].toInt(10);
    req.sort = params.contains("sort") ? params["sort"].toString() : params["orderBy"].toString();
    req.descending = params["orderDesc"].toBool(true);
    req.safeSearch = params["safeSearch"].toBool(false);
    req.contentType = params.contains("contentType") ? params["contentType"].toString() : params["type"].toString();

    const QJsonObject size = params["size"].toObject();
    if (!size.isEmpty()) {
        req.sizeMin = size["min"].toVariant().toLongLong();
        req.sizeMax = size["max"].toVariant().toLongLong();
    }
    const QJsonObject files = params["files"].toObject();
    if (!files.isEmpty()) {
        req.filesMin = files["min"].toInt();
        req.filesMax = files["max"].toInt();
    }
    return req;
}

// --- DHT metadata fallback --------------------------------------------------

// Serialise a stored/fetched torrent for a DHT hit and tag it `fromDHT`.
QJsonObject dhtTorrentJson(const domain::Torrent& torrent, bool includeFiles)
{
    QJsonObject obj = domain::codec::toJson(torrent, { includeFiles });
    obj["fromDHT"] = true;
    return obj;
}

// Ask librats to fetch a torrent's metadata over the DHT/BEP 9, index the
// result, and answer `respond` exactly once. `asArray` shapes the payload:
// search.torrents wants an array (empty on miss), torrent.get wants an object
// (failure on miss). Always marshals back onto the router's thread so the index
// insert and the response happen there. Both search.torrents and torrent.get
// share this one async source-combine.
void dhtLookup(ApiRouter* ctx, app::Application* app, const QString& hash, bool includeFiles, bool asArray,
    const ResultCallback& respond)
{
    auto miss = [asArray, respond]() {
        if (asArray)
            respond(Result::success(QJsonArray()));
        else
            respond(Result::failure("Torrent not found"));
    };

#ifdef RATS_SEARCH_FEATURES
    net::P2PTransport* transport = app->transport();
    librats::Bittorrent* bt = transport ? transport->bittorrent() : nullptr;
    if (!transport || !transport->isBitTorrentEnabled() || !bt || !bt->is_running()) {
        miss();
        return;
    }

    bt->get_torrent_metadata(hash.toStdString(),
        [ctx, app, hash, includeFiles, asArray, respond, miss](
            const librats::bittorrent::TorrentInfo& info, bool success, const std::string& error) {
            Q_UNUSED(error);
            if (!success || !info.is_valid()) {
                QMetaObject::invokeMethod(ctx, [miss]() { miss(); }, Qt::QueuedConnection);
                return;
            }

            // Build the domain entity on this worker thread, then hop to the
            // router thread to touch the index and reply.
            domain::Torrent torrent = net::toDomainTorrent(hash, info);
            QMetaObject::invokeMethod(
                ctx,
                [app, torrent, includeFiles, asArray, respond]() {
                    service::IndexingService::Result inserted = app->indexing()->insert(torrent);
                    const domain::Torrent& stored
                        = (inserted.success || inserted.alreadyExists) ? inserted.torrent : torrent;
                    QJsonObject obj = dhtTorrentJson(stored, includeFiles);
                    if (asArray) {
                        QJsonArray arr;
                        arr.append(obj);
                        respond(Result::success(arr));
                    } else {
                        respond(Result::success(obj));
                    }
                },
                Qt::QueuedConnection);
        });
#else
    Q_UNUSED(ctx);
    Q_UNUSED(app);
    Q_UNUSED(hash);
    Q_UNUSED(includeFiles);
    miss();
#endif
}

// --- Peer torrent fetch -----------------------------------------------------

// Ask a specific peer for a torrent (optionally with its file list) and answer
// `respond` exactly once: with the peer's reply if it arrives before the
// timeout, otherwise by falling back to the DHT lookup. This is what turns a
// remote-only search hit into a full, locally-cloned torrent when the user opens
// it — PeerApi already routes the incoming torrent_response through the index.
void peerLookup(ApiRouter* ctx, app::Application* app, const QString& peerId, const QString& hash, bool includeFiles,
    bool asArray, const ResultCallback& respond)
{
    peer::PeerApi* peerApi = app->peerApi();
    if (!peerApi || peerId.isEmpty() || !app->config()->remoteSearchEnabled()) {
        dhtLookup(ctx, app, hash, includeFiles, asArray, respond);
        return;
    }

    auto done = std::make_shared<bool>(false);
    auto conn = std::make_shared<QMetaObject::Connection>();
    QTimer* timer = new QTimer(ctx);
    timer->setSingleShot(true);

    // Complete once: either the peer answered (deliver its payload) or we time
    // out (fall back to the DHT). Every path disconnects and reaps the timer.
    auto finish = [=](bool matched, const QJsonObject& payload) {
        if (*done)
            return;
        *done = true;
        QObject::disconnect(*conn);
        timer->stop();
        timer->deleteLater();

        if (!matched) {
            dhtLookup(ctx, app, hash, includeFiles, asArray, respond);
            return;
        }
        if (asArray)
            respond(Result::success(QJsonArray { payload }));
        else
            respond(Result::success(payload));
    };

    *conn = QObject::connect(
        peerApi, &peer::PeerApi::remoteTorrentReceived, ctx, [=](const QString& h, const QJsonObject& data) {
            if (h == hash)
                finish(true, data);
        });
    QObject::connect(timer, &QTimer::timeout, ctx, [=]() { finish(false, {}); });

    timer->start(15000);
    peerApi->requestTorrent(peerId, hash, includeFiles);
}

} // namespace

// ===========================================================================
// ApiRouter
// ===========================================================================

struct RemoteSearchSession {
    QTimer* timer = nullptr;
    ResultCallback respond;
    QString query;
    QJsonArray results;
    QHash<QString, int> positions;
    QJsonArray candidates;
    QHash<QString, int> candidatePositions;
    int peers = 0;
};

namespace {

constexpr int remoteResultLimit = 100;

QJsonObject safeRemoteHit(const QJsonObject& raw)
{
    const QString hash = infohash::normalize(raw["hash"].toString());
    const QString name = raw["name"].toString().trimmed();
    const QString peer = raw["peer"].toString();
    if (!infohash::isValid(hash) || name.isEmpty() || peer.isEmpty()) return {};
    QJsonObject item {
        { "hash", hash }, { "name", name.left(512) },
        { "peer", peer }, { "remote", true },
        { "isFileMatch", raw["isFileMatch"].toBool() },
        { "size", qMax<qint64>(0, raw["size"].toVariant().toLongLong()) },
        { "files", qMax(0, raw["files"].toInt()) },
        { "seeders", qMax(0, raw["seeders"].toInt()) },
        { "leechers", qMax(0, raw["leechers"].toInt()) },
    };
    const QJsonArray rawPaths = raw.contains("matchingPaths")
        ? raw["matchingPaths"].toArray() : raw["path"].toArray();
    QJsonArray paths;
    for (int i = 0; i < qMin(rawPaths.size(), 12); ++i) {
        const QString path = rawPaths.at(i).toString().left(256);
        if (!path.isEmpty()) paths.append(path);
    }
    if (!paths.isEmpty()) item["matchingPaths"] = paths;
    return item;
}

bool plausibleLegacyHit(const QJsonObject& raw, const QString& query)
{
    // This is a display heuristic, never a protocol-level association.
    QString searchable = raw["name"].toString().left(512);
    const QJsonArray paths = raw.contains("matchingPaths")
        ? raw["matchingPaths"].toArray() : raw["path"].toArray();
    for (int i = 0; i < qMin(paths.size(), 12); ++i)
        searchable += QLatin1Char(' ') + paths.at(i).toString().left(256);
    const QStringList words = query.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    for (const QString& word : words) {
        if (!searchable.contains(word, Qt::CaseInsensitive)) return false;
    }
    return !words.isEmpty();
}

bool insertRemoteHit(QJsonArray& results, QHash<QString, int>& positions, const QJsonObject& item)
{
    const QString hash = item["hash"].toString();
    auto existing = positions.constFind(hash);
    if (existing != positions.constEnd()) {
        // Prefer the torrent-name match over a file-only match.
        if (results.at(existing.value()).toObject()["isFileMatch"].toBool()
            && !item["isFileMatch"].toBool()) {
            results.replace(existing.value(), item);
            return true;
        }
        return false;
    }
    if (results.size() >= remoteResultLimit) return false;
    positions.insert(hash, results.size());
    results.append(item);
    return true;
}

} // namespace

QJsonArray ApiRouter::collectRemoteSearchResults(const QString& searchId, const QJsonArray& torrents)
{
    if (!app_->config()->remoteSearchEnabled()) return {};
    auto session = remoteSearches_.value(searchId);
    if (!session) return {};

    QJsonArray accepted;
    for (const QJsonValue& value : torrents) {
        const QJsonObject item = safeRemoteHit(value.toObject());
        if (!item.isEmpty() && insertRemoteHit(session->results, session->positions, item))
            accepted.append(item);
    }
    return accepted;
}

QHash<QString, QJsonArray> ApiRouter::collectLegacySearchCandidates(const QJsonArray& torrents)
{
    if (!app_->config()->remoteSearchEnabled() || remoteSearches_.isEmpty()) return {};
    QHash<QString, QJsonArray> accepted;
    for (const QJsonValue& value : torrents) {
        const QJsonObject raw = value.toObject();
        const QJsonObject item = safeRemoteHit(raw);
        if (item.isEmpty()) continue;
        for (auto it = remoteSearches_.cbegin(); it != remoteSearches_.cend(); ++it) {
            const auto& session = it.value();
            if (plausibleLegacyHit(raw, session->query)
                && insertRemoteHit(session->candidates, session->candidatePositions, item))
                accepted[it.key()].append(item);
        }
    }
    return accepted;
}

bool ApiRouter::finishRemoteSearch(const QString& searchId, const QString& error)
{
    auto session = remoteSearches_.take(searchId);
    if (!session) return false;
    session->timer->stop();
    session->timer->deleteLater();
    if (!error.isEmpty()) {
        session->respond(Result::failure(error));
    } else {
        session->respond(Result::success(QJsonObject {
            { "items", session->results }, { "candidates", session->candidates },
            { "peers", session->peers }, { "windowExpired", true } }));
    }
    return true;
}

void ApiRouter::cancelRemoteSearches(const QString& reason)
{
    for (const QString& id : remoteSearches_.keys())
        finishRemoteSearch(id, reason);
}

ApiRouter::ApiRouter(app::Application* app, QObject* parent) : QObject(parent), app_(app)
{
    registerMethods();
    wireEvents();
}

void ApiRouter::wireEvents()
{
    // Bridge service-layer Qt signals to the unified WS event channel. The
    // ApiServer forwards every `event(name, data)` to all connected WebSocket
    // clients, so this restores the live push stream (download progress, votes,
    // feed, config, indexing, remote search) that the desktop GUI already gets
    // through direct signal connections. Event names and payload shapes are a
    // published contract — changing one breaks existing API clients.
    connect(app_->downloads(), &service::DownloadService::progressUpdated, this,
        [this](const QString& hash, const QJsonObject& progress) {
            QJsonObject data = progress;
            data["hash"] = hash;
            emit event(QStringLiteral("downloadProgress"), data);
        });
    connect(app_->downloads(), &service::DownloadService::downloadCompleted, this, [this](const QString& hash) {
        emit event(QStringLiteral("downloadCompleted"), QJsonObject { { "hash", hash }, { "cancelled", false } });
    });
    connect(app_->downloads(), &service::DownloadService::filesReady, this,
        [this](const QString& hash, const QJsonArray& files) {
            emit event(QStringLiteral("filesReady"), QJsonObject { { "hash", hash }, { "files", files } });
        });
    connect(app_->watch(), &app::WatchStore::changed, this, [this] { emit event("watchChanged", {}); });
    auto* window = new QTimer(this);
    window->setSingleShot(true);
    window->setInterval(1000);
    connect(window, &QTimer::timeout, this, [this] {
        if (queuedOmitted_ > 0)
            emit event("trawlOmitted", QJsonObject{{"count", queuedOmitted_}});
        queuedShown_ = queuedOmitted_ = 0;
    });
    using Source = net::Crawler::DiscoverySource;
    connect(app_->crawler(), &net::Crawler::hashQueued, this,
        [this, window](const QString& hash, Source source, bool hasPeer) {
            if (!window->isActive()) window->start();
            if (queuedShown_ >= 12) { ++queuedOmitted_; return; }
            ++queuedShown_;
            const QString origin = source == Source::Sample ? "BEP 51"
                : source == Source::GetPeers ? "get_peers" : "announce_peer";
            emit event("trawlQueued", QJsonObject{{"hash", hash.left(12)}, {"source", origin}, {"hasPeer", hasPeer}});
        });
    connect(app_->crawler(), &net::Crawler::metadataStarted, this,
        [this](const QString& hash, bool hasPeer) {
            emit event("trawlStarted", QJsonObject{{"hash", hash.left(12)}, {"hasPeer", hasPeer}});
        });
    connect(app_->crawler(), &net::Crawler::metadataEnded, this,
        [this](const QString& hash, const QString& outcome) {
            emit event("trawlMetadata", QJsonObject{{"hash", hash.left(12)}, {"outcome", outcome}});
        });
    connect(app_->crawler(), &net::Crawler::statusChanged, this, [this](const QString& status) {
        emit event("trawlState", QJsonObject{{"status", status}});
    });
    connect(app_->indexing(), &service::IndexingService::filesBackfilled, this, [this](const domain::Torrent& t) {
        emit event("torrentBackfilled", QJsonObject{{"hash", t.hash}, {"name", t.name},
            {"size", t.size}, {"files", t.files}});
    });
    connect(app_->indexing(), &service::IndexingService::torrentIndexed, this, [this](const domain::Torrent& torrent) {
        emit event(QStringLiteral("torrentIndexed"), QJsonObject {
            { "hash", torrent.hash },
            { "name", torrent.name },
            { "size", torrent.size },
            { "files", torrent.files },
            { "seeders", torrent.seeders },
            { "leechers", torrent.leechers },
            { "contentType", domain::toString(torrent.contentType) },
            { "contentCategory", domain::toString(torrent.contentCategory) },
        });
    });
    connect(
        app_->voting(), &service::VotingService::votesUpdated, this, [this](const QString& hash, int good, int bad) {
            emit event(
                QStringLiteral("votesUpdated"), QJsonObject { { "hash", hash }, { "good", good }, { "bad", bad } });
        });
    connect(app_->feed(), &service::FeedService::feedUpdated, this, [this]() {
        emit event(QStringLiteral("feedUpdated"), QJsonObject { { "feed", app_->feed()->toJsonArray() } });
    });
    connect(app_->config(), &app::ConfigStore::configChanged, this, [this](const QStringList& changedKeys) {
        if (!app_->config()->remoteSearchEnabled()) {
            for (const QString& id : remoteSearches_.keys())
                finishRemoteSearch(id, QStringLiteral("Remote search permission disabled"));
        }
        QJsonObject data = app_->config()->toJson();
        data["changedKeys"] = QJsonArray::fromStringList(changedKeys);
        emit event(QStringLiteral("configChanged"), data);
    });
    if (app_->peerApi()) {
        const auto routeSearchHits = [this](const QString& searchId, const QJsonArray& torrents) {
            if (searchId.isEmpty()) {
                const auto candidates = collectLegacySearchCandidates(torrents);
                for (auto it = candidates.cbegin(); it != candidates.cend(); ++it)
                    emit event(QStringLiteral("remoteSearchCandidates"),
                        QJsonObject { { "candidateSessionId", it.key() }, { "torrents", it.value() } });
                return;
            }
            const QJsonArray confirmed = collectRemoteSearchResults(searchId, torrents);
            if (!confirmed.isEmpty())
                emit event(QStringLiteral("remoteSearchResults"),
                    QJsonObject { { "searchId", searchId }, { "torrents", confirmed } });
        };
        connect(app_->peerApi(), &peer::PeerApi::remoteSearchResults, this, routeSearchHits);
        connect(app_->peerApi(), &peer::PeerApi::remoteFileSearchResults, this, routeSearchHits);
    }
    connect(app_->transport(), &net::P2PTransport::stopped, this, [this]() {
        for (const QString& id : remoteSearches_.keys())
            finishRemoteSearch(id, QStringLiteral("P2P network stopped"));
    });
    if (app_->databaseSync()) {
        // Two lanes, two event families. databaseSync* is the local user's own
        // operation; databaseServe* is what we are doing for other peers, which a
        // client may display but must never treat as the user's own failure.
        connect(app_->databaseSync(), &service::DatabaseSyncService::syncStarted, this,
            [this](const QJsonObject& info) { emit event(QStringLiteral("databaseSyncStarted"), info); });
        connect(app_->databaseSync(), &service::DatabaseSyncService::syncProgress, this,
            [this](const QJsonObject& info) { emit event(QStringLiteral("databaseSyncProgress"), info); });
        connect(app_->databaseSync(), &service::DatabaseSyncService::syncFinished, this,
            [this](bool, const QJsonObject& summary) { emit event(QStringLiteral("databaseSyncFinished"), summary); });
        connect(app_->databaseSync(), &service::DatabaseSyncService::serveProgress, this,
            [this](const QJsonObject& info) { emit event(QStringLiteral("databaseServeProgress"), info); });
        connect(app_->databaseSync(), &service::DatabaseSyncService::serveFinished, this,
            [this](const QString& peerId, bool success, const QJsonObject& summary) {
                QJsonObject info = summary;
                info["peer"] = peerId;
                info["success"] = success;
                emit event(QStringLiteral("databaseServeFinished"), info);
            });
    }
}

void ApiRouter::add(const QString& name, Handler handler)
{
    handlers_.insert(name, std::move(handler));
}

void ApiRouter::call(const QString& method, const QJsonObject& params, const ResultCallback& respond)
{
    auto it = handlers_.constFind(method);
    if (it == handlers_.constEnd()) {
        respond(Result::failure(QStringLiteral("Unknown method: %1").arg(method)));
        return;
    }
    it.value()(params, respond);
}

void ApiRouter::registerMethods()
{
    // -----------------------------------------------------------------------
    // Search
    // -----------------------------------------------------------------------
    add("search.remote", [this](const QJsonObject& params, const ResultCallback& respond) {
        net::P2PTransport* transport = app_->transport();
        if (!app_->config()->remoteSearchEnabled()) {
            respond(Result::failure("Remote search permission disabled"));
            return;
        }
        if (!transport || !transport->isRunning()) {
            respond(Result::failure("P2P network unavailable"));
            return;
        }
        if (transport->peerCount() == 0) {
            respond(Result::failure("No connected Rats peers"));
            return;
        }
        const QString query = queryText(params).trimmed();
        if (query.size() < 3 || query.size() > 200) {
            respond(Result::failure("Remote search query must be 3-200 characters"));
            return;
        }
        const QString searchId = params["searchId"].toString();
        if (searchId.size() > 64 || QUuid(searchId).isNull() || remoteSearches_.contains(searchId)) {
            respond(Result::failure("Invalid or duplicate remote search ID"));
            return;
        }
        if (remoteSearches_.size() >= 8) {
            respond(Result::failure("Too many remote searches in progress"));
            return;
        }

        auto session = std::make_shared<RemoteSearchSession>();
        session->respond = respond;
        session->query = query;
        session->peers = transport->peerCount();
        session->timer = new QTimer(this);
        session->timer->setSingleShot(true);
        connect(session->timer, &QTimer::timeout, this,
            [this, searchId]() { finishRemoteSearch(searchId); });
        remoteSearches_.insert(searchId, session);
        session->timer->start(5000);

        QJsonObject message {
            { "searchId", searchId }, { "query", query }, { "text", query },
            { "limit", qBound(1, params["limit"].toInt(50), 50) },
            { "orderBy", params["sort"].toString() },
            { "orderDesc", params["orderDesc"].toBool(true) },
            { "safeSearch", params["safeSearch"].toBool(false) },
            { "type", params["contentType"].toString() }
        };
        if (params.contains("size")) message["size"] = params["size"];
        if (params.contains("files")) message["files"] = params["files"];
        const int sent = transport->broadcastMessage("searchTorrent", message);
        if (sent > 0) transport->broadcastMessage("searchFiles", message);
        else finishRemoteSearch(searchId, QStringLiteral("No connected Rats peers"));
    });

    add("search.cancel", [this](const QJsonObject& params, const ResultCallback& respond) {
        const bool cancelled = finishRemoteSearch(params["searchId"].toString(),
            QStringLiteral("Remote search cancelled"));
        respond(Result::success(QJsonObject { { "cancelled", cancelled } }));
    });

    add("search.torrents", [this](const QJsonObject& params, const ResultCallback& respond) {
        const service::SearchService::Request req = buildSearchRequest(params);
        // A torrent search through the API is a user-initiated search, so it is
        // remembered exactly like one typed into the GUI (the store itself
        // honours the searchHistory setting). search.files is deliberately not
        // recorded: a client running both for one query would count it twice.
        app_->searchHistory()->add(req.query);
        const QVector<domain::SearchHit> hits = app_->search()->searchTorrents(req);

        QJsonArray results;
        for (const domain::SearchHit& hit : hits)
            results.append(domain::codec::toJson(hit));

        // An info-hash query (bare hash or magnet link) with no local hit falls
        // back to a DHT lookup on the extracted, normalized hash.
        const QString dhtHash = service::SearchService::extractInfoHash(req.query);
        if (hits.isEmpty() && !dhtHash.isEmpty() && !params["localOnly"].toBool(false)) {
            dhtLookup(this, app_, dhtHash, /*includeFiles*/ false, /*asArray*/ true, respond);
            return;
        }
        respond(Result::success(results));
    });

    add("search.files", [this](const QJsonObject& params, const ResultCallback& respond) {
        const service::SearchService::Request req = buildSearchRequest(params);
        if (req.query.length() <= 2) {
            respond(Result::failure("Query too short"));
            return;
        }
        const QVector<domain::SearchHit> hits = app_->search()->searchFiles(req);

        QJsonArray results;
        for (const domain::SearchHit& hit : hits)
            results.append(domain::codec::toJson(hit, { /*includeFiles*/ true }));
        respond(Result::success(results));
    });

    add("search.top", [this](const QJsonObject& params, const ResultCallback& respond) {
        const QString type = params["type"].toString();
        const QString time = params["time"].toString();
        const int offset = params["index"].toInt(0);
        const int limit = params["limit"].toInt(20);
        const QVector<domain::Torrent> torrents = app_->search()->top(type, time, offset, limit);

        QJsonArray results;
        for (const domain::Torrent& t : torrents)
            results.append(domain::codec::toJson(t));
        respond(Result::success(results));
    });

    add("search.recent", [this](const QJsonObject& params, const ResultCallback& respond) {
        const int limit = params["limit"].toInt(10);
        const QVector<domain::Torrent> torrents = app_->search()->recent(limit);

        QJsonArray results;
        for (const domain::Torrent& t : torrents)
            results.append(domain::codec::toJson(t));
        respond(Result::success(results));
    });

    // -----------------------------------------------------------------------
    // Watch and search history
    // -----------------------------------------------------------------------
    for (const QString method : {"watch.list", "watch.unread", "watch.add", "watch.enable", "watch.delete", "watch.read"}) {
        add(method, [this, method](const QJsonObject& params, const ResultCallback& respond) {
            app_->watch()->request(method, params, respond);
        });
    }

    add("search.history", [this](const QJsonObject& params, const ResultCallback& respond) {
        const int limit = params["limit"].toInt(app::SearchHistoryStore::kMaxEntries);

        QJsonArray results;
        for (const app::SearchHistoryStore::Entry& entry : app_->searchHistory()->entries()) {
            if (limit >= 0 && results.size() >= limit)
                break;
            results.append(QJsonObject { { "query", entry.query },
                { "lastSearchedAt", entry.lastSearchedAt.toMSecsSinceEpoch() }, { "count", entry.count } });
        }
        respond(Result::success(results));
    });

    add("search.history.remove", [this](const QJsonObject& params, const ResultCallback& respond) {
        const QString query = params["query"].toString();
        if (query.trimmed().isEmpty()) {
            respond(Result::failure("Missing query"));
            return;
        }
        const bool removed = app_->searchHistory()->remove(query);
        respond(Result::success(QJsonObject { { "removed", removed } }));
    });

    add("search.history.clear", [this](const QJsonObject& /*params*/, const ResultCallback& respond) {
        const bool cleared = app_->searchHistory()->clear();
        respond(Result::success(QJsonObject { { "cleared", cleared } }));
    });

    // -----------------------------------------------------------------------
    // Torrent lifecycle
    // -----------------------------------------------------------------------
    add("torrent.get", [this](const QJsonObject& params, const ResultCallback& respond) {
        const QString hash = infohash::normalize(params["hash"].toString());
        if (!infohash::isValid(hash)) {
            respond(Result::failure("Invalid hash"));
            return;
        }
        const bool includeFiles = params["files"].toBool(false);
        // Optional: the peer that offered this torrent in a remote search hit.
        const QString peerId = params["peer"].toString();

        std::optional<domain::Torrent> torrent = app_->search()->get(hash, includeFiles);

        // A file list was asked for but the local copy has none: this is a
        // remote-only hit (search replies never carry files). Fetch the full
        // torrent from the offering peer, then the DHT, before giving up.
        const bool needRemoteFiles = includeFiles && (!torrent || torrent->fileList.isEmpty());
        if (needRemoteFiles && !peerId.isEmpty()) {
            peerLookup(this, app_, peerId, hash, includeFiles, /*asArray*/ false, respond);
            return;
        }

        if (!torrent) {
            // Not indexed locally and no peer to ask: try the DHT.
            dhtLookup(this, app_, hash, includeFiles, /*asArray*/ false, respond);
            return;
        }

        QJsonObject obj = domain::codec::toJson(*torrent, { includeFiles });
        if (app_->downloads()->isDownloading(hash))
            obj["download"] = app_->downloads()->getDownload(hash).toJson();
        respond(Result::success(obj));
    });

    add("torrent.remove", [this](const QJsonObject& params, const ResultCallback& respond) {
        const QJsonArray hashes = params["hashes"].toArray();
        if (hashes.isEmpty()) {
            respond(Result::failure("No hashes provided"));
            return;
        }

        const int total = hashes.size();
        int removed = 0;
        int processed = 0;
        for (const QJsonValue& value : hashes) {
            const QString hash = infohash::normalize(value.toString());
            ++processed;
            if (infohash::isValid(hash) && app_->torrents()->remove(hash))
                ++removed;

            if (processed % 100 == 0 || processed == total) {
                QJsonObject progress;
                progress["processed"] = processed;
                progress["removed"] = removed;
                progress["total"] = total;
                emit event("torrentRemoveProgress", progress);
            }
        }

        QJsonObject result;
        result["total"] = total;
        result["removed"] = removed;
        respond(Result::success(result));
    });

    // Re-apply the filter policy across the whole index and remove torrents that
    // no longer pass (e.g. after tightening the adult/size filters). With
    // dryRun=true it only counts; otherwise it removes and emits progress.
    // An optional "filters" object overrides the stored config key by key, so
    // the settings dialog can preview the rules the user is still editing
    // without persisting them first.
    add("torrent.cleanup", [this](const QJsonObject& params, const ResultCallback& respond) {
        const bool dryRun = params["dryRun"].toBool(false);

        service::FilterSettings fs = app_->config()->filterSettings();
        const QJsonObject overrides = params["filters"].toObject();
        if (!overrides.isEmpty()) {
            if (overrides.contains("maxFiles"))
                fs.maxFiles = overrides["maxFiles"].toInt();
            if (overrides.contains("sizeMin"))
                fs.sizeMin = static_cast<qint64>(overrides["sizeMin"].toDouble());
            if (overrides.contains("sizeMax"))
                fs.sizeMax = static_cast<qint64>(overrides["sizeMax"].toDouble());
            if (overrides.contains("adultFilter"))
                fs.adultFilter = overrides["adultFilter"].toBool();
            if (overrides.contains("namingRegExp"))
                fs.namingRegExp = overrides["namingRegExp"].toString();
            if (overrides.contains("namingRegExpNegative"))
                fs.namingRegExpNegative = overrides["namingRegExpNegative"].toBool();
            if (overrides.contains("contentType"))
                fs.contentTypeFilter = overrides["contentType"].toString();
        }

        // An unparsable pattern would silently accept every name (FilterPolicy
        // skips an invalid regex), which reads as "the filter does nothing".
        if (!fs.namingRegExp.isEmpty()) {
            const QRegularExpression re(fs.namingRegExp);
            if (!re.isValid()) {
                respond(Result::failure(QStringLiteral("Invalid name filter regex: %1").arg(re.errorString())));
                return;
            }
        }

        const service::FilterPolicy policy(fs);
        constexpr int kBatch = 500;
        const qint64 total = app_->torrents()->statistics().torrents;
        int scanned = 0;
        int matched = 0;
        // Keyset pagination: OFFSET cannot walk past Manticore's max_matches
        // (1000), and removing rows mid-sweep would shift the rest into offsets
        // already passed.
        qint64 afterId = 0;
        for (;;) {
            const QVector<domain::Torrent> batch = app_->torrents()->pageAfterId(afterId, kBatch);
            if (batch.isEmpty())
                break;
            afterId = batch.last().id;
            for (const domain::Torrent& t : batch) {
                ++scanned;
                if (!policy.accepts(t)) {
                    ++matched;
                    if (!dryRun)
                        app_->torrents()->remove(t.hash);
                }
            }
            emit event("torrentCleanupProgress",
                QJsonObject {
                    { "scanned", scanned }, { "matched", matched }, { "total", static_cast<double>(total) } });
        }
        respond(Result::success(QJsonObject { { "dryRun", dryRun }, { "scanned", scanned }, { "matched", matched },
            { "removed", dryRun ? 0 : matched } }));
    });

    add("torrent.create", [this](const QJsonObject& params, const ResultCallback& respond) {
        const QString path = params["path"].toString();
        if (path.isEmpty()) {
            respond(Result::failure("Empty path"));
            return;
        }
        const QStringList trackers = jsonToStringList(params["trackers"]);
        const QString comment = params["comment"].toString();
        const bool seed = params["seed"].toBool(false);

        if (seed) {
            if (!app_->engine()->fileUploadAllowed()) {
                respond(Result::failure("BT file upload is disabled"));
                return;
            }
            const QString output = params["output"].toString();
            const QString hash = app_->creator()->createAndSeed(path, trackers, comment, output);
            if (hash.isEmpty()) {
                respond(Result::failure("Failed to create torrent"));
                return;
            }
            QJsonObject result;
            result["hash"] = hash;
            result["seeding"] = true;
            respond(Result::success(result));
            return;
        }

        const QString output = params["output"].toString();
        if (output.isEmpty()) {
            respond(Result::failure("Output path required"));
            return;
        }
        if (!app_->creator()->createTorrentFile(path, output, trackers, comment)) {
            respond(Result::failure("Failed to create torrent file"));
            return;
        }
        QJsonObject result;
        result["file"] = output;
        respond(Result::success(result));
    });

    add("torrent.import", [this](const QJsonObject& params, const ResultCallback& respond) {
        const QString file = params.contains("path") ? params["path"].toString() : params["file"].toString();
        if (file.isEmpty()) {
            respond(Result::failure("Empty file path"));
            return;
        }

        const net::TorrentMetadata meta = app_->engine()->readTorrentFile(file);
        if (!meta.valid || !infohash::isValid(meta.hash)) {
            respond(Result::failure("Failed to parse torrent file"));
            return;
        }

        domain::Torrent torrent;
        torrent.hash = infohash::normalize(meta.hash);
        torrent.name = meta.name;
        torrent.size = meta.totalSize;
        torrent.files = meta.files.size();
        torrent.added = QDateTime::currentDateTime();
        for (const net::EngineFile& f : meta.files)
            torrent.fileList.append(domain::File { f.path, f.size });

        const service::IndexingService::Result inserted = app_->indexing()->insert(torrent);
        if (!inserted.success && !inserted.alreadyExists) {
            respond(Result::failure(
                inserted.error.isEmpty() ? QStringLiteral("Failed to import torrent") : inserted.error));
            return;
        }

        QJsonObject result = domain::codec::toJson(inserted.torrent, { /*includeFiles*/ true });
        result["alreadyExists"] = inserted.alreadyExists;
        result["imported"] = !inserted.alreadyExists;
        respond(Result::success(result));
    });

    // -----------------------------------------------------------------------
    // Whole-database replication
    // -----------------------------------------------------------------------
    add("database.export", [this](const QJsonObject& params, const ResultCallback& respond) {
        service::DatabaseSyncService* sync = app_->databaseSync();
        if (!sync) {
            respond(Result::failure("Database sync is not available"));
            return;
        }
        const QString path = params.contains("path") ? params["path"].toString() : params["file"].toString();
        QString error;
        if (!sync->exportToFile(path, &error)) {
            respond(Result::failure(error));
            return;
        }
        // The export runs in the background; progress arrives as
        // databaseSyncProgress events and the summary as databaseSyncFinished.
        respond(Result::success(sync->statusJson()));
    });

    add("database.import", [this](const QJsonObject& params, const ResultCallback& respond) {
        service::DatabaseSyncService* sync = app_->databaseSync();
        if (!sync) {
            respond(Result::failure("Database sync is not available"));
            return;
        }
        const QString path = params.contains("path") ? params["path"].toString() : params["file"].toString();
        service::DatabaseSyncService::ImportOptions options;
        options.applyFilters = params["applyFilters"].toBool(true);
        options.resume = params["resume"].toBool(true);
        options.removeWhenDone = params["removeWhenDone"].toBool(false);

        QString error;
        if (!sync->importFromFile(path, options, &error)) {
            respond(Result::failure(error));
            return;
        }
        respond(Result::success(sync->statusJson()));
    });

    add("database.pull", [this](const QJsonObject& params, const ResultCallback& respond) {
        if (!app_->databaseSync()->peerSyncEnabled()) {
            respond(Result::failure("P2P database pull is unavailable"));
            return;
        }
        service::DatabaseSyncService* sync = app_->databaseSync();
        if (!sync) {
            respond(Result::failure("Database sync is not available"));
            return;
        }
        const QString peerId = params.contains("peer") ? params["peer"].toString() : params["peerId"].toString();
        service::DatabaseSyncService::ImportOptions options;
        options.applyFilters = params["applyFilters"].toBool(true);

        QString error;
        if (!sync->requestFromPeer(peerId, options, &error)) {
            respond(Result::failure(error));
            return;
        }
        respond(Result::success(sync->statusJson()));
    });

    add("database.peers", [this](const QJsonObject& /*params*/, const ResultCallback& respond) {
        // Only the peers a pull could actually succeed against: they advertised
        // databaseSharing in the handshake. Peers with it off, and clients too old
        // to know the feature, are absent rather than listed and unusable.
        const QHash<QString, domain::PeerStats> peers = app_->peers()->databaseSharingPeers();
        QJsonArray result;
        for (auto it = peers.constBegin(); it != peers.constEnd(); ++it) {
            QJsonObject peer = it.value().toJson();
            peer["peerId"] = it.key();
            result.append(peer);
        }
        respond(Result::success(result));
    });

    add("database.status", [this](const QJsonObject& /*params*/, const ResultCallback& respond) {
        service::DatabaseSyncService* sync = app_->databaseSync();
        if (!sync) {
            respond(Result::failure("Database sync is not available"));
            return;
        }
        respond(Result::success(sync->statusJson()));
    });

    add("database.cancel", [this](const QJsonObject& /*params*/, const ResultCallback& respond) {
        service::DatabaseSyncService* sync = app_->databaseSync();
        if (!sync) {
            respond(Result::failure("Database sync is not available"));
            return;
        }
        sync->cancel();
        respond(Result::success(sync->statusJson()));
    });

    // Rebuild the dump we hand to peers. Normally nobody needs to call this — the
    // snapshot renews itself once it ages out or the index drifts away from it —
    // but it is the escape hatch when a user wants peers to see recent work now.
    add("database.snapshot", [this](const QJsonObject& /*params*/, const ResultCallback& respond) {
        if (!app_->transport()->isRunning() || !app_->config()->indexExchangeEnabled()) {
            respond(Result::failure("Outward index sharing is disabled"));
            return;
        }
        service::DatabaseSyncService* sync = app_->databaseSync();
        if (!sync) {
            respond(Result::failure("Database sync is not available"));
            return;
        }
        QString error;
        if (!sync->rebuildSnapshot(&error)) {
            respond(Result::failure(error));
            return;
        }
        respond(Result::success(sync->statusJson()));
    });

    // -----------------------------------------------------------------------
    // Downloads
    // -----------------------------------------------------------------------
    add("download.add", [this](const QJsonObject& params, const ResultCallback& respond) {
        if (!app_->engine()->fileDownloadAllowed()) {
            respond(Result::failure("BT file download is disabled"));
            return;
        }
        const QString link = params.contains("hash") ? params["hash"].toString() : params["magnet"].toString();
        const QString savePath = params["savePath"].toString();
        const QString hash = net::TorrentEngine::parseInfoHash(link);
        if (!infohash::isValid(hash)) {
            respond(Result::failure("Invalid hash"));
            return;
        }

        // Reuse indexed metadata (name/size) when we already know the torrent.
        std::optional<domain::Torrent> known = app_->search()->get(hash, false);
        const bool ok
            = known ? app_->downloads()->addWithInfo(*known, savePath) : app_->downloads()->add(link, savePath);
        respond(ok ? Result::success() : Result::failure("Failed to start download"));
    });

    add("download.addFile", [this](const QJsonObject& params, const ResultCallback& respond) {
        if (!app_->engine()->fileDownloadAllowed()) {
            respond(Result::failure("BT file download is disabled"));
            return;
        }
        const QString file = params.contains("path") ? params["path"].toString() : params["file"].toString();
        const QString savePath = params["savePath"].toString();
        if (file.isEmpty()) {
            respond(Result::failure("Empty file path"));
            return;
        }
        const bool ok = app_->downloads()->addFromFile(file, savePath);
        respond(ok ? Result::success() : Result::failure("Failed to add torrent file"));
    });

    add("download.pause", [this](const QJsonObject& params, const ResultCallback& respond) {
        const QString hash = infohash::normalize(params["hash"].toString());
        const bool ok = app_->downloads()->pause(hash);
        respond(ok ? Result::success() : Result::failure("Download not found"));
    });

    add("download.resume", [this](const QJsonObject& params, const ResultCallback& respond) {
        const QString hash = infohash::normalize(params["hash"].toString());
        const bool ok = app_->downloads()->resume(hash);
        respond(ok ? Result::success() : Result::failure("Download not found or file transfer disabled"));
    });

    add("download.remove", [this](const QJsonObject& params, const ResultCallback& respond) {
        const QString hash = infohash::normalize(params["hash"].toString());
        if (!app_->downloads()->isDownloading(hash)) {
            respond(Result::failure("Download not found"));
            return;
        }
        const bool saveResumeData = params["saveResumeData"].toBool(false);
        app_->downloads()->remove(hash, saveResumeData);
        respond(Result::success());
    });

    add("download.list", [this](const QJsonObject& /*params*/, const ResultCallback& respond) {
        respond(Result::success(app_->downloads()->toJsonArray()));
    });

    add("download.selectFiles", [this](const QJsonObject& params, const ResultCallback& respond) {
        const QString hash = infohash::normalize(params["hash"].toString());
        const bool ok = app_->downloads()->selectFilesJson(hash, params["files"]);
        respond(ok ? Result::success() : Result::failure("Failed to select files"));
    });

    // -----------------------------------------------------------------------
    // Feed
    // -----------------------------------------------------------------------
    add("feed.get", [this](const QJsonObject& params, const ResultCallback& respond) {
        const int index = params["index"].toInt(0);
        const int limit = params["limit"].toInt(20);
        respond(Result::success(app_->feed()->toJsonArray(index, limit)));
    });

    // -----------------------------------------------------------------------
    // Voting (async: the distributed store answers via callback)
    // -----------------------------------------------------------------------
    add("vote.cast", [this](const QJsonObject& params, const ResultCallback& respond) {
        const QString hash = infohash::normalize(params["hash"].toString());
        if (!infohash::isValid(hash)) {
            respond(Result::failure("Invalid hash"));
            return;
        }
        const bool good = params.contains("good") ? params["good"].toBool(true) : params["isGood"].toBool(true);
        app_->voting()->vote(hash, good, [respond](bool ok, const QJsonObject& result, const QString& error) {
            respond(ok ? Result::success(result) : Result::failure(error));
        });
    });

    add("vote.get", [this](const QJsonObject& params, const ResultCallback& respond) {
        const QString hash = infohash::normalize(params["hash"].toString());
        if (!infohash::isValid(hash)) {
            respond(Result::failure("Invalid hash"));
            return;
        }
        app_->voting()->getVotes(hash, [respond](bool ok, const QJsonObject& result, const QString& error) {
            respond(ok ? Result::success(result) : Result::failure(error));
        });
    });

    // -----------------------------------------------------------------------
    // Config
    // -----------------------------------------------------------------------
    add("config.get", [this](const QJsonObject& /*params*/, const ResultCallback& respond) {
        respond(Result::success(app_->config()->toJson()));
    });

    add("config.set", [this](const QJsonObject& params, const ResultCallback& respond) {
        const QStringList changed = app_->config()->fromJson(params);
        // Even a no-op update must prove the current configuration is on disk
        // before the API reports a successful save.
        if (!app_->config()->save()) {
            respond(Result::failure(changed.isEmpty()
                ? QStringLiteral("Settings could not be saved to disk. No values changed in this request.")
                : QStringLiteral("Settings applied in memory but could not be saved to disk. Retry saving before restarting.")));
            return;
        }
        QJsonObject result;
        result["changed"] = QJsonArray::fromStringList(changed);
        respond(Result::success(result));
    });

    add("features.status", [this](const QJsonObject& /*params*/, const ResultCallback& respond) {
        respond(Result::success(app_->featureStatus()));
    });
    add("network.start", [this](const QJsonObject&, const ResultCallback& respond) {
        const bool ok = app_->startNetwork();
        const QJsonObject status = app_->featureStatus();
        respond(ok ? Result::success(status)
                   : Result::failure(status["networkState"] == QLatin1String("failed") &&
                             !status["lastError"].toString().isEmpty()
                         ? status["lastError"].toString()
                         : QStringLiteral("Network start is busy or application is not ready")));
    });
    add("network.stop", [this](const QJsonObject&, const ResultCallback& respond) {
        const bool ok = app_->stopNetwork();
        respond(ok ? Result::success(app_->featureStatus())
                   : Result::failure(QStringLiteral("Network stop is busy or on the wrong thread")));
    });
    add("crawler.start", [this](const QJsonObject&, const ResultCallback& respond) {
        const bool ok = app_->startCrawler();
        const QJsonObject status = app_->featureStatus();
        respond(ok ? Result::success(status)
                   : Result::failure(status["crawlerLastError"].toString().isEmpty()
                         ? QStringLiteral("Crawler start is unavailable") : status["crawlerLastError"].toString()));
    });
    add("crawler.stop", [this](const QJsonObject&, const ResultCallback& respond) {
        respond(app_->stopCrawler() ? Result::success(app_->featureStatus())
                                   : Result::failure(QStringLiteral("Crawler stop is unavailable")));
    });

    // -----------------------------------------------------------------------
    // Statistics & peers
    // -----------------------------------------------------------------------
    add("stats.get", [this](const QJsonObject& /*params*/, const ResultCallback& respond) {
        const data::TorrentRepository::Statistics stats = app_->torrents()->statistics();
        QJsonObject result;
        result["torrents"] = stats.torrents;
        result["files"] = stats.files;
        result["size"] = stats.totalSize;
        result["peers"] = app_->transport() ? app_->transport()->peerCount() : 0;
        respond(Result::success(result));
    });

    // Process-local crawler counters. Metadata success is distinct from a
    // committed index row; discoveredCount also includes file-list backfills.
    add("stats.crawler", [this](const QJsonObject& /*params*/, const ResultCallback& respond) {
        const auto* crawler = app_->crawler();
        if (!crawler) {
            respond(Result::failure(QStringLiteral("Crawler statistics unavailable")));
            return;
        }
        respond(Result::success(QJsonObject {
            { "sampleRequests", crawler->sampleRequestCount() },
            { "sampleResponses", crawler->sampleResponseCount() },
            { "sampleHashes", crawler->sampleHashCount() },
            { "hashesEnqueued", crawler->hashEnqueuedCount() },
            { "activeFetches", crawler->activeFetches() },
            { "queuedFetches", crawler->queuedFetches() },
            { "fetchSuccesses", crawler->fetchSuccessCount() },
            { "fetchErrors", crawler->fetchErrorCount() },
            { "indexedOrBackfilled", crawler->discoveredCount() },
        }));
    });

    add("stats.database", [this](const QJsonObject& /*params*/, const ResultCallback& respond) {
        const data::TorrentRepository::Statistics stats = app_->torrents()->statistics();
        QJsonObject result;
        result["torrents"] = stats.torrents;
        result["files"] = stats.files;
        result["size"] = stats.totalSize;
        respond(Result::success(result));
    });

    add("stats.p2pStatus", [this](const QJsonObject& /*params*/, const ResultCallback& respond) {
        QJsonObject result;
        if (auto* t = app_->transport()) {
            result["peerCount"] = t->peerCount();
            result["dhtNodes"] = static_cast<qint64>(t->dhtNodeCount());
            result["dhtRunning"] = t->isDhtRunning();
            result["running"] = t->isRunning();
        }
        respond(Result::success(result));
    });

    add("peers.list", [this](const QJsonObject& /*params*/, const ResultCallback& respond) {
        const QHash<QString, domain::PeerStats> peers = app_->peers()->connectedPeers();
        QJsonArray result;
        for (auto it = peers.constBegin(); it != peers.constEnd(); ++it) {
            QJsonObject peer = it.value().toJson();
            peer["peerId"] = it.key();
            result.append(peer);
        }
        respond(Result::success(result));
    });

    // -----------------------------------------------------------------------
    // Trackers (fire-and-forget scrape; results flow back into the index)
    // -----------------------------------------------------------------------
    add("tracker.check", [this](const QJsonObject& params, const ResultCallback& respond) {
        if (!app_->transport()->isRunning() || !app_->config()->trackersEnabled()) {
            respond(Result::failure("Tracker scraping is disabled"));
            return;
        }
        const QString hash = infohash::normalize(params["hash"].toString());
        if (!infohash::isValid(hash)) {
            respond(Result::failure("Invalid hash"));
            return;
        }
        app_->trackers()->checkCounts(hash);
        app_->trackers()->checkInfo(hash);
        QJsonObject result;
        result["hash"] = hash;
        result["status"] = "checking";
        respond(Result::success(result));
    });

    // -----------------------------------------------------------------------
    // Updates (async: GitHub release check answers via signals)
    // -----------------------------------------------------------------------
    add("update.check", [this](const QJsonObject& /*params*/, const ResultCallback& respond) {
        service::UpdateService* svc = app_->updates();

        // First of the three terminal signals wins; it disconnects the rest so
        // `respond` runs exactly once.
        auto guard = std::make_shared<bool>(false);
        auto connections = std::make_shared<QVector<QMetaObject::Connection>>();
        auto finish = [guard, connections, respond](const Result& result) {
            if (*guard)
                return;
            *guard = true;
            for (const QMetaObject::Connection& c : *connections)
                QObject::disconnect(c);
            respond(result);
        };

        connections->append(connect(svc, &service::UpdateService::updateAvailable, this,
            [finish](const service::UpdateService::UpdateInfo& info) {
                QJsonObject result;
                result["available"] = true;
                result["version"] = info.version;
                result["downloadUrl"] = info.downloadUrl;
                result["releaseNotes"] = info.releaseNotes;
                result["downloadSize"] = info.downloadSize;
                result["publishedAt"] = info.publishedAt;
                result["prerelease"] = info.isPrerelease;
                finish(Result::success(result));
            }));
        connections->append(connect(svc, &service::UpdateService::noUpdateAvailable, this, [finish]() {
            QJsonObject result;
            result["available"] = false;
            finish(Result::success(result));
        }));
        connections->append(connect(svc, &service::UpdateService::errorOccurred, this,
            [finish](const QString& error) { finish(Result::failure(error)); }));

        svc->checkForUpdates();
    });

    // torrent.export: triggers async .torrent generation, returns hash for tracking
    add("torrent.export", [this](const QJsonObject& params, const ResultCallback& respond) {
        const QString hash = infohash::normalize(params["hash"].toString());
        if (!infohash::isValid(hash)) {
            respond(Result::failure("Invalid hash"));
            return;
        }
        auto opt = app_->search()->get(hash, false);
        if (!opt) {
            respond(Result::failure("Torrent not found"));
            return;
        }
        const QString cachePath = QDir(app_->options().dataDirectory)
                                      .absoluteFilePath(QStringLiteral("torrents/") + hash + QStringLiteral(".torrent"));
        const bool cached = QFile::exists(cachePath);
        if (!cached && (!app_->engine() || !app_->engine()->isReady())) {
            respond(Result::failure("BitTorrent metadata export is unavailable"));
            return;
        }
        app_->exporter()->requestExport(hash, opt->name);
        QJsonObject result;
        result["name"] = opt->name;
        result["hash"] = hash;
        result["downloadUrl"] = QStringLiteral("/download/%1.torrent").arg(hash);
        result["ready"] = cached;
        respond(Result::success(result));
    });
}

} // namespace rats::rest
