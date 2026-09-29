"""Stage ``sec_filings``: SEC filing metadata, issuer profile, filer regime, 8-K items and named events.

Source: ``data/cache/submissions.zip``, the SEC bulk submissions archive (fetched 2026-09-19), read in place.
The archive holds ~991k members, one ``CIK##########.json`` per filer (profile + ``filings.recent``) plus
``CIK##########-submissions-NNN.json`` files with older filings. The central directory is streamed entry by
entry (``iter_zip_entries``) so the ~1M-entry directory never sits in memory as ``zipfile.ZipInfo`` objects.

Phases (each resumable):

``extract``   stream the archive in chunks of ``CHUNK_ENTRIES`` members -> ``_tmp/sec_filings_raw/``
              ``filings_NNNN.parquet`` (every filing dated ``RAW_START`` .. ``FILING_END``, raw strings) and
              ``profile_NNNN.parquet`` (one row per main CIK file). A chunk whose two parts exist is skipped.
``assemble``  DuckDB over the raw parts -> ``sec_filings/`` ``filings.parquet``, ``issuer_profile.parquet``,
              ``filer_regime.parquet``, ``eight_k_items.parquet``, ``events.parquet``, then the manifest.

Clock rules (docs/ALPHA_PANEL_SEC.md):

* ``acceptanceDateTime`` carries a ``Z`` suffix but its clock differs by JSON file (``ACCEPTANCE_RULE``): some
  member files hold true UTC, others the America/New_York wall clock labelled ``Z`` (verified on EDGAR index
  pages; the same accession differs by exactly the ET offset between files). Each (cik, source) file is
  classified from three kinds of evidence (``clock_evidence_sql``): a shared accession whose two values differ by
  the ET offset (the earlier value is the ET file), a time outside EDGAR's 06:00-22:00 ET window under one
  reading only, and a filing date that fits EDGAR's 17:30 ET cutoff under one reading only. ``acceptance_utc``
  = the file's resolved reading, else the accession's consensus from resolved files, else the conservative
  (later) ET reading with ``vintage_risk = 'acceptance_clock_unresolved'``; ``acceptance_clock`` says which.
* ``available_at`` = ``acceptance_utc``; when it is missing, ``filing_date`` + 1 day at 00:00 America/New_York
  (after EDGAR's 22:00 ET close on the filing date), labelled ``available_basis``.
* Filings are immutable: an amendment is a new accession (``is_amendment``), never an overwrite.
* ``issuer_profile`` is the 2026-09-19 snapshot (``vintage_risk = 'snapshot_non_pit'``) except
  ``former_names`` which carry their own SEC dates.
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import os
import re
import struct
import sys
import zlib
from collections.abc import Iterable, Iterator
from dataclasses import dataclass
from pathlib import Path
from typing import Any, BinaryIO

import pyarrow as pa
import pyarrow.parquet as pq

from . import common as C

STAGE = "sec_filings"
SCHEMA = "atx.alpha-panel.sec-filings/v1"
SUBMISSIONS_ZIP = Path(os.environ.get("ATX_SEC_SUBMISSIONS_ZIP",
                                      str(C.PACKAGE_ROOT / "data" / "cache" / "submissions.zip")))
SNAPSHOT_FETCHED = dt.date(2026, 9, 19)
FILING_START = dt.date(2009, 1, 1)
FILING_END = dt.date(2026, 9, 19)
RAW_START = dt.date(2007, 1, 1)  # two warm-up years so filer_regime knows the state on 2009-01-01
CHUNK_ENTRIES = 25_000
FLUSH_ROWS = 150_000
DUCKDB_MEMORY = os.environ.get("ATX_SEC_DUCKDB_MEMORY", "450MB")
ACCEPTANCE_RULE = "acceptance-per-file-clock-v1"
AVAILABLE_RULE = ("available_at = acceptance_utc (acceptance-per-file-clock-v1); when missing, filing_date + 1 day "
                  "00:00 America/New_York (EDGAR closes 22:00 ET), available_basis = 'filing_date_eod_et'")
#: Filer-submitted forms that EDGAR only accepts 06:00-22:00 ET (window evidence).
WINDOW_FORMS = ("8-K", "8-K/A", "10-Q", "10-Q/A", "10-K", "10-K/A", "3", "4", "5", "3/A", "4/A", "5/A", "SC 13G",
                "SC 13G/A", "SC 13D", "SC 13D/A", "6-K", "13F-HR", "13F-HR/A", "424B2", "424B3", "424B5", "FWP",
                "497", "497K", "S-1", "S-3", "S-8", "DEF 14A", "DEFA14A", "144", "D", "D/A", "NPORT-P", "N-CSR",
                "20-F", "40-F", "10-D", "11-K", "SD", "S-4", "425")
#: Forms whose filing date moves to the next business day after 17:30 ET (cutoff evidence).
CUTOFF_FORMS = ("8-K", "8-K/A", "10-Q", "10-Q/A", "10-K", "10-K/A", "DEF 14A", "6-K", "20-F", "40-F", "10-D", "11-K")
WINDOW_MIN = (360, 1325)  # 06:00 .. 22:05 ET, minutes after midnight
CUTOFF_MIN = 1050         # 17:30 ET
CLOCK_MAJORITY = 0.8
REGIME_GRACE_DAYS = 400
#: Annual-only regimes (20-F, 40-F) get 18 months: their filing dates drift and the 20-F deadline moved.
REGIME_GRACE: dict[str, int] = {"domestic": 400, "fpi_20f": 550, "canadian_40f": 550, "fund": 400}

MEMBER_RE = re.compile(r"^CIK(\d{10})(?:-submissions-(\d{3}))?\.json$")
ITEM_RE = re.compile(r"^(\d{1,2})\.(\d{1,2})$")

# submissions JSON key -> raw column
FILING_FIELDS: tuple[tuple[str, str], ...] = (
    ("accessionNumber", "accession"),
    ("filingDate", "filing_date"),
    ("reportDate", "report_date"),
    ("acceptanceDateTime", "acceptance_raw"),
    ("act", "act"),
    ("form", "form"),
    ("fileNumber", "file_number"),
    ("items", "items_raw"),
    ("size", "size"),
    ("isXBRL", "is_xbrl"),
    ("isInlineXBRL", "is_inline_xbrl"),
    ("primaryDocument", "primary_document"),
)
INT_COLUMNS = frozenset({"size", "is_xbrl", "is_inline_xbrl"})

RAW_FILING_SCHEMA = pa.schema(
    [("cik", pa.int64()), ("source", pa.int16())]
    + [(col, pa.int64() if col in INT_COLUMNS else pa.string()) for _, col in FILING_FIELDS]
)
FORMER_NAME_TYPE = pa.list_(pa.struct([("name", pa.string()), ("from_date", pa.date32()), ("to_date", pa.date32())]))
PROFILE_SCHEMA = pa.schema([
    ("cik", pa.int64()), ("name", pa.string()), ("entity_type", pa.string()), ("sic", pa.string()),
    ("sic_description", pa.string()), ("owner_org", pa.string()), ("category", pa.string()),
    ("state_of_incorporation", pa.string()), ("state_of_incorporation_description", pa.string()),
    ("fiscal_year_end", pa.string()), ("tickers", pa.list_(pa.string())), ("exchanges", pa.list_(pa.string())),
    ("ein", pa.string()), ("lei", pa.string()), ("flags", pa.string()),
    ("insider_tx_for_issuer_exists", pa.bool_()), ("insider_tx_for_owner_exists", pa.bool_()),
    ("business_state_or_country", pa.string()), ("business_is_foreign", pa.bool_()),
    ("former_names", FORMER_NAME_TYPE), ("n_extra_files", pa.int32()), ("n_filings_total", pa.int64()),
])

# ---------------------------------------------------------------------------------------------------------
# Form classification (pure)
# ---------------------------------------------------------------------------------------------------------

REGIME_FORMS: dict[str, str] = {
    **{f: "domestic" for f in ("10-K", "10-Q", "10-KT", "10-QT", "10-K405", "10-KSB", "10-QSB", "10-KSB40",
                               "10-KT405")},
    "20-F": "fpi_20f", "20-FR12B": "fpi_20f", "20-FR12G": "fpi_20f",
    "40-F": "canadian_40f", "40FR12B": "canadian_40f", "40FR12G": "canadian_40f",
    **{f: "fund" for f in ("N-CSR", "N-CSRS", "N-1A", "N-1", "N-2", "N-3", "N-4", "N-6", "485BPOS", "485APOS",
                           "485BXT", "N-Q", "NPORT-P", "N-CEN", "N-MFP", "N-MFP1", "N-MFP2", "N-MFP3",
                           "N-SAR", "NSAR-A", "NSAR-B", "NSAR-U", "24F-2NT", "N-30D", "N-30B-2", "497", "497K")},
}

ITEM_EVENTS: dict[str, str] = {
    "2.02": "earnings_release",
    "2.01": "acquisition_completed",
    "1.01": "material_agreement",
    "1.03": "bankruptcy",
    "3.01": "delisting_notice",
    "5.01": "change_in_control",
    "4.02": "nonreliance_restatement",
    "4.01": "auditor_change",
    "8.01": "other_events",
}

FORM_EVENTS: dict[str, str] = {
    "25": "form25_delisting",
    "25-NSE": "form25nse_delisting",
    "15-12B": "form15_12b_deregistration",
    "15-12G": "form15_12g_deregistration",
    "15-15D": "form15_15d_suspension",
    "15F-12B": "form15f_12b_deregistration",
    "15F-12G": "form15f_12g_deregistration",
    "15F-15D": "form15f_15d_suspension",
    "NT 10-K": "late_filing_nt_10k",
    "NT 10-Q": "late_filing_nt_10q",
    "NT 20-F": "late_filing_nt_20f",
}


def base_form(form: str | None) -> str:
    """Form without the amendment suffix: ``8-K/A`` -> ``8-K``."""
    f = (form or "").strip().upper()
    return f[:-2] if f.endswith("/A") else f


def is_amendment(form: str | None) -> bool:
    return (form or "").strip().upper().endswith("/A")


def classify_regime(form: str | None) -> str | None:
    """Filer regime implied by one *original* periodic or fund form; None for every other form."""
    f = (form or "").strip().upper()
    return REGIME_FORMS.get(f)


def is_eight_k(form: str | None) -> bool:
    """8-K and its variants (8-K/A, 8-K12B, 8-K12G3, 8-K15D5 ...)."""
    return base_form(form).startswith("8-K")


def parse_items(raw: str | None) -> list[str]:
    """8-K item list: ``'2.02,9.01'`` -> ``['2.02', '9.01']``; ``'2.2'`` -> ``'2.02'``; order kept, deduplicated."""
    out: list[str] = []
    if not raw:
        return out
    for part in re.split(r"[,;\s]+", str(raw)):
        tok = part.strip().rstrip(".")
        if not tok:
            continue
        if tok.lower().startswith("item"):
            tok = tok[4:].strip()
        if not tok:
            continue
        m = ITEM_RE.match(tok)
        if m:
            tok = f"{int(m.group(1))}.{int(m.group(2)):02d}"
        if tok not in out:
            out.append(tok)
    return out


def event_types(form: str | None, items: Iterable[str]) -> list[tuple[str, str | None]]:
    """Named events for one filing: ``[(event_type, item or None)]``. 8-K items first, then the form event."""
    out: list[tuple[str, str | None]] = []
    if is_eight_k(form):
        for it in items:
            ev = ITEM_EVENTS.get(it)
            if ev:
                out.append((ev, it))
    ev = FORM_EVENTS.get(base_form(form))
    if ev:
        out.append((ev, None))
    return out


@dataclass(frozen=True)
class RegimeSegment:
    regime: str
    valid_from: dt.date
    valid_to: dt.date
    first_filing: dt.date | None
    last_filing: dt.date | None
    n_filings: int
    available_at: dt.datetime | None


def regime_segments(filings: list[tuple[dt.date, str, dt.datetime | None]], horizon: dt.date,
                    grace_days: int | dict[str, int] = REGIME_GRACE_DAYS,
                    start: dt.date | None = None) -> list[RegimeSegment]:
    """Contiguous regime ranges for one CIK from its dated regime-bearing filings (sorted or not).

    A segment starts at the filing date of the first filing of a regime and runs until the day before the
    first filing of a different regime, or ``grace_days`` (an int, or per regime) after its last filing, whichever
    is first; a lapse
    becomes a ``none`` segment until the next regime filing (or ``horizon``). ``available_at`` is the
    acceptance of the segment's first filing; a ``none`` lapse is known at the end of its first day
    (``None`` here, set by the caller). Segments are clipped to ``[start, horizon]``.
    """
    if not filings:
        return []
    def grace(regime: str) -> int:
        return grace_days.get(regime, REGIME_GRACE_DAYS) if isinstance(grace_days, dict) else grace_days

    rows = sorted(filings, key=lambda r: (r[0], r[2] or dt.datetime.min))
    raw: list[list[Any]] = []  # [regime, first, last, n, available_at]
    for d, regime, avail in rows:
        if raw and raw[-1][0] == regime and (d - raw[-1][2]).days <= grace(regime):
            raw[-1][2] = d
            raw[-1][3] += 1
        else:
            raw.append([regime, d, d, 1, avail])
    out: list[RegimeSegment] = []
    for i, (regime, first, last, n, avail) in enumerate(raw):
        nxt = raw[i + 1][1] if i + 1 < len(raw) else None
        end = last + dt.timedelta(days=grace(regime))
        if nxt is not None:
            end = min(end, nxt - dt.timedelta(days=1))
        end = min(end, horizon)
        out.append(RegimeSegment(regime, first, end, first, last, n, avail))
        lapse_to = (nxt - dt.timedelta(days=1)) if nxt is not None else horizon
        if end < lapse_to:
            out.append(RegimeSegment("none", end + dt.timedelta(days=1), lapse_to, None, None, 0, None))
    if start is not None:
        clipped = []
        for s in out:
            if s.valid_to < start:
                continue
            if s.valid_from < start:
                s = RegimeSegment(s.regime, start, s.valid_to, s.first_filing, s.last_filing, s.n_filings,
                                  s.available_at)
            clipped.append(s)
        out = clipped
    return out


# ---------------------------------------------------------------------------------------------------------
# Streaming zip reader (pure; zip64 aware; never materialises the central directory)
# ---------------------------------------------------------------------------------------------------------

@dataclass(frozen=True)
class ZipEntry:
    index: int
    name: str
    method: int
    crc: int
    compressed_size: int
    size: int
    header_offset: int


def _central_directory(fh: BinaryIO) -> tuple[int, int, int]:
    """(entries, cd_size, cd_offset) from the (zip64) end-of-central-directory records."""
    fh.seek(0, os.SEEK_END)
    size = fh.tell()
    tail_len = min(size, 66_000 + 20 + 56)
    fh.seek(size - tail_len)
    tail = fh.read(tail_len)
    i = tail.rfind(b"PK\x05\x06")
    if i < 0:
        raise ValueError("no end-of-central-directory record")
    _sig, _d, _cd, _nd, n, cd_size, cd_off, _cl = struct.unpack("<IHHHHIIH", tail[i:i + 22])
    j = tail.rfind(b"PK\x06\x06", 0, i)
    if j >= 0 and (n == 0xFFFF or cd_size == 0xFFFFFFFF or cd_off == 0xFFFFFFFF):
        vals = struct.unpack("<IQHHIIQQQQ", tail[j:j + 56])
        n, cd_size, cd_off = vals[7], vals[8], vals[9]
    return int(n), int(cd_size), int(cd_off)


def iter_zip_entries(fh: BinaryIO, block: int = 1 << 22) -> Iterator[ZipEntry]:
    """Yield every central-directory entry in directory order, reading ``block`` bytes at a time."""
    n, cd_size, cd_off = _central_directory(fh)
    pos_file = cd_off
    end_file = cd_off + cd_size
    buf = b""
    idx = 0
    while idx < n:
        if len(buf) < 46 + 65535 * 3 and pos_file < end_file:
            fh.seek(pos_file)
            chunk = fh.read(min(block, end_file - pos_file))
            pos_file += len(chunk)
            buf += chunk
        if len(buf) < 46:
            raise ValueError("truncated central directory")
        (sig, _vm, _vn, _flag, method, _mt, _md, crc, csize, usize, nlen, xlen, clen, _dn, _ia, _ea,
         loff) = struct.unpack("<IHHHHHHIIIHHHHHII", buf[:46])
        if sig != 0x02014B50:
            raise ValueError(f"bad central directory signature at entry {idx}")
        rec_len = 46 + nlen + xlen + clen
        if len(buf) < rec_len:
            if pos_file >= end_file:
                raise ValueError("truncated central directory entry")
            continue
        name = buf[46:46 + nlen].decode("utf-8", "replace")
        if csize == 0xFFFFFFFF or usize == 0xFFFFFFFF or loff == 0xFFFFFFFF:
            extra = buf[46 + nlen:46 + nlen + xlen]
            q = 0
            while q + 4 <= len(extra):
                hid, hl = struct.unpack("<HH", extra[q:q + 4])
                if hid == 1:
                    data = extra[q + 4:q + 4 + hl]
                    k = 0
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
        yield ZipEntry(idx, name, method, crc, csize, usize, loff)
        buf = buf[rec_len:]
        idx += 1


def read_zip_member(fh: BinaryIO, entry: ZipEntry) -> bytes:
    """Inflate one member (stored or deflate), verifying size and CRC-32."""
    fh.seek(entry.header_offset)
    head = fh.read(30)
    if head[:4] != b"PK\x03\x04":
        raise ValueError(f"{entry.name}: bad local header")
    nlen, xlen = struct.unpack("<HH", head[26:30])
    fh.seek(entry.header_offset + 30 + nlen + xlen)
    data = fh.read(entry.compressed_size)
    if entry.method == 8:
        data = zlib.decompress(data, -15)
    elif entry.method != 0:
        raise ValueError(f"{entry.name}: unsupported compression {entry.method}")
    if len(data) != entry.size or (zlib.crc32(data) & 0xFFFFFFFF) != entry.crc:
        raise ValueError(f"{entry.name}: size/CRC mismatch")
    return data


# ---------------------------------------------------------------------------------------------------------
# JSON flattening (pure)
# ---------------------------------------------------------------------------------------------------------

def member_kind(name: str) -> tuple[int, int] | None:
    """``CIK0000320193.json`` -> (320193, 0); ``CIK0000320193-submissions-001.json`` -> (320193, 1); else None."""
    m = MEMBER_RE.match(name)
    if not m:
        return None
    return int(m.group(1)), int(m.group(2) or 0)


def columnar_block(doc: dict[str, Any], source: int) -> dict[str, list[Any]]:
    """The columnar filing arrays of a main file (``filings.recent``) or an extra file (the document itself)."""
    if source == 0:
        return (doc.get("filings") or {}).get("recent") or {}
    return doc


def flatten_filings(block: dict[str, list[Any]], cik: int, source: int) -> dict[str, list[Any]]:
    """Columnar SEC arrays -> raw columns (``FILING_FIELDS`` names) of equal length, missing keys as None."""
    n = len(block.get("accessionNumber") or [])
    out: dict[str, list[Any]] = {"cik": [cik] * n, "source": [source] * n}
    for key, col in FILING_FIELDS:
        vals = block.get(key)
        if vals is None or len(vals) != n:
            vals = (list(vals) + [None] * n)[:n] if vals is not None else [None] * n
        if col in INT_COLUMNS:
            vals = [v if isinstance(v, int) and not isinstance(v, bool) else (int(v) if isinstance(v, bool) else None)
                    for v in vals]
        else:
            vals = [None if v is None else str(v) for v in vals]
        out[col] = vals
    return out


def _date10(value: Any) -> dt.date | None:
    if not value:
        return None
    try:
        return dt.date.fromisoformat(str(value)[:10])
    except ValueError:
        return None


def profile_row(doc: dict[str, Any], cik: int) -> dict[str, Any]:
    """One issuer-profile row from a main submissions document (snapshot fields; former names dated)."""
    addr = ((doc.get("addresses") or {}).get("business") or {})
    files = ((doc.get("filings") or {}).get("files") or [])
    recent = ((doc.get("filings") or {}).get("recent") or {})
    n_total = len(recent.get("accessionNumber") or []) + sum(int(f.get("filingCount") or 0) for f in files)

    def s(key: str) -> str | None:
        v = doc.get(key)
        return None if v is None or v == "" else str(v)

    def flag(key: str) -> bool | None:
        v = doc.get(key)
        return None if v is None else bool(v)

    former = []
    for fn in doc.get("formerNames") or []:
        if not isinstance(fn, dict):
            continue
        former.append({"name": fn.get("name"), "from_date": _date10(fn.get("from")), "to_date": _date10(fn.get("to"))})
    foreign = addr.get("isForeignLocation")
    return {
        "cik": cik, "name": s("name"), "entity_type": s("entityType"), "sic": s("sic"),
        "sic_description": s("sicDescription"), "owner_org": s("ownerOrg"), "category": s("category"),
        "state_of_incorporation": s("stateOfIncorporation"),
        "state_of_incorporation_description": s("stateOfIncorporationDescription"),
        "fiscal_year_end": s("fiscalYearEnd"),
        "tickers": [str(t) for t in (doc.get("tickers") or []) if t is not None],
        "exchanges": [None if e is None else str(e) for e in (doc.get("exchanges") or [])],
        "ein": s("ein"), "lei": s("lei"), "flags": s("flags"),
        "insider_tx_for_issuer_exists": flag("insiderTransactionForIssuerExists"),
        "insider_tx_for_owner_exists": flag("insiderTransactionForOwnerExists"),
        "business_state_or_country": addr.get("stateOrCountry"),
        "business_is_foreign": None if foreign is None else bool(foreign),
        "former_names": former, "n_extra_files": len(files), "n_filings_total": n_total,
    }


# ---------------------------------------------------------------------------------------------------------
# Phase 1: extract
# ---------------------------------------------------------------------------------------------------------

def raw_dir() -> Path:
    p = C.build_root() / "_tmp" / "sec_filings_raw"
    p.mkdir(parents=True, exist_ok=True)
    return p


class _FilingBuffer:
    def __init__(self, dest: Path) -> None:
        self.dest = dest
        self.tmp = dest.with_name(dest.name + ".partial")
        self.cols: dict[str, list[Any]] = {f.name: [] for f in RAW_FILING_SCHEMA}
        self.n = 0
        self.rows_written = 0
        self.writer: pq.ParquetWriter | None = None

    def add(self, cols: dict[str, list[Any]]) -> None:
        for k, v in cols.items():
            self.cols[k].extend(v)
        self.n += len(cols["cik"])
        if self.n >= FLUSH_ROWS:
            self.flush()

    def flush(self) -> None:
        if self.n == 0:
            return
        table = pa.Table.from_pydict(self.cols, schema=RAW_FILING_SCHEMA)
        fd = table.column("filing_date")
        import pyarrow.compute as pc
        keep = pc.and_(pc.greater_equal(fd, RAW_START.isoformat()), pc.less_equal(fd, FILING_END.isoformat()))
        table = table.filter(pc.fill_null(keep, False))
        if self.writer is None:
            self.writer = pq.ParquetWriter(self.tmp, RAW_FILING_SCHEMA, compression="zstd")
        if table.num_rows:
            self.writer.write_table(table, row_group_size=200_000)
        self.rows_written += table.num_rows
        self.cols = {f.name: [] for f in RAW_FILING_SCHEMA}
        self.n = 0

    def close(self) -> int:
        self.flush()
        if self.writer is None:
            self.writer = pq.ParquetWriter(self.tmp, RAW_FILING_SCHEMA, compression="zstd")
        self.writer.close()
        os.replace(self.tmp, self.dest)
        return self.rows_written


def _write_profiles(rows: list[dict[str, Any]], dest: Path) -> None:
    tmp = dest.with_name(dest.name + ".partial")
    table = pa.Table.from_pylist(rows, schema=PROFILE_SCHEMA)
    pq.write_table(table, tmp, compression="zstd")
    os.replace(tmp, dest)


def extract(zip_path: Path = SUBMISSIONS_ZIP, limit_chunks: int | None = None) -> dict[str, Any]:
    out = raw_dir()
    ledger = out / "chunks.jsonl"
    done: dict[int, dict[str, Any]] = {}
    if ledger.exists():
        for line in ledger.read_text(encoding="utf-8").splitlines():
            if line.strip():
                rec = json.loads(line)
                done[int(rec["chunk"])] = rec
    stats = {"chunks_done_before": len(done), "chunks_written": 0}
    with zip_path.open("rb") as fh:
        n_entries, _, _ = _central_directory(fh)
        n_chunks = (n_entries + CHUNK_ENTRIES - 1) // CHUNK_ENTRIES
        stats["entries"] = n_entries
        stats["chunks"] = n_chunks
        chunk = -1
        buf: _FilingBuffer | None = None
        profiles: list[dict[str, Any]] = []
        counts = {"main": 0, "extra": 0, "other": 0}
        written = 0
        for entry in iter_zip_entries(fh):
            c = entry.index // CHUNK_ENTRIES
            if c != chunk:
                if buf is not None:
                    _finish_chunk(chunk, buf, profiles, counts, ledger)
                    written += 1
                    stats["chunks_written"] += 1
                    if limit_chunks is not None and written >= limit_chunks:
                        buf = None
                        break
                chunk = c
                buf, profiles, counts = None, [], {"main": 0, "extra": 0, "other": 0}
                if c not in done:
                    buf = _FilingBuffer(out / f"filings_{c:04d}.parquet")
                    print(f"[sec_filings] extract chunk {c + 1}/{n_chunks}", flush=True)
            if buf is None:
                continue
            kind = member_kind(entry.name)
            if kind is None:
                counts["other"] += 1
                continue
            cik, source = kind
            doc = json.loads(read_zip_member(fh, entry))
            buf.add(flatten_filings(columnar_block(doc, source), cik, source))
            if source == 0:
                counts["main"] += 1
                profiles.append(profile_row(doc, cik))
            else:
                counts["extra"] += 1
        if buf is not None:
            _finish_chunk(chunk, buf, profiles, counts, ledger)
            stats["chunks_written"] += 1
    return stats


def _finish_chunk(chunk: int, buf: _FilingBuffer, profiles: list[dict[str, Any]], counts: dict[str, int],
                  ledger: Path) -> None:
    rows = buf.close()
    _write_profiles(profiles, raw_dir() / f"profile_{chunk:04d}.parquet")
    rec = {"chunk": chunk, "filing_rows": rows, "profiles": len(profiles), **counts,
           "written_at": dt.datetime.now(dt.timezone.utc).isoformat()}
    with ledger.open("a", encoding="utf-8") as fh:
        fh.write(json.dumps(rec) + "\n")


# ---------------------------------------------------------------------------------------------------------
# Phase 2: assemble
# ---------------------------------------------------------------------------------------------------------

def _zip_mtime_utc() -> str:
    """The archive's modification time in UTC (when the snapshot landed): the profile's available_at."""
    ts = dt.datetime.fromtimestamp(SUBMISSIONS_ZIP.stat().st_mtime, dt.timezone.utc)
    return ts.replace(tzinfo=None, microsecond=0).isoformat(sep=" ")


def _raw_glob(prefix: str) -> str:
    return (raw_dir() / f"{prefix}_*.parquet").as_posix()


def _sql_list(values: Iterable[str]) -> str:
    return ", ".join("'" + v.replace("'", "''") + "'" for v in values)


ACCEPT_SQL = "try_cast(replace(left(acceptance_raw, 19), 'T', ' ') AS TIMESTAMP)"
EOD_SQL = ("CAST(timezone('America/New_York', CAST(filing_date AS TIMESTAMP) + INTERVAL 1 DAY) "
           "AT TIME ZONE 'UTC' AS TIMESTAMP)")


def items_explode_sql(source: str, keys: str, where: str = "TRUE") -> str:
    """SQL form of :func:`parse_items`: one row per (``keys``, normalised item) of ``source.items``."""
    return f"""
    WITH x AS (
        SELECT {keys},
               trim(unnest(string_split(regexp_replace(regexp_replace(items, '(?i)item', '', 'g'),
                                                       '[;\\s]+', ',', 'g'), ','))) AS tok
        FROM {source} WHERE items IS NOT NULL AND ({where})
    ), y AS (
        SELECT *, regexp_extract(tok, '^(\\d{{1,2}})\\.(\\d{{1,2}})\\.?$', ['a', 'b']) AS m FROM x WHERE tok <> ''
    )
    SELECT DISTINCT {keys},
           CASE WHEN m.a <> '' THEN CAST(CAST(m.a AS INT) AS VARCHAR) || '.' || lpad(CAST(CAST(m.b AS INT) AS VARCHAR), 2, '0')
                ELSE tok END AS item
    FROM y
    """


ET_TO_UTC = "CAST(timezone('America/New_York', {x}) AT TIME ZONE 'UTC' AS TIMESTAMP)"
UTC_TO_ET = "CAST(timezone('America/New_York', CAST({x} AS TIMESTAMPTZ)) AS TIMESTAMP)"
#: EDGAR index pages checked by hand (``Accepted`` is printed in America/New_York), fetched 2026-09-28.
INDEX_PAGE_CHECKS: tuple[tuple[str, str, str], ...] = (
    ("0001140361-26-037020", "2026-09-17T22:30:24.000Z", "2026-09-17 18:30:24"),  # CIK 320193 file: true UTC
    ("0001498233-23-000080", "2023-09-21T19:43:38.000Z", "2023-09-21 19:43:38"),  # ET wall clock labelled Z
    ("0001318568-23-000166", "2023-08-08T20:44:48.000Z", "2023-08-08 20:44:48"),  # ET wall clock labelled Z
    ("0001104659-23-024828", "2023-02-24T15:56:21.000Z", "2023-02-24 10:56:21"),  # true UTC
)


def file_clock(n_et: int, n_utc: int, majority: float = CLOCK_MAJORITY) -> str:
    """A member file's clock from its evidence counts: 'et', 'utc', 'conflict' or 'unresolved'."""
    n = n_et + n_utc
    if n == 0:
        return "unresolved"
    if n_et >= majority * n:
        return "et"
    if n_utc >= majority * n:
        return "utc"
    return "conflict"


