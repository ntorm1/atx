"""S4-0: schema + seed-loader tests for the formula_registry table.

Covers migrations 0075 (schema + table_catalog/field_catalog seed) and 0076
(indexes) for ``formula_registry``, plus ``db/formula_library.py``'s seed
reader/loader (mirrors the item_registry seed pattern: strict
fieldnames==SEED_COLUMNS csv.DictReader contract, DELETE-by-ids-then-upsert
in one transaction).

This is the FOUNDATION task: no formula codes are ported here (that is
S4-1's byte-identity-gated job). These tests assert structure, catalog rows,
idempotency, and the strict seed contract using a tiny synthetic seed CSV --
not the real 53 ratio codes.
"""

from __future__ import annotations

import csv
from pathlib import Path

import duckdb
import pytest


NEW_TABLES = ("formula_registry",)

EXPECTED_COLUMNS = {
    "formula_registry": (
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
        "run_id",
        "source_loaded_at",
    ),
}

SEED_COLUMNS = (
    "formula_code",
    "family",
    "kind",
    "unit",
    "numerator_code",
    "denominator_code",
    "numerator_item_ids",
    "denominator_item_ids",
    "inputs",
    "transform",
    "expression",
    "is_meaningful_rule",
    "definition",
    "citation",
    "valid_from",
    "valid_to",
)


def _columns(con: duckdb.DuckDBPyConnection, table_name: str) -> list[str]:
    rows = con.execute(
        """
        SELECT column_name
        FROM information_schema.columns
        WHERE table_schema = 'main' AND table_name = ?
        ORDER BY ordinal_position
        """,
        [table_name],
    ).fetchall()
    return [row[0] for row in rows]


def _table_exists(con: duckdb.DuckDBPyConnection, table_name: str) -> bool:
    count = con.execute(
        "SELECT count(*) FROM duckdb_tables() WHERE table_name = ?",
        [table_name],
    ).fetchone()[0]
    return count == 1


# ---------------------------------------------------------------------------
# Schema (migration 0075)
# ---------------------------------------------------------------------------


def test_formula_registry_table_exists(tmp_store):
    assert _table_exists(tmp_store.con, "formula_registry")


def test_formula_registry_table_columns(tmp_store):
    assert _columns(tmp_store.con, "formula_registry") == list(EXPECTED_COLUMNS["formula_registry"])


def test_table_catalog_has_formula_registry_row(tmp_store):
    rows = tmp_store.con.execute(
        "SELECT table_name FROM table_catalog WHERE table_name = ANY(?)",
        [list(NEW_TABLES)],
    ).fetchall()
    catalogued = {row[0] for row in rows}
    assert catalogued == set(NEW_TABLES)


def test_field_catalog_has_one_row_per_formula_registry_column(tmp_store):
    rows = tmp_store.con.execute(
        "SELECT field_name FROM field_catalog WHERE table_name = 'formula_registry'",
    ).fetchall()
    catalogued = {row[0] for row in rows}
    expected = set(EXPECTED_COLUMNS["formula_registry"])
    assert catalogued == expected, (
        f"formula_registry field_catalog mismatch; "
        f"missing={sorted(expected - catalogued)}, extra={sorted(catalogued - expected)}"
    )


def test_formula_registry_formula_code_is_primary_key(tmp_store):
    con = tmp_store.con
    con.execute(
        """
        INSERT INTO formula_registry (
            formula_code, family, kind, unit, inputs_json, transform, definition, valid_from
        )
        VALUES ('test_code_pk', 'profitability', 'ratio', 'ratio', '[]', 'divide', 'test', DATE '1900-01-01')
        """
    )
    with pytest.raises(duckdb.Error):
        con.execute(
            """
            INSERT INTO formula_registry (
                formula_code, family, kind, unit, inputs_json, transform, definition, valid_from
            )
            VALUES ('test_code_pk', 'leverage', 'ratio', 'ratio', '[]', 'divide', 'test2', DATE '1900-01-01')
            """
        )


