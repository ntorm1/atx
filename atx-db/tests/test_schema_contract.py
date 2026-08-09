"""PF2-S1 S1-0: declarative schema contract + live-vs-contract drift detector.

Covers:
- migrations 0097 (schema_contract table + its own table_catalog/field_catalog rows)
  and 0098 (indexes), following the schema-vs-index split precedent
  (_formula_registry_schema_catalog / _formula_registry_indexes).
- db/schema_contract.py::detect_schema_drift -- a pure diff of live
  duckdb_tables()/duckdb_columns() against a manifest, emitting typed DriftRow entries.
- db/schema_contract.py::schema_contract_sha256 -- a stable hash over the sorted manifest.

The fixture-only tests below build tiny, self-contained in-memory manifests/DBs (NOT the
real committed CONTRACT) so drift-type coverage is precise and independent of CONTRACT's
future growth. A separate integration test near the bottom checks the real committed
CONTRACT against a fully bootstrapped warehouse.

PF2-S1 S1-2 (schema_contract.py::build_contract_manifest) completes CONTRACT's coverage
over both imperative schema paths -- its coverage/declared_in/consistency tests live in
their own section near the bottom of this file. Migration 0097 now seeds schema_contract
from build_contract_manifest() (the full reconciled manifest) rather than the bare
CONTRACT subset, so the row-count/hash assertions below reference build_contract_manifest()
as the single source of truth for what gets persisted.
"""

from __future__ import annotations

import datetime as dt

import duckdb
import pytest

from atx_db.connection import DuckDBStore
from atx_db.schema_contract import (
    CONTRACT,
    PIT_COLUMN_NAMES,
    ColumnSpec,
    DriftRow,
    build_contract_manifest,
    detect_schema_drift,
    schema_contract_sha256,
)


def _bare_store() -> DuckDBStore:
    """A DuckDBStore wrapping a bare in-memory connection with NO schema bootstrapped.

    Bypasses DuckDBStore.__enter__/initialize() entirely so tests control exactly what
    tables/columns exist -- mirrors the `store.connection = duckdb.connect(...)` pattern
    used elsewhere in db/tests (see test_formula_registry_catalog.py's use of tmp_store,
    and conftest.py's manual connection wiring).
    """
    store = DuckDBStore(":memory:")
    store.connection = duckdb.connect(":memory:")
    return store


def _catalog(con: duckdb.DuckDBPyConnection, table_name: str) -> None:
    con.execute("INSERT INTO table_catalog (table_name) VALUES (?)", [table_name])


def _create_from_manifest(con: duckdb.DuckDBPyConnection, manifest: dict[str, list[ColumnSpec]]) -> None:
    """Materialize CREATE TABLE statements exactly matching a manifest's declared shape."""
    for table_name, specs in manifest.items():
        cols_sql = ", ".join(
            f'"{spec.name}" {spec.data_type}' + ("" if spec.nullable else " NOT NULL") for spec in specs
        )
        con.execute(f'CREATE TABLE "{table_name}" ({cols_sql})')


# A tiny three-table manifest used across the fixture-only drift tests: table_catalog
# itself (so the bookkeeping table used for uncatalogued_table checks is, like real
# production's table_catalog row, declared and cataloguing itself -- no incidental
# undeclared/uncatalogued noise from the test scaffolding), a plain table, and a fact
# table carrying PIT columns (so missing_column and missing_pit_column are independently
# exercisable).
MINI_CONTRACT: dict[str, list[ColumnSpec]] = {
    "table_catalog": [
        ColumnSpec("table_name", "VARCHAR", nullable=False, is_natural_key=True, declared_in="schema_py"),
    ],
    "widget": [
        ColumnSpec("widget_id", "VARCHAR", nullable=False, is_natural_key=True, declared_in="schema_py"),
        ColumnSpec("label", "VARCHAR", nullable=True, declared_in="schema_py"),
    ],
    "widget_fact": [
        ColumnSpec("widget_id", "VARCHAR", nullable=False, is_natural_key=True, declared_in="migration"),
        ColumnSpec("value", "DOUBLE", nullable=False, declared_in="migration"),
        ColumnSpec("as_of_date", "DATE", nullable=False, is_pit_column=True, declared_in="migration"),
        ColumnSpec("available_at", "TIMESTAMP", nullable=False, is_pit_column=True, declared_in="migration"),
        ColumnSpec("source_loaded_at", "TIMESTAMP", nullable=False, is_pit_column=True, declared_in="migration"),
        ColumnSpec("run_id", "VARCHAR", nullable=True, is_pit_column=True, declared_in="migration"),
        ColumnSpec("is_latest_revision", "BOOLEAN", nullable=False, is_pit_column=True, declared_in="migration"),
    ],
}


def _bootstrap_matching(con: duckdb.DuckDBPyConnection) -> None:
    """Create + catalog every MINI_CONTRACT table exactly as declared (zero-drift baseline)."""
    _create_from_manifest(con, MINI_CONTRACT)
    for table_name in MINI_CONTRACT:
        _catalog(con, table_name)


def _matching_fixture() -> DuckDBStore:
    """A store whose live schema exactly matches MINI_CONTRACT, fully catalogued."""
    store = _bare_store()
    _bootstrap_matching(store.con)
    return store


class TestColumnSpecAndDriftRowValidation:
    def test_column_spec_rejects_unknown_declared_in(self):
        with pytest.raises(ValueError):
            ColumnSpec("x", "VARCHAR", nullable=True, declared_in="somewhere_else")

    def test_drift_row_rejects_unknown_drift_type(self):
        with pytest.raises(ValueError):
            DriftRow(drift_type="not_a_real_type", table_name="widget")

    def test_column_spec_declared_in_accepts_schema_py_and_migration(self):
        ColumnSpec("x", "VARCHAR", nullable=True, declared_in="schema_py")
        ColumnSpec("x", "VARCHAR", nullable=True, declared_in="migration")


class TestDetectSchemaDriftBaseline:
    def test_matching_fixture_has_no_drift(self):
        store = _matching_fixture()
        try:
            assert detect_schema_drift(store, MINI_CONTRACT) == []
        finally:
            store.connection.close()

    def test_result_is_deterministic_across_repeated_calls(self):
        store = _matching_fixture()
        try:
            first = detect_schema_drift(store, MINI_CONTRACT)
            second = detect_schema_drift(store, MINI_CONTRACT)
            assert first == second == []
        finally:
            store.connection.close()


class TestPlantedExtraColumn:
    def test_undeclared_column_is_the_only_drift_row(self):
        store = _matching_fixture()
        try:
            store.con.execute('ALTER TABLE widget ADD COLUMN extra_col VARCHAR')
            rows = detect_schema_drift(store, MINI_CONTRACT)
            assert len(rows) == 1
            row = rows[0]
            assert row.drift_type == "undeclared_column"
            assert row.table_name == "widget"
            assert row.column_name == "extra_col"
        finally:
            store.connection.close()


