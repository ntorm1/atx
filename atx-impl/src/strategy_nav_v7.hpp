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
//   --capacity-curve          second pass: S2 at NAV x {.5,1,2,4,8} into <output>/capacity,
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
//                             pass's rows, tripwire and summary are unchanged. --trade-fraction
//                             has no effect on the spo-v3 plan (H 20 is registered).
//   spo-v1 / spo-v2 / spo-v3 blocks are keyed "spo_v1" / "spo_v2" / "spo_v3" (recipe v7,
//   summary v7, extras); the Engine's rule_* / rows_* members produce them.
// --emit-holdings (lane L3) observes the main pass only; the capacity pass drops it.
// Every hooked run also writes <output>/v7_transfer_coefficient.csv (TC per rebalance
// decision and book) and <output>/v7_extras.json (extras' SHA-256, capacity table) after
// the replay's own summary.json.
// Warm start (v8 D-0, --warm-start-sessions K): the decisions before the role's
// decision_begin plan the books but are not scored, so every v7 side file (the transfer
// coefficients, spo_diagnostics.csv, the spo summary blocks) and the spo tripwire cover
// decisions d >= decision_begin only. K = 0: every decision is scored, bytes unchanged.
//
// Threading: the installed extension is thread-local; a replay runs on one thread.

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
  std::shared_ptr<const spo::RiskStore> spo_risk; // opened by dispatch_nav_v7
};
enum class NavV7Pass : atx::u8 { Main = 0, Capacity = 1 };

// One rebalance decision of one book (main pass only).
struct TcRecord {
  atx::i64 session{};
  std::string book;           // "<trading id>+<financing id>"
  atx::f64 tc{};              // transfer coefficient (cost_v2::transfer_coefficient)
  atx::usize members{}, costed{}, banded{};
  atx::f64 c_bar{}, c_ref{}, theta{}; // aim-partial-v6 only (NaN otherwise)
};
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
  // Starts a replay pass: clears the per-run state (decision liquidity cache, the books'
  // reference-cost histories); records and captures accumulate across passes.
  void begin_run(NavV7Pass pass);
  [[nodiscard]] std::span<const TcRecord> tc_records() const noexcept;
  [[nodiscard]] std::span<const BookRecord> books() const noexcept;
  // The spo-v1 engine (nullptr without --rule spo-v1).
  [[nodiscard]] const spo::Engine* spo_engine() const noexcept;
  // spo-v3 --capacity-curve (Ruling E-37): the engine the capacity pass plans on (nullptr
  // without the spo rule and the capacity curve); spo_engine() keeps the main pass's rows.
  [[nodiscard]] const spo::Engine* spo_capacity_engine() const noexcept;
  // Why capture() voided the run (the spo specific-ceiling tripwire); empty otherwise.
  [[nodiscard]] const std::string& void_reason() const noexcept;
  struct State;

private:
  std::unique_ptr<State> state_;
  State* previous_{};
};

// The parsed v7 command line: the options and the replay's own tokens in order (v7 tokens
// consumed, `--rule aim-partial-v6` rewritten to aim-partial-v5; args[0] is the verb).
// InvalidArgument on a usage error (duplicate/missing value, v6 parameters without the v6
// rule, a per-name rate with v6 or the capacity curve, parameters out of range).
struct NavV7Command {
  NavV7Options options;
  std::vector<std::string> args;
  std::string output;
  std::string risk_model, risk_model_sha256; // the spo rules only
};
[[nodiscard]] atx::core::Result<NavV7Command> parse_nav_v7_args(int argc, char** argv);

// ---- seams (strategy_nav_replay.cpp) ----
[[nodiscard]] bool claims_nav_args(int argc, char** argv);
[[nodiscard]] int dispatch_nav_v7(int argc, char** argv, std::ostream& out, std::ostream& err);
// Main pass: the matrix plus S2-KO/S2-FIM copies of its primary (--cost-v2); capacity pass:
// the capacity books of its primary; no extension: the matrix.
[[nodiscard]] std::vector<NavScenario> run_scenarios(std::vector<NavScenario> matrix);
// cost_v2::reserved_cost_model (Ok(nullptr): the replay's own model applies).
[[nodiscard]] atx::core::Result<std::unique_ptr<const atx::engine::book::ReplayCostModel>>
extension_cost_model(const NavScenario& scenario);
// The weight update of one book at decision d: detail::update_weights (the extension only
// observes the transfer coefficient), or aim-partial-v6 / spo-v1 on rebalance decisions.
// tier: the decision's borrow tier per name (empty without tiers); no_locate: decide
// --locates (empty in the replay). Only spo-v1 reads them (its locate floor and financing).
[[nodiscard]] atx::core::Status plan(const TargetReplayInput& x, const NavReplayConfig& cfg,
                                     atx::usize d, bool rebalance, atx::f64 spent,
                                     atx::f64 nav_post, const std::vector<atx::f64>& desired,
                                     std::vector<atx::f64>& planned, TargetReplayDay& out,
                                     std::span<const atx::f64> rates,
                                     std::span<const atx::u8> tier = {},
                                     std::span<const atx::u8> no_locate = {});
// After the replay and before anything is published: records the books and reads the spo
// tripwire (spo::Engine::rows_tripwire: spo-v1/v2 the specific-ceiling tripwire, spo-v3 its
// own) over the scored decisions (a warm-up decision leaves no spo row). An error voids the
// run: the replay returns it before its output directory exists. Ok without an extension
// (identity).
[[nodiscard]] atx::core::Status capture(std::span<const NavScenario> scenarios,
                                        std::span<const NavReplayResult> results,
                                        std::span<const NavSummary> summaries);
// Reserved-id cost labels always; v7 declarations and the v6 rule id with an extension.
void extend_recipe(nlohmann::json& recipe);
void extend_summary(nlohmann::json& summary);
// --emit-holdings manifest: the v6 rule id and the v7 declarations with an extension.
void extend_holdings(nlohmann::json& manifest);
// The v7 lines of `nav --help` (always printed; the flags route through dispatch_nav_v7).
void append_help(std::ostream& out);
} // namespace atx::impl::strategy::v7
