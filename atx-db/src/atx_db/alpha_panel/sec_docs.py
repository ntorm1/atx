"""Per-filing EDGAR document landing for lane OWN (stages ``stakes`` and ``insider_ext``).

No SEC bulk data set exists for Schedules 13D/13G or Form 144 (the SEC Data Library lists none), so the primary
document of each selected filing is fetched once:

* the filing list comes from the published ``sec_filings/filings.parquet`` (one row per (cik, accession); the
  smallest CIK names the archive folder, every CIK of a filing serves the same folder);
* URL ``https://www.sec.gov/Archives/edgar/data/<cik>/<accession without dashes>/<document>`` where ``document`` is
  ``primary_doc.xml`` for the structured forms (the raw XML behind the ``xsl...`` rendering) and the filing's
  ``primary_document`` otherwise;
* fetches go through :class:`atx_db.sec_http.FetchLedgerStore` under ``data/raw/<source>/``: gzip objects at rest
  (``objects/<sha[:2]>/<sha256>.gz``) and ``fetch-ledger.jsonl`` (url, status, sha256, bytes, fetched_at) as the
  receipt ledger; terminal outcomes are never refetched, so a run resumes where it stopped. Every attempt takes a
  token of the host-wide 5 req/s limiter; ``max_new`` bounds the new requests of one run (lane budget).

Also here: reading single members of SEC data-set zips by HTTP range (:func:`zip_directory`, :func:`get_range`,
:func:`inflate_range_file`), used by the ADV and N-PORT landings.
"""

from __future__ import annotations

import concurrent.futures as cf
import csv
import datetime as dt
import time
from pathlib import Path
from typing import Any

from . import common as C
from . import shortflow_common as S

ARCHIVES = "https://www.sec.gov/Archives/edgar/data"
MAX_DOC_BYTES = 20 * 1024 ** 2


def doc_url(cik: int, accession: str, document: str) -> str:
    return f"{ARCHIVES}/{int(cik)}/{accession.replace('-', '')}/{document}"


def structured_document(primary_document: str | None) -> str | None:
    """``xslSCHEDULE_13D_X01/primary_doc.xml`` -> ``primary_doc.xml`` (the raw XML); other names unchanged."""
    if not primary_document:
        return None
    return primary_document.rsplit("/", 1)[-1] if primary_document.lower().startswith("xsl") else primary_document


def scan_row_groups(path, columns: list[str], mask=None):
    """Yield one filtered table per parquet row group (``mask(table) -> boolean array``), single-threaded.

    Replaces ``pyarrow.dataset`` scans with ``use_threads=False``, which stalled for > 10 minutes on the 18.6M-row
    ``sec_filings/filings.parquet`` (2026-09-30) while plain row-group reads take 0.1 s each."""
    import pyarrow.parquet as pq

    pf = pq.ParquetFile(path)
    for i in range(pf.metadata.num_row_groups):
        t = pf.read_row_group(i, columns=columns, use_threads=False)
        if mask is not None:
            t = t.filter(mask(t))
        if t.num_rows:
            yield t


def read_filtered(path, columns: list[str], mask=None):
    """All row groups of :func:`scan_row_groups` concatenated (only the kept rows are ever held together)."""
    import pyarrow as pa
    import pyarrow.parquet as pq

    parts = list(scan_row_groups(path, columns, mask))
    if parts:
        return pa.concat_tables(parts)
    return pq.read_schema(path).empty_table().select(columns)


def sic_ciks():
    """EDGAR CIKs whose issuer profile carries a SIC code (issuers; reporting persons rarely have one)."""
    import pyarrow.compute as pc

    def m(t):
        s = t["sic"]
        return pc.fill_null(pc.and_(pc.not_equal(s, "0000"), pc.not_equal(s, "")), False)
    return read_filtered(C.build_root() / "sec_filings" / "issuer_profile.parquet", ["cik", "sic"], m)["cik"]


def forms_mask(forms: tuple[str, ...], start: dt.date, end: dt.date | None = None):
    import pyarrow as pa
    import pyarrow.compute as pc

    fs = pa.array(list(forms), pa.string())

    def m(t):
        k = pc.and_(pc.is_in(t["form"], value_set=fs), pc.greater_equal(t["filing_date"], pa.scalar(start, pa.date32())))
        if end is not None:
            k = pc.and_(k, pc.less_equal(t["filing_date"], pa.scalar(end, pa.date32())))
        return pc.fill_null(k, False)
    return m


