"""Tests for atxpy.tracks (Task D4: DuckDB/pyarrow read path over the D2
Parquet track store + D3 SQLite catalog).

No compiled C++ artifact that produces a lake exists in this worktree yet
(no `build/` directory, no `track_compact.exe`; `atx-vol/tests/
track_store_test.cpp` writes its own lakes but is not itself buildable
without a full CMake/vcpkg configure) -- see the D4 report for the fixture
choice this implies. Every fixture lake here is built directly with
pyarrow/sqlite3, matching the LANDED schema byte-for-byte against its C++
sources of truth:
  * `atx-vol/include/atx/vol/track_store.hpp` (Tier-B; schema
    v1's column list/order/nullability) and `atx-vol/src/track_store.cpp`'s
    `schema_v1_fields()` (the function that actually builds it);
  * `atx-vol/include/atx/vol/detail/backtest_series_columns.hpp` (the 25
    frozen series columns, in order);
  * `atx-vol/include/atx/vol/catalog.hpp` (Tier-B) /
    `atx-vol/src/catalog.cpp` (the `tracks`/`trials` SQLite DDL).
"""

from __future__ import annotations

import datetime as dt
import sqlite3
from pathlib import Path

import duckdb
import pandas as pd
import pyarrow as pa
import pyarrow.parquet as pq
import pytest

from atxpy import tracks

# Independently transcribed from the C++ sources of truth (NOT imported from
# tracks.py -- reusing tracks.py's own copy here would make the schema-drift
# guard below circular: a typo/reorder in tracks.py's list would silently
# build a matching fixture and never fail). Order/names/nullability copied
# verbatim from:
#   atx-vol/include/atx/vol/detail/backtest_series_columns.hpp (kBacktestSeriesColumns)
SERIES_COLUMNS = (
    "pnl_total", "pnl_delta", "pnl_gamma", "pnl_vega", "pnl_vanna", "pnl_volga",
    "pnl_theta", "pnl_rho", "pnl_charm", "pnl_unexplained", "pnl_settlement",
    "pnl_shares", "financing", "cost", "nav", "cash", "gross_delta", "gross_gamma",
    "gross_vega", "gross_theta", "turnover_notional", "turnover_vega", "n_open_lots",
    "n_unpriced_lots", "n_unpriced_greeks",
)
# atx-vol/include/atx/vol/track_store.hpp (schema comment)
# and track_store.cpp's schema_v1_fields().
SWAP_LANE_COLUMNS = ("swap_pv", "swap_pnl", "gross_vega_abs", "nav_liquidation", "step_pnl_total")

KEY_A = "a1" * 32
KEY_B = "b2" * 32
KEY_C = "c3" * 32

DATES = ["2026-01-02", "2026-01-05", "2026-01-06", "2026-01-07", "2026-01-08"]
NAV_A = [100.0, 101.0, 99.0, 102.0, 103.0]
NAV_B = [100.0, 100.5, 100.2, 101.0, 101.5]
NAV_C = [100.0, 99.0, 98.0, 97.0, 96.0]


def _schema_v1() -> pa.Schema:
    fields = [
        pa.field("track_key", pa.utf8(), nullable=False),
        pa.field("date", pa.date32(), nullable=False),
        pa.field("ts_ns", pa.int64(), nullable=False),
    ]
    fields += [pa.field(c, pa.float64(), nullable=False) for c in SERIES_COLUMNS]
    fields += [pa.field(c, pa.float64(), nullable=True) for c in SWAP_LANE_COLUMNS]
    return pa.schema(fields)


_SCHEMA_V1 = _schema_v1()