def clock_evidence_sql(raw: str) -> str:
    """Per (cik, source) member file: ET-file and UTC-file evidence counts over table ``raw``
    (columns cik, source, acc_key = integer accession key, form, filing_date, ts = raw value, tu = ts read as UTC
    shown in ET). A shared accession counts only when its extreme values differ by exactly the ET offset."""
    wf, cf = _sql_list(WINDOW_FORMS), _sql_list(CUTOFF_FORMS)
    lo, hi = WINDOW_MIN
    c = CUTOFF_MIN
    return f"""
    WITH m AS (
        SELECT acc_key, min(ts) AS lo, max(ts) AS hi FROM {raw} WHERE ts IS NOT NULL
        GROUP BY 1 HAVING min(ts) <> max(ts)
    ), m2 AS (SELECT acc_key, lo, hi FROM m WHERE hi = {ET_TO_UTC.format(x='lo')}),
    pair AS (
        SELECT r.cik, r.source, count(*) FILTER (WHERE r.ts = m2.lo) AS p_et,
               count(*) FILTER (WHERE r.ts = m2.hi) AS p_utc
        FROM {raw} r JOIN m2 USING (acc_key) GROUP BY 1, 2
    ), x AS (
        SELECT cik, source, form, filing_date AS fd, hour(tu) * 60 + minute(tu) AS mu,
               hour(ts) * 60 + minute(ts) AS me, CAST(tu AS DATE) AS du, CAST(ts AS DATE) AS de
        FROM {raw} WHERE ts IS NOT NULL
    ), rowev AS (
        SELECT cik, source, count(*) AS n_rows,
          count(*) FILTER (WHERE form IN ({wf}) AND (mu < {lo} OR mu > {hi}) AND me BETWEEN {lo} AND {hi}) AS w_et,
          count(*) FILTER (WHERE form IN ({wf}) AND (me < {lo} OR me > {hi}) AND mu BETWEEN {lo} AND {hi}) AS w_utc,
          count(*) FILTER (WHERE form IN ({cf}) AND mu < {c} AND fd > du AND me >= {c} AND fd > de) AS c_et,
          count(*) FILTER (WHERE form IN ({cf}) AND me >= {c} AND me <= {hi} AND fd = de AND mu < {c} AND fd = du)
              AS c_utc
        FROM x GROUP BY 1, 2
    )
    SELECT r.cik, r.source, r.n_rows, coalesce(p.p_et, 0) AS p_et, coalesce(p.p_utc, 0) AS p_utc,
           r.w_et, r.w_utc, r.c_et, r.c_utc,
           coalesce(p.p_et, 0) + r.w_et + r.c_et AS n_et, coalesce(p.p_utc, 0) + r.w_utc + r.c_utc AS n_utc
    FROM rowev r LEFT JOIN pair p USING (cik, source)
    """


