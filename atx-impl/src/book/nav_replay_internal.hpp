#pragma once

// P9 C3 (task 1, move-only): the internals the NAV replay TUs share, moved unchanged from
// strategy_nav_replay.cpp's anonymous namespaces into namespace nav_book (types, constants,
// templates; declarations of the functions one TU defines and another calls). Each
// atx-impl/src/book/nav_*.cpp TU includes it and `using namespace nav_book`.

#include "strategy_nav_replay.hpp"
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
#include "atx/engine/data/research_window.hpp" // P9 C3 (P13): the fields manifest's seal
#include "atx/engine/eval/hac.hpp"
#include "atx/engine/parallel/det_pool.hpp"
#include "atx/engine/parallel/lockstep_grid.hpp" // v8 D-1: books on a pool, grid spec
#include "book/nav_rules.hpp" // P9 C3: K-P9-7, the one rule table
#include "book/nav_spec.hpp"  // P9 C3: nav --spec / --print-spec
#include "build_provenance.hpp"  // P9 C1: summary.json producer identity
#include "strategy_holdings.hpp" // v7 W4: the f64 holdings layout
#include "strategy_risk_target.hpp" // P9 C1: each book's leverage rule
#include "strategy_nav_replay_detail.hpp"
#include "strategy_target_replay_detail.hpp"
#include "strategy_nav_v7.hpp" // platform-v7 L4 hook (one seam object)

namespace atx::impl::strategy::nav_book {
using namespace atx;
namespace co = atx::core;
namespace bk = atx::engine::book;
namespace ce = atx::engine::cost;
namespace hac = atx::engine::eval::hac;
namespace par = atx::engine::parallel;
using Json = nlohmann::json;
constexpr f64 nan = std::numeric_limits<f64>::quiet_NaN();
constexpr f64 inf = std::numeric_limits<f64>::infinity();
constexpr i64 day_ns = 86'400'000'000'000LL;
constexpr i64 hour_ns = 3'600'000'000'000LL;
constexpr usize max_dates = 4096, max_names = 20000;
// Books run in lockstep by replay_nav_scenarios (P9 C1: 16, the main books and the capacity
// books of --capacity-curve in one lockstep; 8 before).
constexpr usize max_scenarios = 16;
constexpr usize max_book_workers = 64; // NavReplayConfig::book_workers
constexpr u64 max_grid_file_bytes = 1ULL << 20;
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
// --emit-holdings with a declared hold band (v8 E-16): the snapshot of the band's state
// (rank_set, desired_prev) entering each session's construction.
constexpr u64 hold_trace_name_bytes = 2 * sizeof(f64);
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
bool delta_order(const NameState& s, usize i);
// Decision construction shared by every scenario book of one lockstep replay: the
// desired target depends only on the signal, membership and role prices at d.
struct Construction {
  Construction(usize n, bool locate) : desired(n), no_short(locate ? n : 0) { row.reserve(n); }
  std::vector<std::pair<f64, usize>> row;
  std::vector<f64> desired;
  std::vector<u8> no_short; // locate-in-aim only: 1 = special tier at the decision
  PriceRiskScratch price;   // grows only when neutralizing
  detail::DesiredState state; // v8 (R-4 hold band): carried across decisions; empty when off
  // v8 E-16 (--emit-holdings with a declared hold band only): the band's state entering this
  // session's construction, i.e. what DECIDE at the session reads; empty otherwise.
  bool emit_hold{};
  bk::HoldBandState hold_in;
  // P9 AL-CLOCK (NavReplayInput::info_clock): the decision's row of the clock (empty without
  // one) and, under rate info-clock-rate-v1, its per-name rates (nav_info_clock_rates; empty
  // otherwise). Both set by run_books on the calling thread before the books' DECIDE; a book
  // only reads them.
  std::span<const f64> clock;
  std::vector<f64> clock_rate;
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
  // v8 E-25: the prices MARK reads (mark_flat, carry_absent's presence test, realize): the
  // label role's with --label-role, else x's own close, raw_close and present (the same spans,
  // so the same reads). DECIDE and EXECUTE read x only.
  NavMarkPrices mark{};
  // P9 AL-CLOCK: NavReplayInput::info_clock (the same span for every book; empty without one).
  std::span<const f64> info_clock{};
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
  // P9 C1 (DEC-10): the book's own leverage rule state (null under a fixed leverage) and v7
  // state (null without the v7 seam), both made on the calling thread before the books run;
  // its label ("<trading id>+<financing id>") and whether it is a capacity book (no records).
  std::string label;
  bool capacity{};
  std::unique_ptr<risk_target::BookLeverage> leverage;
  std::unique_ptr<v7::BookState> v7_state;
};
// The shared per-session liquidity cache: always under rate per-name-v1 (its rates read
// it), and at a fixed rate when --liquidity-cache asks for it.
bool liquidity_cached(const NavReplayConfig& cfg);
co::Status validate_nav_config(const NavReplayConfig& cfg);
// The price-exposure session ring the shared construction binds when it neutralizes
// (v8 D-1): charged by the input budget and the pinned run's reserve alike.
u64 ring_bytes(const TargetReplayConfig& target, usize names);
// A warm start of K sessions decides from role row score_begin - K: never before row 0.
co::Status check_warm_start(usize sessions, usize score_begin);
co::Status validate_nav_input(const NavReplayInput& in, const NavReplayConfig& cfg,
                              usize books);
co::Result<std::unique_ptr<const bk::ReplayCostModel>> make_cost_model(const NavScenario& s);
// Same interval guard as the target replay / strategy runner.
bool guarded_move(f64 close_a, f64 close_b, f64 raw_a, f64 raw_b);
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
                                 const NavReplayConfig& cfg, usize t, usize i);
