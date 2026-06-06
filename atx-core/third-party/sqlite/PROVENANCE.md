# SQLite amalgamation — vendored third-party source

This directory contains the **SQLite amalgamation** (single-file C source distribution),
vendored verbatim from the official upstream. It is wrapped by the RAII / `Result`-based
C++ wrapper in [`../../include/atx/core/db/`](../../include/atx/core/db/) and compiled as the
`atx_sqlite3` static library (see `../../CMakeLists.txt`).

## Source

| Field | Value |
|---|---|
| Product | SQLite C source amalgamation |
| Version | **3.53.2** |
| Upstream URL | https://sqlite.org/2026/sqlite-amalgamation-3530200.zip |
| Download date | 2026-06-06 |
| Archive size | 2,943,292 bytes (verified — exact match) |
| Upstream SHA3-256 (archive) | `81142986038e18f96c4a54e1a72562ae17e502a916f2a7701eff43388cbf1a40` |

## Integrity

- The archive was fetched over **HTTPS from the official `sqlite.org` domain**, and its
  byte size matched the upstream-published size exactly (2,943,292 bytes).
- The upstream publishes a **SHA3-256** hash for the archive (above). SHA3-256 is **not**
  available via Windows PowerShell 5.1 `Get-FileHash` (which supports only
  SHA1/256/384/512/MD5), so it was **not** machine-verified on this host. To verify
  manually on a host with SHA3 support (e.g. `openssl dgst -sha3-256`, or .NET 5+ /
  PowerShell 7 `System.Security.Cryptography.SHA3_256`), re-download the archive and
  compare against the published hash above.
- For local change-detection pinning, the SHA-256 of the extracted `sqlite3.c` as vendored
  here is:

  ```
  sqlite3.c  SHA256: 0A409F1633283FA31A9126B11FBFD64A1991C5D30DEFAD07E5745D4667F5E23D
  ```

## Files

| File | Purpose | Vendored |
|---|---|---|
| `sqlite3.c` | the amalgamation (the entire SQLite library in one C file) | ✅ |
| `sqlite3.h` | public C API header | ✅ |
| `sqlite3ext.h` | loadable-extension interface header | ✅ |
| `shell.c` | the `sqlite3` CLI shell — **not** vendored (not needed) | ❌ |

## Compile-time configuration

`sqlite3.c` is compiled by the `atx_sqlite3` CMake target with **warnings disabled** (it is
third-party C and must not trip the project's `/W4 /WX` gate) and these defines (see
`atx-core/CMakeLists.txt` for the authoritative list):

- `SQLITE_THREADSAFE=2` — multi-thread mode: safe from multiple threads **provided no single
  connection is shared across threads** (one `Database` per thread). The wrapper documents
  this rule.
- `SQLITE_DQS=0` — disable the double-quoted-string-literal misfeature (treat `"..."` as an
  identifier only; a hardening default).
- `SQLITE_DEFAULT_FOREIGN_KEYS=1` — foreign-key enforcement on by default.
- `SQLITE_DEFAULT_MEMSTATUS=0` — skip per-call memory accounting (perf).
- `SQLITE_LIKE_DOESNT_MATCH_BLOBS`, `SQLITE_OMIT_DEPRECATED`, `SQLITE_USE_ALLOCA`,
  `SQLITE_ENABLE_COLUMN_METADATA`, `SQLITE_ENABLE_FTS5` — recommended app hardening / features.

## Updating

To bump the vendored version: download the new amalgamation from
<https://sqlite.org/download.html>, replace `sqlite3.c` / `sqlite3.h` / `sqlite3ext.h`, and
update the version, URL, size, and hashes in this file. Re-run the `atx_sqlite3` build and the
`atx/core/db` test suite.
