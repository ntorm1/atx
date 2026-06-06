# ---- Vendored SQLite amalgamation (third-party/sqlite) ----
# Compiled as its own static lib so its C source never sees the project's
# /W4 /WX gate (it is third-party C with its own warning profile). atx-core
# links it PRIVATE: the symbols propagate to the final link, but the
# third-party include dir does NOT — sqlite3.h stays out of the atx-core
# umbrella (the db/ wrapper forward-declares the opaque handles). See
# third-party/sqlite/PROVENANCE.md for source/version/integrity.
add_library(atx_sqlite3 STATIC third-party/sqlite/sqlite3.c)
target_include_directories(atx_sqlite3 PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/third-party/sqlite>)
target_compile_definitions(atx_sqlite3 PUBLIC
    SQLITE_THREADSAFE=2              # multi-thread: one connection per thread
    SQLITE_DQS=0                     # forbid double-quoted string literals (hardening)
    SQLITE_DEFAULT_FOREIGN_KEYS=1    # enforce foreign keys by default
    SQLITE_DEFAULT_MEMSTATUS=0       # skip per-call memory accounting (perf)
    SQLITE_LIKE_DOESNT_MATCH_BLOBS
    SQLITE_OMIT_DEPRECATED
    SQLITE_USE_ALLOCA
    SQLITE_ENABLE_COLUMN_METADATA
    SQLITE_ENABLE_FTS5
)
# Third-party C: silence ALL warnings for this TU (must not trip /WX).
if(MSVC)
    target_compile_options(atx_sqlite3 PRIVATE /w)
else()
    target_compile_options(atx_sqlite3 PRIVATE -w)
endif()

# ---- Vendored miniz (third-party/miniz) ----
# PKZIP reader for the Databento batch .zip. Own static lib so its C source never
# sees /W4 /WX; linked PRIVATE into atx-core (symbols propagate, include dir does
# not -- miniz.h stays confined to the .cpp TUs and the test target). Mirrors the
# atx_sqlite3 pattern. See third-party/miniz/PROVENANCE.md.
add_library(atx_miniz STATIC third-party/miniz/miniz.c)
target_include_directories(atx_miniz PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/third-party/miniz>)
if(MSVC)
    target_compile_options(atx_miniz PRIVATE /w)
else()
    target_compile_options(atx_miniz PRIVATE -w)
endif()

add_library(atx-core STATIC
    src/core.cpp
    src/decimal.cpp
    src/datetime.cpp
    src/symbol.cpp
    src/io/parquet.cpp
    src/db/sqlite.cpp
)
add_library(atx::core ALIAS atx-core)

target_include_directories(atx-core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_compile_features(atx-core PUBLIC cxx_std_20)

find_package(Arrow CONFIG REQUIRED)
find_package(Parquet CONFIG REQUIRED)
find_package(zstd CONFIG REQUIRED)

target_link_libraries(atx-core
    PUBLIC
        spdlog::spdlog
        tl::expected
        unordered_dense::unordered_dense
        Eigen3::Eigen
        xsimd
        Arrow::arrow_shared
        Parquet::parquet_shared
    PRIVATE
        atx_warnings
        atx_sqlite3
        atx_miniz
        zstd::libzstd
)

# Compile in every log level; runtime level (default err) does the filtering.
# Raise this in release builds to strip low-severity calls at compile time.
target_compile_definitions(atx-core
    PUBLIC
        SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_TRACE
)

if(ATX_BUILD_TESTS)
    add_subdirectory(tests)
endif()

if(ATX_BUILD_BENCH)
    add_subdirectory(bench)
endif()
