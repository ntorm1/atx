// Immutable fitted serving record of one expiry (lanes H0-A and H1, r6 2026-10-23). See
// api/fitting/slice_serve_record.hpp for the contract; this file is the build (node
// identification, side families and their K3 statements, node ladders, audits, digests),
// the tiered reads, the fitter's hand-off and the evaluate_quote_slice fast path.
#include "atx/vol/api/fitting/slice_serve_record.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "atx/core/sha256.hpp"
#include "atx/vol/api/fitting/american_price_slice.hpp"
#include "atx/vol/api/fitting/vol_curve.hpp"
#include "atx/vol/api/pricing/american.hpp"
#include "atx/vol/api/pricing/american_iv.hpp"
#include "fitting/correction_detail.hpp" // cheb_node / cheb_dct2 / cheb_clenshaw1d
#include "fitting/deam_iv_cache.hpp"     // DeAmSigmaFamilies, deam_family_certify
#include "fitting/quote_slice_nodes.hpp" // detail::serves (lane C2's served support)
#include "fitting/slice_serve_record_detail.hpp"
#include "pricing/american_boundary.hpp"       // AlExpiryGeometry::Impl (geometry bytes)
#include "pricing/american_family_error.hpp"   // family_price_bounded, envelopes
#include "pricing/american_iv_premium.hpp"     // al_profile_price_bound
#include "pricing/american_sigma_q_family.hpp" // K1 fused price + vega

namespace atx::vol {

using atx::core::Err;
using atx::core::Ok;

namespace {

namespace fused = sigma_q_family_detail;

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kInf = std::numeric_limits<double>::infinity();
constexpr double kEps = std::numeric_limits<double>::epsilon();
constexpr std::size_t kLadderMaxSpot = 16;
constexpr std::size_t kLadderMaxSigma = 8;
// A call side's q box is clipped to q > 0 (a q <= 0 call never exercises early), as the
// fitter's (sigma, q) build does (quote_slice.cpp build_sigma_q).
constexpr double kCallMinYield = 1.0e-9;
// The between-node audit's declared multiple of its measured maximum (Sampled).
constexpr double kBetweenNodeSafety = 2.0;
// A Sampled ladder statement takes this multiple of K3's probe audit (relative).
constexpr double kSampledAuditSafety = 10.0;
constexpr int kLadderIvMaxEvaluations = 16;
constexpr int kFamilyIvMaxEvaluations = 24;
// A ladder root is converged when its map misses the target by at most this x max(1, P).
constexpr double kLadderIvPriceTol = 1.0e-12;
// The ladder statement's rounding floor: 8 ulps of max(F_max, K) (ChainSpotLadder's).
constexpr double kLadderUlpFloor = 8.0 * kEps;
// A plane coefficient at or under this many ulps of max(P_max, 1) is rounding: the
// coefficient series has converged into the samples' own noise.
constexpr double kPlaneNoiseUlps = 64.0;

using Clock = std::chrono::steady_clock;

[[nodiscard]] double us_since(Clock::time_point t0) noexcept {
  return std::chrono::duration<double, std::micro>(Clock::now() - t0).count();
}

[[nodiscard]] bool positive(double x) noexcept { return std::isfinite(x) && x > 0.0; }

[[nodiscard]] std::size_t side_slot(Side side) noexcept { return side == Side::Call ? 0u : 1u; }

[[nodiscard]] bool valid_side(Side side) noexcept {
  return side == Side::Call || side == Side::Put;
}

[[nodiscard]] std::uint64_t bits_of(double x) noexcept { return std::bit_cast<std::uint64_t>(x); }

// ── Chebyshev evaluation with derivatives (first kind, full c0) ─────────────────────
//
// A second copy of the differentiated Clenshaw recurrence expiry_record.cpp's eval_rows<2>
// runs per row (review-H0A m15): kept file-local because that one is fused into the
// record's row layout; both are pinned by their own derivative tests.

struct ChebD2 {
  double v = 0.0;
  double d = 0.0;
  double dd = 0.0;
};

// p(x) = c0 + sum_k c_k T_k(x) and its first two x-derivatives (differentiated Clenshaw).
[[nodiscard]] ChebD2 clenshaw_d2(const double *c, std::size_t n, double x) noexcept {
  double b1 = 0.0, b2 = 0.0, d1 = 0.0, d2 = 0.0, e1 = 0.0, e2 = 0.0;
  const double two_x = 2.0 * x;
  for (std::size_t k = n - 1; k >= 1; --k) {
    const double b = c[k] + two_x * b1 - b2;
    const double d = 2.0 * b1 + two_x * d1 - d2;
    const double e = 4.0 * d1 + two_x * e1 - e2;
    b2 = b1;
    b1 = b;
    d2 = d1;
    d1 = d;
    e2 = e1;
    e1 = e;
  }
  return ChebD2{c[0] + x * b1 - b2, b1 + x * d1 - d2, 2.0 * d1 + x * e1 - e2};
}

struct ChebD1 {
  double v = 0.0;
  double d = 0.0;
};

[[nodiscard]] ChebD1 clenshaw_d1(const double *c, std::size_t n, double x) noexcept {
  double b1 = 0.0, b2 = 0.0, d1 = 0.0, d2 = 0.0;
  const double two_x = 2.0 * x;
  for (std::size_t k = n - 1; k >= 1; --k) {
    const double b = c[k] + two_x * b1 - b2;
    const double d = 2.0 * b1 + two_x * d1 - d2;
    b2 = b1;
    b1 = b;
    d2 = d1;
    d1 = d;
  }
  return ChebD1{c[0] + x * b1 - b2, b1 + x * d1 - d2};
}

// ── The fitter's per-strike carry (quote_slice.cpp calendar_yield_at / effective_yield),
//    operation for operation: the record prices every contract at the yield the fit
//    certified it at (pinned: SliceServeRecord.NodeYieldReproducesTheCertifiedPriceToTheBit
//    and, on the funding-PV model, ...FundingPvNodeYieldReproduces...).

struct CarryModel {
  QuoteSliceMarket market{};
  QuoteSliceDividendModel model = QuoteSliceDividendModel::ContinuousYield;
  double reference_effective_yield = 0.0; // fit.diagnostics.effective_yield
  std::vector<QuoteSliceCarryPoint> curve;

  [[nodiscard]] static double log_strike_distance(double upper, double lower) noexcept {
    const double relative = (upper - lower) / lower;
    return std::isfinite(relative) ? std::log1p(relative) : std::log(upper) - std::log(lower);
  }

  [[nodiscard]] double calendar_yield_at(double strike) const noexcept {
    if (curve.empty()) return market.continuous_yield;
    const auto hi = std::lower_bound(
        curve.begin(), curve.end(), strike,
        [](const QuoteSliceCarryPoint &point, double value) { return point.strike < value; });
    if (hi == curve.end()) return curve.back().continuous_yield;
    if (hi == curve.begin() || hi->strike == strike) return hi->continuous_yield;
    const auto &lo = *(hi - 1);
    const double weight =
        log_strike_distance(strike, lo.strike) / log_strike_distance(hi->strike, lo.strike);
    return std::lerp(lo.continuous_yield, hi->continuous_yield, weight);
  }

  [[nodiscard]] double aggregate_log_retention(double calendar_yield) const noexcept {
    if (market.dividend_pv == 0.0) return 0.0;
    double fraction = market.dividend_pv / market.spot;
    if (model == QuoteSliceDividendModel::AggregateFundingPresentValue) {
      fraction = std::exp(std::log(market.dividend_pv) - std::log(market.spot) +
                          calendar_yield * market.calendar_years);
    }
    if (!(fraction < 1.0)) return kNaN;
    return std::log1p(-fraction);
  }

  [[nodiscard]] double effective_yield_at(double strike) const noexcept {
    const double calendar = calendar_yield_at(strike);
    if (calendar == market.continuous_yield) return reference_effective_yield;
    double value = calendar * (market.calendar_years / market.variance_years);
    if (model == QuoteSliceDividendModel::AggregatePresentValue ||
        model == QuoteSliceDividendModel::AggregateFundingPresentValue) {
      value -= aggregate_log_retention(calendar) / market.variance_years;
    }
    return value;
  }
};

// The zero-volatility American value (american_iv.cpp zero_vol_american_value, the
// non-monotone band's reference): the deterministic payoff's maximum over the exercise
// horizon {0, T, t*}. NaN when the payoff overflows. Third replica (review-H0A m9): its
// band decision is pinned against american_implied_vol's own NonMonotoneRegion refusal
// (SliceServeRecord.TheL1cBandDecisionIsAmericanIvsOwn).
[[nodiscard]] double zero_vol_american(double S, double K, double T, double r, double q,
                                       Side side) noexcept {
  const double A = (side == Side::Call) ? S : K;
  const double a = (side == Side::Call) ? q : r;
  const double B = (side == Side::Call) ? K : S;
  const double b = (side == Side::Call) ? r : q;
  const auto payoff = [A, a, B, b](double t) noexcept {
    return A * std::exp(-a * t) - B * std::exp(-b * t);
  };
  const double at_0 = payoff(0.0);
  const double at_T = payoff(T);
  if (!std::isfinite(at_0) || !std::isfinite(at_T)) return kNaN;
  double best = std::fmax(at_0, at_T);
  // SAFETY: as in american_iv.cpp - IEEE division and log are defined on every
  // degenerate input here; a non-finite t* or one outside (0, T) is skipped.
  const double t_star = std::log((a * A) / (b * B)) / (a - b);
  if (std::isfinite(t_star) && t_star > 0.0 && t_star < T) {
    const double at_star = payoff(t_star);
    if (std::isfinite(at_star)) best = std::fmax(best, at_star);
  }
  return std::fmax(best, 0.0);
}

// ── Node ladders (tier 0) ──────────────────────────────────────────────────────────

struct NodeLadder {
  double x_lo = 0.0; // ln-spot window
  double x_hi = 0.0;
  double s_lo = 0.0; // sigma window
  double s_hi = 0.0;
  double v_node = 0.0; // the node's served sigma on [-1, 1]
  std::uint8_t ns = 0;
  std::uint8_t nsig = 0;
  // plane[i * nsig + j]: the coefficient of T_i(u) T_j(v), u the ln-spot and v the sigma
  // coordinate on [-1, 1].
  std::array<double, kLadderMaxSpot * kLadderMaxSigma> plane{};
  std::array<double, kLadderMaxSpot> price{}; // the plane contracted at v_node
  std::array<double, kLadderMaxSpot> vega{};  // its sigma-derivative at v_node
  double certificate_usd = kNaN;
  ServeBoundClass bound_class = ServeBoundClass::None;
  // The near-exercise-edge band: edge_sd = edge_sign * (ln S - edge_x) * inv_sd, edge_x
  // the log of the node's converged edge (-inf / +inf in the European regime).
  double edge_x = kNaN;
  double edge_sign = 1.0; // put +1, call -1
  double inv_sd = kNaN;   // 1 / (sigma sqrt T) at the node's sigma
};

struct FamilyRead {
  double price = kNaN;
  double vega = kNaN;
  fused::FusedStatus status = fused::FusedStatus::Unsupported;
  bool exercised = false;
};

[[nodiscard]] FamilyRead family_read(const AlSigmaQFamily &f, double S, double K, double sigma,
                                     double q) noexcept {
  const fused::FusedPriceVega pv = fused::price_and_vega(f, S, K, sigma, q);
  return FamilyRead{pv.price, pv.vega, pv.status, pv.exercised};
}

[[nodiscard]] bool family_priced(const FamilyRead &r) noexcept {
  return r.status == fused::FusedStatus::Ok || r.status == fused::FusedStatus::NearNodeDegenerate;
}

// K3's per-point bound of `f`'s price at (S, K, sigma, q) against the cold accurate kernel
// (+inf without a covering envelope or outside its domain).
[[nodiscard]] double point_bound(const AlSigmaQFamily &f, const ValidatedErrorEnvelope *env,
                                 double S, double K, double sigma, double q) noexcept {
  if (env == nullptr || !env->covered()) return kInf;
  const FamilyBoundedPrice fb = family_price_bounded(f, *env, S, K, sigma, q);
  return fb.status == AlSigmaQFamilyStatus::Ok && std::isfinite(fb.price) ? fb.bound_usd : kInf;
}

#if !ATX_VOL_SERVE_RECORD_A0
// K1's fused vega reason: Exercised on the exercise side (vega 0, a number), else Served or
// NearNodeDegenerate (the vega is NaN at the sqrt transform's zero).
[[nodiscard]] ServeGreekReason fused_vega_reason(const FamilyRead &r) noexcept {
  if (r.exercised) return ServeGreekReason::Exercised;
  return r.status == fused::FusedStatus::Ok ? ServeGreekReason::Served
                                            : ServeGreekReason::NearNodeDegenerate;
}
#endif

#if ATX_VOL_SERVE_RECORD_A0
static_assert(kServeEdgeBand == kAlFamilyGreekEdgeBand, "the record's band is A0's");

[[nodiscard]] ServeGreekReason from_a0(AlFamilyGreekReason r) noexcept {
  switch (r) {
  case AlFamilyGreekReason::NotRequested: return ServeGreekReason::NotRequested;
  case AlFamilyGreekReason::Served: return ServeGreekReason::Served;
  case AlFamilyGreekReason::Exercised: return ServeGreekReason::Exercised;
  case AlFamilyGreekReason::Refused: return ServeGreekReason::Refused;
  case AlFamilyGreekReason::NoCarryBumps: return ServeGreekReason::NoCarryBumps;
  case AlFamilyGreekReason::CarryBumpRefused: return ServeGreekReason::CarryBumpRefused;
  case AlFamilyGreekReason::NoQAxis: return ServeGreekReason::NoQAxis;
  case AlFamilyGreekReason::NearNodeDegenerate: return ServeGreekReason::NearNodeDegenerate;
  case AlFamilyGreekReason::ConventionUnsupported: return ServeGreekReason::ConventionUnsupported;
  case AlFamilyGreekReason::NearExerciseEdge: return ServeGreekReason::NearExerciseEdge;
  }
  return ServeGreekReason::Refused;
}
#endif

} // namespace

// ── The record's state ──────────────────────────────────────────────────────────────

struct SliceServeRecord::Impl {
  ServeRecordPolicy policy{};
  std::vector<ServedNode> nodes;
  std::vector<std::int32_t> ladder_of; // 2 per node (call, put): index into ladders, -1 none
  std::vector<NodeLadder> ladders;
  std::shared_ptr<const DeAmSigmaFamilies> families;
  std::shared_ptr<const IVolCurve> curve;
  std::optional<AmericanPriceSlice> prices;
  CarryModel carry{};
  std::optional<AlOpts> al_opts;
  AlOpts fast_opts{};
  double spot = 0.0;
  double tau = 0.0;
  double rate = 0.0;
  double forward = 0.0;
  double strike_lo = 0.0;
  double strike_hi = 0.0;
  bool dated_cash = false;
  ServeSupport support{};
  ServeRecordVersion version{};
  ServeRecordBytes bytes{};
  ServeBuildClocks clocks{};

