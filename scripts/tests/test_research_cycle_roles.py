"""research_cycle.py roles: loop (platform v8 H-1): era shards on the tiny_world fixture split in two eras.

Run: "C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider scripts/tests/test_research_cycle_roles.py

No real data, no build. The era roles are tiny_world's scored sessions (2021-12 .. 2022-12, inside TRAIN) cut at a
session boundary: E1 scores sessions 384..519 and E2 520..655 (its warm-up reaches into E1's window, as allowed). A
fake bounded runner stands in for the IC runner, the NAV replay and the fitter (it records --role-id in its receipts);
the summ phase runs the real nav_summ.py on NAV dirs written from tiny_world's planted book (test_nav_summ_pool), so the
ledger lines are nav_summ's own. The history-era and refusal tests only plan.
"""
from __future__ import annotations

import datetime as dt
import hashlib
import json
from pathlib import Path
import subprocess
import sys

import pytest

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
for p in (HERE.parent, HERE / "fixtures", REPO / "atx-impl" / "tools"):
    sys.path.insert(0, str(p))
import cycle_verdict  # noqa: E402
import research_cycle as RC  # noqa: E402
import research_roles as RR  # noqa: E402
import tiny_world as TW  # noqa: E402
import test_nav_summ_pool as NP  # noqa: E402  (tiny_world NAV rows and the NAV dir writer)
import backtest_integrity as BI  # noqa: E402

NAV_SUMM = (REPO / "atx-impl" / "tools" / "nav_summ.py").as_posix()
DAY = 86_400_000_000_000
SPLIT = 136                     # E2's first scored row in tiny_world's 272 scored sessions (session 520)

FAKE_RUNNER = r'''
import json, sys
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
beh = json.loads(Path("behaviour.json").read_text()) if Path("behaviour.json").exists() else {}
def receipt(outcome, code):
    doc = {"outcome": outcome, "exit_code": code, "wall_seconds": 1.5, "sampled_peak_tree_rss_bytes": 1 << 20}
    if role is not None:
        doc["role_id"] = role
    (out / "receipt.json").write_text(json.dumps(doc))
child = Path(cmd[cmd.index("--output") + 1])
child.mkdir(parents=True, exist_ok=True)
if "fit.py" in " ".join(cmd):
    doc = {"schema": "atx.dsl-composition-weights/v1", "weights": {"new_alpha": 1.0},
           "provenance": {"rule": "ew-theme-v1", "weighted_standalone_turnover": 0.05}}
    (child / "composition_weights.json").write_text(json.dumps(doc))
    for i, a in enumerate(cmd):
        if a == "--era":
            (child / f"composition_weights.{cmd[i + 1]}.json").write_text(json.dumps(doc))
    (child / "admission.json").write_text(json.dumps({"rules": {"tau_limit": 0.7}, "candidates": [
        {"id": "new_alpha", "status": "admitted", "sign_agrees": True, "failed_checks": []}]}))
elif "nav" in cmd:
    for name, text in beh["nav_files"][child.name].items():
        (child / name).write_text(text)
else:
    (child / "summary.json").write_text(json.dumps({"status": "complete"}))
    (child / "orientations.json").write_text("{}")
    (child / "train_combined.json").write_text("{}")
receipt("completed", 0)
'''

FAKE_FIELDS = r'''
import json, sys
from pathlib import Path
a = sys.argv[1:]
out = Path(a[a.index("--output") + 1])
out.mkdir(parents=True, exist_ok=False)
with open("calls.log", "a") as f:
    f.write("fields " + out.name + "\n")
(out / "manifest.json").write_text(json.dumps({"fields": [{"name": "fa"}], "files": {"fa.f64": {"sha256": "a" * 64}}}))
'''

FAKE_CHECK = r'''
with open("calls.log", "a") as f:
    f.write("check\n")
'''


# ------------------------------------------------------------------ the tiny_world eras
def sessions() -> list[int]:
    return [TW.session_ns(d) for d in TW.weekdays_ending(TW.LAST_SESSION, TW.DATES)]


