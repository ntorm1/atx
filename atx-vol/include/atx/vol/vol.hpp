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

// ── Core vocabulary ─────────────────────────────────────────────────────────
#include "atx/vol/types.hpp"    // Side, ExerciseStyle, Result/Status, numeric bounds
#include "atx/vol/version.hpp"  // library version string

// ── European primitives (Black-76 + Greeks + implied vol) ───────────────────
#include "atx/vol/black76.hpp"
#include "atx/vol/greeks.hpp"
#include "atx/vol/implied_vol.hpp"

// ── American pricing / IV + the cached hot path ─────────────────────────────
#include "atx/vol/american.hpp"     // Andersen-Lake / BAW, cached pricer, Greeks
#include "atx/vol/american_iv.hpp"  // American -> implied-vol inversion
#include "atx/vol/correction.hpp"   // Chebyshev CorrectionCache (Black-76 + correction)

// ── SoA batch / vectorized kernels ──────────────────────────────────────────
#include "atx/vol/batch.hpp"

// ── Surfaces (hot-path evaluator + calibration-grade slice types) ───────────
#include "atx/vol/surface.hpp"      // Surface<>, Svi/Essvi slice, svi_w/essvi_w
#include "atx/vol/vol_surface.hpp"  // VolSurface, EssviParams/SviParams, evaluators

// ── Calibration families ────────────────────────────────────────────────────
#include "atx/vol/calib.hpp"        // CalibOpts, FitObs/FitDiag, build_observations
#include "atx/vol/essvi_calib.hpp"  // eSSVI per-slice + surface drivers
#include "atx/vol/svi_calib.hpp"
#include "atx/vol/c8.hpp"
#include "atx/vol/c8_calib.hpp"
#include "atx/vol/cstar.hpp"
#include "atx/vol/cstar_calib.hpp"

// ── Static-arbitrage validators + repair ────────────────────────────────────
#include "atx/vol/arb.hpp"

// ── De-Americanization pipeline (divs -> borrow -> Euro-equiv IV -> fit) ─────
#include "atx/vol/dividend.hpp"     // hybrid forward + PCP borrow
#include "atx/vol/s3.hpp"           // S3/SSVI shape reference
#include "atx/vol/deamer.hpp"       // de_americanize_chain, DeAmOptions
#include "atx/vol/fit_metrics.hpp"  // reduced-chi2 / error bars
#include "atx/vol/parity.hpp"       // re-Americanized fair-value-in-bid-ask

// ── Whole-surface build + the composable session facade ─────────────────────
#include "atx/vol/vola_parity.hpp"     // single-expiry parity harness
#include "atx/vol/surface_parity.hpp"  // run_surface_parity, CalendarRepair
#include "atx/vol/session.hpp"         // VolaSession, SessionInputs, FitPreset

// ── Surface queries / projection / derivatives ──────────────────────────────
#include "atx/vol/projection.hpp"   // eval at non-listed T/K, delta anchors
#include "atx/vol/derivatives.hpp"  // vol-derivative analytics
#include "atx/vol/curve.hpp"        // CurveSet, DividendEvent

// ── Data model (universe, panels, real OPRA loader, archive) ─────────────────
#include "atx/vol/universe.hpp"        // Universe, Underlying, Chain, Uid
#include "atx/vol/data.hpp"            // QuoteFrame, data_install
#include "atx/vol/panel.hpp"           // synthetic + CSV panels
#include "atx/vol/opra_panel.hpp"      // real Databento OPRA cbbo loader
#include "atx/vol/surface_archive.hpp" // fitted-surface archive

// ── Portfolio / risk analytics ──────────────────────────────────────────────
#include "atx/vol/portfolio.hpp"
#include "atx/vol/portfolio_risk.hpp"
#include "atx/vol/calib_pool.hpp"
#include "atx/vol/profile.hpp"
