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
# research headers exactly as an in-tree one can. Plan 5.6 (S5-T27) deliberately
# left that shape alone -- the reasons are recorded at the two add_library calls
# in atx-vol/CMakeLists.txt -- so this file still installs what the build graph
# says today, and the two atx::vol::tools / atx::vol::research alias shims in
# cmake/atx-volConfig.cmake.in are still exactly right: they recreate in-tree
# spellings over unchanged INTERFACE targets. Do not "fix" the reachability here.
#
# What 5.6 DID add is the tools tier's executables, below the library block: three
# operator CLIs that install into <prefix>/bin. They are targets of this package,
# not part of its export set -- see that block for why.

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

# ---- Operator CLIs (plan 5.6) -----------------------------------------------
# The three shipped tools, into <prefix>/bin. ATX_BUILD_TOOLS is ON by default
# (root CMakeLists); with it OFF these targets do not exist and the package
# installs as a headers-plus-libraries distribution, which is why the guard is
# here and not a hard dependency.
#
# NOT IN THE EXPORT SET, on purpose. `install(TARGETS ... EXPORT atx-volTargets)`
# on an executable emits an IMPORTED executable into atx-volTargets.cmake, which
# every `find_package(atx-vol)` consumer then loads whether or not it wants the
# tools -- and the file would name binaries an ATX_BUILD_TOOLS=OFF prefix never
# installed. A consumer links this package's LIBRARIES; it runs the tools from
# PATH like any other program. Keeping them out also means their PRIVATE link to
# atx::vol / atx::core / atx_warnings creates no export-interface obligation:
# the $<LINK_ONLY:> rule that forces an exported target's private deps into an
# export set applies to exported targets, and these are not.
if(ATX_BUILD_TOOLS)
    install(TARGETS
                atx-vol-surface-db-build
                atx-vol-surface-db
                atxvol_spy_dispersion_backtest
            RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
endif()

# ---- Public headers ---------------------------------------------------------
# atx-vol's three include roots plus the two first-party layers its exported
# targets name. Nothing under tests/ or detail-of-another-module ships.
#
# api-restructure Task 3 (2026-08-14): narrowed from the old blanket
# `install(DIRECTORY .../atx-vol/include/ DESTINATION ...)` (which copied
# whatever sat under atx-vol/include/atx/vol/, api/ or not) to exactly the
# include/atx/vol/api/ subtree Tasks 1-2 made the module-split public surface.
# The one non-api/ header that still ships is the generated version header
# immediately below (its own atx/vol/detail/ install, untouched by this
# narrowing); nothing under src/ or an old-layout atx/vol/detail/*.hpp is
# reachable from this DIRECTORY root at all, so there is nothing to
# accidentally leak. atx-vol/tools/include/ and atx-vol/research/include/
# (further below) are a SEPARATE, pre-existing Tier-B tiering (S4-T18 / plan
# 4.1, see atx-vol/CMakeLists.txt) outside the 8-module api/ split this task
# restructures, and are deliberately left as their own install() calls.
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/atx-vol/include/atx/vol/api/"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/atx/vol/api")
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

# On the 1.x line the tiering IS the compatibility statement (plan 5.3, and see
# the API stability section of atx-vol/README.md): Tier-A -- the
# `atx/vol/vol.hpp` set -- is frozen for the 1.x line, so a 1.y consumer builds
# against any 1.z >= y. That is exactly SameMajorVersion. It was
# SameMinorVersion while the version was 0.y.z, where semver gives a minor bump
# the meaning a major bump has now; keeping that would refuse a 1.1 package to a
# `find_package(atx-vol 1.0)` consumer for whom nothing frozen had changed.
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
