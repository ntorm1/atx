#pragma once

// atx-vol umbrella header — the single public entry point for the American-equity
// options pricing + volatility-analytics library.
//
// `#include "atx/vol/api/vol.hpp"` pulls in the whole public surface, grouped below
// by role. Prefer the individual headers in a translation unit that needs only a
// slice of the library (faster builds); reach for this umbrella in application
// code, notebooks, and examples where ergonomics beat compile time.
//
// ── 10-line quickstart (build a surface from a chain, then query it) ─────────
//
//   using namespace atx::vol;
//   // 1. Load a real OPRA cbbo-1m NBBO slice (or build a QuoteFrame yourself).
//   OpraLoadSpec spec{ .path = "xom.parquet", .underlying = "XOM",
//                      .snapshot_iso = "2026-06-05T19:55:00Z", .r = 0.043 };
//   OpraPanel panel = load_opra_cbbo_parquet(spec).value();
//   // 2. Pick a fit preset (Robust = calendar-arb-free near-money, held quality).
//   SessionInputs in = make_session_inputs(FitPreset::Robust, panel.implied_spot,
//                                          0.043, panel.frame.snapshot_ts_ns);
//   // 3. Build once; then query cheaply with no refit.
//   VolaSession s = VolaSession::from_frame(panel.frame, in).value();
//   double iv   = s.iv(150.0, 0.25);                          // Euro-equiv IV
//   double fv   = s.fair_value(150.0, 0.25, Side::Call).value();  // American model px
//   AmericanGreeks g = s.greeks(150.0, 0.25, Side::Call).value();
//   const SessionDiagnostics& d = s.diagnostics();            // fit quality + arb status
//
// Thread-safety and error-handling conventions are per-header; the library-wide
// contract is: every entry is a pure function of its inputs (Result<T>/Status for
// expected failures, no exceptions), and a built VolaSession / VolSurface is safe
// for concurrent const queries. See README.md for the full tour.
//
// ── THE canonical lifecycle (chain -> fit -> priced surface -> archive -> book) ─
//
// One blessed path takes a listed board all the way to a portfolio mark + PnL.
// Every hand-off below is exercised end-to-end (bit-identically) in
// tests/lifecycle_integration_test.cpp; new code should follow this shape.
//
//   using namespace atx::vol;
//   // 1. Chain: an id-addressed board + its MarketEnv (spot / rate / cash divs).
//   OptionChain chain = OptionChain::from_frame(frame,
//       MarketEnv::flat(spot, r, now_ns, cash_divs)).value();
//   // 2. Fit: zero-config auto policy (dense ETF -> HFT linear variance;
//   //    sparse/event/vol-product boards route by profile and board features).
//   PricerFitter fitter{ PricerConfig{} };
//   fitter.fit(chain);
//   const VolaSession& sess = fitter.surface()->session();
//   // 3. Snapshot: distil the live fit into a small, cache-free PricedSurface —
//   //    its cold served theo is bit-identical to the session (override path).
//   PricedSurface ps = sess.to_priced_surface().value();
//   // 4. Archive: serialize -> reload with ZERO theo drift (ATXVSA2 round-trip).
//   auto bytes = write_surface_archive_v2(std::array{SurfaceArchiveItem{"SPY", &ps}}).value();
//   PricedSurface reloaded = SurfaceArchiveV2::open(std::move(bytes)).value()
//                                .reconstruct_symbol("SPY").value();
//   // 5. Book: dedup contracts, mark the portfolio, and Taylor-explain a reprice.
//   Portfolio pf = Portfolio::create(positions).value();               // uid == ps.uid()
//   PortfolioPricer pricer{ std::move(pf) };
//   SurfaceSet surfaces = SurfaceSet::create(std::array{ &reloaded }).value();
//   PriceFrame frame_out = pricer.price(surfaces).value();             // pv + Greeks
//   PnlFrame   explain   = pricer.pnl_explain(base, shifted).value();  // delta/gamma/...
//
// CANONICAL portfolio engine: portfolio_pricer.hpp (the PricedSurface-native
// PortfolioPricer above) — and, since S4-T22 (plan 4.5), the library's ONLY
// portfolio engine. The legacy VolSurface/Universe-bound C-port pair that used
// to sit beside it was DELETED rather than shipped deprecated: v1 ships nothing
// deprecated. Its scenario and attribution capabilities are on this stack as
// `scenario_grid.hpp` and `pnl_attribution.hpp`.
//
// ── WHAT THIS FILE IS, EXACTLY (S4-T18 / plan 3.8 + 4.1) ─────────────────────
//
// This umbrella is the TIER-A SET and nothing else: the API atx-vol is willing
// to freeze for v1. That is a contract, not a convention —
// `tests/vol_umbrella_test.cpp` parses the include list below and fails if it
// drifts from the Tier-A manifest in either direction, if it reaches into a
// non-shipped tier, or if a Tier-A header acquires a non-Tier-A dependency.
// Add an include here only by promoting a header to Tier-A in that manifest.
//
// Tier-A is CLOSED UNDER INCLUSION: if a header listed below includes another
// `atx/vol/` header, that one is listed too. So a few entries are here because
// they are named in a frozen signature rather than because callers reach for
// them directly (query_pricing.hpp, adjusted_greeks.hpp, priced_surface_view.hpp,
// surface_policy.hpp). Freezing a signature freezes its vocabulary; hiding that
// would make the promise unenforceable.
//
// The tiers that are NOT here, and where to find them (api-restructure,
// 2026-08-14: the public surface is include/atx/vol/api/, an 8-module tree —
// analytics, backtest, core, fitting, marketdata, pricing, simd, storage —
// plus this umbrella; the flat include/atx/vol/*.hpp + detail/ layout it used
// to be is gone, and everything not shipped now lives under src/<module>/,
// off the include/ tree entirely rather than in an installed-but-unstable
// detail/ tier):
//
//   Tier-B  include/atx/vol/api/<module>/*.hpp — public and includable,
//           outside the freeze. Advanced calibrators (svi/essvi/cstar/c8_calib
//           are PRIVATE now — see below; c8.hpp's curve-family type and
//           registry name stay Tier-A), the SoA + SIMD batch kernels
//           (batch.hpp, american_batch.hpp, api/simd/), the listed-dispersion
//           domain vocabulary, OPRA hive/batch loaders, harness panels and
//           fixtures, and the earnings-reproduction harness.
//   alpha   include/atx/vol/alpha/*.hpp — a SECOND public include root, also
//           Tier-B and also outside the freeze: the header-only alpha layer
//           (spec / registry / schema / frame / compute / strategy / audit)
//           behind the atx-vol-alpha-audit and atx-vol-longvega CLIs. It is
//           NOT under api/ and is deliberately NOT in the umbrella, but it IS
//           installed (cmake/atx-vol-install.cmake) and therefore reachable
//           out of tree. L7-T1, 2026-08-23: this entry exists because the
//           layer had been on the public BUILD_INTERFACE include path with no
//           install rule at all, so it compiled in-tree and was a hard
//           `file not found` downstream while this manifest said nothing.
//   private src/<module>/ — internal machinery: no include/ path, no
//           stability promise, not installed. This is where the per-family
//           calibrators live now (svi_calib.hpp, essvi_calib.hpp, cstar.hpp,
//           cstar_calib.hpp, c8_calib.hpp all in src/fitting/), alongside
//           spy_fixture.hpp (src/fitting/spy_fixture.hpp — a shared
//           tests/examples/bench fixture, never shipped).
//   tools   tools/include/atx/vol/tools/  (target atx-vol-tools) — surface-db
//           CLI support, run-report writers, the tearsheet.
//   research research/include/atx/vol/research/ (target atx-vol-research) —
//           dispersion run orchestration and the run artifacts it emits.
//   tests   tests/support/ — test fixtures, among them analytics_fixture,
//           opra_fixture, spy_fit_fixture and breadth_fit_fixture.
//
// Consumers should only ever include from api/ — the module headers above and
// this umbrella — or, for the alpha layer, from alpha/. Nothing under src/,
// tools/, research/ or tests/support/ is a stable, or even reachable, public
// entry point.
//
// ── Coordinate + pricing conventions (used everywhere in the library) ──────────
//
//   * Log-moneyness  k = ln(K / F): every slice is stored / queried in k, so an
//     absolute-strike query first resolves the term forward F(T).
//   * Forward / carry  F = S * e^{(r - q) T}, with discrete cash dividends folded
//     into the forward (q is the effective continuous carry, q_eff). A query at T
//     interpolates log-forward/discount state between pillars and holds endpoint
//     carry flat in the tails — one coherent carry mechanic.
//   * Time  T is a year-fraction on a 365.25-day year (data.cpp `year_fraction`).
//   * Greeks are SPOT-based: delta = dP/dS, gamma = d2P/dS2; theta is CALENDAR,
//     theta = dP/dt = -dP/dT. vega = dP/dsigma, rho = dP/dr.
//   * Price basis is the American Andersen-Lake mark; on the cold (index / override)
//     path the Greeks are American cold finite differences on that SAME mark, so
//     `greeks().price` is bit-identical to `fair_value()`.

