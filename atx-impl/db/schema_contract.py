"""PF2-S1 S1-0/S1-2: declarative schema contract + live-vs-contract drift detector.

Today the warehouse's "true" shape is emergent: whatever ``schema.py::ensure_quant_schema``
plus ``db.migrations::MIGRATIONS`` happen to leave behind after running against a live
connection. Nothing writes down what tables/columns *should* exist, so a dropped column,
a widened type, or a new uncatalogued table is invisible until something downstream breaks.

This module is the first piece of clause (E) (schema-as-contract):

- ``ColumnSpec`` / ``CONTRACT``: a declarative manifest, ``table_name -> [ColumnSpec, ...]``.
  Each column is tagged ``declared_in`` (``schema_py`` or ``migration``) because a single
  table's columns can originate from *both* paths -- e.g. a table created by
  ``ensure_quant_schema`` can later gain columns via a migration's ``ALTER TABLE ... ADD
  COLUMN`` (see ``db.migrations::_schema_evolution_alters``), so ``declared_in`` is a
  column-level fact, not a table-level one.

  S1-0 seeded the manifest *machinery* and a representative subset of tables (a mix of
  schema.py-declared catalog/control tables and migration-declared fact/catalog tables,
  covering both PIT and non-PIT columns) -- not an exhaustive enumeration of the ~60+ live
  tables.

- ``build_contract_manifest(con) -> dict[str, list[ColumnSpec]]``: PF2-S1 S1-2's full
  reconciliation. Reuses CONTRACT verbatim wherever it already declares a table (so the
  two never disagree) and derives every other live table from a freshly bootstrapped
  connection's ``duckdb_tables()``/``duckdb_columns()``, attributing ``declared_in`` by
  text-scanning the imperative schema sources (schema.py + connection.py for the
  unversioned bootstrap path, db/migrations/ for the versioned one -- see the module-level
  design note above ``build_contract_manifest``'s definition). This is the manifest
  migration 0097 now persists into ``schema_contract``, and the one a coverage test
  asserts has zero residual against ``duckdb_tables()`` in either direction.

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
``db.migrations::_schema_contract_schema_catalog``.
"""

from __future__ import annotations

import hashlib
import json
import re
from dataclasses import dataclass
from pathlib import Path
from typing import Mapping, Sequence

from .connection import DuckDBStore

# Column-level provenance tags. A table's columns can straddle both: a table created by
# ensure_quant_schema can later gain columns via a migration's ALTER TABLE ADD COLUMN.
DECLARED_IN_VALUES = frozenset({"schema_py", "migration"})
SIGN_VALUES = frozenset({"signed", "non_negative", "non_positive", "unit_interval", "bounded"})
SCHEMA_CONTRACT_VERSION = "v2"
SCHEMA_CONTRACT_VERSION_TABLE = "schema_contract_version"

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
    unit: str | None = None
    sign: str | None = None
    scale: str | None = None
    natural_key: bool | None = None

    def __post_init__(self) -> None:
        if self.declared_in not in DECLARED_IN_VALUES:
            raise ValueError(
                f"declared_in must be one of {sorted(DECLARED_IN_VALUES)}, got {self.declared_in!r}"
            )
        if self.sign is not None and self.sign not in SIGN_VALUES:
            raise ValueError(f"sign must be one of {sorted(SIGN_VALUES)} or None, got {self.sign!r}")
        if self.natural_key is None:
            object.__setattr__(self, "natural_key", self.is_natural_key)


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


@dataclass(frozen=True)
class SchemaContractVersionPin:
    """Persisted schema-contract identity paired with the live manifest hash."""

    version: str
    manifest_sha256: str
    expected_version: str
    expected_manifest_sha256: str


