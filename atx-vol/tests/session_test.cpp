#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "atx/vol/american.hpp"
#include "atx/vol/arb.hpp"  // QuoteFlag (carry-skip diagnostics test)
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
using atx::vol::build_observations_european;
using atx::vol::CalibOpts;
using atx::vol::Chain;
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
using atx::vol::VolCurveKind;
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
  EXPECT_EQ(diag.n_carry_slices, diag.n_slices);
  EXPECT_EQ(diag.n_carry_confident, diag.n_slices);
  EXPECT_TRUE(diag.carry_confident);
  EXPECT_EQ(diag.n_inversion_slices, diag.n_slices);
  EXPECT_GT(diag.n_iv_proposed, std::size_t{0});
  EXPECT_EQ(diag.n_iv_audited, diag.n_iv_proposed);
  EXPECT_EQ(diag.n_iv_rejected_residual, std::size_t{0});
  // Honest certificate (task 2a / carry C1): this build does NOT set
  // deam.audit_fit_inversions, so the eSSVI FIT rows never ran the audited
  // route — only the diagnostic re-run above did. A certificate may not vouch
  // for rows the fit never used, so it must stay false here (the audited
  // variant is covered by AuditedEssviFitCertifiesInversions below).
  EXPECT_FALSE(diag.inversion_certified);
  EXPECT_EQ(diag.n_carry_skipped_expiries, std::size_t{0});

  const auto input_diag = sess->slice_diagnostics();
  ASSERT_EQ(input_diag.size(), sess->expiries().size());
  for (std::size_t i = 0; i < input_diag.size(); ++i) {
    EXPECT_DOUBLE_EQ(input_diag[i].T, sess->expiries()[i].T);
    EXPECT_TRUE(input_diag[i].carry.available);
    EXPECT_TRUE(input_diag[i].carry.confident);
    EXPECT_GE(input_diag[i].carry.n_retained, std::size_t{3});
    EXPECT_TRUE(input_diag[i].inversion_available);
    EXPECT_FALSE(input_diag[i].inversion_certified);  // unaudited fit rows
  }

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
  EXPECT_TRUE(fast.use_deam_cache_for_fit);
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
  EXPECT_TRUE(robust.use_deam_cache_for_fit);

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
  EXPECT_FALSE(acc.use_deam_cache_for_fit);
}

TEST(VolaSession, OffPillarCarryIsCoherentAcrossLiveAndPricedPaths) {
  const SynthPanelSpec spec = make_spec();
  Universe u;
  const Underlying *under = install(spec, u);
  ASSERT_NE(under, nullptr);

  SessionInputs inputs = make_inputs(spec);
  for (const SynthExpiry &expiry : spec.expiries) {
    inputs.expiry_rate_T.push_back(expiry.T);
  }
  inputs.expiry_rates = {0.021, 0.027, 0.034, 0.041};
  const auto session = VolaSession::build(*under, inputs);
  ASSERT_TRUE(session.has_value()) << session.error().to_string();
  auto priced = session->to_priced_surface();
  ASSERT_TRUE(priced.has_value()) << priced.error().to_string();

  const std::span<const atx::vol::SliceContext> pillars = session->expiries();
  ASSERT_GE(pillars.size(), std::size_t{2});
  std::vector<double> probes{pillars.front().T * 0.5, pillars.back().T * 1.5};
  for (std::size_t i = 0; i + 1u < pillars.size(); ++i) {
    probes.push_back(0.5 * (pillars[i].T + pillars[i + 1u].T));
  }

  for (const double T : probes) {
    const double live_forward = session->forward_at(T);
    const double live_reproduced =
        inputs.S * std::exp((session->rate_at(T) - session->q_eff_at(T)) * T);
    EXPECT_NEAR(live_reproduced, live_forward, 2.0e-13 * live_forward) << "T=" << T;

    const double priced_forward = priced->forward_at(T);
    const double priced_reproduced =
        inputs.S * std::exp((priced->rate_at(T) - priced->q_eff_at(T)) * T);
    EXPECT_NEAR(priced_reproduced, priced_forward, 2.0e-13 * priced_forward) << "T=" << T;
    EXPECT_NEAR(priced_forward, live_forward, 2.0e-13 * live_forward) << "T=" << T;
    EXPECT_NEAR(priced->q_eff_at(T), session->q_eff_at(T), 2.0e-13) << "T=" << T;
  }
  EXPECT_DOUBLE_EQ(session->q_eff_at(pillars.front().T * 0.5), pillars.front().q_eff);
  EXPECT_DOUBLE_EQ(session->q_eff_at(pillars.back().T * 1.5), pillars.back().q_eff);

  for (const atx::vol::SliceContext &pillar : pillars) {
    EXPECT_DOUBLE_EQ(session->forward_at(pillar.T), pillar.forward);
    EXPECT_DOUBLE_EQ(session->q_eff_at(pillar.T), pillar.q_eff);
    EXPECT_NEAR(priced->forward_at(pillar.T), pillar.forward,
                2.0e-13 * pillar.forward);
    EXPECT_NEAR(priced->q_eff_at(pillar.T), pillar.q_eff, 2.0e-13);
  }
}

