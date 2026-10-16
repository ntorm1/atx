"""GATE agent 3: the gate's P9-B0 NAV runs (GATE-1, PRE-22, PRE-29, PRE-7a, ALCLOCK-1, FIELDS-SUB), each the receipt of
record's argv with ONLY the tokens the substitution list names changed, on the Release NAV exe p9-3f, through the
bounded runner (RUN-1 / RUN-2: --seconds 600 --max-rss-mib 2048; COVR-1 for the run that reads --risk-model).

usage (cwd = the pool root):
  python b0_nav.py CELL --dry      print the argv and its token diff against the receipt of record; runs nothing
  python b0_nav.py CELL            the bounded run, then the record
CELL (the substitution list, root-gate-report.md "Step 4, agent 3"):
  r2   R-2  `mega-nav-v8-r1-std-t.05-d.1-fixed-obdelta-x.05-loc-L1.1474-v80-run`: --fields / --fields-sha256 -> v15
       (FIELDS-SUB), --output build-equity/p9-b0-r2-nav, + --ann-mean-gross
  x10  X-10 `p9-b0-x10-nav-run`: --output build-equity/p9-b0g-x10-nav, + --ann-mean-gross
  y1   Y-1  `p9-b0-y1-nav-run`: --risk-model / --risk-model-sha256 -> `p9-m2i-dcov-v11` (PRE-29), --output
       build-equity/p9-b0g-y1-nav, + --ann-mean-gross
  yf0  Y-F0 `p9-b0-yf0-nav-run`: --output build-equity/p9-b0g-yf0-nav, + --ann-mean-gross --info-clock-print
Token 0 (the exe) is always the Release exe (M2c-RED-2 (e), OD5-2). Checks before the run: the receipt's argv names no
`--candidate-cache` and no cache-index flag (GATE-IDX, SQL-11), every substituted flag occurs exactly once, no added
flag is already present, every substituted pin re-hashed from its file, the exe = its p9-3f receipt, the output dirs
are new. After: the exe re-hashed, the receipt summarised (outcome, exit, wall, peak, store block: SQL3-2 wants none),
the SHA-256 of every output file written to gate-logs/b0-files-<out>.txt. One line per run in gate-logs/b0-runs.txt,
the receipt copied to gate-logs/receipt-<out>.json. No output file is opened beyond hashing.
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
EXE = Path("build-equity-rel/bin/atx-equity-strategy-targets.exe")
EXE_RECEIPT = Path("build-equity/mega-p9-3f-receipt.json")
V15 = "build-equity/train-2020-2023-lo3-fields-v15/manifest.json"
V11_STORE = "build-equity/p9-m2i-dcov-v11"
CELLS = {
    "r2": ("build-equity/mega-nav-v8-r1-std-t.05-d.1-fixed-obdelta-x.05-loc-L1.1474-v80-run/receipt.json",
           "p9-b0-r2-nav",
           {"--fields": V15, "--fields-sha256": "26fee5ce301b9b0bffa1d72b45e014d973d3d73a080dd59ea55cda976f133b09"},
           ["--ann-mean-gross"]),
    "x10": ("build-equity/p9-b0-x10-nav-run/receipt.json", "p9-b0g-x10-nav", {}, ["--ann-mean-gross"]),
    "y1": ("build-equity/p9-b0-y1-nav-run/receipt.json", "p9-b0g-y1-nav",
           {"--risk-model": V11_STORE,
            "--risk-model-sha256": "397366473b3a3b0985ed4fb78868f4d894e16ae6c6dec05afda8d0c5126823c7"},
           ["--ann-mean-gross"]),
    "yf0": ("build-equity/p9-b0-yf0-nav-run/receipt.json", "p9-b0g-yf0-nav", {},
            ["--ann-mean-gross", "--info-clock-print"]),
}
PIN_OF = {"--fields-sha256": "--fields", "--risk-model-sha256": "--risk-model"}


def sha_file(p: Path) -> str:
    h = hashlib.sha256()
    with p.open("rb") as f:
        for c in iter(lambda: f.read(1 << 22), b""):
            h.update(c)
    return h.hexdigest()


def pin_target(flag: str, value: str) -> Path:
    p = REPO / value
    return p / "manifest.json" if p.is_dir() else p


def build(cell: str) -> tuple[list[str], list[str], str, list[str]]:
    rec, out, subs, add = CELLS[cell]
    ref = json.loads((REPO / rec).read_text(encoding="utf-8-sig"))["command"]
    notes = []
    bad = [t for t in ref if t.startswith("--candidate-cache") or t.endswith("-cache-index")]
    if bad:
        raise SystemExit(f"STOP: the receipt of record names a cache flag {bad} (GATE-IDX / SQL-11)")
    notes.append("receipt of record names no --candidate-cache and no cache-index flag")
    args = list(ref[1:])
    for flag, new in {**subs, "--output": f"build-equity/{out}"}.items():
        idx = [i for i, t in enumerate(args) if t == flag]
        if len(idx) != 1:
            raise SystemExit(f"STOP: {flag} occurs {len(idx)} times in the receipt of record")
        args[idx[0] + 1] = new
    for t in add:
        if t in args:
            raise SystemExit(f"STOP: {t} already in the receipt of record")
    args += add
    for flag, of in PIN_OF.items():
        if flag in subs:
            tgt = pin_target(of, subs[of])
            got = sha_file(tgt)
            if got != subs[flag]:
                raise SystemExit(f"STOP: pin {flag} {subs[flag]} != sha256({tgt.relative_to(REPO).as_posix()}) {got}")
            notes.append(f"pin {flag} re-hashed from {tgt.relative_to(REPO).as_posix()}: = {got}")
    return ref, [str(REPO / EXE), *args], out, notes


def diff(ref: list[str], got: list[str]) -> str:
    d = [(i, a, b) for i, (a, b) in enumerate(zip(ref, got)) if a != b]
    s = f"{len(ref)} / {len(got)} tokens, differing {d}"
    return s + (f", appended {got[len(ref):]}" if len(got) > len(ref) else "")


def free_mib() -> int:
    import psutil  # the runner requires it too
    return psutil.virtual_memory().available // 2**20


def main(argv: list[str]) -> int:
    cell = argv[0]
    ref, cmd, out, notes = build(cell)
    want = json.loads((REPO / EXE_RECEIPT).read_text(encoding="utf-8-sig"))["Executables"]
    want = want["atx-equity-strategy-targets"]["Sha256"]
    before = sha_file(REPO / EXE)
    head = f"{cell} -> build-equity/{out}: exe {EXE.as_posix()} sha256 {before} (p9-3f receipt {'=' if before == want else '!='})"
    if "--dry" in argv:
        print(head)
        for n in notes:
            print("  " + n)
        print("  argv vs receipt of record: " + diff(ref, cmd))
        print("  argv: " + " ".join(cmd[1:]))
        return 0
    if before != want:
        raise SystemExit("STOP: the NAV exe differs from its p9-3f receipt (SPDLOG-RELINK: re-receipt first)")
    for d in (REPO / "build-equity" / out, REPO / "build-equity" / f"{out}-run"):
        if d.exists():
            raise SystemExit(f"STOP: {d} exists (outputs must be new)")
    run_dir = f"build-equity/{out}-run"
    t0 = dt.datetime.now(dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    ram = free_mib()
    runner = [PY, "scripts/run_bounded_research.py", "--output", run_dir, "--seconds", "600", "--max-rss-mib", "2048",
              "--build-type", "Release", "--admission-wait-seconds", "1800", "--", *cmd]
    with (LOGS / f"runner-{out}.log").open("wb") as fh:
        rc = subprocess.run(runner, cwd=REPO, stdout=fh, stderr=subprocess.STDOUT,
                            env={**os.environ, "PYTHONDONTWRITEBYTECODE": "1"}).returncode
    after = sha_file(REPO / EXE)
    line = f"{t0} {head}; free RAM before {ram} MiB; runner-exit {rc}; exe after {'=' if after == before else '!='}"
    rec = REPO / run_dir / "receipt.json"
    if rec.is_file():
        shutil.copyfile(rec, LOGS / f"receipt-{out}.json")
        r = json.loads(rec.read_text(encoding="utf-8"))
        s_path = REPO / run_dir / "start.json"
        s = json.loads(s_path.read_text(encoding="utf-8")) if s_path.is_file() else {}
        line += (f" :: outcome {r.get('outcome')} exit {r.get('exit_code')} wall_s {r.get('wall_seconds')} peak_MiB "
                 f"{round((r.get('sampled_peak_tree_rss_bytes') or 0) / 2**20)} receipt-exe "
                 f"{(r.get('executable_sha256') or '')[:16]} source {(r.get('source_sha') or '')[:8]} build_type "
                 f"{r.get('build_type')} argv_sha256 {(r.get('argv_sha256') or '')[:16]} git {r.get('git')!r} "
                 f"store-block receipt {'store' in r} start {'store' in s} | argv vs receipt of record: "
                 f"{diff(ref, r['command'])}")
    od = REPO / "build-equity" / out
    if od.is_dir():
        files = sorted(p for p in od.rglob("*") if p.is_file())
        rows = [f"{p.relative_to(od).as_posix()}\t{p.stat().st_size}\t{sha_file(p)}" for p in files]
        text = "\n".join(rows) + "\n"
        (LOGS / f"b0-files-{out}.txt").write_text(text, encoding="utf-8", newline="\n")
        line += f" | outputs {len(rows)} files, listing sha256 {hashlib.sha256(text.encode()).hexdigest()[:16]}"
    print(line)
    with (LOGS / "b0-runs.txt").open("a", encoding="utf-8") as fh:
        fh.write(line + "\n")
    return rc


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
