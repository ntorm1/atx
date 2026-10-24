// risk_kernel_family — the HFT kernel. Per (uid, expiry) block: resolve every lane,
// prepare ONE Andersen-Lake geometry (T, r, q_eff) shared by the call and put groups;
// per group: one AlSigmaFamily over the group's sigma box (+ a carry-bump set only
// when rho is needed), then family_greeks per lane. Lanes the family cannot serve
// (outside the box, tau below the family floor, near the exercise edge, a refused
// requested Greek) are priced cold in a second pass over the same block, and so is
// every lane of a group whose family would cost more than pricing its lanes cold
// (FamilyKernelPolicy "FAMILY ECONOMICS"; flagged FamilyUneconomic). Every block prepares
// its geometry, also below the family's tau floor (no family there: ShortTenor), and the
// cold pass prices a lane ON it when the lane's carry is the block's -- bit-identical to
// the fresh american_greeks_al (LEDGER 2026-10-19), minus the per-lane setup.

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>

#include "atx/vol/api/pricing/american.hpp"
#include "atx/vol/api/pricing/pricing_executor.hpp"
#include "risk/risk_kernel_detail.hpp"

namespace atx::vol::risk_detail {

namespace {

using Mask = AlSigmaFamilyGreeksMask;
using Clock = std::chrono::steady_clock;

[[nodiscard]] bool same_bits(double a, double b) noexcept {
  return std::bit_cast<std::uint64_t>(a) == std::bit_cast<std::uint64_t>(b);
}

[[nodiscard]] double seconds_since(Clock::time_point t0) noexcept {
  return std::chrono::duration<double>(Clock::now() - t0).count();
}

// Price, delta, gamma, vega and theta are always served (risk_types.hpp RiskNeeds).
[[nodiscard]] Mask mask_of(RiskNeeds needs) noexcept {
  auto bits = static_cast<std::uint16_t>(Mask::kDelta | Mask::kGamma | Mask::kVega | Mask::kTheta);
  if (needs.second_order) {
    bits = static_cast<std::uint16_t>(bits | Mask::kVanna | Mask::kVolga);
  }
  if (needs.rho) {
    bits = static_cast<std::uint16_t>(bits | Mask::kRho);
  }
  if (needs.charm) {
    bits = static_cast<std::uint16_t>(bits | Mask::kCharm);
  }
  return Mask{bits};
}

enum class Verdict : std::uint8_t { Family, Exercised, Cold };

struct LaneVerdict {
  Verdict verdict{Verdict::Cold};
  RiskFlags flags{0};
};

[[nodiscard]] bool requested_finite(const AlSigmaFamilyGreeks &g, Mask mask) noexcept {
  return std::isfinite(g.price) && std::isfinite(g.delta) && std::isfinite(g.gamma) &&
         std::isfinite(g.vega) && std::isfinite(g.theta) &&
         (!mask.has(Mask::kRho) || std::isfinite(g.rho)) &&
         (!mask.has(Mask::kVanna) || std::isfinite(g.vanna)) &&
         (!mask.has(Mask::kVolga) || std::isfinite(g.volga)) &&
         (!mask.has(Mask::kCharm) || std::isfinite(g.charm));
}

// Whether the family's answer for one lane is served as is, as the exercise row, or
// handed to the cold route (with the flags saying why).
[[nodiscard]] LaneVerdict judge(const AlSigmaFamilyGreeks &g, Mask mask,
                                bool cold_near_edge) noexcept {
  if (g.status != AlSigmaFamilyStatus::Ok) {
    RiskFlags flags = static_cast<RiskFlags>(RiskFlag::ColdFallback);
    if (g.status == AlSigmaFamilyStatus::SigmaOutsideBox ||
        g.status == AlSigmaFamilyStatus::SigmaBelowGuard) {
      flags |= RiskFlag::OutsideFamilyBox;
    }
    return {Verdict::Cold, flags};
  }
  // The exercise Greeks are exact (delta = +-1, the rest 0) even inside the edge band.
  if (g.exercised) {
    return {Verdict::Exercised, 0u};
  }
  if (g.near_edge && cold_near_edge) {
    return {Verdict::Cold, RiskFlag::ColdFallback | RiskFlag::NearExerciseEdge};
  }
  bool refused = false;
  RiskFlags flags = 0u;
  for (std::size_t i = 0; i < kAlSigmaFamilyGreekCount; ++i) {
    // Mask bit i names the same Greek as AlSigmaFamilyGreek index i (american.hpp).
    if (!mask.has(static_cast<std::uint16_t>(1u << i))) {
      continue;
    }
    const AlSigmaFamilyGreekReason why = g.reason[i];
    if (why == AlSigmaFamilyGreekReason::Served || why == AlSigmaFamilyGreekReason::Exercised) {
      continue;
    }
    refused = true;
    if (why == AlSigmaFamilyGreekReason::NearExerciseEdge) {
      flags |= RiskFlag::NearExerciseEdge;
    }
  }
  if (refused || !requested_finite(g, mask)) {
    return {Verdict::Cold, flags | RiskFlag::ColdFallback};
  }
  return {Verdict::Family, 0u};
}

void write_family(RiskFrame &f, std::size_t r, const AlSigmaFamilyGreeks &g,
                  RiskNeeds needs) noexcept {
  f.price[r] = g.price;
  f.delta[r] = g.delta;
  f.gamma[r] = g.gamma;
  f.vega[r] = g.vega;
  f.theta[r] = g.theta;
  f.rho[r] = needs.rho ? g.rho : 0.0;
  f.vanna[r] = needs.second_order ? g.vanna : 0.0;
  f.volga[r] = needs.second_order ? g.volga : 0.0;
  f.charm[r] = needs.charm ? g.charm : 0.0;
  f.route[r] = RiskRoute::Family;
}

// Intrinsic as the family serves it, with the exercise Greeks.
void write_exercised(RiskFrame &f, std::size_t r, const AlSigmaFamilyGreeks &g,
                     Side side) noexcept {
  f.price[r] = g.price;
  f.delta[r] = side == Side::Put ? -1.0 : 1.0;
  f.gamma[r] = f.vega[r] = f.theta[r] = f.rho[r] = 0.0;
  f.vanna[r] = f.volga[r] = f.charm[r] = 0.0;
  f.route[r] = RiskRoute::Exercised;
}

struct BlockCtx {
  const PreparedRiskBook &book;
  const MarketState &state;
  RiskNeeds needs;
  const FamilyKernelPolicy &policy;
  RiskFrame &out;
  WorkspaceState &ws;
  Mask mask;
};

// Hand row `r` to the cold pass, or refuse it when the policy has no cold route.
void send_cold(const BlockCtx &c, std::size_t r, RiskFlags flags) noexcept {
  if (c.policy.cold_fallback) {
    c.out.flags[r] |= flags;
    c.ws.cold[r] = 1u;
    return;
  }
  c.out.flags[r] |= flags & ~static_cast<RiskFlags>(RiskFlag::ColdFallback);
  write_refused(c.out, r, RiskLaneStatus::Refused);
}

// The block geometry's carry inputs (bit-equal on every lane of a well-formed block;
// a lane that disagrees is priced cold rather than on another lane's geometry).
struct BlockCarry {
  double T{0.0};
  double rate{0.0};
  double q_eff{0.0};
  bool geometry_ok{false}; // T >= kAlSigmaFamilyMinTau: a family may be built on it
};

// Whether row `r` resolved to exactly the block's carry (it may then use the geometry).
[[nodiscard]] bool on_block_carry(const RiskFrame &f, std::size_t r,
                                  const BlockCarry &carry) noexcept {
  return same_bits(f.T[r], carry.T) && same_bits(f.rate[r], carry.rate) &&
         same_bits(f.q_eff[r], carry.q_eff);
}

[[nodiscard]] bool eligible(const BlockCtx &c, std::size_t r) noexcept {
  return c.out.status[r] == RiskLaneStatus::Ok && c.ws.cold[r] == 0u;
}

// FamilyKernelPolicy "ADAPTIVE NODE COUNT": ceil(log(hi / lo) / spacing) + 1 clamped to
// [floor, n_sigma], floor = min(n_sigma_min, n_sigma) (the ceiling wins). Clamped in
// double before the cast (an out-of-range float-to-integer conversion is UB); a
// non-finite ratio takes the floor count.
[[nodiscard]] std::uint16_t node_count(const FamilyKernelPolicy &p, double lo,
                                       double hi) noexcept {
  const std::uint16_t floor = std::min(p.n_sigma_min, p.n_sigma);
  const double want = std::ceil(std::log(hi / lo) / p.sigma_node_spacing) + 1.0;
  if (!(want > static_cast<double>(floor))) {
    return floor;
  }
  if (want >= static_cast<double>(p.n_sigma)) {
    return p.n_sigma;
  }
  return static_cast<std::uint16_t>(want);
}

// FamilyKernelPolicy "FAMILY ECONOMICS": whether a family of `n_nodes` sigma nodes costs at
// least the cold-analytic route of its `n_eligible` lanes. Under rho a carry set builds two
// rate-bumped families beside the base one (x3) and a cold lane adds two rate-bumped solves
// to its five (x7/5). Reads only the group's shape and the policy constants, never a clock,
// so the decision is the same for every thread count. Off without an analytic cold route
// (no cold route at all, or the FD one, which the constants do not describe).
[[nodiscard]] bool family_uneconomic(const BlockCtx &c, std::uint32_t n_eligible,
                                     std::uint16_t n_nodes) noexcept {
  const FamilyKernelPolicy &p = c.policy;
  if (!(p.family_economics && p.cold_fallback && p.analytic_cold)) {
    return false;
  }
  constexpr double kRhoFamilyScale = 3.0;
  constexpr double kRhoColdScale = 7.0 / 5.0;
  const double family_us = static_cast<double>(n_nodes) * p.family_node_cost_us *
                           (c.needs.rho ? kRhoFamilyScale : 1.0);
  const double cold_us = static_cast<double>(n_eligible) * p.cold_lane_cost_us *
                         (c.needs.rho ? kRhoColdScale : 1.0);
  return family_us >= cold_us;
}

void family_group(const BlockCtx &c, std::size_t b, std::uint32_t gi, const BlockCarry &carry,
                  double S, BlockCounters &bc) {
  const RiskGroup &grp = c.book.groups()[gi];
  const std::span<const double> strike = c.book.strike();
  RiskFrame &f = c.out;
  double smin = std::numeric_limits<double>::infinity();
  double smax = -std::numeric_limits<double>::infinity();
  std::uint32_t n_eligible = 0;
  for (std::uint32_t r = grp.begin; r < grp.end; ++r) {
    if (f.status[r] != RiskLaneStatus::Ok) {
      continue;
    }
    if (!(carry.geometry_ok && on_block_carry(f, r, carry))) {
      // No family can serve it: below the family's tau floor (ShortTenor), or off the
      // block's geometry.
      const RiskFlags why = f.T[r] < kAlSigmaFamilyMinTau
                                ? RiskFlag::ColdFallback | RiskFlag::ShortTenor
                                : static_cast<RiskFlags>(RiskFlag::ColdFallback);
      send_cold(c, r, why);
      continue;
    }
    smin = std::min(smin, f.sigma[r]);
    smax = std::max(smax, f.sigma[r]);
    ++n_eligible;
  }
  if (n_eligible == 0u) {
    return;
  }

  // Sigma box: the group's resolved range padded, never below the floor nor the
  // family's own guard.
  const double floor = std::max(c.policy.sigma_box_floor, kAlSigmaFamilyMinSigma);
  double lo = std::max(smin * (1.0 - c.policy.sigma_box_pad), floor);
  double hi = smax * (1.0 + c.policy.sigma_box_pad);
  if (smax < lo) { // every lane below the floor: nothing for a family to serve
    for (std::uint32_t r = grp.begin; r < grp.end; ++r) {
      if (eligible(c, r)) {
        send_cold(c, r, RiskFlag::ColdFallback | RiskFlag::OutsideFamilyBox);
      }
    }
    return;
  }
  // A degenerate box (one sigma and pad 0, or a range squeezed onto the floor) cannot
  // be prepared: widen it SYMMETRICALLY by +-pad around its centre, floor-clamped.
  // pad 0 has no width to offer, so the degenerate case alone falls back to 5 %.
  if (!(hi - lo > 1.0e-9)) {
    constexpr double kDegeneratePad = 0.05;
    const double pad = c.policy.sigma_box_pad > 0.0 ? c.policy.sigma_box_pad : kDegeneratePad;
    const double centre = std::max(smax, lo);
    lo = std::max(centre * (1.0 - pad), floor);
    hi = centre * (1.0 + pad);
  }
  const std::uint16_t n_nodes = node_count(c.policy, lo, hi);
  if (family_uneconomic(c, n_eligible, n_nodes)) {
    for (std::uint32_t r = grp.begin; r < grp.end; ++r) {
      if (eligible(c, r)) {
        send_cold(c, r, static_cast<RiskFlags>(RiskFlag::FamilyUneconomic));
      }
    }
    return;
  }

  // The anchor: spot for a call family (McDonald-Schroder internal strike). For a put
  // family the group's MEDIAN eligible strike: the family serves the other strikes by
  // its T8 homogeneity rescale at a few ULP from a fresh solve (american.hpp
  // "THE ANCHOR"; accepted here), and a median anchor keeps that rescale short. A
  // strike anchor is also stable across states, so a put family re-prepares in place.
  double anchor = S;
  if (grp.side == Side::Put) {
    const std::uint32_t target = n_eligible / 2u;
    std::uint32_t k = 0;
    for (std::uint32_t r = grp.begin; r < grp.end; ++r) {
      if (!eligible(c, r)) {
        continue;
      }
      if (k == target) {
        anchor = strike[r];
        break;
      }
      ++k;
    }
  }

  std::optional<AlSigmaFamily> &slot = c.ws.family[gi];
  const Clock::time_point t_build = Clock::now();
  if (!(slot.has_value() && slot->side() == grp.side && same_bits(slot->anchor(), anchor))) {
    slot.emplace(c.ws.geometry[b], grp.side, anchor);
  }
  const Status built = slot->prepare(lo, hi, n_nodes, AlSigmaFamilyAxis::LogSigma);
  bool carry_ok = true;
  if (built.has_value()) {
    ++bc.families_built;
    bc.family_nodes += n_nodes;
    if (c.needs.rho) {
      carry_ok = c.ws.carry[gi].prepare(*slot, Mask{Mask::kRho}).has_value();
      if (carry_ok) {
        ++bc.carry_sets_built;
      }
    }
  }
  bc.family_build_seconds += seconds_since(t_build);
  if (!built.has_value() || !carry_ok) {
    for (std::uint32_t r = grp.begin; r < grp.end; ++r) {
      if (eligible(c, r)) {
        send_cold(c, r, static_cast<RiskFlags>(RiskFlag::ColdFallback));
      }
    }
    return;
  }

  for (std::uint32_t r = grp.begin; r < grp.end; ++r) {
    if (!eligible(c, r)) {
      continue;
    }
    const AlSigmaFamilyGreeks g =
        c.needs.rho ? family_greeks(*slot, c.ws.carry[gi], S, strike[r], f.sigma[r], c.mask)
                    : family_greeks(*slot, S, strike[r], f.sigma[r], c.mask);
    const LaneVerdict v = judge(g, c.mask, c.policy.cold_near_edge);
    switch (v.verdict) {
    case Verdict::Family:
      write_family(f, r, g, c.needs);
      break;
    case Verdict::Exercised:
      write_exercised(f, r, g, grp.side);
      break;
    case Verdict::Cold:
      send_cold(c, r, v.flags);
      break;
    }
  }
}

void process_block(const BlockCtx &c, std::size_t b) {
  const ExpiryBlock blk = c.ws.blocks[b];
  BlockCounters &bc = c.ws.block_counters[b];
  bc = BlockCounters{};
  const std::span<const RiskGroup> groups = c.book.groups();
  const std::span<const double> strike = c.book.strike();

  // 1. Resolve every lane (allocation-free); the first Ok lane fixes the geometry.
  BlockCarry carry;
  bool any_ok = false;
  for (std::uint32_t gi = blk.group_begin; gi < blk.group_end; ++gi) {
    const RiskGroup &grp = groups[gi];
    for (std::uint32_t r = grp.begin; r < grp.end; ++r) {
      c.ws.cold[r] = 0u;
      const ResolvedContract rc = c.state.resolve(grp.uid, strike[r], grp.expiry_ns, grp.side);
      write_resolved(c.out, r, rc);
      if (rc.status == RiskLaneStatus::Ok && !any_ok) {
        any_ok = true;
        carry.T = rc.T;
        carry.rate = rc.rate;
        carry.q_eff = rc.q_eff;
      }
    }
  }
  if (!any_ok) {
    return; // nothing to price: skip the block's family work entirely
  }
  const std::uint32_t uid = groups[blk.group_begin].uid;
  const double S = c.state.underlier(uid)->spot; // an Ok lane implies the underlier
  const AlOpts opts = resolve_al_opts(c.policy.al_opts, c.state, uid);

  // 2. One geometry per (uid, expiry), prepared in place (no allocation once warm). Also
  // below the family's tau floor (~4 us), where it serves the cold pass alone; only the
  // family-capable blocks bill it to family_build_seconds.
  carry.geometry_ok = carry.T >= kAlSigmaFamilyMinTau;
  const Clock::time_point t0 = Clock::now();
  c.ws.geometry[b].prepare(carry.T, carry.rate, carry.q_eff, std::optional<AlOpts>{opts});
  if (carry.geometry_ok) {
    bc.family_build_seconds += seconds_since(t0);
  }

  // 3. Per group: the family and its lanes.
  for (std::uint32_t gi = blk.group_begin; gi < blk.group_end; ++gi) {
    family_group(c, b, gi, carry, S, bc);
  }

  // 4. Second pass: the block's cold list on the resolved inputs, on the block geometry
  // when the lane's carry is the block's.
  const AlExpiryGeometry &geometry = c.ws.geometry[b];
  for (std::uint32_t gi = blk.group_begin; gi < blk.group_end; ++gi) {
    const RiskGroup &grp = groups[gi];
    for (std::uint32_t r = grp.begin; r < grp.end; ++r) {
      if (c.ws.cold[r] != 0u) {
        const AlExpiryGeometry *geo = on_block_carry(c.out, r, carry) ? &geometry : nullptr;
        cold_row(c.out, r, S, strike[r], grp.side, opts, c.policy.analytic_cold, c.needs, geo);
      }
    }
  }
}

} // namespace

void evaluate_family(const PreparedRiskBook &book, const MarketState &state, RiskNeeds needs,
                     const FamilyKernelPolicy &policy, RiskFrame &out, WorkspaceState &ws) {
  const BlockCtx c{book, state, needs, policy, out, ws, mask_of(needs)};
  // Each block is owned by exactly one worker: it writes only its own rows, its own
  // geometry / family / carry slots and its own counters, so the frame depends neither
  // on the thread count nor on which worker claims which block (the schedule).
  if (policy.dynamic_schedule) {
    pricing_executor().run_dynamic(ws.blocks.size(), policy.n_threads,
                                   [&c](std::size_t b, unsigned) { process_block(c, b); });
    return;
  }
  pricing_executor().run_blocks(ws.blocks.size(), policy.n_threads,
                                [&c](std::size_t b) { process_block(c, b); });
}

} // namespace atx::vol::risk_detail
