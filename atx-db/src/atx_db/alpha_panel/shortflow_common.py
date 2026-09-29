"""Shared helpers for the SHORTFLOW stages (ftd, regsho_threshold, short_volume_ext, thirteenf, borrow_proxy).

See ``docs/ALPHA_PANEL_SHORTFLOW.md``.

* Fetching: SEC requests go through :func:`atx_db.sec_http.sec_session` (the approved user agent
  ``atx-db/0.1 atx-research@example.com`` and the host-wide 5 req/s limiter with 403/429 back-off). Other
  hosts go through :class:`PoliteHost` (at most 2 requests/s per host, exponential back-off on 429/5xx and
  transport errors).
* Every fetch appends one JSON line to an append-only receipt ledger (:class:`Ledger`); the last line per key
  wins, so a crash loses at most the in-flight request and a re-run resumes.
* Symbol mapping (:func:`map_symbols_asof`) is the stage-S rule ``si-ticker-asof-settlement-v3`` generalised
  to any key date: the vendor ticker as of the last session on or before the key date, falling back to the
  latest earlier session within 7 calendar days that carries the canonical form; a form carried by two or
  more ids that day is ambiguous and stays unmapped.
"""

from __future__ import annotations

import datetime as dt
import json
import os
import re
import shutil
import time
import urllib.parse
from pathlib import Path
from typing import Any

from . import common as C
from . import finra_fetch as F

RAW_ROOT = C.PACKAGE_ROOT / "data" / "raw"
PUBLIC_UA = "atx-db/0.1 public-data-loader"
HOST_MIN_INTERVAL_S = 0.55          # <= 2 requests/s per non-SEC host
MAX_TRIES = 6
MAX_BACKOFF_S = 120.0
RETRY_CODES = frozenset({429, 500, 502, 503, 504})
MIN_FREE_GB = 35.0
LOOKBACK_DAYS = 7
VENDOR_START = dt.date(2012, 12, 20)
DUCKDB_MEMORY = os.environ.get("ATX_SHORTFLOW_DUCKDB_MEMORY", "400MB")


def utc_now() -> str:
    return dt.datetime.now(dt.UTC).isoformat(timespec="seconds")


def tmp_dir(name: str) -> Path:
    p = C.build_root() / "_tmp" / "shortflow" / name
    p.mkdir(parents=True, exist_ok=True)
    return p


def connect(name: str, memory: str | None = None, threads: int = 2, db_file: str | None = None):
    """Bounded DuckDB under ``_tmp/shortflow/<name>``; ``db_file`` keeps tables on disk (buffer-managed)."""
    return C.connect(memory=memory or DUCKDB_MEMORY, threads=threads, temp_dir=tmp_dir(name) / "spill", db_file=db_file)


# ---------------------------------------------------------------- disk
def free_gb(path: Path = C.PACKAGE_ROOT) -> float:
    return shutil.disk_usage(path).free / 1024 ** 3


def require_free(write_gb: float) -> None:
    """Refuse a step that would leave less than ``MIN_FREE_GB`` free on the drive."""
    free = free_gb()
    if free - write_gb < MIN_FREE_GB:
        raise RuntimeError(f"disk guard: {free:.1f} GB free, step writes up to {write_gb:.1f} GB, floor {MIN_FREE_GB} GB")


# ---------------------------------------------------------------- ledger
class Ledger:
    """Append-only JSONL receipt ledger; ``latest()`` returns the last record per ``key``."""

    def __init__(self, path: Path) -> None:
        self.path = path
        path.parent.mkdir(parents=True, exist_ok=True)

    def append(self, rec: dict[str, Any]) -> None:
        line = json.dumps(rec, sort_keys=True, default=str)
        with self.path.open("a", encoding="utf-8") as fh:
            fh.write(line + "\n")
            fh.flush()
            os.fsync(fh.fileno())

    def records(self) -> list[dict[str, Any]]:
        if not self.path.exists():
            return []
        out = []
        with self.path.open(encoding="utf-8") as fh:
            for line in fh:
                line = line.strip()
                if line:
                    try:
                        out.append(json.loads(line))
                    except json.JSONDecodeError:
                        continue  # a torn last line from a killed writer
        return out

    def latest(self) -> dict[str, dict[str, Any]]:
        out: dict[str, dict[str, Any]] = {}
        for r in self.records():
            if "key" in r:
                out[r["key"]] = r
        return out


# ---------------------------------------------------------------- HTTP
_SEC_SESSION: list[Any] = []


