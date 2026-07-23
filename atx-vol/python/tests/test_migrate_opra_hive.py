"""Tests for ``tools/migrate_opra_hive.py`` — OPRA hive v1 (per-symbol
``{underlying}/{date}.parquet``) -> v2 (date-partitioned
``date=YYYY-MM-DD/data.parquet``) migration.

Fixtures write REAL parquet trees into ``tmp_path`` (pyarrow), never mocks, so
every assertion exercises the actual read/merge/sort/atomic-write path. The tool
is a standalone script under ``atx-vol/tools/`` (no compiled extension), imported
here via ``importlib`` the same way ``test_runarchive.py`` loads its generator.
"""

from __future__ import annotations

import csv
import importlib.util
import pathlib
import sys

import numpy as np
import pyarrow as pa
import pyarrow.parquet as pq
import pytest

# ── load the standalone tool module ──────────────────────────────────────────
_TOOL = pathlib.Path(__file__).resolve().parents[2] / "tools" / "migrate_opra_hive.py"
_spec = importlib.util.spec_from_file_location("migrate_opra_hive", str(_TOOL))
mig = importlib.util.module_from_spec(_spec)
sys.modules["migrate_opra_hive"] = mig
_spec.loader.exec_module(mig)

INT64_MIN = np.iinfo(np.int64).min
TS = 1783108500000000000  # fixed 19:55:00Z-ish ns-since-epoch snapshot stamp


# ── fixture helpers ──────────────────────────────────────────────────────────
def _v1_table(underlying: str, symbols: list[str]) -> pa.Table:
    n = len(symbols)
    return pa.Table.from_pydict(
        {
            "ts": pa.array([TS] * n, pa.timestamp("ns")),
            "underlying": pa.array([underlying] * n, pa.string()),
            "symbol": pa.array(symbols, pa.string()),
            "instrument_id": pa.array(list(range(1, n + 1)), pa.int64()),
            # one unset side (INT64_MIN) to prove sentinel fidelity round-trips
            "bid_px": pa.array([INT64_MIN] + [1_000_000_000] * (n - 1), pa.int64()),
            "ask_px": pa.array([1_010_000_000] * n, pa.int64()),
            "bid_sz": pa.array([5] * n, pa.int64()),
            "ask_sz": pa.array([7] * n, pa.int64()),
        },
        schema=mig.CANONICAL_SCHEMA,
    )


def _write_v1(root: pathlib.Path, underlying: str, date: str, symbols: list[str]) -> None:
    """Write an old-layout per-symbol file ``root/<underlying>/<date>.parquet``."""
    d = root / underlying
    d.mkdir(parents=True, exist_ok=True)
    pq.write_table(_v1_table(underlying, symbols), d / f"{date}.parquet")


def _dst_file(dst: pathlib.Path, date: str) -> pathlib.Path:
    return dst / f"date={date}" / "data.parquet"


D1, D2, D3 = "2026-07-01", "2026-07-02", "2026-07-03"


# ── core migration ───────────────────────────────────────────────────────────
def test_basic_migration_layout_counts_schema_sort(tmp_path):
    src, dst = tmp_path / "v1", tmp_path / "v2"
    # SPY option symbols deliberately UNSORTED to exercise the sort.
    _write_v1(src, "SPY", D1, ["SPY_C3", "SPY_C1", "SPY_C2"])
    _write_v1(src, "QQQ", D1, ["QQQ_C2", "QQQ_C1"])
    _write_v1(src, "SPY", D2, ["SPY_C1"])
    _write_v1(src, "QQQ", D2, ["QQQ_C1"])

    stats = mig.migrate(src, dst)

    assert stats.n_dates == 2
    assert stats.n_written == 2
    assert stats.n_skipped == 0

    for date, expected_rows in [(D1, 5), (D2, 2)]:
        f = _dst_file(dst, date)
        assert f.exists(), f"missing {f}"
        pf = pq.ParquetFile(f)  # raw file read — no hive partition inference
        # canonical 8-column schema stored in the file; the ``date`` is a hive
        # path key, NOT a baked-in column (spec §3).
        assert pf.schema_arrow.equals(mig.CANONICAL_SCHEMA, check_metadata=False)
        assert "date" not in pf.schema_arrow.names
        t = pf.read()
        # row count per date == sum of source rows
        assert t.num_rows == expected_rows
        # rows sorted by (underlying, symbol)
        unds = t.column("underlying").to_pylist()
        syms = t.column("symbol").to_pylist()
        assert unds == sorted(unds)
        assert list(zip(unds, syms)) == sorted(zip(unds, syms))

    # sentinel INT64_MIN survived the round-trip
    t1 = pq.ParquetFile(_dst_file(dst, D1)).read()
    assert INT64_MIN in t1.column("bid_px").to_pylist()


