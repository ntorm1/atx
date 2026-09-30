# Lake platform: registry, verify, catalog, orchestrator

`atx_db.stagelake` (tier1-v3 S1) turns the alpha_panel stage directories under `data/alpha_panel/v1` (override
`ATX_ALPHA_PANEL_ROOT`) into one registered, checkable, queryable lake (ruling D1: the Parquet lake is the system of
record; `data/catalog.duckdb` is the rebuildable serving layer). Stage code keeps using `alpha_panel.common`
(`stage_dir`, `connect`, `copy_to_parquet`, `write_stage_manifest`).

```powershell
cd C:\atx\atx-db; $env:PYTHONPATH = "C:\atx\atx-db\src"
.venv\Scripts\python.exe -m atx_db.stagelake list                          # registry
.venv\Scripts\python.exe -m atx_db.stagelake verify [--stage S] [--no-hash] [--json out.json]
.venv\Scripts\python.exe -m atx_db.stagelake plan   [--only a,b | --from S]   # = orchestrate --dry-run
..\.superpowers\sdd\tier1-parity\run_memory_guarded.py --job-gb 0.4 --wait-minutes 60 -- `
    .venv\Scripts\python.exe -m atx_db.stagelake catalog [--dump]           # DuckDB: always guarded
..\.superpowers\sdd\tier1-parity\run_memory_guarded.py --job-gb 0.2 --allow-nested-guards --wait-minutes 60 -- `
    .venv\Scripts\python.exe -m atx_db.stagelake run [--fetch] [--max-gb 0.8]  # each stage in its own guard
```

`verify`, `plan` and the parity scorecard read files with pyarrow/hashlib only (no DuckDB); their measured peaks are
0.03-0.05 GiB. `catalog` and `run` open DuckDB and always go through the guard.

## Stage contract

A stage publishes files under the build root and one manifest last (`<stage>/manifest.json`, or a named manifest in
a shared directory such as `identity/link_table_manifest.json`). The manifest carries:

| key | content |
| --- | --- |
| `schema` | stage schema id (`atx.alpha-panel.<stage>/vN`) |
| `status` | `complete` |
| `code` | `{module file: {sha256, sha256_lf}}` plus `git_head` (`common.code_identity`) |
| `files` | `{path relative to the manifest's directory: {bytes, sha256}}` for every output |
| `input_manifests_sha256` | `{root-relative manifest path: SHA-256}` of every input stage manifest the build read |

`write_stage_manifest` writes the first four; add the binding with the helper:

```python
from atx_db.stagelake import bind_inputs
C.write_stage_manifest(STAGE, SCHEMA, MODULES, {..., "input_manifests_sha256": bind_inputs("prices", "identity_table")})
```

Every Parquet row carries `available_at` (UTC; naive TIMESTAMP means UTC). A legacy clock column is registered as
such (`clock_utc`, `session_date` = 22:00 UTC of that date, `dissemination_date` + 22 h, `trade_date` + 1 day) and
`verify` warns on it.

## Registry (`stagelake/registry.py`)

One data-only entry per manifest: `name, lane, schema, module, args, fetch, inputs, outputs, manifest, staleness,
vintage, guard_gb, code, built_by, planned, doc`. Outputs are root-relative globs (`ftd/year=*/ftd.parquet`,
`key=*` segments are hive partitions), each with a catalog `view`, a `clock` column (None for static, audit or
look-up tables), an optional `clock_sql`, and for `vintage` outputs the version `keys` and tie-break `order`.

Vintage policies: `event` (immutable rows with their own clock), `vintage` (versions of a key; as-of = latest
version per key), `interval` (dated validity intervals), `snapshot` (history restated from one source snapshot,
`vintage_risk`), `daily` (one row per session and line, known at the session's 22:00 UTC mark), `static` (no clock).

Registering a stage needs no edit under `stagelake/`: declare a literal in the stage module (parsed, never imported):

```python
LAKE_STAGES = [
    {"name": "reference", "lane": "MKT", "schema": "atx.alpha-panel.reference/v1", "args": ["build"],
     "fetch": ["fetch"], "inputs": [], "guard_gb": 0.4, "vintage": "event", "staleness": "...",
     "outputs": [{"glob": "reference/fx_daily.parquet", "view": "reference_fx_daily"}]},
]
```

`module` defaults to the declaring module; `args` is one flat command or a list of commands; `planned: True` until
the first publish. `tests/test_lake_registry.py::test_live_lake_...` fails, with a ready-to-paste entry, for every
manifest or Parquet file under the build root that no entry covers and for every non-planned entry whose manifest
is absent. Scratch directories whose name starts with `_` are ignored.

## `lake verify` (S1.2)

Per stage: FAIL on a missing, unreadable or incomplete manifest (no `schema`, `status` other than `complete`, no
`code`, no `files`), a schema different from the registry, a listed file missing or with another size or SHA-256,
a file matched by a registered output glob but not bound by the manifest, a registered output with no file, an
unreadable Parquet file, a clocked output without its clock column, a maximum clock after now, and any `.partial`
file. STALE when a registered input's manifest SHA-256 differs from the binding recorded at build time (the
controller rebuilds STALE stages). WARN for an input with no recorded binding and for a legacy clock column. Bindings
are read from `input_manifests_sha256`, else from older layouts (`sources.<stage>_manifest`,
`inputs.<stage>_manifest_sha256`, `receipt.input_manifests`), else from the orchestrator sidecar
`_lake/bindings/<stage>.json` when it describes exactly the published manifest.

## Catalog (S1.3, `data/catalog.duckdb`)

Rebuilt from scratch each time (views, macros and comments only; no data copied), written under its final name in
`data/_catalog_build/` and moved into place, so two rebuilds of the same lake are byte-identical. Open it read-only:

```python
import duckdb
con = duckdb.connect("data/catalog.duckdb", read_only=True)
con.sql("SELECT * FROM lake_stages")                                   # registry rows + manifest SHA-256
con.sql("SELECT * FROM fundamentals_events_as_of(lake_cutoff(DATE '2024-03-01'))")  # PIT state for session d
con.sql("SELECT * FROM corporate_actions_as_of(TIMESTAMP '2021-01-01 00:00')")
```

* `<view>_as_of(ts)`: rows whose clock < `ts` (a naive UTC TIMESTAMP); `vintage` outputs return the latest version
  per key. TIMESTAMPTZ clocks are converted to UTC, so the reader's `TimeZone` does not matter.
* `lake_cutoff(d)`: 22:00 UTC of the session before `d`, the decision cutoff of session `d`.
* `COMMENT ON` every view (stage, schema, clock, vintage policy, staleness, manifest SHA-256), macro and documented
  column (`registry.COLUMN_DOCS` plus each output's `columns`).

## Orchestrator (S1.4, `stagelake/orchestrate.py`)

A stage is due when it has no manifest, when a module it recorded (or its registry `code` list) changed (LF-normalised
SHA-256; platform modules such as `common.py` only with `--strict-platform`), when an input manifest differs from the
recorded binding or no binding is recorded, or when an input is due in the same plan. `--dry-run` prints the plan in
topological order with the reasons and commands; `--run` executes the due stages through the guard (each at its
registry `guard_gb`, `--max-gb` caps them), stops at the first failure and writes the binding sidecar after every
success. Stages with `built_by` are rebuilt by their producer; `module: None` stages are reported as `manual`;
network landing commands (`fetch`) run only with `--fetch`.

Legacy stages (no `code`, no bindings) are due until they are rebuilt once by code that records bindings or by the
orchestrator (sidecar). `alpha_panel/build.py` stays as the fixed-order runner until the controller switches.
