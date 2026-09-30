"""SEC sources for the ``events`` stages (lane EVT): EDGAR full-text search harvests, document fetches, text.

Raw landing ``data/raw/sec_events/`` is one :class:`atx_db.sec_http.FetchLedgerStore`: every request (an EDGAR
full-text-search page or a filing document) is one ``fetch-ledger.jsonl`` line (url, bytes, sha256, http status,
fetched_at, user agent) and the body is kept as served (gzip at rest, sha256 over the raw bytes). That ledger is
the receipt file of this source. Requests go through ``sec_http`` (approved user agent, host-wide 5 req/s bucket,
shared 403/429 pause); the lane cap :data:`LANE_REQUEST_CAP` counts ledger lines of this store.

* **Full-text search** (``efts.sec.gov/LATEST/search-index``): one query per (``qid``, form set) over
  :data:`WINDOW_START`..:data:`WINDOW_END` by month; a window whose hit total reaches the 10,000-hit ceiling is
  halved until it fits, then paged 100 hits at a time. A hit is one document of a filing: accession (``adsh``),
  file name, file type (``EX-99.1``...), form, 8-K items, CIKs, file date. Hits are the locator of the exhibit
  file names (no index-page request) and the evidence that a phrase occurs in the document.
  ``_tmp/events/fts_<qid>.parquet`` holds the parsed hits (rebuildable from the raw pages).
* **Documents**: ``https://www.sec.gov/Archives/edgar/data/<cik>/<acc>/<file>``, fetched by up to
  :data:`FETCH_WORKERS` threads sharing the limiter, at most :data:`MAX_DOCUMENT_BYTES` each.
* **Landed earnings releases**: the EX-99 objects of ``data/raw/sec-earnings-release`` (v2 press-release fetch,
  ``units-done.jsonl`` maps accession -> document sha256) are read in place and never refetched.

:func:`document_text` turns a document (HTML or text) into lines: block elements end a line, table cells are
separated by `` | ``.
"""

from __future__ import annotations

import argparse
import datetime as dt
import gzip
import json
import os
import re
import sys
import threading
from collections import Counter
from collections.abc import Callable, Iterable, Iterator
from concurrent.futures import FIRST_COMPLETED, Future, ThreadPoolExecutor, wait
from pathlib import Path
from typing import Any
from urllib.parse import urlencode

import pyarrow as pa
import pyarrow.parquet as pq

from .. import sec_http as SH
from . import common as C

RAW_DIR = Path(os.environ.get("ATX_EVT_RAW_DIR", str(C.PACKAGE_ROOT / "data" / "raw" / "sec_events")))
EARNINGS_RELEASE_DIR = C.PACKAGE_ROOT / "data" / "raw" / "sec-earnings-release"
FTS_ROOT = "https://efts.sec.gov/LATEST/search-index"
ARCHIVES = "https://www.sec.gov/Archives/edgar/data"
FTS_PAGE = 100
FTS_CEILING = 10_000
LANE_REQUEST_CAP = int(os.environ.get("ATX_EVT_SEC_CAP", "60000"))
FETCH_WORKERS = 3
MAX_DOCUMENT_BYTES = 8_000_000
MAX_FTS_BYTES = 4_000_000
WINDOW_START = dt.date(2019, 1, 1)
WINDOW_END = dt.date(2026, 9, 19)  # last filing date of the published sec_filings stage

#: Departure constructs before a role title (FTS does not stem: every inflection is its own phrase).
DEPART_PHRASES = tuple(f"{v} as" for v in (
    "resignation", "resign", "resigned", "resigning", "retire", "retirement", "retired", "retiring", "step down",
    "stepped down", "steps down", "stepping down", "departure", "depart", "departed", "terminated", "termination",
    "transition", "separation", "removed", "removal")) + (
    "from his position as", "from her position as", "from his role as", "from her role as", "ceased to serve as",
    "cease to serve as", "no longer serve as")

