"""Stage ``cpi_u_nsa`` (P9 AL-data request B3): monthly CPI-U, U.S. city average, all items, not seasonally adjusted
(BLS ``CUUR0000SA0``; FRED/ALFRED ``CPIAUCNS``), one row per (reference month, vintage), stamped at an evidenced conservative availability bound.

Source. ALFRED (St. Louis Fed archival FRED) vintages of ``CPIAUCNS``: the download page lists every vintage date
(the dates the series was published or revised); ``alfredgraph.csv`` returns one value column per requested vintage.
Every vintage from ``OBS_START`` on is landed (``VINTAGE_BATCH`` per request, 2020 onward first, vintages
already landed are never asked again). Cross-checks, not inputs: the latest
FRED graph CSV and the keyless BLS API v1 (``CUUR0000SA0``, its latest three years).

Rows (rule ``cpi-u-nsa-vintage-v2``). For each reference month from ``OBS_START``: a ``first_release`` row at the
first selected vintage whose column holds the month, and a ``revision`` row at every later vintage whose value
differs; an identical re-publication adds no row (counted). Nothing is overwritten. A month already present in the
first selected vintage has no observable first release and is excluded and counted, never guessed; a value missing
from a later vintage is reported (``vanished``). Reference months that no vintage holds are listed in
``coverage.missing_ref_months``.

Clock (``alfred-vintage-bound-v2``): when landed authoritative ALFRED download help establishes that a vintage
contains data as it existed on that past date, ``available_at`` is 00:00 UTC two calendar days later. This exceeds
the end of that civil date at any offset from -12 through +14. It is a bound, not an intraday release instant.
Without that evidence, use the actual response-observation timestamp, and label historical timing unavailable.
Both first observations and archival revisions retain vintage_date; exact release_date is unknown. Delivery is
PARTIAL for exact publication clocks, independently of C5/hash acceptance. Legacy cpi_u_nsa remains immutable.

Parts ``parts/year=YYYY/cpi_u_nsa.parquet`` by the UTC year of ``available_at``; C5 manifest as in
``compustat_std_parts`` (written last; an existing stage directory is never replaced).

Network (this module only; brief Task 8 requirement 4): at most ``MAX_REQUESTS`` HTTPS GETs in total, counted
cumulatively from the receipt ledger (every attempt counts, failures included), spaced at least ``MIN_INTERVAL_S``
apart, single attempt, no redirects followed, only to ``ALLOWED_HOSTS``. Every response body is landed under
``data/raw/cpi_u_nsa/`` and receipted in ``receipts.jsonl`` (url, status, bytes, SHA-256, headers, request number).
Requests 2-4 of 2026-10-05 were POSTs of the ALFRED download form (fields in their receipts) by an earlier draft of
this module: two read timeouts and one connection closed by the server; that route was dropped.

    python -m atx_db.alpha_panel.cpi_u_nsa download
    python -m atx_db.alpha_panel.cpi_u_nsa build
    python -m atx_db.alpha_panel.cpi_u_nsa accept
    python -m atx_db.alpha_panel.cpi_u_nsa fetch-url --key <key> --url <url>
"""

from __future__ import annotations

import argparse
import csv
import datetime as dt
import hashlib
import io
import json
import re
import sys
import time
import urllib.parse
from pathlib import Path
from typing import Any
from zoneinfo import ZoneInfo

from . import common as C
from . import compustat_std_parts as P
from . import shortflow_common as S

STAGE = "cpi_u_nsa_v2"
LEGACY_STAGE = "cpi_u_nsa"
SCHEMA = "atx.alpha-panel.cpi-u-nsa/v2"
RULE = "cpi-u-nsa-vintage-v2"
CLOCK_RULE = "alfred-vintage-bound-v2"
STALENESS_DAYS = 100
CONTRACT = {"schema": SCHEMA, "rule": RULE, "clock_rule": CLOCK_RULE, "staleness_days": STALENESS_DAYS}
MODULES = ("cpi_u_nsa", "compustat_std_parts", "common", "shortflow_common")
PART_FILE = "cpi_u_nsa.parquet"
CLOCK_BASIS = "alfred_civil_date_plus_two_days_utc_bound"
OBSERVED_CLOCK_BASIS = "response_observed_at_historical_timing_unavailable"
LEGACY_CONTRACT = {"schema": "atx.alpha-panel.cpi-u-nsa/v1", "rule": "cpi-u-nsa-vintage-v1",
                   "clock_rule": "alfred-vintage-date-0830-et-v1", "staleness_days": STALENESS_DAYS}
