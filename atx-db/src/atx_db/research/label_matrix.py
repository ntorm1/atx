"""Label matrix (L3): forward-return labels per (month end, line, horizon) as immutable Parquet.

Layout (root ``C:/atx/atx-db/data/research`` by default)::

    labels/<label_sha>/_label.json                 the label spec (provisional flag, holdout start, ...)
    labels/<label_sha>/_windows.parquet            one row per (eom, h): formation, entry and exit sessions
    labels/<label_sha>/h=<K>/year=YYYY.parquet     rows of formation year YYYY at horizon K months
    labels/<label_sha>/h=<K>/year=YYYY.json        its sidecar (rows, reasons, file sha256)

Row schema (:data:`LABEL_SCHEMA`), sorted by ``(eom, line_id)``::

    eom DATE, line_id VARCHAR, owner_id VARCHAR, entry_date DATE, exit_date DATE,
    ret DOUBLE, ret_exc DOUBLE, basis VARCHAR, reason INT8

``entry_date`` is the first session after the formation session (the close the position is
entered at; always after ``eom``), ``exit_date`` the window's last session, ``ret`` the total
return entry close -> exit close, ``ret_exc`` the same over the risk-free rate (NULL where
none), ``basis`` the price basis, ``reason`` :data:`LABEL_REASONS` (a line that stops trading
inside its window is ``terminal_pending`` with ``ret`` NULL until a terminal return exists:
counted, never imputed).

``label_sha`` = sha256 of canonical JSON of the label spec (:func:`compute_label_sha`). The spec
must carry (1.9 fix round 1):

* ``provisional`` (bool): true for the price wave's labels (node 1.12), false for final labels
  (node 3.8);
* ``holdout_start``: the ISO date from which formations are sealed (policy v4: ``2024-01-01``),
  or an explicit ``None`` together with a ``holdout_basis`` reason (e.g. a fixture) -- the key is
  mandatory, so labels are never unsealed by omission;
* ``code_digest`` (the builder's code) and ``input_digests`` (the lake datasets / files read), so a
  builder fix or an input change is a new label set.

Holdout (ruling R-6): every read names ``eom_before``; a read past ``holdout_start`` is refused
unless ``allow_holdout=True`` **and** the labels are final **and** the trial registry holds the
wave's ``open_holdout`` record for this ``label_sha`` (``holdout_wave=``). Provisional labels
never open the holdout. :meth:`LabelMatrix.r3b_inputs` records the opening that allowed a
holdout read in ``info['holdout_opening']`` (registry file, whether it is the default
registry, the record's sequence, ``record_sha`` and ``opened_at``); ``registry_root=`` is for
tests (the default registry is the policy one).

The builder (node 1.12's provisional labels, node 3.8's final labels): :meth:`LabelMatrix.create`
(spec), one :meth:`LabelMatrix.write` per (h, year), :meth:`LabelMatrix.write_windows`, then
:meth:`LabelMatrix.complete` with the expected horizons and formation years. Reads
(:meth:`LabelMatrix.scan`, :meth:`LabelMatrix.r3b_inputs`) refuse a set that is not complete, read
exactly the recorded files after checking each one's size and sha256 against ``_complete.json``
(the sha256 once per process per file state), and :meth:`LabelMatrix.r3b_inputs` turns them into
the R3b engine's ``labels``/``maturity`` frames. A completed set is sealed (no further writes).
"""

from __future__ import annotations

import datetime as dt
import itertools
import json
import os
import re
import time
from collections.abc import Iterable, Mapping, Sequence
from pathlib import Path
from typing import TYPE_CHECKING, Any

import duckdb
import numpy as np
import pyarrow as pa
import pyarrow.parquet as pq

from .research_lake import canonical_json, connect_bounded, sha256_file, sha256_text, sql_text

if TYPE_CHECKING:
    import pandas as pd

LABEL_STORE_VERSION = "research-label-parquet-v1"
DEFAULT_ROOT = Path("C:/atx/atx-db/data/research")

