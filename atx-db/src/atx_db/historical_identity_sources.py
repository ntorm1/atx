"""Filing-sourced historical identity evidence (C9 / L2 H2): the verifiable tier.

Scope
-----
The strict, certified point-in-time universe can only include a delisted line
when a dated, retained, authoritative artifact states its identity facts. The
SEC filing record is that artifact for listing registration and removal:

* Form **8-A12B** (Section 12(b) registration of a class on a named national
  securities exchange) and **8-A12G** (Section 12(g) registration; before
  Nasdaq became an exchange in August 2006 this is how Nasdaq NMS classes were
  registered). The cover states the class title and, for 12(b), the exchange.
* Exchange **certifications** (``CERT``, ``CERTNYS``, ``CERTNAS``, ...): the
  exchange certifies that it approved the class for listing and registration.
  The form code (or the filing agent) names the exchange.
* Form **25-NSE** (an exchange removing a class from listing, Rule 12d2-2(a)/(c))
  and **Form 25** (an issuer's voluntary withdrawal, Rule 12d2-2(c)). The
  notice states the exchange, the class and the issuer; it takes effect ten
  days after filing (Rule 12d2-2(d)(1)).

Two evidence tiers come out of this module, both on the 0327
``security_identity_evidence`` shape (:mod:`atx_db.historical_identity`):

``bulk`` (zero network)
    One streaming pass over the retained ``submissions.zip`` locates every
    identity filing with its accession, acceptance clock, primary document and
    filing agent, plus each issuer's dated ``formerNames`` windows.
``document`` (RX5-gated, budgeted)
    The primary document of a filing is fetched once (approved user agent,
    rate-limited, every response cached and ledgered under a ``C9-`` cache
    directory) and parsed for the issuer, class title, exchange and -- where the
    filing states it -- the trading symbol.

Labels (never promoted)
-----------------------
* A fact stated by a parsed filing document at class grain is
  ``verified_dated``. Its clock is the EDGAR acceptance time: when SEC's
  offset-bearing acceptance stamp is a real time of day, ``source_published_at``
  is that stamp and ``availability_status='verified'``; ``available_at`` is
  never earlier than the FC1 floor ``filed date + 46h``
  (:data:`atx_db._fundamental_clock.FUNDAMENTAL_CLOCK_POLICY`). A date-only
  legacy stamp (EDGAR midnight Eastern) gives ``modeled`` availability.
* A 25-NSE/25 effective date that the document does not state is the
  regulatory ``filed + 10 days``: ``inferred`` (the notice is verified; the
  date is a rule, and trading often stopped earlier).
* ``formerNames`` intervals are issuer-name windows the 2026 archive states
  about the past: ``reconstructed`` with ``modeled`` availability.
* A document that cannot be parsed to the fact it is supposed to carry is kept
  as ``unknown`` with a ``rejection_reason``; nothing is dropped silently.
* The link from a vendor price line to a filing-stated symbol is a join of two
  sources (the filing's dated symbol, the vendor's dated ``ticker_tk``). It is
  ``verified_dated`` only for the symbol/class side; the line-level
  ``issuer_link`` it supports is written with the vendor namespace and labelled
  by :data:`LINE_LINK_STATUS` -- the vendor's historical symbol is a later
  reconstruction, so the link is never presented as verified vintage.

This module never opens the warehouse. Callers pass a DuckDB connection to a
scratch database (bootstrapped at migration head, test-template style) or an
in-memory one.
"""

from __future__ import annotations

import datetime as dt
import hashlib
import json
import re
import time
from collections.abc import Callable, Iterable, Iterator, Mapping, Sequence
from dataclasses import dataclass, field
from html import unescape
from pathlib import Path
from typing import Any
from xml.etree import ElementTree
from zoneinfo import ZoneInfo

import duckdb

from ._fundamental_clock import FUNDAMENTAL_CLOCK_POLICY
from ._submissions_archive import SubmissionsArchive
from .historical_identity import EVIDENCE_COLUMNS, EVIDENCE_TABLE, normalize_cik
from .market_owner_bridge import normalize_symbol
from .sec_http import APPROVED_SEC_USER_AGENT, SecRateLimiter, sec_session

USER_AGENT = APPROVED_SEC_USER_AGENT
#: RX5: at most 500 primary-document fetches for the pilot, at most 5 requests per second.
PILOT_FETCH_BUDGET = 500
MAX_REQUESTS_PER_SECOND = 5.0
DEFAULT_MIN_INTERVAL_S = 0.25  # 4 req/s: strictly inside the RX5 ceiling
MAX_DOCUMENT_BYTES = 8 * 1024 * 1024
FETCH_LEDGER_NAME = "C9-fetch-ledger.jsonl"
SEC_ARCHIVES_ROOT = "https://www.sec.gov/Archives/edgar/data"

SOURCE_FILINGS = "SEC EDGAR submissions"
SOURCE_DOCUMENTS = "SEC EDGAR filing documents"
METHOD_DOCUMENT = "c9_filing_document_v1"
METHOD_FORMER_NAMES = "c9_submissions_former_names_v1"
METHOD_LINE_SYMBOL = "c9_filing_symbol_line_join_v1"
LINE_LINK_STATUS = "reconstructed"
AVAILABILITY_POLICY = f"sec_acceptance_or_{FUNDAMENTAL_CLOCK_POLICY}"
FILING_DELAY_EFFECTIVE_DAYS = 10  # Rule 12d2-2(d)(1)

#: Native-key namespaces (never a globally unique ticker).
NS_CLASS = "sec.cik_class"  # "<cik>|<class key>"
NS_ISSUER_NAME = "sec.issuer_name"  # normalized issuer name
NS_EXCHANGE_SYMBOL = "exchange_symbol"  # "<MIC>:<symbol>" or "US:<symbol>"
NS_VENDOR_LINE = "tickerhistory3.securityID"  # shared with RI1 / the owner bridge
VENDOR_LINE_PREFIX = "TBLTICKERHISTORY-"

REGISTRATION_FORMS = frozenset({"8-A12B", "8-A12G", "8-A12B/A", "8-A12G/A"})
DELISTING_FORMS = frozenset({"25", "25/A", "25-NSE", "25-NSE/A"})
DEREGISTRATION_FORMS = frozenset({"15-12B", "15-12G", "15-15D", "15F-12B", "15F-12G", "15F-15D"})
PERIODIC_FORMS = frozenset(
    {"10-K", "10-K405", "10-KSB", "10-KT", "10-Q", "10-QSB", "20-F", "40-F", "10-K/A", "10-Q/A", "20-F/A"}
)


def is_certification_form(form: str) -> bool:
    return form.startswith("CERT")


def is_identity_form(form: str) -> bool:
    return (
        form in REGISTRATION_FORMS
        or form in DELISTING_FORMS
        or form in DEREGISTRATION_FORMS
        or is_certification_form(form)
    )


#: Exchange certification form code -> MIC (paper-era codes name the exchange).
CERT_FORM_MIC = {
    "CERTNYS": "XNYS",
    "CERTNAS": "XNAS",
    "CERTARCA": "ARCX",
    "CERTPAC": "ARCX",  # Pacific Exchange, merged into NYSE Arca
    "CERTAMEX": "XASE",
    "CERTAMX": "XASE",
    "CERTBATS": "BATS",
    "CERTCBO": "XCBO",
    "CERTCHX": "XCHI",
    "CERTPHLX": "XPHL",
    "CERTPHL": "XPHL",
    "CERTBSE": "XBOS",
    "CERTNSX": "XCIS",
    "CERTISE": "XISX",
}

# Exchange names as they appear on 8-A covers, 25/25-NSE notices and filer names.
# Order matters: specific (Arca/American/Chicago...) before the generic NYSE/Nasdaq.
_EXCHANGE_PATTERNS: tuple[tuple[str, re.Pattern[str]], ...] = tuple(
    (mic, re.compile(pattern, re.IGNORECASE))
    for mic, pattern in (
        ("ARCX", r"\bNYSE\s*Arca\b|\bArchipelago\b|\bPacific\s+(?:Stock\s+)?Exchange\b|\bArca\b"),
        ("XASE", r"\bNYSE\s*(?:American|Amex|MKT|Alternext)\b|\bAmerican\s+Stock\s+Exchange\b|\bAMEX\b"),
        ("XCHI", r"\bNYSE\s*Chicago\b|\bChicago\s+Stock\s+Exchange\b"),
        ("XNYS", r"\bNew\s+York\s+Stock\s+Exchange\b|\bNYSE\b"),
        ("BATS", r"\bCboe\s+BZX\b|\bBATS\s+(?:BZX\s+)?Exchange\b|\bBZX\b|\bBats\b"),
        ("XCBO", r"\bChicago\s+Board\s+Options\s+Exchange\b|\bCboe\s+Exchange\b"),
        ("XPHL", r"\bPhiladelphia\s+Stock\s+Exchange\b|\bNasdaq\s+PHLX\b"),
        ("XBOS", r"\bBoston\s+Stock\s+Exchange\b|\bNasdaq\s+BX\b"),
        ("IEXG", r"\bInvestors\s+Exchange\b|\bIEX\b"),
        ("XCIS", r"\bNational\s+Stock\s+Exchange\b"),
        ("LTSE", r"\bLong[-\s]Term\s+Stock\s+Exchange\b"),
        ("XNAS", r"\bNasdaq\b|\bNASDAQ\b|\bNational\s+Market\s+System\b|\bNational\s+Market\b"),
    )
)


def exchange_mic(text: str | None) -> str | None:
    """MIC for an exchange name as written in a filing (``None`` when not named)."""
    if not text:
        return None
    for mic, pattern in _EXCHANGE_PATTERNS:
        if pattern.search(text):
            return mic
    return None


# ---------------------------------------------------------------------------------------------
# Clocks

_EASTERN = ZoneInfo("America/New_York")
_OFFSET_SUFFIX = re.compile(r"(?:Z|[+-]\d{2}:?\d{2})$", re.IGNORECASE)


@dataclass(frozen=True)
class FilingClock:
    """EDGAR acceptance evidence for one filing and the PIT availability it supports."""

    raw: str | None
    published_at: dt.datetime | None  # naive UTC; None when the stamp is date-only/naive/invalid
    available_at: dt.datetime | None  # naive UTC
    availability_status: str  # verified | modeled | unknown
    stamp_status: str  # exact | date_only | zone_unknown | missing | invalid


def filing_clock(raw_acceptance: object, filing_date: dt.date | None) -> FilingClock:
    """Acceptance clock of one filing.

    SEC's ``acceptanceDateTime`` is an offset-bearing UTC stamp. Legacy filings
    carry EDGAR midnight Eastern (a date, not a time): that is not publication
    evidence. ``available_at`` is at least the FC1 floor ``filed date + 46h``.
    """
    raw = str(raw_acceptance).strip() if raw_acceptance not in (None, "") else None
    floor = dt.datetime.combine(filing_date, dt.time()) + dt.timedelta(hours=46) if filing_date else None
    if raw is None:
        return FilingClock(None, None, floor, "modeled" if floor else "unknown", "missing")
    if not _OFFSET_SUFFIX.search(raw):
        return FilingClock(raw, None, floor, "modeled" if floor else "unknown", "zone_unknown")
    try:
        stamp = dt.datetime.fromisoformat(raw.replace("Z", "+00:00").replace("z", "+00:00"))
    except ValueError:
        return FilingClock(raw, None, floor, "modeled" if floor else "unknown", "invalid")
    if stamp.tzinfo is None:
        return FilingClock(raw, None, floor, "modeled" if floor else "unknown", "zone_unknown")
    local = stamp.astimezone(_EASTERN)
    utc = stamp.astimezone(dt.UTC).replace(tzinfo=None)
    if local.time() == dt.time(0, 0):
        return FilingClock(raw, None, floor, "modeled" if floor else "unknown", "date_only")
    available = max(utc, floor) if floor else utc
    return FilingClock(raw, utc, available, "verified", "exact")


