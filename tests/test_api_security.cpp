#include <QtTest>
#include <QDir>
#include <QFile>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QProcess>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QUrlQuery>
#include <QWebSocket>
#include "app/application.h"
#include "app/config_store.h"
#include "net/torrent_engine.h"
#include "rest/api_router.h"
#include "rest/api_server.h"

class TestApiSecurity : public QObject {
    Q_OBJECT
private slots:
    void initTestCase();
    void cleanupTestCase();
    void testHttpRequiresToken();
    void testTokenFileIsPrivateAndPersistent();
    void testAuthenticatedManagementRead();
    void testConfigSetReportsPersistenceFailure();
    void testFilePathsRequireExplicitRoot();
    void testNoFileRootByDefault();
    void testAuthorizedTorrentFileCreationWithoutP2p();
    void testBtTransferDeniedWithoutPermission();
    void testRemoteSearchRequiresConsent();
    void testTraversalAndSymlinks();
    void testWebSocketRequiresTokenAndChecksPaths();
    void testEventsOnlyReachAuthenticatedWebSockets();

private:
    struct Response { int status = 0; QByteArray body; };
    Response request(const QString& method, const QJsonObject& params = {}, bool authorized = true, bool get = false);
    std::unique_ptr<rats::rest::ApiServer> server_;
    QTemporaryDir temp_;
    QString root_;
    int port_ = 0;
};

void TestApiSecurity::initTestCase()
{
    QVERIFY(temp_.isValid());
    root_ = temp_.path() + "/allowed";
    QVERIFY(QDir().mkpath(root_));
    QVERIFY(QDir().mkpath(temp_.path() + "/allowed-sibling"));
    QFile allowed(root_ + "/source.txt");
    QVERIFY(allowed.open(QIODevice::WriteOnly));
    allowed.write("safe");
    allowed.close();
    QFile outside(temp_.path() + "/secret.txt");
    QVERIFY(outside.open(QIODevice::WriteOnly));
    outside.write("private");
    outside.close();

    server_ = std::make_unique<rats::rest::ApiServer>(nullptr);
    server_->setAuthorizedFileRoots({root_});
    QTcpServer probe;
    QVERIFY(probe.listen(QHostAddress::LocalHost, 0));
    const int freePort = probe.serverPort();
    probe.close();
    QVERIFY(server_->start(freePort, freePort + 1));
    QCOMPARE(server_->authenticationToken().size(), 64);
    port_ = server_->httpPort();
}

void TestApiSecurity::cleanupTestCase()
{
    server_->stop();
}

TestApiSecurity::Response TestApiSecurity::request(const QString& method, const QJsonObject& params, bool authorized, bool get)
{
    QNetworkAccessManager manager;
    QUrl url(QStringLiteral("http://127.0.0.1:%1/api/%2").arg(port_).arg(method));
    if (get) {
        QUrlQuery query;
        for (auto it = params.begin(); it != params.end(); ++it)
            query.addQueryItem(it.key(), it.value().toString());
        url.setQuery(query);
    }
    QNetworkRequest req(url);
    if (authorized) req.setRawHeader("Authorization", "Bearer " + server_->authenticationToken());
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    QNetworkReply* reply = get ? manager.get(req) : manager.post(req, QJsonDocument(params).toJson());
    QSignalSpy done(reply, &QNetworkReply::finished);
    if (!reply->isFinished() && !done.wait(5000)) {
        reply->abort();
        reply->deleteLater();
        return {};
    }
    Response result;
    result.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    result.body = reply->readAll();
    reply->deleteLater();
    return result;
}

void TestApiSecurity::testHttpRequiresToken()
{
    QCOMPARE(request("stats.get", {}, false).status, 401);
    QCOMPARE(request("stats.get").status, 400); // authenticated; test server has no router
    QCOMPARE(request("torrent.create", {{"path", root_ + "/source.txt"}, {"seed", true}}, false).status, 401);
    QCOMPARE(request("search.remote", {{"query", "private term"}}, false).status, 401);
    QCOMPARE(request("search.remote", {}, true, true).status, 405);

    QNetworkAccessManager manager;
    QNetworkReply* reply = manager.get(QNetworkRequest(QUrl(QStringLiteral("http://127.0.0.1:%1/metrics").arg(port_))));
    QSignalSpy done(reply, &QNetworkReply::finished);
    if (!reply->isFinished()) QVERIFY(done.wait(5000));
    QCOMPARE(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 401);
    reply->deleteLater();
}