#: Registered full-text queries: qid -> (comma-separated forms, query). FTS syntax: quoted phrases, implicit AND,
#: top-level OR (parentheses are not supported: they return no hits).
QUERIES: dict[str, tuple[str, str]] = {
    "guidance": ("8-K", "guidance OR outlook"),
    "buyback": ("8-K", '"repurchase program" OR "repurchase authorization" OR "buyback program" '
                       'OR "repurchase plan" OR "repurchase of up to"'),
    "merger": ("8-K", '"converted into the right to receive"'),
    "tender": ("SC TO-T,SC 14D9", '"per share" "net to the seller"'),
    "fpi_results": ("6-K", '"financial results" OR "results for the" OR "earnings release"'),
    # period-results wording of 6-K releases (narrower than fpi_results: the fetch plan prefers these)
    "fpi_period_results": ("6-K", " OR ".join(f'"{p}"' for p in (
        "financial results for the first quarter", "financial results for the second quarter",
        "financial results for the third quarter", "financial results for the fourth quarter",
        "results for the three months ended", "results for the six months ended", "results for the nine months ended",
        "results for the quarter ended", "results for the year ended", "results for the fiscal year ended",
        "results for the full year", "results for the half year", "unaudited financial results",
        "quarter financial results", "half year results", "half-year results", "first half results",
        "interim results for the", "quarter results", "annual results for"))),
    # governance: officer departures named by role (the phrase is the evidence), going-concern audit paragraphs
    **{f"ceo_depart_{i}": ("8-K", " OR ".join(f'"{p} {role}"' for p in DEPART_PHRASES for role in roles))
       for i, roles in enumerate((("chief executive officer",), ("president and chief executive officer",),
                                  ("ceo",)))},
    **{f"cfo_depart_{i}": ("8-K", " OR ".join(f'"{p} {role}"' for p in DEPART_PHRASES for role in roles))
       for i, roles in enumerate((("chief financial officer",), ("executive vice president and chief financial officer",
                                                                  "senior vice president and chief financial officer"),
                                  ("vice president and chief financial officer", "cfo")))},
    # going concern: the auditor's explanatory paragraph (AS 2415 / AS 3101) refers to management's plans "in regard
    # to these matters" described in a note; a risk factor saying doubt "could" arise does not use it.
    # (a "10-K,10-K/A" form list returns far fewer hits than "10-K" alone: the original 10-K is queried)
    "going_concern": ("10-K", '"in regard to these matters are also described" OR "regarding these matters are '
                              'also described" OR "in regard to these matters are described" OR "regarding these '
                              'matters are described"'),
    # capital-market events: convertible note pricing releases (two phrases ANDed; one query per note wording since
    # a top-level OR of AND groups is not reliable)
    **{f"convert_{i}": ("8-K", f'"the pricing of" "{notes}"')
       for i, notes in enumerate(("convertible senior notes due", "convertible notes due",
                                  "exchangeable senior notes due"))},
    # spin-offs: Form 10 registrations whose information statement describes a distribution by a parent
    "spinoff": ("10-12B", '"spin-off" OR "separation and distribution agreement" OR "pro rata distribution"'),
}

HIT_SCHEMA = pa.schema([
    ("qid", pa.string()), ("adsh", pa.string()), ("file", pa.string()), ("file_type", pa.string()),
    ("file_description", pa.string()), ("form", pa.string()), ("root_form", pa.string()),
    ("items", pa.list_(pa.string())), ("ciks", pa.list_(pa.int64())), ("file_date", pa.date32()),
    ("period_ending", pa.date32()), ("display_names", pa.list_(pa.string())), ("sics", pa.list_(pa.string())),
    ("window_start", pa.date32()), ("window_end", pa.date32()),
])


class BudgetExceeded(RuntimeError):
    """The lane's SEC request cap would be exceeded."""


# ---------------------------------------------------------------------------------------------------------
# Store and budget
# ---------------------------------------------------------------------------------------------------------