WindowLiquidity window_liquidity(const Ctx& c, usize t, usize i);
// The scenario's row over a window: its half spread, and its declared fallback sigma
// when the window has too few pairs. The same values as computing it in one piece.
Liquidity liquidity_row(const Ctx& c, const WindowLiquidity& window);
Liquidity liquidity_row(const Ctx& c, usize t, usize i);
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
                    bool execution, bool decision, LiquidityCache& cache);
void add(NavBucket& bucket, f64 exposure, f64 pnl);
struct EventValues {
  usize run{};
  f64 exposure{}, r_adj{nan}, r_raw{nan}, haircut{}, pnl{nan};
};
co::Status push_event(const Ctx& c, Book& b, NavEventKind kind, usize t, usize i,
                      const EventValues& v);
// MARK at t > begin: financing on pre-mark dollars x calendar days / day count, then
// drift/stale/write-off. NAVpre_t = NAVpost_{t-1} + P + W - B - L (B: short leg, L:
// long leg), so the fill costs of t-1 land in r_t; cash + sum(held) must reconcile
// to it (checked in close_day).
co::Status mark_session(const Ctx& c, Book& b, const BorrowTiers& tiers, usize t,
                        NavReplayDay& day);
// EXECUTE working orders at close t. Absent names are blocked (order persists); an
// unusable ADV fills nothing; a capped fill leaves the residual working. A target
// order requests order - held (the drift since its decision is traded back); a delta
// order requests order - anchor, its remaining decision-dollar delta (the drift rides).
co::Status execute_orders(const Ctx& c, Book& b, usize t, const LiquidityCache& cache,
                          NavReplayDay& day);
// Borrow tiers at decision d, once for every book. Folds presence rows up to d into
// the first-present rows (monotone: each row is scanned once per replay), then asks
// the engine per name; a missing predictor or Unavailable is warm and flagged. An
// engine refusal of well-formed predictors is an error, never a silent tier.
co::Status classify_borrow(const TargetReplayInput& x, const NavFinancingFields& f, usize d,
                           BorrowTiers& tiers, TierCensus& census);
