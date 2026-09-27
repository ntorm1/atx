"""Batch-ledger runtime for heavy warehouse stages (index section 4 M3; tier-1 v2 node 1.3).

A heavy stage never rewrites a bulk table in one transaction. It is a :class:`BatchStage`:

* ``plan(conn, run_key)`` returns the run's batches (``BatchSpec``: an ordinal plus the key range
  and payload the batch needs). The plan is computed ONCE, when the run is opened, and frozen in
  ``build_runs.spec_json`` with the caller's spec, the stage's code digest and the DuckDB version,
  so every later slice builds exactly the planned batches;
* ``prepare(conn, run_key)`` creates the stage's ``<table>__next`` staging tables, once, in the
  same transaction that opens the run;
* ``build(conn, spec, run_key)`` writes one batch's output into the staging tables. The runner
  wraps it, per batch, in exactly ``BEGIN -> build -> INSERT INTO build_batches -> COMMIT``: a
  batch's output and its ledger row commit together, so a kill loses at most the batch in flight
  and a rerun skips every ledgered batch (no duplicate keys, no gaps);
* ``finalize(conn, run_key)`` validates the complete staging tables and publishes them by one
  short swap (``_bulk_publication.publish_validated_shadows``) whose transaction also marks the
  run ``published`` (:func:`mark_published`); the runner then ``CHECKPOINT``s and asserts that no
  WAL is left.

:func:`run_slice` runs at most ``max_batches`` batches or ``max_seconds`` in one process and one
bounded connection (the stage's ``memory_limit``/``threads`` at connect, a private spill
directory per process and file, ruling C-56), ``CHECKPOINT`` every ``checkpoint_every`` batches
and at slice end, and reconnects every ``recycle_every`` batches (DuckDB 1.5.x instance memory
growth, index M6). ``scripts/run_slices.py`` runs each slice as a fresh guarded subprocess.

Ledger tables (migration 0329, no DEFAULT now(), no key, no index): ``build_runs`` (one row per
(stage, run_key): ``open`` -> ``published`` | ``abandoned``) and ``build_batches`` (one row per
COMMITTED batch). Their small keyed UPDATEs are allowed by the WAL rule (no DEFAULT now()).

Batch workers import only ``duckdb``, the standard library and their own modules (M6).
"""

from __future__ import annotations

import contextlib
import datetime as dt
import hashlib
import importlib
import inspect
import json
import os
import time
import traceback
from collections.abc import Callable, Mapping
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Literal

import duckdb

from .connection import DuckDBStore, open_duckdb_connection

__all__ = [
    "BATCH_STAGES",
    "BatchResult",
    "BatchSpec",
    "BatchStage",
    "RunStateError",
    "SliceResult",
    "abandon_run",
    "assert_no_wal",
    "finalize_run",
    "get_stage",
    "mark_published",
    "open_run",
    "pending_batches",
    "register_stage",
    "run_record",
    "run_slice",
    "stage_code_digest",
    "store_for",
    "utc_now",
    "wal_path",
]

#: Stages whose module registers itself on import (``get_stage`` imports it on first use).
_KNOWN_STAGE_MODULES: dict[str, str] = {
    "bars_unit_correction": "atx_db.bars_unit_correction",
    "companyfacts_rebuild": "atx_db.companyfacts_rebuild",
    "identity_links_rebuild": "atx_db.identity_links_stage",
    "submissions_rebuild": "atx_db.submissions_rebuild",
}
_RUN_STATUSES = ("open", "published", "abandoned")


@dataclass(frozen=True)
class BatchSpec:
    """One planned batch: its ordinal, key range (as text, inclusive) and the payload ``build`` reads."""

    batch_id: int
    lo: str | None = None
    hi: str | None = None
    input_sha256: str | None = None
    payload: Mapping[str, object] = field(default_factory=dict)


@dataclass(frozen=True)
class BatchResult:
    """What one committed batch read and wrote (``note``: JSON text the stage's finalize may read)."""

    rows_in: int
    rows_out: int
    note: str | None = None