def sec_get(url: str, timeout: float = 300.0, stream_to: Path | None = None) -> tuple[int, bytes, dict[str, str]]:
    """GET an SEC URL through the host-wide limiter. 404 returns ``(404, b"", {})``.

    ``stream_to`` streams the body into that file (``.partial`` then rename) and returns ``b""`` as body.
    """
    from atx_db.sec_http import sec_session

    if not _SEC_SESSION:
        _SEC_SESSION.append(sec_session())
    s = _SEC_SESSION[0]
    err: Exception | None = None
    for attempt in range(MAX_TRIES):
        try:
            with s.get(url, timeout=timeout, stream=stream_to is not None) as r:
                hdr = {k.lower(): v for k, v in r.headers.items()}
                if r.status_code == 404:
                    return 404, b"", hdr
                if r.status_code != 200:
                    raise F.FetchError(f"{url}: HTTP {r.status_code}")
                if stream_to is None:
                    return 200, r.content, hdr
                tmp = stream_to.with_name(stream_to.name + ".partial")
                stream_to.parent.mkdir(parents=True, exist_ok=True)
                with tmp.open("wb") as fh:
                    for chunk in r.iter_content(chunk_size=8 << 20):
                        if chunk:
                            fh.write(chunk)
                declared = hdr.get("content-length")
                if declared and declared.isdigit() and tmp.stat().st_size != int(declared):
                    raise F.FetchError(f"{url}: short body {tmp.stat().st_size} vs {declared}")
                os.replace(tmp, stream_to)
                return 200, b"", hdr
        except F.FetchError as exc:
            err = exc
        except Exception as exc:  # transport errors after the adapter's own retries
            err = exc
        time.sleep(min(2.0 ** attempt, MAX_BACKOFF_S))
    raise F.FetchError(f"{url}: gave up after {MAX_TRIES} tries: {err}")


class RateLimited(F.FetchError):
    """The host asked us to stay away longer than ``MAX_BACKOFF_S``; the caller stops and resumes later."""


class PoliteHost:
    """Sequential requests to one non-SEC host, spaced ``HOST_MIN_INTERVAL_S`` apart (<= 2 req/s)."""

    def __init__(self, min_interval: float = HOST_MIN_INTERVAL_S, user_agent: str = PUBLIC_UA) -> None:
        import requests

        self.min_interval = min_interval
        self.session = requests.Session()
        self.session.headers["User-Agent"] = user_agent
        self._last = 0.0

    def request(self, url: str, params: dict[str, str] | None = None, json_body: Any = None,
                accept: str | None = None, timeout: float = 120.0) -> tuple[int, bytes, dict[str, str]]:
        import requests

        err: Exception | None = None
        for attempt in range(MAX_TRIES):
            wait = self.min_interval - (time.monotonic() - self._last)
            if wait > 0:
                time.sleep(wait)
            self._last = time.monotonic()
            headers = {"Accept": accept} if accept else {}
            try:
                if json_body is not None:
                    r = self.session.post(url, params=params, json=json_body, headers=headers, timeout=timeout)
                else:
                    r = self.session.get(url, params=params, headers=headers, timeout=timeout)
            except (requests.ConnectionError, requests.Timeout) as exc:
                err = exc
                time.sleep(min(2.0 ** attempt, MAX_BACKOFF_S))
                continue
            hdr = {k.lower(): v for k, v in r.headers.items()}
            if r.status_code in RETRY_CODES:
                err = F.FetchError(f"{url}: HTTP {r.status_code}")
                ra = hdr.get("retry-after")
                if r.status_code == 429 and ra and ra.isdigit() and float(ra) > MAX_BACKOFF_S:
                    # a long ban (e.g. Cloudflare's 1-hour window): stop instead of retrying into it
                    raise RateLimited(f"{url}: HTTP 429, Retry-After {ra} s")
                time.sleep(min(float(ra), MAX_BACKOFF_S) if ra and ra.isdigit() else min(2.0 ** attempt, MAX_BACKOFF_S))
                continue
            return int(r.status_code), r.content, hdr
        raise F.FetchError(f"{url}: gave up after {MAX_TRIES} tries: {err}")


def full_url(url: str, params: dict[str, str] | None) -> str:
    return url + ("?" + urllib.parse.urlencode(params) if params else "")


def http_date(value: str | None) -> dt.datetime | None:
    """Parse an HTTP ``Last-Modified`` value to aware UTC (None when absent/invalid)."""
    import email.utils

    if not value:
        return None
    try:
        t = email.utils.parsedate_to_datetime(value)
    except (TypeError, ValueError):
        return None
    return t if t.tzinfo else t.replace(tzinfo=dt.UTC)


# ---------------------------------------------------------------- calendars
def _nth_weekday(year: int, month: int, weekday: int, n: int) -> dt.date:
    d = dt.date(year, month, 1)
    d += dt.timedelta(days=(weekday - d.weekday()) % 7)
    return d + dt.timedelta(weeks=n - 1)