// DECIDE, the part shared by every book of decision d: the borrow tiers of d, the
// locate-in-aim mask (allocated only under it), and on a cadence decision the desired
// target (form_desired_target). Returns the effective rebalance (false when the guard
// skips it); `construction` receives the decision's record.
co::Result<bool> decide_construction(const TargetReplayInput& x, const NavFinancingFields& fields,
                                     const NavReplayConfig& cfg, usize d, bool cadence,
                                     std::span<const u8> no_locate, Construction& shared,
                                     BorrowTiers& tiers, TierCensus& census,
                                     ConstructionDay& construction);
// What every book's weight plan at decision d shares.
struct PlanInputs {
  const TargetReplayInput& x;
  const NavReplayConfig& cfg; // the book's (its scenario's locate rule)
  usize d{};
  bool rebalance{};           // effective
  const std::vector<f64>& desired;
  const BorrowTiers& tiers;
  std::span<const u8> no_locate; // decide --locates only; empty in the replay
  // v8 Y-5 two-speed-v1 (the replay only; empty otherwise): F entering this decision's step,
  // which a book under a leverage scaler carries at its own scale (Ruling PM8-16 #10).
  std::span<const f64> two_speed_fast;
  // P9 C1 (DEC-10), the replay only (null otherwise: the loose v7 seam plans): the book's
  // leverage rule state, its v7 state, its label, where its scored leverage records go (null:
  // not scored) and the decision's shared liquidity (cost_v2::decision_liquidity; null when
  // the decision does not rebalance or no book has v7 state).
  risk_target::BookLeverage* leverage{};
  v7::BookState* book_state{};
  std::string_view label{};
  std::vector<NavLeverageRecord>* records{};
  const cost_v2::DecisionLiquidity* liquidity{};
};
// CONSTRUCTION-RULE DISPATCH SITE, shared by the NAV replay (plan_decision) and the
// daily decide path (detail::nav_decide), so a rule added here runs in both. `planned`
// becomes the target rule's plan from `current` toward p.desired (`spent`: the book's
// month-to-date plan, v2 only; `rates`: the per-name-v1 rates or empty), copied into
// `rule` when that is non-empty, then the scenario's locate block at the decision NAV.
co::Status plan_weights(const PlanInputs& p, f64 spent, std::span<const f64> rates,
                        const std::vector<f64>& current, std::vector<f64>& planned, f64 nav_post,
                        std::span<f64> rule, TargetReplayDay& plan, NavReplayDay& day);
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
                         const cost_v2::DecisionLiquidity* liquidity, NavReplayDay& day);
