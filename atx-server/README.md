# ATX research service

`atx-server` is the C++20 service behind the `atx-front` docking workstation.
It replaces report-generation requests with persistent jobs and structured
research documents. Existing report tools remain available for archival exports.

The service runs the existing equity engine and `atx-impl` NAV pipeline. It does
not submit broker orders. A synthetic replay is labeled synthetic throughout the
UI; importing market artifacts does not upgrade their historical verification.

## Run on Windows

Prerequisites: the repository's clang-cl/Visual Studio toolchain, CMake, vcpkg,
Python 3.10+, and Node.js 22+. Run from `C:/atx`:

```powershell
python scripts/atx-workspace.py --build
```

The launcher builds the `server-dev` preset and the browser assets, starts both
processes, checks service readiness, and prints `http://127.0.0.1:3000`. It keeps
the SQLite database at `atx-server/var/workspace.db` and logs beside it. Ctrl+C
stops its child processes. Subsequent starts omit `--build`.
On Windows the launcher runs a temporary copy of the native executable and its
adjacent DLLs under `atx-server/var`, so subsequent builds can replace their own
outputs. The copy is removed after both children stop. `--no-native-snapshot`
opts into running the build output directly, which locks that executable.

For optimized execution, use `python scripts/atx-workspace.py --build --preset server-rel`.
This builds in `build-server-rel`; keep `--preset server-rel` on subsequent starts.
Both server presets install their optional gRPC dependencies under
`deps/vcpkg-server`, separate from the repository's base dependency installation.

To build and test each component explicitly:

```powershell
powershell scripts/atx-build.ps1 -Preset server-dev configure
powershell scripts/atx-build.ps1 -Preset server-dev -Jobs 4 build atx-server atx-server-tests atx-server-core-tests atx-server-research-tests atx-server-database-tests atx-server-access-tests
powershell scripts/atx-build.ps1 -Preset server-dev -Ctest -L atx_server
npm --prefix atx-front ci
npm --prefix atx-front run build
npm --prefix atx-front test
python scripts/test-atx-workspace.py
python scripts/test-atx-workspace.py --server build-server-rel/bin/atx-server.exe --tls
python scripts/test-atx-database.py --server build-server-rel/bin/atx-server.exe --tls
python scripts/test-atx-platform.py --server build-server-rel/bin/atx-server.exe --tls
python scripts/test-atx-team.py --server build-server-rel/bin/atx-server.exe
```

The black-box gate uses its own temporary database, launches the actual native
service and HTTP gateway, executes the real engine, and tests concurrent reads,
authentication, idempotency, scheduling/cancellation, event gaps and crash recovery.
It never opens the operator's database. `--keep` retains test evidence under
`.cache/workspace-integration-*`.
The optional `--tls` gate requires an existing OpenSSL executable, creates a
temporary localhost certificate, verifies its hostname through the gateway, and
removes its private key afterward. It does not modify the machine trust store.
The separate database gate requires Python DuckDB 1.5.x to create disposable
fixtures; it checks the actual native provider, exact values, approved Parquet
views, query restrictions, writer-lock recovery and unchanged database bytes.

For native-only operation:

```powershell
build-server/bin/atx-server.exe --db atx-server/var/workspace.db --repo-root C:/atx --artifact-root C:/atx/data --workers 2
```

Run `npm --prefix atx-front start` in another terminal. The native listener defaults
to `127.0.0.1:50051`. See [the frontend guide](../atx-front/README.md) for gateway
configuration and browser tests. The launcher accepts `--server`, `--db`,
`--artifact-root` (repeatable), `--workers`, `--port`, `--grpc-port` and `--preset`.

## Architecture and contracts

```text
React / Dockview workstation
          | same-origin HTTP + resumable SSE
Node / Fastify gateway (no research state)
          | authenticated gRPC / optional TLS
C++ WorkspaceService
          +-- Store: SQLite WAL + bounded TTL/LRU cache
          +-- Scheduler: bounded queue + cooperative jthread workers
          +-- EventBus: bounded channels + durable sequence reservations
          +-- DatabaseProvider registry: curated, read-only data sources
                 +-- DuckDB C API: atx-db catalog and approved Parquet lake
          +-- ResearchBackend
                 +-- atx-engine alpha DSL and execution replay
                 +-- atx-impl pinned NAV replay
                 +-- validated CSV/JSON artifact import
```

