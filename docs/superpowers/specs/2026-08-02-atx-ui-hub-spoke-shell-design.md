# atx-ui — Hub-and-Spoke Shell Rework — Design

Date: 2026-08-02
Status: Approved — ready for implementation planning
Scope: `atx-ui` shell, client session layer, and the Surface Database workspace.
Depends on: `2026-08-02-atx-server-rpc-foundation-design.md` (spec 1)

## 1. Goal

Turn `atx-ui` from a single-workspace app that loads and fits data in-process
into a **spoke**: a thin client of `atx-server` with a real shell — panel
registry, workspace host, typed command bus, link contexts, versioned layouts —
and a client session layer that keeps every socket call off the render thread.

Ship two workspaces on that shell: the existing six vol panels, and a new Surface
Database browser.

**This spec does not begin until spec 1's acceptance gate passes.** Building the
shell on an unproven transport means debugging two new layers at once.

## 2. Current state

`atx-ui` is 2117 lines across two libraries and one executable.

| Unit | Lines | Role |
|---|---|---|
| `src/framework/application.cpp` | 285 | GLFW/OpenGL/ImGui lifecycle, dockspace, `add_panel(PanelSpec)`, versioned layout id |
| `src/framework/theme.cpp`, `widgets.cpp` | 206 | palette, shared widgets |
| `src/vol/vol_workspace.cpp` | 648 | the six vol panels, all rendering off `VolSurfaceSource &` |
| `src/vol/spy_opra_surface.cpp` | 368 | parquet load + in-process `atx-vol` fit, **and** domain→view-model derivation |
| `src/main.cpp` | 166 | argument parsing; hard-wires one `OpraVolSurface` into one `VolWorkspace` |

`Application` already has the seed of a panel registry: `PanelSpec{id, title,
initial_dock, render, open}`, `DockSlot`, and a `layout_id` that is bumped when
the default topology changes. The shell rework extends this rather than replacing
it.

`VolSurfaceSource` (`include/atx/ui/vol_surface_source.hpp:105`) is a clean
read-only adapter boundary and is the reason this refactor is tractable: the six
panels already depend only on it.

**What the current design cannot do.** `platform-architecture.md:36-39` states
the rule "publish immutable generation-numbered snapshots; panels should never
hold locks or observe a partially updated chain." The current
`VolSurfaceSource` returns `const T&` references into a live adapter, so that
rule is stated but unenforceable. §5 fixes this.

## 3. Decision record

### 3.1 Server-only: the in-process fit path is deleted

`spy_opra_surface.cpp` does two separable things, and they go to different
places:

| today | after |
|---|---|
| parquet load + in-process `atx-vol` fit | **deleted.** Fitting already belongs to the offline `surface_db_populate` tooling. |
| domain → view-model derivation (expiry summaries, curve slices, quote points, diagnostics) | **moved to** `atx-server/encode.hpp` (spec 1 §4.6) |

**Functional consequence, stated plainly:** after this change the UI can display
only surfaces that already exist in a `SurfaceDb`. Pointing `atx-ui` at a raw
parquet snapshot and fitting on the spot stops working; that workflow becomes
"run the populate tool, then browse". The browsable universe is exactly what has
been populated.

This is the accepted cost of having one data path instead of two. The rejected
alternative — keeping both `RemoteSurfaceSource` and `OpraVolSurface` behind
`VolSurfaceSource` — requires implementing every new field twice and lets the hub
be bypassed; divergence between the two would be a matter of when, not if.

### 3.2 No auto-spawn: "not connected" is a first-class state

`atx-ui` never starts a server. `--server host:port` is required. With no
reachable server the app opens to a connection view with the target address, a
retry control with backoff, and the last error — a designed state, not an error
dialog over an empty workspace.

### 3.3 The shell extends `Application`, it does not replace it

`Application` already owns the ImGui/GLFW lifecycle, docking, and layout
persistence. The new shell layer sits above it and supplies `PanelSpec`s. No
rewrite of `application.cpp`.

## 4. Architecture

