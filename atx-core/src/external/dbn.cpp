#include "atx/external/dbn.hpp"

#include <charconv>
#include <cstring>

namespace atx::external::dbn {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

namespace {

// Bounds-checked little-endian cursor over the borrowed buffer. Any read past the
// end leaves ok() == false and yields zero/empty values (callers check ok()).
class Reader {
public:
  explicit Reader(std::span<const std::byte> b, usize off = 0) noexcept : b_{b}, off_{off} {}

  [[nodiscard]] bool ok() const noexcept { return ok_; }
  void skip(usize n) noexcept { advance(n); }

  [[nodiscard]] u8 u8v() noexcept { return need(1) ? std::to_integer<u8>(b_[off_++]) : u8{0}; }
  // NB: read the bytes in SEPARATE statements -- operands of `|` are unsequenced.
  [[nodiscard]] u16 u16v() noexcept {
    u16 v = u8v();
    v = static_cast<u16>(v | (u16{u8v()} << 8));
    return v;
  }
  [[nodiscard]] u32 u32v() noexcept {
    u32 v = u8v();
    v |= u32{u8v()} << 8;
    v |= u32{u8v()} << 16;
    v |= u32{u8v()} << 24;
    return v;
  }
  [[nodiscard]] u64 u64v() noexcept {
    u64 v = 0;
    for (int i = 0; i < 8; ++i) {
      v |= u64{u8v()} << (8 * i);
    }
    return v;
  }
  [[nodiscard]] i64 i64v() noexcept { return static_cast<i64>(u64v()); }

  // Read a null-padded fixed-length cstr of `len` bytes.
  [[nodiscard]] std::string cstr(usize len) {
    if (!need(len)) {
      return {};
    }
    // SAFETY: byte -> char aliasing is permitted; the `len` bytes at off_ are
    // in-bounds (checked by need()).
    const char *p = reinterpret_cast<const char *>(b_.data() + off_);
    const usize n = ::strnlen(p, len);
    std::string s(p, n);
    off_ += len;
    return s;
  }

private:
  [[nodiscard]] bool need(usize n) noexcept {
    if (off_ + n > b_.size()) {
      ok_ = false;
      return false;
    }
    return true;
  }
  void advance(usize n) noexcept {
    if (need(n)) {
      off_ += n;
    }
  }

