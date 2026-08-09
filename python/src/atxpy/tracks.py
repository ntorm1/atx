"""Read-only DuckDB/pyarrow view over the backtest track lakehouse (Task D4,
backtest-production-lakehouse sprint).

Consumes two lakehouse artifacts this sprint landed under one `lake_root`,
and never writes to either of them:

  * D2's Parquet track store -- hive-partitioned batch files under
    `<lake_root>/tracks/underlier=<U>/family=<F>/batch-NNNNNN.parquet`
    (schema v1: `track_key`/`date`/`ts_ns` + the 25 frozen series columns +
    the 5-column nullable swap lane; see
    `atx-vol/include/atx/vol/track_store.hpp` (Tier-B) and
    `atx-vol/include/atx/vol/detail/backtest_series_columns.hpp`, the two
    C++ sources of truth `ALL_COLUMNS` below is copied from verbatim).
  * D3's SQLite catalog -- `<lake_root>/catalog.sqlite` (WAL mode), tables
    `tracks`/`trials` (see
    `atx-vol/include/atx/vol/catalog.hpp`, Tier-B).

`load()` reads Parquet directly through DuckDB (a plain filesystem read --
DuckDB never opens these files for write); `catalog()`/`returns_matrix()`
open `catalog.sqlite` with SQLite's own `mode=ro` URI flag, which refuses to
create the file and refuses writes at the driver level. Both paths are
therefore incapable of taking a lock that could block a concurrent C++
writer (`TrackStore::write_staging`, `compact()`, or `Catalog`'s own
writes) -- see each function's docstring for specifics.
"""

from __future__ import annotations

import sqlite3
from pathlib import Path
from typing import TYPE_CHECKING, Iterable, Sequence

if TYPE_CHECKING:
    import pandas as pd
    import pyarrow as pa

# The 25 frozen series columns, in `kBacktestSeriesColumns` order
# (atx-vol/include/atx/vol/detail/backtest_series_columns.hpp). Copied
# verbatim, not re-derived -- that header is the frozen source of truth.
_SERIES_COLUMNS = (
    "pnl_total", "pnl_delta", "pnl_gamma", "pnl_vega", "pnl_vanna", "pnl_volga",
    "pnl_theta", "pnl_rho", "pnl_charm", "pnl_unexplained", "pnl_settlement",
    "pnl_shares", "financing", "cost", "nav", "cash", "gross_delta", "gross_gamma",
    "gross_vega", "gross_theta", "turnover_notional", "turnover_vega", "n_open_lots",
    "n_unpriced_lots", "n_unpriced_greeks",
)
# The 5 nullable swap-lane columns, in track_store.hpp's documented order.
_SWAP_LANE_COLUMNS = ("swap_pv", "swap_pnl", "gross_vega_abs", "nav_liquidation", "step_pnl_total")
# Virtual columns DuckDB synthesizes from the `underlier=<U>/family=<F>`
# hive-partition directory segments when `hive_partitioning=1` -- not
# present in the Parquet files' own schema (schema_v1_fields(), track_store.cpp).
_HIVE_COLUMNS = ("underlier", "family")

# Every column `load(columns=...)` may project: the 33 stored Parquet
# columns plus the 2 virtual hive columns above. An allow-list, not a
# free-form SQL identifier -- `columns=` is validated against this set (and
# then double-quoted) before it ever reaches DuckDB, so it can never be used
# to inject SQL.
ALL_COLUMNS: tuple[str, ...] = (
    ("track_key", "date", "ts_ns") + _SERIES_COLUMNS + _SWAP_LANE_COLUMNS + _HIVE_COLUMNS
)

# atx-vol/include/atx/vol/catalog.hpp: kCatalogDbName.
_CATALOG_DB_NAME = "catalog.sqlite"


