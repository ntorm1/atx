# Sprint PF4-S10 — Served read tier + panel perf (isolated read-only serving + Arrow cache + materialized cross-section)

**Goal:** make PIT reads of the factor panel **safe, fast, and concurrent** behind the SDK. Today
`db/factor_panel.py::read_panel_asof` opens the 14 GB warehouse `read_only=False` (line 681,
`with connect(db_path, read_only=False)`) — taking a **writer lock on the read path** and re-running
`initialize()`/migrations, so two consumers reading the same panel serialize behind a writer and a served
read can be blocked by (or block) a maintenance rebuild. And every single-date cross-section read replays the
full windowed `row_number() OVER (PARTITION BY security_id, factor_id ORDER BY as_of_date DESC …)` scan over
all of `v_factor_panel` history (lines 621–651), so a point-in-time slice pays a full-history scan. This
sprint lands a NEW thin **`db/panel_serving.py`** served read tier: an **isolated read-only connection pool
(`read_only=True`)** plus an **Arrow result cache keyed by `(as_of, universe, version, factors, release)`**
that **guarantees no writer lock is taken on the read path** (fixing the `read_only=False` hazard PF4-S3
began), with an optional **pinned Parquet/Arrow release backend** that serves without touching the DB at all;
and a **materialized latest cross-section** (`factor_panel_latest_snapshot`) + **covering indexes** so a
single-date PIT read meets a stated budget while returning **rows byte-identical to `read_panel_asof` / the
SDK**. Reserved migrations **0199–0200**.

**Mandate / Owns:** NEW thin `db/panel_serving.py` (the `PanelServer` served tier — isolated read-only
connection pool, Arrow result cache, DuckDB-read-only **and** pinned-release backends, the
`build_panel_latest_snapshot` materializer, and a `served_cross_section(...)` helper); the targeted
`read_only=False → read_only=True` fix on the `db/factor_panel.py` panel **read path** (`read_panel_asof`,
`describe_factor_panel`) with a regression lock; migrations **0199** (`factor_panel_latest_snapshot`
materialized cross-section support table + `panel_serving_catalog` + catalog rows) and **0200** (covering
indexes + serving-catalog registration) in a NEW `db/migrations/bodies_0199_0200.py` wired into
`db/migrations/registry.py`; and `db/tests/test_panel_serving.py`.

**Must NOT touch:** the S10-authored panel **content/contract** — `v_factor_panel` / `v_factor_panel_wide`
view bodies, `PANEL_CONTRACT`, the export-boundary lookahead gate
(`factor_panel_export_gate_report` / `assert_factor_panel_export_ready`), and the panel `expected_schema_sha256`
(the served tier **reads** the same rows; it never redefines a factor, alters the contract, or perturbs the
schema hash). The `db/factors/` engine + families (consume, never redefine). `db/signal_eval.py` (PF4-S1).
The S7 `db/panel_release.py` release engine and S8 `clients/atx-panel/` SDK (the tier **fronts** them — it
reuses their pinned releases and returns identical rows; it does not fork their read logic). The
`db/lake.py` export writer. Any landed migration (≤ 0198) or another sprint's reserved region.

**Depends on:** PF3-S10 (`v_factor_panel` catalogued PIT views + `read_panel_asof` windowed read path +
`lake_export_object_contract` — the surface this tier serves); PF4-S3 (which flags the `read_only=False`
panel-read hazard as a Low finding — S10 **completes** the fix); PF4-S5 (multi-universe + universe
**version** — the `(universe, version)` half of the serving/cache key); PF4-S7 (immutable semver'd
Parquet **+ Arrow** releases — the pinned-release backend's source); PF4-S8 (`atx-panel` SDK — the client
this tier fronts, whose `read_panel(as_of, universe=, factors=, release=)` signature the server mirrors and
whose parity (clause **L**) it must preserve). Sequential **after** PF4-S9 in the product track
(S7→S8→S9→S10); the capstone (PF4-S11) exercises the served tier end-to-end.

---

## Baseline / where the cycles go

The panel content, contract, export boundary, and a windowed as-of read path all exist after PF3-S10, and
the SDK + releases exist after PF4-S7/S8 — but there is **no served read tier**: no isolation from the
writer, no result cache, and no materialized cross-section, so the read path is neither concurrency-safe nor
fast. Measured 2026-07-06 against `atx-impl/db/factor_panel.py` and `atx-impl/db/connection.py`.