  [[nodiscard]] const AlSigmaQFamily *family(Side side) const noexcept {
    return families ? families->of_q(side) : nullptr;
  }
  // The side family's K3 envelope (nullptr: none, or the K3 seam is off).
  [[nodiscard]] const ValidatedErrorEnvelope *envelope(Side side) const noexcept {
#if ATX_VOL_SERVE_RECORD_K3
    return families ? families->q_envelope(side) : nullptr;
#else
    static_cast<void>(side);
    return nullptr;
#endif
  }
  [[nodiscard]] const ServeSideSupport &side_support(Side side) const noexcept {
    return side == Side::Call ? support.call : support.put;
  }
  [[nodiscard]] const NodeLadder *ladder(std::size_t node, Side side) const noexcept {
    const std::int32_t at = ladder_of[2 * node + side_slot(side)];
    return at < 0 ? nullptr : &ladders[static_cast<std::size_t>(at)];
  }
  // The node index whose strike equals `strike` exactly, or nodes.size().
  [[nodiscard]] std::size_t node_at(double strike) const noexcept {
    const auto it = std::lower_bound(
        nodes.begin(), nodes.end(), strike,
        [](const ServedNode &n, double value) { return n.strike < value; });
    return (it != nodes.end() && it->strike == strike)
               ? static_cast<std::size_t>(it - nodes.begin())
               : nodes.size();
  }
};

// The only maker of records (SliceServeRecord::Key), and the fast path's read access.
struct SliceServeRecordAccess {
  [[nodiscard]] static std::shared_ptr<const SliceServeRecord>
  make(std::unique_ptr<const SliceServeRecord::Impl> impl) {
    return std::make_shared<const SliceServeRecord>(SliceServeRecord::Key{}, std::move(impl));
  }
  [[nodiscard]] static const SliceServeRecord::Impl &impl(const SliceServeRecord &r) noexcept {
    return *r.impl_;
  }
};

namespace {

using Impl = SliceServeRecord::Impl;

// ── Build: node identification ────────────────────────────────────────────────────────

[[nodiscard]] bool flagged(const std::vector<std::uint8_t> &flags, std::size_t node,
                           std::uint8_t bit) noexcept {
  return node < flags.size() && (flags[node] & bit) != 0u;
}

[[nodiscard]] ServeRootAcceptance acceptance_of(const ServedNodeSide &s) noexcept {
  ServeRootAcceptance a;
  if (s.status == QuoteSliceEvaluationStatus::Success) {
    if (s.convention == QuoteSliceVolConvention::FittedCurve) {
      a.reason = ServeAcceptanceReason::FittedCurve; // the curve's value: no root
    } else {
      // query_quote_slice's cold Search inversion on the accurate kernel (K3: the
      // established keys-OFF confirm of every non-native route).
      a.kind = ServeAcceptanceKind::Confirmed;
      a.reason = ServeAcceptanceReason::NewtonContract;
    }
  } else if (s.status == QuoteSliceEvaluationStatus::PriceOnly) {
    a.kind = ServeAcceptanceKind::PriceOnly;
    a.reason = s.reason == QuoteSlicePriceOnlyReason::LowVega ? ServeAcceptanceReason::LowVega
                                                              : ServeAcceptanceReason::None;
  }
  return a;
}

void identify_node_side(const QuoteSliceFit &fit, const CarryModel &carry, std::size_t i,
                        Side side, ServedNodeSide &out, ServeBuildClocks &clocks) {
  const AmericanPriceNode &node = fit.prices->nodes()[i];
  const std::uint8_t bit = side == Side::Call ? 1u : 2u;
  out.price = side == Side::Call ? node.call : node.put;
  out.moved = flagged(fit.moved_nodes, i, bit);
  out.q = carry.effective_yield_at(node.strike);
  for (ServeGreekReason &r : out.family_reasons) r = ServeGreekReason::NoMap;
  const Result<QuoteSliceQuery> query = query_quote_slice(fit, node.strike, side);
  if (!query) {
    // An unmoved node whose curve volatility is not positive: no volatility served.
    out.status = QuoteSliceEvaluationStatus::InvalidVolatility;
    out.acceptance = acceptance_of(out);
    return;
  }
  out.status = query->status;
  out.reason = query->reason;
  out.convention = query->vol_convention;
  out.sigma = query->volatility;
  out.iv_lower = query->iv_lower;
  out.iv_upper = query->iv_upper;
  out.acceptance = acceptance_of(out);
  if (out.convention == QuoteSliceVolConvention::ReimpliedRepaired ||
      (out.status == QuoteSliceEvaluationStatus::PriceOnly && out.moved)) {
    ++clocks.reimplied_nodes;
  }
}

[[nodiscard]] bool serves_sigma(const ServedNodeSide &s) noexcept {
  return s.status == QuoteSliceEvaluationStatus::Success && positive(s.sigma);
}

// ── Build: side families (the fitter's (sigma, q) recipe, K3-certified) ───────────────

struct FamilyInputs {
  double spot = 0.0;
  double tau = 0.0;
  double rate = 0.0;
  double reference_q = 0.0;
  double strike_lo = 0.0;
  double strike_hi = 0.0;
  std::optional<AlOpts> al_opts;
};

struct SideBox {
  double sigma_lo = kNaN;
  double sigma_hi = kNaN;
  double q_lo = kNaN;
  double q_hi = kNaN;
};

[[nodiscard]] std::optional<SideBox> side_box(std::span<const ServedNode> nodes, Side side,
                                              const ServeRecordPolicy &policy) noexcept {
  double s_min = kInf, s_max = 0.0, q_min = kInf, q_max = -kInf;
  for (const ServedNode &n : nodes) {
    const ServedNodeSide &s = side == Side::Call ? n.call : n.put;
    if (std::isfinite(s.q)) {
      q_min = std::min(q_min, s.q);
      q_max = std::max(q_max, s.q);
    }
    if (serves_sigma(s)) {
      s_min = std::min(s_min, s.sigma);
      s_max = std::max(s_max, s.sigma);
    }
  }
  if (!(s_max > 0.0) || !std::isfinite(q_min)) return std::nullopt;
  SideBox box;
  box.sigma_lo = std::max(kAlSigmaFamilyMinSigma, policy.sigma_lo_mult * s_min);
  box.sigma_hi = policy.sigma_hi_mult * s_max;
  box.q_lo = q_min;
  box.q_hi = q_max;
  if (side == Side::Call) {
    if (!(box.q_hi > kCallMinYield)) return std::nullopt;
    box.q_lo = std::max(box.q_lo, kCallMinYield);
  }
  if (!(box.sigma_hi > box.sigma_lo)) return std::nullopt;
  return box;
}

[[nodiscard]] AlExpiryGeometry &geometry_of(DeAmSigmaFamilies &holder, Side side) noexcept {
  return side == Side::Put ? holder.q_geometry_put : holder.q_geometry_call;
}

[[nodiscard]] std::optional<AlSigmaQFamily> &slot_of(DeAmSigmaFamilies &holder,
                                                     Side side) noexcept {
  return side == Side::Put ? holder.q_put : holder.q_call;
}

// One side's family into `holder` with the record's box (quote_slice.cpp build_sigma_q,
// operation for operation), then its error contract through lane K3's deam_family_certify
// exactly as the fitter's (review-W2ci M1 / m1 / m2: the validated envelope, audited by
// probes independent of the nodes, and the grid bound). False when the family or its
// certification refuses (the side then serves no family).
[[nodiscard]] bool build_side_family(DeAmSigmaFamilies &holder, const FamilyInputs &in,
                                     const SideBox &box, Side side) {
  AlExpiryGeometry &geometry = geometry_of(holder, side);
  std::optional<AlSigmaQFamily> &slot = slot_of(holder, side);
  geometry.prepare(in.tau, in.rate, std::clamp(in.reference_q, box.q_lo, box.q_hi), in.al_opts);
  const std::uint16_t n_q = in.tau * (box.q_hi - box.q_lo) <= kDeAmQFamilyNarrowCarry
                                ? kDeAmQFamilyNarrowQNodes
                                : kDeAmQFamilyQNodes;
  slot.emplace(geometry, side, in.spot);
  if (!slot->prepare(box.sigma_lo, box.sigma_hi, kDeAmQFamilySigmaNodes, box.q_lo, box.q_hi, n_q,
                     kDeAmFamilyAxis)) {
    slot.reset();
    return false;
  }
  const double band_lo = std::max(in.strike_lo, in.spot * std::exp(-kDeAmFamilyGridLogBand));
  const double band_hi = std::min(in.strike_hi, in.spot * std::exp(kDeAmFamilyGridLogBand));
  if (!deam_family_certify(holder, side, in.spot, in.tau, in.rate, in.al_opts, in.strike_lo,
                           in.strike_hi, band_lo, band_hi)) {
    slot.reset();
    return false;
  }
  return true;
}

// Every number of the envelope its construction derives from the family's nodes (not the
// audit, which the hand-off carries separately), bit for bit.
[[nodiscard]] bool same_envelope(const ValidatedErrorEnvelope &a,
                                 const ValidatedErrorEnvelope &b) noexcept {
  const auto same = [](double x, double y) { return bits_of(x) == bits_of(y); };
  const auto same_n = [&](const auto &x, const auto &y, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
      if (!same(x[i], y[i])) return false;
    }
    return true;
  };
  const FamilyErrorContributions &ca = a.contributions;
  const FamilyErrorContributions &cb = b.contributions;
  return a.n_sigma == b.n_sigma && a.n_q == b.n_q && a.n_quad == b.n_quad &&
         a.n_quad <= kFamilyEnvelopeMaxQuad && same(a.domain.sigma_lo, b.domain.sigma_lo) &&
         same(a.domain.sigma_hi, b.domain.sigma_hi) && same(a.domain.q_lo, b.domain.q_lo) &&
         same(a.domain.q_hi, b.domain.q_hi) && same(a.domain.T, b.domain.T) &&
         a.domain.side == b.domain.side && same_n(a.bound_y, b.bound_y, a.n_quad) &&
         same_n(a.zc, b.zc, a.n_quad) && same_n(a.y_lo, b.y_lo, a.n_quad) &&
         same_n(a.z, b.z, a.n_quad) && same_n(a.w, b.w, a.n_quad) &&
         same(ca.interpolation_y, cb.interpolation_y) &&
         same(ca.boundary_solve_y, cb.boundary_solve_y) && same(ca.reference_y, cb.reference_y) &&
         same(ca.rounding_y, cb.rounding_y) && same(ca.rounding_rel, cb.rounding_rel) &&
         same(ca.sigma_ratio, cb.sigma_ratio) && same(ca.q_ratio, cb.q_ratio) &&
         ca.unverified_nodes == cb.unverified_nodes;
}

// One handed side re-prepared into `holder` with the fitter's recipe; true when the rebuilt
// family reproduces the fitter's two probe prices and its envelope bit for bit. The
// fitter's audit verdict (its 9 probes against the cold kernel) is then applied to the
// rebuilt envelope unchanged, so the record reads the fitter's own error contract.
[[nodiscard]] bool rebuild_handed_side(DeAmSigmaFamilies &holder,
                                       const QuoteSliceServeHandoff::Family &h, Side side,
                                       const std::optional<AlOpts> &al_opts) {
  AlExpiryGeometry &geometry = geometry_of(holder, side);
  std::optional<AlSigmaQFamily> &slot = slot_of(holder, side);
  geometry.prepare(h.geometry_T, h.geometry_r, h.geometry_q, al_opts);
  slot.emplace(geometry, side, h.anchor);
  if (!slot->prepare(h.sigma_lo, h.sigma_hi, h.n_sigma, h.q_lo, h.q_hi, h.n_q,
                     kDeAmFamilyAxis)) {
    slot.reset();
    return false;
  }
  for (const QuoteSliceServeHandoff::Probe &p : h.probes) {
    const AlSigmaQFamilyPrice fp = slot->price(h.anchor, p.K, p.sigma, p.q);
    if (fp.status != AlSigmaQFamilyStatus::Ok || bits_of(fp.price) != bits_of(p.price)) {
      slot.reset();
      return false;
    }
  }
  ValidatedErrorEnvelope env = build_family_error_envelope(*slot);
  if (!same_envelope(env, h.envelope)) {
    slot.reset();
    return false;
  }
  record_family_probe_audit(env, h.envelope.audit);
  if (env.error_class != h.envelope.error_class || env.status != h.envelope.status) {
    slot.reset();
    return false;
  }
  (side == Side::Put ? holder.q_envelope_put : holder.q_envelope_call) = env;
  (side == Side::Put ? holder.q_bound_put : holder.q_bound_call) = h.q_bound;
  return true;
}

struct FamilyBuild {
  std::shared_ptr<const DeAmSigmaFamilies> holder;
  std::array<ServeFamilyOrigin, 2> origin{}; // side_slot order (call, put)
  std::array<bool, 2> mismatch{};
  std::size_t built = 0;
};

// The families of `nodes`: per side the fit's hand-off when it carries that side
// (re-prepared and verified), else the record's own recipe; both K3-certified.
[[nodiscard]] FamilyBuild build_families(std::span<const ServedNode> nodes,
                                         const FamilyInputs &in, const ServeRecordPolicy &policy,
                                         const QuoteSliceServeHandoff *handoff) {
  FamilyBuild out;
  if (!(in.tau >= kAlSigmaFamilyMinTau)) return out;
  auto holder = std::make_shared<DeAmSigmaFamilies>();
  holder->q = in.reference_q;
  const auto t0 = Clock::now();
  for (const Side side : {Side::Put, Side::Call}) {
    const std::size_t s = side_slot(side);
    const QuoteSliceServeHandoff::Family *h =
        handoff != nullptr ? &handoff->sides[side == Side::Put ? 0u : 1u] : nullptr;
    if (h != nullptr && h->present) {
      ++out.built;
      if (rebuild_handed_side(*holder, *h, side, in.al_opts)) {
        out.origin[s] = ServeFamilyOrigin::Handed;
      } else {
        out.mismatch[s] = true;
      }
      continue;
    }
    const std::optional<SideBox> box = side_box(nodes, side, policy);
    if (!box) continue;
    ++out.built;
    if (build_side_family(*holder, in, *box, side)) out.origin[s] = ServeFamilyOrigin::Built;
  }
  holder->build_us = us_since(t0);
  if (holder->of_q(Side::Put) != nullptr || holder->of_q(Side::Call) != nullptr) {
    out.holder = std::move(holder);
  }
  return out;
}

// A supplied holder built for another expiry (review-H0A m7): a present side whose
// geometry clock, rate or anchor differs from the fit's bit for bit.
[[nodiscard]] bool holder_mismatched(const DeAmSigmaFamilies &holder, const QuoteSliceFit &fit) {
  for (const Side side : {Side::Put, Side::Call}) {
    const AlSigmaQFamily *f = holder.of_q(side);
    if (f == nullptr) continue;
    const AlExpiryGeometry &g = side == Side::Put ? holder.q_geometry_put : holder.q_geometry_call;
    if (bits_of(g.T()) != bits_of(fit.market.variance_years) ||
        bits_of(g.r()) != bits_of(fit.diagnostics.effective_rate) ||
        bits_of(f->anchor()) != bits_of(fit.market.spot)) {
      return true;
    }
  }
  return false;
}

// ── Build: ladders ─────────────────────────────────────────────────────────────────────

struct LadderInputs {
  double spot = 0.0;
  double tau = 0.0;
  double rate = 0.0;
  double strike = 0.0;
  double sigma = 0.0;
  double q = 0.0;
  Side side = Side::Call;
  const ValidatedErrorEnvelope *envelope = nullptr; // K3 (nullptr: no validated statement)
  double sampled_rel = kInf; // 10 x K3's probe audit (relative): the Sampled statement only
  // |family - certified price| at the node at the fit's spot (measured; Sampled only).
  double node_family_error = 0.0;
};

// What sampling and checking a ladder measured.
struct LadderMeasure {
  double price_max = 0.0;
  double family_bound_max = 0.0; // the largest K3 per-point bound over samples and checks
  double check_max = 0.0;        // the largest |plane - family| at the check points
};

// 2-D Chebyshev value of the ladder plane at (u, v).
[[nodiscard]] double plane_value(const NodeLadder &l, double u, double v) noexcept {
  std::array<double, kLadderMaxSpot> row{};
  for (std::size_t i = 0; i < l.ns; ++i) {
    row[i] = detail::cheb_clenshaw1d(l.plane.data() + i * l.nsig, l.nsig, v);
  }
  return detail::cheb_clenshaw1d(row.data(), l.ns, u);
}

// Samples the plane at the Chebyshev nodes; false when a sample is refused or exercised.
[[nodiscard]] bool sample_ladder(NodeLadder &l, const AlSigmaQFamily &f, const LadderInputs &in,
                                 LadderMeasure &m, std::size_t &evaluations) {
  std::array<double, kLadderMaxSpot * kLadderMaxSigma> vals{}; // [j * ns + i]
  for (std::uint16_t j = 0; j < l.nsig; ++j) {
    const double sigma = detail::cheb_from_unit(detail::cheb_node(j, l.nsig), l.s_lo, l.s_hi);
    for (std::uint16_t i = 0; i < l.ns; ++i) {
      const double S = std::exp(detail::cheb_from_unit(detail::cheb_node(i, l.ns), l.x_lo, l.x_hi));
      const FamilyRead r = family_read(f, S, in.strike, sigma, in.q);
      ++evaluations;
      if (r.status != fused::FusedStatus::Ok || r.exercised) return false;
      vals[static_cast<std::size_t>(j) * l.ns + i] = r.price;
      m.price_max = std::max(m.price_max, r.price);
      if (in.envelope != nullptr) {
        m.family_bound_max =
            std::max(m.family_bound_max, point_bound(f, in.envelope, S, in.strike, sigma, in.q));
        ++evaluations;
      }
    }
  }
  std::array<double, kLadderMaxSpot> in_row{};
  std::array<double, kLadderMaxSpot> out_row{};
  std::array<double, kLadderMaxSpot * kLadderMaxSigma> tmp{}; // [j * ns + i]
  for (std::size_t j = 0; j < l.nsig; ++j) {
    for (std::size_t i = 0; i < l.ns; ++i) in_row[i] = vals[j * l.ns + i];
    detail::cheb_dct2(in_row.data(), out_row.data(), l.ns);
    for (std::size_t i = 0; i < l.ns; ++i) tmp[j * l.ns + i] = out_row[i];
  }
  std::array<double, kLadderMaxSigma> col{};
  std::array<double, kLadderMaxSigma> col_out{};
  for (std::size_t i = 0; i < l.ns; ++i) {
    for (std::size_t j = 0; j < l.nsig; ++j) col[j] = tmp[j * l.ns + i];
    detail::cheb_dct2(col.data(), col_out.data(), l.nsig);
    for (std::size_t j = 0; j < l.nsig; ++j) l.plane[i * l.nsig + j] = col_out[j];
  }
  return true;
}

// The plane's measured distance to the family off its nodes: every midpoint between
// adjacent (sorted) nodes plus both window ends on each axis, and the node's own sigma
// row (a SAMPLED measurement, and the audit of the tail bound). False when a check point
// is refused or exercised.
[[nodiscard]] bool check_ladder(const NodeLadder &l, const AlSigmaQFamily &f,
                                const LadderInputs &in, LadderMeasure &m,
                                std::size_t &evaluations) {
  const auto checks = [](std::uint8_t n, std::array<double, kLadderMaxSpot + 2> &out) {
    std::array<double, kLadderMaxSpot> nodes_sorted{};
    for (std::uint16_t i = 0; i < n; ++i) nodes_sorted[i] = detail::cheb_node(i, n);
    std::sort(nodes_sorted.begin(), nodes_sorted.begin() + n);
    std::size_t k = 0;
    out[k++] = -1.0;
    for (std::size_t i = 0; i + 1 < n; ++i) {
      out[k++] = 0.5 * (nodes_sorted[i] + nodes_sorted[i + 1]);
    }
    out[k++] = 1.0;
    return k;
  };
  std::array<double, kLadderMaxSpot + 2> us{};
  std::array<double, kLadderMaxSpot + 2> vs{};
  const std::size_t nu = checks(l.ns, us);
  std::size_t nv = checks(l.nsig, vs);
  vs[nv++] = l.v_node;
  for (std::size_t b = 0; b < nv; ++b) {
    const double sigma = detail::cheb_from_unit(vs[b], l.s_lo, l.s_hi);
    for (std::size_t a = 0; a < nu; ++a) {
      const double S = std::exp(detail::cheb_from_unit(us[a], l.x_lo, l.x_hi));
      const FamilyRead r = family_read(f, S, in.strike, sigma, in.q);
      ++evaluations;
      if (r.status != fused::FusedStatus::Ok || r.exercised) return false;
      m.check_max = std::max(m.check_max, std::abs(plane_value(l, us[a], vs[b]) - r.price));
      if (in.envelope != nullptr) {
        m.family_bound_max =
            std::max(m.family_bound_max, point_bound(f, in.envelope, S, in.strike, sigma, in.q));
        ++evaluations;
      }
    }
  }
  return true;
}

// One axis's Chebyshev tail past the plane's last coefficient (K3's refinement rule on the
// ladder). With R_k the largest |coefficient| of order k on this axis, taken in pairs from
// the top (E = max(R_{n-1}, R_{n-2}), P = max(R_{n-3}, R_{n-4}), C = max(R_{n-5}, R_{n-6})) so
// the even / odd alternation of a near-symmetric map never reads as a stall: the decay must
// hold over the last three pairs (E < P < C; two pairs when n < 6) and the tail past the last
// coefficient extrapolates the SLOWEST observed pair decay, rho^2 = max(E / P, P / C):
// E rho / (1 - rho). The check points audit the result (build_ladder: a check above the
// bound vetoes it). A series at the rounding floor is converged. nullopt: fewer than four
// orders, a coefficient rising out of noise, or a decay that does not hold.
template <class Norm>
[[nodiscard]] std::optional<double> axis_tail(Norm &&norm, std::size_t n, double floor) {
  if (n < 4) return std::nullopt;
  const double e = std::max(norm(n - 1), norm(n - 2));
  const double p = std::max(norm(n - 3), norm(n - 4));
  if (e <= floor) return floor;
  if (p <= floor) return std::nullopt;
  double rho2 = e / p;
  if (n >= 6) {
    const double c = std::max(norm(n - 5), norm(n - 6));
    if (!(p < c)) return std::nullopt;
    rho2 = std::max(rho2, p / c);
  }
  if (!(rho2 < 1.0)) return std::nullopt;
  const double rho = std::sqrt(rho2);
  return e * rho / (1.0 - rho) + floor;
}

// The plane's interpolation error bound over its window: twice (aliasing) the sum of the
// verified tails of both axes, plus the coefficients' own rounding. nullopt when either
// axis's order does not verify (the ladder then has no validated statement).
[[nodiscard]] std::optional<double> plane_tail_bound(const NodeLadder &l, double price_max) {
  const double scale = std::max(price_max, 1.0);
  const double floor = kPlaneNoiseUlps * kEps * scale;
  const auto spot_norm = [&l](std::size_t i) {
    double r = 0.0;
    for (std::size_t j = 0; j < l.nsig; ++j) r = std::max(r, std::abs(l.plane[i * l.nsig + j]));
    return r;
  };
  const auto sigma_norm = [&l](std::size_t j) {
    double r = 0.0;
    for (std::size_t i = 0; i < l.ns; ++i) r = std::max(r, std::abs(l.plane[i * l.nsig + j]));
    return r;
  };
  const std::optional<double> tail_u = axis_tail(spot_norm, l.ns, floor);
  const std::optional<double> tail_v = axis_tail(sigma_norm, l.nsig, floor);
  if (!tail_u || !tail_v) return std::nullopt;
  const double rounding =
      4.0 * kEps * scale * static_cast<double>(l.ns) * static_cast<double>(l.nsig);
  return 2.0 * (*tail_u + *tail_v) + rounding;
}

struct LadderStatement {
  double tail = kNaN;
  double check = kNaN;
  double family_bound = kNaN;
};

// One node side's ladder, or nullopt when the window or its statement does not qualify.
[[nodiscard]] std::optional<NodeLadder> build_ladder(const AlSigmaQFamily &f,
                                                     const LadderInputs &in,
                                                     const ServeRecordPolicy &policy,
                                                     std::size_t &evaluations,
                                                     LadderStatement &statement) {
  NodeLadder l;
  l.ns = policy.ladder_spot_nodes;
  l.nsig = policy.ladder_sigma_nodes;
  const double w = std::clamp(policy.ladder_window_sigmas * in.sigma * std::sqrt(in.tau),
                              policy.ladder_window_min, policy.ladder_window_max);
  l.x_lo = std::log(in.spot) - w;
  l.x_hi = std::log(in.spot) + w;
  l.s_lo = std::max(in.sigma * (1.0 - policy.ladder_sigma_half_width), f.sigma_lo());
  l.s_hi = std::min(in.sigma * (1.0 + policy.ladder_sigma_half_width), f.sigma_hi());
  if (!(l.s_lo < in.sigma && in.sigma < l.s_hi)) return std::nullopt;
  l.v_node = detail::cheb_to_unit(in.sigma, l.s_lo, l.s_hi);
  LadderMeasure m;
  if (!sample_ladder(l, f, in, m, evaluations)) return std::nullopt;
  if (!check_ladder(l, f, in, m, evaluations)) return std::nullopt;
  const double forward_max = std::exp(l.x_hi + (in.rate - in.q) * in.tau);
  const double floor = kLadderUlpFloor * std::max(forward_max, in.strike);
  const double limit =
      std::max(policy.ladder_max_cert_usd, policy.ladder_max_cert_rel * forward_max);
  const std::optional<double> tail = plane_tail_bound(l, m.price_max);
  statement.tail = tail ? *tail : kNaN;
  statement.check = m.check_max;
  statement.family_bound = in.envelope != nullptr ? m.family_bound_max : kNaN;
  // Validated: the verified tail (audited: no check point above it) plus K3's per-point
  // bound; Sampled: the measured check plus 10 x K3's probe audit (or the node error).
  const bool validated = tail && m.check_max <= *tail && std::isfinite(statement.family_bound);
  const double validated_usd = validated ? *tail + m.family_bound_max + floor : kInf;
  const double sampled_usd =
      m.check_max +
      std::max(in.sampled_rel * std::max(m.price_max, 1.0), in.node_family_error) + floor;
  if (validated_usd <= limit) {
    l.certificate_usd = validated_usd;
    l.bound_class = ServeBoundClass::RefinementValidated;
  } else if (policy.sampled_ladders && sampled_usd <= limit) {
    l.certificate_usd = sampled_usd;
    l.bound_class = ServeBoundClass::Sampled;
  } else {
    return std::nullopt;
  }
  const double dv_dsigma = 2.0 / (l.s_hi - l.s_lo);
  for (std::size_t i = 0; i < l.ns; ++i) {
    const ChebD1 c = clenshaw_d1(l.plane.data() + i * l.nsig, l.nsig, l.v_node);
    l.price[i] = c.v;
    l.vega[i] = c.d * dv_dsigma;
  }
  return l;
}

// ── Build: audits ──────────────────────────────────────────────────────────────────────

void audit_family_at_nodes(Impl &impl) {
  for (ServedNode &node : impl.nodes) {
    for (const Side side : {Side::Call, Side::Put}) {
      ServedNodeSide &s = side == Side::Call ? node.call : node.put;
      const AlSigmaQFamily *f = impl.family(side);
      if (f == nullptr || !serves_sigma(s)) continue;
      const FamilyRead r = family_read(*f, impl.spot, node.strike, s.sigma, s.q);
      ++impl.clocks.family_evaluations;
      if (!family_priced(r)) continue;
      s.tiers |= kServeNodeFamily;
      s.family_price_delta = r.price - s.price;
#if ATX_VOL_SERVE_RECORD_A0
      // A0's reasons at the fit's spot for the full mask (what the Family tier serves there).
      AlSigmaQGreeksRequest req;
      req.mask = AlFamilyGreekMask::all();
      req.held_fixed = AlFamilyGreekHeldFixed::StickyStrike;
      req.surface_version = impl.policy.surface_version;
      const AlSigmaQFamilyGreeks g = family_greeks(*f, impl.spot, node.strike, s.sigma, s.q, req);
      for (std::size_t k = 0; k < kServeGreekCount; ++k) {
        s.family_reasons[k] = g.status == AlSigmaQFamilyStatus::Ok ? from_a0(g.reason[k])
                                                                   : ServeGreekReason::Refused;
      }
#else
      for (ServeGreekReason &g : s.family_reasons) g = ServeGreekReason::BundleUnavailable;
      s.family_reasons[static_cast<std::size_t>(ServeGreek::Vega)] = fused_vega_reason(r);
#endif
      const ValidatedErrorEnvelope *env = impl.envelope(side);
      s.family_class = env != nullptr && env->covered() ? ServeErrorClass::RefinementValidated
                                                        : ServeErrorClass::Uncovered;
    }
  }
}

// max |family - slice| at the quarter points of every interval, at the fit's spot.
[[nodiscard]] double audit_between_nodes(Impl &impl, Side side) {
  const AlSigmaQFamily *f = impl.family(side);
  if (f == nullptr || impl.nodes.size() < 2) return kNaN;
  double worst = 0.0;
  for (std::size_t i = 0; i + 1 < impl.nodes.size(); ++i) {
    for (const double t : {0.25, 0.5, 0.75}) {
      const double K = std::lerp(impl.nodes[i].strike, impl.nodes[i + 1].strike, t);
      const double sigma = impl.curve->iv(std::log(K / impl.forward));
      const std::optional<double> slice = impl.prices->price(K, side);
      if (!positive(sigma) || !slice) continue;
      const FamilyRead r = family_read(*f, impl.spot, K, sigma, impl.carry.effective_yield_at(K));
      ++impl.clocks.family_evaluations;
      if (!family_priced(r)) continue;
      worst = std::max(worst, std::abs(r.price - *slice));
    }
  }
  return kBetweenNodeSafety * worst;
}

// ── Build: digests ─────────────────────────────────────────────────────────────────────

class Digest {
public:
  void add(double x) { add(std::bit_cast<std::uint64_t>(x)); }
  void add(std::uint64_t x) {
    std::array<std::byte, 8> b{};
    for (std::size_t i = 0; i < 8; ++i) b[i] = static_cast<std::byte>((x >> (8 * i)) & 0xffu);
    ok_ = ok_ && h_.update(b).has_value();
  }
  void add(std::int64_t x) { add(static_cast<std::uint64_t>(x)); }
  void add(std::uint8_t x) { add(static_cast<std::uint64_t>(x)); }
  void add(bool x) { add(static_cast<std::uint64_t>(x ? 1u : 0u)); }
  void add_size(std::size_t x) { add(static_cast<std::uint64_t>(x)); }
  [[nodiscard]] ServeDigest finish() {
    const auto out = h_.finalize();
    return (ok_ && out) ? *out : ServeDigest{};
  }

private:
  atx::core::Sha256 h_{};
  bool ok_ = true;
};

template <class E> [[nodiscard]] std::uint8_t byte_of(E e) noexcept {
  return static_cast<std::uint8_t>(e);
}

[[nodiscard]] ServeDigest fit_digest(const QuoteSliceFit &fit) {
  Digest d;
  d.add(byte_of(fit.fit_status));
  d.add_size(fit.prices->nodes().size());
  for (const AmericanPriceNode &n : fit.prices->nodes()) {
    d.add(n.strike);
    d.add(n.call);
    d.add(n.put);
    d.add(fit.curve->w(std::log(n.strike / fit.diagnostics.forward)));
  }
  d.add_size(fit.moved_nodes.size());
  for (const std::uint8_t m : fit.moved_nodes) d.add(m);
  d.add_size(fit.price_only_nodes.size());
  for (const std::uint8_t m : fit.price_only_nodes) d.add(m);
  d.add(byte_of(fit.curve->kind()));
  d.add(fit.curve->T());
  d.add(fit.curve->F());
  d.add(fit.curve->df());
  d.add(byte_of(fit.floor_policy));
  d.add_size(fit.rows.size());
  for (const QuoteSliceRow &r : fit.rows) {
    d.add(byte_of(r.evaluation_status));
    d.add(r.fitted_iv);
    d.add(r.fitted_price);
    d.add(byte_of(r.price_only_reason));
    d.add(r.iv_lower);
    d.add(r.iv_upper);
    d.add(byte_of(r.vol_source));
  }
  return d.finish();
}

[[nodiscard]] ServeDigest input_digest(const QuoteSliceFit &fit) {
  Digest d;
  const QuoteSliceMarket &m = fit.market;
  for (const double x : {m.spot, m.variance_years, m.calendar_years, m.rate, m.continuous_yield,
                         m.dividend_pv}) {
    d.add(x);
  }
  d.add(byte_of(fit.dividend_model));
  d.add(fit.al_opts.has_value());
  if (fit.al_opts) {
    const AlOpts &o = *fit.al_opts;
    for (const std::uint16_t v : {o.n_collocation, o.n_quadrature, o.n_quad_price,
                                  o.max_newton_iter}) {
      d.add(static_cast<std::uint64_t>(v));
    }
    d.add(o.tol);
    d.add(byte_of(o.profile));
  }
  d.add(static_cast<std::int64_t>(fit.discrete_div_steps));
  d.add(byte_of(fit.discrete_div_scheme));
  d.add_size(fit.variance_clock_dividends.size());
  for (const CashDividend &c : fit.variance_clock_dividends) {
    d.add(c.tau);
    d.add(c.amount);
  }
  d.add_size(fit.carry_curve.size());
  for (const QuoteSliceCarryPoint &c : fit.carry_curve) {
    d.add(c.strike);
    d.add(c.continuous_yield);
  }
  const QuoteSliceSnapshotInfo &s = fit.snapshot;
  d.add(s.quote_ns);
  d.add(s.underlying_ns);
  d.add(static_cast<std::uint64_t>(s.spot_version));
  d.add(byte_of(s.spot_rule));
  d.add(static_cast<std::uint64_t>(s.carry_version));
  d.add(byte_of(s.carry_convention));
  d.add(s.raw_spot);
  d.add(s.spot_stale);
  d.add_size(fit.rows.size());
  for (const QuoteSliceRow &r : fit.rows) {
    d.add(byte_of(r.quote_status));
    d.add(r.admitted);
    d.add(r.market_iv);
    d.add(r.spread_vol);
    d.add(r.weight_w);
    d.add(r.weight_capped);
  }
  return d.finish();
}

[[nodiscard]] ServeDigest policy_digest(const ServeRecordPolicy &p) {
  Digest d;
  d.add(byte_of(p.families));
  d.add(p.ladders);
  d.add(p.sigma_lo_mult);
  d.add(p.sigma_hi_mult);
  d.add(p.ladder_spot_nodes);
  d.add(p.ladder_sigma_nodes);
  d.add(p.ladder_window_sigmas);
  d.add(p.ladder_window_min);
  d.add(p.ladder_window_max);
  d.add(p.ladder_sigma_half_width);
  d.add(p.ladder_max_cert_usd);
  d.add(p.ladder_max_cert_rel);
  d.add(p.max_age_ns);
  d.add(p.iv_tolerance);
  d.add(p.surface_version);
  d.add(static_cast<std::uint64_t>(ATX_VOL_SERVE_RECORD_A0));
  d.add(static_cast<std::uint64_t>(ATX_VOL_SERVE_RECORD_K3));
  // Lane H1: the statement contract (K3 per-point + verified ladder tails) and the key.
  d.add(p.sampled_ladders);
  d.add(static_cast<std::uint64_t>(2)); // statement contract version 2 (H1)
  return d.finish();
}

// ── Build: orchestration ───────────────────────────────────────────────────────────────

[[nodiscard]] FamilyInputs family_inputs(const QuoteSliceFit &fit) {
  const auto nodes = fit.prices->nodes();
  return FamilyInputs{fit.market.spot, fit.market.variance_years, fit.diagnostics.effective_rate,
                      fit.diagnostics.effective_yield, nodes.front().strike, nodes.back().strike,
                      fit.al_opts};
}

void identify_nodes(Impl &impl, const QuoteSliceFit &fit) {
  const auto t0 = Clock::now();
  const auto slice = fit.prices->nodes();
  impl.nodes.resize(slice.size());
  for (std::size_t i = 0; i < slice.size(); ++i) {
    impl.nodes[i].strike = slice[i].strike;
    identify_node_side(fit, impl.carry, i, Side::Call, impl.nodes[i].call, impl.clocks);
    identify_node_side(fit, impl.carry, i, Side::Put, impl.nodes[i].put, impl.clocks);
  }
  impl.clocks.identify_us = us_since(t0);
}

void copy_basics(Impl &impl, const QuoteSliceFit &fit, const ServeRecordPolicy &policy) {
  impl.policy = policy;
  impl.spot = fit.market.spot;
  impl.tau = fit.market.variance_years;
  impl.rate = fit.diagnostics.effective_rate;
  impl.forward = fit.diagnostics.forward;
  impl.strike_lo = fit.prices->nodes().front().strike;
  impl.strike_hi = fit.prices->nodes().back().strike;
  impl.dated_cash = fit.dividend_model == QuoteSliceDividendModel::DatedCash;
  impl.carry = CarryModel{fit.market, fit.dividend_model, fit.diagnostics.effective_yield,
                          fit.carry_curve};
  impl.al_opts = fit.al_opts;
  impl.fast_opts = al_fast_profile(fit.al_opts);
  impl.curve = std::shared_ptr<const IVolCurve>(fit.curve->clone());
  impl.prices = fit.prices;
  impl.ladder_of.assign(2 * fit.prices->nodes().size(), -1);
  impl.support.tau = impl.tau;
  impl.support.tau_min_family = kAlSigmaFamilyMinTau;
  impl.support.tiers[static_cast<std::size_t>(ServeTier::Cold)] = true;
  impl.support.a0_bundle = ATX_VOL_SERVE_RECORD_A0 != 0;
  impl.support.k3_envelope = ATX_VOL_SERVE_RECORD_K3 != 0;
  const double q = fit.diagnostics.effective_yield;
  impl.support.double_continuation =
      detail::classify_regime(impl.rate, q) == detail::ExerciseRegime::Unsupported ||
      detail::classify_regime(q, impl.rate) == detail::ExerciseRegime::Unsupported;
}

// The side support's K3 fields from the side family's envelope and the holder's grid bound.
void describe_side_family(const Impl &impl, Side side, ServeSideSupport &s) {
  const AlSigmaQFamily *f = impl.family(side);
  s.family = f != nullptr;
  if (f == nullptr) return;
  s.sigma_lo = f->sigma_lo();
  s.sigma_hi = f->sigma_hi();
  s.q_lo = f->q_lo();
  s.q_hi = f->q_hi();
  s.domain_bound_usd = impl.families->q_bound(side);
  const ValidatedErrorEnvelope *env = impl.envelope(side);
  if (env == nullptr) return;
  s.family_class = env->covered() ? ServeErrorClass::RefinementValidated
                                  : ServeErrorClass::Uncovered;
  s.envelope_status = static_cast<std::uint8_t>(env->status);
  s.unverified_nodes = env->contributions.unverified_nodes;
  s.audit_probes = env->audit.probes;
  s.audit_violations = env->audit.violations;
  s.audit_max_bound_ratio = env->audit.max_bound_ratio;
  if (env->audit.probes > 0 && std::isfinite(env->audit.max_rel)) {
    s.family_bound_rel = env->audit.max_rel;
  }
}

// `kept`: the origins of a supplied holder that an earlier record of the same fit built
// (the serve_build stage's second phase); nullptr: a caller's holder (Supplied).
void attach_families(Impl &impl, const QuoteSliceFit &fit,
                     std::shared_ptr<const DeAmSigmaFamilies> supplied,
                     const std::array<ServeFamilyOrigin, 2> *kept) {
  const auto t0 = Clock::now();
  std::array<ServeFamilyOrigin, 2> origin{};
  std::array<bool, 2> mismatch{};
  if (supplied) {
    impl.families = std::move(supplied);
    for (const Side side : {Side::Call, Side::Put}) {
      if (impl.families->of_q(side) != nullptr) {
        origin[side_slot(side)] =
            kept != nullptr ? (*kept)[side_slot(side)] : ServeFamilyOrigin::Supplied;
      }
    }
  } else if (impl.policy.families == ServeFamilyMode::Build) {
    FamilyBuild built = build_families(impl.nodes, family_inputs(fit), impl.policy,
                                       fit.serve_handoff.get());
    impl.families = std::move(built.holder);
    impl.clocks.lazy_family_builds = built.built;
    origin = built.origin;
    mismatch = built.mismatch;
  }
  for (const Side side : {Side::Call, Side::Put}) {
    ServeSideSupport &s = side == Side::Call ? impl.support.call : impl.support.put;
    describe_side_family(impl, side, s);
    s.origin = s.family ? origin[side_slot(side)] : ServeFamilyOrigin::None;
    if (s.family) continue;
    if (mismatch[side_slot(side)]) {
      s.reason = ServeUnsupportedReason::HandoffMismatch;
    } else if (impl.policy.families == ServeFamilyMode::None && !impl.families) {
      s.reason = ServeUnsupportedReason::FamiliesNotBuilt;
    } else if (!(impl.tau >= kAlSigmaFamilyMinTau)) {
      s.reason = ServeUnsupportedReason::BelowFamilyTau;
    } else if (!side_box(impl.nodes, side, impl.policy)) {
      s.reason = ServeUnsupportedReason::NoServedVolatility;
    } else {
      s.reason = ServeUnsupportedReason::FamilyRefused;
    }
  }
  impl.clocks.family_us = us_since(t0);
}

// The converged early-exercise edge at the node's (sigma, q): 0 (put) / +inf (call) in the
// European regime, NaN when the solve refuses (the side is then ladder-ineligible).
[[nodiscard]] double locate_edge(const Impl &impl, double strike, double sigma, double q,
                                 Side side) {
  const Result<double> edge =
      exercise_boundary(strike, impl.tau, sigma, impl.rate, q, side, impl.al_opts);
  if (edge) return *edge;
  if (edge.error().code() == ErrorCode::OutOfRange) {
    return side == Side::Put ? 0.0 : std::numeric_limits<double>::infinity();
  }
  return kNaN;
}

void attach_ladders(Impl &impl) {
  const auto t0 = Clock::now();
  for (std::size_t i = 0; i < impl.nodes.size(); ++i) {
    for (const Side side : {Side::Call, Side::Put}) {
      ServedNodeSide &s = side == Side::Call ? impl.nodes[i].call : impl.nodes[i].put;
      const AlSigmaQFamily *f = impl.family(side);
      if (f == nullptr || !(s.tiers & kServeNodeFamily)) continue;
      ServeSideSupport &support = side == Side::Call ? impl.support.call : impl.support.put;
      s.edge_spot = locate_edge(impl, impl.nodes[i].strike, s.sigma, s.q, side);
      if (std::isnan(s.edge_spot)) {
        ++support.ladders_ineligible;
        continue;
      }
      const ValidatedErrorEnvelope *env = impl.envelope(side);
      LadderInputs in;
      in.spot = impl.spot;
      in.tau = impl.tau;
      in.rate = impl.rate;
      in.strike = impl.nodes[i].strike;
      in.sigma = s.sigma;
      in.q = s.q;
      in.side = side;
      in.envelope = env != nullptr && env->covered() ? env : nullptr;
      in.sampled_rel = kSampledAuditSafety * support.family_bound_rel;
      in.node_family_error = std::abs(s.family_price_delta);
      LadderStatement statement;
      std::optional<NodeLadder> l = build_ladder(*f, in, impl.policy,
                                                 impl.clocks.family_evaluations, statement);
      s.ladder_plane_tail_usd = statement.tail;
      s.ladder_plane_check_usd = statement.check;
      s.ladder_family_bound_usd = statement.family_bound;
      if (!l) {
        ++support.ladders_ineligible;
        continue;
      }
      l->edge_x = std::log(s.edge_spot);
      l->edge_sign = side == Side::Put ? 1.0 : -1.0;
      l->inv_sd = 1.0 / (s.sigma * std::sqrt(impl.tau));
      s.ladder_certificate_usd = l->certificate_usd;
      s.ladder_class = l->bound_class;
      s.tiers |= kServeNodeLadder;
      impl.ladder_of[2 * i + side_slot(side)] = static_cast<std::int32_t>(impl.ladders.size());
      impl.ladders.push_back(*l);
      ++support.ladders;
      ++(l->bound_class == ServeBoundClass::RefinementValidated ? support.ladders_validated
                                                                : support.ladders_sampled);
    }
  }
  impl.clocks.ladder_us = us_since(t0);
}

void finish_support(Impl &impl) {
  ServeSupport &s = impl.support;
  const bool family = s.call.family || s.put.family;
  s.tiers[static_cast<std::size_t>(ServeTier::Ladder)] = !impl.ladders.empty();
  s.tiers[static_cast<std::size_t>(ServeTier::Family)] = family;
  s.tiers[static_cast<std::size_t>(ServeTier::Certified)] = !impl.dated_cash;
  s.greeks[static_cast<std::size_t>(ServeTier::Ladder)] = ServeGreekMask::four();
#if ATX_VOL_SERVE_RECORD_A0
  const ServeGreekMask family_greeks = ServeGreekMask::all();
#else
  const ServeGreekMask family_greeks{ServeGreekMask::bit(ServeGreek::Vega)};
#endif
  s.greeks[static_cast<std::size_t>(ServeTier::Family)] = family ? family_greeks
                                                                 : ServeGreekMask::none();
  s.greeks[static_cast<std::size_t>(ServeTier::Certified)] =
      family ? family_greeks : ServeGreekMask::none();
}

// Every heap block a record holds: the node and ladder vectors, the families' state and
// EVERY geometry of the holder - the sigma family's and both (sigma, q) sides', each
// allocated by construction whether or not a side was built (review-H0A m4).
void measure_bytes(Impl &impl) {
  impl.bytes.nodes = impl.nodes.size() * sizeof(ServedNode);
  impl.bytes.ladders = impl.ladders.size() * sizeof(NodeLadder) +
                       impl.ladder_of.size() * sizeof(std::int32_t);
  impl.bytes.fixed = sizeof(Impl) + sizeof(SliceServeRecord);
  if (!impl.families) return;
  impl.bytes.fixed += sizeof(DeAmSigmaFamilies);
  for (const Side side : {Side::Call, Side::Put}) {
    const AlSigmaQFamily *f = impl.family(side);
    if (f != nullptr) impl.bytes.families += f->state_bytes();
  }
  for (const AlExpiryGeometry *g : {&impl.families->geometry, &impl.families->q_geometry_put,
                                    &impl.families->q_geometry_call}) {
    if (g->impl() != nullptr) impl.bytes.geometries += sizeof(AlExpiryGeometry::Impl);
  }
}

[[nodiscard]] std::int64_t unix_ns_now() noexcept {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

void stamp_version(Impl &impl, const QuoteSliceFit &fit) {
  const auto t0 = Clock::now();
  impl.version.fit_sha256 = fit_digest(fit);
  impl.version.policy_sha256 = policy_digest(impl.policy);
  impl.version.input_sha256 = input_digest(fit);
  impl.clocks.digest_us = us_since(t0);
  impl.version.build_steady_ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch())
          .count();
  impl.version.build_wall_ns = unix_ns_now();
  impl.version.as_of_ns =
      fit.snapshot.quote_ns != 0 ? fit.snapshot.quote_ns : impl.version.build_wall_ns;
}

[[nodiscard]] Result<std::shared_ptr<const SliceServeRecord>>
build_record(const QuoteSliceFit &fit, const ServeRecordPolicy &policy,
             std::shared_ptr<const DeAmSigmaFamilies> supplied,
             const std::array<ServeFamilyOrigin, 2> *kept = nullptr) {
  const auto t0 = Clock::now();
  const Status valid = validate_serve_record_policy(policy);
  if (!valid) return Err(valid.error());
  if (fit.fit_status != QuoteSliceFitStatus::Success || !fit.curve || !fit.prices ||
      fit.prices->nodes().empty()) {
    return Err(ErrorCode::Unavailable, "serve record: the fit has no certified price slice");
  }
  if (supplied && holder_mismatched(*supplied, fit)) {
    return Err(ErrorCode::InvalidArgument,
               "serve record: the supplied families were built for another expiry");
  }
  auto impl = std::make_unique<Impl>();
  copy_basics(*impl, fit, policy);
  identify_nodes(*impl, fit);
  if (impl->dated_cash) {
    impl->support.cash_dividends = ServeCashDividends::UnsupportedInRecord;
    impl->support.reason = ServeUnsupportedReason::DatedCash;
    impl->support.call.reason = impl->support.put.reason = ServeUnsupportedReason::DatedCash;
  } else {
    attach_families(*impl, fit, std::move(supplied), kept);
    const auto t_audit = Clock::now();
    audit_family_at_nodes(*impl);
    impl->support.call.between_node_bound_usd = audit_between_nodes(*impl, Side::Call);
    impl->support.put.between_node_bound_usd = audit_between_nodes(*impl, Side::Put);
    impl->clocks.audit_us = us_since(t_audit);
    if (policy.ladders) attach_ladders(*impl);
  }
  finish_support(*impl);
  measure_bytes(*impl);
  stamp_version(*impl, fit);
  impl->clocks.total_us = us_since(t0);
  return Ok(SliceServeRecordAccess::make(std::move(impl)));
}

// ── Reads ──────────────────────────────────────────────────────────────────────────────

struct Contract {
  double strike = 0.0;
  Side side = Side::Call;
  double spot = 0.0;
  bool at_fit = false;
  std::size_t node = 0; // impl.nodes.size(): between nodes
  const ServedNodeSide *served = nullptr;
  double sigma = kNaN; // the sticky-strike volatility
  double q = kNaN;
};

[[nodiscard]] Contract resolve(const Impl &impl, double strike, Side side, double spot) noexcept {
  Contract c;
  c.strike = strike;
  c.side = side;
  c.spot = spot;
  c.at_fit = spot == impl.spot;
  c.node = impl.node_at(strike);
  if (c.node < impl.nodes.size()) {
    c.served = side == Side::Call ? &impl.nodes[c.node].call : &impl.nodes[c.node].put;
    c.sigma = serves_sigma(*c.served) ? c.served->sigma : kNaN;
    c.q = c.served->q;
  } else {
    const double sigma = impl.curve->iv(std::log(strike / impl.forward));
    c.sigma = positive(sigma) ? sigma : kNaN;
    c.q = impl.carry.effective_yield_at(strike);
  }
  return c;
}

void request_reasons(ServeAnswer &a, ServeGreekMask mask, ServeGreekReason requested) noexcept {
  for (std::size_t g = 0; g < kServeGreekCount; ++g) {
    a.reason[g] = mask.has(static_cast<ServeGreek>(g)) ? requested : ServeGreekReason::NotRequested;
  }
}

void set_greek(ServeAnswer &a, ServeGreekMask mask, ServeGreek g, double value,
               ServeGreekReason reason) noexcept {
  if (!mask.has(g)) return;
  const auto at = static_cast<std::size_t>(g);
  a.reason[at] = reason;
  const bool number = reason == ServeGreekReason::Served || reason == ServeGreekReason::Exercised;
  double &slot = g == ServeGreek::Delta   ? a.delta
                 : g == ServeGreek::Gamma ? a.gamma
                 : g == ServeGreek::Vega  ? a.vega
                 : g == ServeGreek::Theta ? a.theta
                 : g == ServeGreek::Rho   ? a.rho
                                          : a.phi;
  slot = number ? value : kNaN;
}

// The certified slice price at the fit's spot: the node's bytes or the piecewise-linear
// slice between nodes.
[[nodiscard]] double slice_price(const Impl &impl, const Contract &c,
                                 ServePriceSource &source) noexcept {
  if (c.served != nullptr) {
    source = ServePriceSource::SliceNode;
    return c.served->price;
  }
  source = ServePriceSource::SlicePiecewiseLinear;
  const std::optional<double> p = impl.prices->price(c.strike, c.side);
  return p ? *p : kNaN;
}

// One map's error statement in USD and what backs it.
struct Statement {
  double usd = kNaN;
  ServeBoundClass cls = ServeBoundClass::None;
};

// Ruling b: at the fit's spot the served price is the slice's (Exact at a node, no
// statement between nodes: review-H0A m3); the map's price only measures the gap to the
// Greeks' map, whose own statement is carried beside. Elsewhere the price is the map's.
void place_price(const Impl &impl, const Contract &c, double map_price, ServePriceSource map_src,
                 Statement map, ServeAnswer &a) noexcept {
  if (c.at_fit) {
    a.price = slice_price(impl, c, a.price_source);
    a.price_map_delta = std::abs(map_price - a.price);
    if (a.price_source == ServePriceSource::SliceNode) {
      a.certificate_usd = 0.0;
      a.certificate_class = ServeBoundClass::Exact;
    } else {
      a.certificate_usd = kNaN;
      a.certificate_class = ServeBoundClass::None;
    }
    a.map_certificate_usd = map.usd;
    a.map_certificate_class = map.cls;
  } else {
    a.price = map_price;
    a.price_source = map_src;
    a.price_map_delta = 0.0;
    a.certificate_usd = map.usd;
    a.certificate_class = map.cls;
  }
  a.sampled = a.certificate_class == ServeBoundClass::Sampled ||
              a.map_certificate_class == ServeBoundClass::Sampled;
}

[[nodiscard]] bool serve_ladder(const Impl &impl, const Contract &c, ServeGreekMask mask,
                                ServeAnswer &a) noexcept {
  if (c.served == nullptr) return false;
  const NodeLadder *l = impl.ladder(c.node, c.side);
  if (l == nullptr) return false;
  const double x = std::log(c.spot);
  if (!(x >= l->x_lo && x <= l->x_hi)) return false;
  const double u = detail::cheb_to_unit(x, l->x_lo, l->x_hi);
  const ChebD2 p = clenshaw_d2(l->price.data(), l->ns, u);
  const double intrinsic = c.side == Side::Put ? c.strike - c.spot : c.spot - c.strike;
  if (!(p.v > intrinsic) || !(p.v > 0.0)) return false; // the family's exercise side
  const double vega = detail::cheb_clenshaw1d(l->vega.data(), l->ns, u);
  const double scale = 2.0 / (l->x_hi - l->x_lo);
  const double p_x = p.d * scale;
  const double p_xx = p.dd * scale * scale;
  const double S = c.spot;
  const double delta = p_x / S;
  const double gamma = (p_xx - p_x) / (S * S);
  const double theta = impl.rate * p.v - (impl.rate - c.q) * S * delta -
                       0.5 * c.sigma * c.sigma * S * S * gamma;
  // The band (A0 fix round): the identity theta and the map's gamma are withheld within
  // kServeEdgeBand sigma sqrt T of the node's converged edge.
  a.edge_sd = l->edge_sign * (x - l->edge_x) * l->inv_sd;
  a.near_edge = a.edge_sd < kServeEdgeBand;
  const ServeGreekReason banded =
      a.near_edge ? ServeGreekReason::NearExerciseEdge : ServeGreekReason::Served;
  request_reasons(a, mask, ServeGreekReason::TierHasNoGreek);
  set_greek(a, mask, ServeGreek::Delta, delta, ServeGreekReason::Served);
  set_greek(a, mask, ServeGreek::Gamma, gamma, banded);
  set_greek(a, mask, ServeGreek::Vega, vega, ServeGreekReason::Served);
  set_greek(a, mask, ServeGreek::Theta, theta, banded);
  a.tier = ServeTier::Ladder;
  a.status = ServeStatus::Ok;
  a.route = ServeRoute::None;
  a.greek_source = ServeGreekSource::Ladder;
  a.convention.model = ServeGreekModel::NodeLadder;
  place_price(impl, c, p.v, ServePriceSource::Ladder, Statement{l->certificate_usd, l->bound_class},
              a);
  return true;
}

// The family map's price and Greeks at (S, K, sigma, q) into `a` (no price placement).
// False with `box` set when the volatility or yield lies outside the family box.
[[nodiscard]] bool family_tier_read(const Impl &impl, const Contract &c, ServeGreekMask mask,
                                    ServeAnswer &a, double &map_price, bool &box) noexcept {
  const AlSigmaQFamily *f = impl.family(c.side);
  if (f == nullptr || !positive(c.sigma)) return false;
#if ATX_VOL_SERVE_RECORD_A0
  AlSigmaQGreeksRequest req;
  req.mask = AlFamilyGreekMask{mask.bits};
  req.held_fixed = AlFamilyGreekHeldFixed::StickyStrike;
  req.surface_version = impl.policy.surface_version;
  const AlSigmaQFamilyGreeks g = family_greeks(*f, c.spot, c.strike, c.sigma, c.q, req);
  if (g.status != AlSigmaQFamilyStatus::Ok) {
    box = g.status == AlSigmaQFamilyStatus::SigmaOutsideBox ||
          g.status == AlSigmaQFamilyStatus::SigmaBelowGuard ||
          g.status == AlSigmaQFamilyStatus::YieldOutsideBox;
    return false;
  }
  map_price = g.price;
  a.edge_sd = g.edge_sd;
  a.near_edge = g.near_edge;
  const double values[kServeGreekCount] = {g.delta, g.gamma, g.vega, g.theta, g.rho, g.phi};
  for (std::size_t k = 0; k < kServeGreekCount; ++k) {
    set_greek(a, mask, static_cast<ServeGreek>(k), values[k], from_a0(g.reason[k]));
  }
  a.status = g.exercised ? ServeStatus::Exercised : ServeStatus::Ok;
#else
  const FamilyRead r = family_read(*f, c.spot, c.strike, c.sigma, c.q);
  if (!family_priced(r)) {
    box = r.status == fused::FusedStatus::OutsideBox;
    return false;
  }
  map_price = r.price;
  request_reasons(a, mask, ServeGreekReason::BundleUnavailable);
  set_greek(a, mask, ServeGreek::Vega, r.vega, fused_vega_reason(r));
  a.status = r.exercised ? ServeStatus::Exercised : ServeStatus::Ok;
#endif
  a.greek_source = ServeGreekSource::Family;
  a.convention.model = ServeGreekModel::SigmaQFamily;
  return true;
}

// The family tier's statement: K3's per-point bound at the contract (review-W2ci For H1
// item 2), RefinementValidated where the side's envelope covers the point; none otherwise
// or when the request skips it (one family pass saved).
[[nodiscard]] Statement family_statement(const Impl &impl, const Contract &c,
                                         bool wanted) noexcept {
  const AlSigmaQFamily *f = impl.family(c.side);
  if (!wanted || f == nullptr) return {};
  const double bound = point_bound(*f, impl.envelope(c.side), c.spot, c.strike, c.sigma, c.q);
  if (!std::isfinite(bound)) return {};
  return Statement{bound, ServeBoundClass::RefinementValidated};
}

[[nodiscard]] bool serve_family(const Impl &impl, const Contract &c, const ServeRequest &request,
                                ServeAnswer &a, bool &box) noexcept {
  double map_price = kNaN;
  if (!family_tier_read(impl, c, request.greeks, a, map_price, box)) return false;
  a.tier = ServeTier::Family;
  a.route = ServeRoute::None;
  place_price(impl, c, map_price, ServePriceSource::Family,
              family_statement(impl, c, request.family_bound), a);
  return true;
}

// Certified: the slice at the fit's spot (Greeks from the family where it serves); at any
// other spot the family answer with its price replaced by one AlProfile::Fast solve.
[[nodiscard]] bool serve_certified(const Impl &impl, const Contract &c,
                                   const ServeRequest &request, ServeAnswer &a, bool &box) {
  const ServeGreekMask mask = request.greeks;
  double map_price = kNaN;
  const bool greeks = family_tier_read(impl, c, mask, a, map_price, box);
  const Statement family = greeks ? family_statement(impl, c, request.family_bound) : Statement{};
  if (c.at_fit) {
    if (!greeks) {
      request_reasons(a, mask, ServeGreekReason::NoMap);
      a.status = ServeStatus::Ok;
      a.greek_source = ServeGreekSource::None;
    }
    a.tier = ServeTier::Certified;
    a.route = ServeRoute::None;
    place_price(impl, c, map_price, ServePriceSource::None, family, a);
    if (!greeks) a.price_map_delta = kNaN;
    return std::isfinite(a.price);
  }
  if (!greeks) return false;
  const Result<double> fast = andersen_lake(c.spot, c.strike, impl.tau, c.sigma, impl.rate, c.q,
                                            c.side, impl.fast_opts);
  if (!fast) return false;
  a.tier = ServeTier::Certified;
  a.route = ServeRoute::None;
  a.price = *fast;
  a.price_source = ServePriceSource::FamilyFastConfirm;
  a.price_map_delta = std::abs(map_price - *fast);
  a.certificate_usd = al_profile_price_bound(AlProfile::Fast, *fast, impl.tau);
  a.certificate_class = ServeBoundClass::KernelTested;
  a.map_certificate_usd = family.usd;
  a.map_certificate_class = family.cls;
  a.sampled = false;
  return true;
}

void refuse(ServeAnswer &a, ServeStatus status, ServeRoute route, ServeGreekMask mask) noexcept {
  a.status = status;
  a.route = route;
  a.tier = ServeTier::Cold;
  a.price = kNaN;
  a.price_source = ServePriceSource::None;
  a.greek_source = ServeGreekSource::None;
  request_reasons(a, mask, ServeGreekReason::NoMap);
}

[[nodiscard]] bool request_valid(double strike, Side side, double spot, ServeTier tier,
                                 ServeTier tier_max) noexcept {
  return positive(strike) && positive(spot) && valid_side(side) &&
         static_cast<std::uint8_t>(tier) <= static_cast<std::uint8_t>(ServeTier::Cold) &&
         static_cast<std::uint8_t>(tier_max) <= static_cast<std::uint8_t>(ServeTier::Cold) &&
         static_cast<std::uint8_t>(tier) <= static_cast<std::uint8_t>(tier_max);
}

[[nodiscard]] ServeTier last_servable(ServeTier tier_max) noexcept {
  return tier_max == ServeTier::Cold ? ServeTier::Certified : tier_max;
}

// The staleness gate of serve() and implied_vol() (review-H0A m1 / m2): with max_age_ns > 0
// a request without a clock (now_ns <= 0) refuses Stale / ColdPath - it never fails open -
// and one older than the policy refuses Stale / Rebuild. A clock before as_of (a replay)
// is fresh.
[[nodiscard]] bool fresh(const Impl &impl, std::int64_t now_ns, std::int64_t &age_ns,
                         ServeStatus &status, ServeRoute &route) noexcept {
  if (impl.policy.max_age_ns <= 0) return true;
  if (now_ns <= 0) {
    status = ServeStatus::Stale;
    route = ServeRoute::ColdPath;
    return false;
  }
  age_ns = now_ns - impl.version.as_of_ns;
  if (age_ns > impl.policy.max_age_ns) {
    status = ServeStatus::Stale;
    route = ServeRoute::Rebuild;
    return false;
  }
  return true;
}

// ── IV ─────────────────────────────────────────────────────────────────────────────────

enum class RootOutcome : std::uint8_t { Converged, BelowBox, AboveBox, Flat, Budget, Refused };

struct Root {
  double sigma = kNaN;
  double residual = kNaN;
  double slope = kNaN; // dP/dsigma at the root
  RootOutcome outcome = RootOutcome::Refused;
  int evaluations = 0;
};

// Newton with a bisection bracket on [lo, hi] of a map f(x) -> {P - target, dP/dx}; the box
// ends are evaluated only when Newton aims past them (ExpiryRecord::implied_vol's rule).
template <class Map>
[[nodiscard]] Root bracketed_newton(Map &&map, double lo, double hi, double seed, double price_tol,
                                    int max_evaluations) noexcept {
  Root out;
  const double box_lo = lo;
  const double box_hi = hi;
  double x = std::clamp(seed, lo, hi);
  for (int it = 0; it < max_evaluations; ++it) {
    double f = 0.0, df = 0.0;
    if (!map(x, f, df)) {
      out.outcome = RootOutcome::Refused;
      return out;
    }
    out.evaluations = it + 1;
    out.sigma = x;
    out.residual = f;
    out.slope = df;
    if (std::abs(f) <= price_tol) {
      out.outcome = df > 0.0 ? RootOutcome::Converged : RootOutcome::Flat;
      return out;
    }
    if (x == box_lo && f > 0.0) {
      out.outcome = RootOutcome::BelowBox;
      return out;
    }
    if (x == box_hi && f < 0.0) {
      out.outcome = RootOutcome::AboveBox;
      return out;
    }
    (f > 0.0 ? hi : lo) = x;
    double next = 0.5 * (lo + hi);
    if (df > 0.0) {
      const double newton = x - f / df;
      if (newton > lo && newton < hi) {
        next = newton;
      } else if (newton >= hi && hi == box_hi) {
        next = box_hi;
      } else if (newton <= lo && lo == box_lo) {
        next = box_lo;
      }
    }
    if (!(hi - lo > 1.0e-15 * box_hi) || next == x) break;
    x = next;
  }
  out.outcome = RootOutcome::Budget;
  return out;
}

[[nodiscard]] Root ladder_root(const NodeLadder &l, double spot, double target) noexcept {
  const double u = detail::cheb_to_unit(std::log(spot), l.x_lo, l.x_hi);
  // The plane contracted at u: one sigma series (lockstep over the sigma columns).
  std::array<double, kLadderMaxSigma> b1{}, b2{}, c{};
  const double two_u = 2.0 * u;
  for (std::size_t k = static_cast<std::size_t>(l.ns) - 1; k >= 1; --k) {
    const double *row = l.plane.data() + k * l.nsig;
    for (std::size_t j = 0; j < l.nsig; ++j) {
      const double b = row[j] + two_u * b1[j] - b2[j];
      b2[j] = b1[j];
      b1[j] = b;
    }
  }
  for (std::size_t j = 0; j < l.nsig; ++j) c[j] = l.plane[j] + u * b1[j] - b2[j];
  const double dv_dsigma = 2.0 / (l.s_hi - l.s_lo);
  const auto map = [&](double sigma, double &f, double &df) noexcept {
    const ChebD1 p = clenshaw_d1(c.data(), l.nsig, detail::cheb_to_unit(sigma, l.s_lo, l.s_hi));
    f = p.v - target;
    df = p.d * dv_dsigma;
    return std::isfinite(f) && std::isfinite(df);
  };
  const double seed = detail::cheb_from_unit(l.v_node, l.s_lo, l.s_hi);
  return bracketed_newton(map, l.s_lo, l.s_hi, seed, kLadderIvPriceTol * std::max(1.0, target),
                          kLadderIvMaxEvaluations);
}

[[nodiscard]] Root family_root(const AlSigmaQFamily &f, const Contract &c, double target,
                               double vol_tol) noexcept {
  const auto map = [&](double sigma, double &fv, double &df) noexcept {
    const FamilyRead r = family_read(f, c.spot, c.strike, sigma, c.q);
    if (!family_priced(r)) return false;
    fv = r.price - target;
    df = r.status == fused::FusedStatus::Ok ? r.vega : 0.0;
    return std::isfinite(fv);
  };
  const double lo = f.sigma_lo();
  const double hi = f.sigma_hi();
  const double seed = positive(c.sigma) ? c.sigma : 0.5 * (lo + hi);
  // Converged when the Newton distance is a thousandth of the requirement (price units:
  // the slope at the seed is unknown here, so a relative price tolerance stands in).
  Root r = bracketed_newton(map, lo, hi, seed, 1.0e-13 * std::max(1.0, target),
                            kFamilyIvMaxEvaluations);
  if (r.outcome == RootOutcome::Budget && r.slope > 0.0 &&
      std::abs(r.residual / r.slope) <= 1.0e-3 * vol_tol) {
    r.outcome = RootOutcome::Converged;
  }
  return r;
}

void take_root(ServeIvAnswer &a, const Root &r) noexcept {
  a.sigma = r.sigma;
  a.price_residual = r.residual;
  a.newton_distance_vol = r.slope > 0.0 ? r.residual / r.slope : kNaN;
  a.evaluations = static_cast<std::uint8_t>(std::min(255, a.evaluations + r.evaluations));
}

// K3's enclosure: both ends of a bracket of width W around the root certified by the
// family price -/+ its envelope bound (american_family_error.hpp).
[[nodiscard]] bool family_enclosure(const Impl &impl, const AlSigmaQFamily &f, const Contract &c,
                                    double sigma, double target, ServeRootAcceptance &acc) {
  const ValidatedErrorEnvelope *env = impl.envelope(c.side);
  if (env == nullptr || !env->covered()) return false;
  const double W = std::min(impl.policy.iv_tolerance, kIvRootRequirement) * (1.0 - 0x1p-20);
  const FamilyBoundedPrice lo = family_price_bounded(f, *env, c.spot, c.strike, sigma - 0.5 * W,
                                                     c.q);
  const FamilyBoundedPrice hi = family_price_bounded(f, *env, c.spot, c.strike, sigma + 0.5 * W,
                                                     c.q);
  if (lo.status != AlSigmaQFamilyStatus::Ok || hi.status != AlSigmaQFamilyStatus::Ok) return false;
  if (!(lo.price + lo.bound_usd <= target && target <= hi.price - hi.bound_usd)) return false;
  acc.kind = ServeAcceptanceKind::Skipped;
  acc.reason = ServeAcceptanceReason::FamilyEnclosure;
  acc.error_class = ServeErrorClass::RefinementValidated;
  acc.bound_iv = W;
  return true;
}

// The ladder tier's root: Ok only on a RefinementValidated ladder whose bound / slope plus
// the Newton distance meets iv_tolerance (review-W2ci M1 item 4, review-H0A m6). Returns
// true when answered; `unconfirmed` when a root converged without being accepted.
[[nodiscard]] bool ladder_iv(const Impl &impl, const Contract &c, double target,
                             ServeIvAnswer &a, bool &box, bool &unconfirmed) noexcept {
  if (c.served == nullptr) return false;
  const NodeLadder *l = impl.ladder(c.node, c.side);
  const double x = std::log(c.spot);
  if (l == nullptr || !(x >= l->x_lo && x <= l->x_hi)) return false;
  const Root r = ladder_root(*l, c.spot, target);
  take_root(a, r);
  box = r.outcome == RootOutcome::BelowBox || r.outcome == RootOutcome::AboveBox;
  if (r.outcome != RootOutcome::Converged) return false;
  const double bound = l->certificate_usd / r.slope + std::abs(a.newton_distance_vol);
  if (l->bound_class != ServeBoundClass::RefinementValidated ||
      !(bound <= impl.policy.iv_tolerance)) {
    unconfirmed = true;
    return false;
  }
  a.tier = ServeTier::Ladder;
  a.status = ServeStatus::Ok;
  a.route = ServeRoute::None;
  a.acceptance.kind = ServeAcceptanceKind::LadderCertificate;
  a.acceptance.error_class = ServeErrorClass::RefinementValidated;
  a.acceptance.bound_iv = bound;
  return true;
}

} // namespace

