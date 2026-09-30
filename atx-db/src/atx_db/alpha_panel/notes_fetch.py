"""Stage ``notes``: SEC Financial Statement and Notes data sets -> extracted raw subsets (S2.1 + S4.4 landing).

Source: every ``*_notes.zip`` linked from the SEC (DERA) page
``https://www.sec.gov/data-research/sec-markets-data/financial-statement-notes-data-sets``: quarterly data
sets ``2009q1`` .. ``2025q2`` and monthly data sets from ``2025_07`` (the page, checked 2026-09-29; the
cadence switched after 2025q2). Each data set holds the XBRL submissions accepted in its span (EDGAR 17:30 ET
cut-off) as tab-separated ``sub/tag/dim/num/txt/ren/pre/cal.tsv`` plus ``readme.htm`` and
``notes-metadata.json``. Ruling D7: land from ``2019q1`` (one year of look-back before the 2020 score window).

``fetch`` handles one zip at a time, the next one downloading in a background thread (so at most two zips are
on disk): refuse unless C: keeps >= 40 GB free after the download, download through the host-wide SEC limiter
with the approved user agent (:func:`shortflow_common.sec_get`), SHA-256, then stream ``sub/dim/txt/num.tsv``
straight out of the zip with the pyarrow CSV reader (8 MiB blocks, nothing extracted, no DuckDB: controller ruling
C-1 lets this run unguarded at <= 0.25 GiB peak; the process stops itself above :data:`MEM_ABORT_GB` and records
its peak commit per zip), filter and type each block, write the parts, verify, delete the zip. Only the Parquet
parts and one receipt line per state change are kept (``data/raw/sec_notes/receipts.jsonl``: url, bytes, sha256,
http_status, fetched_at, Last-Modified, ETag, member size/CRC/lines/rows, part rows/bytes/sha256, verification,
peak memory). Resumable per zip: a data set whose ``parsed`` receipt matches its parts on disk is skipped; a
landed zip whose SHA-256 matches its ``downloaded`` receipt is parsed without a new download.

Parts (``notes/parts/source=<key>/``), rows as served, typed (``''`` = NULL), plus the data-set key ``source``:

* ``sub.parquet``: every submission (all forms): the SUB fields (``cik``/``sic``/``fy`` int64, dates date32,
  flags bool), ``accepted_et_naive`` (EDGAR's America/New_York wall clock, as published) and ``accepted_utc``
  (UTC, DST-aware conversion; the clock rule of the FSDS stager ``fsds_baseline.ACCEPTED_CLOCK``).
* ``txt_dei.parquet``: TXT rows of the dei taxonomy (``version`` ``dei/...``: cover page facts, all forms).
* ``num_dei.parquet``: NUM rows of the dei taxonomy (share counts per class, public float, ...), all forms.
* ``num_items.parquet``: NUM rows of periodic forms (:data:`PERIODIC_FORMS`) that S4.4 needs: standard-taxonomy
  tags (``version <> adsh``) matching :data:`ITEM_TAG_RE` (segment measures and totals, debt, leases, pension,
  share-based compensation, income tax, goodwill/intangibles/impairments) with any dimensions, plus every
  fact (custom tags included) whose dimensions include a business-segment or geographic axis
  (:data:`SEGMENT_AXES`).
* ``dim.parquet``: the DIM rows (``dimhash, segments, segt``) of every ``dimh`` referenced by a kept row; join on
  ``(dimh = dimhash, source)``. SEC writes ``segments`` as ``Axis=Member;`` pairs with 'Statement', 'Axis',
  'Member' and 'Domain' stripped (``BusinessSegments=Americas;ConsolidationItems=OperatingSegments;``).

NUM/TXT ``ddate`` is SEC's month-end rounding of the context end date; ``datp`` is the rounding distance, so the
reported end date is ``ddate - datp`` days. Rows with the same key but ``iprx`` > 0 repeat a fact (kept as
served; consumers dedupe on the lowest ``iprx``).

Verification per zip (receipt ``verify``): zip SHA-256 equals the download receipt; per member the rows read
plus the rows the reader rejected equal the physical lines minus the header (``lines_consistent``; rejected rows
are counted with samples); the SUB accession is unique; every kept NUM/TXT row has a SUB row (``orphan_rows``)
and every referenced dimension hash is found in DIM (``dim_missing``); text that does not parse as the column's
number / date / flag becomes NULL and is counted (``unparsed``); every part re-reads with the written row count.
"""

from __future__ import annotations

import argparse
import concurrent.futures as cf
import datetime as dt
import json
import os
import re
import shutil
import sys
import time
import zipfile
from pathlib import Path
from typing import Any
from urllib.parse import urljoin

from . import common as C
from . import shortflow_common as S

STAGE = "notes"
SCHEMA = "atx.alpha-panel.notes/v1"
PAGE_URL = "https://www.sec.gov/data-research/sec-markets-data/financial-statement-notes-data-sets"
RAW_DIR = S.RAW_ROOT / "sec_notes"
ARCHIVE_RECEIPTS = C.PACKAGE_ROOT / "data" / "archive" / "receipts"
FIRST_KEY = "2019q1"                      # ruling D7
MIN_FREE_GB = 40.0                         # C: floor after the download (lane rule); nothing is extracted
ROW_GROUP = 65536                          # part row groups (bounded writer buffers)
BLOCK_BYTES = 8 << 20                      # pyarrow CSV block: the unit of streaming
MEM_ABORT_GB = 0.30                        # self-stop above this private commit (ruling C-1 budget: <= 0.25 GiB peak)
MEMBERS = ("sub.tsv", "dim.tsv", "num.tsv", "txt.tsv")
NO_DIM = "0x00000000"
PERIODIC_FORMS = ("10-K", "10-Q", "10-KT", "10-QT", "20-F", "40-F", "10-K/A", "10-Q/A", "10-KT/A", "10-QT/A",
                  "20-F/A", "40-F/A")
