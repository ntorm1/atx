"""Synthetic postimplementation checks for audit_fund_fields.py (no real data; run from this directory:
"C:/Program Files/Python312/python.exe" -m pytest test_audit_fund_fields.py -q).

One small synthetic world is built end to end with the real producers: CF-R + FSDS SUB sources -> the T20 events
producer (build_fundamental_events.py) -> the T21 fields producer (prepare_research_fields.py, fund + grp fields,
then me_company over a synthetic shares_out) on a synthetic TRAIN role (2021-2022), a T19-shaped bridge and a lake
line_types table. The FSDS SUB quarters 2023q1..2024q4 are overwritten with garbage after the events run, so any
audit read of a post-TRAIN quarter would fail its pin. Then:
  * the consistent world: C1 PASS (bit-exact re-join), C2 PASS for all six core items, C3 FLAG (declared FC1 share
    exceeded by the FC1 filer), C4 FLAG only for shares_out/delay 252 (the adj form and shrs_q pair are split-safe),
    C5 PASS, C6 FLAG (four issuers: every group is small), C7 PASS;
  * a look-ahead cell planted in be.f64: C1 and C2 FLAG it;
  * shares_out stepped 3% on the factor-break mass session for the repaired names: C4 continuity FLAG;
  * a VAL manifest with one changed definition: C7 FLAG; merge, run_key, seal, budget-stop and usage behaviour.
"""
from __future__ import annotations

import contextlib
import datetime as dt
import hashlib
import io
import json
import math
import shutil
import sys
from pathlib import Path

import numpy as np
import pyarrow as pa
import pyarrow.parquet as pq
import pytest

HERE = Path(__file__).resolve().parent
if str(HERE) not in sys.path:
    sys.path.insert(0, str(HERE))
import audit_fund_fields as A  # noqa: E402

bfe, prf = A.bfe, A.prf
UTC = dt.timezone.utc
DAY_NS = 86_400_000_000_000
IDS = [101, 202, 303, 404, 505, 606]
LINKS = {101: (1001, "P"), 202: (1002, "P"), 303: (1001, "J"), 404: (1003, "P"), 606: (1004, "P")}  # 505 unlinked
SIC = {1001: "3571", 1002: "2834", 1003: "6022", 1004: "3674"}
QUARTERLY = (1001, 1002, 1004)
ANNUAL = 1003
SPLIT = dt.date(2022, 6, 1)            # 1002 (line 202): 2-for-1
RESTATE = dt.date(2022, 4, 1)          # 1001 10-K/A for FY2021 (total assets x 1.05)
FC1_FILED = dt.date(2022, 3, 1)        # 1003 FY2021 10-K: accession absent from SUB -> FC1 clock
MASS = dt.date(2021, 6, 1)             # the role's factor-break-v1 mass session (synthetic)
REPAIRED = (101, 606)
SESSIONS = [d for d in (dt.date(2021, 1, 4) + dt.timedelta(days=k) for k in range(726)) if d.weekday() < 5
            and d <= dt.date(2022, 12, 30)]
SCORE_BEGIN = SESSIONS.index(dt.date(2022, 3, 7))  # after the FC1 10-K of 1003 is usable (2022-03-04)
CF_SCHEMA = pa.schema([("cik", pa.string()), ("taxonomy", pa.string()), ("concept", pa.string()), ("unit", pa.string()),
                       ("period_start", pa.date32()), ("period_end", pa.date32()), ("filed_date", pa.date32()),
                       ("fiscal_year", pa.int32()), ("fiscal_period", pa.string()), ("form", pa.string()),
                       ("accession_number", pa.string()), ("value", pa.float64())])
SUB_SCHEMA = pa.schema([("adsh", pa.string()), ("cik", pa.string()), ("sic", pa.string()), ("form", pa.string()),
                        ("period", pa.date32()), ("fy", pa.int32()), ("fp", pa.string()), ("filed", pa.date32()),
                        ("accepted_utc", pa.timestamp("us", tz="UTC")), ("fye", pa.string())])


def sha(path) -> str:
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def t_of(d: dt.date) -> int:
    return SESSIONS.index(d)


# ---------------------------------------------------------------------------------------------------------------
# Raw sources: CF-R facts and FSDS SUB rows of four issuers, FY2018..FY2022
# ---------------------------------------------------------------------------------------------------------------

def quarter(y, q):
    s = dt.date(y, 3 * q - 2, 1)
    e = (dt.date(y, 3 * q, 28) + dt.timedelta(days=4)).replace(day=1) - dt.timedelta(days=1)
    return s, e


def assets(c, y, q):
    return float((c - 1000) * 1e9 + (y - 2018) * 1e8 + q * 1e7)


def ni(c, y, q):
    return float((c - 1000) * 1e7 + (y - 2018) * 1e6 + q * 1e5)


