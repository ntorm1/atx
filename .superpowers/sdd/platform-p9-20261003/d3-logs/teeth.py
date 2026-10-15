"""Lane D3 part A teeth (r6-wave3-common "every new refusal gets a test with teeth"): each mutation below removes or
weakens one check in the working tree, runs the tests that pin it, and restores the file's original bytes (finally),
then asserts `git status --porcelain` is empty. A mutation is CAUGHT when its tests report >= 1 failure.

  python teeth.py OUT_LOG BASETEMP_DIR      (run from anywhere; the worktree is this file's repo)
"""
from __future__ import annotations

import os
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[4]
PY = sys.executable
TESTS = "scripts/tests/test_wave_kinds.py"
WK, SK = "scripts/wave_kinds.py", "scripts/wave_stage_kinds.py"

MUTATIONS = [  # (id, what the mutation removes, file, old, new, pytest -k)
    ("M1", "C3-3: aliases owned (table_flags)", WK,
     '    out.setdefault("nav", set()).update(flag_aliases(tables))\n', "",
     "c3_3_an_alias_is_owned"),
    ("M2", "C3-3: alias_problems called by the guard", SK,
     "    problems += WK.alias_problems(tables)", "    pass",
     "c3_3_a_malformed_alias_field"),
    ("M3", "C3-3: malformed x-flag-aliases shape", WK,
     "        if not (isinstance(aliases, list) and aliases and all(_alias(a) for a in aliases) and\n"
     "                len(set(aliases)) == len(aliases)):",
     "        if not isinstance(aliases, list):",
     "c3_3_a_malformed_alias_field"),
    ("M4", "C3-3: an alias that is a rule's own flag", WK,
     "            if a in flags:", "            if False:",
     "c3_3_a_malformed_alias_field"),
    ("M5", "C3-3: an alias only on a constant '--flag VALUE' row", WK,
     "        tokens = row.get(\"x-flag\").split() if isinstance(row.get(\"x-flag\"), str) else []\n"
     "        if len(tokens) != 2 or PLACEHOLDER_RE.fullmatch(tokens[1]):",
     "        tokens = row.get(\"x-flag\").split() if isinstance(row.get(\"x-flag\"), str) else []\n"
     "        if False:",
     "c3_3_an_alias_stands_for_a_constant_pair"),
    ("M6", "C3-3: two rows sharing an alias", WK,
     "            if a in seen and seen[a] != row.get(\"id\"):", "            if False:",
     "c3_3_two_rows_may_not_share"),
    ("M7", "C3-3: alias_beside_rule called by the guard", SK,
     "    problems += alias_beside_rule(w, cell, tables, derived)", "    pass",
     "c3_3_an_alias_is_owned"),
    ("M8", "C3-3: alias_beside_rule reads the ref side", SK,
     '    ref_flags = (ref_side(cell) or {}).get("flags")\n    if isinstance(ref_flags, list):\n'
     '        lists[f"{WK.REF_KEY}.flags"] = ref_flags',
     '    ref_flags = (ref_side(cell) or {}).get("flags")\n    if isinstance(ref_flags, list):\n        pass',
     "c3_3_an_alias_is_owned"),
    ("M9", "C3-3: an alias compared as a bare switch (reads)", SK,
     "    if option in switches:", "    if False:",
     "c3_3_an_inherited_alias_passes"),
    ("M10", "C3-3: the named row may change its own alias (alias_exempt)", SK,
     "    return {alias for alias, rid in WK.flag_aliases(tables).items() if rid in named}", "    return set()",
     "c3_3_an_inherited_alias_passes"),
    ("M11", "E2-17: inputs_refusal called by the guard", SK,
     "    problems += inputs_refusal(w, parent, cell, sets, template)", "    pass",
     "e2_17_inputs_only_by"),
    ("M12", "E2-17: the raw routes (inputs_routes)", SK,
     "    out: dict = {}\n    for r, ks in sets.items():\n        for key in ks:\n            if key == \"inputs\":",
     "    out: dict = {}\n    for r, ks in {}.items():\n        for key in ks:\n            if key == \"inputs\":",
     "e2_17_inputs_only_by"),
    ("M13", "E2-17: a resolved difference (inputs_changed)", SK,
     "    return sorted(out)\n\n\ndef inputs_refusal", "    return []\n\n\ndef inputs_refusal",
     "e2_17_a_resolved_difference"),
    ("M14", "E2-17: a role wave's own key (KIND_INPUTS)", WK,
     'KIND_INPUTS = {"field": (), "role": ("role",), "horizon": ()}',
     'KIND_INPUTS = {"field": (), "role": (), "horizon": ()}',
     "e2_17_a_role_wave or e2_17_a_resolved_difference"),
    ("M15", "walk-forward: the parent fits under --engine-fit", WK,
     "    if WALK_FORWARD_PARENT_FIT not in norm:", "    if False:",
     "walk_forward_refuses_a_parent"),
    ("M16", "walk-forward: --theme-tsmom / --theme-resid / --era refused", WK,
     "    bad = sorted({f for f in WALK_FORWARD_REFUSED if f in norm})", "    bad = []",
     "walk_forward_refuses_a_parent"),
    ("M17", "walk-forward: preflight calls the parent check", SK,
     "        problems += WK.walk_forward_parent_problems(spec)", "        pass",
     "walk_forward_refuses_a_parent"),
    ("M18", "walk-forward: its fitter flags owned (KIND_FLAGS)", WK,
     "    for section, flags in KIND_FLAGS.items():\n        out.setdefault(section, set()).update(flags)\n",
     "", "walk_forward_flags_are_the_kinds_own"),
    ("M19", "walk-forward: the block checks", WK,
     "        return [\"walk_forward must be {lag: >= 2", "        return []\n        return [\"walk_forward must be {lag: >= 2",
     "walk_forward_block_refusals"),
    ("M20", "walk-forward: lag >= 2 (K-P9-8)", WK,
     " and wf[\"lag\"] >= 2 and", " and",
     "walk_forward_block_refusals"),
    ("M21", "walk-forward out of SCHEMA_ONLY (the flip)", WK,
     "SCHEMA_ONLY: tuple = ()", "SCHEMA_ONLY: tuple = (\"walk-forward\",)",
     "walk_forward_runs_to_spec or ten_fixtures or c3s_rows"),
    ("M22", "walk-forward: card and monitor renamed", WK,
     '(("fit", "output"), ("card", "output"), ("ic", "w_output")', '(("fit", "output"), ("ic", "w_output")',
     "walk_forward_runs_to_spec"),
]