#: DIM ``segments`` axis names (SEC strips 'Statement' and 'Axis'): us-gaap StatementBusinessSegmentsAxis and
#: srt StatementGeographicalAxis.
SEGMENT_AXES = ("BusinessSegments", "Geographical")
#: Standard-tag prefixes kept for S4.4 (any dimensions). Families: segment measures and consolidated totals;
#: debt and maturities; leases (ASC 842 and 840); pension/OPEB; share-based compensation; income tax
#: components; goodwill, intangibles and impairments.
ITEM_TAG_PREFIXES = (
    # revenue / profit / assets (segment measures and totals)
    "Revenue", "SalesRevenue", "InterestAndDividendIncomeOperating", "NoninterestIncome", "CostOfRevenue",
    "CostOfGoodsAndServicesSold", "GrossProfit", "OperatingIncomeLoss", "OperatingExpenses", "Assets",
    "NoncurrentAssets", "LongLivedAssets", "PropertyPlantAndEquipmentNet", "SegmentReporting", "SegmentExpense",
    "IncomeLossFromContinuingOperationsBeforeIncomeTaxes", "DepreciationDepletionAndAmortization",
    "DepreciationAndAmortization", "DepreciationAmortizationAndAccretionNet", "PaymentsToAcquirePropertyPlantAndEquipment",
    # debt and maturities
    "LongTermDebt", "DebtCurrent", "OtherLongTermDebt", "ShortTermBorrowings", "DebtInstrumentCarryingAmount",
    "ContractualObligation", "DebtAndCapitalLeaseObligations", "LongTermDebtAndCapitalLeaseObligations",
    # leases
    "OperatingLease", "FinanceLease", "LesseeOperatingLease", "LesseeFinanceLease", "LeaseCost", "ShortTermLeaseCost",
    "VariableLeaseCost", "SubleaseIncome", "OperatingLeasesFutureMinimumPayments", "OperatingLeasesRentExpense",
    "CapitalLeaseObligations", "CapitalLeasesFutureMinimumPayments", "RightOfUseAsset",
    # pension / OPEB
    "DefinedBenefitPlan", "DefinedContributionPlan", "PensionAndOtherPostretirement", "PensionCost", "PensionExpense",
    "OtherPostretirementBenefit",
    # share-based compensation
    "ShareBasedCompensation", "AllocatedShareBasedCompensationExpense", "EmployeeServiceShareBasedCompensation",
    "ShareBasedPaymentArrangement", "AdjustmentsToAdditionalPaidInCapitalSharebasedCompensation",
    "StockIssuedDuringPeriodValueShareBasedCompensation",
    # income tax
    "IncomeTax", "CurrentFederal", "CurrentForeign", "CurrentStateAndLocal", "CurrentIncomeTax", "DeferredFederal",
    "DeferredForeign", "DeferredStateAndLocal", "DeferredIncomeTax", "DeferredTax", "EffectiveIncomeTaxRate",
    "UnrecognizedTaxBenefits", "OperatingLossCarryforwards", "TaxCreditCarryforward",
    # goodwill, intangibles, impairments
    "Goodwill", "ImpairmentOf", "AssetImpairmentCharges", "OtherAssetImpairmentCharges", "TangibleAssetImpairmentCharges",
    "IndefiniteLivedIntangibleAssets", "FiniteLivedIntangibleAssets", "IntangibleAssetsNetExcludingGoodwill",
    "IntangibleAssetsNetIncludingGoodwill", "RestructuringCharges",
)
ITEM_TAG_RE = "^(" + "|".join(ITEM_TAG_PREFIXES) + ")"
MODULES = ("notes_fetch", "shortflow_common", "common", "finra_fetch")
_NAME_RE = re.compile(r"(\d{4})(?:q([1-4])|_(\d{2}))_notes(?:_(\d+))?\.zip$", re.IGNORECASE)


def ledger() -> S.Ledger:
    return S.Ledger(RAW_DIR / "receipts.jsonl")


def parts_dir() -> Path:
    return C.stage_dir(STAGE) / "parts"


# ---------------------------------------------------------------- pure helpers
def data_set(url: str) -> dict[str, Any]:
    """``{key, year, first_month, span_start, span_end, cadence, repost}`` of a data-set file name.

    ``2019q1_notes.zip`` -> key ``2019q1`` (quarterly); ``2025_07_notes.zip`` -> key ``2025_07`` (monthly);
    ``2010q1_notes_1.zip`` -> key ``2010q1`` with ``repost`` 1 (SEC re-issued file)."""
    name = url.rsplit("/", 1)[-1]
    m = _NAME_RE.search(name)
    if not m:
        raise ValueError(f"unrecognised notes data set name {name!r}")
    y = int(m.group(1))
    if m.group(2):
        q = int(m.group(2))
        first, last, key, cad = 3 * q - 2, 3 * q, f"{y}q{q}", "quarterly"
    else:
        first = last = int(m.group(3))
        key, cad = f"{y}_{first:02d}", "monthly"
    end = dt.date(y + (last == 12), 1 if last == 12 else last + 1, 1) - dt.timedelta(days=1)
    return {"key": key, "year": y, "first_month": first, "span_start": dt.date(y, first, 1), "span_end": end,
            "cadence": cad, "repost": int(m.group(4)) if m.group(4) else 0, "file": name}