def _require_duckdb():
    try:
        import duckdb
    except ImportError as exc:  # pragma: no cover
        raise ImportError("atxpy.tracks needs duckdb: pip install atxpy[lakehouse]") from exc
    return duckdb


def _require_pandas():
    try:
        import pandas as pd
    except ImportError as exc:  # pragma: no cover
        raise ImportError("atxpy.tracks needs pandas: pip install atxpy[lakehouse]") from exc
    return pd


def _quote_ident(name: str) -> str:
    """Double-quotes a SQL identifier already checked against `ALL_COLUMNS` --
    never called on caller-supplied text that hasn't passed that allow-list."""
    return '"' + name.replace('"', '""') + '"'


def _build_load_query(
    lake_root,
    *,
    underlier: str | None = None,
    family: str | None = None,
    date_range: tuple[str, str] | None = None,
    track_keys: Iterable[str] | None = None,
    columns: Sequence[str] | None = None,
) -> tuple[str, list]:
    """Builds the parameterized DuckDB SQL + bind params for `load()`. Split
    out from `load()` so tests can `EXPLAIN` the exact query DuckDB will plan
    for the real read, to verify partition pruning independent of running it."""
    glob = str(Path(lake_root) / "tracks" / "**" / "*.parquet")
    params: list = [glob]

    if columns is None:
        select = "*"
    else:
        columns = list(columns)
        if not columns:
            raise ValueError("columns=[] would select zero columns")
        unknown = [c for c in columns if c not in ALL_COLUMNS]
        if unknown:
            raise ValueError(f"load(): unknown column(s) {unknown!r}; see atxpy.tracks.ALL_COLUMNS")
        select = ", ".join(_quote_ident(c) for c in columns)

    sql = f"SELECT {select} FROM read_parquet(?, hive_partitioning=1)"

    where: list[str] = []
    if underlier is not None:
        where.append("underlier = ?")
        params.append(underlier)
    if family is not None:
        where.append("family = ?")
        params.append(family)
    if date_range is not None:
        start, end = date_range
        where.append("date >= ? AND date <= ?")
        params.extend([start, end])
    if track_keys is not None:
        track_keys = list(track_keys)
        if not track_keys:
            raise ValueError("track_keys=[] would match no rows")
        placeholders = ", ".join(["?"] * len(track_keys))
        where.append(f"track_key IN ({placeholders})")
        params.extend(track_keys)

    if where:
        sql += " WHERE " + " AND ".join(where)
    # Deterministic output order, matching the on-disk sort within each
    # compacted batch (compact()'s (track_key, date) row order).
    sql += " ORDER BY track_key, date"
    return sql, params


def load(
    lake_root,
    *,
    underlier: str | None = None,
    family: str | None = None,
    date_range: tuple[str, str] | None = None,
    track_keys: Iterable[str] | None = None,
    columns: Sequence[str] | None = None,
) -> "pa.Table":
    """Reads tracks from the Parquet lakehouse under `lake_root`.

    Opens a private in-memory DuckDB connection and scans
    `<lake_root>/tracks/**/*.parquet` directly with `hive_partitioning=1`
    (the `underlier=<U>/family=<F>` directory segments become extra
    `underlier`/`family` columns in the result). This is a plain filesystem
    read -- the connection never opens a Parquet file for write and holds no
    cross-process lock, so it can never block a concurrent C++ writer
    publishing a new batch under the same `lake_root`.

    `underlier`/`family` push down to DuckDB's hive-partition pruning (a
    whole partition directory is skipped without opening any file inside
    it); `date_range`/`track_keys` push down to each batch file's Parquet
    row-group statistics. Every filter value is bound as a DuckDB
    prepared-statement parameter -- never interpolated into the SQL text --
    so a `track_keys`/`underlier`/`family` value cannot inject SQL.

    date_range: optional inclusive `(start, end)` pair of "YYYY-MM-DD" strings.
    track_keys: optional iterable of track_key hex strings (an `IN` filter).
    columns: optional iterable restricting the result to exactly these
      columns, checked against `ALL_COLUMNS`; default is every column
      (the 33 stored columns plus the 2 virtual hive columns).

    Returns a `pyarrow.Table` sorted by `(track_key, date)`.
    """
    duckdb = _require_duckdb()
    sql, params = _build_load_query(
        lake_root, underlier=underlier, family=family, date_range=date_range,
        track_keys=track_keys, columns=columns,
    )
    con = duckdb.connect(":memory:")
    try:
        return con.execute(sql, params).to_arrow_table()
    finally:
        con.close()


