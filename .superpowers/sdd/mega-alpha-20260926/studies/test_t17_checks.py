"""Synthetic smoke test for t17_checks.py (no real data; run: python -m pytest test_t17_checks.py -q).

One small synthetic world (FINRA raw/as-of/schedule, two roles, TRAIN fields manifest, weights + fitter records,
two NAV runs, a TickerHistory3-shaped parquet) built so every check should CLEAR; then C1 is re-run on a variant
where the republished era carries revised values (must CONFIRM) and the merge must keep the other checks.
"""
from __future__ import annotations

import hashlib
import json
import sys
import tempfile
import unittest
from pathlib import Path

import numpy as np
import pandas as pd
from pandas.tseries.holiday import USFederalHolidayCalendar

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import t17_checks as T  # noqa: E402

SC = T.SCENARIO
HOL = USFederalHolidayCalendar().holidays("2017-01-01", "2026-12-31")
BDAY = pd.offsets.CustomBusinessDay(holidays=HOL)


def sha(b: bytes) -> str:
    return hashlib.sha256(b).hexdigest()


def settlements(start="2019-11-01", end="2023-06-30"):
    out = []
    for m in pd.period_range(start, end, freq="M"):
        for d in (pd.Timestamp(m.year, m.month, 15), pd.Timestamp(m.year, m.month, m.days_in_month)):
            while d.weekday() >= 5:
                d -= pd.Timedelta(days=1)
            out.append(d)
    return out


def write_finra(root: Path, baked_pre: bool, rng: np.random.Generator):
    raw = root / "raw"
    raw.mkdir(parents=True)
    (root / "asof").mkdir()
    syms = [f"S{k:03d}" for k in range(60)]
    classes = ["NYSE"] * 50 + ["OTC"] * 10
    sched, asof = [], []
    prev = {s: int(rng.integers(100_000, 5_000_000)) for s in syms}
    for d in settlements():
        diss = d + 7 * BDAY
        cur = {s: int(rng.integers(100_000, 5_000_000)) for s in syms}
        lines = ["accountingYearMonthNumber|symbolCode|issueName|issuerServicesGroupExchangeCode|marketClassCode|"
                 "currentShortPositionQuantity|previousShortPositionQuantity|stockSplitFlag|averageDailyVolumeQuantity|"
                 "daysToCoverQuantity|revisionFlag|changePercent|changePreviousNumber|settlementDate"]
        pre_era = d < pd.Timestamp("2021-06-01")
        for k, s in enumerate(syms):
            revised = k % 10 == 3  # 10% of rows revise the prior cycle
            previous = prev[s] + (0 if (baked_pre and pre_era) else 1234) if revised else prev[s]
            flag = "R" if revised else ""
            lines.append(f"{d:%Y%m%d}|{s}|Issue \"{s}|X|{classes[k]}|{cur[s]}|{previous}||100000|2.5|{flag}|0|0|{d:%Y-%m-%d}")
            if classes[k] != "OTC":
                asof.append((1000 + k, f"{diss:%Y-%m-%d}", cur[s]))
        (raw / f"si_{d:%Y%m%d}.csv").write_text("\n".join(lines) + "\n", encoding="utf-8")
        label = "Exchange Receipt Date" if d < pd.Timestamp("2021-01-01") else "Publication Date"
        sched.append(f"{d:%Y-%m-%d},{d + 2 * BDAY:%Y-%m-%d},{diss:%Y-%m-%d},official,{label},1,http://x")
        prev = cur
    (root / "dissemination_schedule.csv").write_text(
        "settlement_date,due_date,dissemination_date,source,third_column_label,in_download,source_url\n"
        + "\n".join(sched) + "\n")
    asof.sort()
    (root / "asof" / "si_shares.csv").write_text(
        "security_id,available_at,value\n" + "\n".join(f"{a},{b},{c}" for a, b, c in asof) + "\n")
    return sha((root / "asof" / "si_shares.csv").read_bytes())