@dataclass(frozen=True)
class BatchStage:
    """A ledgered heavy stage (see the module docstring for the contract of each callable).

    ``discard`` (optional) drops the stage's staging tables when a run is abandoned.
    """

    name: str
    plan: Callable[[duckdb.DuckDBPyConnection, str], list[BatchSpec]]
    prepare: Callable[[duckdb.DuckDBPyConnection, str], None]
    build: Callable[[duckdb.DuckDBPyConnection, BatchSpec, str], BatchResult]
    finalize: Callable[[duckdb.DuckDBPyConnection, str], dict[str, object]]
    memory_limit: str = "384MB"
    threads: int = 1
    discard: Callable[[duckdb.DuckDBPyConnection, str], None] | None = None
    code_dependencies: tuple[str, ...] = ()


@dataclass(frozen=True)
class SliceResult:
    """Outcome of one slice. ``error`` carries the failure text when ``stopped == "error"``."""

    stage: str
    run_key: str
    committed: tuple[int, ...]
    remaining: int
    stopped: Literal["complete", "max_batches", "max_seconds", "error"]
    error: str | None = None


class RunStateError(RuntimeError):
    """The requested operation does not fit the run's ledger state (absent, abandoned, spec changed...)."""


BATCH_STAGES: dict[str, BatchStage] = {}


def register_stage(stage: BatchStage) -> None:
    """Add ``stage`` to :data:`BATCH_STAGES` (re-registering the same object is a no-op)."""
    existing = BATCH_STAGES.get(stage.name)
    if existing is not None and existing is not stage:
        raise ValueError(f"batch stage {stage.name!r} is already registered by another definition")
    if not stage.name or not stage.name.replace("_", "").isalnum():
        raise ValueError(f"invalid batch stage name {stage.name!r}")
    if isinstance(stage.threads, bool) or not isinstance(stage.threads, int) or stage.threads < 1:
        raise ValueError("threads must be a positive integer")
    BATCH_STAGES[stage.name] = stage


def get_stage(name: str) -> BatchStage:
    """The registered stage ``name`` (a known stage module is imported on first use)."""
    if name not in BATCH_STAGES and name in _KNOWN_STAGE_MODULES:
        importlib.import_module(_KNOWN_STAGE_MODULES[name])
    try:
        return BATCH_STAGES[name]
    except KeyError:
        raise ValueError(f"unknown batch stage {name!r}; registered: {sorted(BATCH_STAGES)}") from None


# ---------------------------------------------------------------------------
# Small helpers
# ---------------------------------------------------------------------------

def utc_now() -> dt.datetime:
    """Naive UTC wall clock: a lineage stamp of the build, never data availability."""
    return dt.datetime.now(dt.UTC).replace(tzinfo=None)


def _canonical(value: object) -> str:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), default=str)


def _batch_dict(spec: BatchSpec) -> dict[str, object]:
    return {"batch_id": int(spec.batch_id), "lo": spec.lo, "hi": spec.hi, "input_sha256": spec.input_sha256,
            "payload": dict(spec.payload)}


def _batch_from(value: Mapping[str, Any]) -> BatchSpec:
    return BatchSpec(batch_id=int(value["batch_id"]), lo=value.get("lo"), hi=value.get("hi"),
                     input_sha256=value.get("input_sha256"), payload=dict(value.get("payload") or {}))


def wal_path(db_path: Path | str) -> Path:
    return Path(str(db_path) + ".wal")


def assert_no_wal(db_path: Path | str) -> None:
    """Raise when ``db_path`` has a WAL after its last connection closed (M3: publication ends with none)."""
    wal = wal_path(db_path)
    if wal.exists() and wal.stat().st_size > 0:
        raise RuntimeError(f"{wal} holds {wal.stat().st_size:,} bytes after CHECKPOINT and close (expected none)")