f64 gross_dollars(const NameState& s);
co::Status close_day(const Ctx& c, Book& b, NavReplayDay& day);
// Final session: still-stale holdings stay at their stale marks and are reported.
co::Status report_unresolved(const Ctx& c, Book& b, usize t);
// The --emit-holdings observer of run_books: book `book` reported to `sink`.
struct Observer {
  NavHoldingsSink* sink{};
  usize book{};
};
// Where the replay's wall time goes (--stage-timers, v8 D-1): the shared construction of
// every decision (borrow tiers, desired target, neutralization, its price exposures
// included) and, of that, the price exposures (the session ring's own clock).
struct StageClock {
  f64 construction{}, exposures{};
};
f64 seconds_since(std::chrono::steady_clock::time_point started);
// P9 C2 (--calibrate-gross): the shared construction of every decision of one run_books call,
// recorded by the calibration pass and restored by the published pass instead of forming it
// again: what the books read of decide_construction's work, i.e. the effective rebalance, the
// tier census and construction record, the desired target after every cadence decision (the
// only decisions that write it) and, with borrow tiers, every decision's tier and missing flags
// (MARK, the locate block and the holdings export read them). The construction's own state (the
// hold band, the price-exposure ring, the locate-in-aim mask, the tier scan) is read by no book,
// so a restored decision hands every book the bytes the formed one would. Valid only while the
// construction reads nothing that differs between the passes (run_nav_replay decides).
struct ConstructionTape {
  struct Decision {
    usize t{};
    bool cadence{}, rebalance{};
    TierCensus census;
    ConstructionDay construction;
  };
  bool replay{}; // false: record
  std::vector<Decision> decisions;
  std::vector<f64> desired;      // names values per cadence decision, in order
  std::vector<u8> tier, missing; // names values per decision (borrow tiers only)
  usize next{}, next_desired{}, next_tier{}; // replay cursors
};
// The record's size bound for `decisions` decisions of `names` names (every decision a cadence
// decision; tiered: two flag bytes per name).
u64 construction_tape_bytes(usize decisions, usize names, bool tiered);
// The record's vectors reserved at construction_tape_bytes' bound once (review S9): the budget
// bounds their capacity too (no growth factor, no reallocation copy while recording).
void reserve_construction(ConstructionTape& tape, usize decisions, usize names, bool tiered);
// Sessions [begin - K, end) (K = warm_start_sessions; 0 without a warm start): MARK
// (t > begin - K) -> EXECUTE (begin - K < t <= end-2) -> DECIDE (t < end-2); rows t >=
// begin are reported. Row t reads data at rows <= t only. The books run in lockstep:
// each book performs exactly its own single-book sequence (mark, execute, decide, close)
// on its own state; only the decision's desired target and borrow tiers (and, with rate
// per-name-v1 or the liquidity cache, the session's liquidity windows) are formed once and
// shared, so every book is bit-identical to a replay on its own. The per-book phases run
// on `pool` when given (engine for_each_lane: lane-owned writes only, the lowest failing
// book's error). An observer only reads the observed book (its trace is written beside,
// never read by, the arithmetic). `stages` (--stage-timers) receives the construction and
// exposure seconds. `tape` (P9 C2, --calibrate-gross; null: every decision formed here, the
// path before C2): record mode appends every decision's shared construction, replay mode
// restores it instead of decide_construction.
co::Result<std::vector<NavReplayResult>> run_books(const TargetReplayInput& x,
                                                   const NavFinancingFields& fields,
                                                   std::span<const Ctx> ctxs,
                                                   const Observer& observer,
                                                   par::DetPool* pool, StageClock* stages,
                                                   ConstructionTape* tape = nullptr,
                                                   std::stop_token stop = {});
Json finite_or_null(f64 x);
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
// Output label of a book: the trading id alone without fields (the legacy names),
// "<trading>+<financing>" in the financing matrix.
std::string scenario_label(const NavScenario& s, bool tiered);
// fields: the pinned borrow-field binding, or null without --fields. label: the summary's
// label_role record (v8 E-25), or null without --label-role.
Json nav_recipe(const TargetReplayRunConfig& cfg, const NavReplayConfig& base,
                std::span<const NavScenario> scenarios, const NavTurnoverLimits& limits,
                const Json& fields, const Json& label);
Json rate_stats_json(const NavRateStats& s);
Json scenario_summary(const NavScenario& sc, bool primary, bool tiered, const NavReplayResult& r,
                      const NavSummary& s, const std::string& daily_sha,
                      const std::string& events_sha);
co::Status write_json(const std::filesystem::path& path, const Json& j);
// The pre-existing columns come first, unchanged; the GMV turnover columns are
// appended, then the construction columns only when an option is non-default, then
// the financing columns only in the financing matrix (with borrow fields).
co::Status write_daily(const std::filesystem::path& path, const NavReplayResult& r,
                       bool construction, bool financing);
