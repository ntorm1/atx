"""Content-addressed per-feature Parquet store (L2) and its R3b input adapter.

Layout (root ``C:/atx/atx-db/data/research`` by default)::

    features/<basis>/<feature_id>/<feature_sha>.parquet   one immutable file per feature version
    features/<basis>/<feature_id>/<feature_sha>.json      its sidecar (rows, reasons, digests, meta)
    manifests/<wave>/<manifest_sha>.json                   [{feature_id, feature_sha, basis}, ...]

File schema (:data:`FEATURE_SCHEMA`), sorted by ``(eom, line_id)``, one row per universe line
and month end::

    eom DATE             calendar month end of the formation month
    line_id VARCHAR      price line (e.g. TBLTICKERHISTORY-<vendor id>)
    owner_id VARCHAR     issuer (CIK) or NULL for a line without an owner link
    raw DOUBLE           the feature value as defined (unoriented)
    signed DOUBLE        catalog orientation x raw (R2b ``signed_raw``)
    rank_u DOUBLE        Blom position (r - 3/8) / (n + 1/4) of the signed base, in (0, 1);
                         R2b ``rank_normal`` = Phi^-1(rank_u) exactly
    z DOUBLE             per-formation sample z-score of the winsorized signed base (R2b ``zscore``)
    reason INT8          :data:`REASONS` (0 valid; < 64 a value slot; >= 64 a universe row without one)
    clock_offset_s INT32 ceil(seconds from ``eom`` 22:00 UTC to the value's latest input clock)

``feature_sha`` = sha256 of canonical JSON of (feature id, catalog row + transform policy, code
digest, input digests) (:func:`compute_feature_sha`): any change of definition, code or input
data is a new file; an identical rebuild is a no-op (:meth:`FeatureStore.write` returns the
existing path). Nothing is ever overwritten.

Cross-sectional transforms are R2b's (:func:`atx_db.research.features.standardize_frame`,
imported, not forked): :func:`standardize_to_store` feeds whole formations to it and keeps
``signed_raw`` and ``zscore``; ``rank_u`` is the Blom position R2b's ``rank_normal`` is the
inverse normal of, recomputed with the same imported rank and checked bit-for-bit against
``standardize_frame``'s ``rank_normal`` on every write (a drift in R2b fails the write).
Industry/size-neutral variants need covariates (industry, verified size) that are not
feature data; they are computed on read from the spine with the same R2b function, not
stored.

A dense file (every universe row, the ones without a value carrying a reason >= 64) carries
its own coverage denominator; :func:`load_feature_table_from_store` feeds it to the R3b
engine (ruling C-44: this adapter lives here, not in ``evaluation.py``).

Add one feature: build a frame per formation-year (``eom, line_id, owner_id, raw, status,
available_at``) from lake inputs, ``sha = compute_feature_sha(...)``, ``if not store.has(...)``:
``store.write(basis, feature_id, sha, (standardize_to_store(chunk, ...) for chunk in years),
meta)``, then list ``{feature_id, feature_sha, basis}`` in the wave manifest
(:func:`write_manifest`).
"""

from __future__ import annotations

import datetime as dt
import json
import os
import re
import time
from collections.abc import Callable, Iterable, Mapping, Sequence
from dataclasses import dataclass, field
from pathlib import Path
from typing import TYPE_CHECKING, Any

import duckdb
import numpy as np
import pyarrow as pa
import pyarrow.parquet as pq

from .research_lake import canonical_json, connect_bounded, sha256_file, sha256_text, sql_text

if TYPE_CHECKING:  # heavy modules load lazily (writer / adapter paths only)
    import pandas as pd

    from .evaluation import CatalogFeature, FeatureData
    from .features import StandardizationPolicy

FEATURE_STORE_VERSION = "research-feature-parquet-v1"
DEFAULT_ROOT = Path("C:/atx/atx-db/data/research")
#: The decision clock of a month end: ``clock_offset_s`` counts from ``eom`` at 22:00 UTC.
DECISION_HOUR = 22

FEATURE_SCHEMA = pa.schema([
    pa.field("eom", pa.date32(), nullable=False),
    pa.field("line_id", pa.string(), nullable=False),
    pa.field("owner_id", pa.string()),
    pa.field("raw", pa.float64()),
    pa.field("signed", pa.float64()),
    pa.field("rank_u", pa.float64()),
    pa.field("z", pa.float64()),
    pa.field("reason", pa.int8(), nullable=False),
    pa.field("clock_offset_s", pa.int32()),
])

#: Append-only reason codes. Below :data:`VALUE_SLOT_LIMIT` a row has a value slot (R2b's
#: matrix rows: universe, panel validity and size gates passed); from it on, a universe row
#: without a candidate value (counted in the coverage denominator, never ranked).
REASONS: Mapping[str, int] = {
    "valid": 0,
    "thin_cross_section": 1,          # in domain; formation below min_names: signed kept, rank_u/z NULL
    "nonpositive_value": 10,
    "missing_denominator": 11,
    "nonpositive_denominator": 12,
    "negative_book": 13,
    "missing_book_operand": 14,
    "loss_firm": 15,
    "missing_loss_operand": 16,
    "zero_payer": 17,
    "nonfinite_value": 18,
    "missing_input": 64,              # the input row is absent or invalid (e.g. R2a panel reason not valid)
    "insufficient_obs": 65,           # a window statistic below its minimum observations
    "stale_input": 66,
    "not_visible_at_cutoff": 67,
    "unverified_size": 68,            # a size feature on an unverified share count
    "no_session_bar": 69,             # the line has no bar at the formation session
}
REASON_NAMES = {code: name for name, code in REASONS.items()}
VALUE_SLOT_LIMIT = 64
IN_DOMAIN = "in_domain"
#: Store column -> R3b variant served by the adapter (``rank_normal`` is derived from ``rank_u``).
STORE_VARIANTS: tuple[str, ...] = ("rank_normal", "signed_raw", "zscore")
ADAPTER_VARIANTS: tuple[str, ...] = ("rank_normal", "rank_u", "signed_raw", "zscore")