# ---------------------------------------------------------------------------------------------
# Budgeted, cache-backed SEC document fetcher


#: Transient outcomes (network error = 0, throttling, SEC maintenance): ledgered, never final.
RETRYABLE_STATUSES = frozenset({0, 429, 500, 502, 503, 504})


class FetchBudgetExhausted(RuntimeError):
    """The RX5 pilot fetch budget is spent; no further network request is made."""


@dataclass(frozen=True)
class CachedResponse:
    url: str
    status: int
    path: Path
    sha256: str
    size: int
    fetched_at: dt.datetime  # naive UTC
    from_cache: bool

    def read(self) -> bytes:
        return self.path.read_bytes()


def archive_document_url(cik: str, accession: str, document: str) -> str:
    digits = accession.replace("-", "")
    if not re.fullmatch(r"\d{18}", digits):
        raise ValueError(f"invalid SEC accession {accession!r}")
    if not re.fullmatch(r"[A-Za-z0-9._\-/]+", document) or ".." in document:
        raise ValueError(f"invalid SEC document name {document!r}")
    return f"{SEC_ARCHIVES_ROOT}/{int(cik)}/{digits}/{document}"


class SecDocumentFetcher:
    """Fetch SEC archive documents once; every response is cached and ledgered.

    * Only the approved project user agent is accepted.
    * Network requests are spaced by ``min_interval_s`` (``>= 1/5 s``: RX5) and, on the default
      session, also take a token from the host-wide ``sec_http`` limiter (<= 5 req/s across all
      workers); that session makes one attempt per request so the budget sees every request.
    * ``budget`` caps *network requests* across runs: the ledger in
      ``cache_dir`` records every request (status, bytes, SHA-256, time), and a
      cached response is served without touching the network or the budget.
    * Non-200 responses are cached too, so a retry never re-spends budget.
    """

    def __init__(
        self,
        cache_dir: str | Path,
        *,
        budget: int = PILOT_FETCH_BUDGET,
        min_interval_s: float = DEFAULT_MIN_INTERVAL_S,
        user_agent: str = USER_AGENT,
        session: Any | None = None,
        timeout_s: float = 30.0,
        retry_backoff_s: float = 5.0,
        monotonic: Callable[[], float] = time.monotonic,
        sleep: Callable[[float], None] = time.sleep,
        now: Callable[[], dt.datetime] = lambda: dt.datetime.now(dt.UTC).replace(tzinfo=None),
        limiter: SecRateLimiter | None = None,
    ) -> None:
        if user_agent != USER_AGENT:
            raise ValueError("SEC document fetches require the approved project user agent")
        if min_interval_s < 1.0 / MAX_REQUESTS_PER_SECOND:
            raise ValueError("SEC document fetches must stay at or below 5 requests per second")
        if budget < 0:
            raise ValueError("fetch budget must be non-negative")
        self.cache_dir = Path(cache_dir)
        self.cache_dir.mkdir(parents=True, exist_ok=True)
        self.ledger_path = self.cache_dir / FETCH_LEDGER_NAME
        self.budget = budget
        self.min_interval_s = min_interval_s
        self.retry_backoff_s = retry_backoff_s
        self.timeout_s = timeout_s
        self._session = session
        self._limiter = limiter
        self._monotonic = monotonic
        self._sleep = sleep
        self._now = now
        self._last_request: float | None = None

    # -- ledger ---------------------------------------------------------------------------
    def ledger(self) -> list[dict[str, Any]]:
        if not self.ledger_path.exists():
            return []
        with self.ledger_path.open(encoding="utf-8") as handle:
            return [json.loads(line) for line in handle if line.strip()]

    @property
    def requests_used(self) -> int:
        return len(self.ledger())

    @property
    def requests_left(self) -> int:
        return max(self.budget - self.requests_used, 0)

    # -- cache ----------------------------------------------------------------------------
    def cache_path(self, url: str) -> Path:
        if not url.startswith(SEC_ARCHIVES_ROOT + "/"):
            raise ValueError(f"not an SEC archive URL: {url}")
        relative = url[len(SEC_ARCHIVES_ROOT) + 1 :]
        return self.cache_dir / "Archives" / "edgar" / "data" / Path(*relative.split("/"))

    def cached(self, url: str) -> CachedResponse | None:
        """A final cached response (200/404/...); transient failures are not final."""
        path = self.cache_path(url)
        meta_path = path.with_name(path.name + ".meta.json")
        if not (path.exists() and meta_path.exists()):
            return None
        meta = json.loads(meta_path.read_text(encoding="utf-8"))
        if int(meta["status"]) in RETRYABLE_STATUSES:
            return None
        payload = path.read_bytes()
        digest = hashlib.sha256(payload).hexdigest()
        if digest != meta.get("sha256"):
            raise ValueError(f"cached SEC document changed on disk: {path}")
        return CachedResponse(
            url=url,
            status=int(meta["status"]),
            path=path,
            sha256=digest,
            size=len(payload),
            fetched_at=dt.datetime.fromisoformat(meta["fetched_at"]),
            from_cache=True,
        )

    def _session_or_default(self) -> Any:
        if self._session is None:
            self._session = sec_session(USER_AGENT, limiter=self._limiter, max_attempts=1)
        return self._session

    def fetch(self, url: str, *, retries: int = 1) -> CachedResponse:
        """Cached response, else one budgeted request (plus ``retries`` on a transient status)."""
        hit = self.cached(url)
        if hit is not None:
            return hit
        response = self._request(url)
        while response.status in RETRYABLE_STATUSES and retries > 0 and self.requests_used < self.budget:
            retries -= 1
            self._sleep(self.retry_backoff_s)
            response = self._request(url)
        return response

    def _request(self, url: str) -> CachedResponse:
        if self.requests_used >= self.budget:
            raise FetchBudgetExhausted(f"C9 fetch budget of {self.budget} requests is spent")
        if self._last_request is not None:
            wait = self.min_interval_s - (self._monotonic() - self._last_request)
            if wait > 0:
                self._sleep(wait)
        self._last_request = self._monotonic()
        fetched_at = self._now()
        status, payload, content_type, error = 0, b"", None, None
        try:
            response = self._session_or_default().get(
                url,
                headers={"User-Agent": USER_AGENT, "Accept-Encoding": "gzip, deflate"},
                timeout=self.timeout_s,
                stream=True,
            )
            try:
                status = int(response.status_code)
                content_type = response.headers.get("Content-Type") if response.headers else None
                chunks: list[bytes] = []
                observed = 0
                for chunk in response.iter_content(chunk_size=64 * 1024):
                    observed += len(chunk)
                    if observed > MAX_DOCUMENT_BYTES:
                        error = "response_too_large"
                        break
                    chunks.append(chunk)
                payload = b"" if error else b"".join(chunks)
            finally:
                response.close()
        except Exception as exc:  # network failure is ledgered, never retried silently
            error = f"{type(exc).__name__}: {exc}"[:300]
        digest = hashlib.sha256(payload).hexdigest()
        path = self.cache_path(url)
        path.parent.mkdir(parents=True, exist_ok=True)
        meta = {
            "url": url,
            "status": status,
            "sha256": digest,
            "bytes": len(payload),
            "fetched_at": fetched_at.isoformat(),
            "content_type": content_type,
            "error": error,
            "user_agent": USER_AGENT,
        }
        path.write_bytes(payload)
        path.with_name(path.name + ".meta.json").write_text(json.dumps(meta, sort_keys=True), encoding="utf-8")
        with self.ledger_path.open("a", encoding="utf-8") as handle:
            handle.write(json.dumps(meta, sort_keys=True) + "\n")
        return CachedResponse(url, status, path, digest, len(payload), fetched_at, False)


# ---------------------------------------------------------------------------------------------
# Bulk tier: one streaming pass over the retained submissions archive

STAGING_DDL = (
    """CREATE TABLE IF NOT EXISTS c9_sec_entities (
        cik VARCHAR, name VARCHAR, entity_type VARCHAR, sic VARCHAR, tickers_json VARCHAR,
        exchanges_json VARCHAR, n_filings BIGINT, first_filing DATE, last_filing DATE,
        n_periodic BIGINT, first_periodic DATE, last_periodic DATE, n_identity BIGINT, member VARCHAR)""",
    """CREATE TABLE IF NOT EXISTS c9_sec_former_names (
        cik VARCHAR, name VARCHAR, from_raw VARCHAR, to_raw VARCHAR, ordinal INTEGER, member VARCHAR)""",
    """CREATE TABLE IF NOT EXISTS c9_sec_filings (
        cik VARCHAR, accession VARCHAR, form VARCHAR, filing_date DATE, report_date DATE,
        acceptance_raw VARCHAR, primary_document VARCHAR, primary_doc_description VARCHAR,
        file_number VARCHAR, film_number VARCHAR, act VARCHAR, items VARCHAR, size BIGINT, member VARCHAR,
        row_index INTEGER)""",
)
_FILING_FIELDS = (
    "accessionNumber",
    "form",
    "filingDate",
    "reportDate",
    "acceptanceDateTime",
    "primaryDocument",
    "primaryDocDescription",
    "fileNumber",
    "filmNumber",
    "act",
    "items",
    "size",
)


def _date(value: object) -> dt.date | None:
    if not value:
        return None
    try:
        return dt.date.fromisoformat(str(value)[:10])
    except ValueError:
        return None


@dataclass
class _EntityAccumulator:
    """Per-issuer activity summary; ISO date strings compare lexically (converted once)."""

    n_filings: int = 0
    first_filing: str | None = None
    last_filing: str | None = None
    n_periodic: int = 0
    first_periodic: str | None = None
    last_periodic: str | None = None
    n_identity: int = 0


def _text(values: Sequence[Any], index: int) -> str | None:
    if index >= len(values):
        return None
    text = str(values[index] if values[index] is not None else "").strip()
    return text or None


def _filing_rows(
    cik: str, block: Mapping[str, Any], member: str, totals: _EntityAccumulator
) -> Iterator[tuple[object, ...]]:
    columns = {name: block.get(name) or [] for name in _FILING_FIELDS}
    forms = columns["form"]
    dates = columns["filingDate"]
    for index, raw_form in enumerate(forms):
        form = str(raw_form or "").strip()
        filed = str(dates[index])[:10] if index < len(dates) and dates[index] else None
        totals.n_filings += 1
        if filed is not None:
            if totals.first_filing is None or filed < totals.first_filing:
                totals.first_filing = filed
            if totals.last_filing is None or filed > totals.last_filing:
                totals.last_filing = filed
        if form in PERIODIC_FORMS:
            totals.n_periodic += 1
            if filed is not None:
                if totals.first_periodic is None or filed < totals.first_periodic:
                    totals.first_periodic = filed
                if totals.last_periodic is None or filed > totals.last_periodic:
                    totals.last_periodic = filed
        if not is_identity_form(form):
            continue
        totals.n_identity += 1
        size = columns["size"][index] if index < len(columns["size"]) else None
        yield (
            cik,
            _text(columns["accessionNumber"], index),
            form,
            _date(filed),
            _date(_text(columns["reportDate"], index)),
            _text(columns["acceptanceDateTime"], index),
            _text(columns["primaryDocument"], index),
            _text(columns["primaryDocDescription"], index),
            _text(columns["fileNumber"], index),
            _text(columns["filmNumber"], index),
            _text(columns["act"], index),
            _text(columns["items"], index),
            int(size) if isinstance(size, int | float) or (isinstance(size, str) and size.isdigit()) else None,
            member,
            index,
        )


@dataclass
class ScanStats:
    archive_sha256: str = ""
    main_members: int = 0
    history_members_read: int = 0
    history_members_missing: int = 0
    entities: int = 0
    filings_seen: int = 0
    identity_filings: int = 0
    former_names: int = 0
    unreadable_members: list[str] = field(default_factory=list)
    seconds: float = 0.0


