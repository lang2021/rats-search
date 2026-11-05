#include <QFile>
#include <QHostAddress>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QUdpSocket>
#include <QtTest>

#include "app/application.h"
#include "app/config_store.h"
#include "data/torrent_repository.h"
#include "net/p2p_transport.h"
#include "services/download_service.h"

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

class TestNetworkLifecycleLoopback : public QObject {
    Q_OBJECT

private:
    struct Reply { int status = 0; QJsonObject body; };

    static Reply post(int port, const QByteArray& token, const QString& method, bool authorized = true, bool get = false)
    {
        QNetworkAccessManager manager;
        QNetworkRequest request(QUrl(QStringLiteral("http://127.0.0.1:%1/api/%2").arg(port).arg(method)));
        if (authorized) request.setRawHeader("Authorization", "Bearer " + token);
        request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
        QNetworkReply* response = get ? manager.get(request) : manager.post(request, "{}");
        QSignalSpy finished(response, &QNetworkReply::finished);
        if (!response->isFinished() && !finished.wait(5000)) {
            response->abort();
            response->deleteLater();
            return {};
        }
        Reply result;
        result.status = response->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        result.body = QJsonDocument::fromJson(response->readAll()).object();
        response->deleteLater();
        return result;
    }

    static int freeTcpPort()
    {
        QTcpServer probe;
        if (!probe.listen(QHostAddress::LocalHost, 0)) return 0;
        return probe.serverPort();
    }

private slots:
    void twoCyclesFailureRetryAndPausedTasks()
    {
        if (!qEnvironmentVariableIsSet("RATS_LOOPBACK_SANDBOX"))
            QSKIP("Run only under an outbound-denying loopback sandbox");
        QTemporaryDir data;
        QVERIFY(data.isValid());
        const int p2p = freeTcpPort();
        const int dht = freeTcpPort();
        int http = 0;
        for (int i = 0; i < 20; ++i) {
            const int candidate = freeTcpPort();
            QTcpServer next;
            if (candidate > 0 && candidate < 65535 && next.listen(QHostAddress::LocalHost, candidate + 1)) {
                http = candidate;
                break;
            }
        }
        QVERIFY(p2p > 0 && dht > 0 && http > 0);

        rats::app::ConfigStore config(data.path() + "/rats.json");
        config.setNetworkEnabled(false);
        config.setIndexerEnabled(false);
        config.setTrackersEnabled(false);
        config.setRestApiEnabled(true);
        config.setHttpPort(http);
        config.setP2pPort(p2p);
        QVERIFY(config.save());

        rats::app::Application::Options options;
        options.dataDirectory = data.path();
        options.clientVersion = QStringLiteral("test");
        options.headless = true;
        options.dhtPort = dht;
        rats::app::Application app(options);
        QVERIFY(app.start());
        int probeMessages = 0;
        app.transport()->registerHandler("lifecycle_probe", [&probeMessages](const QString&, const QJsonObject&) {
            ++probeMessages;
        });
        QCOMPARE(app.featureStatus()["networkState"].toString(), QStringLiteral("stopped"));
        QVERIFY(app.torrents()->statistics().torrents == 0);

        QFile tokenFile(data.path() + "/api-token");
        QVERIFY(tokenFile.open(QIODevice::ReadOnly));
        const QByteArray token = tokenFile.readAll().trimmed();
        tokenFile.close();
        QCOMPARE(token.size(), 64);
        QCOMPARE(post(http, token, "network.start", false).status, 401);
        QCOMPARE(post(http, token, "network.stop", false).status, 401);
        QCOMPARE(post(http, token, "network.start", true, true).status, 405);

        for (int cycle = 0; cycle < 2; ++cycle) {
            const Reply started = post(http, token, "network.start");
            QVERIFY2(started.body["success"].toBool(), "network.start failed");
            QCOMPARE(started.body["data"].toObject()["networkState"].toString(), QStringLiteral("running"));
            QVERIFY(!started.body["data"].toObject()["networkDesired"].toBool());
            QVERIFY(app.transport()->isRunning());
            QVERIFY(app.transport()->isDhtRunning());
            QVERIFY(!app.featureStatus()["crawlerRunning"].toBool());
            QCOMPARE(post(http, token, "search.torrents").status, 200);

            if (cycle == 0) {
                // These fixture hashes have no peers or content. A stopped task
                // must remain visible and paused across the fresh Node.
                app.config()->fromJson({ { "btFileDownload", true } });
                const QString activeHash(40, QLatin1Char('a'));
                const QString manualHash(40, QLatin1Char('b'));
                QVERIFY(app.downloads()->add(activeHash, data.path() + "/files"));
                QVERIFY(app.downloads()->add(manualHash, data.path() + "/files"));
                QVERIFY(app.downloads()->pause(manualHash));
            }

            if (cycle == 0) {
                librats::NodeConfig peerConfig;
                peerConfig.bind_address = "127.0.0.1";
                peerConfig.enable_listen = false;
                peerConfig.protocol = "rats-search/4";
                peerConfig.security = librats::NodeConfig::Security::Noise;
                peerConfig.data_dir = (data.path() + "/peer").toStdString();
                librats::Node peer(std::move(peerConfig));
                auto* messages = peer.add_subsystem(std::make_unique<librats::MessageJson>());
                QVERIFY(peer.start());
                peer.connect("127.0.0.1", static_cast<uint16_t>(p2p));
                QTRY_VERIFY_WITH_TIMEOUT(peer.peer_count() == 1 && app.transport()->peerCount() == 1, 5000);
                messages->send("lifecycle_probe", librats::Json { { "source", "baseline" } });
                QTRY_COMPARE_WITH_TIMEOUT(probeMessages, 1, 3000);
                probeMessages = 0;
                for (int i = 0; i < 20; ++i)
                    messages->send("lifecycle_probe", librats::Json { { "source", "old" } });
                // Let the reactor queue callbacks while the Qt thread is held.
                QThread::msleep(50);
                QVERIFY(app.stopNetwork());
                QVERIFY(app.startNetwork());
                QCoreApplication::processEvents();
                QCOMPARE(probeMessages, 0);
                QTRY_VERIFY_WITH_TIMEOUT(peer.peer_count() == 0, 3000);
                peer.connect("127.0.0.1", static_cast<uint16_t>(p2p));
                QTRY_VERIFY_WITH_TIMEOUT(peer.peer_count() == 1 && app.transport()->peerCount() == 1, 5000);
                messages->send("lifecycle_probe", librats::Json { { "source", "new" } });
                QTRY_COMPARE_WITH_TIMEOUT(probeMessages, 1, 3000);
                QVERIFY(app.stopNetwork());
                peer.stop();
            } else {
                const Reply stopped = post(http, token, "network.stop");
                QVERIFY(stopped.body["success"].toBool());
                QCOMPARE(stopped.body["data"].toObject()["networkState"].toString(), QStringLiteral("stopped"));
            }
            QVERIFY(!app.transport()->isRunning());
            QVERIFY(!app.engine()->fileDownloadAllowed());
            QVERIFY(!app.engine()->fileUploadAllowed());
            QCOMPARE(app.downloads()->allDownloads().size(), 2);
            for (const auto& entry : app.downloads()->allDownloads()) QVERIFY(entry.paused);
            QTcpServer released;
            QVERIFY(released.listen(QHostAddress::LocalHost, p2p));
            released.close();
            QUdpSocket dhtReleased;
            QVERIFY(dhtReleased.bind(QHostAddress::LocalHost, dht));
            dhtReleased.close();
            QCOMPARE(post(http, token, "search.torrents").status, 200);
        }

        // Invalid ports must fail before any Node or socket is created; a later
        // attempt with valid settings must still be able to start.
        app.config()->setP2pPort(70000);
        const Reply failed = post(http, token, "network.start");
        QVERIFY(!failed.body["success"].toBool());
        QCOMPARE(app.featureStatus()["networkState"].toString(), QStringLiteral("failed"));
        QVERIFY(!app.featureStatus()["lastError"].toString().isEmpty());
        QVERIFY(!app.transport()->isRunning());
        app.config()->setP2pPort(p2p);
        const Reply retried = post(http, token, "network.start");
        QVERIFY(retried.body["success"].toBool());
        QVERIFY(app.featureStatus()["lastError"].toString().isEmpty());
        for (const auto& entry : app.downloads()->allDownloads()) QVERIFY(entry.paused);
        QVERIFY(post(http, token, "network.stop").body["success"].toBool());
        app.stop();
    }
};

QTEST_GUILESS_MAIN(TestNetworkLifecycleLoopback)
#include "test_network_lifecycle_loopback.moc"
