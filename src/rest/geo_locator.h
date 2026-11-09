#pragma once

#include <QJsonObject>
#include <QString>
#include <memory>

namespace rats::rest {

// Optional, entirely local MMDB lookup. No IP is sent to an external service.
class GeoLocator {
public:
    explicit GeoLocator(const QString& path);
    ~GeoLocator();
    bool available() const;
    QJsonObject locate(const QString& ip) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace rats::rest
