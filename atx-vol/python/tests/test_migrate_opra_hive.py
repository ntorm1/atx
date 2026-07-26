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
import pyarrow.compute as pc
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


def _rows_for(table: pa.Table, underlying: str) -> set:
    """All rows for one ``underlying`` as a set of value-tuples, so equality is
    independent of row order (a global re-sort is expected) but still catches
    any change to a single cell -- true byte-for-byte content survival."""
    t = table.filter(pc.equal(table.column("underlying"), underlying))
    cols = t.column_names
    return {tuple(t.column(c)[i].as_py() for c in cols) for i in range(t.num_rows)}


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


# ── IMP-3 regression: footer min/max union under-reports a multi-underlying
# row group, which used to make migrate() rebuild (and destroy) the
# destination from the v1 sources alone ────────────────────────────────────
def test_multi_underlying_row_group_dest_not_destructively_rewritten(tmp_path):
    """A destination file with a SINGLE row group spanning {AAA, MMM, ZZZ} has
    footer stats min=AAA, max=ZZZ -- MMM sorts strictly between them and is
    invisible to a naive min/max union, so the old code's derived destination
    set was {AAA, ZZZ}, silently missing MMM.

    The v1 source tree for this date holds {AAA, MMM} (MMM exists in both
    places; ZZZ is paid-only -- added by a Databento pull and never present in
    v1). Because MMM is invisible to the old derived set, the old superset
    check {AAA, ZZZ} >= {AAA, MMM} spuriously evaluates False (MMM looks
    "missing" even though the true destination has it), so the old code falls
    through to ``_merge_date``, which rebuilds the date file from the v1
    sources ALONE -- silently destroying ZZZ, which has no source to rebuild
    from. (Using MMM itself as the only "missing" source symbol would not
    reproduce the destructive path: since MMM is also in v1, the naive rebuild
    would still contain it, masking the bug. ZZZ -- present only in the true
    destination set and never in any derived/rederived set -- is what actually
    gets lost, and that is the concrete harm this test guards against.)

    Confirmed failing against the pre-fix ``_footer_underlyings``: pre-fix,
    this scenario produces ``status == "written"`` and ZZZ is gone from the
    destination afterward (verified by running this exact construction
    against the unpatched function before the fix landed)."""
    src, dst = tmp_path / "v1", tmp_path / "v2"
    _write_v1(src, "AAA", D1, ["AAA_C1"])
    _write_v1(src, "MMM", D1, ["MMM_C1"])
    # ZZZ deliberately absent from v1 -- paid-only, must never be rebuilt away.

    whole = pa.concat_tables(
        [
            _v1_table("AAA", ["AAA_C1"]),
            _v1_table("MMM", ["MMM_C1"]),
            _v1_table("ZZZ", ["ZZZ_C1"]),
        ]
    ).cast(mig.CANONICAL_SCHEMA)
    dst_f = _dst_file(dst, D1)
    dst_f.parent.mkdir(parents=True, exist_ok=True)
    # ONE write_table call -> ONE row group (do not rely on default row-group
    # sizing, which could accidentally split per-underlying and hide the bug).
    with pq.ParquetWriter(dst_f, whole.schema) as writer:
        writer.write_table(whole)

    pf = pq.ParquetFile(dst_f)
    assert pf.metadata.num_row_groups == 1, "fixture must produce a single row group"
    ci = pf.schema_arrow.names.index("underlying")
    st = pf.metadata.row_group(0).column(ci).statistics
    assert st.has_min_max and st.min == "AAA" and st.max == "ZZZ"  # MMM invisible to min/max
    del pf, st  # release the open file handle -- Windows os.replace() inside
    # migrate() would otherwise fail with PermissionError, not the assertion
    # this test actually means to exercise

    before = dst_f.read_bytes()
    stats = mig.migrate(src, dst)

    assert stats.n_skipped == 1
    assert stats.n_written == 0
    assert stats.results[0].status == "skipped"
    assert dst_f.read_bytes() == before  # destination bytes untouched
    # the critical assertion: ZZZ (paid-for, not in v1) must still be there
    assert set(pq.read_table(dst_f).column("underlying").to_pylist()) == {"AAA", "MMM", "ZZZ"}


def test_footer_underlyings_returns_none_for_unreadable_file(tmp_path):
    """The ``None`` ("cannot prove superset" -> force rewrite) contract must
    survive the fix for files that cannot be opened at all."""
    assert mig._footer_underlyings(tmp_path / "does_not_exist.parquet") is None


