#include <QtTest>

#include "net/crawler.h"

#pragma push_macro("emit")
#pragma push_macro("slots")
#pragma push_macro("signals")
#undef emit
#undef slots
#undef signals
#include "librats/dht/dht.h"
#pragma pop_macro("signals")
#pragma pop_macro("slots")
#pragma pop_macro("emit")

namespace rats::net {

class TestCrawlerSampling : public QObject {
    Q_OBJECT

    static std::array<uint8_t, 20> hash(uint16_t n)
    {
        std::array<uint8_t, 20> out{};
        out[0] = static_cast<uint8_t>(n >> 8);
        out[1] = static_cast<uint8_t>(n);
        return out;
    }

private slots:
    void incomingGetPeersUsesHashOnlyQueueAndStopsCleanly()
    {
        Crawler crawler(nullptr);
        crawler.running_ = true;
        crawler.generation_ = 1;
        crawler.onGetPeers(hash(8), 1);
        crawler.onGetPeers(hash(8), 1);
        QCOMPARE(crawler.metadataQueue_.size(), size_t(1));
        QCOMPARE(crawler.hashEnqueuedCount(), 1);
        QVERIFY(crawler.metadataQueue_.front().peerIp.isEmpty());
        QCOMPARE(crawler.metadataQueue_.front().peerPort, uint16_t(0));
        QVERIFY(crawler.metadataQueue_.front().fromGetPeers);
        crawler.setKnownHashFilter([](const QString& key) { return key.startsWith("0009"); });
        crawler.onGetPeers(hash(9), 1);
        QCOMPARE(crawler.metadataQueue_.size(), size_t(1));
        const QString pendingHash = crawler.metadataQueue_.front().infoHash;
        crawler.metadataQueue_.pop(); // simulate dispatch before the failed BEP 9 task
        crawler.onMetadataFailed(pendingHash, false, {}, 1);
        crawler.onGetPeers(hash(8), 1);
        QVERIFY(crawler.metadataQueue_.empty());
        crawler.onAnnounce(hash(8), "127.0.0.2", 6881, 1);
        QCOMPARE(crawler.metadataQueue_.size(), size_t(1));
        QCOMPARE(crawler.metadataQueue_.front().peerPort, uint16_t(6881));
        crawler.stop();
        crawler.onGetPeers(hash(9), 1);
        QVERIFY(crawler.metadataQueue_.empty());
    }

    void sampleAndAnnounceUseSameQueue()
    {
        Crawler crawler(nullptr);
        crawler.running_ = true; // isolated queue test: no DHT or BT socket
        crawler.generation_ = 1;
        librats::SampleInfohashesResult reply;
        reply.status = librats::SampleInfohashesStatus::Success;
        reply.interval = 21600;
        reply.samples = {hash(1), hash(2)};
        crawler.onSampleResult("node-a", reply, 1);
        QCOMPARE(crawler.sampleResponseCount(), 1);
        QCOMPARE(crawler.sampleHashCount(), 2);
        QCOMPARE(crawler.hashEnqueuedCount(), 2);
        QCOMPARE(crawler.metadataQueue_.size(), size_t(2));
        QVERIFY(crawler.metadataQueue_.front().peerIp.isEmpty()); // BEP 9 uses DHT lookup
        QVERIFY(crawler.sampleNextAllowed_.value("node-a") - crawler.clock_.elapsed() > 21599000);

        crawler.onAnnounce(hash(1), "127.0.0.2", 6881, 1);
        QCOMPARE(crawler.metadataQueue_.size(), size_t(2));
        QCOMPARE(crawler.metadataQueue_.front().peerIp, QStringLiteral("127.0.0.2"));
        QCOMPARE(crawler.metadataQueue_.front().peerPort, uint16_t(6881));
        crawler.onAnnounce(hash(3), "127.0.0.3", 6882, 1);
        QCOMPARE(crawler.metadataQueue_.size(), size_t(3));
        QCOMPARE(crawler.hashEnqueuedCount(), 3);
        crawler.setKnownHashFilter([](const QString& key) { return key.startsWith("0004"); });
        crawler.acceptInfoHash(hash(4), {}, 0, 1);
        QCOMPARE(crawler.metadataQueue_.size(), size_t(3));
        crawler.stop();
    }