def scan_submissions_identity(
    archive_path: str | Path,
    con: duckdb.DuckDBPyConnection,
    *,
    limit: int | None = None,
    batch_entities: int = 20_000,
    progress: Callable[[ScanStats], None] | None = None,
) -> ScanStats:
    """Stream the retained ``submissions.zip`` once into the ``c9_sec_*`` staging tables.

    Every main member (one CIK) and each of its history members is read one at
    a time (bounded memory); only identity filings (8-A, exchange
    certifications, Form 25/25-NSE, Form 15), ``formerNames`` windows and a
    per-issuer activity summary are kept. Unreadable members are counted, never
    skipped silently.
    """
    import pyarrow as pa

    for statement in STAGING_DDL:
        con.execute(statement)
    stats = ScanStats()
    started = time.monotonic()
    entities: list[tuple[object, ...]] = []
    names: list[tuple[object, ...]] = []
    filings: list[tuple[object, ...]] = []

    def flush() -> None:
        for table, rows, width in (
            ("c9_sec_entities", entities, 14),
            ("c9_sec_former_names", names, 6),
            ("c9_sec_filings", filings, 15),
        ):
            if not rows:
                continue
            arrow = pa.table({f"c{i}": [row[i] for row in rows] for i in range(width)})
            con.register("c9_scan_batch", arrow)
            try:
                con.execute(f"INSERT INTO {table} SELECT * FROM c9_scan_batch")
            finally:
                con.unregister("c9_scan_batch")
            rows.clear()

    with SubmissionsArchive(Path(archive_path)) as archive:
        stats.archive_sha256 = archive.sha256
        for member in archive.main_members():
            if limit is not None and stats.main_members >= limit:
                break
            stats.main_members += 1
            match = re.fullmatch(r"CIK(\d{10})\.json", member)
            if match is None:
                continue
            cik = match.group(1)
            try:
                data = json.loads(archive.read(member))
            except (ValueError, KeyError, OSError) as exc:
                stats.unreadable_members.append(f"{member}: {type(exc).__name__}")
                continue
            totals = _EntityAccumulator()
            submissions = data.get("filings") or {}
            filings.extend(_filing_rows(cik, submissions.get("recent") or {}, member, totals))
            for extra in submissions.get("files") or ():
                history = str((extra or {}).get("name") or "")
                if not history or history not in archive:
                    stats.history_members_missing += 1
                    continue
                try:
                    block = json.loads(archive.read(history))
                except (ValueError, KeyError, OSError) as exc:
                    stats.unreadable_members.append(f"{history}: {type(exc).__name__}")
                    continue
                stats.history_members_read += 1
                filings.extend(_filing_rows(cik, block, history, totals))
            for ordinal, former in enumerate(data.get("formerNames") or ()):
                names.append(
                    (cik, former.get("name"), former.get("from"), former.get("to"), ordinal, member)
                )
            entities.append(
                (
                    cik,
                    data.get("name"),
                    data.get("entityType"),
                    None if data.get("sic") in (None, "") else str(data.get("sic")),
                    json.dumps(data.get("tickers") or []),
                    json.dumps(data.get("exchanges") or []),
                    totals.n_filings,
                    _date(totals.first_filing),
                    _date(totals.last_filing),
                    totals.n_periodic,
                    _date(totals.first_periodic),
                    _date(totals.last_periodic),
                    totals.n_identity,
                    member,
                )
            )
            stats.filings_seen += totals.n_filings
            stats.identity_filings += totals.n_identity
            stats.former_names += len(data.get("formerNames") or ())
            stats.entities += 1
            if len(entities) >= batch_entities:
                flush()
                stats.seconds = time.monotonic() - started
                if progress is not None:
                    progress(stats)
        flush()
    stats.seconds = time.monotonic() - started
    return stats


# ---------------------------------------------------------------------------------------------
# Class-title classification (L2 §5.3 precedence: exclusions before common/ADR)

_CLASS_RULES: tuple[tuple[str, re.Pattern[str]], ...] = tuple(
    (kind, re.compile(pattern, re.IGNORECASE))
    for kind, pattern in (
        # An American depositary share is an ADR even when it represents "units of ordinary stock".
        ("adr", r"^\W*american\s+deposit[ao]ry\s+(?:shares|receipts)\b(?!.*\bprefer)"),
        ("unit", r"^\W*units?\W*$"),
        # An exchange-traded product named explicitly wins over its holdings' words ("... Bonds ETF").
        # ... unless the class is a note/certificate merely *linked to* an ETF (then it is debt/other).
        ("etp", r"^(?!.*\blinked\b).*(?:\bexchange[-\s]traded\b|\bETF\b|\bETN\b)"),
        ("debt", r"\bnotes?\b|\bdebentures?\b|\bbonds?\b|\bdebt\s+securities\b|\b\d+(?:\.\d+)?\s*%.{0,60}\bdue\b"),
        ("unit", r"\bunits?\b(?!.{0,40}\blimited\s+partnership\b)(?=.{0,80}\b(?:warrant|right|consisting|each)\b)"
                 r"|\b(?:tangible\s+equity|equity|corporate|income\s+PRIDES|purchase\s+contract)\s+units?\b"),
        ("warrant", r"\bwarrants?\b"),
        ("right", r"\brights?\b"),
        ("preferred", r"\bpreferred\b|\bpreference\s+shares?\b|\btrust\s+preferred\b|\bcapital\s+securities\b"),
        ("adr", r"\bamerican\s+deposit[ao]ry\b|\bADSs?\b|\bADRs?\b|\bdeposit[ao]ry\s+(?:shares|receipts)\b"),
        ("lp_unit", r"\bcommon\s+units?\b|\blimited\s+partnership\s+(?:interests?|units?)\b|\bunits?\s+representing\b"),
        ("common", r"\bcommon\s+(?:stock|shares?)\b|\bordinary\s+shares?\b|\bcapital\s+stock\b"),
        ("beneficial_interest", r"\bshares?\s+of\s+beneficial\s+interest\b|\bcommon\s+beneficial\s+interest\b"),
        ("common", r"^\s*(?:class\s+[A-Z]\s+)?shares?\b"),
    )
)
_SHARE_CLASS = re.compile(r"\bclass\s+([A-Z])\b|\bseries\s+([A-Z0-9]{1,3})\b", re.IGNORECASE)


def classify_class_title(title: str | None) -> tuple[str, str | None]:
    """``(security_type, share_class)`` of a filing's class title; ``('unknown', None)`` if unmatched.

    Exclusions (debt, units, warrants, rights, preferred, exchange-traded
    products) win over common/ADR: a unit containing common stock is a unit, a
    depositary share representing preferred is preferred. A name that matches
    nothing stays ``unknown`` -- never common by default.
    """
    if not title or not title.strip():
        return "unknown", None
    text = re.sub(r"\s+", " ", title).strip()
    # Rights *attached* to a class ("Common Stock ... with associated Preferred Stock Purchase Rights")
    # do not change the class itself.
    text = re.sub(
        r"\s*,?\s*(?:(?:together\s+)?with|including)\s+(?:the\s+)?(?:associated|attached)\b.*$", "", text, flags=re.IGNORECASE
    )
    kind = next((kind for kind, pattern in _CLASS_RULES if pattern.search(text)), "unknown")
    share_class = None
    match = _SHARE_CLASS.search(text)
    if match:
        share_class = (match.group(1) or match.group(2) or "").upper() or None
    return kind, share_class


def split_class_list(description: str) -> list[str]:
    """Split a removal notice's class description that lists several classes.

    ``;`` always separates classes. A comma or ``and`` separates classes only
    when every resulting part classifies and the parts are distinct classes
    (``Class A Common Stock, Warrant, Unit``; ``Class A Common Stock and Class B
    Common Stock``) -- never ``Common Stock, par value $0.01``.
    """
    def split(chunk: str, pattern: str, distinct: Callable[[tuple[str, str | None]], object]) -> list[str] | None:
        pieces = [piece.strip(" ,;") for piece in re.split(pattern, chunk) if piece.strip(" ,;")]
        kinds = [classify_class_title(piece) for piece in pieces]
        if len(pieces) > 1 and all(kind != "unknown" for kind, _ in kinds) and len({distinct(k) for k in kinds}) == len(kinds):
            return pieces
        return None

    parts: list[str] = []
    for chunk in (part.strip(" ,;") for part in description.split(";")):
        if not chunk:
            continue
        class_word = r"(?=(?:Class|Series|Common|Ordinary|Preferred|Warrants?|Units?|Rights?)\b)"
        # "Ordinary Shares, Warrants, Rights, and Units": commas (and a final "and") separate
        # classes of different types.
        by_list = split(chunk, r",\s*(?:and\s+)?(?=[A-Z])|\s+and\s+" + class_word, lambda k: k[0])
        # "Class A Common Stock and Class B Common Stock": same type, distinct share classes.
        by_and = split(chunk, r"\s+and\s+" + class_word, lambda k: k)
        parts.extend(by_list or by_and or [chunk])
    return parts


def class_key(title: str | None) -> str:
    """Stable per-issuer class key: type, share class and (for non-common) a title slug."""
    kind, share_class = classify_class_title(title)
    base = kind if share_class is None else f"{kind}:{share_class.lower()}"
    if kind in ("common", "adr", "beneficial_interest", "lp_unit"):
        return base
    text = re.sub(r"par\s+value.*$|,?\s*no\s+par.*$|\$[\d.,]+", "", (title or "").lower())
    slug = re.sub(r"[^a-z0-9%.]+", "-", text).strip("-")[:60]
    return f"{base}:{slug}" if slug else base


# ---------------------------------------------------------------------------------------------
# Document parsing


@dataclass(frozen=True)
class RegisteredClass:
    title: str
    exchange_text: str | None
    mic: str | None
    symbol: str | None = None


@dataclass
class ParsedDocument:
    """What one filing document states (``status='parsed'`` only when the required facts are present)."""

    status: str
    form: str
    issuer_cik: str | None = None
    issuer_name: str | None = None
    exchange_name: str | None = None
    exchange_cik: str | None = None
    classes: list[RegisteredClass] = field(default_factory=list)
    rule_provision: str | None = None
    signature_date: dt.date | None = None
    stated_effective_date: dt.date | None = None
    symbols: list[str] = field(default_factory=list)
    file_number: str | None = None
    notes: list[str] = field(default_factory=list)


_CELL_BREAK = re.compile(r"</t[dh]\s*>", re.IGNORECASE)
_LINE_BREAK = re.compile(r"<br\s*/?>|</(?:tr|p|div|h\d|li|table|center)\s*>", re.IGNORECASE)
_TAG = re.compile(r"<[^>]+>")
_UNICODE_SPACES = "".join(map(chr, (0x00A0, 0x2002, 0x2003, 0x2009, 0x200B)))
_SPACES = re.compile("[ \t\r\f\v" + _UNICODE_SPACES + "]+")


def document_text(payload: bytes) -> str:
    """Plain text of an HTML/TXT filing document: one line per row/paragraph, cells joined by `` | ``.

    Legacy EDGAR documents are Windows-1252 (curly quotes as 0x93/0x94); UTF-8 is tried first.
    """
    try:
        raw = payload.decode("utf-8")
    except UnicodeDecodeError:
        raw = payload.decode("cp1252", errors="replace")
    if re.search(r"<(?:html|table|p|div|font|td)\b", raw, re.IGNORECASE):
        raw = re.sub(r"(?is)<(script|style)\b.*?</\1\s*>", " ", raw)
        raw = _CELL_BREAK.sub(" | ", raw)
        raw = _LINE_BREAK.sub("\n", raw)
        raw = _TAG.sub(" ", raw)
        raw = unescape(raw)
    lines = []
    for line in raw.splitlines():
        cells = [_SPACES.sub(" ", cell).strip() for cell in line.split(" | ")]
        cells = [cell for cell in cells if cell and cell not in ("|",)]
        if cells:
            lines.append(" | ".join(cells))
    return "\n".join(lines)


