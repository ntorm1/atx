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

import duckdb
import pytest

from db.connection import DuckDBStore
from db.schema_contract import (
    CONTRACT,
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

    def test_default_contract_is_stable_across_rebootstraps(self, tmp_store, fresh_store):
        """Stable across re-bootstraps of the same schema: two independently bootstrapped
        warehouses -- the fast template-copy tmp_store and the fully independent
        fresh_store bootstrap path (conftest.py's _build_template vs a real
        DuckDBStore.__enter__ run) -- must agree on the manifest_sha256 persisted by
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
        from db.migrations import _schema_contract_schema_catalog

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
        from db.migrations import apply_pending_migrations

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


class TestBuildContractManifestConsistencyWithHandCuratedContract:
    """The S1-0 hand-written 6-table CONTRACT and the S1-2 complete manifest must never
    disagree on the tables CONTRACT already declares -- build_contract_manifest() reuses
    CONTRACT verbatim for exactly this reason.
    """

    def test_contract_tables_are_reused_verbatim(self, tmp_store):
        manifest = build_contract_manifest(tmp_store.con)
        for table_name, specs in CONTRACT.items():
            assert table_name in manifest
            assert manifest[table_name] == list(specs)

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

    def test_agrees_across_independently_bootstrapped_warehouses(self, tmp_store, fresh_store):
        """tmp_store (template copy) and fresh_store (full real bootstrap) must derive
        the identical manifest -- same schema, same source tree -> same result.
        """
        from_tmp = build_contract_manifest(tmp_store.con)
        from_fresh = build_contract_manifest(fresh_store.con)
        assert set(from_tmp) == set(from_fresh)
        for table_name in from_tmp:
            assert from_tmp[table_name] == from_fresh[table_name], table_name