TEST(DeAmFitCache, CachedAndColdLinearVarianceFitsAreEconomicallyEquivalent) {
  const SynthPanelSpec spec = make_spec();
  Universe u;
  const Underlying* under = install(spec, u);
  ASSERT_NE(under, nullptr);

  SessionInputs cold = make_inputs(spec);
  atx::vol::apply_fit_preset(cold, atx::vol::FitPreset::Fast);
  cold.curve.kind = VolCurveKind::LinearVariance;
  cold.use_deam_cache_for_fit = false;
  SessionInputs cached = cold;
  cached.use_deam_cache_for_fit = true;

  const auto cold_session = VolaSession::build(*under, cold);
  ASSERT_TRUE(cold_session.has_value()) << cold_session.error().to_string();
  const auto cached_session = VolaSession::build(*under, cached);
  ASSERT_TRUE(cached_session.has_value()) << cached_session.error().to_string();

  // These are economic serving tolerances, not an implementation-detail
  // requirement for bit-identical fitted parameters.
  constexpr double kIvTolerance = 5.0e-3;     // 50 vol basis points
  // Cached proposals may move the fitted mark by a few ticks, but must stay a
  // small fraction of the executable uncertainty on this deliberately wide
  // (2% half-spread) fixture and below five cents per share.
  constexpr double kPriceTolerance = 5.0e-2;
  constexpr double kHalfSpreadFraction = 0.25;
  for (std::size_t expiry_index = 0u; expiry_index < cold_session->expiries().size();
       ++expiry_index) {
    const auto& expiry = cold_session->expiries()[expiry_index];
    const Chain& chain = under->chains[expiry_index];
    for (const double strike : {90.0, 100.0, 110.0}) {
      const Side side = strike >= spec.spot ? Side::Call : Side::Put;
      EXPECT_NEAR(cached_session->iv(strike, expiry.T),
                  cold_session->iv(strike, expiry.T), kIvTolerance);
      const auto cached_price = cached_session->fair_value(strike, expiry.T, side);
      const auto cold_price = cold_session->fair_value(strike, expiry.T, side);
      ASSERT_TRUE(cached_price.has_value()) << cached_price.error().to_string();
      ASSERT_TRUE(cold_price.has_value()) << cold_price.error().to_string();
      EXPECT_NEAR(*cached_price, *cold_price, kPriceTolerance);
      const auto strike_it = std::lower_bound(chain.strikes.begin(), chain.strikes.end(), strike);
      ASSERT_NE(strike_it, chain.strikes.end());
      const auto strike_index = static_cast<std::uint16_t>(
          std::distance(chain.strikes.begin(), strike_it));
      const std::size_t quote_index = chain_index(strike_index, side);
      const double half_spread = 0.5 * (chain.asks[quote_index] - chain.bids[quote_index]);
      EXPECT_LE(std::fabs(*cached_price - *cold_price),
                kHalfSpreadFraction * half_spread);
    }
  }
}