void TestApiSecurity::testTokenFileIsPrivateAndPersistent()
{
    QTemporaryDir data;
    QVERIFY(data.isValid());
    rats::app::Application::Options options;
    options.dataDirectory = data.path();
    options.headless = true;
    rats::app::Application app(options);

    rats::rest::ApiServer first(nullptr);
    first.setApplication(&app);
    QVERIFY(first.start(0, 0));
    const QByteArray token = first.authenticationToken();
    QCOMPARE(token.size(), 64);
    first.stop();

    const QFileInfo info(data.path() + "/api-token");
    QVERIFY(info.isFile());
#ifndef Q_OS_WIN
    const auto unsafe = QFileDevice::ReadGroup | QFileDevice::WriteGroup | QFileDevice::ReadOther |
        QFileDevice::WriteOther;
    QVERIFY(!(info.permissions() & unsafe));
#endif
    rats::rest::ApiServer second(nullptr);
    second.setApplication(&app);
    second.setAuthorizedFileRoots({data.path()});
    QTcpServer probe;
    QVERIFY(probe.listen(QHostAddress::LocalHost, 0));
    const int freePort = probe.serverPort();
    probe.close();
    QVERIFY(second.start(freePort, 0));
    QCOMPARE(second.authenticationToken(), token);

    QNetworkAccessManager manager;
    QNetworkRequest req(QUrl(QStringLiteral("http://127.0.0.1:%1/api/torrent.create").arg(second.httpPort())));
    req.setRawHeader("Authorization", "Bearer " + token);
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    QNetworkReply* reply = manager.post(req, QJsonDocument(QJsonObject {
        {"path", data.path() + "/api-token"}, {"seed", true}
    }).toJson());
    QSignalSpy done(reply, &QNetworkReply::finished);
    if (!reply->isFinished()) QVERIFY(done.wait(5000));
    QCOMPARE(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 403);
    reply->deleteLater();
    second.stop();
}

void TestApiSecurity::testAuthenticatedManagementRead()
{
    QTemporaryDir data;
    QVERIFY(data.isValid());
    rats::app::Application::Options options;
    options.dataDirectory = data.path();
    options.headless = true;
    rats::app::Application app(options);
    rats::rest::ApiRouter router(&app);
    rats::rest::ApiServer api(&router);
    api.setApplication(&app);
    QTcpServer probe;
    QVERIFY(probe.listen(QHostAddress::LocalHost, 0));
    const int freePort = probe.serverPort();
    probe.close();
    QVERIFY(api.start(freePort, 0));

    for (const QString& method : {QStringLiteral("config.get"), QStringLiteral("download.list"),
             QStringLiteral("stats.get"), QStringLiteral("search.history")}) {
        QNetworkAccessManager manager;
        QNetworkRequest req(QUrl(QStringLiteral("http://127.0.0.1:%1/api/%2").arg(api.httpPort()).arg(method)));
        req.setRawHeader("Authorization", "Bearer " + api.authenticationToken());
        QNetworkReply* reply = manager.get(req);
        QSignalSpy done(reply, &QNetworkReply::finished);
        if (!reply->isFinished()) QVERIFY(done.wait(5000));
        QCOMPARE(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 200);
        QVERIFY(QJsonDocument::fromJson(reply->readAll()).object()["success"].toBool());
        reply->deleteLater();
    }
    api.stop();
}

