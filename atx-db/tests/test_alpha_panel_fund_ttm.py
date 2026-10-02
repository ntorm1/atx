"""S4.1 cross-concept quarter / TTM repair (fund_items.Snapshot): Q4 = FY - 9M (or FY - Q1 - Q2 - Q3) when the
fiscal-year and interim facts sit in different concepts of one item chain; clock = the latest component clock."""

from __future__ import annotations

import datetime as dt

from atx_db.alpha_panel import fund_items as fi

D = dt.date
T = dt.datetime
RFC = "RevenueFromContractWithCustomerExcludingAssessedTax"


def row(clock, accn, concept, start, end, value, unit="USD", form="10-Q", fy=None, fp=None):
    return (clock, accn, fi.CID[concept], start, end, value, unit, form, clock.date(), fy, fp, "fsds_accepted_utc")


C_Q1, C_Q2, C_Q3 = T(2020, 1, 30, 21), T(2020, 4, 30, 21), T(2020, 7, 30, 21)
C_K, C_Q1B = T(2020, 11, 19, 21), T(2021, 1, 28, 21)
FY_S, FY_E = D(2019, 10, 1), D(2020, 9, 30)


def visa_rows(ytd: bool = True):
    """Visa FY2020 revenue: the 10-Qs tag the ASC 606 concept, the 10-K tags only ``Revenues`` for the year."""
    rows = [row(C_Q1, "q1", RFC, FY_S, D(2019, 12, 31), 6054.0),
            row(C_Q1, "q1", "Assets", None, D(2019, 12, 31), 1000.0),
            row(C_Q2, "q2", RFC, D(2020, 1, 1), D(2020, 3, 31), 5854.0),
            row(C_Q2, "q2", "Assets", None, D(2020, 3, 31), 1000.0),
            row(C_Q3, "q3", RFC, D(2020, 4, 1), D(2020, 6, 30), 4837.0),
            row(C_Q3, "q3", "Assets", None, D(2020, 6, 30), 1000.0),
            row(C_K, "k20", "Revenues", FY_S, FY_E, 21846.0, form="10-K"),
            row(C_K, "k20", "Assets", None, FY_E, 1000.0, form="10-K"),
            row(C_Q1B, "q1b", RFC, D(2020, 10, 1), D(2020, 12, 31), 5687.0),
            row(C_Q1B, "q1b", "Assets", None, D(2020, 12, 31), 1000.0)]
    if ytd:
        rows += [row(C_Q2, "q2", RFC, FY_S, D(2020, 3, 31), 11908.0),
                 row(C_Q3, "q3", RFC, FY_S, D(2020, 6, 30), 16745.0)]
    return sorted(rows, key=lambda r: (r[0], r[1]))


def events(rows, **kw):
    ev, _ = fi.company_events(1, rows, T(2019, 1, 1), {}, **kw)
    return {e["accession"]: e for e in ev}


def test_q4_from_fy_minus_9m_in_another_concept():
    ev = events(visa_rows())
    assert ev["k20"]["sale_ttm"] == 21846.0
    assert ev["k20"]["sale_q"] == 21846.0 - 16745.0
    assert ev["q1b"]["sale_q"] == 5687.0
    assert ev["q1b"]["sale_ttm"] == 5687.0 + (21846.0 - 16745.0) + 4837.0 + 5854.0


def test_q4_from_fy_minus_three_discrete_quarters():
    ev = events(visa_rows(ytd=False))
    assert ev["k20"]["sale_q"] == 21846.0 - 6054.0 - 5854.0 - 4837.0
    assert ev["q1b"]["sale_ttm"] == 5687.0 + (21846.0 - 6054.0 - 5854.0 - 4837.0) + 4837.0 + 5854.0


def test_cross_concept_quarter_clock_is_the_latest_component():
    """The 9M comparative of the other concept arrives only with a later filing: Q4 is derivable from that clock on."""
    rows = [r for r in visa_rows() if r[1] not in ("q3",)]
    late = T(2021, 7, 29, 21)
    rows += [row(late, "q3b", RFC, FY_S, D(2020, 6, 30), 16745.0),             # prior-year 9M comparative
             row(late, "q3b", RFC, D(2020, 10, 1), D(2021, 6, 30), 17000.0),
             row(late, "q3b", "Assets", None, D(2021, 6, 30), 1000.0)]
    rows.sort(key=lambda r: (r[0], r[1]))
    hist: list[tuple] = []
    ev_list = fi.issuer_events(1, rows, T(2019, 1, 1), {}, None, None, hist)
    ev = {e["accession"]: e for e in ev_list}
    assert ev["k20"]["sale_q"] is None                           # 9M of the ASC 606 concept not yet known
    q4 = [h for h in hist if h[0] == "sale_q" and h[1] == FY_E]
    assert [(h[3], h[4], h[5]) for h in q4] == [("q3b", 21846.0 - 16745.0, late)]


