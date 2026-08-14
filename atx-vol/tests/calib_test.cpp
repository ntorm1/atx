#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "atx/vol/api/pricing/american.hpp"
#include "atx/vol/api/fitting/arb.hpp"     // QuoteFlag, to_u8
#include "atx/vol/api/pricing/black76.hpp" // black76_price, black76_value_and_vega
#include "atx/vol/api/fitting/calib.hpp"
#include "fitting/calib_detail.hpp" // SharedLaneBracket, shared_lane_residual_within_budget
#include "fitting/counters.hpp"
#include "atx/vol/api/marketdata/universe.hpp" // Chain, chain_index

// Shared calibration-infrastructure coverage, ported from the C ats-vol tests
// that exercised `ats_vol_svi_build_observations` /
// `ats_vol_calib_obs_accepted` / `ats_vol_calib_default_opts`.
//
// The chains here are synthetic: every quote's mid is a Black-76 price at a
// flat 20% vol, so IV inversion must recover ~0.20 and the ported weight /
// vega formulas are checkable in closed form. Bid/ask straddle the mid by a
// fixed half-spread tight enough to clear every default filter.

namespace {

using atx::vol::american_price;
using atx::vol::AmericanMethod;
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
using atx::vol::deam_inversion_certified;
using atx::vol::DeAmAuditDiagnostics;
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
using atx::vol::validate_calib_options;

// ── Synthetic-chain fixture ─────────────────────────────────────────────

constexpr double kF = 100.0;
constexpr double kT = 0.5;
constexpr double kVol = 0.20;
constexpr double kHalfSpread = 0.02;
const double kDf = std::exp(-0.03 * kT); // discount factor for r = 3%

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

Chain make_american_chain(const std::vector<double> &strikes, double T, double r, double q,
                          double sigma = 0.22) {
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

// Smile variant of make_american_chain: strike i is priced at its OWN vol
// sigmas[i] rather than one flat vol across the board. A flat board gives the
// shared interpolant a ~3x-wide sigma-box that nine Chebyshev nodes fit almost
// exactly, which is why it cannot evidence anything about interpolation stress.
Chain make_american_smile_chain(const std::vector<double> &strikes,
                               const std::vector<double> &sigmas, double T, double r, double q) {
  Chain c;
  c.uid = 3u;
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
      const auto p = american_price(100.0, strikes[i], T, sigmas[i], r, q, side,
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
  EXPECT_EQ(os.n_dropped, 0u); // every non-preferred leg is a silent skip

  for (std::size_t i = 0; i < strikes.size(); ++i) {
    const double K = strikes[i];
    const Side side = preferred_side(K);
    const FitObs &o = os.obs[i]; // obs order follows strike order (one leg each)

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
    const double weight_w =
        std::min(weight_sigma / (denom_w * denom_w + 1e-18), calib_default_opts().max_weight);
    EXPECT_NEAR(o.weight_w, weight_w, std::fabs(weight_w) * 1e-9 + 1e-9);
    EXPECT_DOUBLE_EQ(o.active_weight_w, o.weight_w); // seeded equal for IRLS
  }
}

TEST(BuildObservations, MaxWeightClipsStoredAndActiveWeights) {
  const Chain c = make_priced_chain({90.0, 95.0, 100.0, 105.0, 110.0});
  CalibOpts opts = calib_default_opts();
  opts.max_weight = 0.25;

  const auto result = build_observations(c, kF, kT, kDf, opts);
  ASSERT_TRUE(result.has_value()) << result.error().to_string();
  ASSERT_FALSE(result->obs.empty());
  for (const FitObs &observation : result->obs) {
    EXPECT_DOUBLE_EQ(observation.weight_w, opts.max_weight);
    EXPECT_DOUBLE_EQ(observation.active_weight_w, observation.weight_w);
  }
}

TEST(BuildObservations, RejectsNonPositiveOrNonFiniteMaxWeight) {
  const Chain c = make_priced_chain({90.0, 95.0, 100.0, 105.0, 110.0});
  for (const double invalid : {0.0, -1.0, std::numeric_limits<double>::infinity(),
                               std::numeric_limits<double>::quiet_NaN()}) {
    CalibOpts opts = calib_default_opts();
    opts.max_weight = invalid;
    const auto result = build_observations(c, kF, kT, kDf, opts);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
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
    EXPECT_NEAR(o.mid, mid - kHalfSpread, 1e-12); // target swapped to the bid
    EXPECT_NEAR(o.sigma_mkt, kVol, 1e-4);         // inversion still used raw mid
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
    EXPECT_GT(std::fabs(o.K - 85.0), 1e-9); // the 85 row is gone
  }
}

TEST(BuildObservations, KillFlagOnNonPreferredLeg_CountsDropButKeepsObs) {
  // The prefer-call gate sits AFTER the flag check, so a flagged NON-preferred
  // leg is still counted as a drop while the preferred leg survives (exact C
  // ordering).
  const std::vector<double> strikes{85.0, 90.0, 95.0, 100.0, 105.0, 110.0};
  Chain c = make_priced_chain(strikes);
  c.flags[chain_index(0, Side::Call)] = to_u8(QuoteFlag::Halted); // 85 call = non-preferred

  const auto res = build_observations(c, kF, kT, kDf, calib_default_opts());
  ASSERT_TRUE(res.has_value());
  EXPECT_EQ(res->obs.size(), 6u); // preferred 85 put still present
  EXPECT_EQ(res->n_dropped, 1u);  // non-preferred flagged leg counted
}

TEST(BuildObservations, CrossedQuoteOnPreferredLeg_Dropped) {
  const std::vector<double> strikes{85.0, 90.0, 95.0, 100.0, 105.0, 110.0};
  Chain c = make_priced_chain(strikes);
  const std::size_t idx = chain_index(3, Side::Call); // 100 call preferred
  c.asks[idx] = c.bids[idx];                          // ask == bid violates ask > bid

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
  const std::size_t idx = chain_index(3, Side::Call); // 100 call (ATM, high vega)
  const double mid = c.mids[idx];
  c.bids[idx] = mid - 1.0;
  c.asks[idx] = mid + 1.0; // spread 2.0 -> spread/vega ~0.07 > max_spread_vol 0.05,
                           // while (ask-bid)/mid ~0.36 stays under max_spread_to_mid_pct

  const auto res = build_observations(c, kF, kT, kDf, calib_default_opts());
  ASSERT_TRUE(res.has_value());
  EXPECT_EQ(res->obs.size(), 5u);
  for (const FitObs &o : res->obs) {
    EXPECT_GT(std::fabs(o.K - 100.0), 1e-9);
  }
}

// ── build_observations: ITM-leg fallback (W1-B / F21) ───────────────────
//
// The one-leg-per-strike rule keeps the OTM leg and skips the ITM one. On a
// thin board the OTM leg is frequently unquoted (an unset side arrives as
// bid = ask = 0), so the strike contributes NOTHING even though its ITM leg is
// a clean two-sided quote. These pin the opt-in fallback and, just as
// importantly, the cases where it must NOT fire.

// Erase one leg the way the loader does: an unset side never reaches the chain,
// so its SoA slot keeps the zero-initialized value.
void unquote_leg(Chain &c, std::uint16_t strike_index, Side side) {
  const std::size_t idx = chain_index(strike_index, side);
  c.bids[idx] = 0.0;
  c.asks[idx] = 0.0;
  c.mids[idx] = 0.0;
}

TEST(BuildObservations, UnquotedOtmLeg_WithoutFallback_DiscardsTheWholeStrike) {
  const std::vector<double> strikes{85.0, 90.0, 95.0, 100.0, 105.0, 110.0};
  Chain c = make_priced_chain(strikes);
  unquote_leg(c, 5u, Side::Call); // K = 110 > F: the OTM call is the preferred leg

  const auto res = build_observations(c, kF, kT, kDf, calib_default_opts());
  ASSERT_TRUE(res.has_value()) << res.error().to_string();
  EXPECT_EQ(res->obs.size(), 5u);
  for (const FitObs &o : res->obs) {
    EXPECT_GT(std::fabs(o.K - 110.0), 1e-9) << "the 110 strike must contribute nothing";
  }
}

TEST(BuildObservations, UnquotedOtmLeg_WithFallback_UsesTheItmLeg) {
  const std::vector<double> strikes{85.0, 90.0, 95.0, 100.0, 105.0, 110.0};
  Chain c = make_priced_chain(strikes);
  unquote_leg(c, 5u, Side::Call);
  CalibOpts opts = calib_default_opts();
  opts.itm_leg_fallback = true;

  const auto res = build_observations(c, kF, kT, kDf, opts);
  ASSERT_TRUE(res.has_value()) << res.error().to_string();
  ASSERT_EQ(res->obs.size(), 6u);
  EXPECT_EQ(res->n_itm_fallback, 1u);
  // Rows stay in ascending source-strike order, one per strike, so the rescued
  // 110 row is last — and it is the PUT.
  const FitObs &rescued = res->obs.back();
  EXPECT_NEAR(rescued.K, 110.0, 1e-12);
  EXPECT_EQ(rescued.side, Side::Put);
  // The ITM put inverts to the same flat vol the board was priced at: put-call
  // parity holds in IV space, which is why the ITM leg is usable at all.
  EXPECT_NEAR(rescued.sigma_mkt, kVol, 1e-4);
  // Provenance is re-keyed onto the leg that was actually used.
  ASSERT_EQ(res->provenance.size(), strikes.size());
  EXPECT_EQ(res->provenance.back().side, Side::Put);
  EXPECT_EQ(res->provenance.back().rejection, atx::vol::ObsRejectionReason::None);
}

TEST(BuildObservations, QuotedButRejectedOtmLeg_DoesNotSubstituteTheItmLeg) {
  // The fallback answers "this strike has no OTM quote", not "this strike's OTM
  // quote is poor". A quoted-but-filtered OTM leg means the strike itself is
  // bad, so the ITM leg must NOT be substituted for it.
  const std::vector<double> strikes{85.0, 90.0, 95.0, 100.0, 105.0, 110.0};
  Chain c = make_priced_chain(strikes);
  const std::size_t idx = chain_index(3, Side::Call); // 100 call, wide in vol units
  const double mid = c.mids[idx];
  c.bids[idx] = mid - 1.0;
  c.asks[idx] = mid + 1.0;
  CalibOpts opts = calib_default_opts();
  opts.itm_leg_fallback = true;

  const auto res = build_observations(c, kF, kT, kDf, opts);
  ASSERT_TRUE(res.has_value()) << res.error().to_string();
  EXPECT_EQ(res->obs.size(), 5u);
  EXPECT_EQ(res->n_itm_fallback, 0u);
  for (const FitObs &o : res->obs) {
    EXPECT_GT(std::fabs(o.K - 100.0), 1e-9);
  }
}

TEST(BuildObservations, ItmFallbackIsInertWhenEveryOtmLegIsQuoted) {
  // A dense board must be bit-identical with the flag on: every preferred leg
  // is quotable, so the fallback never runs.
  const std::vector<double> strikes{85.0, 90.0, 95.0, 100.0, 105.0, 110.0};
  const Chain c = make_priced_chain(strikes);
  CalibOpts on = calib_default_opts();
  on.itm_leg_fallback = true;

  const auto base = build_observations(c, kF, kT, kDf, calib_default_opts());
  const auto with_fallback = build_observations(c, kF, kT, kDf, on);
  ASSERT_TRUE(base.has_value());
  ASSERT_TRUE(with_fallback.has_value());
  ASSERT_EQ(base->obs.size(), with_fallback->obs.size());
  EXPECT_EQ(with_fallback->n_itm_fallback, 0u);
  EXPECT_EQ(base->n_dropped, with_fallback->n_dropped);
  for (std::size_t i = 0; i < base->obs.size(); ++i) {
    EXPECT_EQ(base->obs[i].side, with_fallback->obs[i].side);
    EXPECT_DOUBLE_EQ(base->obs[i].K, with_fallback->obs[i].K);
    EXPECT_DOUBLE_EQ(base->obs[i].sigma_mkt, with_fallback->obs[i].sigma_mkt);
    EXPECT_DOUBLE_EQ(base->obs[i].weight_w, with_fallback->obs[i].weight_w);
  }
}

TEST(BuildObservations, ItmFallbackRescuesASliceTheOtmRuleWouldStarve) {
  // The thin-board shape: every strike sits above the forward and only the ITM
  // puts are quoted. Without the fallback the slice yields zero rows.
  const std::vector<double> strikes{105.0, 110.0, 115.0, 120.0, 125.0, 130.0};
  Chain c = make_priced_chain(strikes);
  for (std::uint16_t s = 0; s < strikes.size(); ++s) {
    unquote_leg(c, s, Side::Call);
  }

  const auto starved = build_observations(c, kF, kT, kDf, calib_default_opts());
  ASSERT_FALSE(starved.has_value());
  EXPECT_EQ(starved.error().code(), ErrorCode::NotFound);

  CalibOpts opts = calib_default_opts();
  opts.itm_leg_fallback = true;
  const auto rescued = build_observations(c, kF, kT, kDf, opts);
  ASSERT_TRUE(rescued.has_value()) << rescued.error().to_string();
  EXPECT_EQ(rescued->obs.size(), strikes.size());
  EXPECT_EQ(rescued->n_itm_fallback, static_cast<std::uint32_t>(strikes.size()));
  for (const FitObs &o : rescued->obs) {
    EXPECT_EQ(o.side, Side::Put);
    EXPECT_NEAR(o.sigma_mkt, kVol, 1e-4);
  }
}

TEST(BuildObservations, ItmFallbackRefusalNamesTheItmLegsOwnRejection) {
  // When the fallback runs and the ITM leg is ALSO unusable, the retained
  // rejection must name the ITM leg's binding condition — that is the reason
  // the strike is dead, and reporting the OTM leg's "no quote" would hide it.
  const std::vector<double> strikes{105.0, 110.0, 115.0, 120.0, 125.0, 130.0};
  Chain c = make_priced_chain(strikes);
  for (std::uint16_t s = 0; s < strikes.size(); ++s) {
    unquote_leg(c, s, Side::Call);
    const std::size_t put = chain_index(s, Side::Put);
    c.bids[put] = c.mids[put] - 5.0; // vol-wide: rejected by max_spread_vol
    c.asks[put] = c.mids[put] + 5.0;
  }
  CalibOpts opts = calib_default_opts();
  opts.itm_leg_fallback = true;

  const auto res = build_observations(c, kF, kT, kDf, opts);
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), ErrorCode::NotFound);
  EXPECT_NE(res.error().to_string().find("SpreadVol"), std::string::npos)
      << res.error().to_string();
}

// ── build_observations: one-sided quotes as bounds (T6) ─────────────────
//
// A leg quoted `0.00 x a` is not a missing observation: the ask is a hard upper
// bound on that strike's price and no-arbitrage supplies the lower one. These
// pin the admitted band, the width it is weighted at, and — just as importantly
// — every shape that must still be refused.

// Withdraw the bid the way a venue does when nobody is bidding: the ask stands,
// the bid reads 0, and the loader's mid = 0.5*(bid + ask) halves with it.
void unbid_leg(Chain &c, std::uint16_t strike_index, Side side) {
  const std::size_t idx = chain_index(strike_index, side);
  c.bids[idx] = 0.0;
  c.mids[idx] = 0.5 * c.asks[idx];
}

// A bid-less quote is admitted over its WHOLE ask, so its width in vol units is
// of order the strike's own implied vol — an order of magnitude above the
// half-spread of a two-sided row. That is why the absolute gate, not the
// relative one, decides these rows, and why the tests that expect admission run
// at the loose end of the shipped `max_spread_vol` range (VolProduct 0.35,
// OrdinarySingleName / IlliquidSmallCap 0.25) rather than at the SPY-tuned 0.05.
constexpr double kBoundStrike = 130.0;
constexpr double kLooseSpreadVol = 0.30;
const std::vector<double> kBoundStrikes{110.0, 115.0, 120.0, 125.0, kBoundStrike, 135.0};

// Bound admission is a LAST RESORT: it runs only after the two-sided population
// has already left the slice below the five-row floor. Every test that expects a
// bound to be ADMITTED therefore has to start from a starved slice — four
// two-sided calls at 110-125, and bid-less ones from 130 out. `kBoundStrike`
// (130) is the first bid-less strike in both fixtures, so the shape assertions
// read the same row in each.
const std::vector<double> kStarvedStrikes{110.0, 115.0, 120.0, 125.0,
                                          kBoundStrike, 135.0, 140.0, 145.0};
constexpr std::uint16_t kFirstBidless = 4u;

Chain make_starved_bidless_chain() {
  Chain c = make_priced_chain(kStarvedStrikes);
  for (std::uint16_t s = kFirstBidless; s < kStarvedStrikes.size(); ++s) {
    const std::size_t idx = chain_index(s, Side::Call);
    c.bids[idx] = 0.0;
    c.mids[idx] = 0.5 * c.asks[idx];
  }
  return c;
}

CalibOpts bounds_off_opts() {
  CalibOpts o = calib_default_opts();
  o.one_sided_bounds = false;
  return o;
}

TEST(BuildObservations, BidlessOtmLegs_WithoutBounds_StarveTheSlice) {
  const Chain c = make_starved_bidless_chain();
  CalibOpts opts = bounds_off_opts();
  opts.max_spread_vol = kLooseSpreadVol;

  // Only the four two-sided 110-125 calls survive, one short of the floor, so
  // the whole expiry is lost — the exact shape T6 exists to recover.
  const auto res = build_observations(c, kF, kT, kDf, opts);
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), ErrorCode::NotFound);
  EXPECT_NE(res.error().to_string().find("kept=4 of 8"), std::string::npos)
      << res.error().to_string();
  EXPECT_NE(res.error().to_string().find("InvalidBidAsk x4"), std::string::npos)
      << res.error().to_string();
}