def stage_code_digest(stage: BatchStage) -> str:
    """SHA-256 of this runner's and the stage module's source (line endings normalized).

    Frozen in the run's spec: a slice refuses to continue a run built by other stage code, so one
    run's batches never mix two versions of ``build``.
    """
    package = Path(__file__).resolve().parent
    files = {Path(__file__).resolve(): "batch_runner.py"}
    for function in (stage.plan, stage.prepare, stage.build, stage.finalize, stage.discard,
                     open_duckdb_connection):
        if function is None:
            continue
        source = inspect.getsourcefile(function)
        if source:
            path = Path(source).resolve()
            files[path] = (path.relative_to(package).as_posix() if path.is_relative_to(package)
                           else f"callback:{function.__module__}")
    for relative in stage.code_dependencies:
        path = (package / relative).resolve()
        if not path.is_relative_to(package):
            raise ValueError(f"stage dependency is outside the package: {relative}")
        files[path] = path.relative_to(package).as_posix()
    digest = hashlib.sha256()
    for path, name in sorted(files.items(), key=lambda item: item[1]):
        digest.update(name.encode("utf-8") + b"\0")
        digest.update(path.read_bytes().replace(b"\r\n", b"\n") + b"\0")
    return digest.hexdigest()


def connect(db_path: Path | str, stage: BatchStage, *, memory_limit: str | None = None,
            threads: int | None = None) -> duckdb.DuckDBPyConnection:
    """A read-write connection bounded at connect (WAL replay included), private spill directory, UTC."""
    return open_duckdb_connection(db_path, memory_limit=memory_limit or stage.memory_limit,
                                  threads=threads or stage.threads)


def store_for(conn: duckdb.DuckDBPyConnection) -> DuckDBStore:
    """A :class:`DuckDBStore` view of an open connection (for helpers such as
    ``_bulk_publication.publish_validated_shadows``); it never initializes or reconnects."""
    row = conn.execute("SELECT path FROM duckdb_databases() WHERE database_name = current_database()").fetchone()
    store = DuckDBStore(str(row[0]) if row and row[0] else ":memory:")
    store.connection = conn
    return store


# ---------------------------------------------------------------------------
# Ledger
# ---------------------------------------------------------------------------

def run_record(conn: duckdb.DuckDBPyConnection, stage: str, run_key: str) -> dict[str, Any] | None:
    """The ``build_runs`` row of (stage, run_key) with its spec decoded; None when absent."""
    rows = conn.execute(
        "SELECT spec_json, status, created_at, published_at, code_sha, note FROM build_runs "
        "WHERE stage = ? AND run_key = ?",
        [stage, run_key],
    ).fetchall()
    if not rows:
        return None
    if len(rows) > 1:
        raise RunStateError(f"build_runs holds {len(rows)} rows for ({stage!r}, {run_key!r}); expected one")
    spec_json, status, created_at, published_at, code_sha, note = rows[0]
    return {"stage": stage, "run_key": run_key, "spec": json.loads(spec_json), "status": status,
            "created_at": created_at, "published_at": published_at, "code_sha": code_sha, "note": note}


def _committed_ids(conn: duckdb.DuckDBPyConnection, stage: str, run_key: str) -> list[int]:
    rows = conn.execute(
        "SELECT batch_id, count(*) FROM build_batches WHERE stage = ? AND run_key = ? GROUP BY batch_id",
        [stage, run_key],
    ).fetchall()
    doubled = [int(batch) for batch, count in rows if count > 1]
    if doubled:
        raise RunStateError(f"build_batches holds batches {doubled} of ({stage!r}, {run_key!r}) more than once")
    return sorted(int(batch) for batch, _ in rows)


