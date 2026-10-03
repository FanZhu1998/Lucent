# lucent_catalog: local SQLite catalog, schema migrations, backups, source identity and import records (W4). SQLite is private.
#
# Layout
#   include/lucent/catalog/   public headers (no SQLite types)
#   src/                      implementation and private headers (SQLite wrapper, schema, backups)
#   tests/                    GoogleTest unit tests (synthetic catalogs; no camera fixtures)

find_package(unofficial-sqlite3 CONFIG REQUIRED)

lucent_add_library(catalog
    SOURCES
        include/lucent/catalog/catalog.hpp
        include/lucent/catalog/location.hpp
        include/lucent/catalog/types.hpp
        src/backup.cpp
        src/backup.hpp
        src/catalog.cpp
        src/catalog_impl.hpp
        src/codec.hpp
        src/files.cpp
        src/files.hpp
        src/location.cpp
        src/repository.cpp
        src/schema.cpp
        src/schema.hpp
        src/sqlite.cpp
        src/sqlite.hpp
        src/types.cpp
    PUBLIC_DEPS
        Lucent::core
    PRIVATE_DEPS
        unofficial::sqlite3::sqlite3
)

# ---------------------------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------------------------
# White-box tests see src/ for the private SQLite wrapper (which does not include sqlite3.h), the
# migration list seam (detail::CatalogAccess) and the backup naming helpers.

lucent_add_test(lucent_catalog_tests
    SOURCES
        tests/catalog_test_support.hpp
        tests/location_test.cpp
        tests/schema_test.cpp
        tests/repository_test.cpp
        tests/transaction_test.cpp
        tests/backup_test.cpp
        tests/performance_test.cpp
    DEPS
        Lucent::catalog
    LABELS
        catalog unit
)
if(TARGET lucent_catalog_tests)
    target_include_directories(lucent_catalog_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
endif()
