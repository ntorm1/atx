#pragma once

// atx-vol umbrella header — the single public entry point for the American-equity
// options pricing + volatility-analytics library.
//
// `#include "atx/vol/vol.hpp"` pulls in the whole public surface, grouped below
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
// The four tiers that are NOT here, and where to find them:
//
//   Tier-B  include/atx/vol/*.hpp  — public and includable, outside the freeze.
//           Advanced calibrators (svi/essvi/cstar/c8_calib), the SoA + SIMD
//           batch kernels (batch.hpp, american_batch.hpp, simd/), the
//           listed-dispersion domain vocabulary, OPRA hive/batch loaders,
//           harness panels and fixtures, and the earnings-reproduction harness.
//   detail  include/atx/vol/detail/ — internal machinery, no stability promise.
//   tools   tools/include/atx/vol/tools/  (target atx-vol-tools) — surface-db
//           CLI support, run-report writers, the tearsheet.
//   research research/include/atx/vol/research/ (target atx-vol-research) —
//           dispersion run orchestration and the run artifacts it emits.
//   tests   tests/support/ — test fixtures (analytics_fixture, opra_fixture,
//           spy_fit_fixture, breadth_fit_fixture). spy_fixture.hpp is NOT among
//           them any more: it was promoted to Tier-B include/atx/vol/.
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
#include "atx/vol/types.hpp"   // Side, ExerciseStyle, Result/Status, numeric bounds
#include "atx/vol/version.hpp" // library version string

// ── European primitives (Black-76 + Greeks + implied vol) ───────────────────
#include "atx/vol/black76.hpp"
#include "atx/vol/greeks.hpp"
#include "atx/vol/implied_vol.hpp"

// ── American pricing / IV + the cached hot path ─────────────────────────────
#include "atx/vol/american.hpp"    // Andersen-Lake / BAW, cached pricer, Greeks
#include "atx/vol/american_iv.hpp" // American -> implied-vol inversion
#include "atx/vol/correction.hpp"  // Chebyshev CorrectionCache (Black-76 + correction)

// ── Surfaces (hot-path evaluator + calibration-grade slice types) ───────────
#include "atx/vol/surface.hpp"     // Svi/Essvi slice params, svi_w/essvi_w
#include "atx/vol/vol_surface.hpp" // VolSurface, EssviParams/SviParams, evaluators

// ── Calibration vocabulary ──────────────────────────────────────────────────
//
// The per-family CALIBRATORS (svi_calib, essvi_calib, cstar/cstar_calib,
// c8_calib) are Tier-B — include them directly when you drive a family by hand.
// What is frozen here is the shared calibration vocabulary every fit speaks,
// plus the C8 curve family the arb validator and the curve registry name.
#include "atx/vol/c8.hpp"
#include "atx/vol/calib.hpp" // CalibOpts, FitObs/FitDiag, build_observations

// ── Static-arbitrage validators + repair ────────────────────────────────────
#include "atx/vol/arb.hpp"

// ── De-Americanization pipeline (divs -> borrow -> Euro-equiv IV -> fit) ─────
#include "atx/vol/deamer.hpp"      // de_americanize_chain, DeAmOptions
#include "atx/vol/dividend.hpp"    // hybrid forward + PCP borrow
#include "atx/vol/fit_metrics.hpp" // reduced-chi2 / error bars
#include "atx/vol/parity.hpp"      // re-Americanized fair-value-in-bid-ask

// ── Configurable curve family + auto-selection ──────────────────────────────
#include "atx/vol/curve_fit.hpp"      // fit_curve_surface (curve-agnostic driver)
#include "atx/vol/curve_selector.hpp" // select_curve (out-of-sample curve/config search)
#include "atx/vol/dense_slice.hpp"    // densified convex slice fit
#include "atx/vol/fit_policy.hpp"     // profile/session/event -> effective preset + curve
#include "atx/vol/spline_curve.hpp"   // SpiderRock-parity cubic-spline curve
#include "atx/vol/vol_curve.hpp"      // IVolCurve family, CurveSurface, CurveConfig, VolCurveKind

// ── Whole-surface build + the composable session facade ─────────────────────
#include "atx/vol/chain.hpp"          // OptionChain, OptionId (unique-id chain handle)
#include "atx/vol/market_env.hpp"     // MarketEnv (spot / rate-curve / divs / valuation ts)
#include "atx/vol/pricer_fitter.hpp"  // PricerFitter, FittedSurface, OutputField, ChainValuation
#include "atx/vol/profile.hpp"        // board profile -> routing features
#include "atx/vol/session.hpp"        // VolaSession, SessionInputs, FitPreset
#include "atx/vol/surface_parity.hpp" // run_surface_parity, CalendarRepair
#include "atx/vol/surface_policy.hpp" // risk-admission policy (archive + db gate)

// ── Surface queries / projection / derivatives ──────────────────────────────
#include "atx/vol/contract_projection.hpp" // relative template -> concrete theo option
#include "atx/vol/derivatives.hpp"         // vol-derivative analytics
#include "atx/vol/projection.hpp"          // eval at non-listed T/K, delta anchors
#include "atx/vol/query_pricing.hpp"       // the query knobs every priced path takes
#include "atx/vol/rates_curve.hpp"         // CurveSet, DividendEvent

// ── Data model (universe, corpus, real OPRA loader, archive, db) ────────────
#include "atx/vol/corpus.hpp"              // date x symbol fitted-surface corpus
#include "atx/vol/data.hpp"                // QuoteFrame, data_install
#include "atx/vol/opra_panel.hpp"          // real Databento OPRA cbbo loader
#include "atx/vol/priced_surface.hpp"      // PricedSurface (serialization-ready priced surface)
#include "atx/vol/priced_surface_view.hpp" // zero-copy view over archived bytes
#include "atx/vol/surface_archive.hpp"     // fitted priced-surface archive (ATXVSA2)
#include "atx/vol/surface_db.hpp"          // partitioned surface database
#include "atx/vol/universe.hpp"            // Universe, Underlying, Chain, Uid

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
#include "atx/vol/adjusted_greeks.hpp"  // AdjustedGreeks (named in the pricer's API)
#include "atx/vol/pnl_attribution.hpp"  // spot/vol-axis P&L attribution over the book
#include "atx/vol/portfolio_pricer.hpp" // PricedSurface-native pricer + Taylor PnL explain
#include "atx/vol/scenario_grid.hpp"    // 2-D spot x vol scenario P&L matrix

// ── Backtest engine + strategy vocabulary ───────────────────────────────────
//
// The dispersion RUN ORCHESTRATION (run/workflow/pipeline drivers, run archive)
// is not here — it is atx-vol-research. What is frozen is the engine and the
// vocabulary a strategy is written in.
#include "atx/vol/backtest.hpp"            // Clock, RunConfig, run_backtest, BacktestResult
#include "atx/vol/dispersion.hpp"          // dispersion book / basket vocabulary
#include "atx/vol/dispersion_strangle.hpp" // strangle DSL over the dispersion book
#include "atx/vol/strategy.hpp"            // StrategySpec, LifecycleSpec, resolve_strike_by_delta

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
#include "atx/vol/analytics.hpp"        // SurfaceAnalytics, RND/BKM, var swap, implied corr
#include "atx/vol/earnings_term_fit.hpp" // joint {eMove, st, lt, decay} censored term fit
#include "atx/vol/event_vol.hpp"        // EventSchedule, censoring, implied_emove_joint
#include "atx/vol/sr_tenor_grid.hpp"    // SpiderRock 12-point native tenor grid
#include "atx/vol/vol_time.hpp"         // hybrid business/vol-time clock
