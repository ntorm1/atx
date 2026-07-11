#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include "atx/vol/arb.hpp"       // QuoteFlag, to_u8
#include "atx/vol/american.hpp"
#include "atx/vol/black76.hpp"   // black76_price, black76_value_and_vega
#include "atx/vol/calib.hpp"
#include "atx/vol/universe.hpp"  // Chain, chain_index

// Shared calibration-infrastructure coverage, ported from the C ats-vol tests
// that exercised `ats_vol_svi_build_observations` /
// `ats_vol_calib_obs_accepted` / `ats_vol_calib_default_opts`.
//
// The chains here are synthetic: every quote's mid is a Black-76 price at a
// flat 20% vol, so IV inversion must recover ~0.20 and the ported weight /
// vega formulas are checkable in closed form. Bid/ask straddle the mid by a
// fixed half-spread tight enough to clear every default filter.

namespace {

using atx::vol::black76_price;
using atx::vol::black76_value_and_vega;
using atx::vol::build_observations;
using atx::vol::build_observations_european;
using atx::vol::calib_default_opts;
using atx::vol::CalibAnchorKind;
using atx::vol::CalibLossKind;
using atx::vol::CalibOpts;
using atx::vol::Chain;
using atx::vol::chain_index;
using atx::vol::ErrorCode;
using atx::vol::EssviRhoMode;
using atx::vol::FitObs;
using atx::vol::obs_accepted;
using atx::vol::ObsSet;
using atx::vol::OptimizationLevel;
using atx::vol::QuoteFlag;
using atx::vol::ResidualBasisKind;
using atx::vol::Side;
using atx::vol::to_u8;
using atx::vol::american_price;
using atx::vol::AmericanMethod;

// ── Synthetic-chain fixture ─────────────────────────────────────────────

constexpr double kF = 100.0;
constexpr double kT = 0.5;
constexpr double kVol = 0.20;
constexpr double kHalfSpread = 0.02;
const double kDf = std::exp(-0.03 * kT);  // discount factor for r = 3%

Side preferred_side(double K) noexcept { return (K >= kF) ? Side::Call : Side::Put; }

// Build a chain whose every (strike, side) mid is the flat-vol B76 price, with
// bid/ask = mid ∓ kHalfSpread and no quote flags set.
Chain make_priced_chain(const std::vector<double> &strikes) {
  Chain c;
  c.uid = 1u;
  c.expiry_id = 0u;
  c.T = kT;
  c.strikes = strikes;

  const std::size_t n2 = strikes.size() * 2u;
  c.bids.assign(n2, 0.0);
  c.asks.assign(n2, 0.0);
  c.mids.assign(n2, 0.0);
  c.ivs.assign(n2, std::numeric_limits<double>::quiet_NaN());
  c.bid_sizes.assign(n2, 1);
  c.ask_sizes.assign(n2, 1);
  c.ts_ns.assign(n2, 0);
  c.flags.assign(n2, 0u);

  for (std::size_t s = 0; s < strikes.size(); ++s) {
    const double K = strikes[s];
    for (int side_i = 0; side_i < 2; ++side_i) {
      const auto side = static_cast<Side>(static_cast<std::uint8_t>(side_i));
      const std::size_t idx = chain_index(static_cast<std::uint16_t>(s), side);
      const double mid = black76_price(kF, K, kT, kVol, kDf, side);
      c.mids[idx] = mid;
      c.bids[idx] = mid - kHalfSpread;
      c.asks[idx] = mid + kHalfSpread;
    }
  }
  return c;
}

Chain make_american_chain(const std::vector<double>& strikes, double T,
                          double r, double q, double sigma = 0.22) {
  Chain c;
  c.uid = 2u;
  c.expiry_id = 1u;
  c.T = T;
  c.strikes = strikes;
  const std::size_t n2 = 2u * strikes.size();
  c.bids.assign(n2, 0.0);
  c.asks.assign(n2, 0.0);
  c.mids.assign(n2, 0.0);
  c.ivs.assign(n2, std::numeric_limits<double>::quiet_NaN());
  c.bid_sizes.assign(n2, 10);
  c.ask_sizes.assign(n2, 10);
  c.ts_ns.assign(n2, 0);
  c.flags.assign(n2, 0u);
  for (std::size_t i = 0; i < strikes.size(); ++i) {
    for (Side side : {Side::Call, Side::Put}) {
      const std::size_t idx = chain_index(static_cast<std::uint16_t>(i), side);
      const auto p = american_price(100.0, strikes[i], T, sigma, r, q, side,
                                    AmericanMethod::AndersenLake, std::nullopt);
      EXPECT_TRUE(p.has_value());
      const double mid = p ? *p : 1.0;
      const double half = std::fmin(0.002, 0.10 * mid);
      c.mids[idx] = mid;
      c.bids[idx] = mid - half;
      c.asks[idx] = mid + half;
    }
  }
  return c;
}

// ── build_observations: happy path + field correctness ──────────────────

TEST(BuildObservations, PricedChain_AcceptsPreferredSideWithCorrectFields) {
  const std::vector<double> strikes{90.0, 95.0, 100.0, 105.0, 110.0};
  const Chain c = make_priced_chain(strikes);

  const auto res = build_observations(c, kF, kT, kDf, calib_default_opts());
  ASSERT_TRUE(res.has_value());
  const ObsSet &os = *res;
  ASSERT_EQ(os.obs.size(), 5u);
  EXPECT_EQ(os.n_dropped, 0u);  // every non-preferred leg is a silent skip

  for (std::size_t i = 0; i < strikes.size(); ++i) {
    const double K = strikes[i];
    const Side side = preferred_side(K);
    const FitObs &o = os.obs[i];  // obs order follows strike order (one leg each)

    EXPECT_EQ(o.side, side);
    EXPECT_NEAR(o.K, K, 1e-12);
    EXPECT_DOUBLE_EQ(o.F, kF);
    EXPECT_DOUBLE_EQ(o.df, kDf);
    EXPECT_NEAR(o.k, std::log(K / kF), 1e-12);

    // IV inversion recovers the flat vol the mids were priced from.
    EXPECT_NEAR(o.sigma_mkt, kVol, 1e-4);
    EXPECT_NEAR(o.w_mkt, o.sigma_mkt * o.sigma_mkt * kT, 1e-15);

    // Spread + noise fields.
    EXPECT_NEAR(o.spread, 2.0 * kHalfSpread, 1e-12);
    EXPECT_NEAR(o.noise_sigma, o.spread / o.vega, 1e-12);

    // ANCHOR_MID default: the stored target is the symmetric mid.
    EXPECT_NEAR(o.mid, black76_price(kF, K, kT, kVol, kDf, side), 1e-12);

    // Vega reproduces black76_value_and_vega at the recovered vol.
    const double vega_exp = black76_value_and_vega(kF, K, kT, o.sigma_mkt, kDf, side).vega;
    EXPECT_NEAR(o.vega, vega_exp, 1e-9);

    // weight_w reproduces the ported w-space formula exactly.
    const double denom_w = 2.0 * o.sigma_mkt * kT;
    const double weight_sigma = (o.vega * o.vega) / (o.spread * o.spread + 1e-18);
    const double weight_w = weight_sigma / (denom_w * denom_w + 1e-18);
    EXPECT_NEAR(o.weight_w, weight_w, std::fabs(weight_w) * 1e-9 + 1e-9);
    EXPECT_DOUBLE_EQ(o.active_weight_w, o.weight_w);  // seeded equal for IRLS
  }
}

TEST(BuildObservations, AnchorBid_WritesBidIntoMidTargetButInvertsRawMid) {
  const std::vector<double> strikes{90.0, 95.0, 100.0, 105.0, 110.0};
  const Chain c = make_priced_chain(strikes);
  CalibOpts opts = calib_default_opts();
  opts.anchor_kind = CalibAnchorKind::Bid;

  const auto res = build_observations(c, kF, kT, kDf, opts);
  ASSERT_TRUE(res.has_value());
  for (const FitObs &o : res->obs) {
    const Side side = preferred_side(o.K);
    const double mid = black76_price(kF, o.K, kT, kVol, kDf, side);
    EXPECT_NEAR(o.mid, mid - kHalfSpread, 1e-12);  // target swapped to the bid
    EXPECT_NEAR(o.sigma_mkt, kVol, 1e-4);          // inversion still used raw mid
  }
}

// ── build_observations: filter cascade ──────────────────────────────────

TEST(BuildObservations, FewerThanFiveSurvive_ReturnsNotFound) {
  // 95 put, 100 call, 105 call -> only 3 observations survive.
  const Chain c = make_priced_chain({95.0, 100.0, 105.0});
  const auto res = build_observations(c, kF, kT, kDf, calib_default_opts());
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), ErrorCode::NotFound);
}