void TestApiSecurity::testConfigSetReportsPersistenceFailure()
{
    QTemporaryDir data;
    QVERIFY(data.isValid());
    rats::app::Application::Options options;
    options.dataDirectory = data.path();
    options.headless = true;
    rats::app::Application app(options);
    rats::rest::ApiRouter router(&app);

    const QString configPath = data.path() + "/rats.json";
    QVERIFY(QDir().mkdir(configPath));
    bool called = false;
    rats::Result unchanged;
    router.call("config.set", {}, [&](const rats::Result& result) {
        called = true;
        unchanged = result;
    });
    QVERIFY(called);
    QVERIFY(!unchanged.ok());

    QVERIFY(QDir().rmdir(configPath));
    called = false;
    rats::Result baseline;
    router.call("config.set", {}, [&](const rats::Result& result) {
        called = true;
        baseline = result;
    });
    QVERIFY(called);
    QVERIFY(baseline.ok());
#ifdef Q_OS_MACOS
    QCOMPARE(QProcess::execute("chmod", { "+a", "everyone deny delete", configPath }), 0);
#else
    QVERIFY(QFile::remove(configPath));
    QVERIFY(QDir().mkdir(configPath));
#endif
    called = false;
    rats::Result first;
    router.call("config.set", { { "safeSearch", true } }, [&](const rats::Result& result) {
        called = true;
        first = result;
    });
#ifdef Q_OS_MACOS
    const int resetResult = QProcess::execute("chmod", { "-a", "everyone deny delete", configPath });
    QCOMPARE(resetResult, 0);
#else
    QVERIFY(QDir().rmdir(configPath));
#endif
    QVERIFY(called);
    QVERIFY(!first.ok());
    QVERIFY(first.error().contains("applied in memory"));
    QVERIFY(app.config()->safeSearch());
    QVERIFY(app.config()->hasUnsavedChanges());

    rats::app::ConfigStore diskBeforeRetry(configPath);
    QVERIFY(diskBeforeRetry.load());
    QVERIFY(!diskBeforeRetry.safeSearch());
    called = false;
    rats::Result retry;
    // Repeating the same value must retry the pending disk write.
    router.call("config.set", { { "safeSearch", true } }, [&](const rats::Result& result) {
        called = true;
        retry = result;
    });
    QVERIFY(called);
    QVERIFY(retry.ok());
    QVERIFY(!app.config()->hasUnsavedChanges());
    rats::app::ConfigStore restarted(configPath);
    QVERIFY(restarted.load());
    QVERIFY(restarted.safeSearch());
}

void TestApiSecurity::testNoFileRootByDefault()
{
    rats::rest::ApiServer api(nullptr);
    QTcpServer probe;
    QVERIFY(probe.listen(QHostAddress::LocalHost, 0));
    const int freePort = probe.serverPort();
    probe.close();
    QVERIFY(api.start(freePort, 0));

    QNetworkAccessManager manager;
    QNetworkRequest req(QUrl(QStringLiteral("http://127.0.0.1:%1/api/torrent.create").arg(api.httpPort())));
    req.setRawHeader("Authorization", "Bearer " + api.authenticationToken());
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    QNetworkReply* reply = manager.post(req, QJsonDocument(QJsonObject {
        {"path", root_ + "/source.txt"}, {"seed", true}
    }).toJson());
    QSignalSpy done(reply, &QNetworkReply::finished);
    if (!reply->isFinished()) QVERIFY(done.wait(5000));
    QCOMPARE(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 403);
    reply->deleteLater();
    api.stop();
}

void TestApiSecurity::testAuthorizedTorrentFileCreationWithoutP2p()
{
    QTemporaryDir data;
    QVERIFY(data.isValid());
    const QString allowed = data.path() + "/allowed";
    QVERIFY(QDir().mkpath(allowed));
    const QString source = allowed + "/source.txt";
    const QString output = allowed + "/source.torrent";
    QFile content(source);
    QVERIFY(content.open(QIODevice::WriteOnly));
    QCOMPARE(content.write("local-only test content"), 23);
    content.close();

    rats::app::Application::Options options;
    options.dataDirectory = data.path();
    options.headless = true;
    rats::app::Application app(options); // Do not start P2P or Manticore.
    rats::rest::ApiRouter router(&app);
    rats::rest::ApiServer api(&router);
    api.setApplication(&app);
    api.setAuthorizedFileRoots({allowed});
    QTcpServer probe;
    QVERIFY(probe.listen(QHostAddress::LocalHost, 0));
    const int freePort = probe.serverPort();
    probe.close();
    QVERIFY(api.start(freePort, 0));

    QNetworkAccessManager manager;
    QNetworkRequest req(QUrl(QStringLiteral("http://127.0.0.1:%1/api/torrent.create").arg(api.httpPort())));
    req.setRawHeader("Authorization", "Bearer " + api.authenticationToken());
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    QNetworkReply* reply = manager.post(req, QJsonDocument(QJsonObject {
        {"path", source}, {"output", output}, {"seed", false}
    }).toJson());
    QSignalSpy done(reply, &QNetworkReply::finished);
    if (!reply->isFinished()) QVERIFY(done.wait(5000));
    QCOMPARE(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 200);
    QVERIFY(QJsonDocument::fromJson(reply->readAll()).object()["success"].toBool());
    reply->deleteLater();
    QFile torrent(output);
    QVERIFY(torrent.open(QIODevice::ReadOnly));
    QVERIFY(torrent.size() > 0);
    QCOMPARE(torrent.read(1), QByteArray("d"));
    api.stop();
}

