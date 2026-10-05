"""S3.6 index proxies v2: Russell rank / band rule, S&P-like buffer rule, schedules, point-in-time vintages (A.4),
R7 (a) / (d) rank snapshots with measured receipt keys (A.5), the year-partitioned stage and the registry."""

from __future__ import annotations

import datetime as dt
import json

import pyarrow as pa
import pyarrow.parquet as pq
import pytest

from atx_db.alpha_panel import common as C
from atx_db.alpha_panel import exchange_calendar as E
from atx_db.alpha_panel import indexes as X
from atx_db.alpha_panel import private_index_pit as P

D = dt.date
UTC = dt.UTC


def mark(d: dt.date) -> dt.datetime:
    return dt.datetime(d.year, d.month, d.day, 22, tzinfo=UTC)


def universe(n: int = 3500) -> list[tuple[int, float]]:
    # company k has ME decreasing in k (k = 1 is the largest); every one above $30M
    return [(k, 1e12 / k) for k in range(1, n + 1)]


def test_russell_first_year_is_pure_rank():
    out, p1000 = X.russell_assign(universe(), set(), set())
    assert sum(1 for v in out.values() if v[0] == "R1000") == 1000
    assert sum(1 for v in out.values() if v[0] == "R2000") == 2000 and len(out) == 3000
    assert out[1000][0] == "R1000" and out[1001][0] == "R2000" and 3001 not in out
    assert 0 < p1000 < 1


def test_russell_band_keeps_prior_members_near_the_breakpoint():
    comp = universe()
    out, p1000 = X.russell_assign(comp, set(), set())
    cp = {k: v[2] for k, v in out.items()}
    inside_below = next(k for k in range(1001, 3000) if cp[k] <= p1000 + X.BAND and cp[k + 1] > p1000 + X.BAND)
    far = inside_below + 50
    # prior R1000 companies ranked below 1000 but inside the band stay; outside the band they drop to R2000
    out2, _ = X.russell_assign(comp, prev_r1000={1005, inside_below, far}, prev_r2000={990})
    assert out2[1005][0] == "R1000" and out2[inside_below][0] == "R1000" and out2[far][0] == "R2000"
    # a prior R2000 company ranked just above 1000 stays in R2000 (inside the band)
    assert out2[990][0] == "R2000"
    assert out2[5][0] == "R1000"


def test_russell_minimum_market_cap():
    out, _ = X.russell_assign([(1, 1e9), (2, 29e6)], set(), set())
    assert set(out) == {1}


def test_sp_select_buffer_and_entry_screens():
    comp = [(k, 1e12 / k, k % 7 != 0) for k in range(1, 800)]      # every 7th company fails the entry screens
    first = X.sp_select(comp, set())
    assert len(first) == 500 and 7 not in first and all(b == "entry" for _, b in first.values())
    prev = set(first) | {7, 650}                                   # 7 (screen fails) and 650 (beyond buffer)
    nxt = X.sp_select(comp, prev)
    assert nxt[7] == (7, "buffer_stay") and 650 not in nxt and len(nxt) == 500


def test_schedules():
    assert X.third_friday(2024, 6) == D(2024, 6, 21) and X.third_friday(2023, 3) == D(2023, 3, 17)
    sched = X.sp_schedule(D(2018, 2, 1), D(2018, 12, 31))
    assert sched[0] == (D(2018, 2, 28), D(2018, 3, 16)) and len(sched) == 4
    assert X.last_session_on_or_before([D(2024, 4, 29), D(2024, 5, 1)], D(2024, 4, 30)) == D(2024, 4, 29)


