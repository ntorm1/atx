"""Synthetic fixture of the vendor-panel identity test (P9 lane A3, migration slice 4).

Writes a synthetic TickerHistory3-shaped vendor parquet and a research role projected from it, runs the EXISTING
Python builder (``prepare_research_fields.run`` with ``research_fields_price`` and ``research_fields_ohlc``) on them for
the six fields the engine's vendor-panel kinds build (ret_overnight, ret_intraday, ceq_iss_5y, open_adj, high_adj,
low_adj), and keeps its outputs. The C++ library reads the same inputs in the gtest ``ResearchFieldsVendorFixture.*``
and must reproduce every payload byte, every coverage number and the panel's read statistics.

  python make_vendor_panel_fixture.py [--out DIR]     (default: ./vendor)

Layout under DIR:
  th.parquet  the vendor file: tradingDate date32, securityID int64, open / high / low / close float32, volume and
              cumulReturnFactor float64, shares int64. Row groups: one of 2010 rows (before every axis window), the
              2014-2022 rows (ROW_GROUP_ROWS per group), one straddling the research seal (2023-12-27 .. 2024-01-03)
              and one wholly sealed (2024-02). The sealed rows are synthetic seal probes: the Python reader decodes and
              drops them; the engine never decodes the sealed group, and here not the straddling group's values
              either, since none of its pre-seal rows is selected (a straddling group that keeps a pre-seal row has
              its value chunks decoded whole, sealed rows included, and drops those unread: vendor_panel.hpp).
  role/       atx.recent-research-role/v1: sessions (the NYSE rule sessions 2020-12-01 .. 2021-01-29), ids 1001..1060,
              member, present (the vendor row's unique key and observation contract), volume.f64, close.f64 (vendor
              close x cumulReturnFactor of the same row), raw_close.f64; source_sha256 = th.parquet's SHA-256
  expected/   the Python builder's six payloads and manifest.normalized.json (paths under DIR made relative, the Python
              code identity and runtime versions dropped, as make_research_fields_fixture.py)

The values are deterministic functions of (line, date) through SHA-256 (no RNG, so no numpy version moves them),
prices in cents. Five long lines carry share history back to 2014 (daily around the five-year-back sessions and from
2020-10-15, monthly between); the others start on 2020-10-15. The rules the fixture exercises: a factor-break-v1 mass
session on 2021-01-04 (repaired, kept_split_follow, kept_distribution, kept_gap and noise steps), ceq_iss_5y's A8 lag,
a C-81 line, a null-share span, zero shares and an issuance outside the declared domain, the open-return guards,
invalid / null opens, order-violating and missing bars, non-observations, a duplicate key, an off-calendar row (a
holiday), ids off the role and a null id, a valid vendor row the role marks absent, rows after the role and the seal
probes. Only synthetic data is read.
"""
from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
from pathlib import Path
import shutil
import sys
import tempfile
from unittest import mock

import numpy as np
import pyarrow as pa
import pyarrow.parquet as pq

HERE = Path(__file__).resolve().parent
TOOLS = HERE.parents[2] / "tools"          # atx-engine/tools
sys.path.insert(0, str(HERE))
import make_research_fields_fixture as base  # noqa: E402  (normalized(), sha())

DAY_NS = 86_400_000_000_000
EPOCH = dt.date(1970, 1, 1)
FIELDS = ["ret_overnight", "ret_intraday", "ceq_iss_5y", "open_adj", "high_adj", "low_adj"]
HISTORY_FIRST = dt.date(2014, 6, 2)
DAILY_BACK = (dt.date(2015, 11, 2), dt.date(2016, 2, 29))   # around the role's five-year-back sessions
SHORT_FIRST = dt.date(2020, 10, 15)                        # every line is daily from here
ROLE_FIRST, ROLE_LAST = dt.date(2020, 12, 1), dt.date(2021, 1, 29)
MASS = dt.date(2021, 1, 4)
IDS = tuple(range(1001, 1061))
LONG = (1001, 1002, 1003, 1004, 1005)
SPLIT_FOLLOW, DISTRIBUTION, GAP, NOISE = 1050, 1051, 1052, 1053
GAP_SPAN = (dt.date(2020, 12, 18), dt.date(2021, 1, 5))   # GAP has no row in this span (crosses MASS)
DUPLICATE = (1010, dt.date(2020, 12, 1))
OFF_CALENDAR = (1011, dt.date(2020, 12, 25))               # a weekday holiday: off the rule calendar
ROLE_ABSENT = (1030, 16)                                   # (line, role row): a valid vendor row the role marks absent
OFF_ROLE_ID = 999
ROW_GROUP_ROWS = 2048
SEAL_STRADDLE = (dt.date(2023, 12, 27), dt.date(2023, 12, 28), dt.date(2024, 1, 2), dt.date(2024, 1, 3))
SEALED = tuple(dt.date(2024, 2, d) for d in range(1, 6))
COLUMNS = ("tradingDate", "securityID", "open", "high", "low", "close", "volume", "cumulReturnFactor", "shares")
SCHEMA = pa.schema([("tradingDate", pa.date32()), ("securityID", pa.int64()), ("open", pa.float32()),
                    ("high", pa.float32()), ("low", pa.float32()), ("close", pa.float32()),
                    ("volume", pa.float64()), ("cumulReturnFactor", pa.float64()), ("shares", pa.int64())])
