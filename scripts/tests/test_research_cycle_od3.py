"""research_roles.py for the OD-3 history read (v8-od3-protocol.md, gaps T1 and T4; P9 lane OD3P): fit.carry, per-era
label roles, the clean-source drop list (fit.carry.drop, PRE-31; OD3P-1: the IC exe's --absent-members on the history
roles' IC passes, refused without capability absent-members-v1) and the --risk-model refusal on history (PRE-10).

Run: "C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider scripts/tests/test_research_cycle_od3.py

The tiny_world eras and fake pipeline of test_research_cycle_roles.py are used. This file's fake runner runs the real
carry_weights.py for the fit phase of a fit.carry cycle, and fakes every other phase as that file's runner does.
The T1 / T4 tests are the xpre branch's (`f68058e2`); the T1 refusal of a spec without roles now sits in
scripts/cycle/cli.py make_cycle (E2's split of the cycle driver).
Identity: without fit.carry, label_dir and a --risk-model history book, plan, status, step argv and lock equal
research_roles.py at da3bb239 (blob 313b23b3, root's file before this lane).
"""
from __future__ import annotations

import datetime as dt
import importlib.util
import json
from pathlib import Path
import subprocess
import sys

import pytest

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
for p in (HERE.parent, HERE / "fixtures", HERE, REPO / "atx-impl" / "tools"):
    sys.path.insert(0, str(p))
import carry_weights as CW  # noqa: E402
import research_cycle as RC  # noqa: E402
import research_roles as RR  # noqa: E402
import test_research_cycle_roles as TR  # noqa: E402

ROLES_BLOB = "313b23b39f8e4dd843c335a0e09791ec5ff5d76e"   # research_roles.py at da3bb239 (root's at integration 8)
TOOL = (REPO / "atx-impl" / "tools" / "carry_weights.py").as_posix()

FAKE_RUNNER = r'''
import json, subprocess, sys
from pathlib import Path
args = sys.argv[1:]
k = args.index("--")
opts, cmd = args[:k], args[k + 1:]
out = Path(opts[opts.index("--output") + 1])
out.mkdir(parents=True, exist_ok=False)
role = opts[opts.index("--role-id") + 1] if "--role-id" in opts else None
with open("calls.log", "a") as f:
    f.write(out.name + "\n")
with open("argv.log", "a") as f:
    f.write(json.dumps(cmd) + "\n")
code = 0
if "carry_weights.py" in " ".join(cmd):          # the real tool, on the root's files
    code = subprocess.run([sys.executable, TOOL] + cmd[2:]).returncode
else:
    beh = json.loads(Path("behaviour.json").read_text())
    child = Path(cmd[cmd.index("--output") + 1])
    child.mkdir(parents=True, exist_ok=True)
    if "nav" in cmd:
        for name, text in beh["nav_files"][child.name].items():
            (child / name).write_text(text)
    else:
        (child / "summary.json").write_text(json.dumps({"status": "complete"}))
        (child / "orientations.json").write_text("{}")
        (child / "train_combined.json").write_text("{}")
doc = {"outcome": "completed" if code == 0 else "process-error", "exit_code": code, "wall_seconds": 1.5,
       "sampled_peak_tree_rss_bytes": 1 << 20}
if role is not None:
    doc["role_id"] = role
(out / "receipt.json").write_text(json.dumps(doc))
'''.replace("TOOL]", repr(TOOL) + "]")


def carry_root(tmp: Path, fitted_on: str = "era2") -> tuple[Path, Path, dict]:
    """The two-era tiny_world root with the real carry tool and a TRAIN fit of E2 (the anchor) to carry."""
    root, sp, _ = TR.two_era_root(tmp)
    (root / "scripts" / "runner.py").write_text(FAKE_RUNNER)
    doc = {"schema": "atx.dsl-composition-weights/v1", "library_sha256": RC.sha256_file(root / "lib/lib.json"),
           "train_manifest_sha256": RC.sha256_file(root / fitted_on / "manifest.json"), "weights": {"new_alpha": 1.0},
           "provenance": {"rule": "ew-theme-v1", "weighted_standalone_turnover": 0.05}}
    (root / "fit").mkdir()
    (root / "fit" / "composition_weights.json").write_bytes(CW.canonical_bytes(doc))
    (root / "fit" / "admission.json").write_text(json.dumps({"candidates": [{"id": "new_alpha", "status": "admitted",
                                                                             "sign_agrees": True}]}))
    spec = json.loads(sp.read_text())
    spec["fit"]["carry"] = {"weights": {"path": "fit/composition_weights.json", "sha256": None},
                            "admission": {"path": "fit/admission.json", "sha256": None}}
    spec.pop("gate")
    sp.write_text(json.dumps(spec))
    return root, sp, spec