_NAME = re.compile(r"^[a-z][a-z0-9_]{0,63}$")
_SHA = re.compile(r"^[0-9a-f]{64}$")
_WAVE = re.compile(r"^[a-z0-9][a-z0-9_.-]{0,63}$")


class FeatureStoreParquetError(ValueError):
    """The Parquet feature store cannot write, read or adapt a feature under its contract."""


# ---------------------------------------------------------------------------
# Identity
# ---------------------------------------------------------------------------

def compute_feature_sha(feature_id: str, catalog_row: Mapping[str, object], code_digest: str,
                        input_digests: Mapping[str, str]) -> str:
    """sha256 of canonical JSON of everything that determines a feature file's values.

    ``catalog_row``: the catalog row *and* the transform policy (winsor limits,
    ``min_names``, library versions; see :func:`catalog_row_payload`). ``code_digest``:
    :func:`store_code_digest` (plus the caller's definition module). ``input_digests``: the
    lake dataset digests (or retained-file sha256) read.
    """
    _check_name(feature_id, "feature_id")
    if not isinstance(code_digest, str) or not code_digest:
        raise FeatureStoreParquetError("code_digest must be a non-empty digest")
    payload = {"store_version": FEATURE_STORE_VERSION, "feature_id": feature_id,
               "catalog_row": dict(catalog_row), "code_digest": code_digest,
               "input_digests": {str(k): str(v) for k, v in sorted(input_digests.items())}}
    return sha256_text(canonical_json(payload))


def catalog_row_payload(entry: Any, policy: StandardizationPolicy | None = None,
                        extra: Mapping[str, object] | None = None) -> dict[str, object]:
    """A JSON-able catalog row (an R1a ``AnomalyCatalogEntry`` or a mapping) plus the policy."""
    import pandas as pd

    from . import features as rf

    policy = policy or rf.StandardizationPolicy()
    if isinstance(entry, Mapping):
        row = dict(entry)
    else:
        row = {name: getattr(entry, name) for name in getattr(entry, "__dataclass_fields__", {})}
    row["_policy"] = {"winsor_limits": list(policy.winsor_limits), "min_names": policy.min_names,
                      "rank_u": "blom (r - 3/8) / (n + 1/4), average ranks",
                      "libraries": {"numpy": np.__version__, "pandas": pd.__version__}}
    if extra:
        row["_extra"] = dict(extra)
    return json.loads(canonical_json(row))


def store_code_digest(*extra: Path | str) -> str:
    """Digest of the code that determines stored values (the ``code_digest`` of the store's part).

    Covers the writer transform (:func:`standardize_to_store`, :func:`_clock_offsets`, the
    schema and reason codes), R2b's ``standardize_frame`` / ``_inverse_normal`` / ``_floats``
    / ``StandardizationPolicy`` sources and the whole ``factors.cross_section`` module, plus
    ``extra`` files (the caller's feature definition module). Unrelated edits elsewhere in
    ``features.py`` do not invalidate stored features.
    """
    import hashlib
    import inspect

    from ..factors import cross_section
    from . import features as rf

    digest = hashlib.sha256()
    for part in (FEATURE_STORE_VERSION, str(FEATURE_SCHEMA), canonical_json(dict(REASONS)),
                 inspect.getsource(standardize_to_store), inspect.getsource(_clock_offsets),
                 inspect.getsource(rf.standardize_frame), inspect.getsource(rf._inverse_normal),
                 inspect.getsource(rf._floats), inspect.getsource(rf.StandardizationPolicy)):
        digest.update(part.replace("\r\n", "\n").encode("utf-8"))
    for path in (Path(cross_section.__file__), *(Path(p) for p in extra)):
        digest.update(path.name.encode("utf-8"))
        digest.update(path.read_bytes().replace(b"\r\n", b"\n"))
    return digest.hexdigest()


def _check_name(value: str, label: str) -> None:
    if not isinstance(value, str) or not _NAME.fullmatch(value):
        raise FeatureStoreParquetError(f"{label} {value!r} must match {_NAME.pattern}")


def _check_sha(value: str) -> None:
    if not isinstance(value, str) or not _SHA.fullmatch(value):
        raise FeatureStoreParquetError(f"feature_sha {value!r} must be a sha256 hex digest")


# ---------------------------------------------------------------------------
# Store
# ---------------------------------------------------------------------------

