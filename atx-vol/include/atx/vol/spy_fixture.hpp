#pragma once

// A canonical, deterministic **SPY-like index option surface** fixture.
//
// atx-vol proves American-equity parity on a REAL Databento OPRA single-name
// slice (XOM; see opra_panel.hpp / examples/opra_parity_bench). There is no
// cached index-option data and no API budget to pull one, so this header builds
// a realistic *synthetic* SPY surface from the public known-truth generator
// (`make_synthetic_american_panel`, panel.hpp): a broad, dense strike ladder
// over six weekly/monthly expiries with an index-style term structure and a
// steep, tenor-decaying put skew, priced American on an arbitrage-free S3 truth
// smile with tight (index-liquid) bid-ask spreads and two discrete cash
// dividends inside the year.
//
// Because the truth smile is known, this fixture doubles as an ACCURACY oracle:
// a fit that recovers it lands its ATM vol on `truth.sigma0` and its fair values
// inside the (tight) synthetic bid-ask. It is header-only and depends only on
// the public API, so tests and the SPY benchmark share one definition.
//
// The numbers are representative of a calm-regime SPY (spot ~600, ATM vol ~10%
// front rising to ~16% at 1y, quarterly ~$1.8 dividends), NOT a capture of any
// specific session — this is a fixture, not market data.

#include <cmath>
#include <string>
#include <vector>

#include "atx/vol/curve.hpp"    // DividendEvent
#include "atx/vol/data.hpp"     // iso_to_ns, year_fraction
#include "atx/vol/panel.hpp"    // SynthPanelSpec, SynthExpiry
#include "atx/vol/s3.hpp"       // S3Params
#include "atx/vol/session.hpp"  // SessionInputs, FitPreset, make/apply preset

namespace atx::vol {

// Build the SPY-like known-truth panel spec. `snapshot` anchors the valuation
// date; every expiry's year-fraction is derived from it via `year_fraction`.
[[nodiscard]] inline SynthPanelSpec make_spy_synthetic_spec(
    const std::string& snapshot = "2026-06-19") {
  SynthPanelSpec spec;
  spec.uid = "SPY";
  spec.snapshot_iso = snapshot;
  spec.spot = 600.0;
  spec.r = 0.043;    // matches the XOM slice's rate era
  spec.borrow = 0.0;  // an index ETF: carry comes from the cash dividends below

  // Two discrete cash dividends inside the year (quarterly ~ $1.8, ex-div on the
  // usual Mar/Jun/Sep/Dec cadence — the ones that fall in the fitted window).
  DividendEvent d0;
  d0.ex_date_ns = iso_to_ns("2026-09-18");
  d0.amount = 1.75;
  DividendEvent d1;
  d1.ex_date_ns = iso_to_ns("2026-12-18");
  d1.amount = 1.80;
  spec.cash_divs = {d0, d1};

  // Six expiries: 1w, 1m, 2m, 3m, 6m, 1y. Index term structure: ATM vol rises
  // with tenor (calm contango). The skew is specified in STRIKE space, not the
  // dimensionless z-space: S3's s2 is d(f)/dz at the forward, and the ATM
  // strike-skew is d(sigma)/dk = 0.5 * s2 / sqrt(T), so a *constant* z-space s2
  // would blow up the short-dated strike-skew (a tiny sigma_hat0 = sigma0*sqrt(T)
  // magnifies it). To model a realistic, roughly tenor-flat index strike-skew we
  // therefore SCALE s2 ~ sqrt(T): s2 = 2*sqrt(T)*skew_k with skew_k ~ -0.55
  // (vol per unit log-moneyness). Curvature c2 is kept mild and grows gently.
  // theta = sigma0^2 * T is strictly increasing (calendar-arb-free truth), and
  // each slice is ATF-arbitrage-free (|s2| well inside the S3 bound).
  struct Row {
    const char* iso;
    double sigma0;
    double skew_k;  // target ATM strike-skew d(sigma)/dk  (s2 = 2*sqrt(T)*skew_k)
    double c2;
  };
  const Row rows[] = {
      {"2026-06-26", 0.098, -0.65, 0.15},  // ~1w
      {"2026-07-17", 0.108, -0.62, 0.25},  // ~1m
      {"2026-08-21", 0.118, -0.60, 0.35},  // ~2m
      {"2026-09-18", 0.128, -0.58, 0.45},  // ~3m
      {"2026-12-18", 0.145, -0.55, 0.55},  // ~6m
      {"2027-06-18", 0.162, -0.52, 0.65},  // ~1y
  };
  for (const Row& r : rows) {
    SynthExpiry e;
    e.expiry_iso = r.iso;
    e.T = year_fraction(snapshot, r.iso);
    const double s2 = 2.0 * std::sqrt(e.T) * r.skew_k;  // strike-skew -> z-space
    e.truth = S3Params{r.sigma0, s2, r.c2};
    spec.expiries.push_back(e);
  }

  // Dense 5-wide ladder over the liquid +/-10% band (540..660 on a 600 spot).
  // A shared ladder must suit the SHORTEST tenor too: a 1w index chain does not
  // liquidly quote +/-17% strikes, and those deep near-dated wings only add
  // pathological high-vol de-Am work (and inflate the correction cache's sigma
  // box) without adding fittable signal. +/-10% is dense and covers where the
  // book actually trades across all six tenors.
  for (double K = 540.0; K <= 660.0 + 1e-9; K += 5.0) {
    spec.strikes.push_back(K);
  }

  // Index-liquid spreads: 0.6% of mid, floored at a penny half-tick.
  spec.half_spread_frac = 0.006;
  spec.min_half_spread = 0.01;
  return spec;
}

// Session inputs for the SPY fixture at `preset`. Default `Fast`: the fixture's
// truth is calendar-monotone, so the raw independent-per-slice fit is ALREADY
// calendar-arb-free here (measured) — MonotoneFit repair is unnecessary and only
// adds cost, so `Fast` is the SOTA config on this surface. `Robust` yields the
// same arb-free surface at higher build cost and is the right choice on data
// whose wings actually cross. Fills the market snapshot (spot/rate/divs/
// valuation-ts) from the spec, then applies the preset.
[[nodiscard]] inline SessionInputs make_spy_session_inputs(
    const SynthPanelSpec& spec, FitPreset preset = FitPreset::Fast) {
  SessionInputs in =
      make_session_inputs(preset, spec.spot, spec.r, iso_to_ns(spec.snapshot_iso));
  in.cash_divs = spec.cash_divs;
  return in;
}

}  // namespace atx::vol
