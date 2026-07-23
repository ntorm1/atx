// Theoretical portfolio-RISK engine — implementation.
//
// Ports the scenario/shock engine (ats_greeks_scenario, ats_greeks_portfolio.c)
// and the Sprint 20 Stage II plan / resolve / price / PnL-explain lifecycle
// (ats_vol_portfolio_risk.c, ats_vol_portfolio_resolver.c,
// ats_vol_portfolio_explain.c). See portfolio_risk.hpp for the public contract
// and the port adaptations.

#include "atx/vol/portfolio_risk.hpp"

#include <algorithm>
#include <cmath>

#include "atx/core/math.hpp"       // norm_cdf
#include "atx/vol/black76.hpp"     // black76_price(_from_lnfk)
#include "atx/vol/correction.hpp"  // CorrectionCache::eval
#include "atx/vol/greeks.hpp"      // black76_greeks

namespace atx::vol {

using atx::core::Err;
using atx::core::Ok;

namespace {

// Quantization bucket so legs that intend the same expiry but pass slightly
// different T share a group (matches ATS_VOL_PORT_T_CLOCK_QUANT = one second).
inline constexpr double kTClockQuant = 1.0 / (365.25 * 86400.0);

// Finite-difference step for the smile (atm, skew, curv) capture (5% log-money;
// matches ATS_VOL_PORT_RISK_VOL_ATTR_H).
inline constexpr double kVolAttrH = 0.05;

[[nodiscard]] double quantize_T(double T) noexcept {
  if (!(std::isfinite(T)) || T <= 0.0) return T;
  return std::floor(T / kTClockQuant + 0.5) * kTClockQuant;
}

// B76 forward delta (call: N(d1); put: N(d1)-1).
[[nodiscard]] double b76_forward_delta(double F, double K, double T,
                                       double sigma, Side side) noexcept {
  if (!(T > 0.0) || !(sigma > 0.0) || !(F > 0.0) || !(K > 0.0)) {
    return kQuietNaN;
  }
  const double v = sigma * std::sqrt(T);
  const double d1 = (std::log(F / K) + 0.5 * v * v) / v;
  const double n_d1 = atx::core::norm_cdf(d1);
  return (side == Side::Call) ? n_d1 : (n_d1 - 1.0);
}

// Route resolution (ports ats_vol_port_risk_route_resolve). `usable_corr` is a
// populated cache for this group's side (or nullptr). Returns false only for
// the reserved AL_CORRECTION request (deferred).
struct RouteResolution {
  RoutePolicy route{RoutePolicy::B76Only};
  const CorrectionCache* correction{nullptr};
  std::uint32_t flags{0u};
  bool ok{true};
};

[[nodiscard]] RouteResolution route_resolve(RoutePolicy requested,
                                            const CorrectionCache* usable_corr,
                                            bool exact_T) noexcept {
  RouteResolution out;
  switch (requested) {
    case RoutePolicy::B76Only:
      out.route = RoutePolicy::B76Only;
      out.flags = kFlagRouteB76Only;
      return out;
    case RoutePolicy::B76AlCache:
      if (usable_corr != nullptr && exact_T) {
        out.route = RoutePolicy::B76AlCache;
        out.correction = usable_corr;
        out.flags = kFlagRouteAmerican;
        return out;
      }
      // No exact-T cache: price still lands, route honestly says B76.
      out.route = RoutePolicy::B76Only;
      out.flags = kFlagRouteB76Only | kResolverRouteFallbackB76;
      return out;
    case RoutePolicy::AlCorrection:
      out.flags = kResolverAmericanDeferred | kFlagInvalid;
      out.ok = false;
      return out;
  }
  out.flags = kFlagInvalid;
  out.ok = false;
  return out;
}

// B76 + AL-correction overlay at a single point, route-aware. Mirrors the
// C `price_with_route` used by the PnL-explain swap chain.
[[nodiscard]] double price_with_route(double F, double K, double T, double iv,
                                      double k_log, double df, Side side,
                                      RoutePolicy route,
                                      const CorrectionCache* corr) noexcept {
  if (!(F > 0.0) || !(K > 0.0) || !(T > 0.0) || !(iv > 0.0)) return kQuietNaN;
  double p = black76_price(F, K, T, iv, df, side);
  if (route == RoutePolicy::B76AlCache && corr != nullptr) {
    const double c = corr->eval(k_log, T, iv);
    if (std::isfinite(c)) p += F * c;
  }
  return p;
}

// (atm, skew, curv) of the smile on `surface` through `handle` via three
// inserted-slice IV evaluations (ports capture_smile_atm_skew_curv).
struct SmileFd {
  double atm{kQuietNaN};
  double skew{0.0};
  double curv{0.0};
};

[[nodiscard]] SmileFd capture_smile(const VolSurface& surface,
                                    const InsertedSliceHandle& handle) noexcept {
  const double h = kVolAttrH;
  const double iv_lo = iv_on_inserted_slice(surface, handle, -h);
  const double iv_0 = iv_on_inserted_slice(surface, handle, 0.0);
  const double iv_hi = iv_on_inserted_slice(surface, handle, h);
  SmileFd out;
  out.atm = iv_0;
  if (std::isfinite(iv_hi) && std::isfinite(iv_lo)) {
    out.skew = (iv_hi - iv_lo) / (2.0 * h);
  }
  if (std::isfinite(iv_hi) && std::isfinite(iv_lo) && std::isfinite(iv_0)) {
    out.curv = (iv_hi - 2.0 * iv_0 + iv_lo) / (h * h);
  }
  return out;
}

// Aggregation bucket key (ports agg_key_theo).
[[nodiscard]] std::uint64_t agg_key_theo(const TheoreticalLeg& leg,
                                         AggMode mode) noexcept {
  switch (mode) {
    case AggMode::Total:
      return 0ull;
    case AggMode::ByUid:
      return static_cast<std::uint64_t>(leg.uid);
    case AggMode::ByUidExpiry: {
      const std::uint64_t uid_part = static_cast<std::uint64_t>(leg.uid) << 32;
      const auto t_part = static_cast<std::uint64_t>(
          static_cast<std::uint32_t>(quantize_T(leg.T_clock) * 1.0e6));
      return uid_part | t_part;
    }
    case AggMode::ByGroupId:
      return static_cast<std::uint64_t>(leg.group_id);
  }
  return 0ull;
}

// Resolve the per-side correction from a per-uid market view.
[[nodiscard]] const CorrectionCache* side_correction(const UnderlyingMarket& m,
                                                     Side side) noexcept {
  return (side == Side::Call) ? m.correction_call : m.correction_put;
}

}  // namespace

// ── Scenario / shock engine (ports ats_greeks_scenario) ──────────────────

Result<double> scenario_pnl(std::span<const PortfolioLeg> book,
                            const MarketBinding& binding,
                            std::span<const Shock> shocks) {
  if (binding.universe == nullptr) {
    return Err(ErrorCode::InvalidArgument, "null universe");
  }

  // Compose shock magnitudes (repeated shocks of a kind add).
  double ds_pct = 0.0;
  double dsigma_abs = 0.0;
  double dsigma_rel = 0.0;
  double dr_abs = 0.0;
  double dt_abs = 0.0;
  for (const Shock& s : shocks) {
    switch (s.kind) {
      case ShockKind::SpotPct:
        ds_pct += s.amount;
        break;
      case ShockKind::VolAbs:
        dsigma_abs += s.amount;
        break;
      case ShockKind::VolRel:
        dsigma_rel += s.amount;
        break;
      case ShockKind::RateAbs:
        dr_abs += s.amount;
        break;
      case ShockKind::TimeAbs:
        dt_abs += s.amount;
        break;
      case ShockKind::SurfaceTwist:
        return Err(ErrorCode::NotImplemented, "surface twist shock deferred");
    }
  }

  double pnl = 0.0;
  for (const PortfolioLeg& leg : book) {
    if (leg.kind != LegKind::Option) {
      continue;  // ats_greeks_scenario prices option positions only
    }
    const Uid uid = cid_uid(leg.contract_id);
    const ExpiryId eid = cid_expiry(leg.contract_id);
    auto ctxr = detail::resolve_expiry_context(binding, uid, eid);
    if (!ctxr) {
      continue;  // skip unresolvable positions (matches resolve_position != 0)
    }
    const detail::ExpiryContext& ctx = *ctxr;

    const std::uint16_t sx = cid_strike_idx(leg.contract_id);
    if (static_cast<std::size_t>(sx) >= ctx.chain->strikes.size()) {
      continue;
    }
    const double K = ctx.chain->strikes[static_cast<std::size_t>(sx)];
    if (!(K > 0.0)) continue;
    const Side side = cid_side(leg.contract_id);
    const double S = ctx.under->spot;

    // IV from the surface; fall back to a flat 0.20 when no surface is fitted
    // (matches resolve_position's fixture fallback).
    double sigma = 0.20;
    if (ctx.surface != nullptr && ctx.surface->n_slices() > 0) {
      const double k_log = std::log(K / ctx.F);
      sigma = (ctx.slice_idx != kNoSliceMatch)
                  ? ctx.surface->iv_on_slice(ctx.slice_idx, k_log)
                  : ctx.surface->iv(k_log, ctx.T);
      if (!(std::isfinite(sigma) && sigma > 0.0)) continue;
    }

    // GR-P1-2: when a per-side correction cache is bound in this MarketBinding, the
    // book's marks carry the American early-exercise premium F*c (the pricing engine
    // prices with that overlay). Include the SAME overlay in BOTH scenario legs, or
    // the scenario PnL of an American book silently excludes the EEP change and the
    // base marks disagree with the pricing engine's. (Vol/spot roll stays sticky-
    // strike: sigma is the base-strike IV under a spot shock — a documented modeling
    // choice matching the C; scenario_grid is the smile-roll path.)
    const CorrectionCache* corr =
        (side == Side::Call) ? ctx.correction_call : ctx.correction_put;
    double p_base = black76_price(ctx.F, K, ctx.T, sigma, ctx.df, side);
    if (corr != nullptr) {
      p_base += ctx.F * corr->eval(std::log(K / ctx.F), ctx.T, sigma);
    }

    // Apply shocks. Relative vol first, then absolute; T subtracts elapsed.
    const double S_s = S * (1.0 + ds_pct);
    const double sigma_s = sigma * (1.0 + dsigma_rel) + dsigma_abs;
    const double r_s = ctx.r + dr_abs;
    double T_s = ctx.T - dt_abs;
    if (!(T_s > 0.0)) T_s = kTMinEval;

    const double F_s = S_s * std::exp((r_s - ctx.q) * T_s);
    const double df_s = std::exp(-r_s * T_s);
    double sigma_eff = sigma_s;
    if (!(sigma_eff > 0.0)) sigma_eff = 1.0e-3;

    double p_shocked = black76_price(F_s, K, T_s, sigma_eff, df_s, side);
    if (corr != nullptr) {
      p_shocked += F_s * corr->eval(std::log(K / F_s), T_s, sigma_eff);
    }
    pnl += leg.qty * (p_shocked - p_base);
  }
  return pnl;
}

// ── PricingPlan::create (grouping) ───────────────────────────────────────

Result<PricingPlan> PricingPlan::create(std::span<const TheoreticalLeg> legs) {
  PricingPlan plan;
  plan.legs_.assign(legs.begin(), legs.end());
  if (plan.legs_.empty()) {
    return plan;
  }

  plan.lanes_.reserve(plan.legs_.size());
  plan.groups_.reserve(plan.legs_.size());

  for (std::size_t i = 0; i < plan.legs_.size(); ++i) {
    const TheoreticalLeg& leg = plan.legs_[i];
    const double t_q = quantize_T(leg.T_clock);

    std::size_t g_ix = plan.groups_.size();
    for (std::size_t g = 0; g < plan.groups_.size(); ++g) {
      const Group& gr = plan.groups_[g];
      if (gr.uid == leg.uid && gr.side == leg.side &&
          gr.route_policy == leg.route_policy &&
          gr.interp_mode == leg.interp_mode &&
          gr.extrap_policy == leg.extrap_policy &&
          gr.time_mode == leg.time_mode && gr.T_clock_q == t_q) {
        g_ix = g;
        break;
      }
    }
    if (g_ix == plan.groups_.size()) {
      Group gr;
      gr.uid = leg.uid;
      gr.side = leg.side;
      gr.route_policy = leg.route_policy;
      gr.interp_mode = leg.interp_mode;
      gr.extrap_policy = leg.extrap_policy;
      gr.time_mode = leg.time_mode;
      gr.T_clock_q = t_q;
      plan.groups_.push_back(gr);
    }

    const double mult =
        (std::isfinite(leg.multiplier) && leg.multiplier > 0.0) ? leg.multiplier
                                                                : 100.0;
    Lane lane;
    lane.input_ix = static_cast<std::uint32_t>(i);
    lane.group_ix = static_cast<std::uint32_t>(g_ix);
    lane.side = leg.side;
    lane.qty_x_mult = leg.qty * mult;
    lane.x_orig = leg.x;
    lane.coord_kind = leg.coord_kind;
    lane.delta_convention = leg.delta_convention;
    lane.group_id = leg.group_id;
    plan.lanes_.push_back(lane);
  }

  // Re-sort lanes by (group_ix, input_ix) so each group's lanes are contiguous.
  std::stable_sort(plan.lanes_.begin(), plan.lanes_.end(),
                   [](const Lane& a, const Lane& b) noexcept {
                     if (a.group_ix != b.group_ix) return a.group_ix < b.group_ix;
                     return a.input_ix < b.input_ix;
                   });

  // Recompute per-group lane ranges after the sort.
  for (Group& g : plan.groups_) {
    g.lane_start = 0u;
    g.lane_count = 0u;
  }
  constexpr std::uint32_t kNoGroup = 0xFFFFFFFFu;
  std::uint32_t cur_g = kNoGroup;
  std::uint32_t start = 0u;
  for (std::size_t i = 0; i < plan.lanes_.size(); ++i) {
    const std::uint32_t g = plan.lanes_[i].group_ix;
    if (g != cur_g) {
      if (cur_g != kNoGroup) {
        plan.groups_[cur_g].lane_start = start;
        plan.groups_[cur_g].lane_count = static_cast<std::uint32_t>(i) - start;
      }
      cur_g = g;
      start = static_cast<std::uint32_t>(i);
    }
  }
  if (cur_g != kNoGroup) {
    plan.groups_[cur_g].lane_start = start;
    plan.groups_[cur_g].lane_count =
        static_cast<std::uint32_t>(plan.lanes_.size()) - start;
  }

  return plan;
}

// ── PricingPlan::bind_market (resolver pipeline) ──────────────────────────

std::uint32_t PricingPlan::find_or_build_ctx(Uid uid, double t_q,
                                             const VolSurface* surface,
                                             const CurveSet* curves,
                                             InterpMode interp,
                                             ProjExtrapPolicy extrap,
                                             const TimeModel& tm,
                                             std::uint32_t& out_flags,
                                             bool& out_ok) {
  for (std::size_t i = 0; i < ctxs_.size(); ++i) {
    const Ctx& c = ctxs_[i];
    if (c.uid == uid && c.T_clock_q == t_q && c.surface == surface &&
        c.interp_mode == interp) {
      out_flags |= kResolverInsertedSliceReused;
      out_ok = true;
      return static_cast<std::uint32_t>(i);
    }
  }
  auto handle = surface_insert_vol_slice(*surface, curves, tm, t_q, interp,
                                         extrap, /*with_no_arb_check=*/false);
  if (!handle) {
    out_ok = false;
    return 0u;
  }
  out_flags |= handle->flags;
  Ctx c;
  c.uid = uid;
  c.T_clock_q = t_q;
  c.surface = surface;
  c.interp_mode = interp;
  c.handle = *handle;
  ctxs_.push_back(c);
  out_ok = true;
  return static_cast<std::uint32_t>(ctxs_.size() - 1);
}

void PricingPlan::resolve_group(Group& g, const MarketBinding& binding,
                                const TimeModel& tm) {
  g.ctx_ix = 0xFFFFFFFFu;
  g.flags = 0u;
  g.invalid = false;
  g.surface = nullptr;
  g.curves = nullptr;
  g.correction = nullptr;
  g.resolved_route = RoutePolicy::B76Only;

  // PORT NOTE: the C Stage II resolver's universe + per-uid override lookup is
  // replaced by the portfolio-core per-uid market view.
  const UnderlyingMarket* m = binding.market_for(g.uid);
  if (m == nullptr || m->surface == nullptr || m->curves == nullptr) {
    g.invalid = true;
    g.flags |= kFlagInvalid;
    return;
  }
  g.surface = m->surface;
  g.curves = m->curves;

  std::uint32_t ctx_flags = 0u;
  bool ctx_ok = false;
  const std::uint32_t ctx_ix =
      find_or_build_ctx(g.uid, g.T_clock_q, g.surface, g.curves, g.interp_mode,
                        g.extrap_policy, tm, ctx_flags, ctx_ok);
  g.flags |= ctx_flags;
  if (!ctx_ok) {
    g.invalid = true;
    g.flags |= kFlagInvalid;
    return;
  }
  g.ctx_ix = ctx_ix;
  const Ctx& ctx = ctxs_[ctx_ix];

  // RouteResolver. A populated per-side correction + exact-T pillar unlocks the
  // American-cache route; otherwise honestly fall back to B76.
  const CorrectionCache* raw_corr = side_correction(*m, g.side);
  const CorrectionCache* usable_corr =
      (raw_corr != nullptr && raw_corr->populated()) ? raw_corr : nullptr;
  const bool exact_T = (ctx.handle.exact_slice_idx >= 0);
  const RouteResolution rr = route_resolve(g.route_policy, usable_corr, exact_T);
  g.flags |= rr.flags;
  if (!rr.ok) {
    g.invalid = true;  // AL_CORRECTION deferred — whole group invalid
    return;
  }
  g.correction = rr.correction;
  g.resolved_route = rr.route;

  // StrikeResolver per lane.
  const double F = ctx.handle.F;
  const std::uint32_t lane_end = g.lane_start + g.lane_count;
  for (std::uint32_t i = g.lane_start; i < lane_end; ++i) {
    Lane& lane = lanes_[static_cast<std::size_t>(i)];
    lane.lane_flags = 0u;
    lane.invalid = false;
    if (!(std::isfinite(F) && F > 0.0)) {
      lane.lane_flags |= kFlagInvalid;
      lane.invalid = true;
      continue;
    }
    switch (lane.coord_kind) {
      case CoordKind::Strike:
        if (!(lane.x_orig > 0.0)) {
          lane.lane_flags |= kFlagInvalid;
          lane.invalid = true;
        } else {
          lane.K = lane.x_orig;
          lane.k_log = std::log(lane.K) - ctx.handle.logF;
        }
        break;
      case CoordKind::LogMoneyness:
        if (!std::isfinite(lane.x_orig)) {
          lane.lane_flags |= kFlagInvalid;
          lane.invalid = true;
        } else {
          lane.k_log = lane.x_orig;
          lane.K = F * std::exp(lane.k_log);
        }
        break;
      case CoordKind::StandardMoneyness:
        if (g.curves == nullptr || !(g.curves->spot > 0.0)) {
          lane.lane_flags |= kFlagInvalid;
          lane.invalid = true;
        } else {
          lane.K = g.curves->spot * lane.x_orig;
          if (!(lane.K > 0.0)) {
            lane.lane_flags |= kFlagInvalid;
            lane.invalid = true;
          } else {
            lane.k_log = std::log(lane.K) - ctx.handle.logF;
          }
        }
        break;
      case CoordKind::Delta: {
        auto solved = surface_solve_k_for_delta(
            *g.surface, *g.curves, tm, g.T_clock_q, lane.x_orig, lane.side,
            lane.delta_convention, g.extrap_policy);
        if (!solved) {
          // PORT NOTE: the C surfaces the solver's DELTA_NOT_BRACKETED /
          // INVALID flags through the (discarded-on-error) result struct; the
          // Result port re-raises the observable flags here.
          lane.lane_flags |= kFlagDeltaNotBracketed | kFlagInvalid;
          lane.invalid = true;
        } else {
          lane.lane_flags |= solved->flags;
          lane.K = solved->K;
          lane.k_log = solved->k_log;
          lane.quote_delta = solved->quote_delta;
        }
        break;
      }
    }
  }
}

Status PricingPlan::bind_market(const MarketBinding& binding,
                                const TimeModel& tm) {
  time_model_ = tm;
  bound_ = true;
  ctxs_.clear();  // re-bind allowed across snapshots
  for (Group& g : groups_) {
    resolve_group(g, binding, tm);
  }
  return Ok();
}

// ── PricingPlan::price (theoretical hot pass) ─────────────────────────────

void PricingPlan::price_group(const Group& g, PortfolioRiskMode risk_mode,
                              AggMode agg_mode,
                              std::vector<TheoLegValue>& legs_out,
                              std::vector<PortfolioAggregate>& aggs) const {
  auto bucket = [&aggs](std::uint64_t key) -> PortfolioAggregate& {
    for (PortfolioAggregate& a : aggs) {
      if (a.group_key == key) return a;
    }
    PortfolioAggregate a;
    a.group_key = key;
    aggs.push_back(a);
    return aggs.back();
  };

  const std::uint32_t lane_end = g.lane_start + g.lane_count;

  if (g.invalid || g.ctx_ix == 0xFFFFFFFFu) {
    for (std::uint32_t i = g.lane_start; i < lane_end; ++i) {
      const Lane& lane = lanes_[static_cast<std::size_t>(i)];
      TheoLegValue& lv = legs_out[static_cast<std::size_t>(lane.input_ix)];
      lv.status = LaneStatus::ModelUnavailable;
      lv.resolver_flags = g.flags | lane.lane_flags;
    }
    return;
  }

  const Ctx& ctx = ctxs_[g.ctx_ix];
  const double F = ctx.handle.F;
  const double df = ctx.handle.df;
  const double T = g.T_clock_q;
  const double tau = ctx.handle.tau_vol;
  const double r = ctx.handle.r;
  const double q = std::isfinite(ctx.handle.q_eff) ? ctx.handle.q_eff : 0.0;
  const double m = std::exp((r - q) * T);
  const Side side = g.side;

  for (std::uint32_t i = g.lane_start; i < lane_end; ++i) {
    const Lane& lane = lanes_[static_cast<std::size_t>(i)];
    TheoLegValue& lv = legs_out[static_cast<std::size_t>(lane.input_ix)];
    const std::uint32_t leg_flags = g.flags | lane.lane_flags;
    lv.resolver_flags = leg_flags;
    lv.T_clock = T;
    lv.tau_vol = tau;
    lv.route = g.resolved_route;

    if (lane.invalid) {
      lv.status = LaneStatus::ModelUnavailable;
      continue;
    }

    const double K = std::isfinite(lane.K) ? lane.K : 1.0;
    const double k_log = std::isfinite(lane.k_log) ? lane.k_log : 0.0;
    const double iv = iv_on_inserted_slice(*g.surface, ctx.handle, k_log);
    double price = black76_price_from_lnfk(F, K, T, iv, df, -k_log,
                                           ctx.handle.sqrtT, side);
    if (g.resolved_route == RoutePolicy::B76AlCache && g.correction != nullptr) {
      price += F * g.correction->eval(k_log, T, iv);
    }

    if (!(std::isfinite(iv) && iv > 0.0)) {
      lv.status = LaneStatus::ModelUnavailable;
    } else if (!(std::isfinite(price) && price >= 0.0)) {
      lv.status = LaneStatus::NumericError;
    } else {
      lv.status = LaneStatus::Ok;
    }
    lv.iv = iv;
    lv.price = price;

    double qdelta = lane.quote_delta;
    if (!std::isfinite(qdelta)) {
      qdelta = b76_forward_delta(F, K, T, iv, side);
    }
    lv.quote_delta = qdelta;

    double spot_delta = kQuietNaN;
    double vega = kQuietNaN;
    double theta = kQuietNaN;
    double rho = kQuietNaN;
    if (risk_mode == PortfolioRiskMode::FirstOrder &&
        std::isfinite(iv) && iv > 0.0) {
      const Greeks bg = black76_greeks(F, K, T, iv, r, df, side).greeks;
      double d_fwd = bg.delta;
      vega = bg.vega;
      // GR-P1-1: the served price carries the American early-exercise premium F*c
      // (above) on the B76AlCache route, but the FirstOrder greeks were pure
      // Black-76 — so a deep-ITM American put reported the American price with a
      // EUROPEAN delta/vega/theta/rho, under-hedging by the full EEP delta
      // (order c - c_k). Correct the four first-order axes with the SAME correction
      // jet the sibling price_option (portfolio_price.cpp) uses (gamma stays the B76
      // leg there too). No cache / non-B76AlCache route keeps the exact prior values.
      if (g.resolved_route == RoutePolicy::B76AlCache && g.correction != nullptr) {
        double dk = 0.0, d_t = 0.0, dsig = 0.0;
        const double c_val = g.correction->eval_grad(k_log, T, iv, &dk, &d_t, &dsig);
        d_fwd = bg.delta + c_val - dk;
        vega = bg.vega + F * dsig;
        theta = bg.theta - (r - q) * F * d_fwd - F * d_t;
        rho = bg.rho + T * F * d_fwd;
      } else {
        theta = bg.theta - (r - q) * F * d_fwd;
        rho = bg.rho + T * F * d_fwd;
      }
      spot_delta = m * d_fwd;
      lv.delta = spot_delta;
      lv.gamma = m * m * bg.gamma;
      lv.vega = vega;
      lv.theta = theta;
      lv.rho = rho;
    } else {
      lv.delta = 0.0;
    }

    // GR-P2-5: status-gate the aggregate (the canonical path does). A
    // ModelUnavailable/NumericError lane can carry a FINITE price (e.g. a sigma<=0
    // intrinsic), and `isfinite(price)` used to let it pollute the buckets.
    if (lv.status == LaneStatus::Ok) {
      PortfolioAggregate& slot = bucket(agg_key_theo(
          legs_[static_cast<std::size_t>(lane.input_ix)], agg_mode));
      const double v_d = std::isfinite(spot_delta) ? spot_delta : 0.0;
      const double v_v = std::isfinite(vega) ? vega : 0.0;
      const double v_t = std::isfinite(theta) ? theta : 0.0;
      const double v_r = std::isfinite(rho) ? rho : 0.0;
      slot.value += lane.qty_x_mult * price;
      slot.delta += lane.qty_x_mult * v_d;
      slot.vega += lane.qty_x_mult * v_v;
      slot.theta += lane.qty_x_mult * v_t;
      slot.rho += lane.qty_x_mult * v_r;
      slot.n_legs += 1u;
    }
  }
}

Result<PricingResult> PricingPlan::price(PortfolioRiskMode risk_mode,
                                         AggMode agg_mode) const {
  if (!bound_) {
    return Err(ErrorCode::InvalidArgument, "plan not bound");
  }
  PricingResult result;
  result.legs.resize(legs_.size());
  for (const Group& g : groups_) {
    price_group(g, risk_mode, agg_mode, result.legs, result.aggregates);
  }
  return result;
}

// ── PricingPlan::project_compare (PnL explain) ───────────────────────────

Result<ProjectCompareResult> PricingPlan::project_compare(
    const MarketBinding& source, const MarketBinding& target,
    const TimeModel& tm) {
  // Source pass.
  ATX_TRY_VOID(bind_market(source, tm));
  ATX_TRY(PricingResult src_res, price(PortfolioRiskMode::PriceOnly,
                                       AggMode::Total));

  // Per-group source snapshot (route / corr / F / T / df / smile) captured
  // before the target re-bind overwrites the inserted-slice contexts.
  struct Snapshot {
    RoutePolicy route{RoutePolicy::B76Only};
    const CorrectionCache* corr{nullptr};
    double F{kQuietNaN};
    double T{kQuietNaN};
    double df{kQuietNaN};
    SmileFd smile{};
    bool valid{false};
  };
  std::vector<Snapshot> src_grp(groups_.size());
  for (std::size_t g = 0; g < groups_.size(); ++g) {
    const Group& grp = groups_[g];
    if (grp.ctx_ix == 0xFFFFFFFFu) continue;
    const Ctx& ctx = ctxs_[grp.ctx_ix];
    Snapshot& snap = src_grp[g];
    snap.route = grp.resolved_route;
    snap.corr = grp.correction;
    snap.F = ctx.handle.F;
    snap.T = grp.T_clock_q;
    snap.df = ctx.handle.df;
    snap.smile = capture_smile(*grp.surface, ctx.handle);
    snap.valid = true;
  }

  // Target pass.
  ATX_TRY_VOID(bind_market(target, tm));
  ATX_TRY(PricingResult tgt_res, price(PortfolioRiskMode::PriceOnly,
                                       AggMode::Total));

  ProjectCompareResult out;
  out.explain.resize(legs_.size());

  for (std::size_t g = 0; g < groups_.size(); ++g) {
    const Group& grp = groups_[g];
    if (grp.ctx_ix == 0xFFFFFFFFu) continue;
    const Ctx& t_ctx = ctxs_[grp.ctx_ix];
    const Snapshot& src = src_grp[g];

    const double tgt_F = t_ctx.handle.F;
    const double tgt_T = grp.T_clock_q;
    const double tgt_df = t_ctx.handle.df;
    const RoutePolicy tgt_route = grp.resolved_route;
    const CorrectionCache* tgt_corr = grp.correction;
    const SmileFd tgt_smile = capture_smile(*grp.surface, t_ctx.handle);

    const std::uint32_t lane_end = grp.lane_start + grp.lane_count;
    for (std::uint32_t i = grp.lane_start; i < lane_end; ++i) {
      const Lane& lane = lanes_[static_cast<std::size_t>(i)];
      const std::size_t out_ix = static_cast<std::size_t>(lane.input_ix);
      const double ps = src_res.legs[out_ix].price;
      const double pt = tgt_res.legs[out_ix].price;
      const double src_iv = src_res.legs[out_ix].iv;
      const double tgt_iv = tgt_res.legs[out_ix].iv;
      const double K = lane.K;
      const double k_log = lane.k_log;
      const Side side = lane.side;

      if (!std::isfinite(ps) || !std::isfinite(pt) || !src.valid ||
          !std::isfinite(src.F) || !std::isfinite(src_iv) ||
          !std::isfinite(tgt_iv)) {
        continue;
      }

      const double p_swapF = price_with_route(tgt_F, K, tgt_T, src_iv, k_log,
                                              tgt_df, side, src.route, src.corr);
      const double p_swapVol = price_with_route(tgt_F, K, tgt_T, tgt_iv, k_log,
                                                tgt_df, side, src.route,
                                                src.corr);
      const double p_swapR = price_with_route(tgt_F, K, tgt_T, tgt_iv, k_log,
                                              tgt_df, side, tgt_route, tgt_corr);

      ExplainRow& row = out.explain[out_ix];
      row.d_from_forward = p_swapF - ps;
      row.d_from_vol = p_swapVol - p_swapF;
      row.d_from_route = p_swapR - p_swapVol;
      row.d_from_interp = pt - p_swapR;
      row.d_price = pt - ps;
      row.resolver_flags =
          src_res.legs[out_ix].resolver_flags | tgt_res.legs[out_ix].resolver_flags;

      // Vol-attribution chain: blend (atm, skew, curv) one component at a time
      // and reprice under src.route + src.corr so the four pieces telescope to
      // d_from_vol.
      const double k = k_log;
      const double k2 = k * k;
      const double dlvl = (std::isfinite(tgt_smile.atm) && std::isfinite(src.smile.atm))
                              ? (tgt_smile.atm - src.smile.atm)
                              : 0.0;
      const double dskw = tgt_smile.skew - src.smile.skew;
      const double dcrv = tgt_smile.curv - src.smile.curv;
      const double iv1 = src_iv + dlvl;
      const double iv2 = iv1 + dskw * k;
      const double iv3 = iv2 + 0.5 * dcrv * k2;
      const double p0 = p_swapF;
      const double p1 = price_with_route(tgt_F, K, tgt_T, iv1, k_log, tgt_df,
                                         side, src.route, src.corr);
      const double p2 = price_with_route(tgt_F, K, tgt_T, iv2, k_log, tgt_df,
                                         side, src.route, src.corr);
      const double p3 = price_with_route(tgt_F, K, tgt_T, iv3, k_log, tgt_df,
                                         side, src.route, src.corr);
      const double p4 = p_swapVol;
      row.d_from_vol_level = p1 - p0;
      row.d_from_vol_skew = p2 - p1;
      row.d_from_vol_curvature = p3 - p2;
      row.d_from_vol_higher = p4 - p3;
    }
  }

  out.source = std::move(src_res);
  out.target = std::move(tgt_res);
  return out;
}

}  // namespace atx::vol