def _make_track_table(track_key: str, dates: list[str], nav: list[float]) -> pa.Table:
    """One track's rows, schema-v1-shaped: every series column is 0.0 except
    `nav`; the swap lane is all-NULL (an empty source vector on
    `BacktestResult`, per track_store.hpp's doc comment)."""
    rows = len(dates)
    assert len(nav) == rows
    days = [(dt.date.fromisoformat(d) - dt.date(1970, 1, 1)).days for d in dates]
    cols = {
        "track_key": pa.array([track_key] * rows, type=pa.utf8()),
        "date": pa.array(days, type=pa.date32()),
        "ts_ns": pa.array([i * 86_400_000_000_000 for i in range(rows)], type=pa.int64()),
    }
    for c in SERIES_COLUMNS:
        cols[c] = pa.array(nav if c == "nav" else [0.0] * rows, type=pa.float64())
    for c in SWAP_LANE_COLUMNS:
        cols[c] = pa.array([None] * rows, type=pa.float64())
    return pa.table(cols, schema=_SCHEMA_V1)


def _write_batch(lake_root: Path, underlier: str, family: str, batch_idx: int, table: pa.Table) -> Path:
    d = lake_root / "tracks" / f"underlier={underlier}" / f"family={family}"
    d.mkdir(parents=True, exist_ok=True)
    path = d / f"batch-{batch_idx:06d}.parquet"
    # One row group per file, zstd -- matches write_parquet_batch() (track_store.cpp).
    pq.write_table(table, path, compression="zstd")
    return path


def _basic_lake(tmp_path: Path) -> Path:
    """KEY_A/KEY_B compacted together into one SPY/straddle batch; KEY_C
    alone in a QQQ/straddle batch -- 2 partition directories, 2 files total,
    so a `underlier=SPY` filter should prune exactly one of them."""
    t_a = _make_track_table(KEY_A, DATES, NAV_A)
    t_b = _make_track_table(KEY_B, DATES, NAV_B)
    _write_batch(tmp_path, "SPY", "straddle", 0, pa.concat_tables([t_a, t_b]))
    t_c = _make_track_table(KEY_C, DATES, NAV_C)
    _write_batch(tmp_path, "QQQ", "straddle", 0, t_c)
    return tmp_path


_CATALOG_DDL = (
    "CREATE TABLE tracks(\n"
    "  track_key TEXT PRIMARY KEY,\n"
    "  underlier TEXT NOT NULL, family TEXT NOT NULL,\n"
    "  config_json TEXT NOT NULL,\n"
    "  engine_id TEXT NOT NULL, economics_rev INTEGER NOT NULL,\n"
    "  data_snapshot_id TEXT NOT NULL,\n"
    "  date_min TEXT NOT NULL, date_max TEXT NOT NULL,\n"
    "  status TEXT NOT NULL CHECK(status IN ('staging','compacted','retired')),\n"
    "  file TEXT, row_group INTEGER,\n"
    "  created_ts INTEGER NOT NULL, last_access_ts INTEGER NOT NULL);\n"
    "CREATE TABLE trials(\n"
    "  trial_id INTEGER PRIMARY KEY,\n"
    "  track_key TEXT NOT NULL REFERENCES tracks(track_key),\n"
    "  sweep_id TEXT NOT NULL, sharpe REAL, created_ts INTEGER NOT NULL);\n"
    "CREATE INDEX idx_tracks_dims ON tracks(underlier, family, date_min, date_max);\n"
)


def _build_catalog(lake_root: Path, track_rows: list[dict], trial_rows: list[dict]) -> None:
    """track_rows/trial_rows: dicts of the `tracks`/`trials` DDL's own column
    names (catalog.cpp's kSchemaDdl, reproduced verbatim above); optional
    keys fall back to simple defaults so each test only spells out what it
    cares about. Rows are inserted in list order, so `trial_rows` order
    controls `trials.trial_id` (INTEGER PRIMARY KEY, insertion order)."""
    db_path = lake_root / "catalog.sqlite"
    con = sqlite3.connect(str(db_path))
    try:
        con.executescript(_CATALOG_DDL)
        for r in track_rows:
            con.execute(
                "INSERT INTO tracks(track_key, underlier, family, config_json, engine_id, "
                "economics_rev, data_snapshot_id, date_min, date_max, status, file, row_group, "
                "created_ts, last_access_ts) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
                (
                    r["track_key"], r["underlier"], r["family"], r.get("config_json", "{}"),
                    r.get("engine_id", "eng"), r.get("economics_rev", 1),
                    r.get("data_snapshot_id", "snap"), r["date_min"], r["date_max"],
                    r.get("status", "compacted"), r.get("file"), r.get("row_group"),
                    r.get("created_ts", 1_000), r.get("last_access_ts", 1_000),
                ),
            )
        for t in trial_rows:
            con.execute(
                "INSERT INTO trials(track_key, sweep_id, sharpe, created_ts) VALUES (?,?,?,?)",
                (t["track_key"], t["sweep_id"], t.get("sharpe"), t.get("created_ts", 1_000)),
            )
        con.commit()
    finally:
        con.close()


