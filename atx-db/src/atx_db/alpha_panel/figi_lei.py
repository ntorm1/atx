"""S2.4 / S2.6: FIGI (OpenFIGI mapping API) and LEI (GLEIF golden copy, ISIN-LEI map, Level 2 parents).

Outputs (stage ``security_master``, new files):

* ``figi.parquet``: one row per OpenFIGI result (query grain): ``query_kind`` (``cusip`` = ID_CUSIP with
  exchCode US; ``cusip_any`` = ID_CUSIP without an exchange filter, for CUSIPs with no US composite, e.g.
  delisted lines; ``isin_fragment`` = ID_ISIN candidates rebuilt from a 13F CUSIP field holding characters 3-11
  of a non-US ISIN; ``ticker`` = TICKER with exchCode US for member lines without a CUSIP), ``query_value``,
  ``figi, composite_figi, share_class_figi, ticker, exch_code, name, security_type, security_type2,
  market_sector, security_description, is_us_composite``, the line it maps to (``security_id``: the vendor line
  carrying the US composite ticker on the last vendor session, rule ``ftd-ticker-asof-settlement-v1`` forms) and
  ``fetched_at``. Not point in time (``vintage_risk = 'snapshot_non_pit'``): OpenFIGI returns today's ticker.
* ``line_figi.parquet``: one row per vendor line: composite / share-class FIGI through the line's CUSIPs
  (``cusip_history``, latest run first) or its current ticker, with ``figi_basis``.
* ``lei.parquet``: CIK -> LEI with ``lei_basis`` (``sec_submissions`` = the SEC profile's LEI field;
  ``isin_lei_map`` = GLEIF's ISIN-LEI relationship file on the CIK's derived ISINs; ``nport_issuer_lei`` = the
  issuer LEI funds report on N-PORT for the CIK's CUSIPs (>= 3 accessions, >= 80 % agreement); ``name_address`` = exact
  normalised legal name + registered or HQ address country/region/postal code, unique in both directions) and
  the GLEIF Level 1 fields.
* ``hierarchy.parquet``: Level 2 relationships of every matched LEI: direct and ultimate accounting-consolidation
  parents (``IS_DIRECTLY_CONSOLIDATED_BY`` / ``IS_ULTIMATELY_CONSOLIDATED_BY``) with status, period and
  validation, the parent's name, and the parent's CIK when the parent LEI is itself matched.

Clocks: GLEIF and OpenFIGI are one-time snapshots (``available_at`` = fetch time, ``vintage_risk =
'snapshot_non_pit'``); GLEIF record dates (initial registration, last update, relationship period) are kept.

Sources and terms: OpenFIGI API (``https://api.openfigi.com/v3/mapping``), free and open to the public;
without an API key 25 mapping requests per minute and 10 jobs per request (we send one request every
``FIGI_MIN_INTERVAL_S`` seconds and obey ``ratelimit-*`` headers). GLEIF golden copy, relationship records and
ISIN-LEI files: open data under CC0, published daily.

Landings (``data/raw/openfigi``, ``data/raw/gleif``): responses and files as served plus ``receipts.jsonl``
(url, bytes, sha256, http_status, fetched_at); every fetch resumes from the ledger.

Usage (under the memory guard)::

    python -m atx_db.alpha_panel.figi_lei figi-plan            # job lists from the identity stages
    python -m atx_db.alpha_panel.figi_lei figi-fetch --pass cusip [--limit N]
    python -m atx_db.alpha_panel.figi_lei figi-build
    python -m atx_db.alpha_panel.figi_lei gleif-fetch
    python -m atx_db.alpha_panel.figi_lei gleif-parse
    python -m atx_db.alpha_panel.figi_lei lei-build            # lei.parquet + hierarchy.parquet
"""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
import re
import sys
import time
import unicodedata
from pathlib import Path
from typing import Any

from . import common as C
from . import cusip_history as CH
from . import shortflow_common as S

STAGE = "security_master"
SCHEMA_FIGI = "atx.alpha-panel.figi/v1"
SCHEMA_LEI = "atx.alpha-panel.lei/v1"
MODULES = ("figi_lei", "cusip_history", "common", "shortflow_common")
MEMORY = "250MB"

FIGI_URL = "https://api.openfigi.com/v3/mapping"
FIGI_RAW = S.RAW_ROOT / "openfigi"
FIGI_JOBS_PER_REQUEST = 10
FIGI_MIN_INTERVAL_S = 2.6          # <= 23 requests / minute (documented unauthenticated limit: 25 / minute)
FIGI_PART_REQUESTS = 1000          # requests per response file
FIGI_PASSES = ("cusip", "cusip_any", "isin_fragment", "ticker")
FIGI_WARN_NONE = "No identifier found."

GLEIF_RAW = S.RAW_ROOT / "gleif"
GLEIF_PUBLISH_URL = "https://goldencopy.gleif.org/api/v2/golden-copies/publishes/latest"
GLEIF_ISIN_URL = "https://mapping.gleif.org/api/v2/isin-lei/latest"


# ---------------------------------------------------------------- single-runner lock
def _pid_alive(pid: int) -> bool:
    if os.name != "nt":
        try:
            os.kill(pid, 0)
            return True
        except OSError:
            return False
    import ctypes

    k32 = ctypes.windll.kernel32
    h = k32.OpenProcess(0x1000, False, pid)          # PROCESS_QUERY_LIMITED_INFORMATION
    if not h:
        return False
    code = ctypes.c_ulong()
    ok = k32.GetExitCodeProcess(h, ctypes.byref(code))
    k32.CloseHandle(h)
    return bool(ok) and code.value == 259            # STILL_ACTIVE


class RunLock:
    """Exclusive per-landing lock file (``<dir>/.lock``, holder pid): a second fetcher exits instead of doubling
    the request rate against a host limit. A lock whose pid is gone is taken over."""

    def __init__(self, directory: Path) -> None:
        self.path = directory / ".lock"
        self.held = False

    def __enter__(self) -> "RunLock":
        self.path.parent.mkdir(parents=True, exist_ok=True)
        for _ in range(2):
            try:
                fd = os.open(self.path, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
                os.write(fd, str(os.getpid()).encode())
                os.close(fd)
                self.held = True
                return self
            except FileExistsError:
                try:
                    pid = int(self.path.read_text().strip() or 0)
                except (OSError, ValueError):
                    pid = 0
                if pid and _pid_alive(pid):
                    raise SystemExit(f"{self.path}: held by live pid {pid}; not starting a second runner")
                self.path.unlink(missing_ok=True)
        raise SystemExit(f"{self.path}: could not take the lock")

    def __exit__(self, *exc: object) -> None:
        if self.held:
            self.path.unlink(missing_ok=True)


def peak_memory_gb() -> dict[str, float]:
    """This process's peak working set and peak commit (Windows), for the C-1 unguarded-run record."""
    if os.name != "nt":
        return {}
    import ctypes
    from ctypes import wintypes as w

    class PMC(ctypes.Structure):
        _fields_ = [("cb", w.DWORD), ("PageFaultCount", w.DWORD), ("PeakWorkingSetSize", ctypes.c_size_t),
                    ("WorkingSetSize", ctypes.c_size_t), ("QuotaPeakPagedPoolUsage", ctypes.c_size_t),
                    ("QuotaPagedPoolUsage", ctypes.c_size_t), ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t),
                    ("QuotaNonPagedPoolUsage", ctypes.c_size_t), ("PagefileUsage", ctypes.c_size_t),
                    ("PeakPagefileUsage", ctypes.c_size_t)]

    pmc = PMC()
    pmc.cb = ctypes.sizeof(PMC)
    k32, psapi = ctypes.WinDLL("kernel32"), ctypes.WinDLL("psapi")
    k32.GetCurrentProcess.restype = w.HANDLE
    psapi.GetProcessMemoryInfo.argtypes = [w.HANDLE, ctypes.POINTER(PMC), w.DWORD]
    psapi.GetProcessMemoryInfo(k32.GetCurrentProcess(), ctypes.byref(pmc), pmc.cb)
    return {"peak_working_set_gb": round(pmc.PeakWorkingSetSize / 1024 ** 3, 4),
            "peak_commit_gb": round(pmc.PeakPagefileUsage / 1024 ** 3, 4)}


# ---------------------------------------------------------------- OpenFIGI: pure helpers
def job_key(job: dict[str, str]) -> str:
    """Stable identity of one mapping job (idType, idValue, exchCode)."""
    return f"{job['idType']}|{job['idValue']}|{job.get('exchCode', '')}"


def chunk(jobs: list[dict[str, str]], size: int = FIGI_JOBS_PER_REQUEST) -> list[list[dict[str, str]]]:
    return [jobs[i:i + size] for i in range(0, len(jobs), size)]


