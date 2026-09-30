"""Forward-return labels: arithmetic, the 0.9*h rule, delisting truncation and the D6 holdout cutoff (toy lake)."""

from __future__ import annotations

import datetime as dt
import math
from pathlib import Path

import duckdb
import pytest

from atx_db.alpha_panel import labels as L

N_CAL = 130
CAL = [dt.date(2019, 1, 2) + dt.timedelta(days=i) for i in range(N_CAL)]
CUT = 100  # calendar index of the toy cutoff


def ret(i: int, s: int) -> float:
    return 0.001 * ((i * 7 + s * 3) % 11 - 5)


def _write(con: duckdb.DuckDBPyConnection, rows: list[tuple], cols: str, dest: Path) -> None:
    dest.parent.mkdir(parents=True, exist_ok=True)
    con.execute(f"CREATE OR REPLACE TABLE t ({cols})")
    con.executemany(f"INSERT INTO t VALUES ({','.join('?' * len(cols.split(',')))})", rows)
    con.execute(f"COPY t TO '{dest.as_posix()}' (FORMAT PARQUET)")


@pytest.fixture()
def lake(tmp_path, monkeypatch):
    monkeypatch.setenv("ATX_ALPHA_PANEL_ROOT", str(tmp_path))
    monkeypatch.setattr(L, "LABEL_START", CAL[0])
    monkeypatch.setattr(L, "LABEL_CUTOFF", CAL[CUT])
    con = duckdb.connect()
    _write(con, [(d,) for d in CAL], "session_date DATE", tmp_path / "calendar.parquet")
    rows = []
    for i, d in enumerate(CAL):
        rows.append((d, 1, ret(i, 1), False))                       # A: complete
        if i not in (10, 11, 12) and i not in range(40, 50):       # B: sparse
            rows.append((d, 2, ret(i, 2), False))
        if i <= 15:                                                 # C: delists after session 15 (dlret -0.3)
            rows.append((d, 3, ret(i, 3), False))
        if i <= 15:                                                 # D: delists after 15, dlret unknown
            rows.append((d, 4, ret(i, 4), False))
        rows.append((d, 5, ret(i, 5), i == 8))                      # E: one guarded return
    # the panel goes on past the cutoff with absurd returns that must never be read
    by_year: dict[int, list] = {}
    for r in rows:
        by_year.setdefault(r[0].year, []).append(r)
    for y, rs in by_year.items():
        _write(con, rs, "session_date DATE, security_id BIGINT, ret DOUBLE, ret_guarded BOOLEAN",
               tmp_path / "panel" / f"year={y}" / "panel-01.parquet")
    _write(con, [(3, CAL[15], False, -0.3), (4, CAL[15], False, None), (9, CAL[15], True, 0.0)],
           "security_id BIGINT, last_session DATE, continued BOOLEAN, dlret DOUBLE",
           tmp_path / "delisting" / "events.parquet")
    L.build(memory="200MB", threads=1)
    out = con.execute(f"SELECT * FROM read_parquet('{(tmp_path / 'labels' / '*' / '*.parquet').as_posix()}')").fetchall()
    cols = [d[0] for d in con.description]
    return {r[:2]: dict(zip(cols, r, strict=True)) for r in out}, tmp_path


def lr(i: int, s: int) -> float:
    return math.log1p(ret(i, s))


def test_window_sum_starts_at_d_plus_2(lake) -> None:
    got, _ = lake
    row = got[(CAL[20], 1)]
    assert row["fwd_1"] == pytest.approx(lr(22, 1))
    assert row["fwd_5"] == pytest.approx(sum(lr(k, 1) for k in range(22, 27)))
    assert row["fwd_21"] == pytest.approx(sum(lr(k, 1) for k in range(22, 43)))
    assert row["fwd_63"] == pytest.approx(sum(lr(k, 1) for k in range(22, 85)))
    assert (row["n_5"], row["n_21"], row["n_63"]) == (5, 21, 63)


