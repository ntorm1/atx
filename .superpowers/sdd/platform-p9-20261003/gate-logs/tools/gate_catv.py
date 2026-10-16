"""GATE agent 4, step 7 (G-P10 (a), (b); M2A-4): the read-only checks of the gate head's catalog A (made by
`gate_cat.py`, outside the root), run inside one bounded run: `verify --catalog A --pins` (counts by state and kind),
the render-identity checker `atx-engine/tools/research_store_identity.py --catalog A --root ROOT` (its summary line and
any mismatch line: path, class, digest / offset reasons, never a value), and `verify --catalog A --ledger
build-equity/trials.jsonl` (the ledger is only read). Outputs go to OUT_DIR (outside the root); this prints exit codes,
the verify --pins output and the identity tool's last 40 lines.

usage: python gate_catv.py STORE_EXE ROOT DB OUT_DIR   (cwd = the pool root)
"""
from __future__ import annotations

import subprocess
import sys
from pathlib import Path


def run(argv: list[str], out: Path) -> int:
    r = subprocess.run(argv, capture_output=True, text=True)
    out.write_text(r.stdout + ("\n--- stderr\n" + r.stderr if r.stderr else ""), encoding="utf-8")
    return r.returncode


def main(argv: list[str]) -> int:
    exe, root, db, od = str(Path(argv[0]).resolve()), argv[1], argv[2], Path(argv[3])
    od.mkdir(parents=True, exist_ok=True)
    rc1 = run([exe, "verify", "--catalog", db, "--pins"], od / "verify-pins.txt")
    print(f"verify --pins exit {rc1}")
    print((od / "verify-pins.txt").read_text(encoding="utf-8"))
    rc2 = run([sys.executable, "atx-engine/tools/research_store_identity.py", "--catalog", db, "--root", root],
              od / "identity.txt")
    lines = (od / "identity.txt").read_text(encoding="utf-8").splitlines()
    print(f"render identity exit {rc2}; {len(lines)} output lines; last 40:")
    print("\n".join(lines[-40:]))
    rc3 = run([exe, "verify", "--catalog", db, "--ledger", "build-equity/trials.jsonl"], od / "verify-ledger.txt")
    vl = (od / "verify-ledger.txt").read_text(encoding="utf-8").splitlines()
    print(f"verify --ledger exit {rc3}; first line: {vl[0] if vl else ''}")
    return max(rc1, rc2, rc3)


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
