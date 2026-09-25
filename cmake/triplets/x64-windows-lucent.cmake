# Lucent's vcpkg triplet: 64-bit Windows, dynamic CRT, shared libraries, release only.
#
# - Shared libraries keep LGPL components (Qt, LibRaw) replaceable, as required by
#   the LGPL distribution route described in docs/adr/0003-dependencies-and-linking.md.
# - Release-only halves dependency build time. Lucent's own "dev" preset uses
#   RelWithDebInfo so every binary shares the release CRT (/MD).
# - The same triplet is used as the host triplet so Qt's host tools are not built twice.
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE dynamic)
set(VCPKG_BUILD_TYPE release)
