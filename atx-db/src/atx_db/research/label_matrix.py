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

``label_sha`` = sha256 of canonical JSON of the label spec (:func:`compute_label_sha`); the spec
must say ``provisional`` (true for the price wave's labels, node 1.12; false for the final
labels, node 3.8) and may carry ``holdout_start``: then every read must name ``eom_before``
and a read past the holdout start is refused unless ``allow_holdout=True`` (ruling R-6: the
holdout is opened once, by the trial registry's owner, with final labels).

The builder (node 1.12's provisional labels, node 3.8's final labels) writes one file per
(h, year) with :meth:`LabelMatrix.write` and the windows with :meth:`LabelMatrix.write_windows`;
:meth:`LabelMatrix.r3b_inputs` turns them into the R3b engine's ``labels``/``maturity`` frames.
"""

from __future__ import annotations

import datetime as dt
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


def compute_label_sha(spec: Mapping[str, object]) -> str:
    """sha256 of canonical JSON of the label spec (inputs, code digest, calendar, rules).

    ``spec['provisional']`` must be a bool; ``spec['holdout_start']`` (ISO date or None)
    seals formations from that month end on.
    """
    if not isinstance(spec.get("provisional"), bool):
        raise LabelMatrixError("a label spec says provisional=true|false")
    holdout = spec.get("holdout_start")
    if holdout is not None:
        dt.date.fromisoformat(str(holdout))
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
        return dict(payload["spec"])

    # -- write ---------------------------------------------------------------
    def write(self, label_sha: str, h: int, year: int, batches: Iterable[pa.RecordBatch | pa.Table],
              meta: Mapping[str, object] | None = None) -> Path:
        """Write one (horizon, formation year) file (temp + rename); an existing file is kept."""
        self.spec(label_sha)
        target = self.path(label_sha, h, year)
        if self.has(label_sha, h, year):
            return target
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
        tmp = target.with_name(f"._windows.{os.getpid()}.{time.monotonic_ns()}.tmp")
        pq.write_table(table.sort_by([("eom", "ascending"), ("h", "ascending")]), tmp, compression="zstd")
        os.replace(tmp, target)
        return target

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

    def _guard(self, label_sha: str, eom_before: dt.date, allow_holdout: bool) -> dict[str, Any]:
        spec = self.spec(label_sha)
        holdout = spec.get("holdout_start")
        if holdout is not None and eom_before > dt.date.fromisoformat(str(holdout)) and not allow_holdout:
            raise LabelHoldoutError(f"labels {label_sha[:12]}: reading formations up to {eom_before} opens the "
                                    f"holdout sealed from {holdout} (ruling R-6: once, by the trial registry)")
        return spec

    def files(self, label_sha: str, horizons: Sequence[int], years: Sequence[int] | None = None) -> list[str]:
        paths = []
        for h in horizons:
            directory = self.directory(label_sha) / f"h={_check_horizon(h)}"
            for path in sorted(directory.glob("year=*.parquet")):
                year = int(path.stem.split("=", 1)[1])
                if (years is None or year in years) and path.with_suffix(".json").is_file():
                    paths.append(path.as_posix())
        return paths

    def scan(self, label_sha: str, horizons: Sequence[int], *, eom_before: dt.date,
             years: Sequence[int] | None = None, allow_holdout: bool = False) -> duckdb.DuckDBPyRelation:
        """Rows with ``eom < eom_before`` at the horizons (column ``h`` from the path)."""
        self._guard(label_sha, eom_before, allow_holdout)
        files = self.files(label_sha, horizons, years)
        if not files:
            raise LabelMatrixError(f"labels {label_sha[:12]}: no files for horizons {list(horizons)}")
        literal = "[" + ", ".join(sql_text(p) for p in files) + "]"
        return self.con.sql(f"SELECT * FROM read_parquet({literal}, hive_partitioning=true, "
                            f"hive_types={{'h': INTEGER}}) WHERE eom < DATE '{eom_before.isoformat()}'")

    def r3b_inputs(self, label_sha: str, horizons: Sequence[int], *, calendar: pd.DataFrame,
                   securities: Mapping[str, int], label_cutoff: dt.datetime, eom_before: dt.date | None = None,
                   allow_holdout: bool = False) -> tuple[pd.DataFrame, pd.DataFrame, dict[str, Any]]:
        """R3b ``labels`` and ``maturity`` frames from the label set (the ``load_label_inputs`` counterpart).

        A window is matured when its formation and entry sessions are the calendar's and its
        exit + 1 day 12:00 is <= ``label_cutoff`` (the R3a rule). Labels are the rows at
        matured windows of formations before ``eom_before`` on lines in ``securities``:
        reasons 0-2 -> status 0 (terminal 0/1/2), 3 -> 1, 4 -> 2; the others carry no return
        and are absent (R3b counts them missing). ``anchor_date`` is the row's entry date.
        """
        import pandas as pd

        spec = self.spec(label_sha)
        cal = calendar.sort_values("month_index", kind="stable").reset_index(drop=True)
        eoms = (pd.to_datetime(cal["month_start"]).dt.normalize() + pd.offsets.MonthEnd(0)).dt.date
        if eom_before is None:
            holdout = spec.get("holdout_start")
            eom_before = dt.date.fromisoformat(str(holdout)) if holdout is not None else \
                max(eoms) + dt.timedelta(days=1)
        self._guard(label_sha, eom_before, allow_holdout)
        windows_path = self.directory(label_sha) / "_windows.parquet"
        if not windows_path.is_file():
            raise LabelMatrixError(f"labels {label_sha[:12]} have no _windows.parquet")
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
        con.register("_lm_months_stage", months)
        con.register("_lm_codes_stage", codes)
        try:
            con.execute("CREATE OR REPLACE TEMP TABLE _lm_months AS SELECT CAST(eom AS DATE) AS eom, month_index, "
                        "formed, CAST(formation_date AS DATE) AS formation_date, CAST(entry_date AS DATE) AS entry_date "
                        "FROM _lm_months_stage")
            con.execute("CREATE OR REPLACE TEMP TABLE _lm_codes AS SELECT CAST(line_id AS VARCHAR) AS line_id, "
                        "CAST(code AS BIGINT) AS code FROM _lm_codes_stage")
        finally:
            con.unregister("_lm_months_stage")
            con.unregister("_lm_codes_stage")
        horizon_list = [_check_horizon(h) for h in horizons]
        con.execute(f"""
            CREATE OR REPLACE TEMP TABLE _lm_windows AS
            SELECT m.month_index, m.eom, h.h AS horizon_months, w.exit_date AS expected_end,
                   w.eom IS NOT NULL AND w.formation_date = m.formation_date AND w.entry_date = m.entry_date
                       AS aligned,
                   coalesce(w.eom IS NOT NULL AND w.formation_date = m.formation_date
                            AND w.entry_date = m.entry_date AND w.exit_date IS NOT NULL
                            AND CAST(w.exit_date AS TIMESTAMP) + INTERVAL 1 DAY + INTERVAL 12 HOUR <= ?
                            AND m.eom < ?, false) AS matured
            FROM _lm_months m CROSS JOIN (SELECT unnest(?::INTEGER[]) AS h) h
            LEFT JOIN read_parquet({sql_text(windows_path.as_posix())}) w ON w.eom = m.eom AND w.h = h.h
            WHERE m.formed
        """, [cutoff, eom_before, horizon_list])
        maturity = con.execute("SELECT month_index, horizon_months, expected_end, matured FROM _lm_windows "
                               "ORDER BY horizon_months, month_index").df()
        alignment = dict(con.execute("SELECT aligned, count(*) FROM _lm_windows GROUP BY 1").fetchall())
        files = self.files(label_sha, horizon_list)
        info: dict[str, Any] = {"label_sha": label_sha, "provisional": spec.get("provisional"),
                                "holdout_start": spec.get("holdout_start"), "eom_before": eom_before.isoformat(),
                                "label_cutoff": cutoff.isoformat(),
                                "windows_aligned": int(alignment.get(True, 0)),
                                "windows_not_aligned": int(alignment.get(False, 0)),
                                "files": {Path(p).parent.name + "/" + Path(p).name:
                                          json.loads(Path(p).with_suffix(".json").read_text(encoding="utf-8"))[
                                              "file_sha256"] for p in files}}
        if not files:
            empty = pd.DataFrame({"month_index": np.zeros(0, np.int64), "security": np.zeros(0, np.int64),
                                  "horizon_months": np.zeros(0, np.int64), "forward_return": np.zeros(0),
                                  "status": np.zeros(0, np.int64), "terminal": np.zeros(0, np.int64),
                                  "anchor_date": pd.Series([], dtype="datetime64[us]")})
            return empty, maturity, {**info, "label_rows": 0}
        literal = "[" + ", ".join(sql_text(p) for p in files) + "]"
        status_case = " ".join(f"WHEN {code} THEN {pair[0]}" for code, pair in _R3B_STATUS.items())
        terminal_case = " ".join(f"WHEN {code} THEN {pair[1]}" for code, pair in _R3B_STATUS.items())
        usable = ", ".join(str(code) for code in _R3B_STATUS)
        con.execute(f"""
            CREATE OR REPLACE TEMP TABLE _lm_rows AS
            SELECT r.*, w.month_index, w.matured, c.code
            FROM read_parquet({literal}, hive_partitioning=true, hive_types={{'h': INTEGER}}) r
            JOIN _lm_windows w ON w.eom = r.eom AND w.horizon_months = r.h
            LEFT JOIN _lm_codes c ON c.line_id = r.line_id
            WHERE r.eom < ?
        """, [eom_before])
        counts = con.execute("""
            SELECT h, reason, count(*) FILTER (WHERE matured AND code IS NOT NULL),
                   count(*) FILTER (WHERE NOT matured), count(*) FILTER (WHERE code IS NULL)
            FROM _lm_rows GROUP BY ALL ORDER BY ALL
        """).fetchall()
        info["reasons"] = [{"h": int(h), "reason": LABEL_REASON_NAMES.get(int(r), str(r)), "used_rows": int(a),
                            "unmatured_rows": int(b), "rows_off_securities": int(c)} for h, r, a, b, c in counts]
        labels = con.execute(f"""
            SELECT month_index, code AS security, h AS horizon_months, ret AS forward_return,
                   CASE reason {status_case} END AS status, CASE reason {terminal_case} END AS terminal,
                   entry_date AS anchor_date
            FROM _lm_rows WHERE matured AND code IS NOT NULL AND reason IN ({usable})
            ORDER BY horizon_months, month_index, security
        """).df()
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