// ── Core vocabulary ─────────────────────────────────────────────────────────
#include "atx/vol/api/core/types.hpp"   // Side, ExerciseStyle, Result/Status, numeric bounds
#include "atx/vol/api/core/version.hpp" // library version string

// ── European primitives (Black-76 + Greeks + implied vol) ───────────────────
#include "atx/vol/api/pricing/black76.hpp"
#include "atx/vol/api/pricing/greeks.hpp"
#include "atx/vol/api/pricing/implied_vol.hpp"

// ── American pricing / IV + the cached hot path ─────────────────────────────
#include "atx/vol/api/pricing/american.hpp"    // Andersen-Lake / BAW, cached pricer, Greeks
#include "atx/vol/api/pricing/american_iv.hpp" // American -> implied-vol inversion
#include "atx/vol/api/fitting/correction.hpp"  // Chebyshev CorrectionCache (Black-76 + correction)

// ── Surfaces (hot-path evaluator + calibration-grade slice types) ───────────
#include "atx/vol/api/fitting/surface.hpp"     // Svi/Essvi slice params, svi_w/essvi_w
#include "atx/vol/api/fitting/vol_surface.hpp" // VolSurface, EssviParams/SviParams, evaluators

// ── Calibration vocabulary ──────────────────────────────────────────────────
//
// The per-family CALIBRATORS (svi_calib, essvi_calib, cstar/cstar_calib,
// c8_calib) are PRIVATE (src/fitting/) as of the api-restructure — driving a
// family fitter by hand is internal-code work, not a public entry point; a
// consumer that needs curve-family control uses fit_policy.hpp /
// curve_selector.hpp / curve_fit.hpp instead. What is frozen here is the
// shared calibration vocabulary every fit speaks (calib.hpp), plus the C8
// curve family's public type, the arb validator and the curve registry name
// (c8.hpp — the type, distinct from the now-private c8_calib.hpp fitter).
#include "atx/vol/api/fitting/c8.hpp"
#include "atx/vol/api/fitting/calib.hpp" // CalibOpts, FitObs/FitDiag, build_observations