def test_schedule_on_the_exchange_calendar_and_rank_dates_with_panel_data():
    cal = E.sessions(D(2017, 1, 3), D(2027, 12, 31))
    s = X.schedule(cal, 2018, 2027)
    assert s["R2000P"][:2] == [D(2018, 6, 22), D(2019, 6, 28)] and s["R2000P"][-4:] == [
        D(2026, 6, 26), D(2026, 12, 11), D(2027, 6, 25), D(2027, 12, 10)]
    # S&P June 2026: the third Friday is Juneteenth, so the reconstitution is the session before
    assert D(2026, 6, 18) in s["SP500P"] and D(2026, 6, 19) not in s["SP500P"] and s["SP500P"][-1] == D(2027, 12, 17)
    with pytest.raises(ValueError, match="does not cover"):
        X.schedule(E.sessions(D(2017, 1, 3), D(2026, 12, 31)), 2018, 2027)
    assert X.russell_ranks(2027)[-3:] == [(D(2026, 10, 30), D(2026, 12, 11)), (D(2027, 4, 30), D(2027, 6, 25)),
                                          (D(2027, 10, 29), D(2027, 12, 10))]
    # panel data from 2018-01-02 to the vendor end 2026-09-18: every reconstitution ranked by then is built
    days = [d for d in cal if D(2018, 1, 2) <= d <= D(2026, 9, 18)]
    ranks = X.rank_schedule(cal, days)
    russell = [(r, e) for f, r, e in ranks if f == "russell"]
    sp = [(r, e) for f, r, e in ranks if f == "sp500"]
    assert russell[0] == (D(2018, 5, 11), D(2018, 6, 22)) and russell[-1] == (D(2026, 4, 30), D(2026, 6, 26))
    assert len(russell) == 9
    assert sp[0] == (D(2018, 2, 28), D(2018, 3, 16)) and sp[-1] == (D(2026, 8, 31), D(2026, 9, 18))
    assert (D(2026, 5, 29), D(2026, 6, 18)) in sp and len(sp) == 35
    assert all(e in s["R2000P" if f == "russell" else "SP500P"] for f, _, e in ranks)
    # A partial first quarter still builds a reconstitution when its rank day has data.
    assert X.rank_schedule(cal, [D(2021, 2, 26)]) == [("sp500", D(2021, 2, 26), D(2021, 3, 19))]


def test_russell_gate_counts_companies_per_recon():
    summ = [{"index_id": "R2000P", "recon_date": D(2020, 6, 26), "companies": 1990, "lines": 2050},
            {"index_id": "R2000P", "recon_date": D(2021, 6, 25), "companies": 2101, "lines": 2150},
            {"index_id": "R1000P", "recon_date": D(2021, 6, 25), "companies": 1000, "lines": 1020}]
    g = X.russell_gate(summ)
    assert g["pass"] is False and g["outside"] == {"2021-06-25": 2101}
    assert g["companies"] == {"2020-06-26": 1990, "2021-06-25": 2101} and g["lines"]["2020-06-26"] == 2050
    assert X.russell_gate(summ[:1])["pass"] is True and X.russell_gate([])["pass"] is False


# ---------------------------------------------------------------- A.4 (request lines 603-656)
# Members: X sid 7, W sid 6 (R1 only), V sid 5 (alive), Z sid 8, Y sid 9 (alive). "Alive" = last_session is the last
# session of the data and delisting_date NULL.
A4_DATES = {"R2000P": [(D(2020, 5, 8), D(2020, 6, 26)), (D(2021, 5, 7), D(2021, 6, 25)), (D(2022, 5, 6), D(2022, 6, 24))],
            "SP500P": [(D(2021, 2, 26), D(2021, 3, 19)), (D(2021, 5, 28), D(2021, 6, 18)), (D(2021, 8, 31), D(2021, 9, 17))]}
A4_EFFECTIVE = {"R2000P": [D(2020, 6, 29), D(2021, 6, 28), D(2022, 6, 27)],
                "SP500P": [D(2021, 3, 22), D(2021, 6, 21), D(2021, 9, 20)]}
A4_END = {"R2000P": [D(2021, 6, 25), D(2022, 6, 24), D(2023, 6, 23)],       # R3's from the schedule constant
          "SP500P": [D(2021, 6, 18), D(2021, 9, 17), D(2021, 12, 17)]}
A4_EXITS = {(7, mark(D(2021, 6, 2))), (6, mark(D(2021, 6, 11))), (8, mark(D(2021, 9, 16)))}


def _a4_calendar(last=None):
    last=last or D(2022,12,30)
    return [D(2020,5,1)+dt.timedelta(days=i) for i in range((last-D(2020,5,1)).days+1)
            if (D(2020,5,1)+dt.timedelta(days=i)).weekday()<5]