def write_role(d: Path, start: str, score_start: str, end: str, n: int, id0: int, vanish_years, rng):
    d.mkdir(parents=True)
    days = pd.bdate_range(start, end)
    D = len(days)
    begin = int(np.searchsorted(days.values, np.datetime64(score_start)))
    present = np.ones((D, n), dtype=bool)
    k = 0
    for y in vanish_years:  # two names per year vanish mid-year
        for _ in range(2):
            stop = int(np.searchsorted(days.values, np.datetime64(f"{y}-06-15"))) + k
            present[stop:, k] = False
            k += 1
    member = present & (np.arange(n) < n - 5)[None, :]
    member[:63] = False
    ret = rng.normal(0, 0.01, (D, 1)) + rng.normal(0, 0.01, (D, n))  # common market + idiosyncratic
    close = 50 * np.exp(np.cumsum(ret, 0))
    close[~present] = np.nan
    files = {}
    arrays = {"sessions.i64": (days.values.astype("datetime64[ns]").astype("<i8")),
              "ids.u64": (np.arange(n) + id0).astype("<u8"), "present.u8": present.astype("u1"),
              "member.u8": member.astype("u1"), "close.f64": close.astype("<f8"), "raw_close.f64": close.astype("<f8")}
    for name, arr in arrays.items():
        b = np.ascontiguousarray(arr).tobytes()
        (d / name).write_bytes(b)
        files[name] = {"bytes": len(b), "sha256": sha(b)}
    m = {"schema": "atx.recent-research-role/v1", "status": "complete", "dates": D, "instruments": n,
         "score_begin": begin, "score_end": D,
         "score_end_ns": int((days[-1] + pd.Timedelta(days=1)).value), "files": files}
    raw = json.dumps(m).encode()
    (d / "manifest.json").write_bytes(raw)
    return sha(raw), days, begin, D


