#pragma once

// Scorecard vocabulary + per-cell accounting for atx-vol-oracle-bench
// (bench/oracle/CHARTER.md stage 2).
//
// This TU owns the CHARTER CELL-KEY SCHEMA and the tolerance definitions —
// deliberately outside main() so every edge is unit-testable (gate:
// OracleBench* in tests/oracle_bench_test.cpp):
//
//   cell key   <mode>.<metric>.<moneyness-band>.<dte-band>.<cp>
//   m bands    deep-itm / itm / atm / otm / deep-otm, edges 0.8/0.95/1.05/1.2
//              on m = strike / uPrc, HALF-OPEN [lo, hi) — an exact edge value
//              belongs to the band ABOVE it. Band NAMES are cp-aware (itm/otm
//              is meaningless without the side): a call at m < 0.8 is deep ITM
//              while a put at the same m is deep OTM.
//   dte bands  0-7 / 8-30 / 31-90 / 90+ on calendar days-to-expiry; an exact
//              edge (7, 30, 90) belongs to the band BELOW it, matching the
//              integer-day labels (day 7 is in "0-7", day 8 in "8-30").
//   per cell   n, mae, rmse, p50, p95, p99, max, within_tol_rate
//
// Percentiles are NEAREST-RANK on the sorted |err| samples: rank = ceil(q*n),
// 1-based. Pinned here (and by test) so scorecards stay comparable across
// iterations.
//
// Thread-safety: Scorecard is a plain accumulator, single-threaded by design
// (the bench loop is serial). All free functions are pure.

#include <cstdint>
#include <functional> // std::less<> (transparent map lookups)
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "atx/vol/api/core/types.hpp" // Side, Result

namespace atx::vol::oracle {

// ── Bands ───────────────────────────────────────────────────────────────
enum class MoneynessBand : std::uint8_t { DeepItm = 0, Itm, Atm, Otm, DeepOtm };
enum class DteBand : std::uint8_t { D0To7 = 0, D8To30, D31To90, D90Plus };

// m = strike / uPrc. Half-open [lo, hi) intervals; the band NAME flips with the
// side (low strike = ITM call = OTM put). Non-finite m never reaches here — the
// cohort reader screens non-finite inputs at the boundary.
[[nodiscard]] MoneynessBand moneyness_band(double strike_over_uprc, Side side) noexcept;

// Calendar days to expiry; dte <= 7 -> 0-7 (negatives collapse into 0-7 too:
// the reader admits any finite `years`, and a stale row with years ~ 0- is
// still band-assignable rather than UB).
[[nodiscard]] DteBand dte_band(double dte_days) noexcept;

[[nodiscard]] std::string_view to_string(MoneynessBand band) noexcept; // "deep-itm" ...
[[nodiscard]] std::string_view to_string(DteBand band) noexcept;       // "0-7" ...
[[nodiscard]] std::string_view cp_token(Side side) noexcept;           // "c" / "p"

// "<mode>.<metric>.<mband>.<dteband>.<cp>", e.g. "a.price.atm.0-7.c".
[[nodiscard]] std::string cell_key(std::string_view mode, std::string_view metric,
                                   MoneynessBand mband, DteBand dband, Side side);

// ── Tolerances (stage-2 definitions; the scorecard header records them) ──
// price: |err| <= max(1 tick, 10% of the quoted spread). A crossed/zero spread
// degrades to the tick floor.
inline constexpr double kPriceTick = 0.01;
inline constexpr double kPriceSpreadFrac = 0.10;
// greeks: |err| <= max(abs floor, 1% of |oracle|). The absolute floor keeps a
// zero-valued oracle greek (deep-OTM delta ~ 0) from demanding exact equality.
inline constexpr double kGreekRelTol = 0.01;
inline constexpr double kGreekAbsFloor = 1.0e-4;

[[nodiscard]] double price_tolerance(double bid_prc, double ask_prc) noexcept;
[[nodiscard]] double greek_tolerance(double oracle_value) noexcept;

// Nearest-rank percentile over an ALREADY-SORTED ascending sample span;
// q in (0, 1]. Empty span -> 0.0.
[[nodiscard]] double percentile_nearest_rank(std::span<const double> sorted, double q) noexcept;

// ── Accounting ──────────────────────────────────────────────────────────
struct CellStats {
  std::int64_t n = 0;
  double mae = 0.0;
  double rmse = 0.0;
  double p50 = 0.0;
  double p95 = 0.0;
  double p99 = 0.0;
  double max_abs = 0.0; // serialized as "max" (charter field name)
  double within_tol_rate = 0.0;
};

// Per-mode row accounting + timing for the scorecard header.
struct ModeStats {
  std::int64_t rows_total = 0;         // rows the cohort selected, incl. skipped
  std::int64_t rows_priced = 0;        // rows that produced observations
  std::int64_t rows_null_sentinel = 0; // bidIV/askIV/error was null (-99 at ingest)
  std::int64_t rows_bad_input = 0;     // null/non-finite required input or bad cp
  std::int64_t rows_engine_error = 0;  // pricing entry point returned Err
  double wall_seconds = 0.0;
};

struct ScorecardHeader {
  std::int64_t iter = 0;
  std::string git_sha; // "unknown" when the caller did not pass one
  std::string cohort;
};

class Scorecard {
public:
  // Records one signed error observation into the cell addressed by the
  // coordinates; |err| feeds the stats, `within_tol` the rate. Non-finite
  // errors are the caller's to screen (the bench loop skips them).
  void observe(std::string_view mode, std::string_view metric, MoneynessBand mband, DteBand dband,
               Side side, double err, bool within_tol);

  void set_mode_stats(std::string_view mode, const ModeStats &stats);

  [[nodiscard]] std::size_t n_cells() const noexcept { return cells_.size(); }
  [[nodiscard]] std::vector<std::string> cell_keys() const;
  // NotFound if the key has no observations.
  [[nodiscard]] Result<CellStats> cell(std::string_view key) const;

  // The charter scorecard JSON: header (iter / git_sha / cohort / per-mode
  // timings / tolerance definitions) + one object per cell key. Deterministic:
  // cells emit in sorted key order.
  [[nodiscard]] std::string to_json(const ScorecardHeader &header) const;

private:
  struct Cell {
    std::vector<double> abs_errs;
    std::int64_t n_within = 0;
  };
  // std::less<> enables string_view lookups without a temporary std::string.
  std::map<std::string, Cell, std::less<>> cells_;
  std::map<std::string, ModeStats, std::less<>> mode_stats_;
};

} // namespace atx::vol::oracle