def test_idempotent_rerun_skips_and_does_not_rewrite(tmp_path):
    src, dst = tmp_path / "v1", tmp_path / "v2"
    _write_v1(src, "SPY", D1, ["SPY_C1", "SPY_C2"])
    _write_v1(src, "QQQ", D1, ["QQQ_C1"])
    _write_v1(src, "SPY", D2, ["SPY_C1"])
    _write_v1(src, "QQQ", D2, ["QQQ_C1"])

    first = mig.migrate(src, dst)
    assert first.n_written == 2

    before = {d: _dst_file(dst, d).read_bytes() for d in (D1, D2)}

    second = mig.migrate(src, dst)
    assert second.n_skipped == 2
    assert second.n_written == 0

    # byte-identical: skipped dates are never rewritten
    for d in (D1, D2):
        assert _dst_file(dst, d).read_bytes() == before[d]


def test_from_to_filter(tmp_path):
    src, dst = tmp_path / "v1", tmp_path / "v2"
    for d in (D1, D2, D3):
        _write_v1(src, "SPY", d, ["SPY_C1"])

    stats = mig.migrate(src, dst, date_lo=D2, date_hi=D2)

    assert stats.n_written == 1
    assert _dst_file(dst, D2).exists()
    assert not _dst_file(dst, D1).exists()
    assert not _dst_file(dst, D3).exists()


def test_superset_dst_never_rewritten(tmp_path):
    src, dst = tmp_path / "v1", tmp_path / "v2"
    _write_v1(src, "SPY", D1, ["SPY_C1"])
    _write_v1(src, "QQQ", D1, ["QQQ_C1"])

    # Pre-create a dst date file whose distinct-underlying set is a strict
    # SUPERSET of the source ({SPY, QQQ, IWM} >= {SPY, QQQ}). Written via the
    # tool's own atomic writer so it has one row group per underlying (footer
    # stats give the exact distinct set).
    superset = pa.concat_tables(
        [
            _v1_table("IWM", ["IWM_C1"]),
            _v1_table("QQQ", ["QQQ_C1", "QQQ_C2"]),
            _v1_table("SPY", ["SPY_C1"]),
        ]
    ).cast(mig.CANONICAL_SCHEMA)
    dst_f = _dst_file(dst, D1)
    mig._merge_date  # exists
    mig._write_atomic(superset, dst_f)
    before = dst_f.read_bytes()

    stats = mig.migrate(src, dst)

    assert stats.n_skipped == 1
    assert stats.n_written == 0
    assert dst_f.read_bytes() == before  # untouched
    # superset content still there — not clobbered with just {SPY, QQQ}
    assert set(pq.read_table(dst_f).column("underlying").to_pylist()) == {"IWM", "QQQ", "SPY"}


def test_partial_source_tolerance(tmp_path):
    src, dst = tmp_path / "v1", tmp_path / "v2"
    # SPY present both dates; QQQ only present for D1 (missing D2).
    _write_v1(src, "SPY", D1, ["SPY_C1"])
    _write_v1(src, "QQQ", D1, ["QQQ_C1"])
    _write_v1(src, "SPY", D2, ["SPY_C1"])

    stats = mig.migrate(src, dst)  # must not raise

    assert stats.n_written == 2
    assert set(pq.read_table(_dst_file(dst, D1)).column("underlying").to_pylist()) == {"QQQ", "SPY"}
    # D2 simply lacks QQQ — no error, just the symbols that exist.
    assert set(pq.read_table(_dst_file(dst, D2)).column("underlying").to_pylist()) == {"SPY"}