def test_single_concept_derivation_keeps_priority():
    """A within-concept quarter (Revenues 9M YTD) wins over the cross-concept difference."""
    rows = visa_rows() + [row(C_Q3, "q3", "Revenues", FY_S, D(2020, 6, 30), 16700.0)]
    rows.sort(key=lambda r: (r[0], r[1]))
    ev = events(rows)
    assert ev["k20"]["sale_q"] == 21846.0 - 16700.0


def test_cross_concept_ytd_ttm():
    """Current 6M YTD (ASC 606) + prior FY (Revenues) - prior 6M YTD (ASC 606), with no current discrete quarter."""
    c = T(2021, 4, 29, 21)
    rows = [row(C_Q2, "q2", RFC, FY_S, D(2020, 3, 31), 11908.0),
            row(C_K, "k20", "Revenues", FY_S, FY_E, 21846.0, form="10-K"),
            row(C_K, "k20", "Assets", None, FY_E, 1000.0, form="10-K"),
            row(c, "q2b", RFC, D(2020, 10, 1), D(2021, 3, 31), 12000.0),
            row(c, "q2b", "Assets", None, D(2021, 3, 31), 1000.0)]
    ev = events(sorted(rows, key=lambda r: (r[0], r[1])))
    assert ev["q2b"]["sale_ttm"] == 12000.0 + 21846.0 - 11908.0


def _quarter_rows(concept, vals, year, clock_by_q, accn_by_q, form="10-Q"):
    ends = [D(year, 3, 31), D(year, 6, 30), D(year, 9, 30), D(year, 12, 31)]
    starts = [D(year, 1, 1), D(year, 4, 1), D(year, 7, 1), D(year, 10, 1)]
    return [row(clock_by_q[i], accn_by_q[i], concept, starts[i], ends[i], v, form=form)
            for i, v in enumerate(vals) if v is not None]


def test_item_ttm_from_quarters_of_different_fallback_levels():
    """GrossProfit tagged through 2020; from 2021 only revenue and total costs: gp TTM chains item-level quarters."""
    ck = [T(2020, 5, 1), T(2020, 8, 1), T(2020, 11, 1), T(2021, 2, 20)]
    ak = ["q1", "q2", "q3", "k"]
    rows = (_quarter_rows("GrossProfit", [10.0, 11.0, 12.0, 13.0], 2020, ck, ak)
            + _quarter_rows(RFC, [100.0, 110.0, 120.0, 130.0], 2020, ck, ak))
    c21 = T(2021, 5, 1)
    rows += [row(c21, "q1b", RFC, D(2021, 1, 1), D(2021, 3, 31), 140.0),
             row(c21, "q1b", "CostsAndExpenses", D(2021, 1, 1), D(2021, 3, 31), 126.0),
             row(c21, "q1b", "Assets", None, D(2021, 3, 31), 1000.0)]
    ev = events(sorted(rows, key=lambda r: (r[0], r[1])))
    e = ev["q1b"]
    assert e["gp_q"] == 14.0
    assert e["gp_ttm"] == 14.0 + 13.0 + 12.0 + 11.0 and e["gp_src"] == "quarters_mixed"
    assert ev["k"]["gp_ttm"] == 46.0 and ev["k"]["gp_src"] == "GrossProfit"          # v9 path unchanged


def test_stub_periods_tile_a_quarter():
    """Fresh-start accounting: predecessor Jan 1 - Feb 2 and successor Feb 3 - Mar 31 form Q1."""
    ck = [T(2020, 5, 1), T(2020, 8, 1), T(2020, 11, 1), T(2021, 2, 20)]
    rows = _quarter_rows("Revenues", [100.0, 110.0, 120.0, 130.0], 2020, ck, ["q1", "q2", "q3", "k"])
    c = T(2021, 5, 1)
    rows += [row(c, "q1b", "Revenues", D(2021, 1, 1), D(2021, 2, 2), 40.0),
             row(c, "q1b", "Revenues", D(2021, 2, 3), D(2021, 3, 31), 95.0),
             row(c, "q1b", "Assets", None, D(2021, 3, 31), 1000.0)]
    ev = events(sorted(rows, key=lambda r: (r[0], r[1])))
    assert ev["q1b"]["sale_q"] == 135.0
    assert ev["q1b"]["sale_ttm"] == 135.0 + 130.0 + 120.0 + 110.0


def test_prefix_invariance_of_cross_concept_rows():
    rows = visa_rows()
    full = events(rows)
    for cutoff in (C_Q3, C_K, C_Q1B):
        part = events([r for r in rows if r[0] < cutoff])
        for accn, e in part.items():
            assert e == full[accn], accn
