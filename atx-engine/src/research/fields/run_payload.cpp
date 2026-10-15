#include "atx/engine/research/fields/run_payload.hpp"

#include <algorithm>
#include <bit>
#include <cassert>
#include <cctype>
#include <cstring>
#include <limits>
#include <string>
#include <utility>

#include "atx/core/sha256.hpp"
#include "atx/engine/research/fields/file_io.hpp"

namespace atx::engine::research::fields {
namespace {

static_assert(std::endian::native == std::endian::little,
              "run payloads are little-endian f64 files read in place");

[[nodiscard]] core::Error refused(const FileRequest &request, std::string message) {
  return core::Error(core::ErrorCode::InvalidArgument,
                     "run payload " + request.path.string() + ": " + std::move(message));
}

[[nodiscard]] std::string lower(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(), [](char c) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  });
  return text;
}

} // namespace

core::Result<RunPayload> RunPayload::load(const FileRequest &request, const RoleAxes &role) {
  const usize dates = role.dates();
  const usize columns = role.instruments();
  if (columns != 0 && dates > std::numeric_limits<usize>::max() / columns / sizeof(f64)) {
    return core::Err(refused(request, "the role's shape is too large"));
  }
  const u64 want = static_cast<u64>(dates) * static_cast<u64>(columns) * sizeof(f64);
  ATX_TRY(const auto before, file_stamp(request.path));
  if (before.bytes != want) {
    return core::Err(refused(request, "holds " + std::to_string(before.bytes) +
                                          " bytes, the role's shape needs " +
                                          std::to_string(want)));
  }
  ATX_TRY(const auto blob, read_bounded(request.path, want));
  ATX_TRY(const auto after, file_stamp(request.path));
  if (!(after == before) || blob.size() != want) {
    return core::Err(core::ErrorCode::IoError,
                     "run payload " + request.path.string() + " changed while it was read");
  }
  ATX_TRY(const auto digest, core::sha256_hex(std::string_view(blob)));
  if (digest != lower(request.sha256)) {
    return core::Err(refused(request, "SHA-256 does not match the pin"));
  }
  RunPayload out;
  out.values_.resize(dates * columns);
  if (!blob.empty()) {
    std::memcpy(out.values_.data(), blob.data(), blob.size());
  }
  out.dates_ = dates;
  out.columns_ = columns;
  out.source_ = SourceRecord{record_path(request.path), want, digest};
  return core::Ok(std::move(out));
}

std::span<const f64> RunPayload::row(usize t) const noexcept {
  assert(t < dates_);
  return std::span<const f64>(values_).subspan(t * columns_, columns_);
}

} // namespace atx::engine::research::fields