void TestApiSecurity::testBtTransferDeniedWithoutPermission()
{
    QTemporaryDir data;
    QVERIFY(data.isValid());
    rats::app::Application::Options options;
    options.dataDirectory = data.path();
    options.headless = true;
    rats::app::Application app(options); // No P2P sockets are started.
    rats::rest::ApiRouter router(&app);

    const QString hash(40, QLatin1Char('a'));
    const auto denied = [&router](const QString& method, const QJsonObject& params) {
        bool called = false;
        rats::Result result;
        router.call(method, params, [&](const rats::Result& value) {
            called = true;
            result = value;
        });
        return called && !result.ok() && result.error().contains("disabled");
    };
    app.engine()->setFileTransferPermissions(false, true);
    QVERIFY(denied("download.add", { { "hash", hash } }));
    QVERIFY(denied("download.addFile", { { "path", data.path() + "/source.torrent" } }));
    app.engine()->setFileTransferPermissions(true, false);
    QVERIFY(denied("torrent.create", { { "path", data.path() + "/source.txt" }, { "seed", true } }));
}

void TestApiSecurity::testRemoteSearchRequiresConsent()
{
    QTemporaryDir data;
    QVERIFY(data.isValid());
    rats::app::Application::Options options;
    options.dataDirectory = data.path();
    options.headless = true;
    rats::app::Application app(options);
    rats::rest::ApiRouter router(&app);
    bool called = false;
    rats::Result result;
    router.call("search.remote", {
        { "query", "private term" },
        { "searchId", "b430baf0-0fb7-4c06-b44d-33e84dca7ae3" }
    }, [&](const rats::Result& value) { result = value; called = true; });
    QVERIFY(called);
    QVERIFY(!result.ok());
    QVERIFY(result.error().contains("permission disabled"));
    app.config()->setRemoteSearchEnabled(true);
    called = false;
    router.call("search.remote", {
        { "query", "private term" },
        { "searchId", "b430baf0-0fb7-4c06-b44d-33e84dca7ae3" }
    }, [&](const rats::Result& value) { result = value; called = true; });
    QVERIFY(called);
    QVERIFY(!result.ok());
    QVERIFY(result.error().contains("network unavailable"));
}

void TestApiSecurity::testFilePathsRequireExplicitRoot()
{
    const QString safe = root_ + "/source.txt";
    const QString unsafe = temp_.path() + "/secret.txt";
    QCOMPARE(request("torrent.create", {{"path", unsafe}, {"seed", true}}).status, 403);
    QCOMPARE(request("torrent.create", {{"path", safe}, {"seed", true}}).status, 400);
    QCOMPARE(request("torrent.create", {{"path", safe}, {"output", unsafe}}).status, 403);
    QCOMPARE(request("torrent.import", {{"file", unsafe}}).status, 403);
    QCOMPARE(request("database.import", {{"path", unsafe}, {"removeWhenDone", true}}).status, 403);
    QCOMPARE(request("database.export", {{"file", unsafe}}).status, 403);
    QCOMPARE(request("download.addFile", {{"path", unsafe}}).status, 403);
    QCOMPARE(request("download.add", {{"savePath", unsafe}}).status, 403);
    QCOMPARE(request("config.set", {{"downloadPath", unsafe}}).status, 403);
    QCOMPARE(request("download.add", {}).status, 400); // default download path remains usable
    QCOMPARE(request("torrent.create", {{"path", unsafe}}, true, true).status, 403);
}

void TestApiSecurity::testTraversalAndSymlinks()
{
    const QString unsafe = temp_.path() + "/secret.txt";
    QCOMPARE(request("torrent.create", {{"path", root_ + "/../secret.txt"}}).status, 403);
    QCOMPARE(request("torrent.create", {{"path", temp_.path() + "/allowed-sibling/source.txt"}}).status, 403);
#ifndef Q_OS_WIN
    QVERIFY(QFile::link(unsafe, root_ + "/link.txt"));
    QCOMPARE(request("torrent.create", {{"path", root_ + "/link.txt"}, {"seed", true}}).status, 403);
    QCOMPARE(request("database.export", {{"path", root_ + "/link.txt"}}).status, 403);
    QVERIFY(QFile::link(temp_.path(), root_ + "/outside-dir"));
    QCOMPARE(request("database.export", {{"path", root_ + "/outside-dir/new.dump"}}).status, 403);
    // The lexical clean path is inside the root, but POSIX resolves the link
    // before '..' and would open a file outside it.
    QCOMPARE(request("database.export", {{"path", root_ + "/outside-dir/../escape.dump"}}).status, 403);
    QCOMPARE(request("torrent.create", {{"path", root_}, {"seed", true}}).status, 403);
    QVERIFY(QFile::link(temp_.path() + "/missing", root_ + "/dangling"));
    QCOMPARE(request("database.export", {{"path", root_ + "/dangling"}}).status, 403);
#endif
}