# (line, role row) -> what is planted there (the role row's vendor row)
PLANTED = {(1020, 8): "open_jump", (1021, 12): "open_drop", (1022, 5): "open_zero", (1022, 6): "open_null",
           (1023, 9): "high_low", (1024, 10): "low_high", (1025, 11): "high_null", (1026, 7): "close_zero",
           (1027, 13): "volume_nan", (1028, 14): "factor_null", (1029, 15): "volume_negative"}


def u(*key) -> float:
    """A deterministic uniform [0, 1) of the key (SHA-256: no RNG, no numpy version)."""
    return int(hashlib.sha256(repr(key).encode()).hexdigest()[:13], 16) / float(16 ** 13)


def sessions(first: dt.date, last: dt.date) -> list:
    sys.path.insert(0, str(TOOLS))
    from research_fields_sec import nyse_sessions   # the rule calendar the price module's axis reads
    return [EPOCH + dt.timedelta(days=int(x)) for x in nyse_sessions(first, last)]


def role_days() -> list:
    return sessions(ROLE_FIRST, ROLE_LAST)


def shares_of(sid: int, d: dt.date):
    """Vendor shares (thousands; None = null) of a line on a date."""
    if sid == 1001:
        return 40_000 if d < dt.date(2017, 1, 1) else (44_000 if d < dt.date(2019, 7, 1) else 46_000)
    if sid == 1002:
        return 150_000_000 if d == dt.date(2020, 12, 15) else 30_000      # C-81: one row above the A9 ceiling
    if sid == 1003:
        return None if d < dt.date(2015, 9, 15) else 52_000                 # null shares: no share observation
    if sid == 1004:
        return 1_000 if d < dt.date(2018, 1, 1) else 300_000                # issuance outside [-ln 100, ln 100]
    if sid == 1005:
        return 0 if d < dt.date(2015, 8, 17) else 25_000                    # zero shares: no share observation
    return 10_000


def mass_step(sid: int):
    """(factor multiplier, close multiplier) of the line's first row on or after MASS."""
    if sid == SPLIT_FOLLOW:
        return 0.5, 2.0       # s < 0 and r >= max(|s| / 2, ln 1.25): kept_split_follow
    if sid == DISTRIBUTION:
        return 1.1, 1.0       # 0 < s < ln 1.25: kept_distribution (and a jump cell)
    if sid == NOISE:
        return 1.0, 1.0       # |s| <= 1e-9: not listed
    return 0.9, 1.0           # repaired (GAP: kept_gap, its step spans 20 days)


def row_days(sid: int, calendar: list) -> list:
    """The sessions a line has a vendor row on: long lines daily in DAILY_BACK and from SHORT_FIRST, else on the first
    session of each month from HISTORY_FIRST; the other lines daily from SHORT_FIRST; GAP none in GAP_SPAN."""
    out, months = [], set()
    for d in calendar:
        daily = d >= SHORT_FIRST or (sid in LONG and DAILY_BACK[0] <= d <= DAILY_BACK[1])
        monthly = sid in LONG and d >= HISTORY_FIRST and (d.year, d.month) not in months
        months.add((d.year, d.month))
        if (daily or monthly) and not (sid == GAP and GAP_SPAN[0] <= d <= GAP_SPAN[1]):
            out.append(d)
    return out