def _catalog_ro_uri(lake_root) -> str:
    db_path = Path(lake_root) / _CATALOG_DB_NAME
    # `Path.as_uri()` requires an absolute path and percent-encodes it
    # correctly (drive letters, spaces, ...) -- the standard way to build a
    # `sqlite3` URI connection string across platforms.
    return db_path.resolve().as_uri() + "?mode=ro"


def catalog(lake_root) -> "pd.DataFrame":
    """The D3 catalog's `tracks` table (one row per registered track), read
    via a strictly read-only SQLite connection (`mode=ro` URI): it refuses to
    create `catalog.sqlite` if absent and refuses any write, so it can never
    take a lock that blocks a concurrent C++ writer
    (`Catalog::register_staging`/`mark_compacted`). Rows are ordered by
    `track_key` for deterministic output.
    """
    pd = _require_pandas()
    con = sqlite3.connect(_catalog_ro_uri(lake_root), uri=True)
    try:
        return pd.read_sql_query("SELECT * FROM tracks ORDER BY track_key", con)
    finally:
        con.close()


def returns_matrix(lake_root, sweep_id: str) -> "pd.DataFrame":
    """T x N daily-return pivot of every track trialled under `sweep_id`, for
    B5's CSCV/PBO harness (`atxpy.pbo.cscv_pbo`).

    N is the distinct `track_key`s the D3 catalog's `trials` table recorded
    for `sweep_id`, column-ordered by each track's first `trial_id` (the
    catalog's own determinism convention -- see `Catalog::trial_stats`'s doc
    comment in catalog.cpp). For each track, the return on date `t` is the
    simple return of its `nav` series, `nav[t] / nav[t-1] - 1` (`nav` is
    schema v1's only per-track equity/level column; see track_store.hpp).

    T is the intersection of dates present, with a non-null return, across
    ALL N tracks -- "NaN-free alignment": a date is dropped from the pivot
    entirely (for every column) rather than filled, so a track missing a
    date (or the structural NaN on every track's own first date, where no
    prior `nav` exists to form a return) can never leave a fabricated 0%
    return in the matrix B5's PBO statistics are computed over.

    Empty (`sweep_id` has no trials, or the trialled tracks share no common
    return date) returns an empty DataFrame.
    """
    pd = _require_pandas()
    con = sqlite3.connect(_catalog_ro_uri(lake_root), uri=True)
    try:
        trial_tracks = pd.read_sql_query(
            "SELECT track_key, MIN(trial_id) AS first_trial_id FROM trials "
            "WHERE sweep_id = ? GROUP BY track_key ORDER BY first_trial_id",
            con,
            params=(sweep_id,),
        )
    finally:
        con.close()

    track_keys: list[str] = trial_tracks["track_key"].tolist()
    if not track_keys:
        return pd.DataFrame()

    table = load(lake_root, track_keys=track_keys, columns=["track_key", "date", "nav"])
    df = table.to_pandas()
    df = df.sort_values(["track_key", "date"])
    df["return"] = df.groupby("track_key")["nav"].pct_change()

    wide = df.pivot(index="date", columns="track_key", values="return")
    wide = wide.reindex(columns=track_keys)  # first-trial_id order, not alphabetical
    wide = wide.dropna(axis=0, how="any")
    return wide.sort_index()