The source contract is [workspace.proto](proto/atx/v1/workspace.proto). Generated
C++ bindings live in the build tree; the gateway loads the same protocol file.
Lifecycle fields are typed, while research results carry a versioned JSON schema.
Wire statuses are `queued`, `running`, `succeeded`, `failed`, and `cancelled`.
A queued job can have a future `scheduled_at_ms` Unix timestamp.

### Named workspaces and research library

Named workspaces persist Dockview layout and public panel state in SQLite.
Create starts revision 1; updates and deletion require an exact expected revision.
The service rejects stale writes instead of overwriting another browser's changes.
The limit is 256 records and 96 KiB per version-1 document. Workspaces are shared
within one authenticated team project. Team roles govern reads and mutations;
private projects and tenant isolation are not implemented. Browser tokens are
never part of the workspace document.

The alpha library retains immutable recipe and metadata snapshots. Every edit,
archive or restore creates a revision with a server-computed SHA256 of canonical
submitted JSON. Canonicalization does not expand omitted strategy defaults.
Optional source-job links require exactly matching canonical recipes. The initial
shared project is `local`; stages are `research` and `archived`, without an implied
review or production approval. Limits are 2,048 alpha records, 16,384 total
revisions, 64 KiB recipes, 16 tags and 4,096-byte hypotheses.

Jobs may reference an explicit alpha ID and revision outside their research JSON.
The native service resolves the immutable recipe and retains that lineage with
the job. Experiments submit 1–32 simulations atomically, with up to 96 KiB of
canonical recipes. Queue admission and idempotency cover the complete batch.
Each experiment reports counts derived from its member jobs; cancelling a batch
also requests cooperative cancellation of running members. Limits are 2,048
experiment records and the existing shared active-job capacity.

### Team identity, review and audit

Standalone mode identifies the operator as `local` with administrator privileges.
For a shared installation, create individual credentials in a new private directory:

```powershell
python scripts/atx-team.py --directory C:/atx-private/team --team-name "Equity research" --member "operator:admin:Operator" --member "alice:researcher:Alice" --member "bob:reviewer:Bob" --member "observer:viewer:Observer"
python scripts/atx-workspace.py --preset server-rel --team-config C:/atx-private/team/team.json
```

The parent directory must already exist. The generator refuses existing output,
prints file paths only, and issues random credentials that expire after 90 days.
`--valid-days` changes this; zero explicitly requests no expiration. Restrict the
private directory to the operator with the operating system's access controls.
`team.json` contains only SHA256 token digests; `credentials.json` contains the
individual tokens. Give each person their own token to enter through **Access**.
Do not place the credential file in the repository or a web-served directory.
Replace the configuration and restart both processes to rotate or revoke access.

The immutable native policy admits 1–128 distinct principals and requires an
unexpired administrator at startup. Roles are cumulative: viewers read shared
research and databases; researchers also submit/cancel work and edit records;
reviewers also approve/reject evidence; administrators can additionally withdraw
another person's review. A credential is checked on every RPC. Direct native
event streams end at expiry; browser streams reauthenticate at least every
15 minutes and also end at their credential's expiry.

The launcher enables `ATX_WEB_TEAM_AUTH=1`. With separately launched processes,
set that variable on the gateway and pass `--team-config` to the native server.
Team mode requires a separate `ATX_SERVER_TOKEN` for gateway health and event
delivery. It cannot authenticate ordinary team RPCs or reuse any configured
person's credential, including an expired one. `ATX_WEB_TOKEN` cannot be combined
with team mode. Browser requests forward the actual person's credential; actors
are resolved by the native service and cannot be supplied in mutation bodies.

Review submission freezes an exact alpha revision, its canonical recipe hash,
and 1–12 successful matching runs. The server derives evidence hashes, provenance
and limitations from stored results. Submitted evidence and decision history are
immutable. Decisions use exact expected revisions and require a rationale.
Approval requires an active, current alpha head. Team approval/rejection also
requires independence from the requester and the alpha's authors; standalone
mode explicitly permits self acknowledgement. A requester or administrator can
withdraw a submitted or approved record. Approval is a recorded research review,
not evidence that unverified market history or execution capacity is qualified.