class SchemaContractVersionMismatch(RuntimeError):
    """Raised when the persisted schema-contract version pin does not match live code."""


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
        ColumnSpec(
            "market_cap_id", "VARCHAR", nullable=False, declared_in="migration",
            unit="identifier", sign="bounded", scale="nominal",
        ),
        ColumnSpec(
            "source", "VARCHAR", nullable=False, is_natural_key=True, declared_in="migration",
            unit="identifier", sign="bounded", scale="nominal",
        ),
        ColumnSpec(
            "price_source", "VARCHAR", nullable=False, declared_in="migration",
            unit="identifier", sign="bounded", scale="nominal",
        ),
        ColumnSpec(
            "share_source", "VARCHAR", nullable=False, declared_in="migration",
            unit="identifier", sign="bounded", scale="nominal",
        ),
        ColumnSpec(
            "security_id", "VARCHAR", nullable=False, is_natural_key=True, declared_in="migration",
            unit="identifier", sign="bounded", scale="nominal",
        ),
        ColumnSpec(
            "symbol", "VARCHAR", nullable=True, declared_in="migration",
            unit="identifier", sign="bounded", scale="nominal",
        ),
        ColumnSpec(
            "trade_date", "DATE", nullable=False, is_natural_key=True, declared_in="migration",
            unit="date", sign="bounded", scale="day",
        ),
        ColumnSpec(
            "close", "DOUBLE", nullable=False, declared_in="migration",
            unit="USD", sign="non_negative", scale="1",
        ),
        ColumnSpec(
            "share_count", "DOUBLE", nullable=False, declared_in="migration",
            unit="shares", sign="non_negative", scale="1",
        ),
        ColumnSpec(
            "share_count_type_used", "VARCHAR", nullable=False, declared_in="migration",
            unit="category", sign="bounded", scale="nominal",
        ),
        ColumnSpec(
            "market_cap", "DOUBLE", nullable=False, declared_in="migration",
            unit="USD", sign="non_negative", scale="1",
        ),
        ColumnSpec(
            "is_latest_revision", "BOOLEAN", nullable=False, is_pit_column=True,
            declared_in="migration", unit="flag", sign="bounded", scale="boolean",
        ),
        ColumnSpec(
            "as_of_date", "DATE", nullable=False, is_pit_column=True, declared_in="migration",
            unit="date", sign="bounded", scale="day",
        ),
        ColumnSpec(
            "available_at", "TIMESTAMP", nullable=False, is_pit_column=True,
            declared_in="migration", unit="timestamp", sign="bounded", scale="second",
        ),
        ColumnSpec(
            "price_available_at", "TIMESTAMP", nullable=False, declared_in="migration",
            unit="timestamp", sign="bounded", scale="second",
        ),
        ColumnSpec(
            "share_available_at", "TIMESTAMP", nullable=False, declared_in="migration",
            unit="timestamp", sign="bounded", scale="second",
        ),
        ColumnSpec(
            "price_run_id", "VARCHAR", nullable=True, declared_in="migration",
            unit="identifier", sign="bounded", scale="nominal",
        ),
        ColumnSpec(
            "share_run_id", "VARCHAR", nullable=True, declared_in="migration",
            unit="identifier", sign="bounded", scale="nominal",
        ),
        ColumnSpec(
            "share_history_id", "VARCHAR", nullable=True, declared_in="migration",
            unit="identifier", sign="bounded", scale="nominal",
        ),
        ColumnSpec(
            "input_codes_json", "VARCHAR", nullable=False, declared_in="migration",
            unit="json", sign="bounded", scale="nominal",
        ),
        ColumnSpec(
            "input_lineage_json", "VARCHAR", nullable=False, declared_in="migration",
            unit="json", sign="bounded", scale="nominal",
        ),
        ColumnSpec(
            "run_id", "VARCHAR", nullable=True, is_pit_column=True, declared_in="migration",
            unit="identifier", sign="bounded", scale="nominal",
        ),
        ColumnSpec(
            "source_loaded_at", "TIMESTAMP", nullable=False, is_pit_column=True,
            declared_in="migration", unit="timestamp", sign="bounded", scale="second",
        ),
        ColumnSpec(
            "updated_at", "TIMESTAMP", nullable=False, declared_in="migration",
            unit="timestamp", sign="bounded", scale="second",
        ),
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
                        "unit": spec.unit,
                        "sign": spec.sign,
                        "scale": spec.scale,
                        "natural_key": spec.natural_key,
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