1. **The panel read path takes a writer lock.** `read_panel_asof` (factor_panel.py:657) and
   `describe_factor_panel` (factor_panel.py:694) both open `with connect(db_path, read_only=False)`
   (lines 681, 738). `DuckDBStore.__enter__` (connection.py:22) runs `initialize()` for any non-read-only
   open (connection.py:26), so a *read* re-checks/applies migrations and acquires DuckDB's single-writer
   file lock. Consequence: two reads of the same file serialize, and a served read contends with any
   maintenance/rebuild writer. `DuckDBStore` already accepts `read_only=True` (connection.py:16, 24) and
   **skips `initialize()`** in that mode — the read path simply never uses it. This is the exact
   `read_only=False` hazard PF4-S3 enumerated and this sprint closes.

2. **Every single-date read replays a full-history window scan.** `_read_panel_asof_active`
   (factor_panel.py:585) builds `eligible` = all of `v_factor_panel` with `as_of_date <= :as_of` and
   `available_at <= :as_of_ts`, then a `row_number() OVER (PARTITION BY security_id, factor_id ORDER BY
   as_of_date DESC, available_at DESC, source_loaded_at DESC)` (lines 630–635) and keeps `rn = 1`. For a
   single decision date this scans and ranks **all** history per key. There is no per-`as_of` materialized
   cross-section and no covering index for the latest-per-key lookup — the flagship PIT slice is O(history),
   not O(cross-section).

3. **There is no result cache and no release-backed read.** Each call re-executes the query even for an
   identical `(as_of, universe, version, factors, release)`; there is no zero-copy Arrow surface a consumer
   can hold; and there is no way to serve from a **pinned immutable release** (PF4-S7) without opening the
   live DB — so even a read of a frozen, content-addressed release contends with the live warehouse's lock.

**Already good — do not regress:**
- **`DuckDBStore(read_only=True)` semantics.** connection.py:16/24/26 already opens read-only and skips
  `initialize()`; the served pool builds on this exact primitive rather than a parallel connection wrapper.
- **The windowed as-of read logic** (`_read_panel_asof_active`, factor_panel.py:585–651) is the *source of
  truth* for "latest PIT-visible value per `(security_id, factor_id)`." The materializer **reuses it
  verbatim** to populate the snapshot, so the served fast path is parity-guaranteed by construction, not a
  reimplementation.
- **The `read_parquet(?)` lake read** (lake.py:875) is the proven zero-dependency columnar entry point; the
  pinned-release backend reads the S7 release the same way (no bespoke Parquet reader).
- **The catalogued-control-table precedent** (`lake_export_object_contract`, bodies_0164_0167.py:205) is the
  template `panel_serving_catalog` follows — catalogued, drift-checked, one migration.

---

## PIT / determinism + production contract

Clauses **(A)** bitemporal/no-lookahead, **(B)** append-only catalogued migrations, **(C)** offline tests,
**(D)** determinism/provenance, **(E)** schema-as-contract, **(I)** panel PIT-safety, and **(L)** client/view
parity all apply. The load-bearing clauses for this sprint:

- **(A)/(I)** The served read is the **same PIT-safe cross-section** as `read_panel_asof`: a
  `(security_id, as_of_date)` row carries only inputs with `available_at ≤ as_of_ts` and universe membership
  resolved as-of. The materialized `factor_panel_latest_snapshot` is populated **by running the existing
  windowed read** per decision date — it introduces **zero** new PIT logic and can never widen visibility.
- **(L)** The served tier returns rows **identical** to `read_panel_asof` (the contracted view read) and to
  the S8 SDK for the same `(as_of, universe, version, factors, release)` — a parity test gates the tier.
  Serving must not perturb the panel `expected_schema_sha256` or `PANEL_CONTRACT`.
- **(D)** `build_panel_latest_snapshot` is deterministic (same inputs + dates → byte-identical snapshot
  rows, stable-sorted); the Arrow cache returns identical content on a hit; a rebuild is idempotent.
- **(B)** Migrations **0199–0200** only, schema split from index per WAL precedent, each seeding
  `table_catalog` + `field_catalog` (+ `dataset_catalog`) in the same migration; timestamped DB+WAL backup
  before any live apply (clause **F**):
  - **0199** — `factor_panel_latest_snapshot` (materialized latest cross-section support table) + the
    `panel_serving_catalog` control table + their catalog rows.
  - **0200** — covering indexes on `factor_panel_latest_snapshot` + the `v_factor_panel` serving-catalog
    registration row + refreshed field catalog.