_MONTH_DATE = re.compile(
    r"\b(January|February|March|April|May|June|July|August|September|October|November|December)"
    r"\s+(\d{1,2}),?\s+(\d{4})\b",
    re.IGNORECASE,
)


def _month_date(text: str) -> dt.date | None:
    match = _MONTH_DATE.search(text)
    if not match:
        return None
    try:
        return dt.datetime.strptime(f"{match.group(1).title()} {match.group(2)} {match.group(3)}", "%B %d %Y").date()
    except ValueError:
        return None


_SYMBOL_TOKEN = r"([A-Z][A-Z0-9]{0,5}(?:[.\-/][A-Z0-9]{1,3})?)"
_QUOTE = "[\"'" + "".join(map(chr, (0x2018, 0x2019, 0x201C, 0x201D))) + "]*"
_SYMBOL_PATTERNS = (
    re.compile(
        r"(?i:under\s+the\s+(?:new\s+)?(?:ticker\s+|trading\s+)?symbols?)\s*(?:of\s*)?[:\-]?\s*"
        + _QUOTE + _SYMBOL_TOKEN + _QUOTE
    ),
    re.compile(r"(?i:(?:ticker|trading)\s+symbols?)\s*(?i:is|of|will\s+be)?\s*[:\-]?\s*" + _QUOTE + _SYMBOL_TOKEN + _QUOTE),
    re.compile(
        r"\((?i:NYSE(?:\s+(?:American|Arca|MKT|Amex))?|NASDAQ(?:\s*GS|\s*GM|\s*CM)?|Nasdaq|AMEX|NYSE\s+American)"
        r"\s*:\s*" + _SYMBOL_TOKEN + r"\)"
    ),
)
_NOT_SYMBOLS = frozenset(
    {"THE", "AND", "OF", "NYSE", "NASDAQ", "AMEX", "LLC", "INC", "CORP", "CLASS", "COMMON", "STOCK", "SERIES", "N/A",
     "NA", "NONE", "TBD", "IS", "WILL", "BE", "OR", "TO", "ON", "AS", "AN", "FOR", "USD"}
)


def stated_symbols(text: str) -> list[str]:
    """Trading symbols a filing explicitly states ("under the symbol "XYZ"", "(NYSE: XYZ)")."""
    found: list[str] = []
    for pattern in _SYMBOL_PATTERNS:
        for match in pattern.finditer(text):
            token = match.group(1).strip().upper()
            key = normalize_symbol(token)
            if key and token not in _NOT_SYMBOLS and key not in found:
                found.append(key)
    return found


def _xml_local(root: ElementTree.Element, path: Sequence[str]) -> str | None:
    node: ElementTree.Element | None = root
    for name in path:
        if node is None:
            return None
        node = next((child for child in node if child.tag.rsplit("}", 1)[-1] == name), None)
    if node is None or node.text is None:
        return None
    return node.text.strip() or None


def parse_removal_xml(payload: bytes, form: str) -> ParsedDocument:
    """Form 25 / 25-NSE XML (``notificationOfRemoval``)."""
    try:
        root = ElementTree.fromstring(payload)
    except ElementTree.ParseError:
        return ParsedDocument(status="unreadable", form=form, notes=["xml_parse_error"])
    if root.tag.rsplit("}", 1)[-1] != "notificationOfRemoval":
        return ParsedDocument(status="unreadable", form=form, notes=[f"unexpected_root:{root.tag}"])
    issuer_cik = _xml_local(root, ("issuer", "cik"))
    exchange_name = _xml_local(root, ("exchange", "entityName"))
    title = _xml_local(root, ("descriptionClassSecurity",))
    signed = _xml_local(root, ("signatureData", "signatureDate"))
    mic = exchange_mic(exchange_name)
    document = ParsedDocument(
        status="parsed",
        form=form,
        issuer_cik=normalize_cik(issuer_cik) if issuer_cik else None,
        issuer_name=_xml_local(root, ("issuer", "entityName")),
        exchange_name=exchange_name,
        exchange_cik=_xml_local(root, ("exchange", "cik")),
        classes=[RegisteredClass(part, exchange_name, mic) for part in split_class_list(title)] if title else [],
        rule_provision=_xml_local(root, ("ruleProvision",)),
        signature_date=_date(signed),
        file_number=_xml_local(root, ("issuer", "fileNumber")),
    )
    if not title:
        document.status, document.notes = "no_class", ["descriptionClassSecurity missing"]
    elif mic is None:
        document.status, document.notes = "no_exchange", [f"exchange not recognized: {exchange_name!r}"]
    return document


#: Checked-box glyphs (x, ballot box with x, Wingdings-as-Latin-1 thorn/y-acute, check marks, squares).
_CHECKED = "xX" + "".join(map(chr, (0x2612, 0x00FE, 0x00FD, 0x2713, 0x2714, 0x25A0, 0x2611)))
#: Unchecked-box glyphs (Wingdings-as-Latin-1 diaeresis, "o", ballot box).
_UNCHECKED = "o" + "".join(map(chr, (0x00A8, 0x2610)))


def parse_removal_html(payload: bytes, form: str) -> ParsedDocument:
    """Issuer-filed Form 25 (HTML/TXT): class, issuer / exchange line, checked rule provision."""
    text = document_text(payload)
    flat = re.sub(r"\s*\n\s*", " ", text)
    document = ParsedDocument(status="parsed", form=form)
    names = re.search(r"([^()]{3,300}?)\s*\(\s*Exact\s+name\s+of\s+Issuer", flat, re.IGNORECASE)
    if names:
        segment = re.sub(r"^.*?Commission\s+File\s+Number\s*:?\s*[\d\-]+\s*", "", names.group(1), flags=re.IGNORECASE)
        parts = [part.strip(" |,;_-") for part in re.split(r"\s+/\s+|\s*\|\s*|/", segment) if part.strip(" |,;_-")]
        exchange_parts = [part for part in parts if exchange_mic(part)]
        document.exchange_name = exchange_parts[-1] if exchange_parts else None
        issuer_parts = [part for part in parts if part not in exchange_parts]
        document.issuer_name = issuer_parts[0] if issuer_parts else None
        if document.issuer_name is None and len(exchange_parts) == 1:
            # "<Issuer> <Exchange>" without a separator: split at the exchange name.
            start = min(
                (match.start() for _, pattern in _EXCHANGE_PATTERNS for match in [pattern.search(exchange_parts[0])] if match),
                default=0,
            )
            start = max(exchange_parts[0].rfind(" The ", 0, start + 1), 0) if start else 0
            if start > 0:
                document.issuer_name = exchange_parts[0][:start].strip(" ,;")
                document.exchange_name = exchange_parts[0][start:].strip(" ,;")
    title = re.search(
        r"(?:\(\s*Address[^()]*\)|principal\s+executive\s+offices\s*\))\s*(.{3,400}?)\s*\(\s*Description\s+of\s+class",
        flat,
        re.IGNORECASE,
    )
    if title is None:
        title = re.search(r"([^()]{3,300}?)\s*\(\s*Description\s+of\s+class", flat, re.IGNORECASE)
    if document.exchange_name is None:
        # The exchange is sometimes printed apart from the issuer line: first exchange name on the cover.
        cover = flat[: title.start() if title else 3000]
        cover = re.sub(r"securities\s+exchange\s+act|exchange\s+act", " ", cover, flags=re.IGNORECASE)
        hits = [(match.start(), match.group(0)) for _, pattern in _EXCHANGE_PATTERNS for match in [pattern.search(cover)] if match]
        if hits:
            document.exchange_name = min(hits)[1]
            document.notes.append("exchange_found_on_cover_not_issuer_line")
    mic = exchange_mic(document.exchange_name)
    if title:
        class_title = title.group(1).strip(" |,;")
        if re.search(r"\(Address|telephone|zip\s+code|Exact\s+name", class_title, re.IGNORECASE):
            document.notes.append(f"class_title_rejected:{class_title[:60]}")
        else:
            parts = [
                piece
                for part in re.split(r"\bper\s+share\b\s*(?=[A-Z])", class_title)
                for piece in split_class_list(part)
            ]
            document.classes = [
                RegisteredClass(part, document.exchange_name, mic) for part in parts if re.search(r"[A-Za-z]{3}", part)
            ]
    for provision in re.finditer(r"17\s*CFR\s*240\.12d2-2\s*\(([a-d])\)(?:\s*\((\d)\))?", flat):
        window = flat[max(provision.start() - 40, 0) : provision.start()]
        mark = re.search("([" + _CHECKED + _UNCHECKED + r"])\s*(?:Pursuant\s+to\s*)?$", window)
        if mark and mark.group(1) in _CHECKED:
            document.rule_provision = "17 CFR 240.12d2-2(" + provision.group(1) + ")" + (
                f"({provision.group(2)})" if provision.group(2) else ""
            )
            break
    effective = re.search(
        r"opening\s+of\s+(?:business|the\s+trading\s+session|trading)\s+on\s+(.{0,40})", flat, re.IGNORECASE
    )
    if effective:
        document.stated_effective_date = _month_date(effective.group(1))
    document.symbols = stated_symbols(flat)
    if not document.classes:
        document.status, document.notes = "no_class", ["class description not found"]
    elif mic is None:
        document.status, document.notes = "no_exchange", [f"exchange not recognized: {document.exchange_name!r}"]
    return document


_EIGHT_A_STOP = re.compile(
    r"If\s+this\s+form\s+relates|Securities\s+Act\s+registration|pursuant\s+to\s+Section\s+12\s*\(\s*g\s*\)"
    r"|Item\s+1\.|INFORMATION\s+REQUIRED",
    re.IGNORECASE,
)
_HEADER_NOISE = re.compile(
    r"title\s+of\s+each\s+class|name\s+of\s+each\s+exchange|to\s+be\s+so\s+registered|to\s+be\s+registered"
    r"|each\s+class\s+is|trading\s+symbol|^\(?\s*none\s*\)?\.?$|on\s+which|^\W*(?:so\s+)?registered\W*$",
    re.IGNORECASE,
)
_TICKER_CELL = re.compile(r"^[A-Z][A-Z0-9]{0,5}(?:[.\-/][A-Z0-9]{1,3})?$")


def _split_exchange_cell(cell: str) -> tuple[str, str]:
    """``(title prefix, exchange text)`` for a cell holding both (legacy plain-text covers)."""
    starts = [match.start() for _, pattern in _EXCHANGE_PATTERNS for match in [pattern.search(cell)] if match]
    start = min(starts) if starts else 0
    before = cell[:start]
    article = re.search(r"\bThe\s*$", before, re.IGNORECASE)
    if article:
        start = article.start()
    return cell[:start].strip(" ,;"), cell[start:].strip(" ,;")


_REGISTRATION_HEADER_WORDS = re.compile(
    r"title\s+(?:of|for)\s+each\s+class|name\s+of\s+each\s+exchange(?:\s+on)?(?:\s+which)?"
    r"|(?:on\s+)?(?:which\s+)?each\s+class\s+is\s+to\s+be(?:\s+so)?\s+registered|to\s+be\s+(?:so\s+)?registered"
    r"|trading\s+symbol(?:\(s\)|s)?|\(if\s+applicable\)|^\W*none\W*$",
    re.IGNORECASE,
)
_RULES = re.compile(r"[-_=*]{3,}")
_REG_12B_START = re.compile(
    r"Section\s+12\s*\(\s*b\s*\)\s+of\s+the\s+(?:Exchange\s+)?Act\s*:?|Title\s+(?:of|for)\s+each\s+class", re.IGNORECASE
)
_REG_BLOCK_STOP = re.compile(
    r"If\s+this\s+form\s+relates|Securities\s+Act\s+registration|Securities\s+to\s+be\s+registered\s+pursuant\s+to"
    r"\s+Section\s+12\s*\(\s*g|Item\s+1\b|INFORMATION\s+REQUIRED",
    re.IGNORECASE,
)