def summary(text: str) -> tuple[int, int, int]:
    """(failed, passed, errors) of pytest's last summary line; an error (a broken fixture) is never a catch."""
    found = [re.search(rf"(\d+) {word}", text) for word in ("failed", "passed", "error")]
    return tuple(int(m.group(1)) if m else 0 for m in found)


def main(out_log: str, basetemp: str) -> int:
    env = dict(os.environ, PYTHONDONTWRITEBYTECODE="1", PYTHONHASHSEED="0")
    env.pop("ATX_EQUITY_BIN", None)
    Path(basetemp).mkdir(parents=True, exist_ok=True)       # pytest makes --basetemp itself, not its parent
    lines, missed = [], []
    status = subprocess.run(["git", "status", "--porcelain"], cwd=ROOT, capture_output=True, text=True).stdout
    assert status == "", f"tree not clean before the run:\n{status}"
    head = subprocess.run(["git", "rev-parse", "HEAD"], cwd=ROOT, capture_output=True, text=True).stdout.strip()
    lines.append(f"teeth at {head} (worktree {ROOT}); each row: id, check removed, tests run (-k), failed / passed")
    union = " or ".join(f"({m[5]})" for m in MUTATIONS)            # control: every selection green unmutated
    done = subprocess.run([PY, "-m", "pytest", "-q", "-p", "no:cacheprovider", "--basetemp", f"{basetemp}/M0",
                           TESTS, "-k", union], cwd=ROOT, env=env, capture_output=True, text=True, timeout=3600)
    failed, passed, errors = summary(done.stdout)
    lines.append(f"M0  control, no mutation, every selection below: {failed} failed / {passed} passed / {errors} errors")
    assert failed == 0 and errors == 0 and passed > 0, done.stdout[-3000:]
    for mid, what, rel, old, new, sel in MUTATIONS:
        path = ROOT / rel
        orig = path.read_bytes()
        text = orig.decode("utf-8")
        if "\r\n" in text:                                   # core.autocrlf: the working tree has CRLF
            old, new = old.replace("\n", "\r\n"), new.replace("\n", "\r\n")
        assert text.count(old) == 1, f"{mid}: the mutation's text occurs {text.count(old)} times in {rel}"
        try:
            path.write_bytes(text.replace(old, new).encode("utf-8"))
            done = subprocess.run([PY, "-m", "pytest", "-q", "-p", "no:cacheprovider", "--basetemp",
                                   f"{basetemp}/{mid}", TESTS, "-k", sel], cwd=ROOT, env=env, capture_output=True,
                                  text=True, timeout=1800)
        finally:
            path.write_bytes(orig)
        failed, passed, errors = summary(done.stdout)
        caught = failed > 0 and errors == 0
        if not caught:
            missed.append(mid)
        lines.append(f"{mid}  {'CAUGHT' if caught else 'NOT CAUGHT'}  {what}  [{rel}; -k \"{sel}\"]  "
                     f"{failed} failed / {passed} passed / {errors} errors")
        status = subprocess.run(["git", "status", "--porcelain"], cwd=ROOT, capture_output=True, text=True).stdout
        assert status == "", f"{mid}: tree not clean after the revert:\n{status}"
    lines.append(f"RESULT {'all caught' if not missed else 'NOT CAUGHT: ' + ', '.join(missed)}; tree clean after "
                 "every revert (git status --porcelain empty)")
    Path(out_log).write_text("\n".join(lines) + "\n", encoding="utf-8")
    print("\n".join(lines))
    return 1 if missed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1], sys.argv[2]))
