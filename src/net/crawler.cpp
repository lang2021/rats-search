#include "net/crawler.h"

#include "net/librats_convert.h"
#include "net/p2p_transport.h"

// librats' EventBus exposes a method named emit(), which collides with Qt's
// `emit` keyword macro. Neutralise the macro across all librats includes, then
// restore it so the crawler's own `emit signal` statements keep working.
#pragma push_macro("emit")
#pragma push_macro("slots")
#pragma push_macro("signals")
#undef emit
#undef slots
#undef signals
#ifdef RATS_SEARCH_FEATURES
#include "librats/bittorrent/torrent_info.h"
#include "librats/core/address.h"
#include "librats/dht/dht.h"
#include "librats/subsystems/bittorrent.h"
#endif
#pragma pop_macro("signals")
#pragma pop_macro("slots")
#pragma pop_macro("emit")

#include <QDebug>
#include <QHostAddress>
#include <QRandomGenerator>
#include <QVector>
#include <algorithm>
#include <iterator>

namespace rats::net {

Crawler::Crawler(P2PTransport* transport, QObject* parent)
    : QObject(parent)
    , transport_(transport)
    , running_(false)
    , discoveredCount_(0)
    , activeFetches_(0)
    , fetchSuccessCount_(0)
    , fetchErrorCount_(0)
    , visitedNodesCount_(0)
    , walkIntervalMs_(DEFAULT_WALK_INTERVAL_MS)
{
    walkTimer_ = new QTimer(this);
    ignoreTimer_ = new QTimer(this);
    metadataQueueTimer_ = new QTimer(this);
    sampleTimer_ = new QTimer(this);
    clock_.start();
    callbackGate_ = std::make_shared<CallbackGate>();
    callbackGate_->crawler = this;

    connect(walkTimer_, &QTimer::timeout, this, &Crawler::onSpiderWalk);
    connect(ignoreTimer_, &QTimer::timeout, this, &Crawler::onIgnoreToggle);
    connect(metadataQueueTimer_, &QTimer::timeout, this, &Crawler::processMetadataQueue);
    connect(sampleTimer_, &QTimer::timeout, this, &Crawler::onSampleTick);
}

Crawler::~Crawler()
{
    {
        std::lock_guard<std::mutex> lock(callbackGate_->mutex);
        callbackGate_->crawler = nullptr;
    }
    stop();
}

librats::Bittorrent* Crawler::bittorrent() const
{
    if (!transport_) {
        return nullptr;
    }
    return transport_->bittorrent();
}

bool Crawler::start()
{
    if (running_) {
        return true;
    }
    if (pendingFetches_ > 0) {
        qWarning() << "Crawler: previous metadata requests are still draining";
        return false;
    }

    if (!transport_) {
        qWarning() << "Crawler: P2P transport not available";
        return false;
    }

    if (!transport_->isRunning()) {
        qWarning() << "Crawler: P2P transport is not running - start it first";
        return false;
    }

    librats::Bittorrent* bt = bittorrent();
    if (!bt) {
        qWarning() << "Crawler: BitTorrent subsystem not available from transport";
        return false;
    }

    if (!transport_->isDhtRunning()) {
        qWarning() << "Crawler: DHT is not running";
        return false;
    }

    qInfo() << "Starting DHT crawler...";

    try {
#ifdef RATS_SEARCH_FEATURES
        // Enable spider mode with an announce callback. The core delivers the
        // info-hash already decoded and the peer as a structured Address, so no
        // string parsing is needed. The callback fires on a librats worker
        // thread, so marshal it onto the Qt thread before touching our state.
        bt->set_spider_mode(true);

        const uint64_t generation = ++generation_;
        auto gate = callbackGate_;
        bt->set_spider_announce_callback([gate, generation](const librats::InfoHash& info_hash, const librats::Address& peer) {
            std::array<uint8_t, 20> infoHash = info_hash;
            std::string ip = peer.ip.to_string();
            uint16_t port = peer.port;
            std::lock_guard<std::mutex> lock(gate->mutex);
            if (auto* crawler = gate->crawler)
                QMetaObject::invokeMethod(crawler,
                    [crawler, infoHash, ip, port, generation]() { crawler->onAnnounce(infoHash, ip, port, generation); },
                    Qt::QueuedConnection);
        });

        bt->set_spider_get_peers_callback([gate, generation](const librats::InfoHash& info_hash) {
            std::array<uint8_t, 20> infoHash = info_hash;
            std::lock_guard<std::mutex> lock(gate->mutex);
            if (auto* crawler = gate->crawler)
                QMetaObject::invokeMethod(crawler,
                    [crawler, infoHash, generation]() { crawler->onGetPeers(infoHash, generation); },
                    Qt::QueuedConnection);
        });

        qInfo() << "Spider mode enabled with announce callback";
#endif

        running_ = true;

        walkTimer_->start(walkIntervalMs_);
        ignoreTimer_->start(DEFAULT_IGNORE_INTERVAL_MS);
        metadataQueueTimer_->start(METADATA_QUEUE_INTERVAL_MS);
        sampleTimer_->start(SAMPLE_INTERVAL_MS);

        emit statusChanged("Active");

        qInfo() << "DHT crawler started successfully";
        qInfo() << "DHT nodes:" << transport_->dhtNodeCount();
        return true;
    } catch (const std::exception& e) {
        qWarning() << "Crawler: failed to start:" << e.what();
        return false;
    }
}

void Crawler::stop()
{
    // Discard requests belonging to the old Node even if the crawler was not
    // fully started. A queued callback still carries its old generation.
    ++generation_;
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        std::queue<MetadataRequest> empty;
        metadataQueue_.swap(empty);
    }
    pendingHashes_.clear();
    for (auto it = activeMetadataTaskIds_.cbegin(); it != activeMetadataTaskIds_.cend(); ++it)
        emit metadataEnded(it.key(), QStringLiteral("observationStopped"));
    activeMetadataTaskIds_.clear();
    sampleInFlight_.clear();
    if (!running_) {
        return;
    }