# ------------------------------------------------------------------ T1: fit.carry
def test_carry_replaces_the_pooled_fit_and_feeds_every_era(tmp_path):
    root, sp, _ = carry_root(tmp_path)
    plan = RC.plan_lines(TR.cycle_of(root, sp))
    assert any(line.startswith("# pin carry:weights: fit/composition_weights.json") for line in plan)
    log: list[str] = []
    assert TR.run(root, sp, log) == RC.EXIT_OK, log[-5:]
    assert TR.calls(root) == ["fields F-E1", "fields F-E2", "check", "U-E1-run1", "U-E2-run1", "W-run1", "WT-E1-run1",
                              "WT-E2-run1", "N-E1-run", "N-E2-run"]
    carry = next(a for a in TR.argvs(root) if "carry_weights.py" in " ".join(a))
    e1, e2 = (RC.sha256_file(root / d / "manifest.json") for d in ("era1", "era2"))
    k = carry.index("--era")
    assert carry[k:k + 4] == ["--era", "E1", "era1/manifest.json", e1] and "--era-id" not in carry
    assert carry[-5:] == ["--anchor", "era2/manifest.json", e2, "--output", "out/W"]
    w = root / "out" / "W"
    assert (w / "composition_weights.json").read_bytes() == (root / "fit/composition_weights.json").read_bytes()
    carried = json.loads((w / "composition_weights.E1.json").read_text())
    assert carried["train_manifest_sha256"] == e1 and carried["carried_from"]["rule"] == CW.RULE
    wt = {a[a.index("--output") + 1]: a for a in TR.argvs(root) if "--composition-weights" in a}
    for out, name in (("out/WT-E1-1", "composition_weights.E1.json"), ("out/WT-E2-1", "composition_weights.json")):
        argv = wt[out]
        assert argv[argv.index("--composition-weights") + 1:argv.index("--composition-weights") + 4] == [
            f"out/W/{name}", "--composition-weights-sha256", RC.sha256_file(w / name)]
    status = RC.status_lines(TR.cycle_of(root, sp))
    assert any(line.startswith("fit") and "done" in line for line in status)


@pytest.mark.parametrize("case, code, needle", [
    ("one_role", RC.EXIT_USAGE, "two or more roles"),
    ("malformed", RC.EXIT_USAGE, "fit.carry needs weights and admission"),
    ("pin", RC.EXIT_PIN, "PIN MISMATCH fit.carry.weights"),
    ("missing", RC.EXIT_PIN, "fit.carry.admission missing"),
    ("not_anchor", RC.EXIT_PIN, "not on the anchor role"),
])
def test_carry_refusals(tmp_path, case, code, needle):
    root, sp, spec = carry_root(tmp_path, fitted_on="era1" if case == "not_anchor" else "era2")
    if case == "one_role":
        spec["roles"] = spec["roles"][1:]
    elif case == "malformed":
        spec["fit"]["carry"].pop("admission")
    elif case == "pin":
        spec["fit"]["carry"]["weights"]["sha256"] = "0" * 64
    elif case == "missing":
        (root / "fit" / "admission.json").unlink()
    with pytest.raises(RC.CycleError, match=needle) as e:
        RR.roles_cycle(spec, RC.Resolver(root), spec_path=sp)
    assert e.value.code == code
    sp.write_text(json.dumps(spec))
    assert RC.main(["plan", str(sp), "--root", str(root)]) == code


