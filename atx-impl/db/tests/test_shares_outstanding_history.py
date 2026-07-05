from __future__ import annotations

import datetime as dt


SECURITY_ID = "SEC-CIK-0000320193"


def _seed_security(tmp_store) -> None:
    tmp_store.con.execute(
        """
        INSERT INTO securities (
            security_id,
            issuer_id,
            primary_symbol,
            name,
            source
        )
        VALUES (?, ?, ?, ?, ?)
        """,
        [SECURITY_ID, SECURITY_ID, "AAPL", "Apple Inc.", "test"],
    )


def _insert_statement_point(
    tmp_store,
    *,
    statement_point_id: str,
    metric: str,
    value: float,
    period_start: dt.date | None,
    period_end: dt.date,
    as_of_date: dt.date,
    available_at: dt.datetime,
    accession_number: str,
    period_type: str = "instant",
    is_latest_revision: bool = True,
    taxonomy: str = "dei",
    concept: str | None = None,
    unit: str = "shares",
    unit_type: str = "shares",
) -> None:
    concept = concept or "EntityCommonStockSharesOutstanding"
    tmp_store.con.execute(
        """
        INSERT INTO fundamental_statement_points (
            statement_point_id,
            fact_revision_id,
            revision_group_id,
            source,
            security_id,
            symbol,
            cik,
            statement_type,
            statement_section,
            canonical_metric,
            canonical_label,
            taxonomy,
            concept,
            unit,
            unit_type,
            period_type,
            normal_balance,
            period_start,
            period_end,
            as_of_date,
            available_at,
            fiscal_year,
            fiscal_period,
            form,
            accession_number,
            revision_sequence,
            revision_count,
            is_latest_revision,
            is_value_changed,
            raw_value,
            value,
            previous_raw_value,
            previous_value,
            value_delta,
            value_delta_percent,
            run_id,
            source_url,
            source_loaded_at
        )
        VALUES (
            ?, ?, ?, 'SEC companyfacts API', ?, 'AAPL', '0000320193',
            'share_count', 'shares', ?, ?, ?,
            ?, ?, ?, ?, 'credit',
            ?, ?, ?, ?, 2024, 'FY', '10-K', ?, 1, 1, ?, false,
            ?, ?, NULL, NULL, NULL, NULL, 'seed-run',
            'https://data.sec.gov/submissions/CIK0000320193.json', ?
        )
        """,
        [
            statement_point_id,
            f"fact-{statement_point_id}",
            f"group-{statement_point_id}",
            SECURITY_ID,
            metric,
            metric.replace("_", " ").title(),
            taxonomy,
            concept,
            unit,
            unit_type,
            period_type,
            period_start,
            period_end,
            as_of_date,
            available_at,
            accession_number,
            is_latest_revision,
            value,
            value,
            available_at,
        ],
    )


def _seed_share_statement_points(tmp_store) -> None:
    _seed_security(tmp_store)
    _insert_statement_point(
        tmp_store,
        statement_point_id="shares-2023q3",
        metric="shares_outstanding",
        value=15_550_061_000,
        period_start=None,
        period_end=dt.date(2023, 9, 30),
        as_of_date=dt.date(2023, 10, 27),
        available_at=dt.datetime(2023, 10, 27, 21, 30),
        accession_number="0000320193-23-000106",
    )
    _insert_statement_point(
        tmp_store,
        statement_point_id="shares-2023fy",
        metric="shares_outstanding",
        value=15_442_000_000,
        period_start=None,
        period_end=dt.date(2023, 12, 30),
        as_of_date=dt.date(2024, 2, 2),
        available_at=dt.datetime(2024, 2, 2, 21, 30),
        accession_number="0000320193-24-000006",
    )
    _insert_statement_point(
        tmp_store,
        statement_point_id="shares-basic-2023fy",
        metric="shares_basic_avg",
        value=15_744_231_000,
        period_start=dt.date(2023, 1, 1),
        period_end=dt.date(2023, 12, 30),
        as_of_date=dt.date(2024, 2, 2),
        available_at=dt.datetime(2024, 2, 2, 21, 30),
        accession_number="0000320193-24-000006",
        period_type="duration",
    )
    _insert_statement_point(
        tmp_store,
        statement_point_id="negative-ignored",
        metric="shares_diluted_avg",
        value=-1,
        period_start=dt.date(2023, 1, 1),
        period_end=dt.date(2023, 12, 30),
        as_of_date=dt.date(2024, 2, 2),
        available_at=dt.datetime(2024, 2, 2, 21, 30),
        accession_number="0000320193-24-000006",
        period_type="duration",
    )
    _insert_statement_point(
        tmp_store,
        statement_point_id="non-share-ignored",
        metric="revenue",
        value=383_285_000_000,
        period_start=dt.date(2023, 1, 1),
        period_end=dt.date(2023, 12, 30),
        as_of_date=dt.date(2024, 2, 2),
        available_at=dt.datetime(2024, 2, 2, 21, 30),
        accession_number="0000320193-24-000006",
        period_type="duration",
    )


