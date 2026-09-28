"""Synthetic postimplementation checks for build_fundamental_events.py (no real CF-R/FSDS access)."""
import datetime as dt
import hashlib
import json
import math
import statistics
from pathlib import Path

import pyarrow as pa
import pyarrow.parquet as pq
import pytest

import build_fundamental_events as bfe

UTC = dt.timezone.utc


def d(text: str) -> int:
    return bfe.day_of(dt.date.fromisoformat(text))


def us(text: str) -> int:
    t = dt.datetime.fromisoformat(text).replace(tzinfo=UTC)
    return int((t - dt.datetime(1970, 1, 1, tzinfo=UTC)).total_seconds()) * 1_000_000


def fact(metric, start, end, value, rank=0):
    return (bfe.M[metric], rank, None if start is None else d(start), d(end), float(value))


def acc(adsh, clock, form, facts, period=None, basis=bfe.BASIS_FSDS, fy=None, fp=None):
    clock_us = us(clock)
    filed = clock_us // bfe.US_PER_DAY
    return bfe.Accession(adsh, clock_us, basis, filed, form, None if period is None else d(period), fy, fp, None,
                         list(facts))


def item(row, name):
    return row[12 + bfe.ITEMS.index(name)]


def sha(path) -> str:
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


# ---------------------------------------------------------------------------
# Concept map and period arithmetic
# ---------------------------------------------------------------------------

def test_concept_map_comes_from_the_atx_db_seed():
    cmap = bfe.load_concept_map()
    assert set(cmap.by_metric) == set(bfe.METRICS)
    # total-over-component overrides (fix round 1); the remaining concepts keep the seed order
    assert [c for c, _p in cmap.by_metric["revenue"][:3]] == [
        "Revenues", "RevenueFromContractWithCustomerExcludingAssessedTax",
        "RevenueFromContractWithCustomerIncludingAssessedTax"]
    assert cmap.by_metric["cash"][0][0] == "CashAndCashEquivalentsAtCarryingValue"
    assert cmap.by_metric["st_debt"][0][0] == "DebtCurrent"
    assert cmap.by_metric["cogs"][0][0] == "CostOfGoodsAndServicesSold"
    assert bfe.parameters()["precedence_overrides"]["revenue"] == ["Revenues"]
    assert cmap.by_metric["total_assets"] == [["Assets", 10]]
    mid, rank, unit = cmap.concept_to["WeightedAverageNumberOfDilutedSharesOutstanding"]
    assert (bfe.METRICS[mid], rank, unit) == ("shares_diluted_avg", 0, "shares")
    assert cmap.concept_to["PaymentsToAcquirePropertyPlantAndEquipment"][2] == "USD"
    assert len(cmap.sha256) == 64 and bfe.load_concept_map().sha256 == cmap.sha256
    hashes = bfe.code_hashes()
    assert set(hashes) == {"atx-engine/tools/build_fundamental_events.py",
                           "atx-engine/tools/export_fundamental_fields.py",
                           "atx-db/src/atx_db/statement_map_seed.py", "atx-db/src/atx_db/seeds/statement_map.csv"}


def put_all(series, rows):
    for start, end, value in rows:
        series.put(d(start), d(end), 0, float(value))


def test_quarter_by_ytd_differencing():
    s = bfe.DurationSeries()
    put_all(s, [("2020-01-01", "2020-03-31", 10), ("2020-01-01", "2020-06-30", 25),
                ("2020-01-01", "2020-09-30", 45), ("2020-01-01", "2020-12-31", 70)])
    assert s.quarter(d("2020-03-31"), 0)[2] == 10
    assert s.quarter(d("2020-06-30"), 0) == (d("2020-04-01"), d("2020-06-30"), 15.0)
    assert s.quarter(d("2020-09-30"), 0)[2] == 20
    assert s.quarter(d("2020-12-31"), 0) == (d("2020-10-01"), d("2020-12-31"), 25.0)  # Q4 = FY - 9M
    assert s.ttm(d("2020-12-31"), 0) == 70


def test_ttm_from_ytd_plus_prior_fy_minus_prior_ytd():
    s = bfe.DurationSeries()
    # FY2019 and the 9M comparatives only (no discrete quarters derivable for 2020 Q1/Q2).
    put_all(s, [("2019-01-01", "2019-12-31", 100), ("2019-01-01", "2019-09-30", 70),
                ("2020-01-01", "2020-09-30", 90)])
    assert s.ttm(d("2020-09-30"), 7) == 90 + 100 - 70
    assert s.ttm(d("2020-06-30"), 7) is None


def test_ttm_from_four_chained_quarters():
    s = bfe.DurationSeries()
    put_all(s, [("2019-10-01", "2019-12-31", 4), ("2020-01-01", "2020-03-31", 1), ("2020-04-01", "2020-06-30", 2),
                ("2020-07-01", "2020-09-30", 3)])
    assert s.ttm(d("2020-09-30"), 0) == 10
    s2 = bfe.DurationSeries()
    put_all(s2, [("2020-01-01", "2020-03-31", 1), ("2020-04-01", "2020-06-30", 2), ("2020-07-01", "2020-09-30", 3)])
    assert s2.ttm(d("2020-09-30"), 0) is None


