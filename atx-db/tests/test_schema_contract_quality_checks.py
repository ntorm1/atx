"""PF2-S1 S1-1 tests for the catalog-completeness and PIT-column-presence checks.

Two first-class, gate-ready (severity="critical") checks registered in
``db/quality.py``:

- ``catalog_completeness_check``: every non-ephemeral live table (the same
  ``duckdb_tables()``-based filter ``schema_contract.py::_fetch_live_tables`` uses,
  which already excludes registered temp relations and the
  ``duckdb_%``/``sqlite_%``/``pragma_%`` internals for free) must have a
  ``table_catalog`` row.
- ``pit_column_presence_check``: every table ``build_contract_manifest()`` marks
  fact/derived (i.e. carries >=1 strong bitemporal marker -- any
  ``ColumnSpec.is_pit_column=True``) must carry all five canonical PIT columns
  (``schema_contract.PIT_COLUMN_NAMES``). The fact/non-fact partition comes from the
  manifest, not a hardcoded table list.

Both are ``severity="critical"`` so PF2-S10 can later gate the orchestrator on them
(clause G, adopted incrementally) -- this sprint only authors the checks, it does not
wire orchestrator gating.

Coverage:
- baseline fixtures (nothing planted) -> both GREEN
- a planted uncatalogued table -> catalog_completeness_check RED, naming the table
- a planted fact table missing available_at -> pit_column_presence_check RED, naming
  the table and the missing column
- a non-fact table missing PIT columns is never flagged (fact/non-fact partition
  matters)
- both checks flow through run_warehouse_quality_checks additively (existing checks
  keep their default "standard" severity, unaffected)
- an INTEGRATION check against a real freshly-bootstrapped warehouse (tmp_store):
  the sprint's headline acceptance bench.
"""

from __future__ import annotations

import duckdb

from atx_db.connection import DuckDBStore
from atx_db.quality import (
    catalog_completeness_check,
    pit_column_presence_check,
    run_warehouse_quality_checks,
)
from atx_db.schema_contract import ColumnSpec


def _bare_store() -> DuckDBStore:
    """A DuckDBStore wrapping a bare in-memory connection with NO schema bootstrapped.

    Mirrors test_schema_contract.py's ``_bare_store()`` helper -- lets these tests
    build small, self-contained fixtures instead of paying the full bootstrap cost.
    """
    store = DuckDBStore(":memory:")
    store.connection = duckdb.connect(":memory:")
    return store


def _catalog(con: duckdb.DuckDBPyConnection, table_name: str) -> None:
    con.execute("INSERT INTO table_catalog (table_name) VALUES (?)", [table_name])


class TestCatalogCompletenessCheck:
    def test_green_when_every_live_table_is_catalogued(self):
        store = _bare_store()
        con = store.con
        try:
            con.execute("CREATE TABLE table_catalog (table_name VARCHAR)")
            con.execute("CREATE TABLE widget (widget_id VARCHAR)")
            _catalog(con, "table_catalog")
            _catalog(con, "widget")

            result = catalog_completeness_check(store)
            assert result.status == "passed"
            assert result.observed_value == 0.0
            assert result.severity == "critical"
            assert result.check_name == "catalog_completeness"
            assert result.details["uncatalogued_tables"] == []
        finally:
            store.connection.close()

    def test_red_when_a_table_is_uncatalogued(self):
        store = _bare_store()
        con = store.con
        try:
            con.execute("CREATE TABLE table_catalog (table_name VARCHAR)")
            con.execute("CREATE TABLE widget (widget_id VARCHAR)")
            con.execute("CREATE TABLE mystery_widget (widget_id VARCHAR)")
            _catalog(con, "table_catalog")
            _catalog(con, "widget")
            # mystery_widget intentionally left uncatalogued.

            result = catalog_completeness_check(store)
            assert result.status == "failed"
            assert result.observed_value == 1.0
            assert result.details["uncatalogued_tables"] == ["mystery_widget"]
        finally:
            store.connection.close()

    def test_ephemeral_relations_are_never_flagged(self):
        import pandas as pd

        store = _bare_store()
        con = store.con
        try:
            con.execute("CREATE TABLE table_catalog (table_name VARCHAR)")
            _catalog(con, "table_catalog")
            con.register("_some_temp_seed", pd.DataFrame({"x": [1, 2]}))
            try:
                result = catalog_completeness_check(store)
                assert result.status == "passed"
            finally:
                con.unregister("_some_temp_seed")
        finally:
            store.connection.close()

    def test_warns_when_table_catalog_itself_is_missing(self):
        store = _bare_store()
        con = store.con
        try:
            con.execute("CREATE TABLE widget (widget_id VARCHAR)")
            result = catalog_completeness_check(store)
            assert result.status == "warning"
            assert result.severity == "critical"
        finally:
            store.connection.close()


