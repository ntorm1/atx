"""Cross-checks atxpy.tracks against a REAL C++-written lake (Task D5).

D4's own tests (test_tracks.py) only ever read Python-built fixture lakes --
see that file's own doc comment: "No compiled C++ artifact that produces a
lake exists in this worktree yet". This file closes that writer/reader
integration gap from the READER side. It expects a lake produced by the
paired gtest,
``SweepDriverTest.WritesRealCxxLakeAndCsvSidecarForPythonReadCrossCheck``
(atx-vol/tests/sweep_driver_test.cpp), which runs a REAL sweep through
``run_sweep`` -> ``TrackStore::write_staging`` -> ``compact()`` ->
``Catalog::mark_compacted`` and writes both the lake and an
``expected_navs.csv`` side-car of the exact ``(track_key, date, nav)``
values it computed in memory (the SAME numbers ``run_sweep``'s in-process
``BacktestResult``s held, before anything touched Parquet/SQLite).

Every test here is a no-op skip (not a failure) when the fixture lake is
absent -- this file is not part of the default test run; it is the reader
half of a manually-run pair.

## Running the pair manually

    # 1. Build atx-vol-tests (ATX_VOL_LAKEHOUSE=ON):
    powershell.exe scripts/atx-build.ps1 build atx-vol-tests

    # 2. Run the gtest with the fixture-lake env var set -- it GTEST_SKIPs
    #    (not fails) when the var is unset, so this leg is opt-in and never
    #    blocks the default `atx-vol-tests` run:
    $env:ATX_VOL_D5_FIXTURE_LAKE = "C:/atx-wt/pool-8/build/d5-fixture-lake"
    build/bin/atx-vol-tests.exe `
        --gtest_filter=SweepDriverTest.WritesRealCxxLakeAndCsvSidecarForPythonReadCrossCheck

    # 3. Run this file with the SAME path -- it pytest.skip()s (not fails)
    #    when the directory (or its expected_navs.csv) is absent:
    python -m pytest python/tests/test_tracks_cxx_fixture.py -v
"""

from __future__ import annotations

import csv
import datetime as dt
import os
from pathlib import Path

import pytest

from atxpy import tracks

_ENV_VAR = "ATX_VOL_D5_FIXTURE_LAKE"


def _fixture_lake() -> Path | None:
    """The fixture lake directory named by `_ENV_VAR`, or None if the env var
    is unset or the gtest producer has not (yet) written a usable lake
    there. Never raises -- an absent/partial fixture is a skip, not an
    error, since this file's whole point is to be safely omittable from a
    normal test run."""
    raw = os.environ.get(_ENV_VAR)
    if not raw:
        return None
    lake = Path(raw)
    if not lake.is_dir() or not (lake / "expected_navs.csv").exists():
        return None
    return lake


def _skip_if_absent() -> Path:
    lake = _fixture_lake()
    if lake is None:
        pytest.skip(
            f"set {_ENV_VAR} to a directory produced by "
            "SweepDriverTest.WritesRealCxxLakeAndCsvSidecarForPythonReadCrossCheck "
            "(atx-vol/tests/sweep_driver_test.cpp) to run this cross-check; "
            "skipped otherwise -- see this file's own doc comment for the exact commands."
        )
    return lake


def _load_expected(sidecar: Path) -> dict[tuple[str, dt.date], float]:
    with sidecar.open(newline="") as f:
        rows = list(csv.DictReader(f))
    assert rows, f"{sidecar} side-car was empty"
    lookup = {(r["track_key"], dt.date.fromisoformat(r["date"])): float(r["nav"]) for r in rows}
    assert len(lookup) == len(rows), "duplicate (track_key, date) in the side-car"
    return lookup


def test_cxx_written_lake_navs_match_the_writers_own_recorded_values():
    lake = _skip_if_absent()
    expected = _load_expected(lake / "expected_navs.csv")

    table = tracks.load(lake, columns=["track_key", "date", "nav"])
    df = table.to_pandas()
    assert len(df) == len(expected)

    for _, row in df.iterrows():
        key = (row["track_key"], row["date"])
        assert key in expected, f"reloaded row {key} not in the C++ process's own recorded values"
        # Exact equality, not approx: schema v1's nav column round-trips a
        # float64 through Feather -> Parquet -> DuckDB losslessly, and the
        # side-car was written with 17 significant digits (round-trip-safe
        # for IEEE754 binary64) -- "matches to the double", not merely close.
        assert row["nav"] == expected[key], f"{key}: reloaded {row['nav']!r} != recorded {expected[key]!r}"


def test_cxx_written_catalog_shows_every_track_compacted():
    lake = _skip_if_absent()
    expected = _load_expected(lake / "expected_navs.csv")
    expected_track_keys = {k for k, _ in expected}

    df = tracks.catalog(lake)
    assert set(df["track_key"]) == expected_track_keys
    assert (df["status"] == "compacted").all(), (
        "every track the gtest registered should have been mark_compacted()'d "
        "by the sweep-driver test's D5 compact()->Catalog::mark_compacted glue "
        "before this pytest runs"
    )
    assert df["file"].notna().all()
    assert df["row_group"].notna().all()


def test_cxx_written_returns_matrix_pivots_the_same_sweep():
    lake = _skip_if_absent()
    expected = _load_expected(lake / "expected_navs.csv")
    expected_track_keys = {k for k, _ in expected}

    # sweep_id is fixed by the gtest ("sweep-d5-fixture") -- see
    # SweepDriverTest.WritesRealCxxLakeAndCsvSidecarForPythonReadCrossCheck.
    wide = tracks.returns_matrix(lake, "sweep-d5-fixture")
    assert not wide.empty
    assert set(wide.columns) == expected_track_keys
    assert not wide.isna().values.any()