// ── SliceServeRecord ──────────────────────────────────────────────────────────────────

SliceServeRecord::SliceServeRecord(Key key, std::unique_ptr<const Impl> impl) noexcept
    : impl_(std::move(impl)) {
  static_cast<void>(key);
}
SliceServeRecord::~SliceServeRecord() = default;

std::span<const ServedNode> SliceServeRecord::nodes() const noexcept { return impl_->nodes; }
const ServeSupport &SliceServeRecord::support() const noexcept { return impl_->support; }
const ServeRecordVersion &SliceServeRecord::version() const noexcept { return impl_->version; }
const ServeRecordBytes &SliceServeRecord::bytes() const noexcept { return impl_->bytes; }
const ServeBuildClocks &SliceServeRecord::clocks() const noexcept { return impl_->clocks; }
const ServeRecordPolicy &SliceServeRecord::policy() const noexcept { return impl_->policy; }
const IVolCurve &SliceServeRecord::curve() const noexcept { return *impl_->curve; }
double SliceServeRecord::spot() const noexcept { return impl_->spot; }
double SliceServeRecord::tau() const noexcept { return impl_->tau; }
double SliceServeRecord::rate() const noexcept { return impl_->rate; }
double SliceServeRecord::strike_lo() const noexcept { return impl_->strike_lo; }
double SliceServeRecord::strike_hi() const noexcept { return impl_->strike_hi; }

