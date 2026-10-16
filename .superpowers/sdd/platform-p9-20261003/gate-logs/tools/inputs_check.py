"""GATE (agent 2): do the inputs a gate run would re-run still exist? For each named receipt, every argv token that
names a path under build-equity/ (the argv of record) is checked for existence on disk (names only; nothing opened).

usage: python inputs_check.py OUT RECEIPT [RECEIPT ...]    (paths relative to C:/atx-wt/pool-2)
Writes OUT (one line per receipt: label, missing count, the missing paths; then each existing path) and prints it.
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[5]


def main(argv: list[str]) -> int:
    out, receipts = Path(argv[0]), argv[1:]
    lines = []
    total_missing = 0
    for rec in receipts:
        p = REPO / rec
        if not p.is_file():
            lines.append(f"{rec}: RECEIPT MISSING")
            total_missing += 1
            continue
        cmd = json.loads(p.read_text(encoding="utf-8"))["command"]
        out_dir = None
        if "--output" in cmd:
            out_dir = cmd[cmd.index("--output") + 1]
        paths = []
        for t in cmd[1:]:
            t2 = t.replace("\\", "/")
            if t2.startswith("build-equity/") or t2.startswith("build-equity-rel/"):
                if t2 != (out_dir or "").replace("\\", "/"):
                    paths.append(t2)
        missing = [x for x in paths if not (REPO / x).exists()]
        total_missing += len(missing)
        lines.append(f"{rec}: inputs {len(paths)}, missing {len(missing)}: {missing}")
    text = "\n".join(lines) + f"\nTOTAL missing {total_missing}\n"
    out.write_text(text, encoding="utf-8")
    print(text, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