def clock_decision_sql(evidence: str) -> str:
    """SQL twin of :func:`file_clock` over a ``clock_evidence_sql`` relation."""
    m = CLOCK_MAJORITY
    return f"""
    SELECT *, CASE WHEN n_et + n_utc = 0 THEN 'unresolved'
                   WHEN n_et >= {m} * (n_et + n_utc) THEN 'et'
                   WHEN n_utc >= {m} * (n_et + n_utc) THEN 'utc'
                   ELSE 'conflict' END AS clock
    FROM {evidence}
    """


ACC_KEY_SQL = ("coalesce(try_cast(replace(accession, '-', '') AS BIGINT), "
               "-1 - CAST(hash(accession) % 9000000000000000000 AS BIGINT))")


def resolve_filings(con, raw: str, files: str, dest: str) -> None:
    """Create table ``dest``: one row per (cik, accession) with the resolved ``acceptance_utc``,
    ``acceptance_clock`` and ``vintage_risk``. Stepwise so every heavy operator is a spillable aggregate or join."""
    et = ET_TO_UTC.format(x="r.ts")
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE _rr AS
        SELECT r.*, f.clock, CASE f.clock WHEN 'utc' THEN r.ts WHEN 'et' THEN {et} END AS acc_file
        FROM {raw} r LEFT JOIN {files} f USING (cik, source)""")
    con.execute("""
        CREATE OR REPLACE TEMP TABLE _cons AS
        WITH need AS (SELECT DISTINCT acc_key FROM _rr WHERE acc_file IS NULL AND ts IS NOT NULL)
        SELECT acc_key, min(acc_file) AS acc_cons FROM _rr
        WHERE acc_file IS NOT NULL AND acc_key IN (SELECT acc_key FROM need) GROUP BY 1""")
    con.execute("""
        CREATE OR REPLACE TEMP TABLE _disagree AS
        SELECT acc_key FROM _rr WHERE acc_file IS NOT NULL GROUP BY 1 HAVING min(acc_file) <> max(acc_file)""")
    con.execute("""
        CREATE OR REPLACE TEMP TABLE _dups AS
        SELECT cik, acc_key FROM _rr GROUP BY 1, 2 HAVING count(*) > 1""")
    body = f"""
        SELECT r.cik, r.accession, r.form, r.filing_date,
               CASE WHEN r.ts IS NULL THEN NULL WHEN r.acc_file IS NOT NULL THEN r.acc_file
                    WHEN c.acc_cons IS NOT NULL THEN c.acc_cons ELSE {et} END AS acceptance_utc,
               CASE WHEN r.ts IS NULL THEN NULL WHEN r.acc_file IS NOT NULL THEN 'file_' || r.clock
                    WHEN c.acc_cons IS NOT NULL THEN 'accession_consensus' ELSE 'unresolved_conservative' END
                   AS acceptance_clock,
               CASE WHEN r.ts IS NOT NULL AND r.acc_file IS NULL AND c.acc_cons IS NULL THEN 'acceptance_clock_unresolved'
                    WHEN d.acc_key IS NOT NULL THEN 'acceptance_clock_disagreement' END AS vintage_risk,
               r.acceptance_raw, r.report_date, r.items, r.is_amendment, r.primary_document, r.file_number, r.act,
               r.is_xbrl, r.is_inline_xbrl, r.size, r.source, r.acc_key
        FROM _rr r LEFT JOIN _cons c USING (acc_key) LEFT JOIN _disagree d USING (acc_key)"""
    con.execute(f"""
        CREATE TABLE {dest} AS
        SELECT b.* EXCLUDE (source, acc_key) FROM ({body}) b ANTI JOIN _dups u ON u.cik = b.cik AND u.acc_key = b.acc_key
    """)
    con.execute(f"""
        INSERT INTO {dest}
        SELECT * EXCLUDE (source, acc_key, rn) FROM (
            SELECT b.*, row_number() OVER (PARTITION BY b.cik, b.acc_key ORDER BY b.source) AS rn
            FROM ({body}) b SEMI JOIN _dups u ON u.cik = b.cik AND u.acc_key = b.acc_key)
        WHERE rn = 1""")
    for t in ("_rr", "_cons", "_disagree", "_dups"):
        con.execute(f"DROP TABLE {t}")


def clock_audit(con) -> dict[str, Any]:
    """Per-file clock resolution summary and post-resolution checks against EDGAR's hours and cutoff."""
    out: dict[str, Any] = {"rule": ACCEPTANCE_RULE}
    out["files_by_clock"] = {k: {"files": int(n), "rows": int(r)} for k, n, r in con.execute(
        "SELECT clock, count(*), sum(n_rows) FROM clock_files GROUP BY 1 ORDER BY 1").fetchall()}
    out["rows_by_acceptance_clock"] = {str(k): int(n) for k, n in con.execute(
        f"SELECT acceptance_clock, count(*) FROM filings WHERE filing_date >= DATE '{FILING_START}' GROUP BY 1 "
        "ORDER BY 1").fetchall()}
    out["accessions_with_disagreeing_resolved_files"] = int(con.execute(
        "SELECT count(DISTINCT accession) FROM filings WHERE vintage_risk = 'acceptance_clock_disagreement'"
    ).fetchone()[0])
    wf, cf = _sql_list(WINDOW_FORMS), _sql_list(CUTOFF_FORMS)
    lo, hi = WINDOW_MIN
    rows = con.execute(f"""
        WITH x AS (
            SELECT year(filing_date) AS y, form, filing_date AS fd, acceptance_clock,
                   {UTC_TO_ET.format(x='acceptance_utc')} AS et
            FROM filings WHERE acceptance_utc IS NOT NULL AND filing_date >= DATE '{FILING_START}'
        ), k AS (SELECT *, hour(et) * 60 + minute(et) AS mm, CAST(et AS DATE) AS de FROM x)
        SELECT y, count(*) FILTER (WHERE form IN ({wf})) AS n_w,
               avg(CASE WHEN mm BETWEEN {lo} AND {hi} THEN 1.0 ELSE 0.0 END) FILTER (WHERE form IN ({wf})) AS in_window,
               count(*) FILTER (WHERE form IN ({cf})) AS n_c,
               avg(CASE WHEN (mm < {CUTOFF_MIN} AND fd = de) OR (mm >= {CUTOFF_MIN} AND fd > de) THEN 1.0 ELSE 0.0 END)
                   FILTER (WHERE form IN ({cf})) AS cutoff_consistent,
               avg(CASE WHEN acceptance_clock = 'unresolved_conservative' THEN 1.0 ELSE 0.0 END) AS unresolved_share
        FROM k GROUP BY 1 ORDER BY 1""").fetchall()
    out["per_year"] = {str(y): {"window_forms": int(nw), "in_edgar_window": round(float(iw), 5),
                                "cutoff_forms": int(nc), "cutoff_consistent": round(float(cc), 5),
                                "unresolved_share": round(float(us), 5)} for y, nw, iw, nc, cc, us in rows}
    checks = []
    for acc, raw, idx_et in INDEX_PAGE_CHECKS:
        got = con.execute(f"SELECT DISTINCT acceptance_utc, acceptance_clock FROM filings WHERE accession = '{acc}'"
                          ).fetchall()
        want = con.execute(f"SELECT {ET_TO_UTC.format(x=chr(39) + idx_et + chr(39) + '::TIMESTAMP')}").fetchone()[0]
        checks.append({"accession": acc, "raw": raw, "index_accepted_et": idx_et, "expected_utc": str(want),
                       "resolved": [[str(a), c] for a, c in got], "match": bool(got) and all(a == want for a, _ in got)})
    out["index_page_checks"] = checks
    return out


