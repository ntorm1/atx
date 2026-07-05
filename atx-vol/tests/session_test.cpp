#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "atx/vol/american.hpp"
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

using atx::vol::chain_index;
using atx::vol::data_install;
using atx::vol::DividendEvent;
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
  ASSERT_TRUE(fast.deam.al_opts.has_value());
  EXPECT_EQ(fast.deam.al_opts->n_collocation, al_fast_opts().n_collocation);
  EXPECT_EQ(fast.deam.n_atm, std::size_t{1});
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
  EXPECT_EQ(robust.deam.n_atm, std::size_t{3});
  EXPECT_EQ(robust.calendar_repair, CalendarRepair::MonotoneFit);

  SessionInputs hft;
  apply_fit_preset(hft, FitPreset::Hft);
  EXPECT_EQ(hft.calendar_repair, CalendarRepair::MonotoneFit);
  EXPECT_EQ(hft.deam.n_atm, std::size_t{1});  // fast borrow

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
