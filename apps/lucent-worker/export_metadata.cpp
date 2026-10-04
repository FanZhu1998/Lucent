#include "export_metadata.hpp"

namespace lucent::worker_app {

exporter::ExportMetadata make_export_metadata(const raw::CaptureMetadata& capture,
                                              const std::optional<std::string>& artist,
                                              const std::optional<std::string>& copyright) {
    exporter::ExportMetadata m;
    m.camera_make = capture.make;
    m.camera_model = capture.model;
    m.lens_make = capture.lens_make;
    m.lens_model = capture.lens_model;
    const auto rational = [](const std::optional<raw::ExifRational>& r) -> std::optional<exporter::ExportRational> {
        if (!r) {
            return std::nullopt;
        }
        return exporter::ExportRational{r->numerator, r->denominator};
    };
    m.exposure_time_s = rational(capture.exposure_time_s);
    m.f_number = rational(capture.f_number);
    m.iso = capture.iso;
    if (capture.exposure_bias_ev) {
        m.exposure_bias_ev =
            exporter::ExportSignedRational{capture.exposure_bias_ev->numerator, capture.exposure_bias_ev->denominator};
    }
    m.focal_length_mm = rational(capture.focal_length_mm);
    m.focal_length_35mm_mm = capture.focal_length_35mm_mm;
    if (capture.capture_time) {
        const raw::ExifDateTime& t = *capture.capture_time;
        m.capture_time = exporter::CaptureDateTime{t.year,   t.month,  t.day,    t.hour,
                                                   t.minute, t.second, t.subsec, t.utc_offset_minutes};
    }
    m.artist = artist.value_or(capture.artist);
    m.copyright = copyright.value_or(capture.copyright);
    if (capture.gps) {
        exporter::GpsPosition position;
        position.latitude_deg = capture.gps->latitude_deg;
        position.longitude_deg = capture.gps->longitude_deg;
        position.altitude_m = capture.gps->altitude_m;
        if (capture.gps->utc) {
            const raw::GpsTimestamp& u = *capture.gps->utc;
            position.utc = exporter::GpsTimestamp{u.year, u.month, u.day, u.hour, u.minute, u.second};
        }
        m.gps = position;
    }
    return m;
}

}  // namespace lucent::worker_app
