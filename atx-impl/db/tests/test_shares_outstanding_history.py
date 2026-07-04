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
) -> None:
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
            'share_count', 'shares', ?, ?, 'dei',
            'EntityCommonStockSharesOutstanding', 'shares', 'shares', ?, 'credit',
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
