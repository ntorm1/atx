"""PF-S7 S7-1 tests for the referential (multi-table orphan) quality check type.

``ReferentialQualityCheck`` is the first-class replacement for the hand-written
``LEFT JOIN ... WHERE parent.key IS NULL`` strings scattered through
``db.quality._check_specs``. It compiles down to a plain ``SqlQualityCheck`` so
``run_warehouse_quality_checks`` needs no changes to execute it -- including the
existing ``required_tables``/``warn_if_missing`` no-op path for a parent table
that does not (yet) exist.

Coverage:
- the compiled SQL/dataclass shape (unit, no DB)
- a PLANTED ORPHAN fixture on the registered ``fundamental_ratios ->
  fundamental_points`` check goes RED
- a clean fixture on the same check goes GREEN (0 orphans)
- a NULL child key (``item_id``) is SKIPPED, not counted as an orphan, on the
  registered ``fundamental_points -> fundamental_item`` and
  ``fundamental_statement_points -> fundamental_item`` checks
- the check no-ops (status "warning", not "failed") when the parent table is
  absent
- existing single-table checks are unaffected (additive only)
"""

from __future__ import annotations

import datetime as dt


def _quality_by_name(store, *check_names: str):
    from db.quality import run_warehouse_quality_checks

    return {
        r.check_name: r
        for r in run_warehouse_quality_checks(
            store,
            record=False,
            check_names=check_names,
        )
    }


def _insert_fundamental_ratio(
    con, *, ratio_id: str, security_id: str, period_end: str = "2023-12-31"
) -> None:
    con.execute(
        """
        INSERT INTO fundamental_ratios (
            ratio_id, source, security_id, ratio_code, ratio_category, ratio_kind,
            basis, unit, period_end, as_of_date, available_at
        )
        VALUES (
            ?, 'fixture', ?, 'gross_margin', 'profitability', 'ratio',
            'ttm', 'ratio', ?, ?,
            TIMESTAMP '2024-02-15 22:00:00'
        )
        """,
        [
            ratio_id,
            security_id,
            dt.date.fromisoformat(period_end),
            dt.date.fromisoformat(period_end),
        ],
    )


def _insert_fundamental_point(
    con,
    *,
    security_id: str,
    metric: str = "revenue",
    item_id: int | None = None,
    period_end: str = "2023-12-31",
) -> None:
    con.execute(
        """
        INSERT INTO fundamental_points (
            source, security_id, metric, item_id, period_end, as_of_date
        )
        VALUES ('fixture', ?, ?, ?, ?, ?)
        """,
        [
            security_id,
            metric,
            item_id,
            dt.date.fromisoformat(period_end),
            dt.date.fromisoformat(period_end),
        ],
    )


def _insert_fundamental_statement_point(
    con, *, statement_point_id: str, security_id: str, item_id: int | None = None
) -> None:
    available_at = dt.datetime(2024, 2, 15, 22, 0, 0)
    cols = {
        "statement_point_id": statement_point_id,
        "fact_revision_id": f"fact-{statement_point_id}",
        "revision_group_id": f"group-{statement_point_id}",
        "source": "fixture",
        "security_id": security_id,
        "symbol": "TST",
        "cik": "0000012345",
        "statement_type": "income_statement",
        "statement_section": "revenue",
        "canonical_metric": "revenue",
        "canonical_label": "Revenue",
        "taxonomy": "us-gaap",
        "concept": "Revenues",
        "unit": "USD",
        "unit_type": "monetary",
        "period_type": "duration",
        "normal_balance": "credit",
        "period_start": dt.date(2023, 1, 1),
        "period_end": dt.date(2023, 12, 31),
        "as_of_date": dt.date(2024, 2, 15),
        "available_at": available_at,
        "item_id": item_id,
        "accession_number": f"acc-{statement_point_id}",
        "revision_sequence": 1,
        "revision_count": 1,
        "is_latest_revision": True,
        "is_value_changed": False,
        "source_url": "file://offline",
        "source_loaded_at": available_at,
        "updated_at": available_at,
    }
    keys = ", ".join(cols)
    con.execute(
        f"INSERT INTO fundamental_statement_points ({keys}) VALUES ({', '.join(['?'] * len(cols))})",
        list(cols.values()),
    )


def _insert_fundamental_item(con, item_id: int, canonical_code: str = "revenue") -> None:
    con.execute(
        "INSERT INTO fundamental_item (item_id, canonical_code) VALUES (?, ?)",
        [item_id, canonical_code],
    )


# ---------------------------------------------------------------------------
# Unit tests: the ReferentialQualityCheck type itself (no DB)
# ---------------------------------------------------------------------------