# â”€â”€ schema-drift guard â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

def test_all_columns_matches_landed_schema_shape():
    assert len(SERIES_COLUMNS) == 25
    assert len(SWAP_LANE_COLUMNS) == 5
    assert tracks.ALL_COLUMNS[:3] == ("track_key", "date", "ts_ns")
    assert tracks.ALL_COLUMNS[-2:] == ("underlier", "family")
    assert len(tracks.ALL_COLUMNS) == 3 + 25 + 5 + 2
    # tracks.py's own copies of the frozen column lists must match this
    # file's independent transcription of the C++ headers -- this is the
    # assertion that actually catches a drift (see the comment above
    # SERIES_COLUMNS): both fixture-building here and tracks.py's SQL
    # projection allow-list must agree with the landed schema, not just
    # with each other.
    assert tracks._SERIES_COLUMNS == SERIES_COLUMNS
    assert tracks._SWAP_LANE_COLUMNS == SWAP_LANE_COLUMNS


# â”€â”€ load(): partition pruning â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

def test_load_partition_pruning_touches_only_matching_directory(tmp_path):
    lake = _basic_lake(tmp_path)
    sql, params = tracks._build_load_query(lake, underlier="SPY")
    con = duckdb.connect(":memory:")
    try:
        plan_rows = con.execute("EXPLAIN " + sql, params).fetchall()
    finally:
        con.close()
    plan_text = "\n".join(str(cell) for row in plan_rows for cell in row)
    # 2 partition directories / 2 files total in the fixture lake; filtering
    # on underlier=SPY should prune the QQQ directory without opening it.
    assert "Scanning Files: 1/2" in plan_text


# â”€â”€ load(): column projection â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

def test_load_column_projection_returns_only_requested_columns(tmp_path):
    lake = _basic_lake(tmp_path)
    table = tracks.load(lake, columns=["track_key", "date", "nav"])
    assert table.column_names == ["track_key", "date", "nav"]

    df = table.to_pandas()
    row = df[(df["track_key"] == KEY_A) & (df["date"] == dt.date(2026, 1, 2))]
    assert row["nav"].iloc[0] == pytest.approx(100.0)


def test_load_unknown_column_raises_value_error(tmp_path):
    lake = _basic_lake(tmp_path)
    with pytest.raises(ValueError):
        tracks.load(lake, columns=["not_a_real_column"])


# â”€â”€ load(): track_keys / date_range filters â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

def test_load_filters_by_track_keys_and_date_range(tmp_path):
    lake = _basic_lake(tmp_path)
    table = tracks.load(
        lake, track_keys=[KEY_A, KEY_C], date_range=("2026-01-05", "2026-01-06"),
        columns=["track_key", "date", "nav"],
    )
    df = table.to_pandas()
    assert set(df["track_key"]) == {KEY_A, KEY_C}
    assert set(df["date"]) == {dt.date(2026, 1, 5), dt.date(2026, 1, 6)}
    assert len(df) == 4  # 2 tracks x 2 dates

    row = df[(df["track_key"] == KEY_A) & (df["date"] == dt.date(2026, 1, 5))]
    assert row["nav"].iloc[0] == pytest.approx(101.0)


def test_load_default_reads_every_partition(tmp_path):
    lake = _basic_lake(tmp_path)
    table = tracks.load(lake, columns=["track_key"])
    assert sorted(set(table.column("track_key").to_pylist())) == sorted([KEY_A, KEY_B, KEY_C])
    assert table.num_rows == 3 * len(DATES)


# â”€â”€ load(): read-only â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

