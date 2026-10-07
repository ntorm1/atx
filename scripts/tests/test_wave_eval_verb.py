"""Rulings B2-10 (c) and B2-11: a P9 cell (a v2 wave manifest, atx.research-wave/v2) names and pins the engine verb of
its statistics, and the judge publishes no Statistics of record from another verb.

- verify (B2-11, re-review 1 R1): the cell's locked spec must name exes.eval and carry its pin exes_sha256.eval
  (`lock --exes`); refused otherwise, naming the missing key(s). Nothing the summ writes is read at verify: the summ
  and the bundle run later, in the judge.
- judge (B2-11, re-review 1 R2): the bundle runs nav_summ on the pinned verb (--engine-stats exes.eval) and is
  refused, fail closed (exit 3), when the verb on disk is not the pin or bundle.json (fresh or reused) names another
  verb; the cell's summ.json rows and pbo.json must name the pinned verb too.
- a v1 manifest keeps its plan bytes and behaviour (ruling B2-10 (a)).

Each test runs a real v2 manifest (E2's kinds fixture new-nav-rule.json, re-rooted into the fake world by
test_wave_kinds' helpers) through `research_wave run` in the real stage order: preflight .. spec (lock --exes), run,
match, verify, judge, record. Synthetic files only (wave_fixture.py); no exe runs.

Run: "C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider scripts/tests/test_wave_eval_verb.py
"""
from __future__ import annotations

import hashlib
import json
from pathlib import Path
import re
import sys

import pytest

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent))
import research_cycle as RC  # noqa: E402
import research_wave  # noqa: E402
import test_wave_kinds as K  # noqa: E402  (E2's v2 helpers: load_fixture, world, wave, KindsFake)
import wave_fixture as F  # noqa: E402
import wave_stage_record as WR  # noqa: E402
import wave_steps as WS  # noqa: E402
from cycle.lock import lock_exes  # noqa: E402
from wave_context import Wave  # noqa: E402

FIXTURE = "new-nav-rule.json"
VERB = F.EVAL_VERB                                     # the parent's exes.eval (ruling B2-10 (b): root names it there)
EXES = {"ic": "bin/ic.exe", "nav": "bin/nav.exe"}
ADMIT_ALL = {"alpha_a": ("admitted", 1), "alpha_b": ("admitted", 1), "alpha_c": ("admitted", 1)}


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


DEFAULT_VERB = sha(b"nav_summ's default verb")       # what nav_summ resolves without --engine-stats (fake)
OTHER = sha(b"another eval verb")


class VerbFake(K.KindsFake):
    """KindsFake plus the engine verb. `lock --exes` pins the exes with research_cycle's own rule (cycle.lock.lock_exes,
    over the files in the fake root). The cell's summ (research_cycle run, summ_step) writes summ.json rows naming the
    verb its spec names (--engine-stats exes.eval) and the bundle (nav_summ --bundle) a bundle.json naming the verb of
    its --engine-stats, else nav_summ's default (DEFAULT_VERB): nav_summ_run.engine_exe_sha256 = the SHA-256 of that
    file. ``summ_verb`` / ``bundle_verb`` force another hash; ``pbo_verb`` adds a pbo.json naming that hash."""

    def __init__(self, root: Path, *, summ_verb: str | None = None, bundle_verb: str | None = None,
                 pbo_verb: str | None = None, **kw):
        super().__init__(root, **kw)
        self.summ_verb, self.bundle_verb, self.pbo_verb = summ_verb, bundle_verb, pbo_verb

    def verb_sha(self, verb: str | None) -> str:
        return sha((self.root / verb).read_bytes()) if verb else DEFAULT_VERB

    def cycle(self, args: list[str]):
        if args[0] == "lock" and "--exes" in args:
            doc = json.loads((self.root / args[1]).read_text(encoding="utf-8"))
            lock_exes(doc, self.root / args[1], RC.Resolver(self.root), [])
            F.write_json(self.root, args[1], doc)
        done = super().cycle(args)
        if args[0] == "run" and "--screen" not in args and "--stop-after" not in args:     # the cell's summ
            spec, c = self.spec_outputs(args[1])
            verb = (spec.get("exes") or {}).get("eval")
            run = {"engine_exe": verb, "engine_exe_sha256": self.summ_verb or self.verb_sha(verb)}
            rows = json.loads((self.root / c.cycle_dir() / "summ.json").read_text(encoding="utf-8"))
            F.write_json(self.root, f"{c.cycle_dir()}/summ.json", [dict(r, nav_summ_run=run) for r in rows])
            if self.pbo_verb:
                F.write_json(self.root, f"{c.cycle_dir()}/pbo.json",
                             {"pbo": 0.2, "nav_summ_run": {"engine_exe_sha256": self.pbo_verb}})
        return done

    def bounded(self, argv: list[str]):
        done = super().bounded(argv)
        cmd = argv[argv.index("--") + 1:]
        if "--bundle" in cmd:
            verb = cmd[cmd.index("--engine-stats") + 1] if "--engine-stats" in cmd else None
            out = cmd[cmd.index("--bundle-json") + 1]
            doc = json.loads((self.root / out).read_text(encoding="utf-8"))
            doc["nav_summ_run"] = {"engine_exe": verb or "build-equity/bin/atx-research-eval.exe",
                                   "engine_exe_sha256": self.bundle_verb or self.verb_sha(verb)}
            F.write_json(self.root, out, doc)
        return done


