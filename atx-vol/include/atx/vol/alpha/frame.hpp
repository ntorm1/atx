#pragma once
// ── atx::vol::alpha — a panel of runtime width ──────────────────────────────
//
// WHAT THIS REPLACES. `tools/vrp_train.hpp` carries the panel as
// `std::array<double, kVrpFeatureCount> f{}` -- a COMPILE-TIME width that
// eighteen call sites index against, so the eleventh feature is a recompile of
// the trainer rather than a column in a file. `PanelFrame` is the same data
// with the width moved to runtime and the columns addressed by NAME.
//
// COLUMN-MAJOR, on purpose. Everything downstream reads a panel one COLUMN at
// a time -- per-feature standardization, per-column IC, per-column NaN census,
// winsorization -- and none of it reads a row. Column-major makes each of
// those a contiguous `std::span<const double>` instead of a strided gather.
//
// TYPING IS INFERRED, NOT DECLARED. A column is numeric when every non-empty
// value in it parses as a double, and text otherwise. That rule needs no
// schema annotation, so a panel that grows a column stays readable by a reader
// that has never heard of it -- which is the entire point of resolving by name.
// `nan` parses as numeric (the panel writes NaN for a warmup row and for an
// unavailable strip; see the `%.17g` / canonical-"nan" convention in
// `src/analytics/vrp_panel.hpp`).
//
// DATA QUALITY IS A FIRST-CLASS RESULT, not a script someone runs later.
// `ColumnStats` is computed on load, because the failure this repo actually
// hits is not a wrong number -- it is a column that is silently all-NaN, or
// constant, or whose finite fraction quietly fell to 12% when a tenor stopped
// being quoted. A fit that trains on such a column reports a clean run.

#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <istream>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/vol/alpha/schema.hpp"

namespace atx::vol::alpha {

// Per-column census. Everything here is a count or an extremum over the
// column's own values -- no cross-column arithmetic, so it is cheap enough to
// compute unconditionally on load.
struct ColumnStats {
  std::string name;
  bool numeric{false};
  std::size_t n_rows{0};
  std::size_t n_finite{0};
  std::size_t n_nan{0};
  std::size_t n_inf{0};
  std::size_t n_empty{0}; // text columns: empty cells
  double min{std::numeric_limits<double>::quiet_NaN()};
  double max{std::numeric_limits<double>::quiet_NaN()};
  double mean{std::numeric_limits<double>::quiet_NaN()};

  [[nodiscard]] double finite_fraction() const noexcept {
    return n_rows == 0 ? 0.0 : static_cast<double>(n_finite) / static_cast<double>(n_rows);
  }
  // A numeric column with no finite value is not a feature, it is a hole.
  [[nodiscard]] bool all_missing() const noexcept { return numeric && n_finite == 0; }
  // A column whose finite values never vary carries no cross-sectional
  // information at all; a model will happily consume it and learn nothing.
  [[nodiscard]] bool constant() const noexcept {
    return numeric && n_finite > 0 && min == max;
  }
};

// One column: numeric or text, never both.
class PanelColumn {
public:
  PanelColumn() = default;
  explicit PanelColumn(std::vector<double> values) : data_(std::move(values)) {}
  explicit PanelColumn(std::vector<std::string> values) : data_(std::move(values)) {}

  [[nodiscard]] bool numeric() const noexcept { return std::holds_alternative<std::vector<double>>(data_); }

  [[nodiscard]] std::span<const double> numbers() const noexcept {
    const auto *v = std::get_if<std::vector<double>>(&data_);
    return v != nullptr ? std::span<const double>(*v) : std::span<const double>{};
  }
  [[nodiscard]] std::span<const std::string> text() const noexcept {
    const auto *v = std::get_if<std::vector<std::string>>(&data_);
    return v != nullptr ? std::span<const std::string>(*v) : std::span<const std::string>{};
  }
  [[nodiscard]] std::size_t size() const noexcept {
    return numeric() ? numbers().size() : text().size();
  }

private:
  std::variant<std::vector<double>, std::vector<std::string>> data_;
};

class PanelFrame {
public:
  PanelFrame() = default;

  // Read a TSV whose FIRST non-comment line is the header. `#`-prefixed lines
  // before it are meta and are retained verbatim: the panel writes its run
  // counters there and a reader that discards them loses the provenance.
  //
  // A row with the wrong field count is an ERROR, never a pad or a truncate --
  // a short row means the writer and the reader disagree about the schema, and
  // silently filling it with NaN is how that disagreement survives to a fit.
  [[nodiscard]] static Result<PanelFrame> read_tsv(std::istream &in, char delim = '\t') {
    PanelFrame frame;
    std::string line;
    std::vector<std::vector<std::string>> cells; // column-major raw text
    bool have_header = false;
    std::size_t n_cols = 0;
    std::size_t line_no = 0;

    while (std::getline(in, line)) {
      ++line_no;
      while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
        line.pop_back();
      }
      if (line.empty()) {
        continue;
      }
      if (!have_header) {
        // The header may itself be commented; anything BEFORE it that starts
        // with '#' is meta. Distinguish by trying to parse: the first line that
        // yields more than one field and is not a `key=value` meta line wins.
        if (line[0] == '#' && line.find(delim) == std::string::npos) {
          frame.meta_.push_back(line);
          continue;
        }
        ATX_TRY(auto schema, PanelSchema::from_header(line, delim));
        frame.schema_ = std::move(schema);
        n_cols = frame.schema_.size();
        cells.resize(n_cols);
        have_header = true;
        continue;
      }
      if (line[0] == '#') {
        frame.meta_.push_back(line);
        continue;
      }
      std::size_t col = 0;
      std::size_t start = 0;
      for (std::size_t i = 0; i <= line.size(); ++i) {
        if (i != line.size() && line[i] != delim) {
          continue;
        }
        if (col >= n_cols) {
          return Err(atx::core::ErrorCode::ParseError,
                     "alpha::PanelFrame: line " + std::to_string(line_no) + " has more than " +
                         std::to_string(n_cols) + " fields");
        }
        cells[col].emplace_back(line.substr(start, i - start));
        ++col;
        start = i + 1;
      }
      if (col != n_cols) {
        return Err(atx::core::ErrorCode::ParseError,
                   "alpha::PanelFrame: line " + std::to_string(line_no) + " has " +
                       std::to_string(col) + " fields, header declares " + std::to_string(n_cols));
      }
      ++frame.n_rows_;
    }
    if (!have_header) {
      return Err(atx::core::ErrorCode::ParseError, "alpha::PanelFrame: no header line");
    }

