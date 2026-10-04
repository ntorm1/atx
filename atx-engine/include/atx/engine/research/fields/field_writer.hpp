#pragma once

// atx::engine::research::fields -- one field's payload writer (prepare_research_fields.py
// FieldWriter and digest_and_quantiles). A field is one date-major little-endian f64 file
// `<name>.f64` of shape role dates x role instruments in a new output directory; a non-finite cell
// is written as the canonical quiet NaN 0x7ff8000000000000 (numpy's np.nan bytes). The writer
// streams rows in session order and accumulates, over the role's member cells, exactly the coverage
// block the Python manifest records: member and finite member cells in total, per calendar year and
// inside the score window [score_begin, score_end); every finite cell; the finite member minimum,
// maximum and mean; and the p0.1 / p1 / p50 / p99 / p99.9 quantiles (field_stats.hpp).
//
// The file is created exclusively (an existing file is never truncated); a writer destroyed before
// close() leaves its partial file unpublished (no manifest names it). Not thread-safe; one writer
// per field.

#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/sha256.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/research/fields/role_axes.hpp"

namespace atx::engine::research::fields {

struct YearCoverage {
  i64 year{};
  u64 member_cells{};
  u64 finite_member_cells{};
};

struct MemberQuantiles {
  f64 p0_1{};
  f64 p1{};
  f64 p50{};
  f64 p99{};
  f64 p99_9{};
};

struct Coverage {
  u64 member_cells{};
  u64 finite_member_cells{};
  u64 score_member_cells{};
  u64 score_finite_member_cells{};
  std::vector<YearCoverage> per_year; // ascending year
  u64 finite_cells_all{};
  std::optional<f64> member_finite_min;
  std::optional<f64> member_finite_max;
  std::optional<f64> member_finite_mean;
  std::optional<MemberQuantiles> member_finite_quantiles;
};

struct WrittenField {
  std::string name;
  std::filesystem::path path;
  u64 bytes{};
  std::string sha256;
  Coverage coverage;
};

class FieldWriter {
public:
  // Creates `output_dir`/`name`.f64 exclusively. `role` must outlive the writer.
  [[nodiscard]] static core::Result<FieldWriter>
  create(const std::filesystem::path &output_dir, std::string_view name, const RoleAxes &role);

  // Row t (role instruments() values). Err after close, on a size mismatch, past the last date or
  // on an I/O error.
  [[nodiscard]] core::Status write(std::span<const f64> row);

  // Flushes and closes; Err unless exactly role dates() rows were written and the file has the role
  // shape's size.
  [[nodiscard]] core::Result<WrittenField> close();

  [[nodiscard]] usize rows_written() const noexcept { return t_; }

private:
  struct FileCloser {
    void operator()(std::FILE *file) const noexcept;
  };

  FieldWriter() = default;
  void account(std::span<const f64> canonical_row);

  std::unique_ptr<std::FILE, FileCloser> file_;
  const RoleAxes *role_{}; // non-owning, non-null after create()
  std::string name_;
  std::filesystem::path path_;
  core::Sha256 sha_;
  usize t_{};
  std::vector<f64> canonical_;
  std::vector<f64> row_values_;    // the current row's finite member values, in column order
  std::vector<f64> member_values_; // every finite member value, in write order (quantiles)
  std::map<i64, YearCoverage> years_;
  Coverage coverage_;
  f64 value_sum_{};
  f64 value_min_{};
  f64 value_max_{};
};

} // namespace atx::engine::research::fields
