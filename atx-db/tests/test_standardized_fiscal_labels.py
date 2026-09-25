"""Period-own fiscal labels in fundamental_standardized (tier1 A6).

Company Facts ``fy``/``fp`` belong to the filing that carried a fact.  A
comparative period re-reported in a later filing therefore arrives wearing the
later filing's labels.  These fixtures feed filing labels in and assert that the
standardized labels describe each fact's own fiscal period, and that annual
coverage by fiscal year cannot move when a comparative is re-reported.
"""

from __future__ import annotations

import datetime as dt

import pytest

from atx_db._standardization_set_based import fiscal_year_end_label
from atx_db.item_coverage import ItemCoverageOptions, measure_item_coverage
from atx_db.standardization import refresh_fundamental_standardized

REVENUE = ("revenue", "Revenues", "income_statement", "revenue")
RND = ("rd_expense", "ResearchAndDevelopmentExpense", "income_statement", "operating_expenses")
ASSETS = ("total_assets", "Assets", "balance_sheet", "assets")


def _point(
    store,
    *,
    security_id: str,
    accession: str,
    fy: int,
    fp: str,
    filed: dt.date,
    end: dt.date,
    value: float,
    start: dt.date | None = None,
    metric: tuple[str, str, str, str] = REVENUE,
    form: str = "10-Q",
) -> None:
    """One Company Facts statement point carrying its FILING's fy/fp labels."""

    canonical_metric, concept, statement_type, section = metric
    instant = start is None
    available_at = dt.datetime.combine(filed, dt.time(22))
    point_id = f"{security_id}|{accession}|{concept}|{start}|{end}"
    store.con.execute(
        """
        INSERT INTO fundamental_statement_points (
            statement_point_id,fact_revision_id,revision_group_id,source,security_id,symbol,cik,
            statement_type,statement_section,canonical_metric,canonical_label,taxonomy,concept,
            unit,unit_type,period_type,normal_balance,period_start,period_end,as_of_date,
            available_at,fiscal_year,fiscal_period,form,accession_number,source_accession,
            filed_date,revision_sequence,revision_count,is_latest_revision,is_value_changed,
            raw_value,value,run_id,source_url,source_loaded_at
        ) VALUES (
            ?,?,?,'SEC companyfacts',?,?,'0000000042',?,?,?,?,'us-gaap',?,
            'USD','monetary',?,?,?,?,?,?,?,?,?,?,?,?,1,1,true,false,?,?,'test','https://data.sec.gov/',?
        )
        """,
        [
            point_id,
            f"fact-{point_id}",
            f"group-{security_id}-{concept}-{start}-{end}",
            security_id,
            security_id.split("-")[-1],
            statement_type,
            section,
            canonical_metric,
            canonical_metric.replace("_", " ").title(),
            concept,
            "instant" if instant else "duration",
            "debit" if instant else "credit",
            start,
            end,
            end,
            available_at,
            fy,
            fp,
            form,
            accession,
            accession,
            filed,
            value,
            value,
            available_at,
        ],
    )


def _labels(store, security_id: str, basis: str, canonical_code: str = "revenue") -> dict:
    """(period_end, value) -> (fiscal_year, fiscal_period) for every standardized revision."""

    rows = store.con.execute(
        """
        SELECT period_end, value, fiscal_year, fiscal_period
        FROM fundamental_standardized
        WHERE security_id = ? AND basis = ? AND canonical_code = ?
        ORDER BY period_end, available_at
        """,
        [security_id, basis, canonical_code],
    ).fetchall()
    return {(row[0], row[1]): (row[2], row[3]) for row in rows}


