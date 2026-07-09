"""PF-S4 S4-3: formula catalog surface (queryable / as-of) over formula_registry.

Covers migration 0077 (the ``v_formula_registry`` catalog view + its
table_catalog/field_catalog rows) and the ``formula_registry_asof`` PIT
reader in ``db/asof.py``.

Scope: this is a READ surface over the formula_registry rows already shipped
by S4-0/S4-1/S4-2 (schema: migrations 0075/0076; seed loader + 72 formula
rows: ``db/formula_library.py`` + ``db/seeds/formula_registry.csv``). No
formula logic is touched here -- see ``test_formula_library.py`` for the
schema/seed/operand-grammar/composite-evaluator tests this file does not
duplicate.

PIT interpretation (bound by the S4-3 brief + formula_registry's own
schema): formula_registry has no ``available_at`` column -- only
``valid_from``/``valid_to`` bitemporal DEFINITION validity (migrations.py
_formula_registry_schema_catalog docstring: "valid_from/valid_to make the
formula DEFINITION itself bitemporal ... feeding S4-3's catalog reader").
So "as-of" here means: honor ``valid_from <= as_of_date <
coalesce(valid_to, 9999-12-31)`` -- there is no availability/knowledge-time
axis to additionally filter on for this table today.
"""

from __future__ import annotations

import datetime as dt

import duckdb
import pytest


# Injected as-of reference date for the "over the committed seed" assertions.
# Frozen so the "every committed formula is queryable as-of" checks evaluate at a
# FIXED as-of and can never expire on wall-clock passage. All committed formula
# rows are open-ended (valid_from 1900-01-01, valid_to NULL), so any frozen date
# in their validity window keeps the assertion semantics intact.
AS_OF = dt.date(2026, 7, 6)

NEW_VIEWS = ("v_formula_registry",)

EXPECTED_VIEW_COLUMNS = (
    "formula_code",
    "family",
    "kind",
    "unit",
    "numerator_code",
    "denominator_code",
    "numerator_item_ids_json",
    "denominator_item_ids_json",
    "inputs_json",
    "transform",
    "expression",
    "is_meaningful_rule",
    "definition",
    "citation",
    "valid_from",
    "valid_to",
)


def _table_exists(con: duckdb.DuckDBPyConnection, name: str) -> bool:
    count = con.execute(
        "SELECT count(*) FROM duckdb_tables() WHERE table_name = ?",
        [name],
    ).fetchone()[0]
    return count == 1


def _view_exists(con: duckdb.DuckDBPyConnection, name: str) -> bool:
    count = con.execute(
        "SELECT count(*) FROM duckdb_views() WHERE view_name = ?",
        [name],
    ).fetchone()[0]
    return count == 1


def _columns(con: duckdb.DuckDBPyConnection, name: str) -> list[str]:
    rows = con.execute(
        """
        SELECT column_name
        FROM information_schema.columns
        WHERE table_schema = 'main' AND table_name = ?
        ORDER BY ordinal_position
        """,
        [name],
    ).fetchall()
    return [row[0] for row in rows]


def _seed_two_rows(store) -> None:
    """Seed exactly two formula_registry rows (one open-ended, one retired).

    The warehouse bootstrap now seeds the committed formula catalog (migrations
    0148-0151, PF3-S6), so the table is non-empty in the template. Clear it first
    so this helper establishes a controlled two-row world and the exact-equality
    assertions over the view/reader remain deterministic.
    """
    con = store.con
    con.execute("DELETE FROM formula_registry")
    con.execute(
        """
        INSERT INTO formula_registry (
            formula_code, family, kind, unit, numerator_code, denominator_code,
            numerator_item_ids_json, denominator_item_ids_json, inputs_json,
            transform, expression, is_meaningful_rule, definition, citation,
            valid_from, valid_to
        )
        VALUES
            ('test_margin', 'profitability', 'ratio', 'ratio', 'net_income', 'revenue',
             '[1031]', '[1001]', '["ni", "rev"]',
             'divide', NULL, 'require_positive_denominator', 'Net income over revenue.',
             'Test citation, Journal of Testing.',
             DATE '2000-01-01', NULL),
            ('test_retired_score', 'health', 'score', 'score', NULL, NULL,
             NULL, NULL, '["a", "b"]',
             'identity', 'composite:test_score_v1', NULL, 'A retired test score.',
             NULL,
             DATE '1990-01-01', DATE '2010-01-01')
        """
    )