def shares(c, end: dt.date, filed: dt.date):
    base = (c - 1000) * 1e8
    return 2 * base if c == 1002 and (end >= SPLIT or filed >= SPLIT) else base


class Filings:
    def __init__(self):
        self.cf, self.sub, self.seq = [], {}, {}

    def accession(self, c, filed):
        k = (c, filed.year)
        self.seq[k] = self.seq.get(k, 0) + 1
        return f"{c:010d}-{filed.year % 100:02d}-{self.seq[k]:06d}"

    def fact(self, c, concept, start, end, filed, form, accn, value, unit="USD"):
        self.cf.append({"cik": f"{c:010d}", "taxonomy": "us-gaap", "concept": concept, "unit": unit,
                        "period_start": start, "period_end": end, "filed_date": filed, "fiscal_year": end.year,
                        "fiscal_period": "FY", "form": form, "accession_number": accn, "value": float(value)})

    def submission(self, accn, c, form, period, filed, sic=None):
        q = f"{filed.year}q{(filed.month - 1) // 3 + 1}"
        self.sub.setdefault(q, []).append({
            "adsh": accn, "cik": f"{c:010d}", "sic": sic or SIC[c], "form": form, "period": period, "fy": period.year,
            "fp": "FY" if form.startswith("10-K") else "Q", "filed": filed,
            "accepted_utc": dt.datetime.combine(filed, dt.time(21, 30), tzinfo=UTC), "fye": "1231"})

    def ytd(self, c, concept, y, q, scale, accn, filed, form):
        s, e = dt.date(y, 1, 1), quarter(y, q)[1]
        ps, pe = dt.date(y - 1, 1, 1), quarter(y - 1, q)[1]
        self.fact(c, concept, s, e, filed, form, accn, scale * sum(ni(c, y, k) for k in range(1, q + 1)))
        self.fact(c, concept, ps, pe, filed, form, accn, scale * sum(ni(c, y - 1, k) for k in range(1, q + 1)))

    def ten_q(self, c, y, q):
        filed = quarter(y, q)[1] + dt.timedelta(days=40)
        accn, form = self.accession(c, filed), "10-Q"
        s, e = quarter(y, q)
        ps, pe = quarter(y - 1, q)
        self.fact(c, "Assets", None, e, filed, form, accn, assets(c, y, q))
        self.fact(c, "Assets", None, dt.date(y - 1, 12, 31), filed, form, accn, assets(c, y - 1, 4))
        self.fact(c, "StockholdersEquity", None, e, filed, form, accn, 0.4 * assets(c, y, q))
        self.fact(c, "NetIncomeLoss", s, e, filed, form, accn, ni(c, y, q))
        self.fact(c, "NetIncomeLoss", ps, pe, filed, form, accn, ni(c, y - 1, q))
        if q > 1:
            self.ytd(c, "NetIncomeLoss", y, q, 1.0, accn, filed, form)
        self.ytd(c, "Revenues", y, q, 10.0, accn, filed, form)
        self.ytd(c, "NetCashProvidedByUsedInOperatingActivities", y, q, 1.5, accn, filed, form)
        self.fact(c, "WeightedAverageNumberOfDilutedSharesOutstanding", s, e, filed, form, accn, shares(c, e, filed), "shares")
        self.fact(c, "WeightedAverageNumberOfDilutedSharesOutstanding", ps, pe, filed, form, accn,
                  shares(c, pe, filed), "shares")
        self.submission(accn, c, form, e, filed)

    def ten_k(self, c, y, filed, form="10-K", in_sub=True, assets_scale=1.0):
        accn = self.accession(c, filed)
        e, pe = dt.date(y, 12, 31), dt.date(y - 1, 12, 31)
        self.fact(c, "Assets", None, e, filed, form, accn, assets_scale * assets(c, y, 4))
        self.fact(c, "Assets", None, pe, filed, form, accn, assets(c, y - 1, 4))
        self.fact(c, "StockholdersEquity", None, e, filed, form, accn, 0.4 * assets(c, y, 4))
        if form == "10-K":
            for yy in (y, y - 1):
                s, ee = dt.date(yy, 1, 1), dt.date(yy, 12, 31)
                total = sum(ni(c, yy, k) for k in range(1, 5))
                self.fact(c, "NetIncomeLoss", s, ee, filed, form, accn, total)
                self.fact(c, "Revenues", s, ee, filed, form, accn, 10.0 * total)
                self.fact(c, "NetCashProvidedByUsedInOperatingActivities", s, ee, filed, form, accn, 1.5 * total)
                self.fact(c, "WeightedAverageNumberOfDilutedSharesOutstanding", s, ee, filed, form, accn,
                          shares(c, ee, filed), "shares")
        if in_sub:
            self.submission(accn, c, form, e, filed)


