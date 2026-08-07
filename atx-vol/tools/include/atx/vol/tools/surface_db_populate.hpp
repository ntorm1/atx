#pragma once

// surface_db_populate — the "fit + store" pipeline stage: fit OPRA boards
// (CorpusBoard, corpus.hpp) into a SurfaceDb, honoring each symbol's
// SymbolFitConfig from the db manifest, and store one partition per date.
//
// This is deliberately NOT `build_corpus` pointed at a SurfaceDb: the ONLY
// fit-policy difference from a plain corpus build is that EVERY board's fit
// inputs are overlaid with the symbol's manifest config (`apply_symbol_config`)
// before the fit runs, so a per-symbol operator override (a pinned curve, a
// tighter band_k, ...) reaches the actual fit — see populate_surface_db's
// doc-comment below. The board -> PricedSurface fit itself reuses the exact
// same blessed path `build_corpus` runs (src/corpus_board_fit.{hpp,cpp});
// this header/impl adds only the per-symbol config resolution, date grouping,
// uid stamping, and SurfaceDb partition writes on top.
//
// ── The shared resource underneath (plan 4.7, carried here at 5.6) ───────────
//
// `SurfaceDbPopulateConfig::n_threads` is a REQUEST AGAINST A POOL, not a count
// of threads this stage creates for you. The outer board fan-out is capped at
// the host's core budget, and each board's inner fit dispatches onto the
// PROCESS-GLOBAL pricing pool (detail/pricing_executor.hpp) rather than spawning
// its own fan. Three consequences a caller has to know, because no signature
// here names the pool:
//
//   * concurrent populate calls SHARE one core budget instead of oversubscribing
//     the machine — "safe from many threads" is not "spawns its own threads";
//   * a populate issued from INSIDE another pool dispatch runs inline rather
//     than nesting a second fan;
//   * results do not depend on any of it. Every fit writes disjoint slots, so
//     the partitions are byte-identical at any worker count (the invariant is
//     restated at `UniversePopulateCoverage::failed_cells` and pinned by
//     SurfaceDbPopulate.SharedWorkerBudgetKeepsOutputByteIdentical and
//     GlobalParallelQueuePreservesDeterministicPartitions).
//
// The one ORDERING rule comes from that same header and reaches callers here:
// choose the pool's topology with `configure_pricing_executor` BEFORE the first
// call that can price, because that call is what BUILDS the pool and a later
// configure is refused with AlreadyExists and applies nothing. Configure once,
// from one thread. Nothing in atx-vol does it for you. `pin_outer_workers`
// below is a different knob — it steers which cores the OUTER workers land on,
// it does not configure the pool, and it changes no result either.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "atx/vol/corpus.hpp"     // CorpusBoard
#include "atx/vol/fit_policy.hpp" // FitAdmissionPolicy (Task 2: populate publish floor)
#include "atx/vol/pricer_fitter.hpp" // ExpiryBuildOutcome, ExpiryFitOutcome (SliceDropCell, Task 3)
#include "atx/vol/tools/run_report.hpp" // MetaKv
#include "atx/vol/surface_db.hpp" // SurfaceDb, SymbolFitConfig
#include "atx/vol/types.hpp"      // Result, Status