ALLOWED_HOSTS = frozenset({"api.bls.gov", "www.bls.gov", "fred.stlouisfed.org", "alfred.stlouisfed.org"})
MAX_REQUESTS = 30
MIN_INTERVAL_S = 1.1                              # <= 1 request/s
RAW = S.RAW_ROOT / "cpi_u_nsa"
FRED_GRAPH_URL = "https://fred.stlouisfed.org/graph/fredgraph.csv"
ALFRED_DOWNLOAD_URL = "https://alfred.stlouisfed.org/series/downloaddata"
ALFRED_GRAPH_URL = "https://alfred.stlouisfed.org/graph/alfredgraph.csv"
FRED_SERIES = "CPIAUCNS"
BLS_SERIES = "CUUR0000SA0"
BLS_API_URL = f"https://api.bls.gov/publicAPI/v1/timeseries/data/{BLS_SERIES}"   # keyless v1: the latest 3 years
OBS_START = dt.date(2008, 1, 1)                   # 2020+ is the delivery; earlier months are FY2009+ deflation lookback
PRIMARY_START = dt.date(2020, 1, 1)
VINTAGE_BATCH = 12                                # alfredgraph.csv serves at most 12 lines: requests 8-11 of
                                                  # 2026-10-05 asked for 60 vintages each and got the first 12
ET = ZoneInfo("America/New_York")
RELEASE_ET = dt.time(8, 30)
EXT = {"text/csv": "csv", "application/csv": "csv", "application/json": "json", "text/html": "html",
       "text/plain": "txt", "application/zip": "zip"}
COLUMNS = ("series_id", "ref_month", "value", "vintage_date", "release_date", "available_at", "vintage_kind", "clock_basis", "source")

RULE_TEXT = {
    "rows": ("per reference month from OBS_START: first_release at the first selected ALFRED vintage holding the "
             "month, a revision row at each later vintage with a different value; identical re-publications add no "
             "row; a month present in the first selected vintage is excluded (first release not observable)"),
    "clock": ("PARTIAL exact publication clock: with authoritative civil-date evidence, available_at = "
              "00:00 UTC two calendar days after vintage_date (a conservative bound for offsets -12..+14); "
              "otherwise actual response-observation time, historical timing unavailable; exact release_date unknown"),
    "staleness": (f"a consumer treats a value as stale when the session is more than {STALENESS_DAYS} days after "
                  "the end of its reference month (normal gap to the next month's release about 45 days; "
                  "coverage.observed_max_days_month_end_to_next_vintage_date records the measured maximum)"),
    "values": "index level, 1982-84 = 100, as published (three decimals); never rebased",
}


class NetworkRefused(RuntimeError):
    """A request outside the allowance (host, scheme or request budget); nothing was sent."""


# ---------------------------------------------------------------- clock
def release_instant(release_date: dt.date) -> dt.datetime:
    """08:30 America/New_York on ``release_date`` as a naive UTC TIMESTAMP."""
    return dt.datetime.combine(release_date, RELEASE_ET, ET).astimezone(dt.UTC).replace(tzinfo=None)


# ---------------------------------------------------------------- fetch
def ledger(raw: Path = RAW) -> S.Ledger:
    return S.Ledger(raw / "receipts.jsonl")


def _transport(url: str, params: dict[str, str] | None) -> tuple[int, bytes, dict[str, str]]:
    import requests

    r = requests.get(url, params=params, headers={"User-Agent": S.PUBLIC_UA}, timeout=120, allow_redirects=False)
    return int(r.status_code), r.content, {k.lower(): v for k, v in r.headers.items()}


def _sent(raw: Path) -> list[dict[str, Any]]:
    return [r for r in ledger(raw).records() if r.get("kind") == "http"]