---

## Tasks

### S10-0 — Read-only isolated served tier + connection pool (`db/panel_serving.py`)

**Root cause:** the panel read path opens `read_only=False`, so a *read* takes DuckDB's single-writer lock
and re-runs `initialize()` — reads serialize behind (and contend with) writers, and there is no served
surface that guarantees an isolated read-only connection.

**Fix:** (1) flip the hazard in `db/factor_panel.py`: `read_panel_asof` and `describe_factor_panel` open
`connect(db_path, read_only=True)` (they only read; `read_only=True` skips `initialize()` and takes no writer
lock). Regression-lock: the returned rows are unchanged. (2) Add NEW thin `db/panel_serving.py` with a
`PanelServer` holding an **isolated read-only connection pool** — each pooled handle is a
`DuckDBStore(path, read_only=True)` (or a `.cursor()` on a shared read-only connection for in-process
concurrency) — and never opens the DB writable. A construction-time guard makes a writable open on the read
path a hard error.

```python
# db/panel_serving.py (skeleton — real signatures, no placeholders)
from __future__ import annotations
import threading
from pathlib import Path
from typing import Iterable
import duckdb
import pandas as pd
from .connection import DEFAULT_DB_PATH, DuckDBStore
from .factor_panel import DEFAULT_FACTOR_PANEL_UNIVERSE_ID, READ_PANEL_COLUMNS, _read_panel_asof_active

DEFAULT_UNIVERSE_VERSION = "v1"

class PanelServer:
    """Isolated, read-only served tier over the governed factor panel."""

    def __init__(self, db_path: Path | str = DEFAULT_DB_PATH, *, backend: str = "duckdb_readonly",
                 release: str | None = None, pool_size: int = 4, cache_size: int = 64) -> None:
        if backend not in ("duckdb_readonly", "pinned_release"):
            raise ValueError(f"unknown serving backend: {backend!r}")
        self.db_path = Path(db_path)
        self.backend = backend
        self.release = release
        self._lock = threading.Lock()
        self._pool: list[DuckDBStore] = []
        self._pool_size = pool_size
        self._cache: "OrderedDict[tuple, object]" = __import__("collections").OrderedDict()
        self._cache_size = cache_size

    def _acquire_readonly(self) -> DuckDBStore:
        with self._lock:
            if self._pool:
                return self._pool.pop()
        store = DuckDBStore(self.db_path, read_only=True)  # NEVER read_only=False on the read path
        store.__enter__()
        return store

    def _release(self, store: DuckDBStore) -> None:
        with self._lock:
            if len(self._pool) < self._pool_size:
                self._pool.append(store)
                return
        store.__exit__(None, None, None)

    def close(self) -> None:
        with self._lock:
            pool, self._pool = self._pool, []
        for store in pool:
            store.__exit__(None, None, None)
```

**PIT:** (A)/(I) the pool only ever reads; (L) rows unchanged by the isolation fix.

**Test (TDD — write first, must fail before the fix):**

```python
# db/tests/test_panel_serving.py
import duckdb, pandas as pd, pytest
from db.connection import DuckDBStore
from db.factor_panel import read_panel_asof
from db.panel_serving import PanelServer
from db.tests.test_factor_panel import _insert_factor_value_fixtures

AS_OF = "2024-02-01"

def test_read_panel_asof_opens_read_only(monkeypatch, tmp_store):
    _insert_factor_value_fixtures(tmp_store)
    tmp_store.con.execute("CHECKPOINT")
    path = tmp_store.path
    tmp_store.connection.close(); tmp_store.connection = None  # release the writer lock
    seen = {}
    real = DuckDBStore.__init__
    def spy(self, p=path, *, read_only=False):
        seen["read_only"] = read_only
        return real(self, p, read_only=read_only)
    monkeypatch.setattr(DuckDBStore, "__init__", spy)
    frame = read_panel_asof(AS_OF, db_path=path)
    assert seen["read_only"] is True            # read path NEVER takes a writer lock
    assert not frame.empty

def test_panel_server_pool_never_opens_writable(monkeypatch, tmp_store):
    _insert_factor_value_fixtures(tmp_store)
    tmp_store.con.execute("CHECKPOINT")
    path = tmp_store.path
    tmp_store.connection.close(); tmp_store.connection = None
    opened = []
    orig = duckdb.connect
    monkeypatch.setattr(duckdb, "connect", lambda p, **kw: opened.append(kw.get("read_only")) or orig(p, **kw))
    server = PanelServer(path)
    try:
        server.read_cross_section(AS_OF)
    finally:
        server.close()
    assert opened and all(ro is True for ro in opened)   # every pooled open is read_only
```

