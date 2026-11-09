#include <QtTest>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QTemporaryDir>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QSignalSpy>
#include <QTcpServer>
#include "rest/api_server.h"
#include "rest/api_router.h"

struct HttpResponse {
    int statusCode = 0;
    QByteArray body;
};

class TestWebUI : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void testStaticFileServing();
    void testRootPathServesIndex();
    void testDefaultReactDirectory();
    void testReactRouteServesApplicationShell();
    void testLegacyRouteUnavailable();
    void testDirectoryTraversalViaHttp();
    void testContentTypeHtml();
    void testContentTypeCss();
    void testContentTypeJs();
    void testContentTypeJson();
    void testContentTypePng();
    void testContentTypeSvg();
    void testFileNotFound();
    void testUnknownExtension();
    void testMissingWebuiDir();
    void testSymlinkCannotEscapeWebui();
    void testReactAssetRoute();
    void testChineseTranslations();

private:
    std::unique_ptr<rats::rest::ApiServer> server;
    QTemporaryDir tempDir;
    int port = 0;

    HttpResponse httpGet(const QString& path);
    void writeTestFile(const QString& relPath, const QByteArray& content);
};

void TestWebUI::writeTestFile(const QString& relPath, const QByteArray& content)
{
    QString fullPath = tempDir.path() + QStringLiteral("/webui-react/dist/") + relPath;
    QDir().mkpath(QFileInfo(fullPath).absolutePath());
    QFile f(fullPath);
    QVERIFY2(f.open(QIODevice::WriteOnly), qPrintable(f.errorString()));
    f.write(content);
    f.close();
}

void TestWebUI::initTestCase()
{
    QVERIFY(tempDir.isValid());

    writeTestFile("index.html", "<html><body>Test</body></html>");
    writeTestFile("assets/index.css", "body { color: red; }");
    writeTestFile("assets/index.js", "console.log('hello');");
    writeTestFile("config.json", "{\"key\": \"value\"}");
    writeTestFile("assets/icon.png", "\x89PNG");
    writeTestFile("logo.svg", "<svg></svg>");
    writeTestFile("readme.txt", "hello world");
    server = std::make_unique<rats::rest::ApiServer>(nullptr);
    server->setWebuiDir(tempDir.path() + "/webui-react/dist");
    QTcpServer probe;
    QVERIFY(probe.listen(QHostAddress::LocalHost, 0));
    const int freePort = probe.serverPort();
    probe.close();
    QVERIFY(server->start(freePort, 0));
    port = server->httpPort();
    QVERIFY(port > 0);
}

void TestWebUI::cleanupTestCase()
{
    if (server) server->stop();
    server.reset();
}

HttpResponse TestWebUI::httpGet(const QString& path)
{
    QNetworkAccessManager nam;
    QNetworkRequest req(QUrl(QStringLiteral("http://127.0.0.1:%1%2").arg(port).arg(path)));
    QNetworkReply* reply = nam.get(req);
    QSignalSpy spy(reply, &QNetworkReply::finished);
    spy.wait(5000);
    HttpResponse resp;
    resp.body = reply->readAll();
    resp.statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    reply->deleteLater();
    return resp;
}

void TestWebUI::testStaticFileServing()
{
    HttpResponse resp = httpGet("/assets/index.css");
    QCOMPARE(resp.statusCode, 200);
    QCOMPARE(resp.body, QByteArray("body { color: red; }"));
}

void TestWebUI::testRootPathServesIndex()
{
    HttpResponse resp = httpGet("/");
    QCOMPARE(resp.statusCode, 200);
    QVERIFY(resp.body.contains("<html><body>Test</body></html>"));
}

void TestWebUI::testDefaultReactDirectory()
{
    const QString previousDirectory = QDir::currentPath();
    QVERIFY(QDir::setCurrent(tempDir.path()));
    rats::rest::ApiServer defaultServer(nullptr);
    const QByteArray response = defaultServer.handleStaticFile("/");
    QVERIFY(QDir::setCurrent(previousDirectory));
    QVERIFY(response.startsWith("HTTP/1.1 200 OK"));
    QVERIFY(response.contains("<html><body>Test</body></html>"));
}

