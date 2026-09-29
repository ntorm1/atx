"""S1.5 reference stage: FRED parsing, H.10 / H.15 / VIX publication clocks, fx inversion, French file parsers."""

from __future__ import annotations

import datetime as dt
import json

import pytest

from atx_db.alpha_panel import reference as R

D = dt.date
T = dt.datetime


def test_parse_fred_csv_blank_days_are_none():
    text = "observation_date,DEXJPUS\n2024-01-01,\n2024-01-02,141.50\n2024-01-03,.\n"
    assert R.parse_fred_csv(text, "DEXJPUS") == [(D(2024, 1, 1), None), (D(2024, 1, 2), 141.5), (D(2024, 1, 3), None)]
    with pytest.raises(ValueError):
        R.parse_fred_csv(text, "DEXUSEU")


def test_business_days_skip_weekends_and_federal_holidays():
    assert R.is_business_day(D(2024, 1, 2))
    assert not R.is_business_day(D(2024, 1, 1))            # New Year
    assert not R.is_business_day(D(2024, 1, 6))            # Saturday
    assert not R.is_business_day(D(2021, 6, 18))           # Juneteenth observed (Friday)
    assert R.next_business_day(D(2024, 1, 12)) == D(2024, 1, 16)   # Fri -> Tue after MLK day


def test_et_to_utc_follows_daylight_saving():
    assert R.et_to_utc(D(2024, 1, 8)) == T(2024, 1, 8, 21, 30)     # EST
    assert R.et_to_utc(D(2024, 7, 8)) == T(2024, 7, 8, 20, 30)     # EDT


def test_h15_value_is_published_next_business_day():
    assert R.h15_available_at(D(2026, 9, 28)) == T(2026, 9, 29, 20, 30)    # Mon value, Tue 16:30 ET
    assert R.h15_available_at(D(2024, 1, 12)) == T(2024, 1, 16, 21, 30)    # Fri value, Tue after MLK
    # DFF weekend value: carried into the next business day, published the business day after that
    assert R.h15_available_at(D(2026, 9, 26)) == T(2026, 9, 29, 20, 30)


def test_vix_close_clock_is_same_day_after_close():
    assert R.vix_available_at(D(2024, 3, 1)) == T(2024, 3, 1, 21, 30)


RELEASES = [D(2024, 1, 2), D(2024, 1, 8), D(2024, 1, 10), D(2024, 1, 16), D(2024, 1, 22)]


def test_h10_release_is_first_board_date_from_next_monday():
    # week of 2024-01-01: next Monday 01-08 is a release
    assert R.h10_release_date(D(2024, 1, 3), RELEASES) == (D(2024, 1, 8), "board_release_list")
    # a Monday observation waits for the following week's release, never the same-day one
    assert R.h10_release_date(D(2024, 1, 8), RELEASES) == (D(2024, 1, 16), "board_release_list")
    # the mid-week 01-10 release (a revision) is skipped; 01-15 MLK moves the release to Tuesday 01-16
    assert R.h10_release_date(D(2024, 1, 12), RELEASES) == (D(2024, 1, 16), "board_release_list")
    # outside the list: rule fallback (Monday, or the next business day)
    assert R.h10_release_date(D(2030, 1, 16), RELEASES) == (D(2030, 1, 22), "rule_monday_or_next_business_day")
    # inside a suspension gap of the weekly release: the next listed release (conservative)
    gap = [D(2006, 5, 15), D(2009, 1, 5), D(2009, 1, 12)]
    assert R.h10_release_date(D(2008, 6, 4), gap) == (D(2009, 1, 5), "board_release_after_suspension")


def test_parse_release_dates_json():
    blob = json.dumps([{"yearValue": "2024", "Months": [{"Dates": ["20240108", "20240102"]}]}]).encode()
    assert R.parse_release_dates(blob) == [D(2024, 1, 2), D(2024, 1, 8)]


def test_fx_rows_invert_per_usd_quotes_and_drop_blanks():
    rows = R.fx_rows("DEXJPUS", [(D(2024, 1, 3), 125.0), (D(2024, 1, 4), None)], RELEASES)
    assert rows == [(D(2024, 1, 3), "JPY", 0.008, "DEXJPUS", T(2024, 1, 8, 21, 30))]
    eur = R.fx_rows("DEXUSEU", [(D(2024, 1, 3), 1.09)], RELEASES)
    assert eur[0][1:3] == ("EUR", 1.09)
    assert all(q in ("usd_per_ccy", "ccy_per_usd") for _, q in R.H10.values())
    assert len({c for c, _ in R.H10.values()}) == len(R.H10)


def test_redenominated_bolivar_gets_dated_iso_codes():
    assert R.currency_of("DEXVZUS", D(2018, 8, 17)) == "VEF"
    assert R.currency_of("DEXVZUS", D(2018, 8, 20)) == "VES"
    assert R.currency_of("DEXVZUS", D(2021, 10, 4)) == "VED"
    assert R.currency_of("DEXUSEU", D(2021, 10, 4)) == "EUR"


def test_completeness_counts_missing_business_days():
    obs = [(D(2024, 1, 2), 1.0), (D(2024, 1, 3), None), (D(2024, 1, 4), 1.0), (D(2024, 1, 5), 1.0)]
    c = R.completeness(obs, start=D(2024, 1, 1))
    assert (c["expected"], c["observed"], c["missing"], c["missing_days"]) == (4, 3, 1, ["2024-01-03"])


FF_MONTHLY = """This file was created using the 202608 CRSP database.
The 1-month TBill rate data ...

,Mkt-RF,SMB,HML,RMW,CMA,RF
202607,    1.50,   -0.53,   -3.54,   -4.08,   -2.01,    0.29
202608,    2.56,  -99.99,   -3.54,   -4.08,   -2.01,    0.29

 Annual Factors: January-December
,Mkt-RF,SMB,HML,RMW,CMA,RF
  2025,   12.59,    0.52,    9.72,   -2.76,    6.49,    3.54
"""


def test_parse_french_monthly_stops_at_annual_table():
    cols, rows, vintage = R.parse_french_table(FF_MONTHLY)
    assert cols == ["Mkt-RF", "SMB", "HML", "RMW", "CMA", "RF"] and vintage == "CRSP 202608"
    assert [r[0] for r in rows] == [D(2026, 7, 1), D(2026, 8, 1)]
    assert rows[0][1][0] == pytest.approx(0.015) and rows[1][1][1] is None
    assert [R.factor_column(c) for c in cols][:2] == ["mkt_rf", "smb"]


def test_parse_french_daily_keys():
    text = "header\n\n,Mom   \n20240102,   0.25\n20240103,  -1.10\n\n"
    cols, rows, _ = R.parse_french_table(text)
    assert cols == ["Mom"] and rows == [(D(2024, 1, 2), [0.0025]), (D(2024, 1, 3), [pytest.approx(-0.011)])]


SIC12 = """ 1 NoDur  Consumer Nondurables -- Food, Tobacco
          0100-0999
          2000-2399

 2 Durbl  Consumer Durables -- Cars, TVs
          2520-2589 Household furniture

12 Other  Other -- Mines, Constr, BldMt
"""


def test_parse_siccodes_ranges_and_residual_industry():
    rows = R.parse_siccodes(SIC12, 12)
    assert rows[0] == (12, 1, "NoDur", "Consumer Nondurables -- Food, Tobacco", 100, 999, None)
    assert rows[2] == (12, 2, "Durbl", "Consumer Durables -- Cars, TVs", 2520, 2589, "Household furniture")
    assert rows[-1] == (12, 12, "Other", "Other -- Mines, Constr, BldMt", None, None, None)
