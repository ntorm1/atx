// RiskKernel — the strategy wrapper: call validation, frame / workspace binding, the
// per-uid and per-share rows, the counters; plus the helpers both kernels share.

#include "atx/vol/api/risk/risk_kernel.hpp"

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <utility>

#include "risk/expiry_clock.hpp"
#include "risk/risk_kernel_detail.hpp"

namespace atx::vol {

using atx::core::Err;
using atx::core::Ok;

namespace risk_detail {

namespace {
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
// The family's own "served price is intrinsic" tolerance (american.hpp, the exercise
// regime rule: <= intrinsic + 1e-9 K), reused as the cold route's edge signal.
constexpr double kIntrinsicTolPerStrike = 1.0e-9;
} // namespace

void WorkspaceState::bind(const PreparedRiskBook &book) {
  const std::uint64_t id = book.book_logical_id();
  if (id != 0u && id == book_id) {
    return; // idempotent: the retained families / geometries stay warm
  }
  // Families observe the geometries: drop them before the geometries move.
  family.clear();
  carry.clear();
  geometry.clear();
  blocks.clear();
  const std::span<const RiskGroup> groups = book.groups();
  for (std::uint32_t g = 0; g < groups.size();) {
    std::uint32_t e = g + 1u;
    while (e < groups.size() && groups[e].uid == groups[g].uid &&
           groups[e].expiry_ns == groups[g].expiry_ns) {
      ++e;
    }
    blocks.push_back(ExpiryBlock{g, e});
    g = e;
  }
  geometry.resize(blocks.size());
  family.resize(groups.size());
  carry.resize(groups.size());
  cold.assign(book.n_unique(), std::uint8_t{0});
  block_counters.assign(blocks.size(), BlockCounters{});
  book_id = id;
}

AlOpts resolve_al_opts(const std::optional<AlOpts> &policy, const MarketState &state,
                       std::uint32_t uid) noexcept {
  if (policy.has_value()) {
    return *policy;
  }
  const SurfaceRef surf = state.surface(uid);
  return surf.valid() ? surf->pricing().al_opts : al_fast_opts();
}

void write_resolved(RiskFrame &f, std::size_t r, const ResolvedContract &rc) noexcept {
  f.T[r] = rc.T;
  f.sigma[r] = rc.sigma;
  f.forward[r] = rc.forward;
  f.q_eff[r] = rc.q_eff;
  f.rate[r] = rc.rate;
  f.vendor_price[r] = rc.vendor_price;
  f.vendor_delta[r] = rc.vendor_delta;
  f.flags[r] = rc.flags;
  if (rc.status != RiskLaneStatus::Ok) {
    write_refused(f, r, rc.status);
    return;
  }
  f.status[r] = RiskLaneStatus::Ok;
}

void write_refused(RiskFrame &f, std::size_t r, RiskLaneStatus status) noexcept {
  f.T[r] = f.sigma[r] = f.forward[r] = f.q_eff[r] = f.rate[r] = kNaN;
  f.price[r] = f.delta[r] = f.gamma[r] = f.vega[r] = f.theta[r] = kNaN;
  f.rho[r] = f.vanna[r] = f.volga[r] = f.charm[r] = kNaN;
  f.vendor_price[r] = f.vendor_delta[r] = kNaN;
  f.route[r] = RiskRoute::Refused;
  f.status[r] = status;
}

void cold_row(RiskFrame &f, std::size_t r, double S, double K, Side side, const AlOpts &opts,
              bool analytic, RiskNeeds needs, const AlExpiryGeometry *geometry) {
  const double T = f.T[r];
  const double sigma = f.sigma[r];
  const double rate = f.rate[r];
  const double q = f.q_eff[r];
  const std::optional<AlOpts> o{opts};
  // need_vega stays on: vega is always served, and vanna / volga ride its sigma solves.
  const Result<AmericanGreeks> res =
      !analytic ? american_greeks_fd(S, K, T, sigma, rate, q, side, AmericanMethod::AndersenLake,
                                     o)
      : geometry != nullptr
          ? american_greeks_al(*geometry, S, K, sigma, side, /*need_vega=*/true, needs.rho,
                               needs.charm)
          : american_greeks_al(S, K, T, sigma, rate, q, side, o, /*need_vega=*/true, needs.rho,
                               needs.charm);
  if (!res.has_value()) {
    write_refused(f, r, RiskLaneStatus::NumericError);
    return;
  }
  const AmericanGreeks &g = *res;
  const double rho = needs.rho ? g.rho : 0.0;
  const double vanna = needs.second_order ? g.vanna : 0.0;
  const double volga = needs.second_order ? g.volga : 0.0;
  const double charm = needs.charm ? g.charm : 0.0;
  if (!(std::isfinite(g.price) && std::isfinite(g.delta) && std::isfinite(g.gamma) &&
        std::isfinite(g.vega) && std::isfinite(g.theta) && std::isfinite(rho) &&
        std::isfinite(vanna) && std::isfinite(volga) && std::isfinite(charm))) {
    write_refused(f, r, RiskLaneStatus::NumericError);
    return;
  }
  f.price[r] = g.price;
  f.delta[r] = g.delta;
  f.gamma[r] = g.gamma;
  f.vega[r] = g.vega;
  f.theta[r] = g.theta;
  f.rho[r] = rho;
  f.vanna[r] = vanna;
  f.volga[r] = volga;
  f.charm[r] = charm;
  f.route[r] = analytic ? RiskRoute::ColdAnalytic : RiskRoute::ColdFd;
  f.status[r] = RiskLaneStatus::Ok;
  // NearExerciseEdge set HERE is NARROWER than the family's band. The cold bundles
  // expose no edge distance, so the bit is set only in the exercise regime: a positive
  // intrinsic and a served price within 1e-9 K of it (the family's own exercised
  // rule). Continuation-side lanes inside the family's 0.25 sigma sqrt(T) band
  // (kAlFamilyGreekEdgeBand) are NOT flagged here. The full band meaning exists only
  // on lanes the family kernel actually JUDGED (judge: family near_edge or a per-Greek
  // NearExerciseEdge refusal). Lanes sent cold BEFORE a family answer -- T below
  // kAlSigmaFamilyMinTau, a failed family prepare or carry set, a lane off the block
  // geometry, a group wholly below the sigma floor, a SigmaOutsideBox /
  // SigmaBelowGuard status, a FamilyUneconomic group, a ShortTenor block -- carry only
  // this narrow bit, like every pure-cold row.
  const double intrinsic = side == Side::Put ? K - S : S - K;
  if (intrinsic > 0.0 && g.price <= intrinsic + kIntrinsicTolPerStrike * K) {
    f.flags[r] |= RiskFlag::NearExerciseEdge;
  }
}

} // namespace risk_detail

// ── RiskWorkspace ─────────────────────────────────────────────────────────

struct RiskWorkspace::Impl {
  risk_detail::WorkspaceState state;
};

RiskWorkspace::RiskWorkspace() : impl_{std::make_unique<Impl>()} {}
RiskWorkspace::~RiskWorkspace() = default;
RiskWorkspace::RiskWorkspace(RiskWorkspace &&) noexcept = default;
RiskWorkspace &RiskWorkspace::operator=(RiskWorkspace &&) noexcept = default;

void RiskWorkspace::reserve(const PreparedRiskBook &book) {
  if (!impl_) {
    impl_ = std::make_unique<Impl>();
  }
  impl_->state.bind(book);
}

// ── RiskKernel ────────────────────────────────────────────────────────────

struct RiskKernel::Impl {
  RiskKernelKind kind{RiskKernelKind::Family};
  FamilyKernelPolicy family{};
  ColdKernelPolicy cold{};
};

RiskKernel::RiskKernel() : impl_{std::make_unique<Impl>()} {}
RiskKernel::~RiskKernel() = default;
RiskKernel::RiskKernel(RiskKernel &&) noexcept = default;
RiskKernel &RiskKernel::operator=(RiskKernel &&) noexcept = default;

RiskKernel RiskKernel::family(const FamilyKernelPolicy &policy) {
  RiskKernel k;
  k.impl_->kind = RiskKernelKind::Family;
  k.impl_->family = policy;
  return k;
}

RiskKernel RiskKernel::cold(const ColdKernelPolicy &policy) {
  RiskKernel k;
  k.impl_->kind = RiskKernelKind::Cold;
  k.impl_->cold = policy;
  return k;
}

RiskKernelKind RiskKernel::kind() const noexcept {
  return impl_ ? impl_->kind : RiskKernelKind::Family;
}
// PIMPL rule (risk_stream.cpp): a moved-from kernel reports the default policies.
const FamilyKernelPolicy &RiskKernel::family_policy() const noexcept {
  static const FamilyKernelPolicy kDefault{};
  return impl_ ? impl_->family : kDefault;
}
const ColdKernelPolicy &RiskKernel::cold_policy() const noexcept {
  static const ColdKernelPolicy kDefault{};
  return impl_ ? impl_->cold : kDefault;
}

namespace {

[[nodiscard]] Status validate_family_policy(const FamilyKernelPolicy &p) {
  if (p.n_sigma < kAlSigmaFamilyMinNodes || p.n_sigma > kAlSigmaFamilyMaxNodes) {
    return Err(ErrorCode::InvalidArgument, "RiskKernel: n_sigma outside [4, 16]");
  }
  if (!(std::isfinite(p.sigma_box_pad) && p.sigma_box_pad >= 0.0 && p.sigma_box_pad < 1.0)) {
    return Err(ErrorCode::InvalidArgument, "RiskKernel: sigma_box_pad must be in [0, 1)");
  }
  if (!(std::isfinite(p.sigma_box_floor) && p.sigma_box_floor > 0.0)) {
    return Err(ErrorCode::InvalidArgument, "RiskKernel: sigma_box_floor must be finite > 0");
  }
  // A floor above n_sigma is legal: the ceiling caps the floor (node_count).
  if (p.n_sigma_min < kAlSigmaFamilyMinNodes) {
    return Err(ErrorCode::InvalidArgument, "RiskKernel: n_sigma_min below 4");
  }
  if (p.n_sigma_min_short < kAlSigmaFamilyMinNodes) {
    return Err(ErrorCode::InvalidArgument, "RiskKernel: n_sigma_min_short below 4");
  }
  if (!(std::isfinite(p.short_box_tau) && p.short_box_tau >= 0.0)) {
    return Err(ErrorCode::InvalidArgument, "RiskKernel: short_box_tau must be finite >= 0");
  }
  if (!(std::isfinite(p.sigma_node_spacing) && p.sigma_node_spacing > 0.0)) {
    return Err(ErrorCode::InvalidArgument, "RiskKernel: sigma_node_spacing must be finite > 0");
  }
  if (!(std::isfinite(p.family_node_cost_us) && p.family_node_cost_us > 0.0 &&
        std::isfinite(p.cold_lane_cost_us) && p.cold_lane_cost_us > 0.0)) {
    return Err(ErrorCode::InvalidArgument,
               "RiskKernel: family_node_cost_us / cold_lane_cost_us must be finite > 0");
  }
  return Ok();
}

void tally_rows(RiskFrame &out) noexcept {
  FrameCounters &c = out.counters;
  for (std::size_t r = 0; r < out.size(); ++r) {
    ++c.status_count[static_cast<std::size_t>(out.status[r])];
    c.uneconomic_rows += has_flag(out.flags[r], RiskFlag::FamilyUneconomic) ? 1u : 0u;
    c.short_tenor_rows += has_flag(out.flags[r], RiskFlag::ShortTenor) ? 1u : 0u;
    switch (out.route[r]) {
    case RiskRoute::Family:
      ++c.family_rows;
      break;
    case RiskRoute::ColdAnalytic:
    case RiskRoute::ColdFd:
      ++c.cold_rows;
      break;
    case RiskRoute::Exercised:
      ++c.exercised_rows;
      break;
    case RiskRoute::Refused:
      ++c.refused_rows;
      break;
    case RiskRoute::None:
    case RiskRoute::Share:
      break;
    }
  }
}

} // namespace

Status RiskKernel::evaluate(const PreparedRiskBook &book, const MarketState &state,
                            RiskNeeds needs, RiskFrame &out, RiskWorkspace &ws) const {
  const auto t0 = std::chrono::steady_clock::now();
  const std::uint64_t id = book.book_logical_id();
  const auto fail = [&out](const char *what) -> Status {
    out.clear_values();
    return Err(ErrorCode::InvalidArgument, what);
  };
  if (!impl_) {
    return fail("RiskKernel::evaluate: moved-from kernel");
  }
  if (id == 0u) {
    return fail("RiskKernel::evaluate: unprepared book");
  }
  if (!ws.impl_) {
    return fail("RiskKernel::evaluate: moved-from workspace");
  }
  if (out.book_logical_id != 0u && out.book_logical_id != id) {
    return fail("RiskKernel::evaluate: frame bound to a different book");
  }
  risk_detail::WorkspaceState &w = ws.impl_->state;
  if (w.book_id != 0u && w.book_id != id) {
    return fail("RiskKernel::evaluate: workspace bound to a different book");
  }
  if (impl_->kind == RiskKernelKind::Family) {
    if (Status v = validate_family_policy(impl_->family); !v.has_value()) {
      out.clear_values();
      return v;
    }
  }

  const std::span<const std::uint32_t> uids = book.uids();
  const std::span<const std::uint32_t> share_slot = book.share_uid_slot();
  out.reserve(book.n_unique(), uids.size(), share_slot.size());
  out.clear_values();
  out.book_logical_id = id;
  out.ts_ns = state.ts_ns();
  out.needs = needs;
  w.bind(book);

  // A clock that refuses an expiry (VolTime OutOfRange) is structural: no frame. An
  // expiry whose row carries a T_override never reads the TimeSpec (expiry_clock.hpp).
  for (const RiskGroup &g : book.groups()) {
    const ExpiryCarry *row = risk_detail::expiry_row(state, g.uid, g.expiry_ns);
    if (!risk_detail::expiry_T(state, row, g.expiry_ns).has_value()) {
      return fail("RiskKernel::evaluate: the state's TimeSpec refuses a book expiry");
    }
  }

  for (std::size_t s = 0; s < uids.size(); ++s) {
    const UnderlierState *u = state.underlier(uids[s]);
    out.uid_spot[s] = u != nullptr ? u->spot : std::numeric_limits<double>::quiet_NaN();
    out.uid_flat_rate[s] = u != nullptr ? u->flat_rate : std::numeric_limits<double>::quiet_NaN();
    out.uid_status[s] = u != nullptr ? RiskLaneStatus::Ok : RiskLaneStatus::ModelUnavailable;
  }
  for (std::size_t j = 0; j < share_slot.size(); ++j) {
    out.share_mark[j] = out.uid_spot[share_slot[j]];
    out.share_status[j] = out.uid_status[share_slot[j]];
  }

  switch (impl_->kind) {
  case RiskKernelKind::Family:
    risk_detail::evaluate_family(book, state, needs, impl_->family, out, w);
    for (const risk_detail::BlockCounters &bc : w.block_counters) {
      out.counters.families_built += bc.families_built;
      out.counters.carry_sets_built += bc.carry_sets_built;
      out.counters.family_nodes += bc.family_nodes;
      out.counters.family_build_seconds += bc.family_build_seconds;
    }
    break;
  case RiskKernelKind::Cold:
    risk_detail::evaluate_cold(book, state, needs, impl_->cold, out);
    break;
  }
  tally_rows(out);
  out.counters.evaluate_seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  return Ok();
}

} // namespace atx::vol