class TestPlantedMissingPitColumn:
    def test_fact_table_missing_available_at_is_the_only_drift_row(self):
        store = _bare_store()
        con = store.con
        try:
            _create_from_manifest(con, {"table_catalog": MINI_CONTRACT["table_catalog"]})
            # Build widget_fact WITHOUT available_at (everything else matches the manifest).
            con.execute(
                """
                CREATE TABLE widget_fact (
                    widget_id VARCHAR NOT NULL,
                    value DOUBLE NOT NULL,
                    as_of_date DATE NOT NULL,
                    source_loaded_at TIMESTAMP NOT NULL,
                    run_id VARCHAR,
                    is_latest_revision BOOLEAN NOT NULL
                )
                """
            )
            _create_from_manifest(con, {"widget": MINI_CONTRACT["widget"]})
            for table_name in MINI_CONTRACT:
                _catalog(con, table_name)

            rows = detect_schema_drift(store, MINI_CONTRACT)
            assert len(rows) == 1
            row = rows[0]
            assert row.drift_type == "missing_pit_column"
            assert row.table_name == "widget_fact"
            assert row.column_name == "available_at"
        finally:
            store.connection.close()

    def test_missing_non_pit_column_is_missing_column_not_missing_pit_column(self):
        store = _bare_store()
        con = store.con
        try:
            _create_from_manifest(con, {"table_catalog": MINI_CONTRACT["table_catalog"]})
            # Drop the plain (non-PIT) `label` column from widget.
            con.execute("CREATE TABLE widget (widget_id VARCHAR NOT NULL)")
            _create_from_manifest(con, {"widget_fact": MINI_CONTRACT["widget_fact"]})
            for table_name in MINI_CONTRACT:
                _catalog(con, table_name)

            rows = detect_schema_drift(store, MINI_CONTRACT)
            assert len(rows) == 1
            row = rows[0]
            assert row.drift_type == "missing_column"
            assert row.table_name == "widget"
            assert row.column_name == "label"
        finally:
            store.connection.close()


class TestPlantedUncataloguedTable:
    def test_uncatalogued_table_is_the_only_drift_row(self):
        store = _bare_store()
        con = store.con
        try:
            _create_from_manifest(con, MINI_CONTRACT)
            # Catalog table_catalog and widget; leave `widget_fact` uncatalogued.
            _catalog(con, "table_catalog")
            _catalog(con, "widget")

            rows = detect_schema_drift(store, MINI_CONTRACT)
            assert len(rows) == 1
            row = rows[0]
            assert row.drift_type == "uncatalogued_table"
            assert row.table_name == "widget_fact"
            assert row.column_name is None
        finally:
            store.connection.close()

    def test_missing_table_catalog_table_treats_everything_as_uncatalogued(self):
        store = _bare_store()
        con = store.con
        try:
            # No table_catalog table at all -- only create the two non-catalog tables.
            _create_from_manifest(
                con,
                {"widget": MINI_CONTRACT["widget"], "widget_fact": MINI_CONTRACT["widget_fact"]},
            )

            rows = detect_schema_drift(store, MINI_CONTRACT)
            uncatalogued = {row.table_name for row in rows if row.drift_type == "uncatalogued_table"}
            assert uncatalogued == {"widget", "widget_fact"}
        finally:
            store.connection.close()


class TestPlantedUndeclaredTable:
    def test_table_not_in_manifest_is_undeclared_table(self):
        store = _matching_fixture()
        con = store.con
        try:
            con.execute("CREATE TABLE mystery_table (id VARCHAR)")
            _catalog(con, "mystery_table")  # catalogued, so ONLY undeclared_table fires

            rows = detect_schema_drift(store, MINI_CONTRACT)
            assert len(rows) == 1
            row = rows[0]
            assert row.drift_type == "undeclared_table"
            assert row.table_name == "mystery_table"
        finally:
            store.connection.close()


class TestPlantedMissingTable:
    def test_manifest_table_absent_live_is_missing_table(self):
        store = _bare_store()
        con = store.con
        try:
            _create_from_manifest(
                con,
                {"table_catalog": MINI_CONTRACT["table_catalog"], "widget": MINI_CONTRACT["widget"]},
            )
            _catalog(con, "table_catalog")
            _catalog(con, "widget")
            # widget_fact intentionally never created.

            rows = detect_schema_drift(store, MINI_CONTRACT)
            assert len(rows) == 1
            row = rows[0]
            assert row.drift_type == "missing_table"
            assert row.table_name == "widget_fact"
            assert row.column_name is None
        finally:
            store.connection.close()


class TestPlantedTypeMismatch:
    def test_widened_column_type_is_type_mismatch(self):
        store = _bare_store()
        con = store.con
        try:
            _create_from_manifest(con, {"table_catalog": MINI_CONTRACT["table_catalog"]})
            con.execute("CREATE TABLE widget (widget_id VARCHAR NOT NULL, label INTEGER)")
            _create_from_manifest(con, {"widget_fact": MINI_CONTRACT["widget_fact"]})
            for table_name in MINI_CONTRACT:
                _catalog(con, table_name)

            rows = detect_schema_drift(store, MINI_CONTRACT)
            assert len(rows) == 1
            row = rows[0]
            assert row.drift_type == "type_mismatch"
            assert row.table_name == "widget"
            assert row.column_name == "label"
            assert row.expected == "VARCHAR"
            assert row.actual == "INTEGER"
        finally:
            store.connection.close()


class TestPlantedNullabilityMismatch:
    def test_dropped_not_null_is_nullability_mismatch(self):
        store = _bare_store()
        con = store.con
        try:
            _create_from_manifest(con, {"table_catalog": MINI_CONTRACT["table_catalog"]})
            # widget_id is declared NOT NULL in the manifest; make it nullable live.
            con.execute("CREATE TABLE widget (widget_id VARCHAR, label VARCHAR)")
            _create_from_manifest(con, {"widget_fact": MINI_CONTRACT["widget_fact"]})
            for table_name in MINI_CONTRACT:
                _catalog(con, table_name)

            rows = detect_schema_drift(store, MINI_CONTRACT)
            assert len(rows) == 1
            row = rows[0]
            assert row.drift_type == "nullability_mismatch"
            assert row.table_name == "widget"
            assert row.column_name == "widget_id"
            assert row.expected == "False"
            assert row.actual == "True"
        finally:
            store.connection.close()


class TestEphemeralRelationsExcluded:
    def test_registered_relation_does_not_read_as_drift(self):
        import pandas as pd

        store = _matching_fixture()
        con = store.con
        try:
            con.register("_some_temp_seed", pd.DataFrame({"x": [1, 2]}))
            try:
                rows = detect_schema_drift(store, MINI_CONTRACT)
                assert rows == []
            finally:
                con.unregister("_some_temp_seed")
        finally:
            store.connection.close()


class TestSchemaContractSha256:
    def test_stable_across_repeated_calls(self):
        assert schema_contract_sha256(MINI_CONTRACT) == schema_contract_sha256(MINI_CONTRACT)

    def test_differs_when_manifest_changes(self):
        changed = {
            **MINI_CONTRACT,
            "widget": MINI_CONTRACT["widget"] + [ColumnSpec("new_col", "VARCHAR", nullable=True)],
        }
        assert schema_contract_sha256(MINI_CONTRACT) != schema_contract_sha256(changed)

    def test_insensitive_to_column_declaration_order(self):
        reordered = {
            "widget_fact": list(reversed(MINI_CONTRACT["widget_fact"])),
            "widget": list(reversed(MINI_CONTRACT["widget"])),
            "table_catalog": list(reversed(MINI_CONTRACT["table_catalog"])),
        }
        assert schema_contract_sha256(MINI_CONTRACT) == schema_contract_sha256(reordered)

    def test_insensitive_to_table_declaration_order(self):
        reordered = {
            "widget_fact": MINI_CONTRACT["widget_fact"],
            "table_catalog": MINI_CONTRACT["table_catalog"],
            "widget": MINI_CONTRACT["widget"],
        }
        assert schema_contract_sha256(MINI_CONTRACT) == schema_contract_sha256(reordered)

    def test_hash_is_a_well_formed_sha256_hex_digest(self):
        digest = schema_contract_sha256(MINI_CONTRACT)
        assert len(digest) == 64
        int(digest, 16)  # raises if not hex

    def test_default_contract_is_stable_across_independent_stores(self, tmp_store, fresh_store):
        """Stable across independent stores of the same schema: two isolated
        warehouse copies must agree on the manifest_sha256 persisted by
        migration 0097, and both must match schema_contract_sha256(build_contract_manifest(...))
        computed fresh against either store (PF2-S1 S1-2: migration 0097 now seeds from the
        full reconciled manifest, not the bare 6-table CONTRACT).
        """
        tmp_sha = tmp_store.con.execute("SELECT DISTINCT manifest_sha256 FROM schema_contract").fetchone()[0]
        fresh_sha = fresh_store.con.execute("SELECT DISTINCT manifest_sha256 FROM schema_contract").fetchone()[0]
        assert tmp_sha == fresh_sha == schema_contract_sha256(build_contract_manifest(tmp_store.con))