TEST(BuildObservations, KillFlagOnPreferredLeg_DropsRow) {
  const std::vector<double> strikes{85.0, 90.0, 95.0, 100.0, 105.0, 110.0};
  Chain c = make_priced_chain(strikes);
  // 85 < F -> the put is the preferred leg; flag it out.
  c.flags[chain_index(0, Side::Put)] = to_u8(QuoteFlag::Locked);

  const auto res = build_observations(c, kF, kT, kDf, calib_default_opts());
  ASSERT_TRUE(res.has_value());
  EXPECT_EQ(res->obs.size(), 5u);
  EXPECT_EQ(res->n_dropped, 1u);
  for (const FitObs &o : res->obs) {
    EXPECT_GT(std::fabs(o.K - 85.0), 1e-9);  // the 85 row is gone
  }
}

TEST(BuildObservations, KillFlagOnNonPreferredLeg_CountsDropButKeepsObs) {
  // The prefer-call gate sits AFTER the flag check, so a flagged NON-preferred
  // leg is still counted as a drop while the preferred leg survives (exact C
  // ordering).
  const std::vector<double> strikes{85.0, 90.0, 95.0, 100.0, 105.0, 110.0};
  Chain c = make_priced_chain(strikes);
  c.flags[chain_index(0, Side::Call)] = to_u8(QuoteFlag::Halted);  // 85 call = non-preferred

  const auto res = build_observations(c, kF, kT, kDf, calib_default_opts());
  ASSERT_TRUE(res.has_value());
  EXPECT_EQ(res->obs.size(), 6u);   // preferred 85 put still present
  EXPECT_EQ(res->n_dropped, 1u);    // non-preferred flagged leg counted
}

