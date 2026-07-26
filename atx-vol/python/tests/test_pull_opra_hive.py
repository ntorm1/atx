"""Logic tests for tools/pull_opra_hive.py — the v2 (date-partitioned) OPRA pull.

No network, no Databento spend, no compiled extension. A ``FakeHistorical``
stands in for ``databento.Historical``: ``metadata.get_cost`` returns a fixed
per-symbol unit and ``timeseries.get_range`` returns a ``FakeStore`` whose
``.to_df()`` yields a canned cbbo-1m frame. The tool guards its ``import
databento`` so it imports fine here and the fake is injected into the core
functions (``plan_missing`` / ``preflight`` / ``pull`` / ``merge_date_file``).

Cases (per the task brief):
  * ``plan_missing`` on an empty / partial / complete hive root.
  * preflight math: estimate == unit x n_missing_cells; over-cap degrade keeps
    the index leg + top-N by weight, and BLOCKs (exit 3) below the floor — pinned
    with exact numbers.
  * ``merge_date_file``: union of {A}+{B}, sorted, atomic tmp; ``--force``
    rewrites requested symbols while preserving untouched ones.
  * dry-run records zero ``get_range`` calls.
  * resume: a DBN cache hit re-writes boards with zero ``get_range`` calls.
"""

from __future__ import annotations

import importlib.util
import json
import pathlib
import sys
from unittest import mock

import pandas as pd
import pyarrow as pa
import pyarrow.parquet as pq
import pytest

# tools/ is not an importable package — load the script by path.
_TOOL = pathlib.Path(__file__).resolve().parents[2] / "tools" / "pull_opra_hive.py"
_spec = importlib.util.spec_from_file_location("pull_opra_hive", _TOOL)
ph = importlib.util.module_from_spec(_spec)
sys.modules["pull_opra_hive"] = ph
_spec.loader.exec_module(ph)


# ── Fakes ─────────────────────────────────────────────────────────────────────

def _osi(underlying: str, strike: int = 150) -> str:
    """OSI-style symbol: 6-char space-padded root + expiry/right/strike."""
    root = underlying.replace(".", "")
    return f"{root:<6}260717C{strike * 1000:08d}"


def _raw_df(underlyings, *, bid=1_000_000_000, ask=1_010_000_000, bsz=5, asz=6):
    """A raw databento cbbo-1m ``to_df`` frame: ts index + *_00 price columns."""
    syms = [_osi(u) for u in underlyings]
    n = len(syms)
    idx = pd.to_datetime(["2026-07-20T19:55:00"] * n)
    return pd.DataFrame(
        {
            "symbol": syms,
            "instrument_id": list(range(1, n + 1)),
            "bid_px_00": [bid] * n,
            "ask_px_00": [ask] * n,
            "bid_sz_00": [bsz] * n,
            "ask_sz_00": [asz] * n,
        },
        index=idx,
    )


class FakeStore:
    """Stand-in for ``databento.DBNStore``."""

    def __init__(self, df: pd.DataFrame):
        self._df = df

    def to_df(self, **_kw) -> pd.DataFrame:
        return self._df.copy()

    def to_file(self, path) -> None:  # cache write — persist so a resume can reload
        self._df.to_parquet(path)


def _fake_loader(path) -> FakeStore:
    return FakeStore(pd.read_parquet(path))


class _FakeMeta:
    def __init__(self, unit: float, rec: list):
        self._unit = unit
        self._rec = rec

    def get_cost(self, *, dataset, symbols, schema, start, end, stype_in) -> float:
        self._rec.append(("get_cost", tuple(symbols), start))
        return self._unit * len(symbols)


class _FakeTS:
    def __init__(self, frames: dict, rec: list, raise_on_range: bool):
        self._frames = frames
        self._rec = rec
        self._raise = raise_on_range

    def get_range(self, *, dataset, symbols, schema, start, end, stype_in) -> FakeStore:
        self._rec.append(("get_range", tuple(symbols), start))
        if self._raise:
            raise AssertionError("get_range must not be called on a cache hit")
        return FakeStore(self._frames[start[:10]])


