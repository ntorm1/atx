#pragma once

// Library seam for the traditional SPY listed-options dispersion proxy.
//
// The command bodies that used to live inside examples/spy_dispersion_backtest.cpp
// (~620 LOC of library workflow trapped in an example main) live here so the
// example is thin CLI glue and every stage is unit-testable off the filesystem.
//
// REPRODUCIBILITY: the pinned admission thresholds / fingerprint material that
// determine which surfaces are admitted -- and therefore the dispersion golden
// (final_nav = 247.4065016443293, 82-session) -- are named library constants on
// DispersionCorpusPolicy below. No example-only literal is load-bearing.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "atx/vol/backtest.hpp"           // Clock, BacktestResult
#include "atx/vol/corpus.hpp"             // QualifiedCorpusConfig
#include "atx/vol/dispersion.hpp"         // DispersionUniverse
#include "atx/vol/dispersion_backtest.hpp"// DispersionBacktestConfig, run_dispersion_backtest
#include "atx/vol/dispersion_workflow.hpp"// RunSpec
#include "atx/vol/session.hpp"            // FitPreset
#include "atx/vol/tearsheet.hpp"          // TearSheet
#include "atx/vol/types.hpp"              // Result, Status
#include "atx/vol/vol_surface.hpp"        // VolCurveKind

