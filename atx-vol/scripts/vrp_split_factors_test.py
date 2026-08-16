#!/usr/bin/env python3
"""Tests for vrp_split_factors.py.

Stdlib-only and warehouse-free: the DuckDB read is the one part that needs a
74 GB file, so it is exercised by the panel's own C++ loader gate instead.
What is tested here is everything that decides the BYTES of the reference
file -- universe parsing, the emitted grammar, determinism, and the band
validation -- because those are what `load_vrp_split_factors` contracts on.
"""

from __future__ import annotations

import importlib.util
import sys
from pathlib import Path

import pytest

_SPEC = importlib.util.spec_from_file_location(
    "vrp_split_factors", Path(__file__).with_name("vrp_split_factors.py")
)
assert _SPEC and _SPEC.loader
vsf = importlib.util.module_from_spec(_SPEC)
sys.modules["vrp_split_factors"] = vsf
_SPEC.loader.exec_module(vsf)


EVENTS = [
    {
        "symbol": "NFLX",
        "ex_date": "2025-11-17",
        "price_factor": 0.1,
        "close": 110.29,
        "prev_close": 1112.17,
        "implied_ratio": 10.084051137909148,
    },
    {
        "symbol": "NOW",
        "ex_date": "2025-12-18",
        "price_factor": 0.2,
        "close": 153.38,
        "prev_close": 782.39,
        "implied_ratio": 5.100990351414787,
    },
]


# ── universe parsing ────────────────────────────────────────────────────


def test_read_universe_plain_list_dedupes_and_sorts(tmp_path: Path) -> None:
    p = tmp_path / "u.txt"
    p.write_text("NOW\n# a comment\n\nAAPL\nNOW\n", encoding="utf-8")
    assert vsf.read_universe(p) == ["AAPL", "NOW"]


def test_read_universe_reads_symbol_column_from_csv(tmp_path: Path) -> None:
    p = tmp_path / "u.csv"
    p.write_text(
        "effective_date,symbol,raw_weight\n2026-07-26,SPY,100.0\n2026-07-26,AAPL,1.0\n",
        encoding="utf-8",
    )
    assert vsf.read_universe(p) == ["AAPL", "SPY"]


def test_read_universe_reads_symbol_column_from_tsv(tmp_path: Path) -> None:
    p = tmp_path / "u.tsv"
    p.write_text("symbol\tweight\nBKNG\t1\nNFLX\t2\n", encoding="utf-8")
    assert vsf.read_universe(p) == ["BKNG", "NFLX"]


def test_read_universe_rejects_delimited_file_without_symbol_column(tmp_path: Path) -> None:
    p = tmp_path / "u.csv"
    p.write_text("ticker,weight\nAAPL,1\n", encoding="utf-8")
    with pytest.raises(SystemExit) as e:
        vsf.read_universe(p)
    assert e.value.code == 2


def test_read_universe_empty_file_is_empty_universe(tmp_path: Path) -> None:
    p = tmp_path / "u.txt"
    p.write_text("# only a comment\n\n", encoding="utf-8")
    assert vsf.read_universe(p) == []


# ── emitted grammar ─────────────────────────────────────────────────────


def _header_line(text: str) -> str:
    return next(ln for ln in text.splitlines() if not ln.startswith("#"))


def test_render_emits_the_exact_header_the_cpp_loader_requires() -> None:
    text = vsf.render(EVENTS, {"n_events": "2"})
    first3 = _header_line(text).split("\t")[:3]
    assert first3 == ["symbol", "ex_date", "price_factor"]


def test_render_puts_every_meta_key_behind_a_hash_before_the_header() -> None:
    text = vsf.render(EVENTS, {"band": "x", "n_events": "2"})
    lines = text.splitlines()
    hdr = lines.index(_header_line(text))
    assert all(ln.startswith("#") for ln in lines[:hdr])
    assert "# band=x" in lines[:hdr]


def test_render_rows_carry_factor_in_field_three() -> None:
    rows = [ln for ln in vsf.render(EVENTS, {}).splitlines() if not ln.startswith("#")][1:]
    assert [r.split("\t")[0] for r in rows] == ["NFLX", "NOW"]
    assert [r.split("\t")[2] for r in rows] == [repr(0.1), repr(0.2)]


def test_render_factor_round_trips_through_strtod() -> None:
    rows = [ln for ln in vsf.render(EVENTS, {}).splitlines() if not ln.startswith("#")][1:]
    assert [float(r.split("\t")[2]) for r in rows] == [0.1, 0.2]


def test_render_is_deterministic_and_newline_terminated() -> None:
    a = vsf.render(EVENTS, {"n_events": "2", "band": "b"})
    b = vsf.render(EVENTS, {"band": "b", "n_events": "2"})
    assert a == b
    assert a.endswith("\n")


def test_render_zero_events_still_emits_a_loadable_header() -> None:
    text = vsf.render([], {"n_events": "0"})
    body = [ln for ln in text.splitlines() if not ln.startswith("#")]
    assert body == [
        "symbol\tex_date\tprice_factor\tclose\tprev_close\timplied_ratio\tresidual\tuncorroborated"
    ]


def test_render_tolerates_missing_provenance_fields() -> None:
    ev = [{"symbol": "X", "ex_date": "2026-01-02", "price_factor": 0.5,
           "close": None, "prev_close": None, "implied_ratio": None}]
    row = [ln for ln in vsf.render(ev, {}).splitlines() if not ln.startswith("#")][1]
    assert row.split("\t")[:3] == ["X", "2026-01-02", repr(0.5)]


# ── corroboration ───────────────────────────────────────────────────────