class FakeHistorical:
    def __init__(self, *, unit=0.001, frames=None, raise_on_range=False):
        self.calls: list = []
        self.metadata = _FakeMeta(unit, self.calls)
        self.timeseries = _FakeTS(frames or {}, self.calls, raise_on_range)

    def get_range_calls(self) -> list:
        return [c for c in self.calls if c[0] == "get_range"]


def _decoded(underlyings, *, bid_px=100, ts="2026-07-20T19:55:00") -> pd.DataFrame:
    """A decoded 8-column frame (post-DBN) for merge/plan helpers.

    ``underlyings`` may be an iterable of names or a ``{name: bid_px}`` map.
    ``ts`` is the constant snapshot stamp the whole date file is written with
    (default matches the fixed 19:55Z minute used throughout this file's other
    fixtures); override it to simulate a file written at a different minute."""
    if isinstance(underlyings, dict):
        items = list(underlyings.items())
    else:
        items = [(u, bid_px) for u in underlyings]
    rows = []
    for u, bp in items:
        rows.append(
            {
                "ts": pd.Timestamp(ts),
                "underlying": u,
                "symbol": _osi(u),
                "instrument_id": 1,
                "bid_px": bp,
                "ask_px": bp + 10,
                "bid_sz": 5,
                "ask_sz": 6,
            }
        )
    return pd.DataFrame(rows)


# ── plan_missing ──────────────────────────────────────────────────────────────

def test_plan_missing_empty_root_is_all_cells(tmp_path):
    dates = ["2026-07-20", "2026-07-21"]
    plan = ph.plan_missing(tmp_path, ["SPY", "AAPL"], dates)
    assert plan == {
        "2026-07-20": ["SPY", "AAPL"],
        "2026-07-21": ["SPY", "AAPL"],
    }


def test_plan_missing_partial_file_leaves_only_missing_symbol(tmp_path):
    tgt = tmp_path / "date=2026-07-20" / ph.DATE_FILE
    ph.merge_date_file(None, _decoded(["SPY"]), tgt)
    plan = ph.plan_missing(tmp_path, ["SPY", "AAPL"], ["2026-07-20"])
    assert plan == {"2026-07-20": ["AAPL"]}


def test_plan_missing_complete_file_drops_the_date(tmp_path):
    tgt = tmp_path / "date=2026-07-20" / ph.DATE_FILE
    ph.merge_date_file(None, _decoded(["SPY", "AAPL"]), tgt)
    plan = ph.plan_missing(tmp_path, ["SPY", "AAPL"], ["2026-07-20"])
    assert plan == {}


def test_plan_missing_force_repulls_every_requested_symbol(tmp_path):
    tgt = tmp_path / "date=2026-07-20" / ph.DATE_FILE
    ph.merge_date_file(None, _decoded(["SPY", "AAPL"]), tgt)
    plan = ph.plan_missing(tmp_path, ["SPY", "AAPL"], ["2026-07-20"], force=True)
    assert plan == {"2026-07-20": ["SPY", "AAPL"]}


def test_plan_missing_handles_multi_underlying_row_group(tmp_path):
    """A generic writer may pack several underlyings into ONE sorted row group
    (the one-rg-per-underlying layout is not frozen). Footer min/max would then
    be only {first, last} and drop the middle names — plan_missing must fall
    back to the full underlying column and see them all present, not re-pull."""
    tgt = tmp_path / "date=2026-07-20" / ph.DATE_FILE
    tgt.parent.mkdir(parents=True, exist_ok=True)
    frame = _decoded(["AAPL", "GOOG", "MSFT"]).sort_values("underlying")
    tbl = pa.Table.from_pandas(frame[ph.COLUMNS], schema=ph.ARROW_SCHEMA,
                               preserve_index=False)
    pq.write_table(tbl, tgt, row_group_size=1_000_000)  # single big row group

    # Precondition: exactly one row group whose stats span AAPL..MSFT (min != max),
    # so the naive min/max path would miss GOOG.
    md = pq.ParquetFile(tgt).metadata
    assert md.num_row_groups == 1
    st = md.row_group(0).column(ph.COLUMNS.index("underlying")).statistics
    assert st.has_min_max and st.min != st.max

    assert ph.date_file_underlyings(tgt) == {"AAPL", "GOOG", "MSFT"}
    plan = ph.plan_missing(tmp_path, ["AAPL", "GOOG", "MSFT"], ["2026-07-20"])
    assert plan == {}  # all present -> nothing to re-pull