TEST(BuildObservations, BoundsAreRefusedWhileTheTwoSidedPopulationClearsTheFloor) {
  // The gate that keeps this change out of every board that already works: one
  // bid-less strike on an otherwise-quotable slice recovers nothing, because the
  // five two-sided rows already prepare it. Same chain, same flag, no bound.
  Chain c = make_priced_chain(kBoundStrikes);
  unbid_leg(c, kFirstBidless, Side::Call);
  CalibOpts opts = calib_default_opts();
  opts.max_spread_vol = kLooseSpreadVol;

  const auto off = build_observations(c, kF, kT, kDf, bounds_off_opts());
  const auto on = build_observations(c, kF, kT, kDf, opts);
  ASSERT_TRUE(off.has_value()) << off.error().to_string();
  ASSERT_TRUE(on.has_value()) << on.error().to_string();
  EXPECT_EQ(on->obs.size(), 5u);
  EXPECT_EQ(on->n_bound_admitted, 0u);
  ASSERT_EQ(off->obs.size(), on->obs.size());
  for (std::size_t i = 0; i < off->obs.size(); ++i) {
    EXPECT_DOUBLE_EQ(off->obs[i].K, on->obs[i].K);
    EXPECT_DOUBLE_EQ(off->obs[i].mid, on->obs[i].mid);
    EXPECT_DOUBLE_EQ(off->obs[i].weight_w, on->obs[i].weight_w);
  }
}

TEST(BuildObservations, BidlessOtmLeg_AdmittedAsAnUpperBoundOnTheStrikePrice) {
  Chain c = make_starved_bidless_chain();
  const std::size_t idx = chain_index(kFirstBidless, Side::Call);
  const double ask = c.asks[idx];
  CalibOpts opts = calib_default_opts();
  opts.max_spread_vol = kLooseSpreadVol;
  // Observe the weight the row EARNS. `max_weight` (1e3) is a saturating clip
  // that this synthetic board's every row exceeds; it is order-preserving, but
  // it compresses the degradation this assertion is about. See the weight test
  // below, which pins that the clip cannot promote a bound row.
  opts.max_weight = 1.0e12;

  const auto res = build_observations(c, kF, kT, kDf, opts);
  ASSERT_TRUE(res.has_value()) << res.error().to_string();
  ASSERT_EQ(res->obs.size(), kStarvedStrikes.size());
  EXPECT_EQ(res->n_bound_admitted, 4u);

  // Rows stay in ascending source-strike order, so the first bound row is the
  // first bid-less strike.
  const FitObs &bound = res->obs[kFirstBidless];
  ASSERT_NEAR(bound.K, kBoundStrike, 1e-12);
  EXPECT_EQ(bound.side, Side::Call);
  // K > F, so the call's Black-76 lower bound is 0: the admitted band is the
  // whole of [0, ask], its reference the midpoint, its width the entire ask.
  EXPECT_DOUBLE_EQ(bound.mid, 0.5 * ask);
  EXPECT_DOUBLE_EQ(bound.spread, ask);
  // The band's width is what the row is weighted at — 1/spread degradation, not
  // the half-spread a two-sided quote of the same ask would have earned.
  const double expected_weight = (bound.vega * bound.vega) / (ask * ask + 1.0e-18) /
                                 (4.0 * bound.sigma_mkt * bound.sigma_mkt * kT * kT + 1.0e-18);
  EXPECT_NEAR(bound.weight_w, expected_weight, 1e-9 * std::fabs(expected_weight));
  EXPECT_DOUBLE_EQ(bound.active_weight_w, bound.weight_w);
  // Half the true premium inverts to a materially lower vol than the board's
  // 20%: the row is admitted as a BOUND, never as a mark.
  EXPECT_GT(bound.sigma_mkt, 0.0);
  EXPECT_LT(bound.sigma_mkt, kVol);
}