def line_rows(sid: int, calendar: list, role_row: dict) -> list:
    rows, close, factor, stepped = [], 20.0 + sid % 37, 1.0, False
    for k, d in enumerate(row_days(sid, calendar)):
        prev = close
        close = round(close * (1.0 + 0.03 * (u(sid, d, "close") - 0.5)), 2)
        if k % 63 == 62:
            factor *= 1.002                       # small distributions: steps below the jump threshold
        if d >= MASS and not stepped:
            fm, cm = mass_step(sid)
            factor, close, stepped = factor * fm, round(close * cm, 2), True
        open_ = round(prev * (1.0 + 0.02 * (u(sid, d, "open") - 0.5)), 2)
        row = {"tradingDate": d, "securityID": sid, "open": open_, "close": close,
               "high": round(max(open_, close) * (1.0 + 0.01 * u(sid, d, "high")), 2),
               "low": round(min(open_, close) * (1.0 - 0.01 * u(sid, d, "low")), 2),
               "volume": 100.0 * round(1000.0 * (1.0 + u(sid, d, "volume"))), "cumulReturnFactor": factor,
               "shares": shares_of(sid, d)}
        plant = PLANTED.get((sid, role_row.get(d)))
        if plant == "open_jump":
            row["open"] = round(20.0 * prev, 2)
            row["high"] = round(max(row["open"], close) * 1.001, 2)
        elif plant == "open_drop":
            row["open"] = round(close / 10.0, 2)
            row["low"] = round(row["open"] * 0.999, 2)
        elif plant == "open_zero":
            row["open"] = 0.0
        elif plant == "open_null":
            row["open"] = None
        elif plant == "high_low":
            row["high"] = round(0.5 * min(open_, close), 2)
        elif plant == "low_high":
            row["low"] = round(1.5 * max(open_, close), 2)
        elif plant == "high_null":
            row["high"] = None
        elif plant == "close_zero":
            row["close"] = 0.0
        elif plant == "volume_nan":
            row["volume"] = float("nan")
        elif plant == "factor_null":
            row["cumulReturnFactor"] = None
        elif plant == "volume_negative":
            row["volume"] = -1.0
        rows.append(row)
    return rows


def plain_row(sid, d, close=25.0):
    return {"tradingDate": d, "securityID": sid, "open": close, "high": close * 1.01, "low": close * 0.99,
            "close": close, "volume": 1e5, "cumulReturnFactor": 1.0, "shares": 10_000}


def vendor_rows() -> tuple:
    """(rows before every window, the 2014-2022 rows sorted by (date, id), the seal-straddling rows, sealed rows)."""
    calendar = sessions(HISTORY_FIRST, ROLE_LAST)
    role_row = {d: t for t, d in enumerate(role_days())}
    main = []
    for sid in IDS:
        main += line_rows(sid, calendar, role_row)
    dup = next(r for r in main if (r["securityID"], r["tradingDate"]) == DUPLICATE)
    main.append({**dup, "close": round(dup["close"] * 1.01, 2)})          # duplicate key: quarantined
    main.append(plain_row(OFF_CALENDAR[0], OFF_CALENDAR[1]))               # off the axis calendar
    for d in list(role_row)[:3]:
        main.append(plain_row(OFF_ROLE_ID, d))                             # not a role line
    main.append({**plain_row(1001, list(role_row)[4]), "securityID": None})  # null id
    for d in sessions(dt.date(2021, 2, 1), dt.date(2021, 2, 3)) + sessions(dt.date(2022, 6, 1), dt.date(2022, 6, 3)):
        main += [plain_row(sid, d) for sid in (1001, 1002, 1003)]          # after the role: never selected
    main.sort(key=lambda r: (r["tradingDate"], r["securityID"] or 0))
    old = [plain_row(sid, d) for d in sessions(dt.date(2010, 1, 4), dt.date(2010, 1, 8)) for sid in (1001, 1002)]
    straddle = [plain_row(sid, d) for d in SEAL_STRADDLE for sid in (1001, 1002, 1003)]
    sealed = [plain_row(sid, d) for d in SEALED for sid in LONG]
    return old, main, straddle, sealed


def table_of(rows: list) -> pa.Table:
    return pa.table({c: pa.array([r[c] for r in rows], type=SCHEMA.field(c).type) for c in COLUMNS}, schema=SCHEMA)


def write_vendor(path: Path) -> None:
    old, main, straddle, sealed = vendor_rows()
    with pq.ParquetWriter(path, SCHEMA) as w:
        for part in (old, main, straddle, sealed):
            w.write_table(table_of(part), row_group_size=ROW_GROUP_ROWS)


