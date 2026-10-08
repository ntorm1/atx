"""SIGN teeth (Python, by file; TEETH-RESTORE): each new Python refusal of lane SIGN removed by one working-tree
mutation, its own test node(s) run, the file restored byte for byte before the next mutation. After every restore:
the bytes equal the pre-mutation bytes, `git hash-object` equals HEAD's blob; at the end `git status --porcelain` is
empty. Never git checkout / restore / stash. Synthetic fixtures only.

  python sign-teeth-py.py --root C:/atx-wt/pool-29 --basetemp <dir> --log <sprint>/sign-logs/sign-teeth-py.log
"""
from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import subprocess
import sys

T = "scripts/tests/test_wave_sign_verb.py"
R = f"{T}::test_a_receipt_that_is_not_the_verbs_answer_stops_the_screen"
MUTATIONS = [  # (id, file, needle, replacement, nodes that must FAIL)
    ("P1 receipt schema check off", "scripts/wave_rules.py",
     '    if not isinstance(receipt, dict) or receipt.get("schema") != SIGN_RECEIPT:\n',
     "    if False:\n", [f"{R}[schema]"]),
    ("P2 receipt rule / rows SHA-256 check off", "scripts/wave_rules.py",
     '    if receipt.get("rule") != rule or receipt.get("rows_sha256") != rows_sha256:\n', "    if False:\n",
     [f"{R}[rule]", f"{R}[rows sha]"]),
    ("P3 receipt row count check off", "scripts/wave_rules.py",
     "    if not isinstance(got, list) or len(got) != len(rows):\n", "    if not isinstance(got, list):\n",
     [f"{R}[a row missing]"]),
    ("P4 receipt echo check off", "scripts/wave_rules.py",
     "        if not isinstance(row, dict) or any(row.get(k) != want.get(k) for k in SIGN_ECHO):\n",
     "        if not isinstance(row, dict):\n", [f"{R}[rows reordered]", f"{R}[an echo changed]"]),
    ("P5 receipt verdict check off", "scripts/wave_rules.py",
     '        if row.get("verdict") not in (KEEP, DROP) or not isinstance(row.get("reason"), str):\n',
     "        if False:\n", [f"{R}[an unknown verdict]"]),
    ("P6 receipt kept / dropped check off", "scripts/wave_rules.py",
     '    if receipt.get("kept") != kept or receipt.get("dropped") != dropped:\n', "    if False:\n",
     [f"{R}[kept inconsistent]", f"{R}[a flipped verdict]"]),
    ("P7 missing admission row not refused", "scripts/wave_rules.py",
     "    missing = [c[\"id\"] for c in candidates if c[\"id\"] not in by_id]\n    if missing:\n",
     "    missing = [c[\"id\"] for c in candidates if c[\"id\"] not in by_id]\n    if False:\n",
     [f"{T}::test_a_string_without_an_admission_row_stops_the_screen_before_the_verb"]),
    ("P8 IC exe pin check off", "scripts/wave_stage_library.py",
     "    if pin is not None and now != pin:\n", "    if False:\n", [f"{T}::test_the_ic_exe_must_be_its_pin"]),
    ("P9 spec stage receipt pin off", "scripts/wave_stage_library.py",
     '    if sc.get("sign"):       # P9 lane SIGN', '    if False:       # P9 lane SIGN',
     [f"{T}::test_the_spec_stage_refuses_a_receipt_changed_after_the_screen"]),
    ("P10 the stage back on the Python rule", "scripts/wave_stage_library.py",
     '    sign = sign_verb(w, spec, m["sign_rule"], sign_in)          # the sign rule: the IC exe\'s sign verb (C++)\n'
     '    dec = sign.pop("decision")\n',
     '    sign = {}\n    dec = WR.screen_decision(m["sign_rule"], m["candidates"], sign_in)\n',
     [f"{T}::test_the_screen_stage_runs_the_sign_verb_once_and_records_its_receipt"]),
]


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def git(root: Path, *args: str) -> str:
    return subprocess.run(["git", *args], cwd=root, capture_output=True, text=True, check=True).stdout.strip()


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", type=Path, required=True)
    ap.add_argument("--basetemp", type=Path, required=True)
    ap.add_argument("--log", type=Path, required=True)
    ap.add_argument("--python", default=sys.executable)
    a = ap.parse_args()
    assert git(a.root, "status", "--porcelain") == "", "the tree must be clean before a teeth run"
    env = dict(os.environ, PYTHONDONTWRITEBYTECODE="1", PYTHONHASHSEED="0")
    a.basetemp.mkdir(parents=True, exist_ok=True)
    lines, rows = [], []
    for k, (mid, rel, needle, repl, nodes) in enumerate(MUTATIONS, 1):
        path = a.root / rel
        orig = path.read_bytes()
        text = orig.decode("utf-8")
        nl = "\r\n" if "\r\n" in text else "\n"
        needle, repl = needle.replace("\n", nl), repl.replace("\n", nl)
        assert text.count(needle) == 1, (mid, text.count(needle))
        try:
            path.write_bytes(text.replace(needle, repl).encode("utf-8"))
            done = subprocess.run([a.python, "-m", "pytest", "-q", "-p", "no:cacheprovider", "--basetemp",
                                   str(a.basetemp / f"m{k}"), "--tb=line", *nodes], cwd=a.root, env=env,
                                  capture_output=True, text=True)
        finally:
            path.write_bytes(orig)
        restored = sha(path.read_bytes()) == sha(orig)
        blob = git(a.root, "hash-object", rel) == git(a.root, "rev-parse", f"HEAD:{rel}")
        tail = [x for x in done.stdout.splitlines() if x.strip()][-1]
        lines.append(f"== {mid} ({rel}): pytest exit {done.returncode}: {tail}; restored {restored}, "
                     f"hash-object == HEAD blob {blob}")
        lines += [f"   {x[:300]}" for x in done.stdout.splitlines() if x.startswith(("E ", "FAILED")) or ".py:" in x]
        failed = done.stdout.count("FAILED ") if "FAILED " in done.stdout else 0
        rows.append((mid, done.returncode, tail, failed == len(nodes) and restored and blob))
        assert restored and blob, mid                     # a failed restore is a stop: nothing else runs
    clean = git(a.root, "status", "--porcelain") == ""
    lines.append("\n| mutation | pytest exit | result | every node failed, file restored |\n|---|---|---|---|")
    lines += [f"| {mid} | {code} | {tail} | {'yes' if ok else 'NO'} |" for mid, code, tail, ok in rows]
    caught = all(code == 1 and ok for _m, code, _t, ok in rows)
    lines.append(f"git status --porcelain empty: {clean}")
    lines.append(f"RESULT {'PASS: every mutation caught by its own test(s)' if caught and clean else 'FAIL'}")
    a.log.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print("\n".join(lines[-(len(rows) + 4):]))
    return 0 if caught and clean else 1


if __name__ == "__main__":
    sys.exit(main())
