"""Lane E1 (P9), tasks 1+ (wave 1 proper): the wave driver's run receipts, resume, attempts and launch (plan
2026-10-03-p9-sprint-plan.md, E1; contracts K-P9-10, K-P9-11). Task 0 (P0-FIX) is test_wave_hardening.py.

  test_receipt_k_p9_10_keys               K-P9-10: argv_sha256, attempt, executable_sha256, build_type in receipts
  test_resume_refuses_argv_mismatch       OR-3: a done bounded output is reused only on the command that made it
  test_attempt_subdir_after_floor_kill    OR-4: a planted floor kill resumes to completion in <run dir>/attempt-2
  test_driver_auto_attempt_manifest_flag  the manifest's driver block (each key opt-in) reaches research_cycle
  test_launch_waits_for_free_memory       F-5 (a): bounded launch admission (free memory, no compiler, host claims)
  test_parallel_steps_under_host_budget   OR section 5: ref || u, card || marginal, the judge's summ || bundle || book
  test_lock_exes_pins_and_verify_compares OR-2: lock --exes writes exes_sha256, runs check it, verify compares
  test_receipt_digest_time_free           OR section 3: content digests (no time keys); the manifest's record date
  test_reader_reuse_keyed_on_code_and_verdict_per_run  OR section 3: code-keyed reuse; per-run verdict copies

Every file is synthetic (fake tools, sessions 2020-2021); no data of the repository is read.

Run: "C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider scripts/tests/test_wave_driver.py
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import threading

import psutil
import pytest

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent))
import wave_fixture as F  # noqa: E402
import cycle_resume as CR  # noqa: E402
import research_cycle as RC  # noqa: E402
import research_tree as RT  # noqa: E402
import research_wave  # noqa: E402
import run_bounded_research as RB  # noqa: E402
import test_research_cycle as T  # noqa: E402  (the fake cycle tools)
import wave_manifest as WM  # noqa: E402
import wave_queue as WQ  # noqa: E402
import wave_stage_cell as WSC  # noqa: E402
import wave_stage_record as WR  # noqa: E402
import wave_stage_util as WU  # noqa: E402
import wave_steps as WS  # noqa: E402
from wave_context import Wave, stage_chain as SC  # noqa: E402

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


# ------------------------------------------------------------------ F-5 (a), OR section 5: admission and parallel
class Clock:
    """A fake monotonic clock whose sleep advances it (the admission loop's wait without waiting)."""

    def __init__(self):
        self.t = 0.0

    def __call__(self) -> float:
        return self.t

    def sleep(self, seconds: float) -> None:
        self.t += seconds


def admit_args(tmp_path: Path, **over) -> argparse.Namespace:
    a = argparse.Namespace(max_rss_mib=512, min_free_mib=256, admission_wait_seconds=10.0, host_budget_mib=None,
                           host_claims=tmp_path / "claims")
    for key, value in over.items():
        setattr(a, key, value)
    return a


def write_claim(claims: Path, name: str, pid: int, born: float, mib: int) -> Path:
    claims.mkdir(parents=True, exist_ok=True)
    p = claims / f"{name}{RB.CLAIM_SUFFIX}"
    p.write_text(json.dumps({"pid": pid, "create_time": born, "mib": mib, "output": name}))
    return p


def test_launch_waits_for_free_memory(tmp_path):
    """P9 F-5 (a): with --admission-wait-seconds the runner waits before the launch, at most that long, until free
    memory >= the declared peak + floor and no compiler or linker runs; with --host-budget-mib it also waits for a
    claim of its cap under the host budget (live claims summed, a killed process's claim dropped) and holds it until
    its tree ends. A wait that runs out writes outcome prelaunch-admission-timeout with nothing launched (an
    AUTO_OUTCOMES refusal: --auto-attempt runs it again). Without the flags no argv, receipt key or wait changes."""
    clock = Clock()
    free, busy = iter([100 << 20, 700 << 20, 800 << 20]), iter([[], ["cl.exe"], []])
    block, held = RB.admit(admit_args(tmp_path), tmp_path / "o", available=lambda: next(free),
                           compilers=lambda: next(busy), clock=clock, sleep=clock.sleep)
    assert held is None and (block["checks"], block["free_mib"], block["need_free_mib"]) == (3, 800, 768)
    assert block["waited_seconds"] == 2 * RB.POLL_SECONDS and "refused" not in block
    clock = Clock()
    with pytest.raises(RB.AdmissionTimeout, match="still waiting after 2.0 s") as e:
        RB.admit(admit_args(tmp_path, admission_wait_seconds=2.0), tmp_path / "o", available=lambda: 100 << 20,
                 compilers=lambda: ["ninja.exe"], clock=clock, sleep=clock.sleep)
    assert e.value.block["refused"] == ["free memory 100 MiB < 768 MiB (peak 512 + floor 256)", "running: ninja.exe"]
    assert e.value.block["checks"] == 5 and RB.OUTCOME_ADMISSION in CR.AUTO_OUTCOMES
    assert {"cl.exe", "clang-cl.exe", "ninja.exe", "lld-link.exe", "ninja"} <= RB.COMPILERS
    # the host semaphore: a live claim (this process) counts, a killed one's (a reused pid) is dropped
    me, claims = psutil.Process(), tmp_path / "claims"
    live = write_claim(claims, "live", me.pid, me.create_time(), 900)
    stale = write_claim(claims, "stale", me.pid, me.create_time() - 3600, 4000)
    (claims / RB.CLAIMS_LOCK).write_text(json.dumps({"pid": me.pid, "create_time": me.create_time() - 3600}))
    args, clock = admit_args(tmp_path, host_budget_mib=1000, admission_wait_seconds=1.0), Clock()
    with pytest.raises(RB.AdmissionTimeout, match=r"claimed 900 MiB \+ 512 MiB > host budget 1000 MiB"):
        RB.admit(args, tmp_path / "o", available=lambda: 8 << 30, compilers=lambda: [], clock=clock, sleep=clock.sleep)
    assert live.exists() and not stale.exists() and not (claims / RB.CLAIMS_LOCK).exists()   # a killed lock: gone
    live.unlink()
    block, held = RB.admit(args, tmp_path / "o", available=lambda: 8 << 30, compilers=lambda: [], clock=Clock(),
                           sleep=lambda s: None)
    mine = list(claims.glob("*" + RB.CLAIM_SUFFIX))
    assert block["claimed_by_others_mib"] == 0 and len(mine) == 1 and json.loads(mine[0].read_text())["mib"] == 512
    assert block["host_budget_mib"] == 1000 and block["claims_dir"] == str(claims)
    held.release()
    assert not list(claims.glob("*" + RB.CLAIM_SUFFIX))
    # the runner, end to end: a live claim leaves no room, so the wait runs out (whatever the host's memory)
    write_claim(claims, "live", me.pid, me.create_time(), 900)
    root = tmp_path / "r"
    root.mkdir()
    base = [sys.executable, str(HERE.parent / "run_bounded_research.py"), "--root", str(root), "--no-git", "--seconds",
            "60", "--max-rss-mib", "512", "--min-free-mib", "64"]
    admit = ["--admission-wait-seconds", "1", "--host-budget-mib", "1000", "--host-claims", str(claims)]
    done = subprocess.run([*base, *admit, "--output", str(root / "t-run"), "--", sys.executable, "-c", "print(1)"],
                          capture_output=True, text=True, timeout=120)
    r = json.loads((root / "t-run" / "receipt.json").read_text())
    assert done.returncode == 1 and r["outcome"] == RB.OUTCOME_ADMISSION and r["exit_code"] is None
    assert r["admission"]["refused"] and r["admission"]["wait_seconds"] == 1.0
    assert r["limits"] == {"seconds": 60.0, "max_rss_mib": 512, "min_free_mib": 64, "admission_wait_seconds": 1.0,
                           "host_budget_mib": 1000}
    assert (root / "t-run" / "stdout.log").read_bytes() == b"" and len(list(claims.glob("*.claim"))) == 1
    for bad in (["--host-budget-mib", "1000"], ["--admission-wait-seconds", "0"],
                ["--admission-wait-seconds", "1", "--host-budget-mib", "256"]):     # no wait; zero; below the cap
        done = subprocess.run([*base, *bad, "--output", str(root / "x-run"), "--", sys.executable, "-V"],
                              capture_output=True, text=True, timeout=60)
        assert done.returncode == 2 and not (root / "x-run").exists(), bad
    # research_cycle: the flags reach every bounded process's runner argv, before its --output; none without them
    croot, sp = T.make_root(tmp_path / "cyc")
    assert not any("--admission-wait-seconds" in s.argv or "--host-budget-mib" in s.argv
                   for s in T.cycle_of(croot, sp).steps() if s.kind == "bounded")
    c = T.cycle_of(croot, sp, launch={"admission_wait_seconds": 300.0, "host_budget_mib": 4096})
    for st in (s for s in c.steps() if s.kind == "bounded"):
        k = st.argv.index("--output")
        assert st.argv[k - 4:k] == ["--admission-wait-seconds", "300", "--host-budget-mib", "4096"], st.phase
    with pytest.raises(RC.CycleError, match="--host-budget-mib 1024 is below the u cap max_rss_mib 1536") as e:
        T.cycle_of(croot, sp, launch={"admission_wait_seconds": 300, "host_budget_mib": 1024}).steps()
    assert e.value.code == RC.EXIT_USAGE
    assert RC.main(["plan", str(sp), "--root", str(croot), "--host-budget-mib", "4096"]) == RC.EXIT_USAGE
    # the wave: driver keys checked, then passed to research_cycle and to the readers' and bundle's runner
    assert WM.validate(manifest(driver={"admission_wait_seconds": 60, "host_budget_mib": 4096})) == []
    for bad, needle in (({"host_budget_mib": 4096}, "needs driver.admission_wait_seconds"),
                        ({"admission_wait_seconds": 60, "host_budget_mib": 1024}, "below the readers' cap 1536"),
                        ({"admission_wait_seconds": 0}, "driver.admission_wait_seconds must be"),
                        ({"admission_wait_seconds": "60"}, "driver.admission_wait_seconds must be"),
                        ({"admission_wait_seconds": 60, "host_budget_mib": 10 ** 6}, "driver.host_budget_mib must be")):
        assert any(needle in p for p in WM.validate(manifest(driver=bad))), bad
    driver = {"admission_wait_seconds": 60}
    assert WS.driver_flags(driver) == ["--admission-wait", "60"]
    assert WS.runner_launch_flags(driver) == ["--admission-wait-seconds", "60"] and WS.runner_launch_flags(None) == []
    root = F.build(tmp_path / "w", driver=driver)
    fake = F.FakeCycle(root, ADMIT_ALL)
    assert research_wave.main(["run", F.MANIFEST, "--root", str(root)], executor=fake, log=lambda s: None) == 0
    runs = research_cycle_calls(fake, "run")
    bounded = [a for a in fake.calls if a[1:2] and Path(a[1]).name == "run_bounded_research.py"]
    assert runs and all(a[-4:-2] == ["--admission-wait", "60"] for a in runs)
    assert bounded and all(a[a.index("--output") - 2:a.index("--output")] == ["--admission-wait-seconds", "60"]
                           for a in bounded)


def gated_executor(names: set[str], seen: list[str]):
    """An executor that makes the bounded steps whose run dirs are ``names`` meet at a barrier before they run: a
    pair that does not run at once times out there (BrokenBarrierError, the test fails)."""
    gate = threading.Barrier(len(names), timeout=30)

    def run(argv, root, env, capture):
        name = Path(argv[argv.index("--output") + 1]).name if "--output" in argv else ""
        if name in names:
            gate.wait()
            seen.append(name)
        return RC.execute(argv, root, env, capture)
    return run


def test_parallel_steps_under_host_budget(tmp_path):
    """P9 OR section 5: under a host memory budget (--host-budget-mib, every bounded process holding its declared cap
    in the host semaphore) ref || u and card || marginal run at once (a barrier both must reach), then the cycle goes
    on as before (the compares after both); the judge runs the cell's summ, the bundle and the book reader at once.
    Without the budget the order is that of before; a failed partner stops the cycle after its pair finished; nothing
    runs past --stop-after."""
    launch = {"admission_wait_seconds": 60, "host_budget_mib": 8192}
    root, sp = T.l8_root(tmp_path / "ref")
    T.behave(root, ic_files=T.GOOD_U)
    seen: list[str] = []
    log: list[str] = []
    assert RC.run_cycle(T.cycle_of(root, sp, launch=launch), log=log.append, clean=lambda r: True,
                        executor=gated_executor({"REF-run", "U-run1"}, seen)) == RC.EXIT_OK
    assert sorted(seen) == ["REF-run", "U-run1"] and "   ref || u: side by side under the host memory budget 8192 MiB" \
        in log
    assert log.index("== ref (attempt 1)") < log.index("== u (attempt 1)") < log.index(
        "compare ref-s2 [file]: ref-cell/daily_s2.csv vs out/REF/daily_s2.csv") < log.index("== u: done (out/U-1)")
    root, sp = T.screen_root(tmp_path / "scr")
    seen = []
    log = []
    assert RC.run_cycle(T.cycle_of(root, sp, screen=True, capabilities=T.CAPS, launch=launch), log=log.append,
                        clean=lambda r: True, executor=gated_executor({"C-run", "MIC-run"}, seen)) == RC.EXIT_OK
    assert sorted(seen) == ["C-run", "MIC-run"] and any(x.startswith("   card || marginal: side by side") for x in log)
    assert (root / "out/C/index.json").is_file() and (root / "out/MIC/marginal_ic.json").is_file()
    root, sp = T.screen_root(tmp_path / "plain")                          # no budget: one at a time, as before
    log = []
    assert T.run(root, sp, log, screen=True, capabilities=T.CAPS) == RC.EXIT_OK
    assert not any(" || " in x for x in log) and T.calls(root).index("C-run") < T.calls(root).index("MIC-run")
    root, sp = T.screen_root(tmp_path / "fail")                           # the partner fails after the pair ran
    T.behave(root, **{"MIC-run": "error"})
    with pytest.raises(RC.CycleError, match=r"HARD-STOP \[marginal\]: receipt out/MIC-run: outcome process-error"):
        RC.run_cycle(T.cycle_of(root, sp, screen=True, capabilities=T.CAPS, launch=launch), log=lambda s: None,
                     clean=lambda r: True)
    assert (root / "out/C/index.json").is_file()
    root, sp = T.l8_root(tmp_path / "stop")                               # --stop-after ref: u does not start
    T.behave(root, ic_files=T.GOOD_U)
    assert RC.run_cycle(T.cycle_of(root, sp, launch=launch), stop_after="ref", log=lambda s: None,
                        clean=lambda r: True) == RC.EXIT_OK
    assert "U-run1" not in T.calls(root) and "REF-run" in T.calls(root)
    # the wave's judge: the cell's run (monitor, summ), the bundle and the book reader meet at a barrier
    root = F.build(tmp_path / "w", driver=launch)
    fake, gate, met = F.FakeCycle(root, ADMIT_ALL), threading.Barrier(3, timeout=30), []

    def judge_gated(argv, root_, env=None):
        tool = Path(argv[1]).name if argv[0] != "git" else "git"
        score = tool == "research_cycle.py" and argv[2] == "run" and not {"--screen", "--stop-after"} & set(argv)
        if score or "--bundle" in argv or (tool == "run_bounded_research.py" and "book" in argv):
            gate.wait()
            met.append("summ" if score else "bundle" if "--bundle" in argv else "book")
        return fake(argv, root_, env)
    lines: list[str] = []
    assert research_wave.main(["run", F.MANIFEST, "--root", str(root)], executor=judge_gated, log=lines.append) == 0
    assert sorted(met) == ["book", "bundle", "summ"] and any("summ || bundle || book reader, side by side" in x
                                                             for x in lines)
    assert json.loads((root / "out/waves/w1/wave-result.json").read_text())["verdict"]["rule"] == "pm7-34"


# ------------------------------------------------------------------ OR-2: exes pinned by lock, compared by verify
def test_lock_exes_pins_and_verify_compares(tmp_path):
    """P9 OR-2: `lock --exes` writes exes_sha256 (the SHA-256 of every exe the cell runs; a template's into
    change.set); plan and run stop (exit 3) when an exe no longer hashes to its pin, and a re-lock re-pins it (noted
    RELOCKED). Without --exes the lock writes what it wrote before. Under driver.lock_exes the wave locks every spec it
    writes with --exes before the commit; verify records the parent's and the cell's pins and refuses a cell whose
    pinned NAV exe moved without its reference construction on it."""
    root, sp = T.make_root(tmp_path / "c")
    plain, _ = RC.lock(sp, root)
    assert RC.EXES_PIN not in plain
    spec, notes = RC.lock(sp, root, exes=True)
    ic, nav = sha(b"ic"), sha(b"nav")
    assert spec[RC.EXES_PIN] == {"ic": ic, "nav": nav} and f"locked exe nav: bin/nav.exe {nav}" in notes
    assert {k: v for k, v in spec.items() if k != RC.EXES_PIN} == plain       # nothing else moves
    assert RC.main(["lock", str(sp), "--root", str(root), "--exes", "--write"]) == RC.EXIT_OK
    c = T.cycle_of(root, sp)
    assert c.exe_pins == {"ic": ("bin/ic.exe", ic), "nav": ("bin/nav.exe", nav)}
    assert f"# pin exe nav: bin/nav.exe {nav} [exes_sha256, verified]" in RC.header(c)
    assert not any(x.startswith("# pin exe") for x in RC.header(T.cycle_of(*T.make_root(tmp_path / "p"))))
    (root / "bin" / "nav.exe").write_bytes(b"nav rebuilt")                   # a rebuild: plan / run stop
    with pytest.raises(RC.CycleError, match=rf"PIN MISMATCH exe nav: bin/nav.exe is {sha(b'nav rebuilt')}, spec "
                                            rf"exes_sha256 pins {nav}") as e:
        T.cycle_of(root, sp)
    assert e.value.code == RC.EXIT_PIN
    _, notes = RC.lock(sp, root, exes=True)
    assert f"RELOCKED exe nav: bin/nav.exe {nav} -> {sha(b'nav rebuilt')}" in notes
    for bad in ({"ic": "x"}, {"other": ic}, {}):
        with pytest.raises(RC.CycleError, match="exes_sha256 must map exes keys"):
            RC.validate_spec(dict(json.loads(sp.read_text()), exes_sha256=bad))
    # a template: its pins go into change.set (over the pins its parent's spec carries)
    root, sp = T.make_root(tmp_path / "t")
    assert T.run(root, sp) == RC.EXIT_OK                                    # the parent's outputs (derived inputs)
    assert RC.main(["lock", str(sp), "--root", str(root), "--exes", "--write"]) == RC.EXIT_OK
    child = sp.parent / "child.json"
    child.write_text(json.dumps({"schema": RC.research_spec.TEMPLATE_SCHEMA, "name": "child", "parent": sp.name,
                                 "change": {"set": {"nav.output": "out/N2"}}}))
    assert RC.load_spec(child)[RC.EXES_PIN] == {"ic": ic, "nav": nav}       # inherited from the parent's spec
    (root / "bin" / "nav.exe").write_bytes(b"nav 2")
    doc, notes = RC.lock(child, root, exes=True)
    assert doc["change"]["set"][RC.EXES_PIN] == {"ic": ic, "nav": sha(b"nav 2")} and RC.EXES_PIN not in doc
    assert WS.unpinned(doc)["change"]["set"] == {"nav.output": "out/N2"}     # a resumed rule cell compares without it
    # the wave: driver.lock_exes locks the specs it writes with --exes before their commit
    assert WM.validate(manifest(driver={"lock_exes": True})) == []
    root = F.build(tmp_path / "w", driver={"lock_exes": True})
    fake = F.FakeCycle(root, ADMIT_ALL)
    assert research_wave.main(["run", F.MANIFEST, "--root", str(root)], executor=fake, log=lambda s: None) == 0
    locks = [a for a in research_cycle_calls(fake, "lock") if "--exes" in a]
    assert [F.unrooted(a)[3:] for a in locks] == [["scripts/specs/v8/lib-w1.json", "--exes", "--write"]]
    k = fake.calls.index(locks[0])
    assert fake.calls[k + 1][:2] == ["git", "add"] and "scripts/specs/v8/lib-w1.json" in fake.calls[k + 1]
    root = F.build(tmp_path / "w0")
    fake = F.FakeCycle(root, ADMIT_ALL)
    assert research_wave.main(["run", F.MANIFEST, "--root", str(root)], executor=fake, log=lambda s: None) == 0
    assert not any("--exes" in a for a in fake.calls)                      # no driver key: the argv of before
    res = json.loads((root / "out/waves/w1/wave-result.json").read_text())
    assert "exes_sha256" not in res and "exes_sha256" not in json.loads(
        (root / "out/waves/w1/receipts/07-verify.json").read_text())["outputs"]
    # verify's record and refusal
    a, b = "a" * 64, "b" * 64
    F.write_json(root, F.PARENT, dict(F.cell_spec("p0", F.PARENT_NAV, reference_nav="out/nav-base"),
                                      exes_sha256={"ic": a, "nav": a}))
    F.write_json(root, "scripts/specs/v8/lib-c.json", dict(F.cell_spec("c", "out/nav-c", reference_nav=F.PARENT_NAV),
                                                           exes_sha256={"ic": a, "nav": b}))
    w = Wave(F.MANIFEST, root, executor=fake, log=lambda s: None)
    done = {"preflight": {"parent": {"spec": F.PARENT}}, "match": {"cell_spec": "scripts/specs/v8/lib-c.json"}}
    pins = WR.exes_pins(w, done)
    assert pins == {"parent": {"ic": a, "nav": a}, "cell": {"ic": a, "nav": b}, "differ": ["nav"]}
    assert "exes_sha256 pins: cell " + b in WR.exe_problem({"equal": None, "ref": "missing", "cell": b,
                                                             "parent": a}, pins)
    assert WR.exe_problem({"equal": None, "ref": "ran", "cell": b, "parent": a}, pins) is None
    assert WR.exe_problem({"equal": None, "ref": "none", "cell": b, "parent": a}, pins) is None   # a rule cell
    assert WR.exe_problem({"equal": True, "ref": "missing", "cell": a, "parent": a}, None) is None
    assert "differs from the parent NAV's" in WR.exe_problem({"equal": False, "ref": "missing", "cell": b,
                                                              "parent": a}, None)


# ------------------------------------------------------------------ OR section 3: determinism and stale reuse
def queued_root(path: Path, **over) -> Path:
    """A wave_fixture root (manifest keys ``over``) whose three candidates are queued and pinned, committed."""
    root = F.build(path, **over)
    for c in F.manifest()["candidates"]:
        d = dict(c, schema=WQ.SCHEMA, status="proposed", wave=None,
                 history=[{"status": "proposed", "at": "2026-10-02", "by": "lane"}])
        WQ.write(root, WQ.transition(d, "pinned", "PM", "2026-10-02"))
    F.git(root, "add", "-A")
    F.git(root, "commit", "-q", "-m", "queue")
    return root


def test_receipt_digest_time_free(tmp_path):
    """P9 OR section 3: under driver.receipt_digest "content" the wave's stage receipts are chained, and digested in
    wave-result.json, over their content keys (stage_chain.content_sha256: no started_utc / seconds), so a time
    field never moves a digest; under driver.record_date the queue history is dated from the manifest, not from the
    day the record ran. Without the keys: file digests and today's date, as before."""
    root = queued_root(tmp_path / "c", driver={"receipt_digest": "content", "record_date": "2026-10-01"})
    assert research_wave.main(["run", F.MANIFEST, "--root", str(root)], executor=F.FakeCycle(root, ADMIT_ALL),
                              log=lambda s: None) == 0
    rdir = root / "out/waves/w1/receipts"
    res = json.loads((root / "out/waves/w1/wave-result.json").read_text())
    assert res["receipts"] == {p.stem: SC.content_sha256(p) for p in sorted(rdir.glob("*.json")) if p.stem in
                               res["receipts"]} and len(res["receipts"]) == 8          # every stage before record
    two = json.loads((rdir / "02-register.json").read_text())
    assert two["inputs"] == {SC.PREV_CONTENT: SC.content_sha256(rdir / "01-preflight.json")}
    one = json.loads((rdir / "01-preflight.json").read_text())
    before = SC.content_sha256(rdir / "01-preflight.json")
    (rdir / "01-preflight.json").write_text(json.dumps(dict(one, started_utc="2020-01-02T00:00:00+00:00",
                                                            seconds=123.0), indent=2, sort_keys=True) + "\n")
    assert SC.content_sha256(rdir / "01-preflight.json") == before            # a time field moves no digest
    w = Wave(F.MANIFEST, root, executor=F.FakeCycle(root, ADMIT_ALL), log=lambda s: None)
    assert [r["state"] for r in research_wave.chain_of(w).state(w)] == ["done"] * 9   # the chain still verifies
    assert {c["history"][-1]["at"] for c in WQ.load(root).values()} == {"2026-10-01"}
    root = queued_root(tmp_path / "f")                                        # no keys: as before
    assert research_wave.main(["run", F.MANIFEST, "--root", str(root)], executor=F.FakeCycle(root, ADMIT_ALL),
                              log=lambda s: None) == 0
    rdir = root / "out/waves/w1/receipts"
    res = json.loads((root / "out/waves/w1/wave-result.json").read_text())
    assert res["receipts"]["01-preflight"] == SC.sha256_file(rdir / "01-preflight.json")
    assert set(json.loads((rdir / "02-register.json").read_text())["inputs"]) == {SC.PREV}
    assert {c["history"][-1]["at"] for c in WQ.load(root).values()} == {WQ.today()}
    for bad in ({"receipt_digest": "mtime"}, {"record_date": "2026-13-01"}, {"record_date": "20261001"},
                {"keep_verdicts": 1}):
        assert any(p.startswith("driver.") for p in WM.validate(manifest(driver=bad))), bad


def test_reader_reuse_keyed_on_code_and_verdict_per_run(tmp_path):
    """P9 OR section 3: a reader's or the bundle's output is reused only while the code its run bound (every *.py of
    the receipt) still hashes as bound; under research_cycle --keep-verdicts every verdict write leaves a per-run copy
    <cycle dir>/verdicts/<mode>-<k>.json that is never overwritten, and under driver.keep_verdicts the screen and the
    judge read and pin that copy, so a later run that rewrites cycle_verdict.json leaves their pins whole."""
    root = F.build(tmp_path / "w")
    fake = F.FakeCycle(root, ADMIT_ALL)
    assert research_wave.main(["run", F.MANIFEST, "--root", str(root)], executor=fake, log=lambda s: None) == 0
    w = Wave(F.MANIFEST, root, executor=fake, log=lambda s: None)
    navs = {"cell": "out/nav-w1-L1.1474", "parent": F.PARENT_NAV}
    assert WSC.read_once(w, "book", "book", navs)["kind"] == "book"          # reused: the code is unchanged
    p = root / "out/waves/w1/readers/book-run1/receipt.json"
    doc = json.loads(p.read_text())
    code = [b for b in doc["bindings"] if b["path"].endswith("wave_readers.py")]
    assert code and code[0]["sha256"] == RC.sha256_file(Path(code[0]["path"]))
    p.write_text(json.dumps(dict(doc, bindings=[dict(b, sha256="0" * 64) if b in code else b
                                                for b in doc["bindings"]])))   # the reader's code moved since
    with pytest.raises(WU.StageError, match=r"readers/book.json: its code .*wave_readers.py changed since the run "
                                            r"out/waves/w1/readers/book-run1 wrote it: never reused") as e:
        WSC.read_once(w, "book", "book", navs)
    assert e.value.code == WU.EXIT_PIN
    b = root / "out/waves/w1/bundle-run1/receipt.json"
    doc = json.loads(b.read_text())
    b.write_text(json.dumps(dict(doc, bindings=[dict(x, sha256="0" * 64) if x["path"].endswith("nav_summ.py") else x
                                                for x in doc["bindings"]])))
    with pytest.raises(WU.StageError, match=r"bundle.json: its code .*nav_summ.py changed since the run"):
        WR.bundle_once(w, F.PARENT_NAV, "out/nav-w1-L1.1474")
    # research_cycle --keep-verdicts: a per-run copy, never overwritten
    croot, sp = T.make_root(tmp_path / "c")
    assert T.run(croot, sp) == RC.EXIT_OK and not (croot / "build-equity/cycle-synthetic/verdicts").exists()
    log: list[str] = []
    assert T.run(croot, sp, log, keep_verdicts=True) == RC.EXIT_OK
    vdir = croot / "build-equity/cycle-synthetic"
    first = (vdir / "verdicts/run-1.json").read_bytes()
    assert first == (vdir / "cycle_verdict.json").read_bytes()
    assert "== verdict build-equity/cycle-synthetic/cycle_verdict.json (kept as build-equity/cycle-synthetic/verdicts/" \
           "run-1.json)" in log
    assert T.run(croot, sp, keep_verdicts=True) == RC.EXIT_OK
    assert (vdir / "verdicts/run-1.json").read_bytes() == first and (vdir / "verdicts/run-2.json").is_file()
    # the wave: driver.keep_verdicts reaches research_cycle runs; the screen and the judge pin the copies
    root = F.build(tmp_path / "k", driver={"keep_verdicts": True})
    fake = F.FakeCycle(root, ADMIT_ALL)
    assert research_wave.main(["run", F.MANIFEST, "--root", str(root)], executor=fake, log=lambda s: None) == 0
    assert all("--keep-verdicts" in a for a in research_cycle_calls(fake, "run"))
    rdir = root / "out/waves/w1/receipts"
    screen = json.loads((rdir / "03-screen.json").read_text())["outputs"]
    judge = json.loads((rdir / "08-judge.json").read_text())["outputs"]
    assert screen["verdict"] == "build-equity/cycle-w1/verdicts/screen-1.json"
    assert judge["cycle_verdict"] == "build-equity/cycle-w1/verdicts/run-1.json"
    cyc_dir = root / "build-equity/cycle-w1"
    (cyc_dir / "cycle_verdict.json").write_text("{}")                         # a later run rewrites the latest
    w = Wave(F.MANIFEST, root, executor=fake, log=lambda s: None)
    assert [r["state"] for r in research_wave.chain_of(w).state(w)] == ["done"] * 9   # the pins hold
    root = F.build(tmp_path / "n")                                            # no key: cycle_verdict.json, as before
    assert research_wave.main(["run", F.MANIFEST, "--root", str(root)], executor=F.FakeCycle(root, ADMIT_ALL),
                              log=lambda s: None) == 0
    judge = json.loads((root / "out/waves/w1/receipts/08-judge.json").read_text())["outputs"]
    assert judge["cycle_verdict"] == "build-equity/cycle-w1/cycle_verdict.json"
