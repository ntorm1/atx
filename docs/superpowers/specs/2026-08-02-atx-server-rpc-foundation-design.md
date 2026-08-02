# atx-server — RPC Foundation and Surface Read Path — Design

Date: 2026-08-02
Status: Approved — ready for implementation planning
Scope: new `atx-proto/`, `atx-rpc/`, `atx-server/` subprojects. No `atx-ui` change
in this spec.
Supersedes: `2026-07-26-atx-server-surface-service-design.md` (gRPC transport)

## 1. Goal

Stand up `atx-server`: the central, long-running C++ process that is the backbone
of the ATX American-equity option trading platform. It accepts many concurrent
client connections and answers data requests against the `atx-vol` surface
database. Clients never touch the filesystem; the server is the only process that
knows where data lives.

This is **spec 1 of 2** in the current tranche:

| # | Spec | Deliverable | Depends on |
|---|---|---|---|
| **1** | **atx-proto + atx-rpc + atx-server** (this document) | message contract, transport library, server runtime (realm / catalog / registry / encoders / auth), `SurfaceService` + `AdminService`, `atx-server-cli`, full test gate | — |
| 2 | `atx-ui` hub-and-spoke shell rework | `2026-08-02-atx-ui-hub-spoke-shell-design.md` | 1 |

A later tranche adds `BacktestService` (async job submission, run-archive
catalog) and streaming subscriptions. Both are deliberately excluded here; see
§11.

Spec 2 does not begin until spec 1's acceptance gate (§12) passes. Building a UI
shell on an unproven transport means debugging two new layers simultaneously.

## 2. Decision record

Four decisions were taken during design and are recorded here because each one
constrains everything downstream.

### 2.1 Transport: framed protobuf over raw TCP, not gRPC

gRPC is not installed. The prior vcpkg build ran out of memory and was never
completed; `C:/atx-cache/vcpkg_installed/x64-windows/lib` contains zero gRPC
libraries and there is no `grpc_cpp_plugin.exe`. Protobuf, abseil, re2, and
c-ares *are* installed, and `protoc.exe` is in the vcpkg tree.

**Decision: hand-rolled length-prefixed protobuf framing over TCP.**

The cost is explicit and accepted: this project now owns framing, deframing,
partial-read handling, request routing, deadline plumbing, connection limits, an
error-status envelope, and — if remote access is ever wanted — TLS. gRPC would
have supplied all of that. The compensating benefits are that no dependency
build is required, and the wire is inspectable in a hex dump.

Mitigation of the migration risk: the status code set, the error mapping, and the
handler signature are all modelled directly on gRPC's, so a later swap is
mechanical rather than a semantic redesign. §4.3 and §8 are written with that
constraint in mind.

Consequence for `vcpkg.json`: `grpc` is **removed**. Nothing links it, and
leaving it in the manifest forces a ~45-minute cold build on every fresh
worktree configure for zero benefit. `protobuf` stays. Re-add `grpc` with the
spec that actually adopts it.

### 2.2 Concurrency: poller thread + bounded worker pool

A plain accept-then-hand-to-pool design starves: a worker blocked in `recv()` on
an idle connection is a dead worker, so N idle clients kill a pool of N.

**Decision: a dedicated readiness poller feeds a bounded worker pool.** Details
in §6. This is ~120 lines more than naive thread-per-connection and avoids both
the starvation bug and the unbounded-thread failure mode, without introducing an
async state machine.

### 2.3 Bind: loopback only, no exceptions in v1

There is no TLS in a raw-TCP build. A non-loopback bind would put plaintext auth
tokens and unencrypted market data on the network.

**Decision: the server refuses to start if `--listen` names any non-loopback
address.** Non-zero exit, diagnostic naming the reason. Remote access is a later
spec that must bring transport encryption with it. This is a hard gate, not a
warning.

### 2.4 Server lifecycle: separately launched daemon, no auto-spawn

`atx-server` is always started independently. Clients never spawn it. The UI
treats "not connected" as a first-class state (spec 2, §C2). This keeps exactly
one data path and no child-process lifetime code.

