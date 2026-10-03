"""Lane E1 (P9): the wave driver's hardening. Task 0 = P0-FIX, the Phase-0 blockers and the byte-neutral fixes root
merges before the registered Y-S wave (plan 2026-10-03-p9-sprint-plan.md, section 1.2 "before" rows):

  test_runner_max_refuses_720        OR-1 / DEC-1: one RUNNER_MAX_SECONDS (research_tree) refused above at manifest
                                     load, spec load, the wave step and the runner itself
  test_budget_prefix_list_counts_v8ys  OR section 4 / DEC-2: admission_cycle_prefixes counts every listed prefix
  test_capacity_missing_is_not_done  NV-4: a NAV with --capacity-curve is done only with its capacity files; the book
                                     reader refuses a missing curve the summary declares
  test_exe_sha_in_phase_rows         OR-2: executable_sha256 in phase rows and wave-result.json; verify's parent vs
                                     cell record; research_cycle runs the ref when the NAV exes differ
  test_ledger_append_lock_excl       OR-5: the ledger's verify-and-append under an O_EXCL lock, bytes unchanged
  test_two_seed_suite_command        F-7: the documented suite command (scripts/tests/run_two_seeds.py)
(test_spec_kind_null_pins, F-9, is in test_research_spec.py, beside the fixtures it reads.)

Every file is synthetic (fake tools, sessions 2020-2021); no data of the repository is read.

Run: "C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider scripts/tests/test_wave_hardening.py
"""
from __future__ import annotations

import hashlib
import json
from pathlib import Path
import subprocess
import sys
import threading

import pytest

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent))
import wave_fixture as F  # noqa: E402
import research_cycle as RC  # noqa: E402
import research_tree as RT  # noqa: E402
import research_wave  # noqa: E402
import run_two_seeds as RTS  # noqa: E402
import test_research_cycle as T  # noqa: E402  (the fake cycle tools)
import wave_manifest as WM  # noqa: E402
import wave_readers  # noqa: E402
import wave_result  # noqa: E402
import wave_stage_preflight as WP  # noqa: E402
import wave_stage_record as WR  # noqa: E402
import wave_stage_util as WU  # noqa: E402
import wave_steps as WS  # noqa: E402
from wave_context import Wave  # noqa: E402

BI = F.BI
RULED = {"ruling": "PM8-15", "pool_only": True}
NAV_CAP = {"output": "out/N", "rule": "aim-partial-v5", "leverage": "1.247",
           "flags": ["--aim-leverage", "{leverage}", "--cadence", "1", "--capacity-curve"]}


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def manifest(**over) -> dict:
    """The fixture's wave manifest with its fields pin filled (F.build fills it from the file)."""
    m = F.manifest(**over)
    return dict(m, fields=dict(m["fields"], manifest_sha256="0" * 64))


# ------------------------------------------------------------------ (a) OR-1: the runner's time cap
def test_runner_max_refuses_720(tmp_path):
    """The bounded runner's maximum is one constant, 600 s; y-s's 720 s marginal cap is refused where it is read (the
    wave manifest at load, a cycle spec at load, the wave step that writes the cap into a spec) with the key named,
    and the runner itself refuses more than the constant."""
    assert RT.RUNNER_MAX_SECONDS == 600
    bad = WM.validate(manifest(marginal=dict(RULED, seconds=720)))                       # the manifest
    assert bad and "marginal.seconds 720 is above the bounded runner's maximum 600 s" in bad[0]
    assert WM.validate(manifest(marginal=dict(RULED, seconds=600))) == []                 # the maximum itself
    assert WM.validate(manifest(marginal=RULED)) == []                                    # no cap: the spec's
    root = F.build(tmp_path / "w", marginal=dict(RULED, seconds=600))
    doc = json.loads((root / F.MANIFEST).read_text())
    (root / "y.json").write_text(json.dumps(dict(doc, marginal=dict(RULED, seconds=720))))
    with pytest.raises(WM.WaveError, match="marginal.seconds 720"):                         # load (wave plan / run)
        WM.load(root / "y.json")
    for runner, key in (({"phases": {"marginal": {"seconds": 720}}}, "spec runner.phases.marginal.seconds 720"),
                        ({"phases": {"u": {"seconds": 900, "max_rss_mib": 2560}}}, "spec runner.phases.u.seconds 900"),
                        ({"seconds": 601}, "spec runner.seconds 601")):                   # a spec, at load
        spec = T.minimal_spec()
        spec["runner"] = dict(spec["runner"], **runner)
        with pytest.raises(RC.CycleError) as e:
            RC.validate_spec(spec)
        assert key in str(e.value) and e.value.code == RC.EXIT_USAGE, (runner, e.value)
        path = tmp_path / "spec.json"
        path.write_text(json.dumps(spec))
        with pytest.raises(RC.CycleError, match="above the bounded runner's maximum"):
            RC.load_spec(path)
    spec = T.minimal_spec()
    spec["runner"] = dict(spec["runner"], seconds=600, phases={"marginal": {"seconds": 600}})
    RC.validate_spec(spec)                                                                  # 600 is accepted
    doc = {"marginal": {"output": "out/m"}, "runner": {"seconds": 60}}
    with pytest.raises(ValueError, match="marginal.seconds 720 is above"):                 # the wave step
        WS.marginal_ruled(doc, dict(RULED, seconds=720))
    assert WS.marginal_ruled(doc, dict(RULED, seconds=600))["runner"]["phases"] == {"marginal": {"seconds": 600}}
    probe = tmp_path / "probe"                                                              # the runner itself
    probe.mkdir()
    done = subprocess.run([sys.executable, str(RT.REPO / "scripts" / "run_bounded_research.py"), "--seconds", "601",
                           "--root", str(probe), "--no-git", "--output", str(probe / "o"), "--", sys.executable, "-V"],
                          capture_output=True, text=True, timeout=60)
    assert done.returncode == 2 and "<=600 seconds" in done.stderr and not (probe / "o").exists()