def test_fiscal_year_end_label_follows_compustat_fyr_with_52_53_week_spill():
    assert fiscal_year_end_label(dt.date(2020, 12, 31)) == 2020
    assert fiscal_year_end_label(dt.date(2021, 1, 2)) == 2020  # Saturday nearest Dec 31
    assert fiscal_year_end_label(dt.date(2021, 1, 31)) == 2020  # January FYR labels the prior year
    assert fiscal_year_end_label(dt.date(2020, 9, 26)) == 2020
    assert fiscal_year_end_label(dt.date(2019, 6, 1)) == 2018  # Saturday nearest May 31 is a May year
    assert fiscal_year_end_label(dt.date(2020, 5, 30)) == 2019


def test_comparative_rereports_keep_their_own_fiscal_labels(tmp_store):
    sid = "SEC-CIK-DEC"
    d = dt.date
    # Original filings, each labelled by itself.
    _point(tmp_store, security_id=sid, accession="q1-2020", fy=2020, fp="Q1", filed=d(2020, 5, 1),
           start=d(2020, 1, 1), end=d(2020, 3, 31), value=25.0)
    _point(tmp_store, security_id=sid, accession="10k-2020", fy=2020, fp="FY", filed=d(2021, 2, 15),
           start=d(2020, 1, 1), end=d(2020, 12, 31), value=100.0, form="10-K")
    _point(tmp_store, security_id=sid, accession="10k-2020", fy=2020, fp="FY", filed=d(2021, 2, 15),
           end=d(2020, 12, 31), value=500.0, metric=ASSETS, form="10-K")
    # Q1 2021 10-Q: its own quarter plus the prior-year quarter and year-end balance
    # sheet as comparatives, all carrying the filing's fy=2021 / fp=Q1.
    _point(tmp_store, security_id=sid, accession="q1-2021", fy=2021, fp="Q1", filed=d(2021, 5, 1),
           start=d(2021, 1, 1), end=d(2021, 3, 31), value=30.0)
    _point(tmp_store, security_id=sid, accession="q1-2021", fy=2021, fp="Q1", filed=d(2021, 5, 1),
           start=d(2020, 1, 1), end=d(2020, 3, 31), value=26.0)
    _point(tmp_store, security_id=sid, accession="q1-2021", fy=2021, fp="Q1", filed=d(2021, 5, 1),
           end=d(2020, 12, 31), value=505.0, metric=ASSETS)
    # FY2021 10-K re-reports FY2020 (restated) under fy=2021 / fp=FY.
    _point(tmp_store, security_id=sid, accession="10k-2021", fy=2021, fp="FY", filed=d(2022, 2, 15),
           start=d(2021, 1, 1), end=d(2021, 12, 31), value=120.0, form="10-K")
    _point(tmp_store, security_id=sid, accession="10k-2021", fy=2021, fp="FY", filed=d(2022, 2, 15),
           start=d(2020, 1, 1), end=d(2020, 12, 31), value=101.0, form="10-K")
    # Current year after the last 10-K: projected forward from the FY2021 anchor.
    _point(tmp_store, security_id=sid, accession="q2-2022", fy=2022, fp="Q2", filed=d(2022, 8, 1),
           start=d(2022, 4, 1), end=d(2022, 6, 30), value=33.0)

    refresh_fundamental_standardized(tmp_store)

    assert _labels(tmp_store, sid, "quarterly") == {
        (d(2020, 3, 31), 25.0): (2020, "Q1"),
        (d(2020, 3, 31), 26.0): (2020, "Q1"),  # re-reported by the fy=2021 10-Q
        (d(2021, 3, 31), 30.0): (2021, "Q1"),
        (d(2022, 6, 30), 33.0): (2022, "Q2"),
    }
    assert _labels(tmp_store, sid, "annual") == {
        (d(2020, 12, 31), 100.0): (2020, "FY"),
        (d(2020, 12, 31), 101.0): (2020, "FY"),  # re-reported by the fy=2021 10-K
        (d(2021, 12, 31), 120.0): (2021, "FY"),
    }
    assert _labels(tmp_store, sid, "instant", "total_assets") == {
        (d(2020, 12, 31), 500.0): (2020, "FY"),
        (d(2020, 12, 31), 505.0): (2020, "FY"),  # comparative balance in the fy=2021 Q1 10-Q
    }
    # The carrying filing (and so its own fy/fp) stays recoverable by accession.
    restated = tmp_store.con.execute(
        """
        SELECT s.source_accession, p.fiscal_year, p.fiscal_period
        FROM fundamental_standardized s
        JOIN fundamental_statement_points p
          ON p.accession_number = s.source_accession AND p.period_end = s.period_end
         AND p.concept = 'Revenues' AND p.period_start = s.period_start
        WHERE s.security_id = ? AND s.basis = 'annual' AND s.value = 101.0
        """,
        [sid],
    ).fetchall()
    assert restated == [("10k-2021", 2021, "FY")]


