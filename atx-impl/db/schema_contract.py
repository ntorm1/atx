"""PF2-S1 S1-0: declarative schema contract + live-vs-contract drift detector.

Today the warehouse's "true" shape is emergent: whatever ``schema.py::ensure_quant_schema``
plus ``migrations.py::MIGRATIONS`` happen to leave behind after running against a live
connection. Nothing writes down what tables/columns *should* exist, so a dropped column,
a widened type, or a new uncatalogued table is invisible until something downstream breaks.

This module is the first piece of clause (E) (schema-as-contract):

- ``ColumnSpec`` / ``CONTRACT``: a declarative manifest, ``table_name -> [ColumnSpec, ...]``.
  Each column is tagged ``declared_in`` (``schema_py`` or ``migration``) because a single
  table's columns can originate from *both* paths -- e.g. a table created by
  ``ensure_quant_schema`` can later gain columns via a migration's ``ALTER TABLE ... ADD
  COLUMN`` (see ``migrations.py::_schema_evolution_alters``), so ``declared_in`` is a
  column-level fact, not a table-level one.

  S1-0 seeds the manifest *machinery* and a representative subset of tables (a mix of
  schema.py-declared catalog/control tables and migration-declared fact/catalog tables,
  covering both PIT and non-PIT columns) -- not an exhaustive enumeration of the ~60+ live
  tables. Reconciling the manifest to *full* coverage over both code paths is PF2-S1 S1-2's
  job; a partial manifest here is fine because the detector is used against fixtures (this
  module's tests) and, later, against explicit table subsets -- not asserted `== []` against
  the live warehouse until S1-2 lands.

- ``detect_schema_drift(store, contract=None) -> list[DriftRow]``: a PURE read over
  ``duckdb_tables()``/``duckdb_columns()`` (the live warehouse) diffed against the manifest.
  Deterministic: same inputs -> same (sorted) rows. Never mutates the connection, never
  forces a schema rebuild -- this is meant to be called on demand (an operator command or a
  quality-gate check), not from the hot ``DuckDBStore.initialize()`` path.

- ``schema_contract_sha256``: a stable hash over the sorted manifest, mirroring the hash
  discipline in ``db/lake.py::_schema_sha256`` (declare a shape, hash it, compare it). This
  is the single comparable baseline PF2-S2's post-migration verify will read.

The manifest is additionally persisted as data in the ``schema_contract`` table (migration
0097; indexes in 0098) so it is queryable in plain SQL, not just importable Python -- see
``migrations.py::_schema_contract_schema_catalog``.
"""

from __future__ import annotations

import hashlib
import json
from dataclasses import dataclass
from typing import Mapping, Sequence

from .connection import DuckDBStore

# Column-level provenance tags. A table's columns can straddle both: a table created by
# ensure_quant_schema can later gain columns via a migration's ALTER TABLE ADD COLUMN.
DECLARED_IN_VALUES = frozenset({"schema_py", "migration"})

# The typed drift categories detect_schema_drift can emit. Keep in sync with the S1-0
# plan section (sprint-1-schema-contract.md) -- this is the closed set, not an example.
DRIFT_TYPES = frozenset(
    {
        "undeclared_table",
        "uncatalogued_table",
        "missing_table",
        "undeclared_column",
        "missing_column",
        "type_mismatch",
        "nullability_mismatch",
        "missing_pit_column",
    }
)

# Clause (A)'s canonical bitemporal PIT columns for fact/derived rows. A ColumnSpec's
# is_pit_column flag marks a column as one of these five for a table where the mandate
# applies; it is a per-column fact, not inferred from name alone.
PIT_COLUMN_NAMES = ("as_of_date", "available_at", "source_loaded_at", "run_id", "is_latest_revision")

# Ephemeral/internal relations excluded from both the live-table scan and the catalog
# check, matching schema.py::_seed_field_catalog's existing filter.
_EPHEMERAL_TABLE_PATTERNS = ("duckdb_%", "sqlite_%", "pragma_%")


@dataclass(frozen=True)
class ColumnSpec:
    """One declared (table, column) pair in the schema contract."""

    name: str
    data_type: str
    nullable: bool
    is_natural_key: bool = False
    is_pit_column: bool = False
    declared_in: str = "schema_py"

    def __post_init__(self) -> None:
        if self.declared_in not in DECLARED_IN_VALUES:
            raise ValueError(
                f"declared_in must be one of {sorted(DECLARED_IN_VALUES)}, got {self.declared_in!r}"
            )


@dataclass(frozen=True)
class DriftRow:
    """One typed disagreement between the live warehouse and the manifest (or catalog)."""

    drift_type: str
    table_name: str
    column_name: str | None = None
    expected: str | None = None
    actual: str | None = None

    def __post_init__(self) -> None:
        if self.drift_type not in DRIFT_TYPES:
            raise ValueError(f"drift_type must be one of {sorted(DRIFT_TYPES)}, got {self.drift_type!r}")


