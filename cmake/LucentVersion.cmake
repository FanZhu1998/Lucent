# Build identity used by logs, recipes, diagnostics and support reports.
# Produces LUCENT_VERSION_STRING and LUCENT_GIT_COMMIT for configure_file() templates.
#
# LUCENT_GIT_COMMIT is the abbreviated commit at configure time, with "-dirty" appended when tracked
# files differ from it (uncommitted changes): evidence made by such a build (the qualification
# report and the camera support database, ADR 0018) must not name a commit that cannot have produced
# it (QUA-06). Configure again (tools\build.ps1 -Fresh) after committing to drop the marker.

find_package(Git QUIET)
set(LUCENT_GIT_COMMIT "unknown")
if(GIT_FOUND)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-parse --short=12 HEAD
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        OUTPUT_VARIABLE _lucent_git_commit
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE _lucent_git_result)
    if(_lucent_git_result EQUAL 0 AND _lucent_git_commit)
        set(LUCENT_GIT_COMMIT "${_lucent_git_commit}")
        # Tracked files only: build trees and fixture caches are ignored or untracked.
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" status --porcelain --untracked-files=no
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            OUTPUT_VARIABLE _lucent_git_changes
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
            RESULT_VARIABLE _lucent_git_status_result)
        if(NOT _lucent_git_status_result EQUAL 0)
            set(LUCENT_GIT_COMMIT "${_lucent_git_commit}-unknown-state")
        elseif(_lucent_git_changes)
            set(LUCENT_GIT_COMMIT "${_lucent_git_commit}-dirty")
        endif()
    endif()
endif()

set(LUCENT_VERSION_STRING "${PROJECT_VERSION}")