def test_carry_without_roles_is_refused(tmp_path):
    root, sp, spec = carry_root(tmp_path)
    spec.pop("roles")
    spec["inputs"]["role"] = {"dir": "era2", "path": "era2/manifest.json", "sha256": None, "universe": "u-v1"}
    sp.write_text(json.dumps(spec))
    with pytest.raises(RC.CycleError, match="fit.carry .* needs roles") as e:
        TR.cycle_of(root, sp)
    assert e.value.code == RC.EXIT_USAGE
    assert RC.main(["plan", str(sp), "--root", str(root)]) == RC.EXIT_USAGE      # cycle/cli.py make_cycle (OD3P)


def test_lock_fills_the_carry_and_label_pins(tmp_path):
    root, sp, spec = carry_root(tmp_path)
    labels(root, spec)
    sp.write_text(json.dumps(spec))
    assert RC.main(["lock", str(sp), "--root", str(root), "--write"]) == 0
    spec = json.loads(sp.read_text())
    assert spec["fit"]["carry"]["weights"]["sha256"] == RC.sha256_file(root / "fit/composition_weights.json")
    assert spec["fit"]["carry"]["admission"]["sha256"] == RC.sha256_file(root / "fit/admission.json")
    assert [r["label_manifest_sha256"] for r in spec["roles"]] == [RC.sha256_file(root / d / "manifest.json")
                                                                   for d in ("lab1", "lab2")]
    pins = TR.cycle_of(root, sp).pins
    assert pins["carry:weights"][2] == pins["label_role:E1"][2] == "locked, verified" and "label_role" not in pins


# ------------------------------------------------------------------ T4: per-era label roles
def labels(root: Path, spec: dict) -> None:
    for name, role in zip(("lab1", "lab2"), spec["roles"]):
        (root / name).mkdir()
        (root / name / "manifest.json").write_text(json.dumps({"label_role_of": role["id"]}))
        role["label_dir"] = name


def test_each_era_marks_its_nav_with_its_own_label_role(tmp_path):
    root, sp, _ = TR.two_era_root(tmp_path)
    spec = json.loads(sp.read_text())
    labels(root, spec)
    sp.write_text(json.dumps(spec))
    navs = {st.role: st.argv for st in TR.cycle_of(root, sp).steps() if st.phase == "nav"}
    for era, lab in (("E1", "lab1"), ("E2", "lab2")):
        k = navs[era].index("--label-role")
        assert navs[era][k:k + 4] == ["--label-role", f"{lab}/manifest.json", "--label-role-sha256",
                                      RC.sha256_file(root / lab / "manifest.json")]


@pytest.mark.parametrize("case, needle", [
    ("spec_level_with_two_roles", "inputs.label_role binds one role"),
    ("both", "name their own label_dir"),
    ("empty", "label_dir must be a non-empty string"),
])
def test_label_refusals(tmp_path, case, needle):
    root, sp, _ = TR.two_era_root(tmp_path)
    spec = json.loads(sp.read_text())
    (root / "lab").mkdir()
    (root / "lab" / "manifest.json").write_text("{}")
    if case in ("spec_level_with_two_roles", "both"):
        spec["inputs"]["label_role"] = {"dir": "lab", "path": "lab/manifest.json", "sha256": None}
    if case == "both":
        spec["roles"][0]["label_dir"] = "lab"
    if case == "empty":
        spec["roles"][0]["label_dir"] = ""
    with pytest.raises(RC.CycleError, match=needle) as e:
        RR.roles_cycle(spec, RC.Resolver(root), spec_path=sp)
    assert e.value.code == RC.EXIT_USAGE


def test_one_role_keeps_a_spec_level_label_role(tmp_path):
    _, _, sp_roles, _ = TR.one_role_pair(tmp_path, TR.TW.SCORE_BEGIN, TR.TW.DATES - 1)
    root = tmp_path / "roles"
    (root / "lab").mkdir()
    (root / "lab" / "manifest.json").write_text("{}")
    spec = json.loads(sp_roles.read_text())
    spec["inputs"]["label_role"] = {"dir": "lab", "path": "lab/manifest.json", "sha256": None}
    nav = next(st for st in RR.roles_cycle(spec, RC.Resolver(root), spec_path=sp_roles).steps() if st.phase == "nav")
    assert "--label-role" in nav.argv