# ── merge_date_file ───────────────────────────────────────────────────────────

def test_merge_unions_sorts_and_leaves_no_tmp(tmp_path):
    tgt = tmp_path / "date=2026-07-20" / ph.DATE_FILE
    ph.merge_date_file(None, _decoded(["MSFT"]), tgt)          # existing {MSFT}
    n = ph.merge_date_file(tgt, _decoded(["AAPL"]), tgt)        # + new {AAPL}
    unds = pq.read_table(tgt).column("underlying").to_pylist()
    assert set(unds) == {"AAPL", "MSFT"}
    assert unds == sorted(unds)                                 # sorted by underlying
    assert n == 2
    assert not (tgt.with_suffix(".parquet.tmp")).exists()       # atomic: no leftover


def test_merge_nonforce_keeps_existing_on_conflict(tmp_path):
    tgt = tmp_path / "date=2026-07-20" / ph.DATE_FILE
    ph.merge_date_file(None, _decoded({"AAPL": 111}), tgt)
    ph.merge_date_file(tgt, _decoded({"AAPL": 222}), tgt)       # no force -> existing wins
    tbl = pq.read_table(tgt).to_pandas()
    assert tbl.loc[tbl["underlying"] == "AAPL", "bid_px"].tolist() == [111]


def test_merge_force_rewrites_requested_and_preserves_others(tmp_path):
    tgt = tmp_path / "date=2026-07-20" / ph.DATE_FILE
    ph.merge_date_file(None, _decoded({"AAPL": 111, "GOOG": 999}), tgt)
    ph.merge_date_file(tgt, _decoded({"AAPL": 222}), tgt, force=True)
    tbl = pq.read_table(tgt).to_pandas()
    assert tbl.loc[tbl["underlying"] == "AAPL", "bid_px"].tolist() == [222]  # rewritten
    assert tbl.loc[tbl["underlying"] == "GOOG", "bid_px"].tolist() == [999]  # preserved


def test_merge_existing_file_read_has_no_injected_hive_date_column(tmp_path):
    """``pq.read_table()`` infers a Hive partition key from a
    ``date=YYYY-MM-DD/`` path segment and injects a spurious ``date`` column
    into the result (reproduced against the installed pyarrow 18.0.0). Every
    date file ``merge_date_file`` merges into lives under exactly such a
    path, so its read of an EXISTING file must never see that column.

    The final merged output can't be used to prove this: ``merge_date_file``
    always hard-selects ``COLUMNS`` before writing, which would mask an
    injected column regardless of which pyarrow entry point did the read.
    So spy on the two candidate entry points (whichever one the code under
    test actually calls) and assert on the SCHEMA of what the read itself
    returned, before any column selection happens."""
    tgt = tmp_path / "date=2026-07-20" / ph.DATE_FILE
    ph.merge_date_file(None, _decoded(["SPY"]), tgt)  # real v2 date file on disk

    schemas: list[pa.Schema] = []
    real_read_table = ph.pq.read_table
    real_parquet_file = ph.pq.ParquetFile

    def spy_read_table(path, *a, **kw):
        table = real_read_table(path, *a, **kw)
        schemas.append(table.schema)
        return table

    class SpyParquetFile(real_parquet_file):
        def read(self, *a, **kw):
            table = super().read(*a, **kw)
            schemas.append(table.schema)
            return table

    with mock.patch.object(ph.pq, "read_table", spy_read_table), \
         mock.patch.object(ph.pq, "ParquetFile", SpyParquetFile):
        ph.merge_date_file(tgt, _decoded(["AAPL"]), tgt)  # merges into the EXISTING file

    assert schemas, "expected the existing-file read to be observed"
    schema = schemas[0]
    assert "date" not in schema.names, f"injected Hive `date` column: {schema.names}"
    assert list(schema.names) == ph.COLUMNS


# ── preflight math + degrade ──────────────────────────────────────────────────