def assert_schema_contract_version(
    con,
    *,
    expected_version: str | None = None,
    manifest: Mapping[str, Sequence[ColumnSpec]] | None = None,
) -> SchemaContractVersionPin:
    """Assert the persisted contract version row pins the live manifest hash.

    A schema-contract hash change without a matching version-row migration fails here:
    callers must either keep the manifest stable for the current version, or bump
    ``SCHEMA_CONTRACT_VERSION`` and persist the new version/hash row.
    """
    resolved_version = expected_version or SCHEMA_CONTRACT_VERSION
    resolved_manifest = build_contract_manifest(con) if manifest is None else manifest
    expected_sha256 = schema_contract_sha256(resolved_manifest)

    table_exists = con.execute(
        """
        SELECT count(*)
        FROM duckdb_tables()
        WHERE schema_name = 'main'
          AND table_name = ?
        """,
        [SCHEMA_CONTRACT_VERSION_TABLE],
    ).fetchone()[0]
    if not table_exists:
        raise SchemaContractVersionMismatch(
            f"{SCHEMA_CONTRACT_VERSION_TABLE} is missing; expected {resolved_version} "
            f"with manifest_sha256 {expected_sha256}"
        )

    row = con.execute(
        f"""
        SELECT version, manifest_sha256
        FROM {SCHEMA_CONTRACT_VERSION_TABLE}
        WHERE version = ?
        """,
        [resolved_version],
    ).fetchone()
    if row is None:
        versions = [
            str(value)
            for (value,) in con.execute(
                f"SELECT version FROM {SCHEMA_CONTRACT_VERSION_TABLE} ORDER BY version"
            ).fetchall()
        ]
        raise SchemaContractVersionMismatch(
            f"schema contract version {resolved_version!r} is not persisted; "
            f"persisted versions={versions!r}, expected manifest_sha256 {expected_sha256}"
        )

    version, manifest_sha256 = str(row[0]), str(row[1])
    pin = SchemaContractVersionPin(
        version=version,
        manifest_sha256=manifest_sha256,
        expected_version=resolved_version,
        expected_manifest_sha256=expected_sha256,
    )
    if manifest_sha256 != expected_sha256:
        raise SchemaContractVersionMismatch(
            f"schema contract {version} pins manifest_sha256 {manifest_sha256}, "
            f"but live manifest is {expected_sha256}; bump {SCHEMA_CONTRACT_VERSION_TABLE} "
            "with the contract version when the manifest intentionally changes"
        )
    return pin


# ---------------------------------------------------------------------------
# PF2-S1 S1-2: reconcile BOTH imperative schema paths into one COMPLETE manifest.
#
# CONTRACT (above) is a hand-curated, representative 6-table subset -- deliberately
# partial per S1-0's brief. build_contract_manifest() below is the full reconciliation:
# every live table, sourced from a freshly bootstrapped DuckDBStore's
# duckdb_tables()/duckdb_columns() (the ground truth for shape), with declared_in
# attributed by detecting which imperative source(s) actually declare each table/column.
#
# There are, in practice, THREE source files that create tables imperatively, not two:
#   - db/schema.py::ensure_quant_schema -- the core "always run, CREATE IF NOT EXISTS"
#     bootstrap tables.
#   - db/connection.py::DuckDBStore.initialize() -- creates dataset_runs/
#     dataset_watermarks/security_identifiers directly, with the exact same unversioned
#     bootstrap semantics as schema.py, just physically colocated in connection.py for
#     historical reasons (it sandwiches the ensure_quant_schema/apply_pending_migrations
#     calls). Both feed the `schema_py` declared_in bucket.
#   - db/migrations/::MIGRATIONS -- versioned migrations; the `migration` bucket.
#
# A handful of legacy tables (e.g. xbrl_validation_results, taxonomy, market_cap) were
# later "rebaselined" directly into schema.py's CREATE TABLE list for fresh-bootstrap
# speed, so they are now declared in BOTH schema.py and their original migration. On that
# overlap, `migration` wins -- the migration is the versioned record of when the table
# was introduced (see _reference_classifications' own docstring), matching the precedent
# S1-0 already set by hand-tagging market_cap `migration` even though schema.py also
# creates it. The same precedence applies at column granularity: a column added to an
# already-schema_py table via a migration's `ALTER TABLE ... ADD COLUMN` (e.g.
# _schema_evolution_alters adding `available_at`/`run_id` to security_identifier_history)
# is tagged `migration` even where schema.py has since inlined that same column directly.
#
# This scan is best-effort text introspection over the two/three source files, not a SQL
# parser -- it only needs table/column NAMES (duckdb_columns() is the source of truth for
# types/nullability), so simple regexes over the literal "CREATE TABLE IF NOT EXISTS x"
# and "ALTER TABLE x ADD COLUMN [IF NOT EXISTS] y" statements are sufficient and avoid a
# hand-maintained table-by-table literal that would rot exactly like the manifest itself
# used to. The ONE wrinkle: db/migrations/ creates fundamental_statement_map via an
# f-string-templated helper (`CREATE TABLE IF NOT EXISTS {table_name}` in
# _create_fundamental_statement_map_table), which the literal-CREATE regex cannot see.
# Since that table is ALSO created directly by schema.py, missing the migration side would
# wrongly attribute its base columns schema_py (violating "migration wins on overlap"), so
# we additionally scan for `_create_*_table(conn, "literal")` helper call sites and treat
# their string-literal table_name args as migration-created too.
# ---------------------------------------------------------------------------