class Budget:
    """Ledger-line budget of this store (thread-safe). ``used`` = ledger lines at start + requests made here."""

    def __init__(self, store: SH.FetchLedgerStore, cap: int = LANE_REQUEST_CAP) -> None:
        store.load()
        self.start = store.ledger_lines
        self.used = self.start
        self.cap = cap
        self._lock = threading.Lock()

    def take(self) -> None:
        with self._lock:
            if self.used >= self.cap:
                raise BudgetExceeded(f"lane SEC request cap {self.cap} reached")
            self.used += 1

    def refund(self) -> None:
        with self._lock:
            self.used -= 1


def open_store(root: Path = RAW_DIR) -> SH.FetchLedgerStore:
    root.mkdir(parents=True, exist_ok=True)
    return SH.FetchLedgerStore(root).load()


def ensure(store: SH.FetchLedgerStore, url: str, budget: Budget, *, maximum: int,
           limiter: SH.SecRateLimiter | None = None) -> SH.FetchRecord:
    """The ledgered terminal record of ``url``, else one budgeted fetch."""
    rec = store.lookup(url)
    if rec is not None and store._object_present(rec):  # noqa: SLF001 - same check ensure() makes
        return rec
    budget.take()
    rec, fetched = store.ensure(url, limiter=limiter or SH.default_sec_limiter(), maximum=maximum)
    if not fetched:
        budget.refund()
    return rec


def read_record(store: SH.FetchLedgerStore, url: str) -> bytes | None:
    """Stored bytes of ``url`` (None when not fetched or not a 200)."""
    rec = store.lookup(url)
    if rec is None or not rec.ok:
        return None
    try:
        return store.read(rec)
    except (OSError, ValueError):
        return None


def document_url(cik: int, adsh: str, file: str) -> str:
    return f"{ARCHIVES}/{int(cik)}/{adsh.replace('-', '')}/{file}"


# ---------------------------------------------------------------------------------------------------------
# Full-text search
# ---------------------------------------------------------------------------------------------------------


def fts_url(q: str, forms: str, start: dt.date, end: dt.date, offset: int = 0) -> str:
    params = {"q": q, "forms": forms, "dateRange": "custom", "startdt": start.isoformat(), "enddt": end.isoformat()}
    if offset:
        params["from"] = str(offset)
    return f"{FTS_ROOT}?{urlencode(params)}"


def month_windows(start: dt.date, end: dt.date) -> list[tuple[dt.date, dt.date]]:
    out = []
    d = start
    while d <= end:
        nxt = dt.date(d.year + (d.month == 12), d.month % 12 + 1, 1)
        out.append((d, min(end, nxt - dt.timedelta(days=1))))
        d = nxt
    return out


def _date(v: Any) -> dt.date | None:
    try:
        return dt.date.fromisoformat(str(v)[:10]) if v else None
    except ValueError:
        return None


def parse_hits(payload: dict[str, Any], qid: str, window: tuple[dt.date, dt.date]) -> list[dict[str, Any]]:
    """Rows of :data:`HIT_SCHEMA` from one FTS response page."""
    rows = []
    for h in (payload.get("hits") or {}).get("hits") or []:
        src = h.get("_source") or {}
        hid = str(h.get("_id") or "")
        adsh, _, file = hid.partition(":")
        ciks = []
        for c in src.get("ciks") or []:
            try:
                ciks.append(int(c))
            except (TypeError, ValueError):
                continue
        roots = src.get("root_forms") or []
        rows.append({
            "qid": qid, "adsh": src.get("adsh") or adsh, "file": file, "file_type": src.get("file_type"),
            "file_description": src.get("file_description"), "form": src.get("form"),
            "root_form": roots[0] if roots else None, "items": [str(i) for i in src.get("items") or []],
            "ciks": ciks, "file_date": _date(src.get("file_date")), "period_ending": _date(src.get("period_ending")),
            "display_names": [str(x) for x in src.get("display_names") or []],
            "sics": [str(x) for x in src.get("sics") or []], "window_start": window[0], "window_end": window[1],
        })
    return rows


def _fts_page(store: SH.FetchLedgerStore, url: str, budget: Budget) -> dict[str, Any]:
    rec = ensure(store, url, budget, maximum=MAX_FTS_BYTES)
    if not rec.ok:
        raise RuntimeError(f"FTS page failed ({rec.status} {rec.error}): {url}")
    return json.loads(store.read(rec))