def test_52_53_week_calendar_labels_each_year_once(tmp_store):
    """Saturday-nearest-May-31 year ends straddle May/June; each year keeps one label."""

    sid = "SEC-CIK-RETAIL"
    d = dt.date
    years = (
        (d(2018, 6, 3), d(2019, 6, 1)),  # ends in June, still the FY2018 (May) year
        (d(2019, 6, 2), d(2020, 5, 30)),
        (d(2020, 5, 31), d(2021, 5, 29)),
        (d(2021, 5, 30), d(2022, 5, 28)),
        (d(2022, 5, 29), d(2023, 6, 3)),  # 53 weeks, 14-week fourth quarter
    )
    for index, (start, end) in enumerate(years):
        _point(tmp_store, security_id=sid, accession=f"10k-{index}", fy=2099, fp="FY",
               filed=end + dt.timedelta(days=60), start=start, end=end, value=1000.0 + index, form="10-K")
    # Quarters of the 53-week year: 13, 13, 13 and 14 weeks, plus the 39-week YTD
    # that lets standardization derive the 14-week fourth quarter.
    start = d(2022, 5, 29)
    for quarter, (q_start, q_end) in enumerate(
        ((start, d(2022, 8, 27)), (d(2022, 8, 28), d(2022, 11, 26)), (d(2022, 11, 27), d(2023, 2, 25))),
        start=1,
    ):
        _point(tmp_store, security_id=sid, accession=f"10q-{quarter}", fy=2099, fp=f"Q{quarter}",
               filed=q_end + dt.timedelta(days=40), start=q_start, end=q_end, value=200.0 + quarter)
    _point(tmp_store, security_id=sid, accession="10q-3", fy=2099, fp="Q3", filed=d(2023, 4, 6),
           start=start, end=d(2023, 2, 25), value=606.0)

    refresh_fundamental_standardized(tmp_store)

    annual = _labels(tmp_store, sid, "annual")
    assert annual == {
        (d(2019, 6, 1), 1000.0): (2018, "FY"),
        (d(2020, 5, 30), 1001.0): (2019, "FY"),
        (d(2021, 5, 29), 1002.0): (2020, "FY"),
        (d(2022, 5, 28), 1003.0): (2021, "FY"),
        (d(2023, 6, 3), 1004.0): (2022, "FY"),
    }
    assert _labels(tmp_store, sid, "quarterly") == {
        (d(2022, 8, 27), 201.0): (2022, "Q1"),
        (d(2022, 11, 26), 202.0): (2022, "Q2"),
        (d(2023, 2, 25), 203.0): (2022, "Q3"),
        (d(2023, 6, 3), 398.0): (2022, "Q4_DERIVED"),  # 1004 - 606, a 14-week quarter
    }


