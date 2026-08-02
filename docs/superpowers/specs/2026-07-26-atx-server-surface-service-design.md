# atx-server — Surface Service Foundation — Design

Date: 2026-07-26
Status: **SUPERSEDED** by `2026-08-02-atx-server-rpc-foundation-design.md`
Scope: new `atx-proto/` + `atx-server/` subprojects; no change to `atx-ui` in this spec

> **Superseded 2026-08-02.** This document specified gRPC as the transport. gRPC
> was never installed — the vcpkg build ran out of memory and was abandoned, and
> `C:/atx-cache/vcpkg_installed/x64-windows` contains no gRPC libraries and no
> `grpc_cpp_plugin.exe`. The replacement spec keeps the catalog, realm,
> entitlement, encoder, and blob designs essentially unchanged, and substitutes
> length-prefixed protobuf over raw TCP for the transport. Read the replacement,
> not this. This is retained for the SpiderRock mapping in §3 and the design
> rationale it records.

## 1. Goal

Stand up `atx-server`: the central, long-running C++ process that serves as the
backbone of the ATX American-equity option trading platform. It accepts many
concurrent client connections over gRPC and answers data requests against the
`atx-vol` surface database.

This document specifies **spec 1 of 3**. The full ask — server + backtest
service + a modular `atx-ui` — decomposes as:

| # | Spec | Deliverable | Depends on |
|---|---|---|---|
| **1** | **atx-proto + atx-server surface read path** (this document) | proto contract package, gRPC build wiring, server runtime (catalog / realm / auth / encoders), `SurfaceService` + `AdminService`, `atx-server-cli` test client, full test gate | — |
| 2 | atx-ui shell rework + client session layer | `atx-ui-client` (session, snapshot store, `RemoteSurfaceSource`), command bus, link contexts, panel registry, workspace host; the six existing vol panels become one registered workspace | 1 |
| 3 | BacktestService + backtest workspaces | run-archive catalog RPCs, async job submission (worker pool, job table, restart recovery), Run Catalog / Run Detail workspaces | 1, 2 |

The split is not cosmetic. Spec 2's shell rework needs spec 1's wire proven
end to end. Spec 3's UI needs spec 2's workspace registry. Spec 3's job
scheduler is a distinct hazard class — persistence, cancellation, restart
recovery — that must not ride into the codebase alongside the read path.

## 2. Current state

**What exists.**

- `atx-vol/include/atx/vol/surface_db.hpp` — `SurfaceDb` is a directory:
  `manifest.atxdb` (symbol table + partition index, CRC-validated, generation
  stamped, atomic rewrite) over `partitions/*.atxvsa` (ATXVSA v3 archives).
  `map_surface` gives a zero-copy `PricedSurfaceView` over an LRU-bounded
  partition mmap cache; `load_surface` gives an owned `PricedSurface`;
  `refresh()` picks up an external writer's generation bump. Const queries are
  documented thread-safe over an immutable manifest snapshot.
- `atx-core/include/atx/core/db/sqlite.hpp` + `blob.hpp` — RAII `Database` /
  `Statement` / `Transaction` / `BlobStream` over vendored SQLite
  (`atx_sqlite3`), built with `SQLITE_THREADSAFE=2`, `SQLITE_DQS=0`,
  `SQLITE_DEFAULT_FOREIGN_KEYS=1`, `SQLITE_ENABLE_FTS5`. `prepare_cached`
  returns address-stable prepared statements; `Database::backup_to` performs an
  online database-to-database copy.
- `atx-core/include/atx/core/sha256.hpp` — for token digests.
- `atx-ui/` — a single-workspace desktop app. `main.cpp` hard-wires one
  `OpraVolSurface` (parquet load + in-process fit) into one `VolWorkspace` with
  six fixed panels. `VolSurfaceSource` is the only abstraction seam.

**What is missing.** There is no server process, no wire contract, no
multi-client story, and no way to ask the realm a cross-cutting question
("which days have a fitted SPY surface?") without sweeping every archive index
on disk. Every consumer today is a process that opens the database files
directly.

