"""Lane COV-MV (R10) gtest runner with the exe check beside each run (Ruling COVMV-S S-7, EXE-CUR,
SPDLOG-RELINK).

Usage: run-gtests.py <tag> <log-prefix> [run ...]   runs: mvtarget, mvaimrule, named, s2

Before and after each run it hashes the test exe (SHA-256) and compares it with the exe's SHA in
build-equity/mega-<tag>-receipt.json; both lines head the run's log. A mismatch before the run
skips the run (the exe is not the receipt's: not evidence). Exit 0 only if every run exited 0 with
an equal hash before and after.
"""

import hashlib
import json
import pathlib
import re
import subprocess
import sys
import datetime

POOL = pathlib.Path("C:/atx-wt/pool-31")
BIN = POOL / "build-equity" / "bin"
LOGS = POOL / ".superpowers" / "sdd" / "platform-p9-20261003" / "covmv-logs"
NAMED = (
    "Cov*:NavBookRule.*:VolTarget.*:Spo*:NavSpec.*:NavRules.*:CostLawRegistry.*:"
    "TradeRateRegistry.*:TargetRegistry.*:NavAnnMeanGross.*"
)
RUNS = {
    "mvtarget": ("mvtarget-debug", "atx-engine-book-tests", "MvTarget.*"),
    "mvaimrule": ("mvaimrule-debug", "atx-impl-strategy-target-tests", "MvAimRule.*"),
    "named": ("named-debug", "atx-impl-strategy-target-tests", NAMED),
    "s2": (
        "s2-debug",
        "atx-impl-strategy-target-tests",
        "MvAimRule.PrintFailureLeavesTheBookAndWritesNull",
    ),
}


def sha256(path: pathlib.Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def main() -> int:
    tag, prefix = sys.argv[1], sys.argv[2]
    names = sys.argv[3:] or ["mvtarget", "mvaimrule", "named"]
    receipt_path = POOL / "build-equity" / f"mega-{tag}-receipt.json"
    receipt = json.loads(receipt_path.read_text(encoding="utf-8-sig"))
    ok = True
    for name in names:
        stem, exe, filt = RUNS[name]
        log = LOGS / f"{prefix}-{stem}.log"
        path = BIN / f"{exe}.exe"
        want = receipt["Executables"][exe]["Sha256"]
        before = sha256(path)
        lines = [
            f"{datetime.datetime.now().isoformat(timespec='seconds')} run {name}",
            f"exe {path}",
            f"receipt {receipt_path.name} Source {receipt['Source']} DirtyEntries "
            f"{receipt['DirtyEntries']} ExitCode {receipt['ExitCode']}",
            f"sha256 before {before} receipt {want} equal={before == want}",
            f"filter {filt}",
        ]
        if before != want:
            lines.append("NOT RUN: the exe is not the receipt's")
            log.write_text("\n".join(lines) + "\n", encoding="utf-8")
            print(f"{name}: NOT RUN (sha mismatch)")
            ok = False
            continue
        proc = subprocess.run(
            [str(path), f"--gtest_filter={filt}"],
            capture_output=True,
            text=True,
            cwd=str(BIN),
            check=False,
        )
        after = sha256(path)
        lines.append(f"sha256 after {after} equal={after == want}")
        body = proc.stdout + ("\n[stderr]\n" + proc.stderr if proc.stderr else "")
        log.write_text("\n".join(lines) + "\n\n" + body + f"\nexit={proc.returncode}\n",
                       encoding="utf-8")
        passed = re.search(r"\[  PASSED  \] (\d+) test", proc.stdout)
        failed = re.search(r"\[  FAILED  \] (\d+) test", proc.stdout)
        ran = re.search(r"\[==========\] (\d+) tests? from .* ran", proc.stdout)
        print(f"{name}: exit={proc.returncode} ran={ran.group(1) if ran else '?'} "
              f"passed={passed.group(1) if passed else 0} "
              f"failed={failed.group(1) if failed else 0} sha_equal={after == want}")
        ok = ok and proc.returncode == 0 and after == want
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
