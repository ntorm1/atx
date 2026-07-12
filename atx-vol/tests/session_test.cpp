#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "atx/vol/american.hpp"
#include "atx/vol/calib.hpp"
#include "atx/vol/curve.hpp"
#include "atx/vol/data.hpp"
#include "atx/vol/panel.hpp"
#include "atx/vol/s3.hpp"
#include "atx/vol/session.hpp"
#include "atx/vol/types.hpp"
#include "atx/vol/universe.hpp"

// VolaSession composable-facade acceptance harness (atx/vol/session.hpp).
//
// Drives the same 4-expiry known-truth American-equity panel the SurfaceParity
// harness uses (rising ATM term structure, downward skew, one mid-life cash
// dividend), builds a session, and exercises the query surface: interpolated IV,
// re-Americanized fair value on- and between-slices, Greeks, and the argument-
// validation error path. The strike grid includes the at-forward level (100) so
// the ATM checks land on a real quote.

namespace {

using atx::vol::build_observations;
using atx::vol::CalibOpts;
using atx::vol::chain_index;
using atx::vol::data_install;
using atx::vol::FitDiag;
using atx::vol::DividendEvent;
using atx::vol::InterpMode;
using atx::vol::iso_to_ns;
using atx::vol::make_synthetic_american_panel;
using atx::vol::S3Params;
using atx::vol::SessionInputs;
using atx::vol::Side;
using atx::vol::SynthExpiry;
using atx::vol::SynthPanelSpec;
using atx::vol::Underlying;
using atx::vol::Universe;
using atx::vol::VolaSession;
using atx::vol::year_fraction;

// The 4-expiry known-truth panel spec (mirrors surface_parity_test's panel; the
// strike grid steps by 5 so the at-forward strike 100 is present for the ATM
// checks).
[[nodiscard]] SynthPanelSpec make_spec() {
  const std::string snapshot = "2026-06-19";
  const std::vector<std::string> isos = {
      "2026-07-26",  // ~0.10y
      "2026-10-06",  // ~0.30y
      "2027-01-24",  // ~0.60y
      "2027-06-19",  // ~1.00y
  };
  const std::vector<S3Params> truths = {
      S3Params{0.32, -0.70, 1.00},
      S3Params{0.30, -0.60, 0.90},
      S3Params{0.28, -0.55, 0.80},
      S3Params{0.27, -0.50, 0.70},
  };

  SynthPanelSpec spec;
  spec.uid = "SYNTH";
  spec.snapshot_iso = snapshot;
  spec.spot = 100.0;
  spec.r = 0.03;
  spec.borrow = 0.008;

  DividendEvent div;
  div.ex_date_ns = iso_to_ns("2026-12-15");  // mid-life; inside the 0.6y/1.0y
  div.amount = 0.5;
  spec.cash_divs = {div};

  for (std::size_t i = 0; i < isos.size(); ++i) {
    const double T = year_fraction(snapshot, isos[i]);
    spec.expiries.push_back(SynthExpiry{isos[i], T, truths[i]});
  }
  for (double K = 70.0; K <= 130.0 + 1e-9; K += 5.0) {
    spec.strikes.push_back(K);  // 13 strikes over 70..130, includes 100
  }
  spec.half_spread_frac = 0.02;
  return spec;
}

[[nodiscard]] SessionInputs make_inputs(const SynthPanelSpec& spec) {
  SessionInputs in;
  in.S = spec.spot;
  in.r = spec.r;
  in.cash_divs = spec.cash_divs;
  in.now_ts_ns = iso_to_ns(spec.snapshot_iso);
  in.deam.hyb = spec.hyb;
  in.deam.imply_borrow = true;
  in.deam.n_atm = 3;
  return in;
}

// A 2-expiry panel with DELIBERATELY contrasting smile shapes: the near
// expiry is strongly put-skewed, the far expiry is flat. Used to prove
// SessionInputs::interp actually reaches the eval seam -- ShapeBlend and
// PiecewiseTotalVariance must disagree at an arbitrary-T query strictly
// between two slices whose SHAPES (not just level) differ this much.
[[nodiscard]] SynthPanelSpec make_shape_contrast_spec() {
  const std::string snapshot = "2026-06-19";
  const std::vector<std::string> isos = {
      "2026-07-19",  // ~0.10y, skewed lo
      "2026-12-19",  // ~0.50y, flat hi
  };
  const std::vector<S3Params> truths = {
      S3Params{0.35, -1.20, 1.20},  // strong put skew
      S3Params{0.22, 0.0, 0.30},    // flat
  };

  SynthPanelSpec spec;
  spec.uid = "SYNTH2";
  spec.snapshot_iso = snapshot;
  spec.spot = 100.0;
  spec.r = 0.03;
  spec.borrow = 0.0;

  for (std::size_t i = 0; i < isos.size(); ++i) {
    const double T = year_fraction(snapshot, isos[i]);
    spec.expiries.push_back(SynthExpiry{isos[i], T, truths[i]});
  }
  for (double K = 60.0; K <= 140.0 + 1e-9; K += 5.0) {
    spec.strikes.push_back(K);
  }
  spec.half_spread_frac = 0.02;
  return spec;
}

// Install the spec's panel into `u` and return the resolved underlying pointer.
[[nodiscard]] const Underlying* install(const SynthPanelSpec& spec, Universe& u) {
  const auto panel = make_synthetic_american_panel(spec);
  EXPECT_TRUE(panel.has_value());
  if (!panel) {
    return nullptr;
  }
  const auto uid = data_install(u, panel->frame);
  EXPECT_TRUE(uid.has_value());
  if (!uid) {
    return nullptr;
  }
  const auto under = u.get_underlying(*uid);
  EXPECT_TRUE(under.has_value());
  return under ? *under : nullptr;
}

}  // namespace

