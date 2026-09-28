#include "strategy_cost_v2.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <limits>
#include <string>
#include <utility>
#include "strategy_target_replay_detail.hpp"

namespace atx::impl::strategy::cost_v2 {
namespace {
using namespace atx;
namespace co = atx::core;
namespace bk = atx::engine::book;
constexpr f64 nan = std::numeric_limits<f64>::quiet_NaN();
constexpr f64 bps = 1e-4;

bool finite_positive(f64 x) { return std::isfinite(x) && x > 0; }
bool finite_nonnegative(f64 x) { return std::isfinite(x) && x >= 0; }
// Same interval guard as the NAV replay (strategy_nav_replay.cpp guarded_move).
bool guarded_move(f64 close_a, f64 close_b, f64 raw_a, f64 raw_b) {
  const f64 adjusted = std::log(close_b) - std::log(close_a);
  const f64 raw = std::log(raw_b) - std::log(raw_a);
  return !std::isfinite(adjusted) || std::abs(adjusted) > 1.5 ||
         std::abs(adjusted) > std::abs(raw) + .10;
}
// "capacity-x<m>-v1" with the shortest round-trip spelling of m, '.' written 'p'.
std::string capacity_id(f64 m) {
  std::array<char, 32> text{};
  const auto written = std::to_chars(text.data(), text.data() + text.size(), m);
  std::string spelled(text.data(), written.ptr);
  std::replace(spelled.begin(), spelled.end(), '.', 'p');
  return "capacity-x" + spelled + "-v1";
}
} // namespace

f64 ko_cost_fraction(f64 participation, f64 daily_vol, f64 adv_dollars) noexcept {
  if (!finite_positive(adv_dollars) || !finite_nonnegative(daily_vol) ||
      !finite_nonnegative(participation))
    return nan;
  if (daily_vol == 0) return 0.0; // (sigma/.02) W^(-1/3) -> 0 as sigma -> 0 at fixed ADV
  const f64 scale = daily_vol * adv_dollars / ko_activity_ref; // W / W*
  const f64 root = std::cbrt(scale);                           // (W / W*)^(1/3)
  const f64 bracket = ko_kappa0_bps +
      ko_kappa_i_bps * std::sqrt(participation * root * root / ko_participation_ref);
  return (daily_vol / ko_sigma_ref) / root * bracket * bps;
}

f64 fim_cost_fraction(f64 participation) noexcept {
  if (!finite_nonnegative(participation)) return nan;
  const f64 y = 100.0 * participation; // percent of daily volume
  return std::max(0.0, fim_a_bps + fim_b_bps * y + fim_c_bps * std::sqrt(y)) * bps;
}

co::Result<NameImpactCost> NameImpactCost::create(ImpactLaw law, f64 commission_bps,
                                                  f64 max_participation) {
  if ((law != ImpactLaw::KyleObizhaevaV1 && law != ImpactLaw::FimLiveV1) ||
      !finite_nonnegative(commission_bps) || std::isnan(max_participation) ||
      max_participation <= 0)
    return co::Err(co::ErrorCode::InvalidArgument,
                   "cost v2: impact law, commission >= 0, max participation in (0, inf]");
  return co::Ok(NameImpactCost{law, commission_bps, max_participation});
}

f64 NameImpactCost::cost_fraction(f64 abs_dollars, const bk::LiquidityRow& row) const noexcept {
  if (!finite_positive(row.adv_dollars) || !finite_nonnegative(abs_dollars)) return nan;
  const f64 participation = abs_dollars / row.adv_dollars;
  const f64 impact = law_ == ImpactLaw::KyleObizhaevaV1
      ? ko_cost_fraction(participation, row.daily_vol, row.adv_dollars)
      : fim_cost_fraction(participation);
  return commission_bps_ * bps + impact; // NaN propagates
}

bk::TradeCost NameImpactCost::cost(usize /*instrument*/, usize /*period*/, f64 trade_dollars,
                                   const bk::LiquidityRow& row) const noexcept {
  const f64 requested = std::abs(trade_dollars);
  if (!std::isfinite(requested) || !std::isfinite(cost_fraction(0.0, row)))
    return bk::TradeCost{0.0, 0.0}; // no usable liquidity estimate: no fill, no charge
  f64 fill = requested;
  if (std::isfinite(max_participation_))
    fill = std::min(fill, max_participation_ * row.adv_dollars);
  const f64 fraction = cost_fraction(fill, row);
  if (!std::isfinite(fraction)) return bk::TradeCost{0.0, 0.0};
  const f64 signed_fill = fill == requested ? trade_dollars : std::copysign(fill, trade_dollars);
  return bk::TradeCost{signed_fill, fill * fraction};
}

f64 NameImpactCost::unrationed_cost(usize /*instrument*/, usize /*period*/, f64 trade_dollars,
                                    const bk::LiquidityRow& row) const noexcept {
  const f64 requested = std::abs(trade_dollars);
  if (requested == 0.0) return 0.0;
  return requested * cost_fraction(requested, row); // NaN propagates
}

NavScenario ko_scenario(const NavScenario& s2) {
  NavScenario s = s2;
  s.id = std::string(ko_scenario_id);
  s.cost = NavCostRule::SqrtImpactV1;
  s.half_spread_bps = 0; s.impact_y = 0; s.impact_delta = 0.5; // commission: S2's
  return s;
}
NavScenario fim_scenario(const NavScenario& s2) {
  NavScenario s = ko_scenario(s2);
  s.id = std::string(fim_scenario_id);
  s.commission_bps = 0;
  return s;
}

co::Result<std::unique_ptr<const bk::ReplayCostModel>> reserved_cost_model(const NavScenario& s) {
  const bool ko = s.id == ko_scenario_id, fim = s.id == fim_scenario_id;
  if (!ko && !fim) return co::Ok(std::unique_ptr<const bk::ReplayCostModel>{});
  if (s.cost != NavCostRule::SqrtImpactV1 || s.half_spread_bps != 0 || s.impact_y != 0 ||
      (fim && s.commission_bps != 0))
    return co::Err(co::ErrorCode::InvalidArgument,
                   "cost v2: reserved scenario id " + s.id + " with another shape");
  ATX_TRY(auto model,
          NameImpactCost::create(ko ? ImpactLaw::KyleObizhaevaV1 : ImpactLaw::FimLiveV1,
                                 s.commission_bps, s.max_participation));
  std::unique_ptr<const bk::ReplayCostModel> out =
      std::make_unique<const NameImpactCost>(std::move(model));
  return co::Ok(std::move(out));
}

const char* reserved_cost_rule(std::string_view trading_id) noexcept {
  if (trading_id == ko_scenario_id) return "ko-invariance-v1";
  if (trading_id == fim_scenario_id) return "fim-live-v1";
  return nullptr;
}

std::vector<NavScenario> capacity_scenarios(const NavScenario& s2) {
  std::vector<NavScenario> out;
  out.reserve(capacity_multiples.size());
  for (const f64 m : capacity_multiples) {
    NavScenario s = s2;
    s.id = capacity_id(m);
    s.impact_y = s2.impact_y * std::pow(m, s2.impact_delta); // m = 1: x 1.0, exact
    s.max_participation = s2.max_participation / m;           // m = 1: / 1.0, exact
    out.push_back(std::move(s));
  }
  return out;
}

f64 capacity_multiple(std::string_view trading_id) noexcept {
  for (const f64 m : capacity_multiples)
    if (trading_id == capacity_id(m)) return m;
  return nan;
}

void decision_liquidity(const TargetReplayInput& x, usize d, usize window, usize min_pairs,
                        DecisionLiquidity& out) {
  const usize n = x.instruments;
  out.adv.assign(n, nan); out.sigma.assign(n, nan);
  if (window == 0 || d >= x.dates || x.volume.size() != x.dates * n ||
      x.present.size() != x.dates * n)
    return;
  const usize first = d > window ? d - window : 0;
  for (usize i = 0; i < n; ++i) {
    if (!x.member[d * n + i]) continue;
    f64 dollars = 0, mean = 0, m2 = 0; usize pairs = 0;
    for (usize k = first; k < d; ++k) {
      const usize b = k * n + i;
      if (!x.present[b]) continue;
      dollars += x.raw_close[b] * x.volume[b];
      if (k == 0 || !x.present[b - n] ||
          guarded_move(x.close[b - n], x.close[b], x.raw_close[b - n], x.raw_close[b]))
        continue;
      const f64 r = x.close[b] / x.close[b - n] - 1;
      ++pairs; const f64 delta = r - mean;
      mean += delta / static_cast<f64>(pairs); m2 += delta * (r - mean);
    }
    const bool fallback = pairs < min_pairs || pairs < 2 || !std::isfinite(m2) || m2 < 0;
    out.adv[i] = dollars / static_cast<f64>(window);
    out.sigma[i] = fallback ? nan : std::sqrt(m2 / static_cast<f64>(pairs - 1));
  }
}

co::Status validate_aim_v6(const AimV6Params& p, f64 theta) {
  if (!(p.kappa >= 0 && p.kappa <= 1000) || !(p.band_b >= 0 && p.band_b <= 0.5) ||
      !std::isfinite(p.clip_lo) || !std::isfinite(p.clip_hi) || !(p.clip_lo > 0) ||
      !(p.clip_hi >= p.clip_lo) || !(p.band_exponent >= 0 && p.band_exponent <= 1) ||
      p.reference_decisions == 0 || p.reference_decisions > 4096 ||
      !(theta > 0 && theta * p.clip_hi <= 1))
    return co::Err(co::ErrorCode::InvalidArgument,
                   "aim-partial-v6: kappa in [0, 1000], band b in [0, 0.5], 0 < clip lo <= hi "
                   "with theta * hi <= 1, band exponent in [0, 1], 1..4096 reference decisions");
  return co::Ok();
}

f64 marginal_cost_s2(const NavScenario& s2, f64 q, f64 adv, f64 sigma) noexcept {
  if (!finite_positive(adv) || !finite_nonnegative(q)) return nan;
  const f64 vol = std::isnan(sigma) ? s2.fallback_daily_vol : sigma;
  if (!finite_nonnegative(vol)) return nan;
  return (s2.half_spread_bps + s2.commission_bps) * bps +
         (1.0 + s2.impact_delta) * s2.impact_y * vol * std::pow(q / adv, s2.impact_delta);
}

f64 finite_median(std::span<const f64> values) {
  std::vector<f64> kept;
  kept.reserve(values.size());
  for (const f64 v : values)
    if (std::isfinite(v)) kept.push_back(v);
  if (kept.empty()) return nan;
  std::sort(kept.begin(), kept.end());
  const usize h = kept.size() / 2;
  return kept.size() % 2 ? kept[h] : 0.5 * (kept[h - 1] + kept[h]);
}

void form_aim_v6(std::span<const u8> member, std::span<const f64> desired,
                 std::span<const f64> cost, f64 c_bar, f64 c_ref, f64 theta,
                 const AimV6Params& p, bool rebalance, AimV6Decision& out) {
  const usize n = desired.size();
  out.target.assign(desired.begin(), desired.end());
  out.band.assign(n, -1.0); // -1 dusts nothing: every |gap| >= 0
  out.theta = theta; out.c_bar = c_bar; out.c_ref = c_ref;
  out.scale_long = 1.0; out.scale_short = 1.0; out.costed = 0; out.members = 0;
  for (usize i = 0; i < n; ++i) out.members += member[i] ? 1U : 0U;
  if (!rebalance || out.members == 0) return;
  const bool priced = finite_positive(c_bar);
  const auto ratio = [&](usize i) { return priced ? cost[i] / c_bar : nan; };
  // Regime rate (R2.3). Unpriced dates keep theta.
  if (priced && finite_positive(c_ref))
    out.theta = theta * std::clamp(std::sqrt(c_ref / c_bar), p.clip_lo, p.clip_hi);
  // Cost-scaled no-trade band (R2.2): (b / N)(c_i / c_bar)^(1/3); median band = b / N.
  const f64 base = p.band_b > 0 ? p.band_b / static_cast<f64>(out.members) : -1.0;
  for (usize i = 0; i < n; ++i) {
    if (!member[i]) continue;
    const f64 r = ratio(i);
    if (std::isfinite(r)) ++out.costed;
    if (base >= 0) out.band[i] = std::isfinite(r) ? base * std::pow(r, p.band_exponent) : base;
  }
  // Cost-scaled target (R2.2), each side rescaled to its entry gross. kappa 0 or an
  // unpriced date: target == desired (copied above, never recomputed).
  if (!(p.kappa > 0) || !priced) return;
  f64 long_in = 0, short_in = 0, long_out = 0, short_out = 0;
  for (usize i = 0; i < n; ++i) {
    if (!member[i]) continue;
    const f64 r = ratio(i);
    const f64 t = std::isfinite(r) ? desired[i] / (1.0 + p.kappa * r) : 0.0;
    out.target[i] = t;
    if (desired[i] > 0) long_in += desired[i]; else short_in -= desired[i];
    if (t > 0) long_out += t; else short_out -= t;
  }
  out.scale_long = long_out > 0 ? long_in / long_out : 1.0;
  out.scale_short = short_out > 0 ? short_in / short_out : 1.0;
  for (usize i = 0; i < n; ++i) {
    if (!member[i]) continue;
    out.target[i] *= out.target[i] > 0 ? out.scale_long : out.scale_short;
  }
}

co::Status aim_partial_v6_weights(const TargetReplayInput& in, const TargetReplayConfig& cfg,
                                  usize d, bool rebalance, const AimV6Decision& v6,
                                  std::vector<f64>& current, TargetReplayDay& out) {
  const usize n = in.instruments, offset = d * n;
  if (cfg.rule != TargetReplayRule::AimPartialV5 || d >= in.dates || current.size() != n ||
      v6.target.size() != n || v6.band.size() != n || !(v6.theta >= 0 && v6.theta <= 1))
    return co::Err(co::ErrorCode::InvalidArgument, "aim-partial-v6: decision geometry/config");
  const bool decaying = cfg.exit_rate != 1.0;
  if (decaying && in.present.size() != in.dates * n)
    return co::Err(co::ErrorCode::InvalidArgument,
                   "aim-partial-v6: exit_rate below 1 needs prices (presence; --role)");
  f64 exit_band = 0;
  if (decaying) {
    const usize members = detail::members_at(in, d);
    exit_band = members ? cfg.dust_multiple / static_cast<f64>(members)
                        : std::numeric_limits<f64>::infinity();
  }
  const f64 keep = 1.0 - cfg.exit_rate;
  // The aim-partial-v5 loop (strategy_target_replay.cpp aim_partial_weights), operation for
  // operation, with the member band and theta taken from the v6 decision.
  out.applied_fraction = rebalance ? v6.theta : 0;
  f64 squared = 0;
  for (usize i = 0; i < n; ++i) {
    const bool live = in.member[offset + i] != 0;
    f64 next = 0;
    if (live && !rebalance) {
      next = current[i];
    } else if (live) {
      const f64 aim = cfg.aim_leverage * v6.target[i];
      const f64 gap = aim - current[i];
      const bool dusted = std::abs(gap) <= v6.band[i];
      if (dusted) ++out.construction.banded_names;
      next = dusted ? current[i] : current[i] + v6.theta * gap;
    } else if (decaying && in.present[offset + i]) {
      next = current[i] * keep;
      if (std::abs(next) <= exit_band) next = 0;
    }
    const f64 trade = std::abs(next - current[i]);
    out.turnover += trade;
    if (!live) out.forced_turnover += trade; else out.discretionary_turnover += trade;
    current[i] = next;
    out.gross += std::abs(next); out.net += next;
    out.long_weight += std::max(0.0, next); out.short_weight += std::max(0.0, -next);
    out.max_abs_weight = std::max(out.max_abs_weight, std::abs(next));
    out.held_names += next != 0 ? 1U : 0U; squared += next * next;
  }
  out.effective_names = squared > 0 ? out.gross * out.gross / squared : 0;
  return co::Ok();
}

f64 transfer_coefficient(std::span<const u8> member, std::span<const f64> desired,
                         std::span<const f64> sigma, std::span<const f64> weights) noexcept {
  const usize n = std::min({member.size(), desired.size(), sigma.size(), weights.size()});
  const auto used = [&](usize i) {
    return member[i] && finite_positive(sigma[i]) && std::isfinite(desired[i]) &&
           std::isfinite(weights[i]);
  };
  f64 sa = 0, sw = 0; usize count = 0;
  for (usize i = 0; i < n; ++i) {
    if (!used(i)) continue;
    sa += desired[i] / sigma[i]; sw += weights[i]; ++count;
  }
  if (count < 3) return nan;
  const f64 ma = sa / static_cast<f64>(count), mw = sw / static_cast<f64>(count);
  f64 caw = 0, caa = 0, cww = 0;
  for (usize i = 0; i < n; ++i) {
    if (!used(i)) continue;
    const f64 a = desired[i] / sigma[i] - ma, w = weights[i] - mw;
    caw += a * w; caa += a * a; cww += w * w;
  }
  if (!(caa > 0) || !(cww > 0)) return nan;
  return caw / std::sqrt(caa * cww);
}
} // namespace atx::impl::strategy::cost_v2
