"""Lane E1 (P9), tasks 1+ (wave 1 proper): the wave driver's run receipts, resume, attempts and launch (plan
2026-10-03-p9-sprint-plan.md, E1; contracts K-P9-10, K-P9-11). Task 0 (P0-FIX) is test_wave_hardening.py.

  test_receipt_k_p9_10_keys               K-P9-10: argv_sha256, attempt, executable_sha256, build_type in receipts
  test_resume_refuses_argv_mismatch       OR-3: a done bounded output is reused only on the command that made it
  test_attempt_subdir_after_floor_kill    OR-4: a planted floor kill resumes to completion in <run dir>/attempt-2
  test_driver_auto_attempt_manifest_flag  the manifest's driver block (each key opt-in) reaches research_cycle

Every file is synthetic (fake tools, sessions 2020-2021); no data of the repository is read.

Run: "C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider scripts/tests/test_wave_driver.py
"""
from __future__ import annotations

import hashlib
import json
from pathlib import Path
import subprocess
import sys

import pytest

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent))
import wave_fixture as F  # noqa: E402
import cycle_resume as CR  # noqa: E402
import research_cycle as RC  # noqa: E402
import research_tree as RT  # noqa: E402
import research_wave  # noqa: E402
import test_research_cycle as T  # noqa: E402  (the fake cycle tools)
import wave_manifest as WM  # noqa: E402
import wave_steps as WS  # noqa: E402
from wave_context import Wave  # noqa: E402

ADMIT_ALL = {"alpha_a": ("admitted", 1), "alpha_b": ("admitted", 1), "alpha_c": ("admitted", 1)}


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def manifest(**over) -> dict:
    """The fixture's wave manifest with its fields pin filled (F.build fills it from the file)."""
    m = F.manifest(**over)
    return dict(m, fields=dict(m["fields"], manifest_sha256="0" * 64))


def step_of(root: Path, sp: Path, phase: str, **kw):
    return next(s for s in T.cycle_of(root, sp, **kw).steps() if s.phase == phase)


def stamp(root: Path, run_dir: str, args: list[str], *, command: bool = True, digest: str | None = None) -> None:
    """Give a fake receipt the K-P9-10 keys the real runner writes: argv_sha256 of the command after its executable
    (and the command itself, unless ``command`` is false)."""
    p = root / run_dir / "receipt.json"
    doc = json.loads(p.read_text())
    doc["argv_sha256"] = digest or RT.argv_sha256(args)
    if command:
        doc["command"] = ["C:/python.exe", *args]
    p.write_text(json.dumps(doc))


def research_cycle_calls(fake, verb: str | None = None) -> list[list[str]]:
    """The research_cycle.py argvs a fake wave ran (of one verb, or every verb)."""
    return [a for a in fake.calls if a[1:2] and Path(a[1]).name == "research_cycle.py" and
            (verb is None or a[2] == verb)]


