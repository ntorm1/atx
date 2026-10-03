#pragma once

// The platform-v7 (lane L4) extension of the NAV replay: ONE hook, installed per run by
// ScopedNavExtension and consulted by strategy_nav_replay.cpp at its v7 seams (dispatch
// entry and --help, scenario list, cost model, weight update (plan_weights, shared with the
// decide path), result capture, recipe/summary/holdings manifest). With no
// extension installed every seam is the identity, so default runs are byte-identical; the
// reserved-id cost model (cost_v2::reserved_cost_model) is a pure function of the scenario
// id and applies whenever such a scenario is replayed.
//
// CLI (the `nav` verb; any of these routes the run through dispatch_nav_v7):
//   --cost-v2                 adds the S2-KO and S2-FIM stress books (S1/S2/S3 unchanged)
//   --capacity-curve          the capacity books: S2 at NAV x {.5,1,2,4,8}, in the main lockstep
//                             (P9 C1; before: a second pass), published into <output>/capacity,
//                             then <output>/capacity_curve.csv
//   --rule aim-partial-v6     the cost-aware construction rule (base: aim-partial-v5 flags)
//     --cost-shrink-kappa K   (default 1)   --band-b B (default: --dust-multiple)
//     --band-exponent P       (default 1/3; 0 = the uniform dust band)
//     --rate-clip LO,HI       (default .5,1.5)
//   kappa 0 + --band-exponent 0 + --rate-clip 1,1 (band b = dust) is aim-partial-v5 bit for bit.
//   --rule spo-v1             the cost-aware single-period optimiser (wave 2 W1, strategy_spo.hpp;
//                             base: aim-partial-v5 flags) --risk-model DIR --risk-model-sha256 SHA
//                             (the `risk` verb's output with --emit-exposures all, same role)
//     [--gamma G] [--ic-book .02] [--w-max .01] [--adv-cap-q .05] [--adv-trade-p .01]
//     [--spo-iters 500] [--spo-tol 1e-8] [--target-vol .05] [--spo-horizon 1/theta]
//     [--spo-books all|primary] [--alpha-horizon h (v1: 1)] [--specific-ceiling (v1: inf)]
//     [--specific-ceiling-void on|off (v1: off)] [--spo-gross G (v1: --aim-leverage)];
//     adds <output>/spo_diagnostics.csv; fixed rate, no capacity curve, not with
//     aim-partial-v6. G is the hard cap on planned gross, refused outside (0, 1.5 x
//     --aim-leverage]; the aim-partial-v5 shadow book keeps --aim-leverage.
//   --rule spo-v2             spo-v1 with the fix-up 2 / W1b defaults (strategy_spo.hpp):
//                             alpha horizon 21 (independent of --spo-horizon), G 1.0,
//                             specific ceiling 1.0 with --specific-ceiling-void on, gamma =
//                             gamma_vol (gamma_bind report only). With the void on, a clamped
//                             entry at any decision makes the run VOID: after the replay and
//                             before anything is published (no recipe, NAV, daily, events or
//                             summary file), <output> gets spo_diagnostics.csv,
//                             v7_transfer_coefficient.csv and v7_extras.json (status "void")
//                             and the verb exits 3. --emit-holdings (which streams NAV during
//                             the replay) is refused with the void on.
//   --rule spo-v3 [--spo-alpha implied-aim]   target tracking toward the aim (platform v8
//                             R-6, strategy_spo_v3.hpp): no alpha vector, gamma = S_prior /
//                             sigma_aim (S_prior 20, Ruling E-14), H 20, trade limit .01 ADV,
//                             beta .02, no holding cap, gross above 2 x --aim-leverage a
//                             breach; its own spo_diagnostics.csv columns and tripwire (a
//                             clamp or a breach voids the run with the void on, the default;
//                             v8 E-31a: a scored decision of the primary book whose net or
//                             beta limit is not met voids it whatever the flag, so
//                             --emit-holdings is refused). Allowed: --risk-model(-sha256),
//                             --spo-books, --specific-ceiling(-void); every other spo flag
//                             (--spo-iters and --spo-tol included, E-31a) is refused.
//                             --spo-alpha is refused with spo-v1/v2. v8 E-26: the
//                             replay's --hold-band B / --adv-hold-q Q shape desired exactly as
//                             aim-partial-v5's (the shared construction, detail::form_desired),
//                             so the aim is L x the shaped desired and the rule id carries
//                             +hold-band-B / +adv-hold-Q; both refused with spo-v1/v2.
//                             v8 E-37: --capacity-curve runs report only: each capacity book
//                             is the NAV-m tracker (trade limit and impact at m x NAV) on its
//                             own engine, recorded in v7_extras.json capacity_spo_v3; the main
//                             pass's rows, tripwire, counts and summary are unchanged (review
//                             SPO-4: the capacity engine has no primary book, so E-31a binds the
//                             main pass only); the aim's --adv-hold-q cap reads each multiple's
//                             NAV, m x initial NAV (P9 C1; Ruling E-15 read the initial NAV at
//                             every multiple). --trade-fraction has no effect on the spo-v3 plan
//                             (H 20 is registered).
//   spo-v1 / spo-v2 / spo-v3 blocks are keyed "spo_v1" / "spo_v2" / "spo_v3" (recipe v7,
//   summary v7, extras); the Engine's rule_* / rows_* members produce them.
// --emit-holdings (lane L3) observes the main primary book only.
// Every hooked run also writes <output>/v7_transfer_coefficient.csv (TC per rebalance
// decision and book) and <output>/v7_extras.json (extras' SHA-256, capacity table, the
// capacity summary's SHA-256) before the replay's own summary.json, which binds v7_extras.json
// and capacity/summary.json (summary v7.files; P9 C1: before, they followed it unbound).
//   --risk-target S [--risk-target-bias 1.15] [--risk-target-cadence 21]   (platform v8 R-8,
//                             risk-target-v1, strategy_risk_target.hpp) with --risk-model DIR
//                             --risk-model-sha256 SHA (the spo rules' atx-risk-v1 store and
//                             check, shared with spo-v3): every book's aim leverage becomes
//                             L_t = clip(S / (b sigma_hat_t), .8 L, 1.25 L), sigma_hat_t the
//                             gross-1 current book's ex-ante vol, re-estimated every C sessions;
//                             aim-partial-v5 and spo-v3 only (aim-partial-v6, spo-v1/v2 and the
//                             other target rules refused); recipe.json, summary.json and the
//                             holdings manifest get a "risk_target" block and the rule id
//                             "+risk-target-S"; <output>/risk_target.csv (one row per scored
//                             decision and book: sigma_hat, L_t) beside v7_extras.json. Absent:
//                             nothing of it runs and no byte moves.
// Warm start (v8 D-0, --warm-start-sessions K): the decisions before the role's
// decision_begin plan the books but are not scored, so every v7 side file (the transfer
// coefficients, spo_diagnostics.csv, the spo summary blocks) and the spo tripwire cover
// decisions d >= decision_begin only. K = 0: every decision is scored, bytes unchanged.
//
// Threading (P9 C1, DEC-10): the installed extension is thread-local and is read on the calling
// thread only: the replay asks it for each book's state (make_book_state, configure) before the
// books run, and each replay book then plans through its own BookState and leverage rule
// (NavReplayConfig::leverage), so books on a pool (--book-workers) and construction grids run
// the v7 rules; only the spo engines hold every book's state (shared_plan_state: refused there).
// The loose seam v7::plan (the decide path, the tests) keeps a state per book label here.

