#pragma once

// Task 9 — earnings-repro convention knob-carrier + cohort-validation harness.
//
// ## Two things live here (one header, so the Task-9 file set stays minimal):
//
//   1. `EarningsReproConfig` — the convention knob-carrier threaded INTO
//      `run_earnings_repro` (atx/vol/earnings_repro.hpp)'s config overload so
//      the validation harness below AND the Task-10 sweep can vary conventions
//      without editing the pipeline. Some knobs are WIRED (they change the
//      result); the rest are CARRY-ONLY (held for Task 10/M5 but with no wiring
//      seam in the Task 1..8 pipeline, so they DO NOT affect the result yet).
//   2. The cohort-validation harness — `CohortTruthRow` (one parsed row of the
//      checked-in SpiderRock truth CSV), `parse_cohort_truth_csv`,
//      `validate_cohort_name` (run the pipeline under a config, diff against
//      truth), `CohortResult` (per-tenor residual vector + per-name RMSE +
//      the nEarnCnt schedule-alignment gate), and `cohort_rmse_vol` (pooled).
//      Both the `earnings-validation` batch tool and the slow validation test
//      share this one implementation.
//
// The harness functions are `inline` (header-only) so the tool TU and the test
// TU each get their own copy with no ODR clash, and neither needs a new
// library source file. `atx/vol/earnings_repro.hpp` only FORWARD-declares
// `EarningsReproConfig`, so the 3-arg `run_earnings_repro` consumers (the Task
// 7 smoke test, the `earnings-repro` CLI) never pull this header's heavier
// include graph (`<fstream>`, session.hpp, ...).
//
// ## WIRED vs CARRY-ONLY knobs (see run_earnings_repro's config overload)
//
//   WIRED:
//     - `time`                (TimeSpec)     -> the 12 SR tenor year-fractions'
//                                               time convention (`tenor_years`).
//     - `clock_days_per_year` (double)       -> when > 0, the tenor
//                                               year-fraction is the fixed-clock
//                                               `N_trading_days /
//                                               clock_days_per_year`; when 0
//                                               (default) the calendar-aware
//                                               trading-day advance is used.
//     - `censor_space`        (bool)         -> maps to Task 8's
//                                               EventContext.censor_space: true
//                                               censors each bracket pillar
//                                               BEFORE interpolating (SR FLEX);
//                                               false interpolates a single
//                                               plain cross-pillar quantity,
//                                               then censors once.
//     - `interp`              (InterpSpace)  -> Variance: interpolate (censored)
//                                               TOTAL VARIANCE linearly in T;
//                                               Vol: interpolate (censored) VOL
//                                               linearly in T.
//   CARRY-ONLY (no Task 1..8 wiring seam; carried for Task 10/M5):
//     - `atm_mode`     (AtmMode)     -- the pipeline anchors forward-ATM (k=0)
//                                        unconditionally; no spot/delta anchor
//                                        path exists to route this to.
//     - `deam_pricer`  (DeAmPricer)  -- the session's de-Americanization pricer
//                                        is fixed upstream (ALO); no CRR path.
//     - `implied_borrow` (bool)      -- no borrow-implication seam in this
//                                        pipeline.
//
// ## Thread-safety
//
// `EarningsReproConfig` is a plain value type. `parse_cohort_truth_csv` reads
// the given file and touches no shared state. `validate_cohort_name` /
// `cohort_rmse_vol` are pure functions of their arguments (on top of
// `run_earnings_repro`, itself pure) -- safe to call concurrently.

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "atx/core/error.hpp"                    // Ok, Err
#include "analytics/earnings_repro.hpp"            // run_earnings_repro, EarningsReproResult
#include "atx/vol/api/analytics/earnings_term_fit.hpp"         // EmoveFitCode
#include "atx/vol/api/analytics/event_vol.hpp"                 // EventSchedule
#include "atx/vol/api/fitting/session.hpp"                   // VolaSession
#include "atx/vol/api/fitting/sr_tenor_grid.hpp"             // SrTenorGrid::kTradingDays
#include "atx/vol/api/core/types.hpp"                     // Result, ErrorCode
#include "atx/vol/api/core/vol_time.hpp"                  // TimeSpec

