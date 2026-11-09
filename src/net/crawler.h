#ifndef RATS_NET_CRAWLER_H
#define RATS_NET_CRAWLER_H

#include "domain/torrent.h"

#include <QObject>
#include <QElapsedTimer>
#include <QHash>
#include <QString>
#include <QTimer>
#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <queue>
#include <set>
#include <memory>
#include <string>

namespace librats {
class Bittorrent;
struct SampleInfohashesResult;
}

namespace rats::net {

class P2PTransport;

// DHT crawler for the rats::net layer.
//
// Borrows the librats BitTorrent subsystem from a P2PTransport (non-owning)
// and:
//   1. Walks the DHT to expand the routing table.
//   2. Captures announce_peer messages from other clients (spider mode).
//   3. Fetches BEP 9 metadata for freshly discovered info-hashes.
//   4. Emits the resulting domain::Torrent — and nothing more.
//
// The crawler's responsibility ENDS at discovery: it never touches a database
// and never inserts anything. Persistence, filtering and content classification
// are the caller's job, driven off the discovered() signal.
class Crawler : public QObject {
    Q_OBJECT

public:
    // transport is borrowed (non-owning) and must outlive the crawler.
    explicit Crawler(P2PTransport* transport, QObject* parent = nullptr);
    ~Crawler() override;

    // Lifecycle
    bool start();
    void stop();
    bool isRunning() const;
    bool isStopping() const { return !running_ && pendingFetches_.load() > 0; }
    int pendingFetches() const { return pendingFetches_.load(); }

    // Metrics getters
    int discoveredCount() const { return discoveredCount_.load(); }
    int activeFetches() const { return activeFetches_.load(); }
    int fetchSuccessCount() const { return fetchSuccessCount_.load(); }
    int fetchErrorCount() const { return fetchErrorCount_.load(); }
    int visitedNodesCount() const { return visitedNodesCount_.load(); }
    int sampleRequestCount() const { return sampleRequestCount_.load(); }
    int sampleResponseCount() const { return sampleResponseCount_.load(); }
    int sampleHashCount() const { return sampleHashCount_.load(); }
    int hashEnqueuedCount() const { return hashEnqueuedCount_.load(); }
    int queuedFetches() const {
        std::lock_guard<std::mutex> lock(queueMutex_);
        return static_cast<int>(metadataQueue_.size());
    }
    bool isInvestigating(const QString& hash) const {
        std::lock_guard<std::mutex> lock(queueMutex_);
        return pendingHashes_.count(hash) != 0;
    }
    // Application calls this only after IndexingService confirms a new row or
    // file-list backfill. BEP 9 success alone is counted by fetchSuccessCount.
    void recordIndexed() { ++discoveredCount_; }

    // Tuning
    void setWalkInterval(int intervalMs);

    // The caller decides whether a newly observed hash needs metadata. An
    // inconclusive database read must never be treated as a missing record.
    enum class MetadataDecision { Fetch, Skip, Error };
    using KnownHashFilter = std::function<MetadataDecision(const QString& infoHash)>;
    void setKnownHashFilter(KnownHashFilter filter);
    void recordMetadataIndexResult(uint64_t taskId, const char* result);

    enum class DiscoverySource { Sample, GetPeers, Announce };
    Q_ENUM(DiscoverySource)
    struct PeerHint { QString ip; uint16_t port = 0; };
    void setMetadataPeerProvider(std::function<std::vector<PeerHint>(const QString&)> provider) {
        metadataPeerProvider_ = std::move(provider);
    }
    void setPeerObserver(std::function<void(const QString&, const QString&, uint16_t, bool)> observer) {
        peerObserver_ = std::move(observer);
    }

signals:
    void hashQueued(const QString& hash, DiscoverySource source, bool hasPeer);
    void metadataStarted(const QString& hash, bool hasPeer);
    void metadataEnded(const QString& hash, const QString& outcome);
    void statusChanged(const QString& status);
    // Emitted once metadata has been fetched and a full torrent model built.
    void discovered(const rats::domain::Torrent& torrent);
    void discoveredWithTask(const rats::domain::Torrent& torrent, uint64_t taskId);

private slots:
    void onSpiderWalk();
    void onIgnoreToggle();
    void processMetadataQueue();
    void onSampleTick();

private:
    // A pending metadata fetch.
    // An empty peerIp means "no announcing peer known" — fall back to a DHT
    // search (slow path) instead of a direct peer connection (fast path).
    struct MetadataRequest {
        QString infoHash;
        QString peerIp;
        uint16_t peerPort = 0;
        DiscoverySource source = DiscoverySource::Sample;
    };