def write_sources(root: Path) -> dict:
    fx = Filings()
    for c in QUARTERLY:
        for y in range(2019, 2023):
            for q in (1, 2, 3):
                fx.ten_q(c, y, q)
            fx.ten_k(c, y, dt.date(y + 1, 2, 20))  # FY2022 10-K filed 2023-02-20: after the TRAIN seal
    fx.ten_k(1001, 2021, RESTATE, form="10-K/A", assets_scale=1.05)
    for y in range(2018, 2023):
        filed = dt.date(y + 1, 3, 1)
        fx.ten_k(ANNUAL, y, filed, in_sub=filed != FC1_FILED)
    cf = root / "cf"
    cf.mkdir(parents=True)
    path = cf / "batch-0000.parquet"
    pq.write_table(pa.Table.from_pylist(fx.cf, schema=CF_SCHEMA), path)
    (cf / "manifest.json").write_text(json.dumps({
        "archive": {"sha256": "ab" * 32}, "rule_version": "cf-extract-v2",
        "batches": [{"batch_id": 0, "file": path.name, "parquet_sha256": sha(path), "first_cik": 1000,
                     "last_cik": 1999, "rows": len(fx.cf)}]}))
    fsds = root / "fsds"
    (fsds / "sub").mkdir(parents=True)
    quarters = {}
    for y in range(2019, 2025):
        for qq in range(1, 5):
            q = f"{y}q{qq}"
            if q < "2019q2":
                continue
            p = fsds / "sub" / f"{q}.parquet"
            pq.write_table(pa.Table.from_pylist(fx.sub.get(q, []), schema=SUB_SCHEMA), p)
            quarters[q] = {"tables": {"sub": {"path": f"sub/{q}.parquet", "parquet_sha256": sha(p)}}}
    (fsds / "fsds-staging-manifest.json").write_text(json.dumps({"quarters": quarters}))
    ciks = root / "ciks.txt"
    ciks.write_text("".join(f"{c}\n" for c in sorted(SIC)))
    return {"cf": cf, "fsds": fsds, "ciks": ciks}


def run_events(root: Path, src: dict) -> tuple[Path, str]:
    out = root / "events"
    common = ["--out", str(out)]
    with contextlib.redirect_stdout(io.StringIO()):
        assert bfe.main(["prepare", *common, "--cik-list", str(src["ciks"]), "--companyfacts-dir", str(src["cf"]),
                         "--fsds-dir", str(src["fsds"]), "--companyfacts-manifest-sha256", sha(src["cf"] / "manifest.json"),
                         "--fsds-manifest-sha256", sha(src["fsds"] / "fsds-staging-manifest.json")]) == 0
        assert bfe.main(["events", *common, "--batches", "0"]) == 0
        assert bfe.main(["finalize", *common]) == 0
    for p in (src["fsds"] / "sub").glob("202[34]q*.parquet"):
        p.write_bytes(b"post-TRAIN SUB quarter: the audit must never open it")
    return out, sha(out / "manifest.json")


# ---------------------------------------------------------------------------------------------------------------
# Role, bridge, lake, fields
# ---------------------------------------------------------------------------------------------------------------

def role_arrays():
    nd, n = len(SESSIONS), len(IDS)
    raw = np.full((nd, n), 10.0)
    close = np.full((nd, n), 10.0)
    j = IDS.index(202)
    pre = np.array([d < SPLIT for d in SESSIONS])
    raw[pre, j] = 20.0          # 2-for-1 on SPLIT: raw halves, adjusted close continuous (factor close/raw doubles)
    close[:, j] = 10.0
    return raw, close