# ------------------------------------------------------------------ (b) DEC-2: the budget's prefix list
def admission_records(root: Path, cycles: dict) -> list[dict]:
    """Admission lines ({cycle: [candidate, ...]}) appended to the fixture's ledger; every line, re-read."""
    role = F.role_sha(root)
    lines = [F.admission_line(cycle, cid, role_sha=role) for cycle, ids in cycles.items() for cid in ids]
    BI.ledger_append(root / F.LEDGER, lines, chain=True)
    return BI.ledger_read(root / F.LEDGER)


def test_budget_prefix_list_counts_v8ys(tmp_path):
    """Y-S's budget counts the X hand-written lines (cycles v8x*) and its own library's (v8ys*): a prefix list
    (admission_cycle_prefixes) counts every line of any listed prefix; the one-string form still counts its own
    prefix only, as before (so a v8ys line was not counted under "v8x")."""
    listed = {"id": "y", "admission_cap": 40, "admission_cycle_prefixes": ["v8x", "v8ys"], "construction_cap": 62}
    root = F.build(tmp_path / "r", budget=listed)
    records = admission_records(root, {"v8x3": ["a1", "a2"], "v8x7b": ["a3"], "v8ys": ["y1", "y2"], "v81": ["o1"],
                                       "v8yz": ["o2"]})
    w = Wave(F.MANIFEST, root, executor=F.FakeCycle(root, {}), log=lambda s: None)
    assert WP.admission_used(w, records, listed) == 5                                    # v8x3 2 + v8x7b 1 + v8ys 2
    old = {"id": "y", "admission_cap": 40, "admission_cycle_prefix": "v8x", "construction_cap": 62}
    assert WP.admission_used(w, records, old) == 3                                       # the old form: v8x* only
    out, problems = WP.budget_check(w, records, 2)
    assert problems == [] and out["admission_used"] == 5 and out["admission_cycle_prefixes"] == ["v8x", "v8ys"]
    assert "admission_cycle_prefix" not in out
    line = wave_result.budget_line(out, {"n_before": 2, "n_after": 3})
    assert "(cycles v8x*, v8ys*;" in line
    assert "(cycles v8x*;" in wave_result.budget_line(dict(old, admission_used=3), {"n_before": 2, "n_after": 3})
    tight = dict(listed, admission_cap=7)                                                # 5 used + 3 new > 7
    assert "5 admission trials used + 3 new > cap 7" in WP.budget_check(
        Wave(F.MANIFEST, F.build(tmp_path / "t", budget=tight), executor=F.FakeCycle(root, {}), log=lambda s: None),
        records, 2)[1][0]
    for b in ({"id": "y", "admission_cap": 4, "admission_cycle_prefix": "v8x", "admission_cycle_prefixes": ["v8ys"]},
              {"id": "y", "admission_cap": 4, "admission_cycle_prefixes": []},
              {"id": "y", "admission_cap": 4, "admission_cycle_prefixes": ["v8x", "v8x"]},
              {"id": "y", "admission_cap": 4, "admission_cycle_prefixes": "v8x"},
              {"id": "y", "admission_cap": 4, "admission_cycle_prefixes": ["v8x", ""]},
              {"id": "y", "admission_cycle_prefixes": ["v8x"]},
              {"id": "y", "admission_cap": 4}):
        assert any(p.startswith("budget must be") for p in WM.validate(manifest(budget=b))), b
    assert WM.validate(manifest(budget=old)) == [] and WM.validate(manifest(budget=listed)) == []
    assert WM.budget_prefixes(listed) == ["v8x", "v8ys"] and WM.budget_prefixes(old) == ["v8x"]
    assert WM.budget_prefixes({"id": "y"}) == []