TEST(VolaSession, Build_KnownTruthPanel_SucceedsWithFourArbFreeSlices) {
  const SynthPanelSpec spec = make_spec();
  Universe u;
  const Underlying* under = install(spec, u);
  ASSERT_NE(under, nullptr);
  ASSERT_EQ(under->chains.size(), std::size_t{4});

  const auto sess = VolaSession::build(*under, make_inputs(spec));
  ASSERT_TRUE(sess.has_value()) << sess.error().to_string();

  const auto& diag = sess->diagnostics();
  EXPECT_EQ(diag.n_slices, std::size_t{4});
  EXPECT_EQ(sess->expiries().size(), std::size_t{4});
  EXPECT_EQ(sess->parity().size(), std::size_t{4});
  EXPECT_TRUE(diag.calendar_arb_free);
  EXPECT_GE(diag.worst_frac_within_bidask, 0.90);
  EXPECT_GT(diag.n_quotes, std::size_t{0});

  // Slice context is ascending in T.
  const auto exps = sess->expiries();
  for (std::size_t i = 1; i < exps.size(); ++i) {
    EXPECT_LT(exps[i - 1].T, exps[i].T);
  }
}

TEST(VolaSession, Iv_OnSliceAtm_IsSaneVol) {
  const SynthPanelSpec spec = make_spec();
  Universe u;
  const Underlying* under = install(spec, u);
  ASSERT_NE(under, nullptr);

  const auto sess = VolaSession::build(*under, make_inputs(spec));
  ASSERT_TRUE(sess.has_value()) << sess.error().to_string();

  const double T = sess->expiries()[1].T;  // ~0.30y slice
  const double vol = sess->iv(100.0, T);
  ASSERT_TRUE(std::isfinite(vol));
  EXPECT_GT(vol, 0.01);
  EXPECT_LT(vol, 3.0);
}

TEST(VolaSession, FairValue_OnSliceAtm_IsPositiveAndNearMarket) {
  const SynthPanelSpec spec = make_spec();
  Universe u;
  const Underlying* under = install(spec, u);
  ASSERT_NE(under, nullptr);

  const auto sess = VolaSession::build(*under, make_inputs(spec));
  ASSERT_TRUE(sess.has_value()) << sess.error().to_string();

  // Middle expiry; read the market Call quote at the at-forward strike 100.
  const auto& chain = under->chains[1];
  std::size_t sidx = 0;
  bool found = false;
  for (std::size_t j = 0; j < chain.strikes.size(); ++j) {
    if (std::abs(chain.strikes[j] - 100.0) < 1e-6) {
      sidx = j;
      found = true;
      break;
    }
  }
  ASSERT_TRUE(found);
  const std::size_t ci = chain_index(static_cast<std::uint16_t>(sidx), Side::Call);
  const double bid = chain.bids[ci];
  const double ask = chain.asks[ci];

  const auto fv = sess->fair_value(100.0, chain.T, Side::Call);
  ASSERT_TRUE(fv.has_value()) << fv.error().to_string();
  EXPECT_TRUE(std::isfinite(*fv));
  EXPECT_GT(*fv, 0.0);
  // Re-priced off the fitted surface it should land inside the bid-ask (a light
  // band absorbs any residual fit error).
  EXPECT_GE(*fv, bid - 0.05);
  EXPECT_LE(*fv, ask + 0.05);
}