ServeAnswer SliceServeRecord::serve(const ServeRequest &request) const {
  const Impl &impl = *impl_;
  ServeAnswer a;
  a.convention.surface_version = impl.policy.surface_version;
  const ServeGreekMask mask = request.greeks;
  if (!request_valid(request.strike, request.side, request.spot, request.tier,
                     request.tier_max)) {
    refuse(a, ServeStatus::InvalidArgument, ServeRoute::None, mask);
    return a;
  }
  ServeStatus stale = ServeStatus::Ok;
  ServeRoute stale_route = ServeRoute::None;
  if (!fresh(impl, request.now_ns, a.age_ns, stale, stale_route)) {
    refuse(a, stale, stale_route, mask);
    return a;
  }
  if (impl.dated_cash) {
    refuse(a, ServeStatus::TierUnavailable, ServeRoute::ColdPath, mask);
    return a;
  }
  if (!(request.strike >= impl.strike_lo && request.strike <= impl.strike_hi)) {
    refuse(a, ServeStatus::OutOfDomain, ServeRoute::Refit, mask);
    return a;
  }
  const Contract c = resolve(impl, request.strike, request.side, request.spot);
  if (c.served != nullptr && c.served->status == QuoteSliceEvaluationStatus::PriceOnly) {
    refuse(a, ServeStatus::PriceOnly, c.at_fit ? ServeRoute::None : ServeRoute::Refit, mask);
    a.price_only_reason = c.served->reason; // C0's reason and interval travel with the answer
    a.iv_lower = c.served->iv_lower;
    a.iv_upper = c.served->iv_upper;
    if (c.at_fit) {
      a.price = c.served->price;
      a.price_source = ServePriceSource::SliceNode;
      a.tier = ServeTier::Certified;
      a.certificate_usd = 0.0;
      a.certificate_class = ServeBoundClass::Exact;
    }
    return a;
  }
  bool box = false;
  const auto first = static_cast<std::uint8_t>(request.tier);
  const auto last = static_cast<std::uint8_t>(last_servable(request.tier_max));
  for (std::uint8_t t = first; t <= last; ++t) {
    ServeAnswer tried = a;
    bool done = false;
    switch (static_cast<ServeTier>(t)) {
    case ServeTier::Ladder:
      done = serve_ladder(impl, c, mask, tried);
      break;
    case ServeTier::Family:
      done = serve_family(impl, c, request, tried, box);
      break;
    case ServeTier::Certified:
      done = serve_certified(impl, c, request, tried, box);
      break;
    case ServeTier::Cold:
      break;
    }
    if (done) {
      tried.sigma = c.sigma;
      return tried;
    }
  }
  refuse(a, box ? ServeStatus::SigmaOutOfBox : ServeStatus::TierUnavailable, ServeRoute::ColdPath,
         mask);
  return a;
}