**Accept:** `read_panel_asof` / `describe_factor_panel` open `read_only=True`; the same rows come back
(regression-locked below in S10-2 parity); the `PanelServer` pool opens **only** `read_only=True` handles and
a writable open on the read path raises. `test_read_panel_asof_opens_read_only` **fails on the pre-fix code**
(observes `read_only=False`) and passes after.

### S10-1 — Materialized latest cross-section + covering indexes (perf) — migrations 0199–0200

**Root cause:** a single-date PIT read replays a full-history `row_number()` window scan over all of
`v_factor_panel`; there is no per-`as_of` materialized cross-section and no covering index for the
latest-per-key lookup, so the flagship slice is O(history).

**Fix:** migration **0199** adds `factor_panel_latest_snapshot` — a materialized per-decision-date cross
section keyed `(universe_id, universe_version, snapshot_as_of_date, security_id, factor_id)` carrying
`value, available_at, source_loaded_at, run_id, input_lineage_json, built_at` — plus the `panel_serving_catalog`
control table; migration **0200** adds the covering index and the serving-catalog registration. The
**builder reuses `_read_panel_asof_active` verbatim** per date so snapshot rows equal `read_panel_asof(date)`
by construction. `served_cross_section(...)` reads the snapshot with an index range scan
(`WHERE snapshot_as_of_date = (SELECT max(snapshot_as_of_date) … <= :as_of)`), meeting the budget; on a
snapshot miss it falls back to the windowed view read (same rows). New `db/migrations/bodies_0199_0200.py`
is imported into `db/migrations/registry.py` (append `*_MIGRATIONS_0199_0200`).

```python
# db/migrations/bodies_0199_0200.py (real bodies — split schema/0199 from index/0200)
from __future__ import annotations
import duckdb
from ._runner import Migration
from .bodies_0001_0137 import _catalog_fields_for_tables
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin

def _pf4_s10_panel_serving_snapshot(conn: duckdb.DuckDBPyConnection) -> None:
    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS factor_panel_latest_snapshot (
            universe_id VARCHAR NOT NULL,
            universe_version VARCHAR NOT NULL,
            snapshot_as_of_date DATE NOT NULL,
            security_id VARCHAR NOT NULL,
            factor_id VARCHAR NOT NULL,
            value DOUBLE,
            available_at TIMESTAMP,
            source_loaded_at TIMESTAMP,
            run_id VARCHAR,
            input_lineage_json VARCHAR,
            built_at TIMESTAMP NOT NULL DEFAULT now(),
            PRIMARY KEY (universe_id, universe_version, snapshot_as_of_date, security_id, factor_id)
        )
        """
    )
    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS panel_serving_catalog (
            served_object VARCHAR PRIMARY KEY,
            backend VARCHAR NOT NULL,
            source_object VARCHAR NOT NULL,
            snapshot_table VARCHAR,
            read_only BOOLEAN NOT NULL DEFAULT true,
            perf_budget_ms INTEGER,
            enabled BOOLEAN NOT NULL DEFAULT true,
            updated_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO table_catalog (
            table_name, layer, entity, grain, description, natural_key_json, pit_notes, updated_at
        ) VALUES
        ('factor_panel_latest_snapshot','serving','factor_panel_latest_snapshot',
         'universe_id,universe_version,snapshot_as_of_date,security_id,factor_id',
         'Materialized per-decision-date latest factor cross-section serving the read tier; rows equal read_panel_asof(snapshot_as_of_date).',
         '["universe_id","universe_version","snapshot_as_of_date","security_id","factor_id"]',
         'Derived by replaying the v_factor_panel windowed as-of read per decision date; introduces no new PIT visibility.', now()),
        ('panel_serving_catalog','control','panel_serving_catalog','served_object',
         'Registry of served panel surfaces, their backend (duckdb_readonly | pinned_release), snapshot table, and perf budget.',
         '["served_object"]','Control table; read_only=true asserts the served read path never takes a writer lock.', now())
        """
    )
    _catalog_fields_for_tables(conn, ("factor_panel_latest_snapshot", "panel_serving_catalog"))
    _refresh_schema_contract_v2_pin(conn)

def _pf4_s10_panel_serving_indexes(conn: duckdb.DuckDBPyConnection) -> None:
    conn.execute(
        "CREATE INDEX IF NOT EXISTS idx_factor_panel_latest_snapshot_read "
        "ON factor_panel_latest_snapshot(universe_id, universe_version, snapshot_as_of_date, security_id, factor_id)"
    )
    conn.execute(
        "CREATE INDEX IF NOT EXISTS idx_factor_panel_latest_snapshot_asof "
        "ON factor_panel_latest_snapshot(snapshot_as_of_date)"
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO panel_serving_catalog (
            served_object, backend, source_object, snapshot_table, read_only, perf_budget_ms, enabled, updated_at
        ) VALUES ('v_factor_panel','duckdb_readonly','v_factor_panel','factor_panel_latest_snapshot',true,50,true,now())
        """
    )
    _refresh_schema_contract_v2_pin(conn)

MIGRATIONS: list[Migration] = [
    Migration(version=199, name="pf4_s10_panel_serving_snapshot", up=_pf4_s10_panel_serving_snapshot),
    Migration(version=200, name="pf4_s10_panel_serving_indexes", up=_pf4_s10_panel_serving_indexes),
]
```

