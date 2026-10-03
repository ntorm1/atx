"""Guard G-P5 (P9 lane T1): no Python module computes a rule a C++ module decides, for the P9 list.

The P9 list (plan section 0.4, G-P5): the composition fit mirrors (composition review table rows 1-5b), the statistics
of record (main review F-1), the Newey-West t of the admission screen (table row 7), the gross-match rule
(wave_rules.py:110-119), the PM7-35 sign rule (P9 ruling P9) and horizon_stats.py. Each rule is known here by the
names of the Python functions that compute it (RULE_SYMBOLS). The research Python (SCOPE) is parsed, and every
function definition with one of those names must be an ALLOWLIST row: the mirrors that exist at the P9 base, each with
the C++ that decides the rule (or will: "new in <lane>") and the lane that retires it.

  - a definition that is not a row fails (a new mirror, or a copy of one under another module);
  - a row whose module is gone, or no longer defines a listed symbol, fails (the retiring commit deletes the row, so
    the list cannot go stale);
  - the list never grows, and every row names a retiring lane (P10 = outside the P9 list, kept until after P9);
  - P9_FREEZE: root sets it True at the P9 freeze; from then on every row retired by a P9 lane must be gone (the G-P5
    count is the number of such rows still listed, printed by test_report_the_g_p5_count).
Expiry is the retiring lane's deletion commit, not the C++ file's arrival: plan section 0.6 keeps a Python copy until
one slice after root's identity run, and several replacements (eval/deflated_sharpe.hpp, strategy_ic_composition.cpp)
exist at the base already. BINDINGS are thin wrappers of the C++ core (atxpy._core), not mirrors; the test checks
each one still calls the core.

Out of the scan: tests (test_*.py, *_test.py, conftest.py and anything under tests/ or fixtures/): a test may hold an
independent oracle on purpose.

  "C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider scripts/tests/test_no_python_mirror.py
"""
from __future__ import annotations

import ast
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
FROZEN_AT = "d7c1c520"
P9_FREEZE = False
SCOPE = ("scripts", "atx-engine/tools", "atx-impl/tools", "atx-impl/strategies", "python")
P9_LANES = ("B1", "B2", "C2", "C3", "D2", "D3", "E2")
LANES = P9_LANES + ("P10",)

STATS = "statistics of record (F-1)"
COMPOSITION = "composition fit mirror (composition review table rows 1-5b)"
# (module, symbols, rule, C++ that decides it, retired_by). Frozen at FROZEN_AT: delete rows or symbols, never add.
ALLOWLIST = (
    ("atx-impl/tools/backtest_integrity.py",
     ("psr", "min_trl", "deflated_sharpe", "expected_max_sr", "lo_null_dsr", "effective_n_dsr"), STATS,
     "atx-engine/include/atx/engine/eval/deflated_sharpe.hpp, min_trl.hpp", "B2"),
    ("atx-impl/tools/backtest_integrity.py", ("cscv_pbo",), STATS, "atx-engine/include/atx/engine/eval/pbo.hpp", "B2"),
    ("atx-impl/tools/backtest_integrity.py", ("onc", "onc_base", "effective_trials"), STATS,
     "atx-engine/include/atx/engine/eval/trial_clusters.hpp", "B2"),
    ("atx-impl/tools/nav_summ.py", ("deflated_sharpe", "expected_max_sr", "dsr_rows"), STATS,
     "atx-engine/include/atx/engine/eval/deflated_sharpe.hpp", "B2"),
    ("atx-impl/tools/nav_summ.py", ("memmel_se", "cbb_indices", "paired_stats"), STATS,
     "new in B2 (atx-research-eval paired block, K-P9-5)", "B2"),
    ("atx-impl/tools/dsr_total.py", ("dsr_at", "ledger_rows"), STATS,
     "new in B2 (research/ledger + eval/deflated_sharpe.hpp house_v1)", "B2"),
    ("atx-impl/tools/composition_rules.py",
     ("ew_theme_std", "ew_theme_aim_v2", "tier_weights", "member_cap", "theme_gain_weights"), COMPOSITION,
     "atx-impl/src/strategy_ic_composition.cpp over engine combine/group_rerank.hpp", "D2"),
    ("atx-impl/tools/composition_resid.py", ("centred_tied_ranks", "tie_block_means"), COMPOSITION,
     "atx-impl/src/strategy_ic_theme_resid.cpp over engine combine/group_residualise.hpp", "D2"),
    ("atx-impl/tools/composition_theme_erc.py", ("erc_shares", "theme_erc"), COMPOSITION,
     "atx-impl/src/strategy_ic_theme_erc.cpp over engine combine/group_erc.hpp", "D2"),
    ("atx-impl/tools/composition_ic_shrink.py", ("shrunk_shares", "ic_shrink"), COMPOSITION,
     "atx-impl/src/strategy_ic_shrink.cpp over engine combine/group_shrink.hpp", "D2"),
    ("atx-impl/tools/fit_composition_weights.py", ("newey_west_t", "screen_v4"),
     "admission screen and its Newey-West t (composition review table row 7)",
     "new in B1 (atx-engine research/admission on eval/hac.hpp); the fitter is wired to it by D2", "D2"),
    ("atx-impl/tools/horizon_stats.py", ("theta_weights", "ic_theta", "theta_book_returns"),
     "aim-partial theta of strategy_target_replay.cpp (G-P5 names horizon_stats.py; plan section 2.2 names no owner: "
     "D2 owns the fitter that imports it)", "atx-impl/src/strategy_target_replay.cpp", "D2"),
    ("scripts/wave_rules.py", ("matched_leverage", "gross_matches"), "gross-match rule PM6-6 (wave_rules.py:110-119)",
     "new in C2 (atx-equity-strategy-targets nav --calibrate-gross)", "C2"),
    ("scripts/wave_rules.py", ("sign_pm7_35",), "PM7-35 sign rule (P9 ruling P9: kept in wave 1)",
     "new in B1 (the research/admission sign predicate)", "E2"),
    ("atx-impl/tools/mega_report/data.py", ("memmel_se",), "report renderer's copy of nav_summ.memmel_se (outside the "
     "P9 list)", "new in B2 (atx-research-eval paired block)", "P10"),
    ("python/src/atxpy/pbo.py", ("cscv_pbo",), "atxpy's standalone pandas CSCV (outside the P9 list)",
     "atx-engine/include/atx/engine/eval/pbo.hpp (bound as atxpy._core.pbo_cscv)", "P10"),
)
FROZEN_ROWS = 16
# (module, symbol): wrappers that call the C++ core, checked to still do so.
BINDINGS = {("python/src/atxpy/eval.py", "deflated_sharpe"): "_core.deflated_sharpe("}
RULE_SYMBOLS = frozenset(s for _m, symbols, _r, _c, _l in ALLOWLIST for s in symbols) | {s for _m, s in BINDINGS}