def order_key(key: str) -> tuple[int, int]:
    """Chronological sort key of a data-set key (``2025q2`` < ``2025_07``)."""
    y = int(key[:4])
    return (y, 3 * int(key[5]) - 2) if key[4] == "q" else (y, int(key[5:7]))


def discover(page_html: str, first: str = FIRST_KEY) -> list[dict[str, Any]]:
    """Data sets linked from the page from ``first`` on, oldest first (a re-posted name wins over the plain one)."""
    out: dict[str, dict[str, Any]] = {}
    for href in re.findall(r"href=[\"']([^\"']+?_notes(?:_\d+)?\.zip)[\"']", page_html, re.IGNORECASE):
        ds = data_set(href) | {"url": urljoin(PAGE_URL, href)}
        if order_key(ds["key"]) < order_key(first):
            continue
        if ds["key"] not in out or ds["repost"] > out[ds["key"]]["repost"]:
            out[ds["key"]] = ds
    return sorted(out.values(), key=lambda d: order_key(d["key"]))


def count_lines(blob_tail: bytes, newlines: int, size: int) -> int:
    """Physical lines of a file with ``newlines`` LF bytes whose last byte is ``blob_tail`` (no final LF -> +1)."""
    return newlines + (1 if size and blob_tail != b"\n" else 0)


def free_gb() -> float:
    return shutil.disk_usage(C.PACKAGE_ROOT).free / 1024 ** 3


def require_free(need_gb: float) -> None:
    """Refuse a step that would leave C: below ``MIN_FREE_GB`` (the lane's 40 GB floor)."""
    free = free_gb()
    if free - need_gb < MIN_FREE_GB:
        raise RuntimeError(f"disk guard: {free:.1f} GB free, step needs {need_gb:.1f} GB, floor {MIN_FREE_GB} GB")


# ---------------------------------------------------------------- typed columns
#: column kinds per table (SEC readme order); anything else stays a string. '' is NULL everywhere.
SUB_KINDS = {"cik": "int", "sic": "int", "wksi": "bool", "period": "date", "fy": "int", "filed": "date",
             "prevrpt": "bool", "detail": "bool", "nciks": "int", "pubfloatusd": "float", "floatdate": "date",
             "floatmems": "int"}
NUM_KINDS = {"ddate": "date", "qtrs": "int", "iprx": "int", "value": "float", "footlen": "int", "dimn": "int",
             "durp": "float", "datp": "float", "dcml": "int"}
TXT_KINDS = {"ddate": "date", "qtrs": "int", "iprx": "int", "dcml": "int", "durp": "float", "datp": "float",
             "dimn": "int", "escaped": "bool", "srclen": "int", "txtlen": "int", "footlen": "int"}
DIM_KINDS = {"segt": "bool"}
COLUMNS = {
    "sub": ("adsh", "cik", "name", "sic", "countryba", "stprba", "cityba", "zipba", "bas1", "bas2", "baph", "countryma",
            "stprma", "cityma", "zipma", "mas1", "mas2", "countryinc", "stprinc", "ein", "former", "changed", "afs",
            "wksi", "fye", "form", "period", "fy", "fp", "filed", "accepted", "prevrpt", "detail", "instance", "nciks",
            "aciks", "pubfloatusd", "floatdate", "floataxis", "floatmems"),
    "num": ("adsh", "tag", "version", "ddate", "qtrs", "uom", "dimh", "iprx", "value", "footnote", "footlen", "dimn",
            "coreg", "durp", "datp", "dcml"),
    "txt": ("adsh", "tag", "version", "ddate", "qtrs", "iprx", "lang", "dcml", "durp", "datp", "dimh", "dimn", "coreg",
            "escaped", "srclen", "txtlen", "footnote", "footlen", "context", "value"),
    "dim": ("dimhash", "segments", "segt"),
}
KINDS = {"sub": SUB_KINDS, "num": NUM_KINDS, "txt": TXT_KINDS, "dim": DIM_KINDS}
_NUM_RE = r"^[-+]?([0-9]+\.?[0-9]*|\.[0-9]+)([eE][-+]?[0-9]+)?$"
_INT_RE = r"^[-+]?[0-9]+$"
ET = "America/New_York"


def _pa():
    import pyarrow as pa
    import pyarrow.compute as pc

    return pa, pc


def arrow_type(kind: str | None):
    pa, _ = _pa()
    return {"int": pa.int64(), "float": pa.float64(), "date": pa.date32(), "bool": pa.bool_()}.get(kind, pa.string())


def out_schema(table: str, extra: tuple[tuple[str, Any], ...] = ()):
    """Parquet schema of a part: the table's columns typed by :data:`KINDS`, then ``extra``, then ``source``."""
    pa, _ = _pa()
    cols = [c for c in COLUMNS[table] if not (table == "sub" and c == "accepted")]
    fields = [pa.field(c, arrow_type(KINDS[table].get(c))) for c in cols]
    return pa.schema(fields + [pa.field(n, t) for n, t in extra] + [pa.field("source", pa.string())])


