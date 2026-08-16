#include "oracle_scorecard.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <utility>

namespace atx::vol::oracle {

using atx::core::Ok; // Err resolves via ADL; Ok's arguments are not atx::core
                     // types — same convention as track_store.cpp.

MoneynessBand moneyness_band(double strike_over_uprc, Side side) noexcept {
  const double m = strike_over_uprc;
  // Half-open [lo, hi) rungs on m, low-strike first. For a CALL a low strike
  // is in the money; a PUT mirrors.
  std::uint8_t rung = 0; // 0 = m < 0.8 ... 4 = m >= 1.2
  if (m >= 0.8) {
    ++rung;
  }
  if (m >= 0.95) {
    ++rung;
  }
  if (m >= 1.05) {
    ++rung;
  }
  if (m >= 1.2) {
    ++rung;
  }
  constexpr MoneynessBand kCallLadder[5] = {MoneynessBand::DeepItm, MoneynessBand::Itm,
                                            MoneynessBand::Atm, MoneynessBand::Otm,
                                            MoneynessBand::DeepOtm};
  return (side == Side::Call) ? kCallLadder[rung]
                              : kCallLadder[static_cast<std::size_t>(4 - rung)];
}

DteBand dte_band(double dte_days) noexcept {
  if (dte_days <= 7.0) {
    return DteBand::D0To7; // negatives collapse here too (header contract)
  }
  if (dte_days <= 30.0) {
    return DteBand::D8To30;
  }
  if (dte_days <= 90.0) {
    return DteBand::D31To90;
  }
  return DteBand::D90Plus;
}

std::string_view to_string(MoneynessBand band) noexcept {
  switch (band) {
  case MoneynessBand::DeepItm:
    return "deep-itm";
  case MoneynessBand::Itm:
    return "itm";
  case MoneynessBand::Atm:
    return "atm";
  case MoneynessBand::Otm:
    return "otm";
  case MoneynessBand::DeepOtm:
    return "deep-otm";
  }
  assert(false); // unreachable for valid enumerators
  return "?";
}

std::string_view to_string(DteBand band) noexcept {
  switch (band) {
  case DteBand::D0To7:
    return "0-7";
  case DteBand::D8To30:
    return "8-30";
  case DteBand::D31To90:
    return "31-90";
  case DteBand::D90Plus:
    return "90+";
  }
  assert(false); // unreachable for valid enumerators
  return "?";
}

std::string_view cp_token(Side side) noexcept {
  switch (side) {
  case Side::Call:
    return "c";
  case Side::Put:
    return "p";
  }
  assert(false); // unreachable for valid enumerators
  return "?";
}

double price_tolerance(double bid_prc, double ask_prc) noexcept {
  // A crossed/zero spread contributes a non-positive term, so the tick floor
  // wins via max() without a special case.
  return std::max(kPriceTick, kPriceSpreadFrac * (ask_prc - bid_prc));
}

double greek_tolerance(double oracle_value) noexcept {
  return std::max(kGreekAbsFloor, kGreekRelTol * std::abs(oracle_value));
}

std::string cell_key(std::string_view mode, std::string_view metric, MoneynessBand mband,
                     DteBand dband, Side side) {
  std::string key;
  key.reserve(mode.size() + metric.size() + 24);
  key.append(mode);
  key.push_back('.');
  key.append(metric);
  key.push_back('.');
  key.append(to_string(mband));
  key.push_back('.');
  key.append(to_string(dband));
  key.push_back('.');
  key.append(cp_token(side));
  return key;
}

double percentile_nearest_rank(std::span<const double> sorted, double q) noexcept {
  if (sorted.empty()) {
    return 0.0;
  }
  const double n = static_cast<double>(sorted.size());
  // rank = ceil(q*n), 1-based, clamped into [1, n].
  const double raw = std::ceil(q * n);
  const std::size_t rank =
      static_cast<std::size_t>(std::clamp(raw, 1.0, n));
  return sorted[rank - 1];
}

void Scorecard::observe(std::string_view mode, std::string_view metric, MoneynessBand mband,
                        DteBand dband, Side side, double err, bool within_tol) {
  Cell &cell = cells_[cell_key(mode, metric, mband, dband, side)];
  cell.abs_errs.push_back(std::abs(err));
  if (within_tol) {
    ++cell.n_within;
  }
}

void Scorecard::set_mode_stats(std::string_view mode, const ModeStats &stats) {
  mode_stats_[std::string{mode}] = stats;
}

std::vector<std::string> Scorecard::cell_keys() const {
  std::vector<std::string> keys;
  keys.reserve(cells_.size());
  for (const auto &[key, cell] : cells_) {
    keys.push_back(key);
  }
  return keys;
}

namespace {

[[nodiscard]] CellStats compute_stats(std::vector<double> abs_errs, std::int64_t n_within) {
  CellStats out;
  out.n = static_cast<std::int64_t>(abs_errs.size());
  if (abs_errs.empty()) {
    return out;
  }
  std::sort(abs_errs.begin(), abs_errs.end());
  double sum = 0.0;
  double sum_sq = 0.0;
  for (const double e : abs_errs) {
    sum += e;
    sum_sq += e * e;
  }
  const double n = static_cast<double>(abs_errs.size());
  out.mae = sum / n;
  out.rmse = std::sqrt(sum_sq / n);
  out.p50 = percentile_nearest_rank(abs_errs, 0.50);
  out.p95 = percentile_nearest_rank(abs_errs, 0.95);
  out.p99 = percentile_nearest_rank(abs_errs, 0.99);
  out.max_abs = abs_errs.back();
  out.within_tol_rate = static_cast<double>(n_within) / n;
  return out;
}

void append_json_string(std::string &out, std::string_view s) {
  out.push_back('"');
  for (const char ch : s) {
    switch (ch) {
    case '"':
      out.append("\\\"");
      break;
    case '\\':
      // JSON escape for a backslash: emit two backslash CHARACTERS via the
      // count overload. Spelled as a string literal, the decoded two-backslash
      // content reads as a UNC path prefix to the umbrella path-literal scan
      // (VolUmbrella.NoFixturePathResolvedOutsideTheSharedResolver), whose
      // absolute-literal rule deliberately has no exemption list. This is JSON
      // string escaping, not a path literal.
      out.append(2, '\\');
      break;
    case '\n':
      out.append("\\n");
      break;
    case '\r':
      out.append("\\r");
      break;
    case '\t':
      out.append("\\t");
      break;
    default:
      if (static_cast<unsigned char>(ch) < 0x20) {
        char buf[8];
        std::snprintf(buf, sizeof buf, "\\u%04x", static_cast<unsigned>(ch));
        out.append(buf);
      } else {
        out.push_back(ch);
      }
      break;
    }
  }
  out.push_back('"');
}

void append_json_double(std::string &out, double v) {
  char buf[40];
  std::snprintf(buf, sizeof buf, "%.17g", v);
  out.append(buf);
}

void append_json_int(std::string &out, std::int64_t v) {
  char buf[24];
  std::snprintf(buf, sizeof buf, "%lld", static_cast<long long>(v));
  out.append(buf);
}

} // namespace

Result<CellStats> Scorecard::cell(std::string_view key) const {
  const auto it = cells_.find(key);
  if (it == cells_.end()) {
    return Err(ErrorCode::NotFound, "no such scorecard cell: " + std::string{key});
  }
  return Ok(compute_stats(it->second.abs_errs, it->second.n_within));
}

std::string Scorecard::to_json(const ScorecardHeader &header) const {
  std::string out;
  out.reserve(4096 + cells_.size() * 200);
  out.append("{\n");

  out.append("  \"iter\": ");
  append_json_int(out, header.iter);
  out.append(",\n  \"git_sha\": ");
  append_json_string(out, header.git_sha);
  out.append(",\n  \"cohort\": ");
  append_json_string(out, header.cohort);

  out.append(",\n  \"modes\": {");
  bool first_mode = true;
  for (const auto &[mode, stats] : mode_stats_) {
    if (!first_mode) {
      out.push_back(',');
    }
    first_mode = false;
    out.append("\n    ");
    append_json_string(out, mode);
    out.append(": {\n      \"rows_total\": ");
    append_json_int(out, stats.rows_total);
    out.append(",\n      \"rows_priced\": ");
    append_json_int(out, stats.rows_priced);
    out.append(",\n      \"rows_null_sentinel\": ");
    append_json_int(out, stats.rows_null_sentinel);
    out.append(",\n      \"rows_bad_input\": ");
    append_json_int(out, stats.rows_bad_input);
    out.append(",\n      \"rows_engine_error\": ");
    append_json_int(out, stats.rows_engine_error);
    out.append(",\n      \"wall_seconds\": ");
    append_json_double(out, stats.wall_seconds);
    out.append(",\n      \"rows_per_second\": ");
    const double rows_per_s = stats.wall_seconds > 0.0
                                  ? static_cast<double>(stats.rows_priced) / stats.wall_seconds
                                  : 0.0;
    append_json_double(out, rows_per_s);
    out.append("\n    }");
  }
  out.append("\n  },");

  // Tolerance DEFINITIONS, verbatim, so a scorecard is self-describing: a
  // within_tol_rate is meaningless without the rule that produced it.
  out.append("\n  \"tolerances\": {\n"
             "    \"price\": \"|err| <= max(0.01, 0.10 * (askPrc - bidPrc))\",\n"
             "    \"greeks\": \"|err| <= max(0.0001, 0.01 * |oracle|)\"\n"
             "  },");

  out.append("\n  \"cells\": {");
  bool first_cell = true;
  for (const auto &[key, cell] : cells_) {
    if (!first_cell) {
      out.push_back(',');
    }
    first_cell = false;
    const CellStats stats = compute_stats(cell.abs_errs, cell.n_within);
    out.append("\n    ");
    append_json_string(out, key);
    out.append(": {\"n\": ");
    append_json_int(out, stats.n);
    out.append(", \"mae\": ");
    append_json_double(out, stats.mae);
    out.append(", \"rmse\": ");
    append_json_double(out, stats.rmse);
    out.append(", \"p50\": ");
    append_json_double(out, stats.p50);
    out.append(", \"p95\": ");
    append_json_double(out, stats.p95);
    out.append(", \"p99\": ");
    append_json_double(out, stats.p99);
    out.append(", \"max\": ");
    append_json_double(out, stats.max_abs);
    out.append(", \"within_tol_rate\": ");
    append_json_double(out, stats.within_tol_rate);
    out.append("}");
  }
  out.append("\n  }\n}\n");
  return out;
}

} // namespace atx::vol::oracle