    qInfo() << "Stopping DHT crawler...";
    running_ = false;

    walkTimer_->stop();
    ignoreTimer_->stop();
    metadataQueueTimer_->stop();
    sampleTimer_->stop();

#ifdef RATS_SEARCH_FEATURES
    librats::Bittorrent* bt = bittorrent();
    if (bt) {
        bt->set_spider_mode(false);
        bt->set_spider_get_peers_callback({});
    }
#endif

    // Note: we never stop the BitTorrent subsystem here - the transport owns it.

    emit statusChanged(pendingFetches_ > 0 ? "Stopping" : "Stopped");

    qInfo() << "DHT crawler stopped. Total discovered:" << discoveredCount_.load();
}

bool Crawler::isRunning() const
{
    return running_;
}

void Crawler::setWalkInterval(int intervalMs)
{
    walkIntervalMs_ = intervalMs;
    if (running_ && walkTimer_) {
        walkTimer_->setInterval(intervalMs);
    }
}

void Crawler::setKnownHashFilter(KnownHashFilter filter)
{
    knownHashFilter_ = std::move(filter);
}

void Crawler::onSpiderWalk()
{
    if (!running_) {
        return;
    }

    visitedNodesCount_++;

#ifdef RATS_SEARCH_FEATURES
    // Trigger a spider walk to expand the DHT routing table.
    librats::Bittorrent* bt = bittorrent();
    if (bt) {
        bt->spider_walk();
    }
#endif
}

void Crawler::onSampleTick()
{
#ifdef RATS_SEARCH_FEATURES
    if (!running_ || !transport_->isDhtRunning() || !sampleInFlight_.empty()) return;
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        if (metadataQueue_.size() >= MAX_METADATA_QUEUE) return;
    }
    auto* bt = bittorrent();
    auto* dht = bt ? bt->dht_client() : nullptr;
    if (!dht) return;
    const auto candidates = dht->sample_candidates();
    if (candidates.empty()) return;

    const qint64 now = clock_.elapsed();
    for (auto it = sampleNextAllowed_.begin(); it != sampleNextAllowed_.end(); ) {
        if (it.value() <= now) it = sampleNextAllowed_.erase(it);
        else ++it;
    }
    const size_t start = candidateCursor_ % candidates.size();
    for (size_t offset = 0; offset < candidates.size(); ++offset) {
        const size_t index = (start + offset) % candidates.size();
        const auto& endpoint = candidates[index];
        const QString key = QString::fromStdString(endpoint.to_string());
        if (sampleNextAllowed_.contains(key)) continue;

        librats::NodeId target{};
        for (auto& byte : target) byte = static_cast<uint8_t>(QRandomGenerator::global()->bounded(256));
        const uint64_t generation = generation_.load();
        auto gate = callbackGate_;
        sampleInFlight_.insert(key);
        sampleNextAllowed_.insert(key, now + SAMPLE_FAILURE_DELAY_MS);
        candidateCursor_ = index + 1;
        if (dht->sample_infohashes(endpoint, target, [gate, key, generation](const auto& result) {
                std::lock_guard<std::mutex> lock(gate->mutex);
                if (auto* crawler = gate->crawler)
                    QMetaObject::invokeMethod(crawler,
                        [crawler, key, result, generation]() { crawler->onSampleResult(key, result, generation); },
                        Qt::QueuedConnection);
            })) {
            ++sampleRequestCount_;
        } else {
            sampleInFlight_.erase(key);
        }
        return; // at most one query per timer tick
    }