```
atx-ui/
  include/atx/ui/ , src/
    framework/          UNCHANGED: application, theme, widgets
    shell/
      panel_registry    PanelId -> factory; panels self-register
      workspace         Workspace interface; workspaces own panel sets
      workspace_host    composes the active workspace's panels into Application
      command_bus       typed commands; no globals
      link_context      named channels carrying a selection tuple
      layout            versioned serialize/restore, per workspace
    client/
      session           owns atx::rpc::RpcClient; worker thread; async calls
      snapshot_store    shared_ptr<const T>, generation-stamped
      remote_surface_source  : VolSurfaceSource, backed by snapshot_store
    workspaces/
      vol/              the six existing panels, registered
      surfacedb/        NEW: database browser + coverage matrix
    main.cpp            --server host:port (required), --token, --workspace
```

Library targets:

- `atx-ui-framework` — unchanged.
- `atx-ui-shell` — registry, host, command bus, link context, layout. No
  networking, no `atx-vol`.
- `atx-ui-client` — session, snapshot store, remote source. Links `atx-rpc`,
  `atx-proto`, and `atx::vol`. `atx::vol` is a single library so the link is
  whole; what is bounded is the *usage*, which §6 restricts to the archive
  decode path (`SurfaceArchiveV2::open` / `map_symbol` / `PricedSurfaceView`
  evaluation). No fitting, no parquet, no filesystem access.
- `atx-ui-vol`, `atx-ui-surfacedb` — one per workspace.
- `atx-ui` — the executable.

`atx-ui-shell` deliberately does not depend on `atx-ui-client`. A workspace is
given its data sources; it does not reach for a global session.

## 5. Client session and the snapshot rule

### 5.1 Threading

**No socket call ever runs on the render thread.** This is not a guideline; it is
the reason the layer exists.

```
render thread            session worker thread
     |                            |
 submit(request) ----------------> RpcClient::call (blocking)
     |                            |
 drain_completions()  <----------- push completed response
     |
 swap snapshot (atomic store of shared_ptr<const T>)
     |
 panels read shared_ptr<const T> — no lock, never a partial view
```

`Session::drain_completions()` is called once per frame from the render thread.
Completions are moved off a mutex-guarded queue in one batch; panels see either
the old snapshot or the new one, never a half-updated one.

### 5.2 Snapshot store

```cpp
template <class T>
class SnapshotStore {
public:
  [[nodiscard]] std::shared_ptr<const T> get() const noexcept;  // atomic load
  void publish(std::shared_ptr<const T> next, std::uint64_t generation);
  [[nodiscard]] std::uint64_t generation() const noexcept;
};
```

`generation` is the server's `ResponseMeta.db_generation`, carried through
unchanged. A `publish` whose generation is not greater than the current one is
dropped — an out-of-order completion cannot move the UI backwards.

This is what makes `platform-architecture.md`'s "immutable and
generation-stamped" rule enforceable rather than aspirational.

### 5.3 `RemoteSurfaceSource`

Implements the existing `VolSurfaceSource` interface, backed by a
`SnapshotStore<VolSnapshot>`. The six vol panels compile against it unchanged.

Two members of the current interface need decisions:

- `select_expiry(std::size_t)` is synchronous and returns `bool`. It becomes: set
  the pending index, dispatch a `GetCurve`, return true if the request was
  submitted. The panels already re-read `slice()` every frame, so a one-frame lag
  is invisible.
- `set_quality_mode(UiFitQualityMode)` and its `quality_mode()` getter selected
  and reported an in-process fit quality. There is no in-process fit any more.
  **Both are removed** from the interface; the fit configuration that produced a
  stored surface is read-only server data, surfaced in the diagnostics panel as
  `SymbolFitConfig` rather than as a control. `SurfaceDiagnostics.quality_mode`
  becomes a display-only string carried from the server.

`UiFitQualityMode` and its `--quality` flag are deleted from `main.cpp`.

### 5.4 Reconnection

`Session` owns connection state: `Disconnected | Connecting | Handshaking |
Ready | Failed`. Reconnect is exponential backoff with jitter, capped. A
`CODE_UNAVAILABLE` or `CODE_BUSY` response schedules a retry of that request; a
`CODE_UNAUTHENTICATED` does not — it surfaces to the connection view, because
retrying a bad token forever is a bug, not resilience.

## 6. Binary surface pull

For the vol workspace's dense interactions — axis changes, strike-window resizes,
re-normalisation — a round trip per change is the wrong shape. `Session` pulls
the whole surface once:

```
GetSurfaceBlob -> bytes
  -> atx::vol::SurfaceArchiveV2::open(bytes)
  -> map_symbol(symbol) -> PricedSurfaceView
  -> evaluate locally for every subsequent axis/window change
```

