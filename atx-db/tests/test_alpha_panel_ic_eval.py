"""IC evaluator v2: Spearman with ties, Newey-West, neutralisations, holdout refusal and an end-to-end toy run."""

from __future__ import annotations

import datetime as dt
import json
import math
from pathlib import Path

import duckdb
import numpy as np
import pytest

from atx_db.alpha_panel import ic_eval as E


def ref_spearman(x, y) -> float:
    """Scipy-free reference: average ranks by brute-force counting, then Pearson."""
    def rk(a):
        return [sum(1.0 for v in a if v < u) + (sum(1.0 for v in a if v == u) + 1) / 2 for u in a]
    rx, ry = rk(x), rk(y)
    mx, my = sum(rx) / len(rx), sum(ry) / len(ry)
    sxy = sum((a - mx) * (b - my) for a, b in zip(rx, ry, strict=True))
    return sxy / math.sqrt(sum((a - mx) ** 2 for a in rx) * sum((b - my) ** 2 for b in ry))


def test_spearman_matches_reference_with_ties() -> None:
    x = [1, 2, 2, 3, 5, 5, 5, 9, 0, 4]
    y = [3, 3, 1, 8, 2, 7, 7, 1, 6, 0]
    assert E.spearman(np.array(x, float), np.array(y, float)) == pytest.approx(ref_spearman(x, y))
    assert list(E.rank_avg(np.array([10.0, 20.0, 20.0, 30.0]))) == [1.0, 2.5, 2.5, 4.0]


def test_nw_t_hand_computed() -> None:
    x = np.array([1, 2, 3, 4, 5, 6], float)
    # mean 3.5; gamma0 = 17.5/6; gamma1 = 8.75/6; Bartlett weight 1/2 at lag 1
    want = 3.5 / math.sqrt((17.5 / 6 + 2 * 0.5 * 8.75 / 6) / 6)
    assert E.nw_t(x, 1, min_n=3) == pytest.approx(want)
    assert math.isnan(E.nw_t(x, 1))            # fewer than 30 sessions by default
    assert E.nw_lag(1) == 5 and E.nw_lag(21) == 21


def test_industry_neutral_ic_of_pure_industry_effect_is_zero() -> None:
    rng = np.random.default_rng(3)
    grp = np.repeat(np.arange(8), 40)
    x = grp * 1.0 + rng.normal(0, 0.01, grp.size)
    y = grp * 0.5 + rng.normal(0, 0.01, grp.size)
    raw = E.spearman(x, y)
    _, ind, _ = E.session_ics(x, y, grp, None, 50)
    assert raw > 0.9 and abs(ind) < 0.2


def test_risk_adjusted_ic_removes_risk_exposure() -> None:
    rng = np.random.default_rng(5)
    n = 600
    R = rng.normal(size=(n, 3))
    x = R[:, 0] + rng.normal(0, 0.05, n)
    y = R[:, 0] + rng.normal(0, 0.5, n)
    raw, _, risk = E.session_ics(x, y, None, R, 100)
    assert raw > 0.7 and abs(risk) < 0.15


def test_session_below_min_names_is_skipped() -> None:
    x = np.arange(50, dtype=float)
    assert E.session_ics(x, x, None, None, 100) is None


def test_parse_features() -> None:
    assert E.parse_features("panel:a,b") == ("panel", ["a", "b"])
    assert E.parse_features("gold") == ("gold", None)
    with pytest.raises(ValueError):
        E.parse_features("bogus:a")


# ---------------------------------------------------------------- toy lake

N_CAL = 70
CAL = [dt.date(2019, 11, 20) + dt.timedelta(days=i) for i in range(N_CAL)]
CUT = 60
LAB_COLS = "session_date DATE, security_id BIGINT, fwd_1 DOUBLE, fwd_5 DOUBLE, fwd_21 DOUBLE, fwd_63 DOUBLE, n_5 SMALLINT, n_21 SMALLINT, n_63 SMALLINT"


def _w(con, cols: str, rows: list[tuple], dest: Path) -> None:
    dest.parent.mkdir(parents=True, exist_ok=True)
    con.execute(f"CREATE OR REPLACE TABLE t ({cols})")
    con.executemany(f"INSERT INTO t VALUES ({','.join('?' * len(cols.split(',')))})", rows)
    con.execute(f"COPY t TO '{dest.as_posix()}' (FORMAT PARQUET)")


