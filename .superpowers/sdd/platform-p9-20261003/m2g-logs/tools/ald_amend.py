"""M2g item 1 (ALD-12 (c)): check the AL-DATA report section 5.1 / 5.2 "before" lines byte for byte against the
committed files (git show HEAD:), then (with --apply) replace them by the "after" lines and check the result parses.

The before / after blocks are read from task-AL-DATA-report.md section 5 (the ```json fences in order: 5.1 before,
5.1 after, 5.2 before, 5.2 after); line numbers are the report's. Nothing else is changed.
usage: python ald_amend.py [--apply]   (cwd = pool-2 root)
"""
from __future__ import annotations

import json
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path("C:/atx-wt/pool-2")
REPORT = ROOT / ".superpowers/sdd/platform-p9-20261003/al-data/task-AL-DATA-report.md"
TARGETS = [("scripts/specs/p9/blocked/lazy_prices.json", [17, 18, 19, 20, 28]),
           ("scripts/specs/p9/blocked/hf_crowd.json", [20])]


def blocks() -> list[list[str]]:
    text = REPORT.read_text(encoding="utf-8")
    sec = text[text.index("### 5. The exact spec amendments"):text.index("**5.3 `russell_recon`")]
    return [b.split("\n")[:-1] for b in re.findall(r"```json\n(.*?)```", sec, flags=re.S)]


def main() -> int:
    apply = "--apply" in sys.argv[1:]
    fences = blocks()
    assert len(fences) == 4, len(fences)
    ok = True
    for k, (rel, lines) in enumerate(TARGETS):
        before, after = fences[2 * k], fences[2 * k + 1]
        assert len(before) == len(lines) == len(after), (rel, len(before), len(after))
        head = subprocess.run(["git", "-C", str(ROOT), "show", f"HEAD:{rel}"], capture_output=True, check=True).stdout
        work = (ROOT / rel).read_bytes()
        print(f"{rel}: HEAD blob == working tree bytes: {head == work}; CRLF in HEAD: {b'\r' in head}; "
              f"BOM: {head.startswith(b'\xef\xbb\xbf')}")
        file_lines = head.decode("utf-8").split("\n")
        for n, b in zip(lines, before):
            same = file_lines[n - 1] == b
            ok &= same
            print(f"  line {n}: before byte-identical: {same}")
        if apply and ok:
            new = list(file_lines)
            for n, b, a in zip(lines, before, after):
                assert new[n - 1] == b
                new[n - 1] = a
            out = "\n".join(new).encode("utf-8")
            json.loads(out)       # parses
            old_doc, new_doc = json.loads(head), json.loads(out)
            changed = sorted(k2 for k2 in old_doc if old_doc[k2] != new_doc[k2])
            print(f"  applied; parses; keys same and in order: {list(old_doc) == list(new_doc)}; changed keys: {changed}")
            (ROOT / rel).write_bytes(out)
    print("ALL BEFORE LINES BYTE-IDENTICAL" if ok else "MISMATCH: STOP")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