def test_preflight_estimate_is_unit_times_missing_cells():
    plan = {"2026-07-20": ["SPY", "AAPL"], "2026-07-21": ["SPY"]}  # 3 cells
    fake = FakeHistorical(unit=0.01)
    pf = ph.preflight(
        fake, plan, symbols=["SPY", "AAPL"], weight={"SPY": 0.0, "AAPL": 1.0},
        snap_hm="19:55", cap=300.0, sample_days=3, index_symbol="SPY",
        min_degrade_names=1,
    )
    assert pf.n_missing == 3
    assert pf.unit_cost == pytest.approx(0.01)
    assert pf.total_cost == pytest.approx(0.03)  # unit x cells
    assert pf.keep == ["SPY", "AAPL"]
    assert pf.dropped == []
    assert not pf.blocked


def test_preflight_over_cap_degrades_to_index_plus_topN():
    plan = {"d1": ["SPY", "AAPL", "MSFT"], "d2": ["SPY", "AAPL", "MSFT"]}  # 6 cells
    weight = {"SPY": 0.0, "AAPL": 0.5, "MSFT": 0.3}
    fake = FakeHistorical(unit=10.0)  # total = 10 x 6 = 60 > cap 45
    pf = ph.preflight(
        fake, plan, symbols=["SPY", "AAPL", "MSFT"], weight=weight,
        snap_hm="19:55", cap=45.0, sample_days=1, index_symbol="SPY",
        min_degrade_names=1,
    )
    # per-symbol total = unit(10) x missing_days(2) = 20; greedily under cap 45:
    # SPY(20) + AAPL(40) fit; MSFT(60) drops.
    assert pf.keep == ["SPY", "AAPL"]
    assert pf.dropped == ["MSFT"]
    assert pf.total_cost == pytest.approx(40.0)
    assert not pf.blocked


def test_preflight_blocks_below_floor_exit3(tmp_path):
    plan = {"d1": ["SPY", "AAPL", "MSFT"], "d2": ["SPY", "AAPL", "MSFT"]}
    weight = {"SPY": 0.0, "AAPL": 0.5, "MSFT": 0.3}
    fake = FakeHistorical(unit=10.0)  # per-symbol total 20; only SPY fits under 25
    pf = ph.preflight(
        fake, plan, symbols=["SPY", "AAPL", "MSFT"], weight=weight,
        snap_hm="19:55", cap=25.0, sample_days=1, index_symbol="SPY",
        min_degrade_names=1,
    )
    assert pf.keep == ["SPY"]
    assert pf.blocked  # index leg + 0 names < floor of 1 -> BLOCK

    # …and run() surfaces the block as exit code 3.
    symfile = tmp_path / "syms.tsv"
    symfile.write_text("symbol\traw_weight\nSPY\t0.0\nAAPL\t0.5\nMSFT\t0.3\n")
    out = tmp_path / "hive"
    args = ph.build_parser().parse_args(
        ["--universe", str(symfile), "--start", "2026-07-20", "--end", "2026-07-21",
         "--out", str(out), "--cap", "25", "--sample-days", "1", "--min-degrade-names", "1"]
    )
    frames = {d: _raw_df(["SPY", "AAPL", "MSFT"]) for d in ("2026-07-20", "2026-07-21")}
    rc = ph.run(args, client=FakeHistorical(unit=10.0, frames=frames))
    assert rc == 3


# ── dry-run + resume (integration through run()/pull()) ───────────────────────

def test_dry_run_records_zero_get_range(tmp_path):
    symfile = tmp_path / "syms.txt"
    symfile.write_text("SPY\nAAPL\n")
    out = tmp_path / "hive"
    args = ph.build_parser().parse_args(
        ["--symbols-file", str(symfile), "--start", "2026-07-20", "--end", "2026-07-21",
         "--out", str(out), "--cap", "1000", "--dry-run"]
    )
    frames = {d: _raw_df(["SPY", "AAPL"]) for d in ("2026-07-20", "2026-07-21")}
    fake = FakeHistorical(unit=0.001, frames=frames)
    rc = ph.run(args, client=fake)
    assert rc == 0
    assert fake.get_range_calls() == []            # free preflight only
    assert list(out.glob("date=*/" + ph.DATE_FILE)) == []  # nothing written