# ------------------------------------------------------------------ K-P9-10, OR-3, OR-4
def test_receipt_k_p9_10_keys(tmp_path):
    """Contract K-P9-10: the bounded runner's start.json and receipt.json carry argv_sha256 (research_tree.argv_sha256
    of the command after its executable), attempt (--attempt, default 1), executable_sha256 and build_type
    (--build-type Debug | Release, else null); an attempt outside 1..99 or another build type is a usage error."""
    root = tmp_path / "r"
    root.mkdir()
    runner = str(HERE.parent / "run_bounded_research.py")
    args = ["-c", "print('k-p9-10')"]
    base = [sys.executable, runner, "--root", str(root), "--no-git", "--seconds", "60", "--max-rss-mib", "512",
            "--min-free-mib", "64"]
    done = subprocess.run([*base, "--output", str(root / "a-run" / "attempt-2"), "--attempt", "2", "--build-type",
                           "Release", "--", sys.executable, *args], capture_output=True, text=True)
    assert done.returncode == 0, done.stderr
    for name in ("start.json", "receipt.json"):
        r = json.loads((root / "a-run" / "attempt-2" / name).read_text())
        assert r["argv_sha256"] == RT.argv_sha256(args) == CR.argv_digest(args)
        assert r["attempt"] == 2 and r["build_type"] == "Release"
        assert r["executable_sha256"] == sha(Path(r["command"][0]).read_bytes())
        assert "admission" not in r                                       # no admission flag: no admission block
    assert json.loads((root / "a-run" / "attempt-2" / "receipt.json").read_text())["outcome"] == "completed"
    done = subprocess.run([*base, "--output", str(root / "b-run"), "--", sys.executable, *args],
                          capture_output=True, text=True)
    r = json.loads((root / "b-run" / "receipt.json").read_text())
    assert done.returncode == 0 and r["attempt"] == 1 and r["build_type"] is None
    for bad in (["--attempt", "0"], ["--attempt", "100"], ["--build-type", "RelWithDebInfo"]):
        done = subprocess.run([*base, "--output", str(root / "c-run"), *bad, "--", sys.executable, *args],
                              capture_output=True, text=True)
        assert done.returncode == 2 and "--attempt is 1..99" in done.stderr and not (root / "c-run").exists()
    # research_cycle passes the spec's build type (BUILDS: equity = the dev preset's Debug, equity-rel = Release)
    assert RT.BUILD_TYPES == {"equity": "Debug", "equity-rel": "Release"} and set(RT.BUILD_TYPES) == set(RC.BUILDS)
    rroot, sp = T.make_root(tmp_path / "cyc")
    u = step_of(rroot, sp, "u")
    assert "--build-type" not in u.argv and "--attempt" not in u.argv      # no build key: the argv of before
    sp.write_text(json.dumps(dict(json.loads(sp.read_text()), build="equity-rel")))
    u = step_of(rroot, sp, "u")
    k = u.argv.index("--")
    assert u.argv[u.argv.index("--build-type") + 1] == "Release" and u.argv.index("--build-type") < k


def test_resume_refuses_argv_mismatch(tmp_path):
    """P9 OR-3: a done bounded output whose receipt carries argv_sha256 is reused only when the receipt's command after
    its executable equals the command the spec runs now (cycle_resume.REUSE_NEUTRAL options aside: the u pass's
    --save-combined / --no-composition); with argv_sha256 and no command the digest decides. A changed input of the
    phase under the same output name is a HARD-STOP (exit 3), nothing runs; a receipt written before K-P9-10 is
    reused as before."""
    root, sp = T.make_root(tmp_path)
    assert T.run(root, sp) == RC.EXIT_OK                                  # fake receipts: no argv_sha256 (legacy)
    n = len(T.calls(root))
    u = step_of(root, sp, "u")
    args = CR.step_args(u)
    stamp(root, u.run_dir, args)
    log: list[str] = []
    assert T.run(root, sp, log) == RC.EXIT_OK
    assert f"   receipt argv: argv sha256 {RT.argv_sha256(args)} ({u.run_dir}/receipt.json)" in log
    stamp(root, u.run_dir, [*args, "--no-composition"])                  # the screen's u: reuse-neutral
    log = []
    assert T.run(root, sp, log) == RC.EXIT_OK
    assert any(x.startswith("   receipt argv:") and x.endswith("reuse-neutral options aside") for x in log)
    doc = json.loads(sp.read_text())
    doc["ic"] = dict(doc["ic"], flags=["--workers", "8"])                 # an input of u changed, its name did not
    sp.write_text(json.dumps(doc))
    with pytest.raises(RC.CycleError, match=r"HARD-STOP \[u\]: u output out/U-1 was made by a command with argv "
                                            r"sha256 .* refusing to reuse it") as e:
        T.run(root, sp)
    assert e.value.code == RC.EXIT_PIN
    stamp(root, u.run_dir, args, command=False)                          # digest only: compared by digest
    with pytest.raises(RC.CycleError, match="refusing to reuse it"):
        T.run(root, sp)
    doc["ic"] = dict(doc["ic"], flags=["--workers", "4"])
    sp.write_text(json.dumps(doc))
    assert T.run(root, sp) == RC.EXIT_OK                                  # the digest of the same args: reused
    w = step_of(root, sp, "w")                                            # every bounded phase: the w pass too
    stamp(root, w.run_dir, CR.step_args(w)[:-1])
    with pytest.raises(RC.CycleError, match=r"HARD-STOP \[w\]"):
        T.run(root, sp)
    assert len([c for c in T.calls(root)[n:] if not c.startswith(("check", "summ"))]) == 0   # nothing re-ran
    assert CR.reuse_args("fit", ["--a", "--theme-resid-parent", "p", "--theme-resid-parent-sha256", "s", "--b"]) == \
        ["--a", "--b"]


