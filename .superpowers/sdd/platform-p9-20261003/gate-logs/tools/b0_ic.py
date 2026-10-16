"""GATE agent 3 (prepared for the run after GATE-IDX; rows 4-5 of the substitution list, `root-gate-report.md`
"Step 4, agent 3"): the Y-F0 u / w P9-B0 re-bases on the Release IC exe p9-3f, each the receipt of record's argv with
only token 0 (the exe) and `--output` changed, through the bounded runner at the registered phase caps (prereg C.14:
u 2,560, w 3,072 MiB), with GATE-IDX's and GATE-RELCACHE's checks built in.

usage (cwd = the pool root):
  python b0_ic.py CELL --dry      the checks and the argv (token diff vs the receipt of record); runs nothing
  python b0_ic.py CELL            checks, the bounded run, the post-checks and the record
CELL: yf0u (`p9-b0-yf0-u-run` -> build-equity/p9-b0g-yf0-u) | yf0w (`p9-b0-yf0-w-run` -> build-equity/p9-b0g-yf0-w)

Checks before the run (any failure = STOP, nothing started):
  GATE-IDX      `build-equity/mega-candidate-cache-v8-lo3/index.sqlite` absent; the receipt's argv names exactly that
                cache once and no cache-index flag (SQL-11)
  GATE-RELCACHE the cache dir's listing (relative path, size, mtime_ns; `dir_listing.py`'s form) restricted to the 351
                original files (`gate-logs/cache-lo3-listing-agent2-start.txt` without its index.sqlite row) has digest
                155872de50833fca; before the first Release pass the whole listing is exactly that set; C: >= 15 GB free
  EXE-CUR       the Release IC exe = its p9-3f receipt (SPDLOG-RELINK)
After: the exe re-hashed; the receipt summarised (store block: SQL3-2 wants none); the restricted listing again (must
still be 155872de50833fca: the original files untouched) and the whole-dir listing written to
`gate-logs/cache-lo3-listing-after-<out>.txt` with its cause ("Release identity subtree added at step 4 (<out>)" when it
grew); C: free again (>= 15 GB, GATE-RELCACHE); every output file's SHA-256 in `gate-logs/b0-files-<out>.txt`; one line
in `gate-logs/b0-runs.txt`. No output file is opened beyond hashing.
"""
from __future__ import annotations

import datetime as dt
import hashlib
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[5]
LOGS = REPO / ".superpowers" / "sdd" / "platform-p9-20261003" / "gate-logs"
PY = sys.executable
EXE = Path("build-equity-rel/bin/atx-equity-strategy-ic.exe")
EXE_RECEIPT = Path("build-equity/mega-p9-3f-receipt.json")
CACHE = "build-equity/mega-candidate-cache-v8-lo3"
ORIG = LOGS / "cache-lo3-listing-agent2-start.txt"
ORIG_DIGEST = "155872de50833fca"
CELLS = {"yf0u": ("build-equity/p9-b0-yf0-u-run/receipt.json", "p9-b0g-yf0-u", 2560),
         "yf0w": ("build-equity/p9-b0-yf0-w-run/receipt.json", "p9-b0g-yf0-w", 3072)}
MIN_FREE_GB = 15.0


def sha_file(p: Path) -> str:
    h = hashlib.sha256()
    with p.open("rb") as f:
        for c in iter(lambda: f.read(1 << 22), b""):
            h.update(c)
    return h.hexdigest()


def listing(root: Path) -> list[str]:
    rows = []
    for dp, dn, fn in os.walk(root):
        dn.sort()
        for f in sorted(fn):
            p = os.path.join(dp, f)
            st = os.stat(p)
            rows.append(f"{os.path.relpath(p, root).replace(os.sep, '/')}\t{st.st_size}\t{st.st_mtime_ns}")
    return sorted(rows)


def digest(rows: list[str]) -> str:
    return hashlib.sha256(("\n".join(rows) + "\n").encode()).hexdigest()[:16]


def original_paths() -> set[str]:
    rows = [r for r in ORIG.read_text(encoding="utf-8").splitlines() if r]
    keep = [r for r in rows if r.split("\t")[0] != "index.sqlite"]
    if digest(keep) != ORIG_DIGEST or len(keep) != 351:
        raise SystemExit(f"STOP: {ORIG.name} minus index.sqlite is not the 351-file set {ORIG_DIGEST}")
    return {r.split("\t")[0] for r in keep}


def free_gb() -> float:
    return shutil.disk_usage("C:/").free / 2**30


def checks(cell: str):
    rec, out, cap = CELLS[cell]
    ref = json.loads((REPO / rec).read_text(encoding="utf-8-sig"))["command"]
    if (REPO / CACHE / "index.sqlite").exists():
        raise SystemExit(f"STOP (GATE-IDX): {CACHE}/index.sqlite is present: BLOCKED-OWNER, nothing started")
    idx = [i for i, t in enumerate(ref) if t == "--candidate-cache"]
    if len(idx) != 1 or ref[idx[0] + 1] != CACHE or any(t.endswith("-cache-index") for t in ref):
        raise SystemExit("STOP: the receipt of record does not name exactly the real cache once (or names an index)")
    orig = original_paths()
    now = listing(REPO / CACHE)
    restricted = [r for r in now if r.split("\t")[0] in orig]
    extra = [r for r in now if r.split("\t")[0] not in orig]
    if digest(restricted) != ORIG_DIGEST or len(restricted) != 351:
        raise SystemExit(f"STOP (GATE-RELCACHE): the 351 original files' listing is {digest(restricted)}, not "
                         f"{ORIG_DIGEST}")
    gb = free_gb()
    if gb < MIN_FREE_GB:
        raise SystemExit(f"STOP (GATE-RELCACHE): C: {gb:.1f} GB free < {MIN_FREE_GB}")
    args = list(ref[1:])
    k = [i for i, t in enumerate(args) if t == "--output"]
    if len(k) != 1:
        raise SystemExit("STOP: --output not exactly once")
    args[k[0] + 1] = f"build-equity/{out}"
    notes = [f"GATE-IDX: index.sqlite absent; argv names {CACHE} once, no index flag",
             f"GATE-RELCACHE: original 351 files listing {digest(restricted)}; files beyond them {len(extra)}; "
             f"whole listing {digest(now)} ({len(now)} files); C: {gb:.1f} GB free"]
    return ref, [str(REPO / EXE), *args], out, cap, orig, notes