**Dependency gap.** gRPC and protobuf are not in the vcpkg installed set
(`arrow`, `abseil`, `zstd`, `zlib-ng`, `openssl`, `gtest` are). The `protoc`
currently on `PATH` is an unpinned chocolatey install (libprotoc 29.1) and must
not be what the build uses.

## 3. SpiderRock mapping

The design follows SpiderRock Connect's system architecture. Explicit mapping,
including what is deliberately excluded:

| SpiderRock | atx-server | v1 |
|---|---|---|
| SysEnvironment / SysRealm | `Realm` — a named catalog binding `db_id` → data root | yes |
| MBus typed messages, primary-keyed | proto messages in package `atx.rpc.v1`, explicit key messages | yes |
| TickerKey → ExpiryKey → OptionKey | `SymbolKey` → `ExpiryKey` → `OptionKey`, plus `SurfaceKey{db_id, partition_key, SymbolKey}` | yes |
| MLink service gateway | the `atx-server` process hosting service implementations | yes |
| MLink Tokens (API access control) | `AuthToken` request metadata → `Entitlements` resolved from the catalog | yes |
| Cache service | `Catalog` + `SurfaceRegistry` over `SurfaceDb`'s existing LRU partition mmap cache | yes |
| Catchup-on-restart | catalog warm-start snapshot + per-`db_id` generation drift rescan | yes |
| Messages "live until replaced" by primary key | every response stamps `{db_generation, content_hash}`; clients revalidate rather than refetch | yes |
| SRSE SQL gateway (MariaDB) | replaced by the embedded SQLite `Catalog`. A read-only SQL *gateway* RPC is deferred (§10) | partial |
| Cache/cleanup service | LRU bound only | no |
| MLink/WebSocket subscriptions, SpiderStream multicast | deferred to a streaming spec | no |
| FIX order entry | out of scope for this platform stage | no |

**Inherited hard rule**, from `atx-ui/docs/platform-architecture.md`: no
filesystem path ever crosses the wire. Clients address data as
`(db_id, partition_key, symbol)`. The realm is the only place paths exist, and
it is also the entitlement boundary.

## 4. Architecture

### 4.1 Process shape

One `atx-server` binary. Synchronous gRPC server with a bounded thread pool
(`grpc::ResourceQuota`). `SurfaceDb` const queries are already thread-safe over
an immutable manifest snapshot, so surface reads fan out with no added locking.
No async/callback API in v1: read latency is dominated by disk and ATXVSA
decode, not by request scheduling.

### 4.2 `atx-proto/` — the wire contract

A new top-level directory. The contract is shared by the server, the future
`atx-ui-client`, and any future Python or web client, so it belongs to neither
side.

```
atx-proto/
  CMakeLists.txt
  atx/rpc/v1/
    keys.proto             SymbolKey, ExpiryKey, OptionKey, PartitionKey, SurfaceKey
    common.proto           ResponseMeta, Page, IsoDate, ErrorDetail
    surface.proto          SurfaceMeta, ExpirySummary, VolCurveSlice, VolQuotePoint,
                           SurfaceDiagnostics, SymbolFitConfig, SurfaceBlob
    surface_service.proto  service SurfaceService
    admin.proto            service AdminService, RealmConfig, DatabaseInfo
```

Produces one static CMake target, `atx-proto`, generating `.pb.cc` and
`.grpc.pb.cc` via `protobuf_generate` and the `grpc_cpp_plugin` from the vcpkg
tree.

`RealmConfig` doubles as the realm import/export format (§4.4), so the server
needs no JSON library beyond `google::protobuf::util::JsonStringToMessage`. The
repository has no JSON dependency outside vendored `databento-cpp`, and this
design does not add one.

### 4.3 `atx-server/`

