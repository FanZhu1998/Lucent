#pragma once

// Private RAII helpers around Little CMS. <lcms2.h> must never reach public headers.
//
// Every operation uses its own cmsContext whose error handler records the first error message,
// so concurrent exports never share mutable Little CMS state and diagnostics are not lost.

#include <memory>
#include <string>
#include <type_traits>

#include <lcms2.h>

#include "lucent/core/error.hpp"

namespace lucent::exporter::detail {

// First error reported by Little CMS through a context.
struct LcmsErrorLog {
    bool failed = false;
    cmsUInt32Number code = 0;
    char message[256] = {};
};

// Owns a cmsContext with an error handler bound to an LcmsErrorLog. Objects created in the
// context (profiles, transforms, curves) must be released before the context: declare them
// after the LcmsContext so they are destroyed first.
class LcmsContext {
public:
    // Internal when the context cannot be created; OutOfMemory when the log cannot be allocated.
    [[nodiscard]] static Expected<LcmsContext> create();

    LcmsContext(LcmsContext&&) noexcept = default;
    LcmsContext& operator=(LcmsContext&&) noexcept = default;
    LcmsContext(const LcmsContext&) = delete;
    LcmsContext& operator=(const LcmsContext&) = delete;
    ~LcmsContext() = default;

    [[nodiscard]] cmsContext get() const noexcept { return context_.get(); }

    // Builds an Error with `code`, `where` and "<what>: <first Little CMS message>" (or just
    // `what` when Little CMS reported nothing). native_code is the Little CMS error code.
    [[nodiscard]] Error error(ErrorCode code, std::string what, std::string where) const;

private:
    struct ContextDeleter {
        void operator()(cmsContext context) const noexcept { cmsDeleteContext(context); }
    };

    LcmsContext() = default;

    std::unique_ptr<LcmsErrorLog> log_;  // user data of the context; must outlive it
    std::unique_ptr<std::remove_pointer_t<cmsContext>, ContextDeleter> context_;
};

struct ProfileDeleter {
    void operator()(void* profile) const noexcept { cmsCloseProfile(profile); }
};
struct TransformDeleter {
    void operator()(void* transform) const noexcept { cmsDeleteTransform(transform); }
};
struct ToneCurveDeleter {
    void operator()(cmsToneCurve* curve) const noexcept { cmsFreeToneCurve(curve); }
};
struct MluDeleter {
    void operator()(cmsMLU* mlu) const noexcept { cmsMLUfree(mlu); }
};

using UniqueProfile = std::unique_ptr<void, ProfileDeleter>;
using UniqueTransform = std::unique_ptr<void, TransformDeleter>;
using UniqueToneCurve = std::unique_ptr<cmsToneCurve, ToneCurveDeleter>;
using UniqueMlu = std::unique_ptr<cmsMLU, MluDeleter>;

// "2.19" for LCMS_VERSION 2190 (the version of the linked library, cmsGetEncodedCMMversion()).
[[nodiscard]] std::string lcms_library_version();

}  // namespace lucent::exporter::detail
