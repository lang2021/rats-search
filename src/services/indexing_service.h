#ifndef RATS_SERVICE_INDEXING_SERVICE_H
#define RATS_SERVICE_INDEXING_SERVICE_H

#include "domain/torrent.h"

#include <QObject>
#include <QVector>
#include <utility>

namespace rats::data {
class TorrentRepository;
}

namespace rats::service {

class FilterPolicy;

// The single entry point for putting a torrent into the index, no matter the
// source (DHT crawler, P2P replication, .torrent import, feed sync). The flow
// is: validate -> dedupe (merging incoming votes) -> classify -> filter ->
// persist -> notify.
class IndexingService : public QObject {
    Q_OBJECT

public:
    struct Result {
        bool success = false;
        bool alreadyExists = false;
        bool partialWrite = false; // a main/file row changed but requested files were not fully saved
        bool filesBackfilled = false; // existing row gained a file list and both writes succeeded
        domain::Torrent torrent;
        QString error;
    };

    // Outcome of one insertBatch() call.
    struct BatchResult {
        int inserted = 0; // genuinely new rows written
        int merged = 0; // already present; votes/file list may have been merged
        int rejected = 0; // refused by the filter policy
        int invalid = 0; // no valid hash or no name
        // Row id already belongs to a different torrent. Row ids are derived
        // from the infohash (data/row_id.h) and carry 63 of its bits, so this is
        // a birthday collision — ~5e-6 across a 10M index. Storing such a
        // torrent would overwrite an unrelated one, so it is skipped instead.
        int collided = 0;
        bool writeFailed = false;
        bool partialWrite = false;
        QString error;
    };

    struct BatchOptions {
        // Run the local filter policy over every incoming torrent, exactly as a
        // crawled one. Turning this off imports a foreign index verbatim.
        bool applyFilters = true;
        // Merge votes / backfill file lists on torrents that already exist.
        bool mergeExisting = true;
    };

    IndexingService(data::TorrentRepository* repository, FilterPolicy* filter, QObject* parent = nullptr);

    // Insert `torrent`, running the content classifier first when its content
    // type is unknown. Returns the stored torrent (or the pre-existing one) on
    // success.
    Result insert(domain::Torrent torrent);

    // Bulk insert for mass sources (database-dump import), same flow as insert()
    // but with the per-row round trips collapsed: one existence query and one
    // multi-row write per batch. `torrents` should be at most a few hundred
    // entries — Manticore caps an IN() lookup at max_matches rows.
    //
    // It deliberately does NOT emit torrentIndexed: that signal starts a tracker
    // scrape per torrent, which is right for a trickle of crawled torrents and
    // catastrophic for a million imported ones. Listeners that care about mass
    // imports watch the repository's statisticsChanged instead.
    //
    // `options` has no default argument on purpose: GCC refuses to evaluate a
    // nested struct's member initialisers while the enclosing class is still
    // incomplete, which is exactly what a `= BatchOptions()` default would need.
    // The one-argument overload below does the same job from a context where the
    // class is complete.
    BatchResult insertBatch(QVector<domain::Torrent> torrents, const BatchOptions& options);
    BatchResult insertBatch(QVector<domain::Torrent> torrents)
    {
        return insertBatch(std::move(torrents), BatchOptions());
    }

    // Whether a torrent passes the current filter policy (used by the
    // maintenance sweep that re-applies filters to the existing index).
    bool accepts(const domain::Torrent& torrent) const;

signals:
    // Emitted once when a genuinely new torrent has been indexed. Carries the
    // full stored entity so listeners (tracker checks, UI, replication) need no
    // second lookup.
    void torrentIndexed(const domain::Torrent& torrent);

private:
    data::TorrentRepository* repository_;
    FilterPolicy* filter_;
};

} // namespace rats::service

#endif // RATS_SERVICE_INDEXING_SERVICE_H
