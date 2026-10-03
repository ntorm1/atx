"""research_cycle.py (platform v7 L2): spec parsing, pin resolution, phase ordering, stop-on-refusal, v6.1 identity.

Run: "C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider scripts/tests/test_research_cycle.py

No real run: the v6.1 plan is resolved in hash-only mode against fixtures/v61_pins.json (SHA-256 of the pool-2 files,
captured by fixtures/gen_v61_dry.sh together with v61_train.sh's DRY=1 command lines in fixtures/v61_train_dry.txt);
the `run` tests drive fake runner / builder / check / summ scripts in a temporary root. Set
RESEARCH_CYCLE_LIVE_ROOT=C:/atx-wt/pool-2 to also resolve the plan against the real files (read-only hashing).
"""
from __future__ import annotations

import hashlib
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
    assert [s.phase for s in steps] == [p for p in RC.PHASES if p not in ("card", "monitor", "ref", "marginal")]  # W3
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


def test_sic_events_input_reaches_the_fields_builder_and_the_lo3_template_refuses_until_filled(tmp_path):
    # platform v7 U2: inputs.sic_events (the atx-db fundamentals stage manifest) -> --sic-events DIR --sic-events-sha256
    pins = {"lib.json": "a" * 64, "role/manifest.json": "b" * 64, "stage/manifest.json": "c" * 64}
    inputs = {"library": {"path": "lib.json", "sha256": None},
              "role": {"dir": "role", "path": "role/manifest.json", "sha256": None},
              "sic_events": {"dir": "stage", "path": "stage/manifest.json", "sha256": "c" * 64}}
    fields = {"builder": "b.py", "output": "F", "list": ["grp_ff12"]}
    for with_sic in (True, False):
        spec = minimal_spec(inputs={k: v for k, v in inputs.items() if with_sic or k != "sic_events"}, fields=fields)
        argv = RC.Cycle(spec, RC.Resolver(tmp_path, pins)).fields_step("F", "F/manifest.json", "b" * 64).argv
        if with_sic:
            i = argv.index("--sic-events")
            assert argv[i:i + 4] == ["--sic-events", "stage", "--sic-events-sha256", "c" * 64]
        else:
            assert "--sic-events" not in argv
    spec = RC.load_spec(HERE.parent / "specs" / "v7u-lo3.json")   # the template validates ...
    assert spec["inputs"]["role"]["universe"] == "linked-operating-v3" and "sic_events" in spec["inputs"]
    with pytest.raises(RC.CycleError) as e:                         # ... and stops before any phase until filled
        RC.Cycle(spec, RC.Resolver(tmp_path))
    assert e.value.code == RC.EXIT_PIN


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
if b in ("floor-kill", "floor-kill-partial"):    # P9 OR-4: the host memory floor killed it (before / after a write)
    if b == "floor-kill-partial":
        Path(cmd[cmd.index("--output") + 1]).mkdir(parents=True, exist_ok=True)
    receipt("system-memory-limit", 1); sys.exit(1)
if b == "incomplete":
    (out / "stdout.log").write_text('{"status": "incomplete", "partial": true}'); receipt("process-error", 3); sys.exit(1)
for rel, text in beh.get("touch", {}).get(out.name, []):   # v8 A-3: a file written while this phase runs
    Path(rel).parent.mkdir(parents=True, exist_ok=True)
    Path(rel).write_text(text)
if "marginal" in cmd and any(k not in cmd for k in @MARGINAL_REQUIRED@):   # A2: the verb's required options
    print("marginal IC: bounded config", file=sys.stderr); receipt("process-error", 1); sys.exit(1)
if len(cmd) > 1 and Path(cmd[1]).name in ("fields.py", "check.py", "summ.py", "monitor.py"):  # v8: every-phase
    import subprocess
    code = subprocess.run(cmd).returncode
    receipt("completed" if code == 0 else "process-error", code)
    sys.exit(0 if code == 0 else 1)
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
    (child / "daily_s2.csv").write_text(beh.get("daily", {}).get(child.name, "net_return\n0.001\n"))
    if "--capacity-curve" in cmd and b != "capacity-crash":   # the NAV verb writes these after summary.json (NV-4)
        (child / "capacity_curve.csv").write_text("multiple,book,net_sharpe\n4,s2,1.0\n")
        (child / "v7_extras.json").write_text(json.dumps({"capacity_curve": True}))
elif "marginal" in cmd:                                     # v8 A-2: the IC exe's marginal verb (contract K6)
    (child / "marginal_ic.json").write_text(json.dumps({"candidates": [
        {"id": "new_alpha", "ic21": 0.012, "ic21_hac_t": 2.1, "marginal_ic21": 0.008, "marginal_hac_t": 1.6,
         "max_abs_rho": 0.31, "max_rho_member": "old"}]}))
else:
    (child / "summary.json").write_text(json.dumps({"status": "complete" if b != "incomplete-output" else "partial"}))
    (child / "orientations.json").write_text("{}")
    (child / "train_combined.json").write_text("{}")
    for name, text in beh.get("ic_files", {}).get(child.name, {}).items():  # L8: identity-compare fixtures
        (child / name).write_bytes(text.encode())