def assemble() -> dict[str, Any]:
    stage = C.stage_dir(STAGE)
    con = C.connect(memory=DUCKDB_MEMORY, db_file="sec_filings.duckdb")
    receipt: dict[str, Any] = {}
    g = _raw_glob("filings")
    ev_items = _sql_list(ITEM_EVENTS)
    # Filings: per-file clock resolution, then one row per (cik, accession) (the main file wins over extras).
    with C.timed(receipt, "filings"):
        con.execute(f"""
        CREATE TABLE raw AS
        SELECT cik, source, accession, nullif(form, '') AS form, try_cast(filing_date AS DATE) AS filing_date,
               {ACC_KEY_SQL} AS acc_key,
               {ACCEPT_SQL} AS ts, {UTC_TO_ET.format(x=ACCEPT_SQL)} AS tu, acceptance_raw,
               try_cast(nullif(report_date, '') AS DATE) AS report_date, nullif(items_raw, '') AS items,
               upper(coalesce(form, '')) LIKE '%/A' AS is_amendment, nullif(primary_document, '') AS primary_document,
               nullif(file_number, '') AS file_number, nullif(act, '') AS act, is_xbrl = 1 AS is_xbrl,
               is_inline_xbrl = 1 AS is_inline_xbrl, size
        FROM read_parquet('{g}')""")
        receipt["raw_rows"] = con.execute("SELECT count(*) FROM raw").fetchone()[0]
        con.execute("CREATE TABLE clock_files AS " + clock_decision_sql("(" + clock_evidence_sql("raw") + ")"))
        resolve_filings(con, "raw", "clock_files", "filings0")
        con.execute("DROP TABLE raw")
        con.execute(f"""
        CREATE TABLE filings AS
        SELECT *, coalesce(acceptance_utc, {EOD_SQL}) AS available_at,
               CASE WHEN acceptance_utc IS NULL THEN 'filing_date_eod_et' ELSE 'acceptance' END AS available_basis
        FROM filings0""")
        con.execute("DROP TABLE filings0")
        receipt["dedup_rows_2007"] = con.execute("SELECT count(*) FROM filings").fetchone()[0]
        C.copy_to_parquet(con, "SELECT * FROM clock_files ORDER BY cik, source", stage / "clock_files.parquet")
    with C.timed(receipt, "clock_audit"):
        receipt["clock_audit"] = clock_audit(con)
    with C.timed(receipt, "filings_write"):
        n = C.copy_to_parquet(con, f"""
            SELECT * FROM filings WHERE filing_date BETWEEN DATE '{FILING_START}' AND DATE '{FILING_END}'
            ORDER BY filing_date, cik""", stage / "filings.parquet")
        receipt.setdefault("rows", {})["filings"] = n
    # 8-K items (python-free: DuckDB splits the SEC item string; normalisation checked against parse_items).
    with C.timed(receipt, "eight_k_items"):
        con.execute("CREATE TABLE items AS " + items_explode_sql(
            "filings",
            "cik, accession, form, filing_date, acceptance_utc, available_at, report_date, is_amendment, "
            "acceptance_clock, vintage_risk",
            f"filing_date BETWEEN DATE '{FILING_START}' AND DATE '{FILING_END}' "
            "AND upper(regexp_replace(form, '/A$', '')) LIKE '8-K%'"))
        n = C.copy_to_parquet(con, "SELECT * FROM items ORDER BY filing_date, cik, accession, item",
                              stage / "eight_k_items.parquet")
        receipt["rows"]["eight_k_items"] = n
    # Named events.
    with C.timed(receipt, "events"):
        item_case = " ".join(f"WHEN '{k}' THEN '{v}'" for k, v in ITEM_EVENTS.items())
        form_case = " ".join(f"WHEN '{k}' THEN '{v}'" for k, v in FORM_EVENTS.items())
        forms = _sql_list(FORM_EVENTS)
        n = C.copy_to_parquet(con, f"""
            SELECT cik, accession, form, CASE item {item_case} END AS event_type, item, 'eight_k_item' AS source,
                   report_date AS event_date, acceptance_utc AS event_utc, available_at, filing_date, is_amendment,
                   acceptance_clock, vintage_risk
            FROM items WHERE item IN ({ev_items})
            UNION ALL
            SELECT cik, accession, form, CASE upper(regexp_replace(form, '/A$', '')) {form_case} END AS event_type,
                   NULL AS item, 'form' AS source, report_date AS event_date, acceptance_utc AS event_utc,
                   available_at, filing_date, is_amendment, acceptance_clock, vintage_risk
            FROM filings
            WHERE filing_date BETWEEN DATE '{FILING_START}' AND DATE '{FILING_END}'
              AND upper(regexp_replace(form, '/A$', '')) IN ({forms})
            ORDER BY filing_date, cik, accession""", stage / "events.parquet")
        receipt["rows"]["events"] = n
    # Issuer profile.
    with C.timed(receipt, "issuer_profile"):
        pg = _raw_glob("profile")
        n = C.copy_to_parquet(con, f"""
            SELECT * EXCLUDE (rn), 'snapshot_non_pit' AS vintage_risk,
                   DATE '{SNAPSHOT_FETCHED}' AS snapshot_date,
                   TIMESTAMP '{_zip_mtime_utc()}' AS available_at
            FROM (SELECT *, row_number() OVER (PARTITION BY cik ORDER BY n_filings_total DESC) AS rn
                  FROM read_parquet('{pg}'))
            WHERE rn = 1 ORDER BY cik""", stage / "issuer_profile.parquet")
        receipt["rows"]["issuer_profile"] = n
    with C.timed(receipt, "filer_regime"):
        receipt["rows"]["filer_regime"] = _write_regimes(con, stage / "filer_regime.parquet")
    with C.timed(receipt, "delisting_causes"):
        dl = write_delisting(con, stage)
        receipt["rows"]["delisting_causes"] = dl["rows"]
        receipt["delisting_causes_per_year"] = dl["per_year"]
    with C.timed(receipt, "coverage"):
        receipt["coverage"] = _coverage(con, stage)
    con.close()
    return receipt


