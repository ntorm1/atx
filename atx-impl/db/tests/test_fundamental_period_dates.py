from __future__ import annotations

import datetime as dt


def _insert_statement_point(store) -> None:
    store.con.execute(
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
            'stmt-1',
            'fact-1',
            'rev-1',
            'SEC companyfacts',
            'SEC-CIK-0000000001',
            'TST',
            '0000000001',
            'income_statement',
            'profitability',
            'net_income',
            'Net income',
            'us-gaap',
            'NetIncomeLoss',
            'USD',
            'monetary',
            'duration',
            'credit',
            DATE '2024-01-01',
            DATE '2024-03-31',
            DATE '2024-05-03',
            TIMESTAMP '2024-05-03 22:00:00',
            2024,
            'Q1',
            '10-Q',
            '0000000001-24-000010',
            1,
            1,
            true,
            false,
            100.0,
            100.0,
            NULL,
            NULL,
            NULL,
            NULL,
            'test',
            'https://data.sec.gov/',
            TIMESTAMP '2024-05-03 22:05:00'
        )
        """
    )


def _insert_item_202_8k(store) -> None:
    store.con.execute(
        """
        INSERT INTO sec_submissions (
            security_id,
            cik,
            accession_number,
            filing_date,
            report_date,
            acceptance_datetime,
            form,
            primary_document,
            primary_doc_description,
            file_number,
            film_number,
            items,
            size,
            is_xbrl,
            is_inline_xbrl,
            act,
            source_url,
            run_id,
            source_loaded_at
        )
        VALUES (
            'SEC-CIK-0000000001',
            '0000000001',
            '0000000001-24-000008',
            DATE '2024-04-25',
            DATE '2024-04-24',
            TIMESTAMP '2024-04-24 20:01:00',
            '8-K',
            'earnings.htm',
            'Results of Operations and Financial Condition',
            NULL,
            NULL,
            '2.02',
            12345,
            false,
            false,
            NULL,
            'https://www.sec.gov/Archives/edgar/data/1/000000000124000008/earnings.htm',
            'test',
            TIMESTAMP '2024-04-24 20:02:00'
        )
        """
    )


def test_migration_0008_period_date_columns_exist(tmp_store):
    cols = {
        row[0]
        for row in tmp_store.con.execute(
            """
            SELECT column_name
            FROM information_schema.columns
            WHERE table_schema = 'main'
              AND table_name = 'fundamental_periods'
            """
        ).fetchall()
    }
    for column in ("datadate", "rdq", "pdate", "fdate", "ldate"):
        assert column in cols


def test_refresh_fundamental_periods_infers_four_date_model(tmp_store):
    from db.fundamental_statements import refresh_fundamental_periods

    _insert_statement_point(tmp_store)
    _insert_item_202_8k(tmp_store)

    assert refresh_fundamental_periods(tmp_store) == 1
    row = tmp_store.con.execute(
        """
        SELECT datadate, rdq, pdate, fdate, ldate, as_of_date, available_at
        FROM fundamental_periods
        WHERE security_id = 'SEC-CIK-0000000001'
        """
    ).fetchone()
    assert row is not None
    assert row[0] == dt.date(2024, 3, 31)
    assert row[1] == dt.date(2024, 4, 24)
    assert row[2] == dt.date(2024, 4, 24)
    assert row[3] == dt.date(2024, 5, 3)
    assert row[4] == dt.date(2024, 5, 3)
    assert row[5] == dt.date(2024, 5, 3)


def test_fundamental_period_date_quality_passes_clean_sample(tmp_store):
    from db.fundamental_statements import refresh_fundamental_periods
    from db.quality import run_warehouse_quality_checks

    _insert_statement_point(tmp_store)
    _insert_item_202_8k(tmp_store)
    refresh_fundamental_periods(tmp_store)

    results = run_warehouse_quality_checks(tmp_store, record=False)
    bad_period_results = [
        result for result in results if result.check_name == "bad_fundamental_period_rows"
    ]
    assert bad_period_results
    assert bad_period_results[0].status == "passed"
