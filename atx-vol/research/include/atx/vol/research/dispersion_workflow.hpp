#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "atx/vol/api/backtest/dispersion.hpp"
#include "atx/vol/api/marketdata/opra_batch.hpp"
#include "atx/vol/api/core/types.hpp"

namespace atx::vol {

struct RunSpec {
  std::string label{"SPY listed-options dispersion proxy"};
  std::string date_lo{};
  std::string date_hi{};
  std::string snapshot_suffix{"T19:55:00Z"};
  std::filesystem::path opra_root{};
  std::string path_template{"{symbol}/{date}.parquet"};
  std::filesystem::path universe_path{};
  std::filesystem::path definitions_path{};
  std::filesystem::path occ_ess_root{};
  // Optional point-in-time dividend schedules consumed by OPRA fitting. The
  // corpus build replaces this source path with `dividend_ledger_path`, the
  // authoritative observation artifact used by replay.
  std::filesystem::path dividend_inputs_path{};
  std::filesystem::path dividend_ledger_path{};
  double flat_rate{0.0};
  std::size_t min_names{10};
  double min_weight_coverage{0.8};
  double target_dte_days{30.0};
  double min_dte_days{21.0};
  double max_dte_days{60.0};
  double roll_dte_days{7.0};
  double gross_index_vega{10000.0};
  double delta_band{0.0};
  unsigned fit_workers{0};
  bool core_mode{false};
  // L12: the always-kept index leg of the dispersion trade — the one symbol
  // that is fetched unconditionally and is never a basket constituent. The
  // default is "SPY" **so every existing spec, caller and golden is
  // bit-unchanged**: this field replaces three hardcoded "SPY" literals in
  // dispersion_workflow.cpp, and the default reproduces their output
  // element-for-element.
  //
  // This is a UNIVERSE/INPUT knob, which is why it belongs here. New
  // *methodology* knobs (floors, gates, acceptance thresholds) go to
  // `ListedDispersionMethodology` in listed_dispersion_pipeline.hpp instead,
  // per design section 4.6 — and that struct is deliberately reduced to the
  // three floors a consumer actually reads, so do not re-add dead fields there.
  std::string index_symbol{"SPY"};
};

struct UniverseRow {
  std::string effective_date{};
  std::string symbol{};
  double raw_weight{0.0};
  std::string source{};
  std::string as_of{};
};

[[nodiscard]] Result<RunSpec> read_run_spec(const std::filesystem::path &path);
[[nodiscard]] Status write_resolved_spec(const std::filesystem::path &path, const RunSpec &spec);
[[nodiscard]] Result<std::vector<UniverseRow>> read_universe(const std::filesystem::path &path);
// `index_symbol` trails as a plain `string_view` rather than a `const RunSpec &`
// so these pure front-end functions do not acquire a dependency on RunSpec's
// layout. It has NO DEFAULT on purpose: the "SPY" default it used to carry was
// silently taken by four sites in dispersion_run.cpp that had the configured
// symbol in scope, so a non-SPY run built its corpus, its schedule and its
// reconciliation against SPY. Requiring the argument makes the compiler, not a
// reviewer, the thing that finds the next such site. Pass
// `RunSpec::index_symbol` / `DispersionRunConfig::universe.index_symbol`.
[[nodiscard]] std::vector<std::string> all_symbols(std::span<const UniverseRow> rows,
                                                   std::string_view index_symbol);

// Point-in-time constituent snapshot effective on `date`. Each `effective_date`
// block is treated as a FULL vendor-style snapshot: membership is EXACTLY the
// rows carrying the latest effective_date on/before `date` — so a name present in
// an earlier block but absent from that latest block has LEFT the basket
// (removals/reweights are expressible; the basket is not append-only). The index
// leg is `index_symbol` (no default — see `all_symbols` above), which is never a
// constituent.
// @return Unavailable if no block is effective on/before `date`.
[[nodiscard]] Result<DispersionUniverse> universe_at(std::span<const UniverseRow> rows,
                                                     std::string_view date,
                                                     std::string_view index_symbol);

// UTC calendar date ("YYYY-MM-DD") of a nanosecond-since-epoch timestamp, via
// pure integer civil-from-days arithmetic (no locale / no platform time zone).
// This is the basis on which a snapshot's `ts_ns()` is matched against a
// schedule's `effective_date` for point-in-time re-resolution.
[[nodiscard]] std::string utc_date_from_ns(std::int64_t ts_ns);

// Build a point-in-time universe resolver over an owned `schedule`: given a
// valuation timestamp (UTC ns) it returns `universe_at(schedule,
// utc_date_from_ns(ts), index_symbol)`. This is the seam `DispersionStrategy`
// uses to re-resolve its basket for each backtest step so a mid-window
// reconstitution is honored (fix C1), instead of freezing day-1 membership.
[[nodiscard]] std::function<Result<DispersionUniverse>(std::int64_t)>
make_pit_universe_resolver(std::vector<UniverseRow> schedule, std::string index_symbol = "SPY");

[[nodiscard]] OpraBatchSpec batch_spec(const RunSpec &spec, std::span<const std::string> symbols,
                                       std::string_view date_lo, std::string_view date_hi);

} // namespace atx::vol