# ------------------------------------------------------------------ (c) NV-4: capacity completeness
def nav_step(root: Path, sp: Path):
    return next(s for s in T.cycle_of(root, sp).steps() if s.phase == "nav")


def test_capacity_missing_is_not_done(tmp_path):
    """The NAV verb writes summary.json before its capacity pass and v7_extras.json: a NAV whose argv carries
    --capacity-curve is done only with capacity_curve.csv and v7_extras.json beside its summary (a crashed capacity
    pass is a failed attempt, never done); without the flag the summary alone is done, as before. The book reader
    refuses a missing curve (or 4x row) that the summary declares, and reads null without the declaration."""
    root, sp = T.make_root(tmp_path / "ok", nav=NAV_CAP)
    assert T.run(root, sp) == RC.EXIT_OK                                  # the fake writes both files: done
    assert nav_step(root, sp).done
    root, sp = T.make_root(tmp_path / "crash", nav=NAV_CAP)
    T.behave(root, **{"N-run": "capacity-crash"})                         # exit 0, summary.json only
    with pytest.raises(RC.CycleError, match=r"HARD-STOP \[nav\]: exit 0 but its output is incomplete"):
        T.run(root, sp)
    st = nav_step(root, sp)
    assert st.state == "failed" and "capacity_curve.csv, v7_extras.json of --capacity-curve do not" in st.note
    with pytest.raises(RC.CycleError, match="capacity_curve.csv, v7_extras.json"):          # resume: never done
        T.run(root, sp)
    (root / "out/N/capacity_curve.csv").write_text("multiple,book,net_sharpe\n4,s2,1.0\n")
    st = nav_step(root, sp)
    assert st.state == "failed" and " v7_extras.json of --capacity-curve" in st.note and "capacity_curve.csv," \
        not in st.note
    (root / "out/N/v7_extras.json").write_text("{}")
    assert nav_step(root, sp).done
    root, sp = T.make_root(tmp_path / "plain")                            # no flag: the summary alone, unchanged
    T.behave(root, **{"N-run": "capacity-crash"})
    assert T.run(root, sp) == RC.EXIT_OK and nav_step(root, sp).done
    assert not (root / "out/N/capacity_curve.csv").exists()
    # the book reader (wave_readers.py book), on a synthetic NAV dir
    w = tmp_path / "w"
    F.write_nav(w, "nav", 1.0)
    d = w / "nav"
    assert wave_readers.book(d)["x4_net_sharpe"] == 1.1                   # no declaration, the curve read
    (d / "capacity_curve.csv").unlink()
    assert wave_readers.book(d)["x4_net_sharpe"] is None                  # no declaration: null, as before
    summary = json.loads((d / "summary.json").read_text())
    summary["v7"] = {"declarations": {"capacity_curve": True}}
    (d / "summary.json").write_text(json.dumps(summary))
    assert wave_readers.capacity_expected(summary)
    with pytest.raises(SystemExit, match="capacity_curve.csv is missing but"):
        wave_readers.main(["book", "--nav", f"cell={d}", "--output", str(w / "book.json")])
    assert not (w / "book.json").exists()
    (d / "capacity_curve.csv").write_text("multiple,book,net_sharpe\n1,s2,1.0\n2,s2,0.9\n")    # no 4x row
    with pytest.raises(SystemExit, match="no single finite 4x row"):
        wave_readers.book(d)
    (d / "capacity_curve.csv").write_text("multiple,book,net_sharpe\n4,s2,0.7\n")
    assert wave_readers.book(d)["x4_net_sharpe"] == 0.7
    assert not wave_readers.capacity_expected({"v7": {"declarations": {"capacity_curve": False}}})


# ------------------------------------------------------------------ (d) OR-2: exe SHA-256s
def receipt(root: Path, run_dir: str, exe: str | None, outcome: str = "completed") -> None:
    doc = {"outcome": outcome, "exit_code": 0 if outcome == "completed" else 1, "wall_seconds": 2.0,
           "sampled_peak_tree_rss_bytes": 3 << 20}
    if exe is not None:
        doc["executable_sha256"] = exe
    F.write_json(root, f"{run_dir}/receipt.json", doc)