void TestApiSecurity::testWebSocketRequiresTokenAndChecksPaths()
{
    const QUrl url(QStringLiteral("ws://127.0.0.1:%1").arg(port_ + 1));
    QWebSocket denied;
    QSignalSpy deniedConnected(&denied, &QWebSocket::connected);
    QSignalSpy deniedClosed(&denied, &QWebSocket::disconnected);
    denied.open(url);
    QVERIFY(deniedConnected.wait(5000));
    denied.sendTextMessage("{\"method\":\"stats.get\",\"id\":\"1\"}");
    QVERIFY(deniedClosed.wait(5000));

    QWebSocket accepted;
    QSignalSpy acceptedConnected(&accepted, &QWebSocket::connected);
    QSignalSpy messages(&accepted, &QWebSocket::textMessageReceived);
    accepted.open(url);
    QVERIFY(acceptedConnected.wait(5000));
    accepted.sendTextMessage(QStringLiteral("{\"method\":\"auth\",\"params\":{\"token\":\"%1\"}}")
        .arg(QString::fromLatin1(server_->authenticationToken())));
    QVERIFY(messages.wait(5000));
    QVERIFY(messages.takeFirst().at(0).toString().contains("authenticated"));
    accepted.sendTextMessage(QString::fromUtf8(QJsonDocument(QJsonObject {
        {"method", "torrent.create"}, {"params", QJsonObject {{"path", temp_.path() + "/secret.txt"}, {"seed", true}}}, {"id", "2"}
    }).toJson(QJsonDocument::Compact)));
    QVERIFY(messages.wait(5000));
    QVERIFY(messages.takeFirst().at(0).toString().contains("outside authorized API roots"));
    accepted.close();
}

void TestApiSecurity::testEventsOnlyReachAuthenticatedWebSockets()
{
    QTemporaryDir data;
    QVERIFY(data.isValid());
    rats::app::Application::Options options;
    options.dataDirectory = data.path();
    options.headless = true;
    rats::app::Application app(options);
    rats::rest::ApiRouter router(&app);
    rats::rest::ApiServer api(&router);
    api.setApplication(&app);
    QTcpServer probe;
    QVERIFY(probe.listen(QHostAddress::LocalHost, 0));
    const int freePort = probe.serverPort();
    probe.close();
    QVERIFY(api.start(0, freePort));
    const QUrl url(QStringLiteral("ws://127.0.0.1:%1").arg(freePort));

    QWebSocket untrusted;
    QSignalSpy untrustedConnected(&untrusted, &QWebSocket::connected);
    QSignalSpy untrustedMessages(&untrusted, &QWebSocket::textMessageReceived);
    untrusted.open(url);
    QVERIFY(untrustedConnected.wait(5000));

    QWebSocket trusted;
    QSignalSpy trustedConnected(&trusted, &QWebSocket::connected);
    QSignalSpy trustedMessages(&trusted, &QWebSocket::textMessageReceived);
    trusted.open(url);
    QVERIFY(trustedConnected.wait(5000));
    trusted.sendTextMessage(QStringLiteral("{\"method\":\"auth\",\"params\":{\"token\":\"%1\"}}")
        .arg(QString::fromLatin1(api.authenticationToken())));
    QVERIFY(trustedMessages.wait(5000));
    trustedMessages.clear();

    app.config()->setSafeSearch(!app.config()->safeSearch());
    QTRY_VERIFY_WITH_TIMEOUT(trustedMessages.size() > 0, 5000);
    QVERIFY(trustedMessages.takeFirst().at(0).toString().contains("configChanged"));
    QCOMPARE(untrustedMessages.size(), 0);
    untrusted.close();
    trusted.close();
    api.stop();
}

QTEST_MAIN(TestApiSecurity)
#include "test_api_security.moc"