TEST(DeAmFitCache, TermRateSessionsForceTheColdFitPath) {
  const SynthPanelSpec spec = make_spec();
  Universe u;
  const Underlying* under = install(spec, u);
  ASSERT_NE(under, nullptr);

  SessionInputs in = make_inputs(spec);
  atx::vol::apply_fit_preset(in, atx::vol::FitPreset::Robust);
  ASSERT_TRUE(in.use_deam_cache_for_fit);
  for (const auto& expiry : spec.expiries) {
    in.expiry_rate_T.push_back(expiry.T);
    in.expiry_rates.push_back(spec.r);
  }

  const auto session = VolaSession::build(*under, in);
  ASSERT_TRUE(session.has_value()) << session.error().to_string();
  EXPECT_FALSE(session->inputs().use_correction_cache);
  EXPECT_FALSE(session->inputs().use_deam_cache_for_fit);
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
  //
  // RE-PINNED at the correctness-first-surface-v2 merge (main -> SpiderRock
  // integration): 0.35727349437368516 -> 0.35727349168272737 (delta 2.7e-9
  // absolute, 7.5e-9 relative). The move is NOT in the I1 code this test
  // guards -- it is upstream, in the shared de-Americanization / carry path
  // that EVERY eSSVI fit runs: that sprint reworked deamer.cpp (+398),
  // calib.cpp (+185) and surface_parity.cpp (+38), fixing carry-resolution
  // and de-Am inversion defects, which shifts the fitted slice and hence any
  // absolute value read off it. The same rework moved that sprint's OWN
  // fit-output goldens (it re-pinned prepared_portfolio_test's
  // kGoldenFingerprint in the same commit range). The property this pin
  // exists to protect -- "the PiecewiseTotalVariance default serves the
  // untouched surface_.iv() path, unperturbed by adding InterpMode" -- is
  // unchanged and is still asserted structurally by the >1e-4
  // ShapeBlend-vs-default check above, which passes.
  // Proposal-cache reuse and the coherent log-forward carry interpolation may
  // move this off-pillar fitted IV. Keep it inside the sprint's liquid-node
  // materiality limit; the >1e-4 contrast above is the routing assertion.
  EXPECT_NEAR(iv_default, 0.35727349168272737, 1e-5);
}

TEST(VolaSession, OverrideRefitIsLocalDeterministicAndTimed) {
  const SynthPanelSpec spec = make_spec();
  Universe u;
  const Underlying* under = install(spec, u);
  ASSERT_NE(under, nullptr);
  constexpr std::size_t idx = 1;

  for (const VolCurveKind kind : {VolCurveKind::ConvexDense,
                                  VolCurveKind::Svi,
                                  VolCurveKind::C8}) {
    SessionInputs in = make_inputs(spec);
    in.curve.kind = kind;
    in.enforce_calendar_floor = true;
    auto session = VolaSession::build(*under, in);
    ASSERT_TRUE(session.has_value())
        << "kind=" << static_cast<int>(kind) << " "
        << session.error().to_string();

    const double T = session->expiries()[idx].T;
    const double F = session->expiries()[idx].forward;
    const double df = std::exp(-spec.r * T);
    auto obs = build_observations_european(
        under->chains[idx], spec.spot, spec.r, F, T, df, in.calib);
    ASSERT_TRUE(obs.has_value()) << obs.error().to_string();
    ASSERT_GE(obs->obs.size(), std::size_t{8});

    const double left_before = session->iv(100.0, session->expiries()[idx - 1].T);
    const double right_before = session->iv(100.0, session->expiries()[idx + 1].T);
    auto first = session->refit_slice(idx, obs->obs);
    ASSERT_TRUE(first.has_value())
        << "kind=" << static_cast<int>(kind) << " "
        << first.error().to_string();
    const auto first_diag = session->diagnostics().incremental;
    EXPECT_EQ(first_diag.attempts, 1u);
    EXPECT_EQ(first_diag.committed, 1u);
    EXPECT_EQ(first_diag.rolled_back, 0u);
    EXPECT_TRUE(first_diag.last_committed);
    EXPECT_EQ(first_diag.last_slice_index, idx);
    EXPECT_EQ(first_diag.last_kind, kind);
    EXPECT_EQ(first_diag.last_adjacent_pairs_checked, 2u);
    EXPECT_GE(first_diag.last_total_ms, 0.0);
    EXPECT_GE(first_diag.last_fit_ms, 0.0);
    EXPECT_GE(first_diag.last_calendar_ms, 0.0);
    EXPECT_DOUBLE_EQ(session->iv(100.0, session->expiries()[idx - 1].T),
                     left_before);
    EXPECT_DOUBLE_EQ(session->iv(100.0, session->expiries()[idx + 1].T),
                     right_before);

    const double first_value = session->iv(100.0, T);
    auto second = session->refit_slice(idx, obs->obs);
    ASSERT_TRUE(second.has_value()) << second.error().to_string();
    EXPECT_NEAR(session->iv(100.0, T), first_value, 1.0e-10);
    EXPECT_EQ(session->diagnostics().incremental.attempts, 2u);
    EXPECT_EQ(session->diagnostics().incremental.committed, 2u);
  }
}

