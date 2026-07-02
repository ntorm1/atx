"""PF-S1 S1-4 tests for fact-to-item reference links.

The S1-4 surface is additive: migration 0064 adds nullable item_id columns to
statement and raw XBRL fact tables, backfills them from local registry data, and
adds quality checks for duplicate and unmapped concept resolution. Tests stay
fully offline and use only fixture rows plus the committed seed CSV.
"""

from __future__ import annotations

import datetime as dt


def _columns(store, table_name: str) -> list[str]:
    rows = store.con.execute(
        """
        SELECT column_name
        FROM information_schema.columns
        WHERE table_schema = 'main' AND table_name = ?
        ORDER BY ordinal_position
        """,
        [table_name],
    ).fetchall()
    return [row[0] for row in rows]


def _insert_statement_point(
    store,
    *,
    statement_point_id: str,
    concept: str,
    canonical_metric: str,
    value: float = 100.0,
    taxonomy: str = "us-gaap",
) -> None:
    available_at = dt.datetime(2024, 2, 15, 22, 0, 0)
    cols = {
        "statement_point_id": statement_point_id,
        "fact_revision_id": f"fact-{statement_point_id}",
        "revision_group_id": f"group-{statement_point_id}",
        "source": "SEC companyfacts",
        "security_id": "SEC-S1-4",
        "symbol": "TST",
        "cik": "0000012345",
        "statement_type": "income_statement",
        "statement_section": "revenue",
        "canonical_metric": canonical_metric,
        "canonical_label": canonical_metric.replace("_", " ").title(),
        "taxonomy": taxonomy,
        "concept": concept,
        "unit": "USD",
        "unit_type": "monetary",
        "period_type": "duration",
        "normal_balance": "credit",
        "period_start": dt.date(2023, 1, 1),
        "period_end": dt.date(2023, 12, 31),
        "as_of_date": dt.date(2024, 2, 15),
        "available_at": available_at,
        "fiscal_year": 2023,
        "fiscal_period": "FY",
        "form": "10-K",
        "accession_number": f"acc-{statement_point_id}",
        "revision_sequence": 1,
        "revision_count": 1,
        "is_latest_revision": True,
        "is_value_changed": False,
        "raw_value": value,
        "value": value,
        "previous_raw_value": None,
        "previous_value": None,
        "value_delta": None,
        "value_delta_percent": None,
        "run_id": "s1-4-test",
        "source_url": "file://offline",
        "source_loaded_at": available_at,
        "updated_at": available_at,
    }
    keys = ", ".join(cols)
    store.con.execute(
        f"INSERT INTO fundamental_statement_points ({keys}) VALUES ({', '.join(['?'] * len(cols))})",
        list(cols.values()),
    )


def _insert_raw_point(
    store,
    *,
    metric: str,
    value: float = 200.0,
    taxonomy: str = "us-gaap",
) -> None:
    available_at = dt.datetime(2024, 2, 15, 22, 0, 0)
    store.con.execute(
        """
        INSERT INTO fundamental_points (
            source, security_id, symbol, metric, taxonomy, unit,
            period_start, period_end, as_of_date, fiscal_year, fiscal_period,
            form, accession_number, value, available_at, run_id, source_loaded_at
        )
        VALUES (
            'SEC companyfacts', 'SEC-S1-4', 'TST', ?, ?, 'USD',
            DATE '2023-01-01', DATE '2023-12-31', DATE '2024-02-15', 2023, 'FY',
            '10-K', ?, ?, ?, 's1-4-test', ?
        )
        """,
        [metric, taxonomy, f"acc-raw-{metric}", value, available_at, available_at],
    )


def _snapshot_without_item_id(store, table_name: str, order_by: str):
    columns = [column for column in _columns(store, table_name) if column != "item_id"]
    column_sql = ", ".join(columns)
    return store.con.execute(
        f"SELECT {column_sql} FROM {table_name} ORDER BY {order_by}"
    ).fetchall()


def test_migration_0064_adds_nullable_item_id_columns_and_catalogs(tmp_store):
    rows = tmp_store.con.execute(
        """
        SELECT CAST(version AS INTEGER)
        FROM schema_migrations
        WHERE version ~ '^[0-9]+$'
        """
    ).fetchall()
    versions = {row[0] for row in rows}
    assert 64 in versions

    for table_name in ("fundamental_statement_points", "fundamental_points"):
        column = tmp_store.con.execute(
            """
            SELECT is_nullable
            FROM information_schema.columns
            WHERE table_schema = 'main'
              AND table_name = ?
              AND column_name = 'item_id'
            """,
            [table_name],
        ).fetchone()
        assert column == ("YES",)

        catalog_row = tmp_store.con.execute(
            """
            SELECT semantic_type, nullable
            FROM field_catalog
            WHERE table_name = ?
              AND field_name = 'item_id'
            """,
            [table_name],
        ).fetchone()
        assert catalog_row == ("identifier", True)