def cast_batch(batch, table: str, stats: dict[str, int]):
    """String batch -> typed columns; a non-null text that is not a valid number / date / flag becomes NULL and is
    counted in ``stats['<column>']``."""
    pa, pc = _pa()
    out = {}
    for name in batch.schema.names:
        col = batch.column(name)
        kind = KINDS[table].get(name)
        if kind is None:
            out[name] = col
            continue
        s = pc.utf8_trim_whitespace(col)
        if kind in ("int", "float"):
            ok = pc.match_substring_regex(s, _INT_RE if kind == "int" else _NUM_RE)
        elif kind == "date":
            ok = pc.match_substring_regex(s, r"^[0-9]{8}$")
        else:
            ok = pc.is_in(pc.utf8_lower(s), value_set=pa.array(["0", "1", "true", "false"]))
        bad = int(pc.sum(pc.and_(pc.is_valid(s), pc.invert(ok))).as_py() or 0)
        if bad:
            stats[name] = stats.get(name, 0) + bad
        s = pc.if_else(ok, s, pa.scalar(None, pa.string()))
        if kind == "int":
            out[name] = pc.cast(s, pa.int64())
        elif kind == "float":
            out[name] = pc.cast(s, pa.float64())
        elif kind == "date":
            out[name] = pc.cast(pc.strptime(s, format="%Y%m%d", unit="s", error_is_null=True), pa.date32())
        else:
            out[name] = pc.is_in(pc.utf8_lower(s), value_set=pa.array(["1", "true"]))
            out[name] = pc.if_else(pc.is_valid(s), out[name], pa.scalar(None, pa.bool_()))
    return out


def accepted_clocks(values: list[str | None]) -> tuple[list[dt.datetime | None], list[dt.datetime | None]]:
    """SEC ``accepted`` text (EDGAR America/New_York wall clock) -> (naive as published, aware UTC). Ambiguous
    fall-back hours resolve to the first (daylight) occurrence (``fold=0``)."""
    from zoneinfo import ZoneInfo

    tz = ZoneInfo(ET)
    naive: list[dt.datetime | None] = []
    utc: list[dt.datetime | None] = []
    for v in values:
        t = None
        if v:
            try:
                t = dt.datetime.fromisoformat(v.strip())
            except ValueError:
                t = None
        naive.append(t)
        utc.append(t.replace(tzinfo=tz).astimezone(dt.UTC) if t else None)
    return naive, utc


# ---------------------------------------------------------------- streaming reader
def process_memory() -> dict[str, float]:
    """Current and peak private commit / working set of this process (GiB; Windows, else empty)."""
    if sys.platform != "win32":
        return {}
    import ctypes
    from ctypes import wintypes

    class PMC(ctypes.Structure):
        _fields_ = [("cb", wintypes.DWORD), ("PageFaultCount", wintypes.DWORD), ("PeakWorkingSetSize", ctypes.c_size_t),
                    ("WorkingSetSize", ctypes.c_size_t), ("QuotaPeakPagedPoolUsage", ctypes.c_size_t),
                    ("QuotaPagedPoolUsage", ctypes.c_size_t), ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t),
                    ("QuotaNonPagedPoolUsage", ctypes.c_size_t), ("PagefileUsage", ctypes.c_size_t),
                    ("PeakPagefileUsage", ctypes.c_size_t)]

    k = ctypes.WinDLL("kernel32")
    k.GetCurrentProcess.restype = wintypes.HANDLE
    k.K32GetProcessMemoryInfo.argtypes = [wintypes.HANDLE, ctypes.POINTER(PMC), wintypes.DWORD]
    m = PMC()
    m.cb = ctypes.sizeof(PMC)
    k.K32GetProcessMemoryInfo(k.GetCurrentProcess(), ctypes.byref(m), m.cb)
    g = 1024 ** 3
    return {"commit_gb": round(m.PagefileUsage / g, 4), "peak_commit_gb": round(m.PeakPagefileUsage / g, 4),
            "working_set_gb": round(m.WorkingSetSize / g, 4), "peak_working_set_gb": round(m.PeakWorkingSetSize / g, 4)}


def _check_memory() -> None:
    m = process_memory()
    if m and m["commit_gb"] > MEM_ABORT_GB:
        raise MemoryError(f"notes parse commit {m['commit_gb']} GiB > {MEM_ABORT_GB} GiB (ruling C-1 budget)")


def line_chunks(fh, block: int):
    """Yield line-aligned byte chunks of about ``block`` bytes (the last one may lack a final LF)."""
    carry = b""
    while True:
        chunk = fh.read(block)
        if not chunk:
            if carry:
                yield carry
            return
        data = carry + chunk
        cut = data.rfind(b"\n")
        if cut < 0:
            carry = data
            continue
        carry = data[cut + 1:]
        yield data[:cut + 1]


