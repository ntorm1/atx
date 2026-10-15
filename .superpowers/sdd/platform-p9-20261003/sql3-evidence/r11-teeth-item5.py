"""SQL3 item 5 + SQL3-WQ teeth (P9 RESUME 11; rulings TEETH-RESTORE, TEETH-CRLF). The R10 tool
(r10-teeth-item5.py) with its precondition in the TEETH-CRLF form and two SQL3-WQ mutations added.

Mutations: M1-M4 one item-5 hook call line removed each (cycle_resume.write_binding, cycle_verdict.keep_copy,
cycle_verdict.write_verdict, wave_stage_record.record); M5 the WIP's cwd-relative path form put back in write_binding;
M6 / M7 the SQL3-WQ hunks put back to their old form (wave_queue.write's `p`, emit's `out`).

For each mutation the tool: checks the file is HEAD's (`git hash-object <rel>` = `git rev-parse HEAD:<rel>`, which
applies the clean filter: core.autocrlf=true checks out CRLF over LF blobs) and `git status --porcelain` is empty;
keeps the file's bytes itself (in memory and as a copy under BASETEMP_PARENT) and their raw SHA-256; writes the
mutated bytes; runs the mutation's test file on the mutated tree; restores the kept bytes in a finally block; then
asserts the raw SHA-256 of the working bytes = the kept copy's, `git hash-object` = HEAD's blob id and porcelain
empty. Never git checkout / restore / stash; never the raw bytes against `git cat-file blob` (LF).

Usage: python r11-teeth-item5.py REPO BASETEMP_PARENT LOG
       (env ATX_RESEARCH_STORE_EXE: the store exe the real-exe case copies into its temp root; read-only)
Exit 0: every mutation red with its expected cases and its must-stay-green cases green; 1: a mutation not caught;
2: STOP (a precondition or a restore check failed; the state is printed; nothing may be committed or built on this
tree; the kept copy is under BASETEMP_PARENT).
"""
from __future__ import annotations

import hashlib
import os
from pathlib import Path
import subprocess
import sys

HOOK_TESTS = "scripts/tests/test_research_store_hook.py"
WQ_TESTS = "scripts/tests/test_wave_queue.py"
T = HOOK_TESTS + "::"
BINDING = '    research_store_hook.record(cycle.res.root, f"{st.run_dir}/{BINDING}", "cycle-binding")   # P9 SQL3 item 5'
COPY = '            research_store_hook.record(cycle.res.root, rel, "cycle-verdict")       # P9 SQL3 item 5'
VERDICT = ('    research_store_hook.record(cycle.res.root, f"{cycle.cycle_dir()}/{VERDICT}", "cycle-verdict")   '
           '# P9 SQL3 item 5')
RESULT = '    research_store_hook.record(w.root, w.path(path), "wave-result")     # P9 SQL3 item 5'
WIP_BINDING = '    research_store_hook.record(cycle.res.root, cycle.res.path(f"{st.run_dir}/{BINDING}"), "cycle-binding")'
WQ_WRITE = ('    research_store_hook.record(root, Path(d or QUEUE_DIR) / p.name, "wave-candidate")   '
            '# root-relative (SQL3-WQ)')
WQ_WRITE_OLD = '    research_store_hook.record(root, p, "wave-candidate")'
WQ_EMIT = ('        research_store_hook.record(root, a.output, "wave-manifest")     '
           '# root-relative unless absolute (SQL3-WQ)')
WQ_EMIT_OLD = '        research_store_hook.record(root, out, "wave-manifest")'
REAL = T + "test_real_store_exe_ingests_every_class"
WQ_CASE = WQ_TESTS + "::test_a_relative_root_hands_the_store_hook_root_relative_names"
# (id, file, the exact line, its replacement (None: the line removed), the test file, the cases that must go red)
MUTATIONS = [
    ("M1", "scripts/cycle_resume.py", BINDING, None, HOOK_TESTS,
     [T + "test_write_binding_ingests_its_binding[absolute-root]",
      T + "test_write_binding_ingests_its_binding[relative-root]", REAL]),
    ("M2", "scripts/cycle_verdict.py", COPY, None, HOOK_TESTS,
     [T + "test_write_verdict_ingests_the_verdict_then_its_copy[keep-verdicts]",
      T + "test_keep_copy_ingests_each_copy", REAL]),
    ("M3", "scripts/cycle_verdict.py", VERDICT, None, HOOK_TESTS,
     [T + "test_write_verdict_ingests_the_verdict_then_its_copy[no-copy]",
      T + "test_write_verdict_ingests_the_verdict_then_its_copy[keep-verdicts]", REAL]),
    ("M4", "scripts/wave_stage_record.py", RESULT, None, HOOK_TESTS,
     [T + "test_one_ingest_per_write_in_a_fake_wave", T + "test_fake_wave_bytes_equal_with_a_failing_store"]),
    ("M5", "scripts/cycle_resume.py", BINDING, WIP_BINDING, HOOK_TESTS,
     [T + "test_write_binding_ingests_its_binding[relative-root]"]),
    ("M6", "scripts/wave_queue.py", WQ_WRITE, WQ_WRITE_OLD, WQ_TESTS, [WQ_CASE]),
    ("M7", "scripts/wave_queue.py", WQ_EMIT, WQ_EMIT_OLD, WQ_TESTS, [WQ_CASE]),
]
MUST_STAY_GREEN = {"M5": [T + "test_write_binding_ingests_its_binding[absolute-root]", REAL]}