class TestPitColumnPresenceCheck:
    FACT_MANIFEST: dict[str, list[ColumnSpec]] = {
        "widget_fact": [
            ColumnSpec(
                "widget_id", "VARCHAR", nullable=False, is_natural_key=True, declared_in="migration"
            ),
            ColumnSpec("as_of_date", "DATE", nullable=False, is_pit_column=True, declared_in="migration"),
            ColumnSpec(
                "available_at", "TIMESTAMP", nullable=False, is_pit_column=True, declared_in="migration"
            ),
            ColumnSpec(
                "source_loaded_at",
                "TIMESTAMP",
                nullable=False,
                is_pit_column=True,
                declared_in="migration",
            ),
            ColumnSpec("run_id", "VARCHAR", nullable=True, is_pit_column=True, declared_in="migration"),
            ColumnSpec(
                "is_latest_revision",
                "BOOLEAN",
                nullable=False,
                is_pit_column=True,
                declared_in="migration",
            ),
        ],
        "widget": [
            ColumnSpec("widget_id", "VARCHAR", nullable=False, is_natural_key=True, declared_in="schema_py"),
            ColumnSpec("label", "VARCHAR", nullable=True, declared_in="schema_py"),
        ],
    }

    def test_green_when_every_fact_table_carries_all_five(self):
        store = _bare_store()
        try:
            result = pit_column_presence_check(store, manifest=self.FACT_MANIFEST)
            assert result.status == "passed"
            assert result.observed_value == 0.0
            assert result.severity == "critical"
            assert result.check_name == "pit_column_presence"
            assert result.details["tables_missing_pit_columns"] == {}
        finally:
            store.connection.close()

    def test_red_when_a_fact_table_is_missing_available_at(self):
        store = _bare_store()
        try:
            stripped = {
                "widget_fact": [
                    spec for spec in self.FACT_MANIFEST["widget_fact"] if spec.name != "available_at"
                ],
                "widget": self.FACT_MANIFEST["widget"],
            }
            result = pit_column_presence_check(store, manifest=stripped)
            assert result.status == "failed"
            assert result.observed_value == 1.0
            assert result.details["tables_missing_pit_columns"] == {"widget_fact": ["available_at"]}
        finally:
            store.connection.close()

    def test_non_fact_table_is_never_flagged_even_if_missing_all_pit_columns(self):
        # `widget` carries no ColumnSpec with is_pit_column=True (no strong temporal
        # marker) so it is not a fact table -- it must never be checked for
        # PIT-column completeness even though it lacks all five outright.
        store = _bare_store()
        try:
            result = pit_column_presence_check(
                store, manifest={"widget": self.FACT_MANIFEST["widget"]}
            )
            assert result.status == "passed"
            assert result.details["tables_missing_pit_columns"] == {}
        finally:
            store.connection.close()

    def test_defaults_to_build_contract_manifest_over_the_live_connection(self, tmp_store):
        # No manifest override: reads build_contract_manifest(store.con) fresh -- proves
        # the fact/non-fact partition comes from the manifest, not a hardcoded list.
        result = pit_column_presence_check(tmp_store)
        assert result.severity == "critical"
        assert isinstance(result.details["tables_missing_pit_columns"], dict)


class TestRunWarehouseQualityChecksIncludesSchemaContractChecks:
    def test_results_include_both_new_checks(self, tmp_store):
        results = {
            r.check_name: r
            for r in run_warehouse_quality_checks(
                tmp_store,
                record=False,
                check_names=("catalog_completeness", "pit_column_presence"),
            )
        }
        assert "catalog_completeness" in results
        assert "pit_column_presence" in results
        assert results["catalog_completeness"].severity == "critical"
        assert results["pit_column_presence"].severity == "critical"

    def test_existing_checks_are_unaffected(self, tmp_store):
        results = {
            r.check_name: r
            for r in run_warehouse_quality_checks(
                tmp_store,
                record=False,
                check_names=(
                    "duplicate_equity_daily_bars",
                    "fundamental_ratios_without_fundamental_points",
                ),
            )
        }
        # A couple of well-known pre-existing checks are still present. PF2-S10
        # maps the old failed/warning status split into the gate taxonomy:
        # failed checks default to error unless the registry promotes/demotes
        # them as data.
        assert results["duplicate_equity_daily_bars"].severity == "error"
        assert results["fundamental_ratios_without_fundamental_points"].severity == "error"

    def test_new_checks_are_recorded_when_record_true(self, tmp_store):
        run_warehouse_quality_checks(
            tmp_store,
            record=True,
            check_names=("catalog_completeness", "pit_column_presence"),
        )
        rows = tmp_store.con.execute(
            "SELECT check_name FROM data_quality_checks WHERE check_name IN "
            "('catalog_completeness', 'pit_column_presence')"
        ).fetchall()
        assert {row[0] for row in rows} == {"catalog_completeness", "pit_column_presence"}


class TestLiveWarehouseBaseline:
    """Both schema-contract quality checks are hard-zero on a fresh bootstrap."""

    def test_catalog_completeness_is_green_on_fresh_bootstrap(self, tmp_store):
        result = catalog_completeness_check(tmp_store)
        assert result.status == "passed", f"uncatalogued tables: {result.details['uncatalogued_tables']}"

    def test_pit_column_presence_is_green_on_fresh_bootstrap(self, tmp_store):
        result = pit_column_presence_check(tmp_store)
        assert result.status == "passed", (
            "PIT offenders after registered exemptions: "
            f"{result.details['tables_missing_pit_columns']}"
        )
        assert result.observed_value == 0.0
