"""Characteristics v2 (Task 4): registry contract, look-ahead lint, formulas against numpy references on a synthetic
lake (panel + prices_history + borrow_proxy), NULL / zero-denominator handling, the IV lag and fill_zero rules."""

from __future__ import annotations

import datetime as dt
import json
import math
from pathlib import Path

import numpy as np
import pandas as pd
import pyarrow as pa
import pyarrow.parquet as pq
import pytest

from atx_db.alpha_panel import char_registry as R
from atx_db.alpha_panel import characteristics as CH

D = dt.date
NB = 2
SPY = CH.MKT_ID
LINES = list(range(11, 23))
GRP49 = {i: ("A" if i <= 15 else "B" if i <= 19 else "C") for i in LINES}
GRP12 = {i: ("X" if i <= 17 else "Y") for i in LINES}
GAP_LINE, ZERO_LINE, GUARD_LINE, SPLIT_LINE, LIST_LINE, LATE_LINE = 11, 12, 13, 14, 15, 22
GAP = (D(2019, 3, 4), D(2019, 3, 8))
SPLIT = D(2019, 1, 15)
LIST_WINDOW = (D(2019, 5, 1), D(2019, 5, 20))

STR_COLS = {"grp_ff49", "grp_ff12"}
BOOL_COLS = {"member", "member_equity", "ret_guarded"}
DATE_COLS = {"session_date", "earn_next_expected_date"}
INT_COLS = {"security_id": pa.int64(), "earn_day_offset": pa.int32(), "inst_n_holders": pa.int64(),
            "inst_d_holders": pa.int64()}


def _type(c: str) -> pa.DataType:
    if c in STR_COLS:
        return pa.string()
    if c in BOOL_COLS:
        return pa.bool_()
    if c in DATE_COLS:
        return pa.date32()
    return INT_COLS.get(c, pa.float64())


