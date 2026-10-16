"""Cells root: the read filter of a research_cycle.py console (blind rule; B.0 void (3)). Prints ONLY the step states,
the argvs, the receipts' outcome lines, the bindings, the ledger lines, the gate's final PASS / FAIL line, the fields
check and every stop / error line. The gate's per-candidate lines (status, HAC t, tau, max |rho|), its count line and
its reference-members line are NOT printed (admission figures are printed-only, read after the mechanics verdict).
usage: cycle_filter.py LOG
"""
from __future__ import annotations

import re
import sys

KEEP = re.compile(r"^(== |# |   receipt[: ]|   binding: |   ledger |   fit incomplete|fields (check|reuse)|"
                  r"\"?C:/Program Files/Python312/python\.exe\"? |cycle exit )"
                  r"|^gate \S+ (PASS|FAIL)"
                  r"|HARD-STOP|Traceback|Error|error:|refus|REFUS|STOP|stop:")
DROP = re.compile(r"^gate \S+ \S+: status |^gate \S+: \d+ of \d+ listed|^  \S+: status |^  reference members vs ")


def main(path: str) -> int:
    n = shown = 0
    for line in open(path, encoding="utf-8", errors="replace"):
        n += 1
        line = line.rstrip("\n")
        if DROP.search(line) or not KEEP.search(line):
            continue
        shown += 1
        print(line)
    print(f"[cycle_filter: {shown} of {n} lines shown]")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1]))
