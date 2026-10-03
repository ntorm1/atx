#!/usr/bin/env python3
"""Run one owned research process with time/RAM limits and an immutable receipt.

Arguments after -- are passed directly to the process, never through a shell.
Outputs must be new directories inside the root (--root, default this worktree),
where the process runs. This requires psutil and does not authorize concurrent
compilation or turn a smoke run into alpha evidence.
This is a sampled operational guard, not a process sandbox: supported research
commands must not detach children between samples. Observed descendants remain
owned and monitored after the direct child exits.

The source must be committed: the root's `git status` restricted to the code
pathspec (research_tree.CODE_PATHSPEC) must be empty; dirty paths outside it
(lane reports, research outputs) are listed in the receipt, not refused.
--no-git (contract K3) skips git for a root outside any repository (test roots
such as the tiny_world fixture) and is refused for a root inside one.
--role-id ID (platform v8 H-1) names the era role of the run in start.json and
receipt.json (written only when given).
Contract K-P9-10: every receipt carries argv_sha256 (research_tree.argv_sha256 of
the command after its executable), attempt (--attempt K, default 1),
executable_sha256 and build_type (--build-type Debug | Release, else null);
research_cycle refuses to resume an output whose receipt names another argv.

Launch admission (P9 F-5 (a); each flag absent = no wait, as before):
--admission-wait-seconds S waits, at most S seconds, before the launch until the
host's free memory covers the declared peak plus the floor (--max-rss-mib +
--min-free-mib) and no compiler or linker runs (COMPILERS); --host-budget-mib N
(with S) also waits until the declared caps of every live bounded process that
holds a claim under --host-claims DIR (default: the host's temp dir, so every
worktree of the host shares it) plus this one fit in N MiB, and checks free
memory inside the claims lock net of what the other claims have yet to allocate
(their caps less their trees' RSS now), so launches started at the same instant
never both count the same free memory; it holds its claim until its process
tree (the child included, should the runner die) has ended: the host memory
semaphore that lets research steps run side by side. N bounds the sum of the
declared caps; the free-memory check, not N, keeps them within the host's
memory. A wait that times out writes outcome
prelaunch-admission-timeout (nothing ran). The receipt's "admission" block (only
with the flags) records the wait.
"""
from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import math
from pathlib import Path
import re
import os
import shutil
import subprocess
import sys
import tempfile
import time

import psutil

sys.path.insert(0, str(Path(__file__).resolve().parent))
import research_tree  # noqa: E402

BUILD_TYPES = ("Debug", "Release")
# P9 F-5 (a): a launch waits while one of these runs (a build competes for the host's memory)
COMPILERS = frozenset(n + x for n in ("cl", "clang-cl", "ninja", "lld-link") for x in ("", ".exe"))
POLL_SECONDS = 0.5                     # how often a waiting launch re-checks the host
CLAIMS_DIR = Path(tempfile.gettempdir()) / "atx-host-claims"   # --host-claims default: one per host
CLAIM_SUFFIX, CLAIMS_LOCK = ".claim", "claims.lock"
REPLACE_RETRY_SECONDS = 2.0            # a claim's os.replace retried while another handle holds the file (Windows)
ADOPT_LOCK_SECONDS = 5.0               # how long adopt waits for the claims lock before writing without it
OUTCOME_ADMISSION = "prelaunch-admission-timeout"


class AdmissionTimeout(RuntimeError):
    """The launch admission's wait ran out (the receipt's outcome OUTCOME_ADMISSION; nothing was launched)."""

    def __init__(self, message: str, block: dict):
        super().__init__(message)
        self.block = block


def digest(path: Path) -> str:
    result = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(chunk)
    return result.hexdigest()


def live_owned(owned: dict) -> list[psutil.Process]:
    for process in list(owned.values()):
        try:
            if process.is_running():
                for child in process.children(recursive=True):
                    owned[(child.pid, child.create_time())] = child
        except psutil.NoSuchProcess:
            pass
    return [process for process in owned.values() if process.is_running()]