DELIST_WINDOWS = {"merger": (-120, 90), "bankruptcy": (-730, 90), "deficiency": (-400, 30), "deregistration": (-30, 120),
                  "still_reporting_domestic": (30, 200), "still_reporting_foreign": (30, 400)}
EXCHANGE_FILER_MIN = 20
DELISTING_RULE = (
    "one row per original Form 25 / 25-NSE (cik, accession) of a subject CIK: rows under an exchange's own CIK "
    f"(a CIK that self-files >= {EXCHANGE_FILER_MIN} 25-NSEs: accession prefix = CIK) are dropped. Cause, first "
    "match in order: bankruptcy = an 8-K item 1.03 of the CIK filed -730..+90 days around the Form 25; "
    "merger_or_acquisition = item 2.01 or 5.01 -120..+90 days; exchange_deficiency = a 25-NSE (exchange-filed) with an item "
    "3.01 -400..+30 days; still_reporting = a 10-K/10-Q filed 30..200 days after, or a 20-F/40-F/6-K 30..400 days "
    "after (class, debt or preferred delisting or an exchange transfer; the issuer kept reporting); fund_or_trust = "
    "the CIK's filer_regime is 'fund' on the filing date; voluntary_deregistration = a Form 15 filed -30..+120 days; "
    "else unknown. cause_available_at = the latest acceptance among the Form 25 and the evidence used")