# ------------------------------------------------------------------ OD3P: fit.carry.drop (PRE-20, PRE-31, OD3P-1)
DROP_LIST = {"schema": CW.DROP_SCHEMA, "source": {"path": "flags.md", "sha256": "33" * 32},
             "members": ["old_alpha"], "rules": ["theme-cov-hedge-v1", "mom-volman-v1"], "rules_pending": []}
ABSENT = frozenset({"list-rules", RR.ABSENT_CAPABILITY})          # an IC exe envelope that offers the list
ROLES_937 = "8081a6d0c598fe1fbb30a7405f6727fa0cddbdd7"            # research_roles.py at 937865f0 (pinned-0 design)
LIST = "fit/flagged_members.json"


def era_manifest(root: Path, name: str, begin: str, end: str) -> dict:
    """A role manifest scoring inside [begin, end) (plan and fake run only: the fake pipeline reads no data)."""
    doc = {"schema": "atx.recent-research-role/v1", "universe": {"id": "u-v1"}, "dates": 700, "score_begin": 400,
           "score_start_ns": TR.TW.session_ns(dt.date.fromisoformat(begin) + dt.timedelta(days=1)),
           "score_end_ns": TR.TW.session_ns(dt.date.fromisoformat(end))}
    (root / name).mkdir()
    (root / name / "manifest.json").write_text(json.dumps(doc))
    return {"id": name.upper(), "dir": name, "begin": begin, "end": end, "universe": "u-v1"}


def drop_root(tmp: Path, history: bool = True, drop: bool = True) -> tuple[Path, Path, dict]:
    """A carry cycle of a history era E1 ([2015, 2017), before TRAIN) and the anchor E2 (2020, inside TRAIN) whose
    W has two members, with fit.carry.drop naming one (history=False: carry_root's two TRAIN eras)."""
    if not history:
        root, sp, spec = carry_root(tmp)
    else:
        root = TR.make_root(tmp)
        (root / "scripts" / "runner.py").write_text(FAKE_RUNNER)
        (root / "behaviour.json").write_text(json.dumps({"nav_files": {}}))
        roles = [era_manifest(root, "e1", "2015-01-01", "2017-01-01"), era_manifest(root, "e2", "2020-01-01",
                                                                                     "2021-01-01")]
        spec = TR.base_spec(roles=roles)
        spec.pop("gate")
        spec["summ"].pop("ledger")                                    # a history pool: no ledger line here
        (root / "fit").mkdir()
        (root / "fit" / "admission.json").write_text(json.dumps({"candidates": []}))
        spec["fit"]["carry"] = {"weights": {"path": "fit/composition_weights.json", "sha256": None},
                                "admission": {"path": "fit/admission.json", "sha256": None}}
        sp = tmp / "spec.json"
    anchor = spec["roles"][-1]["dir"]
    doc = {"schema": "atx.dsl-composition-weights/v1", "library_sha256": RC.sha256_file(root / "lib/lib.json"),
           "train_manifest_sha256": RC.sha256_file(root / anchor / "manifest.json"),
           "weights": {"new_alpha": 1.0, "old_alpha": 0.5},
           "provenance": {"rule": "ew-theme-v1", "weighted_standalone_turnover": 0.05}}
    (root / "fit" / "composition_weights.json").write_bytes(CW.canonical_bytes(doc))
    (root / LIST).write_text(json.dumps(DROP_LIST))
    if drop:
        spec["fit"]["carry"]["drop"] = {"path": LIST, "sha256": None}
    sp.write_text(json.dumps(spec))
    return root, sp, spec


def strip_absent(argv: list[str]) -> list[str]:
    """argv without the OD3P-1 tokens: the runner's ``--bind LIST`` and the exe's two flags."""
    out, k = [], 0
    while k < len(argv):
        if argv[k] == "--bind" and argv[k + 1] == LIST or argv[k] in (RR.ABSENT_FLAG, RR.ABSENT_SHA_FLAG):
            k += 2
            continue
        out.append(argv[k])
        k += 1
    return out