def test_footer_underlyings_no_stats_falls_back_to_full_read(tmp_path):
    """A row group written with column statistics disabled must not be
    reported as an unrecoverable ``None`` (the pre-fix function's behaviour,
    which forced an always-rewrite for such files) -- it must fall back to a
    full read of the ``underlying`` column and return the exact set."""
    path = tmp_path / "data.parquet"
    table = pa.concat_tables(
        [_v1_table("AAA", ["AAA_C1"]), _v1_table("ZZZ", ["ZZZ_C1"])]
    ).cast(mig.CANONICAL_SCHEMA)
    pq.write_table(table, path, write_statistics=False)

    pf = pq.ParquetFile(path)
    ci = pf.schema_arrow.names.index("underlying")
    st = pf.metadata.row_group(0).column(ci).statistics
    assert st is None or not st.has_min_max  # precondition: no usable stats

    assert mig._footer_underlyings(path) == {"AAA", "ZZZ"}


# ── R2-a regression: partial-overlap union, not a destructive rebuild ──────────
def test_partial_overlap_union_preserves_destination_only_underlying(tmp_path):
    """The bug the reviewer found (C-01): destination holds an underlying the
    v1 source does NOT have (ZZZ -- e.g. paid Databento data added after
    migration), and the v1 source separately holds an underlying the
    destination lacks (MMM). The old code's superset check ({AAA} !>=
    {AAA, MMM}) fails, so it fell through to ``_merge_date``, which rebuilds
    the date file from the v1 source ALONE and clobbers the destination --
    ZZZ vanishes. The fix must produce the UNION {AAA, MMM, ZZZ}, and ZZZ's
    original rows must survive BYTE-FOR-BYTE (not merely "the underlying
    name is still present somewhere") -- proving nothing rebuilt ZZZ from
    scratch."""
    src, dst = tmp_path / "v1", tmp_path / "v2"
    _write_v1(src, "AAA", D1, ["AAA_C1"])
    _write_v1(src, "MMM", D1, ["MMM_C1"])
    # ZZZ deliberately absent from v1 -- paid-only, must never be rebuilt away.

    dst_f = _dst_file(dst, D1)
    dest_before = pa.concat_tables(
        [_v1_table("AAA", ["AAA_C1"]), _v1_table("ZZZ", ["ZZZ_C1", "ZZZ_C2"])]
    ).cast(mig.CANONICAL_SCHEMA)
    mig._write_atomic(dest_before, dst_f)
    zzz_rows_before = _rows_for(pq.read_table(dst_f), "ZZZ")

    stats = mig.migrate(src, dst)

    assert stats.n_merged == 1
    assert stats.n_written == 0
    assert stats.n_errors == 0
    assert stats.results[0].status == "merged"

    result = pq.read_table(dst_f)
    assert set(result.column("underlying").to_pylist()) == {"AAA", "MMM", "ZZZ"}
    assert _rows_for(result, "ZZZ") == zzz_rows_before  # byte-for-byte survival


def test_overlap_precedence_destination_wins_on_conflicting_content(tmp_path):
    """When the SAME underlying exists in both, with genuinely different row
    content, the destination's rows must win -- it may hold newer, paid data;
    the v1 source is by definition the older tree."""
    src, dst = tmp_path / "v1", tmp_path / "v2"
    # AAA in both places but with DIFFERENT symbols, so a pass here can only
    # mean "destination's content was kept", not a coincidence.
    _write_v1(src, "AAA", D1, ["AAA_SRC_ONLY"])
    _write_v1(src, "MMM", D1, ["MMM_C1"])  # forces the union path (not a superset)

    dst_f = _dst_file(dst, D1)
    dest_before = pa.concat_tables(
        [_v1_table("AAA", ["AAA_DST_ONLY"]), _v1_table("ZZZ", ["ZZZ_C1"])]
    ).cast(mig.CANONICAL_SCHEMA)
    mig._write_atomic(dest_before, dst_f)

    stats = mig.migrate(src, dst)

    assert stats.results[0].status == "merged"
    result = pq.read_table(dst_f)
    assert set(result.column("underlying").to_pylist()) == {"AAA", "MMM", "ZZZ"}
    aaa_syms = result.filter(pc.equal(result.column("underlying"), "AAA")) \
        .column("symbol").to_pylist()
    assert aaa_syms == ["AAA_DST_ONLY"]  # destination's row kept, source's dropped


