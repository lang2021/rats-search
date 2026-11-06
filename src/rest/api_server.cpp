#include "rest/api_server.h"

#include "app/application.h"
#include "common/infohash.h"
#include "common/result.h"
#include "data/database.h"
#include "data/torrent_repository.h"
#include "net/crawler.h"
#include "net/p2p_transport.h"
#include "rest/api_router.h"
#include "services/download_service.h"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMap>
#include <QPointer>
#include <QRandomGenerator>
#include <QSaveFile>
#include <QSet>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QWebSocket>
#include <QWebSocketServer>

namespace rats::rest {

// ============================================================================
// HTTP parsing helpers
// ============================================================================

namespace {

struct HttpRequest {
    QString method;
    QString path;
    QUrlQuery query;
    QMap<QString, QString> headers; // lower-cased keys
    QByteArray body;
};

// Parse the request line and headers from the header block (bytes before the
// terminating blank line). The body is filled in separately by the caller once
// Content-Length bytes are available.
HttpRequest parseHttpHeader(const QByteArray& header)
{
    HttpRequest req;

    const QString str = QString::fromUtf8(header);
    const QStringList lines = str.split("\r\n");
    if (lines.isEmpty()) {
        return req;
    }

    const QStringList requestLine = lines[0].split(' ');
    if (requestLine.size() >= 2) {
        req.method = requestLine[0].toUpper();
        const QUrl url(requestLine[1]);
        req.path = url.path();
        req.query = QUrlQuery(url.query());
    }

    for (int i = 1; i < lines.size(); ++i) {
        if (lines[i].isEmpty()) {
            continue;
        }
        const int colon = lines[i].indexOf(':');
        if (colon > 0) {
            const QString key = lines[i].left(colon).trimmed().toLower();
            const QString value = lines[i].mid(colon + 1).trimmed();
            req.headers[key] = value;
        }
    }

    return req;
}

QByteArray buildHttpResponse(int statusCode, const QString& statusText, const QByteArray& body,
    const QString& contentType = QStringLiteral("application/json"))
{
    QByteArray response;
    response.append(QStringLiteral("HTTP/1.1 %1 %2\r\n").arg(statusCode).arg(statusText).toUtf8());
    response.append(QStringLiteral("Content-Type: %1\r\n").arg(contentType).toUtf8());
    response.append(QStringLiteral("Content-Length: %1\r\n").arg(body.size()).toUtf8());
    response.append("Access-Control-Allow-Origin: *\r\n");
    response.append("Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n");
    response.append("Access-Control-Allow-Headers: Content-Type, Authorization\r\n");
    response.append("Cache-Control: no-store\r\n");
    response.append("Connection: close\r\n");
    response.append("\r\n");
    response.append(body);
    return response;
}

// Coerce a raw query-string value into the most specific JSON type it looks
// like (object/array, number, bool, otherwise string) — matches the legacy
// behavior.
QJsonValue coerceQueryValue(const QString& value)
{
    if ((value.startsWith('{') && value.endsWith('}')) || (value.startsWith('[') && value.endsWith(']'))) {
        const QJsonDocument doc = QJsonDocument::fromJson(value.toUtf8());
        if (!doc.isNull()) {
            return doc.isArray() ? QJsonValue(doc.array()) : QJsonValue(doc.object());
        }
    }

    bool ok = false;
    const int intVal = value.toInt(&ok);
    if (ok) {
        return QJsonValue(intVal);
    }

    if (value.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0) {
        return QJsonValue(true);
    }
    if (value.compare(QStringLiteral("false"), Qt::CaseInsensitive) == 0) {
        return QJsonValue(false);
    }

    return QJsonValue(value);
}

// Serialize a Result into the legacy response envelope.
QJsonObject resultToEnvelope(const Result& result, const QString& requestId)
{
    QJsonObject obj;
    obj["success"] = result.ok();
    if (result.ok()) {
        const QJsonValue& data = result.data();
        if (!data.isNull() && !data.isUndefined()) {
            obj["data"] = data;
        }
    } else if (!result.error().isEmpty()) {
        obj["error"] = result.error();
    }
    if (!requestId.isEmpty()) {
        obj["requestId"] = requestId;
    }
    return obj;
}

} // namespace

// ============================================================================
// ApiServer
// ============================================================================

ApiServer::ApiServer(ApiRouter* router, QObject* parent) : QObject(parent), router_(router)
{
    // Single unified event channel: whatever the router broadcasts is pushed to
    // every WebSocket client.
    if (router_) {
        connect(router_, &ApiRouter::event, this,
            [this](const QString& name, const QJsonObject& data) { broadcastEvent(name, data); });
    }
}

ApiServer::~ApiServer()
{
    stop();
}

void ApiServer::setAuthorizedFileRoots(const QStringList& roots)
{
    configuredFileRoots_ = roots;
}

bool ApiServer::initializeSecurity()
{
    authorizedFileRoots_.clear();
    for (const QString& root : configuredFileRoots_) {
        if (!QDir::isAbsolutePath(root)) return false;
        const QFileInfo info(root);
        if (!info.isDir() || info.canonicalFilePath().isEmpty()) return false;
        authorizedFileRoots_.append(QDir::cleanPath(info.canonicalFilePath()));
    }

    // Tests without an Application receive a process-local secret. Normal runs
    // keep the secret in the selected data directory so the user can unlock the
    // browser without putting credentials in command-line arguments or logs.
    if (!app_) {
        if (authenticationToken_.isEmpty()) {
            QByteArray token;
            for (int i = 0; i < 4; ++i)
                token += QByteArray::number(QRandomGenerator::system()->generate64(), 16).rightJustified(16, '0');
            authenticationToken_ = token;
        }
        return true;
    }

    const QString dataRoot = QFileInfo(app_->options().dataDirectory).canonicalFilePath();
    if (dataRoot.isEmpty()) return false;
    const QString fallbackUi = QDir::currentPath() + QStringLiteral("/webui");
    for (const QString& staticRoot : {webuiDir_.isEmpty() ? fallbackUi : webuiDir_,
             legacyWebuiDir_.isEmpty() ? fallbackUi : legacyWebuiDir_}) {
        const QString canonical = QFileInfo(staticRoot).canonicalFilePath();
        if (!canonical.isEmpty() && (dataRoot == canonical ||
                dataRoot.startsWith(canonical + QDir::separator()))) {
            qWarning() << "Web UI directory cannot contain the API data directory";
            return false;
        }
    }

    const QString path = QDir(app_->options().dataDirectory).filePath(QStringLiteral("api-token"));
    const QFileInfo info(path);
    if (info.exists() || info.isSymbolicLink()) {
        if (!info.isFile() || info.isSymbolicLink()) return false;
#ifndef Q_OS_WIN
        const auto unsafe = QFileDevice::ReadGroup | QFileDevice::WriteGroup | QFileDevice::ExeGroup |
            QFileDevice::ReadOther | QFileDevice::WriteOther | QFileDevice::ExeOther;
        if (info.permissions() & unsafe) return false;
#endif
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) return false;
        authenticationToken_ = file.readAll().trimmed();
    } else {
        QByteArray token;
        for (int i = 0; i < 4; ++i)
            token += QByteArray::number(QRandomGenerator::system()->generate64(), 16).rightJustified(16, '0');
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly)) return false;
#ifndef Q_OS_WIN
        if (!file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)) return false;
