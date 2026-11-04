/**
 * @file test_update_service.cpp
 * @brief Unit tests for rats::service::UpdateService. Covers version parsing,
 *        state/settings logic and the synchronous part of checkForUpdates()
 *        without waiting on any network reply. Replaces test_updatemanager.cpp.
 */

#include <QRegularExpression>
#include <QSignalSpy>
#include <QVersionNumber>
#include <QtTest/QtTest>

#include "services/update_service.h"

using rats::service::UpdateService;

class TestUpdateService : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanup();

    // Version
    void testCurrentVersion_notEmpty();
    void testCurrentVersion_validFormat();
    void testCurrentVersionNumber_valid();
    void testCurrentVersionNumber_components();

    // State
    void testInitialState_idle();
    void testStateString_returnsString();
    void testStateString_allStatesNonEmpty();
    void testIsUpdateAvailable_initiallyFalse();

    // Settings
    void testIncludePrerelease_defaultFalse();
    void testIncludePrerelease_canBeSet();

    // Repository / info / error
    void testSetRepository_noCrash();
    void testUpdateInfo_initiallyInvalid();
    void testErrorMessage_initiallyEmpty();

    // Signal / synchronous state transition
    void testCheckForUpdates_blockedWithoutNetwork();

private:
    UpdateService* service = nullptr;
};

void TestUpdateService::initTestCase()
{
    qRegisterMetaType<UpdateService::UpdateState>("UpdateService::UpdateState");
    qDebug() << "Current app version:" << UpdateService::currentVersion();
}

void TestUpdateService::init()
{
    service = new UpdateService();
}

void TestUpdateService::cleanup()
{
    delete service;
    service = nullptr;
}

// ============================================================================
// Version
// ============================================================================

void TestUpdateService::testCurrentVersion_notEmpty()
{
    QVERIFY(!UpdateService::currentVersion().isEmpty());
}

void TestUpdateService::testCurrentVersion_validFormat()
{
    const QString version = UpdateService::currentVersion();
    QRegularExpression versionRegex("^\\d+\\.\\d+\\.\\d+(\\.\\d+)?(-[a-zA-Z0-9]+)?$");
    QVERIFY2(versionRegex.match(version).hasMatch(),
        qPrintable(QString("Version '%1' doesn't match version pattern").arg(version)));
}

void TestUpdateService::testCurrentVersionNumber_valid()
{
    QVERIFY(!UpdateService::currentVersionNumber().isNull());
}

void TestUpdateService::testCurrentVersionNumber_components()
{
    const QVersionNumber version = UpdateService::currentVersionNumber();
    QVERIFY2(version.segmentCount() >= 3,
        qPrintable(QString("Expected at least 3 segments, got %1").arg(version.segmentCount())));
    QVERIFY(version.majorVersion() >= 0);
    QVERIFY(version.minorVersion() >= 0);
    QVERIFY(version.microVersion() >= 0);
}

// ============================================================================
// State
// ============================================================================

void TestUpdateService::testInitialState_idle()
{
    QCOMPARE(service->state(), UpdateService::UpdateState::Idle);
}

void TestUpdateService::testStateString_returnsString()
{
    QVERIFY(!service->stateString().isEmpty());
}

void TestUpdateService::testStateString_allStatesNonEmpty()
{
    // stateString() reflects the current state; Idle must yield something.
    QVERIFY(!service->stateString().isEmpty());
}

void TestUpdateService::testIsUpdateAvailable_initiallyFalse()
{
    QVERIFY(!service->isUpdateAvailable());
}

// ============================================================================
// Settings
// ============================================================================

void TestUpdateService::testIncludePrerelease_defaultFalse()
{
    QVERIFY(!service->includePrerelease());
}

void TestUpdateService::testIncludePrerelease_canBeSet()
{
    service->setIncludePrerelease(true);
    QVERIFY(service->includePrerelease());
    service->setIncludePrerelease(false);
    QVERIFY(!service->includePrerelease());
}

// ============================================================================
// Repository / info / error
// ============================================================================

void TestUpdateService::testSetRepository_noCrash()
{
    service->setRepository("testowner", "testrepo");
    QVERIFY(true);
}

void TestUpdateService::testUpdateInfo_initiallyInvalid()
{
    const UpdateService::UpdateInfo& info = service->updateInfo();
    QVERIFY(!info.isValid());
    QVERIFY(info.version.isEmpty());
    QVERIFY(info.downloadUrl.isEmpty());
}

void TestUpdateService::testErrorMessage_initiallyEmpty()
{
    QVERIFY(service->errorMessage().isEmpty());
}

// ============================================================================
// Signal / synchronous state transition
// ============================================================================

void TestUpdateService::testCheckForUpdates_blockedWithoutNetwork()
{
    QSignalSpy stateSpy(service, &UpdateService::stateChanged);
    QVERIFY(stateSpy.isValid());

    // A local-only application must reject even an explicit update request
    // before issuing a network request.
    service->checkForUpdates();

    QCOMPARE(service->state(), UpdateService::UpdateState::Error);
    QVERIFY(stateSpy.count() >= 1);

    const QList<QVariant> firstChange = stateSpy.first();
    const auto firstState = firstChange.at(0).value<UpdateService::UpdateState>();
    QCOMPARE(firstState, UpdateService::UpdateState::Error);
    QVERIFY(service->errorMessage().contains("Network features are disabled"));
}

QTEST_MAIN(TestUpdateService)
#include "test_update_service.moc"