# ---------------------------------------------------------------------------
# The manifest itself.
#
# A representative subset (per the S1-0 brief): four schema.py-declared catalog/control
# tables (schema_migrations, source_systems, table_catalog, field_catalog) and two
# migration-declared tables -- formula_registry (migration 0075: a catalog-as-data table
# with NO fact-row PIT columns, only a bitemporal valid_from/valid_to definition window)
# and market_cap (migration 0084: a gold fact table carrying all five canonical PIT
# columns). This mix exercises both declared_in values and both PIT/non-PIT shapes without
# hand-enumerating all ~60+ live tables (S1-2's job).
# ---------------------------------------------------------------------------

CONTRACT: dict[str, list[ColumnSpec]] = {
    "schema_migrations": [
        ColumnSpec("version", "VARCHAR", nullable=False, is_natural_key=True, declared_in="schema_py"),
        ColumnSpec("description", "VARCHAR", nullable=False, declared_in="schema_py"),
        ColumnSpec("checksum", "VARCHAR", nullable=True, declared_in="schema_py"),
        ColumnSpec("applied_at", "TIMESTAMP", nullable=False, declared_in="schema_py"),
    ],
    "source_systems": [
        ColumnSpec("source_system_id", "VARCHAR", nullable=False, is_natural_key=True, declared_in="schema_py"),
        ColumnSpec("name", "VARCHAR", nullable=False, declared_in="schema_py"),
        ColumnSpec("base_url", "VARCHAR", nullable=True, declared_in="schema_py"),
        ColumnSpec("license_note", "VARCHAR", nullable=True, declared_in="schema_py"),
        ColumnSpec("cadence", "VARCHAR", nullable=True, declared_in="schema_py"),
        ColumnSpec("requires_key", "BOOLEAN", nullable=False, declared_in="schema_py"),
        ColumnSpec("metadata_json", "VARCHAR", nullable=True, declared_in="schema_py"),
        ColumnSpec("created_at", "TIMESTAMP", nullable=False, declared_in="schema_py"),
        ColumnSpec("updated_at", "TIMESTAMP", nullable=False, declared_in="schema_py"),
    ],
    "table_catalog": [
        ColumnSpec("table_name", "VARCHAR", nullable=False, is_natural_key=True, declared_in="schema_py"),
        ColumnSpec("layer", "VARCHAR", nullable=False, declared_in="schema_py"),
        ColumnSpec("entity", "VARCHAR", nullable=True, declared_in="schema_py"),
        ColumnSpec("grain", "VARCHAR", nullable=True, declared_in="schema_py"),
        ColumnSpec("description", "VARCHAR", nullable=True, declared_in="schema_py"),
        ColumnSpec("natural_key_json", "VARCHAR", nullable=True, declared_in="schema_py"),
        ColumnSpec("pit_notes", "VARCHAR", nullable=True, declared_in="schema_py"),
        ColumnSpec("created_at", "TIMESTAMP", nullable=False, declared_in="schema_py"),
        ColumnSpec("updated_at", "TIMESTAMP", nullable=False, declared_in="schema_py"),
    ],
    "field_catalog": [
        ColumnSpec("table_name", "VARCHAR", nullable=False, is_natural_key=True, declared_in="schema_py"),
        ColumnSpec("field_name", "VARCHAR", nullable=False, is_natural_key=True, declared_in="schema_py"),
        ColumnSpec("semantic_type", "VARCHAR", nullable=True, declared_in="schema_py"),
        ColumnSpec("description", "VARCHAR", nullable=True, declared_in="schema_py"),
        ColumnSpec("nullable", "BOOLEAN", nullable=True, declared_in="schema_py"),
        ColumnSpec("unit", "VARCHAR", nullable=True, declared_in="schema_py"),
        ColumnSpec("source_field", "VARCHAR", nullable=True, declared_in="schema_py"),
        ColumnSpec("created_at", "TIMESTAMP", nullable=False, declared_in="schema_py"),
        ColumnSpec("updated_at", "TIMESTAMP", nullable=False, declared_in="schema_py"),
    ],
    "formula_registry": [
        ColumnSpec("formula_code", "VARCHAR", nullable=False, is_natural_key=True, declared_in="migration"),
        ColumnSpec("family", "VARCHAR", nullable=False, declared_in="migration"),
        ColumnSpec("kind", "VARCHAR", nullable=False, declared_in="migration"),
        ColumnSpec("unit", "VARCHAR", nullable=False, declared_in="migration"),
        ColumnSpec("numerator_code", "VARCHAR", nullable=True, declared_in="migration"),
        ColumnSpec("denominator_code", "VARCHAR", nullable=True, declared_in="migration"),
        ColumnSpec("numerator_item_ids_json", "VARCHAR", nullable=True, declared_in="migration"),
        ColumnSpec("denominator_item_ids_json", "VARCHAR", nullable=True, declared_in="migration"),
        ColumnSpec("inputs_json", "VARCHAR", nullable=False, declared_in="migration"),
        ColumnSpec("transform", "VARCHAR", nullable=False, declared_in="migration"),
        ColumnSpec("expression", "VARCHAR", nullable=True, declared_in="migration"),
        ColumnSpec("is_meaningful_rule", "VARCHAR", nullable=True, declared_in="migration"),
        ColumnSpec("definition", "VARCHAR", nullable=False, declared_in="migration"),
        ColumnSpec("citation", "VARCHAR", nullable=True, declared_in="migration"),
        ColumnSpec("valid_from", "DATE", nullable=False, declared_in="migration"),
        ColumnSpec("valid_to", "DATE", nullable=True, declared_in="migration"),
        ColumnSpec("run_id", "VARCHAR", nullable=True, declared_in="migration"),
        ColumnSpec("source_loaded_at", "TIMESTAMP", nullable=False, declared_in="migration"),
    ],
    "market_cap": [
        ColumnSpec("market_cap_id", "VARCHAR", nullable=False, declared_in="migration"),
        ColumnSpec("source", "VARCHAR", nullable=False, is_natural_key=True, declared_in="migration"),
        ColumnSpec("price_source", "VARCHAR", nullable=False, declared_in="migration"),
        ColumnSpec("share_source", "VARCHAR", nullable=False, declared_in="migration"),
        ColumnSpec("security_id", "VARCHAR", nullable=False, is_natural_key=True, declared_in="migration"),
        ColumnSpec("symbol", "VARCHAR", nullable=True, declared_in="migration"),
        ColumnSpec("trade_date", "DATE", nullable=False, is_natural_key=True, declared_in="migration"),
        ColumnSpec("close", "DOUBLE", nullable=False, declared_in="migration"),
        ColumnSpec("share_count", "DOUBLE", nullable=False, declared_in="migration"),
        ColumnSpec("share_count_type_used", "VARCHAR", nullable=False, declared_in="migration"),
        ColumnSpec("market_cap", "DOUBLE", nullable=False, declared_in="migration"),
        ColumnSpec("is_latest_revision", "BOOLEAN", nullable=False, is_pit_column=True, declared_in="migration"),
        ColumnSpec("as_of_date", "DATE", nullable=False, is_pit_column=True, declared_in="migration"),
        ColumnSpec("available_at", "TIMESTAMP", nullable=False, is_pit_column=True, declared_in="migration"),
        ColumnSpec("price_available_at", "TIMESTAMP", nullable=False, declared_in="migration"),
        ColumnSpec("share_available_at", "TIMESTAMP", nullable=False, declared_in="migration"),
        ColumnSpec("price_run_id", "VARCHAR", nullable=True, declared_in="migration"),
        ColumnSpec("share_run_id", "VARCHAR", nullable=True, declared_in="migration"),
        ColumnSpec("share_history_id", "VARCHAR", nullable=True, declared_in="migration"),
        ColumnSpec("input_codes_json", "VARCHAR", nullable=False, declared_in="migration"),
        ColumnSpec("input_lineage_json", "VARCHAR", nullable=False, declared_in="migration"),
        ColumnSpec("run_id", "VARCHAR", nullable=True, is_pit_column=True, declared_in="migration"),
        ColumnSpec("source_loaded_at", "TIMESTAMP", nullable=False, is_pit_column=True, declared_in="migration"),
        ColumnSpec("updated_at", "TIMESTAMP", nullable=False, declared_in="migration"),
    ],
}