TEST(VolaSession, OverrideRefitCalendarFailureRollsBackAtomically) {
  const SynthPanelSpec spec = make_spec();
  Universe u;
  const Underlying* under = install(spec, u);
  ASSERT_NE(under, nullptr);
  SessionInputs in = make_inputs(spec);
  in.curve.kind = VolCurveKind::Svi;
  in.enforce_calendar_floor = true;
  auto session = VolaSession::build(*under, in);
  ASSERT_TRUE(session.has_value()) << session.error().to_string();

  constexpr std::size_t idx = 1;
  const double T = session->expiries()[idx].T;
  const double F = session->expiries()[idx].forward;
  const double df = std::exp(-spec.r * T);
  auto obs = build_observations_european(
      under->chains[idx], spec.spot, spec.r, F, T, df, in.calib);
  ASSERT_TRUE(obs.has_value()) << obs.error().to_string();
  for (auto &o : obs->obs) {
    o.sigma_mkt = 2.0;
    o.w_mkt = 4.0 * T;
  }

  std::vector<double> before;
  for (std::size_t i = 0; i < session->expiries().size(); ++i) {
    before.push_back(session->iv(100.0, session->expiries()[i].T));
  }
  const std::size_t used_before = session->expiries()[idx].n_used;
  const auto failed = session->refit_slice(idx, obs->obs);
  EXPECT_FALSE(failed.has_value());
  for (std::size_t i = 0; i < before.size(); ++i) {
    EXPECT_DOUBLE_EQ(session->iv(100.0, session->expiries()[i].T), before[i]);
  }
  EXPECT_EQ(session->expiries()[idx].n_used, used_before);
  const auto &diag = session->diagnostics().incremental;
  EXPECT_EQ(diag.attempts, 1u);
  EXPECT_EQ(diag.committed, 0u);
  EXPECT_EQ(diag.rolled_back, 1u);
  EXPECT_FALSE(diag.last_committed);
  EXPECT_EQ(diag.last_adjacent_pairs_checked, 2u);
}

// ── Certification-hole regressions (correctness-first sprint, task 2) ───────

// 2b (carry I4): a non-Andersen-Lake de-Am method (Baw) has no cold-reference
// audit anywhere in the pipeline, so its output must never be reported as
// inversion-certified. The old certificate was vacuously true for non-AL.
TEST(VolaSession, BawMethodIsNeverInversionCertified) {
  const SynthPanelSpec spec = make_spec();
  Universe u;
  const Underlying* under = install(spec, u);
  ASSERT_NE(under, nullptr);

  SessionInputs in = make_inputs(spec);
  in.deam.method = atx::vol::AmericanMethod::Baw;
  const auto sess = VolaSession::build(*under, in);
  ASSERT_TRUE(sess.has_value()) << sess.error().to_string();
  EXPECT_FALSE(sess->diagnostics().inversion_certified);
  for (const auto& sd : sess->slice_diagnostics()) {
    EXPECT_FALSE(sd.inversion_certified);
  }
}

