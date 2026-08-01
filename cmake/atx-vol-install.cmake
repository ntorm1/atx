# ---- atx-vol distribution package: install + export (plan 5.1) -------------
#
# Included from the ROOT CMakeLists after every add_subdirectory(), because the
# export set spans modules: `install(TARGETS)` needs its targets to exist, and
# atx-tsdb is configured after atx-vol.
#
# ── WHAT SHIPS, AND WHY IT IS THIS MUCH ──────────────────────────────────────
#
# atx-vol's own public headers reach exactly THREE non-atx-vol first-party
# headers -- atx/core/error.hpp, atx/core/macro.hpp, atx/core/aligned.hpp --
# and, through error.hpp, exactly ONE third-party header: <tl/expected.hpp>,
# because `atx::vol::Result<T>` IS `tl::expected<T, atx::core::Error>`. No
# atx-vol public header includes any other third-party header (the only angle
# include with a path anywhere under include/ is <immintrin.h>, in detail/).
#
# The library nevertheless drags atx::core PUBLICLY and cannot stop: the error
# type is a re-export, so `atx/core/error.hpp` is part of the compile closure
# of `atx/vol/types.hpp`, which every other public header includes. And once
# atx-core's include tree ships, atx-core's OWN public surface has to be
# consumable: atx/core/io/parquet.hpp names Arrow, atx/core/log.hpp names
# spdlog, atx/core/linalg/*.hpp name Eigen, atx/core/simd.hpp names xsimd,
# atx/core/container/hash_map.hpp names ankerl. Those are genuinely PUBLIC for
# atx-core; demoting them would be a lie about atx-core's headers, not a fix.
# So this package redistributes the vendored header-only ones and honestly
# find_dependency()s the vcpkg ones (Arrow / Parquet / zstd) in the Config.
#
# ── WHAT IS DELIBERATELY CONTAINED ───────────────────────────────────────────
#
# databento (atx-core/third-party/databento-cpp) is linked PRIVATE into
# atx-core, and for a STATIC library CMake would still record it in the export
# interface as $<LINK_ONLY:databento::databento>. That target is unexportable
# from here: it is added EXCLUDE_FROM_ALL, its own install rules therefore never
# run, and its PUBLIC interface pulls date::date / httplib / nlohmann_json /
# OpenSSL -- an entire second dependency tree, for a client no atx-vol consumer
# can reach (nothing under atx-vol/ includes atx/external/ at all). atx-core
# links it through $<BUILD_LOCAL_INTERFACE:...> instead, which keeps the in-tree
# link identical and drops it from the exported interface. The atx/external/
# headers still ship -- they name no third-party type -- but calling into them
# from an installed consumer will not link. That is the containment, stated.
#
# ── TIERS AND TARGETS ────────────────────────────────────────────────────────
#
# atx-vol-tools / atx-vol-research are INTERFACE include roots whose translation
# units still compile into atx-vol (see atx-vol/CMakeLists.txt); they are linked
# PUBLIC there, so an installed `atx::vol` consumer can reach the tools and
# research headers exactly as an in-tree one can. Plan 5.6 owns splitting the
# .cpp out and demoting them; this file only has to install what the build graph
# says today, and it does. Do not "fix" the reachability here.

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# ---- Static-only distribution (plan 5.2) ------------------------------------
# The full reasoning is at the ATX_SHARED_LIBS block in the root CMakeLists: a
# DLL build of the atx libraries links but is not correct, because every
# instrumentation plane atx-vol carries is a header-inline global and Windows
# gives those one instance per image. There is no ATX_VOL_API export macro to
# fix that for v1, so a shared build is a developer link-speed tool and its
# install output would be a package whose counters and solve ledger silently
# read zero in the consumer.
#
# Refused at INSTALL time, not configure time, on purpose: `dev-shared` must
# still configure and build. `cmake --install` is where the mistake would
# actually ship, so that is where it stops.
if(ATX_SHARED_LIBS)
    install(CODE "message(FATAL_ERROR
        \"atx-vol is distributed STATIC-ONLY (plan 5.2). This build has \"
        \"ATX_SHARED_LIBS=ON, which duplicates the header-inline instrumentation \"
        \"globals (solve ledger, lightweight sampler, phase timers) per image and \"
        \"has no ATX_VOL_API export macro to bind them across a DLL boundary -- an \"
        \"installed consumer would read empty counters with no error. Reconfigure \"
        \"with -DATX_SHARED_LIBS=OFF and install that build.\")")
endif()

set(ATX_VOL_INSTALL_CMAKEDIR "${CMAKE_INSTALL_LIBDIR}/cmake/atx-vol")