// ── Static-arbitrage validators + repair ────────────────────────────────────
#include "atx/vol/api/fitting/arb.hpp"

// ── De-Americanization pipeline (divs -> borrow -> Euro-equiv IV -> fit) ─────
#include "atx/vol/api/fitting/deamer.hpp"      // de_americanize_chain, DeAmOptions
#include "atx/vol/api/pricing/dividend.hpp"    // hybrid forward + PCP borrow
#include "atx/vol/api/fitting/fit_metrics.hpp" // reduced-chi2 / error bars
#include "atx/vol/api/fitting/parity.hpp"      // re-Americanized fair-value-in-bid-ask

// ── Configurable curve family + auto-selection ──────────────────────────────
#include "atx/vol/api/fitting/curve_fit.hpp"      // fit_curve_surface (curve-agnostic driver)
#include "atx/vol/api/fitting/curve_selector.hpp" // select_curve (out-of-sample curve/config search)
#include "atx/vol/api/fitting/dense_slice.hpp"    // densified convex slice fit
#include "atx/vol/api/fitting/fit_policy.hpp"     // profile/session/event -> effective preset + curve
#include "atx/vol/api/fitting/spline_curve.hpp"   // SpiderRock-parity cubic-spline curve
#include "atx/vol/api/fitting/vol_curve.hpp"      // IVolCurve family, CurveSurface, CurveConfig, VolCurveKind