def test_formula_registry_requires_not_null_core_columns(tmp_store):
    con = tmp_store.con
    with pytest.raises(duckdb.Error):
        con.execute(
            """
            INSERT INTO formula_registry (formula_code, family, kind, unit, transform, definition, valid_from)
            VALUES ('test_missing_inputs', 'profitability', 'ratio', 'ratio', 'divide', 'test', DATE '1900-01-01')
            """
        )


# ---------------------------------------------------------------------------
# Indexes (migration 0076)
# ---------------------------------------------------------------------------


def test_formula_registry_indexes_exist(tmp_store):
    rows = tmp_store.con.execute(
        "SELECT index_name, table_name FROM duckdb_indexes() WHERE table_name = ANY(?)",
        [list(NEW_TABLES)],
    ).fetchall()
    index_names = {row[0] for row in rows}
    assert "idx_formula_registry_formula_code" in index_names
    assert "idx_formula_registry_family" in index_names


def test_migrations_0075_and_0076_recorded(tmp_store):
    rows = tmp_store.con.execute(
        "SELECT CAST(version AS INTEGER) FROM schema_migrations WHERE version ~ '^[0-9]+$'"
    ).fetchall()
    versions = {row[0] for row in rows}
    assert 75 in versions, f"Migration 0075 not recorded; found: {sorted(versions)}"
    assert 76 in versions, f"Migration 0076 not recorded; found: {sorted(versions)}"


def test_formula_registry_migration_bodies_are_idempotent(tmp_store):
    """Re-running the migration functions directly must not raise or duplicate catalog rows."""
    from db.migrations import _formula_registry_schema_catalog, _formula_registry_indexes

    _formula_registry_schema_catalog(tmp_store.con)
    _formula_registry_schema_catalog(tmp_store.con)
    _formula_registry_indexes(tmp_store.con)
    _formula_registry_indexes(tmp_store.con)

    catalog_count = tmp_store.con.execute(
        "SELECT count(*) FROM table_catalog WHERE table_name = 'formula_registry'"
    ).fetchone()[0]
    assert catalog_count == 1

    field_count = tmp_store.con.execute(
        "SELECT count(*) FROM field_catalog WHERE table_name = 'formula_registry'"
    ).fetchone()[0]
    assert field_count == len(EXPECTED_COLUMNS["formula_registry"])


def test_apply_pending_migrations_is_idempotent(tmp_store):
    from db.migrations import apply_pending_migrations

    applied = apply_pending_migrations(tmp_store.con)
    assert applied == []


# ---------------------------------------------------------------------------
# Seed reader (db/formula_library.py) -- strict fieldnames contract
# ---------------------------------------------------------------------------


def _valid_seed_values(**overrides: str) -> list[str]:
    values = {
        "formula_code": "net_profit_margin",
        "family": "profitability",
        "kind": "ratio",
        "unit": "ratio",
        "numerator_code": "net_income",
        "denominator_code": "revenue",
        "numerator_item_ids": "[1031]",
        "denominator_item_ids": "[1001]",
        "inputs": '["ni", "rev"]',
        "transform": "divide",
        "expression": "",
        "is_meaningful_rule": "",
        "definition": "Net income divided by revenue.",
        "citation": "",
        "valid_from": "1900-01-01",
        "valid_to": "",
    }
    values.update(overrides)
    return [values[column] for column in SEED_COLUMNS]


def _write_seed_csv(tmp_path: Path, rows: list[list[str]], *, header: list[str] | None = None) -> Path:
    seed_path = tmp_path / "formula_registry.csv"
    with seed_path.open("w", newline="", encoding="utf-8") as fh:
        writer = csv.writer(fh)
        writer.writerow(header or SEED_COLUMNS)
        writer.writerows(rows)
    return seed_path


def test_read_formula_registry_seed_parses_valid_row(tmp_path):
    from db.formula_library import read_formula_registry_seed

    seed_path = _write_seed_csv(tmp_path, [_valid_seed_values()])
    rows = read_formula_registry_seed(seed_path)
    assert len(rows) == 1
    row = rows[0]
    assert row.formula_code == "net_profit_margin"
    assert row.family == "profitability"
    assert row.kind == "ratio"
    assert row.unit == "ratio"
    assert row.numerator_code == "net_income"
    assert row.denominator_code == "revenue"
    assert row.numerator_item_ids == "[1031]"
    assert row.denominator_item_ids == "[1001]"
    assert row.inputs == '["ni", "rev"]'
    assert row.transform == "divide"
    assert row.expression is None
    assert row.is_meaningful_rule is None
    assert row.definition == "Net income divided by revenue."
    assert row.citation is None
    assert row.valid_from == "1900-01-01"
    assert row.valid_to is None


