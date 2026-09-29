"""research_cycle.py (platform v7 L2): spec parsing, pin resolution, phase ordering, stop-on-refusal, v6.1 identity.

Run: "C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider scripts/tests/test_research_cycle.py

No real run: the v6.1 plan is resolved in hash-only mode against fixtures/v61_pins.json (SHA-256 of the pool-2 files,
captured by fixtures/gen_v61_dry.sh together with v61_train.sh's DRY=1 command lines in fixtures/v61_train_dry.txt);
the `run` tests drive fake runner / builder / check / summ scripts in a temporary root. Set
RESEARCH_CYCLE_LIVE_ROOT=C:/atx-wt/pool-2 to also resolve the plan against the real files (read-only hashing).
"""
from __future__ import annotations

import json
import os
import sys
from pathlib import Path

import pytest

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
import research_cycle as RC  # noqa: E402

FIX = HERE / "fixtures"
V61 = HERE.parent / "specs" / "v61.json"


def v61_cycle(root: Path, **kw) -> RC.Cycle:
    known = json.loads((FIX / "v61_pins.json").read_text(encoding="utf-8"))
    return RC.Cycle(RC.load_spec(V61), RC.Resolver(root, known), spec_path=V61, **kw)


# ------------------------------------------------------------------ v6.1 identity (the root acceptance, offline)
def test_plan_equals_v61_train_sh_dry_output_pin_for_pin(tmp_path):
    lines = RC.plan_lines(v61_cycle(tmp_path), lines_only=True)
    want = (FIX / "v61_train_dry.txt").read_text(encoding="utf-8").splitlines()
    assert lines == want


def test_bounded_lines_equal_the_executed_v61_receipts(tmp_path):
    """The resolved argv after the runner's `--` equals what the bounded runner actually executed (its receipts)."""
    receipts = json.loads((FIX / "v61_receipt_commands.json").read_text(encoding="utf-8"))
    steps = {s.phase: s for s in v61_cycle(tmp_path).steps()}
    spec = RC.load_spec(V61)
    for phase, r in receipts.items():
        argv = steps[phase].argv
        k = argv.index("--")
        assert argv[k + 2:] == r["argv_after_executable"], phase
        assert r["outcome"] == "completed" and r["exit_code"] == 0
        lim = r["limits"]
        assert (lim["seconds"], lim["max_rss_mib"], lim["min_free_mib"]) == (
            spec["runner"]["seconds"], spec["runner"]["max_rss_mib"], spec["runner"]["min_free_mib"])


@pytest.mark.skipif(not os.environ.get("RESEARCH_CYCLE_LIVE_ROOT"), reason="set RESEARCH_CYCLE_LIVE_ROOT to hash the "
                                                                          "real pool-2 files")
def test_plan_live_root_equals_the_fixture():
    cycle = RC.Cycle(RC.load_spec(V61), RC.Resolver(Path(os.environ["RESEARCH_CYCLE_LIVE_ROOT"])), spec_path=V61)
    assert RC.plan_lines(cycle, lines_only=True) == (FIX / "v61_train_dry.txt").read_text().splitlines()
    assert all(s.state in ("done", "always") for s in cycle.steps())


def test_v61_states_pins_and_phase_order(tmp_path):
    c = v61_cycle(tmp_path)
    steps = c.steps()
    assert [s.phase for s in steps] == [p for p in RC.PHASES if p not in ("card", "monitor")]  # W3 phases: v61-ops
    assert {s.phase: s.state for s in steps} == {"fields": "done", "check": "always", "u": "done", "fit": "done",
                                                 "gate": "always", "w": "done", "nav": "done", "summ": "always"}
    pins = json.loads((FIX / "v61_pins.json").read_text())
    spec = RC.load_spec(V61)
    for key, item in spec["inputs"].items():  # every spec pin is the file's SHA-256 (as computed by `lock`)
        assert item["sha256"] == pins[item["path"]], key
    assert all(how == "locked (hash-only)" for _, _, how in c.pins.values())


def test_suffix_reuse_and_ledger(tmp_path):
    pins = json.loads((FIX / "v61_pins.json").read_text())
    c = v61_cycle(tmp_path, suffix="r7", reuse_fields="build-equity/recent-fast-train-2020-2022-v2-lo1-fields-v6b",
                  ledger="build-equity/trials.jsonl")
    steps = {s.phase: s for s in c.steps()}
    assert steps["fields"].output == "build-equity/recent-fast-train-2020-2022-v2-lo1-fields-v7-r7"
    assert steps["u"].output == "build-equity/mega-v61-train-u-r7-1"
    assert steps["fit"].output == "build-equity/mega-weights-v61-ew-r7"
    assert steps["w"].output == "build-equity/mega-v61w-train-ew-r7-1"
    assert steps["nav"].output == "build-equity/mega-nav-v61u-ew-t.05-d.1-fixed-obdelta-x.05-loc-L1.247-r7"
    assert steps["nav"].run_dir == steps["nav"].output + "-run"
    assert all(s.state in ("pending", "always") for s in steps.values())
    f = steps["fields"].argv
    assert f[-4:] == ["--reuse", "build-equity/recent-fast-train-2020-2022-v2-lo1-fields-v6b", "--reuse-sha256",
                      pins["build-equity/recent-fast-train-2020-2022-v2-lo1-fields-v6b/manifest.json"]]
    u = steps["u"].argv
    assert "--candidate-cache" in u and u[u.index("--candidate-cache") + 1] == "build-equity/mega-candidate-cache-v61-r7"
    # pins of phases that have not run yet are placeholders, never guesses
    assert u[u.index("--train-fields-sha256") + 1] == ("<sha256:build-equity/recent-fast-train-2020-2022-v2-lo1-"
                                                       "fields-v7-r7/manifest.json>")
    s = steps["summ"].argv
    assert s[-4:-1] == ["build-equity/trials.jsonl", "--ledger-kind", "construction"] and s[-1] == steps["nav"].output
    # inputs keep their names
    assert u[u.index("--library") + 1] == "atx-impl/strategies/fund_industry_ic_v61.json"


