# atx-db Tier-1 Parity — Sprint 3 (Declarative Derived-Metric Engine) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the three parallel derived stacks (96 `fundamental_ratios.RATIO_DEFS`, 41 `metric_engine` growth specs, 61 imperative per-metric factor modules totalling 22,121 lines) with ONE declarative engine: a seed CSV of metric definitions, a hand-written expression DSL compiled to DuckDB SQL, and two base panels (quarterly fundamentals, daily market) over which every metric in the spec's "Derived metric catalog" is computed point-in-time with `available_at = max(input available_at)` and a reproducible `inputs_hash` — then retire the per-metric modules whose math the engine now owns, and publish wide Parquet panels.

**Architecture:** `seeds/derived_metric_definitions.csv` is the source of truth. `derived_dsl.py` tokenizes and parses each `expression` with a hand-written recursive-descent parser into a frozen AST (no `eval`, no `exec`, no `DataFrame.eval`), then lowers the AST to a DuckDB SQL *pair*: a value expression and a parallel availability expression, so a metric's `available_at` is derived by the same lowering that derives its value. `derived_registry.py` reads the seed, validates namespaces (`item:` / `metric:` / `market:`), builds the metric dependency graph, rejects cycles, and produces a topological order. `derived_metrics.py` materialises a quarterly base panel — a pivot of `fundamental_standardized` onto each security's quarterly `period_end` grid — and emits one deterministic `INSERT ... SELECT ... ORDER BY` per metric in topological order into `derived_metric_values`. `market_daily.py` reuses the *same compiler* over a second base panel: `equity_daily_bars` ASOF-joined to the latest visible `fundamental_standardized` and `derived_metric_values` rows with `available_at <= trade_date + 22 hours` (the end-of-day convention `ticker_history.py:392` already stamps onto every bar), and pivots the `window='daily'` metrics onto the published wide table `market_daily_metrics`. `derived_factor_projection.py` closes the retirement loop: it projects any engine metric onto the monthly rebalance grid, winsorizes and z-scores it with the existing `atx_db.factors.cross_section` operators, and writes the same 15-column `fundamental_factor_values` shape the per-metric modules wrote — so 18 leaf modules and 14 `scripts/build_*.py` wrappers can be deleted without losing a published `factor_id`.

**Tech Stack:** Python 3.12, DuckDB 1.5.x (embedded, via `atx_db.connection.DuckDBStore`; `ASOF JOIN`, window frames, `sha256()`, `string_agg(... ORDER BY ...)`), pandas + pyarrow (projection and Parquet export only), stdlib `csv`/`json`/`hashlib`/`dataclasses` for all seed I/O and AST nodes, pytest 9 + pytest-xdist + filelock (fingerprinted schema template in `tests/conftest.py`), numbered migration bodies in `src/atx_db/migrations/bodies_NNNN.py` registered in `migrations/registry.py`.

**Spec:** `C:\atx\docs\superpowers\specs\2026-09-19-tier1-parity-design.md` — sections "Clocks and PIT contract", "Derived metric catalog (PIT, from standardized layer + prices)", "Market data from `tbltickerhistory`", "Serving".

**Supporting audit:** `C:\atx\.superpowers\sdd\tier1-parity\audit-atx-db.md` §3 (three parallel derived stacks; `enterprise_value` availability-as-preference), §4 (bars / corporate-action tables and their exact columns; the `split_factor`-holds-`returnFactor` naming hazard), §9 (duplication: 118 source lines shared by ≥60% of the 46 pandas factor modules; determinism defects).

**Upstream sprints this plan consumes:**
- Sprint 1 — `C:\atx\docs\superpowers\plans\2026-09-19-tier1-s1-foundation.md`. Provides `atx_db.clock` (already landed on `feat/tier1-parity`: `utc_today()`, `resolve_as_of_date()`), migration `0300`, and `atx_db.activation` with `STAGE_ORDER: tuple[str, ...]`, `STAGES: dict[str, Callable[[DuckDBStore, ActivationOptions], StageResult]]`, `StageResult(rows: int, detail: dict[str, object])`, `ActivationOptions`, `begin_stage`, `finish_stage`, `completed_stages`, `select_stages`, `run_activation`.
- Sprint 2 — `C:\atx\docs\superpowers\plans\2026-09-19-tier1-s2-standardization.md`. Provides migration `0301`, the widened item registry (235 → 249 items), and the canonical item codes this plan's seed references. **Sprint 3 references only canonical codes that exist in `seeds/fundamental_items.csv` on `main` today** (verified by reading the seed), so Task 4 does not block on Sprint 2 landing; Sprint 2's 14 new items simply widen coverage of the same expressions later.

## Global Constraints

Copied verbatim from the sprint charter. Every task's requirements implicitly include this section.

- Python 3.12 venv at `C:\atx\atx-db\.venv\Scripts\python.exe`.
- Run tests from `C:\atx\atx-db` with `.venv\Scripts\python.exe -m pytest <file> -n 0 -q`.
- No network in tests.
- No `eval` / `exec`.
- No wall-clock reads in derived paths — only `atx_db.warehouse.now_utc_naive()` for load stamps; `atx_db.clock.utc_today()` only at script edges.
- Every derived value's `available_at` = max of input availabilities.
- Deterministic ordering: `ORDER BY` on every insert-select.
- Never delete existing public tables.
- Commits `feat(db): ...` / `refactor(db): ...` ending with `Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>`.
- Each task ≤ ~450 lines of new code except seed CSV rows.
- TDD per task: write the failing test first, run it, see it fail, then implement.

### Repo facts this plan depends on (verified by reading the files, not assumed)

- Migration registry head is **0299** (`src/atx_db/migrations/registry.py`; gaps at `0138-0139`, `0168-0175`, `0184`, `0297`). Sprint 1 takes `0300`, Sprint 2 takes `0301`. **Sprint 3 takes `0302` only.** Pattern: `src/atx_db/migrations/bodies_NNNN.py` exporting `MIGRATIONS = [Migration(version=NNN, name="...", up=_fn)]`, imported and splatted into `MIGRATIONS` in `registry.py`; `_validate_registry_versions()` enforces ascending order and uniqueness at import.
- `fundamental_standardized` columns (DDL at `src/atx_db/migrations/bodies_0001_0137.py:8131`, insert list at `src/atx_db/_standardization_set_based.py:998-1008`): `standardized_id, source, upstream_source, security_id VARCHAR, symbol, cik, item_id INTEGER, canonical_code VARCHAR, basis VARCHAR, period_start, period_end DATE, fiscal_year, fiscal_period, value DOUBLE, unit, unit_type, source_accession, filed_date, as_of_date, available_at TIMESTAMP, input_codes_json, input_item_ids_json, rule_id, combination_rule, revision_group_id, revision_sequence, revision_count, is_value_changed, previous_value, value_delta, value_delta_percent, update_type, valid_to, is_latest_revision, run_id`. `basis` ∈ `{annual, quarterly, ttm, instant}`. **`security_id` is `VARCHAR`, not an integer.**
- `equity_daily_bars` columns (`src/atx_db/schema.py:536` plus `ALTER`s in `bodies_0001_0137.py:38-40,11362` and `bodies_0263.py`): `source, security_id VARCHAR, vendor_security_id, symbol, trade_date DATE, open, high, low, close, adjusted_close, volume, vwap, dividend_amount, split_factor, is_adjusted, available_at TIMESTAMP, run_id, source_loaded_at, as_of_date, is_latest_revision, shares_outstanding, market_cap_usd`.
- **End-of-day availability convention**: `ticker_history.py:392` stamps `available_at = pd.to_datetime(frame["trading_date"]) + pd.Timedelta(hours=22)`; the set-based path at `ticker_history.py:628,648` uses `min(first_seen) + INTERVAL 22 HOUR`. `features.py` uses the same `as_of_date + 22h` for bar features. This plan therefore joins fundamentals to bars with `available_at <= trade_date + INTERVAL 22 HOUR`.
- **`equity_daily_bars.split_factor` holds the vendor `returnFactor`** (a *total-return* factor, `< 1` on dividend days), and `is_adjusted` is hard-coded `false` while `adjusted_close` is populated from vendor `closePr` (audit §4.3). This plan computes total returns from `adjusted_close` ratios and **never** from `split_factor`.
- `shares_outstanding_history` (`schema.py:664`) carries `security_id, share_count_type, effective_date, as_of_date, available_at, share_count, revision_sequence, is_latest_revision`; `SHARE_COUNT_METRICS` in `shares_outstanding.py` includes `shares_outstanding`. `equity_daily_bars.shares_outstanding` carries the archive (vendor) shares. These are the two sides of the 5% reconciliation.
- There is **no `src/atx_db/market_cap.py`**. `refresh_market_cap` / `MarketCapOptions` / `MarketCapDataset` live in `valuation_multiples.py:835,292,857`; `refresh_enterprise_value` / `EnterpriseValueOptions` / `EnterpriseValueDataset` live in `enterprise_value.py:1110,74,1134`. The `market_cap` table (`schema.py:697`) and `enterprise_value` table stay untouched by this sprint.
- `tests/data/public_api_snapshot.json` pins `sorted(dir(...))` for `atx_db`, `atx_db.asof`, `atx_db.estimates`, `atx_db.migrations`, `atx_db.quality`. The `"atx_db"` list has **577** entries and **does include submodule names** (verified: `piotroski`, `altman_distress`, `fundamental_signals`, `factor_panel`, `lake`, `jobs`, `clock` are all present; `activation` is not yet). Adding a module adds a name; deleting a module removes one. Both directions must be reflected in the same commit.
- None of the 61 per-metric modules is re-exported from `src/atx_db/__init__.py` (verified: zero matches for all 18 deletion candidates), so deletion changes only the submodule-name entries in the snapshot, never a public symbol.
- `tests/conftest.py` fixtures: `_schema_template` (session), `tmp_store`, `fresh_store`, `built_warehouse(name) -> Path`. The fingerprint at `_schema_fingerprint()` hashes all `seeds/*.csv`, so a new seed CSV invalidates the template automatically — no conftest edit needed.
- `atx_db.warehouse` exports `now_utc_naive()` (line 19), `insert_frame`, `json_dumps`, `quality_check`, `register_frame`. `_insert_projection` auto-fills `source_loaded_at = now()` for any target table missing it from the frame — benign lineage, never a signal input.
- `atx_db.factors.cross_section` exports `rank`, `zscore`, `winsorize`, `neutralize`, `pit_safety_report`, `CrossSectionOperatorError`. Signature in use across all 46 pandas modules: `winsorize(frame, value_column=..., output_column=..., partition_columns=(...), limits=float)` and `zscore(frame, value_column=..., output_column=..., partition_columns=(...))`.
- `atx_db.item_registry` exports `read_fundamental_item_seed(path=SEED_PATH) -> tuple[FundamentalItemSeedRow, ...]` and `default_registry()`. `FundamentalItemSeedRow` fields come from the CSV header: `item_id, canonical_code, statement, section, data_type, unit_type, sign_convention, is_derived, definition, citation, alias_scheme, alias_code, coalesce_priority, valid_from, valid_to, vendor, vendor_field, sign_note`.
- `atx_db.jobs.DATASET_REGISTRY: dict[str, tuple[type[Dataset], OptionFactory]]` (line 1230) and `DATASET_DEPENDENCIES: dict[str, tuple[str, ...]]`; `_apply_dataset_dependencies()` raises on a dependency key with no registry entry, so both dicts must be edited together.
- `atx_db.api.catalog` exports `FieldSpec`, `RecordSchema`, `DatasetSpec`, `_PIT_FIELDS`, `DATASETS: Final[tuple[DatasetSpec, ...]]` (line 611), `get_dataset`, `get_schema`, `public_catalog`, `public_schema`, `_record_schema_sha256`. Two dataset specs exist: `ATX.US.FUNDAMENTALS` and `ATX.US.EQUITIES`.
- `atx_db.lake` exports `LakehouseExporter`, `LakeExportResult`, and the private helpers `_schema_sha256(schema)` and `_object_schema(store, object_name)` used to pin an export contract.

### Deviations from the charter, with rationale

Three refinements; everything else in the charter is implemented as written.

1. **`indicator_gt(a, b)` / `indicator_lt(a, b)` added to the function set.** The charter's function list has no comparison. `piotroski_f` (nine binary signals) and `ohlson_o` (two indicator terms) are unrepresentable without one, and the spec's "Quality / accruals" family names both. `indicator_gt(a,b)` lowers to `CASE WHEN a IS NULL OR b IS NULL THEN NULL WHEN a > b THEN 1.0 ELSE 0.0 END`. It adds no cross-sectional dependence and no new failure mode.
2. **Four daily-grid functions added: `lag_d(x, n)`, `avg_d(x, n)`, `tret(n)`, `rvol(n)`.** The charter asks `market_daily_metrics` for 1m/3m/6m/12m returns, 12-1 momentum, 60d/252d realized vol and 20d dollar volume. Those are trading-day windows, not fiscal-quarter windows, so `lag`/`stdev_q` cannot express them. The compiler rejects a daily function inside a quarterly-window metric and vice-versa.
3. **The seed CSV column is `window`; the database column is `metric_window`.** `WINDOW` is a DuckDB reserved word and quoting it in every generated statement is a correctness hazard. The seed header stays `window` exactly as the charter specifies; `derived_registry.py` maps it.

### Ruling required before Task 8 (recorded here, resolved by the sprint owner)

The charter asks for a 1e-9 relative parity harness against the per-metric modules. **Every one of the 61 modules writes a cross-sectionally winsorized and z-scored `value`** (`winsorize` → `zscore` partitioned by `(factor_id, as_of_date)`, 46/46 modules) alongside a `raw_value` that is the actual metric, on a **monthly rebalance grid keyed `as_of_date = last trade date of the month`** — not on `period_end`. A per-security metric engine cannot reproduce a cross-sectional z-score. This plan therefore defines parity as: **engine metric level, as-of-joined to the module's own rebalance grid and sign-normalized by the seed's `orientation`, equals the module's `raw_value` to 1e-9 relative** (Task 7), and separately proves the z-scored `value` is reproduced by the generic projection (Task 7, `test_projection_matches_module_value`). The modules additionally read `fundamental_statement_points` while the engine reads `fundamental_standardized`; the parity fixture is built so both tables carry the same facts, and the harness asserts that precondition explicitly rather than assuming it.

## File structure

**Created**

| Path | Responsibility |
| --- | --- |
| `src/atx_db/derived_dsl.py` | Tokenizer, recursive-descent parser, frozen AST, and the AST → DuckDB SQL lowering (value + availability + max-lag). Pure; touches no store. |
| `src/atx_db/derived_registry.py` | Seed reader for `seeds/derived_metric_definitions.csv`, validation, dependency graph, topological order, cycle detection, `seed_derived_metric_definitions(store)`. |
| `src/atx_db/seeds/derived_metric_definitions.csv` | The declarative derived catalog. Source of truth. |
| `src/atx_db/derived_metrics.py` | The quarterly engine: base panel, per-metric insert-select in topological order, `inputs_hash`, security batching, `DerivedMetricsDataset`. |
| `src/atx_db/market_daily.py` | The daily engine: ASOF join of `equity_daily_bars` to fundamentals/derived metrics, shares reconciliation, the wide `market_daily_metrics` writer, `MarketDailyDataset`. |
| `src/atx_db/derived_factor_projection.py` | Generic monthly-rebalance projection of an engine metric onto `fundamental_factor_values` (winsorize + zscore via `factors.cross_section`). |
| `src/atx_db/seeds/derived_factor_projections.csv` | `factor_id` → `metric_code` + orientation + winsor limit, for the retired modules. |
| `src/atx_db/panel_export.py` | `export_panel_quarterly` / `export_panel_daily_market` → Parquet + JSON manifest. |
| `src/atx_db/migrations/bodies_0302.py` | `derived_metric_definitions`, `derived_metric_values`, `market_daily_metrics`, catalog rows, lake contract, quality checks. |
| `scripts/build_derived_metrics.py` | Operator CLI for the quarterly engine. |
| `scripts/build_market_daily.py` | Operator CLI for the daily engine. |
| `scripts/export_panels.py` | Operator CLI for `panel_export`. |
| `tests/test_derived_dsl.py` | Tokenizer, parser, precedence, rejection of `eval`-shaped input, SQL lowering, availability lowering, max-lag. |
| `tests/test_derived_registry.py` | Seed shape, namespace validation, window-compatibility rules, topological order, cycle rejection, DB seeding. |
| `tests/test_derived_catalog.py` | Every seed row parses; every `item:` exists in `fundamental_items.csv`; every `metric:` resolves; spec-family coverage; no metric/item code collision. |
| `tests/test_derived_metrics.py` | Engine correctness on a hand-built fixture: TTM, avg2, yoy, cagr, accruals, Piotroski, availability propagation, `inputs_hash` stability, batch invariance. |
| `tests/test_market_daily.py` | ASOF correctness, 22h boundary, shares source flag + 5% reconciliation, returns/vol/dollar-volume, no-lookahead probe. |
| `tests/test_derived_parity.py` | Parity harness: engine metric vs each retired module's `raw_value`; projection vs each module's `value`. |
| `tests/test_panel_export.py` | Parquet round-trip, manifest keys, query-hash and schema-sha256 stability, row counts. |
| `tests/test_derived_wiring.py` | `DATASET_REGISTRY` / `DATASET_DEPENDENCIES` / `STAGE_ORDER` / `STAGES` registration and stage signatures. |

**Modified**

| Path | Change |
| --- | --- |
| `src/atx_db/migrations/registry.py` | Import and splat `_MIGRATIONS_0302` (2 lines). |
| `src/atx_db/api/catalog.py` | Add `DERIVED_METRICS_SCHEMA` and `MARKET_DAILY_SCHEMA`; attach to `ATX.US.FUNDAMENTALS` and `ATX.US.EQUITIES`. |
| `src/atx_db/jobs.py` | Register `DerivedMetricsDataset` / `MarketDailyDataset` in `DATASET_REGISTRY`, add two `DATASET_DEPENDENCIES` entries and two option factories. |
| `src/atx_db/activation.py` | Add `stage_derived_metrics` / `stage_market_daily` and extend `STAGE_ORDER`. |
| `tests/data/public_api_snapshot.json` | +8 module names (Tasks 1–9), −18 module names (Task 8). |
| `tests/test_module_boundaries.py` | Nothing structural; it reads the snapshot. Listed so the executor runs it after every add/delete task. |

**Deleted (Task 8 only)**

18 per-metric modules and 14 `scripts/build_*.py` wrappers — enumerated in Task 8.

---

### Task 1: Migration 0302 — the three derived tables, catalog rows, and lake contract

**Files:**
- Create: `C:\atx\atx-db\src\atx_db\migrations\bodies_0302.py`
- Modify: `C:\atx\atx-db\src\atx_db\migrations\registry.py` (2 lines)
- Test: `C:\atx\atx-db\tests\test_derived_migration_0302.py` (new)

**Interfaces:**
- Consumes: `atx_db.migrations._runner.Migration` (existing).
- Produces:
  - Table `derived_metric_definitions(metric_code VARCHAR PRIMARY KEY, family VARCHAR NOT NULL, expression VARCHAR NOT NULL, metric_window VARCHAR NOT NULL, inputs_json VARCHAR NOT NULL, requires_market BOOLEAN NOT NULL, description VARCHAR NOT NULL, version VARCHAR NOT NULL, topological_rank INTEGER NOT NULL, seeded_at TIMESTAMP NOT NULL DEFAULT now())`
  - Table `derived_metric_values(derived_value_id VARCHAR PRIMARY KEY, source VARCHAR NOT NULL, security_id VARCHAR NOT NULL, metric_code VARCHAR NOT NULL, metric_window VARCHAR NOT NULL, period_end DATE NOT NULL, value DOUBLE NOT NULL, available_at TIMESTAMP NOT NULL, inputs_hash VARCHAR NOT NULL, as_of_date DATE NOT NULL, is_latest_revision BOOLEAN NOT NULL DEFAULT true, run_id VARCHAR, source_loaded_at TIMESTAMP NOT NULL DEFAULT now())`
  - Table `market_daily_metrics` — the wide daily spine, 30 published metric columns plus the spine columns listed in the DDL below.
  - `dataset_catalog` + `table_catalog` rows for all three, and `lake_partition_specs` rows for `derived_metric_values` and `market_daily_metrics`.
  - `atx_db.migrations.bodies_0302.MIGRATIONS: list[Migration]` with a single `Migration(version=302, name="derived_metric_engine", up=_derived_metric_engine)`.

- [ ] **Step 1: Write the failing test**

Create `C:\atx\atx-db\tests\test_derived_migration_0302.py`:

```python
"""Tier1-S3 T1: migration 0302 creates the derived-engine tables."""

from __future__ import annotations

import pytest

from atx_db.migrations.registry import MIGRATIONS

_DAILY_METRIC_COLUMNS = (
    "market_cap",
    "enterprise_value",
    "pe_ttm",
    "pb",
    "ps_ttm",
    "pcf_ttm",
    "ev_ebitda",
    "ev_sales",
    "fcf_yield",
    "dividend_yield",
    "earnings_yield",
    "shareholder_yield",
    "net_payout_yield",
    "total_payout_yield",
    "buyback_yield",
    "book_to_market",
    "rd_to_market_equity",
    "gross_profit_to_ev",
    "cfo_to_ev",
    "ebit_to_ev",
    "sales_to_ev",
    "altman_z",
    "total_return_1m",
    "total_return_3m",
    "total_return_6m",
    "total_return_12m",
    "momentum_12_1",
    "realized_vol_60d",
    "realized_vol_252d",
    "dollar_volume_20d",
)


def _columns(store, table: str) -> list[str]:
    rows = store.con.execute(
        """
        SELECT column_name
        FROM duckdb_columns()
        WHERE schema_name = 'main' AND table_name = ?
        ORDER BY column_index
        """,
        [table],
    ).fetchall()
    return [str(row[0]) for row in rows]


def test_migration_302_is_registered_exactly_once():
    versions = [migration.version for migration in MIGRATIONS]
    assert versions.count(302) == 1
    assert max(versions) == 302


def test_derived_metric_definitions_shape(tmp_store):
    assert _columns(tmp_store, "derived_metric_definitions") == [
        "metric_code",
        "family",
        "expression",
        "metric_window",
        "inputs_json",
        "requires_market",
        "description",
        "version",
        "topological_rank",
        "seeded_at",
    ]


def test_derived_metric_values_shape(tmp_store):
    assert _columns(tmp_store, "derived_metric_values") == [
        "derived_value_id",
        "source",
        "security_id",
        "metric_code",
        "metric_window",
        "period_end",
        "value",
        "available_at",
        "inputs_hash",
        "as_of_date",
        "is_latest_revision",
        "run_id",
        "source_loaded_at",
    ]


def test_market_daily_metrics_carries_every_published_daily_metric(tmp_store):
    columns = set(_columns(tmp_store, "market_daily_metrics"))
    assert sorted(set(_DAILY_METRIC_COLUMNS) - columns) == []
    for column in (
        "security_id",
        "trade_date",
        "close",
        "adj_close",
        "volume",
        "shares_outstanding",
        "shares_source",
        "shares_reconciliation_ratio",
        "fundamental_available_at",
        "available_at",
        "inputs_hash",
    ):
        assert column in columns


def test_new_tables_are_catalogued(tmp_store):
    rows = tmp_store.con.execute(
        """
        SELECT table_name FROM table_catalog
        WHERE table_name IN (
            'derived_metric_definitions','derived_metric_values','market_daily_metrics'
        )
        ORDER BY table_name
        """
    ).fetchall()
    assert [str(row[0]) for row in rows] == [
        "derived_metric_definitions",
        "derived_metric_values",
        "market_daily_metrics",
    ]


def test_lake_partition_specs_registered(tmp_store):
    rows = tmp_store.con.execute(
        """
        SELECT object_name, watermark_column FROM lake_partition_specs
        WHERE object_name IN ('derived_metric_values','market_daily_metrics')
        ORDER BY object_name
        """
    ).fetchall()
    assert [(str(a), str(b)) for a, b in rows] == [
        ("derived_metric_values", "available_at"),
        ("market_daily_metrics", "available_at"),
    ]


@pytest.mark.parametrize("table", ["derived_metric_values", "market_daily_metrics"])
def test_tables_are_empty_after_bootstrap(tmp_store, table):
    assert tmp_store.con.execute(f"SELECT count(*) FROM {table}").fetchone()[0] == 0
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `.venv\Scripts\python.exe -m pytest tests/test_derived_migration_0302.py -n 0 -q`
Expected: FAIL — `assert 0 == 1` in `test_migration_302_is_registered_exactly_once`, and `duckdb.CatalogException: Table with name derived_metric_definitions does not exist!` in the shape tests.

- [ ] **Step 3: Create `src/atx_db/migrations/bodies_0302.py`**

```python
"""Declarative derived-metric engine: definitions, values, and the daily market panel."""

from __future__ import annotations

import duckdb

from ._runner import Migration

_DAILY_METRIC_COLUMNS = (
    "market_cap",
    "enterprise_value",
    "pe_ttm",
    "pb",
    "ps_ttm",
    "pcf_ttm",
    "ev_ebitda",
    "ev_sales",
    "fcf_yield",
    "dividend_yield",
    "earnings_yield",
    "shareholder_yield",
    "net_payout_yield",
    "total_payout_yield",
    "buyback_yield",
    "book_to_market",
    "rd_to_market_equity",
    "gross_profit_to_ev",
    "cfo_to_ev",
    "ebit_to_ev",
    "sales_to_ev",
    "altman_z",
    "total_return_1m",
    "total_return_3m",
    "total_return_6m",
    "total_return_12m",
    "momentum_12_1",
    "realized_vol_60d",
    "realized_vol_252d",
    "dollar_volume_20d",
)

_TABLE_CATALOG_ROWS = (
    (
        "derived_metric_definitions",
        "reference",
        "metric",
        "metric_code",
        "Declarative derived-metric catalog seeded from seeds/derived_metric_definitions.csv.",
        '["metric_code"]',
        "Registry table; carries no PIT columns. Re-seeded on every derived build.",
    ),
    (
        "derived_metric_values",
        "derived",
        "security",
        "security_id,metric_code,metric_window,period_end",
        "Point-in-time derived metric values produced by atx_db.derived_metrics from the declarative catalog.",
        '["derived_value_id"]',
        "available_at is the max availability of every input fact; inputs_hash is sha256 over the sorted (code, period_end, revision_sequence, value) tuples consumed.",
    ),
    (
        "market_daily_metrics",
        "derived",
        "security",
        "security_id,trade_date",
        "Daily market panel: prices, shares, market cap, enterprise value, valuation multiples, returns and realized volatility.",
        '["market_daily_id"]',
        "available_at = greatest(trade_date + 22h, fundamental_available_at). Never join on trade_date alone.",
    ),
)

_DATASET_CATALOG_ROWS = (
    (
        "derived_metrics",
        "Declarative PIT derived metrics",
        "Ratios, per-share, growth, leverage, quality and investment metrics computed by one engine from derived_metric_definitions.",
        "security_id,metric_code,metric_window,period_end",
        "derived_metric_values",
    ),
    (
        "market_daily",
        "Daily market and valuation panel",
        "Daily prices, shares, market cap, enterprise value, valuation multiples, total returns and realized volatility.",
        "security_id,trade_date",
        "market_daily_metrics",
    ),
)