    void queueCapAndRetryWithNewPeer()
    {
        Crawler crawler(nullptr);
        crawler.running_ = true;
        crawler.generation_ = 1;
        for (uint16_t i = 0; i < Crawler::MAX_METADATA_QUEUE + 1; ++i)
            crawler.acceptInfoHash(hash(i), {}, 0, 1);
        QCOMPARE(crawler.metadataQueue_.size(), size_t(Crawler::MAX_METADATA_QUEUE));
        QCOMPARE(crawler.hashEnqueuedCount(), Crawler::MAX_METADATA_QUEUE);
        crawler.onAnnounce(hash(Crawler::MAX_METADATA_QUEUE + 1), "127.0.0.5", 6881, 1);
        QCOMPARE(crawler.metadataQueue_.size(), size_t(Crawler::MAX_METADATA_QUEUE));
        QCOMPARE(crawler.metadataQueue_.back().peerIp, QStringLiteral("127.0.0.5"));
        const QString firstHash = crawler.metadataQueue_.front().infoHash;
        crawler.metadataQueue_.pop(); // simulate dispatch of a hash-only request
        crawler.onMetadataFailed(firstHash, false, QStringLiteral("fixture failure"), 1);
        crawler.acceptInfoHash(hash(1), {}, 0, 1);
        QCOMPARE(crawler.metadataQueue_.size(), size_t(Crawler::MAX_METADATA_QUEUE - 1));
        crawler.onAnnounce(hash(1), "127.0.0.4", 6881, 1);
        QCOMPARE(crawler.metadataQueue_.size(), size_t(Crawler::MAX_METADATA_QUEUE));
        QCOMPARE(crawler.metadataQueue_.back().peerIp, QStringLiteral("127.0.0.4"));
        crawler.stop();
    }

    void directPeerFailureRetriesAfterCooldown()
    {
        Crawler crawler(nullptr);
        crawler.running_ = true;
        crawler.generation_ = 1;
        crawler.onAnnounce(hash(9), "127.0.0.4", 6881, 1);
        const QString key = crawler.metadataQueue_.front().infoHash;
        crawler.metadataQueue_.pop(); // dispatched
        crawler.onMetadataFailed(key, true, QStringLiteral("fixture failure"), 1);
        crawler.onAnnounce(hash(9), "127.0.0.4", 6881, 1);
        QVERIFY(crawler.metadataQueue_.empty());
        crawler.retryAfter_[key].afterMs = crawler.clock_.elapsed() - 1;
        crawler.onAnnounce(hash(9), "127.0.0.4", 6881, 1);
        QCOMPARE(crawler.metadataQueue_.size(), size_t(1));
        crawler.stop();
    }

    void stopRejectsOldSampleAndClearsQueue()
    {
        Crawler crawler(nullptr);
        crawler.running_ = true;
        crawler.generation_ = 1;
        librats::SampleInfohashesResult reply;
        reply.status = librats::SampleInfohashesStatus::Success;
        reply.samples = {hash(7)};
        crawler.onSampleResult("node-b", reply, 1);
        QCOMPARE(crawler.metadataQueue_.size(), size_t(1));
        crawler.stop();
        QVERIFY(crawler.metadataQueue_.empty());
        QVERIFY(crawler.pendingHashes_.empty());
        crawler.running_ = true;
        crawler.onSampleResult("node-b", reply, 1);
        QCOMPARE(crawler.sampleHashCount(), 1);
        QVERIFY(crawler.metadataQueue_.empty());
        crawler.stop();
    }
};

} // namespace rats::net

QTEST_MAIN(rats::net::TestCrawlerSampling)
#include "test_crawler_sampling.moc"