def test_read_formula_registry_seed_rejects_wrong_header(tmp_path):
    from db.formula_library import read_formula_registry_seed

    seed_path = _write_seed_csv(
        tmp_path,
        [_valid_seed_values()],
        header=["formula_code", "family"],
    )
    with pytest.raises(ValueError, match="unexpected columns"):
        read_formula_registry_seed(seed_path)


@pytest.mark.parametrize(
    ("row", "message"),
    [
        (_valid_seed_values() + ["extra"], "extra CSV fields"),
        (_valid_seed_values(formula_code=""), "blank required field formula_code"),
        (_valid_seed_values(family=""), "blank required field family"),
        (_valid_seed_values(kind=""), "blank required field kind"),
        (_valid_seed_values(unit=""), "blank required field unit"),
        (_valid_seed_values(transform=""), "blank required field transform"),
        (_valid_seed_values(definition=""), "blank required field definition"),
        (_valid_seed_values(inputs=""), "blank required field inputs"),
        (_valid_seed_values(kind="bogus_kind"), "invalid kind"),
        (_valid_seed_values(transform="bogus_transform"), "invalid transform"),
        (_valid_seed_values(valid_from="19000101"), "invalid valid_from"),
        (_valid_seed_values(inputs="not json"), "invalid inputs JSON"),
        (_valid_seed_values(numerator_item_ids="not json"), "invalid numerator_item_ids JSON"),
    ],
)
def test_read_formula_registry_seed_rejects_malformed_rows(tmp_path, row, message):
    from db.formula_library import read_formula_registry_seed

    seed_path = _write_seed_csv(tmp_path, [row])
    with pytest.raises(ValueError, match=message):
        read_formula_registry_seed(seed_path)


def test_read_formula_registry_seed_allows_blank_citation_and_expression(tmp_path):
    """Brief: citation is empty for plain accounting ratios, required for scores/academic formulas
    (enforced by reviewers/S4-1/S4-2 content, not a DB-level NOT NULL -- so blank must parse cleanly)."""
    from db.formula_library import read_formula_registry_seed

    seed_path = _write_seed_csv(tmp_path, [_valid_seed_values(citation="", expression="")])
    rows = read_formula_registry_seed(seed_path)
    assert rows[0].citation is None
    assert rows[0].expression is None


# ---------------------------------------------------------------------------
# Seed loader (db/formula_library.py) -- DELETE-by-ids-then-upsert in one txn
# ---------------------------------------------------------------------------


def test_seed_formula_registry_loads_rows(tmp_store, tmp_path):
    from db.formula_library import seed_formula_registry

    seed_path = _write_seed_csv(
        tmp_path,
        [
            _valid_seed_values(),
            _valid_seed_values(formula_code="return_on_assets", numerator_code="net_income", denominator_code="assets"),
        ],
    )
    count = seed_formula_registry(tmp_store, seed_path=seed_path)
    assert count == 2

    rows = tmp_store.con.execute(
        "SELECT formula_code FROM formula_registry ORDER BY formula_code"
    ).fetchall()
    assert {row[0] for row in rows} == {"net_profit_margin", "return_on_assets"}


def test_seed_formula_registry_reload_is_idempotent_upsert(tmp_store, tmp_path):
    """Reloading the same seed content twice does not duplicate or error."""
    from db.formula_library import seed_formula_registry

    seed_path = _write_seed_csv(tmp_path, [_valid_seed_values()])
    seed_formula_registry(tmp_store, seed_path=seed_path)
    count = seed_formula_registry(tmp_store, seed_path=seed_path)
    assert count == 1

    rows = tmp_store.con.execute(
        "SELECT count(*) FROM formula_registry WHERE formula_code = 'net_profit_margin'"
    ).fetchone()
    assert rows[0] == 1


