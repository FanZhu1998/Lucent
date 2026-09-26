#include "lcms_support.hpp"

#include <cstdio>
#include <cstring>
#include <new>
#include <utility>

namespace lucent::exporter::detail {
namespace {

void on_lcms_error(cmsContext context, cmsUInt32Number code, const char* text) {
    auto* log = static_cast<LcmsErrorLog*>(cmsGetContextUserData(context));
    if (log == nullptr || log->failed) {
        return;  // keep the first (root-cause) message
    }
    log->failed = true;
    log->code = code;
    std::snprintf(log->message, sizeof(log->message), "%s", text != nullptr ? text : "");
}

}  // namespace

Expected<LcmsContext> LcmsContext::create() {
    LcmsContext result;
    result.log_.reset(new (std::nothrow) LcmsErrorLog{});
    if (!result.log_) {
        return fail(ErrorCode::OutOfMemory, "cannot allocate the Little CMS error log", "export.lcms.context");
    }
    result.context_.reset(cmsCreateContext(nullptr, result.log_.get()));
    if (!result.context_) {
        return fail(ErrorCode::Internal, "cmsCreateContext failed", "export.lcms.context");
    }
    cmsSetLogErrorHandlerTHR(result.get(), on_lcms_error);
    return result;
}

Error LcmsContext::error(ErrorCode code, std::string what, std::string where) const {
    Error error{code, std::move(what), std::move(where), 0};
    if (log_ && log_->failed) {
        error.message += ": ";
        error.message += log_->message;
        error.native_code = log_->code;
    }
    return error;
}

std::string lcms_library_version() {
    const int encoded = cmsGetEncodedCMMversion();  // e.g. 2190
    char text[32];
    std::snprintf(text, sizeof(text), "%d.%d", encoded / 1000, (encoded % 1000) / 10);
    return text;
}

}  // namespace lucent::exporter::detail