def test_resume_dbn_cache_hit_rewrites_boards_without_get_range(tmp_path):
    out = tmp_path / "hive"
    plan = {"2026-07-20": ["SPY", "AAPL"]}
    frames = {"2026-07-20": _raw_df(["SPY", "AAPL"])}
    root_to_sym = {"SPY": "SPY", "AAPL": "AAPL"}
    date_file = out / "date=2026-07-20" / ph.DATE_FILE

    fake1 = FakeHistorical(frames=frames)
    ph.pull(fake1, plan, out, snap_hm="19:55", root_to_sym=root_to_sym,
            store_loader=_fake_loader)
    assert len(fake1.get_range_calls()) == 1       # one call per date
    assert date_file.exists()
    dbn_files = list((out / "_dbn").glob("*.dbn.zst"))
    assert len(dbn_files) == 1                      # raw cached

    # Simulate a crash after download but before split: drop the board, keep DBN.
    date_file.unlink()
    fake2 = FakeHistorical(frames=frames, raise_on_range=True)
    ph.pull(fake2, plan, out, snap_hm="19:55", root_to_sym=root_to_sym,
            store_loader=_fake_loader)
    assert fake2.get_range_calls() == []           # cache hit -> no API, no spend
    assert date_file.exists()                      # boards still written
    unds = set(pq.read_table(date_file).column("underlying").to_pylist())
    assert unds == {"SPY", "AAPL"}


# ── R2-b: "ACTUAL SPEND" -> "REALIZED ESTIMATE", with interpretable cell counts ─

def test_pull_result_realized_estimate_and_cell_counts(tmp_path):
    """The old ``actual_spend`` field/label is gone (F-06: it was never an
    actual charge). ``PullResult`` now exposes ``realized_estimate`` plus the
    cell-level counts that make it interpretable: a requested cell that comes
    back with no matching rows is ``no_options``, not silently absorbed into
    the estimate."""
    plan = {"2026-07-20": ["SPY", "AAPL"]}
    # AAPL has no matching rows this session -> must show up as "no_options",
    # not be silently counted as written or folded into the estimate.
    frames = {"2026-07-20": _raw_df(["SPY"])}
    root_to_sym = {"SPY": "SPY", "AAPL": "AAPL"}
    fake = FakeHistorical(unit=0.01, frames=frames)

    res = ph.pull(fake, plan, tmp_path, snap_hm="19:55", root_to_sym=root_to_sym,
                  unit_cost=0.01, store_loader=_fake_loader)

    assert not hasattr(res, "actual_spend")
    assert res.realized_estimate == pytest.approx(0.01 * 1)  # 1 board actually written
    assert res.cells_requested == 2       # SPY + AAPL requested
    assert res.boards_written == 1        # only SPY had data
    assert res.cells_failed == 0
    assert res.rows_returned == 1         # raw decoded rows for the date (SPY only)
    assert res.unmapped_rows == 0
    no_options = [m for m in res.manifest if m["status"] == "no_options"]
    assert [m["symbol"] for m in no_options] == ["AAPL"]


# ── ET-anchored snapshot minute (DST-aware) ────────────────────────────────────

def test_snap_et_maps_est_and_edt_correctly():
    # 2022-01-03 is EST: 15:55 ET == 20:55 UTC. 2022-07-01 is EDT: 15:55 ET == 19:55 UTC.
    assert ph.snapshot_minute_utc("2022-01-03", "15:55") == "20:55"
    assert ph.snapshot_minute_utc("2022-07-01", "15:55") == "19:55"


def test_snap_and_snap_et_mutually_exclusive():
    # The tool's flag is --snap-utc (the brief's "--snap" shorthand); --snap-et
    # must be mutually exclusive with it and exit 2 (argparse's usual error) when
    # both are given.
    with pytest.raises(SystemExit) as exc:
        ph.build_parser().parse_args(
            ["--symbols-file", "x.txt", "--start", "2026-07-20", "--end", "2026-07-21",
             "--snap-utc", "19:55", "--snap-et", "15:55"]
        )
    assert exc.value.code == 2


# ── Resume minute check (plan_missing) ─────────────────────────────────────────

