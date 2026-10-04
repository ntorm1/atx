#pragma once

// atx::engine::research::eval -- the house's moments, PSR / MinTRL report and DSR rows
// (platform P9 B2, K-P9-5): the statistics of record that atx-impl/tools/backtest_integrity.py
// (moments, psr_report, lo_null_dsr), nav_summ.py (net_moments, sharpe, dsr_rows) and
// dsr_total.py (dsr_at) computed, on the engine's distribution code:
//
//   moments      sessions T; SR = mean / sd (ddof 1), undefined when sd is not positive;
//                skew = mean(e^3) / m2^1.5 and RAW kurtosis = mean(e^4) / m2^2 with e = x - mean,
//                m2 = mean(e^2) (population), undefined when m2 is not positive. numpy order:
//                pairwise sums (research/numpy_reduce.hpp), e^3 and e^4 by pow.
//   sharpe       annualized mean / sd (ddof 1) x sqrt(252); NaN when sd is not positive.
//   psr_report   per annualized benchmark SR*: PSR (eval::probabilistic_sharpe) and MinTRL in
//                sessions and years (eval::min_track_record_length at confidence 1 - alpha).
//   dsr rows     eval::house_dsr with the row's (N, V) (deflated_sharpe_house.hpp): cross (the
//                listed cells' ddof-1 SR variance, or Lo with one cell), lo, at (given V).
//
// Optional values are the Python rows' None.

#include <optional>
#include <span>
#include <string>
#include <vector>

#include "atx/core/types.hpp"

namespace atx::engine::research::eval {

inline constexpr f64 kAnnualization = 252.0;

struct HouseMoments {
  usize sessions{};
  std::optional<f64> sr_daily;
  std::optional<f64> skew;
  std::optional<f64> kurtosis; // raw (non-excess)

  [[nodiscard]] bool defined() const noexcept {
    return sr_daily.has_value() && skew.has_value() && kurtosis.has_value();
  }
};

[[nodiscard]] HouseMoments house_moments(std::span<const f64> x);
[[nodiscard]] f64 house_sharpe(std::span<const f64> x);

struct PsrBenchmark {
  f64 sr_star_annual{};
  f64 sr_star_daily{};
  std::optional<f64> psr;
  std::optional<f64> min_trl_sessions;
  std::optional<f64> min_trl_years;
};

struct PsrReport {
  HouseMoments moments;
  std::optional<f64> sr_annual;
  f64 alpha{};
  std::vector<PsrBenchmark> benchmarks;
};

[[nodiscard]] PsrReport psr_report(std::span<const f64> x, std::span<const f64> sr_stars_annual,
                                   f64 alpha);

// One DSR row: N, the variance it used, SR* and the DSR (each optional as in Python).
struct DsrRow {
  usize n{};
  std::optional<f64> dsr;
  std::optional<f64> sr0_daily;
  std::optional<f64> sr0_annual;
  std::optional<f64> variance_sr;
  std::string variance_source;
};

// dsr_total.dsr_at: undefined unless V, the moments, T >= 2 and N >= 2 are all there.
[[nodiscard]] DsrRow dsr_at(const HouseMoments &m, usize n, std::optional<f64> variance);
// nav_summ.dsr_rows (precondition n >= 2): V = ddof-1 variance of the defined SRs when two or
// more cells list one, else each cell's Lo variance.
[[nodiscard]] std::vector<DsrRow> dsr_rows(std::span<const HouseMoments> cells, usize n);
// backtest_integrity.lo_null_dsr.
[[nodiscard]] DsrRow lo_null_dsr(const HouseMoments &m, usize n);

} // namespace atx::engine::research::eval