receipt("completed", 0)
'''
# The marginal verb's required options, read from atx-impl/src/strategy_marginal_ic.cpp (run_marginal_ic's refusal of an
# unbounded config) and pinned; test_marginal_argv_is_the_verbs_full_cli re-reads the C++ and fails if it moves.
VERB_REQUIRED = ("--candidate-cache", "--library", "--pool", "--role", "--output")
VERB_OPTIONS = {"--candidate-cache", "--library", "--library-sha256", "--pool", "--pool-sha256", "--role", "--themes",
                "--fields", "--output", "--min-names", "--max-memory-mib"}   # dispatch_marginal_ic, each takes a value
FAKE_RUNNER = FAKE_RUNNER.replace("@MARGINAL_REQUIRED@", repr(VERB_REQUIRED))

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
    assert [s.phase for s in steps] == [p for p in RC.PHASES if p not in ("ref", "marginal")]  # L8 ref: identity specs
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
    assert list(steps) == [p for p in RC.PHASES if p not in ("ref", "marginal")]  # v8 marginal: add-alpha specs
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


# ------------------------------------------------------------------ L8: as-built fields, ref phase, compare, ledger grid
V71 = HERE.parent / "specs" / "v71.json"
WAVE2 = ["ins_opp", "inst_best_ideas", "ftd_fail", "ea_overdue"]
FIELDS_V9 = "build-equity/recent-fast-train-2020-2022-v2-lo1-fields-v9"
FIELDS_V9_SHA256 = "8fd00e9f44b475116f483e133c03fe031618390060b8374180278f1cd7b8769b"
V71_CELL = "build-equity/mega-nav-v71u-ew-t.05-d.1-fixed-obdelta-x.05-loc-L1.247"
REF_V9 = "build-equity/mega-nav-v70u-ref-fields-v9"
S2_CSV = "daily_modeled-1bn-stale5-v1+swap-fin-v1.csv"
LO3_CELL = "build-equity/mega-nav-v70-lo3-ew-t.05-d.1-fixed-obdelta-x.05-loc-L1.247"


def v71_known() -> dict:
    """Hash-only pins: the v6.1 fixture pins, the pool-2 files v7.1 pins (fixtures/v71_pins.json, captured from the
    files) and the committed v7.0 / v7.1 library files."""
    known = json.loads((FIX / "v61_pins.json").read_text(encoding="utf-8"))
    known.update(json.loads((FIX / "v71_pins.json").read_text(encoding="utf-8")))
    for name in ("fund_industry_ic_v70.json", "fund_industry_ic_v71.json", "fund_industry_ic_v71.recipe.json"):
        rel = f"atx-impl/strategies/{name}"
        known[rel] = RC.sha256_file(HERE.parents[1] / rel)
    return known


def cell_line(cell: str) -> str:
    """A legacy (v7, unchained) construction line of the trial ledger with the keys the readers use."""
    tid = hashlib.sha256(cell.encode()).hexdigest()[:16]
    return json.dumps({"schema": "atx.trial-ledger/v1", "kind": "construction", "count": 1, "cell": cell,
                       "trial_id": tid}, sort_keys=True) + "\n"


def write_ledger(root: Path, cells: list[str]) -> None:
    p = root / "build-equity" / "trials.jsonl"
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text("".join(cell_line(c) for c in cells), encoding="utf-8")


def v70_grid() -> list[str]:
    """The 34 ledger lines when v7.1 was declared: the n33 grid of spec v70, then the v7.0 cell."""
    return RC.load_spec(V70)["summ"]["cells"] + [V70_CELL]


def v71_cycle(root: Path, spec: dict | None = None, **kw) -> RC.Cycle:
    return RC.Cycle(spec or RC.load_spec(V71), RC.Resolver(root, v71_known()), spec_path=V71, **kw)


def test_v71_spec_is_v70_with_the_declared_changes_only():
    v71, v70 = RC.load_spec(V71), RC.load_spec(V70)
    same = ("schema", "python", "env_path_prepend", "runner", "exes")
    assert {k: v71[k] for k in same} == {k: v70[k] for k in same}
    inp = v71["inputs"]
    assert inp["library"]["path"] == "atx-impl/strategies/fund_industry_ic_v71.json"
    assert inp["baseline_library"] == dict(v70["inputs"]["library"])            # the parent: library v7.0
    for key in ("role", "baseline_fields"):
        assert inp[key] == v70["inputs"][key], key
    assert "identity_bridge" not in inp and "fund_events" not in inp            # fields as built: nothing to build
    assert inp["reference_cell"]["dir"] == V70_CELL == v70["nav"]["output"]     # Ruling W2-a: the v7.0 cell
    assert inp["reference_admission"]["path"] == f"{v70['fit']['output']}/admission.json"
    assert inp["reference_combined"]["path"] == f"{v70['ic']['w_output']}-1/train_combined.json"
    assert inp["reference_daily"]["path"] == f"{V70_CELL}/{S2_CSV}"
    assert inp["reference_orientations"]["path"] == f"{v70['ic']['u_output']}-1/orientations.json"
    assert inp["reference_daily_ic"]["path"] == f"{v70['ic']['u_output']}-1/train_daily_ic.csv"
    f = v71["fields"]
    assert (f["output"], f["manifest_sha256"]) == (FIELDS_V9, FIELDS_V9_SHA256) and "builder" not in f
    assert len(f["list"]) == 63 and f["list"][:41] == v70["fields"]["list"]
    assert f["check"] == dict(v70["fields"]["check"], expect_added=f["list"][41:])
    assert v71["static_check"] == dict(v70["static_check"], expect_added=WAVE2)
    assert v71["ref"] == {"output": REF_V9, "combined": "reference_combined"}
    assert [(c["name"], c["after"], c["mode"], c["a"], c["b"], c.get("keys")) for c in v71["compare"]] == [
        ("ref-v9-s2-daily", "ref", "file", "{input:reference_daily}", "{out:ref}/" + S2_CSV, None),
        ("parent-orientations", "u", "json-rows", "{input:reference_orientations}", "{out:u}/orientations.json",
         "{input:baseline_library}"),                                  # the 44 parent members' rows only
        ("parent-train-daily-ic", "u", "csv-rows", "{input:reference_daily_ic}", "{out:u}/train_daily_ic.csv",
         "{input:baseline_library}")]
    assert v71["ic"] == dict(v70["ic"], u_output="build-equity/mega-v71-train-u", w_output="build-equity/mega-v71w-"
                             "train-ew", cache="build-equity/mega-candidate-cache-v71")  # flags unchanged
    assert v71["fit"] == dict(v70["fit"], output="build-equity/mega-weights-v71-ew",
                              work_dir="build-equity/mega-fit-work-v71")
    assert v71["card"] == dict(v70["card"], output="build-equity/mega-cards-v71")
    assert v71["monitor"] == dict(v70["monitor"], output="build-equity/mega-monitor-v71")
    assert v71["gate"] == {"name": "p1-v71", "admitted": WAVE2, "require": "any", "sign_agrees": True,
                           "report": ["mom_12_1", "si_ratio", "dtc", "sv_flow", "iv_rv_spread", "ear", "sue", "droe"]}
    assert v71["nav"] == dict(v70["nav"], output=V71_CELL)                       # the reference construction
    assert v71["summ"] == {"script": v70["summ"]["script"], "dsr_n": "ledger+1", "cells_from_ledger": True,  # v8 A-3
                           "extra": v70["summ"]["extra"], "ledger": v70["summ"]["ledger"],
                           "ledger_kind": "construction"}


def test_v71_plan_dry_run_resolves_every_pin_and_the_identity_phases(tmp_path):
    write_ledger(tmp_path, v70_grid())
    c = v71_cycle(tmp_path)
    spec, known = RC.load_spec(V71), v71_known()
    for key, item in spec["inputs"].items():  # every pin is the file's SHA-256 (as `lock` computed it)
        assert item["sha256"] == known[item["path"]], key
    assert all(how == "locked (hash-only)" for _, _, how in c.pins.values())
    steps = {s.phase: s for s in c.steps()}
    assert list(steps) == ["fields", "check", "ref", "ref-compare", "u", "u-compare", "fit", "card", "gate", "w",
                           "nav", "monitor", "summ"]
    assert {p: s.state for p, s in steps.items()} == {
        "fields": "done", "check": "always", "ref": "pending", "ref-compare": "always", "u": "pending",
        "u-compare": "always", "fit": "pending", "card": "pending", "gate": "always", "w": "pending", "nav": "pending",
        "monitor": "pending", "summ": "always"}
    assert steps["fields"].argv is None and steps["fields"].output == FIELDS_V9   # as built: no builder line
    fdm = f"{FIELDS_V9}/manifest.json"
    # ref: the v7.0 cell's own NAV command (its receipt), with only the fields pin and the output changed
    receipt = json.loads((FIX / "v70_nav_receipt_command.json").read_text(encoding="utf-8"))
    assert (receipt["outcome"], receipt["exit_code"]) == ("completed", 0)
    want = list(receipt["command"][1:])
    for flag, value in (("--fields", fdm), ("--fields-sha256", FIELDS_V9_SHA256), ("--output", REF_V9)):
        want[want.index(flag) + 1] = value
    ref = steps["ref"].argv
    k = ref.index("--")
    assert ref[k + 1] == spec["exes"]["nav"] and ref[k + 2:] == want
    assert steps["ref"].run_dir == f"{REF_V9}-run" and ref[ref.index("--output") + 1] == f"{REF_V9}-run"
    assert [(x["a"], x["b"]) for x in steps["ref-compare"].checks] == [
        (f"{V70_CELL}/{S2_CSV}", f"{REF_V9}/{S2_CSV}")]
    u_out = "build-equity/mega-v71-train-u-1"
    lib70 = "atx-impl/strategies/fund_industry_ic_v70.json"
    assert [(x["mode"], x["a"], x["b"], x["keys"]) for x in steps["u-compare"].checks] == [
        ("json-rows", "build-equity/mega-v70-train-u-1/orientations.json", f"{u_out}/orientations.json", lib70),
        ("csv-rows", "build-equity/mega-v70-train-u-1/train_daily_ic.csv", f"{u_out}/train_daily_ic.csv", lib70)]
    assert len(RC.compare_keys(RC.Resolver(HERE.parents[1]), lib70)) == 44
    u = steps["u"].argv
    assert u[u.index("--train-fields") + 1] == FIELDS_V9 and u[u.index("--train-fields-sha256") + 1] == FIELDS_V9_SHA256
    assert u[u.index("--library-sha256") + 1] == known["atx-impl/strategies/fund_industry_ic_v71.json"]
    assert u[u.index("--cache-legacy-fields") + 1] == FIELDS_V7
    assert u[-2:] == ["--candidate-cache", "build-equity/mega-candidate-cache-v71"] and steps["u"].output == u_out
    ck = steps["check"].argv
    assert ck[ck.index("--manifest") + 1] == fdm and ck[-2:] == ["--expect-added", ",".join(WAVE2)]
    nav = steps["nav"].argv
    assert nav[nav.index("--fields") + 1] == fdm and nav[nav.index("--fields-sha256") + 1] == FIELDS_V9_SHA256
    assert nav[nav.index("--rule") + 1] == "aim-partial-v5" and nav[nav.index("--aim-leverage") + 1] == "1.247"
    assert nav[nav.index("--output") + 1] == f"{V71_CELL}-run"
    summ = steps["summ"].argv
    r, grid = summ.index("--reference"), v70_grid() + [V71_CELL]
    assert summ[r + 1] == V70_CELL and summ[r + 2:r + 37] == grid                # 34 ledger cells + this one
    assert summ[r + 37:] == ["--dsr-n", "35", "--effective-n", "dirs", "--psr", "--pbo", "--ledger",
                             "build-equity/trials.jsonl", "--ledger-kind", "construction"]
    ns = nav_summ_args(summ[2:])
    assert (ns.dirs, ns.pbo, ns.dsr_n, ns.reference) == (grid, [], 35, V70_CELL)
    lines = RC.plan_lines(c)
    assert sum(x.startswith("#   compare ") for x in lines) == 3 and len([x for x in lines if x.startswith("# pin ")]) == 11
    assert len(RC.plan_lines(c, lines_only=True)) == 9  # check, ref, u, fit, card, w, nav, monitor, summ


def test_v71_ledger_grid_follows_the_ledger_and_dsr_n_is_the_one_key(tmp_path):
    write_ledger(tmp_path, v70_grid() + [LO3_CELL])  # another cell ledgered before v7.1 runs (e.g. U-lo3)
    declared = RC.load_spec(V71)
    declared["summ"]["dsr_n"] = 35                   # an integer N (the v7 rule): a stale one stops and names the value
    with pytest.raises(RC.CycleError) as e:
        v71_cycle(tmp_path, declared).steps()
    assert e.value.code == RC.EXIT_PIN and "lists 35 prior cells" in str(e.value)
    assert "set summ.dsr_n to 36" in str(e.value)
    ledger_n = next(s for s in v71_cycle(tmp_path).steps() if s.phase == "summ").argv  # v8 A-3: "ledger+1"
    spec = RC.load_spec(V71)
    spec["summ"]["dsr_n"] = 36                       # root's one-key change (v7)
    assert next(s for s in v71_cycle(tmp_path, spec).steps() if s.phase == "summ").argv == ledger_n
    summ = next(s for s in v71_cycle(tmp_path, spec).steps() if s.phase == "summ").argv
    r = summ.index("--reference")
    assert summ[r + 2:summ.index("--dsr-n")] == v70_grid() + [LO3_CELL, V71_CELL]
    assert summ[summ.index("--dsr-n") + 1] == "36"
    write_ledger(tmp_path, v70_grid() + [LO3_CELL, V71_CELL])  # after its own summ: this cell is not a prior cell
    assert next(s for s in v71_cycle(tmp_path, spec).steps() if s.phase == "summ").argv == summ
    (tmp_path / "build-equity" / "trials.jsonl").unlink()
    with pytest.raises(RC.CycleError, match="no trial ledger") as e:
        v71_cycle(tmp_path).steps()
    assert e.value.code == RC.EXIT_PIN


@pytest.mark.skipif(not os.environ.get("RESEARCH_CYCLE_LIVE_ROOT"), reason="set RESEARCH_CYCLE_LIVE_ROOT to hash the "
                                                                          "real pool-2 files (after the L8 merge)")
def test_v71_plan_live_root_resolves():
    cycle = RC.Cycle(RC.load_spec(V71), RC.Resolver(Path(os.environ["RESEARCH_CYCLE_LIVE_ROOT"])), spec_path=V71)
    assert all(how == "locked, verified" for _, _, how in cycle.pins.values())
    assert next(s for s in cycle.steps() if s.phase == "fields").state == "done"


# ---------------------------------------------------------------- generic: fake tools in a temporary root
def l8_root(tmp_path: Path, **spec_over) -> tuple[Path, Path]:
    """make_root plus a ref phase and identity compares (their inputs are files in the temporary root)."""
    root, sp = make_root(tmp_path)
    spec = json.loads(sp.read_text())
    (root / "ref-w" / "train_combined.json").write_text('{"ref": 1}')
    (root / "ref-cell" / "daily_s2.csv").write_text("net_return\n0.001\n")  # text mode, as the fake NAV writes it
    (root / "ref-u").mkdir()
    (root / "ref-u" / "daily.csv").write_bytes(b"id,h,v\np1,5,0.1\np1,21,0.2\np2,5,0.3\n")  # bytes, as the fake IC
    (root / "ref-u" / "orientations.json").write_bytes(json.dumps({"candidates": [{"id": "p1", "sign": 1},
                                                                                  {"id": "p2", "sign": -1}]}).encode())
    spec["inputs"].update({k: {"path": p, "sha256": None} for k, p in (
        ("reference_combined", "ref-w/train_combined.json"), ("reference_daily", "ref-cell/daily_s2.csv"),
        ("reference_daily_ic", "ref-u/daily.csv"), ("reference_orientations", "ref-u/orientations.json"))})
    spec["ref"] = {"output": "out/REF", "combined": "reference_combined"}
    spec["compare"] = [
        {"name": "ref-s2", "after": "ref", "mode": "file", "a": "{input:reference_daily}", "b": "{out:ref}/daily_s2.csv"},
        {"name": "rows-ic", "after": "u", "mode": "csv-rows", "a": "{input:reference_daily_ic}",
         "b": "{out:u}/train_daily_ic.csv"},
        {"name": "rows-or", "after": "u", "mode": "json-rows", "a": "{input:reference_orientations}",
         "b": "{out:u}/orientations.json"}]
    spec.update(spec_over)
    sp.write_text(json.dumps(spec), encoding="utf-8")
    return root, sp


GOOD_U = {"U-1": {"train_daily_ic.csv": "id,h,v\np1,5,0.1\np1,21,0.2\np2,5,0.3\nn1,5,0.9\n",
                  "orientations.json": json.dumps({"schema": "x", "candidates": [
                      {"sign": 1, "id": "p1"}, {"id": "p2", "sign": -1}, {"id": "n1", "sign": 1}]})}}


def test_ref_phase_and_identity_compares_run_in_order_and_resume(tmp_path):
    root, sp = l8_root(tmp_path)
    behave(root, ic_files=GOOD_U)
    log = []
    assert run(root, sp, log) == RC.EXIT_OK
    assert calls(root)[:5] == ["fields ", "check", "REF-run", "U-run1", "W-run1"]
    assert log.index("== ref (attempt 1)") < log.index("compare ref-s2 [file]: ref-cell/daily_s2.csv vs out/REF/daily_s2.csv") \
        < log.index("== u (attempt 1)") < log.index("compare rows-ic [csv-rows]: ref-u/daily.csv vs "
                                                    "out/U-1/train_daily_ic.csv")
    size, sha = (root / "ref-cell/daily_s2.csv").stat().st_size, RC.sha256_file(root / "ref-cell/daily_s2.csv")
    assert f"   IDENTICAL: bit for bit ({size} bytes, sha256 {sha})" in log
    assert ("   IDENTICAL: 3 rows of 2 keys byte for byte (a has 0 rows of other keys); b adds 1 rows of 1 other "
            "keys") in log
    assert "   IDENTICAL: 2 candidates objects identical (canonical JSON); b adds 1 objects" in log
    ref = next(s for s in cycle_of(root, sp).steps() if s.phase == "ref").argv
    assert ref[ref.index("--combined") + 1] == "ref-w/train_combined.json"
    assert ref[ref.index("--combined-sha256") + 1] == RC.sha256_file(root / "ref-w/train_combined.json")
    assert ref[ref.index("--aim-leverage") + 1] == "1.247" and ref[ref.index("--rule") + 1] == "aim-partial-v5"
    n = len(calls(root))
    assert run(root, sp) == RC.EXIT_OK                  # resume: nothing re-executed, the compares re-verified
    assert calls(root)[n:] == ["check", calls(root)[n - 1]]
    st = RC.status_lines(cycle_of(root, sp))
    assert any(x.startswith("ref    done") for x in st) and any(x.startswith("u-compare always") for x in st)


def test_a_reference_mismatch_hard_stops_before_the_u_pass(tmp_path):
    root, sp = l8_root(tmp_path)
    behave(root, daily={"REF": "net_return\n0.0010000001\n"}, ic_files=GOOD_U)
    with pytest.raises(RC.CycleError) as e:
        run(root, sp)
    assert e.value.code == RC.EXIT_STOP and "IDENTITY MISMATCH [ref-s2] (file)" in str(e.value)
    assert "no trial" in str(e.value) and calls(root) == ["fields ", "check", "REF-run"]


@pytest.mark.parametrize("files, needle", [
    ({"train_daily_ic.csv": "id,h,v\np1,5,0.1\np1,21,0.25\np2,5,0.3\n"}, "rows-ic"),          # a changed value
    ({"train_daily_ic.csv": "id,h,v\np1,5,0.1\np2,5,0.3\n"}, "rows-ic"),                      # a missing row
    ({"train_daily_ic.csv": "id,h,v\np2,5,0.3\np1,5,0.1\np1,21,0.2\n"}, "rows-ic"),          # another order
    ({"train_daily_ic.csv": "id,h,value\np1,5,0.1\np1,21,0.2\np2,5,0.3\n"}, "headers differ"),
    ({"orientations.json": json.dumps({"candidates": [{"id": "p1", "sign": 1}, {"id": "p2", "sign": 1}]})},
     "rows-or"),
    ({"orientations.json": json.dumps({"candidates": [{"id": "p1", "sign": 1.0}, {"id": "p2", "sign": -1}]})},
     "rows-or"),                                                                             # 1 vs 1.0: bytes differ
])
def test_a_parent_row_mismatch_after_u_hard_stops_before_the_fit(tmp_path, files, needle):
    root, sp = l8_root(tmp_path)
    behave(root, ic_files={"U-1": dict(GOOD_U["U-1"], **files)})
    with pytest.raises(RC.CycleError) as e:
        run(root, sp)
    assert e.value.code == RC.EXIT_STOP and "IDENTITY MISMATCH" in str(e.value) and needle in str(e.value)
    assert calls(root)[-1] == "U-run1" and "W-run1" not in calls(root)


def test_member_keys_leave_the_combined_book_series_out(tmp_path):
    """keys = a library's candidate ids: the parent's member rows, not the runner's __combined__ series (which
    moves with every library change)."""
    res = RC.Resolver(tmp_path)
    (tmp_path / "lib.json").write_text(json.dumps({"candidates": [{"id": "p1"}, {"id": "p2"}]}))
    (tmp_path / "a.csv").write_bytes(b"id,h,v\np1,5,0.1\np2,5,0.3\n__combined__,5,0.7\n")
    (tmp_path / "b.csv").write_bytes(b"id,h,v\np1,5,0.1\np2,5,0.3\nn1,5,0.9\n__combined__,5,0.8\n")
    c = {"name": "m", "mode": "csv-rows", "a": "a.csv", "b": "b.csv"}
    with pytest.raises(RC.CycleError, match="IDENTITY MISMATCH"):
        RC.compare_files(res, c)                                     # a's keys include __combined__
    assert RC.compare_files(res, dict(c, keys="lib.json")) == (
        "2 rows of 2 keys byte for byte (a has 1 rows of other keys); b adds 2 rows of 2 other keys")
    (tmp_path / "lib.json").write_text(json.dumps({"candidates": [{"id": "p1"}, {"id": "p3"}]}))
    with pytest.raises(RC.CycleError, match="a lacks rows of 1 of the 2 keys"):
        RC.compare_files(res, dict(c, keys="lib.json"))
    (tmp_path / "a.json").write_text(json.dumps({"candidates": [{"id": "p1", "s": 1}, {"id": "x", "s": 0}]}))
    (tmp_path / "b.json").write_text(json.dumps({"candidates": [{"id": "p1", "s": 1}, {"id": "x", "s": 2}]}))
    (tmp_path / "lib.json").write_text(json.dumps({"candidates": [{"id": "p1"}]}))
    j = {"name": "j", "mode": "json-rows", "a": "a.json", "b": "b.json", "keys": "lib.json"}
    assert RC.compare_files(res, j) == "1 candidates objects identical (canonical JSON); b adds 1 objects"
    with pytest.raises(RC.CycleError, match="IDENTITY MISMATCH"):
        RC.compare_files(res, {k: v for k, v in j.items() if k != "keys"})
    (tmp_path / "lib.json").write_text(json.dumps({"candidates": []}))
    with pytest.raises(RC.CycleError, match="compare keys"):
        RC.compare_files(res, j)


def test_compare_files_modes(tmp_path):
    res = RC.Resolver(tmp_path)
    (tmp_path / "a.csv").write_bytes(b"id,x\np,1\n")
    (tmp_path / "b.csv").write_bytes(b"id,x\np,1\n")
    c = {"name": "n", "mode": "file", "a": "a.csv", "b": "b.csv"}
    assert RC.compare_files(res, c).startswith("bit for bit")
    (tmp_path / "b.csv").write_bytes(b"id,x\np,1")                   # the missing final newline is a byte
    with pytest.raises(RC.CycleError, match="sha256"):
        RC.compare_files(res, c)
    assert RC.compare_files(res, dict(c, mode="csv-rows")).startswith("1 rows of 1 keys")  # rows: same lines
    with pytest.raises(RC.CycleError, match="missing"):
        RC.compare_files(res, dict(c, b="nope.csv"))
    with pytest.raises(RC.CycleError, match="no key column"):
        RC.compare_files(res, dict(c, mode="csv-rows", key="cid"))
    (tmp_path / "a.json").write_text(json.dumps({"rows": [{"k": "p", "v": 0.0}]}))
    (tmp_path / "b.json").write_text(json.dumps({"rows": [{"v": -0.0, "k": "p"}]}))
    j = {"name": "j", "mode": "json-rows", "a": "a.json", "b": "b.json", "array": "rows", "key": "k"}
    with pytest.raises(RC.CycleError, match="first difference at k p"):
        RC.compare_files(res, j)                                     # -0.0 is not 0.0 byte for byte
    (tmp_path / "b.json").write_text(json.dumps({"rows": [{"v": 0.0, "k": "p"}, {"k": "q"}]}))
    assert RC.compare_files(res, j) == "1 rows objects identical (canonical JSON); b adds 1 objects"
    with pytest.raises(RC.CycleError, match="no candidates"):
        RC.compare_files(res, dict(j, array="candidates"))


def test_as_built_fields_are_pinned_never_rebuilt_or_suffixed(tmp_path):
    root, sp = make_root(tmp_path)
    (root / "fields-v9").mkdir()
    man = {"fields": [{"name": "fa"}, {"name": "fb"}], "files": {"fa.f64": {"sha256": "a" * 64, "bytes": 8},
                                                                  "fb.f64": {"sha256": "b" * 64, "bytes": 8}}}
    (root / "fields-v9" / "manifest.json").write_text(json.dumps(man))
    pin = RC.sha256_file(root / "fields-v9" / "manifest.json")
    fields = {"output": "fields-v9", "manifest_sha256": pin, "list": ["fa", "fb"],
              "check": {"baseline": "baseline_fields", "expect_added": ["fb"]}}
    spec = json.loads(sp.read_text())
    sp.write_text(json.dumps(dict(spec, fields=fields)), encoding="utf-8")
    log = []
    assert run(root, sp, log) == RC.EXIT_OK
    assert calls(root)[:2] == ["check", "U-run1"]                    # no builder call
    assert "fields check: pinned fields-v9/manifest.json sha256 " + pin + ": 2 rows == the spec list" in log
    u = next(s for s in cycle_of(root, sp, suffix="r2").steps() if s.phase == "u").argv
    u = u[u.index("--") + 1:]  # the IC command after the bounded runner's options
    assert u[u.index("--train-fields") + 1] == "fields-v9" and u[u.index("--output") + 1] == "out/U-r2-1"
    assert u[u.index("--train-fields-sha256") + 1] == pin
    with pytest.raises(RC.CycleError) as e:
        cycle_of(root, sp, reuse_fields="fields-base")
    assert e.value.code == RC.EXIT_USAGE
    sp.write_text(json.dumps(dict(spec, fields=dict(fields, manifest_sha256="0" * 64))), encoding="utf-8")
    with pytest.raises(RC.CycleError) as e:
        cycle_of(root, sp).steps()
    assert e.value.code == RC.EXIT_PIN and "PIN MISMATCH fields" in str(e.value)
    sp.write_text(json.dumps(dict(spec, fields=dict(fields, list=["fb", "fa"]))), encoding="utf-8")
    with pytest.raises(RC.CycleError, match="field rows differ"):
        run(root, sp)
    sp.write_text(json.dumps(dict(spec, fields=dict(fields, output="fields-v10"))), encoding="utf-8")
    with pytest.raises(RC.CycleError, match=r"HARD-STOP \[fields\]: pinned fields manifest missing"):
        run(root, sp)


@pytest.mark.parametrize("over, why", [
    (dict(compare=[{"name": "x", "after": "zz", "mode": "file", "a": "a", "b": "b"}]), "not a phase"),
    (dict(compare=[{"name": "x", "after": "ref", "mode": "file", "a": "a", "b": "b"}], ref=None), "not a phase"),
    (dict(compare=[{"name": "x", "after": "u", "mode": "diff", "a": "a", "b": "b"}]), "mode must be"),
    (dict(compare=[{"name": "x", "after": "u", "mode": "file", "a": "{input:nope}", "b": "b"}]), "names no input"),
    (dict(compare=[{"name": "x", "after": "u", "mode": "file", "a": "{out:gate}", "b": "b"}]), "names no out"),
    (dict(compare=[{"name": "x", "after": "u", "mode": "file", "a": "{out:u", "b": "b"}]), "malformed"),
    (dict(compare=[{"name": "x", "after": "u", "mode": "file", "a": "a", "b": "b"}] * 2), "duplicate"),
    (dict(compare=[]), "non-empty"),
    (dict(compare=[{"name": "x", "after": "u", "mode": "file", "a": "a", "b": "b", "keys": "{input:library}"}]),
     "row modes only"),
    (dict(compare=[{"name": "x", "after": "u", "mode": "csv-rows", "a": "a", "b": "b", "keys": "{input:nope}"}]),
     "names no input"),
    (dict(ref={"output": "out/N", "combined": "reference_combined"}), "output of its own"),
    (dict(ref={"output": "out/R", "combined": "library_x"}), "inputs.<ref.combined>"),
    (dict(ref={"output": "out/R"}), "spec ref needs"),
    (dict(fields={"output": "F"}), "builder and list"),
    (dict(fields={"output": "F", "manifest_sha256": "abc"}), "SHA-256"),
    (dict(summ={"script": "s", "dsr_n": 3, "cells_from_ledger": True}), "needs summ.ledger"),
    (dict(summ={"script": "s", "dsr_n": 3, "cells_from_ledger": True, "ledger": "L", "cells": ["a", "b"]}),
     "excludes summ.cells"),
    (dict(summ={"script": "s", "dsr_n": 3, "cells_from_ledger": 1, "ledger": "L"}), "must be true"),
])
def test_identity_spec_sections_are_validated(tmp_path, over, why):
    _, sp = l8_root(tmp_path)
    spec = json.loads(sp.read_text())
    for k, v in over.items():
        if v is None:
            spec.pop(k)
        else:
            spec[k] = v
    with pytest.raises(RC.CycleError) as e:
        RC.validate_spec(spec)
    assert e.value.code == RC.EXIT_USAGE and why in str(e.value)


def test_stop_after_a_compare_step(tmp_path):
    root, sp = l8_root(tmp_path)
    behave(root, ic_files=GOOD_U)
    log = []
    assert run(root, sp, log, stop_after="u-compare") == RC.EXIT_OK
    assert calls(root)[-1] == "U-run1" and log[-1] == "== stopped after u-compare (--stop-after)"
    assert "u-compare" in RC.STOP_PHASES and "ref" in RC.PHASES


# ---------------------------------------------------------------- platform v8 A-3: cycle plumbing
import subprocess  # noqa: E402

import research_tree  # noqa: E402

V1 = "C:/atx/atx-db/data/alpha_panel/v1"
L9_PINS = {  # the stage pins of the L9 report (manifest.json SHA-256 under V1): plain strings in hash-only mode
    "sec_identity_bridge": ("export/identity-bridge-v2-pit",
                            "09aac28f757fa959b0ed4cd9296b2267940e70af98e0d2c67cc45b1df4f7fa01"),
    "earnings_calendar": ("earnings_calendar", "9a4a976b03d0ad04672796f01abc129d0d09e3db23d62ddf57aea68ae3c7d769"),
    "insider": ("insider", "dcd3f1aa4ba6e266c03ef78568ca131c1336f51ade477f03faff2e88a62ba061"),
    "sec_filings": ("sec_filings", "5190fe99e4c2f1f13218d966a67d06995d51b7a31a73151dad2686e829aed693"),
    "thirteenf": ("thirteenf", "8974170f64b4a002cc1b131449c2abf0c7daaab23d4a256992afbdfc4105ffb0"),
    "ftd": ("ftd", "a76d69bed49829d9e14216f1abe2ef76b480c16c576fa49ae63fa2a28050f945"),
    "regsho_threshold": ("regsho_threshold", "68f431f006d9a78bb26eb7d690f4f3d2a00aaee39e41f33a6474aba9e25ad694"),
    "security_master": ("security_master", "3afe06605adac9aa85494cc5d1e7307194392954ad3b6ba8142b281fa62a414a"),
    "short_volume_ext": ("short_volume_ext", "7007a13c226a1730d0ef778d7201bf7c1d0e97ed61d4c12af32c1c4567444928"),
}
REUSE_LO3 = ("build-equity/recent-fast-train-2020-2022-v2-lo3-fields-v7",
             "2f14e20e3ff36b3a2d1fedaedc910f66465c5e308cc0138bd3d12923f1376e31")


def l9_spec(**fields_over) -> tuple[dict, dict]:
    """A fields spec with the ten L9 stage inputs (hash-only pins) and its known-pins map."""
    known = {"lib.json": "a" * 64, "role/manifest.json": "b" * 64, f"{REUSE_LO3[0]}/manifest.json": REUSE_LO3[1]}
    inputs = {"library": {"path": "lib.json", "sha256": None},
              "role": {"dir": "role", "path": "role/manifest.json", "sha256": None}}
    for key, (rel, sha) in L9_PINS.items():
        inputs[key] = {"dir": f"{V1}/{rel}", "path": f"{V1}/{rel}/manifest.json", "sha256": sha}
        known[f"{V1}/{rel}/manifest.json"] = sha
    inputs["reuse_fields"] = {"dir": REUSE_LO3[0], "path": f"{REUSE_LO3[0]}/manifest.json", "sha256": REUSE_LO3[1]}
    fields = dict({"builder": "b.py", "output": "F", "list": ["ea_days_since", "inst_best_ideas"]}, **fields_over)
    return minimal_spec(inputs=inputs, fields=fields), known


def test_stage_inputs_map_to_flags(tmp_path):
    spec, known = l9_spec(reuse_hardlink=True)
    argv = RC.Cycle(spec, RC.Resolver(tmp_path, known)).fields_step("F", "F/manifest.json", "b" * 64).argv
    pin = {k: sha for k, (_, sha) in L9_PINS.items()}
    k = argv.index("--sec-stages")
    assert argv[k:k + 12] == [
        "--sec-stages", V1, "--sec-identity-bridge", f"{V1}/export/identity-bridge-v2-pit",
        "--sec-identity-bridge-sha256", pin["sec_identity_bridge"],
        "--earnings-calendar-sha256", pin["earnings_calendar"], "--insider-sha256", pin["insider"],
        "--sec-filings-sha256", pin["sec_filings"]]
    k = argv.index("--thirteenf")
    assert argv[k:k + 20] == [x for key in RC.HOLDINGS_INPUTS for x in (
        f"--{key.replace('_', '-')}", f"{V1}/{key}", f"--{key.replace('_', '-')}-sha256", pin[key])]
    assert argv[k + 20:] == ["--max-rss-mib", "1536", "--max-seconds", "1800", "--reuse", REUSE_LO3[0],
                             "--reuse-sha256", REUSE_LO3[1], "--reuse-hardlink"]
    assert len([x for x in argv if x.endswith("-sha256")]) == 11      # role + bridge + 3 SEC + 5 holdings + reuse
    plain = RC.Cycle(dict(spec, fields=dict(spec["fields"], reuse_hardlink=False)), RC.Resolver(tmp_path, known))
    assert plain.fields_step("F", "F/manifest.json", "b" * 64).argv[-1] == REUSE_LO3[1]   # no --reuse-hardlink
    with pytest.raises(RC.CycleError) as e:                               # the spec pins the reuse dir already
        RC.Cycle(spec, RC.Resolver(tmp_path, known), reuse_fields="other")
    assert e.value.code == RC.EXIT_USAGE


@pytest.mark.parametrize("change, why", [
    (lambda s: s["inputs"].pop("insider"), "come together"),                         # three of the four SEC inputs
    (lambda s: s["inputs"].pop("sec_identity_bridge"), "come together"),
    (lambda s: s["inputs"]["insider"].update(dir=f"{V1}/form4"), "one alpha-panel root"),  # not named insider
    (lambda s: s["inputs"]["sec_filings"].update(dir="C:/elsewhere/sec_filings"), "one alpha-panel root"),
    (lambda s: s["fields"].update(manifest_sha256="0" * 64), "reuse_fields needs a built fields section"),
])
def test_stage_inputs_are_validated(change, why):
    spec, _ = l9_spec()
    change(spec)
    with pytest.raises(RC.CycleError) as e:
        RC.validate_spec(spec)
    assert e.value.code == RC.EXIT_USAGE and why in str(e.value)


def git(root: Path, *args: str) -> str:
    return subprocess.run(["git", *args], cwd=root, check=True, capture_output=True, text=True).stdout


def committed_root(tmp_path: Path, **spec_over) -> tuple[Path, Path]:
    """make_root as a git work tree: a tracked sprint-dir file and a tracked atx-impl source, all committed."""
    root, sp = make_root(tmp_path, **spec_over)
    (root / ".superpowers" / "sdd" / "x").mkdir(parents=True)
    (root / ".superpowers" / "sdd" / "x" / "progress.md").write_text("a\n")
    (root / "atx-impl").mkdir()
    (root / "atx-impl" / "code.cpp").write_text("int x;\n")
    git(root, "init", "-q")
    for key, value in (("core.autocrlf", "false"), ("user.email", "t@t"), ("user.name", "t")):
        git(root, "config", key, value)
    git(root, "add", "-A")
    git(root, "commit", "-q", "-m", "fixture")
    return root, sp


def test_clean_check_pathspec(tmp_path):
    root, sp = committed_root(tmp_path / "a")
    behave(root, touch={"U-run1": [[".superpowers/sdd/x/progress.md", "a lane report landed\n"],
                                   [".superpowers/sdd/x/task-Z-report.md", "new\n"]]})
    log = []
    assert RC.run_cycle(cycle_of(root, sp), log=log.append) == RC.EXIT_OK     # the real (scoped) git check
    assert "N-run" in calls(root) and "== cycle complete" in log
    listed = [x for x in log if x.startswith("# dirty outside the code pathspec")]
    assert any(".superpowers/sdd/x/progress.md" in x for x in listed)       # listed, never a stop
    blocking, ignored = research_tree.dirty_paths(root)
    assert blocking == [] and ".superpowers/sdd/x/progress.md" in ignored
    root2, sp2 = committed_root(tmp_path / "b")
    behave(root2, touch={"U-run1": [["atx-impl/code.cpp", "int y;\n"]]})   # a code edit mid-run
    with pytest.raises(RC.CycleError, match=r"not clean in the code pathspec: atx-impl/code.cpp") as e:
        RC.run_cycle(cycle_of(root2, sp2), log=lambda s: None)
    assert e.value.code == RC.EXIT_STOP and calls(root2)[-1] == "U-run1" and "W-run1" not in calls(root2)
    (root2 / "scripts" / "new_tool.py").write_text("")                      # an untracked file under scripts/ too
    assert research_tree.dirty_paths(root2)[0] == ["atx-impl/code.cpp", "scripts/new_tool.py"]


def test_dsr_n_from_ledger(tmp_path):
    summ = {"script": "scripts/summ.py", "dsr_n": "ledger+1", "cells_from_ledger": True, "ledger": "trials.jsonl",
            "extra": ["--psr"]}
    root, sp = make_root(tmp_path, summ=summ)
    write_ledger_at = lambda cells: (root / "trials.jsonl").write_text("".join(cell_line(c) for c in cells))
    write_ledger_at(["prior/a", "prior/b"])
    planned = next(s for s in cycle_of(root, sp).steps() if s.phase == "summ").argv
    assert planned[planned.index("--dsr-n") + 1] == "3"
    write_ledger_at(["prior/a", "prior/b", "prior/c"])                   # another cell ledgered between plan and run
    assert run(root, sp) == RC.EXIT_OK                                     # the cycle continues ...
    assert calls(root)[-1] == ("summ --weights out/W/composition_weights.json --reference ref-cell prior/a prior/b "
                               "prior/c out/N --dsr-n 4 --psr --ledger trials.jsonl --ledger-kind construction")
    root2, sp2 = make_root(tmp_path / "plain", summ=dict(summ, cells_from_ledger=None))  # N only, no grid
    spec2 = json.loads(sp2.read_text())
    spec2["summ"].pop("cells_from_ledger")
    sp2.write_text(json.dumps(spec2))
    (root2 / "trials.jsonl").write_text(json.dumps({"cell": "prior/a"}) + "\n" + json.dumps({"cell": "out/N"}) + "\n")
    s = next(x for x in cycle_of(root2, sp2).steps() if x.phase == "summ").argv
    assert s[s.index("--dsr-n") + 1] == "2" and s[-1] == "out/N"        # this cycle's own line is not a prior cell
    for bad in ({"script": "s", "dsr_n": "ledger+1"}, {"script": "s", "dsr_n": "ledger+1", "ledger": "L",
                                                       "cells": ["a"]}):
        with pytest.raises(RC.CycleError) as e:
            RC.validate_spec(dict(json.loads(sp.read_text()), summ=bad))
        assert e.value.code == RC.EXIT_USAGE and "ledger+1" in str(e.value)


def test_every_phase_has_receipt(tmp_path):
    root, sp = ops_root(tmp_path, receipts="every-phase")
    log = []
    assert run(root, sp, log) == RC.EXIT_OK
    steps = {s.phase: s for s in cycle_of(root, sp).steps()}
    cyc = "build-equity/cycle-synthetic"
    run_dirs = {"fields": "out/F-run", "check": f"{cyc}/check-run1", "u": "out/U-run1", "fit": "out/W-run1",
                "card": "out/C-run", "w": "out/WT-run1", "nav": "out/N-run", "monitor": "out/M-run",
                "summ": f"{cyc}/summ-run1"}
    for phase, rd in run_dirs.items():
        r = json.loads((root / rd / "receipt.json").read_text())
        assert (r["outcome"], r["exit_code"]) == ("completed", 0), phase
    assert {p: s.kind for p, s in steps.items() if p != "gate"} == {p: "bounded" for p in run_dirs}
    assert calls(root)[:3] == ["F-run", "fields ", "check-run1"] and "summ-run1" in calls(root)
    v = json.loads((root / cyc / "cycle_verdict.json").read_text())
    assert [p["name"] for p in v["phases"]] == list(run_dirs)
    assert all(p["seconds"] == 1.5 and p["peak_mib"] == 1 for p in v["phases"])   # read from the receipts
    assert run(root, sp) == RC.EXIT_OK                     # resume: the always-run phases take fresh receipt dirs
    assert (root / cyc / "check-run2" / "receipt.json").is_file() and (root / cyc / "summ-run2").is_dir()
    for bad in ({"receipts": "some"}, {"runner": dict(json.loads(sp.read_text())["runner"], phases={"zz": {}})}):
        with pytest.raises(RC.CycleError):
            RC.validate_spec(dict(json.loads(sp.read_text()), **bad))


def test_ref_skipped_when_fields_unchanged(tmp_path):
    root, sp = l8_root(tmp_path)
    spec = json.loads(sp.read_text())
    pin = RC.sha256_file(root / "fields-base" / "manifest.json")    # the parent's fields, as built
    spec["fields"] = {"output": "fields-base", "manifest_sha256": pin, "list": ["fa"]}
    sp.write_text(json.dumps(spec))
    behave(root, ic_files=GOOD_U)
    steps = {s.phase: s for s in cycle_of(root, sp).steps()}
    assert (steps["ref"].state, steps["ref-compare"].state) == ("skipped", "skipped")
    assert "equals the parent's" in steps["ref"].note
    assert not any(x.startswith("REF-") or "REF" in x for x in RC.plan_lines(cycle_of(root, sp), lines_only=True))
    log = []
    assert run(root, sp, log) == RC.EXIT_OK
    assert "REF-run" not in calls(root) and calls(root)[:2] == ["check", "U-run1"]
    assert any(x.startswith("== ref: skipped (fields manifest fields-base/manifest.json equals") for x in log)
    assert any(x.startswith("== ref-compare: skipped") for x in log)
    root2, sp2 = l8_root(tmp_path / "changed")                        # fields differ from the parent's: ref runs
    assert next(s for s in cycle_of(root2, sp2).steps() if s.phase == "ref").state == "pending"


def test_no_git_only_outside_repo(tmp_path):
    if research_tree.repo_root_of(tmp_path) is not None:
        pytest.skip("the temporary directory is itself inside a git repository")
    inside = tmp_path / "repo"
    (inside / ".git").mkdir(parents=True)
    root_in, sp_in = make_root(inside)
    with pytest.raises(RC.CycleError) as e:
        cycle_of(root_in, sp_in, no_git=True)
    assert e.value.code == RC.EXIT_USAGE and "only for a root outside any git repository" in str(e.value)
    root, sp = make_root(tmp_path / "plain")
    c = cycle_of(root, sp, no_git=True)
    u = next(s for s in c.steps() if s.phase == "u").argv
    assert u[:5] == [sys.executable, "scripts/runner.py", "--root", str(root), "--no-git"]   # K3 to the runner
    assert RC.run_cycle(c, log=lambda s: None, clean=lambda r: False) == RC.EXIT_OK        # no tree to check
    assert "--no-git" not in next(s for s in cycle_of(root, sp).steps() if s.phase == "u").argv
    spec = json.loads(sp.read_text())                                  # a tool absent under the root: the repo copy
    spec["runner"]["script"] = "scripts/run_bounded_research.py"
    sp.write_text(json.dumps(spec))
    u = next(s for s in cycle_of(root, sp, no_git=True).steps() if s.phase == "u").argv
    assert u[1] == (research_tree.REPO / "scripts" / "run_bounded_research.py").as_posix()
    assert next(s for s in cycle_of(root, sp).steps() if s.phase == "u").argv[1] == "scripts/run_bounded_research.py"
    runner = str(research_tree.REPO / "scripts" / "run_bounded_research.py")
    base = ["--seconds", "60", "--max-rss-mib", "512", "--min-free-mib", "64", "--", sys.executable, "-c",
            "print('bounded')"]
    done = subprocess.run([sys.executable, runner, "--root", str(root), "--no-git", "--output",
                           str(root / "out" / "probe-run"), *base], capture_output=True, text=True)
    assert done.returncode == 0, done.stderr
    r = json.loads((root / "out" / "probe-run" / "receipt.json").read_text())
    assert (r["outcome"], r["source_sha"], r["dirty_outside_pathspec"]) == ("completed", None, [])
    assert r["git"].startswith("none (--no-git")
    done = subprocess.run([sys.executable, runner, "--root", str(root_in), "--no-git", "--output",
                           str(root_in / "out" / "probe-run"), *base], capture_output=True, text=True)
    assert done.returncode == 2 and "only for a root outside any git repository" in done.stderr
    assert not (root_in / "out" / "probe-run").exists()


def test_build_key_resolves_exe_dir(tmp_path):
    root, sp = make_root(tmp_path)
    spec = json.loads(sp.read_text())
    spec.pop("exes")
    rel = dict(spec, build="equity-rel")
    c = RC.Cycle(rel, RC.Resolver(root))
    assert c.spec["exes"] == {"ic": "build-equity-rel/bin/atx-equity-strategy-ic.exe",
                              "nav": "build-equity-rel/bin/atx-equity-strategy-targets.exe"}
    assert c.spec["env_path_prepend"] == ["C:/atx-cache/vcpkg_installed/x64-windows/bin"]
    assert c.env()["PATH"].startswith(str(Path("C:/atx-cache/vcpkg_installed/x64-windows/bin")) + os.pathsep)
    steps = {s.phase: s for s in c.steps()}
    u, nav = steps["u"].argv, steps["nav"].argv
    assert u[u.index("--") + 1] == "build-equity-rel/bin/atx-equity-strategy-ic.exe"
    assert nav[nav.index("--") + 1:nav.index("--") + 3] == ["build-equity-rel/bin/atx-equity-strategy-targets.exe",
                                                             "nav"]
    dbg = RC.Cycle(dict(spec, build="equity", exes={"ic": "my-ic.exe", "nav": "x/nav.exe"}), RC.Resolver(root))
    assert dbg.spec["exes"] == {"ic": "build-equity/bin/my-ic.exe", "nav": "x/nav.exe"}   # a bare name, a path
    assert dbg.spec["env_path_prepend"][0].endswith("debug/bin")
    pinned = RC.Cycle(dict(rel, env_path_prepend=["D:/dlls"]), RC.Resolver(root))
    assert pinned.spec["env_path_prepend"] == ["D:/dlls"]                   # an explicit DLL path wins
    for bad in (dict(spec, build="release"), spec):                       # unknown build; neither build nor exes
        with pytest.raises(RC.CycleError) as e:
            RC.validate_spec(bad)
        assert e.value.code == RC.EXIT_USAGE


def test_runner_phase_caps_follow_the_od2_rule_and_the_spec(tmp_path):
    root, sp = make_root(tmp_path)
    caps = lambda c, phase: (lambda a: (a[a.index("--seconds") + 1], a[a.index("--max-rss-mib") + 1]))(
        next(s for s in c.steps() if s.phase == phase).argv)
    (root / "role" / "manifest.json").write_text(json.dumps({"universe": {"id": "u-v1"}, "dates": 1155,
                                                             "score_begin": 399, "score_end": 1155}))
    c = cycle_of(root, sp)                                              # the 3-year role: every phase 180 s / 1,536
    assert {p: caps(c, p) for p in ("u", "fit", "w", "nav")} == {p: ("180", "1536") for p in ("u", "fit", "w", "nav")}
    (root / "role" / "manifest.json").write_text(json.dumps({"universe": {"id": "u-v1"}, "dates": 1405,
                                                             "score_begin": 399, "score_end": 1405}))
    c = cycle_of(root, sp)                                              # OD-2: the IC passes on a 4-year role
    assert (caps(c, "u"), caps(c, "w"), caps(c, "fit"), caps(c, "nav")) == (
        ("300", "2560"), ("300", "2560"), ("180", "1536"), ("180", "1536"))
    spec = json.loads(sp.read_text())
    spec["runner"]["phases"] = {"fit": {"seconds": 240}, "u": {"max_rss_mib": 2048}}    # the spec's data wins
    sp.write_text(json.dumps(spec))
    c = cycle_of(root, sp)
    assert (caps(c, "fit"), caps(c, "u"), caps(c, "w")) == (("240", "1536"), ("180", "2048"), ("300", "2560"))
    c = cycle_of(root, sp, runner_overrides=RC.parse_runner_overrides(["max_rss_mib=64"]))  # the CLI wins over all
    assert (caps(c, "u"), caps(c, "fit")) == (("180", "64"), ("240", "64"))


def test_out_root_places_outputs_and_derives_shared_stores(tmp_path, monkeypatch):
    monkeypatch.setattr(RC, "window_id", lambda: "research-window-v2")
    root, sp = make_root(tmp_path)
    spec = json.loads(sp.read_text())
    spec["out_root"] = "research"
    spec["ic"].pop("cache")
    spec["fit"].pop("work_dir")
    sp.write_text(json.dumps(spec))
    role16 = RC.sha256_file(root / "role" / "manifest.json")[:16]
    for suffix in (None, "r2"):
        steps = {s.phase: s for s in cycle_of(root, sp, suffix=suffix).steps()}
        tail = f"-{suffix}" if suffix else ""
        assert steps["fields"].output == f"research/out/F{tail}" and steps["u"].output == f"research/out/U{tail}-1"
        assert steps["nav"].output == f"research/out/N{tail}"
        u, fit = steps["u"].argv, steps["fit"].argv
        assert u[-1] == f"research/candidate-cache/{role16}-research-window-v2"     # shared: never suffixed
        assert fit[fit.index("--work-dir") + 1] == f"research/fit-work/{role16}-research-window-v2"
    assert run(root, sp) == RC.EXIT_OK and (root / "research" / "out" / "N" / "summary.json").is_file()
    assert (root / "research" / "cycle-synthetic" / "cycle_verdict.json").is_file()

    def absent():
        raise LookupError("no research window")
    monkeypatch.setattr(RC, "window_id", absent)
    with pytest.raises(RC.CycleError) as e:
        cycle_of(root, sp).steps()
    assert e.value.code == RC.EXIT_USAGE and "cannot be derived" in str(e.value)
    with pytest.raises(RC.CycleError, match="root-relative"):
        RC.validate_spec(dict(spec, out_root="C:/elsewhere"))


def test_protocol_line_not_counted_in_dsr_n(tmp_path):
    import research_ledger
    summ = {"script": "scripts/summ.py", "dsr_n": "ledger+1", "cells_from_ledger": True, "ledger": "trials.jsonl"}
    root, sp = make_root(tmp_path, summ=summ)
    ledger = root / "trials.jsonl"
    ledger.write_text("".join(json.dumps({"schema": "atx.trial-ledger/v1", "kind": "construction", "cell": c,
                                          "count": 1, "trial_id": c[-1] * 16}, sort_keys=True) + "\n"
                              for c in ("prior/a", "prior/b")))
    window = root / "research_window.json"
    window.write_text('{"schema": "atx.research-window/v2"}\n')
    argv = ["ledger-protocol", "--ledger", "trials.jsonl", "--owner-ruling", "expand TRAIN to include 2023",
            "--date", "2026-09-29", "--window-id", "research-window-v2", "--research-window", "research_window.json",
            "--root", str(root)]
    assert RC.main(argv) == 0 and RC.main(argv) == 0                  # the second call appends nothing
    lines = [json.loads(x) for x in ledger.read_text().splitlines()]
    assert len(lines) == 3 and lines[:2] == [json.loads(x) for x in ledger.read_text().splitlines()[:2]]
    p = lines[2]
    assert {k: p[k] for k in ("schema", "kind", "count", "window_id", "owner_ruling", "date")} == {
        "schema": "atx.trial-ledger/v1", "kind": "protocol", "count": 0, "window_id": "research-window-v2",
        "owner_ruling": "expand TRAIN to include 2023", "date": "2026-09-29"}
    assert p["research_window_sha256"] == RC.sha256_file(window) and "cell" not in p and len(p["trial_id"]) == 16
    assert ledger.read_text().splitlines()[2] == json.dumps(p, sort_keys=True, separators=(",", ":"))
    s = next(x for x in cycle_of(root, sp).steps() if x.phase == "summ").argv
    assert s[s.index("--reference") + 2:s.index("--dsr-n") + 2] == ["prior/a", "prior/b", "out/N", "--dsr-n", "3"]
    spec = json.loads(sp.read_text())                                  # an integer N: the protocol line is no trial
    spec["summ"]["dsr_n"] = 3
    assert next(x for x in RC.Cycle(spec, RC.Resolver(root)).steps() if x.phase == "summ").argv == s
    assert research_ledger.cells(ledger) == ["prior/a", "prior/b"]
    with ledger.open("a") as f:
        f.write(json.dumps({"kind": "construction", "count": 1}) + "\n")     # a trial line without a cell
    with pytest.raises(RC.CycleError, match="line 4 has no cell"):
        cycle_of(root, sp).steps()
    for bad in (["--date", "29/09/2026"], ["--research-window", "nope.json"]):
        args = list(argv)
        args[args.index(bad[0]) + 1] = bad[1]
        assert RC.main(args) == 2


def test_ledger_copied_after_summ(tmp_path):
    summ = {"script": "scripts/summ.py", "dsr_n": "ledger+1", "ledger": "trials.jsonl",
            "ledger_copy": "sprint/trials.jsonl"}
    root, sp = make_root(tmp_path, summ=summ)
    (root / "trials.jsonl").write_text(cell_line("prior/a"))
    log = []
    assert run(root, sp, log) == RC.EXIT_OK
    assert (root / "sprint" / "trials.jsonl").read_bytes() == (root / "trials.jsonl").read_bytes()
    head = hashlib.sha256((("0" * 64) + hashlib.sha256(cell_line("prior/a").rstrip("\n").encode()).hexdigest())
                          .encode()).hexdigest()                                  # review C-6: the fold of line 1
    assert any(x.startswith("   ledger copied: ") and x.endswith(f", chain head {head})") for x in log)
    v = json.loads((root / "build-equity" / "cycle-synthetic" / "cycle_verdict.json").read_text())
    assert v["ledger"] == {"path": "trials.jsonl", "head": head, "lines": 1}     # every verdict records the head
    with pytest.raises(RC.CycleError, match="ledger_copy"):
        RC.validate_spec(dict(json.loads(sp.read_text()), summ={"script": "s", "dsr_n": 3, "ledger_copy": "x"}))


def test_a_verdict_checks_every_recorded_ledger_head(tmp_path):
    """Review F-3: verdicts recorded the ledger's {head, lines} but nothing re-checked them, so an edit of a line an
    earlier verdict read (the tail of an unchained ledger included) went unnoticed. Every verdict under the out base
    that names this ledger is now checked when a cycle writes its own: an append passes, an edit is a hard stop."""
    summ = {"script": "scripts/summ.py", "dsr_n": "ledger+1", "ledger": "trials.jsonl"}
    root, sp = make_root(tmp_path, summ=summ)
    ledger = root / "trials.jsonl"
    ledger.write_text(cell_line("prior/a"))
    assert run(root, sp) == RC.EXIT_OK                                  # verdict: {head of line 1, lines 1}
    other = root / "build-equity" / "cycle-other" / "cycle_verdict.json"   # another cycle's verdict, same ledger
    other.parent.mkdir(parents=True)
    shutil.copyfile(root / "build-equity" / "cycle-synthetic" / "cycle_verdict.json", other)
    with ledger.open("a") as f:
        f.write(cell_line("prior/b"))                                   # an append: the recorded heads still hold
    assert run(root, sp) == RC.EXIT_OK
    v = json.loads((root / "build-equity" / "cycle-synthetic" / "cycle_verdict.json").read_text())
    assert v["ledger"]["lines"] == 2
    ledger.write_text(cell_line("prior/a") + cell_line("prior/c"))     # line 2, read by the last verdict, edited
    with pytest.raises(RC.CycleError, match=r"HARD-STOP \[verdict\]: ledger trials.jsonl: its first 2 line\(s\) no "
                                            "longer fold"):
        run(root, sp)
    ledger.write_text(cell_line("prior/z") + cell_line("prior/b"))     # line 1, read by both verdicts, edited
    with pytest.raises(RC.CycleError, match="cycle-other"):           # the other cycle's verdict is checked first
        run(root, sp)


# ---------------------------------------------------------------- platform v8 A-2: run --screen, verdict, add-alpha
import re  # noqa: E402
import shutil  # noqa: E402

import research_add_alpha as RA  # noqa: E402

STRATEGIES = HERE.parents[1] / "atx-impl" / "strategies"
CAPS = frozenset({"no-composition", "marginal"})


def pool_manifest(root: Path, role: str, weights: str) -> str:
    """A parent's combined-signal manifest as the marginal verb binds it: its role and composition weights by SHA-256."""
    return json.dumps({"schema": "atx.dsl-combined-signal/v1", "role_manifest_sha256": RC.sha256_file(root / role),
                       "composition_weights_sha256": RC.sha256_file(root / weights)})


def screen_root(tmp_path: Path, **over) -> tuple[Path, Path]:
    """ops_root plus a marginal section on the parent's combined signal (themes: the parent's weights, the file the
    pool names), --save-combined and --min-names in the IC flags."""
    root, sp = ops_root(tmp_path, marginal={"output": "out/MIC", "pool": "reference_combined",
                                            "themes": "reference_weights"}, **over)
    (root / "ref-fit").mkdir()
    (root / "ref-fit" / "composition_weights.json").write_text('{"weights": {"old": 1.0}}')
    (root / "ref-w" / "train_combined.json").write_text(pool_manifest(root, "role/manifest.json",
                                                                      "ref-fit/composition_weights.json"))
    spec = json.loads(sp.read_text())
    spec["inputs"]["reference_combined"] = {"path": "ref-w/train_combined.json", "sha256": None}
    spec["inputs"]["reference_weights"] = {"path": "ref-fit/composition_weights.json", "sha256": None}
    spec["ic"]["flags"] = spec["ic"]["flags"] + ["--min-names", "1000", "--save-combined"]
    sp.write_text(json.dumps(spec))
    return root, sp


def parse_marginal(argv: list[str]) -> dict:
    """dispatch_marginal_ic's parse, mirrored: the verb, then option/value pairs of VERB_OPTIONS (each once here), and
    run_marginal_ic's refusal without VERB_REQUIRED."""
    assert argv[0] == "marginal" and len(argv) % 2 == 1, argv
    opts = {}
    for key, value in zip(argv[1::2], argv[2::2]):
        assert key in VERB_OPTIONS and key not in opts and not value.startswith("--"), (key, value)
        opts[key] = value
    assert set(VERB_REQUIRED) <= set(opts), sorted(set(VERB_REQUIRED) - set(opts))
    return opts


def verb_cli_from_cpp() -> tuple[set, set]:
    """(options, required options) of the marginal verb, read from its C++: the `key == "--x") cfg.member` lines of
    dispatch_marginal_ic, and the members run_marginal_ic refuses empty (its "bounded config" check)."""
    text = (HERE.parents[1] / "atx-impl" / "src" / "strategy_marginal_ic.cpp").read_text(encoding="utf-8")
    parser = text[text.index("int dispatch_marginal_ic("):]
    member = dict(re.findall(r'key == "(--[a-z0-9-]+)"\) cfg\.(\w+)', parser))
    member.update({k: "max_working_bytes" for k in re.findall(r'key == "(--max-memory-mib)"\) \{', parser)})
    check = text[text.index("co::Status run_marginal_ic("):text.index("bounded config (needs")]
    empty = set(re.findall(r"(?<!!)cfg\.(\w+)\.empty\(\)", check))
    return set(member), {k for k, v in member.items() if v in empty}


def test_marginal_argv_is_the_verbs_full_cli(tmp_path):
    """The planned marginal argv is what the verb's parser takes: every required option, each option once with a value,
    every pin the spec has (library, pool), the role, the parent's weights as themes, this cycle's fields and cache."""
    assert verb_cli_from_cpp() == (VERB_OPTIONS, set(VERB_REQUIRED))           # the C++ CLI, pinned
    assert set(RC.MARGINAL_REQUIRED) == set(VERB_REQUIRED)
    assert set(RC.MARGINAL_BUILT) | set(RC.MARGINAL_SPEC_FLAGS) == VERB_OPTIONS  # nothing unbuildable, nothing unknown
    root, sp = screen_root(tmp_path)
    c = cycle_of(root, sp, screen=True, capabilities=CAPS)
    st = next(s for s in c.steps() if s.phase == "marginal")
    k = st.argv.index("--")
    assert st.argv[k + 1] == "bin/ic.exe" and st.state == "pending"
    assert parse_marginal(st.argv[k + 2:]) == {
        "--candidate-cache": "out/CC", "--library": "lib/lib.json", "--library-sha256": c.pin("library"),
        "--pool": "ref-w/train_combined.json", "--pool-sha256": c.pin("reference_combined"),
        "--role": "role/manifest.json", "--themes": "ref-fit/composition_weights.json", "--fields": "out/F",
        "--min-names": "1000", "--output": "out/MIC"}                        # --min-names: the u pass's
    runner = st.argv[:k]
    assert [runner[j + 1] for j, x in enumerate(runner) if x == "--bind"] == [
        "bin/ic.exe", "lib/lib.json", "ref-w/train_combined.json", "role/manifest.json",
        "ref-fit/composition_weights.json", "out/F/manifest.json"]
    spec = json.loads(sp.read_text())
    spec["marginal"] = {"output": "out/MIC", "flags": ["--max-memory-mib", "800", "--min-names", "200"]}   # no themes
    sp.write_text(json.dumps(spec))
    st = next(s for s in cycle_of(root, sp, capabilities=CAPS).steps() if s.phase == "marginal")
    opts = parse_marginal(st.argv[st.argv.index("--") + 2:])
    assert "--themes" not in opts and (opts["--max-memory-mib"], opts["--min-names"]) == ("800", "200")


def test_marginal_spec_refused_before_any_run(tmp_path):
    root, sp = screen_root(tmp_path)
    spec = json.loads(sp.read_text())
    for change, needle in ((dict(flags=["--themes"]), "option/value pairs"),        # A-2's bare flag
                           (dict(themes="reference_nope"), "marginal.themes"),
                           (dict(themes=None, flags=["--themes", "ref-fit/composition_weights.json"]), "built by the step"),
                           (dict(flags=["--role", "role/manifest.json"]), "built by the step"),
                           (dict(flags=["--min-names", "2"]), "--min-names '2'"),
                           (dict(flags=["--max-memory-mib", "99999"]), "--max-memory-mib '99999'"),
                           (dict(flags=["--workers", "4"]), "--workers")):
        with pytest.raises(RC.CycleError, match=re.escape(needle)) as e:
            RC.validate_spec(dict(spec, marginal={k: v for k, v in dict(spec["marginal"], **change).items()
                                                  if v is not None}))
        assert e.value.code == RC.EXIT_USAGE
    with pytest.raises(RC.CycleError, match="not a count"):
        RC.validate_spec(dict(spec, ic=dict(spec["ic"], flags=["--min-names", "{n}"])))
    # the verb's bindings, checked when the step is planned: nothing runs
    (root / "ref-fit" / "composition_weights.json").write_text('{"weights": {"old": 0.5}}')   # not the pool's weights
    assert RC.main(["plan", str(sp), "--root", str(root)]) == RC.EXIT_PIN
    with pytest.raises(RC.CycleError, match="names composition weights") as e:
        run(root, sp, screen=True, capabilities=CAPS)
    assert e.value.code == RC.EXIT_PIN and calls(root) == []
    (root / "ref-w" / "train_combined.json").write_text(pool_manifest(root, "lib/base.json",
                                                                      "ref-fit/composition_weights.json"))
    with pytest.raises(RC.CycleError, match="blended on role"):
        run(root, sp, screen=True, capabilities=CAPS)
    assert calls(root) == []


def test_screen_stops_before_w(tmp_path):
    root, sp = screen_root(tmp_path)
    log = []
    c = cycle_of(root, sp, screen=True, capabilities=CAPS)
    assert RC.run_cycle(c, log=log.append, clean=lambda r: True) == RC.EXIT_OK
    assert calls(root) == ["fields ", "check", "U-run1", "W-run1", "C-run", "MIC-run"]   # u -> fit -> card -> marginal
    steps = {s.phase: s for s in cycle_of(root, sp, screen=True, capabilities=CAPS).steps()}
    u = steps["u"].argv
    assert "--no-composition" in u and "--save-combined" not in u                     # B-1, when the exe offers it
    m = steps["marginal"].argv
    pin = RC.sha256_file
    assert m[m.index("--") + 1:] == ["bin/ic.exe", "marginal", "--candidate-cache", "out/CC", "--library",
                                     "lib/lib.json", "--library-sha256", pin(root / "lib/lib.json"), "--pool",
                                     "ref-w/train_combined.json", "--pool-sha256", pin(root / "ref-w/train_combined.json"),
                                     "--role", "role/manifest.json", "--themes", "ref-fit/composition_weights.json",
                                     "--fields", "out/F", "--min-names", "1000", "--output", "out/MIC"]
    assert steps["marginal"].state == "done" and (root / "out" / "MIC" / "marginal_ic.json").is_file()  # the verb ran
    assert {p: steps[p].state for p in ("w", "nav", "monitor", "summ")} == dict.fromkeys(("w", "nav", "monitor",
                                                                                          "summ"), "skipped")
    assert log.index("== marginal") < log.index("gate p1 PASS") < log.index("== w: skipped (screen: runs with the "
                                                                              "full `run`)")
    assert any(x.startswith("gate p1 new_alpha: status admitted") for x in log)            # the admission rows
    assert any(x.startswith("marginal new_alpha: ic21 0.012 (HAC t 2.1); marginal ic21 0.008") for x in log)
    assert log[-1] == "== screen complete: w, nav, monitor and summ run with the full `run`"
    v = json.loads((root / "build-equity" / "cycle-synthetic" / "cycle_verdict.json").read_text())
    assert v["mode"] == "screen" and [r["id"] for r in v["admission"]] == ["new_alpha"]
    assert v["marginal"][0]["max_rho_member"] == "old" and not {"paired", "dsr", "pbo"} & set(v)
    assert [p["name"] for p in v["phases"]] == ["fields", "check", "u", "fit", "card", "marginal"]
    assert RC.plan_lines(cycle_of(root, sp, screen=True, capabilities=CAPS), lines_only=True)[-1] == RC.fmt_argv(m)
    n = len(calls(root))                                                  # the full run continues from the screen
    assert run(root, sp, capabilities=CAPS) == RC.EXIT_OK
    assert calls(root)[n:n + 3] == ["check", "WT-run1", "N-run"] and calls(root)[-1].startswith("summ ")
    root2, sp2 = screen_root(tmp_path / "old-exe")                       # an exe without B-1 / F-2: skipped, no stop
    log2 = []
    assert RC.run_cycle(cycle_of(root2, sp2, screen=True, capabilities=frozenset()), log=log2.append,
                        clean=lambda r: True) == RC.EXIT_OK
    assert "MIC-run" not in calls(root2) and "WT-run1" not in calls(root2)
    u2 = next(s for s in cycle_of(root2, sp2, screen=True, capabilities=frozenset()).steps() if s.phase == "u").argv
    assert "--no-composition" not in u2 and "--save-combined" in u2
    assert "== marginal: skipped (the IC exe offers no marginal verb (contract K6, lane F): skipped)" in log2
    v2 = json.loads((root2 / "build-equity" / "cycle-synthetic" / "cycle_verdict.json").read_text())
    assert v2["marginal"] == [] and "no marginal verb" in v2["marginal_note"]


FAKE_SUMM_JSON = r'''
import json, sys
from pathlib import Path
a = sys.argv[1:]
with open("calls.log", "a") as f:
    f.write("summ " + " ".join(a) + "\n")