def test_plan_missing_repulls_on_minute_mismatch(tmp_path):
    # Existing date file stamped at 19:55Z; the plan expects 20:55Z for that date
    # (e.g. an EST session under --snap-et) -> the whole date is "missing" again,
    # with its full symbol set, and flagged for a full replace (not a union).
    tgt = tmp_path / "date=2026-07-20" / ph.DATE_FILE
    ph.merge_date_file(None, _decoded(["SPY", "AAPL"], ts="2026-07-20T19:55:00"), tgt)

    plan = ph.plan_missing(tmp_path, ["SPY", "AAPL"], ["2026-07-20"],
                           expected_minute={"2026-07-20": "20:55"})
    assert plan == {"2026-07-20": ["SPY", "AAPL"]}
    assert plan.repull_dates == {"2026-07-20"}


def test_plan_missing_subtracts_absent_sidecar(tmp_path):
    # Date file holds {A} only; the sidecar (for the SAME expected minute) says B
    # is a known-absent underlying (e.g. no listed options that day) -> the
    # request for {A, B} has nothing left missing.
    tgt = tmp_path / "date=2026-07-20" / ph.DATE_FILE
    ph.merge_date_file(None, _decoded(["A"]), tgt)
    absent_dir = tmp_path / "_absent"
    absent_dir.mkdir()
    (absent_dir / "2026-07-20.json").write_text(json.dumps(
        {"minute_utc": "19:55", "symbols": ["B"], "asof": "2026-07-20T00:00:00+00:00"}
    ))

    plan = ph.plan_missing(tmp_path, ["A", "B"], ["2026-07-20"],
                           expected_minute={"2026-07-20": "19:55"})
    assert plan == {}


def test_sidecar_ignored_when_minute_differs(tmp_path):
    # The on-disk file's ts (20:55) matches the expected minute (no repull), but
    # the sidecar was recorded at a DIFFERENT minute (19:55, e.g. a stale EDT-era
    # entry) -> it must NOT mask B for a 20:55 request.
    tgt = tmp_path / "date=2026-07-20" / ph.DATE_FILE
    ph.merge_date_file(None, _decoded(["A"], ts="2026-07-20T20:55:00"), tgt)
    absent_dir = tmp_path / "_absent"
    absent_dir.mkdir()
    (absent_dir / "2026-07-20.json").write_text(json.dumps(
        {"minute_utc": "19:55", "symbols": ["B"], "asof": "2026-07-20T00:00:00+00:00"}
    ))

    plan = ph.plan_missing(tmp_path, ["A", "B"], ["2026-07-20"],
                           expected_minute={"2026-07-20": "20:55"})
    assert plan == {"2026-07-20": ["B"]}


# ── Absent-symbol sidecar (write side, through pull()) ─────────────────────────

def test_absent_sidecar_written_after_pull(tmp_path):
    out = tmp_path / "hive"
    plan = {"2026-07-20": ["SPY", "AAPL"]}
    frames = {"2026-07-20": _raw_df(["SPY"])}  # AAPL comes back with zero rows
    root_to_sym = {"SPY": "SPY", "AAPL": "AAPL"}
    fake = FakeHistorical(frames=frames)

    ph.pull(fake, plan, out, snap_hm="19:55", root_to_sym=root_to_sym,
            store_loader=_fake_loader)

    sidecar = out / "_absent" / "2026-07-20.json"
    assert sidecar.exists()
    data = json.loads(sidecar.read_text())
    assert data["symbols"] == ["AAPL"]
    assert data["minute_utc"] == "19:55"
    assert "asof" in data


def test_force_clears_sidecar(tmp_path):
    # A prior sidecar claims {B, C} absent. --force repulls A/B/C fresh; the
    # fake provider now has data for C (the old record was stale) but still none
    # for B -> the rewritten sidecar reflects ONLY this run's findings ({B}), not
    # a merge with the stale prior entry.
    out = tmp_path / "hive"
    absent_dir = out / "_absent"
    absent_dir.mkdir(parents=True)
    (absent_dir / "2026-07-20.json").write_text(json.dumps(
        {"minute_utc": "19:55", "symbols": ["B", "C"], "asof": "2026-07-20T00:00:00+00:00"}
    ))
    plan = {"2026-07-20": ["A", "B", "C"]}
    frames = {"2026-07-20": _raw_df(["A", "C"])}  # B still absent; C now has data
    root_to_sym = {"A": "A", "B": "B", "C": "C"}
    fake = FakeHistorical(frames=frames)

    ph.pull(fake, plan, out, snap_hm="19:55", root_to_sym=root_to_sym,
            store_loader=_fake_loader, force=True)

    sidecar = out / "_absent" / "2026-07-20.json"
    data = json.loads(sidecar.read_text())
    assert data["symbols"] == ["B"]
    assert data["minute_utc"] == "19:55"


