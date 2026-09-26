"""Evaluation cache (L4): per-feature evaluation cells keyed by (feature, labels, evaluation spec).

Layout (root ``C:/atx/atx-db/data/research`` by default)::

    eval/<feature_sha>/<label_sha>/<eval_spec_sha>.parquet          one row per evaluation cell
    eval/<feature_sha>/<label_sha>/<eval_spec_sha>.<table>.parquet  optional per-feature tables
                                                                      (series, slices, quantiles, decay)
    eval/<feature_sha>/<label_sha>/<eval_spec_sha>.json             sidecar (rows, file sha256, meta)

A cell depends only on its feature file, its label set and the evaluation spec, so a new
feature costs one feature's evaluation: :meth:`EvalCache.missing` lists the keys to compute.
Family-level statistics (BH q, Holm, DSR ``n_trials``, ``family_best``) depend on the whole
family and the trial registry; they are recomputed over cached cells, never cached here.

``eval_spec_sha`` = sha256 of canonical JSON of the spec payload (:func:`compute_eval_spec_sha`;
for R3b, ``evaluation.spec_payload(spec)`` without the per-run fields). Files are written
once (temp + rename) and never overwritten.
"""

from __future__ import annotations

import datetime as dt
import json
import os
import re
import time
from collections.abc import Mapping, Sequence
from pathlib import Path
from typing import TYPE_CHECKING, Any

import duckdb
import pyarrow as pa
import pyarrow.parquet as pq

from .research_lake import canonical_json, connect_bounded, sha256_file, sha256_text, sql_text

if TYPE_CHECKING:
    import pandas as pd

EVAL_CACHE_VERSION = "research-eval-cache-v1"
DEFAULT_ROOT = Path("C:/atx/atx-db/data/research")
CELL_KEY = ("basis", "feature_id", "variant", "horizon_months")
#: Spec fields that identify a run, not its results (excluded from the spec sha).
RUN_FIELDS = ("run_id", "feature_versions")
_SHA = re.compile(r"^[0-9a-f]{64}$")
_TABLE = re.compile(r"^[a-z][a-z0-9_]{0,31}$")


class EvalCacheError(ValueError):
    """The evaluation cache cannot write or read cells under its contract."""


def compute_eval_spec_sha(payload: Mapping[str, object]) -> str:
    """sha256 of canonical JSON of the evaluation spec payload (run-identity fields removed)."""
    body = {key: value for key, value in payload.items() if key not in RUN_FIELDS}
    return sha256_text(canonical_json({"cache_version": EVAL_CACHE_VERSION, "spec": body}))


def _check(value: str, label: str) -> None:
    if not isinstance(value, str) or not _SHA.fullmatch(value):
        raise EvalCacheError(f"{label} {value!r} must be a sha256 hex digest")


def _as_table(frame: pa.Table | pd.DataFrame) -> pa.Table:
    if isinstance(frame, pa.Table):
        return frame
    return pa.Table.from_pandas(frame, preserve_index=False)


