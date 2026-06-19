#include "atx/engine/regime/store.hpp"

#include <algorithm>
#include <limits>

#include "atx/tsdb/segment_reader.hpp"

namespace atx::engine::regime {

namespace {
constexpr atx::f64 kNaN = std::numeric_limits<atx::f64>::quiet_NaN();
}  // namespace

atx::core::Result<RegimeStore> RegimeStore::open(const std::string &seg_path) {
  ATX_TRY(auto reader, atx::tsdb::SegmentReader::attach(seg_path));
  const atx::u32 inst_n = reader.instrument_count();
  if (inst_n != 1U) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "RegimeStore::open: expected exactly 1 instrument (MACRO)");
  }
  RegimeStore store;
  const std::span<const atx::i64> times = reader.times();
  store.axis_.assign(times.begin(), times.end());
  const atx::u32 field_n = reader.field_count();
  store.names_.reserve(field_n);
  store.cols_.reserve(field_n);
  for (atx::u32 f = 0; f < field_n; ++f) {
    store.names_.emplace_back(reader.field_name(f));
    // N==1, so the block (length T*N == T, date-major) IS the per-date column.
    const std::span<const atx::f64> block = reader.field_block_view(f);
    store.cols_.emplace_back(block.begin(), block.end());
  }
  if (store.names_.empty() || store.axis_.empty()) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "RegimeStore::open: empty/invalid segment");
  }
  return atx::core::Ok(std::move(store));
}

atx::f64 RegimeStore::value(std::string_view series, atx::i64 t_nanos) const noexcept {
  atx::usize fi = static_cast<atx::usize>(-1);
  for (atx::usize i = 0; i < names_.size(); ++i) {
    if (names_[i] == series) { fi = i; break; }
  }
  if (fi == static_cast<atx::usize>(-1)) return kNaN;
  const auto it = std::upper_bound(axis_.begin(), axis_.end(), t_nanos);
  if (it == axis_.begin()) return kNaN;  // before first axis date
  const atx::usize di = static_cast<atx::usize>((it - axis_.begin()) - 1);
  return cols_[fi][di];
}

}  // namespace atx::engine::regime