def _a4_inputs(index_id,data_end=D(2022,12,30)):
    dates=([(D(2020,5,8),D(2020,6,26)),(D(2021,5,7),D(2021,6,25)),(D(2022,5,6),D(2022,6,24))]
           if index_id=='R2000P' else
           [(D(2021,2,26),D(2021,3,19)),(D(2021,5,28),D(2021,6,18)),(D(2021,8,31),D(2021,9,17))])
    sets=({7,6,5},{7,8,9,5},{5,9})
    recons=[{'index_id':index_id,'rank_date':rank,'recon_date':recon,
        'members':{sid:(100+sid,100.,1000+sid,'rank') for sid in members}}
        for (rank,recon),members in zip(dates,sets)]
    last={7:D(2021,6,1),6:D(2021,6,10),8:D(2021,9,15),5:data_end,9:data_end}
    return recons,last


def _a4_products(recons,cal,last,data_end):
    # the production schedule constant (R2000P: ... 2022-06-24 -> 2023-06-23), complete beyond the data end
    products=X.intervals(recons,cal,last,schedule=X.scheduled_reconstitutions(2020,2023),data_end=data_end)
    products['membership']=list(P.iter_membership(products['constituents'],products['exits'],cal,data_end))
    return products


def _visible(products,cut):
    return {key:sorted((row for row in rows if row['available_at']<cut),key=repr)
            for key,rows in products.items()}


def test_intervals_preserve_rank_and_separately_clock_exit():
    cal = _a4_calendar()
    recons, last = _a4_inputs('R2000P')
    result = _a4_products(recons, cal, last, cal[-1])
    first = next(r for r in result['constituents'] if r['security_id']==7)
    assert first['effective_to'] == D(2021,6,25)
    departure = next(r for r in result['exits'] if r['security_id']==7)
    assert departure['last_session'] == D(2021,6,1)
    assert departure['available_at'] == dt.datetime(2021,6,2,22,tzinfo=UTC)
    final = next(r for r in result['constituents'] if r['recon_date']==D(2022,6,24))
    assert final['effective_to'] == D(2023,6,23)


@pytest.mark.parametrize('index_id',['R2000P','SP500P'])
def test_intervals_are_point_in_time_at_their_stamp(index_id):
    cal=_a4_calendar()
    recons,last=_a4_inputs(index_id)
    assert X.scheduled_reconstitutions(2020,2023)['R2000P'][-2:]==[D(2022,6,24),D(2023,6,23)]
    full=_a4_products(recons,cal,last,cal[-1])
    (_,r1),(r2_rank,r2),(_,r3)=A4_DATES[index_id]
    rows={(r['security_id'],r['recon_date']):r for r in full['constituents']}
    # 1. presence: the planted line X of R2 is in its rank vintage, stamped at the rank mark
    planted=rows[(7,r2)]
    assert planted['index_id']==index_id and planted['available_at']==mark(r2_rank)
    assert planted['effective_from']==A4_EFFECTIVE[index_id][1]
    # every rank vintage holds its whole member set at the rank mark
    for (rank,recon),members in zip(A4_DATES[index_id],({7,6,5},{7,8,9,5},{5,9}),strict=True):
        assert {s for s,r in rows if r==recon}==members
        assert {rows[(s,recon)]['available_at'] for s in members}=={mark(rank)}
    # 2. scheduled ends (R3's from the schedule constant); none equals a last_session
    for recon,eff,end in zip((r1,r2,r3),A4_EFFECTIVE[index_id],A4_END[index_id],strict=True):
        assert {(r['effective_from'],r['effective_to']) for r in full['constituents'] if r['recon_date']==recon}=={(eff,end)}
    assert not {r['effective_to'] for r in full['constituents']}&set(last.values())
    # 3. exit stamps: X 2021-06-02, W 2021-06-11, Z 2021-09-16 (22:00 UTC); no exit for V or Y
    assert {(r['security_id'],r['available_at']) for r in full['exits']}==A4_EXITS
    assert not {r['security_id'] for r in full['exits']}&{5,9}
    assert {r['exit_basis'] for r in full['exits']}=={'ceased_before_observed_vendor_end'}
    summary={r['recon_date']:r for r in full['recon_summary']}
    assert (summary[r2]['additions'],summary[r2]['deletions'])==(2,1)       # Z and Y added, W deleted
    assert summary[r2]['available_at']==mark(r2_rank)
    # 4. as-of replay at five cuts: every product's rows stamped before the cut are equal
    for cut in [mark(D(2021,5,10)),mark(D(2021,5,20)),mark(D(2021,6,15)),mark(D(2021,10,1)),mark(D(2022,5,20))]:
        earlier=[d for d in cal if d<cut.date()]
        current=[r for r in recons if mark(r['rank_date'])<cut]
        exits={7:mark(D(2021,6,2)),6:mark(D(2021,6,11)),8:mark(D(2021,9,16))}
        asof={sid:date if sid in exits and exits[sid]<cut else earlier[-1] for sid,date in last.items()}
        observed=_a4_products(current,cal,asof,earlier[-1])
        seen,want=_visible(observed,cut),_visible(full,cut)
        for product in ('constituents','exits','recon_summary','membership'):
            assert seen[product]==want[product],(product,cut)
        if mark(r2_rank)<cut:
            r2_seen=next(r for r in seen['recon_summary'] if r['recon_date']==r2)
            assert (r2_seen['additions'],r2_seen['deletions'])==(2,1),cut
    # 5. data-extent truncation: the rank vintages do not move when the data ends 2022-06-30 instead
    short_recons,short_last=_a4_inputs(index_id,D(2022,6,30))
    shorter=_a4_products(short_recons,cal,short_last,D(2022,6,30))
    assert full['constituents']==shorter['constituents']
    # 6. the S&P-like quarterly pair runs through the same checks (parametrized)


