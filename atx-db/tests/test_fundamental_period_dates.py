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
    from atx_db.fundamental_statements import refresh_fundamental_periods

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


def test_implausible_8k_report_date_takes_the_acceptance_date(tmp_store):
    """Novell-like: the Item 2.02 8-K is dated at the fiscal quarter end, accepted weeks later."""
    from atx_db.fundamental_statements import refresh_fundamental_periods

    _insert_statement_point(tmp_store)
    _insert_item_202_8k(tmp_store)

    def rdq_for(report_date, filing_date, acceptance):
        tmp_store.con.execute(
            "UPDATE sec_submissions SET report_date=?, filing_date=?, acceptance_datetime=? "
            "WHERE accession_number='0000000001-24-000008'",
            [report_date, filing_date, acceptance],
        )
        refresh_fundamental_periods(tmp_store)
        return tmp_store.con.execute(
            "SELECT rdq, pdate FROM fundamental_periods WHERE security_id='SEC-CIK-0000000001'"
        ).fetchone()

    # Report date = period end, accepted 2024-04-26 16:30 ET (19 business days later):
    # the release date comes from the acceptance, never earlier than it.
    assert rdq_for(dt.date(2024, 3, 31), dt.date(2024, 4, 26), dt.datetime(2024, 4, 26, 20, 30)) == (
        dt.date(2024, 4, 26), dt.date(2024, 4, 26))
    # No usable clock (date-only stamp): the 8-K filing date instead.
    assert rdq_for(dt.date(2024, 3, 31), dt.date(2024, 4, 29), dt.datetime(2024, 4, 29)) == (
        dt.date(2024, 4, 29), dt.date(2024, 4, 29))
    # Within Item 2.02's four business days (Mon -> Fri) the report date stands.
    assert rdq_for(dt.date(2024, 4, 22), dt.date(2024, 4, 26), dt.datetime(2024, 4, 26, 20, 30))[0] == \
        dt.date(2024, 4, 22)


def test_rdq_available_at_is_the_8k_decision_clock_never_a_raw_or_untimed_stamp(tmp_store):
    """PIT edge cases for the 0328 rdq lineage: rdq_available_at is the 8-K's FC1 clock,
    greatest(timed acceptance, filed + 46h) (research.events' evidence clock), so it never
    precedes filed + 46h, never stores an untimed stamp, and is never NULL beside a set rdq."""
    from atx_db.fundamental_statements import refresh_fundamental_periods

    _insert_statement_point(tmp_store)
    _insert_item_202_8k(tmp_store)

    def lineage(report_date, filing_date, acceptance, raw=None):
        tmp_store.con.execute(
            "UPDATE sec_submissions SET report_date=?, filing_date=?, acceptance_datetime=?, "
            "acceptance_datetime_raw=? WHERE accession_number='0000000001-24-000008'",
            [report_date, filing_date, acceptance, raw],
        )
        assert refresh_fundamental_periods(tmp_store) == 1
        row = tmp_store.con.execute(
            "SELECT rdq, rdq_basis, rdq_available_at, rdq_accession_number FROM fundamental_periods "
            "WHERE security_id='SEC-CIK-0000000001'"
        ).fetchone()
        assert len({value is None for value in row}) == 1  # rdq and its lineage are NULL together
        return row[:3]

    d, ts = dt.date, dt.datetime
    # Timed acceptance (16:01 ET) the evening before filing: the clock is filed + 46h, not the stamp.
    assert lineage(d(2024, 4, 24), d(2024, 4, 25), ts(2024, 4, 24, 20, 1)) == (
        d(2024, 4, 24), "reported_date", ts(2024, 4, 26, 22))
    # A timed acceptance after the floor is the clock.
    assert lineage(d(2024, 4, 24), d(2024, 4, 24), ts(2024, 4, 26, 23, 30)) == (
        d(2024, 4, 24), "reported_date", ts(2024, 4, 26, 23, 30))
    # EDGAR-midnight stamp (00:00 ET) is untimed: never stored; the floor instead.
    assert lineage(d(2024, 4, 24), d(2024, 4, 26), ts(2024, 4, 26, 4)) == (
        d(2024, 4, 24), "reported_date", ts(2024, 4, 27, 22))
    # Zone-less raw stamp is untimed as well.
    assert lineage(d(2024, 4, 24), d(2024, 4, 25), ts(2024, 4, 24, 16, 1), raw="2024-04-24T16:01:00") == (
        d(2024, 4, 24), "reported_date", ts(2024, 4, 26, 22))
    # Untimed stamps ABOVE the floor (0.9 N1): only the untimed check keeps them out, so a lost
    # check would surface here as the stamp instead of the floor (filed 04-24 -> 04-25 22:00).
    assert lineage(d(2024, 4, 24), d(2024, 4, 24), ts(2024, 4, 26, 4)) == (
        d(2024, 4, 24), "reported_date", ts(2024, 4, 25, 22))
    assert lineage(d(2024, 4, 24), d(2024, 4, 24), ts(2024, 4, 26, 16, 1), raw="2024-04-26T16:01:00") == (
        d(2024, 4, 24), "reported_date", ts(2024, 4, 25, 22))
    # No acceptance at all: rdq keeps its date and gets the floor, never a NULL clock.
    assert lineage(d(2024, 4, 24), d(2024, 4, 25), None) == (d(2024, 4, 24), "reported_date", ts(2024, 4, 26, 22))
    # Implausible report date with a date-only stamp: filing-date rdq, clocked at its floor.
    assert lineage(d(2024, 3, 31), d(2024, 4, 29), ts(2024, 4, 29)) == (
        d(2024, 4, 29), "filing_date_implausible_report", ts(2024, 4, 30, 22))
    # No filing date: no resolvable clock (FC1), so the 8-K never sets rdq.
    assert lineage(d(2024, 4, 24), None, ts(2024, 4, 24, 20, 1)) == (None, None, None)


def test_fundamental_period_date_quality_passes_clean_sample(tmp_store):
    from atx_db.fundamental_statements import refresh_fundamental_periods
    from atx_db.quality import run_warehouse_quality_checks

    _insert_statement_point(tmp_store)
    _insert_item_202_8k(tmp_store)
    refresh_fundamental_periods(tmp_store)

    results = run_warehouse_quality_checks(
        tmp_store,
        record=False,
        check_names=("bad_fundamental_period_rows",),
    )
    bad_period_results = [
        result for result in results if result.check_name == "bad_fundamental_period_rows"
    ]
    assert bad_period_results
    assert bad_period_results[0].status == "passed"