#endif
        if (file.write(token + '\n') != token.size() + 1 || !file.commit()) return false;
        authenticationToken_ = token;
    }

    if (authenticationToken_.size() != 64) return false;
    for (char c : authenticationToken_) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

bool ApiServer::isAuthorized(const QByteArray& authorization) const
{
    const QByteArray value = authorization.trimmed();
    if (!value.startsWith("Bearer ") || authenticationToken_.isEmpty()) return false;
    const QByteArray supplied = value.mid(7);
    if (supplied.size() != authenticationToken_.size()) return false;
    unsigned char difference = 0;
    for (qsizetype i = 0; i < supplied.size(); ++i)
        difference |= static_cast<unsigned char>(supplied[i] ^ authenticationToken_[i]);
    return difference == 0;
}

bool ApiServer::isAuthorizedPath(const QString& path, bool existing) const
{
    if (path.isEmpty() || !QDir::isAbsolutePath(path) || authorizedFileRoots_.isEmpty()) return false;
    // Do not normalize away '..' before resolving symlinks. The OS follows a
    // symlink first and only then processes '..', which can escape the root even
    // when QDir::cleanPath(path) appears to be inside it.
    if (QDir::fromNativeSeparators(path).split('/').contains(QStringLiteral(".."))) return false;
    const QString cleaned = QDir::cleanPath(path);
    QFileInfo info(cleaned);
    if (info.isSymbolicLink() && !info.exists()) return false; // dangling link
    if (existing && !info.exists()) return false;

    QString resolved;
    if (info.exists()) {
        resolved = info.canonicalFilePath();
    } else {
        // The target may not exist yet. Resolve every existing parent before
        // allowing the backend to create the final path.
        QString ancestor = cleaned;
        QStringList missing;
        while (!QFileInfo::exists(ancestor)) {
            const QFileInfo part(ancestor);
            if (part.isSymbolicLink()) return false;
            missing.prepend(part.fileName());
            const QString parent = part.absolutePath();
            if (parent == ancestor) return false;
            ancestor = parent;
        }
        const QFileInfo parentInfo(ancestor);
        if (!parentInfo.isDir()) return false;
        resolved = parentInfo.canonicalFilePath();
        for (const QString& part : missing) resolved = QDir(resolved).filePath(part);
    }
    resolved = QDir::cleanPath(resolved);
    if (app_) {
        const QDir dataDir(QFileInfo(app_->options().dataDirectory).canonicalFilePath());
        if (resolved == dataDir.filePath(QStringLiteral("api-token")) ||
            resolved == dataDir.filePath(QStringLiteral("identity.key"))) return false;
    }
    for (const QString& root : authorizedFileRoots_) {
        const QString prefix = root.endsWith(QDir::separator()) ? root : root + QDir::separator();
        if (resolved == root || resolved.startsWith(prefix)) return true;
    }
    return false;
}

bool ApiServer::validateFileAccess(const QString& method, const QJsonObject& params, QString* error) const
{
    auto requirePath = [this, error](const QString& path, bool existing) {
        if (isAuthorizedPath(path, existing)) return true;
        if (error) *error = QStringLiteral("Local file path is outside authorized API roots");
        return false;
    };
    auto aliasedPath = [&params]() {
        return (params.contains("path") ? params["path"] : params["file"]).toString();
    };
    auto optionalOutput = [&params, &requirePath](const char* key) {
        const QString path = params[QLatin1String(key)].toString();
        return path.isEmpty() || requirePath(path, false);
    };

    if (method == QLatin1String("torrent.create")) {
        const QString source = params["path"].toString();
        if (!requirePath(source, true) || !optionalOutput("output")) return false;
        // TorrentCreator recursively follows directory entries, including
        // symlinks. Reject links in an authorized tree so a private file outside
        // the root cannot be hashed or seeded through a nested alias.
        if (QFileInfo(source).isDir()) {
            QDirIterator entries(source, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::System,
                QDirIterator::Subdirectories);
            while (entries.hasNext()) {
                entries.next();
                const QFileInfo entry = entries.fileInfo();
                if (entry.isSymbolicLink() || !isAuthorizedPath(entry.absoluteFilePath(), true)) {
                    if (error) *error = QStringLiteral("Torrent source contains a link or path outside authorized API roots");
                    return false;
                }
            }
        }
        return true;
    }
    if (method == QLatin1String("torrent.import") || method == QLatin1String("database.import"))
        return requirePath(aliasedPath(), true);
    if (method == QLatin1String("database.export"))
        return requirePath(aliasedPath(), false);
    if (method == QLatin1String("download.addFile"))
        return requirePath(aliasedPath(), true) && optionalOutput("savePath");
    if (method == QLatin1String("download.add"))
        return optionalOutput("savePath");
    if (method == QLatin1String("config.set") && params.contains("downloadPath"))
        return requirePath(params["downloadPath"].toString(), false);
    return true;
}