  std::span<const std::byte> b_;
  usize off_{0};
  bool ok_{true};
};

constexpr usize kPrefixLen = 8;      // "DBN" + version + u32 frame_len
constexpr usize kFixedMetaLen = 100; // METADATA_FIXED_LEN (all versions)
constexpr usize kDatasetLen = 16;    // DATASET_CSTR_LEN
constexpr usize kReservedLen = 53;   // METADATA_RESERVED_LEN (v2/v3)
constexpr usize kReservedLenV1 = 47; // METADATA_RESERVED_LEN (v1; v1 also has record_count)
constexpr u16 kSymbolCstrLenV1 = 22; // SYMBOL_CSTR_LEN (v1; v2+ carries it in the header)

} // namespace

Result<DbnDecoder> DbnDecoder::open(std::span<const std::byte> dbn) {
  if (dbn.size() < kPrefixLen) {
    return Err(ErrorCode::ParseError, "DBN: buffer shorter than prefix");
  }
  if (!(dbn[0] == std::byte('D') && dbn[1] == std::byte('B') && dbn[2] == std::byte('N'))) {
    return Err(ErrorCode::ParseError, "DBN: bad magic");
  }
  const u8 version = std::to_integer<u8>(dbn[3]);
  if (version < 1 || version > 3) {
    return Err(ErrorCode::NotImplemented, "DBN: unsupported version (expected 1-3)");
  }

  Reader r{dbn, 4};
  const u32 frame_len = r.u32v(); // metadata length following this field
  const usize records_off = kPrefixLen + frame_len;
  if (frame_len < kFixedMetaLen || records_off > dbn.size()) {
    return Err(ErrorCode::ParseError, "DBN: metadata frame out of range");
  }

  // Fixed 100-byte metadata block. v1 carries record_count (after limit) and a
  // fixed symbol_cstr_len (22) with 47 reserved bytes; v2+ drops record_count,
  // carries symbol_cstr_len in the header, and has 53 reserved bytes. Both land
  // the record stream at offset 108 (== 8 + 100).
  DbnDecoder dec;
  dec.buf_ = dbn;
  dec.meta_.version = version;
  dec.meta_.dataset = r.cstr(kDatasetLen);
  dec.meta_.schema = r.u16v();
  (void)r.u64v(); // start
  (void)r.u64v(); // end
  (void)r.u64v(); // limit
  if (version == 1) {
    (void)r.u64v(); // record_count (v1 only)
  }
  (void)r.u8v(); // stype_in
  (void)r.u8v(); // stype_out
  (void)r.u8v(); // ts_out
  u16 sym_len = 0;
  if (version >= 2) {
    sym_len = r.u16v();   // symbol_cstr_len (v2+)
    r.skip(kReservedLen); // reserved (53) -> offset 108
  } else {
    sym_len = kSymbolCstrLenV1; // fixed 22 (v1)
    r.skip(kReservedLenV1);     // reserved (47) -> offset 108
  }
  dec.meta_.symbol_cstr_len = sym_len;
  if (sym_len == 0) {
    return Err(ErrorCode::ParseError, "DBN: zero symbol_cstr_len");
  }

  // Variable sections.
  const u32 schema_def_len = r.u32v();
  r.skip(schema_def_len);
  for (int section = 0; section < 3; ++section) { // symbols, partial, not_found
    const u32 count = r.u32v();
    r.skip(static_cast<usize>(count) * sym_len);
  }
  const u32 map_count = r.u32v();
  for (u32 i = 0; i < map_count; ++i) {
    std::string raw = r.cstr(sym_len);
    const u32 ivl = r.u32v();
    for (u32 j = 0; j < ivl; ++j) {
      (void)r.u32v();                          // start_date
      (void)r.u32v();                          // end_date
      const std::string out = r.cstr(sym_len); // instrument_id as string
      u32 iid = 0;
      const auto res = std::from_chars(out.data(), out.data() + out.size(), iid);
      if (res.ec == std::errc{} && res.ptr == out.data() + out.size()) {
        dec.mappings_.try_emplace(iid, raw); // first interval wins per iid
      }
    }
  }
  if (!r.ok()) {
    return Err(ErrorCode::ParseError, "DBN: truncated metadata");
  }

  dec.cursor_ = records_off;
  return Ok(std::move(dec));
}

std::string_view DbnDecoder::symbol_for(u32 instrument_id) const noexcept {
  const auto it = mappings_.find(instrument_id);
  return it != mappings_.end() ? std::string_view{it->second} : std::string_view{};
}

Result<std::optional<OhlcvMsg>> DbnDecoder::next() {
  while (cursor_ < buf_.size()) {
    if (cursor_ + 16 > buf_.size()) {
      return Err(ErrorCode::ParseError, "DBN: truncated record header");
    }
    const u8 length = std::to_integer<u8>(buf_[cursor_]);
    if (length == 0) {
      return Err(ErrorCode::ParseError, "DBN: zero-length record");
    }
    const usize rec_len = static_cast<usize>(length) * 4;
    if (cursor_ + rec_len > buf_.size()) {
      return Err(ErrorCode::ParseError, "DBN: record exceeds buffer");
    }
    const u8 rtype = std::to_integer<u8>(buf_[cursor_ + 1]);

    if (rtype == static_cast<u8>(RType::Ohlcv1D) || rtype == static_cast<u8>(RType::OhlcvEod)) {
      if (rec_len < 56) {
        return Err(ErrorCode::ParseError, "DBN: OHLCV record too short");
      }
      Reader r{buf_, cursor_};
      OhlcvMsg m{};
      m.hd.length = r.u8v();
      m.hd.rtype = r.u8v();
      m.hd.publisher_id = r.u16v();
      m.hd.instrument_id = r.u32v();
      m.hd.ts_event = r.u64v();
      m.open = r.i64v();
      m.high = r.i64v();
      m.low = r.i64v();
      m.close = r.i64v();
      m.volume = r.u64v();
      cursor_ += rec_len;
      if (!r.ok()) {
        return Err(ErrorCode::ParseError, "DBN: truncated OHLCV record");
      }
      return Ok(std::optional<OhlcvMsg>{m});
    }

    cursor_ += rec_len; // skip unsupported rtype
    ++skipped_;
  }
  return Ok(std::optional<OhlcvMsg>{});
}

} // namespace atx::external::dbn