def _flat_registration_classes(flat: str) -> list[RegisteredClass]:
    """Fallback for covers whose header/title/exchange wrap across lines (incl. legacy two-column text).

    The 12(b) region runs from ``Section 12(b) of the Act:`` (or the ``Title of
    each class`` header) to the next cover landmark; header words and rule
    lines are dropped and the remainder is split at exchange names. Only the
    cover (before ``Item 1``) is searched.
    """
    cover_end = re.search(r"Item\s+1\b", flat, re.IGNORECASE)
    cover = flat[: cover_end.start()] if cover_end else flat[:6000]
    for start in _REG_12B_START.finditer(cover):
        stop = _REG_BLOCK_STOP.search(cover, start.end())
        classes = _split_registration_block(cover[start.end() : stop.start() if stop else len(cover)])
        if classes:
            return classes
    return []


def _split_registration_block(block: str) -> list[RegisteredClass]:
    block = _RULES.sub(" ", block.replace("|", " "))
    block = re.sub(r"\s+", " ", _REGISTRATION_HEADER_WORDS.sub(" ", block)).strip(" :")
    classes: list[RegisteredClass] = []
    position = 0
    while block[position:]:
        hits = [(match.start(), match.end(), mic) for mic, pattern in _EXCHANGE_PATTERNS
                for match in [pattern.search(block, position)] if match]
        if not hits:
            break
        # Earliest exchange name; at the same start the longest wins ("NYSE Amex" over "NYSE").
        start, negative_end, mic = min((hit_start, -hit_end, hit_mic) for hit_start, hit_end, hit_mic in hits)
        end = -negative_end
        # Extend the exchange name to its usual trailing words ("Stock Market LLC", "Exchange, Inc.").
        tail = re.match(r"(?:[\s,]+(?:Global|Select|Capital|Stock|Market|Exchange|LLC|L\.L\.C\.|Inc\.?|Arca|American))*",
                        block[end:], re.IGNORECASE)
        end += tail.end() if tail else 0
        title = re.sub(r"^(?:the|and|none\.?)\s+|\s+(?:the)$", "", block[position:start].strip(" ,;:"), flags=re.IGNORECASE)
        if title and re.search(r"[A-Za-z]{3}", title):
            classes.append(RegisteredClass(title, block[start:end].strip(" ,;"), mic))
        position = end
    return classes


def parse_registration(payload: bytes, form: str) -> ParsedDocument:
    """Form 8-A12B/8-A12G cover: issuer, registered class titles, exchange(s), stated symbols."""
    text = document_text(payload)
    lines = text.splitlines()
    flat = re.sub(r"\s*\n\s*", " ", text)
    document = ParsedDocument(status="parsed", form=form)
    issuer = re.search(r"([^()|]{3,200}?)\s*\|?\s*\(\s*Exact\s+name\s+of\s+registrant", flat, re.IGNORECASE)
    if issuer:
        name = re.sub(r"^.*?(?:OF\s+1934|FORM\s+8-A)\s*", "", issuer.group(1), flags=re.IGNORECASE)
        document.issuer_name = re.sub(r"^[\s_\-=*|]+|[\s_\-=*|]+$", "", name) or None
    classes: list[RegisteredClass] = []
    header = next((i for i, line in enumerate(lines) if re.search(r"Title\s+of\s+each\s+class", line, re.IGNORECASE)), None)
    if header is not None and not form.startswith("8-A12G"):
        pending: str | None = None
        for line in lines[header + 1 : header + 40]:
            if _EIGHT_A_STOP.search(line):
                break
            cells = [
                cell.strip()
                for cell in _RULES.sub(" ", line).split(" | ")
                if re.search(r"[A-Za-z]", cell) and not _HEADER_NOISE.search(cell)
                and not re.fullmatch(r"\W*the\W*", cell, re.IGNORECASE)  # "The | Nasdaq Stock Market LLC"
            ]
            if not cells:
                continue
            exchange_cells = [cell for cell in cells if exchange_mic(cell)]
            symbol_cells = [cell for cell in cells if _TICKER_CELL.match(cell) and cell not in _NOT_SYMBOLS]
            title_cells = [cell for cell in cells if cell not in exchange_cells and cell not in symbol_cells]
            if exchange_cells and not title_cells:
                prefix, exchange_text = _split_exchange_cell(exchange_cells[0])
                if re.search(r"[A-Za-z]{3}", prefix):
                    title_cells, exchange_cells = [prefix], [exchange_text]
            if exchange_cells:
                title = " ".join(title_cells) or pending
                if title:
                    classes.append(
                        RegisteredClass(
                            title.strip(" ,;"),
                            exchange_cells[0],
                            exchange_mic(exchange_cells[0]),
                            normalize_symbol(symbol_cells[0]) if symbol_cells else None,
                        )
                    )
                pending = None
            elif title_cells:
                pending = " ".join(title_cells) if pending is None else f"{pending} {' '.join(title_cells)}"
    if not classes and not form.startswith("8-A12G"):
        classes = _flat_registration_classes(flat)
    if form.startswith("8-A12G"):
        for section in re.finditer(
            r"Section\s+12\s*\(\s*g\s*\)\s+of\s+the\s+(?:Exchange\s+)?Act\s*:?\s*(.{2,300}?)\s*"
            r"(?:\(\s*Title\s+of\s+(?:each\s+)?class|Item\s+1\b)",
            flat,
            re.IGNORECASE,
        ):
            title = _RULES.sub(" ", section.group(1)).strip(" |,;.")
            if re.match(r"and\s+is\s+effective|none\b", title, re.IGNORECASE) or not re.search(r"[A-Za-z]{3}", title):
                continue
            # A 12(g) block laid out as a table may name a market (pre-August-2006 Nasdaq NMS classes).
            classes = _split_registration_block(title)
            if not classes:
                cleaned = re.sub(r"\s+", " ", _REGISTRATION_HEADER_WORDS.sub(" ", title)).strip(" |,;.:")
                classes = [RegisteredClass(cleaned, None, None)] if re.search(r"[A-Za-z]{3}", cleaned) else []
            if classes:
                break
    # One cover row can list several classes ("1.900% Notes due 2019; 2.400% Notes due 2021").
    document.classes = [
        RegisteredClass(part, item.exchange_text, item.mic, item.symbol)
        for item in classes
        for part in split_class_list(item.title)
    ]
    document.symbols = stated_symbols(flat)
    for registered in classes:
        if registered.symbol and registered.symbol not in document.symbols:
            document.symbols.append(registered.symbol)
    if not classes:
        document.status, document.notes = "no_class", ["registered class not found on cover"]
    elif form.startswith("8-A12B") and not any(item.mic for item in classes):
        document.status, document.notes = "no_exchange", ["no recognized exchange on 12(b) cover"]
    return document


def parse_document(payload: bytes, form: str, document_name: str) -> ParsedDocument:
    """Dispatch on form and document type."""
    base = form.split("/")[0]
    if base in ("25", "25-NSE"):
        if document_name.lower().endswith(".xml") or payload.lstrip().startswith(b"<?xml"):
            return parse_removal_xml(payload, form)
        return parse_removal_html(payload, form)
    if base in ("8-A12B", "8-A12G"):
        return parse_registration(payload, form)
    return ParsedDocument(status="unsupported_form", form=form)


def raw_document_name(primary_document: str) -> str:
    """EDGAR serves XML forms through an XSL rendering path; the raw XML sits at the folder root."""
    if re.match(r"xsl[^/]+/", primary_document):
        return primary_document.split("/", 1)[1]
    return primary_document


# ---------------------------------------------------------------------------------------------
# Bulk tier: exchange-certification document names that carry the certified symbol

#: Nasdaq / Cboe certification documents are named after the certified symbol, e.g.
#: ``8A_Cert_GLADL.pdf``, ``GLADZ_8-A_Cert.pdf``, ``8a_cert_gladd.pdf``.
_CERT_NAME_PATTERNS = (
    re.compile(r"^8-?A_?Cert_([A-Za-z]{1,6})(?:_\d+)?\.pdf$", re.IGNORECASE),
    re.compile(r"^([A-Za-z]{1,6})_8-?A_?Cert(?:_\d+)?\.pdf$", re.IGNORECASE),
)
#: NYSE-family certifications: ``<SYMBOL>[8A]<MMDDYY>.pdf`` (``BCAT8A092220.pdf``, ``COSO070125.pdf``).
#: Accepted only when the embedded date is within a week of the filing date.
_CERT_NYSE_NAME = re.compile(r"^([A-Z]{1,5})(?:8A)?(\d{2})(\d{2})(\d{2})\.pdf$")


def cert_document_symbol(primary_document: str | None, filing_date: dt.date | None = None) -> str | None:
    """Symbol encoded in an exchange certification document name, else ``None``.

    Mixed-case names (fund-family words such as ``Emles``) are rejected; the
    NYSE-family form must carry its own filing date.
    """
    if not primary_document:
        return None
    name = primary_document.strip()
    for pattern in _CERT_NAME_PATTERNS:
        match = pattern.match(name)
        if match and (match.group(1).isupper() or match.group(1).islower()):
            return normalize_symbol(match.group(1))
    match = _CERT_NYSE_NAME.match(name)
    if match and filing_date is not None:
        try:
            stamped = dt.date(2000 + int(match.group(4)), int(match.group(2)), int(match.group(3)))
        except ValueError:
            return None
        if abs((stamped - filing_date).days) <= 7:
            return normalize_symbol(match.group(1))
    return None


# ---------------------------------------------------------------------------------------------
# Vendor price lines (TickerHistory3): bounded aggregate, never a per-bar frame

_SQL_SYMBOL = "nullif(trim(both '-' from regexp_replace(upper(trim({col})), '[./\\s]+', '-', 'g')), '')"


def stage_vendor_lines(con: duckdb.DuckDBPyConnection, parquet_path: str | Path, *, ceased_gap_days: int = 7) -> dict[str, object]:
    """``c9_vendor_symbols`` (line x historical symbol span) and ``c9_vendor_lines`` (line span).

    Streaming group-by over a projection of ``securityID, tradingDate,
    ticker_tk`` (positive vendor ids only; ``todayTicker`` is never used as a
    historical key). A line is ``ceased`` when its last bar is more than
    ``ceased_gap_days`` before the file's last date.
    """
    symbol = _SQL_SYMBOL.format(col="ticker_tk")
    con.execute(
        f"""
        CREATE OR REPLACE TABLE c9_vendor_symbols AS
        SELECT CAST(securityID AS BIGINT) AS vendor_id, {symbol} AS symbol,
               min(tradingDate) AS first_date, max(tradingDate) AS last_date, count(*) AS bars
        FROM read_parquet(?)
        WHERE securityID > 0 AND {symbol} IS NOT NULL AND tradingDate IS NOT NULL
        GROUP BY ALL
        """,
        [str(parquet_path)],
    )
    file_last = con.execute("SELECT max(last_date) FROM c9_vendor_symbols").fetchone()[0]
    con.execute(
        """
        CREATE OR REPLACE TABLE c9_vendor_lines AS
        SELECT vendor_id, min(first_date) AS first_date, max(last_date) AS last_date, sum(bars) AS bars,
               arg_max(symbol, last_date) AS last_symbol, count(*) AS n_symbols,
               max(last_date) < CAST(? AS DATE) - CAST(? AS INTEGER) AS ceased
        FROM c9_vendor_symbols GROUP BY vendor_id
        """,
        [file_last, ceased_gap_days],
    )
    lines, ceased = con.execute("SELECT count(*), count(*) FILTER (WHERE ceased) FROM c9_vendor_lines").fetchone()
    return {"file_last_date": file_last, "lines": lines, "ceased_lines": ceased}


# ---------------------------------------------------------------------------------------------
# Stratified fetch plan (RX5 pilot)

PRICE_WINDOW_START = dt.date(2012, 3, 26)  # first TickerHistory3 date