#endif
}

void Crawler::onSampleResult(const QString& nodeKey, const librats::SampleInfohashesResult& result,
    uint64_t generation)
{
#ifdef RATS_SEARCH_FEATURES
    if (!running_ || generation != generation_) return;
    sampleInFlight_.erase(nodeKey);
    if (result.status != librats::SampleInfohashesStatus::Success) return;
    ++sampleResponseCount_;
    sampleNextAllowed_.insert(nodeKey, clock_.elapsed() +
        std::max<qint64>(SAMPLE_MIN_DELAY_MS, static_cast<qint64>(result.interval) * 1000));
    const size_t count = std::min(result.samples.size(), static_cast<size_t>(MAX_SAMPLES_PER_RESPONSE));
    sampleHashCount_ += static_cast<int>(count);
    for (size_t i = 0; i < count; ++i)
        acceptInfoHash(result.samples[i], {}, 0, generation);
#else
    Q_UNUSED(nodeKey); Q_UNUSED(result); Q_UNUSED(generation);
#endif
}

void Crawler::onIgnoreToggle()
{
#ifdef RATS_SEARCH_FEATURES
    // Rate limiting: toggle spider ignore mode to throttle incoming DHT requests.
    librats::Bittorrent* bt = bittorrent();
    if (bt && bt->is_spider_mode()) {
        bool currentIgnore = bt->is_spider_ignoring();
        bt->set_spider_ignore(!currentIgnore);
    }
#endif
}

void Crawler::processMetadataQueue()
{
    if (!running_) {
        return;
    }

    while (activeFetches_.load() < MAX_CONCURRENT_METADATA_FETCHES) {
        MetadataRequest request;
        {
            std::lock_guard<std::mutex> lock(queueMutex_);
            if (metadataQueue_.empty()) {
                break;
            }
            request = metadataQueue_.front();
            metadataQueue_.pop();
        }

        fetchMetadata(request);
    }
}

void Crawler::onAnnounce(const std::array<uint8_t, 20>& infoHash, const std::string& ip, uint16_t port,
    uint64_t generation)
{
    const QString peerIp = QString::fromStdString(ip);
    const QHostAddress address(peerIp);
    const bool validPeer = port != 0 && !address.isNull()
        && address != QHostAddress(QHostAddress::AnyIPv4)
        && address != QHostAddress(QHostAddress::AnyIPv6);
    acceptInfoHash(infoHash, validPeer ? peerIp : QString(), validPeer ? port : 0, generation, DiscoverySource::Announce);
}

void Crawler::onGetPeers(const std::array<uint8_t, 20>& infoHash, uint64_t generation)
{
    acceptInfoHash(infoHash, {}, 0, generation, DiscoverySource::GetPeers);
}

