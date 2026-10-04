#pragma once

// atx::engine::research::fields -- date-major role payload rows read in session order, hashed as
// they are read and verified against the role manifest at the end (prepare_research_fields.py
// RoleRows). A builder reads row t of a source only after it has written its own row t when its
// clock forbids same-session reads (vol_126), so the stream order is part of the look-ahead
// contract: no reader offers random access.

#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/sha256.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/research/fields/role_axes.hpp"

namespace atx::engine::research::fields {

// The element type of a role payload file.
enum class RowKind : u8 { F64, U8 };

class RoleRowReader {
public:
  // Opens payload `file` of `role` (dates x instruments elements of `kind`, little-endian). Err
  // when the manifest lists no such file, its recorded size disagrees with the role shape, or the
  // file cannot be opened.
  [[nodiscard]] static core::Result<RoleRowReader> open(const RoleAxes &role, std::string_view file,
                                                        RowKind kind);

  // The next row into `out` (instruments() elements). Err on a kind or size mismatch, after the
  // last row, or on a short file.
  [[nodiscard]] core::Status next(std::span<f64> out);
  [[nodiscard]] core::Status next(std::span<u8> out);

  // After the last row: Err unless every row was read, the file ends there and its bytes hash to
  // the manifest's SHA-256. The source record of the file.
  [[nodiscard]] core::Result<SourceRecord> finish();

private:
  RoleRowReader() = default;
  [[nodiscard]] core::Status read_row(RowKind kind, usize element_bytes);

  std::filesystem::path path_;
  std::ifstream in_;
  core::Sha256 sha_;
  std::vector<char> buffer_;
  FileReceipt receipt_;
  RowKind kind_{RowKind::F64};
  usize instruments_{};
  usize dates_{};
  usize rows_read_{};
};

} // namespace atx::engine::research::fields