def select_filings(forms: tuple[str, ...], start: dt.date, end: dt.date | None = None) -> list[tuple]:
    """``[(accession, cik, form, filing_date, primary_document, available_at, ciks)]`` newest first, one per accession.

    pyarrow only (streamed row groups; no DuckDB), so a landing can run unguarded under ruling C-1."""
    path = C.build_root() / "sec_filings" / "filings.parquet"
    cols = ["accession", "cik", "form", "filing_date", "primary_document", "available_at"]
    acc: dict[str, list[Any]] = {}
    for batch in scan_row_groups(path, cols, forms_mask(forms, start, end)):
        for a, cik, form, fd, pdoc, av in zip(*(batch.column(c).to_pylist() for c in cols), strict=True):
            cur = acc.get(a)
            if cur is None:
                acc[a] = [a, cik, form, fd, pdoc, av, [cik]]
                continue
            cur[6].append(cik)
            if cik < cur[1]:
                cur[1], cur[4] = cik, pdoc
            if fd < cur[3]:
                cur[3] = fd
            if av is not None and (cur[5] is None or av < cur[5]):
                cur[5] = av
    rows = []
    while acc:  # move entries out one at a time so the dict and the list never both hold everything
        _k, r = acc.popitem()
        rows.append((r[0], r[1], r[2], r[3], r[4], r[5], tuple(sorted(set(r[6])))))
    rows.sort(key=lambda r: (r[3], r[0]), reverse=True)
    return rows


def start_memory_trace() -> None:
    """Start tracemalloc (ruling C-1: an unguarded run records its measured peak)."""
    import tracemalloc

    if not tracemalloc.is_tracing():
        tracemalloc.start()


def peak_memory_gb() -> dict[str, float | None]:
    """Measured peaks of an unguarded run: Python heap (tracemalloc) and the pyarrow memory pool high-water mark."""
    import tracemalloc

    import pyarrow as pa

    py = tracemalloc.get_traced_memory()[1] / 1024 ** 3 if tracemalloc.is_tracing() else None
    return {"python_heap_peak_gb": round(py, 4) if py is not None else None,
            "pyarrow_pool_peak_gb": round(pa.default_memory_pool().max_memory() / 1024 ** 3, 4)}


def store(source: str):
    from atx_db.sec_http import FetchLedgerStore

    root = S.RAW_ROOT / source
    root.mkdir(parents=True, exist_ok=True)
    return FetchLedgerStore(root).load()


def fetch_urls(source: str, urls: list[str], max_new: int, threads: int = 2, progress_every: int = 500) -> dict[str, Any]:
    """Ensure every URL has a terminal record (at most ``max_new`` new fetches); returns counts."""
    from atx_db.sec_http import default_sec_limiter

    st = store(source)
    st.sweep_stale_temp_files()
    limiter = default_sec_limiter()
    todo = [u for u in urls if st.lookup(u) is None]
    todo = todo[:max_new]
    stats = {"urls": len(urls), "already_terminal": len(urls) - len([u for u in urls if st.lookup(u) is None]),
             "attempted": 0, "ok": 0, "not_found": 0, "other": 0}
    t0 = time.perf_counter()

    def one(u: str):
        rec, _ = st.ensure(u, limiter=limiter, timeout=60, maximum=MAX_DOC_BYTES)
        return rec

    with cf.ThreadPoolExecutor(max_workers=threads) as ex:
        for i, rec in enumerate(ex.map(one, todo), 1):
            stats["attempted"] += 1
            if rec.ok:
                stats["ok"] += 1
            elif rec.status == 404:
                stats["not_found"] += 1
            else:
                stats["other"] += 1
            if i % progress_every == 0 or i == len(todo):
                el = time.perf_counter() - t0
                print(f"  {source}: {i}/{len(todo)} ok={stats['ok']} 404={stats['not_found']} other={stats['other']} "
                      f"{i / max(el, 1e-9):.2f}/s", flush=True)
    return stats


def read_doc(source_store, url: str) -> bytes | None:
    rec = source_store.lookup(url)
    if rec is None or not rec.ok:
        return None
    return source_store.read(rec)


def ledger_counts(source: str) -> dict[str, Any]:
    """Requests and outcomes recorded in one source's fetch ledger (every line is one attempt's final record)."""
    import json

    p = S.RAW_ROOT / source / "fetch-ledger.jsonl"
    out = {"lines": 0, "status": {}, "bytes": 0}
    if not p.exists():
        return out
    with p.open(encoding="utf-8") as fh:
        for line in fh:
            try:
                r = json.loads(line)
            except ValueError:
                continue
            out["lines"] += 1
            out["status"][str(r.get("status"))] = out["status"].get(str(r.get("status")), 0) + 1
            out["bytes"] += int(r.get("bytes") or 0)
    return out


def objects_bytes(source: str) -> int:
    root = S.RAW_ROOT / source / "objects"
    return sum(p.stat().st_size for p in root.rglob("*.gz")) if root.exists() else 0