namespace atx::vol {

// ── Pinned corpus/admission policy ──────────────────────────────────────────
//
// These defaults are the *only* load-bearing reproduction knobs for the listed
// SPY dispersion corpus. They were previously inline literals in
// build_corpus_command; changing any of them changes admission and therefore the
// golden NAV. dispersion_corpus_config() below assembles the QualifiedCorpusConfig
// exactly as the example driver did.
struct DispersionCorpusPolicy {
  // Per-profile admission thresholds (applied uniformly to every profile rule).
  std::uint32_t admission_min_quotes{20};
  std::uint32_t admission_min_slices{2};
  bool admission_require_calendar_arb_free{true};
  double admission_calendar_abs_k{0.7};
  bool admission_require_source_provenance{true};
  // Fit template pinned to the HFT market-mark preset with a direct linear-in-
  // variance curve and the calendar floor enforced.
  FitPreset fit_preset{FitPreset::Hft};
  VolCurveKind fit_curve_kind{VolCurveKind::LinearVariance};
  bool fit_enforce_calendar_floor{true};
  // Deterministic policy fingerprint material hashed into the quality sidecar.
  std::string_view policy_fingerprint_material{
      "spy-listed-dispersion-admission-v4-pinned-linear-calendar-floor-k0.7"};
};

// Nonzero 64-bit hash of `text` (0 folded to 1) — the corpus fingerprint hash.
[[nodiscard]] std::uint64_t dispersion_hash_text(std::string_view text);

// Pinned input-fingerprint over the corpus identity: "date_lo|date_hi|n_symbols".
[[nodiscard]] std::uint64_t dispersion_input_fingerprint(std::string_view date_lo,
                                                         std::string_view date_hi,
                                                         std::size_t n_symbols);

// Assemble the QualifiedCorpusConfig for a listed-dispersion corpus build from the
// pinned policy, the across-board worker count, and the input fingerprint. Byte-
// for-byte equivalent to the literals that used to live in build_corpus_command.
[[nodiscard]] QualifiedCorpusConfig dispersion_corpus_config(const DispersionCorpusPolicy &policy,
                                                             unsigned fit_workers,
                                                             std::uint64_t input_fingerprint);

// ── Surface-only backtest compute seam ──────────────────────────────────────

struct DispersionBacktestOutcome {
  BacktestResult track{}; // the rich SoA PnL track
  TearSheet sheet{};      // headline metrics derived from `track`
};

// Map the run spec into the canonical surface-only backtest config (the block that
// used to be inline in run_surface_backtest_command).
[[nodiscard]] DispersionBacktestConfig
dispersion_backtest_config_from_run_spec(const RunSpec &spec);

// Run the canonical surface-only dispersion backtest over an already-qualified
// Clock and bundle the track + tearsheet. Wraps the existing
// run_dispersion_backtest(); artifact persistence stays with the caller.
//
// FROZEN-UNIVERSE overload: the passed membership is held for the whole run. Kept
// for callers that genuinely own a single static basket; the flagship file-driven
// path uses the schedule overload below.
[[nodiscard]] Result<DispersionBacktestOutcome>
run_dispersion_surface_backtest(const Clock &clock, DispersionUniverse universe,
                                const DispersionBacktestConfig &config);

// C1-ACTIVATE point-in-time overload. Threads the raw constituent `schedule` down
// to WS-C's PIT resolver so the basket is re-resolved on every step and a
// mid-window reconstitution (add / reweight / REMOVE) is honored at the next roll.
// This is what `dispersion_run_surface_backtest` now calls: before activation the
// file-driven path resolved the universe once at the first session date and froze
// it, so a schedule with more than one `effective_date` block was silently ignored.
// With a single-block schedule this is byte-identical to the frozen overload.
[[nodiscard]] Result<DispersionBacktestOutcome>
run_dispersion_surface_backtest(const Clock &clock, std::vector<UniverseRow> schedule,
                                const DispersionBacktestConfig &config,
                                std::string_view index_symbol = "SPY");

// ── Native reference reconciliation (M1) ────────────────────────────────────
//
// Independent arithmetic verifier for a persisted listed-dispersion run, ported
// from tools/reference_spy_dispersion.py. Re-derives the vega-flat schedule
// quantities and the model/quote P&L from the persisted TSV artifacts and
// numerically compares them against the recorded values (no market data needed).
struct ReferenceReconRecord {
  std::string record_type{}; // "roll" | "date"
  std::string date{};
  std::int64_t cohort{0};
  // Roll rows carry vega residuals; date rows carry P&L/NAV. Unused fields are
  // left NaN and serialize as "NA".
  double computed_net_vega{0.0};
  double computed_gross_vega{0.0};
  double relative_vega_residual{0.0};
  double computed_model_option_pnl{0.0};
  double computed_quote_mid_pnl{0.0};
  double computed_model_nav{0.0};
  double computed_quote_mid_nav{0.0};
  double quote_mid_coverage{0.0};
  bool is_roll{true};
};

// Recompute + numerically verify the persisted artifacts. `schedule_only` limits
// the check to trade_schedule.tsv (matches the Python tool's flag).
[[nodiscard]] Result<std::vector<ReferenceReconRecord>>
reconcile_dispersion_reference(const std::filesystem::path &run_dir, bool schedule_only = false);

// Serialize reference records to the canonical reference_reconciliation.tsv layout.
[[nodiscard]] Status write_reference_reconciliation_file(
    const std::filesystem::path &path, std::span<const ReferenceReconRecord> records);

struct DispersionVerifyReport {
  std::size_t n_dates{0};
  std::uint32_t n_admitted{0};
  std::size_t n_rolls{0};
};

// ── File-oriented workflow entry points (the CLI dispatches to these) ───────

[[nodiscard]] Status dispersion_build_corpus(const std::filesystem::path &source_spec_path,
                                             const std::filesystem::path &run_dir,
                                             const DispersionCorpusPolicy &policy = {});
[[nodiscard]] Status dispersion_build_schedule(const std::filesystem::path &run_dir);
[[nodiscard]] Status dispersion_run_backtest(const std::filesystem::path &run_dir);
[[nodiscard]] Status dispersion_run_surface_backtest(const std::filesystem::path &run_dir);
[[nodiscard]] Status dispersion_run_projected_var(const std::filesystem::path &run_dir);
// Verifies the artifact envelope AND folds in the native reference reconciliation
// (M1): it recomputes + numerically compares the persisted arithmetic and writes
// run_dir/reference_reconciliation.tsv.
[[nodiscard]] Result<DispersionVerifyReport>
dispersion_verify(const std::filesystem::path &run_dir);

} // namespace atx::vol
