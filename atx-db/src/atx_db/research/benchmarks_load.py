"""Parse the external benchmark files (node X.3, gate U6 / RX14) into six Parquet datasets.

The raw files are downloaded by ``scripts/fetch_benchmarks.py`` into
``<data dir>/raw/benchmarks/<source>/<UTC date>/<file>`` with a ``<file>.receipt.json`` next to each one.
This module reads the newest date directory whose file still matches its receipt's sha256, parses it and
writes zstd Parquet to ``<data dir>/raw/benchmarks/parsed/<dataset>.parquet`` (temp file + rename) plus
``parsed/_manifest.json``. Every Parquet file carries its manifest entry in the Parquet footer key-value
metadata under ``atx.benchmark`` (``pq.read_metadata(path).metadata[b"atx.benchmark"]``).

Unit convention (all six datasets): **returns are stored as decimals** (0.0123 = 1.23 %). French, q5 and
OSAP publish percent, so their returns are converted with ``value / 100`` (``PERCENT_TO_DECIMAL``); JKP
publishes decimals and is stored unchanged. The French NYSE ME breakpoints are market equity in **USD
millions** (French: "NYSE ME percentile (divided by 1000000)") and are stored unchanged in ``*_musd``
columns. Every monthly dataset has a ``month_end`` DATE column = last calendar day of the month (and
``yyyymm`` INT32 where the source is keyed that way). No dataset has a ``year`` column: the research lake
partitions by ``year`` (SignalDoc's ``Year`` is stored as ``publication_year``).

A dataset whose inputs are missing (never fetched, or recorded ``*.unavailable.json``) is reported
``unavailable`` in the manifest with the reason; nothing is ever constructed by hand.

The research lake (node 1.9, ``research/research_lake.py``) registers these files later; see
``lake_registration_specs``.
"""

from __future__ import annotations

import csv
import datetime as dt
import hashlib
import io
import json
import os
import re
import zipfile
from collections.abc import Callable, Sequence
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

import numpy as np
import pyarrow as pa
import pyarrow.compute as pc
import pyarrow.csv as pacsv
import pyarrow.parquet as pq

LOADER_VERSION = "x3-benchmarks-v1"
METADATA_KEY = b"atx.benchmark"
PERCENT_TO_DECIMAL = "decimal = source_percent / 100"
UNIT_DECIMAL_RETURN = "decimal_return"
FRENCH_MISSING_SENTINELS = (-99.99, -999.0)
CSV_BLOCK_BYTES = 256 << 10
ROW_GROUP_ROWS = 131_072


def _csv_read_options() -> pacsv.ReadOptions:
    """Single-threaded, 256 KiB blocks: the streaming reader reads ahead many blocks (on PredictorPortsFull.csv
    4 MiB blocks measured 88 MB of Arrow pool, 1 MiB 38 MB, 256 KiB 47 MB with the whole write) -- keeps X.3
    inside its 0.2 GiB process target. Row groups are coalesced to ``ROW_GROUP_ROWS`` independently."""
    return pacsv.ReadOptions(use_threads=False, block_size=CSV_BLOCK_BYTES)


BENCH_FRENCH_FF5_UMD_MONTHLY = "bench_french_ff5_umd_monthly"
BENCH_FRENCH_ME_BREAKPOINTS = "bench_french_me_breakpoints"
BENCH_JKP_US_FACTORS = "bench_jkp_us_factors"
BENCH_OSAP_SIGNALDOC = "bench_osap_signaldoc"
BENCH_OSAP_PORTFOLIOS = "bench_osap_portfolios"
BENCH_Q5 = "bench_q5"
BENCHMARK_DATASETS: tuple[str, ...] = (
    BENCH_FRENCH_FF5_UMD_MONTHLY,
    BENCH_FRENCH_ME_BREAKPOINTS,
    BENCH_JKP_US_FACTORS,
    BENCH_OSAP_SIGNALDOC,
    BENCH_OSAP_PORTFOLIOS,
    BENCH_Q5,
)

_DATE_DIR = re.compile(r"^\d{4}-\d{2}-\d{2}$")
_FRENCH_KEY = re.compile(r"^\d{4}$|^\d{6}$|^\d{8}$")
_CRSP_VINTAGE = re.compile(r"(\d{6}) CRSP database")


class BenchmarkParseError(ValueError):
    """A downloaded file does not have the structure the parser expects."""


@dataclass(frozen=True)
class RawFile:
    source: str
    name: str
    path: Path
    receipt: dict[str, Any]

    def provenance(self) -> dict[str, Any]:
        return {
            "source": self.source,
            "file": self.name,
            "snapshot_dir": self.path.parent.name,
            "url": self.receipt.get("url"),
            "final_url": self.receipt.get("final_url"),
            "fetched_at_utc": self.receipt.get("fetched_at_utc"),
            "bytes": self.receipt.get("bytes"),
            "sha256": self.receipt.get("sha256"),
            "last_modified": (self.receipt.get("headers") or {}).get("Last-Modified"),
        }


