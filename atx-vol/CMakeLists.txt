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
    src/dense_slice.cpp
    src/profile.cpp
    src/fit_policy.cpp
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
    src/opra_batch.cpp
    src/session.cpp
    src/chain.cpp
    src/pricer_fitter.cpp
    src/vol_curve.cpp
    src/curve_fit.cpp
    src/curve_selector.cpp
    src/priced_surface.cpp
    src/portfolio_pricer.cpp
    src/corpus.cpp
    src/dispersion.cpp
    src/backtest.cpp
    src/strategy.cpp
    src/dispersion_strategy.cpp
    src/tearsheet.cpp
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

# ---- Algorithm counters (P0.2) ----
# PUBLIC so the library AND every consumer (bench/tests that include
# atx/vol/counters.hpp) agree on the macro's value — the header's inline atomic
# storage is guarded by it, so a split view would be an ODR hazard. OFF by
# default: the definition is absent and the macros compile to nothing.
if(ATX_VOL_COUNTERS)
    target_compile_definitions(atx-vol PUBLIC ATX_VOL_COUNTERS)
endif()

if(ATX_BUILD_TESTS)
    add_subdirectory(tests)
endif()

# ---- Benchmarks (Google Benchmark; opt-in via the shared ATX_BUILD_BENCH) ----
# Mirrors atx-core/bench and atx-engine/bench. Two targets:
#   atx-vol-american-bench   — American pricer route/side/API/grid matrix
#   atx-vol-portfolio-bench  — PricedSurface queries + PortfolioPricer throughput
if(ATX_BUILD_BENCH)
    add_subdirectory(bench)
endif()