LABEL_SCHEMA = pa.schema([
    pa.field("eom", pa.date32(), nullable=False),
    pa.field("line_id", pa.string(), nullable=False),
    pa.field("owner_id", pa.string()),
    pa.field("entry_date", pa.date32()),
    pa.field("exit_date", pa.date32()),
    pa.field("ret", pa.float64()),
    pa.field("ret_exc", pa.float64()),
    pa.field("basis", pa.string()),
    pa.field("reason", pa.int8(), nullable=False),
])
WINDOW_SCHEMA = pa.schema([
    pa.field("eom", pa.date32(), nullable=False),
    pa.field("h", pa.int16(), nullable=False),
    pa.field("formation_date", pa.date32(), nullable=False),
    pa.field("entry_date", pa.date32()),
    pa.field("exit_date", pa.date32()),
])

#: Append-only. 0-2 are valid returns (R3b status 0 with terminal 0/1/2), 3 invalid (R3b 1),
#: 4 unsupported basis (R3b 2); 5+ carry no usable return (R3b sees no label: missing).
LABEL_REASONS: Mapping[str, int] = {
    "valid": 0,
    "valid_terminal_observed": 1,     # the window stitches an observed delisting return
    "valid_terminal_policy": 2,       # the window stitches a policy terminal return
    "invalid": 3,
    "unsupported_basis": 4,
    "terminal_pending": 5,            # stopped trading inside the window, no terminal return yet
    "not_matured": 6,
    "missing_entry_bar": 7,
    "missing_exit_bar": 8,
}
LABEL_REASON_NAMES = {code: name for name, code in LABEL_REASONS.items()}
_R3B_STATUS = {0: (0, 0), 1: (0, 1), 2: (0, 2), 3: (1, 0), 4: (2, 0)}
MAX_HORIZON = 60
_SHA = re.compile(r"^[0-9a-f]{64}$")


class LabelMatrixError(ValueError):
    """The label matrix cannot write or read labels under its contract."""


class LabelHoldoutError(LabelMatrixError):
    """A read would open sealed holdout formations."""


_TEMP = itertools.count()
#: Files whose sha256 matched their ``_complete.json`` record in this process: (path, size, mtime_ns) -> sha.
_VERIFIED: dict[tuple[str, int, int], str] = {}


def _verify_file(path: Path, expected: str, what: str) -> None:
    """A completed set's file against its recorded sha256 (hashed once per process per file state)."""
    stat = path.stat()
    key = (str(path), stat.st_size, stat.st_mtime_ns)
    if _VERIFIED.get(key) == expected:
        return
    actual = sha256_file(path)
    if actual != expected:
        raise LabelMatrixError(f"{what}: sha256 {actual[:12]} differs from the {expected[:12]} recorded by complete() "
                               "(the file changed since the set was sealed)")
    _VERIFIED[key] = expected


def _check_spec(spec: Mapping[str, object]) -> None:
    if not isinstance(spec.get("provisional"), bool):
        raise LabelMatrixError("a label spec says provisional=true|false")
    if "holdout_start" not in spec:
        raise LabelMatrixError("a label spec names holdout_start (an ISO date, e.g. policy v4's 2024-01-01, or None "
                               "with a holdout_basis reason): labels are never unsealed by omission (R-6)")
    holdout = spec["holdout_start"]
    if holdout is None:
        basis = spec.get("holdout_basis")
        if not isinstance(basis, str) or not basis.strip():
            raise LabelMatrixError("holdout_start=None needs a holdout_basis reason")
    else:
        try:
            dt.date.fromisoformat(str(holdout))
        except ValueError as error:
            raise LabelMatrixError(f"holdout_start {holdout!r} is not an ISO date") from error
    code = spec.get("code_digest")
    if not isinstance(code, str) or not code.strip():
        raise LabelMatrixError("a label spec carries code_digest (the builder's code): a builder fix is a new set")
    inputs = spec.get("input_digests")
    if not isinstance(inputs, Mapping) or not inputs or not all(isinstance(v, str) and v for v in inputs.values()):
        raise LabelMatrixError("a label spec carries input_digests {name: digest} of every input read")


def compute_label_sha(spec: Mapping[str, object]) -> str:
    """sha256 of canonical JSON of the label spec (see the module docstring for the required keys)."""
    _check_spec(spec)
    return sha256_text(canonical_json({"store_version": LABEL_STORE_VERSION, "spec": dict(spec)}))


def _check_sha(value: str) -> None:
    if not isinstance(value, str) or not _SHA.fullmatch(value):
        raise LabelMatrixError(f"label_sha {value!r} must be a sha256 hex digest")