def fetch(key: str, url: str, params: dict[str, str] | None = None, raw: Path = RAW,
          transport=_transport) -> dict[str, Any]:
    """One budgeted GET; lands the body (whatever the status) and appends its receipt."""
    full = S.full_url(url, params)
    parts = urllib.parse.urlsplit(full)
    if parts.scheme != "https" or parts.hostname not in ALLOWED_HOSTS:
        raise NetworkRefused(f"{full}: host/scheme outside the allowance {sorted(ALLOWED_HOSTS)} (https only)")
    led = ledger(raw)
    sent = _sent(raw)
    if len(sent) >= MAX_REQUESTS:
        raise NetworkRefused(f"request budget spent: {len(sent)} of {MAX_REQUESTS}")
    if sent:
        last = dt.datetime.fromisoformat(sent[-1]["fetched_at"])
        wait = MIN_INTERVAL_S - (dt.datetime.now(dt.UTC) - last).total_seconds()
        if wait > 0:
            time.sleep(min(wait, MIN_INTERVAL_S))
    rec: dict[str, Any] = {"key": key, "kind": "http", "method": "GET", "url": full, "host": parts.hostname,
                           "request_no": len(sent) + 1, "fetched_at": dt.datetime.now(dt.UTC).isoformat()}
    try:
        status, body, hdr = transport(url, params)
    except Exception as exc:                      # a failed attempt still counts against the budget
        rec.update(http_status=None, error=f"{type(exc).__name__}: {exc}")
        led.append(rec)
        raise
    ctype = (hdr.get("content-type") or "").split(";")[0].strip().lower()
    name = f"{re.sub(r'[^A-Za-z0-9_.-]', '_', key)}__{dt.datetime.now(dt.UTC):%Y%m%dT%H%M%S%fZ}.{EXT.get(ctype, 'bin')}"
    raw.mkdir(parents=True, exist_ok=True)
    (raw / (name + ".partial")).write_bytes(body)
    (raw / (name + ".partial")).replace(raw / name)
    rec.update(http_status=status, file=name, observed_at=dt.datetime.now(dt.UTC).isoformat(),
               bytes=len(body), sha256=hashlib.sha256(body).hexdigest(),
               content_type=hdr.get("content-type"), last_modified=hdr.get("last-modified"),
               location=hdr.get("location"))
    led.append(rec)
    return rec


def _landed(raw: Path, key: str) -> dict[str, Any] | None:
    rec = ledger(raw).latest().get(key)
    if not rec or rec.get("http_status") != 200 or not rec.get("file") or not (raw / rec["file"]).is_file():
        return None
    if C.sha256_file(raw / rec["file"]) != rec["sha256"]:
        raise RuntimeError(f"{raw / rec['file']}: sha256 differs from its receipt")
    return rec


# ---------------------------------------------------------------- ALFRED
def alfred_page(page: str) -> tuple[dt.date, list[dt.date]]:
    """(latest observation, every vintage date) from the ALFRED download page of the series."""
    end = re.search(r'name="form\[obs_end_date\]"[^>]*value="(\d{4}-\d{2}-\d{2})"', page)
    sel = re.search(r'name="form\[selected_vintage_dates\]\[\]"[^>]*>(.*?)</select>', page, re.S)
    if not end or not sel:
        raise ValueError("ALFRED download page: observation range or vintage list not found")
    vintages = [dt.date.fromisoformat(v) for v in re.findall(r'<option value="(\d{4}-\d{2}-\d{2})"', sel.group(1))]
    return dt.date.fromisoformat(end.group(1)), sorted(vintages)


def vintage_batches(vintages: list[dt.date], obs_start: dt.date, size: int = VINTAGE_BATCH,
                    priority_from: dt.date = PRIMARY_START) -> list[tuple[str, dict[str, str]]]:
    """``alfredgraph.csv`` GET parameters: one line per vintage on or after ``obs_start``, ``size`` per request,
    the vintages from ``priority_from`` first."""
    sel = [v for v in vintages if v >= obs_start]
    out = []
    for group in ([v for v in sel if v >= priority_from], [v for v in sel if v < priority_from]):
        for i in range(0, len(group), size):
            chunk = group[i:i + size]
            out.append((f"alfred_vintages_{chunk[0]}_{chunk[-1]}",
                        {"id": ",".join([FRED_SERIES] * len(chunk)), "vintage_date": ",".join(map(str, chunk)),
                         "cosd": ",".join([obs_start.isoformat()] * len(chunk))}))
    return out


def _batch_recs(raw: Path) -> list[dict[str, Any]]:
    """Every ALFRED batch landed with HTTP 200 (SHA-256 re-verified) whose body parses."""
    out = []
    for k in sorted(k for k in ledger(raw).latest() if k.startswith("alfred_vintages_")):
        rec = _landed(raw, k)
        if rec:
            try:
                rec = rec | {"vintages": parse_alfred_csv((raw / rec["file"]).read_text(encoding="utf-8"))[0]}
            except (ValueError, IndexError):
                continue
            out.append(rec)
    return out