**PIT:** (A)/(I) snapshot rows are the windowed as-of read replayed per date; (B) 0199 schema/catalog split
from 0200 index/catalog; (D) rebuild is deterministic + idempotent.

**Test (TDD):**

```python
def test_snapshot_rows_equal_read_panel_asof(tmp_store):
    _insert_factor_value_fixtures(tmp_store)
    from db.panel_serving import build_panel_latest_snapshot, served_cross_section
    build_panel_latest_snapshot(tmp_store, as_of_dates=[AS_OF])
    served = served_cross_section(tmp_store, AS_OF)                 # index range scan on the snapshot
    windowed = read_panel_asof(AS_OF, store=tmp_store)             # full window scan on the view
    pd.testing.assert_frame_equal(
        served.reset_index(drop=True), windowed.reset_index(drop=True), check_like=False
    )

def test_served_read_uses_the_snapshot_index_not_a_full_scan(tmp_store):
    _insert_factor_value_fixtures(tmp_store)
    from db.panel_serving import build_panel_latest_snapshot, SERVED_CROSS_SECTION_SQL
    build_panel_latest_snapshot(tmp_store, as_of_dates=[AS_OF])
    plan = tmp_store.con.execute(
        "EXPLAIN " + SERVED_CROSS_SECTION_SQL, [AS_OF, AS_OF]
    ).fetchall()
    text = "\n".join(str(row[1]) for row in plan)
    assert "factor_panel_latest_snapshot" in text                  # hits the materialized cross-section
    assert "v_factor_panel" not in text                            # NOT the full-history window scan

def test_snapshot_build_is_idempotent(tmp_store):
    _insert_factor_value_fixtures(tmp_store)
    from db.panel_serving import build_panel_latest_snapshot
    build_panel_latest_snapshot(tmp_store, as_of_dates=[AS_OF])
    first = tmp_store.con.execute(
        "SELECT * FROM factor_panel_latest_snapshot ORDER BY security_id, factor_id"
    ).df().drop(columns=["built_at"])
    build_panel_latest_snapshot(tmp_store, as_of_dates=[AS_OF])     # rerun = no-op / same rows
    second = tmp_store.con.execute(
        "SELECT * FROM factor_panel_latest_snapshot ORDER BY security_id, factor_id"
    ).df().drop(columns=["built_at"])
    pd.testing.assert_frame_equal(first, second)
```

**Accept:** the served single-date read returns rows identical to `read_panel_asof` for the date;
`EXPLAIN` shows the plan reads `factor_panel_latest_snapshot` (the covering index) and **not** the full
`v_factor_panel` window scan — the budget is asserted on the **plan**, not wall-clock; the snapshot build is
deterministic and idempotent; migrations 0199/0200 apply cleanly and catalog every new table (drift check
green).

### S10-2 — Arrow result cache + pinned Parquet/Arrow release backend