def build_fetch_plan(
    con: duckdb.DuckDBPyConnection,
    *,
    issuers: int = 150,
    era_documents: int = 60,
    seed: float = 0.42,
    window_start: dt.date = PRICE_WINDOW_START,
) -> dict[str, int]:
    """``c9_fetch_plan``: documents to fetch, in priority order.

    *Issuer stratum* -- a random sample of ``issuers`` delisted-issuer CIKs
    (a Form 25/25-NSE filed in the price window and no current SEC ticker),
    stratified by the year of their last removal notice. Per issuer: the
    removal notices of the final 90 days (at most 3, newest first) and the
    latest 8-A12B/8-A12G before the last notice (at most 2). This is the
    coherent sample the line-coverage and precision measurements use.

    *Era stratum* -- ``era_documents`` further 8-A/25 documents spread over
    1996-2011 (older cover formats, lines already trading in 2012).
    Paper filings (no electronic document) are excluded and counted.
    """
    con.execute(f"SELECT setseed({seed})")
    con.execute(
        """
        CREATE OR REPLACE TEMP TABLE c9_delisted_issuers AS
        WITH removals AS (
            SELECT f.cik, max(f.filing_date) AS last_removal, count(*) AS removal_notices
            FROM c9_sec_filings f
            WHERE f.form IN ('25', '25-NSE') AND f.filing_date >= ?
              AND NOT (f.form = '25-NSE' AND substr(f.accession, 1, 10) = f.cik)  -- exchange's filer copy
            GROUP BY f.cik
        )
        SELECT r.*, e.name, year(r.last_removal) AS removal_year
        FROM removals r JOIN c9_sec_entities e USING (cik)
        WHERE e.tickers_json = '[]'
        """,
        [window_start],
    )
    per_year = max(issuers // 15, 1)
    con.execute(
        """
        CREATE OR REPLACE TEMP TABLE c9_sample_issuers AS
        SELECT * FROM (
            SELECT *, row_number() OVER (PARTITION BY removal_year ORDER BY random()) AS draw
            FROM c9_delisted_issuers
        ) WHERE draw <= ?
        """,
        [per_year],
    )
    con.execute(
        """
        CREATE OR REPLACE TABLE c9_fetch_plan AS
        WITH removal_docs AS (
            SELECT f.*, 'issuer' AS stratum, 1 AS kind_rank,
                   row_number() OVER (PARTITION BY f.cik ORDER BY f.filing_date DESC, f.accession DESC) AS pick
            FROM c9_sec_filings f JOIN c9_sample_issuers s USING (cik)
            WHERE f.form IN ('25', '25-NSE') AND f.filing_date BETWEEN s.last_removal - 90 AND s.last_removal
              AND f.primary_document IS NOT NULL AND f.primary_document NOT LIKE '%.paper'
        ), listing_docs AS (
            SELECT f.*, 'issuer' AS stratum, 2 AS kind_rank,
                   row_number() OVER (PARTITION BY f.cik ORDER BY f.filing_date DESC, f.accession DESC) AS pick
            FROM c9_sec_filings f JOIN c9_sample_issuers s USING (cik)
            WHERE f.form IN ('8-A12B', '8-A12G') AND f.filing_date <= s.last_removal
              AND f.primary_document IS NOT NULL AND f.primary_document NOT LIKE '%.paper'
        ), era_docs AS (
            SELECT f.*, 'era' AS stratum, 3 AS kind_rank,
                   row_number() OVER (PARTITION BY f.form, year(f.filing_date) ORDER BY random()) AS pick
            FROM c9_sec_filings f
            WHERE f.form IN ('8-A12B', '8-A12G', '25', '25-NSE') AND f.filing_date BETWEEN DATE '1996-01-01' AND ?
              AND f.primary_document IS NOT NULL AND f.primary_document NOT LIKE '%.paper'
              AND f.cik NOT IN (SELECT cik FROM c9_sample_issuers)
              AND NOT (f.form = '25-NSE' AND substr(f.accession, 1, 10) = f.cik)
        ), picked AS (
            SELECT * FROM removal_docs WHERE pick <= 3
            UNION ALL SELECT * FROM listing_docs WHERE pick <= 2
            UNION ALL SELECT * FROM (
                SELECT * FROM era_docs WHERE pick <= 1 ORDER BY random() LIMIT ?
            )
        )
        SELECT row_number() OVER (ORDER BY kind_rank = 3, cik, kind_rank, filing_date DESC, accession) AS priority,
               stratum, cik, accession, form, filing_date, acceptance_raw, primary_document, file_number, size
        FROM picked
        """,
        [window_start - dt.timedelta(days=1), era_documents],
    )
    counts = dict(
        con.execute("SELECT stratum || ':' || form, count(*) FROM c9_fetch_plan GROUP BY 1 ORDER BY 1").fetchall()
    )
    counts["sample_issuers"] = con.execute("SELECT count(*) FROM c9_sample_issuers").fetchone()[0]
    counts["delisted_issuers"] = con.execute("SELECT count(*) FROM c9_delisted_issuers").fetchone()[0]
    return counts


def fetch_planned_documents(
    con: duckdb.DuckDBPyConnection,
    fetcher: SecDocumentFetcher,
    *,
    max_consecutive_transient: int = 6,
    transient_pause_s: float = 15.0,
    max_attempts_per_url: int = 2,
    progress: Callable[[int, int, CachedResponse], None] | None = None,
) -> dict[str, object]:
    """Fetch ``c9_fetch_plan`` in priority order; stop at the budget or a sustained SEC outage.

    Results (cache hits included) go to ``c9_fetch_results``. After a transient
    outcome the loop pauses ``transient_pause_s x consecutive failures`` (capped
    at 2 minutes); ``max_consecutive_transient`` in a row stops the stage. A
    URL the ledger already shows ``max_attempts_per_url`` transient failures
    for is skipped (reported unfetched), so a persistently unavailable document
    cannot drain the budget.
    """
    attempts: dict[str, int] = {}
    for entry in fetcher.ledger():
        if int(entry["status"]) in RETRYABLE_STATUSES:
            attempts[entry["url"]] = attempts.get(entry["url"], 0) + 1
    con.execute(
        """CREATE TABLE IF NOT EXISTS c9_fetch_results (
            priority BIGINT, url VARCHAR, status INTEGER, sha256 VARCHAR, bytes BIGINT,
            fetched_at TIMESTAMP, from_cache BOOLEAN)"""
    )
    plan = con.execute(
        "SELECT priority, cik, accession, primary_document FROM c9_fetch_plan ORDER BY priority"
    ).fetchall()
    done = {row[0] for row in con.execute("SELECT priority FROM c9_fetch_results WHERE status = 200 OR status BETWEEN 400 AND 499").fetchall()}
    consecutive = 0
    stop_reason = "plan_complete"
    fetched = 0
    for priority, cik, accession, document in plan:
        if priority in done:
            continue
        url = archive_document_url(cik, accession, raw_document_name(document))
        if attempts.get(url, 0) >= max_attempts_per_url and fetcher.cached(url) is None:
            continue
        try:
            response = fetcher.fetch(url, retries=0)
        except FetchBudgetExhausted:
            stop_reason = "budget_exhausted"
            break
        con.execute("DELETE FROM c9_fetch_results WHERE priority = ?", [priority])
        con.execute(
            "INSERT INTO c9_fetch_results VALUES (?, ?, ?, ?, ?, ?, ?)",
            [priority, url, response.status, response.sha256, response.size, response.fetched_at, response.from_cache],
        )
        fetched += 0 if response.from_cache else 1
        if progress is not None:
            progress(priority, len(plan), response)
        if response.status in RETRYABLE_STATUSES:
            attempts[url] = attempts.get(url, 0) + 1
            consecutive += 1
            if consecutive >= max_consecutive_transient:
                stop_reason = "sec_unavailable"
                break
            fetcher._sleep(min(transient_pause_s * consecutive, 120.0))
        else:
            consecutive = 0
    summary = dict(
        con.execute(
            "SELECT CAST(status AS VARCHAR), count(*) FROM c9_fetch_results GROUP BY status ORDER BY status"
        ).fetchall()
    )
    return {
        "stop_reason": stop_reason,
        "network_requests_this_run": fetched,
        "requests_used_total": fetcher.requests_used,
        "results_by_status": summary,
    }


# ---------------------------------------------------------------------------------------------
# Evidence rows (0327 shape)


def _evidence_id(*parts: object) -> str:
    return "C9-" + hashlib.sha256("|".join(str(part) for part in parts).encode()).hexdigest()[:24]


def evidence_row(
    *,
    fact_kind: str,
    source: str,
    namespace: str,
    native_key: str,
    value: Mapping[str, object],
    valid_from: dt.date | None,
    valid_to: dt.date | None,
    locator: str,
    artifact_sha256: str | None,
    revision: str,
    clock: FilingClock | None,
    observed_at: dt.datetime,
    status: str,
    method: str,
    rejection: str | None = None,
    security_id: str | None = None,
    cik: str | None = None,
    symbol: str | None = None,
    run_id: str | None = None,
    loaded: dt.datetime | None = None,
    availability_status: str | None = None,
    available_at: dt.datetime | None = None,
) -> dict[str, object]:
    """One ``security_identity_evidence`` row in :data:`EVIDENCE_COLUMNS` order.

    Rejected rows (``unknown``/``conflicting``) carry no ``available_at`` and
    ``availability_status='unknown'``: they are kept, never linkable.
    """
    rejected = status in ("unknown", "conflicting")
    if rejected:
        availability, available = "unknown", None
    else:
        availability = availability_status or (clock.availability_status if clock else "unknown")
        available = available_at if available_at is not None else (clock.available_at if clock else None)
    row: dict[str, object] = {
        "evidence_id": _evidence_id(method, fact_kind, namespace, native_key, locator, valid_from, status, cik, symbol),
        "fact_kind": fact_kind,
        "source": source,
        "native_key_namespace": namespace,
        "native_key": native_key,
        "security_id": security_id,
        "cik": cik,
        "symbol": symbol,
        "value_json": json.dumps(dict(value), sort_keys=True, default=str),
        "valid_from": valid_from,
        "valid_to": valid_to,
        "source_locator": locator,
        "artifact_sha256": artifact_sha256,
        "source_revision_id": revision,
        "source_time_text": clock.raw if clock else None,
        "source_published_at": clock.published_at if clock else None,
        "observed_at": observed_at,
        "available_at": available,
        "evidence_status": status,
        "availability_status": availability,
        "method": method,
        "rejection_reason": rejection if rejected else None,
        "is_latest_revision": True,
        "run_id": run_id,
        "source_loaded_at": loaded or observed_at,
    }
    assert list(row) == [name for name, _ in EVIDENCE_COLUMNS]
    return row


_ONE_DAY = dt.timedelta(days=1)


@dataclass(frozen=True)
class FilingRef:
    """Submissions metadata of one fetched filing document."""

    cik: str
    accession: str
    form: str
    filing_date: dt.date
    acceptance_raw: str | None
    primary_document: str
    url: str
    sha256: str
    fetched_at: dt.datetime


def document_evidence(ref: FilingRef, parsed: ParsedDocument, *, run_id: str | None = None) -> list[dict[str, object]]:
    """Evidence rows for one parsed filing document (see module docstring for labels)."""
    clock = filing_clock(ref.acceptance_raw, ref.filing_date)
    base_form = ref.form.split("/")[0]
    removal = base_form in ("25", "25-NSE")
    common: dict[str, Any] = {
        "source": SOURCE_DOCUMENTS,
        "artifact_sha256": ref.sha256,
        "revision": ref.accession,
        "clock": clock,
        "observed_at": ref.fetched_at,
        "method": METHOD_DOCUMENT,
        "cik": ref.cik,
        "run_id": run_id,
    }
    header = {
        "form": ref.form,
        "accession": ref.accession,
        "filing_date": ref.filing_date,
        "issuer_name": parsed.issuer_name,
        "availability_policy": AVAILABILITY_POLICY,
        "acceptance_stamp_status": clock.stamp_status,
    }
    if parsed.status != "parsed":
        return [
            evidence_row(
                fact_kind="delisting_effective" if removal else "security_type",
                namespace=NS_CLASS,
                native_key=f"{ref.cik}|unparsed:{ref.accession}",
                value={**header, "parse_status": parsed.status, "notes": parsed.notes},
                valid_from=None,
                valid_to=None,
                locator=ref.url,
                status="unknown",
                rejection=f"document_{parsed.status}",
                **common,
            )
        ]
    status = "verified_dated"
    rejection = None
    subject = ref.cik
    if parsed.issuer_cik is not None and parsed.issuer_cik != ref.cik:
        if ref.accession.replace("-", "")[:10] == ref.cik:
            # An exchange's own submissions list the notices it filed: the subject is the issuer the
            # document names, not the listing CIK (a filer copy, not a conflict).
            subject = parsed.issuer_cik
            header["listed_under_filer_cik"] = ref.cik
            common["cik"] = subject
        else:
            status, rejection = "conflicting", f"issuer_cik_mismatch:{parsed.issuer_cik}"
    rows: list[dict[str, object]] = []
    single_class = len(parsed.classes) == 1
    seen: set[tuple[str, str | None]] = set()
    for item in parsed.classes:
        kind, share_class = classify_class_title(item.title)
        key = f"{subject}|{class_key(item.title)}"
        if (key, item.mic) in seen:
            continue  # the same class listed twice in one document is one fact
        seen.add((key, item.mic))
        value = {
            **header,
            "class_title": item.title,
            "security_type": kind,
            "share_class": share_class,
            "classification_basis": "c9_class_title_v1",
        }
        point_end = ref.filing_date + _ONE_DAY
        # A class the title classifier cannot type yields no accepted fact of any kind.
        class_status, class_rejection = status, rejection
        if kind == "unknown" and status == "verified_dated":
            class_status, class_rejection = "unknown", "class_title_unclassified"
        rows.append(
            evidence_row(
                fact_kind="security_type",
                namespace=NS_CLASS,
                native_key=key,
                value=value,
                valid_from=ref.filing_date,
                valid_to=point_end if removal else None,
                locator=f"{ref.url}#class",
                status=class_status,
                rejection=class_rejection,
                **common,
            )
        )
        if item.mic:
            rows.append(
                evidence_row(
                    fact_kind="primary_listing",
                    namespace=NS_CLASS,
                    native_key=key,
                    value={
                        **value,
                        "primary_mic": item.mic,
                        "exchange_name": item.exchange_text,
                        "listing_basis": (
                            "removal_notice_names_listing"
                            if removal
                            else "section_12b_registration"
                            if base_form == "8-A12B"
                            else "section_12g_registration_names_market"
                        ),
                    },
                    valid_from=ref.filing_date,
                    valid_to=point_end if removal else None,
                    locator=f"{ref.url}#exchange",
                    status=class_status,
                    rejection=class_rejection,
                    **common,
                )
            )
        if removal:
            stated = parsed.stated_effective_date
            effective = stated or ref.filing_date + dt.timedelta(days=FILING_DELAY_EFFECTIVE_DAYS)
            rows.append(
                evidence_row(
                    fact_kind="delisting_effective",
                    namespace=NS_CLASS,
                    native_key=key,
                    value={
                        **value,
                        "exchange_mic": item.mic,
                        "exchange_name": item.exchange_text,
                        "exchange_cik": parsed.exchange_cik,
                        "rule_provision": parsed.rule_provision,
                        "notice_filed": ref.filing_date,
                        "signature_date": parsed.signature_date,
                        "effective_basis": "stated" if stated else "rule_12d2-2(d)(1)_filed_plus_10_days",
                        "trading_cessation": "not_stated",
                    },
                    valid_from=effective,
                    valid_to=None,
                    locator=f"{ref.url}#removal",
                    status=class_status if (stated or class_status != "verified_dated") else "inferred",
                    rejection=class_rejection,
                    **common,
                )
            )
        # A text-stated symbol maps to the class only when both are unique in the document; several
        # stated symbols (e.g. the common's and the warrant's) stay class-ambiguous below.
        symbols = [item.symbol] if item.symbol else (parsed.symbols if single_class and len(parsed.symbols) == 1 else [])
        for symbol in symbols:
            rows.append(
                evidence_row(
                    fact_kind="symbol_mapping",
                    namespace=NS_EXCHANGE_SYMBOL,
                    native_key=f"{item.mic or 'US'}:{symbol}",
                    value={**value, "class_key": key, "primary_mic": item.mic,
                           "basis": "cover_table" if item.symbol else "document_text"},
                    valid_from=ref.filing_date,
                    valid_to=point_end,
                    locator=f"{ref.url}#symbol",
                    # The issuer's symbol statement stands even when its class title is unclassified,
                    # but then only at issuer grain: inferred, never verified.
                    status="inferred" if class_rejection == "class_title_unclassified" else class_status,
                    rejection=None if class_rejection == "class_title_unclassified" else class_rejection,
                    symbol=symbol,
                    **common,
                )
            )
    if not single_class or len(parsed.symbols) > 1:
        for symbol in parsed.symbols:
            if any(item.symbol == symbol for item in parsed.classes):
                continue
            rows.append(
                evidence_row(
                    fact_kind="symbol_mapping",
                    namespace=NS_EXCHANGE_SYMBOL,
                    native_key=f"US:{symbol}",
                    value={**header, "basis": "document_text_class_ambiguous",
                           "classes": [item.title for item in parsed.classes]},
                    valid_from=ref.filing_date,
                    valid_to=ref.filing_date + _ONE_DAY,
                    locator=f"{ref.url}#symbol",
                    status="inferred" if status == "verified_dated" else status,
                    rejection=rejection,
                    symbol=symbol,
                    **common,
                )
            )
    return rows


def insert_evidence(con: duckdb.DuckDBPyConnection, rows: Iterable[Mapping[str, object]], *, table: str = EVIDENCE_TABLE) -> int:
    """Insert rows into the (scratch) 0327 evidence table; the DDL CHECKs apply."""
    names = [name for name, _ in EVIDENCE_COLUMNS]
    records = [[row[name] for name in names] for row in rows]
    if records:
        placeholders = ", ".join("?" for _ in names)
        con.executemany(f"INSERT INTO {table} ({', '.join(names)}) VALUES ({placeholders})", records)
    return len(records)


def bulk_cert_symbol_evidence(
    con: duckdb.DuckDBPyConnection, *, artifact_sha256: str, observed_at: dt.datetime, run_id: str | None = None
) -> list[dict[str, object]]:
    """``symbol_mapping`` rows from exchange-certification document names (``inferred``).

    The exchange authors the certification and names the document after the
    certified symbol; the name is metadata, not a statement inside the
    document, so the row is ``inferred`` (point interval at the filing date).
    """
    rows = con.execute(
        """
        SELECT f.cik, f.accession, f.form, f.filing_date, f.acceptance_raw, f.primary_document, f.file_number,
               f.member, f.row_index, substr(f.accession, 1, 10) AS filer_cik, e.name AS filer_name
        FROM c9_sec_filings f LEFT JOIN c9_sec_entities e ON e.cik = substr(f.accession, 1, 10)
        WHERE f.form LIKE 'CERT%' AND f.primary_document ILIKE '%.pdf'
        """
    ).fetchall()
    out: list[dict[str, object]] = []
    for cik, accession, form, filed, raw, document, file_number, member, index, filer_cik, filer_name in rows:
        symbol = cert_document_symbol(document, filed)
        if symbol is None:
            continue
        mic = CERT_FORM_MIC.get(form) or exchange_mic(filer_name)
        out.append(
            evidence_row(
                fact_kind="symbol_mapping",
                source=SOURCE_FILINGS,
                namespace=NS_EXCHANGE_SYMBOL,
                native_key=f"{mic or 'US'}:{symbol}",
                value={
                    "basis": "exchange_certification_document_name",
                    "form": form,
                    "accession": accession,
                    "primary_document": document,
                    "file_number": file_number,
                    "filer_cik": filer_cik,
                    "filer_name": filer_name,
                    "primary_mic": mic,
                    "availability_policy": AVAILABILITY_POLICY,
                },
                valid_from=filed,
                valid_to=filed + _ONE_DAY if filed else None,
                locator=f"submissions.zip:{member}#filings[{index}].primaryDocument",
                artifact_sha256=artifact_sha256,
                revision=accession,
                clock=filing_clock(raw, filed),
                observed_at=observed_at,
                status="inferred",
                method="c9_cert_document_name_v1",
                cik=cik,
                symbol=symbol,
                run_id=run_id,
            )
        )
    return out


def _name_key(name: str) -> str:
    return re.sub(r"[^A-Z0-9]+", " ", name.upper()).strip()


def former_name_evidence(
    con: duckdb.DuckDBPyConnection,
    ciks: Sequence[str],
    *,
    artifact_sha256: str,
    observed_at: dt.datetime,
    run_id: str | None = None,
) -> list[dict[str, object]]:
    """Issuer-name windows (``issuer_link`` on the issuer-name namespace), ``reconstructed``/``modeled``.

    ``formerNames[].from/to`` are dates the 2026 archive states about the past
    (no historical observation clock): availability is modeled as the window
    start plus the FC1 46h floor. The current name opens at the day after the
    latest former-name end.
    """
    if not ciks:
        return []
    con.execute("CREATE OR REPLACE TEMP TABLE c9_name_scope AS SELECT unnest(?::VARCHAR[]) AS cik", [list(ciks)])
    former = con.execute(
        """
        SELECT n.cik, n.name, n.from_raw, n.to_raw, n.ordinal, n.member
        FROM c9_sec_former_names n JOIN c9_name_scope USING (cik) ORDER BY n.cik, n.ordinal
        """
    ).fetchall()
    current = dict(
        con.execute(
            "SELECT e.cik, e.name FROM c9_sec_entities e JOIN c9_name_scope USING (cik)"
        ).fetchall()
    )
    out: list[dict[str, object]] = []
    last_end: dict[str, dt.date] = {}
    for cik, name, from_raw, to_raw, ordinal, member in former:
        start, end = _date(from_raw), _date(to_raw)
        if not name or start is None:
            continue
        stop = end + _ONE_DAY if end is not None and end >= start else None
        if stop is not None:
            last_end[cik] = max(last_end.get(cik, stop), stop)
        out.append(_name_row(cik, name, start, stop, f"submissions.zip:{member}#formerNames[{ordinal}]",
                             from_raw, artifact_sha256, observed_at, run_id))
    for cik, name in current.items():
        if not name:
            continue
        start = last_end.get(cik)
        if start is None:
            continue  # no dated start for the current name: nothing to assert
        out.append(_name_row(cik, name, start, None, f"submissions.zip:CIK{cik}.json#name",
                             None, artifact_sha256, observed_at, run_id))
    return out


def _name_row(
    cik: str,
    name: str,
    start: dt.date,
    stop: dt.date | None,
    locator: str,
    raw: str | None,
    sha: str,
    observed_at: dt.datetime,
    run_id: str | None,
) -> dict[str, object]:
    modeled = dt.datetime.combine(start, dt.time()) + dt.timedelta(hours=46)
    return evidence_row(
        fact_kind="issuer_link",
        source=SOURCE_FILINGS,
        namespace=NS_ISSUER_NAME,
        native_key=_name_key(name),
        value={"issuer_name": name, "window_source": "formerNames" if raw else "current_name_after_last_former",
               "availability_policy": f"window_start_plus_46h_modeled ({FUNDAMENTAL_CLOCK_POLICY})"},
        valid_from=start,
        valid_to=stop,
        locator=locator,
        artifact_sha256=sha,
        revision=f"submissions.zip@{sha[:12]}",
        clock=FilingClock(raw, None, modeled, "modeled", "date_only"),
        observed_at=observed_at,
        status="reconstructed",
        method=METHOD_FORMER_NAMES,
        cik=cik,
        symbol=None,
        run_id=run_id,
    )


# ---------------------------------------------------------------------------------------------
# Line join: a vendor line trading a filing-anchored symbol around the anchor date

LINE_ANCHOR_LEAD_DAYS = 60  # anchor may precede the symbol span (registration before first trade)
LINE_ANCHOR_LAG_DAYS = 14  # or follow its end slightly (notice after the last trade)
TERMINAL_WINDOW = (-7, 30)  # removal notice filed within [last trade - 7, last trade + 30]
LISTING_WINDOW = (-45, 5)  # registration/certification within [first trade - 45, first trade + 5]

LINE_CANDIDATES_SQL = f"""
WITH anchors AS (
    SELECT evidence_id, cik, symbol, valid_from AS anchor_date, available_at, evidence_status, method
    FROM {EVIDENCE_TABLE}
    WHERE fact_kind = 'symbol_mapping' AND evidence_status IN ('verified_dated', 'inferred')
      AND symbol IS NOT NULL AND cik IS NOT NULL AND valid_from IS NOT NULL AND is_latest_revision
), file_start AS (
    SELECT min(first_date) AS day FROM c9_vendor_symbols
), in_window AS (
    SELECT s.vendor_id, s.symbol, s.first_date AS span_first, s.last_date AS span_last, a.cik,
           a.evidence_id, a.anchor_date, a.available_at, a.evidence_status, a.method, false AS left_censored
    FROM anchors a JOIN c9_vendor_symbols s ON s.symbol = a.symbol
     AND a.anchor_date BETWEEN s.first_date - {LINE_ANCHOR_LEAD_DAYS} AND s.last_date + {LINE_ANCHOR_LAG_DAYS}
), censored AS (
    -- A span already trading on the file's first day started earlier: an older anchor of the
    -- same symbol may be its listing. Only the latest such anchor is kept (a symbol reused
    -- before the file start belongs to the most recent registrant).
    SELECT s.vendor_id, s.symbol, s.first_date AS span_first, s.last_date AS span_last, a.cik,
           a.evidence_id, a.anchor_date, a.available_at, a.evidence_status, a.method, true AS left_censored
    FROM anchors a JOIN c9_vendor_symbols s ON s.symbol = a.symbol
    CROSS JOIN file_start
    WHERE s.first_date <= file_start.day + 5 AND a.anchor_date < s.first_date - {LINE_ANCHOR_LEAD_DAYS}
    QUALIFY a.anchor_date = max(a.anchor_date) OVER (PARTITION BY s.vendor_id, s.symbol, s.first_date)
), matched AS (
    SELECT * FROM in_window UNION ALL SELECT * FROM censored
), pairs AS (
    SELECT vendor_id, symbol, span_first, span_last, cik,
           bool_or(evidence_status = 'verified_dated') AS verified_anchor,
           bool_and(left_censored) AS left_censored_only,
           min(available_at) AS first_available_at,
           list({{'evidence_id': evidence_id, 'status': evidence_status, 'method': method,
                  'date': anchor_date}} ORDER BY anchor_date) AS anchors
    FROM matched GROUP BY ALL
)
SELECT p.*, l.first_date AS line_first, l.last_date AS line_last, l.ceased,
       count(*) OVER (PARTITION BY p.vendor_id, p.symbol, p.span_first) AS issuers_for_span,
       (SELECT min(f.filing_date) FROM c9_sec_filings f
         WHERE f.cik = p.cik AND f.form IN ('25', '25-NSE')
           AND f.filing_date BETWEEN l.last_date + ({TERMINAL_WINDOW[0]}) AND l.last_date + {TERMINAL_WINDOW[1]}
       ) AS terminal_notice_date,
       (SELECT min(f.filing_date) FROM c9_sec_filings f
         WHERE f.cik = p.cik AND (f.form IN ('8-A12B', '8-A12G') OR f.form LIKE 'CERT%')
           AND f.filing_date BETWEEN l.first_date + ({LISTING_WINDOW[0]}) AND l.first_date + {LISTING_WINDOW[1]}
       ) AS listing_filing_date
FROM pairs p JOIN c9_vendor_lines l USING (vendor_id)
"""


def line_link_evidence(
    con: duckdb.DuckDBPyConnection,
    *,
    observed_at: dt.datetime,
    artifact_sha256: str | None = None,
    run_id: str | None = None,
) -> list[dict[str, object]]:
    """Vendor-line ``issuer_link`` rows supported by filing-anchored symbols (``reconstructed``/``modeled``).

    A (line, CIK) pair needs the line to trade the anchored symbol within
    ``[anchor - 14d, anchor + 60d]`` of its span (or, for a span already
    trading on the file's first day, the latest earlier anchor).
    ``available_at`` is the earliest anchor's clock (the link is unknowable
    before its filing); terminal/listing filing alignments are recorded as
    corroboration only. Conflicts are resolved by documented rules only:

    * *succession* -- several issuers anchor the same line symbol span on
      different dates (a holding-company reorganization re-registers the
      symbol): each issuer holds the span from its first anchor date (the
      first issuer from the span start) until the next issuer's anchor date;
    * issuers anchored on the *same* date, or different issuers whose links
      overlap on one line through different symbols, are all kept as
      ``conflicting`` (never linkable);
    * overlapping links of the same issuer (two symbols of one line) merge.
    """
    con.execute(f"CREATE OR REPLACE TABLE c9_line_candidates AS {LINE_CANDIDATES_SQL}")
    rows = con.execute(
        """SELECT vendor_id, symbol, span_first, span_last, cik, verified_anchor, first_available_at, anchors,
                  line_first, line_last, ceased, terminal_notice_date, listing_filing_date, left_censored_only
           FROM c9_line_candidates ORDER BY vendor_id, span_first, symbol, cik"""
    ).fetchall()
    by_vendor: dict[int, list[dict[str, Any]]] = {}
    for (vendor, symbol, first, last, cik, verified, available, anchors, line_first, line_last, ceased,
         terminal, listing, left_censored) in rows:
        first_anchor = min(anchor["date"] for anchor in anchors)
        rules: list[str] = []
        start = first
        if not left_censored and first_anchor > first + dt.timedelta(days=LINE_ANCHOR_LEAD_DAYS):
            # A registration/certification well inside a symbol span is not that span's listing: it may
            # be a successor re-registering the symbol (holding-company reorganization, split-off) or a
            # document named after another issuer's symbol. It supports the link only from its own date.
            start = first_anchor
            rules.append("mid_span_anchor_valid_from_anchor")
        if start > last:
            continue  # anchor after the span ended: no validity to assert
        by_vendor.setdefault(vendor, []).append(
            {
                "vendor": vendor, "symbols": [symbol], "span_first": first, "first": start, "last": last,
                "cik": cik, "verified": verified, "available": available, "anchors": list(anchors),
                "first_anchor": first_anchor, "line_first": line_first,
                "line_last": line_last, "ceased": ceased, "terminal": terminal, "listing": listing,
                "left_censored": left_censored, "status": LINE_LINK_STATUS, "rejection": None, "rules": rules,
            }
        )
    out: list[dict[str, object]] = []
    for vendor, candidates in by_vendor.items():
        for link in _resolve_line_links(candidates):
            value = {
                "cik": link["cik"],
                "share_class_symbol": link["symbols"][0],
                "symbols": link["symbols"],
                "basis": "filing_anchored_symbol_x_vendor_symbol_span",
                "anchor_best_status": "verified_dated" if link["verified"] else "inferred",
                "left_censored_anchor_only": link["left_censored"],
                "resolution_rules": link["rules"],
                "anchors": [{**anchor, "date": str(anchor["date"])} for anchor in link["anchors"]][:8],
                "line_first": link["line_first"],
                "line_last": link["line_last"],
                "line_ceased": link["ceased"],
                "terminal_notice_date": link["terminal"],
                "listing_filing_date": link["listing"],
                "vendor_symbol_is_reconstructed": True,
            }
            out.append(
                evidence_row(
                    fact_kind="issuer_link",
                    source=SOURCE_FILINGS,
                    namespace=NS_VENDOR_LINE,
                    native_key=str(vendor),
                    value=value,
                    valid_from=link["first"],
                    valid_to=link["last"] + _ONE_DAY,
                    locator=(
                        f"TickerHistory3.parquet:securityID={vendor}:ticker_tk={'|'.join(link['symbols'])};"
                        f"anchors={len(link['anchors'])}"
                    ),
                    artifact_sha256=artifact_sha256,
                    revision=METHOD_LINE_SYMBOL,
                    clock=None,
                    observed_at=observed_at,
                    status=link["status"],
                    method=METHOD_LINE_SYMBOL,
                    rejection=link["rejection"],
                    security_id=f"{VENDOR_LINE_PREFIX}{vendor}",
                    cik=link["cik"],
                    symbol=link["symbols"][0],
                    run_id=run_id,
                    availability_status="modeled",
                    available_at=link["available"],
                )
            )
    return out


def _resolve_line_links(candidates: list[dict[str, Any]]) -> list[dict[str, Any]]:
    """Apply the succession / same-date conflict / overlap rules of :func:`line_link_evidence` to one line."""
    spans: dict[tuple[str, dt.date], list[dict[str, Any]]] = {}
    for link in candidates:
        spans.setdefault((link["symbols"][0], link["span_first"]), []).append(link)
    resolved: list[dict[str, Any]] = []
    for group in spans.values():
        if len(group) == 1:
            resolved.extend(group)
            continue
        group.sort(key=lambda link: (link["first_anchor"], link["cik"]))
        dates = [link["first_anchor"] for link in group]
        if len(set(dates)) < len(dates):
            for link in group:
                link["status"], link["rejection"] = "conflicting", "multiple_issuers_same_anchor_date"
            resolved.extend(group)
            continue
        span_first, span_last = group[0]["span_first"], group[0]["last"]
        for index, link in enumerate(group):
            # The first issuer keeps its own start (the span start, or its anchor when mid-span).
            start = link["first"] if index == 0 else max(span_first, link["first_anchor"])
            stop = span_last if index == len(group) - 1 else group[index + 1]["first_anchor"] - _ONE_DAY
            if start > stop:
                continue  # the successor's anchor fell in the lag window after the span: no validity
            link["first"], link["last"] = start, stop
            link["rules"].append("succession_split_at_successor_anchor")
            resolved.append(link)
    rejected = [link for link in resolved if link["status"] != LINE_LINK_STATUS]
    accepted = sorted((link for link in resolved if link["status"] == LINE_LINK_STATUS), key=lambda link: link["first"])
    merged: list[dict[str, Any]] = []
    for link in accepted:
        overlap = [other for other in merged if other["status"] == LINE_LINK_STATUS
                   and link["first"] <= other["last"] and other["first"] <= link["last"]]
        same = [other for other in overlap if other["cik"] == link["cik"]]
        different = [other for other in overlap if other["cik"] != link["cik"]]
        if different:
            for other in [*different, link]:
                other["status"], other["rejection"] = "conflicting", "overlapping_issuer_links_on_line"
            merged.append(link)
            continue
        if same:
            target = same[0]
            target["first"], target["last"] = min(target["first"], link["first"]), max(target["last"], link["last"])
            target["symbols"] += [symbol for symbol in link["symbols"] if symbol not in target["symbols"]]
            target["anchors"] += link["anchors"]
            target["verified"] = target["verified"] or link["verified"]
            target["available"] = min(target["available"], link["available"])
            target["terminal"] = target["terminal"] or link["terminal"]
            target["listing"] = target["listing"] or link["listing"]
            target["left_censored"] = target["left_censored"] and link["left_censored"]
            target["rules"].append("merged_same_issuer_symbols")
            continue
        merged.append(link)
    return merged + rejected
