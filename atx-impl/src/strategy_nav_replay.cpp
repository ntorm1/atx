#include "strategy_nav_replay.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <map>
#include <memory>
#include <new>
#include <ostream>
#include <set>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>
#include <nlohmann/json.hpp>
#include "atx/core/sha256.hpp"
#include "atx/engine/book/replay_cost.hpp"
#include "atx/engine/eval/hac.hpp"
#include "strategy_target_replay_detail.hpp"

namespace atx::impl::strategy {
namespace {
using namespace atx;
namespace co = atx::core;
namespace bk = atx::engine::book;
namespace hac = atx::engine::eval::hac;
using Json = nlohmann::json;
constexpr f64 nan = std::numeric_limits<f64>::quiet_NaN();
constexpr f64 inf = std::numeric_limits<f64>::infinity();
constexpr i64 day_ns = 86'400'000'000'000LL;
constexpr usize max_dates = 4096, max_names = 20000, scenario_count = 3;
constexpr u64 max_event_cap = 1ULL << 24;
// 9 f64 + u32 + 2 u8 + one ranked pair per name (94 B), rounded up.
constexpr u64 per_name_bytes = 192;
constexpr u64 fixed_workspace_bytes = 1ULL << 20;    // histogram, cost model, locals
constexpr u64 publication_slack_bytes = 16ULL << 20; // JSON documents, stream buffers
// Both identities are exact in real arithmetic; this bounds accumulated rounding.
constexpr f64 identity_tolerance = 1e-9;
constexpr const char* turnover_definition =
    "calendar-month one-way turnover = sum over EXECUTION sessions t in the month (the "
    "fill session's month, not the decision's) of sum_i |filled dollars_i,t| / pre-trade "
    "NAV_t; includes forced exits and the initial deployment; write-offs are not fills and "
    "are excluded; *_ex_deployment_month drops the calendar month holding the first "
    "nonzero fill; planned turnover (decision month, drifted marked weights) is reported "
    "only for reconciliation";

struct Budget {
  u64 limit{}, used{};
  bool add(u64 count, u64 width) {
    if (width && count > (limit - used) / width) return false;
    used += count * width; return true;
  }
};
bool within(f64 x, f64 lo, f64 hi) { return std::isfinite(x) && x >= lo && x <= hi; }

// Log10 participation histogram: 0.01-decade bins over [1e-12, 1e4). The p95 is
// the upper edge of the bin holding the 95th percentile, capped at the exact max.
class ParticipationHistogram {
public:
  static constexpr usize bins = 1600;
  void add(f64 p) {
    if (!(p > 0) || !std::isfinite(p)) return;
    const f64 position = (std::log10(p) + 12.0) * 100.0;
    const usize bin = position <= 0 ? usize{0}
        : position >= static_cast<f64>(bins - 1) ? bins - 1 : static_cast<usize>(position);
    ++counts_[bin]; ++total_; maximum_ = std::max(maximum_, p);
  }
  [[nodiscard]] u64 total() const { return total_; }
  [[nodiscard]] f64 maximum() const { return total_ ? maximum_ : nan; }
  [[nodiscard]] f64 upper_quantile(f64 q) const {
    if (!total_) return nan;
    const auto rank = std::max<u64>(1, static_cast<u64>(std::ceil(q * static_cast<f64>(total_))));
    u64 seen = 0;
    for (usize b = 0; b < bins; ++b) {
      seen += counts_[b];
      if (seen >= rank)
        return std::min(maximum_, std::pow(10.0, static_cast<f64>(b + 1) / 100.0 - 12.0));
    }
    return maximum_;
  }
private:
  std::array<u64, bins> counts_{};
  u64 total_{};
  f64 maximum_{};
};

struct NameState {
  explicit NameState(usize n)
      : held(n), mark(n, nan), raw_mark(n, nan), order(n), current(n), planned(n), desired(n),
        written_exposure(n), written_haircut(n), absent(n), active(n), written_off(n) {
    row.reserve(n);
  }
  // held: marked dollars; mark/raw_mark: last OBSERVED adjusted/raw close;
  // order: working target dollars (decision-NAV dollars) when active.
  std::vector<f64> held, mark, raw_mark, order, current, planned, desired;
  std::vector<f64> written_exposure, written_haircut;
  std::vector<u32> absent; // consecutive absent marks while held or written off
  std::vector<u8> active, written_off;
  std::vector<std::pair<f64, usize>> row;
};
struct Ctx {
  const TargetReplayInput& x;
  std::span<const f64> volume;
  const NavReplayConfig& cfg;
  const bk::ReplayCostModel& model;
  f64 eta_long{}, eta_short{}; // terminal haircut return by side (0 unless adverse)
  f64 borrow_rate{};           // annual fraction
  f64 linear_rate{};           // sqrt model: (half spread + commission) per dollar
  bool sqrt_model{};
};
struct Book {
  explicit Book(usize n) : names(n) {}
  NameState names;
  f64 cash{}, nav_pre{}, nav_post{}, pending_cost{}, spent{};
  u32 month{};
  ParticipationHistogram participation;
  NavReplayResult result;
};

co::Status validate_nav_config(const NavReplayConfig& cfg) {
  const auto& s = cfg.scenario;
  const bool id_ok = !s.id.empty() && s.id.size() <= 64 &&
      std::all_of(s.id.begin(), s.id.end(), [](char ch) {
        return (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-';
      });
  const bool cost_ok = (s.cost == NavCostRule::FlatBpsV1 || s.cost == NavCostRule::SqrtImpactV1) &&
      within(s.flat_bps, 0, 10000) && within(s.half_spread_bps, 0, 10000) &&
      within(s.commission_bps, 0, 10000) && within(s.impact_y, 0, 100) &&
      within(s.impact_delta, 0, 2) && s.impact_delta > 0 && !std::isnan(s.max_participation) &&
      s.max_participation > 0 && within(s.annual_borrow_bps, 0, 100000) &&
      within(s.fallback_daily_vol, 0, 10) && s.fallback_daily_vol > 0;
  if (!id_ok || !cost_ok || !s.stale_exit_sessions || s.stale_exit_sessions > max_dates)
    return co::Err(co::ErrorCode::InvalidArgument, "nav replay: invalid scenario");
  if (!std::isfinite(cfg.initial_nav) || cfg.initial_nav <= 0 || cfg.liquidity_window < 2 ||
      cfg.liquidity_window > max_dates || cfg.min_vol_pairs < 2 ||
      cfg.min_vol_pairs > cfg.liquidity_window || !cfg.max_events ||
      cfg.max_events > max_event_cap || cfg.target.one_way_bps != 0 ||
      cfg.target.annual_borrow_bps != 0)
    return co::Err(co::ErrorCode::InvalidArgument,
                   "nav replay: invalid recipe (target-replay cost/borrow must be zero)");
  return co::Ok();
}
co::Status validate_nav_input(const NavReplayInput& in, const NavReplayConfig& cfg) {
  ATX_TRY_VOID(validate_nav_config(cfg));
  const auto& x = in.target;
  ATX_TRY_VOID(detail::validate_replay_input(x, cfg.target));
  const usize cells = x.dates * x.instruments;
  if (x.close.size() != cells || in.volume.size() != cells ||
      x.decision_end - x.decision_begin < 3)
    return co::Err(co::ErrorCode::InvalidArgument,
                   "nav replay: prices+volume required and at least three window sessions");
  for (usize k = 0; k < cells; ++k) {
    const bool bad = x.present[k]
        ? (!std::isfinite(x.close[k]) || x.close[k] <= 0 || !std::isfinite(x.raw_close[k]) ||
           x.raw_close[k] <= 0 || !std::isfinite(in.volume[k]) || in.volume[k] < 0)
        : x.member[k] != 0;
    if (bad)
      return co::Err(co::ErrorCode::InvalidArgument, "nav replay: price/volume/presence contract");
  }
  // Envelope EXCLUDES caller-owned input; events are charged at their cap.
  Budget budget{cfg.target.max_working_bytes};
  if (!budget.add(1, fixed_workspace_bytes) || !budget.add(x.instruments, per_name_bytes) ||
      !budget.add(x.decision_end - x.decision_begin, sizeof(NavReplayDay)) ||
      !budget.add(cfg.max_events, sizeof(NavEvent)))
    return co::Err(co::ErrorCode::OutOfRange, "nav replay: workspace budget");
  return co::Ok();
}
co::Result<std::unique_ptr<const bk::ReplayCostModel>> make_cost_model(const NavScenario& s) {
  std::unique_ptr<const bk::ReplayCostModel> model;
  switch (s.cost) {
  case NavCostRule::FlatBpsV1: {
    ATX_TRY(auto flat, bk::FlatBpsCost::create(s.flat_bps));
    model = std::make_unique<const bk::FlatBpsCost>(std::move(flat));
    break;
  }
  case NavCostRule::SqrtImpactV1: {
    ATX_TRY(auto impact, bk::SqrtImpactCost::create(
        bk::ReplayImpactCfg{s.impact_y, s.impact_delta}, s.max_participation, s.commission_bps));
    model = std::make_unique<const bk::SqrtImpactCost>(std::move(impact));
    break;
  }
  }
  if (!model) return co::Err(co::ErrorCode::InvalidArgument, "nav replay: cost rule");
  return co::Ok(std::move(model));
}

// Same interval guard as the target replay / strategy runner.
bool guarded_move(f64 close_a, f64 close_b, f64 raw_a, f64 raw_b) {
  const f64 adjusted = std::log(close_b) - std::log(close_a);
  const f64 raw = std::log(raw_b) - std::log(raw_a);
  return !std::isfinite(adjusted) || std::abs(adjusted) > 1.5 ||
         std::abs(adjusted) > std::abs(raw) + .10;
}
struct Liquidity {
  bk::LiquidityRow row{};
  bool fallback{};
};
// Execution-time liquidity of name i for session t from [t-w, t) only: raw-dollar
// ADV = sum of present raw_close*volume / w (absent or pre-history rows count as
// zero; never rescaled) and the sample SD of adjusted simple returns over adjacent
// present, unguarded pairs (k-1, k), k in [t-w, t), as strategy_runner.cpp does.
// Fewer than min_vol_pairs pairs -> the scenario's declared fallback sigma.
Liquidity liquidity_row(const Ctx& c, usize t, usize i) {
  const auto& x = c.x; const usize n = x.instruments, w = c.cfg.liquidity_window;
  const usize first = t > w ? t - w : 0;
  f64 dollars = 0, mean = 0, m2 = 0; usize pairs = 0;
  for (usize k = first; k < t; ++k) {
    const usize b = k * n + i;
    if (!x.present[b]) continue;
    dollars += x.raw_close[b] * c.volume[b];
    if (k == 0 || !x.present[b - n] ||
        guarded_move(x.close[b - n], x.close[b], x.raw_close[b - n], x.raw_close[b]))
      continue;
    const f64 r = x.close[b] / x.close[b - n] - 1;
    ++pairs; const f64 delta = r - mean;
    mean += delta / static_cast<f64>(pairs); m2 += delta * (r - mean);
  }
  Liquidity out;
  out.row.adv_dollars = dollars / static_cast<f64>(w);
  out.row.half_spread_bps = c.cfg.scenario.half_spread_bps;
  out.fallback = pairs < c.cfg.min_vol_pairs || !std::isfinite(m2) || m2 < 0;
  out.row.daily_vol = out.fallback ? c.cfg.scenario.fallback_daily_vol
                                   : std::sqrt(m2 / static_cast<f64>(pairs - 1));
  return out;
}

void add(NavBucket& bucket, f64 exposure, f64 pnl) {
  ++bucket.count; bucket.gross_exposure += std::abs(exposure); bucket.pnl += pnl;
}
struct EventValues {
  usize run{};
  f64 exposure{}, r_adj{nan}, r_raw{nan}, haircut{}, pnl{nan};
};
co::Status push_event(const Ctx& c, Book& b, NavEventKind kind, usize t, usize i,
                      const EventValues& v) {
  if (b.result.events.size() >= c.cfg.max_events)
    return co::Err(co::ErrorCode::OutOfRange,
                   "nav replay: event cap exceeded (" + std::to_string(c.cfg.max_events) + ")");
  NavEvent e;
  e.kind = kind; e.short_side = v.exposure < 0; e.run_length = v.run;
  e.session = c.x.session_keys[t]; e.instrument_id = c.x.instrument_ids[i];
  e.exposure = v.exposure; e.r_adj = v.r_adj; e.r_raw = v.r_raw; e.haircut = v.haircut;
  e.pnl = v.pnl;
  b.result.events.push_back(e);
  return co::Ok();
}

// A flat name only remembers its observable marks. A reprint after a write-off is
// reported with its forgone return versus the haircut and never touches NAV.
co::Status mark_flat(const Ctx& c, Book& b, usize t, usize i) {
  const auto& x = c.x; auto& s = b.names; const usize k = t * x.instruments + i;
  if (!x.present[k]) {
    if (s.written_off[i]) ++s.absent[i];
    return co::Ok();
  }
  if (s.written_off[i]) {
    const f64 exposure = s.written_exposure[i], eta = s.written_haircut[i];
    const f64 r_adj = x.close[k] / s.mark[i] - 1, r_raw = x.raw_close[k] / s.raw_mark[i] - 1;
    const f64 forgone = exposure * (r_adj - eta);
    add(b.result.reappeared, exposure, forgone);
    ATX_TRY_VOID(push_event(c, b, NavEventKind::ReappearedAfterWriteOff, t, i,
                            {s.absent[i], exposure, r_adj, r_raw, eta, forgone}));
    s.written_off[i] = 0; s.absent[i] = 0;
  }
  s.mark[i] = x.close[k]; s.raw_mark[i] = x.raw_close[k];
  return co::Ok();
}
// Held and absent at mark t: carry at the stale mark (orders stay blocked) until
// K consecutive absences, then write off at last mark x (1 + eta), causally.
co::Status carry_absent(const Ctx& c, Book& b, usize t, usize i, f64& writeoff) {
  auto& s = b.names; const f64 h = s.held[i];
  ++s.absent[i];
  if (s.absent[i] < c.cfg.scenario.stale_exit_sessions) return co::Ok();
  const f64 eta = h > 0 ? c.eta_long : c.eta_short;
  const f64 loss = h * eta;
  writeoff += loss; b.cash += h * (1.0 + eta);
  add(b.result.written_off, h, loss);
  ATX_TRY_VOID(push_event(c, b, NavEventKind::WriteOff, t, i,
                          {s.absent[i], h, nan, nan, eta, loss}));
  s.held[i] = 0; s.order[i] = 0; s.active[i] = 0;
  s.written_off[i] = 1; s.written_exposure[i] = h; s.written_haircut[i] = eta;
  return co::Ok();
}
// Held and present: realize the true (cumulative, if a gap preceded) adjusted return.
co::Status realize(const Ctx& c, Book& b, usize t, usize i, f64& pnl, NavReplayDay& day) {
  const auto& x = c.x; auto& s = b.names; const usize k = t * x.instruments + i;
  const f64 h = s.held[i], close = x.close[k], raw = x.raw_close[k];
  const f64 next = h * (close / s.mark[i]);
  if (!std::isfinite(next))
    return co::Err(co::ErrorCode::OutOfRange, "nav replay: holding mark overflow");
  const f64 r_adj = close / s.mark[i] - 1, r_raw = raw / s.raw_mark[i] - 1;
  const usize run = s.absent[i];
  pnl += next - h;
  if (run) {
    add(run == 1 ? b.result.gap_run_1 : run < 5 ? b.result.gap_run_2_4 : b.result.gap_run_5_plus,
        h, next - h);
    ATX_TRY_VOID(push_event(c, b, NavEventKind::GapResolved, t, i,
                            {run, h, r_adj, r_raw, 0, next - h}));
    s.absent[i] = 0;
  }
  if (guarded_move(s.mark[i], close, s.raw_mark[i], raw)) {
    const f64 difference = h * (r_raw - r_adj);
    ++day.guarded_intervals; add(b.result.guarded, h, difference);
    b.result.guard_sensitivity += difference;
    ATX_TRY_VOID(push_event(c, b, NavEventKind::Guarded, t, i,
                            {run, h, r_adj, r_raw, 0, difference}));
  }
  s.held[i] = next; s.mark[i] = close; s.raw_mark[i] = raw;
  return co::Ok();
}
// MARK at t > begin: borrow on pre-mark short dollars x calendar days / 365, then
// drift/stale/write-off. NAVpre_t = NAVpost_{t-1} + P + W - B, so the fill costs of
// t-1 land in r_t; cash + sum(held) must reconcile to it (checked in close_day).
co::Status mark_session(const Ctx& c, Book& b, usize t, NavReplayDay& day) {
  const auto& x = c.x; auto& s = b.names; const usize n = x.instruments;
  const f64 calendar_days = static_cast<f64>((x.session_keys[t] - x.session_keys[t - 1]) / day_ns);
  const f64 borrow_factor = c.borrow_rate * calendar_days / 365.0;
  f64 borrow = 0, pnl = 0, writeoff = 0;
  for (usize i = 0; i < n; ++i) if (s.held[i] < 0) borrow += -s.held[i] * borrow_factor;
  for (usize i = 0; i < n; ++i) {
    if (s.held[i] == 0) { ATX_TRY_VOID(mark_flat(c, b, t, i)); continue; }
    if (!x.present[t * n + i]) { ATX_TRY_VOID(carry_absent(c, b, t, i, writeoff)); continue; }
    ATX_TRY_VOID(realize(c, b, t, i, pnl, day));
  }
  b.cash -= borrow;
  const f64 base = b.nav_pre;
  b.nav_pre = b.nav_post + pnl + writeoff - borrow;
  if (!std::isfinite(b.nav_pre) || b.nav_pre <= 0)
    return co::Err(co::ErrorCode::OutOfRange, "nav replay: nonpositive/nonfinite pre-trade NAV");
  day.mark_pnl_dollars = pnl; day.writeoff_dollars = writeoff; day.borrow_dollars = borrow;
  day.net_return = b.nav_pre / base - 1;
  day.gross_return = (pnl + writeoff) / base; day.writeoff_return = writeoff / base;
  day.trade_cost_return = b.pending_cost / base; day.borrow_return = borrow / base;
  const f64 identity = std::abs(day.net_return -
      (day.gross_return - day.trade_cost_return - day.borrow_return));
  b.result.max_return_identity_error = std::max(b.result.max_return_identity_error, identity);
  if (!(identity <= identity_tolerance))
    return co::Err(co::ErrorCode::Internal, "nav replay: r != gross - cost - borrow");
  return co::Ok();
}
// EXECUTE working orders at close t. Absent names are blocked (order persists); an
// unusable ADV fills nothing; a capped fill leaves the residual working.
co::Status execute_orders(const Ctx& c, Book& b, usize t, NavReplayDay& day) {
  const auto& x = c.x; auto& s = b.names; const usize n = x.instruments;
  f64 cost = 0, traded = 0;
  for (usize i = 0; i < n; ++i) {
    if (!s.active[i]) continue;
    if (!x.present[t * n + i]) { ++day.blocked_absent; continue; }
    const f64 requested = s.order[i] - s.held[i];
    if (requested == 0) { s.active[i] = 0; continue; }
    const auto liquidity = liquidity_row(c, t, i);
    const auto priced = c.model.cost(i, t, requested, liquidity.row);
    const f64 full = c.model.unrationed_cost(i, t, requested, liquidity.row);
    if (std::isfinite(full)) day.unrationed_cost_dollars += full; else ++day.unrationed_unpriced;
    const f64 filled = priced.filled_dollars, size = std::abs(filled);
    if (filled == 0) {
      ++day.blocked_liquidity; day.unfilled_dollars += std::abs(requested);
      continue;
    }
    const bool complete = filled == requested;
    // A complete fill lands exactly on the decision-dollar target.
    s.held[i] = complete ? s.order[i] : s.held[i] + filled;
    s.active[i] = complete ? u8{0} : u8{1};
    if (!complete) { ++day.capped_fills; day.unfilled_dollars += std::abs(requested) - size; }
    b.cash -= filled; cost += priced.cost_dollars; traded += size; ++day.fills;
    const f64 linear = c.sqrt_model ? size * c.linear_rate : priced.cost_dollars;
    day.linear_cost_dollars += linear; day.impact_cost_dollars += priced.cost_dollars - linear;
    if (c.sqrt_model && liquidity.fallback) ++day.fallback_vol_fills;
    if (liquidity.row.adv_dollars > 0) b.participation.add(size / liquidity.row.adv_dollars);
  }
  b.cash -= cost;
  day.executed = true; day.traded_dollars = traded; day.trade_cost_dollars = cost;
  day.one_way_turnover = traded / b.nav_pre;
  b.nav_post = b.nav_pre - cost; b.pending_cost = cost;
  if (!std::isfinite(b.nav_post) || b.nav_post <= 0 || !std::isfinite(b.cash))
    return co::Err(co::ErrorCode::OutOfRange, "nav replay: nonpositive NAV after costs");
  return co::Ok();
}
// STABLE EXTENSION POINT: the only place the NAV path forms its desired target.
// A later neutralization step post-processes s.desired HERE, after the tied-rank
// target and before partial/budget planning; it may read only session d (causal).
void form_desired_target(const TargetReplayInput& x, usize d, NameState& s) {
  const usize offset = d * x.instruments;
  detail::desired_target(x.signal.subspan(offset, x.instruments),
                         x.member.subspan(offset, x.instruments), s.row, s.desired);
}
// DECIDE at d: current weights are marked dollars / post-trade NAV (stale names at
// stale marks); the unchanged target rules plan the next weights; every changed
// name gets a working order in decision-NAV dollars (zero exits included). On
// rebalance days unchanged members, and on every day flat nonmembers, cancel.
void plan_decision(const Ctx& c, Book& b, usize d, NavReplayDay& day) {
  const auto& x = c.x; auto& s = b.names; const usize n = x.instruments;
  const bool rebalance = (d - x.decision_begin) % c.cfg.target.cadence == 0;
  if (day.calendar_month != b.month) { b.month = day.calendar_month; b.spent = 0; }
  for (usize i = 0; i < n; ++i) s.current[i] = s.held[i] / b.nav_post;
  if (rebalance) form_desired_target(x, d, s);
  std::copy(s.current.begin(), s.current.end(), s.planned.begin());
  TargetReplayDay plan;
  plan.decision = d; plan.session = x.session_keys[d]; plan.calendar_month = day.calendar_month;
  detail::update_weights(x, c.cfg.target, d, rebalance, b.spent, s.desired, s.planned, plan);
  b.spent += plan.turnover;
  for (usize i = 0; i < n; ++i) {
    if (s.planned[i] != s.current[i]) {
      s.order[i] = s.planned[i] * b.nav_post; s.active[i] = 1;
    } else if (rebalance || !x.member[d * n + i]) {
      s.active[i] = 0;
    }
  }
  day.decision = true; day.rebalance = rebalance;
  day.planned_turnover = plan.turnover; day.planned_forced = plan.forced_turnover;
  day.planned_discretionary = plan.discretionary_turnover;
  day.planned_gross = plan.gross; day.planned_net = plan.net;
  day.applied_fraction = plan.applied_fraction; day.month_planned = b.spent;
  if (c.cfg.target.rule == TargetReplayRule::MonthlyTargetBudgetV2)
    day.budget_excess = std::max(0.0, b.spent - c.cfg.target.monthly_budget);
}
co::Status close_day(const Ctx& c, Book& b, NavReplayDay& day) {
  const auto& s = b.names; f64 longs = 0, shorts = 0;
  for (usize i = 0; i < c.x.instruments; ++i) {
    const f64 h = s.held[i];
    if (h == 0) continue;
    ++day.held_names; (h > 0 ? longs : shorts) += std::abs(h);
    if (s.absent[i]) {
      ++day.stale_names;
      (h > 0 ? day.stale_long_dollars : day.stale_short_dollars) += std::abs(h);
    }
  }
  day.posttrade_nav = b.nav_post; day.long_dollars = longs; day.short_dollars = shorts;
  day.gross_leverage = (longs + shorts) / b.nav_post;
  day.net_leverage = (longs - shorts) / b.nav_post;
  day.cash_ratio = b.cash / b.nav_post;
  const f64 book_error = std::abs(b.cash + (longs - shorts) - b.nav_post) / b.nav_post;
  b.result.max_cash_book_error = std::max(b.result.max_cash_book_error, book_error);
  if (!(book_error <= identity_tolerance))
    return co::Err(co::ErrorCode::Internal, "nav replay: cash + holdings != NAV");
  return co::Ok();
}
// Final session: still-stale holdings stay at their stale marks and are reported.
co::Status report_unresolved(const Ctx& c, Book& b, usize t) {
  const auto& s = b.names;
  for (usize i = 0; i < c.x.instruments; ++i) {
    if (s.held[i] == 0 || !s.absent[i]) continue;
    add(b.result.unresolved, s.held[i], 0);
    ATX_TRY_VOID(push_event(c, b, NavEventKind::UnresolvedAtEnd, t, i,
                            {s.absent[i], s.held[i], nan, nan, 0, nan}));
  }
  return co::Ok();
}
// Sessions [begin, end): MARK (t > begin) -> EXECUTE (begin < t <= end-2) ->
// DECIDE (t < end-2). Row t reads data at rows <= t only.
co::Result<NavReplayResult> run_book(const Ctx& c) {
  const auto& x = c.x; const usize begin = x.decision_begin, end = x.decision_end;
  auto book = std::make_unique<Book>(x.instruments);
  auto& b = *book;
  b.cash = b.nav_pre = b.nav_post = c.cfg.initial_nav;
  auto& r = b.result; r.deployment_index = end; r.days.reserve(end - begin);
  for (usize t = begin; t < end; ++t) {
    NavReplayDay day;
    day.session_index = t; day.session = x.session_keys[t];
    day.calendar_month = detail::calendar_month(day.session);
    day.return_observation = t >= begin + 2;
    if (t > begin) ATX_TRY_VOID(mark_session(c, b, t, day));
    day.pretrade_nav = b.nav_pre;
    if (t > begin && t + 2 <= end) {
      ATX_TRY_VOID(execute_orders(c, b, t, day));
      if (r.deployment_index == end && day.traded_dollars > 0) r.deployment_index = t;
    } else {
      b.nav_post = b.nav_pre; b.pending_cost = 0;
    }
    if (t + 2 < end) plan_decision(c, b, t, day);
    ATX_TRY_VOID(close_day(c, b, day));
    if (t + 1 == end) ATX_TRY_VOID(report_unresolved(c, b, t));
    r.days.push_back(day);
  }
  r.participation_fills = b.participation.total();
  r.participation_p95 = b.participation.upper_quantile(.95);
  r.participation_max = b.participation.maximum();
  return co::Ok(std::move(r));
}

struct Moments {
  usize n{}; f64 mean{}, m2{};
  void add(f64 x) {
    ++n; const f64 d = x - mean; mean += d / static_cast<f64>(n); m2 += d * (x - mean);
  }
  [[nodiscard]] f64 sd() const { return n > 1 ? std::sqrt(m2 / static_cast<f64>(n - 1)) : nan; }
  [[nodiscard]] f64 sharpe() const {
    return n > 1 && m2 > 0 ? mean / sd() * std::sqrt(252.0) : nan;
  }
};
void accumulate_day(const NavReplayDay& d, NavSummary& s) {
  s.trade_cost_dollars += d.trade_cost_dollars; s.linear_cost_dollars += d.linear_cost_dollars;
  s.impact_cost_dollars += d.impact_cost_dollars;
  s.unrationed_cost_dollars += d.unrationed_cost_dollars;
  s.borrow_dollars += d.borrow_dollars; s.writeoff_dollars += d.writeoff_dollars;
  s.fills += d.fills; s.capped_fills += d.capped_fills; s.blocked_absent += d.blocked_absent;
  s.blocked_liquidity += d.blocked_liquidity; s.fallback_vol_fills += d.fallback_vol_fills;
  s.unrationed_unpriced += d.unrationed_unpriced; s.unfilled_dollars += d.unfilled_dollars;
  s.max_gross_leverage = std::max(s.max_gross_leverage, d.gross_leverage);
  s.max_abs_net_leverage = std::max(s.max_abs_net_leverage, std::abs(d.net_leverage));
  s.max_stale_names = std::max(s.max_stale_names, d.stale_names);
  s.max_stale_gross_fraction = std::max(s.max_stale_gross_fraction,
      (d.stale_long_dollars + d.stale_short_dollars) / d.posttrade_nav);
  s.min_cash_ratio = std::min(s.min_cash_ratio, d.cash_ratio);
}
bool within_target(f64 turnover) { return turnover <= nav_monthly_turnover_target; }
void summarize_months(const std::map<u32, NavMonth>& months, u32 deployment_month,
                      NavSummary& s) {
  f64 all = 0, ex = 0; usize count = 0, ex_count = 0;
  for (auto [id, m] : months) {
    m.is_deployment_month = s.deployed && id == deployment_month;
    s.months.push_back(m);
    if (!m.execution_sessions) continue;
    ++count; all += m.one_way_turnover;
    s.max_monthly_turnover = std::max(s.max_monthly_turnover, m.one_way_turnover);
    s.months_within_target += within_target(m.one_way_turnover) ? 1U : 0U;
    if (m.is_deployment_month) continue;
    ++ex_count; ex += m.one_way_turnover;
    s.max_monthly_turnover_ex_deployment =
        std::max(s.max_monthly_turnover_ex_deployment, m.one_way_turnover);
    s.months_within_target_ex_deployment += within_target(m.one_way_turnover) ? 1U : 0U;
  }
  s.execution_months = count;
  s.mean_monthly_turnover = count ? all / static_cast<f64>(count) : nan;
  s.mean_monthly_turnover_ex_deployment = ex_count ? ex / static_cast<f64>(ex_count) : nan;
  if (!ex_count) s.max_monthly_turnover_ex_deployment = nan;
  // NaN compares false: an empty set never meets a target.
  s.meets_turnover_target_mean = within_target(s.mean_monthly_turnover);
  s.meets_turnover_target_mean_ex_deployment =
      within_target(s.mean_monthly_turnover_ex_deployment);
  s.meets_turnover_target_all_months = count && within_target(s.max_monthly_turnover);
  s.meets_turnover_target_all_months_ex_deployment =
      ex_count && within_target(s.max_monthly_turnover_ex_deployment);
}

Json finite_or_null(f64 x) { return std::isfinite(x) ? Json(x) : Json(nullptr); }
Json bucket_json(const NavBucket& b) {
  return Json{{"count", b.count}, {"gross_exposure_dollars", b.gross_exposure},
              {"pnl_dollars", b.pnl}};
}
const char* rule_label(TargetReplayRule rule) {
  return rule == TargetReplayRule::BaselineTargetV1 ? "baseline-target-v1"
                                                    : "monthly-target-budget-v2";
}
constexpr const char* timing_declaration =
    "decide at session d after its mark (modeled +22h mark, +23h decision); fill at the close "
    "of d+1; first return row d+2; decisions [begin,end-2), executions <= end-2, return rows "
    "[begin+2,end) (last two scored decisions dropped vs target replay); cadence phase relative "
    "to begin; final session valuation only, no liquidation";
constexpr const char* accounting_declaration =
    "self-financing marked-dollar book; per session MARK->EXECUTE->DECIDE; cash 0%, no short "
    "rebate, short proceeds stay in cash; fill costs debited to cash at the fill and land in "
    "the next return row; borrow on pre-mark short dollars x calendar days/365; "
    "r_t = NAVpre_t/NAVpre_{t-1}-1 = gross(drift+write-off) - trade_cost(t-1) - borrow, "
    "checked; cash+holdings reconciled to NAV every session";
constexpr const char* target_declaration =
    "tied-rank;neutral-desired-gross1;unchanged target-replay rules on drifted marked weights "
    "(held/post-trade NAV);working orders fixed in decision-NAV dollars until filled, replaced "
    "at a rebalance, or cancelled";
constexpr const char* missing_declaration =
    "causal stale-carry K-session write-off: absence only from present[t]; held absent names "
    "carried at the last observed adjusted close with orders blocked; a reprint within K "
    "realizes the cumulative adjusted return, then pending orders execute at that close; K "
    "consecutive absences write off at last mark x (1+haircut); a reprint after write-off is a "
    "diagnostic event only; still stale at the final session: valued at the stale mark and "
    "reported unresolved; entries on absent names unfilled";
constexpr const char* guard_declaration =
    "adjusted |log| > 1.5 or > |raw log| + 0.10 over the realized (possibly gapped) interval "
    "is flagged and still realized at the adjusted return; sensitivity sum h*(r_raw - r_adj) "
    "reported";
constexpr const char* liquidity_declaration =
    "execution t: ADV = sum over [t-w,t) of present raw_close*volume / w (absent = 0, never "
    "rescaled); sigma = sample SD of adjusted simple returns over adjacent present unguarded "
    "pairs (k-1,k), k in [t-w,t), if >= min_vol_pairs, else fallback; ADV = 0 -> no fill "
    "(liquidity-blocked)";
constexpr const char* limitations_declaration =
    "declared unfitted costs (constant 5 bps half spread, impact Y=0.6, flat 300 bps borrow, "
    "no locate); vendor adjusted-close factor unverified (guard sensitivity reported); "
    "common-stock status unverified; K-session write-off may misclassify halts vs delistings "
    "(missing histogram reported); cash earns 0% and shorts no rebate, so returns approximate "
    "excess returns; v2 budget planned by decision month while actual turnover is by execution "
    "month; working orders keep decision-NAV dollar targets; no terminal liquidation; the last "
    "two scored decisions are dropped vs the target replay; not capacity qualified";

Json scenario_recipe(const NavScenario& s, bool primary) {
  const Json participation = std::isfinite(s.max_participation) ? Json(s.max_participation)
                                                                : Json("uncapped");
  return Json{{"id", s.id}, {"primary", primary},
      {"cost_rule", s.cost == NavCostRule::FlatBpsV1 ? "flat-bps-v1" : "sqrt-impact-v1"},
      {"flat_bps", s.flat_bps}, {"half_spread_bps", s.half_spread_bps},
      {"commission_bps", s.commission_bps}, {"impact_y", s.impact_y},
      {"impact_delta", s.impact_delta}, {"max_participation", participation},
      {"annual_borrow_bps", s.annual_borrow_bps}, {"borrow_day_count", "calendar-days/365"},
      {"fallback_daily_vol", s.fallback_daily_vol},
      {"stale_exit_sessions", s.stale_exit_sessions},
      {"terminal_haircut_long", s.adverse_terminal ? nav_adverse_long_return : 0.0},
      {"terminal_haircut_short", s.adverse_terminal ? nav_adverse_short_return : 0.0}};
}
Json nav_recipe(const TargetReplayRunConfig& cfg, const NavReplayConfig& base,
                const std::vector<NavScenario>& scenarios) {
  Json list = Json::array();
  for (usize k = 0; k < scenarios.size(); ++k)
    list.push_back(scenario_recipe(scenarios[k], k == nav_primary_scenario_index));
  return Json{{"schema", "atx.dsl-nav-replay/v1"},
      {"combined_sha256", cfg.combined_sha256}, {"role_sha256", cfg.role_sha256},
      {"rule", rule_label(cfg.target.rule)}, {"cadence", cfg.target.cadence},
      {"trade_fraction", cfg.target.trade_fraction},
      {"monthly_budget", cfg.target.monthly_budget},
      {"initial_nav", base.initial_nav}, {"liquidity_window", base.liquidity_window},
      {"min_vol_pairs", base.min_vol_pairs}, {"max_events", base.max_events},
      {"max_working_bytes", cfg.target.max_working_bytes}, {"scenarios", std::move(list)},
      {"primary_scenario", scenarios[nav_primary_scenario_index].id},
      {"sharpe_target", nav_sharpe_target},
      {"monthly_turnover_target", nav_monthly_turnover_target},
      {"timing", timing_declaration}, {"accounting", accounting_declaration},
      {"target", target_declaration}, {"turnover", turnover_definition},
      {"missing", missing_declaration}, {"guard", guard_declaration},
      {"liquidity", liquidity_declaration}, {"desired_target_postprocess", "none"},
      {"cost_input_status", "declared-unfitted-scenario-no-locate"}};
}
Json months_json(const NavSummary& s) {
  Json months = Json::array();
  for (const auto& m : s.months)
    months.push_back({{"month", m.month}, {"one_way_turnover", m.one_way_turnover},
        {"traded_dollars", m.traded_dollars}, {"execution_sessions", m.execution_sessions},
        {"traded_sessions", m.traded_sessions}, {"is_deployment_month", m.is_deployment_month},
        {"planned_turnover_decision_month", m.planned_turnover},
        {"decision_sessions", m.decision_sessions}});
  return months;
}
Json turnover_json(const NavSummary& s) {
  f64 monthly_total = 0;
  for (const auto& m : s.months) monthly_total += m.one_way_turnover;
  return Json{{"turnover_definition", turnover_definition}, {"months", months_json(s)},
      {"execution_months", s.execution_months},
      {"mean_monthly_one_way_turnover", finite_or_null(s.mean_monthly_turnover)},
      {"max_monthly_one_way_turnover", s.max_monthly_turnover},
      {"mean_monthly_one_way_turnover_ex_deployment_month",
       finite_or_null(s.mean_monthly_turnover_ex_deployment)},
      {"max_monthly_one_way_turnover_ex_deployment_month",
       finite_or_null(s.max_monthly_turnover_ex_deployment)},
      {"months_le_0.30", s.months_within_target},
      {"months_le_0.30_ex_deployment_month", s.months_within_target_ex_deployment},
      {"deployment", {{"occurred", s.deployed},
          {"session_ns", s.deployed ? Json(s.deployment_session) : Json(nullptr)},
          {"one_way_turnover", s.deployment_turnover},
          {"traded_dollars", s.deployment_dollars}}},
      {"planned_vs_actual", {
          {"total_planned_turnover_decision_basis", s.total_planned_turnover},
          {"total_actual_one_way_turnover_execution_basis", s.total_actual_turnover},
          {"actual_minus_planned", s.total_actual_turnover - s.total_planned_turnover},
          {"monthly_reconciled_total", monthly_total},
          {"monthly_reconciliation_error", monthly_total - s.total_actual_turnover},
          {"basis", "planned = sum|next-cur| on drifted marked weights at decision NAV; "
                    "actual = sum|fills|/pre-trade NAV at execution; gaps: drift, caps and "
                    "working orders, blocked absent names, write-offs"}}}};
}
Json risk_json(const NavReplayResult& r, const NavSummary& s) {
  return Json{
      {"costs", {{"trade_cost_dollars", s.trade_cost_dollars},
          {"linear_cost_dollars", s.linear_cost_dollars},
          {"impact_cost_dollars", s.impact_cost_dollars},
          {"unrationed_full_request_cost_dollars", s.unrationed_cost_dollars},
          {"unrationed_unpriced_requests", s.unrationed_unpriced},
          {"borrow_dollars", s.borrow_dollars}, {"writeoff_pnl_dollars", s.writeoff_dollars},
          {"summed_trade_cost_return", s.summed_trade_cost_return},
          {"summed_borrow_return", s.summed_borrow_return},
          {"summed_writeoff_return", s.summed_writeoff_return}}},
      {"guard", {{"guarded_intervals", r.guarded.count},
          {"guarded_gross_exposure_dollars", r.guarded.gross_exposure},
          {"raw_minus_adjusted_pnl_sensitivity_dollars", r.guard_sensitivity}}},
      {"missing", {{"gap_run_1", bucket_json(r.gap_run_1)},
          {"gap_run_2_4", bucket_json(r.gap_run_2_4)},
          {"gap_run_5_plus", bucket_json(r.gap_run_5_plus)},
          {"written_off", bucket_json(r.written_off)},
          {"reappeared_after_write_off_forgone", bucket_json(r.reappeared)},
          {"unresolved_at_end_stale_mark", bucket_json(r.unresolved)},
          {"blocked_absent_order_sessions", s.blocked_absent}}},
      {"exposure", {{"mean_gross_leverage", s.mean_gross_leverage},
          {"max_gross_leverage", s.max_gross_leverage},
          {"max_abs_net_leverage", s.max_abs_net_leverage},
          {"mean_held_names", s.mean_held_names}, {"mean_stale_names", s.mean_stale_names},
          {"max_stale_names", s.max_stale_names},
          {"max_stale_gross_fraction", s.max_stale_gross_fraction},
          {"min_cash_ratio", s.min_cash_ratio}}},
      {"capacity", {{"fills", s.fills}, {"capped_fills", s.capped_fills},
          {"unfilled_dollars", s.unfilled_dollars},
          {"liquidity_blocked_orders", s.blocked_liquidity},
          {"fallback_vol_fills", s.fallback_vol_fills},
          {"participation_fills", r.participation_fills},
          {"participation_p95_upper_bound", finite_or_null(r.participation_p95)},
          {"participation_p95_resolution", "0.01-decade histogram bin upper edge"},
          {"participation_max", finite_or_null(r.participation_max)}}},
      {"accounting_checks", {{"max_return_identity_error", r.max_return_identity_error},
          {"max_cash_book_relative_error", r.max_cash_book_error},
          {"tolerance", identity_tolerance}}}};
}
Json scenario_summary(const NavScenario& sc, bool primary, const NavReplayResult& r,
                      const NavSummary& s, const std::string& daily_sha,
                      const std::string& events_sha) {
  Json years = Json::array();
  for (const auto& y : s.years)
    years.push_back({{"year", y.year}, {"observations", y.observations},
                     {"net_compounded_return", y.net_return}});
  Json j{{"scenario", sc.id}, {"primary", primary}, {"observations", s.observations},
      {"net_sharpe", finite_or_null(s.net_sharpe)},
      {"gross_sharpe", finite_or_null(s.gross_sharpe)},
      {"hac_t", finite_or_null(s.hac_t)}, {"hac_defined", s.hac_defined},
      {"hac_kernel", "bartlett"}, {"hac_lag", s.hac_lag},
      {"mean_daily_net_return", s.mean_daily_net}, {"ann_mean", s.ann_mean},
      {"ann_vol", finite_or_null(s.ann_vol)}, {"cagr", s.cagr},
      {"max_drawdown", s.max_drawdown}, {"total_net_return", s.total_net_return},
      {"final_nav", s.final_nav}, {"calendar_year_returns", std::move(years)},
      {"meets_sharpe_target", s.meets_sharpe_target},
      {"meets_turnover_target_mean", s.meets_turnover_target_mean},
      {"meets_turnover_target_mean_ex_deployment_month",
       s.meets_turnover_target_mean_ex_deployment},
      {"meets_turnover_target_all_months", s.meets_turnover_target_all_months},
      {"meets_turnover_target_all_months_ex_deployment_month",
       s.meets_turnover_target_all_months_ex_deployment},
      {"events", r.events.size()}, {"daily_csv_sha256", daily_sha},
      {"events_csv_sha256", events_sha}};
  j.update(turnover_json(s));
  j.update(risk_json(r, s));
  return j;
}

co::Status write_json(const std::filesystem::path& path, const Json& j) {
  std::ofstream file(path, std::ios::binary);
  if (!file) return co::Err(co::ErrorCode::IoError, "nav replay: JSON output");
  file << j.dump(2) << '\n'; file.close();
  return file ? co::Ok() : co::Status(co::Err(co::ErrorCode::IoError, "nav replay: JSON close"));
}
co::Status write_daily(const std::filesystem::path& path, const NavReplayResult& r) {
  std::ofstream file(path, std::ios::binary);
  if (!file) return co::Err(co::ErrorCode::IoError, "nav replay: daily output");
  file.imbue(std::locale::classic()); file << std::setprecision(17);
  file << "session_index,session_ns,exec_month,decision,rebalance,executed,return_observation,"
          "pretrade_nav,posttrade_nav,net_return,gross_return,writeoff_return,trade_cost_return,"
          "borrow_return,mark_pnl_dollars,writeoff_dollars,borrow_dollars,traded_dollars,"
          "one_way_turnover,trade_cost_dollars,linear_cost_dollars,impact_cost_dollars,"
          "unrationed_cost_dollars,unrationed_unpriced,fills,capped_fills,unfilled_dollars,"
          "blocked_absent,blocked_liquidity,fallback_vol_fills,planned_turnover,planned_forced,"
          "planned_discretionary,applied_fraction,planned_gross,planned_net,month_planned,"
          "budget_excess,long_dollars,"
          "short_dollars,gross_leverage,net_leverage,held_names,stale_names,stale_long_dollars,"
          "stale_short_dollars,guarded_intervals,cash_ratio\n";
  for (const auto& d : r.days) {
    file << d.session_index << ',' << d.session << ',' << d.calendar_month << ','
         << d.decision << ',' << d.rebalance << ',' << d.executed << ','
         << d.return_observation << ',' << d.pretrade_nav
         << ',' << d.posttrade_nav << ',' << d.net_return << ',' << d.gross_return << ','
         << d.writeoff_return << ',' << d.trade_cost_return << ',' << d.borrow_return << ','
         << d.mark_pnl_dollars << ',' << d.writeoff_dollars << ',' << d.borrow_dollars << ','
         << d.traded_dollars << ',' << d.one_way_turnover << ',' << d.trade_cost_dollars << ','
         << d.linear_cost_dollars << ',' << d.impact_cost_dollars << ','
         << d.unrationed_cost_dollars << ',' << d.unrationed_unpriced << ',' << d.fills << ','
         << d.capped_fills << ','
         << d.unfilled_dollars << ',' << d.blocked_absent << ',' << d.blocked_liquidity << ','
         << d.fallback_vol_fills << ',' << d.planned_turnover << ',' << d.planned_forced << ','
         << d.planned_discretionary << ',' << d.applied_fraction << ',' << d.planned_gross << ','
         << d.planned_net << ',' << d.month_planned << ','
         << d.budget_excess << ',' << d.long_dollars << ',' << d.short_dollars << ','
         << d.gross_leverage << ',' << d.net_leverage << ',' << d.held_names << ',' << d.stale_names
         << ',' << d.stale_long_dollars << ',' << d.stale_short_dollars << ','
         << d.guarded_intervals << ',' << d.cash_ratio << '\n';
  }
  file.close();
  return file ? co::Ok() : co::Status(co::Err(co::ErrorCode::IoError, "nav replay: daily close"));
}
const char* event_label(NavEventKind kind) {
  switch (kind) {
  case NavEventKind::GapResolved: return "gap-resolved";
  case NavEventKind::WriteOff: return "write-off";
  case NavEventKind::Guarded: return "guarded";
  case NavEventKind::ReappearedAfterWriteOff: return "reappeared-after-write-off";
  case NavEventKind::UnresolvedAtEnd: return "unresolved-at-end";
  }
  return "unknown";
}
// Deterministic spelling of unobservable values across standard libraries.
void write_value(std::ostream& out, f64 x) {
  if (std::isnan(x)) out << "nan"; else out << x;
}
co::Status write_events(const std::filesystem::path& path, const NavReplayResult& r) {
  std::ofstream file(path, std::ios::binary);
  if (!file) return co::Err(co::ErrorCode::IoError, "nav replay: events output");
  file.imbue(std::locale::classic()); file << std::setprecision(17);
  file << "kind,session_ns,instrument_id,side,run_length,exposure_dollars,r_adj,r_raw,"
          "haircut,pnl_dollars\n";
  for (const auto& e : r.events) {
    file << event_label(e.kind) << ',' << e.session << ',' << e.instrument_id << ','
         << (e.short_side ? "short" : "long") << ',' << e.run_length << ',';
    write_value(file, e.exposure); file << ','; write_value(file, e.r_adj); file << ',';
    write_value(file, e.r_raw); file << ','; write_value(file, e.haircut); file << ',';
    write_value(file, e.pnl); file << '\n';
  }
  file.close();
  return file ? co::Ok() : co::Status(co::Err(co::ErrorCode::IoError, "nav replay: events close"));
}
// Conservative bound on everything the NAV path holds beside the loaded input:
// all scenario results (days + events at cap), per-name state and publication.
u64 nav_reserve_bytes(u64 max_events) {
  return publication_slack_bytes + scenario_count * fixed_workspace_bytes +
         u64{max_names} * per_name_bytes +
         scenario_count * (u64{max_dates} * sizeof(NavReplayDay) + max_events * sizeof(NavEvent));
}
} // namespace

std::vector<NavScenario> fixed_nav_scenarios() {
  NavScenario linear;
  linear.id = "linear-6bps-stale5-v1"; linear.cost = NavCostRule::FlatBpsV1;
  linear.flat_bps = 6; linear.max_participation = inf; linear.annual_borrow_bps = 300;
  linear.fallback_daily_vol = 0.05; linear.stale_exit_sessions = 5;
  NavScenario modeled;
  modeled.id = "modeled-1bn-stale5-v1"; modeled.cost = NavCostRule::SqrtImpactV1;
  modeled.half_spread_bps = 5; modeled.commission_bps = 1; modeled.impact_y = 0.6;
  modeled.impact_delta = 0.5; modeled.max_participation = 0.01; modeled.annual_borrow_bps = 300;
  modeled.fallback_daily_vol = 0.05; modeled.stale_exit_sessions = 5;
  NavScenario adverse = modeled;
  adverse.id = "modeled-1bn-terminal-adverse-v1"; adverse.stale_exit_sessions = 1;
  adverse.adverse_terminal = true;
  return {linear, modeled, adverse};
}

co::Result<NavReplayResult> replay_nav(const NavReplayInput& in, const NavReplayConfig& cfg) {
  ATX_TRY_VOID(validate_nav_input(in, cfg));
  try {
    ATX_TRY(auto model, make_cost_model(cfg.scenario));
    const auto& s = cfg.scenario;
    const bool sqrt_model = s.cost == NavCostRule::SqrtImpactV1;
    const Ctx ctx{in.target, in.volume, cfg, *model,
                  s.adverse_terminal ? nav_adverse_long_return : 0.0,
                  s.adverse_terminal ? nav_adverse_short_return : 0.0,
                  s.annual_borrow_bps * 1e-4,
                  sqrt_model ? (s.half_spread_bps + s.commission_bps) * 1e-4 : 0.0, sqrt_model};
    return run_book(ctx);
  } catch (const std::bad_alloc&) {
    return co::Err(co::ErrorCode::OutOfRange, "nav replay: allocation failed");
  } catch (const std::length_error&) {
    return co::Err(co::ErrorCode::OutOfRange, "nav replay: allocation extent");
  }
}

co::Result<NavSummary> summarize_nav(const NavReplayResult& result) {
  const auto& days = result.days;
  if (days.size() < 3)
    return co::Err(co::ErrorCode::InvalidArgument, "nav summary: fewer than three sessions");
  try {
    NavSummary s; s.min_cash_ratio = inf;
    Moments net, gross; std::vector<f64> series; series.reserve(days.size());
    std::map<u32, NavMonth> months;
    struct YearNav { usize observations{}; f64 first{}, last{}; };
    std::map<i32, YearNav> years;
    f64 peak = days.front().pretrade_nav, leverage = 0, held = 0, stale = 0;
    for (usize k = 0; k < days.size(); ++k) {
      const auto& d = days[k];
      if (d.executed || d.decision) {
        auto& m = months[d.calendar_month]; m.month = d.calendar_month;
        if (d.executed) {
          ++m.execution_sessions; m.traded_sessions += d.traded_dollars > 0 ? 1U : 0U;
          m.one_way_turnover += d.one_way_turnover; m.traded_dollars += d.traded_dollars;
          s.total_actual_turnover += d.one_way_turnover;
        }
        if (d.decision) {
          ++m.decision_sessions; m.planned_turnover += d.planned_turnover;
          s.total_planned_turnover += d.planned_turnover;
        }
      }
      accumulate_day(d, s);
      peak = std::max(peak, d.pretrade_nav);
      s.max_drawdown = std::max(s.max_drawdown, 1.0 - d.pretrade_nav / peak);
      if (!d.return_observation || k == 0) continue;
      net.add(d.net_return); gross.add(d.gross_return); series.push_back(d.net_return);
      s.summed_trade_cost_return += d.trade_cost_return;
      s.summed_borrow_return += d.borrow_return; s.summed_writeoff_return += d.writeoff_return;
      // Exposure that earned this row's return: the previous session's closing book.
      leverage += days[k - 1].gross_leverage; held += static_cast<f64>(days[k - 1].held_names);
      stale += static_cast<f64>(d.stale_names);
      auto& y = years[static_cast<i32>(detail::calendar_month(d.session) / 100U)];
      if (y.observations++ == 0) y.first = days[k - 1].pretrade_nav;
      y.last = d.pretrade_nav;
    }
    if (net.n < 2) return co::Err(co::ErrorCode::Unavailable, "nav summary: too few return rows");
    const f64 n = static_cast<f64>(net.n);
    s.observations = net.n; s.net_sharpe = net.sharpe(); s.gross_sharpe = gross.sharpe();
    s.mean_daily_net = net.mean; s.ann_mean = net.mean * 252;
    s.ann_vol = net.sd() * std::sqrt(252.0);
    s.final_nav = days.back().pretrade_nav;
    s.total_net_return = s.final_nav / days.front().pretrade_nav - 1;
    s.cagr = std::pow(s.final_nav / days.front().pretrade_nav, 252.0 / n) - 1;
    const auto inference = hac::mean_inference(std::span<const f64>{series},
                                                hac::Kernel::BartlettV1, 5, true, true);
    s.hac_defined = inference.defined != 0; s.hac_t = s.hac_defined ? inference.t : nan;
    s.hac_lag = inference.lag;
    s.mean_gross_leverage = leverage / n; s.mean_held_names = held / n;
    s.mean_stale_names = stale / n;
    for (const auto& [year, v] : years)
      s.years.push_back({year, v.observations, v.last / v.first - 1});
    u32 deployment_month = 0;
    const usize first = days.front().session_index;
    if (result.deployment_index >= first && result.deployment_index - first < days.size()) {
      const auto& d = days[result.deployment_index - first];
      s.deployed = true; s.deployment_session = d.session; deployment_month = d.calendar_month;
      s.deployment_turnover = d.one_way_turnover; s.deployment_dollars = d.traded_dollars;
    }
    summarize_months(months, deployment_month, s);
    s.meets_sharpe_target = s.net_sharpe >= nav_sharpe_target;
    return co::Ok(std::move(s));
  } catch (const std::bad_alloc&) {
    return co::Err(co::ErrorCode::OutOfRange, "nav summary: allocation failed");
  }
}

co::Status run_nav_replay(const TargetReplayRunConfig& cfg, std::ostream& progress) {
  try {
    if (cfg.role_path.empty() || cfg.role_sha256.empty() || cfg.output_directory.empty())
      return co::Err(co::ErrorCode::InvalidArgument, "nav replay: pinned role and output required");
    if (cfg.target.one_way_bps != 0 || cfg.target.annual_borrow_bps != 0)
      return co::Err(co::ErrorCode::InvalidArgument,
                     "nav replay: costs and borrow are fixed scenarios, not flags");
    const auto scenarios = fixed_nav_scenarios();
    NavReplayConfig base; base.target = cfg.target;
    // Admission: the loader is charged against what remains after the NAV
    // workspace reserve, so input + all scenario results fit the one budget.
    const u64 reserve = nav_reserve_bytes(base.max_events);
    if (cfg.target.max_working_bytes <= reserve)
      return co::Err(co::ErrorCode::OutOfRange, "nav replay: budget below NAV workspace reserve");
    auto load = cfg; load.target.max_working_bytes = cfg.target.max_working_bytes - reserve;
    ATX_TRY(auto blend, detail::load_saved_blend(load, true));
    const NavReplayInput input{blend.view(), blend.volume};
    std::vector<NavReplayResult> results; results.reserve(scenarios.size());
    std::vector<NavSummary> summaries; summaries.reserve(scenarios.size());
    for (const auto& scenario : scenarios) {
      auto run = base; run.scenario = scenario;
      ATX_TRY(auto result, replay_nav(input, run));
      ATX_TRY(auto summary, summarize_nav(result));
      results.push_back(std::move(result)); summaries.push_back(std::move(summary));
    }
    const auto dir = std::filesystem::path(cfg.output_directory);
    if (!std::filesystem::create_directory(dir))
      return co::Err(co::ErrorCode::AlreadyExists, "nav replay: output must not exist");
    const auto method = nav_recipe(cfg, base, scenarios);
    ATX_TRY(auto method_sha, co::sha256_hex(method.dump()));
    ATX_TRY_VOID(write_json(dir / "recipe.json", method));
    Json list = Json::array();
    for (usize k = 0; k < scenarios.size(); ++k) {
      const auto daily = dir / ("daily_" + scenarios[k].id + ".csv");
      const auto events = dir / ("events_" + scenarios[k].id + ".csv");
      ATX_TRY_VOID(write_daily(daily, results[k]));
      ATX_TRY_VOID(write_events(events, results[k]));
      ATX_TRY(auto daily_sha, co::sha256_file(daily.string()));
      ATX_TRY(auto events_sha, co::sha256_file(events.string()));
      list.push_back(scenario_summary(scenarios[k], k == nav_primary_scenario_index, results[k],
                                      summaries[k], daily_sha, events_sha));
      progress << "nav replay " << scenarios[k].id << ": net Sharpe "
               << std::setprecision(6) << summaries[k].net_sharpe << ", mean monthly turnover "
               << summaries[k].mean_monthly_turnover << " (ex deployment "
               << summaries[k].mean_monthly_turnover_ex_deployment << ")\n";
    }
    auto bindings = Json::parse(blend.manifest_json);
    // Equal-weight blends carry no weights key; pinned-weight blends bind their SHA.
    const auto weights = bindings.contains("composition_weights_sha256")
        ? bindings.at("composition_weights_sha256") : Json(nullptr);
    const auto semantics = bindings.at("signal_semantics");
    Json summary{{"schema", "atx.dsl-nav-replay-summary/v1"}, {"status", "complete"},
        {"recipe_sha256", method_sha}, {"combined_sha256", cfg.combined_sha256},
        {"role_sha256", cfg.role_sha256}, {"rule", rule_label(cfg.target.rule)},
        {"primary_scenario", scenarios[nav_primary_scenario_index].id},
        {"signal_semantics", semantics}, {"composition_weights_sha256", weights},
        {"scenarios", std::move(list)}, {"source_bindings", std::move(bindings)},
        {"self_financing_nav", true}, {"capacity_qualified", false},
        {"limitations", limitations_declaration}};
    ATX_TRY_VOID(write_json(dir / "summary.json", summary));
    return co::Ok();
  } catch (const std::exception& e) {
    return co::Err(co::ErrorCode::InvalidArgument, std::string("nav replay: ") + e.what());
  }
}

int dispatch_nav_replay(int argc, char** argv, std::ostream& out, std::ostream& err) {
  try {
    TargetReplayRunConfig cfg; std::set<std::string> seen;
    for (int i = 1; i < argc; ++i) {
      const std::string key = argv[i];
      if (key == "--help") {
        out << "nav --combined PATH --combined-sha256 SHA --role PATH --role-sha256 SHA "
               "--output NEWDIR [--rule baseline-v1|monthly-budget-v2] [--cadence 5] "
               "[--trade-fraction .25] [--monthly-budget .30] [--max-bytes 536870912]\n"
               "Runs every fixed scenario (S1 linear-6bps-stale5-v1, S2 modeled-1bn-stale5-v1 "
               "PRIMARY, S3 modeled-1bn-terminal-adverse-v1); costs/borrow are not flags.\n";
        return 0;
      }
      if (key == "--one-way-bps" || key == "--annual-borrow-bps")
        throw std::invalid_argument(key + " is not accepted in nav mode (fixed cost scenarios)");
      if (!seen.insert(key).second || i + 1 >= argc)
        throw std::invalid_argument("duplicate/missing flag");
      const std::string value = argv[++i];
      const auto real = [&]() {
        usize used = 0; const f64 x = std::stod(value, &used);
        if (used != value.size()) throw std::invalid_argument("invalid number");
        return x;
      };
      const auto integer = [&]() {
        u64 x = 0;
        const auto parsed = std::from_chars(value.data(), value.data() + value.size(), x);
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
        const auto x = integer();
        if (x > max_dates) throw std::invalid_argument("cadence exceeds bound");
        cfg.target.cadence = static_cast<usize>(x);
      } else if (key == "--trade-fraction") cfg.target.trade_fraction = real();
      else if (key == "--monthly-budget") cfg.target.monthly_budget = real();
      else if (key == "--max-bytes") cfg.target.max_working_bytes = integer();
      else if (key == "--rule") {
        if (value == "baseline-v1") cfg.target.rule = TargetReplayRule::BaselineTargetV1;
        else if (value == "monthly-budget-v2")
          cfg.target.rule = TargetReplayRule::MonthlyTargetBudgetV2;
        else throw std::invalid_argument("unknown target rule");
      } else throw std::invalid_argument("unknown flag: " + key);
    }
    if (cfg.role_path.empty() || cfg.role_sha256.empty())
      throw std::invalid_argument("--role and --role-sha256 are required in nav mode");
    const auto status = run_nav_replay(cfg, out);
    if (!status) { err << status.error().to_string() << '\n'; return 1; }
    return 0;
  } catch (const std::exception& e) { err << "nav replay: " << e.what() << '\n'; return 2; }
}
} // namespace atx::impl::strategy