def test_seed_formula_registry_reload_replaces_changed_definition(tmp_store, tmp_path):
    from db.formula_library import seed_formula_registry

    seed_path = _write_seed_csv(tmp_path, [_valid_seed_values()])
    seed_formula_registry(tmp_store, seed_path=seed_path)

    updated_path = _write_seed_csv(
        tmp_path,
        [_valid_seed_values(definition="Updated definition text.")],
    )
    seed_formula_registry(tmp_store, seed_path=updated_path)

    definition = tmp_store.con.execute(
        "SELECT definition FROM formula_registry WHERE formula_code = 'net_profit_margin'"
    ).fetchone()[0]
    assert definition == "Updated definition text."


def test_seed_formula_registry_delete_by_ids_removes_stale_rows_only_for_reloaded_codes(tmp_store, tmp_path):
    """DELETE-by-ids-then-upsert: a formula_code present before but absent from the new
    seed file is NOT touched (mirrors item_registry's DELETE WHERE item_id = ANY(seed ids))."""
    from db.formula_library import seed_formula_registry

    first_path = _write_seed_csv(
        tmp_path,
        [
            _valid_seed_values(),
            _valid_seed_values(formula_code="return_on_assets", numerator_code="net_income", denominator_code="assets"),
        ],
    )
    seed_formula_registry(tmp_store, seed_path=first_path)

    second_path = _write_seed_csv(tmp_path, [_valid_seed_values()])
    seed_formula_registry(tmp_store, seed_path=second_path)

    codes = {
        row[0]
        for row in tmp_store.con.execute("SELECT formula_code FROM formula_registry").fetchall()
    }
    assert codes == {"net_profit_margin", "return_on_assets"}


def test_seed_formula_registry_stores_json_and_bitemporal_columns(tmp_store, tmp_path):
    from db.formula_library import seed_formula_registry

    seed_path = _write_seed_csv(tmp_path, [_valid_seed_values()])
    seed_formula_registry(tmp_store, seed_path=seed_path)

    row = tmp_store.con.execute(
        """
        SELECT numerator_item_ids_json, denominator_item_ids_json, inputs_json,
               valid_from, valid_to, citation, expression
        FROM formula_registry WHERE formula_code = 'net_profit_margin'
        """
    ).fetchone()
    assert row[0] == "[1031]"
    assert row[1] == "[1001]"
    assert row[2] == '["ni", "rev"]'
    assert str(row[3]) == "1900-01-01"
    assert row[4] is None
    assert row[5] is None
    assert row[6] is None


def test_seed_formula_registry_rejects_duplicate_formula_code_in_seed(tmp_path):
    from db.formula_library import read_formula_registry_seed

    seed_path = _write_seed_csv(
        tmp_path,
        [
            _valid_seed_values(),
            _valid_seed_values(definition="A conflicting duplicate row."),
        ],
    )
    rows = read_formula_registry_seed(seed_path)
    with pytest.raises(ValueError, match="[Dd]uplicate"):
        from db.formula_library import _dedupe_formula_rows

        _dedupe_formula_rows(rows)


def test_default_formula_registry_seed_path_exists():
    from db.formula_library import SEED_PATH

    assert SEED_PATH.exists()
    assert SEED_PATH.name == "formula_registry.csv"


def test_formula_registry_seed_csv_is_stdlib_parseable():
    """The committed seed is a deterministic CSV parsed with stdlib csv.

    S4-0 ships schema + loader only; the committed seed is intentionally a
    minimal placeholder (S4-1 ports the real 53 codes). This asserts the
    committed file, whatever its size, obeys the strict column contract.
    """
    from db.formula_library import SEED_PATH, SEED_COLUMNS as MODULE_SEED_COLUMNS

    with SEED_PATH.open(newline="", encoding="utf-8") as fh:
        reader = csv.DictReader(fh)
        assert tuple(reader.fieldnames or ()) == MODULE_SEED_COLUMNS
        rows = list(reader)

    if rows:
        assert tuple(rows[0].keys()) == MODULE_SEED_COLUMNS