ServeIvAnswer SliceServeRecord::implied_vol(const ServeIvRequest &request) const {
  const Impl &impl = *impl_;
  ServeIvAnswer a;
  if (!request_valid(request.strike, request.side, request.spot, request.tier,
                     request.tier_max) ||
      !std::isfinite(request.target_price)) {
    a.status = ServeStatus::InvalidArgument;
    a.route = ServeRoute::None;
    return a;
  }
  if (!fresh(impl, request.now_ns, a.age_ns, a.status, a.route)) return a;
  // A Cold request is a route, never a solve inside the record (review-H0A m5).
  if (request.tier == ServeTier::Cold || impl.dated_cash) return a; // TierUnavailable
  if (!(request.strike >= impl.strike_lo && request.strike <= impl.strike_hi)) {
    a.status = ServeStatus::OutOfDomain;
    a.route = ServeRoute::Refit;
    return a;
  }
  const Contract c = resolve(impl, request.strike, request.side, request.spot);
  // The L1c gate in front of every inversion (american_iv.hpp al_non_monotone_gate).
  const double zero_vol = zero_vol_american(c.spot, c.strike, impl.tau, impl.rate, c.q, c.side);
  if (!(request.target_price - zero_vol > al_non_monotone_gate(c.strike, impl.tau, impl.al_opts))) {
    a.status = ServeStatus::NonMonotoneBand;
    return a;
  }
  const auto first = static_cast<std::uint8_t>(request.tier);
  const auto last = static_cast<std::uint8_t>(last_servable(request.tier_max));
  bool box = false;
  bool unconfirmed = false;
  if (first == 0u && ladder_iv(impl, c, request.target_price, a, box, unconfirmed)) return a;
  const AlSigmaQFamily *f = impl.family(c.side);
  if (last < 1u || f == nullptr) {
    a.status = unconfirmed ? ServeStatus::Unconfirmed
                           : (box ? ServeStatus::SigmaOutOfBox : ServeStatus::TierUnavailable);
    return a;
  }
  const Root r = family_root(*f, c, request.target_price, impl.policy.iv_tolerance);
  take_root(a, r);
  if (r.outcome != RootOutcome::Converged) {
    const bool out_of_box = r.outcome == RootOutcome::BelowBox ||
                            r.outcome == RootOutcome::AboveBox;
    a.status = out_of_box ? ServeStatus::SigmaOutOfBox : ServeStatus::NoConvergence;
    return a;
  }
  a.tier = ServeTier::Family;
  if (first <= 1u && family_enclosure(impl, *f, c, r.sigma, request.target_price, a.acceptance)) {
    a.status = ServeStatus::Ok;
    a.route = ServeRoute::None;
    return a;
  }
  if (last < 2u) {
    a.status = ServeStatus::Unconfirmed; // no validated bound: a confirm must accept it
    return a;
  }
  // Certified: one AlProfile::Fast confirm, accepted on its Newton distance (K3's
  // NewtonContract), served Newton-corrected.
  const Result<double> fast = andersen_lake(c.spot, c.strike, impl.tau, r.sigma, impl.rate, c.q,
                                            c.side, impl.fast_opts);
  a.evaluations = static_cast<std::uint8_t>(std::min(255, a.evaluations + 1));
  if (!fast || !(r.slope > 0.0)) {
    a.status = ServeStatus::NoConvergence;
    return a;
  }
  const double distance = (*fast - request.target_price) / r.slope;
  a.tier = ServeTier::Certified;
  a.price_residual = *fast - request.target_price;
  a.newton_distance_vol = distance;
  if (!(std::abs(distance) <= impl.policy.iv_tolerance)) {
    a.status = ServeStatus::NoConvergence;
    return a;
  }
  a.sigma = r.sigma - distance;
  a.status = ServeStatus::Ok;
  a.route = ServeRoute::None;
  a.acceptance.kind = ServeAcceptanceKind::Confirmed;
  a.acceptance.reason = ServeAcceptanceReason::NewtonContract;
  a.acceptance.error_class = ServeErrorClass::Uncovered;
  return a;
}