TEST(BuildObservations, BoundRowIsWeightedFarBelowTheTwoSidedRowItReplaces) {
  // The whole claim of admitting a bound is that it enters the objective at the
  // weight its WIDTH earns, so a starved slice gains evidence without a marked
  // strike being outvoted by a guess. Same strike, same vega, two markets.
  const Chain two_sided = make_priced_chain(kStarvedStrikes);
  const Chain bidless = make_starved_bidless_chain();
  CalibOpts opts = calib_default_opts();
  opts.max_spread_vol = kLooseSpreadVol;
  // The far two-sided rows are penny-wide against a sub-dime mid, so the
  // RELATIVE gate (which bound rows skip by design) would otherwise remove them
  // and leave the two populations incomparable.
  opts.max_spread_to_mid_pct = 3.0;
  opts.max_weight = 1.0e12; // see above: the shipped clip saturates this board

  const auto marked = build_observations(two_sided, kF, kT, kDf, opts);
  const auto bounded = build_observations(bidless, kF, kT, kDf, opts);
  ASSERT_TRUE(marked.has_value()) << marked.error().to_string();
  ASSERT_TRUE(bounded.has_value()) << bounded.error().to_string();
  ASSERT_EQ(marked->obs.size(), kStarvedStrikes.size());
  ASSERT_EQ(bounded->obs.size(), kStarvedStrikes.size());
  EXPECT_LT(bounded->obs[kFirstBidless].weight_w, 0.02 * marked->obs[kFirstBidless].weight_w);

  // And the shipped clip cannot invert that: `min(w, max_weight)` is monotone,
  // so a bound row can be compressed onto the ceiling but never lifted past a
  // row that earned more. Pin it, because a clip applied where weights are
  // RE-DERIVED (the de-Am builder) would collapse every row onto the ceiling
  // and erase exactly this degradation.
  CalibOpts clipped = opts;
  clipped.max_weight = 1.0e3;
  const auto marked_clipped = build_observations(two_sided, kF, kT, kDf, clipped);
  const auto bounded_clipped = build_observations(bidless, kF, kT, kDf, clipped);
  ASSERT_TRUE(marked_clipped.has_value());
  ASSERT_TRUE(bounded_clipped.has_value());
  EXPECT_LE(bounded_clipped->obs[kFirstBidless].weight_w,
            marked_clipped->obs[kFirstBidless].weight_w);
}

TEST(BuildObservations, BoundAdmissionIsInertOnAFullyTwoSidedBoard) {
  // Every leg is quotable, so no bid is ever projected and the population must
  // be bit-identical to the historical two-sided-only rule.
  const std::vector<double> strikes{85.0, 90.0, 95.0, 100.0, 105.0, 110.0};
  const Chain c = make_priced_chain(strikes);

  const auto off = build_observations(c, kF, kT, kDf, bounds_off_opts());
  const auto on = build_observations(c, kF, kT, kDf, calib_default_opts());
  ASSERT_TRUE(off.has_value());
  ASSERT_TRUE(on.has_value());
  ASSERT_EQ(off->obs.size(), on->obs.size());
  EXPECT_EQ(on->n_bound_admitted, 0u);
  EXPECT_EQ(off->n_dropped, on->n_dropped);
  for (std::size_t i = 0; i < off->obs.size(); ++i) {
    EXPECT_EQ(off->obs[i].side, on->obs[i].side);
    EXPECT_DOUBLE_EQ(off->obs[i].K, on->obs[i].K);
    EXPECT_DOUBLE_EQ(off->obs[i].mid, on->obs[i].mid);
    EXPECT_DOUBLE_EQ(off->obs[i].spread, on->obs[i].spread);
    EXPECT_DOUBLE_EQ(off->obs[i].sigma_mkt, on->obs[i].sigma_mkt);
    EXPECT_DOUBLE_EQ(off->obs[i].weight_w, on->obs[i].weight_w);
  }
}

TEST(BuildObservations, AbsentLegBoundsNothingAndIsStillRejected) {
  // bid = ask = 0 is how an unquoted side arrives. It carries no upper bound,
  // so there is nothing to admit and the strike must stay dead.
  Chain c = make_starved_bidless_chain();
  unquote_leg(c, 5u, Side::Call); // K = 135: no ask either
  CalibOpts opts = calib_default_opts();
  opts.max_spread_vol = kLooseSpreadVol;

  const auto res = build_observations(c, kF, kT, kDf, opts);
  ASSERT_TRUE(res.has_value()) << res.error().to_string();
  EXPECT_EQ(res->obs.size(), kStarvedStrikes.size() - 1u);
  EXPECT_EQ(res->n_bound_admitted, 3u);
  for (const FitObs &o : res->obs) {
    EXPECT_GT(std::fabs(o.K - 135.0), 1e-9);
  }
}

TEST(BuildObservations, LockedAndCrossedMarketsAreNeverAdmittedAsBounds) {
  // Two sides that contradict each other do not bracket a price. Both shapes
  // keep a positive bid, which is exactly what the projection refuses to touch.
  const std::size_t idx = chain_index(5u, Side::Call); // K = 135
  Chain locked = make_starved_bidless_chain();
  locked.bids[idx] = locked.asks[idx];
  locked.mids[idx] = locked.asks[idx];
  Chain crossed = make_starved_bidless_chain();
  crossed.bids[idx] = crossed.asks[idx] + 0.01;
  crossed.mids[idx] = 0.5 * (crossed.bids[idx] + crossed.asks[idx]);
  CalibOpts opts = calib_default_opts();
  opts.max_spread_vol = kLooseSpreadVol;

  for (const Chain *c : {&locked, &crossed}) {
    const auto res = build_observations(*c, kF, kT, kDf, opts);
    ASSERT_TRUE(res.has_value()) << res.error().to_string();
    EXPECT_EQ(res->obs.size(), kStarvedStrikes.size() - 1u);
    EXPECT_EQ(res->n_bound_admitted, 3u);
    for (const FitObs &o : res->obs) {
      EXPECT_GT(std::fabs(o.K - 135.0), 1e-9);
    }
  }
}

// The floor only bites in the money, which on this builder means the leg the
// ITM-leg fallback reaches for: the preferred leg is OTM by construction and
// its arbitrage bound is identically zero. K = 88 sits one strike below the
// forward, so its call carries a ~11.8 intrinsic that a 0.00 bid contradicts.
// FIVE strikes, not six: with the 88 put unquoted the remaining four two-sided
// puts leave the slice one row under the floor, which is what lets the bound
// pass run at all.
const std::vector<double> kItmFloorStrikes{88.0, 90.0, 92.0, 94.0, 96.0};

CalibOpts itm_floor_opts() {
  CalibOpts o = calib_default_opts();
  o.itm_leg_fallback = true;
  // A deep-ITM leg admitted over [intrinsic, ask] is wide in vol units by
  // construction; this test is about WHERE the floor lands, not about the
  // budget, so give it room and let the dedicated gate test own that question.
  o.max_spread_vol = 100.0;
  o.min_vega_weight = 0.0;
  return o;
}

TEST(BuildObservations, AskBelowTheArbitrageFloorIsRejectedNotAdmitted) {
  // An ask under the strike's own lower arbitrage bound leaves an EMPTY band.
  // Admitting it would fabricate a price no arbitrage-free model can produce.
  Chain c = make_priced_chain(kItmFloorStrikes);
  const std::size_t call_idx = chain_index(0u, Side::Call);
  c.bids[call_idx] = 0.0;
  c.asks[call_idx] = 0.5 * kDf * (kF - 88.0); // an ask beneath intrinsic
  c.mids[call_idx] = 0.5 * c.asks[call_idx];
  unquote_leg(c, 0u, Side::Put); // force the fallback to consider the call

  // Four two-sided puts remain and the incoherent call cannot make a fifth row,
  // so the slice stays starved — the sharpest statement of the refusal.
  const auto res = build_observations(c, kF, kT, kDf, itm_floor_opts());
  ASSERT_FALSE(res.has_value()) << "the incoherent call must never be admitted";
  EXPECT_EQ(res.error().code(), ErrorCode::NotFound);
}

TEST(BuildObservations, BidlessItmLegProjectsOntoTheTickRoundedArbitrageFloor) {
  Chain c = make_priced_chain(kItmFloorStrikes);
  const std::size_t call_idx = chain_index(0u, Side::Call);
  const double ask = c.asks[call_idx];
  c.bids[call_idx] = 0.0;
  c.mids[call_idx] = 0.5 * ask;
  unquote_leg(c, 0u, Side::Put); // no OTM put at all -> the fallback must run

  const auto res = build_observations(c, kF, kT, kDf, itm_floor_opts());
  ASSERT_TRUE(res.has_value()) << res.error().to_string();
  ASSERT_EQ(res->obs.size(), kItmFloorStrikes.size());
  const FitObs &bound = res->obs.front();
  ASSERT_NEAR(bound.K, 88.0, 1e-12);
  EXPECT_EQ(bound.side, Side::Call);
  EXPECT_EQ(res->n_bound_admitted, 1u);
  EXPECT_EQ(res->n_itm_fallback, 1u) << "the ITM leg is the one that was used";

  const double raw_floor = kDf * (kF - 88.0);
  const double expected_floor = std::ceil(raw_floor / 0.01) * 0.01;
  EXPECT_GT(expected_floor, 0.0);
  EXPECT_DOUBLE_EQ(bound.spread, ask - expected_floor);
  EXPECT_DOUBLE_EQ(bound.mid, 0.5 * (expected_floor + ask));
  EXPECT_GT(bound.mid, raw_floor) << "the reference price must clear the arbitrage floor";
}

TEST(BuildObservations, TwoSidedItmLegOutranksAOneSidedOtmBound) {
  // Both legs of a strike share a vega, so the narrower band is the sharper
  // observation. A real ITM market must displace the OTM bound, not lose to it.
  Chain c = make_priced_chain(kBoundStrikes);
  unbid_leg(c, 4u, Side::Call);
  CalibOpts opts = calib_default_opts();
  opts.itm_leg_fallback = true;
  opts.max_spread_vol = kLooseSpreadVol;

  const auto res = build_observations(c, kF, kT, kDf, opts);
  ASSERT_TRUE(res.has_value()) << res.error().to_string();
  ASSERT_EQ(res->obs.size(), 6u);
  EXPECT_EQ(res->n_itm_fallback, 1u);
  EXPECT_EQ(res->n_bound_admitted, 0u);
  const FitObs &row = res->obs[4];
  EXPECT_NEAR(row.K, kBoundStrike, 1e-12);
  EXPECT_EQ(row.side, Side::Put);
  EXPECT_NEAR(row.sigma_mkt, kVol, 1e-4);
}

TEST(BuildObservations, BidlessOtmLegStillArmsTheItmFallbackWhenTheBoundIsFiltered) {
  // Regression guard. The fallback asks "does this strike have an OTM market?",
  // which bound admission must not answer by renaming the rejection: the K=130
  // bound is thrown out by the default vol-width budget, and the two-sided ITM
  // put must still rescue the strike exactly as it did before T6.
  Chain c = make_priced_chain(kBoundStrikes);
  unbid_leg(c, 4u, Side::Call);
  CalibOpts opts = calib_default_opts(); // max_spread_vol 0.05 rejects the bound
  opts.itm_leg_fallback = true;

  const auto res = build_observations(c, kF, kT, kDf, opts);
  ASSERT_TRUE(res.has_value()) << res.error().to_string();
  ASSERT_EQ(res->obs.size(), 6u);
  EXPECT_EQ(res->n_itm_fallback, 1u);
  EXPECT_EQ(res->n_bound_admitted, 0u);
  EXPECT_EQ(res->obs[4].side, Side::Put);
}