```
atx-server/
  CMakeLists.txt
  include/atx/server/
    config.hpp            ServerConfig: listen address, TLS, limits, state path
    realm.hpp             Realm: db_id -> {kind, root}; the only path holder
    catalog.hpp           Catalog: SQLite in-memory index + on-disk state (§5)
    catalog_refresh.hpp   CatalogRefresher: generation-drift poller
    surface_registry.hpp  lazily-opened SurfaceDb per db_id
    encode.hpp            atx::vol domain -> proto. Pure functions, no gRPC.
    auth.hpp              token digest -> Entitlements
    service_error.hpp     atx::vol::Result -> grpc::Status mapping (§7)
    service_surface.hpp   SurfaceServiceImpl
    service_admin.hpp     AdminServiceImpl
    server.hpp            Server: build / start / wait / shutdown
  src/                    one .cpp per header
  tools/main.cpp          the atx-server binary
  tools/cli.cpp           the atx-server-cli test/ops client
  tests/
```

`encode.hpp` is the load-bearing boundary. Every domain-to-wire translation is
a free function testable without a server, a socket, or a port. It derives the
view models (expiry summaries, curve slices, quote points, diagnostics) from
`atx-vol` domain types directly. This deliberately duplicates logic that today
lives in `atx-ui/src/vol/spy_opra_surface.cpp`; spec 2 deletes the UI-side copy
when the UI switches to the remote source. The duplication is scoped and
temporary, and it is called out here so it is not mistaken for an oversight.

### 4.4 Realm

`Realm` is the SysRealm analogue: a named set of databases the server serves.
It lives in the catalog's on-disk `state.realm` table (§5), not in a config
file. Databases are registered through `AdminService.RegisterDatabase`.
`RealmConfig` (proto) is the import/export format:
`atx-server --realm-import realm.json` on first run, and
`AdminService.ExportRealm` for inspection and backup.

A `db_id` is an opaque client-facing identifier. Roots never leave the process.

## 5. Catalog — the SRSE analogue

Two tiers, one connection, joined by `ATTACH`:

```
main   (":memory:")          DERIVED.       Reconstructible from disk. Rebuilt at start.
state  (state.db, WAL)       AUTHORITATIVE. Not reconstructible. Survives restart.
```

### 5.1 Schema

```sql
-- main: the coverage index.
CREATE TABLE db_source(db_id TEXT PRIMARY KEY, kind TEXT,
                       generation INTEGER, scanned_ns INTEGER);
CREATE TABLE symbol   (db_id TEXT, symbol TEXT, enabled INT, preset INT,
                       curve_kind INT, config_blob BLOB, provenance_blob BLOB,
                       PRIMARY KEY(db_id, symbol));
CREATE TABLE partition(db_id TEXT, key TEXT, surface_count INT, file_size INT,
                       created_ns INT, PRIMARY KEY(db_id, key));
CREATE TABLE surface  (db_id TEXT, part_key TEXT, symbol TEXT, expiry_count INT,
                       spot REAL, model_kind INT, risk_state INT,
                       PRIMARY KEY(db_id, part_key, symbol));
CREATE INDEX surface_by_symbol ON surface(symbol, part_key);
CREATE VIRTUAL TABLE symbol_fts USING fts5(symbol, tags, content='');

-- state: server truth.
CREATE TABLE schema_version(version INTEGER NOT NULL);
CREATE TABLE realm      (db_id TEXT PRIMARY KEY, kind TEXT, root TEXT, added_ns INT);
CREATE TABLE token      (token_sha256 BLOB PRIMARY KEY, label TEXT,
                         created_ns INT, disabled INT NOT NULL DEFAULT 0);
CREATE TABLE entitlement(token_sha256 BLOB, db_id TEXT, mode TEXT,
                         PRIMARY KEY(token_sha256, db_id));
CREATE TABLE kv         (key TEXT PRIMARY KEY, value BLOB);
```

Closed value domains, enforced by `CHECK` constraints so a bad row cannot be
written:

- `db_source.kind` and `realm.kind`: `'surface_db'` only in v1. Spec 3 adds
  `'run_archive'`.