def delisting_causes_sql(events: str, filings: str, regimes: str) -> str:
    w = DELIST_WINDOWS

    def win(kind: str) -> str:
        return f"BETWEEN d.filing_date + {w[kind][0]} AND d.filing_date + {w[kind][1]}"

    return f"""
    WITH xch AS (
        SELECT cik FROM {events}
        WHERE event_type = 'form25nse_delisting' AND try_cast(left(accession, 10) AS BIGINT) = cik
        GROUP BY 1 HAVING count(*) >= {EXCHANGE_FILER_MIN}
    ), d AS (
        SELECT cik, accession, form, filing_date, available_at FROM {events}
        WHERE event_type IN ('form25_delisting', 'form25nse_delisting') AND NOT is_amendment
          AND cik NOT IN (SELECT cik FROM xch)
    ), e AS (SELECT cik, event_type, filing_date, available_at FROM {events}),
    ev AS (
        SELECT d.cik, d.accession,
          min(e.filing_date) FILTER (WHERE e.event_type IN ('acquisition_completed', 'change_in_control')
              AND e.filing_date {win('merger')}) AS merger_date,
          max(e.available_at) FILTER (WHERE e.event_type IN ('acquisition_completed', 'change_in_control')
              AND e.filing_date {win('merger')}) AS merger_av,
          min(e.filing_date) FILTER (WHERE e.event_type = 'bankruptcy' AND e.filing_date {win('bankruptcy')}) AS bk_date,
          max(e.available_at) FILTER (WHERE e.event_type = 'bankruptcy' AND e.filing_date {win('bankruptcy')}) AS bk_av,
          min(e.filing_date) FILTER (WHERE e.event_type = 'delisting_notice' AND e.filing_date {win('deficiency')})
              AS def_date,
          max(e.available_at) FILTER (WHERE e.event_type = 'delisting_notice' AND e.filing_date {win('deficiency')})
              AS def_av,
          min(e.filing_date) FILTER (WHERE e.event_type LIKE 'form15%' AND e.filing_date {win('deregistration')})
              AS f15_date,
          max(e.available_at) FILTER (WHERE e.event_type LIKE 'form15%' AND e.filing_date {win('deregistration')})
              AS f15_av
        FROM d LEFT JOIN e ON e.cik = d.cik GROUP BY 1, 2
    ), pr AS (
        SELECT d.cik, d.accession, min(p.filing_date) AS next_periodic,
               arg_min(p.available_at, p.filing_date) AS next_periodic_av
        FROM d JOIN {filings} p ON p.cik = d.cik
         AND ((p.form IN ('10-K', '10-Q', '10-KT', '10-QT') AND p.filing_date {win('still_reporting_domestic')})
           OR (p.form IN ('20-F', '40-F', '6-K') AND p.filing_date {win('still_reporting_foreign')}))
        GROUP BY 1, 2
    ), rg AS (
        SELECT d.cik, d.accession, arg_max(r.regime, r.valid_from) AS regime, max(r.available_at) AS regime_av
        FROM d JOIN {regimes} r ON r.cik = d.cik AND d.filing_date BETWEEN r.valid_from AND r.valid_to
        GROUP BY 1, 2
    )
    SELECT d.cik, d.accession, d.form, d.filing_date, d.available_at, rg.regime AS filer_regime,
           CASE WHEN ev.bk_date IS NOT NULL THEN 'bankruptcy'
                WHEN ev.merger_date IS NOT NULL THEN 'merger_or_acquisition'
                WHEN d.form LIKE '25-NSE%' AND ev.def_date IS NOT NULL THEN 'exchange_deficiency'
                WHEN pr.next_periodic IS NOT NULL THEN 'still_reporting'
                WHEN rg.regime = 'fund' THEN 'fund_or_trust'
                WHEN ev.f15_date IS NOT NULL THEN 'voluntary_deregistration'
                ELSE 'unknown' END AS cause,
           ev.merger_date, ev.bk_date AS bankruptcy_date, ev.def_date AS deficiency_notice_date,
           ev.f15_date AS form15_date, pr.next_periodic AS next_periodic_filing,
           greatest(d.available_at,
                    CASE WHEN ev.bk_date IS NOT NULL THEN ev.bk_av
                         WHEN ev.merger_date IS NOT NULL THEN ev.merger_av
                         WHEN d.form LIKE '25-NSE%' AND ev.def_date IS NOT NULL THEN ev.def_av
                         WHEN pr.next_periodic IS NOT NULL THEN pr.next_periodic_av
                         WHEN rg.regime = 'fund' THEN rg.regime_av
                         WHEN ev.f15_date IS NOT NULL THEN ev.f15_av END) AS cause_available_at
    FROM d LEFT JOIN ev USING (cik, accession) LEFT JOIN pr USING (cik, accession) LEFT JOIN rg USING (cik, accession)
    """


