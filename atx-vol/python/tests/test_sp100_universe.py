import csv
from pathlib import Path

UNIVERSE = Path(__file__).resolve().parents[2] / "data" / "universe" / "sp100_2026-07.csv"

def _rows():
    with open(UNIVERSE, newline="") as f:
        return list(csv.DictReader(f, delimiter="\t"))

def test_universe_exists_and_has_expected_shape():
    rows = _rows()
    assert len(rows) == 102  # SPY + 101 S&P 100 tickers (GOOG/GOOGL dual class)
    assert rows[0]["symbol"] == "SPY"
    assert float(rows[0]["raw_weight"]) == 100.0

def test_symbols_unique_and_clean():
    rows = _rows()
    syms = [r["symbol"] for r in rows]
    assert len(set(syms)) == len(syms)
    for s in syms:
        assert s == s.strip().upper()
        assert " " not in s

def test_weights_strictly_descending_after_index():
    rows = _rows()
    w = [float(r["raw_weight"]) for r in rows[1:]]
    assert all(a > b for a, b in zip(w, w[1:]))

def test_loads_via_pull_tool_reader():
    import sys
    sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
    import pull_opra_hive as m
    entries = m.read_universe(UNIVERSE)  # Returns list of (symbol, weight) tuples
    assert len(entries) == 102
    # First entry is SPY with weight 100.0
    assert entries[0][0] == "SPY"
    assert entries[0][1] == 100.0
