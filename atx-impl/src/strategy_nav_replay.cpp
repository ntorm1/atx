#include "strategy_nav_replay.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <initializer_list>
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
#include "atx/engine/cost/borrow_tiers.hpp"
#include "atx/engine/eval/hac.hpp"
#include "strategy_holdings.hpp" // v7 W4: the f64 holdings layout
#include "strategy_nav_replay_detail.hpp"
#include "strategy_target_replay_detail.hpp"
#include "strategy_nav_v7.hpp" // platform-v7 L4 hook (one seam object)

namespace atx::impl::strategy {
namespace {
using namespace atx;
namespace co = atx::core;
namespace bk = atx::engine::book;
namespace ce = atx::engine::cost;
namespace hac = atx::engine::eval::hac;
using Json = nlohmann::json;
constexpr f64 nan = std::numeric_limits<f64>::quiet_NaN();
constexpr f64 inf = std::numeric_limits<f64>::infinity();
constexpr i64 day_ns = 86'400'000'000'000LL;
constexpr i64 hour_ns = 3'600'000'000'000LL;
constexpr usize max_dates = 4096, max_names = 20000;
constexpr usize max_scenarios = 8; // books run in lockstep by replay_nav_scenarios
constexpr u64 max_event_cap = 1ULL << 24;
// Per scenario book: 8 f64 + u32 + 2 u8 per name (70 B), plus the delta order basis's
// f64 anchor and u8 flag (79 B; allocated only under it), rounded up.
constexpr u64 per_name_bytes = 192;
// Shared construction per name: one ranked pair + the desired weight.
constexpr u64 shared_name_bytes = sizeof(std::pair<f64, usize>) + sizeof(f64);
// Shared borrow tiers per name (only with fields): tier, missing flag, first present row.
constexpr u64 tier_name_bytes = 2 + sizeof(usize);
// Shared per-name rate state per name (only with rate per-name-v1 or the liquidity
// cache): the session's liquidity cache (ADV, sigma) and one book's rates.
constexpr u64 rate_name_bytes = 3 * sizeof(f64);
// --emit-holdings only: the observed book's per-name trace (four f64 and a status byte)
// and one reported row per name in the session buffer.
constexpr u64 holdings_name_bytes = 4 * sizeof(f64) + 1 + sizeof(NavHolding);
// Borrow-tier clocks (role clock_recipe modeled-session+22h-mark+23h-decision-v1):
// every field cell of row d is visible by the session-date 22:00 UTC mark.
constexpr i64 fields_mark_ns = 22 * hour_ns, decision_clock_ns = 23 * hour_ns;
constexpr u8 tier_gc = static_cast<u8>(ce::BorrowTier::GeneralCollateral);
constexpr u8 tier_warm = static_cast<u8>(ce::BorrowTier::Warm);
constexpr u8 tier_special = static_cast<u8>(ce::BorrowTier::Special);
constexpr usize never_present = std::numeric_limits<usize>::max();
// Present at role session 0: seasoned (finite, as the engine requires, and >= any
// young-IPO threshold).
constexpr f64 seasoned_age_days = std::numeric_limits<f64>::max();
constexpr u64 max_fields_manifest_bytes = 16ULL << 20, max_role_manifest_bytes = 1ULL << 20;
constexpr const char* fields_schema = "atx.research-role-fields/v1";
constexpr std::array<const char*, 2> financing_field_names{"shares_out", "si_shares"};
// Histograms (~45 KB), cost model, locals and the locate-in-aim no-short mask (one
// byte per name, <= 20 KB, shared).
constexpr u64 fixed_workspace_bytes = 1ULL << 20;
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
constexpr const char* daily_turnover_definition =
    "daily one-way turnover in GMV units: tau_t = sum_i |filled dollars_i,t| / pre-trade "
    "gross (long$ + short$ after the mark at t, before trading; stale names at stale "
    "marks) over EXECUTED fills only (forced exits included, planned turnover never; "
    "write-offs are not fills); statistics over execution sessions with positive "
    "pre-trade gross, the deployment session (first nonzero fill) excluded; quantiles "
    "interpolate linearly at (n-1)q over the sorted sessions (numpy default); ceilings "
    "are declared before results and a NaN statistic never meets them";

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
// Streaming NavRateStats of one book's per-name rates, all in [lo, hi]: exact count,
// sum, extremes and clip masses (== lo, == hi); interior values in `bins` equal bins
// over [lo, hi]. Fixed size (inside fixed_workspace_bytes); nothing is stored per sample.
class RateStatistics {
public:
  static constexpr usize bins = 4096;
  void reset(f64 lo, f64 hi) { lo_ = lo; hi_ = hi; }
  void add(f64 rate) {
    assert(rate >= lo_ && rate <= hi_); // per_name_rate_v1 clips, never NaN
    ++n_; sum_ += rate; min_ = std::min(min_, rate); max_ = std::max(max_, rate);
    const bool low = rate <= lo_, high = rate >= hi_;
    at_min_ += low ? 1U : 0U; at_max_ += high ? 1U : 0U;
    if (low || high) return;
    // lo < rate < hi, so the position is in (0, bins); rounding is clamped.
    const f64 position = (rate - lo_) / (hi_ - lo_) * static_cast<f64>(bins);
    const usize bin = position >= static_cast<f64>(bins - 1) ? bins - 1
                                                              : static_cast<usize>(position);
    ++counts_[bin];
  }
  [[nodiscard]] NavRateStats stats() const {
    NavRateStats s;
    s.n = n_; s.at_min_count = at_min_; s.at_max_count = at_max_;
    const f64 count = static_cast<f64>(n_);
    s.mean = n_ ? sum_ / count : nan;
    s.min = n_ ? min_ : nan; s.max = n_ ? max_ : nan;
    s.p05 = quantile(.05); s.p50 = quantile(.5); s.p95 = quantile(.95);
    s.share_at_min = n_ ? static_cast<f64>(at_min_) / count : nan;
    s.share_at_max = n_ ? static_cast<f64>(at_max_) / count : nan;
    return s;
  }
private:
  // Rank ceil(q n) in the order (lo mass, interior bins, hi mass): lo or hi exactly
  // inside a mass (lo == hi puts every sample in the lo mass), else its bin's upper
  // edge capped at the observed max.
  [[nodiscard]] f64 quantile(f64 q) const {
    if (!n_) return nan;
    const usize rank = std::max<usize>(1, static_cast<usize>(std::ceil(q * static_cast<f64>(n_))));
    if (rank <= at_min_) return lo_;
    usize seen = at_min_;
    const f64 width = (hi_ - lo_) / static_cast<f64>(bins);
    for (usize b = 0; b < bins; ++b) {
      seen += counts_[b];
      if (seen >= rank) return std::min(max_, lo_ + static_cast<f64>(b + 1) * width);
    }
    return hi_;
  }
  std::array<usize, bins> counts_{};
  f64 lo_{}, hi_{}, sum_{}, min_{inf}, max_{-inf};
  usize n_{}, at_min_{}, at_max_{};
};

struct NameState {
  NameState(usize n, bool delta_basis)
      : held(n), mark(n, nan), raw_mark(n, nan), order(n), current(n), planned(n),
        written_exposure(n), written_haircut(n), anchor(delta_basis ? n : 0),
        absent(n), active(n), written_off(n), delta(delta_basis ? n : 0) {}
  // held: marked dollars; mark/raw_mark: last OBSERVED adjusted/raw close;
  // order: working target dollars (decision-NAV dollars) when active.
  std::vector<f64> held, mark, raw_mark, order, current, planned;
  std::vector<f64> written_exposure, written_haircut;
  // Delta order basis only (empty otherwise): delta[i] marks a delta order, whose
  // request is order - anchor with anchor = the holding at its decision plus the dollars
  // filled on it since (never marked), so the remaining delta ignores price drift.
  std::vector<f64> anchor;
  std::vector<u32> absent; // consecutive absent marks while held or written off
  std::vector<u8> active, written_off, delta;
};
bool delta_order(const NameState& s, usize i) { return !s.delta.empty() && s.delta[i] != 0; }
// Decision construction shared by every scenario book of one lockstep replay: the
// desired target depends only on the signal, membership and role prices at d.
struct Construction {
  Construction(usize n, bool locate) : desired(n), no_short(locate ? n : 0) { row.reserve(n); }
  std::vector<std::pair<f64, usize>> row;
  std::vector<f64> desired;
  std::vector<u8> no_short; // locate-in-aim only: 1 = special tier at the decision
  PriceRiskScratch price;   // grows only when neutralizing
  detail::DesiredState state; // v8 (R-4 hold band): carried across decisions; empty when off
};
// Borrow tiers of the latest decision, shared by every book of a lockstep replay.
// They are rate-independent (the engine's flag count only); each book maps a tier to
// its own fee. Inactive (empty) without financing fields.
struct BorrowTiers {
  BorrowTiers(usize n, bool on)
      : active(on), tier(on ? n : 0, tier_warm), missing(on ? n : 0, u8{1}),
        first_present(on ? n : 0, never_present) {}
  bool active{};
  std::vector<u8> tier, missing;
  std::vector<usize> first_present; // first present role row among rows [0, scanned)
  usize scanned{};
};
struct TierCensus {
  std::array<usize, 3> members{}; // GC, warm, special
  usize missing{};
};
struct Ctx {
  const TargetReplayInput& x;
  std::span<const f64> volume;
  const NavReplayConfig& cfg;
  const bk::ReplayCostModel& model;
  f64 eta_long{}, eta_short{}; // terminal haircut return by side (0 unless adverse)
  f64 borrow_rate{};           // FlatShortV0: annual fraction on short dollars
  f64 linear_rate{};           // sqrt model: (half spread + commission) per dollar
  bool sqrt_model{};
  f64 long_rate{};                  // TieredSwapV1: long spread, annual fraction
  std::array<f64, 4> short_rate{}; // TieredSwapV1: short spread + fee, by BorrowTier value
};
// --emit-holdings: the observed book's per-name outcomes of the current session, written
// beside (never read by) the book's arithmetic. EXECUTE fields are cleared every session;
// `rule` is the rule's plan of the latest decision (plan_weights).
struct Trace {
  explicit Trace(usize n) : filled(n), cost(n), unfilled(n), rule(n, nan), fill(n) {}
  void clear() {
    std::fill(filled.begin(), filled.end(), 0.0); std::fill(cost.begin(), cost.end(), 0.0);
    std::fill(unfilled.begin(), unfilled.end(), 0.0); std::fill(fill.begin(), fill.end(), u8{0});
  }
  void record(usize i, NavFillStatus status, f64 dollars, f64 fee, f64 left) {
    fill[i] = static_cast<u8>(status); filled[i] = dollars; cost[i] = fee; unfilled[i] = left;
  }
  std::vector<f64> filled, cost, unfilled, rule;
  std::vector<u8> fill; // NavFillStatus
};
struct Book {
  Book(usize n, bool delta_basis) : names(n, delta_basis) {}
  NameState names;
  f64 cash{}, nav_pre{}, nav_post{}, pending_cost{}, spent{};
  u32 month{};
  ParticipationHistogram participation;
  RateStatistics rates; // rate per-name-v1 only
  NavReplayResult result;
  std::unique_ptr<Trace> trace; // the observed book of --emit-holdings only
};

// The shared per-session liquidity cache: always under rate per-name-v1 (its rates read
// it), and at a fixed rate when --liquidity-cache asks for it.
bool liquidity_cached(const NavReplayConfig& cfg) {
  return cfg.rate == NavRateRule::PerNameV1 || cfg.liquidity_cache;
}
bool valid_id(const std::string& id) {
  return !id.empty() && id.size() <= 64 && std::all_of(id.begin(), id.end(), [](char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-';
  });
}
// Each rule admits only its own parameters, so a spec cannot silently mix them.
bool valid_financing(const NavFinancing& f) {
  if (!valid_id(f.id)) return false;
  switch (f.rule) {
  case NavFinancingRule::FlatShortV0:
    return within(f.flat_short_bps, 0, 100000) && f.long_spread_bps == 0 &&
           f.short_spread_bps == 0 && f.gc_bps == 0 && f.warm_bps == 0 && f.special_bps == 0 &&
           f.day_count == 365 && !f.block_special_shorts;
  case NavFinancingRule::TieredSwapV1:
    return f.flat_short_bps == 0 && within(f.long_spread_bps, 0, 10000) &&
           within(f.short_spread_bps, 0, 10000) && within(f.gc_bps, 0, 100000) &&
           within(f.warm_bps, f.gc_bps, 100000) && within(f.special_bps, f.warm_bps, 100000) &&
           (f.day_count == 360 || f.day_count == 365);
  }
  return false;
}
co::Status validate_nav_config(const NavReplayConfig& cfg) {
  const auto& s = cfg.scenario;
  const bool cost_ok = (s.cost == NavCostRule::FlatBpsV1 || s.cost == NavCostRule::SqrtImpactV1) &&
      within(s.flat_bps, 0, 10000) && within(s.half_spread_bps, 0, 10000) &&
      within(s.commission_bps, 0, 10000) && within(s.impact_y, 0, 100) &&
      within(s.impact_delta, 0, 2) && s.impact_delta > 0 && !std::isnan(s.max_participation) &&
      s.max_participation > 0 && within(s.fallback_daily_vol, 0, 10) && s.fallback_daily_vol > 0;
  if (!valid_id(s.id) || !cost_ok || !valid_financing(s.financing) || !s.stale_exit_sessions ||
      s.stale_exit_sessions > max_dates)
    return co::Err(co::ErrorCode::InvalidArgument, "nav replay: invalid scenario");
  if (!std::isfinite(cfg.initial_nav) || cfg.initial_nav <= 0 || cfg.liquidity_window < 2 ||
      cfg.liquidity_window > max_dates || cfg.min_vol_pairs < 2 ||
      cfg.min_vol_pairs > cfg.liquidity_window || !cfg.max_events ||
      cfg.max_events > max_event_cap || cfg.target.one_way_bps != 0 ||
      cfg.target.annual_borrow_bps != 0 || cfg.warm_start_sessions > max_dates)
    return co::Err(co::ErrorCode::InvalidArgument,
                   "nav replay: invalid recipe (target-replay cost/borrow must be zero)");
  // Trading rate: per-name-v1 is aim-partial-v5 only; a fixed rate carries no rate
  // parameters (each stays at its declared default). within() also refuses NaN.
  const bool rate_ok = cfg.rate == NavRateRule::Fixed
      ? cfg.rate_rra == nav_rate_rra && cfg.rate_lambda == nav_rate_lambda &&
            cfg.rate_min == nav_rate_min && cfg.rate_max == nav_rate_max
      : cfg.rate == NavRateRule::PerNameV1 &&
            cfg.target.rule == TargetReplayRule::AimPartialV5 &&
            within(cfg.rate_rra, 0, 1e6) && cfg.rate_rra > 0 &&
            within(cfg.rate_lambda, 0, 1e6) && cfg.rate_lambda > 0 &&
            within(cfg.rate_min, 0, 1) && cfg.rate_min > 0 &&
            within(cfg.rate_max, cfg.rate_min, 1);
  if (!rate_ok)
    return co::Err(co::ErrorCode::InvalidArgument,
                   "nav replay: rate per-name-v1 needs aim-partial-v5, rate_rra and "
                   "rate_lambda in (0, 1e6] and 0 < rate_min <= rate_max <= 1; a fixed rate "
                   "takes no rate parameters");
  // v6: the order basis is one of its two values; locate-in-aim only zeroes shorts
  // that a regression then re-balances (the borrow fields are checked with the input).
  if ((cfg.order_basis != NavOrderBasis::Target && cfg.order_basis != NavOrderBasis::Delta) ||
      (cfg.locate_in_aim && cfg.target.neutralize == TargetNeutralize::None))
    return co::Err(co::ErrorCode::InvalidArgument,
                   "nav replay: order basis target|delta; locate-in-aim needs a neutralizing "
                   "construction (--neutralize) and the borrow fields");
  return co::Ok();
}
// A warm start of K sessions decides from role row score_begin - K: never before row 0.
co::Status check_warm_start(usize sessions, usize score_begin) {
  if (sessions <= score_begin) return co::Ok();
  return co::Err(co::ErrorCode::InvalidArgument,
                 "nav replay: --warm-start-sessions " + std::to_string(sessions) +
                     " exceeds the role's pre-score history (score_begin = " +
                     std::to_string(score_begin) + " sessions)");
}
co::Status validate_nav_input(const NavReplayInput& in, const NavReplayConfig& cfg,
                              usize books) {
  ATX_TRY_VOID(validate_nav_config(cfg));
  const auto& x = in.target;
  ATX_TRY_VOID(detail::validate_replay_input(x, cfg.target));
  const usize cells = x.dates * x.instruments;
  if (x.close.size() != cells || in.volume.size() != cells ||
      x.decision_end - x.decision_begin < 3)
    return co::Err(co::ErrorCode::InvalidArgument,
                   "nav replay: prices+volume required and at least three window sessions");
  ATX_TRY_VOID(check_warm_start(cfg.warm_start_sessions, x.decision_begin));
  if (neutralize_by_industry(cfg.target.neutralize) && x.industry.size() != cells)
    return co::Err(co::ErrorCode::InvalidArgument,
                   "nav replay: the industry ids need the grp_ff12 field (--fields)");
  for (usize k = 0; k < cells; ++k) {
    const bool bad = x.present[k]
        ? (!std::isfinite(x.close[k]) || x.close[k] <= 0 || !std::isfinite(x.raw_close[k]) ||
           x.raw_close[k] <= 0 || !std::isfinite(in.volume[k]) || in.volume[k] < 0)
        : x.member[k] != 0;
    if (bad)
      return co::Err(co::ErrorCode::InvalidArgument, "nav replay: price/volume/presence contract");
  }
  const auto& f = in.financing;
  const bool tiers = !f.shares_out.empty();
  if (tiers != !f.si_shares.empty() ||
      (tiers && (f.shares_out.size() != cells || f.si_shares.size() != cells)))
    return co::Err(co::ErrorCode::InvalidArgument, "nav replay: financing fields geometry");
  // Envelope EXCLUDES caller-owned input; events are charged at their cap. Every
  // lockstep book holds its state, days and events at once; the construction
  // (desired target and, when neutralizing, the price-risk scratch) and the borrow
  // tiers are shared.
  Budget budget{cfg.target.max_working_bytes};
  if (!budget.add(books, fixed_workspace_bytes) ||
      !budget.add(x.instruments, books * per_name_bytes + shared_name_bytes) ||
      !budget.add(tiers ? x.instruments : 0, tier_name_bytes) ||
      !budget.add(liquidity_cached(cfg) ? x.instruments : 0, rate_name_bytes) ||
      !budget.add(x.decision_end - x.decision_begin, books * sizeof(NavReplayDay)) ||
      !budget.add(cfg.max_events, books * sizeof(NavEvent)) ||
      !budget.add(1, detail::construction_scratch_bytes(cfg.target, x.instruments)))
    return co::Err(co::ErrorCode::OutOfRange, "nav replay: workspace budget");
  return co::Ok();
}
co::Result<std::unique_ptr<const bk::ReplayCostModel>> make_cost_model(const NavScenario& s) {
  ATX_TRY(auto extension, v7::extension_cost_model(s)); // L4 hook: reserved cost-v2 ids
  if (extension) return co::Ok(std::move(extension));
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
// The book-independent part of a liquidity row: raw-dollar ADV and the sample SD,
// NaN exactly when the window has too few pairs (the scenario's fallback applies).
struct WindowLiquidity {
  f64 adv{}, sigma{};
};
// Execution-time liquidity of name i for session t from [t-w, t) only: raw-dollar
// ADV = sum of present raw_close*volume / w (absent or pre-history rows count as
// zero; never rescaled) and the sample SD of adjusted simple returns over adjacent
// present, unguarded pairs (k-1, k), k in [t-w, t), as strategy_runner.cpp does.
// Fewer than min_vol_pairs pairs -> the scenario's declared fallback sigma.
WindowLiquidity window_liquidity(const TargetReplayInput& x, std::span<const f64> volume,
                                 const NavReplayConfig& cfg, usize t, usize i) {
  const usize n = x.instruments, w = cfg.liquidity_window;
  const usize first = t > w ? t - w : 0;
  f64 dollars = 0, mean = 0, m2 = 0; usize pairs = 0;
  for (usize k = first; k < t; ++k) {
    const usize b = k * n + i;
    if (!x.present[b]) continue;
    dollars += x.raw_close[b] * volume[b];
    if (k == 0 || !x.present[b - n] ||
        guarded_move(x.close[b - n], x.close[b], x.raw_close[b - n], x.raw_close[b]))
      continue;
    const f64 r = x.close[b] / x.close[b - n] - 1;
    ++pairs; const f64 delta = r - mean;
    mean += delta / static_cast<f64>(pairs); m2 += delta * (r - mean);
  }
  // Not a fallback: pairs >= min_vol_pairs >= 2 and m2 finite >= 0, so sigma is a
  // finite number >= 0; NaN marks the fallback exactly.
  const bool fallback = pairs < cfg.min_vol_pairs || !std::isfinite(m2) || m2 < 0;
  return {dollars / static_cast<f64>(w),
          fallback ? nan : std::sqrt(m2 / static_cast<f64>(pairs - 1))};
}
WindowLiquidity window_liquidity(const Ctx& c, usize t, usize i) {
  return window_liquidity(c.x, c.volume, c.cfg, t, i);
}
// The scenario's row over a window: its half spread, and its declared fallback sigma
// when the window has too few pairs. The same values as computing it in one piece.
Liquidity liquidity_row(const Ctx& c, const WindowLiquidity& window) {
  Liquidity out;
  out.row.adv_dollars = window.adv;
  out.row.half_spread_bps = c.cfg.scenario.half_spread_bps;
  out.fallback = std::isnan(window.sigma);
  out.row.daily_vol = out.fallback ? c.cfg.scenario.fallback_daily_vol : window.sigma;
  return out;
}
Liquidity liquidity_row(const Ctx& c, usize t, usize i) {
  return liquidity_row(c, window_liquidity(c, t, i));
}
// rate per-name-v1 or --liquidity-cache only (empty otherwise): the book-independent
// liquidity of session t, formed ONCE for every lockstep book, and (per-name-v1 only)
// one book's rates. Allocated once, O(names), reused every session (no per-decision or
// per-sample storage). Execution reads it instead of recomputing the window per book:
// fill_liquidity stores exactly window_liquidity(c, t, i), the value execute_orders
// would otherwise compute (same function, same inputs: the window and pair minimum are
// the shared base config's, x and volume are shared), and a stored f64 reads back
// exactly, so execution is bit-identical; each book still applies its own half spread
// and fallback sigma. The decision's per-name rates read it too (liquidity_row's window
// at session d: [d-w, d)).
struct LiquidityCache {
  LiquidityCache(usize n, bool enabled, bool rates)
      : adv(enabled ? n : 0, nan), sigma(enabled ? n : 0, nan), rate(rates ? n : 0, nan) {}
  [[nodiscard]] bool on() const { return !adv.empty(); }
  std::vector<f64> adv, sigma; // NaN: not formed at this session
  std::vector<f64> rate;       // the planning book's per-name rates at decision d
};
// Session t's cache: every name present at t that is a member of the decision at t
// (when `decision`: per-name rates only), or holds a working order in some book at an
// execution session t. It runs before any book's MARK at t, and MARK only ever cancels
// orders (a write-off), so this covers every order execute_orders prices at t, and
// every decision member (members are present). Other names are NaN.
void fill_liquidity(const Ctx& c, const std::vector<std::unique_ptr<Book>>& books, usize t,
                    bool execution, bool decision, LiquidityCache& cache) {
  const auto& x = c.x; const usize n = x.instruments;
  for (usize i = 0; i < n; ++i) {
    const usize k = t * n + i;
    bool need = x.present[k] && decision && x.member[k];
    if (x.present[k] && execution && !need)
      need = std::any_of(books.begin(), books.end(),
                         [i](const std::unique_ptr<Book>& b) { return b->names.active[i] != 0; });
    if (!need) { cache.adv[i] = nan; cache.sigma[i] = nan; continue; }
    const auto window = window_liquidity(c, t, i);
    cache.adv[i] = window.adv; cache.sigma[i] = window.sigma;
  }
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
struct FinancingCharge {
  f64 short_leg{}, long_leg{};
};
// Financing over (t-1, t] on pre-mark dollars. FlatShortV0 keeps the legacy
// expression (rate x calendar days / 365 per short dollar, summed in name order)
// bit for bit; TieredSwapV1 charges the long spread on every long and (short spread
// + fee) on every short at the tier of the latest decision <= t-1. With tiers, the
// short leg is also split by tier, whatever the rule.
FinancingCharge accrue_financing(const Ctx& c, const NameState& s, const BorrowTiers& tiers,
                                 f64 calendar_days, NavReplayDay& day) {
  const auto& financing = c.cfg.scenario.financing;
  const bool flat = financing.rule == NavFinancingRule::FlatShortV0;
  std::array<f64, 4> short_factor{};
  f64 long_factor = 0;
  if (flat) {
    short_factor.fill(c.borrow_rate * calendar_days / 365.0);
  } else {
    const f64 basis = static_cast<f64>(financing.day_count);
    for (usize k = 0; k < short_factor.size(); ++k)
      short_factor[k] = c.short_rate[k] * calendar_days / basis;
    long_factor = c.long_rate * calendar_days / basis;
  }
  FinancingCharge out;
  for (usize i = 0; i < s.held.size(); ++i) {
    const f64 h = s.held[i];
    if (h > 0 && !flat) out.long_leg += h * long_factor;
    if (!(h < 0)) continue;
    const u8 tier = tiers.active ? tiers.tier[i] : tier_warm;
    const f64 charge = -h * short_factor[tier];
    out.short_leg += charge;
    if (!tiers.active) continue;
    const auto slot = static_cast<usize>(tier - tier_gc);
    day.short_dollars_by_tier[slot] += -h; day.short_financing_by_tier[slot] += charge;
    if (!tiers.missing[i]) continue;
    ++day.missing_predictor_shorts; day.missing_predictor_short_dollars += -h;
  }
  return out;
}
// MARK at t > begin: financing on pre-mark dollars x calendar days / day count, then
// drift/stale/write-off. NAVpre_t = NAVpost_{t-1} + P + W - B - L (B: short leg, L:
// long leg), so the fill costs of t-1 land in r_t; cash + sum(held) must reconcile
// to it (checked in close_day).
co::Status mark_session(const Ctx& c, Book& b, const BorrowTiers& tiers, usize t,
                        NavReplayDay& day) {
  const auto& x = c.x; auto& s = b.names; const usize n = x.instruments;
  const f64 calendar_days = static_cast<f64>((x.session_keys[t] - x.session_keys[t - 1]) / day_ns);
  const auto financing = accrue_financing(c, s, tiers, calendar_days, day);
  const f64 borrow = financing.short_leg, long_financing = financing.long_leg;
  f64 pnl = 0, writeoff = 0;
  for (usize i = 0; i < n; ++i) {
    if (s.held[i] == 0) { ATX_TRY_VOID(mark_flat(c, b, t, i)); continue; }
    if (!x.present[t * n + i]) { ATX_TRY_VOID(carry_absent(c, b, t, i, writeoff)); continue; }
    ATX_TRY_VOID(realize(c, b, t, i, pnl, day));
  }
  // Subtracting a zero long leg is exact: flat books keep the legacy arithmetic.
  b.cash -= borrow; b.cash -= long_financing;
  const f64 base = b.nav_pre;
  b.nav_pre = b.nav_post + pnl + writeoff - borrow - long_financing;
  if (!std::isfinite(b.nav_pre) || b.nav_pre <= 0)
    return co::Err(co::ErrorCode::OutOfRange, "nav replay: nonpositive/nonfinite pre-trade NAV");
  day.mark_pnl_dollars = pnl; day.writeoff_dollars = writeoff; day.borrow_dollars = borrow;
  day.long_financing_dollars = long_financing;
  day.net_return = b.nav_pre / base - 1;
  day.gross_return = (pnl + writeoff) / base; day.writeoff_return = writeoff / base;
  day.trade_cost_return = b.pending_cost / base; day.borrow_return = borrow / base;
  day.long_financing_return = long_financing / base;
  const f64 identity = std::abs(day.net_return - (day.gross_return - day.trade_cost_return -
                                                  day.borrow_return - day.long_financing_return));
  b.result.max_return_identity_error = std::max(b.result.max_return_identity_error, identity);
  if (!(identity <= identity_tolerance))
    return co::Err(co::ErrorCode::Internal, "nav replay: r != gross - cost - financing");
  return co::Ok();
}
// EXECUTE working orders at close t. Absent names are blocked (order persists); an
// unusable ADV fills nothing; a capped fill leaves the residual working. A target
// order requests order - held (the drift since its decision is traded back); a delta
// order requests order - anchor, its remaining decision-dollar delta (the drift rides).
co::Status execute_orders(const Ctx& c, Book& b, usize t, const LiquidityCache& cache,
                          NavReplayDay& day) {
  const auto& x = c.x; auto& s = b.names; const usize n = x.instruments;
  Trace* const trace = b.trace.get(); // --emit-holdings observer; never read back
  f64 cost = 0, traded = 0;
  for (usize i = 0; i < n; ++i) {
    if (!s.active[i]) continue;
    if (!x.present[t * n + i]) {
      ++day.blocked_absent;
      if (trace) trace->record(i, NavFillStatus::BlockedAbsent, 0, 0, 0);
      continue;
    }
    const bool delta = delta_order(s, i);
    const f64 requested = s.order[i] - (delta ? s.anchor[i] : s.held[i]);
    if (requested == 0) { s.active[i] = 0; continue; }
    // fill_liquidity formed it (MARK only cancels orders). Fail loud in debug; in
    // release an unformed (NaN) entry recomputes the window -- the same arithmetic, so
    // the same bytes -- instead of reading as an unusable ADV that silently fills nothing.
    assert(!cache.on() || !std::isnan(cache.adv[i]));
    const auto liquidity = cache.on() && !std::isnan(cache.adv[i])
        ? liquidity_row(c, WindowLiquidity{cache.adv[i], cache.sigma[i]})
        : liquidity_row(c, t, i);
    const auto priced = c.model.cost(i, t, requested, liquidity.row);
    const f64 full = c.model.unrationed_cost(i, t, requested, liquidity.row);
    if (std::isfinite(full)) day.unrationed_cost_dollars += full; else ++day.unrationed_unpriced;
    const f64 filled = priced.filled_dollars, size = std::abs(filled);
    if (filled == 0) {
      ++day.blocked_liquidity; day.unfilled_dollars += std::abs(requested);
      if (trace) trace->record(i, NavFillStatus::BlockedLiquidity, 0, 0, std::abs(requested));
      continue;
    }
    const bool complete = filled == requested;
    if (trace)
      trace->record(i, complete ? NavFillStatus::Complete : NavFillStatus::Capped, filled,
                    priced.cost_dollars, complete ? 0.0 : std::abs(requested) - size);
    // A complete target fill lands exactly on the decision-dollar target; a complete
    // delta fill on that target plus the drift since the decision (held - anchor, an
    // exact 0 without drift, so it is then the target fill bit for bit).
    if (delta) {
      s.held[i] = complete ? s.order[i] + (s.held[i] - s.anchor[i]) : s.held[i] + filled;
      if (!complete) s.anchor[i] += filled;
    } else {
      s.held[i] = complete ? s.order[i] : s.held[i] + filled;
    }
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
// STABLE EXTENSION POINT: the only place the NAV path forms its desired target,
// once per rebalance decision for every scenario book. It is the target replay's
// own construction: the tied-rank target, then the configured post-processing
// (price-risk-v1 neutralization with exposures computed once for d and its skip
// guard), before partial/band/budget planning. Reads only sessions <= d. Returns
// false when the guard skips the rebalance (current weights kept, forced exits
// still apply); `out` receives the decision's construction record. With
// locate-in-aim, shared.no_short (the special tier at d) zeroes negative aims first.
co::Result<bool> form_desired_target(const TargetReplayInput& x, const TargetReplayConfig& target,
                                     usize d, Construction& shared, ConstructionDay& out) {
  return detail::form_desired(x, target, d, shared.row, shared.desired, shared.price, out,
                              shared.no_short, &shared.state);
}
// Engine predictors of name i as of decision d (row d fields and prices, presence
// rows <= d); not available when any is missing or out of its declared domain.
ce::BorrowPredictors borrow_predictors(const TargetReplayInput& x, const NavFinancingFields& f,
                                       const BorrowTiers& tiers, usize d, usize i) {
  ce::BorrowPredictors p;
  const usize k = d * x.instruments + i, first = tiers.first_present[i];
  const f64 shares = f.shares_out[k], short_shares = f.si_shares[k], raw = x.raw_close[k];
  if (!x.present[k] || first > d || !(shares >= nav_shares_out_min) ||
      !(shares <= nav_shares_out_max) || !std::isfinite(short_shares) || short_shares < 0 ||
      !std::isfinite(raw) || !(raw > 0))
    return p;
  const f64 cap = shares * raw;
  if (!std::isfinite(cap)) return p;
  p.available = true;
  p.available_at_ns = x.session_keys[d] + fields_mark_ns;
  p.market_cap_usd = cap; p.raw_price_usd = raw;
  p.short_interest_to_float = short_shares / shares;
  p.ipo_age_calendar_days = first == 0 ? seasoned_age_days
      : static_cast<f64>((x.session_keys[d] - x.session_keys[first]) / day_ns);
  return p;
}
// Borrow tiers at decision d, once for every book. Folds presence rows up to d into
// the first-present rows (monotone: each row is scanned once per replay), then asks
// the engine per name; a missing predictor or Unavailable is warm and flagged. An
// engine refusal of well-formed predictors is an error, never a silent tier.
co::Status classify_borrow(const TargetReplayInput& x, const NavFinancingFields& f, usize d,
                           BorrowTiers& tiers, TierCensus& census) {
  const usize n = x.instruments;
  for (; tiers.scanned <= d; ++tiers.scanned)
    for (usize i = 0; i < n; ++i)
      if (x.present[tiers.scanned * n + i] && tiers.first_present[i] == never_present)
        tiers.first_present[i] = tiers.scanned;
  const i64 session = x.session_keys[d];
  if (session > std::numeric_limits<i64>::max() - decision_clock_ns)
    return co::Err(co::ErrorCode::OutOfRange, "nav replay: borrow decision clock");
  const i64 decision = session + decision_clock_ns;
  const ce::BorrowTierRecipe recipe{}; // thresholds only: every fee is the scenario's
  for (usize i = 0; i < n; ++i) {
    const auto predictors = borrow_predictors(x, f, tiers, d, i);
    u8 tier = tier_warm, missing = 1;
    if (predictors.available) {
      ATX_TRY(const auto estimate, ce::estimate_borrow_tier(predictors, decision, recipe));
      if (estimate.tier != ce::BorrowTier::Unavailable) {
        tier = static_cast<u8>(estimate.tier); missing = 0;
      }
    }
    tiers.tier[i] = tier; tiers.missing[i] = missing;
    if (!x.member[d * n + i]) continue;
    ++census.members[static_cast<usize>(tier - tier_gc)]; census.missing += missing;
  }
  return co::Ok();
}
// A name that may not open or grow a short at the decision: the special tier, or (decide
// --locates only; the replay passes none) no locate.
bool short_guarded(const BorrowTiers& tiers, std::span<const u8> no_locate, usize i) {
  return tiers.tier[i] == tier_special || (!no_locate.empty() && no_locate[i] != 0);
}
// DECIDE, the part shared by every book of decision d: the borrow tiers of d, the
// locate-in-aim mask (allocated only under it), and on a cadence decision the desired
// target (form_desired_target). Returns the effective rebalance (false when the guard
// skips it); `construction` receives the decision's record.
co::Result<bool> decide_construction(const TargetReplayInput& x, const NavFinancingFields& fields,
                                     const TargetReplayConfig& target, usize d, bool cadence,
                                     std::span<const u8> no_locate, Construction& shared,
                                     BorrowTiers& tiers, TierCensus& census,
                                     ConstructionDay& construction) {
  if (tiers.active) ATX_TRY_VOID(classify_borrow(x, fields, d, tiers, census));
  // Locate-in-aim: this decision's special tier, classified just above, guards the aim
  // (replay_nav_scenarios and nav_decide refuse locate-in-aim without borrow fields).
  for (usize i = 0; i < shared.no_short.size(); ++i)
    shared.no_short[i] = short_guarded(tiers, no_locate, i) ? u8{1} : u8{0};
  bool rebalance = cadence;
  if (rebalance) {
    ATX_TRY(rebalance, form_desired_target(x, target, d, shared, construction));
  }
  construction.rebalance = rebalance;
  return co::Ok(rebalance);
}
// Locate rule on the planned weights: a special-tier name may not open or grow a
// short, next = max(next, min(cur, 0)); refused growth is counted in decision-NAV
// dollars.
void block_special_plan(const BorrowTiers& tiers, std::span<const u8> no_locate,
                        const std::vector<f64>& current, std::vector<f64>& planned, f64 nav_post,
                        NavReplayDay& day) {
  for (usize i = 0; i < planned.size(); ++i) {
    if (!short_guarded(tiers, no_locate, i)) continue;
    const f64 floor = std::min(current[i], 0.0);
    if (!(planned[i] < floor)) continue;
    day.blocked_short_dollars += (floor - planned[i]) * nav_post;
    ++day.blocked_short_names; planned[i] = floor;
  }
}
// What every book's weight plan at decision d shares.
struct PlanInputs {
  const TargetReplayInput& x;
  const NavReplayConfig& cfg; // the book's (its scenario's locate rule)
  usize d{};
  bool rebalance{};           // effective
  const std::vector<f64>& desired;
  const BorrowTiers& tiers;
  std::span<const u8> no_locate; // decide --locates only; empty in the replay
};
// CONSTRUCTION-RULE DISPATCH SITE, shared by the NAV replay (plan_decision) and the
// daily decide path (detail::nav_decide), so a rule added here runs in both. `planned`
// becomes the target rule's plan from `current` toward p.desired (`spent`: the book's
// month-to-date plan, v2 only; `rates`: the per-name-v1 rates or empty), copied into
// `rule` when that is non-empty, then the scenario's locate block at the decision NAV.
co::Status plan_weights(const PlanInputs& p, f64 spent, std::span<const f64> rates,
                        const std::vector<f64>& current, std::vector<f64>& planned, f64 nav_post,
                        std::span<f64> rule, TargetReplayDay& plan, NavReplayDay& day) {
  std::copy(current.begin(), current.end(), planned.begin());
  // L4/W1 hook: detail::update_weights unless the v7 extension (aim-partial-v6, spo-v1) is
  // installed; spo-v1 also reads the decision's borrow tiers and decide --locates.
  ATX_TRY_VOID(v7::plan(p.x, p.cfg, p.d, p.rebalance, spent, nav_post, p.desired, planned, plan,
                        rates, p.tiers.tier, p.no_locate));
  if (!rule.empty()) std::copy(planned.begin(), planned.end(), rule.begin());
  if (p.cfg.scenario.financing.block_special_shorts)
    block_special_plan(p.tiers, p.no_locate, current, planned, nav_post, day);
  return co::Ok();
}
// A working order an earlier decision placed and this one keeps (unchanged plan on
// a non-rebalance day) must not open or grow a short once the name is special:
// clamp it to min(held, 0); nothing is left to trade when that is the holding. A
// kept delta order first becomes the target order it currently amounts to (its target
// plus the drift since its decision), so a special name only ever holds target orders
// and later drift cannot carry a fill below the floor.
void clamp_kept_order(NameState& s, usize i, NavReplayDay& day) {
  if (delta_order(s, i)) {
    s.order[i] += s.held[i] - s.anchor[i];
    s.delta[i] = 0;
  }
  const f64 floor = std::min(s.held[i], 0.0);
  if (!(s.order[i] < floor)) return;
  day.blocked_short_dollars += floor - s.order[i]; ++day.blocked_short_names;
  s.order[i] = floor;
  if (floor == s.held[i]) s.active[i] = 0;
}
// rate per-name-v1: this book's rate of every name at decision d into cache.rate,
// per_name_rate_v1 at the book's pre-trade NAV on the session's cached liquidity
// (rate_min for a nonmember, which the rule never moves), one sample per member.
void per_name_rates(const Ctx& c, Book& b, usize d, LiquidityCache& cache) {
  const auto& x = c.x; const auto& cfg = c.cfg; const usize n = x.instruments;
  for (usize i = 0; i < n; ++i) {
    if (!x.member[d * n + i]) { cache.rate[i] = cfg.rate_min; continue; }
    const f64 theta = per_name_rate_v1(cfg.rate_rra, cfg.rate_lambda, b.nav_pre, cache.sigma[i],
                                       cache.adv[i], cfg.rate_min, cfg.rate_max);
    cache.rate[i] = theta; b.rates.add(theta);
  }
}
// Delta basis: a nonzero plan on a name the locate rule does not guard becomes a delta
// order anchored at the decision's holding; a zero plan (an exit must end flat) and a
// special-tier name under the locate rule keep target orders.
void anchor_order(NameState& s, usize i, bool locate_guarded) {
  const bool delta = s.planned[i] != 0 && !locate_guarded;
  s.delta[i] = delta ? u8{1} : u8{0};
  if (delta) s.anchor[i] = s.held[i];
}
// DECIDE at d: current weights are marked dollars / post-trade NAV (stale names at
// stale marks); the unchanged target rules plan the next weights toward the shared
// desired target (`rebalance` is effective: cadence day and not skipped), then the
// scenario's locate rule; every changed name gets a working order in decision-NAV
// dollars (zero exits included; a target, or under the delta basis a delta, order).
// On rebalance days unchanged members, and on every day flat nonmembers, cancel.
// Planned turnover, gross/net and the v2 budget are the rule's plan before the locate
// rule (blocked dollars are reported apart). With rate per-name-v1 the aim-partial-v5
// move uses this book's per-name rates.
co::Status plan_decision(const Ctx& c, Book& b, const Construction& shared,
                         const BorrowTiers& tiers, LiquidityCache& cache, usize d,
                         bool rebalance, const ConstructionDay& construction,
                         NavReplayDay& day) {
  const auto& x = c.x; auto& s = b.names; const usize n = x.instruments;
  if (day.calendar_month != b.month) { b.month = day.calendar_month; b.spent = 0; }
  for (usize i = 0; i < n; ++i) s.current[i] = s.held[i] / b.nav_post;
  TargetReplayDay plan;
  plan.decision = d; plan.session = x.session_keys[d]; plan.calendar_month = day.calendar_month;
  std::span<const f64> rates;
  if (c.cfg.rate == NavRateRule::PerNameV1) { per_name_rates(c, b, d, cache); rates = cache.rate; }
  const PlanInputs inputs{x, c.cfg, d, rebalance, shared.desired, tiers, {}};
  const auto rule = b.trace ? std::span<f64>(b.trace->rule) : std::span<f64>{};
  ATX_TRY_VOID(plan_weights(inputs, b.spent, rates, s.current, s.planned, b.nav_post, rule, plan,
                            day));
  b.spent += plan.turnover;
  const bool blocking = c.cfg.scenario.financing.block_special_shorts;
  const bool delta_basis = c.cfg.order_basis == NavOrderBasis::Delta;
  for (usize i = 0; i < n; ++i) {
    if (s.planned[i] != s.current[i]) {
      s.order[i] = s.planned[i] * b.nav_post; s.active[i] = 1;
      if (delta_basis) anchor_order(s, i, blocking && tiers.tier[i] == tier_special);
    } else if (rebalance || !x.member[d * n + i]) {
      s.active[i] = 0;
    } else if (blocking && s.active[i] && tiers.tier[i] == tier_special) {
      clamp_kept_order(s, i, day);
    }
  }
  day.decision = true; day.rebalance = rebalance;
  day.planned_turnover = plan.turnover; day.planned_forced = plan.forced_turnover;
  day.planned_discretionary = plan.discretionary_turnover;
  day.planned_gross = plan.gross; day.planned_net = plan.net;
  day.planned_held_names = plan.held_names; day.decision_members = detail::members_at(x, d);
  day.applied_fraction = plan.applied_fraction; day.month_planned = b.spent;
  if (c.cfg.target.rule == TargetReplayRule::MonthlyTargetBudgetV2)
    day.budget_excess = std::max(0.0, b.spent - c.cfg.target.monthly_budget);
  day.construction = construction; // shared record; the band acts on this book's weights
  day.construction.banded_names = plan.construction.banded_names;
  return co::Ok();
}
f64 gross_dollars(const NameState& s) {
  f64 gross = 0;
  for (const f64 h : s.held) gross += std::abs(h);
  return gross;
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
// Bitwise nonzero: -0.0 counts, so a reported row set is exact (a plan of -0 is state).
bool nonzero_bits(f64 x) { return std::bit_cast<u64>(x) != 0; }
// --emit-holdings: the observed book's names with any state at the end of session t
// (a decision or execution session), in index order, to the sink. Reads only.
co::Status emit_holdings(const TargetReplayInput& x, const Book& b, const Construction& shared,
                         const BorrowTiers& tiers, const NavReplayDay& day, usize t,
                         std::vector<NavHolding>& rows, NavHoldingsSink& sink) {
  const auto& s = b.names; const auto& trace = *b.trace; const usize n = x.instruments;
  rows.clear();
  for (usize i = 0; i < n; ++i) {
    const auto fill = static_cast<NavFillStatus>(trace.fill[i]);
    const bool planned = day.decision && nonzero_bits(s.planned[i]);
    if (!nonzero_bits(s.held[i]) && !planned && !s.active[i] && fill == NavFillStatus::None)
      continue;
    NavHolding h;
    h.index = i; h.instrument_id = x.instrument_ids[i];
    h.member = x.member[t * n + i] != 0; h.stale = s.held[i] != 0 && s.absent[i] != 0;
    if (tiers.active) { h.tier = tiers.tier[i]; h.tier_missing = tiers.missing[i]; }
    h.held_dollars = s.held[i]; h.held_weight = s.held[i] / b.nav_post;
    h.fill = fill; h.filled_dollars = trace.filled[i]; h.fill_cost_dollars = trace.cost[i];
    h.unfilled_dollars = trace.unfilled[i];
    h.desired = day.rebalance ? shared.desired[i] : nan;
    h.rule_weight = day.decision ? trace.rule[i] : nan;
    h.target_weight = day.decision ? s.planned[i] : nan;
    h.order_placed = day.decision && s.planned[i] != s.current[i];
    h.locate_blocked = day.decision && s.planned[i] != trace.rule[i];
    h.order_working = s.active[i] != 0;
    h.order_dollars = s.active[i] ? s.order[i] : nan;
    rows.push_back(h);
  }
  return sink.session(day, rows);
}
// The --emit-holdings observer of run_books: book `book` reported to `sink`.
struct Observer {
  NavHoldingsSink* sink{};
  usize book{};
};
// A cadence decision: (t - begin) % cadence == 0, extended before begin (warm start) so
// the rebalance calendar of the scored rows never depends on the warm-up length.
bool cadence_day(usize t, usize begin, usize cadence) {
  return t >= begin ? (t - begin) % cadence == 0 : (begin - t) % cadence == 0;
}
// A fresh row t (every other field is filled by the session's MARK/EXECUTE/DECIDE).
NavReplayDay open_day(const TargetReplayInput& x, usize t, bool return_observation) {
  NavReplayDay day;
  day.session_index = t; day.session = x.session_keys[t];
  day.calendar_month = detail::calendar_month(day.session);
  day.return_observation = return_observation;
  return day;
}
// Warm start: the scoring boundary, after the warm-up MARK of row decision_begin. The book
// is resized to initial_nav (one factor on every dollar state: holdings, cash, working
// orders, delta anchors and written-off exposures; weights and marks are unchanged) and
// every reported accumulator restarts, so the result holds scored quantities only.
// deployment_index keeps a warm-up deployment (it then precedes the first row).
void start_scoring(const Ctx& c, Book& b) {
  const f64 scale = c.cfg.initial_nav / b.nav_pre;
  auto& s = b.names;
  for (auto* dollars : {&s.held, &s.order, &s.anchor, &s.written_exposure})
    for (f64& v : *dollars) v *= scale;
  b.cash *= scale; b.nav_pre = c.cfg.initial_nav;
  auto& r = b.result;
  r.events.clear();
  for (auto* bucket : {&r.gap_run_1, &r.gap_run_2_4, &r.gap_run_5_plus, &r.written_off,
                       &r.reappeared, &r.unresolved, &r.guarded})
    *bucket = NavBucket{};
  r.guard_sensitivity = 0; r.max_return_identity_error = 0; r.max_cash_book_error = 0;
  b.participation = ParticipationHistogram{};
  b.rates = RateStatistics{}; b.rates.reset(c.cfg.rate_min, c.cfg.rate_max);
}
// Sessions [begin - K, end) (K = warm_start_sessions; 0 without a warm start): MARK
// (t > begin - K) -> EXECUTE (begin - K < t <= end-2) -> DECIDE (t < end-2); rows t >=
// begin are reported. Row t reads data at rows <= t only. The scenario books run in
// lockstep: each book performs exactly its own single-book sequence (mark, execute,
// decide, close) on its own state; only the decision's desired target and borrow
// tiers (and, with rate per-name-v1, the session's liquidity windows) are formed once
// and shared, so every book is bit-identical to a replay on its own. An observer only
// reads the observed book (its trace is written beside, never read by, the arithmetic).
co::Result<std::vector<NavReplayResult>> run_books(const TargetReplayInput& x,
                                                   const NavFinancingFields& fields,
                                                   std::span<const Ctx> ctxs,
                                                   const Observer& observer = {}) {
  const usize begin = x.decision_begin, end = x.decision_end, count = ctxs.size();
  const auto& target = ctxs.front().cfg.target; // identical for every book
  // The rate, order basis, locate-in-aim, cache and warm start (like the target) are the
  // base config's, identical for every book.
  const auto& base = ctxs.front().cfg;
  const usize warm = base.warm_start_sessions, start = begin - warm; // validated: warm <= begin
  // A flat start's row begin+1 marks an empty book; a warm book earns from row begin+1.
  const usize first_return = begin + (warm ? 1U : 2U);
  const bool delta_basis = base.order_basis == NavOrderBasis::Delta;
  std::vector<std::unique_ptr<Book>> books;
  books.reserve(count);
  for (const auto& c : ctxs) {
    auto& b = *books.emplace_back(std::make_unique<Book>(x.instruments, delta_basis));
    b.cash = b.nav_pre = b.nav_post = c.cfg.initial_nav;
    b.result.deployment_index = end; b.result.days.reserve(end - begin);
    b.rates.reset(c.cfg.rate_min, c.cfg.rate_max);
  }
  Construction shared(x.instruments, base.locate_in_aim);
  // Tiers of the latest decision: MARK t reads those of decision t-1 (or earlier).
  BorrowTiers tiers(x.instruments, !fields.shares_out.empty());
  // per-name-v1 caches its decision windows too; --liquidity-cache only execution's.
  const bool per_name = base.rate == NavRateRule::PerNameV1;
  LiquidityCache cache(x.instruments, liquidity_cached(base), per_name);
  std::vector<NavHolding> holdings;
  if (observer.sink) {
    books[observer.book]->trace = std::make_unique<Trace>(x.instruments);
    holdings.reserve(x.instruments);
  }
  std::vector<NavReplayDay> days(count);
  for (usize t = start; t < end; ++t) {
    const bool execution = t > start && t + 2 <= end, decision = t + 2 < end;
    const bool scored = t >= begin, boundary = warm && t == begin;
    const bool rate_decision = per_name && decision;
    if (cache.on() && (execution || rate_decision))
      fill_liquidity(ctxs.front(), books, t, execution, rate_decision, cache);
    for (usize k = 0; k < count; ++k) {
      const auto& c = ctxs[k]; auto& b = *books[k]; auto& day = days[k];
      auto& r = b.result;
      if (b.trace) b.trace->clear();
      day = open_day(x, t, t >= first_return);
      if (t > start) ATX_TRY_VOID(mark_session(c, b, tiers, t, day));
      if (boundary) { // the warm-up MARK is not a scored row: the base row starts here
        start_scoring(c, b);
        day = open_day(x, t, false);
      }
      day.pretrade_nav = b.nav_pre;
      day.pretrade_gross_dollars = gross_dollars(b.names);
      if (execution) {
        ATX_TRY_VOID(execute_orders(c, b, t, cache, day));
        if (r.deployment_index == end && day.traded_dollars > 0) r.deployment_index = t;
        day.one_way_turnover_gmv = day.pretrade_gross_dollars > 0
            ? day.traded_dollars / day.pretrade_gross_dollars : nan;
      } else {
        b.nav_post = b.nav_pre; b.pending_cost = 0;
      }
    }
    if (decision) {
      TierCensus census;
      ConstructionDay construction;
      const bool cadence = cadence_day(t, begin, target.cadence);
      ATX_TRY(const bool rebalance, decide_construction(x, fields, target, t, cadence, {},
                                                        shared, tiers, census, construction));
      for (usize k = 0; k < count; ++k) {
        ATX_TRY_VOID(plan_decision(ctxs[k], *books[k], shared, tiers, cache, t, rebalance,
                                   construction, days[k]));
        days[k].member_tiers = census.members;
        days[k].member_missing_predictors = census.missing;
      }
    }
    for (usize k = 0; k < count; ++k) {
      auto& b = *books[k];
      ATX_TRY_VOID(close_day(ctxs[k], b, days[k]));
      if (t + 1 == end) ATX_TRY_VOID(report_unresolved(ctxs[k], b, t));
      if (!scored) continue; // warm-up rows are never reported
      if (b.trace && (decision || execution))
        ATX_TRY_VOID(emit_holdings(x, b, shared, tiers, days[k], t, holdings, *observer.sink));
      b.result.days.push_back(days[k]);
    }
  }
  std::vector<NavReplayResult> results;
  results.reserve(count);
  for (auto& book : books) {
    auto& b = *book; auto& r = b.result;
    r.participation_fills = b.participation.total();
    r.participation_p95 = b.participation.upper_quantile(.95);
    r.participation_max = b.participation.maximum();
    if (per_name) r.construction.rate_stats = b.rates.stats();
    results.push_back(std::move(r));
  }
  return co::Ok(std::move(results));
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
  s.long_financing_dollars += d.long_financing_dollars;
  for (usize k = 0; k < s.short_financing_by_tier.size(); ++k) {
    s.short_financing_by_tier[k] += d.short_financing_by_tier[k];
    s.member_tier_days[k] += d.member_tiers[k];
  }
  s.missing_predictor_short_name_days += d.missing_predictor_shorts;
  s.missing_predictor_member_days += d.member_missing_predictors;
  s.blocked_short_name_decisions += d.blocked_short_names;
  s.blocked_short_dollars += d.blocked_short_dollars;
}
// Each tier's share of pre-mark short dollars on a return row with shorts.
struct TierShares {
  std::array<std::vector<f64>, 3> share;
  void add(const NavReplayDay& d) {
    const f64 total = d.short_dollars_by_tier[0] + d.short_dollars_by_tier[1] +
                      d.short_dollars_by_tier[2];
    if (!(total > 0)) return;
    for (usize k = 0; k < share.size(); ++k) share[k].push_back(d.short_dollars_by_tier[k] / total);
  }
  void summarize(NavSummary& s) {
    s.short_share_sessions = share[0].size();
    for (usize k = 0; k < share.size(); ++k) {
      auto& v = share[k];
      f64 sum = 0;
      for (const f64 x : v) sum += x;
      std::sort(v.begin(), v.end());
      s.short_share_mean[k] = v.empty() ? nan : sum / static_cast<f64>(v.size());
      s.short_share_p95[k] = detail::sorted_quantile(v, .95);
    }
  }
};
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
constexpr const char* timing_declaration =
    "decide at session d after its mark (modeled +22h mark, +23h decision); fill at the close "
    "of d+1; first return row d+2; decisions [begin,end-2), executions <= end-2, return rows "
    "[begin+2,end) (last two scored decisions dropped vs target replay); cadence phase relative "
    "to begin; final session valuation only, no liquidation";
constexpr const char* accounting_declaration =
    "self-financing marked-dollar book; per session MARK->EXECUTE->DECIDE; cash 0%, no short "
    "rebate, short proceeds stay in cash (excess-return accounting: the swap benchmark "
    "cancels against NAV collateral); fill costs debited to cash at the fill and land in "
    "the next return row; financing on pre-mark dollars x calendar days / the scenario day "
    "count (scenarios[].financing): borrow = the short leg, long_financing = the long leg; "
    "r_t = NAVpre_t/NAVpre_{t-1}-1 = gross(drift+write-off) - trade_cost(t-1) - borrow - "
    "long_financing, checked; cash+holdings reconciled to NAV every session";
constexpr const char* financing_declaration =
    "flat-short-v0: flat_short_bps on every pre-mark short dollar x calendar days/365 (the "
    "legacy accrual); tiered-swap-v1 (prime-broker portfolio swap, benchmark cancelled): "
    "long_spread_bps on every pre-mark long dollar + (short_spread_bps + tier fee) on every "
    "pre-mark short dollar x calendar days/day_count, the fee from the name's borrow tier at "
    "the latest decision <= t-1 (a short that migrates into special pays special until it "
    "exits); block_special_shorts: at DECIDE, after the rule (band/partial/budget), a "
    "special-tier name's next weight = max(next, min(current, 0)) (no new or increased "
    "short; reductions pass), a kept working order is clamped likewise; blocked short "
    "dollars reported; the rule's planned turnover/budget are before the block";
constexpr const char* borrow_tier_declaration =
    "engine estimate_borrow_tier public-predictor-prior-v1 with its default thresholds "
    "(market cap < 1e9 USD, raw price < 5 USD, SI ratio >= 0.10, IPO age < 365 calendar days; "
    "0 flags GC, 1 warm, 2+ special), computed once per decision d for every name from rows "
    "<= d and shared by every scenario (fees are per scenario): market cap = shares_out[d] x "
    "raw_close[d] (mktcap_lagged not used: its presence is not point in time); raw price = "
    "raw_close[d]; SI ratio = si_shares[d] / shares_out[d] (shares_out >= float: understated); "
    "IPO age = calendar days since the first present role session (present at role session "
    "0: seasoned); shares_out outside [1e5, 5e10], non-finite or negative si_shares, an "
    "absent name or an engine Unavailable -> warm, counted as missing; available_at = "
    "session + 22h (fields visibility mark), decision = session + 23h";
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
    "declared unfitted costs (constant 5 bps half spread, impact Y=0.6); financing is a "
    "declared scenario (flat-300-v0 legacy; swap-fin-v1 tier fees are research priors from "
    "public predictors, not lender quotes, and its locate rule is a modeled no-special-shorts "
    "rule, not locate availability; without --fields only flat-300-v0 runs, no locate); "
    "vendor adjusted-close factor unverified (guard sensitivity reported); "
    "common-stock status unverified; K-session write-off may misclassify halts vs delistings "
    "(missing histogram reported); cash earns 0% and shorts no rebate, so returns approximate "
    "excess returns; v2 budget planned by decision month while actual turnover is by execution "
    "month; working orders keep decision-NAV dollar targets; no terminal liquidation; the last "
    "two scored decisions are dropped vs the target replay; not capacity qualified; the "
    "monthly turnover fields (months_le_0.30, meets_turnover_target_*) are LEGACY reporting "
    "only (30%/month target retired 2026-09-27), the declared turnover limits are the daily "
    "GMV mean/p95 ceilings (daily_turnover_gmv, meets_daily_turnover_*); a capped (S2/S3) "
    "deployment that completes over several sessions counts its later sessions as turnover";
// The per-name aim_partial text is head + detail::nonmember_exit_clause + tail ("nonmembers
// exit to 0" at exit_rate 1: the declaration byte for byte as before the split).
constexpr const char* per_name_rate_head =
    "rebalance decision: each member moves next = current + theta_i * (aim_leverage * desired "
    "- current) unless |aim_leverage * desired - current| <= dust_multiple / N_d (N_d = "
    "members at d; 0 = off), which keeps its weight and is counted in banded_names; "
    "non-rebalance decisions keep member weights; ";
constexpr const char* per_name_rate_tail =
    "; rate per-name-v1: "
    "theta_i = clip(sqrt(rate_rra * sigma_i^2 * ADV_i / (rate_lambda * NAV_d)), rate_min, "
    "rate_max), NAV_d the book's pre-trade NAV at d, sigma_i and ADV_i the name's liquidity "
    "row of session d (window [d-w, d), the liquidity declaration); ADV <= 0, sigma <= 0 or "
    "fewer than min_vol_pairs pairs -> rate_min; trade_fraction (theta) unused; "
    "applied_fraction = the members' mean rate; monthly_budget unused; "
    "construction.v5.rate_stats: one sample per member per decision, quantiles exact at the "
    "clip bounds, else the upper edge of a 4096-bin histogram of [rate_min, rate_max]";
constexpr const char* order_basis_declaration =
    "delta (v6 prereg C1): at DECIDE a name whose plan changes to a NONZERO weight gets a delta "
    "order: decision-NAV dollars (planned - current) x NAVpost = planned x NAVpost - held_d; "
    "at EXECUTE t it requests that delta minus the dollars already filled on it (never "
    "drift-adjusted), so price drift between decision and fill rides and the next DECIDE "
    "re-plans from the drifted holding at theta; a complete fill lands on planned x NAVpost "
    "plus the drift since the decision (exactly the target without drift); a capped residual "
    "keeps its remaining delta until filled, replaced by a decision that changes the name's "
    "plan, or cancelled (a rebalance with an unchanged plan, a flat nonmember, a write-off); "
    "a zero plan (every exit) and, under block_special_shorts, any order on a special-tier "
    "name are target orders (a kept delta order on a name that turns special becomes the "
    "target order it amounts to, then is clamped); target (default): the order is the "
    "decision-NAV dollar target and EXECUTE requests target - held";
constexpr const char* locate_in_aim_declaration =
    "v6 prereg C3: at each rebalance decision, after the tied-rank target and before the "
    "neutralization, a member in the special borrow tier of that decision (borrow_tiers, "
    "rows <= d, shared by every book) with a negative desired weight is set to 0; the "
    "neutralization then regresses the zeroed target (net and beta re-balanced over its "
    "used rows) and rescales it to the zeroed target's gross; applies to every book of the "
    "run (the construction is shared); each book's block_special_shorts rule still applies "
    "after the target rule as a safety net; requires the borrow fields and a neutralizing "
    "construction";
constexpr const char* warm_start_declaration =
    "v8 D-0 (review C-7): every book decides and trades from role row score_begin - K (K = "
    "warm_start_sessions <= score_begin: never before the role's first row) under every rule, "
    "cost and financing of this recipe; the MARK of session score_begin closes the warm-up "
    "and is not reported; each book is then resized to initial_nav (holdings, cash, working "
    "orders, delta anchors and written-off exposures scaled by one factor; weights "
    "unchanged) and every reported quantity restarts: rows, events, missing/guard buckets, "
    "participation, rate statistics and accounting checks cover sessions >= score_begin "
    "only; row score_begin is the base row (its fills and decision are scored), return rows "
    "are [score_begin+1, end); a deployment in the warm-up is not a scored deployment; the "
    "cadence phase stays relative to score_begin";

// Output label of a book: the trading id alone without fields (the legacy names),
// "<trading>+<financing>" in the financing matrix.
std::string scenario_label(const NavScenario& s, bool tiered) {
  return tiered ? s.id + "+" + s.financing.id : s.id;
}
Json financing_json(const NavFinancing& f) {
  const bool flat = f.rule == NavFinancingRule::FlatShortV0;
  return Json{{"id", f.id}, {"rule", flat ? "flat-short-v0" : "tiered-swap-v1"},
      {"flat_short_bps", f.flat_short_bps}, {"long_spread_bps", f.long_spread_bps},
      {"short_spread_bps", f.short_spread_bps},
      {"tier_fee_bps", {{"gc", f.gc_bps}, {"warm", f.warm_bps}, {"special", f.special_bps}}},
      {"day_count", "ACT/" + std::to_string(f.day_count)},
      {"block_special_shorts", f.block_special_shorts}};
}
Json scenario_recipe(const NavScenario& s, bool primary, bool tiered) {
  const Json participation = std::isfinite(s.max_participation) ? Json(s.max_participation)
                                                                : Json("uncapped");
  return Json{{"id", scenario_label(s, tiered)}, {"trading_scenario", s.id},
      {"financing_scenario", s.financing.id}, {"primary", primary},
      {"cost_rule", s.cost == NavCostRule::FlatBpsV1 ? "flat-bps-v1" : "sqrt-impact-v1"},
      {"flat_bps", s.flat_bps}, {"half_spread_bps", s.half_spread_bps},
      {"commission_bps", s.commission_bps}, {"impact_y", s.impact_y},
      {"impact_delta", s.impact_delta}, {"max_participation", participation},
      {"financing", financing_json(s.financing)},
      {"fallback_daily_vol", s.fallback_daily_vol},
      {"stale_exit_sessions", s.stale_exit_sessions},
      {"terminal_haircut_long", s.adverse_terminal ? nav_adverse_long_return : 0.0},
      {"terminal_haircut_short", s.adverse_terminal ? nav_adverse_short_return : 0.0}};
}
// fields: the pinned borrow-field binding, or null without --fields.
Json nav_recipe(const TargetReplayRunConfig& cfg, const NavReplayConfig& base,
                const std::vector<NavScenario>& scenarios, const NavTurnoverLimits& limits,
                const Json& fields) {
  const bool tiered = !fields.is_null();
  Json list = Json::array();
  for (usize k = 0; k < scenarios.size(); ++k)
    list.push_back(scenario_recipe(scenarios[k], k == nav_primary_scenario_index, tiered));
  auto j = Json{{"schema", "atx.dsl-nav-replay/v1"},
      {"combined_sha256", cfg.combined_sha256}, {"role_sha256", cfg.role_sha256},
      {"rule", detail::construction_rule_id(cfg.target)}, {"cadence", cfg.target.cadence},
      {"trade_fraction", cfg.target.trade_fraction},
      {"monthly_budget", cfg.target.monthly_budget},
      {"initial_nav", base.initial_nav}, {"liquidity_window", base.liquidity_window},
      {"min_vol_pairs", base.min_vol_pairs}, {"max_events", base.max_events},
      {"max_working_bytes", cfg.target.max_working_bytes}, {"scenarios", std::move(list)},
      {"primary_scenario", scenario_label(scenarios[nav_primary_scenario_index], tiered)},
      {"sharpe_target", nav_sharpe_target},
      {"monthly_turnover_target", nav_monthly_turnover_target},
      {"timing", timing_declaration}, {"accounting", accounting_declaration},
      {"target", target_declaration}, {"turnover", turnover_definition},
      {"missing", missing_declaration}, {"guard", guard_declaration},
      {"liquidity", liquidity_declaration}, {"desired_target_postprocess", "none"},
      {"cost_input_status", tiered ? "declared-unfitted-scenario-modeled-locate-rule"
                                   : "declared-unfitted-scenario-no-locate"},
      {"monthly_turnover_target_status", "legacy-reporting-only;retired-2026-09-27"},
      {"daily_turnover_mean_max", limits.daily_mean_max},
      {"daily_turnover_p95_max", limits.daily_p95_max},
      {"daily_turnover_definition", daily_turnover_definition},
      {"financing", financing_declaration}, {"primary_financing_available", tiered},
      {"financing_fields", fields}};
  if (tiered) j["borrow_tiers"] = borrow_tier_declaration;
  // Construction keys only when non-default (overwrites desired_target_postprocess
  // when neutralizing), so the default recipe carries no construction entries.
  const auto construction = detail::construction_recipe_json(cfg.target);
  if (!construction.empty()) j.update(Json::parse(construction));
  // rate per-name-v1 (aim-partial-v5 only) replaces the fixed rate's keys; a fixed
  // rate adds nothing, so its recipe is the T30 recipe byte for byte.
  if (base.rate == NavRateRule::PerNameV1) {
    j["rate"] = "per-name-v1";
    j["rate_rra"] = base.rate_rra; j["rate_lambda"] = base.rate_lambda;
    j["rate_min"] = base.rate_min; j["rate_max"] = base.rate_max;
    j["aim_partial"] = std::string(per_name_rate_head) +
                       detail::nonmember_exit_clause(cfg.target) + per_name_rate_tail;
  }
  // v6 execution options: keys only when non-default, so the default recipe is the v5
  // recipe byte for byte. The liquidity cache changes no output and records nothing.
  if (base.order_basis == NavOrderBasis::Delta) {
    j["order_basis"] = "delta"; j["order_basis_rule"] = order_basis_declaration;
  }
  if (base.locate_in_aim) {
    j["locate_in_aim"] = true; j["locate_in_aim_rule"] = locate_in_aim_declaration;
  }
  // v8 warm start: keys only when on (the flat-start recipe is unchanged byte for byte).
  if (base.warm_start_sessions) {
    j["warm_start_sessions"] = base.warm_start_sessions;
    j["warm_start_rule"] = warm_start_declaration;
  }
  v7::extend_recipe(j); // L4 hook: identity unless extended or a reserved cost-v2 id
  return j;
}
Json rate_stats_json(const NavRateStats& s) {
  return Json{{"n", s.n}, {"mean", finite_or_null(s.mean)}, {"min", finite_or_null(s.min)},
      {"max", finite_or_null(s.max)}, {"p05", finite_or_null(s.p05)},
      {"p50", finite_or_null(s.p50)}, {"p95", finite_or_null(s.p95)},
      {"at_min_count", s.at_min_count}, {"at_max_count", s.at_max_count},
      {"share_at_min", finite_or_null(s.share_at_min)},
      {"share_at_max", finite_or_null(s.share_at_max)}};
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
Json tier_json(const std::array<f64, 3>& v) {
  return Json{{"gc", finite_or_null(v[0])}, {"warm", finite_or_null(v[1])},
              {"special", finite_or_null(v[2])}};
}
Json financing_summary(const NavScenario& sc, const NavSummary& s, bool tiered) {
  const f64 short_leg = s.borrow_dollars;
  return Json{{"spec", financing_json(sc.financing)}, {"tiers_available", tiered},
      {"long_financing_dollars", s.long_financing_dollars},
      {"short_financing_dollars", short_leg},
      {"total_financing_dollars", s.long_financing_dollars + short_leg},
      {"short_financing_by_tier_dollars", tier_json(s.short_financing_by_tier)},
      {"summed_long_financing_return", s.summed_long_financing_return},
      {"summed_short_financing_return", s.summed_borrow_return},
      {"short_dollar_share_by_tier", {{"basis", "pre-mark short dollars on return rows with "
                                                "shorts; NaN/null without tiers"},
          {"sessions", s.short_share_sessions}, {"mean", tier_json(s.short_share_mean)},
          {"p95", tier_json(s.short_share_p95)}}},
      {"missing_predictor_short_name_days", s.missing_predictor_short_name_days},
      {"missing_predictor_member_decisions", s.missing_predictor_member_days},
      {"member_decisions_by_tier", {{"gc", s.member_tier_days[0]},
          {"warm", s.member_tier_days[1]}, {"special", s.member_tier_days[2]}}},
      {"blocked_short_dollars", s.blocked_short_dollars},
      {"blocked_short_name_decisions", s.blocked_short_name_decisions},
      {"net_exposure", {{"mean_net_leverage", s.mean_net_leverage},
          {"mean_abs_net_leverage", s.mean_abs_net_leverage},
          {"max_abs_net_leverage", s.max_abs_net_leverage}}}};
}
Json scenario_summary(const NavScenario& sc, bool primary, bool tiered, const NavReplayResult& r,
                      const NavSummary& s, const std::string& daily_sha,
                      const std::string& events_sha) {
  Json years = Json::array();
  for (const auto& y : s.years)
    years.push_back({{"year", y.year}, {"observations", y.observations},
                     {"net_compounded_return", y.net_return}});
  Json j{{"scenario", scenario_label(sc, tiered)}, {"trading_scenario", sc.id},
      {"financing_scenario", sc.financing.id}, {"primary", primary},
      {"observations", s.observations},
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
  j["daily_turnover_gmv"] = Json{{"definition", daily_turnover_definition},
      {"basis", "executed-fills;forced-exits-included;deployment-session-excluded"},
      {"sessions", s.daily_turnover_sessions},
      {"zero_gmv_sessions_excluded", s.daily_turnover_zero_gmv_sessions},
      {"mean", finite_or_null(s.daily_turnover_mean)},
      {"median", finite_or_null(s.daily_turnover_median)},
      {"p95", finite_or_null(s.daily_turnover_p95)},
      {"max", finite_or_null(s.daily_turnover_max)},
      {"max_session_ns", s.daily_turnover_sessions ? Json(s.daily_turnover_max_session)
                                                   : Json(nullptr)},
      {"quantile", "linear interpolation at (n-1)q over sorted sessions (numpy default)"},
      {"mean_max", s.daily_limits.daily_mean_max}, {"p95_max", s.daily_limits.daily_p95_max}};
  j["meets_daily_turnover_mean"] = s.meets_daily_turnover_mean;
  j["meets_daily_turnover_p95"] = s.meets_daily_turnover_p95;
  j["financing"] = financing_summary(sc, s, tiered);
  return j;
}

co::Status write_json(const std::filesystem::path& path, const Json& j) {
  std::ofstream file(path, std::ios::binary);
  if (!file) return co::Err(co::ErrorCode::IoError, "nav replay: JSON output");
  file << j.dump(2) << '\n'; file.close();
  return file ? co::Ok() : co::Status(co::Err(co::ErrorCode::IoError, "nav replay: JSON close"));
}
constexpr const char* financing_csv_columns =
    ",long_financing_dollars,long_financing_return,short_gc_dollars,short_warm_dollars,"
    "short_special_dollars,short_financing_gc_dollars,short_financing_warm_dollars,"
    "short_financing_special_dollars,missing_predictor_shorts,missing_predictor_short_dollars,"
    "blocked_short_names,blocked_short_dollars,member_gc,member_warm,member_special,"
    "member_missing_predictors";
void write_financing_csv(std::ostream& file, const NavReplayDay& d) {
  file << ',' << d.long_financing_dollars << ',' << d.long_financing_return;
  for (const f64 v : d.short_dollars_by_tier) file << ',' << v;
  for (const f64 v : d.short_financing_by_tier) file << ',' << v;
  file << ',' << d.missing_predictor_shorts << ',' << d.missing_predictor_short_dollars << ','
       << d.blocked_short_names << ',' << d.blocked_short_dollars;
  for (const usize v : d.member_tiers) file << ',' << v;
  file << ',' << d.member_missing_predictors;
}
// The pre-existing columns come first, unchanged; the GMV turnover columns are
// appended, then the construction columns only when an option is non-default, then
// the financing columns only in the financing matrix (with borrow fields).
co::Status write_daily(const std::filesystem::path& path, const NavReplayResult& r,
                       bool construction, bool financing) {
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
          "stale_short_dollars,guarded_intervals,cash_ratio,pretrade_gross_dollars,"
          "one_way_turnover_gmv";
  if (construction) file << detail::construction_csv_columns();
  if (financing) file << financing_csv_columns;
  file << '\n';
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
         << d.guarded_intervals << ',' << d.cash_ratio << ',' << d.pretrade_gross_dollars << ',';
    if (std::isnan(d.one_way_turnover_gmv)) file << "nan"; else file << d.one_way_turnover_gmv;
    if (construction) detail::write_construction_csv(file, d.construction);
    if (financing) write_financing_csv(file, d);
    file << '\n';
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
bool hash_valid(std::string_view s) {
  return s.size() == 64 && std::all_of(s.begin(), s.end(), [](char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
  });
}
// The whole file, when its SHA-256 equals the external pin and it fits `cap`.
co::Result<Json> pinned_document(const std::string& path, const std::string& pin, u64 cap,
                                 const char* what) {
  const std::string label = std::string("nav replay: ") + what;
  if (!hash_valid(pin)) return co::Err(co::ErrorCode::InvalidArgument, label + " pin");
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file || file.tellg() <= 0 || static_cast<u64>(file.tellg()) > cap)
    return co::Err(co::ErrorCode::InvalidArgument, label + " missing/oversized");
  std::string text(static_cast<usize>(file.tellg()), '\0'); file.seekg(0);
  file.read(text.data(), static_cast<std::streamsize>(text.size()));
  if (!file || file.peek() != std::char_traits<char>::eof())
    return co::Err(co::ErrorCode::IoError, label + " changed extent");
  ATX_TRY(auto digest, co::sha256_hex(text));
  if (digest != pin) return co::Err(co::ErrorCode::InvalidArgument, label + " SHA");
  return co::Ok(Json::parse(text));
}
// Exactly `cells` little-endian f64 whose bytes hash to `sha`.
co::Result<std::vector<f64>> read_field(const std::filesystem::path& path, u64 cells,
                                        const std::string& sha) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file || file.tellg() < 0 || static_cast<u64>(file.tellg()) != cells * sizeof(f64))
    return co::Err(co::ErrorCode::IoError, "nav replay: field payload extent");
  file.seekg(0);
  std::vector<f64> out(static_cast<usize>(cells));
  const auto bytes = std::as_writable_bytes(std::span(out));
  // SAFETY: char accesses the object representation of a trivially copyable f64 array.
  file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  if (!file || file.peek() != std::char_traits<char>::eof())
    return co::Err(co::ErrorCode::IoError, "nav replay: truncated/extended field payload");
  ATX_TRY(auto digest, co::sha256_hex(std::span<const std::byte>(bytes)));
  if (digest != sha)
    return co::Err(co::ErrorCode::InvalidArgument, "nav replay: field payload SHA");
  return co::Ok(std::move(out));
}
// The manifest entry of a field the replay uses: exactly one, declared
// point_in_time true (a missing key refuses), <f8 date-major role geometry, file
// "<name>.f64" (never a path), and a receipt agreeing with the entry SHA.
co::Result<Json> used_field(const Json& manifest, const std::string& name, u64 dates, u64 names) {
  const Json* found = nullptr;
  for (const auto& entry : manifest.at("fields")) {
    if (entry.at("name").get<std::string>() != name) continue;
    if (found)
      return co::Err(co::ErrorCode::InvalidArgument, "nav replay: duplicate field " + name);
    found = &entry;
  }
  if (!found) return co::Err(co::ErrorCode::InvalidArgument, "nav replay: fields lack " + name);
  const auto& e = *found;
  if (!e.contains("point_in_time") || !e.at("point_in_time").is_boolean() ||
      !e.at("point_in_time").get<bool>())
    return co::Err(co::ErrorCode::InvalidArgument,
                   "nav replay: field not declared point in time: " + name);
  const auto sha = e.at("sha256").get<std::string>();
  const auto& receipt = manifest.at("files").at(name + ".f64");
  if (e.at("file").get<std::string>() != name + ".f64" || e.at("dtype") != "<f8" ||
      e.at("layout") != "date-major" || e.at("shape") != Json::array({dates, names}) ||
      !hash_valid(sha) || receipt.at("sha256").get<std::string>() != sha ||
      receipt.at("bytes").get<u64>() != dates * names * sizeof(f64))
    return co::Err(co::ErrorCode::InvalidArgument, "nav replay: field layout/receipt: " + name);
  return co::Ok(e);
}
struct LoadedFields {
  std::vector<f64> shares_out, si_shares;
  std::vector<f64> industry; // industry_group_field; the industry ids only
  Json binding; // recipe record; null when no fields
};
// One used field (used_field checks, payload SHA) and its recipe record in `used`.
co::Result<std::vector<f64>> load_field(const Json& manifest, const std::filesystem::path& base,
                                        const std::string& name, u64 dates, u64 names,
                                        Json& used) {
  ATX_TRY(const auto entry, used_field(manifest, name, dates, names));
  const auto sha = entry.at("sha256").get<std::string>();
  ATX_TRY(auto values, read_field(base / (name + ".f64"), dates * names, sha));
  used[name] = Json{{"sha256", sha}, {"point_in_time", true},
      {"clock", entry.contains("clock") ? entry.at("clock") : Json(nullptr)},
      {"staleness", entry.contains("staleness") ? entry.at("staleness") : Json(nullptr)}};
  return co::Ok(std::move(values));
}
// Pins the atx.research-role-fields/v1 manifest, then its role to the pinned --role
// (manifest, sessions, ids, member SHA against the role manifest's own receipts),
// then loads shares_out and si_shares (and, for the industry ids, grp_ff12) within
// `budget` bytes. Nothing is written.
co::Result<LoadedFields> load_fields(const NavFieldsPin& pin, const TargetReplayRunConfig& cfg,
                                     u64 budget) {
  ATX_TRY(auto m, pinned_document(pin.manifest_path, pin.manifest_sha256,
                                  max_fields_manifest_bytes, "fields manifest"));
  if (m.at("schema") != fields_schema || m.at("status") != "complete")
    return co::Err(co::ErrorCode::InvalidArgument, "nav replay: fields manifest schema/status");
  ATX_TRY(auto role, pinned_document(cfg.role_path, cfg.role_sha256, max_role_manifest_bytes,
                                     "role manifest"));
  const auto& pins = m.at("role"); const auto& receipts = role.at("files");
  if (pins.at("manifest_sha256").get<std::string>() != cfg.role_sha256 ||
      pins.at("sessions_sha256") != receipts.at("sessions.i64").at("sha256") ||
      pins.at("ids_sha256") != receipts.at("ids.u64").at("sha256") ||
      pins.at("member_sha256") != receipts.at("member.u8").at("sha256"))
    return co::Err(co::ErrorCode::InvalidArgument,
                   "nav replay: fields manifest role pins do not match --role");
  const auto dates = role.at("dates").get<u64>(), names = role.at("instruments").get<u64>();
  if (!dates || dates > max_dates || !names || names > max_names)
    return co::Err(co::ErrorCode::InvalidArgument, "nav replay: fields role geometry");
  const u64 cells = dates * names;
  const bool industry = neutralize_by_industry(cfg.target.neutralize);
  const u64 count = financing_field_names.size() + (industry ? 1U : 0U);
  if (cells > budget / (count * sizeof(f64)))
    return co::Err(co::ErrorCode::OutOfRange, "nav replay: fields workspace budget");
  LoadedFields out;
  Json used = Json::object();
  const auto base = std::filesystem::path(pin.manifest_path).parent_path();
  for (const char* field : financing_field_names) {
    const std::string name = field;
    ATX_TRY(auto values, load_field(m, base, name, dates, names, used));
    (name == "shares_out" ? out.shares_out : out.si_shares) = std::move(values);
  }
  if (industry) {
    ATX_TRY(out.industry, load_field(m, base, industry_group_field, dates, names, used));
  }
  out.binding = Json{{"schema", fields_schema},
      {"manifest_sha256", pin.manifest_sha256}, {"role_manifest_sha256", cfg.role_sha256},
      {"fields_used", std::move(used)},
      {"fields_not_used", "mktcap_lagged (presence not point in time): market cap = "
                          "shares_out x raw_close only"}};
  return co::Ok(std::move(out));
}
// The run geometry, from the pinned role manifest alone (no payload is read): the
// loader later requires the blend's dates, instruments and score window to equal it.
struct RoleGeometry {
  usize names{}, sessions{}; // instruments; score_end - score_begin
  usize score_begin{};       // the pre-score history a warm start may use
};
co::Result<RoleGeometry> role_geometry(const TargetReplayRunConfig& cfg) {
  ATX_TRY(auto role, pinned_document(cfg.role_path, cfg.role_sha256, max_role_manifest_bytes,
                                     "role manifest"));
  const auto dates = role.at("dates").get<u64>(), names = role.at("instruments").get<u64>();
  const auto begin = role.at("score_begin").get<u64>(), end = role.at("score_end").get<u64>();
  if (!dates || dates > max_dates || !names || names > max_names || begin >= end || end > dates)
    return co::Err(co::ErrorCode::InvalidArgument, "nav replay: role geometry");
  return co::Ok(RoleGeometry{static_cast<usize>(names), static_cast<usize>(end - begin),
                             static_cast<usize>(begin)});
}
struct AdmittedRun {
  detail::LoadedSavedBlend blend;
  LoadedFields fields;
};
// Admission and load of one pinned run, shared by run_nav_replay and the decide path:
// the workspace reserve of `books` books at the pinned role's own geometry (manifest
// only), then the fields (pinned and role-checked) and the blend charged against what
// remains, so input + all results fit one budget.
co::Result<AdmittedRun> admit_and_load(const TargetReplayRunConfig& cfg,
                                       const NavReplayConfig& base, const NavFieldsPin& fields,
                                       usize books, bool holdings) {
  const bool tiered = !fields.manifest_path.empty();
  ATX_TRY(const auto geometry, role_geometry(cfg));
  // A warm start longer than the pinned role's pre-score history is refused before any
  // payload is read (the replay's own input check repeats it).
  ATX_TRY_VOID(check_warm_start(base.warm_start_sessions, geometry.score_begin));
  const u64 reserve = nav_workspace_reserve_bytes(base, books, tiered, geometry.names,
                                                  geometry.sessions, holdings);
  if (cfg.target.max_working_bytes <= reserve)
    return co::Err(co::ErrorCode::OutOfRange, "nav replay: budget below NAV workspace reserve");
  u64 remaining = cfg.target.max_working_bytes - reserve;
  AdmittedRun out;
  if (tiered) {
    // Pinned and role-checked before the blend is loaded or anything is written.
    ATX_TRY(out.fields, load_fields(fields, cfg, remaining));
    remaining -= (out.fields.shares_out.size() + out.fields.si_shares.size() +
                  out.fields.industry.size()) * sizeof(f64);
  }
  auto load = cfg; load.target.max_working_bytes = remaining;
  ATX_TRY(out.blend, detail::load_saved_blend(load, true));
  // The reserve's geometry is the loaded one (same pinned role manifest).
  if (out.blend.names != geometry.names || out.blend.end - out.blend.begin != geometry.sessions ||
      (tiered && out.fields.shares_out.size() != out.blend.signal.size()))
    return co::Err(co::ErrorCode::InvalidArgument, "nav replay: fields/blend geometry");
  return co::Ok(std::move(out));
}
} // namespace

std::vector<NavFinancing> nav_financing_scenarios() {
  NavFinancing swap;
  swap.id = "swap-fin-v1"; swap.rule = NavFinancingRule::TieredSwapV1; swap.flat_short_bps = 0;
  swap.long_spread_bps = 40; swap.short_spread_bps = 20;
  swap.gc_bps = 30; swap.warm_bps = 100; swap.special_bps = 500;
  swap.day_count = 360; swap.block_special_shorts = true;
  const NavFinancing legacy{}; // flat-300-v0
  NavFinancing engine = swap;
  engine.id = "engine-tiers-v1"; // engine BorrowTierRecipe default fees (asserted by tests)
  engine.gc_bps = 27.5; engine.warm_bps = 300; engine.special_bps = 2750;
  return {swap, legacy, engine};
}

std::vector<NavScenario> fixed_nav_scenarios() {
  NavScenario linear;
  linear.id = "linear-6bps-stale5-v1"; linear.cost = NavCostRule::FlatBpsV1;
  linear.flat_bps = 6; linear.max_participation = inf;
  linear.fallback_daily_vol = 0.05; linear.stale_exit_sessions = 5;
  NavScenario modeled;
  modeled.id = "modeled-1bn-stale5-v1"; modeled.cost = NavCostRule::SqrtImpactV1;
  modeled.half_spread_bps = 5; modeled.commission_bps = 1; modeled.impact_y = 0.6;
  modeled.impact_delta = 0.5; modeled.max_participation = 0.01;
  modeled.fallback_daily_vol = 0.05; modeled.stale_exit_sessions = 5;
  NavScenario adverse = modeled;
  adverse.id = "modeled-1bn-terminal-adverse-v1"; adverse.stale_exit_sessions = 1;
  adverse.adverse_terminal = true;
  return {linear, modeled, adverse};
}

f64 per_name_rate_v1(f64 rra, f64 lambda, f64 nav, f64 daily_vol, f64 adv_dollars,
                     f64 rate_min, f64 rate_max) noexcept {
  const auto usable = [](f64 x) { return std::isfinite(x) && x > 0; };
  if (!usable(rra) || !usable(lambda) || !usable(nav) || !usable(daily_vol) ||
      !usable(adv_dollars))
    return rate_min;
  const f64 theta = std::sqrt(rra * daily_vol * daily_vol * adv_dollars / (lambda * nav));
  // A NaN (inf / inf on overflow) fails the comparison and takes rate_min; +inf clips.
  if (!(theta >= rate_min)) return rate_min;
  return std::min(theta, rate_max);
}

std::vector<NavScenario> nav_scenario_matrix(bool tiered) {
  auto trading = fixed_nav_scenarios();
  if (!tiered) return trading;
  const auto financing = nav_financing_scenarios();
  std::vector<NavScenario> books;
  for (auto s : trading) { s.financing = financing[0]; books.push_back(std::move(s)); }
  for (usize k = 1; k < financing.size(); ++k) {
    auto s = trading[nav_primary_scenario_index]; s.financing = financing[k];
    books.push_back(std::move(s));
  }
  return books;
}

// Conservative bound on everything the NAV path holds beside the loaded input, at the
// run's own geometry (v6 C4; the charge was at max_names x max_dates before): all
// scenario results (days + events at cap), every lockstep book's per-name state, the
// shared construction (with its price-risk scratch when neutralizing), the shared
// borrow tiers, the shared liquidity cache (per-name-v1, or --liquidity-cache at a
// fixed rate: validate_nav_input's predicate) and publication. books <= max_scenarios.
u64 nav_workspace_reserve_bytes(const NavReplayConfig& base, usize books, bool tiered,
                                usize names, usize sessions, bool holdings) {
  const bool cached = liquidity_cached(base);
  return publication_slack_bytes + u64{books} * fixed_workspace_bytes +
         u64{names} * (u64{books} * per_name_bytes + shared_name_bytes +
                       (tiered ? tier_name_bytes : 0) + (cached ? rate_name_bytes : 0) +
                       (holdings ? holdings_name_bytes : 0)) +
         detail::construction_scratch_bytes(base.target, names) +
         u64{books} * (u64{sessions} * sizeof(NavReplayDay) + base.max_events * sizeof(NavEvent));
}

namespace {
co::Result<std::vector<NavReplayResult>> replay_observed(const NavReplayInput& in,
                                                         const NavReplayConfig& base,
                                                         std::span<const NavScenario> scenarios,
                                                         const Observer& observer) {
  if (scenarios.empty() || scenarios.size() > max_scenarios)
    return co::Err(co::ErrorCode::InvalidArgument, "nav replay: 1..8 scenarios per replay");
  if (observer.sink && observer.book >= scenarios.size())
    return co::Err(co::ErrorCode::InvalidArgument, "nav replay: observed book out of range");
  // The NAV volume is authoritative, also for price-risk neutralization.
  TargetReplayInput x = in.target;
  x.volume = in.volume;
  const NavReplayInput input{x, in.volume, in.financing};
  try {
    std::vector<NavReplayConfig> configs(scenarios.size(), base);
    for (usize k = 0; k < scenarios.size(); ++k) {
      configs[k].scenario = scenarios[k];
      ATX_TRY_VOID(validate_nav_config(configs[k]));
      if (scenarios[k].financing.rule == NavFinancingRule::TieredSwapV1 &&
          in.financing.shares_out.empty())
        return co::Err(co::ErrorCode::InvalidArgument,
                       "nav replay: tiered financing requires the borrow fields");
    }
    // The locate-in-aim mask is the decision's special tier: no tiers without fields.
    if (base.locate_in_aim && in.financing.shares_out.empty())
      return co::Err(co::ErrorCode::InvalidArgument,
                     "nav replay: locate-in-aim requires the borrow fields");
    ATX_TRY_VOID(validate_nav_input(input, configs.front(), scenarios.size()));
    std::vector<std::unique_ptr<const bk::ReplayCostModel>> models;
    std::vector<Ctx> ctxs;
    models.reserve(scenarios.size()); ctxs.reserve(scenarios.size());
    for (const auto& cfg : configs) {
      ATX_TRY(auto model, make_cost_model(cfg.scenario));
      models.push_back(std::move(model));
      const auto& s = cfg.scenario; const auto& f = s.financing;
      const bool sqrt_model = s.cost == NavCostRule::SqrtImpactV1;
      Ctx ctx{x, in.volume, cfg, *models.back(),
              s.adverse_terminal ? nav_adverse_long_return : 0.0,
              s.adverse_terminal ? nav_adverse_short_return : 0.0,
              f.flat_short_bps * 1e-4,
              sqrt_model ? (s.half_spread_bps + s.commission_bps) * 1e-4 : 0.0,
              sqrt_model};
      if (f.rule == NavFinancingRule::TieredSwapV1) {
        ctx.long_rate = f.long_spread_bps * 1e-4;
        ctx.short_rate[tier_gc] = (f.short_spread_bps + f.gc_bps) * 1e-4;
        ctx.short_rate[tier_warm] = (f.short_spread_bps + f.warm_bps) * 1e-4;
        ctx.short_rate[tier_special] = (f.short_spread_bps + f.special_bps) * 1e-4;
      }
      ctxs.push_back(ctx);
    }
    return run_books(x, in.financing, ctxs, observer);
  } catch (const std::bad_alloc&) {
    return co::Err(co::ErrorCode::OutOfRange, "nav replay: allocation failed");
  } catch (const std::length_error&) {
    return co::Err(co::ErrorCode::OutOfRange, "nav replay: allocation extent");
  }
}
} // namespace

co::Result<std::vector<NavReplayResult>> replay_nav_scenarios(
    const NavReplayInput& in, const NavReplayConfig& base, std::span<const NavScenario> scenarios) {
  return replay_observed(in, base, scenarios, Observer{});
}

co::Result<std::vector<NavReplayResult>> replay_nav_scenarios(
    const NavReplayInput& in, const NavReplayConfig& base, std::span<const NavScenario> scenarios,
    NavHoldingsSink& sink, usize observed) {
  return replay_observed(in, base, scenarios, Observer{&sink, observed});
}

co::Result<NavReplayResult> replay_nav(const NavReplayInput& in, const NavReplayConfig& cfg) {
  const std::span<const NavScenario> one(&cfg.scenario, 1);
  ATX_TRY(auto results, replay_nav_scenarios(in, cfg, one));
  return co::Ok(std::move(results.front()));
}

co::Result<NavBorrowTiers> nav_borrow_tiers(const NavReplayInput& in, usize d) {
  const auto& x = in.target; const auto& f = in.financing;
  const usize cells = x.dates * x.instruments;
  if (!x.dates || !x.instruments || x.dates > max_dates || x.instruments > max_names ||
      d >= x.dates || x.session_keys.size() != x.dates || x.present.size() != cells ||
      x.member.size() != cells || x.raw_close.size() != cells || f.shares_out.size() != cells ||
      f.si_shares.size() != cells)
    return co::Err(co::ErrorCode::InvalidArgument, "nav borrow tiers: geometry");
  try {
    BorrowTiers tiers(x.instruments, true);
    TierCensus census;
    ATX_TRY_VOID(classify_borrow(x, f, d, tiers, census));
    return co::Ok(NavBorrowTiers{std::move(tiers.tier), std::move(tiers.missing)});
  } catch (const std::bad_alloc&) {
    return co::Err(co::ErrorCode::OutOfRange, "nav borrow tiers: allocation failed");
  }
}

co::Result<NavSummary> summarize_nav(const NavReplayResult& result,
                                     const NavTurnoverLimits& limits) {
  const auto& days = result.days;
  if (days.size() < 3)
    return co::Err(co::ErrorCode::InvalidArgument, "nav summary: fewer than three sessions");
  if (!std::isfinite(limits.daily_mean_max) || !(limits.daily_mean_max > 0) ||
      !std::isfinite(limits.daily_p95_max) || !(limits.daily_p95_max > 0))
    return co::Err(co::ErrorCode::InvalidArgument, "nav summary: daily turnover ceilings");
  try {
    NavSummary s; s.min_cash_ratio = inf; s.daily_limits = limits;
    TierShares shares; f64 net_leverage = 0, abs_net_leverage = 0;
    Moments net, gross; std::vector<f64> series; series.reserve(days.size());
    std::vector<f64> daily_gmv; daily_gmv.reserve(days.size());
    s.daily_turnover_max_session = 0;
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
      if (d.executed && d.session_index != result.deployment_index) {
        if (d.pretrade_gross_dollars > 0) {
          daily_gmv.push_back(d.one_way_turnover_gmv);
          if (daily_gmv.size() == 1 || d.one_way_turnover_gmv > s.daily_turnover_max) {
            s.daily_turnover_max = d.one_way_turnover_gmv;
            s.daily_turnover_max_session = d.session;
          }
        } else {
          ++s.daily_turnover_zero_gmv_sessions;
        }
      }
      accumulate_day(d, s);
      peak = std::max(peak, d.pretrade_nav);
      s.max_drawdown = std::max(s.max_drawdown, 1.0 - d.pretrade_nav / peak);
      if (!d.return_observation || k == 0) continue;
      net.add(d.net_return); gross.add(d.gross_return); series.push_back(d.net_return);
      s.summed_trade_cost_return += d.trade_cost_return;
      s.summed_borrow_return += d.borrow_return; s.summed_writeoff_return += d.writeoff_return;
      s.summed_long_financing_return += d.long_financing_return;
      shares.add(d);
      // Exposure that earned this row's return: the previous session's closing book.
      leverage += days[k - 1].gross_leverage; held += static_cast<f64>(days[k - 1].held_names);
      net_leverage += days[k - 1].net_leverage;
      abs_net_leverage += std::abs(days[k - 1].net_leverage);
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
    s.mean_net_leverage = net_leverage / n; s.mean_abs_net_leverage = abs_net_leverage / n;
    shares.summarize(s);
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
    // Daily GMV turnover (executed fills only), deployment session excluded.
    f64 gmv_sum = 0;
    for (const f64 v : daily_gmv) gmv_sum += v;
    std::sort(daily_gmv.begin(), daily_gmv.end());
    s.daily_turnover_sessions = daily_gmv.size();
    s.daily_turnover_mean = daily_gmv.empty() ? nan : gmv_sum / static_cast<f64>(daily_gmv.size());
    s.daily_turnover_median = detail::sorted_quantile(daily_gmv, .5);
    s.daily_turnover_p95 = detail::sorted_quantile(daily_gmv, .95);
    if (daily_gmv.empty()) s.daily_turnover_max = nan;
    // NaN compares false: an empty or undefined statistic never meets a ceiling.
    s.meets_daily_turnover_mean = s.daily_turnover_mean <= limits.daily_mean_max;
    s.meets_daily_turnover_p95 = s.daily_turnover_p95 <= limits.daily_p95_max;
    return co::Ok(std::move(s));
  } catch (const std::bad_alloc&) {
    return co::Err(co::ErrorCode::OutOfRange, "nav summary: allocation failed");
  }
}

co::Status run_nav_replay(const TargetReplayRunConfig& cfg, std::ostream& progress) {
  return run_nav_replay(cfg, NavTurnoverLimits{}, progress);
}

co::Status run_nav_replay(const TargetReplayRunConfig& cfg, const NavTurnoverLimits& limits,
                          std::ostream& progress) {
  return run_nav_replay(cfg, limits, NavFieldsPin{}, progress);
}

namespace {
// Everything one run publishes, computed before the output directory exists.
struct NavRun {
  const TargetReplayRunConfig& cfg;
  const NavTurnoverLimits& limits;
  const NavReplayConfig& base;
  const std::vector<NavScenario>& scenarios;
  const std::vector<NavReplayResult>& results;
  const std::vector<NavSummary>& summaries;
  const std::string& manifest_json; // the pinned combined manifest
  const Json& fields;               // borrow-field binding; null without --fields
  const Json& warm_start;           // the summary's warm_start record; null without one
};
// The summary's warm_start record; null without a warm start. Called after the replay
// accepted the input (warm_start_sessions <= decision_begin).
Json warm_start_record(const NavReplayConfig& base, const TargetReplayInput& x) {
  const usize k = base.warm_start_sessions;
  if (!k) return Json(nullptr);
  return Json{{"sessions", k},
              {"first_decision_session_ns", x.session_keys[x.decision_begin - k]},
              {"scoring_begins_session_ns", x.session_keys[x.decision_begin]},
              {"rule", "recipe warm_start_rule"}};
}
// construction.v5 of one book over its decision rows: the rule's plan (pre locate
// rule) gross, net and held share of the decision's members.
Json aim_partial_json(const TargetReplayConfig& target, const NavReplayResult& result) {
  std::vector<detail::AimPartialDecision> decisions;
  for (const auto& d : result.days)
    if (d.decision)
      decisions.push_back({d.planned_gross, d.planned_net, d.planned_held_names,
                           d.decision_members});
  return Json::parse(detail::aim_partial_summary_json(target, decisions));
}
co::Result<Json> publish_scenario(const NavRun& run, const std::filesystem::path& dir, usize k,
                                  std::ostream& progress) {
  const bool tiered = !run.fields.is_null();
  const auto& sc = run.scenarios[k]; const auto& result = run.results[k];
  const auto& summary = run.summaries[k];
  const auto label = scenario_label(sc, tiered);
  const auto daily = dir / ("daily_" + label + ".csv");
  const auto events = dir / ("events_" + label + ".csv");
  const bool construction = detail::construction_active(run.cfg.target);
  ATX_TRY_VOID(write_daily(daily, result, construction, tiered));
  ATX_TRY_VOID(write_events(events, result));
  ATX_TRY(auto daily_sha, co::sha256_file(daily.string()));
  ATX_TRY(auto events_sha, co::sha256_file(events.string()));
  auto entry = scenario_summary(sc, k == nav_primary_scenario_index, tiered, result, summary,
                                daily_sha, events_sha);
  if (construction) {
    std::vector<ConstructionDay> decisions;
    for (const auto& d : result.days)
      if (d.decision) decisions.push_back(d.construction);
    entry.update(Json::parse(detail::construction_summary_json(run.cfg.target, decisions)));
    if (run.cfg.target.rule == TargetReplayRule::AimPartialV5) {
      auto v5 = aim_partial_json(run.cfg.target, result);
      if (run.base.rate == NavRateRule::PerNameV1) {
        v5["rate"] = "per-name-v1";
        v5["rate_stats"] = rate_stats_json(result.construction.rate_stats);
      }
      entry["construction"]["v5"] = std::move(v5);
    }
  }
  progress << "nav replay " << label << ": net Sharpe " << std::setprecision(6)
           << summary.net_sharpe << ", mean daily GMV turnover " << summary.daily_turnover_mean
           << " (p95 " << summary.daily_turnover_p95 << "), financing $"
           << summary.long_financing_dollars + summary.borrow_dollars << " (short $"
           << summary.borrow_dollars << "), blocked short $" << summary.blocked_short_dollars
           << ", mean monthly turnover " << summary.mean_monthly_turnover << " (ex deployment "
           << summary.mean_monthly_turnover_ex_deployment << ")\n";
  return co::Ok(std::move(entry));
}
// Exclusive directory: recipe.json, per-scenario CSVs, summary.json LAST.
co::Status publish_nav(const NavRun& run, std::ostream& progress) {
  const auto& cfg = run.cfg;
  const bool tiered = !run.fields.is_null();
  const auto dir = std::filesystem::path(cfg.output_directory);
  if (!std::filesystem::create_directory(dir))
    return co::Err(co::ErrorCode::AlreadyExists, "nav replay: output must not exist");
  const auto method = nav_recipe(cfg, run.base, run.scenarios, run.limits, run.fields);
  ATX_TRY(auto method_sha, co::sha256_hex(method.dump()));
  ATX_TRY_VOID(write_json(dir / "recipe.json", method));
  Json list = Json::array();
  for (usize k = 0; k < run.scenarios.size(); ++k) {
    ATX_TRY(auto entry, publish_scenario(run, dir, k, progress));
    list.push_back(std::move(entry));
  }
  auto bindings = Json::parse(run.manifest_json);
  // Equal-weight blends carry no weights key; pinned-weight blends bind their SHA.
  const auto weights = bindings.contains("composition_weights_sha256")
      ? bindings.at("composition_weights_sha256") : Json(nullptr);
  const auto semantics = bindings.at("signal_semantics");
  Json summary{{"schema", "atx.dsl-nav-replay-summary/v1"}, {"status", "complete"},
      {"recipe_sha256", method_sha}, {"combined_sha256", cfg.combined_sha256},
      {"role_sha256", cfg.role_sha256}, {"rule", detail::construction_rule_id(cfg.target)},
      {"primary_scenario", scenario_label(run.scenarios[nav_primary_scenario_index], tiered)},
      {"primary_financing_available", tiered}, {"financing_fields", run.fields},
      {"signal_semantics", semantics}, {"composition_weights_sha256", weights},
      {"scenarios", std::move(list)}, {"source_bindings", std::move(bindings)},
      {"self_financing_nav", true}, {"capacity_qualified", false},
      {"limitations", limitations_declaration}};
  // v6 execution options, only when non-default (the default summary is unchanged).
  if (run.base.order_basis == NavOrderBasis::Delta) summary["order_basis"] = "delta";
  if (run.base.locate_in_aim) {
    usize zeroed = 0; // the construction is shared: every book carries the same records
    for (const auto& d : run.results.front().days) zeroed += d.construction.locate_zeroed;
    summary["locate_in_aim"] = Json{{"zeroed_special_short_aims", zeroed},
        {"basis", "member-decisions whose negative tied-rank aim was set to 0 (special tier)"}};
  }
  if (!run.warm_start.is_null()) summary["warm_start"] = run.warm_start; // v8, only when on
  v7::extend_summary(summary); // L4 hook: identity unless extended
  return write_json(dir / "summary.json", summary);
}

// ---- --emit-holdings (v7 B3) ----
constexpr const char* holdings_days_columns =
    "session_ns,decision,rebalance,executed,pretrade_nav,posttrade_nav,rows,held_names,"
    "long_dollars,short_dollars,gross_leverage,net_leverage,traded_dollars,fills,capped_fills,"
    "blocked_absent,blocked_liquidity,unfilled_dollars,planned_turnover,planned_gross,"
    "planned_net,orders_placed,blocked_short_names,blocked_short_dollars,neutralize,"
    "neutralize_skipped,locate_zeroed";
constexpr const char* holdings_declaration =
    "primary book; one holdings.csv row per name with any state at the end of each decision "
    "or execution session t (held, the decision's plan, a working order or an EXECUTE outcome "
    "nonzero; -0 counts; an omitted name has every weight 0): held_dollars after EXECUTE at t "
    "(what DECIDE at t reads), held_weight = held_dollars / nav_post (the plan's current "
    "weight, bit for bit); fill/filled_dollars/fill_cost_dollars/unfilled_dollars = EXECUTE at "
    "the close of t (orders placed at earlier decisions); desired_weight = the shared desired "
    "target on effective rebalances (else nan); rule_weight = the target rule's plan and "
    "target_weight = the planned weight after the locate block; planned_trade_weight = "
    "target_weight - held_weight; planned_trade_dollars = order_dollars - held_dollars when "
    "order_placed (else 0); order_dollars = the working order in decision-NAV dollars after "
    "DECIDE (nan when none); decision fields are nan on the final execution-only session; "
    "holdings_days.csv: one row per session, neutralize_skipped = a cadence rebalance the "
    "neutralization guard skipped (ERROR: the book kept its weights)";
bool neutralize_skipped(NeutralizeOutcome outcome) {
  return outcome != NeutralizeOutcome::NotAttempted && outcome != NeutralizeOutcome::Applied;
}
const char* side_label(f64 held) { return held > 0 ? "long" : held < 0 ? "short" : "flat"; }
// Streams the per-name rows (f64: buffered binary rows + a session table, the default;
// csv: the v1 holdings.csv) and holdings_days.csv, one session at a time (O(names) memory
// plus the 1 MiB binary buffer and one table entry per session, within the publication
// slack of the workspace reserve).
class HoldingsEmitter final : public NavHoldingsSink {
public:
  co::Status open(const std::filesystem::path& dir, NavHoldingsFormat format,
                  std::span<const u64> ids) {
    if (!std::filesystem::create_directory(dir))
      return co::Err(co::ErrorCode::AlreadyExists,
                     "nav replay: --emit-holdings directory must not exist");
    dir_ = dir; format_ = format; ids_ = ids;
    days_.open(dir / holdings::days_file, std::ios::binary);
    if (!days_) return co::Err(co::ErrorCode::IoError, "nav replay: holdings output");
    days_.imbue(std::locale::classic()); days_ << std::setprecision(17);
    days_ << holdings_days_columns << '\n';
    if (format_ == NavHoldingsFormat::F64) return binary_.open(dir / holdings::data_file);
    names_.open(dir / "holdings.csv", std::ios::binary);
    if (!names_) return co::Err(co::ErrorCode::IoError, "nav replay: holdings output");
    names_.imbue(std::locale::classic()); names_ << std::setprecision(17);
    names_ << detail::holdings_csv_columns() << '\n';
    return co::Ok();
  }
  co::Status session(const NavReplayDay& d, std::span<const NavHolding> names) override {
    usize placed = 0;
    if (format_ == NavHoldingsFormat::F64) {
      const usize ordinal = table_.size();
      for (const auto& h : names) {
        ATX_TRY_VOID(binary_.append(holdings::pack(ordinal, h)));
        placed += h.order_placed ? 1U : 0U;
      }
      table_.push_back({d.session_index, d.session, d.posttrade_nav, d.decision, rows,
                        names.size()});
    } else {
      for (const auto& h : names) {
        write_name(d, h);
        placed += h.order_placed ? 1U : 0U;
      }
    }
    const bool skipped = neutralize_skipped(d.construction.neutralize);
    days_ << d.session << ',' << d.decision << ',' << d.rebalance << ',' << d.executed << ','
          << d.pretrade_nav << ',' << d.posttrade_nav << ',' << names.size() << ','
          << d.held_names << ',' << d.long_dollars << ',' << d.short_dollars << ','
          << d.gross_leverage << ',' << d.net_leverage << ',' << d.traded_dollars << ','
          << d.fills << ',' << d.capped_fills << ',' << d.blocked_absent << ','
          << d.blocked_liquidity << ',' << d.unfilled_dollars << ',' << d.planned_turnover << ','
          << d.planned_gross << ',' << d.planned_net << ',' << placed << ','
          << d.blocked_short_names << ',' << d.blocked_short_dollars << ','
          << detail::neutralize_outcome_label(d.construction.neutralize) << ',' << skipped << ','
          << d.construction.locate_zeroed << '\n';
    ++sessions; rows += names.size();
    decisions += d.decision ? 1U : 0U; skipped_rebalances += skipped ? 1U : 0U;
    if (!days_ || (format_ == NavHoldingsFormat::Csv && !names_))
      return co::Err(co::ErrorCode::IoError, "nav replay: holdings write");
    return co::Ok();
  }
  // Closes every file; f64 then writes holdings_index.json (the session table).
  co::Status close() {
    days_.close();
    if (!days_) return co::Err(co::ErrorCode::IoError, "nav replay: holdings close");
    if (format_ == NavHoldingsFormat::Csv) {
      names_.close();
      if (!names_) return co::Err(co::ErrorCode::IoError, "nav replay: holdings close");
      return co::Ok();
    }
    ATX_TRY(data_, binary_.close());
    ATX_TRY(index_sha_, holdings::write_index(dir_ / holdings::index_file, ids_, table_, data_));
    return co::Ok();
  }
  // The published files and their SHA-256 (the binary rows' digest was taken as written).
  [[nodiscard]] co::Result<Json> files() const {
    ATX_TRY(auto days_sha, co::sha256_file((dir_ / holdings::days_file).string()));
    if (format_ == NavHoldingsFormat::F64)
      return co::Ok(Json{{holdings::data_file, data_.sha256},
                         {holdings::index_file, index_sha_}, {holdings::days_file, days_sha}});
    ATX_TRY(auto names_sha, co::sha256_file((dir_ / "holdings.csv").string()));
    return co::Ok(Json{{"holdings.csv", names_sha}, {holdings::days_file, days_sha}});
  }
  [[nodiscard]] const std::filesystem::path& directory() const { return dir_; }
  [[nodiscard]] NavHoldingsFormat format() const { return format_; }
  u64 rows{};
  usize sessions{}, decisions{}, skipped_rebalances{};

private:
  void write_name(const NavReplayDay& d, const NavHolding& h) {
    auto& f = names_;
    f << d.session << ',' << h.instrument_id << ',' << h.member << ',' << h.stale << ','
      << detail::borrow_tier_label(h.tier) << ',' << unsigned{h.tier_missing} << ','
      << side_label(h.held_dollars) << ',' << h.held_dollars << ',' << h.held_weight << ','
      << d.posttrade_nav << ',' << detail::fill_status_label(h.fill) << ',' << h.filled_dollars
      << ',' << h.fill_cost_dollars << ',' << h.unfilled_dollars << ',';
    write_value(f, h.desired); f << ',';
    write_value(f, h.rule_weight); f << ',';
    write_value(f, h.target_weight); f << ',';
    write_value(f, d.decision ? h.target_weight - h.held_weight : nan); f << ',';
    f << (h.order_placed ? h.order_dollars - h.held_dollars : 0.0) << ',' << h.order_placed
      << ',' << h.locate_blocked << ',' << h.order_working << ',';
    write_value(f, h.order_dollars); f << '\n';
  }
  std::filesystem::path dir_;
  NavHoldingsFormat format_{NavHoldingsFormat::F64};
  std::span<const u64> ids_; // the role order (borrowed from the loaded blend)
  std::ofstream names_, days_;
  holdings::BinaryAppender binary_;
  std::vector<holdings::SessionEntry> table_;
  holdings::BinaryAppender::Closed data_;
  std::string index_sha_;
};
// manifest.json LAST (after the NAV directory): file SHAs and the NAV recipe SHA. The csv
// format keeps atx.nav-holdings/v1 byte for byte; f64 is v2 (+ format, the index schema).
co::Status publish_holdings(const NavRun& run, const HoldingsEmitter& csv,
                            std::ostream& progress) {
  const bool tiered = !run.fields.is_null();
  const auto method = nav_recipe(run.cfg, run.base, run.scenarios, run.limits, run.fields);
  ATX_TRY(auto method_sha, co::sha256_hex(method.dump()));
  const auto& dir = csv.directory();
  ATX_TRY(auto files, csv.files());
  const bool binary = csv.format() == NavHoldingsFormat::F64;
  const auto book = scenario_label(run.scenarios[nav_primary_scenario_index], tiered);
  Json manifest{{"schema", binary ? holdings::manifest_schema_v2 : "atx.nav-holdings/v1"},
      {"status", "complete"},
      {"book", book}, {"nav_recipe_sha256", method_sha},
      {"combined_sha256", run.cfg.combined_sha256}, {"role_sha256", run.cfg.role_sha256},
      {"rule", detail::construction_rule_id(run.cfg.target)},
      {"sessions", csv.sessions}, {"decision_sessions", csv.decisions}, {"rows", csv.rows},
      {"neutralize_skipped_rebalances", csv.skipped_rebalances},
      {"files", std::move(files)}, {"columns", holdings_declaration}};
  if (binary)
    manifest["format"] = Json{{"id", "f64"}, {"index_schema", holdings::index_schema},
        {"rows", "holdings.f64 (layout in holdings_index.json) carries every holdings.csv "
                 "column of v1: the stored ones exactly, the derived ones by the declared "
                 "expressions"}};
  v7::extend_holdings(manifest); // L4 hook: identity unless extended
  progress << "nav replay holdings " << book << ": " << csv.rows << " rows over "
           << csv.sessions << " sessions\n";
  if (csv.skipped_rebalances)
    progress << "nav replay holdings ERROR: " << csv.skipped_rebalances
             << " cadence rebalance(s) skipped by the neutralization guard\n";
  return write_json(dir / "manifest.json", manifest);
}
} // namespace

co::Status run_nav_replay(const TargetReplayRunConfig& cfg, const NavTurnoverLimits& limits,
                          const NavFieldsPin& fields, std::ostream& progress) {
  return run_nav_replay(cfg, limits, fields, NavRateOptions{}, progress);
}

co::Status run_nav_replay(const TargetReplayRunConfig& cfg, const NavTurnoverLimits& limits,
                          const NavFieldsPin& fields, const NavRateOptions& rate,
                          std::ostream& progress) {
  return run_nav_replay(cfg, limits, fields, rate, NavExecutionOptions{}, progress);
}

co::Status run_nav_replay(const TargetReplayRunConfig& cfg, const NavTurnoverLimits& limits,
                          const NavFieldsPin& fields, const NavRateOptions& rate,
                          const NavExecutionOptions& execution, std::ostream& progress) {
  return run_nav_replay(cfg, limits, fields, rate, execution, NavEmitOptions{}, progress);
}

co::Status run_nav_replay(const TargetReplayRunConfig& cfg, const NavTurnoverLimits& limits,
                          const NavFieldsPin& fields, const NavRateOptions& rate,
                          const NavExecutionOptions& execution, const NavEmitOptions& emit,
                          std::ostream& progress) {
  try {
    if (cfg.role_path.empty() || cfg.role_sha256.empty() || cfg.output_directory.empty())
      return co::Err(co::ErrorCode::InvalidArgument, "nav replay: pinned role and output required");
    if (cfg.target.one_way_bps != 0 || cfg.target.annual_borrow_bps != 0)
      return co::Err(co::ErrorCode::InvalidArgument,
                     "nav replay: costs and borrow are fixed scenarios, not flags");
    if (!std::isfinite(limits.daily_mean_max) || !(limits.daily_mean_max > 0) ||
        !std::isfinite(limits.daily_p95_max) || !(limits.daily_p95_max > 0))
      return co::Err(co::ErrorCode::InvalidArgument, "nav replay: daily turnover ceilings");
    if (fields.manifest_path.empty() != fields.manifest_sha256.empty())
      return co::Err(co::ErrorCode::InvalidArgument,
                     "nav replay: --fields and --fields-sha256 go together");
    const bool tiered = !fields.manifest_path.empty();
    if (neutralize_by_industry(cfg.target.neutralize) && !tiered)
      return co::Err(co::ErrorCode::InvalidArgument,
                     "nav replay: the industry ids need --fields (grp_ff12)");
    const auto scenarios = v7::run_scenarios(nav_scenario_matrix(tiered)); // L4 hook
    NavReplayConfig base; base.target = cfg.target;
    base.rate = rate.rate; base.rate_rra = rate.rate_rra; base.rate_lambda = rate.rate_lambda;
    base.rate_min = rate.rate_min; base.rate_max = rate.rate_max;
    base.order_basis = execution.order_basis; base.locate_in_aim = execution.locate_in_aim;
    base.liquidity_cache = execution.liquidity_cache;
    base.warm_start_sessions = execution.warm_start_sessions;
    const bool holdings = !emit.holdings_directory.empty();
    if (holdings) {
      const auto nav_dir = std::filesystem::path(cfg.output_directory);
      const auto holdings_dir = std::filesystem::path(emit.holdings_directory);
      if (nav_dir.lexically_normal() == holdings_dir.lexically_normal() ||
          std::filesystem::exists(nav_dir) || std::filesystem::exists(holdings_dir))
        return co::Err(co::ErrorCode::AlreadyExists,
                       "nav replay: --output and --emit-holdings must differ and not exist");
    }
    // Admission (the reserve at the pinned role's own geometry, then the fields and the
    // loader against the rest) and the pinned load.
    ATX_TRY(auto admitted, admit_and_load(cfg, base, fields, scenarios.size(), holdings));
    const auto& blend = admitted.blend; const auto& loaded = admitted.fields;
    auto view = blend.view();
    view.industry = loaded.industry; // empty unless an industry id
    const NavReplayInput input{view, blend.volume,
                               NavFinancingFields{loaded.shares_out, loaded.si_shares}};
    // All scenarios in lockstep: the desired target (and its price exposures) and
    // the borrow tiers are formed once per decision for every scenario book. With
    // --emit-holdings the primary book is observed (read only) while it runs.
    std::vector<NavReplayResult> results;
    HoldingsEmitter csv;
    if (holdings) {
      ATX_TRY_VOID(csv.open(emit.holdings_directory, emit.format, view.instrument_ids));
      ATX_TRY(results, replay_nav_scenarios(input, base, scenarios, csv,
                                            nav_primary_scenario_index));
      ATX_TRY_VOID(csv.close());
    } else {
      ATX_TRY(results, replay_nav_scenarios(input, base, scenarios));
    }
    std::vector<NavSummary> summaries; summaries.reserve(scenarios.size());
    for (const auto& result : results) {
      ATX_TRY(auto summary, summarize_nav(result, limits));
      summaries.push_back(std::move(summary));
    }
    // L4 hook: records the books; spo's specific-ceiling tripwire voids the run here, before
    // publish_nav creates the output directory (Ok without an extension).
    ATX_TRY_VOID(v7::capture(scenarios, results, summaries));
    const Json warm = warm_start_record(base, view);
    const NavRun run{cfg, limits, base, scenarios, results, summaries, blend.manifest_json,
                     loaded.binding, warm};
    // Console provenance only: the cache changes no published byte, so no file records it.
    if (base.liquidity_cache) progress << "nav replay: shared execution liquidity cache on\n";
    if (!warm.is_null())
      progress << "nav replay: warm start of " << base.warm_start_sessions
               << " sessions, first decision session_ns "
               << warm.at("first_decision_session_ns").get<i64>() << '\n';
    ATX_TRY_VOID(publish_nav(run, progress));
    return holdings ? publish_holdings(run, csv, progress) : co::Ok();
  } catch (const std::exception& e) {
    return co::Err(co::ErrorCode::InvalidArgument, std::string("nav replay: ") + e.what());
  }
}

int dispatch_nav_replay(int argc, char** argv, std::ostream& out, std::ostream& err) {
  if (v7::claims_nav_args(argc, argv)) return v7::dispatch_nav_v7(argc, argv, out, err); // L4
  try {
    TargetReplayRunConfig cfg; NavTurnoverLimits limits; NavFieldsPin fields;
    NavRateOptions rate; NavExecutionOptions execution; NavEmitOptions emit;
    std::set<std::string> seen;
    for (int i = 1; i < argc; ++i) {
      const std::string key = argv[i];
      // v6 switches take no value; repeated, they are a usage error like any flag.
      if (key == "--locate-in-aim" || key == "--liquidity-cache") {
        if (!seen.insert(key).second) throw std::invalid_argument("duplicate/missing flag");
        (key == "--locate-in-aim" ? execution.locate_in_aim : execution.liquidity_cache) = true;
        continue;
      }
      if (key == "--help") {
        out << "nav --combined PATH --combined-sha256 SHA --role PATH --role-sha256 SHA "
               "--output NEWDIR [--rule baseline-v1|monthly-budget-v2|aim-partial-v5] "
               "[--cadence 5] [--trade-fraction .25 (aim-partial-v5: theta)] "
               "[--monthly-budget .30] "
               "[--neutralize none|price-risk-v1|price-risk-ind-v1|price-risk-ind-v2 "
               "(ind: FF12 demeaning, needs --fields; ind-v2: vol 126 / log-ADV 252 windows)] "
               "[--band-multiple 0] "
               "[--dust-multiple 0 --aim-leverage 1 (aim-partial-v5 only)] "
               "[--rate fixed|per-name-v1 (aim-partial-v5 only) --rate-rra 10 "
               "--rate-lambda .2 --rate-min .01 --rate-max .15 (per-name-v1 only)] "
               "[--daily-turnover-mean-max .20] [--daily-turnover-p95-max .30] "
               "[--fields FIELDS/manifest.json --fields-sha256 SHA] [--max-bytes 536870912] "
               "[--order-basis target|delta] [--exit-rate 1 (below 1: aim-partial-v5, "
               "dust > 0)] [--locate-in-aim (needs --fields and --neutralize)] "
               "[--liquidity-cache] [--emit-holdings NEWDIR (primary book per-name "
               "holdings, streamed; manifest.json last) [--holdings-format f64|csv (f64: "
               "holdings.f64 + holdings_index.json, the default; csv: the v1 holdings.csv; "
               "ignored without --emit-holdings)]] "
               "[--warm-start-sessions 0 (K > 0: decide and trade from score_begin - K, "
               "score from score_begin; K <= the role's score_begin)] "
               "[--hold-band B (aim-partial-v5; v8 hold-band-v1 rank hysteresis, B in [0, 1])]\n"
               "Runs every fixed scenario (S1 linear-6bps-stale5-v1, S2 modeled-1bn-stale5-v1 "
               "PRIMARY, S3 modeled-1bn-terminal-adverse-v1); costs/borrow are not flags.\n"
               "Without --fields: flat-300-v0 financing only. With the pinned role fields "
               "(atx.research-role-fields/v1: shares_out, si_shares): S1/S2/S3 x swap-fin-v1 "
               "(PRIMARY S2), S2 x flat-300-v0, S2 x engine-tiers-v1.\n";
        v7::append_help(out); // L4 hook
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
      else if (key == "--fields") fields.manifest_path = value;
      else if (key == "--fields-sha256") fields.manifest_sha256 = value;
      else if (key == "--emit-holdings") emit.holdings_directory = value;
      else if (key == "--holdings-format") {
        // Accepted without --emit-holdings: the v7 capacity pass drops --emit-holdings
        // and forwards every other flag (strategy_nav_v7.cpp).
        if (value == "f64") emit.format = NavHoldingsFormat::F64;
        else if (value == "csv") emit.format = NavHoldingsFormat::Csv;
        else throw std::invalid_argument("unknown --holdings-format (f64|csv)");
      } else if (key == "--cadence") {
        const auto x = integer();
        if (x > max_dates) throw std::invalid_argument("cadence exceeds bound");
        cfg.target.cadence = static_cast<usize>(x);
      } else if (key == "--trade-fraction") cfg.target.trade_fraction = real();
      else if (key == "--monthly-budget") cfg.target.monthly_budget = real();
      else if (key == "--max-bytes") cfg.target.max_working_bytes = integer();
      else if (key == "--band-multiple") cfg.target.band_multiple = real();
      else if (key == "--dust-multiple") cfg.target.dust_multiple = real();
      else if (key == "--aim-leverage") cfg.target.aim_leverage = real();
      else if (key == "--exit-rate") cfg.target.exit_rate = real();
      else if (key == "--hold-band") cfg.target.hold_band = real(); // v8 R-4 hold-band-v1
      else if (key == "--warm-start-sessions") {
        const auto x = integer();
        if (x > max_dates) throw std::invalid_argument("warm start exceeds bound");
        execution.warm_start_sessions = static_cast<usize>(x);
      } else if (key == "--order-basis") {
        if (value == "target") execution.order_basis = NavOrderBasis::Target;
        else if (value == "delta") execution.order_basis = NavOrderBasis::Delta;
        else throw std::invalid_argument("unknown --order-basis (target|delta)");
      } else if (key == "--rate-rra") rate.rate_rra = real();
      else if (key == "--rate-lambda") rate.rate_lambda = real();
      else if (key == "--rate-min") rate.rate_min = real();
      else if (key == "--rate-max") rate.rate_max = real();
      else if (key == "--daily-turnover-mean-max") limits.daily_mean_max = real();
      else if (key == "--daily-turnover-p95-max") limits.daily_p95_max = real();
      else if (key == "--neutralize") {
        if (!detail::parse_neutralize(value, cfg.target))
          throw std::invalid_argument(
              "unknown --neutralize (none|price-risk-v1|price-risk-ind-v1|price-risk-ind-v2)");
      } else if (key == "--rate") {
        if (value == "fixed") rate.rate = NavRateRule::Fixed;
        else if (value == "per-name-v1") rate.rate = NavRateRule::PerNameV1;
        else throw std::invalid_argument("unknown --rate (fixed|per-name-v1)");
      } else if (key == "--rule") {
        if (value == "baseline-v1") cfg.target.rule = TargetReplayRule::BaselineTargetV1;
        else if (value == "monthly-budget-v2")
          cfg.target.rule = TargetReplayRule::MonthlyTargetBudgetV2;
        else if (value == "aim-partial-v5") cfg.target.rule = TargetReplayRule::AimPartialV5;
        else throw std::invalid_argument("unknown target rule");
      } else throw std::invalid_argument("unknown flag: " + key);
    }
    if (cfg.role_path.empty() || cfg.role_sha256.empty())
      throw std::invalid_argument("--role and --role-sha256 are required in nav mode");
    if (fields.manifest_path.empty() != fields.manifest_sha256.empty())
      throw std::invalid_argument("--fields and --fields-sha256 go together");
    // The rate belongs to aim-partial-v5, its parameters to --rate per-name-v1.
    if (seen.count("--rate") && cfg.target.rule != TargetReplayRule::AimPartialV5)
      throw std::invalid_argument("--rate is aim-partial-v5 only");
    for (const char* flag : {"--rate-rra", "--rate-lambda", "--rate-min", "--rate-max"})
      if (seen.count(flag) && rate.rate != NavRateRule::PerNameV1)
        throw std::invalid_argument(std::string(flag) + " needs --rate per-name-v1");
    const auto status = run_nav_replay(cfg, limits, fields, rate, execution, emit, out);
    if (!status) { err << status.error().to_string() << '\n'; return 1; }
    return 0;
  } catch (const std::exception& e) { err << "nav replay: " << e.what() << '\n'; return 2; }
}

// Seams of the daily decide path (strategy_nav_replay_detail.hpp): the replay's own
// admission, loader, recipe and DECIDE functions, never a copy of them.
namespace detail {
const char* borrow_tier_label(u8 tier) {
  switch (tier) {
  case tier_gc: return "gc";
  case tier_warm: return "warm";
  case tier_special: return "special";
  default: return "none";
  }
}
const char* fill_status_label(NavFillStatus fill) {
  switch (fill) {
  case NavFillStatus::None: return "none";
  case NavFillStatus::Complete: return "complete";
  case NavFillStatus::Capped: return "capped";
  case NavFillStatus::BlockedAbsent: return "blocked-absent";
  case NavFillStatus::BlockedLiquidity: return "blocked-liquidity";
  }
  return "unknown";
}
const char* holdings_csv_columns() {
  return "session_ns,instrument_id,member,stale,tier,tier_missing,side,held_dollars,"
         "held_weight,nav_post,fill,filled_dollars,fill_cost_dollars,unfilled_dollars,"
         "desired_weight,rule_weight,target_weight,planned_trade_weight,planned_trade_dollars,"
         "order_placed,locate_blocked,order_working,order_dollars";
}

NavReplayInput NavDeployLoad::view() const {
  auto x = blend.view();
  x.industry = industry; // empty unless an industry id
  return NavReplayInput{x, blend.volume, NavFinancingFields{shares_out, si_shares}};
}

co::Result<NavDeployLoad> load_nav_deploy(const TargetReplayRunConfig& cfg,
                                          const NavReplayConfig& base,
                                          const NavTurnoverLimits& limits,
                                          const NavFieldsPin& fields) {
  try {
    if (cfg.role_path.empty() || cfg.role_sha256.empty() ||
        fields.manifest_path.empty() != fields.manifest_sha256.empty())
      return co::Err(co::ErrorCode::InvalidArgument,
                     "nav deploy: pinned role required; fields path and SHA go together");
    const bool tiered = !fields.manifest_path.empty();
    if (neutralize_by_industry(cfg.target.neutralize) && !tiered)
      return co::Err(co::ErrorCode::InvalidArgument,
                     "nav deploy: the industry ids need the fields (grp_ff12)");
    ATX_TRY_VOID(validate_nav_config(base));
    ATX_TRY(auto admitted, admit_and_load(cfg, base, fields, 1, false));
    // The recipe run_nav_replay publishes for the same (cfg, base, limits, fields).
    const auto method = nav_recipe(cfg, base, nav_scenario_matrix(tiered), limits,
                                   admitted.fields.binding);
    ATX_TRY(auto digest, co::sha256_hex(method.dump()));
    NavDeployLoad out;
    out.blend = std::move(admitted.blend);
    out.shares_out = std::move(admitted.fields.shares_out);
    out.si_shares = std::move(admitted.fields.si_shares);
    out.industry = std::move(admitted.fields.industry);
    out.recipe_sha256 = std::move(digest);
    return co::Ok(std::move(out));
  } catch (const std::bad_alloc&) {
    return co::Err(co::ErrorCode::OutOfRange, "nav deploy: allocation failed");
  } catch (const std::exception& e) {
    return co::Err(co::ErrorCode::InvalidArgument, std::string("nav deploy: ") + e.what());
  }
}

co::Result<NavDecision> nav_decide(const NavReplayInput& in, const NavReplayConfig& cfg, usize d,
                                   std::span<const f64> held, f64 nav_post,
                                   std::span<const u8> no_locate,
                                   const atx::engine::book::HoldBandState* hold) {
  try {
    // As replay_nav_scenarios: the NAV volume is authoritative, also for price risk.
    TargetReplayInput x = in.target;
    x.volume = in.volume;
    const NavReplayInput input{x, in.volume, in.financing};
    const bool tiered = !in.financing.shares_out.empty();
    if ((cfg.scenario.financing.rule == NavFinancingRule::TieredSwapV1 || cfg.locate_in_aim) &&
        !tiered)
      return co::Err(co::ErrorCode::InvalidArgument,
                     "nav decide: tiered financing and locate-in-aim need the borrow fields");
    if (cfg.rate != NavRateRule::Fixed ||
        cfg.target.rule == TargetReplayRule::MonthlyTargetBudgetV2)
      return co::Err(co::ErrorCode::InvalidArgument,
                     "nav decide: rate per-name-v1 and monthly-budget-v2 carry book state "
                     "(pre-trade NAV, month-to-date plan) that positions do not");
    ATX_TRY_VOID(validate_nav_input(input, cfg, 1));
    const usize n = x.instruments;
    if (d < x.decision_begin || d >= x.decision_end)
      return co::Err(co::ErrorCode::OutOfRange, "nav decide: as-of row outside the score window");
    if (held.size() != n || !std::all_of(held.begin(), held.end(), [](f64 h) {
          return std::isfinite(h);
        }) || !std::isfinite(nav_post) || !(nav_post > 0) ||
        (!no_locate.empty() && no_locate.size() != n))
      return co::Err(co::ErrorCode::InvalidArgument,
                     "nav decide: one finite position per name, NAV > 0, locate mask geometry");
    NavDecision out;
    Construction shared(n, cfg.locate_in_aim);
    if (hold) shared.state.hold = *hold; // v8 R-4: the book's hold-band state entering d
    BorrowTiers tiers(n, tiered);
    TierCensus census;
    out.cadence = (d - x.decision_begin) % cfg.target.cadence == 0;
    ATX_TRY(out.rebalance, decide_construction(x, in.financing, cfg.target, d, out.cadence,
                                               no_locate, shared, tiers, census,
                                               out.construction));
    out.current.resize(n); out.target.resize(n); out.rule.resize(n);
    for (usize i = 0; i < n; ++i) out.current[i] = held[i] / nav_post;
    out.plan.decision = d; out.plan.session = x.session_keys[d];
    out.plan.calendar_month = calendar_month(out.plan.session);
    NavReplayDay day; // receives the locate block's counts
    const PlanInputs inputs{x, cfg, d, out.rebalance, shared.desired, tiers, no_locate};
    ATX_TRY_VOID(plan_weights(inputs, 0.0, {}, out.current, out.target, nav_post, out.rule,
                              out.plan, day));
    out.construction.banded_names = out.plan.construction.banded_names;
    out.members = members_at(x, d);
    out.blocked_short_names = day.blocked_short_names;
    out.blocked_short_dollars = day.blocked_short_dollars;
    out.member_tiers = census.members; out.member_missing_predictors = census.missing;
    out.sigma.assign(n, nan);
    for (usize i = 0; i < n; ++i)
      if (x.member[d * n + i]) out.sigma[i] = window_liquidity(x, in.volume, cfg, d, i).sigma;
    out.desired = std::move(shared.desired); out.no_short = std::move(shared.no_short);
    out.tier = std::move(tiers.tier); out.tier_missing = std::move(tiers.missing);
    out.hold = std::move(shared.state.hold);
    return co::Ok(std::move(out));
  } catch (const std::bad_alloc&) {
    return co::Err(co::ErrorCode::OutOfRange, "nav decide: allocation failed");
  } catch (const std::length_error&) {
    return co::Err(co::ErrorCode::OutOfRange, "nav decide: allocation extent");
  }
}

co::Result<std::vector<f64>> execution_adv(const NavReplayInput& in, const NavReplayConfig& cfg,
                                           usize t) {
  const auto& x = in.target;
  const usize n = x.instruments, cells = x.dates * n;
  if (!t || t > x.dates || !cfg.liquidity_window || in.volume.size() != cells ||
      x.raw_close.size() != cells || x.close.size() != cells || x.present.size() != cells)
    return co::Err(co::ErrorCode::InvalidArgument, "nav execution ADV: session or geometry");
  try {
    std::vector<f64> adv(n);
    for (usize i = 0; i < n; ++i) adv[i] = window_liquidity(x, in.volume, cfg, t, i).adv;
    return co::Ok(std::move(adv));
  } catch (const std::bad_alloc&) {
    return co::Err(co::ErrorCode::OutOfRange, "nav execution ADV: allocation failed");
  }
}
} // namespace detail
} // namespace atx::impl::strategy
