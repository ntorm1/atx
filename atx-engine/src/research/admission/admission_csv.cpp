#include "atx/engine/research/admission/admission_csv.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace atx::engine::research::admission {
namespace {

[[nodiscard]] core::Error invalid(const std::string &message) {
  return core::Error(core::ErrorCode::InvalidArgument, "admission CSV: " + message);
}

[[nodiscard]] std::string optional_float(const std::optional<f64> &value) {
  return value ? python_float_repr(*value) : std::string{};
}

[[nodiscard]] std::string optional_integer(const std::optional<usize> &value) {
  return value ? std::to_string(*value) : std::string{};
}

// The ids of `refs` joined with ';' (Python's ";".join over a list).
[[nodiscard]] std::string joined_ids(std::span<const CandidateMeta> meta,
                                     std::span<const usize> refs) {
  std::string out;
  for (usize i = 0; i < refs.size(); ++i) {
    if (i > 0U) {
      out += ';';
    }
    out += meta[refs[i]].id;
  }
  return out;
}

[[nodiscard]] std::string failed_checks(std::span<const Check> checks) {
  std::string out;
  for (usize i = 0; i < checks.size(); ++i) {
    if (i > 0U) {
      out += ';';
    }
    out += check_name(checks[i]);
  }
  return out;
}

[[nodiscard]] bool references_in_roster(const ScreenRow &row, usize roster) {
  const auto inside = [&](usize k) { return k < roster; };
  return (!row.redundant_with || inside(*row.redundant_with)) &&
         (!row.max_abs_rho_with || inside(*row.max_abs_rho_with)) &&
         std::all_of(row.low_overlap_with.begin(), row.low_overlap_with.end(), inside) &&
         std::all_of(row.undefined_rho_with.begin(), row.undefined_rho_with.end(), inside);
}

// The cells of one candidate in kV4CsvColumns order (fit:2250-2259 and admission_csv's cell()).
[[nodiscard]] std::vector<std::string> v4_cells(std::span<const CandidateMeta> meta, usize k,
                                                const ScreenRow &row) {
  const CandidateMeta &m = meta[k];
  const auto id_of = [&](const std::optional<usize> &ref) {
    return ref ? meta[*ref].id : std::string{};
  };
  return {m.id,
          m.family,
          m.theme,
          m.tier.text,
          std::to_string(m.prior_sign),
          std::string(status_name(row.status)),
          failed_checks(row.failed_checks),
          id_of(row.redundant_with),
          optional_float(row.redundant_rho),
          optional_integer(row.admission_rank),
          std::to_string(row.s_k),
          std::to_string(m.runner_sign),
          m.runner_sign == row.s_k ? "true" : "false",
          python_float_repr(row.tau),
          std::to_string(row.train_days),
          optional_float(row.train_mean),
          optional_float(row.train_sharpe),
          optional_float(row.hac_t),
          optional_float(row.max_abs_rho),
          id_of(row.max_abs_rho_with),
          joined_ids(meta, row.low_overlap_with),
          joined_ids(meta, row.undefined_rho_with),
          m.cache_entry,
          m.cache_payload_sha256};
}

// Appends one CSV line; Err when a cell is unsafe.
[[nodiscard]] core::Status append_line(std::string &out, std::span<const std::string> cells) {
  for (usize i = 0; i < cells.size(); ++i) {
    if (cells[i].find_first_of(",\n") != std::string::npos) {
      return core::Err(invalid("unsafe cell " + cells[i]));
    }
    if (i > 0U) {
      out += ',';
    }
    out += cells[i];
  }
  out += '\n';
  return core::Ok();
}

[[nodiscard]] std::string header(std::span<const std::string_view> columns) {
  std::string out;
  for (usize i = 0; i < columns.size(); ++i) {
    if (i > 0U) {
      out += ',';
    }
    out += columns[i];
  }
  out += '\n';
  return out;
}

} // namespace

core::Result<Tier> tier_from_grade(std::string_view grade) {
  const auto it = std::find(kTierGrades.begin(), kTierGrades.end(), grade);
  if (it == kTierGrades.end()) {
    return core::Err(invalid("tier must be one of A+ .. D or an integer >= 0: " +
                             std::string(grade)));
  }
  return core::Ok(Tier{std::string(grade), static_cast<usize>(it - kTierGrades.begin())});
}