TEST(VolaSession, FairValue_BetweenSlices_IsFinitePositive) {
  const SynthPanelSpec spec = make_spec();
  Universe u;
  const Underlying* under = install(spec, u);
  ASSERT_NE(under, nullptr);

  const auto sess = VolaSession::build(*under, make_inputs(spec));
  ASSERT_TRUE(sess.has_value()) << sess.error().to_string();

  // Midpoint between the first two fitted slices exercises the interpolation
  // path (forward + carry linearly interpolated in T).
  const auto exps = sess->expiries();
  const double Tstar = 0.5 * (exps[0].T + exps[1].T);
  const auto fv = sess->fair_value(100.0, Tstar, Side::Call);
  ASSERT_TRUE(fv.has_value()) << fv.error().to_string();
  EXPECT_TRUE(std::isfinite(*fv));
  EXPECT_GT(*fv, 0.0);
}

TEST(VolaSession, Greeks_Call_HasDeltaInUnitInterval) {
  const SynthPanelSpec spec = make_spec();
  Universe u;
  const Underlying* under = install(spec, u);
  ASSERT_NE(under, nullptr);

  const auto sess = VolaSession::build(*under, make_inputs(spec));
  ASSERT_TRUE(sess.has_value()) << sess.error().to_string();

  const double T = sess->expiries()[1].T;
  const auto g = sess->greeks(100.0, T, Side::Call);
  ASSERT_TRUE(g.has_value()) << g.error().to_string();
  EXPECT_TRUE(std::isfinite(g->delta));
  EXPECT_GT(g->delta, 0.0);
  EXPECT_LT(g->delta, 1.0);
}

TEST(VolaSession, FairValue_NonPositiveStrike_ReturnsInvalidArgument) {
  const SynthPanelSpec spec = make_spec();
  Universe u;
  const Underlying* under = install(spec, u);
  ASSERT_NE(under, nullptr);

  const auto sess = VolaSession::build(*under, make_inputs(spec));
  ASSERT_TRUE(sess.has_value()) << sess.error().to_string();

  const double T = sess->expiries()[0].T;
  const auto fv = sess->fair_value(-1.0, T, Side::Call);
  ASSERT_FALSE(fv.has_value());
  EXPECT_EQ(fv.error().code(), atx::vol::ErrorCode::InvalidArgument);

  // A non-positive maturity is rejected the same way.
  const auto fv_t = sess->fair_value(100.0, 0.0, Side::Call);
  ASSERT_FALSE(fv_t.has_value());
  EXPECT_EQ(fv_t.error().code(), atx::vol::ErrorCode::InvalidArgument);
}

TEST(VolaSession, FromFrame_KnownTruthPanel_BuildsFourSlices) {
  const SynthPanelSpec spec = make_spec();
  const auto panel = make_synthetic_american_panel(spec);
  ASSERT_TRUE(panel.has_value()) << panel.error().to_string();

  const auto sess = VolaSession::from_frame(panel->frame, make_inputs(spec));
  ASSERT_TRUE(sess.has_value()) << sess.error().to_string();
  EXPECT_EQ(sess->diagnostics().n_slices, std::size_t{4});
  EXPECT_TRUE(sess->diagnostics().calendar_arb_free);
}

