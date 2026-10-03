#include "atx/engine/research/fields/field_writer.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <system_error>
#include <utility>

#include "atx/engine/research/fields/field_stats.hpp"
#include "atx/engine/research/fields/file_io.hpp"

namespace atx::engine::research::fields {
namespace {

static_assert(std::endian::native == std::endian::little, "field payloads are little-endian");

// numpy's np.nan: the quiet NaN with a zero payload. Every non-finite cell is written with these
// bytes.
constexpr u64 kCanonicalNanBits = 0x7ff8000000000000ULL;

// The quantile probabilities of the Python manifest (QUANTILES), in its order.
constexpr std::array<f64, 5> kQuantiles{0.001, 0.01, 0.5, 0.99, 0.999};

[[nodiscard]] core::Error io_error(const std::filesystem::path &path, std::string_view what) {
  return core::Error(core::ErrorCode::IoError,
                     "research fields: " + std::string(what) + " " + path.string());
}

// Exclusive creation: an existing path is never truncated (C11 "x" mode, CREATE_NEW / O_EXCL).
[[nodiscard]] std::FILE *open_exclusive(const std::filesystem::path &path) noexcept {
  std::FILE *file = nullptr;
#if defined(_WIN32)
  if (_wfopen_s(&file, path.c_str(), L"wbx") != 0) {
    file = nullptr;
  }
#else
  file = std::fopen(path.c_str(), "wbx");
#endif
  return file;
}

} // namespace

void FieldWriter::FileCloser::operator()(std::FILE *file) const noexcept {
  if (file != nullptr) {
    // A writer destroyed before close(): the partial file stays unpublished; nothing reads its
    // close status.
    static_cast<void>(std::fclose(file));
  }
}

core::Result<FieldWriter> FieldWriter::create(const std::filesystem::path &output_dir,
                                              std::string_view name, const RoleAxes &role) {
  if (name.empty()) {
    return core::Err(core::ErrorCode::InvalidArgument, "research fields: empty field name");
  }
  FieldWriter out;
  out.role_ = &role;
  out.name_ = std::string(name);
  out.path_ = output_dir / (out.name_ + ".f64");
  out.file_.reset(open_exclusive(out.path_));
  if (!out.file_) {
    return core::Err(io_error(out.path_, "cannot create (exists or not writable)"));
  }
  out.canonical_.assign(role.instruments(), 0.0);
  out.row_values_.reserve(role.instruments());
  out.value_min_ = HUGE_VAL;
  out.value_max_ = -HUGE_VAL;
  return core::Ok(std::move(out));
}

core::Status FieldWriter::write(std::span<const f64> row) {
  if (!file_) {
    return core::Err(core::ErrorCode::InvalidArgument, "research fields: " + name_ + " is closed");
  }
  if (row.size() != role_->instruments()) {
    return core::Err(core::ErrorCode::InvalidArgument,
                     "research fields: " + name_ + ": row shape mismatch");
  }
  if (t_ >= role_->dates()) {
    return core::Err(core::ErrorCode::OutOfRange,
                     "research fields: " + name_ + ": row past the last date");
  }
  const f64 nan = std::bit_cast<f64>(kCanonicalNanBits);
  for (usize j = 0; j < row.size(); ++j) {
    canonical_[j] = std::isfinite(row[j]) ? row[j] : nan;
  }
  const auto bytes = std::as_bytes(std::span<const f64>(canonical_));
  if (std::fwrite(bytes.data(), 1U, bytes.size(), file_.get()) != bytes.size()) {
    return core::Err(io_error(path_, "write failed for"));
  }
  ATX_TRY_VOID(sha_.update(bytes));
  account(canonical_);
  ++t_;
  return core::Ok();
}

void FieldWriter::account(std::span<const f64> row) {
  const auto member = role_->member_row(t_);
  u64 member_cells = 0;
  row_values_.clear();
  for (usize j = 0; j < row.size(); ++j) {
    const bool finite = std::isfinite(row[j]);
    coverage_.finite_cells_all += finite ? 1U : 0U;
    if (member[j] == 0) {
      continue;
    }
    ++member_cells;
    if (finite) {
      row_values_.push_back(row[j]);
    }
  }
  const auto finite_cells = static_cast<u64>(row_values_.size());
  coverage_.member_cells += member_cells;
  coverage_.finite_member_cells += finite_cells;
  YearCoverage &year = years_[role_->years()[t_]];
  year.year = role_->years()[t_];
  year.member_cells += member_cells;
  year.finite_member_cells += finite_cells;
  const auto t = static_cast<i64>(t_);
  if (role_->score_begin() <= t && t < role_->score_end()) {
    coverage_.score_member_cells += member_cells;
    coverage_.score_finite_member_cells += finite_cells;
  }
  if (row_values_.empty()) {
    return;
  }
  // The Python takes the row's numpy min / max / sum, then folds them into its running Python
  // min() and max() (the earlier value survives a tie) and +=.
  const f64 row_min = numpy_reduce_min(row_values_);
  const f64 row_max = numpy_reduce_max(row_values_);
  value_min_ = row_min < value_min_ ? row_min : value_min_;
  value_max_ = row_max > value_max_ ? row_max : value_max_;
  value_sum_ += numpy_pairwise_sum(row_values_);
  member_values_.insert(member_values_.end(), row_values_.begin(), row_values_.end());
}

core::Result<WrittenField> FieldWriter::close() {
  if (!file_) {
    return core::Err(core::ErrorCode::InvalidArgument, "research fields: " + name_ + " is closed");
  }
  const bool flushed = std::fflush(file_.get()) == 0;
  const bool closed = std::fclose(file_.release()) == 0;
  if (!flushed || !closed) {
    return core::Err(io_error(path_, "flush/close failed for"));
  }
  if (t_ != role_->dates()) {
    return core::Err(core::ErrorCode::InvalidArgument,
                     "research fields: " + name_ + ": wrote " + std::to_string(t_) + " of " +
                         std::to_string(role_->dates()) + " dates");
  }
  const u64 expected =
      static_cast<u64>(role_->dates()) * static_cast<u64>(role_->instruments()) * sizeof(f64);
  std::error_code ec;
  const auto size = std::filesystem::file_size(path_, ec);
  if (ec || static_cast<u64>(size) != expected) {
    return core::Err(core::ErrorCode::IoError,
                     "research fields: " + name_ + ": output size mismatch");
  }
  ATX_TRY(const auto digest, sha_.finalize());
  WrittenField out;
  out.name = name_;
  out.path = path_;
  out.bytes = expected;
  out.sha256 = hex_digest(digest);
  out.coverage = coverage_;
  for (const auto &entry : years_) {
    out.coverage.per_year.push_back(entry.second);
  }
  const auto count = static_cast<u64>(member_values_.size());
  if (count != 0) {
    out.coverage.member_finite_min = value_min_;
    out.coverage.member_finite_max = value_max_;
    out.coverage.member_finite_mean = value_sum_ / static_cast<f64>(count);
    // np.quantile(values, QUANTILES, overwrite_input=True) on the values in write order: numpy's
    // own partition decides which of two equal values (-0.0, +0.0) each quantile reads.
    std::array<f64, kQuantiles.size()> q{};
    numpy_quantiles(member_values_, kQuantiles, q);
    out.coverage.member_finite_quantiles = MemberQuantiles{q[0], q[1], q[2], q[3], q[4]};
  }
  return core::Ok(std::move(out));
}

} // namespace atx::engine::research::fields