def _last_weekday(year: int, month: int, weekday: int) -> dt.date:
    d = dt.date(year + (month == 12), 1 if month == 12 else month + 1, 1) - dt.timedelta(days=1)
    return d - dt.timedelta(days=(d.weekday() - weekday) % 7)


def _observed(d: dt.date) -> dt.date:
    return d - dt.timedelta(days=1) if d.weekday() == 5 else d + dt.timedelta(days=1) if d.weekday() == 6 else d


def federal_holidays(year: int) -> set[dt.date]:
    """US federal holidays (SEC/EDGAR closed), observed dates."""
    h = {
        _observed(dt.date(year, 1, 1)), _nth_weekday(year, 1, 0, 3), _nth_weekday(year, 2, 0, 3),
        _last_weekday(year, 5, 0), _observed(dt.date(year, 7, 4)), _nth_weekday(year, 9, 0, 1),
        _nth_weekday(year, 10, 0, 2), _observed(dt.date(year, 11, 11)), _nth_weekday(year, 11, 3, 4),
        _observed(dt.date(year, 12, 25)),
    }
    if year >= 2021:
        h.add(_observed(dt.date(year, 6, 19)))
    # New Year's Day of the next year observed on Dec 31 of this year
    nxt = dt.date(year + 1, 1, 1)
    if nxt.weekday() == 5:
        h.add(dt.date(year, 12, 31))
    return h


def is_sec_business_day(d: dt.date) -> bool:
    return d.weekday() < 5 and d not in federal_holidays(d.year)


def next_sec_business_day_on_or_after(d: dt.date) -> dt.date:
    while not is_sec_business_day(d):
        d += dt.timedelta(days=1)
    return d


def at_utc(d: dt.date, hour: int = 0, minute: int = 0) -> dt.datetime:
    return dt.datetime(d.year, d.month, d.day, hour, minute, tzinfo=dt.UTC)


# ---------------------------------------------------------------- symbols
_CANON_RE = re.compile(r"[./\s-]")


def canon(symbol: str | None) -> str | None:
    """Python twin of ``finra_fetch.CANON_SQL``: strip '.', '/', '-', whitespace; case-sensitive."""
    if symbol is None:
        return None
    return _CANON_RE.sub("", symbol.strip())


def vendor_ticker_table(con, sessions: list[dt.date]) -> str:
    """Glob of ``(d, security_id, tk, ck, sid_repaired)`` files (one per year) for every vendor session since
    ``VENDOR_START``.

    Rows with ``securityID > 0`` plus sid-0 rows repaired by ``finra_fetch.sid0_repairs`` (the shared
    ``sid0-bracketed-ticker-v2`` rule, identical to stage P). Only session dates (>= 1,000 vendor rows)
    are kept, as in stage S. Cached per TickerHistory3 identity and rule.
    """
    root = tmp_dir("vendor")
    stamp = root / "vendor_tickers.json"
    ident = C.file_identity(C.TICKERHISTORY)
    sess = [d for d in sessions if d >= VENDOR_START]
    key = {"source": ident, "sid0_rule": F.SID0_RULE, "first": sess[0].isoformat(), "last": sess[-1].isoformat(),
           "n": len(sess), "layout": "per-year-v2"}
    glob = (root / "vendor_tickers_*.parquet").as_posix()
    if stamp.exists() and C.read_json(stamp).get("key") == key:
        return glob
    rep, _ = F.sid0_repairs(con)
    ck_t = F.CANON_SQL.format(x="ticker_tk")
    rows = 0
    for y in sorted({d.year for d in sess}):
        con.execute("CREATE OR REPLACE TEMP TABLE _sess (d DATE)")
        con.executemany("INSERT INTO _sess VALUES (?)", [(d,) for d in sess if d.year == y])
        sql = f"""
            SELECT tradingDate AS d, securityID AS security_id, trim(ticker_tk) AS tk, {ck_t} AS ck, false AS sid_repaired
            FROM read_parquet('{C.TICKERHISTORY.as_posix()}')
            WHERE tradingDate IN (SELECT d FROM _sess)
              AND securityID IS NOT NULL AND securityID > 0 AND ticker_tk IS NOT NULL AND trim(ticker_tk) <> ''
            UNION ALL
            SELECT trade_date, security_id, trim(ticker_tk), {ck_t}, true
            FROM read_parquet('{rep.as_posix()}')
            WHERE basis = 'bracketed' AND trade_date IN (SELECT d FROM _sess)
        """
        rows += C.copy_to_parquet(con, sql, root / f"vendor_tickers_{y}.parquet")
    con.execute("DROP TABLE _sess")
    C.write_json_atomic(stamp, {"key": key, "rows": rows})
    return glob