def stop_owned(owned: dict) -> None:
    """psutil checks creation time before signaling, protecting against PID reuse."""
    live = live_owned(owned)
    for child in reversed(live):
        try:
            child.terminate()
        except psutil.NoSuchProcess:
            pass
    _, alive = psutil.wait_procs(live, timeout=2)
    for child in alive:
        try:
            child.kill()
        except psutil.NoSuchProcess:
            pass
    psutil.wait_procs(alive, timeout=2)


# ------------------------------------------------------------------ launch admission (P9 F-5 (a), OR section 5)
def is_alive(pid: int, born: float) -> bool:
    """Whether the process ``pid`` created at ``born`` still runs (a reused pid is not it); unknowable = alive."""
    try:
        return abs(psutil.Process(pid).create_time() - born) < 0.01
    except psutil.NoSuchProcess:
        return False
    except (psutil.AccessDenied, OSError, ValueError):
        return True


def compilers_running() -> list[str]:
    """The names of the COMPILERS processes running now (sorted, each once)."""
    found = set()
    for proc in psutil.process_iter(["name"]):
        name = (proc.info.get("name") or "").lower()
        if name in COMPILERS:
            found.add(name)
    return sorted(found)


def remove(path: Path) -> None:
    """Remove a claim or lock file; a sharing violation (another process reading it) is retried, then left."""
    for _ in range(20):
        try:
            path.unlink(missing_ok=True)
            return
        except PermissionError:
            time.sleep(0.05)
    print(f"run_bounded_research: warning: could not remove {path}", file=sys.stderr)


def tree_rss_mib(doc: dict) -> int:
    """The memory a claim's processes hold now (MiB): the RSS of the claiming runner's descendants and of its adopted
    child's tree (the research process, which may outlive a hard-killed runner); the runner itself is not counted
    (its cap covers the child tree only). Unreadable processes count 0 (the claim then reserves more, never less)."""
    seen, total = set(), 0
    for key, tree in (("pid", False), ("child_pid", True)):
        try:
            pid, born = int(doc[key]), float(doc[key.replace("pid", "create_time")])
            proc = psutil.Process(pid)
            if abs(proc.create_time() - born) >= 0.01:
                continue
            procs = ([proc] if tree else []) + proc.children(recursive=True)
        except (KeyError, TypeError, ValueError, psutil.Error, OSError):
            continue
        for q in procs:
            if q.pid in seen:
                continue
            seen.add(q.pid)
            try:
                total += q.memory_info().rss
            except (psutil.Error, OSError):
                pass
    return total >> 20


