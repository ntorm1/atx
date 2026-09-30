#include "strategy_target_replay.hpp"
#include "strategy_target_replay_detail.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <iomanip>
#include <limits>
#include <map>
#include <new>
#include <ostream>
#include <set>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>
#include <nlohmann/json.hpp>
#include "atx/core/sha256.hpp"
#include "atx/engine/data/research_window.hpp" // v8 E-25: the seal a label role may not reach

namespace atx::impl::strategy {
namespace {
using namespace atx;
namespace co = atx::core;
namespace eb = atx::engine::book;
namespace rw = atx::engine::data;
using Json = nlohmann::json;
using Ranked = std::pair<f64, usize>;
constexpr f64 nan = std::numeric_limits<f64>::quiet_NaN();
constexpr i64 day_ns = 86'400'000'000'000LL;
constexpr u64 metadata_bytes = 64ULL << 20;
constexpr usize max_dates = 4096, max_names = 20000;
struct Budget {
  u64 limit{}, used{};
  bool add(u64 count, u64 width) {
    if (width && count > (limit - used) / width) return false;
    used += count * width; return true;
  }
};
bool hash_valid(std::string_view s) {
  return s.size() == 64 && std::all_of(s.begin(), s.end(), [](char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
  });
}
u32 calendar_month(i64 session) {
  const std::chrono::year_month_day date{
      std::chrono::sys_days{std::chrono::days{session / day_ns}}};
  return static_cast<u32>(static_cast<int>(date.year())) * 100U +
         static_cast<u32>(static_cast<unsigned>(date.month()));
}
bool neutralizing(const TargetReplayConfig& cfg) {
  return cfg.neutralize == TargetNeutralize::PriceRiskV1 || neutralize_by_industry(cfg.neutralize);
}
// Stable spelling of a neutralization id (recipe, rule id, summary, CLI).
const char* neutralize_name(TargetNeutralize id) {
  switch (id) {
  case TargetNeutralize::None: return "none";
  case TargetNeutralize::PriceRiskV1: return "price-risk-v1";
  case TargetNeutralize::PriceRiskIndV1: return "price-risk-ind-v1";
  case TargetNeutralize::PriceRiskIndV2: return "price-risk-ind-v2";
  }
  return "unknown";
}
bool aim_partial(const TargetReplayConfig& cfg) {
  return cfg.rule == TargetReplayRule::AimPartialV5;
}
// v6 prereg C2: nonmembers decay at exit_rate instead of exiting at once (1 = off).
bool decaying_exit(const TargetReplayConfig& cfg) { return cfg.exit_rate != 1.0; }
// Rate of the aim-partial-v5 move in the target replay: one fixed theta
// (trade_fraction) for every name. The per-name rate (T36, rate per-name-v1) needs a
// NAV and liquidity, so it is the NAV replay's own option: it fills the update_weights
// per_name_rate span and overrides this spelling in its recipe and summary.
const char* aim_rate(const TargetReplayConfig&) { return "fixed"; }
// The compute_price_exposures config contract documented in
// strategy_price_exposures.hpp, checked up front so a bad recipe is refused once
// instead of surfacing as an error at every decision.
bool price_risk_valid(const PriceExposureConfig& c) {
  return c.beta_window >= 2 && c.beta_window <= max_dates && c.vol_window >= 2 &&
         c.vol_window <= max_dates && c.adv_window >= 1 && c.adv_window <= max_dates &&
         c.min_return_pairs >= 2 && c.min_return_pairs <= c.beta_window && c.min_names >= 5 &&
         std::isfinite(c.clip_z) && c.clip_z > 0;
}
co::Status validate_config(const TargetReplayConfig& cfg) {
  if ((cfg.rule != TargetReplayRule::BaselineTargetV1 &&
       cfg.rule != TargetReplayRule::MonthlyTargetBudgetV2 && !aim_partial(cfg)) || !cfg.cadence ||
      cfg.cadence > max_dates || !std::isfinite(cfg.trade_fraction) ||
      cfg.trade_fraction <= 0 || cfg.trade_fraction > 1 ||
      !std::isfinite(cfg.monthly_budget) || cfg.monthly_budget <= 0 ||
      cfg.monthly_budget > 100 || !std::isfinite(cfg.one_way_bps) ||
      cfg.one_way_bps < 0 || cfg.one_way_bps > 10000 ||
      !std::isfinite(cfg.annual_borrow_bps) || cfg.annual_borrow_bps < 0 ||
      cfg.annual_borrow_bps > 100000)
    return co::Err(co::ErrorCode::InvalidArgument, "target replay: invalid recipe");
  if ((cfg.neutralize != TargetNeutralize::None && !neutralizing(cfg)) ||
      !std::isfinite(cfg.band_multiple) || cfg.band_multiple < 0 ||
      (neutralizing(cfg) &&
       (!price_risk_valid(cfg.price_risk) || !std::isfinite(cfg.neutralize_max_amplification) ||
        !(cfg.neutralize_max_amplification > 0) ||
        !(cfg.neutralize_max_excluded_share >= 0 && cfg.neutralize_max_excluded_share <= 1))))
    return co::Err(co::ErrorCode::InvalidArgument, "target replay: invalid construction recipe");
  // The id alone names the windows of price-risk-ind-v2, so no other pair can ride on it.
  if (cfg.neutralize == TargetNeutralize::PriceRiskIndV2 &&
      (cfg.price_risk.vol_window != price_risk_ind_v2_vol_window ||
       cfg.price_risk.adv_window != price_risk_ind_v2_adv_window))
    return co::Err(co::ErrorCode::InvalidArgument,
                   "target replay: price-risk-ind-v2 needs vol_window 126 and adv_window 252");
  // aim-partial-v5 (theta = trade_fraction, already in (0, 1] above): no no-trade band
  // (the dust band replaces it), dust_multiple in [0, 0.5], aim_leverage in [1, 2].
  // The negated ranges also refuse NaN. Its parameters are refused under every other
  // rule, so a baseline or v2 recipe cannot silently carry them.
  if (aim_partial(cfg) &&
      (cfg.band_multiple != 0 || !(cfg.dust_multiple >= 0 && cfg.dust_multiple <= 0.5) ||
       !(cfg.aim_leverage >= 1 && cfg.aim_leverage <= 2)))
    return co::Err(co::ErrorCode::InvalidArgument,
                   "target replay: aim-partial-v5 needs band_multiple 0, dust_multiple in "
                   "[0, 0.5] and aim_leverage in [1, 2]");
  if (!aim_partial(cfg) && (cfg.aim_leverage != 1.0 || cfg.dust_multiple != 0))
    return co::Err(co::ErrorCode::InvalidArgument,
                   "target replay: aim_leverage/dust_multiple are aim-partial-v5 only");
  // exit_rate in (0, 1] (NaN refused); below 1 the decaying exit snaps inside the dust
  // band, so it needs aim-partial-v5 with dust_multiple > 0 (else an exit never ends).
  if (!(cfg.exit_rate > 0 && cfg.exit_rate <= 1) ||
      (decaying_exit(cfg) && (!aim_partial(cfg) || !(cfg.dust_multiple > 0))))
    return co::Err(co::ErrorCode::InvalidArgument,
                   "target replay: exit_rate must be in (0, 1]; below 1 it needs "
                   "aim-partial-v5 with dust_multiple > 0");
  // hold-band-v1 (v8 R-4): aim-partial-v5 only, b in [0, 1] (NaN refused).
  if (hold_band_on(cfg) &&
      (!aim_partial(cfg) || !(*cfg.hold_band >= 0 && *cfg.hold_band <= 1)))
    return co::Err(co::ErrorCode::InvalidArgument,
                   "target replay: hold_band needs aim-partial-v5 and b in [0, 1]");
  // adv-hold-v1 (v8 R-5): Q finite >= 0 (0 = off); on, aim-partial-v5 only.
  if (!std::isfinite(cfg.adv_hold_q) || cfg.adv_hold_q < 0 ||
      (adv_hold_on(cfg) && !aim_partial(cfg)))
    return co::Err(co::ErrorCode::InvalidArgument,
                   "target replay: adv_hold_q must be finite >= 0 (0 = off) and needs "
                   "aim-partial-v5");
  return co::Ok();
}
// compute_price_exposures + neutralize_target scratch: per name the returns block,
// session logs, dollar sums, exposures/ok and regression rows (rounded up; with the
// industry ids' group slot per row, 113 of the 128 bytes), plus the per-interval
// market and, for the industry ids, the group slot table. Zero unless neutralizing
// (and for an invalid price-risk recipe, which validate_config refuses on its own;
// windows are then <= 4096).
u64 price_risk_scratch_bytes(const TargetReplayConfig& cfg, usize instruments) {
  if (!neutralizing(cfg) || !price_risk_valid(cfg.price_risk)) return 0;
  const u64 block = std::max(cfg.price_risk.beta_window, cfg.price_risk.vol_window);
  const u64 groups = neutralize_by_industry(cfg.neutralize) ? kGroupTableBytes : 0;
  return u64{instruments} * (block * sizeof(f64) + 128) + block * sizeof(f64) + groups;
}
// The v8 construction state per name (detail::DesiredState): hold-band-v1's rank_set and
// desired_prev, adv-hold-v1's ADV row and caps. Zero with every v8 option off.
u64 desired_state_bytes(const TargetReplayConfig& cfg, usize instruments) {
  const u64 per_name = (hold_band_on(cfg) ? 2U : 0U) + (adv_hold_on(cfg) ? 2U : 0U);
  return u64{instruments} * per_name * sizeof(f64);
}
co::Status validate_input(const TargetReplayInput& in, const TargetReplayConfig& cfg) {
  ATX_TRY_VOID(validate_config(cfg));
  if (!in.dates || in.dates > max_dates || !in.instruments || in.instruments > max_names ||
      in.decision_begin >= in.decision_end || in.decision_end > in.dates)
    return co::Err(co::ErrorCode::InvalidArgument, "target replay: bounded dimensions/window");
  const auto cells = in.dates * in.instruments;
  Budget budget{cfg.max_working_bytes};
  if (!budget.add(1, 65536) || !budget.add(in.instruments, sizeof(Ranked) + 2 * sizeof(f64)) ||
      !budget.add(in.decision_end - in.decision_begin, sizeof(TargetReplayDay)) ||
      !budget.add(1, price_risk_scratch_bytes(cfg, in.instruments)) ||
      !budget.add(1, desired_state_bytes(cfg, in.instruments)))
    return co::Err(co::ErrorCode::OutOfRange, "target replay: workspace budget");
  const bool prices = !in.close.empty() || !in.raw_close.empty() || !in.present.empty();
  if (in.signal.size() != cells || in.member.size() != cells ||
      in.session_keys.size() != in.dates || in.instrument_ids.size() != in.instruments ||
      (prices && (in.close.size() != cells || in.raw_close.size() != cells ||
                  in.present.size() != cells)) ||
      (!in.volume.empty() && in.volume.size() != cells) ||
      (!in.industry.empty() && in.industry.size() != cells))
    return co::Err(co::ErrorCode::InvalidArgument, "target replay: span geometry");
  if (in.session_keys.front() <= 0 || in.session_keys.back() >= 4'102'444'800'000'000'000LL ||
      std::adjacent_find(in.session_keys.begin(), in.session_keys.end(),
                        std::greater_equal<i64>{}) != in.session_keys.end() ||
      in.instrument_ids.front() == 0 ||
      std::adjacent_find(in.instrument_ids.begin(), in.instrument_ids.end(),
                        std::greater_equal<u64>{}) != in.instrument_ids.end())
    return co::Err(co::ErrorCode::InvalidArgument, "target replay: ordered axes");
  for (const auto s : in.session_keys) if (s % day_ns != 0)
    return co::Err(co::ErrorCode::InvalidArgument, "target replay: session must be UTC midnight");
  for (usize k = 0; k < cells; ++k) {
    if (in.member[k] > 1 || (in.member[k] ? !std::isfinite(in.signal[k]) :
                                         !std::isnan(in.signal[k])) ||
        (prices && in.present[k] > 1))
      return co::Err(co::ErrorCode::InvalidArgument, "target replay: support/value contract");
  }
  return co::Ok();
}
// desired_target, first half: the centred tied rank of every member in [-.5, .5] (0 for
// nonmembers, and for every name with fewer than two members); `row` = the members by
// ascending signal.
void member_ranks(std::span<const f64> signal, std::span<const u8> member,
                  std::vector<Ranked>& row, std::vector<f64>& target) {
  std::fill(target.begin(), target.end(), 0); row.clear();
  for (usize i = 0; i < signal.size(); ++i) if (member[i]) row.emplace_back(signal[i], i);
  std::sort(row.begin(), row.end());
  if (row.size() >= 2) for (usize b = 0; b < row.size();) {
    usize e = b + 1;
    while (e < row.size() && row[e].first == row[b].first) ++e;
    const f64 r = (static_cast<f64>(b) + static_cast<f64>(e - 1)) /
                 (2.0 * static_cast<f64>(row.size() - 1)) - 0.5;
    for (usize k = b; k < e; ++k) target[row[k].second] = r;
    b = e;
  }
}
// desired_target, second half: demean over the members of `row`, then gross 1 (a flat row
// stays flat).
void demean_gross_one(const std::vector<Ranked>& row, std::vector<f64>& target) {
  f64 sum = 0;
  for (const auto& value : row) sum += target[value.second];
  const f64 mean = row.empty() ? 0 : sum / static_cast<f64>(row.size());
  f64 gross = 0;
  for (const auto& value : row) {
    target[value.second] -= mean; gross += std::abs(target[value.second]);
  }
  if (gross > 0) for (const auto& value : row) target[value.second] /= gross;
}
// Same operations and order as IcComposition::finish. In particular a tied
// all-zero blend stays flat, and no daily renormalization changes partial fills.
// The two halves are the pre-v8 body split at the seam hold-band-v1 uses (statements
// unchanged, in the same order).
void desired_target(std::span<const f64> signal, std::span<const u8> member,
                    std::vector<Ranked>& row, std::vector<f64>& target) {
  member_ranks(signal, member, row, target);
  demean_gross_one(row, target);
}
// hold-band-v1 (TargetReplayConfig::hold_band): the tied ranks, the band on the members (the
// fresh desired value of a member is its rank, so rank_now and desired are one row), then the
// unchanged demean and gross 1. The kernel's counts go to `out`.
co::Status held_desired(std::span<const f64> signal, std::span<const u8> member, f64 band,
                        detail::DesiredState* state, std::vector<Ranked>& row,
                        std::vector<f64>& desired, ConstructionDay& out) {
  if (!state)
    return co::Err(co::ErrorCode::InvalidArgument,
                   "target replay: hold_band needs the construction state");
  member_ranks(signal, member, row, desired);
  ATX_TRY(const auto counts, eb::apply_hold_band(desired, desired, member, band, state->hold));
  out.hold_moved = counts.moved; out.hold_kept = counts.kept;
  out.hold_first_set = counts.first_set;
  demean_gross_one(row, desired);
  return co::Ok();
}
usize members_at(const TargetReplayInput& in, usize d) {
  const auto offset = d * in.instruments;
  usize members = 0;
  for (usize i = 0; i < in.instruments; ++i) members += in.member[offset + i] ? 1U : 0U;
  return members;
}
// aim-partial-v5 (TargetReplayConfig): on a rebalance decision each member moves by
// theta_i toward aim = aim_leverage * desired unless its gap is inside the dust band
// dust_multiple / N_d (it then keeps its weight: a small gap, never a large entry);
// otherwise members keep their weights. Nonmembers exit to 0 every decision (exit_rate 1;
// below 1 see the decaying exit below).
// With theta 1, aim_leverage 1 and dust 0 each operation is baseline-v1's (band 0,
// fraction 1): aim = 1.0 * desired is exact and the move is the same expression
// current + theta * (aim - current), so the plan is bit-identical (also when fused,
// as 1.0 * gap is exact). Deliberately no theta == 1 shortcut to `next = aim`: it
// would round differently from baseline (amended ruling R-a).
// exit_rate r < 1 (v6 prereg C2): a nonmember present at d moves current * (1 - r) at every
// decision and snaps to 0 inside the exit band dust_multiple / N_d (+inf when N_d = 0);
// an absent nonmember keeps the immediate exit. r == 1 never reaches that branch, so
// the default exit is the `next = 0` above bit for bit.
void aim_partial_weights(const TargetReplayInput& in, const TargetReplayConfig& cfg, usize d,
                         bool rebalance, const std::vector<f64>& desired,
                         std::vector<f64>& current, std::span<const f64> per_name_rate,
                         TargetReplayDay& out) {
  const auto offset = d * in.instruments;
  // -1 (off) dusts nothing, since every |gap| >= 0.
  f64 dust = -1;
  if (rebalance && cfg.dust_multiple > 0) {
    const usize members = members_at(in, d);
    if (members) dust = cfg.dust_multiple / static_cast<f64>(members);
  }
  const bool decaying = decaying_exit(cfg);
  f64 exit_band = 0;
  if (decaying) { // validate_config: dust_multiple > 0; update_weights: presence given
    const usize members = members_at(in, d);
    exit_band = members ? cfg.dust_multiple / static_cast<f64>(members)
                        : std::numeric_limits<f64>::infinity();
  }
  const f64 keep = 1.0 - cfg.exit_rate;
  // update_weights refused any other span (check_rates): empty, or one rate per name.
  assert(per_name_rate.empty() || per_name_rate.size() == in.instruments);
  const bool per_name = !per_name_rate.empty();
  out.applied_fraction = rebalance ? cfg.trade_fraction : 0;
  f64 squared = 0, rate_sum = 0;
  usize rated = 0;
  for (usize i = 0; i < in.instruments; ++i) {
    const bool live = in.member[offset + i] != 0;
    f64 next = 0;
    if (live && !rebalance) {
      next = current[i];
    } else if (live) {
      const f64 aim = cfg.aim_leverage * desired[i];
      const f64 gap = aim - current[i];
      const bool dusted = std::abs(gap) <= dust;
      if (dusted) ++out.construction.banded_names;
      const f64 theta = per_name ? per_name_rate[i] : cfg.trade_fraction;
      rate_sum += theta; ++rated;
      next = dusted ? current[i] : current[i] + theta * gap;
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
  // Per-name rates: the reported fraction is the members' mean rate (dusted included).
  if (per_name && rebalance) out.applied_fraction = rated ? rate_sum / static_cast<f64>(rated) : 0;
}
// A per-name rate span is aim-partial-v5 only and holds exactly one finite rate in
// [0, 1] per name; any other non-empty span is refused before a weight moves, never
// silently replaced by the fixed theta (T30 review Minor 2, T36 ruling R-c).
co::Status check_rates(const TargetReplayInput& in, const TargetReplayConfig& cfg,
                       std::span<const f64> per_name_rate) {
  if (per_name_rate.empty()) return co::Ok();
  if (!aim_partial(cfg) || per_name_rate.size() != in.instruments ||
      !std::all_of(per_name_rate.begin(), per_name_rate.end(),
                   [](f64 rate) { return rate >= 0 && rate <= 1; }))
    return co::Err(co::ErrorCode::InvalidArgument,
                   "target replay: per-name rates need aim-partial-v5 and exactly one rate in "
                   "[0, 1] per name");
  return co::Ok();
}
[[nodiscard]] co::Status update_weights(const TargetReplayInput& in,
                                        const TargetReplayConfig& cfg, usize d, bool rebalance,
                                        f64 spent, const std::vector<f64>& desired,
                                        std::vector<f64>& current, TargetReplayDay& out,
                                        std::span<const f64> per_name_rate = {}) {
  ATX_TRY_VOID(check_rates(in, cfg, per_name_rate));
  // A decaying exit holds only nonmembers PRESENT at d: presence is required (checked
  // here, not at admission, because the saved blend is admitted before its prices load).
  if (decaying_exit(cfg) && in.present.size() != in.dates * in.instruments)
    return co::Err(co::ErrorCode::InvalidArgument,
                   "target replay: exit_rate below 1 needs prices (presence; --role)");
  if (aim_partial(cfg)) {
    aim_partial_weights(in, cfg, d, rebalance, desired, current, per_name_rate, out);
    return co::Ok();
  }
  const auto offset = d * in.instruments;
  // No-trade band on rebalance decisions: -1 (off) bands nothing, since every
  // |desired - current| >= 0, so the default path is the unbanded arithmetic.
  f64 band = -1;
  if (rebalance && cfg.band_multiple > 0) {
    const usize members = members_at(in, d);
    if (members) band = cfg.band_multiple / static_cast<f64>(members);
  }
  f64 forced = 0, distance = 0;
  for (usize i = 0; i < in.instruments; ++i) {
    if (!in.member[offset + i]) forced += std::abs(current[i]);
    else if (rebalance) {
      const f64 gap = std::abs(desired[i] - current[i]);
      if (gap <= band) ++out.construction.banded_names; else distance += gap;
    }
  }
  f64 fraction = rebalance ? cfg.trade_fraction : 0;
  if (cfg.rule == TargetReplayRule::MonthlyTargetBudgetV2 && distance > 0)
    fraction = std::min(fraction, std::max(0.0, cfg.monthly_budget - spent - forced) / distance);
  out.applied_fraction = fraction;
  f64 squared = 0;
  for (usize i = 0; i < in.instruments; ++i) {
    const bool live = in.member[offset + i] != 0;
    const bool banded = rebalance && live && std::abs(desired[i] - current[i]) <= band;
    const f64 next = !live ? 0 : rebalance && !banded
      ? current[i] + fraction * (desired[i] - current[i]) : current[i];
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
// Rebalance decision d: the tied-rank desired target, then price-risk-v1 (or, for the
// industry ids, its within-groups twin on the decision's industry row). The
// exposures are computed once here per decision (target-independent; the NAV
// replay calls this once per decision for all its scenario books). Guard: a data
// refusal (Unavailable), amplification entry/residual gross above the cap, or an
// excluded-row gross share above the cap skips the rebalance. Contract and
// allocation errors are not data refusals and abort the replay.
// Locate-in-aim (NAV, v6 prereg C3): before the post-processing, a member that may not be
// shorted (no_short) keeps no negative desired weight, so the neutralization's
// intercept and beta columns re-balance the book around the zeroed shorts. Under the
// industry ids no_short is also the hold mask (review I3): a special-tier aim at 0 is
// reset to 0 after the within-group demeaning, so it cannot return as -(group mean).
// adv-hold-v1 (TargetReplayConfig::adv_hold_q): cap_i = Q ADV_i / (aim_leverage NAV) on every
// name with a nonzero desired weight (+inf elsewhere, never read), then one capped pro rata pass
// per side; the pass's record goes to `out`.
co::Status adv_capped(const TargetReplayConfig& cfg, detail::DesiredState* state,
                      std::vector<f64>& desired, ConstructionDay& out) {
  const usize n = desired.size();
  if (!state || state->adv_dollars.size() != n || !std::isfinite(state->nav) || !(state->nav > 0))
    return co::Err(co::ErrorCode::InvalidArgument,
                   "target replay: adv_hold_q needs the NAV replay's ADV row and NAV");
  auto& caps = state->caps;
  caps.assign(n, std::numeric_limits<f64>::infinity());
  const f64 dollars = cfg.aim_leverage * state->nav;
  for (usize i = 0; i < n; ++i) {
    if (desired[i] == 0) continue;
    const f64 adv = state->adv_dollars[i];
    if (!std::isfinite(adv) || adv < 0)
      return co::Err(co::ErrorCode::InvalidArgument,
                     "target replay: adv_hold_q needs a finite ADV >= 0 for every weighted name");
    caps[i] = cfg.adv_hold_q * adv / dollars;
  }
  ATX_TRY(const auto stats, eb::cap_pro_rata_one_pass(desired, caps));
  out.adv_clipped = stats.longs.clipped + stats.shorts.clipped;
  out.adv_clipped_mass = stats.longs.clipped_mass + stats.shorts.clipped_mass;
  out.adv_unplaced_mass = stats.longs.unplaced_mass + stats.shorts.unplaced_mass;
  out.adv_residual_names = stats.longs.residual_names + stats.shorts.residual_names;
  out.adv_residual_mass = stats.longs.residual_mass + stats.shorts.residual_mass;
  out.adv_residual_max = std::max(stats.longs.residual_max, stats.shorts.residual_max);
  return co::Ok();
}
// The end of form_desired: a rebalance that proceeds gets the adv-hold-v1 cap after the
// post-processing (off: `proceed` as it is, nothing touched).
co::Result<bool> finish_desired(const TargetReplayConfig& cfg, detail::DesiredState* state,
                                std::vector<f64>& desired, ConstructionDay& out, bool proceed) {
  if (proceed && adv_hold_on(cfg)) ATX_TRY_VOID(adv_capped(cfg, state, desired, out));
  return co::Ok(proceed);
}
// v8 hold-band-v1: the band acts on the members' tied ranks inside the desired target (between
// the ranks and the demean), on `state`; adv-hold-v1 caps the result of a rebalance that
// proceeds (finish_desired). Both off, the construction is the pre-v8 one.
co::Result<bool> form_desired(const TargetReplayInput& in, const TargetReplayConfig& cfg,
                              usize d, std::vector<Ranked>& row, std::vector<f64>& desired,
                              PriceRiskScratch& scratch, ConstructionDay& out,
                              std::span<const u8> no_short = {},
                              detail::DesiredState* state = nullptr) {
  const usize n = in.instruments, offset = d * n;
  if (!no_short.empty() && no_short.size() != n)
    return co::Err(co::ErrorCode::InvalidArgument, "target replay: no-short mask geometry");
  const auto member = in.member.subspan(offset, n);
  if (hold_band_on(cfg)) {
    ATX_TRY_VOID(held_desired(in.signal.subspan(offset, n), member, *cfg.hold_band, state, row,
                              desired, out));
  } else {
    desired_target(in.signal.subspan(offset, n), member, row, desired);
  }
  for (usize i = 0; i < no_short.size(); ++i) {
    if (!member[i] || !no_short[i] || !(desired[i] < 0)) continue;
    desired[i] = 0; ++out.locate_zeroed;
  }
  if (!neutralizing(cfg)) return finish_desired(cfg, state, desired, out, true);
  const PriceExposureInput prices{in.dates, n, in.close, in.raw_close, in.volume, in.present};
  const bool industry = neutralize_by_industry(cfg.neutralize);
  if (industry && in.industry.size() != in.dates * n)
    return co::Err(co::ErrorCode::InvalidArgument, "target replay: industry ids need the field");
  NeutralizeStats stats;
  const auto status = industry
      ? neutralize_price_risk_within_groups(prices, cfg.price_risk, d, desired, member,
                                            in.industry.subspan(offset, n), scratch, stats,
                                            no_short)
      : neutralize_price_risk(prices, cfg.price_risk, d, desired, member, scratch, stats);
  if (!status && status.error().code() != co::ErrorCode::Unavailable) return co::Err(status.error());
  out.neutralize_used = stats.used; out.neutralize_excluded = stats.excluded;
  out.neutralize_groups = stats.groups;
  out.neutralize_unknown_group_names = stats.unknown_group_names;
  out.neutralize_fallback_names = stats.fallback_names;
  out.neutralize_excluded_share = stats.gross > 0 ? stats.excluded_gross / stats.gross : 0.0;
  out.neutralize_amplification =
      status && stats.residual_gross > 0 ? stats.gross / stats.residual_gross : nan;
  auto outcome = NeutralizeOutcome::Applied;
  if (!status && stats.used < cfg.price_risk.min_names)
    outcome = NeutralizeOutcome::SkippedTooFewNames;
  else if (out.neutralize_excluded_share > cfg.neutralize_max_excluded_share)
    outcome = NeutralizeOutcome::SkippedExcludedShare;
  else if (!status)
    outcome = NeutralizeOutcome::SkippedRefused;
  else if (out.neutralize_amplification > cfg.neutralize_max_amplification)
    outcome = NeutralizeOutcome::SkippedAmplification; // NaN (flat target) never skips
  out.neutralize = outcome;
  return finish_desired(cfg, state, desired, out, outcome == NeutralizeOutcome::Applied);
}
void rough_return(const TargetReplayInput& in, const TargetReplayConfig& cfg,
                  std::span<const f64> weights, TargetReplayDay& out) {
  out.complete_gross_return = nan; out.complete_net_return = nan;
  out.modeled_trade_cost = out.turnover * cfg.one_way_bps / 10000;
  out.modeled_borrow_cost = out.short_weight * cfg.annual_borrow_bps / (10000 * 252);
  if (in.close.empty() || out.endpoint >= in.decision_end) return;
  out.return_mature = true;
  for (usize i = 0; i < in.instruments; ++i) {
    const f64 w = weights[i]; if (w == 0) continue;
    const auto a = out.entry * in.instruments + i, b = out.endpoint * in.instruments + i;
    const bool priced = in.present[a] && in.present[b] &&
        std::isfinite(in.close[a]) && std::isfinite(in.close[b]) &&
        std::isfinite(in.raw_close[a]) && std::isfinite(in.raw_close[b]) &&
        in.close[a] > 0 && in.close[b] > 0 && in.raw_close[a] > 0 && in.raw_close[b] > 0;
    const f64 r = priced ? in.close[b] / in.close[a] - 1 : nan;
    const f64 log_return = priced ? std::log(in.close[b]) - std::log(in.close[a]) : nan;
    const f64 raw_log_return = priced ? std::log(in.raw_close[b]) - std::log(in.raw_close[a]) : nan;
    const bool guarded = priced && (!std::isfinite(r) || std::abs(log_return) > 1.5 ||
                                    std::abs(log_return) > std::abs(raw_log_return) + .10);
    if (!priced || guarded) {
      ++out.missing_names; out.guarded_names += guarded ? 1U : 0U;
      out.missing_long += std::max(0.0, w); out.missing_short += std::max(0.0, -w);
    } else out.observed_return_component += w * r;
  }
  out.missing_gross = out.missing_long + out.missing_short;
  out.return_complete = out.missing_names == 0;
  if (out.return_complete) {
    out.complete_gross_return = out.observed_return_component;
    out.complete_net_return = out.complete_gross_return - out.modeled_trade_cost -
                              out.modeled_borrow_cost;
  }
}
} // namespace

co::Result<TargetReplayResult> replay_targets(const TargetReplayInput& in,
                                            const TargetReplayConfig& cfg) {
  ATX_TRY_VOID(validate_input(in, cfg));
  try {
    TargetReplayResult result; result.deployment_date = in.dates;
    result.days.reserve(in.decision_end - in.decision_begin);
    if (neutralizing(cfg) && (in.close.empty() || in.volume.size() != in.dates * in.instruments))
      return co::Err(co::ErrorCode::InvalidArgument,
                     "target replay: price-risk neutralization requires prices and volume");
    if (neutralize_by_industry(cfg.neutralize) && in.industry.empty())
      return co::Err(co::ErrorCode::InvalidArgument,
                     "target replay: industry neutralization requires the industry field");
    // adv-hold-v1 needs the execution ADV and the run's NAV: a NAV replay (or decide) option.
    if (adv_hold_on(cfg))
      return co::Err(co::ErrorCode::InvalidArgument,
                     "target replay: adv_hold_q is a NAV replay option (ADV and NAV)");
    std::vector<f64> current(in.instruments), desired(in.instruments);
    std::vector<Ranked> row; row.reserve(in.instruments);
    PriceRiskScratch price; // grows only when neutralizing
    detail::DesiredState state; // v8 hold-band-v1 state (empty with the band off)
    u32 month = 0; f64 spent = 0;
    for (usize d = in.decision_begin; d < in.decision_end; ++d) {
      TargetReplayDay day; day.decision = d; day.session = in.session_keys[d];
      day.entry = std::min(in.dates, d + 1); day.endpoint = std::min(in.dates, d + 2);
      day.calendar_month = calendar_month(day.session);
      if (day.calendar_month != month) { month = day.calendar_month; spent = 0; }
      bool rebalance = (d - in.decision_begin) % cfg.cadence == 0;
      if (rebalance) {
        // Qualified: the detail::DesiredState argument brings detail::form_desired in by ADL.
        ATX_TRY(rebalance, ::atx::impl::strategy::form_desired(in, cfg, d, row, desired, price,
                                                               day.construction, {}, &state));
      }
      day.construction.rebalance = rebalance;
      ATX_TRY_VOID(update_weights(in, cfg, d, rebalance, spent, desired, current, day));
      spent += day.turnover; day.month_turnover = spent;
      if (cfg.rule == TargetReplayRule::MonthlyTargetBudgetV2)
        day.budget_excess = std::max(0.0, spent - cfg.monthly_budget);
      if (result.deployment_date == in.dates && day.gross > 0) {
        result.deployment_date = d; result.deployment_turnover = day.turnover;
        day.deployment_turnover = day.turnover;
      }
      result.total_turnover += day.turnover; result.forced_turnover += day.forced_turnover;
      result.discretionary_turnover += day.discretionary_turnover;
      rough_return(in, cfg, current, day); result.days.push_back(day);
    }
    return co::Ok(std::move(result));
  } catch (const std::bad_alloc&) {
    return co::Err(co::ErrorCode::OutOfRange, "target replay: allocation failed");
  } catch (const std::length_error&) {
    return co::Err(co::ErrorCode::OutOfRange, "target replay: allocation extent");
  }
}

namespace {
struct SavedBlend {
  Json manifest;
  usize dates{}, names{}, begin{}, end{};
  std::vector<f64> signal, close, raw, volume; // volume only when requested (NAV replay)
  std::vector<u8> member, present;
  std::vector<i64> sessions;
  std::vector<u64> ids;
  TargetReplayInput view() const {
    return {dates, names, begin, end, signal, member, sessions, ids, close, raw, present, volume};
  }
};
co::Result<Json> pinned_json(const std::string& path, const std::string& pin) {
  if (!hash_valid(pin)) return co::Err(co::ErrorCode::InvalidArgument, "target replay: external pin");
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file || file.tellg() <= 0 || static_cast<u64>(file.tellg()) > (1ULL << 20))
    return co::Err(co::ErrorCode::InvalidArgument, "target replay: manifest missing/oversized");
  std::string text(static_cast<usize>(file.tellg()), '\0'); file.seekg(0);
  file.read(text.data(), static_cast<std::streamsize>(text.size()));
  if (!file || file.peek() != std::char_traits<char>::eof())
    return co::Err(co::ErrorCode::IoError, "target replay: manifest changed extent");
  ATX_TRY(auto digest, co::sha256_hex(text));
  if (digest != pin) return co::Err(co::ErrorCode::InvalidArgument, "target replay: manifest SHA");
  return co::Ok(Json::parse(text));
}
std::string hex(const std::array<std::byte, 32>& bytes) {
  constexpr char digits[] = "0123456789abcdef"; std::string result(64, '0');
  for (usize i = 0; i < bytes.size(); ++i) {
    const auto b = std::to_integer<unsigned>(bytes[i]);
    result[2 * i] = digits[b >> 4]; result[2 * i + 1] = digits[b & 15];
  }
  return result;
}
template<class T> co::Status payload(const std::filesystem::path& base, const Json& files,
                                    const std::string& name, std::vector<T>& out, usize count) {
  const auto bytes = static_cast<u64>(count) * sizeof(T);
  const auto& receipt = files.at(name);
  const auto expected = receipt.at("sha256").get<std::string>();
  if (receipt.at("bytes").get<u64>() != bytes || !hash_valid(expected))
    return co::Err(co::ErrorCode::InvalidArgument, "target replay: invalid payload receipt");
  std::ifstream file(base / name, std::ios::binary | std::ios::ate);
  if (!file || file.tellg() < 0 || static_cast<u64>(file.tellg()) != bytes)
    return co::Err(co::ErrorCode::IoError, "target replay: payload extent: " + name);
  file.seekg(0); out.resize(count);
  auto destination = std::as_writable_bytes(std::span(out)); co::Sha256 digest;
  for (usize offset = 0; offset < destination.size();) {
    const auto size = std::min(usize{65536}, destination.size() - offset);
    auto chunk = destination.subspan(offset, size);
    // SAFETY: char accesses object representations of trivially copyable numeric payloads.
    file.read(reinterpret_cast<char*>(chunk.data()), static_cast<std::streamsize>(size));
    if (!file) return co::Err(co::ErrorCode::IoError, "target replay: truncated payload");
    ATX_TRY_VOID(digest.update(chunk)); offset += size;
  }
  if (file.peek() != std::char_traits<char>::eof())
    return co::Err(co::ErrorCode::IoError, "target replay: payload changed extent");
  ATX_TRY(auto actual, digest.finalize());
  if (hex(actual) != expected)
    return co::Err(co::ErrorCode::InvalidArgument, "target replay: payload SHA: " + name);
  return co::Ok();
}
// Two saved-blend compositions are admitted: the fixed equal-family/equal-within
// blend, and a blend built with externally pinned per-candidate weights (IC runner
// --composition-weights). The IC runner writes composition_weights_sha256 exactly
// for the pinned variant (absent, not null, otherwise); it stays in the manifest,
// which every summary publishes as source_bindings.
bool admitted_signal_semantics(const Json& j) {
  const auto& semantics = j.at("signal_semantics");
  if (!semantics.is_string()) return false;
  const bool pinned = semantics == "exact-pre-target-composition;pinned-candidate-weights;"
                                   "missing-or-unoriented-neutral-fixed-denominator";
  if (!pinned)
    return semantics == "exact-pre-target-composition;equal-family/equal-within;"
                        "missing-or-unoriented-neutral-fixed-denominator" &&
           !j.contains("composition_weights_sha256");
  const auto& weights = j.at("composition_weights_sha256");
  return weights.is_string() && hash_valid(weights.get<std::string>());
}
co::Status admit_saved(const TargetReplayRunConfig& cfg, SavedBlend& out,
                       bool with_volume = false) {
  ATX_TRY(out.manifest, pinned_json(cfg.combined_path, cfg.combined_sha256));
  const auto& j = out.manifest;
  const auto d = j.at("dates").get<u64>(), n = j.at("instruments").get<u64>();
  if (j.at("schema") != "atx.dsl-combined-signal/v1" || j.at("status") != "complete" ||
      j.at("layout") != "date-major-little-endian" || j.at("role_window_required") != true ||
      j.at("actual_trades_or_returns") != false || !admitted_signal_semantics(j) ||
      j.at("member_semantics") != "decision-member-and-source-present-and-finite-positive-close;independent-of-component-coverage" ||
      j.at("finite_semantics") != "one-iff-saved-f64-is-finite;nonmembers-NaN;zero-is-valid-neutral-signal" ||
      !d || d > max_dates || !n || n > max_names)
    return co::Err(co::ErrorCode::InvalidArgument, "target replay: saved blend contract");
  const auto role = j.at("role").get<std::string>();
  if (role != "train" && role != "validation" && role != "holdout")
    return co::Err(co::ErrorCode::InvalidArgument, "target replay: saved role identifier");
  for (const auto* key : {"role_manifest_sha256", "source_sha256", "library_sha256",
                          "train_manifest_sha256", "run_recipe_sha256",
                          "orientation_candidates_sha256"})
    if (!hash_valid(j.at(key).get<std::string>()))
      return co::Err(co::ErrorCode::InvalidArgument, "target replay: provenance pin");
  out.dates = static_cast<usize>(d); out.names = static_cast<usize>(n);
  out.begin = j.at("score_begin").get<usize>(); out.end = j.at("score_end").get<usize>();
  if (out.begin >= out.end || out.end > out.dates)
    return co::Err(co::ErrorCode::InvalidArgument, "target replay: saved score window");
  Budget budget{cfg.target.max_working_bytes}; const auto cells = d * n;
  // Includes metadata parse/output overlap, finite-mask validation, optional
  // close/raw/presence and temporary role membership/axis verification vectors,
  // plus the optional f64 volume payload (8 B/cell) only when volume is requested.
  const u64 cell_bytes = cfg.role_path.empty() ? 10 : (with_volume ? 36 : 28);
  if (!budget.add(1, metadata_bytes) || !budget.add(cells, cell_bytes) ||
      !budget.add(d, 2 * sizeof(i64) + sizeof(TargetReplayDay)) ||
      !budget.add(n, 2 * sizeof(u64) + sizeof(Ranked) + 2 * sizeof(f64)))
    return co::Err(co::ErrorCode::OutOfRange, "target replay: aggregate input/workspace budget");
  const auto base = std::filesystem::path(cfg.combined_path).parent_path();
  const auto prefix = role + "_combined"; const auto& files = j.at("files");
  if (files.size() != 5)
    return co::Err(co::ErrorCode::InvalidArgument, "target replay: saved file set");
  ATX_TRY_VOID(payload(base, files, prefix + ".f64", out.signal, static_cast<usize>(cells)));
  ATX_TRY_VOID(payload(base, files, prefix + "_member.u8", out.member, static_cast<usize>(cells)));
  ATX_TRY_VOID(payload(base, files, prefix + "_sessions.i64", out.sessions, out.dates));
  ATX_TRY_VOID(payload(base, files, prefix + "_ids.u64", out.ids, out.names));
  std::vector<u8> finite;
  ATX_TRY_VOID(payload(base, files, prefix + "_finite.u8", finite, static_cast<usize>(cells)));
  u64 finite_count = 0, members = 0;
  for (usize k = 0; k < finite.size(); ++k) {
    if (finite[k] > 1 || finite[k] != static_cast<u8>(std::isfinite(out.signal[k])))
      return co::Err(co::ErrorCode::InvalidArgument, "target replay: finite-mask mismatch");
    finite_count += finite[k]; members += out.member[k];
  }
  if (j.at("finite_cells").get<u64>() != finite_count ||
      j.at("member_cells").get<u64>() != members)
    return co::Err(co::ErrorCode::InvalidArgument, "target replay: support count mismatch");
  return validate_input(out.view(), cfg.target);
}
co::Status load_prices(const TargetReplayRunConfig& cfg, SavedBlend& out,
                       bool with_volume = false) {
  if (cfg.role_path.empty()) return co::Ok();
  ATX_TRY(auto j, pinned_json(cfg.role_path, cfg.role_sha256));
  // Same contract as engine::data::read_strategy_role (strategy_data.cpp): raw
  // share volume, present => finite and >= 0, absent => NaN. Checked only when
  // requested so the target-replay recipe and its fixtures stay unchanged.
  if (with_volume && j.at("volume_basis") != "raw-share-volume")
    return co::Err(co::ErrorCode::InvalidArgument, "target replay: price-role volume basis");
  if (cfg.role_sha256 != out.manifest.at("role_manifest_sha256").get<std::string>() ||
      j.at("schema") != "atx.recent-research-role/v1" || j.at("status") != "complete" ||
      j.at("source_sha256") != out.manifest.at("source_sha256") ||
      j.at("instrument_namespace") != "spiderrock.securityID" ||
      j.at("close_basis") != "f64(raw-f32-close)*f64-cumulReturnFactor" ||
      j.at("clock_recipe") != "modeled-session+22h-mark+23h-decision-v1" ||
      j.at("common_stock_verified") != false || j.at("historical_vintage_verified") != false ||
      j.at("dates").get<usize>() != out.dates || j.at("instruments").get<usize>() != out.names ||
      j.at("score_begin").get<usize>() != out.begin || j.at("score_end").get<usize>() != out.end)
    return co::Err(co::ErrorCode::InvalidArgument, "target replay: price-role recipe/identity");
  const auto base = std::filesystem::path(cfg.role_path).parent_path(); const auto& files = j.at("files");
  std::vector<i64> sessions; std::vector<u64> ids;
  ATX_TRY_VOID(payload(base, files, "sessions.i64", sessions, out.dates));
  ATX_TRY_VOID(payload(base, files, "ids.u64", ids, out.names));
  if (sessions != out.sessions || ids != out.ids)
    return co::Err(co::ErrorCode::InvalidArgument, "target replay: price-role axes");
  const auto cells = out.dates * out.names;
  ATX_TRY_VOID(payload(base, files, "close.f64", out.close, cells));
  ATX_TRY_VOID(payload(base, files, "raw_close.f64", out.raw, cells));
  ATX_TRY_VOID(payload(base, files, "present.u8", out.present, cells));
  std::vector<u8> member; ATX_TRY_VOID(payload(base, files, "member.u8", member, cells));
  for (usize k = 0; k < cells; ++k) {
    if (member[k] > 1 || out.present[k] > 1 ||
        (out.present[k] ? (!std::isfinite(out.close[k]) || out.close[k] <= 0 ||
                          !std::isfinite(out.raw[k]) || out.raw[k] <= 0)
                        : (!std::isnan(out.close[k]) || !std::isnan(out.raw[k]))) ||
        out.member[k] != static_cast<u8>(member[k] && out.present[k] &&
                                        std::isfinite(out.close[k]) && out.close[k] > 0))
      return co::Err(co::ErrorCode::InvalidArgument, "target replay: price-role presence/support");
  }
  if (!with_volume) return co::Ok();
  ATX_TRY_VOID(payload(base, files, "volume.f64", out.volume, cells));
  for (usize k = 0; k < cells; ++k) {
    if (out.present[k] ? (!std::isfinite(out.volume[k]) || out.volume[k] < 0)
                       : !std::isnan(out.volume[k]))
      return co::Err(co::ErrorCode::InvalidArgument, "target replay: price-role volume contract");
  }
  return co::Ok();
}
// ---- v8 E-25: the NAV verb's label role (detail::check_label_role / load_label_role) ----
// The payloads prepare_recent_research.py --delisting-returns patches (DELISTING_RETURN_RULE):
// the only files a label role may change.
bool label_patchable(const std::string& name) {
  return name == "close.f64" || name == "raw_close.f64" || name == "volume.f64" ||
         name == "present.u8";
}
auto label_refused(const std::string& why) {
  return co::Err(co::ErrorCode::InvalidArgument, "nav replay: --label-role refused: " + why);
}
std::string sealed_label() {
  return "it reaches the " + std::string(rw::kResearchWindowId) + " seal (" +
         std::string(rw::kSealBeginDate) + ")";
}
// The value at `path` inside `j`; nullptr when a key is missing.
const Json* json_at(const Json& j, std::initializer_list<const char*> path) {
  const Json* at = &j;
  for (const char* key : path) {
    if (!at->is_object() || !at->contains(key)) return nullptr;
    at = &at->at(key);
  }
  return at;
}
// A shared file by what it carries (the refusal names the axis).
std::string shared_file(const std::string& name) {
  if (name == "sessions.i64") return "the dates (sessions.i64)";
  if (name == "ids.u64") return "the instruments (ids.u64)";
  if (name == "member.u8") return "the membership (member.u8)";
  return name;
}
// The members a label role declares cleared on its termination sessions (DELISTING_RETURN_RULE:
// member[T] = 0 where the role's lagged membership kept an absent name): universe.delisting
// returns_applied true and applied.members_cleared_on_termination_session N > 0; 0 otherwise.
u64 declared_cleared(const Json& label) {
  const Json* on = json_at(label, {"universe", "delisting", "returns_applied"});
  const Json* n = json_at(label, {"universe", "delisting", "applied",
                                  "members_cleared_on_termination_session"});
  if (!on || !on->is_boolean() || !on->get<bool>() || !n || !n->is_number_integer() ||
      n->get<i64>() <= 0)
    return 0;
  return static_cast<u64>(n->get<i64>());
}
// Whether the two manifests pin different member.u8 bytes (only a declared clearing may).
bool member_differs(const Json& role, const Json& label) {
  const Json* a = json_at(role, {"files", "member.u8", "sha256"});
  const Json* b = json_at(label, {"files", "member.u8", "sha256"});
  return a && b && *a != *b;
}
// The manifest rule of detail::check_label_role (`role`: --role's pinned manifest).
co::Status check_label_manifests(const Json& role, const Json& label) {
  if (!role.is_object() || !label.is_object())
    return label_refused("a role manifest is not a JSON object");
  // The seal first: a label role reaching it is refused whatever else it shares.
  if (label.contains("score_end_ns") && (!label.at("score_end_ns").is_number_integer() ||
                                         label.at("score_end_ns").get<i64>() > rw::kSealBeginNs))
    return label_refused(sealed_label());
  // The membership: member.u8 is --role's, or differs by the members the label role declares
  // cleared on termination sessions (verified cell by cell when the payloads load); only then
  // may score_member_counts, its per-row count, differ.
  const bool cleared = member_differs(role, label);
  if (cleared && declared_cleared(label) == 0)
    return label_refused("the membership (member.u8) differs from --role's (only the members a "
                         "--delisting-returns role declares cleared on termination sessions, "
                         "universe.delisting.applied.members_cleared_on_termination_session, may)");
  const auto own = [cleared](const std::string& key) {
    return key == "files" || key == "universe" || (cleared && key == "score_member_counts");
  };
  for (const auto& item : role.items()) {
    const auto& key = item.key();
    if (own(key)) continue;
    if (!label.contains(key) || label.at(key) != item.value())
      return label_refused("manifest key '" + key + "' differs from --role's (dates, instruments, "
                           "score window, source and base are shared)");
  }
  for (const auto& item : label.items())
    if (!own(item.key()) && !role.contains(item.key()))
      return label_refused("manifest key '" + item.key() + "' is not --role's");
  const Json* mine = json_at(role, {"files"});
  const Json* theirs = json_at(label, {"files"});
  if (!mine || !theirs || !mine->is_object() || !theirs->is_object() ||
      mine->size() != theirs->size())
    return label_refused("its file set differs from --role's");
  for (const auto& item : mine->items()) {
    const auto& name = item.key();
    if (!theirs->contains(name))
      return label_refused("its file set differs from --role's (no " + name + ")");
    const Json* a = json_at(item.value(), {"bytes"});
    const Json* b = json_at(theirs->at(name), {"bytes"});
    if (!a || !b || *a != *b)
      return label_refused(shared_file(name) + " extent differs from --role's");
    if (label_patchable(name) || (cleared && name == "member.u8")) continue;
    a = json_at(item.value(), {"sha256"});
    b = json_at(theirs->at(name), {"sha256"});
    if (!a || !b || *a != *b)
      return label_refused(shared_file(name) + " differs from --role's (only close, raw_close, "
                           "volume and present may: the payloads --delisting-returns patches)");
  }
  const bool restricted = role.contains("universe");
  if (restricted != label.contains("universe"))
    return label_refused("base: one role carries a universe restriction and the other does not");
  if (!restricted) return co::Ok();
  const auto& u = role.at("universe");
  const auto& v = label.at("universe");
  const auto same = [&u, &v](std::initializer_list<const char*> path) {
    const Json* a = json_at(u, path);
    const Json* b = json_at(v, path);
    return a && b && *a == *b;
  };
  if (!same({"id"})) return label_refused("base: universe.id differs from --role's");
  if (!same({"base_role", "manifest_sha256"}) || !same({"base_role", "member_sha256"}))
    return label_refused("base: universe.base_role differs from --role's");
  if (!same({"inputs", "identity_bridge", "manifest_sha256"}))
    return label_refused("base: universe.inputs.identity_bridge differs from --role's");
  if (!same({"inputs", "sic_events", "manifest_sha256"}))
    return label_refused("base: universe.inputs.sic_events differs from --role's");
  if (json_at(u, {"inputs", "delisting", "manifest_sha256"}) &&
      !same({"inputs", "delisting", "manifest_sha256"}))
    return label_refused("base: universe.inputs.delisting differs from --role's");
  return co::Ok();
}
const char* rule_name(TargetReplayRule rule) {
  switch (rule) {
  case TargetReplayRule::BaselineTargetV1: return "baseline-target-v1";
  case TargetReplayRule::MonthlyTargetBudgetV2: return "monthly-target-budget-v2";
  case TargetReplayRule::AimPartialV5: return "aim-partial-v5";
  }
  return "unknown";
}

// ---- construction options (neutralization, no-trade band, the aim-partial-v5 rule):
// recipe, rule id, CSV, summary. Nothing here is emitted unless an option is
// non-default or the rule is aim-partial-v5 (whose CSV banded_names is the dust count).
bool construction_on(const TargetReplayConfig& c) {
  return neutralizing(c) || c.band_multiple > 0 || aim_partial(c);
}
std::string decimal(f64 x) { // shortest round-trip spelling
  std::array<char, 64> text{};
  const auto written = std::to_chars(text.data(), text.data() + text.size(), x);
  return std::string(text.data(), written.ptr);
}
std::string rule_id(const TargetReplayConfig& c) {
  std::string id = rule_name(c.rule);
  if (neutralizing(c)) id += std::string("+neutral-") + neutralize_name(c.neutralize);
  if (c.band_multiple > 0) id += "+band-" + decimal(c.band_multiple);
  if (hold_band_declared(c)) id += "+hold-band-" + decimal(*c.hold_band);
  if (adv_hold_on(c)) id += "+adv-hold-" + decimal(c.adv_hold_q);
  return id;
}
const char* outcome_label(NeutralizeOutcome outcome) {
  switch (outcome) {
  case NeutralizeOutcome::NotAttempted: return "not-attempted";
  case NeutralizeOutcome::Applied: return "applied";
  case NeutralizeOutcome::SkippedTooFewNames: return "skipped-too-few-names";
  case NeutralizeOutcome::SkippedExcludedShare: return "skipped-excluded-share";
  case NeutralizeOutcome::SkippedRefused: return "skipped-refused";
  case NeutralizeOutcome::SkippedAmplification: return "skipped-amplification";
  }
  return "unknown";
}
f64 quantile(std::span<const f64> sorted, f64 q) {
  if (sorted.empty()) return nan;
  const f64 h = static_cast<f64>(sorted.size() - 1) * q;
  const auto lo = static_cast<usize>(std::floor(h));
  const usize hi = std::min(lo + 1, sorted.size() - 1);
  return sorted[lo] + (h - static_cast<f64>(lo)) * (sorted[hi] - sorted[lo]);
}
Json finite_or_null(f64 x) { return std::isfinite(x) ? Json(x) : Json(nullptr); }
constexpr const char* exit_rate_rule_declaration =
    "exit_rate r < 1 (v6 prereg C2): at every decision a nonmember present at d moves next = "
    "current * (1 - r) and is set to 0 when |next| <= dust_multiple / N_d (N_d = members at "
    "d; every name when N_d = 0); a nonmember absent at d exits to 0 at once (stale carry "
    "and write-off unchanged); the moves count as forced turnover; r = 1 is the immediate "
    "exit";
constexpr const char* hold_band_rule_declaration =
    "hold-band-v1 (v8 R-4, rank hysteresis): on every rebalance decision each member's centred "
    "tied rank r_i of the blend (in [-.5, .5], range 1) is compared with rank_set_i, the rank at "
    "which its current desired value was set; if rank_set_i is unset or |r_i - rank_set_i| > "
    "hold_band the member takes r_i as its desired value and rank_set_i = r_i, otherwise it "
    "keeps its previous desired value; then the unchanged demean, gross 1, locate zeroing and "
    "neutralization. The state is per name and carried across decisions; a nonmember keeps its "
    "state and follows the exit rule unchanged; the state advances on every cadence decision, "
    "a neutralization-skipped one included";
constexpr const char* adv_hold_rule_declaration =
    "adv-hold-v1 (v8 R-5, ADV holding cap): on every rebalance decision that proceeds, after the "
    "post-processing (the projection), |desired_i| <= adv_hold_q * ADV_i / (aim_leverage * NAV), "
    "ADV_i = the raw-dollar ADV the execution trade limit reads for this decision's fills "
    "(session d + 1: present raw_close x volume over rows [d + 1 - w, d + 1) / w, w = "
    "liquidity_window; rows <= d only), NAV = initial_nav (the run's, every book and capacity "
    "multiple alike); each clipped name is set to its cap and the clipped mass is added to the "
    "same side's unclipped names pro rata to their weight, one pass (side gross preserved; "
    "unplaced when no unclipped name is left on the side); names the pass lifts above their cap "
    "stay and are reported as the residual breach (summary construction.adv_hold). Ruling "
    "E-15: the cap uses the run's initial NAV for every capacity book, so the capacity book at "
    "multiple m holds up to m x adv_hold_q of ADV (the initial-NAV rule stressed at NAV x m, "
    "not a cap set per multiple)";
// The aim_partial declarations' nonmember clause: the immediate exit (exit_rate 1: the
// default text byte for byte) or, below 1, a pointer to exit_rate_rule.
const char* nonmember_exit(const TargetReplayConfig& c) {
  return decaying_exit(c) ? "nonmembers follow exit_rate_rule" : "nonmembers exit to 0";
}
Json construction_recipe(const TargetReplayConfig& c) {
  Json j = Json::object();
  if (neutralizing(c)) {
    const auto& p = c.price_risk;
    const bool industry = neutralize_by_industry(c.neutralize);
    j["neutralize"] = neutralize_name(c.neutralize);
    j["desired_target_postprocess"] = neutralize_name(c.neutralize);
    j["price_risk"] = Json{{"beta_window", p.beta_window}, {"vol_window", p.vol_window},
        {"adv_window", p.adv_window}, {"min_return_pairs", p.min_return_pairs},
        {"min_names", p.min_names}, {"clip_z", p.clip_z},
        {"exposures", "trailing beta vs equal-weight market, sample vol, log mean raw dollar "
                      "volume; windows end at the decision session (strategy_price_exposures)"},
        {"method", industry
             ? "OLS residual of the tied-rank target on [industry group indicators, z_beta, "
               "z_vol, z_log_adv] by Frisch-Waugh-Lovell (clipped z over member&ok rows, then "
               "target and z demeaned within group, then OLS on [1, demeaned z]), rescaled "
               "to the entry gross; member&!ok rows to zero; exposures computed once per "
               "decision"
             : "OLS residual of the tied-rank target on [1, z_beta, z_vol, z_log_adv] "
               "(clipped z over member&ok rows), rescaled to the entry gross; "
               "member&!ok rows to zero; exposures computed once per decision"}};
    if (industry)
      j["industry"] = Json{{"field", industry_group_field}, {"min_group_names", kMinGroupNames},
          {"max_group_id", kMaxGroupId},
          {"unknown", "a NaN id: all such names form one residual group"},
          {"fallback", "a group (the residual one included) with fewer than min_group_names "
                       "member&ok names has no own level: it loads on the common intercept, "
                       "i.e. all such names are demeaned together as one pooled group"},
          {"clock", "the decision session's row of the pinned point-in-time field"}};
    j["neutralize_guard"] = Json{{"max_amplification", c.neutralize_max_amplification},
        {"max_excluded_gross_share", c.neutralize_max_excluded_share},
        {"amplification", "entry gross / residual gross before rescale"},
        {"on_skip", "Unavailable data refusal or a cap breach skips the rebalance: current "
                    "weights kept, forced exits still apply; contract errors abort"}};
  }
  if (c.band_multiple > 0) {
    j["band_multiple"] = c.band_multiple;
    j["band"] = "rebalance decision: a member with |desired - current| <= band_multiple / N_d "
                "(N_d = members at d) keeps its current weight, others move by the rule's "
                "fraction; banded names are excluded from the monthly-budget-v2 distance";
  }
  if (aim_partial(c)) {
    j["theta"] = c.trade_fraction;
    j["dust_multiple"] = c.dust_multiple;
    j["aim_leverage"] = c.aim_leverage;
    j["rate"] = aim_rate(c);
    j["aim_partial"] = std::string("rebalance decision: each member moves next = current + "
        "theta * (aim_leverage * desired - current) unless |aim_leverage * desired - "
        "current| <= dust_multiple / N_d (N_d = members at d; 0 = off), which keeps its "
        "weight and is counted in banded_names; non-rebalance decisions keep member "
        "weights; ") + nonmember_exit(c) + "; theta = trade_fraction (rate fixed); "
        "monthly_budget unused";
  }
  if (decaying_exit(c)) { // aim-partial-v5 only (validate_config); absent by default
    j["exit_rate"] = c.exit_rate;
    j["exit_rate_rule"] = exit_rate_rule_declaration;
  }
  if (hold_band_declared(c)) { // v8 R-4; absent unset and at b = 0 (the identity)
    j["hold_band"] = *c.hold_band;
    j["hold_band_rule"] = hold_band_rule_declaration;
  }
  if (adv_hold_on(c)) { // v8 R-5; absent at Q = 0 (off)
    j["adv_hold_q"] = c.adv_hold_q;
    j["adv_hold_rule"] = adv_hold_rule_declaration;
  }
  return j;
}
// construction.v5 (see detail::aim_partial_summary_json).
Json aim_partial_summary(const TargetReplayConfig& c,
                         std::span<const detail::AimPartialDecision> decisions) {
  f64 gross = 0, net = 0, share = 0;
  usize shared = 0;
  for (const auto& d : decisions) {
    gross += d.gross; net += d.net;
    if (!d.members) continue;
    share += static_cast<f64>(d.held_names) / static_cast<f64>(d.members); ++shared;
  }
  const auto mean = [](f64 sum, usize count) {
    return count ? Json(sum / static_cast<f64>(count)) : Json(nullptr);
  };
  auto j = Json{{"theta", c.trade_fraction}, {"dust_multiple", c.dust_multiple},
      {"aim_leverage", c.aim_leverage}, {"rate", aim_rate(c)}, {"decisions", decisions.size()},
      {"mean_gross", mean(gross, decisions.size())}, {"mean_net", mean(net, decisions.size())},
      {"mean_held_share", mean(share, shared)}};
  if (decaying_exit(c)) j["exit_rate"] = c.exit_rate;
  return j;
}
// Industry ids, over the applied decisions: demeaning groups (the pooled fallback
// counts as one), unknown-id names and fallback-pooled names (min / median / max;
// null without an applied decision).
Json industry_summary(std::span<const ConstructionDay> decisions) {
  std::vector<f64> groups, unknown, fallback;
  for (const auto& d : decisions) {
    if (d.neutralize != NeutralizeOutcome::Applied) continue;
    groups.push_back(static_cast<f64>(d.neutralize_groups));
    unknown.push_back(static_cast<f64>(d.neutralize_unknown_group_names));
    fallback.push_back(static_cast<f64>(d.neutralize_fallback_names));
  }
  const auto spread = [](std::vector<f64>& v) {
    std::sort(v.begin(), v.end());
    return Json{{"min", v.empty() ? Json(nullptr) : Json(v.front())},
                {"median", finite_or_null(quantile(v, .5))},
                {"max", v.empty() ? Json(nullptr) : Json(v.back())}};
  };
  return Json{{"field", industry_group_field}, {"applied_decisions", groups.size()},
              {"groups", spread(groups)}, {"unknown_group_names", spread(unknown)},
              {"fallback_names", spread(fallback)}};
}
// hold-band-v1 (v8 R-4) over the decisions the kernel saw a member on (moved + kept > 0):
// totals of moved (first set included) and kept members, and the mean kept share.
Json hold_band_summary(const TargetReplayConfig& c, std::span<const ConstructionDay> decisions) {
  usize seen = 0, moved = 0, kept = 0, first = 0;
  f64 share = 0;
  for (const auto& d : decisions) {
    const usize members = d.hold_moved + d.hold_kept;
    if (!members) continue;
    ++seen; moved += d.hold_moved; kept += d.hold_kept; first += d.hold_first_set;
    share += static_cast<f64>(d.hold_kept) / static_cast<f64>(members);
  }
  return Json{{"band", *c.hold_band}, {"rule", "recipe hold_band_rule"}, {"decisions", seen},
      {"moved_names_total", moved}, {"kept_names_total", kept},
      {"first_set_names_total", first},
      {"mean_kept_share", seen ? Json(share / static_cast<f64>(seen)) : Json(nullptr)}};
}
// adv-hold-v1 (v8 R-5) over the rebalances the cap pass ran on: clipped names and mass, the
// unplaced mass, and the residual breach left by the one pass (desired-weight units).
Json adv_hold_summary(const TargetReplayConfig& c, std::span<const ConstructionDay> decisions) {
  usize passes = 0, clipped = 0, clipped_max = 0, breached = 0, over = 0, over_max = 0;
  f64 mass = 0, mass_max = 0, unplaced = 0, unplaced_max = 0, residual = 0, residual_max = 0;
  f64 excess_max = 0;
  for (const auto& d : decisions) {
    if (!d.rebalance) continue;
    ++passes;
    clipped += d.adv_clipped; clipped_max = std::max(clipped_max, d.adv_clipped);
    mass += d.adv_clipped_mass; mass_max = std::max(mass_max, d.adv_clipped_mass);
    unplaced += d.adv_unplaced_mass; unplaced_max = std::max(unplaced_max, d.adv_unplaced_mass);
    breached += d.adv_residual_names ? 1U : 0U;
    over += d.adv_residual_names; over_max = std::max(over_max, d.adv_residual_names);
    residual += d.adv_residual_mass; residual_max = std::max(residual_max, d.adv_residual_mass);
    excess_max = std::max(excess_max, d.adv_residual_max);
  }
  const auto mean = [passes](f64 sum) {
    return passes ? Json(sum / static_cast<f64>(passes)) : Json(nullptr);
  };
  return Json{{"q", c.adv_hold_q}, {"rule", "recipe adv_hold_rule"},
      {"units", "desired weight (x aim_leverage x initial_nav = dollars)"},
      {"decisions", passes}, {"clipped_names_total", clipped},
      {"clipped_names_mean", mean(static_cast<f64>(clipped))},
      {"clipped_names_max", clipped_max}, {"clipped_mass_mean", mean(mass)},
      {"clipped_mass_max", mass_max}, {"unplaced_mass_total", unplaced},
      {"unplaced_mass_max", unplaced_max},
      {"residual_breach", {{"decisions", breached}, {"names_total", over},
                           {"names_max", over_max}, {"mass_mean", mean(residual)},
                           {"mass_max", residual_max}, {"excess_max", excess_max}}}};
}
Json construction_summary(const TargetReplayConfig& c, std::span<const ConstructionDay> decisions) {
  usize cadence_days = 0, rebalanced = 0, attempted = 0, applied = 0, banded = 0;
  usize too_few = 0, excluded = 0, refused = 0, amplified = 0;
  std::vector<f64> used, amplification; f64 excluded_max = 0;
  for (const auto& d : decisions) {
    const bool attempt = d.neutralize != NeutralizeOutcome::NotAttempted;
    const bool skip = attempt && d.neutralize != NeutralizeOutcome::Applied;
    cadence_days += (d.rebalance || skip) ? 1U : 0U;
    rebalanced += d.rebalance ? 1U : 0U; banded += d.banded_names;
    if (!attempt) continue;
    ++attempted; used.push_back(static_cast<f64>(d.neutralize_used));
    excluded_max = std::max(excluded_max, d.neutralize_excluded_share);
    switch (d.neutralize) {
    case NeutralizeOutcome::SkippedTooFewNames: ++too_few; break;
    case NeutralizeOutcome::SkippedExcludedShare: ++excluded; break;
    case NeutralizeOutcome::SkippedRefused: ++refused; break;
    case NeutralizeOutcome::SkippedAmplification: ++amplified; break;
    case NeutralizeOutcome::Applied:
      ++applied;
      if (std::isfinite(d.neutralize_amplification))
        amplification.push_back(d.neutralize_amplification);
      break;
    case NeutralizeOutcome::NotAttempted: break;
    }
  }
  std::sort(used.begin(), used.end());
  std::sort(amplification.begin(), amplification.end());
  const Json used_min = used.empty() ? Json(nullptr) : Json(static_cast<u64>(used.front()));
  const Json amplification_max =
      amplification.empty() ? Json(nullptr) : Json(amplification.back());
  Json skips{{"too-few-names", too_few}, {"excluded-share", excluded}, {"refused", refused},
             {"amplification", amplified}};
  Json body{{"rule_id", rule_id(c)}, {"neutralize", neutralize_name(c.neutralize)},
      {"band_multiple", c.band_multiple}, {"decisions", decisions.size()},
      {"cadence_rebalance_decisions", cadence_days}, {"rebalanced_decisions", rebalanced},
      {"neutralize_attempted_decisions", attempted}, {"neutralize_applied_decisions", applied},
      {"neutralize_skipped_decisions", too_few + excluded + refused + amplified},
      {"neutralize_skip_reasons", std::move(skips)},
      {"neutralize_used_names_min", used_min},
      {"neutralize_used_names_median", finite_or_null(quantile(used, .5))},
      {"neutralize_amplification_median", finite_or_null(quantile(amplification, .5))},
      {"neutralize_amplification_max", amplification_max},
      {"neutralize_max_excluded_share", excluded_max},
      {"banded_names_total", banded},
      {"mean_banded_names_per_rebalance",
       rebalanced ? static_cast<f64>(banded) / static_cast<f64>(rebalanced) : 0.0}};
  if (neutralize_by_industry(c.neutralize))
    body["neutralize_industry"] = industry_summary(decisions);
  if (hold_band_declared(c)) body["hold_band"] = hold_band_summary(c, decisions);
  if (adv_hold_on(c)) body["adv_hold"] = adv_hold_summary(c, decisions);
  return Json{{"construction", std::move(body)}};
}
// The id's CLI spelling; price-risk-ind-v2 also sets its declared vol/log-ADV windows
// (the only price_risk fields any CLI sets). false (cfg untouched) otherwise.
bool parse_neutralize(std::string_view value, TargetReplayConfig& cfg) {
  TargetNeutralize id = TargetNeutralize::None;
  if (value == "none") id = TargetNeutralize::None;
  else if (value == "price-risk-v1") id = TargetNeutralize::PriceRiskV1;
  else if (value == "price-risk-ind-v1") id = TargetNeutralize::PriceRiskIndV1;
  else if (value == "price-risk-ind-v2") id = TargetNeutralize::PriceRiskIndV2;
  else return false;
  cfg.neutralize = id;
  if (id == TargetNeutralize::PriceRiskIndV2) {
    cfg.price_risk.vol_window = price_risk_ind_v2_vol_window;
    cfg.price_risk.adv_window = price_risk_ind_v2_adv_window;
  }
  return true;
}
constexpr const char* construction_columns =
    ",neutralize,neutralize_used,neutralize_excluded,neutralize_excluded_share,"
    "neutralize_amplification,banded_names";
void write_construction(std::ostream& out, const ConstructionDay& c) {
  out << ',' << outcome_label(c.neutralize) << ',' << c.neutralize_used << ','
      << c.neutralize_excluded << ',' << c.neutralize_excluded_share << ',';
  if (std::isnan(c.neutralize_amplification)) out << "nan"; else out << c.neutralize_amplification;
  out << ',' << c.banded_names;
}

Json recipe(const TargetReplayRunConfig& cfg) {
  auto j = Json{{"schema", "atx.dsl-target-replay/v1"}, {"rule", rule_id(cfg.target)},
      {"combined_sha256", cfg.combined_sha256}, {"role_sha256", cfg.role_sha256},
      {"cadence", cfg.target.cadence}, {"trade_fraction", cfg.target.trade_fraction},
      {"monthly_budget", cfg.target.monthly_budget}, {"calendar_basis", "decision-session-month"},
      {"deployment_included", true}, {"forced_exits", "immediate;charged-before-discretionary;may-breach"},
      {"target", "tied-rank;neutral-desired-gross1;partial-no-drift-no-renormalization"},
      {"rough_returns", !cfg.role_path.empty()}, {"entry_delay", 1}, {"return_horizon", 1},
      {"missing_return", "complete-day-undefined;observed-component-and-missing-exposures"},
      {"return_guard", "observed-adjacent-log1.5;adjusted-log-vs-raw+.10;no-missing-zero-fill"},
      {"one_way_bps", cfg.target.one_way_bps}, {"annual_borrow_bps", cfg.target.annual_borrow_bps},
      {"cost_basis", "target-change-no-drift;short-target/252;not-fills"},
      {"max_working_bytes", cfg.target.max_working_bytes}};
  if (construction_on(cfg.target)) j.update(construction_recipe(cfg.target));
  if (decaying_exit(cfg.target))
    j["forced_exits"] = "decay-at-exit-rate;snap-inside-dust-band;absent-immediate";
  return j;
}
co::Status write_json(const std::filesystem::path& path, const Json& j) {
  std::ofstream file(path, std::ios::binary);
  if (!file) return co::Err(co::ErrorCode::IoError, "target replay: JSON output");
  file << j.dump(2) << '\n'; file.close();
  return file ? co::Ok() : co::Status(co::Err(co::ErrorCode::IoError, "target replay: JSON close"));
}
// Construction columns are appended only when an option is non-default, so the
// default CSV is byte-identical to the replay without construction options.
co::Status write_days(const std::filesystem::path& path, const TargetReplayResult& result,
                      bool construction) {
  std::ofstream file(path, std::ios::binary);
  if (!file) return co::Err(co::ErrorCode::IoError, "target replay: daily output");
  file << "decision,session_ns,month,entry,endpoint,turnover,forced,discretionary,deployment,"
          "month_turnover,budget_excess,applied_fraction,gross,net,long_weight,short_weight,"
          "max_abs_weight,effective_names,held_names,return_mature,return_complete,"
          "observed_return_component,missing_long,missing_short,missing_gross,missing_names,"
          "guarded_names,modeled_trade_cost,modeled_borrow_cost,complete_gross_return,complete_net_return";
  if (construction) file << ",rebalance" << construction_columns;
  file << '\n';
  file << std::setprecision(17);
  for (const auto& d : result.days) {
    file << d.decision << ',' << d.session << ',' << d.calendar_month << ',' << d.entry << ','
         << d.endpoint << ',' << d.turnover << ',' << d.forced_turnover << ','
         << d.discretionary_turnover << ',' << d.deployment_turnover << ',' << d.month_turnover << ','
         << d.budget_excess << ',' << d.applied_fraction << ',' << d.gross << ',' << d.net << ','
         << d.long_weight << ',' << d.short_weight << ',' << d.max_abs_weight << ','
         << d.effective_names << ',' << d.held_names << ',' << d.return_mature << ','
         << d.return_complete << ',' << d.observed_return_component << ',' << d.missing_long << ','
         << d.missing_short << ',' << d.missing_gross << ',' << d.missing_names << ','
         << d.guarded_names << ',' << d.modeled_trade_cost << ',' << d.modeled_borrow_cost << ','
         << d.complete_gross_return << ',' << d.complete_net_return;
    if (construction) {
      file << ',' << d.construction.rebalance;
      write_construction(file, d.construction);
    }
    file << '\n';
  }
  file.close();
  return file ? co::Ok() : co::Status(co::Err(co::ErrorCode::IoError, "target replay: daily close"));
}
Json summarize(const TargetReplayResult& result) {
  struct Month { usize days{}; f64 turnover{}, forced{}, discretionary{}, deployment{}, excess{}; };
  std::map<u32, Month> months;
  f64 gross = 0, max_net = 0, max_name = 0, missing = 0; usize mature = 0, complete = 0;
  f64 observed = 0, complete_gross = 0, complete_net = 0, trade_cost = 0, borrow_cost = 0;
  for (const auto& d : result.days) {
    auto& m = months[d.calendar_month]; ++m.days; m.turnover += d.turnover;
    m.forced += d.forced_turnover; m.discretionary += d.discretionary_turnover;
    m.deployment += d.deployment_turnover; m.excess = std::max(m.excess, d.budget_excess);
    gross += d.gross; max_net = std::max(max_net, std::abs(d.net));
    max_name = std::max(max_name, d.max_abs_weight); missing += d.missing_gross;
    mature += d.return_mature ? 1U : 0U; complete += d.return_complete ? 1U : 0U;
    if (d.return_mature) observed += d.observed_return_component;
    if (d.return_complete) { complete_gross += d.complete_gross_return; complete_net += d.complete_net_return; }
    trade_cost += d.modeled_trade_cost; borrow_cost += d.modeled_borrow_cost;
  }
  Json rows = Json::array(); f64 max_month = 0, total = 0;
  for (const auto& [id, m] : months) {
    rows.push_back({{"month", id}, {"observed_decision_sessions", m.days}, {"turnover", m.turnover},
        {"forced", m.forced}, {"discretionary", m.discretionary}, {"deployment", m.deployment},
        {"budget_excess", m.excess}});
    max_month = std::max(max_month, m.turnover); total += m.turnover;
  }
  return Json{{"months", std::move(rows)}, {"total_turnover", result.total_turnover},
      {"monthly_reconciled_total", total}, {"mean_monthly_turnover", total / static_cast<f64>(months.size())},
      {"max_monthly_turnover", max_month}, {"forced_turnover", result.forced_turnover},
      {"discretionary_turnover", result.discretionary_turnover},
      {"deployment_turnover", result.deployment_turnover}, {"deployment_date_index", result.deployment_date},
      {"mean_gross", gross / static_cast<f64>(result.days.size())}, {"max_abs_net", max_net},
      {"max_abs_name_weight", max_name}, {"mature_return_days", mature}, {"complete_return_days", complete},
      {"incomplete_return_days", mature - complete}, {"summed_missing_gross_exposure", missing},
      {"observed_component_sum_not_portfolio_return", observed},
      {"complete_days_gross_sum_not_total_period_return", complete_gross},
      {"complete_days_net_sum_not_total_period_return", complete_net},
      {"modeled_trade_cost_all_decisions", trade_cost}, {"modeled_borrow_cost_all_decisions", borrow_cost},
      {"net_sharpe", nullptr}, {"self_financing_nav", false}, {"capacity_qualified", false}};
}
} // namespace

co::Status run_target_replay(const TargetReplayRunConfig& cfg, std::ostream& progress) {
  try {
    ATX_TRY_VOID(validate_config(cfg.target));
    if constexpr (std::endian::native != std::endian::little)
      return co::Err(co::ErrorCode::Unavailable, "target replay: little-endian host required");
    if (!std::numeric_limits<f64>::is_iec559 || cfg.output_directory.empty() ||
        cfg.role_path.empty() != cfg.role_sha256.empty() ||
        cfg.target.max_working_bytes < metadata_bytes)
      return co::Err(co::ErrorCode::InvalidArgument, "target replay: output/role/budget contract");
    if (neutralizing(cfg.target) && cfg.role_path.empty())
      return co::Err(co::ErrorCode::InvalidArgument,
                     "target replay: --neutralize price-risk-v1 requires --role");
    // The target replay loads no role fields; the NAV replay's --fields carries them.
    if (neutralize_by_industry(cfg.target.neutralize))
      return co::Err(co::ErrorCode::InvalidArgument,
                     "target replay: the industry ids need the grp_ff12 field (nav --fields)");
    // Neutralization reads the role's volume (log ADV); otherwise nothing changes.
    const bool with_volume = neutralizing(cfg.target);
    SavedBlend blend; ATX_TRY_VOID(admit_saved(cfg, blend, with_volume));
    ATX_TRY_VOID(load_prices(cfg, blend, with_volume));
    ATX_TRY(auto result, replay_targets(blend.view(), cfg.target));
    const auto dir = std::filesystem::path(cfg.output_directory);
    if (!std::filesystem::create_directory(dir))
      return co::Err(co::ErrorCode::AlreadyExists, "target replay: output must not exist");
    const auto method = recipe(cfg); ATX_TRY(auto method_sha, co::sha256_hex(method.dump()));
    ATX_TRY_VOID(write_json(dir / "recipe.json", method));
    const bool construction = construction_on(cfg.target);
    ATX_TRY_VOID(write_days(dir / "daily.csv", result, construction));
    ATX_TRY(auto daily_sha, co::sha256_file((dir / "daily.csv").string()));
    auto summary = summarize(result);
    if (construction) {
      std::vector<ConstructionDay> decisions; decisions.reserve(result.days.size());
      for (const auto& d : result.days) decisions.push_back(d.construction);
      summary.update(construction_summary(cfg.target, decisions));
      if (aim_partial(cfg.target)) {
        std::vector<detail::AimPartialDecision> aim; aim.reserve(result.days.size());
        const auto view = blend.view();
        for (const auto& d : result.days)
          aim.push_back({d.gross, d.net, d.held_names, members_at(view, d.decision)});
        summary["construction"]["v5"] = aim_partial_summary(cfg.target, aim);
      }
    }
    summary["schema"] = "atx.dsl-target-replay-summary/v1"; summary["status"] = "complete";
    summary["recipe_sha256"] = method_sha; summary["combined_sha256"] = cfg.combined_sha256;
    summary["source_bindings"] = blend.manifest; summary["daily_csv_sha256"] = daily_sha;
    summary["limitations"] = "planned weights without drift; decision-month turnover includes deployment; "
        "rough delayed observed returns only, incomplete days unavailable; no corporate-action accounting, "
        "fills, funding, realized fees, capacity, NAV or realistic Sharpe";
    ATX_TRY_VOID(write_json(dir / "summary.json", summary));
    progress << "target replay complete: " << result.days.size() << " decisions; total planned turnover "
             << std::setprecision(17) << result.total_turnover << '\n';
    return co::Ok();
  } catch (const std::exception& e) {
    return co::Err(co::ErrorCode::InvalidArgument, std::string("target replay: ") + e.what());
  }
}
int dispatch_target_replay(int argc, char** argv, std::ostream& out, std::ostream& err) {
  try {
    TargetReplayRunConfig cfg; std::set<std::string> seen;
    for (int i = 1; i < argc; ++i) {
      const std::string key = argv[i];
      if (key == "--help") {
        out << "--combined PATH --combined-sha256 SHA --output DIR "
               "[--rule baseline-v1|monthly-budget-v2|aim-partial-v5] [--cadence 5] "
               "[--trade-fraction .25 (aim-partial-v5: theta)] "
               "[--monthly-budget .30] [--role PATH --role-sha256 SHA] "
               "[--neutralize none|price-risk-v1 (needs --role)] [--band-multiple 0] "
               "[--dust-multiple 0 --aim-leverage 1 (aim-partial-v5 only)] "
               "[--exit-rate 1 (below 1: aim-partial-v5, dust > 0, --role)] "
               "[--hold-band B (aim-partial-v5; v8 hold-band-v1, B in [0, 1])] "
               "[--one-way-bps 0 --annual-borrow-bps 0] [--max-bytes 536870912]\n";
        return 0;
      }
      if (!seen.insert(key).second || i + 1 >= argc) throw std::invalid_argument("duplicate/missing flag");
      const std::string value = argv[++i];
      const auto real = [&]() {
        usize used = 0; const f64 x = std::stod(value, &used);
        if (used != value.size()) throw std::invalid_argument("invalid number");
        return x;
      };
      const auto integer = [&]() {
        u64 x = 0; const auto parsed = std::from_chars(value.data(), value.data() + value.size(), x);
        if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size())
          throw std::invalid_argument("invalid integer");
        return x;
      };
      if (key == "--combined") cfg.combined_path = value;
      else if (key == "--combined-sha256") cfg.combined_sha256 = value;
      else if (key == "--output") cfg.output_directory = value;
      else if (key == "--role") cfg.role_path = value;
      else if (key == "--role-sha256") cfg.role_sha256 = value;
      else if (key == "--cadence") {
        const auto x = integer(); if (x > max_dates) throw std::invalid_argument("cadence exceeds bound");
        cfg.target.cadence = static_cast<usize>(x);
      } else if (key == "--trade-fraction") cfg.target.trade_fraction = real();
      else if (key == "--monthly-budget") cfg.target.monthly_budget = real();
      else if (key == "--one-way-bps") cfg.target.one_way_bps = real();
      else if (key == "--annual-borrow-bps") cfg.target.annual_borrow_bps = real();
      else if (key == "--max-bytes") cfg.target.max_working_bytes = integer();
      else if (key == "--band-multiple") cfg.target.band_multiple = real();
      else if (key == "--dust-multiple") cfg.target.dust_multiple = real();
      else if (key == "--aim-leverage") cfg.target.aim_leverage = real();
      else if (key == "--exit-rate") cfg.target.exit_rate = real();
      else if (key == "--hold-band") cfg.target.hold_band = real();
      else if (key == "--neutralize") {
        if (!parse_neutralize(value, cfg.target))
          throw std::invalid_argument("unknown --neutralize (none|price-risk-v1)");
      } else if (key == "--rule") {
        if (value == "baseline-v1") cfg.target.rule = TargetReplayRule::BaselineTargetV1;
        else if (value == "monthly-budget-v2") cfg.target.rule = TargetReplayRule::MonthlyTargetBudgetV2;
        else if (value == "aim-partial-v5") cfg.target.rule = TargetReplayRule::AimPartialV5;
        else throw std::invalid_argument("unknown target rule");
      } else throw std::invalid_argument("unknown flag: " + key);
    }
    const auto status = run_target_replay(cfg, out);
    if (!status) { err << status.error().to_string() << '\n'; return 1; }
    return 0;
  } catch (const std::exception& e) { err << "target replay: " << e.what() << '\n'; return 2; }
}

// Private shared seams for the NAV replay (strategy_target_replay_detail.hpp).
// Thin forwarding wrappers over the file-local implementations above: no code
// motion and no arithmetic change, so replay_targets and its fixtures are
// unaffected. Qualified `::atx::impl::strategy::` calls reach the unnamed-namespace
// functions (a plain call would find these same-named wrappers first).
namespace detail {
TargetReplayInput LoadedSavedBlend::view() const {
  return {dates, names, begin, end, signal, member, sessions, ids, close, raw, present, volume};
}
co::Result<LoadedSavedBlend> load_saved_blend(const TargetReplayRunConfig& cfg, bool with_volume) {
  try {
    ATX_TRY_VOID(validate_config(cfg.target));
    if constexpr (std::endian::native != std::endian::little)
      return co::Err(co::ErrorCode::Unavailable, "target replay: little-endian host required");
    if (!std::numeric_limits<f64>::is_iec559 || cfg.role_path.empty() != cfg.role_sha256.empty() ||
        (with_volume && cfg.role_path.empty()))
      return co::Err(co::ErrorCode::InvalidArgument, "target replay: role/volume load contract");
    if (cfg.target.max_working_bytes < metadata_bytes)
      return co::Err(co::ErrorCode::OutOfRange, "target replay: aggregate input/workspace budget");
    SavedBlend blend;
    ATX_TRY_VOID(admit_saved(cfg, blend, with_volume));
    ATX_TRY_VOID(load_prices(cfg, blend, with_volume));
    LoadedSavedBlend out;
    out.dates = blend.dates; out.names = blend.names; out.begin = blend.begin; out.end = blend.end;
    out.signal = std::move(blend.signal); out.close = std::move(blend.close);
    out.raw = std::move(blend.raw); out.volume = std::move(blend.volume);
    out.member = std::move(blend.member); out.present = std::move(blend.present);
    out.sessions = std::move(blend.sessions); out.ids = std::move(blend.ids);
    out.manifest_json = blend.manifest.dump();
    return co::Ok(std::move(out));
  } catch (const std::bad_alloc&) {
    return co::Err(co::ErrorCode::OutOfRange, "target replay: allocation failed");
  } catch (const std::exception& e) {
    return co::Err(co::ErrorCode::InvalidArgument, std::string("target replay: ") + e.what());
  }
}
co::Status check_label_role(const TargetReplayRunConfig& cfg, const std::string& path,
                            const std::string& sha256) {
  try {
    if (cfg.role_path.empty() || cfg.role_sha256.empty() || path.empty() || sha256.empty())
      return label_refused("--role and --label-role, each with its SHA-256, are required");
    ATX_TRY(const auto role, pinned_json(cfg.role_path, cfg.role_sha256));
    auto label = pinned_json(path, sha256);
    if (!label) return label_refused("its manifest: " + label.error().to_string());
    return check_label_manifests(role, *label);
  } catch (const std::bad_alloc&) {
    return co::Err(co::ErrorCode::OutOfRange, "nav replay: --label-role allocation failed");
  } catch (const std::exception& e) {
    return label_refused(std::string("its manifest: ") + e.what());
  }
}
co::Result<LoadedLabelRole> load_label_role(const TargetReplayRunConfig& cfg,
                                            const std::string& path, const std::string& sha256,
                                            const LoadedSavedBlend& blend) {
  try {
    ATX_TRY_VOID(check_label_role(cfg, path, sha256));
    const usize n = blend.names, cells = blend.dates * blend.names;
    if (!cells || blend.sessions.size() != blend.dates || blend.ids.size() != n ||
        blend.close.size() != cells || blend.raw.size() != cells ||
        blend.present.size() != cells || blend.member.size() != cells)
      return label_refused("it needs --role's prices (the NAV load)");
    // No label payload of a role reaching the seal is opened (the sessions are --role's: the
    // manifests pin the same sessions.i64).
    if (std::any_of(blend.sessions.begin(), blend.sessions.end(),
                    [](i64 session) { return rw::is_sealed(session); }))
      return label_refused(sealed_label());
    ATX_TRY(const auto label, pinned_json(path, sha256));
    const auto base = std::filesystem::path(path).parent_path();
    const auto& files = label.at("files");
    LoadedLabelRole out;
    ATX_TRY_VOID(payload(base, files, "close.f64", out.close, cells));
    ATX_TRY_VOID(payload(base, files, "raw_close.f64", out.raw, cells));
    ATX_TRY_VOID(payload(base, files, "present.u8", out.present, cells));
    std::vector<u8> member;
    ATX_TRY_VOID(payload(base, files, "member.u8", member, cells));
    const auto where = [&blend, n](usize k) {
      return " (row " + std::to_string(k / n) + ", instrument " +
             std::to_string(blend.ids[k % n]) + ")";
    };
    for (usize k = 0; k < cells; ++k) {
      const bool shown = out.present[k] != 0;
      const f64 close = out.close[k], raw = out.raw[k];
      if (out.present[k] > 1 || member[k] > 1 ||
          (shown ? (!std::isfinite(close) || close <= 0 || !std::isfinite(raw) || raw <= 0)
                 : (!std::isnan(close) || !std::isnan(raw))))
        return label_refused("its presence/price contract" + where(k));
      if (blend.present[k] != 0 &&
          (!shown || std::bit_cast<u64>(close) != std::bit_cast<u64>(blend.close[k]) ||
           std::bit_cast<u64>(raw) != std::bit_cast<u64>(blend.raw[k])))
        return label_refused("it differs from --role where --role is present" + where(k) +
                             ": a label role only adds presence");
      if (blend.member[k] != static_cast<u8>(member[k] != 0 && shown && close > 0))
        return label_refused("its membership differs from --role's" + where(k));
      if (!shown || blend.present[k] != 0) continue;
      ++out.label_only_cells;
      const usize row = k / n;
      if (row >= blend.begin && row < blend.end) ++out.label_only_scored_cells;
    }
    return co::Ok(std::move(out));
  } catch (const std::bad_alloc&) {
    return co::Err(co::ErrorCode::OutOfRange, "nav replay: --label-role allocation failed");
  } catch (const std::exception& e) {
    return label_refused(std::string("its manifest: ") + e.what());
  }
}
co::Status validate_replay_input(const TargetReplayInput& in, const TargetReplayConfig& cfg) {
  return validate_input(in, cfg);
}
void desired_target(std::span<const f64> signal, std::span<const u8> member,
                    std::vector<std::pair<f64, usize>>& row, std::vector<f64>& target) {
  ::atx::impl::strategy::desired_target(signal, member, row, target);
}
co::Status update_weights(const TargetReplayInput& in, const TargetReplayConfig& cfg, usize d,
                          bool rebalance, f64 spent, const std::vector<f64>& desired,
                          std::vector<f64>& current, TargetReplayDay& out,
                          std::span<const f64> per_name_rate) {
  return ::atx::impl::strategy::update_weights(in, cfg, d, rebalance, spent, desired, current,
                                               out, per_name_rate);
}
usize members_at(const TargetReplayInput& in, usize d) {
  return ::atx::impl::strategy::members_at(in, d);
}
co::Result<bool> form_desired(const TargetReplayInput& in, const TargetReplayConfig& cfg, usize d,
                              std::vector<std::pair<f64, usize>>& row, std::vector<f64>& desired,
                              PriceRiskScratch& scratch, ConstructionDay& out,
                              std::span<const u8> no_short, DesiredState* state) {
  return ::atx::impl::strategy::form_desired(in, cfg, d, row, desired, scratch, out, no_short,
                                             state);
}
bool construction_active(const TargetReplayConfig& cfg) { return construction_on(cfg); }
std::string construction_rule_id(const TargetReplayConfig& cfg) { return rule_id(cfg); }
std::string construction_recipe_json(const TargetReplayConfig& cfg) {
  return construction_on(cfg) ? construction_recipe(cfg).dump() : std::string{};
}
std::string construction_summary_json(const TargetReplayConfig& cfg,
                                      std::span<const ConstructionDay> decisions) {
  return construction_on(cfg) ? construction_summary(cfg, decisions).dump() : std::string{};
}
std::string aim_partial_summary_json(const TargetReplayConfig& cfg,
                                     std::span<const AimPartialDecision> decisions) {
  return aim_partial(cfg) ? aim_partial_summary(cfg, decisions).dump() : std::string{};
}
u64 construction_scratch_bytes(const TargetReplayConfig& cfg, usize instruments) {
  return price_risk_scratch_bytes(cfg, instruments) + desired_state_bytes(cfg, instruments);
}
const char* construction_csv_columns() { return construction_columns; }
void write_construction_csv(std::ostream& out, const ConstructionDay& day) {
  write_construction(out, day);
}
bool parse_neutralize(std::string_view value, TargetReplayConfig& cfg) {
  return ::atx::impl::strategy::parse_neutralize(value, cfg);
}
const char* neutralize_outcome_label(NeutralizeOutcome outcome) { return outcome_label(outcome); }
f64 sorted_quantile(std::span<const f64> sorted, f64 q) { return quantile(sorted, q); }
u32 calendar_month(i64 session_ns) { return ::atx::impl::strategy::calendar_month(session_ns); }
const char* nonmember_exit_clause(const TargetReplayConfig& cfg) { return nonmember_exit(cfg); }
} // namespace detail
} // namespace atx::impl::strategy