_DB_DIR = Path(__file__).resolve().parent

# Unversioned "always run" bootstrap sources -> declared_in="schema_py".
_SCHEMA_PY_SOURCE_FILES = ("schema.py", "connection.py")
# Versioned migration sources -> declared_in="migration".
_MIGRATION_SOURCE_FILES = (
    "migrations/_runner.py",
    "migrations/bodies_0001_0137.py",
    "migrations/bodies_0140_0143.py",
)

_CREATE_TABLE_RE = re.compile(r"CREATE TABLE IF NOT EXISTS (\w+)")
_ALTER_ADD_COLUMN_RE = re.compile(r"ALTER TABLE (\w+) ADD COLUMN(?: IF NOT EXISTS)? (\w+)")
# f-string-templated helper table creations, e.g.
# `_create_fundamental_statement_map_table(conn, "fundamental_statement_map")`. Only
# string-literal table_name args are captured -- calls passing a variable (e.g. the
# `scratch` rekey temp table) are intentionally skipped, since those are not live tables.
_MIGRATION_HELPER_TABLE_RE = re.compile(r"""_create_\w+_table\(\s*conn\s*,\s*['"](\w+)['"]""")

# Strong bitemporal markers. A table counts as fact/derived (clause (A)'s PIT-column
# mandate applies -> its canonical PIT columns are is_pit_column=True) iff it physically
# carries at least ONE of these. The ubiquitous bookkeeping columns run_id/source_loaded_at
# are deliberately EXCLUDED here: nearly every table has them, so they cannot distinguish a
# fact from a dimension/master/landing table.
#
# This reproduces CONTRACT's hand-curated split automatically, without a layer-set knob or
# a hand override list: market_cap (has available_at + as_of_date) -> fact -> all five PIT
# columns; formula_registry (only run_id/source_loaded_at, no strong marker) -> non-fact ->
# its run_id/source_loaded_at are NOT PIT. trading_calendar/dataset_runs/etl_job_*/insider/
# fund/universes/taxonomy/xbrl_filing_* likewise carry no strong marker -> non-fact -> no
# spurious PIT columns. A genuine fact table that carries none of these markers (e.g. a raw
# landing table) is correctly treated non-fact here; if some table that SHOULD be PIT is
# missing markers, that is a real finding for S1-1's live gate, not a false positive to
# paper over in the manifest.
_STRONG_TEMPORAL_MARKERS = ("as_of_date", "available_at", "is_latest_revision")


def _read_db_source(filename: str) -> str:
    return (_DB_DIR / filename).read_text(encoding="utf-8")


