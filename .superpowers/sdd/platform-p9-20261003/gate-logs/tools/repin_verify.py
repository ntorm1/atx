"""GATE agent 4, step 6 (W1-I2 (b), B2-10 (b), B2-11, EXE-FREEZE): `verify` on the pinned lineage, read-only. For each
spec named, the Cycle the CLI builds (`research_cycle.py status`'s, scripts/cycle/cli.py make_cycle) and, for every step,
the checks a `run` makes before it reuses a done output (scripts/cycle/cli.py:418-434): K-P9-10 argv
(`cycle_resume.check_receipt_argv`, bounded steps) and E1-REUSE-a2 exe pin (`cycle_resume.check_receipt_exe`), plus
W1-I2 (a)'s side of a rule cell's paired parent NAV (`cycle_resume.completed_exe_sha256` of the NAV output against the
spec's resolved `exes_sha256.nav`, wave_stage_record.exe_problem) and B2-11's `exes.eval` key + pin. Every pin line of
the header (`[locked, verified]` or a refusal) is printed as the CLI prints it. Runs nothing, writes nothing.

usage: python repin_verify.py SPEC [SPEC ...]   (cwd = the pool root)
"""
from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path.cwd()
sys.path.insert(0, str(ROOT / "scripts"))
import cycle_resume as CR  # noqa: E402
from cycle.cli import header, make_cycle, step_key  # noqa: E402
from cycle.resolver import Resolver  # noqa: E402
from cycle.spec import EXES_PIN, CycleError, effective_exes  # noqa: E402

MAX_ATTEMPTS = 9


def verify(spec: str) -> int:
    print(f"\n=== {spec}")
    try:
        cycle = make_cycle(Resolver(ROOT), spec_path=ROOT / spec)
        for line in header(cycle):
            if line.startswith("# pin") or line.startswith("# template") or line.startswith("# research_cycle"):
                print(line[:400])
        steps = cycle.steps()
    except CycleError as exc:
        print(f"REFUSED (the cycle cannot be built): {exc}")
        return 1
    s = cycle.spec
    exes, pins = effective_exes(s), s.get(EXES_PIN) or {}
    print(f"exes {exes}")
    print(f"exes_sha256 {pins}")
    bad = 0
    for k, rel in sorted(exes.items()):
        got = cycle.res.sha(rel)
        ok = pins.get(k) == got
        bad += not ok
        print(f"  exe {k}: {rel} disk {got} {'= pin' if ok else '!= pin ' + str(pins.get(k))}")
    if "eval" not in exes or "eval" not in pins:
        bad += 1
        print("  B2-11: exes.eval or exes_sha256.eval MISSING")
    else:
        print("  B2-11: exes.eval named and pinned")
    for st in steps:
        line = f"  {step_key(st):7s} {st.state:8s} {st.output or ''}"
        notes = []
        if st.done and st.kind == "bounded":
            try:
                if st.phase not in ("nav", "ref"):
                    how = CR.check_receipt_argv(cycle, st)
                    notes.append(f"argv: {how or 'receipt predates K-P9-10 (reused as before)'}")
                exe, note = CR.check_receipt_exe(cycle, st)
                notes.append(f"exe: {exe or 'receipt predates K-P9-10 or names no exe (reused as before)'}"
                             + (f" NOTE {note}" if note else ""))
            except CR.ResumeError as exc:
                bad += 1
                notes.append(f"REFUSED: {exc}")
            if st.phase == "nav":
                try:
                    notes.append(f"C-13 binding (informational): {CR.check_binding(cycle, st)}")
                except CR.ResumeError as exc:
                    notes.append(f"C-13 binding (informational, not counted): {exc}")
                made = CR.completed_exe_sha256(cycle.res, st.output, MAX_ATTEMPTS)
                ok = made is not None and made == pins.get("nav")
                bad += not ok
                notes.append(f"W1-I2 (a): NAV made by {made} {'= exes_sha256.nav' if ok else '!= ' + str(pins.get('nav'))}")
        print(line + ("" if not notes else " | " + " | ".join(notes)))
    print(f"--- {spec}: {'HOLDS' if bad == 0 else f'{bad} problem(s)'}")
    return 1 if bad else 0


def parent_nav(spec: str) -> int:
    """W1-I2 (a)'s parent side as the wave reads it (wave_context.Wave.outputs: Cycle(..., verify=False), so a parent
    whose own compare inputs are gone still resolves): the NAV output of the parent spec ``spec`` and the exe its last
    completed receipt names, against the spec's exes_sha256.nav (a rule cell over this parent pins the same exes)."""
    import research_cycle as RC  # noqa: PLC0415
    s = RC.load_spec(ROOT / spec)
    c = RC.Cycle(s, Resolver(ROOT), verify=False)
    nav = c.out(s["nav"]["output"])
    made = CR.completed_exe_sha256(c.res, nav, MAX_ATTEMPTS)
    pin = (s.get(EXES_PIN) or {}).get("nav")
    ok = made is not None and made == pin
    print(f"\n=== parent {spec} (verify=False, the wave's view): NAV {nav} made by {made} "
          f"{'= exes_sha256.nav' if ok else '!= ' + str(pin)}; exes_sha256 {s.get(EXES_PIN)}; exes {s.get('exes')}")
    return 0 if ok else 1


def fields_view(spec: str) -> int:
    """(agent 4, after REPIN-R2) the as-built fields check a `run` makes on a done fields step
    (scripts/cycle/gate.py fields_check: the pinned manifest's rows must equal fields.list), on the spec's Cycle with
    verify=False (the wave's view; a spec whose own compare inputs are gone still resolves); informational."""
    import research_cycle as RC  # noqa: PLC0415
    from cycle.gate import fields_check  # noqa: PLC0415
    s = RC.load_spec(ROOT / spec)
    c = RC.Cycle(s, Resolver(ROOT), verify=False)
    f = s["fields"]
    fdm = f"{f['output']}/manifest.json"
    print(f"\n=== fields {spec} (verify=False): output {f['output']} pin {f.get('manifest_sha256')} disk "
          f"{c.res.sha(fdm)} list {len(f.get('list') or [])} names; NAV output {c.out(s['nav']['output'])}")
    try:
        fields_check(c, fdm, log=lambda line: print("  " + line))
        print("  fields check: holds")
        return 0
    except RC.CycleError as exc:
        print(f"  fields check REFUSES: {exc}")
        return 1


def main(argv: list[str]) -> int:
    rc = 0
    for s in argv:
        if s.startswith("--fields="):
            rc = max(rc, fields_view(s[len("--fields="):]))
            continue
        rc = max(rc, parent_nav(s[len("--parent="):]) if s.startswith("--parent=") else verify(s))
    return rc


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