def stream_member(z: zipfile.ZipFile, name: str, table: str, stats: dict[str, Any]):
    """Yield string record batches of one TSV member; fills ``stats`` (rows, rejected rows, bytes, lines) when done.

    Synchronous: line-aligned chunks of :data:`BLOCK_BYTES` are parsed one at a time by the pyarrow CSV parser (no
    readahead; its streaming reader over a Python file queued unbounded blocks, measured 0.16 GiB after 30 blocks).
    A row with the wrong number of fields (blank lines included) is rejected, counted and sampled."""
    pa, _ = _pa()
    import pyarrow.csv as pacsv

    samples: list[tuple[int, int, str]] = []
    n_invalid = [0]

    def handler(row) -> str:
        n_invalid[0] += 1
        if len(samples) < 5:
            samples.append((row.number, row.actual_columns, (row.text or "")[:300]))
        return "skip"

    cols = COLUMNS[table]
    with z.open(name) as fh:
        head = fh.readline()
        header = head.decode("utf-8-sig").rstrip("\r\n").split("\t")
        if header != list(cols):
            raise RuntimeError(f"{name}: header {header} differs from {list(cols)}")
        lines, nbytes, rows = (1 if head else 0), len(head), 0
        for data in line_chunks(fh, BLOCK_BYTES):
            lines += data.count(b"\n") + (0 if data.endswith(b"\n") else 1)
            nbytes += len(data)
            t = pacsv.read_csv(
                pa.BufferReader(pa.py_buffer(data)),
                read_options=pacsv.ReadOptions(column_names=list(cols), use_threads=False, block_size=len(data) + 1),
                parse_options=pacsv.ParseOptions(delimiter="\t", quote_char=False, double_quote=False, escape_char=False,
                                                 newlines_in_values=False, ignore_empty_lines=False,
                                                 invalid_row_handler=handler),
                convert_options=pacsv.ConvertOptions(column_types={c: pa.string() for c in cols},
                                                     strings_can_be_null=True, null_values=[""], check_utf8=False),
            )
            del data
            for batch in t.to_batches():
                rows += batch.num_rows
                _check_memory()
                yield batch
            del t
    info = z.getinfo(name)
    stats.update(member=name, bytes=info.file_size, crc=info.CRC, rows=rows, invalid_rows=n_invalid[0],
                 invalid_samples=samples, lines=lines, bytes_read=nbytes)
    stats["rows_plus_invalid_equal_lines_minus_header"] = rows + n_invalid[0] == lines - 1


class _PartWriter:
    """Buffered Parquet writer (row groups of ``ROW_GROUP`` rows, ZSTD, ``.partial`` then rename)."""

    def __init__(self, path: Path, schema) -> None:
        import pyarrow.parquet as pq

        self.path, self.schema, self.rows, self.buf, self.n_buf = path, schema, 0, [], 0
        self.tmp = path.with_name(path.name + ".partial")
        self.w = pq.ParquetWriter(self.tmp, schema, compression="zstd")

    def add(self, cols: dict[str, Any]) -> None:
        pa, _ = _pa()
        n = len(next(iter(cols.values())))
        if not n:
            return
        self.buf.append(pa.table([cols[f.name] for f in self.schema], schema=self.schema))
        self.n_buf += n
        if self.n_buf >= ROW_GROUP:
            self.flush()

    def flush(self) -> None:
        pa, _ = _pa()
        if self.buf:
            t = pa.concat_tables(self.buf)
            self.w.write_table(t, row_group_size=ROW_GROUP)
            self.rows += t.num_rows
            self.buf, self.n_buf = [], 0

    def close(self) -> int:
        import pyarrow.parquet as pq

        self.flush()
        self.w.close()
        if pq.ParquetFile(self.tmp).metadata.num_rows != self.rows:
            raise RuntimeError(f"{self.tmp}: re-read row count differs from {self.rows}")
        os.replace(self.tmp, self.path)
        return self.rows