**Root cause:** each read re-executes the query even for an identical key; there is no zero-copy Arrow
surface; and there is no way to serve a **pinned immutable release** (PF4-S7) without opening the live DB —
so even a frozen release read contends with the live warehouse lock.

**Fix:** `PanelServer.read_cross_section(as_of, *, universe_id=…, universe_version=…, factor_ids=None,
security_ids=None, release=None, as_arrow=False)` computes a cache key
`(as_of, universe_id, universe_version, tuple(sorted(factor_ids or ())), release)`; on hit it returns the
cached `pyarrow.Table` (zero-copy) — content identical to the miss path; on miss it runs the backend read and
stores the Arrow table (bounded LRU). The **`pinned_release` backend** reads the S7 release's Parquet/Arrow
via `read_parquet(?)` (lake.py:875 pattern) with **no DB open at all**, guaranteeing zero writer lock on that
path. Arrow is produced with DuckDB's `fetch_arrow_table()` on the read-only connection.

**PIT:** (K) a pinned release is immutable/content-addressed — the release backend never mutates it;
(L) release-backed rows equal the DB-backed rows for the same key; (D) cache hit returns identical content.

**Test (TDD):**

```python
def test_arrow_cache_hit_is_identical_and_runs_once(tmp_store, monkeypatch):
    _insert_factor_value_fixtures(tmp_store)
    from db.panel_serving import build_panel_latest_snapshot, PanelServer
    build_panel_latest_snapshot(tmp_store, as_of_dates=[AS_OF])
    server = PanelServer.from_store(tmp_store)          # test hook binding the server to an open read store
    calls = {"n": 0}
    inner = server._execute_cross_section
    monkeypatch.setattr(server, "_execute_cross_section",
                        lambda *a, **k: (calls.__setitem__("n", calls["n"] + 1), inner(*a, **k))[1])
    first = server.read_cross_section(AS_OF, as_arrow=True)
    second = server.read_cross_section(AS_OF, as_arrow=True)
    import pyarrow as pa
    assert isinstance(first, pa.Table) and first.equals(second)   # identical Arrow content
    assert calls["n"] == 1                                        # second call served from cache

def test_pinned_release_backend_takes_no_db_lock(tmp_store, tmp_path):
    # tmp_store deliberately stays OPEN (a live writer holds the DB) for the whole test.
    _insert_factor_value_fixtures(tmp_store)
    from db.panel_serving import build_panel_latest_snapshot, PanelServer
    build_panel_latest_snapshot(tmp_store, as_of_dates=[AS_OF])
    release_dir = tmp_path / "release"                            # a pinned Parquet/Arrow release (S7 shape)
    served = read_panel_asof(AS_OF, store=tmp_store)
    release_dir.mkdir()
    tmp_store.con.execute(
        "COPY (SELECT * FROM factor_panel_latest_snapshot) TO ? (FORMAT PARQUET)",
        [str(release_dir / "part-00000.parquet")],
    )
    server = PanelServer(tmp_store.path, backend="pinned_release", release=str(release_dir))
    try:
        frame = server.read_cross_section(AS_OF)                  # reads Parquet; NEVER opens the DB
    finally:
        server.close()
    assert not frame.empty                                        # succeeds while the writer still holds the DB
```

**Accept:** an Arrow cache hit returns byte-identical content and executes the underlying query exactly once;
the `pinned_release` backend serves a read from the frozen release **while a writable connection still holds
the DB**, taking no DB lock; release-backed rows equal DB-backed rows for the same key.

### S10-3 — Concurrency proof: N isolated readers, none blocks on a writer

**Root cause:** the sprint's core promise — multiple simultaneous PIT readers, isolated, none blocked on a
writer — is unproven until it is adversarially demonstrated.