class _StreamCheck:
    """Validates a feature stream batch by batch in O(1) memory; accumulates the sidecar stats."""

    def __init__(self) -> None:
        self.rows = 0
        self.last: tuple[int, str] | None = None
        self.reasons: dict[int, int] = {}
        self.eom_min: int | None = None
        self.eom_max: int | None = None
        self.years: set[int] = set()

    def add(self, batch: pa.RecordBatch) -> None:
        n = batch.num_rows
        if n == 0:
            return
        eom = batch.column("eom").cast(pa.int32()).to_numpy(zero_copy_only=False)
        line = np.asarray(batch.column("line_id").to_pylist(), dtype=object)
        if batch.column("line_id").null_count or batch.column("eom").null_count:
            raise FeatureStoreParquetError("eom and line_id are never NULL")
        keys_ok = (eom[1:] > eom[:-1]) | ((eom[1:] == eom[:-1]) & (line[1:] > line[:-1]))
        if not keys_ok.all() or (self.last is not None and (int(eom[0]), str(line[0])) <= self.last):
            raise FeatureStoreParquetError("rows must be strictly increasing in (eom, line_id): unique, sorted")
        self.last = (int(eom[-1]), str(line[-1]))
        reason = batch.column("reason").to_numpy(zero_copy_only=False).astype(np.int16)
        unknown = set(np.unique(reason).tolist()) - set(REASON_NAMES)
        if unknown:
            raise FeatureStoreParquetError(f"unknown reason codes {sorted(unknown)}")
        slot = reason < VALUE_SLOT_LIMIT
        clock = batch.column("clock_offset_s")
        clock_null = np.asarray(clock.is_null().to_numpy(zero_copy_only=False), dtype=bool)
        if (slot & clock_null).any():
            raise FeatureStoreParquetError(f"{int((slot & clock_null).sum())} value rows without a clock")
        offsets = clock.fill_null(0).to_numpy(zero_copy_only=False)
        if (slot & (offsets > 0)).any():
            raise FeatureStoreParquetError(
                f"{int((slot & (offsets > 0)).sum())} value rows whose clock is after their month end's 22:00 UTC "
                "decision clock (look-ahead)")
        valid = reason == REASONS["valid"]
        signed_null = np.asarray(batch.column("signed").is_null().to_numpy(zero_copy_only=False), dtype=bool)
        if (valid & signed_null).any():
            raise FeatureStoreParquetError("a valid row has no signed value")
        rank = batch.column("rank_u").to_numpy(zero_copy_only=False)
        with np.errstate(invalid="ignore"):
            outside = np.isfinite(rank) & ((rank <= 0.0) | (rank >= 1.0))
        if outside.any():
            raise FeatureStoreParquetError("rank_u must lie in (0, 1)")
        for code, count in zip(*np.unique(reason, return_counts=True), strict=True):
            self.reasons[int(code)] = self.reasons.get(int(code), 0) + int(count)
        self.rows += n
        low, high = int(eom.min()), int(eom.max())
        self.eom_min = low if self.eom_min is None else min(self.eom_min, low)
        self.eom_max = high if self.eom_max is None else max(self.eom_max, high)
        epoch = dt.date(1970, 1, 1)
        self.years.update({(epoch + dt.timedelta(days=int(d))).year for d in np.unique(eom)})


def _day(days: int | None) -> str | None:
    return None if days is None else (dt.date(1970, 1, 1) + dt.timedelta(days=days)).isoformat()


