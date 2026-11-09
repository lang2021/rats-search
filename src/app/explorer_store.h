#pragma once

#include <QObject>
#include <QMutex>
#include <QString>
#include <QVector>
#include <cstdint>
#include <functional>
#include <optional>
class QSqlDatabase;

namespace rats::app {

class ExplorerStore : public QObject {
    Q_OBJECT
public:
    struct PeerHint { QString ip; std::uint16_t port = 0; bool handshake = false; };
    struct Counts { int peers = 0; int verifiedPeers = 0; int contacts = 0; int samples = 0; };
    explicit ExplorerStore(QString dataDirectory, QObject* parent = nullptr);
    bool ready() const;
    bool recordPeer(const QString& hash, const QString& ip, std::uint16_t port, bool handshake);
    bool recordContact(const QString& hash, const QString& ip, std::uint16_t port);
    bool recordSample(const QString& targetHash, const QString& sampleHash);
    QVector<PeerHint> peers(const QString& hash, int limit = 16);
    QVector<PeerHint> contacts(const QString& hash, int limit = 8);
    std::optional<Counts> counts(const QString& hash);

private:
    bool withDatabase(const std::function<bool(QSqlDatabase&)>& operation) const;
    QString path_;
    mutable QMutex mutex_;
};

} // namespace rats::app