def test_fiscal_year_end_change_labels_old_new_and_transition_periods(tmp_store):
    """December → September year end: a 9-month transition (Jan-Sep 2019) between calendars."""

    sid = "SEC-CIK-FYECHG"
    d = dt.date
    _point(tmp_store, security_id=sid, accession="10k-2018", fy=2018, fp="FY", filed=d(2019, 2, 20),
           start=d(2018, 1, 1), end=d(2018, 12, 31), value=400.0, form="10-K")
    _point(tmp_store, security_id=sid, accession="10q-2018q3", fy=2018, fp="Q3", filed=d(2018, 11, 1),
           start=d(2018, 7, 1), end=d(2018, 9, 30), value=98.0)
    transition = ((d(2019, 1, 1), d(2019, 3, 31)), (d(2019, 4, 1), d(2019, 6, 30)), (d(2019, 7, 1), d(2019, 9, 30)))
    for index, (start, end) in enumerate(transition, start=1):
        _point(tmp_store, security_id=sid, accession=f"transition-{index}", fy=2019, fp=f"Q{index}",
               filed=end + dt.timedelta(days=40), start=start, end=end, value=100.0 + index)
    _point(tmp_store, security_id=sid, accession="10k-2020", fy=2020, fp="FY", filed=d(2020, 11, 20),
           start=d(2019, 10, 1), end=d(2020, 9, 30), value=440.0, form="10-K")
    _point(tmp_store, security_id=sid, accession="10k-2021", fy=2021, fp="FY", filed=d(2021, 11, 20),
           start=d(2020, 10, 1), end=d(2021, 9, 30), value=460.0, form="10-K")
    # First quarter of the new calendar, with the old calendar's Q4 as comparative.
    _point(tmp_store, security_id=sid, accession="10q-2020q1", fy=2020, fp="Q1", filed=d(2020, 2, 5),
           start=d(2019, 10, 1), end=d(2019, 12, 31), value=110.0)
    _point(tmp_store, security_id=sid, accession="10q-2020q1", fy=2020, fp="Q1", filed=d(2020, 2, 5),
           start=d(2018, 10, 1), end=d(2018, 12, 31), value=99.0)

    refresh_fundamental_standardized(tmp_store)

    assert _labels(tmp_store, sid, "annual") == {
        (d(2018, 12, 31), 400.0): (2018, "FY"),
        (d(2020, 9, 30), 440.0): (2020, "FY"),
        (d(2021, 9, 30), 460.0): (2021, "FY"),
    }
    assert _labels(tmp_store, sid, "quarterly") == {
        (d(2018, 9, 30), 98.0): (2018, "Q3"),
        (d(2018, 12, 31), 99.0): (2018, "Q4"),  # old calendar's Q4, re-reported under fy=2020/Q1
        # Transition months end on the new September calendar (Compustat FYR 9 -> FY2019).
        (d(2019, 3, 31), 101.0): (2019, "Q2"),
        (d(2019, 6, 30), 102.0): (2019, "Q3"),
        (d(2019, 9, 30), 103.0): (2019, "Q4"),
        (d(2019, 12, 31), 110.0): (2020, "Q1"),
    }


def test_issuer_without_annual_period_positions_by_declared_primary_period(tmp_store):
    d = dt.date
    # Calendar-year registrant, one 10-Q so far: current Q2 plus its prior-year comparative.
    _point(tmp_store, security_id="SEC-CIK-NEWCAL", accession="ipo-q2", fy=2025, fp="Q2", filed=d(2025, 8, 10),
           start=d(2025, 4, 1), end=d(2025, 6, 30), value=12.0)
    _point(tmp_store, security_id="SEC-CIK-NEWCAL", accession="ipo-q2", fy=2025, fp="Q2", filed=d(2025, 8, 10),
           start=d(2024, 4, 1), end=d(2024, 6, 30), value=9.0)
    # September-year registrant: fiscal Q1 ends in December.
    _point(tmp_store, security_id="SEC-CIK-NEWSEP", accession="ipo-q1", fy=2026, fp="Q1", filed=d(2026, 2, 10),
           start=d(2025, 10, 1), end=d(2025, 12, 31), value=7.0)
    _point(tmp_store, security_id="SEC-CIK-NEWSEP", accession="ipo-q1", fy=2026, fp="Q1", filed=d(2026, 2, 10),
           start=d(2024, 10, 1), end=d(2024, 12, 31), value=5.0)

    refresh_fundamental_standardized(tmp_store)

    assert _labels(tmp_store, "SEC-CIK-NEWCAL", "quarterly") == {
        (d(2024, 6, 30), 9.0): (2024, "Q2"),
        (d(2025, 6, 30), 12.0): (2025, "Q2"),
    }
    assert _labels(tmp_store, "SEC-CIK-NEWSEP", "quarterly") == {
        (d(2024, 12, 31), 5.0): (2025, "Q1"),
        (d(2025, 12, 31), 7.0): (2026, "Q1"),
    }