    // Borrowed BitTorrent subsystem (null if the transport has none).
    librats::Bittorrent* bittorrent() const;

    // announce_peer callback, already marshalled onto the Qt thread.
    void onAnnounce(const std::array<uint8_t, 20>& infoHash, const std::string& ip, uint16_t port,
        uint64_t generation);
    void onGetPeers(const std::array<uint8_t, 20>& infoHash, uint64_t generation);
    void acceptInfoHash(const std::array<uint8_t, 20>& infoHash, const QString& peerIp,
        uint16_t peerPort, uint64_t generation, DiscoverySource source = DiscoverySource::Sample);
    void onSampleResult(const QString& nodeKey, const librats::SampleInfohashesResult& result,
        uint64_t generation);

    // Kick off a BEP 9 metadata fetch (fast path if peer is known, else DHT).
    void fetchMetadata(const MetadataRequest& request);

    // Handle a completed metadata fetch (runs on the Qt thread).
    void onMetadataReceived(const rats::domain::Torrent& torrent, const QString& infoHash,
        uint64_t generation);
    void onMetadataFailed(const QString& infoHash, bool hadPeer, const QString& error,
        uint64_t generation);
    void rememberCooldown(const QString& infoHash, bool hadPeer);

    // Tuning defaults — no magic numbers scattered through the logic.
    static constexpr int DEFAULT_WALK_INTERVAL_MS = 100;
    static constexpr int DEFAULT_IGNORE_INTERVAL_MS = 1000;
    static constexpr int METADATA_QUEUE_INTERVAL_MS = 100;
    static constexpr int MAX_CONCURRENT_METADATA_FETCHES = 10;
    static constexpr int SAMPLE_INTERVAL_MS = 15000;
    static constexpr int MAX_METADATA_QUEUE = 256;
    static constexpr int MAX_SAMPLES_PER_RESPONSE = 64;
    static constexpr int RETRY_DELAY_MS = 300000;
    static constexpr int SAMPLE_FAILURE_DELAY_MS = 300000;
    static constexpr int SAMPLE_MIN_DELAY_MS = 60000;
    static constexpr size_t MAX_RETRY_HASHES = 10000;

    struct CallbackGate {
        std::mutex mutex;
        Crawler* crawler = nullptr;
    };
    struct RetryState {
        qint64 afterMs = 0;
        bool hadPeer = false;
    };

    friend class TestCrawlerSampling;

    P2PTransport* transport_; // borrowed, non-owning

    std::atomic<bool> running_;
    std::atomic<uint64_t> generation_ { 0 };
    std::atomic<int> discoveredCount_;
    std::atomic<int> activeFetches_;
    std::atomic<int> pendingFetches_ { 0 }; // includes old-generation requests draining after stop
    std::atomic<int> fetchSuccessCount_;
    std::atomic<int> fetchErrorCount_;
    std::atomic<int> visitedNodesCount_;
    std::atomic<int> sampleRequestCount_ { 0 };
    std::atomic<int> sampleResponseCount_ { 0 };
    std::atomic<int> sampleHashCount_ { 0 };
    std::atomic<int> hashEnqueuedCount_ { 0 };

    QTimer* walkTimer_;
    QTimer* ignoreTimer_;
    QTimer* metadataQueueTimer_;
    QTimer* sampleTimer_;
    QElapsedTimer clock_;
    std::shared_ptr<CallbackGate> callbackGate_;
    QHash<QString, qint64> sampleNextAllowed_;
    std::set<QString> sampleInFlight_;
    size_t candidateCursor_ = 0;

    int walkIntervalMs_;

    std::queue<MetadataRequest> metadataQueue_;
    mutable std::mutex queueMutex_;

    std::set<QString> pendingHashes_; // queued or fetching in the current generation
    QHash<QString, uint64_t> activeMetadataTaskIds_; // only fetching; guards late peer hints
    QHash<QString, RetryState> retryAfter_;

    KnownHashFilter knownHashFilter_;
    std::function<std::vector<PeerHint>(const QString&)> metadataPeerProvider_;
    std::function<void(const QString&, const QString&, uint16_t, bool)> peerObserver_;
};

} // namespace rats::net

#endif // RATS_NET_CRAWLER_H
