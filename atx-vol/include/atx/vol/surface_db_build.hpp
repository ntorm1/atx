#pragma once

// surface_db_build — build a production SurfaceDb from OPRA boards, in two
// stages that share this header:
//
//   1. `generate_symbol_configs` — auto-generate the manifest's per-symbol fit
//      configuration from a set of loaded boards (below).
//   2. `build_surface_db` — the one-call driver that chains hive load ->
//      config generation -> streaming populate into a single create-or-open
//      call, plus `write_build_report_csv` for its report (bottom of file).
//
// ── Stage 1: `generate_symbol_configs` ──────────────────────────────────────
//
// Where `surface_db_populate.hpp` FITS boards into partitions, this stage decides
// HOW each symbol should be fit and writes that decision into the manifest's
// symbol table (`SurfaceDb::upsert_symbol`) BEFORE any populate runs. For each
// symbol it takes one representative board, classifies it through the shared
// atx-vol fit-policy seam (`select_fit_policy`, optionally the full held-out
// `select_curve` search), and stores a `SymbolFitConfig` carrying the chosen
// curve family — as a PREFERRED ROUTE by default, or as a hard PIN when
// `AutoConfigSpec::pin_curve_family` is set (see there for why the default is
// not to pin). A symbol whose board cannot be selected on is stored DISABLED
// (fail closed — never silently served), the top-level call still succeeding so
// one bad board never sinks a universe build.
//
// ── Ownership / thread-safety ────────────────────────────────────────────────
//
// `generate_symbol_configs` borrows `db` (mutated via its own serialized
// `upsert_symbol`) and `boards` (read-only) for the call and returns an owning
// report. It is itself single-threaded: it walks symbols in canonical order and
// upserts one at a time, so re-running it is deterministic. Not safe to call
// concurrently with other mutators on the SAME `db` (they share the manifest
// mutex, but interleaved upserts would race the skip-existing/idempotence
// bookkeeping this stage owns). `build_surface_db` is likewise single-threaded
// at the driver level (its fits fan out internally); one build per db root.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "atx/vol/corpus.hpp"             // CorpusBoard
#include "atx/vol/opra_hive.hpp"          // OpraHiveSpec
#include "atx/vol/surface_db.hpp"         // SurfaceDb, SymbolFitConfig, FitPreset
#include "atx/vol/surface_db_populate.hpp" // UniversePopulateCoverage, PopulateSymbolStats
#include "atx/vol/types.hpp"             // Result, Status

