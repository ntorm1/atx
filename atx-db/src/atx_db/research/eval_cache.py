"""Evaluation cache (L4): per-feature evaluation cells keyed by (feature, labels, evaluation spec).

Layout (root ``C:/atx/atx-db/data/research`` by default)::

    eval/<feature_sha>/<label_sha>/<eval_spec_sha>.parquet          one row per evaluation cell
    eval/<feature_sha>/<label_sha>/<eval_spec_sha>.<table>.parquet  optional per-feature tables
                                                                      (series, slices, quantiles, decay)
    eval/<feature_sha>/<label_sha>/<eval_spec_sha>.json             sidecar (rows, file sha256, meta)

A cell depends on its feature file (``feature_sha``), the labels read (``label_sha``) and
everything else the R3b engine reads, which the third component covers (1.9 fix rounds 1-2):
``eval_spec_sha`` = sha256 of canonical JSON of

* the evaluation spec payload (``evaluation.spec_payload(spec)`` without the run-identity fields);
* the **basis** (:func:`basis_key`): its name, status, ``meta`` strings (copied into every
  cell) and its **prepared digests** from R3b's own ``_prepare`` (:func:`basis_prepared_digests`):
  calendar (formation, cutoff, entry), the labels actually read per horizon (so the label read
  window), the context/spine (universe flags, size, venue), supplied NYSE breakpoints,
  investable and population flags, and every control feature's values;
* the label read window (``label_sha``, ``eom_before``, ``allow_holdout``, ``holdout_wave``);
* the R3b code digest (``evaluation._code_sha()``: evaluation.py, stats.py, labels.py) and the
  store -> R3b adapter's (``feature_store.adapter_code_digest()``).

The key is computed from live inputs **before** ``evaluate_bases`` (which releases the frames of
``release_frames=True`` inputs): :func:`basis_prepared_digests` refuses released or empty
frames, and :meth:`EvalCache.write` refuses cells unless the key's basis block equals the
evaluation's own record of what it read (``EvaluationTables.bases[basis]``). Any change in the
parts is a new key, so a stale cell is never served; the sidecar keeps the parts
(``key_parts``) so a reader can see why a key missed. A new feature still costs one feature's
evaluation: :meth:`EvalCache.missing` lists the keys to compute. Family-level statistics
(BH q, Holm, DSR ``n_trials``, ``family_best``) depend on the whole family and the trial
registry; they are recomputed over cached cells, never cached here. Files are written once
(temp + rename) and never overwritten.
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

EVAL_CACHE_VERSION = "research-eval-cache-v2"
DEFAULT_ROOT = Path("C:/atx/atx-db/data/research")
CELL_KEY = ("basis", "feature_id", "variant", "horizon_months")
#: Spec fields that identify a run, not its results (excluded from the spec sha).
RUN_FIELDS = ("run_id", "feature_versions")
_SHA = re.compile(r"^[0-9a-f]{64}$")
_TABLE = re.compile(r"^[a-z][a-z0-9_]{0,31}$")


class EvalCacheError(ValueError):
    """The evaluation cache cannot write or read cells under its contract."""


#: Keys a label read window names (``eom_before`` ISO date; ``holdout_wave`` may be None).
LABEL_WINDOW_KEYS = ("label_sha", "eom_before", "allow_holdout", "holdout_wave")
#: Prepared digests every R3b basis has (the rest depend on the inputs supplied).
REQUIRED_PREPARED = ("calendar_sha256", "context_sha256")
#: The fields of an evaluation's own basis record (``EvaluationTables.bases[basis]``) the key covers.
BASIS_KEY_FIELDS = ("basis", "status", "meta", "prepared_digests")


def basis_prepared_digests(inputs: Any, spec: Any) -> dict[str, str]:
    """R3b's prepared digests of one basis, computed without evaluating (and without releasing its frames).

    Runs ``evaluation._prepare`` on a copy of ``inputs`` with ``release_frames=False`` (the
    caller's frames stay intact for the evaluation itself) and returns ``prep.digests``.
    Refuses a basis whose ``labels`` or ``context`` frame (or ``controls``, when the spec
    names control features) is empty while its calendar has formed months -- e.g.
    ``release_frames=True`` inputs after ``evaluate_bases`` released them: digests of empty
    frames would give different bases one key (1.9 re-review I3). Compute keys before
    evaluating.
    """
    from dataclasses import replace

    from . import evaluation as ev

    checked = ev.validate_spec(spec)
    calendar = inputs.calendar
    formed = int((calendar["status"] == ev.CALENDAR_FORMED).sum()) \
        if len(calendar) and "status" in calendar else 0
    if formed:
        empty = [name for name in ("labels", "context") if not len(getattr(inputs, name))]
        if checked.control_features and not len(inputs.controls):
            empty.append("controls")
        if empty:
            state = ("released by an evaluation (release_frames=True inputs after evaluate_bases)"
                     if inputs.release_frames else "empty")
            raise EvalCacheError(f"basis {inputs.basis}: the {empty} frames are {state} while the calendar has "
                                 f"{formed} formed months; digests of empty frames never key a cache entry "
                                 "(compute the key from live inputs, before evaluate_bases)")
    prep = ev._prepare(replace(inputs, release_frames=False), checked)
    digests = {str(k): str(v) for k, v in prep.digests.items()}
    del prep
    return digests


def basis_key(inputs: Any, spec: Any) -> dict[str, Any]:
    """The key's basis block: name, status, ``meta`` and :func:`basis_prepared_digests` (live inputs only).

    It has the shape of the evaluation's own record ``EvaluationTables.bases[basis]``
    restricted to :data:`BASIS_KEY_FIELDS`, which :meth:`EvalCache.write` compares it with.
    """
    return json.loads(canonical_json({"basis": str(inputs.basis), "status": str(inputs.status),
                                      "meta": dict(inputs.meta),
                                      "prepared_digests": basis_prepared_digests(inputs, spec)}))


def _basis_block(basis: Mapping[str, Any]) -> dict[str, Any]:
    if not isinstance(basis, Mapping) or set(basis) != set(BASIS_KEY_FIELDS):
        raise EvalCacheError(f"basis names exactly {BASIS_KEY_FIELDS}: pass basis_key(inputs, spec)")
    prepared = basis["prepared_digests"]
    missing = [key for key in REQUIRED_PREPARED if not isinstance(prepared, Mapping) or key not in prepared]
    if missing:
        raise EvalCacheError(f"basis prepared_digests lack {missing}: pass basis_key(inputs, spec)")
    return json.loads(canonical_json(dict(basis)))


def eval_key_parts(payload: Mapping[str, object], *, basis: Mapping[str, Any],
                   label_window: Mapping[str, object], code_sha: str | None = None,
                   adapter_sha: str | None = None) -> dict[str, Any]:
    """The canonical parts of an evaluation cache key (see the module docstring).

    ``basis`` is :func:`basis_key`; ``code_sha`` defaults to ``evaluation._code_sha()`` and
    ``adapter_sha`` to ``feature_store.adapter_code_digest()``.
    """
    block = _basis_block(basis)
    if set(label_window) != set(LABEL_WINDOW_KEYS):
        raise EvalCacheError(f"label_window names exactly {LABEL_WINDOW_KEYS}")
    _check(str(label_window["label_sha"]), "label_window['label_sha']")
    dt.date.fromisoformat(str(label_window["eom_before"]))
    if not isinstance(label_window["allow_holdout"], bool):
        raise EvalCacheError("label_window['allow_holdout'] must be a bool")
    if code_sha is None:
        from . import evaluation as ev

        code_sha = ev._code_sha()
    if adapter_sha is None:
        from .feature_store import adapter_code_digest

        adapter_sha = adapter_code_digest()
    _check(code_sha, "code_sha")
    _check(adapter_sha, "adapter_sha")
    body = {key: value for key, value in payload.items() if key not in RUN_FIELDS}
    return json.loads(canonical_json({"cache_version": EVAL_CACHE_VERSION, "spec": body, "basis": block,
                                      "label_window": dict(label_window), "code_sha": code_sha,
                                      "adapter_sha": adapter_sha}))


def compute_eval_spec_sha(payload: Mapping[str, object], *, basis: Mapping[str, Any],
                          label_window: Mapping[str, object], code_sha: str | None = None,
                          adapter_sha: str | None = None) -> str:
    """The cache key's third component: sha256 of :func:`eval_key_parts`."""
    return sha256_text(canonical_json(eval_key_parts(payload, basis=basis, label_window=label_window,
                                                     code_sha=code_sha, adapter_sha=adapter_sha)))


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
              extra: Mapping[str, pa.Table | pd.DataFrame] | None = None, *,
              key_parts: Mapping[str, Any], evaluated: Mapping[str, Any]) -> Path:
        """Write the cells (and optional per-feature tables) once; an existing entry is kept.

        ``key_parts`` (:func:`eval_key_parts`) must hash to ``eval_spec_sha`` and name the
        same ``label_sha``; they are kept in the sidecar. ``evaluated`` is the evaluation's own
        record of the basis the cells come from (``EvaluationTables.bases[basis]``): its
        name, status, meta and ``prepared_digests`` must equal the key's basis block, so cells
        are cached only under the key of the inputs that were actually evaluated (I3).
        """
        target = self.path(feature_sha, label_sha, eval_spec_sha)
        if sha256_text(canonical_json(dict(key_parts))) != eval_spec_sha:
            raise EvalCacheError("key_parts do not hash to eval_spec_sha (compute it with compute_eval_spec_sha)")
        if key_parts.get("label_window", {}).get("label_sha") != label_sha:
            raise EvalCacheError("key_parts name another label_sha than the cache key")
        if not isinstance(evaluated, Mapping) or "prepared_digests" not in evaluated:
            raise EvalCacheError("evaluated= is the evaluation's record of the basis (EvaluationTables.bases[basis], "
                                 "with its prepared_digests)")
        recorded = json.loads(canonical_json({key: evaluated.get(key) for key in BASIS_KEY_FIELDS}))
        keyed = key_parts.get("basis") or {}
        if recorded != keyed:
            differ = sorted(key for key in BASIS_KEY_FIELDS if recorded.get(key) != keyed.get(key))
            digests = sorted(name for name in {*recorded["prepared_digests"], *keyed.get("prepared_digests", {})}
                             if recorded["prepared_digests"].get(name) != keyed.get("prepared_digests", {}).get(name))
            raise EvalCacheError(f"key_parts' basis differs from what the evaluation read ({differ}; digests {digests}): "
                                 "the cells were computed under another key")
        table = _as_table(cells)
        if "basis" in table.column_names and set(table.column("basis").to_pylist()) - {recorded["basis"]}:
            raise EvalCacheError(f"cells of another basis than the evaluated {recorded['basis']!r}")
        if self.has(feature_sha, label_sha, eval_spec_sha):
            return target
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
                       "eval_spec_sha": eval_spec_sha, "files": files, "key_parts": dict(key_parts),
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
    "BASIS_KEY_FIELDS",
    "CELL_KEY",
    "EVAL_CACHE_VERSION",
    "LABEL_WINDOW_KEYS",
    "EvalCache",
    "EvalCacheError",
    "basis_key",
    "basis_prepared_digests",
    "compute_eval_spec_sha",
    "eval_key_parts",
]