def diff(ref: list[str], got: list[str]) -> str:
    d = [(i, a, b) for i, (a, b) in enumerate(zip(ref, got)) if a != b]
    return f"{len(ref)} / {len(got)} tokens, differing {d}" + (
        f", appended {got[len(ref):]}" if len(got) > len(ref) else "")


def main(argv: list[str]) -> int:
    cell = argv[0]
    ref, cmd, out, cap, orig, notes = checks(cell)
    want = json.loads((REPO / EXE_RECEIPT).read_text(encoding="utf-8-sig"))["Executables"]
    want = want["atx-equity-strategy-ic"]["Sha256"]
    before = sha_file(REPO / EXE)
    head = (f"{cell} -> build-equity/{out}: exe {EXE.as_posix()} sha256 {before} (p9-3f receipt "
            f"{'=' if before == want else '!='}); runner cap {cap} MiB")
    if "--dry" in argv:
        print(head)
        for n in notes:
            print("  " + n)
        print("  argv vs receipt of record: " + diff(ref, cmd))
        print("  argv: " + " ".join(cmd[1:]))
        return 0
    if before != want:
        raise SystemExit("STOP: the IC exe differs from its p9-3f receipt (SPDLOG-RELINK: re-receipt first)")
    for d in (REPO / "build-equity" / out, REPO / "build-equity" / f"{out}-run"):
        if d.exists():
            raise SystemExit(f"STOP: {d} exists (outputs must be new)")
    import psutil
    ram = psutil.virtual_memory().available // 2**20
    t0 = dt.datetime.now(dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    run_dir = f"build-equity/{out}-run"
    runner = [PY, "scripts/run_bounded_research.py", "--output", run_dir, "--seconds", "600", "--max-rss-mib", str(cap),
              "--build-type", "Release", "--admission-wait-seconds", "1800", "--", *cmd]
    with (LOGS / f"runner-{out}.log").open("wb") as fh:
        rc = subprocess.run(runner, cwd=REPO, stdout=fh, stderr=subprocess.STDOUT,
                            env={**os.environ, "PYTHONDONTWRITEBYTECODE": "1"}).returncode
    after = sha_file(REPO / EXE)
    now = listing(REPO / CACHE)
    restricted = [r for r in now if r.split("\t")[0] in orig]
    added = [r for r in now if r.split("\t")[0] not in orig]
    lst = LOGS / f"cache-lo3-listing-after-{out}.txt"
    cause = f"Release identity subtree added at step 4 ({out})" if added else "no file added"
    lst.write_text(f"# cause: {cause}; {len(now)} files, listing {digest(now)}; original 351 restricted "
                   f"{digest(restricted)}\n" + "\n".join(now) + "\n", encoding="utf-8", newline="\n")
    line = (f"{t0} {'; '.join([head] + notes)}; free RAM before {ram} MiB; runner-exit {rc}; exe after "
            f"{'=' if after == before else '!='}; cache after: original 351 {digest(restricted)} "
            f"({'unchanged' if digest(restricted) == ORIG_DIGEST and len(restricted) == 351 else 'CHANGED'}), "
            f"added {len(added)} files / {sum(int(r.split(chr(9))[1]) for r in added)} bytes, whole {digest(now)} "
            f"({cause}); C: {free_gb():.1f} GB free after")
    rec = REPO / run_dir / "receipt.json"
    if rec.is_file():
        shutil.copyfile(rec, LOGS / f"receipt-{out}.json")
        r = json.loads(rec.read_text(encoding="utf-8"))
        s_path = REPO / run_dir / "start.json"
        s = json.loads(s_path.read_text(encoding="utf-8")) if s_path.is_file() else {}
        line += (f" :: outcome {r.get('outcome')} exit {r.get('exit_code')} wall_s {r.get('wall_seconds')} peak_MiB "
                 f"{round((r.get('sampled_peak_tree_rss_bytes') or 0) / 2**20)} receipt-exe "
                 f"{(r.get('executable_sha256') or '')[:16]} source {(r.get('source_sha') or '')[:8]} build_type "
                 f"{r.get('build_type')} git {r.get('git')!r} store-block receipt {'store' in r} start {'store' in s}"
                 f" | argv vs receipt of record: {diff(ref, r['command'])}")
    od = REPO / "build-equity" / out
    if od.is_dir():
        rows = [f"{p.relative_to(od).as_posix()}\t{p.stat().st_size}\t{sha_file(p)}"
                for p in sorted(q for q in od.rglob("*") if q.is_file())]
        text = "\n".join(rows) + "\n"
        (LOGS / f"b0-files-{out}.txt").write_text(text, encoding="utf-8", newline="\n")
        line += f" | outputs {len(rows)} files, listing sha256 {hashlib.sha256(text.encode()).hexdigest()[:16]}"
    print(line)
    with (LOGS / "b0-runs.txt").open("a", encoding="utf-8") as fh:
        fh.write(line + "\n")
    return rc


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