class FeatureStore:
    """``features/<basis>/<feature_id>/<feature_sha>.parquet`` with a JSON sidecar; read via DuckDB."""

    def __init__(self, root: Path = Path("C:/atx/atx-db/data/research"), *, memory_limit: str = "256MB",
                 threads: int = 1) -> None:
        self.root = Path(root)
        self.memory_limit = memory_limit
        self.threads = threads
        self._con: duckdb.DuckDBPyConnection | None = None

    # -- paths ---------------------------------------------------------------
    def path(self, basis: str, feature_id: str, feature_sha: str) -> Path:
        _check_name(basis, "basis")
        _check_name(feature_id, "feature_id")
        _check_sha(feature_sha)
        return self.root / "features" / basis / feature_id / f"{feature_sha}.parquet"

    def meta_path(self, basis: str, feature_id: str, feature_sha: str) -> Path:
        return self.path(basis, feature_id, feature_sha).with_suffix(".json")

    def has(self, basis: str, feature_id: str, feature_sha: str) -> bool:
        return self.path(basis, feature_id, feature_sha).is_file() and \
            self.meta_path(basis, feature_id, feature_sha).is_file()

    def read_meta(self, basis: str, feature_id: str, feature_sha: str) -> dict[str, Any]:
        path = self.meta_path(basis, feature_id, feature_sha)
        if not path.is_file() or not self.path(basis, feature_id, feature_sha).is_file():
            raise FeatureStoreParquetError(f"feature {basis}/{feature_id}/{feature_sha} is not in the store")
        meta = json.loads(path.read_text(encoding="utf-8"))
        if meta.get("store_version") != FEATURE_STORE_VERSION:
            raise FeatureStoreParquetError(f"{path}: unsupported store version {meta.get('store_version')!r}")
        return meta

    def feature_shas(self, basis: str, feature_id: str) -> list[str]:
        directory = self.root / "features" / basis / feature_id
        if not directory.is_dir():
            return []
        return sorted(p.stem for p in directory.glob("*.parquet") if _SHA.fullmatch(p.stem)
                      and p.with_suffix(".json").is_file())

    # -- write ---------------------------------------------------------------
    def write(self, basis: str, feature_id: str, feature_sha: str, batches: Iterable[pa.RecordBatch],
              meta: Mapping[str, object]) -> Path:
        """Stream ``batches`` into the feature's file (temp file + rename); idempotent per sha.

        ``meta`` must carry ``expected_sign`` (+1/-1/0, the catalog sign; the adapter serves
        it to R3b). The batches must follow :data:`FEATURE_SCHEMA`, strictly increasing in
        ``(eom, line_id)``; value rows need a clock at or before their month end's decision
        clock. An existing ``(basis, feature_id, feature_sha)`` is returned unchanged (the
        same sha is the same content), and ``batches`` is not consumed.
        """
        target = self.path(basis, feature_id, feature_sha)
        if self.has(basis, feature_id, feature_sha):
            return target
        sign = meta.get("expected_sign")
        if isinstance(sign, bool) or sign not in (-1, 0, 1):
            raise FeatureStoreParquetError("meta['expected_sign'] must be +1, -1 or 0 (two-sided)")
        target.parent.mkdir(parents=True, exist_ok=True)
        stamp = f"{os.getpid()}.{time.monotonic_ns()}"
        tmp = target.with_name(f".{feature_sha}.{stamp}.tmp")
        check = _StreamCheck()
        started = time.perf_counter()
        try:
            with pq.ParquetWriter(tmp, FEATURE_SCHEMA, compression="zstd", write_statistics=True) as writer:
                for batch in batches:
                    for part in (batch.to_batches() if isinstance(batch, pa.Table) else [batch]):
                        if part.schema.names != FEATURE_SCHEMA.names:
                            raise FeatureStoreParquetError(f"batch columns {part.schema.names} != "
                                                           f"{FEATURE_SCHEMA.names}")
                        part = part.cast(FEATURE_SCHEMA)
                        check.add(part)
                        writer.write_batch(part)
            if check.rows == 0:
                raise FeatureStoreParquetError(f"{basis}/{feature_id}: no rows to write")
            sidecar = {
                "store_version": FEATURE_STORE_VERSION, "basis": basis, "feature_id": feature_id,
                "feature_sha": feature_sha, "rows": check.rows,
                "value_rows": sum(n for code, n in check.reasons.items() if code < VALUE_SLOT_LIMIT),
                "reasons": {REASON_NAMES[code]: n for code, n in sorted(check.reasons.items())},
                "eom_min": _day(check.eom_min), "eom_max": _day(check.eom_max), "years": sorted(check.years),
                "bytes": tmp.stat().st_size, "file_sha256": sha256_file(tmp),
                "write_seconds": round(time.perf_counter() - started, 3),
                "written_at": dt.datetime.now(dt.UTC).replace(tzinfo=None, microsecond=0).isoformat(),
                "expected_sign": int(sign), "meta": json.loads(canonical_json(dict(meta))),
            }
            meta_tmp = target.with_name(f".{feature_sha}.{stamp}.json.tmp")
            meta_tmp.write_text(json.dumps(sidecar, indent=1, sort_keys=True) + "\n", encoding="utf-8")
            os.replace(tmp, target)          # data first; the sidecar publishes it (has() needs both)
            os.replace(meta_tmp, self.meta_path(basis, feature_id, feature_sha))
        except BaseException:
            for leftover in (tmp, target.with_name(f".{feature_sha}.{stamp}.json.tmp")):
                if leftover.exists():
                    leftover.unlink()
            raise
        return target

    # -- read ----------------------------------------------------------------
    @property
    def con(self) -> duckdb.DuckDBPyConnection:
        if self._con is None:
            self._con = connect_bounded(None, root=self.root, memory_limit=self.memory_limit,
                                        threads=self.threads)
        return self._con

    def close(self) -> None:
        if self._con is not None:
            self._con.close()
            self._con = None

    def __enter__(self) -> FeatureStore:
        return self

    def __exit__(self, *exc: object) -> None:
        self.close()

    def resolve(self, basis: str, feature_id: str, shas: Mapping[str, str] | None = None) -> str:
        """The pinned sha of a feature, or its only stored sha (several unpinned is an error)."""
        if shas is not None and feature_id in shas:
            sha = shas[feature_id]
            if not self.has(basis, feature_id, sha):
                raise FeatureStoreParquetError(f"pinned {basis}/{feature_id}/{sha} is not in the store")
            return sha
        found = self.feature_shas(basis, feature_id)
        if len(found) != 1:
            raise FeatureStoreParquetError(f"{basis}/{feature_id}: {len(found)} stored versions; pin one "
                                           "(a wave manifest lists feature_sha per feature)")
        return found[0]

    def scan(self, basis: str, feature_ids: Sequence[str], years: Sequence[int] | None = None, *,
             shas: Mapping[str, str] | None = None) -> duckdb.DuckDBPyRelation:
        """One relation over the features' files: ``feature_id`` + the file columns (+ year filter)."""
        if not feature_ids:
            raise FeatureStoreParquetError("scan at least one feature")
        where = ""
        if years is not None:
            chosen = sorted({int(y) for y in years})
            if not chosen:
                raise FeatureStoreParquetError("years must not be empty")
            where = " WHERE " + " OR ".join(
                f"eom BETWEEN DATE '{y:04d}-01-01' AND DATE '{y:04d}-12-31'" for y in chosen)
        parts = []
        for feature_id in feature_ids:
            path = self.path(basis, feature_id, self.resolve(basis, feature_id, shas))
            parts.append(f"SELECT {sql_text(feature_id)} AS feature_id, * FROM read_parquet("
                         f"{sql_text(path.as_posix())}){where}")
        return self.con.sql(" UNION ALL ".join(parts))


# ---------------------------------------------------------------------------
# Wave manifests
# ---------------------------------------------------------------------------

def _manifest_entries(entries: Sequence[Mapping[str, str]]) -> list[dict[str, str]]:
    rows = []
    for entry in entries:
        item = {"basis": str(entry["basis"]), "feature_id": str(entry["feature_id"]),
                "feature_sha": str(entry["feature_sha"])}
        _check_name(item["basis"], "basis")
        _check_name(item["feature_id"], "feature_id")
        _check_sha(item["feature_sha"])
        rows.append(item)
    keys = [(r["basis"], r["feature_id"]) for r in rows]
    if len(set(keys)) != len(keys):
        raise FeatureStoreParquetError("a manifest lists each (basis, feature_id) once")
    return sorted(rows, key=lambda r: (r["basis"], r["feature_id"]))