def test_drop_passes_the_absent_list_to_the_history_ic_passes_only(tmp_path):
    root, sp, _ = drop_root(tmp_path)
    pin = RC.sha256_file(root / LIST)
    plan = RC.plan_lines(TR.cycle_of(root, sp, capabilities=ABSENT))
    assert any(line.startswith(f"# pin carry:drop: {LIST}") for line in plan)
    steps = TR.cycle_of(root, sp, capabilities=ABSENT).steps()
    by = {(st.phase, st.role): st for st in steps}
    for phase in RR.ABSENT_PHASES:
        argv = by[(phase, "E1")].argv                                  # the history era: list bound, flags at the end
        assert argv[-4:] == [RR.ABSENT_FLAG, LIST, RR.ABSENT_SHA_FLAG, pin]
        k = argv.index("--")
        assert argv[k - 2:k] == ["--bind", LIST] and argv[k + 1] == "bin/ic.exe"
        assert "OD3P-1" in by[(phase, "E1")].note
        anchor = by[(phase, "E2")].argv                                # the TRAIN anchor: nothing
        assert RR.ABSENT_FLAG not in anchor and LIST not in anchor
    for key in (("nav", "E1"), ("fields", "E1"), ("nav", "E2")):
        assert LIST not in by[key].argv
    carry = by[("fit", None)].argv
    assert carry[-4:] == ["--drop-list", LIST, "--drop-list-sha256", pin]
    # the only change of the drop to the plan: those tokens on the history IC passes, and the carry's --drop-list
    _, sp0, _ = drop_root(tmp_path / "plain", drop=False)
    plain = TR.cycle_of(tmp_path / "plain" / "r", sp0, capabilities=ABSENT).steps()
    assert [(s.phase, s.role) for s in steps] == [(s.phase, s.role) for s in plain]
    for st, st0 in zip(steps, plain):
        argv = [a.replace(str(tmp_path / "plain" / "r"), str(root)) for a in st0.argv]
        if st.phase == "fit":                                          # the carry binds and reads the list too
            assert strip_absent(st.argv) == argv + ["--drop-list", LIST, "--drop-list-sha256", pin]
            assert st.argv[st.argv.index("--") - 2:st.argv.index("--")] == ["--bind", LIST]
        else:
            assert strip_absent(st.argv) == argv, (st.phase, st.role)
            assert (st.argv != argv) == (st.role == "E1" and st.phase in RR.ABSENT_PHASES)


def test_drop_runs_the_real_carry_and_the_carried_weights_are_train_bytes(tmp_path):
    root, sp, _ = drop_root(tmp_path)
    log: list[str] = []
    assert TR.run(root, sp, log, capabilities=ABSENT, stop_after="w") == RC.EXIT_OK, log[-5:]
    argvs = TR.argvs(root)
    carry = next(a for a in argvs if "carry_weights.py" in " ".join(a))
    assert carry[-4:] == ["--drop-list", LIST, "--drop-list-sha256", RC.sha256_file(root / LIST)]
    w = root / "out" / "W"
    train = (root / "fit/composition_weights.json").read_bytes()
    assert (w / "composition_weights.json").read_bytes() == train
    e1 = json.loads((w / "composition_weights.E1.json").read_bytes())
    assert e1["weights"] == {"new_alpha": 1.0, "old_alpha": 0.5} and "drop" not in e1["carried_from"]   # OD3P-1
    assert json.loads((w / "carry.json").read_bytes())["drop"]["members"] == ["old_alpha"]
    wt = {a[a.index("--output") + 1]: a for a in argvs if "--composition-weights" in a}
    assert wt["out/WT-E1-1"][-4:-2] == [RR.ABSENT_FLAG, LIST] and RR.ABSENT_FLAG not in wt["out/WT-E2-1"]
    # lock fills the drop pin; a changed list is a pin mismatch
    assert RC.main(["lock", str(sp), "--root", str(root), "--write"]) == 0
    spec = json.loads(sp.read_text())
    assert spec["fit"]["carry"]["drop"]["sha256"] == RC.sha256_file(root / LIST)
    assert TR.cycle_of(root, sp, capabilities=ABSENT).pins["carry:drop"][2] == "locked, verified"
    (root / LIST).write_text(json.dumps(dict(DROP_LIST, members=["new_alpha"])))
    with pytest.raises(RC.CycleError, match="PIN MISMATCH fit.carry.drop") as e:
        TR.cycle_of(root, sp, capabilities=ABSENT)
    assert e.value.code == RC.EXIT_PIN