// Deterministic spelling of unobservable values across standard libraries.
void write_value(std::ostream& out, f64 x);
co::Status write_events(const std::filesystem::path& path, const NavReplayResult& r);
struct LoadedFields {
  std::vector<f64> shares_out, si_shares;
  std::vector<f64> industry; // industry_group_field; the industry ids only
  Json binding; // recipe record; null when no fields
  std::string seal; // P9 C3 (P13): "match" or "absent" (fields_seal); never published
  // P9 AL-CLOCK: nav_info_clock_field under rate info-clock-rate-v1 (then also in binding's
  // fields_used) or --info-clock-print (then not: print-only); empty otherwise. Its SHA-256 is
  // the info_clock block's field record.
  std::vector<f64> info_clock;
  std::string info_clock_sha256;
};
// Console only (the run's published bytes do not record it, so a legacy and a sealed manifest
// with the same fields publish the same files).
void log_fields_seal(const LoadedFields& fields, std::ostream& progress);
struct AdmittedRun {
  detail::LoadedSavedBlend blend;
  LoadedFields fields;
  detail::LoadedLabelRole label; // v8 E-25: empty without --label-role
};
// Admission and load of one pinned run, shared by run_nav_replay and the decide path:
// the workspace reserve of `books` books at the pinned role's own geometry (manifest
// only), then the fields (pinned and role-checked) and the blend charged against what
// remains, so input + all results fit one budget. `label` (v8 E-25, --label-role): its
// manifest is checked against --role's before any payload is read, its marks are charged
// before the fields, and they load (checked against the blend) last. P9 AL-CLOCK: rate
// info-clock-rate-v1 (base.rate) and `info_clock_print` (--info-clock-print) need the fields pin
// (InvalidArgument before anything is read) and load nav_info_clock_field with the fields.
co::Result<AdmittedRun> admit_and_load(const TargetReplayRunConfig& cfg,
                                       const NavReplayConfig& base, const NavFieldsPin& fields,
                                       usize books, bool holdings,
                                       const NavLabelRolePin& label = {},
                                       bool info_clock_print = false);
// The variants' lockstep groups (variant indices, in order of first appearance): one group
// of every variant, except with the ADV cap (adv_hold_q > 0, shared by same_shared), whose
// shared construction reads aim_leverage: then one group per distinct aim leverage (review
// A-1), each variant capped at its own leverage as in its standalone run. With v8 Y-5
// two-speed-v1 (never with the cap) likewise one group per distinct aim leverage: the
// shared target encodes the fast sleeve F, built at L (theta_s is pinned at .05 and the
// cadence is one per grid, so L alone separates the standalone constructions).
std::vector<std::vector<usize>> leverage_groups(std::span<const NavReplayConfig> variants);
// Every variant x every scenario (book v * scenarios + k) in one lockstep run_books (with
// the ADV cap at several aim leverages or NAV multiples, or two-speed-v1 at several leverages,
// one per book group: book_groups). One variant is replay_nav_scenarios: the same validation
// and the same books. P9 C1: each config gets the v7 seam's leverage rule (v7::configure) and
// the books' records join the seam's after the replay (v7::observe). P9 C2: `tape` is handed to
// a one-group run_books (InvalidArgument for several groups); observe false (the calibration
// pass) keeps the books' records out of the seam's.
co::Result<std::vector<std::vector<NavReplayResult>>> replay_books(
    const NavReplayInput& in, std::span<const NavReplayConfig> variants,
    std::span<const NavScenario> scenarios, const Observer& observer, StageClock* stages,
    ConstructionTape* tape = nullptr, bool observe = true, std::stop_token stop = {});
constexpr const char* stage_timers_declaration =
    "v8 D-1 --stage-timers, seconds on a steady clock: load = entry to the end of the pinned "
    "load (argument checks and payload SHA verification included); exposures = the price "
    "exposures of the shared construction (session ring); construction = the rest of the "
    "shared per-decision construction (borrow tiers, desired target, neutralization); books = "
    "the rest of the replay (every book's MARK, EXECUTE, DECIDE and close); hash = SHA-256 "
    "of the published files; write = the rest (summaries, CSV and JSON writes); the six "
    "stages sum to wall = entry to the summary.json write (that write excluded); observation "
    "only: no published value depends on a clock";