TEST(FitPreset, PopulatesPolicyFieldsPerPreset) {
  using atx::vol::al_default_opts;
  using atx::vol::al_fast_opts;
  using atx::vol::AmericanMethod;
  using atx::vol::apply_fit_preset;
  using atx::vol::CalendarRepair;
  using atx::vol::FitPreset;
  using atx::vol::make_session_inputs;

  // make_session_inputs fills the market snapshot then applies the policy.
  const auto fast = make_session_inputs(FitPreset::Fast, 100.0, 0.03, 42);
  EXPECT_DOUBLE_EQ(fast.S, 100.0);
  EXPECT_DOUBLE_EQ(fast.r, 0.03);
  EXPECT_EQ(fast.now_ts_ns, std::int64_t{42});
  EXPECT_TRUE(fast.use_correction_cache);
  EXPECT_TRUE(fast.score_parity);
  EXPECT_TRUE(fast.enforce_calendar_floor);
  EXPECT_FALSE(fast.use_deam_cache_for_fit);
  EXPECT_EQ(fast.calib.max_obs_per_slice, 0u);
  EXPECT_DOUBLE_EQ(fast.calib.max_otm_shortcut_premium_spread_frac, 0.0);
  ASSERT_TRUE(fast.deam.al_opts.has_value());
  EXPECT_EQ(fast.deam.al_opts->n_collocation, al_fast_opts().n_collocation);
  EXPECT_EQ(fast.deam.method, AmericanMethod::AndersenLake);
  EXPECT_EQ(fast.deam.n_atm, std::size_t{1});
  EXPECT_EQ(fast.deam.max_borrow_pairs, std::size_t{12});
  EXPECT_DOUBLE_EQ(fast.deam.iv_tol, 1.0e-5);
  EXPECT_EQ(fast.calendar_repair, CalendarRepair::None);

  // apply_fit_preset preserves market fields the caller already set.
  SessionInputs robust;
  robust.S = 200.0;
  robust.r = 0.05;
  apply_fit_preset(robust, FitPreset::Robust);
  EXPECT_DOUBLE_EQ(robust.S, 200.0);
  EXPECT_DOUBLE_EQ(robust.r, 0.05);
  ASSERT_TRUE(robust.deam.al_opts.has_value());
  EXPECT_EQ(robust.deam.al_opts->n_collocation, al_default_opts().n_collocation);
  EXPECT_EQ(robust.deam.method, AmericanMethod::AndersenLake);
  EXPECT_EQ(robust.deam.n_atm, std::size_t{3});
  EXPECT_EQ(robust.calendar_repair, CalendarRepair::MonotoneFit);

  SessionInputs hft;
  apply_fit_preset(hft, FitPreset::Hft);
  EXPECT_EQ(hft.calendar_repair, CalendarRepair::None);
  EXPECT_EQ(hft.deam.method, AmericanMethod::AndersenLake);
  EXPECT_EQ(hft.deam.n_atm, std::size_t{1});  // fast borrow
  EXPECT_EQ(hft.deam.max_borrow_pairs, std::size_t{1});
  EXPECT_EQ(hft.curve.kind, atx::vol::VolCurveKind::LinearVariance);
  EXPECT_FALSE(hft.use_correction_cache);
  EXPECT_FALSE(hft.score_parity);
  EXPECT_FALSE(hft.enforce_calendar_floor);
  EXPECT_FALSE(hft.use_deam_cache_for_fit);
  EXPECT_EQ(hft.calib.max_obs_per_slice, 48u);
  EXPECT_DOUBLE_EQ(hft.calib.max_otm_shortcut_premium_spread_frac, 0.50);

  SessionInputs acc;
  apply_fit_preset(acc, FitPreset::Accurate);
  EXPECT_EQ(acc.calendar_repair, CalendarRepair::None);
  ASSERT_TRUE(acc.deam.al_opts.has_value());
  EXPECT_EQ(acc.deam.al_opts->max_newton_iter, al_default_opts().max_newton_iter);
}