FAKE_IC = r'''
import json, sys
caps = ["list-rules"] + (["absent-members-v1"] if "OFFER" in __file__ else [])
if sys.argv[1:] == ["--list-rules", "--json"]:
    print(json.dumps({"schema": "atx.composition-rules/v1", "capabilities": caps, "rules": []}))
    sys.exit(0)
sys.exit(2)
'''


@pytest.mark.parametrize("exe", ["injected", "probe_without", "probe_with", "not_an_exe"])
def test_drop_is_refused_when_the_ic_exe_lacks_the_capability(tmp_path, exe):
    root, sp, spec = drop_root(tmp_path)
    kw = {}
    if exe == "injected":
        kw["capabilities"] = frozenset({"list-rules", "no-composition", "marginal"})
    elif exe.startswith("probe"):                       # the real probe: `<exe> --list-rules --json` (phases.py)
        name = "fake_ic_OFFER.py" if exe == "probe_with" else "fake_ic.py"
        (root / "bin" / name).write_text(FAKE_IC)
        (root / "bin" / "ic.cmd").write_text(f'@"{sys.executable}" "%~dp0{name}" %*\n')
        spec["exes"]["ic"] = "bin/ic.cmd"
        sp.write_text(json.dumps(spec))
    if exe == "probe_with":                             # the same spec plans when the exe offers it
        steps = TR.cycle_of(root, sp).steps()
        assert next(s for s in steps if s.phase == "w" and s.role == "E1").argv[-4] == RR.ABSENT_FLAG
        return
    with pytest.raises(RC.CycleError, match=RR.ABSENT_CAPABILITY) as e:
        TR.cycle_of(root, sp, **kw).steps()
    assert e.value.code == RC.EXIT_USAGE and "OD3P-1" in str(e.value) and "E1" in str(e.value)
    assert RC.main(["plan", str(sp), "--root", str(root)]) == RC.EXIT_USAGE   # the CLI probes the exe: refused too
    assert not (root / "out").exists()


def test_drop_with_no_history_role_is_refused(tmp_path):
    root, sp, spec = drop_root(tmp_path, history=False)                          # tiny_world: both eras in TRAIN
    with pytest.raises(RC.CycleError, match="none of the roles E1, E2 is a history era") as e:
        RR.roles_cycle(spec, RC.Resolver(root), spec_path=sp, capabilities=ABSENT)
    assert e.value.code == RC.EXIT_USAGE
    spec["fit"]["carry"].pop("drop")                                              # without the list it plans
    RR.roles_cycle(spec, RC.Resolver(root), spec_path=sp, capabilities=ABSENT).steps()