# ------------------------------------------------------------------ spec parsing and pins
def minimal_spec(**over):
    spec = {"schema": RC.SCHEMA, "name": "t", "python": sys.executable,
            "runner": {"script": "r.py", "seconds": 180, "max_rss_mib": 1536, "min_free_mib": 512},
            "inputs": {"library": {"path": "lib.json", "sha256": None}}}
    spec.update(over)
    return spec


def test_spec_validation_refuses_malformed_specs():
    RC.validate_spec(minimal_spec())
    for bad in (dict(minimal_spec(), schema="x"), {k: v for k, v in minimal_spec().items() if k != "runner"},
                minimal_spec(inputs={"libary": {"path": "a", "sha256": None}}),
                minimal_spec(inputs={"library": {"path": "a"}}),
                minimal_spec(gate={"admitted": ["x"]}),
                minimal_spec(ic={"u_output": "U", "cache": "C", "flags": []}),
                minimal_spec(nav={"output": "N", "rule": "r", "flags": []})):
        with pytest.raises(RC.CycleError) as e:
            RC.validate_spec(bad)
        assert e.value.code == RC.EXIT_USAGE
    assert RC.parse_attempts(["u=2", "fit=3"]) == {"u": 2, "fit": 3}
    for bad in (["x=1"], ["u=0"], ["u"], ["u=10"]):
        with pytest.raises(RC.CycleError):
            RC.parse_attempts(bad)


def test_lock_computes_pins_from_files_and_plan_refuses_a_mismatch(tmp_path):
    (tmp_path / "lib.json").write_text("{}", encoding="utf-8")
    sp = tmp_path / "spec.json"
    sp.write_text(json.dumps(minimal_spec()), encoding="utf-8")
    c = RC.Cycle(RC.load_spec(sp), RC.Resolver(tmp_path))
    assert c.pins["library"][2].startswith("UNLOCKED")
    assert RC.main(["lock", str(sp), "--root", str(tmp_path), "--write"]) == 0
    spec = json.loads(sp.read_text())
    assert spec["inputs"]["library"]["sha256"] == RC.sha256_file(tmp_path / "lib.json")
    assert RC.Cycle(spec, RC.Resolver(tmp_path)).pins["library"][2] == "locked, verified"
    (tmp_path / "lib.json").write_text('{"x": 1}', encoding="utf-8")
    with pytest.raises(RC.CycleError) as e:
        RC.Cycle(spec, RC.Resolver(tmp_path))
    assert e.value.code == RC.EXIT_PIN and "PIN MISMATCH library" in str(e.value)
    assert RC.main(["plan", str(sp), "--root", str(tmp_path)]) == RC.EXIT_PIN
    assert RC.main(["lock", str(sp), "--root", str(tmp_path)]) == RC.EXIT_PIN      # never silently relocked
    assert RC.main(["lock", str(sp), "--root", str(tmp_path), "--relock", "--write"]) == 0
    assert json.loads(sp.read_text())["inputs"]["library"]["sha256"] == RC.sha256_file(tmp_path / "lib.json")


def test_role_universe_is_checked(tmp_path):
    (tmp_path / "role").mkdir()
    (tmp_path / "role" / "manifest.json").write_text(json.dumps({"universe": {"id": "other-v1"}}), encoding="utf-8")
    spec = minimal_spec(inputs={"role": {"path": "role/manifest.json", "sha256": None, "universe": "linked-operating-v1"}})
    with pytest.raises(RC.CycleError, match="universe"):
        RC.Cycle(spec, RC.Resolver(tmp_path))


# ------------------------------------------------------------------ run: fake tools in a temporary root
FAKE_RUNNER = r'''
import json, sys
from pathlib import Path
args = sys.argv[1:]
k = args.index("--")
opts, cmd = args[:k], args[k + 1:]
out = Path(opts[opts.index("--output") + 1])
out.mkdir(parents=True, exist_ok=False)
with open("calls.log", "a") as f:
    f.write(out.name + "\n")
beh = json.loads(Path("behaviour.json").read_text()) if Path("behaviour.json").exists() else {}
b = beh.get(out.name, "ok")
def receipt(outcome, code):
    (out / "receipt.json").write_text(json.dumps({"outcome": outcome, "exit_code": code, "wall_seconds": 1.5,
                                                   "sampled_peak_tree_rss_bytes": 1 << 20}))
if b == "noreceipt":
    print("runner: parser error", file=sys.stderr); sys.exit(2)
if b == "refuse":
    receipt("prelaunch-memory-refusal", None); sys.exit(1)
if b == "error":
    receipt("process-error", 1); sys.exit(1)
if b == "incomplete":
    (out / "stdout.log").write_text('{"status": "incomplete", "partial": true}'); receipt("process-error", 3); sys.exit(1)
child = Path(cmd[cmd.index("--output") + 1])
child.mkdir(parents=True, exist_ok=True)
if "card.py" in " ".join(cmd):
    (child / "index.json").write_text("{}")
    (child / "daily_sleeve.csv").write_text("id,decision_index,session_ns,turnover,pnl,coverage\n")
elif "fit.py" in " ".join(cmd):
    (child / "composition_weights.json").write_text("{}")
    (child / "admission.json").write_text(json.dumps(beh.get("admission", {"rules": {"tau_limit": 0.7}, "candidates": [
        {"id": "new_alpha", "status": "admitted", "sign_agrees": True, "failed_checks": []}]})))
elif "nav" in cmd:
    (child / "summary.json").write_text(json.dumps({"status": "complete", "primary_scenario": "s2"}))
    (child / "daily_s2.csv").write_text("net_return\n0.001\n")
else:
    (child / "summary.json").write_text(json.dumps({"status": "complete" if b != "incomplete-output" else "partial"}))
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
    f.write("fields " + " ".join(x for x in a if x.startswith("--reuse")) + "\n")
beh = json.loads(Path("behaviour.json").read_text()) if Path("behaviour.json").exists() else {}
sha_a = "a" * 64 if beh.get("fields") != "changed" else "c" * 64
(out / "manifest.json").write_text(json.dumps({"fields": [{"name": "fa"}, {"name": "fb"}],
    "files": {"fa.f64": {"sha256": sha_a, "bytes": 8}, "fb.f64": {"sha256": "b" * 64, "bytes": 8}}}))
'''