def _ev(symbol: str, factor: float, implied: float | None) -> dict[str, object]:
    return {
        "symbol": symbol,
        "ex_date": "2026-01-02",
        "price_factor": factor,
        "close": 1.0,
        "prev_close": None if implied is None else implied,
        "implied_ratio": implied,
    }


def _band() -> tuple[float, float]:
    return vsf.DEFAULT_CORROBORATE_LO, vsf.DEFAULT_CORROBORATE_HI


def test_corroborate_accepts_a_factor_that_explains_the_step() -> None:
    # NFLX 2025-11-17: 10:1, observed step 10.0841 -> residual 1.0084.
    ok, bad = vsf.corroborate([_ev("NFLX", 0.1, 10.0841)], *_band())
    assert [e["symbol"] for e in ok] == ["NFLX"] and bad == []
    assert ok[0]["residual"] == pytest.approx(1.0084, abs=1e-4)


def test_corroborate_rejects_the_announcement_date_artifact() -> None:
    # V 2015-02-11: declared 4:1 with no price step at all -> residual 0.248.
    ok, bad = vsf.corroborate([_ev("V", 0.249546, 0.9946)], *_band())
    assert ok == [] and [e["symbol"] for e in bad] == ["V"]
    assert bad[0]["residual"] == pytest.approx(0.2482, abs=1e-4)


def test_corroborate_accepts_a_reverse_split() -> None:
    # GE 2021-08-02: 1:8 reverse, factor 8, observed step 0.1287.
    ok, bad = vsf.corroborate([_ev("GE", 8.0, 0.1287)], *_band())
    assert [e["symbol"] for e in ok] == ["GE"] and bad == []


def test_corroborate_accepts_the_tightest_measured_genuine_event() -> None:
    # TSLA 2020-08-31 is the closest genuine event to the floor (0.8883).
    ok, bad = vsf.corroborate([_ev("TSLA", 0.2, 4.4417)], *_band())
    assert [e["symbol"] for e in ok] == ["TSLA"] and bad == []


def test_corroborate_emits_an_uncheckable_event_and_flags_it() -> None:
    ok, bad = vsf.corroborate([_ev("GOOGL", 0.497974, None)], *_band())
    assert [e["symbol"] for e in ok] == ["GOOGL"] and bad == []
    assert ok[0]["uncorroborated"] is True and ok[0]["residual"] is None


def test_corroborate_partitions_a_mixed_batch() -> None:
    ok, bad = vsf.corroborate(
        [_ev("V", 0.249546, 0.9946), _ev("V", 0.25, 4.0064), _ev("NOW", 0.2, 5.1010)], *_band()
    )
    assert len(ok) == 2 and len(bad) == 1
    assert all(e["uncorroborated"] is False for e in ok)


def test_render_lists_rejected_events_in_the_provenance_block() -> None:
    _, bad = vsf.corroborate([_ev("V", 0.249546, 0.9946)], *_band())
    text = vsf.render([], {"n_rejected": "1"}, bad)
    rej = [ln for ln in text.splitlines() if ln.startswith("# REJECTED")]
    assert len(rej) == 1 and "V 2026-01-02" in rej[0]
    # A rejected event must never reach the data rows.
    body = [ln for ln in text.splitlines() if not ln.startswith("#")]
    assert len(body) == 1


def test_render_marks_the_uncorroborated_column() -> None:
    ok, _ = vsf.corroborate([_ev("GOOGL", 0.5, None), _ev("NOW", 0.2, 5.1010)], *_band())
    rows = [ln for ln in vsf.render(ok, {}).splitlines() if not ln.startswith("#")][1:]
    assert [r.split("\t")[-1] for r in rows] == ["1", "0"]


# ── band validation ─────────────────────────────────────────────────────


def test_default_band_matches_the_atx_db_split_policy() -> None:
    assert (vsf.DEFAULT_MIN_RATIO, vsf.DEFAULT_MAX_RATIO) == (0.8, 1.25)


@pytest.mark.parametrize("lo,hi", [(1.5, 1.25), (0.0, 1.25), (-1.0, 1.25), (1.25, 1.25)])
def test_main_rejects_a_degenerate_band(tmp_path: Path, lo: float, hi: float) -> None:
    with pytest.raises(SystemExit) as e:
        vsf.main(["--db", str(tmp_path / "w.duckdb"), "--out", str(tmp_path / "o.tsv"),
                  "--min-ratio", str(lo), "--max-ratio", str(hi)])
    assert e.value.code == 2


@pytest.mark.parametrize("lo,hi", [(1.5, 1.0), (0.0, 1.0), (-1.0, 2.0)])
def test_main_rejects_a_degenerate_corroboration_band(tmp_path: Path, lo: float, hi: float) -> None:
    with pytest.raises(SystemExit) as e:
        vsf.main(["--db", str(tmp_path / "w.duckdb"), "--out", str(tmp_path / "o.tsv"),
                  "--corroborate-lo", str(lo), "--corroborate-hi", str(hi)])
    assert e.value.code == 2


def test_main_rejects_a_missing_universe_before_touching_the_warehouse(tmp_path: Path) -> None:
    with pytest.raises(SystemExit) as e:
        vsf.main(["--db", str(tmp_path / "w.duckdb"), "--out", str(tmp_path / "o.tsv"),
                  "--universe", str(tmp_path / "nope.txt")])
    assert e.value.code == 2


def test_main_rejects_a_missing_warehouse(tmp_path: Path) -> None:
    with pytest.raises(SystemExit) as e:
        vsf.main(["--db", str(tmp_path / "absent.duckdb"), "--out", str(tmp_path / "o.tsv")])
    assert e.value.code == 2