def _scan_declared_in_sources() -> tuple[set[str], set[str], set[tuple[str, str]]]:
    """Best-effort table/column provenance via text introspection of the schema sources.

    Returns (schema_py_tables, migration_tables, migration_added_columns):
      - schema_py_tables: table names created via ``CREATE TABLE IF NOT EXISTS`` in
        schema.py or connection.py.
      - migration_tables: table names created via ``CREATE TABLE IF NOT EXISTS``
        anywhere in db/migrations/ (every such statement lives inside some MIGRATIONS
        entry's ``up()``), PLUS tables created via an f-string-templated
        ``_create_*_table(conn, "literal")`` helper (see _MIGRATION_HELPER_TABLE_RE).
      - migration_added_columns: (table, column) pairs added via an ``ALTER TABLE ...
        ADD COLUMN`` in db/migrations/.
    """
    schema_py_text = "\n".join(_read_db_source(name) for name in _SCHEMA_PY_SOURCE_FILES)
    migration_text = "\n".join(_read_db_source(name) for name in _MIGRATION_SOURCE_FILES)

    schema_py_tables = set(_CREATE_TABLE_RE.findall(schema_py_text))
    migration_tables = set(_CREATE_TABLE_RE.findall(migration_text))
    migration_tables |= set(_MIGRATION_HELPER_TABLE_RE.findall(migration_text))
    migration_added_columns = set(_ALTER_ADD_COLUMN_RE.findall(migration_text))

    return schema_py_tables, migration_tables, migration_added_columns


def _fetch_natural_keys(con) -> dict[str, set[str]]:
    """table_name -> declared natural-key column names.

    Prefers ``table_catalog.natural_key_json`` (the curated business key, which can
    differ from a surrogate PRIMARY KEY); falls back to the live PRIMARY KEY
    constraint's columns for a table with no (or no parseable) natural_key_json.
    """
    natural_keys: dict[str, set[str]] = {}
    try:
        rows = con.execute("SELECT table_name, natural_key_json FROM table_catalog").fetchall()
    except Exception:
        rows = []
    for table_name, natural_key_json in rows:
        if not natural_key_json:
            continue
        try:
            columns = json.loads(natural_key_json)
        except (TypeError, ValueError):
            continue
        if isinstance(columns, list):
            natural_keys[table_name] = {str(c) for c in columns}

    try:
        pk_rows = con.execute(
            """
            SELECT table_name, constraint_column_names
            FROM duckdb_constraints()
            WHERE constraint_type = 'PRIMARY KEY'
            """
        ).fetchall()
    except Exception:
        pk_rows = []
    for table_name, columns in pk_rows:
        if table_name not in natural_keys and columns:
            natural_keys[table_name] = {str(c) for c in columns}

    return natural_keys


def _fetch_field_catalog_units(con) -> dict[tuple[str, str], str]:
    """(table_name, field_name) -> declared field_catalog.unit where present."""
    try:
        rows = con.execute(
            """
            SELECT table_name, field_name, unit
            FROM field_catalog
            WHERE unit IS NOT NULL
              AND length(trim(unit)) > 0
            """
        ).fetchall()
    except Exception:
        rows = []
    return {(str(table_name), str(field_name)): str(unit) for table_name, field_name, unit in rows}


_SIGNED_SEMANTIC_TOKENS = frozenset({"change", "delta", "growth", "net", "return", "returns"})
_RATIO_UNIT_TOKENS = frozenset({"growth", "pct", "percent", "percentage", "percentile", "ratio", "return", "returns", "weight"})
_NON_NEGATIVE_NAME_TOKENS = frozenset({"amount", "count", "price", "quantity", "share", "shares", "volume"})
_PERCENTILE_UNIT_NAMES = frozenset({"percentile", "percent_rank", "pct_rank"})


def _semantic_tokens(name: str) -> set[str]:
    return set(re.findall(r"[a-z0-9]+", name.lower()))


def _has_signed_semantic(name: str) -> bool:
    lower = name.lower()
    tokens = _semantic_tokens(name)
    return bool(tokens & _SIGNED_SEMANTIC_TOKENS) or lower.startswith("ret_")


def _has_ratio_unit_semantic(name: str) -> bool:
    lower = name.lower()
    tokens = _semantic_tokens(name)
    return bool(tokens & _RATIO_UNIT_TOKENS) or lower.startswith("ret_")