FAKE_CHECK = r'''
import json, sys
from pathlib import Path
with open("calls.log", "a") as f:
    f.write("check\n")
beh = json.loads(Path("behaviour.json").read_text()) if Path("behaviour.json").exists() else {}
sys.exit(int(beh.get("check", 0)))
'''

FAKE_SUMM = r'''
import sys
with open("calls.log", "a") as f:
    f.write("summ " + " ".join(sys.argv[1:]) + "\n")
'''


def make_root(tmp_path: Path, **spec_over) -> tuple[Path, Path]:
    root = tmp_path / "root"
    for d in ("scripts", "bin", "lib", "role", "fields-base", "ref-cell", "ref-w"):
        (root / d).mkdir(parents=True)
    (root / "scripts" / "runner.py").write_text(FAKE_RUNNER)
    (root / "scripts" / "fields.py").write_text(FAKE_FIELDS)
    (root / "scripts" / "check.py").write_text(FAKE_CHECK)
    (root / "scripts" / "fit.py").write_text("")
    (root / "scripts" / "summ.py").write_text(FAKE_SUMM)
    (root / "bin" / "ic.exe").write_bytes(b"ic")
    (root / "bin" / "nav.exe").write_bytes(b"nav")
    (root / "lib" / "lib.json").write_text('{"lib": 1}')
    (root / "lib" / "recipe.json").write_text('{"recipe": 1}')
    (root / "lib" / "base.json").write_text('{"lib": 0}')
    (root / "role" / "manifest.json").write_text(json.dumps({"universe": {"id": "u-v1"}}))
    (root / "fields-base" / "manifest.json").write_text(json.dumps({"fields": [{"name": "fa"}],
                                                                    "files": {"fa.f64": {"sha256": "a" * 64, "bytes": 8}}}))
    (root / "ref-cell" / "summary.json").write_text("{}")
    (root / "ref-w" / "admission.json").write_text(json.dumps({"candidates": [{"id": "old", "status": "admitted"}]}))
    spec = {
        "schema": RC.SCHEMA, "name": "synthetic", "python": sys.executable,
        "runner": {"script": "scripts/runner.py", "seconds": 180, "max_rss_mib": 1536, "min_free_mib": 512},
        "exes": {"ic": "bin/ic.exe", "nav": "bin/nav.exe"},
        "inputs": {"library": {"path": "lib/lib.json", "sha256": None},
                   "recipe": {"path": "lib/recipe.json", "sha256": None},
                   "baseline_library": {"path": "lib/base.json", "sha256": None},
                   "role": {"dir": "role", "path": "role/manifest.json", "universe": "u-v1", "sha256": None},
                   "baseline_fields": {"path": "fields-base/manifest.json", "sha256": None},
                   "reference_admission": {"path": "ref-w/admission.json", "sha256": None},
                   "reference_cell": {"dir": "ref-cell", "path": "ref-cell/summary.json", "sha256": None}},
        "fields": {"builder": "scripts/fields.py", "output": "out/F", "list": ["fa", "fb"],
                   "check": {"baseline": "baseline_fields", "expect_added": ["fb"]}},
        "static_check": {"script": "scripts/check.py", "expect_added": ["new_alpha"]},
        "ic": {"u_output": "out/U", "w_output": "out/WT", "cache": "out/CC", "flags": ["--workers", "4"]},
        "fit": {"script": "scripts/fit.py", "output": "out/W", "work_dir": "out/WORK", "flags": ["--screen", "s"],
                "max_seconds": 150, "max_passes": 3},
        "gate": {"name": "p1", "admitted": ["new_alpha"], "sign_agrees": True},
        "nav": {"output": "out/N", "rule": "aim-partial-v5", "leverage": "1.247",
                "flags": ["--aim-leverage", "{leverage}", "--cadence", "1"]},
        "summ": {"script": "scripts/summ.py", "dsr_n": 29}}
    spec.update(spec_over)
    sp = tmp_path / "spec.json"
    sp.write_text(json.dumps(spec), encoding="utf-8")
    return root, sp


def cycle_of(root, sp, **kw):
    return RC.Cycle(RC.load_spec(sp), RC.Resolver(root), spec_path=sp, **kw)


def calls(root):
    p = root / "calls.log"
    return p.read_text().split("\n")[:-1] if p.exists() else []


def run(root, sp, lines=None, **kw):
    stop_after = kw.pop("stop_after", None)
    return RC.run_cycle(cycle_of(root, sp, **kw), stop_after=stop_after, clean=lambda r: True,
                        log=(lines.append if lines is not None else lambda s: None))


def behave(root, **b):
    (root / "behaviour.json").write_text(json.dumps(b))