def git(repo: Path, *args: str) -> bytes:
    return subprocess.run(["git", "-C", str(repo), *args], capture_output=True, check=True).stdout


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def head_ok(repo: Path, rel: str, kept_sha: str | None = None) -> tuple[bool, str]:
    """TEETH-CRLF: the working file's blob id (clean filter applied) = HEAD's, porcelain empty, and (after a
    restore) the raw working bytes = the tool's own kept copy."""
    work = git(repo, "hash-object", "--", rel).decode().strip()
    head = git(repo, "rev-parse", f"HEAD:{rel}").decode().strip()
    raw = sha256((repo / rel).read_bytes())
    porcelain = git(repo, "status", "--porcelain").decode("utf-8", "replace")
    ok = work == head and porcelain == "" and (kept_sha is None or raw == kept_sha)
    kept = "" if kept_sha is None else f"; kept sha256 {kept_sha} {'=' if raw == kept_sha else '!='} working"
    return ok, f"{rel} hash-object {work} {'=' if work == head else '!='} HEAD {head}{kept}; porcelain {porcelain!r}"


def mutate(data: bytes, line: str, new: str | None) -> bytes:
    for eol in (b"\r\n", b"\n"):
        old = line.encode("utf-8") + eol
        if data.count(old) == 1:
            return data.replace(old, b"" if new is None else new.encode("utf-8") + eol)
    raise ValueError(f"the line is not in the file exactly once: {line!r}")


def failed_cases(out: str) -> list[str]:
    return sorted(line.split()[1] for line in out.splitlines() if line.startswith("FAILED "))


def main(argv: list[str]) -> int:
    repo, base, log_path = Path(argv[0]).resolve(), Path(argv[1]), Path(argv[2])
    base.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, PYTHONDONTWRITEBYTECODE="1", PYTHONHASHSEED="0")
    exe = env.get("ATX_RESEARCH_STORE_EXE")
    lines = [f"repo {repo} HEAD {git(repo, 'rev-parse', 'HEAD').decode().strip()}",
             f"ATX_RESEARCH_STORE_EXE {exe} sha256 "
             f"{sha256(Path(exe).read_bytes()) if exe and Path(exe).is_file() else '(absent: the real-exe case skips)'}"]
    caught = 0

    def stop(text: str) -> int:
        lines.append(text)
        log_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
        print("\n".join(lines))
        return 2

    for mid, rel, line, new, tests, expect in MUTATIONS:
        ok, state = head_ok(repo, rel)
        if not ok:
            return stop(f"STOP before {mid}: {state}")
        path = repo / rel
        kept = path.read_bytes()
        kept_sha = sha256(kept)
        try:
            mutated = mutate(kept, line, new)
        except ValueError as exc:
            return stop(f"STOP before {mid} (nothing written): {exc}")
        (base / f"kept-{mid}-{path.name}").write_bytes(kept)       # the tool's own copy, should the run die
        try:
            path.write_bytes(mutated)
            done = subprocess.run([sys.executable, "-m", "pytest", "-p", "no:cacheprovider", "--basetemp",
                                   str(base / f"teeth-{mid}"), "-q", "-rf", "--tb=no", tests], cwd=repo, env=env,
                                  capture_output=True, text=True, timeout=1200)
        finally:
            path.write_bytes(kept)
        ok, state = head_ok(repo, rel, kept_sha)
        if not ok:
            return stop(f"STOP after {mid}: restore check failed: {state}")
        red = failed_cases(done.stdout)
        green_ok = not set(MUST_STAY_GREEN.get(mid, [])) & set(red)
        hit = done.returncode != 0 and set(expect) <= set(red) and green_ok
        caught += hit
        summary = (done.stdout.strip().splitlines() or ["(no output)"])[-1]
        lines.append(f"{mid} {rel} ({tests}) {'CAUGHT' if hit else 'NOT CAUGHT'} exit {done.returncode}: {summary}")
        lines += [f"   red {case}" for case in red]
        lines.append(f"   expected red {len(expect)} all in the red set: {set(expect) <= set(red)}; "
                     f"must stay green {len(MUST_STAY_GREEN.get(mid, []))} green: {green_ok}")
        lines.append(f"   restored: {state}")
    lines.append(f"final porcelain {git(repo, 'status', '--porcelain').decode('utf-8', 'replace')!r}")
    lines.append(f"caught {caught} / {len(MUTATIONS)}")
    text = "\n".join(lines) + "\n"
    log_path.write_text(text, encoding="utf-8")
    print(text, end="")
    return 0 if caught == len(MUTATIONS) else 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