def parse_response(jobs: list[dict[str, str]], body: bytes) -> list[dict[str, Any]]:
    """One row per (job, result); a job without results gives one row with ``warning`` / ``error`` set.

    The response array is aligned with the request array (OpenFIGI v3 contract); a length mismatch raises."""
    arr = json.loads(body)
    if not isinstance(arr, list) or len(arr) != len(jobs):
        raise ValueError(f"OpenFIGI response has {len(arr) if isinstance(arr, list) else 'no'} entries for {len(jobs)} jobs")
    rows: list[dict[str, Any]] = []
    for job, res in zip(jobs, arr):
        base = {"id_type": job["idType"], "query_value": job["idValue"], "query_exch": job.get("exchCode")}
        data = res.get("data") if isinstance(res, dict) else None
        if not data:
            rows.append({**base, "result_rank": None, "warning": res.get("warning") if isinstance(res, dict) else None,
                         "error": res.get("error") if isinstance(res, dict) else None})
            continue
        for i, d in enumerate(data):
            rows.append({**base, "result_rank": i, "warning": None, "error": None, "figi": d.get("figi"),
                         "composite_figi": d.get("compositeFIGI"), "share_class_figi": d.get("shareClassFIGI"),
                         "ticker": d.get("ticker"), "exch_code": d.get("exchCode"), "name": d.get("name"),
                         "security_type": d.get("securityType"), "security_type2": d.get("securityType2"),
                         "market_sector": d.get("marketSector"), "security_description": d.get("securityDescription")})
    return rows


def pick_share_class(rows: list[dict[str, Any]]) -> str | None:
    """The share-class FIGI common to a job's results (None when absent or when results disagree)."""
    vals = {r.get("share_class_figi") for r in rows if r.get("share_class_figi")}
    return vals.pop() if len(vals) == 1 else None


# ---------------------------------------------------------------- OpenFIGI: fetch
class FigiClient:
    """Sequential POSTs to the mapping endpoint: ``FIGI_MIN_INTERVAL_S`` apart, ``ratelimit-*`` headers obeyed."""

    def __init__(self) -> None:
        import requests

        self.session = requests.Session()
        self.session.headers.update({"User-Agent": S.PUBLIC_UA, "Content-Type": "application/json"})
        self._last = 0.0

    def post(self, jobs: list[dict[str, str]]) -> tuple[int, bytes, dict[str, str]]:
        import requests

        err: Exception | None = None
        for attempt in range(8):
            wait = FIGI_MIN_INTERVAL_S - (time.monotonic() - self._last)
            if wait > 0:
                time.sleep(wait)
            self._last = time.monotonic()
            try:
                r = self.session.post(FIGI_URL, data=json.dumps(jobs), timeout=120)
            except (requests.ConnectionError, requests.Timeout) as exc:
                err = exc
                time.sleep(min(2.0 ** attempt, 120))
                continue
            hdr = {k.lower(): v for k, v in r.headers.items()}
            if r.status_code == 429 or r.status_code >= 500:
                err = RuntimeError(f"HTTP {r.status_code}")
                reset = hdr.get("ratelimit-reset") or hdr.get("retry-after")
                time.sleep(min(float(reset) + 1, 120) if reset and reset.replace(".", "").isdigit()
                           else min(2.0 ** attempt, 120))
                continue
            if hdr.get("ratelimit-remaining") == "0" and (hdr.get("ratelimit-reset") or "").isdigit():
                self._last = time.monotonic() + float(hdr["ratelimit-reset"])
            return r.status_code, r.content, hdr
        raise RuntimeError(f"OpenFIGI: gave up after 8 tries: {err}")


def figi_ledger() -> S.Ledger:
    return S.Ledger(FIGI_RAW / "receipts.jsonl")


def done_job_keys() -> set[str]:
    out: set[str] = set()
    for rec in figi_ledger().records():
        if rec.get("kind") == "mapping" and rec.get("http_status") == 200:
            out.update(job_key(j) for j in rec.get("jobs", []))
    return out


def jobs_path(pass_name: str) -> Path:
    return FIGI_RAW / f"jobs_{pass_name}.jsonl"


def figi_fetch(pass_name: str, limit: int | None = None) -> dict[str, Any]:
    """Send every job of ``jobs_<pass>.jsonl`` not yet answered (HTTP 200) in the ledger (one runner at a time)."""
    FIGI_RAW.mkdir(parents=True, exist_ok=True)
    with RunLock(FIGI_RAW):
        return _figi_fetch(pass_name, limit)


def _figi_fetch(pass_name: str, limit: int | None) -> dict[str, Any]:
    (FIGI_RAW / "responses").mkdir(exist_ok=True)
    jobs = [json.loads(line) for line in jobs_path(pass_name).read_text(encoding="utf-8").splitlines() if line.strip()]
    done = done_job_keys()
    todo = [j for j in jobs if job_key(j) not in done]
    batches = chunk(todo)
    if limit is not None:
        batches = batches[:limit]
    led = figi_ledger()
    n_prev = sum(1 for r in led.records() if r.get("kind") == "mapping")
    client = FigiClient()
    stats = {"pass": pass_name, "jobs": len(jobs), "done_before": len(jobs) - len(todo), "requests": 0, "errors": 0}
    print(f"openfigi {pass_name}: {len(jobs)} jobs, {len(todo)} to send in {len(batches)} requests", flush=True)
    for i, batch in enumerate(batches):
        status, body, hdr = client.post(batch)
        seq = n_prev + i
        part = FIGI_RAW / "responses" / f"part-{seq // FIGI_PART_REQUESTS:05d}.jsonl"
        sha = hashlib.sha256(body).hexdigest()
        with part.open("a", encoding="utf-8") as fh:
            fh.write(json.dumps({"seq": seq, "sha256": sha, "jobs": batch,
                                 "body": body.decode("utf-8", errors="replace")}) + "\n")
        led.append({"key": f"req-{seq:07d}", "kind": "mapping", "pass": pass_name, "url": FIGI_URL, "method": "POST",
                    "http_status": status, "bytes": len(body), "sha256": sha, "fetched_at": S.utc_now(),
                    "jobs": batch, "file": f"responses/{part.name}", "seq": seq,
                    "ratelimit_remaining": hdr.get("ratelimit-remaining")})
        stats["requests"] += 1
        if status != 200:
            stats["errors"] += 1
        if (i + 1) % 100 == 0:
            print(f"  {pass_name}: {i + 1}/{len(batches)} requests, errors {stats['errors']}", flush=True)
    return stats


# ---------------------------------------------------------------- OpenFIGI: plan
def figi_plan(fragment_top: int = 300, thirteenf_per_quarter: int = 800, fragment_countries: int = 10) -> dict[str, Any]:
    """Write ``jobs_<pass>.jsonl`` for every pass from the stage lake (deterministic order: priority, value)."""
    root = C.build_root()
    wd = CH.work_dir()
    con = C.connect(memory=MEMORY, threads=2)
    con.create_function("cusip_ok", CH.cusip_valid, ["VARCHAR"], "BOOLEAN", null_handling="special")
    con.create_function("issue_kind", CH.issue_kind, ["VARCHAR"], "VARCHAR", null_handling="special")
    member = (root / "_tmp" / "panel_member" / "*.parquet").as_posix()
    runs = (wd / "ftd_cusip_runs.parquet").as_posix()
    q = (wd / "thirteenf_cusip_q.parquet").as_posix()
    FIGI_RAW.mkdir(parents=True, exist_ok=True)
    con.execute(f"CREATE TABLE mem AS SELECT DISTINCT security_id FROM read_parquet('{member}') WHERE member")
    # pass cusip: member-line FTD CUSIPs first, then other FTD CUSIPs, then value-ranked 13F CUSIPs FTD cannot map
    if not (wd / "ftd_cusip_runs.parquet").exists():   # before cusip_history collect: the ftd stage's own map
        runs = (root / "ftd" / "cusip_map.parquet").as_posix()
        con.execute(f"""CREATE VIEW r0 AS SELECT cusip, security_id, last_seen AS obs_to FROM read_parquet('{runs}')""")
    else:
        con.execute(f"CREATE VIEW r0 AS SELECT cusip, security_id, obs_to FROM read_parquet('{runs}')")
    con.execute("""CREATE TABLE c1 AS
        SELECT cusip, max(CASE WHEN security_id IN (SELECT security_id FROM mem) THEN 1 ELSE 0 END) AS member,
               max(obs_to) AS last_obs
        FROM r0 WHERE cusip_ok(cusip) GROUP BY 1""")
    if not Path(q).exists():
        con.execute("CREATE TABLE c2 (cusip VARCHAR, max_value DOUBLE)")
        rows = con.execute("SELECT cusip FROM c1 ORDER BY -member, -epoch(last_obs), cusip").fetchall()
        n1 = _write_jobs("cusip", [{"idType": "ID_CUSIP", "idValue": r[0], "exchCode": "US"} for r in rows])
        con.close()
        return {"cusip": n1, "note": "ftd stage map only (13F CUSIPs and ISIN fragments after cusip_history collect)"}
    con.execute(f"""CREATE TABLE c2 AS
        WITH k AS (SELECT period_q, cusip, value_sh FROM read_parquet('{q}')
                   WHERE value_sh > 0 AND issue_kind(cusip) = 'equity' AND NOT coalesce(option_title, false)
                     AND cusip NOT IN (SELECT cusip FROM c1)),
        r AS (SELECT *, row_number() OVER (PARTITION BY period_q ORDER BY value_sh DESC, cusip) AS rk FROM k)
        SELECT cusip, max(value_sh) AS max_value FROM r WHERE rk <= {thirteenf_per_quarter} GROUP BY 1""")
    rows = con.execute("""
        SELECT cusip FROM (
            SELECT cusip, 0 AS pri, -member AS a, -epoch(last_obs) AS b FROM c1
            UNION ALL SELECT cusip, 1, 0, -max_value FROM c2)
        ORDER BY pri, a, b, cusip""").fetchall()
    n1 = _write_jobs("cusip", [{"idType": "ID_CUSIP", "idValue": r[0], "exchCode": "US"} for r in rows])
    # pass isin_fragment: 13F CUSIP fields failing the CUSIP check digit, by value; candidate ISINs per country
    frag = con.execute(f"""
        SELECT cusip, sum(value_sh) AS v FROM read_parquet('{q}')
        WHERE value_sh > 0 AND length(cusip) = 9 AND NOT cusip_ok(cusip) AND regexp_matches(cusip, '^[0-9A-Z]{{9}}$')
        GROUP BY 1 ORDER BY v DESC, cusip LIMIT {int(fragment_top)}""").fetchall()
    fjobs = []
    for cusip, _ in frag:
        for isin in CH.isin_fragment_candidates(cusip, CH.FRAGMENT_COUNTRIES[:fragment_countries]):
            fjobs.append({"idType": "ID_ISIN", "idValue": isin, "exchCode": "US"})
    n3 = _write_jobs("isin_fragment", fjobs)
    con.close()
    return {"cusip": n1, "isin_fragment": n3, "fragments": len(frag)}