def test_run_executes_every_phase_in_order_then_resumes_as_a_no_op(tmp_path):
    root, sp = make_root(tmp_path)
    log = []
    assert run(root, sp, log) == RC.EXIT_OK
    assert calls(root) == ["fields ", "check", "U-run1", "W-run1", "WT-run1", "N-run",
                           "summ --weights out/W/composition_weights.json --reference ref-cell --dsr-n 29 out/N"]
    assert "gate p1 PASS" in log and "== cycle complete" in log
    assert any(line.startswith("fields check: 1 of 1 baseline fields byte-identical") for line in log)
    # the NAV line carries L and every pin resolved from the files just written
    nav = next(s for s in cycle_of(root, sp).steps() if s.phase == "nav")
    assert nav.argv[nav.argv.index("--aim-leverage") + 1] == "1.247"
    assert nav.argv[nav.argv.index("--combined-sha256") + 1] == RC.sha256_file(root / "out/WT-1/train_combined.json")
    assert run(root, sp) == RC.EXIT_OK                  # resume: every output exists -> only checks and summ rerun
    assert calls(root)[7:] == ["check", "summ --weights out/W/composition_weights.json --reference ref-cell "
                                        "--dsr-n 29 out/N"]
    st = RC.status_lines(cycle_of(root, sp))
    assert any(line.startswith("u      done") and "completed exit 0" in line for line in st)
    assert any("primary daily CSV out/N/daily_s2.csv sha256" in line for line in st)


def test_refused_receipt_hard_stops_and_is_never_overwritten(tmp_path):
    root, sp = make_root(tmp_path)
    behave(root, **{"U-run1": "refuse"})
    with pytest.raises(RC.CycleError) as e:
        run(root, sp)
    assert e.value.code == RC.EXIT_STOP and "prelaunch-memory-refusal" in str(e.value)
    assert calls(root) == ["fields ", "check", "U-run1"]            # nothing after the refusal ran
    behave(root)
    with pytest.raises(RC.CycleError, match="--attempt u=2"):       # the failed attempt stays; no auto-retry
        run(root, sp)
    assert calls(root) == ["fields ", "check", "U-run1", "check"]
    assert run(root, sp, attempts={"u": 2}) == RC.EXIT_OK
    assert calls(root)[4:7] == ["check", "U-run2", "W-run1"]
    assert cycle_of(root, sp).steps()[2].output == "out/U-2"       # later invocations find the complete attempt
    assert RC.main(["plan", str(sp), "--root", str(root), "--lines-only"]) == 0


@pytest.mark.parametrize("phase,beh,needle", [("WT-run1", "error", "process-error"),
                                              ("N-run", "noreceipt", "no receipt"),
                                              ("U-run1", "incomplete-output", "output is incomplete")])
def test_any_non_completed_receipt_or_output_stops(tmp_path, phase, beh, needle):
    root, sp = make_root(tmp_path)
    behave(root, **{phase: beh})
    with pytest.raises(RC.CycleError, match=needle):
        run(root, sp)
    assert calls(root)[-1] == phase
    assert not any(c.startswith("summ") for c in calls(root))


def test_fit_incomplete_resumes_with_the_next_pass_up_to_max_passes(tmp_path):
    root, sp = make_root(tmp_path)
    behave(root, **{"W-run1": "incomplete"})
    assert run(root, sp) == RC.EXIT_OK
    assert calls(root)[3:6] == ["W-run1", "W-run2", "WT-run1"]
    root2, sp2 = make_root(tmp_path / "b", fit={"script": "scripts/fit.py", "output": "out/W", "work_dir": "out/WORK",
                                                "flags": [], "max_passes": 1})
    behave(root2, **{"W-run1": "incomplete"})
    with pytest.raises(RC.CycleError, match="still incomplete"):
        run(root2, sp2)


def test_gate_failure_and_static_check_failure_stop(tmp_path):
    root, sp = make_root(tmp_path)
    behave(root, admission={"rules": {}, "candidates": [{"id": "new_alpha", "status": "rejected",
                                                         "sign_agrees": True}]})
    log = []
    with pytest.raises(RC.CycleError) as e:
        run(root, sp, log)
    assert e.value.code == RC.EXIT_GATE and "gate p1 FAIL: stop (no later phase runs)" in log
    assert calls(root)[-1] == "W-run1"
    root2, sp2 = make_root(tmp_path / "c")
    behave(root2, check=1)
    with pytest.raises(RC.CycleError, match=r"\[check\]: exit 1"):
        run(root2, sp2)
    assert calls(root2) == ["fields ", "check"]


def test_fields_check_failure_stop_after_dirty_tree_and_partial_outputs(tmp_path):
    root, sp = make_root(tmp_path)
    behave(root, fields="changed")
    with pytest.raises(RC.CycleError, match="fields check FAILED"):
        run(root, sp)
    root2, sp2 = make_root(tmp_path / "d")
    assert run(root2, sp2, stop_after="fields") == RC.EXIT_OK and calls(root2) == ["fields "]
    with pytest.raises(RC.CycleError, match="not clean"):
        RC.run_cycle(cycle_of(root2, sp2), clean=lambda r: False, log=lambda s: None)
    (root2 / "out" / "N").mkdir(parents=True)                       # a partial NAV output is never overwritten
    with pytest.raises(RC.CycleError, match=r"HARD-STOP \[nav\].*fresh --suffix"):
        run(root2, sp2)
    assert "N-run" not in calls(root2)
    root3, sp3 = make_root(tmp_path / "e")
    assert run(root3, sp3, suffix="r2", reuse_fields="fields-base", stop_after="u") == RC.EXIT_OK
    assert calls(root3)[0] == "fields --reuse --reuse-sha256" and calls(root3)[2] == "U-r2-run1"
    assert (root3 / "out" / "F-r2" / "manifest.json").exists() and (root3 / "out" / "U-r2-1").is_dir()


