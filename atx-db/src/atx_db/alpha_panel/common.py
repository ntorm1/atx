"""Shared paths, windows, and publication helpers for the alpha panel build.

See ``docs/ALPHA_PANEL.md`` for the stage contract.
"""

from __future__ import annotations

import datetime as dt
import hashlib
import json
import os
import time
from collections.abc import Iterator
from contextlib import contextmanager
from pathlib import Path
from typing import Any

import duckdb

PACKAGE_ROOT = Path(__file__).resolve().parents[3]  # .../atx-db
DEFAULT_ROOT = PACKAGE_ROOT / "data" / "alpha_panel" / "v1"

TICKERHISTORY = Path(os.environ.get("ATX_TICKERHISTORY", r"C:\Users\natha\Downloads\TickerHistory3.parquet"))
FINRA_SI_DIR = Path(os.environ.get("ATX_FINRA_SI_DIR", r"C:\atx\data\finra_short_interest"))
FINRA_SV_DIR = Path(os.environ.get("ATX_FINRA_SV_DIR", r"C:\atx\data\finra_short_volume"))
COMPANYFACTS_DIR = PACKAGE_ROOT / "data" / "staging" / "companyfacts" / "ee099c7394a357f1"
FSDS_DIR = PACKAGE_ROOT / "data" / "staging" / "fsds-v2"
IDENTITY_R4_DIR = PACKAGE_ROOT / "data" / "research" / "identity_rehearsal" / "session8-phased-r4"

WARMUP_START = dt.date(2018, 1, 2)
COVERAGE_START = dt.date(2020, 9, 28)
SNAPSHOT_DATE = dt.date(2026, 9, 20)
IDENTITY_SEAL = dt.date(2026, 9, 21)
MIN_ROWS_PER_SESSION = 1000
MARK_HOUR_UTC = 22


def build_root() -> Path:
    return Path(os.environ.get("ATX_ALPHA_PANEL_ROOT", str(DEFAULT_ROOT)))


def stage_dir(stage: str) -> Path:
    path = build_root() / stage
    path.mkdir(parents=True, exist_ok=True)
    return path


def connect(memory: str = "600MB", threads: int = 2, temp_dir: Path | None = None,
            db_file: str | None = None) -> duckdb.DuckDBPyConnection:
    """DuckDB bounded well below the guard cap, spilling under the build root.

    ``db_file`` names a scratch database under ``_tmp`` (recreated) so large temp tables live on disk.
    """
    tmp = temp_dir or (build_root() / "_tmp")
    tmp.mkdir(parents=True, exist_ok=True)
    # spill files go to a per-process subdirectory: two builds sharing one temp_directory crash each other
    # (0xC0000005 when panel assemble ran two year ranges side by side, 2026-09-29)
    spill = tmp / f"spill-{os.getpid()}"
    target = ":memory:"
    if db_file:
        path = tmp / db_file
        for p in (path, path.with_name(path.name + ".wal")):
            if p.exists():
                p.unlink()
        target = str(path)
    con = duckdb.connect(
        target,
        config={
            "memory_limit": memory,
            "threads": str(threads),
            "temp_directory": str(spill),
            "preserve_insertion_order": "false",
        }
    )
    con.execute("SET TimeZone='UTC'")
    con.execute("SET enable_progress_bar = false")
    return con


def sha256_file(path: Path, chunk: int = 1 << 22) -> str:
    h = hashlib.sha256()
    with path.open("rb") as fh:
        while True:
            blob = fh.read(chunk)
            if not blob:
                break
            h.update(blob)
    return h.hexdigest()


def file_identity(path: Path) -> dict[str, Any]:
    st = path.stat()
    return {"path": str(path), "bytes": st.st_size, "mtime_ns": st.st_mtime_ns}


def write_json_atomic(path: Path, payload: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(path.suffix + ".partial")
    tmp.write_text(json.dumps(payload, indent=2, sort_keys=True, default=str), encoding="utf-8")
    os.replace(tmp, path)


def read_json(path: Path) -> Any:
    return json.loads(path.read_text(encoding="utf-8"))


def copy_to_parquet(con: duckdb.DuckDBPyConnection, sql: str, dest: Path, row_group_size: int = 262144) -> int:
    """Write a query to ``dest`` atomically (``.partial`` then rename); return row count.

    Wide tables need a smaller ``row_group_size``: the writer buffers one row group in memory."""
    dest.parent.mkdir(parents=True, exist_ok=True)
    tmp = dest.with_name(dest.name + ".partial")
    if tmp.exists():
        tmp.unlink()
    con.execute(f"COPY ({sql}) TO '{tmp.as_posix()}' (FORMAT PARQUET, COMPRESSION ZSTD, ROW_GROUP_SIZE {int(row_group_size)})")
    rows = con.execute(f"SELECT count(*) FROM read_parquet('{tmp.as_posix()}')").fetchone()[0]
    os.replace(tmp, dest)
    return int(rows)


@contextmanager
def timed(receipt: dict[str, Any], key: str) -> Iterator[None]:
    t0 = time.perf_counter()
    try:
        yield
    finally:
        receipt.setdefault("timings_s", {})[key] = round(time.perf_counter() - t0, 3)


def code_identity(*modules: str) -> dict[str, Any]:
    """SHA-256 of each named alpha_panel module's bytes (and of its LF-normalised bytes) plus git HEAD."""
    here = Path(__file__).resolve().parent
    out: dict[str, Any] = {}
    for name in modules:
        path = here / (name if name.endswith(".py") else f"{name}.py")
        blob = path.read_bytes()
        out[path.name] = {"sha256": hashlib.sha256(blob).hexdigest(),
                          "sha256_lf": hashlib.sha256(blob.replace(b"\r\n", b"\n")).hexdigest()}
    head = PACKAGE_ROOT.parent / ".git" / "HEAD"
    try:
        ref = head.read_text().strip()
        if ref.startswith("ref: "):
            ref = (head.parent / ref[5:]).read_text().strip()
        out["git_head"] = ref
    except OSError:
        out["git_head"] = None
    return out


def output_hashes(directory: Path, pattern: str = "**/*.parquet") -> dict[str, dict[str, Any]]:
    """{relative path: {bytes, sha256}} for every file under ``directory`` matching ``pattern``."""
    return {p.relative_to(directory).as_posix(): {"bytes": p.stat().st_size, "sha256": sha256_file(p)}
            for p in sorted(directory.glob(pattern)) if p.is_file() and not p.name.endswith(".partial")}


def write_stage_manifest(stage: str, schema: str, modules: tuple[str, ...], payload: dict[str, Any],
                         pattern: str = "**/*.parquet") -> Path:
    """Publish-last ``<stage>/manifest.json``: schema, status, code identity, every output's SHA-256, then payload."""
    directory = stage_dir(stage)
    manifest = {"schema": schema, "status": "complete", "stage": stage,
                "code": code_identity(*modules), "files": output_hashes(directory, pattern), **payload}
    write_json_atomic(directory / "manifest.json", manifest)
    return directory / "manifest.json"


def calendar_path() -> Path:
    return build_root() / "calendar.parquet"


def load_calendar(con: duckdb.DuckDBPyConnection) -> list[dt.date]:
    rows = con.execute(
        f"SELECT session_date FROM read_parquet('{calendar_path().as_posix()}') ORDER BY 1"
    ).fetchall()
    return [r[0] for r in rows]


def years_between(start: dt.date, end: dt.date) -> list[int]:
    return list(range(start.year, end.year + 1))