def test_52_53_week_fiscal_year():
    s = bfe.DurationSeries()
    # 53-week FY ending 2021-01-02 (371 days) with a 14-week Q4 (98 days); prior 52-week FY.
    put_all(s, [("2019-12-29", "2020-12-26", 5200),                       # not used: 364-day window
                ("2020-12-27", "2022-01-01", 4000), ("2020-12-27", "2021-10-02", 2900)])
    q4 = s.quarter(d("2022-01-01"), 0)
    assert q4 == (d("2021-10-03"), d("2022-01-01"), 1100.0) and d("2022-01-01") - d("2021-10-03") + 1 == 91
    s53 = bfe.DurationSeries()
    put_all(s53, [("2019-12-29", "2021-01-02", 5300), ("2019-12-29", "2020-09-26", 3700)])
    assert d("2021-01-02") - d("2019-12-29") + 1 == 371
    assert s53.ttm(d("2021-01-02"), 7) == 5300
    q = s53.quarter(d("2021-01-02"), 0)
    assert q[2] == 1600 and q[1] - q[0] + 1 == 98
    st = bfe.CikState()
    st.inst("total_assets").put(d("2022-01-01"), 0, 110.0)
    st.inst("total_assets").put(d("2021-01-02"), 0, 100.0)  # 364 days earlier: inside the lag tolerance
    side = bfe._asset_side(st, d("2022-01-01") - bfe.LAG4, bfe.TOL_LAG)
    assert side.at == 100.0


# ---------------------------------------------------------------------------
# Per-CIK replay: restatements, anchor, clocks, zero-fill, fscore, sue, shares, staleness
# ---------------------------------------------------------------------------

def fy_filing(adsh, clock, year, ni, rev, assets, form="10-K", extra=()):
    start, end = f"{year}-01-01", f"{year}-12-31"
    facts = [fact("net_income", start, end, ni), fact("revenue", start, end, rev),
             fact("total_assets", None, end, assets), fact("stockholders_equity", None, end, assets / 2),
             fact("operating_cash_flow", start, end, ni * 1.5)] + list(extra)
    return acc(adsh, clock, form, facts, period=end, fy=year, fp="FY")


def test_restatement_latest_clock_wins_and_is_never_backdated():
    counts = {}
    rows = bfe.process_cik(7, [fy_filing("a1", "2020-02-20T21:00:00", 2019, 100, 1000, 5000),
                               acc("a2", "2020-06-01T21:00:00", "10-K/A",
                                   [fact("net_income", "2019-01-01", "2019-12-31", 80)], period="2019-12-31")],
                           counts)
    assert [r[1] for r in rows] == ["a1", "a2"]
    assert item(rows[0], "ni_ttm") == 100 and item(rows[1], "ni_ttm") == 80
    assert rows[0][-1] == 0 and rows[1][-1] == 1 and counts["restated_facts"] == 1
    assert rows[1][7] == d("2019-12-31") and item(rows[1], "at") == 5000  # the amendment keeps the snapshot


def test_late_10ka_after_next_10q_recomputes_ttm_at_the_newer_anchor():
    q1 = acc("q1", "2020-05-01T21:00:00", "10-Q",
             [fact("revenue", "2020-01-01", "2020-03-31", 300), fact("revenue", "2019-01-01", "2019-03-31", 250),
              fact("total_assets", None, "2020-03-31", 5100)], period="2020-03-31", fy=2020, fp="Q1")
    ka = acc("ka", "2020-06-15T21:00:00", "10-K/A", [fact("revenue", "2019-01-01", "2019-12-31", 900)],
             period="2019-12-31", fy=2019, fp="FY")
    rows = bfe.process_cik(7, [fy_filing("k", "2020-02-20T21:00:00", 2019, 100, 1000, 5000), q1, ka], {})
    k_row, q_row, a_row = rows
    assert k_row[7] == d("2019-12-31") and q_row[7] == d("2020-03-31") and a_row[7] == d("2020-03-31")
    assert item(q_row, "sale_ttm") == 300 + 1000 - 250
    assert item(a_row, "sale_ttm") == 300 + 900 - 250
    assert (a_row[8], a_row[9]) == (2020, "Q1")  # fiscal fields describe the anchor, not the amendment
    assert a_row[6] == d("2019-12-31")           # report_period is the amendment's own period