def test_keep_fields_and_runner_override(tmp_path):
    pins = json.loads((FIX / "v61_pins.json").read_text())
    c = v61_cycle(tmp_path, suffix="r7", keep_fields=True, runner_overrides=RC.parse_runner_overrides(["max_rss_mib=64"]))
    steps = {s.phase: s for s in c.steps()}
    fdm = "build-equity/recent-fast-train-2020-2022-v2-lo1-fields-v7/manifest.json"
    assert steps["fields"].state == "done" and steps["fields"].output == fdm.rsplit("/", 1)[0]
    u = steps["u"].argv
    assert steps["u"].output == "build-equity/mega-v61-train-u-r7-1" and steps["u"].state == "pending"
    assert u[u.index("--train-fields-sha256") + 1] == pins[fdm]            # the kept fields pin, resolved
    k = u.index("--")
    assert u[u.index("--max-rss-mib") + 1] == "64" and u.index("--max-rss-mib") < k       # the runner cap only
    assert u[u.index("--max-memory-mib") + 1] == "1536"
    assert RC.load_spec(V61)["runner"]["max_rss_mib"] == 1536                # the spec itself is untouched
    with pytest.raises(RC.CycleError):
        RC.parse_runner_overrides(["workers=2"])
    with pytest.raises(RC.CycleError, match="exclude"):
        v61_cycle(tmp_path, keep_fields=True, reuse_fields="x")


# ------------------------------------------------------------------ W3: card and monitor phases
V61_OPS = HERE.parent / "specs" / "v61-ops.json"

FAKE_MONITOR = r'''
import json, sys
from pathlib import Path
a = sys.argv[1:]
with open("calls.log", "a") as f:
    f.write("monitor " + " ".join(a) + "\n")
beh = json.loads(Path("behaviour.json").read_text()) if Path("behaviour.json").exists() else {}
if beh.get("monitor") == "fail":
    sys.exit(1)
out = Path(a[a.index("--output") + 1])
out.mkdir(parents=True)
(out / "monitor.json").write_text("{}")
'''


def test_v61_ops_is_v61_plus_the_card_and_monitor_sections():
    ops, v61 = RC.load_spec(V61_OPS), RC.load_spec(V61)
    assert ({k: v for k, v in ops.items() if k not in ("name", "description", "card", "monitor")} ==
            {k: v for k, v in v61.items() if k not in ("name", "description")})
    assert list(ops).index("card") == list(ops).index("fit") + 1 and list(ops).index("monitor") == list(ops).index("nav") + 1


def test_v61_ops_plan_adds_the_card_after_fit_and_the_monitor_after_nav(tmp_path):
    known = json.loads((FIX / "v61_pins.json").read_text(encoding="utf-8"))
    c = RC.Cycle(RC.load_spec(V61_OPS), RC.Resolver(tmp_path, known), spec_path=V61_OPS)
    steps = c.steps()
    assert [s.phase for s in steps] == list(RC.PHASES)
    lines = RC.plan_lines(c, lines_only=True)
    base = (FIX / "v61_train_dry.txt").read_text(encoding="utf-8").splitlines()
    card, monitor = (next(s for s in steps if s.phase == p) for p in ("card", "monitor"))
    assert [x for x in lines if x not in (RC.fmt_argv(card.argv), RC.fmt_argv(monitor.argv))] == base
    assert lines.index(RC.fmt_argv(card.argv)) == [s.phase for s in steps if s.argv].index("card")
    u, w = "build-equity/mega-v61-train-u-1", "build-equity/mega-weights-v61-ew"
    a = card.argv[card.argv.index("--") + 1:]
    assert a == [sys.executable if False else c.py, "atx-impl/tools/alpha_report_card.py", "--u-pass", u, "--train",
                 "build-equity/recent-fast-train-2020-2022-v2-lo1/manifest.json", "--train-sha256", c.pin("role"),
                 "--admission", f"{w}/admission.json", "--admission-sha256", f"<sha256:{w}/admission.json>",
                 "--workers", "4", "--output", "build-equity/mega-cards-v61"]
    assert card.run_dir == "build-equity/mega-cards-v61-run" and card.state == "pending"
    assert monitor.kind == "direct" and monitor.state == "pending"
    m = monitor.argv
    assert m[1:] == ["atx-impl/tools/book_monitor.py", "--baseline", "--daily-ic", f"{u}/train_daily_ic.csv",
                     "--admission", f"{w}/admission.json", "--fit-work", "build-equity/mega-fit-work-v61",
                     "--sleeve-daily", "build-equity/mega-cards-v61/daily_sleeve.csv", "--holdings-days",
                     "build-equity/v7-l3-holdings/holdings_days.csv", "--bias", "build-equity/v7-l4-risk/bias.csv",
                     "--output", "build-equity/mega-monitor-v61"]
    r7 = {s.phase: s for s in RC.Cycle(RC.load_spec(V61_OPS), RC.Resolver(tmp_path, known), spec_path=V61_OPS,
                                       suffix="r7").steps()}
    assert r7["card"].output == "build-equity/mega-cards-v61-r7"
    assert r7["monitor"].argv[r7["monitor"].argv.index("--fit-work") + 1] == "build-equity/mega-fit-work-v61-r7"


def ops_root(tmp_path, **over):
    root, sp = make_root(tmp_path, card={"script": "scripts/card.py", "output": "out/C", "flags": ["--workers", "2"]},
                         monitor={"script": "scripts/monitor.py", "output": "out/M", "bias": "risk/bias.csv"}, **over)
    (root / "scripts" / "card.py").write_text("")
    (root / "scripts" / "monitor.py").write_text(FAKE_MONITOR)
    return root, sp


