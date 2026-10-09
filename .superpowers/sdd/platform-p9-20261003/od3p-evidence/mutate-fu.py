"""OD3P teeth, follow-up round (item 6 = T2, Ruling OD3P-S: S-a, S-d). Ruling TEETH-RESTORE: the tool keeps the
pre-mutation bytes itself, mutates one site, runs the named tests, writes the bytes back in a ``finally``, then asserts
the raw SHA-256 equals the pre-mutation one, ``git hash-object <file>`` equals the ``HEAD:<file>`` blob (nav_summ.py is
CRLF in the working tree, LF in the blob) and ``git status --porcelain`` is empty, before the first mutation and after
every one. A failed restore stops the tool at once (exit 2) and prints the state; nothing is committed by this tool.
Never git checkout / restore / stash.

Usage: "C:/Program Files/Python312/python.exe" mutate-fu.py <own basetemp dir>
"""
import hashlib
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path("C:/atx-wt/pool-21")
PY = "C:/Program Files/Python312/python.exe"
BT = Path(sys.argv[1])
CYCLE_T = "scripts/tests/test_research_cycle_od3.py"
TOOL_T = "atx-impl/tools/test_od3_tooling.py"
RR = "scripts/research_roles.py"
NS = "atx-impl/tools/nav_summ.py"

MUTATIONS = [
    ("A1 S-a: the R-1 guard ignores the eras", RR,
     '"eras": [[e.get("id"), e.get("role_sha256")] if isinstance(e, dict) else e for e in rec.get("eras") or []],',
     '"eras": want["eras"],', CYCLE_T, "other_eras"),
    ("A1b S-a: re-review 1 probe X1 (the guard ignores the eras) on its own -k (review: NOT CAUGHT, 32 passed)", RR,
     '"eras": [[e.get("id"), e.get("role_sha256")] if isinstance(e, dict) else e for e in rec.get("eras") or []],',
     '"eras": want["eras"],', CYCLE_T,
     "(drop or absent or fit_output or carry) and not carry_replaces_the_pooled"),   # that one needs the verb
    ("A2 S-a: the R-1 guard ignores the admission", RR,
     '"admission": rec.get("admission_sha256"),', '"admission": want["admission"],', CYCLE_T, "other_admission"),
    ("A3 S-a: the R-1 guard ignores the anchor", RR,
     '"anchor": rec.get("anchor_role_sha256"),', '"anchor": want["anchor"],', CYCLE_T, "other_anchor"),
    ("D1 S-d: the stale .pending check removed", RR,
     "        if res.exists_dir(pending) or res.path(pending).exists():\n", "        if False:\n", CYCLE_T,
     "stale_pending"),
    ("T1 T2: the --pool-one-sided refusal removed", NS,
     "    if args.pool_one_sided and not (args.pool and (args.pool_reference or args.reference)):\n",
     "    if False:\n", TOOL_T, "refuses_without_its_inputs"),
    ("T2 T2: one_sided=True dropped on the pooled pair", NS,
     "    p = engine_of(args).paired_of(nets, ref_nets, args, one_sided=True)\n",
     "    p = engine_of(args).paired_of(nets, ref_nets, args)\n", TOOL_T, "t2_pool_one_sided_adds"),
    ("T3 T2: the verdict comparison inverted (p >= alpha)", NS,
     '"pass": bool(positive and p1 is not None and p1 < alpha), "rule": rule}',
     '"pass": bool(positive and p1 is not None and p1 >= alpha), "rule": rule}', TOOL_T, "t2_verdict"),
    ("I1 identity: the flag-absent pooled pair made one-sided (with the verdict)", NS,
     '    if not getattr(args, "pool_one_sided", False):\n', "    if False:\n", TOOL_T,
     "flag_absent_nav_summ_equals_roots_blob"),
]


def git(*args: str) -> str:
    return subprocess.run(["git", *args], cwd=ROOT, capture_output=True, text=True, check=True).stdout.strip()


def porcelain() -> str:
    return git("status", "--porcelain")


def blob_ok(rel: str) -> bool:
    return git("hash-object", rel) == git("rev-parse", f"HEAD:{rel}")


def run(test: str, expr: str) -> subprocess.CompletedProcess:
    env = dict(os.environ, PYTHONDONTWRITEBYTECODE="1", PYTHONHASHSEED="0")
    return subprocess.run([PY, "-m", "pytest", "-q", "-p", "no:cacheprovider", "--basetemp", str(BT), test, "-k", expr],
                          cwd=ROOT, capture_output=True, text=True, env=env)


def main() -> int:
    head = git("rev-parse", "--short=8", "HEAD")
    state = porcelain()
    if state:
        print(f"tree not clean before the first mutation (HEAD {head}):\n{state}")
        return 2
    print(f"HEAD {head}; porcelain empty before the first mutation", flush=True)
    bad = 0
    for test, expr in dict.fromkeys((m[4], m[5]) for m in MUTATIONS):     # B0: every selection passes unmutated
        done = run(test, expr)
        summary = (done.stdout.strip().splitlines() or [""])[-1]
        print(f"B0 baseline [{test} -k \"{expr}\"]: exit {done.returncode}; {summary}", flush=True)
        bad += done.returncode != 0
    if bad:
        print("a baseline selection fails unmutated: no mutation run")
        return 1
    for name, rel, old, new, test, expr in MUTATIONS:
        path = ROOT / rel
        original = path.read_bytes()
        before = hashlib.sha256(original).hexdigest()
        if not blob_ok(rel):
            print(f"{name}: {rel} differs from HEAD's blob before the mutation: STOP")
            return 2
        text = original.decode("utf-8")
        nl = "\r\n" if "\r\n" in text else "\n"
        old_n, new_n = old.replace("\n", nl), new.replace("\n", nl)
        if text.count(old_n) != 1:
            print(f"{name}: SITE NOT UNIQUE ({text.count(old_n)})", flush=True)
            bad += 1
            continue
        try:
            path.write_bytes(text.replace(old_n, new_n).encode("utf-8"))
            done = run(test, expr)
        finally:
            path.write_bytes(original)
        after = hashlib.sha256(path.read_bytes()).hexdigest()
        restored = after == before and blob_ok(rel) and porcelain() == ""
        if not restored:
            print(f"{name}: RESTORE FAILED (sha256 {after[:12]} vs {before[:12]}; porcelain:\n{porcelain()}): STOP")
            return 2
        summary = (done.stdout.strip().splitlines() or [""])[-1]
        caught = done.returncode != 0
        print(f"{name} [{test} -k \"{expr}\"]: {'CAUGHT' if caught else 'NOT CAUGHT'} (exit {done.returncode}; "
              f"{summary}); restored: sha256 {before[:12]} == pre-mutation, hash-object == HEAD:{rel}, porcelain "
              "empty", flush=True)
        bad += not caught
    print(f"porcelain after the last restore: {'empty' if porcelain() == '' else porcelain()}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