class TestMigration0097SchemaContractTable:
    def test_table_exists(self, tmp_store):
        count = tmp_store.con.execute(
            "SELECT count(*) FROM duckdb_tables() WHERE table_name = 'schema_contract'"
        ).fetchone()[0]
        assert count == 1

    def test_seeded_with_one_row_per_manifest_column(self, tmp_store):
        # PF2-S1 S1-2: migration 0097 seeds from build_contract_manifest() (every live
        # table, both schema paths reconciled), not the bare 6-table CONTRACT subset.
        expected = sum(len(specs) for specs in build_contract_manifest(tmp_store.con).values())
        actual = tmp_store.con.execute("SELECT count(*) FROM schema_contract").fetchone()[0]
        assert actual == expected
        # Sanity: the full manifest is a strict superset of CONTRACT's representative subset.
        assert expected > sum(len(specs) for specs in CONTRACT.values())

    def test_manifest_sha256_column_matches_schema_contract_sha256(self, tmp_store):
        rows = tmp_store.con.execute("SELECT DISTINCT manifest_sha256 FROM schema_contract").fetchall()
        assert len(rows) == 1
        assert rows[0][0] == schema_contract_sha256(build_contract_manifest(tmp_store.con))

    def test_declared_in_values_are_well_formed(self, tmp_store):
        rows = tmp_store.con.execute("SELECT DISTINCT declared_in FROM schema_contract").fetchall()
        values = {row[0] for row in rows}
        assert values <= {"schema_py", "migration"}
        # Both provenance tags are actually represented in the seeded subset.
        assert values == {"schema_py", "migration"}

    def test_migration_0097_recorded(self, tmp_store):
        versions = {
            row[0]
            for row in tmp_store.con.execute(
                "SELECT CAST(version AS INTEGER) FROM schema_migrations WHERE version ~ '^[0-9]+$'"
            ).fetchall()
        }
        assert 97 in versions, f"Migration 0097 not recorded; found: {sorted(versions)}"

    def test_migration_0098_recorded(self, tmp_store):
        versions = {
            row[0]
            for row in tmp_store.con.execute(
                "SELECT CAST(version AS INTEGER) FROM schema_migrations WHERE version ~ '^[0-9]+$'"
            ).fetchall()
        }
        assert 98 in versions, f"Migration 0098 not recorded; found: {sorted(versions)}"

    def test_migration_body_is_idempotent(self, tmp_store):
        from atx_db.migrations import _schema_contract_schema_catalog

        _schema_contract_schema_catalog(tmp_store.con)
        _schema_contract_schema_catalog(tmp_store.con)

        expected = sum(len(specs) for specs in build_contract_manifest(tmp_store.con).values())
        actual = tmp_store.con.execute("SELECT count(*) FROM schema_contract").fetchone()[0]
        assert actual == expected

        table_catalog_count = tmp_store.con.execute(
            "SELECT count(*) FROM table_catalog WHERE table_name = 'schema_contract'"
        ).fetchone()[0]
        assert table_catalog_count == 1

    def test_apply_pending_migrations_is_idempotent(self, tmp_store):
        from atx_db.migrations import apply_pending_migrations

        applied = apply_pending_migrations(tmp_store.con)
        assert applied == []


class TestMigration0098Indexes:
    def test_indexes_exist(self, tmp_store):
        rows = tmp_store.con.execute(
            "SELECT index_name FROM duckdb_indexes() WHERE table_name = 'schema_contract'"
        ).fetchall()
        index_names = {row[0] for row in rows}
        assert "idx_schema_contract_table_name" in index_names
        assert "idx_schema_contract_declared_in" in index_names


class TestCatalogRegistration:
    """schema_contract is catalogued like any other table per (B)."""

    def test_table_catalog_has_schema_contract_row(self, tmp_store):
        count = tmp_store.con.execute(
            "SELECT count(*) FROM table_catalog WHERE table_name = 'schema_contract'"
        ).fetchone()[0]
        assert count == 1

    def test_field_catalog_has_one_row_per_schema_contract_column(self, tmp_store):
        rows = tmp_store.con.execute(
            "SELECT column_name FROM duckdb_columns() WHERE table_name = 'schema_contract'"
        ).fetchall()
        live_columns = {row[0] for row in rows}

        rows = tmp_store.con.execute(
            "SELECT field_name FROM field_catalog WHERE table_name = 'schema_contract'"
        ).fetchall()
        catalogued = {row[0] for row in rows}
        assert catalogued == live_columns


class TestDetectSchemaDriftOverRealContractAndFullWarehouse:
    """Integration check: the real CONTRACT's declared tables/columns must exactly match
    the fully bootstrapped live warehouse wherever CONTRACT declares them today, even
    though CONTRACT does not yet cover every live table (that full-coverage reconciliation
    is PF2-S1 S1-2's job -- see the module docstring). undeclared_table/uncatalogued_table
    rows for tables outside CONTRACT's current representative subset are expected and are
    excluded from this assertion; anything else would mean CONTRACT disagrees with the
    live schema.py/migrations.py definitions it claims to mirror.
    """

    def test_declared_tables_have_zero_column_level_drift_against_live_warehouse(self, tmp_store):
        column_level_drift_types = {
            "missing_table",
            "undeclared_column",
            "missing_column",
            "type_mismatch",
            "nullability_mismatch",
            "missing_pit_column",
        }
        rows = detect_schema_drift(tmp_store, CONTRACT)
        offending = [row for row in rows if row.drift_type in column_level_drift_types]
        assert offending == [], f"CONTRACT disagrees with the live warehouse: {offending}"