class HostClaims:
    """The host memory semaphore (--host-budget-mib): ``<dir>/<pid>-<ms>.claim`` = {pid, create_time, mib, output,
    child_pid?, child_create_time?} per admitted bounded process (its declared cap, --max-rss-mib; the child once
    launched), written under ``<dir>/claims.lock`` (O_CREAT | O_EXCL) and removed when the process tree has ended.
    Admission checks free memory inside that lock, net of what the other claims have yet to allocate (try_claim), so
    two runners started at the same instant never both count the same free memory. A claim whose runner and child are
    both gone (killed) is stale: dropped and removed by the next check. ``owner`` = (pid, create_time) of the claiming
    process (default this one)."""

    def __init__(self, directory: Path, budget_mib: int, owner: tuple[int, float] | None = None):
        self.dir, self.budget = Path(directory), budget_mib
        if owner is None:
            me = psutil.Process()
            owner = (me.pid, me.create_time())
        self.owner = owner
        self.mine: Path | None = None
        self.doc: dict | None = None

    def live(self) -> list[dict]:
        out = []
        for p in sorted(self.dir.glob("*" + CLAIM_SUFFIX)):
            try:
                doc = json.loads(p.read_text(encoding="utf-8"))
                running = is_alive(int(doc["pid"]), float(doc["create_time"])) or (
                    "child_pid" in doc and is_alive(int(doc["child_pid"]), float(doc["child_create_time"])))
                doc["mib"] = int(doc["mib"])
            except (OSError, ValueError, KeyError, TypeError):
                continue                                   # claims are written whole (os.replace): unreadable = gone
            if running:
                out.append(doc)
            else:
                remove(p)
        return out

    def acquire_lock(self, seconds: float = 30.0) -> Path:
        self.dir.mkdir(parents=True, exist_ok=True)
        lock, end = self.dir / CLAIMS_LOCK, time.monotonic() + seconds
        while True:
            try:
                fd = os.open(lock, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
                break
            except FileExistsError:
                try:
                    held = json.loads(lock.read_text(encoding="utf-8"))
                    if not is_alive(int(held["pid"]), float(held["create_time"])):
                        remove(lock)                       # a killed runner's lock
                        continue
                except (OSError, ValueError, KeyError, TypeError):
                    pass                                   # being written: held
                if time.monotonic() > end:
                    raise RuntimeError(f"host claims lock {lock} held for {seconds} s") from None
                time.sleep(0.05)
        with os.fdopen(fd, "w", encoding="utf-8") as f:
            f.write(json.dumps({"pid": self.owner[0], "create_time": self.owner[1]}) + "\n")
        return lock

    def try_claim(self, mib: int, need: int, output: str, available) -> dict:
        """Under the claims lock: claim ``mib`` when (a) the live claims plus ``mib`` fit the budget and (b) free memory
        (``available()`` bytes, read inside the lock) minus what the other claims have yet to allocate (each claim's
        mib less its processes' RSS now, tree_rss_mib, at least 0) covers ``need`` (this process's peak + floor).
        Returns {claimed, free_mib, claimed_by_others_mib, reserved_by_others_mib}."""
        lock = self.acquire_lock()
        try:
            live = self.live()
            free = available() >> 20
            others = sum(d["mib"] for d in live)
            reserved = sum(max(0, d["mib"] - tree_rss_mib(d)) for d in live)
            out = {"claimed": others + mib <= self.budget and free - reserved >= need, "free_mib": free,
                   "claimed_by_others_mib": others, "reserved_by_others_mib": reserved}
            if out["claimed"]:
                pid, born = self.owner
                self.mine = self.dir / f"{pid}-{int(born * 1000)}{CLAIM_SUFFIX}"
                self.doc = {"pid": pid, "create_time": born, "mib": mib, "output": output}
                self.write()
            return out
        finally:
            remove(lock)

    def write(self, doc: dict | None = None) -> None:
        """Write the claim whole (a temp file, then os.replace). On Windows a replace onto a file another process has
        open (a reader, an indexer) fails with PermissionError: retried for about REPLACE_RETRY_SECONDS, then raised."""
        doc = self.doc if doc is None else doc
        tmp = self.mine.with_name(self.mine.name + ".tmp")
        tmp.write_text(json.dumps(doc) + "\n", encoding="utf-8")
        end = time.monotonic() + REPLACE_RETRY_SECONDS
        while True:
            try:
                os.replace(tmp, self.mine)
                return
            except PermissionError:
                if time.monotonic() >= end:
                    remove(tmp)
                    raise
                time.sleep(0.05)

    def adopt(self, child: psutil.Process) -> None:
        """Record the launched child in this process's claim: the claim lives while the child does (a hard-killed
        runner's orphaned child keeps its share) and its RSS counts against the claim's reservation. Written under
        the claims lock (the other runners read claims only under it) with a retried replace; any failure is a
        warning that keeps the runner-pid claim and never stops the child just launched (re-review 1 N2)."""
        if self.mine is None or self.doc is None:
            return
        lock = None
        try:
            doc = dict(self.doc, child_pid=child.pid, child_create_time=child.create_time())
            try:
                lock = self.acquire_lock(ADOPT_LOCK_SECONDS)
            except (OSError, RuntimeError):
                lock = None                     # a busy lock: the retried replace still writes the claim whole
            self.write(doc)
            self.doc = doc
        except Exception as exc:                # noqa: BLE001 (the claim stays the runner's; the child runs on)
            print(f"run_bounded_research: warning: claim {self.mine} keeps the runner pid only ({exc})",
                  file=sys.stderr)
        finally:
            if lock is not None:
                remove(lock)

    def release(self) -> None:
        if self.mine is not None:
            remove(self.mine)
            self.mine = None


def admit(args, output: Path, *, available=lambda: psutil.virtual_memory().available, compilers=compilers_running,
          clock=time.monotonic, sleep=time.sleep, owner: tuple[int, float] | None = None
          ) -> tuple[dict, HostClaims | None]:
    """Wait, at most args.admission_wait_seconds, until the launch may start: free memory >= the declared peak + the
    floor (need = max_rss_mib + min_free_mib) and no COMPILERS process; with args.host_budget_mib the free check runs
    inside the claims lock, net of the memory the other live claims have yet to allocate, together with a claim of
    max_rss_mib under the host budget (HostClaims.try_claim: two simultaneous launches never both count the same free
    memory). Returns (the receipt's admission block, the claims holding this process's claim, or None); raises
    AdmissionTimeout (its ``block``) when the wait runs out. ``owner``: the claiming process (default this one)."""
    need = args.max_rss_mib + args.min_free_mib
    claims = HostClaims(args.host_claims, args.host_budget_mib, owner) if args.host_budget_mib else None
    block = {"wait_seconds": args.admission_wait_seconds, "need_free_mib": need, "host_budget_mib": args.host_budget_mib,
             "claims_dir": str(claims.dir) if claims else None}
    start, checks = clock(), 0
    while True:
        checks += 1
        others, why = None, []
        if claims is None:
            free, busy = available() >> 20, compilers()
            if free < need:
                why.append(f"free memory {free} MiB < {need} MiB (peak {args.max_rss_mib} + floor {args.min_free_mib})")
            if busy:
                why.append(f"running: {', '.join(busy)}")
            block.update(waited_seconds=round(clock() - start, 3), checks=checks, free_mib=free,
                         claimed_by_others_mib=others)
        else:
            busy = compilers()
            if busy:
                why.append(f"running: {', '.join(busy)}")
            got = claims.try_claim(args.max_rss_mib, need, str(output), available) if not busy else \
                {"claimed": False, "free_mib": available() >> 20, "claimed_by_others_mib": None,
                 "reserved_by_others_mib": None}
            others, reserved = got["claimed_by_others_mib"], got["reserved_by_others_mib"]
            if not busy and others + args.max_rss_mib > args.host_budget_mib:
                why.append(f"claimed {others} MiB + {args.max_rss_mib} MiB > host budget {args.host_budget_mib} MiB")
            if not busy and got["free_mib"] - reserved < need:
                why.append(f"free memory {got['free_mib']} MiB less {reserved} MiB the other claims have yet to "
                           f"allocate < {need} MiB (peak {args.max_rss_mib} + floor {args.min_free_mib})")
            block.update(waited_seconds=round(clock() - start, 3), checks=checks, free_mib=got["free_mib"],
                         claimed_by_others_mib=others, reserved_by_others_mib=reserved)
        if not why:
            return block, claims
        if clock() - start >= args.admission_wait_seconds:
            block["refused"] = why
            raise AdmissionTimeout(f"launch admission: still waiting after {args.admission_wait_seconds} s: "
                                   + "; ".join(why), block)
        sleep(POLL_SECONDS)


def admission_refusal(args) -> str | None:
    """Why the admission flags are refused (None: accepted, or not given): research_tree.launch_refusal, and a host
    budget below this process's own cap would never admit it."""
    refusal = research_tree.launch_refusal(args.admission_wait_seconds, args.host_budget_mib)
    if refusal:
        return f"--admission-wait-seconds / --host-budget-mib: {refusal}"
    if args.host_budget_mib is not None and args.host_budget_mib < args.max_rss_mib:
        return f"--host-budget-mib {args.host_budget_mib} is below --max-rss-mib {args.max_rss_mib}: never admitted"
    return None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--seconds", type=float, default=180)
    parser.add_argument("--max-rss-mib", type=int, default=1024)
    parser.add_argument("--min-free-mib", type=int, default=256)
    parser.add_argument("--bind", type=Path, action="append", default=[])
    parser.add_argument("--root", type=Path, default=None,
                        help="where the process runs and outputs go (default: this worktree)")
    parser.add_argument("--no-git", action="store_true",
                        help="no source pin: only for a --root outside any git repository (contract K3)")
    parser.add_argument("--role-id", default=None,
                        help="the era role of this run (v8 H-1): recorded in start.json and receipt.json")
    parser.add_argument("--attempt", type=int, default=1,
                        help="the attempt this run dir is (K-P9-10; research_cycle's attempt-k sub-dirs): recorded")
    parser.add_argument("--build-type", default=None,
                        help="the CMake build type of the executables (Debug | Release; K-P9-10): recorded")
    parser.add_argument("--admission-wait-seconds", type=float, default=None,
                        help="P9 F-5 (a): wait at most this long for free memory >= peak + floor and no compiler")
    parser.add_argument("--host-budget-mib", type=int, default=None,
                        help="P9 OR section 5: the host memory semaphore's budget (needs --admission-wait-seconds)")
    parser.add_argument("--host-claims", type=Path, default=CLAIMS_DIR,
                        help=f"the semaphore's claims dir (default {CLAIMS_DIR})")
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command[1:] if args.command[:1] == ["--"] else args.command
    if (not command or not math.isfinite(args.seconds) or not 0 < args.seconds <= research_tree.RUNNER_MAX_SECONDS
            or not 32 <= args.max_rss_mib <= 8192 or not 64 <= args.min_free_mib <= 8192):
        parser.error(f"require a command, <={research_tree.RUNNER_MAX_SECONDS} seconds, and explicit bounded RAM "
                     "limits")
    if args.role_id is not None and not re.fullmatch(r"[A-Za-z0-9_]+", args.role_id):
        parser.error("--role-id must match [A-Za-z0-9_]+")
    if not 1 <= args.attempt <= 99 or (args.build_type is not None and args.build_type not in BUILD_TYPES):
        parser.error(f"--attempt is 1..99 and --build-type one of {', '.join(BUILD_TYPES)}")
    refusal = admission_refusal(args)
    if refusal:
        parser.error(refusal)
    root = (args.root or Path(__file__).resolve().parents[1]).resolve()
    if args.no_git and research_tree.no_git_refusal(root):
        parser.error(research_tree.no_git_refusal(root))
    output = args.output.resolve()
    if not output.is_relative_to(root) or output == root:
        parser.error("output must be a new directory inside the root")
    executable = shutil.which(command[0])
    if executable is None:
        parser.error("executable was not found")
    command[0] = str(Path(executable).resolve())
    bindings = []
    for path in args.bind:
        path = path.resolve(strict=True)
        if not path.is_file() or path.stat().st_size > 16 * 1024 * 1024:
            parser.error("bind small config/source manifests, not full data payloads")
        bindings.append({"path": str(path), "sha256": digest(path)})
    source, ignored = None, []
    if not args.no_git:
        source = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root, text=True).strip()
        blocking, ignored = research_tree.dirty_paths(root)
        if blocking:
            parser.error("commit the source before running a recorded research experiment (dirty in the code "
                         f"pathspec: {', '.join(blocking[:8])})")
    output.mkdir(parents=True, exist_ok=False)
    limits = dict(seconds=args.seconds, max_rss_mib=args.max_rss_mib, min_free_mib=args.min_free_mib)
    for key in ("admission_wait_seconds", "host_budget_mib"):     # F-5 (a): recorded only when given
        if getattr(args, key) is not None:
            limits[key] = getattr(args, key)
    receipt = dict(schema="atx.bounded-research-run/v1", source_sha=source,
        started_utc=dt.datetime.now(dt.timezone.utc).isoformat(), command=command,
        executable_sha256=digest(Path(command[0])), argv_sha256=research_tree.argv_sha256(command[1:]),
        attempt=args.attempt, build_type=args.build_type, bindings=bindings,
        limits=limits, sampled_peak_tree_rss_bytes=0,
        minimum_system_free_bytes=psutil.virtual_memory().available,
        outcome="launch-failed", exit_code=None,
        git="none (--no-git: root outside any repository)" if args.no_git else "clean in the code pathspec",
        dirty_outside_pathspec=ignored)
    if args.role_id is not None:
        receipt["role_id"] = args.role_id
    (output / "start.json").write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
    started = time.monotonic()
    child = None
    owned = {}
    claims = None
    try:
        with (output / "stdout.log").open("xb") as stdout, (output / "stderr.log").open("xb") as stderr:
            if args.admission_wait_seconds is not None:
                try:
                    receipt["admission"], claims = admit(args, output)
                except AdmissionTimeout as exc:
                    receipt["outcome"], receipt["admission"] = OUTCOME_ADMISSION, exc.block
                    raise
                started = time.monotonic()         # wall_seconds: the process's own, not the wait's
            if psutil.virtual_memory().available < args.min_free_mib * 1024**2:
                receipt["outcome"] = "prelaunch-memory-refusal"
                raise RuntimeError("available system memory is below the launch floor")
            child = subprocess.Popen(command, cwd=root, stdout=stdout, stderr=stderr,
                creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
            process = psutil.Process(child.pid)
            owned[(process.pid, process.create_time())] = process
            if claims is not None:              # the claim lives (and its reservation shrinks) with the child
                claims.adopt(process)
            while True:
                tree = live_owned(owned)
                if child.poll() is not None and not tree:
                    break
                rss = 0
                for process in tree:
                    try:
                        rss += process.memory_info().rss
                    except psutil.NoSuchProcess:
                        pass
                free = psutil.virtual_memory().available
                receipt["sampled_peak_tree_rss_bytes"] = max(receipt["sampled_peak_tree_rss_bytes"], rss)
                receipt["minimum_system_free_bytes"] = min(receipt["minimum_system_free_bytes"], free)
                reason = ("time-limit" if time.monotonic() - started >= args.seconds else
                          "rss-limit" if rss > args.max_rss_mib * 1024**2 else
                          "system-memory-limit" if free < args.min_free_mib * 1024**2 else "")
                if reason:
                    receipt["outcome"] = reason
                    stop_owned(owned)
                    break
                time.sleep(0.25)
            receipt["exit_code"] = child.wait(timeout=5)
            if receipt["outcome"] == "launch-failed":
                receipt["outcome"] = "completed" if child.returncode == 0 else "process-error"
    except BaseException as exc:
        if receipt["outcome"] not in ("prelaunch-memory-refusal", OUTCOME_ADMISSION):
            receipt["outcome"] = "interrupted" if isinstance(exc, KeyboardInterrupt) else "runner-error"
        receipt["error"] = str(exc)
        if owned:
            stop_owned(owned)
        if child is not None:
            receipt["exit_code"] = child.wait(timeout=5)
    finally:
        if claims is not None:          # the host semaphore's claim ends with the process tree
            claims.release()
        receipt["wall_seconds"] = time.monotonic() - started
        receipt["owned_processes"] = [{"pid": pid, "create_time": born} for pid, born in owned]
        receipt["ownership_scope"] = "sampled descendant identities; commands must not detach unsampled children"
        receipt["logs"] = {name: digest(output / name) for name in ("stdout.log", "stderr.log")
                           if (output / name).exists()}
        with (output / "receipt.json").open("x", encoding="utf-8") as stream:
            json.dump(receipt, stream, indent=2)
            stream.write("\n")
    print(json.dumps(receipt, indent=2))
    return 0 if receipt["outcome"] == "completed" else 1


if __name__ == "__main__":
    sys.exit(main())