- `entitlement.mode`: `'read'` only in v1. Write-capable modes arrive with spec
  3's job submission.
- `kv` keys in v1: `server_uuid`, `catalog_snapshot_ns`.

The connection opens `:memory:` as `main` and attaches the state file as
`state`. `PRAGMA journal_mode = WAL` and a busy timeout are set on the attached
state database; `main`, being in-memory, has no journal mode to configure.

Spec 3 adds `job` and `job_event` to `state` under the same discipline.

### 5.2 Why the catalog earns its place

The `surface` table is the (date × symbol) coverage matrix. Answering "which
days have a fitted SPY surface" today requires sweeping every `.atxvsa` index
in the realm. From the catalog it is one indexed query. The same holds for
"symbols configured but never populated", a data-quality question the
surface-database work keeps running into. FTS5 (already compiled in) backs
symbol and tag search without a new dependency.

### 5.3 Refresh — the cache/catchup analogue

A `CatalogRefresher` thread polls each open `SurfaceDb::generation()` on a
configurable cadence. Reading a generation is one manifest-header read. On
drift it calls `SurfaceDb::refresh()`, re-indexes **only that `db_id`** inside a
single `Transaction`, and bumps `db_source.generation`. That value is exactly
what every `ResponseMeta.db_generation` reports, so client-side revalidation is
correct by construction.

### 5.4 Warm start

On clean shutdown, `main` is snapshotted to `catalog_snapshot.db` via
`Database::backup_to`. On start the snapshot is restored, then each realm
database's on-disk generation is compared against the snapshot's; only drifted
databases are rescanned. With no snapshot present, the server performs a full
scan.

Both paths must produce identical catalog contents, with one documented
exception: `db_source.scanned_ns` records when a row was indexed and therefore
differs between a warm start and a cold scan. The equality assertion in §11
excludes that column and compares every other column of every `main` table.

### 5.5 Token storage

Only `sha256(token)` is stored — never the token itself. Comparison is
constant-time. `entitlement` is the `db_id` gate that `ListDatabases` and every
data RPC consults.

## 6. Threading

`SQLITE_THREADSAFE=2` means no single connection may be used *simultaneously*
by two threads. `atx-core/include/atx/core/db/sqlite.hpp` (lines 22–27) states a
stricter house rule: a single `Database` must not be shared across threads at
all.

That house rule is incompatible with a shared in-memory catalog — a `:memory:`
database is private to its connection, so per-thread connections would yield N
separate databases rather than one shared index.

**Decision: a single `Database` guarded by a `std::mutex`, and amend the
`sqlite.hpp` threading note to state SQLite's actual contract ("not
simultaneously") alongside the per-thread-connection recommendation.**

This is sound because catalog queries are microsecond-scale index lookups
against an in-memory database, and every expensive operation — ATXVSA decode,
curve evaluation, blob read — happens **outside** the lock, after the catalog
has returned identifiers. The rejected alternative (a dedicated catalog thread
fed by a bounded queue) honours the house rule literally but buys no additional
safety, while adding a wakeup hop and a per-call cancellation/deadline story.

The documented escape hatch, if measurement later shows catalog contention: move
`main` to a WAL-mode file database and give each server thread its own
connection. No API change is required for that migration.

## 7. Service surface