def test_emission_window_and_seal_inside_replay():
    counts = {}
    rows = bfe.process_cik(7, [fy_filing("old", "2013-02-20T21:00:00", 2012, 50, 500, 4000),
                               fy_filing("new", "2014-06-02T21:00:00", 2013, 60, 600, 4200),
                               fy_filing("seal", "2025-01-01T00:00:00", 2024, 70, 700, 4400)], counts)
    assert [r[1] for r in rows] == ["new"]
    assert item(rows[0], "at_lag4") == 4000  # pre-window knowledge is still used
    assert counts["accessions_sealed"] == 1


def test_zero_fill_rules_and_presence():
    rows = bfe.process_cik(9, [fy_filing("k", "2021-02-20T21:00:00", 2020, 10, 100, 1000, extra=[
        fact("total_liabilities", None, "2020-12-31", 400), fact("cash", None, "2020-12-31", 50),
        fact("lt_debt", None, "2020-12-31", 120)])], {})
    row = rows[0]
    assert item(row, "debt") == 120 and item(row, "noa") == 1000 - 50 - 400 + 120
    assert item(row, "dvc_ttm") == 0 and item(row, "prstkc_ttm") == 0 and item(row, "sstk_ttm") == 0
    assert math.isnan(item(row, "xrd_ttm")) and math.isnan(item(row, "capx_ttm"))
    assert row[-3] == "debt,dvc_ttm,noa,prstkc_ttm,sstk_ttm"
    no_cf = bfe.process_cik(9, [acc("b", "2021-02-20T21:00:00", "10-K",
                                    [fact("total_assets", None, "2020-12-31", 10)], period="2020-12-31")], {})[0]
    assert math.isnan(item(no_cf, "dvc_ttm")) and item(no_cf, "debt") == 0 and no_cf[-3] == "debt"


def test_balance_items_fallbacks_and_quarter_lags():
    facts = [
        # anchor 2021-06-30: equity via NCI total minus minority, minus preferred; liabilities via at - NCI total
        fact("total_assets", None, "2021-06-30", 1000), fact("equity_incl_minority", None, "2021-06-30", 300),
        fact("minority_int_bs", None, "2021-06-30", 20), fact("pref_stock", None, "2021-06-30", 30),
        fact("cash", None, "2021-06-30", 40), fact("st_investments", None, "2021-06-30", 10),
        fact("st_debt", None, "2021-06-30", 5), fact("lt_debt", None, "2021-06-30", 100),
        fact("stockholders_equity", None, "2021-03-31", 240),   # be_lag1q
        fact("stockholders_equity", None, "2020-03-31", 200),   # be_lag1q_lag4
        fact("stockholders_equity", None, "2020-06-30", 210),
        fact("total_assets", None, "2020-06-30", 900), fact("total_liabilities", None, "2020-06-30", 690),
        fact("cash_and_st_investments", None, "2020-06-30", 60),
        fact("net_income", "2021-04-01", "2021-06-30", 12), fact("net_income", "2020-04-01", "2020-06-30", 9),
        fact("income_tax", "2021-01-01", "2021-06-30", 7), fact("income_tax", "2021-01-01", "2021-03-31", 3),
    ]
    row = bfe.process_cik(8, [acc("q", "2021-08-01T21:00:00", "10-Q", facts, period="2021-06-30")], {})[0]
    assert item(row, "be") == 300 - 20 - 30
    assert item(row, "lt") == 1000 - 300 and item(row, "che") == 50 and item(row, "debt") == 105
    assert item(row, "noa") == 1000 - 50 - 700 + 105 and item(row, "noa_lag4") == 900 - 60 - 690 + 0
    assert item(row, "be_lag1q") == 240 and item(row, "be_lag1q_lag4") == 200
    assert (item(row, "ni_q"), item(row, "ni_q_lag4"), item(row, "txt_q")) == (12, 9, 4)
    assert math.isnan(item(row, "txt_q_lag4")) and row[-3] == "noa_lag4"


CMAP = bfe.load_concept_map()


def cfact(concept, start, end, value):
    """A fact carrying the real statement-map metric and rank of ``concept``."""
    mid, rank, _unit = CMAP.concept_to[concept]
    return (mid, rank, None if start is None else d(start), d(end), float(value))


def test_mixed_revenue_filer_total_wins_and_gp_uses_it():
    counts = {}
    k20, k21 = [], []
    for year, out, rev, rfc, cor in ((2020, k20, 1000, 800, 600), (2021, k21, 1200, 950, 700)):
        s, e = f"{year}-01-01", f"{year}-12-31"
        out += [cfact("RevenueFromContractWithCustomerExcludingAssessedTax", s, e, rfc),  # ASC 606 component
                cfact("Revenues", s, e, rev),                                             # taxonomy total
                cfact("CostOfRevenue", s, e, cor), cfact("Assets", None, e, 5000)]
    rows = bfe.process_cik(11, [acc("k20", "2021-02-20T21:00:00", "10-K", k20, period="2020-12-31"),
                                acc("k21", "2022-02-20T21:00:00", "10-K", k21, period="2021-12-31")], counts)
    assert item(rows[1], "sale_ttm") == 1200 and item(rows[1], "gp_ttm") == 1200 - 700
    assert counts["revenue_rfc_lt_revenues_keys"] == 2
    assert counts["concept_disagreement_keys"] == {"revenue": 2}
    # contract revenue stays the fallback when the total is not tagged
    only = [cfact("RevenueFromContractWithCustomerExcludingAssessedTax", "2021-01-01", "2021-12-31", 950),
            cfact("CostOfRevenue", "2021-01-01", "2021-12-31", 700), cfact("Assets", None, "2021-12-31", 5000)]
    row = bfe.process_cik(12, [acc("k", "2022-02-20T21:00:00", "10-K", only, period="2021-12-31")], {})[0]
    assert (item(row, "sale_ttm"), item(row, "gp_ttm")) == (950, 250)