def figi_plan_arrow() -> dict[str, Any]:
    """Pass ``cusip`` jobs from the ftd stage's CUSIP map with pyarrow only (no DuckDB; ruling C-1): member-line
    CUSIPs first, then by last observation, newest first. The DuckDB plan later adds 13F CUSIPs and fragments."""
    import pyarrow.parquet as pq

    root = C.build_root()
    mem: set[int] = set()
    for f in sorted((root / "_tmp" / "panel_member").glob("*.parquet")):
        t = pq.read_table(f, columns=["security_id", "member"])
        mem.update(sid for sid, m in zip(t.column("security_id").to_pylist(), t.column("member").to_pylist()) if m)
    t = pq.read_table(root / "ftd" / "cusip_map.parquet", columns=["cusip", "security_id", "last_seen"])
    best: dict[str, tuple[int, Any]] = {}
    for c, sid, last in zip(*(t.column(k).to_pylist() for k in ("cusip", "security_id", "last_seen"))):
        if not c or not CH.cusip_valid(c):
            continue
        m, l0 = best.get(c, (0, last))
        best[c] = (max(m, int(sid in mem)), max(l0, last))
    order = sorted(best, key=lambda c: (-best[c][0], -best[c][1].toordinal(), c))
    n = _write_jobs("cusip", [{"idType": "ID_CUSIP", "idValue": c, "exchCode": "US"} for c in order])
    return {"cusip": n, "member_cusips": sum(1 for v in best.values() if v[0]), "member_lines": len(mem)}


def figi_plan_followups() -> dict[str, Any]:
    """After pass ``cusip``: ``cusip_any`` for FTD CUSIPs with no US composite; ``ticker`` for member lines alive
    at the snapshot that no CUSIP covers."""
    root = C.build_root()
    wd = CH.work_dir()
    res = _answers()
    none_cusips = sorted({k.split("|")[1] for k, v in res.items() if k.startswith("ID_CUSIP|") and k.endswith("|US")
                          and not v})
    con = C.connect(memory=MEMORY, threads=2)
    runs = (wd / "ftd_cusip_runs.parquet").as_posix()
    ftd_cusips = {r[0] for r in con.execute(f"SELECT DISTINCT cusip FROM read_parquet('{runs}')").fetchall()}
    n2 = _write_jobs("cusip_any", [{"idType": "ID_CUSIP", "idValue": c} for c in none_cusips if c in ftd_cusips])
    hit_cusips = sorted(k.split("|")[1] for k, v in res.items() if k.startswith("ID_CUSIP|") and v)
    import pyarrow as pa

    con.register("hit_arrow", pa.table({"cusip": pa.array(hit_cusips, pa.string())}))
    con.execute("CREATE TABLE hit AS SELECT * FROM hit_arrow")
    member = (root / "_tmp" / "panel_member" / "*.parquet").as_posix()
    lines = (root / STAGE / "lines.parquet").as_posix()
    tick = con.execute(f"""
        WITH mem AS (SELECT DISTINCT security_id FROM read_parquet('{member}') WHERE member),
        covered AS (SELECT DISTINCT security_id FROM read_parquet('{runs}') WHERE cusip IN (SELECT cusip FROM hit))
        SELECT l.last_ticker FROM read_parquet('{lines}') l
        WHERE l.security_id IN (SELECT security_id FROM mem) AND l.security_id NOT IN (SELECT security_id FROM covered)
          AND l.delisting_date IS NULL AND l.last_ticker IS NOT NULL
        ORDER BY 1""").fetchall()
    con.close()
    n4 = _write_jobs("ticker", [{"idType": "TICKER", "idValue": bloomberg_ticker(t[0]), "exchCode": "US"} for t in tick])
    return {"cusip_any": n2, "ticker": n4}


def bloomberg_ticker(vendor: str) -> str:
    """Vendor ticker -> Bloomberg spelling: share-class and suffix separators '.' / '-' become '/' (BRK.B -> BRK/B)."""
    return re.sub(r"[.\-]", "/", vendor.strip().upper())


def _write_jobs(pass_name: str, jobs: list[dict[str, str]]) -> int:
    seen: set[str] = set()
    uniq = []
    for j in jobs:
        k = job_key(j)
        if k not in seen:
            seen.add(k)
            uniq.append(j)
    p = jobs_path(pass_name)
    p.parent.mkdir(parents=True, exist_ok=True)
    tmp = p.with_name(p.name + ".partial")
    tmp.write_text("".join(json.dumps(j, sort_keys=True) + "\n" for j in uniq), encoding="utf-8")
    os.replace(tmp, p)
    return len(uniq)


def _answers() -> dict[str, list[dict[str, Any]]]:
    """{job_key: result rows (empty when no identifier)} from every HTTP-200 response on disk (last answer wins)."""
    out: dict[str, list[dict[str, Any]]] = {}
    for part in sorted((FIGI_RAW / "responses").glob("part-*.jsonl")):
        for line in part.read_text(encoding="utf-8").splitlines():
            if not line.strip():
                continue
            try:
                rec = json.loads(line)
            except json.JSONDecodeError:
                continue
            body = rec["body"].encode("utf-8")
            if hashlib.sha256(body).hexdigest() != rec["sha256"]:
                continue
            try:
                rows = parse_response(rec["jobs"], body)
            except (ValueError, json.JSONDecodeError):
                continue
            by: dict[str, list[dict[str, Any]]] = {}
            for job in rec["jobs"]:
                by[job_key(job)] = []
            for r in rows:
                k = f"{r['id_type']}|{r['query_value']}|{r['query_exch'] or ''}"
                if r.get("result_rank") is not None:
                    by[k].append(r)
            out.update(by)
    return out


# ---------------------------------------------------------------- OpenFIGI: build
QUERY_KIND = {("ID_CUSIP", True): "cusip", ("ID_CUSIP", False): "cusip_any", ("ID_ISIN", True): "isin_fragment",
              ("ID_ISIN", False): "isin_fragment", ("TICKER", True): "ticker", ("TICKER", False): "ticker"}