def verb_world(tmp_path: Path, *, name_eval: bool = True, lock: bool = True) -> tuple[Path, dict]:
    """E2's v2 fixture in the fake world (test_wave_kinds.world: the parent names and pins exes.eval over a synthetic
    verb file, wave_fixture.pin_eval_verb). Here the parent's pin is taken off, so the cell's pin can only come from
    the wave's own `lock --exes` (``lock``: driver.lock_exes, which hashes every exe the cell runs: ic and nav get
    synthetic files too); ``name_eval`` False drops exes.eval from the parent."""
    m = K.load_fixture(FIXTURE)
    if lock:
        m["driver"] = {"lock_exes": True}
    root = K.world(tmp_path, m)
    parent = json.loads((root / F.PARENT).read_text(encoding="utf-8"))
    assert parent["exes"] == dict(EXES, eval=VERB) and set(parent[RC.EXES_PIN]) == {"eval"}
    parent["exes"] = dict(EXES, **({"eval": VERB} if name_eval else {}))
    parent.pop(RC.EXES_PIN)
    F.write_json(root, F.PARENT, parent)
    for rel in EXES.values():
        F.write(root, rel, f"synthetic exe {rel}\n")
    F.git(root, "add", "-A")
    F.git(root, "commit", "-q", "-m", "the parent unpinned")
    return root, m


def receipt(root: Path, m: dict, name: str) -> dict:
    return json.loads((root / "out/waves" / m["wave"] / "receipts" / name).read_text(encoding="utf-8"))


def bundle_cmds(fake) -> list[list[str]]:
    """The nav_summ --bundle commands the fake ran (the argv after the bounded runner's --)."""
    cmds = [c[c.index("--") + 1:] for c in fake.calls if "--" in c and Path(c[1]).name == "run_bounded_research.py"]
    return [c for c in cmds if "--bundle" in c]


def test_a_named_and_pinned_v2_cell_passes_verify_and_the_judge_runs_the_pinned_verb(tmp_path):
    """Re-review 1 R1: in the real stage order a correctly named and pinned v2 cell passes verify, before any judge
    output exists; R2: the judge's bundle runs on the pinned verb, and the wave is judged and recorded."""
    root, m = verb_world(tmp_path)
    fake = VerbFake(root)
    code, log = K.wave(root, fake, "plan")
    plan = [line for line in log if "--bundle" in line]
    assert code == 0 and len(plan) == 1 and "--engine-stats \"<the cell spec's exes.eval>\" --protocol v8" in plan[0]
    code, log = K.wave(root, fake, "run", "--until", "verify")
    assert code == 0, log
    cell = receipt(root, m, "06-match.json")["outputs"]["cell_spec"]
    w = Wave(F.MANIFEST, root, executor=fake, log=lambda s: None)
    cyc_dir, pin = w.outputs(cell)["cycle_dir"], sha((root / VERB).read_bytes())
    assert not (root / cyc_dir / "summ.json").exists() and not (root / w.wave_path("bundle.json")).exists()
    vf = receipt(root, m, "07-verify.json")["outputs"]
    assert vf["exes_sha256"]["cell"]["eval"] == pin and WR.eval_pin(w, cell) == (VERB, pin)
    code, log = K.wave(root, fake, "plan")      # the plan after match names the cell spec's verb
    assert code == 0 and [f"--engine-stats {VERB} --protocol v8" in line for line in log if "--bundle" in line] == [True]
    code, log = K.wave(root, fake, "run")
    assert code == 0, log
    (cmd,) = bundle_cmds(fake)
    assert Path(cmd[1]).name == "nav_summ.py" and cmd[2:6] == ["--engine-stats", VERB, "--protocol", "v8"]
    bundle = json.loads((root / w.wave_path("bundle.json")).read_text(encoding="utf-8"))
    assert bundle["nav_summ_run"]["engine_exe_sha256"] == pin
    jd = receipt(root, m, "08-judge.json")["outputs"]
    assert jd["bundle"]["path"] == w.wave_path("bundle.json") and (root / "out/waves/k-navrule/wave-result.json").exists()