Tier tier_from_integer(u64 value) {
  return Tier{std::to_string(value), static_cast<usize>(value)};
}

std::string python_float_repr(f64 x) {
  if (std::isnan(x)) {
    return "nan";
  }
  if (std::isinf(x)) {
    return x > 0.0 ? "inf" : "-inf";
  }
  // Shortest round-trip digits in scientific form: [-]d[.ddd]e(+|-)XX.
  std::array<char, 64> buffer{};
  const auto written =
      std::to_chars(buffer.data(), buffer.data() + buffer.size(), x, std::chars_format::scientific);
  std::string_view text(buffer.data(), static_cast<usize>(written.ptr - buffer.data()));
  std::string out;
  if (!text.empty() && text.front() == '-') {
    out += '-';
    text.remove_prefix(1);
  }
  const usize e = text.find('e');
  int magnitude = 0;
  if (written.ec != std::errc{} || e == std::string_view::npos || e + 2U >= text.size() ||
      std::from_chars(text.data() + e + 2U, text.data() + text.size(), magnitude).ec !=
          std::errc{}) {
    return std::string(buffer.data(), static_cast<usize>(written.ptr - buffer.data()));
  }
  std::string digits;
  for (const char c : text.substr(0, e)) {
    if (c != '.') {
      digits += c;
    }
  }
  const bool negative_exponent = text[e + 1U] == '-';
  const int exp10 = negative_exponent ? -magnitude : magnitude;
  // Python's decimal point position: value = 0.<digits> x 10^decpt.
  const int decpt = exp10 + 1;
  const int count = static_cast<int>(digits.size());
  if (decpt <= -4 || decpt > 16) { // repr's exponent form (format code 'r')
    out += digits.front();
    if (count > 1) {
      out += '.';
      out.append(digits, 1, std::string::npos);
    }
    out += negative_exponent ? "e-" : "e+";
    if (magnitude < 10) {
      out += '0';
    }
    out += std::to_string(magnitude);
  } else if (decpt <= 0) {
    out += "0.";
    out.append(static_cast<usize>(-decpt), '0');
    out += digits;
  } else if (decpt >= count) {
    out += digits;
    out.append(static_cast<usize>(decpt - count), '0');
    out += ".0";
  } else {
    out.append(digits, 0, static_cast<usize>(decpt));
    out += '.';
    out.append(digits, static_cast<usize>(decpt), std::string::npos);
  }
  return out;
}

core::Result<std::string> admission_csv(std::span<const CandidateMeta> meta,
                                        std::span<const ScreenRow> rows) {
  if (meta.size() != rows.size() || meta.empty()) {
    return core::Err(invalid("candidate metadata and screen rows differ in length"));
  }
  std::string out = header(kV4CsvColumns);
  for (usize k = 0; k < rows.size(); ++k) {
    if (!references_in_roster(rows[k], meta.size())) {
      return core::Err(invalid("a row names a candidate outside the roster"));
    }
    const std::vector<std::string> cells = v4_cells(meta, k, rows[k]);
    ATX_TRY_VOID(append_line(out, cells));
  }
  return core::Ok(std::move(out));
}

core::Result<std::string> traded_horizon_csv(std::span<const CandidateMeta> meta,
                                             std::span<const ScreenRow> rows,
                                             std::span<const TradedHorizonRow> horizon) {
  if (meta.size() != rows.size() || meta.size() != horizon.size() || meta.empty()) {
    return core::Err(invalid("traded horizon: inputs differ in length"));
  }
  constexpr std::array<std::string_view, 6> columns{"id",      "status", "s_k",
                                                    "h21_days", "ic_h21", "ic_h21_hac_t"};
  std::string out = header(columns);
  for (usize k = 0; k < rows.size(); ++k) {
    const std::vector<std::string> cells{meta[k].id,
                                         std::string(status_name(rows[k].status)),
                                         std::to_string(rows[k].s_k),
                                         std::to_string(horizon[k].days),
                                         optional_float(horizon[k].ic),
                                         optional_float(horizon[k].hac_t)};
    ATX_TRY_VOID(append_line(out, cells));
  }
  return core::Ok(std::move(out));
}

} // namespace atx::engine::research::admission
