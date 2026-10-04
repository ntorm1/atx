#pragma once

// atx::engine::research::store -- the record and catalog digests (contract K-P9-13,
// sql-design section 3.5).
//
// Record digest `atx.record-digest/v1`: SHA-256 over
//   "atx.record-digest/v1\n"
//   "row <table>@<version>\n"                      (begin_row; once per row, children follow)
//   "<column>=<value>\n"                           per non-volatile column, declared order
// with <value>:
//   ~                       NULL
//   i<decimal>              int (i64, '-' for negatives, no leading zeros)
//   b0 | b1                 bool
//   r<16 hex>               real: the IEEE-754 bits, no NaN or zero canonicalisation
//   u<16 hex>               u64
//   t<byte length>:<bytes>  text, sha256, relpath, json (UTF-8 bytes as stored)
//   x<byte length>:<bytes>  blob
// Hex digits are lower case, 16 per value (zero padded); lengths are decimal.
//
// Catalog digest `atx.catalog-digest/v1`: SHA-256 over
//   "atx.catalog-digest/v1\n" then "<table> <row digest>\n" for every row of every
//   non-volatile table, groups in the order given, tables in descriptor order, rows in
//   primary-key order (store.hpp catalog_digest).
//
// The digests are computed only here (C++). Python computes none; the gtest golden vectors
// come from the test-only oracle tests/fixtures/research_store/digest_oracle.py.

#include <cstddef>
#include <span>
#include <string>
#include <string_view>

#include "atx/core/sha256.hpp"
#include "atx/core/types.hpp"

namespace atx::engine::research::store {

inline constexpr std::string_view kRecordDigestDomain = "atx.record-digest/v1";
inline constexpr std::string_view kCatalogDigestDomain = "atx.catalog-digest/v1";

// An incremental digest stream. Single use: after finish() every other call is a contract
// violation (asserted in Debug). Not thread-safe; cheap to construct (no allocation).
class DigestStream {
public:
  enum class Kind : u8 { Record, Catalog };

  // Writes the domain line of `kind` ("atx.record-digest/v1\n" or the catalog one).
  explicit DigestStream(Kind kind = Kind::Record);

  // Record stream: "row <table>@<version>\n".
  void begin_row(std::string_view table, i32 version);
  // Record stream: one "<column>=<value>\n" line each (see the header comment).
  void add_null(std::string_view column);
  void add_int(std::string_view column, i64 value);
  void add_bool(std::string_view column, bool value);
  void add_real(std::string_view column, f64 value);
  void add_u64(std::string_view column, u64 value);
  void add_text(std::string_view column, std::string_view value);
  void add_blob(std::string_view column, std::span<const std::byte> value);
  // Catalog stream: "<table> <row digest>\n".
  void add_entry(std::string_view table, std::string_view row_digest);

  // The SHA-256 of everything written, as 64 lower-case hex digits. Call once.
  [[nodiscard]] std::string finish();

private:
  void write(std::string_view bytes);
  void write_column(std::string_view column, char tag);

  core::Sha256 sha_;
  bool finished_{false};
  bool failed_{false};
};

} // namespace atx::engine::research::store
