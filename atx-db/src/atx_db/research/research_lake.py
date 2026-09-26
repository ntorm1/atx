"""Research lake (L1): immutable, year-partitioned Parquet snapshots of research inputs.

Ruling R-5: research reads immutable Parquet only. A *snapshot* is a directory

    <root>/lake/<snapshot_id>/<dataset>/year=YYYY/part-<i>.parquet
    <root>/lake/<snapshot_id>/_manifest.json

written once and never modified. Two kinds exist:

``export`` (:func:`export_lake_snapshot`)
    Every dataset is one ``SELECT`` (it must yield an integer ``year`` column) run on a
    read-only DuckDB connection with bounded settings and written by
    ``COPY (...) TO ... (FORMAT PARQUET, PARTITION_BY (year), COMPRESSION ZSTD)``. The
    whole snapshot is built in a private temporary directory and published with one
    directory rename, so a reader sees either no snapshot or a complete one. A published
    export snapshot is sealed: nothing is ever added to it.
``external`` (:func:`register_external_dataset`)
    Parsed external files (benchmark libraries: French FF5+UMD, NYSE ME breakpoints, JKP,
    OSAP, q5; dataset names ``bench_*``) are copied in one dataset at a time, each with its
    own digest. Datasets are immutable; registering identical content again is a no-op,
    different content under an existing name is refused.

Research workers never open the prod warehouse: :func:`export_lake_snapshot` refuses
``data/warehouse.duckdb`` unless ``allow_warehouse=True`` (only the OPS agent, holding the
HEAVY token and running under ``run_memory_guarded.py``, passes it). ``db_path=None`` runs
the SELECTs on an in-memory connection (e.g. over ``read_parquet`` of a retained vendor
file such as ``TickerHistory3.parquet``).

The manifest pins every file (relative path, bytes, rows, sha256); readers
(:func:`lake_relation`, :func:`lake_files`) read exactly the manifest's files, and a
dataset's ``dataset_sha256`` is the input digest a feature version records.
"""

from __future__ import annotations

import contextlib
import datetime as dt
import hashlib
import itertools
import json
import os
import re
import shutil
import time
from collections.abc import Iterable, Iterator, Mapping, Sequence
from pathlib import Path
from typing import Any

import duckdb

LAKE_VERSION = "research-lake-v1"
MANIFEST_NAME = "_manifest.json"
KIND_EXPORT = "export"
KIND_EXTERNAL = "external"
#: External dataset names carry this prefix (benchmark libraries parsed by node X.3).
EXTERNAL_PREFIXES = ("bench_",)
#: Default research root (``ATX_RESEARCH_ROOT`` overrides).
DEFAULT_RESEARCH_ROOT = Path("C:/atx/atx-db/data/research")
RESEARCH_ROOT_ENV = "ATX_RESEARCH_ROOT"
#: The prod warehouse file name: never opened by a research worker.
WAREHOUSE_NAME = "warehouse.duckdb"
DEFAULT_MEMORY_LIMIT = "256MB"
DEFAULT_THREADS = 1

_SNAPSHOT = re.compile(r"^[a-z0-9][a-z0-9_.-]{0,95}$")
_DATASET = re.compile(r"^[a-z][a-z0-9_]{0,63}$")
_MEMORY = re.compile(r"^[1-9][0-9]{0,5}(MB|GB)$")
_CHUNK = 1 << 20


class LakeError(ValueError):
    """The lake cannot export, register or read a snapshot under its contract."""


# ---------------------------------------------------------------------------
# Small helpers (shared with the other research-store modules)
# ---------------------------------------------------------------------------

def canonical_json(value: Any) -> str:
    """Deterministic JSON (sorted keys, no whitespace, no NaN); dates become ISO strings."""
    return json.dumps(value, sort_keys=True, separators=(",", ":"), allow_nan=False, default=_json_default)


def _json_default(value: Any) -> Any:
    if isinstance(value, (dt.date, dt.datetime)):
        return value.isoformat()
    if isinstance(value, Path):
        return value.as_posix()
    if isinstance(value, (tuple, set, frozenset)):
        return sorted(value) if isinstance(value, (set, frozenset)) else list(value)
    raise TypeError(f"not JSON-serializable: {type(value).__name__}")


def sha256_text(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(_CHUNK), b""):
            digest.update(chunk)
    return digest.hexdigest()