// ── Whole-surface build + the composable session facade ─────────────────────
#include "atx/vol/api/core/chain.hpp"          // OptionChain, OptionId (unique-id chain handle)
#include "atx/vol/api/core/market_env.hpp"     // MarketEnv (spot / rate-curve / divs / valuation ts)
#include "atx/vol/api/fitting/pricer_fitter.hpp"  // PricerFitter, FittedSurface, OutputField, ChainValuation
#include "atx/vol/api/fitting/profile.hpp"        // board profile -> routing features
#include "atx/vol/api/fitting/session.hpp"        // VolaSession, SessionInputs, FitPreset
#include "atx/vol/api/fitting/surface_parity.hpp" // run_surface_parity, CalendarRepair
#include "atx/vol/api/fitting/surface_policy.hpp" // risk-admission policy (archive + db gate)

// ── Surface queries / projection / derivatives ──────────────────────────────
#include "atx/vol/api/analytics/contract_projection.hpp" // relative template -> concrete theo option
#include "atx/vol/api/pricing/derivatives.hpp"         // vol-derivative analytics
#include "atx/vol/api/fitting/projection.hpp"          // eval at non-listed T/K, delta anchors
#include "atx/vol/api/backtest/query_pricing.hpp"       // the query knobs every priced path takes
#include "atx/vol/api/pricing/rates_curve.hpp"         // CurveSet, DividendEvent

// ── Data model (universe, corpus, real OPRA loader, archive, db) ────────────
#include "atx/vol/api/marketdata/corpus.hpp"              // date x symbol fitted-surface corpus
#include "atx/vol/api/marketdata/data.hpp"                // QuoteFrame, data_install
#include "atx/vol/api/marketdata/opra_panel.hpp"          // real Databento OPRA cbbo loader
#include "atx/vol/api/backtest/priced_surface.hpp"      // PricedSurface (serialization-ready priced surface)
#include "atx/vol/api/backtest/priced_surface_view.hpp" // zero-copy view over archived bytes
#include "atx/vol/api/storage/surface_archive.hpp"     // fitted priced-surface archive (ATXVSA2)
#include "atx/vol/api/storage/surface_db.hpp"          // partitioned surface database
#include "atx/vol/api/marketdata/universe.hpp"            // Universe, Underlying, Chain, Uid