if "--json" in a:
    Path(a[a.index("--json") + 1]).write_text(json.dumps([
        {"dir": "prior\\a", "paired": {"dsr": -1.0}},
        {"dir": "out\\N", "paired": {"dsr": 0.05, "memmel_se": 0.1, "cbb_ci95": [-0.1, 0.2], "lw": {"p_value": 0.4}},
         "deflated": {"n": 2, "dsr": 0.7}, "deflated_effective_n": {"dsr": 0.8},
         "deflated_ledger": {"n": 2, "dsr": 0.93, "variance_sr": 1e-4, "cells": 2, "window_id": "research-window-v2",
                             "legacy_dsr": 0.6, "legacy_cells": 3}}]))
if "--pbo-json" in a:
    Path(a[a.index("--pbo-json") + 1]).write_text(json.dumps({"pbo": 0.25}))
'''


def test_verdict_schema(tmp_path):
    summ = {"script": "scripts/summ.py", "dsr_n": "ledger+1", "cells_from_ledger": True, "ledger": "trials.jsonl",
            "extra": ["--psr", "--pbo"], "origin": "prior"}
    root, sp = screen_root(tmp_path, summ=summ, verdict=True)
    (root / "scripts" / "summ.py").write_text(FAKE_SUMM_JSON)
    (root / "trials.jsonl").write_text(cell_line("prior/a"))
    assert run(root, sp, capabilities=CAPS) == RC.EXIT_OK
    cyc = "build-equity/cycle-synthetic"
    assert calls(root)[-1].endswith(f"--dsr-n 2 --psr --pbo --protocol v8 --origin prior --json {cyc}/summ.json "
                                    f"--pbo-json {cyc}/pbo.json --ledger trials.jsonl --ledger-kind construction "
                                    "--dsr-ledger trials.jsonl")                  # review C-1 / C-2
    v = json.loads((root / cyc / "cycle_verdict.json").read_text())
    assert set(v) == {"schema", "cycle", "mode", "spec_sha256", "admission", "marginal", "phases", "paired", "dsr",
                      "pbo", "ledger"}
    assert (v["schema"], v["cycle"], v["mode"], v["spec_sha256"]) == ("atx.cycle-verdict/v1", "synthetic", "run",
                                                                      RC.sha256_file(sp))
    assert v["paired"] == {"dsr": 0.05, "se": 0.1, "cbb_ci": [-0.1, 0.2], "lw_p": 0.4}   # this cycle's NAV dir row
    assert v["dsr"] == {"n": 2, "cell_count": 0.93, "effective_n": 0.8, "variance_sr": 1e-4, "variance_cells": 2,
                        "window_id": "research-window-v2", "source": "nav_summ --dsr-ledger (deflated_ledger)",
                        "legacy": {"dsr": 0.6, "cells": 3, "note": "legacy variance (ledger lines without a "
                                                                    "window_id): reported, gates nothing"}}
    assert v["pbo"] == 0.25                                        # review C-1: never the cell-count row's 0.7
    assert [r["id"] for r in v["admission"]] == ["new_alpha"] and v["marginal"][0]["marginal_hac_t"] == 1.6
    assert [p["name"] for p in v["phases"]] == ["fields", "check", "u", "fit", "card", "marginal", "w", "nav",
                                                "monitor", "summ"]
    assert all(set(p) == {"name", "seconds", "peak_mib"} and p["seconds"] > 0 for p in v["phases"])
    assert {p["name"]: p["peak_mib"] for p in v["phases"]}["u"] == 1                        # from the receipt
    assert {p["name"]: p["peak_mib"] for p in v["phases"]}["summ"] is None                  # a direct phase
    for bad in ({"verdict": "yes"}, {"marginal": {"output": "M", "pool": "nope"}}):
        with pytest.raises(RC.CycleError):
            RC.validate_spec(dict(json.loads(sp.read_text()), **bad))


# ------------------------------------------------------------------ add-alpha
V70_OUT = {"fields": FIELDS_V7, "u": "build-equity/mega-v70-train-u-1", "w": "build-equity/mega-v70w-train-ew-1",
           "fit": "build-equity/mega-weights-v70-ew", "nav": V70_CELL}
FAKE_IC_PLAN = r'''
import hashlib, json, os, re, sys
from pathlib import Path
a = sys.argv[1:]
if "--help" in a:
    print("equity-strategy-ic --library JSON ... [--no-composition]\n  equity-strategy-ic marginal --candidate-cache DIR")
    sys.exit(0)
