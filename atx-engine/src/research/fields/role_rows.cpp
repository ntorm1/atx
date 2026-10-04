#include "atx/engine/research/fields/role_rows.hpp"

#include <bit>
#include <cstring>
#include <ios>
#include <string>
#include <utility>

#include "atx/engine/research/fields/file_io.hpp"

namespace atx::engine::research::fields {
namespace {

static_assert(std::endian::native == std::endian::little, "role payloads are little-endian");

[[nodiscard]] usize element_bytes(RowKind kind) noexcept {
  return kind == RowKind::F64 ? sizeof(f64) : sizeof(u8);
}

} // namespace

core::Result<RoleRowReader> RoleRowReader::open(const RoleAxes &role, std::string_view file,
                                                RowKind kind) {
  ATX_TRY(auto receipt, role.receipt(file));
  const u64 expected = static_cast<u64>(role.dates()) * static_cast<u64>(role.instruments()) *
                       static_cast<u64>(element_bytes(kind));
  if (receipt.bytes != expected) {
    return core::Err(core::ErrorCode::InvalidArgument,
                     "research role: " + std::string(file) +
                         " is missing or its size disagrees with the role shape");
  }
  RoleRowReader out;
  out.path_ = role.directory() / std::string(file);
  out.in_.open(out.path_, std::ios::binary);
  if (!out.in_) {
    return core::Err(core::ErrorCode::IoError, "research role: cannot open " + out.path_.string());
  }
  out.receipt_ = std::move(receipt);
  out.kind_ = kind;
  out.instruments_ = role.instruments();
  out.dates_ = role.dates();
  out.buffer_.assign(out.instruments_ * element_bytes(kind), '\0');
  return core::Ok(std::move(out));
}

core::Status RoleRowReader::read_row(RowKind kind, usize element) {
  if (kind != kind_ || element != element_bytes(kind_)) {
    return core::Err(core::ErrorCode::InvalidArgument,
                     "research role: row kind mismatch for " + path_.string());
  }
  if (rows_read_ >= dates_) {
    return core::Err(core::ErrorCode::OutOfRange,
                     "research role: read past the last row of " + path_.string());
  }
  in_.read(buffer_.data(), static_cast<std::streamsize>(buffer_.size()));
  if (static_cast<usize>(in_.gcount()) != buffer_.size()) {
    return core::Err(core::ErrorCode::IoError,
                     "research role: " + path_.string() + " is truncated");
  }
  ATX_TRY_VOID(sha_.update(std::as_bytes(std::span<const char>(buffer_))));
  ++rows_read_;
  return core::Ok();
}

core::Status RoleRowReader::next(std::span<f64> out) {
  if (out.size() != instruments_) {
    return core::Err(core::ErrorCode::InvalidArgument, "research role: row size mismatch");
  }
  ATX_TRY_VOID(read_row(RowKind::F64, sizeof(f64)));
  std::memcpy(out.data(), buffer_.data(), buffer_.size());
  return core::Ok();
}

core::Status RoleRowReader::next(std::span<u8> out) {
  if (out.size() != instruments_) {
    return core::Err(core::ErrorCode::InvalidArgument, "research role: row size mismatch");
  }
  ATX_TRY_VOID(read_row(RowKind::U8, sizeof(u8)));
  std::memcpy(out.data(), buffer_.data(), buffer_.size());
  return core::Ok();
}

core::Result<SourceRecord> RoleRowReader::finish() {
  if (rows_read_ != dates_) {
    return core::Err(core::ErrorCode::InvalidArgument,
                     "research role: " + path_.string() + " finished before its last row");
  }
  if (in_.peek() != std::char_traits<char>::eof()) {
    return core::Err(core::ErrorCode::InvalidArgument,
                     "research role: " + path_.string() + " is longer than the role shape");
  }
  ATX_TRY(const auto digest, sha_.finalize());
  if (hex_digest(digest) != receipt_.sha256) {
    return core::Err(core::ErrorCode::InvalidArgument,
                     "research role: " + path_.string() + " bytes do not match the role manifest");
  }
  return core::Ok(SourceRecord{record_path(path_), receipt_.bytes, receipt_.sha256});
}

} // namespace atx::engine::research::fields
