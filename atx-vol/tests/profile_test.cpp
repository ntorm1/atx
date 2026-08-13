#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>
#include <vector>

#include "atx/vol/calib.hpp"        // OptimizationLevel, CalibLossKind
#include "atx/vol/fit_policy.hpp"   // FitPolicyConfig (the routing gate under test)
#include "atx/vol/profile.hpp"
#include "atx/vol/types.hpp"        // ErrorCode
#include "atx/vol/universe.hpp"     // Underlying, Chain
#include "atx/vol/vol_surface.hpp"  // Parametrization, ResidualBasisKind

// Underlier-profile-registry coverage, ported from the C ats-vol tests
// test_profile_classifier.c (classifier + default registry + tick-size) and the
// tier-priority case from test_universe_cadence.c.
//
// The universe set/get-profile roundtrip case is intentionally NOT mirrored: it
// exercises `AtsVolUnderlying::profile_ptr`, a field atx-vol's `Underlying`
// omits (see the profile.hpp PORT NOTE), so there is no C++ surface for it.

namespace {

using atx::vol::CalibLossKind;
using atx::vol::Chain;
using atx::vol::ClassifierInputs;
using atx::vol::classify_profile;
using atx::vol::classify_underlier;
using atx::vol::classify_underlier_with_ticker;
using atx::vol::ErrorCode;
using atx::vol::FitPolicyConfig;
using atx::vol::OptimizationLevel;
using atx::vol::Parametrization;
using atx::vol::PricingRoute;
using atx::vol::Profile;
using atx::vol::profile_default;
using atx::vol::profile_lookup;
using atx::vol::profile_make_cold_fast;
using atx::vol::profile_tier_priority;
using atx::vol::ProfileKind;
using atx::vol::ProfileVerdict;
using atx::vol::ResidualBasisKind;
using atx::vol::tick_size;
using atx::vol::ticker_seed_profile;
using atx::vol::Underlying;

// ── Classifier-input fixtures (mirror the C helpers) ──────────────────────

ClassifierInputs make_spy_inputs() {
  ClassifierInputs in{};
  in.n_live_quotes = 8000u;
  in.n_live_expiries = 35u;
  in.n_atm_quotes = 800u;
  in.median_spread_pct = 0.01;
  in.n_front_expiries = 8u;
  in.has_weeklies = true;
  in.htb_flag = false;
  in.vol_product = false;
  in.n_dividends = 0u;
  in.event_distance_days = 90u;
  in.forward_dispersion_bp = 5.0;
  in.median_q_eff = 0.012;
  return in;
}

ClassifierInputs make_smallcap_inputs() {
  ClassifierInputs in{};
  in.n_live_quotes = 40u;         // sparse
  in.n_live_expiries = 4u;
  in.n_atm_quotes = 6u;
  in.median_spread_pct = 0.50;    // very wide
  in.forward_dispersion_bp = 200.0;  // high dispersion
  in.median_q_eff = 0.0;
  return in;
}

// Build a chain with a flat bid/ask on every (strike, side), SoA-consistent.
Chain make_flat_chain(double T, const std::vector<double>& strikes, double bid,
                      double ask) {
  Chain c;
  c.uid = 1u;
  c.expiry_id = 0u;
  c.T = T;
  c.strikes = strikes;
  const std::size_t n2 = strikes.size() * 2u;
  c.bids.assign(n2, bid);
  c.asks.assign(n2, ask);
  c.bid_sizes.assign(n2, 1);
  c.ask_sizes.assign(n2, 1);
  c.mids.assign(n2, 0.5 * (bid + ask));
  c.ivs.assign(n2, std::numeric_limits<double>::quiet_NaN());
  c.ts_ns.assign(n2, 0);
  c.flags.assign(n2, std::uint8_t{0});
  return c;
}

// A sparse, wide-market underlier: few quotes, ~67% spreads => ILLIQUID.
Underlying make_sparse_underlier() {
  Underlying u;
  u.uid = 1u;
  u.ticker = "ZZZZ";
  u.spot = 50.0;
  u.flags = 0u;
  u.chains.push_back(make_flat_chain(0.05, {40.0, 45.0, 50.0, 55.0, 60.0},
                                     /*bid*/ 1.0, /*ask*/ 2.0));
  return u;
}

// ── classify_profile (ports test_profile_classifier.c) ────────────────────

TEST(ProfileClassifier, ClassifyProfile_SpyShape_ReturnsIndexEtf) {
  const ProfileVerdict v = classify_profile(make_spy_inputs());
  EXPECT_EQ(static_cast<int>(v.kind),
            static_cast<int>(ProfileKind::IndexEtfUltraLiquid));
  EXPECT_GT(v.confidence, 0.6);
}

TEST(ProfileClassifier, ClassifyProfile_SmallcapShape_ReturnsIlliquid) {
  const ProfileVerdict v = classify_profile(make_smallcap_inputs());
  EXPECT_EQ(static_cast<int>(v.kind),
            static_cast<int>(ProfileKind::IlliquidSmallCap));
  EXPECT_GT(v.confidence, 0.5);
}

TEST(ProfileClassifier, ClassifyProfile_HtbFlag_ShortCircuitsToHtb) {
  ClassifierInputs in = make_spy_inputs();  // otherwise SPY-like
  in.htb_flag = true;
  const ProfileVerdict v = classify_profile(in);
  EXPECT_EQ(static_cast<int>(v.kind),
            static_cast<int>(ProfileKind::HtbDividendName));
  EXPECT_GT(v.confidence, 0.85);
}

TEST(ProfileClassifier, ClassifyProfile_VolProductHint_ShortCircuits) {
  ClassifierInputs in = make_spy_inputs();
  in.vol_product = true;
  const ProfileVerdict v = classify_profile(in);
  EXPECT_EQ(static_cast<int>(v.kind), static_cast<int>(ProfileKind::VolProduct));
  EXPECT_GT(v.confidence, 0.85);
}

TEST(ProfileClassifier, ClassifyProfile_ImminentEarnings_ReturnsMegaCapEvent) {
  ClassifierInputs in = make_spy_inputs();
  // Reduce SPY-like signals so the 2-vote earnings bonus flips the bucket.
  in.n_live_quotes = 2500u;
  in.median_spread_pct = 0.04;
  in.n_front_expiries = 1u;
  in.event_distance_days = 5u;
  const ProfileVerdict v = classify_profile(in);
  EXPECT_EQ(static_cast<int>(v.kind),
            static_cast<int>(ProfileKind::MegaCapEvent));
}

// Quote density is a per-expiry property. Two boards carrying the SAME number of
// two-sided legs, one over four expiries and one over sixty, are not the same
// kind of board; an absolute leg count cannot tell them apart.
TEST(ProfileClassifier, ClassifyProfile_QuoteDensity_IsPerExpiry) {
  ClassifierInputs dense{};
  dense.n_live_quotes = 1200u;
  dense.n_live_expiries = 4u;
  dense.n_quoted_expiries = 4u; // 300 legs/expiry
  dense.median_spread_pct = 0.10;

  ClassifierInputs spread_thin = dense;
  spread_thin.n_live_expiries = 60u;
  spread_thin.n_quoted_expiries = 60u; // 20 legs/expiry

  EXPECT_EQ(classify_profile(dense).kind, ProfileKind::IndexEtfUltraLiquid);
  EXPECT_EQ(classify_profile(spread_thin).kind, ProfileKind::OrdinarySingleName);
}

// A 40%-wide book cannot be served by a profile whose quote filter assumes a
// penny market, however dense it is and however tight its listing cadence looks.
// The spread axis's illiquid vote is counted last with a strict `>`, so without
// an explicit veto it loses every tie to a more liquid bucket.
TEST(ProfileClassifier, ClassifyProfile_WideBook_VetoesTheLiquidBuckets) {
  ClassifierInputs in{};
  in.n_live_quotes = 1020u;
  in.n_live_expiries = 12u;
  in.n_quoted_expiries = 12u; // 85 legs/expiry -> the `liquid` density tier
  in.median_spread_pct = 0.41;
  in.n_front_expiries = 8u; // and the cadence axis votes index ETF
  EXPECT_EQ(classify_profile(in).kind, ProfileKind::OrdinarySingleName);

  // The same board quoted tight keeps a liquid verdict -- the veto is the
  // spread's doing, not the density's.
  in.median_spread_pct = 0.10;
  EXPECT_EQ(classify_profile(in).kind, ProfileKind::LiquidSingleName);
}

// ── classify_underlier (chain-state aggregation) ──────────────────────────

TEST(ProfileClassifier, ClassifyUnderlier_SparseWideChain_ReturnsIlliquid) {
  const ProfileVerdict v = classify_underlier(make_sparse_underlier());
  EXPECT_EQ(static_cast<int>(v.kind),
            static_cast<int>(ProfileKind::IlliquidSmallCap));
}

TEST(ProfileClassifier, ClassifyUnderlier_HtbFlag_ShortCircuitsToHtb) {
  Underlying u = make_sparse_underlier();
  u.flags = atx::vol::kUflagHtb;  // set the HTB bit
  const ProfileVerdict v = classify_underlier(u);
  EXPECT_EQ(static_cast<int>(v.kind),
            static_cast<int>(ProfileKind::HtbDividendName));
}

TEST(ProfileClassifier, ClassifyUnderlierWithTicker_SeedHit_ReturnsSeedKind) {
  // AAPL is a compiled-in MEGA_CAP_EVENT seed; the seed short-circuits chain
  // aggregation, so even the sparse underlier reports MEGA_CAP_EVENT.
  const ProfileVerdict v =
      classify_underlier_with_ticker(make_sparse_underlier(), "AAPL");
  EXPECT_EQ(static_cast<int>(v.kind),
            static_cast<int>(ProfileKind::MegaCapEvent));
  EXPECT_GT(v.confidence, 0.9);
}

TEST(ProfileClassifier, ClassifyUnderlierWithTicker_Miss_FallsBackToChainStats) {
  const ProfileVerdict v =
      classify_underlier_with_ticker(make_sparse_underlier(), "ZZZZ");
  EXPECT_EQ(static_cast<int>(v.kind),
            static_cast<int>(ProfileKind::IlliquidSmallCap));
}

// ── Near-money structure (the identifiability features) ───────────────────

// The band the near-money count used to be taken over was 0.5*S < K < 1.5*S,
// which in log-moneyness reaches -0.69 down but only +0.41 up. It is symmetric
// now, and narrow enough that "near the money" means it.
TEST(ProfileClassifier, ClassifierInputs_NearMoneyBand_IsSymmetricInLogMoneyness) {
  Underlying u;
  u.uid = 1u;
  u.ticker = "ZZZZ";
  u.spot = 100.0;
  // |ln(0.55)| = 0.598 and |ln(1.80)| = 0.588 -- both outside the band, but the
  // old price band admitted the 55 strike and rejected the 180 one.
  u.chains.push_back(make_flat_chain(0.05, {55.0, 180.0}, 1.0, 1.2));

  const auto in = atx::vol::classifier_inputs_from_underlier(u);
  EXPECT_EQ(in.n_live_quotes, 4u);
  EXPECT_EQ(in.n_atm_quotes, 0u);
  EXPECT_EQ(in.n_identifiable_expiries, 0u);
}

TEST(ProfileClassifier, ClassifierInputs_IdentifiableExpiries_NeedNearMoneyStrikeDepth) {
  Underlying u;
  u.uid = 1u;
  u.ticker = "ZZZZ";
  u.spot = 100.0;
  u.chains.push_back(make_flat_chain(0.05, {95.0, 98.0, 102.0, 105.0}, 1.0, 1.2));
  u.chains.push_back(make_flat_chain(0.30, {97.0, 100.0, 103.0}, 1.0, 1.2));
  u.chains.push_back(make_flat_chain(0.60, {30.0, 40.0, 300.0, 400.0}, 1.0, 1.2));

  const auto in = atx::vol::classifier_inputs_from_underlier(u);
  EXPECT_EQ(in.n_quoted_expiries, 3u);
  EXPECT_EQ(in.max_near_money_strikes, 4u);
  EXPECT_EQ(in.n_identifiable_expiries, 1u) << "only the four-strike expiry qualifies";
  EXPECT_EQ(in.n_atm_quotes, 14u) << "(4 + 3) near-money strikes, both sides";
}

// The median spread used to be taken over the first 256 two-sided legs in chain
// order, and chains are sorted ascending in T -- so on any board with more than
// 256 legs in its front expiries it measured the front, not the board.
TEST(ProfileClassifier, ClassifierInputs_SpreadMedian_SpansEveryExpiry) {
  Underlying u;
  u.uid = 1u;
  u.ticker = "ZZZZ";
  u.spot = 100.0;
  std::vector<double> tight_strikes;
  for (int i = 0; i < 100; ++i) {
    tight_strikes.push_back(90.0 + 0.2 * static_cast<double>(i));
  }
  std::vector<double> wide_strikes;
  for (int i = 0; i < 200; ++i) {
    wide_strikes.push_back(90.0 + 0.1 * static_cast<double>(i));
  }
  // 200 legs at a 1% spread in front, 400 legs at 67% behind it.
  u.chains.push_back(make_flat_chain(0.05, tight_strikes, 1.00, 1.01));
  u.chains.push_back(make_flat_chain(0.50, wide_strikes, 1.00, 2.00));

  const auto in = atx::vol::classifier_inputs_from_underlier(u);
  EXPECT_EQ(in.n_live_quotes, 600u);
  EXPECT_GT(in.median_spread_pct, 0.5) << "the board median is wide; only the front is tight";
}

// ── median_spread_pct is a statistic of the board, not of its row order ─────
//
// A leg quoted bid = 0.95 / ask = 1.05 has mid exactly 1.0, so its relative
// spread is exactly `ask - bid`. The two widths below are therefore exact
// decimal values and every expectation in this block is an equality, not a
// tolerance.
constexpr double kTightBid = 0.95;
constexpr double kTightAsk = 1.05; // relative spread 0.10
constexpr double kWideBid = 0.75;
constexpr double kWideAsk = 1.25; // relative spread 0.50

struct Leg {
  double strike;
  double bid;
  double ask;
};

// A chain whose strike axis is exactly `legs`, IN THE GIVEN ORDER -- the axis
// the loader builds is in first-seen row order, not sorted, which is the degree
// of freedom these tests exercise. Both sides of a strike carry its quote.
Chain make_chain_from_legs(double T, const std::vector<Leg>& legs) {
  Chain c;
  c.uid = 1u;
  c.expiry_id = 0u;
  c.T = T;
  const std::size_t n2 = legs.size() * 2u;
  c.strikes.reserve(legs.size());
  c.bids.assign(n2, 0.0);
  c.asks.assign(n2, 0.0);
  c.bid_sizes.assign(n2, 1);
  c.ask_sizes.assign(n2, 1);
  c.mids.assign(n2, 0.0);
  c.ivs.assign(n2, std::numeric_limits<double>::quiet_NaN());
  c.ts_ns.assign(n2, 0);
  c.flags.assign(n2, std::uint8_t{0});
  for (std::size_t i = 0; i < legs.size(); ++i) {
    c.strikes.push_back(legs[i].strike);
    for (std::size_t side = 0; side < 2u; ++side) {
      const std::size_t ix = i * 2u + side;
      c.bids[ix] = legs[i].bid;
      c.asks[ix] = legs[i].ask;
      c.mids[ix] = 0.5 * (legs[i].bid + legs[i].ask);
    }
  }
  return c;
}

// 299 wide strikes then 301 tight ones, one expiry. Both sides quoted, so the
// leg stream is 598 wide values followed by 602 tight ones: 1200 legs whose
// EXACT median is the tight width, because the tight legs are the majority.
std::vector<Leg> make_wide_then_tight_legs() {
  std::vector<Leg> legs;
  legs.reserve(600);
  for (int i = 0; i < 299; ++i) {
    legs.push_back(Leg{50.0 + 0.1 * static_cast<double>(i), kWideBid, kWideAsk});
  }
  for (int i = 0; i < 301; ++i) {
    legs.push_back(Leg{100.0 + 0.1 * static_cast<double>(i), kTightBid, kTightAsk});
  }
  return legs;
}

// The estimator must answer with the board's median, not with the median of the
// ~256-entry decimated sample it can afford to keep. Those differ here by
// construction: of 1200 legs, 602 are tight and 598 wide, so the exact median is
// TIGHT -- but a sample that keeps every k-th leg of a stream whose wide legs
// come first splits the sample evenly between the two widths and lands on the
// WIDE value instead.
TEST(ProfileClassifier, ClassifierInputs_SpreadMedian_IsTheBoardMedianNotADecimatedSample) {
  Underlying u;
  u.uid = 1u;
  u.ticker = "ZZZZ";
  u.spot = 100.0;
  u.chains.push_back(make_chain_from_legs(0.05, make_wide_then_tight_legs()));

  const auto in = atx::vol::classifier_inputs_from_underlier(u);
  ASSERT_EQ(in.n_live_quotes, 1200u);
  // One bin of slack: the statistic is the true median quantised DOWN to the bin
  // grid, and 0.10 sits on a bin boundary that a leg's floating-point width can
  // land either side of. The decimated sample this replaces answered 0.50 here,
  // so the assertion has 400 bins of headroom over the behaviour it pins.
  EXPECT_NEAR(in.median_spread_pct, 0.10, 0.0011)
      << "602 of 1200 legs are tight, so the median leg is a tight one";
}

// Rank, not just membership: the statistic must be the MEDIAN of the widths
// present, not the narrowest or the widest of them. Three equally-sized groups
// put the answer strictly between the extremes, so an off-by-rank estimator
// lands on 0.05 or 0.60 rather than 0.20.
TEST(ProfileClassifier, ClassifierInputs_SpreadMedian_SelectsTheMiddleRankNotAnExtreme) {
  std::vector<Leg> legs;
  legs.reserve(300);
  for (int i = 0; i < 100; ++i) {
    const double k = 50.0 + 0.1 * static_cast<double>(i);
    legs.push_back(Leg{k, 0.975, 1.025}); // width 0.05
  }
  for (int i = 0; i < 100; ++i) {
    const double k = 100.0 + 0.1 * static_cast<double>(i);
    legs.push_back(Leg{k, 0.90, 1.10}); // width 0.20
  }
  for (int i = 0; i < 100; ++i) {
    const double k = 150.0 + 0.1 * static_cast<double>(i);
    legs.push_back(Leg{k, 0.70, 1.30}); // width 0.60
  }

  Underlying u;
  u.uid = 1u;
  u.ticker = "ZZZZ";
  u.spot = 100.0;
  u.chains.push_back(make_chain_from_legs(0.05, legs));

  const auto in = atx::vol::classifier_inputs_from_underlier(u);
  ASSERT_EQ(in.n_live_quotes, 600u);
  EXPECT_NEAR(in.median_spread_pct, 0.20, 0.0011)
      << "200 legs below and 200 above -- the median group is the 0.20 one";
}

// The same 1200 legs, presented in two different strike-axis orders. The board
// is the same board; the statistic must not move. A decimated sample cannot
// satisfy this -- which order the legs arrive in decides which of them the
// sample keeps.
TEST(ProfileClassifier, ClassifierInputs_SpreadMedian_IsInvariantToStrikeAxisOrder) {
  std::vector<Leg> forward = make_wide_then_tight_legs();
  std::vector<Leg> reversed(forward.rbegin(), forward.rend());

  Underlying a;
  a.uid = 1u;
  a.ticker = "ZZZZ";
  a.spot = 100.0;
  a.chains.push_back(make_chain_from_legs(0.05, forward));

  Underlying b;
  b.uid = 1u;
  b.ticker = "ZZZZ";
  b.spot = 100.0;
  b.chains.push_back(make_chain_from_legs(0.05, reversed));

  const auto in_a = atx::vol::classifier_inputs_from_underlier(a);
  const auto in_b = atx::vol::classifier_inputs_from_underlier(b);
  ASSERT_EQ(in_a.n_live_quotes, in_b.n_live_quotes);
  EXPECT_DOUBLE_EQ(in_a.median_spread_pct, in_b.median_spread_pct)
      << "same legs, same board, different arrival order";
}

// The T6 regression, in the shape the loader actually produces it.
//
// Admitting a one-sided row (bid = 0, ask > 0) installs it at a strike that may
// be NEW to the chain, which appends that strike to the axis and pushes every
// later strike along -- so the two-sided legs are reordered even though not one
// of them changed. The classifier skips a bid = 0 leg outright, so the board's
// two-sided population is bit-identical between the two arms and the statistic
// over it must be too.
TEST(ProfileClassifier, ClassifierInputs_SpreadMedian_UnmovedByAdmittedOneSidedLegs) {
  const std::vector<Leg> two_sided = make_wide_then_tight_legs();

  Underlying without;
  without.uid = 1u;
  without.ticker = "ZZZZ";
  without.spot = 100.0;
  without.chains.push_back(make_chain_from_legs(0.05, two_sided));

  // Same two-sided legs, but each one now sits behind a bid-less strike that the
  // loader admitted ahead of it -- the axis order every two-sided leg sees is
  // different, and 600 extra strikes carry no bid at all.
  std::vector<Leg> with_bounds;
  with_bounds.reserve(two_sided.size() * 2u);
  for (std::size_t i = 0; i < two_sided.size(); ++i) {
    with_bounds.push_back(Leg{1000.0 + static_cast<double>(i), 0.0, 0.40});
    with_bounds.push_back(two_sided[two_sided.size() - 1u - i]);
  }

  Underlying with;
  with.uid = 1u;
  with.ticker = "ZZZZ";
  with.spot = 100.0;
  with.chains.push_back(make_chain_from_legs(0.05, with_bounds));

  const auto in_without = atx::vol::classifier_inputs_from_underlier(without);
  const auto in_with = atx::vol::classifier_inputs_from_underlier(with);

  EXPECT_EQ(in_with.n_live_quotes, in_without.n_live_quotes)
      << "a bid = 0 leg is not a live quote";
  EXPECT_DOUBLE_EQ(in_with.median_spread_pct, in_without.median_spread_pct)
      << "admitting bounds must not move a statistic computed over two-sided legs";
}

// ── Registry (ports the default-profile cases) ────────────────────────────

TEST(ProfileRegistry, ProfileDefault_NoArg_ReturnsOrdinary) {
  const Profile& def = profile_default();
  EXPECT_EQ(static_cast<int>(def.kind),
            static_cast<int>(ProfileKind::OrdinarySingleName));
  // Stable pointer: the no-arg default is the ordinary registry slot.
  const auto ord = profile_lookup(ProfileKind::OrdinarySingleName);
  ASSERT_TRUE(ord.has_value());
  EXPECT_EQ(&def, *ord);
}

TEST(ProfileRegistry, ProfileLookup_ThreeDefaults_HaveRequiredFields) {
  const auto spy = profile_lookup(ProfileKind::IndexEtfUltraLiquid);
  const auto ord = profile_lookup(ProfileKind::OrdinarySingleName);
  const auto ill = profile_lookup(ProfileKind::IlliquidSmallCap);
  ASSERT_TRUE(spy.has_value());
  ASSERT_TRUE(ord.has_value());
  ASSERT_TRUE(ill.has_value());

  // atm_band tightens with liquidity.
  EXPECT_LT((*spy)->forward_atm_band, (*ord)->forward_atm_band);
  EXPECT_LT((*ord)->forward_atm_band, (*ill)->forward_atm_band);
  // QUICK_MARK iteration cap is non-zero on every profile.
  EXPECT_GT((*spy)->calib.max_iter_quick_mark, std::uint16_t{0});
  EXPECT_GT((*ord)->calib.max_iter_quick_mark, std::uint16_t{0});
  EXPECT_GT((*ill)->calib.max_iter_quick_mark, std::uint16_t{0});
  // SPY lives at TRADING by default; illiquid at QUICK_MARK.
  EXPECT_EQ(static_cast<int>((*spy)->optimization_level),
            static_cast<int>(OptimizationLevel::Trading));
  EXPECT_EQ(static_cast<int>((*ill)->optimization_level),
            static_cast<int>(OptimizationLevel::QuickMark));
  // All three default to the AL cache route.
  EXPECT_EQ(static_cast<int>((*spy)->pricing_route),
            static_cast<int>(PricingRoute::B76AlCache));
}

TEST(ProfileRegistry, ProfileLookup_BetweenTierKinds_ResolveToDefault) {
  EXPECT_TRUE(profile_lookup(ProfileKind::MegaCapEvent).has_value());
  EXPECT_TRUE(profile_lookup(ProfileKind::LiquidSingleName).has_value());
  EXPECT_TRUE(profile_lookup(ProfileKind::HtbDividendName).has_value());
  EXPECT_TRUE(profile_lookup(ProfileKind::VolProduct).has_value());
}

TEST(ProfileRegistry, ProfileLookup_InvalidKind_ReturnsNotFound) {
  const auto bad = profile_lookup(static_cast<ProfileKind>(99));
  ASSERT_FALSE(bad.has_value());
  EXPECT_EQ(bad.error().code(), ErrorCode::NotFound);
}

// Lookup-table value spot-check (hard rule 4): the SPY-like slot must carry the
// exact ported table values.
TEST(ProfileRegistry, ProfileLookup_SpyLike_HasExactTableValues) {
  const auto spy = profile_lookup(ProfileKind::IndexEtfUltraLiquid);
  ASSERT_TRUE(spy.has_value());
  const Profile* p = *spy;

  EXPECT_EQ(static_cast<int>(p->base_surface),
            static_cast<int>(Parametrization::Essvi));
  EXPECT_EQ(static_cast<int>(p->pricing_route),
            static_cast<int>(PricingRoute::B76AlCache));
  EXPECT_DOUBLE_EQ(p->filter.wide_spread_pct, 0.50);
  EXPECT_DOUBLE_EQ(p->filter.min_vega_filter, 1.0e-8);
  EXPECT_EQ(p->calib.max_iter_trading, std::uint16_t{50});
  EXPECT_EQ(p->calib.max_iter_reference, std::uint16_t{400});
  EXPECT_DOUBLE_EQ(p->calib.huber_k, 2.0);
  EXPECT_DOUBLE_EQ(p->calib.min_vega_weight, 1.0e-8);
  EXPECT_FALSE(p->calib.residual_disable);
  EXPECT_EQ(static_cast<int>(p->calib.residual_basis_kind),
            static_cast<int>(ResidualBasisKind::HingeQuad));
  EXPECT_EQ(p->calib.residual_n_basis_terms, std::uint8_t{5});
  EXPECT_EQ(static_cast<int>(p->calib.loss_kind),
            static_cast<int>(CalibLossKind::Mid));
  EXPECT_EQ(p->full_refit_ms, 250u);
  EXPECT_EQ(p->local_refit_us, 50u);
  EXPECT_DOUBLE_EQ(p->forward_atm_band, 0.03);
  EXPECT_DOUBLE_EQ(p->subtick_zeroing_ticks, 0.0);
}

// Illiquid slot spot-check: SVI base + sub-tick zeroing distinguish it.
TEST(ProfileRegistry, ProfileLookup_Illiquid_HasExactTableValues) {
  const auto ill = profile_lookup(ProfileKind::IlliquidSmallCap);
  ASSERT_TRUE(ill.has_value());
  const Profile* p = *ill;
  EXPECT_EQ(static_cast<int>(p->base_surface),
            static_cast<int>(Parametrization::Svi));
  EXPECT_DOUBLE_EQ(p->filter.wide_spread_pct, 3.00);
  EXPECT_DOUBLE_EQ(p->subtick_zeroing_ticks, 0.5);
  EXPECT_EQ(p->full_refit_ms, 5000u);
}

// MEGA_CAP_EVENT clones SPY but loosens the prefit spread cap and drops the
// residual layer.
TEST(ProfileRegistry, ProfileLookup_MegaCapEvent_LoosensSpyBase) {
  const auto mega = profile_lookup(ProfileKind::MegaCapEvent);
  ASSERT_TRUE(mega.has_value());
  const Profile* p = *mega;
  EXPECT_DOUBLE_EQ(p->filter.wide_spread_pct, 1.20);
  EXPECT_DOUBLE_EQ(p->calib.max_spread_vol, 0.20);
  EXPECT_DOUBLE_EQ(p->calib.min_vega_weight, 1.0e-7);
  EXPECT_TRUE(p->calib.residual_disable);
  EXPECT_EQ(static_cast<int>(p->calib.residual_basis_kind),
            static_cast<int>(ResidualBasisKind::HingeQuad));
}

// ── Tier priority (ports test_universe_cadence.c) ─────────────────────────

TEST(ProfileRegistry, TierPriority_KindOrdering_LowerForMoreLiquid) {
  EXPECT_LT(profile_tier_priority(ProfileKind::IndexEtfUltraLiquid),
            profile_tier_priority(ProfileKind::MegaCapEvent));
  EXPECT_LT(profile_tier_priority(ProfileKind::MegaCapEvent),
            profile_tier_priority(ProfileKind::OrdinarySingleName));
  EXPECT_LT(profile_tier_priority(ProfileKind::OrdinarySingleName),
            profile_tier_priority(ProfileKind::IlliquidSmallCap));
  // VOL_PRODUCT maps to the ULTRA tier.
  EXPECT_EQ(profile_tier_priority(ProfileKind::VolProduct),
            profile_tier_priority(ProfileKind::IndexEtfUltraLiquid));
}

// ── Cold-fast factory ─────────────────────────────────────────────────────

TEST(ProfileRegistry, MakeColdFast_FromSpy_SetsColdFastCaps) {
  const Profile& spy = *profile_lookup(ProfileKind::IndexEtfUltraLiquid).value();
  const Profile cf = profile_make_cold_fast(spy);
  EXPECT_EQ(static_cast<int>(cf.optimization_level),
            static_cast<int>(OptimizationLevel::ColdFast));
  EXPECT_EQ(static_cast<int>(cf.calib.optimization_level),
            static_cast<int>(OptimizationLevel::ColdFast));
  EXPECT_EQ(cf.calib.max_inner_iter, std::uint16_t{12});
  EXPECT_TRUE(cf.calib.residual_disable);
  EXPECT_TRUE(cf.calib.morozov_stop);
  EXPECT_TRUE(cf.calib.lee_bound_project);
  // Filter / cadence fields inherit unchanged from the base.
  EXPECT_DOUBLE_EQ(cf.filter.wide_spread_pct, spy.filter.wide_spread_pct);
  EXPECT_EQ(cf.full_refit_ms, spy.full_refit_ms);
}

// ── OPRA tick-size lattice (ports the C tick-size case) ───────────────────

TEST(ProfileRegistry, TickSize_Lattice_MatchesOpra) {
  // Penny-pilot: 1c across all prices.
  EXPECT_NEAR(tick_size(0.50, true), 0.01, 1e-12);
  EXPECT_NEAR(tick_size(5.00, true), 0.01, 1e-12);
  // Penny Interval Program: 1c below $3, 5c at/above $3.
  EXPECT_NEAR(tick_size(0.50, false), 0.01, 1e-12);
  EXPECT_NEAR(tick_size(5.00, false), 0.05, 1e-12);
  // Boundary at $3 (>=).
  EXPECT_NEAR(tick_size(3.00, false), 0.05, 1e-12);
  // Non-finite / negative falls through to 5c.
  EXPECT_NEAR(tick_size(-1.0, false), 0.05, 1e-12);
  EXPECT_NEAR(tick_size(std::numeric_limits<double>::quiet_NaN(), true), 0.05,
              1e-12);
}

// ── 4.3 error model: the seed lookup answers with an optional ─────────────
//
// `ticker_seed_profile` used to be `bool` + a `ProfileKind&` out-param, so the
// unseeded answer was "false, and whatever you happened to leave in out_kind" —
// a shape that only works if every caller remembers not to read the out-param.
// It had no test at all. It now returns `std::optional<ProfileKind>`: absence is
// the type, not a convention, and there is no slot to misread.

TEST(TickerSeedProfile, SeededTickersAnswerWithTheirKind) {
  EXPECT_EQ(ticker_seed_profile("SPY"), std::optional{ProfileKind::IndexEtfUltraLiquid});
  EXPECT_EQ(ticker_seed_profile("TLT"), std::optional{ProfileKind::IndexEtfUltraLiquid});
  EXPECT_EQ(ticker_seed_profile("NVDA"), std::optional{ProfileKind::MegaCapEvent});
  EXPECT_EQ(ticker_seed_profile("MSFT"), std::optional{ProfileKind::MegaCapEvent});
  EXPECT_EQ(ticker_seed_profile("JPM"), std::optional{ProfileKind::LiquidSingleName});
  // MRNA is the HTB placeholder that deliberately routes to LIQUID (matches C).
  EXPECT_EQ(ticker_seed_profile("MRNA"), std::optional{ProfileKind::LiquidSingleName});
}

TEST(TickerSeedProfile, UnseededAndEmptyTickersAnswerNullopt) {
  // The absence branch: previously reachable only as `false` beside an
  // untouched out-param, and never asserted anywhere in the tree.
  EXPECT_FALSE(ticker_seed_profile("ZZZZ").has_value());
  EXPECT_FALSE(ticker_seed_profile("").has_value());
  EXPECT_FALSE(ticker_seed_profile("spy").has_value()) << "the table is case-sensitive";
  EXPECT_FALSE(ticker_seed_profile("SPY ").has_value()) << "no trimming, exact match only";
  EXPECT_FALSE(ticker_seed_profile("SP").has_value()) << "no prefix match";
}

// Provenance, not score: a seeded verdict reports kTickerSeedConfidence, but
// testing the confidence for equality would misclassify any board whose vote
// ratio lands on that value. The seed lookup is the provenance channel and its
// two answers must not drift apart from classify_underlier_with_ticker's.
TEST(TickerSeedProfile, AgreesWithTheTickerAwareClassifier) {
  Underlying under{};
  for (const std::string_view ticker : {"SPY", "AAPL", "XOM", "ZZZZ", ""}) {
    const std::optional<ProfileKind> seed = ticker_seed_profile(ticker);
    const ProfileVerdict verdict = classify_underlier_with_ticker(under, ticker);
    if (seed.has_value()) {
      EXPECT_EQ(verdict.kind, *seed) << ticker;
      EXPECT_DOUBLE_EQ(verdict.confidence, atx::vol::kTickerSeedConfidence) << ticker;
    } else {
      EXPECT_EQ(verdict.kind, classify_underlier(under).kind) << ticker;
    }
  }
}

// ── Reproducibility of the routing decision (sprint W4) ──────────────────
//
// The classifier is the library's answer to "resolve a configuration when the
// caller supplied none". An answer that changes between two adjacent trading
// days on the same underlier has resolved nothing, so these cases pin the two
// properties that make the verdict a function of the BOARD rather than of the
// calendar or of one vote.

// A listing viewed from two different weekdays is the SAME listing. The cadence
// axis used to ask how far away the front expiry was (`T < 0.01` years, i.e.
// 3.65 days), which on a Monday is false for a Friday-only name and on a
// Wednesday is true -- and it votes for the most liquid bucket, so two thirds of
// the universe was reclassified by the day of the week.
TEST(ProfileClassifier, ClassifyUnderlier_WeeklyListing_IsWeekdayInvariant) {
  const std::vector<double> strikes{40.0, 45.0, 50.0, 55.0, 60.0};
  const auto weekly_from = [&strikes](double first_friday_days) {
    Underlying u;
    u.uid = 1u;
    u.ticker = "ZZZZ";
    u.spot = 50.0;
    for (int week = 0; week < 6; ++week) {
      const double days = first_friday_days + 7.0 * static_cast<double>(week);
      u.chains.push_back(make_flat_chain(days / 365.0, strikes, 1.00, 1.05));
    }
    return u;
  };
  // Same Friday-only ladder, seen on a Monday (front expiry 4 days out) and on
  // a Wednesday (2 days out).
  const ProfileVerdict monday = classify_underlier(weekly_from(4.0));
  const ProfileVerdict wednesday = classify_underlier(weekly_from(2.0));
  EXPECT_EQ(monday.kind, wednesday.kind);
  EXPECT_DOUBLE_EQ(monday.confidence, wednesday.confidence);
  EXPECT_NE(monday.kind, ProfileKind::IndexEtfUltraLiquid)
      << "a Friday-only ladder is not an index ETF on any weekday";
}

// The cadence axis still fires for what it was meant to catch: a product that
// lists an expiry every trading day.
TEST(ProfileClassifier, ClassifyProfile_DailyExpiryCycle_VotesIndexEtf) {
  ClassifierInputs weekly{};
  weekly.n_live_quotes = 900u;
  weekly.n_live_expiries = 12u;
  weekly.n_quoted_expiries = 12u;  // 75 legs/expiry -> the `liquid` tier
  weekly.median_spread_pct = 0.10; // -> the `liquid` tier
  weekly.n_front_expiries = 1u;
  ClassifierInputs daily = weekly;
  daily.n_front_expiries = atx::vol::kMinDailyCycleExpiries;

  EXPECT_EQ(classify_profile(weekly).kind, ProfileKind::LiquidSingleName);
  // Two axes say `liquid`, the cadence axis says index ETF: the plurality still
  // holds, but the board is no longer unanimous.
  const ProfileVerdict v = classify_profile(daily);
  EXPECT_EQ(v.kind, ProfileKind::LiquidSingleName);
  EXPECT_LT(v.confidence, classify_profile(weekly).confidence);
}

// Confidence is ORDINAL: it reports how far apart the axes are on the liquidity
// ladder. A vote share cannot express this -- both boards below have exactly one
// axis dissenting from a two-axis majority, so both scored 2/3 before.
TEST(ProfileClassifier, ClassifyProfile_Confidence_MeasuresLadderSpread) {
  ClassifierInputs near_miss{};
  near_miss.n_live_quotes = 900u;
  near_miss.n_live_expiries = 12u;
  near_miss.n_quoted_expiries = 12u;  // 75 legs/expiry -> `liquid`
  near_miss.median_spread_pct = 0.20; // -> `ordinary`, one rung away
  near_miss.n_front_expiries = 1u;

  ClassifierInputs contradiction = near_miss;
  contradiction.median_spread_pct = 0.60; // -> `illiquid`, two rungs away

  EXPECT_DOUBLE_EQ(classify_profile(near_miss).confidence, 0.75);
  EXPECT_DOUBLE_EQ(classify_profile(contradiction).confidence, 0.50);
  // Unanimity is the only way to score 1.0.
  ClassifierInputs unanimous = near_miss;
  unanimous.median_spread_pct = 0.10;
  EXPECT_DOUBLE_EQ(classify_profile(unanimous).confidence, 1.0);
}

// The default routing gate must not sit where the confidence has mass. Adjacent
// rungs are the ordinary condition of a mid-liquidity board and must route
// directly; only a genuine contradiction is worth the held-out selector.
TEST(ProfileClassifier, ClassifyProfile_Confidence_StraddlesTheDefaultGate) {
  const FitPolicyConfig defaults{};
  ClassifierInputs in{};
  in.n_live_quotes = 900u;
  in.n_live_expiries = 12u;
  in.n_quoted_expiries = 12u;
  in.n_front_expiries = 1u;

  in.median_spread_pct = 0.20; // one rung from the density axis
  EXPECT_GE(classify_profile(in).confidence, defaults.min_direct_confidence);
  in.median_spread_pct = 0.60; // two rungs
  EXPECT_LT(classify_profile(in).confidence, defaults.min_direct_confidence);
}

} // namespace
