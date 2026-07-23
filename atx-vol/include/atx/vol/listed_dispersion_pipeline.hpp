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

} // namespace atx::vol