TEST(VolaSession, FairValueLadder_MatchesScalarAndHandlesBadStrikes) {
  const SynthPanelSpec spec = make_spec();
  const auto panel = make_synthetic_american_panel(spec);
  ASSERT_TRUE(panel.has_value()) << panel.error().to_string();
  const auto sess = VolaSession::from_frame(panel->frame, make_inputs(spec));
  ASSERT_TRUE(sess.has_value()) << sess.error().to_string();

  const double T = sess->expiries().front().T;  // an on-slice maturity
  const std::vector<double> strikes = {80.0, 90.0, 100.0, 110.0, 120.0, -5.0};
  std::vector<Side> sides;
  for (const double K : strikes) {
    sides.push_back(K >= 100.0 ? Side::Call : Side::Put);
  }

  std::vector<double> out(strikes.size(), 0.0);
  const auto st = sess->fair_value_ladder(T, strikes, sides, out);
  ASSERT_TRUE(st.has_value()) << st.error().to_string();

  // Each ladder entry is bit-identical to the scalar fair_value; the bad strike
  // becomes NaN without sinking the ladder.
  for (std::size_t i = 0; i < strikes.size(); ++i) {
    if (strikes[i] <= 0.0) {
      EXPECT_TRUE(std::isnan(out[i]));
      continue;
    }
    const auto scalar = sess->fair_value(strikes[i], T, sides[i]);
    ASSERT_TRUE(scalar.has_value());
    EXPECT_DOUBLE_EQ(out[i], *scalar);
  }

  // Structural errors: length mismatch and a bad expiry.
  std::vector<double> short_out(strikes.size() - 1, 0.0);
  EXPECT_FALSE(sess->fair_value_ladder(T, strikes, sides, short_out).has_value());
  EXPECT_FALSE(sess->fair_value_ladder(-1.0, strikes, sides, out).has_value());

  // Greeks ladder matches the scalar greeks entry-for-entry.
  std::vector<atx::vol::AmericanGreeks> g(strikes.size());
  ASSERT_TRUE(sess->greeks_ladder(T, strikes, sides, g).has_value());
  for (std::size_t i = 0; i < strikes.size(); ++i) {
    if (strikes[i] <= 0.0) {
      EXPECT_TRUE(std::isnan(g[i].price));
      continue;
    }
    const auto sg = sess->greeks(strikes[i], T, sides[i]);
    ASSERT_TRUE(sg.has_value());
    EXPECT_DOUBLE_EQ(g[i].delta, sg->delta);
    EXPECT_DOUBLE_EQ(g[i].price, sg->price);
  }
}

TEST(FitPreset, RobustPresetBuildsSessionOnKnownPanel) {
  const SynthPanelSpec spec = make_spec();
  const auto panel = make_synthetic_american_panel(spec);
  ASSERT_TRUE(panel.has_value()) << panel.error().to_string();

  // Start from the panel-driven inputs, then switch to the Robust preset (keeps
  // S/r/divs/now, sets the fit policy + MonotoneFit calendar repair).
  SessionInputs in = make_inputs(spec);
  atx::vol::apply_fit_preset(in, atx::vol::FitPreset::Robust);
  EXPECT_EQ(in.calendar_repair, atx::vol::CalendarRepair::MonotoneFit);

  const auto sess = VolaSession::from_frame(panel->frame, in);
  ASSERT_TRUE(sess.has_value()) << sess.error().to_string();
  EXPECT_EQ(sess->diagnostics().n_slices, std::size_t{4});
  // The clean panel is already calendar-arb-free; Robust preserves that.
  EXPECT_TRUE(sess->diagnostics().calendar_arb_free);
}

// Tick-to-quote: warm-start refit of a single expiry from a fresh observation
// set updates the surface in place, warm-starts from the prior slice, and
// guards its arguments.
TEST(VolaSession, RefitSlice_WarmUpdatesOneExpiryAndGuardsArgs) {
  const SynthPanelSpec spec = make_spec();
  Universe u;
  const Underlying* under = install(spec, u);
  ASSERT_NE(under, nullptr);

  auto sess = VolaSession::build(*under, make_inputs(spec));
  ASSERT_TRUE(sess.has_value()) << sess.error().to_string();

  // Rebuild the observation set for the middle expiry from its chain, on the
  // session's own (forward, T) for that slice.
  const std::size_t idx = 1;
  const auto& chain = under->chains[idx];
  const double T = sess->expiries()[idx].T;
  const double F = sess->expiries()[idx].forward;
  const double df = std::exp(-spec.r * T);
  const auto obs = build_observations(chain, F, T, df, CalibOpts{});
  ASSERT_TRUE(obs.has_value()) << obs.error().to_string();
  ASSERT_GE(obs->obs.size(), std::size_t{5});

  // Query the pre-refit ATM vol, then refit and confirm the surface stays a
  // valid, finite, arb-free slice serving that expiry.
  const double vol_before = sess->iv(100.0, T);
  ASSERT_TRUE(std::isfinite(vol_before));

  const auto diag = sess->refit_slice(idx, obs->obs);
  ASSERT_TRUE(diag.has_value()) << diag.error().to_string();
  EXPECT_EQ(diag->n_quotes_used, static_cast<std::uint32_t>(obs->obs.size()));
  EXPECT_EQ(sess->expiries()[idx].n_used, obs->obs.size());

  const double vol_after = sess->iv(100.0, T);
  EXPECT_TRUE(std::isfinite(vol_after));
  EXPECT_GT(vol_after, 0.01);
  EXPECT_LT(vol_after, 3.0);
  EXPECT_TRUE(sess->diagnostics().calendar_arb_free);

  // A SECOND refit with the same obs is warm from the just-fit slice, so it stays
  // in the same small inner-LM budget as the first — it must NOT blow up toward a
  // cold fit (max_outer·max_inner = 4·12 = 48). A few steps of slack: IRLS
  // reweighting can add a Newton step or two, and the exact LM path shifts
  // slightly with the per-slice parity carry (the obs are rebuilt on the session's
  // own forward), so an exact `<=` is fixture-brittle rather than a real property.
  FitDiag first = *diag;
  const auto diag2 = sess->refit_slice(idx, obs->obs);
  ASSERT_TRUE(diag2.has_value());
  EXPECT_LE(diag2->inner_iters_total, first.inner_iters_total + 8);

  // Guards: out-of-range index and an empty observation set.
  EXPECT_FALSE(sess->refit_slice(99, obs->obs).has_value());
  const std::vector<atx::vol::FitObs> empty;
  EXPECT_FALSE(sess->refit_slice(idx, empty).has_value());
}