bool ApiServer::start(int httpPort, int wsPort, const QString& bindAddress)
{
    if (running_) {
        return true;
    }

    const QHostAddress host(bindAddress);
    if (host.isNull()) {
        qWarning() << "Invalid API bind address:" << bindAddress;
        return false;
    }
    if (!initializeSecurity()) {
        qWarning() << "API security initialization failed";
        return false;
    }
    // ---- HTTP server ----
    if (httpPort > 0) {
        httpServer_ = std::make_unique<QTcpServer>(this);

        connect(httpServer_.get(), &QTcpServer::newConnection, this, [this]() {
            while (httpServer_->hasPendingConnections()) {
                QTcpSocket* socket = httpServer_->nextPendingConnection();

                connect(socket, &QTcpSocket::readyRead, this, [this, socket]() { onHttpReadyRead(socket); });
                connect(socket, &QTcpSocket::disconnected, this, [this, socket]() {
                    httpBuffers_.remove(socket);
                    socket->deleteLater();
                });
            }
        });

        if (!httpServer_->listen(host, static_cast<quint16>(httpPort))) {
            qWarning() << "Failed to start HTTP server on port" << httpPort << ":" << httpServer_->errorString();
            httpServer_.reset();
            return false;
        }

        qInfo() << "HTTP API server listening on port" << httpServer_->serverPort();
        httpPort_ = httpServer_->serverPort();
    }

    // ---- WebSocket server ----
    int wsPortActual = wsPort;
    if (wsPort == -1 && httpPort > 0) {
        wsPortActual = httpPort + 1;
    }

    if (wsPortActual > 0) {
        wsServer_ = std::make_unique<QWebSocketServer>("RatsAPI", QWebSocketServer::NonSecureMode, this);

        connect(wsServer_.get(), &QWebSocketServer::newConnection, this, [this]() {
            while (wsServer_->hasPendingConnections()) {
                QWebSocket* socket = wsServer_->nextPendingConnection();
                wsClients_.append(socket);

                QPointer<QWebSocket> guardedSocket(socket);
                QTimer::singleShot(10000, this, [this, guardedSocket]() {
                    if (guardedSocket && !authenticatedWsClients_.contains(guardedSocket))
                        guardedSocket->close(QWebSocketProtocol::CloseCodePolicyViolated, "Authentication required");
                });

                const QString address = socket->peerAddress().toString();
                qInfo() << "WebSocket client connected:" << address;

                connect(socket, &QWebSocket::textMessageReceived, this,
                    [this, socket](const QString& message) { onWsMessage(socket, message); });

                connect(socket, &QWebSocket::disconnected, this, [this, socket, address]() {
                    wsClients_.removeOne(socket);
                    authenticatedWsClients_.remove(socket);
                    socket->deleteLater();
                    qInfo() << "WebSocket client disconnected:" << address;
                });
            }
        });

        if (!wsServer_->listen(host, static_cast<quint16>(wsPortActual))) {
            qWarning() << "Failed to start WebSocket server on port" << wsPortActual << ":" << wsServer_->errorString();
            wsServer_.reset();
            if (httpServer_) {
                httpServer_->close();
                httpServer_.reset();
                httpPort_ = 0;
            }
            return false;
        }

        qInfo() << "WebSocket server listening on port" << wsServer_->serverPort();
        wsPort_ = wsServer_->serverPort();
    }

    running_ = true;
    startTimeMs_ = QDateTime::currentMSecsSinceEpoch();
    emit started();
    return true;
}

void ApiServer::stop()
{
    if (!running_) {
        return;
    }

    for (QWebSocket* client : wsClients_) {
        client->close();
    }
    wsClients_.clear();
    authenticatedWsClients_.clear();
    httpBuffers_.clear();

    if (wsServer_) {
        wsServer_->close();
        wsServer_.reset();
    }
    if (httpServer_) {
        httpServer_->close();
        httpServer_.reset();
    }

    running_ = false;
    qInfo() << "API server stopped";
}

void ApiServer::broadcastEvent(const QString& event, const QJsonValue& data)
{
    QJsonObject msg;
    msg["event"] = event;
    msg["data"] = data;

    const QString json = QString::fromUtf8(QJsonDocument(msg).toJson(QJsonDocument::Compact));

    for (QWebSocket* client : wsClients_) {
        if (client->isValid() && authenticatedWsClients_.contains(client)) {
            client->sendTextMessage(json);
        }
    }
}

// ----------------------------------------------------------------------------
// HTTP handling
// ----------------------------------------------------------------------------