def code_digest(paths: Iterable[Path | str]) -> str:
    """sha256 over (file name, bytes with LF line endings) of source files, in the given order."""
    digest = hashlib.sha256()
    for item in paths:
        path = Path(item)
        digest.update(path.name.encode("utf-8"))
        digest.update(path.read_bytes().replace(b"\r\n", b"\n"))
    return digest.hexdigest()


def resolve_research_root(root: Path | str | None = None) -> Path:
    """``root``, else ``$ATX_RESEARCH_ROOT``, else :data:`DEFAULT_RESEARCH_ROOT`."""
    if root is not None:
        return Path(root)
    configured = os.environ.get(RESEARCH_ROOT_ENV)
    return Path(os.path.expandvars(configured)).expanduser() if configured else DEFAULT_RESEARCH_ROOT


def default_temp_directory(root: Path) -> Path:
    """DuckDB spill root: ``<root>/../tmp/duckdb`` (``data/tmp/duckdb`` for the default root).

    Each connection spills into a directory of its own below it (:func:`private_temp_directory`).
    """
    return root.parent / "tmp" / "duckdb"


_CONNECTIONS = itertools.count()


def private_temp_directory(root: Path) -> Path:
    """A spill directory for one connection: ``<spill root>/conn-<pid>-<n>-<ns>`` (not created here).

    Ruling C-56: DuckDB gives its spill files the same names in every process
    (``duckdb_temp_storage_*.tmp``) and, at close, deletes the ``duckdb_temp_*`` files of a
    directory it did not create. Two workers sharing one directory therefore overwrote and
    deleted each other's spills (an OOM on a "4690 PiB" block, an INTERNAL "allocation size
    1.7e19", "Failed to delete duckdb_temp_storage_S64K-0.tmp"). A per-connection name that is
    left for DuckDB to create on its first spill is also removed by DuckDB at close; only a
    killed process leaves its (pid-named) directory behind.
    """
    return default_temp_directory(root) / f"conn-{os.getpid()}-{next(_CONNECTIONS)}-{time.monotonic_ns()}"


def sql_text(value: str) -> str:
    return "'" + str(value).replace("'", "''") + "'"


def connect_bounded(db_path: Path | str | None = None, *, root: Path | None = None,
                    memory_limit: str = DEFAULT_MEMORY_LIMIT, threads: int = DEFAULT_THREADS,
                    read_only: bool = True) -> duckdb.DuckDBPyConnection:
    """A DuckDB connection with the research-worker settings applied at connect (M7).

    ``memory_limit`` and ``threads`` are set in the connect config (so they hold during
    WAL replay too), with ``preserve_insertion_order=false`` and a spill directory private
    to this connection (:func:`private_temp_directory`, ruling C-56): concurrent workers
    never share spill files.
    """
    if not _MEMORY.fullmatch(memory_limit):
        raise LakeError("memory_limit must look like '256MB'")
    if isinstance(threads, bool) or not isinstance(threads, int) or not 1 <= threads <= 4:
        raise LakeError("threads must be an integer 1..4")
    research_root = resolve_research_root(root)
    default_temp_directory(research_root).mkdir(parents=True, exist_ok=True)
    temp = private_temp_directory(research_root)
    config = {"memory_limit": memory_limit, "threads": str(threads), "preserve_insertion_order": "false",
              "temp_directory": temp.as_posix(), "max_temp_directory_size": "40GB"}
    if db_path is None:
        con = duckdb.connect(":memory:", config=config)
    else:
        con = duckdb.connect(str(db_path), read_only=read_only, config=config)
    con.execute("SET TimeZone='UTC'")
    con.execute("PRAGMA disable_progress_bar")
    return con


def _refuse_warehouse(db_path: Path, allow_warehouse: bool) -> None:
    if db_path.name.lower() == WAREHOUSE_NAME and not allow_warehouse:
        raise LakeError(
            f"{db_path} is the prod warehouse: research workers never open it (ruling R-5). Only the OPS agent "
            "holding the HEAVY token exports it, under run_memory_guarded.py, with allow_warehouse=True")


def _check_snapshot_id(snapshot_id: str) -> None:
    if not isinstance(snapshot_id, str) or not _SNAPSHOT.fullmatch(snapshot_id):
        raise LakeError(f"snapshot_id {snapshot_id!r} must match {_SNAPSHOT.pattern}")


def _check_dataset(name: str) -> None:
    if not isinstance(name, str) or not _DATASET.fullmatch(name):
        raise LakeError(f"dataset name {name!r} must match {_DATASET.pattern}")


