"""Tier1-S2 T9: per (item, fiscal_year) coverage math, rendering and gate."""
from __future__ import annotations

import datetime as dt
import json

import pandas as pd
import pytest

from atx_db.item_coverage import (
    ITEM_COVERAGE_COLUMNS,
    ITEM_COVERAGE_TARGET_ITEMS,
    ITEM_COVERAGE_TARGET_PCT,
    ItemCoverageOptions,
    compute_item_coverage_rows,
    evaluate_item_coverage_gate,
    refresh_item_coverage,
    render_item_coverage_markdown,
)

UNIVERSE = pd.DataFrame(
    {
        "security_id": ["S1", "S2", "S3", "S4"],
        "fiscal_year": [2020, 2020, 2020, 2020],
    }
)
STANDARDIZED = pd.DataFrame(
    [
        # S1 and S2 report revenue; S3 reports it as NULL; S4 does not report it.
        {"security_id": "S1", "item_id": 1001, "canonical_code": "revenue", "basis": "annual", "fiscal_year": 2020, "value": 10.0},
        {"security_id": "S2", "item_id": 1001, "canonical_code": "revenue", "basis": "annual", "fiscal_year": 2020, "value": 20.0},
        {"security_id": "S3", "item_id": 1001, "canonical_code": "revenue", "basis": "annual", "fiscal_year": 2020, "value": None},
        # only S1 reports total_assets
        {"security_id": "S1", "item_id": 1101, "canonical_code": "total_assets", "basis": "instant", "fiscal_year": 2020, "value": 99.0},
        # a quarterly row must not leak into the annual basis slice
        {"security_id": "S4", "item_id": 1001, "canonical_code": "revenue", "basis": "quarterly", "fiscal_year": 2020, "value": 5.0},
    ]
)


def _insert_universe_rows(
    store,
    rows: tuple[tuple[str, str, dt.date, dt.date | None], ...],
) -> None:
    """Insert universe_membership rows: (security_id, symbol, valid_from, valid_to)."""
    values_sql = ", ".join(["(?,?,?,?,?,?,?,?,?,?,?,?,?,?)"] * len(rows))
    params: list[object] = []
    for security_id, symbol, valid_from, valid_to in rows:
        params.extend(
            [
                "us_common_equity_liquid_v1", security_id, symbol, valid_from, valid_to,
                valid_from, dt.datetime.combine(valid_from, dt.time()), "test", "{}", 1,
                True, "test", None, dt.datetime.combine(valid_from, dt.time()),
            ]
        )
    store.con.execute(
        f"""
        INSERT INTO universe_membership (
            universe_id, security_id, symbol, valid_from, valid_to, as_of_date,
            available_at, reason, rules_json, decision_count, is_latest_revision,
            source, run_id, source_loaded_at
        ) VALUES {values_sql}
        """,
        params,
    )


def _insert_standardized_row(
    store,
    *,
    standardized_id: str,
    security_id: str,
    item_id: int,
    canonical_code: str,
    basis: str,
    fiscal_year: int,
    value: float,
) -> None:
    store.con.execute(
        """
        INSERT INTO fundamental_standardized (
            standardized_id, source, upstream_source, security_id, symbol, cik,
            item_id, canonical_code, basis, period_start, period_end, fiscal_year,
            fiscal_period, value, unit_type, source_accession, filed_date, as_of_date,
            available_at, input_codes_json, input_item_ids_json, rule_id,
            combination_rule, is_latest_revision, run_id
        ) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)
        """,
        [
            standardized_id, "fundamental_standardization_v1", "test", security_id,
            security_id, "1", item_id, canonical_code, basis, dt.date(fiscal_year, 1, 1),
            dt.date(fiscal_year, 12, 31), fiscal_year, "FY", value, "monetary", "acc",
            dt.date(fiscal_year + 1, 2, 1), dt.date(fiscal_year, 12, 31),
            dt.datetime(fiscal_year + 1, 2, 1), "[]", f"[{item_id}]",
            f"std_{basis}_{item_id}", "coalesce_priority", True, None,
        ],
    )


def test_coverage_columns_are_stable():
    assert ITEM_COVERAGE_COLUMNS == (
        "source",
        "universe_id",
        "item_id",
        "canonical_code",
        "basis",
        "fiscal_year",
        "n_securities",
        "n_with_value",
        "coverage_pct",
    )


def test_coverage_counts_distinct_securities_with_a_non_null_value():
    rows = compute_item_coverage_rows(STANDARDIZED, UNIVERSE, ItemCoverageOptions(bases=("annual",)))
    revenue = rows[(rows["item_id"] == 1001) & (rows["basis"] == "annual")].iloc[0]

    assert revenue["n_securities"] == 4
    assert revenue["n_with_value"] == 2
    assert revenue["coverage_pct"] == pytest.approx(50.0)


def test_coverage_is_zero_not_missing_for_an_item_no_one_reports():
    rows = compute_item_coverage_rows(
        STANDARDIZED,
        UNIVERSE,
        ItemCoverageOptions(bases=("annual",), item_ids=(1001, 1003)),
    )
    absent = rows[rows["item_id"] == 1003].iloc[0]

    assert absent["n_with_value"] == 0
    assert absent["coverage_pct"] == pytest.approx(0.0)
    assert absent["n_securities"] == 4