class TestMigration0077View:
    def test_view_exists(self, tmp_store):
        assert _view_exists(tmp_store.con, "v_formula_registry")

    def test_view_is_not_a_base_table(self, tmp_store):
        # formula_registry is catalogued like a table (per (B)/S7a), but the
        # underlying object itself must be a VIEW, not a materialized copy.
        assert not _table_exists(tmp_store.con, "v_formula_registry")

    def test_view_columns(self, tmp_store):
        assert _columns(tmp_store.con, "v_formula_registry") == list(EXPECTED_VIEW_COLUMNS)

    def test_view_reflects_formula_registry_rows_live(self, tmp_store):
        _seed_two_rows(tmp_store)
        rows = tmp_store.con.execute(
            "SELECT formula_code FROM v_formula_registry ORDER BY formula_code"
        ).fetchall()
        codes = {row[0] for row in rows}
        assert codes == {"test_margin", "test_retired_score"}

    def test_migration_0077_recorded(self, tmp_store):
        versions = {
            row[0]
            for row in tmp_store.con.execute(
                "SELECT CAST(version AS INTEGER) FROM schema_migrations WHERE version ~ '^[0-9]+$'"
            ).fetchall()
        }
        assert 77 in versions, f"Migration 0077 not recorded; found: {sorted(versions)}"

    def test_migration_body_is_idempotent(self, tmp_store):
        from db.migrations import _formula_registry_catalog_view

        _formula_registry_catalog_view(tmp_store.con)
        _formula_registry_catalog_view(tmp_store.con)

        table_catalog_count = tmp_store.con.execute(
            "SELECT count(*) FROM table_catalog WHERE table_name = 'v_formula_registry'"
        ).fetchone()[0]
        assert table_catalog_count == 1

        field_catalog_count = tmp_store.con.execute(
            "SELECT count(*) FROM field_catalog WHERE table_name = 'v_formula_registry'"
        ).fetchone()[0]
        assert field_catalog_count == len(EXPECTED_VIEW_COLUMNS)

    def test_apply_pending_migrations_is_idempotent(self, tmp_store):
        from db.migrations import apply_pending_migrations

        applied = apply_pending_migrations(tmp_store.con)
        assert applied == []


class TestCatalogRegistration:
    """Views are catalogued like tables per (B)/S7a."""

    def test_table_catalog_has_view_row(self, tmp_store):
        rows = tmp_store.con.execute(
            "SELECT table_name FROM table_catalog WHERE table_name = ANY(?)",
            [list(NEW_VIEWS)],
        ).fetchall()
        catalogued = {row[0] for row in rows}
        assert catalogued == set(NEW_VIEWS)

    def test_field_catalog_has_one_row_per_view_column(self, tmp_store):
        rows = tmp_store.con.execute(
            "SELECT field_name FROM field_catalog WHERE table_name = 'v_formula_registry'",
        ).fetchall()
        catalogued = {row[0] for row in rows}
        expected = set(EXPECTED_VIEW_COLUMNS)
        assert catalogued == expected, (
            f"v_formula_registry field_catalog mismatch; "
            f"missing={sorted(expected - catalogued)}, extra={sorted(catalogued - expected)}"
        )

    def test_table_catalog_row_documents_citation_and_definition(self, tmp_store):
        row = tmp_store.con.execute(
            "SELECT description, pit_notes FROM table_catalog WHERE table_name = 'v_formula_registry'"
        ).fetchone()
        description, pit_notes = row
        assert description is not None and "formula" in description.lower()
        assert pit_notes is not None and "valid_from" in pit_notes


