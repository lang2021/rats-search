#include "app/watch_store.h"
#include <QJsonArray>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QtTest>

using rats::app::WatchStore;

static rats::Result request(WatchStore& watch, const QString& method, const QJsonObject& params = {})
{
    rats::Result result;
    QEventLoop loop;
    watch.request(method, params, [&](const rats::Result& value) { result = value; loop.quit(); });
    loop.exec();
    return result;
}

class TestWatch : public QObject {
    Q_OBJECT
private slots:
    void filteredPagingAndBulkRead()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        WatchStore watch(dir.path());
        QVERIFY(request(watch, "watch.add", {{"keyword", "Ubuntu"}}).ok());
        QVERIFY(request(watch, "watch.add", {{"keyword", "Blender"}}).ok());
        const QString a(40, 'a'), b(40, 'b'), c(40, 'c');
        watch.observe({{a, "Ubuntu Desktop"}, {b, "Ubuntu Studio"}, {c, "Blender"}});

        auto page = request(watch, "watch.list", {{"ruleId", 1}}).data().toObject();
        QCOMPARE(page["matches"].toArray().size(), 2);
        QVERIFY(!page["more"].toBool());
        page = request(watch, "watch.list", {{"ruleId", 1}, {"unreadOnly", true}}).data().toObject();
        QCOMPARE(page["matches"].toArray().size(), 2);

        QVERIFY(request(watch, "watch.readAll", {{"ruleId", 1}}).ok());
        QCOMPARE(request(watch, "watch.unread").data().toInt(), 1);
        page = request(watch, "watch.list", {{"ruleId", 1}, {"unreadOnly", true}}).data().toObject();
        QVERIFY(page["matches"].toArray().isEmpty());

        QVERIFY(request(watch, "watch.readAll").ok());
        QCOMPARE(request(watch, "watch.unread").data().toInt(), 0);
        // Bulk marking an empty scope remains an idempotent success.
        QVERIFY(request(watch, "watch.readAll").ok());
    }

    void matchingAndRestart()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString a(40, 'a'), b(40, 'b'), c(40, 'c');
        {
            WatchStore watch(dir.path());
            watch.observe({{a, "Ubuntu old"}});
            QVERIFY(request(watch, "watch.add", {{"keyword", "  Ubuntu  "}}).ok());
            QVERIFY(!request(watch, "watch.add", {{"keyword", "uBuNtU"}}).ok());
            QVERIFY(!request(watch, "watch.add", {{"keyword", " "}}).ok());
            QVERIFY(request(watch, "watch.add", {{"keyword", "Desktop"}}).ok());
            QCOMPARE(request(watch, "watch.unread").data().toInt(), 0);
            watch.observe({{a, "Blender"}, {b, "UBUNTU Desktop"}});
            watch.observe({{b.toUpper(), "Ubuntu Desktop"}});
            QCOMPARE(request(watch, "watch.unread").data().toInt(), 2);
            QVERIFY(request(watch, "watch.enable", {{"id", 1}, {"enabled", false}}).ok());
            watch.observe({{c, "Ubuntu"}});
            QCOMPARE(request(watch, "watch.unread").data().toInt(), 2);
            QVERIFY(request(watch, "watch.read", {{"id", 1}}).ok());
        }
        WatchStore watch(dir.path());
        auto page = request(watch, "watch.list", {{"unreadOnly", true}}).data().toObject();
        QCOMPARE(page["matches"].toArray().size(), 1);
        QCOMPARE(page["rules"].toArray().size(), 2);
        QVERIFY(request(watch, "watch.enable", {{"id", 1}, {"enabled", true}}).ok());
        watch.observe({{c, "Ubuntu"}});
        QCOMPARE(request(watch, "watch.unread").data().toInt(), 2);
        QVERIFY(request(watch, "watch.delete", {{"id", 1}}).ok());
        page = request(watch, "watch.list").data().toObject();
        QCOMPARE(page["matches"].toArray().size(), 1);
        QCOMPARE(page["matches"].toArray().first().toObject()["keyword"].toString(), "Desktop");
    }

    void failedWriteDoesNotCreateDiscovery()
    {
        QTemporaryDir dir;
        WatchStore watch(dir.path());
        QVERIFY(request(watch, "watch.add", {{"keyword", "Linux"}}).ok());
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE", "watch-fixture");
            db.setDatabaseName(dir.filePath("watch.sqlite"));
            QVERIFY(db.open());
            QVERIFY(QSqlQuery(db).exec("CREATE TRIGGER fail BEFORE INSERT ON matches BEGIN SELECT RAISE(ABORT,'fixture'); END"));
        }
        QSqlDatabase::removeDatabase("watch-fixture");
        watch.observe({{QString(40, 'a'), "Linux"}});
        const auto result = request(watch, "watch.list");
        QVERIFY(result.ok());
        QVERIFY(result.data().toObject()["matches"].toArray().isEmpty());
        QVERIFY(result.data().toObject()["missedWrite"].toBool());
    }
};
QTEST_GUILESS_MAIN(TestWatch)
#include "test_watch.moc"
