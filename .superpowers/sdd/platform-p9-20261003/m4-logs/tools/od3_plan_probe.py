"""Root M4 (RESUME 10), ABS review item 6 / OD3P re-review 1 item 6 at OD3P's merge: the cycle's plan of a
fit.carry.drop spec naming scripts/specs/p9/od3/flagged_members.json's shape (the lane test's drop_root fake world:
history era E1, TRAIN anchor E2) with
  (1) a fake IC exe lacking capability absent-members-v1  -> refused (exit 2, before any step; the message names the
      capability, the history roles, OD3P-1 and "not read: no producer"; no out dir),
  (2) a fake IC exe offering it                            -> plans; each history role's u / w pass ends
      --absent-members <list> --absent-members-sha256 <pin> after any --candidate-cache / --theme-registry; the TRAIN
      anchor's passes carry nothing,
  (3) root's real IC exe (argv[1]) behind the same .cmd shim -> as (2).
Fakes only (no data, no statistic). usage (cwd = pool root): od3_plan_probe.py REAL_IC_EXE TMPDIR
"""
from __future__ import annotations

import contextlib
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import sys

REPO = Path.cwd()
spec_t = importlib.util.spec_from_file_location("t_od3", REPO / "scripts/tests/test_research_cycle_od3.py")
T = importlib.util.module_from_spec(spec_t)
sys.path.insert(0, str(REPO / "scripts" / "tests"))
spec_t.loader.exec_module(T)
RC, RR, TR = T.RC, T.RR, T.TR

real_exe = Path(sys.argv[1])
tmp = Path(sys.argv[2])
print("real exe", real_exe.as_posix(), "sha256", hashlib.sha256(real_exe.read_bytes()).hexdigest())
ok_all = True


def check(label: str, cond: bool) -> None:
    global ok_all
    ok_all &= bool(cond)
    print(f"  {label}: {bool(cond)}")


def world(name: str, shim: str | None) -> tuple[Path, Path]:
    root, sp, spec = T.drop_root(tmp / name)
    if shim == "fake-without":
        (root / "bin" / "fake_ic.py").write_text(T.FAKE_IC)
        (root / "bin" / "ic.cmd").write_text(f'@"{sys.executable}" "%~dp0fake_ic.py" %*\n')
    elif shim == "fake-with":
        (root / "bin" / "fake_ic_OFFER.py").write_text(T.FAKE_IC)
        (root / "bin" / "ic.cmd").write_text(f'@"{sys.executable}" "%~dp0fake_ic_OFFER.py" %*\n')
    else:
        (root / "bin" / "ic.cmd").write_text(f'@"{real_exe}" %*\n')
    spec["exes"]["ic"] = "bin/ic.cmd"
    sp.write_text(json.dumps(spec))
    return root, sp


def planned(label: str, shim: str) -> None:
    root, sp = world(label, shim)
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        code = RC.main(["plan", str(sp), "--root", str(root)])
    print(f"[{label}] plan exit {code}")
    check("plan exit 0", code == 0)
    pin = RC.sha256_file(root / T.LIST)
    steps = TR.cycle_of(root, sp).steps()
    by = {(s.phase, s.role): s for s in steps}
    for phase in RR.ABSENT_PHASES:
        argv = by[(phase, "E1")].argv
        tail = argv[-4:] == [RR.ABSENT_FLAG, T.LIST, RR.ABSENT_SHA_FLAG, pin]
        after = all(argv.index(f) < len(argv) - 4 for f in ("--candidate-cache", "--theme-registry") if f in argv)
        present = [f for f in ("--candidate-cache", "--theme-registry") if f in argv]
        check(f"E1 {phase}: ends --absent-members <list> --absent-members-sha256 <pin>", tail)
        check(f"E1 {phase}: flags after {present or 'no cache / registry flag in this fake argv'}", after)
        anchor = by[(phase, "E2")].argv
        check(f"E2 (TRAIN anchor) {phase}: no absent flag, no list", RR.ABSENT_FLAG not in anchor and
              T.LIST not in anchor)
    check("no out dir written by plan", not (root / "out").exists())


root, sp = world("fake-without", "fake-without")
out, err = io.StringIO(), io.StringIO()
with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
    code = RC.main(["plan", str(sp), "--root", str(root)])
msg = out.getvalue() + err.getvalue()
print(f"[fake-without] plan exit {code}")
check("exit 2 (EXIT_USAGE)", code == RC.EXIT_USAGE == 2)
for needle in (RR.ABSENT_CAPABILITY, "history roles E1", "OD3P-1", "not read: no producer", "nothing runs"):
    check(f"message names {needle!r}", needle in msg)
check("no out dir (before any step)", not (root / "out").exists())
planned("fake-with", "fake-with")
planned("real-exe", "real")
print("RESULT", "PASS" if ok_all else "FAIL")
sys.exit(0 if ok_all else 1)
