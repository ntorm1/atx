#pragma once

// Listed-dispersion economics — the listed-route home.
//
// This module owns the ~350-400 LOC of listed-dispersion orchestration and route
// economics that were stranded inside the spy_dispersion_backtest example CLI, so
// each subcommand becomes one library call and the extracted economics are finally
// reachable by unit tests. It is named the *listed-route home* so strangle/mag7
// never depend on it.
//
// Wave B Task 1 lands the module foundation: the vega/parity constant, the
// versioned methodology policy struct, and the three per-date adapter seams
// (`listed_quotes_for_date`, `make_listed_forward_lookup`, `make_listed_risk_lookup`)
// lifted verbatim from the example.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "atx/vol/backtest.hpp"                   // MarketSnapshot, QueryExecution (via query_pricing.hpp)
#include "atx/vol/contract_projection.hpp"        // ProjectedMaturitySpec, ProjectedOption
#include "atx/vol/corpus.hpp"                     // CorpusAdmissionRule
#include "atx/vol/dispersion.hpp"                 // DispersionBook
#include "atx/vol/dispersion_workflow.hpp"        // RunSpec, batch_spec
#include "atx/vol/historical_projection.hpp"      // HistoricalProjection{Scenario,Frame,Config}, ProjectedHistoricalVar
#include "atx/vol/listed_dispersion.hpp"          // ListedForwardLookup, DispersionMember, ListedOptionQuote
#include "atx/vol/listed_dispersion_reconciliation.hpp" // ListedReconciliationSnapshot, reconcile_listed_dispersion
#include "atx/vol/listed_dispersion_schedule.hpp" // ListedRiskLookup, ListedOptionRisk
#include "atx/vol/listed_dispersion_strategy.hpp" // ListedDispersionStrategy, MarkDivergence
#include "atx/vol/listed_opra.hpp"                // ListedDefinitionTable, MissingDefinitionPolicy
#include "atx/vol/types.hpp"                      // Result

namespace atx::vol {

// Forward-declared so the schedule builder can accept the CLI's phase timer by
// pointer without this header pulling in run_diagnostics.hpp. Defined in
// atx/vol/run_diagnostics.hpp; the .cpp includes it for the definition.
class PhaseTimer;

// Per-vol-point → per-unit-vol factor (M9 / I4). `spec.gross_index_vega` is dollars
// vega per VOL POINT per side; the library dispersion configs take dollars vega per
// UNIT vol (a unit vol is 100 vol points), so the boundary that hands the library a
// target vega scales by exactly this. Replaces the hand-applied literal `* 100.0`
// scattered at two boundaries in the example. The extraction must be sizing-exact.
inline constexpr double kVegaVolPointToUnitVol = 100.0;

// The versioned authority for the listed route's ENTRY AND ACCEPTANCE FLOORS,
// replacing the loose inline literals that were scattered across build-corpus and
// build-schedule. Values are pinned to the current production numbers (51/60/3).
//
// SCOPE — read this before adding a field. This struct is the authority for the
// three floors below and nothing else. It is deliberately NOT the home of:
//   * the archive verify floors — `RunVerifyOptions` (`run_archive.hpp`) carries
//     its own independent 60/3/40, because the result store must not depend on
//     the listed route. Those numbers duplicate these ON PURPOSE and must be
//     changed together;
//   * the cold-route knobs — `ProjectionConfig` below is the single asserted
//     parity constant both cold routes read;
//   * the corpus admission rule — the CLI builds its own `CorpusAdmissionRule`
//     with production values at `spy_dispersion_backtest.cpp:340-353`.
// An earlier revision carried an `admission` rule, a `core_min_names_per_roll`,
// a `query_route` and an `occ_ess_authority` here. No consumer ever read any of
// them: they were folded into the fingerprint and nothing else, and the
// `admission` default did not even match the production rule the CLI builds — a
// policy struct whose fields are silently ignored is worse than no policy struct,
// so they were removed rather than left to look authoritative.
struct ListedDispersionMethodology {
  // Entry-gate floor: SPY plus at least 50 constituent names (core mode).
  // Read by build-corpus (`spy_dispersion_backtest.cpp:324`).
  std::size_t min_names_entry{51};
  // Core-mode acceptance floors. `core_min_dates` is read by build-schedule
  // (`spy_dispersion_backtest.cpp:429`); `core_min_rolls` by
  // `accept_listed_schedule` below.
  std::size_t core_min_dates{60};
  std::size_t core_min_rolls{3};