TEST(BuildObservations, ATwoSidedItmLegOutranksABoundEvenInsideTheLastResortPass) {
  // The ranking has to survive into the pass where bounds are actually admitted,
  // and that pass is the only place it can be observed: while the preferred leg
  // is merely REJECTED the pre-T6 trigger already fires, but once the same leg
  // is ACCEPTED as a bound only `one_sided` still reports "this strike has no
  // OTM market". Read the rejection reason instead and the bound silently
  // outranks a real two-sided market on the strike's other leg.
  //
  // Shape: every OTM call is offered but not bid. The first four strikes carry a
  // two-sided ITM put, which is exactly four rows — one short of the floor — so
  // the bound pass runs, and those four strikes must KEEP their marked puts.
  Chain c = make_priced_chain(kStarvedStrikes);
  for (std::uint16_t i = 0; i < kStarvedStrikes.size(); ++i) {
    const std::size_t call = chain_index(i, Side::Call);
    c.bids[call] = 0.0;
    c.mids[call] = 0.5 * c.asks[call];
    if (i >= kFirstBidless) {
      unquote_leg(c, i, Side::Put); // no ITM market out here either
    }
  }
  CalibOpts opts = calib_default_opts();
  opts.itm_leg_fallback = true;
  opts.max_spread_vol = kLooseSpreadVol;

  const auto res = build_observations(c, kF, kT, kDf, opts);
  ASSERT_TRUE(res.has_value()) << res.error().to_string();
  ASSERT_EQ(res->obs.size(), kStarvedStrikes.size());
  EXPECT_EQ(res->n_itm_fallback, 4u);
  EXPECT_EQ(res->n_bound_admitted, 4u);
  for (std::size_t i = 0; i < res->obs.size(); ++i) {
    if (i < kFirstBidless) {
      EXPECT_EQ(res->obs[i].side, Side::Put) << "strike " << res->obs[i].K;
      EXPECT_NEAR(res->obs[i].sigma_mkt, kVol, 1e-4);
    } else {
      EXPECT_EQ(res->obs[i].side, Side::Call) << "strike " << res->obs[i].K;
    }
  }
}

TEST(BuildObservations, BoundRowStillFacesTheAbsoluteVolWidthGate) {
  // The relative spread-to-mid gate is skipped for bound rows because it is a
  // pure function of one-sidedness. The absolute vol-space gate is not, and it
  // is what keeps an uninformative bid-less quote out: the same chain that
  // admits the bounds at 0.30 vol points refuses every one at the SPY-tuned 0.05
  // and lets the slice stay starved.
  const Chain c = make_starved_bidless_chain();

  CalibOpts loose = calib_default_opts();
  loose.max_spread_vol = kLooseSpreadVol;
  const auto admitted = build_observations(c, kF, kT, kDf, loose);
  ASSERT_TRUE(admitted.has_value()) << admitted.error().to_string();
  EXPECT_EQ(admitted->n_bound_admitted, 4u);

  // At the SPY budget the widest bound (130, whose ask alone is ~8.8 vol points
  // against its vega) is refused outright while the cheaper ones survive: the
  // gate reads each row's own width, not the fact that it is one-sided.
  const auto res = build_observations(c, kF, kT, kDf, calib_default_opts());
  ASSERT_TRUE(res.has_value()) << res.error().to_string();
  EXPECT_EQ(res->n_bound_admitted, 3u);
  for (const FitObs &o : res->obs) {
    EXPECT_GT(std::fabs(o.K - kBoundStrike), 1e-9);
  }
}

TEST(BuildObservations, BoundAdmissionRescuesASliceTheTwoSidedRuleWouldStarve) {
  // The starvation shape: four marked calls, one short of the floor, and every
  // strike beyond them offered but not bid. Under the two-sided rule the whole
  // expiry is lost; the bounds complete it.
  const Chain c = make_starved_bidless_chain();
  CalibOpts opts = calib_default_opts();
  opts.max_spread_vol = kLooseSpreadVol;
  CalibOpts off = opts;
  off.one_sided_bounds = false;

  const auto starved = build_observations(c, kF, kT, kDf, off);
  ASSERT_FALSE(starved.has_value());
  EXPECT_EQ(starved.error().code(), ErrorCode::NotFound);

  const auto rescued = build_observations(c, kF, kT, kDf, opts);
  ASSERT_TRUE(rescued.has_value()) << rescued.error().to_string();
  EXPECT_EQ(rescued->obs.size(), kStarvedStrikes.size());
  EXPECT_EQ(rescued->n_bound_admitted, 4u);
  for (const FitObs &o : rescued->obs) {
    EXPECT_EQ(o.side, Side::Call);
    EXPECT_GT(o.spread, 0.0);
  }
}

TEST(BuildObservations, BoundsNeverBuildASliceThatNoMarketAnchors) {
  // The other half of the last-resort rule. Upper bounds alone fix only an
  // ENVELOPE; fitting their midpoints as marks would invent a level no market
  // supports, and a level invented per expiry is free to cross its neighbour in
  // total variance. A slice more than one row short of the floor on real
  // evidence therefore stays starved, however many bounds it could supply.
  Chain c = make_starved_bidless_chain();
  unquote_leg(c, 3u, Side::Call); // drop one marked call: three anchors left
  CalibOpts opts = calib_default_opts();
  opts.max_spread_vol = kLooseSpreadVol;

  const auto res = build_observations(c, kF, kT, kDf, opts);
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), ErrorCode::NotFound);
  EXPECT_NE(res.error().to_string().find("kept=3 of 8"), std::string::npos)
      << res.error().to_string();
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
  const Chain chain = make_american_chain({85.0, 90.0, 95.0, 100.0, 105.0, 110.0, 115.0}, T, r, q);
  CalibOpts opts = calib_default_opts();
  opts.max_spread_vol = 1.0;
  opts.max_otm_shortcut_premium_spread_frac = 100.0;
  opts.max_inversion_residual_half_spreads = 0.01;
  opts.min_otm_shortcut_T = 0.0;

  const auto result = build_observations_european(chain, 100.0, r, F, T, df, opts, {}, std::nullopt,
                                                  1.0e-7, 64, AmericanMethod::AndersenLake);
  ASSERT_TRUE(result.has_value()) << (result ? std::string{} : result.error().to_string());
  const auto &diag = result->deam_audit;
  EXPECT_GT(diag.shortcut.n_proposed, 0u);
  EXPECT_EQ(diag.shortcut.n_audited, diag.shortcut.n_proposed);
  EXPECT_GT(diag.n_accurate_fallback, 0u);
  EXPECT_EQ(diag.n_rejected_residual, 0u);
  EXPECT_LE(diag.accurate.max_residual_half_spreads, opts.max_inversion_residual_half_spreads);
  EXPECT_GE(result->obs.size(), 5u);
}

TEST(BuildObservationsEuropean, AccurateRouteSkipsRedundantReferenceReprice) {
  constexpr double T = 1.0;
  constexpr double r = 0.05;
  constexpr double q = 0.02;
  const double F = 100.0 * std::exp((r - q) * T);
  const double df = std::exp(-r * T);
  const Chain chain =
      make_american_chain({82.0, 88.0, 94.0, 100.0, 106.0, 112.0, 118.0}, T, r, q, 0.24);
  CalibOpts reference_opts = calib_default_opts();
  reference_opts.max_spread_vol = 1.0;
  reference_opts.audit_accurate_inversions = true;

  const auto reference =
      build_observations_european(chain, 100.0, r, F, T, df, reference_opts, {}, std::nullopt,
                                  1.0e-7, 64, AmericanMethod::AndersenLake, false);
  ASSERT_TRUE(reference.has_value()) << reference.error().to_string();

  CalibOpts optimized_opts = reference_opts;
  optimized_opts.audit_accurate_inversions = false;
  const auto optimized =
      build_observations_european(chain, 100.0, r, F, T, df, optimized_opts, {}, std::nullopt,
                                  1.0e-7, 64, AmericanMethod::AndersenLake, false);
  ASSERT_TRUE(optimized.has_value()) << optimized.error().to_string();

  ASSERT_EQ(optimized->obs.size(), reference->obs.size());
  ASSERT_EQ(optimized->provenance.size(), reference->provenance.size());
  for (std::size_t index = 0; index < optimized->obs.size(); ++index) {
    const FitObs &actual = optimized->obs[index];
    const FitObs &expected = reference->obs[index];
    EXPECT_EQ(actual.source_strike_index, expected.source_strike_index);
    EXPECT_EQ(actual.side, expected.side);
    EXPECT_DOUBLE_EQ(actual.K, expected.K);
    EXPECT_DOUBLE_EQ(actual.sigma_mkt, expected.sigma_mkt);
  }
  for (std::size_t index = 0; index < optimized->provenance.size(); ++index) {
    EXPECT_EQ(optimized->provenance[index].source_strike_index,
              reference->provenance[index].source_strike_index);
    EXPECT_EQ(optimized->provenance[index].side, reference->provenance[index].side);
    EXPECT_EQ(optimized->provenance[index].rejection, reference->provenance[index].rejection);
  }
  EXPECT_EQ(reference->deam_audit.accurate.n_reference_reprices,
            reference->deam_audit.accurate.n_proposed);
  EXPECT_EQ(optimized->deam_audit.accurate.n_reference_reprices, 0u);
  EXPECT_EQ(optimized->deam_audit.accurate.n_audited, optimized->deam_audit.accurate.n_accepted);
  EXPECT_TRUE(deam_inversion_certified(optimized->deam_audit, 0.10));
}

TEST(BuildObservationsEuropean, LooseAccurateControlsStillRequireReferenceReprice) {
  constexpr double T = 1.0;
  constexpr double r = 0.05;
  constexpr double q = 0.02;
  const double F = 100.0 * std::exp((r - q) * T);
  const double df = std::exp(-r * T);
  const Chain chain =
      make_american_chain({82.0, 88.0, 94.0, 100.0, 106.0, 112.0, 118.0}, T, r, q, 0.24);
  CalibOpts opts = calib_default_opts();
  opts.max_spread_vol = 1.0;
  opts.audit_accurate_inversions = false;

  const auto result = build_observations_european(chain, 100.0, r, F, T, df, opts, {}, std::nullopt,
                                                  1.0e-4, 64, AmericanMethod::AndersenLake, false);
  ASSERT_TRUE(result.has_value()) << result.error().to_string();
  EXPECT_GT(result->deam_audit.accurate.n_proposed, 0u);
  EXPECT_EQ(result->deam_audit.accurate.n_reference_reprices,
            result->deam_audit.accurate.n_proposed);
}