# ---- Exported target names --------------------------------------------------
# The export NAMESPACE is `atx::`, so the exported name is the second component.
# atx-vol / atx-core / atx-tsdb get their in-tree alias spelling back; the two
# side include roots cannot (a namespace cannot add a third `::` level), so the
# Config file recreates atx::vol::tools / atx::vol::research over these.
set_target_properties(atx-vol          PROPERTIES EXPORT_NAME vol)
set_target_properties(atx-vol-tools    PROPERTIES EXPORT_NAME vol-tools)
set_target_properties(atx-vol-research PROPERTIES EXPORT_NAME vol-research)
set_target_properties(atx-core         PROPERTIES EXPORT_NAME core)
set_target_properties(atx-tsdb         PROPERTIES EXPORT_NAME tsdb)
# Internal, but named in the STATIC link interface under $<LINK_ONLY:> (which
# carries the link and NOT the compile usage requirements -- an installed
# consumer does not inherit /W4 /WX from atx::warnings, nor sqlite's defines).
set_target_properties(atx_warnings     PROPERTIES EXPORT_NAME warnings)
set_target_properties(atx_sqlite3      PROPERTIES EXPORT_NAME sqlite3)
set_target_properties(atx_miniz        PROPERTIES EXPORT_NAME miniz)

# ---- Vendored header-only deps whose upstream install is gated off ----------
# tl-expected, spdlog and xsimd install THEMSELVES into this prefix (their own
# install rules + package configs; spdlog's is enabled by SPDLOG_INSTALL in the
# root CMakeLists). Eigen has no CMake of ours to speak of and ankerl gates its
# install on being the top-level project, so those two ride this export set.
#
# PROVENANCE (pinned in the root CMakeLists FetchContent_Declare calls):
#   Eigen            gitlab.com/libeigen/eigen        3.4.0   (MPL-2.0)
#   unordered_dense  github.com/martinus/unordered_dense v4.4.0 (MIT)
install(TARGETS eigen unordered_dense EXPORT atx-volTargets)
install(DIRECTORY "${eigen3_SOURCE_DIR}/Eigen"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(DIRECTORY "${unordered_dense_SOURCE_DIR}/include/ankerl"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

# ---- First-party targets ----------------------------------------------------
install(TARGETS
            atx-vol atx-vol-tools atx-vol-research
            atx-core atx-tsdb
            atx_warnings atx_sqlite3 atx_miniz
        EXPORT atx-volTargets
        ARCHIVE  DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        LIBRARY  DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        RUNTIME  DESTINATION "${CMAKE_INSTALL_BINDIR}")

# ---- Public headers ---------------------------------------------------------
# atx-vol's three include roots plus the two first-party layers its exported
# targets name. Nothing under tests/ or detail-of-another-module ships.
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/atx-vol/include/"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
# The one atx-vol header that is generated rather than checked in: plan 5.3's
# version_generated.hpp, configure_file'd from project(VERSION) into atx-vol's
# binary dir (atx-vol/CMakeLists.txt). It lands in the same atx/vol/detail/
# prefix as the source-tree detail headers, so the installed include tree is
# indistinguishable from the in-tree one and atx/vol/version.hpp resolves it with
# the same quoted include either way.
install(FILES "${ATX_VOL_GENERATED_INCLUDE}/atx/vol/detail/version_generated.hpp"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/atx/vol/detail")
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/atx-vol/tools/include/"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/atx-vol/research/include/"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/atx-core/include/"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/atx-tsdb/include/"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

# ---- Export set + package config -------------------------------------------
install(EXPORT atx-volTargets
        FILE atx-volTargets.cmake
        NAMESPACE atx::
        DESTINATION "${ATX_VOL_INSTALL_CMAKEDIR}")

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/atx-volConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/atx-volConfig.cmake"
    INSTALL_DESTINATION "${ATX_VOL_INSTALL_CMAKEDIR}")

# At 1.0.0 the tiering IS the compatibility statement (plan 5.3, and see the API
# stability section of atx-vol/README.md): Tier-A -- the `atx/vol/vol.hpp` set --
# is frozen for the 1.x line, so a 1.y consumer builds against any 1.z >= y. That
# is exactly SameMajorVersion. It was SameMinorVersion while the version was 0.y.z,
# where semver gives a minor bump the meaning a major bump has now; keeping that
# would refuse a 1.1 package to a `find_package(atx-vol 1.0)` consumer for whom
# nothing frozen had changed.
#
# The version itself comes from PROJECT_VERSION -- `project(atx VERSION ...)` --
# the same single source of truth atx/vol/version.hpp is generated from, so the
# package version and the compiled `atx::vol::version()` cannot disagree.
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/atx-volConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)

install(FILES
            "${CMAKE_CURRENT_BINARY_DIR}/atx-volConfig.cmake"
            "${CMAKE_CURRENT_BINARY_DIR}/atx-volConfigVersion.cmake"
        DESTINATION "${ATX_VOL_INSTALL_CMAKEDIR}")