def write_delisting(con, stage: Path) -> dict[str, Any]:
    """``delisting_causes.parquet`` from the stage's published events, filings and filer_regime files."""
    rel = {k: f"read_parquet('{(stage / f'{k}.parquet').as_posix()}')" for k in ("events", "filings", "filer_regime")}
    n = C.copy_to_parquet(con, f"""
        SELECT * FROM ({delisting_causes_sql(rel['events'], rel['filings'], rel['filer_regime'])})
        ORDER BY filing_date, cik, accession""", stage / "delisting_causes.parquet")
    per_year = {str(y): {c: int(k) for c, k in zip(cs, ks)} for y, cs, ks in con.execute(f"""
        SELECT y, list(cause ORDER BY cause), list(n ORDER BY cause) FROM (
            SELECT year(filing_date) AS y, cause, count(*) AS n
            FROM read_parquet('{(stage / 'delisting_causes.parquet').as_posix()}') GROUP BY 1, 2)
        GROUP BY 1 ORDER BY 1""").fetchall()}
    return {"rows": n, "per_year": per_year}


REGIME_SCHEMA = pa.schema([
    ("cik", pa.int64()), ("regime", pa.string()), ("valid_from", pa.date32()), ("valid_to", pa.date32()),
    ("first_filing", pa.date32()), ("last_filing", pa.date32()), ("n_filings", pa.int32()),
    ("available_at", pa.timestamp("us")),
])


def _write_regimes(con, dest: Path) -> int:
    regime_case = " ".join(f"WHEN '{k}' THEN '{v}'" for k, v in REGIME_FORMS.items())
    forms = _sql_list(REGIME_FORMS)
    reader = con.execute(f"""
        SELECT cik, filing_date, CASE upper(form) {regime_case} END AS regime, available_at
        FROM filings WHERE upper(form) IN ({forms})
        ORDER BY cik, filing_date, available_at""").fetch_record_batch(200_000)
    tmp = dest.with_name(dest.name + ".partial")
    writer = pq.ParquetWriter(tmp, REGIME_SCHEMA, compression="zstd")
    total = 0
    pending: dict[str, list[Any]] = {f.name: [] for f in REGIME_SCHEMA}
    cur: int | None = None
    rows: list[tuple[dt.date, str, dt.datetime | None]] = []

    def emit(cik: int, rows_: list[tuple[dt.date, str, dt.datetime | None]]) -> None:
        for s in regime_segments(rows_, FILING_END, REGIME_GRACE, FILING_START):
            avail = s.available_at
            if s.regime == "none":
                avail = dt.datetime.combine(s.valid_from + dt.timedelta(days=1), dt.time(4, 0))
            pending["cik"].append(cik)
            pending["regime"].append(s.regime)
            pending["valid_from"].append(s.valid_from)
            pending["valid_to"].append(s.valid_to)
            pending["first_filing"].append(s.first_filing)
            pending["last_filing"].append(s.last_filing)
            pending["n_filings"].append(s.n_filings)
            pending["available_at"].append(avail)

    def flush() -> int:
        if not pending["cik"]:
            return 0
        t = pa.Table.from_pydict(pending, schema=REGIME_SCHEMA)
        writer.write_table(t)
        for v in pending.values():
            v.clear()
        return t.num_rows

    for batch in reader:
        d = batch.to_pydict()
        for cik, fd, regime, avail in zip(d["cik"], d["filing_date"], d["regime"], d["available_at"]):
            if cik != cur:
                if cur is not None:
                    emit(cur, rows)
                cur, rows = cik, []
            rows.append((fd, regime, avail))
        if len(pending["cik"]) > 200_000:
            total += flush()
    if cur is not None:
        emit(cur, rows)
    total += flush()
    writer.close()
    os.replace(tmp, dest)
    return total


def _coverage(con, stage: Path) -> dict[str, Any]:
    f = (stage / "filings.parquet").as_posix()
    e = (stage / "events.parquet").as_posix()
    per_year = con.execute(f"""
        SELECT year(filing_date) AS y, count(*) AS filings, count(DISTINCT cik) AS ciks,
               count(*) FILTER (WHERE upper(regexp_replace(form, '/A$', '')) LIKE '8-K%') AS eight_k,
               count(*) FILTER (WHERE upper(form) IN ('10-K','10-Q')) AS ten_k_q,
               count(*) FILTER (WHERE available_basis <> 'acceptance') AS no_acceptance
        FROM read_parquet('{f}') GROUP BY 1 ORDER BY 1""").fetchall()
    ev = con.execute(f"""
        SELECT event_type, year(filing_date) AS y, count(*) FROM read_parquet('{e}') GROUP BY 1, 2 ORDER BY 1, 2
    """).fetchall()
    ev_table: dict[str, dict[str, int]] = {}
    for et, y, n in ev:
        ev_table.setdefault(et, {})[str(y)] = int(n)
    return {
        "filings_per_year": {str(y): {"filings": int(a), "ciks": int(b), "eight_k": int(c), "ten_k_q": int(d),
                                      "no_acceptance": int(x)} for y, a, b, c, d, x in per_year},
        "events_per_year": ev_table,
    }