@dataclass
class FrenchSection:
    title: str
    columns: tuple[str, ...]
    key_digits: int
    keys: list[int] = field(default_factory=list)
    values: list[list[float | None]] = field(default_factory=list)


def default_benchmarks_root() -> Path:
    """``<data dir>/raw/benchmarks`` (``ATX_DATA_DIR`` aware)."""
    from ..connection import resolve_data_dir

    return resolve_data_dir() / "raw" / "benchmarks"


def parsed_root(root: Path | None = None) -> Path:
    return (root or default_benchmarks_root()) / "parsed"


def benchmark_path(name: str, root: Path | None = None) -> Path:
    if name not in BENCHMARK_DATASETS:
        raise KeyError(f"unknown benchmark dataset {name!r}")
    return parsed_root(root) / f"{name}.parquet"


def _sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1 << 16), b""):
            digest.update(block)
    return digest.hexdigest()


def locate_raw(root: Path, source: str, name: str) -> RawFile | None:
    """Newest ``<source>/<YYYY-MM-DD>/<name>`` whose bytes match its receipt's sha256."""
    source_dir = root / source
    if not source_dir.is_dir():
        return None
    for date_dir in sorted((p for p in source_dir.iterdir() if p.is_dir() and _DATE_DIR.match(p.name)), reverse=True):
        path = date_dir / name
        receipt_path = date_dir / f"{name}.receipt.json"
        if not (path.is_file() and receipt_path.is_file()):
            continue
        receipt = json.loads(receipt_path.read_text(encoding="utf-8"))
        if receipt.get("sha256") == _sha256_file(path):
            return RawFile(source, name, path, receipt)
    return None


def _unavailable_reason(root: Path, source: str, name: str) -> str:
    source_dir = root / source
    if source_dir.is_dir():
        for date_dir in sorted((p for p in source_dir.iterdir() if p.is_dir()), reverse=True):
            evidence = date_dir / f"{name}.unavailable.json"
            if evidence.is_file():
                payload = json.loads(evidence.read_text(encoding="utf-8"))
                return f"{source}/{date_dir.name}/{name}: {payload.get('reason', 'unavailable')}"
    return f"{source}/{name}: no receipt-verified download under {source_dir}"


def _read_zip_member(raw: RawFile, suffix: str = ".csv") -> tuple[str, bytes]:
    with zipfile.ZipFile(raw.path) as archive:
        members = [info.filename for info in archive.infolist() if info.filename.lower().endswith(suffix)]
        if len(members) != 1:
            raise BenchmarkParseError(f"{raw.name}: expected one {suffix} member, found {members}")
        return members[0], archive.read(members[0])


def _month_end_from_months(months: np.ndarray) -> pa.Array:
    """``months`` is numpy ``datetime64[M]``; returns the last calendar day of each month as date32."""
    days = (months + np.timedelta64(1, "M")).astype("datetime64[D]") - np.timedelta64(1, "D")
    return pa.array(days, type=pa.date32())