def _num(s: str | None) -> float | None:
    s = (s or "").strip()
    return None if s in ("", ".", "-") else float(s)


def parse_alfred_csv(text: str) -> tuple[list[dt.date], dict[dt.date, list[float | None]]]:
    """(vintage dates, {observation month: value per vintage}) of an ``alfredgraph.csv`` body."""
    rows = list(csv.reader(io.StringIO(text)))
    head = rows[0]
    if head[0] != "observation_date" or not all(re.fullmatch(rf"{FRED_SERIES}_\d{{8}}", h) for h in head[1:]):
        raise ValueError(f"unexpected ALFRED header {head[:4]}")
    vint = [dt.datetime.strptime(h.rsplit("_", 1)[1], "%Y%m%d").date() for h in head[1:]]
    obs = {dt.date.fromisoformat(r[0]): [_num(x) for x in r[1:]] + [None] * (len(vint) + 1 - len(r))
           for r in rows[1:] if r}
    return vint, obs


def merge_vintages(parsed) -> tuple[list[dt.date], dict[dt.date, list[float | None]]]:
    """Batches -> one vintage axis; the same (vintage, month) cell from two batches must agree."""
    by: dict[dt.date, dict[dt.date, float | None]] = {}
    for vint, obs in parsed:
        for m, vals in obs.items():
            cell = by.setdefault(m, {})
            for v, x in zip(vint, vals):
                if v in cell and cell[v] != x:
                    raise ValueError(f"vintage {v} month {m}: two batches disagree ({cell[v]} vs {x})")
                cell[v] = x
    allv = sorted({v for vint, _ in parsed for v in vint})
    return allv, {m: [cell.get(v) for v in allv] for m, cell in sorted(by.items())}


def vintage_bound(vintage_date: dt.date) -> dt.datetime:
    """Controller CPI-1 bound; call only after proving civil-date availability semantics."""
    return dt.datetime.combine(vintage_date + dt.timedelta(days=2), dt.time())


def clock_evidence(raw: Path) -> tuple[bool, dict[str, Any]]:
    """Re-verify landed authoritative help before using its civil-date semantics."""
    rec = _landed(raw, "alfred_clock_downloadhelp_r1")
    if rec and rec.get("url") == "https://alfred.stlouisfed.org/help/downloaddata":
        import html

        text = html.unescape(re.sub(r"<[^>]+>", " ", (raw / rec["file"]).read_text(encoding="utf-8")))
        text = re.sub(r"\s+", " ", text)
        premise = "A vintage date is a date in history used to download data as it actually existed on that past date."
        if premise in text:
            return True, {"status": "civil_date_semantics_verified", "source": _input(rec),
                          "premise": "ALFRED download help defines vintage data as it existed on that past date",
                          "bound": "00:00 UTC two calendar days after vintage_date; offsets -12..+14",
                          "exact_release_time": "unproved", "delivery": "PARTIAL"}
    return False, {"status": "civil_date_semantics_unverified", "delivery": "PARTIAL",
                   "bound": "actual response observation timestamp", "historical_timing": "unavailable"}


def response_observed_at(rec: dict[str, Any]) -> dt.datetime:
    """Receipt observation instant, or legacy filename timestamp created after body retrieval.

    fetched_at is request-start time and cannot alone establish value availability.
    """
    if rec.get("observed_at"):
        return dt.datetime.fromisoformat(rec["observed_at"]).astimezone(dt.UTC).replace(tzinfo=None)
    match = re.search(r"__(\d{8}T\d{12}Z)\.", rec.get("file", ""))
    if not match:
        raise RuntimeError("no proven response-observation timestamp in receipt")
    return dt.datetime.strptime(match.group(1), "%Y%m%dT%H%M%S%fZ")