def test_unreadable_destination_is_refused_not_rewritten(tmp_path):
    """'I cannot read the destination' must never be treated as license to
    overwrite it -- that is the case where overwriting is least defensible."""
    src, dst = tmp_path / "v1", tmp_path / "v2"
    _write_v1(src, "AAA", D1, ["AAA_C1"])

    dst_f = _dst_file(dst, D1)
    dst_f.parent.mkdir(parents=True, exist_ok=True)
    dst_f.write_bytes(b"not a parquet file at all")
    before = dst_f.read_bytes()

    stats = mig.migrate(src, dst)

    assert stats.n_errors == 1
    assert stats.n_written == 0
    assert stats.n_merged == 0
    assert stats.results[0].status == "error"
    assert str(dst_f) in stats.results[0].detail
    assert dst_f.read_bytes() == before  # byte-for-byte untouched


def test_destination_schema_drift_is_error_not_overwrite(tmp_path):
    """Schema drift confined to a non-``underlying`` column is invisible to the
    cheap footer-only scan (which only inspects ``underlying``) and is only
    caught once the union merge fully validates the destination -- and when it
    is caught, the date must be refused, not silently overwritten."""
    src, dst = tmp_path / "v1", tmp_path / "v2"
    _write_v1(src, "AAA", D1, ["AAA_C1"])
    _write_v1(src, "MMM", D1, ["MMM_C1"])  # forces the union path (not a superset)

    dst_f = _dst_file(dst, D1)
    dst_f.parent.mkdir(parents=True, exist_ok=True)
    bad = pa.table(
        {
            "ts": pa.array([TS], pa.timestamp("ns")),
            "underlying": pa.array(["AAA"], pa.string()),
            "symbol": pa.array(["AAA_C1"], pa.string()),
            "instrument_id": pa.array([1], pa.int64()),
            "bid_px": pa.array([1], pa.int32()),  # <- drift (canonical is int64)
            "ask_px": pa.array([1], pa.int64()),
            "bid_sz": pa.array([1], pa.int64()),
            "ask_sz": pa.array([1], pa.int64()),
        }
    )
    pq.write_table(bad, dst_f)
    before = dst_f.read_bytes()

    stats = mig.migrate(src, dst)

    assert stats.n_errors == 1
    assert stats.results[0].status == "error"
    assert "schema" in stats.results[0].detail.lower()
    assert dst_f.read_bytes() == before


def test_dry_run_detects_schema_drift_in_source_add_file(tmp_path):
    """``--dry-run``'s union-merge branch validated only the DESTINATION's
    schema before reporting ``planned_merge``, never the schemas of the
    source ``add_files`` it would fold in. So a date whose v1 source has
    drifted schema was reported as a clean would-be merge, even though the
    real run's ``_merge_union_date`` validates every add_file too (see
    ``test_destination_schema_drift_is_error_not_overwrite`` for the
    equivalent destination-side case) and would actually raise and refuse the
    date. A dry run exists to predict the real run -- it must classify this
    date the same way the real run does: ``error``, never ``planned_merge``."""
    src, dst = tmp_path / "v1", tmp_path / "v2"
    _write_v1(src, "AAA", D1, ["AAA_C1"])  # valid v1 source, already in dst
    # MMM is the v1 source the union would ADD (destination lacks it) -- give
    # it drifted schema (bid_px as int32 instead of the canonical int64).
    bad = pa.table(
        {
            "ts": pa.array([TS], pa.timestamp("ns")),
            "underlying": pa.array(["MMM"], pa.string()),
            "symbol": pa.array(["MMM_C1"], pa.string()),
            "instrument_id": pa.array([1], pa.int64()),
            "bid_px": pa.array([1], pa.int32()),  # <- drift
            "ask_px": pa.array([1], pa.int64()),
            "bid_sz": pa.array([1], pa.int64()),
            "ask_sz": pa.array([1], pa.int64()),
        }
    )
    (src / "MMM").mkdir(parents=True, exist_ok=True)
    pq.write_table(bad, src / "MMM" / f"{D1}.parquet")

    dst_f = _dst_file(dst, D1)
    # Destination has AAA only (valid schema) -- a partial overlap with
    # {AAA, MMM} forces the union path; MMM is the add_file the destination
    # lacks and is the one with drifted schema.
    mig._write_atomic(_v1_table("AAA", ["AAA_C1"]), dst_f)
    before = dst_f.read_bytes()

    stats = mig.migrate(src, dst, dry_run=True)

    assert stats.results[0].status == "error"  # NOT planned_merge
    assert "schema" in stats.results[0].detail.lower()
    assert stats.manifest_path is None
    assert dst_f.read_bytes() == before  # dry-run never writes

    # The real run must actually refuse this date too, confirming the
    # dry-run's prediction matches reality (not just a stricter dry-run).
    stats_real = mig.migrate(src, dst)
    assert stats_real.results[0].status == "error"
    assert dst_f.read_bytes() == before  # still untouched