**Fix:** prove three properties. (1) **N concurrent read-only readers are isolated**: spawn N threads each
reading the cross-section through the pool; all return identical rows with no error. (2) **No writer lock on
the read path**: the served read only ever opens `read_only=True` (asserted in S10-0) — additionally, N
independent `duckdb.connect(path, read_only=True)` handles coexist concurrently (read-only opens are
non-exclusive). (3) **A reader does not block on a writer**: the `pinned_release` backend serves reads while a
separate writable connection holds the DB (S10-2's lock test, run under thread fan-out here).

**PIT:** (A)/(I)/(L) every reader returns the identical PIT cross-section; (C) offline, file-backed
`tmp_store` copy — no network.

**Test (TDD):**

```python
def test_n_concurrent_readers_are_isolated(tmp_store):
    import threading
    _insert_factor_value_fixtures(tmp_store)
    from db.panel_serving import build_panel_latest_snapshot, served_cross_section
    build_panel_latest_snapshot(tmp_store, as_of_dates=[AS_OF])
    baseline = served_cross_section(tmp_store, AS_OF)
    results, errors = {}, {}
    def worker(i):
        try:
            cur = tmp_store.con.cursor()                          # concurrent read cursor; shared read
            results[i] = served_cross_section(tmp_store, AS_OF, cursor=cur)
        except Exception as exc:                                  # pragma: no cover
            errors[i] = exc
    threads = [threading.Thread(target=worker, args=(i,)) for i in range(8)]
    for t in threads: t.start()
    for t in threads: t.join()
    assert not errors
    for i in range(8):
        pd.testing.assert_frame_equal(
            results[i].reset_index(drop=True), baseline.reset_index(drop=True)
        )

def test_multiple_readonly_connections_coexist(tmp_store):
    import duckdb
    _insert_factor_value_fixtures(tmp_store)
    tmp_store.con.execute("CHECKPOINT")
    path = str(tmp_store.path)
    tmp_store.connection.close(); tmp_store.connection = None     # release writer
    conns = [duckdb.connect(path, read_only=True) for _ in range(6)]  # all succeed — non-exclusive
    try:
        counts = {c.execute("SELECT count(*) FROM v_factor_panel").fetchone()[0] for c in conns}
        assert len(counts) == 1                                   # every reader sees the identical panel
    finally:
        for c in conns: c.close()
```

**Accept:** N concurrent readers all return the identical PIT cross-section with zero errors; N read-only
connections coexist (proving read-only opens are non-exclusive and no reader takes a writer lock); the
release-backed reader (S10-2) succeeds while a writer holds the DB — none blocks on a writer.

### S10-4 — Close-out: describe/CLI surface + ledger + PARITY_GAP

**Root cause:** a served tier needs a documented, catalogued entry point, and the tranche must be recorded in
the parity ledgers per the standing process.

**Fix:** add `PanelServer.describe()` (backend, snapshot coverage, cache stats, `read_only=True`) and a thin
CLI `python -m db.panel_serving read --as-of … [--release …] [--arrow]` / `describe` / `build-snapshot`
mirroring `db/factor_panel.py::main`. Confirm the `panel_serving_catalog` row is the documented registry
surface. Then **append a `WAREHOUSE_PARITY_TRANCHES.md` row** and **update `db/PARITY_GAP.md`** noting the
served read tier + `read_only` hazard closure + materialized-cross-section perf surface.

**PIT:** (B) describe/CLI are read-only; (L) documented parity guarantee.

**Test (TDD):**

```python
def test_describe_reports_readonly_backend_and_snapshot(tmp_store):
    _insert_factor_value_fixtures(tmp_store)
    from db.panel_serving import build_panel_latest_snapshot, PanelServer
    build_panel_latest_snapshot(tmp_store, as_of_dates=[AS_OF])
    meta = PanelServer.from_store(tmp_store).describe()
    assert meta["read_only"] is True
    assert meta["backend"] == "duckdb_readonly"
    assert meta["snapshot_table"] == "factor_panel_latest_snapshot"
    assert meta["snapshot_as_of_dates"] >= 1

def test_cli_read_emits_the_pit_cross_section(tmp_store, capsys):
    _insert_factor_value_fixtures(tmp_store)
    from db.panel_serving import build_panel_latest_snapshot, main
    build_panel_latest_snapshot(tmp_store, as_of_dates=[AS_OF])
    tmp_store.con.execute("CHECKPOINT")
    tmp_store.connection.close(); tmp_store.connection = None
    rc = main(["--db-path", str(tmp_store.path), "read", "--as-of", AS_OF])
    assert rc == 0
    assert "security_id" in capsys.readouterr().out
```

**Accept:** `describe()` reports the read-only backend + snapshot coverage; the CLI reads a PIT slice from a
read-only open; the `WAREHOUSE_PARITY_TRANCHES.md` row (start/end SHA, domains, verification commands, live
smoke placeholder for operator run, caveats/next → PF4-S11) is appended and `db/PARITY_GAP.md` is updated;
`git add` names explicit paths (never `git add -A`).

---

## Sequencing & expected compounding

**S10-0 → S10-1 → S10-2 → S10-3 → S10-4.** S10-0 makes the read path read-only-isolated (the correctness
floor) and lands the `PanelServer` shell. S10-1 materializes the latest cross-section + covering indexes so
the served single-date read meets budget while remaining parity-identical (it must land after there is an
isolated reader to serve). S10-2 bolts the Arrow result cache + pinned-release backend onto that reader — the
zero-lock, zero-copy consumption surface. S10-3 then adversarially proves the concurrency promise on the
assembled tier. S10-4 documents + catalogs + records it. Compounding: this is the **serving face of the pf4
product** — the surface the PF4-S8 `atx-panel` SDK and PF4-S9 notebooks read through, and the tier the
PF4-S11 capstone exercises under concurrency. It closes the last read-path hazard (`read_only=False`) the
PF4-S3 review flagged and turns "the panel is queryable" into "the panel is **safely, quickly, concurrently**
queryable behind the SDK."

## Risks / guardrails

- **DuckDB is single-writer.** A read-write open (any process) is exclusive — read-only opens from other
  processes fail while a writer holds the file. The guarantee "readers never block on a writer" is therefore
  delivered by the **`pinned_release` backend** (reads immutable Parquet/Arrow, no DB lock) and by a
  **read-only snapshot** for the DB-backed path — **not** by claiming RO+RW coexistence on one live file.
  The plan and tests reflect this honestly (the writer-isolation proof runs through the release backend).
- **Parity is non-negotiable (clause L).** The served read must equal `read_panel_asof` bit-for-bit — the
  materializer **reuses `_read_panel_asof_active`**, never a reimplementation, so drift is structurally
  impossible. A parity test gates the tier.
- **Never perturb the panel contract.** Serving must not touch `PANEL_CONTRACT`, the export gate, or the
  panel `expected_schema_sha256`. `factor_panel_latest_snapshot` is a *serving derivative*, catalogued
  separately; it is not the exported panel object.
- **Perf is asserted on the plan, not the clock.** Budget conformance is proven via `EXPLAIN` (the snapshot
  index is used, the full window scan is not) or a call-count — never a wall-clock timing race.
- **Stay in lane.** Only migrations **0199–0200** in a new `bodies_0199_0200.py` wired into `registry.py`;
  DB+WAL backup before any live apply; do not edit `v_factor_panel`, `db/factors/`, `db/signal_eval.py`,
  `db/panel_release.py`, or `clients/atx-panel/`.

## Bench / acceptance

- `read_panel_asof` / `describe_factor_panel` open `read_only=True`; the `PanelServer` pool opens **only**
  read-only handles; a writable open on the read path raises.
- `factor_panel_latest_snapshot` + covering indexes land via 0199/0200, catalogued (drift check green); the
  served single-date read returns rows **identical** to `read_panel_asof` and the plan reads the snapshot
  index, not the full `v_factor_panel` window scan.
- The Arrow cache returns byte-identical content on a hit and executes the query once; the `pinned_release`
  backend serves a read from a frozen release while a writer holds the DB (no DB lock).
- N concurrent readers return the identical PIT cross-section with zero errors; N read-only connections
  coexist; the release-backed reader does not block on a writer.
- `python -m pytest atx-impl\db\tests\test_panel_serving.py -q` green, and full
  `python -m pytest atx-impl\db\tests -q` green before commit — **run from `atx-impl/`, never from `db/`**.
- **Live proof-slice smoke** recorded in the ledger (operator-gated): snapshot row count + distinct
  `snapshot_as_of_date`, a served single-date row count == `read_panel_asof` row count, the served EXPLAIN
  plan line, and the concurrency reader count.
- `db/PARITY_GAP.md` updated (read tier isolated + `read_only=False` hazard closed + materialized
  cross-section perf surface); a `WAREHOUSE_PARITY_TRANCHES.md` row appended (start/end SHA, domains,
  verification commands, live smoke with exact counts, caveats/next → PF4-S11 capstone).

**Process:** own git worktree off the integration mainline, merged at sprint end via
`atx-impl/scripts/new_db_worktree.sh new|finish pf4-s10`; controller `superpowers:subagent-driven-development`
(fresh implementer + reviewer per task; TDD + verification-before-completion). Offline tests only (no network;
file-backed `tmp_store` template copy). Never `git add -A` (stage explicit paths); never push unless asked.
Commit trailer EXACTLY `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`.