```protobuf
service AdminService {
  rpc Health(HealthRequest) returns (HealthResponse);
  rpc GetServerInfo(GetServerInfoRequest) returns (ServerInfo);
  rpc GetStats(GetStatsRequest) returns (ServerStats);
  rpc RegisterDatabase(RegisterDatabaseRequest) returns (RegisterDatabaseResponse);
  rpc ExportRealm(ExportRealmRequest) returns (RealmConfig);
}

service SurfaceService {
  rpc ListDatabases(ListDatabasesRequest)   returns (ListDatabasesResponse);
  rpc ListSymbols(ListSymbolsRequest)       returns (ListSymbolsResponse);
  rpc ListPartitions(ListPartitionsRequest) returns (ListPartitionsResponse);
  rpc ListSurfaces(ListSurfacesRequest)     returns (ListSurfacesResponse);
  rpc GetSymbolConfig(GetSymbolConfigRequest) returns (GetSymbolConfigResponse);
  rpc GetSurfaceMeta(GetSurfaceMetaRequest) returns (SurfaceMeta);
  rpc GetCurve(GetCurveRequest)             returns (VolCurveSlice);
  rpc GetSurfaceBlob(GetSurfaceBlobRequest) returns (SurfaceBlob);
}
```

All RPCs are unary. Streaming subscriptions are deferred (§10).

`ServerInfo` reports version, uptime, realm id, the manifest schema hash, and
the ATXVSA schema hash. `ServerStats` reports per-database partition-cache
residency and capacity (`SurfaceDb::partition_cache_stats`) plus request
counters. `ListSurfaces` is the coverage query — filter by symbol, by partition
key range, or both — and is served entirely from the catalog.

**Hybrid payload model.** `GetSurfaceMeta` and `GetCurve` return decoded proto
view models and serve any client in any language. `GetSurfaceBlob` returns the
raw ATXVSA bytes for **one symbol's surface** — never a whole partition — for
clients that link `atx-vol` and want local re-evaluation without a round trip
per axis change or strike expansion. The blob response carries
`atxvsa_schema_hash` and `content_hash`, so a client built against a different
archive layout receives `FAILED_PRECONDITION` rather than mis-reading bytes.
This is the same schema-hash discipline `surface_db.hpp` and
`surface_archive.hpp` already enforce on disk.

The server enforces `max_blob_bytes` (default 16 MiB, beneath a 64 MiB
`max_send_message_size`) and returns `RESOURCE_EXHAUSTED` carrying the actual
size rather than truncating. Chunked blob transfer is deferred with the
streaming spec.

Every response embeds
`ResponseMeta{db_generation, content_hash, server_ns}`.

## 8. Error model and security

`atx::vol::Result<T>` maps to `grpc::Status` through one table in
`service_error.hpp`:

| domain error | gRPC status | note |
|---|---|---|
| `NotFound` | `NOT_FOUND` | |
| `InvalidArgument` | `INVALID_ARGUMENT` | |
| `ParseError` | `DATA_LOSS` | corrupt archive/manifest: CRC or schema-hash failure |
| `IoError` | `UNAVAILABLE` | retryable; client backs off |
| unentitled `db_id` | `PERMISSION_DENIED` | not `NOT_FOUND` — realm ids are not secrets, and masking makes misconfiguration undebuggable |
| unknown or disabled token | `UNAUTHENTICATED` | |
| blob over `max_blob_bytes` | `RESOURCE_EXHAUSTED` | carries the actual byte size |
| client archive schema mismatch | `FAILED_PRECONDITION` | carries both hashes |

Every handler is wrapped so no C++ exception escapes into gRPC. The catch-all
returns `INTERNAL` with a generated incident id; the exception text is written
to the server log only and never to the wire.

**Security posture (v1 default).** Bind `127.0.0.1:50051` with insecure channel
credentials. Token metadata is **required** on every RPC and is checked against
`state.token`. Binding a non-loopback address requires both an explicit
`--listen` and configured TLS (`grpc::SslServerCredentials`); otherwise the
server refuses to start rather than silently exposing an unauthenticated data
service on the network. `--realm-import` and token administration are
loopback-only operations in v1.

## 9. Build and dependency wiring

- `vcpkg.json` gains `grpc` and `protobuf`, pinned by the existing
  `builtin-baseline` (`9e9398f90a6c386bbd6ed89714ddb036b2e969eb`). This matches the policy already used for
  `arrow` and `gtest`, and reuses the vcpkg binary cache, so the cost is one
  cold build and near-zero afterwards across worktrees. `protoc` and
  `grpc_cpp_plugin` come from the vcpkg tree; the unpinned chocolatey `protoc`
  on `PATH` is not used.