This is the one place `atx-ui` links `atx::vol`, and it is bounded to the archive
decode path. The blob's `atxvsa_schema_hash` is checked against the client's
build; a mismatch is reported as a version-skew banner naming both hashes, never
a silent mis-read (spec 1 §7.3 returns `CODE_FAILED_PRECONDITION` for this, so
the check is server-side too).

Policy: pull `GetCurve` for the initially selected expiry so the first frame is
fast, and pull the blob in the background. Once the blob lands, local evaluation
takes over.

## 7. Shell

### 7.1 Panel registry and workspaces

```cpp
struct PanelDescriptor {
  std::string id;            // "vol.curve", "surfacedb.coverage"
  std::string title;
  DockSlot    initial_dock;
  std::function<PanelSpec(WorkspaceContext &)> make;
};

class PanelRegistry {
public:
  void add(PanelDescriptor d);
  [[nodiscard]] const PanelDescriptor *find(std::string_view id) const;
};

class Workspace {
public:
  virtual ~Workspace() = default;
  [[nodiscard]] virtual std::string_view id() const noexcept = 0;
  [[nodiscard]] virtual std::string_view title() const noexcept = 0;
  [[nodiscard]] virtual std::span<const std::string> panel_ids() const noexcept = 0;
  virtual void on_activate(WorkspaceContext &) = 0;
};
```

`WorkspaceHost` owns the registry, the active workspace, and the per-workspace
layout id. Switching workspaces saves the outgoing layout and restores the
incoming one — `Application::layout_id` becomes `"<workspace>-v<n>"`.

### 7.2 Command bus

```cpp
struct SelectDatabase  { std::string db_id; };
struct SelectPartition { std::string partition_key; };
struct SelectSymbol    { std::string symbol; };
struct SelectExpiry    { std::string expiry_iso; };
struct RefreshCatalog  {};
struct Reconnect       {};

using Command = std::variant<SelectDatabase, SelectPartition, SelectSymbol,
                             SelectExpiry, RefreshCatalog, Reconnect>;
```

Panels dispatch commands; they never mutate another panel's state. The bus is
drained once per frame, before rendering, so a frame renders one consistent
post-command state. This satisfies the existing rule "cross-panel actions are
typed state transitions, not global variables."

### 7.3 Link contexts

A named channel carrying the selection tuple:

```cpp
struct Selection {
  std::string db_id;
  std::string partition_key;
  std::string symbol;
  std::string expiry_iso;
};
```

Each panel is assigned a channel (`A`, `B`, `None`) via its title-bar menu.
Publishing to a channel updates every panel on it. This is how the Surface
Database browser drives the vol panels: selecting a coverage cell publishes
`{db_id, partition_key, symbol}` to channel `A`, and the vol workspace's panels
follow.

Channel assignment is part of the persisted layout.

## 8. Surface Database workspace

The concrete answer to "surface database viewer".

```
┌ Databases ─────┐┌ Partitions ────────┐┌ Symbols ──────────────────┐
│ spy_prod    ✓  ││ 2024-03-15  412 sf ││ SPY   18 exp  spot 512.30 │
│ sp100_dev      ││ 2024-03-14  409 sf ││ AAPL  14 exp  spot 172.11 │
└────────────────┘└────────────────────┘└───────────────────────────┘
┌ Coverage matrix (partition × symbol) ──────────────────────────────┐
│        03-11 03-12 03-13 03-14 03-15                               │
│ SPY      ●     ●     ●     ●     ●     ● FITTED                    │
│ AAPL     ●     ●     ○     ●     ●     ○ CONFIGURED_NOT_FITTED     │
│ MSFT     ●     ·     ·     ●     ●     · ABSENT                    │
└────────────────────────────────────────────────────────────────────┘
┌ Symbol detail ─────────────────────────────────────────────────────┐
│ SymbolFitConfig · provenance · db_generation · content_hash        │
└────────────────────────────────────────────────────────────────────┘
```

| Panel | Backing RPC |
|---|---|
| `surfacedb.databases` | `ListDatabases` |
| `surfacedb.partitions` | `ListPartitions` |
| `surfacedb.symbols` | `ListSymbols` |
| `surfacedb.coverage` | `GetCoverage` |
| `surfacedb.detail` | `GetSymbolConfig` + `GetSurfaceMeta` |

The coverage matrix is one indexed catalog query (spec 1 §7.2). It is also the
data-quality view the surface-database work keeps needing: `○` cells are exactly
"configured but never populated".