def iso(ns: int) -> str:
    return (dt.date(1970, 1, 1) + dt.timedelta(days=ns // DAY)).isoformat()


def era_role(root: Path, name: str, first: int, last: int, score_begin: int = TW.SCORE_BEGIN) -> dict:
    """A role manifest over tiny_world sessions [first - score_begin, last] scoring [first, last] (the fake pipeline
    reads its universe and score window only); returns the spec role entry."""
    s = sessions()
    d = root / name
    d.mkdir(parents=True)
    doc = {"schema": "atx.recent-research-role/v1", "universe": {"id": "u-v1"}, "dates": score_begin + last - first + 1,
           "score_begin": score_begin, "score_end": score_begin + last - first + 1, "score_start_ns": s[first],
           "score_end_ns": s[last] + DAY, "source": "tiny_world sessions (scripts/tests/fixtures/tiny_world.py)"}
    (d / "manifest.json").write_text(json.dumps(doc, indent=2))
    end = s[last + 1] if last + 1 < len(s) else s[last] + DAY
    return {"id": "", "dir": name, "manifest_sha256": None, "begin": iso(s[first]), "end": iso(end), "universe": "u-v1"}


def nav_files(tmp: Path, rows: dict, lo: int, hi: int | None, role_sha: str) -> dict:
    d = NP.write_dir(tmp / f"nav-{lo}", rows, lo, hi, role_sha=role_sha)
    return {p.name: p.read_text(encoding="utf-8") for p in d.iterdir()}


def base_spec(**over) -> dict:
    spec = {"schema": RC.SCHEMA, "name": "era", "python": sys.executable,
            "runner": {"script": "scripts/runner.py", "seconds": 180, "max_rss_mib": 1536, "min_free_mib": 512},
            "exes": {"ic": "bin/ic.exe", "nav": "bin/nav.exe"},
            "inputs": {"library": {"path": "lib/lib.json", "sha256": None},
                       "recipe": {"path": "lib/recipe.json", "sha256": None},
                       "baseline_library": {"path": "lib/base.json", "sha256": None}},
            "fields": {"builder": "scripts/fields.py", "output": "out/F", "list": ["fa"]},
            "static_check": {"script": "scripts/check.py"},
            "ic": {"u_output": "out/U", "w_output": "out/WT", "cache": "out/CC", "flags": ["--workers", "4"]},
            "fit": {"script": "scripts/fit.py", "output": "out/W", "work_dir": "out/WORK",
                    "flags": ["--screen", "v4-prior-v1"]},
            "gate": {"name": "g", "admitted": ["new_alpha"], "sign_agrees": True},
            "nav": {"output": "out/N", "rule": "aim-partial-v5", "flags": ["--cadence", "1"]},
            "summ": {"script": NAV_SUMM, "dsr_n": 2, "ledger": "trials.jsonl"}}
    spec.update(over)
    return spec


def make_root(tmp: Path, name: str = "r") -> Path:
    root = tmp / name
    for d in ("scripts", "bin", "lib"):
        (root / d).mkdir(parents=True)
    (root / "scripts" / "runner.py").write_text(FAKE_RUNNER)
    (root / "scripts" / "fields.py").write_text(FAKE_FIELDS)
    (root / "scripts" / "check.py").write_text(FAKE_CHECK)
    (root / "scripts" / "fit.py").write_text("")
    (root / "bin" / "ic.exe").write_bytes(b"ic")
    (root / "bin" / "nav.exe").write_bytes(b"nav")
    (root / "lib" / "lib.json").write_text('{"lib": 1}')
    (root / "lib" / "recipe.json").write_text('{"recipe": 1}')
    (root / "lib" / "base.json").write_text('{"lib": 0}')
    return root


def two_era_root(tmp: Path, **spec_over) -> tuple[Path, Path, list[dict]]:
    root = make_root(tmp)
    last = TW.DATES - 1
    e1 = dict(era_role(root, "era1", TW.SCORE_BEGIN, TW.SCORE_BEGIN + SPLIT - 1), id="E1")
    e2 = dict(era_role(root, "era2", TW.SCORE_BEGIN + SPLIT, last), id="E2")
    shas = [RC.sha256_file(root / r["dir"] / "manifest.json") for r in (e1, e2)]
    rows = NP.tiny_rows(SPLIT)
    (root / "behaviour.json").write_text(json.dumps({"nav_files": {
        "N-E1": nav_files(tmp, rows, 0, SPLIT, shas[0]), "N-E2": nav_files(tmp, rows, SPLIT, None, shas[1])}}))
    sp = tmp / "spec.json"
    sp.write_text(json.dumps(base_spec(roles=[e1, e2], **spec_over)), encoding="utf-8")
    return root, sp, [e1, e2]


def cycle_of(root: Path, sp: Path, **kw):
    return RC.make_cycle(RC.Resolver(root), spec_path=sp, **kw)


def run(root: Path, sp: Path, log: list | None = None, **kw) -> int:
    stop_after = kw.pop("stop_after", None)
    return RC.run_cycle(cycle_of(root, sp, **kw), stop_after=stop_after, clean=lambda r: True,
                        log=(log.append if log is not None else lambda s: None))


def calls(root: Path) -> list[str]:
    p = root / "calls.log"
    return p.read_text().split("\n")[:-1] if p.exists() else []


def argvs(root: Path) -> list[list[str]]:
    return [json.loads(line) for line in (root / "argv.log").read_text().splitlines()]


# ------------------------------------------------------------------ the acceptance test
def test_each_era_has_receipt_and_ledger_line(tmp_path):
    root, sp, roles = two_era_root(tmp_path)
    log: list[str] = []
    assert run(root, sp, log) == RC.EXIT_OK, log[-5:]
    # phase-major: every era's fields, the anchor's check, every era's u, one pooled fit, every era's w and nav
    assert calls(root) == ["fields F-E1", "fields F-E2", "check", "U-E1-run1", "U-E2-run1", "W-run1", "WT-E1-run1",
                           "WT-E2-run1", "N-E1-run", "N-E2-run"]
    for era in ("E1", "E2"):
        for run_dir in (f"out/U-{era}-run1", f"out/WT-{era}-run1", f"out/N-{era}-run"):
            receipt = json.loads((root / run_dir / "receipt.json").read_text())
            assert receipt["role_id"] == era and receipt["outcome"] == "completed", run_dir
    assert "role_id" not in json.loads((root / "out/W-run1/receipt.json").read_text())   # the fit is shared
    fit = next(a for a in argvs(root) if "scripts/fit.py" in a)
    k = fit.index("--era")
    assert fit[k:k + 3] == ["--era", "E1", "era1/manifest.json"]
    assert fit[k + 3] == RC.sha256_file(root / "era1/manifest.json")
    assert fit[k + 4:k + 8:2] == ["out/U-E1-1/orientations.json", "out/U-E1-1/summary.json"]
    assert fit[k + 8:] == ["--era-id", "E2", "--output", "out/W"]
    assert fit[fit.index("--train") + 1] == "era2/manifest.json"                  # the anchor is the last role
    wt = {a[a.index("--output") + 1]: a for a in argvs(root) if "--composition-weights" in a}
    assert wt["out/WT-E1-1"][wt["out/WT-E1-1"].index("--composition-weights") + 1] == "out/W/composition_weights.E1.json"
    assert wt["out/WT-E2-1"][wt["out/WT-E2-1"].index("--composition-weights") + 1] == "out/W/composition_weights.json"
    # the ledger: nav_summ's era lines (no trial) and one pooled line (one trial)
    recs = BI.ledger_read(root / "trials.jsonl")
    assert [r["window"]["label"] for r in recs] == ["ERA E1", "ERA E2", "POOL"]
    assert BI.trial_counts(recs) == [0, 0, 1] and recs[2]["cell"] == "pool:out/N-E1,out/N-E2"
    assert [e["role_sha256"] for e in recs[2]["eras"]] == [RC.sha256_file(root / r["dir"] / "manifest.json")
                                                          for r in roles]
    assert any(line.startswith("== summ") for line in log) and "== cycle complete" in log
    verdict = json.loads((root / "build-equity" / "cycle-era" / "cycle_verdict.json").read_text())
    assert {"u:E1", "u:E2", "w:E1", "nav:E2"} <= {p["name"] for p in verdict["phases"]}
    # resume: every output exists; the always-run check and summ re-run and the ledger takes nothing twice
    assert run(root, sp) == RC.EXIT_OK
    assert calls(root)[10:] == ["check"] and len(BI.ledger_read(root / "trials.jsonl")) == 3
    status = RC.status_lines(cycle_of(root, sp))
    assert any(line.startswith("u:E1   done") and "completed exit 0" in line for line in status)


def test_summ_line_pools_the_eras_and_ledger_n_follows_the_pooled_trial(tmp_path):
    root, sp, _ = two_era_root(tmp_path)
    (root / "trials.jsonl").write_text("")
    spec = json.loads(sp.read_text())
    spec["summ"]["dsr_n"] = "ledger+1"
    sp.write_text(json.dumps(spec))
    summ = next(st for st in cycle_of(root, sp).steps() if st.phase == "summ")
    assert summ.argv[2:] == ["--weights", "out/W/composition_weights.json", "--weights",
                             "out/W/composition_weights.E1.json", "--dsr-n", "1", "--ledger", "trials.jsonl",
                             "--ledger-kind", "construction", "--pool", "out/N-E1", "out/N-E2", "--pool-ids", "E1,E2"]
    plan = RC.plan_lines(cycle_of(root, sp))
    assert [line.split(" [")[0] for line in plan if line.startswith("# phase ")] == [
        "# phase fields:E1", "# phase fields:E2", "# phase check", "# phase u:E1", "# phase u:E2", "# phase fit",
        "# phase gate", "# phase w:E1", "# phase w:E2", "# phase nav:E1", "# phase nav:E2", "# phase summ"]
    assert any(line.startswith("# pin role:E1: era1/manifest.json") for line in plan)


def test_stop_after_a_phase_stops_after_every_era(tmp_path):
    root, sp, _ = two_era_root(tmp_path)
    log: list[str] = []
    assert run(root, sp, log, stop_after="u") == RC.EXIT_OK
    assert calls(root)[-2:] == ["U-E1-run1", "U-E2-run1"] and "== stopped after u (--stop-after)" in log


# ------------------------------------------------------------------ one role: the single-role cycle
def one_role_pair(tmp: Path, first: int, last: int) -> tuple[Path, Path, Path, dict]:
    """A roles: spec with one role and the equivalent single-role spec (inputs.role), in two roots."""
    roots = []
    for name in ("single", "roles"):
        root = make_root(tmp, name)
        role = dict(era_role(root, "era3", first, last), id="E3")
        rows = NP.tiny_rows()
        (root / "behaviour.json").write_text(json.dumps({"nav_files": {
            "N": nav_files(tmp / f"{name}-nav", rows, 0, None, RC.sha256_file(root / "era3/manifest.json"))}}))
        roots.append(root)
    single = base_spec()
    single["inputs"]["role"] = {"dir": "era3", "path": "era3/manifest.json", "sha256": None, "universe": "u-v1"}
    sp_single, sp_roles = tmp / "single.json", tmp / "roles.json"
    sp_single.write_text(json.dumps(single))
    sp_roles.write_text(json.dumps(base_spec(roles=[role])))
    return roots[0], sp_single, sp_roles, role


def tree(root: Path) -> dict:
    """Every output file's SHA-256 except those naming the spec file by its digest: the cycle dir (the verdict's
    spec_sha256) and the NAV run's cycle_binding.json (review C-13)."""
    return {p.relative_to(root).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in sorted(root.rglob("*")) if p.is_file() and "cycle-era" not in p.as_posix() and
            p.name != "cycle_binding.json"}


def test_e3_alone_is_the_single_role_cycle_byte_for_byte(tmp_path):
    root_single, sp_single, sp_roles, _ = one_role_pair(tmp_path, TW.SCORE_BEGIN, TW.DATES - 1)
    root_roles = tmp_path / "roles"
    a, b = cycle_of(root_single, sp_single), cycle_of(root_roles, sp_roles)
    assert type(b) is RC.Cycle and b.role_key is None
    assert RC.plan_lines(a, lines_only=True) == RC.plan_lines(b, lines_only=True)
    phase = lambda c: [line for line in RC.plan_lines(c) if not line.startswith("# research_cycle ")]  # noqa: E731
    assert phase(a) == phase(b)                          # every pin and phase line (the header names the spec file)
    assert RC.status_lines(a)[1:] == RC.status_lines(b)[1:]
    assert run(root_single, sp_single) == run(root_roles, sp_roles) == RC.EXIT_OK
    assert calls(root_single) == calls(root_roles)
    assert tree(root_single) == tree(root_roles)         # every output byte, the ledger included
    bindings = [json.loads(next(r.rglob("cycle_binding.json")).read_text()) for r in (root_single, root_roles)]
    same = [{k: v for k, v in b.items() if k != "spec_sha256"} for b in bindings]
    assert same[0] == same[1]                            # the same NAV command (argv sha256); the spec files differ
    assert BI.ledger_read(root_single / "trials.jsonl")[0]["window"]["label"] == "TRAIN"


def test_a_history_role_alone_pools_its_fit_and_summ(tmp_path):
    root = make_root(tmp_path)
    doc = {"universe": {"id": "u-v1"}, "dates": 700, "score_begin": 400,
           "score_start_ns": TW.session_ns(dt.date(2015, 1, 2)), "score_end_ns": TW.session_ns(dt.date(2016, 1, 1))}
    (root / "e1").mkdir()
    (root / "e1" / "manifest.json").write_text(json.dumps(doc))
    role = {"id": "E1", "dir": "e1", "begin": "2015-01-01", "end": "2016-01-01", "universe": "u-v1"}
    sp = tmp_path / "s.json"
    sp.write_text(json.dumps(base_spec(roles=[role])))
    steps = {st.phase: st for st in cycle_of(root, sp).steps()}
    assert steps["fit"].argv[-4:] == ["--era-id", "E1", "--output", "out/W"]
    assert steps["summ"].argv[-4:] == ["--pool", "out/N", "--pool-ids", "E1"]
    assert steps["u"].output == "out/U-1" and "--role-id" not in steps["u"].argv


# ------------------------------------------------------------------ refusals and lock
@pytest.mark.parametrize("change, needle", [
    (lambda s, r: r[1].update(begin="2021-01-04"), "date order"),
    (lambda s, r: r[1].update(begin=r[0]["end"][:8] + "01") or r[0].update(end="2022-12-01"), "overlaps"),
    (lambda s, r: r[1].update(end="2024-06-01"), "sealed"),
    (lambda s, r: r[0].update(begin="2019-06-01"), "straddles the TRAIN begin"),
    (lambda s, r: r[1].update(id="E/2"), "must match"),
    (lambda s, r: r[1].update(id="E1"), "duplicate id"),
    (lambda s, r: r[1].update(extra=1), "unknown key"),
    (lambda s, r: r[1].update(manifest_sha256="abc"), "SHA-256"),
    (lambda s, r: s["inputs"].update(role={"path": "x", "sha256": None}), "inputs.role is absent"),
    (lambda s, r: s.update(card={"script": "c.py", "output": "C"}), "card are single-role"),
    (lambda s, r: s.update(compare=[{"name": "x", "after": "u", "mode": "file", "a": "a", "b": "b"}]),
     "compare are single-role"),
    (lambda s, r: s["fields"].update(check={"baseline": "x"}), "fields.check are single-role"),
    (lambda s, r: s["summ"].update(cells=["c"]), "summ.cells are single-role"),
    (lambda s, r: s.pop("fit") and s.pop("gate"), "needs fit"),
])
def test_roles_refusals(tmp_path, change, needle):
    root, sp, _ = two_era_root(tmp_path)
    spec = json.loads(sp.read_text())
    change(spec, spec["roles"])
    with pytest.raises(RC.CycleError, match=needle) as e:
        RR.roles_cycle(spec, RC.Resolver(root), spec_path=sp)
    assert e.value.code == RC.EXIT_USAGE
    sp.write_text(json.dumps(spec))
    assert RC.main(["plan", str(sp), "--root", str(root)]) == RC.EXIT_USAGE


def test_cli_options_refused_with_two_roles_and_a_manifest_outside_its_era(tmp_path):
    root, sp, _ = two_era_root(tmp_path)
    for extra in (["--keep-fields", "--suffix", "x"], ["--reuse-fields", "out/F-E1"]):
        assert RC.main(["plan", str(sp), "--root", str(root), *extra]) == RC.EXIT_USAGE
    spec = json.loads(sp.read_text())
    spec["roles"][0]["end"] = "2022-03-01"               # E1's manifest scores until 2022-06
    spec["roles"][1]["begin"] = "2022-06-01"
    with pytest.raises(RC.CycleError, match="outside the era window"):
        RR.roles_cycle(spec, RC.Resolver(root), spec_path=sp)


def test_lock_fills_the_role_pins(tmp_path):
    root, sp, roles = two_era_root(tmp_path)
    assert RC.main(["lock", str(sp), "--root", str(root), "--write"]) == 0
    spec = json.loads(sp.read_text())
    assert [r["manifest_sha256"] for r in spec["roles"]] == [RC.sha256_file(root / r["dir"] / "manifest.json")
                                                             for r in roles]
    assert spec["inputs"]["library"]["sha256"] == RC.sha256_file(root / "lib/lib.json")
    pins = cycle_of(root, sp).pins
    assert pins["role:E1"][2] == pins["role:E2"][2] == "locked, verified" and "role" not in pins
    (root / "era1" / "manifest.json").write_text((root / "era1" / "manifest.json").read_text() + " ")
    with pytest.raises(RC.CycleError, match="PIN MISMATCH role"):
        cycle_of(root, sp)
    assert RC.main(["lock", str(sp), "--root", str(root)]) == RC.EXIT_PIN
    assert RC.main(["lock", str(sp), "--root", str(root), "--relock", "--write"]) == 0


def test_as_built_era_fields_are_pinned_per_role(tmp_path):
    root, sp, _ = two_era_root(tmp_path)
    for name in ("f1", "f2"):
        (root / name).mkdir()
        (root / name / "manifest.json").write_text(json.dumps({"fields": [{"name": "fa"}], "files": {}, "n": name}))
    spec = json.loads(sp.read_text())
    spec["roles"][0]["fields_dir"], spec["roles"][1]["fields_dir"] = "f1", "f2"
    sp.write_text(json.dumps(spec))
    assert RC.main(["lock", str(sp), "--root", str(root), "--write"]) == 0
    spec = json.loads(sp.read_text())
    assert spec["roles"][0]["fields_manifest_sha256"] == RC.sha256_file(root / "f1/manifest.json")
    steps = [st for st in cycle_of(root, sp).steps() if st.phase in ("fields", "u")]
    assert [(cycle_verdict.step_key(st), st.kind, st.state, st.output) for st in steps[:2]] == [
        ("fields:E1", "pinned", "done", "f1"), ("fields:E2", "pinned", "done", "f2")]
    u1 = steps[2].argv
    assert u1[u1.index("--train-fields") + 1:u1.index("--train-fields") + 4] == [
        "f1", "--train-fields-sha256", RC.sha256_file(root / "f1/manifest.json")]


def test_the_script_plans_and_refuses_a_roles_spec(tmp_path):
    """Run as a script: research_roles imports this very module, so its refusals are main()'s exit codes."""
    root, sp, _ = two_era_root(tmp_path)
    script = HERE.parent / "research_cycle.py"
    done = subprocess.run([sys.executable, str(script), "plan", str(sp), "--root", str(root), "--lines-only"],
                          capture_output=True, text=True)
    assert done.returncode == 0, done.stderr
    assert "--role-id E1" in done.stdout and "--era E1 era1/manifest.json" in done.stdout
    spec = json.loads(sp.read_text())
    spec["roles"][1]["end"] = "2024-06-01"
    sp.write_text(json.dumps(spec))
    done = subprocess.run([sys.executable, str(script), "plan", str(sp), "--root", str(root)], capture_output=True,
                          text=True)
    assert done.returncode == RC.EXIT_USAGE and "sealed" in done.stderr and "Traceback" not in done.stderr


# ------------------------------------------------------------------ the bounded runner's --role-id
def test_runner_records_the_role_id(tmp_path):
    pytest.importorskip("psutil")
    runner = HERE.parent / "run_bounded_research.py"
    root = tmp_path / "rt"
    root.mkdir()
    base = [sys.executable, str(runner), "--root", str(root), "--no-git", "--seconds", "60", "--max-rss-mib", "512",
            "--min-free-mib", "64"]
    done = subprocess.run(base + ["--role-id", "E1", "--output", str(root / "a"), "--", sys.executable, "-c", "0"],
                          capture_output=True, text=True)
    assert done.returncode == 0, done.stderr
    for name in ("start.json", "receipt.json"):
        assert json.loads((root / "a" / name).read_text())["role_id"] == "E1"
    done = subprocess.run(base + ["--output", str(root / "b"), "--", sys.executable, "-c", "0"], capture_output=True,
                          text=True)
    assert done.returncode == 0 and "role_id" not in json.loads((root / "b" / "receipt.json").read_text())
    done = subprocess.run(base + ["--role-id", "E/1", "--output", str(root / "c"), "--", sys.executable, "-c", "0"],
                          capture_output=True, text=True)
    assert done.returncode == 2 and "--role-id" in done.stderr and not (root / "c").exists()