def test_seed_formula_registry_loads_from_committed_seed(tmp_store):
    """The real committed seed loads cleanly end-to-end (schema -> loader)."""
    from db.formula_library import seed_formula_registry

    count = seed_formula_registry(tmp_store)
    assert count >= 0

    schema_rows = tmp_store.con.execute(
        "SELECT count(*) FROM formula_registry"
    ).fetchone()[0]
    assert schema_rows == count


# ---------------------------------------------------------------------------
# PF-S4 S4-1: operand-term mini-grammar + composite dispatch (offline, no DuckDB)
#
# S4-1 ported 55 ratio/score codes into formula_registry rows and made
# fundamental_ratios.py drive its RATIO_DEFS from the registry via
# eval_operand_term (the operand-term mini-grammar interpreter) and
# resolve_composite_evaluator (the whitelisted composite dispatch table).
# That +409-line mechanism previously had only transitive coverage via the
# golden byte-identity test; these tests pin its per-shape behavior and its
# fail-closed (KeyError/ValueError) contract directly.
# ---------------------------------------------------------------------------


def test_eval_operand_term_key_shape():
    from db.formula_library import eval_operand_term

    assert eval_operand_term("key:ni", {"ni": 42.0}) == 42.0


def test_eval_operand_term_abs_shape():
    from db.formula_library import eval_operand_term

    assert eval_operand_term("abs:dividends", {"dividends": -7.5}) == 7.5


def test_eval_operand_term_sum_shape():
    from db.formula_library import eval_operand_term

    assert eval_operand_term("sum:a,b", {"a": 3.0, "b": 4.0}) == 7.0


def test_eval_operand_term_abs_sum_shape():
    from db.formula_library import eval_operand_term

    assert eval_operand_term("abs_sum:a,b", {"a": -3.0, "b": 4.0}) == 7.0


def test_eval_operand_term_diff_shape():
    from db.formula_library import eval_operand_term

    assert eval_operand_term("diff:a,b", {"a": 10.0, "b": 3.0}) == 7.0


def test_eval_operand_term_diff_abs_shape():
    """diff_abs: r[k1] - abs(r[k2]) -- e.g. retention_ratio's net_income - |dividends|."""
    from db.formula_library import eval_operand_term

    assert eval_operand_term("diff_abs:ni,dividends", {"ni": 10.0, "dividends": -4.0}) == 6.0


def test_eval_operand_term_diff_z_shape_present_value():
    """diff_z: r[k1] - z(r.get(k2)) -- when k2 is present, behaves like a normal diff."""
    from db.formula_library import eval_operand_term

    assert eval_operand_term(
        "diff_z:current_assets,inventory", {"current_assets": 100.0, "inventory": 30.0}
    ) == 70.0


def test_eval_operand_term_diff_z_shape_missing_value_coalesces_to_zero():
    """diff_z: a missing (r.get returns None) k2 is NaN-coalesced to 0.0, not KeyError'd."""
    from db.formula_library import eval_operand_term

    assert eval_operand_term(
        "diff_z:current_assets,inventory", {"current_assets": 100.0}
    ) == 100.0


def test_eval_operand_term_avg_shape():
    from db.formula_library import eval_operand_term

    assert eval_operand_term("avg:a,b", {"a": 10.0, "b": 20.0}) == 15.0


def test_eval_operand_term_none_shape():
    """The 'none' shape is unused -- score composites bypass operands entirely."""
    from db.formula_library import eval_operand_term

    assert eval_operand_term("none:", {}) is None


def test_eval_operand_term_unknown_shape_raises_key_error():
    """Fail-closed: an unrecognized operand shape name raises KeyError, never eval/exec."""
    from db.formula_library import eval_operand_term

    with pytest.raises(KeyError, match="bogus"):
        eval_operand_term("bogus:x", {})


def test_resolve_composite_evaluator_maps_known_keys_to_named_functions():
    from db.formula_library import (
        altman_z_double_prime,
        beneish_m_score,
        ohlson_o_score,
        piotroski_f_score,
        resolve_composite_evaluator,
    )

    assert resolve_composite_evaluator("composite:altman_z_double_prime_v1") is altman_z_double_prime
    assert resolve_composite_evaluator("composite:piotroski_f_score_v1") is piotroski_f_score
    assert resolve_composite_evaluator("composite:ohlson_o_score_v1") is ohlson_o_score
    assert resolve_composite_evaluator("composite:beneish_m_score_v1") is beneish_m_score


