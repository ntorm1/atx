// Breadth across MARKET REGIMES — the self-contained PricerFitter, with no
// per-regime config, fits and serves an accurate surface whether the market is
// calm, stressed (a selloff), wide (1-min-after-open two-sided markets), or
// event-driven (elevated front vol, 1-min-before-earnings).
//
// These panels are SYNTHETIC (known-truth S3 smiles via make_synthetic_american_
// panel) — the repo has no real intraday-regime OPRA slices (only a single
// 19:55Z snapshot per symbol). They validate robustness of the fit + curve
// selection across regimes; the REAL cross-underlying breadth (SPY index vs XOM
// single-name) lives in spy_bidask_regression_test + vol_breadth_bench.
//
// The truth smiles are arbitrage-free, so a healthy fit reprices the board well
// regardless of regime; the assertions guard that the auto-selecting fitter does
// not fall over (fit succeeds, surface is non-empty, served price-in-band stays
// high) as vol level and spread width swing.

#include <cstdio>
#include <string>

#include <gtest/gtest.h>

#include "atx/vol/panel.hpp"
#include "atx/vol/pricer_fitter.hpp"
#include "atx/vol/spy_fixture.hpp"
#include "atx/vol/vol_curve.hpp"
#include "support/opra_fixture.hpp"

using namespace atx::vol;

namespace {

// Fit a synthetic panel through the auto-selecting PricerFitter and report the
// served price-in-band + the curve the selector chose.
struct RegimeOutcome {
  bool built{false};
  double px_clean{0.0};
  double px_all{0.0};
  VolCurveKind kind{VolCurveKind::ConvexDense};
  std::size_t n_slices{0};
};

RegimeOutcome fit_regime(const char* label, const SynthPanelSpec& spec) {
  RegimeOutcome out;
  auto panel = make_synthetic_american_panel(spec);
  if (!panel.has_value()) {
    std::printf("  [%s] panel build FAILED: %s\n", label,
                panel.error().to_string().c_str());
    return out;
  }
  auto chain = OptionChain::from_frame(
      panel->frame, MarketEnv::flat(spec.spot, spec.r,
                                    iso_to_ns(spec.snapshot_iso), spec.cash_divs));
  if (!chain.has_value()) {
    std::printf("  [%s] chain build FAILED\n", label);
    return out;
  }
  PricerConfig cfg;
  cfg.preset = FitPreset::Fast;  // curve unset => auto-select
  PricerFitter fitter{cfg};
  if (!fitter.fit(*chain).has_value()) {
    std::printf("  [%s] fit FAILED\n", label);
    return out;
  }
  out.built = true;
  if (fitter.selection().has_value()) {
    out.kind = fitter.selection()->chosen.kind;
  }
  const auto sc = testkit::price_in_band(fitter.surface()->session(),
                                         chain->underlying(), spec.spot, spec.r);
  out.px_clean = sc.px_clean;
  out.px_all = sc.px_all;
  out.n_slices = fitter.surface()->session().expiries().size();
  std::printf("  [%-14s] curve=%-13s  pxCLN=%6.2f%%  pxALL=%6.2f%%  slices=%zu\n",
              label, to_string(out.kind), out.px_clean, out.px_all, out.n_slices);
  return out;
}

// A stressed (selloff) variant: scale every slice's ATF vol up and widen the
// market to a stressed two-sided spread.
SynthPanelSpec stressed_spec() {
  SynthPanelSpec s = make_spy_synthetic_spec();
  for (SynthExpiry& e : s.expiries) {
    e.truth.sigma0 *= 2.6;  // ~10-16% calm -> ~26-42% stressed
  }
  s.half_spread_frac = 0.015;
  s.min_half_spread = 0.03;
  return s;
}

// A wide-open-market variant (1-min after the open): calm vol, but markets are
// wide and two-sided before liquidity tightens.
SynthPanelSpec wide_open_spec() {
  SynthPanelSpec s = make_spy_synthetic_spec();
  s.half_spread_frac = 0.030;
  s.min_half_spread = 0.05;
  return s;
}

// A pre-earnings single-name-style variant: elevated, front-loaded vol (the
// earnings jump lives in the near expiries) on a lower-priced name with a wider
// market.
SynthPanelSpec pre_earnings_spec() {
  SynthPanelSpec s = make_spy_synthetic_spec();
  // Front-load the vol term structure (earnings jump decays with tenor).
  const double bump[] = {1.9, 1.7, 1.45, 1.25, 1.1, 1.02};
  for (std::size_t i = 0; i < s.expiries.size(); ++i) {
    s.expiries[i].truth.sigma0 *= bump[i < 6 ? i : 5];
  }
  s.half_spread_frac = 0.012;
  s.min_half_spread = 0.02;
  return s;
}

}  // namespace

TEST(BreadthRegime, AutoSelectAcrossSyntheticRegimes) {
  std::printf("Synthetic market-regime breadth (auto-select, no per-regime config):\n");

  const RegimeOutcome calm = fit_regime("calm", make_spy_synthetic_spec());
  const RegimeOutcome stress = fit_regime("stressed", stressed_spec());
  const RegimeOutcome wide = fit_regime("wide-open", wide_open_spec());
  const RegimeOutcome earn = fit_regime("pre-earnings", pre_earnings_spec());

  // Every regime must fit and produce a non-empty surface.
  for (const RegimeOutcome* r : {&calm, &stress, &wide, &earn}) {
    EXPECT_TRUE(r->built);
    EXPECT_GT(r->n_slices, 0u);
  }

  // The served surface must reprice the (arb-free) board well in every regime.
  // The calm regime has the TIGHTEST market (0.6% spreads) on a sparse synthetic
  // ladder, so the parsimonious backbone the selector picks on this smooth truth
  // is not penny-perfect (~82%); it is the binding floor. Wider markets (stressed
  // selloff, wide-open, pre-earnings) give a wider band and reprice ~100%. The
  // point is robustness — the auto-selecting fitter serves a sane surface as vol
  // level and spread width swing, with no per-regime config.
  EXPECT_GE(calm.px_clean, 78.0);
  EXPECT_GE(stress.px_clean, 95.0);
  EXPECT_GE(wide.px_clean, 95.0);
  EXPECT_GE(earn.px_clean, 95.0);
}