namespace atx::vol {

// Number of SR tenors reproduced end-to-end (`SrTenorGrid::kTradingDays`).
inline constexpr std::size_t kSrTenorCount = 12;

// Interpolation space for the cross-pillar term interpolation.
enum class InterpSpace : std::uint8_t {
  Variance = 0, // interpolate TOTAL VARIANCE (sigma^2 * T) linearly in T (default)
  Vol = 1,      // interpolate VOL linearly in T
};

// ATM anchor convention (CARRY-ONLY: the pipeline is forward-ATM/k=0 only).
enum class AtmMode : std::uint8_t {
  Forward = 0,     // forward-ATM, log-moneyness k = 0 (current behavior)
  Spot = 1,        // spot-ATM (no wiring seam)
  DeltaNeutral = 2 // delta-neutral straddle strike (no wiring seam)
};

// De-Americanization pricer (CARRY-ONLY: the session pins ALO upstream).
enum class DeAmPricer : std::uint8_t {
  Alo = 0, // Andersen-Lake-Offengenden (current path)
  Crr = 1, // Cox-Ross-Rubinstein binomial (no wiring seam)
};

// Convention knobs threaded into `run_earnings_repro` (config overload). Since
// the Task 10 sweep, a DEFAULT-constructed value carries the LOCKED reproduction
// convention (`time = VolTime`, censor-then-interp in variance space) -- the
// combination that best reproduces SpiderRock's censored-term columns. The
// historical Calendar365 pipeline behavior is still reproduced EXACTLY when the
// `time` field is overwritten with the session's own (`sess.inputs().time`) --
// which is precisely what the 3-arg `run_earnings_repro` overload does, so its
// callers (the Task 7 smoke test, the `earnings-repro` CLI's default) are
// unaffected by the new default.
struct EarningsReproConfig {
  // ── WIRED ────────────────────────────────────────────────────────────────
  // Task 10 convention sweep LOCKED VolTime as the default `time`: SpiderRock
  // builds its censored-term `atmCenI_{Nd}` columns in the hybrid
  // volatility-time clock, so the 12 SR tenor year-fractions must accrue in
  // VolTime (not Calendar365). Flipping this default cut the cohort atmCenI
  // pooled RMSE from 0.0301 -> 0.0121 and moved NVDA iEMove 0.045 -> 0.063 (vs
  // truth 0.0665, ~6% low) with the term-fit's `decay` no longer pinned at its
  // bound (fit_error 0.0437 -> 0.0074). See
  // docs/reviews/2026-07-18-atmcen-reproduction-convention-sweep.md.
  //
  // THOSE FOUR NUMBERS PREDATE THE 2026-08-23 CLOCK CORRECTION and have not
  // been re-measured. They were produced under the superseded VolTimeParams
  // (1890/6870, 7.5h session); the clock now ships the measured 1638/7122 with
  // a 6.5h session (see vol_time.hpp's derivation block). The DECISION they
  // support -- VolTime beats Calendar365 here -- is a comparison between two
  // conventions and is not at risk from a correction WITHIN the vol-time
  // clock, which only moved T by fractions of a percent. The magnitudes are
  // stale; re-run the cohort sweep before quoting them as current. NOTE: the
  // 3-arg `run_earnings_repro` overload still OVERRIDES this with
  // `sess.inputs().time`, so the historical calendar smoke path is bit-preserved;
  // only the config-driven (4-arg) default picks VolTime up.
  //
  // DATED CLIFF — ~2031-01, and it is fail-closed, not silent. The SR tenor
  // grid's longest horizon is 504 trading days (~2 years), and under
  // `VolTime` + `clock_days_per_year == 0` that horizon resolves through
  // `VolTimeCalendar::us_default()`, whose coverage window is exactly
  // 2024-01-01 .. 2032-12-31 (vol_time.hpp explains why it is not wider at
  // either end). A snapshot dated after roughly 2031-01 therefore pushes its
  // 504-day tenor past 2032-12-31 and `run_earnings_repro` returns `OutOfRange`
  // for the whole call. That is CORRECT behaviour — the alternative is crediting
  // full sessions for days no calendar covers — but it is a hard date, and every
  // shipped fixture (NVDA 2026-02-10) sits comfortably inside it, so nothing in
  // the suite will start failing before the pipeline does.
  //
  // The cliff was ~2027-01 while the window ended at 2028-12-31; the 2029-2032
  // rule projection moved it out by four years, it did not remove it.
  //
  // Two ways past it, in preference order: raise
  // `kUsDefaultProjectedThroughYear` (vol_time.cpp) — now a one-line change
  // needing no new data, though every added year is projection rather than
  // published fact, so read that file's note on what the projection cannot know
  // before leaning on a distant one; or set `clock_days_per_year > 0`, which
  // bypasses the calendar-aware path entirely at the cost of the convention
  // Task 10 locked.
  TimeSpec time{TimeConvention::VolTime};     // -> tenor_years convention (Task 10 sweep: VolTime)
  double clock_days_per_year{0.0};            // >0: T = N_td / this; 0: calendar-aware
  bool censor_space{true};                    // censor-then-interp (true) vs interp-then-censor
  InterpSpace interp{InterpSpace::Variance};  // variance- vs vol-space interpolation
  // ── CARRY-ONLY (no wiring seam; for Task 10/M5) ──────────────────────────
  AtmMode atm_mode{AtmMode::Forward};
  DeAmPricer deam_pricer{DeAmPricer::Alo};
  bool implied_borrow{false};