def test_resolve_composite_evaluator_unknown_key_raises_key_error():
    """Fail-closed: a composite:-prefixed but unregistered key raises KeyError."""
    from db.formula_library import resolve_composite_evaluator

    with pytest.raises(KeyError, match="unknown"):
        resolve_composite_evaluator("composite:unknown_v1")


def test_resolve_composite_evaluator_non_composite_prefix_raises_key_error():
    """Fail-closed: a string not prefixed with 'composite:' is rejected outright, never
    falls through to a dispatch lookup."""
    from db.formula_library import resolve_composite_evaluator

    with pytest.raises(KeyError, match="not a composite dispatch expression"):
        resolve_composite_evaluator("altman_z_double_prime_v1")


def test_parse_operand_expression_splits_numerator_and_denominator():
    from db.formula_library import parse_operand_expression

    assert parse_operand_expression("key:ni|key:rev") == ("key:ni", "key:rev")


def test_parse_operand_expression_malformed_expression_raises_value_error():
    """Fail-closed: an expression with no '|' separator raises ValueError."""
    from db.formula_library import parse_operand_expression

    with pytest.raises(ValueError, match="malformed operand expression"):
        parse_operand_expression("key:ni_no_separator")


# ---------------------------------------------------------------------------
# PF-S4 S4-2: the new `sum3` operand shape (cash_interest_coverage's
# OCF + interest + tax numerator -- the one genuinely new shape this task adds)
# and the new composite evaluators (DuPont decomposition, Sloan accruals,
# cash-conversion-cycle day-counts, per-share suite, Montier C-score).
# ---------------------------------------------------------------------------


def test_eval_operand_term_sum3_shape():
    from db.formula_library import eval_operand_term

    assert eval_operand_term("sum3:a,b,c", {"a": 3.0, "b": 4.0, "c": 5.0}) == 12.0


def test_resolve_composite_evaluator_maps_s4_2_keys_to_named_functions():
    from db.formula_library import (
        cash_conversion_cycle,
        days_inventory_outstanding,
        days_payables_outstanding,
        days_sales_outstanding,
        eps_basic,
        eps_diluted,
        montier_c_score,
        resolve_composite_evaluator,
        roe_dupont_3way,
        roe_dupont_5way,
        tangible_book_value_per_share,
        total_accruals,
        working_capital_accruals,
    )

    assert resolve_composite_evaluator("composite:roe_dupont_3way_v1") is roe_dupont_3way
    assert resolve_composite_evaluator("composite:roe_dupont_5way_v1") is roe_dupont_5way
    assert resolve_composite_evaluator("composite:total_accruals_v1") is total_accruals
    assert resolve_composite_evaluator("composite:working_capital_accruals_v1") is working_capital_accruals
    assert resolve_composite_evaluator("composite:days_sales_outstanding_v1") is days_sales_outstanding
    assert resolve_composite_evaluator("composite:days_inventory_outstanding_v1") is days_inventory_outstanding
    assert resolve_composite_evaluator("composite:days_payables_outstanding_v1") is days_payables_outstanding
    assert resolve_composite_evaluator("composite:cash_conversion_cycle_v1") is cash_conversion_cycle
    assert resolve_composite_evaluator("composite:eps_basic_v1") is eps_basic
    assert resolve_composite_evaluator("composite:eps_diluted_v1") is eps_diluted
    assert (
        resolve_composite_evaluator("composite:tangible_book_value_per_share_v1")
        is tangible_book_value_per_share
    )
    assert resolve_composite_evaluator("composite:montier_c_score_v1") is montier_c_score


class TestRoeDupont3Way:
    def test_known_value_telescopes_to_roe(self):
        from db.formula_library import roe_dupont_3way

        row = {"ni": 100.0, "rev": 400.0, "assets": 350.0, "equity": 60.0}
        # (100/400) x (400/350) x (350/60) == 100/60
        assert roe_dupont_3way(row) == pytest.approx(100.0 / 60.0)

    def test_non_positive_equity_returns_none(self):
        from db.formula_library import roe_dupont_3way

        row = {"ni": 100.0, "rev": 400.0, "assets": 350.0, "equity": -10.0}
        assert roe_dupont_3way(row) is None