TEST(BuildObservationsEuropean, AdjacentWarmStartsReduceWorkInsideEconomicErrorBudget) {
  constexpr double T = 1.0;
  constexpr double r = 0.05;
  constexpr double q = 0.02;
  const double F = 100.0 * std::exp((r - q) * T);
  const double df = std::exp(-r * T);
  std::vector<double> strikes;
  strikes.reserve(256u);
  for (std::size_t index = 0; index < 256u; ++index) {
    strikes.push_back(80.0 + 40.0 * static_cast<double>(index) / 255.0);
  }
  const Chain chain = make_american_chain(strikes, T, r, q, 0.24);

  const auto run = [&](bool warm_start) {
    CalibOpts opts = calib_default_opts();
    opts.max_spread_vol = 1.0;
    opts.min_vega_weight = 0.0;
    opts.warm_start_deam_adjacent_strikes = warm_start;
    opts.use_shared_boundary_deam = false; // isolate the adjacent-seed A/B
    atx::vol::counters::lightweight::reset();
    const auto before = atx::vol::counters::lightweight::snapshot();
    auto observations =
        build_observations_european(chain, 100.0, r, F, T, df, opts, {}, std::nullopt, 1.0e-7, 64,
                                    AmericanMethod::AndersenLake, false);
    const auto after = atx::vol::counters::lightweight::snapshot();
    return std::pair{std::move(observations),
                     atx::vol::counters::lightweight::delta(before, after)};
  };

  auto [cold, cold_work] = run(false);
  auto [warm, warm_work] = run(true);
  ASSERT_TRUE(cold.has_value()) << cold.error().to_string();
  ASSERT_TRUE(warm.has_value()) << warm.error().to_string();
  ASSERT_EQ(warm->obs.size(), cold->obs.size());
  ASSERT_EQ(warm->provenance.size(), cold->provenance.size());
  for (std::size_t index = 0; index < warm->obs.size(); ++index) {
    const FitObs &actual = warm->obs[index];
    const FitObs &reference = cold->obs[index];
    EXPECT_EQ(actual.source_strike_index, reference.source_strike_index);
    EXPECT_EQ(actual.side, reference.side);
    const double iv_error = std::fabs(actual.sigma_mkt - reference.sigma_mkt);
    const double price_error = std::fabs(actual.mid - reference.mid);
    const double economic_price_budget = std::min(0.005, 0.1 * reference.vega * 1.0e-4);
    EXPECT_LE(iv_error, 1.0e-4);
    EXPECT_LE(price_error, economic_price_budget);
    EXPECT_LT(price_error, 0.5 * reference.spread);
  }
  for (std::size_t index = 0; index < warm->provenance.size(); ++index) {
    EXPECT_EQ(warm->provenance[index].source_strike_index,
              cold->provenance[index].source_strike_index);
    EXPECT_EQ(warm->provenance[index].side, cold->provenance[index].side);
    EXPECT_EQ(warm->provenance[index].rejection, cold->provenance[index].rejection);
  }
  ASSERT_EQ(warm_work.american_iv_samples, cold_work.american_iv_samples);
  ASSERT_GT(warm_work.american_iv_samples, 1u);
  EXPECT_LT(warm_work.residual_evaluations_in_sampled_iv,
            cold_work.residual_evaluations_in_sampled_iv);
}

TEST(BuildObservationsEuropean, SharedSigmaBoundaryPreservesEconomicsWithConstantNodeWork) {
  constexpr double T = 1.0;
  constexpr double r = 0.05;
  constexpr double q = 0.02;
  const double F = 100.0 * std::exp((r - q) * T);
  const double df = std::exp(-r * T);
  std::vector<double> strikes;
  strikes.reserve(96u);
  for (std::size_t index = 0; index < 96u; ++index) {
    strikes.push_back(72.0 + 56.0 * static_cast<double>(index) / 95.0);
  }
  const Chain chain = make_american_chain(strikes, T, r, q, 0.24);

  CalibOpts reference_opts = calib_default_opts();
  reference_opts.max_spread_vol = 1.0;
  reference_opts.min_vega_weight = 0.0;
  reference_opts.use_shared_boundary_deam = false;
  atx::vol::counters::reset();
  const auto reference =
      build_observations_european(chain, 100.0, r, F, T, df, reference_opts, {}, std::nullopt,
                                  1.0e-7, 64, AmericanMethod::AndersenLake, false);
  const auto reference_work = atx::vol::counters::snapshot();
  ASSERT_TRUE(reference.has_value()) << reference.error().to_string();

  CalibOpts shared_opts = reference_opts;
  shared_opts.use_shared_boundary_deam = true;
  atx::vol::counters::reset();
  const auto shared =
      build_observations_european(chain, 100.0, r, F, T, df, shared_opts, {}, std::nullopt, 1.0e-7,
                                  64, AmericanMethod::AndersenLake, false);
  const auto shared_work = atx::vol::counters::snapshot();
  ASSERT_TRUE(shared.has_value()) << shared.error().to_string();

  ASSERT_EQ(shared->obs.size(), reference->obs.size());
  ASSERT_EQ(shared->provenance.size(), reference->provenance.size());
  for (std::size_t index = 0; index < shared->obs.size(); ++index) {
    const FitObs &actual = shared->obs[index];
    const FitObs &expected = reference->obs[index];
    EXPECT_EQ(actual.source_strike_index, expected.source_strike_index);
    EXPECT_EQ(actual.side, expected.side);
    const double iv_error = std::fabs(actual.sigma_mkt - expected.sigma_mkt);
    const double price_error = std::fabs(actual.mid - expected.mid);
    const double economic_price_budget = std::min(0.005, 0.1 * expected.vega * 1.0e-4);
    EXPECT_LE(iv_error, 1.0e-4);
    EXPECT_LE(price_error, economic_price_budget);
    EXPECT_LT(price_error, 0.5 * expected.spread);
  }
  for (std::size_t index = 0; index < shared->provenance.size(); ++index) {
    EXPECT_EQ(shared->provenance[index].source_strike_index,
              reference->provenance[index].source_strike_index);
    EXPECT_EQ(shared->provenance[index].side, reference->provenance[index].side);
    EXPECT_EQ(shared->provenance[index].rejection, reference->provenance[index].rejection);
  }

  const DeAmAuditDiagnostics &audit = shared->deam_audit;
  EXPECT_GT(audit.n_shared_boundary_lanes, 0u);
  EXPECT_GT(audit.n_shared_call_lanes, 0u);
  EXPECT_GT(audit.n_shared_put_lanes, 0u);
  EXPECT_EQ(audit.n_shared_boundary_solves, 18u);
  EXPECT_GT(audit.n_shared_sentinel_reprices, 0u);
  EXPECT_LE(audit.n_shared_sentinel_reprices, 6u);
  EXPECT_LT(audit.n_shared_scalar_fallback_lanes, audit.n_deam_rows);
  EXPECT_GE(audit.accurate.n_proposed, audit.n_shared_boundary_lanes);
  EXPECT_EQ(audit.accurate.n_accepted, audit.accurate.n_proposed);
  EXPECT_EQ(audit.accurate.n_audited, audit.accurate.n_accepted);
  EXPECT_TRUE(deam_inversion_certified(audit, 0.10));
  if constexpr (atx::vol::counters::counters_enabled()) {
    EXPECT_LT(shared_work.get(atx::vol::counters::Counter::BoundarySolves),
              reference_work.get(atx::vol::counters::Counter::BoundarySolves));
  }
  atx::vol::counters::reset();
}

TEST(BuildObservationsEuropean, SharedSigmaBoundaryHoldsEconomicBoundOnSteepSmile) {
  // The smile-stress pin. Two things live here that nothing else in the suite
  // covers:
  //
  // 1. R-08's retirement evidence. Task 2 investigated a suspected
  //    sigma-monotonicity / multi-root risk in the interpolated price map and
  //    retired it as not-live, on a measurement taken with a throwaway probe:
  //    on a sigma in [0.15, 0.8] smile board the worst per-lane IV error against
  //    the EXACT scalar inverter was ~5e-08, ~2000x inside the 1e-4 bound, and the
  //    genuine interpolation wiggles (~ -3.1e-04 near K=110) sit only in low-vega
  //    wings that the existing budget and bracket-sign gates already exclude.
  //    Evidence that lives in a deleted probe is not evidence, so it is pinned
  //    here.
  //
  // 2. R-11a's guard. The Illinois step rewrote HOW every lane finds its root, so
  //    this fixture — the widest sigma-box the route admits, ~15x — is also the
  //    regression test for that change. If the bound below ever fails, that is a
  //    real finding about the root-finder or the interpolant; do not loosen it.
  constexpr double T = 1.0;
  constexpr double r = 0.05;
  constexpr double q = 0.02;
  const double F = 100.0 * std::exp((r - q) * T);
  const double df = std::exp(-r * T);
  std::vector<double> strikes;
  std::vector<double> sigmas;
  strikes.reserve(96u);
  sigmas.reserve(96u);
  for (std::size_t index = 0; index < 96u; ++index) {
    const double t = static_cast<double>(index) / 95.0;
    strikes.push_back(72.0 + 56.0 * t);
    sigmas.push_back(0.80 + (0.15 - 0.80) * t); // steep put skew, box ~[0.0525, 0.8]
  }
  const Chain chain = make_american_smile_chain(strikes, sigmas, T, r, q);

  CalibOpts reference_opts = calib_default_opts();
  reference_opts.max_spread_vol = 1.0;
  reference_opts.min_vega_weight = 0.0;
  reference_opts.use_shared_boundary_deam = false; // the exact scalar inverter
  const auto reference =
      build_observations_european(chain, 100.0, r, F, T, df, reference_opts, {}, std::nullopt,
                                  1.0e-7, 64, AmericanMethod::AndersenLake, false);
  ASSERT_TRUE(reference.has_value()) << reference.error().to_string();

  CalibOpts shared_opts = reference_opts;
  shared_opts.use_shared_boundary_deam = true;
  const auto shared =
      build_observations_european(chain, 100.0, r, F, T, df, shared_opts, {}, std::nullopt, 1.0e-7,
                                  64, AmericanMethod::AndersenLake, false);
  ASSERT_TRUE(shared.has_value()) << shared.error().to_string();

  const DeAmAuditDiagnostics &audit = shared->deam_audit;
  ASSERT_GT(audit.n_shared_boundary_lanes, 0u) << "fixture must actually exercise shared lanes";
  ASSERT_EQ(shared->obs.size(), reference->obs.size());

  // (a) Every row holds the economic bound against the exact scalar reference.
  double worst_iv = 0.0;
  double worst_px = 0.0;
  for (std::size_t index = 0; index < shared->obs.size(); ++index) {
    const FitObs &actual = shared->obs[index];
    const FitObs &expected = reference->obs[index];
    ASSERT_EQ(actual.source_strike_index, expected.source_strike_index);
    ASSERT_EQ(actual.side, expected.side);
    const double iv_error = std::fabs(actual.sigma_mkt - expected.sigma_mkt);
    const double price_error = std::fabs(actual.mid - expected.mid);
    worst_iv = std::max(worst_iv, iv_error);
    worst_px = std::max(worst_px, price_error);
    EXPECT_LE(iv_error, 1.0e-4) << "K=" << chain.strikes[actual.source_strike_index];
    EXPECT_LE(price_error, std::min(0.005, 0.1 * expected.vega * 1.0e-4));
    EXPECT_LT(price_error, 0.5 * expected.spread); // strictly inside the half-spread
  }
  // The route's accuracy claim is not merely "inside 1e-4" — it is inside it by a
  // wide margin, and the margin is the actual evidence R-08 was retired on.
  // Measured here: 4.4e-08 (Task 2's throwaway probe measured 5.0e-08 on the same
  // board shape before the Illinois step; the step did not move it). The bound is
  // pinned ~200x above the measurement so a silent degradation is visible while
  // normal solver jitter is not, and ~10x below the sprint's own 1e-4.
  EXPECT_LE(worst_iv, 1.0e-5) << "worst per-lane IV vs the exact scalar inverter";
  EXPECT_LE(worst_px, 1.0e-4) << "worst per-lane price vs the exact scalar inverter";

  // (b) The route does not blanket-accept: this board exercises the per-lane
  // rejection path, and every row is accounted for as exactly one of {accepted
  // lane, scalar fallback}. A row the gates reject is served by the exact scalar
  // inverter, which is why a lane that cannot prove its bound never reaches a
  // price.
  EXPECT_GT(audit.n_shared_scalar_fallback_lanes, 0u)
      << "fixture must exercise the per-lane rejection path, not only acceptance";
  EXPECT_EQ(audit.n_shared_boundary_lanes + audit.n_shared_scalar_fallback_lanes,
            audit.n_deam_rows);

  // NB on scope. The brief for this test also asked to assert that "lanes in the
  // low-vega wing regions fall back to scalar rather than being accepted". That
  // is NOT assertable here, and the reason is worth recording rather than
  // papering over with a threshold that no row can trip:
  //   * The wiggles Task 2 found (~ -3.1e-04 near K=110) live in low-vega regions
  //     of the SIGMA axis — deep-ITM saturation at intrinsic, inside a lane's own
  //     bracket — not in low-vega STRIKES. There is no "wing row" to observe fall
  //     back.
  //   * This strike range (72-128) at T=1 cannot produce a vega-collapsed row to
  //     begin with: vega over admitted rows spans ~15.3 to ~37.6, well clear of
  //     collapse. That is a geometry fact of the fixture, not a gate observed to
  //     catch one — this fixture sets max_spread_vol = 1.0 and min_vega_weight =
  //     0.0 above, deliberately DISABLING the two gates that would drop a
  //     vega-collapsed row upstream under production defaults, to isolate the
  //     shared route. So the upstream cascade cannot be credited here.
  // So the wing defense is real but sits one layer earlier than the brief placed
  // it, and asserting it here would have been a permanently-true test. What
  // actually keeps the wiggles away from an accepted price is (a) above plus the
  // bracket-sign gate in initialize_shared_lane and the budget > 0 gate in
  // finalize_shared_lane, all of which this fixture does exercise.
}