def test_refresh_materializes_public_sec_share_count_history(tmp_store):
    from db.shares_outstanding import refresh_shares_outstanding_history

    _seed_share_statement_points(tmp_store)

    assert refresh_shares_outstanding_history(tmp_store) == 3
    assert refresh_shares_outstanding_history(tmp_store) == 3

    rows = tmp_store.con.execute(
        """
        SELECT
            share_count_type,
            effective_date,
            share_count,
            accession_number,
            source
        FROM shares_outstanding_history
        ORDER BY share_count_type, effective_date
        """
    ).fetchall()

    assert rows == [
        (
            "shares_basic_avg",
            dt.date(2023, 12, 30),
            15_744_231_000,
            "0000320193-24-000006",
            "SEC XBRL share counts",
        ),
        (
            "shares_outstanding",
            dt.date(2023, 9, 30),
            15_550_061_000,
            "0000320193-23-000106",
            "SEC XBRL share counts",
        ),
        (
            "shares_outstanding",
            dt.date(2023, 12, 30),
            15_442_000_000,
            "0000320193-24-000006",
            "SEC XBRL share counts",
        ),
    ]


def test_refresh_materializes_float_treasury_and_share_class_counts(tmp_store):
    from db.shares_outstanding import refresh_shares_outstanding_history

    _seed_share_statement_points(tmp_store)
    _insert_statement_point(
        tmp_store,
        statement_point_id="float-2023fy",
        metric="float",
        value=14_000_000_000,
        period_start=None,
        period_end=dt.date(2023, 12, 30),
        as_of_date=dt.date(2024, 2, 2),
        available_at=dt.datetime(2024, 2, 2, 21, 30),
        accession_number="0000320193-24-000006",
        taxonomy="dei",
        concept="EntityPublicFloat",
    )
    _insert_statement_point(
        tmp_store,
        statement_point_id="float-too-large-ignored",
        metric="float",
        value=99_000_000_000,
        period_start=None,
        period_end=dt.date(2023, 12, 30),
        as_of_date=dt.date(2024, 2, 2),
        available_at=dt.datetime(2024, 2, 2, 21, 30),
        accession_number="0000320193-24-000006",
        taxonomy="dei",
        concept="EntityPublicFloat",
    )
    _insert_statement_point(
        tmp_store,
        statement_point_id="monetary-public-float-ignored",
        metric="float",
        value=2_500_000_000_000,
        period_start=None,
        period_end=dt.date(2023, 12, 30),
        as_of_date=dt.date(2024, 2, 2),
        available_at=dt.datetime(2024, 2, 2, 21, 30),
        accession_number="0000320193-24-000006",
        taxonomy="dei",
        concept="EntityPublicFloat",
        unit="USD",
        unit_type="monetary",
    )
    _insert_statement_point(
        tmp_store,
        statement_point_id="treasury-2023fy",
        metric="treasury",
        value=100_000_000,
        period_start=None,
        period_end=dt.date(2023, 12, 30),
        as_of_date=dt.date(2024, 2, 2),
        available_at=dt.datetime(2024, 2, 2, 21, 30),
        accession_number="0000320193-24-000006",
        taxonomy="us-gaap",
        concept="TreasuryStockShares",
    )
    _insert_statement_point(
        tmp_store,
        statement_point_id="class-a-2023fy",
        metric="class_a",
        value=10_000_000_000,
        period_start=None,
        period_end=dt.date(2023, 12, 30),
        as_of_date=dt.date(2024, 2, 2),
        available_at=dt.datetime(2024, 2, 2, 21, 30),
        accession_number="0000320193-24-000006",
        taxonomy="us-gaap",
        concept="ClassACommonStockSharesOutstanding",
    )
    _insert_statement_point(
        tmp_store,
        statement_point_id="class-b-2023fy",
        metric="class_b",
        value=5_442_000_000,
        period_start=None,
        period_end=dt.date(2023, 12, 30),
        as_of_date=dt.date(2024, 2, 2),
        available_at=dt.datetime(2024, 2, 2, 21, 30),
        accession_number="0000320193-24-000006",
        taxonomy="us-gaap",
        concept="ClassBCommonStockSharesOutstanding",
    )

    assert refresh_shares_outstanding_history(tmp_store) == 7

    rows = tmp_store.con.execute(
        """
        SELECT share_count_type, share_class, share_count_category, share_count
        FROM shares_outstanding_history
        WHERE share_count_type IN ('float', 'treasury', 'class_a', 'class_b')
        ORDER BY share_count_type
        """
    ).fetchall()
    assert rows == [
        ("class_a", "A", "share_class", 10_000_000_000),
        ("class_b", "B", "share_class", 5_442_000_000),
        ("float", None, "float_treasury", 14_000_000_000),
        ("treasury", None, "float_treasury", 100_000_000),
    ]

    legacy_counts = tmp_store.con.execute(
        """
        SELECT share_count_type, count(*)
        FROM shares_outstanding_history
        WHERE share_count_type IN ('shares_outstanding', 'shares_basic_avg', 'shares_diluted_avg')
        GROUP BY 1
        ORDER BY 1
        """
    ).fetchall()
    assert legacy_counts == [("shares_basic_avg", 1), ("shares_outstanding", 2)]


