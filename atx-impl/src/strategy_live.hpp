#pragma once

#include <iosfwd>
#include <optional>
#include <span>
#include <string>
#include "atx/core/error.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/data/research_window.hpp"

namespace atx::impl::strategy {
// ---- the daily decide path (v7 B1 manifest, B2 decide, B6 locates, B9 health) ----
// TRAIN-only in this build: the role class must be "train" and no session at or past the
// research seal is decided.
//
// atx.book-deploy/v1 binds one deployable book (JSON object; every key required):
//   schema "atx.book-deploy/v1", book (id text),
//   library.sha256, recipe.sha256 (the IC run recipe), orientations.sha256,
//   composition {scheme (text), weights_sha256 (sha, or null for equal weights)},
//   combined {path, sha256}, role {path, sha256, class ("train")},
//   fields {path, sha256, code_sha256, names [text...]}, data_source_sha256,
//   universe.member_semantics,
//   nav {rule, cadence, trade_fraction, dust_multiple, aim_leverage (L), neutralize,
//        band_multiple, monthly_budget, exit_rate, order_basis, locate_in_aim,
//        liquidity_cache, rate ("fixed"), daily_turnover_mean_max, daily_turnover_p95_max,
//        max_working_bytes, recipe_sha256 (the NAV run's summary.json recipe_sha256)},
//   executables {"atx-equity-strategy-targets": sha, "atx-equity-strategy-ic": sha},
//   source.git_sha (40 hex), seal {policy (the research window id, kResearchWindowId),
//   exclusive_session (its seal date, kSealBeginDate)}, owner_gate (null, or {owner, ruling, date}),
//   health {gross_leverage [lo, hi], abs_net_leverage_max, planned_turnover_max,
//           names_without_locate_max}.
// Paths are used as given (relative to the working directory, as every CLI here).
// Refusals (each names the pin): a missing or malformed pin; a file whose SHA-256 differs
// (combined, role, fields); a combined-manifest binding that differs (library, recipe,
// orientations, composition weights, data source, member semantics, role class and
// manifest, fields manifest when the blend records it); fields code_sha256 or a listed
// name absent from the fields manifest; the NAV recipe recomputed from nav + pins
// differing from nav.recipe_sha256; the running executable's SHA differing from
// executables.atx-equity-strategy-targets; a seal block other than the policy below.
// source.git_sha is recorded against the build's configure-time provenance and reported
// (health warning), never a refusal: that provenance is stale by construction (review C4)
// and the executable SHA already binds the code.
inline constexpr const char* book_deploy_schema = "atx.book-deploy/v1";
inline constexpr const char* book_decision_schema = "atx.book-decision/v1";
// The research seal: the research window of atx/engine/data/research_window.hpp (the seal
// read_strategy_role enforces). Every session at or past it is refused; with an owner_gate
// record it is STILL refused: live sessions are an owner gate and are not enabled in this
// build. The two texts point into the header's string literals, so they are NUL-terminated.
inline constexpr const char* research_seal_policy = atx::engine::data::kResearchWindowId.data();
inline constexpr const char* research_seal_session = atx::engine::data::kSealBeginDate.data();
inline constexpr atx::i64 research_seal_exclusive_ns = atx::engine::data::kSealBeginNs;
inline constexpr bool live_sessions_enabled = false;

struct DecideConfig {
  std::string deploy_path, positions_path, locates_path, output_directory;
  std::string asof; // YYYY-MM-DD: a role session in the score window
  // Post-close book NAV (cash + positions after the as-of fills). Absent: the positions
  // file's nav_post column (equal on every selected row).
  std::optional<atx::f64> nav;
  std::string nav_text; // the --nav literal as given (recorded in decision.json pins)
  // Review R1 M-1: positions that select no row at the as-of are refused (a data break)
  // unless the book is declared genuinely flat.
  bool flat_book{};
  // --check-replay: the positions file is a replay's holdings.csv; its target_weight at
  // the as-of must equal the decision bit for bit (every name; an absent name is 0).
  bool check_replay{};
  std::string executable_sha256; // the running executable (the CLI hashes itself)
  std::string build_source_sha;  // configure-time provenance (the CLI's build)
  // ---- v7 W4 ----
  // B7 share orders (orders_shares.csv, expected_holdings.csv): the deployment NAV the
  // weights are sized to (absent: the book NAV), the lot, the minimum notional of a
  // non-exit order and the reference price ("close": the as-of session's raw close).
  std::optional<atx::f64> nav_dollars;
  atx::u64 lot_size{1};
  atx::f64 min_notional{500};
  std::string price_source{"close"};
  // B9: prior decide output directories (each holding decision.json) for the
  // transfer-coefficient band; empty: not supplied (the check warns).
  std::string prior_decisions_directory;
  // B9 freshness: the as-of must be the role's last session unless this is set (then a
  // health warning records how stale the decision is).
  bool allow_stale{};
};
enum class DecideHealth : atx::u8 { Ok = 0, Warn = 1, Error = 2 };
// B9 transfer-coefficient band (finding L3-F2; P2 R2.5): warn when TC(target) < min for
// `consecutive` decisions in a row, the as-of one included.
inline constexpr atx::f64 tc_band_min = 0.5;
inline constexpr atx::usize tc_band_consecutive = 5;
// The run of transfer coefficients below tc_band_min: the as-of's first, then the prior
// decisions' newest first; a value not below (NaN included) ends it; capped at
// tc_band_consecutive. The band warns when the run reaches the cap.
[[nodiscard]] atx::usize tc_band_run(atx::f64 asof_tc,
                                     std::span<const atx::f64> priors_newest_first) noexcept;
struct DecideOutcome {
  DecideHealth health{DecideHealth::Ok};
  bool parity_checked{};
  atx::usize parity_names{}, parity_mismatches{};
};
// Positions, one of:
// - a CSV with columns instrument_id and held_dollars (any order, other columns ignored);
//   with a session_ns column only rows of the as-of session are read (so a replay's v1
//   holdings.csv is a positions file); nav_post, target_weight and shares columns are read
//   when present (shares: the broker's share count, used by the share orders);
// - an f64 holdings directory of `nav --emit-holdings` (or its holdings_index.json): the
//   as-of session's rows (held_dollars, target_weight) and nav_post, every file SHA
//   verified; a session the replay did not report is refused.
// Every id must be a role instrument, at most once; dollars finite.
// Locates CSV (optional): instrument_id,locate with locate 0|1; a name not listed or
// listed 0 has no locate: it may not open or grow a short (the locate-in-aim mask and
// the post-rule block, OR-ed with the modeled special tier).
// Outputs (exclusive directory; decision.json LAST): targets.csv, orders.csv,
// orders_shares.csv, expected_holdings.csv, decision.json (atx.book-decision/v1: pins
// verified, construction record, transfer coefficient, share-order summary, health
// checks, replay parity when checked).
//   orders_shares.csv: instrument_id,side,shares,notional,reference_price,adv_dollars,
//     participation,residual_shares,residual_notional,target_weight,current_weight,exit,
//     short_sale,below_min_notional,reason,tag (strategy_orders.hpp's rule; tag moc = the
//     replay's fill at the next session's close; participation of the raw-dollar ADV that
//     fill reads, nan without ADV);
//   expected_holdings.csv: instrument_id,shares,current_shares,order_shares,
//     reference_price,notional (the book after the sent orders fill; reconcile's
//     --expected).
// Returns the outcome once decision.json is written; any refusal is an error and writes
// nothing (the directory is created only after every check and the decision).
[[nodiscard]] atx::core::Result<DecideOutcome> run_decide(const DecideConfig& cfg,
                                                          std::ostream& progress);
// argv[0] is the "decide" verb: --deploy M --asof YYYY-MM-DD --positions CSV|DIR
// --output NEWDIR [--locates CSV] [--nav DOLLARS] [--check-replay] [--nav-dollars D]
// [--lot-size 1] [--min-notional 500] [--price-source close] [--prior-decisions DIR]
// [--allow-stale]. Exit codes: 0 decided (health ok or warn), 1 refused, 2 usage, 3
// decided with a health ERROR (review before any order), 4 --check-replay mismatch.
[[nodiscard]] int dispatch_decide(int argc, char** argv, std::ostream& out, std::ostream& err);
} // namespace atx::impl::strategy