def write_role(root: Path, vendor: Path) -> str:
    """The role projected from the vendor file: present = a unique key with the observation contract."""
    root.mkdir(parents=True)
    days = role_days()
    t_of = {d: t for t, d in enumerate(days)}
    j_of = {sid: j for j, sid in enumerate(IDS)}
    nd, n = len(days), len(IDS)
    count = np.zeros((nd, n), dtype=np.int64)
    close = np.full((nd, n), np.nan)
    raw = np.full((nd, n), np.nan)
    volume = np.full((nd, n), np.nan)
    ok = np.zeros((nd, n), dtype=bool)
    for r in pq.read_table(vendor).to_pylist():
        t, j = t_of.get(r["tradingDate"]), j_of.get(r["securityID"])
        if t is None or j is None:
            continue
        count[t, j] += 1
        c32 = np.float32(np.nan if r["close"] is None else r["close"])
        f = np.nan if r["cumulReturnFactor"] is None else r["cumulReturnFactor"]
        v = np.nan if r["volume"] is None else r["volume"]
        ok[t, j] = bool(np.isfinite(f) and f > 0 and np.isfinite(c32) and c32 > 0 and np.isfinite(v) and v >= 0)
        raw[t, j], close[t, j], volume[t, j] = float(c32), float(c32) * f, v
    present = ok & (count == 1)
    present[ROLE_ABSENT[1], j_of[ROLE_ABSENT[0]]] = False
    close[~present], raw[~present], volume[~present] = np.nan, np.nan, np.nan
    member = np.ones((nd, n), dtype="u1")
    member[:, j_of[1041]] = 0
    member[:20, j_of[1040]] = 0
    member[(np.add.outer(np.arange(nd), np.arange(n)) % 17) == 0] = 0
    blobs = {"sessions.i64": np.array([(d - EPOCH).days * DAY_NS for d in days], dtype="<i8").tobytes(),
             "ids.u64": np.array(IDS, dtype="<u8").tobytes(), "member.u8": member.tobytes(),
             "present.u8": present.astype("u1").tobytes(), "volume.f64": volume.astype("<f8").tobytes(),
             "close.f64": close.astype("<f8").tobytes(), "raw_close.f64": raw.astype("<f8").tobytes()}
    files = {}
    for name, blob in blobs.items():
        (root / name).write_bytes(blob)
        files[name] = {"bytes": len(blob), "sha256": base.sha(blob)}
    manifest = {"schema": "atx.recent-research-role/v1", "status": "complete", "dates": nd, "instruments": n,
                "instrument_namespace": "spiderrock.securityID", "score_begin": 10, "score_end": nd,
                "source_sha256": base.sha(vendor.read_bytes()), "files": files,
                "clock_recipe": "modeled-session+22h-mark+23h-decision-v1"}
    blob = (json.dumps(manifest, indent=2, sort_keys=True) + "\n").encode()
    (root / "manifest.json").write_bytes(blob)
    return base.sha(blob)


def build(out: Path, vendor: Path | None = None) -> None:
    """Write the inputs under ``out`` (``vendor``: copy this vendor file instead of writing one) and the Python
    builder's outputs under ``out/expected``."""
    sys.path.insert(0, str(TOOLS))
    import field_registry as fr
    import prepare_research_fields as builder   # the EXISTING Python builder
    import research_fields_ohlc as ohlc
    import research_window as rw
    if rw.WINDOW_ID != rw.current()["WINDOW_ID"] or builder.SEAL != rw.current()["SEAL"]:
        raise RuntimeError(f"the fixture is generated under the repository research window, not {rw.WINDOW_ID}")
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    th = out / "th.parquet"
    if vendor is None:
        write_vendor(th)
    else:
        shutil.copyfile(vendor, th)
    role_sha = write_role(out / "role", th)
    with tempfile.TemporaryDirectory() as tmp, mock.patch.dict(builder.ALL_FIELDS), \
            mock.patch.object(builder, "FIELD_MODULES", list(builder.FIELD_MODULES)):
        fr.bind_modules(vars(builder), (ohlc,))   # the ohlc draft module, bound as its shim binds it
        work = Path(tmp) / "fields"
        manifest = builder.run(out / "role", role_sha, work, FIELDS, price_source=th)
        (out / "expected").mkdir()
        for name in FIELDS:
            shutil.copyfile(work / f"{name}.f64", out / "expected" / f"{name}.f64")
    text = json.dumps(base.normalized(manifest, str(out.resolve())), indent=2, sort_keys=True, allow_nan=False)
    (out / "expected" / "manifest.normalized.json").write_bytes((text + "\n").encode())


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    ap.add_argument("--out", type=Path, default=HERE / "vendor")
    a = ap.parse_args(argv)
    build(a.out.resolve())
    return 0


if __name__ == "__main__":
    sys.exit(main())