  // Deterministic, nonzero identity over all policy fields. The fields are
  // folded in a FIXED order — the property is layout independence (the fold is
  // padding-free, so it does not depend on struct layout), NOT independence of
  // the field order itself. Two policies that differ in any single field
  // produce different fingerprints.
  [[nodiscard]] std::uint64_t policy_fingerprint() const;
};

// Verbatim lift of the example's `load_listed_quotes` (spy_dispersion_backtest.cpp
// :401-425): load the single-date OPRA batch for `symbols` and join every loaded
// panel to `definitions` under MissingDefinitionPolicy::SkipUnlisted. Needs live
// OPRA parquet, so it is not unit-tested here — its correctness is pinned by the
// T10 fixture gate.
[[nodiscard]] Result<std::vector<ListedOptionQuote>>
listed_quotes_for_date(const RunSpec &spec, const ListedDefinitionTable &definitions,
                       std::span<const std::string> symbols, std::string_view date);

// Forward-lookup seam for schedule selection (lift of spy_dispersion_backtest.cpp
// :480-491). Resolves each member's ATM forward at (expiry - snapshot.ts_ns()).
// The returned closure BORROWS `snapshot` by reference: it must not outlive the
// snapshot it was built from.
[[nodiscard]] ListedForwardLookup make_listed_forward_lookup(const MarketSnapshot &snapshot);

// Cold per-share risk-lookup seam for projected sizing (lift of
// spy_dispersion_backtest.cpp :733-742). Prices each exact contract through
// `full_greek_seed(strike, residual_T, side, analytic, execution)`. The returned
// closure BORROWS `snapshot` by reference: it must not outlive the snapshot.
[[nodiscard]] ListedRiskLookup make_listed_risk_lookup(const MarketSnapshot &snapshot,
                                                       double residual_T, bool analytic,
                                                       QueryExecution execution);

// M1 reconciliation-clock-coupling fix (design §3, review M1). The engine emits
// zero reconciliation rows until the first roll date, so `run-backtest` feeds the
// reconciler its FULL `clock.refs()` timeline — every session the clock spans,
// including any leading warm-up / low-coverage session before the first roll. The
// low-level `reconcile_listed_dispersion` hard-requires
// `snapshots.front().date == schedule.rolls.front().roll_date` and aborts an
// otherwise-valid corpus when a pre-roll session leads the timeline (it only
// worked historically because `date_lo` happened to coincide with the first roll).
//
// `assemble_reconciliation_snapshots` trims those leading pre-roll sessions so the
// returned span starts exactly at `schedule.rolls.front().roll_date`, mirroring the
// strategy's pre-roll silence. The coupling is made explicit here (an error if the
// first roll date is absent from the timeline) rather than left emergent; the
// low-level precondition stays intact as a defensive invariant.
//
// BORROW: the returned elements are copies of the `ListedReconciliationSnapshot`
// structs, but each one still points at storage owned by the caller — the
// `quotes` span and the `surfaces` set are borrowed from whatever backs
// `full_timeline`. That storage must outlive the returned vector and every
// reconcile driven from it.
//
// CAUTION (open defect, Wave B final review Important #1): trimming makes the
// reconciliation shorter than the backtest, and
// `validate_listed_reconciliation_backtest` still hard-requires equal row
// counts — so a nonzero lead-in currently aborts one call later instead of
// here. Do not treat this seam as closing M1 until that gate is date-aligned.
[[nodiscard]] Result<std::vector<ListedReconciliationSnapshot>>
assemble_reconciliation_snapshots(std::span<const ListedReconciliationSnapshot> full_timeline,
                                  const ListedDispersionSchedule &schedule);

// The single reconciliation entry point the CLI uses: assemble (trim the warm-up
// lead-in) then reconcile. Feeds `reconcile_listed_dispersion` a timeline whose
// front date is the first roll date by construction, so a leading pre-roll session
// no longer aborts the stage.
[[nodiscard]] Result<ListedDispersionReconciliation>
reconcile_listed_schedule(const ListedDispersionSchedule &schedule,
                          std::span<const ListedReconciliationSnapshot> full_timeline,
                          const ListedReconciliationConfig &config = {});

// ── Schedule build (M7) ───────────────────────────────────────────────────────

// The swept schedule knobs pulled out of `RunSpec` so the builder does not depend
// on the `RunSpec` layout. A POD carrying exactly the fields the selection loop
// reads; defaults mirror the `RunSpec` production defaults. The remaining `RunSpec`
// (OPRA root / path template / snapshot suffix) is handed separately as the quote
// source, since `listed_quotes_for_date` still needs the live parquet coordinates.
struct ListedScheduleSpec {
  double target_dte_days{30.0};
  double min_dte_days{21.0};
  double max_dte_days{60.0};
  double roll_dte_days{7.0};
  std::size_t min_names{10};
  double min_weight_coverage{0.8};
  double gross_index_vega{10000.0};
  bool core_mode{false};
};

// The entry/three-roll acceptance gate, factored out of the schedule builder so it
// is unit-testable without live parquet. Verbatim behavior of the example's final
// gate (spy_dispersion_backtest.cpp:532-534): an empty roll set fails the entry
// gate, and a core-mode schedule with fewer than `method.core_min_rolls` (3) rolls
// fails the three-roll gate — both `Err(Unavailable, "…entry/three-roll acceptance
// gate")`. It enforces NO other floor: in particular there is NO names-per-roll
// floor here. The example's build path never applied one, so adding it would be a
// new, behaviour-changing gate; the 40-name floor that exists lives in
// `RunVerifyOptions` and applies at archive-verify time, not at build time.
// Returns `Ok()` when the schedule is accepted.
[[nodiscard]] Status accept_listed_schedule(const ListedDispersionSchedule &schedule,
                                            const ListedScheduleSpec &spec,
                                            const ListedDispersionMethodology &method);

// Verbatim lift of `build_schedule_command`'s selection loop
// (spy_dispersion_backtest.cpp:446-535): per-date snapshot load, DTE roll-trigger,
// per-date universe rebind (`DropRenormalize`, `spec.min_names` floor), forward
// lookup (via `make_listed_forward_lookup`), the per-date OPRA quote join (via
// `listed_quotes_for_date` over `quote_source`), `select_listed_dispersion`,
// requested-vs-traded weight coverage gate, deferral (a failed/under-covered date
// after entry is skipped, not fatal), cohort numbering, the archive
// `surface_fingerprint`, `build_listed_dispersion_roll` sizing, and the entry/
// three-roll acceptance gate (`accept_listed_schedule`). The CLI keeps the phase
// timing, the `trade_schedule.tsv` write, and the section encoders (T9). The
// economic output is pinned byte-identical at T10 (trade_schedule golden
// b640b3ab...).
//
// M1 (design §3): after acceptance the builder validates that `clock` actually
// carries `rolls.front().roll_date` — true by construction (every roll_date is a
// clock ref date), but enforced here so the coupling run-backtest relies on
// (`reconcile_listed_schedule` trimming the full clock timeline down to the first
// roll date) is explicit rather than emergent. A first roll date absent from the
// clock is `Err(InvalidArgument)`.
//
// `timer` (optional, T9/O4): when non-null, the selection loop charges the same
// per-phase wall time the example measured inline before the lift — `selection`
// (snapshot load + universe resolve + select + roll build) and `quote_join` (the
// per-date OPRA parquet join) — into the CLI's PhaseTimer, so build-schedule's
// `diagnostics` section keeps its pre-lift per-phase granularity. The CLI keeps
// timing `setup_read` / `write_outputs` around its own reads/writes. Pure telemetry:
// it never affects the returned schedule, and a null timer (the default) is
// economically identical.
[[nodiscard]] Result<ListedDispersionSchedule>
build_listed_dispersion_schedule(const Clock &clock, const ListedScheduleSpec &spec,
                                 const ListedDispersionMethodology &method,
                                 std::span<const UniverseRow> universe_rows,
                                 const ListedDefinitionTable &definitions,
                                 const RunSpec &quote_source, PhaseTimer *timer = nullptr);

// ── Cold projection (M6, I1) ────────────────────────────────────────────────────

// Per-roll snapshot provider for the cold projection: resolve (load) the surface
// archive for a roll date and hand back a BORROWED MarketSnapshot. Replaces the
// example's inline `archive_of` map + `MarketSnapshot::load` (spy_dispersion_backtest
// .cpp:691-714). The caller owns snapshot lifetime.
//
// LIFETIME: the returned pointer must stay valid until the NEXT call to the
// lookup — that is, for as long as `project_listed_schedule` is processing that
// one roll. It does NOT have to survive the whole call. The projection
// dereferences the snapshot only inside the roll iteration that requested it
// (the rolls it emits are plain data: strikes, sizes and greeks, never pointers
// into the board), so a caller may release each board as soon as the next roll
// date is requested. Requiring whole-call validity would force every roll-date
// board — a full heap deserialize each, not an mmap — to stay resident, making
// peak memory scale with the roll count for no benefit.
using ListedArchiveLookup =
    std::function<Result<const MarketSnapshot *>(std::string_view roll_date)>;

// The cold-idealization knobs shared by BOTH cold routes (I1 — the headline gate).
// `project_listed_schedule` authors the persisted `projected_schedule` marks through
// these, and `run-projected-backtest --execution cold` recomputes its replay marks
// through the SAME constant (wired T9), so the two routes provably share ONE config
// instead of two hand-maintained copies that could silently drift (the I1 root cause).
// `analytic=true` + `QueryExecution::ColdReference` is the single asserted parity
// constant: certified cold Andersen-Lake greeks on the ColdReference route, no fast
// tier attached. Do NOT add drift here — every field participates in the parity.
struct ProjectionConfig {
  bool analytic{true};
  QueryExecution execution{QueryExecution::ColdReference};
};

// Verbatim lift of `project_schedule_command`'s cold reprice
// (spy_dispersion_backtest.cpp:700-825): for each frozen listed roll, resolve its
// snapshot via `archives`, guard the valuation-ts match / residual tenor / roll shape,
// rebuild each member straddle at the surface ATM-forward strike
// (`surface->forward_at(residual_T)`) priced at the cold model value (zero synthetic
// spread — there is no listed market at the interpolated strike) with cold certified
// greeks (via `make_listed_risk_lookup`, the SAME cold seed the replay recomputes
// through — the I1 shared path keyed on `cfg`), reuse the listed
// `build_listed_dispersion_roll` sizing VERBATIM, then `validate_listed_dispersion_schedule`.
// roll_date / valuation_ts_ns / cohort / expiry_ts_ns / n_names / weights / side are
// preserved from the listed build; ONLY per-member strike and its cold greeks differ.
// The CLI keeps the schedule-section + diagnostics + file writes (T9). The economic
// output is pinned byte-identical at T10 (projected_schedule golden d6793d46...).
[[nodiscard]] Result<ListedDispersionSchedule>
project_listed_schedule(const ListedDispersionSchedule &listed, const ListedArchiveLookup &archives,
                        const ProjectionConfig &cfg = {});

// ── Mark divergence observation (L10) ───────────────────────────────────────────

// One mark_divergence row: the frozen schedule mark vs the live seed mark for one
// leg on one session. Field order and names mirror the kMarkDivergenceCols registry
// order the example's encoder emits, so the CLI stages a row 1:1 with no reordering.
struct ListedMarkDivergenceRow {
  std::string date, symbol, raw_symbol;
  double strike{0.0};
  std::int64_t expiry_ts_ns{0};
  Side side{Side::Call};
  double schedule_mark{0.0}, live_mark{0.0}, diff{0.0}, abs_diff_bps_of_mark{0.0};
};

// |live - schedule| / |schedule| * 1e4, and EXACTLY 0.0 when |schedule| == 0.
// Lifted verbatim from the mark-divergence arithmetic in the example's
// `collect_mark_divergence_replay`. The zero-denominator collapse is finding L2 — a
// KNOWN understatement for deep-OTM legs with a frozen mark of 0. It is PRESERVED
// bit-for-bit here on purpose: the metric feeds a pinned artifact, so changing it is
// a separate, deliberate economic decision, not a refactor side effect.
[[nodiscard]] double listed_mark_divergence_bps(double schedule_mark, double live_mark) noexcept;

// Build a StepObserver that appends one row per leg whose live mark diverged from
// its frozen schedule mark on the step just observed. Requires the observed strategy
// to be a ListedDispersionStrategy (Record policy); anything else is a fail-closed
// InvalidArgument. `schedule` and `out` MUST outlive the run_backtest call that
// consumes the returned observer.
//
// BOOK INDEPENDENCE (the L10 equivalence property — do not weaken it). Every row is
// derived from exactly three inputs: the strategy's already-computed
// `last_mark_divergences()` / `next_roll_index()`, the frozen `schedule` handed in
// here, and the observed step's `ref.date` / `snapshot.ts_ns()`. It reads NO
// `PortfolioState`, no cash/hedge ledger, and nothing the engine mutates after
// `IStrategy::on_step` returns — which is what lets a shadow replay loop that steps
// an otherwise-unmanaged book produce byte-identical rows. Adding a book- or
// engine-mutation-order dependence here would silently invalidate that equivalence.
[[nodiscard]] StepObserver
make_mark_divergence_observer(const ListedDispersionSchedule &schedule,
                              std::vector<ListedMarkDivergenceRow> &out);

// ── Projected relative-template VaR (M8) ────────────────────────────────────────

// The materialized output of the book-projected historical VaR: the per-scenario
// aggregate frames, the scenario-major per-leg projections, one risk summary per
// requested confidence, and the position count. The CLI serializes these into its
// three bespoke loose-TSV schemas (projected_risk_scenarios / projected_risk_legs /
// projected_var) — out-of-archive per the design partition rule, so NOT folded into
// run.atxrun this wave (no schema bump). The library owns the economics; the CLI the
// serialization.
struct DispersionBookVar {
  std::vector<HistoricalProjectionFrame> frames; // one per scenario, scenario order
  std::vector<ProjectedOption> legs;             // scenario-major: scenarios * n_positions
  std::vector<ProjectedHistoricalVar> risks;     // one per requested confidence, input order
  std::size_t n_positions{0};                    // == book.positions.size()
  // The prepared relative-template projection's identity hash
  // (`PreparedHistoricalProjection::fingerprint()`), surfaced so the CLI can emit
  // the `projected_var.tsv` provenance column without rebuilding the projection.
  std::uint64_t prepared_fingerprint{0};
};

// Verbatim lift of run_projected_var_command's book -> OptionProjectionSpec synthesis
// + PreparedHistoricalProjection::evaluate_into + projected_historical_var per
// confidence (spy_dispersion_backtest.cpp:1119-1194). Each book position becomes a
// relative template — same uid / side / multiplier / qty, ATM-forward strike, and the
// caller's relative `maturity` — then the templates are re-projected onto every
// historical `scenario` and the loss quantile is split per requested confidence.
//
// `maturity` is the CLI's `dispersion.projected_maturity` (spy_dispersion_backtest
// .cpp:1124), which is defined at :1114 OUTSIDE the :1119-1194 lift range and so is a
// required input here (the plan's book-only signature omitted it). It MUST be the
// relative template (`ProjectedMaturitySpec::days(N)`), NOT an absolute expiry: the
// point of relative-template historical VaR is that each scenario re-ages the option
// to `scenario_valuation + maturity`; substituting the book's absolute projected
// expiry would freeze the tenor and change every non-entry scenario's risk.
//
// M9 note: the per-vol-point gross vega * kVegaVolPointToUnitVol scaling lives in the
// DispersionConfig builder that produces `book` (spy_dispersion_backtest.cpp:1110),
// upstream of this lift — so this function applies no vega x100; the book handed in is
// already sized. That boundary's `* 100.0 -> kVegaVolPointToUnitVol` replacement is a
// CLI line-item wired at T9.
//
// Returns Err(Unavailable) if any scenario projected an incomplete frame
// (n_failed != 0), matching the CLI's post-projection gate (:1175-1178); on that path
// no VaR is meaningful. On success `risks` carries one summary per confidence (loss =
// frames.back().value - scenario.value, the CLI's reference at :1188). The CLI keeps
// the loose-TSV writes (T9).
[[nodiscard]] Result<DispersionBookVar>
dispersion_book_var(const DispersionBook &book, const ProjectedMaturitySpec &maturity,
                    std::span<const HistoricalProjectionScenario> scenarios,
                    std::span<const double> confidences,
                    const HistoricalProjectionConfig &cfg = {});

} // namespace atx::vol