// Band-violation stats (SpiderRock-style, record-only): SessionDiagnostics
// rolls up ParityReport::band across every fitted expiry. Verifies the
// default eSSVI aggregation loop (session.cpp) against a direct sum/max over
// the session's own per-expiry parity reports.
TEST(Session, DiagnosticsAggregateBandStats) {
  // A near-zero bid-ask band: the known-truth panel fits the wide (2%)
  // default spread essentially perfectly (zero violations), which would make
  // this check vacuous. Shrinking the band forces the LM fit's real residual
  // slop to cross it on at least some quotes, so the rollup has real signal.
  SynthPanelSpec spec = make_spec();
  spec.half_spread_frac = 0.0001;
  spec.min_half_spread = 0.0001;
  Universe u;
  const Underlying* under = install(spec, u);
  ASSERT_NE(under, nullptr);

  const auto sess = VolaSession::build(*under, make_inputs(spec));
  ASSERT_TRUE(sess.has_value()) << sess.error().to_string();

  std::size_t want_bid_miss = 0;
  std::size_t want_ask_miss = 0;
  double want_max_prc_err = 0.0;
  for (const auto& p : sess->parity()) {
    want_bid_miss += p.band.n_bid_miss;
    want_ask_miss += p.band.n_ask_miss;
    want_max_prc_err = std::max(want_max_prc_err, p.band.max_prc_err);
  }
  // Guard against a vacuous pass: the known-truth panel is not a perfect
  // fit (worst_frac_within_bidask floors at 0.90 elsewhere in this file), so
  // some band violations must exist for this check to be meaningful.
  ASSERT_GT(want_bid_miss + want_ask_miss, std::size_t{0});

  const auto& diag = sess->diagnostics();
  EXPECT_EQ(diag.n_bid_miss, want_bid_miss);
  EXPECT_EQ(diag.n_ask_miss, want_ask_miss);
  EXPECT_DOUBLE_EQ(diag.max_prc_err, want_max_prc_err);
}