  [[nodiscard]] bool operator==(const EarningsReproConfig &) const = default;
};

// ── Cohort-validation harness ───────────────────────────────────────────────

// One parsed row of the checked-in SpiderRock truth CSV
// (tests/support/tickerhistory_2026-02-10_cohort.csv). Tenor-indexed arrays are
// index-for-index aligned with `SrTenorGrid::kTradingDays`
// (5,10,21,...,504 trading days). A tenor cell that is empty in the CSV parses
// to NaN (`atm_cen_i`) / 0 (`n_earn`) -- the cohort names are fully populated,
// but the parser tolerates gaps for a broader superset (Task 10).
struct CohortTruthRow {
  std::string ticker;
  double iemove{};                                 // iEMove (truth)
  std::array<double, kSrTenorCount> atm_cen_i{};    // atmCenI_{5d..504d}
  std::array<std::size_t, kSrTenorCount> n_earn{};  // nEarnCnt_{5d..504d}
  std::size_t n_earn_total{};                       // nEarnCnt
  double atm_cen_st{};                              // atmCenI_st
  double atm_cen_lt{};                              // atmCenI_lt
  double atm_cen_decay{};                           // atmCenI_decay
};

// Per-name validation result: the model's reproduced 12-tenor atmCenI, the
// per-tenor residual vector (model - truth), the model's own nEarnCnt_Nd from
// the schedule, the per-name RMSE (sqrt(sum r^2 / N) over the FINITE-truth
// tenors), the iEMove diff, and the schedule-alignment gate (`n_earn_match`).
struct CohortResult {
  std::string ticker;
  std::array<double, kSrTenorCount> model_atm_cen_i{};
  std::array<double, kSrTenorCount> residual{}; // model_atm_cen_i - truth.atm_cen_i
  std::array<std::size_t, kSrTenorCount> model_n_earn{};
  double model_emove{};
  double truth_emove{};
  double emove_residual{};              // model_emove - truth_emove
  double rmse_vol{};                    // sqrt( sum r^2 / N ), N = finite-truth tenors
  std::size_t n_tenors_scored{};        // count of finite-truth tenors in the RMSE
  bool n_earn_match{};                  // every model_n_earn[i] == truth.n_earn[i]
  EmoveFitCode fit_code{EmoveFitCode::Ok};
};

namespace detail {

// Splits `line` on ',' into fields (no quoted-field handling -- the truth CSV
// carries only bare numeric/ticker cells). A trailing '\r' (CRLF file) is
// stripped from the last field.
[[nodiscard]] inline std::vector<std::string_view> split_csv(std::string_view line) {
  std::vector<std::string_view> out;
  std::size_t start = 0;
  for (std::size_t i = 0; i <= line.size(); ++i) {
    if (i == line.size() || line[i] == ',') {
      out.push_back(line.substr(start, i - start));
      start = i + 1;
    }
  }
  if (!out.empty()) {
    std::string_view &last = out.back();
    if (!last.empty() && last.back() == '\r') {
      last.remove_suffix(1);
    }
  }
  return out;
}

// Trims surrounding ASCII whitespace/CR from `s`.
[[nodiscard]] inline std::string_view trim(std::string_view s) noexcept {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) {
    s.remove_prefix(1);
  }
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
    s.remove_suffix(1);
  }
  return s;
}

