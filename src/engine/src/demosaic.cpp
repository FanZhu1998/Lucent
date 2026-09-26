// Demosaicing route selection and dispatch (demosaic.hpp).

#include <cstdint>
#include <format>
#include <string_view>
#include <type_traits>
#include <utility>

#include <lucent/engine/demosaic.hpp>

namespace lucent::engine {

namespace {

constexpr const char* kWhereRoute = "engine.demosaic.route";
constexpr const char* kWhereInput = "engine.demosaic.input";

constexpr std::string_view kMalvarAlgorithm = "malvar-he-cutler-2004";
constexpr std::string_view kXTransAlgorithm = "xtrans-3x3-same-colour-mean";
constexpr std::string_view kLinearAlgorithm = "linear-color-bypass";
constexpr std::string_view kMonochromeAlgorithm = "monochrome-bypass";
constexpr std::string_view kMalvarBorder =
    "whole-sample-symmetric mirror (index -k -> k, n-1+k -> n-1-k), which keeps the Bayer phase";
constexpr std::string_view kXTransBorder =
    "same-colour mean over the smallest window, clipped to the image, that holds the colour";

DemosaicRecord record_for(DemosaicRoute route, const image::CfaPattern& cfa) {
    DemosaicRecord record;
    record.route = route;
    switch (route) {
        case DemosaicRoute::BayerMalvarHeCutler:
            record.maturity = RouteMaturity::Validated;
            record.algorithm = kMalvarAlgorithm;
            record.algorithm_version = kMalvarHeCutlerVersion;
            record.border_handling = kMalvarBorder;
            record.cfa = cfa;
            break;
        case DemosaicRoute::XTransBaseline:
            record.maturity = RouteMaturity::Baseline;
            record.algorithm = kXTransAlgorithm;
            record.algorithm_version = kXTransBaselineVersion;
            record.border_handling = kXTransBorder;
            record.cfa = cfa;
            break;
        case DemosaicRoute::LinearColorBypass:
            record.maturity = RouteMaturity::Bypass;
            record.algorithm = kLinearAlgorithm;
            record.algorithm_version = kBypassVersion;
            break;
        case DemosaicRoute::MonochromeBypass:
            record.maturity = RouteMaturity::Bypass;
            record.algorithm = kMonochromeAlgorithm;
            record.algorithm_version = kBypassVersion;
            break;
    }
    return record;
}

// Shared implementation. Sensor is SensorImage (consume: buffers are moved out of `sensor`) or
// const SensorImage (buffers are copied).
template <class Sensor>
Expected<DemosaicedImage> run(Sensor& sensor, const DemosaicOptions& options, const core::CancellationToken& cancel) {
    constexpr bool consume = !std::is_const_v<Sensor>;
    auto route = select_demosaic_route(sensor);
    if (!route) {
        return Unexpected(std::move(route.error()));
    }
    if (sensor.pixels.empty()) {
        return fail(ErrorCode::InvalidArgument, "the sensor image is empty", kWhereInput);
    }
    if (auto cancelled = cancel.check("engine.demosaic.run"); !cancelled) {
        return Unexpected(std::move(cancelled.error()));
    }
    DemosaicedImage result;
    result.demosaic = record_for(*route, sensor.cfa);
    result.normalization = sensor.normalization;
    result.white_balance = sensor.white_balance;

    switch (*route) {
        case DemosaicRoute::BayerMalvarHeCutler:
        case DemosaicRoute::XTransBaseline: {
            auto rgb = image::ImageBuffer<float>::create_uninitialized(sensor.pixels.width(), sensor.pixels.height(),
                                                                       3, options.limits);
            if (!rgb) {
                return Unexpected(std::move(rgb.error()));
            }
            const Status status =
                *route == DemosaicRoute::BayerMalvarHeCutler
                    ? demosaic_bayer_mhc(sensor.pixels.const_view(), sensor.cfa, rgb->view(), cancel,
                                         options.parallel)
                    : demosaic_xtrans_baseline(sensor.pixels.const_view(), sensor.cfa, rgb->view(), cancel,
                                               options.parallel);
            if (!status) {
                return Unexpected(status.error());
            }
            if constexpr (consume) {
                sensor.pixels = {};  // release the mosaic before the caller continues
            }
            result.pixels = std::move(*rgb);
            result.encoding = image::ColorEncoding::camera_native_linear();
            break;
        }
        case DemosaicRoute::LinearColorBypass:
        case DemosaicRoute::MonochromeBypass: {
            if constexpr (consume) {
                result.pixels = std::move(sensor.pixels);
            } else {
                auto copy = sensor.pixels.clone();
                if (!copy) {
                    return Unexpected(std::move(copy.error()));
                }
                result.pixels = std::move(*copy);
            }
            result.encoding = sensor.encoding;
            break;
        }
    }
    if constexpr (consume) {
        result.saturation = std::move(sensor.saturation);
    } else {
        auto mask = sensor.saturation.clone();
        if (!mask) {
            return Unexpected(std::move(mask.error()));
        }
        result.saturation = std::move(*mask);
    }
    return result;
}

}  // namespace

std::string_view to_string(DemosaicRoute route) noexcept {
    switch (route) {
        case DemosaicRoute::BayerMalvarHeCutler:
            return "bayer_malvar_he_cutler";
        case DemosaicRoute::XTransBaseline:
            return "xtrans_baseline";
        case DemosaicRoute::LinearColorBypass:
            return "linear_color_bypass";
        case DemosaicRoute::MonochromeBypass:
            return "monochrome_bypass";
    }
    return "invalid";
}

std::string_view to_string(RouteMaturity maturity) noexcept {
    switch (maturity) {
        case RouteMaturity::Validated:
            return "validated";
        case RouteMaturity::Baseline:
            return "baseline";
        case RouteMaturity::Bypass:
            return "bypass";
    }
    return "invalid";
}

Expected<DemosaicRoute> select_demosaic_route(const SensorImage& sensor) {
    switch (sensor.representation) {
        case raw::RawRepresentation::Mosaic:
            if (sensor.cfa.empty()) {
                return fail(ErrorCode::InvalidArgument, "a mosaic sensor image needs its CFA pattern", kWhereRoute);
            }
            if (sensor.cfa.is_bayer()) {
                return DemosaicRoute::BayerMalvarHeCutler;
            }
            if (sensor.cfa.is_xtrans()) {
                return DemosaicRoute::XTransBaseline;
            }
            return fail(ErrorCode::Unsupported,
                        std::format("no demosaicing route for the {}x{} CFA '{}' (only Bayer and X-Trans are supported)",
                                    sensor.cfa.width(), sensor.cfa.height(), sensor.cfa.to_string()),
                        kWhereRoute);
        case raw::RawRepresentation::LinearColor:
            if (sensor.pixels.channels() != 3 && !sensor.pixels.empty()) {
                return fail(ErrorCode::InvalidArgument, "full-colour sensor data must have 3 channels", kWhereRoute);
            }
            return DemosaicRoute::LinearColorBypass;
        case raw::RawRepresentation::Monochrome:
            if (sensor.pixels.channels() != 1 && !sensor.pixels.empty()) {
                return fail(ErrorCode::InvalidArgument, "monochrome sensor data must have 1 channel", kWhereRoute);
            }
            return DemosaicRoute::MonochromeBypass;
    }
    return fail(ErrorCode::Unsupported, "unknown sensor representation", kWhereRoute);
}

Expected<DemosaicedImage> demosaic(const SensorImage& sensor, const DemosaicOptions& options,
                                   const core::CancellationToken& cancel) {
    return run(sensor, options, cancel);
}

Expected<DemosaicedImage> demosaic(SensorImage&& sensor, const DemosaicOptions& options,
                                   const core::CancellationToken& cancel) {
    return run(sensor, options, cancel);
}

}  // namespace lucent::engine