def test_delisting_date_triggers_an_exit_even_at_the_data_end():
    cal=_a4_calendar(D(2023,1,31))
    recons,last=_a4_inputs('R2000P')
    end=D(2022,12,30)
    # V's line ceased on the data's last session itself: only its delisting_date can say so
    states={sid:(day,None) for sid,day in last.items()}
    states[5]=(end,end)
    states[7]=(D(2021,6,1),D(2021,6,1))
    out=X.intervals(recons,cal,states,schedule=X.scheduled_reconstitutions(2020,2023),data_end=end)
    exits={(r['security_id'],r['recon_date']):r for r in out['exits']}
    assert exits[(5,D(2022,6,24))]['available_at']==mark(D(2023,1,2))
    assert exits[(5,D(2022,6,24))]['exit_basis']=='explicit_delisting_date'
    assert exits[(7,D(2021,6,25))]['exit_basis']=='explicit_delisting_date'
    assert (5,D(2021,6,25)) not in exits and (5,D(2020,6,26)) not in exits   # ceased after those scheduled ends
    assert not [r for r in out['exits'] if r['security_id']==9]               # last_session = data end, no delisting
    assert {r['effective_to'] for r in out['constituents']}=={D(2021,6,25),D(2022,6,24),D(2023,6,23)}


# ---------------------------------------------------------------- R7 (a) / (d) on a synthetic panel
RANK = D(2021, 5, 7)
PANEL_SCHEMA = pa.schema([
    ("session_date", pa.date32()), ("security_id", pa.int64()), ("cik", pa.int64()), ("link_tier", pa.string()),
    ("security_type", pa.string()), ("security_type_basis", pa.string()), ("directory_type", pa.string()),
    ("directory_etf", pa.bool_()), ("is_index", pa.bool_()), ("exchange", pa.string()), ("raw_close", pa.float64()),
    ("filer_regime", pa.string()), ("sec_last_periodic_date", pa.date32()), ("sic", pa.int32()),
    ("sale_ttm", pa.float64()), ("finra_issue_name", pa.string()), ("ni_ttm", pa.float64()), ("ni_q", pa.float64()),
    ("is_etf", pa.bool_()), ("is_adr", pa.bool_()), ("is_adr_likely", pa.bool_()), ("is_fpi", pa.bool_()),
    ("is_lp", pa.bool_()), ("is_royalty_trust", pa.bool_()), ("is_operating", pa.bool_())])