def test_attempt_subdir_after_floor_kill(tmp_path):
    """P9 OR-4: a bounded attempt the host refused for memory (here a planted floor kill, system-memory-limit, before
    it wrote anything) stops the cycle; under --auto-attempt the resume runs attempt 2 in <run dir>/attempt-2 (runner
    --attempt 2) and completes; later invocations read the step from that sub-dir. Without the flag it stays a stop
    naming the flag; an attempt that left output behind is never retried (never overwritten); at most MAX_ATTEMPTS."""
    root, sp = T.make_root(tmp_path / "a")
    T.behave(root, **{"U-run1": "floor-kill"})
    with pytest.raises(RC.CycleError, match="system-memory-limit.*a resume with --auto-attempt runs the next attempt"):
        T.run(root, sp)
    T.behave(root)
    with pytest.raises(RC.CycleError, match=r"--auto-attempt runs attempt 2 in out/U-run1/attempt-2"):
        T.run(root, sp)                                                   # no flag: a stop, as before
    assert not (root / "out/U-run1/attempt-2").exists()
    st = step_of(root, sp, "u", auto_attempt=True)
    assert st.state == "pending" and st.run_dir == "out/U-run1/attempt-2" and st.output == "out/U-1"
    assert st.argv[st.argv.index("--attempt") + 1] == "2" and st.argv[st.argv.index("--output") + 1] == st.run_dir
    assert "attempt 2 (--auto-attempt): out/U-run1 was refused (system-memory-limit)" in st.note
    log: list[str] = []
    assert T.run(root, sp, log, auto_attempt=True) == RC.EXIT_OK and "== cycle complete" in log
    assert T.calls(root)[2:5] == ["U-run1", "check", "check"] and "attempt-2" in T.calls(root)
    assert json.loads((root / "out/U-run1/attempt-2/receipt.json").read_text())["outcome"] == "completed"
    st = step_of(root, sp, "u")                                           # later: read from the sub-dir, done
    assert st.done and st.run_dir == "out/U-run1/attempt-2" and "--attempt" in st.argv
    n = len(T.calls(root))
    assert T.run(root, sp) == RC.EXIT_OK and all(c.startswith(("check", "summ")) for c in T.calls(root)[n:])
    assert any(x.startswith("u      done") and "out/U-run1/attempt-2" in x for x in RC.status_lines(T.cycle_of(root, sp)))
    w = Wave.__new__(Wave)                                                # the wave reads every attempt (timings, seal)
    w.root = root
    assert w.run_dirs("out/U") == ["out/U-run1", "out/U-run1/attempt-2"]
    # a floor kill after the process wrote its output dir: never retried, never overwritten
    root, sp = T.make_root(tmp_path / "b")
    T.behave(root, **{"U-run1": "floor-kill-partial"})
    with pytest.raises(RC.CycleError, match="system-memory-limit"):
        T.run(root, sp, auto_attempt=True)
    T.behave(root)
    with pytest.raises(RC.CycleError, match="--attempt u=2"):
        T.run(root, sp, auto_attempt=True)
    assert not (root / "out/U-run1/attempt-2").exists()
    # MAX_ATTEMPTS: attempts 2..9 refused too, no tenth
    root, sp = T.make_root(tmp_path / "c")
    T.behave(root, **{"U-run1": "refuse"})
    with pytest.raises(RC.CycleError):
        T.run(root, sp)
    for k in range(2, RC.MAX_ATTEMPTS + 1):
        F.write_json(root, f"out/U-run1/attempt-{k}/receipt.json", {"outcome": "prelaunch-memory-refusal",
                                                                     "exit_code": None})
    st = step_of(root, sp, "u", auto_attempt=True)
    assert st.state == "failed" and st.run_dir == f"out/U-run1/attempt-{RC.MAX_ATTEMPTS}"
    assert f"(attempt {RC.MAX_ATTEMPTS})" in st.note and f"the {RC.MAX_ATTEMPTS} attempts are spent" in st.note
    # attempt 2 crashed after writing its output: never retried, and the note names attempt 2's receipt
    root, sp = T.make_root(tmp_path / "d")
    T.behave(root, **{"U-run1": "floor-kill"})
    with pytest.raises(RC.CycleError):
        T.run(root, sp)
    T.behave(root, **{"attempt-2": "floor-kill-partial"})
    with pytest.raises(RC.CycleError, match=r"receipt out/U-run1/attempt-2: outcome system-memory-limit"):
        T.run(root, sp, auto_attempt=True)
    st = step_of(root, sp, "u", auto_attempt=True)
    assert st.state == "failed" and st.run_dir == "out/U-run1/attempt-2" and not (root / "out/U-run1/attempt-3").exists()
    assert st.note.startswith("receipt out/U-run1/attempt-2: outcome system-memory-limit, exit_code 1 (attempt 2)")
    # the NAV: its binding is written into, and read back from, the attempt sub-dir
    root, sp = T.make_root(tmp_path / "e")
    T.behave(root, **{"N-run": "floor-kill"})
    with pytest.raises(RC.CycleError, match=r"HARD-STOP \[nav\]"):
        T.run(root, sp, auto_attempt=True)
    T.behave(root)
    assert T.run(root, sp, auto_attempt=True) == RC.EXIT_OK
    assert (root / "out/N-run/attempt-2/cycle_binding.json").is_file() and not (root / "out/N-run/cycle_binding.json")\
        .exists()
    log = []
    assert T.run(root, sp, log) == RC.EXIT_OK
    assert any(x.startswith("   binding: ") and "out/N-run/attempt-2/cycle_binding.json" in x for x in log)