class TestRoeDupont5Way:
    def test_known_value_telescopes_to_roe(self):
        from db.formula_library import roe_dupont_5way

        row = {"ni": 100.0, "pretax_income": 115.0, "oi": 120.0, "rev": 400.0, "assets": 350.0, "equity": 60.0}
        # (100/115) x (115/120) x (120/400) x (400/350) x (350/60) == 100/60
        assert roe_dupont_5way(row) == pytest.approx(100.0 / 60.0)

    def test_zero_pretax_income_returns_none(self):
        from db.formula_library import roe_dupont_5way

        row = {"ni": 100.0, "pretax_income": 0.0, "oi": 120.0, "rev": 400.0, "assets": 350.0, "equity": 60.0}
        assert roe_dupont_5way(row) is None


class TestTotalAccruals:
    def test_known_value(self):
        from db.formula_library import total_accruals

        row = {"ni": 100.0, "ocf": 130.0, "assets": 350.0, "assets_prior": 300.0}
        # (100 - 130) / ((350 + 300) / 2) == -30 / 325
        assert total_accruals(row) == pytest.approx(-30.0 / 325.0)

    def test_non_positive_average_assets_returns_none(self):
        from db.formula_library import total_accruals

        row = {"ni": 100.0, "ocf": 130.0, "assets": 0.0, "assets_prior": 0.0}
        assert total_accruals(row) is None


class TestWorkingCapitalAccruals:
    def test_known_value(self):
        from db.formula_library import working_capital_accruals

        row = {
            "current_assets": 200.0, "current_assets_prior": 150.0,
            "cash_and_equivalents": 40.0, "cash_and_equivalents_prior": 35.0,
            "current_liabilities": 100.0, "current_liabilities_prior": 90.0,
            "assets": 350.0, "assets_prior": 300.0,
        }
        # [((200-150) - (40-35)) - (100-90)] / ((350+300)/2) == 35 / 325
        assert working_capital_accruals(row) == pytest.approx(35.0 / 325.0)

    def test_non_positive_average_assets_returns_none(self):
        from db.formula_library import working_capital_accruals

        row = {
            "current_assets": 200.0, "current_assets_prior": 150.0,
            "cash_and_equivalents": 40.0, "cash_and_equivalents_prior": 35.0,
            "current_liabilities": 100.0, "current_liabilities_prior": 90.0,
            "assets": 0.0, "assets_prior": 0.0,
        }
        assert working_capital_accruals(row) is None


class TestCashConversionCycleComponents:
    def test_days_sales_outstanding_known_value(self):
        from db.formula_library import days_sales_outstanding

        row = {"accounts_receivable": 50.0, "accounts_receivable_prior": 40.0, "rev": 400.0}
        assert days_sales_outstanding(row) == pytest.approx((50.0 + 40.0) / 2 / 400.0 * 365.0)

    def test_days_inventory_outstanding_known_value(self):
        from db.formula_library import days_inventory_outstanding

        row = {"inventory": 30.0, "inventory_prior": 25.0, "cost_of_revenue": 220.0}
        assert days_inventory_outstanding(row) == pytest.approx((30.0 + 25.0) / 2 / 220.0 * 365.0)

    def test_days_payables_outstanding_known_value(self):
        from db.formula_library import days_payables_outstanding

        row = {"accounts_payable": 35.0, "accounts_payable_prior": 28.0, "cost_of_revenue": 220.0}
        assert days_payables_outstanding(row) == pytest.approx((35.0 + 28.0) / 2 / 220.0 * 365.0)

    def test_cash_conversion_cycle_known_value_sums_components(self):
        from db.formula_library import (
            cash_conversion_cycle,
            days_inventory_outstanding,
            days_payables_outstanding,
            days_sales_outstanding,
        )

        row = {
            "accounts_receivable": 50.0, "accounts_receivable_prior": 40.0,
            "inventory": 30.0, "inventory_prior": 25.0,
            "accounts_payable": 35.0, "accounts_payable_prior": 28.0,
            "rev": 400.0, "cost_of_revenue": 220.0,
        }
        expected = (
            days_sales_outstanding(row) + days_inventory_outstanding(row) - days_payables_outstanding(row)
        )
        assert cash_conversion_cycle(row) == pytest.approx(expected)

    def test_cash_conversion_cycle_none_when_component_unusable(self):
        from db.formula_library import cash_conversion_cycle

        row = {
            "accounts_receivable": 50.0, "accounts_receivable_prior": 40.0,
            "inventory": 30.0, "inventory_prior": 25.0,
            "accounts_payable": 35.0, "accounts_payable_prior": 28.0,
            "rev": 0.0, "cost_of_revenue": 220.0,
        }
        assert cash_conversion_cycle(row) is None