namespace atx::vol {

// How to auto-generate the manifest configs. Field names/defaults are
// contractual (Task 5's build driver constructs this directly).
struct AutoConfigSpec {
  // Board date used for per-symbol selection. Empty ("") => each symbol's
  // EARLIEST available board (min date string) is used. A non-empty date a given
  // symbol has NO board for falls back to that symbol's earliest board (per
  // symbol, silently — one symbol missing the requested date never fails the
  // call or skips the symbol).
  std::string config_date{};
  // Base preset captured into every symbol's config (`symbol_config_from_preset`).
  // The fit-policy decision pins the curve FAMILY; this preset is the numerical
  // tier and is retained (matching `populate_universe_streaming`'s seeding).
  FitPreset preset{FitPreset::Populate};
  // The designated index leg — pinned to the dense index recipe (shared with
  // `populate_universe_streaming` via `seed_symbol_config`), bypassing per-board
  // selection. Empty => no index leg.
  std::string index_symbol{};
  // false (default) => per-symbol selection uses `select_fit_policy` board
  // features only (O(quotes), no fit). true => additionally run the full
  // held-out `select_curve` OOS search and pin its winner; a NotFound/Unavailable
  // selector outcome falls back to the fit-policy decision.
  bool deep_selection{false};
  // false (default) => a symbol already present in the manifest is left
  // UNTOUCHED (idempotent resume; operator overrides win). true => overwrite it.
  bool overwrite_existing{false};
  // Re-attempt the symbols whose STORED config is disabled (FIX-C-2).
  //
  // A fail-closed disable is a machine-generated verdict about ONE config board,
  // not a durable property of the symbol — but skip-existing makes it permanent:
  // once stored, the symbol is `n_skipped_existing` on every later run and no fix
  // to the loader, the hive, or the selector can ever reach it. That is how a
  // build that lost BRK.B to the underlying/OSI-root split (FIX-C-1) would have
  // stayed broken over the production database even after the split was fixed.
  //
  // true => a symbol whose stored config is disabled is re-selected exactly as if
  // it were new: it becomes `n_configured` (enabled) when selection now succeeds,
  // or `n_disabled_failed` when it still fails. ENABLED existing configs are
  // still skipped, so an operator's tuned config is never touched — that is the
  // difference from `overwrite_existing`, which clobbers everything.
  //
  // false (DEFAULT) because a disable can also be deliberate (an operator
  // `upsert_symbol` with `enabled = false` fences a symbol out of production), and
  // silently re-enabling that on every build would be its own defect. The standing
  // disabled set is NAMED in `failed_symbols` on every run instead, so the
  // operator who needs this knob is told it exists rather than having to know.
  bool retry_disabled{false};
  // Does the selected curve family become a HARD PIN in the stored config
  // (`SymbolFitConfig::pin_curve`), or only the preferred route?
  //
  // false (DEFAULT) => `pin_curve` is left false. The family this stage chose is
  // still written to `SymbolFitConfig::curve` (an operator can read which family
  // the policy liked, and `--pin-curve-family true` on the CLI turns it into a
  // pin), but the fit AUTO-ROUTES: `PricerFitter` re-derives the decision per
  // board and — crucially — keeps BOTH of its recovery ladders alive, the
  // construction-failure ladder and the admission-rejection ladder
  // (`pricer_fitter.cpp`'s `auto_routed`, which is false whenever anything is
  // pinned). A production build over real OPRA boards lost 10 of 45 cells to
  // marginal, single-attempt admission rejections that those ladders exist to
  // recover; the pin was switched on for 100% of symbols, so the ladders were
  // off for 100% of cells.
  //
  // true => `pin_curve` is set, restoring the pre-2026-07-25 behaviour: exactly
  // one curve-family attempt per cell, no recovery. The pin's "never silently
  // substituted" immunity is meant for an OPERATOR's explicit instruction; a
  // machine-generated per-symbol guess is not that, which is why it is opt-in.
  //
  // Also gates the LinearVariance fail-closed guard: a pinned LinearVariance
  // config makes the RISK pipeline hard-reject EVERY cell of that symbol
  // ("invalid correctness policy for requested risk surface"), so when this is
  // true AND the symbol's policy requests a Risk output, such a symbol is
  // disabled at config time and named in `failed_symbols` rather than failing
  // 100% of its cells at fit time. A mark-only policy (e.g. the Hft preset) is
  // deliberately exempt — the mark path pins LinearVariance itself.
  bool pin_curve_family{false};
};

// What `generate_symbol_configs` did. The three disposition counters partition
// the distinct symbols it saw: `n_configured + n_skipped_existing +
// n_disabled_failed == n_symbols`.
struct AutoConfigReport {
  std::uint32_t n_symbols{0};          // distinct symbols in `boards`
  std::uint32_t n_configured{0};       // freshly configured (or overwritten), enabled
  std::uint32_t n_skipped_existing{0}; // already present, left untouched (not overwrite)
  std::uint32_t n_disabled_failed{0};  // selection failed -> stored disabled
  // FIX-C-2. A SUB-COUNT of `n_skipped_existing` (not a fourth partition class):
  // how many of the skipped symbols carry a DISABLED stored config. Zero on a
  // healthy resume; non-zero means the database is deliberately not serving that
  // many requested names and will keep not serving them until someone acts
  // (`AutoConfigSpec::retry_disabled`, or an explicit upsert).
  std::uint32_t n_disabled_existing{0};
  // Every symbol this run left DISABLED in the db, sorted — the union of the
  // `n_disabled_failed` names that failed selection HERE and the
  // `n_disabled_existing` names that were already stored disabled and skipped.
  // So `failed_symbols.size() == n_disabled_failed + n_disabled_existing`.
  //
  // The union (rather than just this run's fresh failures) is the FIX-C-2 fix.
  // The first build over a database named its casualty and every build after it
  // did not: the disabled config became `n_skipped_existing`, the populate
  // reported the date `dates_skipped_complete`, and a rerun printed an all-green
  // report while the symbol stayed permanently absent. A standing failure that
  // only the first run mentions is a silent failure. The name is now on EVERY
  // run's report and in the `--report` CSV.
  std::vector<std::string> failed_symbols;
};

// Auto-generate one `SymbolFitConfig` per distinct symbol in `boards` and upsert
// it into `db`'s manifest.
//
// Per symbol (canonical order): pick the config board (`spec.config_date` or the
// symbol's earliest); if already configured and not `spec.overwrite_existing`,
// SKIP it (`n_skipped_existing`, and additionally `n_disabled_existing` + a
// `failed_symbols` entry when that stored config is DISABLED — unless
// `spec.retry_disabled`, which re-selects it as if it were new); if it is
// `spec.index_symbol`, store the dense
// index recipe (`seed_symbol_config`); otherwise build the board's `OptionChain`
// (`OptionChain::from_frame`, the corpus_board_fit path), classify it with
// `select_fit_policy`, and store `symbol_config_from_preset(spec.preset)` with
// the policy's curve recorded (`curve = decision.curve`) and
// `pin_curve = spec.pin_curve_family` (default false — see `AutoConfigSpec`).
// When `spec.deep_selection`, `select_curve`'s held-out winner is recorded
// instead, falling back to the fit-policy decision on a NotFound/Unavailable
// selector outcome. A symbol whose board fails to build a selectable underlying
// — or whose deep selection fails with a hard (non-fallback) error, or throws, or
// (when pinning a Risk-requesting policy) whose chosen family is `LinearVariance`,
// which the risk pipeline refuses outright — is stored DISABLED
// (`symbol_config_from_preset(spec.preset)` with `enabled = false`) and recorded
// in `failed_symbols` (`n_disabled_failed`); the call still succeeds.
//
// @return the disposition report, or an Error only on a db write failure
//         (`upsert_symbol` IoError/ParseError propagated). An empty `boards`
//         span yields an all-zero report (Ok), not an error.
[[nodiscard]] Result<AutoConfigReport>
generate_symbol_configs(SurfaceDb &db, std::span<const CorpusBoard> boards,
                        const AutoConfigSpec &spec);

// ── Stage 2: `build_surface_db` — the one-call build driver ─────────────────
//
// Everything a production build needs in one call: (1) create the db at
// `db_root` iff it has no manifest yet, else open the existing one (mirrors
// `SurfaceDb::open`'s NotFound probe — a resumed build reuses the same root);
// (2) load the `hive` window (`load_opra_hive`); (3) build one `CorpusBoard`
// per SUCCESSFULLY loaded cell (missing/corrupt cells are tallied, never fit);
// (4) `generate_symbol_configs` over those boards (`auto_config`); (5)
// `populate_universe_streaming` them (cell-aware resume, RSS O(dates in
// flight)) with an index leg / preset / worker budget from this spec.
//
// Field names/defaults are contractual (the CLI and Python bindings construct
// this directly).
struct SurfaceDbBuildSpec {
  std::string db_root;         // created if absent, else opened (resume)
  OpraHiveSpec hive;           // the hive window to load (root_dir/date span/symbols)
  AutoConfigSpec auto_config{}; // per-symbol config-generation policy (stage 1)
  // Numerical tier every symbol's fit runs at (passed to the populate);
  // `auto_config.preset` seeds the manifest, this drives the populate fallback.
  FitPreset preset{FitPreset::Populate};
  unsigned fit_workers{0}; // 0 = auto (honors ATX_VOL_FIT_WORKERS); 1 = serial
};

// The full disposition of a `build_surface_db` call: the stage-1 config report,
// the stage-2 populate coverage, and the ingest tallies. The two DATE counters
// describe the hive load in distinct dates, not cells: `n_dates_loaded` is the number
// of distinct dates that produced at least one board; `n_dates_missing` is the
// number of distinct in-range dates that produced NONE (a fully absent or
// unreadable date) — the window is enumerated as CALENDAR days, so every weekend
// and market holiday in range counts as missing (a July window always shows ~9).
//
// The last two split the loader's `n_error` cells into the two things an operator
// must NOT confuse: `n_load_errors` is the CELL count of real defects — a present
// file that is unreadable/unparseable, has the wrong schema, or whose market
// inputs quarantined the cell; `n_coverage_holes` is the CELL count of a present,
// readable date file that simply does not carry that symbol. A real hive has
// non-uniform per-date coverage, so a discover-all build reports MANY holes and
// that is healthy — it is `n_load_errors` that means "something is wrong with the
// data". Both are classified structurally by `load_opra_hive` (never inferred from
// an error code), and together they exhaust the loader's `n_error`. Neither ever
// reaches the fit.
struct SurfaceDbBuildReport {
  AutoConfigReport config;
  UniversePopulateCoverage coverage;
  std::size_t n_dates_loaded{0};
  std::size_t n_dates_missing{0};
  std::size_t n_load_errors{0};
  std::size_t n_coverage_holes{0};
};

// ── Bounding the failed-cell list for display ───────────────────────────────
//
// `report.coverage.failed_cells` holds EVERY failed cell (one per
// `coverage.cells_failed`), because the `--report` CSV is the artifact an
// operator greps and truncating it would throw away the very diagnostic this
// list exists to carry. A terminal is the other consumer, and a
// 51-symbol x 17-date universe that fails wholesale is 867 cells — nobody reads
// 867 lines. So the cap is applied at PRESENTATION only, exactly the way
// `verify_db` already bounds its own fault list (`surface_db_admin.hpp`'s
// `kSurfaceDbVerifyMaxFailures` / `DbVerifySpec::max_reported_failures` /
// `DbVerifyReport::n_failures_elided`) — same vocabulary, same contract:
// truncation is COUNTED, never silent, and the totals stay exact.
inline constexpr std::size_t kSurfaceDbBuildMaxReportedFailedCells = 32;

// The bounded view of a report's failed cells: the prefix to show plus how many
// were left out. `reported` BORROWS `r.coverage.failed_cells` and must not
// outlive `r`. A cap of 0 reports nothing and elides everything (the counters
// still tell the truth), mirroring `DbVerifySpec::max_reported_failures == 0`.
struct ReportedFailedCells {
  std::span<const FailedCell> reported{};
  std::size_t n_elided{0};
};

// `reported.size() + n_elided == r.coverage.failed_cells.size()` always holds.
// The prefix is kept (not a sample) so the shown rows stay in the same
// deterministic (date, symbol) order the populate produced.
[[nodiscard]] ReportedFailedCells
reported_failed_cells(const SurfaceDbBuildReport &r,
                      std::size_t max_reported = kSurfaceDbBuildMaxReportedFailedCells) noexcept;

// The SAME silent-green trap one stage earlier still — at INGEST, before either
// predicate below can see anything at all (R1-b, review C-04).
//
// True iff the loader produced NOT ONE readable date (`n_dates_loaded == 0`) while
// at least one present file was a real defect (`n_load_errors > 0`). That is "the
// entire requested input is corrupt", and until this predicate existed it was
// indistinguishable, BY EXIT CODE, from "an intentional no-op window": the CLI's
// only nonzero verdicts read the config and fit stages, and neither stage ever
// runs when the ingest yields nothing. The reviewer's isolated reproduction —
// one `date=.../data.parquet` holding non-Parquet bytes — printed
// `n_dates_loaded 0 / n_load_errors 1 / cells_to_fit 0 / cells_ok 0`, empty
// stderr, and exit 0. A scheduler cannot act on that.
//
// The counters mean different things and both are load-bearing:
//   - `n_dates_loaded` is a DATE count: distinct in-range dates that produced at
//     least one board. Zero means the fit stage was handed an EMPTY board span.
//   - `n_load_errors` is a CELL count of REAL DEFECTS ONLY — a present file that
//     is unreadable/unparseable, has the wrong schema, or whose market inputs
//     quarantined the cell. It deliberately EXCLUDES `n_coverage_holes` (a
//     present, readable date that simply does not carry that symbol), which every
//     real sparse hive produces in quantity. Keying on `n_error` instead would
//     fire on a healthy discover-all build of a sparse universe.
//
// Three neighbouring shapes stay green, and the second term is what keeps them so:
//   - NO DATES REQUESTED / NONE PRESENT (`n_load_errors == 0`) is the un-pulled
//     window — weekends, holidays, a range ahead of the pull. `n_dates_missing`
//     is large and nothing is wrong. This is a documented graceful no-op and the
//     build's convergence guarantee depends on it staying exit 0.
//   - A HEALTHY CONVERGED RESUME loads its dates fine (`n_dates_loaded > 0`), so
//     the first term excludes it whatever else the run did.
//   - PARTIAL corruption (some dates readable, some not) has `n_dates_loaded > 0`
//     and is NOT a failure — the readable dates were built. The CLI prints a loud
//     stderr WARNING naming `n_load_errors` and still exits 0, because a window
//     that produced real surfaces is not a dead build and a scheduler must not
//     retry it as one.
//
// DISJOINT FROM BOTH EXIT-3 PREDICATES BELOW, and from the carry-masked warning,
// by a single REACHABILITY fact rather than by an algebraic conjunct (the same
// style of argument `is_total_config_failure` uses for its own carry clause):
// `n_dates_loaded == 0` holds exactly when the loaded board span is EMPTY —
// `build_surface_db` inserts a date into `loaded_dates` and pushes a board on the
// very same branch — and an empty span is handed to BOTH later stages. So
// `generate_symbol_configs` sees zero symbols (`n_disabled_failed`,
// `n_disabled_existing`, `n_configured`, `n_skipped_existing` all 0) and
// `populate_universe_streaming` returns its all-zero coverage before doing
// anything. Each of the other three predicates requires a STRICTLY POSITIVE term
// from those zeroed counters — `is_total_config_failure` needs `disabled > 0`,
// `is_total_fit_failure` needs `cells_to_fit > 0`, `is_carry_masked_fit_failure`
// needs `cells_carried > 0` — so none of them can fire on any report this one
// fires on. `SurfaceDbTotalLoadFailure.NeverOverlapsAnotherVerdict` pins the
// reachability link on a REAL corrupt-window build rather than asserting it.
//
// IF THEY EVER DID CO-FIRE, THIS ONE WINS, and the CLI tests it FIRST for that
// reason: it is the most upstream cause. A config stage that disabled everything
// because it was handed nothing is a consequence of the corrupt ingest, and
// telling the operator to fix their universe when their data is unreadable sends
// them to the wrong place. Both map to the same exit code, so the ordering only
// decides which diagnostic is printed.
//
// Maps to `kExitTotalFitFailure` (3) — no new code. 3 already means exactly "the
// build ran to completion and produced NOTHING, and here is why"; a corrupt-only
// window is that, one stage earlier. A fourth code would force every existing
// script to learn it to keep the same behaviour.
[[nodiscard]] bool is_total_load_failure(const SurfaceDbBuildReport &r);

// Did this build attempt work and get NOTHING out of it? True iff it scheduled at
// least one cell (`coverage.cells_to_fit > 0`) and not one of them fitted
// (`coverage.cells_ok == 0`) — the signature of a systematically wrong build
// input, the carry-rate mismatch (`OpraHiveSpec.r` disagreeing with the rate the
// hive's quotes were priced under) being the top suspect: every put-call-parity
// forward is then wrong and every full fit fails identically.
//
// Deliberately NARROW — the three neighbouring shapes are all healthy and must
// not be swept in:
//   - PARTIAL failure (`cells_ok > 0` with some `cells_failed`) is normal in
//     production: real hives carry unfittable boards. Not a failure.
//   - NOTHING TO DO (`cells_to_fit == 0`) is the resume path over an already
//     complete database (and the un-pulled empty window). The build's convergence
//     guarantee is exactly "a re-run fits zero", so this must stay a success.
//   - CARRIED-ONLY (`cells_carried > 0` with `cells_ok == 0`) is FIX-D's converged
//     steady state: a date holding one or more PERMANENTLY-failing cells is
//     rewritten on every run, its failures are retried forever (by design — there
//     is no persisted known-failed state), and its healthy siblings are carried
//     rather than re-fitted. So `cells_ok` is legitimately 0 on a database that is
//     entirely healthy. Before carry-over those siblings were re-fitted and
//     `cells_ok` was large, which is the only reason this predicate did not
//     already misfire.
//
// That last shape is why the `cells_carried` clause exists and must not be
// dropped. Getting it wrong is not a cosmetic false alarm: the CLI's diagnostic
// names a carry-rate mismatch as the top suspect and tells the operator to re-run
// with a different `--r`, which on a healthy converged database would invalidate
// every surface in it.
//
// KNOWN LIMIT of the carry clause, stated in its GENERAL form — the exemption is
// keyed on `cells_carried == 0`, so ANY run that carried at least one cell is
// exempt from this predicate, whatever became of the cells it scheduled. That is
// a strictly WIDER set than "a converged database", and the cost is wider than
// the wrong-`--r` case alone:
//
//   - A run whose every SCHEDULED cell failed for a SYSTEMATIC reason — a fitter
//     regression, a broken loader for a newly-added name, a bad config for it —
//     beside a large healthy CARRIED population exits 0. Adding one ticker to a
//     converged 1030-name universe puts `to_add == 1` on every date, so every
//     date is rewritten: ~257k cells carried, ~250 scheduled, and if all 250 die
//     the report is `cells_to_fit = 250, cells_failed = 250, cells_ok = 0,
//     cells_carried >> 0`. Before carry-over that same run re-fit everything and
//     exited 3.
//   - A genuinely wrong `--r` over a converged database is one INSTANCE of that,
//     not the whole of it: the healthy cells are carried, never re-fitted, so
//     they never re-fail.
//
// A staleness check comparing each stored record's S/r/now_ts_ns against the
// loaded board's MarketEnv is the right home for the stale-input question, but it
// inspects CARRIED cells and this loss is entirely in the SCHEDULED ones — it
// cannot recover the verdict. Do not record it as the complement.
//
// The trade is still the right way round and must not be undone: a false TOTAL
// FIT FAILURE on EVERY healthy resume, telling the operator to re-run with a
// different `--r`, would invalidate every surface in the database if followed.
// What is restored is the SIGNAL, not the verdict — `is_carry_masked_fit_failure`
// below names the ambiguous shape on stderr while the exit code stays 0. The
// operator manual (`atx-vol/docs/surface-db-build.md`, the "Known limit" block in
// [Interest rate / carry]) states the same limit for the CLI's audience; the two
// are deliberately one statement in two registers, so change them together.
//
// Pure predicate over the report; the CLI uses it to pick its exit code, which is
// why the decision lives here (testable) and not in `main`.
[[nodiscard]] bool is_total_fit_failure(const SurfaceDbBuildReport &r);

// The signal the carry exemption gave up — a WARNING, never a verdict.
//
// True iff this run fitted NOTHING (`coverage.cells_ok == 0`), something it
// offered the fitter FAILED (`coverage.cells_failed > 0`), and it CARRIED at
// least one healthy stored surface (`coverage.cells_carried > 0`). That is
// exactly the shape `is_total_fit_failure`'s carry clause exempts, and it is
// genuinely AMBIGUOUS between two runs no counter can tell apart:
//
//   - the CONVERGED STEADY STATE: N permanently-failing cells retried forever
//     (there is no persisted known-failed state, by design) beside their healthy
//     carried siblings. Nothing is wrong; this is the shape FIX-D exists to
//     produce.
//   - EVERY SCHEDULED CELL DIED for a systematic reason, beside a carried
//     population that was never re-fitted and so could not re-fail.
//
// This predicate deliberately makes NO claim about which one happened. It says
// only that the run is one of the two and that `coverage.failed_cells` — each
// carrying the fitter's own reason — is where the answer is. Both interpretations
// print the same counters, which is precisely why the tool has to say they are
// different rather than leave the operator to notice.
//
// THE EXIT CODE STAYS 0. A non-zero exit here would reintroduce the C1 defect the
// carry clause was added to fix: the first shape above IS a healthy production
// database, and failing it would once again tell an operator to re-run with a
// different `--r`. Any future change that maps this predicate to an exit code
// must first show it cannot fire on a converged database — and it can.
//
// IT MAY NEVER CLEAR, AND THAT IS NOT A DEFECT (FIX-G). On a database holding
// permanently-failing cells this fires on EVERY run, forever — that is the
// `prod-2026-07` condition, not an edge case: 9 cells over 8 symbols on 8 dates,
// each of those symbols healthy on its other 16. Only two actions clear it and on
// that population usually neither applies. "Fix the cell" needs the failure to be
// a defect, and three of those nine are genuinely arbitrage-violating boards.
// "Disable the name" (`atx-vol-surface-db disable`, the operator-facing form of
// `SurfaceDb::upsert_symbol` with `enabled = false`) is a per-SYMBOL switch aimed
// at a per-CELL problem, so it costs the name on every date it fits.
//
// The CLI therefore says so, and names the failing cells on the warning line, so
// what the operator watches is the SET CHANGING rather than the line existing. A
// recurring line an operator is never told is expected is a line they stop
// reading — and reading (b), the systematic regression this exists to surface,
// goes with it. Any future edit that makes this quieter must keep the naming: the
// cell set is the only thing that separates (a) from (b).
//
// COUNTER CHOICE: `cells_carried`, never `cells_carried_disabled`. `cells_carried`
// is the term that GRANTS the exemption, so the warning must be keyed on the same
// term or it would not cover what was given up. FIX-E's `cells_carried_disabled`
// (stored surfaces of a switched-OFF symbol, preserved rather than deleted) is
// read by neither exit-code predicate, so a run carrying only those is NOT exempt
// — it still exits 3, and warning on it would duplicate that verdict.
//
// Disjoint from BOTH exit-3 predicates by construction, not just the one this
// warning was carved out of (M-A): `is_total_fit_failure` requires
// `cells_carried == 0` and so does `is_total_config_failure`, while this requires
// `> 0`, so all three are pairwise disjoint on that single term. The CLI can
// never emit a verdict and this hedge for one run, and that is a property of the
// predicates rather than of the order `main` tests them in.
// `SurfaceDbCarryMaskedFitFailure.NeverOverlapsEitherExitCode` proves it over the
// whole small-counter space rather than sampling one point, so dropping the
// `cells_carried` conjunct from EITHER exit predicate fails there.
[[nodiscard]] bool is_carry_masked_fit_failure(const SurfaceDbBuildReport &r);

// The SAME silent-green trap, one stage earlier — and invisible to the predicate
// above. When per-symbol CONFIG SELECTION fails for every symbol the stage tried,
// every config is stored disabled (fail-closed), so nothing is ever scheduled:
// `cells_to_fit == 0`, `cells_ok == 0`, and `is_total_fit_failure` reads that as
// the healthy "nothing to do" resume. The build exits 0 over a database with no
// enabled symbol that will never hold a surface.
//
// True iff the config stage left at least one symbol DISABLED and not one symbol
// ENABLED, and the run produced no surface at all — neither fitted nor CARRIED
// (`coverage.cells_ok == 0 && coverage.cells_carried == 0`). The shape mirrors
// `is_total_fit_failure` deliberately: attempted > 0, succeeded == 0.
// Read off the STANDING state rather than this run's fresh verdicts:
//   disabled = n_disabled_failed + n_disabled_existing
//   enabled  = n_configured + (n_skipped_existing - n_disabled_existing)
//   => disabled > 0 && enabled == 0 && cells_ok == 0 && cells_carried == 0
//
// The `cells_carried` conjunct is FIX-D fix-1's, and it is UNREACHABLE TODAY by
// the argument in the .cpp — it is there so that the two predicates read the same
// evidence for "did this run produce a surface at all", which is the coupling
// whose absence produced the C1 false TOTAL FIT FAILURE one stage down.
//
// The `n_disabled_existing` term is FIX-C-2's: without it, a RESUME over a
// database whose every symbol is stored disabled counted as `n_skipped_existing`,
// scheduled nothing, and exited 0 — the same dead database as a first run that
// disabled everything, reported as the healthy nothing-to-do path. The predicate
// asks "does this database serve any symbol at all?", which no run-local counter
// can answer on its own.
//
// Equally narrow, for the same reasons — four neighbouring shapes stay green:
//   - PARTIAL selection failure (some symbol is enabled alongside some
//     `n_disabled_failed`) is normal: a real universe carries names whose board
//     cannot pin a curve, and they are disabled while the rest build.
//   - NOTHING TO DO (nothing disabled) covers both the resume over an
//     already-configured db (every symbol `n_skipped_existing` and enabled) and
//     the empty window (no symbols seen at all). Convergence needs this.
//   - NEW NAMES FAILING BESIDE PRODUCTIVE FITS: only newly-seen symbols failed
//     selection while already-configured ones went on to fit. `cells_ok > 0`, so
//     the run produced surfaces — partial, not dead.
//   - CARRIED-ONLY (`cells_carried > 0`): the run re-emitted stored surfaces, so
//     it demonstrably produced a populated database. Same class of evidence as
//     `cells_ok > 0`; see `is_total_fit_failure` above for why FIX-D made this a
//     shape a `cells_ok == 0` test can no longer distinguish from a dead build.
//
// The CLI maps this to the SAME exit code as a total fit failure: both answer the
// one question "did this run produce anything at all?", and a script branching on
// the exit does not care which stage swallowed the universe (the stderr
// diagnostic names the stage).
[[nodiscard]] bool is_total_config_failure(const SurfaceDbBuildReport &r);

// Run the whole build (see `SurfaceDbBuildSpec`). Idempotent/resumable: re-running
// over an unchanged hive re-fits ZERO (configs skip-existing, the populate's
// cell-aware filter writes no date) ONCE every loaded cell has either fitted
// successfully or been config-disabled. A cell that FAILS to fit is deliberately
// retried — there is no persisted known-failed state — so it keeps its date in the
// rewrite set and that date's siblings are re-fit on every run: the price of giving
// a transient failure another chance. A grown hive fits only the new dates. An
// EMPTY window (un-pulled days) is a graceful success with all-zero coverage — the
// db is still created. Top-level Err only on a malformed hive spec (`load_opra_hive`)
// or a db config/write failure; a single unloadable/unselectable board never
// aborts the build (it is tallied and, for config, stored disabled).
[[nodiscard]] Result<SurfaceDbBuildReport> build_surface_db(const SurfaceDbBuildSpec &spec);

// Write `r` as a four-section CSV (reuses `write_populate_stats_csv`'s formatting
// discipline: an owned buffer flushed to a binary/truncating stream, IoError on
// open/write failure). Section 1 is a `key,value` table of every scalar counter
// (config.*, coverage.*, and the ingest counters n_dates_loaded / n_dates_missing
// / n_load_errors / n_coverage_holes); section 2 is a `config_disabled_symbol`
// row per `config.failed_symbols` entry; section 3 is a
// `symbol,n_attempted,n_ok,n_failed,n_disabled,n_carried` row per
// `coverage.per_symbol` entry; section 4 is a `date,symbol,code,detail` row per
// `coverage.failed_cells` entry — the WHOLE list, never the printed cap, because
// this file is where an operator goes to root-cause the lost cells. The first
// line is always the pinned header `key,value`.
//
// `coverage.cells_carried` (section 1) and `n_carried` (section 3) are FIX-D
// fix-1's: with `is_total_fit_failure` widened for the carried-only resume, these
// are the only operator-visible evidence that carry-over ran. Both were APPENDED
// to their sections rather than inserted, so a positional reader of the older
// columns is unaffected.
//
// Section 2 is FIX-C-2's: the config stage's disabled NAMES used to exist only on
// stdout, so the durable artifact could say "1 symbol was disabled" and never
// which one — and on every run after the first it did not say even that. The
// section is emitted with its header even when empty, so a consumer parses the
// same shape whether or not anything is disabled.
//
// `detail` is free text from the fitter, so that ONE field is always emitted
// RFC4180-quoted (wrapped in `"`, embedded `"` doubled) — a rejection message may
// legitimately contain a comma and must not be able to shift the columns. Every
// other field in every section is a bare token and stays unquoted.
[[nodiscard]] Status write_build_report_csv(const SurfaceDbBuildReport &r, std::string_view path);

} // namespace atx::vol