# ---------------------------------------------------------------- parse + verify
def parse_archive(zpath: Path, key: str, work: Path | None = None) -> dict[str, Any]:
    """Stream ``sub/dim/txt/num.tsv`` from the zip (no extraction, no DuckDB: ruling C-1), write
    ``parts/source=<key>/{sub,txt_dei,num_dei,num_items,dim}.parquet``, verify; return counts. ``work`` is unused
    (kept for the caller's signature)."""
    pa, pc = _pa()
    pa.set_memory_pool(pa.system_memory_pool())     # freed buffers go back to the OS (mimalloc keeps them)
    pa.set_io_thread_count(1)
    t0 = time.perf_counter()
    out = parts_dir() / f"source={key}"
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    res: dict[str, Any] = {"members": {}, "rows": {}, "verify": {}, "unparsed": {}}
    src = lambda n: pa.array([key] * n, pa.string())  # noqa: E731
    with zipfile.ZipFile(zpath) as z:
        by_base = {Path(n.replace("\\", "/")).name.lower(): n for n in z.namelist() if not n.endswith("/")}
        for m in MEMBERS:
            if m not in by_base:
                raise RuntimeError(f"{zpath.name}: member {m} missing")
        meta = by_base.get("notes-metadata.json")
        if meta:
            S.F.write_gzip_atomic(RAW_DIR / "meta" / f"{zpath.stem}.notes-metadata.json.gz", z.read(meta))
        res["members_in_zip"] = sorted(by_base)

        # 1) SUB: every submission, typed, with both acceptance clocks
        st: dict[str, Any] = {}
        un: dict[str, int] = {}
        w = _PartWriter(out / "sub.parquet", out_schema("sub", (("accepted_et_naive", pa.timestamp("us")),
                                                                ("accepted_utc", pa.timestamp("us", tz="UTC")))))
        adsh_all: list[str] = []
        periodic: list[str] = []
        forms: dict[str, int] = {}
        for b in stream_member(z, by_base["sub.tsv"], "sub", st):
            cols = cast_batch(b.drop_columns(["accepted"]), "sub", un)
            naive, utc = accepted_clocks(b.column("accepted").to_pylist())
            cols["accepted_et_naive"] = pa.array(naive, pa.timestamp("us"))
            cols["accepted_utc"] = pa.array(utc, pa.timestamp("us", tz="UTC"))
            cols["source"] = src(b.num_rows)
            w.add(cols)
            a, f = b.column("adsh").to_pylist(), b.column("form").to_pylist()
            adsh_all += a
            periodic += [x for x, y in zip(a, f, strict=True) if y in PERIODIC_FORMS]
            for y in f:
                forms[y] = forms.get(y, 0) + 1
        res["rows"]["sub"] = w.close()
        res["members"]["sub.tsv"], res["unparsed"]["sub"] = st, un
        sub_set = pa.array(sorted(set(adsh_all)), pa.string())
        per_set = pa.array(sorted(set(periodic)), pa.string())

        # 2) DIM pre-pass: hashes with a business-segment or geographic axis
        seg_re = "(^|;)(" + "|".join(SEGMENT_AXES) + ")="
        seg_chunks = [pa.array([], pa.string())]
        for b in stream_member(z, by_base["dim.tsv"], "dim", {}):
            m = pc.fill_null(pc.match_substring_regex(b.column("segments"), seg_re), False)
            seg_chunks.append(pc.filter(b.column("dimhash"), m))
        seg_set = pc.unique(pa.concat_arrays(seg_chunks))
        del seg_chunks
        # referenced dimension hashes, kept as Arrow chunks compacted by unique() (a Python set of ~1M hashes
        # would cost ~0.1 GiB)
        used: list[Any] = []
        orphans: dict[str, int] = {}

        def add_used(arr) -> None:
            used.append(pc.unique(arr))
            if len(used) >= 64:
                used[:] = [pc.unique(pa.concat_arrays(used))]

        def keep(writer: _PartWriter, b, mask, table: str, part: str, unp: dict[str, int]) -> None:
            fb = b.filter(pc.fill_null(mask, False))
            if not fb.num_rows:
                return
            add_used(fb.column("dimh"))
            orphans[part] = orphans.get(part, 0) + int(pc.sum(pc.invert(pc.is_in(fb.column("adsh"), value_set=sub_set))).as_py() or 0)
            cols = cast_batch(fb, table, unp)
            cols["source"] = src(fb.num_rows)
            writer.add(cols)

        # 3) TXT: dei rows (cover page, every form)
        st, un = {}, {}
        w = _PartWriter(out / "txt_dei.parquet", out_schema("txt"))
        for b in stream_member(z, by_base["txt.tsv"], "txt", st):
            keep(w, b, pc.starts_with(b.column("version"), "dei/"), "txt", "txt_dei", un)
        res["rows"]["txt_dei"] = w.close()
        res["members"]["txt.tsv"], res["unparsed"]["txt"] = st, un

        # 4) NUM: dei rows (every form) and the S4.4 rows (periodic forms)
        st, un = {}, {}
        wd = _PartWriter(out / "num_dei.parquet", out_schema("num"))
        wi = _PartWriter(out / "num_items.parquet", out_schema("num"))
        for b in stream_member(z, by_base["num.tsv"], "num", st):
            ver, tag, dimh = b.column("version"), b.column("tag"), b.column("dimh")
            keep(wd, b, pc.starts_with(ver, "dei/"), "num", "num_dei", un)
            std = pc.and_(pc.not_equal(ver, b.column("adsh")), pc.match_substring_regex(tag, ITEM_TAG_RE))
            item = pc.and_(pc.is_in(b.column("adsh"), value_set=per_set),
                           pc.or_(pc.fill_null(std, False), pc.is_in(dimh, value_set=seg_set)))
            keep(wi, b, item, "num", "num_items", un)
        res["rows"]["num_dei"], res["rows"]["num_items"] = wd.close(), wi.close()
        res["members"]["num.tsv"], res["unparsed"]["num"] = st, un

        # 5) DIM subset: every hash referenced by a kept row
        st, un = {}, {}
        used_set = pc.unique(pa.concat_arrays(used or [pa.array([], pa.string())]))
        used_set = pc.drop_null(pc.filter(used_set, pc.not_equal(used_set, NO_DIM)))
        used.clear()
        found: list[Any] = [pa.array([], pa.string())]
        w = _PartWriter(out / "dim.parquet", out_schema("dim"))
        n_dim = 0
        for b in stream_member(z, by_base["dim.tsv"], "dim", st):
            n_dim += b.num_rows
            fb = b.filter(pc.is_in(b.column("dimhash"), value_set=used_set))
            found.append(fb.column("dimhash"))
            cols = cast_batch(fb, "dim", un)
            cols["source"] = src(fb.num_rows)
            w.add(cols)
        res["rows"]["dim"] = w.close()
        res["members"]["dim.tsv"], res["unparsed"]["dim"] = st, un

    v = res["verify"]
    v["lines_consistent"] = {m: res["members"][m]["rows_plus_invalid_equal_lines_minus_header"] for m in MEMBERS}
    v["invalid_rows"] = {m: res["members"][m]["invalid_rows"] for m in MEMBERS}
    v["sub_duplicate_adsh"] = len(adsh_all) - len(set(adsh_all))
    v["orphan_rows"] = {p: orphans.get(p, 0) for p in ("txt_dei", "num_dei", "num_items")}
    n_found = len(pc.unique(pa.concat_arrays(found)))
    v["dim_missing"] = len(used_set) - n_found
    v["dim_duplicate_hash"] = res["rows"]["dim"] - n_found
    v["dims_referenced"] = len(used_set)
    v["dim_rows_total"] = n_dim
    v["forms"] = dict(sorted(forms.items(), key=lambda kv: -kv[1])[:25])
    v["ok"] = (all(v["lines_consistent"].values()) and v["sub_duplicate_adsh"] == 0
               and all(n == 0 for n in v["orphan_rows"].values()) and v["dim_missing"] == 0)
    res["parts"] = {p.name: {"bytes": p.stat().st_size, "sha256": C.sha256_file(p)} for p in sorted(out.glob("*.parquet"))}
    res["parse_s"] = round(time.perf_counter() - t0, 1)
    res["memory"] = process_memory()
    if not v["ok"]:
        raise RuntimeError(f"{key}: verification failed {json.dumps(v, default=str)[:2000]}")
    return res


