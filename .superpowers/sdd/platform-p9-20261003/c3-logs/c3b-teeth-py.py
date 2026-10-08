"""C3b teeth (Python): each new refusal of lane C3b removed by one working-tree mutation, its own test run, the file
restored byte for byte (SHA-256 checked) before the next mutation. Synthetic fixtures only.

  python c3b-teeth-py.py --root C:/atx-wt/pool-15 --basetemp <dir>    (prints one block per mutation, then a table)
"""
from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import subprocess
import sys

T = "scripts/tests/test_wave_gross_match.py"
MUTATIONS = [  # (id, file, needle, replacement, the test node(s) that must FAIL)
    ("P1 preflight refusal off", "scripts/wave_stage_preflight.py",
     "    if retired:\n        problems.append(retired)\n", "    if False and retired:\n        problems.append(retired)\n",
     [f"{T}::test_pm6_6_is_refused_at_preflight_naming_the_engine"]),
    ("P2 run-stage refusal off", "scripts/wave_stage_cell.py",
     '    refuse_retired(w, "run")\n', "", [f"{T}::test_a_state_dir_older_than_the_retirement_is_refused[run]"]),
    ("P3 match-stage refusal off", "scripts/wave_stage_cell.py",
     '    refuse_retired(w, "match")\n', "", [f"{T}::test_a_state_dir_older_than_the_retirement_is_refused[match]"]),
    ("P4 match fail-closed off", "scripts/wave_stage_cell.py",
     "    if mode not in WR.GROSS_MATCH or WR.GROSS_MATCH[mode] is not None:",
     "    if False and (mode not in WR.GROSS_MATCH or WR.GROSS_MATCH[mode] is not None):",
     [f"{T}::test_match_fails_closed_on_a_mode_without_a_stage"]),
    ("P5 C2-9 missing-key refusal off", "scripts/wave_manifest.py",
     '    if m.get("gross_match") is None:\n        out.append(gross_match_missing(m))\n', "",
     [f"{T}::test_a_manifest_without_gross_match_is_refused_at_plan"]),
    ("P6 C2-10 digest re-check off", "scripts/wave_result.py",
     "    problems = cell_digest_problems(w, cell)\n    if problems:", "    problems = cell_digest_problems(w, cell)\n"
     "    if False and problems:",
     [f"{T}::test_a_cell_edited_during_judge_writes_no_wave_result[{c}]"
      for c in ("record-spec", "run-recipe", "none-cell")]),
]


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", type=Path, required=True)
    ap.add_argument("--basetemp", type=Path, required=True)
    ap.add_argument("--python", default=sys.executable)
    a = ap.parse_args()
    env = dict(os.environ, PYTHONDONTWRITEBYTECODE="1", PYTHONHASHSEED="0")
    a.basetemp.mkdir(parents=True, exist_ok=True)
    rows = []
    for k, (mid, rel, needle, repl, nodes) in enumerate(MUTATIONS, 1):
        path = a.root / rel
        orig = path.read_bytes()
        text = orig.decode("utf-8")
        nl = "\r\n" if "\r\n" in text else "\n"        # the checkout's own newline (core.autocrlf)
        needle, repl = needle.replace("\n", nl), repl.replace("\n", nl)
        assert text.count(needle) == 1, (mid, text.count(needle))
        path.write_bytes(text.replace(needle, repl).encode("utf-8"))
        try:
            done = subprocess.run([a.python, "-m", "pytest", "-q", "-p", "no:cacheprovider", "--basetemp",
                                   str(a.basetemp / f"m{k}"), "--tb=line", *nodes], cwd=a.root, env=env,
                                  capture_output=True, text=True)
        finally:
            path.write_bytes(orig)
        assert sha(path.read_bytes()) == sha(orig), mid
        tail = [x for x in done.stdout.splitlines() if x.strip()][-1]
        print(f"== {mid} ({rel}): pytest exit {done.returncode}: {tail}")
        for line in done.stdout.splitlines():
            if line.startswith(("E ", "FAILED", "C:\\", "scripts")) or ".py:" in line:
                print(f"   {line[:300]}")
        rows.append((mid, done.returncode, tail))
    print("\n| mutation | pytest exit | result |\n|---|---|---|")
    for mid, code, tail in rows:
        print(f"| {mid} | {code} | {tail} |")
    caught = all(code == 1 and "failed" in tail and "error" not in tail for _m, code, tail in rows)   # a FAIL, not
    #                                                                                                  a broken run
    print(f"RESULT {'PASS: every mutation caught by its own test' if caught else 'FAIL'}; files restored "
          "(SHA-256 checked)")
    return 0 if caught else 1


if __name__ == "__main__":
    sys.exit(main())
