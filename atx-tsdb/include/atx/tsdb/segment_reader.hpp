#pragma once

// atx::tsdb::SegmentReader — attach a sealed segment file read-only and read its
// dense grid with O(1) addressing. attach() validates magic/version/SEALED/crc
// before exposing one byte; a bad file returns Err, never UB. Owns the Mapping.

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility> // std::move

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

#include "atx/tsdb/mapping.hpp"
#include "atx/tsdb/segment.hpp"

namespace atx::tsdb {

class SegmentReader {
public:
  /// Map + validate `path`. Err(InvalidArgument) on bad magic/version/unsealed/
  /// short file; Err(IoError) if it cannot be mapped (propagated from Mapping);
  /// Err(Internal) on integrity-crc mismatch.
  [[nodiscard]] static atx::core::Result<SegmentReader> attach(const std::string &path);

  [[nodiscard]] atx::u64 time_count() const noexcept { return header().time_count; }
  [[nodiscard]] atx::u32 instrument_count() const noexcept { return header().instrument_count; }
  [[nodiscard]] atx::u32 field_count() const noexcept { return header().field_count; }
  [[nodiscard]] atx::u64 content_hash() const noexcept { return header().content_hash; }

  /// The ascending unix-nanos time axis (T entries).
  [[nodiscard]] std::span<const atx::i64> times() const noexcept {
    return time_axis(map_.base(), header());
  }

  /// Field index for `name`, or nullopt if absent. O(F) over the directory.
  [[nodiscard]] std::optional<atx::u32> field_index(std::string_view name) const noexcept;

  /// Symbol name for instrument column `inst`. PRECONDITION: inst < N (ABORTS).
  [[nodiscard]] std::string_view symbol_name(atx::u32 inst) const noexcept;

  /// Cell value. PRECONDITION: field < F, t < T, inst < N (ABORTS in debug).
  [[nodiscard]] atx::f64 value(atx::u32 field, atx::u64 t, atx::u32 inst) const noexcept {
    return cell_value(map_.base(), header(), field, t, inst);
  }

  /// Cell presence. PRECONDITION: t < T, inst < N (ABORTS in debug).
  [[nodiscard]] bool present(atx::u64 t, atx::u32 inst) const noexcept {
    return cell_present(map_.base(), header(), t, inst);
  }

  /// Newest row index whose time <= now_nanos, or nullopt if none is visible.
  /// This is the no-look-ahead cutoff: a reader must never address past it.
  [[nodiscard]] std::optional<atx::u64> cutoff_index(atx::i64 now_nanos) const noexcept;

  /// Make the mapping resident (best-effort).
  void prefetch() const noexcept { map_.prefetch(); }

private:
  explicit SegmentReader(Mapping map) noexcept : map_{std::move(map)} {}

  [[nodiscard]] const SegmentHeader &header() const noexcept {
    // SAFETY: attach() verified size >= sizeof(SegmentHeader) and the magic, so
    // the first bytes are a valid header.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    return *reinterpret_cast<const SegmentHeader *>(map_.base());
  }

  Mapping map_;
};

} // namespace atx::tsdb
