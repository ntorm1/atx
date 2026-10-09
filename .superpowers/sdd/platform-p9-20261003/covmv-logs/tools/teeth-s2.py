"""Lane COV-MV fix round 1 (R10): the S-2 teeth build p9-covmv-j under Ruling TEETH-RESTORE.

The mutation puts back the pre-round behaviour (a failed eigen print stops the decision) in
atx-impl/src/strategy_mv_aim.cpp; MvAimRule.PrintFailureLeavesTheBookAndWritesNull must then fail.

The tool keeps the pre-mutation bytes itself (in memory and in a backup copy beside its log),
waits for an idle host (HOST-4: no ninja / clang-cl / lld-link / cmake --build / research-build
process, then a 90 s grace window, re-checked), mutates, runs scripts/research-build.ps1 once,
and restores the bytes in a `finally` block of the same run. After the restore it asserts that
`git status --porcelain` is empty, that `git hash-object <path>` equals `git rev-parse
HEAD:<path>`, and that the raw SHA-256 equals the saved one. A failed restore stops the tool
(exit 2) before any test runs. An admission refusal ("memory admission", no receipt) is restored
and retried after 60 s. Never `git checkout` / `restore` / `stash`.

Usage: teeth-s2.py [bound-minutes]   (default 180, HOST-4)
"""

import datetime
import hashlib
import pathlib
import subprocess
import sys
import time

POOL = pathlib.Path("C:/atx-wt/pool-31")
REL = "atx-impl/src/strategy_mv_aim.cpp"
SRC = POOL / REL
LOGS = POOL / ".superpowers" / "sdd" / "platform-p9-20261003" / "covmv-logs"
LOG = LOGS / "j-teeth.log"
BACKUP = LOGS / "j-teeth-strategy_mv_aim.cpp.orig"
TAG = "p9-covmv-j"
TARGETS = "atx-impl-strategy-target-tests"
OLD = b"  if (auto printed = add_eigen_shares(); !printed) note_unavailable(printed.error());"
NEW = b"  if (auto printed = add_eigen_shares(); !printed) return printed;"
# Ruling HOST-5: only pool builds count (a build tool that, or whose build-tool ancestor, names
# C:\atx-wt\pool-*; or a research-build.ps1 process); a foreign build is ignored, never touched.
# The same test as covmv-logs/tools/wait-build.ps1 Test-PoolBuild.
BUSY_PS = (
    "$self = $PID; $all = @(Get-CimInstance Win32_Process); $byId = @{}; "
    "foreach ($p in $all) { $byId[[int]$p.ProcessId] = $p }; "
    "function Test-PoolBuild($proc) { $p = $proc; "
    "for ($i = 0; $i -lt 12 -and $null -ne $p; $i++) { "
    "if ($p.Name -notmatch '^(ninja|clang-cl|lld-link|cl|link|ccache|cmake)\\.exe$') "
    "{ return $false }; "
    "if ([string]$p.CommandLine -match 'atx-wt[\\\\/]pool-') { return $true }; "
    "$p = $byId[[int]$p.ParentProcessId] }; return $false }; "
    "@($all | Where-Object { ((($_.Name -match '^(ninja|clang-cl|lld-link|cl)\\.exe$') -or "
    "($_.Name -eq 'cmake.exe' -and $_.CommandLine -match '--build')) -and (Test-PoolBuild $_)) "
    "-or ($_.Name -eq 'powershell.exe' -and $_.ProcessId -ne $self -and "
    "$_.CommandLine -match 'research-build') }) | ForEach-Object { $_.Name }; "
    "'free=' + [int][math]::Floor((Get-CimInstance Win32_OperatingSystem).FreePhysicalMemory / 1024)"
)


def log(line: str) -> None:
    stamp = datetime.datetime.now().isoformat(timespec="seconds")
    with open(LOG, "a", encoding="utf-8") as f:
        f.write(f"{stamp} {line}\n")
    print(f"{stamp} {line}", flush=True)


def git(*args: str) -> str:
    out = subprocess.run(["git", "-C", str(POOL), *args], capture_output=True, text=True,
                         check=True)
    return out.stdout.strip()


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def tree_state() -> tuple[str, str, str, str]:
    return (git("status", "--porcelain"), git("hash-object", REL), git("rev-parse", f"HEAD:{REL}"),
            sha256(SRC.read_bytes()))


def host() -> tuple[list[str], int]:
    out = subprocess.run(["powershell", "-NoProfile", "-Command", BUSY_PS], capture_output=True,
                         text=True, check=True).stdout.split()
    free = int(out[-1].split("=")[1])
    return out[:-1], free


def idle_after_grace() -> bool:
    busy, free = host()
    if busy or free < 1000:
        log(f"wait busy={busy} free={free}")
        return False
    log(f"idle free={free}; grace 90 s")
    time.sleep(90)
    busy, free = host()
    if busy or free < 1000:
        log(f"grace broken busy={busy} free={free}")
        return False
    log(f"launch {TAG} free={free}")
    return True


def restore(orig: bytes, orig_sha: str, head_blob: str) -> bool:
    SRC.write_bytes(orig)
    porcelain, blob, head, raw = tree_state()
    good = porcelain == "" and blob == head == head_blob and raw == orig_sha
    log(f"restored: porcelain_empty={porcelain == ''} hash-object={blob} HEAD={head} "
        f"sha256={raw} equal_saved={raw == orig_sha} ok={good}")
    return good


def main() -> int:
    bound = int(sys.argv[1]) if len(sys.argv) > 1 else 180
    porcelain, blob, head, raw = tree_state()
    log(f"start HEAD={git('rev-parse', 'HEAD')} porcelain_empty={porcelain == ''} "
        f"hash-object={blob} HEAD:{REL}={head} sha256={raw}")
    if porcelain != "" or blob != head:
        log("REFUSED: the tree is not clean at HEAD")
        return 3
    orig = SRC.read_bytes()
    if orig.count(OLD) != 1:
        log(f"REFUSED: the mutation line occurs {orig.count(OLD)} times")
        return 3
    BACKUP.write_bytes(orig)
    orig_sha = sha256(orig)
    mutated = orig.replace(OLD, NEW, 1)
    deadline = time.monotonic() + bound * 60
    while time.monotonic() < deadline:
        if not idle_after_grace():
            time.sleep(60)
            continue
        SRC.write_bytes(mutated)
        log(f"mutated {REL}: sha256={sha256(mutated)} (one line: note_unavailable -> return)")
        code = None
        out = ""
        try:
            proc = subprocess.run(
                ["powershell", "-NoProfile", "-File", "scripts\\research-build.ps1", "-Tag", TAG,
                 "-Targets", TARGETS, "-Preset", "equity-dev"],
                capture_output=True, text=True, cwd=str(POOL), check=False,
                env={**__import__("os").environ, "PYTHONDONTWRITEBYTECODE": "1"})
            code = proc.returncode
            out = proc.stdout + proc.stderr
        finally:
            ok = restore(orig, orig_sha, head)
        with open(LOGS / f"j-teeth-wrapper-{TAG}.log", "a", encoding="utf-8") as f:
            f.write(out)
        if not ok:
            log("STOP: restore failed; commit nothing; report the exact state")
            return 2
        if "memory admission" in out:
            log("admission refused; restored; retry in 60 s")
            time.sleep(60)
            continue
        log(f"build {TAG} exit={code}")
        return 0 if code == 0 else 1
    log(f"BLOCKED-HOST: no admission within {bound} minutes; tree restored")
    return 99


if __name__ == "__main__":
    sys.exit(main())
