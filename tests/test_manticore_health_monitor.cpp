#include "data/manticore.h"
#include "rest/api_server.h"

#include <QElapsedTimer>
#include <QEventLoop>
#include <QHash>
#include <QHostAddress>
#include <QTemporaryDir>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QtTest>

namespace {

QByteArray mysqlPacket(quint8 sequence, const QByteArray& payload)
{
    const auto size = static_cast<quint32>(payload.size());
    QByteArray packet;
    packet.append(static_cast<char>(size & 0xff));
    packet.append(static_cast<char>((size >> 8) & 0xff));
    packet.append(static_cast<char>((size >> 16) & 0xff));
    packet.append(static_cast<char>(sequence));
    packet.append(payload);
    return packet;
}

QByteArray mysqlGreeting()
{
    constexpr quint32 kClientLongPassword = 0x00000001;
    constexpr quint32 kClientProtocol41 = 0x00000200;
    constexpr quint32 kClientSecureConnection = 0x00008000;
    constexpr quint32 kClientPluginAuth = 0x00080000;
    constexpr quint32 kCapabilities
        = kClientLongPassword | kClientProtocol41 | kClientSecureConnection | kClientPluginAuth;

    QByteArray payload;
    payload.append(static_cast<char>(10));
    payload.append("8.0.36", 6);
    payload.append('\0');
    payload.append("\x01\x00\x00\x00", 4); // connection id
    payload.append("12345678", 8);
    payload.append('\0');
    payload.append(static_cast<char>(kCapabilities & 0xff));
    payload.append(static_cast<char>((kCapabilities >> 8) & 0xff));
    payload.append(static_cast<char>(45)); // utf8mb4_general_ci
    payload.append("\x02\x00", 2); // server status
    payload.append(static_cast<char>((kCapabilities >> 16) & 0xff));
    payload.append(static_cast<char>((kCapabilities >> 24) & 0xff));
    payload.append(static_cast<char>(21));
    payload.append(QByteArray(10, '\0'));
    payload.append("abcdefghijkl", 12);
    payload.append('\0');
    payload.append("mysql_native_password", 21);
    payload.append('\0');
    return payload;
}

bool takeMysqlPacket(QByteArray& buffer, QByteArray* payload, quint8* sequence)
{
    if (buffer.size() < 4)
        return false;
    const auto size = static_cast<quint32>(static_cast<quint8>(buffer[0]))
        | (static_cast<quint32>(static_cast<quint8>(buffer[1])) << 8)
        | (static_cast<quint32>(static_cast<quint8>(buffer[2])) << 16);
    if (buffer.size() < static_cast<qsizetype>(size + 4))
        return false;
    *sequence = static_cast<quint8>(buffer[3]);
    *payload = buffer.mid(4, static_cast<qsizetype>(size));
    buffer.remove(0, static_cast<qsizetype>(size + 4));
    return true;
}

} // namespace