def test_coverage_never_exceeds_one_hundred_percent():
    duplicated = pd.concat([STANDARDIZED, STANDARDIZED], ignore_index=True)
    rows = compute_item_coverage_rows(duplicated, UNIVERSE, ItemCoverageOptions(bases=("annual",)))

    assert (rows["coverage_pct"] <= 100.0).all()
    assert rows[rows["item_id"] == 1001].iloc[0]["n_with_value"] == 2


def test_coverage_rows_are_sorted_deterministically():
    rows = compute_item_coverage_rows(STANDARDIZED, UNIVERSE, ItemCoverageOptions(bases=("annual", "instant")))
    keys = list(zip(rows["basis"], rows["item_id"], rows["fiscal_year"], strict=True))

    assert keys == sorted(keys)
    assert compute_item_coverage_rows(STANDARDIZED, UNIVERSE, ItemCoverageOptions(bases=("annual", "instant"))).equals(rows)


def test_gate_reports_items_meeting_the_spec_threshold():
    rows = pd.DataFrame(
        [
            {"item_id": i, "canonical_code": f"item_{i}", "basis": "annual", "fiscal_year": 2020,
             "n_securities": 100, "n_with_value": 95, "coverage_pct": 95.0}
            for i in range(1, 112)
        ]
        + [
            {"item_id": 9999, "canonical_code": "thin", "basis": "annual", "fiscal_year": 2020,
             "n_securities": 100, "n_with_value": 10, "coverage_pct": 10.0}
        ]
    )
    gate = evaluate_item_coverage_gate(rows, minimum_fiscal_year=2015)

    assert gate["items_meeting_threshold"] == 111
    assert gate["target_items"] == ITEM_COVERAGE_TARGET_ITEMS
    assert gate["target_coverage_pct"] == ITEM_COVERAGE_TARGET_PCT
    assert gate["status"] == "passed"
    assert gate["shortfall_items"] == 0


def test_gate_fails_below_the_item_threshold():
    rows = pd.DataFrame(
        [
            {"item_id": i, "canonical_code": f"item_{i}", "basis": "annual", "fiscal_year": 2020,
             "n_securities": 100, "n_with_value": 95, "coverage_pct": 95.0}
            for i in range(1, 47)
        ]
    )
    gate = evaluate_item_coverage_gate(rows, minimum_fiscal_year=2015)

    assert gate["items_meeting_threshold"] == 46
    assert gate["status"] == "degraded"
    assert gate["shortfall_items"] == 64


def test_gate_evaluates_only_the_annual_basis():
    """A thin non-annual basis for an item must not drag its annual pass/fail down
    (and a strong non-annual basis must not paper over a thin annual one) --
    the spec gate is stated per fiscal year over annual filings, and
    ``render_item_coverage_markdown`` renders the annual slice by default, so
    the two must agree on the same basis."""
    rows = pd.DataFrame(
        [
            {"item_id": 1, "canonical_code": "revenue", "basis": "annual", "fiscal_year": 2020,
             "n_securities": 100, "n_with_value": 95, "coverage_pct": 95.0},
            {"item_id": 1, "canonical_code": "revenue", "basis": "quarterly", "fiscal_year": 2020,
             "n_securities": 100, "n_with_value": 10, "coverage_pct": 10.0},
            {"item_id": 2, "canonical_code": "total_assets", "basis": "annual", "fiscal_year": 2020,
             "n_securities": 100, "n_with_value": 5, "coverage_pct": 5.0},
            {"item_id": 2, "canonical_code": "total_assets", "basis": "quarterly", "fiscal_year": 2020,
             "n_securities": 100, "n_with_value": 95, "coverage_pct": 95.0},
        ]
    )
    gate = evaluate_item_coverage_gate(rows, minimum_items=1, minimum_fiscal_year=2015)

    # item 1 passes on its annual coverage despite a thin quarterly basis;
    # item 2 fails on its annual coverage despite a strong quarterly basis.
    assert gate["items_meeting_threshold"] == 1
    assert gate["basis"] == "annual"


def test_markdown_renders_a_stable_document():
    rows = compute_item_coverage_rows(STANDARDIZED, UNIVERSE, ItemCoverageOptions(bases=("annual", "instant")))
    text = render_item_coverage_markdown(rows, generated_from="tests/test_item_coverage.py")

    assert text.startswith("# Standardized item coverage\n")
    assert "| item_id | canonical_code | basis | fiscal_year |" in text
    assert "tests/test_item_coverage.py" in text
    assert text == render_item_coverage_markdown(rows, generated_from="tests/test_item_coverage.py")
    assert "now()" not in text


