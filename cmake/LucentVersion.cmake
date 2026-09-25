# Build identity used by logs, recipes, diagnostics and support reports.
# Produces LUCENT_VERSION_STRING and LUCENT_GIT_COMMIT for configure_file() templates.

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
    endif()
endif()

set(LUCENT_VERSION_STRING "${PROJECT_VERSION}")