def test_run_orders_card_before_the_gate_and_monitor_after_nav(tmp_path):
    root, sp = ops_root(tmp_path)
    log = []
    assert run(root, sp, log) == RC.EXIT_OK
    got = calls(root)
    assert got[:5] == ["fields ", "check", "U-run1", "W-run1", "C-run"]
    assert got[5:7] == ["WT-run1", "N-run"]
    assert got[7].startswith("monitor --baseline --daily-ic out/U-1/train_daily_ic.csv --admission "
                             "out/W/admission.json --fit-work out/WORK --sleeve-daily out/C/daily_sleeve.csv --bias "
                             "risk/bias.csv --output out/M")
    assert got[8].startswith("summ ")
    assert log.index("== card") < log.index("gate p1 PASS")
    lines = []
    assert run(root, sp, lines) == RC.EXIT_OK  # resume: every phase done, nothing re-executed
    assert calls(root)[len(got):] == ["check", got[-1]]  # only the always-run check and summ
    assert "== card: done (out/C)" in lines and "== monitor: done (out/M)" in lines


def test_card_output_is_needed_even_when_the_gate_fails(tmp_path):
    root, sp = ops_root(tmp_path)
    behave(root, admission={"rules": {}, "candidates": [{"id": "new_alpha", "status": "reject_veto",
                                                         "sign_agrees": True, "failed_checks": ["veto"]}]})
    with pytest.raises(RC.CycleError) as e:
        run(root, sp)
    assert e.value.code == RC.EXIT_GATE
    assert (root / "out" / "C" / "index.json").is_file()
    assert "C-run" in calls(root) and "WT-run1" not in calls(root)


def test_card_refusal_and_monitor_failure_stop(tmp_path):
    root, sp = ops_root(tmp_path)
    behave(root, **{"C-run": "error"})
    with pytest.raises(RC.CycleError) as e:
        run(root, sp)
    assert "HARD-STOP [card]" in str(e.value) and e.value.code == RC.EXIT_STOP
    root2, sp2 = ops_root(tmp_path / "second")
    behave(root2, monitor="fail")
    with pytest.raises(RC.CycleError) as e:
        run(root2, sp2)
    assert "HARD-STOP [monitor]: exit 1" in str(e.value)
    assert "summ" not in " ".join(calls(root2))


def test_card_and_monitor_need_their_upstream_sections():
    base = minimal_spec()
    for bad in (dict(base, card={"script": "c.py", "output": "C"}),
                dict(base, monitor={"script": "m.py", "output": "M"}),
                dict(base, card={"script": "c.py"})):
        with pytest.raises(RC.CycleError) as e:
            RC.validate_spec(bad)
        assert e.value.code == RC.EXIT_USAGE


# ------------------------------------------------------------------ L7: library v7.0 spec (no run) and gate.require
V70 = HERE.parent / "specs" / "v70.json"
V61_ADMISSION = "build-equity/mega-weights-v61-ew/admission.json"
V61_ADMISSION_SHA256 = "2c78216ddd7e30a639a77f89e7459a8deeaee2d5d8efd509863eed6a480089f0"  # the v6.1 fit (pool-2)
WAVE1 = ["qmj_safety", "nincr", "q5_eg", "smax5", "res_mom_ind"]
FIELDS_V7 = "build-equity/recent-fast-train-2020-2022-v2-lo1-fields-v7"
V61_CELL = "build-equity/mega-nav-v61u-ew-t.05-d.1-fixed-obdelta-x.05-loc-L1.247"
V70_CELL = "build-equity/mega-nav-v70u-ew-t.05-d.1-fixed-obdelta-x.05-loc-L1.247"
STUDIES = HERE.parents[1] / ".superpowers" / "sdd" / "mega-alpha-20260926" / "studies"


def nav_summ_args(argv: list[str]):
    """nav_summ.py's own argparse on argv: its main() is stopped right after parse_args (no NAV dir is read)."""
    import argparse
    import importlib
    from unittest import mock
    sys.path.insert(0, str(STUDIES))
    try:
        module = importlib.import_module("nav_summ")
    finally:
        sys.path.remove(str(STUDIES))
    real = argparse.ArgumentParser.parse_args

    class Parsed(Exception):
        pass

    def stop(self, args=None, namespace=None):
        raise Parsed(real(self, args, namespace))
    with mock.patch.object(argparse.ArgumentParser, "parse_args", stop), pytest.raises(Parsed) as got:
        module.main(argv)
    return got.value.args[0]


def v70_known() -> dict:
    """Hash-only pins: the v6.1 fixture pins, the v6.1 admission, and the committed v7.0 library / recipe files."""
    known = json.loads((FIX / "v61_pins.json").read_text(encoding="utf-8"))
    known[V61_ADMISSION] = V61_ADMISSION_SHA256
    for name in ("fund_industry_ic_v70.json", "fund_industry_ic_v70.recipe.json"):
        rel = f"atx-impl/strategies/{name}"
        known[rel] = RC.sha256_file(HERE.parents[1] / rel)
    return known


def v70_cycle(root: Path, **kw) -> RC.Cycle:
    return RC.Cycle(RC.load_spec(V70), RC.Resolver(root, v70_known()), spec_path=V70, **kw)


