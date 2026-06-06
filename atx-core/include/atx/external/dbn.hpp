#pragma once

// atx::external::dbn — pure decoder for Databento Binary Encoding (DBN) v1/v2/v3.
//
// No zip, no zstd, no Arrow: consumes an already-decompressed DBN byte span and
// yields OHLCV records (rtype 0x23 ohlcv-1d, 0x24 ohlcv-eod). Other record types
// are counted and skipped. Symbols resolve from the metadata symbol-mapping
// section -- no JSON sidecar. Layout: see
// atx-core/plans/2026-06-06-databento-loader.md.
//
// All multi-byte integers are little-endian. Prices are i64 in units of 1e-9;
// the missing-value sentinel is i64::MAX.

#include <cstddef> // std::byte
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

namespace atx::external::dbn {

using atx::core::Result;

inline constexpr i64 kFixedPriceScale = 1'000'000'000;              // 1e-9 per unit
inline constexpr i64 kUndefPrice = std::numeric_limits<i64>::max(); // missing price

enum class RType : u8 {
  Ohlcv1S = 0x20,
  Ohlcv1M = 0x21,
  Ohlcv1H = 0x22,
  Ohlcv1D = 0x23,
  OhlcvEod = 0x24,
};

// 16-byte common record header.
struct RecordHeader {
  u8 length{}; // record size in 32-bit words (multiply by 4 for bytes)
  u8 rtype{};  // RType
  u16 publisher_id{};
  u32 instrument_id{};
  u64 ts_event{}; // ns since the UNIX epoch
};

// 56-byte OHLCV record (RecordHeader.length == 14).
struct OhlcvMsg {
  RecordHeader hd{};
  i64 open{};
  i64 high{};
  i64 low{};
  i64 close{};
  u64 volume{};
};

struct DbnMetadata {
  u8 version{};
  std::string dataset;
  u16 schema{};
  u16 symbol_cstr_len{};
};

// Streaming OHLCV reader over a contiguous DBN buffer (borrowed; the span must
// outlive the decoder). Move-only.
class DbnDecoder {
public:
  [[nodiscard]] static Result<DbnDecoder> open(std::span<const std::byte> dbn);

  DbnDecoder(DbnDecoder &&) noexcept = default;
  DbnDecoder &operator=(DbnDecoder &&) noexcept = default;
  DbnDecoder(const DbnDecoder &) = delete;
  DbnDecoder &operator=(const DbnDecoder &) = delete;
  ~DbnDecoder() = default;

  [[nodiscard]] const DbnMetadata &metadata() const noexcept { return meta_; }

  // instrument_id -> raw symbol from the metadata mappings; empty if unmapped.
  [[nodiscard]] std::string_view symbol_for(u32 instrument_id) const noexcept;

  // Next OHLCV record, skipping (and counting) unsupported rtypes. nullopt at end.
  [[nodiscard]] Result<std::optional<OhlcvMsg>> next();

  [[nodiscard]] i64 skipped_records() const noexcept { return skipped_; }

private:
  DbnDecoder() = default;

  std::span<const std::byte> buf_{};
  usize cursor_{0}; // offset of the next record
  DbnMetadata meta_{};
  std::unordered_map<u32, std::string> mappings_; // instrument_id -> raw_symbol
  i64 skipped_{0};
};

} // namespace atx::external::dbn