def test_refresh_writes_the_coverage_table(tmp_store):
    _insert_universe_rows(
        tmp_store,
        (
            ("S1", "AAA", dt.date(2020, 1, 1), None),
            ("S2", "BBB", dt.date(2020, 1, 1), None),
        ),
    )
    # fundamental_standardized.value is NOT NULL in the committed schema (the
    # brief's fixture inserted a NULL value for S2, which the real table
    # rejects), so "S2 does not report item 1001" is modeled as: S2 simply has
    # no standardized row for it. S2 still lands in n_securities because the
    # FY2020 cohort is derived from universe_membership's own valid_from/
    # valid_to range, not from whether the security has any facts that year.
    _insert_standardized_row(
        tmp_store, standardized_id="std-S1-revenue-annual", security_id="S1",
        item_id=1001, canonical_code="revenue", basis="annual", fiscal_year=2020, value=10.0,
    )

    written = refresh_item_coverage(tmp_store, ItemCoverageOptions(bases=("annual",), item_ids=(1001,)))
    row = tmp_store.con.execute(
        "SELECT n_securities, n_with_value, coverage_pct FROM fundamental_item_coverage WHERE item_id = 1001"
    ).fetchone()

    assert written == 1
    assert row == (2, 1, pytest.approx(50.0))


def test_universe_cohort_derived_from_membership_date_ranges_not_facts(tmp_store):
    """S2 is a current constituent that filed nothing this year -- it must still
    count in n_securities (as 0% for the item). S3 left the universe before
    FY2020 -- it must NOT count, even though it once was a member."""
    _insert_universe_rows(
        tmp_store,
        (
            ("S1", "AAA", dt.date(2020, 1, 1), None),
            ("S2", "BBB", dt.date(2020, 1, 1), None),
            ("S3", "CCC", dt.date(2018, 1, 1), dt.date(2019, 12, 31)),
        ),
    )
    _insert_standardized_row(
        tmp_store, standardized_id="std-S1-revenue-annual", security_id="S1",
        item_id=1001, canonical_code="revenue", basis="annual", fiscal_year=2020, value=10.0,
    )

    refresh_item_coverage(tmp_store, ItemCoverageOptions(bases=("annual",), item_ids=(1001,)))
    row = tmp_store.con.execute(
        "SELECT n_securities, n_with_value, coverage_pct FROM fundamental_item_coverage WHERE item_id = 1001"
    ).fetchone()

    # n_securities == 2 (S1, S2), not 3 (S3 excluded) and not 1 (S2 without
    # facts still counted).
    assert row == (2, 1, pytest.approx(50.0))


def test_refresh_scopes_delete_to_the_recomputed_bases(tmp_store):
    """A --basis annual refresh must not delete previously published rows for
    other bases of the same (source, universe_id)."""
    _insert_universe_rows(tmp_store, (("S1", "AAA", dt.date(2020, 1, 1), None),))
    _insert_standardized_row(
        tmp_store, standardized_id="std-S1-revenue-annual", security_id="S1",
        item_id=1001, canonical_code="revenue", basis="annual", fiscal_year=2020, value=10.0,
    )
    _insert_standardized_row(
        tmp_store, standardized_id="std-S1-revenue-quarterly", security_id="S1",
        item_id=1001, canonical_code="revenue", basis="quarterly", fiscal_year=2020, value=3.0,
    )

    written_full = refresh_item_coverage(tmp_store, ItemCoverageOptions(item_ids=(1001,)))
    assert written_full == 4  # annual, quarterly, instant, ttm
    before_count = tmp_store.con.execute(
        "SELECT count(*) FROM fundamental_item_coverage WHERE item_id = 1001"
    ).fetchone()[0]
    assert before_count == 4

    written_annual = refresh_item_coverage(
        tmp_store, ItemCoverageOptions(bases=("annual",), item_ids=(1001,))
    )
    assert written_annual == 1

    after_count = tmp_store.con.execute(
        "SELECT count(*) FROM fundamental_item_coverage WHERE item_id = 1001"
    ).fetchone()[0]
    surviving_bases = {
        row[0]
        for row in tmp_store.con.execute(
            "SELECT DISTINCT basis FROM fundamental_item_coverage WHERE item_id = 1001"
        ).fetchall()
    }

    assert after_count == 4
    assert surviving_bases == {"annual", "quarterly", "instant", "ttm"}


def test_migration_0301_is_registered_and_idempotent(tmp_store):
    from atx_db.migrations.registry import MIGRATIONS

    versions = [migration.version for migration in MIGRATIONS]
    assert 301 in versions
    assert versions == sorted(versions)
    recorded = tmp_store.con.execute(
        "SELECT count(*) FROM schema_migrations WHERE version = 301"
    ).fetchone()[0]
    catalogued = tmp_store.con.execute(
        "SELECT count(*) FROM table_catalog WHERE table_name = 'fundamental_item_coverage'"
    ).fetchone()[0]

    assert recorded == 1
    assert catalogued == 1


def test_pit_exemption_documents_is_latest_revision_for_the_coverage_table(tmp_store):
    row = tmp_store.con.execute(
        "SELECT table_name, missing_columns, reason FROM pit_exemption WHERE table_name = 'fundamental_item_coverage'"
    ).fetchone()

    assert row is not None
    table_name, missing_columns, reason = row
    assert table_name == "fundamental_item_coverage"
    assert json.loads(missing_columns) == ["is_latest_revision"]
    assert "revision chain" in reason.lower()