def vintage_rows(vintages: list[dt.date], obs: dict[dt.date, list[float | None]],
                 start: dt.date, clock_bounds: dict[dt.date, dt.datetime],
                 clock_basis: str) -> tuple[list[dict[str, Any]], dict[str, Any]]:
    """First-release and revision rows per reference month on or after ``start`` (see the module doc)."""
    rows: list[dict[str, Any]] = []
    info: dict[str, Any] = {"unchanged_republications": 0, "vanished": [], "excluded_first_in_selection": []}
    for m in sorted(obs):
        if m < start:
            continue
        vals = obs[m]
        first = next((i for i, x in enumerate(vals) if x is not None), None)
        if first is None:
            continue
        if first == 0:
            info["excluded_first_in_selection"].append(m)
            continue
        prev = None
        for i in range(first, len(vals)):
            x = vals[i]
            if x is None:
                info["vanished"].append((m, vintages[i]))
                continue
            if prev is not None and x == prev:
                info["unchanged_republications"] += 1
                continue
            rows.append({"series_id": BLS_SERIES, "ref_month": m, "value": x, "vintage_date": vintages[i],
                         "release_date": None, "available_at": clock_bounds[vintages[i]],
                         "vintage_kind": "first_release" if prev is None else "revision",
                         "clock_basis": clock_basis, "source": f"alfred:{FRED_SERIES}"})
            prev = x
    return rows, info


def download(raw: Path = RAW, transport=_transport, obs_start: dt.date = OBS_START,
             batch: int = VINTAGE_BATCH) -> dict[str, Any]:
    """Land the ALFRED vintages and the two cross-checks; files already landed with HTTP 200 are reused, so a
    rerun sends nothing."""
    before = len(_sent(raw))
    page = _landed(raw, "alfred_downloaddata_page") or fetch(
        "alfred_downloaddata_page", ALFRED_DOWNLOAD_URL, {"seid": FRED_SERIES}, raw=raw, transport=transport)
    if page.get("http_status") != 200:
        raise RuntimeError(f"ALFRED download page: HTTP {page.get('http_status')}")
    latest, vintages = alfred_page((raw / page["file"]).read_text(encoding="utf-8"))
    stalled = False
    while True:
        have = {v for r in _batch_recs(raw) for v in r["vintages"]}
        remaining = [v for v in vintages if v >= obs_start and v not in have]
        if not remaining:
            break
        key, params = vintage_batches(remaining, obs_start, batch)[0]
        fetch(key, ALFRED_GRAPH_URL, params, raw=raw, transport=transport)
        if not {v for r in _batch_recs(raw) for v in r["vintages"]} - have:
            stalled = True                        # the server returned no new vintage: stop, never loop on it
            break
    if not _landed(raw, "fred_graph_latest"):
        fetch("fred_graph_latest", FRED_GRAPH_URL, {"id": FRED_SERIES, "cosd": obs_start.isoformat()}, raw=raw,
              transport=transport)
    if not _landed(raw, "bls_api_v1_latest"):
        fetch("bls_api_v1_latest", BLS_API_URL, raw=raw, transport=transport)
    total = len(_sent(raw))
    have = {v for r in _batch_recs(raw) for v in r["vintages"]}
    return {"requests_sent": total - before, "requests_total": total, "latest_observation": latest,
            "vintages_listed": len(vintages), "vintages_requested": sum(v >= obs_start for v in vintages),
            "vintages_missing": [str(v) for v in vintages if v >= obs_start and v not in have], "stalled": stalled,
            "landed": {k: v.get("http_status") for k, v in ledger(raw).latest().items()}}


# ---------------------------------------------------------------- build
def _bls_values(blob: bytes) -> dict[dt.date, float | None]:
    js = json.loads(blob)
    out = {}
    for s in js.get("Results", {}).get("series", []):
        if s.get("seriesID") != BLS_SERIES:
            continue
        for d in s.get("data", []):
            mt = re.fullmatch(r"M(0[1-9]|1[0-2])", d.get("period", ""))
            if mt:
                out[dt.date(int(d["year"]), int(mt.group(1)), 1)] = _num(d.get("value"))
    return out


def _fred_values(blob: bytes) -> dict[dt.date, float | None]:
    rows = list(csv.reader(io.StringIO(blob.decode("utf-8"))))
    if rows[0] != ["observation_date", FRED_SERIES]:
        raise ValueError(f"unexpected FRED header {rows[0]}")
    return {dt.date.fromisoformat(r[0]): _num(r[1]) for r in rows[1:] if r}