// Robust carry weights are functions of bid/ask spread. Re-resolve that carry,
// but retain the certified coordinate when the aggregate move is below the
// documented economic threshold.
TEST(VolaSession, CachedRefitAcceptsImmaterialSpreadOnlyCarryMove) {
  const SynthPanelSpec spec = make_spec();
  Universe u;
  const Underlying* under = install(spec, u);
  ASSERT_NE(under, nullptr);

  const auto sess = VolaSession::build(*under, make_inputs(spec));
  ASSERT_TRUE(sess.has_value()) << sess.error().to_string();

  // Unchanged chain: the certified cache is reusable.
  ASSERT_TRUE(sess->cached_refit_observations(under->chains[0], 0u).has_value());

  // Widen both legs of the at-spot pair symmetrically: mids and flags are
  // bit-identical, but the carry quality weight is not.
  atx::vol::Chain widened = under->chains[0];
  std::size_t atm = 0;
  for (std::size_t j = 1; j < widened.strikes.size(); ++j) {
    if (std::fabs(widened.strikes[j] - spec.spot) <
        std::fabs(widened.strikes[atm] - spec.spot)) {
      atm = j;
    }
  }
  for (const Side side : {Side::Call, Side::Put}) {
    const std::size_t idx = chain_index(static_cast<std::uint16_t>(atm), side);
    const double mid = widened.mids[idx];
    const double half = 1.5 * 0.5 * (widened.asks[idx] - widened.bids[idx]);
    ASSERT_GT(mid - half, 0.0);
    widened.bids[idx] = mid - half;
    widened.asks[idx] = mid + half;
  }
  const auto reused = sess->cached_refit_observations(widened, 0u);
  EXPECT_TRUE(reused.has_value())
      << (reused.has_value() ? "" : reused.error().to_string());
}

// A spread-only update still changes a selected carry input, but with a single
// carry pair the resolved forward depends only on the unchanged call/put mids.
// The incremental cache may therefore reuse the certified European IV/vega and
// refresh only the spread-derived weights after re-resolving the carry.
TEST(VolaSession, CachedRefitAcceptsSpreadOnlyChangeWhenCarryCoordinateIsUnchanged) {
  const SynthPanelSpec spec = make_spec();
  Universe u;
  const Underlying* under = install(spec, u);
  ASSERT_NE(under, nullptr);

  SessionInputs in = make_inputs(spec);
  in.deam.al_opts = atx::vol::al_fast_opts();
  in.deam.max_borrow_pairs = 1u;
  in.deam.min_confident_borrow_pairs = 1u;
  const auto sess = VolaSession::build(*under, in);
  ASSERT_TRUE(sess.has_value()) << sess.error().to_string();

  const auto original = sess->cached_refit_observations(under->chains[0], 0u);
  ASSERT_TRUE(original.has_value()) << original.error().to_string();

  atx::vol::Chain widened = under->chains[0];
  const std::vector<std::uint16_t> pairs =
      carry_pair_strikes(widened, spec.spot, sess->inputs().deam);
  ASSERT_EQ(pairs.size(), 1u);
  const std::size_t strike_index = pairs.front();
  for (const Side side : {Side::Call, Side::Put}) {
    const std::size_t quote_index =
        chain_index(static_cast<std::uint16_t>(strike_index), side);
    const double mid = widened.mids[quote_index];
    const double half = 1.25 * 0.5 *
                        (widened.asks[quote_index] - widened.bids[quote_index]);
    ASSERT_GT(mid - half, 0.0);
    widened.bids[quote_index] = mid - half;
    widened.asks[quote_index] = mid + half;
  }

  const auto refreshed = sess->cached_refit_observations(widened, 0u);
  ASSERT_TRUE(refreshed.has_value()) << refreshed.error().to_string();
  ASSERT_EQ(refreshed->size(), original->size());

  bool weight_changed = false;
  for (std::size_t i = 0; i < refreshed->size(); ++i) {
    EXPECT_DOUBLE_EQ((*refreshed)[i].sigma_mkt, (*original)[i].sigma_mkt);
    EXPECT_DOUBLE_EQ((*refreshed)[i].vega, (*original)[i].vega);
    if ((*refreshed)[i].weight_w != (*original)[i].weight_w) {
      weight_changed = true;
    }
  }
  EXPECT_TRUE(weight_changed);
}

