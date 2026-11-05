#include <QCoreApplication>
#include <QJsonObject>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QUdpSocket>
#include <QtTest>

#include "app/application.h"
#include "app/config_store.h"
#include "data/torrent_repository.h"
#include "domain/torrent.h"
#include "net/p2p_transport.h"
#include "peer/peer_api.h"
#include "rest/api_router.h"
#include "services/search_service.h"
#include "services/peer_registry.h"

// librats' EventBus::emit collides with Qt's keyword macros.
#pragma push_macro("emit")
#pragma push_macro("slots")
#pragma push_macro("signals")
#undef emit
#undef slots
#undef signals
#include "librats/node/node.h"
#include "librats/subsystems/message_json.h"
#pragma pop_macro("signals")
#pragma pop_macro("slots")
#pragma pop_macro("emit")

#include <atomic>

class TestIndexSharingLoopback : public QObject {
    Q_OBJECT

private slots:
    void togglesActualPeerRepliesWithoutStoppingP2p()
    {
        if (!qEnvironmentVariableIsSet("RATS_LOOPBACK_SANDBOX"))
            QSKIP("Run explicitly inside a sandbox that denies non-loopback outbound traffic");

        QTemporaryDir data;
        QVERIFY(data.isValid());
        QTcpServer tcpPort;
        QUdpSocket dhtPort;
        QVERIFY(tcpPort.listen(QHostAddress::LocalHost, 0));
        QVERIFY(dhtPort.bind(QHostAddress::LocalHost, 0));
        const int p2p = tcpPort.serverPort();
        const int dht = dhtPort.localPort();
        tcpPort.close();
        dhtPort.close();

        rats::app::ConfigStore config(data.path() + "/rats.json");
        config.setNetworkEnabled(true);
        config.setIndexerEnabled(false);
        config.setTrackersEnabled(false);
        config.setDatabaseSharing(false);
        config.setP2pReplication(false);
        QVERIFY(config.save());

        qputenv("RATS_DHT_BOOTSTRAP_NODES", "127.0.0.1:9");

        rats::app::Application::Options options;
        options.dataDirectory = data.path();
        options.clientVersion = QStringLiteral("test");
        options.headless = true;
        options.p2pPort = p2p;
        options.dhtPort = dht;
        rats::app::Application app(options);
        QVERIFY(app.start());
        QVERIFY(app.transport()->isDhtRunning());
        QVERIFY(!app.featureStatus()["indexSharingRunning"].toBool());

        librats::NodeConfig clientConfig;
        clientConfig.bind_address = "127.0.0.1";
        clientConfig.enable_listen = false;
        clientConfig.protocol = "rats-search/4";
        clientConfig.security = librats::NodeConfig::Security::Noise;
        clientConfig.data_dir = (data.path() + "/peer").toStdString();
        librats::Node client(std::move(clientConfig));
        auto* messages = client.add_subsystem(std::make_unique<librats::MessageJson>());
        std::atomic<int> replies { 0 };
        std::atomic<int> feedRequests { 0 };
        std::atomic<int> randomReplies { 0 };
        std::atomic<int> torrentRequests { 0 };
        std::atomic<int> webSearchRequests { 0 };
        std::atomic<int> legacyRequests { 0 };
        std::atomic<int> servedSearchReplies { 0 };
        std::atomic<int> advertisedTorrents { -1 };
        messages->on("feed_response", [&replies](const librats::PeerId&, const librats::Json&) { ++replies; });
        messages->on("feed", [&feedRequests](const librats::PeerId&, const librats::Json&) { ++feedRequests; });
        messages->on("randomTorrents_response", [&randomReplies](const librats::PeerId&, const librats::Json&) { ++randomReplies; });
        messages->on("torrent", [&torrentRequests](const librats::PeerId&, const librats::Json&) { ++torrentRequests; });
        messages->on("searchTorrent", [messages, &webSearchRequests, &legacyRequests](const librats::PeerId&, const librats::Json& request) {
            ++webSearchRequests;
            const std::string id = request.value("searchId", std::string());
            const std::string text = request.value("text", std::string());
            if (text.rfind("silent", 0) == 0) return;
            if (text == "flood") {
                QMetaObject::invokeMethod(QCoreApplication::instance(), [messages]() {
                    messages->send("searchTorrent_response", librats::Json {
                        { "hash", "invalid" }, { "name", "flood invalid hash" } });
                    for (int i = 0; i < 120; ++i) {
                        messages->send("searchTorrent_response", librats::Json {
                            { "hash", QString("%1").arg(i, 40, 16, QLatin1Char('0')).toStdString() },
                            { "name", "flood result " + std::to_string(i) } });
                    }
                }, Qt::QueuedConnection);
                return;
            }
            if (text == "legacyonly") {
                if (request.value("query", std::string()) == text) ++legacyRequests;
                QMetaObject::invokeMethod(QCoreApplication::instance(), [messages]() {
                    messages->send("searchTorrent_response", librats::Json {
                        { "hash", std::string(40, 'c') }, { "name", "legacyonly result" } });
                }, Qt::QueuedConnection);
                return;
            }
            QMetaObject::invokeMethod(QCoreApplication::instance(), [messages, id]() {
                messages->send("searchTorrent_response", librats::Json {
                    { "hash", std::string(40, 'b') }, { "name", "controlled remote result" },
                    { "size", 1234 }, { "searchId", id } });
                messages->send("searchTorrent_response", librats::Json {
                    { "hash", std::string(40, 'b') }, { "name", "duplicate result" },
                    { "searchId", id } });
                messages->send("searchTorrent_response", librats::Json {
                    { "hash", std::string(40, 'c') }, { "name", "unrelated legacy result" } });
            }, Qt::QueuedConnection);
        });
        messages->on("searchFiles", [messages](const librats::PeerId&, const librats::Json& request) {
            const std::string text = request.value("text", std::string());
            if (text.rfind("silent", 0) == 0 || text == "legacyonly" || text == "flood") return;
            const std::string id = request.value("searchId", std::string());
            QMetaObject::invokeMethod(QCoreApplication::instance(), [messages, id]() {
                messages->send("searchFiles_response", librats::Json {
                    { "hash", std::string(40, 'b') }, { "name", "file match duplicate" },
                    { "isFileMatch", true }, { "searchId", id } });
            }, Qt::QueuedConnection);
        });
        messages->on("searchTorrent_response", [&servedSearchReplies](const librats::PeerId&, const librats::Json& response) {
            if (response.value("searchId", std::string()) == "5a72c36d-8045-4de6-84d7-c28b24633c22"
                && response.value("hash", std::string()) == std::string(40, 'd'))
                ++servedSearchReplies;
        });
        messages->on("client_info", [&advertisedTorrents](const librats::PeerId&, const librats::Json& info) {
            advertisedTorrents = info.value("torrents", 0);
        });
        QVERIFY(client.start());
        client.connect("127.0.0.1", static_cast<uint16_t>(p2p));
        QTRY_VERIFY_WITH_TIMEOUT(client.peer_count() == 1 && app.transport()->peerCount() == 1, 8000);
        QTRY_COMPARE_WITH_TIMEOUT(advertisedTorrents.load(), 0, 4000);
        app.peers()->updateOurStats(37, 40, 1000);
        QTest::qWait(200);
        QCOMPARE(feedRequests.load(), 0); // passive index sharing never pulls a peer's feed

        const QString hash(40, QLatin1Char('a'));
        QSignalSpy remoteHits(app.peerApi(), &rats::peer::PeerApi::remoteSearchResults);
        QSignalSpy apiEvents(app.api(), &rats::rest::ApiRouter::event);
        const auto eventCount = [&apiEvents](const QString& name) {
            int count = 0;
            for (const QList<QVariant>& args : apiEvents) {
                if (args.at(0).toString() == name) ++count;
            }
            return count;
        };
        const librats::Json searchHit { { "hash", hash.toStdString() }, { "name", "loopback result" } };
        messages->send("searchTorrent_response", searchHit);
        QTest::qWait(200);
        QCOMPARE(remoteHits.size(), 0);
        app.peerApi()->requestTorrent(QString::fromStdString(client.local_id().to_hex()), hash);
        QTest::qWait(200);
        QCOMPARE(torrentRequests.load(), 0);
        app.config()->setRemoteSearchEnabled(true);
        QVERIFY(!app.config()->indexExchangeEnabled());
        messages->send("searchTorrent_response", searchHit);
        QTRY_COMPARE_WITH_TIMEOUT(remoteHits.size(), 1, 4000);
        app.peerApi()->requestTorrent(QString::fromStdString(client.local_id().to_hex()), hash);
        QTRY_COMPARE_WITH_TIMEOUT(torrentRequests.load(), 1, 4000);
        app.config()->setRemoteSearchEnabled(false);
        messages->send("searchTorrent_response", searchHit);
        QTest::qWait(200);
        QCOMPARE(remoteHits.size(), 1);

        // Web search is opt-in and independent of outward index sharing.
        const QString searchId = QStringLiteral("b430baf0-0fb7-4c06-b44d-33e84dca7ae3");
        bool blockedDone = false;
        rats::Result blocked;
        app.api()->call("search.remote", { { "query", "controlled" }, { "searchId", searchId } },
            [&](const rats::Result& result) { blocked = result; blockedDone = true; });
        QVERIFY(blockedDone);
        QVERIFY(!blocked.ok());
        QCOMPARE(webSearchRequests.load(), 0);

        app.config()->setRemoteSearchEnabled(true);
        QVERIFY(!app.config()->indexExchangeEnabled());
        bool localDone = false;
        app.api()->call("search.torrents", { { "query", "controlled" }, { "localOnly", true } },
            [&](const rats::Result& result) { QVERIFY(result.ok()); localDone = true; });
        QVERIFY(localDone);
        QCOMPARE(webSearchRequests.load(), 0);

        bool remoteDone = false;
        rats::Result remote;
        app.api()->call("search.remote", { { "query", "controlled" }, { "searchId", searchId } },
            [&](const rats::Result& result) { remote = result; remoteDone = true; });
        QTRY_VERIFY_WITH_TIMEOUT(eventCount(QStringLiteral("remoteSearchResults")) > 0, 2000);
        QVERIFY(!remoteDone); // confirmed hits stream before the response window closes
        QTRY_VERIFY_WITH_TIMEOUT(remoteDone, 7500);
        QVERIFY(remote.ok());
        const QJsonArray items = remote.data().toObject()["items"].toArray();
        QCOMPARE(items.size(), 1); // same hash from torrent and file responses
        QCOMPARE(items.first().toObject()["hash"].toString(), QString(40, QLatin1Char('b')));
        QVERIFY(items.first().toObject()["remote"].toBool());
        QCOMPARE(remote.data().toObject()["candidates"].toArray().size(), 0);
        QVERIFY(eventCount(QStringLiteral("remoteSearchResults")) > 0);
        QVERIFY(!app.search()->get(QString(40, QLatin1Char('b')), false).has_value());
        QVERIFY(webSearchRequests.load() > 0);

        // A legacy peer receives the unchanged request and answers without an
        const int seenBeforeLegacy = remoteHits.size();
        bool legacyDone = false;
        rats::Result legacy;
        const QString legacyId = QStringLiteral("8462e8ae-2ba1-419a-8c08-cf5f293eb244");
        app.api()->call("search.remote", { { "query", "legacyonly" }, { "searchId", legacyId } },
            [&](const rats::Result& result) { legacy = result; legacyDone = true; });
        QTRY_VERIFY_WITH_TIMEOUT(eventCount(QStringLiteral("remoteSearchCandidates")) > 0, 2000);
        QVERIFY(!legacyDone); // old-protocol candidates also stream immediately
        QTRY_VERIFY_WITH_TIMEOUT(legacyDone, 7500);
        QCOMPARE(legacyRequests.load(), 1);
        QVERIFY(remoteHits.size() > seenBeforeLegacy);
        QVERIFY(legacy.ok());
        QCOMPARE(legacy.data().toObject()["items"].toArray().size(), 0);
        const QJsonArray legacyCandidates = legacy.data().toObject()["candidates"].toArray();
        QCOMPARE(legacyCandidates.size(), 1);
        QCOMPARE(legacyCandidates.first().toObject()["hash"].toString(), QString(40, QLatin1Char('c')));
        QVERIFY(!legacyCandidates.first().toObject()["peer"].toString().isEmpty());
        QVERIFY(eventCount(QStringLiteral("remoteSearchCandidates")) > 0);
        for (const QList<QVariant>& args : apiEvents) {
            if (args.at(0).toString() == QStringLiteral("remoteSearchCandidates"))
                QCOMPARE(args.at(1).toJsonObject()["candidateSessionId"].toString(), legacyId);
        }
        QVERIFY(!app.search()->get(QString(40, QLatin1Char('c')), false).has_value());

        // Concurrent Web windows must not receive each other's unverified
        // candidate event, even though neither event proves protocol ownership.
        const QString applesId = QStringLiteral("22729cba-40fa-4df6-a553-d132041c3964");
        const QString orangesId = QStringLiteral("f5dbe5ea-9c07-4272-9e3b-85ee50cfdd3e");
        app.api()->call("search.remote", { { "query", "silent apples" }, { "searchId", applesId } },
            [&](const rats::Result&) {});
        app.api()->call("search.remote", { { "query", "silent oranges" }, { "searchId", orangesId } },
            [&](const rats::Result&) {});
        const int eventsBeforeConcurrent = apiEvents.size();
        messages->send("searchTorrent_response", librats::Json {
            { "hash", std::string(40, 'a') }, { "name", "silent apples result" } });
        QTRY_VERIFY_WITH_TIMEOUT(apiEvents.size() > eventsBeforeConcurrent, 2000);
        for (int i = eventsBeforeConcurrent; i < apiEvents.size(); ++i) {
            const QList<QVariant>& args = apiEvents.at(i);
            if (args.at(0).toString() == QStringLiteral("remoteSearchCandidates"))
                QCOMPARE(args.at(1).toJsonObject()["candidateSessionId"].toString(), applesId);
        }
        app.api()->call("search.cancel", { { "searchId", applesId } }, [&](const rats::Result&) {});
        app.api()->call("search.cancel", { { "searchId", orangesId } }, [&](const rats::Result&) {});

        // A timed-out empty request finishes, and later legacy packets have no
        // reliable way to distinguish the old request from a newer one.
        bool silentDone = false;
        rats::Result silent;
        app.api()->call("search.remote", { { "query", "silent" },
            { "searchId", "8343dbe2-2783-4af2-ae9a-35ed72c46631" } },
            [&](const rats::Result& result) { silent = result; silentDone = true; });
        QTRY_VERIFY_WITH_TIMEOUT(silentDone, 7500);
        QVERIFY(silent.ok());
        QCOMPARE(silent.data().toObject()["items"].toArray().size(), 0);
        QCOMPARE(silent.data().toObject()["candidates"].toArray().size(), 0);
        const int candidatesBeforeLate = eventCount(QStringLiteral("remoteSearchCandidates"));
        messages->send("searchTorrent_response", librats::Json {
            { "hash", std::string(40, 'e') }, { "name", "silent late reply" } });
        QTest::qWait(150);
        QCOMPARE(eventCount(QStringLiteral("remoteSearchCandidates")), candidatesBeforeLate);

        bool revokedDone = false;
        rats::Result revoked;
        app.api()->call("search.remote", { { "query", "silent" },
            { "searchId", "15b14661-1c8a-4fcb-bcd9-f07180bfb0a2" } },
            [&](const rats::Result& result) { revoked = result; revokedDone = true; });
        app.config()->setRemoteSearchEnabled(false);
        QVERIFY(revokedDone);
        QVERIFY(!revoked.ok());
        messages->send("searchTorrent_response", librats::Json {
            { "hash", std::string(40, 'e') }, { "name", "silent revoked reply" } });
        QTest::qWait(150);
        QCOMPARE(eventCount(QStringLiteral("remoteSearchCandidates")), candidatesBeforeLate);
        app.config()->setRemoteSearchEnabled(true);

        const QString cancelledId = QStringLiteral("db8e80e1-e9b6-45a0-900a-81b3c82c2fe5");
        bool cancelledDone = false;
        rats::Result cancelledResult;
        app.api()->call("search.remote", { { "query", "controlled" }, { "searchId", cancelledId } },
            [&](const rats::Result& result) { cancelledResult = result; cancelledDone = true; });
        app.api()->call("search.cancel", { { "searchId", cancelledId } },
            [&](const rats::Result& result) { QVERIFY(result.data().toObject()["cancelled"].toBool()); });
        QVERIFY(cancelledDone);
        QVERIFY(!cancelledResult.ok());
        messages->send("searchTorrent_response", librats::Json {
            { "hash", std::string(40, 'e') }, { "name", "controlled after cancel" } });
        QTest::qWait(150);
        QCOMPARE(eventCount(QStringLiteral("remoteSearchCandidates")), candidatesBeforeLate);
        bool nextDone = false;
        rats::Result next;
        app.api()->call("search.remote", { { "query", "controlled" },
            { "searchId", "b16af68e-a78d-4989-a288-2b552c0826a8" } },
            [&](const rats::Result& result) { next = result; nextDone = true; });
        messages->send("searchTorrent_response", librats::Json {
            { "hash", std::string(40, 'e') }, { "searchId", cancelledId.toStdString() } });
        messages->send("searchTorrent_response", librats::Json {
            { "hash", std::string(40, 'f') }, { "name", "controlled late legacy result" } });
        QTRY_VERIFY_WITH_TIMEOUT(nextDone, 7500);
        QVERIFY(next.ok());
        QCOMPARE(next.data().toObject()["items"].toArray().size(), 1);
        QCOMPARE(next.data().toObject()["items"].toArray().first().toObject()["hash"].toString(),
            QString(40, QLatin1Char('b')));
        QCOMPARE(next.data().toObject()["candidates"].toArray().size(), 1);
        QCOMPARE(next.data().toObject()["candidates"].toArray().first().toObject()["hash"].toString(),
            QString(40, QLatin1Char('f'))); // never promoted into confirmed items

        bool floodDone = false;
        rats::Result flood;
        app.api()->call("search.remote", { { "query", "flood" },
            { "searchId", "3b9df9f6-8ee2-4b04-ad8d-8774599d479d" } },
            [&](const rats::Result& result) { flood = result; floodDone = true; });
        QTRY_VERIFY_WITH_TIMEOUT(floodDone, 7500);
        QVERIFY(flood.ok());
        QCOMPARE(flood.data().toObject()["items"].toArray().size(), 0);
        QCOMPARE(flood.data().toObject()["candidates"].toArray().size(), 100);
        app.config()->setRemoteSearchEnabled(false);

        rats::domain::Torrent shared;
        shared.hash = QString(40, QLatin1Char('d'));
        shared.name = QStringLiteral("controlled share fixture");
        shared.size = 2345;
        shared.files = 1;
        shared.added = QDateTime::currentDateTimeUtc();
        QVERIFY(app.torrents()->add(shared));
        app.peers()->updateOurStats(37, 40, 1000);
        const librats::Json incomingSearch {
            { "text", "controlled share" },
            { "searchId", "5a72c36d-8045-4de6-84d7-c28b24633c22" }
        };
        messages->send("searchTorrent", incomingSearch);
        QTest::qWait(200);
        QCOMPARE(servedSearchReplies.load(), 0); // no outward index permission

        // A new connection must receive our Feed request even though we have
        // not authorized any outward index sharing.
        app.config()->setP2pReplication(true);
        QVERIFY(!app.config()->indexExchangeEnabled());
        QVERIFY(app.featureStatus()["replicationRunning"].toBool());
        librats::NodeConfig secondConfig;
        secondConfig.bind_address = "127.0.0.1";
        secondConfig.enable_listen = false;
        secondConfig.protocol = "rats-search/4";
        secondConfig.security = librats::NodeConfig::Security::Noise;
        secondConfig.data_dir = (data.path() + "/peer2").toStdString();
        librats::Node second(std::move(secondConfig));
        auto* secondMessages = second.add_subsystem(std::make_unique<librats::MessageJson>());
        std::atomic<int> activeFeedRequests { 0 };
        std::atomic<int> activeRandomRequests { 0 };
        secondMessages->on("feed", [&activeFeedRequests](const librats::PeerId&, const librats::Json&) { ++activeFeedRequests; });
        secondMessages->on("randomTorrents", [&activeRandomRequests](const librats::PeerId&, const librats::Json&) { ++activeRandomRequests; });
        QVERIFY(second.start());
        second.connect("127.0.0.1", static_cast<uint16_t>(p2p));
        QTRY_COMPARE_WITH_TIMEOUT(second.peer_count(), size_t(1), 8000);
        QTRY_VERIFY_WITH_TIMEOUT(activeFeedRequests.load() > 0 && activeRandomRequests.load() > 0, 4000);

        const auto requestFeed = [&]() {
            messages->send("feed", librats::Json { { "localSize", 0 }, { "localFeedDate", 0 } });
        };
        requestFeed();
        QTest::qWait(500);
        QCOMPARE(replies.load(), 0);

        app.config()->setIndexExchangeEnabled(true);
        QVERIFY(app.featureStatus()["indexSharingRunning"].toBool());
        messages->send("searchTorrent", incomingSearch);
        QTRY_COMPARE_WITH_TIMEOUT(servedSearchReplies.load(), 1, 4000);
        QTRY_COMPARE_WITH_TIMEOUT(advertisedTorrents.load(), 37, 4000);
        requestFeed();
        QTest::qWait(300);
        QCOMPARE(replies.load(), 0);
        messages->send("randomTorrents", librats::Json { { "limit", 1 } });
        QTest::qWait(300);
        QCOMPARE(randomReplies.load(), 0);
        app.config()->setP2pReplicationServer(true);
        requestFeed();
        QTRY_COMPARE_WITH_TIMEOUT(replies.load(), 1, 4000);
        messages->send("randomTorrents", librats::Json { { "limit", 1 } });
        QTRY_COMPARE_WITH_TIMEOUT(randomReplies.load(), 1, 4000);

        app.config()->setIndexExchangeEnabled(false);
        QVERIFY(!app.featureStatus()["indexSharingRunning"].toBool());
        QVERIFY(app.featureStatus()["replicationRunning"].toBool());
        QTRY_COMPARE_WITH_TIMEOUT(advertisedTorrents.load(), 0, 4000);
        requestFeed();
        QTest::qWait(500);
        QCOMPARE(replies.load(), 1);
        QCOMPARE(client.peer_count(), size_t(1));
        QVERIFY(app.transport()->isDhtRunning());
        QVERIFY(app.transport()->isRunning());

        app.config()->setP2pReplication(false);
        QVERIFY(!app.featureStatus()["replicationRunning"].toBool());
        QVERIFY(!app.config()->indexExchangeEnabled());

        second.stop();
        client.stop();
        app.stop();
    }
};

QTEST_GUILESS_MAIN(TestIndexSharingLoopback)
#include "test_index_sharing_loopback.moc"