def stitch_parquet(parts: list[Path], dest: Path, row_group: int = ROW_GROUP) -> int:
    """Concatenate Parquet files of one schema in the given order into ``dest`` (streamed by row group,
    ``.partial`` then rename); return the row count. Used by the per-year builds of the consumer stages."""
    import pyarrow.parquet as pq

    parts = [p for p in parts if p.exists()]
    if not parts:
        raise RuntimeError(f"{dest}: nothing to stitch")
    schema = pq.read_schema(parts[0])
    tmp = dest.with_name(dest.name + ".partial")
    rows = 0
    with pq.ParquetWriter(tmp, schema, compression="zstd") as w:
        for p in parts:
            f = pq.ParquetFile(p)
            if not f.schema_arrow.equals(schema):
                raise RuntimeError(f"{p}: schema differs from {parts[0]}")
            for b in f.iter_batches(batch_size=row_group):
                w.write_batch(b, row_group_size=row_group)
                rows += b.num_rows
    if pq.ParquetFile(tmp).metadata.num_rows != rows:
        raise RuntimeError(f"{tmp}: re-read row count differs from {rows}")
    os.replace(tmp, dest)
    return rows


# ---------------------------------------------------------------- fetch
def _zip_path(ds: dict[str, Any]) -> Path:
    return RAW_DIR / ds["file"]


def _parts_ok(key: str, rec: dict[str, Any]) -> bool:
    d = parts_dir() / f"source={key}"
    parts = rec.get("parts") or {}
    return bool(parts) and all((d / n).exists() and (d / n).stat().st_size == v["bytes"] for n, v in parts.items())


def _download(ds: dict[str, Any], expect_mb: float | None) -> dict[str, Any]:
    """Land one zip (streamed, ``.partial`` then rename) and return its receipt (status ``downloaded``)."""
    zpath = _zip_path(ds)
    require_free((expect_mb or 1000.0) / 1024)
    if len([p for p in RAW_DIR.glob("*_notes*.zip")]) >= 2:
        raise RuntimeError("two notes zips already on disk; refusing a third")
    t0 = time.perf_counter()
    st, _, hdr = S.sec_get(ds["url"], timeout=3600, stream_to=zpath)
    rec = {"key": ds["key"], "kind": "zip", "url": ds["url"], "http_status": st, "fetched_at": S.utc_now(),
           "cadence": ds["cadence"], "span": [ds["span_start"].isoformat(), ds["span_end"].isoformat()],
           "repost": ds["repost"], "file": ds["file"]}
    if st != 200:
        return rec | {"status": "absent"}
    rec.update(bytes=zpath.stat().st_size, sha256=C.sha256_file(zpath), last_modified=hdr.get("last-modified"),
               etag=hdr.get("etag"), content_length=hdr.get("content-length"), status="downloaded",
               download_s=round(time.perf_counter() - t0, 1))
    return rec


def _sizes_from_page(page_html: str) -> dict[str, float]:
    """``{file name: MB}`` from the page's file-size column (used only for the disk pre-check)."""
    out: dict[str, float] = {}
    for href, size in re.findall(r'href="[^"]*/([^"/]+_notes(?:_\d+)?\.zip)".*?views-field-filesize">\s*([\d.]+ [KMG]B)',
                                 page_html, re.S):
        v, u = size.split()
        out[href] = float(v) * {"KB": 1 / 1024, "MB": 1.0, "GB": 1024.0}[u]
    return out


def fetch(limit: int | None = None, only: str | None = None, first: str = FIRST_KEY) -> dict[str, Any]:
    RAW_DIR.mkdir(parents=True, exist_ok=True)
    led = ledger()
    st, body, _ = S.sec_get(PAGE_URL, timeout=120)
    if st != 200:
        raise RuntimeError(f"notes page HTTP {st}")
    import hashlib

    snap = RAW_DIR / f"page_{dt.datetime.now(dt.UTC):%Y%m%dT%H%M%SZ}.html.gz"
    S.F.write_gzip_atomic(snap, body)
    led.append({"key": "page", "kind": "page", "url": PAGE_URL, "sha256": hashlib.sha256(body).hexdigest(),
                "bytes": len(body), "fetched_at": S.utc_now(), "file": snap.name})
    html = body.decode("utf-8", errors="replace")
    sizes = _sizes_from_page(html)
    sets = discover(html, first)
    latest = led.latest()
    done = {k for k, v in latest.items() if v.get("status") == "parsed" and _parts_ok(k, v)}
    todo = [d for d in sets if d["key"] not in done and (only is None or d["key"] == only)]
    if limit is not None:
        todo = todo[:limit]
    print(f"notes: {len(sets)} data sets from {first}, {len(done)} parsed, {len(todo)} to do, free {free_gb():.1f} GB",
          flush=True)
    stats: dict[str, Any] = {"listed": len(sets), "parsed_before": len(done), "done_now": 0, "bytes": 0}

    def landed(ds: dict[str, Any]) -> dict[str, Any] | None:
        rec = latest.get(ds["key"])
        z = _zip_path(ds)
        if rec and rec.get("status") == "downloaded" and z.exists() and z.stat().st_size == rec.get("bytes") \
                and C.sha256_file(z) == rec.get("sha256"):
            return rec
        return None

    with cf.ThreadPoolExecutor(max_workers=1) as pool:
        fut: cf.Future | None = None
        for i, ds in enumerate(todo):
            rec = landed(ds)
            if rec is None:
                if fut is None:
                    fut = pool.submit(_download, ds, sizes.get(ds["file"]))
                rec = fut.result()
                fut = None
                led.append(rec)
            if rec.get("status") != "downloaded":
                print(f"  {ds['key']}: HTTP {rec.get('http_status')}", flush=True)
                continue
            if i + 1 < len(todo) and landed(todo[i + 1]) is None:   # prefetch the next zip (<= 2 on disk)
                fut = pool.submit(_download, todo[i + 1], sizes.get(todo[i + 1]["file"]))
            zpath = _zip_path(ds)
            if C.sha256_file(zpath) != rec["sha256"]:
                raise RuntimeError(f"{zpath}: sha256 mismatch vs receipt")
            res = parse_archive(zpath, ds["key"])
            zpath.unlink()
            rec = rec | {"status": "parsed", "parsed_at": S.utc_now(), "zip_deleted": True, "zip_sha256_verified": True,
                         **res}
            led.append(rec)
            stats["done_now"] += 1
            stats["bytes"] += rec["bytes"]
            print(f"  {ds['key']}: {rec['bytes'] / 1e6:.0f} MB zip, rows {res['rows']}, parse {res['parse_s']} s, "
                  f"free {free_gb():.1f} GB", flush=True)
        if fut is not None:
            led.append(fut.result())
    return stats


