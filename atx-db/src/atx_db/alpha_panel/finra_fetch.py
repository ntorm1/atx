"""Polite, resumable downloader for public FINRA files (stages S and V).

Rules (docs/ALPHA_PANEL_FINRA.md): public endpoints only, strictly sequential requests spaced at
least ``MIN_INTERVAL_S`` apart, exponential backoff on 429/5xx/transport errors, 403/404 recorded as
"no file", User-Agent ``UA``. Files are stored gzip-compressed at rest (``mtime=0`` so the gzip
bytes are reproducible); the manifest records the sha256 of the *raw* (uncompressed) bytes.

The manifest is an append-only CSV (``MANIFEST_COLUMNS``); the last row per ``date`` wins, so a
crash never loses more than the in-flight file and a re-run resumes where it stopped.
"""

from __future__ import annotations

import csv
import datetime as dt
import gzip
import hashlib
import os
import time
import urllib.error
import urllib.request
from pathlib import Path
from typing import Any

from . import common as C

UA = "atx-research/0.1 (nathan.tormaschy2@gmail.com)"
MIN_INTERVAL_S = 0.6
MAX_TRIES = 6
MAX_BACKOFF_S = 120.0
ABSENT_CODES = frozenset({403, 404})
RETRY_CODES = frozenset({429, 500, 502, 503, 504})
MANIFEST_COLUMNS = ["date", "file", "bytes", "sha256_of_raw_bytes", "rows", "url", "http_status", "downloaded_at"]

# ~60% of the 0.8 GiB guard cap these stages run under; override for a different cap.
DUCKDB_MEMORY = os.environ.get("ATX_FINRA_DUCKDB_MEMORY", "420MB")

RAW_ROOT = C.PACKAGE_ROOT / "data" / "raw"
SI_RAW_DIR = RAW_ROOT / "finra_short_interest"
SV_RAW_DIR = RAW_ROOT / "finra_short_volume"

_last_request = [0.0]


class FetchError(RuntimeError):
    pass


def get(url: str, accept: str | None = None, timeout: float = 120.0) -> tuple[int, bytes]:
    """GET ``url`` politely. Returns ``(status, body)``; 403/404 return ``(code, b"")``."""
    status, body, _ = request(url, accept=accept, timeout=timeout)
    return status, body


def request(url: str, accept: str | None = None, json_body: Any = None,
            timeout: float = 120.0) -> tuple[int, bytes, dict[str, str]]:
    """GET (or POST ``json_body``) politely. Returns ``(status, body, headers)``; 403/404 -> empty body."""
    import json as _json

    err: Exception | None = None
    for attempt in range(MAX_TRIES):
        wait = MIN_INTERVAL_S - (time.monotonic() - _last_request[0])
        if wait > 0:
            time.sleep(wait)
        _last_request[0] = time.monotonic()
        headers = {"User-Agent": UA}
        if accept:
            headers["Accept"] = accept
        data = None
        if json_body is not None:
            data = _json.dumps(json_body).encode("utf-8")
            headers["Content-Type"] = "application/json"
        try:
            req = urllib.request.Request(url, headers=headers, data=data, method="POST" if data is not None else "GET")
            with urllib.request.urlopen(req, timeout=timeout) as resp:
                return int(resp.status), resp.read(), {k.lower(): v for k, v in resp.headers.items()}
        except urllib.error.HTTPError as exc:
            if exc.code in ABSENT_CODES:
                return int(exc.code), b"", {}
            if exc.code not in RETRY_CODES:
                raise FetchError(f"{url}: HTTP {exc.code}") from exc
            err = exc
            retry_after = exc.headers.get("Retry-After") if exc.headers else None
            if retry_after and retry_after.isdigit():
                time.sleep(min(float(retry_after), MAX_BACKOFF_S))
                continue
        except (urllib.error.URLError, TimeoutError, ConnectionError, OSError) as exc:
            err = exc
        time.sleep(min(2.0 ** attempt, MAX_BACKOFF_S))
    raise FetchError(f"{url}: gave up after {MAX_TRIES} tries: {err}")