class TestPerShareComposites:
    def test_eps_basic_known_value(self):
        from db.formula_library import eps_basic

        assert eps_basic({"ni": 100.0, "shares_basic_avg": 14.5}) == pytest.approx(100.0 / 14.5)

    def test_eps_basic_non_positive_shares_returns_none(self):
        from db.formula_library import eps_basic

        assert eps_basic({"ni": 100.0, "shares_basic_avg": 0.0}) is None

    def test_eps_diluted_known_value(self):
        from db.formula_library import eps_diluted

        assert eps_diluted({"ni": 100.0, "shares_diluted_avg": 15.2}) == pytest.approx(100.0 / 15.2)

    def test_tangible_book_value_per_share_known_value(self):
        from db.formula_library import tangible_book_value_per_share

        row = {"equity": 60.0, "goodwill": 20.0, "intangibles_other": 5.0, "shares": 15.0}
        assert tangible_book_value_per_share(row) == pytest.approx((60.0 - 20.0 - 5.0) / 15.0)

    def test_tangible_book_value_per_share_non_positive_shares_returns_none(self):
        from db.formula_library import tangible_book_value_per_share

        row = {"equity": 60.0, "goodwill": 20.0, "intangibles_other": 5.0, "shares": 0.0}
        assert tangible_book_value_per_share(row) is None


class TestMontierCScore:
    def test_known_value_two_flags_trigger(self):
        from db.formula_library import montier_c_score

        row = {
            "ni": 100.0, "ocf": 130.0, "ni_prior": 80.0, "ocf_prior": 104.0,
            "rev": 400.0, "rev_prior": 320.0,
            "accounts_receivable": 50.0, "accounts_receivable_prior": 40.0,
            "inventory": 30.0, "inventory_prior": 25.0,
            "current_assets": 200.0, "current_assets_prior": 150.0,
            "cash_and_equivalents": 40.0, "cash_and_equivalents_prior": 35.0,
            "assets": 350.0, "assets_prior": 300.0,
            "depreciation_amortization": 25.0, "depreciation_amortization_prior": 20.0,
            "property_plant_equipment_net": 120.0, "property_plant_equipment_net_prior": 110.0,
        }
        # hand-checked: NI-OCF gap narrows (flag1 false), DSO flat (flag2 false),
        # DIO falls (flag3 false), other-current-assets share rises 0.167->0.229 (flag4
        # true), depreciation rate RISES 0.154->0.172 (flag5 false, needs a decline),
        # asset growth 16.7% > 10% (flag6 true) -> score 2
        assert montier_c_score(row) == pytest.approx(2.0)

    def test_none_when_revenue_zero(self):
        from db.formula_library import montier_c_score

        row = {
            "ni": 100.0, "ocf": 130.0, "ni_prior": 80.0, "ocf_prior": 104.0,
            "rev": 0.0, "rev_prior": 320.0,
            "accounts_receivable": 50.0, "accounts_receivable_prior": 40.0,
            "inventory": 30.0, "inventory_prior": 25.0,
            "current_assets": 200.0, "current_assets_prior": 150.0,
            "cash_and_equivalents": 40.0, "cash_and_equivalents_prior": 35.0,
            "assets": 350.0, "assets_prior": 300.0,
            "depreciation_amortization": 25.0, "depreciation_amortization_prior": 20.0,
            "property_plant_equipment_net": 120.0, "property_plant_equipment_net_prior": 110.0,
        }
        assert montier_c_score(row) is None