# sid: (panel cik, link_tier, security_type_basis, directory_etf, filer_regime, sec_last_periodic_date, panel flags)
R7_LINES = {
    1: (101, "strict", "finra_name", False, "domestic", D(2021, 3, 1), {}),               # strict common
    2: (102, "strict", "directory_snapshot", False, "domestic", D(2021, 3, 1), {}),       # typed by the directory only
    3: (None, None, "finra_name", True, None, None, {"is_etf": True, "is_operating": False}),  # directory ETF flag
    4: (104, "backfill", "finra_name", False, "domestic", D(2021, 3, 1), {}),             # name link, same CIK
    5: (105, "backfill", "finra_name", False, "fpi_20f", D(2021, 3, 1), {"is_fpi": True}),  # name link to CIK 101
    6: (106, "backfill", "finra_name", False, "domestic", D(2021, 3, 1), {}),             # no name link: unlinked
    7: (107, "backfill", "finra_name", False, "domestic", D(2019, 1, 1), {"is_operating": False}),  # stale filer
    8: (108, "backfill", "finra_name", False, "domestic", D(2021, 3, 1), {}),             # name CIK without facts
}


def _panel_row(d, sid, cik, tier, basis, detf, regime, periodic, flags):
    row = {"session_date": d, "security_id": sid, "cik": cik, "link_tier": tier, "security_type": "common",
           "security_type_basis": basis, "directory_type": "ETF" if detf else "common", "directory_etf": detf,
           "is_index": False, "exchange": "XNYS", "raw_close": 10.0, "filer_regime": regime,
           "sec_last_periodic_date": periodic, "sic": 3571 if cik else None, "sale_ttm": 1e9 if cik else None,
           "finra_issue_name": f"LINE {sid} INC", "ni_ttm": 5.0 if cik else None, "ni_q": 1.0 if cik else None,
           "is_etf": False, "is_adr": False, "is_adr_likely": False, "is_fpi": False, "is_lp": False,
           "is_royalty_trust": False, "is_operating": True}
    row.update(flags)
    return row


def _write(path, rows, schema=None):
    path.parent.mkdir(parents=True, exist_ok=True)
    pq.write_table(pa.Table.from_pylist(rows, schema=schema), path)


def _lake(root, days, last_day=None):
    """A small lake: vendor calendar, panel / shares / liquidity rows of R7_LINES on ``days``, line states."""
    last_day = last_day or {}
    _write(root / "calendar.parquet", [{"session_date": d} for d in days])
    by_year: dict[int, list] = {}
    for d in days:
        for sid, (cik, tier, basis, detf, regime, periodic, flags) in R7_LINES.items():
            if d <= last_day.get(sid, days[-1]):
                by_year.setdefault(d.year, []).append(_panel_row(d, sid, cik, tier, basis, detf, regime, periodic, flags))
    for y, rows in by_year.items():
        _write(root / "panel" / f"year={y}" / "panel-01.parquet", rows, PANEL_SCHEMA)
        _write(root / "market" / "shares_daily" / f"year={y}" / "shares_daily.parquet",
               [{"session_date": r["session_date"], "security_id": r["security_id"], "shrout": 1e7,
                 "shrout_source": "sec_dei" if r["link_tier"] == "backfill" else "vendor_lag"} for r in rows])
        _write(root / "market" / "liquidity" / f"year={y}" / "liquidity.parquet",
               [{"session_date": r["session_date"], "security_id": r["security_id"], "adv_63": 1e8} for r in rows])
    _write(root / "security_master" / "lines.parquet",
           [{"security_id": sid, "last_session": last_day.get(sid, days[-1]),
             "delisting_date": last_day.get(sid)} for sid in R7_LINES],
           pa.schema([("security_id", pa.int64()), ("last_session", pa.date32()), ("delisting_date", pa.date32())]))


def _pit_links(root):
    t = dt.datetime
    rows = [(1, 101, "high", t(2020, 1, 2, 22)), (4, 104, "name", t(2021, 1, 4, 22)),
            (5, 101, "name", t(2021, 1, 4, 22)), (8, 999, "name", t(2021, 1, 4, 22)),
            (6, 106, "backfill", t(2021, 1, 4, 22)),                       # a backfill tier is never a pit link
            (6, 606, "unknown", t(2021, 1, 4, 22)),                      # arbitrary tiers are not dated links
            (6, 606, "name", t(2021, 5, 8, 1))]                            # known only after the rank mark
    path = root / "identity" / "links_pit.parquet"
    _write(path, [{"security_id": s, "cik": c, "start": D(2020, 1, 2), "end_incl": D(2022, 12, 30),
                   "primary": "P", "tier": tier, "basis": "fixture", "available_at": at} for s, c, tier, at in rows],
           pa.schema([("security_id", pa.int64()), ("cik", pa.int64()), ("start", pa.date32()),
                      ("end_incl", pa.date32()), ("primary", pa.string()), ("tier", pa.string()),
                      ("basis", pa.string()), ("available_at", pa.timestamp("us"))]))
    return path


