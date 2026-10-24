// Lanes H0-A and H1 (r6, 2026-10-23): the immutable fitted serving record of one expiry -
// build from a completed QuoteSliceFit, ownership, the typed node answers, tiered serve,
// the tick-rate implied volatility, routing, versions, bytes, concurrency, the allocation
// budget of the Ladder and Family reads, lane K3's error statements (H1: the families
// certified through deam_family_certify, the per-point bound, typed Sampled statements),
// the fitter's hand-off and the evaluate_quote_slice fast path.
#include "atx/vol/api/fitting/slice_serve_record.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "atx/vol/api/fitting/quote_slice.hpp"
#include "atx/vol/api/pricing/american.hpp"
#include "atx/vol/api/pricing/american_iv.hpp"
#include "fitting/deam_iv_cache.hpp"
#include "fitting/slice_serve_record_detail.hpp"
#include "pricing/american_boundary.hpp"       // AlExpiryGeometry::Impl (bytes)
#include "pricing/american_family_error.hpp"   // K3: family_price_bounded, envelopes
#include "pricing/american_iv_premium.hpp"      // iv_premium_edge_band_top (the L1c band)
#include "pricing/american_sigma_q_family.hpp" // K1 fused price + vega (internal)
#include "support/alloc_counter.hpp"

