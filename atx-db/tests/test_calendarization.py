from __future__ import annotations

import datetime as dt
import json

import pandas as pd
import pytest

SOURCE = "SEC companyfacts"


def _period_days(start: dt.date | None, end: dt.date) -> int | None:
    if start is None:
        return None
    return (end - start).days + 1


def _insert_period(
    store,
    *,
    period_id: str,
    security_id: str,
    symbol: str,
    start: dt.date,
    end: dt.date,
    normalized_period_type: str,
    fiscal_year: int,
    fiscal_period: str,
    accession: str,
    available_at: dt.datetime = dt.datetime(2025, 3, 1, 22, 0),
) -> None:
    days = _period_days(start, end)
    calendar_quarter = ((end.month - 1) // 3) + 1
    store.con.execute(
        """
        INSERT OR REPLACE INTO fundamental_periods (
            fundamental_period_id,
            period_group_id,
            source,
            security_id,
            symbol,
            cik,
            period_start,
            period_end,
            datadate,
            period_days,
            normalized_period_type,
            calendar_year,
            calendar_quarter,
            calendar_period,
            rdq,
            pdate,
            fdate,
            ldate,
            as_of_date,
            available_at,
            form,
            accession_number,
            reported_fiscal_years_json,
            reported_fiscal_periods_json,
            statement_types_json,
            canonical_metrics_json,
            input_statement_point_ids_json,
            statement_point_count,
            canonical_metric_count,
            concept_count,
            value_changed_statement_count,
            has_balance_sheet,
            has_income_statement,
            has_cash_flow,
            has_per_share,
            revision_sequence,
            revision_count,
            is_latest_revision,
            first_available_at,
            latest_available_at,
            source_loaded_at
        )
        VALUES (
            ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, NULL, NULL, ?, ?,
            ?, ?, ?, ?, ?, ?, '["income_statement"]', '["revenue"]', '["stmt"]',
            1, 1, 1, 0, false, true, false, false, 1, 1, true, ?, ?, ?
        )
        """,
        [
            period_id,
            f"group-{period_id}",
            SOURCE,
            security_id,
            symbol,
            security_id[-10:].rjust(10, "0"),
            start,
            end,
            end,
            days,
            normalized_period_type,
            end.year,
            calendar_quarter,
            f"{end.year}Q{calendar_quarter}",
            available_at.date(),
            available_at.date(),
            available_at.date(),
            available_at,
            "10-K" if normalized_period_type == "annual" else "10-Q",
            accession,
            json.dumps([str(fiscal_year)]),
            json.dumps([fiscal_period]),
            available_at,
            available_at,
            available_at,
        ],
    )


def _insert_statement_point(
    store,
    *,
    statement_id: str,
    security_id: str,
    symbol: str,
    metric: str,
    value: float,
    start: dt.date,
    end: dt.date,
    fiscal_year: int,
    fiscal_period: str,
    accession: str,
    available_at: dt.datetime,
) -> None:
    store.con.execute(
        """
        INSERT OR REPLACE INTO fundamental_statement_points (
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
            ?, ?, ?, ?, ?, ?, ?, 'income_statement', 'profitability',
            ?, ?, 'us-gaap', ?, 'USD', 'monetary', 'duration', 'credit',
            ?, ?, ?, ?, ?, ?, ?, ?, 1, 1, true, false, ?, ?, NULL, NULL,
            NULL, NULL, 'test', 'https://data.sec.gov/', ?
        )
        """,
        [
            statement_id,
            f"fact-{statement_id}",
            f"rev-{statement_id}",
            SOURCE,
            security_id,
            symbol,
            security_id[-10:].rjust(10, "0"),
            metric,
            metric.replace("_", " ").title(),
            metric.title().replace("_", ""),
            start,
            end,
            available_at.date(),
            available_at,
            fiscal_year,
            fiscal_period,
            "10-K" if fiscal_period == "FY" else "10-Q",
            accession,
            value,
            value,
            available_at,
        ],
    )


def test_compute_calendar_map_rows_fyr_boundaries_and_53_week_flag() -> None:
    from atx_db.calendarization import compute_calendar_map_rows

    periods = pd.DataFrame(
        [
            {
                "fundamental_period_id": "fy-may",
                "period_group_id": "g-fy-may",
                "source": SOURCE,
                "security_id": "SEC-CIK-0000000001",
                "symbol": "MAY",
                "cik": "0000000001",
                "accession_number": "may-fy",
                "period_start": dt.date(2023, 6, 1),
                "period_end": dt.date(2024, 5, 31),
                "normalized_period_type": "annual",
                "fyr": 5,
                "reported_fiscal_years_json": '["2023"]',
                "reported_fiscal_periods_json": '["FY"]',
                "as_of_date": dt.date(2024, 7, 1),
                "available_at": dt.datetime(2024, 7, 1, 22, 0),
                "is_latest_revision": True,
                "source_loaded_at": dt.datetime(2024, 7, 1, 22, 5),
            },
            {
                "fundamental_period_id": "fy-jun",
                "period_group_id": "g-fy-jun",
                "source": SOURCE,
                "security_id": "SEC-CIK-0000000002",
                "symbol": "JUN",
                "cik": "0000000002",
                "accession_number": "jun-fy",
                "period_start": dt.date(2023, 7, 1),
                "period_end": dt.date(2024, 6, 30),
                "normalized_period_type": "annual",
                "fyr": 6,
                "reported_fiscal_years_json": '["2024"]',
                "reported_fiscal_periods_json": '["FY"]',
                "as_of_date": dt.date(2024, 8, 1),
                "available_at": dt.datetime(2024, 8, 1, 22, 0),
                "is_latest_revision": True,
                "source_loaded_at": dt.datetime(2024, 8, 1, 22, 5),
            },
            {
                "fundamental_period_id": "long-quarter",
                "period_group_id": "g-long-quarter",
                "source": SOURCE,
                "security_id": "SEC-CIK-0000000003",
                "symbol": "LONG",
                "cik": "0000000003",
                "accession_number": "long-q",
                "period_start": dt.date(2024, 1, 28),
                "period_end": dt.date(2025, 2, 1),
                "normalized_period_type": "quarter",
                "fyr": 2,
                "reported_fiscal_years_json": '["2025"]',
                "reported_fiscal_periods_json": '["Q4"]',
                "as_of_date": dt.date(2025, 3, 1),
                "available_at": dt.datetime(2025, 3, 1, 22, 0),
                "is_latest_revision": True,
                "source_loaded_at": dt.datetime(2025, 3, 1, 22, 5),
            },
        ]
    )

    out = compute_calendar_map_rows(periods)
    by_id = out.set_index("fundamental_period_id")

    assert by_id.loc["fy-may", "containing_calendar_year"] == 2024
    assert by_id.loc["fy-may", "greatest_overlap_calendar_year"] == 2023
    assert by_id.loc["fy-jun", "greatest_overlap_calendar_year"] == 2024
    assert by_id.loc["long-quarter", "period_length_days"] == 371
    assert by_id.loc["long-quarter", "week_count"] == 53
    assert bool(by_id.loc["long-quarter", "is_53_week"])
    assert not out[
        [
            "fiscal_scheme_period",
            "containing_calendar_period",
            "greatest_overlap_calendar_period",
        ]
    ].isna().any().any()


def test_calendarization_migrations_are_catalogued(tmp_store) -> None:
    versions = {
        int(row[0])
        for row in tmp_store.con.execute(
            "SELECT CAST(version AS INTEGER) FROM schema_migrations WHERE version ~ '^[0-9]+$'"
        ).fetchall()
    }
    assert {114, 115, 116}.issubset(versions)

    tables = {
        row[0]
        for row in tmp_store.con.execute(
            """
            SELECT table_name
            FROM table_catalog
            WHERE table_name IN (
                'fundamental_calendar_map',
                'fundamental_calendar_ttm',
                'calendarization_coverage'
            )
            """
        ).fetchall()
    }
    assert tables == {
        "fundamental_calendar_map",
        "fundamental_calendar_ttm",
        "calendarization_coverage",
    }

    fields = {
        tuple(row)
        for row in tmp_store.con.execute(
            """
            SELECT table_name, field_name
            FROM field_catalog
            WHERE (table_name, field_name) IN (
                ('fundamental_calendar_map', 'greatest_overlap_calendar_period'),
                ('fundamental_calendar_map', 'is_53_week'),
                ('fundamental_calendar_ttm', 'is_complete'),
                ('calendarization_coverage', 'unflagged_53_week_count')
            )
            """
        ).fetchall()
    }
    assert fields == {
        ("fundamental_calendar_map", "greatest_overlap_calendar_period"),
        ("fundamental_calendar_map", "is_53_week"),
        ("fundamental_calendar_ttm", "is_complete"),
        ("calendarization_coverage", "unflagged_53_week_count"),
    }


def test_refresh_calendar_map_and_quality_checks(tmp_store) -> None:
    from atx_db.calendarization import (
        CalendarizationOptions,
        refresh_calendarization_coverage,
        refresh_fundamental_calendar_map,
    )
    from atx_db.quality import run_warehouse_quality_checks

    _insert_period(
        tmp_store,
        period_id="map-regular",
        security_id="SEC-CIK-0000000101",
        symbol="REG",
        start=dt.date(2024, 1, 1),
        end=dt.date(2024, 3, 31),
        normalized_period_type="quarter",
        fiscal_year=2024,
        fiscal_period="Q1",
        accession="reg-q1",
    )
    _insert_period(
        tmp_store,
        period_id="map-53",
        security_id="SEC-CIK-0000000102",
        symbol="LONG",
        start=dt.date(2024, 1, 28),
        end=dt.date(2025, 2, 1),
        normalized_period_type="quarter",
        fiscal_year=2025,
        fiscal_period="Q4",
        accession="long-q4",
    )

    options = CalendarizationOptions(run_id="test-calendarization")
    assert refresh_fundamental_calendar_map(tmp_store, options) == 2
    assert refresh_calendarization_coverage(tmp_store, options) == 1

    checks = run_warehouse_quality_checks(
        tmp_store,
        record=False,
        check_names=(
            "calendarization_map_exactly_one_label",
            "calendarization_53_week_flagged",
            "calendarization_coverage_green",
        ),
    )
    assert {check.check_name: check.status for check in checks} == {
        "calendarization_map_exactly_one_label": "passed",
        "calendarization_53_week_flagged": "passed",
        "calendarization_coverage_green": "passed",
    }

    tmp_store.con.execute(
        "UPDATE fundamental_calendar_map SET fiscal_scheme_period = NULL WHERE fundamental_period_id = 'map-regular'"
    )
    result = run_warehouse_quality_checks(
        tmp_store,
        record=False,
        check_names=("calendarization_map_exactly_one_label",),
    )[0]
    assert result.status == "failed"

    tmp_store.con.execute(
        "UPDATE fundamental_calendar_map SET fiscal_scheme_period = '2024Q1' WHERE fundamental_period_id = 'map-regular'"
    )
    tmp_store.con.execute(
        "UPDATE fundamental_calendar_map SET is_53_week = false WHERE fundamental_period_id = 'map-53'"
    )
    result = run_warehouse_quality_checks(
        tmp_store,
        record=False,
        check_names=("calendarization_53_week_flagged",),
    )[0]
    assert result.status == "failed"


def _seed_calendar_ttm_issuer(
    store,
    *,
    security_id: str,
    symbol: str,
    periods: list[tuple[dt.date, dt.date, int, str, float, str]],
    annual_end: dt.date,
) -> None:
    annual_start = annual_end.replace(year=annual_end.year - 1) + dt.timedelta(days=1)
    _insert_period(
        store,
        period_id=f"{security_id}-annual",
        security_id=security_id,
        symbol=symbol,
        start=annual_start,
        end=annual_end,
        normalized_period_type="annual",
        fiscal_year=annual_end.year,
        fiscal_period="FY",
        accession=f"{security_id}-fy",
    )
    for idx, (start, end, fiscal_year, fiscal_period, value, accession) in enumerate(periods, 1):
        _insert_period(
            store,
            period_id=f"{security_id}-p{idx}",
            security_id=security_id,
            symbol=symbol,
            start=start,
            end=end,
            normalized_period_type="quarter",
            fiscal_year=fiscal_year,
            fiscal_period=fiscal_period,
            accession=accession,
        )
        _insert_statement_point(
            store,
            statement_id=f"{security_id}-stmt-{idx}",
            security_id=security_id,
            symbol=symbol,
            metric="revenue",
            value=value,
            start=start,
            end=end,
            fiscal_year=fiscal_year,
            fiscal_period=fiscal_period,
            accession=accession,
            available_at=dt.datetime(2025, 3, idx, 22, 0),
        )


def test_calendar_aligned_ttm_emits_shared_calendar_quarter_for_offset_fye(tmp_store) -> None:
    from atx_db.calendarization import refresh_fundamental_calendar_map, refresh_fundamental_calendar_ttm

    _seed_calendar_ttm_issuer(
        tmp_store,
        security_id="SEC-CIK-0000000201",
        symbol="DEC",
        annual_end=dt.date(2024, 12, 31),
        periods=[
            (dt.date(2024, 1, 1), dt.date(2024, 3, 31), 2024, "Q1", 10.0, "dec-q1"),
            (dt.date(2024, 4, 1), dt.date(2024, 6, 30), 2024, "Q2", 20.0, "dec-q2"),
            (dt.date(2024, 7, 1), dt.date(2024, 9, 30), 2024, "Q3", 30.0, "dec-q3"),
            (dt.date(2024, 10, 1), dt.date(2024, 12, 31), 2024, "Q4", 40.0, "dec-q4"),
        ],
    )
    _seed_calendar_ttm_issuer(
        tmp_store,
        security_id="SEC-CIK-0000000202",
        symbol="JAN",
        annual_end=dt.date(2025, 1, 31),
        periods=[
            (dt.date(2024, 2, 1), dt.date(2024, 4, 30), 2025, "Q1", 11.0, "jan-q1"),
            (dt.date(2024, 5, 1), dt.date(2024, 7, 31), 2025, "Q2", 21.0, "jan-q2"),
            (dt.date(2024, 8, 1), dt.date(2024, 10, 31), 2025, "Q3", 31.0, "jan-q3"),
            (dt.date(2024, 11, 1), dt.date(2025, 1, 31), 2025, "Q4", 41.0, "jan-q4"),
        ],
    )
    _seed_calendar_ttm_issuer(
        tmp_store,
        security_id="SEC-CIK-0000000203",
        symbol="PART",
        annual_end=dt.date(2024, 12, 31),
        periods=[
            (dt.date(2024, 1, 1), dt.date(2024, 3, 31), 2024, "Q1", 1.0, "part-q1"),
            (dt.date(2024, 4, 1), dt.date(2024, 6, 30), 2024, "Q2", 2.0, "part-q2"),
            (dt.date(2024, 7, 1), dt.date(2024, 9, 30), 2024, "Q3", 3.0, "part-q3"),
        ],
    )

    assert refresh_fundamental_calendar_map(tmp_store) == 14
    assert refresh_fundamental_calendar_ttm(tmp_store) >= 3

    rows = tmp_store.con.execute(
        """
        SELECT security_id, calendar_period, quarter_count, coverage_days, is_complete, ttm_value
        FROM fundamental_calendar_ttm
        WHERE calendar_period = '2024Q4'
          AND security_id IN ('SEC-CIK-0000000201', 'SEC-CIK-0000000202')
        ORDER BY security_id
        """
    ).fetchall()
    assert rows == [
        ("SEC-CIK-0000000201", "2024Q4", 4, 366, True, pytest.approx(100.0)),
        ("SEC-CIK-0000000202", "2024Q4", 4, 366, True, pytest.approx(104.0)),
    ]

    partial = tmp_store.con.execute(
        """
        SELECT calendar_period, quarter_count, is_complete, ttm_value
        FROM fundamental_calendar_ttm
        WHERE security_id = 'SEC-CIK-0000000203'
        ORDER BY calendar_period_end DESC
        LIMIT 1
        """
    ).fetchone()
    assert partial == ("2024Q3", 3, False, pytest.approx(6.0))


def test_quarterly_ttm_stitch_tags_annual_minus_9mo_path(tmp_store) -> None:
    from atx_db.fundamental_statements import refresh_fundamental_ttm_points

    sid = "SEC-CIK-0000000301"
    symbol = "STCH"
    fixtures = [
        ("q1", dt.date(2024, 1, 1), dt.date(2024, 3, 31), "Q1", 10.0),
        ("six", dt.date(2024, 1, 1), dt.date(2024, 6, 30), "Q2", 30.0),
        ("nine", dt.date(2024, 1, 1), dt.date(2024, 9, 30), "Q3", 60.0),
        ("fy", dt.date(2024, 1, 1), dt.date(2024, 12, 31), "FY", 100.0),
    ]
    for idx, (suffix, start, end, fiscal_period, value) in enumerate(fixtures, 1):
        _insert_statement_point(
            tmp_store,
            statement_id=f"stitch-{suffix}",
            security_id=sid,
            symbol=symbol,
            metric="revenue",
            value=value,
            start=start,
            end=end,
            fiscal_year=2024,
            fiscal_period=fiscal_period,
            accession=f"stitch-{suffix}",
            available_at=dt.datetime(2025, 2, idx, 22, 0),
        )

    assert refresh_fundamental_ttm_points(tmp_store) == 1
    row = tmp_store.con.execute(
        """
        SELECT ttm_value, calculation_method, input_statement_point_ids_json
        FROM fundamental_ttm_points
        WHERE security_id = ?
        """,
        [sid],
    ).fetchone()
    assert row[0] == pytest.approx(100.0)
    assert row[1] == "stitched_quarterly_ttm"
    assert "stitch-fy" not in row[2]


def test_ttm_restatement_uses_latest_visible_derived_quarter_and_anchor_time(tmp_store) -> None:
    from atx_db.fundamental_statements import refresh_fundamental_ttm_points

    sid = "SEC-CIK-0000000304"
    symbol = "RSTT"
    fixtures = [
        ("q1", dt.date(2024, 1, 1), dt.date(2024, 3, 31), "Q1", 10.0, dt.datetime(2024, 5, 1, 22)),
        ("q2", dt.date(2024, 4, 1), dt.date(2024, 6, 30), "Q2", 20.0, dt.datetime(2024, 8, 1, 22)),
        ("q3", dt.date(2024, 7, 1), dt.date(2024, 9, 30), "Q3", 30.0, dt.datetime(2024, 11, 1, 22)),
        ("q4", dt.date(2024, 10, 1), dt.date(2024, 12, 31), "Q4", 40.0, dt.datetime(2025, 2, 1, 22)),
        ("nine", dt.date(2024, 1, 1), dt.date(2024, 9, 30), "Q3", 60.0, dt.datetime(2024, 11, 1, 22)),
        ("fy-restated", dt.date(2024, 1, 1), dt.date(2024, 12, 31), "FY", 105.0, dt.datetime(2026, 2, 1, 22)),
    ]
    for suffix, start, end, fiscal_period, value, available_at in fixtures:
        _insert_statement_point(
            tmp_store,
            statement_id=f"ttm-restatement-{suffix}",
            security_id=sid,
            symbol=symbol,
            metric="revenue",
            value=value,
            start=start,
            end=end,
            fiscal_year=2024,
            fiscal_period=fiscal_period,
            accession=f"ttm-restatement-{suffix}",
            available_at=available_at,
        )

    assert refresh_fundamental_ttm_points(tmp_store) >= 2
    latest = tmp_store.con.execute(
        """
        SELECT ttm_value,available_at,accession_number,calculation_method,is_latest_revision
        FROM fundamental_ttm_points
        WHERE security_id=? AND ttm_end_date=DATE '2024-12-31'
        ORDER BY available_at DESC,ttm_point_id DESC
        LIMIT 1
        """,
        [sid],
    ).fetchone()
    assert latest == (
        pytest.approx(105.0),
        dt.datetime(2026, 2, 1, 22),
        "ttm-restatement-fy-restated",
        "stitched_quarterly_ttm",
        True,
    )


def test_quarterly_ttm_stitch_noops_without_9mo_ytd(tmp_store) -> None:
    from atx_db.fundamental_statements import refresh_fundamental_ttm_points

    sid = "SEC-CIK-0000000302"
    for suffix, start, end, fiscal_period, value in [
        ("q1", dt.date(2024, 1, 1), dt.date(2024, 3, 31), "Q1", 10.0),
        ("six", dt.date(2024, 1, 1), dt.date(2024, 6, 30), "Q2", 30.0),
        ("fy", dt.date(2024, 1, 1), dt.date(2024, 12, 31), "FY", 100.0),
    ]:
        _insert_statement_point(
            tmp_store,
            statement_id=f"thin-{suffix}",
            security_id=sid,
            symbol="THIN",
            metric="revenue",
            value=value,
            start=start,
            end=end,
            fiscal_year=2024,
            fiscal_period=fiscal_period,
            accession=f"thin-{suffix}",
            available_at=dt.datetime(2025, 2, 1 + len(suffix), 22, 0),
        )

    assert refresh_fundamental_ttm_points(tmp_store) == 0


def test_ttm_refresh_can_scope_delete_and_scan_to_one_metric(tmp_store) -> None:
    from atx_db.fundamental_statements import refresh_fundamental_ttm_points

    sid = "SEC-CIK-0000000303"
    quarter_dates = [
        (dt.date(2024, 1, 1), dt.date(2024, 3, 31), "Q1"),
        (dt.date(2024, 4, 1), dt.date(2024, 6, 30), "Q2"),
        (dt.date(2024, 7, 1), dt.date(2024, 9, 30), "Q3"),
        (dt.date(2024, 10, 1), dt.date(2024, 12, 31), "Q4"),
    ]
    for metric, base in (("revenue", 10.0), ("stock_issuance", 1.0)):
        for index, (start, end, fiscal_period) in enumerate(quarter_dates, 1):
            _insert_statement_point(
                tmp_store,
                statement_id=f"scope-{metric}-{index}",
                security_id=sid,
                symbol="SCOPE",
                metric=metric,
                value=base * index,
                start=start,
                end=end,
                fiscal_year=2024,
                fiscal_period=fiscal_period,
                accession=f"scope-{metric}-{index}",
                available_at=dt.datetime(2025, 2, index, 22, 0),
            )

    assert refresh_fundamental_ttm_points(tmp_store) == 2
    tmp_store.con.execute(
        "DELETE FROM fundamental_statement_points WHERE canonical_metric = 'stock_issuance'"
    )

    assert refresh_fundamental_ttm_points(
        tmp_store,
        canonical_metrics=("stock_issuance",),
    ) == 1
    assert tmp_store.con.execute(
        "SELECT canonical_metric FROM fundamental_ttm_points ORDER BY canonical_metric"
    ).fetchall() == [("revenue",)]