def _write(df: pd.DataFrame, path: Path, types: dict[str, pa.DataType]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    arrays = []
    for c, t in types.items():
        vals = df[c].tolist()
        vals = [None if (v is None or (isinstance(v, float) and math.isnan(v)) or v is pd.NaT) else v for v in vals]
        if t == pa.date32():
            vals = [v.date() if isinstance(v, pd.Timestamp) else v for v in vals]
        if pa.types.is_integer(t):
            vals = [None if v is None else int(v) for v in vals]
        arrays.append(pa.array(vals, type=t))
    pq.write_table(pa.Table.from_arrays(arrays, names=list(types)), path)


def business_days(a: dt.date, b: dt.date) -> list[dt.date]:
    out, d = [], a
    while d <= b:
        if d.weekday() < 5:
            out.append(d)
        d += dt.timedelta(days=1)
    return out


def make_lake(root: Path, seed: int = 7) -> dict:
    """Synthetic lake: calendar 2015-2019, prices_history 2015-2017, panel + borrow_proxy 2018-2019."""
    rng = np.random.default_rng(seed)
    cal = business_days(D(2015, 1, 2), D(2019, 12, 31))
    sidx = {d: i + 1 for i, d in enumerate(cal)}
    pq.write_table(pa.table({"session_date": pa.array(cal, pa.date32()),
                             "vendor_rows": pa.array([100] * len(cal), pa.int64()),
                             "vendor_ids": pa.array([100] * len(cal), pa.int64())}), root / "calendar.parquet")
    n = len(cal)
    fac = rng.normal(0.0003, 0.01, n)
    rows = []
    for sid in [SPY, *LINES]:
        beta = 1.0 if sid == SPY else 0.5 + (sid % 5) * 0.3
        eps = rng.normal(0, 0.015, n) * (0 if sid == SPY else 1)
        lr = beta * fac + eps
        start = D(2017, 6, 1) if sid == 20 else (D(2018, 1, 2) if sid == 21 else cal[0])
        close = 50 * np.exp(np.cumsum(lr))
        ovn = rng.normal(0, 0.004, n)
        vol = rng.lognormal(12, 0.5, n)
        iv = np.abs(rng.normal(0.3, 0.05, (3, n)))
        iv_null = rng.random((3, n)) < 0.1
        # earnings: quarterly reaction sessions near fixed calendar dates, line offset, +-3 day jitter
        react = set()
        for yy in range(2015, 2020):
            for mm in (2, 5, 8, 11):
                t = D(yy, mm, 10) + dt.timedelta(days=int(sid % 7) + int(rng.integers(-3, 4)))
                react.add(next(j for j, c in enumerate(cal) if c >= t))
        first_react = None
        ned_now = None
        prev_close = None
        for i, d in enumerate(cal):
            if d < start or (sid == GAP_LINE and GAP[0] <= d <= GAP[1]):
                continue
            ret = None if prev_close is None else close[i] / prev_close - 1
            guarded = sid == GUARD_LINE and i % 97 == 0
            raw_close = close[i] * (2.0 if (sid == SPLIT_LINE and d < SPLIT) else 1.0)
            opn = raw_close / (1 + ovn[i]) if ret is not None else raw_close
            off = 0 if i in react else (1 if (i - 1) in react else None)
            if i - 1 in react:
                # the announcement at session i-1 becomes visible (clock < 22:00 UTC of d-1) from session i
                target = cal[i - 1] + dt.timedelta(days=364)
                ned_now = next((c for c in cal if c >= target), None) or target
                first_react = first_react or i
            shares = 0.0 if sid == ZERO_LINE else 1e8
            r = {
                "session_date": d, "security_id": sid,
                "member": sid not in (21,), "member_equity": sid not in (21, SPY),
                "grp_ff49": None if sid == SPY else GRP49[sid], "grp_ff12": None if sid == SPY else GRP12[sid],
                "open": opn, "high": max(opn, raw_close) * 1.01, "low": min(opn, raw_close) * 0.99,
                "raw_close": raw_close, "close": close[i], "ret": ret, "ret_guarded": guarded,
                "volume": 0.0 if (sid == ZERO_LINE and i % 50 == 0) else vol[i],
                "dollar_volume": 0.0 if (sid == ZERO_LINE and i % 50 == 0) else vol[i] * raw_close,
                "adv63": 0.0 if sid == ZERO_LINE else 1e7 + sid, "mkt_ret": None,
                "ret_intraday": (raw_close / opn - 1) if ret is not None else None,
                "ret_overnight": ((1 + ret) / (raw_close / opn) - 1) if (ret is not None and not guarded) else None,
                "iv_atm_21d": None if iv_null[0, i] else iv[0, i], "iv_atm_63d": None if iv_null[1, i] else iv[1, i],
                "iv_atm_252d": None if iv_null[2, i] else iv[2, i],
                "shares_out": shares, "me_company": shares * raw_close, "me_line": shares * raw_close,
                "si_shares": 1e6 * (1 + (i % 30) / 30), "si_dtc": 2.0 + sid / 10,
                "sv_short_volume": vol[i] * 0.4, "sv_total_volume": vol[i],
                "ftd_quantity": 1000.0 * sid, "inst_shares": 6e7 + sid * 1e5, "inst_n_holders": 200 + sid,
                "inst_d_holders": sid - 16, "inst_pct_change": 0.01 * (sid - 16), "inst_top10_share": 0.3 + sid / 100,
                "earn_recent": 1.0 if off is not None else 0.0, "earn_day_offset": off,
                "earn_next_expected_date": ned_now,
                "sue": 0.5, "fscore": 5.0, "txt_q": 10.0, "txt_q_lag4": 8.0, "at": 1000.0 + sid, "at_lag4": 900.0,
                "ni_q": 20.0, "be_lag1q": 400.0, "ni_q_lag4": 15.0, "be_lag1q_lag4": 380.0, "noa": 500.0,
                "shrs_q": 1e8, "shrs_q_lag4": 0.98e8, "ni_ttm": 80.0, "cfo_ttm": 100.0, "gp_ttm": 300.0,
                "oi_ttm": 120.0, "be": 420.0, "sale_ttm": 900.0 + (i // 63), "debt": 200.0, "che": 50.0,
                "capx_ttm": 40.0, "dvc_ttm": 10.0, "prstkc_ttm": 5.0, "sstk_ttm": 2.0, "xrd_ttm": 0.0,
                "sale_q": 230.0, "sale_q_lag4": 0.0 if sid == ZERO_LINE else 200.0,
                # borrow proxy
                "bp_si_to_io": 0.01 * sid, "bp_ftd_quantity": 0.0 if i % 3 else 500.0 * sid,
                "bp_on_threshold": sid == LIST_LINE and LIST_WINDOW[0] <= d <= LIST_WINDOW[1],
                "bp_run_days": (sidx[d] - sidx[LIST_WINDOW[0]] + 1) if (sid == LIST_LINE and LIST_WINDOW[0] <= d <= LIST_WINDOW[1]) else None,
            }
            rows.append(r)
            prev_close = close[i]
    df = pd.DataFrame(rows)
    df["sidx"] = df["session_date"].map(sidx)
    # equal-weight market of member lines (unguarded), from 2018-01-10 (the panel's mkt_ret starts late too)
    ok = df[df["member"] & ~df["ret_guarded"] & df["ret"].notna()]
    mk = ok.groupby("session_date")["ret"].mean()
    df["mkt_ret"] = df["session_date"].map(mk)
    df.loc[df["session_date"] < D(2018, 1, 10), "mkt_ret"] = None
    df = df.sort_values(["security_id", "session_date"]).reset_index(drop=True)
    panel_cols = list(CH.PANEL_COLUMNS)
    for y in (2018, 2019):
        part = df[pd.to_datetime(df["session_date"]).dt.year == y]
        _write(part, root / "panel" / f"year={y}" / "panel.parquet", {c: _type(c) for c in panel_cols})
        bp = part.rename(columns={"bp_si_to_io": "si_to_io", "bp_ftd_quantity": "ftd_quantity_bp",
                                  "bp_on_threshold": "on_threshold_list", "bp_run_days": "threshold_run_days"})
        bp = bp.assign(ftd_quantity=bp["ftd_quantity_bp"])
        _write(bp, root / "borrow_proxy" / f"year={y}" / "borrow_proxy.parquet",
               {"session_date": pa.date32(), "security_id": pa.int64(), "si_to_io": pa.float64(),
                "ftd_quantity": pa.float64(), "on_threshold_list": pa.bool_(), "threshold_run_days": pa.int64()})
    for y in (2015, 2016, 2017):
        part = df[pd.to_datetime(df["session_date"]).dt.year == y]
        _write(part, root / "prices_history" / f"year={y}" / "prices.parquet",
               {"session_date": pa.date32(), "security_id": pa.int64(), "close": pa.float64(), "ret": pa.float64(),
                "ret_guarded": pa.bool_()})
    for st in ("panel", "borrow_proxy", "prices_history"):
        (root / st / "manifest.json").write_text(json.dumps({"status": "complete", "stage": st}), encoding="utf-8")
    return {"cal": cal, "sidx": sidx, "df": df}


@pytest.fixture(scope="module")
def built(tmp_path_factory):
    root = tmp_path_factory.mktemp("charlake")
    with pytest.MonkeyPatch.context() as mp:
        mp.setenv("ATX_ALPHA_PANEL_ROOT", str(root))
        lake = make_lake(root)
        receipt = CH.run(["extract", "line", "year", "manifest"], [2019], nb=NB, memory="300MB", threads=1)
        out = pq.read_table(root / "characteristics" / "year=2019" / "characteristics.parquet", partitioning=None).to_pandas()
        yield {"root": root, "lake": lake, "out": out, "receipt": receipt}


def _series(built, sid: int, col: str) -> pd.Series:
    o = built["out"]
    return o[o["security_id"] == sid].set_index("session_date")[col]


def _line(built, sid: int) -> pd.DataFrame:
    df = built["lake"]["df"]
    return df[df["security_id"] == sid].reset_index(drop=True)


def _close(a, b, tol=1e-9) -> bool:
    if a is None or (isinstance(a, float) and math.isnan(a)):
        return b is None or (isinstance(b, float) and math.isnan(b))
    if b is None or (isinstance(b, float) and math.isnan(b)):
        return False
    return abs(a - b) <= tol * max(abs(a), abs(b), 1e-3)


def _check(built, sid: int, col: str, ref: dict) -> int:
    got = _series(built, sid, col)
    n = 0
    for d, v in ref.items():
        if d.year != 2019:
            continue
        assert _close(got.get(d), v), (col, sid, d, got.get(d), v)
        n += 1
    return n


def _r(line: pd.DataFrame) -> np.ndarray:
    return np.where(line["ret_guarded"], np.nan, line["ret"].astype(float))


def _full(s: np.ndarray, i: int, k: int) -> bool:
    return i >= k - 1 and s[i] - s[i - k + 1] == k - 1


def _gap(s: np.ndarray, i: int, k: int) -> bool:
    return i >= k and s[i] - s[i - k] == k


# ---------------------------------------------------------------------------------------------- registry and lint

def test_registry_contract():
    feats = R.validate()
    names = [f.name for f in feats]
    assert len(feats) >= 75 and len(names) == len(set(names))
    fam = R.family_counts()
    for f in ("momentum", "reversal", "low_risk", "liquidity", "volume", "options", "short_side", "ownership",
              "seasonality", "event_time", "control"):
        assert fam.get(f, 0) >= 1, f
    for f in feats:
        assert isinstance(f.sql_or_callable, str) and f.prior_sign in (1, -1) and f.kind in R.KINDS
        assert isinstance(f.fill_zero, bool) and isinstance(f.lookback_sessions, int) and f.inputs and f.citation
        if f.kind == "control":
            assert f.prior_sign == 1
    controls = set(R.names(kind="control"))
    assert {"log_me", "log_me_line", "beta_252", "vol_21", "vol_63", "vol_252", "ivol_21", "turnover_21",
            "hl_spread_21", "log_price", "log_adv63", "earn_days_since"} <= controls
    assert {f.name for f in feats if f.fill_zero} == {"regsho_threshold", "regsho_run_days", "ea_window_ahead_5"}
    # every v1 name is kept and importable for evaluate.py
    assert set(CH.SCORECARD) == set(R.SCORECARD_NAMES) and set(CH.EXTRA) == set(R.EXTRA_NAMES)
    from atx_db.alpha_panel import evaluate  # noqa: F401  (import line unchanged)
    man = R.as_manifest()
    assert json.loads(json.dumps(man))["n_features"] == len(feats)
    assert set(man["features"][0]) == {"name", "family", "sql_or_callable", "prior_sign", "citation", "inputs",
                                       "lookback_sessions", "kind", "fill_zero", "notes"}


def test_registry_rejects_bad_entries():
    good = R.REGISTRY[0]
    with pytest.raises(ValueError):
        R.validate([good, good])
    with pytest.raises(ValueError):
        R.validate([R.Feature("x_ctl", "control", "vol_21", -1, "c", ("panel.ret",), 21, "control")])
    with pytest.raises(ValueError):
        R.validate([R.Feature("x_lead", "momentum", "lead(r) OVER (PARTITION BY security_id ORDER BY session_date)",
                              1, "c", ("panel.ret",), 1)])
    with pytest.raises(ValueError):
        R.validate([R.Feature("x_fam", "astrology", "r", 1, "c", ("panel.ret",), 1)])


def test_lint_catches_lookahead_constructs_and_production_sql_is_clean():
    assert R.lint_sql("lead(r) OVER (PARTITION BY security_id ORDER BY session_date)") == ["lead("]
    assert "FOLLOWING" in R.lint_sql("sum(r) OVER (ORDER BY d ROWS BETWEEN CURRENT ROW AND 5 FOLLOWING)")
    assert "negative lag" in R.lint_sql("lag(close, -1) OVER (PARTITION BY security_id ORDER BY session_date)")
    assert R.lint_sql("r - avg(r) OVER (PARTITION BY security_id)")
    assert R.lint_sql("rank() OVER ()")
    assert R.lint_sql("lag(close, 21) OVER (PARTITION BY security_id ORDER BY session_date)") == []
    assert CH.lint_build(R.REGISTRY) == []
    sql = CH.line_sql("inp", "hist", "mkt", "cal.parquet")
    assert "FOLLOWING" not in sql.upper() and "LEAD(" not in sql.upper()


# ---------------------------------------------------------------------------------------------- build output

def test_output_schema_rows_and_manifest(built):
    out, root = built["out"], built["root"]
    names = [f.name for f in R.REGISTRY]
    assert list(out.columns) == ["session_date", "security_id", "member", "member_equity", "grp_ff49", "grp_ff12",
                                 *names]
    assert not out.duplicated(["session_date", "security_id"]).any()
    df = built["lake"]["df"]
    n2019 = int((pd.to_datetime(df["session_date"]).dt.year == 2019).sum())
    assert len(out) == n2019
    vals = out[names].to_numpy(dtype=float)
    assert not np.isinf(vals).any()  # non-finite -> NULL
    man = json.loads((root / "characteristics" / "manifest.json").read_text())
    assert man["status"] == "complete" and man["registry"]["n_features"] == len(names)
    assert "year=2019/characteristics.parquet" in man["files"]
    assert man["receipt"]["coverage"]["features"]["mom_12_1"]["mean"] is None  # 2020-2026 window, no rows here


def test_momentum_reversal_and_volatility_match_reference(built):
    n = 0
    for sid in (GAP_LINE, 16, GUARD_LINE):
        ln = _line(built, sid)
        s, c, r, dts = ln["sidx"].to_numpy(), ln["close"].to_numpy(float), _r(ln), list(ln["session_date"])
        ref = {k: {} for k in ("mom_12_1", "mom_6_1", "mom_12_7", "rev_5", "vol_21", "vol_63", "low_skew_63",
                               "high_52w", "frog_in_pan", "zero_return_days_63")}
        for i, d in enumerate(dts):
            ref["mom_12_1"][d] = c[i - 21] / c[i - 252] - 1 if _gap(s, i, 21) and _gap(s, i, 252) else None
            ref["mom_6_1"][d] = c[i - 21] / c[i - 126] - 1 if _gap(s, i, 21) and _gap(s, i, 126) else None
            ref["mom_12_7"][d] = c[i - 147] / c[i - 252] - 1 if _gap(s, i, 147) and _gap(s, i, 252) else None
            ref["rev_5"][d] = -(c[i] / c[i - 5] - 1) if _gap(s, i, 5) else None
            w = r[max(0, i - 20): i + 1]
            ref["vol_21"][d] = float(np.std(w, ddof=1)) if _full(s, i, 21) and np.isfinite(w).sum() == 21 else None
            w = r[max(0, i - 62): i + 1]
            ok = w[np.isfinite(w)]
            good = _full(s, i, 63) and len(ok) >= 60
            ref["vol_63"][d] = float(np.std(ok, ddof=1)) if good else None
            if good:
                m2, m3 = np.mean((ok - ok.mean()) ** 2), np.mean((ok - ok.mean()) ** 3)
                k = len(ok)
                ref["low_skew_63"][d] = -(math.sqrt(k * (k - 1)) / (k - 2) * m3 / m2 ** 1.5)
                ref["zero_return_days_63"][d] = float((ok == 0).sum()) / k
            else:
                ref["low_skew_63"][d] = ref["zero_return_days_63"][d] = None
            ref["high_52w"][d] = c[i] / c[i - 251: i + 1].max() if _full(s, i, 252) else None
            fp = r[i - 251: i - 20] if i >= 251 else np.array([])
            fpn = np.isfinite(fp).sum()
            if _full(s, i, 252) and ref["mom_12_1"][d] is not None and fpn >= 200:
                ref["frog_in_pan"][d] = -(np.sign(ref["mom_12_1"][d]) * ((fp < 0).sum() - (fp > 0).sum()) / fpn)
            else:
                ref["frog_in_pan"][d] = None
        for col, rr in ref.items():
            n += _check(built, sid, col, rr)
    assert n > 3000
    # the gap line is NULL for a year-long window after the gap and valid again later
    g = _series(built, GAP_LINE, "mom_12_1")
    assert g[D(2019, 3, 11)] is None or math.isnan(g[D(2019, 3, 11)])
    assert np.isfinite(g[D(2019, 2, 28)])


def test_risk_features_match_reference(built):
    n = 0
    for sid in (16, 18, GUARD_LINE):
        ln = _line(built, sid)
        s, r, m, dts = ln["sidx"].to_numpy(), _r(ln), ln["mkt_ret"].to_numpy(float), list(ln["session_date"])
        dref, cref = {}, {}
        for i, d in enumerate(dts):
            dref[d] = cref[d] = None
            if not _full(s, i, 252):
                continue
            rw, mw = r[i - 251: i + 1], m[i - 251: i + 1]
            neg = np.isfinite(rw) & np.isfinite(mw) & (mw < 0)
            if neg.sum() >= 50:
                x, y = mw[neg], rw[neg]
                dref[d] = -(np.mean((x - x.mean()) * (y - y.mean())) / np.mean((x - x.mean()) ** 2))
            both = np.isfinite(rw) & np.isfinite(mw)
            if both.sum() >= 200:
                x, y = mw[both], rw[both]
                xc, yc = x - x.mean(), y - y.mean()
                b = np.mean(xc * yc) / np.mean(xc ** 2)
                e = yc - b * xc
                cref[d] = -(np.mean(e * xc ** 2) / (math.sqrt(np.mean(e ** 2)) * np.mean(xc ** 2)))
        n += _check(built, sid, "downside_beta_252", dref) + _check(built, sid, "low_coskew_252", cref)
    assert n > 1000
    assert _series(built, 16, "low_coskew_252").notna().sum() > 100


def test_iv_features_use_previous_session_only(built):
    ln = _line(built, 17)
    s, dts = ln["sidx"].to_numpy(), list(ln["session_date"])
    iv21, iv252 = ln["iv_atm_21d"].to_numpy(float), ln["iv_atm_252d"].to_numpy(float)
    lvl, slope, chg5 = {}, {}, {}
    for i, d in enumerate(dts):
        w = iv21[max(0, i - 6): i]  # rows t-6..t-1: never the current row
        last = next((v for v in w[::-1] if np.isfinite(v)), None)
        lvl[d] = None if last is None else -last
        slope[d] = iv252[i - 1] - iv21[i - 1] if i >= 1 and np.isfinite(iv252[i - 1]) and np.isfinite(iv21[i - 1]) else None
        chg5[d] = (-(iv21[i - 1] - iv21[i - 6]) if _gap(s, i, 6) and np.isfinite(iv21[i - 1]) and np.isfinite(iv21[i - 6])
                   else None)
    assert _check(built, 17, "iv_level_21", lvl) > 200
    assert _check(built, 17, "iv_term_slope", slope) > 200
    assert _check(built, 17, "iv_change_5", chg5) > 200
    # changing today's IV cannot change today's value: iv_level_21 differs from -iv_atm_21d(t) on most rows
    got = _series(built, 17, "iv_level_21")
    today = ln.set_index("session_date")["iv_atm_21d"]
    same = sum(1 for d, v in got.items() if np.isfinite(v) and today.get(d) is not None and abs(v + today[d]) < 1e-15)
    assert same < 5


def test_zero_denominators_and_nulls(built):
    o = built["out"]
    z = o[o["security_id"] == ZERO_LINE]
    for col in ("ftd_to_shares", "ftd_latest_to_shares", "io_ratio", "turnover_21", "si_ratio", "log_adv63",
                "sale_growth_q", "log_me", "regsho_threshold"):
        if col == "regsho_threshold":
            assert (z[col] == 0).all()
        else:
            assert z[col].isna().all(), col
    # amihud skips zero-volume days instead of dividing by zero
    assert z["amihud_21"].notna().any() and np.isfinite(z["amihud_21"].dropna()).all()
    # a normal line has them
    n = o[o["security_id"] == 16]
    assert n[["ftd_to_shares", "io_ratio", "turnover_21", "si_ratio", "log_adv63", "sale_growth_q"]].notna().all().all()
    assert np.allclose(n["sale_growth_q"], -(230.0 / 200.0 - 1))  # prior-signed (v1 stored it raw)
    assert np.allclose(n["io_ratio"], (6e7 + 16 * 1e5) / 1e8)
    assert np.allclose(n["breadth_change"], 0.0)
    b = o[o["security_id"] == 19]
    assert np.allclose(b["breadth_change"], 3 / (219 - 3))


def test_fill_zero_and_short_side(built):
    o = built["out"]
    for col in ("regsho_threshold", "regsho_run_days", "ea_window_ahead_5"):
        assert o[col].notna().all(), col
    lst = o[o["security_id"] == LIST_LINE].set_index("session_date")
    on = [d for d in lst.index if LIST_WINDOW[0] <= d <= LIST_WINDOW[1]]
    assert (lst.loc[on, "regsho_threshold"] == -1).all() and (lst.drop(on)["regsho_threshold"] == 0).all()
    assert lst.loc[on[0], "regsho_run_days"] == pytest.approx(-math.log(2))
    assert lst.loc[on[4], "regsho_run_days"] == pytest.approx(-math.log(6))
    assert (lst.drop(on)["regsho_run_days"] == 0).all()
    assert np.allclose(o[o["security_id"] == 16]["si_to_io"], -0.16)
    ft = o[o["security_id"] == 16]["ftd_latest_to_shares"]
    assert set(np.round(ft / (-500.0 * 16 / 1e8), 9)) <= {0.0, 1.0}


def test_ea_window_ahead_matches_reference(built):
    lake = built["lake"]
    sidx = lake["sidx"]
    n_on = 0
    for sid in (16, 17, 18):
        ln = _line(built, sid)
        s, dts = ln["sidx"].to_numpy(), list(ln["session_date"])
        ned, off = list(ln["earn_next_expected_date"]), list(ln["earn_day_offset"])
        first_seen: dict = {}
        last_react = None
        ref = {}
        for i, d in enumerate(dts):
            if ned[i] is not None and not (isinstance(ned[i], float) and math.isnan(ned[i])):
                first_seen.setdefault(ned[i], s[i])
            if off[i] == 0:
                last_react = d
            cands = [e for e, f in first_seen.items() if s[i] - 300 <= f <= s[i] and e > d
                     and (last_react is None or (e - last_react).days > 30)]
            nxt = min(cands) if cands else None
            gap = (sidx[nxt] - s[i]) if (nxt is not None and nxt in sidx) else None
            ref[d] = 1.0 if (gap is not None and 2 <= gap <= 6) else 0.0
        _check(built, sid, "ea_window_ahead_5", ref)
        n_on += sum(1 for d, v in ref.items() if d.year == 2019 and v == 1.0)
    assert n_on >= 20
    # the literal column is a year ahead: it never falls in d+2..d+6
    df = lake["df"]
    gaps = (pd.to_datetime(df["earn_next_expected_date"]) - pd.to_datetime(df["session_date"])).dt.days.dropna()
    assert gaps.min() > 200


def test_long_history_seasonality_and_monthly_coskew(built):
    lake = built["lake"]
    cal, sidx = lake["cal"], lake["sidx"]
    by_s = {v: k for k, v in sidx.items()}
    df = lake["df"]
    spy = df[df["security_id"] == SPY].set_index("sidx")
    n_month = pd.Series([d.year * 12 + d.month - 1 for d in cal]).value_counts()
    last_month = max(d.year * 12 + d.month - 1 for d in cal)
    month_last = {}
    for d in cal:
        month_last[d.year * 12 + d.month - 1] = d

    def monthly(line: pd.DataFrame) -> dict:
        lr = np.log1p(_r(line))
        mi = [d.year * 12 + d.month - 1 for d in line["session_date"]]
        t = pd.DataFrame({"m": mi, "lr": lr})
        g = t.groupby("m")["lr"].agg(["sum", "count"])
        return {m: math.exp(v["sum"]) - 1 for m, v in g.iterrows() if v["count"] == n_month[m]}

    mk = monthly(spy.reset_index())
    for sid in (16, 18, 20):
        ln = _line(built, sid)
        lrs = dict(zip(ln["sidx"], np.log1p(_r(ln)), strict=True))
        mo = monthly(ln)
        seas, cosk = {}, {}
        for d, si in zip(ln["session_date"], ln["sidx"], strict=True):
            vals = []
            for k in (2, 3, 4, 5):
                w = [lrs.get(j) for j in range(si - (252 * k - 8), si - (252 * k - 28) + 1)]
                w = [v for v in w if v is not None and np.isfinite(v)]
                if len(w) == 21:
                    vals.append(math.exp(sum(w)) - 1)
            seas[d] = float(np.mean(vals)) if vals else None
            mi = d.year * 12 + d.month - 1
            ml = mi if (month_last[mi] == d and mi < last_month) else mi - 1
            pairs = [(mk[j], mo[j]) for j in range(ml - 59, ml + 1) if j in mk and j in mo]
            if len(pairs) >= 48:
                x, y = np.array([p[0] for p in pairs]), np.array([p[1] for p in pairs])
                xc, yc = x - x.mean(), y - y.mean()
                b = np.mean(xc * yc) / np.mean(xc ** 2)
                e = yc - b * xc
                cosk[d] = -(np.mean(e * xc ** 2) / (math.sqrt(np.mean(e ** 2)) * np.mean(xc ** 2)))
            else:
                cosk[d] = None
        _check(built, sid, "seas_annual_avg_2_5", seas)
        _check(built, sid, "low_coskew_60m", cosk)
    assert _series(built, 16, "seas_annual_avg_2_5").notna().sum() > 200
    assert _series(built, 16, "low_coskew_60m").notna().sum() > 100
    assert _series(built, 20, "low_coskew_60m").notna().sum() == 0  # starts 2017-06: < 48 months in 2019
    del by_s


def test_group_statistics_same_session(built):
    o = built["out"]
    day = o[o["session_date"] == D(2019, 8, 15)].set_index("security_id")
    # members of FF49 group A: 11..15 (member lines with mom_12_1)
    moms = {sid: day.loc[sid, "mom_12_1"] for sid in range(11, 16) if np.isfinite(day.loc[sid, "mom_12_1"])}
    assert len(moms) >= 3
    ref = float(np.mean(list(moms.values())))
    for sid in range(11, 16):
        assert day.loc[sid, "ind_mom_12_1"] == pytest.approx(ref, rel=1e-12)
        if sid in moms:
            assert day.loc[sid, "within_ind_mom"] == pytest.approx(moms[sid] - ref, rel=1e-9, abs=1e-15)
    # group C has 3 lines but line 21 is not a member -> fewer than 3 -> NULL
    assert np.isnan(day.loc[20, "ind_mom_12_1"]) and np.isnan(day.loc[22, "ind_mom_12_1"])
    sec = [day.loc[s, "mom_12_1"] for s in LINES if GRP12[s] == "X" and s != 21 and np.isfinite(day.loc[s, "mom_12_1"])]
    assert day.loc[11, "sector_mom_ff12_12_1"] == pytest.approx(float(np.mean(sec)), rel=1e-12)
