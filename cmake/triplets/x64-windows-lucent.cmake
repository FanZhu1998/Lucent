# Lucent's vcpkg triplet: 64-bit Windows, dynamic CRT, shared libraries, release only.
#
# - Shared libraries keep LGPL components (Qt, LibRaw) replaceable, as required by
#   the LGPL distribution route described in docs/adr/0003-linking-and-licensing.md.
# - Release-only halves dependency build time. Lucent's own "dev" preset uses
#   RelWithDebInfo so every binary shares the release CRT (/MD).
# - The same triplet is used as the host triplet so Qt's host tools are not built twice.
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE dynamic)
set(VCPKG_BUILD_TYPE release)

# No path of the build machine in the binaries Lucent ships (W12; ADR 0002 and ADR 0009). vcpkg
# links release DLLs with /DEBUG, and some ports put their source file names into messages:
#
# - /PDBALTPATH:%_PDB% makes the linker write only the PDB's file name into the debug directory of
#   every DLL and EXE, instead of its full path below the buildtrees folder. link.exe expands %_PDB%
#   itself; CMake, Ninja, cmd.exe (no _PDB variable) and meson (no interpolation) pass it unchanged.
# - /d1trimfile:<folder>/ removes <folder>/ from __FILE__, __builtin_FILE() and
#   std::source_location (libtiff and Qt Quick put __FILE__ into messages). MSVC documents no
#   source-path mapping. This front-end option is undocumented; MSVC 19.44 honours it (also for
#   headers, case-insensitively, either separator), accepts it silently, and leaves diagnostics,
#   /showIncludes and the debug information unchanged. /pathmap requires /experimental:deterministic,
#   which changes more than paths. The folders trimmed are the buildtrees folder (sources and
#   generated files), the installed tree (headers of dependencies) and the vcpkg root (ports,
#   packages); the first matching prefix wins. A folder containing a space is skipped (the flags are
#   a space-separated string).
#
# tools\package.ps1 scans every shipped file for these folders, the repository and the user profile,
# and fails on any hit: a compiler that stopped honouring an option cannot ship a leak. Changing this
# file (or the post-portfile step below), even its line endings, changes the ABI hash of every port,
# so every port is rebuilt once.
#
# vcpkg_copy_pdbs finds a DLL's PDB through the path the DLL records, which /PDBALTPATH reduces to
# the file name; post-portfile/install-pdbs.cmake installs the PDBs as before (developer symbols).
set(VCPKG_LINKER_FLAGS "/PDBALTPATH:%_PDB%")
set(VCPKG_POST_PORTFILE_INCLUDES "${CMAKE_CURRENT_LIST_DIR}/post-portfile/install-pdbs.cmake")
set(z_lucent_trim_flags "")
set(z_lucent_trim_folders "")
# The triplet is also read where vcpkg only collects triplet settings; these variables exist when a
# port is built (scripts/ports.cmake defines them before it includes the triplet).
if(DEFINED CURRENT_BUILDTREES_DIR AND CURRENT_BUILDTREES_DIR)
    get_filename_component(z_lucent_folder "${CURRENT_BUILDTREES_DIR}" DIRECTORY)
    list(APPEND z_lucent_trim_folders "${z_lucent_folder}")
endif()
if(DEFINED CURRENT_INSTALLED_DIR AND CURRENT_INSTALLED_DIR)
    get_filename_component(z_lucent_folder "${CURRENT_INSTALLED_DIR}" DIRECTORY)
    list(APPEND z_lucent_trim_folders "${z_lucent_folder}")
endif()
if(DEFINED VCPKG_ROOT_DIR AND VCPKG_ROOT_DIR)
    list(APPEND z_lucent_trim_folders "${VCPKG_ROOT_DIR}")
endif()
foreach(z_lucent_folder IN LISTS z_lucent_trim_folders)
    file(TO_CMAKE_PATH "${z_lucent_folder}" z_lucent_folder)
    string(REGEX REPLACE "/+$" "" z_lucent_folder "${z_lucent_folder}")
    if(z_lucent_folder AND NOT z_lucent_folder MATCHES "[ \"]")
        string(APPEND z_lucent_trim_flags " /d1trimfile:${z_lucent_folder}/")
    endif()
endforeach()
string(STRIP "${z_lucent_trim_flags}" z_lucent_trim_flags)
set(VCPKG_C_FLAGS "${z_lucent_trim_flags}")
set(VCPKG_CXX_FLAGS "${z_lucent_trim_flags}")
unset(z_lucent_trim_flags)
unset(z_lucent_trim_folders)
unset(z_lucent_folder)
