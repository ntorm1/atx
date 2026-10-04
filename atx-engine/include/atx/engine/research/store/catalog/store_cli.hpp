#pragma once

// atx::engine::research::store::catalog -- the `atx-research-store` command line (P9 SQL2;
// sql-design section 3.9). Exit codes: 0 ok, 2 usage, 3 refusal (a stale or missing pin under
// --strict, a sealed / oversized / outside-root path, a sealed --root or `cache init --import`
// DIR, a foreign or newer store (also as the target of `catalog --rebuild`), a changed
// trial-ledger line, two files claiming one candidate id or build tag), 4 error.
//
//   init --catalog DB
//   catalog --catalog DB --root R [--specs GLOB]... [--ledger L]... [--include P]...
//           [--rebuild] [--verify-payloads DIR] [--classes FILE]
//   ingest --catalog DB --class C --path P [--root R] [--classes FILE]
//   verify --catalog DB --pins [--spec S] [--strict]
//   query {artifacts|pins|stale-pins|runs|timings|producers} --catalog DB [--json]
//   digest --catalog DB
//   dump --catalog DB --table T
//   cache init DIR [--import]
//   cache prune --base DIR
//   schema --json --db catalog|cache
//   quick-check (--catalog DB | --cache DIR)
//
// `cache init` is the only way a cache index (<DIR>/index.sqlite) is created. Its help carries
// ruling SQL1-MON's warning: book_monitor.py --fit-work does not yet read records that exist
// only in a SQLite cache index (SQL3 carries the fix); no P9 cell uses an index (SQL-11).

#include <filesystem>
#include <iosfwd>
#include <span>
#include <string>

#include "atx/core/db/sqlite.hpp"
#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

namespace atx::engine::research::store::catalog {

inline constexpr int kExitOk = 0;
inline constexpr int kExitUsage = 2;
inline constexpr int kExitRefusal = 3;
inline constexpr int kExitError = 4;

// Run the CLI over `args` (the program name excluded); prints to `out` / `err`; returns the
// exit code.
[[nodiscard]] int run_store_cli(std::span<const std::string> args, std::ostream &out,
                                std::ostream &err);

struct CacheImportReport {
  i64 imported{};
  i64 present{};  // a row with this key existed already (kept)
  i64 rejected{}; // not a valid record-store file (name, schema, kind, key or content SHA-256)
  i64 sealed{};   // partition / kind directories never opened (a year token 2024-2099)
};

// `cache init DIR --import`: every legacy record-store file under `dir` (<dir>/<kind>/<sha>.json,
// partition "", and <dir>/<root>/<kind>/<sha>.json, partition <root>) that record_store.py's
// checks accept becomes a `record` row exactly as record_store.py writes one (compact canonical
// key, compact body in insertion order, bodies over 1 MiB in objects/<c[0:2]>/<c>.json).
// Seal (fail closed): Err(PermissionDenied) when `dir` itself holds a standalone year token
// 2024-2099 (seal_guard.hpp sealed_root), before anything is read; a partition or kind
// directory so named is skipped unopened (`sealed`); a file whose stem is not 64 lower-case hex
// is rejected unopened.
[[nodiscard]] core::Result<CacheImportReport>
import_legacy_records(core::db::Database &cache, const std::filesystem::path &dir);

// `cache prune --base DIR`: delete the `record` rows whose partition directory <dir>/<root> no
// longer exists; returns the number of rows deleted.
[[nodiscard]] core::Result<i64> prune_cache(core::db::Database &cache,
                                            const std::filesystem::path &dir);

} // namespace atx::engine::research::store::catalog