def figi_build() -> dict[str, Any]:
    """``figi.parquet`` from every landed response; US composite tickers mapped to the vendor line."""
    import pyarrow as pa

    root = C.build_root()
    fetched: dict[str, str] = {}
    for rec in figi_ledger().records():
        if rec.get("kind") == "mapping" and rec.get("http_status") == 200:
            for j in rec.get("jobs", []):
                fetched[job_key(j)] = rec["fetched_at"]
    rows: list[dict[str, Any]] = []
    for key, res in _answers().items():
        id_type, value, exch = key.split("|")
        kind = QUERY_KIND[(id_type, bool(exch))]
        base = {"query_kind": kind, "id_type": id_type, "query_value": value, "query_exch": exch or None,
                "cusip_field": value if id_type == "ID_CUSIP" else (value[2:11] if id_type == "ID_ISIN" else None),
                "fetched_at": fetched.get(key)}
        if not res:
            rows.append({**base, "result_rank": None})
        for r in res:
            rows.append({**base, **{k: r.get(k) for k in ("result_rank", "figi", "composite_figi", "share_class_figi",
                                                         "ticker", "exch_code", "name", "security_type",
                                                         "security_type2", "market_sector", "security_description")}})
    cols = ["query_kind", "id_type", "query_value", "query_exch", "cusip_field", "result_rank", "figi", "composite_figi",
            "share_class_figi", "ticker", "exch_code", "name", "security_type", "security_type2", "market_sector",
            "security_description", "fetched_at"]
    tbl = pa.table({c: [r.get(c) for r in rows] for c in cols})
    con = C.connect(memory=MEMORY, threads=2)
    con.register("raw_rows", tbl)
    vendor = (root / "_tmp" / "shortflow" / "vendor" / "vendor_tickers_*.parquet").as_posix()
    last = con.execute(f"SELECT max(d) FROM read_parquet('{vendor}')").fetchone()[0]
    wd = CH.work_dir()
    mapped = wd / "figi_ticker_map.parquet"
    S.map_symbols_asof(con, f"""SELECT DATE '{last}' AS key_date, ticker AS symbol FROM raw_rows
                               WHERE exch_code = 'US' AND ticker IS NOT NULL""", vendor, mapped)
    dest = root / STAGE / "figi.parquet"
    n = C.copy_to_parquet(con, f"""
        WITH frag AS (   -- an ISIN fragment counts only when exactly one candidate ISIN resolves to a US composite
            SELECT cusip_field FROM raw_rows WHERE query_kind = 'isin_fragment' AND exch_code = 'US'
            GROUP BY 1 HAVING count(DISTINCT composite_figi) = 1
        )
        SELECT r.query_kind, r.id_type, r.query_value, r.query_exch, r.cusip_field, CAST(r.result_rank AS INTEGER) AS result_rank,
               r.figi, r.composite_figi, r.share_class_figi, r.ticker, r.exch_code, r.name, r.security_type,
               r.security_type2, r.market_sector, r.security_description,
               coalesce(r.exch_code = 'US', false) AS is_us_composite,
               CASE WHEN r.exch_code = 'US' AND (r.query_kind <> 'isin_fragment' OR r.cusip_field IN (SELECT cusip_field FROM frag))
                    THEN m.candidate_security_id END AS security_id,
               CAST(r.fetched_at AS TIMESTAMP) AS fetched_at, CAST(r.fetched_at AS TIMESTAMP) AS available_at,
               'snapshot_non_pit' AS vintage_risk
        FROM raw_rows r LEFT JOIN read_parquet('{mapped.as_posix()}') m
          ON m.symbol = r.ticker AND m.key_date = DATE '{last}' AND r.exch_code = 'US'
        ORDER BY r.query_kind, r.query_value, r.result_rank NULLS FIRST""", dest)
    d = dest.as_posix()
    summary = con.execute(f"""
        SELECT query_kind, count(DISTINCT query_value) AS queries,
               count(DISTINCT query_value) FILTER (WHERE figi IS NOT NULL) AS answered,
               count(DISTINCT query_value) FILTER (WHERE is_us_composite) AS us_composite,
               count(DISTINCT query_value) FILTER (WHERE security_id IS NOT NULL) AS mapped_to_line
        FROM read_parquet('{d}') GROUP BY 1 ORDER BY 1""").fetchall()
    con.close()
    return {"rows": n, "last_vendor_session": str(last),
            "by_kind": {r[0]: dict(zip(("queries", "answered", "us_composite", "mapped_to_line"), r[1:])) for r in summary}}


def figi_lines() -> dict[str, Any]:
    """``line_figi.parquet``: per line, the FIGI of its latest CUSIP run with an answer, else its ticker's."""
    root = C.build_root()
    con = C.connect(memory=MEMORY, threads=2)
    figi = (root / STAGE / "figi.parquet").as_posix()
    hist = (root / STAGE / "cusip_history.parquet").as_posix()
    lines = (root / STAGE / "lines.parquet").as_posix()
    dest = root / STAGE / "line_figi.parquet"
    n = C.copy_to_parquet(con, f"""
        WITH per_cusip AS (   -- one answer per CUSIP: the US composite, else the common share-class FIGI
            SELECT cusip_field AS cusip,
                   any_value(composite_figi) FILTER (WHERE is_us_composite) AS composite_figi,
                   CASE WHEN count(DISTINCT share_class_figi) = 1 THEN any_value(share_class_figi) END AS share_class_figi,
                   any_value(ticker) FILTER (WHERE is_us_composite) AS us_ticker, any_value(name) AS name,
                   any_value(security_type) AS security_type, bool_or(is_us_composite) AS us, max(fetched_at) AS fetched_at
            FROM read_parquet('{figi}') WHERE query_kind IN ('cusip', 'cusip_any') AND figi IS NOT NULL
            GROUP BY 1
        ),
        via_cusip AS (
            SELECT h.security_id, p.*, row_number() OVER (PARTITION BY h.security_id
                   ORDER BY p.us DESC, h.obs_to DESC, h.basis = 'ftd_symbol' DESC, h.cusip) AS rk
            FROM read_parquet('{hist}') h JOIN per_cusip p USING (cusip)
            WHERE h.cusip_kind = 'cusip' AND (p.composite_figi IS NOT NULL OR p.share_class_figi IS NOT NULL)
        ),
        via_ticker AS (
            SELECT security_id, any_value(composite_figi) AS composite_figi, any_value(share_class_figi) AS share_class_figi,
                   any_value(ticker) AS us_ticker, any_value(name) AS name, any_value(security_type) AS security_type,
                   max(fetched_at) AS fetched_at
            FROM read_parquet('{figi}') WHERE query_kind = 'ticker' AND is_us_composite AND security_id IS NOT NULL
            GROUP BY 1 HAVING count(DISTINCT composite_figi) = 1
        )
        SELECT l.security_id,
               coalesce(c.composite_figi, t.composite_figi) AS composite_figi,
               coalesce(c.share_class_figi, t.share_class_figi) AS share_class_figi,
               coalesce(c.us_ticker, t.us_ticker) AS figi_ticker, coalesce(c.name, t.name) AS figi_name,
               coalesce(c.security_type, t.security_type) AS figi_security_type, c.cusip AS figi_cusip,
               CASE WHEN c.composite_figi IS NOT NULL THEN 'cusip_us_composite'
                    WHEN c.share_class_figi IS NOT NULL THEN 'cusip_share_class'
                    WHEN t.security_id IS NOT NULL THEN 'ticker_us_composite' END AS figi_basis,
               CAST(coalesce(c.fetched_at, t.fetched_at) AS TIMESTAMP) AS available_at, 'snapshot_non_pit' AS vintage_risk
        FROM read_parquet('{lines}') l
        LEFT JOIN (SELECT * FROM via_cusip WHERE rk = 1) c USING (security_id)
        LEFT JOIN via_ticker t USING (security_id)
        ORDER BY l.security_id""", dest)
    member = (root / "_tmp" / "panel_member" / "*.parquet").as_posix()
    panel = (root / "panel" / "year=*" / "*.parquet").as_posix()
    d = dest.as_posix()
    res: dict[str, Any] = {"rows": n}
    for label, sql in (("member_lines", f"SELECT DISTINCT security_id FROM read_parquet('{member}') WHERE member"),
                       ("member_equity_lines", f"SELECT DISTINCT security_id FROM read_parquet('{panel}', hive_partitioning = false) WHERE member_equity")):
        try:
            r = con.execute(f"""SELECT count(*), count(*) FILTER (WHERE composite_figi IS NOT NULL OR share_class_figi IS NOT NULL),
                                       count(*) FILTER (WHERE composite_figi IS NOT NULL)
                                FROM read_parquet('{d}') WHERE security_id IN ({sql})""").fetchone()
        except Exception as exc:  # panel stage absent or mid-rebuild
            res[label] = {"error": str(exc)[:200]}
            continue
        res[label] = {"lines": r[0], "with_figi": r[1], "share_with_figi": round(r[1] / r[0], 4) if r[0] else None,
                      "with_us_composite": r[2]}
    res["by_basis"] = dict(con.execute(f"SELECT coalesce(figi_basis, 'none'), count(*) FROM read_parquet('{d}') GROUP BY 1").fetchall())
    con.close()
    return res


