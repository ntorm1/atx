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

# Pre-1.0: a minor bump is a breaking bump, so SameMinorVersion is the honest
# compatibility statement for 0.y.z.
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/atx-volConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMinorVersion)

install(FILES
            "${CMAKE_CURRENT_BINARY_DIR}/atx-volConfig.cmake"
            "${CMAKE_CURRENT_BINARY_DIR}/atx-volConfigVersion.cmake"
        DESTINATION "${ATX_VOL_INSTALL_CMAKEDIR}")