TEST(BuildObservationsEuropean, SharedSigmaBoundaryRunsUnderHftShortcutPreset) {
  // SPY resolves to IndexEtfUltraLiquid -> FitPreset::Hft, which sets
  // max_otm_shortcut_premium_spread_frac = 0.50 (src/session.cpp). The OTM
  // shortcut and the shared boundary are per-ROW alternatives, not board-level
  // exclusives, so the shared route must still serve the non-shortcut rows.
  //
  // Both arms hold the shortcut fixed at the live Hft value, which isolates the
  // shared-boundary change: the shortcut mask is derived from pre-de-Am row
  // fields only, so it is bit-identical in both arms and every shortcut-claimed
  // row is bit-identical too. The remaining rows are the ones under test.
  constexpr double T = 1.0;
  constexpr double r = 0.05;
  constexpr double q = 0.02;
  const double F = 100.0 * std::exp((r - q) * T);
  const double df = std::exp(-r * T);
  std::vector<double> strikes;
  strikes.reserve(96u);
  for (std::size_t index = 0; index < 96u; ++index) {
    strikes.push_back(72.0 + 56.0 * static_cast<double>(index) / 95.0);
  }
  const Chain chain = make_american_chain(strikes, T, r, q, 0.24);

  CalibOpts reference_opts = calib_default_opts();
  reference_opts.max_spread_vol = 1.0;
  reference_opts.min_vega_weight = 0.0;
  reference_opts.max_otm_shortcut_premium_spread_frac = 0.50; // the live Hft preset value
  reference_opts.use_shared_boundary_deam = false;
  const auto reference =
      build_observations_european(chain, 100.0, r, F, T, df, reference_opts, {}, std::nullopt,
                                  1.0e-7, 64, AmericanMethod::AndersenLake, false);
  ASSERT_TRUE(reference.has_value()) << reference.error().to_string();
  ASSERT_EQ(reference->deam_audit.n_shared_boundary_solves, 0u);
  // The fixture must exercise BOTH routes, or "they coexist" proves nothing. On
  // this board the shortcut claims the whole call side (a q = 2% American call
  // carries almost no early-exercise premium), leaving the puts to the shared
  // boundary — the exact partition the wiring is for.
  ASSERT_GE(reference->deam_audit.shortcut.n_proposed, 10u)
      << "fixture must exercise the OTM shortcut on a substantive row population";

  CalibOpts shared_opts = reference_opts;
  shared_opts.use_shared_boundary_deam = true;
  const auto shared =
      build_observations_european(chain, 100.0, r, F, T, df, shared_opts, {}, std::nullopt, 1.0e-7,
                                  64, AmericanMethod::AndersenLake, false);
  // build_observations_european fails Internal if a shortcut-claimed row ever
  // carries a shared proposal, so a successful return IS the disjointness proof.
  ASSERT_TRUE(shared.has_value()) << shared.error().to_string();

  const DeAmAuditDiagnostics &audit = shared->deam_audit;
  // The headline: the shared route activates under the live Hft shortcut config.
  EXPECT_GT(audit.n_shared_boundary_solves, 0u);
  EXPECT_GT(audit.n_shared_boundary_lanes, 0u);
  // The shortcut mask is single-sourced: turning the shared route on must not
  // move one row into or out of the shortcut population.
  EXPECT_EQ(audit.shortcut.n_proposed, reference->deam_audit.shortcut.n_proposed);
  // Shortcut rows and shared lanes are disjoint subsets of the same row set, so
  // their populations can never overshoot it. On this fixture the two routes
  // partition the board exactly (every row is claimed by one of them), which is
  // the strongest form of the disjointness claim: no row is double-counted and
  // none is stranded.
  EXPECT_EQ(audit.n_shared_boundary_lanes + audit.shortcut.n_proposed, audit.n_deam_rows);
  EXPECT_LE(audit.n_shared_boundary_lanes, audit.accurate.n_proposed);
  EXPECT_EQ(audit.n_shared_scalar_fallback_lanes, 0u); // every non-shortcut row got a lane
  EXPECT_TRUE(deam_inversion_certified(audit, 0.10));

  ASSERT_EQ(shared->obs.size(), reference->obs.size());
  ASSERT_EQ(shared->provenance.size(), reference->provenance.size());
  for (std::size_t index = 0; index < shared->obs.size(); ++index) {
    const FitObs &actual = shared->obs[index];
    const FitObs &expected = reference->obs[index];
    EXPECT_EQ(actual.source_strike_index, expected.source_strike_index);
    EXPECT_EQ(actual.side, expected.side);
    const double iv_error = std::fabs(actual.sigma_mkt - expected.sigma_mkt);
    const double price_error = std::fabs(actual.mid - expected.mid);
    const double economic_price_budget = std::min(0.005, 0.1 * expected.vega * 1.0e-4);
    EXPECT_LE(iv_error, 1.0e-4);
    EXPECT_LE(price_error, economic_price_budget);
    EXPECT_LT(price_error, 0.5 * expected.spread);
  }
  for (std::size_t index = 0; index < shared->provenance.size(); ++index) {
    EXPECT_EQ(shared->provenance[index].source_strike_index,
              reference->provenance[index].source_strike_index);
    EXPECT_EQ(shared->provenance[index].side, reference->provenance[index].side);
    EXPECT_EQ(shared->provenance[index].rejection, reference->provenance[index].rejection);
  }
}

TEST(BuildObservationsEuropean, SharedSigmaBoundaryServesPutSideOnNegativeBorrow) {
  // R-09. A hard-to-borrow single name quotes r > 0 against a slightly NEGATIVE
  // effective yield. The shared interpolant's internal regime for the PUT side is
  // (rate = r, yield = q_eff): with r > 0 that is a regular single-boundary
  // American-put regime, so the put side must share boundaries. The CALL side's
  // internal rate IS q_eff, so it is genuinely unsupported and stays scalar.
  constexpr double T = 1.0;
  constexpr double r = 0.05;
  constexpr double q = -0.02; // negative borrow => q_eff == q == -0.02
  const double F = 100.0 * std::exp((r - q) * T);
  const double df = std::exp(-r * T);
  std::vector<double> strikes;
  strikes.reserve(96u);
  for (std::size_t index = 0; index < 96u; ++index) {
    strikes.push_back(72.0 + 56.0 * static_cast<double>(index) / 95.0);
  }
  const Chain chain = make_american_chain(strikes, T, r, q, 0.24);

  CalibOpts reference_opts = calib_default_opts();
  reference_opts.max_spread_vol = 1.0;
  reference_opts.min_vega_weight = 0.0;
  reference_opts.use_shared_boundary_deam = false;
  const auto reference =
      build_observations_european(chain, 100.0, r, F, T, df, reference_opts, {}, std::nullopt,
                                  1.0e-7, 64, AmericanMethod::AndersenLake, false);
  ASSERT_TRUE(reference.has_value()) << reference.error().to_string();

  CalibOpts shared_opts = reference_opts;
  shared_opts.use_shared_boundary_deam = true;
  const auto shared =
      build_observations_european(chain, 100.0, r, F, T, df, shared_opts, {}, std::nullopt, 1.0e-7,
                                  64, AmericanMethod::AndersenLake, false);
  ASSERT_TRUE(shared.has_value()) << shared.error().to_string();

  const DeAmAuditDiagnostics &audit = shared->deam_audit;
  EXPECT_GT(audit.n_shared_put_lanes, 0u);
  EXPECT_EQ(audit.n_shared_call_lanes, 0u); // internal call rate == q_eff < 0
  EXPECT_EQ(audit.n_shared_boundary_solves, 9u); // one nine-node build, put side only
  EXPECT_TRUE(deam_inversion_certified(audit, 0.10));

  ASSERT_EQ(shared->obs.size(), reference->obs.size());
  for (std::size_t index = 0; index < shared->obs.size(); ++index) {
    const FitObs &actual = shared->obs[index];
    const FitObs &expected = reference->obs[index];
    EXPECT_EQ(actual.source_strike_index, expected.source_strike_index);
    EXPECT_EQ(actual.side, expected.side);
    const double iv_error = std::fabs(actual.sigma_mkt - expected.sigma_mkt);
    const double price_error = std::fabs(actual.mid - expected.mid);
    const double economic_price_budget = std::min(0.005, 0.1 * expected.vega * 1.0e-4);
    EXPECT_LE(iv_error, 1.0e-4);
    EXPECT_LE(price_error, economic_price_budget);
    EXPECT_LT(price_error, 0.5 * expected.spread);
  }
  for (std::size_t index = 0; index < shared->provenance.size(); ++index) {
    EXPECT_EQ(shared->provenance[index].rejection, reference->provenance[index].rejection);
  }
}

TEST(BuildObservationsEuropean, SharedSigmaBoundaryKeepsShortTenorOnScalarPath) {
  constexpr double T = 2.0 / 365.25;
  constexpr double r = 0.05;
  constexpr double q = 0.02;
  const double F = 100.0 * std::exp((r - q) * T);
  const double df = std::exp(-r * T);
  std::vector<double> strikes;
  strikes.reserve(48u);
  for (std::size_t index = 0; index < 48u; ++index) {
    strikes.push_back(94.0 + 12.0 * static_cast<double>(index) / 47.0);
  }
  const Chain chain = make_american_chain(strikes, T, r, q, 0.30);
  CalibOpts opts = calib_default_opts();
  opts.max_spread_vol = 5.0;
  opts.min_vega_weight = 0.0;
  opts.use_shared_boundary_deam = true;

  const auto result = build_observations_european(chain, 100.0, r, F, T, df, opts, {}, std::nullopt,
                                                  1.0e-7, 64, AmericanMethod::AndersenLake, false);
  ASSERT_TRUE(result.has_value()) << result.error().to_string();
  EXPECT_EQ(result->deam_audit.n_shared_boundary_lanes, 0u);
  EXPECT_EQ(result->deam_audit.n_shared_boundary_solves, 0u);
  EXPECT_EQ(result->deam_audit.accurate.n_accepted, result->obs.size());
}