def _live_tables(store: DuckDBStore) -> set[str]:
    rows = store.con.execute(
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


class TestBuildContractManifestCoverage:
    """PF2-S1 S1-2's headline acceptance: on a freshly bootstrapped DB (both
    ensure_quant_schema and every MIGRATIONS entry applied), build_contract_manifest()
    must cover EVERY live table -- zero residual in both directions. An incomplete
    manifest makes detect_schema_drift cry wolf on legitimate tables (see the sprint
    plan's risk note), which is exactly what S1-2 exists to close out.
    """

    def test_manifest_tables_exactly_match_live_tables(self, tmp_store):
        manifest = build_contract_manifest(tmp_store.con)
        live = _live_tables(tmp_store)
        assert set(manifest) == live
        # Spelled out both directions per the task's acceptance wording.
        assert set(manifest) - live == set()
        assert live - set(manifest) == set()

    def test_manifest_covers_representative_tables_from_both_paths(self, tmp_store):
        """Sanity spot-check: well-known tables from each path are present at all."""
        manifest = build_contract_manifest(tmp_store.con)
        # schema.py-only.
        assert "securities" in manifest
        # migrations.py-only.
        assert "fundamental_item" in manifest
        # connection.py's own bootstrap-time tables (the third, folded-into-schema_py path).
        assert "dataset_runs" in manifest
        assert "dataset_watermarks" in manifest
        assert "security_identifiers" in manifest

    def test_manifest_has_no_column_level_drift_against_the_live_warehouse(self, tmp_store):
        """detect_schema_drift over a fresh bootstrap with the COMPLETE manifest: no
        undeclared_table, no missing_table (coverage is exact), and no column-level
        drift (every column's shape is read straight off duckdb_columns(), so it cannot
        disagree with itself).
        """
        manifest = build_contract_manifest(tmp_store.con)
        rows = detect_schema_drift(tmp_store, manifest)
        by_type: dict[str, list[DriftRow]] = {}
        for row in rows:
            by_type.setdefault(row.drift_type, []).append(row)

        for drift_type in (
            "undeclared_table",
            "missing_table",
            "undeclared_column",
            "missing_column",
            "type_mismatch",
            "nullability_mismatch",
            "missing_pit_column",
        ):
            assert by_type.get(drift_type, []) == [], f"unexpected {drift_type}: {by_type.get(drift_type)}"


class TestBuildContractManifestDeclaredInAttribution:
    """declared_in must correctly attribute each table to schema_py vs migration --
    reconciling BOTH imperative schema paths, not just the ones CONTRACT already covers.
    """

    def _declared_in_values(self, manifest, table_name: str) -> set[str]:
        return {spec.declared_in for spec in manifest[table_name]}

    def test_schema_py_only_table_is_declared_in_schema_py(self, tmp_store):
        # `dataset_catalog` is created only by schema.py::ensure_quant_schema, with no
        # migration ever touching its columns either -- a clean, unmixed schema_py table.
        manifest = build_contract_manifest(tmp_store.con)
        assert self._declared_in_values(manifest, "dataset_catalog") == {"schema_py"}

    def test_schema_py_table_with_a_later_migration_added_column_splits_at_column_level(self, tmp_store):
        # `securities` is created by schema.py, but migrations.py separately ADDs
        # `entity_id` to it later -- so declared_in splits within this one table.
        manifest = build_contract_manifest(tmp_store.con)
        specs = {spec.name: spec for spec in manifest["securities"]}
        assert specs["entity_id"].declared_in == "migration"
        assert specs["security_id"].declared_in == "schema_py"

    def test_migration_only_table_is_declared_in_migration(self, tmp_store):
        # `fundamental_item` is created only inside a migrations.py MIGRATIONS entry;
        # schema.py never mentions it.
        manifest = build_contract_manifest(tmp_store.con)
        assert self._declared_in_values(manifest, "fundamental_item") == {"migration"}

    def test_connection_py_bootstrap_tables_are_declared_in_schema_py(self, tmp_store):
        # dataset_runs/dataset_watermarks/security_identifiers are created directly by
        # connection.py::DuckDBStore.initialize() -- the same unversioned "always run"
        # bootstrap semantics as schema.py, just physically colocated elsewhere.
        manifest = build_contract_manifest(tmp_store.con)
        for table_name in ("dataset_runs", "dataset_watermarks", "security_identifiers"):
            assert self._declared_in_values(manifest, table_name) == {"schema_py"}

    def test_table_declared_in_both_paths_is_attributed_to_migration(self, tmp_store):
        # xbrl_validation_results is created by BOTH schema.py's ensure_quant_schema AND
        # a registered migration (migration "xbrl_validation_results", S4d) -- a legacy
        # table later "rebaselined" into schema.py for fresh-bootstrap speed. migration
        # wins on overlap, matching the market_cap precedent CONTRACT already set.
        manifest = build_contract_manifest(tmp_store.con)
        assert self._declared_in_values(manifest, "xbrl_validation_results") == {"migration"}

    def test_column_added_by_migration_to_a_schema_py_table_is_migration_at_column_level(self, tmp_store):
        # security_identifier_history is created by schema.py, but its own CREATE TABLE
        # already inlines available_at/run_id -- both ALSO explicitly ADDed by
        # migrations.py::_schema_evolution_alters. declared_in is a column-level fact
        # (per the module docstring): those two columns are migration-declared, while
        # the table's other columns (created only by schema.py) stay schema_py.
        manifest = build_contract_manifest(tmp_store.con)
        specs = {spec.name: spec for spec in manifest["security_identifier_history"]}
        assert specs["available_at"].declared_in == "migration"
        assert specs["run_id"].declared_in == "migration"
        assert specs["security_id"].declared_in == "schema_py"
        assert specs["as_of_date"].declared_in == "schema_py"
        assert specs["source_loaded_at"].declared_in == "schema_py"

    def test_fstring_helper_created_table_base_columns_are_migration(self, tmp_store):
        # fundamental_statement_map is created by BOTH schema.py (direct CREATE) AND a
        # migration -- but the migration creates it via an f-string-templated helper
        # (_create_fundamental_statement_map_table(conn, "fundamental_statement_map"),
        # `CREATE TABLE IF NOT EXISTS {table_name}`) that the literal CREATE-TABLE regex
        # cannot see. _MIGRATION_HELPER_TABLE_RE catches the helper call site so this
        # genuine overlap table is attributed migration (migration wins), including its
        # BASE columns (source, taxonomy, concept, ...), not just its 4 ALTER-added ones.
        manifest = build_contract_manifest(tmp_store.con)
        specs = {spec.name: spec for spec in manifest["fundamental_statement_map"]}
        for base_col in ("source", "taxonomy", "concept", "statement_type", "canonical_metric"):
            assert specs[base_col].declared_in == "migration", base_col
        # And the ALTER-added ones remain migration too.
        for added_col in ("item_id", "industry_template", "is_derived", "derivation_expr"):
            assert specs[added_col].declared_in == "migration", added_col

    def test_fstring_helper_scratch_temp_table_is_not_captured_as_live(self, tmp_store):
        # The helper is also called with a VARIABLE (`scratch` = the rekey temp table),
        # not a string literal -- _MIGRATION_HELPER_TABLE_RE deliberately skips it, and it
        # is dropped within the migration, so it must not appear in the manifest at all.
        from atx_db.schema_contract import _scan_declared_in_sources

        _schema_py, migration_tables, _added = _scan_declared_in_sources()
        assert "fundamental_statement_map_rekey" not in migration_tables
        manifest = build_contract_manifest(tmp_store.con)
        assert "fundamental_statement_map_rekey" not in manifest


class TestBuildContractManifestPitColumnScoping:
    """is_pit_column must be scoped to genuine fact/derived tables. A table counts as
    fact/derived only if it carries a STRONG bitemporal marker (as_of_date / available_at
    / is_latest_revision) -- the ubiquitous bookkeeping run_id/source_loaded_at alone do
    NOT qualify (nearly every table has them). A blind name-match, OR the earlier
    {bronze,silver,gold} layer proxy, over-marks ~100 / ~37 dimension/master/landing
    tables and would feed false positives into S1-1's PIT-presence gate -- the exact
    "cries wolf" failure the sprint plan warns against.
    """

    def _pit_cols(self, manifest, table_name: str) -> set[str]:
        return {spec.name for spec in manifest[table_name] if spec.is_pit_column}

    def test_control_and_audit_tables_have_no_pit_columns(self, tmp_store):
        manifest = build_contract_manifest(tmp_store.con)
        # These all carry run_id and/or source_loaded_at but no strong temporal marker --
        # none should be marked a PIT column.
        for table_name in (
            "dataset_runs",       # control (connection.py bootstrap)
            "etl_job_runs",       # control
            "etl_job_steps",      # control
            "etl_job_audit",      # audit
            "lake_export_runs",   # audit
            "schema_contract",    # control (this sprint's own table)
        ):
            assert self._pit_cols(manifest, table_name) == set(), table_name

    def test_dimension_master_landing_tables_have_no_pit_columns(self, tmp_store):
        # The reviewer's set: tables the {bronze,silver,gold} layer proxy wrongly flagged
        # as fact. They are dimension/master/landing tables that legitimately lack the
        # strong bitemporal markers, so under the temporal-marker rule they carry NO PIT
        # columns. Pin the residual so a broadening of the fact test can't silently
        # re-introduce the false positives.
        manifest = build_contract_manifest(tmp_store.con)
        for table_name in (
            "trading_calendar",       # only source_loaded_at
            "insider",                # run_id + source_loaded_at, no marker
            "fund",                   # run_id + source_loaded_at, no marker
            "fund_class",             # run_id + source_loaded_at, no marker
            "thirteenf_managers",     # run_id + source_loaded_at, no marker
            "universes",              # no PIT-named columns at all
            "formula_registry",       # CONTRACT: registry, run_id/source_loaded_at NOT PIT
            "sec_submissions",        # landing table, no marker
            "raw_source_files",       # landing table, no marker
            "xbrl_filing_facts",      # raw landing, no marker
            "xbrl_filing_contexts",   # raw landing, no marker
            "taxonomy",               # reference, no marker
        ):
            assert self._pit_cols(manifest, table_name) == set(), table_name

    def test_run_id_and_source_loaded_at_on_a_non_fact_table_are_not_pit(self, tmp_store):
        manifest = build_contract_manifest(tmp_store.con)
        for table_name in ("dataset_runs", "insider", "fund", "thirteenf_managers"):
            specs = {spec.name: spec for spec in manifest[table_name]}
            for col in ("run_id", "source_loaded_at"):
                if col in specs:
                    assert specs[col].is_pit_column is False, f"{table_name}.{col}"

    def test_formula_registry_pit_scoping_matches_contract(self, tmp_store):
        # formula_registry (a registry with NO strong temporal marker) is one of CONTRACT's
        # 6 tables, reused verbatim -- its run_id/source_loaded_at are deliberately NOT PIT.
        # The temporal-marker rule reproduces exactly that split, so the verbatim CONTRACT
        # value and the rule agree: zero PIT columns.
        manifest = build_contract_manifest(tmp_store.con)
        assert self._pit_cols(manifest, "formula_registry") == set()

    def test_fact_table_marks_its_canonical_pit_columns(self, tmp_store):
        # market_cap (carries available_at/as_of_date/is_latest_revision) is one of
        # CONTRACT's 6 tables -- reused verbatim -- and marks all five canonical PIT
        # columns. Assert the reconciled manifest preserves that.
        manifest = build_contract_manifest(tmp_store.con)
        assert self._pit_cols(manifest, "market_cap") == set(PIT_COLUMN_NAMES)

    def test_non_contract_fact_table_marks_present_pit_columns(self, tmp_store):
        # valuation_multiples is NOT in CONTRACT, so is_pit_column is derived by the
        # temporal-marker rule: it carries as_of_date/available_at/is_latest_revision (so
        # it is fact), and every canonical PIT column it physically has must be marked.
        manifest = build_contract_manifest(tmp_store.con)
        live_cols = {
            row[0]
            for row in tmp_store.con.execute(
                "SELECT column_name FROM duckdb_columns() WHERE table_name = 'valuation_multiples'"
            ).fetchall()
        }
        expected = {name for name in PIT_COLUMN_NAMES if name in live_cols}
        assert self._pit_cols(manifest, "valuation_multiples") == expected
        assert expected  # sanity: it really does have PIT columns to mark

    def test_every_pit_marked_table_actually_carries_a_strong_temporal_marker(self, tmp_store):
        # The invariant behind the rule: a table may only have PIT-marked columns if it
        # physically carries at least one strong marker. Zero residual otherwise.
        from atx_db.schema_contract import _STRONG_TEMPORAL_MARKERS

        manifest = build_contract_manifest(tmp_store.con)
        offenders = []
        for table_name, specs in manifest.items():
            col_names = {spec.name for spec in specs}
            has_pit = any(spec.is_pit_column for spec in specs)
            has_marker = any(marker in col_names for marker in _STRONG_TEMPORAL_MARKERS)
            if has_pit and not has_marker:
                offenders.append(table_name)
        assert offenders == [], f"PIT-marked tables lacking a strong temporal marker: {offenders}"


class TestBuildContractManifestConsistencyWithHandCuratedContract:
    """The S1-0 hand-written 6-table CONTRACT and the S1-2 complete manifest must never
    disagree on the structural fields CONTRACT already declares. S2-1 may fill missing
    semantic fields from field_catalog, while preserving explicit semantic declarations.
    """

    def test_contract_tables_preserve_hand_curated_declarations(self, tmp_store):
        manifest = build_contract_manifest(tmp_store.con)
        for table_name, specs in CONTRACT.items():
            assert table_name in manifest
            actual = {spec.name: spec for spec in manifest[table_name]}
            for expected in specs:
                assert expected.name in actual
                got = actual[expected.name]
                assert got.data_type == expected.data_type
                assert got.nullable == expected.nullable
                assert got.is_natural_key == expected.is_natural_key
                assert got.is_pit_column == expected.is_pit_column
                assert got.declared_in == expected.declared_in
                assert got.natural_key == expected.natural_key
                if expected.unit is not None:
                    assert got.unit == expected.unit
                if expected.sign is not None:
                    assert got.sign == expected.sign
                if expected.scale is not None:
                    assert got.scale == expected.scale

    def test_contract_declared_in_values_agree_with_manifest(self, tmp_store):
        manifest = build_contract_manifest(tmp_store.con)
        for table_name, specs in CONTRACT.items():
            expected = {spec.name: spec.declared_in for spec in specs}
            actual = {spec.name: spec.declared_in for spec in manifest[table_name]}
            assert actual == expected, f"{table_name}: manifest disagrees with CONTRACT's declared_in"


class TestBuildContractManifestDeterminism:
    def test_deterministic_given_the_same_bootstrapped_db(self, tmp_store):
        first = build_contract_manifest(tmp_store.con)
        second = build_contract_manifest(tmp_store.con)
        assert first == second

    def test_agrees_across_independent_warehouses(self, tmp_store, fresh_store):
        """Independent stores with the same schema must derive the same manifest."""
        from_tmp = build_contract_manifest(tmp_store.con)
        from_fresh = build_contract_manifest(fresh_store.con)
        assert set(from_tmp) == set(from_fresh)
        for table_name in from_tmp:
            assert from_tmp[table_name] == from_fresh[table_name], table_name


# ---------------------------------------------------------------------------
# PF2-S1 S1-3: warehouse data-catalog as-of reader + CLI.
#
# Migration 0099 adds v_warehouse_catalog (one row per table_name/field_name,
# LEFT JOIN table_catalog + field_catalog, plus best-effort v_formula_registry
# lineage), catalogued with its own table_catalog/field_catalog rows exactly
# like the v_formula_registry precedent (see test_formula_registry_catalog.py).
#
# CRITICAL PIT difference from formula_registry_asof: table_catalog/field_catalog
# carry `updated_at` (knowledge time), not valid_from/valid_to DEFINITION
# validity, so warehouse_catalog_asof DOES use as_of_ts -- a catalog row updated
# AFTER the as-of instant must be excluded (no lookahead). All as-of-date
# literals below use safely future dates (2026+) relative to any real bootstrap
# `now()` seeded at test-run time, so real catalog rows (e.g. `table_catalog`
# itself) are never accidentally excluded by the gate.
# ---------------------------------------------------------------------------

WAREHOUSE_CATALOG_VIEW_COLUMNS = (
    "table_name",
    "layer",
    "entity",
    "grain",
    "table_description",
    "natural_key_json",
    "pit_notes",
    "table_updated_at",
    "field_name",
    "semantic_type",
    "field_description",
    "field_nullable",
    "field_unit",
    "source_field",
    "field_updated_at",
    "formula_code",
    "formula_family",
    "formula_kind",
    "formula_unit",
    "formula_expression",
    "formula_citation",
    "formula_valid_from",
    "formula_valid_to",
)


def _insert_table_catalog_row(
    con: duckdb.DuckDBPyConnection,
    table_name: str,
    *,
    layer: str = "gold",
    entity: str = "test",
    updated_at: dt.datetime,
) -> None:
    con.execute(
        """
        INSERT INTO table_catalog (
            table_name, layer, entity, grain, description, natural_key_json, pit_notes, updated_at
        )
        VALUES (?, ?, ?, ?, ?, ?, ?, ?)
        """,
        [
            table_name,
            layer,
            entity,
            f"{table_name}_id",
            f"Test table {table_name}.",
            f'["{table_name}_id"]',
            "Test.",
            updated_at,
        ],
    )


def _insert_field_catalog_row(
    con: duckdb.DuckDBPyConnection,
    table_name: str,
    field_name: str,
    *,
    updated_at: dt.datetime,
) -> None:
    con.execute(
        """
        INSERT INTO field_catalog (
            table_name, field_name, semantic_type, description, nullable, unit, source_field, updated_at
        )
        VALUES (?, ?, 'text', 'Test field.', true, NULL, NULL, ?)
        """,
        [table_name, field_name, updated_at],
    )


class TestMigration0099WarehouseCatalogView:
    def test_view_exists(self, tmp_store):
        count = tmp_store.con.execute(
            "SELECT count(*) FROM duckdb_views() WHERE view_name = 'v_warehouse_catalog'"
        ).fetchone()[0]
        assert count == 1

    def test_view_is_not_a_base_table(self, tmp_store):
        # Catalogued like a table per (B)/S7a, but the underlying object must be a VIEW.
        count = tmp_store.con.execute(
            "SELECT count(*) FROM duckdb_tables() WHERE table_name = 'v_warehouse_catalog'"
        ).fetchone()[0]
        assert count == 0

    def test_view_columns(self, tmp_store):
        rows = tmp_store.con.execute(
            """
            SELECT column_name
            FROM information_schema.columns
            WHERE table_schema = 'main' AND table_name = 'v_warehouse_catalog'
            ORDER BY ordinal_position
            """
        ).fetchall()
        assert [row[0] for row in rows] == list(WAREHOUSE_CATALOG_VIEW_COLUMNS)

    def test_migration_0099_recorded(self, tmp_store):
        versions = {
            row[0]
            for row in tmp_store.con.execute(
                "SELECT CAST(version AS INTEGER) FROM schema_migrations WHERE version ~ '^[0-9]+$'"
            ).fetchall()
        }
        assert 99 in versions, f"Migration 0099 not recorded; found: {sorted(versions)}"

    def test_migration_body_is_idempotent(self, tmp_store):
        from atx_db.migrations import _warehouse_catalog_view

        _warehouse_catalog_view(tmp_store.con)
        _warehouse_catalog_view(tmp_store.con)

        table_catalog_count = tmp_store.con.execute(
            "SELECT count(*) FROM table_catalog WHERE table_name = 'v_warehouse_catalog'"
        ).fetchone()[0]
        assert table_catalog_count == 1

        field_catalog_count = tmp_store.con.execute(
            "SELECT count(*) FROM field_catalog WHERE table_name = 'v_warehouse_catalog'"
        ).fetchone()[0]
        assert field_catalog_count == len(WAREHOUSE_CATALOG_VIEW_COLUMNS)

    def test_apply_pending_migrations_is_idempotent(self, tmp_store):
        from atx_db.migrations import apply_pending_migrations

        applied = apply_pending_migrations(tmp_store.con)
        assert applied == []

    def test_migration_body_without_formula_registry_view_keeps_null_formula_columns(self):
        from atx_db.migrations import _warehouse_catalog_view

        store = _bare_store()
        try:
            store.con.execute(
                """
                CREATE TABLE table_catalog (
                    table_name VARCHAR PRIMARY KEY,
                    layer VARCHAR,
                    entity VARCHAR,
                    grain VARCHAR,
                    description VARCHAR,
                    natural_key_json VARCHAR,
                    pit_notes VARCHAR,
                    updated_at TIMESTAMP
                )
                """
            )
            store.con.execute(
                """
                CREATE TABLE field_catalog (
                    table_name VARCHAR NOT NULL,
                    field_name VARCHAR NOT NULL,
                    semantic_type VARCHAR,
                    description VARCHAR,
                    nullable BOOLEAN,
                    unit VARCHAR,
                    source_field VARCHAR,
                    updated_at TIMESTAMP,
                    PRIMARY KEY (table_name, field_name)
                )
                """
            )
            _insert_table_catalog_row(
                store.con,
                "test_wca_no_formula_registry",
                entity="formula",
                updated_at=dt.datetime(2020, 1, 1),
            )
            _insert_field_catalog_row(
                store.con,
                "test_wca_no_formula_registry",
                "missing_formula",
                updated_at=dt.datetime(2020, 1, 1),
            )

            _warehouse_catalog_view(store.con)

            row = store.con.execute(
                """
                SELECT formula_code, formula_family, formula_valid_from
                FROM v_warehouse_catalog
                WHERE table_name = 'test_wca_no_formula_registry'
                """
            ).fetchone()
            assert row == (None, None, None)

            formula_column_types = dict(
                store.con.execute(
                    """
                    SELECT column_name, data_type
                    FROM information_schema.columns
                    WHERE table_schema = 'main'
                      AND table_name = 'v_warehouse_catalog'
                      AND column_name IN ('formula_code', 'formula_family', 'formula_valid_from')
                    """
                ).fetchall()
            )
            assert formula_column_types == {
                "formula_code": "VARCHAR",
                "formula_family": "VARCHAR",
                "formula_valid_from": "DATE",
            }
        finally:
            store.connection.close()


class TestWarehouseCatalogViewCatalogRegistration:
    """Views are catalogued like tables per (B)/S7a."""

    def test_table_catalog_has_view_row(self, tmp_store):
        count = tmp_store.con.execute(
            "SELECT count(*) FROM table_catalog WHERE table_name = 'v_warehouse_catalog'"
        ).fetchone()[0]
        assert count == 1

    def test_field_catalog_has_one_row_per_view_column(self, tmp_store):
        rows = tmp_store.con.execute(
            "SELECT field_name FROM field_catalog WHERE table_name = 'v_warehouse_catalog'"
        ).fetchall()
        catalogued = {row[0] for row in rows}
        expected = set(WAREHOUSE_CATALOG_VIEW_COLUMNS)
        assert catalogued == expected, (
            f"v_warehouse_catalog field_catalog mismatch; "
            f"missing={sorted(expected - catalogued)}, extra={sorted(catalogued - expected)}"
        )

    def test_view_carries_its_own_catalog_rows(self, tmp_store):
        """S1-3 acceptance: "view carries its own catalog rows" -- v_warehouse_catalog
        catalogs itself, and those rows are themselves selectable through the view.
        """
        rows = tmp_store.con.execute(
            "SELECT field_name FROM v_warehouse_catalog WHERE table_name = 'v_warehouse_catalog'"
        ).fetchall()
        fields = {row[0] for row in rows}
        assert fields == set(WAREHOUSE_CATALOG_VIEW_COLUMNS)


class TestWarehouseCatalogAsofReader:
    """warehouse_catalog_asof(as_of_date, ..., tables=, layers=) in db/asof.py."""

    def test_returns_table_and_field_rows_filterable_by_tables(self, tmp_store):
        from atx_db.asof import warehouse_catalog_asof

        _insert_table_catalog_row(
            tmp_store.con, "test_wca_alpha", layer="gold", updated_at=dt.datetime(2020, 1, 1)
        )
        _insert_field_catalog_row(
            tmp_store.con, "test_wca_alpha", "col_a", updated_at=dt.datetime(2020, 1, 1)
        )
        _insert_table_catalog_row(
            tmp_store.con, "test_wca_beta", layer="silver", updated_at=dt.datetime(2020, 1, 1)
        )
        _insert_field_catalog_row(
            tmp_store.con, "test_wca_beta", "col_b", updated_at=dt.datetime(2020, 1, 1)
        )

        result = warehouse_catalog_asof(
            dt.date(2030, 1, 1), store=tmp_store, tables=["test_wca_alpha"]
        )
        assert set(result["table_name"]) == {"test_wca_alpha"}
        assert set(result["field_name"]) == {"col_a"}

    def test_filters_by_layers(self, tmp_store):
        from atx_db.asof import warehouse_catalog_asof

        _insert_table_catalog_row(
            tmp_store.con, "test_wca_alpha", layer="gold", updated_at=dt.datetime(2020, 1, 1)
        )
        _insert_table_catalog_row(
            tmp_store.con, "test_wca_beta", layer="silver", updated_at=dt.datetime(2020, 1, 1)
        )

        result = warehouse_catalog_asof(dt.date(2030, 1, 1), store=tmp_store, layers=["silver"])
        table_names = set(result["table_name"])
        assert "test_wca_beta" in table_names
        assert "test_wca_alpha" not in table_names

    def test_table_with_no_field_rows_still_appears(self, tmp_store):
        from atx_db.asof import warehouse_catalog_asof

        _insert_table_catalog_row(
            tmp_store.con, "test_wca_no_fields", updated_at=dt.datetime(2020, 1, 1)
        )

        result = warehouse_catalog_asof(
            dt.date(2030, 1, 1), store=tmp_store, tables=["test_wca_no_fields"]
        )
        assert len(result) == 1
        assert result["field_name"].isna().iloc[0]

    def test_result_ordered_by_table_name_then_field_name(self, tmp_store):
        from atx_db.asof import warehouse_catalog_asof

        _insert_table_catalog_row(
            tmp_store.con, "test_wca_zzz", updated_at=dt.datetime(2020, 1, 1)
        )
        _insert_field_catalog_row(
            tmp_store.con, "test_wca_zzz", "b_col", updated_at=dt.datetime(2020, 1, 1)
        )
        _insert_field_catalog_row(
            tmp_store.con, "test_wca_zzz", "a_col", updated_at=dt.datetime(2020, 1, 1)
        )
        _insert_table_catalog_row(
            tmp_store.con, "test_wca_aaa", updated_at=dt.datetime(2020, 1, 1)
        )
        _insert_field_catalog_row(
            tmp_store.con, "test_wca_aaa", "only_col", updated_at=dt.datetime(2020, 1, 1)
        )

        result = warehouse_catalog_asof(
            dt.date(2030, 1, 1),
            store=tmp_store,
            tables=["test_wca_zzz", "test_wca_aaa"],
        )
        rows = list(zip(result["table_name"], result["field_name"]))
        assert rows == [
            ("test_wca_aaa", "only_col"),
            ("test_wca_zzz", "a_col"),
            ("test_wca_zzz", "b_col"),
        ]

    def test_opens_own_connection_when_no_store_given(self, tmp_store):
        from atx_db.asof import warehouse_catalog_asof

        db_path = tmp_store.path
        tmp_store.connection.close()
        tmp_store.connection = None

        result = warehouse_catalog_asof(dt.date(2030, 1, 1), db_path=db_path, tables=["table_catalog"])
        assert not result.empty
        assert set(result["table_name"]) == {"table_catalog"}


class TestWarehouseCatalogAsofNoLookahead:
    """CRITICAL PIT gate (differs from formula_registry_asof): table_catalog/field_catalog
    carry `updated_at` (knowledge time), not valid_from/valid_to. A catalog row updated
    AFTER the as-of instant must be excluded -- no lookahead.
    """

    def test_table_row_future_updated_at_excludes_before_and_includes_after(self, tmp_store):
        from atx_db.asof import warehouse_catalog_asof

        future_updated_at = dt.datetime(2035, 6, 15, 12, 0, 0)
        _insert_table_catalog_row(
            tmp_store.con, "test_wca_lookahead", updated_at=future_updated_at
        )

        before = warehouse_catalog_asof(
            dt.date(2035, 6, 14), store=tmp_store, tables=["test_wca_lookahead"]
        )
        assert before.empty, (
            "a table_catalog row updated_at AFTER the as-of instant must be excluded (no lookahead)"
        )

        after = warehouse_catalog_asof(
            dt.date(2035, 6, 16), store=tmp_store, tables=["test_wca_lookahead"]
        )
        assert len(after) == 1
        assert after.iloc[0]["table_name"] == "test_wca_lookahead"

    def test_field_row_future_updated_at_excludes_before_and_includes_after(self, tmp_store):
        from atx_db.asof import warehouse_catalog_asof

        base_updated_at = dt.datetime(2020, 1, 1)
        future_field_updated_at = dt.datetime(2035, 6, 15)
        _insert_table_catalog_row(
            tmp_store.con, "test_wca_field_lookahead", updated_at=base_updated_at
        )
        _insert_field_catalog_row(
            tmp_store.con, "test_wca_field_lookahead", "old_col", updated_at=base_updated_at
        )
        _insert_field_catalog_row(
            tmp_store.con,
            "test_wca_field_lookahead",
            "future_col",
            updated_at=future_field_updated_at,
        )

        before = warehouse_catalog_asof(
            dt.date(2035, 6, 14), store=tmp_store, tables=["test_wca_field_lookahead"]
        )
        # The table row's own updated_at is old (visible), but the future-dated field
        # row must be excluded while the old field row remains.
        assert set(before["field_name"]) == {"old_col"}

        after = warehouse_catalog_asof(
            dt.date(2035, 6, 16), store=tmp_store, tables=["test_wca_field_lookahead"]
        )
        assert set(after["field_name"]) == {"old_col", "future_col"}

    def test_table_remains_visible_when_only_field_rows_are_future_dated(self, tmp_store):
        from atx_db.asof import warehouse_catalog_asof

        _insert_table_catalog_row(
            tmp_store.con,
            "test_wca_only_future_fields",
            updated_at=dt.datetime(2020, 1, 1),
        )
        _insert_field_catalog_row(
            tmp_store.con,
            "test_wca_only_future_fields",
            "future_col",
            updated_at=dt.datetime(2035, 6, 15),
        )

        before = warehouse_catalog_asof(
            dt.date(2035, 6, 14), store=tmp_store, tables=["test_wca_only_future_fields"]
        )
        assert len(before) == 1
        assert before.iloc[0]["table_name"] == "test_wca_only_future_fields"
        assert before["field_name"].isna().iloc[0]

        after = warehouse_catalog_asof(
            dt.date(2035, 6, 16), store=tmp_store, tables=["test_wca_only_future_fields"]
        )
        assert len(after) == 1
        assert after.iloc[0]["field_name"] == "future_col"


class TestWarehouseCatalogFormulaLineage:
    """Best-effort v_formula_registry lineage join for catalogued formula surfaces
    (table_catalog.entity = 'formula'), keyed on field_catalog.field_name =
    v_formula_registry.formula_code.
    """

    def test_entity_formula_row_joins_matching_formula_registry_code(self, tmp_store):
        from atx_db.asof import warehouse_catalog_asof

        tmp_store.con.execute(
            """
            INSERT INTO formula_registry (
                formula_code, family, kind, unit, numerator_code, denominator_code,
                numerator_item_ids_json, denominator_item_ids_json, inputs_json,
                transform, expression, is_meaningful_rule, definition, citation,
                valid_from, valid_to
            )
            VALUES (
                'test_wca_formula', 'profitability', 'ratio', 'ratio', 'net_income', 'revenue',
                NULL, NULL, '["ni", "rev"]',
                'divide', NULL, NULL, 'Test formula.', 'Test citation, Journal of Testing.',
                DATE '2000-01-01', NULL
            )
            """
        )
        _insert_table_catalog_row(
            tmp_store.con,
            "test_wca_formula_surface",
            entity="formula",
            updated_at=dt.datetime(2020, 1, 1),
        )
        _insert_field_catalog_row(
            tmp_store.con,
            "test_wca_formula_surface",
            "test_wca_formula",
            updated_at=dt.datetime(2020, 1, 1),
        )

        result = warehouse_catalog_asof(
            dt.date(2030, 1, 1), store=tmp_store, tables=["test_wca_formula_surface"]
        )
        assert len(result) == 1
        row = result.iloc[0]
        assert row["formula_code"] == "test_wca_formula"
        assert row["formula_family"] == "profitability"
        assert row["formula_kind"] == "ratio"
        assert row["formula_citation"] == "Test citation, Journal of Testing."

    def test_non_formula_entity_leaves_formula_columns_null(self, tmp_store):
        from atx_db.asof import warehouse_catalog_asof

        _insert_table_catalog_row(
            tmp_store.con, "test_wca_non_formula", entity="fact", updated_at=dt.datetime(2020, 1, 1)
        )
        _insert_field_catalog_row(
            tmp_store.con, "test_wca_non_formula", "some_col", updated_at=dt.datetime(2020, 1, 1)
        )

        result = warehouse_catalog_asof(
            dt.date(2030, 1, 1), store=tmp_store, tables=["test_wca_non_formula"]
        )
        assert len(result) == 1
        assert result["formula_code"].isna().iloc[0]

    def test_formula_lineage_is_null_before_formula_valid_from(self, tmp_store):
        from atx_db.asof import warehouse_catalog_asof

        tmp_store.con.execute(
            """
            INSERT INTO formula_registry (
                formula_code, family, kind, unit, numerator_code, denominator_code,
                numerator_item_ids_json, denominator_item_ids_json, inputs_json,
                transform, expression, is_meaningful_rule, definition, citation,
                valid_from, valid_to
            )
            VALUES (
                'test_wca_future_formula', 'profitability', 'ratio', 'ratio', 'net_income', 'revenue',
                NULL, NULL, '["ni", "rev"]',
                'divide', NULL, NULL, 'Future formula.', 'Future citation.',
                DATE '2035-01-01', NULL
            )
            """
        )
        _insert_table_catalog_row(
            tmp_store.con,
            "test_wca_future_formula_surface",
            entity="formula",
            updated_at=dt.datetime(2020, 1, 1),
        )
        _insert_field_catalog_row(
            tmp_store.con,
            "test_wca_future_formula_surface",
            "test_wca_future_formula",
            updated_at=dt.datetime(2020, 1, 1),
        )

        before = warehouse_catalog_asof(
            dt.date(2034, 12, 31), store=tmp_store, tables=["test_wca_future_formula_surface"]
        )
        assert len(before) == 1
        assert before.iloc[0]["field_name"] == "test_wca_future_formula"
        assert before["formula_code"].isna().iloc[0]

        after = warehouse_catalog_asof(
            dt.date(2035, 1, 1), store=tmp_store, tables=["test_wca_future_formula_surface"]
        )
        assert len(after) == 1
        assert after.iloc[0]["formula_code"] == "test_wca_future_formula"

    def test_formula_lineage_is_null_after_formula_valid_to(self, tmp_store):
        from atx_db.asof import warehouse_catalog_asof

        tmp_store.con.execute(
            """
            INSERT INTO formula_registry (
                formula_code, family, kind, unit, numerator_code, denominator_code,
                numerator_item_ids_json, denominator_item_ids_json, inputs_json,
                transform, expression, is_meaningful_rule, definition, citation,
                valid_from, valid_to
            )
            VALUES (
                'test_wca_retired_formula', 'profitability', 'ratio', 'ratio', 'net_income', 'revenue',
                NULL, NULL, '["ni", "rev"]',
                'divide', NULL, NULL, 'Retired formula.', 'Retired citation.',
                DATE '2000-01-01', DATE '2030-01-01'
            )
            """
        )
        _insert_table_catalog_row(
            tmp_store.con,
            "test_wca_retired_formula_surface",
            entity="formula",
            updated_at=dt.datetime(2020, 1, 1),
        )
        _insert_field_catalog_row(
            tmp_store.con,
            "test_wca_retired_formula_surface",
            "test_wca_retired_formula",
            updated_at=dt.datetime(2020, 1, 1),
        )

        active = warehouse_catalog_asof(
            dt.date(2029, 12, 31), store=tmp_store, tables=["test_wca_retired_formula_surface"]
        )
        assert len(active) == 1
        assert active.iloc[0]["formula_code"] == "test_wca_retired_formula"

        retired = warehouse_catalog_asof(
            dt.date(2030, 1, 1), store=tmp_store, tables=["test_wca_retired_formula_surface"]
        )
        assert len(retired) == 1
        assert retired.iloc[0]["field_name"] == "test_wca_retired_formula"
        assert retired["formula_code"].isna().iloc[0]


class TestWarehouseCatalogCli:
    """Thin `python -m db.asof warehouse-catalog` CLI (db/asof.py main())."""

    def test_main_warehouse_catalog_subcommand_prints_resolved_catalog(self, tmp_store, capsys):
        from atx_db.asof import main as asof_main

        db_path = tmp_store.path
        tmp_store.connection.close()
        tmp_store.connection = None

        exit_code = asof_main(
            [
                "warehouse-catalog",
                "--as-of",
                "2030-01-01",
                "--db-path",
                str(db_path),
                "--tables",
                "table_catalog",
            ]
        )
        assert exit_code == 0
        captured = capsys.readouterr()
        assert "table_catalog" in captured.out

    def test_main_requires_a_command(self):
        from atx_db.asof import main as asof_main

        with pytest.raises(SystemExit):
            asof_main([])