# ---------------------------------------------------------------- manifest
def finalize() -> Path:
    """Re-hash every part against its ``parsed`` receipt, archive the receipt ledger, publish ``manifest.json``."""
    led = ledger()
    recs = {k: v for k, v in led.latest().items() if v.get("status") == "parsed"}
    bad = []
    for k, v in recs.items():
        d = parts_dir() / f"source={k}"
        for n, meta in v["parts"].items():
            p = d / n
            if not p.exists() or C.sha256_file(p) != meta["sha256"]:
                bad.append(f"{k}/{n}")
    if bad:
        raise RuntimeError(f"parts differ from receipts: {bad[:20]}")
    ARCHIVE_RECEIPTS.mkdir(parents=True, exist_ok=True)
    arch = ARCHIVE_RECEIPTS / "sec_notes-receipts.jsonl"
    shutil.copyfile(led.path, arch)
    keys = sorted(recs, key=order_key)
    rows = {p: sum(recs[k]["rows"][p] for k in keys) for p in ("sub", "txt_dei", "num_dei", "num_items", "dim")}
    payload = {
        "source": {"page": PAGE_URL, "url_pattern": "https://www.sec.gov/files/dera/data/financial-statement-notes-"
                   "data-sets/{YYYYqN|YYYY_MM}_notes.zip", "first_key": FIRST_KEY, "ruling": "D7",
                   "cadence": "quarterly to 2025q2, monthly from 2025_07",
                   "terms": "SEC public data; EDGAR fair-access policy (declared user agent, <= 10 req/s; we cap at 5)",
                   "ledger": str(led.path), "ledger_sha256": C.sha256_file(led.path), "archive_copy": str(arch)},
        "data_sets": {k: {"url": recs[k]["url"], "bytes": recs[k]["bytes"], "sha256": recs[k]["sha256"],
                          "last_modified": recs[k].get("last_modified"), "span": recs[k]["span"],
                          "rows": recs[k]["rows"], "verify_ok": recs[k]["verify"]["ok"],
                          "peak_commit_gb": (recs[k].get("memory") or {}).get("peak_commit_gb")} for k in keys},
        "zip_bytes_landed": sum(recs[k]["bytes"] for k in keys),
        "rows": rows,
        "filters": {"txt_dei": "version LIKE 'dei/%'", "num_dei": "version LIKE 'dei/%'",
                    "num_items": f"form IN {PERIODIC_FORMS} AND ((version <> adsh AND tag ~ '{ITEM_TAG_RE}') OR "
                                 f"(dimensional AND segments has axis {SEGMENT_AXES}))"},
        "clock": "sub.accepted_utc = SEC 'accepted' (America/New_York wall clock) converted DST-aware to UTC",
        "runtime": "pure pyarrow streaming (no DuckDB), unguarded per controller ruling C-1; peak per data set above",
    }
    return C.write_stage_manifest(STAGE, SCHEMA, MODULES, payload)


def status() -> dict[str, Any]:
    latest = ledger().latest()
    parsed = sorted((k for k, v in latest.items() if v.get("status") == "parsed"), key=order_key)
    return {"parsed": len(parsed), "first": parsed[0] if parsed else None, "last": parsed[-1] if parsed else None,
            "zips_on_disk": sorted(p.name for p in RAW_DIR.glob("*_notes*.zip")), "free_gb": round(free_gb(), 1),
            "zip_bytes": sum(latest[k]["bytes"] for k in parsed),
            "parts_bytes": sum(p.stat().st_size for p in parts_dir().rglob("*.parquet"))}


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    f = sub.add_parser("fetch")
    f.add_argument("--limit", type=int)
    f.add_argument("--only")
    f.add_argument("--first", default=FIRST_KEY)
    sub.add_parser("finalize")
    sub.add_parser("status")
    args = ap.parse_args(argv)
    if args.cmd == "fetch":
        print(json.dumps(fetch(args.limit, args.only, args.first), default=str), flush=True)
    elif args.cmd == "finalize":
        print(finalize(), flush=True)
    else:
        print(json.dumps(status(), default=str), flush=True)
    return 0


if __name__ == "__main__":
    # os._exit: a failed parse must not hang at interpreter shutdown on pyarrow's reader thread (observed once)
    try:
        code = main()
    except BaseException:
        import traceback

        traceback.print_exc()
        code = 1
    sys.stdout.flush()
    sys.stderr.flush()
    os._exit(code)