namespace rats::data {

class TestManticoreHealthMonitor : public QObject {
    Q_OBJECT

private slots:
    void apiRemainsResponsiveWhenManticoreDoesNotAnswer();
};

void TestManticoreHealthMonitor::apiRemainsResponsiveWhenManticoreDoesNotAnswer()
{
    QTemporaryDir dataDir;
    QVERIFY(dataDir.isValid());

    // A local fake searchd completes the MySQL handshake and acknowledges the
    // driver setup query, then deliberately withholds its first SQL response.
    // The driver currently uses COM_STMT_PREPARE during open, so stall that
    // query to exercise a real MySQL protocol read timeout.
    QTcpServer stalledSearchd;
    QVERIFY(stalledSearchd.listen(QHostAddress::LocalHost, 0));
    int acceptedConnections = 0;
    int stalledQueries = 0;
    struct ClientState { QByteArray buffer; int phase = 0; };
    QHash<QTcpSocket*, ClientState> clients;
    connect(&stalledSearchd, &QTcpServer::newConnection, &stalledSearchd, [&] {
        while (stalledSearchd.hasPendingConnections()) {
            QTcpSocket* socket = stalledSearchd.nextPendingConnection();
            ++acceptedConnections;
            clients.insert(socket, ClientState {});
            socket->write(mysqlPacket(0, mysqlGreeting()));
            connect(socket, &QTcpSocket::readyRead, &stalledSearchd, [&, socket] {
                auto it = clients.find(socket);
                if (it == clients.end())
                    return;
                it->buffer.append(socket->readAll());
                QByteArray payload;
                quint8 sequence = 0;
                while (takeMysqlPacket(it->buffer, &payload, &sequence)) {
                    if (it->phase == 0) {
                        // Authenticate successfully; QMYSQL may issue a setup
                        // query before the status probe.
                        socket->write(mysqlPacket(2, QByteArray::fromHex("00000002000000")));
                        it->phase = 1;
                    } else if (!payload.isEmpty() && static_cast<quint8>(payload[0]) == 0x03) {
                        const QByteArray sql = payload.mid(1).trimmed().toUpper();
                        if (sql.startsWith("SET NAMES")) {
                        socket->write(mysqlPacket(static_cast<quint8>(sequence + 1),
                            QByteArray::fromHex("00000002000000")));
                        } else {
                            ++stalledQueries;
                            it->phase = 2;
                            return; // Intentionally leave the driver's SQL read waiting.
                        }
                    } else if (static_cast<quint8>(payload[0]) == 0x16) {
                        ++stalledQueries;
                        it->phase = 2;
                        return; // Intentionally stall COM_STMT_PREPARE.
                    }
                }
            });
            connect(socket, &QTcpSocket::disconnected, &stalledSearchd, [&, socket] {
                clients.remove(socket);
                socket->deleteLater();
            });
        }
    });

    QTcpServer portReservation;
    QVERIFY(portReservation.listen(QHostAddress::LocalHost, 0));
    const quint16 apiPort = portReservation.serverPort();
    portReservation.close();

    rats::rest::ApiServer api(nullptr);
    QVERIFY(api.start(apiPort, 0, QStringLiteral("127.0.0.1")));

    rats::data::Manticore manticore(dataDir.path());
    manticore.port_ = stalledSearchd.serverPort();
    manticore.status_ = rats::data::Manticore::Status::Running;
    manticore.connectionCheckTimer_->setInterval(10);

    QTcpSocket client;
    client.connectToHost(QHostAddress::LocalHost, apiPort);
    QVERIFY(client.waitForConnected(1000));
    client.write("GET /healthz HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
    QVERIFY(client.waitForBytesWritten(1000));

    QElapsedTimer elapsed;
    elapsed.start();
    QEventLoop responseLoop;
    QTimer deadline;
    deadline.setSingleShot(true);
    connect(&client, &QTcpSocket::readyRead, &responseLoop, &QEventLoop::quit);
    connect(&deadline, &QTimer::timeout, &responseLoop, &QEventLoop::quit);
    deadline.start(400);
    manticore.connectionCheckTimer_->start();
    responseLoop.exec();

    QVERIFY2(client.bytesAvailable() > 0,
        "HTTP health endpoint must respond while the Manticore probe is stalled");
    QVERIFY2(elapsed.elapsed() < 400,
        qPrintable(QStringLiteral("health response took %1 ms").arg(elapsed.elapsed())));
    QTRY_COMPARE_WITH_TIMEOUT(acceptedConnections, 1, 100);
    QTRY_COMPARE_WITH_TIMEOUT(stalledQueries, 1, 100);

    // Once the first probe is blocked, further timer ticks must not open more
    // sockets or stack up additional health checks.
    manticore.connectionCheckTimer_->stop();
    QTest::qWait(2200); // Longer than MYSQL_OPT_READ_TIMEOUT.
    QTRY_VERIFY_WITH_TIMEOUT(!manticore.connectionProbeInFlight_.load(), 1000);
    QCOMPARE(acceptedConnections, 1);
}

} // namespace rats::data

QTEST_MAIN(rats::data::TestManticoreHealthMonitor)
#include "test_manticore_health_monitor.moc"
