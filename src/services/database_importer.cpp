#include "services/database_importer.h"

#include "services/database_dump.h"
#include "services/indexing_service.h"

#include <QDebug>

#include <utility>

namespace rats::service {

DatabaseImporter::DatabaseImporter(IndexingService* indexing) : indexing_(indexing) { }

DatabaseImporter::Result DatabaseImporter::run(
    const QString& path, const Options& options, const CancelToken& cancel, const ProgressFn& onProgress)
{
    Result result;
    if (!indexing_) {
        result.error = tr("Indexing is not available.");
        return result;
    }

    DumpReader reader;
    QString error;
    if (!reader.open(path, &error)) {
        result.error = error;
        return result;
    }

    if (options.startOffset > 0 && reader.seekTo(options.startOffset)) {
        qInfo() << "[DatabaseImport] resuming" << path << "at byte" << options.startOffset;
    }
    result.offset = reader.offset();

    IndexingService::BatchOptions batchOptions;
    batchOptions.applyFilters = options.applyFilters;

    QVector<domain::Torrent> batch;
    while (!cancel.cancelled() && reader.readBatch(batch, &error)) {
        // Size first: insertBatch takes the vector by value, and moving into it
        // saves copying a frame's worth of torrents (with their file lists) that
        // readBatch is about to clear anyway.
        result.processed += batch.size();
        const IndexingService::BatchResult applied = indexing_->insertBatch(std::move(batch), batchOptions);
        result.inserted += applied.inserted;
        result.merged += applied.merged;
        result.rejected += applied.rejected;
        result.collided += applied.collided;
        if (applied.writeFailed) {
            // The two tables have no shared transaction. Do not mark this frame
            // committed or advance its resume offset when a file write fails.
            result.error = applied.error;
            return result;
        }
        result.offset = reader.offset();
        if (onProgress)
            onProgress(result);
    }

    result.cancelled = cancel.cancelled();
    result.truncated = reader.atEnd() && !reader.complete();

    // A read that stopped short of the end *and* reported an error is a real
    // failure. Running off the end without a footer is not: the frames already
    // merged are valid, and truncated says so.
    if (!reader.atEnd() && !error.isEmpty()) {
        result.error = error;
        return result;
    }

    result.ok = !result.cancelled;
    return result;
}

} // namespace rats::service