def test_total_tagged_later_wins_and_later_component_is_shadowed():
    s, e = "2020-01-01", "2020-12-31"
    first = acc("a", "2021-02-20T21:00:00", "10-K", [cfact("RevenueFromContractWithCustomerExcludingAssessedTax",
                                                           s, e, 800), cfact("Assets", None, e, 10)], period=e)
    total = acc("b", "2021-05-01T21:00:00", "10-K/A", [cfact("Revenues", s, e, 1000)], period=e)
    comp = acc("c", "2021-06-01T21:00:00", "10-K/A", [cfact("RevenueFromContractWithCustomerExcludingAssessedTax",
                                                          s, e, 810)], period=e)
    counts = {}
    rows = bfe.process_cik(13, [first, total, comp], counts)
    assert [item(r, "sale_ttm") for r in rows] == [800, 1000, 1000]
    assert counts["rank_shadowed_facts"] == 1 and counts["restated_facts"] == 0


def test_report_period_falls_back_when_the_sub_period_does_not_snap():
    counts = {}
    facts = [fact("total_assets", None, "2021-06-19", 100), fact("total_assets", None, "2020-06-20", 90)]
    a = acc("m", "2021-08-01T21:00:00", "10-Q", facts, period="2021-06-30")  # FSDS month-end period, 11 d away
    assert bfe.report_period(a, counts) == d("2021-06-19") and counts["report_period_snap_fallback"] == 1
    row = bfe.process_cik(14, [a], {})[0]
    assert row[7] == d("2021-06-19") and item(row, "at") == 100 and item(row, "at_lag4") == 90
    late = acc("x", "2021-08-01T21:00:00", "10-Q", [fact("total_assets", None, "2021-09-30", 1)], period="2021-06-30")
    assert bfe.report_period(late) is None  # a core end after the filing date never sets the anchor


def test_split_restated_comparative_keeps_the_share_pair_consistent():
    q, lq = ("2021-10-01", "2021-12-31"), ("2020-10-01", "2020-12-31")
    pre = acc("p", "2021-02-20T21:00:00", "10-K", [fact("shares_diluted_avg", *lq, 100.0),
                                                   fact("total_assets", None, lq[1], 10)], period=lq[1])
    # 4:1 split during 2021: the 2021 filing re-reports the prior-year quarter on the post-split basis
    post = acc("n", "2022-02-20T21:00:00", "10-K", [fact("shares_diluted_avg", *q, 404.0),
                                                    fact("shares_diluted_avg", *lq, 400.0),
                                                    fact("total_assets", None, q[1], 11)], period=q[1])
    counts = {}
    row = bfe.process_cik(15, [pre, post], counts)[-1]
    assert (item(row, "shrs_q"), item(row, "shrs_q_lag4")) == (404.0, 400.0)
    assert counts["restated_facts"] == 1 and "share_pairs_unconfirmed" not in counts
    # comparative not re-reported: the stale pre-split lag is kept (values unchanged) but counted
    post_nc = acc("n", "2022-02-20T21:00:00", "10-K", [fact("shares_diluted_avg", *q, 404.0),
                                                       fact("total_assets", None, q[1], 11)], period=q[1])
    counts = {}
    row = bfe.process_cik(15, [pre, post_nc], counts)[-1]
    assert item(row, "shrs_q_lag4") == 100.0 and counts["share_pairs_unconfirmed"] == 1


def test_sub_clock_cast_is_unit_safe(tmp_path):
    rows = [{"adsh": "0000000001-21-000001", "cik": "0000000001", "sic": "3571", "form": "10-K",
             "period": dt.date(2020, 12, 31), "fy": 2020, "fp": "FY", "filed": dt.date(2021, 2, 20),
             "accepted_utc": dt.datetime(2021, 2, 20, 21, 30, tzinfo=UTC), "fye": "1231"}]
    schema = pa.schema([f if f.name != "accepted_utc" else pa.field("accepted_utc", pa.timestamp("ns", tz="UTC"))
                        for f in SUB_SCHEMA])
    (tmp_path / "sub").mkdir()
    path = tmp_path / "sub" / "2021q1.parquet"
    pq.write_table(pa.Table.from_pylist(rows, schema=schema), path)
    clock, _sic, _stats = bfe.build_clock_tables(tmp_path, [{"quarter": "2021q1", "file": "sub/2021q1.parquet",
                                                              "sha256": sha(path)}], {1})
    assert clock.column("accepted_us").to_pylist() == [us("2021-02-20T21:30:00")]