def test_dataset_wrapper_records_run_and_quality(tmp_store):
    from db.shares_outstanding import SharesOutstandingHistoryDataset, SharesOutstandingHistoryOptions

    _seed_share_statement_points(tmp_store)

    result = SharesOutstandingHistoryDataset().run(
        tmp_store,
        SharesOutstandingHistoryOptions(source="test-share-source"),
    )

    assert result.dataset_id == "shares_outstanding_history"
    assert result.rows_loaded == 3
    assert result.run_id
    assert (
        tmp_store.con.execute(
            """
            SELECT status
            FROM dataset_runs
            WHERE run_id = ?
            """,
            [result.run_id],
        ).fetchone()[0]
        == "succeeded"
    )
    assert (
        tmp_store.con.execute(
            """
            SELECT status
            FROM data_quality_checks
            WHERE dataset_id = 'shares_outstanding_history'
              AND check_name = 'rows_loaded'
            ORDER BY checked_at DESC
            LIMIT 1
            """
        ).fetchone()[0]
        == "passed"
    )


def test_shares_outstanding_asof_filters_by_availability_and_type(tmp_store):
    from db.asof import shares_outstanding_asof
    from db.shares_outstanding import refresh_shares_outstanding_history

    _seed_share_statement_points(tmp_store)
    refresh_shares_outstanding_history(tmp_store)
    db_path = tmp_store.path
    tmp_store.connection.close()
    tmp_store.connection = None

    before_fy = shares_outstanding_asof(
        dt.date(2024, 1, 15),
        db_path=db_path,
        symbols=("AAPL",),
        share_count_types=("shares_outstanding",),
    )
    after_fy = shares_outstanding_asof(
        dt.date(2024, 3, 1),
        db_path=db_path,
        symbols=("AAPL",),
        share_count_types=("shares_outstanding",),
    )
    basic = shares_outstanding_asof(
        dt.date(2024, 3, 1),
        db_path=db_path,
        symbols=("AAPL",),
        share_count_types=("shares_basic_avg",),
    )

    assert before_fy["share_count"].tolist() == [15_550_061_000]
    assert after_fy["share_count"].tolist() == [15_442_000_000]
    assert basic["share_count"].tolist() == [15_744_231_000]


def test_share_count_extension_migration_and_catalog_are_present(tmp_store):
    columns = {
        row[0]
        for row in tmp_store.con.execute(
            """
            SELECT column_name
            FROM duckdb_columns()
            WHERE table_name = 'shares_outstanding_history'
            """
        ).fetchall()
    }
    assert {"share_class", "share_count_category"}.issubset(columns)
    assert (
        tmp_store.con.execute(
            "SELECT description FROM schema_migrations WHERE version = 144"
        ).fetchone()[0]
        == "pf3_s5_share_count_extensions"
    )
    assert (
        tmp_store.con.execute(
            """
            SELECT count(*)
            FROM field_catalog
            WHERE table_name = 'shares_outstanding_history'
              AND field_name IN ('share_count_type', 'share_class', 'share_count_category', 'share_count')
            """
        ).fetchone()[0]
        == 4
    )


def test_shares_quality_checks_and_watermarks_pass_clean_sample(tmp_store):
    from db.quality import run_warehouse_quality_checks
    from db.shares_outstanding import refresh_shares_outstanding_history
    from db.watermarks import refresh_warehouse_watermarks

    _seed_share_statement_points(tmp_store)
    refresh_shares_outstanding_history(tmp_store)
    refresh_warehouse_watermarks(tmp_store)

    target_results = [
        result
        for result in run_warehouse_quality_checks(
            tmp_store,
            record=False,
            dataset_ids=("shares_outstanding_history",),
        )
        if result.dataset_id == "shares_outstanding_history"
    ]
    assert {result.check_name for result in target_results} == {
        "duplicate_shares_outstanding_history",
        "bad_shares_outstanding_history_rows",
        "float_shares_not_above_outstanding",
        "orphan_shares_outstanding_security_ids",
    }
    assert {result.status for result in target_results} == {"passed"}

    watermarks = tmp_store.con.execute(
        """
        SELECT watermark_name, watermark_value
        FROM dataset_watermarks
        WHERE dataset_id = 'shares_outstanding_history'
        ORDER BY watermark_name
        """
    ).fetchall()
    assert watermarks == [
        ("max_available_at", "2024-02-02 21:30:00"),
        ("max_effective_date", "2023-12-30"),
    ]