if os.environ.get("FAKE_IC_ARGV"):                     # opt-in: the last plan argv, for the tests that read it
    Path(os.environ["FAKE_IC_ARGV"]).write_text(json.dumps(a))
lib = json.loads(Path(a[a.index("--library") + 1]).read_text())
declared = {f["name"] for f in lib["fields"]} - {"close", "raw_close", "volume"}
rows = [{"id": c["id"], "dsl_sha256": hashlib.sha256(c["dsl"].encode()).hexdigest(),
         "num_slots": 9 if c["id"].startswith("wide_") else 3, "required_lookback": 20,
         "extra_fields": sorted(set(re.findall(r"[A-Za-z_][A-Za-z0-9_]*", c["dsl"])) & declared), "node_count": 5}
        for c in lib["candidates"]]
print(json.dumps({"mode": "metadata-only-no-payload", "candidates": rows,
                  "library_sha256": a[a.index("--library-sha256") + 1]}))
'''


def v71_entry(cid: str) -> dict:
    lib = json.loads((STRATEGIES / "fund_industry_ic_v71.json").read_text(encoding="utf-8"))
    return next(c for c in lib["candidates"] if c["id"] == cid)


def entry_text(blob: bytes, cid: str) -> bytes:
    """The encoded candidate object of `cid` (indent 4) in a library file."""
    m = re.search(rb'\n    \{\n      "id": "' + cid.encode() + rb'",\n.*?\n    \}', blob, re.S)
    assert m is not None, cid
    return m.group(0)


def add_alpha_root(tmp_path: Path) -> Path:
    """A root with the registry minus the four wave-2 alphas, the legacy parent v7.0 (library, recipe), the v70 spec
    and the parent cycle's outputs the child pins (files with made-up content)."""
    root = tmp_path / "root"
    s = root / "atx-impl" / "strategies"
    (s / "alphas").mkdir(parents=True)
    (s / "libraries").mkdir()
    reg = json.loads((STRATEGIES / "alphas" / "registry.json").read_text(encoding="utf-8"))
    reg["alphas"] = [a for a in reg["alphas"] if a["id"] not in WAVE2]
    (s / "alphas" / "registry.json").write_bytes(RA.G.encode_data(reg))
    for name in ("fund_industry_ic_v70.json", "fund_industry_ic_v70.recipe.json"):
        shutil.copyfile(STRATEGIES / name, s / name)
    (root / "scripts" / "specs").mkdir(parents=True)
    shutil.copyfile(V70, root / "scripts" / "specs" / "v70.json")
    files = {"build-equity/recent-fast-train-2020-2022-v2-lo1/manifest.json":
             json.dumps({"universe": {"id": "linked-operating-v1"}, "dates": 1155, "score_begin": 399}),
             f"{FIELDS_V7}/manifest.json": json.dumps({"fields": [{"name": "be"}]}),
             f"{V70_OUT['u']}/summary.json": json.dumps({"status": "complete"}),
             f"{V70_OUT['u']}/orientations.json": "{}", f"{V70_OUT['u']}/train_daily_ic.csv": "id,h,v\n",
             f"{V70_OUT['w']}/summary.json": json.dumps({"status": "complete"}),
             f"{V70_OUT['fit']}/admission.json": "{}", f"{V70_OUT['fit']}/composition_weights.json": "{}",
             f"{V70_CELL}/summary.json": json.dumps({"primary_scenario": "modeled-1bn-stale5-v1+swap-fin-v1"}),
             f"{V70_CELL}/{S2_CSV}": "net_return\n"}
    for rel, text in files.items():
        (root / rel).parent.mkdir(parents=True, exist_ok=True)
        (root / rel).write_text(text)
    (root / V70_OUT["w"] / "train_combined.json").write_text(pool_manifest(
        root, "build-equity/recent-fast-train-2020-2022-v2-lo1/manifest.json",
        f"{V70_OUT['fit']}/composition_weights.json"))              # the v7.0 pool: its role and weights by SHA-256
    return root


def add_argv(root: Path, cid: str, name: str = "v71a", **over) -> list[str]:
    c = dict(v71_entry(cid), **over)
    return ["add-alpha", "--id", c["id"], "--dsl", c["dsl"], "--theme", c["theme"], "--tier", c["tier"],
            "--prior-sign", str(c["prior_sign"]), "--citation", c["citation"], "--origin", "prior", "--parent", "v70",
            "--name", name, "--root", str(root)]


def k1_plan(tmp_path: Path, ids: list[str]) -> Path:
    """The recorded K1 rows (atx-impl/strategies/alphas/fixtures) of the given members, as a plan of another library."""
    fixture = json.loads((STRATEGIES / "alphas" / "fixtures" / "v71_plan_k1.json").read_text(encoding="utf-8"))
    rows = {r["id"]: r for r in fixture["candidates"]}
    path = tmp_path / f"plan-{len(ids)}.json"
    path.write_text(json.dumps({"mode": "metadata-only-no-payload", "candidates": [rows[i] for i in ids]}))
    return path


def test_add_alpha_entry_byte_identical_to_committed(tmp_path):
    root = add_alpha_root(tmp_path)
    s = root / "atx-impl" / "strategies"
    v70_ids = [c["id"] for c in json.loads((s / "fund_industry_ic_v70.json").read_text())["candidates"]]
    plan = k1_plan(tmp_path, v70_ids + ["ftd_fail"])
    assert RC.main(add_argv(root, "ftd_fail") + ["--plan-json", str(plan)]) == RC.EXIT_OK
    blob = (s / "fund_industry_ic_v71a.json").read_bytes()
    committed = (STRATEGIES / "fund_industry_ic_v71.json").read_bytes()
    assert entry_text(blob, "ftd_fail") == entry_text(committed, "ftd_fail")      # the new entry, byte for byte
    parent = (s / "fund_industry_ic_v70.json").read_bytes()
    region = lambda b: b.split(b'"candidates": [\n', 1)[1]                        # noqa: E731
    assert region(blob).startswith(region(parent)[:-len(b"\n  ]\n}\n")] + b",\n")  # the 44 parent entries unchanged
    reg = json.loads((s / "alphas" / "registry.json").read_text(encoding="utf-8"))
    a = next(x for x in reg["alphas"] if x["id"] == "ftd_fail")
    assert (a["origin"], a["added_in"], a["prior_sign_source"], a["notes"]["formula"]) == ("prior", "v71a",
                                                                                          a["citation"], None)
    lib = json.loads((s / "libraries" / "v71a.json").read_text(encoding="utf-8"))
    assert lib["id"] == "fund_industry_ic_v71a" and lib["parent"] == "v70" and lib["members"] == v70_ids + ["ftd_fail"]
    assert [(e["id"], set(e) - {"id", "basis"}) for e in lib["budget_exceptions"]] == [
        ("q5_eg", {"max_extra_fields"}), ("qmj_safety", {"max_slots"})]       # inherited from the legacy v7.0 recipe
    rec = json.loads((s / "fund_industry_ic_v71a.recipe.v2.json").read_text(encoding="utf-8"))
    assert rec["generation"]["new_members"] == ["ftd_fail"] and rec["parent"]["sha256"] == RC.sha256_file(
        STRATEGIES / "fund_industry_ic_v70.json")
    assert "`ftd_fail`" in (s / "libraries" / "v71a.prereg.md").read_text(encoding="utf-8")
    sp = root / "scripts" / "specs" / "v8" / "lib-v71a.json"
    spec = RC.load_spec(sp)
    assert all(item["sha256"] for item in spec["inputs"].values())              # locked
    assert spec["inputs"]["library"]["path"] == "atx-impl/strategies/fund_industry_ic_v71a.json"
    assert spec["inputs"]["baseline_library"]["path"] == "atx-impl/strategies/fund_industry_ic_v70.json"
    assert spec["inputs"]["reference_combined"]["path"] == f"{V70_OUT['w']}/train_combined.json"
    assert spec["inputs"]["reference_daily"]["path"] == f"{V70_CELL}/{S2_CSV}"
    assert spec["inputs"]["baseline_fields"]["path"] == f"{FIELDS_V7}/manifest.json"
    assert spec["fields"] == {"output": FIELDS_V7, "manifest_sha256": RC.sha256_file(root / FIELDS_V7 / "manifest.json"),
                              "list": RC.load_spec(V70)["fields"]["list"]}
    assert (spec["ic"]["u_output"], spec["ic"]["w_output"], spec["nav"]["output"], spec["fit"]["output"]) == (
        "build-equity/mega-v71a-train-u", "build-equity/mega-v71aw-train-ew", V70_CELL.replace("v70u", "v71au"),
        "build-equity/mega-weights-v71a-ew")
    assert "cache" not in spec["ic"] and "work_dir" not in spec["fit"] and "static_check" not in spec
    assert spec["gate"] == {"name": "p1-v71a", "admitted": ["ftd_fail"], "require": "any", "sign_agrees": True,
                            "report": []}
    assert (spec["summ"]["dsr_n"], spec["summ"]["cells_from_ledger"], "cells" in spec["summ"]) == ("ledger+1", True,
                                                                                                   False)
    assert (spec["receipts"], spec["verdict"]) == ("every-phase", True)
    assert spec["marginal"] == {"output": "build-equity/mega-v71a-train-u-marginal", "pool": "reference_combined",
                                "themes": "reference_weights"}                  # A2: the parent's weights, by input
    assert spec["inputs"]["reference_weights"]["path"] == f"{V70_OUT['fit']}/composition_weights.json"
    c = RC.Cycle(spec, RC.Resolver(root), capabilities=CAPS)                       # the step accepts the derived spec
    st = c.marginal_step(c.ipath("library"), FIELDS_V7)
    opts = parse_marginal(st.argv[st.argv.index("--") + 2:])
    assert (opts["--themes"], opts["--role"], opts["--pool-sha256"], opts["--min-names"]) == (
        f"{V70_OUT['fit']}/composition_weights.json", "build-equity/recent-fast-train-2020-2022-v2-lo1/manifest.json",
        spec["inputs"]["reference_combined"]["sha256"], "1000")
    assert [c["name"] for c in spec["compare"]] == ["ref-s2-daily", "parent-orientations", "parent-train-daily-ic"]
    assert spec["runner"]["phases"] == {"marginal": RA.MARGINAL_CAPS}           # a 3-year role: no OD-2 caps (5f)
    reg_bytes = (s / "alphas" / "registry.json").read_bytes()
    assert RC.main(add_argv(root, "ftd_fail") + ["--plan-json", str(plan)]) == RC.EXIT_OK   # identical: reused
    assert (s / "alphas" / "registry.json").read_bytes() == reg_bytes
    assert json.loads((s / "libraries" / "v71a.json").read_text())["members"].count("ftd_fail") == 1


def test_add_alpha_refusals_write_nothing(tmp_path):
    root = add_alpha_root(tmp_path)
    s = root / "atx-impl" / "strategies"
    v70_ids = [c["id"] for c in json.loads((s / "fund_industry_ic_v70.json").read_text())["candidates"]]
    plan = k1_plan(tmp_path, v70_ids + ["ftd_fail"])
    before = sorted(p.relative_to(root).as_posix() for p in root.rglob("*") if p.is_file())
    for argv in (add_argv(root, "ftd_fail", theme="event_driven"),                  # unknown theme
                 add_argv(root, "ftd_fail", tier="Z"),
                 [x if x != "prior" else "guess" for x in add_argv(root, "ftd_fail")],   # K5 origin
                 add_argv(root, "ftd_fail", dsl=v71_entry("sue")["dsl"]),          # the DSL of a registered alpha
                 add_argv(root, "sue", name="v71b")):                               # already a member of v7.0
        assert RC.main(argv + ["--plan-json", str(plan)]) == RC.EXIT_USAGE, argv
    short = k1_plan(tmp_path, v70_ids)                                               # no row for the new member
    assert RC.main(add_argv(root, "ftd_fail") + ["--plan-json", str(short)]) == RC.EXIT_USAGE
    assert sorted(p.relative_to(root).as_posix() for p in root.rglob("*") if p.is_file()) == before
    assert RC.main(add_argv(root, "ftd_fail") + ["--plan-json", str(plan)]) == RC.EXIT_OK
    changed = add_argv(root, "ftd_fail", citation="another paper")                   # an id is never redefined
    assert RC.main(changed + ["--plan-json", str(plan)]) == RC.EXIT_USAGE
    spec = RC.load_spec(root / "scripts" / "specs" / "v8" / "lib-v71a.json")
    (root / f"{spec['ic']['u_output']}-1").mkdir(parents=True)                       # the cycle has started
    plan2 = k1_plan(tmp_path, v70_ids + ["ftd_fail", "ea_overdue"])
    assert RC.main(add_argv(root, "ea_overdue") + ["--plan-json", str(plan2)]) == RC.EXIT_USAGE
    assert RA.next_name("v71") == "v72" and RA.next_name("v7-lo3") == "v7-lo4"


def test_add_alpha_validates_through_the_exe_plan(tmp_path, monkeypatch):
    root = add_alpha_root(tmp_path)
    monkeypatch.setenv("FAKE_IC_ARGV", str(tmp_path / "fake_ic_argv.json"))
    (root / "bin").mkdir()
    (root / "bin" / "fake_ic.py").write_text(FAKE_IC_PLAN)
    (root / "bin" / "ic.cmd").write_text(f'@"{sys.executable}" "%~dp0fake_ic.py" %*\n')
    assert RC.exe_capabilities("bin/ic.cmd", root) == CAPS and RC.exe_capabilities("bin/none.exe", root) == frozenset()
    spec = json.loads((root / "scripts" / "specs" / "v70.json").read_text())
    spec["exes"]["ic"] = "bin/ic.cmd"
    (root / "scripts" / "specs" / "v70.json").write_text(json.dumps(spec))
    (root / "build-equity" / "recent-fast-train-2020-2022-v2-lo1" / "manifest.json").write_text(json.dumps(
        {"universe": {"id": "linked-operating-v1"}, "dates": 1405, "score_begin": 399}))   # the 4-year role
    assert RC.main(add_argv(root, "ins_opp")) == RC.EXIT_OK                          # the exe's --plan-only rows
    plan_argv = json.loads((tmp_path / "fake_ic_argv.json").read_text())
    assert "--plan-only" in plan_argv                                                 # PM6-9: the spec's IC cap
    assert plan_argv[plan_argv.index("--max-memory-mib") + 1] == RC.option_value(spec["ic"]["flags"],
                                                                                   "--max-memory-mib") == "1536"
    child = RC.load_spec(root / "scripts" / "specs" / "v8" / "lib-v71a.json")
    assert child["runner"]["phases"] == {"u": {"seconds": 300, "max_rss_mib": 2560},
                                         "w": {"seconds": 300, "max_rss_mib": 2560},   # OD-2, written as spec data
                                         "marginal": {"seconds": 360}}                  # integration 8 item 5f
    assert RC.Cycle(child, RC.Resolver(root), verify=False).phase_caps("marginal") == {
        "seconds": 360, "max_rss_mib": child["runner"]["max_rss_mib"], "min_free_mib": child["runner"]["min_free_mib"]}
    lib = json.loads((root / "atx-impl" / "strategies" / "fund_industry_ic_v71a.json").read_text())
    assert lib["families"][-1]["id"] == "ownership_flow"                             # a new theme enters with ins_opp
    wide = add_argv(root, "ea_overdue", name="v71c", id="wide_overdue", dsl="rank((-1 * ea_days_to_expected))")
    assert RC.main(wide) == RC.EXIT_USAGE                                             # 9 slots > 7: refused
    assert not (root / "atx-impl" / "strategies" / "libraries" / "v71c.json").exists()


# ---------------------------------------------------------------- PM addition: cache gc
def test_cache_gc_keeps_referenced_stores_and_never_touches_state(tmp_path, monkeypatch):
    import research_gc
    monkeypatch.setattr(RC, "window_id", lambda: "research-window-v2")
    root, sp = make_root(tmp_path)
    spec = json.loads(sp.read_text())
    spec["ic"]["cache"], spec["fit"]["work_dir"] = "build-equity/mega-candidate-cache-v2", "build-equity/mega-fit-work-v2"
    sp.write_text(json.dumps(spec))
    derived = dict(spec, name="derived", ic={k: v for k, v in spec["ic"].items() if k != "cache"},
                   fit={k: v for k, v in spec["fit"].items() if k != "work_dir"})
    sp2 = tmp_path / "derived.json"
    sp2.write_text(json.dumps(derived))
    role16 = RC.sha256_file(root / "role" / "manifest.json")[:16]
    kept = ["mega-candidate-cache-v2", "mega-fit-work-v2", f"candidate-cache/{role16}-research-window-v2",
            f"fit-work/{role16}-research-window-v2"]
    unreferenced = ["mega-candidate-cache-v1", "mega-fit-work-v1-r7", "candidate-cache/0123456789abcdef-research-window-v1"]
    for rel in kept + unreferenced:
        (root / "build-equity" / rel / ("a" * 64)).mkdir(parents=True)
        (root / "build-equity" / rel / ("a" * 64) / "x.f64").write_bytes(b"\0" * 2048)
    state = {"trials.jsonl": "{}\n", "mega-nav-x/summary.json": "{}", "recent-role/manifest.json": "{}",
             "mega-candidate-cache-odd/receipt.json": "{}"}           # the last looks like a store but holds a receipt
    for rel, text in state.items():
        (root / "build-equity" / rel).parent.mkdir(parents=True, exist_ok=True)
        (root / "build-equity" / rel).write_text(text)
    log = []
    rep = research_gc.gc([sp, sp2], root, ["build-equity"], apply=False, log=log.append)
    assert sorted(rep["keep"]) == sorted(f"build-equity/{r}" for r in kept)
    assert sorted(rep["gc"]) == sorted(f"build-equity/{r}" for r in unreferenced)
    assert rep["skip"] == ["build-equity/mega-candidate-cache-odd"] and rep["deleted"] == []
    assert any(x.startswith("keep  build-equity/mega-candidate-cache-v2") and "(referenced by synthetic)" in x
               for x in log)
    assert log[-1].startswith("== 3 unreferenced store dirs, 0.0 MiB (dry run")
    assert all((root / "build-equity" / r).is_dir() for r in kept + unreferenced)   # a dry run deletes nothing
    assert RC.main(["cache", "gc", "--keep-referenced-by", str(sp), str(sp2), "--root", str(root), "--apply"]) == 0
    assert all((root / "build-equity" / r).is_dir() for r in kept) and not any(
        (root / "build-equity" / r).exists() for r in unreferenced)
    assert all((root / "build-equity" / r).is_file() for r in state)                 # ledger, NAV, role, receipt kept
    with pytest.raises(SystemExit) as e:                                             # a keep list is required
        RC.main(["cache", "gc", "--root", str(root)])
    assert e.value.code == RC.EXIT_USAGE


def test_cache_gc_keeps_a_referenced_store_however_the_paths_are_spelled(tmp_path, monkeypatch):
    """Review C-12: --under ./build-equity, build-equity\\, an absolute path, a '..' detour or (Windows) another case
    used to name no referenced store, so --apply deleted them all; a spec's store spelled with './', a trailing slash or
    another case is the same store too."""
    import research_gc
    monkeypatch.setattr(RC, "window_id", lambda: "research-window-v2")
    root, sp = make_root(tmp_path)
    spec = json.loads(sp.read_text())
    cache = "./BUILD-EQUITY/mega-candidate-cache-v2/" if os.name == "nt" else "./build-equity//mega-candidate-cache-v2/"
    spec["ic"]["cache"], spec["fit"]["work_dir"] = cache, "build-equity/sub/../mega-fit-work-v2"
    sp.write_text(json.dumps(spec))
    kept, unreferenced = ["mega-candidate-cache-v2", "mega-fit-work-v2"], ["mega-candidate-cache-v1"]
    for rel in kept + unreferenced:
        (root / "build-equity" / rel / "s").mkdir(parents=True)
        (root / "build-equity" / rel / "s" / "x.f64").write_bytes(b"\0" * 64)
    (root / "sub").mkdir()
    spellings = ["build-equity", "./build-equity", "build-equity/", str(root / "build-equity"),
                 "sub/../build-equity"] + (["build-equity\\", "Build-Equity"] if os.name == "nt" else [])
    for under in spellings:                                    # listed as spelled; Windows compares case-folded
        rep = research_gc.gc([sp], root, [under], apply=False, log=lambda s: None)
        assert [[os.path.normcase(x) for x in rep[k]] for k in ("keep", "gc")] == \
            [[os.path.normcase(f"build-equity/{r}") for r in rels] for rels in (kept, unreferenced)], under
    rep = research_gc.gc([sp], root, spellings, apply=True, log=lambda s: None)       # one listing, however spelled
    assert rep["deleted"] == ["build-equity/mega-candidate-cache-v1"] and len(rep["keep"]) == 2
    assert all((root / "build-equity" / r / "s" / "x.f64").is_file() for r in kept)