class TestFormulaRegistryAsofReader:
    """PIT reader: formula_registry_asof(as_of_date, ...) in db/asof.py."""

    def test_returns_full_definition_with_citation_for_a_known_code(self, tmp_store):
        from db.asof import formula_registry_asof

        _seed_two_rows(tmp_store)
        result = formula_registry_asof(
            dt.date(2026, 1, 1), store=tmp_store, formula_codes=["test_margin"]
        )
        assert len(result) == 1
        row = result.iloc[0]
        assert row["formula_code"] == "test_margin"
        assert row["family"] == "profitability"
        assert row["kind"] == "ratio"
        assert row["unit"] == "ratio"
        assert row["numerator_item_ids_json"] == "[1031]"
        assert row["denominator_item_ids_json"] == "[1001]"
        assert row["citation"] == "Test citation, Journal of Testing."
        assert row["is_meaningful_rule"] == "require_positive_denominator"
        assert row["valid_from"].strftime("%Y-%m-%d") == "2000-01-01"

    def test_excludes_formula_not_yet_valid_as_of_date(self, tmp_store):
        from db.asof import formula_registry_asof

        _seed_two_rows(tmp_store)
        # test_margin's valid_from is 2000-01-01; before that it must not appear.
        before = formula_registry_asof(
            dt.date(1999, 1, 1), store=tmp_store, formula_codes=["test_margin"]
        )
        assert before.empty

    def test_excludes_formula_retired_before_as_of_date(self, tmp_store):
        from db.asof import formula_registry_asof

        _seed_two_rows(tmp_store)
        # test_retired_score's valid_to is 2010-01-01; after that it must not appear.
        after_retirement = formula_registry_asof(
            dt.date(2015, 1, 1), store=tmp_store, formula_codes=["test_retired_score"]
        )
        assert after_retirement.empty

        while_valid = formula_registry_asof(
            dt.date(2000, 1, 1), store=tmp_store, formula_codes=["test_retired_score"]
        )
        assert len(while_valid) == 1

    def test_filters_by_family(self, tmp_store):
        from db.asof import formula_registry_asof

        _seed_two_rows(tmp_store)
        # 2005-01-01 is within test_retired_score's [1990-01-01, 2010-01-01) window.
        result = formula_registry_asof(dt.date(2005, 1, 1), store=tmp_store, families=["health"])
        codes = set(result["formula_code"])
        assert codes == {"test_retired_score"}
        assert "test_margin" not in codes

    def test_no_filters_returns_every_currently_valid_row(self, tmp_store):
        from db.asof import formula_registry_asof

        _seed_two_rows(tmp_store)
        result = formula_registry_asof(dt.date(2026, 1, 1), store=tmp_store)
        codes = set(result["formula_code"])
        # test_retired_score's valid_to (2010-01-01) has passed by 2026, so only
        # test_margin (still open-ended) is visible as-of 2026.
        assert codes == {"test_margin"}

    def test_result_ordered_by_family_then_formula_code(self, tmp_store):
        from db.asof import formula_registry_asof

        _seed_two_rows(tmp_store)
        result = formula_registry_asof(dt.date(2000, 6, 1), store=tmp_store)
        # Both rows are valid as-of 2000-06-01 (test_margin from 2000-01-01 open-
        # ended; test_retired_score from 1990-01-01 to 2010-01-01).
        assert list(result["formula_code"]) == ["test_retired_score", "test_margin"]

    def test_opens_own_connection_when_no_store_given(self, tmp_store):
        """Without an open store, the reader opens (and closes) its own read-only connection."""
        from db.asof import formula_registry_asof
        from db.formula_library import seed_formula_registry

        seed_formula_registry(tmp_store)
        db_path = tmp_store.path
        tmp_store.connection.close()
        tmp_store.connection = None

        result = formula_registry_asof(dt.date(2026, 1, 1), db_path=db_path)
        assert not result.empty
        assert "net_profit_margin" in set(result["formula_code"])


class TestFormulaRegistryAsofOverCommittedSeed:
    """End-to-end: the real committed seed is fully catalog-queryable."""

    def test_every_committed_formula_is_queryable_as_of_today(self, tmp_store):
        from db.asof import formula_registry_asof
        from db.formula_library import seed_formula_registry

        seeded_count = seed_formula_registry(tmp_store)
        assert seeded_count > 0

        result = formula_registry_asof(AS_OF, store=tmp_store)
        assert len(result) == seeded_count

        # Every row must carry a human definition; citation may be blank for
        # plain accounting ratios but the column must at least be present/queryable.
        assert result["definition"].notna().all()
        assert set(EXPECTED_VIEW_COLUMNS).issubset(set(result.columns))

    def test_altman_z_double_prime_definition_and_citation_are_queryable(self, tmp_store):
        from db.asof import formula_registry_asof
        from db.formula_library import seed_formula_registry

        seed_formula_registry(tmp_store)
        result = formula_registry_asof(
            AS_OF, store=tmp_store, formula_codes=["altman_z_double_prime"]
        )
        assert len(result) == 1
        row = result.iloc[0]
        assert row["kind"] == "score"
        assert row["citation"] and "Altman" in row["citation"]