def test_v70_spec_is_v61_ops_with_the_declared_changes_only():
    v70, ops = RC.load_spec(V70), RC.load_spec(V61_OPS)
    assert list(v70) == list(ops)
    same = ("schema", "python", "env_path_prepend", "runner", "exes")
    assert {k: v70[k] for k in same} == {k: ops[k] for k in same}
    # inputs: v7.0 library / recipe, v6.1 as the baseline library, fields-v7 as the baseline fields, the v6.1 cell
    assert list(v70["inputs"]) == list(ops["inputs"])
    for key in ("role", "identity_bridge", "fund_events"):
        assert v70["inputs"][key] == ops["inputs"][key], key
    assert v70["inputs"]["baseline_library"] == dict(ops["inputs"]["library"])
    assert v70["inputs"]["baseline_fields"]["path"] == f"{FIELDS_V7}/manifest.json"
    assert v70["inputs"]["reference_admission"]["path"] == V61_ADMISSION
    assert v70["inputs"]["reference_cell"]["dir"] == ops["nav"]["output"]  # the v6.1 final cell
    # fields: lo1-fields-v7 as-is (the same builder line), nothing added
    strip = lambda f: {k: v for k, v in f.items() if k != "check"}
    assert strip(v70["fields"]) == strip(ops["fields"])
    assert v70["fields"]["output"] == ops["fields"]["output"] == FIELDS_V7
    assert v70["fields"]["check"] == dict(ops["fields"]["check"], expect_added=[])
    assert v70["static_check"] == {"script": ops["static_check"]["script"],
                                   "args": ["--max-roster", "56", "--require-baseline-prefix"], "expect_added": WAVE1}
    assert v70["ic"]["flags"] == ops["ic"]["flags"] + ["--cache-legacy-fields", FIELDS_V7]
    assert (v70["ic"]["u_output"], v70["ic"]["w_output"], v70["ic"]["cache"]) == (
        "build-equity/mega-v70-train-u", "build-equity/mega-v70w-train-ew", "build-equity/mega-candidate-cache-v70")
    assert v70["fit"] == dict(ops["fit"], output="build-equity/mega-weights-v70-ew",
                              work_dir="build-equity/mega-fit-work-v70")
    assert v70["card"] == dict(ops["card"], output="build-equity/mega-cards-v70")
    assert v70["gate"] == {"name": "p1-v70", "admitted": WAVE1, "require": "any", "sign_agrees": True,
                           "report": ["bac", "smax", "droe", "sue", "cbop", "res_mom_12_1", "within_ind_mom"]}
    assert v70["nav"] == dict(ops["nav"], output="build-equity/mega-nav-v70u-ew-t.05-d.1-fixed-obdelta-x.05-loc-L1.247")
    assert v70["monitor"] == dict(ops["monitor"], output="build-equity/mega-monitor-v70")
    cells = v70["summ"]["cells"]  # the n33 scoring grid (mega-nav-v7-summ-n33.json), then this cycle's v7.0 cell
    assert v70["summ"] == dict(ops["summ"], dsr_n=34, cells=cells, extra=["--effective-n", "dirs", "--psr", "--pbo"],
                               ledger="build-equity/trials.jsonl")
    assert len(cells) == len(set(cells)) == 33 and cells[-5:] == [
        f"{V61_CELL}{x}" for x in ("", "-v6C1", "-v6C2", "-v6C3")] + ["build-equity/mega-nav-v61u-spo-v1-L1.247"]
    assert v70["inputs"]["reference_cell"]["dir"] == V61_CELL and v70["nav"]["output"] == V70_CELL not in cells


def test_v70_plan_dry_run_resolves_every_input_pin(tmp_path):
    c = v70_cycle(tmp_path)
    spec, known = RC.load_spec(V70), v70_known()
    for key, item in spec["inputs"].items():  # every pin is the file's SHA-256 (as `lock` computed it)
        assert item["sha256"] == known[item["path"]], key
    assert all(how == "locked (hash-only)" for _, _, how in c.pins.values())
    steps = {s.phase: s for s in c.steps()}
    assert list(steps) == list(RC.PHASES)
    assert {p: s.state for p, s in steps.items()} == {
        "fields": "done", "check": "always", "u": "pending", "fit": "pending", "card": "pending", "gate": "always",
        "w": "pending", "nav": "pending", "monitor": "pending", "summ": "always"}
    lines = RC.plan_lines(c)
    assert lines[0].startswith("# research_cycle v70: spec") and len([x for x in lines if x.startswith("# pin ")]) == 9
    assert len(RC.plan_lines(c, lines_only=True)) == 9  # every phase but the internal gate prints its command line
    fdm = f"{FIELDS_V7}/manifest.json"
    ck = steps["check"].argv
    assert ck[ck.index("--manifest") + 1] == fdm and ck[-2:] == ["--expect-added", ",".join(WAVE1)]
    assert ck[ck.index("--baseline") + 1] == "atx-impl/strategies/fund_industry_ic_v61.json" and "56" in ck
    u = steps["u"].argv
    assert u[u.index("--train-fields-sha256") + 1] == known[fdm]  # the fields pin resolves (fields-v7 exists)
    assert u[u.index("--library-sha256") + 1] == known["atx-impl/strategies/fund_industry_ic_v70.json"]
    assert u[u.index("--cache-legacy-fields") + 1] == FIELDS_V7
    assert u[-2:] == ["--candidate-cache", "build-equity/mega-candidate-cache-v70"]
    assert steps["u"].output == "build-equity/mega-v70-train-u-1"
    nav = steps["nav"].argv
    k = nav.index("--")
    assert nav[nav.index("--rule") + 1] == "aim-partial-v5" and nav[nav.index("--aim-leverage") + 1] == "1.247"
    assert nav[k + 1:k + 3] == ["build-equity/bin/atx-equity-strategy-targets.exe", "nav"]
    summ = steps["summ"].argv
    r, grid = summ.index("--reference"), spec["summ"]["cells"] + [V70_CELL]
    assert summ[2:r] == ["--weights", "build-equity/mega-weights-v70-ew/composition_weights.json"]
    assert summ[r + 1] == V61_CELL and summ[r + 2:r + 36] == grid  # the 34-cell grid, one positional block
    assert summ[r + 36:] == ["--dsr-n", "34", "--effective-n", "dirs", "--psr", "--pbo", "--ledger",
                             "build-equity/trials.jsonl", "--ledger-kind", "construction"]
    ns = nav_summ_args(summ[2:])  # nav_summ's own argparse: the grid as dirs, an empty --pbo (= the dirs), the ledger
    assert (ns.dirs, ns.pbo, ns.dsr_n, ns.reference, ns.effective_n, ns.psr) == (grid, [], 34, V61_CELL, "dirs", True)
    assert (ns.ledger, ns.ledger_kind, ns.weights) == ("build-equity/trials.jsonl", "construction", [summ[3]])
    assert v70_cycle(tmp_path, ledger="other.jsonl").steps()[-1].argv[-3:] == [  # the CLI --ledger overrides
        "other.jsonl", "--ledger-kind", "construction"]
    # the CLI prints the same plan (hash-only roots are not reachable from the CLI: resolve against a fake root)
    assert RC.main(["plan", str(V70), "--root", str(tmp_path)]) == RC.EXIT_PIN  # inputs absent there: a pin stop