def publish_figi(receipt: dict[str, Any]) -> Path:
    root = C.build_root()
    led = figi_ledger()
    recs = [r for r in led.records() if r.get("kind") == "mapping"]
    manifest = {
        "schema": SCHEMA_FIGI, "status": "complete", "stage": STAGE, "rule_text": __doc__,
        "code": C.code_identity(*MODULES),
        "files": C.output_hashes(root / STAGE, "figi.parquet") | C.output_hashes(root / STAGE, "line_figi.parquet"),
        "input_manifests_sha256": {k: C.sha256_file(p) for k, p in (
            ("cusip_history_manifest_sha256", root / STAGE / "cusip_history_manifest.json"),
            ("security_master_manifest_sha256", root / STAGE / "manifest.json")) if p.exists()},
        "sources": {"openfigi": {"url": FIGI_URL, "ledger": str(led.path), "ledger_sha256": C.sha256_file(led.path),
                                 "requests": len(recs), "requests_http_200": sum(1 for r in recs if r.get("http_status") == 200),
                                 "response_bytes": sum(int(r.get("bytes") or 0) for r in recs),
                                 "first_fetch": min((r["fetched_at"] for r in recs), default=None),
                                 "last_fetch": max((r["fetched_at"] for r in recs), default=None),
                                 "terms": "OpenFIGI API: free and open to the public; unauthenticated limit 25 mapping "
                                          "requests / minute, 10 jobs / request (docs, 2026-09-29)"}},
        "staleness": "snapshot identifiers; FIGIs are permanent, tickers are as of the fetch",
        "receipt": receipt,
    }
    path = root / STAGE / "figi_manifest.json"
    C.write_json_atomic(path, manifest)
    return path


# ---------------------------------------------------------------- GLEIF: fetch + parse
def gleif_ledger() -> S.Ledger:
    return S.Ledger(GLEIF_RAW / "receipts.jsonl")


def _stream(url: str, dest: Path) -> dict[str, Any]:
    import requests

    tmp = dest.with_name(dest.name + ".partial")
    h = hashlib.sha256()
    with requests.get(url, headers={"User-Agent": S.PUBLIC_UA}, stream=True, timeout=600) as r:
        status = r.status_code
        hdr = {k.lower(): v for k, v in r.headers.items()}
        if status != 200:
            return {"http_status": status}
        with tmp.open("wb") as fh:
            for blob in r.iter_content(chunk_size=8 << 20):
                if blob:
                    fh.write(blob)
                    h.update(blob)
    os.replace(tmp, dest)
    return {"http_status": status, "bytes": dest.stat().st_size, "sha256": h.hexdigest(),
            "last_modified": hdr.get("last-modified"), "etag": hdr.get("etag")}


def gleif_fetch(kinds: tuple[str, ...] = ("lei2", "rr", "isin_lei")) -> dict[str, Any]:
    """Golden-copy LEI2 and RR CSV zips of the latest publish, and the latest ISIN-LEI relationship file."""
    GLEIF_RAW.mkdir(parents=True, exist_ok=True)
    with RunLock(GLEIF_RAW):
        return _gleif_fetch(kinds)


def _gleif_fetch(kinds: tuple[str, ...]) -> dict[str, Any]:
    led = gleif_ledger()
    have = led.latest()
    host = S.PoliteHost(min_interval=1.0)
    st, body, _ = host.request(GLEIF_PUBLISH_URL, accept="application/json")
    pub = json.loads(body)
    snap = GLEIF_RAW / f"publish_{dt.datetime.now(dt.UTC):%Y%m%dT%H%M%SZ}.json"
    snap.write_bytes(body)
    led.append({"key": "publish", "kind": "index", "url": GLEIF_PUBLISH_URL, "http_status": st, "bytes": len(body),
                "sha256": hashlib.sha256(body).hexdigest(), "fetched_at": S.utc_now(), "file": snap.name})
    data = pub.get("data", pub)
    targets = []
    for kind in ("lei2", "rr"):
        f = data[kind]["full_file"]["csv"]
        targets.append((kind, f["url"], {"record_count": f.get("record_count"), "publish_date": data[kind].get("publish_date"),
                                         "declared_size": f.get("size")}))
    st, body, _ = host.request(GLEIF_ISIN_URL, accept="application/vnd.api+json")
    isin = json.loads(body)["data"]["attributes"]
    led.append({"key": "isin_index", "kind": "index", "url": GLEIF_ISIN_URL, "http_status": st, "bytes": len(body),
                "sha256": hashlib.sha256(body).hexdigest(), "fetched_at": S.utc_now(), "file_name": isin.get("fileName")})
    targets.append(("isin_lei", isin["downloadLink"], {"file_name": isin.get("fileName"), "uploaded_at": isin.get("uploadedAt")}))
    out = {}
    for kind, url, meta in targets:
        if kind not in kinds:
            continue
        name = url.rsplit("/", 2)[-1] if kind != "isin_lei" else meta["file_name"]
        prev = have.get(kind)
        if prev and prev.get("url") == url and prev.get("status") in ("downloaded", "parsed"):
            out[kind] = "already landed"
            continue
        S.require_free(2.0)
        dest = GLEIF_RAW / name
        time.sleep(1.0)
        res = _stream(url, dest)
        rec = {"key": kind, "kind": "file", "url": url, "file": name, "fetched_at": S.utc_now(), **meta, **res,
               "status": "downloaded" if res.get("http_status") == 200 else "failed"}
        led.append(rec)
        out[kind] = {k: rec.get(k) for k in ("http_status", "bytes", "sha256")}
    return out


LEI2_COLUMNS = {
    "LEI": "lei", "Entity.LegalName": "legal_name",
    **{f"Entity.OtherEntityNames.OtherEntityName.{i}": f"other_name_{i}" for i in range(1, 6)},
    "Entity.LegalAddress.FirstAddressLine": "legal_line1", "Entity.LegalAddress.City": "legal_city",
    "Entity.LegalAddress.Region": "legal_region", "Entity.LegalAddress.Country": "legal_country",
    "Entity.LegalAddress.PostalCode": "legal_postal",
    "Entity.HeadquartersAddress.FirstAddressLine": "hq_line1", "Entity.HeadquartersAddress.City": "hq_city",
    "Entity.HeadquartersAddress.Region": "hq_region", "Entity.HeadquartersAddress.Country": "hq_country",
    "Entity.HeadquartersAddress.PostalCode": "hq_postal",
    "Entity.RegistrationAuthority.RegistrationAuthorityID": "ra_id",
    "Entity.RegistrationAuthority.RegistrationAuthorityEntityID": "ra_entity_id",
    "Entity.LegalJurisdiction": "legal_jurisdiction", "Entity.EntityCategory": "entity_category",
    "Entity.EntityStatus": "entity_status", "Entity.SuccessorEntity.1.SuccessorLEI": "successor_lei",
    "Registration.InitialRegistrationDate": "initial_registration", "Registration.LastUpdateDate": "last_update",
    "Registration.RegistrationStatus": "registration_status", "Registration.ValidationSources": "validation_sources",
}
RR_COLUMNS = {
    "Relationship.StartNode.NodeID": "child_lei", "Relationship.EndNode.NodeID": "parent_lei",
    "Relationship.EndNode.NodeIDType": "parent_id_type", "Relationship.RelationshipType": "relationship_type",
    "Relationship.RelationshipStatus": "relationship_status",
    "Relationship.Period.1.startDate": "period1_start", "Relationship.Period.1.endDate": "period1_end",
    "Relationship.Period.1.periodType": "period1_type", "Relationship.Period.2.startDate": "period2_start",
    "Relationship.Period.2.endDate": "period2_end", "Relationship.Period.2.periodType": "period2_type",
    "Registration.InitialRegistrationDate": "initial_registration", "Registration.LastUpdateDate": "last_update",
    "Registration.RegistrationStatus": "registration_status", "Registration.ValidationSources": "validation_sources",
}
ISIN_COLUMNS = {"LEI": "lei", "ISIN": "isin"}