def test_referential_quality_check_compiles_to_sql_quality_check():
    from db.quality import ReferentialQualityCheck, SqlQualityCheck

    spec = ReferentialQualityCheck(
        dataset_id="widgets",
        check_name="widgets_without_makers",
        child_table="widgets",
        child_key="maker_id",
        parent_table="makers",
        parent_key="maker_id",
    )
    compiled = spec.compile()

    assert isinstance(compiled, SqlQualityCheck)
    assert compiled.dataset_id == "widgets"
    assert compiled.check_name == "widgets_without_makers"
    # table_name defaults to the child table when not overridden.
    assert compiled.table_name == "widgets"
    assert compiled.threshold == 0.0
    assert compiled.comparator == "eq"
    assert compiled.required_tables == ("widgets", "makers")
    assert compiled.warn_if_missing is True
    assert compiled.failure_status == "failed"
    # Exact anti-join semantics: NULL child key excluded from both sides.
    assert "LEFT JOIN makers p" in compiled.sql
    assert "ON p.maker_id = c.maker_id" in compiled.sql
    assert "c.maker_id IS NOT NULL" in compiled.sql
    assert "p.maker_id IS NULL" in compiled.sql


def test_referential_quality_check_compiles_composite_join_keys():
    from db.quality import ReferentialQualityCheck

    spec = ReferentialQualityCheck(
        dataset_id="widgets",
        check_name="widgets_without_makers",
        child_table="widgets",
        parent_table="makers",
        child_keys=("maker_id", "period_end"),
        parent_keys=("maker_id", "period_end"),
    )
    compiled = spec.compile()

    assert "ON p.maker_id = c.maker_id AND p.period_end = c.period_end" in compiled.sql
    assert "c.maker_id IS NOT NULL AND c.period_end IS NOT NULL" in compiled.sql
    assert "p.maker_id IS NULL" in compiled.sql


def test_referential_quality_check_table_name_override():
    from db.quality import ReferentialQualityCheck

    spec = ReferentialQualityCheck(
        dataset_id="widgets",
        check_name="widgets_without_makers",
        child_table="widgets",
        child_key="maker_id",
        parent_table="makers",
        parent_key="maker_id",
        table_name="widget_catalog",
    )
    assert spec.compile().table_name == "widget_catalog"


def test_referential_check_specs_are_registered_in_check_specs():
    from db.quality import _check_specs

    names = {
        s.check_name
        for s in _check_specs(daily_macro_stale_days=10, monthly_macro_stale_days=70)
    }
    assert "fundamental_ratios_without_fundamental_points" in names
    assert "fundamental_points_item_without_fundamental_item" in names
    assert "fundamental_statement_points_item_without_fundamental_item" in names


# ---------------------------------------------------------------------------
# fundamental_ratios -> fundamental_points (or fundamental_statement_points)
# ---------------------------------------------------------------------------


def test_planted_orphan_fundamental_ratio_is_red(tmp_store):
    """A fundamental_ratios row whose security has NO backing fundamental_points
    row is an orphan -> the check goes red."""
    con = tmp_store.con
    _insert_fundamental_ratio(con, ratio_id="ratio-orphan", security_id="SEC-ORPHAN")

    results = _quality_by_name(tmp_store, "fundamental_ratios_without_fundamental_points")
    check = results["fundamental_ratios_without_fundamental_points"]

    assert check.status == "failed"
    assert check.observed_value == 1.0


def test_clean_fundamental_ratio_with_backing_points_is_green(tmp_store):
    """A fundamental_ratios row whose security DOES have a backing
    fundamental_points row resolves cleanly -> 0 orphans, green."""
    con = tmp_store.con
    _insert_fundamental_ratio(con, ratio_id="ratio-ok", security_id="SEC-OK")
    _insert_fundamental_point(con, security_id="SEC-OK")

    results = _quality_by_name(tmp_store, "fundamental_ratios_without_fundamental_points")
    check = results["fundamental_ratios_without_fundamental_points"]

    assert check.status == "passed"
    assert check.observed_value == 0.0


def test_fundamental_ratio_same_security_wrong_period_is_red(tmp_store):
    """A security-only parent row is not enough: ratios resolve to points on
    at least (security_id, period_end)."""
    con = tmp_store.con
    _insert_fundamental_ratio(
        con, ratio_id="ratio-wrong-period", security_id="SEC-A", period_end="2023-12-31"
    )
    _insert_fundamental_point(con, security_id="SEC-A", period_end="2022-12-31")

    results = _quality_by_name(tmp_store, "fundamental_ratios_without_fundamental_points")
    check = results["fundamental_ratios_without_fundamental_points"]

    assert check.status == "failed"
    assert check.observed_value == 1.0


# ---------------------------------------------------------------------------
# fundamental_points / fundamental_statement_points -> fundamental_item
# ---------------------------------------------------------------------------


def test_fundamental_points_item_id_orphan_is_red(tmp_store):
    """A fundamental_points row whose item_id has no matching fundamental_item
    row is an orphan -> red, counting only the unresolved row."""
    con = tmp_store.con
    _insert_fundamental_item(con, item_id=9001, canonical_code="revenue")
    # Resolves cleanly (item_id 9001 exists).
    _insert_fundamental_point(con, security_id="SEC-A", metric="revenue", item_id=9001)
    # Orphan: item_id 9999 does not exist in fundamental_item.
    _insert_fundamental_point(con, security_id="SEC-A", metric="mystery_metric", item_id=9999)

    results = _quality_by_name(tmp_store, "fundamental_points_item_without_fundamental_item")
    check = results["fundamental_points_item_without_fundamental_item"]

    assert check.status == "failed"
    assert check.observed_value == 1.0