namespace atx::vol {

// ── Task 2: the populate publish floor ───────────────────────────────────────
// The quote-fidelity publication gate for every populate fit. The evidence
// (worst per-expiry frac_fv_within_bidask, scored against the fit's OWN
// admitted rows — curve_fit.cpp:918) and the check (fit_policy.cpp:144-149,
// QualityBelowFloor) both pre-exist; this floor is what arms them. 0.35:
// blocks the demonstrated 2025-04-10 publish (worst long slices ~5% in-band;
// any floor >= 0.3 refuses it) while sitting far under calm-day boards
// (>= ~55% against the strictly harder full-chain population; ~94.65%
// board-wide on the served SPY regression board). A refused board lands in
// the pre-existing FailedCell lane; safe mode retains the previously stored
// record. TIGHTENING ONLY — no oracle/QP tolerance moves.
inline constexpr double kPopulateMinWorstFracInBand = 0.35;

// WP12 Mark-serving defaults plus the floor above. Every other field is the
// default on purpose: the strict Risk shape gates already run separately via
// RiskAdmission::Required (the no-arb oracle), and structural coverage is the
// mark_domain/tenor-truncation policies' job downstream.
[[nodiscard]] FitAdmissionPolicy populate_admission_policy() noexcept;

struct SurfaceDbPopulateConfig {
  // Base fit inputs (preset etc). Per-symbol SymbolFitConfig from the db
  // manifest is overlaid via apply_symbol_config; a symbol absent from the
  // manifest uses `fallback` unchanged.
  SymbolFitConfig fallback{};
  unsigned n_threads{0};    // 0 = serial; determinism must hold regardless
  bool skip_existing{true}; // date key already in db.partitions() -> skip whole date
  // C4 wave-2 (perf, finding 13): cap the outer worker budget at the physical
  // P-cores and pin outer workers to them (the compute-bound fit path regresses
  // past the P-core count on a hybrid P/E host). Default ON. Set false to measure
  // the unpinned/uncapped scaling curve or on hosts where the pin is unwanted;
  // results are byte-identical either way (pinning only steers scheduling).
  bool pin_outer_workers{true};
  // FIX-D carry-over: date -> canonical symbols whose ALREADY-STORED surface is
  // to be re-emitted into the rewritten partition instead of re-fitted. A carried
  // cell is not dispatched to the fitter at all; its record is read back with
  // `reconstruct_entry` (byte-lossless, gated by SurfaceArchiveV2.Reemit* tests)
  // and appended to the write. The CALLER owns the validity decision -- this
  // struct carries no predicate. `populate_universe_streaming` populates it; a
  // direct caller that leaves it empty gets exactly the previous behaviour.
  std::map<std::string, std::vector<std::string>> carry_over{};
  // Does this caller ATTEST that the partitions this populate writes may be
  // stamped with the manifest's current config fingerprint -- i.e. blessed for a
  // later resume to CARRY instead of re-fitting? Forwarded verbatim to
  // `SurfaceDb::write_partition`; see `DbConfigAttestation` for the full argument
  // and for the fold's write-time semantics.
  //
  // FIX-D fix-2 (I-3). `populate_surface_db` used to pass `FitterProduced`
  // UNCONDITIONALLY, on the strength of a comment asserting that every carried
  // item had itself been admitted by a fingerprint gate. That gate is real, but it
  // lives one frame UP in `populate_universe_streaming` -- and the field above says
  // in as many words that this struct carries no predicate. So a direct caller who
  // filled `carry_over` themselves got their stored surfaces re-emitted verbatim
  // AND stamped with a current-config fingerprint, making the staleness STICKY
  // (re-blessed by every later resume) rather than one-shot. That is the exact
  // outcome the attestation was added to prevent, asserted in a comment as though
  // it had been checked.
  //
  // The claim now travels WITH the decision: whoever fills `carry_over` is the only
  // one who can vouch for it, so they are the one who sets this. The default is
  // `None`, which fails CLOSED -- an unstamped partition folds to the 0 "unknown"
  // sentinel and is re-fit rather than reused, so forgetting to attest costs one
  // wasted re-fit and never a silently carried stale surface.
  //
  // `populate_universe_streaming` sets `FitterProduced` because IT ran the gate
  // (`carry_valid`: a non-zero stored fingerprint equal to a freshly recomputed
  // fold over the partition's symbols). A direct caller of `populate_surface_db`
  // that fits everything itself under this manifest's configs and carries nothing
  // may honestly set it too; one that supplies a `carry_over` it has not gated
  // must not.
  DbConfigAttestation attest{DbConfigAttestation::None};
  // ── REV-R3 (review C-02 / F-02): the coverage-regression opt-out ────────────
  //
  // A partition write is WHOLE-FILE (tmp + rename, no merge), so a stored symbol
  // that is missing from a date's candidate item list is DESTROYED by the commit.
  // By default `populate_surface_db` REFUSES such a write: it leaves the existing
  // partition untouched, counts the date in `n_dates_refused_coverage_regression`
  // and names the at-risk cells in `coverage_regression_cells`. Fail CLOSED —
  // this defaults to `false` because losing a stored surface is irreversible and
  // silent (the format keeps no tombstone, so a destroyed cell is byte-for-byte a
  // cell that was never fitted), while a refusal costs one run.
  //
  // Set `true` only for a run that INTENDS retirement — deliberately shrinking a
  // date's stored coverage. The regression is still DETECTED and still reported
  // (same counters, in `n_dates_dropped_coverage_regression`), so the run leaves
  // an audit record of exactly which surfaces it destroyed; only the refusal is
  // waived.
  //
  // WHAT IT DOES NOT WAIVE (REV-R3 fix-1, review I-3): the abort on an existing
  // partition FILE that will not OPEN. That was fused onto this flag and has been
  // split off — it is now unconditional. The two are different claims. "I have
  // read the named list and I want those surfaces gone" does not imply "and
  // please also overwrite any partition you could not parse", and because this
  // flag is WHOLE-RUN, the operator retiring one cell would have been opted into
  // the second waiver for every date in the run. An unreadable partition has no
  // named list to authorise, so there is nothing here for a caller to consent
  // to; the remedy is to delete the file (surface_db.hpp's documented remedy #1),
  // which makes it the ordinary first-write path.
  bool allow_coverage_regression{false};
  // Safe default for an intentional date rewrite (`skip_existing=false`):
  // successful fits replace same-symbol records, while every existing record
  // without a successful replacement is retained. This includes failed and
  // disabled refits as well as symbols absent from the incoming board set.
  //
  // Set true only for an operator-requested destructive replacement. In that
  // mode the rewritten partition contains successful incoming fits only, so a
  // failed/disabled/absent existing cell may be removed.
  bool destructive_rewrite{false};
  // Cooperative cancellation (plan item 5.5). Default-constructed => never
  // cancels. Polled in TWO places:
  //   * at the TOP OF EACH QUEUED FIT TASK — the run's dominant cost — so a stop
  //     drains the fit queue at one relaxed load per board instead of running
  //     every already-queued fit to completion;
  //   * in the DRAIN LOOP, at the top of each date and again after that date's
  //     fits have been waited on, both before its partition write. The second of
  //     those is what stops a date whose boards skipped their fit from being
  //     committed with cells that were never attempted.
  //
  // This is the SAFEST of the four cancellable entries, and for a structural
  // reason: `SurfaceDb::write_partition` atomically commits a date's archive file
  // (tmp+rename) together with a generation-bumped manifest, so every date the
  // drain loop already finished is durably and consistently on disk. Cancelling
  // between dates therefore leaves a VALID database holding a prefix of the
  // requested dates — not a damaged one — and a re-run resumes from it under the
  // ordinary `skip_existing` / carry-over path with no special recovery.
  //
  // The stop does not abandon in-flight fit workers — a board already inside its
  // fit runs to the end of that fit — and the drain loop leaves by the same route
  // the scheduler-incomplete path already uses, so the fit-runner jthread is
  // joined before `populate_surface_db` returns `ErrorCode::Cancelled`. What the
  // stop does reach is every board still QUEUED behind those, which is what keeps
  // the return prompt and keeps peak RSS at O(dates in flight).
  //
  // The referenced flag must outlive the populate call.
  CancelToken cancel{};
};

struct PopulateSymbolStats {
  std::string symbol;
  std::uint32_t n_attempted{0};
  std::uint32_t n_ok{0};
  std::uint32_t n_failed{0};
  std::uint32_t n_disabled{0}; // skipped because manifest enabled=false
  // FIX-D: cells re-emitted from the existing partition instead of re-fitted.
  // Deliberately NOT folded into n_ok: n_ok means "cells this run FITTED", which
  // is what `is_total_fit_failure`'s cells_ok == 0 clause depends on.
  std::uint32_t n_carried{0};
  // Mean fit-quality score over successful fits, when the shared corpus fit
  // path yields one (oos_in_band from curve selection; see corpus.cpp's
  // CorpusEntry.oos_in_band recording). NaN when unavailable (e.g. the
  // pinned-curve path has no OOS score — mirrors corpus.cpp).
  double mean_oos_in_band{std::numeric_limits<double>::quiet_NaN()};
};

// One (date, symbol) cell whose fit FAILED, carrying the reason the fit itself
// produced. `n_failed` answers HOW MANY cells died; this answers WHICH and WHY.
//
// `detail` is the fit `Error`'s own message, verbatim — for the risk pipeline
// that is PricerFitter's formatted rejection string naming the failing gate, the
// offending slice, the log-moneyness and the slack. It is deliberately NOT a
// re-derivation: an operator must be able to root-cause a lost cell from the run
// report alone, which is exactly what a bare `cells_failed` count could not do.
//
// Purely diagnostic. Recording a failure does NOT make it sticky: there is no
// persisted known-failed state (see `surface_db_build.hpp`'s ruling), so a cell
// listed here is retried on the next run exactly as it was before.
struct FailedCell {
  std::string date;   // partition key (the board's date)
  std::string symbol; // the board's symbol
  // The fit `Error`, split into its two halves; `detail` is "" when the Error
  // carried no message.
  ErrorCode code{ErrorCode::Unknown};
  std::string detail;
};

// Task 3 (mark-domain-robustness observability, plan 2026-08-02). One
// non-Fitted expiry from a WRITTEN cell's PUBLISHED attempt -- the surface-db
// fitter silently drops long-dated expiries on stressed days, and this is the
// per-slice record that makes a drop visible instead of only showing up as an
// extrapolated backtest mark. `outcome` is the admission layer's own taxonomy
// (`ExpiryBuildOutcome`, pricer_fitter.hpp) read off `FitSlot::slice_drops`
// (see corpus_board_fit.cpp) -- NEVER `Fitted`, by construction, since that
// value is filtered out before it reaches here.
//
// `fit_outcome` is the FIT DRIVER's own, finer-grained reason (Fix Round 1):
// `ExpiryBuildReport::fit_outcome` (pricer_fitter.hpp), populated by
// `completed_attempt_report` from `VolaSession::expiry_fit_reports()` when
// `outcome == Missing`. Meaningful ONLY then -- see that field's own doc for
// exactly which of the fit driver's `ExpiryFitOutcome` values can appear and
// which default (`Fitted`, the enum's 0 value) means "no rich reason is
// available", the same convention this field inherits. The vocabulary includes
// `PrepUncovered` (Task 1: admitted rows fail the k-coverage predicate) and
// `FitRefusedCalendar` (Task 6: a ConvexDense slice refused because the
// previous, UNCOVERED committed slice's calendar floor bound it only outside
// that slice's own data range) -- both are truthful truncations of an
// otherwise-served board, not defects.
//
// Distinct from `FailedCell`: a `FailedCell` is a WHOLE (date, symbol) cell
// whose fit failed outright and produced no surface at all; a `SliceDropCell`
// is a PARTIAL drop inside an otherwise successfully fitted, WRITTEN board --
// the surface is real and served, it just does not reach as far in tenor as
// its board's raw quotes suggested it should.
struct SliceDropCell {
  std::string date;
  std::string symbol;
  double T{0.0};
  ExpiryBuildOutcome outcome{ExpiryBuildOutcome::Missing};
  std::size_t n_used{0u};
  ExpiryFitOutcome fit_outcome{ExpiryFitOutcome::Fitted};
};

// REV-R3. One (date, symbol) surface that IS stored in a date's existing
// partition and is ABSENT from the candidate partition a rewrite would commit —
// i.e. a surface the write would destroy. `symbol` is the CANONICAL archive key
// (ASCII-upper, truncated to `kSurfaceDbKeyMax`), because that is the form the
// partition's directory stores and the form the comparison is made in.
//
// Whether the surface actually died depends on the run: with the guard on
// (default) the date was refused and the surface is still on disk untouched;
// with `allow_coverage_regression` the write went ahead and this is the record of
// what it destroyed. The two are told apart by the two date counters, never by
// this list.
//
// DETERMINISM: appended by the single drain thread, dates ascending, and within a
// date in ascending CANONICAL SYMBOL order (a `set_difference` of two sorted
// ranges — the partition directory and the candidate item list, both sorted
// before the compare). No unordered container and no completion order can reach
// it, so it is byte-identical for any `n_threads`.
struct CoverageRegressionCell {
  std::string date;
  std::string symbol;
};

struct SurfaceDbPopulateStats {
  std::uint32_t n_boards{0};
  std::uint32_t n_ok{0};
  std::uint32_t n_failed{0};
  std::uint32_t n_carried{0}; // FIX-D: cells re-emitted from the existing partition
  // FIX-E: present-but-DISABLED cells re-emitted verbatim so a rewrite does not
  // DELETE them. Deliberately separate from `n_carried`: that counter means
  // "healthy stored surface reused instead of re-fitted" and is read as evidence
  // the run produced a serviceable database (`is_total_fit_failure`), which a
  // preserved disabled cell is not. A cell counted here is ALSO counted in the
  // per-symbol `n_disabled` -- it was not fitted; only its bytes were kept.
  std::uint32_t n_carried_disabled{0};
  std::uint32_t n_dates_written{0};
  std::uint32_t n_dates_skipped_existing{0};
  // REV-R3. Dates whose candidate partition was NOT a superset of the stored one
  // and which were therefore NOT written: the existing partition is untouched and
  // every surface it held is still on disk. Non-zero means the run did not do
  // what was asked, which is why the build CLI maps it to a non-zero exit.
  std::uint32_t n_dates_refused_coverage_regression{0};
  // REV-R3 fix-2 (review N-3). Of the dates in the counter above, how many had a
  // partition FILE on disk that the manifest does not list. Always <= it. This is
  // a CAUSE discriminator, not a second refusal counter: a refusal on a listed
  // partition means this run's cells failed to fit (check the build inputs, the
  // carry rate first), while a refusal on an unlisted one means the manifest and
  // the disk disagree (nothing failed; the run was narrower than a file the index
  // does not know about) and the remedies are different enough that offering the
  // wrong one is harmful. Refusals only — the destructive branch prints no
  // cause advice, so it needs no discriminator.
  std::uint32_t n_dates_refused_partition_unlisted{0};
  // REV-R3. The same detection on a run that set `allow_coverage_regression`:
  // the date WAS written and the stored surfaces named in
  // `coverage_regression_cells` for it are GONE. This exists so the destructive
  // mode still leaves a record — the 95-surface incident was invisible precisely
  // because nothing counted or named what it removed.
  std::uint32_t n_dates_dropped_coverage_regression{0};
  // Every cell behind the two counters above, ascending by (date, canonical
  // symbol). COMPLETE, never capped — display bounding is the CLI's job, the same
  // split `failed_cells` uses.
  std::vector<CoverageRegressionCell> coverage_regression_cells;
  std::vector<PopulateSymbolStats> per_symbol; // sorted by symbol
  // One entry per cell counted in `n_failed`, ascending by (date, symbol).
  //
  // DETERMINISM (a repo invariant: identical output for any thread count):
  // entries are appended by the SINGLE drain thread as it walks dates in
  // ascending order and, within a date, boards in ascending (date, symbol) sort
  // order — never by a fit worker. Completion order therefore cannot reach this
  // list, and it is byte-identical for any `SurfaceDbPopulateConfig::n_threads`.
  std::vector<FailedCell> failed_cells;
  // Task 3 (mark-domain-robustness observability). Every non-Fitted expiry
  // from a WRITTEN date's fresh (not carried) fits, ascending by (date,
  // symbol, chain index) -- the same drain-thread ordering `failed_cells`
  // uses, and gated the same way `n_dates_written` is: a date the coverage
  // guard REFUSED contributes nothing here, because its slice_drops describe
  // a partition that was never actually committed.
  std::vector<SliceDropCell> slice_drops;
  // Count of WRITTEN dates with at least one entry in `slice_drops` above.
  std::uint32_t n_dates_with_slice_drops{0};
  // The minimum, over WRITTEN dates, of that date's longest fitted pillar --
  // i.e. min over dates of (max over that date's WRITTEN surfaces'
  // `PricedSurface::tenor_domain().max_T`, surfaces with an empty domain
  // excluded). 0.0 when no written date produced a single non-empty domain
  // (mirrors `TenorDomain::empty()`'s own convention: a real max_T is always
  // > 0). A day where even the best-reaching surface fell short is exactly
  // the "stressed day drops long-dated expiries" signature this task exists
  // to surface.
  double max_T_min{0.0};
};

// Deterministic test seam for the streaming/per-date-release path. Production
// callers pass nullptr (the default). Both callbacks default-empty; when set
// they MUST be thread-safe: `before_board_fit` runs on a fit worker thread
// immediately before each board's fit begins, `after_partition_write` runs on
// the draining thread immediately after a date's partition is written. The sole
// use is the streaming test, which blocks a later date's board until an earlier
// date's partition lands to prove writes are streamed (not deferred to a single
// global join) — see SurfaceDbPopulate.StreamsPartitionsBeforeGlobalJoin.
struct PopulateTestHooks {
  std::function<void(const std::string &date, const std::string &symbol)> before_board_fit{};
  std::function<void(const std::string &date)> after_partition_write{};
  // U4 (R-14) / FIX-4: observe EVERY inner fit-worker budget OFFER, together with
  // the live outstanding-board count the offer was resolved against
  // (`inner_fit_workers == inner_budget / min(inner_budget, boards_outstanding)`;
  // 0 = auto sizing, the outer-serial mode). FIX-4 made the budget live, so this
  // fires more than once per board: once at claim and once per surface-build
  // request the fit issues (see surface_db_populate.cpp's reclaim block).
  //
  // THREAD-SAFETY CHANGED WITH FIX-4: this now runs on the fitting WORKER thread
  // and several boards can resolve concurrently — the callback must synchronize
  // itself. Carrying `boards_outstanding` is what makes a test able to assert the
  // offer function per offer instead of racing on "the" resolved value.
  std::function<void(const std::string &symbol, unsigned inner_fit_workers,
                     std::size_t boards_outstanding)>
      on_inner_fit_workers{};
  // FIX-4: fires immediately after a board's fit slot is complete and the
  // outstanding-board count has been decremented; receives the POST-decrement
  // count. This is the deterministic release seam for the straggler gate: a test
  // holding one board inside `before_board_fit` can wait for
  // `boards_outstanding == 1` instead of sleeping. Runs on the fit worker thread
  // (concurrently across boards); a throwing callback is swallowed because this
  // fires from a destructor.
  std::function<void(std::size_t boards_outstanding)> on_board_fit_done{};
  // R1-a (review C-06): forwarded verbatim to the fit scheduler's own
  // `FitSchedulerTestHooks` so a PRE-TASK scheduler failure can be injected
  // through the FULL populate path rather than only in a scheduler-local unit
  // test. Both run on the fit-runner thread before any board is claimed; a throw
  // from either makes `run_bounded_fit_tasks` return a non-Ok Status with not one
  // `MarkDone` having fired, which is the exact shape that used to deadlock the
  // per-date drain. Declared as bare std::function rather than by embedding
  // `detail::FitSchedulerTestHooks` so this public header keeps no dependency on
  // the detail/ scheduler header. Empty in production (the whole struct is
  // nullptr there).
  std::function<void(std::size_t worker_ordinal)> before_worker_launch{};
  std::function<void()> before_scheduler_setup{};
  // Backward-compatible name used by the C-9 remediation tests. When both launch
  // hooks are populated, both run (the canonical hook above first).
  std::function<void(std::size_t worker_ordinal)> before_fit_worker_launch{};
};

// Fit every board and store one partition per distinct board date (key =
// date). Eligible boards share one bounded dynamic queue across all dates; the
// fits stream: each date's partition is aggregated, written, and RELEASED in
// ascending date order as soon as that date's fits complete, while later dates
// are still being fit — so peak RSS is O(dates in flight), not O(all dates).
// Results (surfaces + stats) are byte-identical to a launch-then-join-then-write
// populate: every board's fit is independent/deterministic and the drain visits
// dates and boards in the same deterministic date/symbol order. A board whose
// symbol's manifest config has
// enabled=false is skipped (n_disabled). A board whose fit fails records
// n_failed and does NOT abort the date (document per-name failures, don't
// silently drop). A date with zero successful fits writes NO partition.
//
// W-EX — A FIT THAT THROWS IS A FAILED CELL, NOT A FAILED BUILD. A C++ exception
// escaping the per-board fit (std::bad_alloc under memory pressure is the live
// case) is caught ON THE WORKER and recorded as exactly the failure above:
// `n_failed`, and a `FailedCell` whose `code` is `Internal` and whose `detail`
// is `"fit worker exception: <what()>"`. The date, its other symbols and every
// later date are unaffected, and this function still returns Ok. It used to
// travel to the bounded scheduler's catch(...), which discarded the message and
// returned Err(Internal) for the WHOLE call -- abandoning work that had not even
// been scheduled yet -- and the destructor / jthread bodies the unwind passed
// through are implicitly noexcept, so one shape of it killed the process
// outright with nothing printed on either stream. No path from a fit to
// std::terminate remains: the worker boundary, `MarkDone`'s destructor, the
// fit-runner jthread body and the bounded scheduler are each closed
// independently. Gate:
// SurfaceDbPopulate.WorkerExceptionFailsOnlyItsOwnCellAndTheBuildContinues.
//
// THE COVERAGE GUARD (REV-R3, review C-02/F-02). A date's write is REFUSED when
// its candidate item list is not a SUPERSET of the symbols the existing partition
// already holds — because the write is whole-file, so every stored symbol missing
// from the candidate would be destroyed by the commit. The existing set is read
// from the partition's own DIRECTORY (`SurfaceArchiveV2::directory()`, FIX-H's
// precedent), never inferred from the manifest — and so is the decision that a
// partition EXISTS at all (REV-R3 fix-1, review I-1): the lookup is
// `SurfaceDb::open_partition_file`, which skips the manifest, so a partition file
// present on disk but unlisted is compared against rather than overwritten. A
// refusal is PER-DATE: the existing partition is left untouched, the date is
// counted in `n_dates_refused_coverage_regression`, the at-risk cells are named
// in `coverage_regression_cells`, and every OTHER date in the run proceeds
// normally. This is not an Err — the run continues and reports.
// `allow_coverage_regression` opts out for a run that intends retirement; see
// that field, and note it does NOT waive the unreadable-partition Err below.
// Partition write uses SurfaceArchiveItem{symbol, &surface} with owning
// symbol-string storage kept alive across the call, and forwards
// `cfg.attest` to it — so by DEFAULT the partitions this writes carry NO
// config fingerprint and a later resume re-fits them rather than reusing them
// (fail closed; see `SurfaceDbPopulateConfig::attest`).
// TOP-LEVEL Err — every `return Err` in this function's own frame, in source
// order. REV-R6 re-derived this list by reading them all, because the previous
// wording claimed to be exhaustive and was short three entries (2, 5 and 8):
//   1. an EMPTY boards span (`InvalidArgument`).
//   2. `skip_existing`'s EXISTENCE PROBE failing: any `open_partition` error
//      other than `NotFound`, on a date this run is about to SKIP rather than
//      rewrite. Entry 6 is the rewrite-path twin, and is the one the paragraphs
//      below argue; this one is the same read on a date nothing would have
//      written, and it aborts too.
//   3. a FIT-SCHEDULER TERMINATION that left a date's fits UNSTARTED (below).
//   4. a CARRY READ-BACK failure (below).
//   5. `with_uid` rejecting a surface that FITTED, while stamping its symbol uid
//      onto the `PricedSurface` about to be archived.
//   6. an UNREADABLE EXISTING PARTITION on a date about to be rewritten (below).
//   7. `db.write_partition` failing — a db write error, or a date key the db
//      rejects.
//   8. the fit scheduler's Status, read AFTER the join: a fit task that failed
//      or threw MID-RUN, once at least one date had already drained. Distinct
//      from 3, which is pre-task and writes nothing; this one leaves every date
//      already committed on disk, which is the point of
//      `SurfaceDbPopulate.CompletedDatesSurviveLaterWorkerThrow`.
// NOTHING ENFORCES THIS LIST — no test enumerates this function's Err sites, so
// it is only as complete as its last reader. A new `return Err` belongs here in
// the commit that writes it.
//
// THE UNREADABLE EXISTING PARTITION (REV-R3, review C-02/F-02 and I-3). A date
// whose partition FILE exists and will not open (any `SurfaceArchiveV2::open_file`
// error other than `NotFound`) aborts the whole populate, propagating the archive's
// own error verbatim. This is a NEW Err on a path that previously could not produce
// one: such a partition used to be silently overwritten. The reasoning is the
// coverage guard's: a file whose bytes cannot be read cannot be asked what it
// holds, so whether the rewrite destroys anything is unanswerable, and the rewrite
// is the one action that makes it permanently unanswerable. It is UNCONDITIONAL —
// `allow_coverage_regression` does not waive it (I-3); deleting the unreadable
// file does, by making the date a first write.
//
// THE FIT-SCHEDULER TERMINATION (R1-a, review C-06). `run_bounded_fit_tasks` has
// two PRE-TASK failure returns — a background-worker launch failure and a
// scratch-allocation failure — on which NOT ONE board is fitted and therefore not
// one per-date completion counter is ever decremented. The per-date drain used to
// sleep on those counters forever: the process hung with no output, and never
// reached the point where it could observe the scheduler's Status. It now wakes on
// scheduler termination as well, and returns the SCHEDULER'S OWN Status (never a
// newly invented code — the scheduler's message already names the cause) for the
// first date left incomplete. No partition is written from a partially-fitted
// date. Dates already fully drained AND written before the failure stay on disk:
// each `write_partition` is an atomic tmp+rename plus a generation-bumped
// manifest, committed by the drain before the scheduler's Status is ever read, and
// a re-run's cell-aware filter skips them. The date is the resume unit and that is
// correct.
//
// THE CARRY READ-BACK FAILURE (FIX-F, M-6). A cell named in `carry_over` whose
// stored record cannot be opened, found, or reconstructed is a hard Err that
// aborts the whole populate — it does not skip the record and carry on. This is
// wider than it was before FIX-E: an ENABLED carry only reaches the read-back
// behind the caller's fingerprint gate, while a DISABLED (preserved) carry
// reaches it on EVERY rewrite of that date, including on pre-FIX-D databases
// whose records are the oldest in the deployment. Failing loud is deliberate:
// the only alternative is to write the partition without the record, which is
// the very deletion the preserve exists to prevent, performed on the one record
// already known to be unreadable. Nothing already earned is lost — this date's
// `write_partition` has not run, and every earlier date is already committed and
// will be skipped by a re-run.
[[nodiscard]] Result<SurfaceDbPopulateStats>
populate_surface_db(SurfaceDb &db, std::span<const CorpusBoard> boards,
                    const SurfaceDbPopulateConfig &cfg = {},
                    const PopulateTestHooks *test_hooks = nullptr);

// ── F-c: universe-scale, cell-aware resumable populate ──────────────────────
//
// The universe populate driver's testable core. Wraps populate_surface_db with
// (1) per-symbol manifest-config seeding (an index leg pinned to the dense recipe,
// every other symbol left on the preset's auto-selector) and (2) CELL-AWARE
// idempotent resume: a partition (= date) is (re)written only when a loaded board
// adds a symbol the partition does not already carry, so re-running as the OPRA
// pull dribbles in new (symbol,date) cells fits only the new work and a re-run over
// unchanged data fits ZERO once every loaded cell has either fitted successfully or
// been config-disabled (a disabled cell is excluded from the pending tally; a cell
// that FAILS to fit is not, so it is retried — see build_surface_db's contract).
// Uses the fused streaming populate_surface_db underneath
// (per-date fit->serialize->release on the executor pool), so RSS stays
// O(dates in flight). The caller loads the hive (load_opra_daterange ->
// corpus_board_from_opra) and hands the available boards in; this function owns the
// config seeding, the resume filtering, the populate call, and the coverage report.
//
// Determinism: byte-identical across fit_workers (populate_surface_db invariant);
// the resume filter is a deterministic date/symbol grouping.
//
// SAFETY (FIX-E): the other way a rewrite could drop a stored surface is a symbol
// that IS in this run's loaded set but whose manifest config is DISABLED. That
// cell is never fitted, so a naive rewrite simply omitted it — and the guard
// above could not fire, because the cell had already been counted into the same
// number the guard compares. Such a cell is now re-emitted VERBATIM from the
// existing partition (`cells_carried_disabled`), unconditionally and independent
// of the carry-over fingerprint: `enabled = false` means stop fitting this
// symbol, never delete what is already stored.
//
// SAFETY (REV-R3, review C-02/F-02): the THIRD way — the one both guards above
// are structurally blind to — is a loaded, ENABLED, already-stored cell whose
// re-fit FAILS. Nothing is appended for it, and the whole-file rewrite drops it.
// The filter cannot see this coming (the cell is counted into `present` before it
// is fitted), so the check now ALSO runs in the WRITE path, on the actual
// candidate item list, as a SET comparison against the existing partition's own
// directory. A date whose candidate is not a superset is not written:
// `dates_refused_coverage_regression`, with the at-risk cells named in
// `coverage_regression_cells`. `UniversePopulateSpec::allow_coverage_regression`
// opts out for a retirement run.
//
// SAFETY: a date rewrite merges successful fits into its existing partition by
// default. Existing cells whose incoming refit fails (and existing symbols absent
// from the loaded set) retain their exact serialized record bytes. Destructive
// replacement is available only through the explicit flag below.
struct UniversePopulateSpec {
  std::string index_symbol{};        // pinned to the dense index recipe; empty = none
  FitPreset preset{FitPreset::Fast}; // non-index symbols use this preset's auto-selector
  unsigned fit_workers{0};           // 0 = auto (honors ATX_VOL_FIT_WORKERS); 1 = serial
  // REV-R3: forwarded verbatim to `SurfaceDbPopulateConfig::allow_coverage_regression`.
  // Default false = the guard is ON = a rewrite that would destroy a stored
  // surface is refused for that date. See that field for the full argument.
  bool allow_coverage_regression{false};
  // Existing cells without a successful replacement are retained by default.
  // Opt in only when the rewritten partition should contain successes from the
  // incoming board set and nothing else.
  bool destructive_rewrite{false};
};

struct UniversePopulateCoverage {
  std::uint32_t cells_loaded{0};             // boards handed in (available parquet cells)
  // NEW (symbol,date) cells scheduled this run. A cell whose resolved config is
  // DISABLED is never counted: it can never be added to a partition, so treating
  // it as pending work would keep its date in the rewrite set forever.
  std::uint32_t cells_to_fit{0};
  // Already-present cells dragged back through the FITTER by a same-date
  // rewrite. FIX-D: this is now the FAILURE mode, not the normal one -- a cell
  // only lands here when its stored surface could not be validated for reuse
  // (see cells_carried). On a resume over an unchanged database and hive this
  // must be 0; a nonzero value means something invalidated the carry.
  std::uint32_t cells_refit{0};
  // FIX-D: already-present cells whose STORED surface was re-emitted verbatim
  // into the rewritten partition instead of being re-fitted. Byte-identical to
  // what was there before (SurfaceArchiveV2.Reemit* is the gate). These are NOT
  // counted in cells_ok, which keeps meaning "cells this run fitted".
  std::uint32_t cells_carried{0};
  // FIX-E: already-present cells whose manifest config is DISABLED and whose
  // stored surface was therefore re-emitted verbatim rather than deleted.
  // `enabled = false` means STOP FITTING this symbol; it does not mean DELETE
  // what is already stored. Before FIX-E such a cell was counted `present`,
  // excluded from the carry set AND skipped by the populate, so a whole-partition
  // rewrite triggered by any UNRELATED new symbol on the same date silently
  // destroyed it -- and the would-drop guard below could not see it, because the
  // cell had already been counted into the very number that guard compares.
  //
  // Kept OUT of `cells_carried` on purpose: that counter means "healthy stored
  // surface reused instead of re-fitted" and `is_total_fit_failure` /
  // `is_total_config_failure` read it as proof the run produced a serviceable
  // database. A preserved disabled surface is not that proof -- it is a config
  // the operator has switched off, whose bytes are merely not being thrown away.
  // These cells are in neither `cells_refit` (never offered to the fitter) nor
  // `cells_ok`.
  std::uint32_t cells_carried_disabled{0};
  std::uint32_t cells_already_present{0};    // skipped: symbol already in its date partition
  // populate n_ok / n_failed over the dates this run put through the fitter. They
  // count FIT outcomes, not commits: a cell that fitted on a date the write path
  // then REFUSED (REV-R3) is still `cells_ok`, because it really did fit — it just
  // did not land. `dates_refused_coverage_regression` is the authority on what
  // reached disk, which is why the CLI's verdict reads it separately.
  std::uint32_t cells_ok{0};
  std::uint32_t cells_failed{0};
  std::uint32_t dates_total{0};              // distinct dates among the loaded boards
  // Dates this run really COMMITTED — taken from the populate's own write-site
  // count, not from the filter's "dates I intend to rewrite". REV-R3 fix-1
  // (review M-2): it was the filter's count minus the write path's refusals,
  // which still overcounted a date whose candidate ended up EMPTY (no write, and
  // no refusal either, because the guard is gated on a non-empty candidate).
  // `dates_written + dates_refused_coverage_regression` is therefore NOT the
  // number of dates chosen for rewrite; nothing here promises that identity.
  std::uint32_t dates_written{0};
  // Dates with NOTHING left to add: every loaded cell is either already in the
  // partition or config-DISABLED (a disabled cell can never be added, so a date
  // whose only gap is disabled symbols is complete, not pending).
  std::uint32_t dates_skipped_complete{0};
  // Retained for report-schema compatibility. Safe merge makes this zero; an
  // explicit destructive rewrite is audited by the write-path coverage counters.
  std::uint32_t dates_skipped_would_drop{0};
  // ── REV-R3: the SECOND would-drop guard, and why there are two ─────────────
  //
  // `dates_skipped_would_drop` above is the FILTER's guard and it fires BEFORE
  // any fit: it catches a stored symbol that this run's loaded board set does not
  // mention at all. It is a COUNT comparison against `part->count()` and it is
  // structurally blind to the case that destroyed 95 production surfaces — a cell
  // that IS loaded, IS enabled, and whose re-fit FAILS. Such a cell is counted
  // into `present` before its fit outcome is known, so the count still matches
  // and the filter waves the date through.
  //
  // This counter is the WRITE path's guard (`populate_surface_db`), which runs
  // after the fits with the actual candidate item list in hand and compares SETS,
  // not counts. Every date it names was NOT written; its stored surfaces are
  // untouched on disk, and `dates_written` above never counted it (it is the
  // write site's own count).
  std::uint32_t dates_refused_coverage_regression{0};
  // REV-R3 fix-2 (review N-3). The subset of the counter above whose partition
  // file was on disk but NOT listed in the manifest — a crash between
  // `write_partition`'s archive rename and its manifest persist, a restored older
  // manifest, a hand-assembled root, or a `drop_partition` interrupted (or whose
  // unlink failed) after its manifest commit. Always <=
  // `dates_refused_coverage_regression`. The CLI reads it to pick which cause it
  // names in the refusal banner, because the wrong-carry-rate advice is actively
  // misleading for this state. Carried straight through from the populate.
  std::uint32_t dates_refused_partition_unlisted{0};
  // Dates written ANYWAY under `allow_coverage_regression`. The surfaces named
  // for those dates in `coverage_regression_cells` are gone.
  std::uint32_t dates_dropped_coverage_regression{0};
  // Every cell behind the two counters above, ascending by (date, canonical
  // symbol), complete and uncapped. Carried straight through from the populate.
  std::vector<CoverageRegressionCell> coverage_regression_cells;
  // Per-symbol dispositions from the underlying populate — over every date this
  // run PROCESSED, which since REV-R3 is NOT the same set as the dates it WROTE.
  // A date the coverage guard REFUSED ran its fits in full and withheld only the
  // commit: every counter here (`n_attempted`, `n_ok`, `n_failed`, `n_disabled`,
  // `n_carried`) is accumulated in the drain's per-cell walk, which completes
  // before the write decision is taken, so a refused date contributes rows while
  // `dates_written` never counts it. This line said "written dates only" — the
  // same staleness the two comments above were corrected for ("they count FIT
  // outcomes, not commits") and this one was missed (REV-R5, review M-2).
  //
  // Dates the resume filter SKIPPED (`dates_skipped_complete`) contribute nothing:
  // they never enter the walk at all. So the set is "processed", not "written" and
  // not "all requested".
  std::vector<PopulateSymbolStats> per_symbol;
  // WHY each of `cells_failed` failed, ascending by (date, symbol) — the fit
  // stage's answer to `AutoConfigReport::failed_symbols`, which names the symbols
  // the CONFIG stage is not serving (as of FIX-C-2 that is the STANDING disabled
  // set — this run's refusals plus the ones already stored disabled — not just
  // this run's). Carried straight through from the underlying populate (same
  // determinism guarantee), so `failed_cells.size() == cells_failed`: the
  // populate appends exactly one entry here on the same branch that increments
  // `n_failed`, and nothing else touches either.
  //
  // REV-R6: that identity holds over the same PROCESSED set as `per_symbol`
  // above, NOT over "the dates this run wrote", which is what this line used to
  // say. Both sides accumulate in the drain's per-cell walk, which finishes
  // before the coverage guard's refusal decision and before the write — so a
  // REFUSED date contributes to both while `dates_written` never counts it. Same
  // staleness the paragraph above was corrected for, eight lines away from it.
  std::vector<FailedCell> failed_cells;
  // Task 3 (mark-domain-robustness observability). Carried straight through
  // from the underlying populate's `SurfaceDbPopulateStats::slice_drops` /
  // `n_dates_with_slice_drops` / `max_T_min` — see those fields for the exact
  // contract. Unlike `per_symbol` / `failed_cells` above, these ARE scoped to
  // dates this run actually WROTE (a refused date's partial fits are not on
  // disk, so their slice drops describe nothing a reader could load).
  std::vector<SliceDropCell> slice_drops;
  std::uint32_t n_dates_with_slice_drops{0};
  double max_T_min{0.0};
};
// NOTE: the cell counters do NOT reconcile against `cells_loaded`. A DISABLED cell
// that is absent from its partition on a skipped-complete date is in none of
// `cells_to_fit` / `cells_refit` / `cells_already_present`, and
// `PopulateSymbolStats::n_disabled` covers only the dates this run PROCESSED —
// its increment is in the same pre-write walk as every other counter in
// `per_symbol` (see that field), so it takes rows from a REFUSED date and takes
// nothing at all from a date the resume filter skipped as complete. Either way
// it is short of the loaded set, which is the point here. (REV-R6: this NOTE
// said "the dates this run WROTE", contradicting the field doc 18 lines above
// that it annotates.) So `cells_loaded` is the input count, not the sum of a
// partition. Treat it as such: there is deliberately no disabled-cell counter
// here.

// Seed per-symbol configs (idempotent) then cell-aware-resume-populate the given
// boards into `db`. Empty `boards` is a graceful no-op (all-zero coverage), NOT an
// error — an un-pulled window legitimately yields no boards.
//
// TOP-LEVEL Err — every `return Err` in this function's own frame (REV-R6, same
// re-derivation as `populate_surface_db`'s list above):
//   1. `db.upsert_symbol` failing while seeding a per-symbol config.
//   2. anything `populate_surface_db` itself Errs on, propagated VERBATIM — which
//      since REV-R3 includes an UNREADABLE EXISTING PARTITION on a date this run
//      was going to rewrite (see that function's contract above; it is not waived
//      by `allow_coverage_regression`).
//   3. TWO `ErrorCode::Internal` cross-checks this function raises ITSELF, added
//      by FIX-D and FIX-E and NOT covered by the two clauses above: the
//      populate's `n_carried`, and its `n_carried_disabled`, each compared
//      against the carry set this function asked for. They assert this file's own
//      two halves agree; nothing a caller passes can trip them, and no test pins
//      either. The sentence they falsified predated them and survived an edit to
//      its very next clause, which is why they are named here rather than folded
//      into "or on anything the populate Errs on".
// A coverage REFUSAL is deliberately not an Err: the run completes and reports it
// in `dates_refused_coverage_regression`.
[[nodiscard]] Result<UniversePopulateCoverage>
populate_universe_streaming(SurfaceDb &db, std::span<const CorpusBoard> boards,
                            const UniversePopulateSpec &spec,
                            const PopulateTestHooks *test_hooks = nullptr);

// Stats file for the report: meta (caller's, plus n_boards/n_ok/n_failed/
// n_carried/n_dates_written appended), header
// "symbol,n_attempted,n_ok,n_failed,n_disabled,success_rate,mean_oos_in_band,n_carried",
// one row per symbol (%.10g; mean_oos_in_band prints "nan" when NaN).
//
// success_rate = n_ok / (n_attempted - n_disabled - n_carried), and "nan" when
// that denominator is EMPTY. Both the n_carried subtraction and the nan are
// FIX-D fix-1's (I3): a carried cell was never offered to the fitter, so it
// belongs in neither half of a FIT success rate, and a symbol with no fitted-or-
// failed cell at all has no such rate — the previous `max(1, …)` floor reported
// 0% for it, which on a converged carry resume is every healthy symbol in the
// database. `n_carried` is APPENDED to the row so a positional reader of the
// older columns is unaffected.
[[nodiscard]] Status write_populate_stats_csv(const SurfaceDbPopulateStats &s, const MetaKv &meta,
                                              std::string_view path);

} // namespace atx::vol