def test_verify_refuses_a_v2_cell_without_exes_eval_or_its_pin(tmp_path):
    """Ruling B2-11: verify refuses a v2 cell whose locked spec does not name exes.eval, or names it without the
    exes_sha256.eval pin (no driver.lock_exes and an unpinned parent), fail closed, before the judge runs."""
    for label, name_eval, lock, needle in (
            ("unnamed", False, True, "lacks exes.eval and exes_sha256.eval: every P9 cell names the engine verb"),
            ("unpinned", True, False, "lacks exes_sha256.eval: every P9 cell names the engine verb")):
        root, m = verb_world(tmp_path / label, name_eval=name_eval, lock=lock)
        fake = VerbFake(root)
        code, log = K.wave(root, fake, "run")
        assert code == 4, (label, log)
        cell = receipt(root, m, "06-match.json")["outputs"]["cell_spec"]
        err = receipt(root, m, "07-verify.failed-1.json")["error"]
        assert f"verify: the v2 cell spec {cell} {needle}" in err and "rulings B2-10 (c), B2-11" in err, (label, err)
        assert not (root / "out/waves" / m["wave"] / "receipts" / "08-judge.json").exists() and not bundle_cmds(fake)
        w = Wave(F.MANIFEST, root, executor=fake, log=lambda s: None)
        with pytest.raises(WR.StageError, match="ruling B2-11") as exc:   # the judge's own guard (a spec edited later)
            WR.judge_verb(w, cell)
        assert exc.value.code == WR.EXIT_PIN


@pytest.mark.parametrize("budget", [None, 4096])
def test_the_judge_refuses_a_bundle_of_another_verb_fresh_and_reused(tmp_path, budget):
    """Re-review 1 R2: a bundle.json naming another verb than the pin is refused when the bundle has just run and
    again when a later judge would reuse it; no wave result is written. Both judge branches: serial, and side by side
    under driver.host_budget_mib (P9 OR section 5)."""
    root, m = verb_world(tmp_path)
    if budget:
        m = dict(m, driver=dict(m["driver"], host_budget_mib=budget, admission_wait_seconds=60))
        F.write_json(root, F.MANIFEST, K.rerooted(m, root))
        F.git(root, "commit", "-q", "-am", "side by side")
    code, log = K.wave(root, VerbFake(root), "run", "--until", "verify")
    assert code == 0, log
    pin, out = sha((root / VERB).read_bytes()), f"out/waves/{m['wave']}/bundle.json"
    for k, fake in ((1, VerbFake(root, bundle_verb=OTHER)), (2, VerbFake(root))):
        code, log = K.wave(root, fake, "run")
        assert code == 3, (k, log)
        err = receipt(root, m, f"08-judge.failed-{k}.json")["error"]
        assert re.search(rf"judge: {re.escape(out)}: its statistics come from the engine verb {OTHER} "
                         rf"\(nav_summ_run\.engine_exe_sha256\), not the cell spec's exes_sha256 pin of exes\.eval "
                         rf"{re.escape(VERB)} {pin}: .*ruling B2-11", err), (k, err)
        assert len(bundle_cmds(fake)) == (1 if k == 1 else 0)      # fresh, then reused (never run again)
        assert bool(budget) == any("side by side under the host memory budget" in line for line in log)
        assert not (root / "out/waves" / m["wave"] / "wave-result.json").exists()
    (root / out).unlink()                       # the stale bundle removed: the next judge runs the pinned verb
    fake = VerbFake(root)
    code, log = K.wave(root, fake, "run")
    assert code == 0 and len(bundle_cmds(fake)) == 1, log


def test_the_judge_refuses_a_verb_whose_bytes_moved(tmp_path):
    """Ruling B2-11: the verb on disk must hash to the cell spec's pin before the judge runs anything (fail closed,
    exit 3): neither the cell's summ nor the bundle runs."""
    root, m = verb_world(tmp_path)
    fake = VerbFake(root)
    code, log = K.wave(root, fake, "run", "--until", "verify")
    assert code == 0, log
    pin = sha((root / VERB).read_bytes())
    F.write(root, VERB, "a rebuilt eval verb\n")
    F.git(root, "commit", "-q", "-am", "the verb rebuilt after lock")
    calls = len(fake.calls)
    code, log = K.wave(root, fake, "run")
    assert code == 3, log
    err = receipt(root, m, "08-judge.failed-1.json")["error"]
    assert (f"judge: the engine verb exes.eval {VERB} on disk is {sha((root / VERB).read_bytes())}, not the cell "
            f"spec's exes_sha256 pin {pin}") in err and "nothing ran (ruling B2-11)" in err, err
    assert not [c for c in fake.calls[calls:] if c[0] != "git"], fake.calls[calls:]     # no summ, bundle or reader
    assert not (root / f"out/waves/{m['wave']}/bundle.json").exists()


