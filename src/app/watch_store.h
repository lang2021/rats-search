#pragma once

#include "common/result.h"
#include <QJsonObject>
#include <QMap>
#include <QObject>
#include <QThread>

namespace rats::app {

// Index producers only enqueue hash/name pairs. Rule edits and persistence run
// in the same serial order on a private Qt SQL connection.
class WatchStore : public QObject {
    Q_OBJECT
public:
    explicit WatchStore(const QString& dataDirectory);
    ~WatchStore() override;
    void observe(const QMap<QString, QString>& names);
    void request(const QString& method, const QJsonObject& params, ResultCallback respond);

signals:
    void changed();

private:
    class Storage;
    QThread thread_;
    Storage* storage_;
};

} // namespace rats::app