def fscore_state():
    facts = []
    for year, at, ni, cfo, ltd, ca, cl, sale, gp, sstk in (
            (2019, 900, 70, 80, 250, 350, 250, 700, 250, None),
            (2020, 1000, 80, 90, 250, 400, 250, 800, 300, None),
            (2021, 1200, 120, 150, 200, 500, 250, 1000, 400, 10)):
        s, e = f"{year}-01-01", f"{year}-12-31"
        facts += [fact("total_assets", None, e, at), fact("net_income", s, e, ni),
                  fact("operating_cash_flow", s, e, cfo), fact("lt_debt", None, e, ltd),
                  fact("current_assets", None, e, ca), fact("current_liabilities", None, e, cl),
                  fact("revenue", s, e, sale), fact("gross_profit", s, e, gp),
                  fact("stockholders_equity", None, e, at / 2)]
        if sstk is not None:
            facts.append(fact("stock_issuance", s, e, sstk))
    return facts


def test_fscore_all_nine_signals():
    row = bfe.process_cik(3, [acc("k", "2022-02-20T21:00:00", "10-K", fscore_state(), period="2021-12-31")], {})[0]
    # roa+, cfo+, droa+, accrual+, lever down, liquidity up, equity issued (0), margin up, turnover up
    assert item(row, "fscore") == 8.0
    flags = row[-3].split(",")
    assert "debt" in flags and "fscore" not in flags  # lt_debt and stock issuance were both reported
    facts = [f for f in fscore_state() if f[0] != bfe.M["stock_issuance"]]
    row = bfe.process_cik(3, [acc("k", "2022-02-20T21:00:00", "10-K", facts, period="2021-12-31")], {})[0]
    assert item(row, "fscore") == 9.0 and {"fscore", "sstk_ttm"} <= set(row[-3].split(","))
    facts = [f for f in fscore_state() if not (f[0] == bfe.M["current_assets"] and f[3] == d("2020-12-31"))]
    row = bfe.process_cik(3, [acc("k", "2022-02-20T21:00:00", "10-K", facts, period="2021-12-31")], {})[0]
    assert math.isnan(item(row, "fscore"))


def quarter_filings(values, restate=None):
    """One 10-Q/10-K per quarter from 2018Q1 with a direct 3-month net-income fact."""
    ends = ["03-31", "06-30", "09-30", "12-31"]
    starts = ["01-01", "04-01", "07-01", "10-01"]
    out = []
    for i, value in enumerate(values):
        year, k = 2018 + i // 4, i % 4
        end = f"{year}-{ends[k]}"
        filed = (dt.date.fromisoformat(end) + dt.timedelta(days=40)).isoformat() + "T21:00:00"
        facts = [fact("net_income", f"{year}-{starts[k]}", end, value), fact("total_assets", None, end, 1000)]
        if restate is not None and i == len(values) - 2:
            facts.append(fact("net_income", *restate))
        out.append(acc(f"f{i:02d}", filed, "10-K" if k == 3 else "10-Q", facts, period=end))
    return out


def expected_sue(values):
    diffs = [values[i] - values[i - 4] for i in range(4, len(values))]
    cur, hist = diffs[-1], diffs[-9:-1]
    return cur / statistics.stdev(hist)


def test_sue_on_first_reported_quarters():
    values = [10, 12, 11, 14, 11, 13, 12, 16, 12, 15, 13, 17, 14, 16, 15, 25]
    rows = bfe.process_cik(5, quarter_filings(values), {})
    assert item(rows[-1], "sue") == pytest.approx(expected_sue(values[-13:]))
    assert item(rows[-1], "ni_q") == 25 and item(rows[-1], "ni_q_lag4") == 17
    # A later restatement of an old quarter changes knowledge, never the first-reported SUE history.
    restated = bfe.process_cik(5, quarter_filings(values, restate=("2019-01-01", "2019-03-31", 99)), {})
    assert item(restated[-1], "sue") == pytest.approx(item(rows[-1], "sue"))
    assert math.isnan(item(rows[6], "sue"))  # too little history


def test_share_pair_scale_error_is_dropped():
    counts = {}
    facts = [fact("shares_diluted_avg", "2021-10-01", "2021-12-31", 1.0e6),
             fact("shares_diluted_avg", "2020-10-01", "2020-12-31", 1.0e9),
             fact("total_assets", None, "2021-12-31", 10)]
    row = bfe.process_cik(4, [acc("k", "2022-02-20T21:00:00", "10-K", facts, period="2021-12-31")], counts)[0]
    assert math.isnan(item(row, "shrs_q")) and math.isnan(item(row, "shrs_q_lag4"))
    assert counts["share_pairs_rejected"] == 1
    facts[1] = fact("shares_diluted_avg", "2020-10-01", "2020-12-31", 0.9e6)
    row = bfe.process_cik(4, [acc("k", "2022-02-20T21:00:00", "10-K", facts, period="2021-12-31")], {})[0]
    assert (item(row, "shrs_q"), item(row, "shrs_q_lag4")) == (1.0e6, 0.9e6)


