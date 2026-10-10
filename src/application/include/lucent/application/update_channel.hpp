#pragma once

// The update channel a build checks (ADR 0022). It is fixed when Lucent is built, from the CMake
// cache variables (defaults: the environment variables of the same names at the first configure):
//
//   LUCENT_UPDATE_CHANNEL                the channel's name, for example "stable"
//   LUCENT_UPDATE_MANIFEST_URL           the https URL of its release manifest (update_manifest.hpp);
//                                        the detached signature is <URL>.sig
//   LUCENT_UPDATE_PUBLIC_KEYS            the ECDSA P-256 public keys whose signatures are accepted,
//                                        ';'-separated uncompressed points (130 hex digits each;
//                                        more than one only while a key is being replaced)
//   LUCENT_UPDATE_PUBLISHER_THUMBPRINTS  optional: the Authenticode signer certificates an installer
//                                        must be signed with (';'-separated SHA-1 or SHA-256
//                                        thumbprints); empty: the installer's signature is not checked
//
// All empty (the default): updates are not configured, the shell says so, and Lucent never opens a
// network connection. The first three must be set together; src/application/CMakeLists.txt refuses
// a partial or malformed configuration, and make_update_channel() checks it again (the keys through
// CNG). Nothing at run time can change the channel or its keys: they are compiled into Lucent.exe,
// which Authenticode signing protects (ADR 0009).
//
// Thread-safety: pure functions; build_update_channel() is computed once (thread-safe).

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <lucent/application/http_client.hpp>
#include <lucent/core/error.hpp>
#include <lucent/core/signature.hpp>

namespace lucent::app {

struct UpdateChannel {
    std::string name;                                 // "stable"
    std::string manifest_url;                         // https://.../release-manifest.json
    std::vector<core::EcdsaP256PublicKey> keys;       // pinned release-manifest keys
    std::vector<std::string> publisher_thumbprints;   // lowercase hex (40: SHA-1, 64: SHA-256)

    [[nodiscard]] bool configured() const noexcept { return !manifest_url.empty(); }
    [[nodiscard]] std::string signature_url() const { return manifest_url + ".sig"; }
};

// A channel from its configuration text (see the file comment): lists separated by ';', spaces and
// letter case ignored. All four empty: a channel that is not configured. Errors ("app.update.channel",
// InvalidArgument): a name, URL or key without the others, an invalid channel name, a URL `policy`
// refuses (UrlPolicy::HttpsOnly for builds), a key that is not a P-256 point, a thumbprint that is
// neither 40 nor 64 hex digits.
[[nodiscard]] Expected<UpdateChannel> make_update_channel(std::string_view name, std::string_view manifest_url,
                                                          std::string_view keys, std::string_view thumbprints,
                                                          UrlPolicy policy = UrlPolicy::HttpsOnly);

struct BuildUpdateChannel {
    UpdateChannel channel;        // not configured when `problem` is set
    std::optional<Error> problem; // why the compiled configuration was refused (never with a valid build)
};

// The channel compiled into this build.
[[nodiscard]] const BuildUpdateChannel& build_update_channel();

}  // namespace lucent::app