// Parses a decimal cell. Empty => NaN (a legitimately-missing tenor). A
// non-empty, unparseable cell => `false` (column misalignment / corrupt row).
[[nodiscard]] inline bool parse_double_cell(std::string_view s, double &out) noexcept {
  s = trim(s);
  if (s.empty()) {
    out = std::numeric_limits<double>::quiet_NaN();
    return true;
  }
  const char *b = s.data();
  const char *e = s.data() + s.size();
  const auto res = std::from_chars(b, e, out);
  return res.ec == std::errc{} && res.ptr == e;
}

// Parses a non-negative count cell. Empty => 0. Non-empty unparseable => false.
[[nodiscard]] inline bool parse_count_cell(std::string_view s, std::size_t &out) noexcept {
  s = trim(s);
  if (s.empty()) {
    out = 0;
    return true;
  }
  unsigned long long v{};
  const char *b = s.data();
  const char *e = s.data() + s.size();
  const auto res = std::from_chars(b, e, v);
  if (res.ec != std::errc{} || res.ptr != e) {
    return false;
  }
  out = static_cast<std::size_t>(v);
  return true;
}

} // namespace detail

// Parses the checked-in cohort truth CSV. Columns are resolved BY HEADER NAME
// (not fixed index), matching the loader convention elsewhere in atx-vol -- the
// 12 per-tenor columns are `atmCenI_{Nd}` / `nEarnCnt_{Nd}` for each
// `SrTenorGrid::kTradingDays` value, so the parsed arrays are guaranteed
// aligned to the grid the pipeline reproduces.
//
// @return  Ok(rows) one per data line. Err(IoError) if the file cannot be
//          opened. Err(InvalidArgument) if the header is missing a required
//          column, or any data row is short or carries an unparseable cell.
[[nodiscard]] inline Result<std::vector<CohortTruthRow>>
parse_cohort_truth_csv(std::string_view path) {
  std::ifstream in{std::string{path}};
  if (!in.is_open()) {
    return atx::core::Err(ErrorCode::IoError,
                          "parse_cohort_truth_csv: cannot open " + std::string{path});
  }

  std::string header_line;
  if (!std::getline(in, header_line)) {
    return atx::core::Err(ErrorCode::InvalidArgument,
                          "parse_cohort_truth_csv: empty file (no header)");
  }
  const std::vector<std::string_view> header = detail::split_csv(header_line);

  // Header name -> column index.
  const auto col = [&](std::string_view name) -> long {
    for (std::size_t i = 0; i < header.size(); ++i) {
      if (detail::trim(header[i]) == name) {
        return static_cast<long>(i);
      }
    }
    return -1;
  };

  const long c_ticker = col("ticker");
  const long c_iemove = col("iEMove");
  const long c_nec = col("nEarnCnt");
  const long c_st = col("atmCenI_st");
  const long c_lt = col("atmCenI_lt");
  const long c_decay = col("atmCenI_decay");
  std::array<long, kSrTenorCount> c_atm{};
  std::array<long, kSrTenorCount> c_nec_t{};
  for (std::size_t i = 0; i < kSrTenorCount; ++i) {
    const std::string suffix = std::to_string(SrTenorGrid::kTradingDays[i]) + "d";
    c_atm[i] = col("atmCenI_" + suffix);
    c_nec_t[i] = col("nEarnCnt_" + suffix);
  }

  auto missing = [&](long c) { return c < 0; };
  if (missing(c_ticker) || missing(c_iemove) || missing(c_nec) || missing(c_st) ||
      missing(c_lt) || missing(c_decay)) {
    return atx::core::Err(ErrorCode::InvalidArgument,
                          "parse_cohort_truth_csv: missing a required scalar column");
  }
  for (std::size_t i = 0; i < kSrTenorCount; ++i) {
    if (missing(c_atm[i]) || missing(c_nec_t[i])) {
      return atx::core::Err(ErrorCode::InvalidArgument,
                            "parse_cohort_truth_csv: missing a per-tenor column");
    }
  }

  // Largest column index any field references -- a data row shorter than this
  // is corrupt (loop-invariant, computed once).
  long max_col = std::max({c_ticker, c_iemove, c_nec, c_st, c_lt, c_decay});
  for (std::size_t i = 0; i < kSrTenorCount; ++i) {
    max_col = std::max(max_col, std::max(c_atm[i], c_nec_t[i]));
  }

  std::vector<CohortTruthRow> rows;
  std::string line;
  while (std::getline(in, line)) {
    if (detail::trim(line).empty()) {
      continue;
    }
    const std::vector<std::string_view> f = detail::split_csv(line);
    const auto at = [&](long c) -> std::string_view {
      return (c >= 0 && static_cast<std::size_t>(c) < f.size()) ? f[static_cast<std::size_t>(c)]
                                                                : std::string_view{};
    };
    if (static_cast<long>(f.size()) <= max_col) {
      return atx::core::Err(ErrorCode::InvalidArgument,
                            "parse_cohort_truth_csv: short data row");
    }

    CohortTruthRow row;
    row.ticker = std::string{detail::trim(at(c_ticker))};
    bool ok = detail::parse_double_cell(at(c_iemove), row.iemove);
    ok = detail::parse_count_cell(at(c_nec), row.n_earn_total) && ok;
    ok = detail::parse_double_cell(at(c_st), row.atm_cen_st) && ok;
    ok = detail::parse_double_cell(at(c_lt), row.atm_cen_lt) && ok;
    ok = detail::parse_double_cell(at(c_decay), row.atm_cen_decay) && ok;
    for (std::size_t i = 0; i < kSrTenorCount; ++i) {
      ok = detail::parse_double_cell(at(c_atm[i]), row.atm_cen_i[i]) && ok;
      ok = detail::parse_count_cell(at(c_nec_t[i]), row.n_earn[i]) && ok;
    }
    if (!ok) {
      return atx::core::Err(ErrorCode::InvalidArgument,
                            "parse_cohort_truth_csv: unparseable cell in row for " + row.ticker);
    }
    rows.push_back(std::move(row));
  }

  return atx::core::Ok(std::move(rows));
}