def test_staleness_quarterly_vs_annual_only():
    annual = bfe.process_cik(6, [fy_filing("f1", "2020-04-20T21:00:00", 2019, 1, 10, 100, form="20-F"),
                                 fy_filing("f2", "2021-04-20T21:00:00", 2020, 1, 10, 100, form="20-F")], {})
    assert [r[11] for r in annual] == [400, 400]
    q = acc("q", "2020-05-01T21:00:00", "10-Q", [fact("total_assets", None, "2020-03-31", 100)], period="2020-03-31")
    mixed = bfe.process_cik(6, [fy_filing("k", "2020-02-20T21:00:00", 2019, 1, 10, 100), q,
                                fy_filing("k2", "2021-02-20T21:00:00", 2020, 1, 10, 100),
                                fy_filing("k3", "2022-06-20T21:00:00", 2021, 1, 10, 100)], {})
    assert [r[11] for r in mixed] == [400, 200, 200, 400]


# ---------------------------------------------------------------------------
# End to end: prepare -> events (chunked, resumable) -> finalize, on synthetic parquet sources
# ---------------------------------------------------------------------------

CF_SCHEMA = pa.schema([("cik", pa.string()), ("taxonomy", pa.string()), ("concept", pa.string()), ("unit", pa.string()),
                       ("period_start", pa.date32()), ("period_end", pa.date32()), ("filed_date", pa.date32()),
                       ("fiscal_year", pa.int32()), ("fiscal_period", pa.string()), ("form", pa.string()),
                       ("accession_number", pa.string()), ("value", pa.float64())])
SUB_SCHEMA = pa.schema([("adsh", pa.string()), ("cik", pa.string()), ("sic", pa.string()), ("form", pa.string()),
                        ("period", pa.date32()), ("fy", pa.int32()), ("fp", pa.string()), ("filed", pa.date32()),
                        ("accepted_utc", pa.timestamp("us", tz="UTC")), ("fye", pa.string())])


def cf_row(cik, concept, start, end, filed, form, accn, value, unit="USD", taxonomy="us-gaap"):
    return {"cik": f"{cik:010d}", "taxonomy": taxonomy, "concept": concept, "unit": unit,
            "period_start": None if start is None else dt.date.fromisoformat(start),
            "period_end": dt.date.fromisoformat(end), "filed_date": dt.date.fromisoformat(filed),
            "fiscal_year": int(end[:4]), "fiscal_period": "FY", "form": form, "accession_number": accn,
            "value": float(value)}


def cf_annual(cik, year, filed, accn, form="10-K", scale=1.0):
    s, e = f"{year}-01-01", f"{year}-12-31"
    return [cf_row(cik, "Assets", None, e, filed, form, accn, 1000 * scale),
            cf_row(cik, "StockholdersEquity", None, e, filed, form, accn, 400 * scale),
            cf_row(cik, "NetIncomeLoss", s, e, filed, form, accn, 50 * scale),
            cf_row(cik, "Revenues", s, e, filed, form, accn, 900 * scale)]


def sub_row(adsh, cik, sic, form, period, filed, accepted, fy=None, fp="FY"):
    return {"adsh": adsh, "cik": f"{cik:010d}", "sic": sic, "form": form,
            "period": dt.date.fromisoformat(period), "fy": fy or int(period[:4]), "fp": fp,
            "filed": dt.date.fromisoformat(filed),
            "accepted_utc": dt.datetime.fromisoformat(accepted).replace(tzinfo=UTC), "fye": "1231"}