void ApiServer::onHttpReadyRead(QTcpSocket* socket)
{
    QByteArray& buffer = httpBuffers_[socket];
    buffer.append(socket->readAll());

    // Wait until the full header block has arrived.
    const int headerEnd = buffer.indexOf("\r\n\r\n");
    if (headerEnd < 0) {
        return;
    }

    HttpRequest req = parseHttpHeader(buffer.left(headerEnd));

    // For requests with a body, wait until Content-Length bytes are buffered.
    const int bodyStart = headerEnd + 4;
    const int contentLength = req.headers.value(QStringLiteral("content-length")).toInt();
    if (contentLength > 0) {
        if (buffer.size() - bodyStart < contentLength) {
            return; // more body bytes still incoming
        }
        req.body = buffer.mid(bodyStart, contentLength);
    }

    // Request fully received; this connection is single-shot (Connection: close).
    httpBuffers_.remove(socket);

    // CORS preflight.
    if (req.method == "OPTIONS") {
        socket->write(buildHttpResponse(200, "OK", QByteArray()));
        socket->disconnectFromHost();
        return;
    }

    // Static UI and the minimal liveness probe are public; every data-bearing
    // HTTP endpoint requires the same token as WebSocket messages and events.
    const bool protectedPath = (req.path.startsWith("/api/") && !req.path.startsWith("/api/translations/")) ||
        req.path == "/metrics" || req.path == "/readyz" || req.path.startsWith("/download/");
    if (protectedPath && !isAuthorized(req.headers.value(QStringLiteral("authorization")).toUtf8())) {
        requestsByStatus_[401]++;
        socket->write(buildHttpResponse(401, "Unauthorized", "{\"error\":\"Authentication required\"}"));
        socket->disconnectFromHost();
        return;
    }

    // Health check endpoints (Kubernetes probes)
    if (req.path == "/healthz") {
        socket->write(handleHealthz());
        socket->disconnectFromHost();
        return;
    }
    if (req.path == "/readyz") {
        socket->write(handleReadyz());
        socket->disconnectFromHost();
        return;
    }

    // Prometheus metrics endpoint
    if (req.path == "/metrics") {
        socket->write(handleMetrics());
        socket->disconnectFromHost();
        return;
    }

    if (req.path.startsWith("/download/")) {
        socket->write(handleTorrentDownload(req.path));
        socket->disconnectFromHost();
        return;
    }

    // Translations endpoint: /api/translations/<lang>
    if (req.path.startsWith("/api/translations/")) {
        const QString lang = req.path.mid(QStringLiteral("/api/translations/").size());
        socket->write(handleTranslations(lang));
        socket->disconnectFromHost();
        return;
    }

    if (req.path == "/legacy" || req.path.startsWith("/legacy/")) {
        socket->write(handleStaticFile(req.path));
        socket->disconnectFromHost();
        return;
    }

    // Angular path routing: serve the application shell only for known UI
    // routes. API, health, metrics, downloads and missing assets retain their
    // normal response behavior.
    const QString routePath = req.path;
    const QString legacyHash = routePath.startsWith("/search/permalink/")
        ? routePath.mid(QStringLiteral("/search/permalink/").size())
        : routePath.startsWith("/search/") ? routePath.mid(QStringLiteral("/search/").size()) : QString();
    const bool legacyDetailRoute = infohash::isValid(legacyHash);
    const bool appRoute = routePath == "/search" || legacyDetailRoute || routePath == "/discover" || routePath == "/history" ||
        routePath == "/console" || routePath.startsWith("/console/") || routePath.startsWith("/torrent/");
    if (appRoute) {
        socket->write(handleStaticFile("/index.html"));
        socket->disconnectFromHost();
        return;
    }

    // Static file serving for webui
    if (req.path == "/" || req.path.startsWith("/css/") || req.path.startsWith("/js/") ||
        req.path.startsWith("/images/") || req.path.startsWith("/vendor/") ||
        req.path.endsWith(".html") || req.path.endsWith(".css") ||
        req.path.endsWith(".js") || req.path.endsWith(".json") || req.path.endsWith(".svg") ||
        req.path.endsWith(".ico") || req.path.endsWith(".png") || req.path.endsWith(".jpg") ||
        req.path.endsWith(".woff") || req.path.endsWith(".woff2")) {
        socket->write(handleStaticFile(req.path));
        socket->disconnectFromHost();
        return;
    }

    if (!req.path.startsWith("/api/")) {
        requestsByStatus_[404]++;
        socket->write(buildHttpResponse(404, "Not Found", "{\"error\":\"Not Found\"}"));
        socket->disconnectFromHost();
        return;
    }

    const QString method = req.path.mid(5); // strip "/api/"
    if ((method == QLatin1String("search.remote") || method == QLatin1String("search.cancel") ||
            method == QLatin1String("network.start") || method == QLatin1String("network.stop") ||
            method == QLatin1String("crawler.start") || method == QLatin1String("crawler.stop"))
        && req.method != "POST") {
        requestsByStatus_[405]++;
        socket->write(buildHttpResponse(405, "Method Not Allowed", "{\"error\":\"POST required\"}"));
        socket->disconnectFromHost();
        return;
    }

    // Build params from the query string...
    QJsonObject params;
    const auto items = req.query.queryItems();
    for (const auto& item : items) {
        params[item.first] = coerceQueryValue(item.second);
    }
    // ...then overlay a JSON object body (POST), which takes precedence.
    if (!req.body.isEmpty()) {
        const QJsonDocument bodyDoc = QJsonDocument::fromJson(req.body);
        if (bodyDoc.isObject()) {
            const QJsonObject bodyObj = bodyDoc.object();
            for (auto it = bodyObj.begin(); it != bodyObj.end(); ++it) {
                params[it.key()] = it.value();
            }
        }
    }

    dispatchHttp(socket, method, params);
}

void ApiServer::dispatchHttp(QTcpSocket* socket, const QString& method, const QJsonObject& params)
{
    const QString requestId = QString::number(++nextRequestId_, 16);

    httpRequestsTotal_++;
    requestsByMethod_[method]++;
    requestStartTimes_[requestId] = QDateTime::currentMSecsSinceEpoch();

    QString accessError;
    if (!validateFileAccess(method, params, &accessError)) {
        httpRequestsError_++;
        requestsByStatus_[403]++;
        requestStartTimes_.remove(requestId);
        const QJsonObject env = resultToEnvelope(Result::failure(accessError), requestId);
        socket->write(buildHttpResponse(403, "Forbidden", QJsonDocument(env).toJson()));
        socket->disconnectFromHost();
        return;
    }

    if (!router_) {
        httpRequestsError_++;
        requestsByStatus_[400]++;
        requestStartTimes_.remove(requestId);
        const QJsonObject env = resultToEnvelope(Result::failure("API router unavailable"), requestId);
        socket->write(buildHttpResponse(400, "Bad Request", QJsonDocument(env).toJson()));
        socket->disconnectFromHost();
        return;
    }

    QPointer<QTcpSocket> guardedSocket(socket);
    router_->call(method, params, [this, guardedSocket, requestId, method](const Result& result) {
        if (!guardedSocket || !guardedSocket->isOpen()) {
            requestStartTimes_.remove(requestId);
            return;
        }

        if (result.ok()) {
            httpRequestsSuccess_++;
        } else {
            httpRequestsError_++;
        }
        int statusCode = result.ok() ? 200 : 400;
        requestsByStatus_[statusCode]++;

        qint64 now = QDateTime::currentMSecsSinceEpoch();
        auto it = requestStartTimes_.find(requestId);
        if (it != requestStartTimes_.end()) {
            double durationMs = now - it.value();
            double durationSec = durationMs / 1000.0;
            durationSumMs_ += durationMs;
            durationCount_++;
            for (int i = 0; i < DURATION_BUCKETS_COUNT; i++) {
                if (durationSec <= DURATION_BUCKETS[i]) {
                    durationBucketCounts[i]++;
                }
            }
            if (method.startsWith("search.")) {
                searchQueriesTotal_++;
                searchDurationSumMs_ += durationMs;
                for (int i = 0; i < DURATION_BUCKETS_COUNT; i++) {
                    if (durationSec <= DURATION_BUCKETS[i]) {
                        searchDurationBuckets_[i]++;
                    }
                }
            }
            requestStartTimes_.remove(requestId);
        }

        const QJsonObject env = resultToEnvelope(result, requestId);
        guardedSocket->write(buildHttpResponse(statusCode, result.ok() ? "OK" : "Bad Request", QJsonDocument(env).toJson()));
        guardedSocket->disconnectFromHost();
    });
}