TEST(BuildObservations, CrossedQuoteOnPreferredLeg_Dropped) {
  const std::vector<double> strikes{85.0, 90.0, 95.0, 100.0, 105.0, 110.0};
  Chain c = make_priced_chain(strikes);
  const std::size_t idx = chain_index(3, Side::Call);  // 100 call preferred
  c.asks[idx] = c.bids[idx];                            // ask == bid violates ask > bid

  const auto res = build_observations(c, kF, kT, kDf, calib_default_opts());
  ASSERT_TRUE(res.has_value());
  EXPECT_EQ(res->obs.size(), 5u);
  for (const FitObs &o : res->obs) {
    EXPECT_GT(std::fabs(o.K - 100.0), 1e-9);
  }
}

TEST(BuildObservations, WideSpreadVolOnPreferredLeg_Dropped) {
  const std::vector<double> strikes{85.0, 90.0, 95.0, 100.0, 105.0, 110.0};
  Chain c = make_priced_chain(strikes);
  const std::size_t idx = chain_index(3, Side::Call);  // 100 call (ATM, high vega)
  const double mid = c.mids[idx];
  c.bids[idx] = mid - 1.0;
  c.asks[idx] = mid + 1.0;  // spread 2.0 -> spread/vega ~0.07 > max_spread_vol 0.05,
                            // while (ask-bid)/mid ~0.36 stays under max_spread_to_mid_pct

  const auto res = build_observations(c, kF, kT, kDf, calib_default_opts());
  ASSERT_TRUE(res.has_value());
  EXPECT_EQ(res->obs.size(), 5u);
  for (const FitObs &o : res->obs) {
    EXPECT_GT(std::fabs(o.K - 100.0), 1e-9);
  }
}

TEST(BuildObservations, NonPositiveForward_ReturnsInvalidArgument) {
  const Chain c = make_priced_chain({90.0, 95.0, 100.0, 105.0, 110.0});
  const auto res = build_observations(c, 0.0, kT, kDf, calib_default_opts());
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), ErrorCode::InvalidArgument);
}

TEST(BuildObservations, NonPositiveT_ReturnsInvalidArgument) {
  const Chain c = make_priced_chain({90.0, 95.0, 100.0, 105.0, 110.0});
  const auto res = build_observations(c, kF, 0.0, kDf, calib_default_opts());
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), ErrorCode::InvalidArgument);
}