def test_load_does_not_mutate_lakehouse_files(tmp_path):
    lake = _basic_lake(tmp_path)
    parquet_files = sorted((lake / "tracks").rglob("*.parquet"))
    mtimes_before = {p: p.stat().st_mtime_ns for p in parquet_files}

    tracks.load(lake)

    mtimes_after = {p: p.stat().st_mtime_ns for p in parquet_files}
    assert mtimes_before == mtimes_after
    all_files_after = sorted(p for p in (lake / "tracks").rglob("*") if p.is_file())
    assert all_files_after == parquet_files  # no stray temp/lock files


# â”€â”€ catalog() â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

def test_catalog_returns_tracks_table(tmp_path):
    lake = tmp_path
    _build_catalog(
        lake,
        track_rows=[
            {
                "track_key": KEY_A, "underlier": "SPY", "family": "straddle",
                "date_min": DATES[0], "date_max": DATES[-1], "status": "compacted",
                "file": "tracks/underlier=SPY/family=straddle/batch-000000.parquet",
                "row_group": 0,
            },
            {
                "track_key": KEY_B, "underlier": "SPY", "family": "straddle",
                "date_min": DATES[0], "date_max": DATES[-1], "status": "staging",
            },
        ],
        trial_rows=[],
    )

    df = tracks.catalog(lake)

    assert list(df["track_key"]) == [KEY_A, KEY_B]  # ORDER BY track_key
    row_a = df[df["track_key"] == KEY_A].iloc[0]
    assert row_a["status"] == "compacted"
    assert row_a["underlier"] == "SPY"
    assert int(row_a["row_group"]) == 0
    row_b = df[df["track_key"] == KEY_B].iloc[0]
    assert row_b["status"] == "staging"
    assert pd.isna(row_b["file"])
    assert pd.isna(row_b["row_group"])


def test_catalog_missing_db_raises_and_does_not_create_file(tmp_path):
    lake = tmp_path  # no catalog.sqlite written
    with pytest.raises(sqlite3.OperationalError):
        tracks.catalog(lake)
    assert not (lake / "catalog.sqlite").exists()


# â”€â”€ returns_matrix() â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

def test_returns_matrix_pivots_nan_free_with_expected_values(tmp_path):
    lake = _basic_lake(tmp_path)
    _build_catalog(
        lake,
        track_rows=[
            {"track_key": k, "underlier": "SPY" if k != KEY_C else "QQQ", "family": "straddle",
             "date_min": DATES[0], "date_max": DATES[-1]}
            for k in (KEY_A, KEY_B, KEY_C)
        ],
        # Insertion order != alphabetical track_key order, so a passing
        # "columns == [KEY_B, KEY_C, KEY_A]" assertion actually exercises the
        # first-trial_id reindex rather than pandas' default sort.
        trial_rows=[
            {"track_key": KEY_B, "sweep_id": "sweep-1", "sharpe": 0.3},
            {"track_key": KEY_C, "sweep_id": "sweep-1", "sharpe": 0.1},
            {"track_key": KEY_A, "sweep_id": "sweep-1", "sharpe": 0.2},
        ],
    )

    wide = tracks.returns_matrix(lake, "sweep-1")

    assert list(wide.columns) == [KEY_B, KEY_C, KEY_A]
    assert wide.shape == (4, 3)  # 5 dates - 1 structural-NaN first date = T=4; N=3 tracks
    assert not wide.isna().values.any()

    expected_a = [NAV_A[i] / NAV_A[i - 1] - 1.0 for i in range(1, len(NAV_A))]
    assert wide[KEY_A].tolist() == pytest.approx(expected_a)
    expected_b = [NAV_B[i] / NAV_B[i - 1] - 1.0 for i in range(1, len(NAV_B))]
    assert wide[KEY_B].tolist() == pytest.approx(expected_b)


def test_returns_matrix_unknown_sweep_returns_empty(tmp_path):
    lake = _basic_lake(tmp_path)
    _build_catalog(lake, track_rows=[], trial_rows=[])

    wide = tracks.returns_matrix(lake, "no-such-sweep")

    assert wide.empty