def csv_zip_to_parquet(zpath: Path, columns: dict[str, str], dest: Path, batch: int = 100_000) -> int:
    """Stream the single CSV member of ``zpath`` into Parquet with the ``columns`` renamed (all strings).

    Arrow's streaming CSV reader (quoted newlines allowed, 8 MB blocks) keeps the working set small and runs the
    lei2 golden copy (~0.5 GB zipped, 338 columns) in about a minute; ``batch`` is the Parquet row-group size."""
    import csv
    import io
    import zipfile

    import pyarrow as pa
    import pyarrow.csv as pacsv
    import pyarrow.parquet as pq

    schema = pa.schema([(v, pa.string()) for v in columns.values()])
    tmp = dest.with_name(dest.name + ".partial")
    dest.parent.mkdir(parents=True, exist_ok=True)
    n = 0
    with zipfile.ZipFile(zpath) as z:
        member = [m for m in z.namelist() if m.lower().endswith(".csv")]
        if len(member) != 1:
            raise RuntimeError(f"{zpath.name}: expected one CSV member, got {member}")
        with z.open(member[0]) as raw:
            header = next(csv.reader(io.TextIOWrapper(raw, encoding="utf-8-sig", newline="")))
        missing = [k for k in columns if k not in header]
        if missing:
            raise RuntimeError(f"{zpath.name}: columns missing from header: {missing}")
        with z.open(member[0]) as raw, pq.ParquetWriter(tmp, schema, compression="zstd") as w:
            reader = pacsv.open_csv(
                raw,
                read_options=pacsv.ReadOptions(column_names=header, skip_rows=1, block_size=8 << 20),
                parse_options=pacsv.ParseOptions(newlines_in_values=True),
                convert_options=pacsv.ConvertOptions(include_columns=list(columns),
                                                     column_types={k: pa.string() for k in columns},
                                                     strings_can_be_null=True, null_values=[""]))
            for rb in reader:
                t = pa.Table.from_batches([rb]).select(list(columns)).rename_columns(list(columns.values()))
                w.write_table(t.cast(schema), row_group_size=batch)
                n += t.num_rows
    os.replace(tmp, dest)
    return n


def parsed_dir() -> Path:
    p = GLEIF_RAW / "parsed"
    p.mkdir(parents=True, exist_ok=True)
    return p


def gleif_parse(delete_zips: bool = True) -> dict[str, Any]:
    """Parse each landed zip to ``data/raw/gleif/parsed/<kind>.parquet``; delete the zip; receipt kept."""
    led = gleif_ledger()
    out = {}
    for kind, cols in (("lei2", LEI2_COLUMNS), ("rr", RR_COLUMNS), ("isin_lei", ISIN_COLUMNS)):
        rec = led.latest().get(kind)
        if not rec or rec.get("status") not in ("downloaded", "parsed"):
            out[kind] = "not landed"
            continue
        if rec.get("status") == "parsed":
            out[kind] = "already parsed"
            continue
        zpath = GLEIF_RAW / rec["file"]
        if C.sha256_file(zpath) != rec["sha256"]:
            raise RuntimeError(f"{zpath}: sha256 mismatch vs receipt")
        dest = parsed_dir() / f"{kind}.parquet"
        t0 = time.perf_counter()
        n = csv_zip_to_parquet(zpath, cols, dest, batch=25_000 if kind == "lei2" else 100_000)
        if delete_zips:
            zpath.unlink()
        rec = {**rec, "status": "parsed", "parsed_at": S.utc_now(), "rows": n, "zip_deleted": delete_zips,
               "parsed_file": f"parsed/{dest.name}", "parsed_bytes": dest.stat().st_size,
               "parsed_sha256": C.sha256_file(dest), "parse_seconds": round(time.perf_counter() - t0, 1)}
        led.append(rec)
        out[kind] = {"rows": n, "parquet_bytes": rec["parsed_bytes"]}
    return out


# ---------------------------------------------------------------- LEI: match
_FORM_CANON = (("INCORPORATED", "INC"), ("CORPORATION", "CORP"), ("COMPANY", "CO"), ("LIMITED", "LTD"),
               ("PUBLIC LIMITED CO", "PLC"), ("HOLDINGS", "HLDGS"), ("HOLDING", "HLDG"), ("INTERNATIONAL", "INTL"),
               ("L P", "LP"), ("L L C", "LLC"), ("N V", "NV"), ("S A", "SA"), ("A G", "AG"), ("S E", "SE"),
               ("CO LTD", "CO LTD"), ("BANCORPORATION", "BANCORP"), ("TRUST", "TR"), ("GROUP", "GRP"))


def norm_entity_name(name: str | None) -> str | None:
    """Comparable legal name: ASCII upper case, '&' -> AND, punctuation dropped, SEC state tags (``/DE/``) and a
    leading THE removed, corporate-form words canonicalised (INCORPORATED -> INC, CORPORATION -> CORP ...)."""
    if not name:
        return None
    s = unicodedata.normalize("NFKD", name).encode("ascii", "ignore").decode().upper()
    s = re.sub(r"\s/[A-Z]{2,3}/?\s*$", " ", s)        # SEC conformed-name state tag: 'MOODYS CORP /DE/'
    s = re.sub(r"/[A-Z]{2,3}/?$", "", s.strip())
    s = s.replace("&", " AND ").replace("'", "").replace("’", "")
    s = re.sub(r"[^A-Z0-9]+", " ", s)
    s = " " + " ".join(s.split()) + " "
    for a, b in _FORM_CANON:
        s = s.replace(f" {a} ", f" {b} ")
    s = s.strip()
    if s.startswith("THE "):
        s = s[4:]
    return s or None


def postal5(v: str | None) -> str | None:
    """Comparable postal code: US ZIP -> first 5 digits; others upper case without spaces."""
    if not v:
        return None
    s = re.sub(r"\s+", "", v.upper())
    m = re.match(r"^(\d{5})(-?\d{4})?$", s)
    return m.group(1) if m else s


CA_PROVINCE = {"A0": "CA-AB", "A1": "CA-BC", "A2": "CA-MB", "A3": "CA-NB", "A4": "CA-NL", "A5": "CA-NS", "A6": "CA-ON",
               "A7": "CA-PE", "A8": "CA-QC", "A9": "CA-SK", "B0": "CA-YT"}


#: EDGAR foreign state/country codes (the common ones among listed filers) -> ISO 3166-1 alpha-2.
EDGAR_COUNTRY = {"X0": "GB", "F4": "CN", "K3": "HK", "E9": "KY", "D0": "BM", "C3": "AU", "J3": "GR", "L2": "IE",
                 "L3": "IL", "M0": "JP", "U0": "SG", "Q2": "NZ", "D5": "BR", "P7": "NL", "O5": "MX", "1T": "MH",
                 "V8": "CH", "I0": "FR", "2M": "DE", "N4": "LU", "V7": "SE", "N8": "MY", "T3": "ZA", "F5": "TW",
                 "Q8": "NO", "G4": "CY", "L6": "IT", "U3": "ES", "K7": "IN", "G7": "DK", "F3": "CL", "C1": "AR",
                 "R1": "PA", "M5": "KR", "D8": "VG", "C9": "BE", "H9": "FI", "Y9": "JE", "Y7": "GG", "Y8": "IM",
                 "J1": "GI", "C5": "BS", "N2": "LI", "O1": "MT", "C4": "AT", "R9": "PL", "S1": "PT", "W8": "TR",
                 "R6": "PH", "W1": "TH", "K8": "ID", "R5": "PE", "F8": "CO", "C0": "AE", "N0": "LR", "P8": "CW",
                 "K5": "HU", "2N": "CZ", "1Z": "RU", "2H": "UA", "Q1": "VN", "H2": "EG", "K6": "IS"}


def edgar_country(code: str | None) -> str | None:
    """EDGAR state / country code -> ISO country: US states -> US, Canadian provinces -> CA, else the table."""
    if not code:
        return None
    c = code.strip().upper()
    if c in CA_PROVINCE:
        return "CA"
    if re.fullmatch(r"[A-Z]{2}", c) and c not in EDGAR_COUNTRY:
        return "US"
    return EDGAR_COUNTRY.get(c)


def sec_region(code: str | None) -> str | None:
    """EDGAR state / country code -> ISO 3166-2 region (US states and Canadian provinces), else None."""
    if not code:
        return None
    c = code.strip().upper()
    if c in CA_PROVINCE:
        return CA_PROVINCE[c]
    if re.fullmatch(r"[A-Z]{2}", c):
        return f"US-{c}"
    return None


def lei_valid(lei: str | None) -> bool:
    """ISO 17442 check: 20 alphanumerics, letters -> numbers, integer mod 97 == 1."""
    if not lei or not re.fullmatch(r"[0-9A-Z]{20}", lei):
        return False
    return int("".join(str(int(c, 36)) for c in lei)) % 97 == 1


