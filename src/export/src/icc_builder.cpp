#include "icc_builder.hpp"

#include <new>
#include <string>

namespace lucent::exporter::detail {
namespace {

// IEC 61966-2-1 as Little CMS parametric type 4 (ICC 'para' function type 3):
// Y = (a X + b)^g for X >= d, Y = c X for X < d.
constexpr cmsFloat64Number kSrgbCurveParams[5] = {2.4, 1.0 / 1.055, 0.055 / 1.055, 1.0 / 12.92, 0.04045};

cmsCIExyYTRIPLE to_lcms(const image::RgbPrimaries& p) noexcept {
    return cmsCIExyYTRIPLE{{p.red.x, p.red.y, 1.0}, {p.green.x, p.green.y, 1.0}, {p.blue.x, p.blue.y, 1.0}};
}

Expected<UniqueProfile> create_rgb_profile(const LcmsContext& context, const image::RgbPrimaries& primaries,
                                           image::Chromaticity white, cmsToneCurve* curve) {
    const cmsCIExyY white_xyY{white.x, white.y, 1.0};
    const cmsCIExyYTRIPLE lcms_primaries = to_lcms(primaries);
    cmsToneCurve* curves[3] = {curve, curve, curve};
    UniqueProfile profile(cmsCreateRGBProfileTHR(context.get(), &white_xyY, &lcms_primaries, curves));
    if (!profile) {
        return Unexpected(context.error(ErrorCode::Internal, "cmsCreateRGBProfile failed", "export.icc.create"));
    }
    return profile;
}

Status write_text_tag(const LcmsContext& context, cmsHPROFILE profile, cmsTagSignature tag,
                      std::string_view text) {
    UniqueMlu mlu(cmsMLUalloc(context.get(), 1));
    const std::string value(text);
    if (!mlu || !cmsMLUsetASCII(mlu.get(), "en", "US", value.c_str()) ||
        !cmsWriteTag(profile, tag, mlu.get())) {
        return Unexpected(context.error(ErrorCode::Internal, "cannot write an ICC text tag", "export.icc.text"));
    }
    return ok();
}

}  // namespace

Expected<std::vector<std::uint8_t>> build_display_icc(const image::RgbPrimaries& primaries,
                                                      image::Chromaticity white, std::string_view description) {
    try {
        auto context = LcmsContext::create();
        if (!context) {
            return Unexpected(std::move(context.error()));
        }
        UniqueToneCurve curve(cmsBuildParametricToneCurve(context->get(), 4, kSrgbCurveParams));
        if (!curve) {
            return Unexpected(
                context->error(ErrorCode::Internal, "cannot build the sRGB tone curve", "export.icc.curve"));
        }
        auto profile = create_rgb_profile(*context, primaries, white, curve.get());
        if (!profile) {
            return Unexpected(std::move(profile.error()));
        }
        cmsHPROFILE handle = profile->get();
        cmsSetProfileVersion(handle, 4.3);
        cmsSetDeviceClass(handle, cmsSigDisplayClass);
        cmsSetHeaderRenderingIntent(handle, INTENT_PERCEPTUAL);
        cmsSetHeaderManufacturer(handle, 0);
        cmsSetHeaderModel(handle, 0);
        cmsSetHeaderAttributes(handle, 0);
        cmsSetHeaderFlags(handle, 0);
        if (auto status = write_text_tag(*context, handle, cmsSigProfileDescriptionTag, description); !status) {
            return Unexpected(std::move(status.error()));
        }
        if (auto status = write_text_tag(*context, handle, cmsSigCopyrightTag, kIccCopyright); !status) {
            return Unexpected(std::move(status.error()));
        }

        cmsUInt32Number size = 0;
        if (!cmsSaveProfileToMem(handle, nullptr, &size) || size < 128) {
            return Unexpected(
                context->error(ErrorCode::Internal, "cannot size the serialised ICC profile", "export.icc.save"));
        }
        std::vector<std::uint8_t> bytes(size);
        if (!cmsSaveProfileToMem(handle, bytes.data(), &size) || size != bytes.size()) {
            return Unexpected(
                context->error(ErrorCode::Internal, "cannot serialise the ICC profile", "export.icc.save"));
        }

        // Little CMS stamps the header with the creation time of the profile object and offers no
        // setter. Overwrite it with the fixed date (big-endian uInt16Numbers). The profile ID
        // (MD5) is not computed, so no checksum depends on the date.
        for (std::size_t i = 0; i < 6; ++i) {
            bytes[kIccDateOffset + 2 * i] = static_cast<std::uint8_t>(kIccCreationDate[i] >> 8);
            bytes[kIccDateOffset + 2 * i + 1] = static_cast<std::uint8_t>(kIccCreationDate[i] & 0xFF);
        }
        return bytes;
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "out of memory while building an ICC profile", "export.icc.build");
    }
}

Expected<UniqueProfile> create_linear_rgb_profile(const LcmsContext& context, const image::RgbPrimaries& primaries,
                                                  image::Chromaticity white) {
    UniqueToneCurve linear(cmsBuildGamma(context.get(), 1.0));
    if (!linear) {
        return Unexpected(context.error(ErrorCode::Internal, "cannot build a linear tone curve", "export.icc.curve"));
    }
    return create_rgb_profile(context, primaries, white, linear.get());
}

}  // namespace lucent::exporter::detail