#: Queries with few hits are paged over yearly windows (a window over the 10,000-hit ceiling is halved anyway).
MONTHLY_QUERIES = frozenset({"guidance", "buyback", "merger", "tender", "fpi_results"})


def year_windows(start: dt.date, end: dt.date) -> list[tuple[dt.date, dt.date]]:
    return [(max(start, dt.date(y, 1, 1)), min(end, dt.date(y, 12, 31))) for y in range(start.year, end.year + 1)]


def harvest(qid: str, start: dt.date = WINDOW_START, end: dt.date = WINDOW_END,
            store: SH.FetchLedgerStore | None = None, budget: Budget | None = None) -> dict[str, Any]:
    """Page every hit of registered query ``qid`` into ``_tmp/events/fts_<qid>.parquet``; return window stats."""
    forms, q = QUERIES[qid]
    store = store or open_store()
    budget = budget or Budget(store)
    wins = month_windows(start, end) if qid in MONTHLY_QUERIES else year_windows(start, end)
    stack = list(reversed(wins))
    stats = {"windows": 0, "hits_total": 0, "hits_unique": 0, "short_windows": [], "requests_before": budget.used}
    dest = fts_hits_path(qid)
    dest.parent.mkdir(parents=True, exist_ok=True)
    tmp = dest.with_name(dest.name + ".partial")
    writer = pq.ParquetWriter(tmp, HIT_SCHEMA, compression="zstd")
    rows_out = 0
    try:
        while stack:
            s, e = stack.pop()
            first = _fts_page(store, fts_url(q, forms, s, e), budget)
            tot = (first.get("hits") or {}).get("total") or {}
            total, relation = int(tot.get("value") or 0), tot.get("relation", "eq")
            if (relation != "eq" or total > FTS_CEILING - FTS_PAGE) and e > s:
                mid = s + (e - s) // 2
                stack.extend([(mid + dt.timedelta(days=1), e), (s, mid)])
                continue
            win_rows = parse_hits(first, qid, (s, e))
            offsets = list(range(FTS_PAGE, min(total, FTS_CEILING), FTS_PAGE))
            # pages are latency-bound (~1 s each): FETCH_WORKERS threads share the host-wide limiter
            with ThreadPoolExecutor(max_workers=FETCH_WORKERS, thread_name_prefix="evt-fts") as pool:
                pages = list(pool.map(lambda off: _fts_page(store, fts_url(q, forms, s, e, off), budget), offsets))
            for page in pages:
                win_rows.extend(parse_hits(page, qid, (s, e)))
            seen: set[tuple[str, str]] = set()
            uniq = []
            for r in win_rows:  # windows are disjoint in file_date; pages of one window may repeat a hit
                k = (r["adsh"], r["file"])
                if k not in seen:
                    seen.add(k)
                    uniq.append(r)
            stats["windows"] += 1
            stats["hits_total"] += total
            stats["hits_unique"] += len(uniq)
            if len(uniq) < min(total, FTS_CEILING):
                stats["short_windows"].append([s.isoformat(), e.isoformat(), total, len(uniq)])
            if uniq:
                writer.write_table(pa.Table.from_pylist(uniq, schema=HIT_SCHEMA))
                rows_out += len(uniq)
    finally:
        writer.close()
    os.replace(tmp, dest)
    stats.update(qid=qid, forms=forms, q=q, start=start.isoformat(), end=end.isoformat(), rows=rows_out,
                 requests_after=budget.used, output=str(dest))
    return stats


def fts_hits_path(qid: str) -> Path:
    return C.build_root() / "_tmp" / "events" / f"fts_{qid}.parquet"


# ---------------------------------------------------------------------------------------------------------
# Documents
# ---------------------------------------------------------------------------------------------------------