def test_fundamental_points_null_item_id_is_skipped_not_orphan(tmp_store):
    """A NULL item_id is not-yet-mapped, not an orphan -- it must be skipped
    entirely (not counted on either side of the anti-join)."""
    con = tmp_store.con
    # No fundamental_item rows at all; if the NULL key were treated as an
    # orphan this would incorrectly go red.
    _insert_fundamental_point(con, security_id="SEC-A", metric="unmapped_metric", item_id=None)

    results = _quality_by_name(tmp_store, "fundamental_points_item_without_fundamental_item")
    check = results["fundamental_points_item_without_fundamental_item"]

    assert check.status == "passed"
    assert check.observed_value == 0.0


def test_fundamental_statement_points_item_orphan_and_null_skip(tmp_store):
    """Same orphan + NULL-skip semantics for the fundamental_statement_points
    variant of the item-resolution check."""
    con = tmp_store.con
    _insert_fundamental_item(con, item_id=9002, canonical_code="net_income")
    _insert_fundamental_statement_point(
        con, statement_point_id="stmt-ok", security_id="SEC-B", item_id=9002
    )
    _insert_fundamental_statement_point(
        con, statement_point_id="stmt-orphan", security_id="SEC-B", item_id=9999
    )
    _insert_fundamental_statement_point(
        con, statement_point_id="stmt-unmapped", security_id="SEC-B", item_id=None
    )

    results = _quality_by_name(tmp_store, "fundamental_statement_points_item_without_fundamental_item")
    check = results["fundamental_statement_points_item_without_fundamental_item"]

    assert check.status == "failed"
    # Only the item_id=9999 row counts; the NULL item_id row is skipped.
    assert check.observed_value == 1.0


# ---------------------------------------------------------------------------
# No-op when the parent table is absent
# ---------------------------------------------------------------------------


def test_referential_check_no_ops_when_parent_table_missing(tmp_store):
    """If a forward-looking parent table does not exist yet, the check must
    no-op (status 'warning'), not fail and not raise.

    Points a check at a genuinely nonexistent table name (rather than
    dropping a real, already-migrated table) so the fixture reflects an
    actual "table not landed yet" state and cannot collide with unrelated
    pre-existing checks/views that also depend on the real fundamentals
    tables. ``_check_specs`` is patched to return only this one spec so the
    test exercises exactly the no-op path under test.
    """
    from unittest.mock import patch

    from db.quality import ReferentialQualityCheck

    con = tmp_store.con
    _insert_fundamental_point(con, security_id="SEC-A", metric="revenue", item_id=1)

    spec = ReferentialQualityCheck(
        dataset_id="fundamental_points",
        check_name="fundamental_points_item_without_not_yet_landed_parent",
        child_table="fundamental_points",
        child_key="item_id",
        parent_table="fundamental_statement_points_v2_not_yet_landed",
        parent_key="item_id",
    )
    with patch("db.quality._check_specs", return_value=(spec.compile(),)):
        results = _quality_by_name(tmp_store, spec.check_name)

    check = results[spec.check_name]
    assert check.status == "warning"
    assert check.observed_value is None
    assert check.details["missing_tables"] == ["fundamental_statement_points_v2_not_yet_landed"]


def test_referential_check_no_ops_when_child_table_missing(tmp_store):
    """Symmetrically, a missing child table also no-ops rather than raising."""
    from unittest.mock import patch

    from db.quality import ReferentialQualityCheck

    spec = ReferentialQualityCheck(
        dataset_id="fundamental_ratios",
        check_name="not_yet_landed_child_without_fundamental_points",
        child_table="fundamental_ratios_v2_not_yet_landed",
        child_key="security_id",
        parent_table="fundamental_points",
        parent_key="security_id",
    )
    with patch("db.quality._check_specs", return_value=(spec.compile(),)):
        results = _quality_by_name(tmp_store, spec.check_name)

    check = results[spec.check_name]
    assert check.status == "warning"
    assert check.observed_value is None
    assert check.details["missing_tables"] == ["fundamental_ratios_v2_not_yet_landed"]


# ---------------------------------------------------------------------------
# Additive-only: existing single-table checks unaffected
# ---------------------------------------------------------------------------


def test_existing_single_table_checks_still_present(tmp_store):
    results = _quality_by_name(
        tmp_store,
        "orphan_equity_daily_bars",
        "xbrl_filing_facts_without_context",
        "duplicate_fundamental_ratio_natural_keys",
    )

    # A sample of pre-existing hand-rolled orphan/scalar checks must still run.
    for check_name in (
        "orphan_equity_daily_bars",
        "xbrl_filing_facts_without_context",
        "duplicate_fundamental_ratio_natural_keys",
    ):
        assert check_name in results, f"{check_name!r} missing -- referential type must be additive"
        assert results[check_name].status in {"passed", "failed", "warning"}
