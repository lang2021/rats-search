#include "app/explorer_store.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileDevice>
#include <QFileInfo>
#include <QMutexLocker>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QUuid>

#include <utility>

namespace rats::app {
namespace {
constexpr qint64 ttlMs = 24LL * 60 * 60 * 1000;
constexpr int peerCap = 20000;
constexpr int contactCap = 20000;
constexpr int sampleCap = 50000;

qint64 nowMs() { return QDateTime::currentMSecsSinceEpoch(); }

bool ensureSchema(QSqlDatabase& db)
{
    QSqlQuery query(db);
    return query.exec("PRAGMA journal_mode=DELETE") &&
        query.exec("CREATE TABLE IF NOT EXISTS peers("
                   "hash TEXT NOT NULL,ip TEXT NOT NULL,port INTEGER NOT NULL,"
                   "observed INTEGER NOT NULL,verified INTEGER NOT NULL DEFAULT 0,"
                   "PRIMARY KEY(hash,ip,port))") &&
        query.exec("CREATE INDEX IF NOT EXISTS peers_recent ON peers(hash,observed DESC)") &&
        query.exec("CREATE TABLE IF NOT EXISTS contacts("
                   "hash TEXT NOT NULL,ip TEXT NOT NULL,port INTEGER NOT NULL,"
                   "observed INTEGER NOT NULL,PRIMARY KEY(hash,ip,port))") &&
        query.exec("CREATE INDEX IF NOT EXISTS contacts_recent ON contacts(hash,observed DESC)") &&
        query.exec("CREATE TABLE IF NOT EXISTS samples("
                   "target_hash TEXT NOT NULL,sample_hash TEXT NOT NULL,observed INTEGER NOT NULL,"
                   "PRIMARY KEY(target_hash,sample_hash))") &&
        query.exec("CREATE INDEX IF NOT EXISTS samples_recent ON samples(target_hash,observed DESC)");
}

bool pruneExpired(QSqlDatabase& db, qint64 cutoff)
{
    QSqlQuery query(db);
    return query.exec(QString("DELETE FROM peers WHERE observed<%1").arg(cutoff)) &&
        query.exec(QString("DELETE FROM contacts WHERE observed<%1").arg(cutoff)) &&
        query.exec(QString("DELETE FROM samples WHERE observed<%1").arg(cutoff));
}

bool pruneToCap(QSqlDatabase& db, const QString& table, int cap)
{
    QSqlQuery query(db);
    return query.exec(QString("DELETE FROM %1 WHERE rowid IN("
                              "SELECT rowid FROM %1 ORDER BY observed DESC LIMIT -1 OFFSET %2)")
                          .arg(table).arg(cap));
}
} // namespace

ExplorerStore::ExplorerStore(QString directory, QObject* parent)
    : QObject(parent), path_(QDir(std::move(directory)).filePath("explorer.sqlite"))
{
}

bool ExplorerStore::withDatabase(const std::function<bool(QSqlDatabase&)>& operation) const
{
    QMutexLocker lock(&mutex_);
    if (!QDir().mkpath(QFileInfo(path_).absolutePath())) return false;

    const QString connectionName = QUuid::createUuid().toString(QUuid::WithoutBraces);
    bool ok = false;
    {
        auto db = QSqlDatabase::addDatabase("QSQLITE", connectionName);
        db.setDatabaseName(path_);
        db.setConnectOptions("QSQLITE_BUSY_TIMEOUT=1000");
        if (db.open()) {
            QFile file(path_);
            if (file.exists())
                file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
            if (ensureSchema(db)) ok = operation(db);
            if (file.exists())
                file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        }
        db.close();
    }
    QSqlDatabase::removeDatabase(connectionName);
    return ok;
}

bool ExplorerStore::ready() const
{
    return withDatabase([](QSqlDatabase&) { return true; });
}

bool ExplorerStore::recordPeer(const QString& hash, const QString& ip,
                               std::uint16_t port, bool handshake)
{
    if (hash.isEmpty() || ip.isEmpty() || port == 0) return false;
    const qint64 now = nowMs();
    return withDatabase([&](QSqlDatabase& db) {
        QSqlQuery query(db);
        if (!query.prepare("INSERT INTO peers VALUES(?,?,?,?,?) "
                           "ON CONFLICT(hash,ip,port) DO UPDATE SET "
                           "observed=excluded.observed,verified=MAX(peers.verified,excluded.verified)"))
            return false;
        query.addBindValue(hash.toLower());
        query.addBindValue(ip);
        query.addBindValue(port);
        query.addBindValue(now);
        query.addBindValue(handshake ? now : 0);
        return query.exec() && pruneExpired(db, now - ttlMs) &&
            pruneToCap(db, "peers", peerCap);
    });
}

bool ExplorerStore::recordContact(const QString& hash, const QString& ip,
                                  std::uint16_t port)
{
    if (hash.isEmpty() || ip.isEmpty() || port == 0) return false;
    const qint64 now = nowMs();
    return withDatabase([&](QSqlDatabase& db) {
        QSqlQuery query(db);
        if (!query.prepare("INSERT INTO contacts VALUES(?,?,?,?) "
                           "ON CONFLICT(hash,ip,port) DO UPDATE SET observed=excluded.observed"))
            return false;
        query.addBindValue(hash.toLower());
        query.addBindValue(ip);
        query.addBindValue(port);
        query.addBindValue(now);
        return query.exec() && pruneExpired(db, now - ttlMs) &&
            pruneToCap(db, "contacts", contactCap);
    });
}

bool ExplorerStore::recordSample(const QString& targetHash, const QString& sampleHash)
{
    if (targetHash.isEmpty() || sampleHash.isEmpty()) return false;
    const qint64 now = nowMs();
    return withDatabase([&](QSqlDatabase& db) {
        QSqlQuery query(db);
        if (!query.prepare("INSERT INTO samples VALUES(?,?,?) "
                           "ON CONFLICT(target_hash,sample_hash) DO UPDATE SET observed=excluded.observed"))
            return false;
        query.addBindValue(targetHash.toLower());
        query.addBindValue(sampleHash.toLower());
        query.addBindValue(now);
        return query.exec() && pruneExpired(db, now - ttlMs) &&
            pruneToCap(db, "samples", sampleCap);
    });
}

QVector<ExplorerStore::PeerHint> ExplorerStore::peers(const QString& hash, int limit)
{
    QVector<PeerHint> result;
    const qint64 cutoff = nowMs() - ttlMs;
    withDatabase([&](QSqlDatabase& db) {
        QSqlQuery query(db);
        if (!query.prepare("SELECT ip,port,verified FROM peers WHERE hash=? AND observed>=? "
                           "ORDER BY observed DESC LIMIT ?")) return false;
        query.addBindValue(hash.toLower());
        query.addBindValue(cutoff);
        query.addBindValue(qBound(1, limit, 32));
        if (!query.exec()) return false;
        while (query.next())
            result.append({query.value(0).toString(),
                           static_cast<std::uint16_t>(query.value(1).toUInt()),
                           query.value(2).toLongLong() > 0});
        return pruneExpired(db, cutoff);
    });
    return result;
}

QVector<ExplorerStore::PeerHint> ExplorerStore::contacts(const QString& hash, int limit)
{
    QVector<PeerHint> result;
    const qint64 cutoff = nowMs() - ttlMs;
    withDatabase([&](QSqlDatabase& db) {
        QSqlQuery query(db);
        if (!query.prepare("SELECT ip,port FROM contacts WHERE hash=? AND observed>=? "
                           "ORDER BY observed DESC LIMIT ?")) return false;
        query.addBindValue(hash.toLower());
        query.addBindValue(cutoff);
        query.addBindValue(qBound(1, limit, 8));
        if (!query.exec()) return false;
        while (query.next())
            result.append({query.value(0).toString(),
                           static_cast<std::uint16_t>(query.value(1).toUInt()), false});
        return pruneExpired(db, cutoff);
    });
    return result;
}

std::optional<ExplorerStore::Counts> ExplorerStore::counts(const QString& hash)
{
    Counts counts;
    const qint64 cutoff = nowMs() - ttlMs;
    const bool ok = withDatabase([&](QSqlDatabase& db) {
        if (!pruneExpired(db, cutoff)) return false;
        const auto readCount = [&](const QString& sql, int* value, bool includeVerified = false) {
            QSqlQuery query(db);
            if (!query.prepare(sql)) return false;
            query.addBindValue(hash.toLower());
            query.addBindValue(cutoff);
            if (includeVerified) query.addBindValue(cutoff);
            if (!query.exec() || !query.next()) return false;
            *value = query.value(0).toInt();
            return true;
        };
        return readCount("SELECT COUNT(*) FROM peers WHERE hash=? AND observed>=?", &counts.peers) &&
            readCount("SELECT COUNT(*) FROM peers WHERE hash=? AND observed>=? AND verified>=?",
                      &counts.verifiedPeers, true) &&
            readCount("SELECT COUNT(*) FROM contacts WHERE hash=? AND observed>=?", &counts.contacts) &&
            readCount("SELECT COUNT(*) FROM samples WHERE target_hash=? AND observed>=?", &counts.samples);
    });
    return ok ? std::optional<Counts>(counts) : std::nullopt;
}

} // namespace rats::app