def open_run(conn: duckdb.DuckDBPyConnection, stage: str, run_key: str, spec: Mapping[str, object],
             code_sha: str | None) -> None:
    """Open (stage, run_key), or confirm the open run continues with the same spec and code.

    A new run: refuses while another run of the stage is ``open`` (its staging tables would be
    replaced); then, in ONE transaction, inserts the ``build_runs`` row, computes the plan, runs
    ``prepare`` and freezes ``{spec, plan, code_digest, duckdb_version}`` in ``spec_json``.
    An existing run: the caller's spec, the stage code digest and the DuckDB version (the plan's
    bucket hashes) must equal the frozen ones; an abandoned run is refused; an open or published
    run is left as is.
    """
    definition = get_stage(stage)
    requested = json.loads(_canonical(dict(spec)))
    digest = stage_code_digest(definition)
    record = run_record(conn, stage, run_key)
    if record is not None:
        stored = record["spec"]
        if record["status"] == "abandoned":
            raise RunStateError(f"run ({stage!r}, {run_key!r}) was abandoned; open a new run_key")
        if stored.get("spec") != requested:
            raise RunStateError(f"run ({stage!r}, {run_key!r}) was opened with spec {stored.get('spec')}, "
                                f"not {requested}; use a new run_key")
        if record["status"] == "open":
            if stored.get("code_digest") != digest:
                raise RunStateError(
                    f"run ({stage!r}, {run_key!r}) was built by stage code {str(stored.get('code_digest'))[:12]}, "
                    f"this is {digest[:12]}: finish it with that code or abandon it and open a new run_key")
            if stored.get("duckdb_version") != duckdb.__version__:
                raise RunStateError(f"run ({stage!r}, {run_key!r}) was planned under DuckDB "
                                    f"{stored.get('duckdb_version')}, this is {duckdb.__version__}")
        return
    others = conn.execute(
        "SELECT run_key FROM build_runs WHERE stage = ? AND status = 'open' AND run_key <> ?", [stage, run_key]
    ).fetchall()
    if others:
        raise RunStateError(f"stage {stage!r} already has open run(s) {[row[0] for row in others]}; finish or "
                            "abandon them first (they own the staging tables)")
    frozen: dict[str, object] = {"spec": requested, "code_digest": digest, "duckdb_version": duckdb.__version__,
                                 "plan": None}
    conn.execute("BEGIN TRANSACTION")
    try:
        conn.execute(
            "INSERT INTO build_runs (stage, run_key, spec_json, status, created_at, published_at, code_sha, note) "
            "VALUES (?, ?, ?, 'open', ?, NULL, ?, NULL)",
            [stage, run_key, _canonical(frozen), utc_now(), code_sha],
        )
        batches = definition.plan(conn, run_key)
        ids = [int(batch.batch_id) for batch in batches]
        if not batches or len(set(ids)) != len(ids) or min(ids) < 0:
            raise ValueError(f"stage {stage!r} planned invalid batch ids {ids[:20]}")
        frozen["plan"] = [_batch_dict(batch) for batch in sorted(batches, key=lambda b: b.batch_id)]
        definition.prepare(conn, run_key)
        conn.execute("UPDATE build_runs SET spec_json = ? WHERE stage = ? AND run_key = ?",
                     [_canonical(frozen), stage, run_key])
        conn.execute("COMMIT")
    except BaseException:
        with contextlib.suppress(Exception):
            conn.execute("ROLLBACK")
        raise
    conn.execute("CHECKPOINT")


def pending_batches(conn: duckdb.DuckDBPyConnection, stage: str, run_key: str) -> list[BatchSpec]:
    """The run's planned batches without a ``build_batches`` row, in batch order."""
    record = run_record(conn, stage, run_key)
    if record is None:
        raise RunStateError(f"no build run ({stage!r}, {run_key!r}); open it first")
    plan = record["spec"].get("plan") or []
    committed = set(_committed_ids(conn, stage, run_key))
    unknown = committed - {int(item["batch_id"]) for item in plan}
    if unknown:
        raise RunStateError(f"build_batches holds batches {sorted(unknown)} that the plan of "
                            f"({stage!r}, {run_key!r}) does not name")
    return [_batch_from(item) for item in plan if int(item["batch_id"]) not in committed]


def mark_published(conn: duckdb.DuckDBPyConnection, stage: str, run_key: str,
                   receipt: Mapping[str, object] | None = None) -> None:
    """Mark the open run published (call it inside the publishing swap's transaction)."""
    changed = conn.execute(
        "UPDATE build_runs SET status = 'published', published_at = ?, note = ? "
        "WHERE stage = ? AND run_key = ? AND status = 'open'",
        [utc_now(), None if receipt is None else _canonical(dict(receipt)), stage, run_key],
    ).fetchone()
    if not changed or int(changed[0]) != 1:
        raise RunStateError(f"run ({stage!r}, {run_key!r}) is not open; nothing was published")


