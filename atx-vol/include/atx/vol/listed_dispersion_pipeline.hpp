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
#include "atx/vol/corpus.hpp"                     // CorpusAdmissionRule
#include "atx/vol/dispersion_workflow.hpp"        // RunSpec, batch_spec
#include "atx/vol/listed_dispersion.hpp"          // ListedForwardLookup, DispersionMember, ListedOptionQuote
#include "atx/vol/listed_dispersion_reconciliation.hpp" // ListedReconciliationSnapshot, reconcile_listed_dispersion
#include "atx/vol/listed_dispersion_schedule.hpp" // ListedRiskLookup, ListedOptionRisk
#include "atx/vol/listed_opra.hpp"                // ListedDefinitionTable, MissingDefinitionPolicy
#include "atx/vol/types.hpp"                      // Result

namespace atx::vol {

// Per-vol-point → per-unit-vol factor (M9 / I4). `spec.gross_index_vega` is dollars
// vega per VOL POINT per side; the library dispersion configs take dollars vega per
// UNIT vol (a unit vol is 100 vol points), so the boundary that hands the library a
// target vega scales by exactly this. Replaces the hand-applied literal `* 100.0`
// scattered at two boundaries in the example. The extraction must be sizing-exact.
inline constexpr double kVegaVolPointToUnitVol = 100.0;

// One versioned methodology policy replacing the loose inline literals previously
// scattered across build-corpus / build-schedule / verify / run-projected-backtest.
// Thresholds are pinned to the current production values (51 / 60 / 3 / 40).
struct ListedDispersionMethodology {
  // Corpus admission rule (fit-quality gate applied to every profile).
  CorpusAdmissionRule admission{};
  // Entry-gate floor: SPY plus at least 50 constituent names (core mode).
  std::size_t min_names_entry{51};
  // Core-mode acceptance floors.
  std::size_t core_min_dates{60};
  std::size_t core_min_rolls{3};
  std::uint32_t core_min_names_per_roll{40};
  // Canonical query route for the cold economics (route P).
  QueryExecution query_route{QueryExecution::ColdReference};
  // Whether every qualified date must carry OCC ESS settlement authority.
  bool occ_ess_authority{true};

  // Deterministic, nonzero, order-independent identity over all policy fields.
  // Two policies that differ in any single field produce different fingerprints.
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
// gate")`. It enforces NO other floor: in particular `core_min_names_per_roll` is
// NOT consulted here (that methodology field is inert in the example's build path;
// activating it would be a new gate). Returns `Ok()` when the schedule is accepted.
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
[[nodiscard]] Result<ListedDispersionSchedule>
build_listed_dispersion_schedule(const Clock &clock, const ListedScheduleSpec &spec,
                                 const ListedDispersionMethodology &method,
                                 std::span<const UniverseRow> universe_rows,
                                 const ListedDefinitionTable &definitions,
                                 const RunSpec &quote_source);

// ── Cold projection (M6, I1) ────────────────────────────────────────────────────

// Per-roll snapshot provider for the cold projection: resolve (load) the surface
// archive for a roll date and hand back a BORROWED MarketSnapshot. Replaces the
// example's inline `archive_of` map + `MarketSnapshot::load` (spy_dispersion_backtest
// .cpp:691-714). The returned pointer must stay valid for the duration of the
// `project_listed_schedule` call — the caller owns snapshot lifetime, matching the
// example where each per-roll snapshot lives across its member repricing.
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

} // namespace atx::vol
