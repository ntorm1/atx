# ---- atx-vol: equity-options pricing & volatility-fitting library ----------
#
# Ported from the C17 `ats-vol` library into idiomatic C++20 following the atx
# agent profile (.agents/cpp/agent.md). Depends on atx-core for the error
# vocabulary (Result/Status) and the special-function math (normal CDF/PDF)
# folded in during the port. Layered above atx-core, beside atx-tsdb.
#
# Scope: the full ats-vol port (Black-76 + Greeks, implied vol, eSSVI/SVI/C8/
# CStar calibrators, Andersen-Lake/BAW American pricers, curves, universe,
# portfolio/risk, vol derivatives, surface archive) PLUS a Vola-Dynamics
# American-equity parity layer: de-Americanization (american_iv, deamer), hybrid
# dividends + borrow (dividend), exact S3 curve (s3), fit metrics / reduced-χ²
# (fit_metrics), parity metrics (parity), panel fixtures (panel), and the
# single-/multi-expiry parity harness (vola_parity, surface_parity). See
# README.md and docs/superpowers/specs/2026-07-04-atx-vol-vola-parity-design.md.

add_library(atx-vol ${ATX_LIB_TYPE}
    src/version.cpp
    src/black76.cpp
    src/greeks.cpp
    src/implied_vol.cpp
    src/surface.cpp
    src/curve.cpp
    src/universe.cpp
    src/derivatives.cpp
    src/vol_surface.cpp
    src/american.cpp
    src/correction.cpp
    src/arb.cpp
    src/calib.cpp
    src/c8.cpp
    src/c8_calib.cpp
    src/essvi_calib.cpp
    src/svi_calib.cpp
    src/cstar.cpp
    src/cstar_calib.cpp
    src/profile.cpp
    src/data.cpp
    src/surface_archive.cpp
    src/batch.cpp
    src/portfolio_price.cpp
    src/bulk.cpp
    src/portfolio_greeks.cpp
    src/calib_pool.cpp
    src/projection.cpp
    src/portfolio_risk.cpp
    src/american_iv.cpp
    src/dividend.cpp
    src/s3.cpp
    src/fit_metrics.cpp
    src/deamer.cpp
    src/parity.cpp
    src/panel.cpp
    src/vola_parity.cpp
    src/surface_parity.cpp
    src/opra_panel.cpp
    src/session.cpp
)
add_library(atx::vol ALIAS atx-vol)

target_include_directories(atx-vol
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_compile_features(atx-vol PUBLIC cxx_std_20)

target_link_libraries(atx-vol
    PUBLIC
        atx::core
    PRIVATE
        atx_warnings
)

if(ATX_BUILD_TESTS)
    add_subdirectory(tests)
endif()

# Real-data OPRA parity + throughput benchmark (opt-in via ATX_BUILD_EXAMPLES,
# shared with atx-core's example gate). Loads a real Databento OPRA cbbo-1m NBBO
# slice, de-Americanizes + fits the surface, and reports fit quality + cold-vs-
# cached throughput. Links atx::vol (+ atx::core transitively).
if(ATX_BUILD_EXAMPLES)
    add_executable(opra_parity_bench examples/opra_parity_bench.cpp)
    target_link_libraries(opra_parity_bench PRIVATE atx::vol atx::core atx_warnings)
endif()
