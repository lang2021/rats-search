#ifndef RATS_REST_API_ROUTER_H
#define RATS_REST_API_ROUTER_H

#include "common/result.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <functional>
#include <memory>

namespace rats::app {
class Application;
}

namespace rats::rest {

struct RemoteSearchSession;

// The unified method surface of the application. A single dotted-name method
// table (e.g. "search.torrents", "download.add", "config.set") maps names to
// handlers that call into the Application's services and answer asynchronously
// with a Result. The HTTP/WS ApiServer and the GUI both dispatch through here,
// so there is exactly one routing mechanism. P2P request handling is a separate
// concern and lives in the peer layer.
class ApiRouter : public QObject {
    Q_OBJECT

public:
    using Handler = std::function<void(const QJsonObject& params, const ResultCallback& respond)>;

    explicit ApiRouter(app::Application* app, QObject* parent = nullptr);

    // Dispatch a method by name. Unknown methods answer with a failure Result.
    // `respond` is always invoked exactly once.
    void call(const QString& method, const QJsonObject& params, const ResultCallback& respond);
    void cancelRemoteSearches(const QString& reason);

signals:
    // Broadcast to all connected clients (WebSocket). Emitted for push events
    // like download progress and feed updates.
    void event(const QString& name, const QJsonObject& data);

private:
    QJsonArray collectRemoteSearchResults(const QString& searchId, const QJsonArray& torrents);
    QHash<QString, QJsonArray> collectLegacySearchCandidates(const QJsonArray& torrents);
    bool finishRemoteSearch(const QString& searchId, const QString& error = {});
    void registerMethods();
    // Re-broadcast service-layer signals (download progress, votes, feed, config,
    // remote search hits, …) to all WebSocket clients as push events, preserving
    // the master event names/shapes so existing API clients keep working.
    void wireEvents();
    void add(const QString& name, Handler handler);

    int queuedShown_ = 0;
    qint64 queuedOmitted_ = 0;
    app::Application* app_;
    QHash<QString, Handler> handlers_;
    QHash<QString, std::shared_ptr<RemoteSearchSession>> remoteSearches_;
};

} // namespace rats::rest

#endif // RATS_REST_API_ROUTER_H
