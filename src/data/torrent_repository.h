#ifndef RATS_DATA_TORRENT_REPOSITORY_H
#define RATS_DATA_TORRENT_REPOSITORY_H

#include "domain/torrent.h"

#include <QHash>
#include <QMutex>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <QVector>
#include <optional>

namespace rats::data {

class Database;

// Repository for the `torrents` and `files` Manticore tables. Owns the mapping
// between rows and domain::Torrent, all search queries, and the incrementally
// maintained statistics. Process lifecycle lives in Manticore and raw SQL in
// Database/SelectQuery.
class TorrentRepository : public QObject {
    Q_OBJECT

public:
    // A write can leave a main torrent row without its requested file list.
    // There is no transaction spanning the two Manticore tables.
    // Unknown is for a chunked batch whose main-table write failed: earlier
    // chunks might have been committed, but the API cannot count them exactly.
    enum class WriteOutcome { Complete, Partial, Failed, Unknown };

    struct FileLookup {
        enum class State { Present, Missing, Error };
        State state = State::Error;
        QVector<domain::File> files;
        // Only set for a readable row when the caller supplies the main row's
        // declared file count. Missing with count 0 remains ambiguous.
        std::optional<bool> countMatches;
    };

    struct SearchQuery {
        QString text;
        int offset = 0;
        int limit = 10;
        QString sort; // user sort key ("seeders", "size", "added"); mapped to a
                      // safe column
        bool descending = true;
        bool safeSearch = false;
        QString contentType; // "video".."archive", or "application" (Software+Games)
        qint64 sizeMin = 0;
        qint64 sizeMax = 0;
        int filesMin = 0;
        int filesMax = 0;
    };

    struct Statistics {
        qint64 torrents = 0;
        qint64 files = 0;
        qint64 totalSize = 0;
    };

    explicit TorrentRepository(Database* db, QObject* parent = nullptr);

    // Load statistics from the existing tables. Call once after the database is
    // up. There are no id counters to restore: a row id is derived from the
    // infohash (data/row_id.h), not handed out sequentially.
    void primeFromDatabase();

    // What a hash resolves to before a write touches the table.
    struct RowSlot {
        qint64 id = 0; // 0 when the hash is not a usable infohash
        bool taken = false; // some row already occupies this id
        bool ours = false; // ... and it carries this very hash
        // The stored row, when it is ours and resolve() was asked for it.
        std::optional<domain::Torrent> stored;
        // taken && !ours is a 63-bit id collision: writing would overwrite an
        // unrelated torrent, so every write path refuses it.
        bool collided() const { return taken && !ours; }
    };

    // One docid lookup answering every question a write has: is this torrent
    // already stored, and is its slot held by a different one? Pass `withRow` to
    // get the stored copy back as well, and the result to add() so the write does
    // not repeat the query — that is what the insert path does instead of a
    // get() followed by an add() that looks the same row up again.
    RowSlot resolve(const QString& hash, bool withRow = false);

    // CRUD ---------------------------------------------------------------------
    // Idempotent: a torrent that is already stored is left untouched and reports
    // success. Use update() to rewrite one. Refuses a torrent whose row id
    // belongs to a different hash.
    bool add(const domain::Torrent& torrent, WriteOutcome* outcome = nullptr);
    // Same, reusing a slot the caller has already resolved. The slot must belong
    // to this torrent's hash; one that does not is refused rather than trusted,
    // so a stale or mismatched slot cannot turn into an overwrite.
    bool add(const domain::Torrent& torrent, const RowSlot& slot, WriteOutcome* outcome = nullptr);
    bool update(const domain::Torrent& torrent);
    bool remove(const QString& hash);
    bool exists(const QString& hash);
    std::optional<domain::Torrent> get(const QString& hash, bool includeFiles = false);

    // Bulk lookup used by the dump importer, where a per-row round trip would
    // dominate the runtime. `hashes` must not exceed Manticore's max_matches
    // (1000) — callers batch.
    //
    // Hashes are resolved to row ids and those are looked up, so a row that comes
    // back under a hash the caller did not ask for is a 63-bit id collision and
    // is dropped rather than reported as a hit. Pass `collided` to learn which of
    // the requested hashes lost such a race: they cannot be stored (their slot
    // belongs to another torrent) and must not be written.
    QHash<QString, domain::Torrent> getMany(const QStringList& hashes, QSet<QString>* collided = nullptr);
    // Insert torrents already known to be absent, in as few statements as the
    // packet limit allows. Statistics move once for the whole batch instead of
    // once per row, so a million-row import does not emit a million signals.
    //
    // Rows sharing a derived id are collapsed to the first: the batch write reports
    // no per-row outcome, so a second row under one id would be dropped by Manticore
    // while still counting towards the statistics here. Callers must still dedupe
    // against the *index* (see getMany's `collided`) — this only guards the batch
    // against itself, which is the one collision no prior lookup can see.
    //
    // Returns the number of main rows reported written, which is what the
    // statistics moved by. Inspect outcome to detect missing file-list writes.
    int addMany(const QVector<domain::Torrent>& torrents, WriteOutcome* outcome = nullptr);