def test_dry_run_writes_nothing(tmp_path):
    src, dst = tmp_path / "v1", tmp_path / "v2"
    _write_v1(src, "SPY", D1, ["SPY_C1"])
    _write_v1(src, "QQQ", D1, ["QQQ_C1"])

    stats = mig.migrate(src, dst, dry_run=True)

    assert not dst.exists() or list(dst.glob("date=*")) == []
    assert list(dst.glob("migration_manifest_*.csv")) == [] if dst.exists() else True
    assert stats.manifest_path is None
    assert stats.n_written == 0
    assert [r.status for r in stats.results] == ["planned"]
    assert stats.results[0].n_rows == 2  # planned row count read from source footers


def test_schema_drift_is_hard_error(tmp_path):
    src, dst = tmp_path / "v1", tmp_path / "v2"
    _write_v1(src, "SPY", D1, ["SPY_C1"])
    # A drifted file: bid_px stored as int32 instead of the canonical int64.
    bad = pa.table(
        {
            "ts": pa.array([TS], pa.timestamp("ns")),
            "underlying": pa.array(["QQQ"], pa.string()),
            "symbol": pa.array(["QQQ_C1"], pa.string()),
            "instrument_id": pa.array([1], pa.int64()),
            "bid_px": pa.array([1], pa.int32()),  # <- drift
            "ask_px": pa.array([1], pa.int64()),
            "bid_sz": pa.array([1], pa.int64()),
            "ask_sz": pa.array([1], pa.int64()),
        }
    )
    (src / "QQQ").mkdir(parents=True, exist_ok=True)
    pq.write_table(bad, src / "QQQ" / f"{D1}.parquet")

    with pytest.raises(Exception) as exc:
        mig.migrate(src, dst)
    assert "schema" in str(exc.value).lower()


def test_manifest_csv_written(tmp_path):
    src, dst = tmp_path / "v1", tmp_path / "v2"
    _write_v1(src, "SPY", D1, ["SPY_C1"])
    _write_v1(src, "QQQ", D1, ["QQQ_C1"])
    _write_v1(src, "SPY", D2, ["SPY_C1"])

    stats = mig.migrate(src, dst)

    manifests = list(dst.glob("migration_manifest_*.csv"))
    assert len(manifests) == 1
    assert stats.manifest_path == str(manifests[0])
    with open(manifests[0], newline="") as fh:
        rows = list(csv.DictReader(fh))
    assert [r["date"] for r in rows] == [D1, D2]
    assert {r["status"] for r in rows} == {"written"}
    # D1 merged two source files
    d1 = next(r for r in rows if r["date"] == D1)
    assert int(d1["n_source_files"]) == 2
    assert int(d1["n_rows"]) == 2


def test_merge_date_direct(tmp_path):
    """The per-date primitive reads several per-symbol files, validates, merges,
    sorts and writes one canonical file."""
    src, dst = tmp_path / "v1", tmp_path / "v2"
    _write_v1(src, "SPY", D1, ["SPY_C2", "SPY_C1"])
    _write_v1(src, "QQQ", D1, ["QQQ_C1"])
    src_files = [src / "SPY" / f"{D1}.parquet", src / "QQQ" / f"{D1}.parquet"]
    out = _dst_file(dst, D1)

    n_rows = mig._merge_date(src_files, out)

    assert n_rows == 3
    pf = pq.ParquetFile(out)
    assert pf.schema_arrow.equals(mig.CANONICAL_SCHEMA, check_metadata=False)
    t = pf.read()
    pairs = list(zip(t.column("underlying").to_pylist(), t.column("symbol").to_pylist()))
    assert pairs == sorted(pairs)