def raw_root(source: str) -> Path:
    return S.RAW_ROOT / source


# ---------------------------------------------------------------- zip members by HTTP range
# SEC data-set zips are served with byte-range support (206). One ranged GET of the zip tail yields the central
# directory; one ranged GET per member yields its local header and deflated bytes, streamed to disk as served.
TAIL_BYTES = 2_000_000
_SESSION: list[Any] = []


def session():
    from atx_db.sec_http import sec_session

    if not _SESSION:
        _SESSION.append(sec_session())
    return _SESSION[0]


def head(url: str) -> tuple[int, dict[str, str]]:
    r = session().head(url, timeout=60, allow_redirects=True)
    try:
        return int(r.status_code), {k.lower(): v for k, v in r.headers.items()}
    finally:
        r.close()


def get_range(url: str, lo: int, hi: int, dest: Path | None = None, timeout: float = 1800.0) -> tuple[int, bytes, dict[str, str]]:
    """Ranged GET of bytes [lo, hi]; streamed to ``dest`` (``.partial`` then rename, body ``b''``) when given."""
    import hashlib
    import os

    r = session().get(url, headers={"Range": f"bytes={lo}-{hi}"}, timeout=timeout, stream=dest is not None)
    try:
        hdr = {k.lower(): v for k, v in r.headers.items()}
        st = int(r.status_code)
        if dest is None or st != 206:
            return st, (r.content if st == 206 else b""), hdr
        tmp = dest.with_name(dest.name + ".partial")
        dest.parent.mkdir(parents=True, exist_ok=True)
        h = hashlib.sha256()
        n = 0
        with tmp.open("wb") as fh:
            for chunk in r.iter_content(chunk_size=8 << 20):
                if chunk:
                    fh.write(chunk)
                    h.update(chunk)
                    n += len(chunk)
        if n != hi - lo + 1:
            tmp.unlink()
            raise RuntimeError(f"{url}: range short ({n} of {hi - lo + 1} bytes)")
        os.replace(tmp, dest)
        hdr["x-sha256"] = h.hexdigest()
        hdr["x-bytes"] = str(n)
        return st, b"", hdr
    finally:
        r.close()


def central_directory(tail: bytes) -> list[dict[str, Any]]:
    """Central-directory entries of a (non-zip64) zip from its last bytes: name, method, crc, sizes, offset."""
    import struct

    k = tail.rfind(b"PK\x05\x06")
    if k < 0:
        raise ValueError("end of central directory not found in tail")
    n_entries, cd_size = struct.unpack("<HI", tail[k + 10:k + 16])
    p = k - cd_size
    if p < 0:
        raise ValueError("central directory larger than tail")
    out = []
    for _ in range(n_entries):
        if tail[p:p + 4] != b"PK\x01\x02":
            raise ValueError(f"bad central directory entry at {p}")
        method, _t, _d, crc, csize, usize, nl, el, cl = struct.unpack("<HHHIIIHHH", tail[p + 10:p + 34])
        off = struct.unpack("<I", tail[p + 42:p + 46])[0]
        name = tail[p + 46:p + 46 + nl].decode("utf-8", "replace")
        out.append({"name": name, "method": method, "crc": crc, "csize": csize, "usize": usize, "offset": off})
        p += 46 + nl + el + cl
    return out


def zip_directory(url: str) -> tuple[list[dict[str, Any]], dict[str, Any]]:
    """``(entries, info)`` of a remote zip: HEAD for size / Last-Modified, one ranged GET of the tail."""
    st, hdr = head(url)
    if st != 200:
        raise RuntimeError(f"{url}: HEAD {st}")
    size = int(hdr["content-length"])
    lo = max(size - TAIL_BYTES, 0)
    st, body, _h2 = get_range(url, lo, size - 1)
    if st != 206:
        raise RuntimeError(f"{url}: ranged GET {st}")
    import hashlib

    return central_directory(body), {"zip_bytes": size, "last_modified": hdr.get("last-modified"),
                                     "etag": hdr.get("etag"), "tail_range": f"bytes={lo}-{size - 1}",
                                     "tail_sha256": hashlib.sha256(body).hexdigest()}


def member_range(entry: dict[str, Any], zip_bytes: int) -> tuple[int, int]:
    """Byte range covering the member's local header (name + up to 1 KiB extra) and its deflated data."""
    lo = entry["offset"]
    return lo, min(lo + 30 + len(entry["name"].encode()) + 1024 + entry["csize"], zip_bytes) - 1