def count_data_rows(body: bytes) -> int:
    """Pipe-delimited FINRA text: data lines = non-empty lines with a '|' minus the header."""
    n = 0
    for line in body.splitlines():
        if b"|" in line and line.strip():
            n += 1
    return max(0, n - 1)


def load_manifest(path: Path) -> dict[str, dict[str, str]]:
    """Last row per date wins (the manifest is append-only)."""
    out: dict[str, dict[str, str]] = {}
    if not path.exists():
        return out
    with path.open(newline="", encoding="utf-8") as fh:
        for row in csv.DictReader(fh):
            if row.get("date"):
                out[row["date"]] = row
    return out


def append_manifest(path: Path, row: dict[str, Any]) -> None:
    new = not path.exists() or path.stat().st_size == 0
    with path.open("a", newline="", encoding="utf-8") as fh:
        w = csv.DictWriter(fh, fieldnames=MANIFEST_COLUMNS)
        if new:
            w.writeheader()
        w.writerow({k: row.get(k, "") for k in MANIFEST_COLUMNS})
        fh.flush()
        os.fsync(fh.fileno())


def write_gzip_atomic(dest: Path, body: bytes) -> None:
    dest.parent.mkdir(parents=True, exist_ok=True)
    tmp = dest.with_name(dest.name + ".partial")
    tmp.write_bytes(gzip.compress(body, compresslevel=9, mtime=0))
    os.replace(tmp, dest)


def read_gzip_verified(path: Path, sha256: str | None) -> bytes:
    body = gzip.decompress(path.read_bytes())
    if sha256 and hashlib.sha256(body).hexdigest() != sha256:
        raise FetchError(f"{path}: raw sha256 mismatch vs manifest")
    return body


def fetch_one(date: dt.date, url: str, dest: Path, manifest: Path) -> dict[str, Any]:
    status, body = get(url)
    row: dict[str, Any] = {
        "date": date.isoformat(),
        "url": url,
        "http_status": status,
        "downloaded_at": dt.datetime.now(dt.UTC).isoformat(timespec="seconds"),
    }
    if status == 200 and body:
        write_gzip_atomic(dest, body)
        row.update(file=dest.name, bytes=len(body), sha256_of_raw_bytes=hashlib.sha256(body).hexdigest(),
                   rows=count_data_rows(body))
    else:
        row.update(file="", bytes=0, sha256_of_raw_bytes="", rows=0)
    append_manifest(manifest, row)
    return row


def is_done(row: dict[str, str] | None, raw_dir: Path, retry_absent: bool) -> bool:
    if row is None:
        return False
    status = int(row.get("http_status") or 0)
    if status == 200:
        return bool(row.get("file")) and (raw_dir / row["file"]).exists()
    if status in ABSENT_CODES:
        return not retry_absent
    return False


# ---------------------------------------------------------------- vendor ticker map helpers
# FINRA symbols vs ORATS ticker_tk: FINRA short interest writes class shares without punctuation
# (BRKB, BFB, GEFB, BFHPRA), CNMS writes a slash (BRK/B, AIG/WS) and ORATS writes a dot (BRK.B,
# BFH.PRA, AAC.U). The canonical form strips '.', '/', '-' and whitespace on both sides (the rule of
# the earlier as-of producer, iteration21_finra_si_asof.py) but stays CASE-SENSITIVE: CNMS and ORATS
# mark preferreds / rights / when-issued with lower-case letters (TpC = AT&T pref. C, SRVr), and
# upper-casing them collides with real common tickers (TpC -> TPC = Tutor Perini).
CANON_SQL = "regexp_replace(trim({x}), '[./\\s-]', '', 'g')"


def finra_tmp(name: str) -> Path:
    p = C.build_root() / "_tmp" / name
    p.mkdir(parents=True, exist_ok=True)
    return p