    frame.columns_.reserve(n_cols);
    frame.stats_.reserve(n_cols);
    for (std::size_t c = 0; c < n_cols; ++c) {
      frame.ingest_column(std::string(frame.schema_.name_at(c)), std::move(cells[c]));
    }
    return Ok(std::move(frame));
  }

  [[nodiscard]] const PanelSchema &schema() const noexcept { return schema_; }
  [[nodiscard]] std::size_t rows() const noexcept { return n_rows_; }
  [[nodiscard]] std::size_t cols() const noexcept { return columns_.size(); }
  [[nodiscard]] const std::vector<std::string> &meta() const noexcept { return meta_; }
  [[nodiscard]] const std::vector<ColumnStats> &stats() const noexcept { return stats_; }

  [[nodiscard]] Result<std::span<const double>> numbers(std::string_view name) const {
    ATX_TRY(const std::size_t idx, schema_.index_of(name));
    if (!columns_[idx].numeric()) {
      return Err(atx::core::ErrorCode::InvalidArgument,
                 "alpha::PanelFrame: column '" + std::string(name) + "' is text, not numeric");
    }
    return Ok(columns_[idx].numbers());
  }

  [[nodiscard]] Result<std::span<const std::string>> strings(std::string_view name) const {
    ATX_TRY(const std::size_t idx, schema_.index_of(name));
    if (columns_[idx].numeric()) {
      return Err(atx::core::ErrorCode::InvalidArgument,
                 "alpha::PanelFrame: column '" + std::string(name) + "' is numeric, not text");
    }
    return Ok(columns_[idx].text());
  }

  [[nodiscard]] Result<const ColumnStats *> column_stats(std::string_view name) const {
    ATX_TRY(const std::size_t idx, schema_.index_of(name));
    return Ok(&stats_[idx]);
  }

  // Columns a fit would consume that carry no usable values. The list a caller
  // should refuse to train on rather than discover in a scorecard.
  [[nodiscard]] std::vector<std::string> unusable_columns() const {
    std::vector<std::string> out;
    for (const ColumnStats &s : stats_) {
      if (s.all_missing() || s.constant()) {
        out.push_back(s.name);
      }
    }
    return out;
  }

private:
  // Numeric iff every NON-EMPTY cell parses as a double. An empty cell is
  // missing, not text: a TSV writer that omits a value produces "", and
  // calling the whole column text because one row was blank would silently
  // demote a feature out of the fit.
  void ingest_column(std::string name, std::vector<std::string> raw) {
    std::vector<double> nums;
    nums.reserve(raw.size());
    bool numeric = true;
    std::size_t n_empty = 0;
    for (const std::string &cell : raw) {
      if (cell.empty()) {
        ++n_empty;
        nums.push_back(std::numeric_limits<double>::quiet_NaN());
        continue;
      }
      const char *first = cell.data();
      char *end = nullptr;
      const double v = std::strtod(first, &end);
      if (end == first || static_cast<std::size_t>(end - first) != cell.size()) {
        numeric = false;
        break;
      }
      nums.push_back(v);
    }

    ColumnStats st;
    st.name = name;
    st.numeric = numeric;
    st.n_rows = raw.size();
    st.n_empty = n_empty;
    if (!numeric) {
      columns_.emplace_back(std::move(raw));
      stats_.push_back(std::move(st));
      return;
    }

    double lo = std::numeric_limits<double>::infinity();
    double hi = -std::numeric_limits<double>::infinity();
    double sum = 0.0;
    for (const double v : nums) {
      if (std::isnan(v)) {
        ++st.n_nan;
      } else if (std::isinf(v)) {
        ++st.n_inf;
      } else {
        ++st.n_finite;
        sum += v;
        lo = v < lo ? v : lo;
        hi = v > hi ? v : hi;
      }
    }
    if (st.n_finite > 0) {
      st.min = lo;
      st.max = hi;
      st.mean = sum / static_cast<double>(st.n_finite);
    }
    columns_.emplace_back(std::move(nums));
    stats_.push_back(std::move(st));
  }

  PanelSchema schema_;
  std::vector<PanelColumn> columns_;
  std::vector<ColumnStats> stats_;
  std::vector<std::string> meta_;
  std::size_t n_rows_{0};
};

} // namespace atx::vol::alpha