def _now() -> str:
    return dt.datetime.now(dt.UTC).replace(tzinfo=None, microsecond=0).isoformat()


def _write_json_atomic(path: Path, payload: Mapping[str, Any]) -> None:
    tmp = path.with_name(f".{path.name}.{os.getpid()}.{time.monotonic_ns()}.tmp")
    tmp.write_text(json.dumps(payload, indent=1, sort_keys=True, default=_json_default) + "\n", encoding="utf-8")
    os.replace(tmp, path)


@contextlib.contextmanager
def _lock(path: Path, timeout_s: float = 120.0) -> Iterator[None]:
    """Exclusive create of ``path`` as a cross-process lock (manifest updates only)."""
    deadline = time.monotonic() + timeout_s
    while True:
        try:
            fd = os.open(path, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
            break
        except FileExistsError:
            if time.monotonic() > deadline:
                raise LakeError(f"lock {path} held for more than {timeout_s:.0f} s; inspect its owner") from None
            time.sleep(0.2)
    try:
        os.write(fd, str(os.getpid()).encode("ascii"))
        os.close(fd)
        yield
    finally:
        with contextlib.suppress(FileNotFoundError):
            os.remove(path)


# ---------------------------------------------------------------------------
# Writing datasets
# ---------------------------------------------------------------------------

def lake_root(root: Path | str | None = None) -> Path:
    return resolve_research_root(root) / "lake"


def snapshot_path(snapshot_id: str, root: Path | str | None = None) -> Path:
    _check_snapshot_id(snapshot_id)
    return lake_root(root) / snapshot_id


def _copy_dataset(con: duckdb.DuckDBPyConnection, select: str, target: Path) -> int:
    """``COPY (select) TO target`` partitioned by ``year``; returns the rows written."""
    columns = {row[0]: str(row[1]).upper() for row in con.execute(
        f"DESCRIBE SELECT * FROM ({select}) AS _lake_dataset").fetchall()}
    if "year" not in columns:
        raise LakeError("a lake dataset SELECT must yield an integer 'year' partition column")
    if columns["year"] not in ("INTEGER", "BIGINT", "SMALLINT", "TINYINT", "HUGEINT", "UINTEGER", "USMALLINT"):
        raise LakeError(f"the 'year' partition column must be an integer, not {columns['year']}")
    row = con.execute(
        f"COPY ({select}) TO {sql_text(target.as_posix())} "
        "(FORMAT PARQUET, PARTITION_BY (year), COMPRESSION ZSTD, FILENAME_PATTERN 'part-{i}')").fetchone()
    return int(row[0]) if row else 0


def _describe_dataset(con: duckdb.DuckDBPyConnection, directory: Path, dataset: str) -> dict[str, Any]:
    """Files (path ``<dataset>/year=Y/part-i.parquet``, bytes, rows, sha256), years, columns, digest.

    The digest covers the columns and, per file, its path inside the dataset, sha256 and
    rows: the same content registered under another snapshot has the same digest.
    """
    files = sorted(directory.glob("year=*/*.parquet"))
    if not files:
        raise LakeError(f"dataset {dataset} wrote no files (empty SELECT?)")
    listing = [path.as_posix() for path in files]
    literal = "[" + ", ".join(sql_text(name) for name in listing) + "]"
    counts = {Path(name).as_posix(): int(rows) for name, rows in con.execute(
        f"SELECT file_name, num_rows FROM parquet_file_metadata({literal})").fetchall()}
    records = []
    inner = []
    years: set[int] = set()
    for path in files:
        year_text = path.parent.name.split("=", 1)[1]
        if not re.fullmatch(r"-?[0-9]{1,4}", year_text):
            raise LakeError(f"unexpected partition directory {path.parent.name}")
        years.add(int(year_text))
        relative = path.relative_to(directory).as_posix()
        record = {"path": f"{dataset}/{relative}", "bytes": path.stat().st_size,
                  "rows": counts[path.as_posix()], "sha256": sha256_file(path)}
        records.append(record)
        inner.append([relative, record["sha256"], record["rows"]])
    columns = [[str(name), str(kind)] for name, kind, *_ in con.execute(
        f"DESCRIBE SELECT * FROM read_parquet({literal}, hive_partitioning=true)").fetchall()]
    rows = sum(record["rows"] for record in records)
    digest = sha256_text(canonical_json({"columns": columns, "files": inner}))
    return {"rows": rows, "years": sorted(years), "columns": columns, "files": records, "dataset_sha256": digest}


def _snapshot_digest(datasets: Mapping[str, Mapping[str, Any]]) -> str:
    return sha256_text(canonical_json({name: item["dataset_sha256"] for name, item in sorted(datasets.items())}))


def export_lake_snapshot(db_path: Path | str | None, snapshot_id: str, datasets: Mapping[str, str], *,
                         root: Path | str | None = None, memory_limit: str = DEFAULT_MEMORY_LIMIT,
                         threads: int = DEFAULT_THREADS, allow_warehouse: bool = False,
                         source_meta: Mapping[str, Any] | None = None) -> Path:
    """Export ``datasets`` (name -> SELECT with an integer ``year`` column) as a sealed snapshot.

    ``db_path`` is opened read-only (``None``: an in-memory connection for SELECTs over
    Parquet/CSV files). The snapshot is written to a private temporary directory and
    published with one rename; an existing snapshot id is refused (snapshots are
    immutable). Returns the snapshot directory.
    """
    _check_snapshot_id(snapshot_id)
    if not datasets:
        raise LakeError("export at least one dataset")
    for name, select in datasets.items():
        _check_dataset(name)
        if not isinstance(select, str) or not select.strip():
            raise LakeError(f"dataset {name}: SELECT must be a non-empty string")
    base = lake_root(root)
    final = base / snapshot_id
    if final.exists():
        raise LakeError(f"lake snapshot {snapshot_id} already exists at {final}; snapshots are immutable")
    source = None
    if db_path is not None:
        source = Path(db_path).resolve()
        _refuse_warehouse(source, allow_warehouse)
        if not source.is_file():
            raise LakeError(f"source database not found: {source}")
    base.mkdir(parents=True, exist_ok=True)
    work = base / f".tmp-{snapshot_id}-{os.getpid()}-{time.monotonic_ns()}"
    work.mkdir()
    started = time.perf_counter()
    try:
        con = connect_bounded(source, root=resolve_research_root(root), memory_limit=memory_limit,
                              threads=threads, read_only=True)
        try:
            described: dict[str, dict[str, Any]] = {}
            for name, select in sorted(datasets.items()):
                t0 = time.perf_counter()
                copied = _copy_dataset(con, select, work / name)
                item = _describe_dataset(con, work / name, name)
                if item["rows"] != copied:
                    raise LakeError(f"dataset {name}: COPY reported {copied} rows, files hold {item['rows']}")
                described[name] = {"select": select, "select_sha256": sha256_text(select), **item,
                                   "seconds": round(time.perf_counter() - t0, 3)}
        finally:
            con.close()
        manifest = {"lake_version": LAKE_VERSION, "snapshot_id": snapshot_id, "kind": KIND_EXPORT,
                    "created_at": _now(), "duckdb": duckdb.__version__,
                    "source": None if source is None else {
                        "path": source.as_posix(), "bytes": source.stat().st_size,
                        "mtime": dt.datetime.fromtimestamp(source.stat().st_mtime, dt.UTC).replace(
                            tzinfo=None).isoformat()},
                    "source_meta": dict(source_meta or {}), "settings": {"memory_limit": memory_limit,
                                                                         "threads": threads},
                    "seconds": round(time.perf_counter() - started, 3),
                    "datasets": described, "snapshot_sha256": _snapshot_digest(described)}
        _write_json_atomic(work / MANIFEST_NAME, manifest)
        os.replace(work, final)
    except BaseException:
        shutil.rmtree(work, ignore_errors=True)
        raise
    return final


def register_external_dataset(snapshot_id: str, dataset: str, sources: Path | str | Sequence[Path | str], *,
                              year_sql: str, root: Path | str | None = None, select: str | None = None,
                              source_meta: Mapping[str, Any] | None = None,
                              memory_limit: str = DEFAULT_MEMORY_LIMIT) -> Path:
    """Register parsed external Parquet files (``bench_*``) as one immutable lake dataset.

    ``sources``: the Parquet file(s) (e.g. node X.3's ``data/raw/benchmarks/parsed/*.parquet``).
    ``year_sql``: the partition expression over their columns (e.g. ``year(date)`` or
    ``CAST(yyyymm / 100 AS INTEGER)``). ``select`` overrides the default
    ``SELECT *, CAST(<year_sql> AS INTEGER) AS year FROM read_parquet(<sources>)`` and may
    reference ``{sources}`` for the file list literal. The snapshot must be an
    ``external`` one (created here on first use); an identical re-registration returns the
    existing dataset, different content under the same name is refused.
    """
    _check_snapshot_id(snapshot_id)
    _check_dataset(dataset)
    if not dataset.startswith(EXTERNAL_PREFIXES):
        raise LakeError(f"external dataset names start with {EXTERNAL_PREFIXES}: {dataset!r}")
    items = [Path(sources)] if isinstance(sources, (str, Path)) else [Path(item) for item in sources]
    if not items:
        raise LakeError("register at least one source file")
    for item in items:
        if not item.is_file():
            raise LakeError(f"external source not found: {item}")
    literal = "[" + ", ".join(sql_text(item.resolve().as_posix()) for item in sorted(items)) + "]"
    if select is None:
        if not isinstance(year_sql, str) or not year_sql.strip():
            raise LakeError("year_sql is required (the partition expression)")
        query = f"SELECT *, CAST({year_sql} AS INTEGER) AS year FROM read_parquet({literal})"
    else:
        query = select.format(sources=literal)
    research_root = resolve_research_root(root)
    snapshot = snapshot_path(snapshot_id, research_root)
    snapshot.mkdir(parents=True, exist_ok=True)
    manifest_path = snapshot / MANIFEST_NAME
    work = snapshot / f".tmp-{dataset}-{os.getpid()}-{time.monotonic_ns()}"
    con = connect_bounded(None, root=research_root, memory_limit=memory_limit)
    try:
        copied = _copy_dataset(con, query, work)
        described = _describe_dataset(con, work, dataset)
        if described["rows"] != copied:
            raise LakeError(f"dataset {dataset}: COPY reported {copied} rows, files hold {described['rows']}")
        entry ={"select": query, "select_sha256": sha256_text(query), **described,
                 "sources": [{"path": item.resolve().as_posix(), "bytes": item.stat().st_size,
                              "sha256": sha256_file(item)} for item in sorted(items)],
                 "source_meta": dict(source_meta or {}), "registered_at": _now()}
        with _lock(snapshot / "_manifest.lock"):
            manifest = (json.loads(manifest_path.read_text(encoding="utf-8")) if manifest_path.is_file() else
                        {"lake_version": LAKE_VERSION, "snapshot_id": snapshot_id, "kind": KIND_EXTERNAL,
                         "created_at": _now(), "datasets": {}})
            if manifest.get("kind") != KIND_EXTERNAL:
                raise LakeError(f"snapshot {snapshot_id} is a sealed {manifest.get('kind')!r} snapshot; external "
                                "datasets go into an external snapshot")
            existing = manifest["datasets"].get(dataset)
            if existing is not None:
                if existing["dataset_sha256"] != entry["dataset_sha256"]:
                    raise LakeError(f"dataset {dataset} already registered in {snapshot_id} with different content "
                                    f"({existing['dataset_sha256']} != {entry['dataset_sha256']}); use a new name "
                                    "or snapshot")
                shutil.rmtree(work, ignore_errors=True)
                return snapshot / dataset
            os.replace(work, snapshot / dataset)
            manifest["datasets"][dataset] = entry
            manifest["snapshot_sha256"] = _snapshot_digest(manifest["datasets"])
            _write_json_atomic(manifest_path, manifest)
    except BaseException:
        shutil.rmtree(work, ignore_errors=True)
        raise
    finally:
        con.close()
    return snapshot / dataset


def register_benchmarks(snapshot_id: str, specs: Sequence[Mapping[str, Any]], *,
                        root: Path | str | None = None) -> dict[str, Path]:
    """Register node X.3's parsed benchmark files (``benchmarks_load.lake_registration_specs()``).

    Each spec names ``dataset`` (``bench_*``), ``path`` and ``parquet_sha256``; the file must
    still hash to it (pinned inputs). Monthly datasets partition by ``year(month_end)``; a
    table without ``month_end`` (reference/documentation tables) goes to ``year=0``.
    """
    import pyarrow.parquet as pq

    registered: dict[str, Path] = {}
    for spec in specs:
        path = Path(str(spec["path"]))
        expected = spec.get("parquet_sha256")
        actual = sha256_file(path)
        if expected is not None and actual != expected:
            raise LakeError(f"{spec['dataset']}: {path} hashes to {actual}, its X.3 manifest says {expected}")
        names = set(pq.read_schema(path).names)
        year_sql = "year(month_end)" if "month_end" in names else "0"
        registered[str(spec["dataset"])] = register_external_dataset(
            snapshot_id, str(spec["dataset"]), path, year_sql=year_sql, root=root,
            source_meta={key: spec.get(key) for key in ("parquet_sha256", "rows", "date_min", "date_max")})
    return registered


# ---------------------------------------------------------------------------
# Reading
# ---------------------------------------------------------------------------

def lake_manifest(snapshot_id: str, root: Path | str | None = None) -> dict[str, Any]:
    path = snapshot_path(snapshot_id, root) / MANIFEST_NAME
    if not path.is_file():
        raise LakeError(f"lake snapshot {snapshot_id} has no manifest at {path}")
    manifest = json.loads(path.read_text(encoding="utf-8"))
    if manifest.get("lake_version") != LAKE_VERSION:
        raise LakeError(f"lake snapshot {snapshot_id}: unsupported lake version {manifest.get('lake_version')!r}")
    return manifest


def _dataset_entry(snapshot_id: str, dataset: str, root: Path | str | None) -> dict[str, Any]:
    manifest = lake_manifest(snapshot_id, root)
    entry = manifest["datasets"].get(dataset)
    if entry is None:
        raise LakeError(f"lake snapshot {snapshot_id} has no dataset {dataset!r} "
                        f"(has {sorted(manifest['datasets'])})")
    return entry


def lake_dataset_digest(snapshot_id: str, dataset: str, root: Path | str | None = None) -> str:
    """The dataset's content digest (the input digest a feature version records)."""
    return str(_dataset_entry(snapshot_id, dataset, root)["dataset_sha256"])


def lake_files(snapshot_id: str, dataset: str, years: Sequence[int] | None = None,
               root: Path | str | None = None) -> list[str]:
    """Absolute posix paths of the manifest's files of ``dataset`` (optionally some years only)."""
    entry = _dataset_entry(snapshot_id, dataset, root)
    base = snapshot_path(snapshot_id, root)
    wanted = None if years is None else {int(y) for y in years}
    paths = []
    for record in entry["files"]:
        year = int(record["path"].split("/")[1].split("=", 1)[1])
        if wanted is None or year in wanted:
            paths.append((base / record["path"]).as_posix())
    return paths


def lake_relation(con: duckdb.DuckDBPyConnection, snapshot_id: str, dataset: str,
                  years: Sequence[int] | None = None, root: Path | str | None = None) -> duckdb.DuckDBPyRelation:
    """The dataset (exactly its manifest files, ``year`` restored from the partition path)."""
    files = lake_files(snapshot_id, dataset, years, root)
    if not files:
        raise LakeError(f"{snapshot_id}/{dataset}: no files for years {years}")
    literal = "[" + ", ".join(sql_text(path) for path in files) + "]"
    return con.sql(f"SELECT * FROM read_parquet({literal}, hive_partitioning=true)")


def verify_lake_snapshot(snapshot_id: str, root: Path | str | None = None) -> dict[str, Any]:
    """Re-hash every file of the snapshot against its manifest (read-only)."""
    manifest = lake_manifest(snapshot_id, root)
    base = snapshot_path(snapshot_id, root)
    problems: list[str] = []
    files = 0
    for name, entry in sorted(manifest["datasets"].items()):
        for record in entry["files"]:
            files += 1
            path = base / record["path"]
            if not path.is_file():
                problems.append(f"{name}: missing {record['path']}")
            elif path.stat().st_size != record["bytes"] or sha256_file(path) != record["sha256"]:
                problems.append(f"{name}: changed {record['path']}")
    return {"snapshot_id": snapshot_id, "files": files, "ok": not problems, "problems": problems,
            "snapshot_sha256": manifest.get("snapshot_sha256")}


__all__ = [
    "DEFAULT_RESEARCH_ROOT",
    "EXTERNAL_PREFIXES",
    "KIND_EXPORT",
    "KIND_EXTERNAL",
    "LAKE_VERSION",
    "LakeError",
    "canonical_json",
    "code_digest",
    "connect_bounded",
    "export_lake_snapshot",
    "lake_dataset_digest",
    "lake_files",
    "lake_manifest",
    "lake_relation",
    "lake_root",
    "private_temp_directory",
    "register_benchmarks",
    "register_external_dataset",
    "resolve_research_root",
    "sha256_file",
    "sha256_text",
    "snapshot_path",
    "verify_lake_snapshot",
]