    // Search -------------------------------------------------------------------
    QVector<domain::SearchHit> searchTorrents(const SearchQuery& query);
    QVector<domain::SearchHit> searchFiles(const SearchQuery& query);
    QVector<domain::Torrent> recent(int limit = 10);
    QVector<domain::Torrent> top(const QString& type, const QString& time, int offset, int limit);
    QVector<domain::Torrent> random(int limit = 5, bool includeFiles = false);
    // A page of all torrents ordered by id — used by maintenance sweeps.
    // Keyset ("seek") pagination: pass 0 to start, then the id of the last row
    // of the previous page. OFFSET paging cannot be used for a full sweep —
    // Manticore refuses any offset >= max_matches (1000 by default) with
    // "offset out of bounds", and rows deleted mid-sweep shift the remaining
    // ones into the offsets already consumed.
    QVector<domain::Torrent> pageAfterId(qint64 afterId, int limit);

    // Files --------------------------------------------------------------------
    QVector<domain::File> filesOf(const QString& hash);
    QHash<QString, QVector<domain::File>> filesOf(const QStringList& hashes);
    // Unlike filesOf(), this does not conflate no row with a failed query.
    FileLookup inspectFiles(const QString& hash, std::optional<int> declaredCount = std::nullopt);

    // Partial updates ----------------------------------------------------------
    bool updateTrackerCounts(const QString& hash, int seeders, int leechers, int completed);
    // Replace the file list of an already-stored torrent and sync its file count
    // and statistics. Used to backfill a copy that was indexed from metadata
    // before its file list was available. No-op (returns false) on empty input.
    bool updateFiles(const QString& hash, const QVector<domain::File>& files, WriteOutcome* outcome = nullptr);
    bool mergeInfo(const QString& hash, const QJsonObject& info);
    bool updateClassification(const QString& hash, domain::ContentType type, domain::ContentCategory category);

    // Snapshot of the counters. Safe from any thread: the bulk paths run on the
    // database-sync worker while the GUI reads this, so the three fields have to
    // move together and be published atomically.
    Statistics statistics() const;

    // Row-id migration ---------------------------------------------------------
    // Outcome of one page of re-keying (see migrateRowIdPage).
    struct RowIdMigrationPage {
        int scanned = 0;
        int rewritten = 0;
        // Rows that cannot be re-keyed: an unusable hash, or a slot that already
        // belongs to a different torrent. They are deleted — a row nothing can
        // address by hash is worse than a missing one, because it still inflates
        // the counters and is still served by sweeps.
        int dropped = 0;
        bool finished = false; // no rows left past the cursor
        // The page could not be read or could not be committed. Distinct from
        // `finished`: a failed query yields no rows either, and treating that as
        // the end of the table would leave the sweep short while reporting
        // success. `cursor` is left where the page started, so a retry re-reads
        // exactly these rows.
        bool failed = false;
    };

    // Re-key one page of `table` from the old sequential row ids to the ones
    // derived from each row's infohash, advancing `cursor` past the rows it read.
    //
    // Safe to interrupt and safe to re-run: a row is rewritten only when its id
    // differs from the derived one, so a second pass over an already-migrated
    // table rewrites nothing. Derived ids land far above the counter's range, so
    // an ascending sweep meets every unmigrated row before it meets any rewritten
    // one; the pass ends when the cursor runs off the end of the table.
    //
    // A page that fails to read or to commit comes back with `failed` set and the
    // cursor untouched. The caller must stop there rather than move on: skipping
    // a page would leave those rows under a counter id, where nothing can find
    // them by hash again.
    //
    // Only MigrationService calls this. It lives here because re-keying a row
    // means reading and rewriting it through exactly the same mapping a normal
    // write uses — that mapping is this class's job and nothing else may
    // reimplement it.
    RowIdMigrationPage migrateRowIdPage(const QString& table, qint64& cursor, int limit);

signals:
    void torrentUpdated(const QString& hash);
    void statisticsChanged(qint64 torrents, qint64 files, qint64 totalSize);

private:
    domain::Torrent rowToTorrent(const QVariantMap& row) const;
    QString buildNameIndex(const domain::Torrent& torrent) const;
    bool saveFiles(const QString& hash, const QVector<domain::File>& files);
    // Column map for one torrents row, id included. Shared by add() and addMany().
    QVariantMap torrentRow(const domain::Torrent& torrent);
    // Column map for one files row (the "\n"-joined path/size blobs).
    QVariantMap filesRow(const QString& hash, const QVector<domain::File>& files);
    QVector<domain::Torrent> selectTorrents(const QString& sql, const QVariantList& params = {});

    // Map a user-supplied sort key to a whitelisted column, or empty if unknown.
    static QString resolveSortColumn(const QString& key);
    // Build the "contentType = N" / "contentType IN (5,6)" fragment for a filter.
    static QString contentTypeFilter(const QString& type);

    // Apply a delta to the counters and emit statisticsChanged with the result.
    // The lock covers only the arithmetic — the signal is emitted outside it, so
    // a direct-connected slot cannot re-enter the repository under the lock.
    void bumpStatistics(qint64 torrents, qint64 files, qint64 totalSize);

    Database* db_;
    mutable QMutex statsMutex_; // guards stats_
    Statistics stats_;
};

} // namespace rats::data

#endif // RATS_DATA_TORRENT_REPOSITORY_H