// 2e (carry I2): when too few pairs sit inside the ±6% ATM band, the carry
// solve falls back to the nearest co-terminal pairs at ANY moneyness — a carry
// input can therefore live outside the legacy hardcoded ±25% invalidation
// band. A mid change on such a pair must still invalidate the certified cache.
TEST(VolaSession, CachedRefitInvalidatesCarryPairOutsideLegacyBand) {
  SynthPanelSpec spec;
  spec.uid = "WING";
  spec.snapshot_iso = "2026-06-19";
  spec.spot = 100.0;
  spec.r = 0.05;
  const std::string expiry_iso = "2027-06-19";  // ~1.0y
  const double T = year_fraction(spec.snapshot_iso, expiry_iso);
  spec.expiries.push_back(SynthExpiry{expiry_iso, T, S3Params{0.30, -0.50, 0.80}});
  // Every strike sits beyond |K/S - 1| = 0.25: the nearest two-sided carry
  // pairs are all OUTSIDE the legacy certification band.
  spec.strikes = {56.0, 60.0, 64.0, 68.0, 72.0, 128.0, 132.0, 136.0, 140.0, 144.0};
  spec.half_spread_frac = 0.01;

  Universe u;
  const Underlying* under = install(spec, u);
  ASSERT_NE(under, nullptr);

  SessionInputs in = make_inputs(spec);
  // Pin the Andersen-Lake preset so build() keeps n_atm = 3: the carry solve
  // then selects the three nearest pairs (72, 128, plus a tie) — all > 25%.
  in.deam.al_opts = atx::vol::al_fast_opts();
  in.deam.iv_tol = 1.0e-5;
  ASSERT_EQ(in.deam.n_atm, 3u);

  const auto sess = VolaSession::build(*under, in);
  ASSERT_TRUE(sess.has_value()) << sess.error().to_string();
  ASSERT_TRUE(sess->cached_refit_observations(under->chains[0], 0u).has_value());

  // Move the PUT mid at K = 128: a leg of a selected carry pair, but NOT a fit
  // row (the OTM/preferred leg at K > F is the call), and |K/S - 1| = 0.28
  // escapes the legacy ±25% check entirely.
  atx::vol::Chain moved = under->chains[0];
  std::size_t k128 = moved.strikes.size();
  for (std::size_t j = 0; j < moved.strikes.size(); ++j) {
    if (std::fabs(moved.strikes[j] - 128.0) < 1e-9) k128 = j;
  }
  ASSERT_LT(k128, moved.strikes.size());
  const std::size_t idx = chain_index(static_cast<std::uint16_t>(k128), Side::Put);
  moved.bids[idx] *= 1.10;
  moved.asks[idx] *= 1.10;
  moved.mids[idx] *= 1.10;
  const auto reused = sess->cached_refit_observations(moved, 0u);
  EXPECT_FALSE(reused.has_value())
      << "a mid change on a selected carry pair outside the legacy band must invalidate";
}

// 2a (carry C1) positive: with deam.audit_fit_inversions (the risk serving
// policy) the eSSVI FIT rows themselves run the cold-reference audit, so the
// certificate is honestly earned — and the fallback rung stays usable.
TEST(VolaSession, AuditedEssviFitCertifiesInversions) {
  const SynthPanelSpec spec = make_spec();
  Universe u;
  const Underlying* under = install(spec, u);
  ASSERT_NE(under, nullptr);

  SessionInputs in = make_inputs(spec);
  in.deam.audit_fit_inversions = true;
  const auto sess = VolaSession::build(*under, in);
  ASSERT_TRUE(sess.has_value()) << sess.error().to_string();

  const auto& diag = sess->diagnostics();
  EXPECT_EQ(diag.n_slices, std::size_t{4});
  EXPECT_GT(diag.n_iv_proposed, std::size_t{0});
  EXPECT_EQ(diag.n_iv_audited, diag.n_iv_proposed);
  EXPECT_TRUE(diag.inversion_certified);
  for (const auto& sd : sess->slice_diagnostics()) {
    EXPECT_TRUE(sd.inversion_certified);
  }
}