// ----------------------------------------------------------------------------
// WebSocket handling
// ----------------------------------------------------------------------------

void ApiServer::onWsMessage(QWebSocket* socket, const QString& message)
{
    wsMessagesTotal_++;

    const QJsonDocument doc = QJsonDocument::fromJson(message.toUtf8());
    if (!doc.isObject()) {
        socket->sendTextMessage("{\"success\":false,\"error\":\"Invalid JSON\"}");
        return;
    }

    const QJsonObject obj = doc.object();
    const QString method = obj["method"].toString();
    const QJsonObject params = obj["params"].toObject();
    const QString requestId = obj["id"].toString();

    if (!authenticatedWsClients_.contains(socket)) {
        if (method == QLatin1String("auth") && isAuthorized("Bearer " + params["token"].toString().toUtf8())) {
            authenticatedWsClients_.insert(socket);
            socket->sendTextMessage("{\"success\":true,\"data\":{\"authenticated\":true}}");
        } else {
            socket->close(QWebSocketProtocol::CloseCodePolicyViolated, "Authentication required");
        }
        return;
    }

    if (method.isEmpty()) {
        socket->sendTextMessage("{\"success\":false,\"error\":\"Missing method\"}");
        return;
    }

    QString accessError;
    if (!validateFileAccess(method, params, &accessError)) {
        const QJsonObject env = resultToEnvelope(Result::failure(accessError), requestId);
        socket->sendTextMessage(QString::fromUtf8(QJsonDocument(env).toJson(QJsonDocument::Compact)));
        return;
    }

    if (!router_) {
        const QJsonObject env = resultToEnvelope(Result::failure("API router unavailable"), requestId);
        socket->sendTextMessage(QString::fromUtf8(QJsonDocument(env).toJson(QJsonDocument::Compact)));
        return;
    }

    QPointer<QWebSocket> guardedSocket(socket);
    router_->call(method, params, [guardedSocket, requestId](const Result& result) {
        if (!guardedSocket || !guardedSocket->isValid()) {
            return;
        }
        const QJsonObject env = resultToEnvelope(result, requestId);
        guardedSocket->sendTextMessage(QString::fromUtf8(QJsonDocument(env).toJson(QJsonDocument::Compact)));
    });
}

// ----------------------------------------------------------------------------
// Webui & Health/Metrics
// ----------------------------------------------------------------------------

void ApiServer::setWebuiDir(const QString& dir)
{
    webuiDir_ = dir;
}

void ApiServer::setLegacyWebuiDir(const QString& dir)
{
    legacyWebuiDir_ = dir;
}

void ApiServer::setApplication(app::Application* app)
{
    app_ = app;
}