def build_world(tmp: Path, baked_pre=False):
    rng = np.random.default_rng(5)
    finra = tmp / "finra"
    asof_sha = write_finra(finra, baked_pre, rng)
    be = tmp / "root" / "build-equity"
    tsha, tdays, tb, tD = write_role(be / "train", "2018-06-01", "2020-01-01", "2022-12-30", 60, 100, (2020, 2021, 2022), rng)
    vsha, _, _, _ = write_role(be / "val", "2021-06-01", "2023-01-01", "2024-12-31", 60, 130, (2023, 2024), rng)
    fields = be / "fields.json"
    fields.write_text(json.dumps({"role": {"manifest_sha256": tsha}, "fields": [
        {"name": "si_shares", "coverage": {"vintage_risk": {"first_session_vintage_safe": "2021-07-26"}},
         "sources": [{"path": str(finra / "asof" / "si_shares.csv"), "sha256": asof_sha}]}]}))
    wd = be / "weights"
    wd.mkdir()
    cands, work = [], be / "work" / tsha / "tag"
    (work / "context").mkdir(parents=True)
    (work / "factors").mkdir()
    b, e = tb, tD - 2
    (work / "context" / "context.json").write_text(json.dumps({"decision_begin": b, "decision_end_exclusive": e}))
    for i, cid in enumerate(T.C2_SI + T.C2_CONTROLS):
        payload = sha(cid.encode())
        f = np.where(np.arange(e - b) % 2 == 0, 0.006, -0.004)  # identical moments in both windows
        (work / "factors" / f"{payload}.json").write_text(json.dumps(
            {"candidate_id": cid, "role_manifest_sha256": tsha, "f_unsigned": [float(x) for x in f]}))
        cands.append({"id": cid, "sign": -1 if i == 0 else 1, "weight": 0.1, "cache_payload_sha256": payload})
    wraw = json.dumps({"train_manifest_sha256": tsha, "provenance": {"candidates": cands}}).encode()
    (wd / "composition_weights.json").write_bytes(wraw)
    for name, start, end in (("nav_train", "2020-01-02", "2022-12-30"), ("nav_val", "2023-01-03", "2024-12-31")):
        nd = be / name
        nd.mkdir()
        days = pd.bdate_range(start, end)
        pd.DataFrame({"session_ns": days.values.astype("datetime64[ns]").astype("int64"),
                      "pretrade_gross_dollars": 7.7e8, "held_names": 2900, "guarded_intervals": rng.poisson(0.2, len(days)),
                      "writeoff_return": 0.0}).to_csv(nd / f"daily_{SC}.csv", index=False)
        ev = pd.DataFrame({"kind": ["write-off", "guarded"] * 4, "session_ns": days.values[::len(days) // 8][:8].astype("int64"),
                           "instrument_id": 1, "side": "long", "run_length": 5, "exposure_dollars": 2e5,
                           "r_adj": 0.0, "r_raw": 0.0, "haircut": 0.0, "pnl_dollars": 0.0})
        ev.to_csv(nd / f"events_{SC}.csv", index=False)
    # TickerHistory3-shaped parquet: H crushes on the flag-0 session; I (clean) flat
    ids = np.r_[np.arange(100, 160), np.arange(130, 190)]
    ids = np.unique(ids)
    days = pd.bdate_range("2019-12-02", "2024-12-31")
    rows = []
    for sid in ids:
        flags = np.full(len(days), "N", dtype=object)
        ev_idx = np.arange(30 + int(sid) % 20, len(days), 63)
        iv_i = np.full(len(days), 0.30)
        iv_h = iv_i.copy()
        for j in ev_idx:
            flags[j] = "0"
            if j + 1 < len(days):
                flags[j + 1] = "1"
            iv_h[max(j - 21, 0):j] = 0.375  # earnings premium before the event, crushed on the reaction session
        rows.append(pd.DataFrame({"tradingDate": days.date, "securityID": np.int64(sid), "earnFlag": flags,
                                  "atmCenI_21d": iv_i.astype("float32"), "atmCenH_21d": iv_h.astype("float32")}))
    th = pd.concat(rows, ignore_index=True)
    import pyarrow as pa
    import pyarrow.parquet as pq
    tbl = pa.Table.from_pandas(th, preserve_index=False).cast(pa.schema([
        ("tradingDate", pa.date32()), ("securityID", pa.int64()), ("earnFlag", pa.string()),
        ("atmCenI_21d", pa.float32()), ("atmCenH_21d", pa.float32())]))
    pq.write_table(tbl, tmp / "th.parquet", row_group_size=50_000)
    args = ["--root", str(tmp / "root"), "--finra", str(finra), "--tickerhistory", str(tmp / "th.parquet"),
            "--train-role", str(be / "train"), "--val-role", str(be / "val"), "--train-fields", str(fields),
            "--weights-dir", str(wd), "--work-dir", str(be / "work"), "--train-nav", str(be / "nav_train"),
            "--val-nav", str(be / "nav_val"), "--train-role-sha", tsha, "--val-role-sha", vsha,
            "--weights-sha", sha(wraw), "--budget-seconds", "120"]
    return args


class T17ChecksSmoke(unittest.TestCase):
    def test_all_clear_then_c1_confirmed_merge(self):
        with tempfile.TemporaryDirectory() as td:
            tmp = Path(td)
            args = build_world(tmp / "w1")
            out = tmp / "out"
            rc = T.main(args + ["--output", str(out), "--checks", ",".join(T.CHECKS)])
            doc = json.loads((out / "t17_checks.json").read_text())
            status = {c: doc["checks"][c]["status"] for c in T.CHECKS}
            self.assertEqual(rc, 0, status)
            self.assertEqual(status, {c: "CLEARED" for c in T.CHECKS},
                             {c: doc["checks"][c]["finding"] for c in T.CHECKS})
            md = (out / "t17_checks.md").read_text(encoding="utf-8")
            for c in T.CHECKS:
                self.assertIn(f"**{c} —", md)
            T._VALUE_SETS.clear()
            args2 = build_world(tmp / "w2", baked_pre=True)
            rc = T.main(args2 + ["--output", str(out), "--checks", "C1"])
            self.assertEqual(rc, 0)
            doc = json.loads((out / "t17_checks.json").read_text())
            self.assertEqual(doc["checks"]["C1"]["status"], "CONFIRMED", doc["checks"]["C1"]["finding"])
            self.assertEqual(doc["checks"]["C8"]["status"], "CLEARED")  # merged, not clobbered

    def test_bad_check_name(self):
        with tempfile.TemporaryDirectory() as td:
            self.assertEqual(T.main(["--output", td, "--checks", "C9"]), 2)


if __name__ == "__main__":
    unittest.main()