def fetch_documents(urls: Iterable[str], *, limit: int | None = None, workers: int = FETCH_WORKERS,
                    store: SH.FetchLedgerStore | None = None, budget: Budget | None = None,
                    progress_every: int = 500) -> dict[str, int]:
    """Fetch each URL not yet ledgered (<= ``limit`` new requests); resumable, budgeted, threaded."""
    store = store or open_store()
    budget = budget or Budget(store)
    counts: Counter[str] = Counter()
    todo = []
    for u in urls:
        rec = store.lookup(u)
        if rec is not None and store._object_present(rec):  # noqa: SLF001
            counts["ledgered"] += 1
            continue
        todo.append(u)
    if limit is not None:
        counts["deferred"] = max(0, len(todo) - limit)
        todo = todo[:limit]

    def one(u: str) -> str:
        rec = ensure(store, u, budget, maximum=MAX_DOCUMENT_BYTES)
        return "ok" if rec.ok else f"status_{rec.status}"

    pool = ThreadPoolExecutor(max_workers=max(1, min(workers, FETCH_WORKERS)), thread_name_prefix="evt-fetch")
    pending: set[Future[str]] = set()
    done = 0
    try:
        for u in todo:
            pending.add(pool.submit(one, u))
            if len(pending) >= 2 * workers:
                fin, pending = wait(pending, return_when=FIRST_COMPLETED)
                for f in fin:
                    counts[f.result()] += 1
                    done += 1
                    if progress_every and done % progress_every == 0:
                        print(json.dumps({"done": done, "of": len(todo), "used": budget.used, **counts}), flush=True)
        while pending:
            fin, pending = wait(pending, return_when=FIRST_COMPLETED)
            for f in fin:
                counts[f.result()] += 1
    except BaseException:
        pool.shutdown(wait=True, cancel_futures=True)
        raise
    pool.shutdown(wait=True)
    counts["requests_used_total"] = budget.used
    return dict(counts)


def landed_release_index() -> dict[str, tuple[str, str]]:
    """``{accession: (exhibit file, document sha256)}`` of the landed v2 earnings-release EX-99 objects."""
    out: dict[str, tuple[str, str]] = {}
    path = EARNINGS_RELEASE_DIR / SH.UNITS_DONE_NAME
    if not path.exists():
        return out
    with path.open("rb") as fh:
        for raw in fh:
            try:
                d = json.loads(raw)
            except ValueError:
                continue
            if d.get("outcome") == "document_ok" and d.get("document_sha256"):
                out[str(d["accession"])] = (str(d.get("exhibit") or ""), str(d["document_sha256"]))
    return out


def read_landed(sha256: str) -> bytes | None:
    for p in (EARNINGS_RELEASE_DIR / "objects" / sha256[:2] / f"{sha256}.gz",):
        if p.exists():
            return gzip.decompress(p.read_bytes())
    p = EARNINGS_RELEASE_DIR / "objects" / sha256[:2] / sha256
    return p.read_bytes() if p.exists() else None


# ---------------------------------------------------------------------------------------------------------
# Document catalog
# ---------------------------------------------------------------------------------------------------------


def catalog_path() -> Path:
    return C.build_root() / "_tmp" / "events" / "doc_catalog.parquet"


def fetched_documents(store: SH.FetchLedgerStore | None = None) -> list[tuple[str, str]]:
    """(url, sha256) of every successfully fetched filing document (not FTS pages) in the landing."""
    store = store or open_store()
    out: dict[str, str] = {}
    if store.ledger_path.exists():
        with store.ledger_path.open("rb") as fh:
            for raw in fh:
                try:
                    d = json.loads(raw)
                except ValueError:
                    continue
                url = str(d.get("url") or "")
                if url.startswith(ARCHIVES) and d.get("status") == 200 and d.get("sha256") and not d.get("error"):
                    out[url] = str(d["sha256"])
    return sorted(out.items())