@pytest.fixture()
def lake(tmp_path, monkeypatch):
    monkeypatch.setenv("ATX_ALPHA_PANEL_ROOT", str(tmp_path))
    return tmp_path


def test_rank_snapshot_uses_dated_type_and_strict_name_links_only(lake):
    _lake(lake, [D(2021, 5, 6), RANK])
    con = C.connect(memory="200MB", threads=1)
    try:
        bare = {r["security_id"]: r for r in X.snapshot(con, RANK)}
        census = X.rank_census(con)
        linked = {r["security_id"]: r for r in X.snapshot(con, RANK, _pit_links(lake))}
        census_linked = X.rank_census(con)
    finally:
        con.close()
    # (a): the directory-typed line is out; the line the directory calls an ETF is in (no directory_* input)
    assert set(bare) == set(linked) == {1, 3, 4, 5, 6, 7, 8}
    # (d) without pit links: every backfill line is its own company with no issuer facts (stale filer 7 stays in)
    assert {s: r["cik"] for s, r in bare.items()} == {1: 101, 3: None, 4: None, 5: None, 6: None, 7: None, 8: None}
    assert bare[5]["ni_ttm"] is None and bare[4]["cik_source"] is None and bare[1]["cik_source"] == "strict"
    # (d) with pit links: 4 by its own CIK, 5 by CIK 101 with 101's facts; 8's name CIK has no row (unlinked);
    # 6's backfill-tier and late links do not count
    assert {s: r["cik"] for s, r in linked.items()} == {1: 101, 3: None, 4: 104, 5: 101, 6: None, 7: None, 8: None}
    assert linked[5]["ni_ttm"] == 5.0 and linked[5]["cik_source"] == linked[4]["cik_source"] == "pit_link"
    common = {"listed_lines": 8, "eligible_lines": 7, "v1_eligible_lines": 5, "v1_eligible_dropped_by_a": 1,
              "v1_eligible_dropped_other": 0, "eligible_not_v1": 3, "backfill_lines": 5,
              "excluded_only_is_operating": 0, "backfill_cik_would_fail_operating": 1}
    assert census == {**common, "backfill_relinked_by_name": 0, "backfill_name_link_without_facts": 0,
                      "backfill_ambiguous_link": 0, "backfill_left_unlinked": 5}
    assert census_linked == {**common, "backfill_relinked_by_name": 2, "backfill_name_link_without_facts": 1,
                             "backfill_ambiguous_link": 0, "backfill_left_unlinked": 3}


def test_rank_snapshot_ignores_every_directory_column(lake):
    _lake(lake, [RANK])
    path = lake / "panel" / "year=2021" / "panel-01.parquet"
    keep = ("security_id", "cik", "me", "ni_ttm", "ni_q", "adv_63")
    con = C.connect(memory="200MB", threads=1)
    try:
        before = [{k: r[k] for k in keep} for r in X.snapshot(con, RANK)]
        rows = pq.read_table(path).to_pylist()
        for r in rows:
            r.update(directory_etf=not r["directory_etf"], directory_type="fund")
        pq.write_table(pa.Table.from_pylist(rows, schema=PANEL_SCHEMA), path)
        assert [{k: r[k] for k in keep} for r in X.snapshot(con, RANK)] == before and len(before) == 7
    finally:
        con.close()


def _build_lake(root):
    days = E.sessions(D(2021, 1, 4), D(2022, 1, 14))
    _lake(root, days, last_day={6: D(2021, 7, 1)})
    return days


