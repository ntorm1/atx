#include "atx/engine/research/eval/house_moments.hpp"

#include <cmath>
#include <limits>

#include "atx/engine/eval/deflated_sharpe_house.hpp"
#include "atx/engine/eval/min_trl.hpp"
#include "atx/engine/research/numpy_reduce.hpp"

namespace atx::engine::research::eval {
namespace {

namespace ev = atx::engine::eval;

constexpr std::string_view kLoSource = "Lo (2002) sampling variance (1 + SR^2/2)/T: single cell";

[[nodiscard]] std::optional<f64> finite_or_none(f64 x) {
  return std::isnan(x) ? std::nullopt : std::optional<f64>(x);
}

// A defined row at (N, V): SR*, its annualized value and the DSR.
void fill_row(DsrRow &row, const HouseMoments &m, usize n, f64 variance) {
  const ev::HouseDsr d = ev::house_dsr(*m.sr_daily, m.sessions, *m.skew, *m.kurtosis, n, variance);
  row.sr0_daily = d.sr0_daily;
  row.sr0_annual = d.sr0_daily * std::sqrt(kAnnualization);
  row.dsr = d.dsr;
}

} // namespace

HouseMoments house_moments(std::span<const f64> x) {
  HouseMoments out;
  const usize t = x.size();
  out.sessions = t;
  if (t == 0U) {
    return out;
  }
  const f64 tf = static_cast<f64>(t);
  const f64 mean = numpy::mean(x);
  std::vector<f64> e2(t);
  std::vector<f64> e3(t);
  std::vector<f64> e4(t);
  for (usize i = 0; i < t; ++i) {
    const f64 e = x[i] - mean;
    e2[i] = e * e;
    e3[i] = std::pow(e, 3.0);
    e4[i] = std::pow(e, 4.0);
  }
  const f64 m2 = numpy::sum(e2) / tf;
  const f64 sd = t > 1U ? numpy::std_dev(x, 1U) : 0.0;
  if (sd > 0.0) {
    out.sr_daily = mean / sd;
  }
  if (m2 > 0.0) {
    out.skew = (numpy::sum(e3) / tf) / std::pow(m2, 1.5);
    out.kurtosis = (numpy::sum(e4) / tf) / std::pow(m2, 2.0);
  }
  return out;
}

f64 house_sharpe(std::span<const f64> x) {
  const f64 sd = x.size() > 1U ? numpy::std_dev(x, 1U) : 0.0;
  if (!(sd > 0.0)) {
    return std::numeric_limits<f64>::quiet_NaN();
  }
  return numpy::mean(x) / sd * std::sqrt(kAnnualization);
}

PsrReport psr_report(std::span<const f64> x, std::span<const f64> sr_stars_annual, f64 alpha) {
  PsrReport out;
  out.moments = house_moments(x);
  out.alpha = alpha;
  const HouseMoments &m = out.moments;
  if (m.sr_daily) {
    out.sr_annual = *m.sr_daily * std::sqrt(kAnnualization);
  }
  for (const f64 star : sr_stars_annual) {
    PsrBenchmark row;
    row.sr_star_annual = star;
    row.sr_star_daily = star / std::sqrt(kAnnualization);
    if (m.sr_daily && m.skew) {
      const f64 excess = *m.kurtosis - 3.0;
      row.psr = finite_or_none(
          ev::probabilistic_sharpe(*m.sr_daily, row.sr_star_daily, m.sessions, *m.skew, excess));
      row.min_trl_sessions = finite_or_none(ev::min_track_record_length(
          *m.sr_daily, row.sr_star_daily, *m.skew, excess, 1.0 - alpha));
      if (row.min_trl_sessions) {
        row.min_trl_years = *row.min_trl_sessions / kAnnualization;
      }
    }
    out.benchmarks.push_back(row);
  }
  return out;
}

DsrRow dsr_at(const HouseMoments &m, usize n, std::optional<f64> variance) {
  DsrRow row;
  row.n = n;
  if (!variance || !m.defined() || m.sessions < 2U || n < 2U) {
    return row;
  }
  fill_row(row, m, n, *variance);
  return row;
}

std::vector<DsrRow> dsr_rows(std::span<const HouseMoments> cells, usize n) {
  std::vector<f64> srs;
  for (const HouseMoments &m : cells) {
    if (m.sr_daily) {
      srs.push_back(*m.sr_daily);
    }
  }
  const bool cross = cells.size() >= 2U && srs.size() >= 2U;
  const f64 v_cross = cross ? numpy::var(srs, 1U) : 0.0;
  std::vector<DsrRow> out;
  out.reserve(cells.size());
  for (const HouseMoments &m : cells) {
    DsrRow row;
    row.n = n;
    if (!m.sr_daily || !m.skew || m.sessions < 2U) {
      row.variance_source = "undefined (constant or too short net series)";
      out.push_back(row);
      continue;
    }
    if (cross) {
      row.variance_sr = v_cross;
      row.variance_source =
          "cross-cell sample variance of " + std::to_string(srs.size()) + " per-session SRs";
    } else {
      row.variance_sr = ev::lo_sampling_variance(*m.sr_daily, m.sessions);
      row.variance_source = std::string(kLoSource);
    }
    fill_row(row, m, n, *row.variance_sr);
    out.push_back(row);
  }
  return out;
}

DsrRow lo_null_dsr(const HouseMoments &m, usize n) {
  DsrRow row;
  row.n = n;
  row.variance_source = std::string(kLoSource);
  if (!m.defined() || m.sessions < 2U || n < 2U) {
    return row;
  }
  row.variance_sr = ev::lo_sampling_variance(*m.sr_daily, m.sessions);
  fill_row(row, m, n, *row.variance_sr);
  return row;
}

} // namespace atx::engine::research::eval