def test_drop_changes_only_the_history_ic_passes_against_the_pinned_zero_module(tmp_path):
    """Identity with research_roles.py at 937865f0 (the pinned-0 design): a carry without drop plans the same steps;
    with drop the steps differ only by the OD3P-1 tokens on the history roles' IC passes and the carry note."""
    try:
        text = subprocess.run(["git", "cat-file", "-p", ROLES_937], capture_output=True, check=True, cwd=HERE).stdout
    except (OSError, subprocess.CalledProcessError):
        pytest.skip(f"git history with the research_roles blob {ROLES_937[:8]} is unavailable")
    path = tmp_path / "old" / "research_roles_937.py"
    path.parent.mkdir()
    path.write_bytes(text)
    mod_spec = importlib.util.spec_from_file_location("research_roles_937", path)
    old = importlib.util.module_from_spec(mod_spec)
    mod_spec.loader.exec_module(old)
    for drop in (False, True):
        root, sp, spec = drop_root(tmp_path / f"d{int(drop)}", drop=drop)
        new_c, old_c = (m.roles_cycle(json.loads(json.dumps(spec)), RC.Resolver(root), spec_path=sp,
                                      capabilities=ABSENT) for m in (RR, old))
        new_s, old_s = new_c.steps(), old_c.steps()
        assert [(s.phase, s.role, s.output, s.state) for s in new_s] == [(s.phase, s.role, s.output, s.state)
                                                                        for s in old_s]
        for a, b in zip(new_s, old_s):
            if drop and a.role == "E1" and a.phase in RR.ABSENT_PHASES:
                assert strip_absent(a.argv) == b.argv and a.argv != b.argv
            else:
                assert a.argv == b.argv, (a.phase, a.role)
            assert a.note == b.note or drop and (a.phase == "fit" or "OD3P-1" in a.note)
        if not drop:
            assert RC.plan_lines(new_c) == RC.plan_lines(old_c)
            assert RC.status_lines(new_c) == RC.status_lines(old_c)


@pytest.mark.parametrize("where", ["nav.rule", "nav.flags", "nav.flags=", "malformed"])
def test_drop_refuses_a_spec_that_applies_a_flagged_rule(tmp_path, where):
    root, sp, spec = drop_root(tmp_path)
    if where == "nav.rule":
        spec["nav"]["rule"] = "mom-volman-v1"
    elif where == "nav.flags":
        spec["nav"]["flags"] += ["--theme-hedge", "theme-cov-hedge-v1"]
    elif where == "nav.flags=":
        spec["nav"]["flags"] += ["--theme-hedge=theme-cov-hedge-v1"]
    else:
        spec["fit"]["carry"]["drop"].pop("sha256")
    needle = "fit.carry needs weights and admission" if where == "malformed" else "flagged rule"
    with pytest.raises(RC.CycleError, match=needle) as e:
        RR.roles_cycle(spec, RC.Resolver(root), spec_path=sp)
    assert e.value.code == RC.EXIT_USAGE
    sp.write_text(json.dumps(spec))
    assert RC.main(["plan", str(sp), "--root", str(root)]) == RC.EXIT_USAGE


# ------------------------------------------------------------------ OD3P: --risk-model on history (PRE-10)
def history_root(tmp: Path) -> tuple[Path, Path, dict]:
    """Two history eras (2014, 2015: before TRAIN) on the fake pipeline (plan only)."""
    root = TR.make_root(tmp)
    roles = []
    for name, begin, end in (("E1", "2014-01-01", "2015-01-01"), ("E2", "2015-01-01", "2016-01-01")):
        doc = {"universe": {"id": "u-v1"}, "dates": 700, "score_begin": 400,
               "score_start_ns": TR.TW.session_ns(dt.date.fromisoformat(begin) + dt.timedelta(days=1)),
               "score_end_ns": TR.TW.session_ns(dt.date.fromisoformat(end))}
        (root / name.lower()).mkdir()
        (root / name.lower() / "manifest.json").write_text(json.dumps(doc))
        roles.append({"id": name, "dir": name.lower(), "begin": begin, "end": end, "universe": "u-v1"})
    spec = TR.base_spec(roles=roles)
    sp = tmp / "s.json"
    sp.write_text(json.dumps(spec))
    return root, sp, spec


RISK = ["--risk-model", "risk/r.atxcov", "--risk-model-sha256", "a" * 64]


