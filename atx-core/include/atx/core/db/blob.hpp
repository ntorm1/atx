#pragma once

// atx::core::db — BlobStream: RAII incremental BLOB I/O over sqlite3_blob.
//
// SQLite can open a handle to a single BLOB value already stored in a row and
// read/write it in pieces without materializing the whole value in memory
// (sqlite3_blob_open/read/write/close). BlobStream wraps that handle with RAII
// (closes on destruction) and Result returns.
//
// The BLOB must already exist with its final size: incremental writes cannot
// grow or shrink the value. Pre-size with `zeroblob(N)` (or a prior INSERT of
// an N-byte blob) before opening writable.
//
// Opaque handle: forward-declares sqlite3_blob; <sqlite3.h> is included only in
// src/db/sqlite.cpp.

#include <cstddef>     // std::byte
#include <span>        // std::span
#include <string_view> // std::string_view

#include "atx/core/db/sqlite.hpp" // Database (connection the blob is opened on)
#include "atx/core/error.hpp"     // Result, Status
#include "atx/core/types.hpp"     // i64, usize

struct sqlite3_blob; // opaque; defined in <sqlite3.h>

namespace atx::core::db {

// ===========================================================================
//  BlobStream — an open handle to one stored BLOB value (RAII, move-only).
// ===========================================================================
class BlobStream {
public:
  // Open the BLOB in `table`.`column` at the given ROWID on `db`. `writable`
  // selects read/write vs read-only. `schema` is the attached-database name
  // ("main" by default). Returns Err if the row/column/blob does not exist.
  [[nodiscard]] static Result<BlobStream> open(Database &db, std::string_view table,
                                               std::string_view column, i64 rowid, bool writable,
                                               std::string_view schema = "main");

  ~BlobStream() noexcept;
  BlobStream(BlobStream &&other) noexcept;
  BlobStream &operator=(BlobStream &&other) noexcept;
  BlobStream(const BlobStream &) = delete;
  BlobStream &operator=(const BlobStream &) = delete;

  // Size of the BLOB in bytes (fixed for the lifetime of the handle).
  [[nodiscard]] i64 size() const noexcept;

  // Read up to out.size() bytes starting at `offset`. Returns the number of
  // bytes read (== out.size() on success). Err if [offset, offset+size) is out
  // of range or on I/O error.
  [[nodiscard]] Result<usize> read(std::span<std::byte> out, i64 offset);

  // Write in.size() bytes starting at `offset`. The handle must have been
  // opened writable and [offset, offset+size) must lie within the BLOB. Err
  // otherwise (the BLOB cannot be resized).
  [[nodiscard]] Status write(std::span<const std::byte> in, i64 offset);

private:
  explicit BlobStream(sqlite3_blob *blob) noexcept : blob_{blob} {}

  sqlite3_blob *blob_{nullptr}; // owned; closed in the destructor
};

} // namespace atx::core::db