def write_role(root: Path, sessions=SESSIONS) -> str:
    root.mkdir(parents=True)
    nd, n = len(sessions), len(IDS)
    raw, close = role_arrays() if sessions is SESSIONS else (np.full((nd, n), 10.0), np.full((nd, n), 10.0))
    member = np.ones((nd, n), dtype="u1")
    present = np.ones((nd, n), dtype="u1")
    blobs = {"sessions.i64": np.array([(d - dt.date(1970, 1, 1)).days * DAY_NS for d in sessions], dtype="<i8").tobytes(),
             "ids.u64": np.array(IDS, dtype="<u8").tobytes(), "member.u8": member.tobytes(),
             "close.f64": close.astype("<f8").tobytes(), "raw_close.f64": raw.astype("<f8").tobytes(),
             "present.u8": present.tobytes(), "volume.f64": np.full((nd, n), 1e5).astype("<f8").tobytes()}
    files = {}
    for name, blob in blobs.items():
        (root / name).write_bytes(blob)
        files[name] = {"bytes": len(blob), "sha256": hashlib.sha256(blob).hexdigest()}
    header = ("mass_session,session,session_ns,prev_session,gap_days,column,security_id,ln_factor_step,factor_step,"
              "ln_raw_move,ln_adj_move,action\n")
    lines = [f"{MASS},{MASS},0,{MASS - dt.timedelta(days=1)},1,{IDS.index(s)},{s},-0.03,0.97,0.0,-0.03,repaired\n"
             for s in REPAIRED]
    (root / "repair_cells.csv").write_text(header + "".join(lines))
    manifest = {"schema": "atx.recent-research-role/v1", "status": "complete", "dates": nd, "instruments": n,
                "instrument_namespace": "spiderrock.securityID", "score_begin": SCORE_BEGIN if sessions is SESSIONS else 0,
                "score_end": nd, "source_sha256": "0" * 64, "files": files,
                "clock_recipe": "modeled-session+22h-mark+23h-decision-v1",
                "repair": {"cells": {"file": "repair_cells.csv", "bytes": (root / "repair_cells.csv").stat().st_size,
                                     "sha256": sha(root / "repair_cells.csv")},
                           "mass_sessions": [{"session": MASS.isoformat()}]}}
    (root / "manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
    return sha(root / "manifest.json")


def write_bridge(root: Path) -> str:
    root.mkdir(parents=True)
    long_ago = dt.datetime(2015, 1, 1, 22)
    rows = [(sid, cik, dt.date(2015, 1, 1), None, long_ago, kind, "high", "reconstructed_high")
            for sid, (cik, kind) in LINKS.items()]
    cols = list(zip(*rows))
    table = pa.table({"sr_id": pa.array(cols[0], pa.int64()), "cik": pa.array(cols[1], pa.int64()),
                      "start": pa.array(cols[2], pa.date32()), "end_incl": pa.array(cols[3], pa.date32()),
                      "available_at": pa.array(cols[4], pa.timestamp("us")), "primary": pa.array(cols[5], pa.string()),
                      "tier": pa.array(cols[6], pa.string()), "basis": pa.array(cols[7], pa.string())})
    pq.write_table(table, root / "links.parquet")
    (root / "manifest.json").write_text(json.dumps({
        "schema": "atx.identity-bridge/v1", "status": "complete", "rehearsal_identity": True,
        "source": {"scope_complete": False},
        "files": {"links.parquet": {"bytes": (root / "links.parquet").stat().st_size, "sha256": sha(root / "links.parquet"),
                                    "rows": len(rows)}}}), encoding="utf-8")
    return sha(root / "manifest.json")


def write_lake(root: Path) -> str:
    types = root / "line_types" / "year=0" / "part-0.parquet"
    types.parent.mkdir(parents=True)
    kinds = {101: "common", 202: "common", 303: "common", 404: "common_unverified", 505: "ETF", 606: "common"}
    pq.write_table(pa.table({"line_id": [f"TBLTICKERHISTORY-{s}" for s in kinds],
                             "security_type": list(kinds.values()),
                             "current_symbol": [f"S{s}" for s in kinds], "last_symbol": [f"S{s}" for s in kinds]}), types)
    entry = {"path": "line_types/year=0/part-0.parquet", "bytes": types.stat().st_size, "sha256": sha(types)}
    (root / "_manifest.json").write_text(json.dumps({"snapshot_id": "fixture", "datasets": {
        "line_types": {"files": [entry]}, "spine_monthly": {"files": []}}}), encoding="utf-8")
    return sha(root / "_manifest.json")


def shares_out_matrix():
    nd = len(SESSIONS)
    so = np.empty((nd, len(IDS)))
    for i, sid in enumerate(IDS):
        so[:, i] = {101: 1e8, 202: 1e8, 303: 5e7, 404: 3e7, 505: 1e7, 606: 1e8}[sid]
    post = np.array([d >= SPLIT for d in SESSIONS])
    so[post, IDS.index(202)] = 2e8  # restated to the session's share basis: doubles at the split
    return so


def add_entry(manifest: dict, fields_dir: Path, name: str, extra: dict, first=False):
    path = fields_dir / f"{name}.f64"
    manifest["files"][path.name] = {"bytes": path.stat().st_size, "sha256": sha(path)}
    entry = {"name": name, "file": path.name, "dtype": "<f8", "layout": "date-major",
             "shape": [len(SESSIONS), len(IDS)], "sha256": sha(path), "units": "fixture", "clock": "fixture",
             "staleness": "fixture", "source_columns": [], "caveats": [], "point_in_time": True,
             "non_pit_aspects": [], **extra}
    if first:
        manifest["fields"].insert(0, entry)
    else:
        manifest["fields"].append(entry)


def write_fields(root: Path, role: Path, role_sha: str, bridge: Path, bridge_sha: str, events: Path,
                 events_sha: str) -> tuple[Path, str]:
    out = root / "fields"
    with contextlib.redirect_stdout(io.StringIO()):
        prf.run(role, role_sha, out, list(A.FUND_NAMES) + list(A.GRP_NAMES), identity_bridge=bridge,
                identity_bridge_sha256=bridge_sha, fund_events=events, fund_events_sha256=events_sha,
                fund_lag_sessions=1)
        (out / "shares_out.f64").write_bytes(shares_out_matrix().astype("<f8").tobytes())
        _, sources, _, extras = prf.issuer_fields(["me_company"], prf.Role(role, role_sha), out, prf.Budget(700, 600),
                                                   bridge=bridge, bridge_sha256=bridge_sha, events=None,
                                                   events_sha256=None, lag=1)
    manifest = json.loads((out / "manifest.json").read_text(encoding="utf-8"))
    add_entry(manifest, out, "shares_out", {"sources": []}, first=True)
    add_entry(manifest, out, "me_company", {"sources": sources["me_company"], "depends_on": ["shares_out"],
                                            **extras["me_company"]})
    (out / "manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
    return out, sha(out / "manifest.json")


def write_val(root: Path, fields_manifest: Path, change=None) -> tuple[Path, str]:
    m = json.loads(fields_manifest.read_text(encoding="utf-8"))
    train_role = m["role"]["path"]
    val_role = "C:/val/recent-fast-validation-2023-2024-v1"
    m["role"] = dict(m["role"], path=val_role, manifest_sha256="ff" * 32, last_session="2024-12-31")
    for e in m["fields"]:
        e["coverage"] = {"scrambled": True}  # data keys are ignored by C7
        for s in e.get("sources", []):
            if A._norm_path(s["path"]).startswith(A._norm_path(train_role) + "/"):
                s["path"], s["sha256"] = val_role + "/" + Path(s["path"]).name, "ee" * 32
    if change:
        change(m)
    root.mkdir(parents=True, exist_ok=True)
    path = root / "manifest.json"
    path.write_text(json.dumps(m), encoding="utf-8")
    return path, sha(path)


@pytest.fixture(scope="module")
def world(tmp_path_factory):
    root = tmp_path_factory.mktemp("t25")
    src = write_sources(root / "raw")
    events, events_sha = run_events(root, src)
    role_sha = write_role(root / "role")
    bridge_sha = write_bridge(root / "bridge")
    lake_sha = write_lake(root / "lake")
    fields, fields_sha = write_fields(root, root / "role", role_sha, root / "bridge", bridge_sha, events, events_sha)
    val, val_sha = write_val(root / "val", fields / "manifest.json")
    return {"root": root, "src": src, "events": events, "events_sha": events_sha, "role": root / "role",
            "role_sha": role_sha, "bridge": root / "bridge", "bridge_sha": bridge_sha, "lake": root / "lake",
            "lake_sha": lake_sha, "fields": fields, "fields_sha": fields_sha, "val": val, "val_sha": val_sha}


def argv(w, out, checks, *extra, **over):
    p = dict(w, **over)
    return ["--output", str(out), "--checks", checks, "--fields", str(p["fields"]), "--fields-sha256", p["fields_sha"],
            "--role", str(p["role"]), "--role-sha256", p["role_sha"], "--identity-bridge", str(p["bridge"]),
            "--identity-bridge-sha256", p["bridge_sha"], "--fund-events", str(p["events"]),
            "--fund-events-sha256", p["events_sha"], "--val-fields-manifest", str(p["val"]),
            "--val-fields-sha256", p["val_sha"], "--lake", str(p["lake"]), "--lake-sha256", p["lake_sha"], *extra]


def run(w, out, checks, *extra, **over) -> int:
    with contextlib.redirect_stdout(io.StringIO()):
        return A.main(argv(w, out, checks, *extra, **over))


def doc(out: Path) -> dict:
    return json.loads((out / "audit.json").read_text(encoding="utf-8"))


def clone_fields(w, dst: Path, edit) -> tuple[Path, str]:
    """A copy of the fields artifact with edit(name -> ndarray dict) applied and the manifest re-pinned."""
    shutil.copytree(w["fields"], dst)
    m = json.loads((dst / "manifest.json").read_text(encoding="utf-8"))
    for name, arr in edit().items():
        path = dst / f"{name}.f64"
        path.write_bytes(arr.astype("<f8").tobytes())
        m["files"][path.name]["sha256"] = sha(path)
        next(e for e in m["fields"] if e["name"] == name)["sha256"] = sha(path)
    (dst / "manifest.json").write_text(json.dumps(m), encoding="utf-8")
    return dst, sha(dst / "manifest.json")


def field(path: Path, name: str) -> np.ndarray:
    return np.fromfile(path / f"{name}.f64", dtype="<f8").reshape(len(SESSIONS), len(IDS))


# ---------------------------------------------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------------------------------------------

def test_consistent_world_every_check(world):
    out = world["root"] / "audit-all"
    assert run(world, out, "C1,C3,C7") == 0
    assert run(world, out, "C4,C5,C6") == 0
    assert run(world, out, "C2", "--items", "be,at,shrs_q") == 0
    assert doc(out)["checks"]["C2"]["status"] == "INCOMPLETE"
    assert run(world, out, "C2", "--items", "cfo_ttm,ni_ttm,sale_ttm") == 0
    d = doc(out)
    st = {c: d["checks"][c]["status"] for c in A.CHECKS}
    assert st == {"C1": "PASS", "C2": "PASS", "C3": "FLAG", "C4": "FLAG", "C5": "PASS", "C6": "FLAG", "C7": "PASS"}, st
    assert (out / "audit.md").read_text(encoding="utf-8").count("## C") == 7

    c1 = d["checks"]["C1"]["metrics"]
    assert set(c1["fields"]) == set(A.FUND_NAMES) | set(A.GRP_NAMES) | {"me_company"}
    for name, r in c1["fields"].items():
        assert r["reproduction"]["value_mismatch"] == 0 and r["reproduction"]["nan_pattern_mismatch"] == 0, name
        assert r["reasons_consistent"], name
    be = c1["fields"]["be"]
    assert be["nan_reasons_member_cells"]["unlinked"] == len(SESSIONS)          # line 505
    assert be["nan_reasons_member_cells"]["secondary"] == len(SESSIONS)         # line 303 (J)
    assert be["coverage"]["score_window"]["common_member"]["frac"] == pytest.approx(0.8)  # 4 of 5 common lines
    assert c1["common_lines"]["common"] == 5 and c1["link_member_cells"] == c1["producer_link_member_cells"]

    c2 = d["checks"]["C2"]["metrics"]
    assert all(c2["replay_code_matches_events_pins"].values()) and c2["concept_map_matches"]
    for item in A.CORE_ITEMS:
        r = c2["items"][item]
        assert r["sampled"] == 50 and r["passed"] == 50, (item, r["failures"])
        assert r["min_row_slack_hours"] > 0 and r["min_witness_slack_hours"] > 0
        assert r["cf_full_decodes"] == 0 and r["cf_batches_read"] == 1  # the padded-CIK row-group filter held
    assert set(c2["items"]["ni_ttm"]["witness_kinds"]) <= {"fy_direct", "ytd_plus_fy_minus_prior_ytd", "four_quarters"}
    assert "ytd_plus_fy_minus_prior_ytd" in c2["items"]["sale_ttm"]["witness_kinds"]
    fc1_cells = [c for c in c2["items"]["at"]["cells"] if c["row"]["clock_basis"] == bfe.BASIS_FC1]
    assert all(c["sub"]["status"] == "fc1_ok" for c in fc1_cells)
    assert all(c["sub"]["status"] == "match" for c in c2["items"]["at"]["cells"] if c not in fc1_cells)
    concept = c2["items"]["shrs_q"]["lag4_concept_check"]
    assert concept["lag4_witnessed"] > 0 and concept["concept_mismatch"] == 0 and concept["metric_mismatch"] == 0

    c3 = d["checks"]["C3"]["metrics"]
    assert "FC1" in d["checks"]["C3"]["finding"]
    assert c3["member_cells_score_window"]["at"]["fc1_row_cells"] > 0          # the FC1 filer's cells
    assert c3["member_cells_score_window"]["at"]["in_place_revisions"] == 1     # 10-K/A of 1001 inside TRAIN
    assert c3["vintages_linked_role_window"]["at"]["vintages"]["2"] >= 1
    assert c3["period_end_monotone_violations"] == 0 and set(c3["staleness_days_values"]) <= {200, 400}
    assert all(int(y) < 2023 for y in c3["per_clock_year"])                     # 2023 rows never decoded

    c4 = d["checks"]["C4"]["metrics"]
    assert c4["pair_status"] == {"shares_out/delay(shares_out,252)": "FLAG", "adj/delay(adj,252)": "PASS",
                                 "shrs_q/shrs_q_lag4": "PASS"}
    so = c4["pairs"]["shares_out/delay(shares_out,252)"]
    assert so["share_explained"] == 1.0 and so["population_names"] == 1 and so["explained_examples"] == [202]
    assert c4["pairs"]["adj/delay(adj,252)"]["big_cells"] == 0
    assert c4["pairs"]["adj/delay(adj,252)"]["split_neutral_cells"] == so["population_cells"]
    fb = c4["factor_break_continuity"][MASS.isoformat()]
    assert fb["repaired_names_on_axis"] == 2 and fb["adj"]["ln_0.01"]["repaired_share_that_day"] == 0.0
    assert all(s["status"] in ("CIK not linked on the role", "split date outside the role") for s in c4["spot_splits"])

    c5 = d["checks"]["C5"]["metrics"]
    assert c5["me_company_sign"]["nonpositive_cells"] == 0 and c5["ratios"]["be/me_company"]["n"] > 0
    assert c5["ratios"]["cfo_ttm/at"]["tail_cells"] == 0
    c6 = d["checks"]["C6"]["metrics"]["groups"]
    assert c6["grp_ff49"]["share_small_groups"] == 1.0 and c6["grp_ff49"]["min_group_size"] == 1
    assert c6["grp_ff12"]["min_group_size"] >= 1
    assert d["checks"]["C7"]["metrics"]["fields_compared"] == len(A.FUND_NAMES) + len(A.GRP_NAMES) + 2


def test_planted_look_ahead_cell_is_flagged(world):
    t = t_of(dt.date(2022, 5, 10))           # Q1 2022 10-Q of 1001 accepted 2022-05-10 21:30 UTC: usable 2022-05-11
    i = IDS.index(101)

    def edit():
        be = field(world["fields"], "be").copy()
        assert be[t, i] == pytest.approx(0.4 * assets(1001, 2021, 4))
        be[t, i] = 0.4 * assets(1001, 2022, 1)  # the next filing's book equity, one session early
        return {"be": be}
    fields, fields_sha = clone_fields(world, world["root"] / "fields-lookahead", edit)
    out = world["root"] / "audit-lookahead"
    assert run(world, out, "C1", fields=fields, fields_sha=fields_sha) == 0
    r = doc(out)["checks"]["C1"]
    assert r["status"] == "FLAG"
    rep = r["metrics"]["fields"]["be"]["reproduction"]
    assert rep["value_mismatch"] == 1 and rep["first_mismatch"]["session"] == "2022-05-10"
    assert rep["first_mismatch"]["sid"] == 101
    assert run(world, out, "C2", "--items", "be", "--sample", "1000", fields=fields, fields_sha=fields_sha) == 2  # new run_key
    out2 = world["root"] / "audit-lookahead-c2"
    assert run(world, out2, "C2", "--items", "be", "--sample", "1000", fields=fields, fields_sha=fields_sha) == 0
    item = doc(out2)["checks"]["C2"]["metrics"]["items"]["be"]
    assert item["sampled"] == item["candidates"] and item["status"] == "FLAG"
    bad = [c for c in item["cells"] if not c["pass"]]
    assert len(bad) == 1 and bad[0]["session"] == "2022-05-10" and bad[0]["sid"] == 101
    assert bad[0]["join_equal"] is False and bad[0]["witness"]["status"] == "none"
    assert item["failures"] == {"join": 1, "witness": 1}


def test_factor_break_step_in_shares_out_is_flagged_and_spot_checks(world, monkeypatch):
    tm = t_of(MASS)

    def edit():
        so = field(world["fields"], "shares_out").copy()
        for sid in REPAIRED:
            so[tm:, IDS.index(sid)] *= 1.03  # a 3% artifact step left in shares_out on the mass session
        return {"shares_out": so}
    fields, fields_sha = clone_fields(world, world["root"] / "fields-fbstep", edit)
    monkeypatch.setattr(A, "SPOT_SPLITS", (("S202", 1002, SPLIT.isoformat(), 2.0), ("NONE", 999999, "2022-01-03", 2.0)))
    out = world["root"] / "audit-fbstep"
    assert run(world, out, "C4", fields=fields, fields_sha=fields_sha) == 0
    r = doc(out)["checks"]["C4"]
    assert r["status"] == "FLAG" and "adj jumps on 2021-06-01 (ln_0.01)" in r["finding"]
    fb = r["metrics"]["factor_break_continuity"][MASS.isoformat()]
    assert fb["adj"]["ln_0.01"]["repaired_share_that_day"] == 1.0
    assert fb["adj"]["ln1.5"]["repaired_share_that_day"] == 0.0
    spot = r["metrics"]["spot_splits"]
    assert spot[1]["status"].startswith("CIK not linked")
    line = spot[0]["lines"][0]
    assert line["sid"] == 202 and line["symbol"] == "S202" and line["detected"] == [{"session": SPLIT.isoformat(), "factor": 2.0}]
    at0 = next(x for x in line["at"] if x["offset"] == 0)
    assert at0["ln_shares_out_over_delay252"] == pytest.approx(math.log(2))
    assert at0["ln_adj_over_delay252"] == pytest.approx(0.0) and at0["ln_split_factor_252"] == pytest.approx(math.log(2))
    before = next(x for x in line["at"] if x["offset"] == -1)
    assert before["ln_shares_out_over_delay252"] == pytest.approx(0.0)


def test_val_definition_change_is_flagged(world):
    val, val_sha = write_val(world["root"] / "val-changed", world["fields"] / "manifest.json",
                             change=lambda m: next(e for e in m["fields"] if e["name"] == "be").update(units="EUR"))
    out = world["root"] / "audit-val"
    assert run(world, out, "C7", val=val, val_sha=val_sha) == 0
    r = doc(out)["checks"]["C7"]
    assert r["status"] == "FLAG" and r["metrics"]["differing_fields"] == {"be": ["units"]}


def test_refusals_seal_pins_merge_and_budget(world, monkeypatch):
    root = world["root"]
    # a role reaching 2023 is refused before anything is read
    late = SESSIONS + [dt.date(2023, 1, 3)]
    late_sha = write_role(root / "role-2023", sessions=late)
    assert run(world, root / "audit-seal", "C7", role=root / "role-2023", role_sha=late_sha) == 2
    assert not (root / "audit-seal" / "audit.json").exists()
    # a wrong pin is refused
    assert run(world, root / "audit-pin", "C7", events_sha="0" * 64) == 2
    # usage
    assert run(world, root / "audit-usage", "C9") == 2
    assert run(world, root / "audit-usage", "C2", "--items", "roe") == 2
    # merge: a re-run replaces only its check; another seed is another run_key
    out = root / "audit-merge"
    assert run(world, out, "C7") == 0
    assert run(world, out, "C5") == 0
    assert set(doc(out)["checks"]) == {"C5", "C7"}
    assert run(world, out, "C5", "--seed", "7") == 2
    # budget stop: completed checks are kept, exit 3
    real = A.Budget.check

    def stop(self, stage):
        if stage.startswith("C6"):
            raise A.BudgetStop("synthetic stop")
        return real(self, stage)
    monkeypatch.setattr(A.Budget, "check", stop)
    out = root / "audit-budget"
    assert run(world, out, "C6,C7") == 3
    assert set(doc(out)["checks"]) == set()  # C6 runs first (check order) and stops; nothing completed
    assert run(world, out, "C7") == 0 and set(doc(out)["checks"]) == {"C7"}
    assert run(world, out, "C6") == 3 and set(doc(out)["checks"]) == {"C7"}

    def stop_c2(self, stage):
        if stage == "C2 at sample":
            raise A.BudgetStop("synthetic stop")
        return real(self, stage)
    monkeypatch.setattr(A.Budget, "check", stop_c2)
    assert run(world, out, "C2", "--items", "be,at") == 3        # be completes and is saved; at stops
    c2 = doc(out)["checks"]["C2"]
    assert c2["status"] == "INCOMPLETE" and set(c2["metrics"]["items"]) == {"be"}
    monkeypatch.setattr(A.Budget, "check", real)
    assert run(world, out, "C2", "--items", "at") == 0
    assert set(doc(out)["checks"]["C2"]["metrics"]["items"]) == {"be", "at"}


def test_witness_derivations_and_earliest_public_clock():
    book = A.FactBook()
    d = lambda text: (dt.date.fromisoformat(text) - dt.date(1970, 1, 1)).days  # noqa: E731
    us = lambda text: d(text) * 86_400_000_000  # noqa: E731
    # an instant reported twice: the earliest public clock wins
    book.add("total_assets", None, d("2021-12-31"), 100.0, us("2022-03-01"), "a2", "fsds_accepted_utc", "Assets")
    book.add("total_assets", None, d("2021-12-31"), 100.0, us("2022-02-01"), "a1", "fsds_accepted_utc", "Assets")
    w = A.witness_item(book, "at", d("2021-12-31"), 100.0)
    assert w.best[0] == us("2022-02-01") and w.best[2][0][4] == "a1"
    assert A.witness_item(book, "at", d("2021-12-31"), 101.0).best is None
    # TTM from YTD + FY - prior YTD, and from four discrete quarters
    for s, e, v in (("2022-01-01", "2022-06-30", 60.0), ("2021-01-01", "2021-12-31", 100.0),
                    ("2021-01-01", "2021-06-30", 40.0)):
        book.add("revenue", d(s), d(e), v, us("2022-08-01"), "q2", "fsds_accepted_utc", "Revenues")
    w = A.witness_item(book, "sale_ttm", d("2022-06-30"), 120.0)
    assert w.best[1] == "ytd_plus_fy_minus_prior_ytd"
    for k, (s, e) in enumerate((("2021-07-01", "2021-09-30"), ("2021-10-01", "2021-12-31"), ("2022-01-01", "2022-03-31"),
                                ("2022-04-01", "2022-06-30"))):
        book.add("net_income", d(s), d(e), 1.0 + k, us("2022-08-01"), "q", "fsds_accepted_utc", "NetIncomeLoss")
    w = A.witness_item(book, "ni_ttm", d("2022-06-30"), 10.0)
    assert w.best[1] == "four_quarters"
    # book equity net of preferred stock; shares with the concept recorded
    book.add("stockholders_equity", None, d("2022-06-30"), 50.0, us("2022-08-01"), "q", "x", "StockholdersEquity")
    book.add("pref_stock", None, d("2022-06-30"), 5.0, us("2022-08-01"), "q", "x", "PreferredStockValue")
    assert A.witness_item(book, "be", d("2022-06-30"), 45.0).best[1] == "equity_minus_pref"
    book.add("shares_basic_avg", d("2022-04-01"), d("2022-06-30"), 7.0, us("2022-08-01"), "q", "x",
             "WeightedAverageNumberOfSharesIssuedBasic")
    got = A.witness_item(book, "shrs_q", d("2022-06-30"), 7.0).best
    assert got[2][0][7] == ["WeightedAverageNumberOfSharesIssuedBasic"]