@pytest.mark.parametrize("case", ["two_eras", "one_era", "equals_form", "ref_flags"])
def test_a_book_reading_the_risk_model_is_refused_on_history(tmp_path, case):
    root, sp, spec = history_root(tmp_path)
    if case == "one_era":
        spec["roles"] = spec["roles"][1:]
        spec["summ"]["extra"] = ["--era-of", "0123456789abcdef"]
    if case == "equals_form":
        spec["nav"]["flags"] += ["--risk-model=risk/r.atxcov"]
    elif case == "ref_flags":
        spec["roles"] = spec["roles"][1:]
        spec["summ"]["extra"] = ["--era-of", "0123456789abcdef"]
        (root / "lib" / "parent_combined.json").write_text("{}")
        spec["inputs"]["reference_combined"] = {"path": "lib/parent_combined.json", "sha256": None}
        spec["ref"] = {"output": "out/REF", "combined": "reference_combined", "flags": ["--cadence", "1", *RISK]}
    else:
        spec["nav"]["flags"] += RISK
    plain = json.loads(json.dumps(spec))
    for section in ("nav", "ref"):
        if section in plain:
            plain[section]["flags"] = [f for f in plain[section]["flags"] if not f.startswith("--risk-model")
                                       and f not in ("risk/r.atxcov", "a" * 64)]
    RR.roles_cycle(plain, RC.Resolver(root), spec_path=sp).steps()            # without the flag it plans
    with pytest.raises(RC.CycleError, match="P9-F0h") as e:
        RR.roles_cycle(spec, RC.Resolver(root), spec_path=sp)
    assert e.value.code == RC.EXIT_USAGE and "PRE-10" in str(e.value)
    sp.write_text(json.dumps(spec))
    assert RC.main(["plan", str(sp), "--root", str(root)]) == RC.EXIT_USAGE


def test_a_risk_model_book_inside_train_still_plans(tmp_path):
    root, sp, _ = TR.two_era_root(tmp_path)                                     # tiny_world: both eras in TRAIN
    spec = json.loads(sp.read_text())
    spec["nav"]["flags"] += RISK
    nav = [st for st in RR.roles_cycle(spec, RC.Resolver(root), spec_path=sp).steps() if st.phase == "nav"]
    assert len(nav) == 2 and all("--risk-model" in st.argv for st in nav)


# ------------------------------------------------------------------ identity: no fit.carry, no label_dir
def old_roles():
    try:
        text = subprocess.run(["git", "cat-file", "-p", ROLES_BLOB], capture_output=True, check=True,
                              cwd=HERE).stdout
    except (OSError, subprocess.CalledProcessError):
        pytest.skip(f"git history with the research_roles blob {ROLES_BLOB[:8]} is unavailable")
    return text


def load_old(tmp: Path):
    path = tmp / "old" / "research_roles_i8.py"
    path.parent.mkdir(exist_ok=True)
    path.write_bytes(old_roles())
    spec = importlib.util.spec_from_file_location("research_roles_i8", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def test_without_the_new_keys_plans_equal_the_integration_8_module(tmp_path):
    old = load_old(tmp_path)
    root, sp, _ = TR.two_era_root(tmp_path / "two")
    _, _, sp1, _ = TR.one_role_pair(tmp_path / "one", TR.TW.SCORE_BEGIN, TR.TW.DATES - 1)
    root1 = tmp_path / "one" / "roles"
    hroot, hsp, hspec = history_root(tmp_path / "hist")
    hspec["summ"].pop("ledger")                                               # a history pool, not ledgered
    hsp.write_text(json.dumps(hspec))
    rspec = json.loads(sp.read_text())
    rspec["nav"]["flags"] += RISK                                             # a risk-model book inside TRAIN
    rsp = tmp_path / "two" / "risk.json"
    rsp.write_text(json.dumps(rspec))
    for r, path in ((root, sp), (root1, sp1), (hroot, hsp), (root, rsp)):
        spec = json.loads(path.read_text())
        cycles = [m.roles_cycle(json.loads(json.dumps(spec)), RC.Resolver(r), spec_path=path) for m in (RR, old)]
        assert RC.plan_lines(cycles[0]) == RC.plan_lines(cycles[1])
        assert RC.status_lines(cycles[0]) == RC.status_lines(cycles[1])
        assert [(s.phase, s.argv, s.output, s.state) for s in cycles[0].steps()] == [
            (s.phase, s.argv, s.output, s.state) for s in cycles[1].steps()]
    spec = json.loads(sp.read_text())
    a, b = RR.lock(json.loads(json.dumps(spec)), root), old.lock(json.loads(json.dumps(spec)), root)
    assert a == b