// --stage-timers (v8 D-1): the run's clock marks and the replay's own clock. The stages
// are differences of nested intervals, so each is >= 0 and they sum to wall.
struct StageTimes {
  using Clock = std::chrono::steady_clock;
  Clock::time_point started{Clock::now()}, loaded{started}, replayed{started};
  StageClock replay{};
  f64 hash{};
  [[nodiscard]] Json json() const {
    const auto now = Clock::now();
    const auto between = [](Clock::time_point a, Clock::time_point b) {
      return std::chrono::duration<f64>(b - a).count();
    };
    const f64 replay_wall = between(loaded, replayed), post = between(replayed, now);
    return Json{{"load", between(started, loaded)}, {"exposures", replay.exposures},
                {"construction", replay.construction - replay.exposures},
                {"books", replay_wall - replay.construction}, {"hash", hash},
                {"write", post - hash}, {"wall", between(started, now)},
                {"definition", stage_timers_declaration}};
  }
};
// A SHA-256 of published bytes; its wall time joins the hash stage when timed.
template <class Digest>
co::Result<std::string> timed_digest(StageTimes* times, Digest&& digest) {
  if (!times) return digest();
  const auto started = std::chrono::steady_clock::now();
  auto out = digest();
  times->hash += seconds_since(started);
  return out;
}
// Everything one run publishes, computed before the output directory exists. scenarios,
// results and summaries are the main books; P9 C1: capacity_* the capacity books of
// --capacity-curve (empty without), published into <output>/capacity, and records the run's v7
// records from its own books (null without the v7 seam; the capacity directory's run, where
// capacity_pass, reads the same records and publishes no v7 file).
struct NavRun {
  const TargetReplayRunConfig& cfg;
  const NavTurnoverLimits& limits;
  const NavReplayConfig& base;
  std::span<const NavScenario> scenarios;
  std::span<const NavReplayResult> results;
  std::span<const NavSummary> summaries;
  const std::string& manifest_json; // the pinned combined manifest
  const Json& fields;               // borrow-field binding; null without --fields
  const Json& warm_start;           // the summary's warm_start record; null without one
  StageTimes* times;                // --stage-timers: the clock (null: untimed)
  bool summary_timers;              // write stage_seconds into summary.json (single runs)
  const Json& label;                // v8 E-25: the summary's label_role record; null without
  std::span<const NavScenario> capacity_scenarios{};
  std::span<const NavReplayResult> capacity_results{};
  std::span<const NavSummary> capacity_summaries{};
  const v7::RunRecords* records{};
  bool capacity_pass{};
  // P9 C2: summary.json's calibration record (--calibrate-gross; null: no key). The main
  // directory only; the recipe never records it (the decide path's recipe pin stays the plain
  // run's at the final leverage).
  const Json* calibration{};
  // P9 C3 (PRE-7a, --ann-mean-gross): each scenario of summary.json gains ann_mean_gross. The
  // main directory only (the capacity directory's run leaves it false); the recipe never
  // records it.
  bool ann_mean_gross{};
  // P9 AL-CLOCK (Ruling ALCLOCK-1, --info-clock-print): each scenario of summary.json gains the
  // info_clock block; this is its field record {name, sha256} (null: no block). The main
  // directory only (the capacity directory's run leaves it null); the recipe never records it.
  const Json* info_clock{};
};
// The info_clock block of one main book (Ruling ALCLOCK-1): the book's NavInfoClockStats, its
// shares (NaN -> null), the rate it runs, the rule's constants at the run's cadence and `field`.
Json info_clock_json(const NavReplayConfig& base, const NavInfoClockStats& s, const Json& field);
// The summary's warm_start record; null without a warm start. Called after the replay
// accepted the input (warm_start_sessions <= decision_begin). The role rows of the first
// warm-up decision and of score_begin (review A-3); publish_nav adds each book's gross
// leverage on row score_begin.
Json warm_start_record(const NavReplayConfig& base, const TargetReplayInput& x);
// The summary's label_role record (v8 E-25); null without --label-role. The recipe binds the
// manifest SHA-256 and the rule.
Json label_record(const NavLabelRolePin& pin, const detail::LoadedLabelRole& label);
// Exclusive directory: recipe.json, per-scenario CSVs, summary.json LAST. P9 C1: under the v7
// seam the capacity directory, the v7 files and v7_extras.json come before summary.json, which
// binds v7_extras.json and capacity/summary.json (v7.files) and names its producer.
co::Status publish_nav(const NavRun& run, std::ostream& progress);
// ---- --emit-holdings (v7 B3) ----
constexpr const char* holdings_days_columns =
    "session_ns,decision,rebalance,executed,pretrade_nav,posttrade_nav,rows,held_names,"
    "long_dollars,short_dollars,gross_leverage,net_leverage,traded_dollars,fills,capped_fills,"
    "blocked_absent,blocked_liquidity,unfilled_dollars,planned_turnover,planned_gross,"
    "planned_net,orders_placed,blocked_short_names,blocked_short_dollars,neutralize,"
    "neutralize_skipped,locate_zeroed";