// ── Free functions ─────────────────────────────────────────────────────────────────────

Status validate_serve_record_policy(const ServeRecordPolicy &p) {
  const bool ok =
      (p.families == ServeFamilyMode::None || p.families == ServeFamilyMode::Build) &&
      std::isfinite(p.sigma_lo_mult) && p.sigma_lo_mult > 0.0 && p.sigma_lo_mult <= 1.0 &&
      std::isfinite(p.sigma_hi_mult) && p.sigma_hi_mult >= 1.0 &&
      p.ladder_spot_nodes >= 4 && p.ladder_spot_nodes <= kLadderMaxSpot &&
      p.ladder_sigma_nodes >= 3 && p.ladder_sigma_nodes <= kLadderMaxSigma &&
      std::isfinite(p.ladder_window_sigmas) && p.ladder_window_sigmas > 0.0 &&
      std::isfinite(p.ladder_window_min) && p.ladder_window_min > 0.0 &&
      std::isfinite(p.ladder_window_max) && p.ladder_window_max >= p.ladder_window_min &&
      p.ladder_window_max <= 0.5 && std::isfinite(p.ladder_sigma_half_width) &&
      p.ladder_sigma_half_width > 0.0 && p.ladder_sigma_half_width <= 0.9 &&
      p.ladder_max_cert_usd >= 0.0 && std::isfinite(p.ladder_max_cert_usd) &&
      p.ladder_max_cert_rel >= 0.0 && std::isfinite(p.ladder_max_cert_rel) &&
      p.max_age_ns >= 0 && std::isfinite(p.iv_tolerance) && p.iv_tolerance > 0.0;
  if (!ok) return Err(ErrorCode::InvalidArgument, "serve record: policy field out of range");
  return Ok();
}

