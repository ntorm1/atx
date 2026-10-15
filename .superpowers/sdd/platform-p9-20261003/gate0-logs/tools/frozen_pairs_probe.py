"""GATE-0 (RESUME 11) IC-S: what adding the four G-P5-invisible pairs to FROZEN_PAIRS would assert (read-only).

Imports scripts/tests/test_no_python_mirror.py by path (PYTHONDONTWRITEBYTECODE=1 expected; no file is edited) and
prints, for the candidate (module, symbol) pairs named by wave-review-ic.md S-3 / wave-review-python.md S-1:
  1. every definition of a candidate name in the guard's SCOPE (methods included, as the scan sees them);
  2. the guard's tests with the pairs added to FROZEN_PAIRS only (RULE_SYMBOLS widened), in memory;
  3. the same with matching P10 ALLOWLIST rows added too (the E2 fed4d4cb form of a ruled row), in memory.
usage: frozen_pairs_probe.py REPO
"""
from __future__ import annotations

import importlib.util
import sys
from pathlib import Path

CANDIDATES = (   # (module, symbols, rule, C++ that decides it)
    ("atx-impl/tools/backtest_integrity.py", ("fold_head", "chain_head", "trial_counts", "ledger_n", "dsr_variance"),
     "ledger counting and chain rules", "atx-engine/include/atx/engine/research/ledger/trial_counts.hpp"),
    ("atx-impl/tools/composition_theme_tsmom.py", ("masses", "theme_masses", "sleeves", "trailing", "schedule"),
     "theme-tsmom-v1 schedule", "atx-engine/src/research/composition/fit.cpp"),
    ("atx-impl/tools/composition_resid.py", ("registered_order", "theme_order"),
     "theme-resid-v1 order", "atx-engine/src/research/composition/fit.cpp"),
    ("atx-impl/tools/composition_mom_volman.py", ("schedule",),
     "mom-volman-v1 sleeve", "atx-engine/src/research/composition/fit.cpp"),
)
TESTS = ("test_no_new_python_mirror", "test_every_row_names_an_existing_definition", "test_bindings_still_call_the_core",
         "test_the_allowlist_only_shrinks_and_names_rule_owner_and_lane",
         "test_rows_retired_by_a_p9_lane_are_gone_at_the_freeze", "test_the_scan_sees_the_known_mirrors_and_skips_tests")


def load(repo: Path):
    spec = importlib.util.spec_from_file_location("mirror_guard_probe", repo / "scripts/tests/test_no_python_mirror.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def run(mod, label: str) -> None:
    print(f"== {label}")
    for name in TESTS:
        try:
            getattr(mod, name)()
            print(f"  {name}: pass")
        except AssertionError as exc:
            print(f"  {name}: FAIL {str(exc)[:600]}")


def main() -> int:
    repo = Path(sys.argv[1]).resolve()
    mod = load(repo)
    names = {s for _m, symbols, _r, _c in CANDIDATES for s in symbols}
    print(f"FROZEN_AT {mod.FROZEN_AT}; FROZEN_PAIRS {len(mod.FROZEN_PAIRS)}; ALLOWLIST rows {len(mod.ALLOWLIST)}")
    print("== definitions of the candidate names in SCOPE (ast.walk: functions and methods)")
    for path in mod.scanned_modules():
        hit = sorted(mod.defined_functions(path) & names)
        if hit:
            print(f"  {path.relative_to(repo).as_posix()}: {', '.join(hit)}")
    run(mod, "as committed")
    pairs = {(m, s) for m, symbols, _r, _c in CANDIDATES for s in symbols}
    mod.FROZEN_PAIRS = frozenset(mod.FROZEN_PAIRS | pairs)
    mod.RULE_SYMBOLS = frozenset(s for _m, s in mod.FROZEN_PAIRS) | {s for _m, s in mod.BINDINGS}
    run(mod, "pairs added to FROZEN_PAIRS only (in memory)")
    mod.ALLOWLIST = mod.ALLOWLIST + tuple((m, symbols, rule, cpp, "P10") for m, symbols, rule, cpp in CANDIDATES)
    run(mod, "pairs in FROZEN_PAIRS plus P10 ALLOWLIST rows (in memory)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