def _regime_rule() -> str:
    return (f"original periodic/fund forms -> regime {sorted(set(REGIME_FORMS.values()))}; a segment "
                             f"runs from its first filing until the day before a different regime's filing or "
                             f"{REGIME_GRACE} days (per regime) after its last filing; lapses are 'none'; filings from "
                             f"{RAW_START} seed the state, segments clipped to {FILING_START}..{FILING_END}; "
                             "available_at = first filing's acceptance (none: the lapse day + 1, 04:00 UTC)")


def publish(receipt: dict[str, Any]) -> Path:
    zip_sha = C.sha256_file(SUBMISSIONS_ZIP)
    payload = {
        "rules": {
            "acceptance": ACCEPTANCE_RULE + ": acceptanceDateTime's 'Z' suffix is literal UTC (clock_audit)",
            "available_at": AVAILABLE_RULE,
            "window": f"filing_date {FILING_START}..{FILING_END}",
            "dedup": "one row per (cik, accession); the main CIK file wins over its extra files",
            "eight_k_items": "one row per (cik, accession, item) for 8-K* forms (co-registrant 8-Ks repeat per CIK); "
                             "items normalised N.NN",
            "events": {"items": ITEM_EVENTS, "forms": FORM_EVENTS,
                       "event_utc": "acceptance_utc of the filing; event_date = SEC reportDate (date of the event)"},
            "filer_regime": _regime_rule(),

            "delisting_causes": DELISTING_RULE,
            "issuer_profile": "2026-09-19 snapshot of name/SIC/category/tickers/exchanges: NOT point in time "
                              "(vintage_risk = snapshot_non_pit); former_names carry SEC from/to dates",
        },
        "staleness": "event data, no staleness (filer_regime carries explicit valid_from/valid_to)",
        "sources": {"submissions_zip": {**C.file_identity(SUBMISSIONS_ZIP), "sha256": zip_sha,
                                        "fetched": SNAPSHOT_FETCHED.isoformat()}},
        **receipt,
    }
    return C.write_stage_manifest(STAGE, SCHEMA, ("common", "sec_filings"), payload)


INDEX_ACCEPTED_RE = re.compile(r'Accepted</div>\s*<div class="info">([^<]+)</div>')


def verify_index_pages(per_class: int = 10, seed: int = 20260928, scope: str = "2.02") -> dict[str, Any]:
    """Sample 8-K item 2.02 accessions per ``acceptance_clock`` class, fetch their EDGAR index pages (``Accepted``
    is printed in America/New_York) and compare with ``acceptance_utc``. Network: approved SEC agent, host limiter."""
    from .. import sec_http

    stage = C.stage_dir(STAGE)
    con = C.connect(memory="300MB")
    items = (stage / "eight_k_items.parquet").as_posix()
    scope_sql = (f"f.accession IN (SELECT accession FROM read_parquet('{items}') WHERE item = '2.02')"
                 if scope == "2.02" else "TRUE")
    rows = con.execute(f"""
        SELECT acceptance_clock, cik, accession, acceptance_utc, acceptance_raw FROM (
            SELECT f.*, row_number() OVER (PARTITION BY f.acceptance_clock ORDER BY hash(f.accession || '{seed}')) AS k
            FROM read_parquet('{(stage / 'filings.parquet').as_posix()}') f
            WHERE {scope_sql}
              AND f.acceptance_clock IS NOT NULL AND f.filing_date >= DATE '2012-01-01')
        WHERE k <= {per_class} ORDER BY 1, 3""").fetchall()
    session = sec_http.sec_session()
    checks = []
    for clock, cik, acc, acc_utc, raw in rows:
        url = f"https://www.sec.gov/Archives/edgar/data/{cik}/{acc.replace('-', '')}/{acc}-index.htm"
        resp = session.get(url, timeout=60)
        m = INDEX_ACCEPTED_RE.search(resp.text) if resp.status_code == 200 else None
        idx_et = m.group(1).strip() if m else None
        want = None
        if idx_et:
            want = con.execute(f"SELECT {ET_TO_UTC.format(x=chr(39) + idx_et + chr(39) + '::TIMESTAMP')}").fetchone()[0]
        checks.append({"acceptance_clock": clock, "cik": cik, "accession": acc, "raw": raw,
                       "index_accepted_et": idx_et, "index_utc": str(want) if want else None,
                       "ours_utc": str(acc_utc), "http_status": resp.status_code,
                       "match": want is not None and want == acc_utc,
                       "ours_minus_index_min": None if want is None else int((acc_utc - want).total_seconds() // 60)})
    con.close()
    summary: dict[str, Any] = {}
    for c in checks:
        s = summary.setdefault(c["acceptance_clock"], {"n": 0, "match": 0, "ours_later": 0, "ours_earlier": 0,
                                                       "no_page": 0})
        s["n"] += 1
        if c["ours_minus_index_min"] is None:
            s["no_page"] += 1
        elif c["match"]:
            s["match"] += 1
        elif c["ours_minus_index_min"] > 0:
            s["ours_later"] += 1
        else:
            s["ours_earlier"] += 1
    return {"fetched_at": dt.datetime.now(dt.timezone.utc).isoformat(), "per_class": per_class, "scope": scope,
            "summary": summary, "checks": checks}


def republish(extra: dict[str, Any]) -> Path:
    """Rewrite the stage manifest with its previous payload plus ``extra`` (fresh file hashes and code identity)."""
    man = C.stage_dir(STAGE) / "manifest.json"
    prev = C.read_json(man)
    payload = {k: v for k, v in prev.items() if k not in ("schema", "status", "stage", "code", "files")}
    payload.update(extra)
    return C.write_stage_manifest(STAGE, SCHEMA, ("common", "sec_filings"), payload)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--phase", choices=("extract", "assemble", "derived", "verify", "all"), default="all")
    ap.add_argument("--limit-chunks", type=int, default=None, help="extract at most N new chunks (testing)")
    ap.add_argument("--per-class", type=int, default=10, help="verify: index pages per acceptance_clock class")
    ap.add_argument("--scope", choices=("2.02", "all"), default="2.02", help="verify: 8-K item 2.02 or any form")
    args = ap.parse_args(argv)
    if args.phase in ("extract", "all"):
        print(json.dumps(extract(limit_chunks=args.limit_chunks), default=str), flush=True)
    if args.phase in ("assemble", "all"):
        receipt = assemble()
        print(json.dumps(receipt.get("rows"), default=str), flush=True)
        print(json.dumps(receipt.get("clock_audit"), default=str), flush=True)
        print(publish(receipt), flush=True)
    if args.phase == "derived":  # rebuild filer_regime and delisting_causes from the published stage files
        stage = C.stage_dir(STAGE)
        con = C.connect(memory=DUCKDB_MEMORY)
        con.execute(f"CREATE VIEW filings AS SELECT * FROM read_parquet('{(stage / 'filings.parquet').as_posix()}')")
        n_reg = _write_regimes(con, stage / "filer_regime.parquet")
        dl = write_delisting(con, stage)
        con.close()
        prev = C.read_json(stage / "manifest.json")
        rows = {**prev.get("rows", {}), "delisting_causes": dl["rows"], "filer_regime": n_reg}
        rules = {**prev.get("rules", {}), "delisting_causes": DELISTING_RULE, "filer_regime": _regime_rule()}
        print(json.dumps(dl["per_year"]), flush=True)
        print(republish({"rows": rows, "rules": rules, "delisting_causes_per_year": dl["per_year"]}), flush=True)
    if args.phase in ("verify", "all"):
        ver = verify_index_pages(args.per_class, scope=args.scope)
        print(json.dumps(ver["summary"]), flush=True)
        key = "index_page_verification" + ("" if args.scope == "2.02" else "_all_forms")
        print(republish({key: ver}), flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
