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

# ---- Official Databento C++ client (third-party/databento-cpp submodule) ----
# Full CMake library: HTTP Historical/Live API client + maintained DBN decoder
# (distinct from our self-contained batch-zip loader in external/databento). Built
# via its own bundled CMake as a static lib; tests, examples, and static analyzers
# are forced off so they never enter our build. It FetchContents its own
# json/httplib/date at configure time; OpenSSL and zstd resolve from vcpkg (zstd is
# already an atx dep, openssl was added to vcpkg.json for this). EXCLUDE_FROM_ALL so
# only the bits atx-core actually links get built. See the submodule's pinned tag
# (v0.59.0) for source/version. Linked PRIVATE into atx-core, mirroring sqlite/miniz:
# databento's symbols + include dirs reach atx-core's own TUs, but are NOT re-exported
# to downstream consumers of atx::core.
set(DATABENTO_ENABLE_UNIT_TESTING OFF CACHE BOOL "" FORCE)
set(DATABENTO_ENABLE_EXAMPLES     OFF CACHE BOOL "" FORCE)
set(DATABENTO_ENABLE_CLANG_TIDY   OFF CACHE BOOL "" FORCE)
set(DATABENTO_ENABLE_CPPCHECK     OFF CACHE BOOL "" FORCE)
set(DATABENTO_ENABLE_CCACHE       OFF CACHE BOOL "" FORCE)
set(DATABENTO_WARNINGS_AS_ERRORS  OFF CACHE BOOL "" FORCE)
add_subdirectory(third-party/databento-cpp EXCLUDE_FROM_ALL)
# Demote databento's headers (and its fetched deps') to SYSTEM includes so they never
# trip atx-core's /W4 /permissive- /WX gate when an atx TU includes <databento/...>.
get_target_property(_dbn_incs databento INTERFACE_INCLUDE_DIRECTORIES)
if(_dbn_incs)
    set_target_properties(databento PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_dbn_incs}")
endif()

add_library(atx-core STATIC
    src/core.cpp
    src/decimal.cpp
    src/datetime.cpp
    src/symbol.cpp
    src/io/parquet.cpp
    src/io/parquet_writer.cpp
    src/io/zip_reader.cpp
    src/db/sqlite.cpp
    src/external/dbn.cpp
    src/external/databento.cpp
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
        databento::databento
        zstd::libzstd
)

# Compile in every log level; runtime level (default err) does the filtering.
# Raise this in release builds to strip low-severity calls at compile time.
target_compile_definitions(atx-core
    PUBLIC
        SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_TRACE
)

# Opt-in fast inflate (Phase 4): link zlib-ng and flip ATX_FAST_INFLATE in
# zip_reader.cpp. PRIVATE/local — miniz still parses the central directory; only
# the inflate step changes, and the public ZipEntryReader interface is unchanged.
if(ATX_FAST_INFLATE)
    find_package(zlib-ng CONFIG REQUIRED)
    target_link_libraries(atx-core PRIVATE zlib-ng::zlib)
    target_compile_definitions(atx-core PRIVATE ATX_FAST_INFLATE=1)
endif()

if(ATX_BUILD_TESTS)
    add_subdirectory(tests)
endif()

if(ATX_BUILD_BENCH)
    add_subdirectory(bench)
endif()

# Opt-in example/smoke-test executables (e.g. the Databento client). Off by default
# so they never enter the normal build or CI.
option(ATX_BUILD_EXAMPLES "Build atx-core example executables" OFF)
if(ATX_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
