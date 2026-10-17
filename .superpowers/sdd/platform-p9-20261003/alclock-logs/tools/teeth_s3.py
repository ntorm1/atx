"""AL-CLOCK fix round 1 teeth for review S-3 (ruling TEETH-RESTORE; HOST-4 / HOST-5): ONE Debug build of
atx-impl-strategy-target-tests with one mutation of atx-impl/src/book/nav_session.cpp, restored by this tool.

The mutation: the restored (calibrated) pass stops forming each decision's info-clock rates,
    if (clock_rule)                                  ->  if (clock_rule && !(tape && tape->replay))
before nav_info_clock_rates, so pass 2 of --calibrate-gross keeps the construction's initial vector (every name at
the slow rate). Expected on the teeth exe: InfoClockRate.CalibrateGrossKeepsTheRatesAndCountsOnce FAILS, every other
InfoClock* test passes (no other test runs the rule through the calibrate pass).

Steps: (1) assert `git status --porcelain` is empty and the file's `git hash-object` equals HEAD's blob; keep the
pre-mutation bytes in memory and as alclock-logs/teeth-s3-<TAG>.orig (git-ignored); (2) one CRLF-aware replacement,
asserted unique; (3) wait for no pool build (tools/pool-builds.ps1, 60 s quiet, research-build's memory thresholds;
bound 180 min) and run scripts/research-build.ps1 once (a memory-admission refusal is waited out inside the bound);
(4) in `finally`, BEFORE any test, write the kept bytes back and assert raw SHA-256 = the pre-mutation SHA-256,
hash-object = HEAD's blob and porcelain empty. Never git checkout / restore / stash. A failed restore exits 3: stop,
commit nothing, report the state.

usage (from the pool root): python teeth_s3.py TAG          exit: the build's code, 3 restore failure, 75 gate timeout
"""
import hashlib
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path("C:/atx-wt/pool-30")
LOGS = ROOT / ".superpowers" / "sdd" / "platform-p9-20261003" / "alclock-logs"
PROBE = LOGS / "tools" / "pool-builds.ps1"
TARGET = "atx-impl/src/book/nav_session.cpp"
OLD = "        if (clock_rule){eol}          ATX_TRY_VOID(nav_info_clock_rates("
NEW = "        if (clock_rule && !(tape && tape->replay)){eol}          ATX_TRY_VOID(nav_info_clock_rates("
BOUND = 10800


def git(*args: str) -> str:
    return subprocess.run(["git", "-C", str(ROOT), *args], check=True, capture_output=True, text=True).stdout


def porcelain() -> str:
    return git("status", "--porcelain")


def head_blob() -> str:
    return git("rev-parse", f"HEAD:{TARGET}").strip()


def worktree_blob() -> str:
    return git("hash-object", TARGET).strip()


def log(line: str) -> None:
    stamp = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
    with open(LOGS / "teeth-s3.log", "a", encoding="utf-8") as f:
        f.write(f"{stamp} {line}\n")
    print(line, flush=True)


def probe() -> tuple[int, int, int, str]:
    out = subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(PROBE)],
                         check=True, capture_output=True, text=True).stdout.split()
    return int(out[0]), int(out[1]), int(out[2]), out[3]


def wait_quiet(t0: float) -> bool:
    quiet = 0
    while time.time() - t0 < BOUND:
        busy, free, commit, who = probe()
        if busy == 0 and free >= 1000 and commit >= 2500:
            quiet += 1
            if quiet >= 2:
                log(f"quiet: free {free} commit {commit}")
                return True
        else:
            quiet = 0
            log(f"waiting: pool build procs {busy} ({who}) free {free} commit {commit} ({int(time.time() - t0)} s)")
        time.sleep(30)
    return False


def build(tag: str) -> int:
    t0 = time.time()
    out = LOGS / f"research-build-{tag}.out"
    while True:
        if not wait_quiet(t0):
            log(f"{tag} GATE TIMEOUT after {int(time.time() - t0)} s")
            return 75
        log(f"{tag} start (head {git('rev-parse', '--short', 'HEAD').strip()}, mutated {TARGET})")
        with open(out, "w", encoding="utf-8") as f:
            rc = subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
                                 str(ROOT / "scripts" / "research-build.ps1"), "-Tag", tag, "-Preset", "equity-dev",
                                 "-Targets", "atx-impl-strategy-target-tests"],
                                cwd=ROOT, stdout=f, stderr=subprocess.STDOUT).returncode
        if rc != 0 and "memory admission" in out.read_text(encoding="utf-8", errors="replace"):
            log(f"{tag} research-build memory admission refused; waiting")
            time.sleep(60)
            continue
        log(f"{tag} end exit {rc} wall {int(time.time() - t0)} s")
        return rc


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    tag = sys.argv[1]
    path = ROOT / TARGET
    if porcelain():
        log("refused: the working tree is not clean before the mutation")
        return 2
    blob = head_blob()
    if worktree_blob() != blob:
        log("refused: the file differs from HEAD's blob before the mutation")
        return 2
    kept = path.read_bytes()
    sha_before = hashlib.sha256(kept).hexdigest()
    shutil.copyfile(path, LOGS / f"teeth-s3-{tag}.orig")
    eol = "\r\n" if b"\r\n" in kept else "\n"
    text = kept.decode("utf-8")
    old, new = OLD.format(eol=eol), NEW.format(eol=eol)
    if text.count(old) != 1:
        log(f"refused: the mutation text occurs {text.count(old)} times")
        return 2
    rc = 3
    try:
        path.write_bytes(text.replace(old, new).encode("utf-8"))
        log(f"mutated {TARGET}: 'if (clock_rule)' -> 'if (clock_rule && !(tape && tape->replay))'; "
            f"porcelain {porcelain().strip()!r}")
        rc = build(tag)
    finally:
        path.write_bytes(kept)
        sha_after = hashlib.sha256(path.read_bytes()).hexdigest()
        restored = sha_after == sha_before and worktree_blob() == blob and porcelain() == ""
        log(f"restored: sha256 {sha_after[:16]} (before {sha_before[:16]}), hash-object = HEAD blob "
            f"{worktree_blob() == blob} ({blob[:16]}), porcelain empty {porcelain() == ''} -> "
            f"{'OK' if restored else 'RESTORE FAILED'}")
        if not restored:
            rc = 3
    bdir = ROOT / "build-equity"
    for suffix in ("receipt.json", "build.log", "cache-stats.log"):
        src = bdir / f"mega-{tag}-{suffix}"
        if src.exists():
            shutil.copyfile(src, LOGS / src.name)
    return rc


if __name__ == "__main__":
    raise SystemExit(main())