def lei_build() -> dict[str, Any]:
    """``lei.parquet`` (CIK -> LEI) and ``hierarchy.parquet`` (Level 2 parents)."""
    root = C.build_root()
    pdir = parsed_dir()
    con = C.connect(memory=MEMORY, threads=1, db_file="identity_v3_lei.duckdb")
    for fn in (norm_entity_name, postal5, sec_region, edgar_country):
        con.create_function(fn.__name__, fn, ["VARCHAR"], "VARCHAR", null_handling="special")
    con.create_function("lei_ok", lei_valid, ["VARCHAR"], "BOOLEAN", null_handling="special")
    lei2 = (pdir / "lei2.parquet").as_posix()
    rr = (pdir / "rr.parquet").as_posix()
    isin = (pdir / "isin_lei.parquet").as_posix()
    link = _link_table_path()
    filers = (root / STAGE / "filer_addresses.parquet").as_posix()
    names = (root / STAGE / "name_history.parquet").as_posix()
    hist = (root / STAGE / "cusip_history.parquet").as_posix()
    receipt: dict[str, Any] = {"link_table": link.name}
    con.execute(f"CREATE TABLE ciks AS SELECT DISTINCT cik FROM read_parquet('{link.as_posix()}')")
    # a view, not a table: 3.4 M GLEIF records stay in Parquet and each use reads only its columns
    con.execute(f"""CREATE VIEW g AS
        SELECT lei, legal_name, other_name_1, other_name_2, other_name_3, legal_city, legal_region, legal_country,
               legal_postal, hq_city, hq_region, hq_country, hq_postal, legal_jurisdiction, entity_category,
               entity_status, registration_status, successor_lei, initial_registration, last_update,
               validation_sources, ra_id, ra_entity_id
        FROM read_parquet('{lei2}')""")
    # 1. the SEC profile's own LEI field
    con.execute(f"""CREATE TABLE m_sec AS
        SELECT f.cik, upper(trim(f.lei)) AS lei FROM read_parquet('{filers}') f
        WHERE f.cik IN (SELECT cik FROM ciks) AND lei_ok(upper(trim(f.lei)))""")
    # 2. derived ISINs of the CIK's lines -> GLEIF ISIN-LEI file
    con.execute(f"""CREATE TABLE m_isin AS
        WITH x AS (
            SELECT DISTINCT l.cik, i.lei, h.isin
            FROM read_parquet('{hist}') h
            JOIN (SELECT DISTINCT security_id, cik FROM read_parquet('{link.as_posix()}')) l USING (security_id)
            JOIN read_parquet('{isin}') i ON i.isin = h.isin
            WHERE h.isin IS NOT NULL
        )
        SELECT cik, arg_max(lei, n) AS lei, count(*) AS n_leis, max(n) AS n_isins
        FROM (SELECT cik, lei, count(*) AS n FROM x GROUP BY 1, 2) GROUP BY 1""")
    # 2b. N-PORT fund-reported issuer LEI of the CIK's CUSIPs (lane OWN's nport stage, when published): the LEI
    # reported by most accessions, at least 3 of them and 80 % agreement
    nport = CH.nport_parts()
    receipt["nport_parts"] = len(nport)
    con.execute("CREATE TABLE m_nport (cik BIGINT, lei VARCHAR, n BIGINT, share DOUBLE)")
    if nport:
        glob = (root / "nport" / "parts" / "quarter=*" / "holdings.parquet").as_posix()
        con.execute(f"""INSERT INTO m_nport
            WITH x AS (
                SELECT l.cik, upper(trim(h.issuer_lei)) AS lei, count(DISTINCT h.accession) AS n
                FROM read_parquet('{glob}', hive_partitioning = false) h
                JOIN (SELECT DISTINCT cusip, security_id FROM read_parquet('{hist}') WHERE cusip_kind = 'cusip') c USING (cusip)
                JOIN (SELECT DISTINCT security_id, cik FROM read_parquet('{link.as_posix()}')) l USING (security_id)
                WHERE lei_ok(upper(trim(h.issuer_lei)))
                GROUP BY 1, 2
            )
            SELECT cik, arg_max(lei, n), max(n), max(n) / sum(n) FROM x GROUP BY 1
            HAVING max(n) >= 3 AND max(n) / sum(n) >= 0.8""")
    # 3. exact normalised name (current or former SEC names) + address or jurisdiction corroboration
    con.execute(f"""CREATE TABLE sec_names AS
        SELECT DISTINCT cik, norm_entity_name(name) AS nn FROM read_parquet('{names}')
        WHERE cik IN (SELECT cik FROM ciks) AND norm_entity_name(name) IS NOT NULL""")
    con.execute("""CREATE TABLE g_names AS
        SELECT DISTINCT lei, nn FROM (
            SELECT lei, norm_entity_name(legal_name) AS nn FROM g
            UNION ALL SELECT lei, norm_entity_name(other_name_1) FROM g WHERE other_name_1 IS NOT NULL
            UNION ALL SELECT lei, norm_entity_name(other_name_2) FROM g WHERE other_name_2 IS NOT NULL
            UNION ALL SELECT lei, norm_entity_name(other_name_3) FROM g WHERE other_name_3 IS NOT NULL)
        WHERE nn IN (SELECT nn FROM sec_names)""")
    con.execute(f"""CREATE TABLE m_name AS
        WITH f AS (SELECT * FROM read_parquet('{filers}') WHERE cik IN (SELECT cik FROM ciks)),
        c AS (
            SELECT s.cik, gn.lei, g.registration_status, g.entity_status,
                   (postal5(f.business_zipCode) IN (postal5(g.hq_postal), postal5(g.legal_postal))
                    OR postal5(f.mailing_zipCode) IN (postal5(g.hq_postal), postal5(g.legal_postal))) AS zip_match,
                   (sec_region(f.business_stateOrCountry) IN (g.hq_region, g.legal_region)
                    AND upper(f.business_city) IN (upper(g.hq_city), upper(g.legal_city))) AS city_match,
                   sec_region(f.state_of_incorporation) = g.legal_jurisdiction AS jurisdiction_match,
                   (edgar_country(f.business_stateOrCountry) IN (g.hq_country, g.legal_country)
                    AND upper(f.business_city) IN (upper(g.hq_city), upper(g.legal_city))) AS foreign_city_match
            FROM sec_names s JOIN g_names gn USING (nn) JOIN g USING (lei) LEFT JOIN f USING (cik)
        ),
        ok AS (SELECT * FROM c WHERE coalesce(zip_match, false) OR coalesce(city_match, false)
                                  OR coalesce(jurisdiction_match, false) OR coalesce(foreign_city_match, false)),
        pick AS (   -- one LEI per CIK: prefer an ISSUED registration when several corroborate
            SELECT cik, CASE WHEN count(DISTINCT lei) = 1 THEN any_value(lei)
                             WHEN count(DISTINCT lei) FILTER (WHERE registration_status = 'ISSUED') = 1
                             THEN any_value(lei) FILTER (WHERE registration_status = 'ISSUED') END AS lei,
                   bool_or(zip_match) AS zip_match, bool_or(city_match) AS city_match,
                   bool_or(jurisdiction_match) AS jurisdiction_match
            FROM ok GROUP BY 1
        )
        SELECT * FROM pick WHERE lei IS NOT NULL""")
    # combine: SEC field, then ISIN map, then name + address; an LEI claimed by two CIKs under the same method
    # is dropped for that method
    con.execute("""CREATE TABLE cand AS
        SELECT cik, lei, 'sec_submissions' AS lei_basis, 1 AS pri FROM m_sec
        UNION ALL SELECT cik, lei, 'isin_lei_map', 2 FROM m_isin WHERE n_leis = 1 OR n_isins >= 2
        UNION ALL SELECT cik, lei, 'nport_issuer_lei', 3 FROM m_nport
        UNION ALL SELECT cik, lei, 'name_address', 4 FROM m_name""")
    con.execute("""CREATE TABLE cand2 AS
        SELECT c.* FROM cand c ANTI JOIN (SELECT lei, pri FROM cand GROUP BY 1, 2 HAVING count(DISTINCT cik) > 1) d
          ON d.lei = c.lei AND d.pri = c.pri""")
    con.execute("""CREATE TABLE best AS
        SELECT cik, arg_min(lei, pri) AS lei, arg_min(lei_basis, pri) AS lei_basis,
               list(DISTINCT lei_basis ORDER BY lei_basis) AS methods, count(DISTINCT lei) AS n_distinct_leis
        FROM cand2 GROUP BY 1""")
    dest = root / STAGE / "lei.parquet"
    fetched = (gleif_ledger().latest().get("lei2") or {}).get("fetched_at")
    receipt["lei_rows"] = C.copy_to_parquet(con, f"""
        SELECT b.cik, b.lei, b.lei_basis, b.methods, b.n_distinct_leis > 1 AS methods_disagree,
               g.legal_name, g.legal_jurisdiction, g.legal_country, g.hq_country, g.entity_category, g.entity_status,
               g.registration_status, g.validation_sources, g.successor_lei,
               try_cast(left(g.initial_registration, 10) AS DATE) AS initial_registration_date,
               try_cast(left(g.last_update, 10) AS DATE) AS last_update_date,
               TIMESTAMP '{(fetched or '2026-09-29T00:00:00')[:19].replace('T', ' ')}' AS available_at,
               'snapshot_non_pit' AS vintage_risk
        FROM best b LEFT JOIN g USING (lei) ORDER BY b.cik""", dest)
    d = dest.as_posix()
    receipt["lei_coverage"] = _lei_coverage(con, d, link)
    receipt["method_counts"] = {k: int(v) for k, v in con.execute(
        "SELECT lei_basis, count(*) FROM best GROUP BY 1 ORDER BY 1").fetchall()}
    receipt["method_agreement"] = dict(zip(("ciks_two_plus_methods", "disagree"), con.execute(
        "SELECT count(*) FILTER (WHERE len(methods) > 1), count(*) FILTER (WHERE n_distinct_leis > 1) FROM best").fetchone()))
    receipt["hierarchy"] = _hierarchy(con, d, rr, root / STAGE / "hierarchy.parquet", fetched)
    con.close()
    (C.build_root() / "_tmp" / "identity_v3_lei.duckdb").unlink(missing_ok=True)
    return receipt