def _has_rank_semantic(name: str) -> bool:
    lower = name.lower()
    tokens = _semantic_tokens(name)
    return "rank" in tokens or lower.endswith("_rank")


def _has_unit_interval_semantic(name: str, unit: str) -> bool:
    lower = name.lower()
    tokens = _semantic_tokens(name)
    unit_lower = unit.lower()
    return (
        "percentile" in lower
        or unit_lower in _PERCENTILE_UNIT_NAMES
        or (_has_rank_semantic(name) and ({"pct", "percent", "percentile"} & tokens))
    )


def _infer_semantic_unit(name: str, data_type: str) -> str:
    lower = name.lower()
    dtype = data_type.upper()
    if lower.endswith("_date") or dtype == "DATE" or lower in {"valid_from", "valid_to"}:
        return "date"
    if lower.endswith("_at") or "TIMESTAMP" in dtype or "DATETIME" in dtype:
        return "timestamp"
    if dtype == "BOOLEAN" or lower.startswith("is_") or lower.startswith("has_"):
        return "flag"
    if lower.endswith("_json") or "json" in lower:
        return "json"
    if lower.endswith("_id") or lower in {
        "id",
        "source",
        "symbol",
        "ticker",
        "cik",
        "cusip",
        "figi",
        "run_id",
    }:
        return "identifier"
    if lower.endswith("_usd") or lower in {
        "value_usd",
        "cash_amount",
        "dollar_volume",
        "market_cap",
        "close",
        "open",
        "high",
        "low",
        "vwap",
    }:
        return "USD"
    if _has_ratio_unit_semantic(name):
        return "ratio"
    if "volume" in lower or "share" in lower or "quantity" in lower or lower.endswith("_count"):
        return "shares"
    if any(token in lower for token in ("code", "type", "status", "category", "source")) or "VARCHAR" in dtype:
        return "category"
    return "dimensionless"


def _infer_semantic_sign(name: str, data_type: str, unit: str) -> str:
    lower = name.lower()
    dtype = data_type.upper()
    tokens = _semantic_tokens(name)
    unit_lower = unit.lower()
    if (
        unit in {"date", "timestamp", "flag", "identifier", "category", "json"}
        or "VARCHAR" in dtype
        or dtype == "BOOLEAN"
    ):
        return "bounded"
    if _has_unit_interval_semantic(name, unit) or lower in {"weight", "confidence", "extraction_confidence"}:
        return "unit_interval"
    if _has_rank_semantic(name):
        return "bounded"
    if _has_signed_semantic(name):
        return "signed"
    if tokens & _NON_NEGATIVE_NAME_TOKENS or "market_cap" in lower:
        return "non_negative"
    if unit_lower in {"usd", "shares", "count"}:
        return "non_negative"
    if unit_lower == "ratio":
        return "bounded"
    return "signed"


def _infer_semantic_scale(unit: str, data_type: str) -> str:
    dtype = data_type.upper()
    if unit == "date":
        return "day"
    if unit == "timestamp":
        return "second"
    if unit == "flag":
        return "boolean"
    if unit in {"identifier", "category", "json"} or "VARCHAR" in dtype:
        return "nominal"
    return "1"


def _resolve_semantic_spec(
    *,
    table_name: str,
    spec: ColumnSpec,
    table_is_fact: bool,
    field_units: Mapping[tuple[str, str], str],
) -> ColumnSpec:
    """Attach deterministic semantic defaults without changing structural fields."""
    unit = spec.unit or field_units.get((table_name, spec.name))
    sign = spec.sign
    scale = spec.scale

    if table_is_fact:
        unit = unit or _infer_semantic_unit(spec.name, spec.data_type)
        sign = sign or _infer_semantic_sign(spec.name, spec.data_type, unit)
        scale = scale or _infer_semantic_scale(unit, spec.data_type)

    if unit == spec.unit and sign == spec.sign and scale == spec.scale and spec.natural_key == spec.is_natural_key:
        return spec
    return ColumnSpec(
        name=spec.name,
        data_type=spec.data_type,
        nullable=spec.nullable,
        is_natural_key=spec.is_natural_key,
        is_pit_column=spec.is_pit_column,
        declared_in=spec.declared_in,
        unit=unit,
        sign=sign,
        scale=scale,
        natural_key=spec.is_natural_key,
    )