- Root `CMakeLists.txt` gains
  `option(ATX_BUILD_SERVER "Build the ATX gRPC server" OFF)`, gating
  `add_subdirectory(atx-proto)` and `add_subdirectory(atx-server)`. An
  `atx-vol` developer never builds gRPC.
- `atx-server` links `atx::vol`, `atx::core`, `atx-proto`, `gRPC::grpc++`,
  `protobuf::libprotobuf`, and `atx_warnings`. Generated protobuf sources are
  excluded from the `/W4 /WX` gate via the `atx_sqlite3` precedent (their own
  target, warnings suppressed).
- A `server` CMake preset configures with `ATX_BUILD_SERVER=ON`.

## 10. Deliberately deferred

- **Streaming subscriptions** (`SubscribeSurface`), the subscription registry,
  per-client bounded queues, conflation, and snapshot-then-delta. Own spec.
- **A read-only SQL gateway RPC** (`AdminService.Query`) — the literal SRSE
  surface. Real value for research clients, but it is an injection and
  unbounded-DoS surface needing a statement allowlist and a query-planner
  budget. Own spec.
- **Chunked blob transfer** for surfaces exceeding `max_blob_bytes`.
- **Cache-cleanup service**: v1 relies on the existing LRU bound.
- **FIX order entry** and any order-capable path.

## 11. Testing

| Test | Proves |
|---|---|
| Encoder unit tests — pure functions, no server, no socket | domain-to-proto fidelity against existing `surface_db_test` fixtures |
| Catalog schema and migration test | `schema_version` upgrade path, foreign-key enforcement, `DQS=0` compliance |
| Catalog rebuild determinism | scanning a fixture realm twice yields identical row sets; warm-start-from-snapshot equals a cold full scan on every `main` column except `db_source.scanned_ns` (§5.4) |
| Blob fidelity | `GetSurfaceBlob` bytes → `SurfaceArchiveV2` → `PricedSurfaceView` → `fair_value` is bit-identical to the same query run locally against the `SurfaceDb` |
| Service tests over `InProcessChannel` | the full RPC path against a temporary database built by `surface_db_populate`, with no port binding and therefore no flake |
| Entitlement tests | a token without the `db_id` gets `PERMISSION_DENIED`; a disabled token gets `UNAUTHENTICATED`; `ListDatabases` returns only entitled ids |
| Concurrency test | N threads issuing `GetCurve` while a writer bumps the generation: no torn reads, `db_generation` monotone, catalog re-index atomic |
| Non-loopback refusal test | `--listen 0.0.0.0:50051` without TLS fails startup with a non-zero exit code |
| `atx-server-cli` smoke | a real client process performs connect → `ListDatabases` → `ListSurfaces` → `GetCurve` → `GetSurfaceBlob` against a live server |

Tests are labelled `atx_server` and are built only under `ATX_BUILD_SERVER=ON`.

## 12. Acceptance criteria

1. `cmake --preset server && cmake --build --preset server` succeeds from a
   clean worktree, and a default `cmake --preset dev` build is unaffected — no
   gRPC or protobuf compiled.
2. `atx-server --realm-import realm.json` registers a surface database and the
   process starts bound to `127.0.0.1:50051`.
3. `atx-server-cli surfaces --db <id> --symbol SPY` returns the coverage list
   for a populated fixture database.
4. `atx-server-cli curve --db <id> --key <date> --symbol SPY --expiry <iso>`
   returns a curve whose points match a locally computed curve over the same
   `SurfaceDb`.
5. The blob-fidelity test passes bit-identically.
6. `ctest -L atx_server` is green.
7. Starting with `--listen 0.0.0.0:50051` and no TLS exits non-zero with a
   diagnostic naming the missing TLS configuration.
8. Killing and restarting the server restores the catalog from its snapshot and
   produces a catalog identical to a cold full scan.