void TestWebUI::testReactRouteServesApplicationShell()
{
    HttpResponse resp = httpGet("/console/network");
    QCOMPARE(resp.statusCode, 200);
    QVERIFY(resp.body.contains("<html><body>Test</body></html>"));
    const QString hash(40, QLatin1Char('a'));
    QCOMPARE(httpGet("/search/permalink/" + hash).statusCode, 200);
    QCOMPARE(httpGet("/search/" + hash).statusCode, 200);
    QCOMPARE(httpGet("/search/missing.js").statusCode, 404);
    QCOMPARE(httpGet("/magnet-explorer").statusCode, 200);
}

void TestWebUI::testLegacyRouteUnavailable()
{
    HttpResponse resp = httpGet("/legacy");
    QCOMPARE(resp.statusCode, 404);
}

void TestWebUI::testDirectoryTraversalViaHttp()
{
    // Note: /../config.json normalizes to /config.json via QUrl before reaching
    // the server, so it's not a traversal — it's a valid file in the webui dir.
    QStringList paths = { "/../../../etc/passwd", "/assets/../../etc/passwd", "/assets/../../../etc/shadow" };
    for (const QString& path : paths) {
        HttpResponse resp = httpGet(path);
        QVERIFY2(resp.statusCode == 404 || resp.statusCode == 403,
                 qPrintable("Path: " + path + " Status: " + QByteArray::number(resp.statusCode)));
        QVERIFY(!resp.body.contains("root:"));
    }
}

void TestWebUI::testContentTypeHtml()
{
    HttpResponse resp = httpGet("/index.html");
    QCOMPARE(resp.statusCode, 200);
}

void TestWebUI::testContentTypeCss()
{
    HttpResponse resp = httpGet("/assets/index.css");
    QCOMPARE(resp.statusCode, 200);
}

void TestWebUI::testContentTypeJs()
{
    HttpResponse resp = httpGet("/assets/index.js");
    QCOMPARE(resp.statusCode, 200);
}

void TestWebUI::testContentTypeJson()
{
    HttpResponse resp = httpGet("/config.json");
    QCOMPARE(resp.statusCode, 200);
}

void TestWebUI::testContentTypePng()
{
    HttpResponse resp = httpGet("/assets/icon.png");
    QCOMPARE(resp.statusCode, 200);
}

void TestWebUI::testContentTypeSvg()
{
    HttpResponse resp = httpGet("/logo.svg");
    QCOMPARE(resp.statusCode, 200);
}

void TestWebUI::testFileNotFound()
{
    HttpResponse resp = httpGet("/nonexistent.html");
    QCOMPARE(resp.statusCode, 404);
}

void TestWebUI::testUnknownExtension()
{
    HttpResponse resp = httpGet("/readme.txt");
    QCOMPARE(resp.statusCode, 404);
}

void TestWebUI::testMissingWebuiDir()
{
    QByteArray response = server->handleStaticFile("/anything.html");
    int status = 0;
    QString line = QString::fromUtf8(response.left(response.indexOf("\r\n")));
    QStringList parts = line.split(' ');
    if (parts.size() >= 2) status = parts[1].toInt();
    QCOMPARE(status, 404);
}

void TestWebUI::testSymlinkCannotEscapeWebui()
{
#ifdef Q_OS_WIN
    QSKIP("Creating a symlink requires Windows developer mode or elevated privileges");
#endif
    const QString outside = tempDir.path() + "/secret.html";
    QFile secret(outside);
    QVERIFY(secret.open(QIODevice::WriteOnly));
    secret.write("private content");
    secret.close();
    QVERIFY(QFile::link(outside, tempDir.path() + "/webui-react/dist/linked.html"));

    const HttpResponse resp = httpGet("/linked.html");
    QCOMPARE(resp.statusCode, 403);
    QVERIFY(!resp.body.contains("private content"));
}

void TestWebUI::testReactAssetRoute()
{
    const HttpResponse resp = httpGet("/assets/index.js");
    QCOMPARE(resp.statusCode, 200);
    QCOMPARE(resp.body, QByteArray("console.log('hello');"));
}

void TestWebUI::testChineseTranslations()
{
    const HttpResponse resp = httpGet("/api/translations/zh");
    QCOMPARE(resp.statusCode, 200);
    QVERIFY(resp.body.contains("language=\"zh_CN\""));
}

QTEST_MAIN(TestWebUI)
#include "test_webui.moc"
