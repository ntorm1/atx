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
import pathlib
import sys

import pandas as pd
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


def _decoded(underlyings, *, bid_px=100) -> pd.DataFrame:
    """A decoded 8-column frame (post-DBN) for merge/plan helpers.

    ``underlyings`` may be an iterable of names or a ``{name: bid_px}`` map."""
    if isinstance(underlyings, dict):
        items = list(underlyings.items())
    else:
        items = [(u, bid_px) for u in underlyings]
    rows = []
    for u, bp in items:
        rows.append(
            {
                "ts": pd.Timestamp("2026-07-20T19:55:00"),
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