def test_driver_auto_attempt_manifest_flag(tmp_path):
    """A wave manifest's driver block (wave_manifest.DRIVER_KEYS) gives every research_cycle run of the wave
    --auto-attempt when driver.auto_attempt is true; without the block every argv is that of before."""
    assert WM.validate(manifest(driver={"auto_attempt": True})) == []
    assert WM.validate(manifest(driver={"auto_attempt": "yes"})) == ["driver.auto_attempt must be true or false"]
    assert WM.validate(manifest(driver={"retry": True})) == ["driver: unknown key 'retry' (known: "
                                                             f"{', '.join(WM.DRIVER_KEYS)})"]
    assert WM.validate(manifest(driver=[])) and WM.validate(manifest(launch={"auto_attempt": True}))
    assert WS.driver_flags(None) == [] and WS.driver_flags({"auto_attempt": False}) == []
    assert WS.driver_flags({"auto_attempt": True}) == ["--auto-attempt"]
    for driver, want in ((None, False), ({"auto_attempt": True}, True)):
        root = F.build(tmp_path / f"w{want}", **({"driver": driver} if driver else {}))
        fake = F.FakeCycle(root, ADMIT_ALL)
        assert research_wave.main(["run", F.MANIFEST, "--root", str(root)], executor=fake, log=lambda s: None) == 0
        runs, others = research_cycle_calls(fake, "run"), [a for a in research_cycle_calls(fake) if a[2] != "run"]
        assert runs and all(("--auto-attempt" in a) is want for a in runs)
        assert others and not any("--auto-attempt" in a for a in others)