def test_build_writes_year_parts_and_a_measured_manifest(lake):
    days = _build_lake(lake)
    receipt = X.build()
    out = lake / X.STAGE
    m = json.loads((out / "manifest.json").read_text())
    assert m["schema"] == X.SCHEMA and m["status"] == "complete" and m["stage"] == "indexes_v2"
    assert m["rule"] == ["russell-proxy-v2", "sp500-proxy-v2"] and m["pit_links"] is None
    assert m["clock_rule"] == X.CLOCK_RULE and m["staleness_days"] == X.STALENESS_DAYS
    assert set(m["code"]) >= {"indexes.py", "private_index_pit.py", "exchange_calendar.py", "git_head"}
    assert set(m["input_manifests_sha256"]) == set(X.INPUTS) and m["gate_status"]["pass"] is False
    assert sorted(m["files"]) == [f"{p}/available_year={y}/{p}.parquet" for p, y in (
        ("constituents", 2021), ("exits", 2021), ("membership", 2021), ("membership", 2022), ("recon_summary", 2021))]
    for rel, f in m["files"].items():
        assert f["bytes"] == (out / rel).stat().st_size and f["sha256"] == C.sha256_file(out / rel)
    assert not (lake / "indexes").exists()                                    # v1 is never written
    # reconstitutions: Russell 2021-06-25, S&P 2021-03-19 / 06-18 / 09-17 / 12-17 (every rank date with data)
    r = m["receipt"]
    assert r["counts_on_recon"] == r["rank_vintage_rows_on_recon"] == {
        "R1000P 2021-06-25": 7, "R2000P 2021-06-25": 0, "R3000P 2021-06-25": 7, "SP500P 2021-03-19": 1,
        "SP500P 2021-06-18": 1, "SP500P 2021-09-17": 1, "SP500P 2021-12-17": 1}
    assert r["exit_rows"] == {"R1000P": 1, "R2000P": 0, "R3000P": 1, "SP500P": 0}
    assert r["directory_decided_on_rank"] == {k: 0 for k in ("2021-02-26", "2021-05-07", "2021-05-28",
                                                             "2021-08-31", "2021-11-30")}
    assert len(r["backfill_cik_on_rank"]) == 5 and all(
        v == {"selected_company_keys": 0, "excluded_only_is_operating": 0} for v in r["backfill_cik_on_rank"].values())
    e = r["eligibility_on_rank"]["2021-05-07"]
    assert (e["selected_lines"], e["selected_directory_etf_ignored"], e["selected_backfill_lines"]) == (7, 1, 5)
    assert (e["v1_eligible_dropped_by_a"], e["backfill_cik_would_fail_operating"]) == (1, 1)
    assert e["selected_backfill_shrout_from_sec"] == 5 and e["backfill_left_unlinked"] == 5
    assert r["coverage"]["vendor_data_end"] == "2022-01-14" and r["coverage"]["membership_through"] == "2022-01-18"
    assert r["coverage"]["rank_dates"]["first"] == "2021-02-26"
    assert r["focus_2020_plus"]["from"] == "2020-01-01" and receipt["exchange_calendar"]["match"] is True
    # rows: rank vintage at the rank mark; the exit of line 6 (last 2021-07-01) at 2021-07-02 22:00
    con = C.connect(memory="200MB", threads=1)
    try:
        def q(product, sql):
            return con.execute(sql.format(f"read_parquet('{(out / product).as_posix()}/available_year=*/*.parquet')")).fetchall()

        assert q("constituents", "SELECT DISTINCT recon_date, effective_from, effective_to, available_at FROM {} "
                 "WHERE index_id = 'R3000P'") == [(D(2021, 6, 25), D(2021, 6, 28), D(2022, 6, 24),
                                                   dt.datetime(2021, 5, 7, 22))]
        assert q("exits", "SELECT index_id, security_id, last_session, available_at, exit_basis FROM {} ORDER BY 1") == [
            ("R1000P", 6, D(2021, 7, 1), dt.datetime(2021, 7, 2, 22), "explicit_delisting_date"),
            ("R3000P", 6, D(2021, 7, 1), dt.datetime(2021, 7, 2, 22), "explicit_delisting_date")]
        member6 = [r[0] for r in q("membership", "SELECT session_date FROM {} WHERE security_id = 6 AND r3000 "
                                                 "ORDER BY 1")]
        assert member6 == [d for d in days if D(2021, 6, 28) <= d <= D(2021, 7, 2)]
        assert q("membership", "SELECT max(session_date), max(available_at) FROM {}") == [
            (D(2022, 1, 18), dt.datetime(2022, 1, 14, 22))]
    finally:
        con.close()
    with pytest.raises(RuntimeError, match="immutable"):
        X.build()