@pytest.fixture()
def sources(tmp_path):
    cf = tmp_path / "cf"
    cf.mkdir()
    b0 = (cf_annual(1001, 2012, "2013-02-20", "0001001-13-000001")
          + cf_annual(1001, 2019, "2020-02-20", "0001001-20-000001")
          + cf_annual(1001, 2020, "2021-02-20", "0001001-21-000001", scale=1.1)
          + [cf_row(1001, "Assets", None, "2020-12-31", "2021-02-20", "10-K", "0001001-21-000001", 5, unit="CAD"),
             cf_row(1001, "SomethingElse", None, "2020-12-31", "2021-02-20", "10-K", "0001001-21-000001", 5),
             cf_row(1001, "Assets", None, "2020-12-31", "2021-02-20", "8-K", "0001001-21-000009", 7),
             cf_row(1001, "WeightedAverageNumberOfDilutedSharesOutstanding", "2020-01-01", "2020-12-31",
                    "2021-02-20", "10-K", "0001001-21-000001", 9, unit="USD"),                 # unit mismatch
             cf_row(1001, "NetIncomeLoss", "2020-01-01", "2020-12-31", "2021-02-20", "10-K",
                    "0001001-21-000001", float("nan")),                                         # non-finite
             cf_row(1001, "Revenues", "2018-01-01", "2020-12-31", "2021-02-20", "10-K",
                    "0001001-21-000001", 2700)]                                                 # 3-year span
          + cf_annual(1002, 2020, "2021-03-01", "0001002-21-000001")            # not in SUB -> FC1
          + [cf_row(1003, c, "2020-01-01", "2020-12-31", "2021-03-05", "10-K", "0001003-21-000001", v)
             for c, v in (("RevenueFromContractWithCustomerExcludingAssessedTax", 700), ("Revenues", 900),
                          ("CostOfRevenue", 500))]
          + [cf_row(1003, "Assets", None, "2020-12-31", "2021-03-05", "10-K", "0001003-21-000001", 3000)]
          + cf_annual(1500, 2020, "2021-03-01", "0001500-21-000001"))           # out of scope
    b1 = (cf_annual(2001, 2023, "2024-02-15", "0002001-24-000001")              # FC1
          + cf_annual(2001, 2024, "2024-12-31", "0002001-24-000099")            # accepted at the seal
          + cf_annual(2001, 2025, "2025-02-15", "0002001-25-000001"))           # filed after the seal
    batches = []
    for i, (rows, lo, hi) in enumerate(((b0, 1000, 1999), (b1, 2000, 2999))):
        path = cf / f"batch-{i:04d}.parquet"
        pq.write_table(pa.Table.from_pylist(rows, schema=CF_SCHEMA), path)
        batches.append({"batch_id": i, "file": path.name, "parquet_sha256": sha(path), "first_cik": lo,
                        "last_cik": hi, "rows": len(rows)})
    (cf / "manifest.json").write_text(json.dumps({"archive": {"sha256": "ab" * 32}, "rule_version": "cf-extract-v2",
                                                  "batches": batches}))
    fsds = tmp_path / "fsds"
    (fsds / "sub").mkdir(parents=True)
    subs = {
        "2019q4": [],
        "2020q1": [sub_row("0001001-20-000001", 1001, "3571", "10-K", "2019-12-31", "2020-02-20",
                           "2020-02-20T21:30:00"),
                   sub_row("0001500-20-000001", 1500, "1000", "10-K", "2019-12-31", "2020-02-20",
                           "2020-02-20T21:30:00")],
        "2021q1": [sub_row("0001001-21-000001", 1001, "7372", "10-K", "2020-12-31", "2021-02-20",
                           "2021-02-20T14:05:00"),
                   sub_row("0001001-21-000002", 1001, None, "8-K", "2020-12-31", "2021-02-25",
                           "2021-02-25T14:05:00")],
        "2024q4": [sub_row("0002001-24-000099", 2001, "2834", "10-K", "2024-12-31", "2024-12-31",
                           "2025-01-01T00:00:00")],
    }
    quarters = {}
    for q, rows in subs.items():
        path = fsds / "sub" / f"{q}.parquet"
        pq.write_table(pa.Table.from_pylist(rows, schema=SUB_SCHEMA), path)
        quarters[q] = {"tables": {"sub": {"path": f"sub/{q}.parquet", "parquet_sha256": sha(path)}}}
    post = fsds / "sub" / "2025q1.parquet"
    post.write_bytes(b"not a parquet file: opening it would fail")  # the seal: must never be opened
    quarters["2025q1"] = {"tables": {"sub": {"path": "sub/2025q1.parquet", "parquet_sha256": sha(post)}}}
    (fsds / "fsds-staging-manifest.json").write_text(json.dumps({"quarters": quarters}))
    ciks = tmp_path / "ciks.txt"
    ciks.write_text("# T19 scope\n1001\n0000001002\n1003\n2001\n")
    return {"cf": cf, "fsds": fsds, "ciks": ciks, "out": tmp_path / "out",
            "cf_sha": sha(cf / "manifest.json"), "fsds_sha": sha(fsds / "fsds-staging-manifest.json")}


def run(stage, s, *extra):
    argv = [stage, "--out", str(s["out"])]
    if stage == "prepare":
        argv += ["--cik-list", str(s["ciks"]), "--companyfacts-dir", str(s["cf"]), "--fsds-dir", str(s["fsds"]),
                 "--companyfacts-manifest-sha256", s["cf_sha"], "--fsds-manifest-sha256", s["fsds_sha"]]
    return bfe.main(argv + list(extra))