def map_symbols_asof(con, keys_sql: str, vendor: Path | str, dest: Path, lookback_days: int = LOOKBACK_DAYS) -> int:
    """Map distinct ``(key_date, symbol)`` pairs (``keys_sql``) to vendor ids as of ``key_date``.

    Output columns: ``key_date, symbol, ck, candidate_security_id`` (set only when exactly one id carries
    the canonical form on the vendor date), ``n_ids, vendor_date, vendor_date_fallback, exact, sid_repaired,
    outcome`` (``candidate`` / ``ambiguous`` / ``unmapped``).

    The vendor date is the latest session on or before ``key_date`` (within ``lookback_days`` calendar days)
    on which some vendor row carries the canonical form: the stage-S primary date when the form is on the
    last session, else the stage-S fallback. Only data dated on or before ``key_date`` is read.
    """
    ck_k = F.CANON_SQL.format(x="symbol")
    sql = f"""
        WITH k AS (
            SELECT DISTINCT key_date, symbol, {ck_k} AS ck FROM ({keys_sql}) WHERE symbol IS NOT NULL AND trim(symbol) <> ''
        ),
        v AS (
            SELECT d, ck, min(security_id) AS sid, count(DISTINCT security_id) AS n_ids, bool_and(sid_repaired) AS rep
            FROM read_parquet('{_posix(vendor)}')
            WHERE ck IN (SELECT DISTINCT ck FROM k)
              AND d BETWEEN (SELECT min(key_date) FROM k) - INTERVAL {lookback_days + 1} DAY AND (SELECT max(key_date) FROM k)
            GROUP BY 1, 2
        ),
        j AS (
            SELECT k.key_date, k.symbol, k.ck, v.d AS vendor_date, v.sid, v.n_ids, v.rep
            FROM k ASOF LEFT JOIN v ON k.ck = v.ck AND k.key_date >= v.d
        ),
        j2 AS (
            SELECT key_date, symbol, ck,
                   CASE WHEN vendor_date IS NOT NULL AND date_diff('day', vendor_date, key_date) <= {lookback_days}
                        THEN vendor_date END AS vendor_date,
                   sid, n_ids, rep
            FROM j
        ),
        last_sess AS (
            SELECT DISTINCT d FROM read_parquet('{_posix(vendor)}')
        ),
        md AS (
            SELECT k2.key_date, max(ls.d) AS map_date FROM (SELECT DISTINCT key_date FROM k) k2
            LEFT JOIN last_sess ls ON ls.d <= k2.key_date AND ls.d >= k2.key_date - INTERVAL {lookback_days} DAY
            GROUP BY 1
        ),
        ex AS (
            SELECT DISTINCT d, security_id, tk FROM read_parquet('{_posix(vendor)}')
            WHERE ck IN (SELECT DISTINCT ck FROM k)
              AND d BETWEEN (SELECT min(key_date) FROM k) - INTERVAL {lookback_days + 1} DAY AND (SELECT max(key_date) FROM k)
        )
        SELECT j2.key_date, j2.symbol, j2.ck,
               CAST(CASE WHEN j2.vendor_date IS NOT NULL AND j2.n_ids = 1 THEN j2.sid END AS BIGINT) AS candidate_security_id,
               CASE WHEN j2.vendor_date IS NOT NULL THEN j2.n_ids END AS n_ids,
               j2.vendor_date,
               coalesce(j2.vendor_date < md.map_date, false) AS vendor_date_fallback,
               coalesce(j2.vendor_date IS NOT NULL AND j2.n_ids = 1 AND ex.tk IS NOT NULL, false) AS exact,
               coalesce(j2.vendor_date IS NOT NULL AND j2.n_ids = 1 AND j2.rep, false) AS sid_repaired,
               CASE WHEN j2.vendor_date IS NULL THEN 'unmapped' WHEN j2.n_ids > 1 THEN 'ambiguous' ELSE 'candidate' END AS outcome
        FROM j2
        LEFT JOIN md ON md.key_date = j2.key_date
        LEFT JOIN ex ON ex.d = j2.vendor_date AND ex.security_id = j2.sid AND ex.tk = trim(j2.symbol)
    """
    return C.copy_to_parquet(con, sql, dest)


def _posix(p: Path | str) -> str:
    return p.as_posix() if isinstance(p, Path) else p


def sessions_all(con) -> list[dt.date]:
    return F.sessions(con)


def year_table(rows: list[tuple], cols: list[str]) -> dict[str, dict[str, Any]]:
    """``[(year, v1, v2, ...)]`` -> ``{year: {col: v}}`` (JSON-friendly)."""
    out: dict[str, dict[str, Any]] = {}
    for r in rows:
        out[str(r[0])] = {c: (float(v) if isinstance(v, float) else v) for c, v in zip(cols, r[1:])}
    return out