@pytest.fixture()
def lake(tmp_path, monkeypatch):
    monkeypatch.setenv("ATX_ALPHA_PANEL_ROOT", str(tmp_path))
    monkeypatch.setattr(E, "LABEL_CUTOFF", CAL[CUT])
    monkeypatch.setattr(E, "DISCOVERY", (CAL[0], dt.date(2019, 12, 31)))
    monkeypatch.setattr(E, "TRAIN_START", dt.date(2020, 1, 2))
    monkeypatch.setattr(E, "MIN_NAMES", 10)
    monkeypatch.setattr(E, "FIRST_SESSION", CAL[0])
    monkeypatch.setattr(E, "NW_MIN_SESSIONS", 5)
    rng = np.random.default_rng(11)
    con = duckdb.connect()
    _w(con, "session_date DATE", [(d,) for d in CAL], tmp_path / "calendar.parquet")
    n = 60
    panel: dict[int, list] = {}
    gold: dict[int, list] = {}
    labels: dict[int, list] = {}
    char: dict[int, list] = {}
    for i, d in enumerate(CAL):
        for s in range(1, n + 1):
            f1 = float(rng.normal())
            f2 = float(rng.normal()) if s % 10 else None
            noise = float(rng.normal())
            panel.setdefault(d.year, []).append((d, s, s <= 50, f"g{s % 4}", 100 + s, "strict", True, True, f1, f2))
            gold.setdefault(d.year, []).append((d, s, f1 + 0.3 * float(rng.normal())))
            char.setdefault(d.year, []).append((d, s, float(rng.normal()), float(rng.normal()), float(rng.normal())))
            lab = [0.3 * f1 + noise if i + 1 + h <= CUT else None for h in (1, 5, 21, 63)]
            if d <= CAL[CUT - 2]:
                labels.setdefault(d.year, []).append((d, s, *lab, 5, 21, 63))
    for y, rows in panel.items():
        _w(con, "session_date DATE, security_id BIGINT, member_equity BOOLEAN, grp_ff49 VARCHAR, cik BIGINT, link_tier VARCHAR,"
                " is_issuer_primary BOOLEAN, is_common BOOLEAN, f1 DOUBLE, f2 DOUBLE", rows,
           tmp_path / "panel" / f"year={y}" / "panel-01.parquet")
    for y, rows in gold.items():
        _w(con, "session_date DATE, security_id BIGINT, g1 DOUBLE", rows, tmp_path / "gold" / f"year={y}" / "g.parquet")
    for y, rows in labels.items():
        _w(con, LAB_COLS, rows, tmp_path / "labels" / f"year={y}" / "labels.parquet")
    return tmp_path, char, con


def test_end_to_end_toy(lake) -> None:
    root, _char, _con = lake
    out = root / "ic.json"
    res = E.run([("panel", ["f1", "f2"]), ("gold", ["g1"])], out, overlap=("panel", ["f2"]), memory="200MB")
    assert json.loads(out.read_text())["schema"] == E.SCHEMA
    f1 = res["features"]["panel:f1"]
    m5 = f1["ic"]["h5"]["member_equity"]
    st = m5["TRAIN"]
    assert st["sessions"] > 10 and st["ic_mean"] > 0.05 and st["nw_t"] > 2
    assert st["names_mean"] == pytest.approx(50.0)
    assert "2019" in m5["years"] and "2020" in m5["years"]
    assert m5["DISCOVERY"]["sessions"] > 5
    assert st["risk_ic_mean"] is None           # no characteristics stage -> skipped
    assert any("risk" in n for n in res["notices"])
    assert f1["coverage"]["2019"] == pytest.approx(1.0)
    assert res["features"]["panel:f2"]["coverage"]["2020"] == pytest.approx(0.9, abs=0.03)
    assert f1["rank_autocorr"]["lag1"] is not None and abs(f1["rank_autocorr"]["lag1"]) < 0.3
    assert f1["overlap"]["with"] == "panel:f2" and 0 <= f1["overlap"]["max_abs_spearman"] <= 1
    assert res["features"]["gold:g1"]["redundancy"]["with"] == "panel:f1"
    assert res["features"]["gold:g1"]["redundancy"]["max_abs_spearman"] > 0.5
    assert max(f1["ic"]["h1"]["member_equity"]["years"]) == "2020"


def test_end_to_end_with_risk_inputs(lake) -> None:
    root, char, con = lake
    for y, rs in char.items():
        _w(con, "session_date DATE, security_id BIGINT, beta_252 DOUBLE, vol_63 DOUBLE, log_adv63 DOUBLE", rs,
           root / "characteristics" / f"year={y}" / "c.parquet")
    res = E.run([("panel", ["f1"])], root / "ic.json", memory="200MB")
    st = res["features"]["panel:f1"]["ic"]["h5"]["member_equity"]["TRAIN"]
    assert st["risk_ic_mean"] is not None and st["ind_ic_mean"] is not None
    assert not any("risk" in n for n in res["notices"])


def test_refuses_label_beyond_cutoff(lake) -> None:
    root, _c, con = lake
    bad = CAL[CUT - 3]  # decision CUT-3: fwd_5 would end at CUT+3
    _w(con, LAB_COLS, [(bad, 1, 0.1, 0.2, None, None, 5, 21, 63)], root / "labels" / "year=2020" / "labels.parquet")
    with pytest.raises(E.HoldoutViolation):
        E.run([("panel", ["f1"])], root / "ic.json", memory="200MB")


def test_refuses_decision_after_train_end(lake) -> None:
    root, _c, con = lake
    late = CAL[CUT - 1]
    _w(con, LAB_COLS, [(late, 1, 0.1, None, None, None, None, None, None)], root / "labels" / "year=2020" / "labels.parquet")
    with pytest.raises(E.HoldoutViolation):
        E.run([("panel", ["f1"])], root / "ic.json", memory="200MB")