def test_end_to_end_clock_seal_sic_resume_and_manifest_last(sources):
    s = sources
    assert run("prepare", s) == 0
    assert run("prepare", s) == 0  # idempotent: verified no-op
    sic = pq.read_table(s["out"] / "sic_events.parquet")
    assert sic.schema == bfe.SIC_SCHEMA
    assert sic.column("cik").to_pylist() == [1001, 1001] and sic.column("sic").to_pylist() == [3571, 7372]

    assert run("events", s, "--batches", "0") == 0
    assert run("finalize", s) == 2 and not (s["out"] / "manifest.json").exists()  # batch 1 missing
    assert run("events", s, "--batches", "0-1") == 0                               # batch 0 verified, 1 built
    assert run("finalize", s) == 0
    manifest = json.loads((s["out"] / "manifest.json").read_text())
    assert manifest["schema"] == bfe.SCHEMA and manifest["status"] == "complete"
    events_path = s["out"] / "fundamental_events.parquet"
    assert manifest["files"]["fundamental_events.parquet"]["sha256"] == sha(events_path)
    assert manifest["files"]["sic_events.parquet"]["sha256"] == sha(s["out"] / "sic_events.parquet")
    assert [q["quarter"] for q in manifest["inputs"]["fsds"]["sub_quarters"]] == ["2019q4", "2020q1", "2021q1",
                                                                                  "2024q4"]
    ev = pq.read_table(events_path)
    assert ev.schema == bfe.EVENT_SCHEMA
    rows = ev.to_pylist()
    assert [(r["cik"], r["accession"]) for r in rows] == [
        (1001, "0001001-20-000001"), (1001, "0001001-21-000001"), (1002, "0001002-21-000001"),
        (1003, "0001003-21-000001"), (2001, "0002001-24-000001")]
    r0, r1, r2, mixed, r3 = rows
    assert (mixed["sale_ttm"], mixed["gp_ttm"]) == (900, 400)  # Revenues total; gp = total - cost of revenue
    assert r0["clock_basis"] == bfe.BASIS_FSDS and r0["accepted_utc"] == dt.datetime(2020, 2, 20, 21, 30, tzinfo=UTC)
    assert r2["clock_basis"] == bfe.BASIS_FC1 and r2["accepted_utc"] == dt.datetime(2021, 3, 2, 22, 0, tzinfo=UTC)
    assert r3["clock_basis"] == bfe.BASIS_FC1
    assert r1["at"] == pytest.approx(1100) and r1["at_lag4"] == 1000 and r1["ni_ttm"] == pytest.approx(55)
    assert r0["period_end"] == dt.date(2019, 12, 31) and r0["fiscal_year"] == 2019 and r0["fiscal_year_end"] == "1231"
    assert r0["n_facts"] == 4 and r0["staleness_days"] == 400
    assert manifest["counts"]["rows"] == 5 and manifest["counts"]["rows_fc1"] == 3
    totals = manifest["counts"]["batch_totals"]
    assert totals["accessions_sealed"] == 1
    assert (totals["unit_mismatch_dropped"], totals["nonfinite_dropped"], totals["duration_shape_dropped"]) == (1, 1, 1)
    assert totals["revenue_rfc_lt_revenues_keys"] == 1
    assert manifest["parameters"]["precedence_overrides"]["revenue"] == ["Revenues"]
    assert (s["out"] / "cik_scope.txt").read_text() == "1001\n1002\n1003\n2001\n"
    assert manifest["inputs"]["cik_list"]["count"] == 4

    assert run("events", s, "--batches", "0-1") == 0  # both verified and skipped
    assert run("finalize", s) == 2                    # manifest is published once
    data = s["out"] / "events" / "batch-0000.parquet"
    data.write_bytes(data.read_bytes() + b"x")
    assert run("events", s, "--batches", "0") == 2    # tampered output refuses resume


def test_budget_stop_is_resumable_and_pins_are_enforced(sources):
    s = sources
    bad = dict(s, cf_sha="0" * 64)
    assert run("prepare", bad) == 2
    assert run("prepare", s) == 0
    assert run("events", s, "--batches", "0-1", "--max-seconds", "10", "--projected-batch-seconds", "60") == 3
    assert not list((s["out"] / "events").glob("*.receipt.json"))
    assert run("events", s, "--batches", "0-1") == 0
    assert run("finalize", s) == 0


def test_cik_list_formats(tmp_path):
    txt = tmp_path / "a.txt"
    txt.write_text("# c\n0000000042\n7 # trailing\n\n42\n")
    assert bfe.read_cik_list(txt) == [7, 42]
    csv_path = tmp_path / "b.csv"
    csv_path.write_text("sr_id,cik\n1,0000000042\n2,9\n")
    assert bfe.read_cik_list(csv_path) == [9, 42]
    pq_path = tmp_path / "c.parquet"
    pq.write_table(pa.table({"cik": pa.array(["0000000005", None, "11"])}), pq_path)
    assert bfe.read_cik_list(pq_path) == [5, 11]
    bad = tmp_path / "d.txt"
    bad.write_text("abc\n")
    with pytest.raises(bfe.UsageError):
        bfe.read_cik_list(bad)