def abandon_run(conn: duckdb.DuckDBPyConnection, stage: str, run_key: str, reason: str) -> None:
    """Mark an open run abandoned (it is never published) and drop its staging tables."""
    definition = get_stage(stage)
    conn.execute("BEGIN TRANSACTION")
    try:
        changed = conn.execute(
            "UPDATE build_runs SET status = 'abandoned', note = ? WHERE stage = ? AND run_key = ? AND status = 'open'",
            [_canonical({"abandoned_at": utc_now(), "reason": reason}), stage, run_key],
        ).fetchone()
        if not changed or int(changed[0]) != 1:
            raise RunStateError(f"run ({stage!r}, {run_key!r}) is not open")
        if definition.discard is not None:
            definition.discard(conn, run_key)
        conn.execute("COMMIT")
    except BaseException:
        with contextlib.suppress(Exception):
            conn.execute("ROLLBACK")
        raise
    conn.execute("CHECKPOINT")


# ---------------------------------------------------------------------------
# Slices
# ---------------------------------------------------------------------------

Emit = Callable[[dict[str, object]], None]


def _build_one(conn: duckdb.DuckDBPyConnection, definition: BatchStage, spec: BatchSpec, run_key: str) -> bool:
    """One batch in exactly BEGIN -> build -> INSERT INTO build_batches -> COMMIT (False: already ledgered)."""
    started = utc_now()
    conn.execute("BEGIN TRANSACTION")
    try:
        found = conn.execute(
            "SELECT count(*) FROM build_batches WHERE stage = ? AND run_key = ? AND batch_id = ?",
            [definition.name, run_key, int(spec.batch_id)],
        ).fetchone()
        if found and int(found[0]):
            conn.execute("ROLLBACK")
            return False
        result = definition.build(conn, spec, run_key)
        for name in ("rows_in", "rows_out"):
            value = getattr(result, name)
            if isinstance(value, bool) or not isinstance(value, int) or value < 0:
                raise ValueError(f"batch {spec.batch_id} of {definition.name!r} returned {name}={value!r}")
        conn.execute(
            "INSERT INTO build_batches (stage, run_key, batch_id, lo, hi, input_sha256, rows_in, rows_out, "
            "started_at, finished_at, process_id, note) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
            [definition.name, run_key, int(spec.batch_id), spec.lo, spec.hi, spec.input_sha256,
             int(result.rows_in), int(result.rows_out), started, utc_now(), os.getpid(), result.note],
        )
        conn.execute("COMMIT")
    except BaseException:
        with contextlib.suppress(Exception):
            conn.execute("ROLLBACK")
        raise
    return True