def _fetch_live_tables(con) -> set[str]:
    """Base tables in the main schema, excluding ephemeral/internal relations.

    Uses duckdb_tables() (not duckdb_columns()) so registered temp relations (e.g. the
    ``_field_catalog_seed`` pattern in schema.py) are excluded for free -- con.register()
    relations show up in duckdb_columns() but never in duckdb_tables(). The explicit
    duckdb_%/sqlite_%/pragma_% filters are kept anyway to mirror _seed_field_catalog's
    filter literally.
    """
    rows = con.execute(
        """
        SELECT table_name
        FROM duckdb_tables()
        WHERE schema_name = 'main'
          AND table_name NOT LIKE 'duckdb_%'
          AND table_name NOT LIKE 'sqlite_%'
          AND table_name NOT LIKE 'pragma_%'
        """
    ).fetchall()
    return {row[0] for row in rows}


def _fetch_live_columns(con, table_names: Sequence[str]) -> dict[str, dict[str, tuple[str, bool]]]:
    """column_name -> (data_type, is_nullable) per live table, scoped to table_names."""
    if not table_names:
        return {}
    rows = con.execute(
        """
        SELECT table_name, column_name, data_type, is_nullable
        FROM duckdb_columns()
        WHERE schema_name = 'main'
          AND table_name = ANY(?)
        """,
        [list(table_names)],
    ).fetchall()
    out: dict[str, dict[str, tuple[str, bool]]] = {}
    for table_name, column_name, data_type, is_nullable in rows:
        out.setdefault(table_name, {})[column_name] = (data_type, bool(is_nullable))
    return out