## 3. Current state

**What exists.**

- `atx-vol/include/atx/vol/surface_db.hpp` — `SurfaceDb` is a directory:
  `manifest.atxdb` (symbol table + partition index, CRC-validated, generation
  stamped, atomic rewrite) over `partitions/*.atxvsa` (ATXVSA v2 archives).
  `map_surface` gives a zero-copy `PricedSurfaceView` over an LRU-bounded
  partition mmap cache; `load_surface` gives an owned `PricedSurface`;
  `refresh()` picks up an external writer's generation bump. Const queries are
  documented thread-safe over an immutable manifest snapshot
  (`surface_db.hpp:528-541`).
- `atx-vol/include/atx/vol/surface_archive.hpp` —
  `write_surface_archive_v2(std::span<const SurfaceArchiveItem>, ArchiveV2WriteOpts)`
  returns `Result<std::vector<std::byte>>` (line 652), and
  `SurfaceArchiveV2::open(std::vector<std::byte>)` / `map_symbol` (lines 682, 744)
  invert it. This pair is what makes §7.3's blob path possible.
- `atx-core/include/atx/core/db/sqlite.hpp` + `blob.hpp` — RAII `Database` /
  `Statement` / `Transaction` / `BlobStream` over vendored SQLite
  (`atx_sqlite3`), built with `SQLITE_THREADSAFE=2`, `SQLITE_DQS=0`,
  `SQLITE_DEFAULT_FOREIGN_KEYS=1`, `SQLITE_ENABLE_FTS5`. `prepare_cached` returns
  address-stable prepared statements; `Database::backup_to` performs an online
  database-to-database copy.
- `atx-core/include/atx/core/sha256.hpp` — for token digests.
- `atx-ui/` — a single-workspace desktop app, 2117 lines. `main.cpp` hard-wires
  one `OpraVolSurface` (parquet load + in-process fit) into one `VolWorkspace`
  with six fixed panels. `VolSurfaceSource`
  (`atx-ui/include/atx/ui/vol_surface_source.hpp:105`) is the only abstraction
  seam.