Each user mutation records bounded metadata and its authenticated actor in the
same SQLite transaction. Audit insertion failure rolls back the mutation;
internal worker progress and completion remain independent. Old rows retain
`migration_unknown` attribution rather than inventing a historical actor.
The audit log is append-only through the service, with indexed pagination and no
fixed retention cap; operating-system owners of the SQLite file remain trusted.
Limits are 2,048 reviews, 8,192 decisions, 96 KiB frozen evidence per review and
16 MiB aggregate source results. Credentials, raw SQL, layouts and recipes do
not appear in audit metadata or event notifications.

Browser routes are `GET /api/session`, `GET /api/team`, `GET /api/audit`,
`GET/POST /api/reviews`, `GET /api/reviews/:id`,
`POST /api/reviews/:id/decision` and `GET /api/reviews/:id/decisions`.
`GET /healthz` reports only readiness; authenticated `/api/health` includes
operational counters.

### Registered market panels and expression validation

The launcher discovers the published `atx-db/data/alpha_panel/v1/panel/manifest.json`
and registers it as `atx-equity-panel` with the SHA256 of its exact bytes. Set
`ATX_DB_DATA_DIR` to change the discovery root, pass repeatable
`--research-dataset ID=[SHA256=]PATH` to override discovery, or use
`--no-research-datasets` to disable it. Native-only registration requires the pin:
`--research-dataset ID=SHA256=MANIFEST`. These settings are independent of database
viewer sources. A manifest is limited to 1 MiB and must declare a complete v2 panel.

`real_dsl` requests carry both `dataset_id` and `dataset_sha256`. Only explicitly
listed, individually verified monthly TRAIN payloads from 2018–2022 are read;
requests cannot supply filesystem paths. Selected files total at most 2 GiB.
Each job uses an isolated DuckDB reader, then the native alpha compiler and
cash/holdings replay with explicit delay, trading costs and borrow assumptions.
The initial cohort contains up to 512 instruments selected using the first
scoring session's eligible membership and published prior-session ADV63.
Membership subsequently gates signals; marks for existing holdings remain
available to the execution engine. New cohort entrants are not added mid-run.

Fields are currently price/liquidity inputs. `close` is a causal forward
total-return index constructed from observed returns, avoiding backward-adjusted
price-level leakage. Guarded returns and missing marks retain the execution
engine's declared adverse-liquidation behavior. Historical source vintages,
common-stock history, borrow availability and capacity are not certified by
this adapter; read each result's provenance before interpreting performance.

`ValidateExpression` (`POST /api/analysis/validate`) compiles the same language
used by execution without reading market rows. The catalog exposes 35 canonical
operators and limits: 1,024 UTF-8 bytes, 256 tokens/nodes, nesting 16, literal
windows 1–252, and cumulative lookback 756 sessions. Successful validation does
not establish data coverage. Execution additionally checks panel dimensions,
working memory and a conservative weighted-work budget.

`CompareJobs` (`POST /api/analysis/compare`) accepts 2–12 successful runs and
matches exact start/end return periods across all inputs. Complete returns drive
correlations and metrics; chart sampling happens afterward. Missing periods are
omitted and reported, never zero-filled. Constant variance produces null ratios.
Mixed synthetic/market evidence requires explicit opt-in. Existing runs without
complete return series must be rerun. The total input budget is 16 MiB and
200,000 observations; two comparisons may execute concurrently. Compounded
matched returns are analytical comparisons, not a combined portfolio replay.

The opt-in real-data integration gate is `python scripts/test-atx-research.py
--server build-server-rel/bin/atx-server.exe`. It uses an isolated service/database,
reads the registered TRAIN panel, and checks native validation, execution,
comparison and restart durability. It never writes market payloads.

| Operation | Browser endpoint | gRPC method |
| --- | --- | --- |
| Health and resource counters | `GET /api/health` | `GetHealth` |
| Research modes and defaults | `GET /api/strategies` | `ListStrategies` |
| Submit or schedule work | `POST /api/jobs` | `SubmitJob` |
| Paged job summaries | `GET /api/jobs` | `ListJobs` |
| Request and complete result | `GET /api/jobs/:id` | `GetJob` |
| Cancel queued/running work | `POST /api/jobs/:id/cancel` | `CancelJob` |
| Replay/live event channel | `GET /api/events` | `WatchEvents` |
| Configured data sources | `GET /api/data/sources` | `ListDataSources` |
| Database tables and views | `GET /api/data/catalog` | `GetDataCatalog` |
| Bounded table preview | `GET /api/data/preview` | `PreviewData` |
| Read-only SQL result | `POST /api/data/query` | `QueryData` |

