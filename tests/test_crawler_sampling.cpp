#include <QtTest>
#include <QTemporaryDir>

#include "data/database.h"
#include "data/manticore.h"
#include "data/torrent_repository.h"
#include "net/crawler.h"
#include "net/librats_convert.h"
#include "services/indexing_service.h"

#pragma push_macro("emit")
#pragma push_macro("slots")
#pragma push_macro("signals")
#undef emit
#undef slots
#undef signals
#include "librats/dht/dht.h"
#include "librats/bittorrent/bencode.h"
#include "librats/bittorrent/torrent_info.h"
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
    void feedRecordIsBackfilledFromVerifiedMetadata()
    {
        QTemporaryDir isolated;
        QVERIFY(isolated.isValid());
        data::Manticore searchd(isolated.path());
        QVERIFY(searchd.start());
        data::Database db(&searchd);
        data::TorrentRepository repo(&db);
        service::IndexingService indexing(&repo, nullptr);

        librats::BencodeValue infoDict = librats::BencodeValue::create_dict();
        infoDict["name"] = librats::BencodeValue("source-name.bin");
        infoDict["length"] = librats::BencodeValue(int64_t(10));
        infoDict["piece length"] = librats::BencodeValue(int64_t(16384));
        infoDict["pieces"] = librats::BencodeValue(std::string(20, 'x'));
        const auto parsed = librats::bittorrent::TorrentInfo::from_info_dict(
            infoDict.encode(), librats::bittorrent::InfoHash {});
        QVERIFY(parsed.has_value());
        QVERIFY(parsed->has_metadata());
        auto wrongHash = parsed->info_hash();
        wrongHash[0] ^= 0xff;
        QVERIFY(!librats::bittorrent::TorrentInfo::from_info_dict(infoDict.encode(), wrongHash).has_value());
        const QString key = QString::fromStdString(parsed->info_hash_hex());

        domain::Torrent partial = toDomainTorrent(key, *parsed);
        partial.name = QStringLiteral("preserve existing name");
        partial.fileList.clear(); // Feed carries a count, but no concrete files.
        partial.ipv4 = QStringLiteral("192.0.2.10");
        partial.added = QDateTime::fromSecsSinceEpoch(1000000);
        QVERIFY(indexing.insert(partial).success);
        QCOMPARE(repo.inspectMetadataNeed(key), data::TorrentRepository::MetadataNeed::MissingFiles);

        Crawler crawler(nullptr);
        crawler.running_ = true; // no socket or public network in this fixture
        crawler.generation_ = 1;
        crawler.setKnownHashFilter([&repo](const QString& hashKey) {
            using Need = data::TorrentRepository::MetadataNeed;
            switch (repo.inspectMetadataNeed(hashKey)) {
            case Need::New:
            case Need::MissingFiles: return Crawler::MetadataDecision::Fetch;
            case Need::CompleteOrAmbiguous: return Crawler::MetadataDecision::Skip;
            case Need::Error: return Crawler::MetadataDecision::Error;
            }
            return Crawler::MetadataDecision::Error;
        });
        service::IndexingService::Result persisted;
        QObject::connect(&crawler, &Crawler::discovered, &indexing,
            [&](const domain::Torrent& torrent) {
                persisted = indexing.insert(torrent);
                if (persisted.success && (!persisted.alreadyExists || persisted.filesBackfilled))
                    crawler.recordIndexed();
            });

        crawler.onSampleResult("fixture-node", [&] {
            librats::SampleInfohashesResult result;
            result.status = librats::SampleInfohashesStatus::Success;
            result.samples = { parsed->info_hash() };
            return result;
        }(), 1);
        QCOMPARE(crawler.metadataQueue_.size(), size_t(1));
        QCOMPARE(crawler.queuedFetches(), 1);
        crawler.onGetPeers(parsed->info_hash(), 1);
        QCOMPARE(crawler.metadataQueue_.size(), size_t(1));
        crawler.onAnnounce(parsed->info_hash(), "127.0.0.2", 6881, 1);
        QCOMPARE(crawler.metadataQueue_.size(), size_t(1));
        QCOMPARE(crawler.metadataQueue_.front().peerIp, QStringLiteral("127.0.0.2"));
        crawler.metadataQueue_.pop(); // simulate BEP 9 dispatch; same task/limits

        // Feed the parsed, hash-verified info dict through the normal success
        // callback and the discovered -> IndexingService signal connection.
        crawler.onMetadataReceived(toDomainTorrent(key, *parsed), key, 1);
        QVERIFY(persisted.success);
        QVERIFY(persisted.alreadyExists);
        QVERIFY(persisted.filesBackfilled);
        QCOMPARE(crawler.discoveredCount(), 1);
        QTRY_COMPARE(repo.inspectFiles(key, 1).state, data::TorrentRepository::FileLookup::State::Present);
        const auto stored = repo.get(key);
        QVERIFY(stored.has_value());
        QCOMPARE(stored->name, partial.name);
        QCOMPARE(stored->ipv4, partial.ipv4);
        QCOMPARE(stored->added.toSecsSinceEpoch(), partial.added.toSecsSinceEpoch());
        QCOMPARE(repo.inspectFiles(key, 1).files.size(), 1);
        QCOMPARE(repo.inspectMetadataNeed(key), data::TorrentRepository::MetadataNeed::CompleteOrAmbiguous);

        crawler.retryAfter_.clear();
        crawler.onGetPeers(parsed->info_hash(), 1);
        QVERIFY(crawler.metadataQueue_.empty()); // complete: no second task
        const auto again = indexing.insert(toDomainTorrent(key, *parsed));
        QVERIFY(again.success);
        QVERIFY(!again.filesBackfilled);

        // A failed metadata attempt leaves the Feed record intact and obeys
        // the existing cooldown before a later discovery can retry it.
        infoDict["name"] = librats::BencodeValue("other.bin");
        const auto other = librats::bittorrent::TorrentInfo::from_info_dict(
            infoDict.encode(), librats::bittorrent::InfoHash {});
        QVERIFY(other.has_value());
        const QString otherKey = QString::fromStdString(other->info_hash_hex());
        domain::Torrent otherPartial = toDomainTorrent(otherKey, *other);
        otherPartial.fileList.clear();
        QVERIFY(indexing.insert(otherPartial).success);
        crawler.onGetPeers(other->info_hash(), 1);
        QCOMPARE(crawler.metadataQueue_.size(), size_t(1));
        crawler.metadataQueue_.pop();
        crawler.onMetadataFailed(otherKey, false, QStringLiteral("fixture timeout"), 1);
        QCOMPARE(crawler.discoveredCount(), 1);
        QCOMPARE(repo.inspectMetadataNeed(otherKey), data::TorrentRepository::MetadataNeed::MissingFiles);
        crawler.onGetPeers(other->info_hash(), 1);
        QVERIFY(crawler.metadataQueue_.empty());
        crawler.retryAfter_[otherKey].afterMs = crawler.clock_.elapsed() - 1;
        crawler.onGetPeers(other->info_hash(), 1);
        QCOMPARE(crawler.metadataQueue_.size(), size_t(1));
        crawler.metadataQueue_.pop();

        infoDict["name"] = librats::BencodeValue("new-resource.bin");
        const auto fresh = librats::bittorrent::TorrentInfo::from_info_dict(
            infoDict.encode(), librats::bittorrent::InfoHash {});
        QVERIFY(fresh.has_value());
        const QString freshKey = QString::fromStdString(fresh->info_hash_hex());
        QCOMPARE(repo.inspectMetadataNeed(freshKey), data::TorrentRepository::MetadataNeed::New);
        crawler.onGetPeers(fresh->info_hash(), 1);
        QCOMPARE(crawler.metadataQueue_.size(), size_t(1));
        crawler.metadataQueue_.pop();
        crawler.onMetadataReceived(toDomainTorrent(freshKey, *fresh), freshKey, 1);
        QVERIFY(repo.exists(freshKey));
        QTRY_COMPARE(repo.inspectFiles(freshKey, 1).state, data::TorrentRepository::FileLookup::State::Present);
        QCOMPARE(crawler.discoveredCount(), 2);
        crawler.stop();
        QCOMPARE(crawler.queuedFetches(), 0);
        searchd.stop();
    }

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
        crawler.setKnownHashFilter([](const QString& key) {
            return key.startsWith("0009") ? Crawler::MetadataDecision::Skip : Crawler::MetadataDecision::Fetch;
        });
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

    void lateAnnounceKeepsOneInFlightTaskAndOldGenerationIsIgnored()
    {
        Crawler crawler(nullptr);
        crawler.running_ = true;
        crawler.generation_ = 1;
        librats::SampleInfohashesResult reply;
        reply.status = librats::SampleInfohashesStatus::Success;
        reply.samples = {hash(10)};
        crawler.onSampleResult("node-a", reply, 1);
        QCOMPARE(crawler.metadataQueue_.size(), size_t(1));
        const QString key = crawler.metadataQueue_.front().infoHash;
        crawler.metadataQueue_.pop(); // the request is now fetching
        crawler.activeMetadataTaskIds_.insert(key, 42);

        crawler.onAnnounce(hash(10), "127.0.0.2", 6881, 1);
        crawler.onAnnounce(hash(10), "127.0.0.2", 6881, 1);
        QVERIFY(crawler.metadataQueue_.empty());
        QCOMPARE(crawler.hashEnqueuedCount(), 1);
        QCOMPARE(crawler.activeMetadataTaskIds_.value(key), uint64_t(42));

        crawler.stop();
        QVERIFY(crawler.activeMetadataTaskIds_.empty());
        crawler.running_ = true;
        crawler.onAnnounce(hash(10), "127.0.0.2", 6881, 1);
        QVERIFY(crawler.metadataQueue_.empty()); // old Node callback cannot enter a new run
        crawler.stop();
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
        crawler.setKnownHashFilter([](const QString& key) {
            return key.startsWith("0004") ? Crawler::MetadataDecision::Skip : Crawler::MetadataDecision::Fetch;
        });
        crawler.acceptInfoHash(hash(4), {}, 0, 1);
        QCOMPARE(crawler.metadataQueue_.size(), size_t(3));
        crawler.setKnownHashFilter([](const QString&) { return Crawler::MetadataDecision::Error; });
        crawler.acceptInfoHash(hash(5), {}, 0, 1);
        QCOMPARE(crawler.metadataQueue_.size(), size_t(3)); // failed lookup is not a missing resource
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