void Crawler::acceptInfoHash(const std::array<uint8_t, 20>& infoHash, const QString& peerIp,
    uint16_t peerPort, uint64_t generation, DiscoverySource source)
{
    if (!running_ || generation != generation_) return;
    // Convert info-hash to a lower-case hex string.
    QString hashHex;
    hashHex.reserve(40);
    for (uint8_t byte : infoHash) {
        hashHex += QString("%1").arg(byte, 2, 16, QChar('0'));
    }
    if (pendingHashes_.count(hashHex)) {
        // A later announce can upgrade a queued hash-only sample to the known
        // resource peer. A running task receives the hint using its own id.
        if (!peerIp.isEmpty() && peerPort) {
            bool queued = false;
            {
                std::lock_guard<std::mutex> lock(queueMutex_);
                std::queue<MetadataRequest> pending;
                while (!metadataQueue_.empty()) {
                    auto request = metadataQueue_.front();
                    metadataQueue_.pop();
                    if (request.infoHash == hashHex) {
                        queued = true;
                        if (request.peerIp.isEmpty()) {
                            request.peerIp = peerIp;
                            request.peerPort = peerPort;
                        }
                    }
                    pending.push(std::move(request));
                }
                metadataQueue_.swap(pending);
            }
            if (!queued) {
                const uint64_t taskId = activeMetadataTaskIds_.value(hashHex, 0);
                if (taskId) {
                    if (auto* bt = bittorrent())
                        bt->add_metadata_peer(hashHex.toStdString(), taskId, peerIp.toStdString(), peerPort);
                }
            }
        }
        return;
    }
    const auto retry = retryAfter_.constFind(hashHex);
    if (retry != retryAfter_.cend() && retry->afterMs > clock_.elapsed() &&
        !(peerPort && !peerIp.isEmpty() && !retry->hadPeer)) return;

    // A hash-only candidate needs no file-table lookup until the caller has
    // found an existing main row. Explicitly missing file rows may be retried
    // through this same bounded queue; uncertain reads must stop here.
    if (knownHashFilter_) {
        const MetadataDecision decision = knownHashFilter_(hashHex);
        if (decision == MetadataDecision::Error) {
            qWarning() << "Crawler: metadata completeness check failed for" << hashHex.left(8);
            return;
        }
        if (decision == MetadataDecision::Skip) {
            qDebug() << "Already indexed or completeness ambiguous:" << hashHex.left(8);
            return;
        }
    }

    // All three sources share the bounded queue. Only announce_peer supplies a
    // resource peer; a get_peers requester and BEP 51 nodes are DHT contacts.
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        if (metadataQueue_.size() >= MAX_METADATA_QUEUE) {
            if (peerIp.isEmpty() || !peerPort) return;
            // An announce with a real resource peer is more useful than a
            // queued hash-only sample when the bounded queue is full.
            std::queue<MetadataRequest> pending;
            bool evicted = false;
            while (!metadataQueue_.empty()) {
                auto queued = metadataQueue_.front();
                metadataQueue_.pop();
                if (!evicted && queued.peerIp.isEmpty()) {
                    pendingHashes_.erase(queued.infoHash);
                    evicted = true;
                } else {
                    pending.push(std::move(queued));
                }
            }
            metadataQueue_.swap(pending);
            if (!evicted) return;
        }
        MetadataRequest request;
        request.infoHash = hashHex;
        request.peerIp = peerIp;
        request.peerPort = peerPort;
        request.fromGetPeers = source == DiscoverySource::GetPeers;
        metadataQueue_.push(request);
    }
    retryAfter_.remove(hashHex);
    pendingHashes_.insert(hashHex);
    ++hashEnqueuedCount_;
    emit hashQueued(hashHex, source, !peerIp.isEmpty() && peerPort);
}

