"""SQL3 item 5 teeth (P9 RESUME 10; ruling TEETH-RESTORE): one mutation per hook call line in B2's three writers
(the call removed), plus M5 (the WIP's cwd-relative path form put back in write_binding). For each mutation the tool
keeps the file's bytes itself, writes the mutated bytes, runs scripts/tests/test_research_store_hook.py on the mutated
tree, restores the kept bytes in a finally block, then asserts the file's SHA-256 equals HEAD's blob and
`git status --porcelain` is empty (also checked before each mutation). Never git checkout / restore / stash.

Usage: python r10-teeth-item5.py REPO BASETEMP_PARENT LOG
       (env ATX_RESEARCH_STORE_EXE: the store exe the real-exe case copies; read-only)
Exit 0: every mutation red with its expected cases; 1: a mutation not caught; 2: STOP (a precondition or a restore
check failed; the state is printed; nothing may be committed or built on this tree).
"""
from __future__ import annotations

import hashlib
import os
from pathlib import Path
import subprocess
import sys

HOOK_TESTS = "scripts/tests/test_research_store_hook.py"
T = HOOK_TESTS + "::"
BINDING = '    research_store_hook.record(cycle.res.root, f"{st.run_dir}/{BINDING}", "cycle-binding")   # P9 SQL3 item 5'
COPY = '            research_store_hook.record(cycle.res.root, rel, "cycle-verdict")       # P9 SQL3 item 5'
VERDICT = ('    research_store_hook.record(cycle.res.root, f"{cycle.cycle_dir()}/{VERDICT}", "cycle-verdict")   '
           '# P9 SQL3 item 5')
RESULT = '    research_store_hook.record(w.root, w.path(path), "wave-result")     # P9 SQL3 item 5'
WIP_BINDING = '    research_store_hook.record(cycle.res.root, cycle.res.path(f"{st.run_dir}/{BINDING}"), "cycle-binding")'
REAL = T + "test_real_store_exe_ingests_every_class"
# (id, file, the exact line, its replacement (None: the line removed), the cases that must go red)
MUTATIONS = [
    ("M1", "scripts/cycle_resume.py", BINDING, None,
     [T + "test_write_binding_ingests_its_binding[absolute-root]",
      T + "test_write_binding_ingests_its_binding[relative-root]", REAL]),
    ("M2", "scripts/cycle_verdict.py", COPY, None,
     [T + "test_write_verdict_ingests_the_verdict_then_its_copy[keep-verdicts]",
      T + "test_keep_copy_ingests_each_copy", REAL]),
    ("M3", "scripts/cycle_verdict.py", VERDICT, None,
     [T + "test_write_verdict_ingests_the_verdict_then_its_copy[no-copy]",
      T + "test_write_verdict_ingests_the_verdict_then_its_copy[keep-verdicts]", REAL]),
    ("M4", "scripts/wave_stage_record.py", RESULT, None,
     [T + "test_one_ingest_per_write_in_a_fake_wave", T + "test_fake_wave_bytes_equal_with_a_failing_store"]),
    ("M5", "scripts/cycle_resume.py", BINDING, WIP_BINDING,
     [T + "test_write_binding_ingests_its_binding[relative-root]"]),
]
MUST_STAY_GREEN = {"M5": [T + "test_write_binding_ingests_its_binding[absolute-root]", REAL]}


def git(repo: Path, *args: str) -> bytes:
    return subprocess.run(["git", "-C", str(repo), *args], capture_output=True, check=True).stdout


def head_ok(repo: Path, rel: str) -> tuple[bool, str]:
    head = hashlib.sha256(git(repo, "cat-file", "blob", f"HEAD:{rel}")).hexdigest()
    now = hashlib.sha256((repo / rel).read_bytes()).hexdigest()
    porcelain = git(repo, "status", "--porcelain").decode("utf-8", "replace")
    return now == head and porcelain == "", f"{rel} sha256 {now} HEAD blob {head}; porcelain {porcelain!r}"


def mutate(data: bytes, line: str, new: str | None) -> bytes:
    for eol in (b"\r\n", b"\n"):
        old = line.encode("utf-8") + eol
        if data.count(old) == 1:
            return data.replace(old, b"" if new is None else new.encode("utf-8") + eol)
    raise SystemExit(f"STOP: the line is not in the file exactly once: {line!r}")


def failed_cases(out: str) -> list[str]:
    return sorted(line.split()[1] for line in out.splitlines() if line.startswith("FAILED "))


def main(argv: list[str]) -> int:
    repo, base, log_path = Path(argv[0]).resolve(), Path(argv[1]), Path(argv[2])
    base.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, PYTHONDONTWRITEBYTECODE="1", PYTHONHASHSEED="0")
    lines, caught = [], 0
    for mid, rel, line, new, expect in MUTATIONS:
        ok, state = head_ok(repo, rel)
        if not ok:
            print(f"STOP before {mid}: {state}")
            return 2
        path = repo / rel
        kept = path.read_bytes()
        try:
            path.write_bytes(mutate(kept, line, new))
            done = subprocess.run([sys.executable, "-m", "pytest", "-p", "no:cacheprovider", "--basetemp",
                                   str(base / f"teeth-{mid}"), "-q", "-rf", "--tb=no", HOOK_TESTS], cwd=repo, env=env,
                                  capture_output=True, text=True, timeout=1200)
        finally:
            path.write_bytes(kept)
        ok, state = head_ok(repo, rel)
        if not ok:
            print(f"STOP after {mid}: restore check failed: {state}")
            return 2
        red = failed_cases(done.stdout)
        green_ok = not set(MUST_STAY_GREEN.get(mid, [])) & set(red)
        hit = done.returncode != 0 and set(expect) <= set(red) and green_ok
        caught += hit
        summary = (done.stdout.strip().splitlines() or ["(no output)"])[-1]
        lines.append(f"{mid} {rel} {'CAUGHT' if hit else 'NOT CAUGHT'} exit {done.returncode}: {summary}")
        lines += [f"   red {case}" for case in red]
        lines.append(f"   expected red {len(expect)} all in the red set: {set(expect) <= set(red)}; "
                     f"must stay green {len(MUST_STAY_GREEN.get(mid, []))} green: {green_ok}")
        lines.append(f"   restored: {state}")
    lines.append(f"caught {caught} / {len(MUTATIONS)}")
    text = "\n".join(lines) + "\n"
    log_path.write_text(text, encoding="utf-8")
    print(text, end="")
    return 0 if caught == len(MUTATIONS) else 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
