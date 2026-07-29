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
//   // 4. Archive: serialize -> reload with ZERO theo drift (ATXVSA v3 round-trip).
//   auto bytes = write_surface_archive(std::array{SurfaceArchiveItem{"SPY", &ps}}).value();
//   PricedSurface reloaded = SurfaceArchive::open(std::move(bytes)).value()
//                                .map_symbol("SPY").value();
//   // 5. Book: dedup contracts, mark the portfolio, and Taylor-explain a reprice.
//   Portfolio pf = Portfolio::create(positions).value();               // uid == ps.uid()
//   PortfolioPricer pricer{ std::move(pf) };
//   SurfaceSet surfaces = SurfaceSet::create(std::array{ &reloaded }).value();
//   PriceFrame frame_out = pricer.price(surfaces).value();             // pv + Greeks
//   PnlFrame   explain   = pricer.pnl_explain(base, shifted).value();  // delta/gamma/...
//
// CANONICAL portfolio engine: portfolio_pricer.hpp (the PricedSurface-native
// PortfolioPricer above). `portfolio.hpp` / `portfolio_risk.hpp` are the
// DEPRECATED legacy VolSurface/Universe-bound engine (banner-marked below); do not
// build new features on them.
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

// ── SoA batch / vectorized kernels ──────────────────────────────────────────
#include "atx/vol/batch.hpp"

// ── Surfaces (hot-path evaluator + calibration-grade slice types) ───────────
#include "atx/vol/surface.hpp"     // Surface<>, Svi/Essvi slice, svi_w/essvi_w
#include "atx/vol/vol_surface.hpp" // VolSurface, EssviParams/SviParams, evaluators

// ── Calibration families ────────────────────────────────────────────────────
#include "atx/vol/c8.hpp"
#include "atx/vol/c8_calib.hpp"
#include "atx/vol/calib.hpp" // CalibOpts, FitObs/FitDiag, build_observations
#include "atx/vol/cstar.hpp"
#include "atx/vol/cstar_calib.hpp"
#include "atx/vol/essvi_calib.hpp" // eSSVI per-slice + surface drivers
#include "atx/vol/svi_calib.hpp"

// ── Static-arbitrage validators + repair ────────────────────────────────────
#include "atx/vol/arb.hpp"

// ── De-Americanization pipeline (divs -> borrow -> Euro-equiv IV -> fit) ─────
#include "atx/vol/deamer.hpp"      // de_americanize_chain, DeAmOptions
#include "atx/vol/dividend.hpp"    // hybrid forward + PCP borrow
#include "atx/vol/fit_metrics.hpp" // reduced-chi2 / error bars
#include "atx/vol/parity.hpp"      // re-Americanized fair-value-in-bid-ask
#include "atx/vol/s3.hpp"          // S3/SSVI shape reference

// ── Configurable curve family + auto-selection ──────────────────────────────
#include "atx/vol/curve_fit.hpp"      // fit_curve_surface (curve-agnostic driver)
#include "atx/vol/curve_selector.hpp" // select_curve (out-of-sample curve/config search)
#include "atx/vol/fit_policy.hpp"     // profile/session/event -> effective preset + curve
#include "atx/vol/vol_curve.hpp"      // IVolCurve family, CurveSurface, CurveConfig, VolCurveKind

// ── Whole-surface build + the composable session facade ─────────────────────
#include "atx/vol/chain.hpp"          // OptionChain, OptionId (unique-id chain handle)
#include "atx/vol/market_env.hpp"     // MarketEnv (spot / rate-curve / divs / valuation ts)
#include "atx/vol/pricer_fitter.hpp"  // PricerFitter, FittedSurface, OutputField, ChainValuation
#include "atx/vol/session.hpp"        // VolaSession, SessionInputs, FitPreset
#include "atx/vol/surface_parity.hpp" // run_surface_parity, CalendarRepair
#include "atx/vol/vola_parity.hpp"    // single-expiry parity harness

// ── Surface queries / projection / derivatives ──────────────────────────────
#include "atx/vol/contract_projection.hpp" // relative template -> concrete theo option
#include "atx/vol/historical_projection.hpp" // historical relative-template risk / VaR
#include "atx/vol/phase_profile.hpp" // compile-time opt-in phase timers
#include "atx/vol/curve.hpp"               // CurveSet, DividendEvent
#include "atx/vol/derivatives.hpp"         // vol-derivative analytics
#include "atx/vol/projection.hpp"          // eval at non-listed T/K, delta anchors