def _check_horizon(h: int) -> int:
    if isinstance(h, bool) or not isinstance(h, (int, np.integer)) or not 1 <= int(h) <= MAX_HORIZON:
        raise LabelMatrixError(f"horizon must be an integer 1..{MAX_HORIZON} months")
    return int(h)


def _days(column: pa.Array | pa.ChunkedArray, fill: int | None = None) -> np.ndarray:
    """Days since 1970-01-01 (``fill`` replaces NULLs; without it NULL-free input is expected)."""
    days = column.cast(pa.int32())
    if fill is not None:
        days = days.fill_null(fill)
    return days.to_numpy(zero_copy_only=False)


class LabelMatrix:
    """``labels/<label_sha>/h=<K>/year=YYYY.parquet`` files, their windows and spec; read via DuckDB."""

    def __init__(self, root: Path = Path("C:/atx/atx-db/data/research"), *, memory_limit: str = "256MB",
                 threads: int = 1) -> None:
        self.root = Path(root)
        self.memory_limit = memory_limit
        self.threads = threads
        self._con: duckdb.DuckDBPyConnection | None = None

    # -- paths and spec --------------------------------------------------------
    def directory(self, label_sha: str) -> Path:
        _check_sha(label_sha)
        return self.root / "labels" / label_sha

    def path(self, label_sha: str, h: int, year: int) -> Path:
        return self.directory(label_sha) / f"h={_check_horizon(h)}" / f"year={int(year):04d}.parquet"

    def has(self, label_sha: str, h: int, year: int) -> bool:
        path = self.path(label_sha, h, year)
        return path.is_file() and path.with_suffix(".json").is_file()

    def create(self, label_sha: str, spec: Mapping[str, object]) -> Path:
        """Record the spec (``_label.json``) once; the sha must be the spec's."""
        if compute_label_sha(spec) != label_sha:
            raise LabelMatrixError("label_sha is not the spec's sha (compute_label_sha)")
        target = self.directory(label_sha) / "_label.json"
        payload = {"store_version": LABEL_STORE_VERSION, "label_sha": label_sha,
                   "spec": json.loads(canonical_json(dict(spec)))}
        if target.is_file():
            if json.loads(target.read_text(encoding="utf-8"))["spec"] != payload["spec"]:
                raise LabelMatrixError(f"labels/{label_sha}/_label.json holds another spec")
            return target
        target.parent.mkdir(parents=True, exist_ok=True)
        _write_atomic_text(target, json.dumps(payload, indent=1, sort_keys=True) + "\n")
        return target

    def spec(self, label_sha: str) -> dict[str, Any]:
        target = self.directory(label_sha) / "_label.json"
        if not target.is_file():
            raise LabelMatrixError(f"label set {label_sha} was not created")
        payload = json.loads(target.read_text(encoding="utf-8"))
        if payload.get("store_version") != LABEL_STORE_VERSION:
            raise LabelMatrixError(f"label set {label_sha}: unsupported store version")
        spec = dict(payload["spec"])
        _check_spec(spec)
        if compute_label_sha(spec) != label_sha:
            raise LabelMatrixError(f"labels/{label_sha}/_label.json does not hash to its label_sha")
        return spec

    def _refuse_if_complete(self, label_sha: str) -> None:
        if (self.directory(label_sha) / "_complete.json").is_file():
            raise LabelMatrixError(f"label set {label_sha[:12]} is complete (sealed); a change is a new label set")

    # -- write ---------------------------------------------------------------
    def write(self, label_sha: str, h: int, year: int, batches: Iterable[pa.RecordBatch | pa.Table],
              meta: Mapping[str, object] | None = None) -> Path:
        """Write one (horizon, formation year) file (temp + rename); an existing file is kept."""
        self.spec(label_sha)
        target = self.path(label_sha, h, year)
        if self.has(label_sha, h, year):
            return target
        self._refuse_if_complete(label_sha)
        target.parent.mkdir(parents=True, exist_ok=True)
        stamp = f"{os.getpid()}.{time.monotonic_ns()}"
        tmp = target.with_name(f".{target.stem}.{stamp}.tmp")
        rows, reasons, last = 0, {}, None
        low, high = (dt.date(int(year), 1, 1) - dt.date(1970, 1, 1)).days, \
            (dt.date(int(year), 12, 31) - dt.date(1970, 1, 1)).days
        try:
            with pq.ParquetWriter(tmp, LABEL_SCHEMA, compression="zstd", write_statistics=True) as writer:
                for item in batches:
                    for batch in (item.to_batches() if isinstance(item, pa.Table) else [item]):
                        if batch.schema.names != LABEL_SCHEMA.names:
                            raise LabelMatrixError(f"batch columns {batch.schema.names} != {LABEL_SCHEMA.names}")
                        batch = batch.cast(LABEL_SCHEMA)
                        last = _check_label_batch(batch, low, high, last)
                        for code, count in zip(*np.unique(batch.column("reason").to_numpy(zero_copy_only=False),
                                                          return_counts=True), strict=True):
                            reasons[int(code)] = reasons.get(int(code), 0) + int(count)
                        rows += batch.num_rows
                        writer.write_batch(batch)
            sidecar = {"store_version": LABEL_STORE_VERSION, "label_sha": label_sha, "h": int(h), "year": int(year),
                       "rows": rows, "reasons": {LABEL_REASON_NAMES[c]: n for c, n in sorted(reasons.items())},
                       "bytes": tmp.stat().st_size, "file_sha256": sha256_file(tmp),
                       "meta": json.loads(canonical_json(dict(meta or {})))}
            meta_tmp = target.with_name(f".{target.stem}.{stamp}.json.tmp")
            meta_tmp.write_text(json.dumps(sidecar, indent=1, sort_keys=True) + "\n", encoding="utf-8")
            os.replace(tmp, target)
            os.replace(meta_tmp, target.with_suffix(".json"))
        except BaseException:
            for leftover in (tmp, target.with_name(f".{target.stem}.{stamp}.json.tmp")):
                if leftover.exists():
                    leftover.unlink()
            raise
        return target

    def write_windows(self, label_sha: str, table: pa.Table) -> Path:
        """The (eom, h) windows of the label set: formation session, entry session, exit session."""
        self.spec(label_sha)
        target = self.directory(label_sha) / "_windows.parquet"
        if table.schema.names != WINDOW_SCHEMA.names:
            raise LabelMatrixError(f"window columns {table.schema.names} != {WINDOW_SCHEMA.names}")
        table = table.cast(WINDOW_SCHEMA)
        keys = list(zip(_days(table.column("eom")).tolist(), table.column("h").to_pylist(), strict=True))
        if len(set(keys)) != len(keys):
            raise LabelMatrixError("one window per (eom, h)")
        entry, formation = _days(table.column("entry_date"), 0), _days(table.column("formation_date"))
        eom = _days(table.column("eom"))
        has_entry = ~np.asarray(table.column("entry_date").is_null().to_numpy(zero_copy_only=False), dtype=bool)
        if (formation > eom).any() or (has_entry & (entry <= eom)).any():
            raise LabelMatrixError("a window's formation session is after its month end or its entry is not after it")
        if target.is_file():
            existing = pq.read_table(target)
            if not existing.equals(table.sort_by([("eom", "ascending"), ("h", "ascending")])):
                raise LabelMatrixError(f"labels/{label_sha}/_windows.parquet holds other windows")
            return target
        self._refuse_if_complete(label_sha)
        tmp = target.with_name(f"._windows.{os.getpid()}.{time.monotonic_ns()}.tmp")
        pq.write_table(table.sort_by([("eom", "ascending"), ("h", "ascending")]), tmp, compression="zstd")
        os.replace(tmp, target)
        return target

    def complete(self, label_sha: str, horizons: Sequence[int], years: Sequence[int]) -> Path:
        """Seal the set: every (h, year) of the expected grid and the windows must exist.

        Writes ``_complete.json`` (the grid and each file's bytes and sha256); reads refuse a
        set without it and read exactly its files. Idempotent for the same grid and files.
        """
        self.spec(label_sha)
        grid_h = sorted({_check_horizon(h) for h in horizons})
        grid_y = sorted({int(y) for y in years})
        if not grid_h or not grid_y:
            raise LabelMatrixError("complete() needs the expected horizons and formation years")
        windows = self.directory(label_sha) / "_windows.parquet"
        missing = [f"h={h}/year={y:04d}" for h in grid_h for y in grid_y if not self.has(label_sha, h, y)]
        if not windows.is_file():
            missing.append("_windows.parquet")
        if missing:
            raise LabelMatrixError(f"label set {label_sha[:12]} is incomplete: missing {missing}")
        files = {}
        for h in grid_h:
            for y in grid_y:
                sidecar = json.loads(self.path(label_sha, h, y).with_suffix(".json").read_text(encoding="utf-8"))
                files[f"h={h}/year={y:04d}.parquet"] = {"bytes": sidecar["bytes"], "sha256": sidecar["file_sha256"],
                                                        "rows": sidecar["rows"]}
        payload = {"store_version": LABEL_STORE_VERSION, "label_sha": label_sha, "horizons": grid_h,
                   "years": grid_y, "files": files, "windows_sha256": sha256_file(windows)}
        target = self.directory(label_sha) / "_complete.json"
        if target.is_file():
            if json.loads(target.read_text(encoding="utf-8")) != payload:
                raise LabelMatrixError(f"label set {label_sha[:12]} was completed with another grid or files")
            return target
        _write_atomic_text(target, json.dumps(payload, indent=1, sort_keys=True) + "\n")
        return target

    def _complete_record(self, label_sha: str) -> dict[str, Any]:
        target = self.directory(label_sha) / "_complete.json"
        if not target.is_file():
            raise LabelMatrixError(f"label set {label_sha[:12]} is not complete (LabelMatrix.complete): "
                                   "an incomplete set is never read")
        return json.loads(target.read_text(encoding="utf-8"))

    def _complete_files(self, label_sha: str, horizons: Sequence[int],
                        years: Sequence[int] | None = None) -> list[str]:
        """The completed set's files at ``horizons`` (optionally some years), checked on disk (bytes, sha256)."""
        record = self._complete_record(label_sha)
        wanted = [_check_horizon(h) for h in horizons]
        outside = sorted(set(wanted) - set(record["horizons"]))
        if outside:
            raise LabelMatrixError(f"horizons {outside} are not in the completed set {record['horizons']}")
        paths = []
        for h in wanted:
            for y in record["years"]:
                if years is not None and y not in years:
                    continue
                name = f"h={h}/year={y:04d}.parquet"
                path = self.directory(label_sha) / name
                if not path.is_file() or path.stat().st_size != record["files"][name]["bytes"]:
                    raise LabelMatrixError(f"label set {label_sha[:12]}: {name} is missing or changed since complete()")
                _verify_file(path, record["files"][name]["sha256"], f"label set {label_sha[:12]}: {name}")
                paths.append(path.as_posix())
        return paths

    # -- read ----------------------------------------------------------------
    @property
    def con(self) -> duckdb.DuckDBPyConnection:
        if self._con is None:
            self._con = connect_bounded(None, root=self.root, memory_limit=self.memory_limit, threads=self.threads)
        return self._con

    def close(self) -> None:
        if self._con is not None:
            self._con.close()
            self._con = None

    def __enter__(self) -> LabelMatrix:
        return self

    def __exit__(self, *exc: object) -> None:
        self.close()

    def _guard(self, label_sha: str, eom_before: dt.date, allow_holdout: bool, holdout_wave: str | None,
               registry_root: Path | str | None) -> dict[str, Any] | None:
        """R-6: a read past ``holdout_start`` needs final labels and the wave's recorded opening.

        Returns the opening that allowed a holdout read (None when the read stays sealed).
        """
        if isinstance(eom_before, dt.datetime) or not isinstance(eom_before, dt.date):
            raise LabelMatrixError("eom_before must be a datetime.date")
        spec = self.spec(label_sha)
        holdout = spec["holdout_start"]
        if holdout is None or eom_before <= dt.date.fromisoformat(str(holdout)):
            return None
        where = f"labels {label_sha[:12]}: reading formations up to {eom_before} opens the holdout sealed from {holdout}"
        if not allow_holdout:
            raise LabelHoldoutError(f"{where} (ruling R-6: pass allow_holdout only for the wave's one opening)")
        if spec["provisional"]:
            raise LabelHoldoutError(f"{where}: provisional labels never open the holdout (R-6: final labels only)")
        if not holdout_wave:
            raise LabelHoldoutError(f"{where}: name the wave (holdout_wave=) whose trial-registry opening allows it")
        from .trial_registry import TrialRegistry

        registry = TrialRegistry(registry_root)
        opening = registry.holdout_opening(holdout_wave)
        if opening is None or not opening.get("final_labels") or opening.get("label_sha") != label_sha:
            raise LabelHoldoutError(f"{where}: the trial registry has no open_holdout record of wave "
                                    f"{holdout_wave!r} for this label_sha (found {opening and opening.get('label_sha')})")
        return {"registry": registry.path.as_posix(), "registry_default": registry_root is None,
                "wave": holdout_wave, "sequence": opening.get("sequence"), "record_sha": opening.get("record_sha"),
                "opened_at": opening.get("opened_at"), "label_sha": label_sha}

    def scan(self, label_sha: str, horizons: Sequence[int], *, eom_before: dt.date,
             years: Sequence[int] | None = None, allow_holdout: bool = False, holdout_wave: str | None = None,
             registry_root: Path | str | None = None) -> duckdb.DuckDBPyRelation:
        """Rows with ``eom < eom_before`` at the horizons (column ``h`` from the path) of a complete set."""
        self._guard(label_sha, eom_before, allow_holdout, holdout_wave, registry_root)
        files = self._complete_files(label_sha, horizons, years)
        if not files:
            raise LabelMatrixError(f"labels {label_sha[:12]}: no files for horizons {list(horizons)}")
        literal = "[" + ", ".join(sql_text(p) for p in files) + "]"
        return self.con.sql(f"SELECT * FROM read_parquet({literal}, hive_partitioning=true, "
                            f"hive_types={{'h': INTEGER}}) WHERE eom < DATE '{eom_before.isoformat()}'")

    def r3b_inputs(self, label_sha: str, horizons: Sequence[int], *, calendar: pd.DataFrame,
                   securities: Mapping[str, int], label_cutoff: dt.datetime, eom_before: dt.date | None = None,
                   allow_holdout: bool = False, holdout_wave: str | None = None,
                   registry_root: Path | str | None = None) -> tuple[pd.DataFrame, pd.DataFrame, dict[str, Any]]:
        """R3b ``labels`` and ``maturity`` frames from the label set (the ``load_label_inputs`` counterpart).

        A window is matured when its formation and entry sessions are the calendar's, its
        formation is before ``eom_before`` (default: the spec's ``holdout_start``) and its
        exit + 1 day 12:00 is <= ``label_cutoff`` (the R3a rule). Labels are the rows at
        matured windows on lines in ``securities``: reasons 0-2 -> status 0 (terminal 0/1/2),
        3 -> 1, 4 -> 2; the others carry no return and are absent (R3b counts them missing).
        ``anchor_date`` is the row's entry date. The set must be complete; the holdout rule
        is :meth:`_guard`'s.
        """
        import pandas as pd

        spec = self.spec(label_sha)
        cal = calendar.sort_values("month_index", kind="stable").reset_index(drop=True)
        eoms = (pd.to_datetime(cal["month_start"]).dt.normalize() + pd.offsets.MonthEnd(0)).dt.date
        if eom_before is None:
            holdout = spec["holdout_start"]
            eom_before = dt.date.fromisoformat(str(holdout)) if holdout is not None else \
                max(eoms) + dt.timedelta(days=1)
        opening = self._guard(label_sha, eom_before, allow_holdout, holdout_wave, registry_root)
        files = self._complete_files(label_sha, horizons)
        record = self._complete_record(label_sha)
        tag = f"_lm{next(_TEMP)}"
        windows_path = self.directory(label_sha) / "_windows.parquet"
        if not windows_path.is_file():
            raise LabelMatrixError(f"labels {label_sha[:12]} have no _windows.parquet")
        _verify_file(windows_path, record["windows_sha256"], f"label set {label_sha[:12]}: _windows.parquet")
        cutoff = label_cutoff.replace(tzinfo=None) if label_cutoff.tzinfo is None else \
            label_cutoff.astimezone(dt.UTC).replace(tzinfo=None)
        con = self.con
        # datetime64 columns (NaT -> NULL) cast to DATE in SQL.
        months = pd.DataFrame({"eom": pd.to_datetime(pd.Series(list(eoms))).to_numpy(),
                               "month_index": cal["month_index"].to_numpy(np.int64),
                               "formed": (cal["status"] == "formed").to_numpy(bool),
                               "formation_date": pd.to_datetime(cal["formation_date"]).to_numpy(),
                               "entry_date": pd.to_datetime(cal["entry_date"]).to_numpy()})
        codes = pd.DataFrame({"line_id": pd.Series(list(securities.keys()), dtype=object),
                              "code": np.fromiter(securities.values(), dtype=np.int64, count=len(securities))})
        con.register(f"{tag}_months_stage", months)
        con.register(f"{tag}_codes_stage", codes)
        try:
            con.execute(f"CREATE TEMP TABLE {tag}_months AS SELECT CAST(eom AS DATE) AS eom, month_index, formed, "
                        "CAST(formation_date AS DATE) AS formation_date, CAST(entry_date AS DATE) AS entry_date "
                        f"FROM {tag}_months_stage")
            con.execute(f"CREATE TEMP TABLE {tag}_codes AS SELECT CAST(line_id AS VARCHAR) AS line_id, "
                        f"CAST(code AS BIGINT) AS code FROM {tag}_codes_stage")
        finally:
            con.unregister(f"{tag}_months_stage")
            con.unregister(f"{tag}_codes_stage")
        horizon_list = [_check_horizon(h) for h in horizons]
        con.execute(f"""
            CREATE TEMP TABLE {tag}_windows AS
            SELECT m.month_index, m.eom, h.h AS horizon_months, w.exit_date AS expected_end,
                   w.eom IS NOT NULL AND w.formation_date = m.formation_date AND w.entry_date = m.entry_date
                       AS aligned,
                   coalesce(w.eom IS NOT NULL AND w.formation_date = m.formation_date
                            AND w.entry_date = m.entry_date AND w.exit_date IS NOT NULL
                            AND CAST(w.exit_date AS TIMESTAMP) + INTERVAL 1 DAY + INTERVAL 12 HOUR <= ?
                            AND m.eom < ?, false) AS matured
            FROM {tag}_months m CROSS JOIN (SELECT unnest(?::INTEGER[]) AS h) h
            LEFT JOIN read_parquet({sql_text(windows_path.as_posix())}) w ON w.eom = m.eom AND w.h = h.h
            WHERE m.formed
        """, [cutoff, eom_before, horizon_list])
        maturity = con.execute(f"SELECT month_index, horizon_months, expected_end, matured FROM {tag}_windows "
                               "ORDER BY horizon_months, month_index").df()
        alignment = dict(con.execute(f"SELECT aligned, count(*) FROM {tag}_windows GROUP BY 1").fetchall())
        info: dict[str, Any] = {"label_sha": label_sha, "provisional": spec.get("provisional"),
                                "holdout_start": spec.get("holdout_start"), "eom_before": eom_before.isoformat(),
                                "allow_holdout": bool(allow_holdout), "holdout_wave": holdout_wave,
                                "holdout_opening": opening, "label_cutoff": cutoff.isoformat(),
                                "windows_aligned": int(alignment.get(True, 0)),
                                "windows_not_aligned": int(alignment.get(False, 0)),
                                "windows_sha256": record["windows_sha256"],
                                "files": {name: record["files"][name]["sha256"] for name in
                                          (Path(p).parent.name + "/" + Path(p).name for p in files)}}
        literal = "[" + ", ".join(sql_text(p) for p in files) + "]"
        status_case = " ".join(f"WHEN {code} THEN {pair[0]}" for code, pair in _R3B_STATUS.items())
        terminal_case = " ".join(f"WHEN {code} THEN {pair[1]}" for code, pair in _R3B_STATUS.items())
        usable = ", ".join(str(code) for code in _R3B_STATUS)
        con.execute(f"""
            CREATE TEMP TABLE {tag}_rows AS
            SELECT r.*, w.month_index, w.matured, c.code
            FROM read_parquet({literal}, hive_partitioning=true, hive_types={{'h': INTEGER}}) r
            JOIN {tag}_windows w ON w.eom = r.eom AND w.horizon_months = r.h
            LEFT JOIN {tag}_codes c ON c.line_id = r.line_id
            WHERE r.eom < ?
        """, [eom_before])
        counts = con.execute(f"""
            SELECT h, reason, count(*) FILTER (WHERE matured AND code IS NOT NULL),
                   count(*) FILTER (WHERE NOT matured), count(*) FILTER (WHERE code IS NULL)
            FROM {tag}_rows GROUP BY ALL ORDER BY ALL
        """).fetchall()
        info["reasons"] = [{"h": int(h), "reason": LABEL_REASON_NAMES.get(int(r), str(r)), "used_rows": int(a),
                            "unmatured_rows": int(b), "rows_off_securities": int(c)} for h, r, a, b, c in counts]
        labels = con.execute(f"""
            SELECT month_index, code AS security, h AS horizon_months, ret AS forward_return,
                   CASE reason {status_case} END AS status, CASE reason {terminal_case} END AS terminal,
                   entry_date AS anchor_date
            FROM {tag}_rows WHERE matured AND code IS NOT NULL AND reason IN ({usable})
            ORDER BY horizon_months, month_index, security
        """).df()
        for suffix in ("months", "codes", "windows", "rows"):
            con.execute(f"DROP TABLE IF EXISTS {tag}_{suffix}")
        info["label_rows"] = len(labels)
        return labels, maturity, info


