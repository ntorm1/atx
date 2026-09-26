#!/usr/bin/env python
"""Fetch the public external benchmark files (node X.3, gate U6 / RX14, ruling C-42).

Sources (all public, none are SEC):

* Ken French data library -- Fama-French 5 factors 2x3 (monthly, daily), momentum (monthly, daily),
  NYSE ME breakpoints.
* JKP (jkpfactors.com) -- US factor returns, all 153 factors, monthly, capped value weights; plus the
  JKP NYSE size cutoffs file (supplemental, kept raw).
* Open Source Asset Pricing (openassetpricing.com, release 2.0.0) -- ``SignalDoc.csv``, the full predictor
  portfolio file ``PredictorPortsFull.csv`` and the long-short wide file ``PredictorLSretWide.csv``.
* global-q (global-q.org) -- q5 factors, monthly. The file name carries the release year, so the current
  link is discovered from the factors page, which is kept as evidence.

Politeness: one request at a time, ``--pause`` seconds between requests, a descriptive user agent that
contains no personal email, at most two retries (5xx / 429 / connection errors), ``Retry-After`` honoured.
Nothing behind a login is attempted: a response that is not the expected payload (an HTML page where a
zip/CSV was expected, a 401/403/404) is recorded as ``<file>.unavailable.json`` with the evidence and the
dataset that depends on it falls back (the loader labels it unavailable). Data is never hand-constructed.

Layout::

    data/raw/benchmarks/<source>/<UTC date>/<file>                  downloaded bytes, unmodified
    data/raw/benchmarks/<source>/<UTC date>/<file>.receipt.json     url, final url, UTC times, bytes, sha256
    data/raw/benchmarks/<source>/<UTC date>/<file>.unavailable.json evidence when a file could not be fetched
    data/raw/benchmarks/_runs/<UTC timestamp>.json                  one summary per run

After the downloads (unless ``--no-parse``) the parsed datasets are built by
``atx_db.research.benchmarks_load.build_benchmark_datasets`` into ``data/raw/benchmarks/parsed/``.
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
import zipfile
from collections.abc import Callable, Mapping
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

os.environ.setdefault("OPENBLAS_NUM_THREADS", "1")

import requests

USER_AGENT = "atx-db/0.1 benchmark-fetch (non-commercial academic research; public benchmark files)"
DEFAULT_PAUSE_S = 3.0
TIMEOUT_S = (20, 120)  # connect, read
CHUNK = 1 << 16
MAX_RETRIES = 2
RETRY_BACKOFF_S = (10.0, 30.0)
BODY_EVIDENCE_CHARS = 1500

FRENCH_BASE = "https://mba.tuck.dartmouth.edu/pages/faculty/ken.french/ftp/"
JKP_S3 = "https://jkpfactors-data.s3.amazonaws.com/public/"
OSAP_DATA_PAGE = "https://www.openassetpricing.com/data/"
GDRIVE_DOWNLOAD = "https://drive.google.com/uc?export=download&id={file_id}"
GLOBALQ_BASE = "https://global-q.org"
GLOBALQ_FACTORS_PAGE = GLOBALQ_BASE + "/factors.html"
GLOBALQ_Q5_MONTHLY = re.compile(r'href="(/uploads/[^"]*/q5_factors_monthly_(\d{4})\.csv)"')


@dataclass(frozen=True)
class Target:
    """One file to download. ``url`` is ``None`` when it is discovered from an earlier page."""

    source: str
    name: str
    kind: str  # zip | csv | html | json
    role: str
    url: str | None = None
    expect_filename: str | None = None  # Content-Disposition name that must match (Google Drive)
    drive_id: str | None = None


@dataclass
class Outcome:
    target: Target
    status: str  # fetched | skipped_existing | unavailable
    path: Path | None = None
    receipt: dict[str, Any] = field(default_factory=dict)
    evidence: dict[str, Any] = field(default_factory=dict)


def _french(name: str, role: str) -> Target:
    return Target("french", name, "zip", role, url=FRENCH_BASE + name)


def _osap(name: str, drive_id: str, role: str) -> Target:
    return Target(
        "osap",
        name,
        "csv",
        role,
        url=GDRIVE_DOWNLOAD.format(file_id=drive_id),
        expect_filename=name,
        drive_id=drive_id,
    )


# OSAP Google Drive file ids as linked from OSAP_DATA_PAGE (release 2.0.0); the page is fetched first and
# each receipt records whether its id is still linked from it.
OSAP_SIGNALDOC_ID = "1Sev9s6cPFUGgxp1pFiej0lGzpsMqJCI2"
OSAP_PORTS_FULL_ID = "1g7w-yQ6Cg2qbMEkER9Q3vgns4JszXQo6"
OSAP_LS_WIDE_ID = "10sOryk_ddjkXagaajTKUk1nwJs2ZLRiI"

TARGETS: dict[str, tuple[Target, ...]] = {
    "french": (
        _french("F-F_Research_Data_5_Factors_2x3_CSV.zip", "bench_french_ff5_umd_monthly (FF5 monthly)"),
        _french("F-F_Momentum_Factor_CSV.zip", "bench_french_ff5_umd_monthly (UMD monthly)"),
        _french("ME_Breakpoints_CSV.zip", "bench_french_me_breakpoints"),
        _french("F-F_Research_Data_5_Factors_2x3_daily_CSV.zip", "raw only (FF5 daily)"),
        _french("F-F_Momentum_Factor_daily_CSV.zip", "raw only (UMD daily)"),
    ),
    "jkp": (
        Target(
            "jkp",
            "usa_all_factors_monthly_vw_cap.zip",
            "zip",
            "bench_jkp_us_factors",
            url=JKP_S3 + "%5Busa%5D_%5Ball_factors%5D_%5Bmonthly%5D_%5Bvw_cap%5D.zip",
        ),
        Target(
            "jkp", "nyse_cutoffs.csv", "csv", "raw only (JKP NYSE size cutoffs)", url=JKP_S3 + "other/nyse_cutoffs.csv"
        ),
    ),
    "osap": (
        Target("osap", "data_page.html", "html", "evidence (Google Drive ids)", url=OSAP_DATA_PAGE),
        _osap("SignalDoc.csv", OSAP_SIGNALDOC_ID, "bench_osap_signaldoc"),
        _osap("PredictorPortsFull.csv", OSAP_PORTS_FULL_ID, "bench_osap_portfolios"),
        _osap("PredictorLSretWide.csv", OSAP_LS_WIDE_ID, "cross-check of bench_osap_portfolios LS rows"),
    ),
    "globalq": (
        Target("globalq", "factors.html", "html", "evidence (q5 monthly link discovery)", url=GLOBALQ_FACTORS_PAGE),
        Target("globalq", "q5_factors_monthly.csv", "csv", "bench_q5"),
    ),
}


def _utc_now() -> dt.datetime:
    return dt.datetime.now(dt.UTC).replace(microsecond=0)


def _iso(ts: dt.datetime) -> str:
    return ts.strftime("%Y-%m-%dT%H:%M:%SZ")


def _sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(CHUNK), b""):
            digest.update(block)
    return digest.hexdigest()


def _peak_private_mb() -> float | None:
    """Peak committed (pagefile-backed) bytes of this process, Windows only."""
    if os.name != "nt":
        return None
    import ctypes
    from ctypes import wintypes

    class _Counters(ctypes.Structure):
        _fields_ = [
            ("cb", wintypes.DWORD),
            ("PageFaultCount", wintypes.DWORD),
            ("PeakWorkingSetSize", ctypes.c_size_t),
            ("WorkingSetSize", ctypes.c_size_t),
            ("QuotaPeakPagedPoolUsage", ctypes.c_size_t),
            ("QuotaPagedPoolUsage", ctypes.c_size_t),
            ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t),
            ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
            ("PagefileUsage", ctypes.c_size_t),
            ("PeakPagefileUsage", ctypes.c_size_t),
            ("PrivateUsage", ctypes.c_size_t),
        ]

    kernel32 = ctypes.WinDLL("kernel32")
    kernel32.GetCurrentProcess.restype = wintypes.HANDLE
    psapi = ctypes.WinDLL("psapi")
    psapi.GetProcessMemoryInfo.argtypes = [wintypes.HANDLE, ctypes.POINTER(_Counters), wintypes.DWORD]
    psapi.GetProcessMemoryInfo.restype = wintypes.BOOL
    counters = _Counters()
    counters.cb = ctypes.sizeof(counters)
    if not psapi.GetProcessMemoryInfo(kernel32.GetCurrentProcess(), ctypes.byref(counters), counters.cb):
        return None
    return round(counters.PeakPagefileUsage / 2**20, 1)


class PoliteSession:
    """Serial HTTP client: one request in flight, a pause before every request after the first."""

    def __init__(self, pause_s: float) -> None:
        self._session = requests.Session()
        self._session.headers.update({"User-Agent": USER_AGENT, "Accept-Encoding": "gzip, deflate"})
        self._pause_s = pause_s
        self._last: float | None = None
        self.requests_made = 0

    def _wait(self, extra_s: float = 0.0) -> None:
        if self._last is not None:
            remaining = self._pause_s + extra_s - (time.monotonic() - self._last)
            if remaining > 0:
                time.sleep(remaining)

    def get(self, url: str) -> requests.Response:
        """GET with streaming; retries only transient failures (5xx, 429, connection errors)."""
        extra = 0.0
        last_error: Exception | None = None
        for attempt in range(MAX_RETRIES + 1):
            self._wait(extra)
            self._last = time.monotonic()
            self.requests_made += 1
            try:
                response = self._session.get(url, stream=True, timeout=TIMEOUT_S, allow_redirects=True)
            except requests.RequestException as exc:  # connection reset, timeout, DNS
                last_error = exc
                if attempt < MAX_RETRIES:
                    extra = RETRY_BACKOFF_S[attempt]
                    continue
                raise
            if response.status_code in (429, 500, 502, 503, 504) and attempt < MAX_RETRIES:
                retry_after = response.headers.get("Retry-After", "")
                extra = float(retry_after) if retry_after.isdigit() else RETRY_BACKOFF_S[attempt]
                response.close()
                continue
            return response
        raise RuntimeError(f"unreachable retry state for {url}: {last_error}")

    def mark_done(self) -> None:
        self._last = time.monotonic()


def _validate_payload(path: Path, target: Target, headers: Mapping[str, str]) -> tuple[bool, dict[str, Any]]:
    """Check the bytes are the expected kind of file (never an HTML login / interstitial page)."""
    detail: dict[str, Any] = {}
    with path.open("rb") as handle:
        head = handle.read(4096)
    looks_html = head.lstrip()[:15].lower().startswith((b"<!doctype html", b"<html"))
    if target.expect_filename is not None:
        disposition = headers.get("Content-Disposition", "")
        detail["content_disposition"] = disposition
        if f'filename="{target.expect_filename}"' not in disposition:
            return False, detail | {"reason": f"Content-Disposition does not name {target.expect_filename}"}
    if target.kind == "zip":
        if not zipfile.is_zipfile(path):
            return False, detail | {"reason": "payload is not a zip archive", "looks_html": looks_html}
        with zipfile.ZipFile(path) as archive:
            bad = archive.testzip()
            members = [(info.filename, info.file_size) for info in archive.infolist()]
        if bad is not None:
            return False, detail | {"reason": f"zip member failed CRC: {bad}"}
        return True, detail | {"zip_members": members}
    if target.kind == "csv":
        if looks_html:
            return False, detail | {"reason": "expected CSV, received an HTML page"}
        first_line = head.split(b"\n", 1)[0].decode("utf-8", errors="replace").strip()
        if "," not in first_line:
            return False, detail | {"reason": "first line has no comma", "first_line": first_line[:300]}
        return True, detail | {"first_line": first_line[:300]}
    if target.kind == "json":
        try:
            json.loads(path.read_text(encoding="utf-8"))
        except ValueError as exc:
            return False, detail | {"reason": f"invalid JSON: {exc}"}
        return True, detail
    return True, detail | {"looks_html": looks_html}


def _write_json(path: Path, payload: dict[str, Any]) -> None:
    tmp = path.with_name(path.name + ".tmp")
    tmp.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    os.replace(tmp, path)


def _existing_ok(path: Path) -> dict[str, Any] | None:
    receipt_path = path.with_name(path.name + ".receipt.json")
    if not (path.is_file() and receipt_path.is_file()):
        return None
    receipt = json.loads(receipt_path.read_text(encoding="utf-8"))
    if receipt.get("sha256") != _sha256_file(path):
        return None
    return receipt


def fetch_target(session: PoliteSession, target: Target, url: str, out_dir: Path, refetch: bool) -> Outcome:
    out_dir.mkdir(parents=True, exist_ok=True)
    path = out_dir / target.name
    unavailable_path = path.with_name(path.name + ".unavailable.json")
    if not refetch:
        existing = _existing_ok(path)
        if existing is not None:
            return Outcome(target, "skipped_existing", path, receipt=existing)
    started = _utc_now()
    base_evidence = {
        "source": target.source,
        "name": target.name,
        "role": target.role,
        "url": url,
        "attempted_at_utc": _iso(started),
        "user_agent": USER_AGENT,
    }
    try:
        response = session.get(url)
    except requests.RequestException as exc:
        evidence = base_evidence | {"status": "unavailable", "reason": f"request failed: {exc!r}"}
        _write_json(unavailable_path, evidence)
        return Outcome(target, "unavailable", evidence=evidence)
    headers = response.headers  # case-insensitive: some servers send lower-case names
    kept_headers = {
        key: headers[key]
        for key in ("Content-Type", "Content-Length", "Last-Modified", "ETag", "Content-Disposition", "Location")
        if key in headers
    }
    history = [{"status": item.status_code, "url": item.url} for item in response.history]
    if response.status_code != 200:
        body = response.text[:BODY_EVIDENCE_CHARS]
        response.close()
        session.mark_done()
        evidence = base_evidence | {
            "status": "unavailable",
            "reason": f"HTTP {response.status_code}",
            "http_status": response.status_code,
            "final_url": response.url,
            "redirects": history,
            "headers": kept_headers,
            "body_head": body,
        }
        _write_json(unavailable_path, evidence)
        return Outcome(target, "unavailable", evidence=evidence)
    part = path.with_name(path.name + ".part")
    digest = hashlib.sha256()
    size = 0
    with part.open("wb") as handle:
        for block in response.iter_content(CHUNK):
            if block:
                handle.write(block)
                digest.update(block)
                size += len(block)
    response.close()
    session.mark_done()
    completed = _utc_now()
    ok, validation = _validate_payload(part, target, headers)
    if not ok:
        body = part.read_bytes()[:BODY_EVIDENCE_CHARS].decode("utf-8", errors="replace")
        part.unlink()
        evidence = base_evidence | {
            "status": "unavailable",
            "reason": validation.get("reason", "payload validation failed"),
            "http_status": response.status_code,
            "final_url": response.url,
            "redirects": history,
            "headers": kept_headers,
            "bytes": size,
            "body_head": body,
            "validation": validation,
        }
        _write_json(unavailable_path, evidence)
        return Outcome(target, "unavailable", evidence=evidence)
    os.replace(part, path)
    if unavailable_path.exists():
        unavailable_path.unlink()
    receipt = base_evidence | {
        "status": "fetched",
        "http_status": response.status_code,
        "final_url": response.url,
        "redirects": history,
        "fetched_at_utc": _iso(started),
        "completed_at_utc": _iso(completed),
        "bytes": size,
        "sha256": digest.hexdigest(),
        "headers": kept_headers,
        "validation": validation,
    }
    if target.drive_id is not None:
        receipt["drive_id"] = target.drive_id
    _write_json(path.with_name(path.name + ".receipt.json"), receipt)
    return Outcome(target, "fetched", path, receipt=receipt)


def _discover_globalq(page: Path) -> tuple[str | None, dict[str, Any]]:
    html = page.read_text(encoding="utf-8", errors="replace")
    found = sorted({(int(year), href) for href, year in GLOBALQ_Q5_MONTHLY.findall(html)})
    if not found:
        return None, {"reason": "no q5_factors_monthly_<year>.csv link on the factors page"}
    year, href = found[-1]
    return GLOBALQ_BASE + href, {"discovered_links": [h for _, h in found], "release_year": year}


def _osap_ids_linked(page: Path) -> dict[str, bool]:
    html = page.read_text(encoding="utf-8", errors="replace")
    return {drive_id: drive_id in html for drive_id in (OSAP_SIGNALDOC_ID, OSAP_PORTS_FULL_ID, OSAP_LS_WIDE_ID)}


def _annotate_receipt(outcome: Outcome, extra: dict[str, Any]) -> None:
    if outcome.path is None or outcome.status != "fetched":
        return
    outcome.receipt.update(extra)
    _write_json(outcome.path.with_name(outcome.path.name + ".receipt.json"), outcome.receipt)


def run_source(
    session: PoliteSession, source: str, root: Path, date_dir: str, refetch: bool, log: Callable[[str], None]
) -> list[Outcome]:
    out_dir = root / source / date_dir
    outcomes: list[Outcome] = []
    context: dict[str, Any] = {}
    for target in TARGETS[source]:
        url = target.url
        extra: dict[str, Any] = {}
        if source == "globalq" and target.name == "q5_factors_monthly.csv":
            page = out_dir / "factors.html"
            if not page.is_file():
                evidence = {"status": "unavailable", "reason": "factors page not fetched; link not discoverable"}
                _write_json(out_dir / (target.name + ".unavailable.json"), evidence)
                outcomes.append(Outcome(target, "unavailable", evidence=evidence))
                continue
            url, discovery = _discover_globalq(page)
            if url is None:
                _write_json(out_dir / (target.name + ".unavailable.json"), {"status": "unavailable"} | discovery)
                outcomes.append(Outcome(target, "unavailable", evidence=discovery))
                continue
            extra = {"discovery": discovery | {"page": "factors.html"}}
        if source == "osap" and target.drive_id is not None:
            linked = context.get("osap_linked")
            extra = {"drive_id_linked_from_data_page": None if linked is None else linked.get(target.drive_id)}
        assert url is not None
        outcome = fetch_target(session, target, url, out_dir, refetch)
        if extra:
            _annotate_receipt(outcome, extra)
        if source == "osap" and target.name == "data_page.html" and outcome.path is not None:
            context["osap_linked"] = _osap_ids_linked(outcome.path)
        size = outcome.receipt.get("bytes") if outcome.receipt else None
        log(f"{source}/{target.name}: {outcome.status}" + (f" ({size} bytes)" if size is not None else ""))
        outcomes.append(outcome)
    return outcomes


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Fetch public benchmark files (French, JKP, OSAP, global-q q5).")
    parser.add_argument("--root", type=Path, default=None, help="default: <data dir>/raw/benchmarks")
    parser.add_argument("--date", default=None, help="UTC date directory (default: today, UTC)")
    parser.add_argument("--sources", default="french,jkp,osap,globalq")
    parser.add_argument("--pause", type=float, default=DEFAULT_PAUSE_S, help="seconds between requests")
    parser.add_argument("--refetch", action="store_true", help="download again even if a verified copy exists")
    parser.add_argument("--no-parse", action="store_true", help="download only; skip building parsed datasets")
    parser.add_argument("--parse-only", action="store_true", help="skip downloads; build parsed datasets")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    from atx_db.connection import resolve_data_dir

    root = (args.root or resolve_data_dir() / "raw" / "benchmarks").resolve()
    date_dir = args.date or _utc_now().date().isoformat()
    sources = [item.strip() for item in args.sources.split(",") if item.strip()]
    unknown = sorted(set(sources) - set(TARGETS))
    if unknown:
        raise SystemExit(f"unknown sources: {unknown}")
    started = _utc_now()
    summary: dict[str, Any] = {"started_at_utc": _iso(started), "root": str(root), "date_dir": date_dir}

    def log(message: str) -> None:
        print(message, flush=True)

    if not args.parse_only:
        session = PoliteSession(args.pause)
        results: list[dict[str, Any]] = []
        for source in sources:
            for outcome in run_source(session, source, root, date_dir, args.refetch, log):
                results.append(
                    {
                        "source": source,
                        "name": outcome.target.name,
                        "role": outcome.target.role,
                        "status": outcome.status,
                        "bytes": outcome.receipt.get("bytes"),
                        "sha256": outcome.receipt.get("sha256"),
                        "reason": outcome.evidence.get("reason"),
                    }
                )
        summary["requests_made"] = session.requests_made
        summary["files"] = results
    if not args.no_parse:
        from atx_db.research.benchmarks_load import build_benchmark_datasets

        summary["datasets"] = build_benchmark_datasets(root)
        for name, info in summary["datasets"].items():
            log(
                f"{name}: {info['status']} rows={info.get('rows')} "
                f"range={info.get('date_min')}..{info.get('date_max')} {info.get('reason') or ''}".rstrip()
            )
    summary["finished_at_utc"] = _iso(_utc_now())
    summary["peak_private_mb"] = _peak_private_mb()
    runs = root / "_runs"
    runs.mkdir(parents=True, exist_ok=True)
    _write_json(runs / (started.strftime("%Y%m%dT%H%M%SZ") + ".json"), summary)
    log(f"peak_private_mb={summary['peak_private_mb']} summary={runs / (started.strftime('%Y%m%dT%H%M%SZ') + '.json')}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