// ── Portfolio / risk analytics ──────────────────────────────────────────────
//
// The sole portfolio path: dedup + American mark + American cold-FD Greeks +
// Taylor PnL-explain over N underlyings, with by-underlier / by-expiry
// aggregation via `reduce_risk_buckets` / `reduce_pnl_risk_buckets`. Its
// scenario and attribution post-processes are `scenario_grid.hpp` (2-D
// spot x vol P&L matrix) and `pnl_attribution.hpp` (the level/skew/curvature
// vol split); relative option templates resolve to concrete contracts through
// `contract_projection.hpp`.
//
// S4-T22 promoted the two post-processes into Tier-A. They were the Sprint-3
// "fold-or-keep" leftovers — test/bench-consumed only — but deleting the legacy
// engine made them the library's ONLY scenario and attribution risk, and a
// frozen portfolio API that cannot reach either through the one include would
// be freezing the wrong shape. Both are pure serial post-processes over frozen
// vocabulary (Position / SurfaceSet / AmericanGreeks) and add no dependency.
#include "atx/vol/api/pricing/adjusted_greeks.hpp"  // AdjustedGreeks (named in the pricer's API)
#include "atx/vol/api/backtest/deriv_book.hpp"       // portfolio-layer swap-book pricing against a SurfaceSet
#include "atx/vol/api/analytics/pnl_attribution.hpp"  // spot/vol-axis P&L attribution over the book
#include "atx/vol/api/backtest/portfolio_pricer.hpp" // PricedSurface-native pricer + Taylor PnL explain
#include "atx/vol/api/analytics/scenario_grid.hpp"    // 2-D spot x vol scenario P&L matrix

// ── Backtest engine + strategy vocabulary ───────────────────────────────────
//
// The dispersion RUN ORCHESTRATION (run/workflow/pipeline drivers, run archive)
// is not here — it is atx-vol-research. What is frozen is the engine and the
// vocabulary a strategy is written in.
#include "atx/vol/api/backtest/backtest.hpp"            // Clock, RunConfig, run_backtest, BacktestResult
#include "atx/vol/api/backtest/dispersion.hpp"          // dispersion book / basket vocabulary
#include "atx/vol/api/backtest/dispersion_strangle.hpp" // strangle DSL over the dispersion book
#include "atx/vol/api/backtest/strategy.hpp"            // StrategySpec, LifecycleSpec, resolve_strike_by_delta
#include "atx/vol/api/pricing/swap_leg.hpp"            // swap_contract_for_lot, SwapSignalProbe

// ── Surface analytics (E5 / AN-W) ───────────────────────────────────────────
//
// The analytics flagship — `compute_surface_analytics` (ATMF term structure,
// delta wings / RR / BF, skew & curvature, forward vol), the
// closed-form-validated density stack (Breeden-Litzenberger RND, implied
// CDF/quantiles, BKM moments, the OTM log-strip MFIV / variance swap), the
// implied-correlation helpers, and the SpiderRock-parity earnings-censored ATM
// pipeline.
//
// These were absent from the umbrella before E5, which made the layer
// effectively unshippable through the one-include public API: a caller who
// included `atx/vol/vol.hpp` could not name `AnalyticsConfig`,
// `compute_surface_analytics`, `risk_neutral_density`, `EventSchedule`,
// `fit_earnings_term` or the vol-time clock at all. `vol_umbrella_test.cpp`
// names a symbol from EACH of the headers below, so dropping one fails to
// COMPILE instead of silently shrinking the public surface again.
//
// Delta-convention note (AN-P2-6): `analytics.hpp` documents the three
// "25-delta strike" conventions that exist in this library and exposes the
// choice as `AnalyticsConfig::delta_convention` (`DeltaConvention`,
// single-sourced from `projection.hpp`). It defaults to `American` — the
// shipped behaviour.
#include "atx/vol/api/analytics/analytics.hpp"        // SurfaceAnalytics, RND/BKM, var swap, implied corr
#include "atx/vol/api/analytics/earnings_term_fit.hpp" // joint {eMove, st, lt, decay} censored term fit
#include "atx/vol/api/analytics/event_vol.hpp"        // EventSchedule, censoring, implied_emove_joint
#include "atx/vol/api/fitting/sr_tenor_grid.hpp"    // SpiderRock 12-point native tenor grid
#include "atx/vol/api/core/vol_time.hpp"         // hybrid business/vol-time clock