@pytest.mark.skipif(not os.environ.get("RESEARCH_CYCLE_LIVE_ROOT"), reason="set RESEARCH_CYCLE_LIVE_ROOT to hash the "
                                                                          "real pool-2 files (after the L7 merge)")
def test_v70_plan_live_root_resolves():
    cycle = RC.Cycle(RC.load_spec(V70), RC.Resolver(Path(os.environ["RESEARCH_CYCLE_LIVE_ROOT"])), spec_path=V70)
    assert all(how == "locked, verified" for _, _, how in cycle.pins.values())
    assert next(s for s in cycle.steps() if s.phase == "fields").state == "done"


def two_member_root(tmp_path, admission, **gate):
    root, sp = make_root(tmp_path, gate={"name": "p1", "admitted": ["a1", "a2"], "sign_agrees": True, **gate})
    behave(root, admission={"rules": {}, "candidates": admission})
    return root, sp


def test_gate_require_any_passes_with_one_admitted_member_and_prints_every_row(tmp_path):
    rows = [{"id": "a1", "status": "reject_redundant", "sign_agrees": True},
            {"id": "a2", "status": "admitted", "sign_agrees": True}]
    root, sp = two_member_root(tmp_path, rows, require="any")
    log = []
    assert run(root, sp, log) == RC.EXIT_OK
    assert any(x.startswith("gate p1 a1: status reject_redundant") for x in log)
    assert any(x.startswith("gate p1 a2: status admitted") for x in log)
    assert "gate p1: 1 of 2 listed candidates admitted with the prior sign (require any)" in log
    assert "gate p1 PASS" in log and "WT-run1" in calls(root)
    root2, sp2 = two_member_root(tmp_path / "all", rows)  # default require all: the same rows stop the cycle
    with pytest.raises(RC.CycleError) as e:
        run(root2, sp2)
    assert e.value.code == RC.EXIT_GATE and "WT-run1" not in calls(root2)


def test_gate_require_any_stops_when_none_is_admitted_with_its_sign(tmp_path):
    rows = [{"id": "a1", "status": "reject_veto", "sign_agrees": True},
            {"id": "a2", "status": "admitted", "sign_agrees": False}]
    root, sp = two_member_root(tmp_path, rows, require="any")
    log = []
    with pytest.raises(RC.CycleError) as e:
        run(root, sp, log)
    assert e.value.code == RC.EXIT_GATE and "gate p1 FAIL: stop (no later phase runs)" in log
    assert "gate p1: 0 of 2 listed candidates admitted with the prior sign (require any)" in log
    assert "WT-run1" not in calls(root)
    with pytest.raises(RC.CycleError) as e:
        RC.validate_spec(dict(RC.load_spec(V70), gate=dict(RC.load_spec(V70)["gate"], require="most")))
    assert e.value.code == RC.EXIT_USAGE


def test_summ_grid_cells_and_spec_ledger_reach_nav_summ_and_a_suffix_renames_only_this_cell(tmp_path):
    summ = {"script": "scripts/summ.py", "dsr_n": 3, "cells": ["prior/a", "prior/b"], "extra": ["--psr", "--pbo"],
            "ledger": "trials.jsonl"}
    root, sp = make_root(tmp_path, summ=summ)
    assert run(root, sp) == RC.EXIT_OK
    assert calls(root)[-1] == ("summ --weights out/W/composition_weights.json --reference ref-cell prior/a prior/b "
                               "out/N --dsr-n 3 --psr --pbo --ledger trials.jsonl --ledger-kind construction")
    s = next(x for x in cycle_of(root, sp, suffix="r2").steps() if x.phase == "summ").argv
    assert s[s.index("--reference") + 2:s.index("--dsr-n")] == ["prior/a", "prior/b", "out/N-r2"]
    root2, sp2 = make_root(tmp_path / "plain")  # no cells, no spec ledger: the v6.1 layout (this cell last)
    assert run(root2, sp2, ledger="L.jsonl") == RC.EXIT_OK
    assert calls(root2)[-1].endswith("--dsr-n 29 --ledger L.jsonl --ledger-kind construction out/N")


@pytest.mark.parametrize("cells, why", [
    (["prior/a"], "must be the declared N"),                  # 1 + 1 != dsr_n 3
    (["prior/a", "prior/a"], "distinct prior NAV dirs"),
    (["prior/a", "out/N"], "distinct prior NAV dirs"),         # this cycle's own output
    ([], "distinct prior NAV dirs"),
    ("prior/a", "distinct prior NAV dirs"),
])
def test_summ_cells_are_refused_unless_they_are_the_declared_grid(tmp_path, cells, why):
    _, sp = make_root(tmp_path, summ={"script": "scripts/summ.py", "dsr_n": 3, "cells": cells})
    with pytest.raises(RC.CycleError) as e:
        RC.load_spec(sp)
    assert e.value.code == RC.EXIT_USAGE and why in str(e.value)