Job lists omit request/result documents. Read one job to retrieve them. Events are
notifications: refresh the job snapshot after a completion or replay gap. Channels
are `jobs`, `system`, and `all`, optionally filtered by job ID. Sequence numbers
are decimal strings in JavaScript to preserve the complete uint64 range.

## Database viewer

The dockable Database panel uses provider-neutral source, catalog, column and
result messages. `DatabaseProvider` adapters own connection details and query
execution; the browser and gateway only use server-assigned source IDs. DuckDB
is implemented. PostgreSQL/MySQL can be added behind this interface; they are not
advertised as available adapters.

Open `http://localhost:3000/?panel=database`, or use the **Database** toolbar button.

The workspace launcher discovers `ATX_DB_PATH`, or looks beneath `ATX_DB_DATA_DIR`
(default `atx-db/data`) for `catalog.duckdb`, then `warehouse.duckdb`. The current
atx-db serving layer is the catalog of Parquet-backed views. When discovering a
catalog, the launcher permits reads beneath its `alpha_panel/v1` directory if
present. It does not initialize, migrate, checkpoint or copy the warehouse.

Use explicit configuration to override discovery:

```powershell
python scripts/atx-workspace.py --preset server-rel --data-source atx-db=C:/atx/atx-db/data/catalog.duckdb --data-root C:/atx/atx-db/data/alpha_panel/v1
```

Both flags are repeatable. Explicit sources have no implicit readable data roots;
add only the directories needed by their views. `--no-data-sources` disables
discovery and database viewing. Native-only launches accept the same
`--data-source ID=PATH` and `--data-root PATH` flags. Paths and connection
credentials cannot be supplied through the browser API.

