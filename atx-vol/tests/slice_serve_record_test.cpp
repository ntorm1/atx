// Lane H0-A (r6, 2026-10-23): the immutable fitted serving record of one expiry -
// build from a completed QuoteSliceFit, ownership, the typed node answers, tiered serve,
// the tick-rate implied volatility, routing, versions, bytes, concurrency and the
// allocation budget of the Ladder and Family reads.
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
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "atx/vol/api/fitting/quote_slice.hpp"
#include "atx/vol/api/pricing/american.hpp"
#include "atx/vol/api/pricing/american_iv.hpp"
#include "fitting/slice_serve_record_detail.hpp"
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

TEST(SliceServeRecord, FixtureFitIsServedWithPerStrikeCarry) {
  const QuoteSliceFit &fit = base_fit();
  ASSERT_EQ(fit.fit_status, QuoteSliceFitStatus::Success);
  ASSERT_TRUE(fit.prices.has_value());
  ASSERT_GE(fit.prices->nodes().size(), 20u);
  ASSERT_FALSE(fit.carry_curve.empty());
  ASSERT_NE(base_record(), nullptr);
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
TEST(SliceServeRecord, NodeYieldReproducesTheCertifiedPriceToTheBit) {
  const auto record = base_record();
  ASSERT_NE(record, nullptr);
  const QuoteSliceFit &fit = base_fit();
  std::size_t checked = 0;
  for (const ServedNode &node : record->nodes()) {
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
  EXPECT_GE(checked, 40u);
}

// Ruling b: at the fit's own spot every tier serves the certified node price; the tier's
// Greeks come from its own map and the answer declares the gap.
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
        EXPECT_EQ(a.tier, tier);
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
    const ServeSideSupport &support =
        side == Side::Call ? record->support().call : record->support().put;
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
      EXPECT_EQ(a.reason[static_cast<std::size_t>(ServeGreek::Vega)], ServeGreekReason::Served);
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
}

// Tier 0: price and four Greeks from one map - the ladder's price within its certificate
// of the family, and its delta / gamma / vega the derivatives of that same map.
TEST(SliceServeRecord, LadderServesPriceAndFourGreeksFromOneCertifiedMap) {
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
// off it (diagnostic in the lane report); a 5 % sigma window lets ladders reach the band.
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

// The family tier claims its probed bound only inside the probed band |ln(K / S)| <= 0.25
// (and at a node at least its measured error there); a ladder's certificate carries the
// node's measured family error.
TEST(SliceServeRecord, FamilyCertificateClaimsNothingOutsideTheProbedBand) {
  const auto record = base_record();
  ASSERT_NE(record, nullptr);
  const double S = 1.003 * kSpot;
  const ServeAnswer inside =
      record->serve(at(101.3, Side::Put, S, ServeTier::Family, ServeTier::Family));
  ASSERT_TRUE(answered(inside.status));
  EXPECT_TRUE(std::isfinite(inside.certificate_usd));
  const ServeAnswer outside =
      record->serve(at(71.3, Side::Put, S, ServeTier::Family, ServeTier::Family));
  ASSERT_TRUE(answered(outside.status));
  EXPECT_TRUE(std::isnan(outside.certificate_usd));
  std::size_t ladders = 0;
  for (const ServedNode &node : record->nodes()) {
    for (const Side side : {Side::Call, Side::Put}) {
      const ServedNodeSide &s = side_of(node, side);
      if (!(s.tiers & kServeNodeLadder)) continue;
      EXPECT_GE(s.ladder_certificate_usd, std::abs(s.family_price_delta)) << node.strike;
      const ServeAnswer at_node =
          record->serve(at(node.strike, side, S, ServeTier::Family, ServeTier::Family));
      if (answered(at_node.status)) {
        EXPECT_GE(at_node.certificate_usd, std::abs(s.family_price_delta)) << node.strike;
      }
      ++ladders;
    }
  }
  EXPECT_GT(ladders, 10u);
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
// Lane K3 seam: where the side family's validated envelope covers it, a family root is
// accepted by the envelope's enclosure (no confirm solve) within the 1e-7 requirement of
// the accurate kernel's root.
TEST(SliceServeRecord, FamilyRootsAreAcceptedByTheEnvelopesEnclosure) {
  const auto record = base_record();
  ASSERT_NE(record, nullptr);
  const QuoteSliceFit &fit = base_fit();
  const double S = 0.998 * kSpot;
  std::size_t skipped = 0;
  for (const ServedNode &node : record->nodes()) {
    const ServedNodeSide &s = node.put;
    if (!(s.tiers & kServeNodeFamily) || s.family_class != ServeErrorClass::RefinementValidated) {
      continue;
    }
    const ServeAnswer f =
        record->serve(at(node.strike, Side::Put, S, ServeTier::Family, ServeTier::Family));
    if (f.status != ServeStatus::Ok) continue;
    const double target = f.price * (1.0 + 1.0e-3);
    const ServeIvAnswer iv = record->implied_vol(
        ServeIvRequest{node.strike, Side::Put, S, target, ServeTier::Family, ServeTier::Family});
    if (iv.status != ServeStatus::Ok) continue;
    EXPECT_EQ(iv.acceptance.kind, ServeAcceptanceKind::Skipped);
    EXPECT_EQ(iv.acceptance.reason, ServeAcceptanceReason::FamilyEnclosure);
    EXPECT_LE(iv.acceptance.bound_iv, 1.0e-7);
    const Result<double> lo = andersen_lake(S, node.strike, fit.market.variance_years,
                                            iv.sigma - 1.0e-7, fit.diagnostics.effective_rate,
                                            s.q, Side::Put, fit.al_opts);
    const Result<double> hi = andersen_lake(S, node.strike, fit.market.variance_years,
                                            iv.sigma + 1.0e-7, fit.diagnostics.effective_rate,
                                            s.q, Side::Put, fit.al_opts);
    ASSERT_TRUE(lo.has_value() && hi.has_value());
    EXPECT_LE(*lo, target) << node.strike; // the accurate root lies within 1e-7
    EXPECT_GE(*hi, target) << node.strike;
    ++skipped;
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

TEST(SliceServeRecord, AStrikeOutsideTheCertifiedDomainRoutesToARefit) {
  const auto record = base_record();
  ASSERT_NE(record, nullptr);
  const ServeAnswer a = record->serve(at(50.0, Side::Put, kSpot, ServeTier::Ladder,
                                         ServeTier::Certified));
  EXPECT_EQ(a.status, ServeStatus::OutOfDomain);
  EXPECT_EQ(a.route, ServeRoute::Refit);
  EXPECT_TRUE(std::isnan(a.price));
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
  EXPECT_EQ(here.reason[static_cast<std::size_t>(ServeGreek::Delta)], ServeGreekReason::NoMap);
  const ServeAnswer moved = record.serve(at(node.strike, Side::Put, 1.01 * kSpot,
                                            ServeTier::Ladder, ServeTier::Certified));
  EXPECT_EQ(moved.status, ServeStatus::PriceOnly);
  EXPECT_EQ(moved.route, ServeRoute::Refit);
  EXPECT_TRUE(std::isnan(moved.price));
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

// IV at tick rate: the ladder tier inverts its own map; the family tier its fused map; the
// certified tier confirms the family root with one Fast solve.
TEST(SliceServeRecord, ImpliedVolInvertsEachTiersOwnMap) {
  const auto record = base_record();
  ASSERT_NE(record, nullptr);
  const QuoteSliceFit &fit = base_fit();
  const auto families = detail::build_serve_families(fit, ServeRecordPolicy{});
  ASSERT_NE(families, nullptr);
  std::size_t ladder = 0;
  for (const ServedNode &node : record->nodes()) {
    const ServedNodeSide &s = node.call;
    if (!(s.tiers & kServeNodeLadder)) continue;
    const double S = 1.002 * kSpot;
    // A target the ladder's own map prices at its served sigma: the root is that sigma.
    const ServeAnswer priced =
        record->serve(at(node.strike, Side::Call, S, ServeTier::Ladder, ServeTier::Ladder));
    if (priced.status != ServeStatus::Ok) continue;
    ServeIvRequest r{node.strike, Side::Call, S, priced.price, ServeTier::Ladder,
                     ServeTier::Ladder};
    const ServeIvAnswer iv = record->implied_vol(r);
    if (iv.status == ServeStatus::NonMonotoneBand) continue;
    ASSERT_EQ(iv.status, ServeStatus::Ok) << node.strike;
    EXPECT_EQ(iv.tier, ServeTier::Ladder);
    EXPECT_NEAR(iv.sigma, s.sigma, 1.0e-9) << node.strike;
    EXPECT_EQ(iv.acceptance.kind, ServeAcceptanceKind::LadderCertificate);
    EXPECT_LE(iv.evaluations, 16u);
    ++ladder;
  }
  EXPECT_GT(ladder, 5u);

  // Between nodes (no ladder): the family tier inverts the family's own price at the
  // sticky sigma back to that sigma, and a lower target to a lower root.
  const double K = 101.3;
  const double S = 0.996 * kSpot;
  const ServeAnswer f = record->serve(at(K, Side::Put, S, ServeTier::Family, ServeTier::Family));
  ASSERT_EQ(f.status, ServeStatus::Ok);
  const ServeIvAnswer self = record->implied_vol(
      ServeIvRequest{K, Side::Put, S, f.price, ServeTier::Ladder, ServeTier::Family});
  EXPECT_EQ(self.tier, ServeTier::Family);
  EXPECT_NEAR(self.sigma, f.sigma, 1.0e-9);
  ServeIvRequest fr{K, Side::Put, S, f.price - 0.03 * f.vega * f.sigma, ServeTier::Ladder,
                    ServeTier::Family};
  const ServeIvAnswer fam = record->implied_vol(fr);
  EXPECT_EQ(fam.tier, ServeTier::Family);
#if !ATX_VOL_SERVE_RECORD_K3
  EXPECT_EQ(fam.status, ServeStatus::Unconfirmed); // no validated envelope in this build
  EXPECT_EQ(fam.route, ServeRoute::ColdPath);
  EXPECT_EQ(fam.acceptance.kind, ServeAcceptanceKind::None);
  EXPECT_EQ(fam.acceptance.error_class, ServeErrorClass::Uncovered);
#endif
  EXPECT_LE(std::abs(fam.newton_distance_vol), 1.0e-9);
  EXPECT_NEAR(fam.sigma, 0.97 * f.sigma, 2.0e-3);
  // The certified tier confirms that root with one Fast solve (K3's NewtonContract).
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
    const ServeIvAnswer iv = record->implied_vol(
        ServeIvRequest{deep.strike, Side::Put, kSpot, intrinsic + 1.0e-9 * deep.strike, tier,
                       ServeTier::Certified});
    EXPECT_EQ(iv.status, ServeStatus::NonMonotoneBand) << to_string(tier);
    EXPECT_EQ(iv.route, ServeRoute::ColdPath);
    EXPECT_EQ(iv.evaluations, 0u);
  }
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
          const ServeIvAnswer iv = record->implied_vol(
              ServeIvRequest{node.strike, side, 1.0031 * kSpot, a.price, tier, tier});
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

TEST(SliceServeRecord, BytesAndClocksAreReported) {
  const auto record = base_record();
  ASSERT_NE(record, nullptr);
  const ServeRecordBytes &b = record->bytes();
  EXPECT_EQ(b.nodes, record->nodes().size() * sizeof(ServedNode));
  EXPECT_GT(b.ladders, 0u);
  EXPECT_GT(b.families, 0u);
  EXPECT_GT(b.geometries, 0u);
  EXPECT_EQ(b.total(), b.nodes + b.ladders + b.families + b.geometries + b.fixed);
  const ServeBuildClocks &c = record->clocks();
  EXPECT_EQ(c.lazy_family_builds, 2u);
  EXPECT_GT(c.family_evaluations, 0u);
  EXPECT_GT(c.total_us, 0.0);
  EXPECT_GE(c.total_us, c.family_us);
  EXPECT_TRUE(record->support().tiers[0] && record->support().tiers[1] &&
              record->support().tiers[2] && record->support().tiers[3]);
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
              ServeIvRequest{node.strike, side, 1.002 * kSpot, a.price, tier, tier});
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

} // namespace
} // namespace atx::vol