def test_exe_sha_in_phase_rows(tmp_path):
    """executable_sha256 is kept in every phase row (and so in wave-result.json's timings); verify records the parent
    NAV's and the cell NAV's exe SHA-256 and whether the cell's reference construction ran on the cell's exe; with
    fields equal to the parent's, research_cycle skips the ref only while the NAV exe is not known to differ from the
    one the parent NAV's receipt records."""
    a, b = "a" * 64, "b" * 64
    root = F.build(tmp_path / "r")
    cell = "scripts/specs/v8/lib-c1.json"
    spec = F.cell_spec("c1", "out/nav-c1", reference_nav=F.PARENT_NAV)
    spec["inputs"]["reference_combined"] = {"path": "out/w-p0-1/train_combined.json", "sha256": None}
    spec["ref"] = {"output": "out/ref-c1", "combined": "reference_combined"}
    F.write_json(root, cell, spec)
    w = Wave(F.MANIFEST, root, executor=F.FakeCycle(root, {}), log=lambda s: None)
    receipt(root, f"{F.PARENT_NAV}-run", a)
    receipt(root, "out/nav-c1-run", b, outcome="system-memory-limit")    # a killed attempt, then the retry
    receipt(root, "out/nav-c1-run2", b)
    rows = WU.phase_rows(w, cell)
    navs = [r for r in rows if r["phase"] == "nav"]
    assert [(r["run_dir"], r["executable_sha256"]) for r in navs] == [("out/nav-c1-run", b), ("out/nav-c1-run2", b)]
    assert WU.completed_exe(navs) == b and WU.completed_exe(navs[:1]) is None
    done = {"preflight": {"parent": {"nav": F.PARENT_NAV}}, "match": {"cell_spec": cell}}
    assert WR.nav_exe(w, done, rows) == {"parent": a, "cell": b, "equal": False, "ref": "missing"}
    receipt(root, "out/ref-c1-run", a)                                    # the ref ran on the parent's exe: missing
    assert WR.nav_exe(w, done, WU.phase_rows(w, cell))["ref"] == "missing"
    receipt(root, "out/ref-c1-run2", b)                                   # on the cell's: ran
    assert WR.nav_exe(w, done, WU.phase_rows(w, cell)) == {"parent": a, "cell": b, "equal": False, "ref": "ran"}
    receipt(root, f"{F.PARENT_NAV}-run2", b)                              # the parent's last completed run: equal
    assert WR.nav_exe(w, done, WU.phase_rows(w, cell))["equal"] is True
    plain = F.cell_spec("c2", "out/nav-c2", reference_nav=F.PARENT_NAV)  # no ref phase (e.g. a rule cell)
    F.write_json(root, "scripts/specs/v8/lib-c2.json", plain)
    done2 = {"preflight": {"parent": {"nav": F.PARENT_NAV}}, "match": {"cell_spec": "scripts/specs/v8/lib-c2.json"}}
    assert WR.nav_exe(w, done2, WU.phase_rows(w, "scripts/specs/v8/lib-c2.json")) == {
        "parent": b, "cell": None, "equal": None, "ref": "none"}
    # a whole fake wave: the timings rows carry the key and wave-result.json the verify record
    root = F.build(tmp_path / "wave")
    lines: list[str] = []
    assert research_wave.main(["run", F.MANIFEST, "--root", str(root)], executor=F.FakeCycle(root, {
        "alpha_a": ("admitted", 1), "alpha_b": ("admitted", 1), "alpha_c": ("admitted", 1)}), log=lines.append) == 0
    res = json.loads((root / "out/waves/w1/wave-result.json").read_text())
    assert res["timings"] and all("executable_sha256" in r for r in res["timings"])
    assert res["nav_exe"] == {"parent": None, "cell": None, "equal": None, "ref": "none"}
    # research_cycle: fields equal to the parent's; the ref runs when the NAV exe differs from the parent NAV's
    root, sp = T.l8_root(tmp_path / "cyc")
    doc = json.loads(sp.read_text())
    doc["fields"] = {"output": "fields-base", "manifest_sha256": RC.sha256_file(root / "fields-base/manifest.json"),
                     "list": ["fa"]}
    sp.write_text(json.dumps(doc))
    T.behave(root, ic_files=T.GOOD_U)
    ref = lambda: next(s for s in T.cycle_of(root, sp).steps() if s.phase == "ref")   # noqa: E731
    assert ref().state == "skipped"                                       # no parent receipt: unknown, as before
    nav_exe = RC.sha256_file(root / "bin/nav.exe")
    receipt(root, "ref-cell-run", nav_exe)
    assert ref().state == "skipped" and "identical by construction" in ref().note           # the same exe
    receipt(root, "ref-cell-run2", a)                                     # the parent's last completed run: another
    st = ref()
    assert st.state == "pending" and f"the NAV exe differs (sha256 {nav_exe}, the parent NAV's {a})" in st.note
    log: list[str] = []
    assert T.run(root, sp, log) == RC.EXIT_OK
    assert "REF-run" in T.calls(root) and any(x.startswith("compare ref-s2 [file]") for x in log)