def test_dry_run_classifies_without_writing_all_new_shapes(tmp_path):
    """``--dry-run`` must report the same classification as a real run for
    every new outcome, and must never write in any of them."""
    # Shape: partial overlap -> would "merge".
    src1, dst1 = tmp_path / "v1a", tmp_path / "v2a"
    _write_v1(src1, "AAA", D1, ["AAA_C1"])
    _write_v1(src1, "MMM", D1, ["MMM_C1"])
    dst_f1 = _dst_file(dst1, D1)
    mig._write_atomic(
        pa.concat_tables(
            [_v1_table("AAA", ["AAA_C1"]), _v1_table("ZZZ", ["ZZZ_C1"])]
        ).cast(mig.CANONICAL_SCHEMA),
        dst_f1,
    )
    before1 = dst_f1.read_bytes()
    stats1 = mig.migrate(src1, dst1, dry_run=True)
    assert stats1.results[0].status == "planned_merge"
    assert stats1.manifest_path is None
    assert dst_f1.read_bytes() == before1

    # Shape: unreadable destination -> would "error".
    src2, dst2 = tmp_path / "v1b", tmp_path / "v2b"
    _write_v1(src2, "AAA", D1, ["AAA_C1"])
    dst_f2 = _dst_file(dst2, D1)
    dst_f2.parent.mkdir(parents=True, exist_ok=True)
    dst_f2.write_bytes(b"garbage, not parquet")
    before2 = dst_f2.read_bytes()
    stats2 = mig.migrate(src2, dst2, dry_run=True)
    assert stats2.results[0].status == "error"
    assert dst_f2.read_bytes() == before2

    # Shape: destination schema drift -> would "error", not a plan to merge.
    src3, dst3 = tmp_path / "v1c", tmp_path / "v2c"
    _write_v1(src3, "AAA", D1, ["AAA_C1"])
    _write_v1(src3, "MMM", D1, ["MMM_C1"])
    dst_f3 = _dst_file(dst3, D1)
    dst_f3.parent.mkdir(parents=True, exist_ok=True)
    bad = pa.table(
        {
            "ts": pa.array([TS], pa.timestamp("ns")),
            "underlying": pa.array(["AAA"], pa.string()),
            "symbol": pa.array(["AAA_C1"], pa.string()),
            "instrument_id": pa.array([1], pa.int64()),
            "bid_px": pa.array([1], pa.int32()),  # <- drift
            "ask_px": pa.array([1], pa.int64()),
            "bid_sz": pa.array([1], pa.int64()),
            "ask_sz": pa.array([1], pa.int64()),
        }
    )
    pq.write_table(bad, dst_f3)
    before3 = dst_f3.read_bytes()
    stats3 = mig.migrate(src3, dst3, dry_run=True)
    assert stats3.results[0].status == "error"
    assert dst_f3.read_bytes() == before3

    # Shape: strict superset -> would still "skip" (unchanged behavior).
    src4, dst4 = tmp_path / "v1d", tmp_path / "v2d"
    _write_v1(src4, "AAA", D1, ["AAA_C1"])
    dst_f4 = _dst_file(dst4, D1)
    mig._write_atomic(
        pa.concat_tables(
            [_v1_table("AAA", ["AAA_C1"]), _v1_table("ZZZ", ["ZZZ_C1"])]
        ).cast(mig.CANONICAL_SCHEMA),
        dst_f4,
    )
    before4 = dst_f4.read_bytes()
    stats4 = mig.migrate(src4, dst4, dry_run=True)
    assert stats4.results[0].status == "skipped"
    assert dst_f4.read_bytes() == before4
