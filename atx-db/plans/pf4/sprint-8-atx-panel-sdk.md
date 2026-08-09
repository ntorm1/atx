# Sprint PF4-S8 — `atx-panel` Python SDK client (standalone, read-only, view-parity)

**Track:** Product (Track C). **Reserved migration:** `0198` (client-version/compat registry row only, if any —
see S8-4). **Sequential after PF4-S7** (release engine) and **PF4-S5** (multi-universe filter); shares the
panel export/read/serve path with PF4-S7/S10 and must not run a concurrent live worktree with them.

**Goal:** ship a thin, installable, **standalone** client — the **primary quant consumption interface** — that a
downstream team `pip install atx-panel`s and uses to pull a **point-in-time, lookahead-tested factor panel**
(pandas **or** zero-copy Arrow), with **NO requirement to import any internal `db.*` module**. The client wraps
the **REAL** read path: it either (a) opens the warehouse DB/lake with an **ISOLATED READ-ONLY** DuckDB
connection (`read_only=True`) and issues the *same contracted `v_factor_panel` read* as
`db.factor_panel.read_panel_asof`, or (b) reads directly from a **pinned immutable Parquet/Arrow release**
(PF4-S7) with **no DB at all**. This sprint is where clause **(L) client/view parity** stops being a design
promise and becomes an **enforced, gated boundary**: the SDK read returns rows **bit-for-bit identical** to the
contracted view read for the same `(as_of, universe, factors, release)`, and a cross-tree parity test
(`db/tests/test_sdk_parity.py`) gates the client. The client never takes a writer lock (fixing the
`read_only=False` hazard `db.factor_panel.read_panel_asof` carries today — see S8-1 root cause).

**Mandate / Owns:** NEW top-level package `clients/atx-panel/` with its **own** `pyproject.toml`
(distribution name `atx-panel`, import name `atx_panel`), a typed public API with docstrings
(`read_panel`, `read_panel_arrow`, `factors`, `factor_meta`, `signal`, `releases`, `PanelClient`), the isolated
read-only reader, the DB-less release reader, the metadata/data-dictionary + signal-lookup surfaces, the
client's own `clients/atx-panel/tests/`, and the cross-tree parity gate `db/tests/test_sdk_parity.py`.
Migration **0198** only if a client-version/compat registry row is needed (S8-4).

**Must NOT touch:** the panel content or its contract — the SDK **consumes** `v_factor_panel` and **never**
redefines a factor, mutates the export, or perturbs `schema_sha256`/`PANEL_CONTRACT_SHA256`; the PF4-S7 release
engine (`db/panel_release.py`) — the SDK **reads** a published release, it does not publish/prune one (clause
K: a pinned release is immutable); `db/factor_panel.py`'s writer path and the S10 served tier
(`db/panel_serving.py`); any landed migration (≤ `0197`) or another sprint's reserved region. The SDK adds **no
runtime dependency** on the `db` package (an enforced boundary test, S8-0).

