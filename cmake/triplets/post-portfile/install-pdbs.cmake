# Included by vcpkg after the portfile of every port built with the x64-windows-lucent triplet
# (VCPKG_POST_PORTFILE_INCLUDES in ../x64-windows-lucent.cmake; this file's content is part of every
# port's ABI hash, so changing it rebuilds every port).
#
# The triplet links with /PDBALTPATH:%_PDB% (no build-machine path in the binaries, ADR 0002), so a
# DLL or EXE records only its PDB's file name. vcpkg_copy_pdbs asks `dumpbin /PDBPATH` where the PDB
# is and gets no answer any more: each portfile's call warns "Could not find a matching pdb file"
# and installs no PDB. This step installs them as vcpkg did before. dumpbin /PDBPATH also searches
# the symbol path, and reports only a PDB that matches the binary, so with the folders of the
# port's release build tree that hold PDBs on _NT_SYMBOL_PATH it names each binary's own PDB, which
# is copied next to the binary in the package. A binary whose PDB is not in this port's build tree
# (a copy of another port's DLL among the tools) gets none, as before. PDBs are never shipped (ADR
# 0009); they give developers the dependencies' symbols.

if(NOT VCPKG_TARGET_IS_WINDOWS OR VCPKG_TARGET_IS_MINGW)
    return()
endif()

set(z_lucent_tree "${CURRENT_BUILDTREES_DIR}/${TARGET_TRIPLET}-rel")
set(z_lucent_pdb_dirs "")
if(IS_DIRECTORY "${z_lucent_tree}")
    file(GLOB_RECURSE z_lucent_pdbs LIST_DIRECTORIES false "${z_lucent_tree}/*.pdb")
    foreach(z_lucent_pdb IN LISTS z_lucent_pdbs)
        get_filename_component(z_lucent_dir "${z_lucent_pdb}" DIRECTORY)
        file(TO_NATIVE_PATH "${z_lucent_dir}" z_lucent_dir)
        list(APPEND z_lucent_pdb_dirs "${z_lucent_dir}")
    endforeach()
    list(REMOVE_DUPLICATES z_lucent_pdb_dirs)
endif()

if(z_lucent_pdb_dirs)
    set(z_lucent_saved_symbol_path "$ENV{_NT_SYMBOL_PATH}")
    set(z_lucent_saved_vslang "$ENV{VSLANG}")
    # The symbol path is a semicolon-separated list of folders, as a CMake list is.
    set(ENV{_NT_SYMBOL_PATH} "${z_lucent_pdb_dirs}")
    set(ENV{VSLANG} 1033)   # English dumpbin output, as vcpkg_copy_pdbs sets it
    file(GLOB_RECURSE z_lucent_binaries LIST_DIRECTORIES false
        "${CURRENT_PACKAGES_DIR}/*.dll" "${CURRENT_PACKAGES_DIR}/*.exe")
    set(z_lucent_installed 0)
    foreach(z_lucent_binary IN LISTS z_lucent_binaries)
        get_filename_component(z_lucent_binary_dir "${z_lucent_binary}" DIRECTORY)
        execute_process(COMMAND dumpbin /nologo /PDBPATH "${z_lucent_binary}"
            OUTPUT_VARIABLE z_lucent_output
            ERROR_QUIET
            RESULT_VARIABLE z_lucent_result)
        if(NOT z_lucent_result EQUAL 0 OR NOT z_lucent_output MATCHES "PDB file found at '([^']+)'")
            continue()
        endif()
        file(TO_CMAKE_PATH "${CMAKE_MATCH_1}" z_lucent_found)
        get_filename_component(z_lucent_found_dir "${z_lucent_found}" DIRECTORY)
        string(TOLOWER "${z_lucent_found_dir}" z_lucent_found_key)
        string(TOLOWER "${z_lucent_binary_dir}" z_lucent_binary_key)
        if(z_lucent_found_key STREQUAL z_lucent_binary_key)
            continue()   # already beside the binary
        endif()
        file(COPY "${z_lucent_found}" DESTINATION "${z_lucent_binary_dir}")
        math(EXPR z_lucent_installed "${z_lucent_installed} + 1")
    endforeach()
    set(ENV{_NT_SYMBOL_PATH} "${z_lucent_saved_symbol_path}")
    set(ENV{VSLANG} "${z_lucent_saved_vslang}")
    message(STATUS "Installed ${z_lucent_installed} PDB file(s) found through the symbol path (x64-windows-lucent links with /PDBALTPATH)")
endif()

unset(z_lucent_tree)
unset(z_lucent_pdb_dirs)
unset(z_lucent_pdbs)
unset(z_lucent_pdb)
unset(z_lucent_dir)
unset(z_lucent_binaries)
unset(z_lucent_binary)
unset(z_lucent_binary_dir)
unset(z_lucent_output)
unset(z_lucent_result)
unset(z_lucent_found)
unset(z_lucent_found_dir)
unset(z_lucent_found_key)
unset(z_lucent_binary_key)
unset(z_lucent_installed)
unset(z_lucent_saved_symbol_path)
unset(z_lucent_saved_vslang)