bool neutralize_skipped(NeutralizeOutcome outcome);
const char* side_label(f64 held);
// Streams the per-name rows (f64: buffered binary rows + a session table, the default;
// csv: the v1 holdings.csv) and holdings_days.csv, one session at a time (O(names) memory
// plus the 1 MiB binary buffer and one table entry per session, within the publication
// slack of the workspace reserve). hold_state (a declared hold band, v8 E-16): every row
// also carries the name's rank_set and desired_prev; off, every byte is the pre-v8 layout.
class HoldingsEmitter final : public NavHoldingsSink {
public:
  co::Status open(const std::filesystem::path& dir, NavHoldingsFormat format,
                  std::span<const u64> ids, bool hold_state) {
    if (!std::filesystem::create_directory(dir))
      return co::Err(co::ErrorCode::AlreadyExists,
                     "nav replay: --emit-holdings directory must not exist");
    dir_ = dir; format_ = format; ids_ = ids; hold_ = hold_state;
    days_.open(dir / holdings::days_file, std::ios::binary);
    if (!days_) return co::Err(co::ErrorCode::IoError, "nav replay: holdings output");
    days_.imbue(std::locale::classic()); days_ << std::setprecision(17);
    days_ << holdings_days_columns << '\n';
    if (format_ == NavHoldingsFormat::F64)
      return binary_.open(dir / holdings::data_file,
                          hold_ ? holdings::hold_row_width : holdings::row_width);
    names_.open(dir / "holdings.csv", std::ios::binary);
    if (!names_) return co::Err(co::ErrorCode::IoError, "nav replay: holdings output");
    names_.imbue(std::locale::classic()); names_ << std::setprecision(17);
    names_ << detail::holdings_csv_columns();
    if (hold_)
      for (const char* column : holdings::hold_column_names) names_ << ',' << column;
    names_ << '\n';
    return co::Ok();
  }
  co::Status session(const NavReplayDay& d, std::span<const NavHolding> names) override {
    usize placed = 0;
    if (format_ == NavHoldingsFormat::F64) {
      const usize ordinal = table_.size();
      for (const auto& h : names) {
        if (hold_) ATX_TRY_VOID(binary_.append(holdings::pack_hold(ordinal, h)));
        else ATX_TRY_VOID(binary_.append(holdings::pack(ordinal, h)));
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
    ATX_TRY(index_sha_,
            holdings::write_index(dir_ / holdings::index_file, ids_, table_, data_, hold_));
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
  [[nodiscard]] bool hold_state() const { return hold_; }
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
    write_value(f, h.order_dollars);
    if (hold_) {
      f << ','; write_value(f, h.rank_set);
      f << ','; write_value(f, h.desired_prev);
    }
    f << '\n';
  }
  std::filesystem::path dir_;
  NavHoldingsFormat format_{NavHoldingsFormat::F64};
  bool hold_{}; // v8 E-16: a declared hold band's state columns
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
                            std::ostream& progress);
// Command-line numbers: the whole value, else std::invalid_argument.
f64 parse_real(const std::string& value);
u64 parse_integer(const std::string& value);
// The construction flags (nav_grid_variant_flags), parsed exactly as the command line
// parses them (the command line calls this); false for any other key. A bad value throws
// std::invalid_argument (std::out_of_range from std::stod).
bool apply_construction_flag(const std::string& key, const std::string& value,
                             TargetReplayConfig& target);
// A construction grid file (bounded, schema-checked, string flag values) and its SHA,
// under the engine's variant rules: ids, allowed keys, at most nav_max_grid_variants.
struct GridSpec {
  std::vector<par::GridVariant> variants;
  std::string sha256;
};
co::Result<GridSpec> read_grid(const std::string& path);
} // namespace atx::impl::strategy::nav_book