def session_counts(con: Any) -> dict[dt.date, int]:
    """Rows per vendor ``tradingDate`` (cached per TickerHistory3 identity)."""
    dest = finra_tmp("finra_th") / "th_date_counts.parquet"
    stamp = dest.with_suffix(".json")
    ident = C.file_identity(C.TICKERHISTORY)
    if not (dest.exists() and stamp.exists() and C.read_json(stamp).get("source") == ident):
        C.copy_to_parquet(
            con, f"SELECT tradingDate AS d, count(*) AS n FROM read_parquet('{C.TICKERHISTORY.as_posix()}') GROUP BY 1", dest
        )
        C.write_json_atomic(stamp, {"source": ident})
    rows = con.execute(f"SELECT d, n FROM read_parquet('{dest.as_posix()}') ORDER BY 1").fetchall()
    return {d: int(n) for d, n in rows}


def sessions(con: Any) -> list[dt.date]:
    """Session calendar: vendor dates with at least ``MIN_ROWS_PER_SESSION`` rows (docs/ALPHA_PANEL.md)."""
    return sorted(d for d, n in session_counts(con).items() if n >= C.MIN_ROWS_PER_SESSION)


SID0_RULE = "sid0-bracketed-ticker-v2"
SID0_START = dt.date(2017, 10, 1)
SID0_MAX_BRACKET_DAYS = 400


