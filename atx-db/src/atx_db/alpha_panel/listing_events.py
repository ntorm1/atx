"""S2.2 listing events, line listing dates and issuer name history (stage ``security_master``, new files).

Sources: the SEC submissions archive ``data/cache/submissions.zip`` (read in place with the streaming reader of
``sec_filings``) for listing forms of every year and the filer addresses, and the ``sec_filings`` stage
(2009+ filings with resolved acceptance clocks). Lines come from ``security_master/lines.parquet``; issuers
from the identity links (``identity/links.parquet`` r4 strict, 2012+, and ``identity/link_table.parquet``).

Outputs:

* ``listing_events.parquet`` (event grain, one row per (cik, accession)): ``event_class`` (``registration``:
  8-A12B/G, 10-12B/G, 20-FR12B/G, 40FR12B/G; ``successor``: 8-K12B, 8-K12G3, 8-K15D5; ``certification``: an
  exchange's CERT* approval; ``offering``: S-1, S-11, F-1, SB-2 (originals), 424B4/424B1 (final prospectus),
  EFFECT (notice of effectiveness); ``adr``: F-6 family; ``delisting``: 25, 25-NSE; ``deregistration``: 15-12B/G,
  15-15D, 15F-*), ``form, filing_date, available_at, available_basis, file_number, is_amendment, source``
  (``sec_filings`` 2009+, else ``submissions_zip``).
* ``name_history.parquet``: one row per (cik, name, valid_from): the SEC ``formerNames`` ranges and the current
  name from the last change. ``available_at`` = 22:00 UTC of the day the name took effect on EDGAR (the
  archive is a 2026-09-19 snapshot; ``vintage_risk = 'snapshot_dates'``).
* ``filer_addresses.parquet``: snapshot business / mailing addresses, LEI, EIN, state of incorporation per CIK
  (``vintage_risk = 'snapshot_non_pit'``), read by the LEI name-address match.
* ``line_listing.parquet``: one row per vendor line, rule ``line-listing-v1`` (below).

Rule ``line-listing-v1`` for a line with first vendor session f and issuer ``cik0`` (the CIK of its earliest
link, strict first):

1. ``sec_confirmed``: ``cik0`` has a registration, certification, successor, F-6, 424B4/424B1 or EFFECT event
   filed in [f - ``CONFIRM_BEFORE_DAYS``, f + ``CONFIRM_AFTER_DAYS``], or an S-1/S-11/F-1/SB-2 original in
   [f - 400 d, f]. ``listing_date`` = f; ``listing_type`` = ipo (an offering form), spin_off (10-12B/G),
   successor (8-K12*), adr (F-6 / 20-FR12B), else exchange_registration.
2. ``successor_of_line``: another line of ``cik0`` (or the line whose last ticker equals this line's first
   ticker) ended within ``SUCCESSOR_GAP_DAYS`` before f: a vendor re-key (holding-company reorganisation,
   redomicile). ``listing_date`` = the predecessor's ``listing_date`` (NULL when the predecessor is censored).
3. ``vendor_batch``: f is a vendor coverage batch (``BATCH_MIN_LINES`` or more lines first appear that session;
   2012-03-26 is the file start): the line traded before the vendor saw it. ``listing_date`` = the earliest
   registration / certification of ``cik0`` before f when one exists (``sec_registration_before_vendor``),
   else NULL (``censored``).
4. otherwise ``vendor_first_session``: ``listing_date`` = f (not censored, no SEC corroboration; mostly
   non-operating lines: ETFs, preferreds, warrants, units, unlinked lines).

``listing_available_at`` = the first session's 22:00 UTC, or the confirming event's ``available_at`` when later
(never backdated). ``delisting_date`` / ``delisting_form``: the line's last session when it died before
2026-09-01, with the 25 / 25-NSE / 15 of ``cik0`` filed in [last - 60 d, last + 30 d] when one exists.

Usage (under the memory guard)::

    python -m atx_db.alpha_panel.listing_events extract     # resumable zip pass -> _tmp/identity_v3/sub_*
    python -m atx_db.alpha_panel.listing_events build       # the four outputs + manifest
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import sys
import time
from pathlib import Path
from typing import Any
from zoneinfo import ZoneInfo

import pyarrow as pa
import pyarrow.parquet as pq

from . import common as C
from . import sec_filings as SF

STAGE = "security_master"
SCHEMA = "atx.alpha-panel.listing-events/v1"
RULE = "line-listing-v1"
MODULES = ("listing_events", "sec_filings", "common")
MEMORY = "250MB"
CHUNK_ENTRIES = 50_000
CONFIRM_BEFORE_DAYS = 180
CONFIRM_AFTER_DAYS = 10
OFFERING_LOOKBACK_DAYS = 400
SUCCESSOR_GAP_DAYS = 10
BATCH_MIN_LINES = 100
DEAD_BEFORE = dt.date(2026, 9, 1)
POST_2012 = dt.date(2013, 1, 1)

REGISTRATION = ("8-A12B", "8-A12G", "10-12B", "10-12G", "20-FR12B", "20-FR12G", "40FR12B", "40FR12G")
SUCCESSOR = ("8-K12B", "8-K12G3", "8-K15D5")
OFFERING_ORIGINAL = ("S-1", "S-11", "F-1", "SB-2")
PROSPECTUS = ("424B4", "424B1")
EFFECT = ("EFFECT",)
ADR = ("F-6", "F-6EF", "F-6 POS")
DELISTING = ("25", "25-NSE")
DEREGISTRATION = ("15-12B", "15-12G", "15-15D", "15F-12B", "15F-12G", "15F-15D")
ALL_FORMS = REGISTRATION + SUCCESSOR + OFFERING_ORIGINAL + PROSPECTUS + EFFECT + ADR + DELISTING + DEREGISTRATION
#: Byte tokens that pre-filter archive members before a JSON parse (a member without any cannot hold an event).
TOKENS = tuple(f'"{f}'.encode() for f in ("8-A12", "10-12", "20-FR12", "40FR12", "8-K12", "8-K15D5", "CERT", "S-1",
                                          "S-11", "F-1", "SB-2", "424B4", "424B1", "EFFECT", "F-6", "25", "15-1",
                                          "15F-"))


def event_class(form: str | None) -> str | None:
    """Listing event class of one EDGAR form (amendments share their base form's class); None otherwise."""
    f = (form or "").strip().upper()
    base = f[:-2] if f.endswith("/A") else f
    if base in REGISTRATION:
        return "registration"
    if base in SUCCESSOR:
        return "successor"
    if base.startswith("CERT"):
        return "certification"
    if base in OFFERING_ORIGINAL or base in PROSPECTUS or base in EFFECT:
        return "offering"
    if base in ADR:
        return "adr"
    if base in DELISTING:
        return "delisting"
    if base in DEREGISTRATION:
        return "deregistration"
    return None


def listing_type(forms: set[str]) -> str:
    """Listing type from the confirming forms of one line (base forms, upper case)."""
    if forms & set(OFFERING_ORIGINAL + PROSPECTUS):
        return "ipo"
    if forms & {"10-12B", "10-12G"}:
        return "spin_off"
    if forms & set(SUCCESSOR):
        return "successor"
    if forms & (set(ADR) | {"20-FR12B", "20-FR12G"}):
        return "adr"
    return "exchange_registration"


def name_ranges(current: str | None, former: list[dict[str, Any]]) -> list[tuple[str, dt.date | None, dt.date | None]]:
    """``[(name, valid_from, valid_to)]`` from the SEC ``formerNames`` list plus the current name (valid from the
    day after the latest former name's ``to`` date, open-ended). Dates are the ``from`` / ``to`` days."""
    out: list[tuple[str, dt.date | None, dt.date | None]] = []
    last_to: dt.date | None = None
    for fn in former:
        nm = (fn.get("name") or "").strip()
        a, b = _day(fn.get("from")), _day(fn.get("to"))
        if not nm:
            continue
        out.append((nm, a, b))
        if b and (last_to is None or b > last_to):
            last_to = b
    if current:
        out.append((current.strip(), last_to + dt.timedelta(days=1) if last_to else None, None))
    return sorted(out, key=lambda r: (r[1] or dt.date.min, r[0]))


_ET = ZoneInfo("America/New_York")


def _day(v: Any) -> dt.date | None:
    """SEC former-name stamps are midnight America/New_York written in UTC (``2007-01-10T05:00:00.000Z``)."""
    if not v:
        return None
    try:
        ts = dt.datetime.fromisoformat(str(v).replace("Z", "+00:00"))
    except ValueError:
        return None
    return ts.astimezone(_ET).date() if ts.tzinfo else ts.date()


# ---------------------------------------------------------------- extract
def iter_entries_fast(fh, block: int = 1 << 22):
    """``sec_filings.iter_zip_entries`` with an offset cursor: the buffer is compacted once per block, not
    re-sliced per entry (the per-entry slice made the 991k-entry directory scan take ~55 min). Same records."""
    import struct

    n, cd_size, cd_off = SF._central_directory(fh)
    pos_file, end_file = cd_off, cd_off + cd_size
    buf, pos, idx = b"", 0, 0
    while idx < n:
        if len(buf) - pos < 46 + 65535 * 3 and pos_file < end_file:
            fh.seek(pos_file)
            chunk = fh.read(min(block, end_file - pos_file))
            pos_file += len(chunk)
            buf, pos = buf[pos:] + chunk, 0
        if len(buf) - pos < 46:
            raise ValueError("truncated central directory")
        (sig, _vm, _vn, _flag, method, _mt, _md, crc, csize, usize, nlen, xlen, clen, _dn, _ia, _ea,
         loff) = struct.unpack_from("<IHHHHHHIIIHHHHHII", buf, pos)
        if sig != 0x02014B50:
            raise ValueError(f"bad central directory signature at entry {idx}")
        rec_len = 46 + nlen + xlen + clen
        if len(buf) - pos < rec_len:
            if pos_file >= end_file:
                raise ValueError("truncated central directory entry")
            fh.seek(pos_file)
            chunk = fh.read(min(block, end_file - pos_file))
            pos_file += len(chunk)
            buf, pos = buf[pos:] + chunk, 0
            continue
        name = buf[pos + 46:pos + 46 + nlen].decode("utf-8", "replace")
        if csize == 0xFFFFFFFF or usize == 0xFFFFFFFF or loff == 0xFFFFFFFF:
            extra = buf[pos + 46 + nlen:pos + 46 + nlen + xlen]
            q = 0
            while q + 4 <= len(extra):
                hid, hl = struct.unpack("<HH", extra[q:q + 4])
                if hid == 1:
                    data, k = extra[q + 4:q + 4 + hl], 0
                    if usize == 0xFFFFFFFF:
                        usize = struct.unpack("<Q", data[k:k + 8])[0]
                        k += 8
                    if csize == 0xFFFFFFFF:
                        csize = struct.unpack("<Q", data[k:k + 8])[0]
                        k += 8
                    if loff == 0xFFFFFFFF:
                        loff = struct.unpack("<Q", data[k:k + 8])[0]
                    break
                q += 4 + hl
        yield SF.ZipEntry(idx, name, method, crc, csize, usize, loff)
        pos += rec_len
        idx += 1


def work_dir() -> Path:
    p = C.build_root() / "_tmp" / "identity_v3" / "sub"
    p.mkdir(parents=True, exist_ok=True)
    return p


EVENT_SCHEMA = pa.schema([("cik", pa.int64()), ("source_file", pa.int16()), ("accession", pa.string()),
                          ("form", pa.string()), ("filing_date", pa.string()), ("acceptance_raw", pa.string()),
                          ("file_number", pa.string()), ("report_date", pa.string())])
ADDR_FIELDS = ("street1", "street2", "city", "stateOrCountry", "zipCode", "country", "countryCode")
FILER_SCHEMA = pa.schema(
    [("cik", pa.int64()), ("name", pa.string()), ("entity_type", pa.string()), ("lei", pa.string()),
     ("ein", pa.string()), ("state_of_incorporation", pa.string()), ("tickers", pa.list_(pa.string())),
     ("exchanges", pa.list_(pa.string()))]
    + [(f"business_{k}", pa.string()) for k in ADDR_FIELDS] + [(f"mailing_{k}", pa.string()) for k in ADDR_FIELDS]
    + [("former_names", SF.FORMER_NAME_TYPE)])


def _events_of(doc: dict[str, Any], cik: int, source: int) -> dict[str, list[Any]]:
    block = SF.columnar_block(doc, source)
    forms = block.get("form") or []
    idx = [i for i, f in enumerate(forms) if event_class(f)]
    out: dict[str, list[Any]] = {k.name: [] for k in EVENT_SCHEMA}

    def col(key: str) -> list[Any]:
        v = block.get(key) or []
        return v if len(v) == len(forms) else (list(v) + [None] * len(forms))[:len(forms)]

    acc, fd, ac, fn, rd = (col(k) for k in ("accessionNumber", "filingDate", "acceptanceDateTime", "fileNumber",
                                             "reportDate"))
    for i in idx:
        out["cik"].append(cik)
        out["source_file"].append(source)
        out["accession"].append(acc[i])
        out["form"].append(forms[i])
        out["filing_date"].append(fd[i])
        out["acceptance_raw"].append(ac[i])
        out["file_number"].append(fn[i] or None)
        out["report_date"].append(rd[i] or None)
    return out


def _filer_row(doc: dict[str, Any], cik: int) -> dict[str, Any]:
    addr = doc.get("addresses") or {}
    row: dict[str, Any] = {"cik": cik, "name": doc.get("name"), "entity_type": doc.get("entityType"),
                           "lei": doc.get("lei") or None, "ein": doc.get("ein") or None,
                           "state_of_incorporation": doc.get("stateOfIncorporation") or None,
                           "tickers": [str(t) for t in doc.get("tickers") or [] if t],
                           "exchanges": [str(e) for e in doc.get("exchanges") or [] if e]}
    for kind in ("business", "mailing"):
        a = addr.get(kind) or {}
        for k in ADDR_FIELDS:
            v = a.get(k)
            row[f"{kind}_{k}"] = None if v in (None, "") else str(v)
    row["former_names"] = [{"name": fn.get("name"), "from_date": _day(fn.get("from")), "to_date": _day(fn.get("to"))}
                           for fn in doc.get("formerNames") or [] if isinstance(fn, dict)]
    return row


def extract(zip_path: Path = SF.SUBMISSIONS_ZIP) -> dict[str, Any]:
    """Stream the archive in chunks of ``CHUNK_ENTRIES`` members (resumable: a chunk whose parts exist is
    skipped) -> ``events_NNNN.parquet`` (listing forms, every year) and ``filers_NNNN.parquet`` (main files of
    operating filers and of any filer with a listing event). One runner at a time (lock file)."""
    from .figi_lei import RunLock, peak_memory_gb

    with RunLock(work_dir()):
        stats = _extract(zip_path)
    stats["memory"] = peak_memory_gb()
    C.write_json_atomic(work_dir() / "extract_receipt.json", stats)
    return stats


def _extract(zip_path: Path) -> dict[str, Any]:
    out = work_dir()
    stats: dict[str, Any] = {"chunks_written": 0, "members_parsed": 0, "members_skipped": 0}
    t0 = time.perf_counter()
    with zip_path.open("rb") as fh:
        n, _, _ = SF._central_directory(fh)
        n_chunks = (n + CHUNK_ENTRIES - 1) // CHUNK_ENTRIES
        stats["entries"], stats["chunks"] = n, n_chunks
        cur = -1
        ev: dict[str, list[Any]] = {}
        filers: list[dict[str, Any]] = []

        def flush(c: int) -> None:
            _write(pa.Table.from_pydict(ev, schema=EVENT_SCHEMA), out / f"events_{c:04d}.parquet")
            _write(pa.Table.from_pylist(filers, schema=FILER_SCHEMA), out / f"filers_{c:04d}.parquet")
            stats["chunks_written"] += 1

        skip = False
        for entry in iter_entries_fast(fh):
            c = entry.index // CHUNK_ENTRIES
            if c != cur:
                if cur >= 0 and not skip:
                    flush(cur)
                cur, ev, filers = c, {k.name: [] for k in EVENT_SCHEMA}, []
                skip = (out / f"events_{c:04d}.parquet").exists() and (out / f"filers_{c:04d}.parquet").exists()
                if not skip:
                    print(f"[listing_events] chunk {c + 1}/{n_chunks} {time.perf_counter() - t0:.0f}s", flush=True)
            if skip:
                continue
            kind = SF.member_kind(entry.name)
            if kind is None:
                continue
            raw = SF.read_zip_member(fh, entry)
            operating = kind[1] == 0 and b'"entityType":"operating"' in raw[:4096]
            if not operating and not any(t in raw for t in TOKENS):
                stats["members_skipped"] += 1
                continue
            doc = json.loads(raw)
            stats["members_parsed"] += 1
            cik, source = kind
            e = _events_of(doc, cik, source)
            for k, v in e.items():
                ev[k].extend(v)
            if source == 0 and (operating or e["cik"]):
                filers.append(_filer_row(doc, cik))
        if cur >= 0 and not skip:
            flush(cur)
    stats["seconds"] = round(time.perf_counter() - t0, 1)
    return stats


def _write(table: pa.Table, dest: Path) -> None:
    tmp = dest.with_name(dest.name + ".partial")
    pq.write_table(table, tmp, compression="zstd")
    tmp.replace(dest)


# ---------------------------------------------------------------- build
def _sql_list(values: tuple[str, ...]) -> str:
    return ", ".join("'" + v + "'" for v in values)


def build() -> dict[str, Any]:
    t0 = time.perf_counter()
    root = C.build_root()
    wd = work_dir()
    out = C.stage_dir(STAGE)
    con = C.connect(memory=MEMORY, threads=2)
    con.create_function("event_class", event_class, ["VARCHAR"], "VARCHAR", null_handling="special")
    receipt: dict[str, Any] = {"rule": RULE}
    filings = (root / "sec_filings" / "filings.parquet").as_posix()
    ev_glob = (wd / "events_*.parquet").as_posix()
    fl_glob = (wd / "filers_*.parquet").as_posix()
    eod = SF.EOD_SQL
    # events: sec_filings (2009+, resolved clocks) wins; the archive adds earlier years
    con.execute(f"""CREATE TABLE ev AS
        SELECT cik, accession, form, filing_date, available_at, available_basis, file_number, is_amendment,
               'sec_filings' AS source, event_class(form) AS event_class
        FROM read_parquet('{filings}')
        WHERE (upper(regexp_replace(form, '/A$', '')) IN ({_sql_list(ALL_FORMS)}) OR upper(form) LIKE 'CERT%')
          AND event_class(form) IS NOT NULL""")
    con.execute(f"""INSERT INTO ev
        WITH z AS (
            SELECT cik, accession, form, try_cast(filing_date AS DATE) AS filing_date, file_number,
                   row_number() OVER (PARTITION BY cik, accession ORDER BY source_file) AS rn
            FROM read_parquet('{ev_glob}')
        )
        SELECT cik, accession, form, filing_date, {eod.replace('filing_date', 'z.filing_date')} AS available_at,
               'filing_date_eod_et', file_number, upper(form) LIKE '%/A', 'submissions_zip', event_class(form)
        FROM z WHERE rn = 1 AND filing_date IS NOT NULL
          AND NOT EXISTS (SELECT 1 FROM ev e WHERE e.cik = z.cik AND e.accession = z.accession)""")
    receipt["listing_events"] = C.copy_to_parquet(con, """
        SELECT cik, accession, form, upper(regexp_replace(form, '/A$', '')) AS base_form, event_class, filing_date,
               available_at, available_basis, file_number, is_amendment, source
        FROM ev ORDER BY cik, filing_date, accession""", out / "listing_events.parquet")
    receipt["events_by_class"] = dict(con.execute(
        "SELECT event_class || ':' || source, count(*) FROM ev GROUP BY 1 ORDER BY 1").fetchall())
    # filers (latest main file per cik) and name history
    con.execute(f"""CREATE TABLE filers AS
        SELECT * EXCLUDE (rn) FROM (SELECT *, row_number() OVER (PARTITION BY cik ORDER BY name) AS rn
                                    FROM read_parquet('{fl_glob}')) WHERE rn = 1""")
    receipt["filer_addresses"] = C.copy_to_parquet(con, f"""
        SELECT * EXCLUDE (former_names), 'snapshot_non_pit' AS vintage_risk, DATE '{SF.SNAPSHOT_FETCHED}' AS snapshot_date
        FROM filers ORDER BY cik""", out / "filer_addresses.parquet")
    rows = []
    for cik, name, former in con.execute("SELECT cik, name, former_names FROM filers").fetchall():
        fm = [{"name": f["name"], "from": f["from_date"].isoformat() if f["from_date"] else None,
               "to": f["to_date"].isoformat() if f["to_date"] else None} for f in former or []]
        for nm, a, b in name_ranges(name, fm):
            rows.append((cik, nm, a, b, b is None))
    names_tbl = pa.table({"cik": pa.array([r[0] for r in rows], pa.int64()), "name": [r[1] for r in rows],
                          "valid_from": pa.array([r[2] for r in rows], pa.date32()),
                          "valid_to": pa.array([r[3] for r in rows], pa.date32()), "is_current": [r[4] for r in rows]})
    con.register("names_arrow", names_tbl)
    con.execute("CREATE TABLE names AS SELECT * FROM names_arrow")
    con.unregister("names_arrow")
    del names_tbl, rows
    receipt["name_history"] = C.copy_to_parquet(con, """
        SELECT cik, name, valid_from, valid_to, is_current,
               CASE WHEN valid_from IS NULL THEN NULL ELSE CAST(valid_from AS TIMESTAMP) + INTERVAL 22 HOUR END
                   AS available_at,
               CASE WHEN is_current THEN 'sec_current_name' ELSE 'sec_former_names' END AS source,
               'snapshot_dates' AS vintage_risk
        FROM names ORDER BY cik, valid_from NULLS FIRST, name""", out / "name_history.parquet")
    receipt["line_listing"] = _line_listing(con, root, out)
    receipt["seconds"] = round(time.perf_counter() - t0, 1)
    con.close()
    return receipt


def _line_listing(con, root: Path, out: Path) -> dict[str, Any]:
    lines = (root / STAGE / "lines.parquet").as_posix()
    strict = (root / "identity" / "links.parquet").as_posix()
    table = (root / "identity" / "link_table.parquet").as_posix()
    th = C.TICKERHISTORY.as_posix()
    conf = _sql_list(REGISTRATION + SUCCESSOR + PROSPECTUS + EFFECT + ADR)
    orig = _sql_list(OFFERING_ORIGINAL)
    reg = _sql_list(REGISTRATION + SUCCESSOR)
    con.execute(f"""CREATE TABLE ln AS
        SELECT l.*, f.first_ticker FROM read_parquet('{lines}') l
        LEFT JOIN (SELECT securityID AS security_id, arg_min(ticker_tk, tradingDate) AS first_ticker
                   FROM read_parquet('{th}') WHERE securityID > 0 GROUP BY 1) f USING (security_id)""")
    con.execute(f"""CREATE TABLE cik0 AS
        SELECT security_id, arg_min(cik, pri * 100000 + (d - DATE '1990-01-01')) AS cik0 FROM (
            SELECT security_id, cik, 0 AS pri, "start" AS d FROM read_parquet('{strict}')
            UNION ALL
            SELECT security_id, cik, CASE link_tier WHEN 'strict' THEN 0 WHEN 'name' THEN 1 ELSE 2 END, valid_from
            FROM read_parquet('{table}'))
        GROUP BY 1""")
    con.execute(f"""CREATE TABLE batch AS
        SELECT first_session AS d FROM ln GROUP BY 1 HAVING count(*) >= {BATCH_MIN_LINES}""")
    con.execute(f"""CREATE TABLE confirm AS
        SELECT l.security_id, list(DISTINCT e.base_form ORDER BY e.base_form) AS forms,
               arg_min(e.base_form, e.filing_date) AS first_form, min(e.filing_date) AS first_event_date,
               max(e.available_at) FILTER (WHERE e.filing_date <= l.first_session) AS event_available_at
        FROM ln l JOIN cik0 c USING (security_id)
        JOIN (SELECT *, upper(regexp_replace(form, '/A$', '')) AS base_form FROM ev) e ON e.cik = c.cik0
        WHERE (e.base_form IN ({conf}) OR e.base_form LIKE 'CERT%')
              AND e.filing_date BETWEEN l.first_session - INTERVAL {CONFIRM_BEFORE_DAYS} DAY
                                    AND l.first_session + INTERVAL {CONFIRM_AFTER_DAYS} DAY
           OR e.base_form IN ({orig}) AND e.filing_date BETWEEN l.first_session - INTERVAL {OFFERING_LOOKBACK_DAYS} DAY
                                                          AND l.first_session
        GROUP BY 1""")
    con.execute(f"""CREATE TABLE pred AS
        SELECT l.security_id, arg_max(p.security_id, p.last_session) AS pred_id
        FROM ln l LEFT JOIN cik0 c USING (security_id)
        JOIN ln p ON p.security_id <> l.security_id
             AND p.last_session < l.first_session
             AND p.last_session >= l.first_session - INTERVAL {SUCCESSOR_GAP_DAYS} DAY
        LEFT JOIN cik0 pc ON pc.security_id = p.security_id
        WHERE (c.cik0 IS NOT NULL AND pc.cik0 = c.cik0) OR (p.last_ticker = l.first_ticker)
        GROUP BY 1""")
    con.execute(f"""CREATE TABLE prior_reg AS
        SELECT l.security_id, min(e.filing_date) AS reg_date, arg_min(e.form, e.filing_date) AS reg_form
        FROM ln l JOIN cik0 c USING (security_id)
        JOIN ev e ON e.cik = c.cik0
        WHERE (upper(regexp_replace(e.form, '/A$', '')) IN ({reg}) OR upper(e.form) LIKE 'CERT%')
          AND e.filing_date < l.first_session
        GROUP BY 1""")
    con.execute(f"""CREATE TABLE delist AS
        SELECT l.security_id, arg_min(e.form, e.filing_date) AS delisting_form, min(e.filing_date) AS delisting_form_date
        FROM ln l JOIN cik0 c USING (security_id) JOIN ev e ON e.cik = c.cik0
        WHERE l.last_session < DATE '{DEAD_BEFORE}' AND e.event_class IN ('delisting', 'deregistration')
          AND e.filing_date BETWEEN l.last_session - INTERVAL 60 DAY AND l.last_session + INTERVAL 30 DAY
        GROUP BY 1""")
    con.create_function("listing_type", lambda forms: listing_type(set(forms or [])) if forms else None,
                        ["VARCHAR[]"], "VARCHAR", null_handling="special")
    con.execute("""CREATE TABLE base AS
        SELECT l.security_id, l.first_session, l.last_session, l.left_censored, l.first_ticker, l.last_ticker,
               l.is_index_line, c.cik0 AS cik, cf.forms AS confirm_forms, cf.first_form, cf.first_event_date,
               cf.event_available_at, p.pred_id, pr.reg_date, pr.reg_form, b.d IS NOT NULL AS vendor_batch,
               d.delisting_form, d.delisting_form_date,
               CASE WHEN cf.security_id IS NOT NULL THEN 'sec_confirmed'
                    WHEN p.pred_id IS NOT NULL THEN 'successor_of_line'
                    WHEN b.d IS NOT NULL AND pr.reg_date IS NOT NULL THEN 'sec_registration_before_vendor'
                    WHEN b.d IS NOT NULL THEN 'censored'
                    ELSE 'vendor_first_session' END AS listing_basis
        FROM ln l LEFT JOIN cik0 c USING (security_id) LEFT JOIN confirm cf USING (security_id)
        LEFT JOIN pred p USING (security_id) LEFT JOIN prior_reg pr USING (security_id)
        LEFT JOIN batch b ON b.d = l.first_session LEFT JOIN delist d USING (security_id)""")
    # successor chains: inherit the predecessor's resolved date (iterate; chains are short)
    con.execute("""ALTER TABLE base ADD COLUMN listing_date DATE""")
    con.execute("""UPDATE base SET listing_date = CASE listing_basis
        WHEN 'sec_confirmed' THEN first_session WHEN 'vendor_first_session' THEN first_session
        WHEN 'sec_registration_before_vendor' THEN reg_date END""")
    for _ in range(6):
        con.execute("""UPDATE base SET listing_date = p.listing_date
            FROM base p WHERE base.listing_basis = 'successor_of_line' AND p.security_id = base.pred_id
              AND base.listing_date IS DISTINCT FROM p.listing_date""")
    dest = out / "line_listing.parquet"
    n = C.copy_to_parquet(con, f"""
        SELECT security_id, cik, first_session, last_session, left_censored, vendor_batch, first_ticker, last_ticker,
               listing_date, listing_basis, listing_type(confirm_forms) AS listing_type, confirm_forms,
               first_form AS listing_event_form, first_event_date AS listing_event_date,
               pred_id AS predecessor_security_id, reg_form AS prior_registration_form,
               CASE WHEN listing_date IS NULL THEN NULL
                    ELSE greatest(CAST(first_session AS TIMESTAMP) + INTERVAL 22 HOUR,
                                  coalesce(event_available_at, TIMESTAMP '1900-01-01')) END AS listing_available_at,
               CASE WHEN last_session < DATE '{DEAD_BEFORE}' THEN last_session END AS delisting_date,
               delisting_form, delisting_form_date,
               CASE WHEN last_session < DATE '{DEAD_BEFORE}' THEN CAST(last_session AS TIMESTAMP) + INTERVAL 22 HOUR END
                   AS delisting_available_at,
               is_index_line
        FROM base ORDER BY security_id""", dest)
    d = dest.as_posix()
    member = (root / "_tmp" / "panel_member" / "*.parquet").as_posix()
    mem = f"(SELECT DISTINCT security_id FROM read_parquet('{member}') WHERE member)"
    measure = {}
    for label, where in (("all_lines", "TRUE"), ("linked_lines", "cik IS NOT NULL"),
                         ("member_lines", f"security_id IN {mem}"), ("non_index_lines", "NOT is_index_line")):
        r = con.execute(f"""
            SELECT count(*), count(*) FILTER (WHERE listing_date IS NOT NULL),
                   count(*) FILTER (WHERE listing_basis IN ('sec_confirmed', 'sec_registration_before_vendor')
                                     OR (listing_basis = 'successor_of_line' AND listing_date IS NOT NULL))
            FROM read_parquet('{d}') WHERE first_session >= DATE '{POST_2012}' AND {where}""").fetchone()
        measure[label] = {"lines_first_seen_2013_plus": r[0], "listing_date_known": r[1],
                          "share_known": round(r[1] / r[0], 4) if r[0] else None,
                          "share_sec_evidence": round(r[2] / r[0], 4) if r[0] else None}
    by_basis = con.execute(f"""SELECT listing_basis, count(*) FILTER (WHERE first_session >= DATE '{POST_2012}'),
                                      count(*) FROM read_parquet('{d}') GROUP BY 1 ORDER BY 1""").fetchall()
    return {"rows": n, "done_measure": measure,
            "basis_counts": {b: {"first_seen_2013_plus": int(a), "all": int(c)} for b, a, c in by_basis},
            "vendor_batch_dates": [str(r[0]) for r in con.execute("SELECT d FROM batch ORDER BY 1").fetchall()]}


def publish(receipt: dict[str, Any]) -> Path:
    root = C.build_root()
    wd = work_dir()
    files: dict[str, Any] = {}
    for name in ("listing_events.parquet", "line_listing.parquet", "name_history.parquet", "filer_addresses.parquet"):
        files.update(C.output_hashes(root / STAGE, name))
    inputs = {name: C.sha256_file(path) for name, path in (
        ("sec_filings_manifest_sha256", root / "sec_filings" / "manifest.json"),
        ("security_master_manifest_sha256", root / STAGE / "manifest.json"),
        ("identity_manifest_sha256", root / "identity" / "manifest.json"),
        ("identity_link_table_manifest_sha256", root / "identity" / "link_table_manifest.json"),
    ) if path.exists()}
    extract_receipt = wd / "extract_receipt.json"
    manifest = {
        "schema": SCHEMA, "status": "complete", "stage": STAGE, "rule": RULE, "rule_text": __doc__,
        "code": C.code_identity(*MODULES), "files": files, "input_manifests_sha256": inputs,
        "sources": {"submissions_zip": {**C.file_identity(SF.SUBMISSIONS_ZIP), "fetched": SF.SNAPSHOT_FETCHED.isoformat()},
                    "extract_receipt": C.read_json(extract_receipt) if extract_receipt.exists() else None,
                    "extract_parts": C.output_hashes(wd, "*.parquet")},
        "staleness": "events and listing dates are static facts; name_history carries valid_from / valid_to",
        "receipt": receipt,
    }
    path = root / STAGE / "listing_events_manifest.json"
    C.write_json_atomic(path, manifest)
    return path


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("cmd", choices=("extract", "build", "all"))
    args = ap.parse_args(argv)
    if args.cmd in ("extract", "all"):
        print(json.dumps(extract()), flush=True)
    if args.cmd in ("build", "all"):
        rec = build()
        print(json.dumps(rec, default=str, indent=1), flush=True)
        print(publish(rec), flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