def build_catalog(con: Any, dest: Path | None = None) -> dict[str, Any]:
    """``_tmp/events/doc_catalog.parquet``: one row per available document (landed v2 EX-99 or fetched here) with
    the filing's metadata and clock from ``sec_filings/filings.parquet`` and the FTS queries that hit it."""
    dest = dest or catalog_path()
    idx = landed_release_index()
    con.register("_landed", pa.table({"accession": list(idx), "file": [v[0] for v in idx.values()],
                                      "sha": [v[1] for v in idx.values()]}))
    docs = fetched_documents()
    parts = [u[len(ARCHIVES) + 1:].split("/") for u, _ in docs]
    con.register("_fetched", pa.table({
        "url": [u for u, _ in docs], "sha": [s for _, s in docs],
        "url_cik": [int(p[0]) for p in parts], "acc18": [p[1] for p in parts], "file": [p[2] for p in parts]}))
    hit_files = sorted(fts_hits_path(q).as_posix() for q in QUERIES if fts_hits_path(q).exists())
    if hit_files:
        lst = ", ".join(f"'{h}'" for h in hit_files)
        con.execute(f"""
            CREATE OR REPLACE TEMP TABLE _hits AS
            SELECT replace(adsh, '-', '') AS acc18, file, any_value(file_type) AS file_type,
                   any_value(file_description) AS file_description, list(DISTINCT qid) AS qids,
                   any_value(ciks) AS ciks
            FROM read_parquet([{lst}]) GROUP BY 1, 2""")
    else:
        con.execute("CREATE OR REPLACE TEMP TABLE _hits (acc18 VARCHAR, file VARCHAR, file_type VARCHAR, "
                    "file_description VARCHAR, qids VARCHAR[], ciks BIGINT[])")
    f = (C.build_root() / "sec_filings" / "filings.parquet").as_posix()
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE _acc AS
        WITH w AS (
            SELECT accession, replace(accession, '-', '') AS acc18 FROM _landed
            UNION SELECT NULL, acc18 FROM _fetched
        )
        SELECT f.cik, f.accession, replace(f.accession, '-', '') AS acc18, f.form, f.items, f.filing_date,
               f.report_date AS event_date, f.available_at, f.acceptance_clock, f.vintage_risk, f.is_amendment
        FROM read_parquet('{f}') f WHERE replace(f.accession, '-', '') IN (SELECT acc18 FROM w)""")
    n = C.copy_to_parquet(con, """
        WITH d AS (
            SELECT l.accession AS acc_hint, replace(l.accession, '-', '') AS acc18, NULL::BIGINT AS url_cik,
                   'landed_v2' AS doc_source, l.file AS source_doc, l.sha AS doc_sha256, NULL AS url FROM _landed l
            UNION ALL
            SELECT NULL, x.acc18, x.url_cik, 'sec_events', x.file, x.sha, x.url FROM _fetched x
        ), m AS (
            SELECT d.*, a.cik, a.accession, a.form, a.items, a.filing_date, a.event_date, a.available_at,
                   a.acceptance_clock, a.vintage_risk, a.is_amendment,
                   row_number() OVER (PARTITION BY d.doc_source, d.acc18, d.source_doc
                                      ORDER BY (a.cik = d.url_cik) DESC, a.cik) AS rn
            FROM d JOIN _acc a ON a.acc18 = d.acc18
        )
        SELECT m.cik, m.accession, m.form, m.items, m.filing_date, m.event_date, m.available_at, m.acceptance_clock,
               m.vintage_risk, m.is_amendment, m.doc_source, m.source_doc, m.doc_sha256, m.url, h.file_type,
               h.file_description, coalesce(h.qids, []::VARCHAR[]) AS qids
        FROM m LEFT JOIN _hits h ON h.acc18 = m.acc18 AND h.file = m.source_doc
        WHERE m.rn = 1
        ORDER BY m.cik, m.available_at, m.accession, m.source_doc""", dest)
    for t in ("_landed", "_fetched"):
        con.unregister(t)
    return {"documents": n, "landed_index": len(idx), "fetched_documents": len(docs)}


def iter_catalog(pred: Callable[[dict[str, Any]], bool] | None = None, path: Path | None = None
                 ) -> Iterator[dict[str, Any]]:
    """Catalog rows (pyarrow only, no DuckDB) passing ``pred``."""
    path = path or catalog_path()
    for batch in pq.ParquetFile(path).iter_batches(batch_size=4000):
        for r in batch.to_pylist():
            if pred is None or pred(r):
                yield r


def read_document(r: dict[str, Any], store: SH.FetchLedgerStore | None = None) -> bytes | None:
    if r["doc_source"] == "landed_v2":
        return read_landed(r["doc_sha256"])
    return read_record(store or open_store(), r["url"])


# ---------------------------------------------------------------------------------------------------------
# Text
# ---------------------------------------------------------------------------------------------------------

_BLOCK_TAGS = ("tr", "p", "div", "br", "li", "h1", "h2", "h3", "h4", "h5", "h6", "table", "center", "ul", "ol",
               "title", "font_block")
_WS = re.compile(r"[ \t\r\f\v  ​   ]+")
_PUNCT = str.maketrans({"\xa0": " ", "’": "'", "‘": "'", "“": '"', "”": '"', "–": "-",
                        "—": "-", "−": "-", "‐": "-", "‑": "-", "•": " ", "·": " ",
                        "●": " ", "▪": " ", "‣": " ", "�": " ", "†": " ", "‡": " "})


def document_text(blob: bytes, name: str = "") -> str:
    """Plain text of an EDGAR document: one line per block element, `` | `` between table cells."""
    if not blob:
        return ""
    head = blob[:2000].lower()
    try:
        raw = blob.decode("utf-8")
    except UnicodeDecodeError:
        raw = blob.decode("cp1252", errors="replace")  # EDGAR HTML is mostly ASCII + windows-1252 punctuation
    is_html = name.lower().endswith((".htm", ".html")) or b"<html" in head or b"<div" in head or b"<p" in head
    if is_html:
        import lxml.html

        raw = re.sub(r"^\s*<\?xml[^>]*\?>", "", raw)  # lxml refuses str input carrying an encoding declaration
        try:
            root = lxml.html.document_fromstring(raw)
        except Exception:  # noqa: BLE001 - malformed markup: fall back to a tag strip
            root = None
        if root is not None:
            for bad in root.xpath("//script|//style|//head"):
                bad.drop_tree()
            for el in root.iter():  # source line breaks are not structure (only block elements are)
                if el.tag == "pre":
                    continue
                if el.text and ("\n" in el.text or "\r" in el.text):
                    el.text = el.text.replace("\r", " ").replace("\n", " ")
                if el.tail and ("\n" in el.tail or "\r" in el.tail):
                    el.tail = el.tail.replace("\r", " ").replace("\n", " ")
            # footnote markers glue onto numbers ("$2.55<sup>1</sup>", CSS-raised <font>/<span>): drop them
            for sup in list(root.iter("sup", "font", "span")):
                txt = (sup.text_content() or "").strip()
                if not txt or len(txt) > 3 or len(sup) or txt.lower() in ("st", "nd", "rd", "th"):
                    continue
                style = (sup.get("style") or "").replace(" ", "").lower()
                if sup.tag == "sup" or "vertical-align:super" in style or re.search(r"(?:^|;)top:-\d", style):
                    sup.drop_tree()
            # block elements inside a table cell stay on the row's line (a <tr> is one line)
            # (the list keeps the element proxies alive, so their ids stay theirs during the pass below)
            cell_blocks = [el for cell in root.iter("td", "th") for el in cell.iterdescendants()
                           if el.tag in _BLOCK_TAGS and el.tag not in ("table", "tr")]
            in_cell = {id(el) for el in cell_blocks}
            for el in root.iter("td", "th"):
                el.tail = " | " + (el.tail or "")
            for el in root.iter(*_BLOCK_TAGS):
                el.tail = (" " if id(el) in in_cell else "\n") + (el.tail or "")
            text = root.text_content()
        else:
            text = re.sub(r"<[^>]+>", " ", raw)
    else:
        text = raw
    text = text.translate(_PUNCT)
    lines = []
    for line in text.split("\n"):
        line = _WS.sub(" ", line).strip()
        if line and line.strip("| "):
            lines.append(line)
    return "\n".join(lines)


def peak_memory() -> dict[str, Any]:
    """Peak working set / pagefile (commit) of this process in bytes (Windows), else ru_maxrss."""
    try:
        import ctypes
        from ctypes import wintypes

        class PMC(ctypes.Structure):
            _fields_ = [("cb", wintypes.DWORD), ("PageFaultCount", wintypes.DWORD),
                        ("PeakWorkingSetSize", ctypes.c_size_t), ("WorkingSetSize", ctypes.c_size_t),
                        ("QuotaPeakPagedPoolUsage", ctypes.c_size_t), ("QuotaPagedPoolUsage", ctypes.c_size_t),
                        ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t), ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
                        ("PagefileUsage", ctypes.c_size_t), ("PeakPagefileUsage", ctypes.c_size_t)]

        pmc = PMC()
        pmc.cb = ctypes.sizeof(PMC)
        k32 = ctypes.windll.kernel32
        k32.GetCurrentProcess.restype = wintypes.HANDLE
        k32.K32GetProcessMemoryInfo.argtypes = [wintypes.HANDLE, ctypes.POINTER(PMC), wintypes.DWORD]
        if not k32.K32GetProcessMemoryInfo(k32.GetCurrentProcess(), ctypes.byref(pmc), pmc.cb):
            raise OSError("K32GetProcessMemoryInfo failed")
        return {"peak_working_set_bytes": int(pmc.PeakWorkingSetSize),
                "peak_commit_bytes": int(pmc.PeakPagefileUsage)}
    except (AttributeError, OSError):
        import resource  # type: ignore[import-not-found]

        return {"ru_maxrss_kb": resource.getrusage(resource.RUSAGE_SELF).ru_maxrss}


# ---------------------------------------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------------------------------------


def _write_log(kind: str, payload: dict[str, Any]) -> None:
    log = C.build_root() / "_logs" / "events_sources.jsonl"
    log.parent.mkdir(parents=True, exist_ok=True)
    with log.open("a", encoding="utf-8") as fh:
        fh.write(json.dumps({"kind": kind, "at": dt.datetime.now(dt.UTC).isoformat(timespec="seconds"),
                             **payload}, default=str) + "\n")


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    h = sub.add_parser("harvest")
    h.add_argument("qids", nargs="+", choices=sorted(QUERIES))
    h.add_argument("--start", type=dt.date.fromisoformat, default=WINDOW_START)
    h.add_argument("--end", type=dt.date.fromisoformat, default=WINDOW_END)
    f = sub.add_parser("fetch")
    f.add_argument("url_list", type=Path, help="text file, one URL per line (priority order)")
    f.add_argument("--limit", type=int, default=None)
    sub.add_parser("status")
    sub.add_parser("catalog", help="rebuild _tmp/events/doc_catalog.parquet (DuckDB: run guarded)")
    args = ap.parse_args(argv)
    if args.cmd == "catalog":
        con = C.connect(memory="300MB", threads=2)
        out = build_catalog(con)
        con.close()
        _write_log("catalog", out)
        print(json.dumps(out), flush=True)
        return 0
    store = open_store()
    budget = Budget(store)
    if args.cmd == "harvest":
        out = {}
        for qid in args.qids:
            out[qid] = harvest(qid, args.start, args.end, store, budget)
            _write_log("harvest", out[qid])
            print(json.dumps(out[qid], default=str), flush=True)
    elif args.cmd == "fetch":
        urls = [u.strip() for u in args.url_list.read_text(encoding="utf-8").splitlines() if u.strip()]
        out = fetch_documents(urls, limit=args.limit, store=store, budget=budget)
        out["url_list"] = str(args.url_list)
        _write_log("fetch", out)
    else:
        out = {"ledger_lines": store.ledger_lines, "terminal": store.terminal_count, "cap": budget.cap}
    out_peak = peak_memory()
    _write_log("peak_memory", {"cmd": args.cmd, **out_peak})
    print(json.dumps({"result": out, "peak_memory": out_peak}, default=str), flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