class EvalCache:
    """Cells per ``(feature_sha, label_sha, eval_spec_sha)``; read via DuckDB."""

    def __init__(self, root: Path = Path("C:/atx/atx-db/data/research"), *, memory_limit: str = "256MB",
                 threads: int = 1) -> None:
        self.root = Path(root)
        self.memory_limit = memory_limit
        self.threads = threads
        self._con: duckdb.DuckDBPyConnection | None = None

    def path(self, feature_sha: str, label_sha: str, eval_spec_sha: str, table: str = "cells") -> Path:
        for value, label in ((feature_sha, "feature_sha"), (label_sha, "label_sha"),
                             (eval_spec_sha, "eval_spec_sha")):
            _check(value, label)
        if not _TABLE.fullmatch(table):
            raise EvalCacheError(f"table name {table!r} must match {_TABLE.pattern}")
        name = f"{eval_spec_sha}.parquet" if table == "cells" else f"{eval_spec_sha}.{table}.parquet"
        return self.root / "eval" / feature_sha / label_sha / name

    def meta_path(self, feature_sha: str, label_sha: str, eval_spec_sha: str) -> Path:
        return self.path(feature_sha, label_sha, eval_spec_sha).with_suffix(".json")

    def has(self, feature_sha: str, label_sha: str, eval_spec_sha: str) -> bool:
        return self.path(feature_sha, label_sha, eval_spec_sha).is_file() and \
            self.meta_path(feature_sha, label_sha, eval_spec_sha).is_file()

    def missing(self, keys: Sequence[tuple[str, str, str]]) -> list[tuple[str, str, str]]:
        """The keys without cached cells (what an incremental run must compute)."""
        return [key for key in keys if not self.has(*key)]

    def write(self, feature_sha: str, label_sha: str, eval_spec_sha: str, cells: pa.Table | pd.DataFrame,
              meta: Mapping[str, object] | None = None,
              extra: Mapping[str, pa.Table | pd.DataFrame] | None = None) -> Path:
        """Write the cells (and optional per-feature tables) once; an existing entry is kept."""
        target = self.path(feature_sha, label_sha, eval_spec_sha)
        if self.has(feature_sha, label_sha, eval_spec_sha):
            return target
        table = _as_table(cells)
        if table.num_rows == 0:
            raise EvalCacheError("no cells to cache")
        present = [name for name in CELL_KEY if name in table.column_names]
        if present:
            keys = list(zip(*(table.column(name).to_pylist() for name in present), strict=True))
            if len(set(keys)) != len(keys):
                raise EvalCacheError(f"one cell per {tuple(present)}")
        tables = {"cells": table, **{name: _as_table(frame) for name, frame in (extra or {}).items()}}
        target.parent.mkdir(parents=True, exist_ok=True)
        stamp = f"{os.getpid()}.{time.monotonic_ns()}"
        written: list[tuple[Path, Path]] = []
        try:
            files = {}
            for name, item in tables.items():
                final = self.path(feature_sha, label_sha, eval_spec_sha, name)
                tmp = final.with_name(f".{final.name}.{stamp}.tmp")
                pq.write_table(item, tmp, compression="zstd")
                written.append((tmp, final))
                files[name] = {"rows": item.num_rows, "bytes": tmp.stat().st_size, "sha256": sha256_file(tmp)}
            sidecar = {"cache_version": EVAL_CACHE_VERSION, "feature_sha": feature_sha, "label_sha": label_sha,
                       "eval_spec_sha": eval_spec_sha, "files": files,
                       "written_at": dt.datetime.now(dt.UTC).replace(tzinfo=None, microsecond=0).isoformat(),
                       "meta": json.loads(canonical_json(dict(meta or {})))}
            meta_tmp = target.with_name(f".{target.stem}.{stamp}.json.tmp")
            meta_tmp.write_text(json.dumps(sidecar, indent=1, sort_keys=True) + "\n", encoding="utf-8")
            written.append((meta_tmp, self.meta_path(feature_sha, label_sha, eval_spec_sha)))
            for tmp, final in written:      # the sidecar last: has() needs it
                os.replace(tmp, final)
        except BaseException:
            for tmp, _ in written:
                if tmp.exists():
                    tmp.unlink()
            raise
        return target

    def read_meta(self, feature_sha: str, label_sha: str, eval_spec_sha: str) -> dict[str, Any]:
        path = self.meta_path(feature_sha, label_sha, eval_spec_sha)
        if not path.is_file():
            raise EvalCacheError(f"no cached cells for {feature_sha[:12]}/{label_sha[:12]}/{eval_spec_sha[:12]}")
        return json.loads(path.read_text(encoding="utf-8"))

    def read(self, feature_sha: str, label_sha: str, eval_spec_sha: str, table: str = "cells") -> pa.Table:
        meta = self.read_meta(feature_sha, label_sha, eval_spec_sha)
        if table not in meta["files"]:
            raise EvalCacheError(f"cached entry has no {table!r} table")
        return pq.read_table(self.path(feature_sha, label_sha, eval_spec_sha, table))

    @property
    def con(self) -> duckdb.DuckDBPyConnection:
        if self._con is None:
            self._con = connect_bounded(None, root=self.root, memory_limit=self.memory_limit, threads=self.threads)
        return self._con

    def close(self) -> None:
        if self._con is not None:
            self._con.close()
            self._con = None

    def scan(self, keys: Sequence[tuple[str, str, str]], table: str = "cells") -> duckdb.DuckDBPyRelation:
        """Cached rows of many keys in one relation (``feature_sha``/``label_sha``/``eval_spec_sha`` added)."""
        parts = []
        for feature_sha, label_sha, eval_spec_sha in keys:
            if not self.has(feature_sha, label_sha, eval_spec_sha):
                raise EvalCacheError(f"not cached: {feature_sha[:12]}/{label_sha[:12]}/{eval_spec_sha[:12]}")
            path = self.path(feature_sha, label_sha, eval_spec_sha, table)
            parts.append(f"SELECT {sql_text(feature_sha)} AS feature_sha, {sql_text(label_sha)} AS label_sha, "
                         f"{sql_text(eval_spec_sha)} AS eval_spec_sha, * FROM read_parquet("
                         f"{sql_text(path.as_posix())}, union_by_name=true)")
        if not parts:
            raise EvalCacheError("scan at least one key")
        return self.con.sql(" UNION ALL BY NAME ".join(parts))


__all__ = [
    "CELL_KEY",
    "EVAL_CACHE_VERSION",
    "EvalCache",
    "EvalCacheError",
    "compute_eval_spec_sha",
]