def build_contract_manifest(con) -> dict[str, list[ColumnSpec]]:
    """Reconcile BOTH imperative schema paths into one COMPLETE manifest.

    ``con`` must be a connection to a fully bootstrapped warehouse (``ensure_quant_schema``
    + all ``MIGRATIONS`` applied) -- duckdb_tables()/duckdb_columns() are the ground truth
    this derives types/nullability from (per the S1-2 design guidance: derive
    programmatically rather than hand-maintain a second giant literal).

    Wherever CONTRACT (the S1-0 hand-curated 6-table subset) already declares a table,
    its structural ColumnSpec declarations are preserved, with semantic fields resolved
    through the same deterministic explicit-declaration / field_catalog / inference path
    used for derived tables. Every other live table is derived fresh:
      - column shape (data_type, nullable) from duckdb_columns();
      - is_natural_key from table_catalog.natural_key_json (or a PRIMARY KEY fallback);
      - is_pit_column is a canonical PIT name (PIT_COLUMN_NAMES) AND the table carries at
        least one strong bitemporal marker (_STRONG_TEMPORAL_MARKERS: as_of_date /
        available_at / is_latest_revision) -- the ubiquitous bookkeeping run_id/
        source_loaded_at alone do NOT make a table a fact, so a control/dimension/master/
        landing table keeps is_pit_column=False (otherwise it feeds false positives into
        S1-1's PIT-presence gate). This reproduces CONTRACT's hand-curated split with no
        layer-set knob or override list;
      - declared_in from _scan_declared_in_sources() (``migration`` wins on any
        schema_py/migration overlap, at both table and column granularity).

    Deterministic given a bootstrapped DB: same live schema + same source tree -> same
    manifest, every time.
    """
    live_tables = _fetch_live_tables(con)
    live_columns = _fetch_live_columns(con, sorted(live_tables))
    natural_keys = _fetch_natural_keys(con)
    field_units = _fetch_field_catalog_units(con)
    # schema_py_tables is unused now that migration-wins-on-overlap collapses to a single
    # membership test (a non-migration table is schema_py by construction); keep the scan
    # returning it for callers/tests that want the full provenance triple.
    _schema_py_tables, migration_tables, migration_added_columns = _scan_declared_in_sources()

    manifest: dict[str, list[ColumnSpec]] = {}
    for table_name in sorted(live_tables):
        table_columns = live_columns.get(table_name, {})
        table_is_fact = any(marker in table_columns for marker in _STRONG_TEMPORAL_MARKERS)

        if table_name in CONTRACT:
            manifest[table_name] = [
                _resolve_semantic_spec(
                    table_name=table_name,
                    spec=spec,
                    table_is_fact=table_is_fact,
                    field_units=field_units,
                )
                for spec in CONTRACT[table_name]
            ]
            continue

        # migration wins on overlap. A table absent from BOTH scans (e.g. created by some
        # other mechanism) falls through to schema_py rather than being dropped -- every
        # live table must land in the manifest (zero residual).
        table_is_migration = table_name in migration_tables
        # PIT columns only count where the fact/derived-row mandate applies: the table must
        # physically carry a strong bitemporal marker (as_of_date / available_at /
        # is_latest_revision). run_id/source_loaded_at alone do not qualify.
        natural_key_columns = natural_keys.get(table_name, set())
        columns: list[ColumnSpec] = []
        for column_name, (data_type, nullable) in sorted(table_columns.items()):
            column_is_migration = table_is_migration or (table_name, column_name) in migration_added_columns
            spec = ColumnSpec(
                name=column_name,
                data_type=data_type,
                nullable=nullable,
                is_natural_key=column_name in natural_key_columns,
                is_pit_column=table_is_fact and column_name in PIT_COLUMN_NAMES,
                declared_in="migration" if column_is_migration else "schema_py",
            )
            columns.append(
                _resolve_semantic_spec(
                    table_name=table_name,
                    spec=spec,
                    table_is_fact=table_is_fact,
                    field_units=field_units,
                )
            )
        manifest[table_name] = columns

    return manifest