def sid0_repairs(con: Any) -> tuple[Path, dict[str, Any]]:
    """Vendor rows filed under ``securityID = 0`` -> the id that carries the same ticker around them.

    TickerHistory3 files 40-80 real lines a day under ``securityID = 0`` (MSFT on most sessions
    2025-11-24 .. 2026-08). Rule ``sid0-bracketed-ticker-v2`` (identical to the stage-P repair agreed
    with the controller): a sid-0 row ``(d, ticker_tk)`` is reassigned to id ``S`` iff
    (1) it is the only sid-0 row with that ticker on ``d``; (2) the nearest non-zero row with that
    ticker strictly before ``d`` and the nearest strictly after ``d`` both have securityID ``S`` (a
    neighbour date carrying two ids fails); (3) each bracket row is within 400 calendar days of ``d``;
    and (4) ``S`` has no row of its own on ``d`` (the original row wins; the repaired row is dropped).
    Tickers are compared exactly as the vendor writes them. The later neighbour is dated after ``d``:
    this is a vendor-identity repair (a static property of the vendor file), not a point-in-time
    decision. Rows failing the rule stay unusable (``basis`` records why). Cached per vendor identity.
    """
    dest = finra_tmp("finra_th") / "th_sid0_repair.parquet"
    stamp = dest.with_suffix(".json")
    ident = C.file_identity(C.TICKERHISTORY)
    if dest.exists() and stamp.exists():
        meta = C.read_json(stamp)
        if meta.get("source") == ident and meta.get("rule") == SID0_RULE:
            return dest, meta["stats"]
    src = C.TICKERHISTORY.as_posix()
    lo = SID0_START - dt.timedelta(days=SID0_MAX_BRACKET_DAYS)
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE z AS
        SELECT tradingDate AS d, ticker_tk AS tk, count(*) AS n0
        FROM read_parquet('{src}')
        WHERE securityID = 0 AND tradingDate >= DATE '{SID0_START}' AND ticker_tk IS NOT NULL AND ticker_tk <> ''
        GROUP BY 1, 2
    """)
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE nz AS
        SELECT tradingDate AS d, ticker_tk AS tk, min(securityID) AS sid, count(DISTINCT securityID) AS n
        FROM read_parquet('{src}')
        WHERE securityID > 0 AND tradingDate >= DATE '{lo}' AND ticker_tk IN (SELECT DISTINCT tk FROM z)
        GROUP BY 1, 2
    """)
    con.execute("""
        CREATE OR REPLACE TEMP TABLE zb AS
        WITH p AS (
            SELECT z.d, z.tk, z.n0, nz.d AS prev_d, nz.sid AS prev_sid, nz.n AS prev_n
            FROM z ASOF LEFT JOIN nz ON z.tk = nz.tk AND z.d > nz.d
        )
        SELECT p.*, nz.d AS next_d, nz.sid AS next_sid, nz.n AS next_n
        FROM p ASOF LEFT JOIN nz ON p.tk = nz.tk AND p.d < nz.d
    """)
    # (4): does the bracket id already have a row of its own on d?
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE own AS
        SELECT DISTINCT tradingDate AS d, securityID AS sid
        FROM read_parquet('{src}')
        WHERE tradingDate >= DATE '{SID0_START}'
          AND securityID IN (SELECT DISTINCT prev_sid FROM zb WHERE prev_sid = next_sid)
    """)
    sql = f"""
        SELECT d AS trade_date, tk AS ticker_tk,
               CASE WHEN basis = 'bracketed' THEN prev_sid END AS security_id,
               basis, n0 AS sid0_rows, prev_d, prev_sid, next_d, next_sid
        FROM (
            SELECT zb.*, CASE
                WHEN n0 > 1 THEN 'several_sid0_rows'
                WHEN prev_sid IS NULL AND next_sid IS NULL THEN 'ticker_never_nonzero'
                WHEN prev_sid IS NULL THEN 'next_only'
                WHEN next_sid IS NULL THEN 'prev_only'
                WHEN prev_n > 1 OR next_n > 1 THEN 'neighbour_ambiguous'
                WHEN prev_sid <> next_sid THEN 'bracket_disagrees'
                WHEN date_diff('day', zb.prev_d, zb.d) > {SID0_MAX_BRACKET_DAYS}
                  OR date_diff('day', zb.d, zb.next_d) > {SID0_MAX_BRACKET_DAYS} THEN 'bracket_too_far'
                WHEN own.sid IS NOT NULL THEN 'id_has_own_row'
                ELSE 'bracketed' END AS basis
            FROM zb LEFT JOIN own ON own.d = zb.d AND own.sid = zb.prev_sid
        )
        ORDER BY trade_date, ticker_tk
    """
    n = C.copy_to_parquet(con, sql, dest)
    basis = dict(con.execute(f"SELECT basis, count(*) FROM read_parquet('{dest.as_posix()}') GROUP BY 1").fetchall())
    for t in ("z", "nz", "zb", "own"):
        con.execute(f"DROP TABLE {t}")
    st = {"rule": SID0_RULE, "since": SID0_START.isoformat(), "sid0_date_ticker_pairs": n,
          "basis": {k: int(v) for k, v in basis.items()}}
    C.write_json_atomic(stamp, {"source": ident, "rule": SID0_RULE, "stats": st})
    return dest, st


def ticker_extract(con: Any, dates: list[dt.date], dest: Path) -> int:
    """``(map_date, security_id, ticker_tk, sid_repaired)`` vendor rows for ``dates``.

    Rows with ``securityID > 0`` plus sid-0 rows repaired by :func:`sid0_repairs` (cached by
    source, rule and dates).
    """
    ident = C.file_identity(C.TICKERHISTORY)
    key = hashlib.sha256(",".join(d.isoformat() for d in sorted(dates)).encode()).hexdigest()
    stamp = dest.with_suffix(".json")
    if dest.exists() and stamp.exists():
        meta = C.read_json(stamp)
        if meta.get("source") == ident and meta.get("dates_sha256") == key and meta.get("sid0_rule") == SID0_RULE:
            return int(meta["rows"])
    rep, _ = sid0_repairs(con)
    lst = ", ".join(f"DATE '{d}'" for d in sorted(dates))
    sql = f"""
        SELECT tradingDate AS map_date, securityID AS security_id, ticker_tk, false AS sid_repaired
        FROM read_parquet('{C.TICKERHISTORY.as_posix()}')
        WHERE tradingDate IN ({lst})
          AND securityID IS NOT NULL AND securityID > 0 AND ticker_tk IS NOT NULL AND trim(ticker_tk) <> ''
        UNION ALL
        SELECT trade_date, security_id, ticker_tk, true
        FROM read_parquet('{rep.as_posix()}')
        WHERE basis = 'bracketed' AND trade_date IN ({lst})
        ORDER BY 1, 3
    """
    n = C.copy_to_parquet(con, sql, dest)
    C.write_json_atomic(stamp, {"source": ident, "dates_sha256": key, "dates": len(dates), "rows": n,
                                "sid0_rule": SID0_RULE})
    return n