# ------------------------------------------------------------------ (e) OR-5: the ledger append lock
def test_ledger_append_lock_excl(tmp_path):
    """ledger_append's verify-and-append runs under <ledger>.lock (O_CREAT | O_EXCL): the appended bytes are those of
    the chain rule; a held lock makes a writer wait, then refuse with nothing appended; the lock is removed after an
    append and after a refusal; concurrent writers serialise into one valid chain."""
    led = tmp_path / "d" / "trials.jsonl"
    lock = led.with_name("trials.jsonl.lock")
    r1, r2 = F.construction_line("c1", sha(b"1"), 1.0), F.construction_line("c2", sha(b"2"), 1.1)
    appended, skipped = BI.ledger_append(led, [r1, r2], chain=True)
    assert len(appended) == 2 and skipped == [] and not lock.exists()
    one = json.dumps(dict(r1, prev_sha256=BI.CHAIN_GENESIS), sort_keys=True, separators=(",", ":"))
    two = json.dumps(dict(r2, prev_sha256=BI.line_sha256(one)), sort_keys=True, separators=(",", ":"))
    assert led.read_text(encoding="utf-8") == one + "\n" + two + "\n"                         # bytes unchanged
    before = led.read_bytes()
    lock.write_text('{"pid": 4242}\n')                                    # another writer holds it
    with pytest.raises(ValueError, match=r"trials.jsonl.lock is held \(\{\"pid\": 4242\}\)"):
        BI.ledger_append(led, [F.construction_line("c3", sha(b"3"), 1.2)], chain=True, lock_seconds=0.2)
    assert led.read_bytes() == before and lock.exists()                   # nothing appended; not ours to remove
    lock.unlink()
    broken = tmp_path / "broken.jsonl"
    broken.write_text(one + "\n" + json.dumps(dict(r2, prev_sha256="f" * 64), sort_keys=True,
                                               separators=(",", ":")) + "\n", encoding="utf-8")
    with pytest.raises(ValueError, match="hash chain broken"):           # a refusal inside the lock
        BI.ledger_append(broken, [F.construction_line("c3", sha(b"3"), 1.2)], chain=True)
    assert not broken.with_name("broken.jsonl.lock").exists()
    n, gate, errors = 12, threading.Barrier(12), []

    def writer(k: int) -> None:
        gate.wait()
        try:
            BI.ledger_append(led, [F.construction_line(f"t{k}", sha(f"t{k}".encode()), 1.0)], chain=True)
        except Exception as exc:  # noqa: BLE001  (reported below)
            errors.append(exc)
    threads = [threading.Thread(target=writer, args=(k,)) for k in range(n)]
    for t in threads:
        t.start()
    for t in threads:
        t.join(60)
    assert errors == [] and not lock.exists()
    records = BI.ledger_read(led)                                         # the chain verifies
    assert len(records) == 2 + n and {r["cell"] for r in records} == {"c1", "c2"} | {f"t{k}" for k in range(n)}


# ------------------------------------------------------------------ (g) F-7: the two-seed suite command
def test_two_seed_suite_command():
    """run_two_seeds.py runs the given pytest paths (default scripts/tests) once under PYTHONHASHSEED=0, once under
    PYTHONHASHSEED=1, from the repository root."""
    runs = RTS.runs([])
    assert [env for env, _ in runs] == [{"PYTHONHASHSEED": "0"}, {"PYTHONHASHSEED": "1"}]
    assert all(cmd[1:] == ["-m", "pytest", "-q", "-p", "no:cacheprovider", "scripts/tests"] for _, cmd in runs)
    assert RTS.runs(["atx-engine/tools", "atx-impl/tools"])[1][1][-2:] == ["atx-engine/tools", "atx-impl/tools"]
    assert RTS.REPO == RT.REPO