def write_manifest(root: Path, wave: str, entries: Sequence[Mapping[str, str]]) -> str:
    """Write ``manifests/<wave>/<manifest_sha>.json`` for stored features; returns the manifest sha.

    The sha covers the sorted entries only, so the same set is the same manifest (an
    existing file is kept). Every entry must be in the store.
    """
    if not isinstance(wave, str) or not _WAVE.fullmatch(wave):
        raise FeatureStoreParquetError(f"wave {wave!r} must match {_WAVE.pattern}")
    rows = _manifest_entries(entries)
    if not rows:
        raise FeatureStoreParquetError("a manifest lists at least one feature")
    store = FeatureStore(Path(root))
    missing = [f"{r['basis']}/{r['feature_id']}/{r['feature_sha']}" for r in rows
               if not store.has(r["basis"], r["feature_id"], r["feature_sha"])]
    if missing:
        raise FeatureStoreParquetError(f"manifest entries not in the store: {missing}")
    manifest_sha = sha256_text(canonical_json({"store_version": FEATURE_STORE_VERSION, "entries": rows}))
    target = Path(root) / "manifests" / wave / f"{manifest_sha}.json"
    if not target.is_file():
        target.parent.mkdir(parents=True, exist_ok=True)
        files = {f"{r['basis']}/{r['feature_id']}": store.read_meta(r["basis"], r["feature_id"],
                                                                     r["feature_sha"])["file_sha256"] for r in rows}
        payload = {"store_version": FEATURE_STORE_VERSION, "wave": wave, "manifest_sha": manifest_sha,
                   "entries": rows, "file_sha256": files,
                   "created_at": dt.datetime.now(dt.UTC).replace(tzinfo=None, microsecond=0).isoformat()}
        tmp = target.with_name(f".{manifest_sha}.{os.getpid()}.{time.monotonic_ns()}.tmp")
        tmp.write_text(json.dumps(payload, indent=1, sort_keys=True) + "\n", encoding="utf-8")
        os.replace(tmp, target)
    return manifest_sha


def read_manifest(root: Path, wave: str, manifest_sha: str) -> list[dict[str, str]]:
    path = Path(root) / "manifests" / wave / f"{manifest_sha}.json"
    if not path.is_file():
        raise FeatureStoreParquetError(f"manifest {wave}/{manifest_sha} not found")
    payload = json.loads(path.read_text(encoding="utf-8"))
    rows = _manifest_entries(payload["entries"])
    if sha256_text(canonical_json({"store_version": FEATURE_STORE_VERSION, "entries": rows})) != manifest_sha:
        raise FeatureStoreParquetError(f"manifest {wave}/{manifest_sha}: content does not match its sha")
    return rows


# ---------------------------------------------------------------------------
# Writer transform (R2b's standardize_frame, imported)
# ---------------------------------------------------------------------------