def inflate_range_file(src: Path, entry: dict[str, Any], dest: Path) -> int:
    """Served member range (starting at its local header) -> inflated ``dest``; streaming, CRC-32 and size checked."""
    import os
    import struct
    import zlib

    with src.open("rb") as fh:
        head_ = fh.read(30)
        if head_[:4] != b"PK\x03\x04":
            raise ValueError(f"{entry['name']}: no local header at range start")
        nl, el = struct.unpack("<HH", head_[26:30])
        fh.seek(30 + nl + el)
        left = entry["csize"]
        dec = zlib.decompressobj(-15) if entry["method"] == 8 else None
        crc, n = 0, 0
        tmp = dest.with_name(dest.name + ".partial")
        dest.parent.mkdir(parents=True, exist_ok=True)
        try:
            with tmp.open("wb") as out:
                while left > 0:
                    chunk = fh.read(min(1 << 20, left))
                    if not chunk:
                        break
                    left -= len(chunk)
                    data = dec.decompress(chunk) if dec else chunk
                    crc = zlib.crc32(data, crc)
                    n += len(data)
                    out.write(data)
                if dec:
                    data = dec.flush()
                    crc = zlib.crc32(data, crc)
                    n += len(data)
                    out.write(data)
        except zlib.error as exc:
            tmp.unlink(missing_ok=True)
            raise ValueError(f"{entry['name']}: inflate failed ({exc})") from exc
    if left != 0 or crc != entry["crc"] or n != entry["usize"]:
        tmp.unlink()
        raise ValueError(f"{entry['name']}: CRC/size mismatch ({crc:08x}/{n} vs {entry['crc']:08x}/{entry['usize']})")
    os.replace(tmp, dest)
    return n


def csv_to_parquet_stream(src: Path, dest: Path, *, delimiter: str = ",", quoted: bool = True, encoding: str = "utf8",
                          columns: list[str] | None = None, row_filter: Any = None, rename: Any = None,
                          block_size: int = 4 << 20, row_group_size: int = 16384) -> dict[str, Any]:
    """Stream a delimited file into Parquet with pyarrow only (no DuckDB): every column read as a string.

    ``columns`` keeps only those header names; ``row_filter(batch) -> boolean mask`` drops rows; ``rename(name)``
    maps header names to output names (duplicates get ``_2``, ``_3``). Rows that do not parse are skipped and
    counted. Peak memory stays near one block (``block_size``) plus one row group."""
    import os

    import pyarrow as pa
    import pyarrow.csv as pacsv
    import pyarrow.parquet as pq

    with src.open("r", encoding="utf-8" if encoding == "utf8" else "latin-1", errors="replace", newline="") as fh:
        first = fh.readline().rstrip("\r\n")
    raw_names = next(csv.reader([first], delimiter=delimiter)) if quoted else first.split(delimiter)
    names: list[str] = []
    seen: dict[str, int] = {}
    for h in raw_names:
        base = (rename(h) if rename else h.strip())
        seen[base] = seen.get(base, 0) + 1
        names.append(base if seen[base] == 1 else f"{base}_{seen[base]}")
    keep = None
    if columns is not None:
        want = {(rename(c) if rename else c) for c in columns}
        keep = [n for n in names if n in want]
    bad = {"n": 0}
    width = len(keep or names)
    if width > 60:  # wide tables: keep one buffered row group near the size of a 60-column one
        row_group_size = max(2048, row_group_size * 60 // width)
        block_size = max(1 << 20, block_size * 60 // width)

    def skip(_row):
        bad["n"] += 1
        return "skip"

    ropts = pacsv.ReadOptions(block_size=block_size, column_names=names, skip_rows=1, encoding=encoding, use_threads=False)
    popts = pacsv.ParseOptions(delimiter=delimiter, quote_char='"' if quoted else False, newlines_in_values=quoted,
                               invalid_row_handler=skip)
    copts = pacsv.ConvertOptions(column_types={n: pa.string() for n in names}, include_columns=keep,
                                 strings_can_be_null=True, check_utf8=False)
    tmp = dest.with_name(dest.name + ".partial")
    dest.parent.mkdir(parents=True, exist_ok=True)
    rows = 0
    writer = None
    try:
        with pacsv.open_csv(src, read_options=ropts, parse_options=popts, convert_options=copts) as rd:
            for batch in rd:
                if row_filter is not None:
                    batch = batch.filter(row_filter(batch))
                if writer is None:
                    writer = pq.ParquetWriter(tmp, batch.schema, compression="zstd")
                if batch.num_rows:
                    writer.write_table(pa.Table.from_batches([batch]), row_group_size=row_group_size)
                    rows += batch.num_rows
    finally:
        if writer is not None:
            writer.close()
    if writer is None:
        raise ValueError(f"{src}: no data")
    os.replace(tmp, dest)
    return {"rows": rows, "skipped_rows": bad["n"], "columns": len(keep or names)}
