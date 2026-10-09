"""OD3P fix round 1, R-1 probe on the lane's fakes (synthetic; no data, no exe).

Usage: python probe_r1.py <worktree> <tmpdir>

Cases (one fake root each; line 2 = Y-F0 carry without drop, line 4 = the clean-source carry with drop):
  old_b6     fit.output = the book's TRAIN fit dir (the dir holding the carried composition_weights.json), as the
             runbook's old B6 ("the book's own section") yields: the reviewer's failure.
  shared     lines 2 and 4 given the SAME fresh fit.output: line 2 runs first, then line 4 plans.
  fixed_b6   the fixed B6: each book its own fresh fit.output (out/od3-fit-yf0, out/od3-fit-clean).
"""
from __future__ import annotations

import json
from pathlib import Path
import shutil
import sys

wt, tmp = Path(sys.argv[1]), Path(sys.argv[2])
tests = wt / "scripts" / "tests"
for p in (tests.parent, tests / "fixtures", tests, wt / "atx-impl" / "tools"):
    sys.path.insert(0, str(p))
import research_cycle as RC  # noqa: E402
import test_research_cycle_od3 as T  # noqa: E402

TR = T.TR


def plan(root, sp):
    try:
        lines = RC.plan_lines(TR.cycle_of(root, sp, capabilities=T.ABSENT))
    except RC.CycleError as exc:
        return [f"REFUSED exit {exc.code}: {exc}"]
    return lines


def show_fit(lines):
    for k, line in enumerate(lines):
        if line.startswith("# phase fit"):
            print("   ", line[:160])
            argv = lines[k + 1] if k + 1 < len(lines) else ""
            toks = argv.split()
            if "--output" in toks:
                i = toks.index("--output")
                print("    carry argv tail:", " ".join(toks[i:]))
            return
    print("   ", lines[0][:300] if lines else "<no lines>")


def run(root, sp, stop_after="w"):
    log: list[str] = []
    try:
        code = TR.run(root, sp, log, capabilities=T.ABSENT, stop_after=stop_after)
        print(f"    run --stop-after {stop_after}: exit {code}")
    except RC.CycleError as exc:
        print(f"    run --stop-after {stop_after}: CycleError exit {exc.code}: {str(exc)[:160]}")
    print("    calls:", ", ".join(TR.calls(root)))


def two_specs(root: Path, spec: dict, out2: str, out4: str):
    """line 2 (no drop) and line 4 (drop) on one root."""
    s4 = json.loads(json.dumps(spec))
    s2 = json.loads(json.dumps(spec))
    s2["fit"]["carry"].pop("drop")
    s2["fit"]["output"], s4["fit"]["output"] = out2, out4
    s2["ic"]["u_output"], s2["ic"]["w_output"], s2["nav"]["output"] = "out/od3-u-yf0", "out/od3-w-yf0", "out/od3-nav-yf0"
    s4["ic"]["u_output"], s4["ic"]["w_output"], s4["nav"]["output"] = ("out/od3-u-clean", "out/od3-w-clean",
                                                                         "out/od3-nav-clean")
    p2, p4 = root.parent / "od3-yf0.json", root.parent / "od3-clean.json"
    p2.write_text(json.dumps(s2))
    p4.write_text(json.dumps(s4))
    return p2, p4


def case(name):
    d = tmp / name
    if d.exists():
        shutil.rmtree(d)
    d.mkdir(parents=True)
    root, sp, spec = T.drop_root(d)
    return root, sp, spec


print("== old_b6: fit.output = the TRAIN fit dir holding W (runbook B6 before the fix)")
root, sp, spec = case("old_b6")
spec["fit"]["output"] = "fit"                    # the book's TRAIN fit dir: holds composition_weights.json (= W)
sp.write_text(json.dumps(spec))
show_fit(plan(root, sp))
run(root, sp)
print("    carried era file written:", (root / "fit" / "composition_weights.E1.json").exists(),
      "| carry.json:", (root / "fit" / "carry.json").exists())

print("== shared: lines 2 and 4 with one fresh fit.output (out/od3-fit), line 2 run first")
root, sp, spec = case("shared")
p2, p4 = two_specs(root, spec, "out/od3-fit", "out/od3-fit")
print("  line 2 plan:")
show_fit(plan(root, p2))
run(root, p2)
print("  line 4 plan (after line 2 ran):")
show_fit(plan(root, p4))
rec = json.loads((root / "out/od3-fit/carry.json").read_text())
print("    carry.json drop record:", "drop" in rec)

print("== fixed_b6: each book its own fresh fit.output (out/od3-fit-yf0, out/od3-fit-clean)")
root, sp, spec = case("fixed_b6")
p2, p4 = two_specs(root, spec, "out/od3-fit-yf0", "out/od3-fit-clean")
for label, p in (("line 2", p2), ("line 4", p4)):
    print(f"  {label} plan:")
    show_fit(plan(root, p))
    run(root, p)
rec = json.loads((root / "out/od3-fit-clean/carry.json").read_text())
print("    line 4 carry.json drop record:", rec.get("drop", {}).get("members"), "| line 2 carry.json drop:",
      "drop" in json.loads((root / "out/od3-fit-yf0/carry.json").read_text()))
wt_e1 = [a for a in TR.argvs(root) if "--composition-weights" in a and "out/od3-w-clean-E1-1" in a]
print("    line 4 WT-E1 argv tail:", wt_e1[0][-4:-1] if wt_e1 else "<none>")
for label, p in (("line 2", p2), ("line 4", p4)):
    st = [ln for ln in RC.status_lines(TR.cycle_of(root, p, capabilities=T.ABSENT)) if ln.startswith("fit")]
    print(f"  {label} status after run:", st[0][:120] if st else "<none>")
