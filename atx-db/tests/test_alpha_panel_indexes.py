"""S3.6 index proxies: Russell rank / band rule, S&P-like buffer rule, schedules and membership intervals."""

from __future__ import annotations

import datetime as dt

import pytest

from atx_db.alpha_panel import indexes as X

D = dt.date


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


def test_intervals_end_at_next_recon_or_last_session_and_turnover():
    cal = [D(2020, 6, 26), D(2020, 6, 29), D(2020, 6, 30), D(2021, 6, 25), D(2021, 6, 28)]
    recons = [{"index_id": "R2000P", "rank_date": D(2020, 5, 8), "recon_date": D(2020, 6, 26),
               "members": {1: (10, 50.0, 1001, "rank"), 2: (20, 50.0, 1500, "rank")}},
              {"index_id": "R2000P", "rank_date": D(2021, 5, 7), "recon_date": D(2021, 6, 25),
               "members": {1: (10, 60.0, 1001, "rank"), 3: (30, 40.0, 1200, "rank")}}]
    rows, summary = X.intervals(recons, cal, {2: D(2020, 6, 30)})
    r = {(x["security_id"], x["recon_date"]): x for x in rows}
    assert r[(1, D(2020, 6, 26))]["effective_from"] == D(2020, 6, 29)
    assert r[(1, D(2020, 6, 26))]["effective_to"] == D(2021, 6, 25)
    assert r[(2, D(2020, 6, 26))]["effective_to"] == D(2020, 6, 30)       # delisted: leaves on its last session
    assert r[(3, D(2021, 6, 25))]["effective_to"] is None and r[(3, D(2021, 6, 25))]["weight"] == pytest.approx(0.4)
    s = summary[1]
    assert (s["additions"], s["deletions"]) == (1, 0)                      # line 2 was gone before the recon
    assert s["turnover_oneway"] == pytest.approx(0.5) and s["turnover_cap"] == pytest.approx(0.4)
    assert r[(1, D(2020, 6, 26))]["available_at"] == dt.datetime(2020, 5, 8, 22)