def run_slice(db_path: Path, stage: str, run_key: str, *, max_batches: int = 8, max_seconds: int = 600,
              checkpoint_every: int = 4, recycle_every: int = 4, memory_limit: str | None = None,
              threads: int | None = None, on_event: Emit | None = None) -> SliceResult:
    """Build up to ``max_batches`` pending batches of an open run, or until ``max_seconds`` have passed.

    A batch is never interrupted by the time budget (it is checked before each batch). A failing
    batch rolls back and ends the slice with ``stopped == "error"`` (its text in ``error``); the
    batches committed before it stay. ``on_event`` receives ``batch_begin`` / ``batch_committed``
    events. ``memory_limit``/``threads`` override the stage's connect budget (M8: one step down).
    """
    if max_batches < 1 or max_seconds < 1 or checkpoint_every < 1 or recycle_every < 1:
        raise ValueError("max_batches, max_seconds, checkpoint_every and recycle_every must be >= 1")
    definition = get_stage(stage)
    emit = on_event or (lambda event: None)
    deadline = time.monotonic() + max_seconds
    committed: list[int] = []
    stopped: Literal["complete", "max_batches", "max_seconds", "error"] = "complete"
    error: str | None = None
    conn = connect(db_path, definition, memory_limit=memory_limit, threads=threads)
    try:
        record = run_record(conn, stage, run_key)
        if record is None or record["status"] != "open":
            raise RunStateError(f"run ({stage!r}, {run_key!r}) is "
                                f"{'absent' if record is None else record['status']}; only an open run takes batches")
        frozen = record["spec"]
        if frozen.get("code_digest") != stage_code_digest(definition):
            raise RunStateError(f"run ({stage!r}, {run_key!r}) was built by other stage code; abandon it or "
                                "finish it with that code")
        if frozen.get("duckdb_version") != duckdb.__version__:
            raise RunStateError(f"run ({stage!r}, {run_key!r}) was planned under DuckDB "
                                f"{frozen.get('duckdb_version')}, this is {duckdb.__version__}")
        pending = pending_batches(conn, stage, run_key)
        since_checkpoint = since_connect = 0
        for index, spec in enumerate(pending):
            if len(committed) >= max_batches:
                stopped = "max_batches"
                break
            if time.monotonic() >= deadline:
                stopped = "max_seconds"
                break
            emit({"event": "batch_begin", "stage": stage, "run_key": run_key, "batch_id": spec.batch_id,
                  "utc": utc_now().isoformat()})
            began = time.monotonic()
            try:
                built = _build_one(conn, definition, spec, run_key)
            except Exception as exc:  # the batch rolled back; committed batches stay
                stopped, error = "error", f"batch {spec.batch_id}: {exc!r}"
                emit({"event": "batch_failed", "batch_id": spec.batch_id, "error": repr(exc)[:2000],
                      "traceback": traceback.format_exc()[-4000:]})
                break
            if built:
                committed.append(spec.batch_id)
                emit({"event": "batch_committed", "stage": stage, "run_key": run_key, "batch_id": spec.batch_id,
                      "seconds": round(time.monotonic() - began, 2), "utc": utc_now().isoformat()})
            since_checkpoint += 1
            since_connect += 1
            if since_checkpoint >= checkpoint_every:
                conn.execute("CHECKPOINT")
                since_checkpoint = 0
            if since_connect >= recycle_every and index + 1 < len(pending):
                conn.execute("CHECKPOINT")
                conn.close()
                conn = connect(db_path, definition, memory_limit=memory_limit, threads=threads)
                since_checkpoint = since_connect = 0
        conn.execute("CHECKPOINT")
        remaining = len(pending_batches(conn, stage, run_key))
    finally:
        conn.close()
    if stopped == "complete" and remaining:
        stopped = "max_batches"
    return SliceResult(stage=stage, run_key=run_key, committed=tuple(committed), remaining=remaining,
                       stopped=stopped, error=error)


def finalize_run(db_path: Path, stage: str, run_key: str, *, memory_limit: str | None = None,
                 threads: int | None = None) -> dict[str, object]:
    """Validate and publish a run whose every planned batch is committed; idempotent once published.

    The stage's ``finalize`` validates, swaps and marks the run published in one transaction
    (:func:`mark_published`); this function then ``CHECKPOINT``s, closes and asserts no WAL. A run
    already published only gets the ``CHECKPOINT`` and the WAL check (a kill after the swap's
    COMMIT is replayed at the next open).
    """
    definition = get_stage(stage)
    conn = connect(db_path, definition, memory_limit=memory_limit, threads=threads)
    try:
        record = run_record(conn, stage, run_key)
        if record is None:
            raise RunStateError(f"no build run ({stage!r}, {run_key!r})")
        if record["status"] == "published":
            receipt: dict[str, object] = {"already_published": True, "published_at": str(record["published_at"]),
                                          "receipt": json.loads(record["note"]) if record["note"] else None}
        elif record["status"] != "open":
            raise RunStateError(f"run ({stage!r}, {run_key!r}) is {record['status']}; it cannot be published")
        else:
            if record["spec"].get("code_digest") != stage_code_digest(definition):
                raise RunStateError(f"run ({stage!r}, {run_key!r}) was built by other stage code")
            if record["spec"].get("duckdb_version") != duckdb.__version__:
                raise RunStateError(f"run ({stage!r}, {run_key!r}) was planned under another DuckDB version")
            pending = pending_batches(conn, stage, run_key)
            if pending:
                raise RunStateError(f"run ({stage!r}, {run_key!r}) has {len(pending)} uncommitted batch(es) "
                                    f"{[b.batch_id for b in pending][:20]}; run slices first")
            receipt = dict(definition.finalize(conn, run_key))
            after = run_record(conn, stage, run_key)
            if after is None or after["status"] != "published":
                raise RunStateError(f"stage {stage!r} finalize returned without publishing the run "
                                    "(it must call mark_published inside its swap)")
        conn.execute("CHECKPOINT")
    finally:
        conn.close()
    assert_no_wal(db_path)
    receipt["wal_after_close"] = None
    return receipt