void Crawler::fetchMetadata(const MetadataRequest& request)
{
#ifdef RATS_SEARCH_FEATURES
    librats::Bittorrent* bt = bittorrent();
    if (!bt) {
        onMetadataFailed(request.infoHash, !request.peerIp.isEmpty(),
            QStringLiteral("BT metadata engine unavailable"), generation_.load());
        return;
    }

    activeFetches_++;
    pendingFetches_++;
    const uint64_t generation = generation_.load();

    const QString infoHash = request.infoHash;
    const QString peerIp = request.peerIp;
    const uint16_t peerPort = request.peerPort;
    auto gate = callbackGate_;

    // librats manages the temporary torrent, the BEP 9 fetch and the timeout; it
    // invokes this callback exactly once (success or timeout) on a worker thread.
    auto onResult = [gate, infoHash, peerIp, peerPort, generation](
                        const librats::bittorrent::TorrentInfo& torrentInfo, bool success, const std::string& err) {
        std::lock_guard<std::mutex> lock(gate->mutex);
        auto* crawler = gate->crawler;
        if (!crawler) return;
        const int remaining = --crawler->pendingFetches_;
        --crawler->activeFetches_;
        if (remaining == 0 && !crawler->running_) {
            QMetaObject::invokeMethod(crawler, [crawler]() {
                if (!crawler->running_ && crawler->pendingFetches_ == 0) emit crawler->statusChanged("Stopped");
            }, Qt::QueuedConnection);
        }
        if (!crawler->running_ || generation != crawler->generation_)
            return;

        if (!success || !torrentInfo.is_valid() || !torrentInfo.has_metadata()
            || torrentInfo.files().files().empty() || torrentInfo.info_hash_hex() != infoHash.toStdString()) {
            const QString error = success ? QStringLiteral("invalid or mismatched BEP 9 metadata")
                                          : QString::fromStdString(err);
            QMetaObject::invokeMethod(crawler, [crawler, infoHash, peerIp, error, generation]() {
                crawler->onMetadataFailed(infoHash, !peerIp.isEmpty(), error, generation);
            }, Qt::QueuedConnection);
            return;
        }

        // Build the domain torrent here, on the worker thread, then marshal the
        // finished value onto the Qt thread.
        const domain::Torrent torrent = toDomainTorrent(infoHash, torrentInfo, peerIp, static_cast<int>(peerPort));

        QMetaObject::invokeMethod(crawler, [crawler, torrent, infoHash, generation]() {
            crawler->onMetadataReceived(torrent, infoHash, generation);
        }, Qt::QueuedConnection);
    };

    uint64_t taskId = 0;
    if (!peerIp.isEmpty() && peerPort > 0) {
        // Fast path: fetch directly from the announcing peer (no DHT search).
        qDebug() << "Fetching metadata for" << infoHash.left(8) << "directly from" << peerIp << ":" << peerPort;
        taskId = bt->get_torrent_metadata_from_peer(infoHash.toStdString(), peerIp.toStdString(), peerPort, onResult);
    } else {
        // Slow path: let librats find peers via the DHT.
        qDebug() << "Fetching metadata for" << infoHash.left(8) << "via DHT search";
        taskId = bt->get_torrent_metadata(infoHash.toStdString(), onResult);
    }
    if (taskId) {
        activeMetadataTaskIds_.insert(infoHash, taskId);
        emit metadataStarted(infoHash, !peerIp.isEmpty() && peerPort);
    }
#else
    Q_UNUSED(request);
    qWarning() << "BitTorrent features not enabled, cannot fetch metadata";
#endif
}

void Crawler::onMetadataReceived(const rats::domain::Torrent& torrent, const QString& infoHash,
    uint64_t generation)
{
    if (!running_ || generation != generation_)
        return;
    activeMetadataTaskIds_.remove(infoHash);
    pendingHashes_.erase(infoHash);
    rememberCooldown(infoHash, true); // also covers an index filter rejecting the result
    fetchSuccessCount_++;
    emit metadataEnded(infoHash, QStringLiteral("received"));
    emit discovered(torrent);
}

void Crawler::onMetadataFailed(const QString& infoHash, bool hadPeer, const QString& error,
    uint64_t generation)
{
    if (!running_ || generation != generation_) return;
    activeMetadataTaskIds_.remove(infoHash);
    pendingHashes_.erase(infoHash);
    rememberCooldown(infoHash, hadPeer);
    ++fetchErrorCount_;
    // Only forward errors produced by our metadata implementation.
    static const QStringList known = {
        "metadata timeout", "bittorrent stopped", "shutting down", "invalid info hash",
        "torrent already active without metadata", "failed to create metadata task",
        "bittorrent not running", "BT metadata engine unavailable", "invalid or mismatched BEP 9 metadata"
    };
    emit metadataEnded(infoHash, known.contains(error) ? error : QStringLiteral("unknown"));
    qInfo() << "Failed to get metadata for" << infoHash.left(8) << ":" << error;
}

void Crawler::rememberCooldown(const QString& infoHash, bool hadPeer)
{
    if (retryAfter_.size() >= static_cast<qsizetype>(MAX_RETRY_HASHES)) {
        auto oldest = retryAfter_.begin();
        for (auto it = retryAfter_.begin(); it != retryAfter_.end(); ++it)
            if (it->afterMs < oldest->afterMs) oldest = it;
        retryAfter_.erase(oldest);
    }
    retryAfter_.insert(infoHash, RetryState{clock_.elapsed() + RETRY_DELAY_MS, hadPeer});
}

} // namespace rats::net