// Same rollup, exercised through the CurveSurface (non-Essvi) build path --
// the second of the two aggregation loops in session.cpp that the 07-11
// sprint's session-guard-fix history shows drift when only one is edited.
TEST(Session, DiagnosticsAggregateBandStats_CurveSurfacePath) {
  // Same tight-band rationale as DiagnosticsAggregateBandStats above, but this
  // one exercises the SEPARATE CurveSurface-path rollup (session.cpp: the
  // `crep.per_expiry` aggregation loop, distinct from the legacy eSSVI loop the
  // sibling test covers). The curve family must be one the non-Essvi
  // CurveSurface driver actually serves AND one whose fit leaves real residual
  // slop against the noiseless known-truth panel, or the rollup has no signal
  // to aggregate. The original LinearVariance choice no longer qualifies: the
  // adaptive-knot linear-total-variance family is near-interpolating and, after
  // the fitting-pipeline sprint tightened it, fits each smooth s3 slice to well
  // inside a 1 bp band (zero violations at any band width -- verified). Raw SVI
  // is parsimonious (5 params) and structurally cannot reproduce the s3
  // hyperbola smile exactly, so it crosses the tight band on ~40 held quotes --
  // giving the CurveSurface-path aggregation loop genuine non-zero band stats
  // to roll up.
  SynthPanelSpec spec = make_spec();
  spec.half_spread_frac = 0.0001;
  spec.min_half_spread = 0.0001;
  Universe u;
  const Underlying* under = install(spec, u);
  ASSERT_NE(under, nullptr);

  SessionInputs in = make_inputs(spec);
  in.curve.kind = atx::vol::VolCurveKind::Svi;

  const auto sess = VolaSession::build(*under, in);
  ASSERT_TRUE(sess.has_value()) << sess.error().to_string();

  std::size_t want_bid_miss = 0;
  std::size_t want_ask_miss = 0;
  double want_max_prc_err = 0.0;
  for (const auto& p : sess->parity()) {
    want_bid_miss += p.band.n_bid_miss;
    want_ask_miss += p.band.n_ask_miss;
    want_max_prc_err = std::max(want_max_prc_err, p.band.max_prc_err);
  }
  ASSERT_GT(want_bid_miss + want_ask_miss, std::size_t{0});

  const auto& diag = sess->diagnostics();
  EXPECT_EQ(diag.n_bid_miss, want_bid_miss);
  EXPECT_EQ(diag.n_ask_miss, want_ask_miss);
  EXPECT_DOUBLE_EQ(diag.max_prc_err, want_max_prc_err);
}

TEST(Session, InterpModeReachesEval) {
  // Two synthetic slices with deliberately different shapes (skewed lo, flat
  // hi -- see make_shape_contrast_spec). A production SessionInputs::interp
  // = ShapeBlend must reach the eval seam: an arbitrary-T query strictly
  // between the two fitted slices, at a strike away from ATM (where the
  // skew shapes diverge), must differ from the PiecewiseTotalVariance
  // default -- proving ShapeBlend is actually served, not dead config.
  const SynthPanelSpec spec = make_shape_contrast_spec();
  Universe u;
  const Underlying* under = install(spec, u);
  ASSERT_NE(under, nullptr);
  ASSERT_EQ(under->chains.size(), std::size_t{2});

  SessionInputs in_default = make_inputs(spec);
  ASSERT_EQ(in_default.interp, InterpMode::PiecewiseTotalVariance);  // default
  const auto sess_default = VolaSession::build(*under, in_default);
  ASSERT_TRUE(sess_default.has_value()) << sess_default.error().to_string();

  SessionInputs in_shape = make_inputs(spec);
  in_shape.interp = InterpMode::ShapeBlend;
  const auto sess_shape = VolaSession::build(*under, in_shape);
  ASSERT_TRUE(sess_shape.has_value()) << sess_shape.error().to_string();

  // Strictly between the two fitted slices; a strike well off ATM so the
  // skew-shape divergence between the parents shows up in the blend.
  const auto exps = sess_default->expiries();
  ASSERT_EQ(exps.size(), std::size_t{2});
  const double T_mid = 0.5 * (exps[0].T + exps[1].T);
  constexpr double kSkewedStrike = 80.0;

  const double iv_default = sess_default->iv(kSkewedStrike, T_mid);
  const double iv_shape = sess_shape->iv(kSkewedStrike, T_mid);
  ASSERT_TRUE(std::isfinite(iv_default));
  ASSERT_TRUE(std::isfinite(iv_shape));
  EXPECT_GT(std::fabs(iv_shape - iv_default), 1e-4)
      << "iv_default=" << iv_default << " iv_shape=" << iv_shape;

  // Default is bit-identical to a session built before this task: the
  // PiecewiseTotalVariance branch of the query path is untouched code
  // (same surface_.iv() call it always was), so this golden pin -- captured
  // once off the fixture above -- must hold going forward. EXPECT_NEAR (not
  // EXPECT_EQ) because this pins a full eSSVI Levenberg-Marquardt fit
  // output, which is susceptible to cross-machine ULP drift in the LM's
  // transcendental/linear-algebra steps (same fragility class as the
  // quarantined MultinamePipeline bit-exact pins); the bit-identity of the
  // untouched default code path is proven structurally by the argument
  // above, not by this literal's exactness.
  EXPECT_NEAR(iv_default, 0.35727349437368516, 1e-12);
}