**What is missing.** No server process, no wire contract, no multi-client story,
and no way to ask a cross-cutting question ("which days have a fitted SPY
surface?") without sweeping every archive index on disk. Every consumer today
opens the database files directly.

**Server-socket infrastructure.** None. The vendored `databento-cpp` has a TCP
*client* (`third-party/databento-cpp/src/detail/tcp_client.cpp`) but no listener.
The build is MSVC/clang-cl on Windows with no CI workflows, so Winsock2 is the
only backend that must work; the shim in §4.2 keeps a BSD-sockets path compiling
without committing to testing it.

## 4. Architecture

### 4.1 Decomposition — four units

```
atx-proto/    message contract only. protoc codegen -> static lib. No transport.
atx-rpc/      transport. socket shim, framing, envelope, Client, Dispatcher,
              ServiceRegistry. Shared by the server AND every client.
atx-server/   realm, catalog, surface registry, encoders, service impls, daemon.
atx-ui/       spec 2. Links atx-rpc + atx-proto.
```

`atx-rpc` is the load-bearing split. Without it, framing is written twice — once
in the server, once in the UI client — and the two drift. With it, a future
Python or web client is the only thing that needs new code.

`atx-server` links `atx::vol`. `atx-ui` deliberately does not, with one narrow
exception: it links the `atx-vol` archive-decode path so it can read pulled
binary surfaces (§7.3). That single dependency is explicit and bounded.

### 4.2 `atx-rpc/` — the transport library

```
atx-rpc/
  CMakeLists.txt
  include/atx/rpc/
    socket.hpp        Winsock2/BSD shim: Socket, Listener, WsaScope
    frame.hpp         read_frame / write_frame, max_frame_bytes
    method_table.hpp  typed registration: add<Req,Resp>(name, fn)
    service.hpp       Service interface
    dispatcher.hpp    Dispatcher: envelope -> MethodTable -> envelope
    server.hpp        RpcServer: acceptor + poller + worker pool + limits
    client.hpp        RpcClient: connect, handshake, call<Req,Resp>, reconnect
  src/                one .cpp per header
  tests/
```

`socket.hpp` is a thin RAII wrapper — `Socket`, `Listener`, and a `WsaScope`
that performs `WSAStartup`/`WSACleanup` once per process. It is not an
abstraction layer over I/O models; it exists so that `#ifdef _WIN32` appears in
exactly one file.

`frame.hpp` and `dispatcher.hpp` are pure and take no socket, so both are
directly unit-testable and fuzzable.

### 4.3 Wire protocol

**Framing.** Fixed 8-byte prefix, then one protobuf envelope. Nothing is
hand-parsed past the prefix.

```
[u32 magic = 0x41545852 'ATXR'][u32 payload_len]   <- 8 bytes, little-endian
[payload_len bytes: atx.rpc.v1.Envelope]
```

The magic catches a client pointed at the wrong port on byte 0, rather than
after a confusing protobuf parse failure. `payload_len` is validated against
`max_frame_bytes` (default 64 MiB) **before any allocation** — unbounded
allocation from an attacker-controlled length is the classic hand-rolled-framer
defect and is closed by construction here.

**Envelope.**

```protobuf
// atx/rpc/v1/envelope.proto
package atx.rpc.v1;

enum Code {
  CODE_OK                 = 0;
  CODE_NOT_FOUND          = 1;
  CODE_INVALID_ARGUMENT   = 2;
  CODE_PERMISSION_DENIED  = 3;
  CODE_UNAUTHENTICATED    = 4;
  CODE_RESOURCE_EXHAUSTED = 5;
  CODE_FAILED_PRECONDITION= 6;
  CODE_UNAVAILABLE        = 7;
  CODE_DATA_LOSS          = 8;
  CODE_DEADLINE_EXCEEDED  = 9;
  CODE_UNIMPLEMENTED      = 10;
  CODE_BUSY               = 11;
  CODE_INTERNAL           = 12;
}

message Status {
  Code   code        = 1;
  string message     = 2;
  string incident_id = 3;  // set only on CODE_INTERNAL; correlates to server log
}

message Envelope {
  uint32 protocol_version = 1;  // validated at handshake, not per call
  uint64 correlation_id   = 2;  // echoed on the response
  bool   is_response      = 3;
  string method           = 4;  // "atx.rpc.v1.SurfaceService/GetCurve"
  bytes  payload          = 5;  // serialized request or response message
  Status status           = 6;  // response only
  bytes  auth_token       = 7;  // request only
  int64  deadline_unix_ns = 8;  // request only; 0 = none
}
```

`Code` is deliberately gRPC's status set. The error table in §8 therefore
transfers unchanged if gRPC is later adopted.

`method` is a string rather than an integer id: debuggable in a hex dump, no
registry to keep synchronised between client and server, and the hash lookup is
free next to an ATXVSA decode.

**Deadlines, stated honestly.** `deadline_unix_ns` is checked at dispatch and at
cooperative checkpoints inside long handlers. A handler already inside a decode
is **not** preempted. This is a weaker guarantee than gRPC cancellation and is
documented as such rather than implied.

**Correlation ids under serial connections.** v1 processes requests on one
connection strictly serially, so ordering alone would suffice for matching. The
field is present anyway so that pipelining or multiplexing can be added later
without a wire break.

**Handshake.** The first frame in each direction is `Hello` / `HelloAck`,
carrying `protocol_version`, the server build id, and the ATXVSA schema hash. A
version mismatch closes the connection with a `Status` naming both versions —
not a silent hang, and not a garbled decode 200 frames later.

### 4.4 Service registration

```cpp
class Service {
public:
  virtual ~Service() = default;
  [[nodiscard]] virtual std::string_view name() const noexcept = 0;
  virtual void register_methods(MethodTable &t) = 0;
};

// MethodTable owns decode/encode; handlers never see bytes or envelopes.
template <class Req, class Resp>
void MethodTable::add(std::string_view method,
                      std::function<Status(const CallContext &, const Req &, Resp &)> fn);
```

`CallContext` carries the resolved entitlements, the deadline, the peer
description, and the incident-id sink.

This is the concrete content of the "modular and scaleable" requirement: adding
`BacktestService` later is one class plus one registration line, with no change
to transport, framing, or dispatch.

### 4.5 `atx-proto/` — the message contract

```
atx-proto/
  CMakeLists.txt
  atx/rpc/v1/
    envelope.proto         Envelope, Status, Code, Hello, HelloAck
    keys.proto             SymbolKey, ExpiryKey, OptionKey, PartitionKey, SurfaceKey
    common.proto           ResponseMeta, Page, IsoDate, ErrorDetail
    surface.proto          SurfaceMeta, ExpirySummary, VolCurveSlice, VolQuotePoint,
                           SurfaceDiagnostics, SymbolFitConfig, SurfaceBlob
    surface_service.proto  SurfaceService request/response messages
    admin.proto            AdminService messages, RealmConfig, DatabaseInfo
```

Produces one static CMake target, `atx-proto`, via `protobuf_generate` using the
vcpkg `protoc`. The unpinned chocolatey `protoc` on `PATH` is not used.

There are no `service` declarations in the `.proto` files — with no gRPC plugin,
they would generate nothing. Method names are string constants in
`surface_service.proto`'s leading comment and in `atx-server`'s registration
code; §12.9 requires a test asserting the two agree.

`RealmConfig` doubles as the realm import/export format (§5), so the server needs
no JSON library beyond `google::protobuf::util::JsonStringToMessage`. This
design adds no JSON dependency.

### 4.6 `atx-server/`

```
atx-server/
  CMakeLists.txt
  include/atx/server/
    config.hpp            ServerConfig: listen address, limits, state path
    realm.hpp             Realm: db_id -> {kind, root}; the only path holder
    catalog.hpp           Catalog: SQLite in-memory index + on-disk state (§6)
    catalog_refresh.hpp   CatalogRefresher: generation-drift poller
    surface_registry.hpp  lazily-opened SurfaceDb per db_id
    encode.hpp            atx::vol domain -> proto. Pure functions, no transport.
    auth.hpp              token digest -> Entitlements
    service_error.hpp     atx::vol::Result -> Status mapping (§8)
    service_surface.hpp   SurfaceServiceImpl
    service_admin.hpp     AdminServiceImpl
    server.hpp            Server: build / start / wait / shutdown
  src/                    one .cpp per header
  tools/main.cpp          the atx-server binary
  tools/cli.cpp           the atx-server-cli test/ops client
  tests/
```

`encode.hpp` is the load-bearing boundary. Every domain-to-wire translation is a
free function testable without a server, a socket, or a port. It derives view
models (expiry summaries, curve slices, quote points, diagnostics) from `atx-vol`
domain types directly.

This logic currently lives in `atx-ui/src/vol/spy_opra_surface.cpp`. Spec 2
deletes the UI-side copy. Until spec 2 lands, the two exist in parallel; that
duplication is scoped, temporary, and called out here so it is not mistaken for
an oversight.

### 4.7 Realm

`Realm` is the set of databases the server serves: `db_id -> {kind, root}`. It
lives in the catalog's on-disk `state.realm` table (§6), not a config file.
Databases are registered through `AdminService.RegisterDatabase`. `RealmConfig`
(proto) is the import/export format: `atx-server --realm-import realm.json` on
first run, `AdminService.ExportRealm` for inspection and backup.

**A `db_id` is an opaque client-facing identifier. Filesystem roots never leave
the process.** This is the inherited hard rule from
`atx-ui/docs/platform-architecture.md`: no filesystem path ever crosses the wire.
Clients address data as `(db_id, partition_key, symbol)`. The realm is the only
place paths exist, and it is also the entitlement boundary.

## 5. Catalog — the coverage index

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

- `db_source.kind` and `realm.kind`: `'surface_db'` only in v1. The backtest
  tranche adds `'run_archive'`.
- `entitlement.mode`: `'read'` only in v1. Write-capable modes arrive with job
  submission.
- `kv` keys in v1: `server_uuid`, `catalog_snapshot_ns`.

The connection opens `:memory:` as `main` and attaches the state file as `state`.
`PRAGMA journal_mode = WAL` and a busy timeout are set on the attached state
database; `main`, being in-memory, has no journal mode to configure.

### 5.2 Why the catalog earns its place

The `surface` table is the (partition × symbol) coverage matrix. Answering "which
days have a fitted SPY surface" today requires sweeping every `.atxvsa` index in
the realm. From the catalog it is one indexed query. The same holds for "symbols
configured but never populated" — a data-quality question the surface-database
work keeps running into, and the one that spec 2's coverage-matrix panel renders.
FTS5 is already compiled into the vendored SQLite, so symbol and tag search costs
no new dependency.

### 5.3 Refresh

A `CatalogRefresher` thread polls each open `SurfaceDb::generation()` on a
configurable cadence. Reading a generation is one manifest-header read. On drift
it calls `SurfaceDb::refresh()`, re-indexes **only that `db_id`** inside a single
`Transaction`, and bumps `db_source.generation`. That value is exactly what every
`ResponseMeta.db_generation` reports, so client-side revalidation is correct by
construction.

### 5.4 Warm start

On clean shutdown, `main` is snapshotted to `catalog_snapshot.db` via
`Database::backup_to`. On start the snapshot is restored, then each realm
database's on-disk generation is compared against the snapshot's; only drifted
databases are rescanned. With no snapshot present, the server performs a full
scan.

Both paths must produce identical catalog contents, with one documented
exception: `db_source.scanned_ns` records when a row was indexed and therefore
differs between a warm start and a cold scan. The equality assertion in §12
excludes that column and compares every other column of every `main` table.

### 5.5 Token storage

Only `sha256(token)` is stored — never the token itself. Comparison is
constant-time. `entitlement` is the `db_id` gate that `ListDatabases` and every
data RPC consults.

## 6. Threading

### 6.1 Connection handling

```
accept thread ──> enforces max_connections, registers conn with the poller
poller thread ──> WSAPoll/poll over all idle conns; on readable, pushes to the
                  bounded ready queue and marks the conn in-flight so it is not
                  re-dispatched
worker 0..N   ──> pops a conn, blocking-reads one full frame, dispatches, writes
                  the response, clears in-flight, returns the conn to the poller
```

Reads stay blocking, but only on a socket already known to be readable, so an
idle connection never occupies a worker. No async state machine is required.

A connection that becomes readable and then stalls mid-frame (slowloris) is
bounded by `SO_RCVTIMEO` / `SO_SNDTIMEO`; a partial-frame timeout closes the
connection. The ready queue is bounded; overflow is answered with `CODE_BUSY`
rather than unbounded growth.

`N` defaults to `std::thread::hardware_concurrency()`. `SurfaceDb` const queries
are already thread-safe over an immutable manifest snapshot, so surface reads fan
out with no added locking.

### 6.2 Catalog locking

`SQLITE_THREADSAFE=2` means no single connection may be used *simultaneously* by
two threads. `atx-core/include/atx/core/db/sqlite.hpp` (lines 22–27) states a
stricter house rule: a single `Database` must not be shared across threads at
all.

That house rule is incompatible with a shared in-memory catalog — a `:memory:`
database is private to its connection, so per-thread connections would yield N
separate databases rather than one shared index.

**Decision: a single `Database` guarded by a `std::mutex`, and amend the
`sqlite.hpp` threading note to state SQLite's actual contract ("not
simultaneously") alongside the per-thread-connection recommendation.**

This is sound because catalog queries are microsecond-scale index lookups against
an in-memory database, and every expensive operation — ATXVSA decode, curve
evaluation, blob encode — happens **outside** the lock, after the catalog has
returned identifiers. The rejected alternative (a dedicated catalog thread fed by
a bounded queue) honours the house rule literally but buys no additional safety,
while adding a wakeup hop and a per-call cancellation story.

The documented escape hatch, if measurement later shows catalog contention: move
`main` to a WAL-mode file database and give each worker its own connection. No
API change is required for that migration.

## 7. Service surface

All calls are unary request/response. Streaming is deferred (§11).

```
AdminService
  Health            (HealthRequest)            -> HealthResponse
  GetServerInfo     (GetServerInfoRequest)     -> ServerInfo
  GetStats          (GetStatsRequest)          -> ServerStats
  RegisterDatabase  (RegisterDatabaseRequest)  -> RegisterDatabaseResponse
  ExportRealm       (ExportRealmRequest)       -> RealmConfig

SurfaceService
  ListDatabases     (ListDatabasesRequest)     -> ListDatabasesResponse
  ListSymbols       (ListSymbolsRequest)       -> ListSymbolsResponse
  ListPartitions    (ListPartitionsRequest)    -> ListPartitionsResponse
  ListSurfaces      (ListSurfacesRequest)      -> ListSurfacesResponse
  GetCoverage       (GetCoverageRequest)       -> GetCoverageResponse
  GetSymbolConfig   (GetSymbolConfigRequest)   -> GetSymbolConfigResponse
  GetSurfaceMeta    (GetSurfaceMetaRequest)    -> SurfaceMeta
  GetCurve          (GetCurveRequest)          -> VolCurveSlice
  GetSurfaceBlob    (GetSurfaceBlobRequest)    -> SurfaceBlob
```

### 7.1 Admin

`ServerInfo` reports version, uptime, realm id, the manifest schema hash, and the
ATXVSA schema hash. `ServerStats` reports per-database partition-cache residency
and capacity (`SurfaceDb::partition_cache_stats`), connection counts, ready-queue
depth, and request counters.

### 7.2 Coverage

`ListSurfaces` filters by symbol, by partition-key range, or both, and is served
entirely from the catalog. `GetCoverage` returns the (partition × symbol) matrix
for a symbol set and key range, with a per-cell state of
`FITTED | CONFIGURED_NOT_FITTED | ABSENT`. It is the query spec 2's coverage panel
renders and the reason the catalog exists.

### 7.3 Hybrid payload model

`GetSurfaceMeta` and `GetCurve` return decoded proto view models and serve any
client in any language.

`GetSurfaceBlob` returns a **single-symbol ATXVSA v2 archive** for clients that
link `atx-vol` and want local re-evaluation without a round trip per axis change
or strike expansion. Construction, verified against the real API:

```
SurfaceDb::load_surface(db_id, partition_key, symbol)   // Result<PricedSurface>
  -> SurfaceArchiveItem
  -> write_surface_archive_v2(items, opts)              // Result<std::vector<std::byte>>
```

The client inverts it with `SurfaceArchiveV2::open(bytes)` → `map_symbol` →
`PricedSurfaceView`, then evaluates locally.

This costs one decode plus one encode per request. v1 accepts that and caches the
encoded bytes by `content_hash`. Zero-copy slicing of the partition's directory
entry is a later optimisation, not a v1 requirement.

Never a whole partition. The response carries `atxvsa_schema_hash` and
`content_hash`, so a client built against a different archive layout receives
`CODE_FAILED_PRECONDITION` carrying both hashes rather than mis-reading bytes.
This is the same schema-hash discipline `surface_db.hpp` and
`surface_archive.hpp` already enforce on disk.

The server enforces `max_blob_bytes` (default 16 MiB, beneath the 64 MiB
`max_frame_bytes`) and returns `CODE_RESOURCE_EXHAUSTED` carrying the actual size
rather than truncating. Chunked transfer is deferred with the streaming spec.

Every response embeds `ResponseMeta{db_generation, content_hash, server_ns}` so
clients revalidate rather than refetch.

## 8. Error model

`atx::vol::Result<T>` maps to `Status` through one table in `service_error.hpp`:

| domain error | code | note |
|---|---|---|
| `NotFound` | `CODE_NOT_FOUND` | |
| `InvalidArgument` | `CODE_INVALID_ARGUMENT` | |
| `ParseError` | `CODE_DATA_LOSS` | corrupt archive/manifest: CRC or schema-hash failure |
| `IoError` | `CODE_UNAVAILABLE` | retryable; client backs off |
| unentitled `db_id` | `CODE_PERMISSION_DENIED` | not `NOT_FOUND` — realm ids are not secrets, and masking makes misconfiguration undebuggable |
| unknown or disabled token | `CODE_UNAUTHENTICATED` | |
| blob over `max_blob_bytes` | `CODE_RESOURCE_EXHAUSTED` | carries the actual byte size |
| client archive schema mismatch | `CODE_FAILED_PRECONDITION` | carries both hashes |
| unknown `method` string | `CODE_UNIMPLEMENTED` | |
| ready queue full | `CODE_BUSY` | client retries with backoff |
| deadline passed at dispatch | `CODE_DEADLINE_EXCEEDED` | |

Every handler is wrapped so no C++ exception escapes into the dispatcher. The
catch-all returns `CODE_INTERNAL` with a generated incident id; the exception
text is written to the server log only and never to the wire.

## 9. Security posture

- **Bind is loopback-only.** `--listen` with any non-loopback address fails
  startup with a non-zero exit and a diagnostic naming the reason. There is no
  TLS in this build; a remote bind would ship plaintext tokens and unencrypted
  market data. This is a hard gate (§2.3).
- Token metadata is **required** on every request and checked against
  `state.token`. Only `sha256(token)` is stored; comparison is constant-time.
- No filesystem path crosses the wire in either direction.
- `--realm-import` and token administration are loopback-only operations, which
  in v1 is every operation.
- `payload_len` is bounds-checked before allocation; `max_connections`,
  `max_frame_bytes`, `max_blob_bytes`, and the ready-queue depth are all
  configured limits with defaults, not unbounded.

## 10. Build and dependency wiring

- `vcpkg.json`: **remove `grpc`**, keep `protobuf`. Nothing links gRPC, and
  leaving it forces a ~45-minute cold build on every fresh worktree configure.
- Root `CMakeLists.txt` gains
  `option(ATX_BUILD_SERVER "Build the ATX server" OFF)`. `atx-proto` and
  `atx-rpc` are added when `ATX_BUILD_SERVER` **or** the UI option is on;
  `atx-server` is added under `ATX_BUILD_SERVER` alone. An `atx-vol` developer
  builds none of it.
- `atx-rpc` links `atx::core`, `atx-proto`, `protobuf::libprotobuf`,
  `atx_warnings`, and (on Windows) `ws2_32`.
- `atx-server` links `atx::vol`, `atx::core`, `atx-rpc`, `atx-proto`,
  `atx_warnings`.
- Generated protobuf sources live in their own target with warnings suppressed
  (the `atx_sqlite3` precedent), so `/W4 /WX` still gates every hand-written
  file.
- A `server` CMake preset configures with `ATX_BUILD_SERVER=ON`.
- `protoc` comes from the vcpkg tree
  (`C:/atx-cache/vcpkg_installed/x64-windows/tools/protobuf/protoc.exe`). The
  unpinned chocolatey `protoc` on `PATH` is not used.

## 11. Deliberately deferred

- **Streaming subscriptions** (`SubscribeSurface`), the subscription registry,
  per-client bounded queues, conflation, and snapshot-then-delta. Own spec. This
  is also the point at which the §6.1 poller design should be re-evaluated
  against IOCP/epoll, because many mostly-idle long-lived connections is exactly
  the workload the current design is weakest at.
- **TLS and remote binding.** Must arrive together; neither is useful alone.
- **`BacktestService`** — async job submission, worker pool, job table, restart
  recovery. A distinct hazard class that must not ride in alongside the read
  path.
- **A read-only SQL gateway RPC** — real value for research clients, but an
  injection and unbounded-DoS surface needing a statement allowlist and a
  query-planner budget. Own spec.
- **Chunked blob transfer** for surfaces exceeding `max_blob_bytes`.
- **Request pipelining / multiplexing.** The wire already carries
  `correlation_id` for it.
- **Cache-cleanup service**: v1 relies on the existing `SurfaceDb` LRU bound.
- **FIX order entry** and any order-capable path.

## 12. Testing

| # | Test | Proves |
|---|---|---|
| 1 | `frame` round-trip and fuzz corpus: truncated prefix, truncated payload, `payload_len` = 0, `payload_len` > `max_frame_bytes`, bad magic, garbage protobuf | the hand-rolled framer does not over-allocate, over-read, or hang. Highest-risk code in the design. |
| 2 | Handshake version mismatch | connection closed with both versions in `Status`; no hang, no partial decode |
| 3 | Slowloris | a readable-then-stalled connection is closed by `SO_RCVTIMEO` and the worker pool is not starved |
| 4 | `max_connections` refusal and ready-queue overflow | refusal at the accept boundary and `CODE_BUSY` under overflow; no unbounded growth |
| 5 | Encoder unit tests — no server, no socket | domain-to-proto fidelity against existing `surface_db_test` fixtures |
| 6 | Catalog schema and migration | `schema_version` upgrade path, foreign-key enforcement, `DQS=0` compliance, `CHECK` domains reject bad rows |
| 7 | Catalog rebuild determinism | scanning a fixture realm twice yields identical row sets; warm-start-from-snapshot equals a cold full scan on every `main` column except `db_source.scanned_ns` (§5.4) |
| 8 | **Blob fidelity** | `GetSurfaceBlob` bytes → `SurfaceArchiveV2::open` → `map_symbol` → `fair_value` is **bit-identical** to the same query run locally against the `SurfaceDb` |
| 9 | Method-name agreement | every method string registered by a `Service` appears in the proto contract's declared method list, and vice versa (§4.5) |
| 10 | Dispatcher tests, no socket at all | the full request→response path in-process |
| 11 | Loopback service tests on an **ephemeral port** | real sockets end to end with zero port-collision flake |
| 12 | Entitlements | a token without the `db_id` gets `PERMISSION_DENIED`; a disabled token gets `UNAUTHENTICATED`; `ListDatabases` returns only entitled ids |
| 13 | Concurrency | N workers issuing `GetCurve` while a writer bumps the generation: no torn reads, `db_generation` monotone, catalog re-index atomic |
| 14 | Non-loopback refusal | `--listen 0.0.0.0:50051` exits non-zero with a diagnostic naming the missing transport encryption |
| 15 | `atx-server-cli` smoke | a real client process performs connect → handshake → `ListDatabases` → `ListSurfaces` → `GetCoverage` → `GetCurve` → `GetSurfaceBlob` against a live server |

Tests are labelled `atx_server` and are built only under `ATX_BUILD_SERVER=ON`.

## 13. Acceptance criteria

1. `cmake --preset server && cmake --build --preset server` succeeds from a clean
   worktree, and a `cmake --preset dev` build with `ATX_BUILD_SERVER=OFF` and
   `ATX_BUILD_UI=OFF` compiles no protobuf, no `atx-rpc`, and no server code.
2. `atx-server --realm-import realm.json` registers a surface database and the
   process starts bound to `127.0.0.1:50051`.
3. `atx-server --listen 0.0.0.0:50051` exits non-zero with a diagnostic naming
   the missing transport encryption.
4. `atx-server-cli surfaces --db <id> --symbol SPY` returns the coverage list for
   a populated fixture database.
5. `atx-server-cli curve --db <id> --key <date> --symbol SPY --expiry <iso>`
   returns a curve whose points match a locally computed curve over the same
   `SurfaceDb`.
6. `atx-server-cli blob --db <id> --key <date> --symbol SPY --out spy.atxvsa`
   writes bytes that `SurfaceArchiveV2::open` accepts, and test 12.8 passes
   bit-identically.
7. The framing fuzz corpus (12.1) runs clean under ASan/UBSan where available,
   and no input causes an allocation larger than `max_frame_bytes`.
8. `ctest -L atx_server` is green.
9. Killing and restarting the server restores the catalog from its snapshot and
   produces a catalog identical to a cold full scan on every column except
   `db_source.scanned_ns`.
10. `grpc` no longer appears in `vcpkg.json`.