Selecting a cell publishes to the link context and, if the vol workspace is
open, drives it. A "Open in vol workspace" action switches workspaces with the
selection carried over.

## 9. Testing

Panel rendering is not tested; everything below runs headless with no graphics
context, matching the existing `atx-ui/tests` pattern.

| Test | Proves |
|---|---|
| `SnapshotStore` generation monotonicity | an out-of-order or stale `publish` is dropped; `get()` never returns a torn value |
| `Session` completion draining | a completion enqueued from the worker thread is visible only after `drain_completions()`; requests are never issued from the caller's thread |
| Reconnect policy | backoff sequence with jitter bounds; `UNAUTHENTICATED` stops retrying while `UNAVAILABLE` does not |
| Command bus routing | each command variant reaches exactly its registered handler; the bus is empty after a drain |
| Link context fan-out | publishing on channel `A` updates only channel-`A` subscribers |
| `PanelRegistry` / `WorkspaceHost` | unknown panel id is rejected; workspace switch saves and restores the correct layout id |
| Layout round-trip | serialize → restore reproduces panel open-state, dock slots, and channel assignments; an unknown version resets to defaults instead of throwing |
| `RemoteSurfaceSource` against a fake session | the six panels' `VolSurfaceSource` contract is satisfied from canned proto responses, with no socket |
| Blob decode path | a `SurfaceBlob` fixture decodes to a `PricedSurfaceView` whose `fair_value` matches the `GetCurve` response for the same expiry |
| Schema-hash skew | a blob whose `atxvsa_schema_hash` differs raises the version-skew state rather than decoding |
| `VolWorkspaceState` projections | existing tests, unchanged |

An end-to-end test starts a real `atx-server` on an ephemeral loopback port
against a fixture `SurfaceDb`, runs `atx-ui --headless --frames N`, and asserts
the vol snapshot generation advanced and the coverage matrix is non-empty.

## 10. Build wiring

- `ATX_BUILD_UI` (already exists, default OFF) now also pulls in `atx-proto` and
  `atx-rpc`. Spec 1 §10 states the shared gating.
- New targets `atx-ui-shell`, `atx-ui-client`, `atx-ui-surfacedb`.
- `atx-ui-vol` loses `src/vol/spy_opra_surface.cpp` and its direct `atx::vol`
  dependency; only `atx-ui-client` keeps one, for archive decode.
- `atx_warnings` / `/W4 /WX` applies to every new target.

## 11. Deliberately deferred

- **Streaming/live updates.** The snapshot store and generation discipline are
  the right shape for it, but the server has no subscription RPC yet (spec 1
  §11). Until then the UI polls on demand.
- **More than two workspaces.** The registry exists to make a third cheap, not
  because a third is planned in this spec.
- **Multi-server connections.** One `Session`, one server.
- **Workspace layout sharing/export.**
- **The vol panels' own redesign.** They move onto the shell unchanged. Reworking
  the shell and the panels simultaneously would make regressions impossible to
  attribute.

## 12. Acceptance criteria

1. `cmake --preset dev -DATX_BUILD_UI=ON` builds `atx-ui` with the new targets;
   a build without `ATX_BUILD_UI` compiles no shell, client, or proto code.
2. `atx-ui` with no `--server` argument exits non-zero with usage naming the
   required flag.
3. `atx-ui --server 127.0.0.1:50051` with no server running opens to the
   connection view showing the target address and a retry control, and does not
   crash or hang.
4. With a server running against a populated fixture database, the Surface
   Database workspace lists databases, partitions, and symbols, and renders a
   coverage matrix with at least one `FITTED` and one `CONFIGURED_NOT_FITTED`
   cell.
5. Selecting a coverage cell drives the vol workspace's six panels to that
   `(db_id, partition_key, symbol)` via the link context.
6. Changing the x-axis, y-axis, or normalised window after the blob has landed
   issues **zero** network requests, confirmed by a request counter assertion.
7. A blob with a mismatched `atxvsa_schema_hash` produces a version-skew banner
   naming both hashes and no decoded surface.
8. `src/vol/spy_opra_surface.cpp` and `UiFitQualityMode` no longer exist in
   `atx-ui`.
9. No socket call originates from the render thread, asserted by a thread-id
   check compiled into debug builds of `Session`.
10. `ctest -L atx_ui` is green.