def _derived_metric_engine(conn: duckdb.DuckDBPyConnection) -> None:
    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS derived_metric_definitions (
            metric_code VARCHAR PRIMARY KEY,
            family VARCHAR NOT NULL,
            expression VARCHAR NOT NULL,
            metric_window VARCHAR NOT NULL,
            inputs_json VARCHAR NOT NULL,
            requires_market BOOLEAN NOT NULL,
            description VARCHAR NOT NULL,
            version VARCHAR NOT NULL,
            topological_rank INTEGER NOT NULL,
            seeded_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS derived_metric_values (
            derived_value_id VARCHAR PRIMARY KEY,
            source VARCHAR NOT NULL,
            security_id VARCHAR NOT NULL,
            metric_code VARCHAR NOT NULL,
            metric_window VARCHAR NOT NULL,
            period_end DATE NOT NULL,
            value DOUBLE NOT NULL,
            available_at TIMESTAMP NOT NULL,
            inputs_hash VARCHAR NOT NULL,
            as_of_date DATE NOT NULL,
            is_latest_revision BOOLEAN NOT NULL DEFAULT true,
            run_id VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    conn.execute(
        "CREATE INDEX IF NOT EXISTS idx_derived_metric_values_lookup "
        "ON derived_metric_values (security_id, metric_code, period_end)"
    )
    metric_columns = ",\n            ".join(f"{name} DOUBLE" for name in _DAILY_METRIC_COLUMNS)
    conn.execute(
        f"""
        CREATE TABLE IF NOT EXISTS market_daily_metrics (
            market_daily_id VARCHAR PRIMARY KEY,
            source VARCHAR NOT NULL,
            security_id VARCHAR NOT NULL,
            symbol VARCHAR,
            trade_date DATE NOT NULL,
            close DOUBLE,
            adj_close DOUBLE,
            volume BIGINT,
            shares_outstanding DOUBLE,
            shares_source VARCHAR,
            shares_reconciliation_ratio DOUBLE,
            {metric_columns},
            fundamental_available_at TIMESTAMP,
            available_at TIMESTAMP NOT NULL,
            inputs_hash VARCHAR NOT NULL,
            as_of_date DATE NOT NULL,
            is_latest_revision BOOLEAN NOT NULL DEFAULT true,
            run_id VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    conn.execute(
        "CREATE INDEX IF NOT EXISTS idx_market_daily_metrics_lookup "
        "ON market_daily_metrics (security_id, trade_date)"
    )
    conn.executemany(
        """
        INSERT OR REPLACE INTO table_catalog (
            table_name, layer, entity, grain, description,
            natural_key_json, pit_notes, updated_at
        ) VALUES (?, ?, ?, ?, ?, ?, ?, now())
        """,
        list(_TABLE_CATALOG_ROWS),
    )
    conn.executemany(
        """
        INSERT OR REPLACE INTO dataset_catalog (
            dataset_id, source_system_id, name, description, grain,
            primary_table, pit_column, available_at_column, updated_at
        ) VALUES (?, 'sec_edgar', ?, ?, ?, ?, 'as_of_date', 'available_at', now())
        """,
        list(_DATASET_CATALOG_ROWS),
    )
    conn.executemany(
        """
        INSERT OR REPLACE INTO lake_partition_specs (
            object_name, partition_columns_json, watermark_column, updated_at
        ) VALUES (?, ?, 'available_at', now())
        """,
        [
            ("derived_metric_values", '["metric_window"]'),
            ("market_daily_metrics", '["as_of_date"]'),
        ],
    )


MIGRATIONS = [
    Migration(
        version=302,
        name="derived_metric_engine",
        up=_derived_metric_engine,
    )
]
```

- [ ] **Step 4: Register 0302 in `src/atx_db/migrations/registry.py`**

Add the import immediately after the existing `from .bodies_0299 import MIGRATIONS as _MIGRATIONS_0299` line:

```python
from .bodies_0302 import MIGRATIONS as _MIGRATIONS_0302
```

and add the splat as the last entry of the `MIGRATIONS` list, after `*_MIGRATIONS_0299,`:

```python
    *_MIGRATIONS_0302,
```

- [ ] **Step 5: Run the tests**

Run: `.venv\Scripts\python.exe -m pytest tests/test_derived_migration_0302.py -n 0 -q`
Expected: `8 passed`. The first run after a schema change rebuilds the fingerprinted template, so allow ~3 minutes.

Run: `.venv\Scripts\python.exe -m pytest tests/test_migration_governance.py tests/test_schema_contract_v2.py -n 0 -q --run-slow`
Expected: `passed`. If `test_schema_contract_v2` pins a relation count, bump it by 3 in this commit and justify the new number in the commit body.

- [ ] **Step 6: Commit**

```
git add src/atx_db/migrations/bodies_0302.py src/atx_db/migrations/registry.py tests/test_derived_migration_0302.py
git commit -m "feat(db): add migration 0302 for the declarative derived-metric engine

Creates derived_metric_definitions (the seeded catalog), derived_metric_values
(the PIT long store with available_at and inputs_hash) and market_daily_metrics
(the wide daily spine with 30 published metric columns), plus table and dataset
catalog rows and lake partition specs for both value tables.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 2: `derived_dsl.py` — tokenizer, recursive-descent parser, AST, and SQL lowering

**Files:**
- Create: `C:\atx\atx-db\src\atx_db\derived_dsl.py`
- Modify: `C:\atx\atx-db\tests\data\public_api_snapshot.json` (add `"derived_dsl"` to the `"atx_db"` list, keeping it sorted)
- Test: `C:\atx\atx-db\tests\test_derived_dsl.py` (new)

**Interfaces:**
- Consumes: nothing from earlier tasks (pure stdlib).
- Produces:
  - `atx_db.derived_dsl.DslError(ValueError)`
  - `atx_db.derived_dsl.Token(kind: str, text: str, position: int)` — frozen dataclass; `kind` ∈ `{"number", "name", "op", "end"}`.
  - `atx_db.derived_dsl.tokenize(expression: str) -> tuple[Token, ...]`
  - AST nodes, all frozen dataclasses: `Number(value: float)`, `Ref(name: str)`, `Neg(operand: Node)`, `BinOp(op: str, left: Node, right: Node)`, `Call(name: str, args: tuple[Node, ...])`; `Node = Number | Ref | Neg | BinOp | Call`.
  - `atx_db.derived_dsl.parse_expression(expression: str) -> Node`
  - `atx_db.derived_dsl.expression_names(node: Node) -> tuple[str, ...]` — sorted distinct `Ref` names.
  - `atx_db.derived_dsl.QUARTER_FUNCTIONS: frozenset[str]`, `DAILY_FUNCTIONS: frozenset[str]`, `SCALAR_FUNCTIONS: frozenset[str]`, `FUNCTION_ARITY: dict[str, tuple[int, int]]`.
  - `atx_db.derived_dsl.LowerContext(grid: str, columns: dict[str, str], availability: dict[str, str], partition_sql: str, order_sql: str)` — frozen dataclass; `grid` ∈ `{"quarter", "day"}`, `columns` maps a `Ref` name to the SQL column expression holding its value, `availability` maps it to the SQL column expression holding its `available_at`.
  - `atx_db.derived_dsl.Lowered(value_sql: str, availability_sql: str, max_lag: int)` — frozen dataclass.
  - `atx_db.derived_dsl.lower(node: Node, context: LowerContext) -> Lowered`
  - `atx_db.derived_dsl.compile_expression(expression: str, context: LowerContext) -> Lowered`

**Grammar** (LL(1), left-associative binary operators):

```
expr    := term (("+" | "-") term)*
term    := factor (("*" | "/") factor)*
factor  := "-" factor | primary
primary := number | name "(" [expr ("," expr)*] ")" | name | "(" expr ")"
```

**Function table** (`FUNCTION_ARITY`, min and max argument count):

| function | arity | grid | lowering sketch |
| --- | --- | --- | --- |
| `safe_div(a, b)` | 2,2 | any | `CASE WHEN b IS NULL OR b = 0 THEN NULL ELSE a / b END` |
| `coalesce(a, ...)` | 1,8 | any | `coalesce(...)`; availability is the availability of the first non-null branch, lowered as a matching `CASE` chain |
| `abs(a)` | 1,1 | any | `abs(a)` |
| `min(a, b)` / `max(a, b)` | 2,2 | any | `least(a, b)` / `greatest(a, b)` |
| `ln(a)` | 1,1 | any | `CASE WHEN a IS NULL OR a <= 0 THEN NULL ELSE ln(a) END` |
| `indicator_gt(a, b)` / `indicator_lt(a, b)` | 2,2 | any | `CASE WHEN a IS NULL OR b IS NULL THEN NULL WHEN a > b THEN 1.0 ELSE 0.0 END` |
| `ttm(a)` | 1,1 | quarter | `CASE WHEN count(a) OVER w4 = 4 THEN sum(a) OVER w4 END`, `w4 = ROWS BETWEEN 3 PRECEDING AND CURRENT ROW` |
| `avg2(a)` | 1,1 | quarter | `CASE WHEN lag(a, 4) OVER w IS NULL THEN NULL ELSE (a + lag(a, 4) OVER w) / 2.0 END` |
| `lag(a, n)` | 2,2 | quarter | `lag(a, n) OVER w`; `n` must be a non-negative integer literal |
| `yoy(a)` | 1,1 | quarter | `safe_div(a - lag(a,4), abs(lag(a,4)))` lowering, so a negative base yields a sign-correct rate |
| `qoq(a)` | 1,1 | quarter | same with `lag(a,1)` |
| `cagr(a, y)` | 2,2 | quarter | `CASE WHEN a > 0 AND lag(a, 4*y) OVER w > 0 THEN power(a / lag(a, 4*y) OVER w, 1.0/y) - 1 END`; `y` is a positive integer literal |
| `stdev_q(a, n)` | 2,2 | quarter | `CASE WHEN count(a) OVER wn = n THEN stddev_samp(a) OVER wn END` |
| `lag_d(a, n)` | 2,2 | day | `lag(a, n) OVER w` |
| `avg_d(a, n)` | 2,2 | day | `CASE WHEN count(a) OVER wn = n THEN avg(a) OVER wn END` |
| `tret(a, n)` | 2,2 | day | `(a / lag(a, n) OVER w) - 1`, guarded against a null or zero base; `a` is normally the `adj_close` column |
| `rvol(a, n)` | 2,2 | day | `sqrt(252.0) * CASE WHEN count(a) OVER wn = n THEN stddev_samp(a) OVER wn END`; `a` is normally the `log_return` column |

**Availability lowering rule.** A `Ref` lowers to `context.availability[name]`. A `Number` lowers to `NULL` (a literal has no availability). Any n-ary node lowers to `greatest(...)` over its children's non-null availability expressions (`greatest` in DuckDB ignores NULL only when wrapped, so the lowering emits `greatest(coalesce(x, TIMESTAMP '-infinity'), ...)` and the engine drops rows whose value is NULL, which is exactly the rows where availability would be meaningless). A window function lowers its availability to `max(child_availability) OVER <the same window frame>` — this is what makes "`available_at` = max of input availabilities" true by construction for `ttm`, `avg2`, `lag`, `yoy`, `qoq`, `cagr`, `stdev_q`, `lag_d`, `avg_d`, `tret`, `rvol`.

**`max_lag`.** `Lowered.max_lag` is the deepest row offset the expression reaches back on its grid: `0` for scalar nodes, `3` for `ttm`, `4` for `avg2`/`yoy`, `1` for `qoq`, `n` for `lag(a, n)` / `lag_d(a, n)`, `4*y` for `cagr(a, y)`, `n - 1` for `stdev_q(a, n)` / `avg_d(a, n)` / `rvol(a, n)`, `n` for `tret(a, n)`, and for any composite node the maximum of its children plus the node's own offset where the node is itself a window. Task 5 uses it to bound the `inputs_hash` window.

- [ ] **Step 1: Write the failing test**

Create `C:\atx\atx-db\tests\test_derived_dsl.py`:

```python
"""Tier1-S3 T2: the derived-metric expression DSL parses and lowers to DuckDB SQL."""

from __future__ import annotations

import pytest

from atx_db.derived_dsl import (
    BinOp,
    DslError,
    LowerContext,
    Neg,
    Number,
    Ref,
    compile_expression,
    expression_names,
    parse_expression,
    tokenize,
)

_QUARTER = LowerContext(
    grid="quarter",
    columns={"revenue": "b.revenue", "total_assets": "b.total_assets"},
    availability={"revenue": "b.revenue_at", "total_assets": "b.total_assets_at"},
    partition_sql="b.security_id",
    order_sql="b.period_end",
)

_DAY = LowerContext(
    grid="day",
    columns={"adj_close": "d.adj_close", "log_return": "d.log_return", "volume": "d.volume"},
    availability={"adj_close": "d.bar_at", "log_return": "d.bar_at", "volume": "d.bar_at"},
    partition_sql="d.security_id",
    order_sql="d.trade_date",
)


def test_tokenize_splits_names_numbers_and_operators():
    kinds = [token.kind for token in tokenize("safe_div(a, 2.5) - -b")]
    assert kinds == [
        "name", "op", "name", "op", "number", "op", "op", "op", "name", "end",
    ]


def test_tokenize_rejects_a_character_outside_the_grammar():
    with pytest.raises(DslError) as excinfo:
        tokenize("revenue ** 2")
    assert "position" in str(excinfo.value)


def test_parse_builds_left_associative_subtraction():
    node = parse_expression("a - b - c")
    assert node == BinOp("-", BinOp("-", Ref("a"), Ref("b")), Ref("c"))


def test_parse_gives_multiplication_higher_precedence_than_addition():
    assert parse_expression("a + b * c") == BinOp("+", Ref("a"), BinOp("*", Ref("b"), Ref("c")))


def test_parse_honours_parentheses_and_unary_minus():
    assert parse_expression("-(a + 1)") == Neg(BinOp("+", Ref("a"), Number(1.0)))


def test_parse_rejects_an_unknown_function():
    with pytest.raises(DslError) as excinfo:
        parse_expression("__import__(a)")
    assert "unknown function" in str(excinfo.value)


def test_parse_rejects_wrong_arity():
    with pytest.raises(DslError) as excinfo:
        parse_expression("safe_div(a)")
    assert "safe_div" in str(excinfo.value)


def test_parse_rejects_trailing_input():
    with pytest.raises(DslError):
        parse_expression("a b")


def test_expression_names_are_sorted_and_deduplicated():
    assert expression_names(parse_expression("b + a + b")) == ("a", "b")


def test_ttm_lowers_to_a_four_row_window_with_a_completeness_guard():
    lowered = compile_expression("ttm(revenue)", _QUARTER)
    assert "ROWS BETWEEN 3 PRECEDING AND CURRENT ROW" in lowered.value_sql
    assert "count(b.revenue)" in lowered.value_sql
    assert lowered.max_lag == 3


def test_window_availability_uses_the_same_frame():
    lowered = compile_expression("ttm(revenue)", _QUARTER)
    assert "max(b.revenue_at)" in lowered.availability_sql
    assert lowered.availability_sql.count("ROWS BETWEEN 3 PRECEDING AND CURRENT ROW") == 1


def test_binary_availability_is_the_greatest_of_both_sides():
    lowered = compile_expression("revenue - total_assets", _QUARTER)
    assert "greatest(" in lowered.availability_sql
    assert "b.revenue_at" in lowered.availability_sql
    assert "b.total_assets_at" in lowered.availability_sql


def test_safe_div_guards_a_zero_denominator():
    lowered = compile_expression("safe_div(revenue, total_assets)", _QUARTER)
    assert "= 0" in lowered.value_sql
    assert "ELSE" in lowered.value_sql


def test_cagr_requires_an_integer_year_literal():
    with pytest.raises(DslError) as excinfo:
        compile_expression("cagr(revenue, total_assets)", _QUARTER)
    assert "integer literal" in str(excinfo.value)
    assert compile_expression("cagr(revenue, 3)", _QUARTER).max_lag == 12


def test_number_has_no_availability():
    lowered = compile_expression("revenue * 2", _QUARTER)
    assert lowered.availability_sql.count("b.revenue_at") == 1


def test_unknown_reference_is_rejected_by_the_lowering():
    with pytest.raises(DslError) as excinfo:
        compile_expression("ebitda", _QUARTER)
    assert "ebitda" in str(excinfo.value)


def test_quarter_function_is_rejected_on_the_daily_grid():
    with pytest.raises(DslError) as excinfo:
        compile_expression("ttm(adj_close)", _DAY)
    assert "quarter" in str(excinfo.value)


def test_daily_function_is_rejected_on_the_quarterly_grid():
    with pytest.raises(DslError) as excinfo:
        compile_expression("tret(revenue, 21)", _QUARTER)
    assert "day" in str(excinfo.value)


def test_tret_divides_by_the_lagged_price():
    lowered = compile_expression("tret(adj_close, 21)", _DAY)
    assert "lag(d.adj_close, 21)" in lowered.value_sql
    assert lowered.max_lag == 21


def test_rvol_annualizes_with_sqrt_252():
    lowered = compile_expression("rvol(log_return, 60)", _DAY)
    assert "sqrt(252" in lowered.value_sql
    assert "stddev_samp(d.log_return)" in lowered.value_sql
    assert lowered.max_lag == 59


def test_no_expression_reaches_python_builtins():
    for hostile in ("__class__", "open('x')", "eval(a)", "a; DROP TABLE t", "a--b\nc"):
        with pytest.raises(DslError):
            parse_expression(hostile)
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `.venv\Scripts\python.exe -m pytest tests/test_derived_dsl.py -n 0 -q`
Expected: FAIL — `ModuleNotFoundError: No module named 'atx_db.derived_dsl'`.

- [ ] **Step 3: Create `src/atx_db/derived_dsl.py`**

```python
"""A restricted arithmetic DSL for derived-metric definitions.

The DSL is parsed by a hand-written tokenizer and recursive-descent parser into
a frozen AST and lowered to DuckDB SQL. There is no ``eval``, no ``exec`` and no
``DataFrame.eval``: the only characters the tokenizer accepts are digits, the
decimal point, ASCII letters, the underscore, the five operators ``+ - * /``,
parentheses and the comma. Anything else is a ``DslError`` naming its position.

Every lowering produces three things: the SQL value expression, a parallel SQL
availability expression built from the same tree, and the deepest row offset the
expression reaches back on its grid. The availability expression is what makes
"a derived value's available_at is the max over its inputs" true by
construction rather than by convention.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Union

__all__ = [
    "BinOp",
    "Call",
    "DAILY_FUNCTIONS",
    "DslError",
    "FUNCTION_ARITY",
    "LowerContext",
    "Lowered",
    "Neg",
    "Node",
    "Number",
    "QUARTER_FUNCTIONS",
    "Ref",
    "SCALAR_FUNCTIONS",
    "Token",
    "compile_expression",
    "expression_names",
    "lower",
    "parse_expression",
    "tokenize",
]

_NEG_INFINITY = "TIMESTAMP '-infinity'"


class DslError(ValueError):
    """A malformed or unsupported derived-metric expression."""


@dataclass(frozen=True)
class Token:
    kind: str
    text: str
    position: int


@dataclass(frozen=True)
class Number:
    value: float


@dataclass(frozen=True)
class Ref:
    name: str


@dataclass(frozen=True)
class Neg:
    operand: "Node"


@dataclass(frozen=True)
class BinOp:
    op: str
    left: "Node"
    right: "Node"


@dataclass(frozen=True)
class Call:
    name: str
    args: tuple["Node", ...]


Node = Union[Number, Ref, Neg, BinOp, Call]

SCALAR_FUNCTIONS = frozenset(
    {"safe_div", "coalesce", "abs", "min", "max", "ln", "indicator_gt", "indicator_lt"}
)
QUARTER_FUNCTIONS = frozenset({"ttm", "avg2", "lag", "yoy", "qoq", "cagr", "stdev_q"})
DAILY_FUNCTIONS = frozenset({"lag_d", "avg_d", "tret", "rvol"})

FUNCTION_ARITY: dict[str, tuple[int, int]] = {
    "safe_div": (2, 2),
    "coalesce": (1, 8),
    "abs": (1, 1),
    "min": (2, 2),
    "max": (2, 2),
    "ln": (1, 1),
    "indicator_gt": (2, 2),
    "indicator_lt": (2, 2),
    "ttm": (1, 1),
    "avg2": (1, 1),
    "lag": (2, 2),
    "yoy": (1, 1),
    "qoq": (1, 1),
    "cagr": (2, 2),
    "stdev_q": (2, 2),
    "lag_d": (2, 2),
    "avg_d": (2, 2),
    "tret": (2, 2),
    "rvol": (2, 2),
}

_OPERATORS = frozenset({"+", "-", "*", "/", "(", ")", ","})
_NAME_START = frozenset("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ_")
_NAME_BODY = _NAME_START | frozenset("0123456789")
_DIGITS = frozenset("0123456789")


def tokenize(expression: str) -> tuple[Token, ...]:
    tokens: list[Token] = []
    index = 0
    length = len(expression)
    while index < length:
        char = expression[index]
        if char in " \t":
            index += 1
            continue
        if char in _OPERATORS:
            tokens.append(Token("op", char, index))
            index += 1
            continue
        if char in _DIGITS or (char == "." and index + 1 < length and expression[index + 1] in _DIGITS):
            start = index
            seen_dot = False
            while index < length and (expression[index] in _DIGITS or (expression[index] == "." and not seen_dot)):
                seen_dot = seen_dot or expression[index] == "."
                index += 1
            tokens.append(Token("number", expression[start:index], start))
            continue
        if char in _NAME_START:
            start = index
            while index < length and expression[index] in _NAME_BODY:
                index += 1
            tokens.append(Token("name", expression[start:index], start))
            continue
        raise DslError(f"unsupported character {char!r} at position {index} in {expression!r}")
    tokens.append(Token("end", "", length))
    return tuple(tokens)


class _Parser:
    def __init__(self, tokens: tuple[Token, ...], expression: str) -> None:
        self._tokens = tokens
        self._expression = expression
        self._index = 0

    @property
    def _current(self) -> Token:
        return self._tokens[self._index]

    def _advance(self) -> Token:
        token = self._tokens[self._index]
        self._index += 1
        return token

    def _expect_op(self, text: str) -> None:
        token = self._current
        if token.kind != "op" or token.text != text:
            raise DslError(
                f"expected {text!r} at position {token.position} in {self._expression!r}, got {token.text!r}"
            )
        self._advance()

    def parse(self) -> Node:
        node = self._expr()
        if self._current.kind != "end":
            raise DslError(
                f"unexpected trailing input at position {self._current.position} in {self._expression!r}"
            )
        return node

    def _expr(self) -> Node:
        node = self._term()
        while self._current.kind == "op" and self._current.text in ("+", "-"):
            op = self._advance().text
            node = BinOp(op, node, self._term())
        return node

    def _term(self) -> Node:
        node = self._factor()
        while self._current.kind == "op" and self._current.text in ("*", "/"):
            op = self._advance().text
            node = BinOp(op, node, self._factor())
        return node

    def _factor(self) -> Node:
        if self._current.kind == "op" and self._current.text == "-":
            self._advance()
            return Neg(self._factor())
        return self._primary()

    def _primary(self) -> Node:
        token = self._current
        if token.kind == "number":
            self._advance()
            return Number(float(token.text))
        if token.kind == "op" and token.text == "(":
            self._advance()
            node = self._expr()
            self._expect_op(")")
            return node
        if token.kind == "name":
            self._advance()
            if self._current.kind == "op" and self._current.text == "(":
                return self._call(token)
            return Ref(token.text)
        raise DslError(f"unexpected token {token.text!r} at position {token.position} in {self._expression!r}")

    def _call(self, name_token: Token) -> Node:
        name = name_token.text
        if name not in FUNCTION_ARITY:
            raise DslError(
                f"unknown function {name!r} at position {name_token.position} in {self._expression!r}"
            )
        self._expect_op("(")
        args: list[Node] = []
        if not (self._current.kind == "op" and self._current.text == ")"):
            args.append(self._expr())
            while self._current.kind == "op" and self._current.text == ",":
                self._advance()
                args.append(self._expr())
        self._expect_op(")")
        minimum, maximum = FUNCTION_ARITY[name]
        if not minimum <= len(args) <= maximum:
            raise DslError(
                f"{name!r} takes {minimum}..{maximum} arguments, got {len(args)} in {self._expression!r}"
            )
        return Call(name, tuple(args))


def parse_expression(expression: str) -> Node:
    text = expression.strip()
    if not text:
        raise DslError("empty expression")
    return _Parser(tokenize(text), text).parse()


def expression_names(node: Node) -> tuple[str, ...]:
    found: set[str] = set()

    def walk(current: Node) -> None:
        if isinstance(current, Ref):
            found.add(current.name)
        elif isinstance(current, Neg):
            walk(current.operand)
        elif isinstance(current, BinOp):
            walk(current.left)
            walk(current.right)
        elif isinstance(current, Call):
            for argument in current.args:
                walk(argument)

    walk(node)
    return tuple(sorted(found))


@dataclass(frozen=True)
class LowerContext:
    grid: str
    columns: dict[str, str]
    availability: dict[str, str]
    partition_sql: str
    order_sql: str


@dataclass(frozen=True)
class Lowered:
    value_sql: str
    availability_sql: str
    max_lag: int


def _frame(context: LowerContext, preceding: int) -> str:
    return (
        f"PARTITION BY {context.partition_sql} ORDER BY {context.order_sql} "
        f"ROWS BETWEEN {preceding} PRECEDING AND CURRENT ROW"
    )


def _unbounded(context: LowerContext) -> str:
    return f"PARTITION BY {context.partition_sql} ORDER BY {context.order_sql}"


def _greatest(parts: list[str]) -> str:
    live = [part for part in parts if part != "NULL"]
    if not live:
        return "NULL"
    if len(live) == 1:
        return live[0]
    wrapped = ", ".join(f"coalesce({part}, {_NEG_INFINITY})" for part in live)
    return f"nullif(greatest({wrapped}), {_NEG_INFINITY})"


def _integer_literal(node: Node, *, function: str, expression_hint: str) -> int:
    if not isinstance(node, Number) or node.value != int(node.value) or node.value < 0:
        raise DslError(
            f"{function!r} requires a non-negative integer literal argument in {expression_hint}"
        )
    return int(node.value)


def lower(node: Node, context: LowerContext) -> Lowered:  # noqa: C901 - one closed dispatch
    if context.grid not in ("quarter", "day"):
        raise DslError(f"unknown grid {context.grid!r}")
    if isinstance(node, Number):
        return Lowered(repr(float(node.value)), "NULL", 0)
    if isinstance(node, Ref):
        if node.name not in context.columns:
            raise DslError(f"unresolved reference {node.name!r} on the {context.grid} grid")
        return Lowered(context.columns[node.name], context.availability[node.name], 0)
    if isinstance(node, Neg):
        inner = lower(node.operand, context)
        return Lowered(f"(-({inner.value_sql}))", inner.availability_sql, inner.max_lag)
    if isinstance(node, BinOp):
        left = lower(node.left, context)
        right = lower(node.right, context)
        if node.op == "/":
            value = (
                f"(CASE WHEN ({right.value_sql}) IS NULL OR ({right.value_sql}) = 0 "
                f"THEN NULL ELSE ({left.value_sql}) / ({right.value_sql}) END)"
            )
        else:
            value = f"(({left.value_sql}) {node.op} ({right.value_sql}))"
        return Lowered(
            value,
            _greatest([left.availability_sql, right.availability_sql]),
            max(left.max_lag, right.max_lag),
        )
    if isinstance(node, Call):
        return _lower_call(node, context)
    raise DslError(f"unsupported node {node!r}")


def _lower_call(node: Call, context: LowerContext) -> Lowered:  # noqa: C901 - one closed dispatch
    name = node.name
    if name in QUARTER_FUNCTIONS and context.grid != "quarter":
        raise DslError(f"{name!r} is only valid on the quarter grid, not {context.grid!r}")
    if name in DAILY_FUNCTIONS and context.grid != "day":
        raise DslError(f"{name!r} is only valid on the day grid, not {context.grid!r}")

    inner = lower(node.args[0], context)

    if name in ("tret", "rvol"):
        periods = _integer_literal(node.args[1], function=name, expression_hint="a daily window length")
        if name == "tret":
            previous = f"lag({inner.value_sql}, {periods}) OVER ({_unbounded(context)})"
            value = (
                f"(CASE WHEN {previous} IS NULL OR {previous} = 0 THEN NULL "
                f"ELSE ({inner.value_sql}) / {previous} - 1.0 END)"
            )
            frame = _frame(context, periods)
            availability = f"max({inner.availability_sql}) OVER ({frame})"
            return Lowered(value, availability, inner.max_lag + periods)
        if periods < 2:
            raise DslError("'rvol' requires a non-negative integer literal window of at least 2")
        frame = _frame(context, periods - 1)
        value = (
            f"(CASE WHEN count({inner.value_sql}) OVER ({frame}) = {periods} "
            f"THEN sqrt(252.0) * stddev_samp({inner.value_sql}) OVER ({frame}) END)"
        )
        availability = f"max({inner.availability_sql}) OVER ({frame})"
        return Lowered(value, availability, inner.max_lag + periods - 1)

    if name == "safe_div":
        right = lower(node.args[1], context)
        value = (
            f"(CASE WHEN ({right.value_sql}) IS NULL OR ({right.value_sql}) = 0 THEN NULL "
            f"ELSE ({inner.value_sql}) / ({right.value_sql}) END)"
        )
        return Lowered(
            value,
            _greatest([inner.availability_sql, right.availability_sql]),
            max(inner.max_lag, right.max_lag),
        )
    if name == "coalesce":
        branches = [inner] + [lower(argument, context) for argument in node.args[1:]]
        value = "coalesce(" + ", ".join(f"({branch.value_sql})" for branch in branches) + ")"
        availability = "NULL"
        for branch in reversed(branches):
            availability = (
                f"(CASE WHEN ({branch.value_sql}) IS NOT NULL THEN {branch.availability_sql} "
                f"ELSE {availability} END)"
            )
        return Lowered(value, availability, max(branch.max_lag for branch in branches))
    if name == "abs":
        return Lowered(f"abs({inner.value_sql})", inner.availability_sql, inner.max_lag)
    if name == "ln":
        value = (
            f"(CASE WHEN ({inner.value_sql}) IS NULL OR ({inner.value_sql}) <= 0 "
            f"THEN NULL ELSE ln({inner.value_sql}) END)"
        )
        return Lowered(value, inner.availability_sql, inner.max_lag)
    if name in ("min", "max"):
        right = lower(node.args[1], context)
        sql_function = "least" if name == "min" else "greatest"
        return Lowered(
            f"{sql_function}(({inner.value_sql}), ({right.value_sql}))",
            _greatest([inner.availability_sql, right.availability_sql]),
            max(inner.max_lag, right.max_lag),
        )
    if name in ("indicator_gt", "indicator_lt"):
        right = lower(node.args[1], context)
        comparison = ">" if name == "indicator_gt" else "<"
        value = (
            f"(CASE WHEN ({inner.value_sql}) IS NULL OR ({right.value_sql}) IS NULL THEN NULL "
            f"WHEN ({inner.value_sql}) {comparison} ({right.value_sql}) THEN 1.0 ELSE 0.0 END)"
        )
        return Lowered(
            value,
            _greatest([inner.availability_sql, right.availability_sql]),
            max(inner.max_lag, right.max_lag),
        )

    if name == "ttm":
        frame = _frame(context, 3)
        value = (
            f"(CASE WHEN count({inner.value_sql}) OVER ({frame}) = 4 "
            f"THEN sum({inner.value_sql}) OVER ({frame}) END)"
        )
        availability = f"max({inner.availability_sql}) OVER ({frame})"
        return Lowered(value, availability, inner.max_lag + 3)
    if name == "avg2":
        frame = _frame(context, 4)
        previous = f"lag({inner.value_sql}, 4) OVER ({_unbounded(context)})"
        value = f"(CASE WHEN {previous} IS NULL THEN NULL ELSE (({inner.value_sql}) + {previous}) / 2.0 END)"
        availability = f"max({inner.availability_sql}) OVER ({frame})"
        return Lowered(value, availability, inner.max_lag + 4)
    if name in ("lag", "lag_d"):
        periods = _integer_literal(node.args[1], function=name, expression_hint="a period count")
        frame = _frame(context, periods)
        value = f"lag({inner.value_sql}, {periods}) OVER ({_unbounded(context)})"
        availability = f"lag({inner.availability_sql}, {periods}) OVER ({_unbounded(context)})"
        _ = frame
        return Lowered(value, availability, inner.max_lag + periods)
    if name in ("yoy", "qoq"):
        periods = 4 if name == "yoy" else 1
        frame = _frame(context, periods)
        previous = f"lag({inner.value_sql}, {periods}) OVER ({_unbounded(context)})"
        value = (
            f"(CASE WHEN {previous} IS NULL OR {previous} = 0 THEN NULL "
            f"ELSE (({inner.value_sql}) - {previous}) / abs({previous}) END)"
        )
        availability = f"max({inner.availability_sql}) OVER ({frame})"
        return Lowered(value, availability, inner.max_lag + periods)
    if name == "cagr":
        years = _integer_literal(node.args[1], function="cagr", expression_hint="a year count")
        if years < 1:
            raise DslError("'cagr' requires a non-negative integer literal year count of at least 1")
        periods = 4 * years
        frame = _frame(context, periods)
        previous = f"lag({inner.value_sql}, {periods}) OVER ({_unbounded(context)})"
        value = (
            f"(CASE WHEN ({inner.value_sql}) > 0 AND {previous} > 0 "
            f"THEN power(({inner.value_sql}) / {previous}, 1.0 / {years}.0) - 1.0 END)"
        )
        availability = f"max({inner.availability_sql}) OVER ({frame})"
        return Lowered(value, availability, inner.max_lag + periods)
    if name in ("stdev_q", "avg_d"):
        count = _integer_literal(node.args[1], function=name, expression_hint="a window length")
        if count < 2:
            raise DslError(f"{name!r} requires a non-negative integer literal window of at least 2")
        frame = _frame(context, count - 1)
        aggregate = "stddev_samp" if name == "stdev_q" else "avg"
        value = (
            f"(CASE WHEN count({inner.value_sql}) OVER ({frame}) = {count} "
            f"THEN {aggregate}({inner.value_sql}) OVER ({frame}) END)"
        )
        availability = f"max({inner.availability_sql}) OVER ({frame})"
        return Lowered(value, availability, inner.max_lag + count - 1)
    raise DslError(f"unhandled function {name!r}")


def compile_expression(expression: str, context: LowerContext) -> Lowered:
    return lower(parse_expression(expression), context)
```

- [ ] **Step 4: Add `derived_dsl` to the public API snapshot**

In `C:\atx\atx-db\tests\data\public_api_snapshot.json`, insert `"derived_dsl"` into the `"atx_db"` array so the array stays sorted (it belongs between `"delisting"` and `"discover"`-class neighbours; the test compares against `sorted(dir(atx_db))`, so alphabetical placement is mandatory).

- [ ] **Step 5: Run the tests**

Run: `.venv\Scripts\python.exe -m pytest tests/test_derived_dsl.py -n 0 -q`
Expected: `22 passed`.

Run: `.venv\Scripts\python.exe -m pytest tests/test_module_boundaries.py -n 0 -q`
Expected: `passed` — no import-cycle or private-import violation, and the snapshot matches.

Run: `.venv\Scripts\python.exe -m mypy --strict src/atx_db/derived_dsl.py`
Expected: `Success: no issues found in 1 source file`.

- [ ] **Step 6: Commit**

```
git add src/atx_db/derived_dsl.py tests/test_derived_dsl.py tests/data/public_api_snapshot.json
git commit -m "feat(db): add the derived-metric expression DSL

Hand-written tokenizer plus recursive-descent parser produce a frozen AST for a
restricted arithmetic grammar; a closed lowering emits DuckDB SQL for the value,
a parallel SQL expression for available_at built from the same tree, and the
deepest row offset the expression reaches. No eval, no exec, no DataFrame.eval.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 3: `derived_registry.py` — seed reader, validation, dependency graph, topological order

**Files:**
- Create: `C:\atx\atx-db\src\atx_db\derived_registry.py`
- Create: `C:\atx\atx-db\src\atx_db\seeds\derived_metric_definitions.csv` (header + the 6 bootstrap rows below; Task 4 grows it to the full catalog)
- Modify: `C:\atx\atx-db\tests\data\public_api_snapshot.json` (add `"derived_registry"`)
- Test: `C:\atx\atx-db\tests\test_derived_registry.py` (new)

**Interfaces:**
- Consumes: `atx_db.derived_dsl.parse_expression`, `expression_names`, `DslError`, `QUARTER_FUNCTIONS`, `DAILY_FUNCTIONS`, `Call`, `Node` (Task 2); `atx_db.item_registry.read_fundamental_item_seed` (existing); `atx_db.connection.DuckDBStore` (existing); `atx_db.warehouse.now_utc_naive` (existing).
- Produces:
  - `atx_db.derived_registry.DERIVED_SEED_PATH: Path` — `src/atx_db/seeds/derived_metric_definitions.csv`
  - `atx_db.derived_registry.DERIVED_SEED_COLUMNS: tuple[str, ...]` = `("metric_code", "family", "expression", "window", "inputs", "requires_market", "description", "version")`
  - `atx_db.derived_registry.METRIC_WINDOWS: frozenset[str]` = `{"q", "ttm", "annual", "instant", "avg2", "daily"}`
  - `atx_db.derived_registry.QUARTER_GRID_WINDOWS: frozenset[str]` = `{"q", "ttm", "avg2", "instant"}`
  - `atx_db.derived_registry.MARKET_COLUMNS: frozenset[str]` = `{"close", "adj_close", "log_return", "volume", "archive_shares", "dei_shares", "shares_outstanding"}`
  - `atx_db.derived_registry.DerivedMetricDefinition` — frozen dataclass: `metric_code: str`, `family: str`, `expression: str`, `window: str`, `inputs: tuple[str, ...]` (each `"item:<code>"`, `"metric:<code>"` or `"market:<column>"`), `requires_market: bool`, `description: str`, `version: str`; properties `item_inputs`, `metric_inputs`, `market_inputs` returning the bare codes.
  - `atx_db.derived_registry.read_derived_seed(path: Path | str = DERIVED_SEED_PATH) -> tuple[DerivedMetricDefinition, ...]`
  - `atx_db.derived_registry.default_derived_definitions() -> tuple[DerivedMetricDefinition, ...]` — `lru_cache`d `read_derived_seed()`
  - `atx_db.derived_registry.validate_definitions(definitions, *, item_codes: frozenset[str]) -> None` — raises `DerivedRegistryError`
  - `atx_db.derived_registry.topological_order(definitions) -> tuple[DerivedMetricDefinition, ...]` — Kahn's algorithm, ties broken by `metric_code` so the order is byte-stable; raises `DerivedRegistryError` naming the cycle
  - `atx_db.derived_registry.known_item_codes(seed_path: str | None = None) -> frozenset[str]` — distinct `canonical_code` from `seeds/fundamental_items.csv`
  - `atx_db.derived_registry.derived_statement_item_codes(seed_path: str | None = None) -> frozenset[str]` — the `canonical_code`s whose `statement` is `derived` (the 27 dead registry rows identified in audit §2.4 / §9: `roe`, `roa`, `roic`, `p_e`, `p_b`, `p_s`, `market_cap`, `enterprise_value`, `ev_ebitda`, `ev_sales`, `net_margin`, `ebitda_margin`, `current_ratio`, `debt_equity`, `dividend_yield`, `interest_coverage`, `sales_per_share`, `cash_per_share`, `cash_conversion_cycle`, `dio_days`, `dpo_days`, `dso_days`, `tangible_book_value_per_share`, `gross_margin__1414`, `operating_margin__1415`, `book_value_per_share__1401`, `fcf_per_share__1403`)
  - `atx_db.derived_registry.RECLAIMED_ITEM_CODES: frozenset[str]` — the derived-statement codes this sprint deliberately reclaims for the engine
  - `atx_db.derived_registry.seed_derived_metric_definitions(store: DuckDBStore, definitions=None) -> int` — truncate-and-insert into `derived_metric_definitions` with `topological_rank`, `ORDER BY metric_code`
  - `atx_db.derived_registry.DerivedRegistryError(ValueError)`

**Validation rules** (each has a test):
1. `metric_code` matches `^[a-z][a-z0-9_]{2,63}$` and is unique.
2. `window` ∈ `METRIC_WINDOWS`.
3. `requires_market` is `"true"`/`"false"` (lower-case, exact).
4. Every input carries a namespace: `item:`, `metric:` or `market:`.
5. `market:` inputs appear only on `window="daily"` rows, and every `window="daily"` row has `requires_market = true`; conversely `requires_market = true` implies `window = "daily"`.
6. `market:` column names are in `MARKET_COLUMNS`.
7. `item:` codes exist in `seeds/fundamental_items.csv`.
8. `metric:` codes exist in the seed.
9. The bare names in the parsed expression are exactly the set of bare input codes — no unused input, no undeclared reference.
10. A bare name resolves to exactly one namespace within a row (no `item:revenue` and `metric:revenue` in the same row).
11. **A `metric_code` must not equal a `canonical_code` in `fundamental_items.csv` that belongs to a real statement** (`income`, `balance`, `cashflow`, `bank`, `insurance`, `reit`, `utility`, `broker_dealer`). A collision with a `statement = derived` code is permitted **only** when the code is listed in `RECLAIMED_ITEM_CODES`. This is the resolution of the audit §9 "two sources of truth" hazard: the 27 `derived`-statement registry rows have no source alias and no source item, can never emit a `fundamental_standardized` row, and are hereby declared owned by the derived engine instead. A test asserts every member of `RECLAIMED_ITEM_CODES` is in `derived_statement_item_codes()`, so the set can never quietly shadow a live item.
12. Quarter-grid functions appear only on rows whose `window` ∈ `QUARTER_GRID_WINDOWS`; daily functions only on `window = "daily"`.
13. If a row applies a quarter-grid window function to a `metric:` reference, that referenced metric's `window` must be in `QUARTER_GRID_WINDOWS` (its values sit on the same quarterly `period_end` grid).
14. A `window="daily"` row may reference `metric:` codes of any quarter-grid window (they are ASOF-joined) but may not apply a quarter-grid function to them.
15. The dependency graph over `metric:` edges is acyclic.

**Bootstrap seed content** for this task (Task 4 replaces the body with the full catalog; the header never changes):

```csv
metric_code,family,expression,window,inputs,requires_market,description,version
gross_profit_q,rollup,"coalesce(gross_profit__1004, revenue - cost_of_revenue_cogs)",q,item:gross_profit__1004|item:revenue|item:cost_of_revenue_cogs,false,Quarterly gross profit with a revenue-less-COGS fallback.,1
revenue_ttm,rollup,ttm(revenue),ttm,item:revenue,false,Trailing-twelve-month total revenue.,1
gross_profit_ttm,rollup,ttm(gross_profit_q),ttm,metric:gross_profit_q,false,Trailing-twelve-month gross profit.,1
gross_margin,profitability,"safe_div(gross_profit_ttm, revenue_ttm)",ttm,metric:gross_profit_ttm|metric:revenue_ttm,false,Trailing-twelve-month gross margin.,1
total_assets_avg2,rollup,avg2(total_assets),avg2,item:total_assets,false,Average of current and four-quarters-prior total assets.,1
gross_profitability,profitability,"safe_div(gross_profit_ttm, total_assets_avg2)",ttm,metric:gross_profit_ttm|metric:total_assets_avg2,false,Novy-Marx gross profitability on average assets.,1
```

- [ ] **Step 1: Write the failing test**

Create `C:\atx\atx-db\tests\test_derived_registry.py`:

```python
"""Tier1-S3 T3: the derived-metric seed registry, its validation and its ordering."""

from __future__ import annotations

import csv

import pytest

from atx_db.derived_registry import (
    DERIVED_SEED_COLUMNS,
    DERIVED_SEED_PATH,
    METRIC_WINDOWS,
    RECLAIMED_ITEM_CODES,
    DerivedMetricDefinition,
    DerivedRegistryError,
    default_derived_definitions,
    derived_statement_item_codes,
    known_item_codes,
    read_derived_seed,
    seed_derived_metric_definitions,
    topological_order,
    validate_definitions,
)

_ITEMS = frozenset({"revenue", "cost_of_revenue_cogs", "gross_profit__1004", "total_assets"})


def _definition(**overrides) -> DerivedMetricDefinition:
    base = dict(
        metric_code="gross_margin",
        family="profitability",
        expression="safe_div(revenue, total_assets)",
        window="ttm",
        inputs=("item:revenue", "item:total_assets"),
        requires_market=False,
        description="Test metric.",
        version="1",
    )
    base.update(overrides)
    return DerivedMetricDefinition(**base)


def test_seed_header_is_the_charter_header():
    with DERIVED_SEED_PATH.open("r", encoding="utf-8", newline="") as handle:
        header = tuple(next(csv.reader(handle)))
    assert header == DERIVED_SEED_COLUMNS
    assert DERIVED_SEED_COLUMNS == (
        "metric_code",
        "family",
        "expression",
        "window",
        "inputs",
        "requires_market",
        "description",
        "version",
    )


def test_seed_rows_round_trip_into_definitions():
    definitions = read_derived_seed()
    assert definitions == default_derived_definitions()
    codes = {definition.metric_code for definition in definitions}
    assert {"revenue_ttm", "gross_profit_ttm", "gross_margin"} <= codes
    ttm = next(d for d in definitions if d.metric_code == "revenue_ttm")
    assert ttm.item_inputs == ("revenue",)
    assert ttm.metric_inputs == ()
    assert ttm.window in METRIC_WINDOWS


def test_the_shipped_seed_validates_against_the_item_registry():
    validate_definitions(default_derived_definitions(), item_codes=known_item_codes())


def test_known_item_codes_reads_the_fundamental_item_seed():
    codes = known_item_codes()
    assert "revenue" in codes
    assert "total_assets" in codes
    assert "cash_flow_from_operations" in codes


def test_undeclared_reference_is_rejected():
    with pytest.raises(DerivedRegistryError) as excinfo:
        validate_definitions(
            (_definition(inputs=("item:revenue",)),),
            item_codes=_ITEMS,
        )
    assert "total_assets" in str(excinfo.value)


def test_unused_declared_input_is_rejected():
    with pytest.raises(DerivedRegistryError) as excinfo:
        validate_definitions(
            (_definition(inputs=("item:revenue", "item:total_assets", "item:inventory")),),
            item_codes=_ITEMS | {"inventory"},
        )
    assert "inventory" in str(excinfo.value)


def test_unknown_item_code_is_rejected():
    with pytest.raises(DerivedRegistryError) as excinfo:
        validate_definitions(
            (_definition(expression="safe_div(revenue, nonsense)", inputs=("item:revenue", "item:nonsense")),),
            item_codes=_ITEMS,
        )
    assert "nonsense" in str(excinfo.value)


def test_metric_code_colliding_with_an_item_code_is_rejected():
    with pytest.raises(DerivedRegistryError) as excinfo:
        validate_definitions((_definition(metric_code="revenue"),), item_codes=_ITEMS)
    assert "collides" in str(excinfo.value)


def test_every_reclaimed_code_is_a_dead_derived_statement_row():
    dead = derived_statement_item_codes()
    assert RECLAIMED_ITEM_CODES <= dead, sorted(RECLAIMED_ITEM_CODES - dead)
    assert "revenue" not in dead
    assert "total_assets" not in dead


def test_a_reclaimed_derived_statement_code_is_allowed_as_a_metric_code():
    validate_definitions(
        (_definition(metric_code="roa"),),
        item_codes=_ITEMS | {"roa"},
        reclaimable_codes=frozenset({"roa"}),
    )


def test_market_input_outside_the_daily_window_is_rejected():
    with pytest.raises(DerivedRegistryError) as excinfo:
        validate_definitions(
            (
                _definition(
                    metric_code="bad_market",
                    expression="safe_div(revenue, close)",
                    inputs=("item:revenue", "market:close"),
                    window="ttm",
                ),
            ),
            item_codes=_ITEMS,
        )
    assert "daily" in str(excinfo.value)


def test_daily_window_requires_the_market_flag():
    with pytest.raises(DerivedRegistryError):
        validate_definitions(
            (
                _definition(
                    metric_code="bad_flag",
                    expression="safe_div(revenue, close)",
                    inputs=("item:revenue", "market:close"),
                    window="daily",
                    requires_market=False,
                ),
            ),
            item_codes=_ITEMS,
        )


def test_daily_function_in_a_quarterly_metric_is_rejected():
    with pytest.raises(DerivedRegistryError) as excinfo:
        validate_definitions(
            (_definition(metric_code="bad_fn", expression="tret(21)", inputs=()),),
            item_codes=_ITEMS,
        )
    assert "tret" in str(excinfo.value)


def test_quarter_window_function_on_a_non_grid_metric_reference_is_rejected():
    definitions = (
        _definition(
            metric_code="annual_thing",
            expression="revenue",
            inputs=("item:revenue",),
            window="annual",
        ),
        _definition(
            metric_code="bad_grid",
            expression="ttm(annual_thing)",
            inputs=("metric:annual_thing",),
            window="ttm",
        ),
    )
    with pytest.raises(DerivedRegistryError) as excinfo:
        validate_definitions(definitions, item_codes=_ITEMS)
    assert "annual_thing" in str(excinfo.value)


def test_topological_order_places_dependencies_first():
    ordered = topological_order(default_derived_definitions())
    positions = {definition.metric_code: index for index, definition in enumerate(ordered)}
    assert positions["gross_profit_q"] < positions["gross_profit_ttm"]
    assert positions["gross_profit_ttm"] < positions["gross_margin"]
    assert positions["total_assets_avg2"] < positions["gross_profitability"]


def test_topological_order_is_stable_across_input_permutations():
    definitions = default_derived_definitions()
    forward = [d.metric_code for d in topological_order(definitions)]
    backward = [d.metric_code for d in topological_order(tuple(reversed(definitions)))]
    assert forward == backward


def test_cycle_is_rejected_by_name():
    cyclic = (
        _definition(metric_code="alpha_metric", expression="beta_metric", inputs=("metric:beta_metric",)),
        _definition(metric_code="beta_metric", expression="alpha_metric", inputs=("metric:alpha_metric",)),
    )
    with pytest.raises(DerivedRegistryError) as excinfo:
        topological_order(cyclic)
    message = str(excinfo.value)
    assert "alpha_metric" in message and "beta_metric" in message


def test_seeding_writes_every_definition_with_its_rank(tmp_store):
    count = seed_derived_metric_definitions(tmp_store)
    assert count == len(default_derived_definitions())
    rows = tmp_store.con.execute(
        "SELECT metric_code, metric_window, topological_rank FROM derived_metric_definitions ORDER BY metric_code"
    ).fetchall()
    assert len(rows) == count
    ranks = {str(code): int(rank) for code, _window, rank in rows}
    assert ranks["gross_profit_q"] < ranks["gross_profit_ttm"] < ranks["gross_margin"]


def test_seeding_is_idempotent(tmp_store):
    first = seed_derived_metric_definitions(tmp_store)
    second = seed_derived_metric_definitions(tmp_store)
    assert first == second
    total = tmp_store.con.execute("SELECT count(*) FROM derived_metric_definitions").fetchone()[0]
    assert total == first
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `.venv\Scripts\python.exe -m pytest tests/test_derived_registry.py -n 0 -q`
Expected: FAIL — `ModuleNotFoundError: No module named 'atx_db.derived_registry'`.

- [ ] **Step 3: Create `src/atx_db/seeds/derived_metric_definitions.csv`**

Write the header plus the six bootstrap rows exactly as given in the "Bootstrap seed content" block above. Rows are sorted by `metric_code`? **No** — the file is sorted by `(family, metric_code)` so related metrics stay adjacent for review; `read_derived_seed` does not depend on file order and `topological_order` re-sorts, so a sorting guard is unnecessary. The only ordering test is `test_topological_order_is_stable_across_input_permutations`.

- [ ] **Step 4: Create `src/atx_db/derived_registry.py`**

```python
"""The declarative derived-metric catalog: seed I/O, validation, and ordering."""

from __future__ import annotations

import csv
import json
import re
from dataclasses import dataclass
from functools import lru_cache
from pathlib import Path
from typing import Iterable

from .connection import DuckDBStore
from .derived_dsl import (
    DAILY_FUNCTIONS,
    QUARTER_FUNCTIONS,
    Call,
    DslError,
    Node,
    BinOp,
    Neg,
    Ref,
    expression_names,
    parse_expression,
)
from .item_registry import read_fundamental_item_seed
from .warehouse import now_utc_naive

__all__ = [
    "DERIVED_SEED_COLUMNS",
    "DERIVED_SEED_PATH",
    "MARKET_COLUMNS",
    "METRIC_WINDOWS",
    "QUARTER_GRID_WINDOWS",
    "RECLAIMED_ITEM_CODES",
    "DerivedMetricDefinition",
    "DerivedRegistryError",
    "default_derived_definitions",
    "derived_statement_item_codes",
    "known_item_codes",
    "read_derived_seed",
    "seed_derived_metric_definitions",
    "topological_order",
    "validate_definitions",
]

DERIVED_SEED_PATH = Path(__file__).resolve().parent / "seeds" / "derived_metric_definitions.csv"
DERIVED_SEED_COLUMNS = (
    "metric_code",
    "family",
    "expression",
    "window",
    "inputs",
    "requires_market",
    "description",
    "version",
)
METRIC_WINDOWS = frozenset({"q", "ttm", "annual", "instant", "avg2", "daily"})
QUARTER_GRID_WINDOWS = frozenset({"q", "ttm", "avg2", "instant"})
MARKET_COLUMNS = frozenset(
    {"close", "adj_close", "log_return", "volume", "archive_shares", "dei_shares", "shares_outstanding"}
)
_METRIC_CODE_RE = re.compile(r"^[a-z][a-z0-9_]{2,63}$")
_NAMESPACES = ("item:", "metric:", "market:")
DERIVED_SOURCE_NAME = "atx-db declarative derived metrics v1"

#: Registry codes on the ``derived`` statement that the engine deliberately owns.
#: Every member must be in :func:`derived_statement_item_codes`; those rows have
#: no source alias and no source item, so they can never emit a standardized row.
RECLAIMED_ITEM_CODES = frozenset(
    {
        "market_cap",
        "enterprise_value",
        "ev_ebitda",
        "ev_sales",
        "roa",
        "roe",
        "roic",
        "net_margin",
        "ebitda_margin",
        "current_ratio",
        "interest_coverage",
        "dividend_yield",
        "sales_per_share",
        "cash_per_share",
        "tangible_book_value_per_share",
    }
)


class DerivedRegistryError(ValueError):
    """A malformed derived-metric catalog."""


@dataclass(frozen=True)
class DerivedMetricDefinition:
    metric_code: str
    family: str
    expression: str
    window: str
    inputs: tuple[str, ...]
    requires_market: bool
    description: str
    version: str

    def _bare(self, prefix: str) -> tuple[str, ...]:
        return tuple(value[len(prefix) :] for value in self.inputs if value.startswith(prefix))

    @property
    def item_inputs(self) -> tuple[str, ...]:
        return self._bare("item:")

    @property
    def metric_inputs(self) -> tuple[str, ...]:
        return self._bare("metric:")

    @property
    def market_inputs(self) -> tuple[str, ...]:
        return self._bare("market:")

    @property
    def bare_names(self) -> tuple[str, ...]:
        return tuple(sorted(self.item_inputs + self.metric_inputs + self.market_inputs))


def _fail(message: str) -> None:
    raise DerivedRegistryError(message)


def read_derived_seed(path: Path | str = DERIVED_SEED_PATH) -> tuple[DerivedMetricDefinition, ...]:
    seed_path = Path(path)
    definitions: list[DerivedMetricDefinition] = []
    with seed_path.open("r", encoding="utf-8", newline="") as handle:
        reader = csv.DictReader(handle)
        if tuple(reader.fieldnames or ()) != DERIVED_SEED_COLUMNS:
            _fail(f"{seed_path} header must be {DERIVED_SEED_COLUMNS}, got {reader.fieldnames}")
        for row_number, raw in enumerate(reader, start=2):
            flag = (raw["requires_market"] or "").strip()
            if flag not in ("true", "false"):
                _fail(f"{seed_path}:{row_number} requires_market must be 'true' or 'false', got {flag!r}")
            inputs = tuple(part for part in (raw["inputs"] or "").split("|") if part)
            definitions.append(
                DerivedMetricDefinition(
                    metric_code=(raw["metric_code"] or "").strip(),
                    family=(raw["family"] or "").strip(),
                    expression=(raw["expression"] or "").strip(),
                    window=(raw["window"] or "").strip(),
                    inputs=inputs,
                    requires_market=flag == "true",
                    description=(raw["description"] or "").strip(),
                    version=(raw["version"] or "").strip(),
                )
            )
    return tuple(definitions)


@lru_cache(maxsize=1)
def default_derived_definitions() -> tuple[DerivedMetricDefinition, ...]:
    return read_derived_seed()


@lru_cache(maxsize=1)
def known_item_codes(seed_path: str | None = None) -> frozenset[str]:
    rows = read_fundamental_item_seed() if seed_path is None else read_fundamental_item_seed(seed_path)
    return frozenset(row.canonical_code for row in rows)


@lru_cache(maxsize=1)
def derived_statement_item_codes(seed_path: str | None = None) -> frozenset[str]:
    rows = read_fundamental_item_seed() if seed_path is None else read_fundamental_item_seed(seed_path)
    return frozenset(row.canonical_code for row in rows if row.statement == "derived")


def _window_calls(node: Node) -> tuple[Call, ...]:
    found: list[Call] = []

    def walk(current: Node) -> None:
        if isinstance(current, Call):
            if current.name in QUARTER_FUNCTIONS or current.name in DAILY_FUNCTIONS:
                found.append(current)
            for argument in current.args:
                walk(argument)
        elif isinstance(current, BinOp):
            walk(current.left)
            walk(current.right)
        elif isinstance(current, Neg):
            walk(current.operand)

    walk(node)
    return tuple(found)


def _direct_refs(node: Node) -> tuple[str, ...]:
    if isinstance(node, Ref):
        return (node.name,)
    if isinstance(node, Neg):
        return _direct_refs(node.operand)
    if isinstance(node, BinOp):
        return _direct_refs(node.left) + _direct_refs(node.right)
    if isinstance(node, Call):
        names: list[str] = []
        for argument in node.args:
            names.extend(_direct_refs(argument))
        return tuple(names)
    return ()


def validate_definitions(
    definitions: Iterable[DerivedMetricDefinition],
    *,
    item_codes: frozenset[str],
    reclaimable_codes: frozenset[str] | None = None,
) -> None:
    rows = tuple(definitions)
    reclaimable = RECLAIMED_ITEM_CODES if reclaimable_codes is None else reclaimable_codes
    by_code: dict[str, DerivedMetricDefinition] = {}
    for definition in rows:
        if not _METRIC_CODE_RE.fullmatch(definition.metric_code):
            _fail(f"metric_code {definition.metric_code!r} must match {_METRIC_CODE_RE.pattern}")
        if definition.metric_code in by_code:
            _fail(f"duplicate metric_code {definition.metric_code!r}")
        if definition.metric_code in item_codes and definition.metric_code not in reclaimable:
            _fail(
                f"metric_code {definition.metric_code!r} collides with a fundamental_items canonical_code; "
                "derived metric codes and standardized item codes share one namespace for consumers. "
                "Add it to RECLAIMED_ITEM_CODES only if it is a dead 'derived'-statement registry row."
            )
        by_code[definition.metric_code] = definition

    for definition in rows:
        where = f"metric {definition.metric_code!r}"
        if definition.window not in METRIC_WINDOWS:
            _fail(f"{where}: window {definition.window!r} not in {sorted(METRIC_WINDOWS)}")
        if definition.requires_market != (definition.window == "daily"):
            _fail(f"{where}: requires_market must be true if and only if window is 'daily'")
        for value in definition.inputs:
            if not value.startswith(_NAMESPACES):
                _fail(f"{where}: input {value!r} must start with one of {_NAMESPACES}")
        if definition.market_inputs and definition.window != "daily":
            _fail(f"{where}: market: inputs are only valid on the daily window")
        for column in definition.market_inputs:
            if column not in MARKET_COLUMNS:
                _fail(f"{where}: unknown market column {column!r}; expected one of {sorted(MARKET_COLUMNS)}")
        for code in definition.item_inputs:
            if code not in item_codes:
                _fail(f"{where}: item code {code!r} is not a canonical_code in seeds/fundamental_items.csv")
        for code in definition.metric_inputs:
            if code not in by_code:
                _fail(f"{where}: metric code {code!r} is not defined in the derived seed")
        if len(definition.bare_names) != len(set(definition.bare_names)):
            _fail(f"{where}: a bare name is declared in two namespaces")

        try:
            node = parse_expression(definition.expression)
        except DslError as error:
            raise DerivedRegistryError(f"{where}: {error}") from error
        used = set(expression_names(node))
        declared = set(definition.bare_names)
        for name in sorted(used - declared):
            _fail(f"{where}: expression references {name!r} which is not declared in inputs")
        for name in sorted(declared - used):
            _fail(f"{where}: input {name!r} is declared but never referenced by the expression")

        for call in _window_calls(node):
            if call.name in DAILY_FUNCTIONS and definition.window != "daily":
                _fail(f"{where}: daily function {call.name!r} requires window 'daily'")
            if call.name in QUARTER_FUNCTIONS:
                if definition.window not in QUARTER_GRID_WINDOWS:
                    _fail(
                        f"{where}: quarter function {call.name!r} requires a quarter-grid window "
                        f"({sorted(QUARTER_GRID_WINDOWS)}), got {definition.window!r}"
                    )
                for name in _direct_refs(call.args[0]):
                    referenced = by_code.get(name)
                    if referenced is not None and referenced.window not in QUARTER_GRID_WINDOWS:
                        _fail(
                            f"{where}: {call.name!r} is applied to metric {name!r} whose window "
                            f"{referenced.window!r} is not on the quarterly period_end grid"
                        )


def topological_order(
    definitions: Iterable[DerivedMetricDefinition],
) -> tuple[DerivedMetricDefinition, ...]:
    rows = {definition.metric_code: definition for definition in definitions}
    pending = {code: set(row.metric_inputs) & set(rows) for code, row in rows.items()}
    ordered: list[DerivedMetricDefinition] = []
    ready = sorted(code for code, deps in pending.items() if not deps)
    while ready:
        code = ready.pop(0)
        ordered.append(rows[code])
        del pending[code]
        released: list[str] = []
        for other, deps in pending.items():
            if code in deps:
                deps.discard(code)
                if not deps:
                    released.append(other)
        for other in sorted(released):
            ready.append(other)
        ready.sort()
    if pending:
        _fail(f"derived metric dependency cycle among: {sorted(pending)}")
    return tuple(ordered)


def seed_derived_metric_definitions(
    store: DuckDBStore,
    definitions: Iterable[DerivedMetricDefinition] | None = None,
) -> int:
    rows = tuple(definitions) if definitions is not None else default_derived_definitions()
    validate_definitions(rows, item_codes=known_item_codes())
    ordered = topological_order(rows)
    rank_by_code = {definition.metric_code: index for index, definition in enumerate(ordered)}
    stamp = now_utc_naive()
    payload = [
        (
            definition.metric_code,
            definition.family,
            definition.expression,
            definition.window,
            json.dumps(list(definition.inputs), separators=(",", ":")),
            definition.requires_market,
            definition.description,
            definition.version,
            rank_by_code[definition.metric_code],
            stamp,
        )
        for definition in sorted(rows, key=lambda row: row.metric_code)
    ]
    with store.transaction():
        store.con.execute("DELETE FROM derived_metric_definitions")
        store.con.executemany(
            """
            INSERT INTO derived_metric_definitions (
                metric_code, family, expression, metric_window, inputs_json,
                requires_market, description, version, topological_rank, seeded_at
            ) VALUES (?,?,?,?,?,?,?,?,?,?)
            """,
            payload,
        )
    return len(payload)
```

- [ ] **Step 5: Add `derived_registry` to the public API snapshot**

Insert `"derived_registry"` into the `"atx_db"` array in `tests/data/public_api_snapshot.json`, immediately after `"derived_dsl"`.

- [ ] **Step 6: Run the tests**

Run: `.venv\Scripts\python.exe -m pytest tests/test_derived_registry.py tests/test_derived_dsl.py -n 0 -q`
Expected: `41 passed`.

Run: `.venv\Scripts\python.exe -m pytest tests/test_module_boundaries.py -n 0 -q`
Expected: `passed`.

Run: `.venv\Scripts\python.exe -m mypy --strict src/atx_db/derived_registry.py`
Expected: `Success: no issues found in 1 source file`.

- [ ] **Step 7: Commit**

```
git add src/atx_db/derived_registry.py src/atx_db/seeds/derived_metric_definitions.csv tests/test_derived_registry.py tests/data/public_api_snapshot.json
git commit -m "feat(db): add the derived-metric seed registry and dependency ordering

Reads seeds/derived_metric_definitions.csv into frozen definitions, validates
namespaces, window/grid compatibility and metric-vs-item code collisions, builds
the metric dependency graph with a stable Kahn ordering, and seeds
derived_metric_definitions with a topological_rank.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 4: The derived catalog — full seed content and catalog tests

**Files:**
- Modify: `C:\atx\atx-db\src\atx_db\seeds\derived_metric_definitions.csv` (replace the six bootstrap rows with the full catalog)
- Test: `C:\atx\atx-db\tests\test_derived_catalog.py` (new)

**Interfaces:**
- Consumes: `atx_db.derived_registry.default_derived_definitions`, `validate_definitions`, `topological_order`, `known_item_codes`, `derived_statement_item_codes`, `RECLAIMED_ITEM_CODES`, `METRIC_WINDOWS` (Task 3); `atx_db.derived_dsl.parse_expression` (Task 2).
- Produces: no new Python symbols. The catalog itself is the deliverable: **173 metric definitions**, of which **30 carry `window=daily`** and match the 30 published columns of `market_daily_metrics` one-for-one.

**Item-code reality check (verified against `seeds/fundamental_items.csv` on `main`).** Six codes the spec's derived families need carry *zero* alias rows today and therefore never emit a `fundamental_standardized` value until Sprint 2 lands: `cash_and_st_investments`, `common_equity`, `short_term_debt`, `ebitda_standardised`, `operating_lease_liability`, `change_in_working_capital` (audit §2.4). Every expression that needs one of them goes through a `coalesce` fallback metric (`cash_st_investments_q`, `common_equity_q`, `total_debt_q`, `ebitda_q`) so the catalog produces values on today's warehouse and silently improves when Sprint 2 fills the aliases. `total_debt` **is** a live `balance` item code, so the fallback metric is named `total_debt_q`, not `total_debt`. `effective_tax_rate` is an `estimate` item code, so the metric is `effective_tax_rate_ttm`.

- [ ] **Step 1: Write the failing test**

Create `C:\atx\atx-db\tests\test_derived_catalog.py`:

```python
"""Tier1-S3 T4: the shipped derived catalog is complete, well-formed and spec-aligned."""

from __future__ import annotations

import pytest

from atx_db.derived_dsl import parse_expression
from atx_db.derived_registry import (
    METRIC_WINDOWS,
    RECLAIMED_ITEM_CODES,
    default_derived_definitions,
    derived_statement_item_codes,
    known_item_codes,
    topological_order,
    validate_definitions,
)

EXPECTED_METRIC_COUNT = 173
EXPECTED_DAILY_COUNT = 30

# Every family named in the spec section "Derived metric catalog", plus the
# rollup family that carries the TTM sums and avg2 balances the spec requires.
EXPECTED_FAMILIES = {
    "rollup",
    "per_share",
    "profitability",
    "growth",
    "leverage",
    "quality",
    "investment",
    "payout",
    "market",
}

# Named, one-for-one, from the spec's family lists.
SPEC_REQUIRED_METRICS = {
    "eps_ttm",
    "sales_per_share",
    "book_per_share",
    "cfo_per_share",
    "fcf_per_share",
    "dividends_per_share",
    "market_cap",
    "enterprise_value",
    "ev_ebitda",
    "ev_sales",
    "pe_ttm",
    "pb",
    "ps_ttm",
    "pcf_ttm",
    "fcf_yield",
    "dividend_yield",
    "earnings_yield",
    "shareholder_yield",
    "gross_margin",
    "operating_margin",
    "net_margin",
    "ebitda_margin",
    "roa",
    "roe",
    "roic",
    "roic_ex_goodwill",
    "gross_profitability",
    "cash_profitability",
    "asset_turnover",
    "revenue_growth_yoy",
    "gross_profit_growth_yoy",
    "operating_income_growth_yoy",
    "net_income_growth_yoy",
    "eps_diluted_growth_yoy",
    "cfo_growth_yoy",
    "fcf_growth_yoy",
    "asset_growth",
    "shares_growth_yoy",
    "book_value_growth_yoy",
    "capex_growth_yoy",
    "revenue_cagr_1y",
    "revenue_cagr_3y",
    "total_debt_q",
    "net_debt",
    "debt_to_equity",
    "debt_to_assets",
    "net_debt_ebitda",
    "interest_coverage",
    "current_ratio",
    "quick_ratio",
    "cash_ratio",
    "total_accruals",
    "percent_accruals",
    "noa",
    "delta_noa",
    "piotroski_f",
    "altman_z",
    "beneish_m",
    "ohlson_o",
    "earnings_variability",
    "capex_to_depreciation",
    "capex_to_sales",
    "external_financing",
    "net_equity_issuance",
    "net_debt_issuance",
    "payout_ratio",
    "buyback_yield",
    "total_payout_yield",
}

MARKET_DAILY_COLUMNS = {
    "market_cap",
    "enterprise_value",
    "pe_ttm",
    "pb",
    "ps_ttm",
    "pcf_ttm",
    "ev_ebitda",
    "ev_sales",
    "fcf_yield",
    "dividend_yield",
    "earnings_yield",
    "shareholder_yield",
    "net_payout_yield",
    "total_payout_yield",
    "buyback_yield",
    "book_to_market",
    "rd_to_market_equity",
    "gross_profit_to_ev",
    "cfo_to_ev",
    "ebit_to_ev",
    "sales_to_ev",
    "altman_z",
    "total_return_1m",
    "total_return_3m",
    "total_return_6m",
    "total_return_12m",
    "momentum_12_1",
    "realized_vol_60d",
    "realized_vol_252d",
    "dollar_volume_20d",
}


@pytest.fixture(scope="module")
def definitions():
    return default_derived_definitions()


def test_catalog_has_the_expected_size(definitions):
    assert len(definitions) == EXPECTED_METRIC_COUNT
    daily = [d for d in definitions if d.window == "daily"]
    assert len(daily) == EXPECTED_DAILY_COUNT


def test_catalog_validates(definitions):
    validate_definitions(definitions, item_codes=known_item_codes())


def test_every_expression_parses(definitions):
    for definition in definitions:
        parse_expression(definition.expression)


def test_families_are_exactly_the_spec_families(definitions):
    assert {definition.family for definition in definitions} == EXPECTED_FAMILIES


def test_every_spec_named_metric_is_present(definitions):
    codes = {definition.metric_code for definition in definitions}
    assert sorted(SPEC_REQUIRED_METRICS - codes) == []


def test_daily_metric_codes_match_the_published_columns(definitions):
    daily = {d.metric_code for d in definitions if d.window == "daily"}
    assert daily == MARKET_DAILY_COLUMNS


def test_every_window_is_known(definitions):
    assert {definition.window for definition in definitions} <= METRIC_WINDOWS


def test_every_reclaimed_code_that_is_used_is_a_dead_derived_row(definitions):
    codes = {definition.metric_code for definition in definitions}
    used = codes & known_item_codes()
    assert used <= RECLAIMED_ITEM_CODES
    assert used <= derived_statement_item_codes()


def test_catalog_is_acyclic_and_orders_stably(definitions):
    ordered = topological_order(definitions)
    assert len(ordered) == len(definitions)
    positions = {d.metric_code: i for i, d in enumerate(ordered)}
    for definition in definitions:
        for dependency in definition.metric_inputs:
            assert positions[dependency] < positions[definition.metric_code]


def test_descriptions_and_versions_are_populated(definitions):
    for definition in definitions:
        assert definition.description.endswith(".")
        assert definition.version == "1"


def test_no_metric_is_defined_twice(definitions):
    codes = [definition.metric_code for definition in definitions]
    assert len(codes) == len(set(codes))
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `.venv\Scripts\python.exe -m pytest tests/test_derived_catalog.py -n 0 -q`
Expected: FAIL — `assert 6 == 173` in `test_catalog_has_the_expected_size`.

- [ ] **Step 3: Write the full catalog**

Replace the body of `src/atx_db/seeds/derived_metric_definitions.csv` with exactly these rows, keeping the header line unchanged. Fields containing a comma are double-quoted, per RFC 4180 and `csv.DictReader` defaults.

```csv
metric_code,family,expression,window,inputs,requires_market,description,version
gross_profit_q,rollup,"coalesce(gross_profit__1004, revenue - cost_of_revenue_cogs)",q,item:gross_profit__1004|item:revenue|item:cost_of_revenue_cogs,false,Quarterly gross profit with a revenue-less-cost-of-revenue fallback.,1
ebitda_q,rollup,"coalesce(ebitda_standardised, operating_income + d_and_a_income_statement, operating_income + d_and_a_cash_flow)",q,item:ebitda_standardised|item:operating_income|item:d_and_a_income_statement|item:d_and_a_cash_flow,false,Quarterly EBITDA with two composition fallbacks.,1
cash_st_investments_q,rollup,"coalesce(cash_and_st_investments, cash_only + short_term_investments, cash_only)",q,item:cash_and_st_investments|item:cash_only|item:short_term_investments,false,Cash and short-term investments with composition fallbacks.,1
common_equity_q,rollup,"coalesce(common_equity, stockholders_equity - preferred_stock, stockholders_equity)",q,item:common_equity|item:stockholders_equity|item:preferred_stock,false,Common equity with a preferred-stock deduction fallback.,1
total_debt_q,rollup,"coalesce(total_debt, coalesce(short_term_debt, 0) + coalesce(long_term_debt, 0))",q,item:total_debt|item:short_term_debt|item:long_term_debt,false,Interest-bearing debt with a short-plus-long composition fallback.,1
net_debt,rollup,total_debt_q - cash_st_investments_q,q,metric:total_debt_q|metric:cash_st_investments_q,false,Total debt less cash and short-term investments.,1
capex_q,rollup,abs(capex__1305),q,item:capex__1305,false,Quarterly capital expenditure as a positive magnitude.,1
fcf_q,rollup,cash_flow_from_operations - capex_q,q,item:cash_flow_from_operations|metric:capex_q,false,Quarterly free cash flow.,1
invested_capital_q,rollup,"total_debt_q + common_equity_q + coalesce(minority_interest_bs, 0) - cash_st_investments_q",q,metric:total_debt_q|metric:common_equity_q|item:minority_interest_bs|metric:cash_st_investments_q,false,Invested capital net of excess cash.,1
invested_capital_ex_goodwill_q,rollup,"invested_capital_q - coalesce(goodwill, 0)",q,metric:invested_capital_q|item:goodwill,false,Invested capital excluding goodwill.,1
revenue_ttm,rollup,ttm(revenue),ttm,item:revenue,false,Trailing-twelve-month total revenue.,1
cost_of_revenue_ttm,rollup,ttm(cost_of_revenue_cogs),ttm,item:cost_of_revenue_cogs,false,Trailing-twelve-month cost of revenue.,1
gross_profit_ttm,rollup,ttm(gross_profit_q),ttm,metric:gross_profit_q,false,Trailing-twelve-month gross profit.,1
sga_expense_ttm,rollup,ttm(sg_and_a),ttm,item:sg_and_a,false,Trailing-twelve-month selling general and administrative expense.,1
rd_expense_ttm,rollup,ttm(r_and_d_expense),ttm,item:r_and_d_expense,false,Trailing-twelve-month research and development expense.,1
depreciation_ttm,rollup,ttm(d_and_a_cash_flow),ttm,item:d_and_a_cash_flow,false,Trailing-twelve-month depreciation and amortization from the cash-flow statement.,1
operating_income_ttm,rollup,ttm(operating_income),ttm,item:operating_income,false,Trailing-twelve-month operating income.,1
ebitda_ttm,rollup,ttm(ebitda_q),ttm,metric:ebitda_q,false,Trailing-twelve-month EBITDA.,1
interest_expense_ttm,rollup,ttm(interest_expense_total),ttm,item:interest_expense_total,false,Trailing-twelve-month total interest expense.,1
pretax_income_ttm,rollup,ttm(pretax_income),ttm,item:pretax_income,false,Trailing-twelve-month pretax income.,1
income_tax_ttm,rollup,ttm(income_tax_total),ttm,item:income_tax_total,false,Trailing-twelve-month total income tax expense.,1
net_income_ttm,rollup,ttm(net_income_total),ttm,item:net_income_total,false,Trailing-twelve-month net income.,1
net_income_common_ttm,rollup,ttm(net_income_to_common),ttm,item:net_income_to_common,false,Trailing-twelve-month net income available to common.,1
eps_diluted_ttm,rollup,ttm(eps_diluted),ttm,item:eps_diluted,false,Trailing-twelve-month diluted earnings per share.,1
cfo_ttm,rollup,ttm(cash_flow_from_operations),ttm,item:cash_flow_from_operations,false,Trailing-twelve-month cash flow from operations.,1
capex_ttm,rollup,ttm(capex_q),ttm,metric:capex_q,false,Trailing-twelve-month capital expenditure.,1
fcf_ttm,rollup,ttm(fcf_q),ttm,metric:fcf_q,false,Trailing-twelve-month free cash flow.,1
dividends_paid_ttm,rollup,ttm(total_dividends_paid),ttm,item:total_dividends_paid,false,Trailing-twelve-month total dividends paid.,1
common_dividends_ttm,rollup,ttm(common_dividends_paid),ttm,item:common_dividends_paid,false,Trailing-twelve-month common dividends paid.,1
share_repurchase_ttm,rollup,ttm(stock_repurchases_buybacks),ttm,item:stock_repurchases_buybacks,false,Trailing-twelve-month share repurchases.,1
share_issuance_ttm,rollup,ttm(stock_issuance),ttm,item:stock_issuance,false,Trailing-twelve-month share issuance.,1
debt_issuance_ttm,rollup,ttm(lt_debt_issued),ttm,item:lt_debt_issued,false,Trailing-twelve-month long-term debt issued.,1
debt_reduction_ttm,rollup,ttm(lt_debt_repaid),ttm,item:lt_debt_repaid,false,Trailing-twelve-month long-term debt repaid.,1
stock_compensation_ttm,rollup,ttm(stock_based_compensation),ttm,item:stock_based_compensation,false,Trailing-twelve-month stock-based compensation.,1
acquisitions_ttm,rollup,ttm(acquisitions),ttm,item:acquisitions,false,Trailing-twelve-month cash paid for acquisitions.,1
total_payout_ttm,rollup,common_dividends_ttm + share_repurchase_ttm - share_issuance_ttm,ttm,metric:common_dividends_ttm|metric:share_repurchase_ttm|metric:share_issuance_ttm,false,Trailing-twelve-month net payout to common shareholders.,1
total_assets_avg2,rollup,avg2(total_assets),avg2,item:total_assets,false,Average of current and four-quarters-prior total assets.,1
stockholders_equity_avg2,rollup,avg2(stockholders_equity),avg2,item:stockholders_equity,false,Average of current and four-quarters-prior stockholders equity.,1
common_equity_avg2,rollup,avg2(common_equity_q),avg2,metric:common_equity_q,false,Average of current and four-quarters-prior common equity.,1
inventory_avg2,rollup,avg2(inventory),avg2,item:inventory,false,Average of current and four-quarters-prior inventory.,1
receivables_avg2,rollup,avg2(accounts_receivable),avg2,item:accounts_receivable,false,Average of current and four-quarters-prior receivables.,1
payables_avg2,rollup,avg2(accounts_payable),avg2,item:accounts_payable,false,Average of current and four-quarters-prior payables.,1
total_debt_avg2,rollup,avg2(total_debt_q),avg2,metric:total_debt_q,false,Average of current and four-quarters-prior total debt.,1
invested_capital_avg2,rollup,avg2(invested_capital_q),avg2,metric:invested_capital_q,false,Average of current and four-quarters-prior invested capital.,1
invested_capital_ex_goodwill_avg2,rollup,avg2(invested_capital_ex_goodwill_q),avg2,metric:invested_capital_ex_goodwill_q,false,Average invested capital excluding goodwill.,1
eps_ttm,per_share,"safe_div(net_income_common_ttm, weighted_avg_shares_diluted)",ttm,metric:net_income_common_ttm|item:weighted_avg_shares_diluted,false,Trailing-twelve-month earnings per diluted share.,1
sales_per_share,per_share,"safe_div(revenue_ttm, shares_outstanding_period_end)",ttm,metric:revenue_ttm|item:shares_outstanding_period_end,false,Trailing-twelve-month revenue per period-end share.,1
book_per_share,per_share,"safe_div(common_equity_q, shares_outstanding_period_end)",q,metric:common_equity_q|item:shares_outstanding_period_end,false,Common book value per period-end share.,1
tangible_book_value_per_share,per_share,"safe_div(common_equity_q - coalesce(goodwill, 0) - coalesce(other_intangibles, 0), shares_outstanding_period_end)",q,metric:common_equity_q|item:goodwill|item:other_intangibles|item:shares_outstanding_period_end,false,Tangible common book value per period-end share.,1
cfo_per_share,per_share,"safe_div(cfo_ttm, shares_outstanding_period_end)",ttm,metric:cfo_ttm|item:shares_outstanding_period_end,false,Trailing-twelve-month operating cash flow per share.,1
fcf_per_share,per_share,"safe_div(fcf_ttm, shares_outstanding_period_end)",ttm,metric:fcf_ttm|item:shares_outstanding_period_end,false,Trailing-twelve-month free cash flow per share.,1
dividends_per_share,per_share,"safe_div(common_dividends_ttm, shares_outstanding_period_end)",ttm,metric:common_dividends_ttm|item:shares_outstanding_period_end,false,Trailing-twelve-month common dividends per share.,1
cash_per_share,per_share,"safe_div(cash_st_investments_q, shares_outstanding_period_end)",q,metric:cash_st_investments_q|item:shares_outstanding_period_end,false,Cash and short-term investments per period-end share.,1
gross_margin,profitability,"safe_div(gross_profit_ttm, revenue_ttm)",ttm,metric:gross_profit_ttm|metric:revenue_ttm,false,Trailing-twelve-month gross margin.,1
operating_margin,profitability,"safe_div(operating_income_ttm, revenue_ttm)",ttm,metric:operating_income_ttm|metric:revenue_ttm,false,Trailing-twelve-month operating margin.,1
net_margin,profitability,"safe_div(net_income_ttm, revenue_ttm)",ttm,metric:net_income_ttm|metric:revenue_ttm,false,Trailing-twelve-month net margin.,1
ebitda_margin,profitability,"safe_div(ebitda_ttm, revenue_ttm)",ttm,metric:ebitda_ttm|metric:revenue_ttm,false,Trailing-twelve-month EBITDA margin.,1
effective_tax_rate_ttm,profitability,"safe_div(income_tax_ttm, pretax_income_ttm)",ttm,metric:income_tax_ttm|metric:pretax_income_ttm,false,Trailing-twelve-month effective tax rate.,1
nopat_ttm,profitability,operating_income_ttm * (1 - effective_tax_rate_ttm),ttm,metric:operating_income_ttm|metric:effective_tax_rate_ttm,false,Trailing-twelve-month net operating profit after tax.,1
roa,profitability,"safe_div(net_income_ttm, total_assets_avg2)",ttm,metric:net_income_ttm|metric:total_assets_avg2,false,Return on average total assets.,1
roe,profitability,"safe_div(net_income_common_ttm, common_equity_avg2)",ttm,metric:net_income_common_ttm|metric:common_equity_avg2,false,Return on average common equity.,1
roic,profitability,"safe_div(nopat_ttm, invested_capital_avg2)",ttm,metric:nopat_ttm|metric:invested_capital_avg2,false,Return on average invested capital.,1
roic_ex_goodwill,profitability,"safe_div(nopat_ttm, invested_capital_ex_goodwill_avg2)",ttm,metric:nopat_ttm|metric:invested_capital_ex_goodwill_avg2,false,Return on average invested capital excluding goodwill.,1
gross_profitability,profitability,"safe_div(gross_profit_ttm, total_assets_avg2)",ttm,metric:gross_profit_ttm|metric:total_assets_avg2,false,Novy-Marx gross profitability on average assets.,1
operating_profitability,profitability,"safe_div(operating_income_ttm, total_assets_avg2)",ttm,metric:operating_income_ttm|metric:total_assets_avg2,false,Operating profitability on average assets.,1
change_in_receivables_ttm,profitability,"accounts_receivable - lag(accounts_receivable, 4)",ttm,item:accounts_receivable,false,Year-over-year change in receivables.,1
change_in_inventory_ttm,profitability,"inventory - lag(inventory, 4)",ttm,item:inventory,false,Year-over-year change in inventory.,1
change_in_payables_ttm,profitability,"accounts_payable - lag(accounts_payable, 4)",ttm,item:accounts_payable,false,Year-over-year change in payables.,1
cash_profitability,profitability,"safe_div(operating_income_ttm + depreciation_ttm - change_in_receivables_ttm - change_in_inventory_ttm + change_in_payables_ttm, total_assets_avg2)",ttm,metric:operating_income_ttm|metric:depreciation_ttm|metric:change_in_receivables_ttm|metric:change_in_inventory_ttm|metric:change_in_payables_ttm|metric:total_assets_avg2,false,Ball-Gerakos-Linnainmaa-Nikolaev cash-based operating profitability.,1
asset_turnover,profitability,"safe_div(revenue_ttm, total_assets_avg2)",ttm,metric:revenue_ttm|metric:total_assets_avg2,false,Revenue over average total assets.,1
cfo_to_assets,profitability,"safe_div(cfo_ttm, total_assets_avg2)",ttm,metric:cfo_ttm|metric:total_assets_avg2,false,Operating cash flow over average total assets.,1
revenue_growth_yoy,growth,yoy(revenue_ttm),ttm,metric:revenue_ttm,false,Year-over-year trailing revenue growth.,1
gross_profit_growth_yoy,growth,yoy(gross_profit_ttm),ttm,metric:gross_profit_ttm,false,Year-over-year trailing gross-profit growth.,1
operating_income_growth_yoy,growth,yoy(operating_income_ttm),ttm,metric:operating_income_ttm,false,Year-over-year trailing operating-income growth.,1
net_income_growth_yoy,growth,yoy(net_income_ttm),ttm,metric:net_income_ttm,false,Year-over-year trailing net-income growth.,1
eps_diluted_growth_yoy,growth,yoy(eps_diluted_ttm),ttm,metric:eps_diluted_ttm,false,Year-over-year trailing diluted-EPS growth.,1
cfo_growth_yoy,growth,yoy(cfo_ttm),ttm,metric:cfo_ttm,false,Year-over-year trailing operating-cash-flow growth.,1
fcf_growth_yoy,growth,yoy(fcf_ttm),ttm,metric:fcf_ttm,false,Year-over-year trailing free-cash-flow growth.,1
capex_growth_yoy,growth,yoy(capex_ttm),ttm,metric:capex_ttm,false,Year-over-year trailing capital-expenditure growth.,1
rd_expense_growth_yoy,growth,yoy(rd_expense_ttm),ttm,metric:rd_expense_ttm,false,Year-over-year trailing research-and-development growth.,1
tax_expense_change_yoy,growth,yoy(income_tax_ttm),ttm,metric:income_tax_ttm,false,Year-over-year trailing tax-expense growth.,1
asset_growth,growth,yoy(total_assets),q,item:total_assets,false,Year-over-year total-asset growth.,1
shares_growth_yoy,growth,yoy(shares_outstanding_period_end),q,item:shares_outstanding_period_end,false,Year-over-year period-end share-count growth.,1
book_value_growth_yoy,growth,yoy(common_equity_q),q,metric:common_equity_q,false,Year-over-year common book-value growth.,1
revenue_growth_qoq,growth,qoq(revenue_ttm),ttm,metric:revenue_ttm,false,Quarter-over-quarter trailing revenue growth.,1
eps_diluted_growth_qoq,growth,qoq(eps_diluted_ttm),ttm,metric:eps_diluted_ttm,false,Quarter-over-quarter trailing diluted-EPS growth.,1
revenue_cagr_1y,growth,"cagr(revenue_ttm, 1)",ttm,metric:revenue_ttm,false,One-year compound annual trailing revenue growth.,1
revenue_cagr_3y,growth,"cagr(revenue_ttm, 3)",ttm,metric:revenue_ttm,false,Three-year compound annual trailing revenue growth.,1
eps_cagr_3y,growth,"cagr(eps_diluted_ttm, 3)",ttm,metric:eps_diluted_ttm,false,Three-year compound annual trailing diluted-EPS growth.,1
cfo_cagr_3y,growth,"cagr(cfo_ttm, 3)",ttm,metric:cfo_ttm,false,Three-year compound annual trailing operating-cash-flow growth.,1
total_assets_cagr_3y,growth,"cagr(total_assets, 3)",q,item:total_assets,false,Three-year compound annual total-asset growth.,1
gross_margin_change_yoy,growth,"gross_margin - lag(gross_margin, 4)",ttm,metric:gross_margin,false,Year-over-year change in trailing gross margin.,1
operating_margin_change_yoy,growth,"operating_margin - lag(operating_margin, 4)",ttm,metric:operating_margin,false,Year-over-year change in trailing operating margin.,1
net_margin_change_yoy,growth,"net_margin - lag(net_margin, 4)",ttm,metric:net_margin,false,Year-over-year change in trailing net margin.,1
asset_turnover_change_yoy,growth,"asset_turnover - lag(asset_turnover, 4)",ttm,metric:asset_turnover,false,Year-over-year change in asset turnover.,1
operating_profitability_change_yoy,growth,"operating_profitability - lag(operating_profitability, 4)",ttm,metric:operating_profitability,false,Year-over-year change in operating profitability.,1
roe_change_yoy,growth,"roe - lag(roe, 4)",ttm,metric:roe,false,Year-over-year change in return on equity.,1
debt_to_equity,leverage,"safe_div(total_debt_q, stockholders_equity)",q,metric:total_debt_q|item:stockholders_equity,false,Total debt over stockholders equity.,1
debt_to_assets,leverage,"safe_div(total_debt_q, total_assets)",q,metric:total_debt_q|item:total_assets,false,Total debt over total assets.,1
long_term_debt_to_assets,leverage,"safe_div(long_term_debt, total_assets)",q,item:long_term_debt|item:total_assets,false,Long-term debt over total assets.,1
net_debt_ebitda,leverage,"safe_div(net_debt, ebitda_ttm)",ttm,metric:net_debt|metric:ebitda_ttm,false,Net debt over trailing EBITDA.,1
interest_coverage,leverage,"safe_div(operating_income_ttm, abs(interest_expense_ttm))",ttm,metric:operating_income_ttm|metric:interest_expense_ttm,false,Trailing operating income over absolute interest expense.,1
current_ratio,leverage,"safe_div(current_assets, current_liabilities)",q,item:current_assets|item:current_liabilities,false,Current assets over current liabilities.,1
quick_ratio,leverage,"safe_div(current_assets - coalesce(inventory, 0), current_liabilities)",q,item:current_assets|item:inventory|item:current_liabilities,false,Current assets less inventory over current liabilities.,1
cash_ratio,leverage,"safe_div(cash_st_investments_q, current_liabilities)",q,metric:cash_st_investments_q|item:current_liabilities,false,Cash and short-term investments over current liabilities.,1
total_accruals,quality,"safe_div(net_income_ttm - cfo_ttm, total_assets_avg2)",ttm,metric:net_income_ttm|metric:cfo_ttm|metric:total_assets_avg2,false,Sloan cash-flow-statement total accruals scaled by average assets.,1
percent_accruals,quality,"safe_div(net_income_ttm - cfo_ttm, abs(net_income_ttm))",ttm,metric:net_income_ttm|metric:cfo_ttm,false,Total accruals scaled by absolute trailing earnings.,1
noa,quality,(total_assets - cash_st_investments_q) - (total_liabilities - total_debt_q),q,item:total_assets|metric:cash_st_investments_q|item:total_liabilities|metric:total_debt_q,false,Net operating assets: operating assets less operating liabilities.,1
noa_to_assets,quality,"safe_div(noa, lag(total_assets, 4))",q,metric:noa|item:total_assets,false,Net operating assets scaled by lagged total assets.,1
delta_noa,quality,"safe_div(noa - lag(noa, 4), lag(total_assets, 4))",q,metric:noa|item:total_assets,false,Year-over-year change in net operating assets scaled by lagged assets.,1
operating_working_capital_q,quality,"(current_assets - cash_st_investments_q) - (current_liabilities - coalesce(short_term_debt, 0))",q,item:current_assets|metric:cash_st_investments_q|item:current_liabilities|item:short_term_debt,false,Operating working capital excluding cash and short-term debt.,1
working_capital_accruals,quality,"safe_div(operating_working_capital_q - lag(operating_working_capital_q, 4), total_assets_avg2)",ttm,metric:operating_working_capital_q|metric:total_assets_avg2,false,Year-over-year change in operating working capital scaled by average assets.,1
rsst_accruals,quality,"safe_div((noa - lag(noa, 4)) + (coalesce(long_term_investments, 0) - lag(coalesce(long_term_investments, 0), 4)) + (total_debt_q - lag(total_debt_q, 4)), total_assets_avg2)",ttm,metric:noa|item:long_term_investments|metric:total_debt_q|metric:total_assets_avg2,false,Richardson-Sloan-Soliman-Tuna broad accruals scaled by average assets.,1
earnings_variability,quality,"stdev_q(eps_diluted_growth_yoy, 12)",ttm,metric:eps_diluted_growth_yoy,false,Twelve-quarter sample standard deviation of year-over-year diluted-EPS growth.,1
piotroski_f,quality,"indicator_gt(roa, 0) + indicator_gt(cfo_to_assets, 0) + indicator_gt(roa - lag(roa, 4), 0) + indicator_gt(cfo_to_assets, roa) + indicator_lt(long_term_debt_to_assets - lag(long_term_debt_to_assets, 4), 0) + indicator_gt(current_ratio - lag(current_ratio, 4), 0) + indicator_lt(shares_growth_yoy, 0.00001) + indicator_gt(gross_margin - lag(gross_margin, 4), 0) + indicator_gt(asset_turnover - lag(asset_turnover, 4), 0)",ttm,metric:roa|metric:cfo_to_assets|metric:long_term_debt_to_assets|metric:current_ratio|metric:shares_growth_yoy|metric:gross_margin|metric:asset_turnover,false,Piotroski nine-signal fundamental strength score.,1
altman_z_book,quality,"1.2 * safe_div(current_assets - current_liabilities, total_assets) + 1.4 * safe_div(retained_earnings, total_assets) + 3.3 * safe_div(operating_income_ttm, total_assets) + 0.6 * safe_div(common_equity_q, total_liabilities) + safe_div(revenue_ttm, total_assets)",ttm,item:current_assets|item:current_liabilities|item:total_assets|item:retained_earnings|metric:operating_income_ttm|metric:common_equity_q|item:total_liabilities|metric:revenue_ttm,false,Altman Z-score with book equity in the fourth term.,1
beneish_dsri,quality,"safe_div(safe_div(accounts_receivable, revenue_ttm), lag(safe_div(accounts_receivable, revenue_ttm), 4))",ttm,item:accounts_receivable|metric:revenue_ttm,false,Beneish days-sales-in-receivables index.,1
beneish_gmi,quality,"safe_div(lag(gross_margin, 4), gross_margin)",ttm,metric:gross_margin,false,Beneish gross-margin index.,1
beneish_aqi,quality,"safe_div(safe_div(total_assets - current_assets - pp_and_e_net, total_assets), lag(safe_div(total_assets - current_assets - pp_and_e_net, total_assets), 4))",ttm,item:total_assets|item:current_assets|item:pp_and_e_net,false,Beneish asset-quality index.,1
beneish_sgi,quality,"safe_div(revenue_ttm, lag(revenue_ttm, 4))",ttm,metric:revenue_ttm,false,Beneish sales-growth index.,1
beneish_depi,quality,"safe_div(lag(safe_div(depreciation_ttm, depreciation_ttm + pp_and_e_net), 4), safe_div(depreciation_ttm, depreciation_ttm + pp_and_e_net))",ttm,metric:depreciation_ttm|item:pp_and_e_net,false,Beneish depreciation index.,1
beneish_sgai,quality,"safe_div(safe_div(sga_expense_ttm, revenue_ttm), lag(safe_div(sga_expense_ttm, revenue_ttm), 4))",ttm,metric:sga_expense_ttm|metric:revenue_ttm,false,Beneish selling-general-and-administrative index.,1
beneish_tata,quality,"safe_div(net_income_ttm - cfo_ttm, total_assets)",ttm,metric:net_income_ttm|metric:cfo_ttm|item:total_assets,false,Beneish total-accruals-to-total-assets term.,1
beneish_lvgi,quality,"safe_div(safe_div(total_liabilities, total_assets), lag(safe_div(total_liabilities, total_assets), 4))",ttm,item:total_liabilities|item:total_assets,false,Beneish leverage index.,1
beneish_m,quality,-4.84 + 0.92 * beneish_dsri + 0.528 * beneish_gmi + 0.404 * beneish_aqi + 0.892 * beneish_sgi + 0.115 * beneish_depi - 0.172 * beneish_sgai + 4.679 * beneish_tata - 0.327 * beneish_lvgi,ttm,metric:beneish_dsri|metric:beneish_gmi|metric:beneish_aqi|metric:beneish_sgi|metric:beneish_depi|metric:beneish_sgai|metric:beneish_tata|metric:beneish_lvgi,false,Beneish eight-variable earnings-manipulation M-score.,1
ohlson_tlta,quality,"safe_div(total_liabilities, total_assets)",q,item:total_liabilities|item:total_assets,false,Ohlson total-liabilities-to-total-assets term.,1
ohlson_wcta,quality,"safe_div(current_assets - current_liabilities, total_assets)",q,item:current_assets|item:current_liabilities|item:total_assets,false,Ohlson working-capital-to-total-assets term.,1
ohlson_clca,quality,"safe_div(current_liabilities, current_assets)",q,item:current_liabilities|item:current_assets,false,Ohlson current-liabilities-to-current-assets term.,1
ohlson_oeneg,quality,"indicator_gt(total_liabilities, total_assets)",q,item:total_liabilities|item:total_assets,false,Ohlson negative-book-equity indicator.,1
ohlson_nita,quality,"safe_div(net_income_ttm, total_assets)",ttm,metric:net_income_ttm|item:total_assets,false,Ohlson net-income-to-total-assets term.,1
ohlson_futl,quality,"safe_div(cfo_ttm, total_liabilities)",ttm,metric:cfo_ttm|item:total_liabilities,false,Ohlson funds-from-operations-to-total-liabilities term.,1
ohlson_intwo,quality,"indicator_lt(net_income_ttm, 0) * indicator_lt(lag(net_income_ttm, 4), 0)",ttm,metric:net_income_ttm,false,Ohlson two-consecutive-loss-years indicator.,1
ohlson_chin,quality,"safe_div(net_income_ttm - lag(net_income_ttm, 4), abs(net_income_ttm) + abs(lag(net_income_ttm, 4)))",ttm,metric:net_income_ttm,false,Ohlson scaled change-in-net-income term.,1
ohlson_o,quality,-1.32 - 0.407 * ln(total_assets) + 6.03 * ohlson_tlta - 1.43 * ohlson_wcta + 0.0757 * ohlson_clca - 1.72 * ohlson_oeneg - 2.37 * ohlson_nita - 1.83 * ohlson_futl + 0.285 * ohlson_intwo - 0.521 * ohlson_chin,ttm,item:total_assets|metric:ohlson_tlta|metric:ohlson_wcta|metric:ohlson_clca|metric:ohlson_oeneg|metric:ohlson_nita|metric:ohlson_futl|metric:ohlson_intwo|metric:ohlson_chin,false,Ohlson O-score bankruptcy probability index without the GNP price-level deflator.,1
capex_to_depreciation,investment,"safe_div(capex_ttm, depreciation_ttm)",ttm,metric:capex_ttm|metric:depreciation_ttm,false,Trailing capital expenditure over trailing depreciation.,1
capex_to_sales,investment,"safe_div(capex_ttm, revenue_ttm)",ttm,metric:capex_ttm|metric:revenue_ttm,false,Trailing capital expenditure over trailing revenue.,1
rd_intensity_sales,investment,"safe_div(rd_expense_ttm, revenue_ttm)",ttm,metric:rd_expense_ttm|metric:revenue_ttm,false,Trailing research-and-development expense over trailing revenue.,1
net_equity_issuance,investment,"safe_div(share_issuance_ttm - share_repurchase_ttm, total_assets_avg2)",ttm,metric:share_issuance_ttm|metric:share_repurchase_ttm|metric:total_assets_avg2,false,Net equity issued scaled by average total assets.,1
net_debt_issuance,investment,"safe_div(debt_issuance_ttm - debt_reduction_ttm, total_assets_avg2)",ttm,metric:debt_issuance_ttm|metric:debt_reduction_ttm|metric:total_assets_avg2,false,Net long-term debt issued scaled by average total assets.,1
external_financing,investment,net_equity_issuance + net_debt_issuance,ttm,metric:net_equity_issuance|metric:net_debt_issuance,false,Total net external financing scaled by average total assets.,1
tax_to_book_income,investment,"safe_div(income_tax_ttm, net_income_ttm)",ttm,metric:income_tax_ttm|metric:net_income_ttm,false,Trailing tax expense over trailing net income.,1
payout_ratio,payout,"safe_div(common_dividends_ttm, net_income_common_ttm)",ttm,metric:common_dividends_ttm|metric:net_income_common_ttm,false,Trailing common dividends over trailing earnings available to common.,1
buyback_ratio,payout,"safe_div(share_repurchase_ttm - share_issuance_ttm, total_assets_avg2)",ttm,metric:share_repurchase_ttm|metric:share_issuance_ttm|metric:total_assets_avg2,false,Net buybacks scaled by average total assets.,1
market_cap,market,close * shares_outstanding,daily,market:close|market:shares_outstanding,true,Price times point-in-time shares outstanding.,1
enterprise_value,market,"market_cap + total_debt_q + coalesce(preferred_stock, 0) + coalesce(minority_interest_bs, 0) - cash_st_investments_q",daily,metric:market_cap|metric:total_debt_q|item:preferred_stock|item:minority_interest_bs|metric:cash_st_investments_q,true,Market cap plus debt preferred and minority interest less cash.,1
pe_ttm,market,"safe_div(market_cap, net_income_common_ttm)",daily,metric:market_cap|metric:net_income_common_ttm,true,Price to trailing earnings available to common.,1
pb,market,"safe_div(market_cap, common_equity_q)",daily,metric:market_cap|metric:common_equity_q,true,Price to common book value.,1
ps_ttm,market,"safe_div(market_cap, revenue_ttm)",daily,metric:market_cap|metric:revenue_ttm,true,Price to trailing revenue.,1
pcf_ttm,market,"safe_div(market_cap, cfo_ttm)",daily,metric:market_cap|metric:cfo_ttm,true,Price to trailing operating cash flow.,1
ev_ebitda,market,"safe_div(enterprise_value, ebitda_ttm)",daily,metric:enterprise_value|metric:ebitda_ttm,true,Enterprise value to trailing EBITDA.,1
ev_sales,market,"safe_div(enterprise_value, revenue_ttm)",daily,metric:enterprise_value|metric:revenue_ttm,true,Enterprise value to trailing revenue.,1
fcf_yield,market,"safe_div(fcf_ttm, market_cap)",daily,metric:fcf_ttm|metric:market_cap,true,Trailing free cash flow over market cap.,1
dividend_yield,market,"safe_div(common_dividends_ttm, market_cap)",daily,metric:common_dividends_ttm|metric:market_cap,true,Trailing common dividends over market cap.,1
earnings_yield,market,"safe_div(net_income_common_ttm, market_cap)",daily,metric:net_income_common_ttm|metric:market_cap,true,Trailing earnings available to common over market cap.,1
shareholder_yield,market,"safe_div(total_payout_ttm + debt_reduction_ttm - debt_issuance_ttm, market_cap)",daily,metric:total_payout_ttm|metric:debt_reduction_ttm|metric:debt_issuance_ttm|metric:market_cap,true,Net payout plus net debt paydown over market cap.,1
net_payout_yield,market,"safe_div(total_payout_ttm, market_cap)",daily,metric:total_payout_ttm|metric:market_cap,true,Dividends plus net buybacks over market cap.,1
total_payout_yield,market,"safe_div(common_dividends_ttm + share_repurchase_ttm, market_cap)",daily,metric:common_dividends_ttm|metric:share_repurchase_ttm|metric:market_cap,true,Gross dividends plus gross buybacks over market cap.,1
buyback_yield,market,"safe_div(share_repurchase_ttm - share_issuance_ttm, market_cap)",daily,metric:share_repurchase_ttm|metric:share_issuance_ttm|metric:market_cap,true,Net buybacks over market cap.,1
book_to_market,market,"safe_div(common_equity_q, market_cap)",daily,metric:common_equity_q|metric:market_cap,true,Common book value over market cap.,1
rd_to_market_equity,market,"safe_div(rd_expense_ttm, market_cap)",daily,metric:rd_expense_ttm|metric:market_cap,true,Trailing research-and-development expense over market cap.,1
gross_profit_to_ev,market,"safe_div(gross_profit_ttm, enterprise_value)",daily,metric:gross_profit_ttm|metric:enterprise_value,true,Trailing gross profit over enterprise value.,1
cfo_to_ev,market,"safe_div(cfo_ttm, enterprise_value)",daily,metric:cfo_ttm|metric:enterprise_value,true,Trailing operating cash flow over enterprise value.,1
ebit_to_ev,market,"safe_div(operating_income_ttm, enterprise_value)",daily,metric:operating_income_ttm|metric:enterprise_value,true,Trailing operating income over enterprise value.,1
sales_to_ev,market,"safe_div(revenue_ttm, enterprise_value)",daily,metric:revenue_ttm|metric:enterprise_value,true,Trailing revenue over enterprise value.,1
altman_z,market,"1.2 * safe_div(current_assets - current_liabilities, total_assets) + 1.4 * safe_div(retained_earnings, total_assets) + 3.3 * safe_div(operating_income_ttm, total_assets) + 0.6 * safe_div(market_cap, total_liabilities) + safe_div(revenue_ttm, total_assets)",daily,item:current_assets|item:current_liabilities|item:total_assets|item:retained_earnings|metric:operating_income_ttm|metric:market_cap|item:total_liabilities|metric:revenue_ttm,true,Altman Z-score with market equity in the fourth term.,1
total_return_1m,market,"tret(adj_close, 21)",daily,market:adj_close,true,Twenty-one-trading-day total return.,1
total_return_3m,market,"tret(adj_close, 63)",daily,market:adj_close,true,Sixty-three-trading-day total return.,1
total_return_6m,market,"tret(adj_close, 126)",daily,market:adj_close,true,One-hundred-twenty-six-trading-day total return.,1
total_return_12m,market,"tret(adj_close, 252)",daily,market:adj_close,true,Two-hundred-fifty-two-trading-day total return.,1
momentum_12_1,market,"safe_div(1 + tret(adj_close, 252), 1 + tret(adj_close, 21)) - 1",daily,market:adj_close,true,Twelve-month total return skipping the most recent month.,1
realized_vol_60d,market,"rvol(log_return, 60)",daily,market:log_return,true,Annualized sixty-day realized volatility of daily log returns.,1
realized_vol_252d,market,"rvol(log_return, 252)",daily,market:log_return,true,Annualized two-hundred-fifty-two-day realized volatility of daily log returns.,1
dollar_volume_20d,market,"avg_d(close * volume, 20)",daily,market:close|market:volume,true,Twenty-day average daily dollar volume.,1
```

- [ ] **Step 4: Verify the counts match the test**

Run: `.venv\Scripts\python.exe -c "from atx_db.derived_registry import default_derived_definitions as d; r=d(); print(len(r), sum(1 for x in r if x.window=='daily'), sorted({x.family for x in r}))"`
Expected: `173 30 ['growth', 'investment', 'leverage', 'market', 'payout', 'per_share', 'profitability', 'quality', 'rollup']`

If the printed count differs, the CSV was transcribed with a missing or extra row; fix the CSV, not the test.

- [ ] **Step 5: Run the tests**

Run: `.venv\Scripts\python.exe -m pytest tests/test_derived_catalog.py tests/test_derived_registry.py -n 0 -q`
Expected: `52 passed`.

- [ ] **Step 6: Commit**

```
git add src/atx_db/seeds/derived_metric_definitions.csv tests/test_derived_catalog.py
git commit -m "feat(db): seed the full declarative derived-metric catalog

173 definitions covering every family in the Tier-1 spec: TTM rollups and avg2
balances, per-share, profitability, growth and rate-of-change, leverage and
liquidity, quality and accruals (Sloan, percent accruals, NOA, delta NOA,
Piotroski F, Altman Z, Beneish M, Ohlson O), investment, payout, and the 30
daily market metrics that map one-for-one onto the 30 market_daily_metrics columns.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 5: `derived_metrics.py` — the quarterly engine

**Files:**
- Create: `C:\atx\atx-db\src\atx_db\derived_metrics.py`
- Create: `C:\atx\atx-db\scripts\build_derived_metrics.py`
- Modify: `C:\atx\atx-db\tests\data\public_api_snapshot.json` (add `"derived_metrics"`)
- Test: `C:\atx\atx-db\tests\test_derived_metrics.py` (new)

**Interfaces:**
- Consumes: `atx_db.derived_dsl.LowerContext`, `Lowered`, `compile_expression` (Task 2); `atx_db.derived_registry.DerivedMetricDefinition`, `DERIVED_SOURCE_NAME`, `default_derived_definitions`, `known_item_codes`, `seed_derived_metric_definitions`, `topological_order`, `validate_definitions`, `QUARTER_GRID_WINDOWS` (Task 3); `atx_db.connection.DuckDBStore`, `atx_db.dataset.Dataset`, `DatasetLoadResult`, `atx_db.clock.utc_today` (script edge only).
- Produces:
  - `atx_db.derived_metrics.DerivedMetricsOptions` — frozen dataclass: `source: str = DERIVED_SOURCE_NAME`, `security_ids: tuple[str, ...] | None = None`, `metric_codes: tuple[str, ...] | None = None`, `batch_size: int = 500`, `run_id: str | None = None`.
  - `atx_db.derived_metrics.quarterly_context(definition, *, item_codes, metric_codes) -> LowerContext`
  - `atx_db.derived_metrics.build_metric_sql(definition, *, lowered: Lowered, item_codes: tuple[str, ...], metric_codes: tuple[str, ...], source: str) -> str` — the full `INSERT ... SELECT ... ORDER BY` for one metric, with `?` placeholders for `source`, `metric_code`, `metric_window`, `run_id` and the security-batch list.
  - `atx_db.derived_metrics.select_security_batches(store, options) -> list[tuple[str, ...]]`
  - `atx_db.derived_metrics.refresh_derived_metrics(store, options: DerivedMetricsOptions | None = None) -> int`
  - `atx_db.derived_metrics.DerivedMetricsDataset(Dataset)` with `dataset_id = "derived_metrics"`, `source_name = DERIVED_SOURCE_NAME`.

**Engine shape (one statement per metric, in topological order, per security batch).**

```
WITH facts AS (               -- latest visible standardized facts for the batch
  SELECT security_id, canonical_code, period_end, value, available_at, revision_sequence
  FROM fundamental_standardized
  WHERE is_latest_revision AND basis IN ('quarterly', 'instant')
    AND security_id IN (?, ?, ...)
), picked AS (                -- one value per (security, code, period_end)
  SELECT security_id, canonical_code, period_end,
         arg_max(value, (available_at, revision_sequence)) AS value,
         arg_max(revision_sequence, (available_at, revision_sequence)) AS revision_sequence,
         max(available_at) AS available_at
  FROM facts GROUP BY 1, 2, 3
), grid AS (                  -- the security's quarterly period_end grid, row-numbered
  SELECT security_id, period_end,
         row_number() OVER (PARTITION BY security_id ORDER BY period_end) AS rn
  FROM (SELECT DISTINCT security_id, period_end FROM picked)
), base AS (                  -- the pivot: one column pair per referenced item code
  SELECT g.security_id, g.period_end, g.rn,
         max(CASE WHEN p.canonical_code = 'revenue' THEN p.value END) AS "revenue",
         max(CASE WHEN p.canonical_code = 'revenue' THEN p.available_at END) AS "revenue__at",
         ...
  FROM grid g LEFT JOIN picked p USING (security_id, period_end)
  GROUP BY 1, 2, 3
), deps AS (                  -- previously computed metric dependencies, same grid
  SELECT security_id, period_end,
         max(CASE WHEN metric_code = 'gross_profit_ttm' THEN value END) AS "gross_profit_ttm",
         max(CASE WHEN metric_code = 'gross_profit_ttm' THEN available_at END) AS "gross_profit_ttm__at",
         ...
  FROM derived_metric_values
  WHERE source = ? AND metric_code IN (...) AND security_id IN (?, ?, ...)
  GROUP BY 1, 2
), hash_parts AS (            -- the exact facts the expression can reach
  SELECT t.security_id, t.period_end,
         l.canonical_code || '|' || CAST(l.period_end AS VARCHAR) || '|' ||
         CAST(l.revision_sequence AS VARCHAR) || '|' || CAST(l.value AS VARCHAR) AS payload
  FROM grid t
  JOIN grid lg ON lg.security_id = t.security_id AND lg.rn BETWEEN t.rn - <max_lag> AND t.rn
  JOIN picked l ON l.security_id = lg.security_id AND l.period_end = lg.period_end
   AND l.canonical_code IN (<item codes>)
  UNION ALL
  SELECT t.security_id, t.period_end,
         'metric:' || d.metric_code || '|' || CAST(d.period_end AS VARCHAR) || '|' || d.inputs_hash
  FROM grid t
  JOIN grid lg ON lg.security_id = t.security_id AND lg.rn BETWEEN t.rn - <max_lag> AND t.rn
  JOIN derived_metric_values d ON d.security_id = lg.security_id AND d.period_end = lg.period_end
   AND d.source = ? AND d.metric_code IN (<metric codes>)
), hashed AS (
  SELECT security_id, period_end, sha256(string_agg(payload, ';' ORDER BY payload)) AS inputs_hash
  FROM hash_parts GROUP BY 1, 2
), computed AS (
  SELECT b.security_id, b.period_end,
         <value_sql> AS value, <availability_sql> AS available_at
  FROM base b LEFT JOIN deps d USING (security_id, period_end)
)
INSERT INTO derived_metric_values (...)
SELECT sha256(? || '|' || c.security_id || '|' || ? || '|' || CAST(c.period_end AS VARCHAR)),
       ?, c.security_id, ?, ?, c.period_end, c.value, c.available_at, h.inputs_hash,
       CAST(c.available_at AS DATE), true, ?
FROM computed c JOIN hashed h USING (security_id, period_end)
WHERE c.value IS NOT NULL AND isfinite(c.value) AND c.available_at IS NOT NULL
ORDER BY c.security_id, c.period_end
```

`LowerContext.columns` maps an item code to `b."<code>"` and a metric code to `d."<code>"`; `LowerContext.availability` maps them to `b."<code>__at"` and `d."<code>__at"`. `partition_sql = 'b.security_id'`, `order_sql = 'b.period_end'`. Because the availability expression is produced by the same lowering as the value expression, `available_at` is the max over exactly the inputs the value consumed — including the lagged rows of a `ttm`/`avg2`/`cagr` frame.

`as_of_date = CAST(available_at AS DATE)`, i.e. the decision date, not a wall-clock stamp. No `now()` anywhere in the derived path; `source_loaded_at` is filled by the table default and is lineage only.

- [ ] **Step 1: Write the failing test**

Create `C:\atx\atx-db\tests\test_derived_metrics.py`:

```python
"""Tier1-S3 T5: the quarterly derived-metric engine."""

from __future__ import annotations

import datetime as dt
import math

import pytest

from atx_db.derived_metrics import (
    DerivedMetricsOptions,
    refresh_derived_metrics,
    select_security_batches,
)
from atx_db.derived_registry import seed_derived_metric_definitions

_QUARTERS = (
    dt.date(2019, 3, 31),
    dt.date(2019, 6, 30),
    dt.date(2019, 9, 30),
    dt.date(2019, 12, 31),
    dt.date(2020, 3, 31),
    dt.date(2020, 6, 30),
    dt.date(2020, 9, 30),
    dt.date(2020, 12, 31),
)


def _insert_fact(store, security_id, code, basis, period_end, value, available_at, revision=1):
    store.con.execute(
        """
        INSERT INTO fundamental_standardized (
            standardized_id, source, security_id, item_id, canonical_code, basis,
            period_end, value, as_of_date, available_at, input_codes_json,
            input_item_ids_json, rule_id, combination_rule, revision_sequence,
            is_latest_revision
        ) VALUES (?, 'test', ?, 1, ?, ?, ?, ?, ?, ?, '[]', '[]', 'r', 'direct', ?, true)
        """,
        [
            f"{security_id}|{code}|{basis}|{period_end}|{revision}",
            security_id,
            code,
            basis,
            period_end,
            value,
            available_at.date(),
            available_at,
            revision,
        ],
    )


def _available(period_end: dt.date, *, lag_days: int = 40) -> dt.datetime:
    return dt.datetime.combine(period_end + dt.timedelta(days=lag_days), dt.time(21, 0))


@pytest.fixture
def seeded(tmp_store):
    seed_derived_metric_definitions(tmp_store)
    for index, period_end in enumerate(_QUARTERS):
        revenue = 100.0 + 10.0 * index
        cogs = 60.0 + 4.0 * index
        assets = 1000.0 + 50.0 * index
        _insert_fact(tmp_store, "S1", "revenue", "quarterly", period_end, revenue, _available(period_end))
        _insert_fact(
            tmp_store, "S1", "cost_of_revenue_cogs", "quarterly", period_end, cogs, _available(period_end)
        )
        _insert_fact(
            tmp_store, "S1", "total_assets", "instant", period_end, assets, _available(period_end, lag_days=50)
        )
    return tmp_store


def _value(store, metric_code, period_end):
    row = store.con.execute(
        "SELECT value, available_at, inputs_hash FROM derived_metric_values "
        "WHERE metric_code = ? AND period_end = ? AND security_id = 'S1'",
        [metric_code, period_end],
    ).fetchone()
    return row


def test_batches_are_deterministic_and_bounded(seeded):
    batches = select_security_batches(seeded, DerivedMetricsOptions(batch_size=1))
    assert batches == [("S1",)]


def test_ttm_sums_exactly_four_quarters(seeded):
    refresh_derived_metrics(seeded, DerivedMetricsOptions(metric_codes=("revenue_ttm",)))
    assert _value(seeded, "revenue_ttm", _QUARTERS[2]) is None  # fewer than 4 observations
    value, _available_at, _hash = _value(seeded, "revenue_ttm", _QUARTERS[3])
    assert value == pytest.approx(100.0 + 110.0 + 120.0 + 130.0)
    value, _available_at, _hash = _value(seeded, "revenue_ttm", _QUARTERS[7])
    assert value == pytest.approx(140.0 + 150.0 + 160.0 + 170.0)


def test_available_at_is_the_max_over_the_ttm_frame(seeded):
    refresh_derived_metrics(seeded, DerivedMetricsOptions(metric_codes=("revenue_ttm",)))
    _value_, available_at, _hash = _value(seeded, "revenue_ttm", _QUARTERS[3])
    assert available_at == _available(_QUARTERS[3])


def test_avg2_averages_the_current_and_year_ago_balance(seeded):
    refresh_derived_metrics(seeded, DerivedMetricsOptions(metric_codes=("total_assets_avg2",)))
    assert _value(seeded, "total_assets_avg2", _QUARTERS[3]) is None
    value, available_at, _hash = _value(seeded, "total_assets_avg2", _QUARTERS[4])
    assert value == pytest.approx((1000.0 + 1200.0) / 2.0)
    assert available_at == _available(_QUARTERS[4], lag_days=50)


def test_dependency_metrics_are_computed_in_topological_order(seeded):
    refresh_derived_metrics(
        seeded,
        DerivedMetricsOptions(
            metric_codes=("gross_profit_q", "gross_profit_ttm", "revenue_ttm", "gross_margin")
        ),
    )
    gross_profit, _at, _hash = _value(seeded, "gross_profit_ttm", _QUARTERS[3])
    revenue, _at2, _hash2 = _value(seeded, "revenue_ttm", _QUARTERS[3])
    margin, _at3, _hash3 = _value(seeded, "gross_margin", _QUARTERS[3])
    assert margin == pytest.approx(gross_profit / revenue)


def test_yoy_uses_the_absolute_base(seeded):
    refresh_derived_metrics(
        seeded, DerivedMetricsOptions(metric_codes=("revenue_ttm", "revenue_growth_yoy"))
    )
    current, _a, _h = _value(seeded, "revenue_ttm", _QUARTERS[7])
    prior, _b, _i = _value(seeded, "revenue_ttm", _QUARTERS[3])
    growth, _c, _j = _value(seeded, "revenue_growth_yoy", _QUARTERS[7])
    assert growth == pytest.approx((current - prior) / abs(prior))


def test_a_zero_denominator_yields_no_row_rather_than_infinity(tmp_store):
    seed_derived_metric_definitions(tmp_store)
    for period_end in _QUARTERS[:4]:
        _insert_fact(tmp_store, "S2", "revenue", "quarterly", period_end, 0.0, _available(period_end))
        _insert_fact(
            tmp_store, "S2", "cost_of_revenue_cogs", "quarterly", period_end, 0.0, _available(period_end)
        )
    refresh_derived_metrics(
        tmp_store,
        DerivedMetricsOptions(metric_codes=("gross_profit_q", "gross_profit_ttm", "revenue_ttm", "gross_margin")),
    )
    rows = tmp_store.con.execute(
        "SELECT count(*) FROM derived_metric_values WHERE metric_code = 'gross_margin'"
    ).fetchone()[0]
    assert rows == 0


def test_inputs_hash_is_stable_across_reruns(seeded):
    refresh_derived_metrics(seeded, DerivedMetricsOptions(metric_codes=("revenue_ttm",)))
    first = seeded.con.execute(
        "SELECT inputs_hash FROM derived_metric_values WHERE metric_code = 'revenue_ttm' ORDER BY period_end"
    ).fetchall()
    refresh_derived_metrics(seeded, DerivedMetricsOptions(metric_codes=("revenue_ttm",)))
    second = seeded.con.execute(
        "SELECT inputs_hash FROM derived_metric_values WHERE metric_code = 'revenue_ttm' ORDER BY period_end"
    ).fetchall()
    assert first == second
    assert all(len(str(row[0])) == 64 for row in first)


def test_inputs_hash_changes_when_an_input_value_changes(seeded):
    refresh_derived_metrics(seeded, DerivedMetricsOptions(metric_codes=("revenue_ttm",)))
    before = _value(seeded, "revenue_ttm", _QUARTERS[7])[2]
    seeded.con.execute(
        "UPDATE fundamental_standardized SET value = value + 1 "
        "WHERE canonical_code = 'revenue' AND period_end = ?",
        [_QUARTERS[6]],
    )
    refresh_derived_metrics(seeded, DerivedMetricsOptions(metric_codes=("revenue_ttm",)))
    after = _value(seeded, "revenue_ttm", _QUARTERS[7])[2]
    assert before != after


def test_rerun_is_idempotent_on_row_counts(seeded):
    options = DerivedMetricsOptions(metric_codes=("revenue_ttm", "total_assets_avg2"))
    first = refresh_derived_metrics(seeded, options)
    second = refresh_derived_metrics(seeded, options)
    assert first == second
    total = seeded.con.execute("SELECT count(*) FROM derived_metric_values").fetchone()[0]
    assert total == first


def test_batch_size_does_not_change_the_result(tmp_store):
    seed_derived_metric_definitions(tmp_store)
    for security_id in ("S1", "S2", "S3"):
        for index, period_end in enumerate(_QUARTERS):
            _insert_fact(
                tmp_store,
                security_id,
                "revenue",
                "quarterly",
                period_end,
                100.0 + index + len(security_id),
                _available(period_end),
            )
    refresh_derived_metrics(tmp_store, DerivedMetricsOptions(metric_codes=("revenue_ttm",), batch_size=3))
    wide = tmp_store.con.execute(
        "SELECT security_id, period_end, value, inputs_hash FROM derived_metric_values ORDER BY 1, 2"
    ).fetchall()
    refresh_derived_metrics(tmp_store, DerivedMetricsOptions(metric_codes=("revenue_ttm",), batch_size=1))
    narrow = tmp_store.con.execute(
        "SELECT security_id, period_end, value, inputs_hash FROM derived_metric_values ORDER BY 1, 2"
    ).fetchall()
    assert wide == narrow


def test_every_emitted_value_is_finite_and_has_an_availability(seeded):
    refresh_derived_metrics(seeded)
    bad = seeded.con.execute(
        "SELECT count(*) FROM derived_metric_values "
        "WHERE value IS NULL OR NOT isfinite(value) OR available_at IS NULL"
    ).fetchone()[0]
    assert bad == 0


def test_as_of_date_never_precedes_availability(seeded):
    refresh_derived_metrics(seeded)
    violations = seeded.con.execute(
        "SELECT count(*) FROM derived_metric_values WHERE as_of_date < CAST(available_at AS DATE)"
    ).fetchone()[0]
    assert violations == 0


def test_full_catalog_runs_without_error(seeded):
    rows = refresh_derived_metrics(seeded)
    assert rows > 0
    codes = seeded.con.execute(
        "SELECT count(DISTINCT metric_code) FROM derived_metric_values"
    ).fetchone()[0]
    assert codes >= 5


def test_no_wall_clock_in_the_module_source():
    import inspect

    import atx_db.derived_metrics as module

    source = inspect.getsource(module)
    for forbidden in ("date.today", "utcnow", "now()", "Timestamp.now", "time.time"):
        assert forbidden not in source, forbidden
```

Note on `test_no_wall_clock_in_the_module_source`: `math` is imported by the test only for readability of future additions; drop it if the linter objects.

- [ ] **Step 2: Run the test to verify it fails**

Run: `.venv\Scripts\python.exe -m pytest tests/test_derived_metrics.py -n 0 -q`
Expected: FAIL — `ModuleNotFoundError: No module named 'atx_db.derived_metrics'`.

- [ ] **Step 3: Create `src/atx_db/derived_metrics.py`**

```python
"""The quarterly derived-metric engine: one generated statement per metric."""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any, Iterable

from .connection import DuckDBStore
from .dataset import Dataset, DatasetLoadResult
from .derived_dsl import LowerContext, Lowered, compile_expression
from .derived_registry import (
    DERIVED_SOURCE_NAME,
    DerivedMetricDefinition,
    default_derived_definitions,
    known_item_codes,
    topological_order,
    validate_definitions,
)

__all__ = [
    "DerivedMetricsDataset",
    "DerivedMetricsOptions",
    "build_metric_sql",
    "quarterly_context",
    "refresh_derived_metrics",
    "select_security_batches",
]

_VALUE_COLUMNS = (
    "derived_value_id",
    "source",
    "security_id",
    "metric_code",
    "metric_window",
    "period_end",
    "value",
    "available_at",
    "inputs_hash",
    "as_of_date",
    "is_latest_revision",
    "run_id",
)


@dataclass(frozen=True)
class DerivedMetricsOptions:
    source: str = DERIVED_SOURCE_NAME
    security_ids: tuple[str, ...] | None = None
    metric_codes: tuple[str, ...] | None = None
    batch_size: int = 500
    run_id: str | None = None


def quarterly_context(
    definition: DerivedMetricDefinition,
    *,
    item_codes: tuple[str, ...],
    metric_codes: tuple[str, ...],
) -> LowerContext:
    columns: dict[str, str] = {}
    availability: dict[str, str] = {}
    for code in item_codes:
        columns[code] = f'b."{code}"'
        availability[code] = f'b."{code}__at"'
    for code in metric_codes:
        columns[code] = f'd."{code}"'
        availability[code] = f'd."{code}__at"'
    _ = definition
    return LowerContext(
        grid="quarter",
        columns=columns,
        availability=availability,
        partition_sql="b.security_id",
        order_sql="b.period_end",
    )


def _pivot_pairs(alias: str, codes: Iterable[str], key_column: str) -> str:
    fragments: list[str] = []
    for code in codes:
        literal = code.replace("'", "''")
        fragments.append(
            f"max(CASE WHEN {alias}.{key_column} = '{literal}' THEN {alias}.value END) AS \"{code}\""
        )
        fragments.append(
            f"max(CASE WHEN {alias}.{key_column} = '{literal}' "
            f"THEN {alias}.available_at END) AS \"{code}__at\""
        )
    return ",\n           ".join(fragments) if fragments else "NULL AS __unused"


def _in_list(codes: Iterable[str]) -> str:
    quoted = ", ".join("'" + code.replace("'", "''") + "'" for code in codes)
    return quoted or "''"


def build_metric_sql(
    definition: DerivedMetricDefinition,
    *,
    lowered: Lowered,
    item_codes: tuple[str, ...],
    metric_codes: tuple[str, ...],
    security_count: int,
) -> str:
    """Return the full INSERT statement for one metric and one security batch.

    Placeholders, in order: source (facts scope is unfiltered), the security ids
    for ``facts``, the source and security ids for ``deps``, the source and
    security ids for the metric half of ``hash_parts``, then
    source, metric_code (id hash), source, metric_code, metric_window, run_id.
    """
    securities = ", ".join(["?"] * security_count)
    deps_cte = (
        f"""deps AS (
    SELECT security_id, period_end,
           {_pivot_pairs('m', metric_codes, 'metric_code')}
    FROM derived_metric_values m
    WHERE m.source = ? AND m.metric_code IN ({_in_list(metric_codes)})
      AND m.security_id IN ({securities})
    GROUP BY 1, 2
)"""
        if metric_codes
        else """deps AS (
    SELECT CAST(NULL AS VARCHAR) AS security_id, CAST(NULL AS DATE) AS period_end
    WHERE false
)"""
    )
    metric_hash_branch = (
        f"""
    UNION ALL
    SELECT t.security_id, t.period_end,
           'metric:' || m.metric_code || '|' || CAST(m.period_end AS VARCHAR) || '|' || m.inputs_hash AS payload
    FROM grid t
    JOIN grid lg ON lg.security_id = t.security_id AND lg.rn BETWEEN t.rn - {lowered.max_lag} AND t.rn
    JOIN derived_metric_values m
      ON m.security_id = lg.security_id AND m.period_end = lg.period_end
     AND m.source = ? AND m.metric_code IN ({_in_list(metric_codes)})"""
        if metric_codes
        else ""
    )
    return f"""
INSERT INTO derived_metric_values ({", ".join(_VALUE_COLUMNS)})
WITH facts AS (
    SELECT security_id, canonical_code, period_end, value, available_at, revision_sequence
    FROM fundamental_standardized
    WHERE is_latest_revision
      AND basis IN ('quarterly', 'instant')
      AND value IS NOT NULL
      AND available_at IS NOT NULL
      AND security_id IN ({securities})
), picked AS (
    SELECT security_id, canonical_code, period_end,
           arg_max(value, (available_at, revision_sequence)) AS value,
           arg_max(revision_sequence, (available_at, revision_sequence)) AS revision_sequence,
           max(available_at) AS available_at
    FROM facts
    GROUP BY 1, 2, 3
), grid AS (
    SELECT security_id, period_end,
           row_number() OVER (PARTITION BY security_id ORDER BY period_end) AS rn
    FROM (SELECT DISTINCT security_id, period_end FROM picked)
), base AS (
    SELECT g.security_id, g.period_end, g.rn,
           {_pivot_pairs('p', item_codes, 'canonical_code')}
    FROM grid g
    LEFT JOIN picked p ON p.security_id = g.security_id AND p.period_end = g.period_end
    GROUP BY 1, 2, 3
), {deps_cte}, hash_parts AS (
    SELECT t.security_id, t.period_end,
           l.canonical_code || '|' || CAST(l.period_end AS VARCHAR) || '|' ||
           CAST(l.revision_sequence AS VARCHAR) || '|' || CAST(l.value AS VARCHAR) AS payload
    FROM grid t
    JOIN grid lg ON lg.security_id = t.security_id AND lg.rn BETWEEN t.rn - {lowered.max_lag} AND t.rn
    JOIN picked l ON l.security_id = lg.security_id AND l.period_end = lg.period_end
     AND l.canonical_code IN ({_in_list(item_codes)}){metric_hash_branch}
), hashed AS (
    SELECT security_id, period_end,
           sha256(string_agg(payload, ';' ORDER BY payload)) AS inputs_hash
    FROM hash_parts
    GROUP BY 1, 2
), computed AS (
    SELECT b.security_id, b.period_end,
           {lowered.value_sql} AS value,
           {lowered.availability_sql} AS available_at
    FROM base b
    LEFT JOIN deps d ON d.security_id = b.security_id AND d.period_end = b.period_end
)
SELECT sha256(? || '|' || c.security_id || '|' || ? || '|' || CAST(c.period_end AS VARCHAR)),
       ?, c.security_id, ?, ?, c.period_end, c.value, c.available_at, h.inputs_hash,
       CAST(c.available_at AS DATE), true, ?
FROM computed c
JOIN hashed h ON h.security_id = c.security_id AND h.period_end = c.period_end
WHERE c.value IS NOT NULL AND isfinite(c.value) AND c.available_at IS NOT NULL
ORDER BY c.security_id, c.period_end
"""


def select_security_batches(
    store: DuckDBStore,
    options: DerivedMetricsOptions | None = None,
) -> list[tuple[str, ...]]:
    options = options or DerivedMetricsOptions()
    if options.security_ids is not None:
        identifiers = sorted(options.security_ids)
    else:
        rows = store.con.execute(
            """
            SELECT DISTINCT security_id
            FROM fundamental_standardized
            WHERE is_latest_revision AND basis IN ('quarterly', 'instant')
            ORDER BY security_id
            """
        ).fetchall()
        identifiers = [str(row[0]) for row in rows]
    size = max(1, int(options.batch_size))
    return [tuple(identifiers[start : start + size]) for start in range(0, len(identifiers), size)]


def _selected(
    definitions: tuple[DerivedMetricDefinition, ...],
    options: DerivedMetricsOptions,
) -> tuple[DerivedMetricDefinition, ...]:
    if options.metric_codes is None:
        chosen = definitions
    else:
        wanted = set(options.metric_codes)
        chosen = tuple(d for d in definitions if d.metric_code in wanted)
    return tuple(d for d in topological_order(definitions) if d in set(chosen))


def refresh_derived_metrics(
    store: DuckDBStore,
    options: DerivedMetricsOptions | None = None,
) -> int:
    options = options or DerivedMetricsOptions()
    store.initialize()
    definitions = default_derived_definitions()
    validate_definitions(definitions, item_codes=known_item_codes())
    quarterly = tuple(d for d in definitions if d.window != "daily")
    ordered = tuple(d for d in topological_order(quarterly) if _wanted(d, options))
    batches = select_security_batches(store, options)
    inserted = 0
    for batch in batches:
        with store.transaction():
            predicates = ["source = ?", f"security_id IN ({', '.join(['?'] * len(batch))})"]
            params: list[Any] = [options.source, *batch]
            if options.metric_codes is not None:
                codes = [d.metric_code for d in ordered]
                predicates.append(f"metric_code IN ({', '.join(['?'] * len(codes))})")
                params.extend(codes)
            store.con.execute(
                f"DELETE FROM derived_metric_values WHERE {' AND '.join(predicates)}", params
            )
            for definition in ordered:
                lowered = compile_expression(
                    definition.expression,
                    quarterly_context(
                        definition,
                        item_codes=tuple(sorted(definition.item_inputs)),
                        metric_codes=tuple(sorted(definition.metric_inputs)),
                    ),
                )
                item_codes = tuple(sorted(definition.item_inputs))
                metric_codes = tuple(sorted(definition.metric_inputs))
                sql = build_metric_sql(
                    definition,
                    lowered=lowered,
                    item_codes=item_codes,
                    metric_codes=metric_codes,
                    security_count=len(batch),
                )
                bind: list[Any] = list(batch)
                if metric_codes:
                    bind.extend([options.source, *batch])
                    bind.append(options.source)
                bind.extend(
                    [
                        options.source,
                        definition.metric_code,
                        options.source,
                        definition.metric_code,
                        definition.window,
                        options.run_id,
                    ]
                )
                cursor = store.con.execute(sql, bind)
                affected = cursor.fetchall()
                inserted += int(affected[0][0]) if affected else 0
    return inserted


def _wanted(definition: DerivedMetricDefinition, options: DerivedMetricsOptions) -> bool:
    return options.metric_codes is None or definition.metric_code in set(options.metric_codes)


class DerivedMetricsDataset(Dataset):
    dataset_id = "derived_metrics"
    source_name = DERIVED_SOURCE_NAME

    def ensure_schema(self, store: DuckDBStore) -> None:
        store.initialize()

    def load(self, store: DuckDBStore, options: Any) -> DatasetLoadResult:
        resolved = options if isinstance(options, DerivedMetricsOptions) else DerivedMetricsOptions()
        rows = refresh_derived_metrics(store, resolved)
        return DatasetLoadResult(
            dataset_id=self.dataset_id,
            rows_loaded=rows,
            source=resolved.source,
            details={"metric_codes": list(resolved.metric_codes or ())},
        )
```

Implementation notes for the executor:
- DuckDB's `INSERT` returns the affected row count as a single-row result, so `cursor.fetchall()` yields `[(n,)]`. If the installed DuckDB returns an empty result for `INSERT ... SELECT`, replace the count with `SELECT count(*) FROM derived_metric_values WHERE source = ? AND metric_code = ? AND security_id IN (...)` executed immediately after; keep the aggregate in the same transaction so the number is exact.
- `_selected` is superseded by the inline `ordered` computation in `refresh_derived_metrics`; delete `_selected` before committing rather than shipping dead code.
- `definition` is unused inside `quarterly_context`; keep the parameter for symmetry with `market_daily.daily_context` and silence the linter with the `_ = definition` line shown.

- [ ] **Step 4: Create `scripts/build_derived_metrics.py`**

```python
"""Operator entry point for the declarative derived-metric engine."""

from __future__ import annotations

import argparse
import json
from pathlib import Path

from atx_db.connection import DuckDBStore, resolve_data_dir
from atx_db.derived_metrics import DerivedMetricsOptions, refresh_derived_metrics
from atx_db.derived_registry import seed_derived_metric_definitions


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--db-path", type=Path, default=None)
    parser.add_argument("--metric", action="append", default=None)
    parser.add_argument("--security-id", action="append", default=None)
    parser.add_argument("--batch-size", type=int, default=500)
    parser.add_argument("--run-id", default=None)
    args = parser.parse_args(argv)

    db_path = args.db_path or (resolve_data_dir() / "warehouse.duckdb")
    with DuckDBStore(db_path) as store:
        store.initialize()
        seeded = seed_derived_metric_definitions(store)
        rows = refresh_derived_metrics(
            store,
            DerivedMetricsOptions(
                metric_codes=tuple(args.metric) if args.metric else None,
                security_ids=tuple(args.security_id) if args.security_id else None,
                batch_size=args.batch_size,
                run_id=args.run_id,
            ),
        )
    print(json.dumps({"definitions_seeded": seeded, "values_written": rows}, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
```

Verify `resolve_data_dir` is importable from `atx_db.connection` before writing this file; if it lives elsewhere, import it from where it actually is rather than adding a shim.

- [ ] **Step 5: Add `derived_metrics` to the public API snapshot**

Insert `"derived_metrics"` into the `"atx_db"` array, after `"derived_dsl"` and before `"derived_registry"`.

- [ ] **Step 6: Run the tests**

Run: `.venv\Scripts\python.exe -m pytest tests/test_derived_metrics.py -n 0 -q`
Expected: `15 passed`.

Run: `.venv\Scripts\python.exe -m pytest tests/test_derived_dsl.py tests/test_derived_registry.py tests/test_derived_catalog.py tests/test_module_boundaries.py -n 0 -q`
Expected: `passed`, no regressions.

- [ ] **Step 7: Commit**

```
git add src/atx_db/derived_metrics.py scripts/build_derived_metrics.py tests/test_derived_metrics.py tests/data/public_api_snapshot.json
git commit -m "feat(db): add the quarterly declarative derived-metric engine

Pivots fundamental_standardized onto each security's quarterly period_end grid
and emits one deterministic INSERT-SELECT per metric in topological order.
available_at comes from the same lowering as the value, so it is the max over
exactly the facts the expression consumed; inputs_hash is a sha256 over the
sorted (code, period_end, revision_sequence, value) payloads inside the
expression's own window. No wall-clock read anywhere in the path.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 6: `market_daily.py` — the ASOF market join and the wide daily panel

**Files:**
- Create: `C:\atx\atx-db\src\atx_db\market_daily.py`
- Create: `C:\atx\atx-db\scripts\build_market_daily.py`
- Modify: `C:\atx\atx-db\tests\data\public_api_snapshot.json` (add `"market_daily"`)
- Test: `C:\atx\atx-db\tests\test_market_daily.py` (new)

**Interfaces:**
- Consumes: `atx_db.derived_dsl.LowerContext`, `compile_expression` (Task 2); `atx_db.derived_registry.DERIVED_SOURCE_NAME`, `default_derived_definitions`, `topological_order`, `MARKET_COLUMNS` (Task 3); `atx_db.connection.DuckDBStore`, `atx_db.dataset.Dataset`, `DatasetLoadResult`.
- Produces:
  - `atx_db.market_daily.MARKET_DAILY_SOURCE_NAME: str` = `"atx-db daily market panel v1"`
  - `atx_db.market_daily.END_OF_DAY_HOURS: int` = `22` — the `ticker_history.py:392` convention, named once.
  - `atx_db.market_daily.MarketDailyOptions` — frozen dataclass: `source: str = MARKET_DAILY_SOURCE_NAME`, `derived_source: str = DERIVED_SOURCE_NAME`, `bar_source: str | None = None`, `start_date: dt.date | None = None`, `end_date: dt.date | None = None`, `security_ids: tuple[str, ...] | None = None`, `batch_size: int = 200`, `shares_tolerance: float = 0.05`, `run_id: str | None = None`.
  - `atx_db.market_daily.daily_context(*, names: tuple[str, ...]) -> LowerContext`
  - `atx_db.market_daily.build_market_daily_sql(*, item_codes, metric_codes, daily_definitions, security_count, date_predicate) -> str`
  - `atx_db.market_daily.refresh_market_daily_metrics(store, options=None) -> int`
  - `atx_db.market_daily.shares_reconciliation_report(store, *, source=MARKET_DAILY_SOURCE_NAME, tolerance=0.05) -> dict[str, object]` — keys `securities_with_both_sources`, `securities_within_tolerance`, `pass_rate`, `tolerance`, `meets_spec_gate` (the spec's "≥ 95% of securities").
  - `atx_db.market_daily.MarketDailyDataset(Dataset)` with `dataset_id = "market_daily"`.

**Join contract.**

1. `bars` — `equity_daily_bars` deduped to one row per `(security_id, trade_date)` by `available_at DESC, source DESC` (the same tiebreak `valuation_multiples.refresh_market_cap` uses), restricted to `close > 0` and `adjusted_close > 0`, with:
   - `cutoff = CAST(trade_date AS TIMESTAMP) + INTERVAL 22 HOUR` — the end-of-day convention already stamped onto `equity_daily_bars.available_at` by `ticker_history.py:392`; the code uses `INTERVAL {END_OF_DAY_HOURS} HOUR` so the constant appears once.
   - `bar_at = greatest(available_at, cutoff)`
   - `adj_close = adjusted_close` — **never** `split_factor`, which holds the vendor `returnFactor` (audit §4.3).
   - `log_return = ln(adj_close / lag(adj_close) OVER (PARTITION BY security_id ORDER BY trade_date))`, null when the lag is null or non-positive.
   - `archive_shares = shares_outstanding` (the vendor `shares` column).
2. `dei_shares` — `shares_outstanding_history` filtered to `share_count_type = 'shares_outstanding' AND is_latest_revision`, ASOF-joined on `bars.cutoff >= h.available_at` with the running-latest-known-value dedupe described below.
3. `shares_outstanding = coalesce(dei_shares, archive_shares)`; `shares_source = CASE WHEN dei_shares IS NOT NULL THEN 'dei' WHEN archive_shares IS NOT NULL THEN 'archive' ELSE NULL END`; `shares_reconciliation_ratio = CASE WHEN archive_shares IS NULL OR archive_shares = 0 OR dei_shares IS NULL THEN NULL ELSE dei_shares / archive_shares END`.
4. Fundamentals and quarterly derived metrics — one `ASOF LEFT JOIN` per referenced code against

   ```sql
   latest_by_code AS (
       SELECT security_id, code, available_at, period_end, value
       FROM (
           SELECT security_id, code, available_at,
                  arg_max(value, (period_end, available_at, revision_sequence)) OVER w AS value,
                  max(period_end) OVER w AS period_end,
                  row_number() OVER (PARTITION BY security_id, code, available_at
                                     ORDER BY period_end DESC, revision_sequence DESC) AS rk
           FROM fund_long
           WINDOW w AS (PARTITION BY security_id, code ORDER BY available_at
                        ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW)
       )
       WHERE rk = 1
   )
   ```

   This is the *running latest-known value*: at each availability event the code carries the value of the newest fiscal period a consumer could have known, so a later-arriving restatement of an **older** period does not overwrite a newer period. A plain `ASOF` on `available_at` alone would, which is the audit §3.2 "availability-as-preference" defect in `enterprise_value.py` inverted; this join is a hard filter in both directions.
5. `fundamental_available_at = greatest(...)` over the joined codes' `available_at`, null-safe; `available_at = greatest(bar_at, coalesce(fundamental_available_at, bar_at))`. A row is emitted only when `available_at IS NOT NULL`.
6. Daily metrics are computed by chained CTEs, one per metric in topological order: CTE `m_<rank>` selects `p.*` plus `<value_sql> AS "<code>"` and `<availability_sql> AS "<code>__at"` from the previous relation aliased `p`. Because every name — item code, quarterly metric code and market column — is already a column of the running relation, `LowerContext.columns[name] = 'p."<name>"'` uniformly, and a daily metric can reference an earlier daily metric with no special case.
7. `inputs_hash = sha256(security_id || '|' || trade_date || '|' || close || '|' || adj_close || '|' || coalesce(shares_outstanding, -1) || '|' || coalesce(shares_source, '') || '|' || coalesce(CAST(fundamental_available_at AS VARCHAR), ''))`.
8. `as_of_date = trade_date`; the PIT gate is `available_at`, exactly as for `equity_daily_bars`.

- [ ] **Step 1: Write the failing test**

Create `C:\atx\atx-db\tests\test_market_daily.py`:

```python
"""Tier1-S3 T6: the daily market panel and its ASOF fundamental join."""

from __future__ import annotations

import datetime as dt
import math

import pytest

from atx_db.derived_metrics import DerivedMetricsOptions, refresh_derived_metrics
from atx_db.derived_registry import seed_derived_metric_definitions
from atx_db.market_daily import (
    END_OF_DAY_HOURS,
    MarketDailyOptions,
    refresh_market_daily_metrics,
    shares_reconciliation_report,
)

_QUARTERS = (
    dt.date(2019, 3, 31),
    dt.date(2019, 6, 30),
    dt.date(2019, 9, 30),
    dt.date(2019, 12, 31),
)
_FIRST_TRADE = dt.date(2020, 1, 2)


def _fact(store, security_id, code, basis, period_end, value, available_at, revision=1):
    store.con.execute(
        """
        INSERT INTO fundamental_standardized (
            standardized_id, source, security_id, item_id, canonical_code, basis,
            period_end, value, as_of_date, available_at, input_codes_json,
            input_item_ids_json, rule_id, combination_rule, revision_sequence,
            is_latest_revision
        ) VALUES (?, 'test', ?, 1, ?, ?, ?, ?, ?, ?, '[]', '[]', 'r', 'direct', ?, true)
        """,
        [
            f"{security_id}|{code}|{basis}|{period_end}|{revision}",
            security_id,
            code,
            basis,
            period_end,
            value,
            available_at.date(),
            available_at,
            revision,
        ],
    )


def _bar(store, security_id, trade_date, close, shares=None):
    store.con.execute(
        """
        INSERT INTO equity_daily_bars (
            source, security_id, symbol, trade_date, open, high, low, close,
            adjusted_close, volume, split_factor, is_adjusted, available_at,
            as_of_date, is_latest_revision, shares_outstanding
        ) VALUES ('test', ?, 'AAA', ?, ?, ?, ?, ?, ?, 1000, 1.0, false, ?, ?, true, ?)
        """,
        [
            security_id,
            trade_date,
            close,
            close,
            close,
            close,
            close,
            dt.datetime.combine(trade_date, dt.time(END_OF_DAY_HOURS, 0)),
            trade_date,
            shares,
        ],
    )


@pytest.fixture
def panel(tmp_store):
    seed_derived_metric_definitions(tmp_store)
    for index, period_end in enumerate(_QUARTERS):
        available_at = dt.datetime.combine(period_end + dt.timedelta(days=40), dt.time(21, 0))
        _fact(tmp_store, "S1", "revenue", "quarterly", period_end, 100.0 + index, available_at)
        _fact(tmp_store, "S1", "cost_of_revenue_cogs", "quarterly", period_end, 60.0, available_at)
        _fact(tmp_store, "S1", "net_income_to_common", "quarterly", period_end, 10.0, available_at)
        _fact(tmp_store, "S1", "total_assets", "instant", period_end, 1000.0, available_at)
        _fact(tmp_store, "S1", "stockholders_equity", "instant", period_end, 500.0, available_at)
    for offset in range(300):
        trade_date = _FIRST_TRADE + dt.timedelta(days=offset)
        if trade_date.weekday() >= 5:
            continue
        _bar(tmp_store, "S1", trade_date, 20.0 + 0.01 * offset, shares=1_000_000.0)
    refresh_derived_metrics(tmp_store, DerivedMetricsOptions())
    return tmp_store


def test_market_cap_is_price_times_shares(panel):
    refresh_market_daily_metrics(panel, MarketDailyOptions())
    row = panel.con.execute(
        "SELECT close, shares_outstanding, market_cap, shares_source "
        "FROM market_daily_metrics WHERE trade_date = ? ",
        [dt.date(2020, 6, 1)],
    ).fetchone()
    assert row is not None
    close, shares, market_cap, source = row
    assert market_cap == pytest.approx(close * shares)
    assert source == "archive"


def test_no_row_uses_a_fundamental_that_was_not_yet_available(panel):
    refresh_market_daily_metrics(panel, MarketDailyOptions())
    violations = panel.con.execute(
        """
        SELECT count(*) FROM market_daily_metrics
        WHERE fundamental_available_at IS NOT NULL
          AND fundamental_available_at > CAST(trade_date AS TIMESTAMP) + INTERVAL 22 HOUR
        """
    ).fetchone()[0]
    assert violations == 0


def test_available_at_is_never_before_the_bar_close(panel):
    refresh_market_daily_metrics(panel, MarketDailyOptions())
    violations = panel.con.execute(
        """
        SELECT count(*) FROM market_daily_metrics
        WHERE available_at < CAST(trade_date AS TIMESTAMP) + INTERVAL 22 HOUR
        """
    ).fetchone()[0]
    assert violations == 0


def test_a_fundamental_filed_after_the_close_is_used_only_from_the_next_day(tmp_store):
    seed_derived_metric_definitions(tmp_store)
    filed = dt.datetime(2020, 3, 2, 23, 0)  # after the 22:00 cutoff on 2020-03-02
    for index, period_end in enumerate(_QUARTERS):
        _fact(tmp_store, "S1", "revenue", "quarterly", period_end, 100.0 + index, filed)
    for trade_date in (dt.date(2020, 3, 2), dt.date(2020, 3, 3)):
        _bar(tmp_store, "S1", trade_date, 20.0, shares=1_000_000.0)
    refresh_derived_metrics(tmp_store, DerivedMetricsOptions())
    refresh_market_daily_metrics(tmp_store, MarketDailyOptions())
    rows = dict(
        tmp_store.con.execute(
            "SELECT trade_date, ps_ttm FROM market_daily_metrics ORDER BY trade_date"
        ).fetchall()
    )
    assert rows[dt.date(2020, 3, 2)] is None
    assert rows[dt.date(2020, 3, 3)] is not None


def test_dei_shares_win_over_archive_shares_and_are_reconciled(panel):
    panel.con.execute(
        """
        INSERT INTO shares_outstanding_history (
            share_history_id, source, security_id, cik, share_count_type, taxonomy,
            concept, unit, period_type, period_end, effective_date, as_of_date,
            available_at, accession_number, revision_sequence, revision_count,
            is_latest_revision, share_count
        ) VALUES ('h1','test','S1','0000000001','shares_outstanding','dei',
                  'EntityCommonStockSharesOutstanding','shares','instant',
                  DATE '2019-12-31', DATE '2020-01-15', DATE '2020-01-15',
                  TIMESTAMP '2020-01-15 21:00:00','acc',1,1,true, 1_020_000.0)
        """
    )
    refresh_market_daily_metrics(panel, MarketDailyOptions())
    row = panel.con.execute(
        "SELECT shares_outstanding, shares_source, shares_reconciliation_ratio "
        "FROM market_daily_metrics WHERE trade_date = ?",
        [dt.date(2020, 6, 1)],
    ).fetchone()
    shares, source, ratio = row
    assert source == "dei"
    assert shares == pytest.approx(1_020_000.0)
    assert ratio == pytest.approx(1.02)
    report = shares_reconciliation_report(panel)
    assert report["securities_with_both_sources"] == 1
    assert report["securities_within_tolerance"] == 1
    assert report["pass_rate"] == pytest.approx(1.0)
    assert report["meets_spec_gate"] is True


def test_a_ten_percent_shares_gap_fails_the_reconciliation_gate(panel):
    panel.con.execute(
        """
        INSERT INTO shares_outstanding_history (
            share_history_id, source, security_id, cik, share_count_type, taxonomy,
            concept, unit, period_type, period_end, effective_date, as_of_date,
            available_at, accession_number, revision_sequence, revision_count,
            is_latest_revision, share_count
        ) VALUES ('h2','test','S1','0000000001','shares_outstanding','dei',
                  'EntityCommonStockSharesOutstanding','shares','instant',
                  DATE '2019-12-31', DATE '2020-01-15', DATE '2020-01-15',
                  TIMESTAMP '2020-01-15 21:00:00','acc',1,1,true, 1_100_000.0)
        """
    )
    refresh_market_daily_metrics(panel, MarketDailyOptions())
    report = shares_reconciliation_report(panel)
    assert report["securities_within_tolerance"] == 0
    assert report["meets_spec_gate"] is False


def test_total_returns_come_from_adjusted_close_not_split_factor(panel):
    refresh_market_daily_metrics(panel, MarketDailyOptions())
    row = panel.con.execute(
        """
        SELECT m.adj_close, m.total_return_1m,
               (SELECT adj_close FROM market_daily_metrics x
                WHERE x.security_id = m.security_id AND x.trade_date < m.trade_date
                ORDER BY x.trade_date DESC LIMIT 1 OFFSET 20) AS base
        FROM market_daily_metrics m
        WHERE m.trade_date = DATE '2020-09-01'
        """
    ).fetchone()
    adj_close, one_month, base = row
    assert one_month == pytest.approx(adj_close / base - 1.0)


def test_realized_vol_is_annualized(panel):
    refresh_market_daily_metrics(panel, MarketDailyOptions())
    row = panel.con.execute(
        "SELECT realized_vol_60d FROM market_daily_metrics "
        "WHERE trade_date = DATE '2020-09-01'"
    ).fetchone()
    assert row[0] is not None
    assert 0.0 <= row[0] < 5.0


def test_momentum_skips_the_most_recent_month(panel):
    refresh_market_daily_metrics(panel, MarketDailyOptions())
    row = panel.con.execute(
        "SELECT total_return_12m, total_return_1m, momentum_12_1 FROM market_daily_metrics "
        "WHERE trade_date = DATE '2020-12-01'"
    ).fetchone()
    twelve, one, momentum = row
    if twelve is not None and one is not None:
        assert momentum == pytest.approx((1 + twelve) / (1 + one) - 1)


def test_dollar_volume_is_the_twenty_day_average(panel):
    refresh_market_daily_metrics(panel, MarketDailyOptions())
    row = panel.con.execute(
        "SELECT dollar_volume_20d FROM market_daily_metrics WHERE trade_date = DATE '2020-09-01'"
    ).fetchone()
    assert row[0] == pytest.approx(
        panel.con.execute(
            """
            SELECT avg(close * volume) FROM (
                SELECT close, volume FROM market_daily_metrics
                WHERE trade_date <= DATE '2020-09-01' ORDER BY trade_date DESC LIMIT 20
            )
            """
        ).fetchone()[0]
    )


def test_rerun_is_idempotent(panel):
    first = refresh_market_daily_metrics(panel, MarketDailyOptions())
    second = refresh_market_daily_metrics(panel, MarketDailyOptions())
    assert first == second
    total = panel.con.execute("SELECT count(*) FROM market_daily_metrics").fetchone()[0]
    assert total == first


def test_inputs_hash_is_populated_and_stable(panel):
    refresh_market_daily_metrics(panel, MarketDailyOptions())
    before = panel.con.execute(
        "SELECT inputs_hash FROM market_daily_metrics ORDER BY trade_date"
    ).fetchall()
    refresh_market_daily_metrics(panel, MarketDailyOptions())
    after = panel.con.execute(
        "SELECT inputs_hash FROM market_daily_metrics ORDER BY trade_date"
    ).fetchall()
    assert before == after
    assert all(len(str(row[0])) == 64 for row in before)


def test_no_wall_clock_in_the_module_source():
    import inspect

    import atx_db.market_daily as module

    source = inspect.getsource(module)
    for forbidden in ("date.today", "utcnow", "Timestamp.now", "time.time"):
        assert forbidden not in source, forbidden
    assert math is not None
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `.venv\Scripts\python.exe -m pytest tests/test_market_daily.py -n 0 -q`
Expected: FAIL — `ModuleNotFoundError: No module named 'atx_db.market_daily'`.

- [ ] **Step 3: Create `src/atx_db/market_daily.py`**

```python
"""The daily market panel: bars ASOF-joined to point-in-time fundamentals."""

from __future__ import annotations

import datetime as dt
from dataclasses import dataclass
from typing import Any, Iterable

from .connection import DuckDBStore
from .dataset import Dataset, DatasetLoadResult
from .derived_dsl import LowerContext, compile_expression
from .derived_registry import (
    DERIVED_SOURCE_NAME,
    DerivedMetricDefinition,
    default_derived_definitions,
    topological_order,
)

__all__ = [
    "END_OF_DAY_HOURS",
    "MARKET_DAILY_SOURCE_NAME",
    "MarketDailyDataset",
    "MarketDailyOptions",
    "build_market_daily_sql",
    "daily_context",
    "refresh_market_daily_metrics",
    "shares_reconciliation_report",
]

MARKET_DAILY_SOURCE_NAME = "atx-db daily market panel v1"
#: The end-of-day availability convention stamped onto every bar by
#: ``ticker_history.py`` (``trading_date + 22 hours``).
END_OF_DAY_HOURS = 22
_MARKET_BASE_COLUMNS = ("close", "adj_close", "volume", "log_return", "shares_outstanding")


@dataclass(frozen=True)
class MarketDailyOptions:
    source: str = MARKET_DAILY_SOURCE_NAME
    derived_source: str = DERIVED_SOURCE_NAME
    bar_source: str | None = None
    start_date: dt.date | None = None
    end_date: dt.date | None = None
    security_ids: tuple[str, ...] | None = None
    batch_size: int = 200
    shares_tolerance: float = 0.05
    run_id: str | None = None


def daily_context(*, names: Iterable[str]) -> LowerContext:
    columns = {name: f'p."{name}"' for name in names}
    availability = {name: f'p."{name}__at"' for name in names}
    return LowerContext(
        grid="day",
        columns=columns,
        availability=availability,
        partition_sql="p.security_id",
        order_sql="p.trade_date",
    )


def _quote(code: str) -> str:
    return "'" + code.replace("'", "''") + "'"


def _asof_joins(codes: tuple[str, ...]) -> tuple[str, str]:
    joins: list[str] = []
    projections: list[str] = []
    for index, code in enumerate(codes):
        alias = f"f{index}"
        joins.append(
            f"ASOF LEFT JOIN (SELECT security_id, available_at, value FROM latest_by_code "
            f"WHERE code = {_quote(code)}) {alias} "
            f"ON {alias}.security_id = b.security_id AND b.cutoff >= {alias}.available_at"
        )
        projections.append(f'{alias}.value AS "{code}"')
        projections.append(f'{alias}.available_at AS "{code}__at"')
    return "\n    ".join(joins), ",\n           ".join(projections)


def build_market_daily_sql(
    *,
    item_codes: tuple[str, ...],
    metric_codes: tuple[str, ...],
    daily_definitions: tuple[DerivedMetricDefinition, ...],
    security_count: int,
    date_predicate: str,
) -> str:
    codes = tuple(item_codes) + tuple(metric_codes)
    joins, projections = _asof_joins(codes)
    securities = ", ".join(["?"] * security_count)
    availability_terms = ", ".join(
        [f'coalesce(f{index}.available_at, TIMESTAMP \'-infinity\')' for index in range(len(codes))]
    )
    fundamental_at = (
        f"nullif(greatest({availability_terms}), TIMESTAMP '-infinity')"
        if codes
        else "CAST(NULL AS TIMESTAMP)"
    )

    chain: list[str] = []
    previous = "panel"
    for rank, definition in enumerate(daily_definitions):
        names = tuple(definition.item_inputs) + tuple(definition.metric_inputs) + tuple(
            definition.market_inputs
        )
        lowered = compile_expression(definition.expression, daily_context(names=names))
        alias = f"m{rank}"
        chain.append(
            f"""{alias} AS (
    SELECT p.*,
           {lowered.value_sql} AS "{definition.metric_code}",
           {lowered.availability_sql} AS "{definition.metric_code}__at"
    FROM {previous} p
)"""
        )
        previous = alias

    metric_columns = ", ".join(f'"{definition.metric_code}"' for definition in daily_definitions)
    chain_sql = (",\n".join(chain) + ",\n") if chain else ""

    return f"""
INSERT INTO market_daily_metrics (
    market_daily_id, source, security_id, symbol, trade_date, close, adj_close, volume,
    shares_outstanding, shares_source, shares_reconciliation_ratio,
    {metric_columns},
    fundamental_available_at, available_at, inputs_hash, as_of_date, is_latest_revision, run_id
)
WITH bars AS (
    SELECT security_id, trade_date,
           arg_max(symbol, (available_at, source)) AS symbol,
           arg_max(close, (available_at, source)) AS close,
           arg_max(adjusted_close, (available_at, source)) AS adj_close,
           arg_max(volume, (available_at, source)) AS volume,
           arg_max(shares_outstanding, (available_at, source)) AS archive_shares,
           greatest(max(available_at),
                    CAST(trade_date AS TIMESTAMP) + INTERVAL {END_OF_DAY_HOURS} HOUR) AS bar_at,
           CAST(trade_date AS TIMESTAMP) + INTERVAL {END_OF_DAY_HOURS} HOUR AS cutoff
    FROM equity_daily_bars
    WHERE close > 0 AND adjusted_close > 0 AND trade_date IS NOT NULL
      AND security_id IN ({securities})
      {date_predicate}
    GROUP BY security_id, trade_date
), fund_long AS (
    SELECT security_id, canonical_code AS code, period_end, value, available_at, revision_sequence
    FROM fundamental_standardized
    WHERE is_latest_revision AND basis IN ('quarterly', 'instant')
      AND canonical_code IN ({", ".join(_quote(code) for code in item_codes) or "''"})
    UNION ALL
    SELECT security_id, metric_code AS code, period_end, value, available_at, 0
    FROM derived_metric_values
    WHERE source = ?
      AND metric_code IN ({", ".join(_quote(code) for code in metric_codes) or "''"})
), latest_by_code AS (
    SELECT security_id, code, available_at, value
    FROM (
        SELECT security_id, code, available_at,
               arg_max(value, (period_end, available_at, revision_sequence)) OVER w AS value,
               row_number() OVER (PARTITION BY security_id, code, available_at
                                  ORDER BY period_end DESC, revision_sequence DESC) AS rk
        FROM fund_long
        WINDOW w AS (PARTITION BY security_id, code ORDER BY available_at
                     ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW)
    )
    WHERE rk = 1
), shares_state AS (
    SELECT security_id, available_at, share_count
    FROM (
        SELECT security_id, available_at,
               arg_max(share_count, (effective_date, available_at, revision_sequence)) OVER w AS share_count,
               row_number() OVER (PARTITION BY security_id, available_at
                                  ORDER BY effective_date DESC, revision_sequence DESC) AS rk
        FROM shares_outstanding_history
        WHERE is_latest_revision AND share_count_type = 'shares_outstanding'
          AND available_at IS NOT NULL AND share_count > 0
        WINDOW w AS (PARTITION BY security_id ORDER BY available_at
                     ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW)
    )
    WHERE rk = 1
), joined AS (
    SELECT b.security_id, b.trade_date, b.symbol, b.close, b.adj_close, b.volume,
           b.archive_shares, b.bar_at, b.cutoff,
           s.share_count AS dei_shares,
           {projections},
           {fundamental_at} AS fundamental_available_at
    FROM bars b
    ASOF LEFT JOIN shares_state s
      ON s.security_id = b.security_id AND b.cutoff >= s.available_at
    {joins}
), panel AS (
    SELECT j.security_id, j.trade_date, j.symbol, j.close, j.adj_close, j.volume,
           j.archive_shares, j.dei_shares, j.bar_at, j.fundamental_available_at,
           coalesce(j.dei_shares, j.archive_shares) AS "shares_outstanding",
           CASE WHEN j.dei_shares IS NOT NULL THEN 'dei'
                WHEN j.archive_shares IS NOT NULL THEN 'archive' END AS shares_source,
           CASE WHEN j.dei_shares IS NULL OR j.archive_shares IS NULL OR j.archive_shares = 0
                THEN NULL ELSE j.dei_shares / j.archive_shares END AS shares_reconciliation_ratio,
           j.bar_at AS "close__at", j.bar_at AS "adj_close__at", j.bar_at AS "volume__at",
           j.bar_at AS "shares_outstanding__at", j.bar_at AS "log_return__at",
           CASE WHEN lag(j.adj_close) OVER (PARTITION BY j.security_id ORDER BY j.trade_date) > 0
                THEN ln(j.adj_close / lag(j.adj_close) OVER (PARTITION BY j.security_id
                                                             ORDER BY j.trade_date)) END AS "log_return",
           j.* EXCLUDE (security_id, trade_date, symbol, close, adj_close, volume,
                        archive_shares, dei_shares, bar_at, cutoff, fundamental_available_at)
    FROM joined j
),
{chain_sql}final AS (SELECT * FROM {previous})
SELECT sha256(? || '|' || f.security_id || '|' || CAST(f.trade_date AS VARCHAR)),
       ?, f.security_id, f.symbol, f.trade_date, f.close, f.adj_close, f.volume,
       f."shares_outstanding", f.shares_source, f.shares_reconciliation_ratio,
       {metric_columns},
       f.fundamental_available_at,
       greatest(f.bar_at, coalesce(f.fundamental_available_at, f.bar_at)),
       sha256(f.security_id || '|' || CAST(f.trade_date AS VARCHAR) || '|' ||
              CAST(f.close AS VARCHAR) || '|' || CAST(f.adj_close AS VARCHAR) || '|' ||
              coalesce(CAST(f."shares_outstanding" AS VARCHAR), '') || '|' ||
              coalesce(f.shares_source, '') || '|' ||
              coalesce(CAST(f.fundamental_available_at AS VARCHAR), '')),
       f.trade_date, true, ?
FROM final f
ORDER BY f.security_id, f.trade_date
"""


def _daily_definitions() -> tuple[DerivedMetricDefinition, ...]:
    definitions = default_derived_definitions()
    ordered = topological_order(definitions)
    return tuple(d for d in ordered if d.window == "daily")


def _referenced_codes() -> tuple[tuple[str, ...], tuple[str, ...]]:
    daily = _daily_definitions()
    daily_codes = {d.metric_code for d in daily}
    items: set[str] = set()
    metrics: set[str] = set()
    for definition in daily:
        items.update(definition.item_inputs)
        metrics.update(code for code in definition.metric_inputs if code not in daily_codes)
    return tuple(sorted(items)), tuple(sorted(metrics))


def refresh_market_daily_metrics(
    store: DuckDBStore,
    options: MarketDailyOptions | None = None,
) -> int:
    options = options or MarketDailyOptions()
    store.initialize()
    item_codes, metric_codes = _referenced_codes()
    daily = _daily_definitions()

    if options.security_ids is not None:
        identifiers = sorted(options.security_ids)
    else:
        identifiers = [
            str(row[0])
            for row in store.con.execute(
                "SELECT DISTINCT security_id FROM equity_daily_bars "
                "WHERE close > 0 ORDER BY security_id"
            ).fetchall()
        ]
    size = max(1, int(options.batch_size))
    batches = [tuple(identifiers[i : i + size]) for i in range(0, len(identifiers), size)]

    date_fragments: list[str] = []
    date_params: list[Any] = []
    if options.start_date is not None:
        date_fragments.append("AND trade_date >= ?")
        date_params.append(options.start_date)
    if options.end_date is not None:
        date_fragments.append("AND trade_date <= ?")
        date_params.append(options.end_date)
    if options.bar_source is not None:
        date_fragments.append("AND source = ?")
        date_params.append(options.bar_source)
    date_predicate = "\n      ".join(date_fragments)

    total = 0
    for batch in batches:
        sql = build_market_daily_sql(
            item_codes=item_codes,
            metric_codes=metric_codes,
            daily_definitions=daily,
            security_count=len(batch),
            date_predicate=date_predicate,
        )
        bind: list[Any] = [*batch, *date_params, options.derived_source, options.source, options.source,
                           options.run_id]
        with store.transaction():
            predicates = ["source = ?", f"security_id IN ({', '.join(['?'] * len(batch))})"]
            params: list[Any] = [options.source, *batch]
            if options.start_date is not None:
                predicates.append("trade_date >= ?")
                params.append(options.start_date)
            if options.end_date is not None:
                predicates.append("trade_date <= ?")
                params.append(options.end_date)
            store.con.execute(
                f"DELETE FROM market_daily_metrics WHERE {' AND '.join(predicates)}", params
            )
            store.con.execute(sql, bind)
            total += int(
                store.con.execute(
                    f"SELECT count(*) FROM market_daily_metrics WHERE source = ? "
                    f"AND security_id IN ({', '.join(['?'] * len(batch))})",
                    [options.source, *batch],
                ).fetchone()[0]
            )
    return total


def shares_reconciliation_report(
    store: DuckDBStore,
    *,
    source: str = MARKET_DAILY_SOURCE_NAME,
    tolerance: float = 0.05,
) -> dict[str, object]:
    row = store.con.execute(
        """
        WITH per_security AS (
            SELECT security_id,
                   count(*) FILTER (WHERE shares_reconciliation_ratio IS NOT NULL) AS observed,
                   count(*) FILTER (
                       WHERE shares_reconciliation_ratio IS NOT NULL
                         AND abs(shares_reconciliation_ratio - 1.0) <= ?
                   ) AS within
            FROM market_daily_metrics
            WHERE source = ?
            GROUP BY security_id
        )
        SELECT count(*) FILTER (WHERE observed > 0),
               count(*) FILTER (WHERE observed > 0 AND within = observed)
        FROM per_security
        """,
        [tolerance, source],
    ).fetchone()
    both = int(row[0])
    within = int(row[1])
    pass_rate = (within / both) if both else 0.0
    return {
        "securities_with_both_sources": both,
        "securities_within_tolerance": within,
        "pass_rate": pass_rate,
        "tolerance": tolerance,
        "meets_spec_gate": bool(both) and pass_rate >= 0.95,
    }


class MarketDailyDataset(Dataset):
    dataset_id = "market_daily"
    source_name = MARKET_DAILY_SOURCE_NAME

    def ensure_schema(self, store: DuckDBStore) -> None:
        store.initialize()

    def load(self, store: DuckDBStore, options: Any) -> DatasetLoadResult:
        resolved = options if isinstance(options, MarketDailyOptions) else MarketDailyOptions()
        rows = refresh_market_daily_metrics(store, resolved)
        return DatasetLoadResult(
            dataset_id=self.dataset_id,
            rows_loaded=rows,
            source=resolved.source,
            details=shares_reconciliation_report(
                store, source=resolved.source, tolerance=resolved.shares_tolerance
            ),
        )
```

Implementation notes for the executor:
- The `panel` CTE uses `j.* EXCLUDE (...)` to carry the per-code `"<code>"` / `"<code>__at"` pairs forward without naming them twice. If the installed DuckDB rejects `EXCLUDE` inside a projection that also lists explicit columns, replace it with the explicit generated column list (`_asof_joins` already knows the names) — do not drop the columns.
- `log_return` is defined after the `EXCLUDE` in the same `SELECT`; DuckDB allows sibling column references only via a lateral or a second CTE. If it errors, split `panel` into `panel_raw` (everything except `log_return`) and `panel` (`SELECT p.*, <log_return expr> FROM panel_raw p`) — two CTEs, no behaviour change.
- Bind order is: batch securities, date params, `derived_source`, then `source` twice (the id hash and the `source` column), then `run_id`. Assert this in `test_rerun_is_idempotent` failing loudly rather than silently mis-binding.

- [ ] **Step 4: Create `scripts/build_market_daily.py`**

Mirror `scripts/build_derived_metrics.py` exactly, substituting `refresh_market_daily_metrics` / `MarketDailyOptions`, adding `--start-date` / `--end-date` parsed with `dt.date.fromisoformat`, and printing `json.dumps({"rows": rows, **shares_reconciliation_report(store)}, sort_keys=True, default=str)`.

- [ ] **Step 5: Add `market_daily` to the public API snapshot**

Insert `"market_daily"` into the `"atx_db"` array, in sorted position (between `"macro_metrics"` and `"metric_engine"`).

- [ ] **Step 6: Run the tests**

Run: `.venv\Scripts\python.exe -m pytest tests/test_market_daily.py -n 0 -q`
Expected: `13 passed`.

Run: `.venv\Scripts\python.exe -m pytest tests/test_derived_metrics.py tests/test_module_boundaries.py -n 0 -q`
Expected: `passed`.

- [ ] **Step 7: Commit**

```
git add src/atx_db/market_daily.py scripts/build_market_daily.py tests/test_market_daily.py tests/data/public_api_snapshot.json
git commit -m "feat(db): add the daily market panel with a PIT ASOF fundamental join

equity_daily_bars ASOF-joined to the running latest-known standardized fact and
derived metric with available_at <= trade_date + 22h, so a later restatement of
an older period never overwrites a newer one. Shares reconcile dei against the
vendor archive with a source flag, a ratio column and a 5% spec gate. Total
returns come from adjusted_close, never from split_factor, which holds the
vendor returnFactor.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 7: `derived_factor_projection.py` and the parity harness

**Files:**
- Create: `C:\atx\atx-db\src\atx_db\derived_factor_projection.py`
- Create: `C:\atx\atx-db\src\atx_db\seeds\derived_factor_projections.csv`
- Modify: `C:\atx\atx-db\tests\data\public_api_snapshot.json` (add `"derived_factor_projection"`)
- Test: `C:\atx\atx-db\tests\test_derived_parity.py` (new)

**Why this task exists.** Every per-metric module writes two numbers: `raw_value` (the metric) and `value` (that metric winsorized and z-scored cross-sectionally within `(factor_id, as_of_date)`), on a **monthly rebalance grid** whose `as_of_date` is the last trade date of a month, gated by `universe_membership`. Deleting a module without replacing both numbers would silently drop a published `factor_id` from `fundamental_factor_values`. `derived_factor_projection.py` is the generic replacement: it projects any engine metric onto that grid and applies the *same* `atx_db.factors.cross_section.winsorize` → `zscore` pair the 46 pandas modules use, so the retirement in Task 8 is a refactor rather than a feature removal.

**Interfaces:**
- Consumes: `atx_db.factors.cross_section.winsorize`, `zscore` (existing); `atx_db.universe.DEFAULT_UNIVERSE_ID` (existing); `atx_db.warehouse.insert_frame`, `json_dumps` (existing); `atx_db.market_daily.END_OF_DAY_HOURS` (Task 6); `atx_db.derived_registry.DERIVED_SOURCE_NAME` (Task 3).
- Produces:
  - `atx_db.derived_factor_projection.PROJECTION_SEED_PATH: Path`
  - `atx_db.derived_factor_projection.PROJECTION_SEED_COLUMNS: tuple[str, ...]` = `("factor_id", "metric_code", "source_window", "orientation", "factor_name", "family", "winsor_limit", "minimum_names_per_date", "retired_module")`
  - `atx_db.derived_factor_projection.FactorProjection` — frozen dataclass mirroring those columns (`orientation: int` ∈ `{1, -1}`, `winsor_limit: float`, `minimum_names_per_date: int`).
  - `atx_db.derived_factor_projection.read_projection_seed(path=PROJECTION_SEED_PATH) -> tuple[FactorProjection, ...]`
  - `atx_db.derived_factor_projection.default_projections() -> tuple[FactorProjection, ...]` (`lru_cache`d)
  - `atx_db.derived_factor_projection.PROJECTION_OUTPUT_COLUMNS: tuple[str, ...]` — the identical 15-column shape all 46 pandas modules use: `factor_value_id, factor_id, factor_name, family, security_id, symbol, as_of_date, raw_value, value, available_at, input_ids_json, input_lineage_json, is_latest_revision, run_id, source`.
  - `atx_db.derived_factor_projection.FactorProjectionOptions` — frozen dataclass: `source: str = "atx-db derived factor projection v1"`, `derived_source: str = DERIVED_SOURCE_NAME`, `market_source: str = MARKET_DAILY_SOURCE_NAME`, `universe_id: str = DEFAULT_UNIVERSE_ID`, `factor_ids: tuple[str, ...] | None = None`, `start_date: dt.date | None = None`, `end_date: dt.date | None = None`, `run_id: str | None = None`.
  - `atx_db.derived_factor_projection.load_projection_inputs(store, projection, options) -> pd.DataFrame` — columns `security_id, symbol, as_of_date, metric_value, metric_available_at, decision_available_at, period_end`.
  - `atx_db.derived_factor_projection.compute_projection_rows(inputs, projection, options) -> pd.DataFrame`
  - `atx_db.derived_factor_projection.refresh_projected_factor_values(store, options=None) -> int`

**The seed** (`src/atx_db/seeds/derived_factor_projections.csv`) — 24 rows covering the 18 modules Task 8 deletes. `orientation` is the module's own sign convention read out of its `_lineage()` `"orientation"` / `"formula"` field.

```csv
factor_id,metric_code,source_window,orientation,factor_name,family,winsor_limit,minimum_names_per_date,retired_module
distress_altman_z_score,altman_z,daily,1,PIT corrected Altman Z-score,fundamental_distress,0.01,20,altman_distress
quality_low_beneish_m_score,beneish_m,quarter,-1,PIT low Beneish M-score,fundamental_quality,0.01,20,beneish_m_score
quality_net_operating_assets,noa_to_assets,quarter,-1,PIT low net operating assets,fundamental_quality,0.01,20,net_operating_assets
quality_low_rsst_accruals,rsst_accruals,quarter,-1,PIT low RSST accruals,fundamental_quality,0.01,20,rsst_accruals
quality_low_quarterly_operating_working_capital_accruals,working_capital_accruals,quarter,-1,PIT low quarterly operating working-capital accruals,fundamental_quality,0.01,20,quarterly_working_capital_accruals
efficiency_annual_asset_turnover_change,asset_turnover_change_yoy,quarter,1,PIT annual asset-turnover change,fundamental_efficiency,0.01,20,asset_turnover_change
profitability_annual_net_margin_change,net_margin_change_yoy,quarter,1,PIT annual change in net profit margin,fundamental_profitability,0.01,20,annual_margin_change
profitability_annual_operating_margin_change,operating_margin_change_yoy,quarter,1,PIT annual change in operating margin,fundamental_profitability,0.01,20,annual_margin_change
profitability_annual_gross_margin_change,gross_margin_change_yoy,quarter,1,PIT annual change in gross margin,fundamental_profitability,0.01,20,annual_margin_change
profitability_quarterly_gross_margin_change_yoy,gross_margin_change_yoy,quarter,1,PIT quarterly gross-margin change year over year,fundamental_profitability,0.01,20,quarterly_gross_margin_change
profitability_quarterly_operating_profitability_change_yoy,operating_profitability_change_yoy,quarter,1,PIT quarterly operating-profitability change year over year,fundamental_profitability,0.01,20,quarterly_profitability_change
financing_low_external_financing,external_financing,quarter,-1,PIT low external financing,fundamental_financing,0.01,20,external_financing
financing_low_net_debt_financing,net_debt_issuance,quarter,-1,PIT low net debt financing,fundamental_financing,0.01,20,net_debt_financing
financing_low_net_share_issuance,shares_growth_yoy,quarter,-1,PIT low net share issuance,fundamental_financing,0.01,20,net_issuance
financing_net_payout_yield,net_payout_yield,daily,1,PIT cash-flow net payout yield,fundamental_financing,0.01,20,net_payout
valuation_enterprise_yield_ebit,ebit_to_ev,daily,1,PIT operating-income enterprise yield,fundamental_valuation,0.01,20,enterprise_yield
valuation_gross_profit_enterprise_yield,gross_profit_to_ev,daily,1,PIT gross-profit enterprise yield,fundamental_valuation,0.01,20,enterprise_yield
valuation_operating_cash_flow_enterprise_yield,cfo_to_ev,daily,1,PIT operating-cash-flow enterprise yield,fundamental_valuation,0.01,20,enterprise_yield
valuation_enterprise_yield_sales,sales_to_ev,daily,1,PIT revenue enterprise yield,fundamental_valuation,0.01,20,enterprise_yield
valuation_rd_to_market_equity,rd_to_market_equity,daily,1,PIT research and development to market equity,fundamental_valuation,0.01,20,rd_intensity
intangibles_large_rd_increase,rd_expense_growth_yoy,quarter,1,PIT large research and development increase,fundamental_intangibles,0.01,20,rd_increase
earnings_tax_expense_momentum,tax_expense_change_yoy,quarter,1,PIT tax-expense momentum,fundamental_earnings,0.01,20,tax_expense_momentum
earnings_tax_to_book_income,tax_to_book_income,quarter,1,PIT tax to book income,fundamental_earnings,0.01,20,tax_to_book_income
investment_conservative_asset_growth,asset_growth,quarter,-1,PIT conservative annual asset growth,fundamental_investment,0.01,20,KEPT
```

The last row's `retired_module` is the literal `KEPT`: `asset_growth.py` is **not** deleted (twelve other modules import it — see Task 8), but its metric is in the catalog and the projection is proven against it, which is what makes the parity harness meaningful for the module that stays.

**Rebalance grid** (identical to the one `asset_growth.load_asset_growth_inputs` builds, verified by reading it):

```sql
price_dedup AS (
    SELECT security_id, any_value(symbol) AS symbol, trade_date, max(available_at) AS price_available_at
    FROM equity_daily_bars
    WHERE close > 0 AND trade_date IS NOT NULL AND available_at IS NOT NULL
    GROUP BY security_id, trade_date
), price_months AS (
    SELECT *, row_number() OVER (PARTITION BY security_id, year(trade_date), month(trade_date)
                                 ORDER BY trade_date DESC) AS month_rank
    FROM price_dedup
), rebalances AS (SELECT * FROM price_months WHERE month_rank = 1),
governed AS (
    SELECT p.*, u.available_at AS universe_available_at, u.universe_id,
           u.valid_from AS universe_valid_from, u.valid_to AS universe_valid_to,
           u.source AS universe_source,
           row_number() OVER (PARTITION BY p.security_id, p.trade_date
                              ORDER BY u.valid_from DESC, u.available_at DESC NULLS LAST,
                                       u.source_loaded_at DESC, u.source DESC) AS universe_rank
    FROM rebalances p
    JOIN universe_membership u
      ON u.universe_id = ? AND u.security_id = p.security_id
     AND u.valid_from <= p.trade_date AND (u.valid_to IS NULL OR u.valid_to >= p.trade_date)
     AND u.as_of_date <= p.trade_date AND u.is_member AND u.is_latest_revision
     AND (u.available_at IS NULL OR u.available_at <= p.price_available_at)
)
```

For `source_window = 'quarter'` the metric is ASOF-joined from `derived_metric_values` on `available_at <= trade_date + 22 HOUR`; for `'daily'` it is read directly from `market_daily_metrics` at that `trade_date`. `decision_available_at = greatest(price_available_at, universe_available_at, metric_available_at)`.

- [ ] **Step 1: Write the failing test**

Create `C:\atx\atx-db\tests\test_derived_parity.py`:

```python
"""Tier1-S3 T7: the engine reproduces every retired module's raw metric."""

from __future__ import annotations

import datetime as dt
import importlib

import pandas as pd
import pytest

from atx_db.derived_factor_projection import (
    FactorProjectionOptions,
    default_projections,
    refresh_projected_factor_values,
)
from atx_db.derived_metrics import DerivedMetricsOptions, refresh_derived_metrics
from atx_db.derived_registry import default_derived_definitions, seed_derived_metric_definitions
from atx_db.market_daily import MarketDailyOptions, refresh_market_daily_metrics

RELATIVE_TOLERANCE = 1e-9

PROJECTION_SOURCE = "atx-db derived factor projection v1"

# The 61 per-metric modules come in two shapes (audit §3.1c): 46 pandas modules
# expose ``load_<x>_inputs`` + ``compute_<x>_rows``, and 15 pure set-based
# modules expose only ``refresh_<x>_values``. ``annual_margin_change`` is a third
# case: it has the pandas pair and no refresh function at all. Each entry is
# (module, options class, kind, entry points, prerequisite refreshers); every
# name below was read out of the module on main, not guessed.
MODULE_CASES = (
    ("altman_distress", "AltmanDistressOptions", "frame",
     ("load_altman_distress_inputs", "compute_altman_distress_rows"),
     (("cash_flow_profitability", "refresh_cash_flow_profitability_values",
       "CashFlowProfitabilityOptions"),
      ("fundamental_signals", "refresh_fundamental_signal_values", "FundamentalSignalOptions"))),
    ("net_operating_assets", "NetOperatingAssetsOptions", "frame",
     ("load_net_operating_assets_inputs", "compute_net_operating_assets_rows"), ()),
    ("quarterly_working_capital_accruals", "QuarterlyWorkingCapitalAccrualsOptions", "frame",
     ("load_quarterly_working_capital_accruals_inputs",
      "compute_quarterly_working_capital_accruals_rows"), ()),
    ("asset_turnover_change", "AssetTurnoverChangeOptions", "frame",
     ("load_asset_turnover_change_inputs", "compute_asset_turnover_change_rows"), ()),
    ("annual_margin_change", "AnnualMarginChangeOptions", "frame",
     ("load_annual_margin_change_inputs", "compute_annual_margin_change_rows"), ()),
    ("quarterly_gross_margin_change", "QuarterlyGrossMarginChangeOptions", "frame",
     ("load_quarterly_gross_margin_change_inputs", "compute_quarterly_gross_margin_change_rows"), ()),
    ("quarterly_profitability_change", "QuarterlyProfitabilityChangeOptions", "frame",
     ("load_quarterly_profitability_change_inputs", "compute_quarterly_profitability_change_rows"), ()),
    ("net_issuance", "NetIssuanceOptions", "frame",
     ("load_net_issuance_inputs", "compute_net_issuance_rows"), ()),
    ("net_payout", "NetPayoutOptions", "frame",
     ("load_net_payout_inputs", "compute_net_payout_rows"), ()),
    ("enterprise_yield", "EnterpriseYieldOptions", "frame",
     ("load_enterprise_yield_inputs", "compute_enterprise_yield_rows"), ()),
    ("asset_growth", "AssetGrowthOptions", "frame",
     ("load_asset_growth_inputs", "compute_asset_growth_rows"), ()),
    ("beneish_m_score", "BeneishMScoreOptions", "refresh",
     ("refresh_beneish_m_score_values",), ()),
    ("rsst_accruals", "RsstAccrualsOptions", "refresh", ("refresh_rsst_accruals_values",), ()),
    ("external_financing", "ExternalFinancingOptions", "refresh",
     ("refresh_external_financing_values",), ()),
    ("net_debt_financing", "NetDebtFinancingOptions", "refresh",
     ("refresh_net_debt_financing_values",), ()),
    ("rd_intensity", "RdIntensityOptions", "refresh", ("refresh_rd_intensity_values",), ()),
    ("rd_increase", "RdIncreaseOptions", "refresh", ("refresh_rd_increase_values",), ()),
    ("tax_expense_momentum", "TaxExpenseMomentumOptions", "refresh",
     ("refresh_tax_expense_momentum_values",), ()),
    ("tax_to_book_income", "TaxToBookIncomeOptions", "refresh",
     ("refresh_tax_to_book_income_values",), ()),
)


def _module_rows(store, case) -> pd.DataFrame:
    """Return the module's own (factor_id, security_id, as_of_date, raw_value, value) rows."""
    module_name, options_name, kind, entry_points, prerequisites = case
    module = importlib.import_module(f"atx_db.{module_name}")
    for prerequisite_module, prerequisite_function, prerequisite_options in prerequisites:
        prerequisite = importlib.import_module(f"atx_db.{prerequisite_module}")
        getattr(prerequisite, prerequisite_function)(
            store, getattr(prerequisite, prerequisite_options)()
        )
    options = getattr(module, options_name)()
    if kind == "frame":
        loader_name, computer_name = entry_points
        frame = getattr(module, computer_name)(getattr(module, loader_name)(store, options), options)
        return frame[["factor_id", "security_id", "as_of_date", "raw_value", "value"]].copy()
    getattr(module, entry_points[0])(store, options)
    return store.con.execute(
        "SELECT factor_id, security_id, as_of_date, raw_value, value "
        "FROM fundamental_factor_values WHERE source = ? ORDER BY 1, 2, 3",
        [module.SOURCE_NAME],
    ).df()


def test_every_module_case_resolves_to_real_symbols():
    """The case table is the contract; a rename upstream must fail loudly here."""
    for module_name, options_name, kind, entry_points, prerequisites in MODULE_CASES:
        module = importlib.import_module(f"atx_db.{module_name}")
        assert hasattr(module, options_name), f"{module_name}.{options_name}"
        assert hasattr(module, "SOURCE_NAME"), f"{module_name}.SOURCE_NAME"
        assert kind in ("frame", "refresh")
        for name in entry_points:
            assert hasattr(module, name), f"{module_name}.{name}"
        for prerequisite_module, prerequisite_function, prerequisite_options in prerequisites:
            prerequisite = importlib.import_module(f"atx_db.{prerequisite_module}")
            assert hasattr(prerequisite, prerequisite_function)
            assert hasattr(prerequisite, prerequisite_options)


def test_every_projection_metric_exists_in_the_catalog():
    codes = {definition.metric_code for definition in default_derived_definitions()}
    missing = sorted(
        projection.metric_code
        for projection in default_projections()
        if projection.metric_code not in codes
    )
    assert missing == []


def test_every_projection_orientation_is_plus_or_minus_one():
    assert {projection.orientation for projection in default_projections()} == {1, -1}


def test_projection_covers_every_retired_module(retired_modules):
    covered = {projection.retired_module for projection in default_projections()}
    assert set(retired_modules) <= covered


@pytest.fixture
def retired_modules():
    return {projection.retired_module for projection in default_projections()} - {"KEPT"}


@pytest.fixture
def parity_warehouse(tmp_store, parity_facts):
    """A warehouse whose statement-points and standardized layers carry identical facts.

    The per-metric modules read ``fundamental_statement_points``; the engine reads
    ``fundamental_standardized``. Parity at 1e-9 is only meaningful when both
    tables carry the same numbers, so the fixture writes both from one list and
    asserts the precondition before any comparison runs.
    """
    seed_derived_metric_definitions(tmp_store)
    parity_facts(tmp_store)
    mismatched = tmp_store.con.execute(
        """
        SELECT count(*) FROM fundamental_standardized s
        FULL OUTER JOIN fundamental_statement_points p
          ON p.security_id = s.security_id AND p.canonical_metric = s.canonical_code
         AND p.period_end = s.period_end
        WHERE s.value IS DISTINCT FROM p.value
        """
    ).fetchone()[0]
    assert mismatched == 0, "the parity fixture must write identical facts to both layers"
    refresh_derived_metrics(tmp_store, DerivedMetricsOptions())
    refresh_market_daily_metrics(tmp_store, MarketDailyOptions())
    refresh_projected_factor_values(tmp_store, FactorProjectionOptions())
    return tmp_store


@pytest.mark.parametrize("case", MODULE_CASES, ids=[case[0] for case in MODULE_CASES])
def test_engine_metric_matches_the_module_raw_value(parity_warehouse, case):
    module_name = case[0]
    expected = _module_rows(parity_warehouse, case)
    if expected.empty:
        pytest.skip(f"{module_name} produced no rows on the parity fixture")

    projected_ids = {
        projection.factor_id
        for projection in default_projections()
        if projection.retired_module in (module_name, "KEPT")
    }
    actual = parity_warehouse.con.execute(
        "SELECT factor_id, security_id, as_of_date, raw_value FROM fundamental_factor_values "
        "WHERE source = ? ORDER BY 1, 2, 3",
        [PROJECTION_SOURCE],
    ).df()

    merged = expected[["factor_id", "security_id", "as_of_date", "raw_value"]].merge(
        actual, on=["factor_id", "security_id", "as_of_date"], suffixes=("_module", "_engine")
    )
    compared = merged[merged["factor_id"].isin(projected_ids)]
    assert not compared.empty, f"no overlapping rows for {module_name}"
    difference = (compared["raw_value_engine"] - compared["raw_value_module"]).abs()
    scale = compared["raw_value_module"].abs().clip(lower=1.0)
    assert (difference / scale).max() <= RELATIVE_TOLERANCE


@pytest.mark.parametrize("case", MODULE_CASES, ids=[case[0] for case in MODULE_CASES])
def test_projection_matches_module_value_where_the_cohort_matches(parity_warehouse, case):
    """The z-scored column is reproduced when the two grids select the same cohort."""
    module_name = case[0]
    expected = _module_rows(parity_warehouse, case)
    if expected.empty:
        pytest.skip(f"{module_name} produced no rows on the parity fixture")
    actual = parity_warehouse.con.execute(
        "SELECT factor_id, security_id, as_of_date, value FROM fundamental_factor_values "
        "WHERE source = ?",
        [PROJECTION_SOURCE],
    ).df()
    merged = expected[["factor_id", "security_id", "as_of_date", "value"]].merge(
        actual, on=["factor_id", "security_id", "as_of_date"], suffixes=("_module", "_engine")
    )
    if merged.empty:
        pytest.skip(f"{module_name}: no overlapping rebalance rows")
    module_sizes = expected.groupby(["factor_id", "as_of_date"]).size()
    merged_sizes = merged.groupby(["factor_id", "as_of_date"]).size()
    same_cohort = merged_sizes.index[merged_sizes.eq(module_sizes.reindex(merged_sizes.index))]
    aligned = merged.set_index(["factor_id", "as_of_date"]).loc[same_cohort]
    if aligned.empty:
        pytest.skip(f"{module_name}: no rebalance date where both grids select the same cohort")
    difference = (aligned["value_engine"] - aligned["value_module"]).abs()
    assert difference.max() <= 1e-9


def test_projection_output_has_the_canonical_fifteen_column_shape(parity_warehouse):
    from atx_db.derived_factor_projection import PROJECTION_OUTPUT_COLUMNS

    assert PROJECTION_OUTPUT_COLUMNS == (
        "factor_value_id",
        "factor_id",
        "factor_name",
        "family",
        "security_id",
        "symbol",
        "as_of_date",
        "raw_value",
        "value",
        "available_at",
        "input_ids_json",
        "input_lineage_json",
        "is_latest_revision",
        "run_id",
        "source",
    )


def test_projection_available_at_never_precedes_the_metric_availability(parity_warehouse):
    violations = parity_warehouse.con.execute(
        """
        SELECT count(*) FROM fundamental_factor_values f
        WHERE f.source = 'atx-db derived factor projection v1'
          AND f.available_at < CAST(f.as_of_date AS TIMESTAMP)
        """
    ).fetchone()[0]
    assert violations == 0
```

The `parity_facts` fixture belongs in this file too: a callable that writes one list of `(security_id, canonical_code, statement_metric, period_type, period_end, value, available_at, accession)` tuples into **both** `fundamental_standardized` (via the helper in `tests/test_derived_metrics.py`, imported or duplicated) and `fundamental_statement_points`, plus `equity_daily_bars` and `universe_membership` rows for 25 securities × 16 quarters so `minimum_names_per_date = 20` is satisfied. Build it by reading `fundamental_statement_points`' column list from `duckdb_columns()` once and inserting only the non-null-constrained columns.

- [ ] **Step 2: Run the test to verify it fails**

Run: `.venv\Scripts\python.exe -m pytest tests/test_derived_parity.py -n 0 -q`
Expected: FAIL — `ModuleNotFoundError: No module named 'atx_db.derived_factor_projection'`.

- [ ] **Step 3: Create the seed and `src/atx_db/derived_factor_projection.py`**

Write the CSV exactly as listed above, then:

```python
"""Project an engine metric onto the monthly factor-panel grid."""

from __future__ import annotations

import csv
import datetime as dt
import hashlib
from dataclasses import dataclass
from functools import lru_cache
from pathlib import Path
from typing import Any

import pandas as pd

from .connection import DuckDBStore
from .derived_registry import DERIVED_SOURCE_NAME
from .factors.cross_section import winsorize, zscore
from .market_daily import END_OF_DAY_HOURS, MARKET_DAILY_SOURCE_NAME
from .universe import DEFAULT_UNIVERSE_ID
from .warehouse import insert_frame, json_dumps

__all__ = [
    "PROJECTION_OUTPUT_COLUMNS",
    "PROJECTION_SEED_COLUMNS",
    "PROJECTION_SEED_PATH",
    "FactorProjection",
    "FactorProjectionOptions",
    "compute_projection_rows",
    "default_projections",
    "load_projection_inputs",
    "read_projection_seed",
    "refresh_projected_factor_values",
]

PROJECTION_SOURCE_NAME = "atx-db derived factor projection v1"
PROJECTION_SEED_PATH = Path(__file__).resolve().parent / "seeds" / "derived_factor_projections.csv"
PROJECTION_SEED_COLUMNS = (
    "factor_id",
    "metric_code",
    "source_window",
    "orientation",
    "factor_name",
    "family",
    "winsor_limit",
    "minimum_names_per_date",
    "retired_module",
)
PROJECTION_OUTPUT_COLUMNS = (
    "factor_value_id",
    "factor_id",
    "factor_name",
    "family",
    "security_id",
    "symbol",
    "as_of_date",
    "raw_value",
    "value",
    "available_at",
    "input_ids_json",
    "input_lineage_json",
    "is_latest_revision",
    "run_id",
    "source",
)


@dataclass(frozen=True)
class FactorProjection:
    factor_id: str
    metric_code: str
    source_window: str
    orientation: int
    factor_name: str
    family: str
    winsor_limit: float
    minimum_names_per_date: int
    retired_module: str


@dataclass(frozen=True)
class FactorProjectionOptions:
    source: str = PROJECTION_SOURCE_NAME
    derived_source: str = DERIVED_SOURCE_NAME
    market_source: str = MARKET_DAILY_SOURCE_NAME
    universe_id: str = DEFAULT_UNIVERSE_ID
    factor_ids: tuple[str, ...] | None = None
    start_date: dt.date | None = None
    end_date: dt.date | None = None
    run_id: str | None = None


def read_projection_seed(path: Path | str = PROJECTION_SEED_PATH) -> tuple[FactorProjection, ...]:
    seed_path = Path(path)
    rows: list[FactorProjection] = []
    with seed_path.open("r", encoding="utf-8", newline="") as handle:
        reader = csv.DictReader(handle)
        if tuple(reader.fieldnames or ()) != PROJECTION_SEED_COLUMNS:
            raise ValueError(f"{seed_path} header must be {PROJECTION_SEED_COLUMNS}")
        for raw in reader:
            orientation = int(raw["orientation"])
            if orientation not in (1, -1):
                raise ValueError(f"orientation must be 1 or -1, got {orientation}")
            if raw["source_window"] not in ("quarter", "daily"):
                raise ValueError(f"unknown source_window {raw['source_window']!r}")
            rows.append(
                FactorProjection(
                    factor_id=raw["factor_id"],
                    metric_code=raw["metric_code"],
                    source_window=raw["source_window"],
                    orientation=orientation,
                    factor_name=raw["factor_name"],
                    family=raw["family"],
                    winsor_limit=float(raw["winsor_limit"]),
                    minimum_names_per_date=int(raw["minimum_names_per_date"]),
                    retired_module=raw["retired_module"],
                )
            )
    return tuple(rows)


@lru_cache(maxsize=1)
def default_projections() -> tuple[FactorProjection, ...]:
    return read_projection_seed()


_GRID_SQL = """
WITH price_dedup AS (
    SELECT security_id, any_value(symbol) AS symbol, trade_date,
           max(available_at) AS price_available_at
    FROM equity_daily_bars
    WHERE close > 0 AND trade_date IS NOT NULL AND available_at IS NOT NULL
    GROUP BY security_id, trade_date
), price_months AS (
    SELECT *, row_number() OVER (PARTITION BY security_id, year(trade_date), month(trade_date)
                                 ORDER BY trade_date DESC) AS month_rank
    FROM price_dedup
), rebalances AS (
    SELECT * FROM price_months WHERE month_rank = 1 {date_predicate}
), governed AS (
    SELECT p.security_id, p.symbol, p.trade_date, p.price_available_at,
           u.available_at AS universe_available_at,
           row_number() OVER (PARTITION BY p.security_id, p.trade_date
                              ORDER BY u.valid_from DESC, u.available_at DESC NULLS LAST,
                                       u.source_loaded_at DESC, u.source DESC) AS universe_rank
    FROM rebalances p
    JOIN universe_membership u
      ON u.universe_id = ? AND u.security_id = p.security_id
     AND u.valid_from <= p.trade_date AND (u.valid_to IS NULL OR u.valid_to >= p.trade_date)
     AND u.as_of_date <= p.trade_date AND u.is_member AND u.is_latest_revision
     AND (u.available_at IS NULL OR u.available_at <= p.price_available_at)
), grid AS (
    SELECT * EXCLUDE (universe_rank),
           CAST(trade_date AS TIMESTAMP) + INTERVAL {end_of_day} HOUR AS cutoff
    FROM governed WHERE universe_rank = 1
)
"""


def load_projection_inputs(
    store: DuckDBStore,
    projection: FactorProjection,
    options: FactorProjectionOptions,
) -> pd.DataFrame:
    date_fragments: list[str] = []
    params: list[Any] = []
    if options.start_date is not None:
        date_fragments.append("AND trade_date >= ?")
        params.append(options.start_date)
    if options.end_date is not None:
        date_fragments.append("AND trade_date <= ?")
        params.append(options.end_date)
    prefix = _GRID_SQL.format(
        date_predicate=" ".join(date_fragments), end_of_day=END_OF_DAY_HOURS
    )
    params.append(options.universe_id)
    if projection.source_window == "quarter":
        sql = (
            prefix
            + """
            SELECT g.security_id, g.symbol, g.trade_date AS as_of_date,
                   d.value AS metric_value, d.available_at AS metric_available_at,
                   d.period_end,
                   greatest(g.price_available_at,
                            coalesce(g.universe_available_at, g.price_available_at),
                            d.available_at) AS decision_available_at
            FROM grid g
            ASOF JOIN (
                SELECT security_id, available_at, value, period_end
                FROM derived_metric_values
                WHERE source = ? AND metric_code = ?
            ) d ON d.security_id = g.security_id AND g.cutoff >= d.available_at
            ORDER BY g.trade_date, g.security_id
            """
        )
        params.extend([options.derived_source, projection.metric_code])
    else:
        sql = (
            prefix
            + f"""
            SELECT g.security_id, g.symbol, g.trade_date AS as_of_date,
                   m."{projection.metric_code}" AS metric_value,
                   m.available_at AS metric_available_at,
                   CAST(NULL AS DATE) AS period_end,
                   greatest(g.price_available_at,
                            coalesce(g.universe_available_at, g.price_available_at),
                            m.available_at) AS decision_available_at
            FROM grid g
            JOIN market_daily_metrics m
              ON m.security_id = g.security_id AND m.trade_date = g.trade_date
             AND m.source = ?
            ORDER BY g.trade_date, g.security_id
            """
        )
        params.append(options.market_source)
    return store.con.execute(sql, params).df()


def _factor_value_id(source: str, factor_id: str, security_id: str, as_of_date: Any) -> str:
    payload = "|".join(str(part) for part in (source, factor_id, security_id, as_of_date))
    return hashlib.sha256(payload.encode()).hexdigest()


def compute_projection_rows(
    inputs: pd.DataFrame,
    projection: FactorProjection,
    options: FactorProjectionOptions,
) -> pd.DataFrame:
    if inputs is None or inputs.empty:
        return pd.DataFrame(columns=list(PROJECTION_OUTPUT_COLUMNS))
    rows = inputs.copy()
    rows["as_of_date"] = pd.to_datetime(rows["as_of_date"], errors="coerce").dt.date
    rows["available_at"] = pd.to_datetime(rows["decision_available_at"], errors="coerce")
    rows["metric_value"] = pd.to_numeric(rows["metric_value"], errors="coerce")
    rows = rows.dropna(subset=["security_id", "as_of_date", "available_at", "metric_value"])
    rows = rows[rows["metric_value"].apply(lambda value: value == value and abs(value) != float("inf"))]
    counts = rows.groupby("as_of_date")["security_id"].transform("nunique")
    rows = rows[counts >= projection.minimum_names_per_date].copy()
    if rows.empty:
        return pd.DataFrame(columns=list(PROJECTION_OUTPUT_COLUMNS))
    rows["factor_id"] = projection.factor_id
    rows["factor_name"] = projection.factor_name
    rows["family"] = projection.family
    rows["raw_value"] = projection.orientation * rows["metric_value"]
    rows = winsorize(
        rows,
        value_column="raw_value",
        output_column="winsorized_value",
        partition_columns=("factor_id", "as_of_date"),
        limits=projection.winsor_limit,
    )
    rows = zscore(
        rows,
        value_column="winsorized_value",
        output_column="value",
        partition_columns=("factor_id", "as_of_date"),
    )
    rows["input_ids_json"] = json_dumps(
        [f"metric:{projection.metric_code}", f"universe:{options.universe_id}"]
    )
    rows["input_lineage_json"] = [
        json_dumps(
            {
                "method": "derived_metric_projection_v1",
                "metric_code": projection.metric_code,
                "source_window": projection.source_window,
                "orientation": projection.orientation,
                "winsor_limits": [projection.winsor_limit, projection.winsor_limit],
                "decision": {
                    "as_of_date": as_of_date,
                    "available_at": available_at,
                    "universe_id": options.universe_id,
                },
                "metric": {
                    "period_end": period_end,
                    "value": metric_value,
                    "available_at": metric_available_at,
                },
            }
        )
        for as_of_date, available_at, period_end, metric_value, metric_available_at in zip(
            rows["as_of_date"],
            rows["available_at"],
            rows["period_end"],
            rows["metric_value"],
            rows["metric_available_at"],
            strict=True,
        )
    ]
    rows["is_latest_revision"] = True
    rows["run_id"] = options.run_id
    rows["source"] = options.source
    rows["factor_value_id"] = [
        _factor_value_id(options.source, projection.factor_id, security_id, as_of_date)
        for security_id, as_of_date in zip(rows["security_id"], rows["as_of_date"], strict=True)
    ]
    return (
        rows[list(PROJECTION_OUTPUT_COLUMNS)]
        .dropna(subset=["value"])
        .sort_values(["as_of_date", "security_id"], kind="stable")
        .reset_index(drop=True)
    )


def refresh_projected_factor_values(
    store: DuckDBStore,
    options: FactorProjectionOptions | None = None,
) -> int:
    options = options or FactorProjectionOptions()
    store.initialize()
    wanted = set(options.factor_ids) if options.factor_ids is not None else None
    total = 0
    for projection in default_projections():
        if wanted is not None and projection.factor_id not in wanted:
            continue
        frame = compute_projection_rows(
            load_projection_inputs(store, projection, options), projection, options
        )
        with store.transaction():
            store.con.execute(
                "DELETE FROM fundamental_factor_values WHERE source = ? AND factor_id = ?",
                [options.source, projection.factor_id],
            )
            if not frame.empty:
                insert_frame(
                    store,
                    frame,
                    "fundamental_factor_values",
                    f"projection_{projection.factor_id}_insert",
                )
        total += len(frame)
    return total
```

- [ ] **Step 4: Add `derived_factor_projection` to the public API snapshot**

Insert `"derived_factor_projection"` into the `"atx_db"` array, before `"derived_dsl"`.

- [ ] **Step 5: Run the tests**

Run: `.venv\Scripts\python.exe -m pytest tests/test_derived_parity.py -n 0 -q`
Expected: `45 passed` (7 structural tests + 19 raw-value cases + 19 z-score cases), with `skipped` allowed only where a case prints `produced no rows on the parity fixture` or `no rebalance date where both grids select the same cohort`. **A skip in the raw-value test is a fixture defect, not a pass** — if more than three raw-value cases skip, widen the `parity_facts` fixture until they run.

Run: `.venv\Scripts\python.exe -m pytest tests/test_module_boundaries.py -n 0 -q`
Expected: `passed`.

- [ ] **Step 6: Commit**

```
git add src/atx_db/derived_factor_projection.py src/atx_db/seeds/derived_factor_projections.csv tests/test_derived_parity.py tests/data/public_api_snapshot.json
git commit -m "feat(db): add generic factor projection and the retirement parity harness

derived_factor_projection projects any engine metric onto the monthly
universe-gated rebalance grid and applies the same winsorize/zscore pair the
per-metric modules use, so a retired module keeps publishing its factor_id.
The parity harness compares the engine metric to each module's raw_value at
1e-9 relative on a fixture whose statement-points and standardized layers are
asserted identical before any comparison runs.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 8: Retire the covered per-metric modules

**Do not start this task until Task 7's `tests/test_derived_parity.py` passes with no raw-value skips.** The parity evidence is the licence to delete.

**Files:**
- Delete (18 modules): `src/atx_db/altman_distress.py`, `beneish_m_score.py`, `net_operating_assets.py`, `rsst_accruals.py`, `quarterly_working_capital_accruals.py`, `asset_turnover_change.py`, `annual_margin_change.py`, `quarterly_gross_margin_change.py`, `quarterly_profitability_change.py`, `external_financing.py`, `net_debt_financing.py`, `net_issuance.py`, `net_payout.py`, `enterprise_yield.py`, `rd_intensity.py`, `rd_increase.py`, `tax_expense_momentum.py`, `tax_to_book_income.py`
- Delete (14 wrappers): `scripts/build_altman_distress.py`, `build_beneish_m_score.py`, `build_net_operating_assets.py`, `build_rsst_accruals.py`, `build_quarterly_working_capital_accruals.py`, `build_quarterly_gross_margin_change.py`, `build_quarterly_profitability_change.py`, `build_external_financing.py`, `build_net_debt_financing.py`, `build_net_issuance.py`, `build_net_payout.py`, `build_rd_increase.py`, `build_tax_expense_momentum.py`, `build_tax_to_book_income.py`
- Delete (18 test files): `tests/test_altman_distress.py`, `test_beneish_m_score_factor.py`, `test_net_operating_assets.py`, `test_rsst_accruals.py`, `test_quarterly_working_capital_accruals.py`, `test_asset_turnover_change.py`, `test_annual_margin_change.py`, `test_quarterly_gross_margin_change.py`, `test_quarterly_profitability_change.py`, `test_external_financing.py`, `test_net_debt_financing.py`, `test_net_issuance.py`, `test_net_payout.py`, `test_enterprise_yield.py`, `test_rd_intensity.py`, `test_rd_increase.py`, `test_tax_expense_momentum.py`, `test_tax_to_book_income.py`
- Create: `src/atx_db/migrations/bodies_0303.py`
- Modify: `src/atx_db/migrations/registry.py` (2 lines)
- Modify: `tests/data/public_api_snapshot.json` (remove the 18 module names)
- Modify: `tests/test_derived_parity.py` (reduce `MODULE_CASES` to the kept modules, add the two retirement assertions below)

**Reference scan (run, not assumed).** A strict scan for `from .X import`, `from atx_db.X import`, `import atx_db.X` and `from ..X import` across every `.py` in the repo returns, for all 18 modules, only: their own test file, their `scripts/build_*.py` wrapper where one exists, and `src/atx_db/rsst_accruals.py` → `net_operating_assets` (both deleted together). **No migration body imports any of them** — `bodies_0199/0201/0209/0228/0234/0236-0239/0242-0246/0250-0252/0256` carry the factor ids and source names as *string literals* only, so applied migration history is unaffected. **No entry in `jobs.DATASET_REGISTRY`, `cli.py` or `orchestrator.py` references any of them** (audit §3.1c: "None of the 61 `refresh_*_values` functions appear in `jobs.py`, `cli.py`, or `orchestrator.py`").

**Kept (44 of the 62 modules), with the reason each one stays.** The audit counts 61 modules that write `fundamental_factor_values`; `annual_margin_change` is a 62nd with the same shape but no `refresh_*_values` entry point at all (it is in the deletion set).

| Reason | Modules |
| --- | --- |
| Imported by another module that stays (deleting it would need a cascade this sprint does not scope) | `asset_growth`, `cash_profitability`, `quarterly_cash_profitability`, `quarterly_gross_profitability`, `quarterly_operating_profitability`, `quarterly_revenue_growth`, `quarterly_roe`, `delta_roe`, `earnings_surprise`, `revenue_surprise`, `piotroski`, `fundamental_signals`, `cash_flow_profitability`, `profitability_trend`, `expected_growth`, `earnings_revenue_agreement`, `filing_reaction`, `quarterly_revenue_margin_confirmation` |
| Cross-sectional composite of two or more z-scored factors — not a per-security metric | `conditional_router`, `profitability_investment`, `cash_profitability_growth`, `qmj_profitability`, `continuous_financial_strength`, `twin_momentum` |
| Regression or trend estimate (OLS/WLS) the arithmetic DSL cannot express | `asset_turnover_trend`, `gross_margin_trend`, `operating_leverage`, `operating_cost_inflexibility`, `expected_growth_rolling`, `organization_capital` |
| Peer- or industry-relative "abnormal" construction | `abnormal_capex`, `abnormal_inventory_growth`, `abnormal_receivables_growth`, `quarterly_abnormal_inventory_growth`, `noa_proxy_turnover_change` |
| Earnings-event / surprise family with its own announcement clock | `earnings_acceleration`, `earnings_confirmation`, `earnings_persistence`, `earnings_revenue_confirmation`, `earnings_revenue_growth_agreement`, `earnings_seasonality`, `fundamental_momentum` |
| Needs a price-path input the quarterly engine does not carry | `inventory_volatility`, `quarterly_inventory_investment` |

**Net: 18 deleted, 44 kept.** `fundamental_factor_values` keeps every one of the 24 `factor_id`s the deleted modules published, now written by `derived_factor_projection`.

**Interfaces:**
- Consumes: `atx_db.derived_factor_projection.default_projections`, `refresh_projected_factor_values` (Task 7).
- Produces: `atx_db.migrations.bodies_0303.MIGRATIONS` — one `Migration(version=303, name="retire_per_metric_module_declarations", up=_retire_declarations)` that repoints `factor_definition.declared_in` for the 24 retired factor ids at `atx_db.derived_factor_projection`, so the catalog no longer names a module that does not exist.

- [ ] **Step 1: Write the failing test**

Append to `C:\atx\atx-db\tests\test_derived_parity.py`:

```python
RETIRED_MODULES = (
    "altman_distress",
    "beneish_m_score",
    "net_operating_assets",
    "rsst_accruals",
    "quarterly_working_capital_accruals",
    "asset_turnover_change",
    "annual_margin_change",
    "quarterly_gross_margin_change",
    "quarterly_profitability_change",
    "external_financing",
    "net_debt_financing",
    "net_issuance",
    "net_payout",
    "enterprise_yield",
    "rd_intensity",
    "rd_increase",
    "tax_expense_momentum",
    "tax_to_book_income",
)


@pytest.mark.parametrize("module_name", RETIRED_MODULES)
def test_retired_module_is_gone(module_name):
    with pytest.raises(ModuleNotFoundError):
        importlib.import_module(f"atx_db.{module_name}")


def test_retired_build_script_is_gone():
    from pathlib import Path

    scripts = Path(__file__).resolve().parents[1] / "scripts"
    for name in (
        "build_altman_distress.py",
        "build_beneish_m_score.py",
        "build_net_operating_assets.py",
        "build_rsst_accruals.py",
        "build_quarterly_working_capital_accruals.py",
        "build_quarterly_gross_margin_change.py",
        "build_quarterly_profitability_change.py",
        "build_external_financing.py",
        "build_net_debt_financing.py",
        "build_net_issuance.py",
        "build_net_payout.py",
        "build_rd_increase.py",
        "build_tax_expense_momentum.py",
        "build_tax_to_book_income.py",
    ):
        assert not (scripts / name).exists(), name


def test_every_retired_factor_id_is_still_published(parity_warehouse):
    published = {
        str(row[0])
        for row in parity_warehouse.con.execute(
            "SELECT DISTINCT factor_id FROM fundamental_factor_values WHERE source = ?",
            [PROJECTION_SOURCE],
        ).fetchall()
    }
    retired_ids = {
        projection.factor_id
        for projection in default_projections()
        if projection.retired_module in RETIRED_MODULES
    }
    assert retired_ids - published == set()


def test_factor_definitions_no_longer_name_a_deleted_module(tmp_store):
    stale = tmp_store.con.execute(
        """
        SELECT count(*) FROM factor_definition
        WHERE declared_in IN (
            'atx_db.altman_distress','atx_db.beneish_m_score','atx_db.net_operating_assets',
            'atx_db.rsst_accruals','atx_db.quarterly_working_capital_accruals',
            'atx_db.asset_turnover_change','atx_db.annual_margin_change',
            'atx_db.quarterly_gross_margin_change','atx_db.quarterly_profitability_change',
            'atx_db.external_financing','atx_db.net_debt_financing','atx_db.net_issuance',
            'atx_db.net_payout','atx_db.enterprise_yield','atx_db.rd_intensity',
            'atx_db.rd_increase','atx_db.tax_expense_momentum','atx_db.tax_to_book_income'
        )
        """
    ).fetchone()[0]
    assert stale == 0
```

Also reduce `MODULE_CASES` in the same file to the modules that survive — that is, drop every entry whose module is in `RETIRED_MODULES`, leaving exactly `("asset_growth", ...)`. The 18 deleted cases have already served their purpose; the passing run from Task 7 is the evidence and is quoted in this task's commit body.

- [ ] **Step 2: Run the test to verify it fails**

Run: `.venv\Scripts\python.exe -m pytest tests/test_derived_parity.py -n 0 -q`
Expected: FAIL — 18 × `Failed: DID NOT RAISE <class 'ModuleNotFoundError'>`, plus `test_retired_build_script_is_gone` failing on `build_altman_distress.py` and `test_factor_definitions_no_longer_name_a_deleted_module` failing with a non-zero count.

- [ ] **Step 3: Delete the modules, wrappers and tests**

```
git rm src/atx_db/altman_distress.py src/atx_db/beneish_m_score.py src/atx_db/net_operating_assets.py src/atx_db/rsst_accruals.py src/atx_db/quarterly_working_capital_accruals.py src/atx_db/asset_turnover_change.py src/atx_db/annual_margin_change.py src/atx_db/quarterly_gross_margin_change.py src/atx_db/quarterly_profitability_change.py src/atx_db/external_financing.py src/atx_db/net_debt_financing.py src/atx_db/net_issuance.py src/atx_db/net_payout.py src/atx_db/enterprise_yield.py src/atx_db/rd_intensity.py src/atx_db/rd_increase.py src/atx_db/tax_expense_momentum.py src/atx_db/tax_to_book_income.py
git rm scripts/build_altman_distress.py scripts/build_beneish_m_score.py scripts/build_net_operating_assets.py scripts/build_rsst_accruals.py scripts/build_quarterly_working_capital_accruals.py scripts/build_quarterly_gross_margin_change.py scripts/build_quarterly_profitability_change.py scripts/build_external_financing.py scripts/build_net_debt_financing.py scripts/build_net_issuance.py scripts/build_net_payout.py scripts/build_rd_increase.py scripts/build_tax_expense_momentum.py scripts/build_tax_to_book_income.py
git rm tests/test_altman_distress.py tests/test_beneish_m_score_factor.py tests/test_net_operating_assets.py tests/test_rsst_accruals.py tests/test_quarterly_working_capital_accruals.py tests/test_asset_turnover_change.py tests/test_annual_margin_change.py tests/test_quarterly_gross_margin_change.py tests/test_quarterly_profitability_change.py tests/test_external_financing.py tests/test_net_debt_financing.py tests/test_net_issuance.py tests/test_net_payout.py tests/test_enterprise_yield.py tests/test_rd_intensity.py tests/test_rd_increase.py tests/test_tax_expense_momentum.py tests/test_tax_to_book_income.py
```

- [ ] **Step 4: Create `src/atx_db/migrations/bodies_0303.py`**

```python
"""Repoint retired per-metric factor declarations at the derived-metric engine."""

from __future__ import annotations

import duckdb

from ._runner import Migration

RETIRED_DECLARATIONS = (
    "atx_db.altman_distress",
    "atx_db.beneish_m_score",
    "atx_db.net_operating_assets",
    "atx_db.rsst_accruals",
    "atx_db.quarterly_working_capital_accruals",
    "atx_db.asset_turnover_change",
    "atx_db.annual_margin_change",
    "atx_db.quarterly_gross_margin_change",
    "atx_db.quarterly_profitability_change",
    "atx_db.external_financing",
    "atx_db.net_debt_financing",
    "atx_db.net_issuance",
    "atx_db.net_payout",
    "atx_db.enterprise_yield",
    "atx_db.rd_intensity",
    "atx_db.rd_increase",
    "atx_db.tax_expense_momentum",
    "atx_db.tax_to_book_income",
)
REPLACEMENT = "atx_db.derived_factor_projection"


def _retire_declarations(conn: duckdb.DuckDBPyConnection) -> None:
    placeholders = ", ".join(["?"] * len(RETIRED_DECLARATIONS))
    conn.execute(
        f"UPDATE factor_definition SET declared_in = ? WHERE declared_in IN ({placeholders})",
        [REPLACEMENT, *RETIRED_DECLARATIONS],
    )


MIGRATIONS = [
    Migration(
        version=303,
        name="retire_per_metric_module_declarations",
        up=_retire_declarations,
    )
]
```

Register it in `registry.py` exactly as 0302 was: one import line after `bodies_0302`, one `*_MIGRATIONS_0303,` splat at the end of the list.

If `factor_definition.declared_in` stores a bare module name (`altman_distress`) rather than a dotted path, adjust `RETIRED_DECLARATIONS` to match what the 0199–0256 bodies actually insert — read one of them (`bodies_0209.py`) and copy the exact literal rather than guessing.

- [ ] **Step 5: Update `tests/data/public_api_snapshot.json`**

Remove exactly these 18 strings from the `"atx_db"` array: `altman_distress`, `annual_margin_change`, `asset_turnover_change`, `beneish_m_score`, `enterprise_yield`, `external_financing`, `net_debt_financing`, `net_issuance`, `net_operating_assets`, `net_payout`, `quarterly_gross_margin_change`, `quarterly_profitability_change`, `quarterly_working_capital_accruals`, `rd_increase`, `rd_intensity`, `rsst_accruals`, `tax_expense_momentum`, `tax_to_book_income`. Leave the array otherwise untouched and still sorted.

Regenerate rather than hand-edit if you prefer:

```
.venv\Scripts\python.exe -c "import json; from atx_db.module_boundaries import public_api_snapshot; json.dump(public_api_snapshot(), open('tests/data/public_api_snapshot.json','w'), indent=2, sort_keys=True); print('ok')"
```

Then diff the file and confirm the only changes are the 18 removals plus the additions this sprint already made.

- [ ] **Step 6: Run the tests**

Run: `.venv\Scripts\python.exe -m pytest tests/test_derived_parity.py -n 0 -q`
Expected: `passed` — 18 `test_retired_module_is_gone` cases, the two script/definition assertions, the retained `asset_growth` parity pair, and the structural tests.

Run: `.venv\Scripts\python.exe -m pytest tests/test_module_boundaries.py tests/test_import.py -n 0 -q`
Expected: `passed` — the snapshot matches `dir(atx_db)` exactly and no import cycle appeared.

Run: `.venv\Scripts\python.exe -m pytest tests/ -n 4 -q`
Expected: `passed` with the 18 deleted test files no longer collected. Any other failure means something still references a deleted module; fix the reference, do not restore the module.

- [ ] **Step 7: Commit**

```
git add -A
git commit -m "refactor(db): retire 18 per-metric factor modules covered by the engine

Deletes 18 leaf modules, 14 scripts/build_*.py wrappers and 18 test files whose
metrics the declarative catalog now owns, after tests/test_derived_parity.py
proved each module's raw_value to 1e-9 relative against the engine. Every one of
the 24 published factor_ids is still written, now by derived_factor_projection.
Migration 0303 repoints factor_definition.declared_in at the engine. 44 modules
stay: composites of z-scored factors, OLS/WLS trend estimates, peer-relative
abnormal constructions, the earnings-event family, and modules other survivors
still import.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 9: `panel_export.py` and the two new public schemas

**Files:**
- Create: `C:\atx\atx-db\src\atx_db\panel_export.py`
- Create: `C:\atx\atx-db\scripts\export_panels.py`
- Modify: `C:\atx\atx-db\src\atx_db\api\catalog.py`
- Modify: `C:\atx\atx-db\tests\data\public_api_snapshot.json` (add `"panel_export"`)
- Test: `C:\atx\atx-db\tests\test_panel_export.py` (new)

**Interfaces:**
- Consumes: `atx_db.connection.DuckDBStore`; `atx_db.lake._object_schema`, `atx_db.lake._schema_sha256` (existing private helpers — imported through a thin re-export, see note); `atx_db.derived_registry.DERIVED_SOURCE_NAME`; `atx_db.market_daily.MARKET_DAILY_SOURCE_NAME`; `atx_db.api.catalog.FieldSpec`, `RecordSchema`, `DatasetSpec`, `DATASETS`, `_PIT_FIELDS`, `public_schema`.
- Produces:
  - `atx_db.panel_export.PANEL_EXPORT_CONTRACT_VERSION: str = "1.0.0"`
  - `atx_db.panel_export.PanelExportResult` — frozen dataclass: `panel: str`, `parquet_path: Path`, `manifest_path: Path`, `row_count: int`, `column_count: int`, `query_sha256: str`, `schema_sha256: str`.
  - `atx_db.panel_export.export_panel_quarterly(store, as_of, items, metrics, out_dir) -> PanelExportResult`
  - `atx_db.panel_export.export_panel_daily_market(store, as_of, metrics, out_dir) -> PanelExportResult`
  - `atx_db.api.catalog.DERIVED_METRICS_SCHEMA: RecordSchema` (code `derived-metrics`, attached to `ATX.US.FUNDAMENTALS`)
  - `atx_db.api.catalog.MARKET_DAILY_SCHEMA: RecordSchema` (code `market-daily-1d`, attached to `ATX.US.EQUITIES`)

**Panel shapes.** Both are wide: one row per entity-period, one column per requested item/metric.
- `panel_quarterly(as_of, items, metrics)` — key `(security_id, period_end)`; a column per `item:` code pivoted from `fundamental_standardized` and per `metric_code` pivoted from `derived_metric_values`, each selected with `available_at <= as_of` and the greatest `available_at` per `(security_id, code, period_end)`. A `panel_available_at` column carries the row-level max.
- `panel_daily_market(as_of, metrics)` — key `(security_id, trade_date)`; the requested subset of `market_daily_metrics` columns, filtered `trade_date <= as_of AND available_at <= as_of + INTERVAL 22 HOUR`.

**Manifest** (JSON, written next to the Parquet, following the `lake.py` conventions — `_schema_sha256` over `duckdb_columns()` output, `sort_keys=True`, `separators=(",", ":")` for the hashed payload):

```json
{
  "panel": "panel_quarterly",
  "contract_version": "1.0.0",
  "as_of": "2024-06-30",
  "inputs": {"items": ["revenue"], "metrics": ["gross_margin"],
             "source_tables": ["fundamental_standardized", "derived_metric_values"],
             "derived_source": "atx-db declarative derived metrics v1"},
  "query_sha256": "<sha256 of the exact SQL text executed>",
  "schema_sha256": "<lake._schema_sha256 of the result columns>",
  "row_count": 1234,
  "column_count": 5,
  "parquet_path": "panel_quarterly_2024-06-30.parquet",
  "parquet_sha256": "<sha256 of the written file bytes>"
}
```

Note on the private-helper import: `_object_schema` and `_schema_sha256` are module-private in `lake.py`, and `module_boundaries.py` rejects cross-*package* private imports. `panel_export` and `lake` are in the same package (`atx_db`), so `from .lake import _object_schema, _schema_sha256` is allowed. Confirm by running `tests/test_module_boundaries.py` in Step 5; if it objects, promote both helpers in `lake.py` to `object_schema` / `schema_sha256` with the private names kept as aliases, and update `lake.py`'s internal call sites in the same commit.

- [ ] **Step 1: Write the failing test**

Create `C:\atx\atx-db\tests\test_panel_export.py`:

```python
"""Tier1-S3 T9: wide panel exports and their manifests."""

from __future__ import annotations

import datetime as dt
import json

import pyarrow.parquet as pq
import pytest

from atx_db.api.catalog import DATASETS, get_schema, public_schema
from atx_db.derived_metrics import DerivedMetricsOptions, refresh_derived_metrics
from atx_db.derived_registry import seed_derived_metric_definitions
from atx_db.market_daily import MarketDailyOptions, refresh_market_daily_metrics
from atx_db.panel_export import (
    PANEL_EXPORT_CONTRACT_VERSION,
    export_panel_daily_market,
    export_panel_quarterly,
)

AS_OF = dt.date(2021, 6, 30)


@pytest.fixture
def exported(tmp_store, derived_fixture, tmp_path):
    """``derived_fixture`` is the shared builder introduced in tests/test_derived_metrics.py."""
    seed_derived_metric_definitions(tmp_store)
    derived_fixture(tmp_store)
    refresh_derived_metrics(tmp_store, DerivedMetricsOptions())
    refresh_market_daily_metrics(tmp_store, MarketDailyOptions())
    return tmp_store, tmp_path


def test_quarterly_panel_writes_parquet_and_manifest(exported):
    store, out_dir = exported
    result = export_panel_quarterly(
        store, AS_OF, items=("revenue", "total_assets"), metrics=("gross_margin",), out_dir=out_dir
    )
    assert result.parquet_path.exists()
    assert result.manifest_path.exists()
    table = pq.read_table(result.parquet_path)
    assert set(table.column_names) >= {
        "security_id", "period_end", "revenue", "total_assets", "gross_margin", "panel_available_at"
    }
    assert table.num_rows == result.row_count


def test_quarterly_manifest_has_the_documented_keys(exported):
    store, out_dir = exported
    result = export_panel_quarterly(
        store, AS_OF, items=("revenue",), metrics=("gross_margin",), out_dir=out_dir
    )
    manifest = json.loads(result.manifest_path.read_text(encoding="utf-8"))
    assert set(manifest) == {
        "panel", "contract_version", "as_of", "inputs", "query_sha256",
        "schema_sha256", "row_count", "column_count", "parquet_path", "parquet_sha256",
    }
    assert manifest["panel"] == "panel_quarterly"
    assert manifest["contract_version"] == PANEL_EXPORT_CONTRACT_VERSION
    assert manifest["as_of"] == AS_OF.isoformat()
    assert manifest["inputs"]["items"] == ["revenue"]
    assert manifest["inputs"]["metrics"] == ["gross_margin"]
    assert len(manifest["query_sha256"]) == 64
    assert len(manifest["schema_sha256"]) == 64
    assert len(manifest["parquet_sha256"]) == 64


def test_repeated_export_is_byte_identical(exported):
    store, out_dir = exported
    first = export_panel_quarterly(store, AS_OF, items=("revenue",), metrics=(), out_dir=out_dir)
    first_bytes = first.parquet_path.read_bytes()
    first_manifest = json.loads(first.manifest_path.read_text(encoding="utf-8"))
    second = export_panel_quarterly(store, AS_OF, items=("revenue",), metrics=(), out_dir=out_dir)
    second_manifest = json.loads(second.manifest_path.read_text(encoding="utf-8"))
    assert second.parquet_path.read_bytes() == first_bytes
    assert second_manifest == first_manifest


def test_query_hash_changes_with_the_requested_columns(exported):
    store, out_dir = exported
    one = export_panel_quarterly(store, AS_OF, items=("revenue",), metrics=(), out_dir=out_dir)
    two = export_panel_quarterly(
        store, AS_OF, items=("revenue", "total_assets"), metrics=(), out_dir=out_dir
    )
    assert one.query_sha256 != two.query_sha256


def test_quarterly_panel_never_exposes_a_row_available_after_the_as_of(exported):
    store, out_dir = exported
    result = export_panel_quarterly(store, AS_OF, items=("revenue",), metrics=(), out_dir=out_dir)
    table = pq.read_table(result.parquet_path).to_pandas()
    if not table.empty:
        assert table["panel_available_at"].max() <= dt.datetime.combine(AS_OF, dt.time(23, 59, 59))


def test_daily_market_panel_exports_the_requested_metrics(exported):
    store, out_dir = exported
    result = export_panel_daily_market(
        store, AS_OF, metrics=("market_cap", "pe_ttm", "total_return_1m"), out_dir=out_dir
    )
    table = pq.read_table(result.parquet_path)
    assert set(table.column_names) >= {
        "security_id", "trade_date", "market_cap", "pe_ttm", "total_return_1m", "available_at"
    }
    manifest = json.loads(result.manifest_path.read_text(encoding="utf-8"))
    assert manifest["panel"] == "panel_daily_market"
    assert manifest["inputs"]["metrics"] == ["market_cap", "pe_ttm", "total_return_1m"]


def test_daily_market_panel_rejects_an_unknown_metric(exported):
    store, out_dir = exported
    with pytest.raises(ValueError) as excinfo:
        export_panel_daily_market(store, AS_OF, metrics=("not_a_metric",), out_dir=out_dir)
    assert "not_a_metric" in str(excinfo.value)


def test_quarterly_panel_rejects_an_unknown_item(exported):
    store, out_dir = exported
    with pytest.raises(ValueError):
        export_panel_quarterly(store, AS_OF, items=("nope",), metrics=(), out_dir=out_dir)


def test_derived_metrics_schema_is_registered():
    schema = get_schema("ATX.US.FUNDAMENTALS", "derived-metrics")
    assert schema.source_table == "derived_metric_values"
    assert schema.time_column == "period_end"
    assert schema.item_column == "metric_code"
    assert schema.natural_key == ("security_id", "metric_code", "metric_window", "period_end")
    assert "inputs_hash" in schema.field_names
    payload = public_schema(schema)
    assert len(payload["schema_sha256"]) == 64


def test_market_daily_schema_is_registered():
    schema = get_schema("ATX.US.EQUITIES", "market-daily-1d")
    assert schema.source_table == "market_daily_metrics"
    assert schema.time_column == "trade_date"
    assert schema.natural_key == ("security_id", "trade_date")
    for name in ("market_cap", "enterprise_value", "pe_ttm", "shares_source", "momentum_12_1"):
        assert name in schema.field_names


def test_catalog_still_exposes_both_datasets():
    codes = {dataset.code for dataset in DATASETS}
    assert codes == {"ATX.US.FUNDAMENTALS", "ATX.US.EQUITIES"}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `.venv\Scripts\python.exe -m pytest tests/test_panel_export.py -n 0 -q`
Expected: FAIL — `ModuleNotFoundError: No module named 'atx_db.panel_export'` and `KeyError: 'derived-metrics'`.

- [ ] **Step 3: Create `src/atx_db/panel_export.py`**

```python
"""Wide point-in-time panel exports with reproducible manifests."""

from __future__ import annotations

import datetime as dt
import hashlib
import json
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Sequence

from .connection import DuckDBStore
from .derived_registry import DERIVED_SOURCE_NAME, default_derived_definitions, known_item_codes
from .lake import _object_schema, _schema_sha256
from .market_daily import END_OF_DAY_HOURS, MARKET_DAILY_SOURCE_NAME

__all__ = [
    "PANEL_EXPORT_CONTRACT_VERSION",
    "PanelExportResult",
    "export_panel_daily_market",
    "export_panel_quarterly",
]

PANEL_EXPORT_CONTRACT_VERSION = "1.0.0"


@dataclass(frozen=True)
class PanelExportResult:
    panel: str
    parquet_path: Path
    manifest_path: Path
    row_count: int
    column_count: int
    query_sha256: str
    schema_sha256: str


def _sha256_text(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def _sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _quote(value: str) -> str:
    return "'" + value.replace("'", "''") + "'"


def _identifier(value: str) -> str:
    if not value.replace("_", "").isalnum():
        raise ValueError(f"unsupported panel column name {value!r}")
    return f'"{value}"'


def _write(
    store: DuckDBStore,
    *,
    panel: str,
    sql: str,
    params: Sequence[object],
    inputs: dict[str, object],
    as_of: dt.date,
    out_dir: Path,
) -> PanelExportResult:
    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    parquet_path = out_dir / f"{panel}_{as_of.isoformat()}.parquet"
    manifest_path = out_dir / f"{panel}_{as_of.isoformat()}.manifest.json"
    relation = f"_panel_{panel}"
    store.con.execute(f"CREATE OR REPLACE TEMP VIEW {relation} AS {sql}", list(params))
    row_count = int(store.con.execute(f"SELECT count(*) FROM {relation}").fetchone()[0])
    schema = _object_schema(store, relation)
    store.con.execute(
        f"COPY (SELECT * FROM {relation}) TO '{parquet_path.as_posix()}' "
        "(FORMAT PARQUET, COMPRESSION ZSTD)"
    )
    manifest = {
        "panel": panel,
        "contract_version": PANEL_EXPORT_CONTRACT_VERSION,
        "as_of": as_of.isoformat(),
        "inputs": inputs,
        "query_sha256": _sha256_text(sql),
        "schema_sha256": _schema_sha256(schema),
        "row_count": row_count,
        "column_count": len(schema),
        "parquet_path": parquet_path.name,
        "parquet_sha256": _sha256_file(parquet_path),
    }
    manifest_path.write_text(
        json.dumps(manifest, sort_keys=True, indent=2) + "\n", encoding="utf-8"
    )
    store.con.execute(f"DROP VIEW IF EXISTS {relation}")
    return PanelExportResult(
        panel=panel,
        parquet_path=parquet_path,
        manifest_path=manifest_path,
        row_count=row_count,
        column_count=len(schema),
        query_sha256=str(manifest["query_sha256"]),
        schema_sha256=str(manifest["schema_sha256"]),
    )


def export_panel_quarterly(
    store: DuckDBStore,
    as_of: dt.date,
    items: Iterable[str],
    metrics: Iterable[str],
    out_dir: Path | str,
    *,
    derived_source: str = DERIVED_SOURCE_NAME,
) -> PanelExportResult:
    item_codes = tuple(dict.fromkeys(items))
    metric_codes = tuple(dict.fromkeys(metrics))
    unknown_items = sorted(set(item_codes) - known_item_codes())
    if unknown_items:
        raise ValueError(f"unknown fundamental item codes: {unknown_items}")
    catalog = {definition.metric_code for definition in default_derived_definitions()}
    unknown_metrics = sorted(set(metric_codes) - catalog)
    if unknown_metrics:
        raise ValueError(f"unknown derived metric codes: {unknown_metrics}")

    item_columns = "".join(
        f",\n           max(CASE WHEN u.code = {_quote(code)} THEN u.value END) AS {_identifier(code)}"
        for code in item_codes
    )
    metric_columns = "".join(
        f",\n           max(CASE WHEN u.code = {_quote(code)} THEN u.value END) AS {_identifier(code)}"
        for code in metric_codes
    )
    sql = f"""
    WITH unioned AS (
        SELECT security_id, canonical_code AS code, period_end, value, available_at
        FROM fundamental_standardized
        WHERE is_latest_revision AND available_at <= ?
          AND canonical_code IN ({", ".join(_quote(code) for code in item_codes) or "''"})
        UNION ALL
        SELECT security_id, metric_code AS code, period_end, value, available_at
        FROM derived_metric_values
        WHERE source = ? AND available_at <= ?
          AND metric_code IN ({", ".join(_quote(code) for code in metric_codes) or "''"})
    ), picked AS (
        SELECT security_id, code, period_end,
               arg_max(value, available_at) AS value,
               max(available_at) AS available_at
        FROM unioned
        GROUP BY 1, 2, 3
    )
    SELECT u.security_id, u.period_end{item_columns}{metric_columns},
           max(u.available_at) AS panel_available_at
    FROM picked u
    GROUP BY u.security_id, u.period_end
    ORDER BY u.security_id, u.period_end
    """
    cutoff = dt.datetime.combine(as_of, dt.time(23, 59, 59))
    return _write(
        store,
        panel="panel_quarterly",
        sql=sql,
        params=[cutoff, derived_source, cutoff],
        inputs={
            "items": list(item_codes),
            "metrics": list(metric_codes),
            "source_tables": ["fundamental_standardized", "derived_metric_values"],
            "derived_source": derived_source,
        },
        as_of=as_of,
        out_dir=Path(out_dir),
    )


def export_panel_daily_market(
    store: DuckDBStore,
    as_of: dt.date,
    metrics: Iterable[str],
    out_dir: Path | str,
    *,
    market_source: str = MARKET_DAILY_SOURCE_NAME,
) -> PanelExportResult:
    metric_codes = tuple(dict.fromkeys(metrics))
    available = {
        str(row[0])
        for row in store.con.execute(
            "SELECT column_name FROM duckdb_columns() "
            "WHERE schema_name = 'main' AND table_name = 'market_daily_metrics'"
        ).fetchall()
    }
    unknown = sorted(set(metric_codes) - available)
    if unknown:
        raise ValueError(f"unknown market_daily_metrics columns: {unknown}")
    projection = "".join(f", m.{_identifier(code)}" for code in metric_codes)
    sql = f"""
    SELECT m.security_id, m.trade_date, m.symbol, m.close, m.adj_close,
           m.shares_outstanding, m.shares_source{projection},
           m.available_at, m.inputs_hash
    FROM market_daily_metrics m
    WHERE m.source = ? AND m.is_latest_revision
      AND m.trade_date <= ?
      AND m.available_at <= CAST(? AS TIMESTAMP) + INTERVAL {END_OF_DAY_HOURS} HOUR
    ORDER BY m.security_id, m.trade_date
    """
    return _write(
        store,
        panel="panel_daily_market",
        sql=sql,
        params=[market_source, as_of, as_of],
        inputs={
            "metrics": list(metric_codes),
            "source_tables": ["market_daily_metrics"],
            "market_source": market_source,
        },
        as_of=as_of,
        out_dir=Path(out_dir),
    )
```

- [ ] **Step 4: Register the two schemas in `src/atx_db/api/catalog.py`**

Insert after `DAILY_BARS_SCHEMA` (line 574) and before `DATASETS` (line 611):

```python
DERIVED_METRICS_SCHEMA = RecordSchema(
    dataset="ATX.US.FUNDAMENTALS",
    code="derived-metrics",
    version="1.0.0",
    title="Point-in-time derived metrics",
    description=(
        "Ratios, per-share, growth, leverage, quality, investment and payout metrics computed "
        "by one declarative engine from the standardized fundamentals layer."
    ),
    source_table="derived_metric_values",
    time_column="period_end",
    natural_key=("security_id", "metric_code", "metric_window", "period_end"),
    item_column="metric_code",
    basis_column="metric_window",
    fields=(
        FieldSpec("security_id", "security_id", "string", "Stable ATX security identifier.", nullable=False),
        FieldSpec("metric", "metric_code", "string", "Derived metric code.", nullable=False, filterable=True),
        FieldSpec(
            "window", "metric_window", "string",
            "Metric window: q, ttm, annual, instant, avg2 or daily.", nullable=False, filterable=True,
        ),
        FieldSpec("period_end", "period_end", "date", "Fiscal period end.", nullable=False),
        FieldSpec("value", "value", "float64", "Derived metric value.", nullable=False),
        FieldSpec(
            "inputs_hash", "inputs_hash", "string",
            "SHA-256 over the sorted (code, period_end, revision_sequence, value) inputs consumed.",
            nullable=False,
        ),
        FieldSpec("source", "source", "string", "ATX engine identifier.", nullable=False),
        *_PIT_FIELDS,
    ),
)


MARKET_DAILY_SCHEMA = RecordSchema(
    dataset="ATX.US.EQUITIES",
    code="market-daily-1d",
    version="1.0.0",
    title="Daily market and valuation panel",
    description=(
        "Daily prices, point-in-time shares, market cap, enterprise value, valuation multiples, "
        "total returns, momentum and realized volatility."
    ),
    source_table="market_daily_metrics",
    time_column="trade_date",
    natural_key=("security_id", "trade_date"),
    supports_vintages=False,
    fields=(
        FieldSpec("security_id", "security_id", "string", "Stable ATX security identifier.", nullable=False),
        FieldSpec("symbol", "symbol", "string", "Ticker for the observation."),
        FieldSpec("trade_date", "trade_date", "date", "Exchange trading date.", nullable=False),
        FieldSpec("close", "close", "float64", "Unadjusted closing price.", "USD"),
        FieldSpec("adj_close", "adj_close", "float64", "Corporate-action adjusted close.", "USD"),
        FieldSpec("volume", "volume", "int64", "Share volume.", "shares"),
        FieldSpec("shares_outstanding", "shares_outstanding", "float64", "Point-in-time shares.", "shares"),
        FieldSpec("shares_source", "shares_source", "string", "dei or archive.", filterable=True),
        FieldSpec(
            "shares_reconciliation_ratio", "shares_reconciliation_ratio", "float64",
            "dei shares divided by archive shares on the same date.",
        ),
        *(
            FieldSpec(name, name, "float64", description, unit)
            for name, description, unit in (
                ("market_cap", "Price times point-in-time shares outstanding.", "USD"),
                ("enterprise_value", "Market cap plus debt, preferred and minority less cash.", "USD"),
                ("pe_ttm", "Price to trailing earnings available to common.", None),
                ("pb", "Price to common book value.", None),
                ("ps_ttm", "Price to trailing revenue.", None),
                ("pcf_ttm", "Price to trailing operating cash flow.", None),
                ("ev_ebitda", "Enterprise value to trailing EBITDA.", None),
                ("ev_sales", "Enterprise value to trailing revenue.", None),
                ("fcf_yield", "Trailing free cash flow over market cap.", None),
                ("dividend_yield", "Trailing common dividends over market cap.", None),
                ("earnings_yield", "Trailing earnings available to common over market cap.", None),
                ("shareholder_yield", "Net payout plus net debt paydown over market cap.", None),
                ("net_payout_yield", "Dividends plus net buybacks over market cap.", None),
                ("total_payout_yield", "Gross dividends plus gross buybacks over market cap.", None),
                ("buyback_yield", "Net buybacks over market cap.", None),
                ("book_to_market", "Common book value over market cap.", None),
                ("rd_to_market_equity", "Trailing R&D expense over market cap.", None),
                ("gross_profit_to_ev", "Trailing gross profit over enterprise value.", None),
                ("cfo_to_ev", "Trailing operating cash flow over enterprise value.", None),
                ("ebit_to_ev", "Trailing operating income over enterprise value.", None),
                ("sales_to_ev", "Trailing revenue over enterprise value.", None),
                ("altman_z", "Altman Z-score with market equity.", None),
                ("total_return_1m", "Twenty-one-trading-day total return.", None),
                ("total_return_3m", "Sixty-three-trading-day total return.", None),
                ("total_return_6m", "One-hundred-twenty-six-trading-day total return.", None),
                ("total_return_12m", "Two-hundred-fifty-two-trading-day total return.", None),
                ("momentum_12_1", "Twelve-month total return skipping the last month.", None),
                ("realized_vol_60d", "Annualized sixty-day realized volatility.", None),
                ("realized_vol_252d", "Annualized two-hundred-fifty-two-day realized volatility.", None),
                ("dollar_volume_20d", "Twenty-day average daily dollar volume.", "USD"),
            )
        ),
        FieldSpec(
            "fundamental_available_at", "fundamental_available_at", "timestamp",
            "Max availability of the fundamentals joined into this row.",
        ),
        FieldSpec(
            "inputs_hash", "inputs_hash", "string",
            "SHA-256 over the price, share and fundamental-availability inputs.", nullable=False,
        ),
        FieldSpec("source", "source", "string", "ATX engine identifier.", nullable=False),
        *_PIT_FIELDS,
    ),
)
```

Then add `DERIVED_METRICS_SCHEMA` to the `ATX.US.FUNDAMENTALS` `schemas` tuple (after `RESTATEMENTS_SCHEMA`) and `MARKET_DAILY_SCHEMA` to the `ATX.US.EQUITIES` `schemas` tuple (after `DAILY_BARS_SCHEMA`).

- [ ] **Step 5: Create `scripts/export_panels.py`**

argparse over `--db-path`, `--as-of` (`dt.date.fromisoformat`, required), `--out-dir` (required), `--item` (repeatable), `--metric` (repeatable), `--panel` (`quarterly` | `daily` | `both`, default `both`); calls the two exporters and prints one JSON line per panel with the `PanelExportResult` fields (`default=str`).

- [ ] **Step 6: Run the tests**

Run: `.venv\Scripts\python.exe -m pytest tests/test_panel_export.py -n 0 -q`
Expected: `11 passed`.

Run: `.venv\Scripts\python.exe -m pytest tests/test_api_service.py tests/test_restatement_events.py tests/test_schema_contract_v2.py tests/test_module_boundaries.py -n 0 -q --run-slow`
Expected: `passed`. If a test pins the number of schemas per dataset or a catalog SHA, update the pinned value in this commit and justify it in the body — the two additions are intentional.

- [ ] **Step 7: Commit**

```
git add src/atx_db/panel_export.py scripts/export_panels.py src/atx_db/api/catalog.py tests/test_panel_export.py tests/data/public_api_snapshot.json
git commit -m "feat(db): add wide panel exports and the derived/market public schemas

export_panel_quarterly and export_panel_daily_market write Parquet plus a JSON
manifest carrying the query hash, the lake-convention schema sha256, the file
sha256, the row and column counts and the exact inputs, so an export is
reproducible and attributable. api/catalog gains derived-metrics on
ATX.US.FUNDAMENTALS and market-daily-1d on ATX.US.EQUITIES.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 10: Wire the engine into `jobs.DATASET_REGISTRY` and the activation ladder

**Depends on Sprint 1 Task 5/7 having landed** — `atx_db.activation` currently ships only the ledger half (`STAGE_ORDER`, `StageResult`, `select_stages`, `begin_stage`, `finish_stage`, `completed_stages`, verified by reading `src/atx_db/activation.py`). `STAGES`, `ActivationOptions` and `run_activation` arrive with S1 Tasks 5–7. If they are not present when this task starts, do Steps 1–4 (the `jobs.py` half) and hold Steps 5–7 until they are; do not invent them here.

**Files:**
- Modify: `C:\atx\atx-db\src\atx_db\jobs.py`
- Modify: `C:\atx\atx-db\src\atx_db\activation.py`
- Test: `C:\atx\atx-db\tests\test_derived_wiring.py` (new)

**Interfaces:**
- Consumes: `atx_db.derived_metrics.DerivedMetricsDataset`, `DerivedMetricsOptions` (Task 5); `atx_db.market_daily.MarketDailyDataset`, `MarketDailyOptions`, `shares_reconciliation_report` (Task 6); `atx_db.derived_registry.seed_derived_metric_definitions` (Task 3); `atx_db.activation.StageResult`, `STAGE_ORDER`, `STAGES`, `ActivationOptions` (Sprint 1).
- Produces:
  - `atx_db.jobs._derived_metrics_options(params: dict[str, Any]) -> DerivedMetricsOptions`
  - `atx_db.jobs._market_daily_options(params: dict[str, Any]) -> MarketDailyOptions`
  - `DATASET_REGISTRY["derived_metrics"]`, `DATASET_REGISTRY["market_daily"]`
  - `DATASET_DEPENDENCIES["derived_metrics"] = ("fundamental_standardized",)`
  - `DATASET_DEPENDENCIES["market_daily"] = ("derived_metrics", "tbltickerhistory_daily", "shares_outstanding_history")`
  - `atx_db.activation.stage_derived_metrics(store, options) -> StageResult`
  - `atx_db.activation.stage_market_daily(store, options) -> StageResult`
  - `STAGE_ORDER` extended with `"derived_metrics"` and `"market_daily"`, in that order, appended after `"reconciliation"` and before `"provider_coverage"`.

- [ ] **Step 1: Write the failing test**

Create `C:\atx\atx-db\tests\test_derived_wiring.py`:

```python
"""Tier1-S3 T10: the derived engine is a first-class dataset and activation stage."""

from __future__ import annotations

import inspect

import pytest

from atx_db.jobs import DATASET_DEPENDENCIES, DATASET_REGISTRY


def test_both_datasets_are_registered():
    assert "derived_metrics" in DATASET_REGISTRY
    assert "market_daily" in DATASET_REGISTRY


def test_dataset_classes_expose_the_expected_identity():
    from atx_db.derived_metrics import DerivedMetricsDataset
    from atx_db.market_daily import MarketDailyDataset

    derived_cls, _derived_factory = DATASET_REGISTRY["derived_metrics"]
    market_cls, _market_factory = DATASET_REGISTRY["market_daily"]
    assert derived_cls is DerivedMetricsDataset
    assert market_cls is MarketDailyDataset
    assert derived_cls.dataset_id == "derived_metrics"
    assert market_cls.dataset_id == "market_daily"


def test_option_factories_accept_a_plain_params_dict():
    _derived_cls, derived_factory = DATASET_REGISTRY["derived_metrics"]
    _market_cls, market_factory = DATASET_REGISTRY["market_daily"]
    derived = derived_factory({"metric_codes": ["revenue_ttm"], "batch_size": 7})
    assert derived.metric_codes == ("revenue_ttm",)
    assert derived.batch_size == 7
    market = market_factory({"start_date": "2020-01-02", "batch_size": 3})
    assert market.batch_size == 3
    assert market.start_date is not None
    assert market_factory({}).start_date is None


def test_dependency_edges_are_declared_and_resolvable():
    assert DATASET_DEPENDENCIES["derived_metrics"] == ("fundamental_standardized",)
    assert DATASET_DEPENDENCIES["market_daily"] == (
        "derived_metrics",
        "tbltickerhistory_daily",
        "shares_outstanding_history",
    )
    for dataset_id, dependencies in DATASET_DEPENDENCIES.items():
        for dependency in dependencies:
            assert dependency in DATASET_REGISTRY, f"{dataset_id} -> {dependency}"


def test_depends_on_is_applied_to_the_classes():
    derived_cls, _ = DATASET_REGISTRY["derived_metrics"]
    market_cls, _ = DATASET_REGISTRY["market_daily"]
    assert derived_cls.depends_on == ("fundamental_standardized",)
    assert "derived_metrics" in market_cls.depends_on


def test_activation_stage_order_places_the_engine_after_reconciliation():
    from atx_db.activation import STAGE_ORDER

    assert "derived_metrics" in STAGE_ORDER
    assert "market_daily" in STAGE_ORDER
    assert STAGE_ORDER.index("standardized") < STAGE_ORDER.index("derived_metrics")
    assert STAGE_ORDER.index("reconciliation") < STAGE_ORDER.index("derived_metrics")
    assert STAGE_ORDER.index("derived_metrics") < STAGE_ORDER.index("market_daily")
    assert STAGE_ORDER.index("market_daily") < STAGE_ORDER.index("provider_coverage")
    assert STAGE_ORDER.index("ticker_history_publish") < STAGE_ORDER.index("market_daily")


def test_both_stages_are_registered_with_the_uniform_signature():
    from atx_db.activation import STAGES, StageResult

    for name in ("derived_metrics", "market_daily"):
        function = STAGES[name]
        parameters = list(inspect.signature(function).parameters)
        assert parameters == ["store", "options"], f"{name} has {parameters}"
        assert inspect.signature(function).return_annotation in (StageResult, "StageResult")


def test_stage_order_and_stages_stay_in_lockstep():
    from atx_db.activation import STAGE_ORDER, STAGES

    assert set(STAGES) <= set(STAGE_ORDER)


def test_derived_stage_seeds_definitions_and_returns_a_row_count(tmp_store, derived_fixture):
    from atx_db.activation import ActivationOptions, stage_derived_metrics

    derived_fixture(tmp_store)
    result = stage_derived_metrics(tmp_store, ActivationOptions(db_path=tmp_store.path))
    assert result.rows > 0
    assert result.detail["definitions_seeded"] > 0
    assert result.detail["metric_count"] > 0
    seeded = tmp_store.con.execute(
        "SELECT count(*) FROM derived_metric_definitions"
    ).fetchone()[0]
    assert seeded == result.detail["definitions_seeded"]


def test_market_stage_reports_the_shares_reconciliation(tmp_store, derived_fixture):
    from atx_db.activation import ActivationOptions, stage_derived_metrics, stage_market_daily

    derived_fixture(tmp_store)
    options = ActivationOptions(db_path=tmp_store.path)
    stage_derived_metrics(tmp_store, options)
    result = stage_market_daily(tmp_store, options)
    assert result.rows >= 0
    assert "pass_rate" in result.detail
    assert "meets_spec_gate" in result.detail


def test_stage_payloads_are_json_serialisable(tmp_store, derived_fixture):
    import json

    from atx_db.activation import ActivationOptions, stage_derived_metrics, stage_market_daily

    derived_fixture(tmp_store)
    options = ActivationOptions(db_path=tmp_store.path)
    for stage in (stage_derived_metrics, stage_market_daily):
        json.dumps(stage(tmp_store, options).detail, default=str)
```

`derived_fixture` is the shared builder introduced in `tests/test_derived_metrics.py`; move it into `tests/conftest.py` as a plain `@pytest.fixture` returning a callable in this task so all four test modules share one copy, and delete the local duplicates. `tmp_store.path` is the `DuckDBStore` attribute holding the database file; if the attribute is named differently, read `connection.py` and use the real name rather than adding a property.

- [ ] **Step 2: Run the test to verify it fails**

Run: `.venv\Scripts\python.exe -m pytest tests/test_derived_wiring.py -n 0 -q`
Expected: FAIL — `KeyError: 'derived_metrics'` from `DATASET_REGISTRY` and `AssertionError: assert 'derived_metrics' in STAGE_ORDER`.

- [ ] **Step 3: Register the datasets in `src/atx_db/jobs.py`**

Add the imports alongside the other dataset imports at the top of the module:

```python
from .derived_metrics import DerivedMetricsDataset, DerivedMetricsOptions
from .market_daily import MarketDailyDataset, MarketDailyOptions
```

Add the two option factories next to `_fred_macro_options` (just before `DATASET_REGISTRY`):

```python
def _derived_metrics_options(params: dict[str, Any]) -> DerivedMetricsOptions:
    default = DerivedMetricsOptions()
    return DerivedMetricsOptions(
        source=params.get("source") or default.source,
        security_ids=_tuple_or_none(params.get("security_ids")),
        metric_codes=_tuple_or_none(params.get("metric_codes")),
        batch_size=int(params.get("batch_size", default.batch_size)),
        run_id=params.get("run_id") or default.run_id,
    )


def _market_daily_options(params: dict[str, Any]) -> MarketDailyOptions:
    default = MarketDailyOptions()
    return MarketDailyOptions(
        source=params.get("source") or default.source,
        derived_source=params.get("derived_source") or default.derived_source,
        bar_source=params.get("bar_source") or default.bar_source,
        start_date=_date_or_none(params.get("start_date")),
        end_date=_date_or_none(params.get("end_date")),
        security_ids=_tuple_or_none(params.get("security_ids")),
        batch_size=int(params.get("batch_size", default.batch_size)),
        shares_tolerance=float(params.get("shares_tolerance", default.shares_tolerance)),
        run_id=params.get("run_id") or default.run_id,
    )
```

`_tuple_or_none` and `_date_or_none` already exist in `jobs.py` and are used by every other factory — do not redefine them.

Add the two registry entries to `DATASET_REGISTRY`:

```python
    DerivedMetricsDataset.dataset_id: (DerivedMetricsDataset, _derived_metrics_options),
    MarketDailyDataset.dataset_id: (MarketDailyDataset, _market_daily_options),
```

Add the two dependency entries to `DATASET_DEPENDENCIES`, keeping the dict's existing alphabetical-ish grouping:

```python
    "derived_metrics": ("fundamental_standardized",),
    "market_daily": (
        "derived_metrics",
        "tbltickerhistory_daily",
        "shares_outstanding_history",
    ),
```

`_apply_dataset_dependencies()` runs at import and raises on a dependency key with no registry entry, so both edits must land together.

- [ ] **Step 4: Run the jobs half**

Run: `.venv\Scripts\python.exe -m pytest tests/test_derived_wiring.py -n 0 -q -k "registered or classes or factories or dependency or depends_on"`
Expected: `5 passed`.

Run: `.venv\Scripts\python.exe -m pytest tests/test_jobs_dag.py tests/test_warehouse_jobs_cli.py -n 0 -q`
Expected: `passed`. If `test_jobs_dag.py` pins the number of registered datasets, bump it by 2 and justify it in the commit body.

- [ ] **Step 5: Add the two activation stages**

In `src/atx_db/activation.py`, extend `STAGE_ORDER` (the two new names go between `"reconciliation"` and `"provider_coverage"`):

```python
STAGE_ORDER: tuple[str, ...] = (
    ...
    "reconciliation",
    "derived_metrics",
    "market_daily",
    "provider_coverage",
)
```

and add the two stage functions next to the other stage functions S1 Task 7 introduced:

```python
def stage_derived_metrics(store: DuckDBStore, options: ActivationOptions) -> StageResult:
    """Seed the declarative catalog and materialise every quarterly derived metric."""
    from .derived_metrics import DerivedMetricsOptions, refresh_derived_metrics
    from .derived_registry import seed_derived_metric_definitions

    seeded = seed_derived_metric_definitions(store)
    rows = refresh_derived_metrics(store, DerivedMetricsOptions(run_id=options.run_id))
    metric_count = int(
        store.con.execute(
            "SELECT count(DISTINCT metric_code) FROM derived_metric_values"
        ).fetchone()[0]
    )
    return StageResult(
        rows=rows,
        detail={"definitions_seeded": seeded, "metric_count": metric_count},
    )


def stage_market_daily(store: DuckDBStore, options: ActivationOptions) -> StageResult:
    """Build the daily market panel from bars plus the point-in-time fundamental state."""
    from .market_daily import (
        MarketDailyOptions,
        refresh_market_daily_metrics,
        shares_reconciliation_report,
    )

    rows = refresh_market_daily_metrics(store, MarketDailyOptions(run_id=options.run_id))
    return StageResult(rows=rows, detail=dict(shares_reconciliation_report(store)))
```

and register both in `STAGES`:

```python
    "derived_metrics": stage_derived_metrics,
    "market_daily": stage_market_daily,
```

The local imports inside the stage functions are deliberate: `activation` is imported by `cli.py` at start-up, and a module-level import of `derived_metrics` would pull pandas into every CLI invocation. The other stage functions in this module follow the same pattern.

- [ ] **Step 6: Run the full suite**

Run: `.venv\Scripts\python.exe -m pytest tests/test_derived_wiring.py -n 0 -q`
Expected: `11 passed`.

Run: `.venv\Scripts\python.exe -m pytest tests/test_activation_ledger.py tests/test_activation_ladder.py -n 0 -q`
Expected: `passed`. `test_activation_ladder.py` asserts `tuple(sorted(STAGES)) == tuple(sorted(STAGE_ORDER))`; with the two new stages registered this holds. If it pins `_OFFLINE_SLICE`, extend that tuple with `"derived_metrics"` and `"market_daily"` in the same commit and justify it.

Run: `.venv\Scripts\python.exe -m pytest tests/ -n 4 -q`
Expected: `passed` — the whole suite, after the Task 8 deletions.

Run: `.venv\Scripts\python.exe -m mypy --strict src/atx_db/derived_dsl.py src/atx_db/derived_registry.py src/atx_db/derived_metrics.py src/atx_db/market_daily.py src/atx_db/derived_factor_projection.py src/atx_db/panel_export.py`
Expected: `Success: no issues found in 6 source files`.

Run: `.venv\Scripts\python.exe -m ruff check src/atx_db/derived_dsl.py src/atx_db/derived_registry.py src/atx_db/derived_metrics.py src/atx_db/market_daily.py src/atx_db/derived_factor_projection.py src/atx_db/panel_export.py`
Expected: `All checks passed!`

- [ ] **Step 7: Commit**

```
git add src/atx_db/jobs.py src/atx_db/activation.py tests/test_derived_wiring.py tests/conftest.py
git commit -m "feat(db): wire the derived engine into the job DAG and activation ladder

derived_metrics and market_daily become first-class Dataset entries with
declared dependency edges, so they get watermarks, retries and ordering instead
of living only in scripts/. The activation ladder gains a derived stage pair
between reconciliation and provider_coverage; the market stage reports the
shares reconciliation pass rate in its StageResult detail.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

## After the sprint

Operator sequence on a live warehouse, once Sprint 1's ladder and Sprint 2's widened item registry have both landed:

```
.venv\Scripts\python.exe scripts\warehouse_activate.py --start-stage derived_metrics --stop-stage market_daily
.venv\Scripts\python.exe scripts\export_panels.py --as-of 2026-09-19 --out-dir data\panels --panel both
```

Facts worth appending to `atx-vol/docs/LEDGER.md` on gate-pass: the measured row count of `derived_metric_values` per metric window, the `market_daily_metrics` row count and date span, the shares-reconciliation `pass_rate` against the spec's 95% gate, and the wall-clock of a full `refresh_derived_metrics` at the batch size used.