QByteArray ApiServer::handleHealthz() const
{
    QJsonObject status;
    status["status"] = "ok";
    status["timestamp"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    QJsonDocument doc(status);
    return buildHttpResponse(200, "OK", doc.toJson(), "application/json");
}

QByteArray ApiServer::handleReadyz() const
{
    QJsonObject status;
    bool ready = true;

    status["api"] = router_ ? "ready" : "not_initialized";
    if (!router_) ready = false;

    status["http"] = httpServer_ && httpServer_->isListening() ? "listening" : "not_listening";
    if (!httpServer_ || !httpServer_->isListening()) ready = false;

    status["websocket"] = wsServer_ && wsServer_->isListening() ? "listening" : "not_listening";

    status["status"] = ready ? "ready" : "not_ready";
    status["timestamp"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);

    QJsonDocument doc(status);
    return buildHttpResponse(ready ? 200 : 503, ready ? "OK" : "Service Unavailable", doc.toJson(),
        "application/json");
}

QByteArray ApiServer::handleMetrics() const
{
    QStringList metrics;
    qint64 timestamp = QDateTime::currentMSecsSinceEpoch();

    // Server uptime
    qint64 uptimeMs = startTimeMs_ > 0 ? (timestamp - startTimeMs_) : 0;
    metrics << "# HELP rats_server_uptime_seconds Server uptime in seconds";
    metrics << "# TYPE rats_server_uptime_seconds gauge";
    metrics << QString("rats_server_uptime_seconds %1 %2").arg(uptimeMs / 1000).arg(timestamp);

    if (startTimeMs_ > 0) {
        metrics << "# HELP rats_start_time_seconds Timestamp when the server started (Unix epoch seconds)";
        metrics << "# TYPE rats_start_time_seconds gauge";
        metrics << QString("rats_start_time_seconds %1 %2").arg(startTimeMs_ / 1000.0, 0, 'f', 3).arg(timestamp);
    }

    metrics << "# HELP rats_websocket_connections Current number of WebSocket connections";
    metrics << "# TYPE rats_websocket_connections gauge";
    metrics << QString("rats_websocket_connections %1 %2").arg(wsClients_.size()).arg(timestamp);

    metrics << "# HELP rats_http_server_running Whether HTTP server is running (1=yes, 0=no)";
    metrics << "# TYPE rats_http_server_running gauge";
    metrics << QString("rats_http_server_running %1 %2").arg(running_ ? 1 : 0).arg(timestamp);

    metrics << "# HELP rats_ws_server_running Whether WebSocket server is running (1=yes, 0=no)";
    metrics << "# TYPE rats_ws_server_running gauge";
    metrics << QString("rats_ws_server_running %1 %2")
                   .arg(wsServer_ && wsServer_->isListening() ? 1 : 0)
                   .arg(timestamp);

    metrics << "# HELP rats_http_port HTTP server port";
    metrics << "# TYPE rats_http_port gauge";
    metrics << QString("rats_http_port %1 %2").arg(httpPort_).arg(timestamp);

    metrics << "# HELP rats_ws_port WebSocket server port";
    metrics << "# TYPE rats_ws_port gauge";
    metrics << QString("rats_ws_port %1 %2").arg(wsPort_).arg(timestamp);

    metrics << "# HELP rats_http_requests_total Total HTTP requests";
    metrics << "# TYPE rats_http_requests_total counter";
    metrics << QString("rats_http_requests_total %1 %2").arg(httpRequestsTotal_).arg(timestamp);

    metrics << "# HELP rats_http_requests_success_total Successful HTTP requests";
    metrics << "# TYPE rats_http_requests_success_total counter";
    metrics << QString("rats_http_requests_success_total %1 %2").arg(httpRequestsSuccess_).arg(timestamp);

    metrics << "# HELP rats_http_requests_error_total Failed HTTP requests";
    metrics << "# TYPE rats_http_requests_error_total counter";
    metrics << QString("rats_http_requests_error_total %1 %2").arg(httpRequestsError_).arg(timestamp);

    metrics << "# HELP rats_ws_messages_total Total WebSocket messages received";
    metrics << "# TYPE rats_ws_messages_total counter";
    metrics << QString("rats_ws_messages_total %1 %2").arg(wsMessagesTotal_).arg(timestamp);

    // Per-method breakdown
    if (!requestsByMethod_.isEmpty()) {
        metrics << "# HELP rats_http_requests_by_method_total Total HTTP requests by method";
        metrics << "# TYPE rats_http_requests_by_method_total counter";
        for (auto it = requestsByMethod_.constBegin(); it != requestsByMethod_.constEnd(); ++it) {
            metrics << QString("rats_http_requests_by_method_total{method=\"%1\"} %2 %3")
                           .arg(it.key())
                           .arg(it.value())
                           .arg(timestamp);
        }
    }

    // Per-status breakdown
    if (!requestsByStatus_.isEmpty()) {
        metrics << "# HELP rats_http_requests_by_status_total Total HTTP requests by status code";
        metrics << "# TYPE rats_http_requests_by_status_total counter";
        for (auto it = requestsByStatus_.constBegin(); it != requestsByStatus_.constEnd(); ++it) {
            metrics << QString("rats_http_requests_by_status_total{status=\"%1\"} %2 %3")
                           .arg(it.key())
                           .arg(it.value())
                           .arg(timestamp);
        }
    }

    // Request duration histogram
    if (durationCount_ > 0) {
        metrics << "# HELP rats_http_request_duration_seconds HTTP request duration in seconds";
        metrics << "# TYPE rats_http_request_duration_seconds histogram";
        for (int i = 0; i < DURATION_BUCKETS_COUNT; i++) {
            QString le = (i == DURATION_BUCKETS_COUNT - 1) ? "+Inf"
                                                            : QString::number(DURATION_BUCKETS[i], 'g', 4);
            metrics << QString("rats_http_request_duration_seconds_bucket{le=\"%1\"} %2 %3")
                           .arg(le)
                           .arg(durationBucketCounts[i])
                           .arg(timestamp);
        }
        metrics << QString("rats_http_request_duration_seconds_sum %1 %2")
                       .arg(durationSumMs_ / 1000.0, 0, 'f', 6)
                       .arg(timestamp);
        metrics << QString("rats_http_request_duration_seconds_count %1 %2")
                       .arg(durationCount_)
                       .arg(timestamp);
    }

    // Search-specific metrics
    if (searchQueriesTotal_ > 0) {
        metrics << "# HELP rats_search_queries_total Total search queries executed";
        metrics << "# TYPE rats_search_queries_total counter";
        metrics << QString("rats_search_queries_total %1 %2").arg(searchQueriesTotal_).arg(timestamp);

        metrics << "# HELP rats_search_duration_seconds Search query duration in seconds";
        metrics << "# TYPE rats_search_duration_seconds histogram";
        for (int i = 0; i < DURATION_BUCKETS_COUNT; i++) {
            QString le = (i == DURATION_BUCKETS_COUNT - 1) ? "+Inf"
                                                            : QString::number(DURATION_BUCKETS[i], 'g', 4);
            metrics << QString("rats_search_duration_seconds_bucket{le=\"%1\"} %2 %3")
                           .arg(le)
                           .arg(searchDurationBuckets_[i])
                           .arg(timestamp);
        }
        metrics << QString("rats_search_duration_seconds_sum %1 %2")
                       .arg(searchDurationSumMs_ / 1000.0, 0, 'f', 6)
                       .arg(timestamp);
        metrics << QString("rats_search_duration_seconds_count %1 %2")
                       .arg(searchQueriesTotal_)
                       .arg(timestamp);
    }

    // =========================================================================
    // P2P Network Metrics
    // =========================================================================
    if (app_) {
        auto* transport = app_->transport();
        if (transport) {
            metrics << "# HELP rats_p2p_peer_count Current number of connected P2P peers";
            metrics << "# TYPE rats_p2p_peer_count gauge";
            metrics << QString("rats_p2p_peer_count %1 %2").arg(transport->peerCount()).arg(timestamp);

            metrics << "# HELP rats_p2p_dht_node_count Number of DHT nodes in routing table";
            metrics << "# TYPE rats_p2p_dht_node_count gauge";
            metrics << QString("rats_p2p_dht_node_count %1 %2").arg(transport->dhtNodeCount()).arg(timestamp);

            metrics << "# HELP rats_p2p_dht_running Whether DHT is running (1=yes, 0=no)";
            metrics << "# TYPE rats_p2p_dht_running gauge";
            metrics << QString("rats_p2p_dht_running %1 %2").arg(transport->isDhtRunning() ? 1 : 0).arg(timestamp);

            metrics << "# HELP rats_p2p_bittorrent_enabled Whether BitTorrent is enabled (1=yes, 0=no)";
            metrics << "# TYPE rats_p2p_bittorrent_enabled gauge";
            metrics << QString("rats_p2p_bittorrent_enabled %1 %2")
                           .arg(transport->isBitTorrentEnabled() ? 1 : 0)
                           .arg(timestamp);

            metrics << "# HELP rats_p2p_running Whether P2P network is running (1=yes, 0=no)";
            metrics << "# TYPE rats_p2p_running gauge";
            metrics << QString("rats_p2p_running %1 %2").arg(transport->isRunning() ? 1 : 0).arg(timestamp);
        }
    }

    // =========================================================================
    // Torrent Database Metrics
    // =========================================================================
    if (app_) {
        auto* torrents = app_->torrents();
        if (torrents) {
            auto stats = torrents->statistics();

            metrics << "# HELP rats_db_torrents_total Total torrents in database";
            metrics << "# TYPE rats_db_torrents_total gauge";
            metrics << QString("rats_db_torrents_total %1 %2").arg(stats.torrents).arg(timestamp);

            metrics << "# HELP rats_db_files_total Total files in database";
            metrics << "# TYPE rats_db_files_total gauge";
            metrics << QString("rats_db_files_total %1 %2").arg(stats.files).arg(timestamp);

            metrics << "# HELP rats_db_total_content_size_bytes Total content size of all torrents in bytes";
            metrics << "# TYPE rats_db_total_content_size_bytes gauge";
            metrics << QString("rats_db_total_content_size_bytes %1 %2").arg(stats.totalSize).arg(timestamp);

            metrics << "# HELP rats_db_ready Whether database is ready (1=yes, 0=no)";
            metrics << "# TYPE rats_db_ready gauge";
            metrics << QString("rats_db_ready 1 %1").arg(timestamp);
        } else {
            metrics << "# HELP rats_db_ready Whether database is ready (1=yes, 0=no)";
            metrics << "# TYPE rats_db_ready gauge";
            metrics << QString("rats_db_ready 0 %1").arg(timestamp);
        }
    }

    // Database index size on disk (update at most once per 60s)
    if (app_ && timestamp - dbIndexSizeLastUpdate_ > 60000) {
        const QString dataDir = app_->options().dataDirectory;
        const QString dbDir = dataDir + "/database";
        QDir dir(dbDir);
        if (dir.exists()) {
            qint64 totalSize = 0;
            for (const auto& entry : dir.entryInfoList(QDir::Files))
                totalSize += entry.size();
            dbIndexSizeBytes_ = totalSize;
        }
        dbIndexSizeLastUpdate_ = timestamp;
    }
    metrics << "# HELP rats_db_index_size_bytes Total size of database index files on disk";
    metrics << "# TYPE rats_db_index_size_bytes gauge";
    metrics << QString("rats_db_index_size_bytes %1 %2").arg(dbIndexSizeBytes_).arg(timestamp);

    // =========================================================================
    // Crawler (DHT Spider) Metrics
    // =========================================================================
    if (app_) {
        auto* crawler = app_->crawler();
        if (crawler) {
            metrics << "# HELP rats_spider_running Whether DHT spider is running (1=yes, 0=no)";
            metrics << "# TYPE rats_spider_running gauge";
            metrics << QString("rats_spider_running %1 %2").arg(crawler->isRunning() ? 1 : 0).arg(timestamp);

            metrics << "# HELP rats_spider_indexed_total Total torrents indexed by spider";
            metrics << "# TYPE rats_spider_indexed_total gauge";
            metrics << QString("rats_spider_indexed_total %1 %2").arg(crawler->discoveredCount()).arg(timestamp);

            metrics << "# HELP rats_spider_pending_fetches Current pending metadata fetches";
            metrics << "# TYPE rats_spider_pending_fetches gauge";
            metrics << QString("rats_spider_pending_fetches %1 %2").arg(crawler->activeFetches()).arg(timestamp);

            metrics << "# HELP rats_spider_dht_nodes DHT routing table size";
            metrics << "# TYPE rats_spider_dht_nodes gauge";
            metrics << QString("rats_spider_dht_nodes %1 %2")
                           .arg(app_->transport() ? app_->transport()->dhtNodeCount() : 0)
                           .arg(timestamp);

            metrics << "# HELP rats_spider_visited_nodes Total visited DHT nodes";
            metrics << "# TYPE rats_spider_visited_nodes gauge";
            metrics << QString("rats_spider_visited_nodes %1 %2").arg(crawler->visitedNodesCount()).arg(timestamp);

            metrics << "# TYPE rats_spider_sample_requests_total counter";
            metrics << QString("rats_spider_sample_requests_total %1 %2").arg(crawler->sampleRequestCount()).arg(timestamp);
            metrics << "# TYPE rats_spider_sample_responses_total counter";
            metrics << QString("rats_spider_sample_responses_total %1 %2").arg(crawler->sampleResponseCount()).arg(timestamp);
            metrics << "# TYPE rats_spider_sample_hashes_total counter";
            metrics << QString("rats_spider_sample_hashes_total %1 %2").arg(crawler->sampleHashCount()).arg(timestamp);
            metrics << "# TYPE rats_spider_hashes_enqueued_total counter";
            metrics << QString("rats_spider_hashes_enqueued_total %1 %2").arg(crawler->hashEnqueuedCount()).arg(timestamp);

            metrics << "# HELP rats_spider_fetch_success_total Total successful metadata fetches";
            metrics << "# TYPE rats_spider_fetch_success_total counter";
            metrics << QString("rats_spider_fetch_success_total %1 %2").arg(crawler->fetchSuccessCount()).arg(timestamp);

            metrics << "# HELP rats_spider_fetch_errors_total Total failed metadata fetches";
            metrics << "# TYPE rats_spider_fetch_errors_total counter";
            metrics << QString("rats_spider_fetch_errors_total %1 %2").arg(crawler->fetchErrorCount()).arg(timestamp);
        }
    }

    // =========================================================================
    // Download Service Metrics
    // =========================================================================
    if (app_) {
        auto* downloads = app_->downloads();
        if (downloads) {
            auto all = downloads->allDownloads();
            int activeCount = 0;
            int pausedCount = 0;
            qint64 totalDownloaded = 0;
            double totalSpeed = 0.0;

            for (const auto& d : all) {
                if (!d.completed) {
                    activeCount++;
                    if (d.paused) pausedCount++;
                    totalDownloaded += d.downloadedBytes;
                    totalSpeed += d.downloadSpeed;
                }
            }

            metrics << "# HELP rats_downloads_active Number of active downloads";
            metrics << "# TYPE rats_downloads_active gauge";
            metrics << QString("rats_downloads_active %1 %2").arg(activeCount).arg(timestamp);

            metrics << "# HELP rats_downloads_paused Number of paused downloads";
            metrics << "# TYPE rats_downloads_paused gauge";
            metrics << QString("rats_downloads_paused %1 %2").arg(pausedCount).arg(timestamp);

            metrics << "# HELP rats_downloads_total_bytes_downloaded Total bytes downloaded across all torrents";
            metrics << "# TYPE rats_downloads_total_bytes_downloaded counter";
            metrics << QString("rats_downloads_total_bytes_downloaded %1 %2").arg(totalDownloaded).arg(timestamp);

            metrics << "# HELP rats_downloads_speed_bytes_per_second Current total download speed in bytes/second";
            metrics << "# TYPE rats_downloads_speed_bytes_per_second gauge";
            metrics << QString("rats_downloads_speed_bytes_per_second %1 %2")
                           .arg(static_cast<qint64>(totalSpeed))
                           .arg(timestamp);

            metrics << "# HELP rats_downloads_total Total number of downloads (active + completed)";
            metrics << "# TYPE rats_downloads_total gauge";
            metrics << QString("rats_downloads_total %1 %2").arg(all.size()).arg(timestamp);
        }
    }

    QString body = metrics.join("\n") + "\n";
    return buildHttpResponse(200, "OK", body.toUtf8(), "text/plain; version=0.0.4; charset=utf-8");
}

QByteArray ApiServer::handleStaticFile(const QString& path) const
{
    QString requestPath = path;
    const bool legacy = path == "/legacy" || path.startsWith("/legacy/") || path.startsWith("/css/") ||
        path.startsWith("/js/") || path.startsWith("/vendor/");
    QString dir = legacy ? legacyWebuiDir_ : webuiDir_;
    if (legacy) {
        requestPath = path == "/legacy" ? QStringLiteral("/") :
            path.startsWith("/legacy/") ? path.mid(QStringLiteral("/legacy").size()) : path;
    }
    if (dir.isEmpty()) {
        dir = legacy ? QDir::currentPath() + "/webui" : QDir::currentPath() + "/webui";
    }
    dir = QDir(dir).absolutePath();

    QString filePath;
    if (requestPath == "/" || requestPath.isEmpty()) {
        filePath = dir + "/index.html";
    } else {
        filePath = dir + requestPath;
    }

    // Security: prevent directory traversal
    QString cleanPath = QDir::cleanPath(filePath);
    QString cleanBase = QDir(dir).absolutePath();
    if (!cleanPath.startsWith(cleanBase + "/")) {
        return buildHttpResponse(403, "Forbidden", "{\"error\":\"Access denied\"}");
    }
    filePath = cleanPath;

    QFile file(filePath);
    if (!file.exists()) {
        return buildHttpResponse(404, "Not Found", "{\"error\":\"File not found\"}");
    }
    if (!QFileInfo(filePath).isFile())
        return buildHttpResponse(403, "Forbidden", "{\"error\":\"Access denied\"}");
    const QString canonicalBase = QFileInfo(cleanBase).canonicalFilePath();
    const QString canonicalFile = QFileInfo(filePath).canonicalFilePath();
    if (canonicalBase.isEmpty() || !canonicalFile.startsWith(canonicalBase + "/")) {
        return buildHttpResponse(403, "Forbidden", "{\"error\":\"Access denied\"}");
    }
    if (!file.open(QIODevice::ReadOnly)) {
        return buildHttpResponse(500, "Internal Server Error", "{\"error\":\"Cannot read file\"}");
    }

    QByteArray content = file.readAll();
    file.close();

    QString contentType = "text/plain";
    if (filePath.endsWith(".html"))
        contentType = "text/html";
    else if (filePath.endsWith(".css"))
        contentType = "text/css";
    else if (filePath.endsWith(".js"))
        contentType = "application/javascript";
    else if (filePath.endsWith(".json"))
        contentType = "application/json";
    else if (filePath.endsWith(".png"))
        contentType = "image/png";
    else if (filePath.endsWith(".jpg") || filePath.endsWith(".jpeg"))
        contentType = "image/jpeg";
    else if (filePath.endsWith(".svg"))
        contentType = "image/svg+xml";
    else if (filePath.endsWith(".ico"))
        contentType = "image/x-icon";
    else if (filePath.endsWith(".woff2"))
        contentType = "font/woff2";
    else if (filePath.endsWith(".woff"))
        contentType = "font/woff";

    return buildHttpResponse(200, "OK", content, contentType);
}

QByteArray ApiServer::handleTorrentDownload(const QString& path) const
{
    if (!app_ || !path.endsWith(QLatin1String(".torrent")))
        return buildHttpResponse(404, "Not Found", "{\"error\":\"Not found\"}");

    const QString hash = infohash::normalize(path.mid(10, path.size() - 18));
    if (!infohash::isValid(hash))
        return buildHttpResponse(404, "Not Found", "{\"error\":\"Not found\"}");

    const QString filePath = QDir(app_->options().dataDirectory)
                                 .absoluteFilePath(QStringLiteral("torrents/") + hash.toLower() + QStringLiteral(".torrent"));
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly))
        return buildHttpResponse(404, "Not Found", "{\"error\":\"Torrent not ready\"}");
    return buildHttpResponse(200, "OK", file.readAll(), "application/x-bittorrent");
}

QByteArray ApiServer::handleTranslations(const QString& lang) const
{
    static const QSet<QString> kValidLangs = {
        QStringLiteral("ru"), QStringLiteral("de"), QStringLiteral("es"),
        QStringLiteral("fr"), QStringLiteral("ja"), QStringLiteral("zh"),
        QStringLiteral("ko")
    };
    const QString actualLang = kValidLangs.contains(lang) ? lang : QStringLiteral("ru");
    QFile file(QStringLiteral(":/ts/%1").arg(actualLang));
    if (!file.open(QIODevice::ReadOnly))
        return buildHttpResponse(404, "Not Found", QByteArray());
    return buildHttpResponse(200, "OK", file.readAll(), QStringLiteral("application/xml"));
}

} // namespace rats::rest