DuckDB is opened read-only for each operation and closed afterward. An active
writer in another process can prevent access: the viewer reports this condition
and allows retry. Use a checkpointed serving catalog/snapshot when ingestion
needs uninterrupted write access. A live DuckDB file must not be copied as a
substitute for publishing a consistent snapshot. See DuckDB's
[concurrency model](https://duckdb.org/docs/current/connect/concurrency).

Queries admit a single SELECT statement, including CTEs, with bounded returned
rows and exact string cells. NULL, empty strings and truncated cells are separate
states. Preview pages have no implicit sort guarantee; use SQL with an explicit
`ORDER BY` for a reproducible order. Catalog results contain tables and views;
column names and database type names accompany each preview/query result.

Limits are two concurrent database operations, five seconds per query, 64 KiB of
SQL, 500 returned rows (200 by default), 256 columns, one million skipped preview
rows and 2,000 catalog objects. The adapter bounds response text to 4 MiB and each
cell to at most 16 KiB. Wide results use a smaller, explicitly marked cell limit
to keep the intermediate rendered text within 64 MiB. Each connection has a
256 MiB DuckDB memory budget, one execution thread and disabled disk spilling.
The browser renders a window of rows rather than constructing a DOM cell for
every returned value. Complex source expressions still require memory before
rendering; restrict expensive work to appropriate research jobs.

The adapter disables arbitrary external access and extension installation/loading,
locks its configuration, and allows file reads only within operator-approved
data roots. The SELECT restriction prevents database/file writes. These controls
serve a trusted analyst workspace; DuckDB's settings are not an OS sandbox for
hostile SQL or untrusted tenants. Resource and interruption limits also do not
constitute a hard process-RSS ceiling. See
[DuckDB's embedding security guidance](https://duckdb.org/docs/current/operations_manual/securing_duckdb/overview).

The native dependency uses DuckDB's stable C API, pinned to version 1.5.1 to
match atx-db. Windows x64 and Linux x64 release archives are SHA-256 checked at
configure time. Set `ATX_DUCKDB_ROOT` to an existing 1.5.1 C API distribution for
offline builds or other platforms. Windows builds copy `duckdb.dll` and its
license notice beside each dependent executable. This dependency adds no Python
runtime requirement to the service.

## Research modes

Submit a browser request with an idempotency key:

```json
{
  "idempotency_key": "my-research-request-001",
  "scheduled_at_ms": 0,
  "request": {
    "mode": "synthetic_demo",
    "strategy_dsl": "rank(delta(close,5))",
    "sessions": 504,
    "initial_capital": 1000000,
    "trade_bps": 6,
    "annual_borrow_bps": 300
  }
}
```

- `synthetic_demo` evaluates a bounded causal alpha expression with the existing
  parser/compiler/evaluator and delayed execution replay. Prices are generated;
  these results are for validating the system, not trading evidence.
- `import_nav` takes `artifact_path`, optional `summary_path`, and optional
  `start_date`/`end_date`. It reads allowed NAV CSV artifacts, recalculates metrics
  from complete observations, and records hashes of the bytes read. Summary
  metadata never replaces recalculated metrics.
- `real_nav` takes `combined_path`, `combined_sha256`, `role_path`, and
  `role_sha256`. It runs the pinned saved-blend NAV pipeline inside `atx-impl` and
  returns structured results from the primary scenario. Output directories are
  generated by the server. The UI provides the pinned-input configuration form.

Artifact paths are relative to `--repo-root` and must resolve beneath a configured
`--artifact-root`. Traversal,
out-of-root paths, malformed/nonfinite data and excessive resource requests are
refused. Input artifacts must be immutable while in use. The initial replay
adapter admits TRAIN evidence and preserves the existing report's locked
validation/VAL/2023–2025 restrictions, including checks on actual session dates.
Unlocking evaluation roles requires a separate explicit policy change.

Results contain metrics, equity/drawdown curves, return histograms, monthly
returns, available fills/positions and provenance. Undefined ratios are `null`;
unavailable benchmark/factor/trade evidence stays empty. Statistics use all
qualified rows; charts have a bounded number of points.

Provenance records the engine build Git SHA, SHA-256 of the stored canonical request,
input artifact hashes, execution assumptions and evaluation restrictions. Canonical
pretrade NAV must reconcile with observed returns within absolute tolerance
`1e-10`; contradictory evidence is rejected. Legacy posttrade-only imports carry
an explicit warning that this reconciliation is unavailable.

## Durability, concurrency and resource limits

SQLite uses the repository's pinned amalgamation, WAL, full synchronous commits,
foreign keys, a busy timeout, versioned migrations and prepared statements.
One process owns each database, enforced with an OS file lease. Store/cache
mutations share one mutex; a write invalidates its cached snapshot before the
lock is released. Workers execute research outside store/scheduler locks.

Defaults: two execution workers, 1,024 active queued/running jobs, a 512-entry /
32 MiB / two-second read cache, a 2,048-event replay ring, and 32 native event
subscribers. Requests are limited to 64 KiB JSON with bounded nesting; results
are limited to 16 MiB. Job pages are bounded. Increase execution workers only
with a corresponding memory budget for concurrent research workloads.

The synchronous gRPC server uses a 96-thread resource quota and a 128 MiB memory
quota target. The thread quota bounds allocations from its synchronous RPC pool;
the separate 64-poller setting controls waiting pollers. Exhausted RPC thread
admission can return `RESOURCE_EXHAUSTED`. The memory quota governs gRPC-accounted
buffers and reclamation; process memory also includes execution workers, research
data, SQLite, the cache, and other runtime allocations. See the
[gRPC ResourceQuota contract](https://grpc.github.io/grpc/cpp/classgrpc_1_1_resource_quota.html).

The gateway shares one native event stream across up to 512 browser subscribers
by default. Its replay ring is bounded by 2,048 events and 4 MiB; a slow browser
is disconnected when its output buffer exceeds 256 KiB. Per-IP HTTP admission
defaults to 240 requests per minute. See the frontend guide for tuning these
limits behind a proxy or shared network.

The same idempotency key and canonical request/schedule returns the same job;
different content returns a conflict. Cancellation is terminal even if a worker
finishes concurrently. Queued future work survives a restart. Work interrupted
while running becomes failed with an explicit recovery error; retrying it requires
a new idempotency key. This avoids silently replaying a computation with outputs.
Cancellation is cooperative during computation. Cancellation during final file
publication can leave a completed forensic artifact directory, while the job
remains cancelled and exposes no successful result. Publication I/O failures can
leave an incomplete directory without the final `summary.json` marker.

Events remain memory-resident. Sequence blocks are reserved durably in SQLite,
so a reconnect after restart or ring eviction receives a gap and refreshes its
snapshot. This is bounded replay, not a durable message broker or exactly-once
event delivery.

## Operations

Loopback is the default. `ATX_SERVER_TOKEN` supplies a 32–256 character native
bearer token. The launcher creates one for its two child processes without writing
it to disk. `ATX_WEB_TOKEN` is a separate optional browser access token; the UI
keeps it in memory. Tokens are never included in URLs. Native non-loopback binds
require both a token and `--tls-cert`/`--tls-key`. For browser deployment, put the
gateway behind HTTPS and configure its independent access token.

The service is a shared research workspace. Team mode authenticates individual
principals and enforces viewer, researcher, reviewer and administrator roles.
All team members share one project; there is no tenant separation. The topology
is one native service with local storage. The SQLite file belongs on local disk.
Health includes queue/worker/cache/event counters. Cache bytes are accounted
storage, not process RSS. Research failures are persisted on jobs; scheduler
failures emit system events. Bound artifact roots narrowly in deployments.

For a backup, stop the service cleanly and copy its database and research output
directory together, or use SQLite's online backup API. Do not copy a live database
file while omitting its WAL. Retention/deletion of old jobs and artifacts is an
operator task in this version; the service does not silently discard evidence.

The implementation follows the [gRPC C++ service guidance](https://grpc.io/docs/languages/cpp/best_practices/)
and [SQLite WAL operating constraints](https://www.sqlite.org/wal.html).

## Windows protobuf build compatibility

The pinned protobuf C++ runtime 6.33.4 is built with MSVC by vcpkg. Its headers enable
`PROTOBUF_RESTRICT` for clang, which adds an `I` to clang-cl's pointer parameter mangling:
the generated client references `InternalSwap` with `PEIAV`, while the MSVC DLL exports
the corresponding `PEAV` symbol. A generated, target-scoped `port_def.inc` include overlay
disables this annotation only for clang-cl, matching the packaged DLL declarations.
The overlay preserves the upstream header and license, checks its exact version and macro
before patching, and leaves the shared vcpkg installation untouched. See
[protobuf-msvc-abi.cmake](cmake/protobuf-msvc-abi.cmake).

Only the generated protobuf target suppresses clang's offset warning for protoc's private
runtime tables; first-party warning checks remain enabled.
Native qualification covers authenticated RPCs, subscriber capacity, prompt stream
cancellation, durable resources, governance, research execution and recurring workflows.

## Recurring workflows

The Operations workspace includes a Workflows panel. A definition freezes one
recipe or an exact saved-alpha revision, including dataset pins and research dates.
Recurrence repeats that recipe; it does not roll the dates or follow later alpha edits.
Use a fixed UTC interval from 60 seconds to 30 days and a lifetime budget of
1 to 1,000 admitted jobs. The service retains at most 256 immutable definitions.

Pause and resume require the current revision. Pausing stops new admissions and
leaves an active job running; cancel that job separately if needed. A completed
budget cannot be reset by resuming. Only one occurrence executes at a time,
including while a cancelled worker is still stopping. Queue backpressure keeps
the due slot pending. After downtime the scheduler admits the latest due slot
and records missed/skipped counts instead of creating an unbounded catch-up queue.

The joined scheduler timer rechecks the creator's research role and expiry before
each admission. Removed, expired or demoted creators and archived source alphas
cause an auditable automatic pause. Permissions are read from the native startup
policy; restart the server after an operator changes its team configuration.
Slot, job and cursor writes commit together. Definitions and occurrence links
are immutable, and run pages retain the original scheduled UTC slot and actor.

## Executable receipts

Research results include `provenance.runtime_build_sha256` when the standalone
server executes them. The pin hashes a canonical content manifest of the running
executable and adjacent native libraries, excluding absolute paths, timestamps
and operating-system libraries. Relocating identical distribution files preserves
the pin; changing executable or library content changes it. Distributions must
remain immutable while running. Embedded/test research backends may omit this pin.

The native historical paper adapter has a bounded, complete ledger with separate
fills, fees, financing, liquidations and valuations. Persistent paper accounts are
still under development; this adapter alone does not create an account or execute
broker orders.