ServeRecordPolicy serve_record_policy_tick() noexcept {
  ServeRecordPolicy p;
  p.ladder_spot_nodes = 16;
  p.ladder_sigma_nodes = 8;
  p.ladder_sigma_half_width = 0.10;
  p.ladder_window_max = 0.02;
  return p;
}

Result<std::shared_ptr<const SliceServeRecord>> build_serve_record(const QuoteSliceFit &fit,
                                                                  const ServeRecordPolicy &policy) {
  return build_record(fit, policy, nullptr);
}

const char *to_string(ServeTier value) noexcept {
  switch (value) {
  case ServeTier::Ladder: return "ladder";
  case ServeTier::Family: return "family";
  case ServeTier::Certified: return "certified";
  case ServeTier::Cold: return "cold";
  }
  return "unknown";
}

const char *to_string(ServeStatus value) noexcept {
  switch (value) {
  case ServeStatus::Ok: return "ok";
  case ServeStatus::Exercised: return "exercised";
  case ServeStatus::PriceOnly: return "price_only";
  case ServeStatus::OutOfDomain: return "out_of_domain";
  case ServeStatus::SigmaOutOfBox: return "sigma_out_of_box";
  case ServeStatus::TierUnavailable: return "tier_unavailable";
  case ServeStatus::Stale: return "stale";
  case ServeStatus::NoConvergence: return "no_convergence";
  case ServeStatus::NonMonotoneBand: return "non_monotone_band";
  case ServeStatus::Unconfirmed: return "unconfirmed";
  case ServeStatus::InvalidArgument: return "invalid_argument";
  case ServeStatus::BudgetExhausted: return "budget_exhausted";
  case ServeStatus::Invalidated: return "invalidated";
  }
  return "unknown";
}