def _link_table_path() -> Path:
    root = C.build_root()
    v3 = root / "identity" / "link_table_v3.parquet"
    return v3 if v3.exists() else root / "identity" / "link_table.parquet"


def _lei_coverage(con, lei: str, link: Path) -> dict[str, Any]:
    root = C.build_root()
    panel = (root / "panel" / "year=*" / "*.parquet").as_posix()
    out: dict[str, Any] = {}
    sets = {"linked_ciks": f"SELECT DISTINCT cik FROM read_parquet('{link.as_posix()}')",
            "linked_ciks_pit_tiers": f"SELECT DISTINCT cik FROM read_parquet('{link.as_posix()}') WHERE link_tier <> 'backfill'",
            "member_equity_ciks": (f"SELECT DISTINCT l.cik FROM read_parquet('{link.as_posix()}') l JOIN "
                                   f"(SELECT DISTINCT security_id FROM read_parquet('{panel}', hive_partitioning = false) "
                                   f"WHERE member_equity) p USING (security_id)")}
    for label, sql in sets.items():
        try:
            n, k = con.execute(f"""SELECT count(*), count(l.lei) FROM ({sql}) c
                                   LEFT JOIN read_parquet('{lei}') l USING (cik)""").fetchone()
        except Exception as exc:
            out[label] = {"error": str(exc)[:200]}
            continue
        out[label] = {"ciks": n, "with_lei": k, "share": round(k / n, 4) if n else None}
    return out


def _hierarchy(con, lei: str, rr: str, dest: Path, fetched: str | None) -> dict[str, Any]:
    n = C.copy_to_parquet(con, f"""
        WITH r AS (
            SELECT * FROM read_parquet('{rr}')
            WHERE relationship_type IN ('IS_DIRECTLY_CONSOLIDATED_BY', 'IS_ULTIMATELY_CONSOLIDATED_BY')
        ),
        pc AS (SELECT lei, min(cik) AS parent_cik, count(*) AS n FROM read_parquet('{lei}') GROUP BY 1)
        SELECT l.cik, l.lei, CASE r.relationship_type WHEN 'IS_DIRECTLY_CONSOLIDATED_BY' THEN 'direct'
                                                     ELSE 'ultimate' END AS parent_level,
               r.parent_lei, g.legal_name AS parent_name, g.legal_country AS parent_country,
               g.legal_jurisdiction AS parent_jurisdiction, g.entity_status AS parent_entity_status,
               CASE WHEN pc.n = 1 THEN pc.parent_cik END AS parent_cik,
               r.relationship_status, r.period1_type, try_cast(left(r.period1_start, 10) AS DATE) AS period1_start,
               try_cast(left(r.period1_end, 10) AS DATE) AS period1_end, r.period2_type,
               try_cast(left(r.period2_start, 10) AS DATE) AS period2_start,
               try_cast(left(r.period2_end, 10) AS DATE) AS period2_end, r.validation_sources,
               r.registration_status AS relationship_registration_status,
               try_cast(left(r.initial_registration, 10) AS DATE) AS relationship_initial_registration,
               try_cast(left(r.last_update, 10) AS DATE) AS relationship_last_update,
               TIMESTAMP '{(fetched or '2026-09-29T00:00:00')[:19].replace('T', ' ')}' AS available_at,
               'snapshot_non_pit' AS vintage_risk
        FROM read_parquet('{lei}') l JOIN r ON r.child_lei = l.lei
        LEFT JOIN g ON g.lei = r.parent_lei LEFT JOIN pc ON pc.lei = r.parent_lei
        ORDER BY l.cik, parent_level""", dest)
    d = dest.as_posix()
    r = con.execute(f"""SELECT count(DISTINCT cik), count(DISTINCT cik) FILTER (WHERE parent_name IS NOT NULL),
                               count(*) FILTER (WHERE parent_level = 'direct'), count(*) FILTER (WHERE parent_level = 'ultimate'),
                               count(*) FILTER (WHERE parent_cik IS NOT NULL), count(*) FILTER (WHERE parent_name IS NULL)
                        FROM read_parquet('{d}')""").fetchone()
    return {"rows": n, "ciks_with_relationship": r[0], "ciks_parent_known": r[1], "direct_rows": r[2],
            "ultimate_rows": r[3], "rows_parent_cik_known": r[4], "rows_parent_name_missing": r[5]}


def publish_lei(receipt: dict[str, Any]) -> Path:
    root = C.build_root()
    led = gleif_ledger()
    files = {k: {x: v.get(x) for x in ("url", "bytes", "sha256", "fetched_at", "rows", "parsed_file", "parsed_sha256",
                                        "zip_deleted", "record_count", "publish_date", "file_name")}
             for k, v in led.latest().items() if v.get("kind") == "file"}
    manifest = {
        "schema": SCHEMA_LEI, "status": "complete", "stage": STAGE, "rule_text": __doc__,
        "code": C.code_identity(*MODULES),
        "files": C.output_hashes(root / STAGE, "lei.parquet") | C.output_hashes(root / STAGE, "hierarchy.parquet"),
        "input_manifests_sha256": {k: C.sha256_file(p) for k, p in (
            ("listing_events_manifest_sha256", root / STAGE / "listing_events_manifest.json"),
            ("cusip_history_manifest_sha256", root / STAGE / "cusip_history_manifest.json"),
            ("link_table_v3_manifest_sha256", root / "identity" / "link_table_v3_manifest.json"),
            ("link_table_manifest_sha256", root / "identity" / "link_table_manifest.json")) if p.exists()},
        "sources": {"gleif": {"ledger": str(led.path), "ledger_sha256": C.sha256_file(led.path), "files": files,
                              "terms": "GLEIF LEI data, relationship records and ISIN-LEI files: free, open data "
                                       "(CC0 1.0), published daily"}},
        "staleness": "snapshot; GLEIF record dates are kept (initial registration, last update, relationship periods)",
        "receipt": receipt,
    }
    path = root / STAGE / "lei_manifest.json"
    C.write_json_atomic(path, manifest)
    return path


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("figi-plan")
    sub.add_parser("figi-plan-followups")
    f = sub.add_parser("figi-fetch")
    f.add_argument("--pass", dest="pass_name", choices=FIGI_PASSES, required=True)
    f.add_argument("--limit", type=int)
    sub.add_parser("figi-build")
    sub.add_parser("figi-lines")
    g = sub.add_parser("gleif-fetch")
    g.add_argument("--kinds", default="lei2,rr,isin_lei")
    sub.add_parser("figi-run-early")
    sub.add_parser("gleif-parse")
    sub.add_parser("lei-build")
    args = ap.parse_args(argv)
    if args.cmd == "figi-plan":
        print(json.dumps(figi_plan()), flush=True)
    elif args.cmd == "figi-plan-followups":
        print(json.dumps(figi_plan_followups()), flush=True)
    elif args.cmd == "figi-fetch":
        print(json.dumps(figi_fetch(args.pass_name, args.limit)), flush=True)
    elif args.cmd == "figi-build":
        print(json.dumps(figi_build(), default=str), flush=True)
    elif args.cmd == "figi-lines":
        rec = figi_lines()
        print(json.dumps(rec, default=str), flush=True)
        print(publish_figi(rec), flush=True)
    elif args.cmd == "gleif-fetch":
        print(json.dumps(gleif_fetch(tuple(args.kinds.split(","))), default=str), flush=True)
        print(json.dumps(peak_memory_gb()), flush=True)
    elif args.cmd == "figi-run-early":   # pyarrow plan + network fetch: no DuckDB (ruling C-1)
        if not jobs_path("cusip").exists():
            print(json.dumps(figi_plan_arrow()), flush=True)
        print(json.dumps(figi_fetch("cusip")), flush=True)
        print(json.dumps(peak_memory_gb()), flush=True)
    elif args.cmd == "gleif-parse":
        print(json.dumps(gleif_parse(), default=str), flush=True)
        print(json.dumps(peak_memory_gb()), flush=True)
    elif args.cmd == "lei-build":
        rec = lei_build()
        print(json.dumps(rec, default=str, indent=1), flush=True)
        print(publish_lei(rec), flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
