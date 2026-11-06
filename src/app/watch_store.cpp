#include "app/watch_store.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QUuid>

namespace rats::app {
namespace {
Result sqlFailure(const QSqlError& e)
{
    qWarning() << "[Watch] local database:" << e.databaseText();
    return Result::failure(QStringLiteral("Watch 本地数据库读写失败"));
}

bool execute(QSqlQuery& q, const QString& sql, const QVariantList& values = {})
{
    if (!q.prepare(sql)) return false;
    for (const auto& value : values) q.addBindValue(value);
    return q.exec();
}

QJsonArray rows(QSqlQuery& q)
{
    QJsonArray list;
    const auto fields = q.record();
    while (q.next()) {
        QJsonObject item;
        for (int i = 0; i < fields.count(); ++i)
            item.insert(fields.fieldName(i), QJsonValue::fromVariant(q.value(i)));
        list.append(item);
    }
    return list;
}
} // namespace

class WatchStore::Storage : public QObject {
public:
    explicit Storage(const QString& directory) : directory(directory) {}
    QString directory;
    const QString connection = QUuid::createUuid().toString();
    QSqlDatabase db;
    bool ready = false;
    bool missedWrite = false;

    bool open()
    {
        if (ready) return true;
        if (!db.isValid()) {
            db = QSqlDatabase::addDatabase("QSQLITE", connection);
            db.setDatabaseName(QDir(directory).filePath("watch.sqlite"));
            db.setConnectOptions("QSQLITE_BUSY_TIMEOUT=1000");
        }
        if (!QDir().mkpath(directory)) return false;
        QFile file(db.databaseName());
        if (!file.exists()) {
            if (!file.open(QIODevice::NewOnly | QIODevice::ReadWrite, QFile::ReadOwner | QFile::WriteOwner))
                return false;
            file.close();
        }
        if (!db.isOpen() && !db.open()) return false;
        QSqlQuery q(db);
        for (auto sql : {
                "PRAGMA foreign_keys=ON",
                "CREATE TABLE IF NOT EXISTS rules(id INTEGER PRIMARY KEY AUTOINCREMENT, keyword TEXT NOT NULL COLLATE NOCASE UNIQUE, enabled INTEGER NOT NULL DEFAULT 1)",
                "CREATE TABLE IF NOT EXISTS matches(id INTEGER PRIMARY KEY AUTOINCREMENT, rule INTEGER NOT NULL REFERENCES rules(id) ON DELETE CASCADE, hash TEXT NOT NULL, name TEXT NOT NULL, time INTEGER NOT NULL, seen INTEGER NOT NULL DEFAULT 0, UNIQUE(rule,hash))",
                "CREATE INDEX IF NOT EXISTS unread_matches ON matches(id) WHERE seen=0" }) {
            if (!q.exec(sql)) { sqlFailure(q.lastError()); return false; }
        }
        ready = true;
        return true;
    }

    Result request(const QString& method, const QJsonObject& p, bool& changed)
    {
        if (!open()) return Result::failure("Watch 本地数据库不可用");
        QSqlQuery q(db);
        if (method == "watch.unread") {
            if (!q.exec("SELECT COUNT(*) FROM matches WHERE seen=0") || !q.next()) return sqlFailure(q.lastError());
            return Result::success(q.value(0).toLongLong());
        }
        if (method == "watch.list") {
            if (!q.exec("SELECT r.id,r.keyword,r.enabled,COUNT(m.id) AS total,COUNT(CASE WHEN m.seen=0 THEN 1 END) AS unread FROM rules r LEFT JOIN matches m ON m.rule=r.id GROUP BY r.id ORDER BY r.id DESC"))
                return sqlFailure(q.lastError());
            const auto rules = rows(q);
            QString sql = "SELECT m.id,r.keyword,m.hash,m.name,m.time,m.seen FROM matches m JOIN rules r ON r.id=m.rule WHERE 1=1";
            QVariantList args;
            if (p["unreadOnly"].toBool()) sql += " AND m.seen=0";
            if (const auto before = p["before"].toInteger(); before > 0) {
                sql += " AND m.id<?";
                args.append(before);
            }
            if (!execute(q, sql + " ORDER BY m.id DESC LIMIT 51", args)) return sqlFailure(q.lastError());
            auto matches = rows(q);
            const bool more = matches.size() > 50;
            if (more) matches.removeLast();
            return Result::success(QJsonObject{{"rules", rules}, {"matches", matches}, {"more", more}, {"missedWrite", missedWrite}});
        }
        const auto id = p["id"].toInteger();
        if (method == "watch.add") {
            const QString keyword = p["keyword"].toString().trimmed();
            if (keyword.isEmpty() || keyword.size() > 256) return Result::failure("关键词须为 1 至 256 个字符");
            if (!execute(q, "INSERT INTO rules(keyword) VALUES(?) ON CONFLICT(keyword) DO NOTHING", {keyword}))
                return sqlFailure(q.lastError());
            if (!q.numRowsAffected()) return Result::failure("该关键词已存在");
        } else if (id <= 0) {
            return Result::failure("无效的 Watch 记录编号");
        } else if (method == "watch.enable" && p["enabled"].isBool()) {
            if (!execute(q, "UPDATE rules SET enabled=? WHERE id=?", {p["enabled"].toBool(), id})) return sqlFailure(q.lastError());
        } else if (method == "watch.delete") {
            if (!execute(q, "DELETE FROM rules WHERE id=?", {id})) return sqlFailure(q.lastError());
        } else if (method == "watch.read") {
            if (!execute(q, "UPDATE matches SET seen=1 WHERE id=?", {id})) return sqlFailure(q.lastError());
        } else {
            return Result::failure("无效的 Watch 操作");
        }
        changed = q.numRowsAffected() > 0;
        return changed ? Result::success(true) : Result::failure("记录不存在");
    }

