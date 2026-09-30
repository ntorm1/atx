"""Resume of a done NAV output (platform v8 review W1-C, C-13): scored only when made from the current spec.

Run: "C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider scripts/tests/test_cycle_resume.py

Fake runner / tools in a temporary root (test_research_cycle.make_root); nothing real is run.
"""
from __future__ import annotations

import json
from pathlib import Path
import sys

import pytest

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
import cycle_resume as CR  # noqa: E402
import research_cycle as RC  # noqa: E402
from test_research_cycle import calls, cycle_of, make_root, run  # noqa: E402


def nav_step(root: Path, sp: Path):
    return next(s for s in cycle_of(root, sp).steps() if s.phase == "nav")


def test_a_done_nav_made_under_another_spec_is_refused(tmp_path):
    """The review's example: the spec changes (a re-lock, any edit) after the NAV ran, output names unchanged; resume
    used to score the old cell under the new spec_sha256. Now the NAV run's binding names the spec it was made from,
    and a mismatch stops before summ with both digests."""
    root, sp = make_root(tmp_path)
    log: list[str] = []
    assert run(root, sp, log) == RC.EXIT_OK
    binding = json.loads((root / "out" / "N-run" / CR.BINDING).read_text())
    st = nav_step(root, sp)
    assert binding == {"schema": CR.SCHEMA, "output": "out/N", "spec_sha256": RC.sha256_file(sp),
                       "argv_sha256": CR.argv_digest(CR.step_args(st))}
    n = len(calls(root))
    log.clear()
    assert run(root, sp, log) == RC.EXIT_OK                                          # same spec: resumes
    assert f"   binding: spec sha256 {RC.sha256_file(sp)}" in log and calls(root)[n:] == ["check", calls(root)[-1]]
    old = RC.sha256_file(sp)
    spec = json.loads(sp.read_text())
    spec["summ"]["dsr_n"] = 30                                                       # re-locked / edited later
    sp.write_text(json.dumps(spec))
    n = len(calls(root))
    with pytest.raises(RC.CycleError) as e:
        run(root, sp)
    assert e.value.code == RC.EXIT_PIN and "HARD-STOP [nav]" in str(e.value)
    assert f"spec sha256 {old}" in str(e.value) and f"current spec is sha256 {RC.sha256_file(sp)}" in str(e.value)
    assert not any(c.startswith("summ ") for c in calls(root)[n:])                   # the stale cell is not scored
    assert run(root, sp, suffix="r2") == RC.EXIT_OK                                  # a fresh --suffix re-runs it


def test_a_nav_without_a_binding_is_checked_on_its_argv(tmp_path):
    """An output made before the binding existed: its receipt's command must be the NAV command this spec would run
    (every pin in it); a receipt without it, a failed receipt or another command is refused with both digests."""
    root, sp = make_root(tmp_path)
    assert run(root, sp) == RC.EXIT_OK
    (root / "out" / "N-run" / CR.BINDING).unlink()
    receipt_path = root / "out" / "N-run" / "receipt.json"
    receipt = json.loads(receipt_path.read_text())
    st = nav_step(root, sp)
    k = st.argv.index("--")
    with pytest.raises(RC.CycleError, match="no cycle binding and no completed receipt with its command"):
        run(root, sp)                                                                # the fake receipt has none
    receipt_path.write_text(json.dumps(dict(receipt, command=[str(root / st.argv[k + 1])] + st.argv[k + 2:])))
    log: list[str] = []
    assert run(root, sp, log) == RC.EXIT_OK and any("binding: argv sha256" in x and "receipt.json" in x for x in log)
    changed = list(st.argv[k + 2:])
    changed[changed.index("--aim-leverage") + 1] = "1.5"                             # another NAV command
    receipt_path.write_text(json.dumps(dict(receipt, command=[st.argv[k + 1]] + changed)))
    with pytest.raises(RC.CycleError) as e:
        run(root, sp)
    assert e.value.code == RC.EXIT_PIN and CR.argv_digest(changed) in str(e.value)
    assert CR.argv_digest(st.argv[k + 2:]) in str(e.value)
    receipt_path.write_text(json.dumps(dict(receipt, command=[st.argv[k + 1]] + st.argv[k + 2:],
                                            outcome="rss-limit")))
    with pytest.raises(RC.CycleError, match="no completed receipt"):
        run(root, sp)


def test_a_binding_without_a_spec_digest_uses_its_argv(tmp_path):
    """A cycle built without a spec file records only the argv digest; resume compares that."""
    root, sp = make_root(tmp_path)
    spec = RC.load_spec(sp)
    c = RC.Cycle(spec, RC.Resolver(root))
    assert RC.run_cycle(c, log=lambda s: None, clean=lambda r: True) == RC.EXIT_OK
    binding = json.loads((root / "out" / "N-run" / CR.BINDING).read_text())
    assert binding["spec_sha256"] is None
    log: list[str] = []
    assert RC.run_cycle(RC.Cycle(spec, RC.Resolver(root)), log=log.append, clean=lambda r: True) == RC.EXIT_OK
    assert any(x.startswith("   binding: argv sha256") for x in log)
    spec["nav"]["flags"] = ["--aim-leverage", "{leverage}", "--cadence", "2"]
    with pytest.raises(RC.CycleError, match="was made by a command with argv sha256"):
        RC.run_cycle(RC.Cycle(spec, RC.Resolver(root)), log=lambda s: None, clean=lambda r: True)