**Depends on:** PF4-S7 (`db/panel_release.py` — immutable semver `YYYY.MM.patch` releases with a release
manifest + per-file checksums + Arrow/Feather **and** Parquet — the release layout the SDK's `release=` reads),
PF4-S5 (`db/universe.py` multi-universe + version filter, and its extension of `read_panel_asof` with a
universe/version filter — the parity target for the SDK's `universe`/`version` args), PF4-S1
(`db/signal_eval.py` — the IC/decay surfaces `signal(factor_id)` looks up), PF3-S10 `db/factor_panel.py`
(`read_panel_asof`, `describe_factor_panel`, `FACTOR_PANEL_COLUMNS` — the contracted read the SDK mirrors), and
PF3-S2 `db/panel_contract.py` (`PANEL_CONTRACT`, `PANEL_CONTRACT_SHA256` — the column contract the SDK embeds a
verified copy of). **Reconcile to the landed names** where a dependency's exact symbol differs at implementation
time (per ROADMAP): if PF4-S5 named the universe filter parameters differently, or PF4-S7 named the release
manifest fields differently, the S8 implementer reconciles the SDK to the landed surface and keeps the parity
test as the guard.

---

## Baseline / where the cycles go

The panel is produced (PF3-S10), scored (PF4-S1), gated (PF4-S2), survivorship-safe (PF4-S4), multi-universe
(PF4-S5), and released immutably (PF4-S7) — but there is **no consumption surface a downstream quant can use
without cloning the repo and importing internal modules**. Measured 2026-07-06 against `atx-impl/db`.

1. **The only read path is an internal, non-exported function that opens the DB read-write.**
   `db.factor_panel.read_panel_asof(as_of_date, *, as_of_ts, db_path, store, factor_ids, security_ids, wide)`
   (`db/factor_panel.py:657`) is the documented consumer read — but `db.factor_panel` **is not exported from
   `db/__init__.py`** (confirmed: `factor_panel` appears in neither the imports nor `__all__`), so a consumer
   must reach into a private module. Worse, when no `store` is passed it opens the 14 GB DB with
   **`connect(db_path, read_only=False)`** (`db/factor_panel.py:681`) — every "read" takes a **writer lock**,
   so concurrent reads serialize and a reader can block or be blocked by a writer. There is no `pip install`-able
   client, no typed public API, no zero-copy Arrow entry point, and no way to read a **pinned release** without
   the live DB.

2. **`describe_factor_panel` exists but reads through the same read-write connection** (`db/factor_panel.py:738`
   opens `read_only=False`). Metadata/data-dictionary access is entangled with the writer path and, again, only
   reachable via the private module.

3. **The panel column contract is declared but not embeddable by an external package.**
   `db/panel_contract.py` holds `PANEL_CONTRACT` (8 `PanelColumnSpec` rows: `security_id`, `as_of_date`,
   `factor_id`, `value`, `available_at`, `source_loaded_at`, `run_id`, `input_lineage_json`) + a stable
   `PANEL_CONTRACT_SHA256`. A standalone client cannot import it (clause: no `db.*` dependency), so it must
   carry a **verified copy** and assert equality against the warehouse hash via the parity test.

4. **Releases (PF4-S7) have no reader.** PF4-S7 emits immutable `YYYY.MM.patch` releases (manifest + checksums +
   Parquet **and** Arrow/Feather), but nothing lets a consumer *pin and read* one offline, DB-free — the whole
   point of an immutable release for reproducibility.

**Already good — do not regress:**
- **The contracted read SQL in `db.factor_panel._read_panel_asof_active`** (`db/factor_panel.py:585`) — the
  `WITH params … eligible (as_of_date ≤ param AND available_at ≤ param_ts) … ranked (row_number OVER PARTITION
  BY security_id, factor_id ORDER BY as_of_date DESC, available_at DESC, source_loaded_at DESC) … WHERE rn = 1`
  latest-PIT-cross-section query is the **canonical** read. The SDK embeds this **verbatim** so its rows are
  bit-for-bit identical; the parity test is the guard that keeps them in lockstep.
- **`connect(path, *, read_only=False)`** (`db/connection.py:163`) already threads `read_only` straight to
  `duckdb.connect(..., read_only=read_only)`. The SDK opens **its own** `duckdb.connect(str(path),
  read_only=True)` (it does not import `db.connection`) — same primitive, no writer lock.
- **`PANEL_CONTRACT_SHA256`** (`db/panel_contract.py:91`) — the stable contract hash the SDK's embedded copy is
  checked against, so a contract drift on either side is caught.
- **`pivot_factor_panel_wide`** (`db/factor_panel.py:226`) — the `wide=True` pivot the SDK reproduces so wide
  output matches the view read.

---

## PIT / determinism + production contract

Clauses **(A)** bitemporal/no-lookahead, **(C)** offline tests, **(D)** determinism/provenance, **(E)**
schema-as-contract, **(I)** panel PIT-safety, **(K)** release immutability, and **(L)** client/view parity all
apply — and **(L) is ENFORCED by this sprint** at the client boundary.

- **(L)** The SDK read path returns rows **identical** to the contracted view read for the same
  `(as_of, universe, factors, release)`. `db/tests/test_sdk_parity.py` asserts
  `pd.testing.assert_frame_equal(atx_panel.read_panel(...), db.factor_panel.read_panel_asof(...))` (long **and**
  wide) and `read_panel_arrow(...).to_pandas()` equality; the client does not land unless this is green. This is
  the clause's first enforcement.
- **(A)/(I)** The SDK issues the **same** `available_at ≤ as_of_ts AND as_of_date ≤ as_of` PIT gate and the same
  as-of universe filter as the view — it introduces **zero** new lookahead surface; a planted future-dated row
  is absent from the SDK read exactly as from the view read.
- **(K)** A pinned release is read **read-only and checksum-verified**; the SDK never writes, mutates, or prunes
  a release directory. Re-reading a pinned release is a pure function of its immutable bytes.
- **(C)/(D)** Every test is offline: an in-memory/temp-file fixture DB and a fixture release directory, no
  network, no live DB. Same inputs + args → same rows + same Arrow schema.
- **(E)** The SDK embeds a copy of the panel column contract and asserts its hash equals the warehouse
  `PANEL_CONTRACT_SHA256`; a divergence fails a test.
- **(B)** Migration **0198** only if a client-version/compat registry row is needed (S8-4); if landed it seeds
  `table_catalog` + `field_catalog` in the same migration, schema/catalog + index together (tiny table),
  timestamped DB+WAL backup before any live apply. Otherwise **no migration**.

---

## Package layout (real, no placeholders)

```
clients/atx-panel/
  pyproject.toml            # distribution name "atx-panel", import name "atx_panel"
  README.md                 # quickstart: install → pin release → PIT cross-section (pandas + Arrow)
  src/atx_panel/
    __init__.py             # typed public API: read_panel, read_panel_arrow, factors, factor_meta,
                            #   signal, releases, PanelClient, PanelSource, FactorMeta, ReleaseInfo, __version__
    _contract.py            # embedded verified copy of the panel column contract + PANEL_CONTRACT_SHA256 guard
    _reader.py              # DB source: isolated read_only=True DuckDB conn + canonical v_factor_panel SQL
    _release.py             # release source: DB-less pinned Parquet/Arrow reader + manifest/checksum verify
    _metadata.py            # factors()/factor_meta(): data-dictionary access over panel_contract + catalog
    _signal.py              # signal(factor_id): IC/decay lookup over the PF4-S1 signal_eval surfaces
    __main__.py             # thin CLI: `python -m atx_panel read|describe|releases|factors|signal`
    py.typed                # PEP 561 typing marker (shipped in the wheel)
  tests/
    conftest.py             # builds a fixture DB (v_factor_panel + universe_membership + signal surface)
                            #   and a fixture release dir (Parquet + Arrow + manifest + checksums)
    test_public_api.py      # installable + typed API present + docstrings + NO db.* import
    test_read_panel.py      # read_panel long/wide over the isolated read-only DB source
    test_read_panel_arrow.py# zero-copy Arrow equivalence
    test_read_only_isolation.py  # read_only=True; concurrent reader does not block / no writer lock
    test_release_pinning.py # pinned release read (DB-less) + releases() + latest resolution + checksum verify
    test_metadata_signal.py # factors()/factor_meta()/signal() resolve; contract hash matches warehouse
db/tests/test_sdk_parity.py # CROSS-TREE clause-(L) gate: SDK rows == read_panel_asof rows, bit-for-bit
```

**`clients/atx-panel/pyproject.toml`** (real, PEP 621 + hatchling):

```toml
[build-system]
requires = ["hatchling>=1.25"]
build-backend = "hatchling.build"

[project]
name = "atx-panel"
version = "0.1.0"
description = "Standalone point-in-time factor-panel client for the atx warehouse (pandas + zero-copy Arrow, immutable release pinning)."
readme = "README.md"
requires-python = ">=3.10"
license = { text = "Proprietary" }
authors = [{ name = "atx" }]
keywords = ["quant", "factor", "panel", "point-in-time", "arrow", "duckdb"]
dependencies = [
  "duckdb>=0.10",
  "pandas>=2.0",
  "pyarrow>=15.0",
]

[project.optional-dependencies]
test = ["pytest>=8.0"]

[project.scripts]
atx-panel = "atx_panel.__main__:main"

[tool.hatch.build.targets.wheel]
packages = ["src/atx_panel"]
```

**Public API contract** (the typed surface the tests assert; every symbol carries a docstring):

```python
# src/atx_panel/__init__.py  (signatures — bodies land per task)
from __future__ import annotations
import datetime as dt
from dataclasses import dataclass
from pathlib import Path
import pandas as pd
import pyarrow as pa

__version__ = "0.1.0"

@dataclass(frozen=True)
class PanelSource:
    """Where a client reads from: a warehouse DB path, a release root, or both."""
    db_path: Path | None = None
    release_root: Path | None = None

@dataclass(frozen=True)
class FactorMeta:
    """Data-dictionary entry for one factor: id, unit, sign, scale, description, lineage, IC headline."""
    factor_id: str
    unit: str
    sign: str
    scale: str
    description: str | None
    ic_mean: float | None
    ic_decay_halflife: float | None

@dataclass(frozen=True)
class ReleaseInfo:
    """An immutable published panel release: semver, publish date, checksum, paths."""
    release: str            # "2026.07.0"
    published_at: str
    panel_contract_sha256: str
    schema_sha256: str
    path: Path

class PanelClient:
    """Thin, standalone, read-only client over the contracted factor panel.

    A client reads either an isolated read-only warehouse DB connection or a pinned
    immutable Parquet/Arrow release (no DB). It never takes a writer lock and never
    imports internal `db.*` modules.
    """
    def __init__(self, db_path: Path | str | None = None,
                 release_root: Path | str | None = None) -> None: ...
    def read_panel(self, as_of, *, universe=None, version=None, factors=None,
                   release=None, as_of_ts=None, wide=False) -> pd.DataFrame: ...
    def read_panel_arrow(self, as_of, *, universe=None, version=None, factors=None,
                         release=None, as_of_ts=None, wide=False) -> pa.Table: ...
    def factors(self) -> pd.DataFrame: ...
    def factor_meta(self, factor_id: str) -> FactorMeta: ...
    def signal(self, factor_id: str) -> pd.DataFrame: ...
    def releases(self) -> list[ReleaseInfo]: ...

# Module-level convenience functions bind to a default client resolved from
# ATX_PANEL_DB / ATX_PANEL_RELEASE_ROOT env vars (or explicit source= kwargs).
def read_panel(as_of, *, universe=None, version=None, factors=None, release=None,
               as_of_ts=None, wide=False, source: PanelSource | None = None) -> pd.DataFrame: ...
def read_panel_arrow(as_of, *, universe=None, version=None, factors=None, release=None,
                     as_of_ts=None, wide=False, source: PanelSource | None = None) -> pa.Table: ...
def factors(*, source: PanelSource | None = None) -> pd.DataFrame: ...
def factor_meta(factor_id: str, *, source: PanelSource | None = None) -> FactorMeta: ...
def signal(factor_id: str, *, source: PanelSource | None = None) -> pd.DataFrame: ...
def releases(*, source: PanelSource | None = None) -> list[ReleaseInfo]: ...
```

---

## Tasks

### S8-0 — Standalone package scaffold + typed public API + no-`db.*`-dependency boundary

**Root cause:** there is no installable client at all; the only read path is a non-exported internal function
(`db.factor_panel` is absent from `db/__init__.py`), so a consumer must clone the repo and import a private
module. A downstream quant cannot `pip install` anything.

**Fix (TDD — write the tests first):** create `clients/atx-panel/` with the **real** `pyproject.toml` above,
`src/atx_panel/` with `py.typed`, and the typed public API skeleton (every function/class present with a real
docstring; bodies may `raise NotImplementedError` **only** in this scaffolding step — subsequent tasks fill
them and remove the stubs). Establish the **enforced boundary**: `atx_panel` imports **no** `db.*` module.

Write `clients/atx-panel/tests/test_public_api.py` first (fails until the package exists):

```python
import importlib
import inspect
import sys
import atx_panel

PUBLIC = ["read_panel", "read_panel_arrow", "factors", "factor_meta", "signal", "releases",
          "PanelClient", "PanelSource", "FactorMeta", "ReleaseInfo", "__version__"]

def test_public_api_symbols_present():
    for name in PUBLIC:
        assert hasattr(atx_panel, name), f"missing public symbol: {name}"

def test_public_callables_have_docstrings():
    for name in ["read_panel", "read_panel_arrow", "factors", "factor_meta", "signal", "releases"]:
        obj = getattr(atx_panel, name)
        assert (obj.__doc__ or "").strip(), f"{name} must have a docstring"
    assert (atx_panel.PanelClient.__doc__ or "").strip()

def test_client_imports_no_internal_db_module():
    # The whole point of a standalone client: it must not depend on the warehouse package.
    importlib.reload(atx_panel)
    leaked = [m for m in sys.modules if m == "db" or m.startswith("db.")]
    assert not leaked, f"atx_panel must not import internal db.* modules; leaked: {leaked}"

def test_read_panel_signature_is_typed_and_keyword_only():
    sig = inspect.signature(atx_panel.PanelClient.read_panel)
    params = sig.parameters
    for kw in ["universe", "version", "factors", "release", "as_of_ts", "wide"]:
        assert params[kw].kind is inspect.Parameter.KEYWORD_ONLY, f"{kw} must be keyword-only"
    assert params["wide"].default is False
```

Also verify the package **builds/installs**: `python -m pip install -e clients/atx-panel` succeeds and
`python -c "import atx_panel; print(atx_panel.__version__)"` prints `0.1.0` (recorded in the ledger; the offline
suite asserts import + API only, no network).

**PIT:** (C) offline; (E) `py.typed` shipped so the typed contract is consumable. No migration.

**Accept:** the package installs standalone; all public symbols present with docstrings; `read_panel`'s
`universe/version/factors/release/as_of_ts/wide` are keyword-only with `wide` defaulting `False`; the
no-`db.*`-import boundary test is green.

### S8-1 — `read_panel` over an ISOLATED READ-ONLY DB connection + clause-(L) view parity

**Root cause:** `read_panel_asof` opens the DB `read_only=False` (a writer lock on every read) and lives in a
non-exported private module. The SDK must read **without** a writer lock and return rows **bit-for-bit
identical** to the contracted view read.

**Fix (TDD):** implement `_reader.py` — open **the SDK's own** `duckdb.connect(str(db_path),
read_only=True)` (the SDK does **not** import `db.connection`) and issue the **canonical** `v_factor_panel`
read, embedded **verbatim** from `db.factor_panel._read_panel_asof_active`: the
`params → eligible(as_of_date ≤ as_of AND available_at ≤ as_of_ts) → ranked(row_number OVER PARTITION BY
security_id, factor_id ORDER BY as_of_date DESC, available_at DESC, source_loaded_at DESC) → WHERE rn = 1`
query, with optional `factor_id`/`security_id` registered-frame joins and the `wide` pivot reproducing
`pivot_factor_panel_wide`. Map `universe`/`version` to the PF4-S5 universe/version filter (reconcile to the
landed parameter names). `read_panel` returns exactly `READ_PANEL_COLUMNS` order for long, and the wide pivot
for `wide=True`.

Write `clients/atx-panel/tests/test_read_panel.py` first (against the `conftest.py` fixture DB):

```python
import pandas as pd
import atx_panel

def test_read_panel_long_returns_pit_cross_section(fixture_db):
    client = atx_panel.PanelClient(db_path=fixture_db)
    frame = client.read_panel("2026-03-31")
    assert list(frame.columns) == ["security_id", "as_of_date", "factor_id", "value",
                                   "available_at", "source_loaded_at", "run_id", "input_lineage_json"]
    # a future-dated input (available_at after as_of) must be absent (clause A/I)
    assert not ((pd.to_datetime(frame["available_at"]).dt.date > frame["as_of_date"]).any())

def test_read_panel_wide_is_pivot_of_long(fixture_db):
    client = atx_panel.PanelClient(db_path=fixture_db)
    long = client.read_panel("2026-03-31")
    wide = client.read_panel("2026-03-31", wide=True)
    assert set(["security_id", "as_of_date"]).issubset(wide.columns)
    assert len(wide) == long[["security_id", "as_of_date"]].drop_duplicates().shape[0]

def test_factor_filter_narrows_rows(fixture_db):
    client = atx_panel.PanelClient(db_path=fixture_db)
    one = client.read_panel("2026-03-31", factors=["f_value_ep"])
    assert set(one["factor_id"].unique()) == {"f_value_ep"}
```

And the **cross-tree clause-(L) gate** `db/tests/test_sdk_parity.py` (imports both trees; the SDK is on
`sys.path` via the editable install / a path insert):

```python
import pandas as pd
import pytest
from db import factor_panel                    # internal contracted read
import atx_panel                                # standalone client

@pytest.mark.parametrize("wide", [False, True])
def test_sdk_read_matches_view_read_bitforbit(sdk_parity_db, wide):
    db_path = sdk_parity_db                      # fixture DB with v_factor_panel populated
    expected = factor_panel.read_panel_asof("2026-03-31", db_path=db_path, wide=wide)
    got = atx_panel.PanelClient(db_path=db_path).read_panel("2026-03-31", wide=wide)
    pd.testing.assert_frame_equal(
        got.reset_index(drop=True), expected.reset_index(drop=True), check_dtype=True,
    )

def test_sdk_read_matches_view_read_with_filters(sdk_parity_db):
    db_path = sdk_parity_db
    kw = dict(factor_ids=["f_value_ep"], security_ids=["SEC_A"])
    expected = factor_panel.read_panel_asof("2026-03-31", db_path=db_path, **kw)
    got = atx_panel.PanelClient(db_path=db_path).read_panel(
        "2026-03-31", factors=["f_value_ep"], security_ids=["SEC_A"])
    pd.testing.assert_frame_equal(got.reset_index(drop=True), expected.reset_index(drop=True))
```

**PIT:** (L) bit-for-bit parity gated; (A)/(I) same PIT + as-of universe gate as the view; (D) deterministic
same-inputs→same-rows.

**Accept:** `read_panel` returns the PIT cross-section over a `read_only=True` connection; the parity gate is
green for long, wide, and filtered reads; the SDK issues no `read_only=False` connection anywhere (asserted in
S8-2).

### S8-2 — `read_panel_arrow` (zero-copy) + read-only isolation (concurrent reads never block)

**Root cause:** there is no zero-copy columnar entry point, and the internal read takes a writer lock, so
concurrent consumers serialize and a reader can be blocked by a writer.

**Fix (TDD):** implement `read_panel_arrow` returning a `pyarrow.Table` via DuckDB's zero-copy
`con.execute(sql, params).arrow()` over the **same** canonical query — so `read_panel_arrow(...).to_pandas()`
equals `read_panel(...)`. Assert the SDK opens **only** `read_only=True` connections (no writer lock), and prove
two concurrent SDK reads run without blocking.

Write `clients/atx-panel/tests/test_read_panel_arrow.py` and `test_read_only_isolation.py` first:

```python
# test_read_panel_arrow.py
import pyarrow as pa
import atx_panel

def test_read_panel_arrow_equals_pandas(fixture_db):
    client = atx_panel.PanelClient(db_path=fixture_db)
    table = client.read_panel_arrow("2026-03-31")
    assert isinstance(table, pa.Table)
    frame = client.read_panel("2026-03-31")
    # zero-copy Arrow round-trips to the identical pandas frame
    assert table.to_pandas().reset_index(drop=True).equals(frame.reset_index(drop=True))
    assert table.column_names[:4] == ["security_id", "as_of_date", "factor_id", "value"]
```

```python
# test_read_only_isolation.py
import duckdb
import atx_panel
from atx_panel import _reader

def test_sdk_only_opens_read_only(monkeypatch, fixture_db):
    seen = []
    real_connect = duckdb.connect
    def spy(path, *a, **kw):
        seen.append(kw.get("read_only", False))
        return real_connect(path, *a, **kw)
    monkeypatch.setattr(_reader.duckdb, "connect", spy)
    atx_panel.PanelClient(db_path=fixture_db).read_panel("2026-03-31")
    assert seen and all(flag is True for flag in seen), f"SDK must open read_only=True only; saw {seen}"

def test_concurrent_readonly_reads_do_not_block(fixture_db):
    # Two independent read-only handles on the same DB file succeed concurrently;
    # a read-only connection takes no writer lock, so a concurrent writer would not be blocked.
    a = atx_panel.PanelClient(db_path=fixture_db)
    b = atx_panel.PanelClient(db_path=fixture_db)
    fa = a.read_panel("2026-03-31")
    fb = b.read_panel("2026-03-31")
    assert fa.equals(fb)
```

**PIT:** (L) Arrow read equals pandas read equals view read; (C) offline; no writer lock taken.

**Accept:** `read_panel_arrow` returns a `pyarrow.Table` whose `to_pandas()` equals `read_panel`; the SDK opens
`read_only=True` **only** (spy test green); two concurrent SDK reads succeed and return equal frames.

### S8-3 — Release pinning: DB-less pinned Parquet/Arrow read + `releases()` + latest resolution

**Root cause:** PF4-S7 publishes immutable `YYYY.MM.patch` releases (manifest + checksums + Parquet + Arrow),
but nothing lets a consumer **pin and read** one — the reproducibility guarantee an immutable release exists
for. And a consumer may not have (or want to open) the live 14 GB DB.

**Fix (TDD):** implement `_release.py` — read a pinned release **with no DB**: resolve `release="2026.07.0"`
against `release_root` (unset → the **latest published** release from the manifest index), load the release's
Parquet (or Arrow/Feather) partitions with `pyarrow`, **verify the per-file checksums** against the PF4-S7
release manifest (clause K — refuse a tampered/mutated release), then apply the same in-frame as-of/universe/
factor filters so a release read returns the **same rows** as the DB read of the same content. `releases()`
enumerates `ReleaseInfo` from the manifest index. Reconcile the manifest field names to PF4-S7's landed
`db/panel_release.py` schema.

Write `clients/atx-panel/tests/test_release_pinning.py` first (against the `conftest.py` fixture release dir —
two releases `2026.06.0` and `2026.07.0` with distinct content + a manifest + checksums):

```python
import atx_panel

def test_releases_enumerates_published(fixture_release_root):
    client = atx_panel.PanelClient(release_root=fixture_release_root)
    rels = {r.release for r in client.releases()}
    assert {"2026.06.0", "2026.07.0"} <= rels

def test_pinned_release_returns_that_snapshot(fixture_release_root):
    client = atx_panel.PanelClient(release_root=fixture_release_root)
    older = client.read_panel("2026-06-30", release="2026.06.0")
    newer = client.read_panel("2026-06-30", release="2026.07.0")
    # the pinned older release must NOT reflect the newer release's added rows
    assert older["factor_id"].nunique() < newer["factor_id"].nunique()

def test_unpinned_resolves_to_latest(fixture_release_root):
    client = atx_panel.PanelClient(release_root=fixture_release_root)
    latest = client.read_panel("2026-06-30")               # no release= → latest published
    pinned = client.read_panel("2026-06-30", release="2026.07.0")
    assert latest.reset_index(drop=True).equals(pinned.reset_index(drop=True))

def test_read_from_release_needs_no_db(fixture_release_root):
    # release_root only, db_path=None → a full read with no DuckDB file present
    client = atx_panel.PanelClient(release_root=fixture_release_root)
    frame = client.read_panel("2026-06-30", release="2026.07.0")
    assert not frame.empty

def test_tampered_release_checksum_is_rejected(tampered_release_root):
    client = atx_panel.PanelClient(release_root=tampered_release_root)
    import pytest
    with pytest.raises(Exception):
        client.read_panel("2026-06-30", release="2026.07.0")
```

**PIT:** (K) pinned release immutable + checksum-verified, never mutated by the reader; (L) release read rows ==
DB read rows for the same content; (C) fully offline, DB-free.

**Accept:** `releases()` lists published releases; a pinned release returns its immutable snapshot; unset
resolves to latest; a release read succeeds with **no DB**; a tampered/checksum-mismatched release is rejected.

### S8-4 — Factor metadata / data dictionary + `signal(factor_id)` IC-decay lookup + migration 0198 (if any)

**Root cause:** a consumer needs the **data dictionary** (unit/sign/scale/description/lineage per factor) and
each factor's **signal** headline (IC / IC-decay from PF4-S1) without importing internal modules — today both
are only reachable via `db.*`.

**Fix (TDD):** implement `_metadata.py` and `_signal.py`. `factors()` returns a DataFrame of factor entries and
`factor_meta(factor_id)` a `FactorMeta`, sourced from the panel's `factor_id` universe joined to the panel
column contract (unit/sign/scale) and any catalogued factor descriptions — read over the **read-only** DB
source **or** derived from a release's manifest metadata (DB-free). `_contract.py` carries the **embedded
verified copy** of the panel column contract and asserts its hash equals the warehouse `PANEL_CONTRACT_SHA256`.
`signal(factor_id)` looks up the per-factor **rank-IC / IC-decay** rows from the PF4-S1 `signal_eval` surface
(reconcile to its landed table/view name), read-only. **Migration 0198** — land a minimal, catalogued
`atx_panel_client_registry(client_name, client_version, min_release, max_release, panel_contract_sha256,
registered_at, available_at)` with `table_catalog` + `field_catalog` rows and one seed row pinning `atx-panel`
`0.1.0` to `PANEL_CONTRACT_SHA256` **only if** a client/release compatibility registry is wanted; otherwise
land **no migration** and document that decision. The SDK's `releases()`/`factor_meta()` may consult the
registry for compat when present but must not require it.

Write `clients/atx-panel/tests/test_metadata_signal.py` first:

```python
import atx_panel

def test_factors_lists_panel_factors(fixture_db):
    client = atx_panel.PanelClient(db_path=fixture_db)
    df = client.factors()
    assert "factor_id" in df.columns and "unit" in df.columns and "sign" in df.columns
    assert "f_value_ep" in set(df["factor_id"])

def test_factor_meta_resolves(fixture_db):
    meta = atx_panel.PanelClient(db_path=fixture_db).factor_meta("f_value_ep")
    assert meta.factor_id == "f_value_ep"
    assert meta.sign in {"signed", "bounded", "positive", "non_negative"}

def test_signal_returns_ic_decay(fixture_db):
    sig = atx_panel.PanelClient(db_path=fixture_db).signal("f_value_ep")
    assert {"horizon", "rank_ic"}.issubset(sig.columns)   # IC per horizon ladder
    assert len(sig) >= 1

def test_embedded_contract_hash_matches_warehouse(fixture_db):
    from atx_panel import _contract
    assert _contract.PANEL_CONTRACT_SHA256 == _contract.warehouse_contract_sha256(fixture_db)
```

And, if migration 0198 lands, add to `db/tests/test_sdk_parity.py`:

```python
def test_client_registry_seed_pins_contract(sdk_parity_db):
    from db.connection import connect
    from db.panel_contract import PANEL_CONTRACT_SHA256
    with connect(sdk_parity_db, read_only=True) as store:
        row = store.con.execute(
            "SELECT client_name, client_version, panel_contract_sha256 "
            "FROM atx_panel_client_registry WHERE client_name = 'atx-panel'").fetchone()
    assert row is not None and row[1] == "0.1.0" and row[2] == PANEL_CONTRACT_SHA256
```

**PIT:** (E) embedded contract hash == warehouse hash; (A) signal lookup is a read-only join, no lookahead
introduced; (B) 0198 (if landed) catalogued + seeded in-migration, backup before any live apply.

**Accept:** `factors()`/`factor_meta()` resolve unit/sign/scale + description; `signal(factor_id)` returns IC per
horizon + decay; the embedded contract hash matches the warehouse `PANEL_CONTRACT_SHA256`; if 0198 lands, the
client-registry seed pins `atx-panel 0.1.0` to the contract hash and is catalogued.

### S8-5 — Cross-tree parity gate wiring, CLI, README quickstart + closeout (ledger + PARITY_GAP)

**Root cause:** the clause-(L) gate must run inside the warehouse offline suite (so the client can never drift
from the view), the client needs a runnable CLI + quickstart, and the sprint must record its parity closure.

**Fix (TDD):** finalize `db/tests/test_sdk_parity.py` so it is collected by `python -m pytest atx-impl\db\tests
-q` (put the client `src/` on `sys.path` in its `conftest`/a path shim, or rely on the editable install — the
test **skips with a clear reason** only if `atx_panel` is not importable, and the CI/operator note documents the
editable install so it is **not** skipped in the gate run). Implement `__main__.py`: `python -m atx_panel
read --as-of … [--wide] [--release …] [--factor-id …]`, `describe`, `releases`, `factors`, `signal
--factor-id …` — CSV/JSON to stdout, mirroring the `db.factor_panel` CLI shape. Write `README.md` with the
install → pin release → PIT cross-section (pandas + Arrow) quickstart. Then **closeout**: append a
`WAREHOUSE_PARITY_TRANCHES.md` row (start/end SHA, domains = `clients/atx-panel` + `db/tests/test_sdk_parity.py`
[+ migration `0198` if landed], verification commands, offline-proven / OPERATOR-PENDING live note, caveats/next
→ PF4-S9 docs) and update `db/PARITY_GAP.md` (clause L now enforced at the client boundary; the standalone SDK
consumption surface landed). Never `git add -A` (stage explicit paths).

Write the CLI test first:

```python
# clients/atx-panel/tests/test_public_api.py (append)
import subprocess, sys
def test_cli_read_runs(fixture_db, monkeypatch):
    monkeypatch.setenv("ATX_PANEL_DB", str(fixture_db))
    out = subprocess.run([sys.executable, "-m", "atx_panel", "read", "--as-of", "2026-03-31"],
                         capture_output=True, text=True)
    assert out.returncode == 0 and "security_id" in out.stdout
```

**PIT:** (L) the gate is collected in the warehouse suite; (C) offline; (D) deterministic CSV output.

**Accept:** `db/tests/test_sdk_parity.py` is collected and green in `python -m pytest atx-impl\db\tests -q`
(not silently skipped in the gate run); the CLI reads a slice; the README quickstart runs offline; the ledger
row + `PARITY_GAP.md` update land; full offline suite green before commit.

---

## Sequencing & expected compounding

**S8-0 → S8-1 → S8-2 → S8-3 → S8-4 → S8-5.** S8-0 lays the installable package + typed API + the
no-`db.*`-dependency boundary (the standalone contract). S8-1 fills the core read over an **isolated read-only**
connection and stands up the **clause-(L) bit-for-bit parity gate** — the load-bearing deliverable; it must
land before Arrow/release/metadata build on it. S8-2 adds zero-copy Arrow and proves read-only isolation
(no writer lock). S8-3 adds DB-less **release pinning** (the reproducibility surface over PF4-S7's immutable
releases). S8-4 adds the data dictionary + signal lookups (and the optional 0198 compat registry). S8-5 wires
the gate into the warehouse suite, ships the CLI/README, and records closure. **Compounding:** the SDK is the
**primary quant consumption interface** the north star promises (`pip install atx-panel` → pin a release → pull
a PIT panel, pandas or zero-copy Arrow) — and it is the surface **PF4-S9** documents (dictionary + notebooks)
and **PF4-S10** fronts with a served read tier. Its parity gate makes "the SDK read equals the view read"
a permanently enforced invariant, not a one-time claim.

## Risks / guardrails

- **The parity gate is the whole point — it must be bit-for-bit and adversarial.** Use
  `pd.testing.assert_frame_equal(check_dtype=True)`, cover long/wide/filtered/universe-filtered, and keep the
  canonical SQL a **verbatim** copy so any drift in `db.factor_panel` trips the gate. A permissive parity test
  is worse than none — downstream trusts the SDK blindly.
- **Never take a writer lock.** The SDK opens `read_only=True` **only** (spy-tested). Do **not** import or reuse
  `db.factor_panel.read_panel_asof`'s `read_only=False` path; the S10 served tier and S3 hardening fix that
  hazard warehouse-side, but the SDK must be safe independently.
- **No `db.*` runtime dependency.** The package is standalone; the boundary test fails if any `db`/`db.*` module
  is imported. The embedded contract copy + hash assertion is how the SDK stays honest without importing the
  contract module.
- **A pinned release is immutable (clause K).** The release reader is read-only + checksum-verifying; it never
  writes, prunes, or mutates a release dir, and it rejects a checksum mismatch.
- **Reconcile to landed names.** PF4-S5's universe/version filter params and PF4-S7's release-manifest fields
  are the parity/reader targets — bind to their **landed** symbols; the parity test guards the binding.
- **Stay in lane.** Do not touch `db/factors/`, the panel contract definition, `db/panel_release.py`
  (publisher), or `db/panel_serving.py` (S10). Migration **0198** only, and only if a client-compat registry is
  wanted; DB+WAL backup before any live apply.

## Bench / acceptance

- `clients/atx-panel/` installs standalone (`python -m pip install -e clients/atx-panel`;
  `import atx_panel` prints `0.1.0`); the typed public API is present with docstrings and imports **no** `db.*`.
- `read_panel(as_of, universe, version, factors, release, as_of_ts, wide)` returns rows **bit-for-bit
  identical** to `db.factor_panel.read_panel_asof(...)` for the same args (long **and** wide, filtered) —
  `db/tests/test_sdk_parity.py` green.
- `read_panel_arrow(...)` returns a `pyarrow.Table` equal to `read_panel(...)` on `to_pandas()`.
- Reads are **read-only isolated**: the SDK opens `read_only=True` only (spy-tested); concurrent SDK reads do
  not block and return equal frames.
- Release pinning: `read_panel(..., release="2026.07.0")` returns the pinned immutable snapshot; unset resolves
  to latest; a release read succeeds with **no DB**; a tampered/checksum-mismatched release is rejected.
- `factors()`/`factor_meta()` resolve unit/sign/scale + description; `signal(factor_id)` returns IC per horizon
  + decay; the embedded contract hash equals the warehouse `PANEL_CONTRACT_SHA256`.
- Migration **0198** lands **only if** a client-compat registry is needed; if landed it is catalogued + seeded.
- `python -m pytest clients\atx-panel\tests -q` green, and full `python -m pytest atx-impl\db\tests -q` green
  (with `db/tests/test_sdk_parity.py` collected, not silently skipped) before commit — **run from `atx-impl/`**,
  never from `db/`.
- **Live proof-slice smoke** recorded in the ledger (OPERATOR-PENDING per scope decision #1): SDK vs view
  row-count equality on a live cross-section, the pinned release id read, and the `read_only=True` connection
  confirmation.
- `db/PARITY_GAP.md` updated (clause L enforced at the client boundary; standalone SDK consumption surface
  landed); a `WAREHOUSE_PARITY_TRANCHES.md` row appended (start/end SHA, domains, verification commands, live
  smoke note, caveats/next → PF4-S9 data-dictionary + docs + notebooks).

**Process:** own git worktree off the integration mainline, merged at sprint end via
`atx-impl/scripts/new_db_worktree.sh new|finish pf4-s8`; sequential after PF4-S7 and PF4-S5, no concurrent live
worktree with PF4-S7/S10 (shared panel export/read/serve path). Controller
`superpowers:subagent-driven-development` (fresh implementer + reviewer per task; TDD +
verification-before-completion). New module ⇒ new `test_*.py`. Never `git add -A` (stage explicit paths); never
push unless asked. Commit trailer EXACTLY
`Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`.
