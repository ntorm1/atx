"""Lane D3, ruling C3-13 teeth (resume 8 round; r6-wave3-common "every new refusal gets a test with teeth"): each
mutation below removes or weakens one piece of the C3-13 derivation in the working tree, runs the tests that pin it,
restores the file's original bytes (finally), then asserts `git status --porcelain` is empty. A mutation is CAUGHT
when its tests report >= 1 failure. Same harness as part A's teeth.py (this dir).
Ruling TEETH-RESTORE (resume 9): after every restore it also asserts the file's SHA-256 equals its pre-mutation SHA-256
and `git hash-object <file>` equals HEAD's blob id (git's own clean conversion, so core.autocrlf does not matter);
a failed restore stops the run (assert) with the mutation already undone by `finally`.

  python c3-13-teeth.py OUT_LOG BASETEMP_DIR      (run from anywhere; the worktree is this file's repo)
"""
from __future__ import annotations

import hashlib
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
    ("N1", "C3-13: nav_rule_split splits nothing (the --rule row appended to nav.flags, as before the fix)", WK,
     "    if NAV_RULE_FLAG not in nav:\n        return {}, derived\n",
     "    if True:\n        return {}, derived\n",
     "c3_13"),
    ("N2", "C3-13: the row's param x-flags dropped (only nav.rule set)", WK,
     "    if nav:\n        flags[\"nav\"] = nav\n", "",
     "c3_13_nav_rule_split or c3_13_a_rule_row_sets_nav_rule"),
    ("N3", "C3-13: kind_constants does not split (the spec stage writes the --rule row into nav.flags)", SK,
     "        return WK.nav_rule_split(derived)\n", "        return {}, derived\n",
     "c3_13_a_rule_row_sets_nav_rule"),
    ("N4", "C3-13: the E2-8 guard's base not split (the guard refuses the fixed cell)", SK,
     "        cell_ops = WK.nav_rule_split(derived)[1]", "        cell_ops = derived",
     "c3_13_a_rule_row_sets_nav_rule"),
    ("N5", "C3-13: an over-broad split (another row kind's flag taken as nav.rule)", WK,
     "    if NAV_RULE_FLAG not in nav:\n        return {}, derived\n    value = nav.pop(NAV_RULE_FLAG)\n",
     "    if not nav:\n        return {}, derived\n"
     "    value = nav.pop(NAV_RULE_FLAG) if NAV_RULE_FLAG in nav else nav.pop(next(iter(nav)))\n",
     "c3_13_other_row_kinds or c3_13_nav_rule_split"),
]


def summary(text: str) -> tuple[int, int, int]:
    """(failed, passed, errors) of pytest's last summary line; an error (a broken fixture) is never a catch."""
    found = [re.search(rf"(\d+) {word}", text) for word in ("failed", "passed", "error")]
    return tuple(int(m.group(1)) if m else 0 for m in found)


def blob_is_heads(rel: str) -> bool:
    """TEETH-RESTORE: the working file, through git's own clean conversion (core.autocrlf), is HEAD's blob."""
    def git(*args: str) -> str:
        return subprocess.run(["git", *args], cwd=ROOT, capture_output=True, text=True).stdout.strip()
    return git("hash-object", rel) == git("rev-parse", f"HEAD:{rel}") != ""


def main(out_log: str, basetemp: str) -> int:
    env = dict(os.environ, PYTHONDONTWRITEBYTECODE="1", PYTHONHASHSEED="0")
    env.pop("ATX_EQUITY_BIN", None)
    Path(basetemp).mkdir(parents=True, exist_ok=True)       # pytest makes --basetemp itself, not its parent
    lines, missed = [], []
    status = subprocess.run(["git", "status", "--porcelain"], cwd=ROOT, capture_output=True, text=True).stdout
    assert status == "", f"tree not clean before the run:\n{status}"
    head = subprocess.run(["git", "rev-parse", "HEAD"], cwd=ROOT, capture_output=True, text=True).stdout.strip()
    lines.append(f"C3-13 teeth at {head} (worktree {ROOT}); each row: id, what is removed, tests run (-k), "
                 "failed / passed")
    union = " or ".join(f"({m[5]})" for m in MUTATIONS)            # control: every selection green unmutated
    done = subprocess.run([PY, "-m", "pytest", "-q", "-p", "no:cacheprovider", "--basetemp", f"{basetemp}/N0",
                           TESTS, "-k", union], cwd=ROOT, env=env, capture_output=True, text=True, timeout=3600)
    failed, passed, errors = summary(done.stdout)
    lines.append(f"N0  control, no mutation, every selection below: {failed} failed / {passed} passed / {errors} errors")
    assert failed == 0 and errors == 0 and passed > 0, done.stdout[-3000:]
    for rel in sorted({m[2] for m in MUTATIONS}):
        assert blob_is_heads(rel), f"{rel} is not HEAD's blob before the run"
    for mid, what, rel, old, new, sel in MUTATIONS:
        path = ROOT / rel
        orig = path.read_bytes()
        before = hashlib.sha256(orig).hexdigest()
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
        after = hashlib.sha256(path.read_bytes()).hexdigest()       # TEETH-RESTORE: bytes back, HEAD's blob
        assert after == before, f"{mid}: {rel} SHA-256 {after} after the restore, {before} before the mutation"
        assert blob_is_heads(rel), f"{mid}: {rel} is not HEAD's blob after the restore"
        failed, passed, errors = summary(done.stdout)
        caught = failed > 0 and errors == 0
        if not caught:
            missed.append(mid)
        lines.append(f"{mid}  {'CAUGHT' if caught else 'NOT CAUGHT'}  {what}  [{rel}; -k \"{sel}\"]  "
                     f"{failed} failed / {passed} passed / {errors} errors  (restored: sha256 {after[:12]} = "
                     "pre-mutation; blob = HEAD's)")
        status = subprocess.run(["git", "status", "--porcelain"], cwd=ROOT, capture_output=True, text=True).stdout
        assert status == "", f"{mid}: tree not clean after the revert:\n{status}"
    lines.append(f"RESULT {'all caught' if not missed else 'NOT CAUGHT: ' + ', '.join(missed)}; tree clean after "
                 "every revert (git status --porcelain empty; SHA-256 = pre-mutation; git hash-object = HEAD's blob)")
    Path(out_log).write_text("\n".join(lines) + "\n", encoding="utf-8")
    print("\n".join(lines))
    return 1 if missed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1], sys.argv[2]))