def _coverage_by_year(store) -> dict:
    options = ItemCoverageOptions(
        as_of_date=dt.date(2023, 6, 1),
        universe_id="a6_fixture_members",
        bases=("annual",),
        minimum_fiscal_year=2019,
        maximum_fiscal_year=2022,
    )
    frame = measure_item_coverage(store, options)
    frame = frame[frame["canonical_code"].isin(["revenue", "r_and_d_expense"])]
    return {
        (row.canonical_code, int(row.fiscal_year)): (int(row.n_securities), int(row.n_with_value))
        for row in frame.itertuples(index=False)
    }


@pytest.mark.parametrize("with_comparatives", [False, True])
def test_annual_coverage_by_fiscal_year_is_unmoved_by_comparative_rereports(tmp_store, with_comparatives):
    """R&D is discontinued in FY2021: the FY2021 10-K shows it only as the FY2020 comparative."""

    sid = "SEC-CIK-COVER"
    d = dt.date
    tmp_store.con.execute(
        """
        INSERT INTO universe_membership
            (universe_id,security_id,symbol,valid_from,valid_to,as_of_date,available_at,reason,
             rules_json,decision_count,is_latest_revision,source,is_member)
        VALUES ('a6_fixture_members',?,'COVER',DATE '2019-01-01',NULL,DATE '2019-01-01',
                TIMESTAMP '2019-01-01','member','{}',1,true,'test',true)
        """,
        [sid],
    )
    filings = {
        2019: (d(2020, 2, 15), {REVENUE: 90.0, RND: 5.0}),
        2020: (d(2021, 2, 15), {REVENUE: 100.0, RND: 6.0}),
        2021: (d(2022, 2, 15), {REVENUE: 120.0}),
    }
    for year, (filed, values) in filings.items():
        for metric, value in values.items():
            _point(tmp_store, security_id=sid, accession=f"10k-{year}", fy=year, fp="FY", filed=filed,
                   start=d(year, 1, 1), end=d(year, 12, 31), value=value, metric=metric, form="10-K")
        prior = filings.get(year - 1)
        if with_comparatives and prior is not None:
            for metric, value in prior[1].items():
                _point(tmp_store, security_id=sid, accession=f"10k-{year}", fy=year, fp="FY", filed=filed,
                       start=d(year - 1, 1, 1), end=d(year - 1, 12, 31), value=value, metric=metric, form="10-K")

    refresh_fundamental_standardized(tmp_store)
    if with_comparatives:
        # The fixture really re-reports: FY2020 has a later revision from the fy=2021 10-K.
        assert tmp_store.con.execute(
            """
            SELECT count(*) FROM fundamental_standardized
            WHERE security_id=? AND basis='annual' AND period_end=DATE '2020-12-31'
              AND source_accession='10k-2021'
            """,
            [sid],
        ).fetchone()[0] == 2

    assert _coverage_by_year(tmp_store) == {
        ("revenue", 2019): (1, 1),
        ("revenue", 2020): (1, 1),
        ("revenue", 2021): (1, 1),
        ("revenue", 2022): (1, 0),
        ("r_and_d_expense", 2019): (1, 1),
        ("r_and_d_expense", 2020): (1, 1),
        ("r_and_d_expense", 2021): (1, 0),
        ("r_and_d_expense", 2022): (1, 0),
    }
