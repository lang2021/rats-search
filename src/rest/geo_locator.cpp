#include "rest/geo_locator.h"

#ifdef RATS_GEOIP_MMDB
#include <maxminddb.h>
#endif

#include <QByteArray>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>

namespace rats::rest {

struct GeoLocator::Impl {
    QMutex mutex;
    bool opened = false;
#ifdef RATS_GEOIP_MMDB
    MMDB_s database{};
#endif
};

GeoLocator::GeoLocator(const QString& path) : impl_(std::make_unique<Impl>())
{
#ifdef RATS_GEOIP_MMDB
    if (QFileInfo::exists(path)) {
        const QByteArray bytes = QFile::encodeName(path);
        impl_->opened = MMDB_open(bytes.constData(), MMDB_MODE_MMAP, &impl_->database) == MMDB_SUCCESS;
    }
#else
    Q_UNUSED(path);
#endif
}

GeoLocator::~GeoLocator()
{
#ifdef RATS_GEOIP_MMDB
    if (impl_->opened) MMDB_close(&impl_->database);
#endif
}

bool GeoLocator::available() const { return impl_->opened; }

QJsonObject GeoLocator::locate(const QString& ip) const
{
#ifdef RATS_GEOIP_MMDB
    if (!impl_->opened) return {};
    QMutexLocker lock(&impl_->mutex);
    int gaiError = 0;
    int mmdbError = 0;
    const QByteArray address = ip.toLatin1();
    auto result = MMDB_lookup_string(&impl_->database, address.constData(), &gaiError, &mmdbError);
    if (gaiError || mmdbError != MMDB_SUCCESS || !result.found_entry) return {};
    MMDB_entry_data_s latitude{}, longitude{}, country{}, city{};
    if (MMDB_get_value(&result.entry, &latitude, "location", "latitude", nullptr) != MMDB_SUCCESS ||
        MMDB_get_value(&result.entry, &longitude, "location", "longitude", nullptr) != MMDB_SUCCESS ||
        !latitude.has_data || !longitude.has_data || latitude.type != MMDB_DATA_TYPE_DOUBLE ||
        longitude.type != MMDB_DATA_TYPE_DOUBLE) return {};
    MMDB_get_value(&result.entry, &country, "country", "names", "en", nullptr);
    MMDB_get_value(&result.entry, &city, "city", "names", "en", nullptr);
    QString region;
    if (country.has_data && country.type == MMDB_DATA_TYPE_UTF8_STRING)
        region = QString::fromUtf8(country.utf8_string, static_cast<qsizetype>(country.data_size));
    if (city.has_data && city.type == MMDB_DATA_TYPE_UTF8_STRING) {
        if (!region.isEmpty()) region += QStringLiteral(" · ");
        region += QString::fromUtf8(city.utf8_string, static_cast<qsizetype>(city.data_size));
    }
    return {{"longitude", longitude.double_value}, {"latitude", latitude.double_value},
            {"region", region.isEmpty() ? QStringLiteral("地区未知") : region}};
#else
    Q_UNUSED(ip);
    return {};
#endif
}

} // namespace rats::rest