TEST(BuildObservationsEuropean, ShortcutIsColdAuditedAndFallsBackWhenNeeded) {
  constexpr double T = 1.0;
  constexpr double r = 0.08;
  constexpr double q = 0.08;
  constexpr double F = 100.0;
  const double df = std::exp(-r * T);
  const Chain chain = make_american_chain(
      {85.0, 90.0, 95.0, 100.0, 105.0, 110.0, 115.0}, T, r, q);
  CalibOpts opts = calib_default_opts();
  opts.max_spread_vol = 1.0;
  opts.max_otm_shortcut_premium_spread_frac = 100.0;
  opts.max_inversion_residual_half_spreads = 0.01;
  opts.min_otm_shortcut_T = 0.0;

  const auto result = build_observations_european(
      chain, 100.0, r, F, T, df, opts, {}, std::nullopt, 1.0e-7, 64,
      AmericanMethod::AndersenLake);
  ASSERT_TRUE(result.has_value())
      << (result ? std::string{} : result.error().to_string());
  const auto& diag = result->deam_audit;
  EXPECT_GT(diag.shortcut.n_proposed, 0u);
  EXPECT_EQ(diag.shortcut.n_audited, diag.shortcut.n_proposed);
  EXPECT_GT(diag.n_accurate_fallback, 0u);
  EXPECT_EQ(diag.n_rejected_residual, 0u);
  EXPECT_LE(diag.accurate.max_residual_half_spreads,
            opts.max_inversion_residual_half_spreads);
  EXPECT_GE(result->obs.size(), 5u);
}

TEST(BuildObservationsEuropean, UltraShortTenorBypassesShortcut) {
  constexpr double T = 1.0 / 365.25;
  constexpr double r = 0.04;
  constexpr double q = 0.02;
  const double F = 100.0 * std::exp((r - q) * T);
  const double df = std::exp(-r * T);
  const Chain chain = make_american_chain(
      {97.0, 98.0, 99.0, 100.0, 101.0, 102.0, 103.0}, T, r, q, 0.30);
  CalibOpts opts = calib_default_opts();
  opts.max_spread_vol = 5.0;
  opts.min_vega_weight = 0.0;
  opts.max_otm_shortcut_premium_spread_frac = 100.0;
  opts.min_otm_shortcut_T = 7.0 / 365.25;

  const auto result = build_observations_european(
      chain, 100.0, r, F, T, df, opts, {}, std::nullopt, 1.0e-7, 64,
      AmericanMethod::AndersenLake);
  ASSERT_TRUE(result.has_value())
      << (result ? std::string{} : result.error().to_string());
  EXPECT_EQ(result->deam_audit.shortcut.n_proposed, 0u);
  EXPECT_GT(result->deam_audit.n_forced_short_tenor, 0u);
  EXPECT_EQ(result->deam_audit.accurate.n_accepted, result->obs.size());
}

// ── obs_accepted: agreement with the builder ─────────────────────────────

TEST(ObsAccepted, AgreesWithBuilderRowByRow) {
  const std::vector<double> strikes{85.0, 90.0, 95.0, 100.0, 105.0, 110.0};
  const Chain c = make_priced_chain(strikes);
  const CalibOpts opts = calib_default_opts();

  const auto res = build_observations(c, kF, kT, kDf, opts);
  ASSERT_TRUE(res.has_value());

  for (std::size_t s = 0; s < strikes.size(); ++s) {
    for (int side_i = 0; side_i < 2; ++side_i) {
      const auto side = static_cast<Side>(static_cast<std::uint8_t>(side_i));
      const auto acc =
          obs_accepted(c, static_cast<std::uint16_t>(s), side, kF, kT, kDf, opts);

      bool in_builder = false;
      double sigma_builder = 0.0;
      for (const FitObs &o : res->obs) {
        if (std::fabs(o.K - strikes[s]) < 1e-9 && o.side == side) {
          in_builder = true;
          sigma_builder = o.sigma_mkt;
          break;
        }
      }

      EXPECT_EQ(acc.has_value(), in_builder);
      if (in_builder) {
        ASSERT_TRUE(acc.has_value());
        EXPECT_NEAR(*acc, sigma_builder, 1e-12);
      } else {
        ASSERT_FALSE(acc.has_value());
        EXPECT_EQ(acc.error().code(), ErrorCode::NotFound);
      }
    }
  }
}