def _clock_offsets(eom: np.ndarray, available: np.ndarray) -> np.ndarray:
    """ceil((available_at - (eom + 22:00)) / 1 s) as float (NaN where no clock); never rounds a clock earlier."""
    ref = eom.astype("datetime64[D]").astype("datetime64[us]") + np.timedelta64(DECISION_HOUR, "h")
    stamps = available.astype("datetime64[us]")
    missing = np.isnat(stamps)
    delta = (stamps - ref).astype("int64")
    seconds = -((-delta) // 1_000_000)
    out = seconds.astype("float64")
    out[missing] = np.nan
    return out


def standardize_to_store(frame: pd.DataFrame, *, expected_sign: int, log_base: bool = False,
                         policy: StandardizationPolicy | None = None, feature_id: str = "feature") -> pa.Table:
    """One chunk of *whole* formations -> a :data:`FEATURE_SCHEMA` table (sorted by eom, line_id).

    ``frame`` columns: ``eom`` (month end), ``line_id``, ``owner_id`` (None for an unlinked
    line), ``raw`` (float, NaN allowed), ``status`` (``in_domain``, an R2b domain status, or a
    no-value :data:`REASONS` name) and ``available_at`` (naive UTC; NaT where none).
    Candidate rows (``in_domain`` and R2b domain statuses) go through
    :func:`features.standardize_frame` with R2b's orientation (the catalog sign; +1 for a
    two-sided hypothesis) and ``log_base`` (``log_winsor_z``). Every formation is
    independent, so chunking (e.g. per year) does not change a value.
    """
    import pandas as pd

    from ..factors import cross_section
    from . import features as rf

    if expected_sign not in (-1, 0, 1):
        raise FeatureStoreParquetError("expected_sign must be +1, -1 or 0")
    policy = policy or rf.StandardizationPolicy()
    orientation = expected_sign if expected_sign in (-1, 1) else 1
    need = ("eom", "line_id", "owner_id", "raw", "status", "available_at")
    missing = [name for name in need if name not in frame]
    if missing:
        raise FeatureStoreParquetError(f"frame lacks columns {missing}")
    work = pd.DataFrame({"eom": pd.to_datetime(frame["eom"]).dt.normalize(),
                         "line_id": frame["line_id"].astype(object), "owner_id": frame["owner_id"].astype(object),
                         "raw": pd.to_numeric(frame["raw"], errors="coerce").astype("float64"),
                         "status": frame["status"].astype(str),
                         "available_at": pd.to_datetime(frame["available_at"])})
    work = work.sort_values(["eom", "line_id"], kind="stable").reset_index(drop=True)
    if work.duplicated(["eom", "line_id"]).any():
        raise FeatureStoreParquetError("one row per (eom, line_id)")
    if work["line_id"].isna().any():
        raise FeatureStoreParquetError("line_id is never NULL")
    month_end = work["eom"] + pd.offsets.MonthEnd(0)
    if not (month_end == work["eom"]).all():
        raise FeatureStoreParquetError("eom must be a calendar month end")
    statuses = set(work["status"].unique())
    unknown = statuses - set(rf.DOMAIN_STATUSES) - {name for name, code in REASONS.items()
                                                     if code >= VALUE_SLOT_LIMIT}
    if unknown:
        raise FeatureStoreParquetError(f"unknown row statuses {sorted(unknown)}")
    size = len(work)
    signed = np.full(size, np.nan)
    rank_u = np.full(size, np.nan)
    z = np.full(size, np.nan)
    reason = np.full(size, -1, dtype=np.int16)
    candidate = work["status"].isin(rf.DOMAIN_STATUSES).to_numpy(dtype=bool)
    positions = np.flatnonzero(candidate)
    if len(positions):
        cand = work.iloc[positions]
        std_in = pd.DataFrame({"formation_date": cand["eom"].to_numpy(), "security_id": cand["line_id"].to_numpy(),
                               "raw_value": cand["raw"].to_numpy(), "domain_status": cand["status"].to_numpy(),
                               "industry_group": None, "log_size": np.nan})
        result, _stats = rf.standardize_frame(std_in, feature_id=feature_id, orientation=orientation,
                                              log_base=log_base, variants=("signed_raw", "winsor", "zscore",
                                                                           "rank_normal"), policy=policy)
        if not (np.array_equal(result["security_id"].to_numpy(dtype=object), cand["line_id"].to_numpy(dtype=object))
                and np.array_equal(pd.to_datetime(result["formation_date"]).to_numpy(), cand["eom"].to_numpy())):
            raise FeatureStoreParquetError("standardize_frame reordered rows unexpectedly")
        c_signed = result["signed_raw"].to_numpy(dtype=float)
        c_z = result["zscore"].to_numpy(dtype=float)
        c_normal = result["rank_normal"].to_numpy(dtype=float)
        ranked = np.flatnonzero(np.isfinite(c_normal))
        c_rank = np.full(len(positions), np.nan)
        if len(ranked):
            # The Blom position R2b takes the inverse normal of: the same imported rank, the same
            # arithmetic; checked bit for bit against standardize_frame's rank_normal below.
            raw = cand["raw"].to_numpy(dtype=float)[ranked]
            with np.errstate(divide="ignore", invalid="ignore"):
                base = np.log(raw) if log_base else raw
            rank_frame = pd.DataFrame({"as_of_date": cand["eom"].to_numpy()[ranked], "value": orientation * base},
                                      index=ranked)
            percent = cross_section.rank(rank_frame, partition_columns=("as_of_date",), method="average")
            names = rank_frame.groupby("as_of_date")["value"].transform("count").to_numpy(dtype=float)
            position = np.round(pd.to_numeric(percent["value"]).to_numpy(dtype=float) * (names - 1.0) * 2.0) / 2.0 + 1.0
            blom = (position - 0.375) / (names + 0.25)
            if not np.array_equal(rf._inverse_normal(blom), c_normal[ranked]):
                raise FeatureStoreParquetError(f"{feature_id}: rank_u does not reproduce R2b rank_normal bit for bit "
                                               "(features.standardize_frame changed; review this store)")
            c_rank[ranked] = blom
        status = cand["status"].to_numpy(dtype=object)
        in_domain = status == IN_DOMAIN
        c_reason = np.empty(len(positions), dtype=np.int16)
        for name in set(status.tolist()) - {IN_DOMAIN}:
            c_reason[status == name] = REASONS[name]
        c_reason[in_domain & np.isfinite(c_signed) & np.isfinite(c_normal)] = REASONS["valid"]
        c_reason[in_domain & np.isfinite(c_signed) & ~np.isfinite(c_normal)] = REASONS["thin_cross_section"]
        c_reason[in_domain & ~np.isfinite(c_signed)] = REASONS["nonfinite_value"]
        signed[positions], z[positions], rank_u[positions], reason[positions] = c_signed, c_z, c_rank, c_reason
    others = np.flatnonzero(~candidate)
    if len(others):
        reason[others] = work["status"].iloc[others].map(REASONS).to_numpy(dtype=np.int16)
    if (reason < 0).any():
        raise FeatureStoreParquetError("internal: unassigned reason")
    offsets = _clock_offsets(work["eom"].to_numpy(), work["available_at"].to_numpy())
    if np.nanmax(np.abs(offsets), initial=0.0) > 2**31 - 1:
        raise FeatureStoreParquetError("clock offset beyond INT32 seconds")
    return pa.table({
        "eom": pa.array(work["eom"].dt.date.to_numpy(), type=pa.date32()),
        "line_id": pa.array(work["line_id"].to_numpy(dtype=object), type=pa.string()),
        "owner_id": pa.array(work["owner_id"].where(work["owner_id"].notna(), None).to_numpy(dtype=object),
                             type=pa.string()),
        "raw": pa.array(work["raw"].to_numpy(), type=pa.float64(), from_pandas=True),
        "signed": pa.array(signed, type=pa.float64(), from_pandas=True),
        "rank_u": pa.array(rank_u, type=pa.float64(), from_pandas=True),
        "z": pa.array(z, type=pa.float64(), from_pandas=True),
        "reason": pa.array(reason.astype(np.int8), type=pa.int8()),
        "clock_offset_s": pa.array(offsets, type=pa.float64(), from_pandas=True).cast(pa.int32()),
    }, schema=FEATURE_SCHEMA)


def build_feature(store: FeatureStore, basis: str, feature_id: str, feature_sha: str,
                  chunks: Iterable[pd.DataFrame], *, expected_sign: int, log_base: bool = False,
                  policy: StandardizationPolicy | None = None, meta: Mapping[str, object] | None = None) -> Path:
    """Standardize each chunk of whole formations and stream it into the store (the add-one-feature path)."""
    if store.has(basis, feature_id, feature_sha):
        return store.path(basis, feature_id, feature_sha)
    batches = (standardize_to_store(chunk, expected_sign=expected_sign, log_base=log_base, policy=policy,
                                    feature_id=feature_id) for chunk in chunks)
    return store.write(basis, feature_id, feature_sha, batches,
                       {**dict(meta or {}), "expected_sign": expected_sign, "log_base": log_base})


# ---------------------------------------------------------------------------
# R3b input adapter (ruling C-44)
# ---------------------------------------------------------------------------

@dataclass
class StoreFeatureTable:
    """What :func:`load_feature_table_from_store` hands the R3b engine for one basis.

    ``variants_by_feature`` and ``load_feature`` plug into ``evaluation.BasisInputs``;
    ``load_controls(names, variant)`` gives its ``controls`` frame; ``digests`` records the
    files read (sha256 per feature) and rows outside the calendar's formed months.
    """

    basis: str
    entries: tuple[dict[str, str], ...]
    variants_by_feature: dict[str, tuple[str, ...]]
    expected_signs: dict[str, int]
    load_feature: Callable[[str], FeatureData]
    load_controls: Callable[[Sequence[str], str], pd.DataFrame]
    digests: dict[str, Any] = field(default_factory=dict)


def _month_ends(month_start: Any) -> np.ndarray:
    import pandas as pd

    starts = pd.to_datetime(pd.Series(month_start)).dt.normalize()
    return (starts + pd.offsets.MonthEnd(0)).dt.date.to_numpy()


def load_feature_table_from_store(store: FeatureStore, entries: Sequence[Mapping[str, str]], *,
                                  calendar: pd.DataFrame, securities: Mapping[str, int],
                                  variants: Sequence[str] = STORE_VARIANTS,
                                  catalog: Mapping[str, CatalogFeature] | None = None) -> StoreFeatureTable:
    """R3b adapter over stored feature files (the Parquet counterpart of ``evaluation.load_feature_table``).

    ``entries``: manifest rows ``{basis, feature_id, feature_sha}`` of one basis.
    ``calendar``: the R3b ``BasisInputs.calendar`` (``month_index``, ``month_start``,
    ``status``); a month's ``eom`` is its calendar month end. ``securities``: line_id -> the
    integer code the R3b labels/context use. ``variants``: R3b variants to serve, from
    :data:`ADAPTER_VARIANTS` (``signed_raw`` <- signed, ``zscore`` <- z, ``rank_normal`` <-
    Phi^-1(rank_u) with R2b's own inverse normal, ``rank_u`` as stored).

    Values are the rows with a value slot (reason < 64, R2b's matrix rows) at *formed*
    months of the calendar; rows at other months inside the calendar range are left out and
    counted; a value row on a line without a code is a contract violation (raises).
    ``available_at`` = ``eom`` 22:00 UTC + ``clock_offset_s`` (R3b refuses any value after its
    formation cutoff). ``unlinked_line`` = ``owner_id IS NULL``. Date rows exist for every
    formed month and variant: ``thin_cross_section`` where the month has a thin row or no
    valid row, ``degenerate_cross_section`` where the variant has no value, else ``formed``;
    ``coverage_fraction`` = values / all rows of the month (the file's own universe).
    """
    import pandas as pd

    from .evaluation import EvaluationInputError, FeatureData
    from .features import _inverse_normal

    rows = _manifest_entries(entries)
    if not rows:
        raise FeatureStoreParquetError("no features to load")
    bases = {r["basis"] for r in rows}
    if len(bases) != 1:
        raise FeatureStoreParquetError(f"one basis per table, got {sorted(bases)}")
    basis = bases.pop()
    served = tuple(sorted(set(variants)))
    if not served or set(served) - set(ADAPTER_VARIANTS):
        raise FeatureStoreParquetError(f"variants must come from {ADAPTER_VARIANTS}")
    metas = {r["feature_id"]: store.read_meta(basis, r["feature_id"], r["feature_sha"]) for r in rows}
    signs = {fid: int(meta["expected_sign"]) for fid, meta in metas.items()}
    if catalog is not None:
        for fid, sign in signs.items():
            if fid in catalog and catalog[fid].expected_sign != sign:
                raise FeatureStoreParquetError(f"{fid}: stored expected_sign {sign} != catalog "
                                               f"{catalog[fid].expected_sign}")
    cal = calendar.sort_values("month_index", kind="stable").reset_index(drop=True)
    eoms = _month_ends(cal["month_start"])
    formed = (cal["status"] == "formed").to_numpy(dtype=bool)
    lo, hi = eoms.min(), eoms.max()
    con = store.con
    con.execute("CREATE OR REPLACE TEMP TABLE _fs_months (eom DATE, month_index BIGINT, formed BOOLEAN)")
    con.executemany("INSERT INTO _fs_months VALUES (?, ?, ?)",
                    [(e, int(i), bool(f)) for e, i, f in zip(eoms, cal["month_index"], formed, strict=True)])
    con.execute("CREATE OR REPLACE TEMP TABLE _fs_securities (line_id VARCHAR, code BIGINT)")
    security_frame = pd.DataFrame({"line_id": pd.Series(list(securities.keys()), dtype=object),
                                   "code": np.fromiter(securities.values(), dtype=np.int64, count=len(securities))})
    con.register("_fs_security_stage", security_frame)
    try:
        con.execute("INSERT INTO _fs_securities SELECT CAST(line_id AS VARCHAR), CAST(code AS BIGINT) "
                    "FROM _fs_security_stage")
    finally:
        con.unregister("_fs_security_stage")
    formed_index = cal.loc[formed, "month_index"].to_numpy(dtype=np.int64)
    by_id = {r["feature_id"]: r for r in rows}
    digests: dict[str, Any] = {"store_version": FEATURE_STORE_VERSION, "basis": basis,
                               "files": {fid: metas[fid]["file_sha256"] for fid in sorted(metas)},
                               "outside_formed_months": {}}
    thin_code, valid_code = REASONS["thin_cross_section"], REASONS["valid"]

    def _path(feature_id: str) -> str:
        if feature_id not in by_id:
            raise EvaluationInputError(f"{feature_id} is not in the store table")
        entry = by_id[feature_id]
        return sql_text(store.path(basis, feature_id, entry["feature_sha"]).as_posix())

    def _arrays(feature_id: str) -> dict[str, np.ndarray]:
        source = _path(feature_id)
        outside = con.execute(f"""
            SELECT count(*) FROM read_parquet({source}) r LEFT JOIN _fs_months m ON m.eom = r.eom
            WHERE r.eom BETWEEN ? AND ? AND r.reason < {VALUE_SLOT_LIMIT} AND NOT coalesce(m.formed, false)
        """, [lo, hi]).fetchone()[0]
        digests["outside_formed_months"][feature_id] = int(outside)
        unmapped = con.execute(f"""
            SELECT count(*) FROM read_parquet({source}) r JOIN _fs_months m ON m.eom = r.eom AND m.formed
            LEFT JOIN _fs_securities s ON s.line_id = r.line_id
            WHERE r.reason < {VALUE_SLOT_LIMIT} AND s.code IS NULL
        """).fetchone()[0]
        if unmapped:
            raise EvaluationInputError(f"{feature_id}: {unmapped} value rows on lines outside the supplied "
                                       "securities (the ranked context)")
        return con.execute(f"""
            SELECT m.month_index, s.code AS security, r.reason, r.signed, r.z, r.rank_u,
                   CASE WHEN r.clock_offset_s IS NULL THEN -1
                        ELSE epoch_us(CAST(r.eom AS TIMESTAMP) + INTERVAL {DECISION_HOUR} HOUR)
                             + CAST(r.clock_offset_s AS BIGINT) * 1000000 END AS available_at_us,
                   r.owner_id IS NULL AS unlinked_line
            FROM read_parquet({source}) r
            JOIN _fs_months m ON m.eom = r.eom AND m.formed
            JOIN _fs_securities s ON s.line_id = r.line_id
            WHERE r.reason < {VALUE_SLOT_LIMIT}
            ORDER BY m.month_index, s.code
        """).fetchnumpy()

    def _variant_values(arrays: Mapping[str, np.ndarray], variant: str) -> np.ndarray:
        def column(name: str) -> np.ndarray:
            data = arrays[name]
            if np.ma.isMaskedArray(data):
                return np.ma.filled(data.astype(float), np.nan)
            return np.asarray(data, dtype=float)

        if variant == "signed_raw":
            return column("signed")
        if variant == "zscore":
            return column("z")
        rank = column("rank_u")
        if variant == "rank_u":
            return rank
        out = np.full(len(rank), np.nan)
        ok = np.isfinite(rank)
        if ok.any():
            out[ok] = _inverse_normal(rank[ok])
        return out

    def _dates(feature_id: str, variant_values: Mapping[str, np.ndarray],
               month: np.ndarray, reason: np.ndarray) -> pd.DataFrame:
        source = _path(feature_id)
        universe = dict(con.execute(f"""
            SELECT m.month_index, count(*) FROM read_parquet({source}) r
            JOIN _fs_months m ON m.eom = r.eom AND m.formed GROUP BY 1
        """).fetchall())
        span = int(cal["month_index"].max()) + 1
        thin_rows = np.bincount(month[reason == thin_code], minlength=span)
        valid_rows = np.bincount(month[reason == valid_code], minlength=span)
        records = []
        for variant in served:
            counts = np.bincount(month[np.isfinite(variant_values[variant])], minlength=span)
            for index in formed_index:
                names = int(universe.get(int(index), 0))
                n = int(counts[index])
                if thin_rows[index] > 0 or valid_rows[index] == 0:
                    status = "thin_cross_section"
                elif n == 0:
                    status = "degenerate_cross_section"
                else:
                    status = "formed"
                records.append((int(index), variant, status, (n / names) if names else np.nan))
        return pd.DataFrame(records, columns=["month_index", "variant", "date_status", "coverage_fraction"])

    def load_feature(feature_id: str) -> FeatureData:
        arrays = _arrays(feature_id)
        month = np.asarray(arrays["month_index"], dtype=np.int64)
        security = np.asarray(arrays["security"], dtype=np.int64)
        reason = np.asarray(arrays["reason"], dtype=np.int16)
        available = np.asarray(arrays["available_at_us"], dtype=np.int64)
        stamps = available.astype("datetime64[us]")
        stamps[available < 0] = np.datetime64("NaT", "us")
        unlinked = np.asarray(arrays["unlinked_line"], dtype=bool)
        values = {variant: _variant_values(arrays, variant) for variant in served}
        n = len(month)
        frame = pd.DataFrame({
            "month_index": np.tile(month, len(served)), "security": np.tile(security, len(served)),
            "variant": pd.Categorical.from_codes(np.repeat(np.arange(len(served)), n), categories=list(served)),
            "value": np.concatenate([values[v] for v in served]) if n else np.zeros(0),
            "available_at": np.tile(stamps, len(served)), "unlinked_line": np.tile(unlinked, len(served))})
        entry = None if catalog is None else catalog.get(feature_id)
        return FeatureData(feature_id, signs[feature_id], frame, _dates(feature_id, values, month, reason),
                           None if entry is None else entry.anomaly_class,
                           None if entry is None else entry.hypothesis_family)

    def load_controls(names: Sequence[str], variant: str) -> pd.DataFrame:
        if variant not in ADAPTER_VARIANTS:
            raise FeatureStoreParquetError(f"control variant {variant!r} is not served")
        frames = []
        for name in names:
            if name not in by_id:
                continue
            arrays = _arrays(name)
            frames.append(pd.DataFrame({"month_index": np.asarray(arrays["month_index"], dtype=np.int64),
                                        "security": np.asarray(arrays["security"], dtype=np.int64),
                                        "control": name, "value": _variant_values(arrays, variant)}))
        if not frames:
            return pd.DataFrame(columns=["month_index", "security", "control", "value"])
        return pd.concat(frames, ignore_index=True)

    return StoreFeatureTable(basis, tuple(rows), {fid: served for fid in sorted(by_id)}, signs, load_feature,
                             load_controls, digests)


__all__ = [
    "ADAPTER_VARIANTS",
    "DEFAULT_ROOT",
    "FEATURE_SCHEMA",
    "FEATURE_STORE_VERSION",
    "REASONS",
    "REASON_NAMES",
    "STORE_VARIANTS",
    "VALUE_SLOT_LIMIT",
    "FeatureStore",
    "FeatureStoreParquetError",
    "StoreFeatureTable",
    "build_feature",
    "catalog_row_payload",
    "compute_feature_sha",
    "load_feature_table_from_store",
    "read_manifest",
    "standardize_to_store",
    "store_code_digest",
    "write_manifest",
]