def _month_end_from_yyyymm(keys: Sequence[int]) -> pa.Array:
    ints = np.asarray(keys, dtype=np.int64)
    months = ((ints // 100 - 1970) * 12 + (ints % 100 - 1)).astype("datetime64[M]")
    bad = (ints % 100 < 1) | (ints % 100 > 12)
    if bad.any():
        raise BenchmarkParseError(f"invalid YYYYMM keys: {ints[bad][:5].tolist()}")
    return _month_end_from_months(months)


def _month_end_from_dates(dates: pa.Array | pa.ChunkedArray) -> pa.Array:
    if dates.null_count:
        raise BenchmarkParseError("null dates in a monthly series")
    days = np.asarray(dates.to_numpy(zero_copy_only=False), dtype="datetime64[D]")
    return _month_end_from_months(days.astype("datetime64[M]"))


def _french_float(cell: str) -> float | None:
    value = float(cell)
    return None if value in FRENCH_MISSING_SENTINELS else value


def parse_french_sections(text: str) -> list[FrenchSection]:
    """Split a Ken French CSV into its sections (monthly, annual, daily ...).

    A header line has an empty first cell (``,Mkt-RF,SMB,...``); data lines start with a 4/6/8-digit key;
    every other non-blank line is free text that closes the open section and titles the next one.
    """
    sections: list[FrenchSection] = []
    pending: list[str] = []
    current: FrenchSection | None = None
    for number, raw_line in enumerate(text.splitlines(), start=1):
        line = raw_line.strip()
        if not line:
            continue
        cells = [cell.strip() for cell in line.split(",")]
        while cells and cells[-1] == "":
            cells.pop()
        if len(cells) > 1 and cells[0] == "" and all(cells[1:]):
            if current is not None:
                sections.append(current)
            current = FrenchSection(title=" ".join(pending), columns=tuple(cells[1:]), key_digits=0)
            pending = []
            continue
        if cells and _FRENCH_KEY.match(cells[0]):
            if current is None:
                raise BenchmarkParseError(f"line {number}: data row before any header")
            if len(cells) != len(current.columns) + 1:
                raise BenchmarkParseError(f"line {number}: {len(cells) - 1} values for {len(current.columns)} columns")
            digits = len(cells[0])
            if current.key_digits == 0:
                current.key_digits = digits
            elif digits != current.key_digits:
                raise BenchmarkParseError(
                    f"line {number}: key width {digits} inside a {current.key_digits}-digit section"
                )
            current.keys.append(int(cells[0]))
            current.values.append([_french_float(cell) for cell in cells[1:]])
            continue
        if current is not None:
            sections.append(current)
            current = None
        pending.append(line)
    if current is not None:
        sections.append(current)
    return sections


def _only_monthly(sections: list[FrenchSection], name: str) -> FrenchSection:
    monthly = [section for section in sections if section.key_digits == 6]
    if len(monthly) != 1:
        raise BenchmarkParseError(f"{name}: expected one monthly (YYYYMM) section, found {len(monthly)}")
    section = monthly[0]
    if section.keys != sorted(set(section.keys)):
        raise BenchmarkParseError(f"{name}: monthly keys are not strictly increasing")
    return section


def _crsp_vintage(text: str) -> str | None:
    match = _CRSP_VINTAGE.search(text)
    return match.group(1) if match else None


def _snake(name: str) -> str:
    """``SampleStartYear`` -> ``sample_start_year``, ``Cat.Signal`` -> ``cat_signal``, ``T-Stat`` -> ``t_stat``."""
    split = re.sub(r"(?<=[a-z0-9])(?=[A-Z])", "_", name.strip())
    return re.sub(r"[^0-9a-z]+", "_", split.lower()).strip("_")


# --------------------------------------------------------------------------------------------- French
def build_french_ff5_umd_monthly(ff5: RawFile, umd: RawFile) -> tuple[pa.Table, dict[str, Any]]:
    """FF5 2x3 monthly factors full-outer-joined with the monthly momentum factor on YYYYMM."""
    ff5_member, ff5_bytes = _read_zip_member(ff5)
    umd_member, umd_bytes = _read_zip_member(umd)
    ff5_text = ff5_bytes.decode("latin-1")
    umd_text = umd_bytes.decode("latin-1")
    ff5_sections = parse_french_sections(ff5_text)
    umd_sections = parse_french_sections(umd_text)
    ff5_month = _only_monthly(ff5_sections, ff5.name)
    umd_month = _only_monthly(umd_sections, umd.name)
    expected = ("Mkt-RF", "SMB", "HML", "RMW", "CMA", "RF")
    if ff5_month.columns != expected:
        raise BenchmarkParseError(f"{ff5.name}: columns {ff5_month.columns} != {expected}")
    if umd_month.columns != ("Mom",):
        raise BenchmarkParseError(f"{umd.name}: columns {umd_month.columns} != ('Mom',)")
    ff5_by_key = dict(zip(ff5_month.keys, ff5_month.values, strict=True))
    umd_by_key = dict(zip(umd_month.keys, umd_month.values, strict=True))
    keys = sorted(set(ff5_by_key) | set(umd_by_key))
    out_names = ("mkt_rf", "smb", "hml", "rmw", "cma", "rf")
    columns: dict[str, list[float | None]] = {name: [] for name in (*out_names, "umd")}
    for key in keys:
        row = ff5_by_key.get(key)
        for index, name in enumerate(out_names):
            value = None if row is None else row[index]
            columns[name].append(None if value is None else value / 100.0)
        mom = umd_by_key.get(key)
        columns["umd"].append(None if mom is None or mom[0] is None else mom[0] / 100.0)
    arrays: dict[str, pa.Array] = {
        "month_end": _month_end_from_yyyymm(keys),
        "yyyymm": pa.array(keys, type=pa.int32()),
    }
    arrays.update({name: pa.array(values, type=pa.float64()) for name, values in columns.items()})
    table = pa.table(arrays)
    info = {
        "unit": UNIT_DECIMAL_RETURN,
        "conversion": PERCENT_TO_DECIMAL,
        "join": "full outer join of the FF5 monthly section and the Mom monthly section on YYYYMM",
        "column_sources": {
            "mkt_rf": f"{ff5_member}:Mkt-RF",
            "smb": f"{ff5_member}:SMB",
            "hml": f"{ff5_member}:HML",
            "rmw": f"{ff5_member}:RMW",
            "cma": f"{ff5_member}:CMA",
            "rf": f"{ff5_member}:RF",
            "umd": f"{umd_member}:Mom",
        },
        "missing_sentinels_to_null": list(FRENCH_MISSING_SENTINELS),
        "crsp_vintage": {"ff5": _crsp_vintage(ff5_text), "umd": _crsp_vintage(umd_text)},
        "ff5_months": len(ff5_by_key),
        "umd_months": len(umd_by_key),
        "both_months": len(set(ff5_by_key) & set(umd_by_key)),
        "ff5_range_yyyymm": [ff5_month.keys[0], ff5_month.keys[-1]],
        "umd_range_yyyymm": [umd_month.keys[0], umd_month.keys[-1]],
        "excluded_sections": [
            {"file": member, "title": section.title[:120], "key_digits": section.key_digits, "rows": len(section.keys)}
            for member, sections in ((ff5_member, ff5_sections), (umd_member, umd_sections))
            for section in sections
            if section.key_digits != 6
        ],
    }
    return table, info


def build_french_me_breakpoints(raw: RawFile) -> tuple[pa.Table, dict[str, Any]]:
    """NYSE market-equity percentiles 5 %, 10 %, ..., 100 % per month, USD millions (no header row)."""
    member, payload = _read_zip_member(raw)
    text = payload.decode("latin-1")
    keys: list[int] = []
    counts: list[int] = []
    percentiles: list[list[float | None]] = [[] for _ in range(20)]
    non_positive = 0
    text_lines: list[str] = []
    for number, raw_line in enumerate(text.splitlines(), start=1):
        line = raw_line.strip()
        if not line:
            continue
        cells = [cell.strip() for cell in line.split(",")]
        while cells and cells[-1] == "":
            cells.pop()
        if not re.fullmatch(r"\d{6}", cells[0]):
            text_lines.append(line)
            continue
        if len(cells) != 22:
            raise BenchmarkParseError(f"{member} line {number}: {len(cells)} fields, expected 22")
        keys.append(int(cells[0]))
        counts.append(int(cells[1]))
        for index, cell in enumerate(cells[2:]):
            value = float(cell)
            if value <= 0:
                non_positive += 1
                percentiles[index].append(None)
            else:
                percentiles[index].append(value)
    if not keys or keys != sorted(set(keys)):
        raise BenchmarkParseError(f"{member}: no rows or keys not strictly increasing")
    arrays: dict[str, pa.Array] = {
        "month_end": _month_end_from_yyyymm(keys),
        "yyyymm": pa.array(keys, type=pa.int32()),
        "n_nyse_firms": pa.array(counts, type=pa.int32()),
    }
    names = [f"me_p{5 * (index + 1):02d}_musd" for index in range(20)]
    for name, values in zip(names, percentiles, strict=True):
        arrays[name] = pa.array(values, type=pa.float64())
    for index in range(1, 20):
        lower = np.asarray(percentiles[index - 1], dtype=float)
        upper = np.asarray(percentiles[index], dtype=float)
        if np.any(upper < lower):
            raise BenchmarkParseError(f"{member}: percentile {5 * (index + 1)} below {5 * index} in some month")
    info = {
        "unit": "usd_millions",
        "conversion": "none (French publishes NYSE ME / 1,000,000 = USD millions)",
        "timing": "percentiles of NYSE market equity at the end of month_end (French ME_Breakpoints)",
        "columns": {
            "n_nyse_firms": "number of NYSE firms",
            **{name: f"{name[4:6]}th NYSE ME percentile" for name in names},
        },
        "non_positive_values_to_null": non_positive,
        "header_text": " ".join(text_lines)[:400],
        "crsp_vintage": _crsp_vintage(text),
    }
    return pa.table(arrays), info


# --------------------------------------------------------------------------------------------- JKP
def build_jkp_us_factors(raw: RawFile) -> tuple[pa.Table, dict[str, Any]]:
    """JKP US factor returns, all factors, monthly, capped value weights (decimals, stored unchanged)."""
    member, payload = _read_zip_member(raw)
    table = pacsv.read_csv(
        io.BytesIO(payload),
        read_options=_csv_read_options(),
        convert_options=pacsv.ConvertOptions(
            column_types={
                "location": pa.string(),
                "name": pa.string(),
                "freq": pa.string(),
                "weighting": pa.string(),
                "direction": pa.int8(),
                "n_stocks": pa.int32(),
                "n_stocks_min": pa.int32(),
                "date": pa.date32(),
                "ret": pa.float64(),
            },
            null_values=["", "NA"],
        ),
    )
    expected = ["location", "name", "freq", "weighting", "direction", "n_stocks", "n_stocks_min", "date", "ret"]
    if table.column_names != expected:
        raise BenchmarkParseError(f"{member}: columns {table.column_names} != {expected}")
    month_end = _month_end_from_dates(table.column("date"))
    not_month_end = int(pc.sum(pc.not_equal(table.column("date"), month_end)).as_py() or 0)
    table = table.append_column("month_end", month_end)
    table = table.select(["month_end", "date", *expected[:7], "ret"])
    locations = sorted(set(table.column("location").to_pylist()))
    freqs = sorted(set(table.column("freq").to_pylist()))
    weights = sorted(set(table.column("weighting").to_pylist()))
    if locations != ["usa"] or freqs != ["monthly"] or weights != ["vw_cap"]:
        raise BenchmarkParseError(f"{member}: unexpected slice {locations} {freqs} {weights}")
    abs_ret = pc.abs(table.column("ret"))
    info = {
        "unit": UNIT_DECIMAL_RETURN,
        "conversion": "none (JKP publishes decimal returns)",
        "zip_member": member,
        "n_factors": len(set(table.column("name").to_pylist())),
        "date_not_calendar_month_end": not_month_end,
        "ret_nulls": table.column("ret").null_count,
        "ret_abs_max": pc.max(abs_ret).as_py(),
        "ret_abs_median": float(np.nanmedian(abs_ret.to_numpy(zero_copy_only=False).astype(float))),
        "direction_note": "JKP 'direction' is the sign applied so the factor is long the high-expected-return leg",
    }
    return table, info


# --------------------------------------------------------------------------------------------- OSAP
_SIGNALDOC_TYPES: dict[str, pa.DataType] = {
    "Year": pa.int32(),
    "SampleStartYear": pa.int32(),
    "SampleEndYear": pa.int32(),
    "Sign": pa.int8(),
    "Return": pa.float64(),
    "T-Stat": pa.float64(),
    "LS Quantile": pa.float64(),
    "Portfolio Period": pa.int32(),
    "Start Month": pa.int32(),
}


def build_osap_signaldoc(raw: RawFile) -> tuple[pa.Table, dict[str, Any]]:
    """OSAP ``SignalDoc.csv``: one row per signal with the original paper's reported return and t-stat."""
    with raw.path.open(encoding="utf-8", newline="") as handle:
        header = next(csv.reader(handle))
    column_types = {name: _SIGNALDOC_TYPES.get(name, pa.string()) for name in header}
    table = pacsv.read_csv(
        raw.path,
        read_options=_csv_read_options(),
        convert_options=pacsv.ConvertOptions(
            column_types=column_types, null_values=["", "NA"], strings_can_be_null=True
        ),
    )
    renamed: dict[str, str] = {}
    arrays: list[pa.Array | pa.ChunkedArray] = []
    # ``year`` is the research lake's partition column, so the OP publication year gets its own name.
    special = {"Return": "op_return", "Year": "publication_year"}
    for name in table.column_names:
        target = special.get(name) or _snake(name)
        if target in renamed.values():
            raise BenchmarkParseError(f"{raw.name}: column name collision on {target}")
        renamed[name] = target
        column = table.column(name)
        arrays.append(pc.divide(column, 100.0) if name == "Return" else column)
    out = pa.table(arrays, names=list(renamed.values()))
    acronyms = out.column("acronym")
    if acronyms.null_count or len(set(acronyms.to_pylist())) != out.num_rows:
        raise BenchmarkParseError(f"{raw.name}: Acronym is not a unique non-null key")
    categories: dict[str, int] = {}
    for value in out.column("cat_signal").to_pylist():
        categories[str(value)] = categories.get(str(value), 0) + 1
    info = {
        "unit": "op_return: decimal monthly return reported in the original paper; t_stat unchanged",
        "conversion": f"op_return: {PERCENT_TO_DECIMAL} (SignalDoc 'Return' is percent per month)",
        "source_columns": renamed,
        "cat_signal_counts": categories,
        "t_stat_nulls": out.column("t_stat").null_count,
        "sample_year_range": [
            pc.min(out.column("sample_start_year")).as_py(),
            pc.max(out.column("sample_end_year")).as_py(),
        ],
        "publication_year_range": [
            pc.min(out.column("publication_year")).as_py(),
            pc.max(out.column("publication_year")).as_py(),
        ],
    }
    return out, info


_PORTS_SCHEMA = pa.schema(
    [
        ("signalname", pa.string()),
        ("port", pa.string()),
        ("date", pa.date32()),
        ("month_end", pa.date32()),
        ("ret", pa.float64()),
        ("signallag", pa.float64()),
        ("n_long", pa.int32()),
        ("n_short", pa.int32()),
    ]
)


def write_osap_portfolios(
    raw: RawFile, out_path: Path, metadata: Callable[[dict[str, Any]], bytes]
) -> tuple[dict[str, Any], pa.Table]:
    """Stream ``PredictorPortsFull.csv`` (~1.2M rows) to Parquet, percent -> decimal, ~128k-row row groups.

    Returns the stream statistics and the LS rows (signalname, date, decimal ret) for the cross-check, so the
    written file is never re-read.
    """
    reader = pacsv.open_csv(
        raw.path,
        read_options=_csv_read_options(),
        convert_options=pacsv.ConvertOptions(
            column_types={
                "signalname": pa.string(),
                "port": pa.string(),
                "date": pa.date32(),
                "ret": pa.float64(),
                "signallag": pa.float64(),
                "Nlong": pa.int32(),
                "Nshort": pa.int32(),
            },
            null_values=["", "NA"],
        ),
    )
    expected = ["signalname", "port", "date", "ret", "signallag", "Nlong", "Nshort"]
    if reader.schema.names != expected:
        raise BenchmarkParseError(f"{raw.name}: columns {reader.schema.names} != {expected}")
    rows = 0
    ret_nulls = 0
    not_month_end = 0
    date_min: dt.date | None = None
    date_max: dt.date | None = None
    signals: set[str] = set()
    ports: dict[str, int] = {}
    abs_max = 0.0
    out_path.parent.mkdir(parents=True, exist_ok=True)
    tmp = out_path.with_name(out_path.name + ".tmp")
    writer = pq.ParquetWriter(tmp, _PORTS_SCHEMA, compression="zstd")
    pending: list[pa.RecordBatch] = []
    pending_rows = 0
    ls_batches: list[pa.RecordBatch] = []

    def flush() -> None:
        # write_table closes one row group per call; write_batch would buffer up to ~1M rows in memory.
        nonlocal pending_rows
        if pending:
            writer.write_table(pa.Table.from_batches(pending), row_group_size=pending_rows)
            pending.clear()
            pending_rows = 0

    try:
        for batch in reader:
            if batch.num_rows == 0:
                continue
            dates = batch.column("date")
            month_end = _month_end_from_dates(dates)
            ret = pc.divide(batch.column("ret"), 100.0)
            out = pa.RecordBatch.from_arrays(
                [
                    batch.column("signalname"),
                    batch.column("port"),
                    dates,
                    month_end,
                    ret,
                    batch.column("signallag"),
                    batch.column("Nlong"),
                    batch.column("Nshort"),
                ],
                schema=_PORTS_SCHEMA,
            )
            pending.append(out)
            pending_rows += out.num_rows
            if pending_rows >= ROW_GROUP_ROWS:
                flush()
            ls_batches.append(out.select(["signalname", "date", "ret"]).filter(pc.equal(out.column("port"), "LS")))
            rows += batch.num_rows
            ret_nulls += ret.null_count
            not_month_end += int(pc.sum(pc.not_equal(dates, month_end)).as_py() or 0)
            low, high = pc.min_max(month_end).values()
            date_min = low.as_py() if date_min is None else min(date_min, low.as_py())
            date_max = high.as_py() if date_max is None else max(date_max, high.as_py())
            signals.update(pc.unique(batch.column("signalname")).to_pylist())
            for item in pc.value_counts(batch.column("port")).to_pylist():
                ports[item["values"]] = ports.get(item["values"], 0) + item["counts"]
            batch_max = pc.max(pc.abs(ret)).as_py()
            abs_max = max(abs_max, batch_max or 0.0)
        flush()
        stats = {
            "rows": rows,
            "date_min": None if date_min is None else date_min.isoformat(),
            "date_max": None if date_max is None else date_max.isoformat(),
            "n_signals": len(signals),
            "port_counts": dict(sorted(ports.items())),
            "ret_nulls": ret_nulls,
            "ret_abs_max": abs_max,
            "date_not_calendar_month_end": not_month_end,
        }
        writer.add_key_value_metadata({METADATA_KEY: metadata(stats)})
    except BaseException:
        writer.close()
        tmp.unlink(missing_ok=True)
        raise
    writer.close()
    os.replace(tmp, out_path)
    ls_rows = pa.Table.from_batches(
        ls_batches, schema=pa.schema([_PORTS_SCHEMA.field(n) for n in ("signalname", "date", "ret")])
    )
    return stats, ls_rows


def _osap_ls_cross_check(ls: pa.Table, wide: RawFile) -> dict[str, Any]:
    """Compare the LS rows of the portfolio file with ``PredictorLSretWide.csv`` (both percent at source)."""
    wide_table = pacsv.read_csv(
        wide.path,
        read_options=_csv_read_options(),
        convert_options=pacsv.ConvertOptions(column_types={"date": pa.date32()}, null_values=["", "NA"]),
    )
    names: list[str] = []
    dates: list[pa.Array] = []
    values: list[pa.Array] = []
    for column in wide_table.column_names:
        if column == "date":
            continue
        data = wide_table.column(column).cast(pa.float64())
        keep = pc.is_valid(data)
        kept = pc.filter(data, keep)
        names.extend([column] * len(kept))
        dates.append(pc.filter(wide_table.column("date"), keep).combine_chunks())
        values.append(pc.divide(kept, 100.0).combine_chunks())
    melted = pa.table(
        {
            "signalname": pa.array(names, type=pa.string()),
            "date": pa.concat_arrays(dates),
            "ret_wide": pa.concat_arrays(values),
        }
    )
    ls_valid = ls.filter(pc.is_valid(ls.column("ret")))
    joined = ls_valid.join(melted, keys=["signalname", "date"], join_type="inner", use_threads=False)
    diff = pc.abs(pc.subtract(joined.column("ret"), joined.column("ret_wide")))
    return {
        "wide_file": wide.name,
        "wide_sha256": wide.receipt.get("sha256"),
        "wide_signals": wide_table.num_columns - 1,
        "wide_nonnull_values": melted.num_rows,
        "ls_nonnull_rows": ls_valid.num_rows,
        "matched": joined.num_rows,
        "max_abs_diff_decimal": pc.max(diff).as_py() if joined.num_rows else None,
    }


# --------------------------------------------------------------------------------------------- global-q
def build_q5(raw: RawFile) -> tuple[pa.Table, dict[str, Any]]:
    """global-q q5 monthly factors: R_F, R_MKT (market excess return), R_ME, R_IA, R_ROE, R_EG; percent -> decimal."""
    table = pacsv.read_csv(
        raw.path, read_options=_csv_read_options(), convert_options=pacsv.ConvertOptions(null_values=["", "NA"])
    )
    expected = ["year", "month", "R_F", "R_MKT", "R_ME", "R_IA", "R_ROE", "R_EG"]
    if table.column_names != expected:
        raise BenchmarkParseError(f"{raw.name}: columns {table.column_names} != {expected}")
    years = np.asarray(table.column("year").to_numpy(zero_copy_only=False), dtype=np.int64)
    months = np.asarray(table.column("month").to_numpy(zero_copy_only=False), dtype=np.int64)
    keys = (years * 100 + months).tolist()
    if keys != sorted(set(keys)):
        raise BenchmarkParseError(f"{raw.name}: (year, month) not strictly increasing")
    # No ``year``/``month`` columns: ``year`` is the research lake's partition column (yyyymm carries both).
    arrays: dict[str, pa.Array] = {
        "month_end": _month_end_from_yyyymm(keys),
        "yyyymm": pa.array(keys, type=pa.int32()),
    }
    for name in expected[2:]:
        arrays[name.lower()] = pc.divide(table.column(name).cast(pa.float64()), 100.0).combine_chunks()
    discovery = raw.receipt.get("discovery") or {}
    info = {
        "unit": UNIT_DECIMAL_RETURN,
        "conversion": PERCENT_TO_DECIMAL,
        "column_sources": {name.lower(): name for name in expected[2:]},
        "r_mkt_note": "global-q R_MKT is the market excess return (value-weighted market minus R_F)",
        "release_year": discovery.get("release_year"),
        "upstream_file": (raw.receipt.get("final_url") or "").rsplit("/", 1)[-1],
    }
    return pa.table(arrays), info


# --------------------------------------------------------------------------------------------- driver
def _date_range(table: pa.Table, column: str) -> tuple[str | None, str | None]:
    if column not in table.column_names or table.num_rows == 0:
        return None, None
    low, high = pc.min_max(table.column(column)).values()
    return (None if low.as_py() is None else str(low.as_py()), None if high.as_py() is None else str(high.as_py()))


def _schema_listing(schema: pa.Schema) -> list[list[str]]:
    return [[item.name, str(item.type)] for item in schema]


def _write_table(table: pa.Table, path: Path, entry: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(path.name + ".tmp")
    stamped = table.replace_schema_metadata({METADATA_KEY: json.dumps(entry, sort_keys=True, default=str).encode()})
    pq.write_table(stamped, tmp, compression="zstd")
    os.replace(tmp, path)


def _utc_now_iso() -> str:
    return dt.datetime.now(dt.UTC).replace(microsecond=0).strftime("%Y-%m-%dT%H:%M:%SZ")


_INPUTS: dict[str, tuple[tuple[str, str], ...]] = {
    BENCH_FRENCH_FF5_UMD_MONTHLY: (
        ("french", "F-F_Research_Data_5_Factors_2x3_CSV.zip"),
        ("french", "F-F_Momentum_Factor_CSV.zip"),
    ),
    BENCH_FRENCH_ME_BREAKPOINTS: (("french", "ME_Breakpoints_CSV.zip"),),
    BENCH_JKP_US_FACTORS: (("jkp", "usa_all_factors_monthly_vw_cap.zip"),),
    BENCH_OSAP_SIGNALDOC: (("osap", "SignalDoc.csv"),),
    BENCH_OSAP_PORTFOLIOS: (("osap", "PredictorPortsFull.csv"),),
    BENCH_Q5: (("globalq", "q5_factors_monthly.csv"),),
}


def _build_one(name: str, root: Path, inputs: list[RawFile], parsed_at: str) -> dict[str, Any]:
    path = benchmark_path(name, root)
    base: dict[str, Any] = {
        "dataset": name,
        "status": "present",
        "path": str(path),
        "loader_version": LOADER_VERSION,
        "parsed_at_utc": parsed_at,
        "sources": [raw.provenance() for raw in inputs],
    }
    if name == BENCH_OSAP_PORTFOLIOS:
        info_base = {
            "unit": UNIT_DECIMAL_RETURN,
            "conversion": f"ret: {PERCENT_TO_DECIMAL}",
            "date_note": "date is OSAP's month-end trading date; month_end is the calendar month end",
            "port_note": "port 01..10 are quantile portfolios (low..high signal), LS is long-short per the OP",
        }

        def metadata(stats: dict[str, Any]) -> bytes:
            entry = base | {"rows": stats["rows"], "date_min": stats["date_min"], "date_max": stats["date_max"]}
            entry["info"] = info_base | {"stats": stats}
            entry["schema"] = _schema_listing(_PORTS_SCHEMA)
            return json.dumps(entry, sort_keys=True, default=str).encode()

        stats, ls_rows = write_osap_portfolios(inputs[0], path, metadata)
        pa.default_memory_pool().release_unused()
        info = info_base | {"stats": stats}
        wide = locate_raw(root, "osap", "PredictorLSretWide.csv")
        info["ls_cross_check"] = (
            _osap_ls_cross_check(ls_rows, wide) if wide is not None else {"status": "wide file unavailable"}
        )
        del ls_rows
        entry = base | {"rows": stats["rows"], "date_min": stats["date_min"], "date_max": stats["date_max"]}
        entry["schema"] = _schema_listing(_PORTS_SCHEMA)
        entry["info"] = info
    else:
        builders: dict[str, Callable[..., tuple[pa.Table, dict[str, Any]]]] = {
            BENCH_FRENCH_FF5_UMD_MONTHLY: build_french_ff5_umd_monthly,
            BENCH_FRENCH_ME_BREAKPOINTS: build_french_me_breakpoints,
            BENCH_JKP_US_FACTORS: build_jkp_us_factors,
            BENCH_OSAP_SIGNALDOC: build_osap_signaldoc,
            BENCH_Q5: build_q5,
        }
        table, info = builders[name](*inputs)
        if "year" in table.column_names:
            raise BenchmarkParseError(f"{name}: 'year' is reserved for the research lake partition column")
        date_min, date_max = _date_range(table, "month_end")
        entry = base | {"rows": table.num_rows, "date_min": date_min, "date_max": date_max}
        entry["schema"] = _schema_listing(table.schema)
        entry["info"] = info
        _write_table(table, path, entry)
    entry["parquet_bytes"] = path.stat().st_size
    entry["parquet_sha256"] = _sha256_file(path)
    return entry


def build_benchmark_datasets(root: Path | None = None) -> dict[str, dict[str, Any]]:
    """Parse every available input into ``parsed/<dataset>.parquet`` and write ``parsed/_manifest.json``.

    Returns the manifest entries keyed by dataset. A dataset with a missing input is ``unavailable`` (its stale
    Parquet, if any, is removed so no reader picks it up); a parse failure is ``error`` with the message.
    """
    root = (root or default_benchmarks_root()).resolve()
    parsed = parsed_root(root)
    parsed.mkdir(parents=True, exist_ok=True)
    parsed_at = _utc_now_iso()
    manifest: dict[str, dict[str, Any]] = {}
    for name in BENCHMARK_DATASETS:
        inputs: list[RawFile] = []
        missing: list[str] = []
        for source, file_name in _INPUTS[name]:
            raw = locate_raw(root, source, file_name)
            if raw is None:
                missing.append(_unavailable_reason(root, source, file_name))
            else:
                inputs.append(raw)
        path = benchmark_path(name, root)
        if missing:
            path.unlink(missing_ok=True)
            manifest[name] = {"dataset": name, "status": "unavailable", "reason": "; ".join(missing), "rows": 0}
            continue
        try:
            manifest[name] = _build_one(name, root, inputs, parsed_at)
        except (BenchmarkParseError, pa.ArrowInvalid, KeyError, ValueError) as exc:
            path.unlink(missing_ok=True)
            manifest[name] = {"dataset": name, "status": "error", "reason": f"{type(exc).__name__}: {exc}", "rows": 0}
        pa.default_memory_pool().release_unused()
    payload = {"loader_version": LOADER_VERSION, "parsed_at_utc": parsed_at, "root": str(root), "datasets": manifest}
    tmp = parsed / "_manifest.json.tmp"
    tmp.write_text(json.dumps(payload, indent=2, sort_keys=True, default=str) + "\n", encoding="utf-8")
    os.replace(tmp, parsed / "_manifest.json")
    return manifest


def load_manifest(root: Path | None = None) -> dict[str, Any]:
    return json.loads((parsed_root(root) / "_manifest.json").read_text(encoding="utf-8"))


def read_benchmark(name: str, root: Path | None = None, columns: Sequence[str] | None = None) -> pa.Table:
    """Read one parsed dataset (small tables; ``bench_osap_portfolios`` ~1.2M rows -- pass ``columns``/filter)."""
    path = benchmark_path(name, root)
    if not path.is_file():
        entry = load_manifest(root)["datasets"].get(name, {})
        raise FileNotFoundError(f"{name} is {entry.get('status', 'missing')}: {entry.get('reason', path)}")
    return pq.read_table(path, columns=list(columns) if columns is not None else None)


def lake_registration_specs(root: Path | None = None) -> list[dict[str, Any]]:
    """What the research lake (node 1.9 / 1.12) needs to register each present dataset."""
    manifest = load_manifest(root)["datasets"]
    specs: list[dict[str, Any]] = []
    for name in BENCHMARK_DATASETS:
        entry = manifest.get(name, {})
        if entry.get("status") != "present":
            continue
        specs.append(
            {
                "dataset": name,
                "path": entry["path"],
                "parquet_sha256": entry["parquet_sha256"],
                "rows": entry["rows"],
                "date_min": entry.get("date_min"),
                "date_max": entry.get("date_max"),
                "select": f"SELECT * FROM read_parquet('{Path(entry['path']).as_posix()}')",
            }
        )
    return specs


__all__ = [
    "BENCHMARK_DATASETS",
    "PERCENT_TO_DECIMAL",
    "BenchmarkParseError",
    "FrenchSection",
    "RawFile",
    "benchmark_path",
    "build_benchmark_datasets",
    "build_french_ff5_umd_monthly",
    "build_french_me_breakpoints",
    "build_jkp_us_factors",
    "build_osap_signaldoc",
    "build_q5",
    "default_benchmarks_root",
    "lake_registration_specs",
    "load_manifest",
    "locate_raw",
    "parse_french_sections",
    "parsed_root",
    "read_benchmark",
    "write_osap_portfolios",
]