def scanned_modules() -> list[Path]:
    out = []
    for top in SCOPE:
        for path in sorted((REPO / top).rglob("*.py")):
            rel = path.relative_to(REPO).as_posix()
            parts = rel.split("/")
            name = parts[-1]
            if name.startswith("test_") or name.endswith("_test.py") or name == "conftest.py":
                continue
            if "tests" in parts[:-1] or "fixtures" in parts[:-1] or "__pycache__" in parts:
                continue
            out.append(path)
    return out


def defined_functions(path: Path) -> set[str]:
    """Every function or method name defined anywhere in the module."""
    tree = ast.parse(path.read_text(encoding="utf-8"), filename=str(path))
    return {node.name for node in ast.walk(tree) if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))}


def top_level_functions(path: Path) -> set[str]:
    tree = ast.parse(path.read_text(encoding="utf-8"), filename=str(path))
    return {node.name for node in tree.body if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))}


def allowed() -> set[tuple[str, str]]:
    return {(module, s) for module, symbols, _r, _c, _l in ALLOWLIST for s in symbols} | set(BINDINGS)


def test_no_new_python_mirror():
    permitted = allowed()
    found = sorted((path.relative_to(REPO).as_posix(), name) for path in scanned_modules()
                   for name in defined_functions(path) & RULE_SYMBOLS)
    new = [f"{module}: def {name}" for module, name in found if (module, name) not in permitted]
    assert not new, ("Python mirror(s) of a C++-decided rule outside the allowlist frozen at "
                     f"{FROZEN_AT} (G-P5): call the engine instead: {new}")


def test_every_row_names_an_existing_definition():
    stale = []
    for module, symbols, _rule, _cpp, _lane in ALLOWLIST:
        path = REPO / module
        if not path.is_file():
            stale.append(f"{module}: module gone")
            continue
        missing = set(symbols) - top_level_functions(path)
        if missing:
            stale.append(f"{module}: no longer defines {sorted(missing)}")
    assert not stale, f"stale rows: delete them (or the symbol) in the retiring commit: {stale}"


def test_bindings_still_call_the_core():
    for (module, symbol), call in BINDINGS.items():
        path = REPO / module
        assert path.is_file() and symbol in top_level_functions(path), module
        assert call in path.read_text(encoding="utf-8"), f"{module}: {symbol} no longer calls {call}"


def test_the_allowlist_only_shrinks_and_names_rule_owner_and_lane():
    assert len(ALLOWLIST) <= FROZEN_ROWS, f"the mirror allowlist frozen at {FROZEN_AT} gained rows"
    for module, symbols, rule, cpp, lane in ALLOWLIST:
        assert symbols and rule and cpp and lane in LANES, module
        if not cpp.startswith("new in "):
            first = cpp.split(",")[0].split(" ")[0]
            assert (REPO / first).is_file(), f"{module}: the deciding C++ {first} does not exist"


def test_rows_retired_by_a_p9_lane_are_gone_at_the_freeze():
    left = [f"{module} {list(symbols)} ({lane})" for module, symbols, _r, _c, lane in ALLOWLIST if lane in P9_LANES]
    if P9_FREEZE:
        assert not left, f"G-P5 at the P9 freeze: these mirrors are still listed: {left}"


def test_report_the_g_p5_count(capsys):
    count = sum(len(symbols) for _m, symbols, _r, _c, lane in ALLOWLIST if lane in P9_LANES)
    with capsys.disabled():
        print(f"\nG-P5: {count} mirrored rule function(s) in "
              f"{sum(1 for row in ALLOWLIST if row[4] in P9_LANES)} row(s) left for P9 lanes")
    assert count >= 0


def test_the_scan_sees_the_known_mirrors_and_skips_tests():
    rels = {p.relative_to(REPO).as_posix() for p in scanned_modules()}
    assert {"atx-impl/tools/backtest_integrity.py", "atx-impl/tools/nav_summ.py", "scripts/wave_rules.py"} <= rels
    assert not any(r.rsplit("/", 1)[-1].startswith("test_") or "/tests/" in r for r in rels)
