#pragma once

#include <array>
#include <iosfwd>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include "strategy_target_replay.hpp"

namespace atx::impl::strategy {
// Self-financing marked-dollar NAV replay of one pinned saved blend under the
// same target rules as replay_targets (baseline-v1 / monthly-budget-v2 /
// aim-partial-v5).
//
// Timing: decide at session d (after its mark), fill at session d+1's close,
// first return row d+2. Decisions [begin, end-2); executions <= end-2; return rows
// [begin+2, end); the final session is valuation only. Results never depend on
// rows at or beyond decision_end (they are only contract-validated); session t
// reads only rows <= t (liquidity: [t-w, t)).
// Order per session t: MARK (financing on pre-mark dollars, drift, stale/write-off)
// -> EXECUTE (working orders at close t, costs to cash) -> DECIDE (targets in
// decision-NAV dollars). Cash earns 0%, short proceeds stay in cash, no rebate:
// excess-return accounting, the benchmark cancelling against collateral.
//
// Missing prices are detected only from present[t]. A held absent name is carried
// at its last observed adjusted close with its orders blocked; a reprint within K
// sessions realizes the true cumulative return and then executes pending orders;
// K consecutive absences write it off at last mark x (1 + haircut). A reprint after
// a write-off is a diagnostic event only. No lookahead anywhere.
//
// Construction (TargetReplayConfig neutralize / band_multiple, and aim-partial-v5's
// dust band and aim leverage) is the target replay's own, applied at the NAV path's
// single desired-target extension point; with the defaults every pre-existing output
// value is unchanged. aim-partial-v5 may trade at a per-name rate (NavRateRule).

enum class NavCostRule : atx::u8 { FlatBpsV1 = 1, SqrtImpactV1 = 2 };

// Adverse terminal haircuts of scenario S3: copies of the engine's causal default
// book::assumed_missing_price_return(ListingExchange::Unknown, side) (Shumway
// -0.55 long / +0.30 short, both losses). Parity is asserted by the NAV tests.
inline constexpr atx::f64 nav_adverse_long_return = -0.55;
inline constexpr atx::f64 nav_adverse_short_return = 0.30;
// Owner targets reported (never optimized against) in every summary.
inline constexpr atx::f64 nav_sharpe_target = 1.0;
// LEGACY (retired 2026-09-27): monthly turnover is reported for continuity only.
inline constexpr atx::f64 nav_monthly_turnover_target = 0.30;
// Declared daily one-way turnover ceilings in GMV units (owner ruling 2026-09-27):
// tau_t = sum|fill$| / pre-trade (long$ + short$), deployment session excluded,
// forced exits included. Recorded in every recipe; CLI-overridable.
inline constexpr atx::f64 nav_daily_turnover_mean_max = 0.20;
inline constexpr atx::f64 nav_daily_turnover_p95_max = 0.30;
struct NavTurnoverLimits {
  atx::f64 daily_mean_max{nav_daily_turnover_mean_max};
  atx::f64 daily_p95_max{nav_daily_turnover_p95_max};
};

// Financing of one scenario book, accrued at MARK t over (t-1, t] on pre-mark
// dollars x calendar days / day_count (the same basis the legacy borrow used):
// - FlatShortV0: flat_short_bps on every short dollar, day_count 365, no long leg,
//   no tiers, no locate rule: the legacy accrual, bit for bit.
// - TieredSwapV1 (prime-broker portfolio swap): long_spread_bps on long dollars plus
//   (short_spread_bps + tier fee) on each short dollar, the fee (gc/warm/special_bps)
//   from the name's borrow tier at the latest decision <= t-1 (so a short that
//   migrates into special pays special until it exits). block_special_shorts: at
//   DECIDE a special-tier name may not open or grow a short,
//   next = max(next, min(cur, 0)); reductions and exits pass.
enum class NavFinancingRule : atx::u8 { FlatShortV0 = 0, TieredSwapV1 = 1 };
struct NavFinancing {
  std::string id{"flat-300-v0"};
  NavFinancingRule rule{NavFinancingRule::FlatShortV0};
  atx::f64 flat_short_bps{300};                   // FlatShortV0 only (0 otherwise)
  atx::f64 long_spread_bps{}, short_spread_bps{}; // TieredSwapV1 only
  atx::f64 gc_bps{}, warm_bps{}, special_bps{};   // TieredSwapV1 only, gc <= warm <= special
  atx::u32 day_count{365};                        // 360 | 365 (FlatShortV0: 365)
  bool block_special_shorts{};                    // TieredSwapV1 only
};
// Declared financing scenarios (owner ruling 2026-09-27, handoff 2 section 2b):
// [0] swap-fin-v1 PRIMARY (long 40, short spread 20, tiers 30/100/500 bps, ACT/360,
// locate block), [1] flat-300-v0 (legacy), [2] engine-tiers-v1 (swap-fin-v1 with
// the engine BorrowTierRecipe default fees 27.5/300/2750 bps).
[[nodiscard]] std::vector<NavFinancing> nav_financing_scenarios();

struct NavScenario {
  std::string id; // the trading scenario; the financing carries its own id
  NavCostRule cost{NavCostRule::FlatBpsV1};
  atx::f64 flat_bps{};          // FlatBpsV1 per-dollar rate
  atx::f64 half_spread_bps{};   // SqrtImpactV1 liquidity-row half spread
  atx::f64 commission_bps{};    // SqrtImpactV1 commission
  atx::f64 impact_y{}, impact_delta{0.5};
  atx::f64 max_participation{}; // SqrtImpactV1 cap per session (fraction of ADV)
  NavFinancing financing{};     // default: flat-300-v0
  atx::f64 fallback_daily_vol{0.05};
  atx::usize stale_exit_sessions{5}; // K consecutive absent marks -> write-off
  bool adverse_terminal{};           // false: haircut 0; true: nav_adverse_* returns
};
// Fixed research trading scenarios, each with flat-300-v0 financing, in this order:
// S1 linear-6bps-stale5-v1, S2 modeled-1bn-stale5-v1 (PRIMARY),
// S3 modeled-1bn-terminal-adverse-v1.
[[nodiscard]] std::vector<NavScenario> fixed_nav_scenarios();
inline constexpr atx::usize nav_primary_scenario_index = 1;
// The books of one run. Without borrow fields: fixed_nav_scenarios() (flat-300-v0).
// With them: S1/S2/S3 x swap-fin-v1, then S2 x flat-300-v0 and S2 x engine-tiers-v1.
// Either way the primary is nav_primary_scenario_index (S2, swap-fin-v1 if tiered).
[[nodiscard]] std::vector<NavScenario> nav_scenario_matrix(bool tiered);

// Trading rate of the aim-partial-v5 move (T36, pre-registered rate per-name-v1).
// Fixed: every member moves by the target's trade_fraction (theta). PerNameV1: member
// i of decision d moves by per_name_rate_v1 at NAV = the book's pre-trade NAV at d and
// sigma_i / ADV_i = its liquidity row of session d (window [d-w, d), the execution
// liquidity definition); a name with ADV <= 0, sigma <= 0 or fewer than min_vol_pairs
// return pairs (the fallback sigma) gets rate_min, never NaN.
enum class NavRateRule : atx::u8 { Fixed = 0, PerNameV1 = 1 };
// Declared per-name-v1 defaults (research brief 4.B): RRA 10, JKMP lambda 0.2 (0.1%
// impact at 1% of ADV), clip [0.01, 0.15].
inline constexpr atx::f64 nav_rate_rra = 10.0, nav_rate_lambda = 0.2;
inline constexpr atx::f64 nav_rate_min = 0.01, nav_rate_max = 0.15;
// theta = clip(sqrt(rra * daily_vol^2 * adv_dollars / (lambda * nav)), rate_min, rate_max):
// Garleanu-Pedersen (2013) partial adjustment under quadratic cost with the JKMP impact
// calibration Lambda = lambda / ADV and risk aversion RRA / NAV; daily_vol a daily SD,
// adv_dollars raw dollars. rra, lambda, nav, daily_vol or adv_dollars not finite and
// > 0 -> rate_min; a NaN quotient -> rate_min; never NaN. Precondition (validated with
// the config): 0 < rate_min <= rate_max.
[[nodiscard]] atx::f64 per_name_rate_v1(atx::f64 rra, atx::f64 lambda, atx::f64 nav,
                                        atx::f64 daily_vol, atx::f64 adv_dollars,
                                        atx::f64 rate_min, atx::f64 rate_max) noexcept;
// What a working order fixes (v6 prereg C1).
// Target (default): the decision-NAV dollar target planned x NAVpost; EXECUTE requests
//   target - held, so one-day price drift between decision and fill is traded back.
// Delta: a nonzero plan's order is the decision-NAV dollar delta
//   (planned - current) x NAVpost = planned x NAVpost - held_d; EXECUTE requests that
//   delta minus the dollars already filled on it, so the drift rides and the next DECIDE
//   re-plans from the drifted holding at theta. A complete fill lands on the target plus
//   the drift since the decision; a capped residual keeps its remaining delta (never
//   drift-adjusted) until filled, replaced by a decision that changes the name's plan, or
//   cancelled. A zero plan (every exit) and, under the locate rule, an order on a
//   special-tier name stay target orders. Without drift Delta is Target bit for bit.
enum class NavOrderBasis : atx::u8 { Target = 0, Delta = 1 };
struct NavReplayConfig {
  TargetReplayConfig target{}; // one_way_bps and annual_borrow_bps must be zero
  NavScenario scenario{};
  atx::f64 initial_nav{1'000'000'000.0};
  atx::usize liquidity_window{63}, min_vol_pairs{20};
  atx::u64 max_events{262'144}; // explicit refusal beyond
  // PerNameV1 requires target.rule aim-partial-v5, rate_rra and rate_lambda in (0, 1e6]
  // and 0 < rate_min <= rate_max <= 1. Fixed requires every rate_* at its default, so a
  // fixed-rate recipe cannot silently carry rate parameters.
  NavRateRule rate{NavRateRule::Fixed};
  atx::f64 rate_rra{nav_rate_rra}, rate_min{nav_rate_min}, rate_max{nav_rate_max};
  atx::f64 rate_lambda{nav_rate_lambda};
  NavOrderBasis order_basis{NavOrderBasis::Target};
  // Locate-in-aim (v6 prereg C3): at each rebalance decision a member in the special borrow
  // tier of that decision gets no negative desired weight BEFORE the neutralization
  // (detail::form_desired no_short), shared by every book; each book's post-rule
  // locate block stays as a safety net. Under the industry ids those aims are also held
  // at 0 through the group demeaning (review I3). Requires the borrow fields and a
  // neutralizing construction (target.neutralize != None).
  bool locate_in_aim{};
  // Execution liquidity from the shared per-session cache also at a fixed rate (v6 review F8):
  // each execution session forms every working order's window once for all books
  // instead of once per book. The same arithmetic, so every output is bit-identical
  // (asserted by the tests); only work and 24 bytes per name of admission change.
  bool liquidity_cache{};
  // Warm start (v8 D-0, review C-7), K sessions: every book decides and trades from role
  // row decision_begin - K (never before row 0: K <= decision_begin, else InvalidArgument)
  // under this config's own rules, costs and financing. The MARK of row decision_begin
  // closes the warm-up; each book is then resized to initial_nav (holdings, cash, working
  // orders, delta anchors and written-off exposures scaled by one factor) and every
  // reported quantity restarts there: rows, events, buckets, participation, rate
  // statistics and accounting checks cover rows >= decision_begin only. Row
  // decision_begin is the scored base row (its warm-up MARK is not reported; its fills
  // and decision are), return rows are [decision_begin + 1, decision_end), and a
  // deployment during the warm-up leaves deployment_index before the first row. The
  // cadence phase stays relative to decision_begin. 0 (default): the flat start, every
  // output bit for bit.
  atx::usize warm_start_sessions{};
  // Books on a deterministic pool (v8 D-1): each session's per-book phases (MARK and
  // EXECUTE; then DECIDE, close and report) run on book_workers threads around the shared
  // decision, which stays on the calling thread. Every book's arithmetic is its own and the
  // shared state is read-only inside a phase, so every output is bit-identical to 1 (the
  // default: sequential, today's loop). 1..64; above 1 refused with rate per-name-v1 (one
  // shared rate buffer) and while a v7 extension is installed (its hook is thread-local).
  atx::usize book_workers{1};
};
// The v6 execution options of a run_nav_replay call (copied into its NavReplayConfig;
// CLI --order-basis target|delta, --locate-in-aim, --liquidity-cache; v8
// --warm-start-sessions K, --book-workers N).
struct NavExecutionOptions {
  NavOrderBasis order_basis{NavOrderBasis::Target};
  bool locate_in_aim{}, liquidity_cache{};
  atx::usize warm_start_sessions{};
  atx::usize book_workers{1};
};
// The trading rate of a run_nav_replay call (copied into its NavReplayConfig; CLI
// --rate fixed|per-name-v1, --rate-rra, --rate-lambda, --rate-min, --rate-max).
struct NavRateOptions {
  NavRateRule rate{NavRateRule::Fixed};
  atx::f64 rate_rra{nav_rate_rra}, rate_min{nav_rate_min}, rate_max{nav_rate_max};
  atx::f64 rate_lambda{nav_rate_lambda};
};
// Point-in-time role fields (atx.research-role-fields/v1: date-major, role dates x
// ids, NaN = not visible by the session's 22:00 UTC mark) behind the borrow tiers.
// Both empty (no tiers) or both dates x instruments; any value is admitted.
struct NavFinancingFields {
  std::span<const atx::f64> shares_out, si_shares;
};
// Borrowed for the synchronous call. Prices and volume are required; members must
// be present. Present cells: close/raw finite > 0, volume finite >= 0. volume is
// authoritative (it also feeds price-risk neutralization); target.volume is ignored.
// target.industry (grp_ff12 ids) is required by, and only read by, the industry ids.
// A TieredSwapV1 scenario requires the financing fields; with them every book also
// reports its short dollars by tier.
struct NavReplayInput {
  TargetReplayInput target;
  std::span<const atx::f64> volume;
  NavFinancingFields financing{};
};

// Borrow tier of every name at decision d, from rows <= d only (formed once per
// decision and shared by every book; the fees are each scenario's own):
// engine estimate_borrow_tier (PublicPredictorPriorV1, default thresholds) with
//   market cap = shares_out[d] x raw_close[d] (mktcap_lagged is not used: its
//     presence is not point in time),
//   raw price = raw_close[d], SI ratio = si_shares[d] / shares_out[d] (shares_out
//     >= float, so the ratio is understated),
//   IPO age = calendar days since the name's first present role session (present
//     at role session 0: seasoned),
//   available_at = session + 22h (the fields' visibility mark), decision = +23h.
// shares_out outside [nav_shares_out_min, nav_shares_out_max], a non-finite or
// negative si_shares, an absent name, or an engine Unavailable -> Warm, missing.
inline constexpr atx::f64 nav_shares_out_min = 1e5, nav_shares_out_max = 5e10;
struct NavBorrowTiers {
  std::vector<atx::u8> tier;    // engine BorrowTier value: 1 GC, 2 warm, 3 special
  std::vector<atx::u8> missing; // 1: a predictor was missing (charged warm)
};
[[nodiscard]] atx::core::Result<NavBorrowTiers> nav_borrow_tiers(const NavReplayInput& in,
                                                                atx::usize d);

// One row per session t in [decision_begin, decision_end). Row decision_begin
// carries only the first decision (under a warm start: the base row, with the fills of
// the last warm-up decision and its own decision, no mark). Return fields are relative
// to the previous row's pre-trade NAV: net = gross - trade_cost - borrow - long_financing,
// where trade cost is the PREVIOUS session's fills (costs of fills at t land in the
// return of t+1) and borrow is the whole short financing leg (flat rate, or short
// spread + tier fee). rebalance is effective (a cadence decision not skipped by the
// neutralize guard).
struct NavReplayDay {
  atx::usize session_index{};
  atx::i64 session{};
  atx::u32 calendar_month{}; // YYYYMM of this session; the execution month of its fills
  bool decision{}, rebalance{}, executed{}, return_observation{};
  atx::f64 pretrade_nav{}, posttrade_nav{};
  atx::f64 net_return{}, gross_return{}, writeoff_return{}, trade_cost_return{}, borrow_return{};
  atx::f64 mark_pnl_dollars{}, writeoff_dollars{}, borrow_dollars{};
  atx::f64 traded_dollars{}, one_way_turnover{}; // turnover = traded / pre-trade NAV
  atx::f64 trade_cost_dollars{}, linear_cost_dollars{}, impact_cost_dollars{};
  atx::f64 unrationed_cost_dollars{}, unfilled_dollars{};
  atx::usize fills{}, capped_fills{}, blocked_absent{}, blocked_liquidity{};
  atx::usize fallback_vol_fills{}, unrationed_unpriced{};
  atx::f64 planned_turnover{}, planned_forced{}, planned_discretionary{}, applied_fraction{};
  atx::f64 planned_gross{}, planned_net{}; // planned weights after the decision
  // Decision rows: nonzero planned weights and the decision's members N_d (feed the
  // aim-partial-v5 construction.v5 summary; not CSV columns).
  atx::usize planned_held_names{}, decision_members{};
  atx::f64 month_planned{}, budget_excess{}; // decision-month planned turnover (v2 budget)
  atx::f64 long_dollars{}, short_dollars{}, gross_leverage{}, net_leverage{};
  atx::usize held_names{}, stale_names{};
  atx::f64 stale_long_dollars{}, stale_short_dollars{};
  atx::usize guarded_intervals{};
  atx::f64 cash_ratio{}; // cash / post-trade NAV at end of session
  // GMV turnover: pre-trade gross = sum |held| after MARK, before EXECUTE (stale
  // names at stale marks); one_way_turnover_gmv = traded / that gross on execution
  // sessions (NaN when the pre-trade gross is 0, e.g. deployment), 0 otherwise.
  atx::f64 pretrade_gross_dollars{}, one_way_turnover_gmv{};
  ConstructionDay construction{}; // decision rows only
  // Financing at MARK t on pre-mark dollars. The by-tier arrays (GC, warm, special)
  // split the short leg and are filled whenever the replay has borrow tiers,
  // whatever the rule; a short whose predictors were missing is charged warm.
  atx::f64 long_financing_dollars{}, long_financing_return{};
  std::array<atx::f64, 3> short_dollars_by_tier{}, short_financing_by_tier{};
  atx::usize missing_predictor_shorts{};
  atx::f64 missing_predictor_short_dollars{};
  // DECIDE: short growth refused by the locate rule (planned weights in decision-NAV
  // dollars; a kept working order in its own dollars), and the decision's member
  // census by tier (GC, warm, special) with the members missing a predictor.
  atx::usize blocked_short_names{};
  atx::f64 blocked_short_dollars{};
  std::array<atx::usize, 3> member_tiers{};
  atx::usize member_missing_predictors{};
};
enum class NavEventKind : atx::u8 {
  GapResolved = 1, WriteOff = 2, Guarded = 3, ReappearedAfterWriteOff = 4, UnresolvedAtEnd = 5
};
struct NavEvent {
  NavEventKind kind{NavEventKind::GapResolved};
  bool short_side{};
  atx::usize run_length{};
  atx::i64 session{};
  atx::u64 instrument_id{};
  atx::f64 exposure{}; // signed dollars at the event's pre-event mark
  atx::f64 r_adj{}, r_raw{}, haircut{}, pnl{}; // NaN where not observable
};
struct NavBucket {
  atx::usize count{};
  atx::f64 gross_exposure{}, pnl{};
};
// One book's per-name rates (PerNameV1) over the members of every decision row: one
// sample per member per decision, at that book's own NAV. at_min_count / at_max_count
// count samples equal to rate_min / rate_max (a sample counts in both when they are
// equal; no-liquidity names are at_min); share_* = count / n. Quantiles at rank
// ceil(q n): exactly rate_min / rate_max when the rank falls in those masses, else the
// upper edge of its bin among 4096 equal bins of [rate_min, rate_max], capped at max
// (error < (rate_max - rate_min) / 4096). Fixed rate: every field zero. PerNameV1 with
// n == 0: mean, min, max, quantiles and shares NaN.
struct NavRateStats {
  atx::usize n{}, at_min_count{}, at_max_count{};
  atx::f64 mean{}, min{}, max{}, p05{}, p50{}, p95{}, share_at_min{}, share_at_max{};
};
struct NavConstructionStats {
  NavRateStats rate_stats{};
};
struct NavReplayResult {
  std::vector<NavReplayDay> days;
  std::vector<NavEvent> events;
  NavBucket gap_run_1, gap_run_2_4, gap_run_5_plus, written_off, reappeared, unresolved, guarded;
  atx::f64 guard_sensitivity{}; // sum h * (r_raw - r_adj) over realized guarded intervals
  atx::u64 participation_fills{};
  atx::f64 participation_p95{}, participation_max{}; // p95: 0.01-decade histogram upper edge
  atx::f64 max_return_identity_error{}, max_cash_book_error{};
  // The first session with a nonzero fill; decision_end sentinel when nothing ever filled.
  // Under a warm start it precedes decision_begin when the book deployed in the warm-up.
  atx::usize deployment_index{};
  NavConstructionStats construction{}; // aim-partial-v5 per-name rate statistics
};
[[nodiscard]] atx::core::Result<NavReplayResult> replay_nav(const NavReplayInput& in,
                                                          const NavReplayConfig& cfg);
// Several scenarios over one input in lockstep: each decision's desired target
// (and, for price-risk-v1, its price exposures) is formed ONCE and shared by every
// scenario book; the books are otherwise independent. results[k] is bit-identical
// to replay_nav(in, base with scenario = scenarios[k]). 1 <= scenarios <= 8.
[[nodiscard]] atx::core::Result<std::vector<NavReplayResult>> replay_nav_scenarios(
    const NavReplayInput& in, const NavReplayConfig& base, std::span<const NavScenario> scenarios);

// ---- per-name holdings of one book (--emit-holdings, v7 B3) ----
// EXECUTE outcome of one name's working order at session t.
enum class NavFillStatus : atx::u8 {
  None = 0,             // no working order was priced at t
  Complete = 1,         // filled in full
  Capped = 2,           // participation-capped partial fill; the residual keeps working
  BlockedAbsent = 3,    // absent at t: nothing filled, the order persists
  BlockedLiquidity = 4, // unusable ADV at t: nothing filled, the order persists
};
// One name of the observed book at the end of session t (after MARK, EXECUTE, DECIDE).
// A name is reported when any of held, the decision's plan (bitwise, so -0 counts), a
// working order or an EXECUTE outcome is nonzero. held_* is what DECIDE at t read:
// held_weight = held_dollars / post-trade NAV_t, the same expression as the plan's current
// weight, so decide --asof t fed these dollars and that NAV reproduces target_weight.
struct NavHolding {
  atx::usize index{};               // instrument index in the role
  atx::u64 instrument_id{};
  bool member{}, stale{};            // member of session t; held and absent at t
  atx::u8 tier{}, tier_missing{};    // BorrowTier of the latest decision (0: no tiers)
  atx::f64 held_dollars{}, held_weight{};
  NavFillStatus fill{NavFillStatus::None};
  atx::f64 filled_dollars{}, fill_cost_dollars{}, unfilled_dollars{}; // EXECUTE at t
  // DECIDE at t (NaN / false on the final, execution-only session):
  atx::f64 desired{};       // shared desired target (effective rebalance; NaN otherwise)
  atx::f64 rule_weight{};   // the target rule's plan, before the locate block
  atx::f64 target_weight{}; // the book's planned weight (after the locate block)
  bool order_placed{};      // target_weight != held_weight: DECIDE placed a new order
  bool locate_blocked{};    // the locate block changed the rule's plan
  bool order_working{};     // a working order is active after DECIDE
  atx::f64 order_dollars{}; // its decision-NAV dollars (NaN when none)
};
// Receives the observed book once per decision or execution session t, after the book
// closed t (`day` is that book's NavReplayDay row); `names` ascend by index. An error
// aborts the replay.
class NavHoldingsSink {
public:
  NavHoldingsSink() = default;
  NavHoldingsSink(const NavHoldingsSink&) = delete;
  NavHoldingsSink& operator=(const NavHoldingsSink&) = delete;
  virtual ~NavHoldingsSink() = default;
  [[nodiscard]] virtual atx::core::Status session(const NavReplayDay& day,
                                                  std::span<const NavHolding> names) = 0;
};
// replay_nav_scenarios with book `observed` (< scenarios.size()) reported to `sink`.
// Observation only: every result is bit-identical to the overload without a sink.
[[nodiscard]] atx::core::Result<std::vector<NavReplayResult>> replay_nav_scenarios(
    const NavReplayInput& in, const NavReplayConfig& base, std::span<const NavScenario> scenarios,
    NavHoldingsSink& sink, atx::usize observed);

// Construction grid (v8 D-1): every variant runs every scenario, all variants x scenarios
// books in lockstep over the one input with ONE shared construction per decision (the
// desired target and its price exposures, the borrow tiers, the liquidity windows), formed
// on every session that is a cadence decision of some variant. results[v][k] is
// bit-identical to replay_nav_scenarios(in, variants[v], scenarios)[k]. The variants may
// differ only in the target construction keys of nav_grid_variant_flags (rule, cadence,
// trade_fraction, monthly_budget, band_multiple, dust_multiple, aim_leverage, exit_rate);
// any other difference (the v8 hold_band and adv_hold_q included) is InvalidArgument, and
// with a hold band every variant has the base's cadence (the band's state advances on the
// shared cadence decisions). 1 <= variants <= nav_max_grid_variants; the workspace budget is
// charged for every book.
inline constexpr atx::usize nav_max_grid_variants = 16;
[[nodiscard]] atx::core::Result<std::vector<std::vector<NavReplayResult>>> replay_nav_grid(
    const NavReplayInput& in, std::span<const NavReplayConfig> variants,
    std::span<const NavScenario> scenarios);
// The CLI flags a grid variant may set (each parsed exactly as on the nav command line).
inline constexpr std::array<std::string_view, 8> nav_grid_variant_flags{
    "--rule", "--cadence", "--trade-fraction", "--monthly-budget", "--band-multiple",
    "--dust-multiple", "--aim-leverage", "--exit-rate"};

struct NavMonth {
  atx::u32 month{};
  atx::usize execution_sessions{}, traded_sessions{}, decision_sessions{};
  atx::f64 one_way_turnover{}, traded_dollars{}, planned_turnover{};
  bool is_deployment_month{};
};
struct NavYear {
  atx::i32 year{};
  atx::usize observations{};
  atx::f64 net_return{};
};
struct NavSummary {
  atx::usize observations{};
  atx::f64 net_sharpe{}, gross_sharpe{}, mean_daily_net{}, ann_mean{}, ann_vol{}, cagr{};
  atx::f64 max_drawdown{}, total_net_return{}, final_nav{};
  bool hac_defined{};
  atx::f64 hac_t{}; // Bartlett lag 5, small-sample corrected; NaN when undefined
  atx::usize hac_lag{};
  std::vector<NavYear> years;
  std::vector<NavMonth> months; // every month with an execution or a decision session
  atx::usize execution_months{};
  atx::f64 mean_monthly_turnover{}, max_monthly_turnover{};
  atx::f64 mean_monthly_turnover_ex_deployment{}, max_monthly_turnover_ex_deployment{};
  atx::usize months_within_target{}, months_within_target_ex_deployment{};
  bool deployed{};
  atx::i64 deployment_session{};
  atx::f64 deployment_turnover{}, deployment_dollars{};
  atx::f64 total_actual_turnover{}, total_planned_turnover{};
  atx::f64 trade_cost_dollars{}, linear_cost_dollars{}, impact_cost_dollars{};
  atx::f64 unrationed_cost_dollars{}, borrow_dollars{}, writeoff_dollars{};
  atx::f64 summed_trade_cost_return{}, summed_borrow_return{}, summed_writeoff_return{};
  atx::usize fills{}, capped_fills{}, blocked_absent{}, blocked_liquidity{};
  atx::usize fallback_vol_fills{}, unrationed_unpriced{};
  atx::f64 unfilled_dollars{};
  atx::f64 mean_gross_leverage{}, max_gross_leverage{}, max_abs_net_leverage{};
  atx::f64 mean_held_names{}, mean_stale_names{}, max_stale_gross_fraction{};
  atx::usize max_stale_names{};
  atx::f64 min_cash_ratio{};
  bool meets_sharpe_target{}, meets_turnover_target_mean{}; // monthly flags: LEGACY
  bool meets_turnover_target_mean_ex_deployment{};
  bool meets_turnover_target_all_months{}, meets_turnover_target_all_months_ex_deployment{};
  // Daily one-way turnover in GMV units over execution sessions with a positive
  // pre-trade gross, the deployment session excluded (forced exits included; every
  // fill counts, planned turnover never does). Quantiles interpolate linearly at
  // (n-1)q over the sorted sessions (numpy default); NaN when no session qualifies.
  NavTurnoverLimits daily_limits{};
  atx::usize daily_turnover_sessions{}, daily_turnover_zero_gmv_sessions{};
  atx::f64 daily_turnover_mean{}, daily_turnover_median{}, daily_turnover_p95{};
  atx::f64 daily_turnover_max{};
  atx::i64 daily_turnover_max_session{};
  bool meets_daily_turnover_mean{}, meets_daily_turnover_p95{}; // NaN never meets
  // Financing: the long leg; the short leg (borrow_dollars) by tier; each tier's
  // share of pre-mark short dollars over return rows with shorts (mean, p95; NaN
  // without tiers or shorts); missing predictors; locate blocks; net exposure over
  // return rows (the previous session's closing book: the block can un-neutralize).
  atx::f64 long_financing_dollars{}, summed_long_financing_return{};
  std::array<atx::f64, 3> short_financing_by_tier{}, short_share_mean{}, short_share_p95{};
  atx::usize short_share_sessions{}, missing_predictor_short_name_days{};
  atx::usize missing_predictor_member_days{}, blocked_short_name_decisions{};
  std::array<atx::usize, 3> member_tier_days{};
  atx::f64 blocked_short_dollars{}, mean_net_leverage{}, mean_abs_net_leverage{};
};
// Return statistics over rows with return_observation; turnover by the calendar
// month of the EXECUTION session (all months include deployment; *_ex_deployment
// exclude the deployment month); planned turnover by decision month; daily GMV
// turnover against `limits`.
[[nodiscard]] atx::core::Result<NavSummary> summarize_nav(const NavReplayResult& result,
                                                          const NavTurnoverLimits& limits = {});

// Pinned role fields for the borrow tiers: the atx.research-role-fields/v1
// manifest.json path and its SHA-256. Both empty: no tiers (legacy flat-300-v0).
// The manifest's role pins (manifest, sessions, ids, member SHA) must equal the
// pinned --role, and shares_out and si_shares must be declared point_in_time true.
// The industry neutralization ids also load industry_group_field (grp_ff12) from
// it, under the same checks; they require the fields.
struct NavFieldsPin {
  std::string manifest_path, manifest_sha256;
};

// Workspace a pinned run reserves before it loads any payload (v6 C4): publication
// slack, every book's fixed workspace, per-name state, days and events (at the
// max_events cap), the shared construction (with its neutralization scratch and, when
// neutralizing, the price-exposure session ring of v8 D-1), the
// borrow tiers (tiered) and the shared liquidity cache (rate per-name-v1 or
// base.liquidity_cache), at the ACTUAL geometry: `names` instruments and `sessions` =
// score_end - score_begin rows per book, read from the pinned role manifest.
// run_nav_replay refuses (OutOfRange) when max_working_bytes <= this reserve and
// charges the fields and the saved-blend loader against the rest. `holdings`
// (--emit-holdings) adds the observed book's per-name trace and row buffer; false is
// the reserve without it, byte for byte.
[[nodiscard]] atx::u64 nav_workspace_reserve_bytes(const NavReplayConfig& base,
                                                   atx::usize books, bool tiered,
                                                   atx::usize names, atx::usize sessions,
                                                   bool holdings = false);
// Pinned saved blend + role (with volume); every scenario of nav_scenario_matrix,
// run in lockstep; exclusive output directory: recipe.json, daily_<S>.csv,
// events_<S>.csv, summary.json LAST (S: the trading id, or "<trading>+<financing>"
// with fields). Everything, fields pins included, is checked and computed before
// the directory is created.
[[nodiscard]] atx::core::Status run_nav_replay(const TargetReplayRunConfig& cfg,
                                               std::ostream& progress);
[[nodiscard]] atx::core::Status run_nav_replay(const TargetReplayRunConfig& cfg,
                                               const NavTurnoverLimits& limits,
                                               std::ostream& progress);
[[nodiscard]] atx::core::Status run_nav_replay(const TargetReplayRunConfig& cfg,
                                               const NavTurnoverLimits& limits,
                                               const NavFieldsPin& fields,
                                               std::ostream& progress);
// With the trading rate: PerNameV1 adds rate = "per-name-v1", rate_rra, rate_lambda,
// rate_min and rate_max to the recipe and construction.v5.rate_stats to every scenario
// summary. NavRateOptions{} (Fixed) is exactly the four-argument overload.
[[nodiscard]] atx::core::Status run_nav_replay(const TargetReplayRunConfig& cfg,
                                               const NavTurnoverLimits& limits,
                                               const NavFieldsPin& fields,
                                               const NavRateOptions& rate,
                                               std::ostream& progress);
// With the v6 execution options: Delta adds order_basis / order_basis_rule to the recipe
// and order_basis to the summary; locate-in-aim adds locate_in_aim / locate_in_aim_rule
// to the recipe and locate_in_aim {zeroed_special_short_aims} to the summary; the
// liquidity cache adds nothing (every output byte is unchanged). A warm start K > 0 adds
// warm_start_sessions / warm_start_rule to the recipe and warm_start {sessions,
// first_decision_session_ns, scoring_begins_session_ns} to the summary; it is refused
// (InvalidArgument, before any payload is loaded) when K exceeds the pinned role's
// score_begin. NavExecutionOptions{} is exactly the five-argument overload.
[[nodiscard]] atx::core::Status run_nav_replay(const TargetReplayRunConfig& cfg,
                                               const NavTurnoverLimits& limits,
                                               const NavFieldsPin& fields,
                                               const NavRateOptions& rate,
                                               const NavExecutionOptions& execution,
                                               std::ostream& progress);
// --emit-holdings NEWDIR (v7 B3): the PRIMARY book's per-name rows and per-session
// summary (holdings_days.csv) for every decision or execution session, streamed while
// the replay runs, then manifest.json (file SHAs, the NAV recipe SHA) LAST, after the NAV
// directory is published; a directory without manifest.json is incomplete. Both
// directories must not exist and must differ. The NAV directory is byte-identical with or
// without it, in either format; NavEmitOptions{} (empty directory) is exactly the
// six-argument overload.
// Formats (v7 W4, finding L3-F1: the v1 CSV text cost ~3x the NAV run's wall time):
//   F64 (default): holdings.f64 + holdings_index.json (strategy_holdings.hpp), manifest
//       atx.nav-holdings/v2; every holdings.csv column recoverable bit for bit.
//   Csv: holdings.csv, manifest atx.nav-holdings/v1, byte for byte the L3 output.
enum class NavHoldingsFormat : atx::u8 { F64 = 0, Csv = 1 };
// stage_timers (v8 D-1, CLI --stage-timers): summary.json gains stage_seconds {load,
// exposures, construction, books, hash, write, wall, definition}; the six stages partition
// wall (entry to the summary.json write). Off (default): no stage_seconds key, so
// summary.json stays reproducible byte for byte (the clocks read in the replay are
// observation only: no published value depends on them); every other file is unchanged
// either way.
struct NavEmitOptions {
  std::string holdings_directory;
  NavHoldingsFormat format{NavHoldingsFormat::F64};
  bool stage_timers{};
};
[[nodiscard]] atx::core::Status run_nav_replay(const TargetReplayRunConfig& cfg,
                                               const NavTurnoverLimits& limits,
                                               const NavFieldsPin& fields,
                                               const NavRateOptions& rate,
                                               const NavExecutionOptions& execution,
                                               const NavEmitOptions& emit,
                                               std::ostream& progress);
// argv[0] is the "nav" verb. Rejects --one-way-bps / --annual-borrow-bps.
// --fields PATH/manifest.json --fields-sha256 SHA enables the financing matrix.
// --rate fixed|per-name-v1 is refused (usage error) unless --rule aim-partial-v5, and
// --rate-rra/--rate-lambda/--rate-min/--rate-max unless --rate per-name-v1.
// v6: --order-basis target|delta, --exit-rate R (TargetReplayConfig::exit_rate), and
// the valueless flags --locate-in-aim and --liquidity-cache. v7: --emit-holdings NEWDIR
// [--holdings-format f64|csv]. v8: --warm-start-sessions K, --book-workers N, the valueless
// --stage-timers, and --construction-grid GRID.json (run_nav_grid).
[[nodiscard]] int dispatch_nav_replay(int argc, char** argv, std::ostream& out,
                                      std::ostream& err);

// --construction-grid (v8 D-1): the nav run of `cfg` (every other flag as parsed) for every
// variant of the grid file, all variants in one lockstep replay (replay_nav_grid) over one
// pinned load. Grid file: {"schema": "atx.nav-construction-grid/v1", "variants": [{"id":
// "<a-z0-9->", "flags": {"--trade-fraction": ".05", ...}}]}: each variant overrides only
// nav_grid_variant_flags, with string values parsed as on the command line. Output
// (exclusive): <output>/<id>/ byte for byte the directory the standalone nav run with the
// variant's flags publishes, then <output>/grid_manifest.json LAST (atx.nav-grid-run/v1:
// the grid file SHA, each variant's flags and file SHAs; stage_seconds with --stage-timers,
// which then stay out of the variant summaries). Refused with --emit-holdings and while a
// v7 extension is installed.
[[nodiscard]] atx::core::Status run_nav_grid(const TargetReplayRunConfig& cfg,
                                             const NavTurnoverLimits& limits,
                                             const NavFieldsPin& fields,
                                             const NavRateOptions& rate,
                                             const NavExecutionOptions& execution,
                                             const NavEmitOptions& emit,
                                             const std::string& grid_path,
                                             std::ostream& progress);
} // namespace atx::impl::strategy
