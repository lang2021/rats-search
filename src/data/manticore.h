#ifndef RATS_DATA_MANTICORE_H
#define RATS_DATA_MANTICORE_H

#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QSqlDatabase>
#include <QString>
#include <QTimer>
#include <memory>
#include <atomic>

class QThread;

namespace rats::data {

/**
 * @brief Manticore - Manages the Manticore Search (searchd) process
 *
 * This class is responsible for:
 * - Starting/stopping the searchd process
 * - Generating the sphinx.conf configuration file
 * - Monitoring the process health
 * - Providing a MySQL connection to the database
 */
class Manticore : public QObject {
    Q_OBJECT

    friend class TestManticoreHealthMonitor;

public:
    enum class Status { Stopped, Starting, Running, Error };
    Q_ENUM(Status)

    explicit Manticore(const QString& dataDirectory, QObject* parent = nullptr);
    ~Manticore();

    /**
     * @brief Start the Manticore Search daemon
     * @return true if started successfully or already running
     */
    bool start();

    /**
     * @brief Stop the Manticore Search daemon
     */
    void stop();

    /**
     * @brief Check if Manticore is running
     */
    bool isRunning() const;
    qint64 databaseSizeBytes() const;

    /**
     * @brief Get the port searchd actually listens on (chosen at start()).
     */
    int port() const { return port_; }

    /**
     * @brief Get QSqlDatabase connection to Manticore
     * @note Must call start() first
     */
    QSqlDatabase getDatabase() const;

    /**
     * @brief Wait for Manticore to be ready (blocking)
     * @param timeoutMs Maximum time to wait in milliseconds
     * @return true if ready, false if timeout
     */
    bool waitForReady(int timeoutMs = 30000);

    /**
     * @brief Delete the Manticore binlog (write-ahead log) files.
     *
     * searchd refuses to start when its binlog cannot be replayed (truncated
     * by a hard kill / power loss / full disk), and Manticore ships no repair
     * tool — the only recovery is to drop the log and continue from the last
     * on-disk RT chunks. start() calls this automatically when it detects that
     * failure mode; exposed publicly for tests and manual maintenance.
     *
     * @return true if at least one binlog file was removed.
     */
    bool resetBinlog();

    /**
     * @brief Keep the searchd-owned logs from growing without bound.
     *
     * Manticore never rotates its own logs, so an installation that runs for
     * months accumulates gigabytes next to the database: deletes the obsolete
     * query.log (query logging is no longer configured at all) and rotates
     * searchd.log once it passes its size cap. Called from start() before the
     * daemon opens them; exposed publicly for tests and manual maintenance.
     */
    void pruneLogs();

    /**
     * @brief Does this searchd log line report an unusable binlog?
     *
     * Matches the fatal replay errors only ("FATAL: binlog: log missing txn
     * marker at pos=... (corrupted?)"), never the informational
     * "binlog: replaying log ..." progress lines. Static so tests can pin the
     * exact set of lines that trigger a reset.
     */
    static bool isBinlogFailureLine(const QString& line);

private slots:
    void onProcessStarted();
    void onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void onProcessError(QProcess::ProcessError processError);
    void onProcessReadyRead();
    void checkConnection();

private:
    bool generateConfig();
    bool createDatabaseDirectories();
    QString findSearchdPath();
    bool testConnection();
    static bool testConnection(int port, const QString& connectionName);
    void stopConnectionMonitor();
    void setStatus(Status status);
    // Log the reason a startup/runtime step failed and move to Status::Error.
    // Every failure path funnels through here, so the cause always reaches the
    // log even though nothing observes the status directly.
    void fail(const QString& message);
    bool isPortAvailable(int port);
    int findAvailablePort(int startPort, int maxAttempts = 10);

    // start() helpers — each returns true on success and, on failure, calls
    // fail() before returning false.
    bool attachToExternalInstance(qint64 startupElapsedMs);
    bool resolveSearchdPath();
    bool ensureAvailablePort();
    bool prepareDatabaseAndConfig();
    bool verifyDriverAvailable();
    bool launchSearchdProcess();

    // Platform-specific searchd teardown, shared by stop() and the binlog
    // recovery restart. Leaves the process signals connected.
    void shutdownProcess();

    // True if the searchd run that just failed blamed the binlog — either on
    // its stdout/stderr (Unix) or in the part of searchd.log written since we
    // launched it (Windows daemon mode, where the pipes belong to the parent
    // that already exited).
    bool binlogFailureReported();

    // waitForReady() helpers.
    void sleepWithEventLoop(int intervalMs);

    /**
     * @brief Compute total size of the Manticore database directory in bytes.
     * @return Size in bytes, or 0 if directory doesn't exist / on error.
     */
    qint64 computeDatabaseSizeBytes() const;

    /**
     * @brief Calculate a startup timeout proportional to the database size.
     *
     * Manticore needs to precache every RT table on startup; the bigger the
     * on-disk data, the longer it takes before `searchd` starts accepting
     * connections. Base timeout is used for empty/small databases; for larger
     * ones additional milliseconds are added per MB of data.
     *
     * @return Timeout in milliseconds, clamped between a sane min and max.
     */
    int computeStartupTimeoutMs() const;

    QString dataDirectory_;
    QString databasePath_;
    QString configPath_;
    QString pidFilePath_;
    QString searchdLogPath_;
    QString searchdPath_;
    int port_;
    Status status_;
    QString version_;

    std::unique_ptr<QProcess> process_;
    std::unique_ptr<QTimer> connectionCheckTimer_;
    QPointer<QThread> connectionProbeThread_;
    std::atomic_bool connectionProbeInFlight_ { false };
    quint64 connectionProbeGeneration_ = 0;
    bool isExternalInstance_;
    bool isWindowsDaemonMode_;
    QString connectionName_;

    // Binlog recovery state, all scoped to a single start() call.
    bool binlogFailureSeen_ = false; // set while parsing this run's searchd output
    bool binlogResetDone_ = false; // at most one reset+retry per start()
    qint64 searchdLogOffset_ = 0; // size of searchd.log just before launch
};

} // namespace rats::data

#endif // RATS_DATA_MANTICORE_H