TEST(BuildObservationsEuropean, SharedSigmaBoundaryKeepsNegativeRatesOnScalarPath) {
  // Pins the RETAINED function-level guard: r < 0. R-09 removed the companion
  // q_eff < 0 bail (a put side under r > 0 is a regular American-put regime), but
  // r < 0 flips the PUT side's internal rate negative, and al_xmax_put(K, r<0,
  // q>=0) == 0 means there is no asymptotic boundary to interpolate at all. This
  // board must stay wholly on the scalar inverter.
  //
  // q_eff == q == +0.02 here, so r < 0 is unambiguously the only guard that can
  // fire — the assertion cannot be satisfied by the removed q_eff < 0 bail.
  constexpr double T = 1.0;
  constexpr double r = -0.01;
  constexpr double q = 0.02;
  static_assert(r < 0.0 && q > 0.0, "fixture must isolate the retained r < 0 guard");
  const double F = 100.0 * std::exp((r - q) * T);
  const double df = std::exp(-r * T);
  std::vector<double> strikes;
  strikes.reserve(48u);
  for (std::size_t index = 0; index < 48u; ++index) {
    strikes.push_back(78.0 + 44.0 * static_cast<double>(index) / 47.0);
  }
  const Chain chain = make_american_chain(strikes, T, r, q, 0.24);
  CalibOpts opts = calib_default_opts();
  opts.max_spread_vol = 5.0;
  opts.min_vega_weight = 0.0;
  opts.use_shared_boundary_deam = true;

  const auto result = build_observations_european(chain, 100.0, r, F, T, df, opts, {}, std::nullopt,
                                                  1.0e-7, 64, AmericanMethod::AndersenLake, false);
  ASSERT_TRUE(result.has_value()) << result.error().to_string();
  EXPECT_EQ(result->deam_audit.n_shared_boundary_lanes, 0u);
  EXPECT_EQ(result->deam_audit.n_shared_boundary_solves, 0u);
  EXPECT_EQ(result->deam_audit.n_shared_call_lanes, 0u);
  EXPECT_EQ(result->deam_audit.n_shared_put_lanes, 0u);
}

TEST(SharedLaneAcceptance, BoundsCombinedResidual) {
  // R-07. A shared lane's sigma is the root of the NINE-node interpolated price
  // map, so the true price error is bounded by the root-find residual PLUS the
  // nine-node map's own interpolation error (estimated by the 9-vs-5 gap):
  //     |price_true - mid| <= |price_true - price| + |price - mid|
  //                        ~= |price - embedded|   + |price - mid|.
  // Gating the two terms against `budget` INDEPENDENTLY only proves the sum is
  // within 2 x budget. The sprint bound is a single budget, so the SUM is the
  // quantity that must clear it.
  using atx::vol::detail::shared_lane_residual_within_budget;
  constexpr double kBudget = 1.0e-4;
  constexpr double kMid = 5.0;

  // The gap the independent gate misses: each term alone is comfortably inside
  // `budget`, but the true price error this lane can carry is 1.2 x budget.
  const double split_price = kMid + 0.6 * kBudget;
  const double split_embedded = split_price - 0.6 * kBudget;
  ASSERT_LT(std::fabs(split_price - kMid), kBudget);           // each term...
  ASSERT_LT(std::fabs(split_price - split_embedded), kBudget); // ...passes alone
  ASSERT_GT(std::fabs(split_price - kMid) + std::fabs(split_price - split_embedded), kBudget);
  EXPECT_FALSE(shared_lane_residual_within_budget(split_price, kMid, split_embedded, kBudget));

  // A lane whose COMBINED error is inside the budget stays acceptable — the
  // tightening must not reject lanes that do prove the bound.
  const double good_price = kMid + 0.3 * kBudget;
  const double good_embedded = good_price - 0.3 * kBudget;
  EXPECT_TRUE(shared_lane_residual_within_budget(good_price, kMid, good_embedded, kBudget));

  // Exactly at the bound: the sum is what is compared, and <= budget is accepted.
  const double edge_price = kMid + 0.5 * kBudget;
  const double edge_embedded = edge_price - 0.5 * kBudget;
  EXPECT_TRUE(shared_lane_residual_within_budget(edge_price, kMid, edge_embedded, kBudget));

  // Either term alone blowing the budget still rejects (strictly stronger gate).
  EXPECT_FALSE(shared_lane_residual_within_budget(kMid + 1.5 * kBudget, kMid, kMid + 1.5 * kBudget,
                                                  kBudget));
  EXPECT_FALSE(shared_lane_residual_within_budget(kMid, kMid, kMid + 1.5 * kBudget, kBudget));

  // Fail-closed: a non-finite input or a non-positive budget is never acceptable.
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const double inf = std::numeric_limits<double>::infinity();
  EXPECT_FALSE(shared_lane_residual_within_budget(nan, kMid, kMid, kBudget));
  EXPECT_FALSE(shared_lane_residual_within_budget(kMid, nan, kMid, kBudget));
  EXPECT_FALSE(shared_lane_residual_within_budget(kMid, kMid, nan, kBudget));
  EXPECT_FALSE(shared_lane_residual_within_budget(inf, kMid, kMid, kBudget));
  EXPECT_FALSE(shared_lane_residual_within_budget(kMid, kMid, kMid, 0.0));
  EXPECT_FALSE(shared_lane_residual_within_budget(kMid, kMid, kMid, -kBudget));
  EXPECT_FALSE(shared_lane_residual_within_budget(kMid, kMid, kMid, nan));
}

// Drives detail::SharedLaneBracket exactly as iterate_shared_lanes does — same
// next_sigma()/update() pair, same `hi - lo <= solve_tol` width termination, same
// bounded max_iter — but against a closed-form Black-76 price map rather than the
// nine-node interpolant. That substitution is what makes the lane's cost
// countable; the STEPPING LOGIC under test is the production one, not a copy.
//
// Returns the number of residual evaluations, or -1 when the endpoints do not
// bracket (which production's initialize_shared_lane rejects before iterating).
// The two endpoint evaluations are excluded: initialize_shared_lane pays those
// identically before and after R-11a, so the iteration count is the comparable
// unit. In production each of these evals is 12 barycentric interps plus a
// 48-node premium quadrature — this count IS the lane's cost.
[[nodiscard]] int drive_shared_lane_bracket(double F, double K, double T, double df, Side side,
                                            double mid, double sigma_lo, double sigma_hi,
                                            double solve_tol, std::uint16_t max_iter,
                                            double *sigma_out) {
  atx::vol::detail::SharedLaneBracket bracket{};
  bracket.lo = sigma_lo;
  bracket.hi = sigma_hi;
  bracket.f_lo = black76_price(F, K, T, sigma_lo, df, side) - mid;
  bracket.f_hi = black76_price(F, K, T, sigma_hi, df, side) - mid;
  if (!(bracket.f_lo < 0.0 && bracket.f_hi >= 0.0)) {
    return -1;
  }
  int evals = 0;
  for (std::uint16_t iteration = 0; iteration < max_iter; ++iteration) {
    if (bracket.hi - bracket.lo <= solve_tol) {
      break;
    }
    const double sigma = bracket.next_sigma();
    bracket.update(sigma, black76_price(F, K, T, sigma, df, side) - mid);
    ++evals;
  }
  *sigma_out = 0.5 * (bracket.lo + bracket.hi);
  return evals;
}

TEST(SharedLaneIteration, IllinoisStepCutsEvalsPerLane) {
  // R-11a. A shared lane opens on [interp.sigma_lo(), sigma_mkt] and its root
  // structurally HUGS the `hi` end: sigma_mkt is the Black-76 iv of an AMERICAN
  // mid, which always overstates the American iv the lane is actually solving
  // for, so the root sits a few percent below `hi`. That is the one geometry
  // plain regula falsi handles worst — the secant lands in the outer quarter, the
  // 25%-shrink guard rejects it, and the step degenerates to pure bisection for
  // the whole solve. Bisecting a ~0.1-0.75 wide bracket to solve_tol = 1e-7 costs
  // ~21-23 evals, versus the ~4-8 safeguarded-Newton evals of the scalar path the
  // shared route replaced.
  //
  // The fixture mirrors that geometry on the widest sigma-box the route admits.
  constexpr double T = 1.0;
  constexpr double r = 0.05;
  constexpr double q = 0.02;
  constexpr double S = 100.0;
  const double F = S * std::exp((r - q) * T);
  const double df = std::exp(-r * T);
  constexpr double kSolveTol = 1.0e-7;  // production solve_tol at the default iv_tol = 1e-7
  constexpr std::uint16_t kMaxIter = 64u;

  int total_evals = 0;
  int worst_evals = 0;
  int lanes = 0;
  double worst_sigma_error = 0.0;
  for (int index = 0; index < 96; ++index) {
    const double t = static_cast<double>(index) / 95.0;
    const double K = 72.0 + 56.0 * t;
    const double sigma_mkt = 0.80 + (0.15 - 0.80) * t; // steep [0.15, 0.8] smile
    const double sigma_lo = 0.35 * 0.15;               // the route's 0.35 * min_seed
    const double sigma_root = 0.95 * sigma_mkt;        // root hugs `hi`, as in production
    for (const Side side : {Side::Call, Side::Put}) {
      const double mid = black76_price(F, K, T, sigma_root, df, side);
      double sigma = 0.0;
      const int evals = drive_shared_lane_bracket(F, K, T, df, side, mid, sigma_lo, sigma_mkt,
                                                  kSolveTol, kMaxIter, &sigma);
      if (evals < 0) {
        continue;
      }
      ++lanes;
      total_evals += evals;
      worst_evals = std::max(worst_evals, evals);
      worst_sigma_error = std::max(worst_sigma_error, std::fabs(sigma - sigma_root));
    }
  }
  ASSERT_GT(lanes, 100) << "fixture must exercise a substantive lane population";
  const double mean_evals = static_cast<double>(total_evals) / static_cast<double>(lanes);

  // Speed is not bought with accuracy: termination is still on bracket WIDTH, so
  // the accepted sigma (the bracket midpoint) is within solve_tol/2 of the true
  // root regardless of how the step chooses its probes. This must hold before and
  // after R-11a — it is what makes the eval-count comparison fair.
  EXPECT_LE(worst_sigma_error, kSolveTol) << "worst |sigma - root| = " << worst_sigma_error;

  // The headline. Measured on this fixture, mean / worst evals per lane:
  //     falsi + the old 25% trust region (pre-R-11a)  22.50 / 24
  //     Illinois, trust region replaced by a backstop   5.19 / 11
  // Thresholds sit just above the measured values so a regression in either the
  // step rule or the safeguard interaction fails here rather than silently
  // costing throughput. NB both are well under SharedLaneBracket's 24-step
  // secant budget, so the bisection backstop is not what is being measured.
  EXPECT_LE(mean_evals, 7.0) << "mean evals/lane = " << mean_evals;
  EXPECT_LE(worst_evals, 13) << "worst evals/lane = " << worst_evals;
}

