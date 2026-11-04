#include "services/peer_registry.h"

#include "net/p2p_transport.h"

#include <QDateTime>
#include <QDebug>

namespace rats::service {

PeerRegistry::PeerRegistry(net::P2PTransport* transport, QString clientVersion, QObject* parent)
    : QObject(parent), transport_(transport), clientVersion_(std::move(clientVersion))
{
    connect(transport_, &net::P2PTransport::peerConnected, this, &PeerRegistry::onPeerConnected);
    connect(transport_, &net::P2PTransport::peerDisconnected, this, &PeerRegistry::onPeerDisconnected);
    transport_->registerHandler(QStringLiteral("client_info"),
        [this](const QString& peerId, const QJsonObject& data) { onClientInfo(peerId, data); });
}

void PeerRegistry::updateOurStats(qint64 torrents, qint64 files, qint64 totalSize)
{
    ourTorrents_ = torrents;
    ourFiles_ = files;
    ourTotalSize_ = totalSize;
}

void PeerRegistry::setDatabaseSharing(bool enabled)
{
    if (ourDatabaseSharing_ == enabled)
        return;
    ourDatabaseSharing_ = enabled;

    // Peers cached our handshake; without a re-announce they would keep offering
    // (or keep hiding) us for the rest of the connection.
    if (transport_ && transport_->isRunning())
        transport_->broadcastMessage(QStringLiteral("client_info"), ourStats().toJson());
}

void PeerRegistry::setIndexSharingEnabled(bool enabled)
{
    if (ourIndexSharing_ == enabled)
        return;
    ourIndexSharing_ = enabled;
    // Index counts are still metadata about the user's collection. Refresh
    // connected peers when the permission changes, including when it closes.
    if (transport_ && transport_->isRunning())
        transport_->broadcastMessage(QStringLiteral("client_info"), ourStats().toJson());
}

domain::PeerStats PeerRegistry::ourStats() const
{
    domain::PeerStats s;
    s.clientVersion = clientVersion_;
    s.torrents = ourIndexSharing_ ? ourTorrents_ : 0;
    s.files = ourIndexSharing_ ? ourFiles_ : 0;
    s.totalSize = ourIndexSharing_ ? ourTotalSize_ : 0;
    s.peersConnected = transport_->peerCount();
    s.databaseSharing = ourDatabaseSharing_;
    return s;
}

void PeerRegistry::onPeerConnected(const QString& peerId)
{
    // Introduce ourselves; the peer replies with its own client_info.
    transport_->sendMessage(peerId, QStringLiteral("client_info"), ourStats().toJson());
}

void PeerRegistry::onPeerDisconnected(const QString& peerId)
{
    QMutexLocker locker(&mutex_);
    peers_.remove(peerId);
}

void PeerRegistry::onClientInfo(const QString& peerId, const QJsonObject& data)
{
    domain::PeerStats stats = domain::PeerStats::fromJson(data);
    stats.connectedAt = QDateTime::currentMSecsSinceEpoch();

    {
        QMutexLocker locker(&mutex_);
        // A peer re-announces when it toggles database sharing, so this is not
        // necessarily the first client_info of the connection: keep the original
        // connect time rather than resetting it on every update.
        auto existing = peers_.constFind(peerId);
        if (existing != peers_.constEnd() && existing->connectedAt > 0)
            stats.connectedAt = existing->connectedAt;
        peers_[peerId] = stats;
    }

    qInfo() << "[PeerRegistry] peer" << peerId.left(8) << "v" << stats.clientVersion << "torrents:" << stats.torrents
            << "files:" << stats.files;
    emit peerStatsReceived(peerId, stats);
}

QHash<QString, domain::PeerStats> PeerRegistry::connectedPeers() const
{
    QMutexLocker locker(&mutex_);
    return peers_;
}

QHash<QString, domain::PeerStats> PeerRegistry::databaseSharingPeers() const
{
    QMutexLocker locker(&mutex_);
    QHash<QString, domain::PeerStats> result;
    for (auto it = peers_.constBegin(); it != peers_.constEnd(); ++it) {
        if (it.value().databaseSharing)
            result.insert(it.key(), it.value());
    }
    return result;
}

qint64 PeerRegistry::remoteTorrentsCount() const
{
    QMutexLocker locker(&mutex_);
    qint64 total = 0;
    for (const domain::PeerStats& s : peers_)
        total += s.torrents;
    return total;
}

} // namespace rats::service