# ── Fix-round regression tests (code review: Critical 1, Important 2/3/4) ──────

def test_pull_replaces_full_file_on_minute_mismatch_not_force_merge(tmp_path):
    """Critical-1 regression. Existing file holds {AAPL, SPY} stamped at
    19:55Z. A repull-flagged pull (simulating a detected minute mismatch, e.g.
    a DST-boundary re-run) requests {AAPL, SPY} at a NEW minute (20:55Z) but
    the fresh provider response only has SPY (AAPL comes back with zero rows
    this time). The old bug: a force-style merge only replaces underlyings
    PRESENT in the new frame, so AAPL@19:55 would survive untouched, leaving
    the file holding two different snapshot instants at once (AAPL@19:55 +
    SPY@20:55) -- a corrupted, permanently-mismatching file (the mismatch
    check would flag it forever, but the sidecar branch is unreachable because
    the mismatch branch `continue`s first). The fix: a repull-flagged date is
    a genuine full replacement -- the resulting file must hold ONLY what this
    pull actually returned (SPY), at the new minute, with AAPL simply absent
    (correctly re-queued as missing on the next resume, not stale)."""
    out = tmp_path / "hive"
    date = "2026-01-02"
    tgt = out / f"date={date}" / ph.DATE_FILE
    ph.merge_date_file(None, _decoded(["AAPL", "SPY"], ts=f"{date}T19:55:00"), tgt)

    plan = {date: ["AAPL", "SPY"]}
    frames = {date: _raw_df(["SPY"])}  # AAPL: zero rows this time
    root_to_sym = {"AAPL": "AAPL", "SPY": "SPY"}
    fake = FakeHistorical(frames=frames)

    ph.pull(fake, plan, out, snap_hm="20:55", root_to_sym=root_to_sym,
            store_loader=_fake_loader, repull_dates={date})

    tbl = pq.read_table(tgt).to_pandas()
    assert set(tbl["underlying"]) == {"SPY"}  # AAPL@19:55 must NOT survive
    tss = tbl["ts"].unique()
    assert len(tss) == 1                      # single constant ts -- no mixed file
    ts = pd.Timestamp(tss[0])
    assert (ts.hour, ts.minute) == (20, 55)   # the NEW minute, not the stale 19:55


def test_plan_missing_treats_mixed_ts_file_as_mismatch(tmp_path):
    """Critical-1 belt-and-braces regression. A file whose row groups do NOT
    all agree on `ts` (e.g. a straggler from before this fix, or any other
    corruption) must be treated as a mismatch even when the FIRST row group
    happens to agree with the expected minute -- row group order is not a
    guarantee of "the truth", so `_date_file_snap_ts` must scan every row
    group, not just the first."""
    tgt = tmp_path / "date=2026-07-20" / ph.DATE_FILE
    tgt.parent.mkdir(parents=True, exist_ok=True)
    frame_a = _decoded(["AAPL"], ts="2026-07-20T19:55:00")  # row group 0: matches "want"
    frame_b = _decoded(["SPY"], ts="2026-07-20T20:55:00")   # row group 1: does NOT
    combined = pd.concat([frame_a, frame_b], ignore_index=True)
    writer = pq.ParquetWriter(tgt, ph.ARROW_SCHEMA)
    for _sym, grp in combined.groupby("underlying", sort=True):
        writer.write_table(pa.Table.from_pandas(grp[ph.COLUMNS], schema=ph.ARROW_SCHEMA,
                                                 preserve_index=False))
    writer.close()

    plan = ph.plan_missing(tmp_path, ["AAPL", "SPY"], ["2026-07-20"],
                           expected_minute={"2026-07-20": "19:55"})
    assert plan == {"2026-07-20": ["AAPL", "SPY"]}
    assert plan.repull_dates == {"2026-07-20"}


