#include <QCoreApplication>
#include <QJsonObject>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QUdpSocket>
#include <QtTest>

#include "app/application.h"
#include "app/config_store.h"
#include "net/p2p_transport.h"
#include "peer/peer_api.h"
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
        std::atomic<int> advertisedTorrents { -1 };
        messages->on("feed_response", [&replies](const librats::PeerId&, const librats::Json&) { ++replies; });
        messages->on("feed", [&feedRequests](const librats::PeerId&, const librats::Json&) { ++feedRequests; });
        messages->on("randomTorrents_response", [&randomReplies](const librats::PeerId&, const librats::Json&) { ++randomReplies; });
        messages->on("torrent", [&torrentRequests](const librats::PeerId&, const librats::Json&) { ++torrentRequests; });
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
