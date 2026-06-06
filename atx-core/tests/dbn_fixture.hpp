#pragma once

// Test-only builders: assemble a valid DBN v2 byte stream, zstd-compress it, and
// pack it into an in-memory PKZIP archive. Shared by dbn_test and databento_test.
// Uses the vendored miniz + the vcpkg zstd, both linked into the test target.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <miniz.h>
#include <zstd.h>

namespace atx::test {

struct OhlcvRow {
  std::uint32_t instrument_id{};
  std::uint64_t ts_event{}; // ns
  std::int64_t open{}, high{}, low{}, close{};
  std::uint64_t volume{};
  std::uint8_t rtype{0x23}; // OHLCV_1D
};

struct SymMap {
  std::uint32_t instrument_id{};
  std::string raw_symbol;
  std::uint32_t start_date{}; // YYYYMMDD
  std::uint32_t end_date{};   // YYYYMMDD
};

namespace detail {

inline void put_u16(std::vector<std::byte> &b, std::uint16_t v) {
  b.push_back(std::byte(v & 0xFF));
  b.push_back(std::byte((v >> 8) & 0xFF));
}
inline void put_u32(std::vector<std::byte> &b, std::uint32_t v) {
  for (int i = 0; i < 4; ++i) {
    b.push_back(std::byte((v >> (8 * i)) & 0xFF));
  }
}
inline void put_u64(std::vector<std::byte> &b, std::uint64_t v) {
  for (int i = 0; i < 8; ++i) {
    b.push_back(std::byte((v >> (8 * i)) & 0xFF));
  }
}
inline void put_i64(std::vector<std::byte> &b, std::int64_t v) {
  put_u64(b, static_cast<std::uint64_t>(v));
}
inline void put_cstr(std::vector<std::byte> &b, std::string_view s, std::size_t len) {
  for (std::size_t i = 0; i < len; ++i) {
    b.push_back(i < s.size() ? std::byte(static_cast<unsigned char>(s[i])) : std::byte(0));
  }
}
inline void put_record(std::vector<std::byte> &b, const OhlcvRow &r) {
  b.push_back(std::byte(14)); // length == 56 / 4
  b.push_back(std::byte(r.rtype));
  put_u16(b, 0); // publisher_id
  put_u32(b, r.instrument_id);
  put_u64(b, r.ts_event);
  put_i64(b, r.open);
  put_i64(b, r.high);
  put_i64(b, r.low);
  put_i64(b, r.close);
  put_u64(b, r.volume);
}

} // namespace detail

// Build an uncompressed DBN stream (version 1, 2, or 3). For v1 the symbol cstr
// length is fixed at 22 (the `symbol_cstr_len` argument is ignored and a
// record_count field is emitted); v2+ carries symbol_cstr_len in the header.
// Defaults keep fixtures compact (v2, 32-byte symbols).
inline std::vector<std::byte> build_dbn(const std::vector<SymMap> &maps,
                                        const std::vector<OhlcvRow> &rows,
                                        std::uint16_t symbol_cstr_len = 32,
                                        std::uint8_t version = 2) {
  using namespace detail;
  const std::uint16_t cstr_len = (version == 1) ? std::uint16_t{22} : symbol_cstr_len;
  std::vector<std::byte> b;
  b.push_back(std::byte('D'));
  b.push_back(std::byte('B'));
  b.push_back(std::byte('N'));
  b.push_back(std::byte(version));
  put_u32(b, 0);                           // frame_len placeholder @ offset 4
  const std::size_t meta_start = b.size(); // == 8

  // Fixed 100-byte metadata block (v1 and v2+ both total 100).
  put_cstr(b, "EQUS.SUMMARY", 16); // dataset
  put_u16(b, 8);                   // schema = Ohlcv1D
  put_u64(b, 0);                   // start
  put_u64(b, 0);                   // end
  put_u64(b, 0);                   // limit
  if (version == 1) {
    put_u64(b, static_cast<std::uint64_t>(rows.size())); // record_count (v1 only)
  }
  b.push_back(std::byte(0)); // stype_in
  b.push_back(std::byte(0)); // stype_out
  b.push_back(std::byte(0)); // ts_out
  if (version >= 2) {
    put_u16(b, cstr_len);                // symbol_cstr_len (v2+)
    b.insert(b.end(), 53, std::byte(0)); // reserved (53)
  } else {
    b.insert(b.end(), 47, std::byte(0)); // reserved (47)
  }

  // Variable sections.
  put_u32(b, 0);                                       // schema_definition_len
  put_u32(b, 0);                                       // symbols count
  put_u32(b, 0);                                       // partial count
  put_u32(b, 0);                                       // not_found count
  put_u32(b, static_cast<std::uint32_t>(maps.size())); // mappings count
  for (const auto &m : maps) {
    put_cstr(b, m.raw_symbol, cstr_len); // raw_symbol (key)
    put_u32(b, 1);                       // interval_count
    put_u32(b, m.start_date);
    put_u32(b, m.end_date);
    put_cstr(b, std::to_string(m.instrument_id), cstr_len); // out = iid string
  }

  // Backpatch frame_len = bytes from meta_start to here.
  const std::uint32_t frame_len = static_cast<std::uint32_t>(b.size() - meta_start);
  for (int i = 0; i < 4; ++i) {
    b[4 + static_cast<std::size_t>(i)] = std::byte((frame_len >> (8 * i)) & 0xFF);
  }

  for (const auto &r : rows) {
    put_record(b, r);
  }
  return b;
}

inline std::vector<std::byte> zstd_compress(std::span<const std::byte> in) {
  const std::size_t bound = ZSTD_compressBound(in.size());
  std::vector<std::byte> out(bound);
  const std::size_t n = ZSTD_compress(out.data(), bound, in.data(), in.size(), 3);
  out.resize(ZSTD_isError(n) ? 0 : n);
  return out;
}

// Build a PKZIP archive (STORE) from (name, bytes) entries.
inline std::vector<std::byte>
build_zip(const std::vector<std::pair<std::string, std::vector<std::byte>>> &entries) {
  mz_zip_archive zip{};
  mz_zip_writer_init_heap(&zip, 0, 0);
  for (const auto &e : entries) {
    mz_zip_writer_add_mem(&zip, e.first.c_str(), e.second.data(), e.second.size(),
                          MZ_NO_COMPRESSION);
  }
  void *buf = nullptr;
  std::size_t sz = 0;
  mz_zip_writer_finalize_heap_archive(&zip, &buf, &sz);
  std::vector<std::byte> out(static_cast<std::byte *>(buf), static_cast<std::byte *>(buf) + sz);
  mz_free(buf);
  mz_zip_writer_end(&zip);
  return out;
}

} // namespace atx::test