// 2d (carry I5): an expiry whose carry pairs are all kill-flagged fails the
// carry resolve and is dropped from the fitted surface — but the skip must be
// COUNTED in the session diagnostics, never silently absorbed (§5.2).
TEST(VolaSession, CarryFailedExpiryIsCountedInDiagnostics) {
  const SynthPanelSpec spec = make_spec();
  Universe u;
  const auto panel = make_synthetic_american_panel(spec);
  ASSERT_TRUE(panel.has_value());
  const auto uid = data_install(u, panel->frame);
  ASSERT_TRUE(uid.has_value());
  auto under_res = u.get_underlying(*uid);
  ASSERT_TRUE(under_res.has_value());
  Underlying* under = *under_res;
  ASSERT_EQ(under->chains.size(), std::size_t{4});

  // Cross-flag every quote of the second expiry: no co-terminal pair survives
  // leg validity, so carry resolution fails for that chain.
  for (std::uint8_t& flag : under->chains[1].flags) {
    flag |= static_cast<std::uint8_t>(atx::vol::QuoteFlag::Crossed);
  }

  const auto sess = VolaSession::build(*under, make_inputs(spec));
  ASSERT_TRUE(sess.has_value()) << sess.error().to_string();
  EXPECT_EQ(sess->diagnostics().n_slices, std::size_t{3});
  EXPECT_EQ(sess->diagnostics().n_carry_skipped_expiries, std::size_t{1});
}

// 2d follow-up (review I-2): under the risk policy's fit audit, an expiry
// whose rows are all AUDIT-dropped (here: locked quotes — the audit cannot
// evaluate a zero-spread budget, and the accurate fallback re-audit fails the
// same way) falls below the usable-observation floor and is dropped from the
// surface. That audit-created gap must be COUNTED, not silently absorbed —
// the same §5.2 surfacing as a carry skip. Without the audit flag those rows
// would have been fitted, so this gap is new-reachable and must not hide.
TEST(VolaSession, AuditStarvedExpiryIsCountedInDiagnostics) {
  const SynthPanelSpec spec = make_spec();
  Universe u;
  const auto panel = make_synthetic_american_panel(spec);
  ASSERT_TRUE(panel.has_value());
  const auto uid = data_install(u, panel->frame);
  ASSERT_TRUE(uid.has_value());
  auto under_res = u.get_underlying(*uid);
  ASSERT_TRUE(under_res.has_value());
  Underlying* under = *under_res;
  ASSERT_EQ(under->chains.size(), std::size_t{4});

  // Lock every quote of the third expiry (bid == ask == mid, flags clear):
  // the legs stay carry-valid (ask >= bid), so carry resolves — but every fit
  // row fails the zero-spread audit and is dropped by the audit protocol.
  atx::vol::Chain& locked = under->chains[2];
  for (std::size_t i = 0; i < locked.mids.size(); ++i) {
    locked.bids[i] = locked.mids[i];
    locked.asks[i] = locked.mids[i];
  }

  SessionInputs in = make_inputs(spec);
  in.deam.audit_fit_inversions = true;
  const auto sess = VolaSession::build(*under, in);
  ASSERT_TRUE(sess.has_value()) << sess.error().to_string();
  EXPECT_EQ(sess->diagnostics().n_slices, std::size_t{3});
  EXPECT_EQ(sess->diagnostics().n_carry_skipped_expiries, std::size_t{0});
  EXPECT_EQ(sess->diagnostics().n_audit_starved_expiries, std::size_t{1});
}