def test_the_judge_refuses_a_summ_or_pbo_of_another_verb(tmp_path):
    """Ruling B2-11 (re-review 1 R1 fix, its optional part): after the cell's summ, the judge checks every summ.json
    row and the pbo.json against the pin (cycle_verdict.check_summ_verb, the verdict's own rule); a refusal stops
    before the bundle runs. Then a summ on the pinned verb is judged and recorded."""
    root, m = verb_world(tmp_path)
    code, log = K.wave(root, VerbFake(root), "run", "--until", "verify")
    assert code == 0, log
    cell = receipt(root, m, "06-match.json")["outputs"]["cell_spec"]
    cyc_dir = Wave(F.MANIFEST, root, executor=VerbFake(root), log=lambda s: None).outputs(cell)["cycle_dir"]
    for k, fake, needle in ((1, VerbFake(root, summ_verb=OTHER), f"{cyc_dir}/summ.json row "),
                            (2, VerbFake(root, pbo_verb=OTHER), f"{cyc_dir}/pbo.json: ")):
        code, log = K.wave(root, fake, "run")
        assert code == 3, (k, log)
        err = receipt(root, m, f"08-judge.failed-{k}.json")["error"]
        assert f"judge: {needle}" in err and f"engine verb {OTHER}" in err and "ruling B2-11" in err, (k, err)
        assert not bundle_cmds(fake)
    (root / cyc_dir / "pbo.json").unlink()
    code, log = K.wave(root, VerbFake(root), "run")
    assert code == 0, log


def test_a_v1_manifest_keeps_its_plan_bytes_and_behaviour(tmp_path):
    """Ruling B2-10 (a): a v1 manifest asks for no exes.eval, its bundle argv carries no --engine-stats (the argv of
    before, byte for byte), its verify plan line is unchanged, and a summ or bundle of any verb is recorded, not
    refused."""
    argv = WS.bundle_argv("py", "out/a", "out/b", "o/bundle.json", "o/bundle-run1", files=["out/a/daily_s2.csv"])
    assert argv[argv.index("--") + 1:] == ["py", WS.NAV_SUMM, "--protocol", "v8", "--bundle", "out/a", "out/b",
                                           "--bundle-json", "o/bundle.json"]
    v2 = WS.bundle_argv("py", "out/a", "out/b", "o/bundle.json", "o/bundle-run1", files=["out/a/daily_s2.csv"],
                        verb=VERB)
    assert v2[v2.index("--") + 1:] == ["py", WS.NAV_SUMM, "--engine-stats", VERB] + argv[argv.index("--") + 3:]
    assert v2[:v2.index("--")] == argv[:argv.index("--")]                     # the runner's binds are the same
    root = F.build(tmp_path)
    fake = VerbFake(root, summ_verb=OTHER, bundle_verb=OTHER)
    fake.admission = ADMIT_ALL
    plan: list[str] = []
    assert research_wave.main(["plan", F.MANIFEST, "--root", str(root)], executor=fake, log=plan.append) == 0
    assert [line for line in plan if line.startswith("#   mechanics rule v8-mech")] == [K.V1_VERIFY_LINE]
    bundle_plan = [line for line in plan if "--bundle" in line]
    assert len(bundle_plan) == 1 and "--engine-stats" not in bundle_plan[0], bundle_plan
    assert research_wave.main(["run", F.MANIFEST, "--root", str(root)], executor=fake, log=lambda s: None) == 0
    (cmd,) = bundle_cmds(fake)
    assert cmd[2:4] == ["--protocol", "v8"] and "--engine-stats" not in cmd
    w = Wave(F.MANIFEST, root, executor=fake, log=lambda s: None)
    done = {"match": {"cell_spec": "scripts/specs/v8/lib-w1.json"}}
    assert w.manifest["schema"] == "atx.research-wave/v1" and WR.eval_problem(w, done) is None
    assert WR.judge_verb(w, done["match"]["cell_spec"]) is None
    res = json.loads((root / "out/waves/w1/wave-result.json").read_text(encoding="utf-8"))
    assert res["verdict"]["rule"] == "pm7-34"                                  # judged and recorded
    assert RC.EXES_PIN not in json.loads((root / done["match"]["cell_spec"]).read_text(encoding="utf-8"))
