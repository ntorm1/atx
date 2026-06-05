#pragma once

// atx::tsdb::SegmentBuilder — assemble a dense panel grid in RAM and write it as
// a sealed, position-independent segment file. Single-threaded; build-once.
//
// Cells default to NaN with present-bit clear; set() writes a value and sets the
// present bit. write() lays out every section, computes content_hash + integrity
// crc, sets the SEALED flag, and writes the whole file in one pass.

#include <string>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

namespace atx::tsdb {

class SegmentBuilder {
public:
  /// Declare the grid shape: F field names (column order == field index), N
  /// symbol names (column order == instrument index), and the T ascending
  /// unix-nanos time axis. All field blocks start NaN / not-present.
  SegmentBuilder(std::vector<std::string> field_names, std::vector<std::string> symbols,
                 std::vector<atx::i64> time_axis);

  /// Set cell (field, t, inst) to `value` and mark it present.
  /// PRECONDITION: field < F, t < T, inst < N (ABORTS in debug).
  void set(atx::u32 field, atx::u64 t, atx::u32 inst, atx::f64 value) noexcept;

  /// Lay out + write the sealed segment to `path`. `created_at_nanos` is stamped
  /// into the header verbatim (caller supplies the clock). Err(IoError) if the
  /// file cannot be opened/written.
  [[nodiscard]] atx::core::Status write(const std::string &path, atx::i64 created_at_nanos) const;

private:
  std::vector<std::string> field_names_;
  std::vector<std::string> symbols_;
  std::vector<atx::i64> time_axis_;
  std::vector<atx::f64> blocks_;  // F*T*N, NaN-initialised
  std::vector<atx::u64> present_; // T*mask_words(N), zero-initialised
};

} // namespace atx::tsdb
