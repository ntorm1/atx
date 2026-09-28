#pragma once

#include <iosfwd>
#include <optional>
#include <string>
#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

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
//   source.git_sha (40 hex), seal {policy "research-seal-v1", exclusive_session
//   "2025-01-01"}, owner_gate (null, or {owner, ruling, date}),
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
// The research seal: exclusive 2025-01-01 UTC, the instant of kSeal in
// atx-engine/src/data/strategy_data.cpp (unchanged here). Every session at or past it is
// refused; with an owner_gate record it is STILL refused: live sessions are an owner
// gate and are not enabled in this build.
inline constexpr const char* research_seal_policy = "research-seal-v1";
inline constexpr const char* research_seal_session = "2025-01-01";
inline constexpr atx::i64 research_seal_exclusive_ns = 1'735'689'600'000'000'000LL;
inline constexpr bool live_sessions_enabled = false;

struct DecideConfig {
  std::string deploy_path, positions_path, locates_path, output_directory;
  std::string asof; // YYYY-MM-DD: a role session in the score window
  // Post-close book NAV (cash + positions after the as-of fills). Absent: the positions
  // file's nav_post column (equal on every selected row).
  std::optional<atx::f64> nav;
  // --check-replay: the positions file is a replay's holdings.csv; its target_weight at
  // the as-of must equal the decision bit for bit (every name; an absent name is 0).
  bool check_replay{};
  std::string executable_sha256; // the running executable (the CLI hashes itself)
  std::string build_source_sha;  // configure-time provenance (the CLI's build)
};
enum class DecideHealth : atx::u8 { Ok = 0, Warn = 1, Error = 2 };
struct DecideOutcome {
  DecideHealth health{DecideHealth::Ok};
  bool parity_checked{};
  atx::usize parity_names{}, parity_mismatches{};
};
// Positions CSV: header with columns instrument_id and held_dollars (any order, other
// columns ignored); with a session_ns column only rows of the as-of session are read (so
// a replay's holdings.csv is a positions file); nav_post and target_weight columns are
// read when present. Every id must be a role instrument, at most once; dollars finite.
// Locates CSV (optional): instrument_id,locate with locate 0|1; a name not listed or
// listed 0 has no locate: it may not open or grow a short (the locate-in-aim mask and
// the post-rule block, OR-ed with the modeled special tier).
// Outputs (exclusive directory; decision.json LAST): targets.csv, orders.csv,
// decision.json (atx.book-decision/v1: pins verified, construction record, transfer
// coefficient, health checks, replay parity when checked).
// Returns the outcome once decision.json is written; any refusal is an error and writes
// nothing (the directory is created only after every check and the decision).
[[nodiscard]] atx::core::Result<DecideOutcome> run_decide(const DecideConfig& cfg,
                                                          std::ostream& progress);
// argv[0] is the "decide" verb: --deploy M --asof YYYY-MM-DD --positions CSV --output NEWDIR
// [--locates CSV] [--nav DOLLARS] [--check-replay]. Exit codes: 0 decided (health ok or
// warn), 1 refused, 2 usage, 3 decided with a health ERROR (review before any order),
// 4 --check-replay mismatch.
[[nodiscard]] int dispatch_decide(int argc, char** argv, std::ostream& out, std::ostream& err);
} // namespace atx::impl::strategy