#include <filesystem>
#include <iosfwd>
#include <memory>
#include <span>
#include <string>
#include <vector>
#include <nlohmann/json_fwd.hpp>
#include "atx/core/error.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/book/replay_cost.hpp"
#include "strategy_cost_v2.hpp"
#include "strategy_nav_replay.hpp"
#include "strategy_risk_target.hpp"
#include "strategy_spo.hpp"
#include "strategy_target_replay.hpp"

namespace atx::impl::strategy::v7 {

struct NavV7Options {
  bool stress{};   // --cost-v2
  bool capacity{}; // --capacity-curve
  bool aim_v6{};   // --rule aim-partial-v6
  cost_v2::AimV6Params v6{};
  bool spo_v1{};   // --rule spo-v1, spo-v2 or spo-v3 (spo_params.version tells which)
  spo::SpoParams spo_params{};
  // opened by dispatch_nav_v7; the spo rules' and the risk target's (one store, one check)
  std::shared_ptr<const spo::RiskStore> spo_risk;
  risk_target::Options risk_target{}; // --risk-target (v8 R-8)
};
enum class NavV7Pass : atx::u8 { Main = 0, Capacity = 1 };

// One rebalance decision of one book (main books only; strategy_nav_replay.hpp, which a replay
// book's NavReplayResult::transfer carries).
using TcRecord = NavTransferRecord;
// One replayed book (captured after the run, before publication).
struct BookRecord {
  NavV7Pass pass{NavV7Pass::Main};
  std::string book;
  bool primary{};
  atx::f64 multiple{};  // capacity multiple; NaN outside the capacity pass
  atx::f64 initial_nav{}, traded_dollars{}, trade_cost_dollars{};
  NavSummary summary{};
  std::vector<atx::f64> net_returns; // return-observation rows, in order
  atx::f64 participation_p95{}, participation_max{};
};

// RAII installation of the extension on this thread (restores the previous one).
class ScopedNavExtension {
public:
  explicit ScopedNavExtension(const NavV7Options& options);
  ~ScopedNavExtension();
  ScopedNavExtension(const ScopedNavExtension&) = delete;
  ScopedNavExtension& operator=(const ScopedNavExtension&) = delete;
  ScopedNavExtension(ScopedNavExtension&&) = delete;
  ScopedNavExtension& operator=(ScopedNavExtension&&) = delete;
  // Starts a replay pass: clears the per-run state (decision liquidity cache, the loose seam's
  // books, the spo engines' and the scaler's book states); records and captures accumulate
  // across passes. Capacity: every book a replay plans is a capacity book.
  void begin_run(NavV7Pass pass);
  [[nodiscard]] std::span<const TcRecord> tc_records() const noexcept;
  [[nodiscard]] std::span<const BookRecord> books() const noexcept;
  // The spo-v1 engine (nullptr without --rule spo-v1).
  [[nodiscard]] const spo::Engine* spo_engine() const noexcept;
  // spo-v3 --capacity-curve (Ruling E-37): the engine the capacity pass plans on (nullptr
  // without the spo rule and the capacity curve); spo_engine() keeps the main pass's rows.
  [[nodiscard]] const spo::Engine* spo_capacity_engine() const noexcept;
  // The risk-target-v1 scaler (nullptr without --risk-target): its records are the main books'
  // scored decisions (those the loose seam planned and those each replay handed back).
  [[nodiscard]] const risk_target::Scaler* risk_target_scaler() const noexcept;
  // Why capture() voided the run (the spo specific-ceiling tripwire); empty otherwise.
  [[nodiscard]] const std::string& void_reason() const noexcept;
  struct State;

private:
  std::unique_ptr<State> state_;
  State* previous_{};
};

// One replay book's v7 state (P9 C1, DEC-10), made on the calling thread by make_book_state and
// then touched by that book's lane only: the run's read-only state, the book's label and kind
// (a capacity book: the capacity pass, or a capacity id under --capacity-curve), its
// aim-partial-v6 reference-cost history and scratch, and its scored transfer coefficients
// (moved into its NavReplayResult::transfer when the replay ends).
struct BookState {
  ScopedNavExtension::State* run{};
  std::string label; // "<trading id>+<financing id>"
  bool capacity{};
  std::vector<atx::f64> c_history, costs;
  cost_v2::AimV6Decision decision;
  std::vector<TcRecord> tc;
};
// Everything one published run's v7 files read, from its own books (P9 C1): the main books'
// scored transfer coefficients and leverage records, each merged in session order and, within
// a session, in book order (the order the lockstep made them), and every book's record.
struct RunRecords {
  std::vector<TcRecord> tc;
  std::vector<risk_target::Record> leverage;
  std::vector<BookRecord> books;
};
// The books of one lockstep run: the main books (run_scenarios), then under --capacity-curve
// (main pass) the capacity books of the primary from capacity_begin on (P9 C1: one lockstep).
struct LockstepBooks {
  std::vector<NavScenario> books;
  atx::usize capacity_begin{};
};

// The parsed v7 command line: the options and the replay's own tokens in order (v7 tokens
// consumed, `--rule aim-partial-v6` rewritten to aim-partial-v5; args[0] is the verb).
// InvalidArgument on a usage error (duplicate/missing value, v6 parameters without the v6
// rule, a per-name rate with v6 or the capacity curve, parameters out of range).
struct NavV7Command {
  NavV7Options options;
  std::vector<std::string> args;
  std::string output;
  std::string risk_model, risk_model_sha256; // the spo rules and the risk target
};
[[nodiscard]] atx::core::Result<NavV7Command> parse_nav_v7_args(int argc, char** argv);

// ---- seams (strategy_nav_replay.cpp) ----
[[nodiscard]] bool claims_nav_args(int argc, char** argv);
[[nodiscard]] int dispatch_nav_v7(int argc, char** argv, std::ostream& out, std::ostream& err);
// Main pass: the matrix plus S2-KO/S2-FIM copies of its primary (--cost-v2); capacity pass:
// the capacity books of its primary; no extension: the matrix.
[[nodiscard]] std::vector<NavScenario> run_scenarios(std::vector<NavScenario> matrix);
// The books of one nav run (P9 C1): run_scenarios(matrix), then under --capacity-curve in the
// main pass the capacity books of the matrix's primary (cost_v2::capacity_scenarios) from
// capacity_begin on; otherwise capacity_begin = books.size(). No extension: the matrix.
[[nodiscard]] LockstepBooks lockstep_scenarios(std::vector<NavScenario> matrix);
// True when `scenario` is a capacity book of the installed extension: in the capacity pass every
// book; in the main pass a capacity id (cost_v2::capacity_multiple finite) under
// --capacity-curve. False without an extension.
[[nodiscard]] bool capacity_book(const NavScenario& scenario);
// The NAV multiple whose NAV a book's ADV cap reads (P9 C1): cost_v2::capacity_multiple of a
// capacity book, 1 for every other book.
[[nodiscard]] atx::f64 nav_multiple(const NavScenario& scenario);
// cost_v2::reserved_cost_model (Ok(nullptr): the replay's own model applies).
[[nodiscard]] atx::core::Result<std::unique_ptr<const atx::engine::book::ReplayCostModel>>
extension_cost_model(const NavScenario& scenario);
// P9 C1, DEC-10: what the replay asks the installed extension on the calling thread before its
// books run. configure: a book config under --risk-target / --vol-target gets the extension's
// leverage rule (risk_target::leverage_rule) unless it carries one; identity otherwise (the
// extension notes whether --adv-hold-q is on, which picks the spo-v3 capacity sentence of the
// summary, holdings manifest and v7_extras.json; P9 C1 fix 1). make_book_state: the book's v7
// state (nullptr without an extension). shared_plan_state: the spo engines hold every book's
// state (books on a pool and grids are refused with them).
void configure(NavReplayConfig& cfg);
[[nodiscard]] std::unique_ptr<BookState> make_book_state(const NavScenario& scenario);
[[nodiscard]] bool shared_plan_state();
// The weight update of one book at decision d: detail::update_weights (the extension only
// observes the transfer coefficient), or aim-partial-v6 / spo-v1 on rebalance decisions.
// tier: the decision's borrow tier per name (empty without tiers); no_locate: decide
// --locates (empty in the replay). Only spo-v1 reads them (its locate floor and financing).
// two_speed_fast: two-speed-v1's F entering the decision (DesiredState::fast_before; empty
// otherwise); under --risk-target / --vol-target a two-speed rebalance needs it: the book's plan
// carries its fast holding at its own scale lambda_t = L_t / L (engine::book::two_speed_carry,
// Ruling PM8-16 #10). This loose seam keeps the book's state by its label in the extension
// (the decide path and the tests); a replay book plans through plan_book.
[[nodiscard]] atx::core::Status plan(const TargetReplayInput& x, const NavReplayConfig& cfg,
                                     atx::usize d, bool rebalance, atx::f64 spent,
                                     atx::f64 nav_post, const std::vector<atx::f64>& desired,
                                     std::vector<atx::f64>& planned, TargetReplayDay& out,
                                     std::span<const atx::f64> rates,
                                     std::span<const atx::u8> tier = {},
                                     std::span<const atx::u8> no_locate = {},
                                     std::span<const atx::f64> two_speed_fast = {});
// The rule of one replay book at decision d (P9 C1): `cfg` is the book's config at the aim
// leverage its leverage rule set (base_leverage: the book's own L when that rule moved it, NaN
// under Fixed), `desired` the target that rule handed on. book nullptr (no extension):
// detail::update_weights. liquidity: the decision's shared decision liquidity
// (cost_v2::decision_liquidity, formed once on the calling thread for every book of a rebalance
// decision); nullptr: the extension's own cache (one thread only).
[[nodiscard]] atx::core::Status plan_book(BookState* book, const TargetReplayInput& x,
                                          const NavReplayConfig& cfg, atx::f64 base_leverage,
                                          atx::usize d, bool rebalance, atx::f64 spent,
                                          atx::f64 nav_post, const std::vector<atx::f64>& desired,
                                          std::vector<atx::f64>& planned, TargetReplayDay& out,
                                          std::span<const atx::f64> rates,
                                          std::span<const atx::u8> tier,
                                          std::span<const atx::u8> no_locate,
                                          const cost_v2::DecisionLiquidity* liquidity);
// After a replay (calling thread): the books' scored records (NavReplayResult::leverage,
// ::transfer), merged in session then book order, join the extension's (tc_records, the
// scaler's records()). Identity without an extension.
void observe(std::span<const NavReplayResult> results);
// After the replay and before anything is published: records the books and reads the spo
// tripwire (spo::Engine::rows_tripwire: spo-v1/v2 the specific-ceiling tripwire, spo-v3 its
// own) over the scored decisions (a warm-up decision leaves no spo row). An error voids the
// run: the replay returns it before its output directory exists. Ok without an extension
// (identity).
[[nodiscard]] atx::core::Status capture(std::span<const NavScenario> scenarios,
                                        std::span<const NavReplayResult> results,
                                        std::span<const NavSummary> summaries);
// One run's records from its own books (P9 C1; scenarios, results and summaries aligned, the
// capacity books included). Empty without an extension.
[[nodiscard]] RunRecords run_records(std::span<const NavScenario> scenarios,
                                     std::span<const NavReplayResult> results,
                                     std::span<const NavSummary> summaries);
// True while an extension is installed on this thread.
[[nodiscard]] bool installed();
// While alive, the extension publishes the capacity directory (P9 C1): the declarations read
// pass "capacity" and the summary blocks are the capacity pass's (spo-v3: the capacity engine's;
// the risk target: parameters only). Restores the pass it found.
class CapacityPublication {
public:
  CapacityPublication();
  ~CapacityPublication();
  CapacityPublication(const CapacityPublication&) = delete;
  CapacityPublication& operator=(const CapacityPublication&) = delete;
  CapacityPublication(CapacityPublication&&) = delete;
  CapacityPublication& operator=(CapacityPublication&&) = delete;

private:
  NavV7Pass previous_{NavV7Pass::Main};
};
// The v7 files of one complete run into `dir` (P9 C1, before its summary.json):
// v7_transfer_coefficient.csv, the risk / vol target series, capacity_curve.csv (with the
// capacity books), spo_diagnostics.csv, then v7_extras.json, whose files map binds each and
// capacity/summary.json (`capacity_summary_sha256`, empty: none). `binding` receives what the
// run's summary.json binds: {"v7_extras.json": SHA[, "capacity/summary.json": SHA]}; null and
// nothing written without an extension.
[[nodiscard]] atx::core::Status publish_extras(const std::filesystem::path& dir,
                                               const RunRecords& records,
                                               const std::string& capacity_summary_sha256,
                                               nlohmann::json& binding);
// Reserved-id cost labels always; v7 declarations and the v6 rule id with an extension.
void extend_recipe(nlohmann::json& recipe);
// The extension's own records (tc_records, the scaler's records()).
void extend_summary(nlohmann::json& summary);
// A published run's summary (P9 C1): its own records; `binding` (publish_extras) non-null:
// summary v7.files = binding and the extras sentence says they precede it; null (the capacity
// directory): the sentence of a run without the binding.
void extend_summary(nlohmann::json& summary, const RunRecords& records,
                    const nlohmann::json& binding);
// --emit-holdings manifest: the v6 rule id and the v7 declarations with an extension.
void extend_holdings(nlohmann::json& manifest);
// The v7 lines of `nav --help` (always printed; the flags route through dispatch_nav_v7).
void append_help(std::ostream& out);
} // namespace atx::impl::strategy::v7