// Runs `run_earnings_repro` under `cfg` and diffs the reproduced 12-tenor
// atmCenI + iEMove + nEarnCnt_Nd against `truth`. RMSE is `sqrt(sum r^2 / N)`
// over the tenors with a FINITE truth value (mirrors fit_metrics.hpp's RMSE
// idiom). The schedule-alignment gate `n_earn_match` is true iff every model
// nEarnCnt_Nd equals the truth column exactly.
//
// @return  Ok(CohortResult) on a successful fit; propagates
//          `run_earnings_repro`'s error otherwise (e.g. a board with < 2
//          fitted expiries).
[[nodiscard]] inline Result<CohortResult>
validate_cohort_name(const VolaSession &sess, const EventSchedule &sched, std::int64_t now_ns,
                     const CohortTruthRow &truth, const EarningsReproConfig &cfg) {
  auto repro = run_earnings_repro(sess, sched, now_ns, cfg);
  if (!repro.has_value()) {
    return atx::core::Err(repro.error());
  }

  CohortResult out;
  out.ticker = truth.ticker;
  out.model_atm_cen_i = repro->atm_cen_i;
  out.model_n_earn = repro->n_earn;
  out.model_emove = repro->fit.emove;
  out.truth_emove = truth.iemove;
  out.emove_residual = repro->fit.emove - truth.iemove;
  out.fit_code = repro->fit.fit_code;

  double sse = 0.0;
  std::size_t scored = 0;
  bool match = true;
  for (std::size_t i = 0; i < kSrTenorCount; ++i) {
    const double r = repro->atm_cen_i[i] - truth.atm_cen_i[i];
    out.residual[i] = r;
    if (std::isfinite(r)) {
      sse += r * r;
      ++scored;
    }
    if (repro->n_earn[i] != truth.n_earn[i]) {
      match = false;
    }
  }
  out.n_tenors_scored = scored;
  out.rmse_vol = (scored > 0) ? std::sqrt(sse / static_cast<double>(scored))
                              : std::numeric_limits<double>::quiet_NaN();
  out.n_earn_match = match;
  return atx::core::Ok(std::move(out));
}

// Pooled cohort RMSE across per-name results: `sqrt( sum over all names and all
// finite-truth tenors of r^2 / total_count )`. NaN if no tenor was scored.
[[nodiscard]] inline double cohort_rmse_vol(std::span<const CohortResult> results) noexcept {
  double sse = 0.0;
  std::size_t n = 0;
  for (const CohortResult &c : results) {
    for (std::size_t i = 0; i < kSrTenorCount; ++i) {
      const double r = c.residual[i];
      if (std::isfinite(r)) {
        sse += r * r;
        ++n;
      }
    }
  }
  return (n > 0) ? std::sqrt(sse / static_cast<double>(n))
                 : std::numeric_limits<double>::quiet_NaN();
}

} // namespace atx::vol