TEST(BuildObservationsEuropean, UltraShortTenorBypassesShortcut) {
  constexpr double T = 1.0 / 365.25;
  constexpr double r = 0.04;
  constexpr double q = 0.02;
  const double F = 100.0 * std::exp((r - q) * T);
  const double df = std::exp(-r * T);
  const Chain chain =
      make_american_chain({97.0, 98.0, 99.0, 100.0, 101.0, 102.0, 103.0}, T, r, q, 0.30);
  CalibOpts opts = calib_default_opts();
  opts.max_spread_vol = 5.0;
  opts.min_vega_weight = 0.0;
  opts.max_otm_shortcut_premium_spread_frac = 100.0;
  opts.min_otm_shortcut_T = 7.0 / 365.25;

  const auto result = build_observations_european(chain, 100.0, r, F, T, df, opts, {}, std::nullopt,
                                                  1.0e-7, 64, AmericanMethod::AndersenLake);
  ASSERT_TRUE(result.has_value()) << (result ? std::string{} : result.error().to_string());
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
      const auto acc = obs_accepted(c, static_cast<std::uint16_t>(s), side, kF, kT, kDf, opts);

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
  EXPECT_TRUE(o.warm_start_deam_adjacent_strikes);
  EXPECT_TRUE(o.use_shared_boundary_deam);
  EXPECT_FALSE(o.audit_accurate_inversions);

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
  EXPECT_EQ(o.max_iter_cold_fast, 10u); // impl = 10 (header comment's "5" is stale)
  EXPECT_DOUBLE_EQ(o.wing_floor_alpha, 0.0);
  EXPECT_TRUE(o.lee_bound_project);
  EXPECT_FALSE(o.morozov_stop); // impl = 0 (header comment's "1" is stale)
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
  EXPECT_DOUBLE_EQ(o.max_spread_to_mid_pct, 0.60); // impl = 0.60 (comment's "0.40" is stale)
}

TEST(CalibOpts, ValidationRejectsPersistedPoliciesThatAreNotImplemented) {
  std::vector<CalibOpts> unsupported;

  CalibOpts interval = calib_default_opts();
  interval.loss_kind = CalibLossKind::Interval;
  unsupported.push_back(interval);
  CalibOpts shared_rho = calib_default_opts();
  shared_rho.essvi_rho_mode = EssviRhoMode::Shared;
  unsupported.push_back(shared_rho);
  CalibOpts asymmetric = calib_default_opts();
  asymmetric.essvi_asymmetric_rho = true;
  unsupported.push_back(asymmetric);
  CalibOpts fallback = calib_default_opts();
  fallback.essvi_fallback_rmse_threshold = 0.02;
  unsupported.push_back(fallback);
  CalibOpts butterfly = calib_default_opts();
  butterfly.n_butterfly_grid = 128u;
  unsupported.push_back(butterfly);
  for (const ResidualBasisKind basis :
       {ResidualBasisKind::Chebyshev, ResidualBasisKind::WingBspline, ResidualBasisKind::Fengler}) {
    CalibOpts residual = calib_default_opts();
    residual.residual_basis_kind = basis;
    unsupported.push_back(residual);
  }

  ASSERT_TRUE(validate_calib_options(calib_default_opts()).has_value());
  for (const CalibOpts &opts : unsupported) {
    const auto status = validate_calib_options(opts);
    ASSERT_FALSE(status.has_value());
    EXPECT_EQ(status.error().code(), ErrorCode::NotImplemented);
  }
}

TEST(CalibOpts, ValidationAcceptsEveryImplementedResidualIdentity) {
  // None is the disabled-layer identity: valid while the residual layer is off
  // (the default). Enabling it with a None basis is a no-op, tested separately.
  {
    CalibOpts opts = calib_default_opts(); // residual_disable == true, None basis
    EXPECT_TRUE(validate_calib_options(opts).has_value());
  }
  // HingeQuad and C2Bspline are the implemented enabled bases.
  for (const ResidualBasisKind basis :
       {ResidualBasisKind::HingeQuad, ResidualBasisKind::C2Bspline}) {
    CalibOpts opts = calib_default_opts();
    opts.residual_disable = false;
    opts.residual_basis_kind = basis;
    EXPECT_TRUE(validate_calib_options(opts).has_value());
  }
}

TEST(CalibOpts, ValidationRejectsEnabledResidualWithNoneBasis) {
  // "No persisted no-op": a config that enables the residual layer yet leaves the
  // basis None fits nothing and must be rejected as a contradiction. The disabled
  // default (also None basis) stays valid — the guard keys on residual_disable.
  CalibOpts noop = calib_default_opts();
  noop.residual_disable = false; // basis stays None
  const auto status = validate_calib_options(noop);
  ASSERT_FALSE(status.has_value());
  EXPECT_EQ(status.error().code(), ErrorCode::InvalidArgument);

  EXPECT_TRUE(validate_calib_options(calib_default_opts()).has_value());
}

// ── Inversion certification: drop-cap semantics (task 2c / carry I6) ─────
//
// The de-Am stage DROPS a node whose inversion fails or whose accurate reprice
// exceeds the half-spread budget (deamer.hpp's documented semantics). The
// certificate must tolerate such drops up to a capped fraction of usable
// nodes — one bad quote must not reject an entire risk generation — and must
// refuse certification beyond the cap, or whenever any route accepted
// proposals it never audited (a non-Andersen-Lake method).

[[nodiscard]] std::uint32_t route_propose_total(const DeAmAuditDiagnostics &a) {
  return a.shortcut.n_proposed + a.cache.n_proposed + a.fast.n_proposed + a.accurate.n_proposed;
}
[[nodiscard]] std::uint32_t route_audit_total(const DeAmAuditDiagnostics &a) {
  return a.shortcut.n_audited + a.cache.n_audited + a.fast.n_audited + a.accurate.n_audited;
}

// Chain whose mids are genuine American prices at `sigma` on carry q, except
// the strikes in `poison` (all must satisfy F < K < S): those get the RAW
// EUROPEAN price at `sigma`, which sits BELOW the American intrinsic floor
// S - K, so the American inversion has no attainable sigma and the row drops.
Chain make_poisoned_american_chain(const std::vector<double> &strikes,
                                   const std::vector<double> &poison, double T, double r, double q,
                                   double sigma) {
  Chain c = make_american_chain(strikes, T, r, q, sigma);
  const double F = 100.0 * std::exp((r - q) * T);
  const double df = std::exp(-r * T);
  for (std::size_t i = 0; i < strikes.size(); ++i) {
    for (const double bad : poison) {
      if (std::fabs(strikes[i] - bad) > 1e-9)
        continue;
      const std::size_t idx = chain_index(static_cast<std::uint16_t>(i), Side::Call);
      const double eu = black76_price(F, strikes[i], T, sigma, df, Side::Call);
      EXPECT_LT(eu, 100.0 - strikes[i]) << "poison mid must undercut intrinsic";
      const double half = std::fmin(0.002, 0.10 * eu);
      c.mids[idx] = eu;
      c.bids[idx] = eu - half;
      c.asks[idx] = eu + half;
    }
  }
  return c;
}

TEST(DeamCertification, SingleUnattainableQuoteIsDroppedAndStillCertified) {
  constexpr double T = 1.0;
  constexpr double r = 0.05;
  constexpr double q = 0.20; // heavy carry: F ~ 86 < S, so F < K < S exists
  constexpr double sigma = 0.22;
  const double F = 100.0 * std::exp((r - q) * T);
  const double df = std::exp(-r * T);
  // Exactly ONE call strike sits in the ill-conditioned F < K < S zone (where
  // an American call pins near intrinsic): the poisoned one. Every other row
  // is a well-conditioned OTM leg that must survive.
  const std::vector<double> strikes{60.0, 64.0,  68.0,  72.0,  76.0,  80.0, 84.0,
                                    92.0, 100.0, 104.0, 108.0, 112.0, 116.0};
  const Chain chain = make_poisoned_american_chain(strikes, {92.0}, T, r, q, sigma);
  CalibOpts opts = calib_default_opts();
  opts.max_spread_vol = 1.0;

  const auto result = build_observations_european(chain, 100.0, r, F, T, df, opts, {}, std::nullopt,
                                                  1.0e-7, 64, AmericanMethod::AndersenLake);
  ASSERT_TRUE(result.has_value()) << (result ? std::string{} : result.error().to_string());
  const DeAmAuditDiagnostics &audit = result->deam_audit;

  // The poisoned node was dropped from the fit set and counted, never fitted.
  ASSERT_GT(audit.n_deam_rows, 0u);
  EXPECT_EQ(audit.n_deam_rows - audit.n_deam_accepted, 1u);
  for (const FitObs &o : result->obs) {
    EXPECT_GT(std::fabs(o.K - 92.0), 1e-9) << "poisoned node must not be fitted";
  }

  // One tolerated drop in 14 usable rows certifies under the default cap —
  // the old audited==proposed rule would have rejected the whole generation.
  EXPECT_LT(route_audit_total(audit), route_propose_total(audit));
  EXPECT_TRUE(deam_inversion_certified(audit, 0.10));
}

TEST(DeamCertification, DropsBeyondCapRefuseCertification) {
  constexpr double T = 1.0;
  constexpr double r = 0.05;
  constexpr double q = 0.20;
  constexpr double sigma = 0.22;
  const double F = 100.0 * std::exp((r - q) * T);
  const double df = std::exp(-r * T);
  const std::vector<double> strikes{60.0, 64.0, 68.0, 72.0,  76.0,  80.0,  84.0,
                                    88.0, 92.0, 95.0, 100.0, 104.0, 108.0, 112.0};
  const Chain chain = make_poisoned_american_chain(strikes, {88.0, 92.0, 95.0}, T, r, q, sigma);
  CalibOpts opts = calib_default_opts();
  opts.max_spread_vol = 1.0;

  const auto result = build_observations_european(chain, 100.0, r, F, T, df, opts, {}, std::nullopt,
                                                  1.0e-7, 64, AmericanMethod::AndersenLake);
  ASSERT_TRUE(result.has_value()) << (result ? std::string{} : result.error().to_string());
  const DeAmAuditDiagnostics &audit = result->deam_audit;
  EXPECT_EQ(audit.n_deam_rows - audit.n_deam_accepted, 3u);
  // 3 of 14 usable nodes (~21%) exceeds the 10% cap: fail-closed AT the cap.
  EXPECT_FALSE(deam_inversion_certified(audit, 0.10));
}

TEST(DeamCertification, PredicateCapsDropsAndRefusesUnauditedAcceptance) {
  DeAmAuditDiagnostics clean{};
  clean.accurate.n_proposed = 20;
  clean.accurate.n_audited = 20;
  clean.accurate.n_accepted = 20;
  clean.n_deam_rows = 20;
  clean.n_deam_accepted = 20;
  EXPECT_TRUE(deam_inversion_certified(clean, 0.10));

  DeAmAuditDiagnostics one_drop = clean;
  one_drop.n_deam_accepted = 19;
  one_drop.accurate.n_accepted = 19;
  EXPECT_TRUE(deam_inversion_certified(one_drop, 0.10)); // 5% <= 10%

  DeAmAuditDiagnostics over_cap = clean;
  over_cap.n_deam_accepted = 15;
  over_cap.accurate.n_accepted = 15;
  EXPECT_FALSE(deam_inversion_certified(over_cap, 0.10)); // 25% > 10%

  // A route that accepted more than it audited (the vacuous-certification
  // shape of AmericanMethod::Baw, task 2b / carry I4) can never certify.
  DeAmAuditDiagnostics unaudited = clean;
  unaudited.accurate.n_audited = 0;
  EXPECT_FALSE(deam_inversion_certified(unaudited, 0.10));

  // Degenerate ledgers and bad budgets fail closed.
  EXPECT_FALSE(deam_inversion_certified(DeAmAuditDiagnostics{}, 0.10));
  EXPECT_FALSE(deam_inversion_certified(clean, -0.10));
  EXPECT_FALSE(deam_inversion_certified(clean, std::numeric_limits<double>::quiet_NaN()));
}

} // namespace