def test_min_count_rule() -> None:
    assert [L.min_count(h) for h in (1, 5, 21, 63)] == [1, 5, 19, 57]


def test_ninety_percent_rule(lake) -> None:
    got, _ = lake
    # B misses sessions 10-12: decision 5 -> window 7..11 (h=5) has 3 of 5 -> NULL; h=21 window 7..27 has 18 < 19 -> NULL
    r = got[(CAL[5], 2)]
    assert r["fwd_5"] is None and r["fwd_21"] is None and r["n_21"] == 18
    # decision 6: h=21 window 8..28 has 18 < 19 -> NULL; decision 13: window 15..35 complete
    assert got[(CAL[13], 2)]["fwd_21"] is not None
    # window 40..49 missing entirely; decision 30 h=21 (32..52): 11 present < 19 -> NULL
    assert got[(CAL[30], 2)]["fwd_21"] is None
    # one gap in 21 is fine: decision 9, h=21 window 11..31 misses 11,12 -> 19 present -> OK
    assert got[(CAL[9], 2)]["n_21"] == 19 and got[(CAL[9], 2)]["fwd_21"] is not None


def test_guarded_return_is_missing(lake) -> None:
    got, _ = lake
    r = got[(CAL[5], 5)]  # h=5 window 7..11 holds the guarded session 8
    assert r["n_5"] == 4 and r["fwd_5"] is None
    assert got[(CAL[5], 5)]["fwd_21"] == pytest.approx(sum(lr(k, 5) for k in range(7, 28) if k != 8))


def test_delisting_truncation_with_dlret(lake) -> None:
    got, _ = lake
    r = got[(CAL[10], 3)]  # h=21 window 12..32; line ends at 15; dlret at the next slot
    assert r["fwd_21"] == pytest.approx(sum(lr(k, 3) for k in range(12, 16)) + math.log(0.7))
    assert r["n_21"] == 5
    assert r["fwd_63"] == pytest.approx(r["fwd_21"])


def test_delisting_unknown_dlret_counts_complete_without_term(lake) -> None:
    got, _ = lake
    r = got[(CAL[10], 4)]
    assert r["fwd_21"] == pytest.approx(sum(lr(k, 4) for k in range(12, 16)))
    assert r["n_21"] == 4


def test_delisting_window_not_truncated_when_it_ends_first(lake) -> None:
    got, _ = lake
    r = got[(CAL[5], 3)]  # h=5 window 7..11 before the delisting: ordinary
    assert r["fwd_5"] == pytest.approx(sum(lr(k, 3) for k in range(7, 12)))
    # last session (15) has no label (nothing to trade); session 14 earns only the delisting return
    assert (CAL[15], 3) not in got
    assert got[(CAL[14], 3)]["fwd_5"] == pytest.approx(math.log(0.7))


def test_cutoff_no_label_ends_after_cutoff(lake) -> None:
    got, _ = lake
    last = max(d for d, _s in got)
    assert last == CAL[CUT - 2]                       # fwd_1 ends at d+2 <= cutoff
    for (d, _s), r in got.items():
        i = CAL.index(d)
        for h in (1, 5, 21, 63):
            if r[f"fwd_{h}"] is not None:
                assert i + 1 + h <= CUT
    assert got[(CAL[CUT - 5], 1)]["fwd_5"] is None and got[(CAL[CUT - 6], 1)]["fwd_5"] is not None
    assert got[(CAL[CUT - 6], 1)]["fwd_5"] == pytest.approx(sum(lr(k, 1) for k in range(CUT - 4, CUT + 1)))


def test_panel_scan_past_cutoff_refused(lake) -> None:
    con = duckdb.connect()
    with pytest.raises(L.HoldoutViolation):
        L.scan_panel(con, [], CAL[0], CAL[CUT + 1])


def test_manifest_written(lake) -> None:
    _, root = lake
    import json
    m = json.loads((root / "labels" / "manifest.json").read_text())
    assert m["max_label_end"] <= str(CAL[CUT]) and m["status"] == "complete"