def _fetch_catalogued_tables(con) -> set[str]:
    """table_catalog's table_name set; empty (not an error) if table_catalog is absent."""
    try:
        rows = con.execute("SELECT table_name FROM table_catalog").fetchall()
    except Exception:
        return set()
    return {row[0] for row in rows}


def detect_schema_drift(
    store: DuckDBStore, contract: Mapping[str, Sequence[ColumnSpec]] | None = None
) -> list[DriftRow]:
    """Pure diff of the live warehouse against the manifest. Deterministic; read-only.

    ``contract`` defaults to the module-level CONTRACT; callers (tests, and later PF2-S2)
    may pass a narrower/custom manifest -- e.g. to scope the scan to a subset of tables
    without asserting the full live warehouse matches an as-yet-incomplete manifest (that
    completeness gate is PF2-S1 S1-2's job).

    Returns rows sorted by (drift_type, table_name, column_name) for determinism.
    """
    manifest: Mapping[str, Sequence[ColumnSpec]] = CONTRACT if contract is None else contract
    con = store.con

    live_tables = _fetch_live_tables(con)
    catalogued_tables = _fetch_catalogued_tables(con)
    live_columns = _fetch_live_columns(con, sorted(live_tables))

    manifest_table_names = set(manifest)
    drift: list[DriftRow] = []

    for table_name in live_tables - manifest_table_names:
        drift.append(DriftRow(drift_type="undeclared_table", table_name=table_name))

    for table_name in manifest_table_names - live_tables:
        drift.append(DriftRow(drift_type="missing_table", table_name=table_name))

    for table_name in live_tables:
        if table_name not in catalogued_tables:
            drift.append(DriftRow(drift_type="uncatalogued_table", table_name=table_name))

    for table_name in manifest_table_names & live_tables:
        specs = {spec.name: spec for spec in manifest[table_name]}
        live = live_columns.get(table_name, {})

        for column_name in set(specs) - set(live):
            spec = specs[column_name]
            drift_type = "missing_pit_column" if spec.is_pit_column else "missing_column"
            drift.append(DriftRow(drift_type=drift_type, table_name=table_name, column_name=column_name))

        for column_name in set(live) - set(specs):
            drift.append(DriftRow(drift_type="undeclared_column", table_name=table_name, column_name=column_name))

        for column_name in set(specs) & set(live):
            spec = specs[column_name]
            live_type, live_nullable = live[column_name]
            if live_type.upper() != spec.data_type.upper():
                drift.append(
                    DriftRow(
                        drift_type="type_mismatch",
                        table_name=table_name,
                        column_name=column_name,
                        expected=spec.data_type,
                        actual=live_type,
                    )
                )
            if live_nullable != spec.nullable:
                drift.append(
                    DriftRow(
                        drift_type="nullability_mismatch",
                        table_name=table_name,
                        column_name=column_name,
                        expected=str(spec.nullable),
                        actual=str(live_nullable),
                    )
                )

    return sorted(drift, key=lambda row: (row.drift_type, row.table_name, row.column_name or ""))


def _manifest_payload(manifest: Mapping[str, Sequence[ColumnSpec]]) -> list[dict[str, object]]:
    """Deterministic JSON-able payload: sorted tables, sorted columns within each table."""
    payload: list[dict[str, object]] = []
    for table_name in sorted(manifest):
        columns = sorted(manifest[table_name], key=lambda spec: spec.name)
        payload.append(
            {
                "table": table_name,
                "columns": [
                    {
                        "name": spec.name,
                        "data_type": spec.data_type,
                        "nullable": spec.nullable,
                        "is_natural_key": spec.is_natural_key,
                        "is_pit_column": spec.is_pit_column,
                        "declared_in": spec.declared_in,
                    }
                    for spec in columns
                ],
            }
        )
    return payload


def schema_contract_sha256(contract: Mapping[str, Sequence[ColumnSpec]] | None = None) -> str:
    """Stable hash over the sorted manifest, mirroring db/lake.py::_schema_sha256."""
    manifest: Mapping[str, Sequence[ColumnSpec]] = CONTRACT if contract is None else contract
    payload = json.dumps(_manifest_payload(manifest), sort_keys=True, separators=(",", ":")).encode("utf-8")
    return hashlib.sha256(payload).hexdigest()