def test_run_with_snap_et_threads_per_date_minute_through_preflight_and_pull(tmp_path):
    """Important-3(b): a real run()-level test (replacing the earlier ad hoc
    smoke script) proving --snap-et's per-date {date: "HH:MM"} map threads all
    the way through preflight() -> pull() -> the stamped ts, the DBN cache
    name, and the manifest filename."""
    symfile = tmp_path / "syms.txt"
    symfile.write_text("SPY\nAAPL\n")
    out = tmp_path / "hive"
    date = "2026-01-02"  # EST session: 15:55 ET == 20:55 UTC

    args = ph.build_parser().parse_args([
        "--symbols-file", str(symfile), "--start", date, "--end", date,
        "--out", str(out), "--cap", "1000", "--snap-et", "15:55",
    ])
    frames = {date: _raw_df(["SPY", "AAPL"])}
    fake = FakeHistorical(unit=0.001, frames=frames)

    rc = ph.run(args, client=fake, store_loader=_fake_loader)
    assert rc == 0

    date_file = out / f"date={date}" / ph.DATE_FILE
    tbl = pq.read_table(date_file).to_pandas()
    ts = pd.Timestamp(tbl["ts"].iloc[0])
    assert (ts.hour, ts.minute) == (20, 55)  # EST: 15:55 ET == 20:55Z

    dbn_files = list((out / "_dbn").glob(f"{date}_2055_*.dbn.zst"))
    assert len(dbn_files) == 1  # resolved per-date minute embedded in the cache name

    manifests = list(out.glob("manifest_hive_*"))
    assert len(manifests) == 1
    assert "1555" in manifests[0].name  # labeled by the stable ET time, not the UTC minute


def test_empty_session_does_not_record_sidecar(tmp_path):
    """Important-2 regression. A WHOLE-session zero-row response (every
    requested symbol, not just one) is a bad-window/holiday signature -- e.g.
    a real XNYS early close (Jul 3, day after Thanksgiving, Christmas Eve, all
    13:00 ET closes) where a 15:55 ET snapshot window falls after the session
    ended -- not genuine per-name absence. It must NOT be recorded in the
    sidecar (which would permanently latch the date "complete" and silently
    hide it from every future resume, recoverable only via --force)."""
    out = tmp_path / "hive"
    plan = {"2026-07-03": ["SPY", "AAPL"]}
    frames = {"2026-07-03": _raw_df([])}  # nothing at all in the snapshot window
    root_to_sym = {"SPY": "SPY", "AAPL": "AAPL"}
    fake = FakeHistorical(frames=frames)

    ph.pull(fake, plan, out, snap_hm="19:55", root_to_sym=root_to_sym,
            store_loader=_fake_loader)

    sidecar = out / "_absent" / "2026-07-03.json"
    assert not sidecar.exists()


def test_force_sidecar_preserves_entries_for_symbols_not_requested_this_round(tmp_path):
    """Important-4 regression. A prior sidecar claims {B, C} absent. A --force
    pull this round requests only {A, B} -- C is NOT part of this invocation
    (e.g. the cap-degrade filter dropped it before pull() ever saw it). The
    rewritten sidecar must re-confirm B (still absent) but leave C's record
    completely untouched: this call never re-checked C, so force must not
    erase that memory (which would re-queue -- and re-bill -- C later)."""
    out = tmp_path / "hive"
    absent_dir = out / "_absent"
    absent_dir.mkdir(parents=True)
    (absent_dir / "2026-07-20.json").write_text(json.dumps(
        {"minute_utc": "19:55", "symbols": ["B", "C"], "asof": "2026-07-20T00:00:00+00:00"}
    ))
    plan = {"2026-07-20": ["A", "B"]}  # C is NOT requested this round
    frames = {"2026-07-20": _raw_df(["A"])}  # B still absent; A has data
    root_to_sym = {"A": "A", "B": "B", "C": "C"}
    fake = FakeHistorical(frames=frames)

    ph.pull(fake, plan, out, snap_hm="19:55", root_to_sym=root_to_sym,
            store_loader=_fake_loader, force=True)

    sidecar = out / "_absent" / "2026-07-20.json"
    data = json.loads(sidecar.read_text())
    assert set(data["symbols"]) == {"B", "C"}  # B re-confirmed; C preserved untouched
