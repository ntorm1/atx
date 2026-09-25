"""Which filings declare a fiscal year end, and which annual periods may anchor one (AF1 fix round 1).

M1: an annual-report form (10-K, 10-KT, 20-F, 40-F) declares its own (latest-ending) annual
period whatever fp its facts carry -- a 10-K whose facts have a blank fp is not out-ranked by a
10-Q "twelve months ended" column. M2: for an issuer with declared years, only annual periods a
year-end filing reports can anchor; a 10-Q "twelve months ended" column in a year whose 10-K is
missing never becomes that fiscal year.
"""

from __future__ import annotations

import datetime as dt

from atx_db.standardization import refresh_fundamental_standardized
from tests.test_standardized_fiscal_labels import _coverage_by_year, _labels, _member, _point

D = dt.date
# (accession, fy, fp, form, filed, start, end, value)
TEN_Q_JUNE_2021 = [
    ("q2-2021", 2021, "Q2", "10-Q", D(2021, 8, 1), D(2021, 4, 1), D(2021, 6, 30), 25.0),
    ("q2-2021", 2021, "Q2", "10-Q", D(2021, 8, 1), D(2020, 7, 1), D(2021, 6, 30), 99.0),  # twelve months ended
]


def _points(store, security_id: str, rows) -> None:
    for accession, fy, fp, form, filed, start, end, value in rows:
        _point(
            store,
            security_id=security_id,
            accession=accession,
            fy=fy,
            fp=fp,
            filed=filed,
            start=start,
            end=end,
            value=value,
            form=form,
        )


def test_blank_fp_ten_k_declares_its_year_over_a_ten_q_twelve_months_column(tmp_store):
    sid = "SEC-CIK-BLANKFP"
    # The June column is seen first at equal breadth; the 10-K's facts carry no fp at all.
    ten_k = [("10k-2021", None, None, "10-K", D(2022, 2, 15), D(2021, 1, 1), D(2021, 12, 31), 101.0)]
    _points(tmp_store, sid, TEN_Q_JUNE_2021 + ten_k)

    refresh_fundamental_standardized(tmp_store)

    # Before: the June column anchored (2021, FY); Dec-2021 became (2022, Q2), Apr-Jun (2021, Q4).
    assert _labels(tmp_store, sid, "annual") == {
        (D(2021, 6, 30), 99.0): (2021, "Q2"),
        (D(2021, 12, 31), 101.0): (2021, "FY"),
    }
    assert _labels(tmp_store, sid, "quarterly") == {(D(2021, 6, 30), 25.0): (2021, "Q2")}


def test_ten_q_twelve_months_column_does_not_anchor_a_missing_ten_k_year(tmp_store):
    sid = "SEC-CIK-DELINQ"
    _member(tmp_store, sid)
    # FY2019 and FY2022 are declared; the FY2020 and FY2021 10-Ks are absent, with no comparatives.
    ten_ks = [
        (f"10k-{year}", year, "FY", "10-K", D(year + 1, 2, 15), D(year, 1, 1), D(year, 12, 31), 100.0 + year - 2019)
        for year in (2019, 2022)
    ]
    _points(tmp_store, sid, ten_ks + TEN_Q_JUNE_2021)

    refresh_fundamental_standardized(tmp_store)

    # Before: Jul20-Jun21 anchored as (2021, FY) and counted as FY2021 coverage; Apr-Jun 2021 was (2021, Q4).
    assert _labels(tmp_store, sid, "annual") == {
        (D(2019, 12, 31), 100.0): (2019, "FY"),
        (D(2021, 6, 30), 99.0): (2021, "Q2"),
        (D(2022, 12, 31), 103.0): (2022, "FY"),
    }
    assert _labels(tmp_store, sid, "quarterly") == {(D(2021, 6, 30), 25.0): (2021, "Q2")}
    coverage = _coverage_by_year(tmp_store)
    assert {year: coverage[("revenue", year)] for year in range(2019, 2023)} == {
        2019: (1, 1),
        2020: (1, 0),
        2021: (1, 0),
        2022: (1, 1),
    }