def _compare(latest: dict[dt.date, float], other: dict[dt.date, float | None], name: str) -> dict[str, Any]:
    """The latest ALFRED value per month against another source, over the months both hold."""
    both = sorted(m for m in other if m in latest and other[m] is not None)
    lo = min(other, default=None)
    return {"compared": len(both), "first": str(both[0]) if both else None, "last": str(both[-1]) if both else None,
            "mismatches": [{"ref_month": str(m), "alfred_latest": latest[m], name: other[m]} for m in both
                           if abs(latest[m] - other[m]) > 5e-4],
            "only_in_source": [str(m) for m in sorted(other) if m not in latest and other[m] is not None
                               and m >= min(latest, default=m)],
            "blank_in_source": [str(m) for m in sorted(other) if other[m] is None],
            "missing_in_source": [str(m) for m in sorted(latest) if lo is not None and m >= lo and m not in other]}


def _next_month(m: dt.date) -> dt.date:
    return dt.date(m.year + m.month // 12, m.month % 12 + 1, 1)


def _months(a: dt.date, b: dt.date) -> list[dt.date]:
    out, m = [], a
    while m <= b:
        out.append(m)
        m = _next_month(m)
    return out


def _input(rec: dict[str, Any]) -> dict[str, Any]:
    return {k: rec.get(k) for k in ("key", "url", "request_no", "fetched_at", "http_status", "bytes", "sha256",
                                    "last_modified")}


def _write_part(path: Path, rows: list[dict[str, Any]]) -> None:
    import pyarrow as pa
    import pyarrow.parquet as pq

    schema = pa.schema([("series_id", pa.string()), ("ref_month", pa.date32()), ("value", pa.float64()),
                        ("vintage_date", pa.date32()), ("release_date", pa.date32()), ("available_at", pa.timestamp("us")),
                        ("vintage_kind", pa.string()), ("clock_basis", pa.string()), ("source", pa.string())])
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(path.name + ".partial")
    pq.write_table(pa.Table.from_pylist([{c: r[c] for c in COLUMNS} for r in rows], schema=schema), tmp,
                   compression="zstd")
    tmp.replace(path)


def build(raw: Path = RAW, obs_start: dt.date = OBS_START, stage: str = STAGE) -> dict[str, Any]:
    """Offline: landed receipts (SHA-256 re-verified) -> year parts, then ``manifest.json`` last."""
    out = C.build_root() / stage
    if out.exists() and any(out.iterdir()):
        raise RuntimeError(f"{out} already exists: published outputs are never replaced; build a new --stage name")
    page = _landed(raw, "alfred_downloaddata_page")
    if not page:
        raise RuntimeError("no landed ALFRED download page (run download)")
    latest_obs, listed = alfred_page((raw / page["file"]).read_text(encoding="utf-8"))
    recs = _batch_recs(raw)
    vintages, obs = merge_vintages([parse_alfred_csv((raw / r["file"]).read_text(encoding="utf-8")) for r in recs])
    if missing := sorted({v for v in listed if v >= obs_start} - set(vintages)):
        raise RuntimeError(f"{len(missing)} listed vintages not landed, e.g. {missing[:3]}")
    keep = [i for i, v in enumerate(vintages) if v >= obs_start]
    used = [vintages[i] for i in keep]
    civil_proven, evidence = clock_evidence(raw)
    if civil_proven:
        bounds = {v: vintage_bound(v) for v in used}
    else:
        bounds = {v: min(response_observed_at(r) for r in recs if v in r["vintages"]) for v in used}
    rows, info = vintage_rows(used, {m: [vals[i] for i in keep] for m, vals in obs.items()}, obs_start,
                              bounds, CLOCK_BASIS if civil_proven else OBSERVED_CLOCK_BASIS)
    latest: dict[dt.date, float] = {}
    for r in rows:
        latest[r["ref_month"]] = r["value"]
    first = {r["ref_month"]: r for r in rows if r["vintage_kind"] == "first_release"}
    ordered = sorted(first)
    # days from the last day of month a to the first release of the next month that has one
    gaps = [((first[b]["vintage_date"] - _next_month(a)).days + 1, str(a)) for a, b in zip(ordered, ordered[1:])]
    inputs = {page["file"]: _input(page)} | {r["file"]: _input(r) | {
        "vintages_requested": len([v for x in urllib.parse.parse_qs(urllib.parse.urlsplit(r["url"]).query)
                                   .get("vintage_date", []) for v in x.split(",") if v]),
        "vintages_returned": len(r["vintages"]), "first_vintage": str(r["vintages"][0]),
        "last_vintage": str(r["vintages"][-1])} for r in recs}
    if civil_proven:
        proof = _landed(raw, "alfred_clock_downloadhelp_r1")
        inputs[proof["file"]] = _input(proof)
    crosscheck: dict[str, Any] = {}
    for key, name, parse in (("fred_graph_latest", "fred_latest", _fred_values),
                             ("bls_api_v1_latest", "bls_api_v1", _bls_values)):
        rec = _landed(raw, key)
        if not rec:
            crosscheck[name] = {"status": "not_landed"}
            continue
        inputs[rec["file"]] = _input(rec)
        crosscheck[name] = _compare(latest, parse((raw / rec["file"]).read_bytes()), name.split("_api")[0])
    by_year: dict[int, list[dict[str, Any]]] = {}
    for r in sorted(rows, key=lambda r: (r["available_at"], r["ref_month"])):
        by_year.setdefault(r["available_at"].year, []).append(r)
    files = {}
    for y, part in sorted(by_year.items()):
        rel = f"parts/year={y}/{PART_FILE}"
        _write_part(out / rel, part)
        files[rel] = {"bytes": (out / rel).stat().st_size, "sha256": C.sha256_file(out / rel)}
    led = ledger(raw)
    manifest = {
        "schema": SCHEMA, "status": "complete", "stage": stage, "rule": RULE, "clock_rule": CLOCK_RULE,
        "staleness_days": STALENESS_DAYS, "files": files, "columns": list(COLUMNS), "code": C.code_identity(*MODULES),
        "inputs": inputs,
        "raw_ledger": {"path": str(led.path), "sha256": C.sha256_file(led.path), "http_requests": len(_sent(raw)),
                       "budget": MAX_REQUESTS},
        "counts": {"rows": len(rows), "first_release": len(first), "revision": len(rows) - len(first),
                   "unchanged_republications": info["unchanged_republications"],
                   "per_part": {str(y): len(p) for y, p in sorted(by_year.items())}},
        "excluded": [{"reason": "month already present in the first selected vintage: first release not observable",
                      "ref_months": [str(m) for m in info["excluded_first_in_selection"]]}],
        "vanished": [{"ref_month": str(m), "vintage_date": str(v)} for m, v in info["vanished"]],
        "coverage": {"obs_start": str(obs_start), "first_ref_month": str(ordered[0]) if ordered else None,
                     "latest_ref_month": str(ordered[-1]) if ordered else None,
                     "latest_vintage": str(used[-1]) if used else None,
                     "alfred_page_latest_observation": str(latest_obs), "vintages_listed": len(listed),
                     "vintages_used": len(used),
                     "missing_ref_months": [str(m) for m in (_months(ordered[0], ordered[-1]) if ordered else [])
                                            if m not in first],
                     "primary_window_ref_months": sum(m >= PRIMARY_START for m in first),
                     "observed_max_days_month_end_to_next_vintage_date": max(gaps) if gaps else None},
        "crosscheck": crosscheck, "rule_text": RULE_TEXT, "clock_evidence": evidence,
        "exact_clock_requirement": "PARTIAL",
        "built_at_utc": dt.datetime.now(dt.UTC).isoformat(timespec="seconds"),
    }
    C.write_json_atomic(out / "manifest.json", manifest)
    return {"stage": str(out), "manifest_sha256": C.sha256_file(out / "manifest.json"), **manifest["counts"],
            "coverage": manifest["coverage"], "crosscheck": crosscheck}



# ---------------------------------------------------------------- accept
def accept(stage: str = STAGE) -> tuple[bool, list[str]]:
    """(i)+(iv) C5 self-check (``compustat_std_parts.check_stage``) plus coverage, cross-checks and the 2020+ rows."""
    d = C.build_root() / stage
    contract = LEGACY_CONTRACT if stage == LEGACY_STAGE else CONTRACT
    chk = P.check_stage(d, contract)
    m = C.read_json(d / "manifest.json")
    out = [f"$ python -m atx_db.alpha_panel.cpi_u_nsa accept --stage {stage}", "",
           "(i) C5 manifest self-check (engine PinnedStage contract; every listed file re-hashed)",
           f"  manifest {chk['manifest']} bytes={chk['manifest_bytes']} sha256={chk['manifest_sha256']}",
           f"  contract errors={len(chk['contract_errors'])} {chk['contract_errors']}",
           f"  files listed={chk['files']} verified={chk['verified']} bad={len(chk['bad'])} {chk['bad']} "
           f"unlisted={len(chk['unlisted'])} {chk['unlisted']}", "",
           "(iv) part year check (available_at inside the part's UTC year)"]
    out += [f"  {rel} rows={p['rows']} min={p['min']} max={p['max']} outside_year={p['outside_year']}"
            for rel, p in chk["parts"].items()]
    out += [f"  rows_outside_year={chk['rows_outside_year']}  (i)+(iv) {'PASS' if chk['ok'] else 'FAIL'}", ""]
    if not chk["ok"]:
        return False, out
    out += ["exact publication-clock requirement: PARTIAL (C5 PASS is separate)",
            "  clock evidence: " + json.dumps(m.get("clock_evidence", {"status": "legacy_unproved_0830_clock"})),
            ("  legacy available_at uses unproved 08:30 stamps; preserved for provenance, superseded by cpi_u_nsa_v2"
             if stage == LEGACY_STAGE else
             "  release_date unknown; vintage_date retained; available_at is a bound, not an exact release instant")]
    cov = m["coverage"]
    out += [f"coverage: ref months {cov['first_ref_month']} .. {cov['latest_ref_month']} (latest vintage "
            f"{cov['latest_vintage']}; ALFRED page latest observation {cov['alfred_page_latest_observation']}); "
            f"{cov['vintages_used']} of {cov['vintages_listed']} listed vintages used (from {cov['obs_start']}); "
            f"the year of the latest vintage is partial",
            f"  missing ref months (no vintage holds them): {cov['missing_ref_months']}",
            f"  counts {m['counts']}", f"  excluded {m['excluded']}", f"  vanished {m['vanished']}",
            f"  observed max days from month end to the next month's first vintage date: "
            f"{cov.get('observed_max_days_month_end_to_next_vintage_date', cov.get('observed_max_days_month_end_to_next_release'))} "
            f"(staleness_days {m['staleness_days']})",
            f"  network: {m['raw_ledger']['http_requests']} of {m['raw_ledger']['budget']} requests "
            f"(ledger {m['raw_ledger']['path']} sha256 {m['raw_ledger']['sha256']})"]
    out += [f"  crosscheck {name}: " + json.dumps(c, default=str) for name, c in m["crosscheck"].items()]
    con = C.connect(memory="200MB", threads=1)
    try:
        src = "read_parquet([" + ", ".join(f"'{P._sql_path(d / rel)}'" for rel in sorted(m["files"])) + "])"
        sql = (f"SELECT year(ref_month), count(*) FILTER (WHERE vintage_kind = 'first_release'), "
               f"count(*) FILTER (WHERE vintage_kind = 'revision'), min(available_at), max(available_at), "
               f"string_agg(DISTINCT clock_basis, ',') FROM {src} WHERE ref_month >= DATE '{PRIMARY_START}' "
               "GROUP BY 1 ORDER BY 1")
        out += ["", f"primary window {PRIMARY_START.year}+ by reference year", f"  sql: {sql}",
                "  ref_year first_release revision min_available_at max_available_at clock_basis"]
        out += ["  " + " ".join(str(x) for x in r) for r in con.execute(sql).fetchall()]
        sql = (f"SELECT ref_month, vintage_date, available_at, value, vintage_kind, clock_basis FROM {src} "
               f"WHERE ref_month >= DATE '{PRIMARY_START}' ORDER BY ref_month, available_at")
        out += ["", f"rows {PRIMARY_START}+ (ref_month vintage_date available_at_utc value kind clock_basis)", f"  sql: {sql}"]
        out += ["  " + " ".join(str(x) for x in r) for r in con.execute(sql).fetchall()]
    finally:
        con.close()
    return chk["ok"], out


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("download")
    for name in ("build", "accept"):
        sub.add_parser(name).add_argument("--stage", default=STAGE)
    f = sub.add_parser("fetch-url")
    f.add_argument("--key", required=True)
    f.add_argument("--url", required=True)
    args = ap.parse_args(argv)
    if args.cmd == "download":
        print(json.dumps(download(), indent=2, default=str))
        return 0
    if args.cmd == "build":
        print(json.dumps(build(stage=args.stage), indent=2, default=str))
        return 0
    if args.cmd == "accept":
        ok, lines = accept(args.stage)
        print("\n".join(lines))
        return 0 if ok else 1
    rec = fetch(args.key, args.url)
    print(json.dumps(rec, indent=2, default=str))
    return 0 if rec.get("http_status") == 200 else 1


if __name__ == "__main__":
    sys.exit(main())