const char *to_string(ServeRoute value) noexcept {
  switch (value) {
  case ServeRoute::None: return "none";
  case ServeRoute::ColdPath: return "cold_path";
  case ServeRoute::Rebuild: return "rebuild";
  case ServeRoute::Refit: return "refit";
  }
  return "unknown";
}

const char *to_string(ServeGreekReason value) noexcept {
  switch (value) {
  case ServeGreekReason::NotRequested: return "not_requested";
  case ServeGreekReason::Served: return "served";
  case ServeGreekReason::Exercised: return "exercised";
  case ServeGreekReason::Refused: return "refused";
  case ServeGreekReason::NoCarryBumps: return "no_carry_bumps";
  case ServeGreekReason::CarryBumpRefused: return "carry_bump_refused";
  case ServeGreekReason::NoQAxis: return "no_q_axis";
  case ServeGreekReason::NearNodeDegenerate: return "near_node_degenerate";
  case ServeGreekReason::ConventionUnsupported: return "convention_unsupported";
  case ServeGreekReason::NearExerciseEdge: return "near_exercise_edge";
  case ServeGreekReason::BundleUnavailable: return "bundle_unavailable";
  case ServeGreekReason::TierHasNoGreek: return "tier_has_no_greek";
  case ServeGreekReason::NoMap: return "no_map";
  }
  return "unknown";
}

const char *to_string(ServeUnsupportedReason value) noexcept {
  switch (value) {
  case ServeUnsupportedReason::None: return "none";
  case ServeUnsupportedReason::DatedCash: return "dated_cash";
  case ServeUnsupportedReason::BelowFamilyTau: return "below_family_tau";
  case ServeUnsupportedReason::FamilyRefused: return "family_refused";
  case ServeUnsupportedReason::FamiliesNotBuilt: return "families_not_built";
  case ServeUnsupportedReason::NoServedVolatility: return "no_served_volatility";
  case ServeUnsupportedReason::HandoffMismatch: return "handoff_mismatch";
  }
  return "unknown";
}

const char *to_string(ServeBoundClass value) noexcept {
  switch (value) {
  case ServeBoundClass::None: return "none";
  case ServeBoundClass::Exact: return "exact";
  case ServeBoundClass::RefinementValidated: return "refinement_validated";
  case ServeBoundClass::Sampled: return "sampled";
  case ServeBoundClass::KernelTested: return "kernel_tested";
  }
  return "unknown";
}

namespace detail {

void stamp_serve_handoff(QuoteSliceFit &out, const DeAmSigmaFamilies *families) {
  if (families == nullptr || out.fit_status != QuoteSliceFitStatus::Success) {
    out.serve_handoff.reset();
    return;
  }
  auto handoff = std::make_shared<QuoteSliceServeHandoff>();
  bool any = false;
  for (const Side side : {Side::Put, Side::Call}) {
    const AlSigmaQFamily *f = families->of_q(side);
    if (f == nullptr) continue;
    const AlExpiryGeometry &g =
        side == Side::Put ? families->q_geometry_put : families->q_geometry_call;
    QuoteSliceServeHandoff::Family &h = handoff->sides[side == Side::Put ? 0u : 1u];
    h.present = true;
    h.geometry_T = g.T();
    h.geometry_r = g.r();
    h.geometry_q = g.q();
    h.anchor = f->anchor();
    h.sigma_lo = f->sigma_lo();
    h.sigma_hi = f->sigma_hi();
    h.n_sigma = f->n_sigma();
    h.q_lo = f->q_lo();
    h.q_hi = f->q_hi();
    h.n_q = f->n_q();
    if (const ValidatedErrorEnvelope *env = families->q_envelope(side)) {
      h.envelope = *env;
      h.envelope.family = nullptr;
    }
    h.q_bound = families->q_bound(side);
    // Two of the fitter family's own prices off its nodes (the identity the rebuild must
    // reproduce): at the anchor strike mid-box, and 5 % out of the money low in the box.
    const double moneyness = side == Side::Put ? 0.95 : 1.05;
    const std::array<QuoteSliceServeHandoff::Probe, 2> at{
        QuoteSliceServeHandoff::Probe{h.anchor, std::sqrt(h.sigma_lo * h.sigma_hi),
                                      0.5 * (h.q_lo + h.q_hi), kNaN},
        QuoteSliceServeHandoff::Probe{h.anchor * moneyness,
                                      h.sigma_lo * std::pow(h.sigma_hi / h.sigma_lo, 0.3),
                                      h.q_lo + 0.7 * (h.q_hi - h.q_lo), kNaN}};
    for (std::size_t i = 0; i < at.size(); ++i) {
      h.probes[i] = at[i];
      const AlSigmaQFamilyPrice fp = f->price(h.anchor, at[i].K, at[i].sigma, at[i].q);
      h.probes[i].price = fp.status == AlSigmaQFamilyStatus::Ok ? fp.price : kNaN;
    }
    any = true;
  }
  if (any) {
    out.serve_handoff = std::move(handoff);
  } else {
    out.serve_handoff.reset();
  }
}

std::shared_ptr<const DeAmSigmaFamilies> build_serve_families(const QuoteSliceFit &fit,
                                                              const ServeRecordPolicy &policy) {
  if (fit.fit_status != QuoteSliceFitStatus::Success || !fit.curve || !fit.prices ||
      fit.prices->nodes().empty() || fit.dividend_model == QuoteSliceDividendModel::DatedCash) {
    return nullptr;
  }
  Impl impl;
  copy_basics(impl, fit, policy);
  identify_nodes(impl, fit);
  return build_families(impl.nodes, family_inputs(fit), policy, nullptr).holder;
}

Result<std::shared_ptr<const SliceServeRecord>>
build_serve_record_with(const QuoteSliceFit &fit, const ServeRecordPolicy &policy,
                        std::shared_ptr<const DeAmSigmaFamilies> families) {
  return build_record(fit, policy, std::move(families));
}

Result<std::shared_ptr<const SliceServeRecord>>
build_serve_record_on(const QuoteSliceFit &fit, const ServeRecordPolicy &policy,
                      const SliceServeRecord &first) {
  const Impl &held = SliceServeRecordAccess::impl(first);
  if (!held.families) return build_record(fit, policy, nullptr);
  // side_slot order (call, put).
  const std::array<ServeFamilyOrigin, 2> kept{held.support.call.origin, held.support.put.origin};
  return build_record(fit, policy, held.families, &kept);
}

std::optional<ServeRecordContract> serve_record_contract(const SliceServeRecord &record,
                                                         double strike, Side side) {
  const Impl &impl = SliceServeRecordAccess::impl(record);
  if (!positive(strike) || !valid_side(side) ||
      !(strike >= impl.strike_lo && strike <= impl.strike_hi)) {
    return std::nullopt;
  }
  const Contract c = resolve(impl, strike, side, impl.spot);
  return ServeRecordContract{impl.spot, impl.tau, impl.rate, c.q, impl.al_opts};
}

std::optional<QuoteSliceValue> serve_record_value(const QuoteSliceFit &fit, double strike,
                                                  Side side) {
  const SliceServeRecord *record = fit.serve_record.get();
  if (record == nullptr || fit.fit_status != QuoteSliceFitStatus::Success || !fit.prices ||
      !fit.curve || !positive(strike) || !valid_side(side)) {
    return std::nullopt;
  }
  const Impl &impl = SliceServeRecordAccess::impl(*record);
  const auto nodes = fit.prices->nodes();
  // The record must be THIS fit's: the market and clock, and the certified node prices
  // that bracket the strike, bit for bit.
  if (bits_of(impl.spot) != bits_of(fit.market.spot) ||
      bits_of(impl.tau) != bits_of(fit.market.variance_years) ||
      bits_of(impl.rate) != bits_of(fit.diagnostics.effective_rate) ||
      impl.nodes.size() != nodes.size() ||
      !(strike >= impl.strike_lo && strike <= impl.strike_hi)) {
    return std::nullopt;
  }
  const auto at = std::lower_bound(nodes.begin(), nodes.end(), strike,
      [](const AmericanPriceNode &n, double k) { return n.strike < k; });
  const auto hi = static_cast<std::size_t>(at - nodes.begin());
  const bool is_node = at != nodes.end() && at->strike == strike;
  const auto same_node = [&](std::size_t i) {
    const double mine = side == Side::Call ? impl.nodes[i].call.price : impl.nodes[i].put.price;
    const double fits = side == Side::Call ? nodes[i].call : nodes[i].put;
    return bits_of(impl.nodes[i].strike) == bits_of(nodes[i].strike) &&
           bits_of(mine) == bits_of(fits);
  };
  if (hi >= nodes.size() || !same_node(hi) || (!is_node && (hi == 0 || !same_node(hi - 1)))) {
    return std::nullopt;
  }
  if (is_node) {
    const std::uint8_t bit = side == Side::Call ? 1u : 2u;
    // An unmoved node serves the curve's volatility (no inversion to save); a price-only
    // node side is never identified.
    if (!flagged(fit.moved_nodes, hi, bit) || flagged(fit.price_only_nodes, hi, bit)) {
      return std::nullopt;
    }
  }
  if (!detail::serves(fit, strike, side)) return std::nullopt; // withheld: the query types it
  const std::optional<double> price = fit.prices->price(strike, side);
  if (!price) return std::nullopt;
  ServeIvRequest r;
  r.strike = strike;
  r.side = side;
  r.spot = impl.spot;
  r.target_price = *price;
  r.tier = ServeTier::Family;
  r.tier_max = ServeTier::Family;
  const ServeIvAnswer iv = record->implied_vol(r);
  if (iv.status != ServeStatus::Ok || iv.acceptance.kind != ServeAcceptanceKind::Skipped ||
      iv.acceptance.error_class != ServeErrorClass::RefinementValidated ||
      !(iv.sigma - iv.acceptance.bound_iv > kIvMin)) {
    return std::nullopt;
  }
  return QuoteSliceValue{iv.sigma, *price};
}

} // namespace detail

} // namespace atx::vol