# Real-data OPRA parity + throughput benchmark (opt-in via ATX_BUILD_EXAMPLES,
# shared with atx-core's example gate). Loads a real Databento OPRA cbbo-1m NBBO
# slice, de-Americanizes + fits the surface, and reports fit quality + cold-vs-
# cached throughput. Links atx::vol (+ atx::core transitively).
if(ATX_BUILD_EXAMPLES)
    add_executable(opra_parity_bench examples/opra_parity_bench.cpp)
    target_link_libraries(opra_parity_bench PRIVATE atx::vol atx::core atx_warnings)

    # SOTA perf + accuracy proof on the synthetic SPY index fixture (no data
    # dependency; known-truth accuracy oracle). See examples/spy_surface_bench.cpp.
    add_executable(spy_surface_bench examples/spy_surface_bench.cpp)
    target_link_libraries(spy_surface_bench PRIVATE atx::vol atx::core atx_warnings)

    add_executable(spy_diag examples/spy_diag.cpp)
    target_link_libraries(spy_diag PRIVATE atx::vol atx::core atx_warnings)

    # SOTA %-within-bid-ask acceptance: eSSVI (3 DoF) vs dense C16 (16 DoF) on the
    # real SPY board, scored through the same re-Americanization. See
    # examples/spy_bidask_bench.cpp.
    add_executable(spy_bidask_bench examples/spy_bidask_bench.cpp)
    target_link_libraries(spy_bidask_bench PRIVATE atx::vol atx::core atx_warnings)

    # SOTA American-IV conversion throughput + accuracy proof (known-truth oracle,
    # no data dependency). Reports inversions/sec and round-trip sigma error for
    # the cold baseline vs the warm-started AloPricer path. See
    # examples/american_iv_bench.cpp.
    add_executable(american_iv_bench examples/american_iv_bench.cpp)
    target_link_libraries(american_iv_bench PRIVATE atx::vol atx::core atx_warnings)

    # Unified library layer (OptionChain -> PricerFitter -> parallel value_chain)
    # proof: parallel-inversion scaling {1,2,4,8 threads}, thread-count determinism,
    # and tick-to-quote update->revalue on the real SPY OPRA board. See
    # examples/chain_pricer_bench.cpp.
    add_executable(chain_pricer_bench examples/chain_pricer_bench.cpp)
    target_link_libraries(chain_pricer_bench PRIVATE atx::vol atx::core atx_warnings)

    # Ten real Databento OPRA SPY snapshots spanning intraday/stress regimes:
    # cold fit latency distribution with a per-slice 98% clean in-band gate.
    add_executable(spy_fit_matrix_bench examples/spy_fit_matrix_bench.cpp)
    target_link_libraries(spy_fit_matrix_bench PRIVATE atx::vol atx::core atx_warnings)

    # Dump the SPY Dec-2026 vol smile + bid/ask IV bands over z in [-2,2] (CSV on
    # stdout) for plotting. See examples/spy_dec_curve.cpp.
    add_executable(spy_dec_curve examples/spy_dec_curve.cpp)
    target_link_libraries(spy_dec_curve PRIVATE atx::vol atx::core atx_warnings)

    # Carry diagnostic: locate the put-call-consistent forward per expiry (repo's
    # own Andersen-Lake de-Am) vs the pipeline forward. See examples/spy_carry_diag.cpp.
    add_executable(spy_carry_diag examples/spy_carry_diag.cpp)
    target_link_libraries(spy_carry_diag PRIVATE atx::vol atx::core atx_warnings)

    # Out-of-sample honesty check: leave-every-other-strike-out generalization of
    # the convex-QP dense fit. See examples/spy_oos_check.cpp.
    add_executable(spy_oos_check examples/spy_oos_check.cpp)
    target_link_libraries(spy_oos_check PRIVATE atx::vol atx::core atx_warnings)

    # Breadth: one PricerFitter auto-selects the curve per underlying (index SPY
    # vs single-name XOM) with no per-symbol config. See examples/vol_breadth_bench.cpp.
    add_executable(vol_breadth_bench examples/vol_breadth_bench.cpp)
    target_link_libraries(vol_breadth_bench PRIVATE atx::vol atx::core atx_warnings)

    # Surface-archive (v3) deserialization throughput: fit -> serialize -> reload,
    # reporting ns/surface for single-symbol random access + full map_all, plus the
    # hardware vs table CRC-32C rate. See examples/surface_archive_bench.cpp.
    add_executable(surface_archive_bench examples/surface_archive_bench.cpp)
    target_link_libraries(surface_archive_bench PRIVATE atx::vol atx::core atx_warnings)

    # PricedSurface-native portfolio pricer + Taylor PnL-explain throughput: mixed
    # multi-underlying book, kernel-floor vs price() overhead, thread scaling +
    # determinism, and pnl_explain rate. See examples/portfolio_pricer_bench.cpp.
    add_executable(portfolio_pricer_bench examples/portfolio_pricer_bench.cpp)
    target_link_libraries(portfolio_pricer_bench PRIVATE atx::vol atx::core atx_warnings)

    # Backtest engine (Phase B3) worked examples on a synthetic corpus: build ->
    # run_backtest -> tearsheet headline metrics -> write_backtest_tsv. Both are
    # OFF-by-default demonstrations; the worked-example GATE lives in the test
    # suite (tests/tearsheet_test.cpp, self-contained). See examples/*.cpp.
    add_executable(atxvol_dispersion_backtest examples/dispersion_backtest.cpp)
    target_link_libraries(atxvol_dispersion_backtest PRIVATE atx::vol atx::core atx_warnings)

    add_executable(atxvol_strategy_examples examples/strategy_examples.cpp)
    target_link_libraries(atxvol_strategy_examples PRIVATE atx::vol atx::core atx_warnings)

    # Deliverable: short the 40-delta 6m SPY strangle, restriked every day
    # (2026-01-02 -> 2026-07-02). Builds a deterministic rolling-vol synthetic SPY
    # corpus, runs the backtest (timed), confirms the quote-slice CSV + ATXVSA
    # surface-archive stores, and prints a tearsheet + TSV table. The correctness
    # GATE is tests/spy_strangle_backtest_test.cpp. See examples/spy_strangle_backtest.cpp.
    add_executable(spy_strangle_backtest examples/spy_strangle_backtest.cpp)
    target_link_libraries(spy_strangle_backtest PRIVATE atx::vol atx::core atx_warnings)

    # Build a REAL SPY surface corpus from a pulled OPRA parquet hive
    # (databento_bulk_opra output): load_opra_daterange -> build_corpus -> ATXVSA
    # archives + manifest, which spy_strangle_backtest --manifest then backtests.
    add_executable(spy_ytd_corpus examples/spy_ytd_corpus.cpp)
    target_link_libraries(spy_ytd_corpus PRIVATE atx::vol atx::core atx_warnings)

    # Validation harness: re-run the SAME short 40d 6m strangle a second way — from
    # the RAW OPRA mid marks on actual listed contracts — and compare its P&L series
    # against the interpolated-from-fits backtest (with a bridge series that splits
    # the gap into strike/tenor granularity vs fit-vs-market fidelity).
    add_executable(spy_strangle_tradeable examples/spy_strangle_tradeable.cpp)
    target_link_libraries(spy_strangle_tradeable PRIVATE atx::vol atx::core atx_warnings)
endif()