@pytest.mark.parametrize("product", X.PRODUCTS)
def test_build_refuses_existing_parts_without_a_manifest(lake, product):
    _build_lake(lake)
    part = lake / X.STAGE / product / "available_year=2021" / f"{product}.parquet"
    part.parent.mkdir(parents=True, exist_ok=True)
    original = b"existing protected artifact without a manifest"
    part.write_bytes(original)
    with pytest.raises(RuntimeError, match="existing.*immutable"):
        X.build()
    assert part.read_bytes() == original
    assert list((lake / X.STAGE).rglob("*.parquet")) == [part]
    assert not (lake / X.STAGE / "manifest.json").exists()


def test_build_with_pit_links_records_them_and_relinks(lake):
    _build_lake(lake)
    links = _pit_links(lake)
    X.main(["build", "--pit-links", str(links)])
    m = json.loads((lake / X.STAGE / "manifest.json").read_text())
    assert m["pit_links"] == {"path": str(links), "bytes": links.stat().st_size, "sha256": C.sha256_file(links)}
    e = m["receipt"]["eligibility_on_rank"]["2021-05-07"]
    assert (e["selected_backfill_relinked"], e["backfill_relinked_by_name"]) == (2, 2)
    assert m["receipt"]["backfill_cik_on_rank"]["2021-05-07"] == {"selected_company_keys": 0,
                                                                  "excluded_only_is_operating": 0}
    # lines 1 and 5 now form one company (CIK 101): 6 R3000 companies for 7 lines
    s = {(x["index_id"], x["recon_date"]): x for x in m["receipt"]["recon_summary"]}
    assert (s[("R3000P", "2021-06-25")]["lines"], s[("R3000P", "2021-06-25")]["companies"]) == (7, 6)


def test_build_aborts_when_the_exchange_calendar_differs_from_the_vendor(lake):
    days = _build_lake(lake)
    _write(lake / "calendar.parquet", [{"session_date": d} for d in days if d != D(2021, 6, 25)])
    with pytest.raises(ValueError, match="2021-06-25"):
        X.build()
    assert not list((lake / X.STAGE).rglob("*.parquet")) and not (lake / X.STAGE / "manifest.json").exists()


def test_measure_counts_directory_and_backfill_on_v1_selected_lines(lake, capsys):
    _lake(lake, [D(2021, 5, 6), RANK])
    _write(lake / "indexes" / "constituents.parquet",
           [{"index_id": "R3000P", "security_id": s, "cik": 100 + s, "rank_date": RANK} for s in (1, 2, 4, 6)])
    X.main(["measure", "--from-year", "2020"])
    got = json.loads(capsys.readouterr().out)
    e = got["per_rank_date"]["2021-05-07"]
    assert e["R3000P"] == {"v1_selected": 4, "type_not_finra": 1, "type_directory": 1, "directory_etf": 0,
                           "backfill_link": 2, "either": 3, "v1_companies": 4}
    assert (e["v2_eligible"]["eligible_lines"], e["v2_eligible"]["companies_me_30m"]) == (7, 7)
    assert got["pit_links"] is None and got["focus_2020_plus_russell"] == {}


def test_registry_loads_and_declares_both_stages():
    from atx_db.stagelake import registry

    stages = {s.name: s for s in registry.load()}
    v2, v1 = stages["indexes_v2"], stages["indexes"]
    assert v2.module == "atx_db.alpha_panel.indexes" and v2.schema == X.SCHEMA and v1.module is None
    assert v2.planned is True  # first real publication belongs to Task 7
    assert {o.view: (o.clock, v2.output_vintage(o)) for o in v2.outputs} == {
        "indexes_v2_constituents": ("available_at", "interval"), "indexes_v2_exits": ("available_at", "event"),
        "indexes_v2_membership": ("available_at", "daily"),
        "indexes_v2_recon_summary": ("available_at", "vintage")}
    assert all(o.glob.startswith("indexes_v2/") and "/available_year=*/" in o.glob for o in v2.outputs)
    assert {o.glob for o in v1.outputs} == {"indexes/constituents.parquet", "indexes/recon_summary.parquet",
                                            "indexes/membership/year=*/membership.parquet"}