// ── Data model (universe, panels, real OPRA loader, archive) ─────────────────
#include "atx/vol/data.hpp"            // QuoteFrame, data_install
#include "atx/vol/listed_opra.hpp"     // strict listed-contract definition join
#include "atx/vol/occ_ess.hpp"         // OCC non-standard deliverable authority
#include "atx/vol/opra_panel.hpp"      // real Databento OPRA cbbo loader
#include "atx/vol/panel.hpp"           // synthetic + CSV panels
#include "atx/vol/priced_surface.hpp"  // PricedSurface (serialization-ready priced surface)
#include "atx/vol/spy_fixture.hpp"     // deterministic SPY index known-truth fixture
#include "atx/vol/surface_archive.hpp" // fitted priced-surface archive (v3)
#include "atx/vol/universe.hpp"        // Universe, Underlying, Chain, Uid

// ── Portfolio / risk analytics ──────────────────────────────────────────────
//
// CANONICAL portfolio path: `portfolio_pricer.hpp` (PricedSurface-native
// `PortfolioPricer` — dedup + American mark + American cold-FD Greeks + Taylor
// PnL-explain over N underlyings). New code should use it.
//
// The `portfolio.hpp` / `portfolio_risk.hpp` pair below is the DEPRECATED legacy
// VolSurface/Universe-bound engine. It is retained (not deleted) only because it
// still carries capabilities the canonical pricer does not yet cover — stock/cash
// legs, by-uid/by-expiry/by-group aggregation, chain-moneyness/strike bulk
// selection, the multi-shock scenario engine, theoretical/delta-coordinate legs,
// and forward/vol/route/interp factor PnL attribution. Do not build new features
// on it; migrate those capabilities onto the PricedSurface path as they are needed.
#include "atx/vol/portfolio.hpp"        // DEPRECATED legacy VolSurface-bound portfolio + bulk
#include "atx/vol/portfolio_pricer.hpp" // CANONICAL PricedSurface-native pricer + Taylor PnL explain
#include "atx/vol/portfolio_risk.hpp"   // DEPRECATED legacy scenario / theoretical-leg risk
#include "atx/vol/profile.hpp"

// Traditional listed-options dispersion workflow.
#include "atx/vol/backtest.hpp"
#include "atx/vol/dispersion.hpp"
#include "atx/vol/dispersion_backtest.hpp"
#include "atx/vol/dispersion_strangle.hpp" // strangle DSL over the dispersion book
#include "atx/vol/dispersion_workflow.hpp"
#include "atx/vol/listed_dispersion.hpp"
#include "atx/vol/listed_dispersion_reconciliation.hpp"
#include "atx/vol/listed_dispersion_schedule.hpp"
#include "atx/vol/listed_dispersion_strategy.hpp"
#include "atx/vol/strategy.hpp" // StrategySpec, LifecycleSpec, resolve_strike_by_delta

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
// `fit_earnings_term` or the vol-time clock at all. `vol_umbrella_test.cpp` now
// names a symbol from EACH of the headers below, so dropping one fails a test
// instead of silently shrinking the public surface again.
//
// Delta-convention note (AN-P2-6): `analytics.hpp` documents the three
// "25-delta strike" conventions that exist in this library and exposes the
// choice as `AnalyticsConfig::delta_convention` (`DeltaConvention`,
// single-sourced from `projection.hpp`). It defaults to `American` — the
// shipped behaviour.
#include "atx/vol/analytics.hpp"         // SurfaceAnalytics, RND/BKM, var swap, implied corr
#include "atx/vol/dense_slice.hpp"       // densified convex slice fit
#include "atx/vol/earnings_term_fit.hpp" // joint {eMove, st, lt, decay} censored term fit
#include "atx/vol/event_vol.hpp"         // EventSchedule, censoring, implied_emove_joint
#include "atx/vol/sr_tenor_grid.hpp"     // SpiderRock 12-point native tenor grid
#include "atx/vol/vol_time.hpp"          // hybrid business/vol-time clock

// ── Reporting artifacts ─────────────────────────────────────────────────────
#include "atx/vol/run_report.hpp" // run-directory metric / series writers
#include "atx/vol/tearsheet.hpp"  // TearSheet performance summary (+ benchmark-relative)