TEST(ObsAccepted, NonPreferredSide_ReturnsNotFound) {
  const Chain c = make_priced_chain({90.0, 95.0, 100.0, 105.0, 110.0});
  // strike index 4 = 110 >= F -> preferred is the call; asking the put rejects.
  const auto acc = obs_accepted(c, 4, Side::Put, kF, kT, kDf, calib_default_opts());
  ASSERT_FALSE(acc.has_value());
  EXPECT_EQ(acc.error().code(), ErrorCode::NotFound);
}

TEST(ObsAccepted, OutOfRangeStrike_ReturnsInvalidArgument) {
  const Chain c = make_priced_chain({90.0, 95.0, 100.0, 105.0, 110.0});
  const auto acc = obs_accepted(c, 99, Side::Call, kF, kT, kDf, calib_default_opts());
  ASSERT_FALSE(acc.has_value());
  EXPECT_EQ(acc.error().code(), ErrorCode::InvalidArgument);
}

TEST(ObsAccepted, NonPositiveDiscount_ReturnsInvalidArgument) {
  const Chain c = make_priced_chain({90.0, 95.0, 100.0, 105.0, 110.0});
  const auto acc = obs_accepted(c, 2, Side::Call, kF, kT, 0.0, calib_default_opts());
  ASSERT_FALSE(acc.has_value());
  EXPECT_EQ(acc.error().code(), ErrorCode::InvalidArgument);
}

// ── default options parity with the C library ────────────────────────────

TEST(CalibOpts, DefaultOpts_MatchCLibraryValues) {
  const CalibOpts o = calib_default_opts();

  EXPECT_EQ(o.max_outer_iter, 4u);
  EXPECT_EQ(o.max_inner_iter, 12u);
  EXPECT_DOUBLE_EQ(o.tol_param, 1.0e-9);
  EXPECT_DOUBLE_EQ(o.tol_residual, 1.0e-10);
  EXPECT_DOUBLE_EQ(o.huber_k, 1.5);
  EXPECT_DOUBLE_EQ(o.min_vega_weight, 1.0e-6);
  EXPECT_DOUBLE_EQ(o.max_spread_vol, 0.05);
  EXPECT_DOUBLE_EQ(o.max_weight, 1.0e3);
  EXPECT_DOUBLE_EQ(o.prior_strength, 0.0);
  EXPECT_EQ(o.essvi_rho_mode, EssviRhoMode::PerSlice);
  EXPECT_EQ(o.optimization_level, OptimizationLevel::Trading);
  EXPECT_DOUBLE_EQ(o.essvi_fallback_rmse_threshold, 0.01);
  EXPECT_EQ(o.n_butterfly_grid, 200u);
  EXPECT_EQ(o.max_iter_quick_mark, 8u);
  EXPECT_EQ(o.max_iter_trading, 35u);
  EXPECT_EQ(o.max_iter_risk, 100u);
  EXPECT_EQ(o.max_iter_reference, 250u);
  EXPECT_EQ(o.max_iter_cold_fast, 10u);  // impl = 10 (header comment's "5" is stale)
  EXPECT_DOUBLE_EQ(o.wing_floor_alpha, 0.0);
  EXPECT_TRUE(o.lee_bound_project);
  EXPECT_FALSE(o.morozov_stop);  // impl = 0 (header comment's "1" is stale)
  EXPECT_DOUBLE_EQ(o.morozov_tau, 1.1);
  EXPECT_TRUE(o.validate_no_arb);
  EXPECT_TRUE(o.residual_disable);
  EXPECT_EQ(o.residual_basis_kind, ResidualBasisKind::None);
  EXPECT_EQ(o.residual_n_basis_terms, 0u);
  EXPECT_DOUBLE_EQ(o.residual_ridge_factor, 0.0);
  EXPECT_EQ(o.loss_kind, CalibLossKind::Mid);
  EXPECT_EQ(o.anchor_kind, CalibAnchorKind::Mid);
  EXPECT_FALSE(o.essvi_asymmetric_rho);
  EXPECT_EQ(o.min_obs_per_slice, 4u);
  EXPECT_DOUBLE_EQ(o.max_post_fit_sigma, 2.0);
  EXPECT_DOUBLE_EQ(o.max_spread_to_mid_pct, 0.60);  // impl = 0.60 (comment's "0.40" is stale)
}

}  // namespace