def _check_label_batch(batch: pa.RecordBatch, low: int, high: int, last: tuple[int, str] | None
                       ) -> tuple[int, str] | None:
    n = batch.num_rows
    if n == 0:
        return last
    if batch.column("eom").null_count or batch.column("line_id").null_count:
        raise LabelMatrixError("eom and line_id are never NULL")
    eom = _days(batch.column("eom"))
    if eom.min() < low or eom.max() > high:
        raise LabelMatrixError("a row's formation year differs from its file's year")
    month_end = np.array([(dt.date(1970, 1, 1) + dt.timedelta(days=int(d)) + dt.timedelta(days=1)).day == 1
                          for d in np.unique(eom)])
    if not month_end.all():
        raise LabelMatrixError("eom must be a calendar month end")
    line = np.asarray(batch.column("line_id").to_pylist(), dtype=object)
    ordered = (eom[1:] > eom[:-1]) | ((eom[1:] == eom[:-1]) & (line[1:] > line[:-1]))
    if not ordered.all() or (last is not None and (int(eom[0]), str(line[0])) <= last):
        raise LabelMatrixError("rows must be strictly increasing in (eom, line_id)")
    reason = batch.column("reason").to_numpy(zero_copy_only=False).astype(np.int16)
    unknown = set(np.unique(reason).tolist()) - set(LABEL_REASON_NAMES)
    if unknown:
        raise LabelMatrixError(f"unknown label reasons {sorted(unknown)}")
    entry_null = np.asarray(batch.column("entry_date").is_null().to_numpy(zero_copy_only=False), dtype=bool)
    exit_null = np.asarray(batch.column("exit_date").is_null().to_numpy(zero_copy_only=False), dtype=bool)
    ret_null = np.asarray(batch.column("ret").is_null().to_numpy(zero_copy_only=False), dtype=bool)
    entry = _days(batch.column("entry_date"), 0)
    exit_ = _days(batch.column("exit_date"), 0)
    if (~entry_null & (entry <= eom)).any():
        raise LabelMatrixError("an entry session must be after the month end (look-ahead)")
    if (~entry_null & ~exit_null & (exit_ <= entry)).any():
        raise LabelMatrixError("an exit session must be after the entry session")
    valid = reason <= LABEL_REASONS["valid_terminal_policy"]
    if (valid & (ret_null | entry_null | exit_null)).any():
        raise LabelMatrixError("a valid label needs a return, an entry and an exit session")
    if ((reason == LABEL_REASONS["terminal_pending"]) & ~ret_null).any():
        raise LabelMatrixError("terminal_pending labels carry no return (never imputed)")
    return int(eom[-1]), str(line[-1])


def _write_atomic_text(path: Path, text: str) -> None:
    tmp = path.with_name(f".{path.name}.{os.getpid()}.{time.monotonic_ns()}.tmp")
    tmp.write_text(text, encoding="utf-8")
    os.replace(tmp, path)


__all__ = [
    "LABEL_REASONS",
    "LABEL_REASON_NAMES",
    "LABEL_SCHEMA",
    "LABEL_STORE_VERSION",
    "WINDOW_SCHEMA",
    "LabelHoldoutError",
    "LabelMatrix",
    "LabelMatrixError",
    "compute_label_sha",
]