// rfx task 5 review fixes (perf C1 dedup): the per-slice carry certification a
// session reports must be bit-identical to the SERIAL REFERENCE the
// pre-task-5 certification pass computed — resolve_chain_forward on the
// session's own deam options, whose caches are the CALLER's (this test's:
// empty), never the session-built hot-path caches. Covers, per slice and with
// exact EXPECT_EQ:
//  * finding 1 (Critical): the precomputed-carry indexing bug served slice
//    0's carry to EVERY slice — the i>=1 reference comparisons here fail
//    under that bug (the distinctness assertion at the end documents that the
//    fixture actually gives slices different carry, so the check has power);
//  * finding 2 (Important): with use_correction_cache=true the FIT resolves
//    carry through the session-built caches, but certification must still
//    report the cache-free reference numbers (curve-driver branch: the
//    prepass re-resolves with the caller's caches; eSSVI branch: the reuse
//    gate falls back to the serial recompute).
TEST(VolaSession, CarryCertificationMatchesSerialReferencePerSlice) {
  const SynthPanelSpec spec = make_spec();
  Universe u;
  const Underlying* under = install(spec, u);
  ASSERT_NE(under, nullptr);

  for (const bool use_cache : {false, true}) {
    for (const VolCurveKind kind : {VolCurveKind::Essvi, VolCurveKind::ConvexDense}) {
      SCOPED_TRACE("use_correction_cache=" + std::to_string(use_cache) +
                   " kind=" + std::to_string(static_cast<int>(kind)));
      SessionInputs in = make_inputs(spec);
      in.curve.kind = kind;
      in.use_correction_cache = use_cache;
      // Pin the Andersen-Lake preset so build() does not substitute its own
      // (al_fast/1e-5/n_atm=1) defaults: the in-test serial reference below
      // must run the EXACT effective deam options the build used.
      in.deam.al_opts = atx::vol::al_fast_opts();
      in.deam.iv_tol = 1.0e-5;

      const auto sess = VolaSession::build(*under, in);
      ASSERT_TRUE(sess.has_value()) << sess.error().to_string();
      const auto slices = sess->slice_diagnostics();
      ASSERT_EQ(slices.size(), sess->expiries().size());
      ASSERT_GE(slices.size(), std::size_t{2});

      bool any_distinct_from_first = false;
      for (std::size_t i = 0; i < slices.size(); ++i) {
        SCOPED_TRACE("slice " + std::to_string(i));
        const double T = sess->expiries()[i].T;
        const atx::vol::Chain* chain = nullptr;
        for (const auto& c : under->chains) {
          if (c.T == T) {
            chain = &c;
            break;
          }
        }
        ASSERT_NE(chain, nullptr);
        // The serial reference: what the pre-task-5 certification pass ran —
        // resolve_chain_forward with the session's deam options (in.deam ==
        // the build's effective deam here, al_opts pinned above; caches are
        // the caller's, i.e. empty).
        const auto ref = atx::vol::resolve_chain_forward(
            *chain, in.S, in.r, in.cash_divs, in.now_ts_ns, in.deam);
        ASSERT_TRUE(ref.has_value()) << ref.error().to_string();
        const atx::vol::CarryDiagnostics& rc = ref->carry;
        const atx::vol::SessionCarryDiagnostics& sc = slices[i].carry;
        EXPECT_TRUE(sc.available);
        EXPECT_EQ(sc.n_candidates, rc.n_candidates);
        EXPECT_EQ(sc.n_attempted, rc.n_attempted);
        EXPECT_EQ(sc.n_solved, rc.n_solved);
        EXPECT_EQ(sc.n_retained, rc.n_retained);
        EXPECT_EQ(sc.effective_pair_count, rc.effective_pair_count);
        EXPECT_EQ(sc.dispersion, rc.dispersion);
        EXPECT_EQ(sc.max_leave_one_out_shift, rc.max_leave_one_out_shift);
        EXPECT_EQ(sc.confidence_half_width, rc.confidence_half_width);
        EXPECT_EQ(sc.max_pcp_residual, rc.max_pcp_residual);
        EXPECT_EQ(sc.confident, rc.confident);
        if (i > 0 && (sc.dispersion != slices[0].carry.dispersion ||
                      sc.max_pcp_residual != slices[0].carry.max_pcp_residual ||
                      sc.confidence_half_width !=
                          slices[0].carry.confidence_half_width)) {
          any_distinct_from_first = true;
        }
      }
      // Fixture power guard: the slices must carry DISTINCT diagnostics, or
      // the indexing regression (every slice reads slice 0) would be
      // invisible to the reference comparison above.
      EXPECT_TRUE(any_distinct_from_first);
    }
  }
}