def test_migration_0064_backfills_item_ids_idempotently_and_preserves_fact_bytes(tmp_store):
    from db.item_registry import seed_fundamental_item_registry
    from db.migrations import _fundamental_fact_item_links

    seed_fundamental_item_registry(tmp_store)
    _insert_statement_point(
        tmp_store,
        statement_point_id="stmt-revenue",
        concept="Revenues",
        canonical_metric="revenue",
    )
    _insert_statement_point(
        tmp_store,
        statement_point_id="stmt-unmapped",
        concept="UnmappedStatementConcept",
        canonical_metric="unmapped_statement_metric",
    )
    _insert_raw_point(tmp_store, metric="NetIncomeLoss")
    _insert_raw_point(tmp_store, metric="UnmappedRawConcept")

    before_statement = _snapshot_without_item_id(
        tmp_store, "fundamental_statement_points", "statement_point_id"
    )
    before_raw = _snapshot_without_item_id(
        tmp_store, "fundamental_points", "security_id, metric, accession_number"
    )

    _fundamental_fact_item_links(tmp_store.con)
    _fundamental_fact_item_links(tmp_store.con)

    statement_ids = dict(
        tmp_store.con.execute(
            """
            SELECT statement_point_id, item_id
            FROM fundamental_statement_points
            ORDER BY statement_point_id
            """
        ).fetchall()
    )
    raw_ids = dict(
        tmp_store.con.execute(
            """
            SELECT metric, item_id
            FROM fundamental_points
            ORDER BY metric
            """
        ).fetchall()
    )

    assert statement_ids == {"stmt-revenue": 1001, "stmt-unmapped": None}
    assert raw_ids == {"NetIncomeLoss": 1031, "UnmappedRawConcept": None}
    assert _snapshot_without_item_id(
        tmp_store, "fundamental_statement_points", "statement_point_id"
    ) == before_statement
    assert _snapshot_without_item_id(
        tmp_store, "fundamental_points", "security_id, metric, accession_number"
    ) == before_raw

    catalog_count = tmp_store.con.execute(
        """
        SELECT count(*)
        FROM field_catalog
        WHERE (table_name, field_name) IN (
            ('fundamental_statement_points', 'item_id'),
            ('fundamental_points', 'item_id')
        )
        """
    ).fetchone()[0]
    assert catalog_count == 2


def test_real_seed_has_no_duplicate_concept_to_item_mappings(tmp_store):
    from db.item_registry import seed_fundamental_item_registry
    from db.quality import run_warehouse_quality_checks

    seed_fundamental_item_registry(tmp_store)
    results = {r.check_name: r for r in run_warehouse_quality_checks(tmp_store, record=False)}

    result = results["duplicate_fundamental_item_alias_item_mappings"]
    assert result.status == "passed"
    assert result.observed_value == 0.0


def test_duplicate_concept_to_item_mapping_quality_check_fails(tmp_store):
    from db.item_registry import seed_fundamental_item_registry
    from db.quality import run_warehouse_quality_checks

    seed_fundamental_item_registry(tmp_store)
    tmp_store.con.execute(
        """
        INSERT INTO fundamental_item_alias (
            item_id, alias_scheme, alias_code, coalesce_priority, valid_from, valid_to
        )
        VALUES (1002, 'us-gaap', 'NetIncomeLoss', 999, DATE '1900-01-01', NULL)
        """
    )

    results = {r.check_name: r for r in run_warehouse_quality_checks(tmp_store, record=False)}
    result = results["duplicate_fundamental_item_alias_item_mappings"]

    assert result.status == "failed"
    assert result.observed_value == 1.0
    assert result.details["rows"][0]["concept"] == "NetIncomeLoss"
    assert result.details["rows"][0]["item_id_count"] == 2


def test_unmapped_fact_concepts_quality_check_warns_with_details(tmp_store):
    from db.item_registry import seed_fundamental_item_registry
    from db.migrations import _fundamental_fact_item_links
    from db.quality import run_warehouse_quality_checks

    seed_fundamental_item_registry(tmp_store)
    _insert_statement_point(
        tmp_store,
        statement_point_id="stmt-unmapped",
        concept="UnmappedStatementConcept",
        canonical_metric="unmapped_statement_metric",
    )
    _insert_raw_point(tmp_store, metric="UnmappedRawConcept")
    _fundamental_fact_item_links(tmp_store.con)

    results = {r.check_name: r for r in run_warehouse_quality_checks(tmp_store, record=False)}
    result = results["unmapped_fundamental_fact_concepts"]
    detail_pairs = {
        (row["fact_table"], row["taxonomy"], row["concept"])
        for row in result.details["rows"]
    }

    assert result.status == "warning"
    assert result.observed_value == 2.0
    assert (
        "fundamental_statement_points",
        "us-gaap",
        "UnmappedStatementConcept",
    ) in detail_pairs
    assert ("fundamental_points", "us-gaap", "UnmappedRawConcept") in detail_pairs