namespace atx::vol {
namespace {

constexpr double kSpot = 100.0;
constexpr double kRate = 0.03;
constexpr double kYield = 0.01;

constexpr double kInf = std::numeric_limits<double>::infinity();

std::uint64_t bits(double value) { return std::bit_cast<std::uint64_t>(value); }

double smile(double strike, double forward) {
  const double k = std::log(strike / forward);
  return 0.22 - 0.10 * k + 0.25 * k * k;
}

// A per-strike calendar yield (the funding-PV shape: every strike its own carry).
double strike_yield(double strike, double spread) {
  return kYield + spread * std::sin(0.37 * strike);
}

struct Chain {
  QuoteSliceMarket market;
  std::vector<QuoteSliceQuote> quotes;
};

// Accurate Andersen-Lake prices on a smile, strikes k_lo, k_lo + step, ... (count of
// them), both sides, a per-strike yield, +/- 1 % spreads (at least 0.01; a price under its
// spread is bidless). The default grid is 70-130 every 2.5.
Chain smile_chain(double T, double yield_spread, double k_lo = 70.0, double step = 2.5,
                  int count = 25, double vol_shift = 0.0) {
  Chain out;
  out.market = QuoteSliceMarket{kSpot, T, T, kRate, kYield, 0.0};
  const double forward = kSpot * std::exp((kRate - kYield) * T);
  for (int i = 0; i < count; ++i) {
    const double K = k_lo + step * static_cast<double>(i);
    const double q = strike_yield(K, yield_spread);
    for (const Side side : {Side::Call, Side::Put}) {
      const Result<double> price =
          andersen_lake(kSpot, K, T, smile(K, forward) + vol_shift, kRate, q, side);
      if (!price) continue;
      const double spread = std::max(0.01, 0.01 * *price);
      out.quotes.push_back({K, side, std::max(*price - spread, 0.0), *price + spread, q});
    }
  }
  return out;
}

QuoteSliceFit fit_chain(double T, double yield_spread, double k_lo = 70.0, double step = 2.5,
                        int count = 25, double vol_shift = 0.0) {
  const Chain chain = smile_chain(T, yield_spread, k_lo, step, count, vol_shift);
  Result<QuoteSliceFit> fit = fit_quote_slice(chain.market, chain.quotes);
  EXPECT_TRUE(fit.has_value());
  return std::move(*fit);
}

// The 0.25 y fixture, fitted once per process (fit_quote_slice is deterministic).
const QuoteSliceFit &base_fit() {
  static const QuoteSliceFit fit = fit_chain(0.25, 2.0e-4);
  return fit;
}

std::shared_ptr<const SliceServeRecord> base_record() {
  static const std::shared_ptr<const SliceServeRecord> record = [] {
    Result<std::shared_ptr<const SliceServeRecord>> built = build_serve_record(base_fit());
    EXPECT_TRUE(built.has_value());
    return built ? *built : nullptr;
  }();
  return record;
}

const ServedNodeSide &side_of(const ServedNode &node, Side side) {
  return side == Side::Call ? node.call : node.put;
}

const ServeSideSupport &support_of(const SliceServeRecord &record, Side side) {
  return side == Side::Call ? record.support().call : record.support().put;
}

// The contract's effective yield exactly as the fitter derives it (ContinuousYield:
// the calendar yield on the variance clock, T_cal == T_var here).
double effective_q(const QuoteSliceFit &fit, double strike) {
  const auto &curve = fit.carry_curve;
  EXPECT_FALSE(curve.empty());
  for (const QuoteSliceCarryPoint &point : curve) {
    if (point.strike == strike) return point.continuous_yield;
  }
  ADD_FAILURE() << "no carry point at " << strike;
  return std::numeric_limits<double>::quiet_NaN();
}

// QuoteSliceFit::carry_curve's documented interpolation: linear in log strike between the
// carry points, flat beyond them.
double carry_between(const QuoteSliceFit &fit, double strike) {
  const auto &curve = fit.carry_curve;
  if (strike <= curve.front().strike) return curve.front().continuous_yield;
  if (strike >= curve.back().strike) return curve.back().continuous_yield;
  for (std::size_t i = 1; i < curve.size(); ++i) {
    if (curve[i].strike < strike) continue;
    const double w = std::log(strike / curve[i - 1].strike) /
                     std::log(curve[i].strike / curve[i - 1].strike);
    return std::lerp(curve[i - 1].continuous_yield, curve[i].continuous_yield, w);
  }
  return curve.back().continuous_yield;
}

// Ok and Exercised are both answers (Exercised: the family map's exercise regime).
bool answered(ServeStatus status) {
  return status == ServeStatus::Ok || status == ServeStatus::Exercised;
}

ServeRequest at(double strike, Side side, double spot, ServeTier tier, ServeTier tier_max) {
  ServeRequest r;
  r.strike = strike;
  r.side = side;
  r.spot = spot;
  r.tier = tier;
  r.tier_max = tier_max;
  r.greeks = ServeGreekMask::four();
  return r;
}

ServeIvRequest iv_at(double strike, Side side, double spot, double target, ServeTier tier,
                     ServeTier tier_max) {
  ServeIvRequest r;
  r.strike = strike;
  r.side = side;
  r.spot = spot;
  r.target_price = target;
  r.tier = tier;
  r.tier_max = tier_max;
  return r;
}

// The record's own certified families with lane K3's envelope WITHDRAWN (class Uncovered,
// grid bound +inf) and the build-time probe audit kept: no statement can be validated, so
// every ladder the audit admits is Sampled. The holder is built here, never shared, so the
// test may write it before the record observes it.
std::shared_ptr<const DeAmSigmaFamilies> withdrawn_holder(const QuoteSliceFit &fit) {
  auto holder = std::const_pointer_cast<DeAmSigmaFamilies>(
      detail::build_serve_families(fit, ServeRecordPolicy{}));
  EXPECT_NE(holder, nullptr);
  if (holder == nullptr) return nullptr;
  for (ValidatedErrorEnvelope *env : {&holder->q_envelope_put, &holder->q_envelope_call}) {
    EXPECT_GT(env->audit.probes, 0u);
    env->error_class = FamilyErrorClass::Uncovered;
  }
  holder->q_bound_put = kInf;
  holder->q_bound_call = kInf;
  return holder;
}

TEST(SliceServeRecord, FixtureFitIsServedWithPerStrikeCarry) {
  const QuoteSliceFit &fit = base_fit();
  ASSERT_EQ(fit.fit_status, QuoteSliceFitStatus::Success);
  ASSERT_TRUE(fit.prices.has_value());
  ASSERT_GE(fit.prices->nodes().size(), 20u);
  ASSERT_FALSE(fit.carry_curve.empty());
  ASSERT_NE(base_record(), nullptr);
  EXPECT_EQ(fit.serve_handoff, nullptr); // no (sigma, q) families in this fit
  EXPECT_EQ(fit.serve_record, nullptr);
}

// Deliverable 1 / 4: every node's call and put price is the slice's, byte for byte.
TEST(SliceServeRecord, BuildCopiesEveryNodePriceByteForByte) {
  const auto record = base_record();
  ASSERT_NE(record, nullptr);
  const auto slice = base_fit().prices->nodes();
  const auto nodes = record->nodes();
  ASSERT_EQ(nodes.size(), slice.size());
  for (std::size_t i = 0; i < nodes.size(); ++i) {
    EXPECT_EQ(bits(nodes[i].strike), bits(slice[i].strike)) << i;
    EXPECT_EQ(bits(nodes[i].call.price), bits(slice[i].call)) << i;
    EXPECT_EQ(bits(nodes[i].put.price), bits(slice[i].put)) << i;
  }
}

// Lane C0's one-answer contract: each node side carries query_quote_slice's answer.
TEST(SliceServeRecord, NodeSidesCarryTheTypedQueryAnswer) {
  const auto record = base_record();
  ASSERT_NE(record, nullptr);
  for (const ServedNode &node : record->nodes()) {
    for (const Side side : {Side::Call, Side::Put}) {
      const ServedNodeSide &s = side_of(node, side);
      const Result<QuoteSliceQuery> query = query_quote_slice(base_fit(), node.strike, side);
      ASSERT_TRUE(query.has_value()) << node.strike;
      EXPECT_EQ(s.status, query->status);
      EXPECT_EQ(s.reason, query->reason);
      EXPECT_EQ(s.convention, query->vol_convention);
      EXPECT_EQ(bits(s.sigma), bits(query->volatility)) << node.strike;
      EXPECT_EQ(bits(s.iv_lower), bits(query->iv_lower));
      EXPECT_EQ(bits(s.iv_upper), bits(query->iv_upper));
      EXPECT_EQ(bits(s.q), bits(effective_q(base_fit(), node.strike))) << node.strike;
      if (s.convention == QuoteSliceVolConvention::FittedCurve) {
        EXPECT_EQ(s.acceptance.kind, ServeAcceptanceKind::None);
        EXPECT_EQ(s.acceptance.reason, ServeAcceptanceReason::FittedCurve);
      }
    }
  }
}

// The record's per-strike yield is the fitter's: an unmoved node's certified price is the
// accurate kernel at the node's served sigma and the record's q, to the bit.
void expect_node_yields_reproduce_prices(const QuoteSliceFit &fit, const SliceServeRecord &record,
                                         std::size_t at_least) {
  std::size_t checked = 0;
  for (const ServedNode &node : record.nodes()) {
    for (const Side side : {Side::Call, Side::Put}) {
      const ServedNodeSide &s = side_of(node, side);
      if (s.moved || s.convention != QuoteSliceVolConvention::FittedCurve) continue;
      const Result<double> cold = andersen_lake(fit.market.spot, node.strike,
                                                fit.market.variance_years, s.sigma,
                                                fit.diagnostics.effective_rate, s.q, side,
                                                fit.al_opts);
      ASSERT_TRUE(cold.has_value());
      EXPECT_EQ(bits(*cold), bits(s.price)) << node.strike;
      ++checked;
    }
  }
  EXPECT_GE(checked, at_least);
}

TEST(SliceServeRecord, NodeYieldReproducesTheCertifiedPriceToTheBit) {
  const auto record = base_record();
  ASSERT_NE(record, nullptr);
  expect_node_yields_reproduce_prices(base_fit(), *record, 40u);
}

// Review-H0A m10: the funding-PV carry (the SPY default, --dividends funding-pv) with a
// dividend PV and a calendar clock unlike the variance clock reproduces the certified
// prices to the bit as well.
TEST(SliceServeRecord, FundingPvNodeYieldReproducesTheCertifiedPriceToTheBit) {
  const QuoteSliceMarket market{kSpot, 0.25, 0.27, kRate, kYield, 0.8};
  const double forward = kSpot * std::exp((kRate - kYield) * 0.27) - 0.8;
  std::vector<QuoteSliceQuote> quotes;
  for (int i = 0; i < 21; ++i) {
    const double K = 75.0 + 2.5 * static_cast<double>(i);
    const double q = strike_yield(K, 3.0e-4);
    for (const Side side : {Side::Call, Side::Put}) {
      const Result<double> price = andersen_lake(kSpot, K, 0.25, smile(K, forward), kRate, q, side);
      if (!price) continue;
      const double spread = std::max(0.01, 0.01 * *price);
      quotes.push_back({K, side, std::max(*price - spread, 0.0), *price + spread, q});
    }
  }
  QuoteSliceConfig config;
  config.dividend_model = QuoteSliceDividendModel::AggregateFundingPresentValue;
  const Result<QuoteSliceFit> fit = fit_quote_slice(market, quotes, config);
  ASSERT_TRUE(fit.has_value());
  ASSERT_EQ(fit->fit_status, QuoteSliceFitStatus::Success) << fit->fit_error;
  ASSERT_EQ(fit->dividend_model, QuoteSliceDividendModel::AggregateFundingPresentValue);
  ServeRecordPolicy policy;
  policy.ladders = false;
  const auto record = build_serve_record(*fit, policy);
  ASSERT_TRUE(record.has_value());
  expect_node_yields_reproduce_prices(*fit, **record, 20u);
}

// Ruling b: at the fit's own spot every tier serves the certified node price (Exact); the
// tier's Greeks come from its own map and the answer declares the gap and that map's own
// statement.
TEST(SliceServeRecord, ServeAtTheFitSpotReturnsTheCertifiedNodePriceOnEveryTier) {
  const auto record = base_record();
  ASSERT_NE(record, nullptr);
  std::size_t per_tier[3] = {0, 0, 0};
  for (const ServedNode &node : record->nodes()) {
    for (const Side side : {Side::Call, Side::Put}) {
      const ServedNodeSide &s = side_of(node, side);
      for (const ServeTier tier : {ServeTier::Ladder, ServeTier::Family, ServeTier::Certified}) {
        const ServeAnswer a = record->serve(at(node.strike, side, kSpot, tier, tier));
        if (a.status != ServeStatus::Ok) continue;
        ++per_tier[static_cast<std::size_t>(tier)];
        EXPECT_EQ(bits(a.price), bits(s.price)) << node.strike;
        EXPECT_EQ(a.price_source, ServePriceSource::SliceNode);
        EXPECT_EQ(a.certificate_usd, 0.0);
        EXPECT_EQ(a.certificate_class, ServeBoundClass::Exact);
        EXPECT_EQ(a.tier, tier);
        if (tier == ServeTier::Ladder) {
          EXPECT_EQ(bits(a.map_certificate_usd), bits(s.ladder_certificate_usd));
          EXPECT_EQ(a.map_certificate_class, s.ladder_class);
          EXPECT_EQ(a.sampled, s.ladder_class == ServeBoundClass::Sampled);
        }
        if (tier == ServeTier::Family) {
          EXPECT_EQ(a.greek_source, ServeGreekSource::Family);
          EXPECT_EQ(bits(a.price_map_delta), bits(std::abs(s.family_price_delta)));
        }
        if (a.greek_source != ServeGreekSource::None) {
          EXPECT_TRUE(std::isfinite(a.price_map_delta));
          EXPECT_GE(a.price_map_delta, 0.0);
        }
      }
    }
  }
  EXPECT_GT(per_tier[0], 10u);
  EXPECT_GT(per_tier[1], 40u);
  EXPECT_GT(per_tier[2], 40u);
}

// Deliverable 4: between nodes at the fit's spot the served price is the slice's
// piecewise-linear price; the family's price lies within the declared between-node bound
// at points the build's audit did not visit (eighths of every interval).
TEST(SliceServeRecord, BetweenNodesAtTheFitSpotServeTheSliceWithinTheDeclaredBound) {
  const auto record = base_record();
  ASSERT_NE(record, nullptr);
  const auto nodes = record->nodes();
  std::size_t checked = 0;
  for (const Side side : {Side::Call, Side::Put}) {
    const ServeSideSupport &support = support_of(*record, side);
    ASSERT_TRUE(support.family);
    ASSERT_TRUE(std::isfinite(support.between_node_bound_usd));
    for (std::size_t i = 0; i + 1 < nodes.size(); ++i) {
      for (int m = 1; m <= 7; ++m) {
        const double K = nodes[i].strike +
                         (nodes[i + 1].strike - nodes[i].strike) * static_cast<double>(m) / 8.0;
        const std::optional<double> slice = base_fit().prices->price(K, side);
        ASSERT_TRUE(slice.has_value());
        const ServeAnswer c =
            record->serve(at(K, side, kSpot, ServeTier::Certified, ServeTier::Certified));
        ASSERT_TRUE(answered(c.status)) << K;
        EXPECT_EQ(bits(c.price), bits(*slice)) << K;
        EXPECT_EQ(c.price_source, ServePriceSource::SlicePiecewiseLinear);
        const ServeAnswer f =
            record->serve(at(K, side, kSpot, ServeTier::Family, ServeTier::Family));
        if (!answered(f.status)) continue;
        EXPECT_EQ(bits(f.price), bits(*slice)) << K;
        EXPECT_LE(f.price_map_delta, support.between_node_bound_usd) << K;
        ++checked;
      }
    }
  }
  EXPECT_GT(checked, 200u);
}

// Review-H0A m3: between nodes at the fit's spot the served price is the slice, which
// claims no statement; the Greek map's own statement is carried beside it with its class.
TEST(SliceServeRecord, TheSliceBetweenNodesClaimsNoCertificateAndTheMapCarriesItsOwn) {
  const auto record = base_record();
  ASSERT_NE(record, nullptr);
  for (const Side side : {Side::Call, Side::Put}) {
    const ServeAnswer a = record->serve(at(101.3, side, kSpot, ServeTier::Family,
                                           ServeTier::Family));
    ASSERT_EQ(a.status, ServeStatus::Ok);
    EXPECT_EQ(a.price_source, ServePriceSource::SlicePiecewiseLinear);
    EXPECT_TRUE(std::isnan(a.certificate_usd));
    EXPECT_EQ(a.certificate_class, ServeBoundClass::None);
    const ServeAnswer moved = record->serve(at(101.3, side, 1.0001 * kSpot, ServeTier::Family,
                                               ServeTier::Family));
    ASSERT_EQ(moved.status, ServeStatus::Ok);
    EXPECT_EQ(a.map_certificate_class, moved.certificate_class);
    if (moved.certificate_class == ServeBoundClass::RefinementValidated) {
      EXPECT_TRUE(std::isfinite(a.map_certificate_usd));
    }
  }
}

// The family tier serves K1's fused price and vega at a moved spot, to the bit, and its
// other Greeks carry a typed reason (without lane A0's bundle: BundleUnavailable).
TEST(SliceServeRecord, FamilyTierServesTheFamilyMapToTheBitAtAMovedSpot) {
  const QuoteSliceFit &fit = base_fit();
  const auto families = detail::build_serve_families(fit, ServeRecordPolicy{});
  ASSERT_NE(families, nullptr);
  const auto built = detail::build_serve_record_with(fit, ServeRecordPolicy{}, families);
  ASSERT_TRUE(built.has_value());
  const auto &record = *built;
  const double S = kSpot * 1.013;
  std::size_t checked = 0;
  for (const ServedNode &node : record->nodes()) {
    for (const Side side : {Side::Call, Side::Put}) {
      const ServedNodeSide &s = side_of(node, side);
      const AlSigmaQFamily *fam = families->of_q(side);
      if (fam == nullptr || !(s.tiers & kServeNodeFamily)) continue;
      const ServeAnswer a = record->serve(at(node.strike, side, S, ServeTier::Family,
                                             ServeTier::Family));
      const sigma_q_family_detail::FusedPriceVega pv =
          sigma_q_family_detail::price_and_vega(*fam, S, node.strike, s.sigma, s.q);
      if (pv.status != sigma_q_family_detail::FusedStatus::Ok) continue;
      ASSERT_TRUE(a.status == ServeStatus::Ok || a.status == ServeStatus::Exercised);
      EXPECT_EQ(bits(a.price), bits(pv.price)) << node.strike;
      EXPECT_EQ(bits(a.vega), bits(pv.vega)) << node.strike;
      EXPECT_EQ(bits(a.sigma), bits(s.sigma));
      EXPECT_EQ(a.price_source, ServePriceSource::Family);
      EXPECT_EQ(a.price_map_delta, 0.0);
      // A number on both sides of the exercise edge: Exercised there (A0's reason too).
      EXPECT_EQ(a.reason[static_cast<std::size_t>(ServeGreek::Vega)],
                a.status == ServeStatus::Exercised ? ServeGreekReason::Exercised
                                                   : ServeGreekReason::Served);
#if !ATX_VOL_SERVE_RECORD_A0
      EXPECT_EQ(a.reason[static_cast<std::size_t>(ServeGreek::Delta)],
                ServeGreekReason::BundleUnavailable);
      EXPECT_TRUE(std::isnan(a.delta));
#endif
      ++checked;
    }
  }
  EXPECT_GT(checked, 40u);
  EXPECT_EQ(record->clocks().lazy_family_builds, 0u); // supplied: no kernel work
  EXPECT_EQ(record->support().call.origin, ServeFamilyOrigin::Supplied);
}

// A node side's family_reasons are exactly the Family tier's reasons at the fit's spot for
// the full mask (A0's bundle under the seam, else vega only).
TEST(SliceServeRecord, NodeFamilyReasonsAreTheFamilyTiersReasonsAtTheFitSpot) {
  const auto record = base_record();
  ASSERT_NE(record, nullptr);
  std::size_t checked = 0;
  for (const ServedNode &node : record->nodes()) {
    for (const Side side : {Side::Call, Side::Put}) {
      const ServedNodeSide &s = side_of(node, side);
      if (!(s.tiers & kServeNodeFamily)) continue;
      ServeRequest r = at(node.strike, side, record->spot(), ServeTier::Family, ServeTier::Family);
      r.greeks = ServeGreekMask::all();
      const ServeAnswer a = record->serve(r);
      ASSERT_TRUE(answered(a.status)) << node.strike;
      for (std::size_t k = 0; k < kServeGreekCount; ++k) {
        EXPECT_EQ(a.reason[k], s.family_reasons[k]) << node.strike << " greek " << k;
      }
      ++checked;
    }
  }
  EXPECT_GT(checked, 40u);
}

// Tier 0: price and four Greeks from one map - the ladder's price within its statement of
// the family, and its delta / gamma / vega the derivatives of that same map.
TEST(SliceServeRecord, LadderServesPriceAndFourGreeksFromOneMap) {
  const auto record = base_record();
  ASSERT_NE(record, nullptr);
  const QuoteSliceFit &fit = base_fit();
  const auto families = detail::build_serve_families(fit, ServeRecordPolicy{});
  ASSERT_NE(families, nullptr);
  std::size_t checked = 0;
  for (const ServedNode &node : record->nodes()) {
    for (const Side side : {Side::Call, Side::Put}) {
      const ServedNodeSide &s = side_of(node, side);
      if (!(s.tiers & kServeNodeLadder)) continue;
      for (const double move : {0.997, 1.0021, 1.0137}) {
        const double S = kSpot * move;
        const ServeAnswer a =
            record->serve(at(node.strike, side, S, ServeTier::Ladder, ServeTier::Ladder));
        if (a.status != ServeStatus::Ok) continue;
        EXPECT_EQ(a.price_source, ServePriceSource::Ladder);
        EXPECT_EQ(a.greek_source, ServeGreekSource::Ladder);
        EXPECT_EQ(a.price_map_delta, 0.0);
        EXPECT_EQ(bits(a.certificate_usd), bits(s.ladder_certificate_usd));
        EXPECT_EQ(a.certificate_class, s.ladder_class);
        const auto fam = sigma_q_family_detail::price_and_vega(*families->of_q(side), S,
                                                               node.strike, s.sigma, s.q);
        ASSERT_EQ(fam.status, sigma_q_family_detail::FusedStatus::Ok);
        EXPECT_LE(std::abs(a.price - fam.price), s.ladder_certificate_usd) << node.strike;
        // delta / gamma: the ladder price's own central differences (one map).
        const double h = 1.0e-4 * S;
        const ServeAnswer up = record->serve(at(node.strike, side, S + h, ServeTier::Ladder,
                                                ServeTier::Ladder));
        const ServeAnswer dn = record->serve(at(node.strike, side, S - h, ServeTier::Ladder,
                                                ServeTier::Ladder));
        if (up.status != ServeStatus::Ok || dn.status != ServeStatus::Ok) continue;
        if (a.near_edge) continue; // theta / gamma withheld: the band test below
        const double fd_delta = (up.price - dn.price) / (2.0 * h);
        const double fd_gamma = (up.price - 2.0 * a.price + dn.price) / (h * h);
        EXPECT_NEAR(a.delta, fd_delta, 1.0e-7) << node.strike;
        EXPECT_NEAR(a.gamma, fd_gamma, 2.0e-5) << node.strike;
        EXPECT_NEAR(a.vega, fam.vega, 1.0e-4 * std::max(1.0, std::abs(fam.vega)))
            << node.strike;
        const double theta = record->rate() * a.price -
                             (record->rate() - s.q) * S * a.delta -
                             0.5 * s.sigma * s.sigma * S * S * a.gamma;
        EXPECT_EQ(bits(a.theta), bits(theta));
        EXPECT_EQ(a.reason[static_cast<std::size_t>(ServeGreek::Theta)],
                  ServeGreekReason::Served);
        ++checked;
      }
    }
  }
  EXPECT_GT(checked, 60u);
}

// A0 fix round (review-A0 MJ1): within kServeEdgeBand sigma sqrt T of the node's converged
// exercise edge the ladder's identity theta and its gamma are withheld with
// NearExerciseEdge; price, delta and vega stay served; outside the band all four serve.
// Strikes every 0.5 over 90-120. Under the default sigma half-width (0.25) a ladder's
// low-sigma samples cross the edge first, so every tier-0 row here stays >= 0.57 sigma sqrt T
// off it (diagnostic in the H0-A report); a 5 % sigma window lets ladders reach the band.
TEST(SliceServeRecord, LadderWithholdsThetaAndGammaInsideTheNearExerciseEdgeBand) {
  static const QuoteSliceFit fine = fit_chain(0.25, 2.0e-4, 90.0, 0.5, 61);
  ServeRecordPolicy policy;
  policy.ladder_sigma_half_width = 0.05;
  const auto built = build_serve_record(fine, policy);
  ASSERT_TRUE(built.has_value());
  const std::shared_ptr<const SliceServeRecord> &record = *built;
  const double sqrt_t = std::sqrt(record->tau());
  std::size_t in_band = 0;
  std::size_t outside = 0;
  for (const ServedNode &node : record->nodes()) {
    for (const Side side : {Side::Call, Side::Put}) {
      const ServedNodeSide &s = side_of(node, side);
      if (!(s.tiers & kServeNodeLadder)) continue;
      const Result<double> edge = exercise_boundary(node.strike, record->tau(), s.sigma,
                                                    record->rate(), s.q, side, fine.al_opts);
      if (edge) {
        EXPECT_EQ(bits(s.edge_spot), bits(*edge)) << node.strike;
      } else {
        ASSERT_EQ(edge.error().code(), ErrorCode::OutOfRange) << node.strike;
        EXPECT_EQ(s.edge_spot, side == Side::Put ? 0.0 : kInf) << node.strike;
      }
      for (int j = 0; j <= 32; ++j) {
        const double S = kSpot * std::exp(-0.04 + 0.0025 * static_cast<double>(j));
        const ServeAnswer a =
            record->serve(at(node.strike, side, S, ServeTier::Ladder, ServeTier::Ladder));
        if (a.status != ServeStatus::Ok) continue;
        const double ratio = side == Side::Put ? S / s.edge_spot : s.edge_spot / S;
        const double want = std::log(ratio) / (s.sigma * sqrt_t);
        if (std::isinf(want)) {
          EXPECT_EQ(a.edge_sd, want);
        } else {
          EXPECT_NEAR(a.edge_sd, want, 1.0e-12 * std::max(1.0, std::abs(want))) << node.strike;
        }
        EXPECT_EQ(a.near_edge, a.edge_sd < kServeEdgeBand) << node.strike << " S " << S;
        const auto reason = [&](ServeGreek g) { return a.reason[static_cast<std::size_t>(g)]; };
        EXPECT_TRUE(std::isfinite(a.price));
        EXPECT_EQ(reason(ServeGreek::Delta), ServeGreekReason::Served);
        EXPECT_EQ(reason(ServeGreek::Vega), ServeGreekReason::Served);
        EXPECT_TRUE(std::isfinite(a.delta) && std::isfinite(a.vega));
        if (a.near_edge) {
          EXPECT_EQ(reason(ServeGreek::Gamma), ServeGreekReason::NearExerciseEdge);
          EXPECT_EQ(reason(ServeGreek::Theta), ServeGreekReason::NearExerciseEdge);
          EXPECT_TRUE(std::isnan(a.gamma) && std::isnan(a.theta)) << node.strike;
          ++in_band;
        } else {
          EXPECT_EQ(reason(ServeGreek::Gamma), ServeGreekReason::Served);
          EXPECT_EQ(reason(ServeGreek::Theta), ServeGreekReason::Served);
          EXPECT_TRUE(std::isfinite(a.gamma) && std::isfinite(a.theta)) << node.strike;
          ++outside;
        }
      }
    }
  }
  EXPECT_GT(in_band, 0u);
  EXPECT_GT(outside, 100u);
#if !ATX_VOL_SERVE_RECORD_A0
  // Without A0's bundle the family tier has no edge and serves no theta / gamma.
  const ServeAnswer fam =
      record->serve(at(101.25, Side::Put, 1.003 * kSpot, ServeTier::Family, ServeTier::Family));
  ASSERT_TRUE(answered(fam.status));
  EXPECT_TRUE(std::isnan(fam.edge_sd));
  EXPECT_FALSE(fam.near_edge);
  EXPECT_TRUE(std::isnan(fam.gamma) && std::isnan(fam.theta));
#endif
}

// Review-W2ci For H1 item 2 / review-H0A m13: the family tier's statement is lane K3's
// PER-POINT bound at the contract (family_price_bounded on the side's audited envelope),
// at every strike of the domain (no probed band), RefinementValidated where it is finite;
// none (NaN) otherwise or when the request skips it.
TEST(SliceServeRecord, FamilyCertificateIsK3sPerPointBound) {
  const QuoteSliceFit &fit = base_fit();
  const auto families = detail::build_serve_families(fit, ServeRecordPolicy{});
  ASSERT_NE(families, nullptr);
  const auto built = detail::build_serve_record_with(fit, ServeRecordPolicy{}, families);
  ASSERT_TRUE(built.has_value());
  const double S = 1.003 * kSpot;
  std::size_t validated = 0;
  for (const Side side : {Side::Call, Side::Put}) {
    const AlSigmaQFamily *f = families->of_q(side);
    const ValidatedErrorEnvelope *env = families->q_envelope(side);
    ASSERT_NE(f, nullptr);
    ASSERT_NE(env, nullptr);
    for (const double K : {71.3, 84.9, 101.3, 117.5, 129.1}) {
      const ServeAnswer a = (*built)->serve(at(K, side, S, ServeTier::Family, ServeTier::Family));
      if (!answered(a.status)) continue;
      const double q = carry_between(fit, K);
      const double sigma = (*built)->curve().iv(std::log(K / fit.diagnostics.forward));
      const FamilyBoundedPrice fb = family_price_bounded(*f, *env, S, K, sigma, q);
      if (env->covered() && fb.status == AlSigmaQFamilyStatus::Ok && std::isfinite(fb.bound_usd)) {
        EXPECT_EQ(bits(a.certificate_usd), bits(fb.bound_usd)) << K;
        EXPECT_EQ(a.certificate_class, ServeBoundClass::RefinementValidated);
        EXPECT_FALSE(a.sampled);
        ++validated;
      } else {
        EXPECT_TRUE(std::isnan(a.certificate_usd)) << K;
        EXPECT_EQ(a.certificate_class, ServeBoundClass::None);
      }
      ServeRequest skip = at(K, side, S, ServeTier::Family, ServeTier::Family);
      skip.family_bound = false;
      const ServeAnswer b = (*built)->serve(skip);
      EXPECT_EQ(bits(b.price), bits(a.price));
      EXPECT_TRUE(std::isnan(b.certificate_usd));
      EXPECT_EQ(b.certificate_class, ServeBoundClass::None);
    }
  }
  EXPECT_GE(validated, 5u); // the call side at least (per-point bound ~1e-11 USD)
}

// Review-W2ci M1 / m1 / m2: the record's families are certified through lane K3's
// deam_family_certify like the fitter's - the audited envelope (9 probes) is the one the
// record reads - and a self-built and a supplied holder state one contract.
TEST(SliceServeRecord, FamiliesCarryK3sAuditedEnvelopeAndOneErrorContract) {
  const QuoteSliceFit &fit = base_fit();
  const auto record = base_record();
  ASSERT_NE(record, nullptr);
  const auto families = detail::build_serve_families(fit, ServeRecordPolicy{});
  ASSERT_NE(families, nullptr);
  const auto supplied = detail::build_serve_record_with(fit, ServeRecordPolicy{}, families);
  ASSERT_TRUE(supplied.has_value());
  for (const Side side : {Side::Call, Side::Put}) {
    const ServeSideSupport &own = support_of(*record, side);
    const ServeSideSupport &held = support_of(**supplied, side);
    const ValidatedErrorEnvelope *env = families->q_envelope(side);
    ASSERT_TRUE(own.family);
    ASSERT_NE(env, nullptr);
    EXPECT_EQ(own.origin, ServeFamilyOrigin::Built);
    EXPECT_EQ(held.origin, ServeFamilyOrigin::Supplied);
    EXPECT_EQ(own.audit_probes, kDeAmFamilyProbes);
    EXPECT_EQ(own.audit_probes, env->audit.probes);
    EXPECT_EQ(own.audit_violations, env->audit.violations);
    EXPECT_EQ(own.envelope_status, static_cast<std::uint8_t>(env->status));
    EXPECT_EQ(own.unverified_nodes, env->contributions.unverified_nodes);
    EXPECT_EQ(own.family_class, env->covered() ? ServeErrorClass::RefinementValidated
                                               : ServeErrorClass::Uncovered);
    EXPECT_EQ(bits(own.family_bound_rel), bits(env->audit.max_rel)); // K3's estimate
    EXPECT_EQ(bits(own.domain_bound_usd), bits(families->q_bound(side)));
    // One contract: the self-built and the supplied holder state the same numbers.
    EXPECT_EQ(own.family_class, held.family_class);
    EXPECT_EQ(own.audit_probes, held.audit_probes);
    EXPECT_EQ(bits(own.family_bound_rel), bits(held.family_bound_rel));
    EXPECT_EQ(bits(own.domain_bound_usd), bits(held.domain_bound_usd));
    EXPECT_EQ(own.ladders_validated, held.ladders_validated);
    EXPECT_EQ(own.ladders_sampled, held.ladders_sampled);
  }
  EXPECT_EQ(record->support().call.family_class, ServeErrorClass::RefinementValidated);
}

// Review-W2ci M1 items 3 / 5: every ladder's statement is classed. A RefinementValidated
// ladder's statement is its verified plane tail (audited: no check point above it) plus
// K3's largest per-point bound over its sample and check points; a Sampled one (served only
// under sampled_ladders, labelled) is the measured check plus 10 x K3's probe audit.
TEST(SliceServeRecord, EveryLadderStatementIsClassedAndSampledLaddersAreLabelled) {
  const auto record = base_record();
  ASSERT_NE(record, nullptr);
  for (const Side side : {Side::Call, Side::Put}) {
    const ServeSideSupport &support = support_of(*record, side);
    std::size_t validated = 0;
    std::size_t sampled = 0;
    for (const ServedNode &node : record->nodes()) {
      const ServedNodeSide &s = side_of(node, side);
      if (!(s.tiers & kServeNodeLadder)) {
        EXPECT_TRUE(std::isnan(s.ladder_certificate_usd));
        EXPECT_EQ(s.ladder_class, ServeBoundClass::None);
        continue;
      }
      EXPECT_TRUE(std::isfinite(s.ladder_plane_check_usd));
      if (s.ladder_class == ServeBoundClass::RefinementValidated) {
        ++validated;
        ASSERT_TRUE(std::isfinite(s.ladder_plane_tail_usd)) << node.strike;
        ASSERT_TRUE(std::isfinite(s.ladder_family_bound_usd)) << node.strike;
        EXPECT_LE(s.ladder_plane_check_usd, s.ladder_plane_tail_usd) << node.strike;
        EXPECT_GE(s.ladder_certificate_usd, s.ladder_plane_tail_usd + s.ladder_family_bound_usd);
        // An unmoved node's slice price is the cold kernel's: K3's claim covers it.
        if (!s.moved) {
          EXPECT_GE(s.ladder_certificate_usd, std::abs(s.family_price_delta)) << node.strike;
        }
      } else {
        ASSERT_EQ(s.ladder_class, ServeBoundClass::Sampled) << node.strike;
        ++sampled;
        EXPECT_GE(s.ladder_certificate_usd, s.ladder_plane_check_usd);
        const ServeAnswer a = record->serve(at(node.strike, side, 1.002 * kSpot,
                                               ServeTier::Ladder, ServeTier::Ladder));
        if (a.status == ServeStatus::Ok) {
          EXPECT_EQ(a.certificate_class, ServeBoundClass::Sampled);
          EXPECT_TRUE(a.sampled);
        }
      }
    }
    EXPECT_EQ(validated, support.ladders_validated);
    EXPECT_EQ(sampled, support.ladders_sampled);
    EXPECT_EQ(validated + sampled, support.ladders);
  }
  // The call side's per-point bound is ~1e-11 USD on this fixture: its ladders validate.
  EXPECT_GT(record->support().call.ladders_validated, 10u);
}

// sampled_ladders = false: a ladder whose validated statement does not qualify is dropped,
// never served on its estimate.
TEST(SliceServeRecord, WithoutSampledLaddersOnlyValidatedLaddersServe) {
  ServeRecordPolicy policy;
  policy.sampled_ladders = false;
  const auto built = build_serve_record(base_fit(), policy);
  ASSERT_TRUE(built.has_value());
  for (const Side side : {Side::Call, Side::Put}) {
    const ServeSideSupport &s = support_of(**built, side);
    EXPECT_EQ(s.ladders_sampled, 0u);
    EXPECT_EQ(s.ladders, s.ladders_validated);
    EXPECT_EQ(s.ladders_validated, support_of(*base_record(), side).ladders_validated);
  }
  EXPECT_NE((*built)->version().policy_sha256, base_record()->version().policy_sha256);
}

// Review-W2ci M1 item 4 / review-H0A m6: a ladder root is Ok only on a RefinementValidated
// ladder whose statement / slope plus the Newton distance meets iv_tolerance; on a Sampled
// ladder (here every ladder: K3's envelope withdrawn, its audit kept) it is never Ok.
TEST(SliceServeRecord, ALadderIvRootIsNeverOkOnASampledStatement) {
  const QuoteSliceFit &fit = base_fit();
  const auto built = detail::build_serve_record_with(fit, ServeRecordPolicy{},
                                                     withdrawn_holder(fit));
  ASSERT_TRUE(built.has_value());
  const SliceServeRecord &record = **built;
  std::size_t tried = 0;
  for (const ServedNode &node : record.nodes()) {
    for (const Side side : {Side::Call, Side::Put}) {
      const ServedNodeSide &s = side_of(node, side);
      if (!(s.tiers & kServeNodeLadder)) continue;
      EXPECT_EQ(s.ladder_class, ServeBoundClass::Sampled) << node.strike;
      const double S = 1.002 * kSpot;
      const ServeAnswer priced =
          record.serve(at(node.strike, side, S, ServeTier::Ladder, ServeTier::Ladder));
      if (priced.status != ServeStatus::Ok) continue;
      EXPECT_TRUE(priced.sampled);
      const ServeIvAnswer iv = record.implied_vol(
          iv_at(node.strike, side, S, priced.price, ServeTier::Ladder, ServeTier::Ladder));
      if (iv.status == ServeStatus::NonMonotoneBand) continue;
      EXPECT_NE(iv.status, ServeStatus::Ok) << node.strike;
      EXPECT_NE(iv.acceptance.kind, ServeAcceptanceKind::LadderCertificate);
      EXPECT_EQ(iv.status, ServeStatus::Unconfirmed) << node.strike;
      EXPECT_EQ(iv.route, ServeRoute::ColdPath);
      ++tried;
    }
  }
  EXPECT_GT(tried, 10u);
  EXPECT_EQ(record.support().call.family_class, ServeErrorClass::Uncovered);
  EXPECT_EQ(record.support().call.ladders_validated, 0u);
}

#if ATX_VOL_SERVE_RECORD_A0
// Lane A0 seam: the family tier serves A0's bundle verbatim - price and every Greek to the
// bit, the reasons value for value, sticky strike held.
TEST(SliceServeRecord, FamilyTierGreeksAreA0sBundleToTheBit) {
  const QuoteSliceFit &fit = base_fit();
  const auto families = detail::build_serve_families(fit, ServeRecordPolicy{});
  ASSERT_NE(families, nullptr);
  const auto built = detail::build_serve_record_with(fit, ServeRecordPolicy{}, families);
  ASSERT_TRUE(built.has_value());
  const double S = 1.007 * kSpot;
  std::size_t checked = 0;
  for (const ServedNode &node : (*built)->nodes()) {
    for (const Side side : {Side::Call, Side::Put}) {
      const ServedNodeSide &s = side_of(node, side);
      if (!(s.tiers & kServeNodeFamily)) continue;
      ServeRequest r = at(node.strike, side, S, ServeTier::Family, ServeTier::Family);
      r.greeks = ServeGreekMask::all();
      const ServeAnswer a = (*built)->serve(r);
      AlSigmaQGreeksRequest req;
      req.mask = AlFamilyGreekMask::all();
      req.held_fixed = AlFamilyGreekHeldFixed::StickyStrike;
      const AlSigmaQFamilyGreeks g =
          family_greeks(*families->of_q(side), S, node.strike, s.sigma, s.q, req);
      if (g.status != AlSigmaQFamilyStatus::Ok) continue;
      ASSERT_TRUE(answered(a.status)) << node.strike;
      EXPECT_EQ(a.status == ServeStatus::Exercised, g.exercised);
      EXPECT_EQ(bits(a.price), bits(g.price)) << node.strike;
      EXPECT_EQ(bits(a.edge_sd), bits(g.edge_sd)) << node.strike; // A0 fix round
      EXPECT_EQ(a.near_edge, g.near_edge) << node.strike;
      const double want[kServeGreekCount] = {g.delta, g.gamma, g.vega, g.theta, g.rho, g.phi};
      const double got[kServeGreekCount] = {a.delta, a.gamma, a.vega, a.theta, a.rho, a.phi};
      for (std::size_t k = 0; k < kServeGreekCount; ++k) {
        EXPECT_EQ(static_cast<int>(a.reason[k]), static_cast<int>(g.reason[k])) << k;
        const bool number = g.reason[k] == AlFamilyGreekReason::Served ||
                            g.reason[k] == AlFamilyGreekReason::Exercised;
        if (number) {
          EXPECT_EQ(bits(got[k]), bits(want[k])) << node.strike << " greek " << k;
        } else {
          EXPECT_TRUE(std::isnan(got[k])) << k;
        }
      }
      ++checked;
    }
  }
  EXPECT_GT(checked, 40u);
}
#endif

#if ATX_VOL_SERVE_RECORD_K3
// Lane K3 seam: where the side family's validated envelope encloses it, a family root is
// accepted by the envelope's enclosure (no confirm solve) within the 1e-7 requirement of
// the accurate kernel's root. Both sides: on the fixture the put envelope is covered but
// its unverified quadrature nodes carry box bounds (0.04-0.5 USD, no enclosure), the call
// envelope's bound is ~1e-11 USD (H0-A report, Diagnostics).
TEST(SliceServeRecord, FamilyRootsAreAcceptedByTheEnvelopesEnclosure) {
  const auto record = base_record();
  ASSERT_NE(record, nullptr);
  const QuoteSliceFit &fit = base_fit();
  const double S = 0.998 * kSpot;
  std::size_t skipped = 0;
  for (const ServedNode &node : record->nodes()) {
    for (const Side side : {Side::Call, Side::Put}) {
      const ServedNodeSide &s = side_of(node, side);
      if (!(s.tiers & kServeNodeFamily) ||
          s.family_class != ServeErrorClass::RefinementValidated) {
        continue;
      }
      const ServeAnswer f =
          record->serve(at(node.strike, side, S, ServeTier::Family, ServeTier::Family));
      if (f.status != ServeStatus::Ok) continue;
      const double target = f.price * (1.0 + 1.0e-3);
      const ServeIvAnswer iv = record->implied_vol(
          iv_at(node.strike, side, S, target, ServeTier::Family, ServeTier::Family));
      if (iv.status != ServeStatus::Ok) continue;
      EXPECT_EQ(iv.acceptance.kind, ServeAcceptanceKind::Skipped);
      EXPECT_EQ(iv.acceptance.reason, ServeAcceptanceReason::FamilyEnclosure);
      EXPECT_LE(iv.acceptance.bound_iv, 1.0e-7);
      const Result<double> lo = andersen_lake(S, node.strike, fit.market.variance_years,
                                              iv.sigma - 1.0e-7, fit.diagnostics.effective_rate,
                                              s.q, side, fit.al_opts);
      const Result<double> hi = andersen_lake(S, node.strike, fit.market.variance_years,
                                              iv.sigma + 1.0e-7, fit.diagnostics.effective_rate,
                                              s.q, side, fit.al_opts);
      ASSERT_TRUE(lo.has_value() && hi.has_value());
      EXPECT_LE(*lo, target) << node.strike; // the accurate root lies within 1e-7
      EXPECT_GE(*hi, target) << node.strike;
      ++skipped;
    }
  }
  EXPECT_GT(skipped, 5u);
}
#endif

// Ownership: the record owns everything it reads - destroying the fit changes no answer.
TEST(SliceServeRecord, TheRecordOutlivesTheFitItWasBuiltFrom) {
  auto fit = std::make_unique<QuoteSliceFit>(fit_chain(0.25, 2.0e-4));
  ASSERT_EQ(fit->fit_status, QuoteSliceFitStatus::Success);
  Result<std::shared_ptr<const SliceServeRecord>> built = build_serve_record(*fit);
  ASSERT_TRUE(built.has_value());
  std::shared_ptr<const SliceServeRecord> record = std::move(*built);
  const std::vector<double> strikes = {85.0, 100.0, 101.3, 117.5};
  const auto answers = [&] {
    std::vector<double> out;
    for (const double K : strikes) {
      for (const Side side : {Side::Call, Side::Put}) {
        for (const ServeTier tier : {ServeTier::Ladder, ServeTier::Family, ServeTier::Certified}) {
          for (const double S : {kSpot, 1.004 * kSpot}) {
            const ServeAnswer a = record->serve(at(K, side, S, tier, ServeTier::Certified));
            out.push_back(a.price);
            out.push_back(a.vega);
            out.push_back(static_cast<double>(a.status));
          }
        }
      }
    }
    return out;
  };
  const std::vector<double> before = answers();
  fit.reset(); // the fit, its curve and its price slice are gone
  const std::vector<double> after = answers();
  ASSERT_EQ(before.size(), after.size());
  for (std::size_t i = 0; i < before.size(); ++i) EXPECT_EQ(bits(before[i]), bits(after[i])) << i;
  // A copy of the shared_ptr keeps the families and their geometries alive on its own.
  const std::shared_ptr<const SliceServeRecord> copy = record;
  record.reset();
  const ServeAnswer a = copy->serve(at(100.0, Side::Put, 1.004 * kSpot, ServeTier::Family,
                                       ServeTier::Family));
  EXPECT_EQ(a.status, ServeStatus::Ok);
  EXPECT_TRUE(std::isfinite(a.price));
}

// Review-H0A m7: a supplied holder built for another expiry is refused; the caller's own
// reference to a matching holder may be dropped before serving (the record holds it).
TEST(SliceServeRecord, AMismatchedHolderIsRefusedAndTheCallersHolderMayBeDropped) {
  const QuoteSliceFit other = fit_chain(0.5, 2.0e-4);
  const auto foreign = detail::build_serve_families(other, ServeRecordPolicy{});
  ASSERT_NE(foreign, nullptr);
  const auto refused = detail::build_serve_record_with(base_fit(), ServeRecordPolicy{}, foreign);
  ASSERT_FALSE(refused.has_value());
  EXPECT_EQ(refused.error().code(), ErrorCode::InvalidArgument);

  auto mine = detail::build_serve_families(base_fit(), ServeRecordPolicy{});
  ASSERT_NE(mine, nullptr);
  ServeRecordPolicy policy;
  policy.ladders = false;
  const auto built = detail::build_serve_record_with(base_fit(), policy, mine);
  ASSERT_TRUE(built.has_value());
  const ServeAnswer before = (*built)->serve(at(101.3, Side::Put, 1.002 * kSpot,
                                                ServeTier::Family, ServeTier::Family));
  ASSERT_EQ(before.status, ServeStatus::Ok);
  mine.reset(); // the caller's reference: the record's own keeps the families alive
  const ServeAnswer after = (*built)->serve(at(101.3, Side::Put, 1.002 * kSpot,
                                               ServeTier::Family, ServeTier::Family));
  EXPECT_EQ(after.status, ServeStatus::Ok);
  EXPECT_EQ(bits(after.price), bits(before.price));
  EXPECT_EQ(bits(after.vega), bits(before.vega));
}

TEST(SliceServeRecord, AStrikeOutsideTheCertifiedDomainRoutesToARefit) {
  const auto record = base_record();
  ASSERT_NE(record, nullptr);
  const ServeAnswer a = record->serve(at(50.0, Side::Put, kSpot, ServeTier::Ladder,
                                         ServeTier::Certified));
  EXPECT_EQ(a.status, ServeStatus::OutOfDomain);
  EXPECT_EQ(a.route, ServeRoute::Refit);
  EXPECT_TRUE(std::isnan(a.price));
  const ServeIvAnswer iv = record->implied_vol(
      iv_at(50.0, Side::Put, kSpot, 1.0, ServeTier::Ladder, ServeTier::Certified));
  EXPECT_EQ(iv.status, ServeStatus::OutOfDomain);
  EXPECT_EQ(iv.route, ServeRoute::Refit);
}

// D1: a dated-cash expiry is tier 3 only in this phase; no tier <= 2 serves it.
TEST(SliceServeRecord, ADatedCashFitIsColdOnlyWithTheSupportReason) {
  QuoteSliceFit fit = fit_chain(0.25, 2.0e-4);
  fit.dividend_model = QuoteSliceDividendModel::DatedCash;
  Result<std::shared_ptr<const SliceServeRecord>> built = build_serve_record(fit);
  ASSERT_TRUE(built.has_value());
  const SliceServeRecord &record = **built;
  EXPECT_EQ(record.support().cash_dividends, ServeCashDividends::UnsupportedInRecord);
  EXPECT_EQ(record.support().reason, ServeUnsupportedReason::DatedCash);
  EXPECT_FALSE(record.support().tiers[0]);
  EXPECT_FALSE(record.support().tiers[1]);
  EXPECT_FALSE(record.support().tiers[2]);
  EXPECT_TRUE(record.support().tiers[3]);
  EXPECT_EQ(record.clocks().lazy_family_builds, 0u);
  for (const double S : {kSpot, 1.01 * kSpot}) {
    const ServeAnswer a =
        record.serve(at(100.0, Side::Put, S, ServeTier::Ladder, ServeTier::Certified));
    EXPECT_EQ(a.status, ServeStatus::TierUnavailable);
    EXPECT_EQ(a.route, ServeRoute::ColdPath);
    EXPECT_TRUE(std::isnan(a.price));
  }
}

// Below kAlSigmaFamilyMinTau no family is built: the certified slice still serves at the
// fit's spot, and a moved spot routes to the cold path.
TEST(SliceServeRecord, AnExpiryBelowTheFamilyTauServesOnlyTheSliceAtItsSpot) {
  const QuoteSliceFit fit = fit_chain(2.0 / 365.0, 0.0, 96.0, 0.5, 17);
  ASSERT_EQ(fit.fit_status, QuoteSliceFitStatus::Success);
  Result<std::shared_ptr<const SliceServeRecord>> built = build_serve_record(fit);
  ASSERT_TRUE(built.has_value());
  const SliceServeRecord &record = **built;
  EXPECT_FALSE(record.support().call.family);
  EXPECT_EQ(record.support().put.reason, ServeUnsupportedReason::BelowFamilyTau);
  const double K = record.nodes()[record.nodes().size() / 2].strike;
  const ServeAnswer here = record.serve(at(K, Side::Put, kSpot, ServeTier::Ladder,
                                           ServeTier::Certified));
  EXPECT_EQ(here.status, ServeStatus::Ok);
  EXPECT_EQ(here.tier, ServeTier::Certified);
  EXPECT_EQ(here.greek_source, ServeGreekSource::None);
  const ServeAnswer moved = record.serve(at(K, Side::Put, 1.002 * kSpot, ServeTier::Ladder,
                                            ServeTier::Certified));
  EXPECT_EQ(moved.status, ServeStatus::TierUnavailable);
  EXPECT_EQ(moved.route, ServeRoute::ColdPath);
}

TEST(SliceServeRecord, AnAnswerOlderThanThePolicyIsStale) {
  ServeRecordPolicy policy;
  policy.max_age_ns = 1000;
  policy.ladders = false;
  Result<std::shared_ptr<const SliceServeRecord>> built = build_serve_record(base_fit(), policy);
  ASSERT_TRUE(built.has_value());
  const SliceServeRecord &record = **built;
  ServeRequest r = at(100.0, Side::Call, kSpot, ServeTier::Family, ServeTier::Certified);
  r.now_ns = record.version().as_of_ns + 999;
  EXPECT_EQ(record.serve(r).status, ServeStatus::Ok);
  r.now_ns = record.version().as_of_ns + 2000;
  const ServeAnswer stale = record.serve(r);
  EXPECT_EQ(stale.status, ServeStatus::Stale);
  EXPECT_EQ(stale.route, ServeRoute::Rebuild);
  EXPECT_EQ(stale.age_ns, 2000);
}

// Review-H0A m1 / m2: implied_vol is stale-checked like serve(), and with max_age_ns set a
// request without a clock (now_ns 0) refuses Stale / ColdPath on both - never fails open.
TEST(SliceServeRecord, AMissingClockRefusesAndImpliedVolIsStaleChecked) {
  ServeRecordPolicy policy;
  policy.max_age_ns = 1000;
  policy.ladders = false;
  const auto built = build_serve_record(base_fit(), policy);
  ASSERT_TRUE(built.has_value());
  const SliceServeRecord &record = **built;
  ServeRequest r = at(100.0, Side::Call, kSpot, ServeTier::Family, ServeTier::Certified);
  r.now_ns = 0;
  const ServeAnswer no_clock = record.serve(r);
  EXPECT_EQ(no_clock.status, ServeStatus::Stale);
  EXPECT_EQ(no_clock.route, ServeRoute::ColdPath);
  EXPECT_TRUE(std::isnan(no_clock.price));
  r.now_ns = record.version().as_of_ns + 10;
  const ServeAnswer priced = record.serve(r);
  ASSERT_EQ(priced.status, ServeStatus::Ok);
  ServeIvRequest iv = iv_at(100.0, Side::Call, kSpot, priced.price, ServeTier::Family,
                            ServeTier::Family);
  iv.now_ns = 0;
  const ServeIvAnswer iv_no_clock = record.implied_vol(iv);
  EXPECT_EQ(iv_no_clock.status, ServeStatus::Stale);
  EXPECT_EQ(iv_no_clock.route, ServeRoute::ColdPath);
  EXPECT_EQ(iv_no_clock.evaluations, 0u);
  iv.now_ns = record.version().as_of_ns + 5000;
  const ServeIvAnswer iv_old = record.implied_vol(iv);
  EXPECT_EQ(iv_old.status, ServeStatus::Stale);
  EXPECT_EQ(iv_old.route, ServeRoute::Rebuild);
  EXPECT_EQ(iv_old.age_ns, 5000);
  iv.now_ns = record.version().as_of_ns + 10;
  EXPECT_NE(record.implied_vol(iv).status, ServeStatus::Stale);
}

// Review-H0A m5: implied_vol with tier Cold is a route, never a solve inside the record.
TEST(SliceServeRecord, ImpliedVolWithAColdTierIsARouteNotASolve) {
  const auto record = base_record();
  ASSERT_NE(record, nullptr);
  const ServeAnswer f =
      record->serve(at(101.3, Side::Put, 0.996 * kSpot, ServeTier::Family, ServeTier::Family));
  ASSERT_EQ(f.status, ServeStatus::Ok);
  const ServeIvAnswer iv = record->implied_vol(
      iv_at(101.3, Side::Put, 0.996 * kSpot, f.price, ServeTier::Cold, ServeTier::Cold));
  EXPECT_EQ(iv.status, ServeStatus::TierUnavailable);
  EXPECT_EQ(iv.route, ServeRoute::ColdPath);
  EXPECT_EQ(iv.evaluations, 0u);
  EXPECT_TRUE(std::isnan(iv.sigma));
}

// A price-only node side serves its certified price at the fit's spot and nothing else.
TEST(SliceServeRecord, APriceOnlyNodeServesItsPriceAtTheFitSpotAndRoutesOtherwise) {
  QuoteSliceFit fit = fit_chain(0.25, 2.0e-4);
  const std::size_t n = fit.prices->nodes().size();
  const std::size_t i = n / 2;
  fit.price_only_nodes.assign(n, 0u);
  fit.price_only_nodes[i] = 2u; // the put
  Result<std::shared_ptr<const SliceServeRecord>> built = build_serve_record(fit);
  ASSERT_TRUE(built.has_value());
  const SliceServeRecord &record = **built;
  const ServedNode &node = record.nodes()[i];
  EXPECT_EQ(node.put.status, QuoteSliceEvaluationStatus::PriceOnly);
  EXPECT_EQ(node.put.tiers, 0u);
  const ServeAnswer here = record.serve(at(node.strike, Side::Put, kSpot, ServeTier::Ladder,
                                           ServeTier::Certified));
  EXPECT_EQ(here.status, ServeStatus::PriceOnly);
  EXPECT_EQ(bits(here.price), bits(node.put.price));
  EXPECT_EQ(here.certificate_class, ServeBoundClass::Exact);
  EXPECT_EQ(here.reason[static_cast<std::size_t>(ServeGreek::Delta)], ServeGreekReason::NoMap);
  EXPECT_TRUE(std::isnan(here.delta));
  const ServeAnswer moved = record.serve(at(node.strike, Side::Put, 1.01 * kSpot,
                                            ServeTier::Ladder, ServeTier::Certified));
  EXPECT_EQ(moved.status, ServeStatus::PriceOnly);
  EXPECT_EQ(moved.route, ServeRoute::Refit);
  EXPECT_TRUE(std::isnan(moved.price));
}

// Unsupported risk is never a zero: a Greek the answering tier's map has no number for is
// NaN with a typed reason (the ladder has no rho / phi; a refusal has no map at all).
TEST(SliceServeRecord, UnsupportedRiskIsNaNWithATypedReasonNeverZero) {
  const auto record = base_record();
  ASSERT_NE(record, nullptr);
  std::size_t ladders = 0;
  for (const ServedNode &node : record->nodes()) {
    if (!(node.call.tiers & kServeNodeLadder)) continue;
    ServeRequest r = at(node.strike, Side::Call, 1.001 * kSpot, ServeTier::Ladder,
                        ServeTier::Ladder);
    r.greeks = ServeGreekMask::all();
    const ServeAnswer a = record->serve(r);
    if (a.status != ServeStatus::Ok) continue;
    EXPECT_TRUE(std::isnan(a.rho) && std::isnan(a.phi));
    EXPECT_EQ(a.reason[static_cast<std::size_t>(ServeGreek::Rho)],
              ServeGreekReason::TierHasNoGreek);
    EXPECT_EQ(a.reason[static_cast<std::size_t>(ServeGreek::Phi)],
              ServeGreekReason::TierHasNoGreek);
    ++ladders;
  }
  EXPECT_GT(ladders, 5u);
  ServeRequest out = at(50.0, Side::Call, kSpot, ServeTier::Ladder, ServeTier::Certified);
  out.greeks = ServeGreekMask::all();
  const ServeAnswer refused = record->serve(out);
  for (std::size_t k = 0; k < kServeGreekCount; ++k) {
    EXPECT_EQ(refused.reason[k], ServeGreekReason::NoMap) << k;
  }
  EXPECT_TRUE(std::isnan(refused.delta) && std::isnan(refused.vega));
}

// Supplied families whose box misses the contract refuse it on the family tier: families
// of the same expiry and carry fitted 0.3 vol higher, boxed tightly around their sigmas.
TEST(SliceServeRecord, AVolatilityOutsideTheFamilyBoxRoutesToTheColdPath) {
  ServeRecordPolicy narrow;
  narrow.sigma_lo_mult = 0.999;
  narrow.sigma_hi_mult = 1.001;
  const QuoteSliceFit high = fit_chain(0.25, 2.0e-4, 70.0, 2.5, 25, 0.3);
  ASSERT_EQ(high.fit_status, QuoteSliceFitStatus::Success);
  const auto families = detail::build_serve_families(high, narrow);
  ASSERT_NE(families, nullptr);
  ServeRecordPolicy policy;
  policy.ladders = false;
  const auto built = detail::build_serve_record_with(base_fit(), policy, families);
  ASSERT_TRUE(built.has_value());
  std::size_t refused = 0;
  for (const ServedNode &node : (*built)->nodes()) {
    const ServeAnswer a = (*built)->serve(at(node.strike, Side::Put, 1.003 * kSpot,
                                             ServeTier::Family, ServeTier::Family));
    if (a.status == ServeStatus::SigmaOutOfBox) {
      EXPECT_EQ(a.route, ServeRoute::ColdPath);
      ++refused;
    }
  }
  EXPECT_GT(refused, 5u);
}

// IV at tick rate: the ladder tier inverts its own map (accepted only on a validated
// statement within iv_tolerance); the family tier its fused map; the certified tier
// confirms the family root with one Fast solve. Ladder roots under the default policy
// (12 x 6 nodes, +/- 25 % sigma) carry a statement / slope of ~1e-7 to 1e-5 and are
// Unconfirmed; the tick profile (serve_record_policy_tick: 16 x 8, +/- 10 %, +/- 2 % ln S)
// meets iv_tolerance.
TEST(SliceServeRecord, ImpliedVolInvertsEachTiersOwnMap) {
  const auto record = base_record();
  ASSERT_NE(record, nullptr);
  const QuoteSliceFit &fit = base_fit();
  const auto tick = build_serve_record(fit, serve_record_policy_tick());
  ASSERT_TRUE(tick.has_value());
  std::size_t default_unconfirmed = 0;
  for (const SliceServeRecord *r : {record.get(), tick->get()}) {
    std::size_t ladder = 0;
    std::size_t unconfirmed = 0;
    for (const ServedNode &node : r->nodes()) {
      const ServedNodeSide &s = node.call;
      if (!(s.tiers & kServeNodeLadder)) continue;
      const double S = 1.002 * kSpot;
      // A target the ladder's own map prices at its served sigma: the root is that sigma.
      const ServeAnswer priced =
          r->serve(at(node.strike, Side::Call, S, ServeTier::Ladder, ServeTier::Ladder));
      if (priced.status != ServeStatus::Ok) continue;
      const ServeIvAnswer iv = r->implied_vol(
          iv_at(node.strike, Side::Call, S, priced.price, ServeTier::Ladder, ServeTier::Ladder));
      if (iv.status == ServeStatus::NonMonotoneBand) continue;
      EXPECT_NEAR(iv.sigma, s.sigma, 1.0e-9) << node.strike;
      EXPECT_LE(iv.evaluations, 16u);
      if (iv.status == ServeStatus::Ok) {
        EXPECT_EQ(iv.tier, ServeTier::Ladder);
        EXPECT_EQ(s.ladder_class, ServeBoundClass::RefinementValidated);
        EXPECT_EQ(iv.acceptance.kind, ServeAcceptanceKind::LadderCertificate);
        EXPECT_EQ(iv.acceptance.error_class, ServeErrorClass::RefinementValidated);
        EXPECT_LE(iv.acceptance.bound_iv, r->policy().iv_tolerance);
        ++ladder;
      } else {
        EXPECT_EQ(iv.status, ServeStatus::Unconfirmed) << node.strike;
        EXPECT_EQ(iv.route, ServeRoute::ColdPath);
        ++unconfirmed;
      }
    }
    if (r == tick->get()) {
      EXPECT_GT(ladder, 20u) << "unconfirmed " << unconfirmed;
    } else {
      default_unconfirmed = unconfirmed;
    }
  }
  EXPECT_GT(default_unconfirmed, 0u);

  // Between nodes (no ladder): the family tier inverts the family's own price at the
  // sticky sigma back to that sigma, and a lower target to a lower root.
  const double K = 101.3;
  const double S = 0.996 * kSpot;
  const ServeAnswer f = record->serve(at(K, Side::Put, S, ServeTier::Family, ServeTier::Family));
  ASSERT_EQ(f.status, ServeStatus::Ok);
  const ServeIvAnswer self = record->implied_vol(
      iv_at(K, Side::Put, S, f.price, ServeTier::Ladder, ServeTier::Family));
  EXPECT_EQ(self.tier, ServeTier::Family);
  EXPECT_NEAR(self.sigma, f.sigma, 1.0e-9);
  ServeIvRequest fr = iv_at(K, Side::Put, S, f.price - 0.03 * f.vega * f.sigma, ServeTier::Ladder,
                            ServeTier::Family);
  const ServeIvAnswer fam = record->implied_vol(fr);
  EXPECT_EQ(fam.tier, ServeTier::Family);
  EXPECT_LE(std::abs(fam.newton_distance_vol), 1.0e-9);
  EXPECT_NEAR(fam.sigma, 0.97 * f.sigma, 2.0e-3);
  // The certified tier confirms that root with one Fast solve (K3's NewtonContract).
  fr.tier = ServeTier::Certified;
  fr.tier_max = ServeTier::Certified;
  const ServeIvAnswer cert = record->implied_vol(fr);
  ASSERT_EQ(cert.status, ServeStatus::Ok);
  EXPECT_EQ(cert.tier, ServeTier::Certified);
  EXPECT_EQ(cert.acceptance.kind, ServeAcceptanceKind::Confirmed);
  EXPECT_EQ(cert.acceptance.reason, ServeAcceptanceReason::NewtonContract);
  EXPECT_LE(std::abs(cert.newton_distance_vol), record->policy().iv_tolerance);
  // ... and the confirmed root is the accurate kernel's own root to the requirement.
  const double qK = carry_between(fit, K); // T_cal == T_var: the calendar yield itself
  const Result<double> at_root = andersen_lake(S, K, fit.market.variance_years, cert.sigma,
                                               fit.diagnostics.effective_rate, qK, Side::Put,
                                               fit.al_opts);
  ASSERT_TRUE(at_root.has_value());
  EXPECT_LE(std::abs(*at_root - fr.target_price) / f.vega, 1.0e-7);
}

// The L1c gate runs in front of every inversion: a deep in-the-money target within the
// non-monotone band is routed to the accurate cold inversion on every tier.
TEST(SliceServeRecord, ImpliedVolRoutesANonMonotoneBandTargetToTheAccurateInversion) {
  const auto record = base_record();
  ASSERT_NE(record, nullptr);
  const ServedNode &deep = record->nodes().back(); // K 130 put: deep in the money
  const double intrinsic = deep.strike - kSpot;
  for (const ServeTier tier : {ServeTier::Ladder, ServeTier::Family, ServeTier::Certified}) {
    const ServeIvAnswer iv = record->implied_vol(iv_at(deep.strike, Side::Put, kSpot,
                                                       intrinsic + 1.0e-9 * deep.strike, tier,
                                                       ServeTier::Certified));
    EXPECT_EQ(iv.status, ServeStatus::NonMonotoneBand) << to_string(tier);
    EXPECT_EQ(iv.route, ServeRoute::ColdPath);
    EXPECT_EQ(iv.evaluations, 0u);
  }
}

// Review-H0A m9: the record's band decision (its third replica of the zero-volatility
// value) refuses every target american_implied_vol refuses as NonMonotoneRegion, refuses
// only inside the band (at or below american's own band top, iv_premium_edge_band_top),
// and never refuses a target above it. Targets: the accurate kernel's prices at small
// volatilities (the plateau and the band) and at the node's served sigma.
TEST(SliceServeRecord, TheL1cBandDecisionIsAmericanIvsOwn) {
  const auto record = base_record();
  ASSERT_NE(record, nullptr);
  const QuoteSliceFit &fit = base_fit();
  std::size_t american_refused = 0;
  std::size_t record_refused = 0;
  std::size_t above = 0;
  for (const ServedNode &node : record->nodes()) {
    for (const Side side : {Side::Call, Side::Put}) {
      const ServedNodeSide &s = side_of(node, side);
      const double K = node.strike;
      const double T = record->tau();
      const double r = record->rate();
      const double top = iv_premium_edge_band_top(kSpot, K, T, r, s.q, side, fit.al_opts);
      ASSERT_TRUE(std::isfinite(top)) << K;
      for (const double sigma : {0.006, 0.01, 0.03, 0.1, s.sigma}) {
        const Result<double> target = andersen_lake(kSpot, K, T, sigma, r, s.q, side,
                                                    fit.al_opts);
        ASSERT_TRUE(target.has_value()) << K << " " << sigma;
        IvOpts opts;
        opts.al_opts = fit.al_opts;
        opts.refuse_non_monotone = true;
        const IvResult cold = american_implied_vol(*target, kSpot, K, T, r, s.q, side, opts);
        const ServeIvAnswer iv = record->implied_vol(
            iv_at(K, side, kSpot, *target, ServeTier::Family, ServeTier::Family));
        if (cold.status == IvStatus::NonMonotoneRegion) {
          EXPECT_EQ(iv.status, ServeStatus::NonMonotoneBand) << K << " " << sigma;
          ++american_refused;
        }
        if (iv.status == ServeStatus::NonMonotoneBand) {
          EXPECT_LE(*target, top) << K << " " << sigma;
          EXPECT_EQ(iv.route, ServeRoute::ColdPath);
          ++record_refused;
        }
        if (*target > top) {
          EXPECT_NE(iv.status, ServeStatus::NonMonotoneBand) << K << " " << sigma;
          ++above;
        }
      }
    }
  }
  EXPECT_GT(american_refused, 0u);
  EXPECT_GE(record_refused, american_refused);
  EXPECT_GT(above, 0u);
}

TEST(SliceServeRecord, ConcurrentServesAreBitIdenticalToTheSerialLoop) {
  const auto record = base_record();
  ASSERT_NE(record, nullptr);
  const auto run = [&record](std::vector<double> &out) {
    for (const ServedNode &node : record->nodes()) {
      for (const Side side : {Side::Call, Side::Put}) {
        for (const ServeTier tier : {ServeTier::Ladder, ServeTier::Family}) {
          const ServeAnswer a =
              record->serve(at(node.strike, side, 1.0031 * kSpot, tier, tier));
          out.push_back(a.price);
          out.push_back(a.delta);
          out.push_back(a.vega);
          out.push_back(a.certificate_usd);
          const ServeIvAnswer iv = record->implied_vol(
              iv_at(node.strike, side, 1.0031 * kSpot, a.price, tier, tier));
          out.push_back(iv.sigma);
        }
      }
    }
  };
  std::vector<double> serial;
  run(serial);
  constexpr int kThreads = 4;
  std::vector<std::vector<double>> parallel(kThreads);
  {
    std::vector<std::jthread> pool;
    for (int t = 0; t < kThreads; ++t) pool.emplace_back([&, t] { run(parallel[t]); });
  }
  for (const std::vector<double> &p : parallel) {
    ASSERT_EQ(p.size(), serial.size());
    for (std::size_t i = 0; i < p.size(); ++i) EXPECT_EQ(bits(p[i]), bits(serial[i])) << i;
  }
}

TEST(SliceServeRecord, VersionDigestsAreDeterministicAndKeyedOnTheirInputs) {
  const QuoteSliceFit &fit = base_fit();
  const auto a = build_serve_record(fit);
  const auto b = build_serve_record(fit);
  ASSERT_TRUE(a.has_value() && b.has_value());
  EXPECT_EQ((*a)->version().fit_sha256, (*b)->version().fit_sha256);
  EXPECT_EQ((*a)->version().policy_sha256, (*b)->version().policy_sha256);
  EXPECT_EQ((*a)->version().input_sha256, (*b)->version().input_sha256);
  ServeRecordPolicy other;
  other.ladder_spot_nodes = 10;
  const auto c = build_serve_record(fit, other);
  ASSERT_TRUE(c.has_value());
  EXPECT_NE((*a)->version().policy_sha256, (*c)->version().policy_sha256);
  EXPECT_EQ((*a)->version().fit_sha256, (*c)->version().fit_sha256);
  QuoteSliceFit changed = fit_chain(0.25, 2.0e-4);
  const auto row = std::find_if(changed.rows.begin(), changed.rows.end(),
                                [](const QuoteSliceRow &r) { return std::isfinite(r.market_iv); });
  ASSERT_NE(row, changed.rows.end());
  row->market_iv += 1.0e-3; // a quote-level input
  const auto d = build_serve_record(changed);
  ASSERT_TRUE(d.has_value());
  EXPECT_NE((*a)->version().input_sha256, (*d)->version().input_sha256);
  EXPECT_EQ((*a)->version().fit_sha256, (*d)->version().fit_sha256);
  changed.moved_nodes.assign(changed.prices->nodes().size(), 0u);
  changed.moved_nodes[3] = 1u; // a served-state flag
  const auto e = build_serve_record(changed);
  ASSERT_TRUE(e.has_value());
  EXPECT_NE((*a)->version().fit_sha256, (*e)->version().fit_sha256);
}

// Review-H0A m4: bytes count every geometry the holder owns (three, each allocated by
// construction) besides the families' state.
TEST(SliceServeRecord, BytesAndClocksAreReported) {
  const auto record = base_record();
  ASSERT_NE(record, nullptr);
  const ServeRecordBytes &b = record->bytes();
  EXPECT_EQ(b.nodes, record->nodes().size() * sizeof(ServedNode));
  EXPECT_GT(b.ladders, 0u);
  EXPECT_GT(b.families, 0u);
  EXPECT_EQ(b.geometries, 3u * sizeof(AlExpiryGeometry::Impl));
  EXPECT_EQ(b.total(), b.nodes + b.ladders + b.families + b.geometries + b.fixed);
  const ServeBuildClocks &c = record->clocks();
  EXPECT_EQ(c.lazy_family_builds, 2u);
  EXPECT_GT(c.family_evaluations, 0u);
  EXPECT_GT(c.total_us, 0.0);
  EXPECT_GE(c.total_us, c.family_us);
  EXPECT_TRUE(record->support().tiers[0] && record->support().tiers[1] &&
              record->support().tiers[2] && record->support().tiers[3]);
}

// Review-H0A m14: the seams this build carries are visible in the support block, not only
// through policy_sha256.
TEST(SliceServeRecord, SeamFlagsAreVisibleInTheSupport) {
  const auto record = base_record();
  ASSERT_NE(record, nullptr);
  EXPECT_EQ(record->support().a0_bundle, ATX_VOL_SERVE_RECORD_A0 != 0);
  EXPECT_EQ(record->support().k3_envelope, ATX_VOL_SERVE_RECORD_K3 != 0);
}

// Review-H0A m8: no record exists without the builder (the constructor takes a key only
// the builder can make).
TEST(SliceServeRecord, OnlyTheBuilderMakesARecord) {
  static_assert(!std::is_constructible_v<SliceServeRecord,
                                         std::unique_ptr<const SliceServeRecord::Impl>>);
  static_assert(!std::is_default_constructible_v<SliceServeRecord::Key>);
  EXPECT_NE(base_record(), nullptr);
}

TEST(SliceServeRecord, AnInvalidPolicyIsRefused) {
  ServeRecordPolicy p;
  p.ladder_spot_nodes = 2;
  EXPECT_FALSE(validate_serve_record_policy(p).has_value());
  p = ServeRecordPolicy{};
  p.sigma_lo_mult = 2.0;
  EXPECT_FALSE(validate_serve_record_policy(p).has_value());
  p = ServeRecordPolicy{};
  p.iv_tolerance = 0.0;
  EXPECT_FALSE(build_serve_record(base_fit(), p).has_value());
  EXPECT_TRUE(validate_serve_record_policy(ServeRecordPolicy{}).has_value());
}

TEST(SliceServeRecord, AFitWithoutACertifiedSliceIsUnavailable) {
  QuoteSliceFit empty;
  const auto built = build_serve_record(empty);
  ASSERT_FALSE(built.has_value());
  EXPECT_EQ(built.error().code(), ErrorCode::Unavailable);
}

// Deliverable 7: the Ladder and Family reads (serve and implied_vol) allocate nothing.
TEST(SliceServeRecordAlloc, LadderAndFamilyReadsAllocateNothing) {
  const auto record = base_record();
  ASSERT_NE(record, nullptr);
  double sum = 0.0;
  std::size_t served = 0;
  const std::uint64_t n = testkit::count_allocations([&] {
    for (const ServedNode &node : record->nodes()) {
      for (const Side side : {Side::Call, Side::Put}) {
        for (const ServeTier tier : {ServeTier::Ladder, ServeTier::Family}) {
          ServeRequest r = at(node.strike, side, 1.002 * kSpot, tier, tier);
          r.greeks = ServeGreekMask::all();
          const ServeAnswer a = record->serve(r);
          const ServeIvAnswer iv = record->implied_vol(
              iv_at(node.strike, side, 1.002 * kSpot, a.price, tier, tier));
          if (a.status == ServeStatus::Ok && std::isfinite(iv.sigma)) {
            sum += a.price + iv.sigma;
            ++served;
          }
        }
        const ServeAnswer between = record->serve(at(node.strike + 0.7, side, kSpot,
                                                     ServeTier::Family, ServeTier::Family));
        if (std::isfinite(between.price)) sum += between.price;
      }
    }
  });
  EXPECT_EQ(n, 0u);
  EXPECT_GT(served, 60u);
  EXPECT_TRUE(std::isfinite(sum));
}

// ── Lane H1 phase B: the fitter's hand-off ─────────────────────────────────────────────

// The native fit route's family configuration (lane K4's fit_native_route_test.cpp).
QuoteSliceConfig family_q_config() {
  QuoteSliceConfig config;
  config.dividend_model = QuoteSliceDividendModel::ContinuousYield;
  config.channel.repair_half_spreads = 0.5;
  config.channel.fit_space = QuoteSliceFitSpace::VolChannel;
  config.channel.deam_profile = QuoteSliceDeamProfile::Screen;
  config.channel.deam_cache = true;
  config.channel.deam_inversion = QuoteSliceDeamInversion::Premium;
  config.channel.deam_confirm_kernel = QuoteSliceDeamConfirmKernel::Fast;
  config.channel.deam_family = QuoteSliceDeamFamily::Always;
  config.channel.deam_family_q = true;
  config.channel.deam_family_native = true;
  config.grid_kernel = QuoteSliceGridKernel::Family;
  return config;
}

// The hook (end of certify_and_serve) on its own: no families or no certified fit - no
// hand-off (an earlier one is cleared); a holder's families - each present side's recipe,
// its envelope and two of its own prices.
TEST(SliceServeRecordHandoff, TheHookStampsTheFamiliesOfACertifiedFitOnly) {
  QuoteSliceFit fit = fit_chain(0.25, 2.0e-4);
  const auto families = detail::build_serve_families(fit, ServeRecordPolicy{});
  ASSERT_NE(families, nullptr);
  detail::stamp_serve_handoff(fit, families.get());
  ASSERT_NE(fit.serve_handoff, nullptr);
  for (const Side side : {Side::Put, Side::Call}) {
    const QuoteSliceServeHandoff::Family &h = fit.serve_handoff->sides[side == Side::Put ? 0 : 1];
    const AlSigmaQFamily *f = families->of_q(side);
    ASSERT_NE(f, nullptr);
    ASSERT_TRUE(h.present);
    EXPECT_EQ(bits(h.sigma_lo), bits(f->sigma_lo()));
    EXPECT_EQ(h.n_q, f->n_q());
    EXPECT_EQ(h.envelope.family, nullptr);
    EXPECT_EQ(h.envelope.audit.probes, families->q_envelope(side)->audit.probes);
    for (const QuoteSliceServeHandoff::Probe &p : h.probes) {
      EXPECT_EQ(bits(p.price), bits(f->price(h.anchor, p.K, p.sigma, p.q).price));
    }
  }
  detail::stamp_serve_handoff(fit, nullptr);
  EXPECT_EQ(fit.serve_handoff, nullptr);
  detail::stamp_serve_handoff(fit, families.get());
  fit.fit_status = QuoteSliceFitStatus::ArbitrageFailure;
  detail::stamp_serve_handoff(fit, families.get());
  EXPECT_EQ(fit.serve_handoff, nullptr);
}

// certify_and_serve hands the fitter's families to the record builder: the record
// re-prepares them from the hand-off (off the fit), keeps a side only when it reproduces
// the fitter's own family (the fitter's holder is still in this thread's de-Am memo here,
// so the test compares prices bit for bit), and reads the fitter's audited envelope.
TEST(SliceServeRecordHandoff, TheRecordServesTheFittersOwnFamilies) {
  const Chain chain = smile_chain(0.25, 2.0e-4);
  const QuoteSliceConfig config = family_q_config();
  deam_iv_memo_clear();
  const Result<QuoteSliceFit> fit = fit_quote_slice_channel(chain.market, chain.quotes, config,
                                                            nullptr);
  ASSERT_TRUE(fit.has_value());
  ASSERT_EQ(fit->fit_status, QuoteSliceFitStatus::Success) << fit->fit_error;
  ASSERT_NE(fit->serve_handoff, nullptr);
  bool reused = false;
  const auto cache =
      deam_iv_memo(deam_iv_key(*fit, chain.quotes, config), chain.quotes.size(), reused);
  ASSERT_TRUE(reused);
  ASSERT_NE(cache->families, nullptr);
  const DeAmSigmaFamilies &fitter = *cache->families;
  ServeRecordPolicy policy;
  policy.ladders = false;
  const auto built = build_serve_record(*fit, policy);
  ASSERT_TRUE(built.has_value());
  const SliceServeRecord &record = **built;
  std::size_t compared = 0;
  std::size_t handed = 0;
  for (const Side side : {Side::Put, Side::Call}) {
    const AlSigmaQFamily *f = fitter.of_q(side);
    const ServeSideSupport &s = support_of(record, side);
    if (f == nullptr) {
      EXPECT_NE(s.origin, ServeFamilyOrigin::Handed);
      continue;
    }
    ++handed;
    ASSERT_TRUE(s.family) << to_string(s.reason);
    EXPECT_EQ(s.origin, ServeFamilyOrigin::Handed);
    EXPECT_EQ(bits(s.sigma_lo), bits(f->sigma_lo()));
    EXPECT_EQ(bits(s.q_hi), bits(f->q_hi()));
    const ValidatedErrorEnvelope *env = fitter.q_envelope(side);
    ASSERT_NE(env, nullptr);
    EXPECT_EQ(s.family_class, env->covered() ? ServeErrorClass::RefinementValidated
                                             : ServeErrorClass::Uncovered);
    EXPECT_EQ(s.audit_probes, env->audit.probes);
    EXPECT_EQ(bits(s.domain_bound_usd), bits(fitter.q_bound(side)));
    // The record's family tier at a moved spot IS the fitter's family, to the bit.
    for (const ServedNode &node : record.nodes()) {
      const ServedNodeSide &ns = side_of(node, side);
      if (!(ns.tiers & kServeNodeFamily)) continue;
      const double S = 1.004 * kSpot;
      const ServeAnswer a = record.serve(at(node.strike, side, S, ServeTier::Family,
                                            ServeTier::Family));
      const auto pv = sigma_q_family_detail::price_and_vega(*f, S, node.strike, ns.sigma, ns.q);
      if (pv.status != sigma_q_family_detail::FusedStatus::Ok || !answered(a.status)) continue;
      EXPECT_EQ(bits(a.price), bits(pv.price)) << node.strike;
      ++compared;
    }
  }
  EXPECT_GT(handed, 0u);
  EXPECT_GT(compared, 10u);
  EXPECT_EQ(record.clocks().lazy_family_builds, handed);
}

// A hand-off the rebuilt family does not reproduce (a probe price one ulp off) refuses
// that side with HandoffMismatch: a record never serves a family it cannot tie to the fit.
TEST(SliceServeRecordHandoff, AHandoffTheRebuildDoesNotReproduceIsRefused) {
  const Chain chain = smile_chain(0.25, 2.0e-4);
  deam_iv_memo_clear();
  Result<QuoteSliceFit> fit = fit_quote_slice_channel(chain.market, chain.quotes,
                                                      family_q_config(), nullptr);
  ASSERT_TRUE(fit.has_value());
  ASSERT_NE(fit->serve_handoff, nullptr);
  auto forged = std::make_shared<QuoteSliceServeHandoff>(*fit->serve_handoff);
  std::size_t forged_sides = 0;
  for (QuoteSliceServeHandoff::Family &side : forged->sides) {
    if (!side.present) continue;
    side.probes[0].price = std::nextafter(side.probes[0].price, kInf);
    ++forged_sides;
  }
  ASSERT_GT(forged_sides, 0u);
  fit->serve_handoff = std::move(forged);
  ServeRecordPolicy policy;
  policy.ladders = false;
  const auto built = build_serve_record(*fit, policy);
  ASSERT_TRUE(built.has_value());
  std::size_t refused = 0;
  for (std::size_t i = 0; i < 2; ++i) {
    if (!fit->serve_handoff->sides[i].present) continue;
    const ServeSideSupport &s = i == 0 ? (*built)->support().put : (*built)->support().call;
    EXPECT_FALSE(s.family);
    EXPECT_EQ(s.reason, ServeUnsupportedReason::HandoffMismatch);
    ++refused;
  }
  EXPECT_EQ(refused, forged_sides);
}

// ── Lane H1: the evaluate_quote_slice fast path ───────────────────────────────────────

// With a record attached, a between-node query returns the SAME certified price and the
// record family's root of it where K3's enclosure accepts it - within the record's
// iv_tolerance (plus the cold inversion's own tolerance) of the cold accurate inversion's
// volatility; at an unmoved node and wherever the enclosure does not accept, exactly the
// answer without a record.
TEST(SliceServeRecordFastPath, BetweenNodeVolatilitiesComeFromTheFamilyWithinTheContract) {
  QuoteSliceFit with = fit_chain(0.25, 2.0e-4);
  const QuoteSliceFit &without = base_fit();
  const auto built = build_serve_record(with);
  ASSERT_TRUE(built.has_value());
  with.serve_record = *built;
  std::size_t fast = 0;
  std::size_t same = 0;
  const auto nodes = without.prices->nodes();
  for (const Side side : {Side::Call, Side::Put}) {
    for (std::size_t i = 0; i < nodes.size(); ++i) {
      for (const double t : {0.0, 0.3, 0.5}) {
        if (i + 1 == nodes.size() && t > 0.0) continue;
        const double K = t == 0.0 ? nodes[i].strike
                                  : std::lerp(nodes[i].strike, nodes[i + 1].strike, t);
        const Result<QuoteSliceValue> off = evaluate_quote_slice(without, K, side);
        const Result<QuoteSliceValue> on = evaluate_quote_slice(with, K, side);
        ASSERT_EQ(off.has_value(), on.has_value()) << K;
        if (!off) continue;
        EXPECT_EQ(bits(on->price), bits(off->price)) << K;
        const std::optional<QuoteSliceValue> direct = detail::serve_record_value(with, K, side);
        if (direct) {
          EXPECT_EQ(bits(direct->volatility), bits(on->volatility)) << K;
          EXPECT_LE(std::abs(on->volatility - off->volatility), 2.0e-7) << K;
          ++fast;
        } else {
          EXPECT_EQ(bits(on->volatility), bits(off->volatility)) << K;
          ++same;
        }
        if (t == 0.0) EXPECT_FALSE(direct.has_value()) << "an unmoved node: the curve's sigma";
      }
    }
  }
  EXPECT_GT(fast, 10u);
  EXPECT_GT(same, 10u);
}

// A record that is not this fit's (another fit's prices) never answers: the query is the
// fit's own, bit for bit.
TEST(SliceServeRecordFastPath, ARecordOfAnotherFitIsNeverUsed) {
  QuoteSliceFit with = fit_chain(0.25, 2.0e-4, 70.0, 2.5, 25, 0.01);
  const auto built = build_serve_record(base_fit());
  ASSERT_TRUE(built.has_value());
  with.serve_record = *built; // the 0.22 smile's record on the 0.23 smile's fit
  for (const double K : {83.7, 101.3, 112.2}) {
    EXPECT_FALSE(detail::serve_record_value(with, K, Side::Put).has_value()) << K;
  }
}

} // namespace
} // namespace atx::vol