    Result match(const QMap<QString, QString>& names, qint64 time, bool& changed)
    {
        if (!open()) return Result::failure("Watch 本地数据库不可用");
        QSqlQuery rules(db);
        if (!rules.exec("SELECT id,keyword FROM rules WHERE enabled=1")) return sqlFailure(rules.lastError());
        if (!rules.next()) return Result::success();
        QSqlQuery insert(db);
        if (!insert.prepare("INSERT INTO matches(rule,hash,name,time) VALUES(?,?,?,?) ON CONFLICT(rule,hash) DO NOTHING"))
            return sqlFailure(insert.lastError());
        bool transaction = false;
        do {
            const auto keyword = rules.value(1).toString();
            for (auto it = names.cbegin(); it != names.cend(); ++it) {
                if (!it.value().contains(keyword, Qt::CaseInsensitive)) continue;
                if (!transaction) {
                    if (!db.transaction()) return sqlFailure(db.lastError());
                    transaction = true;
                }
                insert.bindValue(0, rules.value(0));
                insert.bindValue(1, it.key().toLower());
                insert.bindValue(2, it.value());
                insert.bindValue(3, time);
                if (!insert.exec()) { db.rollback(); return sqlFailure(insert.lastError()); }
                changed |= insert.numRowsAffected() > 0;
            }
        } while (rules.next());
        rules.finish();
        insert.finish();
        if (transaction && !db.commit()) {
            auto err = db.lastError();
            db.rollback();
            return sqlFailure(err);
        }
        return Result::success();
    }
};

WatchStore::WatchStore(const QString& directory) : storage_(new Storage(directory))
{
    storage_->moveToThread(&thread_);
    connect(&thread_, &QThread::finished, storage_, &QObject::deleteLater);
    thread_.start();
}

WatchStore::~WatchStore()
{
    // Drain accepted writes before closing the SQL connection on its own thread.
    QMetaObject::invokeMethod(storage_, [this] {
        storage_->db = {};
        QSqlDatabase::removeDatabase(storage_->connection);
    }, Qt::BlockingQueuedConnection);
    thread_.quit();
    thread_.wait();
}

void WatchStore::request(const QString& method, const QJsonObject& params, ResultCallback respond)
{
    QMetaObject::invokeMethod(storage_, [this, method, params, respond = std::move(respond)] {
        bool notify = false;
        const auto result = storage_->request(method, params, notify);
        QMetaObject::invokeMethod(this, [this, result, notify, respond] {
            if (result.ok() && notify) emit changed();
            respond(result);
        }, Qt::QueuedConnection);
    }, Qt::QueuedConnection);
}

void WatchStore::observe(const QMap<QString, QString>& names)
{
    const auto time = QDateTime::currentMSecsSinceEpoch();
    QMetaObject::invokeMethod(storage_, [this, names, time] {
        bool notify = false;
        const auto result = storage_->match(names, time, notify);
        if (!result.ok()) {
            storage_->missedWrite = true;
            qWarning() << "[Watch]" << result.error();
        }
        if (notify || !result.ok()) emit changed();
    }, Qt::QueuedConnection);
}
} // namespace rats::app
